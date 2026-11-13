/* Horizon OS layer, native host: process start and boot.

   What rtld and nn::init::Start do on hardware (see ports/zelda-totk/PORT.md, "How the game starts"), done here:
   map the guest window, load the relocated image (nx_image.py already linked it), make this host thread the
   guest main thread, run every module's DT_INIT in rtld's order (the SDK crt's _init: TLS registration with musl,
   then the module's constructors), then nninitStartup and nnMain.

   Usage: <game>.exe [image.bin] [--romfs <dir>] [--user <dir>] [--trace] [--prepare-shaders]
   Environment: HOS_TRACE=1 (log HLE calls), HOS_MEMORY_MB (memory the game is told it has),
                HOS_NVN_CONTINUE=1 (go on past nvnBootstrapLoader). */
#include "hos.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif
#include "hos_paths.h"
#include "hos_wide_size.h"   /* (the widescreen picture's size, shared with the Insert menu) */
#include "../gpu/vk/vk_api.h"
#include "../gpu/vk/oot_cheat_items.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef HOOKSHOT_VERSION   /* (nx2/build.py gives it: the release and a fingerprint of the sources, nx2/version.py) */
#define HOOKSHOT_VERSION "unknown"
#endif
const char *hos_version(void) { return HOOKSHOT_VERSION; }   /* (discord_rpc.c's status) */
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#else
#include <windows.h>   /* (linux: host/posix's) */
#include <sys/mman.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif
/* (linux) the separator of the paths the executable's folder is joined with */
#ifdef _WIN32
#define SEP 92
#define SEP_S "\\"
#else
#define SEP '/'
#define SEP_S "/"
#endif

#ifdef A64_MEM_ZERO
/* web: guest address = wasm address; the guest window is wasm memory below the host's data (-sGLOBAL_BASE) */
extern char __global_base;
uint64_t hos_space_end;
#define GUEST_SPACE (hos_space_end)
#else
#define GUEST_SPACE HOS_GUEST_SPACE
#endif
#define IMAGE_BASE hos_image_base   /* (the lift's: Eden's layout) */

#ifdef _WIN32
static void no_power_throttling(void) {
    const char *e = getenv("HOS_POWER_THROTTLING");
    if (e && atoi(e)) return;
    typedef BOOL(WINAPI * SetInfo)(HANDLE, int, LPVOID, DWORD);
    SetInfo set = (SetInfo)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (!set) return;
    struct { ULONG version, control, state; } s = {1, 0x1 | 0x4, 0};
    if (!set(GetCurrentProcess(), 4, &s, sizeof s)) {
        s.control = 0x1;
        set(GetCurrentProcess(), 4, &s, sizeof s);
    }
}
#endif

int hos_game_is_mkw(void) { return !strcmp(hos_main_build_id, "d00e4f399e411d0613aa8d26b1c02fef"); }

/* gen/hos_image.c (build.py lift) */
extern const uint64_t hos_main_stack_size;
extern const int hos_main_priority, hos_main_core, hos_module_count;
extern const uint64_t hos_module_init[];
extern const char *const hos_module_name[];
extern const uint64_t hos_entry_nninitStartup, hos_entry_nnMain;
extern const uint64_t hos_entry_init_libc0, hos_entry_init_libc1, hos_entry_init_libc2, hos_entry_nnosInitialize,
    hos_entry_nninitInitializeSdkModule;
/* nn_fs.c */
extern const char *hos_romfs_dir, *hos_user_dir;

/* HOS_LARGE_PAGES=1 (experiment): the guest window in 2 MB pages, for fewer TLB misses (the game touches hundreds of MB
   of scattered heap; 4 KB pages cover only ~12 MB of it in the CPU's TLB). The whole window is then physical memory at
   once (4 GB, locked), and Windows needs the account to hold "Lock pages in memory" (SeLockMemoryPrivilege, Local
   Security Policy > User Rights Assignment; log off and on after): without it this logs why and uses normal pages. */
int hos_large_pages;   /* (hos_mem.c: freed pages are not discarded then) */
#if defined(_WIN32) && !defined(A64_MEM_ZERO)
#pragma comment(lib, "advapi32.lib")   /* (the token privilege calls) */
static void *reserve_large(SIZE_T size) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return NULL;
    TOKEN_PRIVILEGES tp = {0};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL looked = LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid);
    const BOOL adjusted = looked && AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    const DWORD err = GetLastError();
    CloseHandle(token);
    if (!adjusted || err == ERROR_NOT_ALL_ASSIGNED) {
        hos_log("mem: HOS_LARGE_PAGES: this account does not hold SeLockMemoryPrivilege (\"Lock pages in memory\"): "
                "normal pages");
        return NULL;
    }
    const SIZE_T large = GetLargePageMinimum();
    if (!large || size % large) return NULL;
    void *p = VirtualAlloc((void *)A64_HOST_BASE, size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (!p) hos_log("mem: HOS_LARGE_PAGES: %llu MB of large pages not available (error %lu): normal pages",
                    (unsigned long long)(size >> 20), GetLastError());
    return p;
}
#endif

#if !defined(_WIN32) && !defined(A64_MEM_ZERO)
extern int hos_guest_memfd;   /* (below) */
static int startup_stderr = -1;   /* (keep startup warnings visible if stderr becomes hookshot_log.txt) */
#endif
/* The guest window lives at the constant A64_HOST_BASE (rt/a64.h), so lifted code never loads its base. */
static int reserve_guest(void) {
#if defined(A64_MEM_ZERO)
    hos_space_end = (uint64_t)(uintptr_t)&__global_base & ~0xFFFFULL;
    return hos_space_end >= (1ULL << 30);
#elif defined(_WIN32)
    /* Committed but untouched pages cost no physical memory. Write-watched: the renderer's buffer cache asks which
       pages changed (GetWriteWatch) so that it uploads only those (soft write watch builds mark pages themselves). */
#if defined(A64_WRITE_TRACK)
    const DWORD watch = 0;
    if (env_on("HOS_LARGE_PAGES") && reserve_large(GUEST_SPACE) == (void *)A64_HOST_BASE) {
        hos_large_pages = 1;
        hos_log("mem: the guest window is in large pages (%llu MB)", (unsigned long long)(GUEST_SPACE >> 20));
        return 1;
    }
#else
    const DWORD watch = MEM_WRITE_WATCH;
#endif
    /* mkw-recomp: 512 GB reserved (write-watched: the renderer's change tracking, a 16 MB bitmap), committed as the
       runtime hands memory out (hos_commit) or on first touch */
    void hos_commit_init(void);
    hos_commit_init();
    return VirtualAlloc((void *)A64_HOST_BASE, GUEST_SPACE, MEM_RESERVE | watch, PAGE_NOACCESS) == (void *)A64_HOST_BASE;
#else
    /* (linux) the whole window readable and writable at once, its pages made as they are first touched (no commit
       step: MAP_NORESERVE). Decommitting is madvise (host/posix/win32_posix.c VirtualFree): the pages read as zero. */
    void hos_commit_init(void);
    hos_commit_init();   /* (the crash report) */
    /* The window is a memfd's, shared, so the Vulkan back end can hand the GPU the game's own pages as a dma-buf
       (udmabuf, gpu/vk/vk_memory.cpp import_dma_buf) instead of a host pointer: amdgpu checks every page of a host-pointer
       import again at each command submission, 15-45 ms a submission in Kokiri Forest. Pages of a shared mapping are freed
       with MADV_REMOVE, not MADV_DONTNEED (hos_posix_guest_discard). HOS_GUEST_MEMFD=0: anonymous memory, as before */
    {
        /* (only where /dev/udmabuf can be opened: a shared mapping's pages cannot be host-pointer imports (RADV:
           VK_ERROR_INVALID_EXTERNAL_HANDLE), so a pool udmabuf refused would be no import at all. A pool larger than udmabuf's
           size_limit_mb (64 MB unless raised) is imported as several dma-bufs bound into one sparse buffer, so no setting is
           needed. HOS_GUEST_MEMFD=1: the memfd all the same) */
        const char *e = getenv("HOS_GUEST_MEMFD");
        int usable = 0;
        unsigned long long limit_mb = 0;
        {
            const int u = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
            if (u < 0 && (errno == EACCES || errno == EPERM)) {
                const int err = errno;
                static int warned;
                if (!warned) {
                    warned = 1;
                    const char *warning = "WARNING: /dev/udmabuf permission denied; GPU imports may be slower, especially on AMD/RADV. "
                        "Ask your administrator for access via its device group or a device-specific udev rule "
                        "(see README, On Linux). Do not run the game as root.";
                    if (startup_stderr >= 0) dprintf(startup_stderr, "%s (%s)\n", warning, strerror(err));
                    hos_log("%s (%s)", warning, strerror(err));
                }
            }
            if (startup_stderr >= 0) close(startup_stderr), startup_stderr = -1;
            if (u >= 0) {
                close(u);
                FILE *f = fopen("/sys/module/udmabuf/parameters/size_limit_mb", "r");
                if (f) {
                    if (fscanf(f, "%llu", &limit_mb) != 1) limit_mb = 0;
                    fclose(f);
                }
                usable = limit_mb > 0;
            }
            if (!usable && !e)
                hos_log("mem: guest memory stays host-pointer imports for the GPU (slow submissions on AMD): %s",
                        u < 0 ? "/dev/udmabuf cannot be opened" : "udmabuf's size_limit_mb cannot be read");
        }
        if (e ? atoi(e) != 0 : usable) {
            const int fd = (int)syscall(SYS_memfd_create, "hookshot-guest", 1u /* MFD_CLOEXEC */ | 2u /* MFD_ALLOW_SEALING */);
            if (fd >= 0 && ftruncate(fd, (off_t)GUEST_SPACE) == 0) {
                fcntl(fd, 1033 /* F_ADD_SEALS */, 0x0002 /* F_SEAL_SHRINK */ | 0x0004 /* F_SEAL_GROW */);
                void *p = mmap((void *)A64_HOST_BASE, GUEST_SPACE, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_NORESERVE, fd, 0);
                if (p == (void *)A64_HOST_BASE) {
                    hos_guest_memfd = fd;
                    hos_log("mem: the guest window is a memfd's (%d), shared with the GPU as dma-bufs", fd);
                    return 1;
                }
                if (p != MAP_FAILED) munmap(p, GUEST_SPACE);
            }
            if (fd >= 0) close(fd);
            hos_log("mem: no memfd for the guest window (errno %d): anonymous memory", errno);
        }
    }
    if (mmap((void *)A64_HOST_BASE, GUEST_SPACE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0) != (void *)A64_HOST_BASE)
        return 0;
    /* (issue 312) no transparent huge pages in the guest window: the renderer imports its pools as userptr memory, and every
       collapse khugepaged makes in them changes their mappings, which the kernel answers by pinning the pages again at the
       next submission: ~250 ms a submit on RADV while the GPU sat idle. HOS_THP=1: as before */
    if (!env_on("HOS_THP")) madvise((void *)A64_HOST_BASE, GUEST_SPACE, MADV_NOHUGEPAGE);
#ifdef __linux__
    /* (HOS_NO_THP=1, experiment: none outside the window either. With THP "always" 400-450 MB of the host heap's and the binary's bss are huge
       pages, each resident whole when any of its 2 MB is touched) */
    if (env_on("HOS_NO_THP")) prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
#endif
    return 1;
#endif
}

#if !defined(_WIN32) && !defined(A64_MEM_ZERO)
/* the guest window's memfd for the Vulkan back end (-1: anonymous memory), and freeing guest pages so that
   they read as zero again on either kind of mapping */
int hos_guest_memfd = -1;
int hos_posix_guest_memfd(void) { return hos_guest_memfd; }
int hos_posix_guest_discard(void *p, size_t n) {
    return madvise(p, n, hos_guest_memfd >= 0 ? MADV_REMOVE : MADV_DONTNEED);
}
#endif

/* The guest window, for the renderer (gpu/render/rx_buffer.cpp). */
uint8_t *hos_guest_base(void) { return A64_MEM; }
uint64_t hos_guest_size(void) { return GUEST_SPACE; }

/* Call guest code at `addr` as a fresh call from the top of the main thread's stack, with whatever arguments the
   caller put in x0-x7 (call_guest clears x0 and x1 first; call_guest_args keeps them). */
static void call_guest_args(Ctx *c, uint64_t addr, uint64_t sp, const char *what) {
    A64Fn f = a64_lookup(addr);
    if (!addr || !f) hos_fatal(c, "%s: no function at %#llx", what, (unsigned long long)addr);
    hos_log("boot: %s (%#llx)", what, (unsigned long long)addr);
    c->x[30] = 0;
    c->x[31] = sp;
    void hos_mem_checkpoint(const char *what);
    hos_mem_checkpoint(what);
    f(c);
}
static void call_guest(Ctx *c, uint64_t addr, uint64_t sp, const char *what) {
    c->x[0] = c->x[1] = 0;
    call_guest_args(c, addr, sp, what);
}

void hos_update_check(void);   /* (host/hos_update.c) */
#ifndef _WIN32
/* (linux) Saves live in $XDG_DATA_HOME/hookshot (~/.local/share/hookshot), so a rebuilt game folder still finds them.
   A user/ folder beside the executable from before is moved there the first time, entry by entry. Anything that
   cannot be moved keeps the old place. --user still wins.
   A symbolic link in it, to saves the player keeps elsewhere (a synced folder, another drive), is made again in the
   new place, to the same folder, and never entered. It was moved as it stood, and one written from where it stood
   ("../../saves") then named nothing; from another drive it was copied as the folder it names and delete_tree emptied
   that folder, which is not Hookshot's: the next start found the link still here, kept the old place, and showed no
   saves. A user/ that is a link itself is the player's choice of place, and is used as it is. */
