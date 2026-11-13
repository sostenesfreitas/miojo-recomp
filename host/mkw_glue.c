/* mkw-recomp's pieces of the standalone runtime that TOTKPort's hos/ keeps elsewhere: the 64-bit guest window's
   commit (TOTK commits its whole 4 GB window up front; MKW's 512 GB window is only reserved), and stand-ins for the
   hooks of files not taken into host/ (the renderer and display, the profiler, mods, TOTK's own patches). */
#include "hos.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#endif

/* ---- the guest window ---------------------------------------------------------------------------------------
   The runtime commits what it hands out (image, system region, heap, address regions: hos_commit), because host
   APIs given guest buffers (ReadFile into a game buffer) fail on reserved pages without raising an exception. A
   guest or host access to a page nothing committed is committed on first touch, 64 KB at a time, by the handler. */
/* (hookshot) a commit refused at Windows' commit limit (1455: RAM and page file full) is tried again for up to 5 s: the renderer's
   textures give commit back below a reserve (vk_texture.cpp's commit rule: every VRAM block is charged to the process's commit).
   Before, the commit failed and the game's next write to those pages crashed it: a 16 GB PC at 33.9 GB of commit, an area loading
   (a 960 KB commit, then a write into it from "ARes Loading 1"). 1: committed after waiting; 0: not */
static int commit_wait(void *p, SIZE_T size) {
    static volatile LONG waits, recovered;
    for (int i = 0; i < 100; i++) {   /* (100 x 50 ms) */
        Sleep(50);
        if (VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE)) {
            const LONG n = InterlockedIncrement(&recovered);
            if (n <= 8 || (n & 255) == 0)
                hos_log("mem: commit of %#llx bytes at the commit limit made after %d ms (%ld so far)", (unsigned long long)size,
                        (i + 1) * 50, (long)n);
            return 1;
        }
        if (GetLastError() != ERROR_COMMITMENT_LIMIT) return 0;
    }
    if (InterlockedIncrement(&waits) <= 8) hos_log("mem: commit of %#llx bytes still refused after 5 s at the commit limit", (unsigned long long)size);
    return 0;
}
/* (a commit that failed: the line, and at the commit limit Windows' out-of-memory notice, said once, on its own thread) */
static void commit_refused(uint64_t lo, uint64_t size, DWORD err) {
    hos_log("mem: commit of %#llx bytes at %#llx failed (error %lu)", (unsigned long long)size, (unsigned long long)lo, err);
    if (err == ERROR_COMMITMENT_LIMIT) {   /* (1455: RAM and page file full; the game crashes soon after) */
        static volatile LONG said;
        if (InterlockedExchange(&said, 1) == 0) {
            hos_log("mem: Windows is out of memory (RAM and page file): close other programs, or let Windows manage the page file size");
            extern DWORD WINAPI hos_out_of_memory_box(LPVOID);
            CloseHandle(CreateThread(NULL, 0, hos_out_of_memory_box, NULL, 0, NULL));
        }
    }
}

void hos_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    if (!hos_guest_range(addr, size)) {   /* (hookshot: guest pages only; the posix layer's commit changes any page's protection) */
        hos_log("mem: commit of %#llx bytes at %#llx: outside the guest window", (unsigned long long)size, (unsigned long long)addr);
        return;
    }
    const uint64_t lo = addr & ~0xFFFFULL, hi = (addr + size + 0xFFFF) & ~0xFFFFULL;
    if (!VirtualAlloc(GP(lo), (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE)) {
        const DWORD err = GetLastError();
        if (err == ERROR_COMMITMENT_LIMIT && commit_wait(GP(lo), (SIZE_T)(hi - lo))) return;
        commit_refused(lo, hi - lo, err);
    }
}

