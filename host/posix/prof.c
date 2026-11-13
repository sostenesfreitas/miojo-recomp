/* (linux) host/mkw_glue.c's profilers, with the same variables and the same lines in the log and files:
   HOS_PROFILE=<seconds> (the busiest threads), HOS_THREAD_TIMES=<seconds> (threads over 5% of a core), HOS_SAMPLE=<thread
   name> + HOS_SAMPLE_FILE (one thread's stacks every millisecond), HOS_SAMPLE_ALL=<path> (every guest thread and the
   renderer's at once, every 2 ms, while <path>.go exists) and HOS_WATCH=<file> (data breakpoints on guest addresses).
   Windows reads other threads with SuspendThread + GetThreadContext + RtlVirtualUnwind and sets debug registers; here:
   - CPU time per thread from /proc/self/task/<tid>/schedstat (stat's utime + stime without schedstats);
   - a thread's stack from inside it: SAMPLE_SIG sent to it with tgkill, its handler takes the interrupted rip from the
     signal's context and unwinds from there with libgcc's unwinder (the .eh_frame clang writes), the sampler waits;
   - breakpoints with perf_event_open (PERF_TYPE_BREAKPOINT, sigtrap=1: a SIGTRAP in the thread that touched the
     address, si_code TRAP_PERF), allowed on our own threads at the default perf_event_paranoid (2).
   Frames are written as <module>+<offset from its load base>: hookshot and hookshot_vk.so,
   offsets as in their linker maps (position-independent, based at 0). */
#include "../hos.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#include <windows.h>
#ifndef TRAP_PERF   /* (the kernel's since 5.13; glibc names it from 2.36) */
#define TRAP_PERF 6
#endif

int hos_thread_handles(HANDLE *out, const char **names, int max);   /* hos_thread.c */
void hos_prof_start(void);

/* ---- the process's threads: id, CPU time, name (the guest thread's, else the thread's own: SetThreadDescription) ---- */
typedef struct { int tid; uint64_t cpu_ns; char name[64]; int guest; } Task;