static int is_dir(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int is_link(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
static const char *move_old, *move_from, *move_new;   /* (the user folder on the move, as realpath names it and as given, and the new place) */
static int moved_to(char *target, const char *dir) {   /* (a target under dir: named where it will be) */
    char there[MAX_PATH];
    const size_t n = dir && dir[0] == SEP ? strlen(dir) : 0;
    if (!n || strncmp(target, dir, n) || target[n] != SEP) return 0;
    snprintf(there, sizeof there, "%s%s", move_new, target + n);
    snprintf(target, MAX_PATH, "%s", there);
    return 1;
}
static int copy_link(const char *from, const char *to) {   /* (to what it names, in full; one that names nothing: as written) */
    char target[MAX_PATH];
    if (!realpath(from, target)) {
        const ssize_t n = readlink(from, target, sizeof target - 1);
        if (n < 0) return 0;
        target[n] = 0;
    }
    /* (to an entry that moves too: where it will be. One whose entry went first names nothing by now, so its text is
       checked too: else it named the old place, and only when the folder listed it before the entry) */
    if (!moved_to(target, move_old)) moved_to(target, move_from);
    return symlink(target, to) == 0;
}
static int has_link(const char *p) {
    if (is_link(p)) return 1;
    if (!is_dir(p)) return 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int found = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", p, fd.cFileName);
            found = has_link(a);
        } while (!found && FindNextFileA(h, &fd));
        FindClose(h);
    }
    return found;
}
static int copy_tree(const char *from, const char *to) {
    if (is_link(from)) return copy_link(from, to);
    if (!is_dir(from)) {
        FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
        int ok = in && out;
        char buf[1 << 16];
        for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
        ok = ok && in && !ferror(in);
        if (in) fclose(in);
        if (out && fclose(out)) ok = 0;
        return ok;
    }
    if (!CreateDirectoryA(to, NULL) && !is_dir(to)) return 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", from);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int ok = 1;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH], b[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", from, fd.cFileName);
            snprintf(b, sizeof b, "%s" SEP_S "%s", to, fd.cFileName);
            ok = copy_tree(a, b);
        } while (ok && FindNextFileA(h, &fd));
        FindClose(h);
    }
    return ok;
}
static void delete_tree(const char *p) {
    if (!is_dir(p) || is_link(p)) {   /* (a link goes; what it names stays) */
        DeleteFileA(p);
        return;
    }
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", p, fd.cFileName);
            delete_tree(a);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(p);
}
static const char *saves_dir(const char *beside) {
    static char dir[MAX_PATH];
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (is_link(beside)) return beside;
    if (xdg && xdg[0] == '/') snprintf(dir, sizeof dir, "%s", xdg);
    else if (home && home[0] == '/') {
        snprintf(dir, sizeof dir, "%s/.local", home);
        CreateDirectoryA(dir, NULL);
        snprintf(dir, sizeof dir, "%s/.local/share", home);
    } else return beside;
    CreateDirectoryA(dir, NULL);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "/hookshot");
    CreateDirectoryA(dir, NULL);
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) return beside;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", beside);
    int moved = 0, kept = 0;
    char real[MAX_PATH];
    move_old = realpath(beside, real), move_from = beside, move_new = dir;
    /* (looked at first, then moved: with an entry the new place has already, the old place is kept, and it keeps them all.
       The others used to go all the same, and the game then ran here without them) */
    for (int move = 0; move < 2 && !kept; move++) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return dir;
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char from[MAX_PATH], to[MAX_PATH];
            snprintf(from, sizeof from, "%s" SEP_S "%s", beside, fd.cFileName);
            snprintf(to, sizeof to, "%s" SEP_S "%s", dir, fd.cFileName);
            if (!move) kept += GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES;
            else if (!has_link(from) && MoveFileExA(from, to, 0)) moved++;
            else if (copy_tree(from, to)) delete_tree(from), moved++;
            else delete_tree(to), kept++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (kept) {
        hos_log("saves: %d of %s's entries could not go to %s: the saves stay in %s", kept, beside, dir, beside);
        return beside;
    }
    RemoveDirectoryA(beside);
    if (moved) {
        char note[MAX_PATH];
        const char *sep = strrchr(beside, SEP);
        snprintf(note, sizeof note, "%.*s" SEP_S "saves-moved.txt", sep ? (int)(sep - beside) : 0, beside);
        FILE *f = fopen(note, "w");
        if (f) {
            fprintf(f, "Hookshot's saves are now in\n%s\n\nThey moved there from the user folder that was here.\n", dir);
            fclose(f);
        }
        hos_log("saves: moved from %s to %s", beside, dir);
    }
    return dir;
}
#endif

/* hookshot_env.txt beside the game: NAME=VALUE lines set as environment variables before anything reads them, for the
   switches players are asked to test (RX_VK_NOOPT and the rest). Lines starting with # are notes. On Linux this runs
   before the window, so HOS_HEADLESS and HOS_WINDOW_SIZE are already set when the window is made.
   HOS_ENV_FILE=<path> reads that file instead: a scripted or A/B test run brings its own switches and leaves the
   player's hookshot_env.txt as it is.
   (hookshot) hookshot_env.txt is a file players pass around, so it sets only the game's own switches (RX_, HOS_,
   HOOKSHOT_, EDEN_, NVN_ and SDL_ names), never HOS_ENV_FILE or SDL's library override, and no value that is a
   path but RX_VK_DLSS_DIR's: the system's variables (the Vulkan loader's layers, PATH and the like) can load code, and
   the dump switches write files where they name. The file HOS_ENV_FILE names sets anything. */
static int env_line_allowed(const char *name, const char *value) {
    static const char *const prefixes[] = {"RX_", "HOS_", "HOOKSHOT_", "EDEN_", "NVN_", "SDL_"};
    int ok = 0;
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) ok |= !strncmp(name, prefixes[i], strlen(prefixes[i]));
    if (!ok || !strcmp(name, "HOS_ENV_FILE") || !strcmp(name, "SDL_DYNAMIC_API")) return 0;
    /* (hookshot) nor the libraries SDL loads by name (SDL_VULKAN_LIBRARY, SDL_OPENGL_LIBRARY, SDL_VIDEO_GL_DRIVER,
       SDL_VIDEO_EGL_DRIVER...): a bare file name is looked for beside the game and in the current folder on Windows */
    if (!strncmp(name, "SDL_", 4) && (strstr(name, "_LIB") || strstr(name, "GL_DRIVER"))) return 0;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    /* (hookshot) the switches that name a file to write or a folder take no value here, not even a bare name: one
       resolves in the game's folder (HOS_SAMPLE_FILE=hookshot_vk.dll emptied the renderer) */
    const size_t nl = strlen(name);
    int names_file = (nl > 5 && (!strcmp(name + nl - 5, "_FILE") || !strcmp(name + nl - 4, "_DIR"))) ||
                     !strcmp(name, "RX_VK_FRAME_LOG") || !strcmp(name, "HOS_PRESENT_TRACE");
    /* (hookshot) and the test switches whose value is a file or folder under another name (HOS_AUDIO_DUMP=image.bin
       overwrote the game's image), or that press buttons and menu keys on a timer (a shared file erased a save from
       the file select) */
    static const char *const by_name[] = {
        "HOS_AUDIO_DUMP", "HOS_AUDIO_WB_DUMP", "HOS_SHADER_DUMP", "HOS_FS_OVERLAY", "HOS_MODS", "HOS_INPUT_LIVE",
        "HOS_INPUT_SCRIPT", "HOS_SAMPLE_ALL", "HOS_CHAIN", "HOS_WATCH", "HOOKSHOT_BUILDER_HOME", "RX_VK_SHOTS", "RX_VK_SHOT_GO",
        "RX_VK_FRAMELOG", "RX_VK_HANG_DUMP", "RX_VK_DUMP", "RX_VK_DUMP_SPV", "RX_VK_DUMP_MESH_SPV", "RX_VK_DUMP_STAGES",
        "RX_VK_DUMP_FAILED", "RX_VK_IR", "RX_VK_MENU_SCRIPT", "RX_DUMP_WGSL", "RX_DUMP_SLOW_WGSL", "RX_DUMP_TICS",
        "RX_WGSL_OVERRIDE"};
    for (size_t i = 0; i < sizeof by_name / sizeof *by_name; i++) names_file |= !strcmp(name, by_name[i]);
    /* RX_VK_DLSS_DIR: a folder on this PC, not a share (\\server\x or //server/x) or a device path */
    if (!strcmp(name, "RX_VK_DLSS_DIR"))
        return !((value[0] == '\\' || value[0] == '/') && (value[1] == '\\' || value[1] == '/'));
    if (names_file) return 0;
    const int drive = ((value[0] | 0x20) >= 'a' && (value[0] | 0x20) <= 'z' && value[1] == ':');
    return !strpbrk(value, "/\\") && !drive && value[0] != '~' && strcmp(value, ".") && strcmp(value, "..");
}
static void load_hookshot_env(void) {
    char path[MAX_PATH + 32];
    const char *other = getenv("HOS_ENV_FILE");
    if (other && *other) {
        snprintf(path, sizeof path, "%s", other);
    } else {
        DWORD n = hos_game_path_a(path, MAX_PATH);
        while (n && path[n - 1] != 92 && path[n - 1] != 47) n--;
        memcpy(path + n, "hookshot_env.txt", 17);
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    /* The environment the game was started with wins over the file: a switch set for one launch (HOS_FULLSCREEN=0 in a
       shell, a script's RX_VK_PACE=1) beats the same name saved in the file. The file's own later lines still replace its
       earlier ones: the names it set are kept here (for the whole run: a second read sets them again), so they are not taken
       for the environment's */
    static char from_file[256][128];   /* (names of up to 127 characters; a file with more names than this has its later
                                          lines for the names past it taken as the environment's, logged as left out) */
    static int from_file_n;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;   /* (Notepad's BOM) */
        while (*s == ' ' || *s == 9) s++;
        size_t len = strlen(s);
        while (len && (s[len - 1] == 10 || s[len - 1] == 13 || s[len - 1] == ' ' || s[len - 1] == 9)) s[--len] = 0;
        if (!len || *s == '#' || !strchr(s, '=')) continue;
        char *eq = strchr(s, '=');
        *eq = 0;
        if (!(other && *other) && !env_line_allowed(s, eq + 1)) {
            fprintf(stderr, "env: hookshot_env.txt: %s left out (only the game's switches, and no paths, are taken from it)\n", s);
            continue;
        }
        int ours = 0;
        for (int i = 0; i < from_file_n && !ours; i++) ours = !strcmp(from_file[i], s);
        if (!ours && getenv(s)) {
            fprintf(stderr, "env: %s: %s left out (set in the environment, which goes first)\n",
                    other && *other ? "HOS_ENV_FILE" : "hookshot_env.txt", s);
            continue;
        }
        if (!ours && from_file_n < (int)(sizeof from_file / sizeof *from_file) && strlen(s) < sizeof from_file[0])
            strcpy(from_file[from_file_n++], s);
#ifdef _WIN32
        *eq = '=';
        _putenv(s);
#else
        setenv(s, eq + 1, 1);
#endif
    }
    fclose(f);
}

/* Switches this build no longer reads, still set (in the environment or hookshot_env.txt, as older issues told players):
   one log line each, naming what took its place, so a bug report shows the switch did nothing */
static void removed_switch_notes(void) {
    static const char *const gone[][2] = {
        {"RX_VK_NO_DESC_REUSE", "RX_VK_DESC_REUSE=never"},
        {"RX_VK_DESC_SLOTGEN", "RX_VK_DESC_REUSE (the same 0 and 1) or the Descriptor reuse row"},
        {"RX_VK_DESC_REUSE_DGEN", NULL},
        {"RX_VK_NO_NAN_GUARD", "RX_VK_NAN_GUARD=0"},
        {"RX_VK_FOLLOW_MESH", NULL},
        {"RX_VK_OOT", NULL},
        {"RX_VK_OOT_EXPOSURE", NULL},
        {"RX_VK_OOT_FOG", NULL},
        {"RX_VK_OOT_VISUAL", NULL},
        {"HOS_FIX_RES", NULL},
        {"HOS_SCENE_NATIVE", NULL},
        {"TOTK_MINIMAL_LAYOUT", NULL},
    };
    for (size_t i = 0; i < sizeof gone / sizeof *gone; i++)
        if (getenv(gone[i][0])) {
            if (gone[i][1]) hos_log("env: %s is no longer read: use %s", gone[i][0], gone[i][1]);
            else hos_log("env: %s is no longer read: it does nothing", gone[i][0]);
        }
}

int hos_oot_protect_hearts;
volatile int hos_oot_jump_held;   /* (the Cheats page's Moon jump) Jump is down: host/nn_hid.c */
volatile int hos_oot_live_action;   /* (test runs) the live input file's "!cheat <action>": host/nn_hid.c */

int hos_oot_hearts_enabled(void) {
    int rx_option(int id, int otherwise);
    const int menu = rx_option(HOS_OPT_CHEAT_HEARTS, -1);
    return menu < 0 ? hos_oot_protect_hearts : menu;
}

int hos_oot_is_player(Ctx *c, uint64_t actor) {
    if (recomp_load64(c, actor) != 0x84b6c460ULL) return 0;
    // Other actors share this vtable. The actor's resource record identifies Link.
    const uint64_t resource = recomp_load64(c, actor + 16);
    return resource && recomp_load32(c, resource + 88) == 0x79616c50 &&
           recomp_load16(c, resource + 92) == 0x7265 &&
           recomp_load8(c, resource + 94) == 0;
}

/* Movement speed (the Debug tab's row): Link's character controller and the row's percent, as oot_player_speed saw them on
   his last update (0: none, or 1x) */
static uint64_t oot_speed_controller;
static int oot_speed_percent = 100;

/* A character controller's forward speed being set (nx2/oot_patches.py patch_cct_speed: s0, in metres a second, or a frame when
   per_second is 0): Link's scaled by the row while it is above 1x and the player steers him (a scene's walk, as the one into a
   map, keeps its pace), the physics step moving him by it with its collisions. Only speeds up to 15 m/s (his run is 4.5): a ledge
   climb sets some 55 for a frame, and scaled it threw him past the ledge. Every other controller as it is. */
void hos_oot_cct_speed(Ctx *c, uint64_t controller, int per_second) {
    if (!controller || controller != __atomic_load_n(&oot_speed_controller, __ATOMIC_RELAXED)) return;
    const int percent = __atomic_load_n(&oot_speed_percent, __ATOMIC_RELAXED);
    int hos_move_stick_held(void);   /* host/nn_hid.c */
    if (percent <= 100 || percent > 400 || !hos_move_stick_held()) return;
    float speed;
    memcpy(&speed, &c->vreg[0][0], sizeof speed);
    if (!(fabsf(speed) * (per_second ? 1.0f : 60.0f) <= 15.0f)) return;   /* (and NaN) */
    speed *= percent / 100.0f;
    memcpy(&c->vreg[0][0], &speed, sizeof speed);
}

#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
/* OneShotAddOrRemovePouchContent (f_828d4a70) for one pouch content by actor name: delta > 0 adds, < 0 removes.
   Returns the event status (top bits of the result word), or -1 when the pouch is not ready. */