DWORD WINAPI hos_out_of_memory_box(LPVOID unused) {
    (void)unused;
    MessageBoxA(NULL,
                "Windows has run out of memory (RAM plus the page file), so Hookshot may close.\n\n"
                "Close other programs, and in Windows' System > About > Advanced system settings > Performance > Advanced > Virtual "
                "memory, choose \"Automatically manage paging file size\" (or give it at least 16 GB). A lower Quality preset in the "
                "Insert menu also uses less memory.",
                "Hookshot: out of memory", MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return 0;
}

static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t *p = (const uint8_t *)rec->ExceptionInformation[1];
    if (p < A64_MEM || p >= A64_MEM + HOS_GUEST_SPACE) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t guest = (uint64_t)(p - A64_MEM);
    if (guest < 0x100000) return EXCEPTION_CONTINUE_SEARCH;   /* (a null pointer: the game's own fault) */
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG touched;
    if (InterlockedIncrement(&touched) <= 16)
        hos_log("mem: %#llx committed on first touch (nothing handed it out)", (unsigned long long)guest);
    if (VirtualAlloc(GP(guest & ~0xFFFFULL), 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_EXECUTION;
    /* (hookshot: at the commit limit the touch waits for the renderer to give commit back, as hos_commit does) */
    const DWORD err = GetLastError();
    if (err == ERROR_COMMITMENT_LIMIT && commit_wait(GP(guest & ~0xFFFFULL), 0x10000)) return EXCEPTION_CONTINUE_EXECUTION;
    commit_refused(guest & ~0xFFFFULL, 0x10000, err);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A crash nothing handles: what, where (mkw.exe+offset: nx2/dllmap.py reads mkw.map the same way), and the guest
   state of the thread, as of its last call out of lifted code, with its frame chain from x29. */
static uint64_t peek_guest(uint64_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (a >= HOS_GUEST_SPACE - 8 || !VirtualQuery(GP(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    return G64(a);
}

static LONG WINAPI report_crash(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    const uint64_t rip = info->ContextRecord->Rip;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    const char *base = strrchr(name, '\\');
    const uint64_t addr = rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0;
    hos_log("CRASH: exception %#lx (%s %#llx, guest %#llx) at %s+%#llx", rec->ExceptionCode,
            rec->NumberParameters > 1 && rec->ExceptionInformation[0] == 1 ? "write" : "read",
            (unsigned long long)addr, (unsigned long long)(addr - (uint64_t)A64_MEM), base ? base + 1 : name,
            (unsigned long long)(rip - (uint64_t)mod));
#ifdef _WIN32
    {   /* the host stack from the faulting frame (x64 unwind data), as module+offset (nx2/samap.py maps mkw.exe's) */
        CONTEXT ctx = *info->ContextRecord;
        char hb[1024];
        int n = snprintf(hb, sizeof hb, "CRASH: host backtrace:");
        for (int i = 0; i < 20 && ctx.Rip && n < (int)sizeof hb - 48; i++) {
            HMODULE m = NULL;
            char mn[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m))
                GetModuleFileNameA(m, mn, MAX_PATH);
            const char *mb = strrchr(mn, '\\');
            n += snprintf(hb + n, sizeof hb - n, " %s+%#llx", mb ? mb + 1 : mn, (unsigned long long)(ctx.Rip - (uint64_t)m));
            DWORD64 image = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
            if (!f) break;
            void *handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler_data, &frame, NULL);
        }
        hos_log("%s", hb);
    }
#else
    {   /* (hookshot) Linux: the host stack as module+offset, from glibc (the frames above the signal's included): the
           x64 unwind data is Windows' */
        void *pcs[24];
        const int got = backtrace(pcs, 24);
        char hb[1024];
        int n = snprintf(hb, sizeof hb, "CRASH: host backtrace:");
        for (int i = 0; i < got && n < (int)sizeof hb - 48; i++) {
            Dl_info d;
            const char *mn = "?", *mb;
            uint64_t off = (uint64_t)(uintptr_t)pcs[i];
            if (dladdr(pcs[i], &d) && d.dli_fname) mn = d.dli_fname, off -= (uint64_t)(uintptr_t)d.dli_fbase;
            mb = strrchr(mn, '/');
            n += snprintf(hb + n, sizeof hb - n, " %s+%#llx", mb ? mb + 1 : mn, (unsigned long long)off);
        }
        hos_log("%s", hb);
    }
#endif
    Ctx *c = hos_ctx();
    if (c) {
        hos_log("CRASH: thread %s, sp %#llx", hos_thread_name(hos_self()), (unsigned long long)c->x[31]);
        for (int i = 0; i < 31; i += 4)
            hos_log("CRASH:   x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx", i,
                    (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1], i + 2,
                    (unsigned long long)c->x[i + 2], i + 3, i + 3 < 31 ? (unsigned long long)c->x[i + 3] : 0ULL);
        char bt[1024];
        int n = snprintf(bt, sizeof bt, "CRASH: guest backtrace: lr %#llx", (unsigned long long)c->x[30]);
        uint64_t fp = c->x[29];
        for (int i = 0; i < 40 && fp && !(fp & 15) && n < (int)sizeof bt - 24; i++) {
            const uint64_t lr = peek_guest(fp + 8), next = peek_guest(fp);
            n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)lr);
            if (next <= fp) break;
            fp = next;
        }
        hos_log("%s", bt);
        /* the memory x0, x1 and x19 point to (the object a crash is usually about) */
        static const int dumped[] = {0, 1, 19};
        for (int k = 0; k < 3; k++) {
            const int r = dumped[k];
            const uint64_t base = c->x[r];
            if (base < 0x100000 || !peek_guest(base) && !peek_guest(base + 8) && !peek_guest(base + 0x100)) continue;
            for (int row = 0; row < 0x120; row += 32)
                hos_log("CRASH: [x%d+%#05x] %016llx %016llx %016llx %016llx", r, row,
                        (unsigned long long)peek_guest(base + row), (unsigned long long)peek_guest(base + row + 8),
                        (unsigned long long)peek_guest(base + row + 16), (unsigned long long)peek_guest(base + row + 24));
        }
    }
    hos_log_flush();
    void hos_hostcall_stats(void);
    hos_hostcall_stats();   /* (HOS_HOSTCALL_STATS=1: the host calls made, and the last 256 of them) */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* (hos_prof.c's, for hos_fatal) the current thread's guest frame chain from x29 */
void hos_backtrace(void) {
    Ctx *c = hos_ctx();
    if (!c) return;
    char bt[1024];
    int n = snprintf(bt, sizeof bt, "guest backtrace: lr %#llx", (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 40 && fp && !(fp & 15) && n < (int)sizeof bt - 24; i++) {
        const uint64_t lr = peek_guest(fp + 8), next = peek_guest(fp);
        n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    fprintf(stderr, "[hos] %s\n", bt);
    fflush(stderr);
}

/* HOS_STATS_AFTER=<seconds> (with HOS_HOSTCALL_STATS=1): the host call counts and the last calls at that time, for
   runs that are stopped rather than exit (what a quiet game is doing) */
static DWORD WINAPI stats_after(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    void hos_hostcall_stats(void);
    hos_log("stats: after %u s", (unsigned)(uintptr_t)p);
    void hos_thread_dump(void);
    hos_thread_dump();
    hos_log_flush();
    hos_hostcall_stats();
    return 0;
}

/* heap corruption and fail-fast reports go to the process's end without reaching the unhandled filter: reported here
   first (the backtrace is where the heap noticed, after the bad write) */
static LONG CALLBACK report_fatal(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000374 || code == 0xC0000409) report_crash(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

void hos_commit_init(void) {
    AddVectoredExceptionHandler(1, commit_on_touch);
    AddVectoredExceptionHandler(0, report_fatal);
    SetUnhandledExceptionFilter(report_crash);
    const char *after = getenv("HOS_STATS_AFTER");
    if (after && atoi(after) > 0) CreateThread(NULL, 0, stats_after, (void *)(uintptr_t)atoi(after), 0, NULL);
}

/* ---- stand-ins --------------------------------------------------------------------------------------------- */
/* HOS_PROFILE=<seconds> (in place of TOTKPort's hos_prof.c): every interval, the process's busiest threads and their
   share of a core (guest threads by their guest names, host threads by their descriptions): which one a frame rate
   waits for */
#ifdef _WIN32
#include <tlhelp32.h>
int hos_thread_handles(HANDLE *out, const char **names, int max);   /* hos_thread.c */
static DWORD WINAPI prof_thread(void *p) {
    const DWORD ms = (DWORD)(uintptr_t)p * 1000;
    enum { MAXT = 512 };
    static DWORD ids[MAXT];
    static uint64_t was[MAXT];
    int nwas = 0;
    for (;;) {
        Sleep(ms);
        static HANDLE gh[1024];
        static const char *gn[1024];
        const int ng = hos_thread_handles(gh, gn, 1024);
        struct { DWORD id; uint64_t cpu, delta; char name[64]; } cur[MAXT];
        int n = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok && n < MAXT; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c0, e0, k, u;
            if (GetThreadTimes(h, &c0, &e0, &k, &u)) {
                cur[n].id = te.th32ThreadID;
                cur[n].cpu = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
                cur[n].delta = cur[n].cpu;
                for (int i = 0; i < nwas; i++)
                    if (ids[i] == te.th32ThreadID) { cur[n].delta = cur[n].cpu - was[i]; break; }
                snprintf(cur[n].name, sizeof cur[n].name, "tid %lu", te.th32ThreadID);
                for (int i = 0; i < ng; i++)
                    if (GetThreadId(gh[i]) == te.th32ThreadID) { snprintf(cur[n].name, sizeof cur[n].name, "%s", gn[i]); break; }
                if (!strncmp(cur[n].name, "tid ", 4)) {
                    PWSTR d = NULL;
                    if (SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0])
                        WideCharToMultiByte(CP_UTF8, 0, d, -1, cur[n].name, sizeof cur[n].name, NULL, NULL);
                    if (d) LocalFree(d);
                }
                n++;
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        for (int i = 0; i < n && i < MAXT; i++) ids[i] = cur[i].id, was[i] = cur[i].cpu;
        nwas = n;
        for (int i = 0; i < n; i++)   /* (busiest first) */
            for (int j = i + 1; j < n; j++)
                if (cur[j].delta > cur[i].delta) { char t[sizeof cur[0]]; memcpy(t, &cur[i], sizeof t); memcpy(&cur[i], &cur[j], sizeof t); memcpy(&cur[j], t, sizeof t); }
        char line[1024];
        int len = snprintf(line, sizeof line, "prof: busiest threads (%% of a core):");
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += cur[i].delta;
        for (int i = 0; i < n && i < 10 && len < (int)sizeof line - 80; i++)
            len += snprintf(line + len, sizeof line - len, " %s %.0f,", cur[i].name, cur[i].delta / (ms * 100.0));
        snprintf(line + len, sizeof line - len, " all %.0f", total / (ms * 100.0));
        hos_log("%s", line);
    }
    return 0;
}
/* HOS_SAMPLE=<thread name> (with HOS_PROFILE=<seconds>): that thread's instruction pointer sampled every millisecond;
   every interval the most frequent places, as module+offset of the leaf and of the first frame in mkw.exe under it
   (what of ours called into wgpu or the system). nx2/samap.py maps the mkw.exe offsets to functions. */
static DWORD WINAPI sample_thread(void *p) {
    const char *want = (const char *)p;
    const DWORD ms = (DWORD)atoi(getenv("HOS_PROFILE")) * 1000;
    HANDLE th = NULL;
    while (!th) {   /* (the renderer's threads start late) */
        Sleep(1000);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok && !th; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
            if (!h) continue;
            PWSTR d = NULL;
            char name[64] = "";
            if (SUCCEEDED(GetThreadDescription(h, &d)) && d) WideCharToMultiByte(CP_UTF8, 0, d, -1, name, sizeof name, NULL, NULL);
            if (d) LocalFree(d);
            if (strstr(name, want)) th = h;
            else CloseHandle(h);
        }
        CloseHandle(snap);
    }
    hos_log("prof: sampling thread \"%s\"", want);
    const HMODULE exe = GetModuleHandleA(NULL);
    enum { SLOTS = 4096 };
    static struct { uint64_t key; uint32_t n; } leaf[SLOTS], ours[SLOTS];
    static char modname[64][64];
    static HMODULE mods[64];
    int nmods = 0;
    uint32_t total = 0;
    DWORD last = GetTickCount();
    /* HOS_SAMPLE_FILE=<path>: every sample's whole stack too, one line of module+offset frames (leaf first), for
       an offline profile (work/prof.py maps them to functions with the linker maps) */
    FILE *stacks = getenv("HOS_SAMPLE_FILE") ? fopen(getenv("HOS_SAMPLE_FILE"), "w") : NULL;
    for (;;) {
        Sleep(1);
        if (SuspendThread(th) == (DWORD)-1) return 0;   /* (the thread ended) */
        CONTEXT ctx = {0};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        const BOOL got = GetThreadContext(th, &ctx);
        uint64_t keys[2] = {0, 0};
        char stack_line[1024];
        int stack_len = 0;
        if (got) {
            for (int depth = 0; depth < 48 && ctx.Rip; depth++) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                int mi = -1;
                for (int i = 0; i < nmods; i++) if (mods[i] == m) mi = i;
                if (mi < 0 && nmods < 64) {
                    mi = nmods++;
                    mods[mi] = m;
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, MAX_PATH);
                    const char *b = strrchr(path, '\\');
                    snprintf(modname[mi], sizeof modname[mi], "%s", b ? b + 1 : path);
                }
                const uint64_t key = (uint64_t)(mi < 0 ? 63 : mi) << 56 | (ctx.Rip - (uint64_t)m);
                if (stacks && stack_len < (int)sizeof stack_line - 96)
                    stack_len += snprintf(stack_line + stack_len, sizeof stack_line - stack_len, "%s%s+%llx",
                                          depth ? ";" : "", mi < 0 ? "?" : modname[mi],
                                          (unsigned long long)(ctx.Rip - (uint64_t)m));
                if (depth == 0) keys[0] = key;
                if (m == exe && !keys[1]) keys[1] = key;
                if (m == exe && !stacks) break;
                DWORD64 image = 0;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
                if (!f) break;
                void *hd;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &hd, &frame, NULL);
            }
        }
        ResumeThread(th);
        if (stacks && got && stack_len) fprintf(stacks, "%s\n", stack_line);
        if (got) {
            total++;
            for (int t = 0; t < 2; t++) {
                if (!keys[t]) continue;
                typeof(leaf[0]) *tab = t ? ours : leaf;
                for (uint32_t s = (uint32_t)(keys[t] * 0x9E3779B97F4A7C15ull >> 52), k = 0; k < SLOTS; k++, s = (s + 1) % SLOTS) {
                    if (tab[s].key == keys[t]) { tab[s].n++; break; }
                    if (!tab[s].key) { tab[s].key = keys[t], tab[s].n = 1; break; }
                }
            }
        }
        if (GetTickCount() - last >= ms) {
            last = GetTickCount();
            for (int t = 0; t < 2; t++) {
                typeof(leaf[0]) *tab = t ? ours : leaf;
                char line[2048];
                int len = snprintf(line, sizeof line, "prof-sample %s (%u samples):", t ? "ours" : "leaf", total);
                for (int r = 0; r < 30 && len < (int)sizeof line - 64; r++) {
                    int best = -1;
                    for (int s = 0; s < SLOTS; s++) if (tab[s].n && (best < 0 || tab[s].n > tab[best].n)) best = s;
                    if (best < 0) break;
                    len += snprintf(line + len, sizeof line - len, " %s+%llx %u,", modname[tab[best].key >> 56],
                                    (unsigned long long)(tab[best].key & ((1ull << 56) - 1)), tab[best].n);
                    tab[best].n = 0;
                }
                hos_log("%s", line);
                memset(tab, 0, sizeof leaf);
            }
            total = 0;
            if (stacks) fflush(stacks);
        }
    }
    return 0;
}

/* HOS_THREAD_TIMES=<seconds>: every interval, the process's threads that used over 5% of a core, by name, with
   their share of one core (which thread holds the frame back) */
static DWORD WINAPI thread_times_thread(void *p) {
    const DWORD ms = (DWORD)(uintptr_t)p * 1000;
    enum { MAX = 512 };
    static struct { DWORD tid; uint64_t cpu; } last[MAX];
    int nlast = 0;
    for (;;) {
        Sleep(ms);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof te};
        char line[4096];
        int n = snprintf(line, sizeof line, "threads in %u s:", (unsigned)(ms / 1000));
        static struct { DWORD tid; uint64_t cpu; } now[MAX];
        int nnow = 0;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || nnow >= MAX) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c, x, k, u;
            uint64_t cpu = 0;
            if (GetThreadTimes(h, &c, &x, &k, &u))
                cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
            uint64_t before = 0;
            int seen = 0;
            for (int i = 0; i < nlast; i++)
                if (last[i].tid == te.th32ThreadID) before = last[i].cpu, seen = 1;
            now[nnow].tid = te.th32ThreadID, now[nnow].cpu = cpu, nnow++;
            const double pct = seen ? (double)(cpu - before) / (ms * 10000.0) * 100.0 : 0;
            if (pct >= 5.0 && n < (int)sizeof line - 96) {
                PWSTR d = NULL;
                char name[64] = "?";
                if (SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0]) WideCharToMultiByte(CP_UTF8, 0, d, -1, name, sizeof name, NULL, NULL);
                if (d) LocalFree(d);
                n += snprintf(line + n, sizeof line - n, " %s %.0f%%;", name, pct);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        memcpy(last, now, sizeof(now[0]) * (size_t)nnow);
        nlast = nnow;
        hos_log("prof: %s", line);
    }
    return 0;
}

/* (hookshot) HOS_SAMPLE_ALL=<path> (with HOS_PROFILE): while <path>.go exists, every 2 ms every guest thread and the
   renderer's threads (descriptions starting "mkw_vk") are sampled at the same instant: one line per thread,
   "<ms> <name>|<frames>", the frames module+offset leaf first (16 at most). What each thread does while another waits:
   the frame's critical path (scripts/crit.py) */
/* HOS_WATCH=<file> (diagnostic): when <file> exists (up to 4 guest addresses in hex), CPU data breakpoints on them on
   every guest thread: 4 s on reads and writes, then 4 s on writes; <file>.out gets, by address and the host place of
   the access, how many; then <file> is renamed <file>.done and the next one may come. */
static uint64_t watch_keys[8192];
static volatile LONG watch_counts[8192];
static volatile LONG watch_phase;
static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD64 hit = e->ContextRecord->Dr6 & 15;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;
    int idx = 0;
    while (!(hit >> idx & 1)) idx++;
    const uint64_t key = ((uint64_t)(watch_phase & 1) << 62) | ((uint64_t)idx << 58) | (uint64_t)e->ExceptionRecord->ExceptionAddress;
    for (uint32_t i = (uint32_t)((key >> 2) * 2654435761u) & 8191, n = 0; n < 8192; n++, i = (i + 1) & 8191) {
        uint64_t k = __atomic_load_n(&watch_keys[i], __ATOMIC_RELAXED);
        if (!k) {
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&watch_keys[i], &expect, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) k = key;
            else k = expect;
        }
        if (k == key) { InterlockedIncrement(&watch_counts[i]); break; }
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void watch_arm(const uint64_t *addr, int n, int rw) {
    static HANDLE gh[1024];
    static const char *gn[1024];
    const int ng = hos_thread_handles(gh, gn, 1024);
    for (int i = 0; i < ng; i++) {
        if (GetThreadId(gh[i]) == GetCurrentThreadId()) continue;
        if (SuspendThread(gh[i]) == (DWORD)-1) continue;
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(gh[i], &c)) {
            DWORD64 *dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
            c.Dr7 = 0;
            for (int k = 0; k < n && rw; k++) {
                *dr[k] = (DWORD64)GP(addr[k]);
                c.Dr7 |= (DWORD64)1 << (k * 2);                     /* (local enable) */
                c.Dr7 |= (DWORD64)((rw & 3) | (3 << 2)) << (16 + k * 4);   /* (rw, 4 bytes) */
            }
            c.Dr6 = 0;
            SetThreadContext(gh[i], &c);
        }
        ResumeThread(gh[i]);
    }
}
static DWORD WINAPI watch_thread(void *p) {
    const char *path = (const char *)p;
    AddVectoredExceptionHandler(1, watch_handler);
    for (;;) {
        Sleep(300);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        uint64_t addr[4];
        int n = 0;
        unsigned long long v;
        while (n < 4 && fscanf(f, "%llx", &v) == 1) addr[n++] = v;
        fclose(f);
        memset(watch_keys, 0, sizeof watch_keys);
        memset((void *)watch_counts, 0, sizeof watch_counts);
        for (int k = 0; k < n; k++) hos_log("watch: %llx holds %08x", (unsigned long long)addr[k], *(uint32_t *)GP(addr[k]));
        for (int ph = 0; ph < 2; ph++) {
            watch_phase = ph;
            for (int t = 0; t < 4; t++) {   /* (threads that started since) */
                watch_arm(addr, n, ph ? 1 : 3);
                Sleep(1000);
            }
        }
        watch_arm(addr, 0, 0);
        char out[MAX_PATH], done[MAX_PATH];
        snprintf(out, sizeof out, "%s.out", path);
        snprintf(done, sizeof done, "%s.done", path);
        f = fopen(out, "a");
        if (f) {
            for (int i = 0; i < 8192; i++)
                if (watch_keys[i]) {
                    const uint64_t k = watch_keys[i], rip = k & ((1ULL << 58) - 1);
                    HMODULE m = NULL;
                    char full[MAX_PATH] = "?";
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m);
                    if (m) GetModuleFileNameA(m, full, sizeof full);
                    const char *name = strrchr(full, 92) ? strrchr(full, 92) + 1 : full;
                    fprintf(f, "%llx %s %s+%llx %ld\n", (unsigned long long)addr[(k >> 58) & 3], k >> 62 & 1 ? "w" : "rw", name,
                            (unsigned long long)(rip - (uint64_t)m), watch_counts[i]);
                }
            fclose(f);
        }
        DeleteFileA(done);
        MoveFileA(path, done);
        hos_log("watch: done");
    }
}
static DWORD WINAPI sample_all_thread(void *p) {
    const char *path = (const char *)p;
    char go[MAX_PATH];
    snprintf(go, sizeof go, "%s.go", path);
    FILE *out = NULL;
    static HMODULE mods[64];
    static char modname[64][64];
    int nmods = 0;
    enum { MAXT = 128 };
    static HANDLE th[MAXT];
    static char tn[MAXT][64];
    int nt = 0;
    int64_t t0 = 0;
    for (;;) {
        if (GetFileAttributesA(go) == INVALID_FILE_ATTRIBUTES) {
            if (out) { fclose(out); out = NULL; hos_log("prof: all-thread sampling stopped"); }
            Sleep(200);
            continue;
        }
        if (!out) {
            out = fopen(path, "a");
            if (!out) { Sleep(1000); continue; }
            for (int i = 0; i < nt; i++) CloseHandle(th[i]);
            nt = 0;
            static HANDLE gh[1024];
            static const char *gn[1024];
            const int ng = hos_thread_handles(gh, gn, 1024);
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok && nt < MAXT; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                char name[64] = "";
                for (int i = 0; i < ng; i++)
                    if (GetThreadId(gh[i]) == te.th32ThreadID) { snprintf(name, sizeof name, "%s", gn[i]); break; }
                HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                if (!h) continue;
                if (!name[0]) {
                    PWSTR d = NULL;
                    char desc[64] = "";
                    if (SUCCEEDED(GetThreadDescription(h, &d)) && d) WideCharToMultiByte(CP_UTF8, 0, d, -1, desc, sizeof desc, NULL, NULL);
                    if (d) LocalFree(d);
                    if (!strncmp(desc, "mkw_vk", 6)) snprintf(name, sizeof name, "%s", desc);
                }
                if (!name[0]) { CloseHandle(h); continue; }
                for (char *q = name; *q; q++) if (*q == ' ' || *q == '|') *q = '_';
                th[nt] = h;
                snprintf(tn[nt], sizeof tn[nt], "%s", name);
                nt++;
            }
            CloseHandle(snap);
            t0 = hos_now_ns();
            fprintf(out, "# t0_ns %lld\n", (long long)t0);   /* (the clock of HOS_CHAIN's records: scripts/chain.py) */
            hos_log("prof: all-thread sampling of %d threads into %s", nt, path);
        }
        Sleep(2);
        const long ms = (long)((hos_now_ns() - t0) / 1000000);
        for (int i = 0; i < nt; i++) {
            if (SuspendThread(th[i]) == (DWORD)-1) continue;
            CONTEXT ctx = {0};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            const BOOL got = GetThreadContext(th[i], &ctx);
            char line[1024];
            int len = snprintf(line, sizeof line, "%ld %s|", ms, tn[i]);
            for (int depth = 0; got && depth < 16 && ctx.Rip && len < (int)sizeof line - 96; depth++) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                int mi = -1;
                for (int k = 0; k < nmods; k++) if (mods[k] == m) mi = k;
                if (mi < 0 && nmods < 64) {
                    mi = nmods++;
                    mods[mi] = m;
                    char mp[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, mp, MAX_PATH);
                    const char *b = strrchr(mp, '\\');
                    snprintf(modname[mi], sizeof modname[mi], "%s", b ? b + 1 : mp);
                }
                len += snprintf(line + len, sizeof line - len, "%s%s+%llx", depth ? ";" : "", mi < 0 ? "?" : modname[mi],
                                (unsigned long long)(ctx.Rip - (uint64_t)m));
                DWORD64 image = 0;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
                if (!f) break;
                void *hd;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &hd, &frame, NULL);
            }
            ResumeThread(th[i]);
            fprintf(out, "%s\n", line);
        }
    }
    return 0;
}

