/* (hookshot) Allocations of the game's "OptimizeChangeBank" thread tried again when their heap has no room, instead of the
   game going on with a null pointer and crashing a few frames later.

   The OptimizeChangeBank thread rebuilds a model's material block (f_804566e0, "gsys::ModelResource(MatBlock)") and takes
   memory for it from heaps its caller names (its second argument's +8 for the CPU's parts, +16 for the GPU's): small heaps
   of their own per model resource ("Model/....bfres", tens of KB), children of a large one. When such a heap is full none
   of these allocations is checked:
   - two CPU buffers from the heap's tryAlloc (vtable +0x30), at 0x8045690c (stored at the MatBlock's +200, its bump
     allocator's memory) and 0x80456da0 (stored at +168, the same base/used/size layout), each sized for the model. A
     null one makes its bump allocator hand out guest address 0: the next store, in f_8044cb30, writes guest 0x8 (issue
     1697: "CRASH: exception 0xc0000005 (write ..., guest 0x8)", thread OptimizeChangeBanc, lr 0x80456d48, every time a few
     frames after loading that player's save, since 2.0.12);
   - the GPU memory, f_80522050 called at 0x80456f18 and 0x80457094, leaves the material block with no memory pool
     (0x80457248), and the block is written to guest address 0 two calls later (TomShafford/Hookshot's crash at
     backflip + attack).
   Each is tried again in the heap's parents up to the root, the first with room (the GPU memory then in the game's
   default heap). Only these call sites are changed: f_80522050's other callers (some 30) are as shipped.

   Freeing: the MatBlock's destructor (f_8093f940) frees +168 and +200 through f_8021dea0, the game's free by address,
   which walks the heap tree for the heap whose range holds the block (read from the lifted code, not traced in a run), so
   a block from a parent goes back to that parent. The model's own heap does not count it (MatBlock +680's accounting
   neither). Every block moved to a parent is counted per call site, and the totals are logged every 16 moves after the
   first 60 log lines (a growing total over a session would be a leak).

   The GPU part is ported from TomShafford/Hookshot 55d68e8cd6 and 65c1998222 (GPL-3.0, a Hookshot fork), by TomShafford;
   its waits before the parents are left out (its own log showed they did not help).

   A sead heap here (vtable 0x84c172c8, the heap class of these): +0x20 its name, +0x28 its memory, +0x30 its size, +0x38 its
   parent, vtable +0x30 tryAlloc(size, alignment), vtable +0x80 its free size (ExpHeap: the free list summed, main+0x834916d0).

   HOS_GPU_ALLOC_RETRY=0: none is tried again (as shipped).
   HOS_GPU_ALLOC_TEST_FAIL=<n> or <gpu>,<cpu> (a test): the first n allocations of each kind at these sites (or that many of
   the GPU memory and of the CPU buffers) are refused by the heap they name, as a full heap would, so the retry runs (with
   HOS_GPU_ALLOC_RETRY=0: the game's crash, as shipped). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEAP_NAME = 0x20, HEAP_START = 0x28, HEAP_SIZE = 0x30, HEAP_PARENT = 0x38, VT_TRY_ALLOC = 0x30, VT_FREE_SIZE = 0x80 };

static int retry_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("HOS_GPU_ALLOC_RETRY") || atoi(getenv("HOS_GPU_ALLOC_RETRY")) != 0;
    return on;
}
/* HOS_GPU_ALLOC_TEST_FAIL: 1 while this kind's first n allocations are refused (kind 0 the GPU memory, 1 the CPU buffer) */
static int test_fail(int kind) {
    static long left[2];
    static volatile int read;   /* (set once both counts are in: no thread decrements a count another is still setting) */
    if (!__atomic_load_n(&read, __ATOMIC_ACQUIRE)) {   /* ("<n>": both kinds; "<gpu>,<cpu>": each its own) */
        static volatile long lock;
        while (__sync_lock_test_and_set(&lock, 1)) {}
        if (!read) {
            const char *e = getenv("HOS_GPU_ALLOC_TEST_FAIL"), *comma = e ? strchr(e, ',') : NULL;
            left[0] = e ? atol(e) : 0;
            left[1] = !e ? 0 : comma ? atol(comma + 1) : atol(e);
            __atomic_store_n(&read, 1, __ATOMIC_RELEASE);
        }
        __sync_lock_release(&lock);
    }
    return __atomic_sub_fetch(&left[kind], 1, __ATOMIC_RELAXED) >= 0;
}
static int readable(uint64_t a) { return a >= 0x80000000ULL && a < (1ULL << 39) - 0x100; }
static int guest_string(Ctx *c, uint64_t at, char *out, int n) {   /* (a guest C string: 1 if it reads as one) */
    int i = 0, ok = readable(at);
    for (; ok && i < n - 1; i++) {
        const char ch = (char)recomp_load8(c, at + i);
        if (!ch) break;
        if (ch < 32 || ch >= 127) ok = 0;
        out[i] = ch;
    }
    if (!ok || i < 2) i = 0, out[i++] = '?', ok = 0;
    out[i] = 0;
    return ok;
}
/* A guest function called as the game would, every register and flag as before but x0: its result */
static uint64_t guest_call(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t lr) {
    uint64_t x[31];
    const uint8_t n = c->n, z = c->z, cf = c->c, v = c->v;
    memcpy(x, c->x, sizeof x);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[30] = lr;
    a64_icall(c, fn);
    const uint64_t r = c->x[0];
    memcpy(c->x, x, sizeof x);
    c->n = n, c->z = z, c->c = cf, c->v = v;
    return r;
}
static uint64_t heap_method(Ctx *c, uint64_t heap, unsigned slot) {   /* (0: none) */
    if (!readable(heap)) return 0;
    const uint64_t vt = recomp_load64(c, heap);
    return readable(vt) ? recomp_load64(c, vt + slot) : 0;
}
static uint64_t free_size(Ctx *c, uint64_t heap) {   /* (the heap's own getFreeSize; ~0: unknown) */
    const uint64_t fn = heap_method(c, heap, VT_FREE_SIZE);
    return fn && a64_lookup(fn) ? guest_call(c, fn, heap, 0, 0, c->x[30]) : ~0ULL;
}
static void describe(Ctx *c, uint64_t heap, char *out, int n) {
    char name[48], kb[32];
    if (!readable(heap)) { snprintf(out, n, "heap %#llx", (unsigned long long)heap); return; }
    if (!guest_string(c, recomp_load64(c, heap + HEAP_NAME), name, sizeof name))
        guest_string(c, recomp_load64(c, heap + HEAP_START), name, sizeof name);
    const uint64_t f = free_size(c, heap);
    if (f == ~0ULL) snprintf(kb, sizeof kb, "?");
    else snprintf(kb, sizeof kb, "%llu", (unsigned long long)(f >> 10));
    snprintf(out, n, "heap %#llx \"%s\" (%llu KB, %s KB free)", (unsigned long long)heap, name,
             (unsigned long long)(recomp_load64(c, heap + HEAP_SIZE) >> 10), kb);
}
static int tell(void) {   /* (the first 60 failures logged) */
    static volatile long told;
    return __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 60;
}
/* Blocks moved to a parent heap, per call site (0, 1: the CPU buffers at 0x8045690c and 0x80456da0; 2: the GPU memory),
   and their bytes: the totals logged every 16th move after the log budget, and with each move while it lasts */
