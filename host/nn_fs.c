/* nn::fs: mounts map to host folders.
     rom:      the game's RomFS (extracted by tools/nx.py romfs), read-only
     save:     <user>/save/<uid>     (MountSaveData / MountSaveDataReadOnly)
     cache:    <user>/cache/<index>  (MountCacheStorage)
     temp:     <user>/temp           (MountTemporaryStorage)
   hos_romfs_dir and hos_user_dir are set by hos_main.c. Reads are positional (ReadFile with an offset), so any number
   of guest threads can read one file at once. Results: switchbrew "Error codes" (module 2 = fs). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

/* (hookshot, #1468) A game folder whose deepest file passes 260 characters (MAX_PATH: "E:\Games\...\romfs\Event\EventSetting\
   Ev..." at 262) is read too. The host paths here are up to HOS_FS_PATH bytes, and on Windows each file call goes through
   its wide function: the path converted from the game's UTF-8 strings and, when it is
   long and absolute, given the extended-length prefix \\?\ (with its / made \), which Windows takes without its long-paths
   setting. Short paths are as they were. */
#define HOS_FS_PATH 1024
#ifdef _WIN32
static const wchar_t *fs_wide(const char *p, wchar_t *w, size_t cap) {
    const size_t n = strlen(p);
    const int already = p[0] == '\\' && p[1] == '\\' && p[2] == '?';
    const int unc = !already && p[0] == '\\' && p[1] == '\\';
    const int drive = p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
    const int extended = !already && n >= 240 && (drive || unc);
    size_t at = 0;
    if (extended) {
        wcscpy(w, unc ? L"\\\\?\\UNC" : L"\\\\?\\");
        at = wcslen(w);
        if (unc) p++;   /* (\\server\share -> \\?\UNC\server\share) */
    }
    if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w + at, (int)(cap - at)) <= 0) w[0] = 0;
    if (extended)
        for (wchar_t *q = w; *q; q++)
            if (*q == L'/') *q = L'\\';
    return w;
}
#define FS_W(p) fs_wide((p), (wchar_t[HOS_FS_PATH + 8]){0}, HOS_FS_PATH + 8)
static void fs_find_data(const WIN32_FIND_DATAW *w, WIN32_FIND_DATAA *a) {
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime = w->ftCreationTime, a->ftLastAccessTime = w->ftLastAccessTime, a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh, a->nFileSizeLow = w->nFileSizeLow;
    a->dwReserved0 = w->dwReserved0, a->dwReserved1 = w->dwReserved1;
    if (WideCharToMultiByte(CP_UTF8, 0, w->cFileName, -1, a->cFileName, (int)sizeof a->cFileName, NULL, NULL) <= 0) a->cFileName[0] = 0;
    if (WideCharToMultiByte(CP_UTF8, 0, w->cAlternateFileName, -1, a->cAlternateFileName, (int)sizeof a->cAlternateFileName, NULL,
                            NULL) <= 0)
        a->cAlternateFileName[0] = 0;
}
static HANDLE fs_CreateFile(const char *p, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD how, DWORD flags, HANDLE t) {
    return CreateFileW(FS_W(p), access, share, sa, how, flags, t);
}
static DWORD fs_GetFileAttributes(const char *p) { return GetFileAttributesW(FS_W(p)); }
static BOOL fs_CreateDirectory(const char *p, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryW(FS_W(p), sa); }
static BOOL fs_DeleteFile(const char *p) { return DeleteFileW(FS_W(p)); }
static BOOL fs_RemoveDirectory(const char *p) { return RemoveDirectoryW(FS_W(p)); }
static BOOL fs_MoveFileEx(const char *a, const char *b, DWORD flags) { return MoveFileExW(FS_W(a), FS_W(b), flags); }
static HANDLE fs_FindFirstFile(const char *p, WIN32_FIND_DATAA *fd) {
    WIN32_FIND_DATAW w;
    const HANDLE h = FindFirstFileW(FS_W(p), &w);
    if (h != INVALID_HANDLE_VALUE) fs_find_data(&w, fd);
    return h;
}
static BOOL fs_FindNextFile(HANDLE h, WIN32_FIND_DATAA *fd) {
    WIN32_FIND_DATAW w;
    if (!FindNextFileW(h, &w)) return FALSE;
    fs_find_data(&w, fd);
    return TRUE;
}
#define CreateFileA fs_CreateFile
#define GetFileAttributesA fs_GetFileAttributes
#define CreateDirectoryA fs_CreateDirectory
#define DeleteFileA fs_DeleteFile
#define RemoveDirectoryA fs_RemoveDirectory
#define MoveFileExA fs_MoveFileEx
#define FindFirstFileA fs_FindFirstFile
#define FindNextFileA fs_FindNextFile
#endif

