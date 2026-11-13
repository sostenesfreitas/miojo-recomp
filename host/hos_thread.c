/* Guest threads: one host thread each, with its own Ctx, TLS block and thread-local region.

   TPIDR_EL0 (TLS): a 4 KB block per thread, TP in its middle. TOTK's main reads its one thread-local at TP+0x10
   (local-exec, after AArch64's 16-byte TCB); the area below TP is zeroed room for whatever the SDK's musl keeps
   there. TPIDRRO_EL0 (thread-local region): 0x200 bytes; the SDK's nn::os keeps the current ThreadType* at
   +0x1F8, so lifted SDK code that reads it inline finds the same thread the HLE knows. */
#include "hos.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION   /* (older Windows SDKs lack it: Windows 10 1803 and later take it) */
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#include <emmintrin.h>
#ifdef __EMSCRIPTEN__
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>
#endif

struct HosThread {
    Ctx ctx;
    uint64_t guest_tt, entry, arg, stack, stack_size, id;
    uint64_t tls_block, tlr;
    int priority, ideal_core;
    uint64_t core_mask;
    uint64_t tls_slots[HOS_TLS_SLOTS];
    char name[48];
    HANDLE handle;
    int started;
    int serial;   /* one of the frame's serial threads (hos_thread_serial), set with the name */
    /* the kernel side (hos_svc.c): this thread's Horizon handle and the event it blocks on */
    uint32_t kernel_handle;
    HANDLE wake;
    /* virtual cores (HOS_VCORES): the core held + 1 (0: none), the last one, a better waiter's request, queued, and
       whether it has run without one since an escape (vc_acquire; until its next block, hos_vcore_away, or the next
       hos_vcore_back) */
    int vc_held, vc_last, vc_queued, vc_escaped;
    volatile int vc_kick;
    int64_t vc_since;
    CONDITION_VARIABLE vc_cv;
    /* (hookshot) fibers: the context running now (NULL: ctx, the thread's own), the thread's host fiber once it
       switched to a guest fiber */
    Ctx *volatile run_ctx;
    void *host_fiber;
};

/* The SDK's own mutexes (InternalCriticalSection) tag a lock word with the owner's handle, read from
   ThreadType+0x1B0 through TLR+0x1F8 (sdk+0x13186c). Every thread gets a unique handle there. */
#define TT_HANDLE 0x1B0
static volatile LONG next_handle = 0x10000;
uint32_t hos_thread_handle(HosThread *t) { return t->kernel_handle; }

/* nnosInitialize gives the main thread the SDK's own ThreadType (it writes TLR+0x1F8): adopt it, so the HLE and the
   SDK agree on which object is the current thread. */
void hos_thread_adopt_guest(HosThread *t) {
    uint64_t tt = G64(t->tlr + 0x1F8);
    if (tt && tt != t->guest_tt) {
        t->guest_tt = tt;
        G32(tt + TT_HANDLE) = t->kernel_handle;
    }
}
int hos_thread_prio(HosThread *t) { return t->priority; }

int hos_trace;
extern const uint64_t hos_thread_start_hook, hos_thread_exit_hook;   /* gen/hos_image.c */
static _Thread_local HosThread *cur;
static SRWLOCK threads_lock = SRWLOCK_INIT;
static HosThread *threads[1024];
static int nthreads;
static volatile LONG64 next_id = 0x50;

HosThread *hos_self(void) { return cur; }
Ctx *hos_ctx(void) { return cur ? (cur->run_ctx ? cur->run_ctx : &cur->ctx) : NULL; }
uint64_t hos_thread_guest(HosThread *t) { return t->guest_tt; }
uint64_t hos_thread_id(HosThread *t) { return t->id; }
const char *hos_thread_name(HosThread *t) { return t->name; }
uint64_t *hos_tls_slots(HosThread *t) { return t->tls_slots; }
/* The core a thread runs on. Most of TOTK's threads have no ideal core (-2, "use default") and a one-core affinity
   mask, so the mask decides; the engine keys per-core resources (its command memory arenas, for one) on
   GetCurrentCoreNumber, so threads pinned to different cores must report different ones. */
/* HOS_PIN_BY_CORE=1 (experiment): every game thread runs on the host's logical core 2 x its core number (one per
   physical core on SMT machines), so threads the game puts on the same core never run at the same time, as on the
   Switch, where TOTK keeps per-core data without locks (both MacNativeGames and totk-pc-recomp found it relies on
   that); they are still time-sliced at any instruction, not scheduled by priority as Horizon does. */
static void pin_by_core(HosThread *t) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    static int on = -1;
    if (on < 0) on = env_on("HOS_PIN_BY_CORE");
    if (on && t->handle) SetThreadAffinityMask(t->handle, (DWORD_PTR)1 << (2 * hos_thread_core(t)));
#else
    (void)t;
#endif
}
void hos_thread_affinity(HosThread *t, int ideal, uint64_t mask) {
    if (ideal >= 0) t->ideal_core = ideal;
    if (mask) t->core_mask = mask;
    pin_by_core(t);
}
uint64_t hos_thread_mask(HosThread *t) { return t->core_mask; }
void hos_thread_boost(int level) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    static int off = -1;
    if (off < 0) off = env_on("HOS_NO_PRIORITIES");
    if (!off) SetThreadPriority(GetCurrentThread(), level >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
#else
    (void)level;
#endif
}

int hos_cores(void) {
    static int n;
    if (!n) {
        const char *e = getenv("HOS_CORES");
        n = e && atoi(e) >= 1 && atoi(e) <= 8 ? atoi(e) : 6;   /* (mkw-recomp: a Switch 2 application's six) */
        if (!e && hos_game_cores()) n = hos_game_cores();   /* (totk_cores.c: the game set up for more cores) */
    }
    return n;
}
int hos_thread_core(HosThread *t) {
    if (!t) return 0;
    if (t->vc_held) return t->vc_held - 1;   /* (virtual cores: the one it holds) */
    const int n = hos_cores();
    if (t->ideal_core >= 0) return t->ideal_core % n;
    /* the mask's lowest core, wrapped (a thread pinned past the cores there are shares a core number: the extra
       BaseProc executors, totk_cores.c) */
    return t->core_mask ? (int)(__builtin_ctzll(t->core_mask) % (unsigned)n) : 0;
}
void hos_thread_stack(HosThread *t, uint64_t *addr, uint64_t *size) { *addr = t->stack; *size = t->stack_size; }

int hos_thread_priority(HosThread *t, int set, int value) {
    int old = t->priority;
    if (set) t->priority = value;
    return old;
}

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
static SRWLOCK log_lock = SRWLOCK_INIT, log_write_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE log_cv = CONDITION_VARIABLE_INIT;
static char *log_q[2048];
static int log_head, log_count;
static volatile LONG log_async = -1;
static volatile DWORD log_writer;   /* log_main's thread */

static int log_take_locked(char **out) {
    int n = 0;
    while (log_count) {
        out[n++] = log_q[log_head];
        log_head = (log_head + 1) % 2048;
        log_count--;
    }
    return n;
}

/* (hookshot) A batch goes out in one write. stderr is unbuffered (hos_main.c), so each line was a write of its own: 30-90 a
   second in play, every one extending the log file. With the game on a slow disk (a SATA SSD, 90% full) those writes kept
   the disk busy: its 4 KB reads took 10-200 ms where they take 0.4 ms, and what the game read from it meanwhile (a page of
   its own code, a file of the game's) waited as long: the long frames of a session. */
static void log_write(char **lines, int n) {
    size_t total = 0;
    for (int i = 0; i < n; i++) total += strlen(lines[i]);
    char *all = n > 1 ? (char *)malloc(total + 1) : NULL;
    if (all) {
        size_t at = 0;
        for (int i = 0; i < n; i++) {
            const size_t len = strlen(lines[i]);
            memcpy(all + at, lines[i], len);
            at += len;
            free(lines[i]);
        }
        fwrite(all, 1, total, stderr);
        free(all);
    } else {   /* (one line, or no memory for the batch: line by line) */
        for (int i = 0; i < n; i++) {
            fputs(lines[i], stderr);
            free(lines[i]);
        }
    }
    fflush(stderr);
}

/* (hookshot) and the writer waits a little after the line it is woken for, so the lines of that time go out together: a few
   writes a second however many lines there are. A queue half full goes out at once, before a burst can fill it (a full
   queue has the thread that logs write the line itself). HOS_LOG_BATCH_MS=<ms> (default 250, at most 2000; 0: the lines
   there are at each wake, as before). Nothing waiting is lost: every way out writes the queue first (log_flush,
   log_crash_filter), and the writer holds no lock while it waits */
enum { LOG_BATCH_FULL = 1024 };
static DWORD log_batch_ms(void) {
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("HOS_LOG_BATCH_MS");
        const int v = e && *e ? atoi(e) : 250;
        ms = v < 0 ? 0 : v > 2000 ? 2000 : v;
    }
    return (DWORD)ms;
}

static DWORD WINAPI log_main(void *p) {
    (void)p;
    static char *batch[2048];
    log_writer = GetCurrentThreadId();
    for (;;) {
        AcquireSRWLockExclusive(&log_lock);
        while (!log_count) SleepConditionVariableSRW(&log_cv, &log_lock, INFINITE, 0);
        /* (log_queue wakes it at each line: until the time is up or the queue half full) */
        const ULONGLONG until = GetTickCount64() + log_batch_ms();
        for (ULONGLONG now; log_count < LOG_BATCH_FULL && (now = GetTickCount64()) < until;)
            SleepConditionVariableSRW(&log_cv, &log_lock, (DWORD)(until - now), 0);
        const int n = log_take_locked(batch);
        AcquireSRWLockExclusive(&log_write_lock);
        ReleaseSRWLockExclusive(&log_lock);
        log_write(batch, n);
        ReleaseSRWLockExclusive(&log_write_lock);
    }
}

/* the queued lines straight to the handle (crash paths: no CRT stream, no write lock) */
static void log_drain_raw(void) {
    if (!TryAcquireSRWLockExclusive(&log_lock)) return;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    while (log_count) {
        const char *line = log_q[log_head];
        DWORD written;
        WriteFile(err, line, (DWORD)strlen(line), &written, NULL);
        log_head = (log_head + 1) % 2048;
        log_count--;
    }
    ReleaseSRWLockExclusive(&log_lock);
}

static void log_flush(void) {
    if (log_async <= 0) return;
    /* (a crash report on the writer itself, in its own write: it holds log_write_lock) */
    if (GetCurrentThreadId() == log_writer) { log_drain_raw(); return; }
    static char *batch[2048];
    AcquireSRWLockExclusive(&log_write_lock);
    AcquireSRWLockExclusive(&log_lock);
    const int n = log_take_locked(batch);
    ReleaseSRWLockExclusive(&log_lock);
    log_write(batch, n);
    ReleaseSRWLockExclusive(&log_write_lock);
}

static LPTOP_LEVEL_EXCEPTION_FILTER log_prev_filter;

static LONG WINAPI log_crash_filter(EXCEPTION_POINTERS *e) {
    if (GetCurrentThreadId() == log_writer) log_drain_raw();
    else {
        /* the writer may be in the middle of a batch: it finishes first (at most 2 s: a write stuck on the disk), then the
           rest of the queue goes out (before: the queue was left unwritten whenever the writer was busy) */
        int got = 0;
        for (int i = 0; i < 2000 && !(got = TryAcquireSRWLockExclusive(&log_write_lock)); i++) Sleep(1);
        log_drain_raw();
        if (got) ReleaseSRWLockExclusive(&log_write_lock);
    }
    return log_prev_filter ? log_prev_filter(e) : EXCEPTION_CONTINUE_SEARCH;
}