static int oot_pouch_change(Ctx *c, const char *name, int delta) {
    const uint64_t pouch_singleton = recomp_load64(c, 0x84c9e0d0ULL);
    const uint64_t state_singleton = recomp_load64(c, 0x84c9e2a8ULL);
    const uint64_t actor_db = recomp_load64(c, 0x84eb6888ULL);
    const uint64_t actor_info = recomp_load64(c, 0x84e8c060ULL);
    A64Fn edit = a64_lookup(0x828d4a70ULL);
    if (!edit || !pouch_singleton || !recomp_load64(c, pouch_singleton) ||
        !state_singleton || !recomp_load64(c, state_singleton) || !actor_db || !actor_info || !delta ||
        strlen(name) >= 0x60) return -1;

    // OneShotAddOrRemovePouchContent reads the event parameters through pointers.
    const Ctx saved = *c;
    const uint64_t event = (c->x[31] - 0xe0) & ~0xfULL;
    memset(A64_PTR(event), 0, 0xe0);
    memcpy(A64_PTR(event + 0x80), name, strlen(name) + 1);
    recomp_store64_sp(c, event + 0x20, event + 0x60);
    recomp_store64_sp(c, event + 0x30, event + 0x70);
    recomp_store64_sp(c, event + 0x38, event + 0x68);
    recomp_store64_sp(c, event + 0x48, event + 0x74);
    recomp_store64_sp(c, event + 0x50, event + 0x74);
    recomp_store64_sp(c, event + 0x60, event + 0x80);
    recomp_store64_sp(c, event + 0x68, 0x83f0a6d8ULL); // Script
    recomp_store32_sp(c, event + 0x70, (uint32_t)delta);
    c->x[31] = event;
    c->x[0] = event;
    edit(c);
    const int result = (int)(recomp_load32(c, event + 0x18) >> 30);
    *c = saved;
    return result;
}