const char *hos_romfs_dir, *hos_user_dir;

#define R_PATH_NOT_FOUND 0x202          /* fs::ResultPathNotFound */
#define R_PATH_EXISTS 0x402             /* fs::ResultPathAlreadyExists */
#define R_NOT_MOUNTED 0xCA02            /* fs::ResultInvalidMountName-ish: no such mount */
#define R_TARGET_NOT_FOUND 0x7D402      /* fs::ResultTargetNotFound (save data) */

#define FS_PATH_MAX 0x302                /* (nn::fs paths: 0x300 bytes and the NUL; DirectoryEntry's name is 0x301) */

typedef struct { char name[32]; char root[HOS_FS_PATH]; int writable; } Mount;
static Mount mounts[16];
static int nmounts;
static SRWLOCK mount_lock = SRWLOCK_INIT;

static int mount(const char *name, const char *root, int writable) {   /* 0: the table is full (16 mounts) */
    AcquireSRWLockExclusive(&mount_lock);
    int i;
    for (i = 0; i < nmounts && strcmp(mounts[i].name, name); i++) {}
    if (i == nmounts) {   /* (hookshot: a 17th name was written past the table) */
        if (nmounts == 16) {
            ReleaseSRWLockExclusive(&mount_lock);
            hos_log("fs: %s: not mounted (16 mounts already)", name);
            return 0;
        }
        nmounts++;
    }
    snprintf(mounts[i].name, sizeof mounts[i].name, "%s", name);
    snprintf(mounts[i].root, sizeof mounts[i].root, "%s", root);
    mounts[i].writable = writable;
    ReleaseSRWLockExclusive(&mount_lock);
    if (writable) CreateDirectoryA(root, NULL);
    hos_log("fs: mounted %s: -> %s", name, root);
    return 1;
}

int hos_mods_romfs(const char *rel, char *out, size_t cap);   /* mods.c */
int hos_mods_dir_extras(const char *rel, const char *base_dir, void (*fn)(void *, const char *, int, uint64_t), void *arg);

/* (hookshot) a guest path below its mount stays below it on the host: no "." or ".." part, no \\ or : (a drive or a
   Windows stream), no control characters; on Windows also no part ending in a dot or a space (Windows drops them) and
   no device name (CON, NUL, COM1, COM\u00b9, "NUL .txt"...) */
static int guest_path_ok(const char *rest) {
#ifdef _WIN32
    static const char *const devices[] = {"CON", "PRN", "AUX", "NUL", "COM", "LPT", "CONIN$", "CONOUT$", "CLOCK$"};
#endif
    for (const char *p = rest; *p;) {
        const char *e = strchr(p, '/');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 32 || p[i] == '\\' || p[i] == ':') return 0;
#ifdef _WIN32
        if (n && (p[n - 1] == '.' || p[n - 1] == ' ')) return 0;
        for (size_t d = 0; d < sizeof devices / sizeof *devices; d++) {
            const size_t k = strlen(devices[d]);
            if (n < k || _strnicmp(p, devices[d], k)) continue;
            const char *after = p + k;
            if (k == 3 && (!strcmp(devices[d], "COM") || !strcmp(devices[d], "LPT"))) {
                /* (a digit, or a superscript 1, 2 or 3 in UTF-8, the game's code page: Windows takes those too) */
                if (after < p + n && *after >= '0' && *after <= '9') after++;
                else if (after + 1 < p + n && (unsigned char)after[0] == 0xC2 &&
                         ((unsigned char)after[1] == 0xB9 || (unsigned char)after[1] == 0xB2 || (unsigned char)after[1] == 0xB3))
                    after += 2;
                else continue;
            }
            while (after < p + n && *after == ' ') after++;   /* ("NUL .txt" is NUL too) */
            if (after == p + n || *after == '.') return 0;
        }
#endif
        p += n;
        while (*p == '/') p++;
    }
    return 1;
}

/* "mount:/a/b" -> host path (a mod's file instead of the RomFS one, when a mod has it: mods.c). Returns 0 if the mount
   is unknown, or the path leaves it (guest_path_ok). */