/* Lines are written by log_main, not by the thread that logs: a write that blocks (a console; the log file while the
   disk is busy: the render thread was seen held 140-310 ms in one line's write) holds no game thread. Nothing queued is
   lost when the game ends or crashes: hos_fatal, report_crash/report_fatal (mkw_glue.c), log_crash_filter,
   svcExitProcess, the window's close and the process's exit (atexit) write the queue first. HOS_LOG_SYNC=1: each line
   written by the thread that logs it, as files and pipes were before */
static int log_queue(const char *line) {
    if (log_async < 0) {
        AcquireSRWLockExclusive(&log_lock);
        if (log_async < 0) {
            int on = !env_on("HOS_LOG_SYNC");
            if (on && !CreateThread(NULL, 0, log_main, NULL, 0, NULL)) on = 0;
            if (on) log_prev_filter = SetUnhandledExceptionFilter(log_crash_filter);
            if (on) atexit(log_flush);
            log_async = on;
        }
        ReleaseSRWLockExclusive(&log_lock);
    }
    if (!log_async) return 0;
    char *s = _strdup(line);
    if (!s) return 0;
    AcquireSRWLockExclusive(&log_lock);
    if (log_count < 2048) {
        log_q[(log_head + log_count) % 2048] = s;
        log_count++;
        ReleaseSRWLockExclusive(&log_lock);
        WakeConditionVariable(&log_cv);
        return 1;
    }
    ReleaseSRWLockExclusive(&log_lock);
    free(s);
    log_flush();
    return 0;
}
#else
static int log_queue(const char *line) { (void)line; return 0; }
static void log_flush(void) {}
#endif

void hos_log_flush(void) { log_flush(); }

void hos_log(const char *fmt, ...) {
    char buf[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "[hos %s] %s\n", cur ? cur->name : "host", buf);
    extern volatile int hos_control_on;   /* (control.c: log lines for its subscribers) */
    if (hos_control_on) {
        void hos_control_log(const char *line);
        hos_control_log(line);
    }
    if (log_queue(line)) return;
    fputs(line, stderr);
    fflush(stderr);
}

void hos_fatal(Ctx *c, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!c) c = hos_ctx();
    log_flush();
    fprintf(stderr, "[hos %s] FATAL: %s", cur ? cur->name : "host", buf);
    if (c) fprintf(stderr, " (lr %#llx sp %#llx)", (unsigned long long)c->x[30], (unsigned long long)c->x[31]);
    fprintf(stderr, "\n");
    fflush(stderr);
    void hos_backtrace(void), hos_hostcall_stats(void);
    hos_backtrace();
    hos_hostcall_stats();
    ExitProcess(3);
}

/* Traps from the lifted code (rt/a64_rt.c): name the thread, then stop. */
void a64_on_trap(Ctx *c, uint64_t pc, uint32_t insn, const char *why) {
    hos_fatal(c, "%s at %#llx (insn %08x)", why, (unsigned long long)pc, insn);
}

static void setup_guest(HosThread *t) {
    t->tls_block = hos_sys_alloc(0x1000, 0x1000);
    t->tlr = hos_sys_alloc(0x200, 0x200);
    G64(t->tlr + 0x1F8) = t->guest_tt;
    t->kernel_handle = (uint32_t)InterlockedIncrement(&next_handle);
    G32(t->guest_tt + TT_HANDLE) = t->kernel_handle;
    /* (hookshot) SDK 22's InternalCriticalSection::IsLockedByCurrentThread reads the owner handle from TLR+0x110
       directly (SDK 21: ThreadType+0x1B0 through TLR+0x1F8). Left 0, every free mutex looked held by the caller
       and the first heap lock aborted in pthread_mutex_lock. */
    G32(t->tlr + 0x110) = t->kernel_handle;
    t->wake = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset: one signal, one wake-up */
    Ctx *c = &t->ctx;
    memset(c, 0, sizeof *c);
    c->tpidr_el0 = t->tls_block + 0x800;
    c->tpidrro_el0 = t->tlr;
    c->thread = t;
}

static void track(HosThread *t) {
    AcquireSRWLockExclusive(&threads_lock);
    if (nthreads < 1024) threads[nthreads++] = t;
    ReleaseSRWLockExclusive(&threads_lock);
}

/* Host handles of every running guest thread, for the profiler (hos_prof.c). */
int hos_thread_handles(HANDLE *out, const char **names, int max) {
    int n = 0;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads && n < max; i++)
        if (threads[i]->handle) { out[n] = threads[i]->handle; names[n] = threads[i]->name; n++; }
    ReleaseSRWLockShared(&threads_lock);
    return n;
}

/* (mkw-recomp) Every guest thread's name and guest registers as of its last call out of lifted code, with its
   frame chain from x29: what each thread of a stalled game waits in (mkw_glue.c, HOS_STATS_AFTER). */
void hos_thread_dump(void) {
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads; i++) {
        const Ctx *c = &threads[i]->ctx;
        char bt[400];
        int n = 0;
        uint64_t fp = c->x[29];
        for (int k = 0; k < 12 && fp && !(fp & 15) && fp < HOS_GUEST_SPACE && n < (int)sizeof bt - 24; k++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(GP(fp), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)G64(fp + 8));
            const uint64_t next = G64(fp);
            if (next <= fp) break;
            fp = next;
        }
        bt[n] = 0;
        hos_log("thread %-24s lr %#llx sp %#llx x0 %#llx; frames%s", threads[i]->name, (unsigned long long)c->x[30],
                (unsigned long long)c->x[31], (unsigned long long)c->x[0], bt);
        if (hos_game_is_mkw() && c->x[30] == 0x825c6844ULL) continue;   /* (MKW's idle thread loop) */
        /* registers (current at the call out), with the text a register (or the word it points to) leads to */
        for (int r = 0; r < 29; r++) {
            uint64_t v = c->x[r], s = 0;
            MEMORY_BASIC_INFORMATION mbi;
            for (int hop = 0; hop < 2 && v > 0x100000 && v < HOS_GUEST_SPACE - 64; hop++) {
                if (!VirtualQuery(GP(v), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                const uint8_t *p = (const uint8_t *)GP(v);
                int k = 0;
                while (k < 48 && p[k] >= 0x20 && p[k] < 0x7f) k++;
                if (k >= 6 && (p[k] == 0 || k == 48)) { s = v; break; }
                v = G64(v);
            }
            if (s) hos_log("thread %s:   x%d %#llx -> \"%.48s\"", threads[i]->name, r, (unsigned long long)c->x[r],
                           (const char *)GP(s));
        }
    }
    ReleaseSRWLockShared(&threads_lock);
}

/* HOS_FP_REPORT=1 (debugging): the FPCR / FPSR of the busiest game threads, with the thread stats */
int hos_thread_fp_report(char *out, int cap) {
    int len = 0;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads && len < cap - 64; i++) {
        const char *nm = threads[i]->name;
        if (strstr(nm, "Main") || strstr(nm, "Worker") || strstr(nm, "Executor"))
            len += snprintf(out + len, cap - len, " %s fpcr %llx fpsr %llx;", nm,
                            (unsigned long long)threads[i]->ctx.fpcr, (unsigned long long)threads[i]->ctx.fpsr);
    }
    ReleaseSRWLockShared(&threads_lock);
    return len;
}

HosThread *hos_thread_by_handle(uint32_t h) {
    HosThread *r = NULL;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->kernel_handle == h) { r = threads[i]; break; }
    ReleaseSRWLockShared(&threads_lock);
    return r;
}

/* Block on the thread's wake event (the kernel layer's lock must not be held). */
/* (HOS_CHAIN: the kernel's waits, svc mutexes, condition variables and the address arbiter, end in a wake of the
   thread itself: one key per thread, bit 62: never a guest address) */
static uint64_t wake_chain_key(HosThread *t) { return 1ULL << 62 | (uint64_t)(uintptr_t)t; }
void hos_thread_wait(HosThread *t, int64_t timeout_ns, int *timed_out) {
    DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)((timeout_ns + 999999) / 1000000);
    HOS_CHAIN(HOS_CHAIN_WAIT, wake_chain_key(t));
    const int away = hos_vcore_away();
    *timed_out = WaitForSingleObject(t->wake, ms) == WAIT_TIMEOUT;
    if (away) hos_vcore_back();
    HOS_CHAIN(HOS_CHAIN_WOKE, wake_chain_key(t));
}
void hos_thread_wake(HosThread *t) {
    HOS_CHAIN(HOS_CHAIN_SIGNAL, wake_chain_key(t));
    SetEvent(t->wake);
}

HosThread *hos_thread_of(uint64_t guest_tt) {
    HosThread *r = NULL;
    AcquireSRWLockShared(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->guest_tt == guest_tt) { r = threads[i]; break; }
    ReleaseSRWLockShared(&threads_lock);
    return r;
}

Ctx *hos_boot_main_thread(uint64_t stack_size) {
    HosThread *t = (HosThread *)calloc(1, sizeof *t);
    t->guest_tt = hos_sys_alloc(0x400, 0x10);      /* a ThreadType for the main thread; the SDK never made one */
    t->stack_size = stack_size;
    t->stack = hos_sys_alloc(stack_size, 0x1000);
    t->id = InterlockedIncrement64(&next_id);
    t->priority = 44;                              /* main thread priority in TOTK's NPDM range */
    t->ideal_core = 0;
    t->started = 1;
    snprintf(t->name, sizeof t->name, "MainThread");
    __atomic_store_n(&t->serial, 1, __ATOMIC_RELAXED);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);   /* a real handle, for the profiler */
    SetThreadDescription(GetCurrentThread(), L"MainThread");   /* (HOS_SAMPLE finds threads by it) */
    hos_thread_pin(t->handle, t->name);
    setup_guest(t);
    t->ctx.x[31] = (t->stack + stack_size) & ~15ULL;
    cur = t;
    track(t);
    hos_vcore_back();   /* (virtual cores: before any guest code) */
    return &t->ctx;
}

static DWORD WINAPI thread_main(void *p) {
    HosThread *t = (HosThread *)p;
    cur = t;
    Ctx *c = &t->ctx;
    c->x[0] = t->arg;
    c->x[30] = 0;
    c->x[31] = (t->stack + t->stack_size) & ~15ULL;
    A64Fn f = a64_lookup(t->entry);
    if (!f) hos_fatal(c, "thread %s: entry %#llx is not a function", t->name, (unsigned long long)t->entry);
    hos_vcore_back();   /* (virtual cores: before any guest code) */
    if (hos_trace) hos_log("thread start: entry %#llx arg %#llx", (unsigned long long)t->entry, (unsigned long long)t->arg);
    /* What the SDK's own thread entry does before the thread function (see build.py thread_hooks): nnmusl's start
       hook gives the thread its TLS (thread pointer with the DTV at TP+8, every module's TLS block) and leaves the
       thread pointer at TLR+0x1F0. Without it every thread would share one DTV, and so every thread_local. */
    uint64_t hook = hos_thread_start_hook ? G64(hos_thread_start_hook) : 0;
    if (hook) {
        c->x[0] = t->guest_tt;
        a64_icall(c, hook);
        if (G64(t->tlr + 0x1F0)) c->tpidr_el0 = G64(t->tlr + 0x1F0);
        if (hos_trace || env_on("HOS_TLS_TRACE"))
            hos_log("thread %s: TLS hook gave TP %#llx, DTV %#llx; ideal core %d mask %#llx priority %d", t->name,
                    (unsigned long long)c->tpidr_el0, (unsigned long long)G64(c->tpidr_el0 + 8), t->ideal_core,
                    (unsigned long long)t->core_mask, t->priority);
        c->x[31] = (t->stack + t->stack_size) & ~15ULL;
    }
    c->x[0] = t->arg;
    c->x[30] = 0;
    f(c);
    hook = hos_thread_exit_hook ? G64(hos_thread_exit_hook) : 0;
    if (hook) {
        c->x[0] = t->guest_tt;
        a64_icall(c, hook);
    }
    if (hos_trace) hos_log("thread exit");
    hos_vcore_away();
    return 0;
}