void hos_prof_start(void) {
    const char *tt = getenv("HOS_THREAD_TIMES");
    if (tt && atoi(tt) > 0) CreateThread(NULL, 0, thread_times_thread, (void *)(uintptr_t)atoi(tt), 0, NULL);
    const char *e = getenv("HOS_PROFILE");
    if (e && atoi(e) > 0) CreateThread(NULL, 0, prof_thread, (void *)(uintptr_t)atoi(e), 0, NULL);
    const char *s = getenv("HOS_SAMPLE");
    if (e && atoi(e) > 0 && s && *s) CreateThread(NULL, 0, sample_thread, (void *)s, 0, NULL);
    const char *all = getenv("HOS_SAMPLE_ALL");
    if (all && *all) CreateThread(NULL, 0, sample_all_thread, (void *)all, 0, NULL);
    const char *watch = getenv("HOS_WATCH");
    if (watch && *watch) CreateThread(NULL, 0, watch_thread, (void *)watch, 0, NULL);
}
#endif   /* (linux) host/posix/prof.c */
void hos_mem_checkpoint(const char *what) { (void)what; }        /* (hos_prof.c: HOS_MEM_TIMELINE) */
int hos_game_cores(void) { return 0; }                           /* (totk_cores.c) */
int hos_blank_cached_icon(const char *guest, const char *host) { (void)guest, (void)host; return 0; }   /* (TOTK) */
int hos_ultrawide_redirect(const char *guest, char *out, size_t cap) { (void)guest, (void)out, (void)cap; return 0; }
uint64_t hos_title_input(uint64_t buttons) { return buttons; }   /* (totk_title.c) */
uint64_t hos_frame_step_buttons(uint64_t buttons) { return buttons; }   /* (nn_vi.c) */
/* (hookshot) hos_sdl_pads, hos_sdl_rumble: host/pad_sdl.c (from TOTKPort; SDL2.dll beside the exe) */