/* QueryGetPouchContentNumByActorName (f_82a3c260): f_805b63e0(pouch manager, &name, 0, 0) counts one content. */
static int oot_pouch_count(Ctx *c, const char *name) {
    const uint64_t manager = recomp_load64(c, 0x84e98e28ULL);
    A64Fn count = a64_lookup(0x805b63e0ULL);
    if (!manager || !count || strlen(name) >= 0x60) return -1;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memcpy(A64_PTR(frame + 0x10), name, strlen(name) + 1);
    recomp_store64_sp(c, frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = manager, c->x[1] = frame, c->x[2] = 0, c->x[3] = 0;
    count(c);
    const int n = (int)(int32_t)c->x[0];
    *c = saved;
    return n;
}

static void oot_edit_pouch(Ctx *c, int action) {
    static const char *const items[] = {
        "Item_DekuSeed", "Item_DekuNut", "Item_DekuStick", "Item_Bomb", "Item_Arrow_Normal"
    };
    const unsigned item = (unsigned)(action - 200) / 32;
    const int delta = (action - 200) % 32 - 10;
    if (item >= sizeof items / sizeof items[0] || !delta || delta < -10 || delta > 10) return;
    const int result = oot_pouch_change(c, items[item], delta);
    if (result >= 0) hos_log("cheats: pouch %s %+d (event status %d)", items[item], delta, result);
}

/* The Inventory page (gpu/vk/oot_cheat_items.h): counts go to the menu twice a second, edits come back as actions. */
#define OOT_ITEM_NAME(label, name, kind) name,
#define OOT_ITEM_KIND(label, name, kind) kind,
#define OOT_NO_SECTION(name)
static const char *const oot_item_names[] = {OOT_CHEAT_ITEMS(OOT_NO_SECTION, OOT_ITEM_NAME)};
static const unsigned char oot_item_kinds[] = {OOT_CHEAT_ITEMS(OOT_NO_SECTION, OOT_ITEM_KIND)};
#define OOT_ITEM_COUNT (sizeof oot_item_names / sizeof oot_item_names[0])

static int oot_item_count(Ctx *c, const char *name);
static int oot_gamedata_bool(Ctx *c, uint32_t hash, int *value);
static int oot_gamedata_set_bool(Ctx *c, uint32_t hash, int value);
static int oot_gamedata_string(Ctx *c, uint32_t hash, char *buf, size_t size);

static void oot_inventory_refresh(Ctx *c, int64_t now) {
    void rx_option_set(int id, int value);
    static int64_t next;
    static int probed;
    if (now < next) return;
    next = now + 500000000LL;
    int loaded = 0;   /* (a save's counts read: the first item's is not -1) */
    for (unsigned i = 0; i < OOT_ITEM_COUNT; i++) {
        const int count = oot_item_count(c, oot_item_names[i]);
        if (!i) loaded = count >= 0;
        rx_option_set(OOT_CHEAT_ITEM_BASE + (int)i, count);
    }
    {   /* Link's age for the Debug tab's Switch age row: 0 child, 1 adult, -1 no save loaded (Sequence_IsChildhood) */
        int child = 1;
        rx_option_set(OOT_CHEAT_AGE_ID, loaded && oot_gamedata_bool(c, 0xcea220d3u, &child) ? (child ? 0 : 1) : -1);
    }
    if (loaded) {   /* the map Link is in and his age, logged when either changes: where a log's problem happened, the "Where in
                       the game" of the Insert menu's Bug report (gpu/vk/vk_bug_report.cpp reads the last of these lines) */
        static char logged[100];
        char map[80], line[100];
        int child = 1;
        if (oot_gamedata_string(c, 0x1d6189dau, map, sizeof map) && map[0]) {   /* Sequence_CurrentBanc */
            snprintf(line, sizeof line, "%s (%s)", map,
                     oot_gamedata_bool(c, 0xcea220d3u, &child) ? (child ? "child" : "adult") : "age not known");
            if (strcmp(line, logged)) hos_log("map: %s", line), snprintf(logged, sizeof logged, "%s", line);
        }
    }
    if (!probed && oot_pouch_count(c, "Item_DekuSeed") >= 0) {
        probed = 1;   /* (one line naming every count, both spellings: which names the pouch knows) */
        static const char *const probes[] = {OOT_CHEAT_ITEM_PROBES};
        const unsigned total = (unsigned)(OOT_ITEM_COUNT + sizeof probes / sizeof probes[0]);
        char line[512];
        size_t used = 0;
        for (unsigned i = 0; i < total; i++) {   /* (several short lines: the log cuts long ones) */
            const char *name = i < OOT_ITEM_COUNT ? oot_item_names[i] : probes[i - OOT_ITEM_COUNT];
            used += (size_t)snprintf(line + used, sizeof line - used, " %s=%d", name, oot_pouch_count(c, name));
            if (used > 300 || i + 1 == total) hos_log("cheats: inventory probe:%s", line), used = 0;
        }
    }
}

/* Masks share one slot (the Happy Mask Shop lends one at a time). The game takes "Item_Mask_X" when adding one and
   may keep it as "Armor_Child_Mask_X", so both spellings count and a new mask first returns the one held. */
static int oot_mask_alias(const char *name, char *alias, size_t size) {
    if (!strncmp(name, "Armor_Child_Mask_", 17)) return snprintf(alias, size, "Item_Mask_%s", name + 17), 1;
    return 0;
}

static int oot_item_count(Ctx *c, const char *name) {
    char alias[64];
    const int n = oot_pouch_count(c, name);
    if (!oot_mask_alias(name, alias, sizeof alias)) return n;
    const int m = oot_pouch_count(c, alias);
    return n > m ? n : m;
}

static void oot_inventory_edit(Ctx *c, int action) {
    const unsigned item = (unsigned)(action - OOT_CHEAT_ITEM_ACTION) / 256, op = (unsigned)(action - OOT_CHEAT_ITEM_ACTION) % 256;
    if (item >= OOT_ITEM_COUNT) return;
    const char *name = oot_item_names[item];
    char alias[64];
    const int is_mask = oot_mask_alias(name, alias, sizeof alias);
    const int have = oot_item_count(c, name);
    int delta = 0;
    if (op == 200) delta = have > 0 ? -have : 1;   /* unlock / relock */
    else if (op < 128) delta = (int)op - 64;
    if (!delta || have < 0) return;
    if (is_mask && delta > 0) {   /* return whichever mask is in the slot first */
        for (unsigned i = 0; i < OOT_ITEM_COUNT; i++) {
            char other_alias[64];
            if (i == item || !oot_mask_alias(oot_item_names[i], other_alias, sizeof other_alias)) continue;
            const char *spellings[2] = {oot_item_names[i], other_alias};
            for (unsigned k = 0; k < 2; k++) {
                const int held = oot_pouch_count(c, spellings[k]);
                if (held > 0) hos_log("cheats: inventory returned %s (status %d)", spellings[k], oot_pouch_change(c, spellings[k], -held));
            }
        }
        delta = 1;
    }
    int result = -1;
    if (is_mask && delta < 0) {   /* remove under whichever spelling holds it */
        const char *spellings[2] = {name, alias};
        for (unsigned k = 0; k < 2; k++) {
            const int held = oot_pouch_count(c, spellings[k]);
            if (held > 0) result = oot_pouch_change(c, spellings[k], -held);
        }
    } else {
        result = oot_pouch_change(c, is_mask ? alias : name, delta);   /* (masks are added as Item_Mask_X, as the shop does) */
    }
    hos_log("cheats: inventory %s %+d (had %d, now %d, event status %d)", name, delta, have, oot_item_count(c, name), result);
}

/* The Cheats page's Infinite ammo: twice a second each consumable is brought back to the most the pouch has held since
   the row was turned on, through the same pouch edit the Inventory page uses. An item the pouch does not hold yet
   (count 0 or unknown) is left alone, so the cheat never unlocks one. */
static void oot_infinite_ammo(Ctx *c, int64_t now) {
    int rx_option(int id, int otherwise);
    static const char *const ammo[] = {"Item_DekuStick", "Item_DekuNut", "Item_DekuSeed", "Item_Bomb", "Item_Bombchu",
                                       "Item_Arrow_Normal"};
    enum { AMMO_COUNT = sizeof ammo / sizeof ammo[0] };
    static int most[AMMO_COUNT];
    static int64_t next;
    if (rx_option(HOS_OPT_CHEAT_AMMO, 0) <= 0) {
        memset(most, 0, sizeof most);
        return;
    }
    if (now < next) return;
    next = now + 500000000LL;
    for (unsigned i = 0; i < AMMO_COUNT; i++) {
        const int have = oot_pouch_count(c, ammo[i]);
        if (have > most[i]) most[i] = have;
        else if (have >= 0 && have < most[i]) {
            static int logged;   /* (the first refills only: a test run reads them, play would fill the log) */
            const int result = oot_pouch_change(c, ammo[i], most[i] - have);
            if (logged < 8) logged++, hos_log("cheats: infinite ammo: %s %d -> %d (event status %d)", ammo[i], have, most[i], result);
        }
    }
}

static void oot_set_time(Ctx *c, unsigned hour) {
    // ExecuteWmSetTime passes seconds since midnight to the world manager.
    const uint64_t singleton = recomp_load64(c, 0x84c9e898ULL);
    const uint64_t world = singleton ? recomp_load64(c, singleton) : 0;
    A64Fn set_time = a64_lookup(0x834722d0ULL);
    if (!set_time || !world || !recomp_load64(c, world + 0x510)) {
        hos_log("cheats: world clock unavailable");
        return;
    }
    const Ctx saved = *c;
    c->x[0] = world;
    c->x[1] = hour * 3600;
    set_time(c);
    *c = saved;
    hos_log("cheats: requested %02u:00", hour);
}

/* The Cheats page's Lock time: the clock goes back to the chosen hour every 5 s (ExecuteWmSetTime, as Set time). */
static void oot_lock_time(Ctx *c, int64_t now) {
    int rx_option(int id, int otherwise);
    static int64_t next;
    const int hour = rx_option(HOS_OPT_CHEAT_TIMELOCK, -1);
    if (hour < 0 || hour > 23 || now < next) return;
    next = now + 5000000000LL;
    const uint64_t singleton = recomp_load64(c, 0x84c9e898ULL);
    const uint64_t world = singleton ? recomp_load64(c, singleton) : 0;
    A64Fn set_time = a64_lookup(0x834722d0ULL);
    if (!set_time || !world || !recomp_load64(c, world + 0x510)) return;
    const Ctx saved = *c;
    c->x[0] = world;
    c->x[1] = (uint64_t)hour * 3600;
    set_time(c);
    *c = saved;
    static int told;
    if (!told) told = 1, hos_log("cheats: clock held at %02d:00", hour);
}

/* Ctrl+1 hearts, Ctrl+3 ammo, Ctrl+4 moon jump, Ctrl+5 lock time: each switches its row while the game is in front. */
static void oot_cheat_hotkeys(void) {
    void rx_option_set(int id, int value);
    int rx_option(int id, int otherwise);
    static const int keys[] = {'1', '3', '4', '5'};
    static const int ids[] = {HOS_OPT_CHEAT_HEARTS, HOS_OPT_CHEAT_AMMO, HOS_OPT_CHEAT_MOONJUMP, HOS_OPT_CHEAT_TIMELOCK};
    static int held[4];
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    const int ctrl = foreground == GetCurrentProcessId() && !rx_option(1, 0) && GetAsyncKeyState(VK_CONTROL) < 0;
    for (unsigned i = 0; i < 4; i++) {
        const int down = ctrl && GetAsyncKeyState(keys[i]) < 0;
        if (down && !held[i]) rx_option_set(ids[i], 1), hos_log("cheats: Ctrl+%c switched a row", keys[i]);
        held[i] = down;
    }
}

/* Cross-map teleport (build e0f3ccfa). Map changes in this game only run through an event: an entrance actor
   requests the Dm_SY_BancChangeGo event with NextBanc / NextStartPos / BootEventName arguments, the event calls
   GameSystemActor's ReserveBancChange, and the sequence's ChangeBancOnEvent loads the map when the event ends.
   This builds the same event request (see work/oot-teleport-research.md). The menu labels in gpu/vk/vk_menu.cpp
   (teleport_names) follow this table's order. */
typedef struct {
    const char *child_banc, *child_start, *child_boot;
    const char *adult_banc, *adult_start, *adult_boot;
} OotDestination;
static const OotDestination oot_destinations[] = {   /* (generated: work/oot-teleport-tools/write_destinations.py) */
    {"KokiriForest_Child", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive",
     "KokiriForest_Adult", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive"},   /* Kokiri Forest */
    {"HyruleField_Child", "HyruleField_FrontOfLostWoodBridge", "Dm_SY_BancChangeArrive",
     "HyruleField_Adult", "HyruleField_FrontOfLostWoodBridge", "Dm_SY_BancChangeArrive"},   /* Hyrule Field */
    {"KakarikoVillage_Child", "KakarikoVillage_Entrance", "Dm_SY_BancChangeArrive_KakarikoVillage_01",
     "KakarikoVillage_Adult", "KakarikoVillage_Entrance", "Dm_SY_BancChangeArrive_KakarikoVillage_01"},   /* Kakariko Village */
    {"Market_Child", "Market_Entrance", "Dm_SY_BancChangeArrive_Market_02",
     "Market_Adult", "Market_Entrance", "Dm_SY_BancChangeArrive_Market_02"},   /* Market */
    {"HyruleCastle", "HyruleCastle_Entrance", "Dm_SY_BancChangeArrive_HyruleCastle_01",
     "GanonCastle", "GanonCastleEntrance", "Dm_SY_BancChangeArrive_GanonCastle_01"},   /* Hyrule Castle */
    {"LostWood_Child", "LostWood_FrontOfSacredForestMeadow", "Dm_SY_BancChangeArrive_LostWood",
     "LostWood_Adult", "LostWood_FrontOfSacredForestMeadow", "Dm_SY_BancChangeArrive_LostWood"},   /* Lost Woods */
    {"SacredForestMeadow_Child", "OutsideOfForestTemple", "Dm_SY_BancChangeArrive_SacredForestMeadow_01",
     "SacredForestMeadow_Adult", "OutsideOfForestTemple", "Dm_SY_BancChangeArrive_SacredForestMeadow_01"},   /* Sacred Forest Meadow */
    {"HyruleField_Child", "LakeHylia_FromGerudoValley", "Dm_SY_BancChangeArrive_LakeHylia_02",
     "HyruleField_Adult", "LakeHylia_FromGerudoValley", "Dm_SY_BancChangeArrive_LakeHylia_02"},   /* Lake Hylia */
    {"Graveyard_Child", "GraveYard_KakarikoSide", "Dm_SY_BancChangeArrive_Graveyard_01",
     "Graveyard_Adult", "GraveYard_KakarikoSide", "Dm_SY_BancChangeArrive_Graveyard_01"},   /* Graveyard */
    {"DeathMountainTrail_Child", "MountainTrail_KakarikoSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_01",
     "DeathMountainTrail_Adult", "MountainTrail_KakarikoSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_01"},   /* Death Mountain Trail */
    {"DeathMountainCrater_Child", "Crater_GoronCitySide", "DeathMountainCrater_Arrive",
     "DeathMountainCrater_Adult", "Crater_GoronCitySide", "DeathMountainCrater_Arrive"},   /* Death Mountain Crater */
    {"GoronCity_Child", "GoronCity_MountainTrailSide", "Dm_SY_BancChangeArrive_GoronCity_01",
     "GoronCity_Adult", "GoronCity_MountainTrailSide", "Dm_SY_BancChangeArrive_GoronCity_01"},   /* Goron City */
    {"ZoraRiver_Child", "ZoraRiver_Entrance", "Dm_SY_BancChangeArrive",
     "ZoraRiver_Adult", "ZoraRiver_Entrance", "Dm_SY_BancChangeArrive"},   /* Zora's River */
    {"ZoraDomain_Child", "ZoraDomain_FromZoraRiver", "Dm_SY_BancChangeArrive",
     "ZoraDomain_Adult", "ZoraDomain_FromZoraRiver", "Dm_SY_BancChangeArrive"},   /* Zora's Domain */
    {"ZoraFountain_Child", "ZoraFountain_FromZoraDomain", "Dm_SY_BancChangeArrive",
     "ZoraFountain_Adult", "ZoraFountain_FromZoraDomain", "Dm_SY_BancChangeArrive"},   /* Zora's Fountain */
    {"GerudoValley_Child", "GerudoValley_Entrance", "FromHyruleField_ArriveGerudoValley",
     "GerudoValley_Adult", "GerudoValley_Entrance", "FromHyruleField_ArriveGerudoValley"},   /* Gerudo Valley */
    {NULL, NULL, NULL,
     "GerudoFortress_Adult", "GerudoFortress_FromHountedWasteland", "Dm_SY_BancChangeArrive_GerudoFortress_02"},   /* Gerudo Fortress */
    {"HountedWasteland_Child", "HountedWasteland_FromGerudoFortress", "FromGerudoFortress_ArriveWateland",
     "HountedWasteland_Adult", "HountedWasteland_FromGerudoFortress", "FromGerudoFortress_ArriveWateland"},   /* Haunted Wasteland */
    {"DesertColossus_Child", "DesertColossus_FromHountedWasteland", "Dm_SY_BancChangeArrive_DesertColossus_01",
     "DesertColossus_Adult", "DesertColossus_FromHountedWasteland", "Dm_SY_BancChangeArrive_DesertColossus_01"},   /* Desert Colossus */
    {"InsideTheDekuTree", "Entrance", "Dm_SY_BancChangeArrive_InsideTheDekuTree_01",
     "InsideTheDekuTree", "Entrance", "Dm_SY_BancChangeArrive_InsideTheDekuTree_01"},   /* Inside the Deku Tree */
    {"DodongoCavern", "DodongoCavern_MountainTrailSide", "Dm_SY_BancChangeArrive",
     "DodongoCavern", "DodongoCavern_MountainTrailSide", "Dm_SY_BancChangeArrive"},   /* Dodongo's Cavern */
    {"InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", "Dm_SY_BancChangeArrive",
     "InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", "Dm_SY_BancChangeArrive"},   /* Jabu-Jabu's Belly */
    {"ForestTemple", "ForestTemple", "Dm_SY_BancChangeArrive",
     "ForestTemple", "ForestTemple", "Dm_SY_BancChangeArrive"},   /* Forest Temple */
    {"FireTemple", "FireTemple", "Dm_SY_BancChangeArrive",
     "FireTemple", "FireTemple", "Dm_SY_BancChangeArrive"},   /* Fire Temple */
    {"WaterTemple", "WaterTemple_FromLakeHylia", "SageOfWater_Arrival_WaterTemple",
     "WaterTemple", "WaterTemple_FromLakeHylia", "SageOfWater_Arrival_WaterTemple"},   /* Water Temple */
    {"ShadowTemple", "Entrance", "ShadowTemple_Arrive",
     "ShadowTemple", "Entrance", "ShadowTemple_Arrive"},   /* Shadow Temple */
    {"SpiritTemple", "SpiritTemple_FromRightHand", "Dm_SY_BancChangeArrive",
     "SpiritTemple", "SpiritTemple_FromRightHand", "Dm_SY_BancChangeArrive"},   /* Spirit Temple */
    {"BottomOfTheWell", "Well_KakarikoSide", "Dm_SY_BancChangeArrive",
     "BottomOfTheWell", "Well_KakarikoSide", "Dm_SY_BancChangeArrive"},   /* Bottom of the Well */
    {"IceCavern", "IceCavern_FromZoraFountain", "SageOfWater_Arrival_IceCavern",
     "IceCavern", "IceCavern_FromZoraFountain", "SageOfWater_Arrival_IceCavern"},   /* Ice Cavern */
    {"GerudoTrainingGround", "GerudoTrainingGround_FromGerudoFortress", "FromGerudoFortress_ArriveTrainingGround",
     "GerudoTrainingGround", "GerudoTrainingGround_FromGerudoFortress", "FromGerudoFortress_ArriveTrainingGround"},   /* Gerudo Training Ground */
    {"InsideGanonCastle", "Entrance", "Dm_SY_BancChangeArrive_InsideGanonCastle_01",
     "InsideGanonCastle", "Entrance", "Dm_SY_BancChangeArrive_InsideGanonCastle_01"},   /* Ganon's Castle */
};
#define OOT_AGE_ACTION 999   /* (the Cheats page's Age row) */
#define OOT_TELEPORT_ACTION 1000   /* (pouch edits use 200 to 348) */
#define OOT_GAMEDATA_SLOT 0x84c9e180ULL   /* GameDataMgr, as ReserveBancChange's readers load it */
#define OOT_EVENTMGR_SLOT 0x84c9e090ULL   /* EventMgr, as SeqOneShotRequestEvent loads it */
#define OOT_EVENT_ARG_SIZE 0x1b20ULL      /* EventRequestArg: SeqOneShotRequestEvent reserves this much stack */

static uint64_t oot_singleton(Ctx *c, uint64_t slot) {
    const uint64_t holder = recomp_load64(c, slot);
    return holder ? recomp_load64(c, holder) : 0;
}

/* GameDataMgr getters by MurMur3 name hash: f_8022d9c0 (bool) and f_80cd0ea0 (String64). 1 when found. */
static int oot_gamedata_bool(Ctx *c, uint32_t hash, int *value) {
    const uint64_t mgr = oot_singleton(c, OOT_GAMEDATA_SLOT);
    A64Fn get = a64_lookup(0x8022d9c0ULL);
    if (!mgr || !get) return 0;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    recomp_store64_sp(c, out, 0);
    c->x[31] = out;
    c->x[0] = mgr, c->x[1] = out, c->x[2] = hash;
    get(c);
    const int found = (int)(c->x[0] & 1);
    *value = recomp_load8(c, out) & 1;
    *c = saved;
    return found;
}

static int oot_gamedata_string(Ctx *c, uint32_t hash, char *buf, size_t size) {
    const uint64_t mgr = oot_singleton(c, OOT_GAMEDATA_SLOT);
    A64Fn get = a64_lookup(0x80cd0ea0ULL);
    buf[0] = 0;
    if (!mgr || !get) return 0;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    recomp_store64_sp(c, out, 0);
    c->x[31] = out;
    c->x[0] = mgr, c->x[1] = out, c->x[2] = hash;
    get(c);
    const int found = (int)(c->x[0] & 1);
    const uint64_t text = recomp_load64(c, out);
    *c = saved;
    if (!found || !text) return 0;
    for (size_t i = 0; i + 1 < size; i++) {
        buf[i] = (char)recomp_load8(c, text + i);
        buf[i + 1] = 0;
        if (!buf[i]) break;
    }
    return 1;
}

/* One argument of an EventRequestArg: the array of 200-byte entries at arg+8, capacity arg+0, count arg+16.
   Entry: +0 type (1 string pointer, 4 bool), +8 name buffer, +16 its capacity, +152 the value. As
   QueryBancChangeEventRequestArg (f_829df380) fills them. A string value is a pointer the request does not copy. */
static int oot_event_arg(Ctx *c, uint64_t arg, const char *name, uint32_t type, uint64_t value) {
    const uint32_t capacity = recomp_load32(c, arg), count = recomp_load32(c, arg + 16);
    if (count >= capacity) return 0;
    const uint64_t entry = recomp_load64(c, arg + 8) + (uint64_t)count * 200;
    const uint64_t name_buf = recomp_load64(c, entry + 8);
    const uint32_t name_cap = recomp_load32(c, entry + 16);
    const size_t length = strlen(name);
    if (!name_buf || length + 1 > name_cap) return 0;
    memcpy(A64_PTR(name_buf), name, length + 1);
    if (type == 4) recomp_store8(c, entry + 152, value & 1);
    else recomp_store64(c, entry + 152, value);
    recomp_store32(c, entry, type);
    recomp_store32(c, arg + 16, count + 1);
    return 1;
}

/* (the Main sequence's "Event" test: an event is playing) */
static int oot_event_playing(Ctx *c) {
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    return events && recomp_load64(c, events + 104);
}

static struct {
    int64_t requested_at, check_at;
    unsigned destination;
    const char *why, *banc;   /* (the last call's: why it was not sent, the map it was sent to; the control bridge's reply) */
    uint64_t result;          /* (and the event manager's answer to the request: 0 seen when the map did not change) */
} teleport;

static int oot_teleport(Ctx *c, uint64_t actor, unsigned index) {
    teleport.why = "no such destination", teleport.banc = NULL;
    if (index >= sizeof oot_destinations / sizeof oot_destinations[0]) return 0;
    const int64_t now = hos_now_ns();
    if (teleport.requested_at && now - teleport.requested_at < 8000000000LL) {
        hos_log("cheats: teleport ignored, a request is still in progress");
        teleport.why = "another teleport is still in progress (one every 8 s)";
        return 0;
    }
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    A64Fn make_arg = a64_lookup(0x80d99f10ULL), request = a64_lookup(0x80d9a820ULL);
    A64Fn set_link = a64_lookup(0x802dc440ULL), release_link = a64_lookup(0x8022b6f0ULL);
    if (!events || !make_arg || !request || !set_link || !release_link) {
        hos_log("cheats: teleport unavailable (event manager %#llx)", (unsigned long long)events);
        teleport.why = "the game's event manager is not there yet";
        return 0;
    }
    if (recomp_load64(c, events + 104)) {   /* (the Main sequence's "Event" test: an event is playing) */
        hos_log("cheats: teleport waits until the current event ends");
        teleport.why = "an event is playing (a cutscene, a map change's)";
        return 0;
    }
    int loading = 0, child = 1;
    if (oot_gamedata_bool(c, 0xef2b60c2u, &loading) && loading) {   /* Sequence_IsLoadingBanc */
        hos_log("cheats: teleport ignored while a map loads");
        teleport.why = "a map is loading";
        return 0;
    }
    if (!oot_gamedata_bool(c, 0xcea220d3u, &child)) hos_log("cheats: Sequence_IsChildhood not found, using child maps");
    const OotDestination *d = &oot_destinations[index];
    const char *banc = child ? d->child_banc : d->adult_banc;
    const char *start = child ? d->child_start : d->adult_start;
    const char *boot = child ? d->child_boot : d->adult_boot;
    if (!banc) {
        hos_log("cheats: teleport %u has no %s map", index, child ? "child" : "adult");
        teleport.why = child ? "this destination has no map for the child" : "this destination has no map for the adult";
        return 0;
    }

    /* The event reads its string arguments after this request returns, so they live in memory that stays. */
    static uint64_t strings;
    if (!strings) strings = hos_sys_alloc(512, 16);
    if (!strings) return teleport.why = "out of memory", 0;
    memset(A64_PTR(strings), 0, 512);
    memcpy(A64_PTR(strings), banc, strlen(banc) + 1);
    memcpy(A64_PTR(strings + 128), start, strlen(start) + 1);
    memcpy(A64_PTR(strings + 256), boot, strlen(boot) + 1);

    const char *event_env = getenv("HOS_OOT_TELEPORT_EVENT");
    const char *event = event_env && *event_env ? event_env : "Dm_SY_BancChangeGo";
    const char *starter_env = getenv("HOS_OOT_TELEPORT_STARTER");
    const int with_starter = !(starter_env && !strcmp(starter_env, "0"));

    const Ctx saved = *c;
    const uint64_t arg = (c->x[31] - OOT_EVENT_ARG_SIZE - 0x100) & ~0xfULL;
    c->x[31] = arg;
    c->x[0] = arg;
    make_arg(c);
    int ok = 1;
    {   /* the event's name: arg+6800 points at its buffer, arg+6808 the capacity */
        const uint64_t buf = recomp_load64(c, arg + 6800);
        const uint32_t cap = recomp_load32(c, arg + 6808);
        if (!buf || strlen(event) + 1 > cap) ok = 0;
        else memcpy(A64_PTR(buf), event, strlen(event) + 1);
    }
    ok = ok && oot_event_arg(c, arg, "NextBanc", 1, strings);
    ok = ok && oot_event_arg(c, arg, "NextStartPos", 1, strings + 128);
    ok = ok && oot_event_arg(c, arg, "BootEventName", 1, strings + 256);
    ok = ok && oot_event_arg(c, arg, "IsCheckAir", 4, 0);
    if (ok && with_starter) {   /* the starter actor (arg+0x1950), as OneShotEventMemberRequestEventByEventName sets it */
        c->x[31] = arg;
        c->x[0] = arg + 0x1950, c->x[1] = actor, c->x[2] = 0;
        set_link(c);
    }
    uint64_t result = 0;
    if (ok) {
        c->x[31] = arg;
        c->x[0] = events, c->x[1] = arg;
        request(c);
        result = c->x[0];
    }
    /* SeqOneShotRequestEvent's cleanup of the arg's two actor links */
    for (unsigned i = 0; i < 2; i++) {
        const uint64_t link = arg + (i ? 0x1950 : 0x1968);
        recomp_store64(c, link, 0x84994b20ULL);
        c->x[31] = arg;
        c->x[0] = link;
        release_link(c);
    }
    *c = saved;
    /* (issues 2217, 2588) A map-hiding area can leave this saved flag set when a debug teleport skips its exit.
       Only clear it once the event manager takes the request; the destination may hide its own minimap.
       Use the game's Bool setter (mgr, value, hash): it queues the change for the live value AND the save's copy.
       Writing only the live byte restores the HUD until the next load, but saves keep the old hidden value. */
    int hidden = 0;
    if (ok && result && oot_gamedata_bool(c, 0x9a4dd779u, &hidden) && hidden) {
        A64Fn set_bool = a64_lookup(0x806a2b20ULL);
        const uint64_t mgr = oot_singleton(c, OOT_GAMEDATA_SLOT);
        if (set_bool && mgr) {
            c->x[0] = mgr, c->x[1] = 0, c->x[2] = 0x9a4dd779u;   /* MapIconDeactivateByAreaActor */
            set_bool(c);
            *c = saved;
            hos_log("cheats: teleport: the area left had hidden the minimap (MapIconDeactivateByAreaActor): restore queued");
        } else {   /* (another game build: say so, a report's log then shows why the minimap stayed away) */
            hos_log("cheats: teleport: the area left had hidden the minimap (MapIconDeactivateByAreaActor): %s not found, left hidden",
                    set_bool ? "the game data manager" : "the game's Bool setter");
        }
    }
    teleport.requested_at = now;
    teleport.check_at = now + 1500000000LL;
    teleport.destination = index;
    hos_log("cheats: teleport %s to %s at %s (boot %s, event %s, starter %d): request %s, result %#llx",
            child ? "child" : "adult", banc, start, boot, event, with_starter, ok ? "sent" : "not built",
            (unsigned long long)result);
    teleport.result = result;
    if (ok) teleport.banc = banc;
    else teleport.why = "the game's event request could not be built";
    return ok;   /* (sent; the request's own result is only logged) */
}

/* Writes a GameData bool directly: GameDataMgr+32 is the (hash, index) table (+44 its size), the bools are 24-byte
   entries at GameDataMgr+24 (+16 their count) with the value at +22, as f_8022d9c0 reads them. 1 when found. */
static int oot_gamedata_set_bool(Ctx *c, uint32_t hash, int value) {
    const uint64_t mgr = oot_singleton(c, OOT_GAMEDATA_SLOT);
    if (!mgr) return 0;
    const uint64_t table = recomp_load64(c, mgr + 32);
    const uint32_t size = recomp_load32(c, mgr + 44);
    if (!table || !size) return 0;
    for (uint32_t probe = 0, slot = hash % size; probe < size; probe++, slot = (slot + 1) % size) {
        const uint32_t key = recomp_load32(c, table + slot * 8ULL);
        if (!key) return 0;
        if (key != hash) continue;
        const uint32_t index = recomp_load32(c, table + slot * 8ULL + 4), count = recomp_load32(c, mgr + 16);
        if (index >= count) return 0;
        recomp_store8(c, recomp_load64(c, mgr + 24) + index * 24ULL + 22, value ? 1 : 0);
        return 1;
    }
    return 0;
}

/* Age toggle: flips Sequence_IsChildhood (what the Temple of Time event sets) and reloads the current map, or the
   Market when this map is not in oot_destinations, through the teleport so the other age's map and Link load. 1: the map
   change was sent (teleport.banc its map); 0: not, teleport.why says why (the control bridge's reply) */
static int oot_toggle_age(Ctx *c, uint64_t actor) {
    int child = 1;
    if (!oot_gamedata_bool(c, 0xcea220d3u, &child)) {
        hos_log("cheats: age toggle: Sequence_IsChildhood not found");
        teleport.why = "Link's age is not in the game's data (Sequence_IsChildhood not found)", teleport.banc = NULL;
        return 0;
    }
    char current[80];
    oot_gamedata_string(c, 0x1d6189dau, current, sizeof current);   /* Sequence_CurrentBanc */
    unsigned target = 3;   /* Market (the Temple of Time is in it) */
    for (unsigned i = 0; i < sizeof oot_destinations / sizeof oot_destinations[0]; i++) {
        const OotDestination *d = &oot_destinations[i];
        const char *other = child ? d->adult_banc : d->child_banc;
        if (other && ((d->child_banc && !strcmp(d->child_banc, current)) || (d->adult_banc && !strcmp(d->adult_banc, current)))) {
            target = i;
            break;
        }
    }
    if (!oot_gamedata_set_bool(c, 0xcea220d3u, !child)) {
        hos_log("cheats: age toggle: could not write Sequence_IsChildhood");
        teleport.why = "Link's age could not be written (Sequence_IsChildhood)", teleport.banc = NULL;
        return 0;
    }
    hos_log("cheats: age toggle: %s -> %s, reloading via destination %u (current map \"%s\")",
            child ? "child" : "adult", child ? "adult" : "child", target, current);
    if (!oot_teleport(c, actor, target)) {
        oot_gamedata_set_bool(c, 0xcea220d3u, child);
        hos_log("cheats: age toggle undone, the map change was not requested");
        return 0;
    }
    return 1;
}

static void oot_teleport_followup(Ctx *c) {
    if (!teleport.check_at || hos_now_ns() < teleport.check_at) return;
    teleport.check_at = 0;
    char next[80], current[80];
    oot_gamedata_string(c, 0xee27bf71u, next, sizeof next);     /* Sequence_ReservedNextBanc */
    oot_gamedata_string(c, 0x1d6189dau, current, sizeof current);   /* Sequence_CurrentBanc */
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    hos_log("cheats: teleport check: reserved \"%s\", current \"%s\", event playing %d", next, current,
            events && recomp_load64(c, events + 104) ? 1 : 0);
}

/* (hookshot) the control bridge (host/control.c): the map the game is in (Sequence_CurrentBanc), read on the game's thread
   with the player's update every 250 ms and kept for the bridge's thread; the last one known (it stays through loads and
   menus, where Link is not updated). And the debug teleport by destination, as the Cheats page asks for it: taken at the
   next update of the player, in play, and what came of it (sent, or refused and why) kept for the bridge's reply */
static SRWLOCK oot_map_lock = SRWLOCK_INIT;
static char oot_map[80];
static int oot_flags = -1;   /* (1 an event playing, 2 a map loading, 4 the child; -1 not read yet) */
static void oot_map_refresh(Ctx *c, int64_t now) {
    static int64_t next;
    if (now < next) return;
    next = now + 250000000LL;
    char map[80];
    if (!oot_gamedata_string(c, 0x1d6189dau, map, sizeof map) || !map[0]) return;   /* Sequence_CurrentBanc */
    int loading = 0, child = 1;
    oot_gamedata_bool(c, 0xef2b60c2u, &loading);   /* Sequence_IsLoadingBanc */
    oot_gamedata_bool(c, 0xcea220d3u, &child);     /* Sequence_IsChildhood */
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    const int flags = (events && recomp_load64(c, events + 104) ? 1 : 0) | (loading ? 2 : 0) | (child ? 4 : 0);
    AcquireSRWLockExclusive(&oot_map_lock);
    memcpy(oot_map, map, sizeof oot_map);
    oot_flags = flags;
    ReleaseSRWLockExclusive(&oot_map_lock);
}
/* (with the map) whether an event (a cutscene, a map change's) is playing, a map is loading, Link is the child: 0 before any */
int hos_oot_state(int *event, int *loading, int *child) {
    AcquireSRWLockShared(&oot_map_lock);
    const int f = oot_flags;
    ReleaseSRWLockShared(&oot_map_lock);
    if (f < 0) return 0;
    *event = f & 1, *loading = (f >> 1) & 1, *child = (f >> 2) & 1;
    return 1;
}
int hos_oot_current_map(char *out, size_t cap) {
    AcquireSRWLockShared(&oot_map_lock);
    const int have = oot_map[0] != 0;
    if (have) snprintf(out, cap, "%s", oot_map);
    ReleaseSRWLockShared(&oot_map_lock);
    return have;
}
int hos_oot_destination(int index, const char **child, const char **adult) {
    if (index < 0 || index >= (int)(sizeof oot_destinations / sizeof oot_destinations[0])) return 0;
    *child = oot_destinations[index].child_banc, *adult = oot_destinations[index].adult_banc;
    return 1;
}
/* The bridge's teleport: asked on its thread, taken at the player's next update (oot_bridge_teleport_take), and what came
   of it kept for the bridge, by ticket. A later ask replaces one not taken yet. (oot_map_lock) */
#define OOT_BRIDGE_AGE 0x10000   /* (oot_bridge_tp.asked: the age switch to the child, | 1 to the adult; not a destination) */
static struct {
    int asked;    /* the destination + 1 waiting to be taken, OOT_BRIDGE_AGE | adult the age switch; 0 none */
    int ticket;   /* the last ask's */
    int taking;   /* the ticket taken by the player's update and not done yet (it is not replaced meanwhile) */
    int done_ticket, result, accepted;   /* the last one taken: 1 sent, -1 refused, 2 (the age switch) Link was that age
                                            already; accepted: the event manager's answer */
    char why[96], map[80];
} oot_bridge_tp;
int hos_oot_teleport_ask(int index) {
    if (index < 0 || index >= (int)(sizeof oot_destinations / sizeof oot_destinations[0])) return 0;
    AcquireSRWLockExclusive(&oot_map_lock);
    const int ticket = ++oot_bridge_tp.ticket;
    __atomic_store_n(&oot_bridge_tp.asked, index + 1, __ATOMIC_RELAXED);   /* (read without the lock first: take) */
    ReleaseSRWLockExclusive(&oot_map_lock);
    return ticket;
}
/* The bridge's age switch to the adult (1) or the child (0): the Debug tab's Switch age (oot_toggle_age), asked and taken as
   a teleport is, with the same tickets, result and cancel. The age wanted goes with it and is checked when it is taken (the
   bridge's sample of Link's age can be old): already that age, nothing is done (result 2) */
int hos_oot_age_ask(int adult) {
    AcquireSRWLockExclusive(&oot_map_lock);
    const int ticket = ++oot_bridge_tp.ticket;
    __atomic_store_n(&oot_bridge_tp.asked, OOT_BRIDGE_AGE | (adult ? 1 : 0), __ATOMIC_RELAXED);
    ReleaseSRWLockExclusive(&oot_map_lock);
    return ticket;
}
/* 1 sent (map: the map it goes to; accepted: whether the game's event manager took the request), -1 refused (why: the
   reason), 2 (the age switch) Link was that age already, nothing done, 0 not taken yet (or being taken), -2 replaced by a
   later ask or cancelled */
int hos_oot_teleport_result(int ticket, char *why, size_t why_cap, char *map, size_t map_cap, int *accepted) {
    AcquireSRWLockShared(&oot_map_lock);
    int r = -2;
    if (oot_bridge_tp.done_ticket == ticket) {
        r = oot_bridge_tp.result, *accepted = oot_bridge_tp.accepted;
        snprintf(why, why_cap, "%s", oot_bridge_tp.why);
        snprintf(map, map_cap, "%s", oot_bridge_tp.map);
    } else if (oot_bridge_tp.taking == ticket || (oot_bridge_tp.ticket == ticket && oot_bridge_tp.asked)) r = 0;
    ReleaseSRWLockShared(&oot_map_lock);
    return r;
}
void hos_oot_teleport_cancel(int ticket) {   /* (the bridge gave up waiting: not taken later behind its back) */
    AcquireSRWLockExclusive(&oot_map_lock);
    if (oot_bridge_tp.ticket == ticket) __atomic_store_n(&oot_bridge_tp.asked, 0, __ATOMIC_RELAXED);
    ReleaseSRWLockExclusive(&oot_map_lock);
}
static int oot_bridge_teleport_take(int *ticket) {   /* (the player's update) the destination + 1 asked for, or 0 */
    if (!__atomic_load_n(&oot_bridge_tp.asked, __ATOMIC_RELAXED)) return 0;
    AcquireSRWLockExclusive(&oot_map_lock);
    const int asked = oot_bridge_tp.asked;
    *ticket = oot_bridge_tp.ticket;
    if (asked) oot_bridge_tp.taking = *ticket;   /* (still "not taken yet" to the bridge until done) */
    __atomic_store_n(&oot_bridge_tp.asked, 0, __ATOMIC_RELAXED);
    ReleaseSRWLockExclusive(&oot_map_lock);
    return asked;
}
static void oot_bridge_teleport_done(int ticket, int sent) {   /* sent: 1, 0 refused (teleport.why), 2 already that age */
    AcquireSRWLockExclusive(&oot_map_lock);
    if (sent == 2) {
        oot_bridge_tp.done_ticket = ticket, oot_bridge_tp.result = 2, oot_bridge_tp.taking = 0, oot_bridge_tp.accepted = 0;
        oot_bridge_tp.why[0] = oot_bridge_tp.map[0] = 0;
        ReleaseSRWLockExclusive(&oot_map_lock);
        return;
    }
    oot_bridge_tp.done_ticket = ticket, oot_bridge_tp.result = sent ? 1 : -1, oot_bridge_tp.taking = 0;
    oot_bridge_tp.accepted = sent && teleport.result != 0;
    snprintf(oot_bridge_tp.why, sizeof oot_bridge_tp.why, "%s", sent || !teleport.why ? "" : teleport.why);
    snprintf(oot_bridge_tp.map, sizeof oot_bridge_tp.map, "%s", sent && teleport.banc ? teleport.banc : "");
    ReleaseSRWLockExclusive(&oot_map_lock);
}

static uint64_t oot_gameplay_view(Ctx *c) {
    const uint64_t singleton = recomp_load64(c, 0x84ca6bc0ULL);
    const uint64_t manager = singleton ? recomp_load64(c, singleton) : 0;
    const uint64_t cameras = manager && recomp_load32(c, manager + 32) ? recomp_load64(c, manager + 40) : 0;
    const uint64_t camera = cameras ? recomp_load64(c, cameras) : 0;
    return camera ? recomp_load64(c, camera + 120) : 0;
}

static float oot_load_float(Ctx *c, uint64_t addr) {
    const uint32_t bits = recomp_load32(c, addr);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static _Thread_local uint64_t flight_transform_controller;
static _Thread_local uint64_t flight_pose_actor;
static _Thread_local float flight_transform_yaw;
static int64_t flight_player_seen;

/* Link's controller (vtable 0x84c54f10, the one OneShotPhysicsCctChangePosition moves) from his components, 0 when the chain
   is not there */
static uint64_t oot_player_controller(Ctx *c, uint64_t components) {
    const unsigned offsets[] = {80, 32, 16, 8};
    uint64_t controller = components;
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        controller = recomp_load64(c, controller + offsets[i]);
        if (!controller) return 0;
    }
    return recomp_load64(c, controller) == 0x84c54f10ULL ? controller : 0;
}

/* Movement speed (the Debug tab's row, HOS_OPT_CHEAT_SPEED in percent): Link's controller and the percent for
   hos_oot_cct_speed, which scales the forward speed the game sets on it. (The controller's speed rate beside it, +476, is no
   use: the game sets it again before each step.) */
static void oot_player_speed(Ctx *c, uint64_t components) {
    int rx_option(int id, int otherwise);
    static int logged = 100;
    int percent = rx_option(HOS_OPT_CHEAT_SPEED, 100);
    if (percent < 100 || percent > 400) percent = 100;
    __atomic_store_n(&oot_speed_controller, percent > 100 ? oot_player_controller(c, components) : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&oot_speed_percent, percent, __ATOMIC_RELAXED);
    if (percent != logged) hos_log("cheats: movement speed %d%%", percent);
    logged = percent;
}

/* Whether the game moved Link since the flight last placed him: he is more than 2 units (the flight moves him 4 a second)
   from where it holds him. A respawn after a fall or a warp keeps his actor, so the flight took him straight back to
   where it had him, over the same drop: he fell, respawned and fell again until the flight was switched off. It starts
   again from where the game put him instead. */
static int oot_flight_moved_away(Ctx *c, uint64_t actor, const float held[3]) {
    float apart = 0;
    for (unsigned i = 0; i < 3; i++) {
        const float d = oot_load_float(c, actor + 796 + i * 4) - held[i];
        apart += d * d;
    }
    return apart > 2.0f * 2.0f;
}

static void oot_update_player_cheats(Ctx *c, uint64_t actor, uint64_t components) {
    int rx_option(int id, int otherwise);
    static uint64_t flying_actor, marked_actor, marked_controller;
    static int64_t last_tick, last_player_tick;
    static float target[3], velocity[3], mark[3], yaw;
    /* Moon jump: while Jump is held Link rises as in the flight, straight up, and falls when it is let go. */
    const int flight_on = rx_option(HOS_OPT_CHEAT_FLIGHT, 0) > 0;
    const int moon = !flight_on && rx_option(HOS_OPT_CHEAT_MOONJUMP, 0) > 0 && hos_oot_jump_held;
    int flying = flight_on || moon;
    /* An event that takes Link (a map change he flew into, the age toggle's, a cutscene) waits for him to stand, and the
       flight holds him in the air: the screen the event had faded stayed black until the flight was switched off, and a
       second request met "waits until the current event ends". While an event plays he is let go and lands; the flight
       takes him again, from where he then stands, when it has ended. */
    if (flying && oot_event_playing(c)) flying = 0;
    if (!flying) flying_actor = 0;
    const int64_t now = hos_now_ns();
    __atomic_store_n(&flight_player_seen, now, __ATOMIC_RELAXED);
    if (marked_actor != actor || now - last_player_tick > 2000000000LL) marked_controller = 0;
    last_player_tick = now;
    int action = rx_option(HOS_OPT_CHEAT_ACTION, 0);
    if (!action) action = __atomic_exchange_n(&hos_oot_live_action, 0, __ATOMIC_RELAXED);
    int bridge_ticket = 0;   /* (the control bridge's teleport or age switch, host/control.c: what came of it is kept for its reply) */
    int bridge_adult = -1;   /* (the bridge's age switch: the age wanted, 1 the adult) */
    if (!action) {
        const int asked = oot_bridge_teleport_take(&bridge_ticket);
        if (asked & OOT_BRIDGE_AGE) action = OOT_AGE_ACTION, bridge_adult = asked & 1;
        else if (asked) action = OOT_TELEPORT_ACTION + asked - 1;
    }
    oot_teleport_followup(c);
    oot_inventory_refresh(c, now);
    oot_player_speed(c, components);
    oot_infinite_ammo(c, now);
    oot_lock_time(c, now);
    oot_cheat_hotkeys();
    oot_map_refresh(c, now);
    if (action >= OOT_CHEAT_ITEM_ACTION && action < OOT_CHEAT_ITEM_ACTION + 256 * (int)OOT_ITEM_COUNT) {
        oot_inventory_edit(c, action);
    } else if (action == OOT_AGE_ACTION) {
        int child = 1;   /* (the bridge's: Link's age now, not its sample's; already the age wanted, nothing to flip) */
        if (bridge_adult >= 0 && oot_gamedata_bool(c, 0xcea220d3u, &child) && !child == bridge_adult) {
            hos_log("cheats: age toggle: Link is the %s already", child ? "child" : "adult");
            oot_bridge_teleport_done(bridge_ticket, 2);
        } else {
            const int sent = oot_toggle_age(c, actor);
            if (bridge_ticket) oot_bridge_teleport_done(bridge_ticket, sent);
        }
    } else if (action >= OOT_TELEPORT_ACTION) {
        const int sent = oot_teleport(c, actor, (unsigned)(action - OOT_TELEPORT_ACTION));
        if (bridge_ticket) oot_bridge_teleport_done(bridge_ticket, sent);
    } else if (action >= 200) {
        oot_edit_pouch(c, action);
    } else if (action >= 100 && action < 124) {
        oot_set_time(c, action - 100);
    }
    if (!flying && action != 2 && action != 3) return;

    // Follow OneShotPhysicsCctChangePosition to Link's controller.
    const uint64_t controller = oot_player_controller(c, components);
    if (!controller || recomp_load64(c, 0x84c54f10ULL + 0xe8) != 0x806d2530ULL) return;
    A64Fn change_position = a64_lookup(0x806d2530ULL);
    if (!change_position) return;

    if (action == 2) {
        for (unsigned i = 0; i < 3; i++) {
            const uint32_t bits = recomp_load32(c, actor + 796 + i * 4);
            memcpy(&mark[i], &bits, sizeof bits);
        }
        marked_actor = actor;
        marked_controller = controller;
        hos_log("cheats: marked position %.2f %.2f %.2f", mark[0], mark[1], mark[2]);
    }
    const int returning = action == 3 && marked_controller == controller;
    if (action == 3 && !returning) hos_log("cheats: no mark for this player controller");
    if (!flying && !returning) return;
    const int64_t elapsed = now - last_tick;
    if (flying_actor != actor)
        yaw = atan2f(oot_load_float(c, actor + 816), oot_load_float(c, actor + 840));
    if (returning) {
        memcpy(target, mark, sizeof target);
        memset(velocity, 0, sizeof velocity);
        flying_actor = flying ? actor : 0;
    } else if (flying_actor != actor || elapsed < 0 || elapsed > 250000000 || oot_flight_moved_away(c, actor, target)) {
        for (unsigned i = 0; i < 3; i++) target[i] = oot_load_float(c, actor + 796 + i * 4);
        memset(velocity, 0, sizeof velocity);
        flying_actor = actor;
        last_tick = now;
        return;
    } else if (elapsed == 0) {
        return;
    }
    last_tick = now;

    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    if (!returning) {
        const float dt = fminf((float)elapsed * 1e-9f, 0.05f);
        float desired[3] = {0};
        if (foreground == GetCurrentProcessId() && !rx_option(1, 0) && !rx_option(HOS_OPT_CHEAT_CAMERA, 0)) {
            uint64_t hos_flight_input(void);
            const uint64_t axes = hos_flight_input();
            const float right = (int16_t)axes / 32767.0f, forward = (int16_t)(axes >> 16) / 32767.0f;
            const int vertical = (GetAsyncKeyState(VK_PRIOR) < 0) - (GetAsyncKeyState(VK_NEXT) < 0);
            const uint64_t view = oot_gameplay_view(c);
            float rx = 1, rz = 0;
            if (view && recomp_load64(c, view) == 0x84c18a88ULL) {
                const float x = oot_load_float(c, view + 8), z = oot_load_float(c, view + 16);
                const float length = hypotf(x, z);
                if (length > 0.01f) rx = x / length, rz = z / length;
            }
            desired[0] = rx * right + rz * forward;
            desired[1] = vertical ? vertical : (int16_t)(axes >> 32);
            desired[2] = rz * right - rx * forward;
        }
        if (moon) {
            static int64_t told;
            desired[0] = desired[2] = 0, desired[1] = 1;
            if (now - told > 1000000000LL) told = now, hos_log("cheats: moon jump at height %.2f", target[1]);
        }
        const float length = sqrtf(desired[0] * desired[0] + desired[1] * desired[1] + desired[2] * desired[2]);
        const float speed = 4.0f / fmaxf(1.0f, length);
        const float blend = 1.0f - expf(-12.0f * dt);
        for (unsigned i = 0; i < 3; i++) {
            velocity[i] += (desired[i] * speed - velocity[i]) * blend;
            target[i] += velocity[i] * dt;
        }
        if (hypotf(desired[0], desired[2]) > 0.08f) {
            const float delta = remainderf(atan2f(desired[0], desired[2]) - yaw, 6.28318531f);
            yaw += fmaxf(-8.0f * dt, fminf(delta, 8.0f * dt));
        }
    }

    const Ctx saved = *c;
    const uint64_t arg = c->x[31] - 16;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t bits;
        memcpy(&bits, &target[i], sizeof bits);
        recomp_store32_sp(c, arg + i * 4, bits);
    }
    recomp_store32_sp(c, arg + 12, 0);
    c->x[31] = arg;
    c->x[0] = controller;
    c->x[1] = arg;
    flight_transform_yaw = yaw;
    flight_pose_actor = flying && !returning ? actor : 0;
    flight_transform_controller = flying && !returning ? controller : 0;
    change_position(c);
    flight_transform_controller = 0;
    *c = saved;
    if (returning) hos_log("cheats: returned to %.2f %.2f %.2f", mark[0], mark[1], mark[2]);
}
#endif

int hos_oot_flight_shortcut_ready(void) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    const int64_t seen = __atomic_load_n(&flight_player_seen, __ATOMIC_RELAXED);
    return seen && hos_now_ns() - seen < 250000000;
#else
    return 0;
#endif
}