HosThread *hos_thread_create(uint64_t guest_tt, uint64_t entry, uint64_t arg, uint64_t stack, uint64_t stack_size,
                             int priority, int core) {
    HosThread *t = (HosThread *)calloc(1, sizeof *t);
    t->guest_tt = guest_tt;
    t->entry = entry;
    t->arg = arg;
    t->stack = stack;
    t->stack_size = stack_size;
    t->priority = priority;
    t->ideal_core = core;
    t->id = InterlockedIncrement64(&next_id);
    snprintf(t->name, sizeof t->name, "Thread_%llx", (unsigned long long)entry);
    setup_guest(t);   /* FPCR starts at 0, as the kernel does it; TOTK sets FZ itself (nnMain+0x474, thread entries) */
    track(t);
    return t;
}

void hos_thread_start(HosThread *t) {
    if (t->started) return;
    t->started = 1;
    /* Host stack: lifted code nests a C call per guest call, so give it plenty. */
    t->handle = CreateThread(NULL, 64 << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->handle) hos_fatal(NULL, "CreateThread failed for %s", t->name);
    if (t->name[0]) {   /* (named before it started: the OS name too, for profilers and HOS_SAMPLE) */
        wchar_t w[48];
        MultiByteToWideChar(CP_UTF8, 0, t->name, -1, w, 48);
        SetThreadDescription(t->handle, w);
    }
    hos_thread_pin(t->handle, t->name);
    pin_by_core(t);
#ifdef __EMSCRIPTEN__
    { void web_thread_named(HANDLE t, const char *name); web_thread_named(t->handle, t->name); }
#endif
}

void hos_thread_join(HosThread *t) {
    if (!t->handle) return;
    const int away = hos_vcore_away();
    /* (EvilOOT, 11-07: a freeze after Load, one thread joining another for good: a join of over 10 s says who joins whom, once) */
    if (WaitForSingleObject(t->handle, 10000) == WAIT_TIMEOUT) {
        HosThread *self = hos_self();
        hos_log("thread %s: joining %s for 10 s", self ? hos_thread_name(self) : "?", t->name);
        WaitForSingleObject(t->handle, INFINITE);
    }
    if (away) hos_vcore_back();
}

void hos_thread_destroy(HosThread *t) {
    AcquireSRWLockExclusive(&threads_lock);
    for (int i = 0; i < nthreads; i++)
        if (threads[i] == t) { threads[i] = threads[--nthreads]; break; }
    ReleaseSRWLockExclusive(&threads_lock);
    if (t->handle) CloseHandle(t->handle);
    if (t->wake) CloseHandle(t->wake);
    free(t);
}

/* The frame's serial threads: OoT's game thread (sead::MainThread, the boot thread's MainThread before the game names
   it) and its Presentation Thread. A frame waits on each in turn, so a wait of theirs that ends late ends the frame late;
   the host keeps their short waits short (sleep_ns's pollers, the spin before a wait sleeps: hos_spin_ns). By name, as
   the game names them: kept in the thread's record when it is named, not read from the name while it may change. */
static int serial_name(const char *name) { return strstr(name, "MainThread") || !strcmp(name, "Presentation Thread"); }
int hos_thread_serial(const HosThread *t) { return t && __atomic_load_n(&t->serial, __ATOMIC_RELAXED); }

/* The processors this process may run on (its affinity mask, as the game starts with it): Windows with one processor
   group; 0 = not known (more than one group, or elsewhere). Up to SHORT_SLEEP_TIMER_THREADS of them, the host keeps the
   behaviour measured best there (sleep_ns's timer, hos_spin_ns's one spin). */
enum { SHORT_SLEEP_TIMER_THREADS = 6 };
static int process_threads(void) {
    int threads = 0;
#if defined(_WIN32)   /* (measured on Windows only) */
    /* (with more than one processor group the mask is one group's, not the process's: not known then) */
    DWORD_PTR proc = 0, sys = 0;
    if (GetActiveProcessorGroupCount() == 1 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        threads = __builtin_popcountll((uint64_t)proc);
#endif
    return threads;
}

/* How long a wait spins before it sleeps (nn_os_sync.c's events, queues and mutexes; hos_svc.c's mutexes), in ns, for the
   calling thread: HOS_SPIN_US (20 us) on the frame's serial threads, HOS_SPIN_OTHER_US (5 us, never more than HOS_SPIN_US)
   on the others, where there are more than SHORT_SLEEP_TIMER_THREADS processors. With 6 or fewer (or not known), every
   thread keeps HOS_SPIN_US unless HOS_SPIN_OTHER_US is set: there the short spin saved no CPU and cost 1.6 % fps (6 pairs,
   Hyrule, 6 of 16 processors), as a 24-thread run with only 2 pollers did (1.7 %). A sleeper runs again 6.6-8.4 us after its signal (HOS_WAKE_LAT, Hyrule), so a spin much longer than that
   mostly burns its thread's time: 90 % of the spin's 15 % of a core in Hyrule was on the job workers and the actor
   executors. 5 us for every thread cut the process's CPU a frame 3.3 % at the same fps (6 pairs, Hyrule, 16 threads); the
   serial threads keep 20 us, as their wait ends are on the frame's path. HOS_SPIN_OTHER_US=20: as before, one spin for all;
   HOS_SPIN_US=0: no spin. */
int64_t hos_spin_ns(void) {
    static int64_t both = -1;   /* (the serial threads' ns << 32 | the others': one atomic word, chosen once) */
    int64_t b = __atomic_load_n(&both, __ATOMIC_RELAXED);
    if (b < 0) {
        const char *e = getenv("HOS_SPIN_US"), *o = getenv("HOS_SPIN_OTHER_US");
        int64_t s = e ? atoll(e) : 20, other = o ? atoll(o) : process_threads() > SHORT_SLEEP_TIMER_THREADS ? 5 : s;   /* (us, clamped before they become ns) */
        s = (s < 0 ? 0 : s > 1000000 ? 1000000 : s) * 1000;
        other = (other < 0 ? 0 : other > 1000000 ? 1000000 : other) * 1000;
        other = other > s ? s : other;
        b = s << 32 | other;
        __atomic_store_n(&both, b, __ATOMIC_RELAXED);
    }
    return hos_thread_serial(hos_self()) ? b >> 32 : b & 0xFFFFFFFF;
}

void hos_thread_set_name(HosThread *t, const char *name) {
    snprintf(t->name, sizeof t->name, "%s", name);
    __atomic_store_n(&t->serial, serial_name(t->name), __ATOMIC_RELAXED);   /* (atomic: SetThreadName may name another thread) */
    if (t->handle) {
        wchar_t w[48];
        MultiByteToWideChar(CP_UTF8, 0, t->name, -1, w, 48);
        SetThreadDescription(t->handle, w);
        hos_thread_pin(t->handle, t->name);
    }
}

/* HOS_PIN=<name part>=<hex mask>,...[,*=<hex mask>] (experiment): host CPU affinity by thread name (game threads as
   they are named or started, host threads as they register); "*" for every other named thread. The first matching
   entry wins. (2026-10-20: the castle's MainThread and ModuleSystem workers each on a physical core of their own, the
   rest elsewhere, was slower than Windows' own scheduling: 63-68 against 69-71 fps.) */
/* HOS_PRIO=<name part>=<level>,... (experiment, same matching): host thread priority, -2 (lowest) to 2 (highest).
   (10-20: MainThread, ModuleSystem workers and BaseProc executors above normal: no change at the castle) */
typedef struct { char pat[40]; long long value; } NameSpec;
static int parse_name_specs(const char *e, NameSpec *out, int max, int base) {
    int n = 0;
    while (e && *e && n < max) {
        const char *eq = strchr(e, '='), *end = strchr(e, ',');
        if (!end) end = e + strlen(e);
        if (!eq || eq > end) break;
        snprintf(out[n].pat, sizeof out[n].pat, "%.*s", (int)(eq - e), e);
        out[n++].value = strtoll(eq + 1, NULL, base);
        e = *end ? end + 1 : end;
    }
    return n;
}
static const NameSpec *match_name_spec(const NameSpec *s, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(s[i].pat, "*") ? strstr(name, s[i].pat) != NULL : 1) return &s[i];
    return NULL;
}
/* The last physical core (its logical processors' mask), kept for the Vulkan back end's replay thread (gpu/vk
   vk_replay.cpp pins it there): the game's threads run on the others. The replay is on the frame's critical path
   (MKW waits for the last frame's GPU work before it submits the next), and on a core shared with game threads
   it ran ~1.7x slower than alone. Only on machines with 8+ logical processors; HOS_RESERVE_CORE=0: none.
   (hookshot) On a CPU with cores of more than one kind, the last core of the fastest kind: the last core of all is an
   efficiency core on Intel's hybrid CPUs (an i7-12700K's logical processors 16-19), the slowest place for the replay
   (an i7-13700K's Kokiri Forest: 48.7 fps with it there, 60 on a performance core). EfficiencyClass is higher the faster
   the core, and the same for every core of a CPU of one kind (the last core, as before). Only with 4+ cores of the
   fastest kind (8+ logical processors always have them on a CPU of one kind): a laptop's 2 performance cores and 8
   efficiency ones (i5-1235U) would keep one of the two from the game; then none is kept. The calling thread's
   processor group's cores: the masks the affinity calls take are its group's. On Linux the same choice from
   /sys/devices/cpu_core/cpus (Intel's hybrid CPUs; last_fastest_core below); without that file, the last core, as before. */
/* (hookshot) The cores that share the largest L3, on a CPU whose cores do not all share one of the same size: AMD's X3D CPUs
   with two CCDs (7900X3D, 7950X3D, 9900X3D, 9950X3D) have the 3D V-Cache on one CCD only (96 MB against 32 MB). Windows
   moves a game there only when its Game Bar knows the program as a game; this one it does not know, so its threads ran on
   both, and the replay's reserved core (the last one) is on the CCD without the cache. On a 9950X3D (SMT off), Castle Town's
   market (2,300 draws): the replay 11.5-12.3 ms a frame and 57-58 fps as it was (the slowest two seconds 54), 10.1-10.5 ms and
   59.9 fps (58.7) with the game's threads kept to the first CCD. So they are kept to those cores. 0 for none: one L3 for all, L3s
   of one size (a 7950X, both CCDs alike), L3s that do not all share as many logical processors (a hybrid CPU's two kinds of
   core: a Ryzen AI 9 HX 370 has 4 Zen 5 cores on a 16 MB L3 and 8 Zen 5c on an 8 MB one, and the game is not squeezed onto the 4;
   an X3D's CCDs are alike but for the cache), or fewer than 8 cores on the largest (the 12-core 7900X3D and 9900X3D have 6 there:
   not measured, left as they were). Measured with SMT off only: with it the 8 cores are 16 logical processors.
   HOS_CACHE_CORES=0: none. Windows only, as the reserved core's choice above: Linux keeps the last core, which on these CPUs is
   on the CCD without the cache.
   (the choice: each L3's logical processor mask and size, n of them, and each core's mask, ncores of them.
   tests/test_reserved_core.py) */