/* TOTK's emulated GPU (gpu.c, its Maxwell command processor), which nvdrv.c and the debug commands call into: MKW
   has none (its NVN2 calls go to nvn_native.c), so a submission is only counted. */
int hos_gpu_async(void) { return 0; }
void hos_gpu_submit(int channel_fd, const uint64_t *entries, uint32_t n) {
    static volatile LONG seen;
    if (InterlockedIncrement(&seen) <= 4) hos_log("gpu: submit of %u entries on channel %d (no GPU yet)", n, channel_fd);
    (void)entries;
}
void hos_gpu_queue_submit(int channel_fd, const uint64_t *entries, uint32_t n, int incr_id) {
    (void)incr_id;
    hos_gpu_submit(channel_fd, entries, n);
}

/* The loop-header interrupt polls (nx2/poll.py): the virtual cores (HOS_VCORES) ask a thread holding a core a
   waiter should have to give it up there, so guest code with no nn::os call or SVC in it (a long job) is preempted
   as Horizon would preempt it. */
/* (nx2/rtgen.py) the loop-header poll: the lifted code has cleared c->poll and calls this with no argument, so that the
   compiler keeps guest registers in host registers across the loop header (a call handed the context could change any
   of it). The thread's context is the running one's (hos_ctx: a fiber's while it runs one), which is where the kick
   (hos_thread.c) set the flag; nothing here reads it */