#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
static void oot_write_flight_rotation(Ctx *c, uint64_t matrix) {
    const float s = sinf(flight_transform_yaw), co = cosf(flight_transform_yaw);
    const float rotation[3][3] = {{co, 0, s}, {0, 1, 0}, {-s, 0, co}};
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 3; col++) {
            uint32_t bits;
            memcpy(&bits, &rotation[row][col], sizeof bits);
            recomp_store32(c, matrix + row * 16 + col * 4, bits);
        }
    }
}
#endif

void hos_oot_flight_rotation(Ctx *c, uint64_t controller, uint64_t matrix) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    if (flight_transform_controller == controller) oot_write_flight_rotation(c, matrix);
#else
    (void)c;
    (void)controller;
    (void)matrix;
#endif
}

void hos_oot_flight_pose(Ctx *c, uint64_t actor, uint64_t matrix) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    if (flight_pose_actor == actor) {
        flight_pose_actor = 0;
        oot_write_flight_rotation(c, matrix);
    }
#else
    (void)c;
    (void)actor;
    (void)matrix;
#endif
}

/* The new-game opening, build e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348. Sequence/Opening.module.ainb requests
   Opening_Introduction and Opening_LoadingKokiriForest (SeqOneShotRequestEvent, f_80d9a820; the event name is the
   pointer at the request arg + 6800, which that function reads). Opening_Introduction's flowchart writes the state
   on both paths: the clock at 6:00, Meet_DekuTree's challenge Step01, the forest terrain pin, then
   EventTransition of the timeline Dm_Kokiri_0030. Its own skip query (IsEventSkipedMostRecent, f_80de6cb0) then
   takes the short edge: Navi warped to the position that edge names, the in-game music, Navi's talked flag, the
   camera handed back, and an autosave. Opening_LoadingKokiriForest's flowchart plays the timeline Dm_Kokiri_0000
   and then waits until the forest banc has loaded. Neither timeline writes save data. Meet_DekuTree is a later
   flow and is not one of these names.

   With Skip opening on, the request is still sent, so the flowchart applies that edge. The two timelines are not
   started: f_83b5bfb0 is their only starter (EventTransition at 0x80eaba00). Leaving bit 0 of the timeline player
   + 0xf25 clear makes that caller take its own exit at 0x80eac5e0, which finishes the action node, instead of the
   exit at 0x80eac640 that leaves the timeline running. The skip query is answered yes only while one of these two
   requests is the current one, so the short edge runs and a title demo that uses the same query does not. The
   cutscene frame counter is not moved. mode 0 observes a request, 1 a timeline start (return 1: do not start it),
   2 the skip query (return 1: x0 is 1), 3 Link is updating and no event is playing (the opening has let him go). */