static int host_path(const char *guest_mem, char *out, size_t cap, int *writable) {
    /* (hookshot) one copy of the path, read once: another guest thread may change guest memory between the check and
       the use */
    char guest[FS_PATH_MAX];
    if ((size_t)snprintf(guest, sizeof guest, "%s", guest_mem) >= sizeof guest) return 0;
    const char *colon = strchr(guest, ':');
    if (!colon) return 0;
    size_t n = (size_t)(colon - guest);
    int ok = 0;
    AcquireSRWLockShared(&mount_lock);
    for (int i = 0; i < nmounts; i++) {
        if (strlen(mounts[i].name) == n && !strncmp(mounts[i].name, guest, n)) {
            const char *rest = colon + 1;
            while (*rest == '/') rest++;
            if (!guest_path_ok(rest)) break;
            if (mounts[i].writable || !hos_mods_romfs(rest, out, cap))
                if ((size_t)snprintf(out, cap, "%s/%s", mounts[i].root, rest) >= cap) break;   /* (cut short: not this path) */
            if (writable) *writable = mounts[i].writable;
            ok = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&mount_lock);
    for (char *p = out; ok && *p; p++) if (*p == '/') *p = '\\';
    return ok;
}

/* For other OS layer parts that open game files themselves (videos): "mount:/a/b" -> host path, 0 if unknown. */
int hos_fs_host_path(const char *guest, char *out, size_t cap) { return host_path(guest, out, cap, NULL); }

/* (hookshot) a guest string: "" unless it lies in guest memory and ends within FS_PATH_MAX bytes */
static const char *gstr(uint64_t a) {
    if (!a || a >= (uint64_t)HOS_SPACE_END) return "";
    const uint64_t room = (uint64_t)HOS_SPACE_END - a;
    const char *s = (const char *)GP(a);
    const size_t max = room < FS_PATH_MAX ? (size_t)room : FS_PATH_MAX;
    return strnlen(s, max) < max ? s : "";
}

static void make_dirs(const char *path) {   /* every parent of path */
    char tmp[HOS_FS_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++)
        if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
}

/* ---- handles: a FileHandle / DirectoryHandle is one pointer-sized value; ours are table indices + 1 ----------- */
typedef struct { char name[HOS_FS_PATH]; int is_dir; uint64_t size; } ExtraEntry;   /* (a listed folder's mod-only entries) */
typedef struct {
    HANDLE h; int writable; HANDLE find; WIN32_FIND_DATAA fd; int find_done, is_dir; char path[HOS_FS_PATH];
    ExtraEntry *extras; int nextras, extra_pos;
} Handle;
static Handle *handles[4096];
static SRWLOCK handle_lock = SRWLOCK_INIT;

static uint64_t new_handle(Handle *h) {
    AcquireSRWLockExclusive(&handle_lock);
    for (int i = 0; i < 4096; i++)
        if (!handles[i]) { handles[i] = h; ReleaseSRWLockExclusive(&handle_lock); return (uint64_t)i + 1; }
    ReleaseSRWLockExclusive(&handle_lock);
    hos_fatal(NULL, "fs: out of handles");
    return 0;
}
static Handle *get_handle(Ctx *c, uint64_t id) {
    Handle *h = id && id <= 4096 ? handles[id - 1] : NULL;
    if (!h) hos_fatal(c, "fs: bad handle %#llx", (unsigned long long)id);
    return h;
}
static void free_handle(uint64_t id) {
    AcquireSRWLockExclusive(&handle_lock);
    if (id && id <= 4096) handles[id - 1] = NULL;
    ReleaseSRWLockExclusive(&handle_lock);
}

/* ---- mounts --------------------------------------------------------------------------------------------------- */
HLE(_ZN2nn2fs22QueryMountRomCacheSizeEPm) { G64(ARG(0)) = 0x10000; RET(0); }
HLE(_ZN2nn2fs8MountRomEPKcPvm) {
    if (!hos_romfs_dir) hos_fatal(c, "fs: MountRom but no RomFS folder given (--romfs)");
    RET(mount(gstr(ARG(0)), hos_romfs_dir, 0) ? 0 : R_NOT_MOUNTED);
}
/* (hookshot) MountRom(char const* name): the cacheless overload OoT uses (SDK 22). Left to the SDK it opens the
   file-system proxy over IPC and aborts on the empty reply. */
HLE(_ZN2nn2fs8MountRomEPKc) {
    if (!hos_romfs_dir) hos_fatal(c, "fs: MountRom but no RomFS folder given (--romfs)");
    RET(mount(gstr(ARG(0)), hos_romfs_dir, 0) ? 0 : R_NOT_MOUNTED);
}

/* (hookshot) the folder the game's saves are mounted from, for the control bridge's saves (host/control.c) */
static char save_root[HOS_FS_PATH];
int hos_fs_save_root(char *out, size_t cap) {
    AcquireSRWLockShared(&mount_lock);
    const int have = save_root[0] != 0;
    if (have) snprintf(out, cap, "%s", save_root);
    ReleaseSRWLockShared(&mount_lock);
    return have;
}

static int user_mount(const char *name, const char *sub) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s\\%s", hos_user_dir, sub);
    make_dirs(root);
    CreateDirectoryA(root, NULL);
    const int ok = mount(name, root, 1);
    if (ok && !strncmp(sub, "save\\", 5)) {
        AcquireSRWLockExclusive(&mount_lock);
        snprintf(save_root, sizeof save_root, "%s", root);
        ReleaseSRWLockExclusive(&mount_lock);
    }
    return ok;
}