static volatile long moved_n[3], moved_bytes[3];
static void count_moved(int site, uint64_t bytes, int say) {
    static const char *const names[] = {"model buffer (0x8045690c)", "model buffer (0x80456da0)", "GPU memory"};
    const long n = __atomic_add_fetch(&moved_n[site], 1, __ATOMIC_RELAXED);
    const long b = __atomic_add_fetch(&moved_bytes[site], (long)bytes, __ATOMIC_RELAXED);
    if (say || n % 16 == 0)
        hos_log("oot: heap-full retries so far: %s %ld blocks, %ld KB in parent heaps", names[site], n, b >> 10);
}

/* ---- the GPU memory: f_80522050(x0 the memory block, x1 the request: +0 the count, +8 the heap, +0x20 what it is for; x2
   the size of one). nx2/oot_patches.py makes f_804566e0's two calls of it call this, with the game's function ---- */
static int gpu_alloc_in(Ctx *c, A64Fn game, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t lr, uint64_t heap,
                        uint64_t in) {
    recomp_store64(c, a1 + 8, in);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[30] = lr;
    game(c);
    recomp_store64(c, a1 + 8, heap);
    return (int)(c->x[0] & 1);
}
void hos_oot_gpu_alloc(Ctx *c, A64Fn game) {
    const uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], lr = c->x[30];
    const int on = retry_on() && readable(a1);
    const int refused = readable(a1) && recomp_load64(c, a1 + 8) && test_fail(0);
    if (refused && !on) {   /* (the failure alone, as a full heap gives it) */
        hos_log("oot: GPU memory refused by HOS_GPU_ALLOC_TEST_FAIL, not tried again (HOS_GPU_ALLOC_RETRY=0) (lr %#llx)",
                (unsigned long long)lr);
        c->x[0] = 0;
        return;
    }
    if (!refused) {
        game(c);
        if (!on || (c->x[0] & 1)) return;
    }
    const uint64_t count = recomp_load64(c, a1), heap = recomp_load64(c, a1 + 8), what = recomp_load64(c, a1 + 0x20);
    char name[64], where[160];
    guest_string(c, what, name, sizeof name);
    describe(c, heap, where, sizeof where);
    const int say = tell();
    if (say)
        hos_log("oot: GPU memory for %s, %#llx bytes from %s, %s (lr %#llx)", name, (unsigned long long)(count * a2), where,
                refused ? "refused by HOS_GPU_ALLOC_TEST_FAIL" : "failed", (unsigned long long)lr);
    int ok = 0;
    /* (the heaps it was made in: their parents up to the root, the free left in them after their children) */
    uint64_t h = heap;
    for (int up = 0; up < 8 && readable(h) && !ok; up++) {
        const uint64_t parent = recomp_load64(c, h + HEAP_PARENT);
        if (!readable(parent) || parent == h) break;
        h = parent;
        const uint64_t f = free_size(c, h);
        if (f != ~0ULL && f < count * a2 + 0x2000) continue;
        ok = gpu_alloc_in(c, game, a0, a1, a2, lr, heap, h);
        if (say) {
            describe(c, h, where, sizeof where);
            hos_log("oot: GPU memory: %s in its parent %s", ok ? "made" : "not made either", where);
        }
    }
    if (!ok && heap) {   /* (the game's own default when no heap is named: the thread's heap, or the root) */
        ok = gpu_alloc_in(c, game, a0, a1, a2, lr, heap, 0);
        if (say) hos_log("oot: GPU memory: %s in the game's default heap", ok ? "made" : "not made either");
    }
    if (ok) count_moved(2, count * a2, say);
    if (!ok && say) hos_log("oot: GPU memory: no heap had room: the game goes on without it (and crashes writing to it)");
}