static uint64_t pick_cache_cores(const uint64_t *masks, const uint64_t *sizes, int n, const uint64_t *cores, int ncores) {
    uint64_t best = 0, most = 0, least = ~0ull;
    int width = 0, alike = 1;   /* (how many logical processors an L3 is shared by, and whether all are shared by as many) */
    for (int i = 0; i < n; i++) {
        if (!masks[i] || !sizes[i]) continue;
        if (!width) width = __builtin_popcountll(masks[i]);
        else if (__builtin_popcountll(masks[i]) != width) alike = 0;
        if (sizes[i] > most) most = sizes[i], best = masks[i];
        else if (sizes[i] == most) best |= masks[i];   /* (several of the largest size: all of them) */
        if (sizes[i] < least) least = sizes[i];
    }
    if (!best || least == most || !alike) return 0;
    int physical = 0;
    for (int i = 0; i < ncores; i++) physical += (cores[i] & best) != 0;
    if (physical < 8) return 0;
    return best;
}
/* (the choice: the cores' logical processor masks and EfficiencyClass, n of them; 0 for none. tests/test_reserved_core.py) */
static uint64_t pick_reserved_core(const uint64_t *masks, const unsigned char *classes, int n) {
    uint64_t last = 0;
    int best = -1, fast = 0, logical = 0;
    for (int i = 0; i < n; i++) {
        if (!masks[i]) continue;
        logical += __builtin_popcountll(masks[i]);
        if ((int)classes[i] > best) {
            best = classes[i], last = masks[i], fast = 1;
        } else if ((int)classes[i] == best) {
            fast++;
            if (masks[i] > last) last = masks[i];
        }
    }
    if (logical < 8 || fast < 4) return 0;

    // One-time debug logging
    static bool logged = false;
    if (!logged) {
        logged = true;
        hos_log("vk: reserved core selection: %d cores, %d logical, best class=%d, fast cores=%d, selected=%#llx",
                n, logical, best, fast, (unsigned long long)last);
        for (int i = 0; i < n; i++) {
            if (masks[i]) {
                hos_log("vk:   core %d: mask=%#llx, class=%d, popcount=%d",
                        i, (unsigned long long)masks[i], classes[i], __builtin_popcountll(masks[i]));
            }
        }
    }
    return last;
}
#if defined(_WIN32)
/* (pick_cache_cores over the calling thread's group's L3 caches; 0: no such split, or HOS_CACHE_CORES=0) */
uint64_t hos_cache_cpus(void) {
    static uint64_t mask = ~0ull;
    if (mask != ~0ull) return mask;
    mask = 0;
    if (getenv("HOS_CACHE_CORES") && !atoi(getenv("HOS_CACHE_CORES"))) return mask;
    static uint64_t masks[64], sizes[64], cores[256];
    DWORD len = 0;
    int n = 0, ncores = 0;
    GROUP_AFFINITY ga;
    const WORD group = GetThreadGroupAffinity(GetCurrentThread(), &ga) ? ga.Group : 0;
    if (GetLogicalProcessorInformationEx(RelationCache, NULL, &len) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return mask;
    char *buf = (char *)malloc(len);
    if (buf && GetLogicalProcessorInformationEx(RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len))
        for (DWORD at = 0; at < len && n < 64;) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + at);
            if (!e->Size) break;
            if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == group) {
                masks[n] = (uint64_t)e->Cache.GroupMask.Mask;
                sizes[n] = e->Cache.CacheSize;
                n++;
            }
            at += e->Size;
        }
    free(buf);
    len = 0;   /* (and the group's cores: how many of them the largest L3 has) */
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return mask;
    buf = (char *)malloc(len);
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len))
        for (DWORD at = 0; at < len && ncores < 256;) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + at);
            if (!e->Size) break;
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount && e->Processor.GroupMask[0].Group == group)
                cores[ncores++] = (uint64_t)e->Processor.GroupMask[0].Mask;
            at += e->Size;
        }
    free(buf);
    mask = pick_cache_cores(masks, sizes, n, cores, ncores);
    if (mask) {
        uint64_t most = 0, least = ~0ull;
        for (int i = 0; i < n; i++) {
            if (sizes[i] > most) most = sizes[i];
            if (sizes[i] && sizes[i] < least) least = sizes[i];
        }
        hos_log("threads: the game's on logical processors %#llx, the cores that share the largest L3 (%llu MB; the others' is "
                "%llu MB). HOS_CACHE_CORES=0: every core", (unsigned long long)mask, (unsigned long long)(most >> 20),
                (unsigned long long)(least >> 20));
    }
    return mask;
}
/* (pick_reserved_core over the calling thread's group's cores; *cores: how many, 0 when they could not be read) */
static uint64_t last_fastest_core_legacy(int *cores) {
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[256];
    DWORD len = sizeof info;
    if (!GetLogicalProcessorInformation(info, &len)) {
        hos_log("vk: GetLogicalProcessorInformation failed: %lu", GetLastError());
        return 0;
    }

    int logical = 0;
    int current_core = 0;
    uint64_t masks[256];
    unsigned char classes[256];

    int pcore_candidates = 0;
    int ecore_candidates = 0;

    for (DWORD i = 0; i < len / sizeof info[0] && current_core < 256; i++) {
        if (info[i].Relationship == RelationProcessorCore) {
            uint64_t mask = (uint64_t)info[i].ProcessorMask;
            int popcount = __builtin_popcountll(mask);
            logical += popcount;

            // Heuristic: cores with 2+ logical processors = P-cores (with HT)
            // cores with 1 logical processor = E-cores (no HT)
            if (popcount >= 2) {
                classes[current_core] = 1;  // P-core
                pcore_candidates++;
            } else {
                classes[current_core] = 0;  // E-core
                ecore_candidates++;
            }
            masks[current_core] = mask;
            current_core++;
        }
    }

    *cores = current_core;

    hos_log("vk: legacy API fallback: %d cores, %d logical, P-candidates=%d, E-candidates=%d",
            current_core, logical, pcore_candidates, ecore_candidates);

    // If mixed core types detected, use enhanced picker
    if (pcore_candidates > 0 && ecore_candidates > 0) {
        hos_log("vk: Intel hybrid CPU detected via legacy API heuristic");
        return pick_reserved_core(masks, classes, current_core);
    }

    // Single CPU type - use original logic (last core)
    uint64_t last = 0;
    for (int i = 0; i < current_core; i++) {
        if ((uint64_t)masks[i] > last) last = (uint64_t)masks[i];
    }
    if (logical >= 8) return last;
    return 0;
}

/* (pick_reserved_core over the calling thread's group's cores; *cores: how many, 0 when they could not be read) */
static uint64_t last_fastest_core(int *cores) {
    static uint64_t masks[256];
    static unsigned char classes[256];
    DWORD len = 0;
    int n = 0;
    *cores = 0;
    GROUP_AFFINITY ga;
    const WORD group = GetThreadGroupAffinity(GetCurrentThread(), &ga) ? ga.Group : 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        hos_log("vk: GetLogicalProcessorInformationEx failed: %lu", GetLastError());
        return last_fastest_core_legacy(cores);
    }
    char *buf = (char *)malloc(len);
    if (!buf) {
        hos_log("vk: failed to allocate buffer for processor info");
        return last_fastest_core_legacy(cores);
    }

    bool success = false;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
        for (DWORD at = 0; at < len && n < 256;) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + at);
            if (!e->Size) break;
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount && e->Processor.GroupMask[0].Group == group) {
                masks[n] = (uint64_t)e->Processor.GroupMask[0].Mask;
                classes[n] = e->Processor.EfficiencyClass;
                n++;
            }
            at += e->Size;
        }
        success = true;
    }

    free(buf);

    if (!success || n == 0) {
        hos_log("vk: extended API failed or no cores found, trying legacy API with Intel hybrid detection");
        return last_fastest_core_legacy(cores);
    }

    *cores = n;
    return pick_reserved_core(masks, classes, n);
}
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
/* (hookshot) A Linux cpulist ("0-7", "0-11,16-19") as a mask of logical processors 0-63; 0 when it does not parse.
   (tests/test_reserved_core.py) */
static uint64_t parse_cpu_list(const char *s) {
    uint64_t mask = 0;
    while (*s && *s != '\n') {
        char *end;
        long a = strtol(s, &end, 10), b = a;
        if (end == s || a < 0) return 0;
        s = end;
        if (*s == '-') {
            b = strtol(++s, &end, 10);
            if (end == s || b < a) return 0;
            s = end;
        }
        for (long i = a; i <= b && i < 64; i++) mask |= 1ull << i;
        if (*s == ',') s++;
        else if (*s && *s != '\n') return 0;
    }
    return mask;
}
/* (hookshot) Linux: Windows' EfficiencyClass has no file of its own, but on Intel's hybrid CPUs the kernel lists the
   performance cores' logical processors in /sys/devices/cpu_core/cpus (the hybrid PMU; the efficiency cores are in
   cpu_atom). Without it the last core was kept, and on those CPUs that is an efficiency core: a Core Ultra 7 265KF's
   logical processor 19 (P-cores 0-7, E-cores 8-19). Those cores are class 1, the others 0, and pick_reserved_core
   chooses as on Windows. *cores: 0 when the file is not there (a CPU of one kind, AMD, an older kernel): as before. */
static uint64_t last_fastest_core(int *cores) {
    *cores = 0;
    char line[256] = {0};
    FILE *f = fopen("/sys/devices/cpu_core/cpus", "r");
    if (!f) return 0;
    const bool read = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    const uint64_t fast = read ? parse_cpu_list(line) : 0;
    if (!fast) return 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[64];
    DWORD len = sizeof info;
    if (!GetLogicalProcessorInformation(info, &len)) return 0;
    uint64_t masks[64];
    unsigned char classes[64];
    int n = 0;
    for (DWORD i = 0; i < len / sizeof info[0] && n < 64; i++)
        if (info[i].Relationship == RelationProcessorCore) {
            masks[n] = (uint64_t)info[i].ProcessorMask;
            classes[n] = (masks[n] & fast) != 0;
            n++;
        }
    *cores = n;
    return pick_reserved_core(masks, classes, n);
}
#endif
uint64_t hos_reserved_cpus(void) {
    static uint64_t mask = ~0ull;
    if (mask != ~0ull) return mask;
    mask = 0;

    // Allow explicit hex mask override for testing
    const char* override = getenv("HOS_RESERVE_CORE_MASK");
    if (override) {
        char* endptr;
        mask = strtoull(override, &endptr, 16);
        if (*endptr != '\0') {
            hos_log("vk: HOS_RESERVE_CORE_MASK '%s' is not a valid hex value, ignoring", override);
            mask = 0;
        } else {
            hos_log("vk: HOS_RESERVE_CORE_MASK override: %#llx", (unsigned long long)mask);
        }
        return mask;
    }

    if (getenv("HOS_RESERVE_CORE") && !atoi(getenv("HOS_RESERVE_CORE"))) {
        hos_log("vk: HOS_RESERVE_CORE=0, no core reserved");
        return mask;
    }
#if defined(_WIN32)
    /* (hookshot) none where the game is kept to the cores of the largest L3 (hos_cache_cpus): they are 8 cores on the CPUs
       measured (a 9950X3D, SMT off), and with one of them kept for the replay the game's threads had 7. In the market the replay
       then took 9.8-10.0 ms a frame but the frame rate stayed at 56.9-57.4; with the 8 shared, 10.1-10.5 ms and 59.9 fps.
       HOS_RESERVE_CORE=1: the last core of the fastest kind is kept all the same, as on other CPUs. That is normally a core of
       the other CCD, not the cache core of the 9.8-10.0 ms above, and the game's threads then have all of the cache cores */
    if (hos_cache_cpus() && !(getenv("HOS_RESERVE_CORE") && atoi(getenv("HOS_RESERVE_CORE")))) return mask;
    {
        int cores = 0;
        const uint64_t fast = last_fastest_core(&cores);
        if (cores) {
            mask = fast;
            hos_log("vk: reserved core mask (enhanced): %#llx (%d cores)", (unsigned long long)mask, cores);
            return mask;
        }
    }
#elif !defined(__EMSCRIPTEN__)
    {
        int cores = 0;
        const uint64_t fast = last_fastest_core(&cores);
        if (cores) {
            mask = fast;
            hos_log("vk: reserved core mask (hybrid, /sys/devices/cpu_core): %#llx (%d cores)", (unsigned long long)mask, cores);
            return mask;
        }
    }
#endif
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[256];
    DWORD len = sizeof info;
    if (!GetLogicalProcessorInformation(info, &len)) return mask;
    int logical = 0;
    uint64_t last = 0;
    for (DWORD i = 0; i < len / sizeof info[0]; i++)
        if (info[i].Relationship == RelationProcessorCore) {
            logical += __builtin_popcountll((uint64_t)info[i].ProcessorMask);
            if ((uint64_t)info[i].ProcessorMask > last) last = (uint64_t)info[i].ProcessorMask;
        }
    if (logical >= 8) mask = last;
    hos_log("vk: reserved core mask (fallback): %#llx", (unsigned long long)mask);
    return mask;
}