int hos_oot_opening(Ctx *c, int mode) {
    int rx_option(int id, int otherwise);
    static int armed;
    static char logged[64];
    char name[80];
    uint64_t p = 0;
    if (mode == 3) {
        armed = 0;
        logged[0] = 0;
        return 0;
    }
    if (!c || rx_option(HOS_OPT_SKIP_OPENING, 0) <= 0) {
        armed = 0;
        return 0;
    }
    name[0] = 0;
    if (mode == 0) {
        const uint64_t arg = c->x[1];
        p = arg ? recomp_load64(c, arg + 6800) : 0;
    } else if (mode == 1 && c->x[1]) {
        p = recomp_load64(c, c->x[1]);   /* EventTransition passes a pointer to the timeline's name */
    }
    if (p) {
        size_t i = 0;
        for (; i + 1 < sizeof name; i++) {
            name[i] = (char)recomp_load8(c, p + i);
            name[i + 1] = 0;
            if (!name[i]) break;
        }
    }
    if (mode == 0) {
        if (!strcmp(name, "Opening_Introduction") || !strcmp(name, "Opening_LoadingKokiriForest")) {
            armed = 1;
            if (strcmp(logged, name)) {
                size_t i = 0;
                for (; name[i] && i + 1 < sizeof logged; i++) logged[i] = name[i];
                logged[i] = 0;
                hos_log("opening skip: %s request sent", name);
            }
        } else if (name[0]) {
            /* a different event (Meet_DekuTree included) is not skipped and drops the arm */
            armed = 0;
            logged[0] = 0;
        }
        return 0;   /* the request is still sent */
    }
    if (!armed) return 0;
    if (mode == 2) {
        c->x[0] = 1;
        return 1;
    }
    if (mode == 1 && (!strcmp(name, "Dm_Kokiri_0000") || !strcmp(name, "Dm_Kokiri_0030"))) {
        const uint64_t player = c->x[0];
        if (player) recomp_store8(c, player + 0xf25, recomp_load8(c, player + 0xf25) & ~(uint64_t)1);
        return 1;
    }
    return 0;
}