static uint64_t task_cpu_ns(int tid) {
    char p[64];
    unsigned long long ns = 0, ut = 0, st = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
    FILE *f = fopen(p, "r");
    if (f) {
        const int got = fscanf(f, "%llu", &ns);
        fclose(f);
        if (got == 1) return ns;
    }
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    f = fopen(p, "r");
    if (!f) return 0;
    char line[1024];
    const char *s = fgets(line, sizeof line, f) ? strrchr(line, ')') : NULL;
    fclose(f);
    /* (after "(comm)": state is field 3, utime 14, stime 15) */
    if (!s || sscanf(s + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
    return (ut + st) * (1000000000ull / (unsigned long long)sysconf(_SC_CLK_TCK));
}
static void task_comm(int tid, char *out, size_t cap) {
    char p[64];
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, (int)cap, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}
static int tasks(Task *out, int max) {
    static HANDLE gh[1024];
    static const char *gn[1024];
    const int ng = hos_thread_handles(gh, gn, 1024);
    static int gtid[1024];
    for (int i = 0; i < ng; i++) gtid[i] = (int)GetThreadId(gh[i]);
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;) {
        const int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        Task *t = &out[n++];
        t->tid = tid;
        t->cpu_ns = task_cpu_ns(tid);
        t->guest = 0;
        t->name[0] = 0;
        for (int i = 0; i < ng; i++)
            if (gtid[i] == tid) { snprintf(t->name, sizeof t->name, "%s", gn[i]); t->guest = 1; break; }
        if (!t->name[0]) task_comm(tid, t->name, sizeof t->name);
        if (!t->name[0]) snprintf(t->name, sizeof t->name, "tid %d", tid);
    }
    closedir(d);
    return n;
}

/* HOS_PROFILE=<seconds>: every interval, the busiest threads and their share of a core */
static DWORD WINAPI prof_thread(void *p) {
    const DWORD ms = (DWORD)(uintptr_t)p * 1000;
    enum { MAXT = 512 };
    static int ids[MAXT];
    static uint64_t was[MAXT];
    int nwas = 0;
    for (;;) {
        Sleep(ms);
        static Task cur[MAXT];
        static uint64_t delta[MAXT];
        const int n = tasks(cur, MAXT);
        for (int i = 0; i < n; i++) {
            delta[i] = cur[i].cpu_ns;
            for (int k = 0; k < nwas; k++)
                if (ids[k] == cur[i].tid) { delta[i] = cur[i].cpu_ns - was[k]; break; }
        }
        for (int i = 0; i < n; i++) ids[i] = cur[i].tid, was[i] = cur[i].cpu_ns;
        nwas = n;
        for (int i = 0; i < n; i++)   /* (busiest first) */
            for (int j = i + 1; j < n; j++)
                if (delta[j] > delta[i]) {
                    Task t = cur[i]; cur[i] = cur[j]; cur[j] = t;
                    const uint64_t d = delta[i]; delta[i] = delta[j]; delta[j] = d;
                }
        char line[1024];
        int len = snprintf(line, sizeof line, "prof: busiest threads (%% of a core):");
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += delta[i];
        for (int i = 0; i < n && i < 10 && len < (int)sizeof line - 80; i++)
            len += snprintf(line + len, sizeof line - len, " %s %.0f,", cur[i].name, delta[i] / (ms * 10000.0));
        snprintf(line + len, sizeof line - len, " all %.0f", total / (ms * 10000.0));
        hos_log("%s", line);
    }
    return 0;
}

/* HOS_THREAD_TIMES=<seconds>: every interval, the threads that used over 5% of a core */
static DWORD WINAPI thread_times_thread(void *p) {
    const DWORD ms = (DWORD)(uintptr_t)p * 1000;
    enum { MAXT = 512 };
    static struct { int tid; uint64_t cpu; } last[MAXT];
    int nlast = 0;
    for (;;) {
        Sleep(ms);
        static Task now[MAXT];
        const int n = tasks(now, MAXT);
        char line[4096];
        int len = snprintf(line, sizeof line, "threads in %u s:", (unsigned)(ms / 1000));
        for (int i = 0; i < n; i++) {
            uint64_t before = 0;
            int seen = 0;
            for (int k = 0; k < nlast; k++)
                if (last[k].tid == now[i].tid) before = last[k].cpu, seen = 1;
            const double pct = seen ? (double)(now[i].cpu_ns - before) / (ms * 1e6) * 100.0 : 0;
            if (pct >= 5.0 && len < (int)sizeof line - 96) len += snprintf(line + len, sizeof line - len, " %s %.0f%%;", now[i].name, pct);
        }
        for (int i = 0; i < n; i++) last[i].tid = now[i].tid, last[i].cpu = now[i].cpu_ns;
        nlast = n;
        hos_log("prof: %s", line);
    }
    return 0;
}

/* ---- one thread's stack, taken inside it ---- */
#define SAMPLE_SIG (SIGRTMIN + 3)
enum { DEPTH = 48 };
static volatile int sample_tid;
static volatile int sample_done;
static uintptr_t sample_pcs[DEPTH];
static int sample_n;

typedef struct { uintptr_t *pcs; int n, max; uintptr_t rip; int found; } Walk;
static _Unwind_Reason_Code walk_step(struct _Unwind_Context *uc, void *p) {
    Walk *w = (Walk *)p;
    int before = 0;
    const uintptr_t ip = (uintptr_t)_Unwind_GetIPInfo(uc, &before);
    if (!w->found) {   /* (the handler's frames and the signal's trampoline, up to the interrupted frame: frame 0) */
        if (ip == w->rip) w->found = 1;
        return _URC_NO_REASON;
    }
    if (w->n >= w->max || !ip) return _URC_END_OF_STACK;
    w->pcs[w->n++] = ip;   /* (return addresses, as the Windows sampler writes them) */
    return _URC_NO_REASON;
}
static void on_sample(int sig, siginfo_t *si, void *uctx) {
    (void)sig, (void)si;
    const int saved = errno;
    if ((int)syscall(SYS_gettid) == sample_tid) {
        const ucontext_t *u = (const ucontext_t *)uctx;
        const uintptr_t rip = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
        Walk w = {sample_pcs + 1, 0, DEPTH - 1, rip, 0};
        sample_pcs[0] = rip;
        _Unwind_Backtrace(walk_step, &w);
        sample_n = 1 + w.n;
        __atomic_store_n(&sample_done, 1, __ATOMIC_RELEASE);
    }
    errno = saved;
}
static void sampling_init(void) {
    static int done;
    if (done++) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SAMPLE_SIG, &sa, NULL);
    Walk w = {sample_pcs, 0, 1, 0, 1};
    _Unwind_Backtrace(walk_step, &w);   /* (the unwinder's first use loads and sets up: not in a handler) */
}
/* the thread's frames, leaf first, into pcs; 0 when it did not answer within 50 ms or has ended */
static int sample_one(int tid, uintptr_t *pcs, int max) {
    __atomic_store_n(&sample_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sample_tid, tid, __ATOMIC_RELEASE);
    if (syscall(SYS_tgkill, getpid(), tid, SAMPLE_SIG)) { sample_tid = 0; return -1; }
    const int64_t until = hos_now_ns() + 50000000;
    while (!__atomic_load_n(&sample_done, __ATOMIC_ACQUIRE)) {
        if (hos_now_ns() > until) { __atomic_store_n(&sample_tid, 0, __ATOMIC_RELEASE); return 0; }
        sched_yield();
    }
    sample_tid = 0;
    const int n = sample_n < max ? sample_n : max;
    memcpy(pcs, sample_pcs, sizeof *pcs * (size_t)n);
    return n;
}

