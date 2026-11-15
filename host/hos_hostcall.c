/* Host calls: guest-callable addresses made at run time, each bound to a host function.

   The lifted code reaches them through indirect calls (blr / br): rt/a64_rt.c asks a64_call_miss for any address
   that is not a lifted function. Each host call is a 16-byte slot in HOS_HOSTCALL_BASE.. (no memory behind it).
   Used for APIs that hand out function pointers, NVN above all: every proc nvnDeviceGetProcAddress returns is one. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

#define HOS_HOSTCALL_BASE 0x06000000ULL
#define HOS_HOSTCALL_MAX 8192

typedef void (*HostFn)(Ctx *c, void *user);
typedef struct { HostFn fn; void *user; char name[64]; volatile LONG calls; LONG before; } HostCall;
static HostCall calls[HOS_HOSTCALL_MAX];
static volatile LONG ncalls;
static SRWLOCK lock = SRWLOCK_INIT;

/* The guest address of a host call named `name` (made on first request, the same address after). */
uint64_t hos_hostcall(const char *name, HostFn fn, void *user) {
    AcquireSRWLockExclusive(&lock);
    for (LONG i = 0; i < ncalls; i++)
        if (!strcmp(calls[i].name, name)) { ReleaseSRWLockExclusive(&lock); return HOS_HOSTCALL_BASE + 16ULL * i; }
    if (ncalls == HOS_HOSTCALL_MAX) hos_fatal(NULL, "out of host calls");
    LONG i = ncalls++;
    calls[i].fn = fn;
    calls[i].user = user;
    snprintf(calls[i].name, sizeof calls[i].name, "%s", name);
    ReleaseSRWLockExclusive(&lock);
    return HOS_HOSTCALL_BASE + 16ULL * i;
}

/* (mkw-recomp) the host function and user data behind a host call's address: 0 when it is not one */
uint64_t hos_hostcall_target(uint64_t addr, HostFn *fn, void **user) {
    const uint64_t i = (addr - HOS_HOSTCALL_BASE) / 16;
    if (addr < HOS_HOSTCALL_BASE || i >= (uint64_t)ncalls) return 0;
    *fn = calls[i].fn, *user = calls[i].user;
    return addr;
}

const char *hos_hostcall_name(uint64_t addr) {
    uint64_t i = (addr - HOS_HOSTCALL_BASE) / 16;
    return addr >= HOS_HOSTCALL_BASE && i < (uint64_t)ncalls ? calls[i].name : NULL;
}

/* rt/a64_rt.c a64_lookup: a host call without user data as a plain function, called as fn(c) (its user argument, which
   such handlers do not read, is then whatever the register holds). Not with HOS_HOSTCALL_STATS or tracing (they count
   in a64_call_miss) or HOS_HOSTCALL_SLOW=1. Not on the web: wasm checks an indirect call's type, and a HostFn
   (two parameters) called as an A64Fn (one) traps with "function signature mismatch". */
A64Fn a64_lookup_host(uint64_t addr) {
#ifdef __EMSCRIPTEN__
    (void)addr;
    return NULL;
#endif
    if (addr < HOS_HOSTCALL_BASE || (addr - HOS_HOSTCALL_BASE) % 16) return NULL;
    const uint64_t i = (addr - HOS_HOSTCALL_BASE) / 16;
    if (i >= (uint64_t)ncalls || calls[i].user) return NULL;
    static int slow = -1;
    if (slow < 0) slow = env_on("HOS_HOSTCALL_STATS") || hos_trace || hos_hle_stats || env_on("HOS_HOSTCALL_SLOW");
    return slow ? NULL : (A64Fn)(void *)calls[i].fn;
}

/* The last host calls, for HOS_HOSTCALL_STATS: what a stalled thread was doing. */
#define RING 256
typedef struct { uint32_t call; uint32_t thread; uint64_t x[4], lr; } Recent;
static Recent ring[RING];
static volatile LONG ring_pos;