void hos_thread_pin(void *h, const char *name) {
    const uint64_t reserved = hos_reserved_cpus();
#if defined(_WIN32)
    const uint64_t cache = hos_cache_cpus();   /* (hookshot: the cores of the largest L3, when the CPU's differ) */
#else
    const uint64_t cache = 0;
#endif
    if (reserved || cache) {
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        uint64_t may = (uint64_t)proc;
        if (cache && (may & cache)) may &= cache;
        if (may & ~reserved) SetThreadAffinityMask(h, (DWORD_PTR)(may & ~reserved));
    }
    static NameSpec pins[24], prios[24];
    static int npins = -1, nprios;
    static SRWLOCK lk = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lk);
    if (npins < 0) {
        npins = parse_name_specs(getenv("HOS_PIN"), pins, 24, 16);
        nprios = parse_name_specs(getenv("HOS_PRIO"), prios, 24, 10);
    }
    ReleaseSRWLockExclusive(&lk);
    const NameSpec *p = match_name_spec(pins, npins, name);
    if (p && p->value) SetThreadAffinityMask(h, (DWORD_PTR)p->value);
    static const int levels[5] = {THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
                                  THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST};
    if ((p = match_name_spec(prios, nprios, name)) && p->value >= -2 && p->value <= 2)
        SetThreadPriority(h, levels[p->value + 2]);
    else if (strstr(name, "OptimizeChange") && !env_on("HOS_OCB_NORMAL")) {
        /* (OoT's OptimizeChangeBank (guest priority 27, a background thread) walks every placed object of the
           area by name, again and again (Kokiri: ~344 names, ~250k lookups a second). On the console it only gets the
           time nothing more important wants; at the host's normal priority it took the renderer's and the driver's
           cores and the frame waited (Kokiri, RX 6600 + 5600G: 17-20 fps; lowest: 21-28). HOS_OCB_NORMAL=1: as before) */
        SetThreadPriority(h, THREAD_PRIORITY_LOWEST);
    }
}

/* ---- time ------------------------------------------------------------------------------------------------ */
uint64_t hos_ticks(void) { return recomp_cntpct(NULL); }

int64_t hos_now_ns(void) {
    static LARGE_INTEGER freq;
    static int64_t ns_per_tick;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        ns_per_tick = 1000000000LL % f.QuadPart == 0 ? 1000000000LL / f.QuadPart : 0;
        freq = f;
    }
    QueryPerformanceCounter(&now);
    if (ns_per_tick) return (int64_t)now.QuadPart * ns_per_tick;
    return (int64_t)((now.QuadPart / freq.QuadPart) * 1000000000LL + (now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
}

static void sleep_ns(int64_t ns, int guest);
/* How sleeps under a millisecond wait: 0, all yield until their time; 1, the game's own (SleepThread) wait on the timer,
   which wakes about 0.5 ms late, and the host's (the frame limiter, the audio renderer) yield; 2, all wait on the timer.
   asked: the value of HOS_SHORT_SLEEP_TIMER, NULL when it is not set; set, it is read by env_on's rule (on: the timer for
   all, 2; off: yielding for all, 0). threads: the logical processors this process may run on, 0 when not known. Not set:
   1 with SHORT_SLEEP_TIMER_THREADS of them or fewer, which is no more than the game's six cores, else 0.
   OoT's idle job workers sleep 20 us and look again, about 7,600 times a second, and a yielding loop keeps a processor
   busy for each: with so few processors that is time the frame's threads need. Measured with the game held to part of a
   5700X3D: on 4 threads Hyrule Field went from 47 to 51 fps and lost its long frames (over 250 ms in 2 of 4 runs
   yielding, in none with the timer), Kokiri's worst frame went from 53-422 ms to 30 ms; on 6 the frame rate stayed and a
   frame's CPU time fell 9 %. On 8 threads and more yielding stays: with the timer for all sleeps there were no long
   frames either way and Kokiri lost 0.2 fps under its cap, and the timer made TOTK's 20 us waits for its job workers 25x
   longer, ~2000 a second on the frame's critical path. */
static int hos_short_sleep_timer(const char *asked, int threads) {
    if (asked) return env_value_off(asked) ? 0 : 2;
    return threads > 0 && threads <= SHORT_SLEEP_TIMER_THREADS ? 1 : 0;
}
/* Where the game's short sleeps yield (more than SHORT_SLEEP_TIMER_THREADS processors), at most this many of its
   threads yield through one at the same time; the game's next short sleep then waits on the timer until one is back.
   OoT's idle job workers all poll their queues in 20 us steps: the first few notice new work within ~20 us, the rest
   stop keeping a processor busy each (as Go's scheduler keeps at most half of its idle threads looking for work, and
   RPCS3 yields below its host quantum only on CPUs with few threads). The frame's serial threads (hos_thread_serial:
   the game thread's wait for its own batch of jobs) and the host's sleeps always yield and are not counted.
   asked: HOS_SHORT_SLEEP_POLLERS, NULL when not set: a number, 0 = no limit (every short sleep yields, as before); not
   a number = not set. Not set: a quarter of the processors (4 of 16), none where they are not known. */
static volatile LONG short_pollers;   /* (the game's threads in the yield loop now) */
static int hos_short_sleep_pollers(const char *asked, int threads) {
    char *end = NULL;
    const long n = asked && *asked ? strtol(asked, &end, 10) : 0;
    if (end && end != asked && *end == 0) return n > 0 ? (int)(n < 1024 ? n : 1024) : 0;
    return threads > SHORT_SLEEP_TIMER_THREADS ? threads / 4 : 0;
}
/* (virtual cores: a sleep gives the core up, SleepThread(0) offers it to a waiter as good) guest: the game's own sleep */
static void sleep_away(int64_t ns, int guest) {
    if (ns <= 0) {
        hos_vcore_yield();
        SwitchToThread();
        return;
    }
    HOS_CHAIN(HOS_CHAIN_SLEEP, (uint64_t)ns);
    const int away = hos_vcore_away();
    sleep_ns(ns, guest);
    if (away) hos_vcore_back();
    HOS_CHAIN(HOS_CHAIN_AWAKE, (uint64_t)ns);
}
void hos_sleep_ns(int64_t ns) { sleep_away(ns, 0); }
static void sleep_ns(int64_t ns, int guest) {
    /* A high-resolution waitable timer (one per thread) sleeps to within ~0.5 ms without spinning; Sleep() rounds to
       the timer resolution, so without one the last millisecond is spun (threads that sleep in short steps, the
       vibration thread for one, used 15% of a core spinning). */
    static _Thread_local HANDLE timer;
    static _Thread_local int tried;
    int64_t until = hos_now_ns() + ns;
#ifdef __EMSCRIPTEN__
    /* The web has no high-resolution timer, and its clock is a call into JavaScript: spinning to the deadline below
       burned ~0.8 of a core across TOTK's sleeping threads (cores its game, render and WebGPU threads needed). A
       timed wait (a futex: Atomics.wait) to `slack` before the time, then the rest spun; slack follows how late the
       waits wake (HOS_WEB_SPIN_SLEEP=1: spun, as before). */
    static int web_spin = -1;
    if (web_spin < 0) web_spin = env_on("HOS_WEB_SPIN_SLEEP");
    static _Atomic int64_t slack = 300000;
    const int64_t s = atomic_load_explicit(&slack, memory_order_relaxed);
    if (!web_spin && ns > s + 50000) {
        const int64_t want = until - s;
        usleep((useconds_t)((want - hos_now_ns()) / 1000));
        const int64_t late = hos_now_ns() - want;
        int64_t next = s + (late + 25000 - s) / 8;
        next = next < 50000 ? 50000 : next > 2000000 ? 2000000 : next;
        atomic_store_explicit(&slack, next, memory_order_relaxed);
    }
    while (hos_now_ns() < until) sched_yield();
    return;
#endif
    /* Under a millisecond: yielded away until the time, or the timer for the game's own sleeps where the processors
       are few (hos_short_sleep_timer). HOS_SHORT_SLEEP_TIMER=1: the timer always, 0: never, not set: by the processors */
    /* (the choice, how | the cap << 2: one atomic word, so a thread sees both or neither) */
    static int choice = -1;
    int chosen = __atomic_load_n(&choice, __ATOMIC_RELAXED);
    if (chosen < 0) {
        const int threads = process_threads();   /* (0 elsewhere than Windows: the timer when asked for) */
        int how = hos_short_sleep_timer(getenv("HOS_SHORT_SLEEP_TIMER"), threads);
#if !defined(_WIN32)
        /* (hookshot) Linux: the game's own sleeps wait on the timer whatever the processors, the host's still yield (1).
           With #2602, OoT's five idle job workers and sead::MainThread yielded 3.3-3.6 ms a frame each in Hyrule Field
           (RTX 3090, 7900X, 24 threads), ~9 ms of CPU a frame in all, while one of them waited on a GPU readback. With
           the timer for all sleeps the frame's CPU fell 44.8 -> 35.4 ms at the same median frame (11.30 / 11.32 ms).
           The job workers are not on the frame's critical path there (HOS_CHAIN), so their ~0.5 ms later wake costs
           nothing measured; on 4-8 thread CPUs (the Steam Deck: 8) those yields took processors the frame needs.
           HOS_SHORT_SLEEP_TIMER=0: all yield, as before; =1: all on the timer */
        if (!getenv("HOS_SHORT_SLEEP_TIMER")) how = 1;
#endif
        /* (the line says what was chosen, before any thread has made its timer: where the high-resolution timer cannot
           be made, Windows before 10 1803, a short sleep falls through to the yield loop at the end and nothing changes) */
        static volatile LONG told;
        if (how && !InterlockedExchange(&told, 1))
            hos_log("sleep: %s sleeps under a millisecond wait on the timer where the high-resolution one can be made "
                    "(%d logical processors, 0: not counted; HOS_SHORT_SLEEP_TIMER=0: all yield)",
                    how == 2 ? "all" : "the game's", threads);
        /* (HOS_SHORT_SLEEP_TIMER set says how all short sleeps wait: no limit then) */
        const int cap = how || getenv("HOS_SHORT_SLEEP_TIMER") ? 0 : hos_short_sleep_pollers(getenv("HOS_SHORT_SLEEP_POLLERS"), threads);
        if (cap && !InterlockedExchange(&told, 1))
            hos_log("sleep: at most %d of the game's threads yield through a short sleep at a time, the others wait on the "
                    "timer (%d logical processors; HOS_SHORT_SLEEP_POLLERS=0: all yield)", cap, threads);
        chosen = how | cap << 2;
        __atomic_store_n(&choice, chosen, __ATOMIC_RELAXED);
    }
    const int short_timer = chosen & 3, poll_cap = chosen >> 2;
    if (ns < 1000000 && short_timer < (guest ? 1 : 2)) {
        const int counted = guest && poll_cap && !hos_thread_serial(hos_self());
        if (!counted || InterlockedIncrement(&short_pollers) <= poll_cap) {
            while (hos_now_ns() < until)
                if (!SwitchToThread()) YieldProcessor();
            if (counted) InterlockedDecrement(&short_pollers);
            return;
        }
        InterlockedDecrement(&short_pollers);   /* (enough of them poll already: the timer, below) */
    }
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);   /* relative, 100 ns units */
        if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    if (ns > 2000000) Sleep((DWORD)((ns - 1000000) / 1000000));
    while (hos_now_ns() < until) SwitchToThread();
}