/* a code address as module + offset: the module's file name and load base (dladdr) */
typedef struct { uintptr_t base; char name[64]; } Module;
static Module mods[64];
static int nmods;
static int module_of(uintptr_t pc, uintptr_t *base) {
    Dl_info d;
    if (!dladdr((void *)pc, &d) || !d.dli_fbase) { *base = 0; return -1; }
    *base = (uintptr_t)d.dli_fbase;
    for (int i = 0; i < nmods; i++)
        if (mods[i].base == *base) return i;
    if (nmods >= 64) return -1;
    const char *n = d.dli_fname && d.dli_fname[0] ? d.dli_fname : "hookshot";
    const char *b = strrchr(n, '/');
    mods[nmods].base = *base;
    snprintf(mods[nmods].name, sizeof mods[nmods].name, "%s", b ? b + 1 : n);
    return nmods++;
}
static uintptr_t exe_base(void) {
    uintptr_t base = 0;
    module_of((uintptr_t)&hos_prof_start, &base);
    return base;
}

/* HOS_SAMPLE=<thread name> (with HOS_PROFILE=<seconds>): that thread sampled every millisecond; every interval its
   most frequent places, as module+offset of the leaf and of the first frame in the runtime under it. HOS_SAMPLE_FILE:
   every sample's whole stack too, a line of frames leaf first (scripts/prof.py) */