/* nn::fs::IsSaveDataExisting(ApplicationId, account::Uid const&) -> bool */
HLE(_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s\\save\\%016llx%016llx", hos_user_dir, (unsigned long long)G64(ARG(1) + 8),
             (unsigned long long)G64(ARG(1)));
    RET(GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES);
}
/* MountSaveData(char const*, account::Uid const&) */
HLE(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE) {
    char sub[64];
    snprintf(sub, sizeof sub, "save\\%016llx%016llx", (unsigned long long)G64(ARG(1) + 8), (unsigned long long)G64(ARG(1)));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
/* MountSaveDataReadOnly(char const*, ApplicationId, account::Uid const&) */
HLE(_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE) {
    char sub[64];
    snprintf(sub, sizeof sub, "save\\%016llx%016llx", (unsigned long long)G64(ARG(2) + 8), (unsigned long long)G64(ARG(2)));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
HLE(_ZN2nn2fs21MountTemporaryStorageEPKc) { RET(user_mount(gstr(ARG(0)), "temp") ? 0 : R_NOT_MOUNTED); }
/* MountCacheStorage(char const*, int index) */
HLE(_ZN2nn2fs17MountCacheStorageEPKci) {
    char sub[32];
    snprintf(sub, sizeof sub, "cache\\%d", (int)ARG(1));
    RET(user_mount(gstr(ARG(0)), sub) ? 0 : R_NOT_MOUNTED);
}
/* CreateCacheStorage(int index, long size, long journal) */
HLE(_ZN2nn2fs18CreateCacheStorageEill) {
    char root[HOS_FS_PATH];
    snprintf(root, sizeof root, "%s\\cache\\%d\\", hos_user_dir, (int)ARG(0));
    make_dirs(root);
    RET(0);
}
HLE(_ZN2nn2fs18DeleteCacheStorageEi) { RET(0); }
/* The cache-storage list: report the indices that exist on disk. Handle: one value; info: {s32 index, pad[0x1C]} */
static int cache_list_pos;
HLE(_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE) { G64(ARG(0)) = 1; cache_list_pos = 0; RET(0); }
HLE(_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi) {
    int n = 0, max = (int)ARG(3);
    for (; cache_list_pos < 16 && n < max; cache_list_pos++) {
        char root[HOS_FS_PATH];
        snprintf(root, sizeof root, "%s\\cache\\%d", hos_user_dir, cache_list_pos);
        if (GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES) {
            memset(GP(ARG(1) + 0x20 * n), 0, 0x20);
            G32(ARG(1) + 0x20 * n) = (uint32_t)cache_list_pos;
            n++;
        }
    }
    G32(ARG(0)) = (uint32_t)n;
    RET(0);
}
HLE(_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE) {}
HLE(_ZN2nn2fs7UnmountEPKc) {
    AcquireSRWLockExclusive(&mount_lock);
    for (int i = 0; i < nmounts; i++)
        if (!strcmp(mounts[i].name, gstr(ARG(0)))) { mounts[i] = mounts[--nmounts]; break; }
    ReleaseSRWLockExclusive(&mount_lock);
}
HLE(_ZN2nn2fs6CommitEPKc) { RET(0); }
HLE(_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE) {}

/* ---- files ---------------------------------------------------------------------------------------------------- */
int hos_blank_cached_icon(const char *guest, const char *host);   /* (totk_icon_cache.c: TOTK's fused weapon icons) */
/* HOS_FS_TRACE=1: every file opened (or not found) is logged */
static int fs_trace(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_FS_TRACE");
    return on;
}
/* OpenFile(FileHandle*, char const* path, int mode): mode bit 0 read, bit 1 write, bit 2 append */
HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {
    char path[HOS_FS_PATH];
    int writable = 0;
    const char *g = gstr(ARG(1));
    /* (mkw-recomp 16bc3bc: the host's file calls block, so the virtual core goes to another thread meanwhile, as
       Horizon would run another while this one waits for the file system. OoT's ARes loaders held it up to 250 ms.)
       (hookshot, #1929) From before host_path: the first open of a button-prompt file builds it there (prompts.c,
       251-357 ms on Linux), and other threads that open it meanwhile wait in there for the build; with the core held
       through that, four threads waiting for the core ran without one within 50 ms */
    const int away = hos_vcore_away();
    if (!host_path(g, path, sizeof path, &writable)) {
        if (away) hos_vcore_back();
        hos_log("fs: OpenFile(%s): unknown mount", g);
        RET(R_PATH_NOT_FOUND);
        return;
    }
    int want_write = (ARG(2) & 6) != 0;
    if (want_write && !writable) { if (away) hos_vcore_back(); RET(R_PATH_NOT_FOUND); return; }
    /* (mkw-recomp) HOS_NO_MOVIES=1: MKW's WebM movies (cup openings, billboards, emblems) are not found, and the
       game goes on without them (decoded by nn_video.c otherwise) */
    if (!want_write && env_on("HOS_NO_MOVIES")) {
        const size_t n = strlen(g);
        if (n > 5 && !_stricmp(g + n - 5, ".webm")) {
            static int logged;
            if (!logged++) hos_log("fs: OpenFile(%s): movies are left out (HOS_NO_MOVIES)", g);
            if (away) hos_vcore_back();
            RET(R_PATH_NOT_FOUND);
            return;
        }
    }
    { int hos_ultrawide_redirect(const char *guest, char *out, size_t cap);   /* (totk_ultrawide_ui.c: the HUD) */
      char alt[HOS_FS_PATH];
      if (!want_write && hos_ultrawide_redirect(g, alt, sizeof alt)) snprintf(path, sizeof path, "%s", alt); }
    if (!want_write && hos_blank_cached_icon(g, path)) { if (away) hos_vcore_back(); RET(R_PATH_NOT_FOUND); return; }
    {   /* HOS_FS_OVERLAY=<folder> (a test): a file of the game's data found there is read in place of the game's own */
        /* (read once: every OpenFile asked the environment; one atomic pointer, "" until read, so a thread sees the
           value or reads it itself) */
        static const char *overlay = "";
        const char *ov = __atomic_load_n(&overlay, __ATOMIC_RELAXED);
        if (ov && !*ov) {
            ov = getenv("HOS_FS_OVERLAY");
            if (ov && !*ov) ov = NULL;   /* (set but empty: no overlay, as before) */
            __atomic_store_n(&overlay, ov, __ATOMIC_RELAXED);
        }
        const char *rel = !want_write && ov ? strstr(g, ":/") : NULL;
        if (rel) {
            char alt[HOS_FS_PATH];
            snprintf(alt, sizeof alt, "%s/%s", ov, rel + 2);
            if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) {
                hos_log("fs: OpenFile(%s): from the overlay", g);
                snprintf(path, sizeof path, "%s", alt);
            }
        }
    }
    HANDLE h = CreateFileA(path, GENERIC_READ | (want_write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (away) hos_vcore_back();
    if (h == INVALID_HANDLE_VALUE) {
        if (hos_trace || fs_trace()) hos_log("fs: OpenFile(%s): not found", g);
        RET(R_PATH_NOT_FOUND);
        return;
    }
    Handle *fh = (Handle *)calloc(1, sizeof *fh);
    fh->h = h;
    fh->writable = want_write;
    snprintf(fh->path, sizeof fh->path, "%s", g);
    G64(ARG(0)) = new_handle(fh);
    if (hos_trace || fs_trace()) hos_log("fs: OpenFile(%s)", g);
    RET(0);
}

static uint64_t read_at(Ctx *c, uint64_t id, int64_t off, uint64_t buf, uint64_t size) {
    Handle *fh = get_handle(c, id);
    uint64_t done = 0;
    if (!hos_guest_range(buf, size)) return 0;   /* (hookshot: the file's bytes go into guest memory only) */
    static int64_t delay_ns = -1, delay_after_ns;   /* HOS_FS_DELAY_MS=<ms>[,<after s>] (experiment): slower reads */
    if (delay_ns < 0) {
        const char *e = getenv("HOS_FS_DELAY_MS");
        delay_ns = e ? (int64_t)(atof(e) * 1e6) : 0;
        const char *a = e ? strchr(e, ',') : NULL;
        delay_after_ns = hos_now_ns() + (a ? (int64_t)(atof(a + 1) * 1e9) : 0);
    }
    if (delay_ns && hos_now_ns() >= delay_after_ns) {
        if (fs_trace()) hos_log("fs: read %s at %lld, %llu bytes", fh->path, (long long)off, (unsigned long long)size);
        hos_sleep_ns(delay_ns);
    }
    const int away = hos_vcore_away();   /* (as OpenFile: no core held through the read) */
    while (done < size) {
        OVERLAPPED ov = {0};
        int64_t o = off + (int64_t)done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = (DWORD)(size - done > 0x40000000 ? 0x40000000 : size - done), got = 0;
        if (!ReadFile(fh->h, GP(buf + done), chunk, &got, &ov) || !got) break;
        done += got;
    }
    if (away) hos_vcore_back();
    HOS_WRITTEN(GP(buf), (size_t)done);   /* (into guest memory: the GPU may read it) */
    {   /* HOS_FS_WATCH=<lo hex>:<hi hex> (debugging): reads into that guest range, with the file */
        static int on = -1;
        static unsigned long long wlo, whi;
        if (on < 0) on = getenv("HOS_FS_WATCH") && sscanf(getenv("HOS_FS_WATCH"), "%llx:%llx", &wlo, &whi) == 2;
        if (on && buf < whi && buf + done > wlo)
            hos_log("fs: read into watched memory: %s at %lld, %llu bytes to %#llx", fh->path, (long long)off,
                    (unsigned long long)done, (unsigned long long)buf);
    }
    return done;
}

/* ReadFile(FileHandle, long offset, void* buf, unsigned long size) -> Result (must read it all) */
HLE(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm) {
    uint64_t n = read_at(c, ARG(0), (int64_t)ARG(1), ARG(2), ARG(3));
    RET(n == ARG(3) ? 0 : 0x2EE202);   /* fs::ResultOutOfRange */
}
/* ReadFile(unsigned long* read, FileHandle, long offset, void* buf, unsigned long size[, ReadOption const&]) */
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm) {
    G64(ARG(0)) = read_at(c, ARG(1), (int64_t)ARG(2), ARG(3), ARG(4));
    RET(0);
}
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE) {
    G64(ARG(0)) = read_at(c, ARG(1), (int64_t)ARG(2), ARG(3), ARG(4));
    RET(0);
}
/* GetFileSize(long*, FileHandle) */
HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE) {
    LARGE_INTEGER sz = {0};   /* (hookshot: a folder's handle has no size: 0, not the host stack's bytes) */
    GetFileSizeEx(get_handle(c, ARG(1))->h, &sz);
    G64(ARG(0)) = (uint64_t)sz.QuadPart;
    RET(0);
}
/* WriteFile(FileHandle, long offset, void const* buf, unsigned long size, WriteOption const&) */
HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE) {
    Handle *fh = get_handle(c, ARG(0));
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)ARG(1);
    ov.OffsetHigh = (DWORD)(ARG(1) >> 32);
    DWORD put = 0;
    /* (hookshot) from guest memory only, and the whole size: a DWORD cut wrote less than asked and said so */
    if (ARG(3) <= 0xFFFFFFFFu && hos_guest_range(ARG(2), ARG(3))) WriteFile(fh->h, GP(ARG(2)), (DWORD)ARG(3), &put, &ov);
    RET(put == ARG(3) ? 0 : 0x2EE202);
}
HLE(_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl) {
    Handle *fh = get_handle(c, ARG(0));
    LARGE_INTEGER sz;
    sz.QuadPart = (LONGLONG)ARG(1);
    SetFilePointerEx(fh->h, sz, NULL, FILE_BEGIN);
    SetEndOfFile(fh->h);
    RET(0);
}
HLE(_ZN2nn2fs9FlushFileENS0_10FileHandleE) { FlushFileBuffers(get_handle(c, ARG(0))->h); RET(0); }
HLE(_ZN2nn2fs9CloseFileENS0_10FileHandleE) {
    Handle *fh = get_handle(c, ARG(0));
    CloseHandle(fh->h);
    free_handle(ARG(0));
    free(fh);
}