/* ---- nn::os threads -------------------------------------------------------------------------------------- */
static HosThread *thread_arg(Ctx *c, uint64_t tt) {
    HosThread *t = hos_thread_of(tt);
    if (!t) hos_fatal(c, "unknown nn::os::ThreadType %#llx", (unsigned long long)tt);
    return t;
}

/* nn::os::CreateThread(ThreadType*, void (*)(void*), void* arg, void* stack, unsigned long size, int prio) */
HLE(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi) {
    HOS_TRACE_CALL("CreateThread");
    hos_thread_create(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (int)ARG(5), -2);
    RET(0);
}

/* nn::os::CreateThread(ThreadType*, void (*)(void*), void* arg, void* stack, unsigned long size, int prio,
   int idealCore) (hookshot: OoT's Porter thread pool uses this overload; left to the guest SDK it reached
   ThreadManagerImplByHorizon::MapAliasStack, which needs svcMapMemory, and aborted) */
HLE(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii) {
    HOS_TRACE_CALL("CreateThread");
    hos_thread_create(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (int)ARG(5), (int)ARG(6));
    RET(0);
}

HLE(_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("StartThread");
    hos_thread_start(thread_arg(c, ARG(0)));
}

HLE(_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("WaitThread");
    hos_thread_join(thread_arg(c, ARG(0)));
}

HLE(_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE) {
    HOS_TRACE_CALL("DestroyThread");
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_join(t);
    hos_thread_destroy(t);
}

HLE(_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc) {
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_set_name(t, ARG(1) ? (const char *)GP(ARG(1)) : "");
    if (hos_trace) hos_log("SetThreadName %s", t->name);
}

HLE(_ZN2nn2os16GetCurrentThreadEv) { RET(cur->guest_tt); }
HLE(_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE) { RET(thread_arg(c, ARG(0))->id); }
HLE(_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE) { RET((uint32_t)hos_thread_priority(thread_arg(c, ARG(0)), 0, 0)); }
HLE(_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi) {
    RET((uint32_t)hos_thread_priority(thread_arg(c, ARG(0)), 1, (int)ARG(1)));
}

/* nn::os::SetThreadCoreMask(ThreadType*, int idealCore, unsigned long mask) */
HLE(_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim) {
    HosThread *t = thread_arg(c, ARG(0));
    hos_thread_affinity(t, (int)ARG(1), ARG(2));
}

HLE(_ZN2nn2os20GetCurrentCoreNumberEv) { RET((uint32_t)hos_thread_core(cur)); }
HLE(_ZN2nn2os26GetThreadAvailableCoreMaskEv) { RET((1u << hos_cores()) - 1); }   /* cores 0-2: what an application gets */

HLE(_ZN2nn2os19GetCurrentStackInfoEPmS1_) {
    if (ARG(0)) G64(ARG(0)) = cur->stack;
    if (ARG(1)) G64(ARG(1)) = cur->stack_size;
}

HLE(_ZN2nn2os11YieldThreadEv) {
    hos_vcore_yield();
    SwitchToThread();
}
/* HOS_SLEEP_LOG=1 (diagnostic): guest sleeps by caller (LR) every 5 s: count, asked, slept */
void hos_sleep_logged(int64_t ns, uint64_t lr) {
    /* HOS_POLL_SLEEP_NS=<ns> (experiment): a guest sleep of up to 100 us lasts at most that long. TOTK's job system
       waits for its workers by sleeping 20 us and looking again (~90 times a frame at the castle, on MainThread and
       the workers): a shorter step notices the job's end sooner (the loop looks again; nothing else changes).
       Measured 2026-10-21 (castle, 2000 ns, 3 rounds): no change. */
    static int64_t poll_cap = -1;
    if (poll_cap < 0) {
        const char *e = getenv("HOS_POLL_SLEEP_NS");
        poll_cap = e ? atoll(e) : 0;
    }
    if (poll_cap && ns > poll_cap && ns <= 100000) ns = poll_cap;
    static int on = -1;
    if (on < 0) on = env_on("HOS_SLEEP_LOG");
    if (!on) { sleep_away(ns, 1); return; }
    const int64_t t0 = hos_now_ns();
    sleep_away(ns, 1);
    const int64_t took = hos_now_ns() - t0;
    static SRWLOCK lk = SRWLOCK_INIT;
    static struct { uint64_t lr; int64_t n, asked, slept; } tab[64];
    static int64_t last;
    AcquireSRWLockExclusive(&lk);
    int i = 0;
    while (i < 64 && tab[i].lr && tab[i].lr != lr) i++;
    if (i < 64) {
        tab[i].lr = lr;
        tab[i].n++;
        tab[i].asked += ns > 0 ? ns : 0;
        tab[i].slept += took;
    }
    const int64_t now = hos_now_ns();
    if (now - last > 5000000000LL) {
        last = now;
        for (int k = 0; k < 64 && tab[k].lr; k++)
            if (tab[k].n)
                hos_log("sleep: lr %#llx: %lld calls, asked %.2f ms, slept %.2f ms (%.1f us each)",
                        (unsigned long long)tab[k].lr, (long long)tab[k].n, tab[k].asked / 1e6, tab[k].slept / 1e6,
                        tab[k].slept / 1e3 / tab[k].n);
        memset(tab, 0, sizeof tab);
    }
    ReleaseSRWLockExclusive(&lk);
}

/* (hookshot) OoT's present waits for its frame's GPU work by polling an NVN event, sleeping ~1.6 ms between looks
   (0x80590500: `while (nvnEventGetValue(e) != 1) SleepThread(...)`, ~3.5 sleeps a frame, 2 ms each on Windows): the
   frame noticed the GPU's signal late, ~7 ms of the 25 ms frame asleep on its critical path. The GPU writes the event
   into guest memory itself, so the sleep looks at it between yields (its virtual core given up) and ends once it is
   1, at the latest when the asked time is up. Only where the poll is nvnEventGetValue. HOS_PRESENT_POLL=1 (an
   experiment: no fps change measured, and it keeps a core busy while it looks) */
static int present_poll(Ctx *c, int64_t ns) {
    static int on = -1;
    static uint64_t fn_ok, fn_bad;
    if (on < 0) on = getenv("HOS_PRESENT_POLL") && atoi(getenv("HOS_PRESENT_POLL")) != 0;
    if (!on || c->x[30] != 0x80590520 || ns <= 0 || ns > 20000000) return 0;
    const uint64_t fp = G64(c->x[24]);
    if (fp != fn_ok) {
        if (fp == fn_bad) return 0;
        const char *hos_hostcall_name(uint64_t addr);
        const char *n = hos_hostcall_name(fp);
        if (!n || strcmp(n, "nvnEventGetValue")) { fn_bad = fp; return 0; }
        fn_ok = fp;
        hos_log("sleep: OoT's present poll looks at its event between yields (HOS_PRESENT_POLL)");
    }
    int hos_nvn_event_value(uint64_t guest_event, uint32_t *v);
    const uint64_t ev = c->x[23] + (uint64_t)G32(c->x[22] + 0x164) * 64;
    uint32_t v = 0;
    if (!hos_nvn_event_value(ev, &v)) return 0;
    const int away = hos_vcore_away();
    const int64_t end = hos_now_ns() + ns;
    while (v != 1 && hos_now_ns() < end) {
        SwitchToThread();
        for (int i = 0; i < 64; i++) _mm_pause();
        hos_nvn_event_value(ev, &v);
    }
    if (away) hos_vcore_back();
    return 1;
}
/* (hookshot) The FPS counter's Breakdown (gpu/vk/vk_fps.cpp, through MkwVkHost slept_ns): the time slept in OoT's present poll
   (0x80590520 above: waiting for its frame's GPU work) and in the window's acquire (host/nn_vi.c: the frame pacing), whichever
   thread they are on (OoT simulates on its main thread and records on its presentation thread, which waits for it), and each
   thread's other guest sleeps. A clock read before and after a sleep, only while the Breakdown shows (hos_fps_timing, set by the
   back end through MkwVkHost fps_timing): off, a load of that flag */
volatile int hos_fps_timing;
volatile int64_t hos_poll_slept_ns, hos_acquire_slept_ns;
_Thread_local int64_t hos_slept_ns[2];
static void slept_since(uint64_t lr, int64_t t0) {
    if (!t0) return;
    const int64_t d = hos_now_ns() - t0;
    if ((lr & ~1ull) == 0x80590520) InterlockedAdd64(&hos_poll_slept_ns, d);
    else hos_slept_ns[1] += d;
}
HLE(_ZN2nn2os11SleepThreadENS_8TimeSpanE) {
    const int64_t t0 = hos_fps_timing ? hos_now_ns() : 0;
    if (present_poll(c, (int64_t)ARG(0))) { slept_since(c->x[30], t0); return; }
    if (c->x[30] == 0x80590520 && env_on("HOS_SLEEP_LOG")) {   /* (hookshot, diagnostic: OoT's present poll) */
        static int logged;
        if (logged++ < 3) {
            const char *hos_hostcall_name(uint64_t addr);
            const uint64_t fp = G64(c->x[24]), sync = c->x[23] + (uint64_t)G32(c->x[22] + 0x164) * 64;
            const char *n = hos_hostcall_name(fp);
            hos_log("sleep: OoT's present poll: fn %#llx (%s), arg %#llx, sleep %lld ns", (unsigned long long)fp,
                    n ? n : "guest code", (unsigned long long)sync, (long long)ARG(0));
        }
    }
    hos_sleep_logged((int64_t)ARG(0), c->x[30]);
    slept_since(c->x[30], t0);
}

/* ---- TLS slots --------------------------------------------------------------------------------------------- */
static volatile LONG slot_used[HOS_TLS_SLOTS];

/* nn::os::AllocateTlsSlot(TlsSlot*, void (*)(unsigned long)) -> Result */
HLE(_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE) {
    for (int i = 0; i < HOS_TLS_SLOTS; i++) {
        if (!InterlockedCompareExchange(&slot_used[i], 1, 0)) {
            G32(ARG(0)) = (uint32_t)i;
            RET(0);
            return;
        }
    }
    RET(0x2C01);   /* os::ResultOutOfResource */
}