void hos_setjmp_return(Ctx *c, uint64_t target);   /* hos_setjmp.c: a longjmp's BR to a setjmp site */
int a64_call_miss(Ctx *c, uint64_t target) {
    hos_setjmp_return(c, target);   /* (hookshot; does not return when it is one) */
    if (!target) {
        /* (hookshot) a callback table entry zeroed at run time: OoT's UpdateTextBoxTextureThread reaches its text release trampoline
           with a null function pointer at the same cutscene point every session. 0 is never a real function; returning 1 turns the
           tail call into a plain return. chrispository, Hookshot PR 55 */
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
            hos_log("skipped an indirect call to 0 (lr %#llx, thread %s)", (unsigned long long)c->x[30], hos_thread_name(hos_self()));
        return 1;
    }
    if (target < HOS_HOSTCALL_BASE || (target - HOS_HOSTCALL_BASE) % 16) return 0;
    uint64_t i = (target - HOS_HOSTCALL_BASE) / 16;
    if (i >= (uint64_t)ncalls) return 0;
    HostCall *h = &calls[i];
    /* (the ring and the counts only for HOS_HOSTCALL_STATS / HOS_HLE_STATS / tracing: every NVN call took two interlocked
       increments on lines all game threads share) */
    static int stats = -1;
    if (stats < 0) stats = env_on("HOS_HOSTCALL_STATS") || hos_trace || hos_hle_stats;
    if (!stats) {
        h->fn(c, h->user);
        return 1;
    }
    Recent *r = &ring[(InterlockedIncrement(&ring_pos) - 1) & (RING - 1)];
    r->call = (uint32_t)i;
    r->thread = GetCurrentThreadId();
    memcpy(r->x, c->x, sizeof r->x);
    r->lr = c->x[30];
    LONG n = InterlockedIncrement(&h->calls);
    if (hos_trace && n <= 3) hos_log("hostcall %s(%#llx, %#llx, %#llx, %#llx)", h->name, (unsigned long long)c->x[0],
                                     (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    h->fn(c, h->user);
    return 1;
}

/* HOS_HLE_STATS=1 (off by default): about every 5 s, how often each HLE function (hos.h's HLE) and each host call ran in
   the measured clock span, most called first: "hle: <total> calls of <n> functions in <elapsed> s", then "<count> <name>;"
   (HLE functions by their full mangled name). scripts/cpu_report.py --calls reads them. HLE functions called from host
   code (one HLE function calling another) count too; the sdk's NVN exports count as their host call (nvn2_dispatch). */
int hos_hle_stats;
static HleCount *volatile hos_hle_list;
static int64_t hos_hle_before_ns;
void hos_hle_counted(HleCount *h, void (*fn)(Ctx *c), Ctx *c) {
    if (!__atomic_load_n(&h->listed, __ATOMIC_ACQUIRE) && !InterlockedExchange(&h->listed, 1)) {
        HleCount *head;
        do h->next = head = __atomic_load_n(&hos_hle_list, __ATOMIC_ACQUIRE);
        while (InterlockedCompareExchangePointer((PVOID volatile *)&hos_hle_list, h, head) != head);
    }
    InterlockedIncrement(&h->calls);
    fn(c);
}
/* (the counts wrap: a 32-bit LONG passes its maximum after 2^31 calls, so a count since the last report is the difference
   taken unsigned) */
typedef struct { const char *name; unsigned long n; } HleRow;
static int hos_hle_row_cmp(const void *a, const void *b) {
    const unsigned long x = ((const HleRow *)a)->n, y = ((const HleRow *)b)->n;
    return (x < y) - (x > y);
}
static void hos_hle_report(void) {
    if (!hos_hle_stats) return;
    const int64_t end_ns = hos_now_ns(), begin_ns = hos_hle_before_ns;
    if (end_ns <= begin_ns) return;
    hos_hle_before_ns = end_ns;
    enum { TOP = HOS_HOSTCALL_MAX + 2048 };
    static HleRow row[TOP];
    int n = 0;
    unsigned long long total = 0;
    for (HleCount *h = __atomic_load_n(&hos_hle_list, __ATOMIC_ACQUIRE); h && n < TOP; h = h->next) {
        const long now = __atomic_load_n(&h->calls, __ATOMIC_RELAXED);
        const unsigned long d = (unsigned long)now - (unsigned long)h->before;
        h->before = now;
        if (d) row[n].name = h->name, row[n].n = d, n++, total += d;
    }
    AcquireSRWLockExclusive(&lock);   /* (the table's names are published under this lock) */
    const LONG nc = ncalls;
    for (LONG i = 0; i < nc && n < TOP; i++) {
        const LONG now = __atomic_load_n(&calls[i].calls, __ATOMIC_RELAXED);
        const unsigned long d = (ULONG)now - (ULONG)calls[i].before;
        calls[i].before = now;
        if (d) row[n].name = calls[i].name, row[n].n = d, n++, total += d;
    }
    ReleaseSRWLockExclusive(&lock);
    qsort(row, n, sizeof *row, hos_hle_row_cmp);
    /* (every row is printed: a row left out would lose its counts for good, as its baseline moved on) */
    hos_log("hle: %llu calls of %d functions in %.9f s; begin_ns=%lld end_ns=%lld", total, n,
            (double)(end_ns - begin_ns) / 1e9, (long long)begin_ns, (long long)end_ns);
    for (int i = 0; i < n; i++) {
        hos_log("hle: %lu %s;", row[i].n, row[i].name);
    }
}
static DWORD WINAPI hos_hle_thread(void *p) {
    (void)p;
    for (;;) {
        Sleep(5000);
        hos_hle_report();
    }
    return 0;
}
/* hos_main.c, before the game starts: counting and the 5 s log from here on */
void hos_hle_stats_start(void) {
    hos_hle_before_ns = hos_now_ns();
    hos_hle_stats = 1;   /* (published to the report thread at creation; the game starts after this function) */
    HANDLE thread = CreateThread(NULL, 0, hos_hle_thread, NULL, 0, NULL);
    if (!thread) { hos_hle_stats = 0; hos_log("hle: call counters could not start; counting stays off"); return; }
    CloseHandle(thread);
}

/* Call guest code from inside a host function (an HLE handler), with up to four arguments, on the current guest
   stack. The guest callee keeps the AArch64 callee-saved registers; LR is restored here. Returns x0. */
uint64_t hos_call_guest(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t lr = c->x[30];
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    c->x[30] = 0;
    a64_icall(c, fn);
    c->x[30] = lr;
    return c->x[0];
}

/* HOS_HOSTCALL_STATS=1: at exit, how often each host call ran (the NVN surface a game really uses). */
void hos_hostcall_stats(void) {
    if (!env_on("HOS_HOSTCALL_STATS")) return;
    fprintf(stderr, "[hos] host calls:\n");
    for (LONG i = 0; i < ncalls; i++) fprintf(stderr, "  %10ld  %s\n", calls[i].calls, calls[i].name);
    LONG end = ring_pos, start = end > RING ? end - RING : 0;
    fprintf(stderr, "[hos] last %ld host calls (oldest first): thread, call(x0, x1, x2, x3) lr\n", end - start);
    for (LONG k = start; k < end; k++) {
        Recent *r = &ring[k & (RING - 1)];
        fprintf(stderr, "  %6u %s(%#llx, %#llx, %#llx, %#llx) lr %#llx\n", r->thread, calls[r->call].name,
                (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2],
                (unsigned long long)r->x[3], (unsigned long long)r->lr);
    }
    fflush(stderr);
}
