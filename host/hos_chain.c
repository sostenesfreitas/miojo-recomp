/* HOS_CHAIN=<path> (diagnostic): the guest threads' blocks and wake-ups, for the frame's critical path.

   While <path>.go exists, every wait that blocks (events, semaphores, message queues, mutexes, condition variables,
   multi-waits), every signal that can end one, every guest sleep and every present is recorded with the time, the
   thread, the object's guest address and the guest caller (lr). When <path>.go goes away (or the buffer is full,
   about 2 million records, 64 MB), the records go to <path>. scripts/chain.py walks each frame back from its present:
   the thread that ran, the wait that stopped it, the signal that ended that wait and the thread that sent it, and so
   on to the frame before; it prints how much of the frame each thread's running time, the wake-ups and the sleeps
   take, and with HOS_SAMPLE_ALL samples of the same run, the functions on that path.

   Off (no HOS_CHAIN): one load and a branch per wait and signal (hos_chain_on). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

volatile int hos_chain_on;

typedef struct {   /* (32 bytes; scripts/chain.py reads them as "<qQQHB5x") */
    int64_t t;          /* hos_now_ns */
    uint64_t key, lr;   /* the object's guest address (a sleep: its length in ns), the guest caller */
    uint16_t thread;    /* index into names */
    uint8_t kind;       /* HOS_CHAIN_* */
    uint8_t pad[5];
} ChainRec;

enum { CHAIN_CAP = 2 << 20, CHAIN_THREADS = 1024 };
static ChainRec *recs;
static volatile LONG64 nrecs;
static HosThread *threads[CHAIN_THREADS];   /* (names resolved when written: games name threads after they start) */
static volatile LONG nthreads;
static _Thread_local int my_index = -1;

void hos_chain_note(int kind, uint64_t key, uint64_t lr) {
    if (!hos_chain_on) return;
    const LONG64 i = InterlockedIncrement64(&nrecs) - 1;
    if (i >= CHAIN_CAP) { hos_chain_on = 0; return; }
    if (my_index < 0) {
        const LONG k = InterlockedIncrement(&nthreads) - 1;
        my_index = k < CHAIN_THREADS ? (int)k : CHAIN_THREADS - 1;
        if (k < CHAIN_THREADS) threads[k] = hos_self();
    }
    if (!lr) {
        Ctx *c = hos_ctx();
        lr = c ? c->x[30] : 0;
    }
    ChainRec *r = &recs[i];
    r->t = hos_now_ns();
    r->key = key;
    r->lr = lr;
    r->thread = (uint16_t)my_index;
    r->kind = (uint8_t)kind;
}

static void chain_write(const char *path) {
    LONG64 n = nrecs;
    if (n > CHAIN_CAP) n = CHAIN_CAP;
    FILE *f = fopen(path, "wb");
    if (!f) { hos_log("chain: cannot write %s", path); return; }
    const int nt = nthreads < CHAIN_THREADS ? (int)nthreads : CHAIN_THREADS;
    fprintf(f, "HOSCHAIN1\n");
    for (int i = 0; i < nt; i++) {
        char name[64];
        if (threads[i]) snprintf(name, sizeof name, "%s", hos_thread_name(threads[i]));
        else snprintf(name, sizeof name, "host-%d", i);   /* (a host thread: the vsync timer, the renderer's) */
        for (char *q = name; *q; q++) if (*q == ' ' || *q == '\n') *q = '_';
        fprintf(f, "thread %d %s\n", i, name);
    }
    fprintf(f, "records %lld\n", (long long)n);
    /* (records still being filled in by a thread that took its index just before the stop: at worst one odd record) */
    fwrite(recs, sizeof *recs, (size_t)n, f);
    fclose(f);
    hos_log("chain: %lld records of %d threads written to %s", (long long)n, nt, path);
}

static DWORD WINAPI chain_thread(void *p) {
    const char *path = (const char *)p;
    char go[1024];
    snprintf(go, sizeof go, "%s.go", path);
    int done = 0;
    for (;;) {
        Sleep(100);
        FILE *g = fopen(go, "rb");
        if (g) fclose(g);
        if (g && !hos_chain_on && !done && !nrecs) {
            recs = (ChainRec *)calloc(CHAIN_CAP, sizeof *recs);
            if (!recs) { hos_log("chain: no memory for the records"); return 0; }
            hos_log("chain: recording (%s)", path);
            hos_chain_on = 1;
        } else if (!done && nrecs && (!g || !hos_chain_on)) {
            hos_chain_on = 0;
            Sleep(50);   /* (notes under way finish) */
            chain_write(path);
            done = 1;   /* (one recording a run) */
        }
    }
    return 0;
}

void hos_chain_start(void) {
    const char *path = getenv("HOS_CHAIN");
    if (path && *path) CreateThread(NULL, 0, chain_thread, (void *)path, 0, NULL);
}