HLE(_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE) { if ((uint32_t)ARG(0) < HOS_TLS_SLOTS) slot_used[(uint32_t)ARG(0)] = 0; }
HLE(_ZN2nn2os11GetTlsValueENS0_7TlsSlotE) { RET((uint32_t)ARG(0) < HOS_TLS_SLOTS ? cur->tls_slots[(uint32_t)ARG(0)] : 0); }
HLE(_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm) { if ((uint32_t)ARG(0) < HOS_TLS_SLOTS) cur->tls_slots[(uint32_t)ARG(0)] = ARG(1); }

/* ---- ticks ------------------------------------------------------------------------------------------------- */
volatile uint64_t hos_gfx_module;
/* HOS_TICK_LOG=1 (diagnostic): who reads the clock, by caller (LR), every 5 s */
static void tick_log(uint64_t lr) {
    static CRITICAL_SECTION cs; static volatile LONG once; static uint64_t lrs[512]; static uint32_t counts[512]; static int64_t next;
    if (!InterlockedExchange(&once, 1)) InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    for (int i = (int)((lr >> 2) * 2654435761u >> 8) & 511, n = 0; n < 512; n++, i = (i + 1) & 511)
        if (!lrs[i] || lrs[i] == lr) { lrs[i] = lr; counts[i]++; break; }
    const int64_t now = hos_now_ns();
    if (now > next) {
        next = now + 5000000000LL;
        char line[1500]; int n = 0;
        for (int k = 0; k < 40 && n < 1400; k++) {
            int best = -1;
            for (int i = 0; i < 512; i++) if (counts[i] && (best < 0 || counts[i] > counts[best])) best = i;
            if (best < 0) break;
            n += snprintf(line + n, sizeof line - n, " %llx:%u", (unsigned long long)lrs[best], counts[best]);
            counts[best] = 0;
        }
        memset(counts, 0, sizeof counts);
        hos_log("tick readers in 5 s:%s", line);
    }
    LeaveCriticalSection(&cs);
}
HLE(_ZN2nn2os13GetSystemTickEv) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_TICK_LOG");
    if (on) tick_log(c->x[30]);
    /* (hookshot) The graphics module: x19 at its frame-end clock read (main+0x346c770 is after the bl), for nn_vi.c's
       real-time step */
    if (c->x[30] == 0x8348c770) hos_gfx_module = c->x[19];
    RET(hos_ticks());
}
HLE(_ZN2nn2os22GetSystemTickFrequencyEv) { RET(HOS_TICK_HZ); }
/* (hookshot) At the counter's own rate, HOS_TICK_HZ (31.25 MHz: 32 ns a tick). These converted at the Switch 1's
   19.2 MHz (ticks = ns * 12 / 625) while GetSystemTick counts 31.25 MHz, so a tick span came out 1.63x too long: OoT's
   present slept 1.63 ms for its 1 ms poll step (0x80590500: SleepThread(ConvertToTimeSpan(frequency / 1000))), and
   any of its timing through these was off by as much. HOS_TICK_CONVERT_192=1: as before */
static int tick_convert_192(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_TICK_CONVERT_192");
    return on;
}
HLE(_ZN2nn2os13ConvertToTickENS_8TimeSpanE) {
    int64_t ns = (int64_t)ARG(0);
    if (tick_convert_192()) { RET((ns / 625) * 12 + (ns % 625) * 12 / 625); return; }
    RET(ns / (int64_t)(1000000000ULL / HOS_TICK_HZ));
}
HLE(_ZN2nn2os17ConvertToTimeSpanENS0_4TickE) {
    int64_t t = (int64_t)ARG(0);
    if (tick_convert_192()) { RET((t / 12) * 625 + (t % 12) * 625 / 12); return; }
    RET(t * (int64_t)(1000000000ULL / HOS_TICK_HZ));
}

/* ---- fibers (hookshot: nn::async's FiberTaskCoordinator, on OoT's nn.npln threads) ----------------------------
   A guest fiber is a Win32 fiber with its own guest context: the lifted frames on its host stack keep using that
   context wherever it runs, so a fiber may resume on another thread. Switching in, it takes the running thread's
   identity (TLS registers, host thread object); the thread's own context stays in HosThread.ctx. The fiber function
   returns the FiberType to switch to next (nn::os semantics). */
typedef struct HosFiber {
    Ctx ctx;   /* (first: a context's fiber is found from its address) */
    uint64_t ft, fn, arg, stack, size;
    void *host;
    int started, done;
    struct HosFiber *next;
} HosFiber;
static SRWLOCK fibers_lock = SRWLOCK_INIT;
static HosFiber *fibers;

static HosFiber *fiber_of_ft(uint64_t ft) {
    AcquireSRWLockShared(&fibers_lock);
    HosFiber *f = fibers;
    while (f && f->ft != ft) f = f->next;
    ReleaseSRWLockShared(&fibers_lock);
    return f;
}
static HosFiber *fiber_of_ctx(Ctx *c) {
    AcquireSRWLockShared(&fibers_lock);
    HosFiber *f = fibers;
    while (f && &f->ctx != c) f = f->next;
    ReleaseSRWLockShared(&fibers_lock);
    return f;
}
static void fiber_switch(Ctx *from, HosFiber *to);

static void WINAPI fiber_main(void *p) {
    HosFiber *f = (HosFiber *)p;
    Ctx *c = &f->ctx;
    c->x[0] = f->arg;
    c->x[30] = 0;
    c->x[31] = (f->stack + f->size) & ~15ULL;
    a64_icall(c, f->fn);   /* FiberType* (*)(void*) */
    const uint64_t next = c->x[0];
    f->done = 1;
    HosFiber *to = next ? fiber_of_ft(next) : NULL;   /* (NULL: back to the thread's own context, as nn::os does) */
    if (next && !to) hos_fatal(c, "fiber %#llx returned %#llx: not an initialized fiber", (unsigned long long)f->ft,
                               (unsigned long long)next);
    fiber_switch(c, to);
    hos_fatal(c, "fiber %#llx: switched to after it returned", (unsigned long long)f->ft);
}

/* from: the context calling (the thread's own or a fiber's); to: NULL for the thread's own context */
static void fiber_switch(Ctx *from, HosFiber *to) {
    HosThread *t = cur;
    if (!t->host_fiber) {
        t->host_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        if (!t->host_fiber) hos_fatal(from, "ConvertThreadToFiber failed (%lu)", GetLastError());
    }
    Ctx *dst = to ? &to->ctx : &t->ctx;
    if (dst == from) return;
    if (to) {
        if (to->done) hos_fatal(from, "SwitchToFiber(%#llx): that fiber has returned", (unsigned long long)to->ft);
        if (!to->started) {   /* the thread's state it inherits: FPCR, then the identity below */
            to->started = 1;
            to->ctx.fpcr = from->fpcr;
            to->ctx.fpsr = from->fpsr;
        }
    }
    /* the running thread's identity, whoever ran this context last */
    dst->tpidr_el0 = t->ctx.tpidr_el0;
    dst->tpidrro_el0 = t->ctx.tpidrro_el0;
    dst->thread = t->ctx.thread;
    if (from->poll) dst->poll = 1;   /* (a pending virtual-core request follows the thread) */
    t->run_ctx = to ? &to->ctx : NULL;
    SwitchToFiber(to ? to->host : t->host_fiber);
    /* back in `from`, maybe on another thread: take that one's identity (fiber_switch's caller set it already) */
}

/* InitializeFiber(FiberType*, FiberType* (*)(void*), void* arg, void* stack, unsigned long size, int flags) */
HLE(_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi) {
    HosFiber *f = fiber_of_ft(ARG(0));
    if (!f) {
        f = (HosFiber *)calloc(1, sizeof *f);
        f->ft = ARG(0);
        AcquireSRWLockExclusive(&fibers_lock);
        f->next = fibers, fibers = f;
        ReleaseSRWLockExclusive(&fibers_lock);
    } else if (f->host) {
        DeleteFiber(f->host);
    }
    memset(&f->ctx, 0, sizeof f->ctx);
    f->fn = ARG(1), f->arg = ARG(2), f->stack = ARG(3), f->size = ARG(4);
    f->started = f->done = 0;
    f->host = CreateFiberEx(64 << 10, 64 << 20, FIBER_FLAG_FLOAT_SWITCH, fiber_main, f);
    if (!f->host) hos_fatal(c, "CreateFiberEx failed (%lu)", GetLastError());
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 8 || hos_trace)
        hos_log("fiber %#llx: function %#llx arg %#llx stack %#llx+%#llx flags %d", (unsigned long long)f->ft,
                (unsigned long long)f->fn, (unsigned long long)f->arg, (unsigned long long)f->stack,
                (unsigned long long)f->size, (int)ARG(5));
}

HLE(_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE) {
    HosFiber *f = fiber_of_ft(ARG(0));
    if (f && f->host && &f->ctx != c) { DeleteFiber(f->host); f->host = NULL; f->done = 1; }
}

HLE(_ZN2nn2os15GetCurrentFiberEv) {
    HosFiber *f = c == &cur->ctx ? NULL : fiber_of_ctx(c);
    RET(f ? f->ft : 0);
}

/* SwitchToFiber(FiberType*): NULL goes back to the thread's own context */
HLE(_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE) {
    HosFiber *to = NULL;
    if (ARG(0) && !(to = fiber_of_ft(ARG(0))))
        hos_fatal(c, "SwitchToFiber(%#llx): not an initialized fiber", (unsigned long long)ARG(0));
    fiber_switch(c, to);
}

/* Every thread's kernel handle (the tag its SDK mutexes hold), for hos_kernel_dump. */
void hos_thread_log_handles(void) {
    AcquireSRWLockShared(&threads_lock);
    /* (lr: the return address of the thread's last call, x0 its first argument: in a wait, which game code waits on
       what; the web has no stack walk) */
    for (int i = 0; i < nthreads; i++)
        hos_log("kernel:   handle %#x = %s: lr %#llx x0 %#llx x19 %#llx x20 %#llx x21 %#llx x22 %#llx",
                threads[i]->kernel_handle, threads[i]->name, (unsigned long long)threads[i]->ctx.x[30],
                (unsigned long long)threads[i]->ctx.x[0], (unsigned long long)threads[i]->ctx.x[19],
                (unsigned long long)threads[i]->ctx.x[20], (unsigned long long)threads[i]->ctx.x[21],
                (unsigned long long)threads[i]->ctx.x[22]);
    ReleaseSRWLockShared(&threads_lock);
}

/* ---- virtual cores (on unless HOS_VCORES=0) -------------------------------------------------------------------
   Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by the current core
   number without locks; both MacNativeGames (threads.c) and totk-pc-recomp (runtime/kernel.c, MIT) schedule the game's
   threads on three virtual cores for it. Here a guest thread holds one of hos_cores() cores while it runs guest code
   and gives it up where Horizon would switch threads: in every block (hos_cv_sleep, hos_thread_wait, hos_sleep_ns,
   the NVN waits), at a yield, at exit. A free core goes to the best waiter allowed on it (lower priority value first,
   then the one waiting longest). A waiter better than a holder asks it to give the core up (vc_kick), which it does at
   its next checkpoint (every nn::os call and SVC: hos_vcore_checkpoint), as Horizon would run the better thread at
   once. Guest code with no checkpoint (a spin loop) keeps its core, so a waiter that has waited too long runs anyway
   without one (an "escape", logged with the holders): HOS_VCORE_ESCAPE_US for a better waiter, HOS_VCORE_ESCAPE_EQ_US
   otherwise (both 2000000: 2 s). An escape runs a thread with no core while the holder still runs on it, so two threads
   share per-core state (#1929); it is kept only as a deadlock breaker. 250 ms was the default before: on the 25-minute
   route it escaped 6-8 times a run, at waits that ended by themselves within 1.2 s (0 escapes at 2 s, frame times the
   same or better). Every 5 s the log has a "vcores:" line. GetCurrentCoreNumber answers the
   core a thread holds. */