/* ---- the CPU buffers: the heap's tryAlloc at 0x8045690c or 0x80456da0 (lr 0x80456910, 0x80456da4) gave x0 = 0 (or
   HOS_GPU_ALLOC_TEST_FAIL refused it: nx2/oot_patches.py then made no call). Tried in each parent with room; x0 the block,
   or 0 still ---- */
int hos_oot_heap_refuse(void) { return test_fail(1); }
void hos_oot_heap_retry(Ctx *c, uint64_t heap, uint64_t size, uint64_t align, uint64_t lr) {
    if (!retry_on()) {
        hos_log("oot: model buffer, %#llx bytes from heap %#llx, failed: not tried again (HOS_GPU_ALLOC_RETRY=0) (lr %#llx)",
                (unsigned long long)size, (unsigned long long)heap, (unsigned long long)lr);
        return;
    }
    char where[160];
    const int say = tell();
    describe(c, heap, where, sizeof where);
    if (say) hos_log("oot: model buffer, %#llx bytes from %s, failed (lr %#llx)", (unsigned long long)size, where,
                     (unsigned long long)lr);
    uint64_t h = heap, got = 0;
    for (int up = 0; up < 8 && readable(h) && !got; up++) {
        const uint64_t parent = recomp_load64(c, h + HEAP_PARENT);
        if (!readable(parent) || parent == h) break;
        h = parent;
        const uint64_t f = free_size(c, h), fn = heap_method(c, h, VT_TRY_ALLOC);
        if ((f != ~0ULL && f < size + 0x2000) || !fn || !a64_lookup(fn)) continue;
        got = guest_call(c, fn, h, size, align, lr);
        if (say) {
            describe(c, h, where, sizeof where);
            hos_log("oot: model buffer: %s in its parent %s", got ? "made" : "not made either", where);
        }
    }
    if (got) count_moved(lr == 0x80456da4ULL, size, say);
    if (!got && say) hos_log("oot: model buffer: no heap had room: the game goes on without it (and crashes writing to it)");
    c->x[0] = got;
}