void a64_poll_cur(void) {
    void hos_vcore_checkpoint(void);
    hos_vcore_checkpoint();
}
void a64_poll(Ctx *c) {   /* (the old form, A64_POLL_CTX builds) */
    c->poll = 0;
    a64_poll_cur();
}

/* CNTPCT_EL0 at the Switch 2's 31.25 MHz (rt/a64_rt.c's default counts the Switch 1's 19.2 MHz). The SDK checks
   nn::os::GetSystemTickFrequency against 31250000 at init and aborts otherwise. */
uint64_t a64_cntpct_host(void) {
    static LARGE_INTEGER base;
    static uint64_t mult;
    LARGE_INTEGER now;
    if (!__atomic_load_n(&mult, __ATOMIC_ACQUIRE)) {
        LARGE_INTEGER f, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&b);
        base = b;
        __atomic_store_n(&mult, (HOS_TICK_HZ << 32) / (uint64_t)f.QuadPart, __ATOMIC_RELEASE);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)(((unsigned __int128)(uint64_t)(now.QuadPart - base.QuadPart) * mult) >> 32);
}

/* ---- what the renderer (gpu/render, TOTKPort's) asks of TOTKPort's hos_prof.c and totk_ultrawide.c ---- */
int hos_ab(void) { return 0; }                    /* (hos_prof.c: A/B measurement phases) */
/* (with the renderer's mem: lines) HOS_MEM_MAP=1: how much of the guest window is committed, and its resident part */
void hos_memory_map_log(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_MEM_MAP");
    if (!on) return;
    uint64_t committed = 0, regions = 0;
    for (uint64_t a = 0; a < HOS_GUEST_SPACE;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(GP(a), &m, sizeof m)) break;
        const uint64_t end = (uint64_t)((uint8_t *)m.BaseAddress + m.RegionSize - (uint8_t *)GP(0));
        if (m.State == MEM_COMMIT) committed += m.RegionSize, regions++;
        a = end > a ? end : a + 0x1000;
    }
    hos_log("mem: guest window %llu MB committed in %llu regions", (unsigned long long)(committed >> 20),
            (unsigned long long)regions);
    /* the rest of the address space: private memory by allocation, the largest */
    struct { uint64_t base, bytes; } top[8] = {{0}};
    uint64_t other = 0, cur_base = 0, cur = 0;
    const uint64_t g0 = (uint64_t)(uintptr_t)GP(0), g1 = g0 + HOS_GUEST_SPACE;
    for (uint64_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)(uintptr_t)a, &m, sizeof m)) break;
        const uint64_t base = (uint64_t)(uintptr_t)m.AllocationBase, end = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
        if (base != cur_base) {
            for (int k = 0; k < 8 && cur; k++)
                if (cur > top[k].bytes) {
                    memmove(&top[k + 1], &top[k], (size_t)(7 - k) * sizeof top[0]);
                    top[k].base = cur_base, top[k].bytes = cur;
                    break;
                }
            cur_base = base, cur = 0;
        }
        if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && (a < g0 || a >= g1)) other += m.RegionSize, cur += m.RegionSize;
        a = end > a ? end : a + 0x1000;
    }
    hos_log("mem: private outside the guest window %llu MB; largest: %llx %llu MB, %llx %llu MB, %llx %llu MB, %llx %llu "
            "MB, %llx %llu MB, %llx %llu MB", (unsigned long long)(other >> 20), (unsigned long long)top[0].base,
            (unsigned long long)(top[0].bytes >> 20), (unsigned long long)top[1].base, (unsigned long long)(top[1].bytes >> 20),
            (unsigned long long)top[2].base, (unsigned long long)(top[2].bytes >> 20), (unsigned long long)top[3].base,
            (unsigned long long)(top[3].bytes >> 20), (unsigned long long)top[4].base, (unsigned long long)(top[4].bytes >> 20),
            (unsigned long long)top[5].base, (unsigned long long)(top[5].bytes >> 20));
}
void hos_ultrawide_apply(void) {}                 /* (totk_ultrawide.c) */
float hos_view_aspect;                            /* (totk_ultrawide.c: 0, the game's own 16:9) */
void hos_host_thread_register_self(const char *name) {   /* (hos_prof.c: names for its stats; here the thread's description) */
    wchar_t w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(GetCurrentThread(), w);
}
void hos_process_memory(uint64_t *private_bytes, uint64_t *working_set) {
    typedef struct {
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
            QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    } Counters;
    typedef BOOL(WINAPI * Fn)(HANDLE, Counters *, DWORD);
    static Fn fn;
    if (!fn) fn = (Fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    Counters m;
    memset(&m, 0, sizeof m);
    m.cb = sizeof m;
    if (!fn || !fn(GetCurrentProcess(), &m, sizeof m)) memset(&m, 0, sizeof m);
    *private_bytes = m.PrivateUsage;
    *working_set = m.WorkingSetSize;
}