void hos_oot_lift_player(Ctx *c, uint64_t actor) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    flight_pose_actor = 0;
    if (recomp_load64(c, actor) != 0x84b45e18ULL) return;
    const uint64_t components = recomp_load64(c, actor + 624);
    const uint64_t movement = components ? recomp_load64(c, components + 800) : 0;
    if (movement < HOS_HEAP_BASE + 0x1d50 || recomp_load64(c, movement + 24) != actor ||
        !hos_oot_is_player(c, movement - 0x1d50)) return;
    if (!oot_event_playing(c)) hos_oot_opening(c, 3);   /* the opening has let Link go: later events keep their own skip */

    // Link can be updated by different guest workers on successive frames.
    static SRWLOCK cheats_lock = SRWLOCK_INIT;
    if (!TryAcquireSRWLockExclusive(&cheats_lock)) return;
    oot_update_player_cheats(c, actor, components);
    ReleaseSRWLockExclusive(&cheats_lock);
#else
    (void)c;
    (void)actor;
#endif
}

#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
static void oot_build_free_view(Ctx *c, uint64_t view, uint64_t out) {
    int rx_option(int id, int otherwise);
    static uint64_t active_view;
    static int64_t last_tick;
    static double eye[3], yaw, pitch;
    const int64_t now = hos_now_ns();
    const int64_t elapsed = now - last_tick;

    if (!view) {
        active_view = 0;
        return;
    }
    if (active_view != view || elapsed > 500000000) {
        double target[3];
        memcpy(eye, A64_PTR(view + 96), sizeof eye);
        memcpy(target, A64_PTR(view + 120), sizeof target);
        const double x = eye[0] - target[0], y = eye[1] - target[1], z = eye[2] - target[2];
        yaw = atan2(x, z);
        pitch = atan2(y, hypot(x, z));
        active_view = view;
        hos_log("cheats: free camera at %.2f %.2f %.2f", eye[0], eye[1], eye[2]);
    }
    last_tick = now;
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    const double dt = elapsed > 0 && elapsed < 100000000 ? (double)elapsed * 1e-9 : 0;
    const int control = foreground == GetCurrentProcessId() && !rx_option(1, 0);
    int height = 0;
    uint64_t hos_free_camera_input(int *height);
    const uint64_t input = control ? hos_free_camera_input(&height) : 0;
    if (control) {
        yaw -= 1.6 * dt * (int16_t)(input >> 32) / 32767.0;
        pitch -= 1.2 * dt * (int16_t)(input >> 48) / 32767.0;
        pitch = fmax(-1.55, fmin(1.55, pitch));
    }
    const double sy = sin(yaw), cy = cos(yaw), sp = sin(pitch), cp = cos(pitch);
    const double axes[3][3] = {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}};
    if (control) {
        const double step = dt * (GetAsyncKeyState(VK_CONTROL) < 0 ? 30.0 : 8.0);
        const double right = (int16_t)input / 32767.0;
        const double forward = (int16_t)(input >> 16) / 32767.0;
        const int key_height = (GetAsyncKeyState(VK_PRIOR) < 0) - (GetAsyncKeyState(VK_NEXT) < 0);
        eye[1] += step * (key_height ? key_height : height);
        for (int i = 0; i < 3; i++)
            eye[i] += step * (right * axes[0][i] - forward * axes[2][i]);
    }
    for (int row = 0; row < 3; row++) {
        const double *axis = axes[row];
        const float values[4] = {(float)axis[0], (float)axis[1], (float)axis[2],
            (float)-(axis[0] * eye[0] + axis[1] * eye[1] + axis[2] * eye[2])};
        for (int col = 0; col < 4; col++) {
            uint32_t bits;
            memcpy(&bits, &values[col], sizeof bits);
            recomp_store32(c, out + (row * 4 + col) * 4, bits);
        }
    }
}
#endif

void hos_oot_free_view(Ctx *c, uint64_t view) {
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
    int rx_option(int id, int otherwise);
    if (!view || recomp_load64(c, view) != 0x84c18a88ULL) return;

    // Camera zero supplies the gameplay view. The same matrix routine also serves other cameras.
    if (oot_gameplay_view(c) != view) return;

    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    const int enabled = rx_option(HOS_OPT_CHEAT_CAMERA, 0);
    oot_build_free_view(c, enabled ? view : 0, view + 8);
    ReleaseSRWLockExclusive(&lock);
#else
    (void)c;
    (void)view;
#endif
}

float hos_aspect_ratio = 0;   /* (the window's default shape: host/display.c) */
/* (nx2/oot_wide.py) read by the widescreen patches in the game's code; 0: the game's own 16:9 */
volatile uint64_t hos_wide_out, hos_wide_scene;
volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
volatile uint32_t hos_wide_keep_mask = 0xf;   /* (nx2/oot_wide.py KEEP_VTABLES: the screens that keep the whole picture, a bit each; MkwVkHost.wide_menus) */
/* (nx2/oot_occlusion.py) read by the game's code where it decides to make occlusion queries: 1, none (HOOKSHOT_OCCLUSION_OFF=1) */
volatile int hos_occlusion_off;
static void occlusion_switch(void) {
    const char *e = getenv("HOOKSHOT_OCCLUSION_OFF");
    hos_occlusion_off = e && e[0] == '1';
    if (hos_occlusion_off) hos_log("occlusion off: the game makes no occlusion queries (HOOKSHOT_OCCLUSION_OFF=1)");
}
/* (hookshot, issue 1241) The game needs about 10 GB of memory and swap together (the page file on Windows): on 8 GB of RAM
   and no swap it closed before the title with no word of why, and adding swap let it play. Below that, the log says so and
   a message says it once at boot; the game starts anyway. The text, or 0 when there is enough */
#define HOS_MEMORY_NEEDED_MB 10240ull
static int memory_notice_text(unsigned long long ram_mb, unsigned long long swap_mb, char *out, size_t cap) {
    if (ram_mb + swap_mb >= HOS_MEMORY_NEEDED_MB) return 0;
    snprintf(out, cap,
             "This PC has %.1f GB of memory and %.1f GB of swap (the page file on Windows). The game needs about %llu GB of the "
             "two together; with less it can close by itself while it loads.\n\nClose other programs, or add swap (on Windows: "
             "let it manage the page file's size), then start the game again. It starts now anyway.",
             ram_mb / 1024.0, swap_mb / 1024.0, HOS_MEMORY_NEEDED_MB / 1024);
    return 1;
}
#ifndef A64_MEM_ZERO
static void memory_notice(void) {
    unsigned long long ram = 0, swap = 0;   /* (MB) */
#ifdef _WIN32
    MEMORYSTATUSEX m = {sizeof m};
    if (!GlobalMemoryStatusEx(&m)) return;
    ram = m.ullTotalPhys >> 20;
    swap = m.ullTotalPageFile > m.ullTotalPhys ? (m.ullTotalPageFile - m.ullTotalPhys) >> 20 : 0;   /* (the commit limit: RAM and page file) */
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si)) return;
    ram = ((unsigned long long)si.totalram * si.mem_unit) >> 20, swap = ((unsigned long long)si.totalswap * si.mem_unit) >> 20;
#else
    return;
#endif
    char text[512];
    if (!memory_notice_text(ram, swap, text, sizeof text)) {
        hos_log("boot: %llu MB of memory, %llu MB of swap", ram, swap);
        return;
    }
    hos_log("boot: LOW MEMORY: %llu MB of memory, %llu MB of swap, below the ~%llu MB the game needs", ram, swap, HOS_MEMORY_NEEDED_MB);
    if (env_on("HOS_HEADLESS")) return;
#ifndef _WIN32
    int hos_sdl_load(void);
    hos_sdl_load();   /* (host/posix MessageBoxA shows it through the SDL already loaded) */