/* CreateFile(char const*, long size) */
HLE(_ZN2nn2fs10CreateFileEPKcl) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable) { RET(R_PATH_NOT_FOUND); return; }
    make_dirs(path);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { RET(GetLastError() == ERROR_FILE_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND); return; }
    LARGE_INTEGER sz;
    sz.QuadPart = (LONGLONG)ARG(1);
    SetFilePointerEx(h, sz, NULL, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
    RET(0);
}
HLE(_ZN2nn2fs10DeleteFileEPKc) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable || !DeleteFileA(path)) { RET(R_PATH_NOT_FOUND); return; }
    RET(0);
}
HLE(_ZN2nn2fs15CreateDirectoryEPKc) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable) { RET(R_PATH_NOT_FOUND); return; }
    make_dirs(path);
    RET(CreateDirectoryA(path, NULL) ? 0 : GetLastError() == ERROR_ALREADY_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND);
}
static void delete_tree(const char *dir) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char sub[HOS_FS_PATH];
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            /* (hookshot) a link (a symbolic link, a junction) goes itself: never what it points to */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (!DeleteFileA(sub)) RemoveDirectoryA(sub); }
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(sub);
            else DeleteFileA(sub);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryA(dir);
}
HLE(_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc) {
    char path[HOS_FS_PATH];
    int writable = 0;
    if (!host_path(gstr(ARG(0)), path, sizeof path, &writable) || !writable) { RET(R_PATH_NOT_FOUND); return; }
    /* (hookshot) the folder named itself a link: the link goes, never what it points to (as for the links inside) */
    const DWORD at = GetFileAttributesA(path);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (!DeleteFileA(path)) RemoveDirectoryA(path);
        RET(0);
        return;
    }
    delete_tree(path);
    RET(0);
}
HLE(_ZN2nn2fs10RenameFileEPKcS2_) {
    char a[HOS_FS_PATH], b[HOS_FS_PATH];
    int w1 = 0, w2 = 0;
    if (!host_path(gstr(ARG(0)), a, sizeof a, &w1) || !host_path(gstr(ARG(1)), b, sizeof b, &w2) || !w1 || !w2) { RET(R_PATH_NOT_FOUND); return; }
    RET(MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : R_PATH_NOT_FOUND);
}
/* a RomFS folder that only mods have: rom:/x -> some mod entry lies under x */
static void count_extra(void *arg, const char *name, int is_dir, uint64_t size) { (void)name, (void)is_dir, (void)size; ++*(int *)arg; }
static const char *rom_rel(const char *guest) {   /* the path inside the RomFS mount, or NULL */
    const char *colon = strchr(guest, ':');
    if (!colon || !hos_romfs_dir) return NULL;
    char root[HOS_FS_PATH];
    if (!host_path(guest, root, sizeof root, NULL)) return NULL;
    AcquireSRWLockShared(&mount_lock);
    int rom = 0;
    for (int i = 0; i < nmounts; i++)
        if (strlen(mounts[i].name) == (size_t)(colon - guest) && !strncmp(mounts[i].name, guest, (size_t)(colon - guest)))
            rom = !mounts[i].writable && !strcmp(mounts[i].root, hos_romfs_dir);
    ReleaseSRWLockShared(&mount_lock);
    if (!rom) return NULL;
    const char *rest = colon + 1;
    while (*rest == '/') rest++;
    return rest;
}
static int is_mod_dir(const char *guest) {
    const char *rel = rom_rel(guest);
    int n = 0;
    if (rel) hos_mods_dir_extras(rel, "<", count_extra, &n);   /* ("<": no such folder: every entry counts) */
    return n > 0;
}