static DWORD WINAPI sample_thread(void *p) {
    const char *want = (const char *)p;
    const DWORD ms = (DWORD)atoi(getenv("HOS_PROFILE")) * 1000;
    sampling_init();
    int tid = 0;
    while (!tid) {   /* (the renderer's threads start late) */
        Sleep(1000);
        static Task all[512];
        const int n = tasks(all, 512);
        for (int i = 0; i < n && !tid; i++)
            if (strstr(all[i].name, want)) tid = all[i].tid;
    }
    hos_log("prof: sampling thread \"%s\"", want);
    const uintptr_t exe = exe_base();
    enum { SLOTS = 4096 };
    static struct { uint64_t key; uint32_t n; } leaf[SLOTS], ours[SLOTS];
    uint32_t total = 0;
    DWORD last = GetTickCount();
    FILE *stacks = getenv("HOS_SAMPLE_FILE") ? fopen(getenv("HOS_SAMPLE_FILE"), "w") : NULL;
    for (;;) {
        Sleep(1);
        uintptr_t pcs[DEPTH];
        const int n = sample_one(tid, pcs, DEPTH);
        if (n < 0) return 0;   /* (the thread ended) */
        uint64_t keys[2] = {0, 0};
        char stack_line[1024];
        int stack_len = 0;
        for (int depth = 0; depth < n; depth++) {
            uintptr_t base;
            const int mi = module_of(pcs[depth], &base);
            const uint64_t key = (uint64_t)(mi < 0 ? 63 : mi) << 56 | (pcs[depth] - base);
            if (stacks && stack_len < (int)sizeof stack_line - 96)
                stack_len += snprintf(stack_line + stack_len, sizeof stack_line - stack_len, "%s%s+%llx", depth ? ";" : "",
                                      mi < 0 ? "?" : mods[mi].name, (unsigned long long)(pcs[depth] - base));
            if (depth == 0) keys[0] = key;
            if (base == exe && !keys[1]) keys[1] = key;
            if (base == exe && !stacks) break;
        }
        if (stacks && stack_len) fprintf(stacks, "%s\n", stack_line);
        if (n > 0) {
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
                    const int mi = (int)(tab[best].key >> 56);
                    len += snprintf(line + len, sizeof line - len, " %s+%llx %u,", mi < nmods ? mods[mi].name : "?",
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

/* HOS_SAMPLE_ALL=<path> (with HOS_PROFILE): while <path>.go exists, every 2 ms every guest thread and the renderer's
   threads (names starting "mkw_vk") sampled one after another: a line per thread, "<ms> <name>|<frames>", 16 frames
   at most (scripts/crit.py, scripts/hotlist.py) */
static DWORD WINAPI sample_all_thread(void *p) {
    const char *path = (const char *)p;
    char go[MAX_PATH];
    snprintf(go, sizeof go, "%s.go", path);
    sampling_init();
    FILE *out = NULL;
    enum { MAXT = 128 };
    static int th[MAXT];
    static char tn[MAXT][64];
    int nt = 0;
    int64_t t0 = 0;
    const int self = (int)syscall(SYS_gettid);
    /* (with HOS_CHAIN: "# at <ns>" before each sample, when it was taken: the threads are sampled one after another, a
       millisecond or more apart, and scripts/chain.py lays the samples on its exact path) */
    const int at = getenv("HOS_CHAIN") != NULL;
    for (;;) {
        if (access(go, F_OK)) {
            if (out) { fclose(out); out = NULL; hos_log("prof: all-thread sampling stopped"); }
            Sleep(200);
            continue;
        }
        if (!out) {
            out = fopen(path, "a");
            if (!out) { Sleep(1000); continue; }
            static Task all[512];
            const int n = tasks(all, 512);
            nt = 0;
            for (int i = 0; i < n && nt < MAXT; i++) {
                if (all[i].tid == self || (!all[i].guest && strncmp(all[i].name, "mkw_vk", 6))) continue;
                th[nt] = all[i].tid;
                snprintf(tn[nt], sizeof tn[nt], "%s", all[i].name);
                for (char *q = tn[nt]; *q; q++) if (*q == ' ' || *q == '|') *q = '_';
                nt++;
            }
            t0 = hos_now_ns();
            fprintf(out, "# t0_ns %lld\n", (long long)t0);   /* (the clock of HOS_CHAIN's records: scripts/chain.py) */
            hos_log("prof: all-thread sampling of %d threads into %s", nt, path);
        }
        Sleep(2);
        const long ms = (long)((hos_now_ns() - t0) / 1000000);
        for (int i = 0; i < nt; i++) {
            uintptr_t pcs[16];
            const int n = sample_one(th[i], pcs, 16);
            if (n < 0) continue;
            if (at) fprintf(out, "# at %lld\n", (long long)hos_now_ns());
            char line[1024];
            int len = snprintf(line, sizeof line, "%ld %s|", ms, tn[i]);
            for (int depth = 0; depth < n && len < (int)sizeof line - 96; depth++) {
                uintptr_t base;
                const int mi = module_of(pcs[depth], &base);
                len += snprintf(line + len, sizeof line - len, "%s%s+%llx", depth ? ";" : "", mi < 0 ? "?" : mods[mi].name,
                                (unsigned long long)(pcs[depth] - base));
            }
            fprintf(out, "%s\n", line);
        }
    }
    return 0;
}

/* HOS_WATCH=<file> (diagnostic): when <file> exists (up to 4 guest addresses in hex), data breakpoints on them on every
   guest thread: 4 s on reads and writes, then 4 s on writes; <file>.out gets, by address and the host place of the
   access, how many; then <file> is renamed <file>.done and the next one may come. */
static uint64_t watch_keys[8192];
static volatile LONG watch_counts[8192];
static volatile LONG watch_phase;
static struct sigaction watch_prev;
static void on_watch(int sig, siginfo_t *si, void *uctx) {
    if (si->si_code != TRAP_PERF) {   /* (a breakpoint of the game's or a debugger's: host/linux/win32.c's handlers) */
        if (watch_prev.sa_flags & SA_SIGINFO) watch_prev.sa_sigaction(sig, si, uctx);
        else if (watch_prev.sa_handler != SIG_DFL && watch_prev.sa_handler != SIG_IGN) watch_prev.sa_handler(sig);
        else { signal(sig, SIG_DFL); raise(sig); }
        return;
    }
    /* (si_perf_data, the event's sig_data: the watch's index. glibc's siginfo_t has no name for it: the kernel puts it
       after si_addr, at 24) */
    const unsigned long idx = *(const unsigned long *)((const char *)si + 24) & 3;
    const uint64_t rip = (uint64_t)((const ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
    const uint64_t key = ((uint64_t)(watch_phase & 1) << 62) | ((uint64_t)idx << 58) | rip;
    for (uint32_t i = (uint32_t)((key >> 2) * 2654435761u) & 8191, n = 0; n < 8192; n++, i = (i + 1) & 8191) {
        uint64_t k = __atomic_load_n(&watch_keys[i], __ATOMIC_RELAXED);
        if (!k) {
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&watch_keys[i], &expect, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) k = key;
            else k = expect;
        }
        if (k == key) { InterlockedIncrement(&watch_counts[i]); break; }
    }
}
enum { WATCH_FDS = 1024 * 4 };
static int watch_fd[WATCH_FDS];
static int nwatch_fd;
static void watch_arm(const uint64_t *addr, int n, int rw) {
    for (int i = 0; i < nwatch_fd; i++) close(watch_fd[i]);
    nwatch_fd = 0;
    if (!rw) return;
    static HANDLE gh[1024];
    static const char *gn[1024];
    const int ng = hos_thread_handles(gh, gn, 1024);
    int failed = 0, err = 0;
    for (int t = 0; t < ng; t++) {
        const int tid = (int)GetThreadId(gh[t]);
        if (!tid || tid == (int)GetCurrentThreadId()) continue;
        for (int k = 0; k < n && nwatch_fd < WATCH_FDS; k++) {
            struct perf_event_attr a;
            memset(&a, 0, sizeof a);
            a.type = PERF_TYPE_BREAKPOINT;
            a.size = sizeof a;
            a.bp_type = rw == 1 ? HW_BREAKPOINT_W : HW_BREAKPOINT_RW;
            a.bp_addr = (uint64_t)(uintptr_t)GP(addr[k]);
            a.bp_len = HW_BREAKPOINT_LEN_4;
            a.sample_period = 1;
            a.exclude_kernel = 1;
            a.exclude_hv = 1;
            a.sigtrap = 1;
            a.remove_on_exec = 1;
            a.sig_data = (uint64_t)k;
            const int fd = (int)syscall(SYS_perf_event_open, &a, tid, -1, -1, PERF_FLAG_FD_CLOEXEC);
            if (fd >= 0) watch_fd[nwatch_fd++] = fd;
            else failed++, err = errno;
        }
    }
    if (failed) hos_log("watch: %d breakpoints not set (perf_event_open: %s; /proc/sys/kernel/perf_event_paranoid)", failed, strerror(err));
}
static DWORD WINAPI watch_thread(void *p) {
    const char *path = (const char *)p;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_watch;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (;;) {
        Sleep(300);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        uint64_t addr[4];
        int n = 0;
        unsigned long long v;
        while (n < 4 && fscanf(f, "%llx", &v) == 1) addr[n++] = v;
        fclose(f);
        struct sigaction cur;   /* (ours ahead of win32.c's, which it calls for every other SIGTRAP) */
        sigaction(SIGTRAP, NULL, &cur);
        if (cur.sa_sigaction != on_watch) sigaction(SIGTRAP, &sa, &watch_prev);
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
                    uintptr_t base;
                    const int mi = module_of((uintptr_t)rip, &base);
                    fprintf(f, "%llx %s %s+%llx %ld\n", (unsigned long long)addr[(k >> 58) & 3], k >> 62 & 1 ? "w" : "rw",
                            mi < 0 ? "?" : mods[mi].name, (unsigned long long)(rip - base), (long)watch_counts[i]);
                }
            fclose(f);
        }
        unlink(done);
        rename(path, done);
        hos_log("watch: done");
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