#endif
    MessageBoxA(NULL, text, "Hookshot", MB_OK | MB_ICONWARNING);
}
#endif
#ifdef _WIN32
/* (hookshot) On laptops with two GPUs, the drivers run a program on the dedicated one when it exports these (NVIDIA Optimus, AMD
   PowerXpress): without them a laptop could put the game on its integrated GPU, 5 fps (issue 176) */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* (hookshot) The game's paths are UTF-8 char strings through Windows' A functions and fopen: its manifest (nx2/win_manifest.py)
   makes the process's code page UTF-8, from Windows 10 1903 on. Before that, or with the manifest missing, those take the system's
   ANSI code page, and a folder name it cannot spell (Japanese on a Western Windows, an emoji) comes back as "?": nothing opens and
   the game closed with no window and no log. The exe's own path through GetModuleFileNameA must name the exe (a name past MAX_PATH
   bytes fails the same way); when it does not, a message says why (none for the builder's --prepare-shaders run: its log has it). */
static int narrow_paths_ok(int argc, char **argv) {
    char a[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, a, MAX_PATH);
    if (n && n < MAX_PATH && GetFileAttributesA(a) != INVALID_FILE_ATTRIBUTES) return 1;
    wchar_t w[MAX_PATH * 2] = L"";
    GetModuleFileNameW(NULL, w, MAX_PATH * 2);
    fprintf(stderr, "hookshot: this folder's path cannot be opened (code page %u, %s): move the game to a folder with a plain name\n",
            GetACP(), n >= MAX_PATH ? "too long" : "has characters the code page lacks");
    int quiet = env_on("HOS_HEADLESS");
    for (int i = 1; i < argc; i++) quiet |= !strcmp(argv[i], "--prepare-shaders");
    if (!quiet) {
        wchar_t text[MAX_PATH * 2 + 640];
        _snwprintf(text, sizeof text / sizeof text[0],
                   L"Hookshot can't open its files in this folder:\n\n%ls\n\n"
                   L"%ls\n\nMove the game folder somewhere with a shorter name of plain letters and digits (for example C:\\Games\\Hookshot), "
                   L"or update Windows.",
                   w, GetACP() == CP_UTF8 ? L"The path is too long."
                                          : L"Its name has characters this Windows can only give the game from Windows 10 version 1903 on.");
        text[sizeof text / sizeof text[0] - 1] = 0;
        MessageBoxW(NULL, text, L"Hookshot", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    return 0;
}

int main(int argc, char **argv) {
#else
static int game_main(int argc, char **argv) {
#endif
#ifdef _WIN32
    if (!narrow_paths_ok(argc, argv)) return 1;
    {
        void hos_relaunch_keep_env(void);   /* (host/hos_relaunch.c: the environment before the file's lines, for a restart) */
        hos_relaunch_keep_env();
    }
    load_hookshot_env();
#endif
    occlusion_switch();
    {
        const char *protect_hearts = getenv("HOS_OOT_PROTECT_HEARTS");
        hos_oot_protect_hearts = protect_hearts && !strcmp(protect_hearts, "1");
    }
#ifdef _WIN32
    {   // (the whole process, not only the window's thread: the renderer asks the window's size from its own, and on a 4K screen at 150%
        // Windows answered such a thread 2560 x 1440: the picture was that size, in the corner of the window. Hookshot issues 15, 18, 20)
        typedef BOOL(WINAPI * SetAware)(void *);
        SetAware set = (SetAware)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (set && !env_on("HOS_DPI_UNAWARE")) set((void *)(intptr_t)-4);
    }
#endif
    {   // (started from its icon the game has nowhere to log to: hookshot_log.txt beside it, last run only, for players to send in)
#ifdef _WIN32
        {   /* (started again by the Insert menu's Restart now (host/hos_relaunch.c): the run before still has its log open until
               it exits, so wait for it (up to 10 s) before the log moves to .1, or the move fails and its log is overwritten) */
            const char *of = getenv("HOS_RESTART_OF");
            if (of && *of) {
                HANDLE before = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(of, NULL, 10));
                if (before) WaitForSingleObject(before, 10000), CloseHandle(before);
                SetEnvironmentVariableA("HOS_RESTART_OF", NULL);   /* (not passed on: a later restart sets its own) */
                _putenv("HOS_RESTART_OF=");
            }
        }
#endif
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (!err || err == INVALID_HANDLE_VALUE || GetFileType(err) == FILE_TYPE_UNKNOWN) {
            char path[MAX_PATH + 32];
            DWORD n = hos_game_path_a(path, MAX_PATH);
            while (n && path[n - 1] != 92 && path[n - 1] != 47) n--;
            /* (the four runs before stay as hookshot_log.1.txt (the last) to .4.txt: a player who started the game again after a
               crash sent in the new run's log, and the crash was gone) */
            char from[MAX_PATH + 32], to[MAX_PATH + 32];
            for (int k = 4; k >= 1; k--) {
                if (k == 1) memcpy(from + n, "hookshot_log.txt", 17), memcpy(from, path, n);
                else memcpy(from, path, n), snprintf(from + n, 32, "hookshot_log.%d.txt", k - 1);
                memcpy(to, path, n), snprintf(to + n, 32, "hookshot_log.%d.txt", k);
                MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
            }
            memcpy(path + n, "hookshot_log.txt", 17);
#if !defined(_WIN32) && !defined(A64_MEM_ZERO)
            startup_stderr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
#endif
            if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    hos_log("hookshot %s", HOOKSHOT_VERSION);   /* (the log's first line: which build a report comes from) */
    removed_switch_notes();
    void hos_control_start(const char *version);
    hos_control_start(HOOKSHOT_VERSION);   /* (HOS_CONTROL=<socket>: the control bridge, host/control.c) */
    const char *image = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--romfs") && i + 1 < argc) hos_romfs_dir = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) hos_user_dir = argv[++i];
        else if (!strcmp(argv[i], "--trace")) hos_trace = 1;
        else if (!strcmp(argv[i], "--prepare-shaders")) {   /* (hookshot) a build's last step: no game */
#ifdef _WIN32
            _putenv("HOOKSHOT_PREPARE_ONLY=1");
#else
            setenv("HOOKSHOT_PREPARE_ONLY", "1", 1);
#endif
            int rx_prepare_shaders(void);
            if (rx_prepare_shaders()) {
                printf("@@done shaders\n");
                return 0;
            }
            printf("@@error no usable Vulkan here: the game prepares its shaders at its first start\n");
            return 3;
        }
        else if (!image) image = argv[i];
    }
#ifdef _WIN32
    {   /* (hookshot) the Discord status (discord_rpc.c), a game run only (a --prepare-shaders run returned above). Linux starts
           it in its own main, below; on Windows it was never started: no status and no "discord:" line in any log */
        void hos_discord_start(void);
        hos_discord_start();
    }
#endif
    static char beside_image[MAX_PATH], beside_romfs[MAX_PATH], beside_user[MAX_PATH];
    if (!image) {   /* (hookshot) started with nothing, as a game is: everything beside the executable (the builder's folder) */
        char dir[MAX_PATH];
        const DWORD len = hos_game_path_a(dir, MAX_PATH);
        char *slash = len ? strrchr(dir, SEP) : NULL;
        if (slash) {
            *slash = 0;
            SetCurrentDirectoryA(dir);
            snprintf(beside_image, MAX_PATH, "%s" SEP_S "image.bin", dir);
            snprintf(beside_romfs, MAX_PATH, "%s" SEP_S "romfs", dir);
            snprintf(beside_user, MAX_PATH, "%s" SEP_S "user", dir);
            if (GetFileAttributesA(beside_image) != INVALID_FILE_ATTRIBUTES) {
                image = beside_image;
                if (!hos_romfs_dir) hos_romfs_dir = beside_romfs;
#ifndef _WIN32
                if (!hos_user_dir) hos_user_dir = saves_dir(beside_user);
#else
                if (!hos_user_dir) hos_user_dir = beside_user;
#endif
                /* (a game folder from the builder: is a newer release out? host/hos_update.c. After the saves' place is known:
                   on Linux the check keeps its "Skip this version" there) */
                hos_update_check();
            }
        }
    }
    if (!image) {
        fprintf(stderr, "usage: %s [image.bin] [--romfs <dir>] [--user <dir>] [--trace] [--prepare-shaders]\n",
                argv[0]);
        return 2;
    }
    if (env_on("HOS_TRACE")) hos_trace = 1;
    if (!hos_user_dir) hos_user_dir = "user";
    CreateDirectoryA(hos_user_dir, NULL);
    if (!reserve_guest()) {
        fprintf(stderr, "cannot reserve the guest window\n");
        return 1;
    }
#ifdef A64_MEM_ZERO
    web_fs_init(NULL, hos_romfs_dir);
    /* (the whole file, no range: the server sends it brotli-compressed, 85 -> 26 MB) */
    const int got = web_fetch_all(image, A64_MEM + IMAGE_BASE, GUEST_SPACE - IMAGE_BASE);
    if (got < 0) {
        fprintf(stderr, "cannot fetch %s\n", image);
        return 1;
    }
    size_t n = (size_t)got;
#else
    FILE *f = fopen(image, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", image);
        return 1;
    }
    /* exactly the file's size: a read into "the rest of the guest window" (~4 GB) made Windows commit physical memory
       for ~2 GB of it before the game ran (the whole heap was resident from the start, ~1.3 GB of it never used) */
    fseek(f, 0, SEEK_END);
    const long long file_size = _ftelli64(f);
    fseek(f, 0, SEEK_SET);
    if (file_size > 0) hos_commit(IMAGE_BASE, (uint64_t)file_size);
    size_t n = file_size > 0 && (uint64_t)file_size <= GUEST_SPACE - IMAGE_BASE
                   ? fread(A64_MEM + IMAGE_BASE, 1, (size_t)file_size, f) : 0;
    fclose(f);
#endif
    if (IMAGE_BASE + n != hos_image_end) {
        fprintf(stderr, "%s: %zu bytes, the lifted code expects %#llx: lift and image do not match\n", image, n,
                (unsigned long long)(hos_image_end - IMAGE_BASE));
        return 1;
    }
    hos_mem_init(hos_image_end);
    {   /* (hookshot) Widescreen and ultrawide (nx2/oot_wide.py): HOS_ASPECT=<ratio> (2.3889 for 3440x1440), else the Insert menu's
           Aspect ratio (on unless it says 0) takes the main monitor's shape. The game's picture takes that shape at its own height (the
           world wider at the sides on an ultrawide screen, narrower on a 16:10 or 4:3 one), its 16:9 cameras the picture's ratio.
           An output that is not 16:9 takes precedence over both HOS_ASPECT and the Aspect ratio, matching the menu's picture. Wider
           than 16:9 the HUD, menus and movies keep the middle 16:9; narrower they fill the picture's width. Until now the one 16:9
           constant in the game's data was changed and the 16:9 picture stretched to the window. */
        const char *e = getenv("HOS_ASPECT");
        float want = e ? (float)atof(e) : 0.0f;
        if (!e) {
            int saved = -1, version = 0;   /* (the Aspect ratio's ultrawide= and options_version=, -1 / 0 without them) */
            char path[MAX_PATH];
            const DWORD len = hos_game_path_a(path, MAX_PATH);
            char *slash = len ? strrchr(path, SEP) : NULL;
            if (slash) {
                snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "hookshot_options.ini");
                FILE *o = fopen(path, "r");
                if (o) {
                    char line[80];
                    while (fgets(line, sizeof line, o)) {
                        if (!strncmp(line, "ultrawide=", 10)) saved = atoi(line + 10) != 0;
                        if (!strncmp(line, "options_version=", 16)) version = atoi(line + 16);
                    }
                    fclose(o);
                }
            }
            const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
            /* (no line: the monitor's shape; an older file's 16:9 on a screen narrower than 16:9 too: hos_aspect_on) */
            const int on = hos_aspect_on(saved, version, sw, sh);
            if (on && sh > 0) want = (float)sw / (float)sh;
        }
        extern volatile uint64_t hos_wide_out, hos_wide_scene;
        extern volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
        uint32_t hos_output_size(int height);
        const uint32_t output_w = hos_output_size(0), output_h = hos_output_size(1);
        /* An output of a shape of its own (the Insert menu's Aspect ratio: 21:9, 16:10, 4:3, a 32:9 surround...) asks for that
           shape, over HOS_ASPECT and Ultrawide, even if the OS reports one monitor (host/hos_wide_size.h) */
        if (hos_output_own_shape(output_w, output_h)) want = (float)output_w / (float)output_h;
        const double game = 16.0 / 9.0;
        /* (a screen narrower than 16:9 too, 16:10 to 5:4: the picture keeps the game's height and is narrower, the
           cameras take its ratio, and the HUD fills its width instead of a middle 16:9 wider than the screen. A 4:3 CRT
           at the 1440 output: picture 1920 x 1440, scene 1440 x 1080, HUD 100%) */
        if (want > game * 1.01 || (want > 1.0f && want < game * 0.99)) {
            const double r = hos_wide_clamp(want);
            /* (the height kept, the width at the screen's ratio, at most hos_wide_max_px() pixels (5120 x 2160's, a 21:9 screen's
               native at the 4K output) with the shape kept: host/hos_wide_size.h, which the Insert menu's Output and Render
               resolution rows share; issue 1031) */
            unsigned ow, oh;
            hos_wide_size(output_h, r, hos_wide_max_px(), &ow, &oh);
            const unsigned sh = oh * 3 / 4, sw = (unsigned)(ow * 0.75 / 8 + 0.5) * 8;   /* (the scene, 3/4 of the picture as shipped) */
            hos_wide_out = (uint64_t)oh << 32 | ow;
            hos_wide_scene = (uint64_t)sh << 32 | sw;
            hos_wide_ratio = (float)ow / (float)oh;
            hos_wide_squeeze = (float)fmin(1.0, game / ((double)ow / oh));   /* (a narrower screen: the whole width) */
            hos_wide_margin = (1.0f - hos_wide_squeeze) / 2.0f;
            hos_aspect_ratio = hos_wide_ratio;
            hos_log("boot: widescreen: picture %ux%u, scene %ux%u, camera ratio %.4f, HUD and movies in the middle %.1f%%", ow, oh, sw, sh,
                    (double)hos_wide_ratio, hos_wide_squeeze * 100.0);
        } else if (want > 1.0f) {
            hos_log("boot: widescreen: a %.4f screen is 16:9: the game's own picture", (double)want);
        }
    }
    hos_log("boot: image %zu MB at %#llx, %llu MB of memory for the game", n >> 20, IMAGE_BASE,
            (unsigned long long)(hos_total_memory >> 20));
#ifndef A64_MEM_ZERO
    memory_notice();
#endif
    void hos_mods_init(const char *image_path);
    hos_mods_init(image);   /* mods.c: <exe folder>\mods (RomFS files) */

    Ctx *c = hos_boot_main_thread(hos_main_stack_size);
    uint64_t sp = c->x[31];
    void hos_prof_start(void);
    hos_prof_start();   /* HOS_PROFILE=<seconds> */
    void hos_chain_start(void);
    hos_chain_start();  /* HOS_CHAIN=<path> */
#ifdef _WIN32
    timeBeginPeriod(1);   /* 1 ms timer resolution: the 60 Hz vsync and the game's frame pacing sleep in short steps */
    no_power_throttling();
#endif
    void hos_display_start(void);
    hos_display_start();  /* the game's window; HOS_HEADLESS=1 for none */
    /* nn::init::Start's order (sdk+0xfa060), with rtld's three callbacks done here: f1 only marks rtld initialized,
       f2 is every module's DT_INIT, f3 the DT_FINIs (not needed: we never return from nnMain to unload). */
    call_guest(c, hos_entry_init_libc0, sp, "_init_libc0");
    c->x[0] = hos_thread_handle(hos_self());   /* the main thread's handle */
    c->x[1] = 0;                               /* program arguments: none (an unmapped address) */
    call_guest_args(c, hos_entry_nnosInitialize, sp, "nnosInitialize");
    void hos_thread_adopt_guest(HosThread *t);
    hos_thread_adopt_guest(hos_self());
    call_guest(c, hos_entry_init_libc1, sp, "_init_libc1");
    /* nninitInitializeSdkModule (sdk+0xfa000) only opens the SDK's service clients (sm:, diag, oe, aoc, err, fs,
       pctl) over IPC; the HLE stands in for every one of them, so it is not run. */
    call_guest(c, hos_entry_nninitStartup, sp, "nninitStartup");
    call_guest(c, hos_entry_init_libc2, sp, "_init_libc2");
    for (int i = 0; i < hos_module_count; i++) {
        char what[64];
        snprintf(what, sizeof what, "%s DT_INIT", hos_module_name[i]);
        call_guest(c, hos_module_init[i], sp, what);
    }
    call_guest(c, hos_entry_nnMain, sp, "nnMain");
    hos_log("nnMain returned");
    return 0;
}

#ifndef _WIN32
/* (linux) The window belongs on the process's main thread (SDL), so the game runs on a thread of its own, with the
   stack the guest main thread's lifted code needs (the main thread has the system's 8 MB): host/posix/display_sdl.c */
static int game_argc;
static char **game_argv;
static DWORD WINAPI game_thread(void *p) {
    (void)p;
    ExitProcess((UINT)game_main(game_argc, game_argv));
}
/* (hookshot, linux) Room in the drivers' own shader caches for what the preparation builds. The pipelines the notes predict (~43,000
   in OoT, vk_draw.cpp predict_run) are built into the driver's disk cache alone, not into mkw_vk_pipelines.bin, which is read in whole
   at every start. NVIDIA's cache holds 1 GiB by default and cleans itself down to that as it is written: a full preparation filled
   it to the limit (1026 MB) and the pipelines it dropped were compiled again in play. Mesa's (RADV, ANV) has the same 1 GiB default.
   Raised for this process only, before anything loads Vulkan; a value the player set (or hookshot_env.txt) is kept. Windows: the
   NVIDIA driver takes its size from the control panel's Shader Cache Size, which a program cannot set (readme) */
static void driver_cache_room(void) {
    setenv("__GL_SHADER_DISK_CACHE_SIZE", "10737418240", 0);   /* (10 GiB, bytes) */
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "10G", 0);
}
#include <errno.h>
#include <sys/stat.h>
/* (hookshot, linux) NVIDIA's driver keeps no shader cache at all when __GL_SHADER_DISK_CACHE_PATH names a folder that does not exist:
   it neither creates it nor falls back to ~/.cache/nvidia, and every start compiles every pipeline again (a test of 300 pipelines:
   7.9 ms each at the first start and at the second, against 0.06 ms from the cache; driver 620.70). The folder a player or a
   launcher named is made here, before anything loads Vulkan. Mesa makes its own (MESA_SHADER_CACHE_DIR, parents too). Not needed:
   NVIDIA files the cache by the program's file name alone (bin/hookshot), not its folder, bytes or date, nor VkApplicationInfo, so
   an update in place, a folder moved or copied and every Hookshot folder on the PC share one cache (a cache of another driver or
   GPU is the driver's to drop) */
static void driver_cache_folder(void) {
    const char *dir = getenv("__GL_SHADER_DISK_CACHE_PATH");
    if (!dir || !*dir || strlen(dir) >= 4096) return;
    char path[4096];
    strcpy(path, dir);
    for (char *p = path + 1;; p++) {   /* (each parent, then the folder itself: mkdir -p) */
        const char c = *p;
        if (c != '/' && c) continue;
        *p = 0;
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "vk: __GL_SHADER_DISK_CACHE_PATH %s: cannot make %s (%s): NVIDIA's driver keeps no shader cache\n", dir,
                    path, strerror(errno));
            return;
        }
        if (!c) {   /* (a file already at that name counts as made for mkdir, but the driver can't use it) */
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
                fprintf(stderr, "vk: __GL_SHADER_DISK_CACHE_PATH %s: cannot make it, a file has that name: NVIDIA's driver keeps no shader cache\n", dir);
            return;
        }
        *p = '/';
    }
}
int main(int argc, char **argv) {
    int hos_display_init(void);
    void hos_display_loop(void);
    load_hookshot_env();
    driver_cache_room();
    driver_cache_folder();
    game_argc = argc, game_argv = argv;
    int prepare_shaders = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare-shaders")) prepare_shaders = 1;
        else if (!strcmp(argv[i], "--romfs") || !strcmp(argv[i], "--user")) i++;
    }
    /* The builder's preparation worker needs Vulkan, but no SDL display. */
    const int window = prepare_shaders ? 0 : hos_display_init();
    if (window < 0) {
        hos_log_flush();
        return 1;
    }
    if (!prepare_shaders) {   /* (hookshot) the Discord status (discord_rpc.c): only a game run */
        void hos_discord_start(void);
        hos_discord_start();
    }
    HANDLE t = CreateThread(NULL, 64u << 20, game_thread, NULL, 0, NULL);
    if (!t) {
        fprintf(stderr, "cannot start the game's thread\n");
        return 1;
    }
    SetThreadDescription(t, L"Game");
    if (window) hos_display_loop();   /* (does not return: the window's events until the process ends) */
    WaitForSingleObject(t, INFINITE);
    return 0;
}
#endif