/* GetEntryType(DirectoryEntryType*, char const*): 0 directory, 1 file */
HLE(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc) {
    char path[HOS_FS_PATH];
    if (!host_path(gstr(ARG(1)), path, sizeof path, NULL)) { RET(R_PATH_NOT_FOUND); return; }
    hos_blank_cached_icon(gstr(ARG(1)), path);
    DWORD at = GetFileAttributesA(path);
    if (at == INVALID_FILE_ATTRIBUTES && is_mod_dir(gstr(ARG(1)))) at = FILE_ATTRIBUTE_DIRECTORY;   /* (only in mods) */
    if (fs_trace())
        hos_log("fs: GetEntryType(%s): %s", gstr(ARG(1)), at == INVALID_FILE_ATTRIBUTES ? "not found"
                : (at & FILE_ATTRIBUTE_DIRECTORY) ? "directory" : "file");
    if (at == INVALID_FILE_ATTRIBUTES) { RET(R_PATH_NOT_FOUND); return; }
    G32(ARG(0)) = (at & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
    RET(0);
}

/* ---- directories: DirectoryEntry = { char name[0x301]; pad[3]; s8 type at 0x304 (0 directory, 1 file); pad[3]; s64 size at 0x308 } ---- */
static void add_extra(void *arg, const char *name, int is_dir, uint64_t size) {
    Handle *dh = (Handle *)arg;
    if (!(dh->nextras & 15)) dh->extras = (ExtraEntry *)realloc(dh->extras, sizeof *dh->extras * (size_t)(dh->nextras + 16));
    ExtraEntry *e = &dh->extras[dh->nextras++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir, e->size = size;
}
HLE(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci) {
    char path[HOS_FS_PATH], pat[HOS_FS_PATH];
    if (!host_path(gstr(ARG(1)), path, sizeof path, NULL)) { RET(R_PATH_NOT_FOUND); return; }
    snprintf(pat, sizeof pat, "%s\\*", path);
    Handle *dh = (Handle *)calloc(1, sizeof *dh);
    dh->is_dir = (int)ARG(2);   /* mode: 1 directories, 2 files, 3 both */
    snprintf(dh->path, sizeof dh->path, "%s", pat);   /* (for GetDirectoryEntryCount) */
    dh->find = FindFirstFileA(pat, &dh->fd);
    if (dh->find == INVALID_HANDLE_VALUE) dh->find_done = 1;
    if (rom_rel(gstr(ARG(1)))) hos_mods_dir_extras(rom_rel(gstr(ARG(1))), path, add_extra, dh);
    if (fs_trace())
        hos_log("fs: OpenDirectory(%s, mode %d)%s, %d entries from mods", gstr(ARG(1)), (int)ARG(2),
                dh->find == INVALID_HANDLE_VALUE ? ": not found" : "", dh->nextras);
    if (dh->find == INVALID_HANDLE_VALUE && !dh->nextras) { free(dh); RET(R_PATH_NOT_FOUND); return; }
    G64(ARG(0)) = new_handle(dh);
    RET(0);
}
/* (hookshot) A directory's listing is read, counted and closed under one lock: a CloseDirectory on another guest thread freed
   the handle (and its find handle) while a ReadDirectory or GetDirectoryEntryCount still walked it */
static SRWLOCK dir_lock = SRWLOCK_INIT;
/* ReadDirectory(long* count, DirectoryEntry* entries, DirectoryHandle, long max) */
HLE(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl) {
    AcquireSRWLockExclusive(&dir_lock);
    Handle *dh = get_handle(c, ARG(2));
    int64_t n = 0, max = (int64_t)ARG(3);
    while (n < max && !dh->find_done) {
        WIN32_FIND_DATAA *fd = &dh->fd;
        int isdir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int want = (isdir && (dh->is_dir & 1)) || (!isdir && (dh->is_dir & 2));
        if (strcmp(fd->cFileName, ".") && strcmp(fd->cFileName, "..") && want) {
            uint64_t e = ARG(1) + 0x310 * n;
            memset(GP(e), 0, 0x310);
            snprintf((char *)GP(e), 0x301, "%s", fd->cFileName);
            *(uint8_t *)GP(e + 0x304) = isdir ? 0 : 1;
            G64(e + 0x308) = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
            n++;
        }
        if (!FindNextFileA(dh->find, &dh->fd)) dh->find_done = 1;
    }
    while (n < max && dh->find_done && dh->extra_pos < dh->nextras) {   /* then the mods' own entries */
        const ExtraEntry *x = &dh->extras[dh->extra_pos++];
        if (!((x->is_dir && (dh->is_dir & 1)) || (!x->is_dir && (dh->is_dir & 2)))) continue;
        uint64_t e = ARG(1) + 0x310 * n;
        memset(GP(e), 0, 0x310);
        snprintf((char *)GP(e), 0x301, "%s", x->name);
        *(uint8_t *)GP(e + 0x304) = x->is_dir ? 0 : 1;
        G64(e + 0x308) = x->size;
        n++;
    }
    ReleaseSRWLockExclusive(&dir_lock);
    G64(ARG(0)) = (uint64_t)n;
    RET(0);
}
/* (hookshot) GetDirectoryEntryCount(long* count, DirectoryHandle): every entry the open mode lists, read or not */
HLE(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE) {
    AcquireSRWLockExclusive(&dir_lock);
    Handle *dh = get_handle(c, ARG(1));
    int64_t n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE f = dh->path[0] ? FindFirstFileA(dh->path, &fd) : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") &&
                ((isdir && (dh->is_dir & 1)) || (!isdir && (dh->is_dir & 2))))
                n++;
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    for (int i = 0; i < dh->nextras; i++)
        n += (dh->extras[i].is_dir && (dh->is_dir & 1)) || (!dh->extras[i].is_dir && (dh->is_dir & 2));
    ReleaseSRWLockExclusive(&dir_lock);
    G64(ARG(0)) = (uint64_t)n;
    RET(0);
}
HLE(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE) {
    AcquireSRWLockExclusive(&dir_lock);
    Handle *dh = get_handle(c, ARG(0));
    if (dh->find != INVALID_HANDLE_VALUE) FindClose(dh->find);
    free_handle(ARG(0));
    ReleaseSRWLockExclusive(&dir_lock);
    free(dh->extras);
    free(dh);
}