#if !defined(__EMSCRIPTEN__)
static SRWLOCK vc_lock = SRWLOCK_INIT;
static HosThread *vc_owner[8];
static HosThread *vc_queue[1024];
static int vc_nq;
static struct { int64_t takes, waits, wait_ns, max_ns, escapes, kicks, yields, since; } vc_st;

int hos_vcores_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HOS_VCORES");
        on = e ? atoi(e) != 0 : 1;   /* (mkw-recomp: on unless HOS_VCORES=0; the game's job queues need it) */
    }
    return on;
}
static int vc_cores(void) { return hos_cores() > 8 ? 8 : hos_cores(); }
static unsigned vc_allowed(HosThread *t) {
    const unsigned all = (1u << vc_cores()) - 1;
    unsigned m = (unsigned)(t->core_mask & all);
    if (!m) m = 1u << hos_thread_core(t);
    return m;
}
/* a runs before b */
static int vc_better(HosThread *a, HosThread *b) {
    return a->priority != b->priority ? a->priority < b->priority : a->vc_since < b->vc_since;
}
/* a free core t may run on (its last one, its ideal one, then the lowest), or -1; holding vc_lock */
static int vc_free_for(HosThread *t) {
    const unsigned m = vc_allowed(t);
    if (t->vc_last > 0 && (m >> (t->vc_last - 1) & 1) && !vc_owner[t->vc_last - 1]) return t->vc_last - 1;
    const int ideal = hos_thread_core(t);
    if ((m >> ideal & 1) && !vc_owner[ideal]) return ideal;
    for (int c = 0; c < vc_cores(); c++)
        if ((m >> c & 1) && !vc_owner[c]) return c;
    return -1;
}
static void vc_take(HosThread *t, int c) {
    vc_owner[c] = t;
    t->vc_held = c + 1;
    t->vc_last = c + 1;
    t->vc_kick = 0;
}
static void vc_dequeue(HosThread *t) {
    for (int i = 0; i < vc_nq; i++)
        if (vc_queue[i] == t) {
            vc_queue[i] = vc_queue[--vc_nq];
            break;
        }
    t->vc_queued = 0;
}
/* (HOS_CHAIN: a thread's wait for a core, and the grant that ends it; bit 63: never a guest address) */
static uint64_t vc_chain_key(HosThread *t) { return 1ULL << 63 | (uint64_t)(uintptr_t)t; }
/* core c is free: to the best waiter allowed on it; holding vc_lock */
static void vc_grant(int c) {
    HosThread *best = NULL;
    for (int i = 0; i < vc_nq; i++)
        if ((vc_allowed(vc_queue[i]) >> c & 1) && (!best || vc_better(vc_queue[i], best))) best = vc_queue[i];
    if (!best) return;
    vc_dequeue(best);
    vc_take(best, c);
    HOS_CHAIN(HOS_CHAIN_SIGNAL, vc_chain_key(best));
    WakeConditionVariable(&best->vc_cv);
}
static void vc_stats_locked(int64_t now) {
    if (!vc_st.since) vc_st.since = now;
    if (now - vc_st.since < 5000000000LL) return;
    hos_log("vcores: %lld takes, %lld waits (mean %.0f us, longest %.1f ms), %lld kicks, %lld yields, %lld escapes in %.0f s",
            (long long)vc_st.takes, (long long)vc_st.waits, vc_st.waits ? vc_st.wait_ns / 1e3 / vc_st.waits : 0.0,
            vc_st.max_ns / 1e6, (long long)vc_st.kicks, (long long)vc_st.yields, (long long)vc_st.escapes,
            (now - vc_st.since) / 1e9);
    memset(&vc_st, 0, sizeof vc_st);
    vc_st.since = now;
}
static int64_t vc_env_ns(const char *name, int64_t us) {
    const char *e = getenv(name);
    return (e ? atoll(e) : us) * 1000;
}
static void vc_acquire(HosThread *t) {
    static int64_t esc_better = -1, esc_eq, slice;
    if (esc_better < 0) {
        esc_eq = vc_env_ns("HOS_VCORE_ESCAPE_EQ_US", 2000000);
        esc_better = vc_env_ns("HOS_VCORE_ESCAPE_US", 2000000);
        slice = vc_env_ns("HOS_VCORE_SLICE_US", 2000);
    }
    AcquireSRWLockExclusive(&vc_lock);
    vc_st.takes++;
    int c = vc_free_for(t);
    if (c >= 0) {
        vc_take(t, c);
        ReleaseSRWLockExclusive(&vc_lock);
        return;
    }
    const int64_t t0 = hos_now_ns();
    HOS_CHAIN(HOS_CHAIN_WAIT, vc_chain_key(t));
    t->vc_since = t0;
    t->vc_queued = 1;
    if (vc_nq < 1024) vc_queue[vc_nq++] = t;
    vc_st.waits++;
    for (;;) {
        /* the worst holder worse than t on a core t may use gives it up at its next checkpoint; after
           HOS_VCORE_SLICE_US (2000) of waiting, a holder as good as t does too (a time slice: Horizon would leave
           it running, but its threads never run two at once on one core, which the game's per-core data needs). The
           holder sees the request at its next nn::os call or SVC, or at a loop header (the interrupt polls) */
        const unsigned m = vc_allowed(t);
        HosThread *worst = NULL;
        for (int k = 0; k < vc_cores(); k++) {
            HosThread *h = vc_owner[k];
            if ((m >> k & 1) && h && h->priority > t->priority && (!worst || h->priority > worst->priority)) worst = h;
        }
        if (!worst && hos_now_ns() - t0 > slice)
            for (int k = 0; k < vc_cores() && !worst; k++)
                if ((m >> k & 1) && vc_owner[k] && vc_owner[k]->priority == t->priority) worst = vc_owner[k];
        if (worst && !worst->vc_kick) {
            worst->vc_kick = worst->priority > t->priority ? 1 : 2;
            Ctx *rc = worst->run_ctx;   /* (a fiber's context while it runs one) */
            __atomic_store_n(rc ? &rc->poll : &worst->ctx.poll, 1, __ATOMIC_RELAXED);
            vc_st.kicks++;
        }
        SleepConditionVariableSRW(&t->vc_cv, &vc_lock, 1, 0);
        if (t->vc_held) break;   /* granted by a release */
        if ((c = vc_free_for(t)) >= 0) {
            vc_dequeue(t);
            vc_take(t, c);
            break;
        }
        const int64_t waited = hos_now_ns() - t0;
        if (waited > (worst ? esc_better : esc_eq)) {
            vc_dequeue(t);
            t->vc_escaped = 1;
            vc_st.escapes++;
            static int logged;
            if (logged < 40) {
                logged++;
                char who[256] = "";
                int len = 0;
                for (int k = 0; k < vc_cores() && len < 200; k++)
                    if (m >> k & 1)
                        len += snprintf(who + len, sizeof who - len, " core %d: %s (%d)%s;", k,
                                        vc_owner[k] ? vc_owner[k]->name : "-", vc_owner[k] ? vc_owner[k]->priority : 0,
                                        vc_owner[k] && vc_owner[k]->vc_kick ? " asked" : "");
                hos_log("vcores: %s (priority %d) ran without a core after %.1f ms:%s", t->name, t->priority, waited / 1e6, who);
            }
            break;
        }
    }
    const int64_t waited = hos_now_ns() - t0;
    HOS_CHAIN(HOS_CHAIN_WOKE, vc_chain_key(t));
    vc_st.wait_ns += waited;
    if (waited > vc_st.max_ns) vc_st.max_ns = waited;
    vc_stats_locked(hos_now_ns());
    ReleaseSRWLockExclusive(&vc_lock);
}
static void vc_release(HosThread *t) {
    if (!t->vc_held) return;
    AcquireSRWLockExclusive(&vc_lock);
    const int c = t->vc_held - 1;
    vc_owner[c] = NULL;
    t->vc_held = 0;
    t->vc_kick = 0;
    vc_grant(c);
    ReleaseSRWLockExclusive(&vc_lock);
}
/* gives the core to a waiter allowed on it that is better than t (strict) or as good (a yield) */
static void vc_yield(HosThread *t, int strict) {
    if (!t->vc_held) return;
    AcquireSRWLockExclusive(&vc_lock);
    const int c = t->vc_held - 1;
    int any = 0;
    for (int i = 0; i < vc_nq && !any; i++) {
        HosThread *w = vc_queue[i];
        any = (vc_allowed(w) >> c & 1) && (strict ? w->priority < t->priority : w->priority <= t->priority);
    }
    t->vc_kick = 0;
    if (any) vc_st.yields++;
    ReleaseSRWLockExclusive(&vc_lock);
    if (!any) return;
    vc_release(t);
    vc_acquire(t);
}
/* 1 when the caller is to take a core again after its block (hos_vcore_back): it gave its core up, or it has run without
   one since an escape. An escape lasts until the thread's next block, as every other thread waits for a core after one;
   it took none again before, so an escaped thread ran with no core, and the core number of a core another thread held,
   for the rest of its life (issue 1929) */
int hos_vcore_away(void) {
    HosThread *t = cur;
    if (!hos_vcores_on() || !t) return 0;
    if (!t->vc_held) {
        const int escaped = t->vc_escaped;   /* (only the outermost block of nested ones takes the core again) */
        t->vc_escaped = 0;
        return escaped;
    }
    vc_release(t);
    return 1;
}
void hos_vcore_back(void) {
    HosThread *t = cur;
    if (!hos_vcores_on() || !t) return;
    /* after it the thread holds a core or has escaped again (vc_acquire sets the flag again): the escape the flag stood
       for has ended, also at a back without its away (a thread's start). A flag left up while a core is held would make
       a later inner block's away answer 1 and take a core in the middle of the outer block */
    t->vc_escaped = 0;
    if (!t->vc_held) vc_acquire(t);
}
/* Lifted code calls this from its interrupt polls with the loop's guest registers still in host registers (a64_poll_cur,
   mkw_glue.c: the poll hands over no context, so the compiler need not spill them). It, and everything it calls, must never
   read or write the thread's guest context (Ctx) or run guest code: what it would see there may be older than the loop. A
   stall dump of a thread spinning in a polled loop shows such older registers for the same reason. */
void hos_vcore_checkpoint(void) {
    HosThread *t = cur;
    if (t && t->vc_kick) vc_yield(t, t->vc_kick == 1);
}
void hos_vcore_yield(void) {
    HosThread *t = cur;
    if (hos_vcores_on() && t) vc_yield(t, 0);
}
/* SleepConditionVariableSRW for guest threads: the core given up for the wait; after it the lock is let go while a
   core is taken again (waiting for a core while holding it could stall the thread that will wake the next waiter),
   so callers see a spurious wake-up and look at their condition again, as they do anyway */
int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    const int away = hos_vcore_away();
    const BOOL r = SleepConditionVariableSRW((PCONDITION_VARIABLE)cv, (PSRWLOCK)lk, ms, 0);
    if (away) {
        ReleaseSRWLockExclusive((PSRWLOCK)lk);
        hos_vcore_back();
        AcquireSRWLockExclusive((PSRWLOCK)lk);
    }
    return r;
}
#else
int hos_vcores_on(void) { return 0; }
int hos_vcore_away(void) { return 0; }
void hos_vcore_back(void) {}
void hos_vcore_checkpoint(void) {}
void hos_vcore_yield(void) {}
int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)cv, (PSRWLOCK)lk, ms, 0);
}
#endif
