/* The native NVN layer's API (HOS_NVN_NATIVE=1): TOTK's NVN calls are answered here instead of by the lifted NVN
   driver over an emulated Switch GPU (hos/gpu.c). See gpu/render/rx_nvn.h for the design; the replay of what is
   recorded here is gpu/render/rx_nvn.cpp.

   - Objects with semantics (device, memory pools, buffers, textures, samplers, descriptor pools, queue, window, sync,
     events, command buffer memory) are implemented as NVN defines them, in place in the game's object memory.
   - Calls that only change GPU state or do GPU work (state objects bound, draws, clears, copies...) are recorded as
     commands in host memory; a submit replays them (rx_nvn_submit) at the submit call, which is when the console reads
     what they point at: TOTK fills deferred texture tables and constant pools for a frame just before its submit and
     rewrites them just after.
   - A function TOTK has not been seen calling stops the game with its name when called (no silent wrong result).

   Calls come from lifted code: arguments are in the guest registers (x0-x7, floats in v0-v7), pointers are guest
   addresses. Adapted from the MIT-licensed NVN layer of d2r-mobile / MacNativeGames (jhaizhou), which runs TOTK on
   Metal: its semantics, TOTK findings and command records; simplified here where noted.

   mkw-recomp: TOTKPort's hos/nvn_native.c (e88744a) for Mario Kart World's NVN2 (API 231.39): the Switch 2 driver's
   device limits, GPU addresses over the 64-bit guest window (identity below 1 TB, virtual pools above), and NVN2's
   own procs (program builders, native images, sparse tile layouts, recording modes). The SDK's exports reach the
   same functions as the proc table (nvn2_dispatch, from gen/nvn2_exports.c). */
#include "hos.h"
#include "../gpu/render/rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif
uint64_t hos_hostcall(const char *name, void (*fn)(Ctx *, void *), void *user);   /* hos_hostcall.c */
void hos_gpu_track(uint64_t guest, uint64_t size, int on);                          /* nvdrv.c */

/* ---- calling convention: arguments from the guest registers ---- */
#define NV(name) static void n_##name(Ctx *c, void *u_)
static void die(const char *fmt, ...);
/* (hookshot) guest memory this layer reads or writes for the game: the n bytes at a, which must lie in the guest window
   (die otherwise). Objects and arrays are guest memory, so their addresses and sizes are checked before use. An
   object argument of no known size (P) is checked for its start (NVN_OBJ_MIN bytes): the window's top region ends at
   the window's end, so an object there may end there too; arrays and objects of known size are checked whole. */
#define NVN_OBJ_MIN 1u
static void *gw(uint64_t a, uint64_t n) {
    void *p = hos_guest_range(a, n);
    if (!p) die("guest memory %#llx + %#llx is outside the guest window", (unsigned long long)a, (unsigned long long)n);
    return p;
}
/* (hookshot) a host pointer read from guest memory (a pool or texture an object names): it must point into the window */
static const void *in_window(const void *p, size_t n) {
    if (!p) return NULL;
    const uintptr_t q = (uintptr_t)p, base = (uintptr_t)A64_MEM;
    if (q < base || !hos_guest_range(q - base, n)) die("object pointer %p is outside the guest window", p);
    return p;
}
#define POOL(p) ((const nvn_pool *)in_window((p), sizeof(nvn_pool)))
#define P(i) ((void *)(c->x[i] ? gw(c->x[i], NVN_OBJ_MIN) : NULL))   /* host pointer of a guest pointer argument */
#define PN(i, n) ((void *)(c->x[i] ? gw(c->x[i], (n)) : NULL))     /* the same for an array of n bytes */
#define I(i) ((int)c->x[i])
#define U32(i) ((uint32_t)c->x[i])
#define U64(i) (c->x[i])
#define BOOL(i) ((int)(c->x[i] & 0xff))               /* NVNboolean: callers do not extend it */
static float fl(const Ctx *c, int i) { float f; memcpy(&f, &c->vreg[i][0], 4); return f; }
#define F(i) fl(c, i)                                  /* the i-th float argument (v0-v7) */
static void retf(Ctx *c, float f) { memset(c->vreg[0], 0, 16); memcpy(&c->vreg[0][0], &f, 4); }
static uint64_t h2g(const void *p) { return p ? (uint64_t)((const uint8_t *)p - A64_MEM) : 0; }
static void *g2h(uint64_t a) { return a ? gw(a, NVN_OBJ_MIN) : NULL; }
#define RETP(p) RET(h2g(p))

static int nvn_log_on(void) { static int v = -1; if (v < 0) v = env_on("HOS_NVN_LOG_OBJECTS"); return v; }
#define OLOG(...) do { if (nvn_log_on()) hos_log("nvn: " __VA_ARGS__); } while (0)
static void die(const char *fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    hos_fatal(NULL, "native NVN: %s", b);
}

/* ---- serials, GPU addresses, virtual pools ---- */
static volatile LONG64 serials;
static uint64_t new_serial(void) { return (uint64_t)InterlockedIncrement64(&serials); }

#define VPAGE 0x10000ull                     /* NVN virtual mapping granularity (64 KiB) */
#define VBASE 0x10000000000ull               /* virtual pools' GPU addresses start at 1 TB: past the 512 GB guest window */
/* page[i]: guest address, 0 unmapped; run[i]: how many pages from i on are contiguous in guest memory (0 unmapped).
   A finalized pool's map stays allocated (dead), so lookups can use a map without the lock: the renderer asks
   tens of thousands of times a frame (render targets live in virtual pools; nvn_gpu_contiguous walked up to 1 GB
   page by page, each page a locked list walk: 8% of the replay thread) */
struct nvn_vmap { uint64_t gpu, size; uint64_t *page; uint32_t *run; struct nvn_vmap *next; };
static struct nvn_vmap *vmaps, *dead_vmaps;
static SRWLOCK vmap_lock = SRWLOCK_INIT;
static volatile LONG vmap_gen = 1;           /* bumped when a map is added or removed */
static volatile LONG vpage_gen = 1;          /* bumped by every virtual page (re)mapping too (the renderer's caches) */
uint32_t nvn_vmap_generation(void) { return (uint32_t)vpage_gen + (uint32_t)vmap_gen; }
static uint64_t next_vbase = VBASE;

static struct nvn_vmap *vmap_find(uint64_t gpu) {
    static _Thread_local struct nvn_vmap *last;
    static _Thread_local LONG last_gen;
    struct nvn_vmap *m = last;
    if (m && last_gen == vmap_gen && gpu >= m->gpu && gpu < m->gpu + m->size) return m;
    AcquireSRWLockShared(&vmap_lock);
    for (m = vmaps; m; m = m->next)
        if (gpu >= m->gpu && gpu < m->gpu + m->size) break;
    last_gen = vmap_gen;
    ReleaseSRWLockShared(&vmap_lock);
    last = m;
    return m;
}
static void vmap_runs(struct nvn_vmap *m) {   /* (vmap_lock held exclusively) */
    const uint64_t n = m->size / VPAGE;
    for (uint64_t i = n; i-- > 0;)
        m->run[i] = !m->page[i] ? 0
                  : i + 1 < n && m->page[i + 1] && m->page[i + 1] == m->page[i] + VPAGE ? m->run[i + 1] + 1 : 1;
}

/* (hookshot) a virtual pool's page table, from the host's list by the pool's GPU address (the pool object is guest memory) */
static struct nvn_vmap *pool_vmap(const nvn_pool *o) {
    if (!(o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || o->gpu < VBASE) return NULL;
    struct nvn_vmap *m = vmap_find(o->gpu);
    return m && m->gpu == o->gpu ? m : NULL;
}
uint64_t nvn_gpu_guest(uint64_t gpu) {
    /* (mkw-recomp: nothing is mapped below the system region at 16 MB: a small address is an unbound buffer, which
       TOTK's fully committed 4 GB window read as zeros and MKW's sparse window faults on) */
    if (gpu < VBASE) return gpu >= HOS_SYSTEM_BASE && gpu < HOS_SPACE_END ? gpu : 0;
    const struct nvn_vmap *m = vmap_find(gpu);
    if (!m) return 0;
    const uint64_t p = m->page[(gpu - m->gpu) / VPAGE];
    return p ? p + (gpu - m->gpu) % VPAGE : 0;
}
uint8_t *nvn_gpu_host(uint64_t gpu) {
    const uint64_t g = nvn_gpu_guest(gpu);
    return g ? (uint8_t *)GP(g) : NULL;
}
uint64_t nvn_gpu_contiguous(uint64_t gpu, uint64_t n) {
    if (gpu < VBASE) {   /* (hookshot: up to the guest window's end, not past it) */
        if (gpu < HOS_SYSTEM_BASE || gpu >= HOS_SPACE_END) return 0;
        return n < HOS_SPACE_END - gpu ? n : HOS_SPACE_END - gpu;
    }
    const struct nvn_vmap *m = vmap_find(gpu);
    if (!m) return 0;
    const uint64_t i = (gpu - m->gpu) / VPAGE;
    if (!m->page[i]) return 0;
    const uint64_t have = (uint64_t)m->run[i] * VPAGE - (gpu - m->gpu) % VPAGE;
    return have < n ? have : n;
}
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        const uint64_t k = nvn_gpu_contiguous(gpu, n);
        const uint8_t *s = k ? nvn_gpu_host(gpu) : NULL;
        if (!s) return 0;
        memcpy(d, s, k);
        d += k, gpu += k, n -= k;
    }
    return 1;
}
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        const uint64_t k = nvn_gpu_contiguous(gpu, n);
        uint8_t *d = k ? nvn_gpu_host(gpu) : NULL;
        if (!d) return 0;
        memcpy(d, s, k);
        HOS_WRITTEN(d, (size_t)k);
        s += k, gpu += k, n -= k;
    }
    return 1;
}

/* ---- device ---- */
static volatile LONG depth_minus_w = 1;   /* (OoT's mode from the start: the shaders prepared before the game made its device were
                                            translated without the depth conversion, and the menu's text and logo fell outside the depth range
                                            on a first start. d3zessete, Hookshot PR 54) */
int nvn_depth_minus_w(void) { return depth_minus_w != 0; }
uint64_t nvn_native_proc(const char *name);

NV(nvnDeviceBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_dev_builder)); }
NV(nvnDeviceBuilderSetFlags) { ((nvn_dev_builder *)P(0))->flags = U32(1); }
NV(nvnDeviceInitialize) {
    nvn_dev *o = (nvn_dev *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('D', 'E', 'V', 0);
    o->flags = ((const nvn_dev_builder *)P(1))->flags;
    hos_log("nvn: native NVN device (flags %#x): no Switch GPU emulation", o->flags);
    RET(1);
}
NV(nvnDeviceFinalize) { ((nvn_dev *)P(0))->magic = 0; }
NV(nvnDeviceGetProcAddress) { RET(U64(1) ? nvn_native_proc((const char *)GP(U64(1))) : 0); }
/* (hookshot) API version 231.40: OoT (SDK 22.2.8) skips its graphics setup unless the minor is >= 40 (main
   0x8348c11c); MKW's driver said 39, which MKW accepts too. */
/* The Switch 2 NVN2 driver's device limits: its nvnDeviceGetInteger (sdk+0x65b680) run for every id under
   Unicorn with a zeroed device. The few it reads from device state (0 there) keep NVN1's value (TOTKPort). */
static const int devinfo[] = {
    [0x00] = 231, [0x01] = 40, [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 32, [0x07] = 4,
    [0x08] = 16, [0x09] = 32, [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 32, [0x0F] = 32,
    [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2,
    [0x18] = 256, [0x19] = 4096, [0x1A] = 16384, [0x1B] = 16, [0x1C] = 1024, [0x1D] = 1024, [0x1E] = 64,
    [0x1F] = 1024, [0x20] = 2147483647, [0x21] = 65535, [0x22] = 65535, [0x23] = 8, [0x24] = 1048576,
    [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 262144, [0x2C] = 16,
    [0x2D] = 2048, [0x2E] = 201, [0x2F] = 201, [0x30] = 20, [0x31] = 5, [0x32] = 1, [0x33] = 8, [0x34] = 8,
    [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1,
    [0x44] = 1, [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1,
    [0x4D] = 16, [0x4E] = 2048, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256,
    [0x54] = 16384, [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096,
    [0x5C] = 16384, [0x5D] = 4096, [0x5E] = 128, [0x5F] = 32, [0x60] = 65535, [0x61] = 1, [0x62] = 0, [0x63] = 256,
    [0x64] = 16, [0x65] = 65535, [0x66] = 65535, [0x67] = 256, [0x68] = 1, [0x69] = 16, [0x6A] = 16, [0x6B] = 16,
    [0x6C] = 1, [0x6D] = 1, [0x6E] = 1, [0x6F] = 256, [0x70] = 0, [0x71] = 16, [0x72] = 8, [0x73] = 16,
    [0x74] = 32, [0x75] = 31, [0x76] = 536870912, [0x77] = 16777216, [0x78] = 16777216, [0x79] = 1073741824,
    [0x7A] = 32, [0x7B] = 1, [0x7C] = 64, [0x7D] = 0, [0x7E] = 1, [0x7F] = 2147483647, [0x80] = 32, [0x81] = 32,
    [0x82] = 4194303, [0x83] = 512, [0x84] = 256, [0x85] = 32, [0x86] = 16383, [0x87] = 0, [0x88] = 8, [0x89] = 0,
    [0x8A] = 0, [0x8B] = 4, [0x8C] = 256, [0x8D] = 16, [0x8E] = 0, [0x8F] = 1, [0x90] = 31, [0x91] = 0,
    [0x92] = 64, [0x93] = 1, [0x94] = 58368, [0x95] = 24576, [0x96] = 0, [0x97] = 256, [0x98] = 262144, [0x99] = 0,
    [0x9A] = 0, [0x9B] = 0, [0x9C] = 201, [0x9D] = 14, [0x9E] = 201, [0x9F] = 20, [0xA0] = 163840, [0xA1] = 8,
    [0xA2] = 4, [0xA3] = 256, [0xA4] = 256, [0xA5] = 4, [0xA6] = 65536, [0xA7] = 201, [0xA8] = 5, [0xA9] = 1,
    [0xAA] = 0, [0xAB] = 63, [0xAC] = 1, [0xAD] = 0, [0xAE] = 1, [0xAF] = 0, [0xB0] = 4096, [0xB1] = 32768,
    [0xB2] = 0, [0xB3] = 0, [0xB4] = 0, [0xB5] = 0, [0xB6] = 0, [0xB7] = 0, [0xB8] = 0, [0xB9] = 0, [0xBA] = 0,
    [0xBB] = 0, [0xBC] = 0, [0xBD] = 0, [0xBE] = 0, [0xBF] = 0,
};
NV(nvnDeviceGetInteger) {
    const unsigned w = U32(1);
    if (w >= sizeof devinfo / sizeof *devinfo) die("device property %#x", w);
    static int devinfo_log = -1;
    if (devinfo_log < 0) devinfo_log = env_on("HOS_NVN_DEVINFO_LOG");
    if (devinfo_log) hos_log("nvn: nvnDeviceGetInteger(%u) -> %d", w, devinfo[w]);
    *(int *)P(2) = devinfo[w];
}
NV(nvnDeviceRegisterFastClearColor) { RET(0); }   /* "not registered": the game keeps its normal clears */
NV(nvnDeviceRegisterFastClearColori) { RET(0); }
NV(nvnDeviceRegisterFastClearColorui) { RET(0); }
NV(nvnDeviceRegisterFastClearDepth) { RET(0); }
NV(nvnDeviceGetImageHandle) { RET((uint64_t)I(1)); }
/* combined handle = texture id (20 bits) | sampler id << 20: the Maxwell layout, which the shaders' handle reads expect */
NV(nvnDeviceGetTextureHandle) {
    const int t = I(1), s = I(2);
    /* The SDK sign-extends both ids before combining them, and texture 0 always means no handle.
       A missing texture can have id -1 (Castle Town's guard house): it stays the all-ones sentinel,
       rather than stopping the game here. Keep the signed ids, including their upper bits. */
    RET(t ? (uint64_t)t | (uint64_t)s << 20 : 0);
}
NV(nvnDeviceGetTexelFetchHandle) { RET((uint64_t)I(1)); }
NV(nvnDeviceGetSeparateTextureHandle) { RET((uint64_t)I(1)); }
NV(nvnDeviceGetSeparateSamplerHandle) { RET((uint64_t)I(1) << 20); }
NV(nvnDeviceInstallDebugCallback) {}
NV(nvnDeviceSetWindowOriginMode) {
    ((nvn_dev *)P(0))->origin = U32(1);
    hos_log("nvn: window origin %s", U32(1) ? "upper left" : "lower left");
}
NV(nvnDeviceSetDepthMode) {
    ((nvn_dev *)P(0))->depth_mode = U32(1);
    const LONG was = InterlockedExchange(&depth_minus_w, U32(1) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    hos_log("nvn: depth mode %s", U32(1) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? "near = -w (GL)" : "near = 0");
    if (was != (LONG)(depth_minus_w != 0))
        hos_log("nvn: the depth mode is not the one the prepared shaders were made for: their vertex stages draw wrong");
}
NV(nvnDeviceFinalizeCommandHandle) {}
/* (mkw-recomp) GPU time on the CPU's tick clock (31.25 MHz: 32 ns a tick), as on the console: MKW's frame pacer
   (0x806bced0) takes a timestamp report, ConvertToTick's it and subtracts its own GetSystemTick of the frame's start.
   From hos_now_ns (since boot, where the ticks count from the start) that came to the PC's uptime: always late, the
   game stayed at 30 fps */
NV(nvnDeviceGetCurrentTimestampInNanoseconds) { RET(hos_ticks() * (1000000000ull / HOS_TICK_HZ)); }
/* counters hold Tegra GPU ticks (19.2 MHz): ns = ticks * 625 / 12 */
NV(nvnDeviceGetTimestampInNanoseconds) {
    const uint64_t t = ((const NVNcounterData *)P(1))->timestamp;
    RET(t / 12 * 625 + t % 12 * 625 / 12);
}
NV(nvnDeviceSetDebugLabel) {}
NV(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NV(nvnDeviceSetIntermediateShaderCache) {}

/* ---- memory pools ---- */
NV(nvnMemoryPoolBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_pool_builder)); }
NV(nvnMemoryPoolBuilderSetDevice) { ((nvn_pool_builder *)P(0))->dev = P(1); }
NV(nvnMemoryPoolBuilderSetStorage) { ((nvn_pool_builder *)P(0))->mem = U64(1); ((nvn_pool_builder *)P(0))->size = U64(2); }
NV(nvnMemoryPoolBuilderSetFlags) { ((nvn_pool_builder *)P(0))->flags = U32(1); }
NV(nvnMemoryPoolInitialize) {
    const nvn_pool_builder *b = (const nvn_pool_builder *)P(1);
    nvn_pool *o = (nvn_pool *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('P', 'O', 'O', 'L');
    o->flags = b->flags;
    o->size = b->size;
    if (o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        if (b->mem || !o->size || o->size % VPAGE) die("virtual pool storage %#llx size %#llx", (unsigned long long)b->mem,
                                                       (unsigned long long)o->size);
        struct nvn_vmap *m = (struct nvn_vmap *)calloc(1, sizeof *m);
        m->page = (uint64_t *)calloc(o->size / VPAGE, sizeof(uint64_t));
        m->run = (uint32_t *)calloc(o->size / VPAGE, sizeof(uint32_t));
        if (!m->page || !m->run || o->size >= VBASE) die("virtual pool of %#llx bytes", (unsigned long long)o->size);
        AcquireSRWLockExclusive(&vmap_lock);
        m->gpu = next_vbase;
        m->size = o->size;
        next_vbase += (o->size + VPAGE - 1) & ~(VPAGE - 1);
        m->next = vmaps;
        vmaps = m;
        InterlockedIncrement(&vmap_gen);
        ReleaseSRWLockExclusive(&vmap_lock);
        o->gpu = m->gpu;
        OLOG("virtual pool %#llx bytes at GPU %#llx", (unsigned long long)o->size, (unsigned long long)o->gpu);
        if (getenv("HOS_NVN_LOG_VMAP"))
            hos_log("nvn: virtual pool %#llx bytes at GPU %#llx (flags %#x, lr %#llx)", (unsigned long long)o->size,
                    (unsigned long long)o->gpu, o->flags, (unsigned long long)c->x[30]);
        RET(1);
        return;
    }
    if (!b->mem) die("memory pool without storage (flags %#x size %#llx)", o->flags, (unsigned long long)o->size);
    gw(b->mem, o->size);   /* (hookshot: the storage lies in guest memory) */
    o->mem = b->mem;
    o->gpu = b->mem;
    hos_gpu_track(o->gpu, o->size, 1);   /* (the CPU's writes to it reach the renderer through write watch) */
    rx_nvn_pool_created(o->gpu, o->size);
    void hos_prompts_pool(uint64_t mem, uint64_t size);   /* (hookshot, prompts.c: the button prompts' textures, changed in place) */
    hos_prompts_pool(o->mem, o->size);
    OLOG("pool %#llx size %#llx flags %#x", (unsigned long long)o->gpu, (unsigned long long)o->size, o->flags);
    RET(1);
}
/* (hookshot) A memory-pool call made with no pool: the game passed 0 while the banc changed (thread
   OptimizeChangeBanc, Kakariko's day/night transition), and flags at +4 of it crashed. 0 then, and who asked, once.
   P(0) returns NULL for a 0 argument, but dereferencing that NULL is undefined behaviour: the compiler folds the
   check away and reads A64_MEM + offset instead, so the guard has to come before the dereference. */
static int no_pool(Ctx *c, const char *what) {
    if (c->x[0]) return 0;
    static int told;
    if (told++ < 4) hos_log("nvn: %s of no memory pool (lr %#llx): 0", what, (unsigned long long)c->x[30]);
    return 1;
}
NV(nvnMemoryPoolFinalize) {
    if (no_pool(c, "finalize")) return;
    nvn_pool *o = (nvn_pool *)P(0);
    struct nvn_vmap *vm = pool_vmap(o);
    if (vm) {   /* (its GPU range is not reused) */
        int mapped = 0;   /* (pages still mapped: what the GPU drew there goes into their memory first) */
        for (uint64_t k = 0; k < vm->size / VPAGE && !mapped; k++) mapped = vm->page[k] != 0;
        if (mapped) rx_nvn_vmap_changing(vm->gpu, vm->size);
        AcquireSRWLockExclusive(&vmap_lock);
        int found = 0;
        for (struct nvn_vmap **p = &vmaps; *p; p = &(*p)->next)
            if (*p == vm) {
                *p = vm->next;
                found = 1;
                break;
            }
        if (found) {   /* (kept: a lookup may still hold it) */
            vm->next = dead_vmaps;
            dead_vmaps = vm;
        }
        InterlockedIncrement(&vmap_gen);
        ReleaseSRWLockExclusive(&vmap_lock);
    } else if (o->mem) {
        hos_gpu_track(o->gpu, o->size, 0);
        void hos_prompts_pool_gone(uint64_t mem);   /* (hookshot, prompts.c: the prompts' textures no longer written there) */
        hos_prompts_pool_gone(o->mem);
    }
    o->magic = 0;
}
NV(nvnMemoryPoolGetFlags) {
    if (no_pool(c, "flags")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->flags);
}
NV(nvnMemoryPoolGetSize) {
    if (no_pool(c, "size")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->size);
}
NV(nvnMemoryPoolMap) {
    if (no_pool(c, "map")) { RET(0); return; }
    const nvn_pool *o = (const nvn_pool *)P(0);
    if (o->magic != NVN_MAGIC('P', 'O', 'O', 'L') || (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) { RET(0); return; }
    if (!o->mem) die("CPU mapping of a virtual pool");
    RET(o->mem);
}
NV(nvnMemoryPoolGetBufferAddress) {
    if (no_pool(c, "buffer address")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->gpu);
}
NV(nvnMemoryPoolFlushMappedRange) {}   /* CPU writes are seen through write watch */
NV(nvnMemoryPoolInvalidateMappedRange) {   /* the CPU is about to read what the GPU wrote */
    if (no_pool(c, "invalidate mapped range")) return;
    const nvn_pool *o = (const nvn_pool *)P(0);
    if ((int64_t)U64(1) >= 0) rx_nvn_invalidate(o->gpu + U64(1), U64(2));
}
NV(nvnMemoryPoolSetDebugLabel) {}
/* NVNmappingRequest (0x28 bytes): physical pool (guest pointer), physical offset, virtual offset, size, storage class */
NV(nvnMemoryPoolMapVirtual) {
    if (no_pool(c, "map virtual")) return;
    nvn_pool *v = (nvn_pool *)P(0);
    const int n = I(1);
    struct nvn_vmap *vm = pool_vmap(v);
    if (!vm || n < 0) die("MapVirtual on a pool that is not virtual");
    const uint8_t *r = (const uint8_t *)PN(2, 0x28ull * (uint64_t)n);
    for (int i = 0; i < n; i++) {
        uint64_t pool, size;
        int64_t poff, voff;
        memcpy(&pool, r + 0x28 * i, 8);
        memcpy(&poff, r + 0x28 * i + 8, 8);
        memcpy(&voff, r + 0x28 * i + 16, 8);
        memcpy(&size, r + 0x28 * i + 24, 8);
        if (voff < 0 || poff < 0 || !size || size > vm->size || (uint64_t)voff > vm->size - size ||
            ((uint64_t)voff | (uint64_t)poff | size) % VPAGE)
            die("MapVirtual request %d: virtual %#llx physical %#llx size %#llx", i, (unsigned long long)voff,
                (unsigned long long)poff, (unsigned long long)size);
        const nvn_pool *p = pool ? (const nvn_pool *)gw(pool, sizeof(nvn_pool)) : NULL;
        /* (hookshot: the physical pages must lie in guest memory; the pool object is guest memory) */
        if (p && (!p->mem || p->gpu != p->mem || size > p->size || (uint64_t)poff > p->size - size ||
                  !hos_guest_range(p->gpu + (uint64_t)poff, size)))
            die("MapVirtual request %d: physical pool", i);
        {   /* (pages mapped now that change: what the GPU drew into textures there goes into their memory first, while
               the old mapping holds: MK World draws its course maps' tiles in small pools and moves their pages) */
            int mapped = 0;
            for (uint64_t k = 0; k < size / VPAGE && !mapped; k++) mapped = vm->page[(uint64_t)voff / VPAGE + k] != 0;
            if (mapped) rx_nvn_vmap_changing(v->gpu + (uint64_t)voff, size);
        }
        {   /* (HOS_NVN_LOG_VMAP=<lowest pool GPU address, hex>: the requests into pools from there, 3000 lines; how many
               in all every 2000) */
            static int log_vmap = -1;
            static uint64_t vmap_from;
            static volatile LONG logged, total;
            if (log_vmap < 0) {
                const char *e = getenv("HOS_NVN_LOG_VMAP");
                log_vmap = e != NULL;
                vmap_from = e ? strtoull(e, NULL, 16) : 0;
                if (vmap_from == 1) vmap_from = 0;
            }
            const LONG t = InterlockedIncrement(&total);
            if (log_vmap && ((v->gpu >= vmap_from && InterlockedIncrement(&logged) <= 3000) || t % 2000 == 0)) {
                uint64_t nz = 0;   /* (what the physical pages hold as they are mapped) */
                if (p && p->mem)
                    for (uint64_t k = 0; k < size; k++) nz += ((const uint8_t *)GP(p->mem))[(uint64_t)poff + k] != 0;
                hos_log("nvn: MapVirtual #%ld: pool %#llx (%#llx bytes) +%#llx..+%#llx -> %s %#llx+%#llx (flags %#x), %llu bytes "
                        "not zero there (lr %#llx)", (long)t, (unsigned long long)v->gpu, (unsigned long long)v->size,
                        (unsigned long long)voff, (unsigned long long)((uint64_t)voff + size), p ? "pool" : "unmapped",
                        (unsigned long long)(p ? p->gpu : 0), (unsigned long long)poff, p ? p->flags : 0,
                        (unsigned long long)nz, (unsigned long long)c->x[30]);
            }
        }
        AcquireSRWLockExclusive(&vmap_lock);
        for (uint64_t k = 0; k < size / VPAGE; k++)
            vm->page[(uint64_t)voff / VPAGE + k] = p ? p->gpu + (uint64_t)poff + k * VPAGE : 0;
        vmap_runs(vm);
        InterlockedIncrement(&vpage_gen);
        ReleaseSRWLockExclusive(&vmap_lock);
    }
    RET(1);
}

/* ---- buffers (a range of a pool) ---- */
NV(nvnBufferBuilderSetDevice) { ((nvn_buffer_builder *)P(0))->dev = P(1); }
NV(nvnBufferBuilderSetDefaults) {
    nvn_buffer_builder *b = (nvn_buffer_builder *)P(0);
    void *d = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = d;
}
NV(nvnBufferBuilderSetStorage) {
    nvn_buffer_builder *b = (nvn_buffer_builder *)P(0);
    b->pool = (nvn_pool *)P(1);
    b->off = (int64_t)U64(2);
    b->size = U64(3);
}
NV(nvnBufferInitialize) {
    const nvn_buffer_builder *b = (const nvn_buffer_builder *)P(1);
    const nvn_pool *bp = POOL(b->pool);
    if (!bp || bp->magic != NVN_MAGIC('P', 'O', 'O', 'L') || b->off < 0 || (uint64_t)b->off > bp->size ||
        b->size > bp->size - (uint64_t)b->off) { RET(0); return; }
    nvn_buffer *o = (nvn_buffer *)P(0);
    o->magic = NVN_MAGIC('B', 'U', 'F', 0);
    o->pool = b->pool;
    o->off = b->off;
    o->size = b->size;
    RET(1);
}
NV(nvnBufferFinalize) { memset(P(0), 0, sizeof(nvn_buffer)); }
/* (hookshot, issue 287) A buffer with no memory pool, mapped: the game filled 0x3d0 bytes at what it got back, 0, and crashed
   on a scene change (Hyrule Castle / Castle Town / Hyrule Field, Kakariko day to night; thread OptimizeChangeBanc, right after
   "flags of no memory pool"). Zeroed scratch memory in the game's address space instead, as large as the biggest asked for
   (never freed: it comes rarely), and who asked, once. */
static uint64_t scratch_map(uint64_t size, uint64_t lr) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint64_t at, have;
    static int told;
    AcquireSRWLockExclusive(&lock);
    if (size > have) {
        have = (size + 0xffff) & ~0xffffull;
        at = hos_sys_alloc(have, 0x1000);
    }
    const uint64_t r = at;
    if (told++ < 4) hos_log("nvn: a buffer of no memory pool mapped (lr %#llx): %#llx bytes of scratch", (unsigned long long)lr,
                            (unsigned long long)size);
    ReleaseSRWLockExclusive(&lock);
    return r;
}
NV(nvnBufferMap) {
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    if (!o) { RET(0); return; }
    if (!o->pool) { RET(scratch_map(o->size ? o->size : 0x1000, c->x[30])); return; }
    const nvn_pool *pool = POOL(o->pool);
    if (!pool->mem) { RET(0); return; }
    RET(pool->mem + (uint64_t)o->off);
}
NV(nvnBufferGetAddress) {
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    RET(o->pool ? POOL(o->pool)->gpu + (uint64_t)o->off : 0);
}
NV(nvnBufferGetMemoryPool) { RETP(((const nvn_buffer *)P(0))->pool); }
NV(nvnBufferGetMemoryOffset) { RET((uint64_t)((const nvn_buffer *)P(0))->off); }
NV(nvnBufferGetSize) { RET(((const nvn_buffer *)P(0))->size); }
NV(nvnBufferFlushMappedRange) {}
NV(nvnBufferInvalidateMappedRange) {
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    if (o->pool) rx_nvn_invalidate(POOL(o->pool)->gpu + (uint64_t)o->off + U64(1), U64(2));
}
NV(nvnBufferSetDebugLabel) {}

/* ---- descriptor pools ---- */
NV(nvnTexturePoolInitialize) {
    nvn_dpool *o = (nvn_dpool *)P(0);
    o->pool = (const nvn_pool *)P(1);
    o->off = (int64_t)U64(2);
    o->n = I(3);
    rx_nvn_set_texture_pool_size(o->n);
    hos_log("nvn: texture pool of %d descriptors", o->n);
    RET(1);
}
NV(nvnTexturePoolFinalize) {}
NV(nvnTexturePoolSetDebugLabel) {}
NV(nvnTexturePoolRegisterTexture) { rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3)); }
NV(nvnSamplerPoolInitialize) {
    nvn_dpool *o = (nvn_dpool *)P(0);
    o->pool = (const nvn_pool *)P(1);
    o->off = (int64_t)U64(2);
    o->n = I(3);
    RET(1);
}
NV(nvnSamplerPoolFinalize) {}
NV(nvnSamplerPoolSetDebugLabel) {}
NV(nvnSamplerPoolRegisterSampler) { rx_nvn_register_sampler(I(1), &((const nvn_samp *)P(2))->p); }

/* ---- textures ---- */
/* (hookshot) A texture builder of no address: a scratch one instead, said once. The game passed 0 after a long freeze (the
   ocarina note sheet's first build, combat in Hyrule Field) and nvnTextureBuilderSetSwizzle read A64_MEM + 0x5c (issue 500);
   a builder set up wrongly is a missing texture, not a crash. As PR 510 for memory pools. */
static nvn_tex_builder *tb_or_scratch(void *p, uint64_t lr) {
    static nvn_tex_builder scratch;
    if (p) return (nvn_tex_builder *)p;
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 4) hos_log("nvn: a texture builder of no address (lr %#llx): a scratch one", (unsigned long long)lr);
    memset(&scratch, 0, sizeof scratch);
    return &scratch;
}
#define TB(p) tb_or_scratch((p), c->x[30])
#define CTB(p) ((const nvn_tex_builder *)tb_or_scratch((p), c->x[30]))
#define TX(p) ((nvn_tex *)(p))
#define CTX(p) ((const nvn_tex *)(p))
NV(nvnTextureBuilderSetDefaults) {
    nvn_tex_builder *b = TB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->d.swizzle_r = 2, b->d.swizzle_g = 3, b->d.swizzle_b = 4, b->d.swizzle_a = 5;
    b->d.target = NVN_TEXTURE_TARGET_2D;
    b->d.width = b->d.height = b->d.depth = b->d.levels = 1;
}
NV(nvnTextureBuilderSetDevice) { TB(P(0))->dev = P(1); }
NV(nvnTextureBuilderSetFlags) { TB(P(0))->d.flags = I(1); }
NV(nvnTextureBuilderSetTarget) { TB(P(0))->d.target = I(1); }
NV(nvnTextureBuilderSetWidth) { TB(P(0))->d.width = U32(1); }
NV(nvnTextureBuilderSetHeight) { TB(P(0))->d.height = U32(1); }
NV(nvnTextureBuilderSetDepth) { TB(P(0))->d.depth = U32(1); }
NV(nvnTextureBuilderSetSize1D) { nvn_tex_builder *b = TB(P(0)); b->d.width = U32(1); b->d.height = b->d.depth = 1; }
NV(nvnTextureBuilderSetSize2D) {
    nvn_tex_builder *b = TB(P(0));
    b->d.width = U32(1), b->d.height = U32(2), b->d.depth = 1;
    {   /* (hookshot) HOS_SIZE_TRACE=1: who asks for the picture's sizes (the output and the scene: for wider screens) */
        static int on = -1;
        static volatile LONG left = 24;
        if (on < 0) on = env_on("HOS_SIZE_TRACE");
        const uint32_t w = U32(1);
        if (on && (w == 2560 || w == 2048 || w == 1536 || w == 3840 || w == 1920) && InterlockedDecrement(&left) >= 0) {
            void hos_backtrace(void);
            hos_log("nvn: texture size %ux%u (lr %#llx)", w, U32(2), (unsigned long long)c->x[30]);
            hos_backtrace();
        }
    }
}
NV(nvnTextureBuilderSetSize3D) { nvn_tex_builder *b = TB(P(0)); b->d.width = U32(1); b->d.height = U32(2); b->d.depth = U32(3); }
NV(nvnTextureBuilderSetLevels) { TB(P(0))->d.levels = U32(1); }
NV(nvnTextureBuilderSetFormat) { TB(P(0))->d.format = I(1); }
NV(nvnTextureBuilderSetDepthStencilMode) {
    if (I(1) != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) die("stencil-plane texture sampling");
}
NV(nvnTextureBuilderSetSwizzle) {
    nvn_tex_builder *b = TB(P(0));
    for (int i = 1; i <= 4; i++) if (U32(i) > 5) die("texture swizzle %u", U32(i));
    b->d.swizzle_r = U32(1), b->d.swizzle_g = U32(2), b->d.swizzle_b = U32(3), b->d.swizzle_a = U32(4);
}
NV(nvnTextureBuilderSetSamples) { TB(P(0))->d.samples = I(1); }
NV(nvnTextureBuilderSetStride) { TB(P(0))->d.stride = (uint32_t)U64(1); }
NV(nvnTextureBuilderSetStorage) { TB(P(0))->pool = (nvn_pool *)P(1); TB(P(0))->off = (int64_t)U64(2); }
NV(nvnTextureBuilderSetPackagedTextureLayout) {
    nvn_tex_builder *b = TB(P(0));
    const NVNpackagedTextureLayout *l = (const NVNpackagedTextureLayout *)P(1);
    b->has_layout = l != NULL;
    if (l) memcpy(b->layout, l->layout, 8);
    b->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NV(nvnTextureBuilderSetPackagedTextureData) { TB(P(0))->pkg_data = U64(1); }
NV(nvnTextureBuilderGetPackagedTextureData) { RET(CTB(P(0))->pkg_data); }
/* storage classes are opaque identifiers here: 0 buffer textures, 1 the rest (not a Tegra memory kind) */
static int storage_class(const nvn_texdesc *d) { return d->target == NVN_TEXTURE_TARGET_BUFFER ? 0 : 1; }
NV(nvnTextureBuilderGetStorageClass) { RET(storage_class(&CTB(P(0))->d)); }
NV(nvnTextureBuilderGetFlags) { RET((uint32_t)CTB(P(0))->d.flags); }
/* (hookshot, issues 284 and 287) A size or alignment asked of no builder: the game passed 0 here while a scene loaded (thread
   OptimizeChangeBanc, entering Castle Town), and the format read at +4 of it crashed the game. Size 0 then, and who asked, once */
static int no_builder(Ctx *c, const char *what) {
    if (c->x[0]) return 0;
    static int told;
    if (told++ < 4) hos_log("nvn: %s of no texture builder (lr %#llx): 0", what, (unsigned long long)c->x[30]);
    return 1;
}
NV(nvnTextureBuilderGetStorageSize) {
    if (no_builder(c, "storage size")) { RET(0); return; }
    nvn_texlayout L;
    nvn_tex_layout(&CTB(P(0))->d, &L);
    if (!L.bpb) hos_log("nvn: texture format %#x unknown: storage size 0", CTB(P(0))->d.format);
    RET(L.size);
}
NV(nvnTextureBuilderGetStorageAlignment) {
    if (no_builder(c, "storage alignment")) { RET(512); return; }
    nvn_texlayout L;
    nvn_tex_layout(&CTB(P(0))->d, &L);
    RET(L.align);
}
NV(nvnTextureBuilderGetZCullStorageSize) { RET(0); }   /* no ZCull: nothing to store */
NV(nvnTextureBuilderGetTarget) { RET((uint32_t)CTB(P(0))->d.target); }
NV(nvnTextureBuilderGetWidth) { RET(CTB(P(0))->d.width); }
NV(nvnTextureBuilderGetHeight) { RET(CTB(P(0))->d.height); }
NV(nvnTextureBuilderGetDepth) { RET(CTB(P(0))->d.depth); }
NV(nvnTextureBuilderGetLevels) { RET(CTB(P(0))->d.levels); }
NV(nvnTextureBuilderGetFormat) { RET((uint32_t)CTB(P(0))->d.format); }
NV(nvnTextureBuilderGetSamples) { RET((uint32_t)CTB(P(0))->d.samples); }
NV(nvnTextureBuilderGetStride) { RET(CTB(P(0))->d.stride); }
NV(nvnTextureBuilderGetMemoryOffset) { RET((uint64_t)CTB(P(0))->off); }
NV(nvnTextureBuilderGetViewOffset) {
    if (no_builder(c, "view offset")) { RET(0); return; }
    const nvn_tex_builder *b = CTB(P(0));
    const nvn_view *w = (const nvn_view *)P(1);
    if (!w) { RET(0); return; }
    nvn_texlayout L;
    nvn_tex_layout(&b->d, &L);
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&b->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * L.layer_size;
    RET(off);
}

/* the texture currently initialised at each storage address (nvn_tex_live_at) */
#define LIVE_SLOTS 65536
static struct { uint64_t gpu; nvn_tex t; } live[LIVE_SLOTS];
static SRWLOCK live_lock = SRWLOCK_INIT;
static unsigned live_slot(uint64_t gpu) { return (unsigned)((gpu >> 9) * 0x9E3779B97F4A7C15ull >> 48) & (LIVE_SLOTS - 1); }
static void live_put(const nvn_tex *t) {
    if (!t->gpu) return;
    AcquireSRWLockExclusive(&live_lock);
    unsigned s = live_slot(t->gpu), k;
    for (k = 0; k < 64; k++) {   /* (open addressing over a short run: a stale entry there is simply replaced) */
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == t->gpu || !live[j].gpu) {
            live[j].gpu = t->gpu;
            live[j].t = *t;
            break;
        }
    }
    if (k == 64) {
        live[s].gpu = t->gpu;
        live[s].t = *t;
    }
    ReleaseSRWLockExclusive(&live_lock);
}
static void live_del(const nvn_tex *t) {
    if (!t->gpu) return;
    AcquireSRWLockExclusive(&live_lock);
    const unsigned s = live_slot(t->gpu);
    for (unsigned k = 0; k < 64; k++) {
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == t->gpu) {
            if (live[j].t.serial == t->serial) live[j].gpu = 0, live[j].t.serial = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&live_lock);
}
int nvn_tex_live_at(uint64_t gpu, nvn_tex *out) {
    int found = 0;
    AcquireSRWLockShared(&live_lock);
    const unsigned s = live_slot(gpu);
    for (unsigned k = 0; k < 64; k++) {
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == gpu) {
            *out = live[j].t;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&live_lock);
    return found;
}

void hos_watch_guest_writes(const uint8_t *data, uint64_t size);   /* (below) */
NV(nvnTextureInitialize) {
    const nvn_tex_builder *b = CTB(P(1));
    nvn_tex *o = TX(P(0));
    if (o->magic == NVN_MAGIC('T', 'E', 'X', 0) && o->serial) {   /* initialised again without Finalize */
        live_del(o);
        rx_nvn_texture_released(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('T', 'E', 'X', 0);
    o->d = b->d;
    o->pool = b->pool;
    o->off = b->off;
    o->has_layout = b->has_layout;
    memcpy(o->layout, b->layout, 8);
    nvn_tex_layout(&o->d, &o->L);
    o->gpu = b->pool ? POOL(b->pool)->gpu + (uint64_t)b->off : 0;
    if (b->pkg_data) {   /* packaged data: copied into the storage (a CPU write the dirty tracking sees) */
        if (!o->gpu || !nvn_gpu_write(o->gpu, gw(b->pkg_data, o->L.size), o->L.size)) die("packaged texture data without storage");
    }
    o->serial = new_serial();
    live_put(o);
    rx_nvn_texture_created(o);
    {   /* RX_NVN_WATCH_TEX=<w>x<h> (debugging): who writes the storage of textures that size (a page watch, with call
           stacks: hos_watch_guest_writes), for writes the dirty tracking misses */
        static int on = -1;
        static unsigned ww, wh;
        if (on < 0) on = getenv("RX_NVN_WATCH_TEX") && sscanf(getenv("RX_NVN_WATCH_TEX"), "%ux%u", &ww, &wh) == 2;
        if (on && o->d.width == ww && o->d.height == wh && o->gpu && o->gpu < VBASE)
            hos_watch_guest_writes(GP(o->gpu), o->L.size);
    }
    OLOG("texture %ux%ux%u levels %u format %#x target %d flags %#x at %#llx size %#llx", o->d.width, o->d.height,
         o->d.depth, o->d.levels, o->d.format, o->d.target, o->d.flags, (unsigned long long)o->gpu,
         (unsigned long long)o->L.size);
    RET(1);
}
NV(nvnTextureFinalize) {
    nvn_tex *o = TX(P(0));
    live_del(o);
    rx_nvn_texture_released(o);
    o->magic = 0;
}
NV(nvnTextureGetStorageClass) { RET(storage_class(&CTX(P(0))->d)); }
NV(nvnTextureGetFlags) { RET((uint32_t)CTX(P(0))->d.flags); }
NV(nvnTextureGetWidth) { RET(CTX(P(0))->d.width); }
NV(nvnTextureGetHeight) { RET(CTX(P(0))->d.height); }
NV(nvnTextureGetDepth) { RET(CTX(P(0))->d.depth); }
NV(nvnTextureGetLevels) { RET(CTX(P(0))->d.levels); }
NV(nvnTextureGetFormat) { RET((uint32_t)CTX(P(0))->d.format); }
NV(nvnTextureGetSamples) { RET((uint32_t)CTX(P(0))->d.samples); }
NV(nvnTextureGetTarget) { RET((uint32_t)CTX(P(0))->d.target); }
NV(nvnTextureGetStride) { RET(CTX(P(0))->d.stride); }
NV(nvnTextureGetStorageSize) { RET(CTX(P(0))->L.size); }
NV(nvnTextureGetZCullStorageSize) { RET(0); }
NV(nvnTextureGetTextureAddress) { RET(CTX(P(0))->gpu); }
NV(nvnTextureGetMemoryOffset) { RET((uint64_t)CTX(P(0))->off); }
NV(nvnTextureGetDebugID) { RET(CTX(P(0))->serial); }
NV(nvnTextureSetDebugLabel) {}
NV(nvnTextureGetSwizzle) {
    const nvn_texdesc *d = &CTX(P(0))->d;
    *(int *)P(1) = d->swizzle_r, *(int *)P(2) = d->swizzle_g, *(int *)P(3) = d->swizzle_b, *(int *)P(4) = d->swizzle_a;
}
NV(nvnTextureCompare) {
    const nvn_tex *x = CTX(P(0)), *y = CTX(P(1));
    RET(!memcmp(&x->d, &y->d, sizeof x->d) && x->gpu == y->gpu);
}
NV(nvnTextureGetRawStorageClass) { RET(1); }   /* (no prototype; callers compare it with 1) */
#define VW(p) ((nvn_view *)(p))
#define CVW(p) ((const nvn_view *)(p))
NV(nvnTextureViewSetDefaults) { memset(P(0), 0, sizeof(nvn_view)); }
NV(nvnTextureViewSetLevels) {
    if (U32(1) > 255 || U32(2) > 255) die("texture view levels %u/%u", U32(1), U32(2));
    nvn_view *v = VW(P(0));
    v->set_levels = 1, v->base_level = (uint8_t)U32(1), v->nlevels = (uint8_t)U32(2);
}
NV(nvnTextureViewSetLayers) {
    if (U32(1) > 65535 || U32(2) > 65535) die("texture view layers %u/%u", U32(1), U32(2));
    nvn_view *v = VW(P(0));
    v->set_layers = 1, v->min_layer = (uint16_t)U32(1), v->nlayers = (uint16_t)U32(2);
}
NV(nvnTextureViewSetFormat) { VW(P(0))->set_format = 1; VW(P(0))->format = (uint16_t)U32(1); }
NV(nvnTextureViewSetTarget) { VW(P(0))->set_target = 1; VW(P(0))->target = (uint8_t)U32(1); }
NV(nvnTextureViewSetSwizzle) {
    nvn_view *v = VW(P(0));
    for (int i = 1; i <= 4; i++) if (U32(i) > 5) die("texture view swizzle %u", U32(i));
    v->set_swizzle = 1;
    for (int i = 0; i < 4; i++) v->swz[i] = (uint8_t)U32(i + 1);
}
NV(nvnTextureViewSetDepthStencilMode) {
    if (I(1) != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) die("stencil-plane texture view sampling");
}
NV(nvnTextureViewGetLevels) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->base_level; *(int *)P(2) = v->nlevels; RET(v->set_levels); }
NV(nvnTextureViewGetLayers) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->min_layer; *(int *)P(2) = v->nlayers; RET(v->set_layers); }
NV(nvnTextureViewGetFormat) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->format; RET(v->set_format); }
NV(nvnTextureViewGetTarget) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->target; RET(v->set_target); }
NV(nvnTextureGetViewOffset) {
    const nvn_tex *t = CTX(P(0));
    const nvn_view *w = CVW(P(1));
    if (!w) { RET(0); return; }
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&t->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * t->L.layer_size;
    RET(off);
}
/* RX_NVN_WATCH_TEXELS_SRC=<w>x<h> (debugging): after each WriteTexels into a texture that size its source buffer's pages are made read-only, and the
   next write to them is logged with the writer's call stack (f_XXXXXXXX frames are guest code), then allowed */
static uintptr_t wts_lo[16], wts_hi[16];
static int wts_n;
static volatile LONG wts_reports;
void hos_backtrace(void);
static LONG CALLBACK wts_handler(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    for (int i = 0; i < wts_n; i++)
        if (a >= wts_lo[i] && a < wts_hi[i]) {
            HosThread *th = hos_self();
            const char *tn = th ? hos_thread_name(th) : "?";
            static ULONGLONG after = ~0ull;   /* RX_NVN_WATCH_AFTER=<seconds>: only writes after that are reported */
            if (after == ~0ull) after = GetTickCount64() + (getenv("RX_NVN_WATCH_AFTER") ? (ULONGLONG)atoi(getenv("RX_NVN_WATCH_AFTER")) * 1000 : 0);
            static int all_threads = -1;   /* RX_NVN_WATCH_ALL=1: the Resource Memory thread's writes too */
            if (all_threads < 0) all_threads = env_on("RX_NVN_WATCH_ALL");
            if ((all_threads || strcmp(tn, "Resource Memory")) && GetTickCount64() >= after && InterlockedIncrement(&wts_reports) <= 12) {   /* (not the allocation's zero fill) */
                hos_log("nvn: write to a WriteTexels source at guest %#llx by %s (host pc exe+%#llx)", (unsigned long long)(a - (uintptr_t)GP(0)), tn,
                        (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleA(NULL)));
                hos_backtrace();
            }
            DWORD old;
            VirtualProtect((void *)(a & ~(uintptr_t)4095), 4096, PAGE_READWRITE, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI wts_rearm(void *p) {   /* (every 2 ms: the pages a write made writable are watched again) */
    (void)p;
    for (int k = 0; k < 300000 && wts_reports < 12; k++) {
        Sleep(2);
        for (int i = 0; i < wts_n; i++) {
            DWORD old;
            VirtualProtect((void *)wts_lo[i], wts_hi[i] - wts_lo[i], PAGE_READONLY, &old);
        }
    }
    return 0;
}
/* (also for other debugging hooks: nn_image_debug.c) the pages of [data, data + size) watched as above */
void hos_watch_guest_writes(const uint8_t *data, uint64_t size) {
    const uintptr_t lo = (uintptr_t)data & ~(uintptr_t)4095, hi = ((uintptr_t)data + size + 4095) & ~(uintptr_t)4095;
    for (int i = 0; i < wts_n; i++) if (wts_lo[i] == lo) return;
    if (wts_n >= 16) return;
    static int handler;
    if (!handler++) {
        AddVectoredExceptionHandler(1, wts_handler);
        CreateThread(NULL, 0, wts_rearm, NULL, 0, NULL);
    }
    wts_lo[wts_n] = lo, wts_hi[wts_n] = hi, wts_n++;
    DWORD old;
    VirtualProtect((void *)lo, hi - lo, PAGE_READONLY, &old);
    hos_log("nvn: watching writes to guest %#llx..%#llx", (unsigned long long)(lo - (uintptr_t)GP(0)),
            (unsigned long long)(hi - (uintptr_t)GP(0)));
}
static void watch_texels_source(const nvn_tex *t, const uint8_t *data, uint64_t size) {
    static int on = -1;
    static unsigned want_w, want_h;   /* RX_NVN_WATCH_TEXELS_SRC=<width>x<height>: only uploads into textures that size */
    if (on < 0) on = getenv("RX_NVN_WATCH_TEXELS_SRC") && sscanf(getenv("RX_NVN_WATCH_TEXELS_SRC"), "%ux%u", &want_w, &want_h) == 2;
    if (!on || t->d.width != want_w || t->d.height != want_h || size < 4096 || wts_n >= 16) return;
    const uintptr_t lo = ((uintptr_t)data + 4095) & ~(uintptr_t)4095, hi = ((uintptr_t)data + size) & ~(uintptr_t)4095;
    if (hi <= lo) return;
    for (int i = 0; i < wts_n; i++) if (wts_lo[i] == lo) return;
    static int handler;
    if (!handler++) {
        AddVectoredExceptionHandler(1, wts_handler);
        CreateThread(NULL, 0, wts_rearm, NULL, 0, NULL);
    }
    wts_lo[wts_n] = lo, wts_hi[wts_n] = hi, wts_n++;
    DWORD old;
    VirtualProtect((void *)lo, hi - lo, PAGE_READONLY, &old);
    hos_log("nvn: watching WriteTexels source guest %#llx..%#llx", (unsigned long long)(lo - (uintptr_t)GP(0)),
            (unsigned long long)(hi - (uintptr_t)GP(0)));
}
/* (hookshot) the bytes of the game's buffer that ReadTexels / WriteTexels touch at row and image strides (as
   texture_read_texels walks it: depth images of height rows of wb bytes); 0 when that overflows */
static uint64_t texels_extent(uint64_t wb, uint64_t hb, uint64_t depth, uint64_t row, uint64_t img) {
    uint64_t a = 0, b = 0, n = 0;
    if (!wb || !hb) return 1;
    if (__builtin_mul_overflow(depth - 1, img, &a) || __builtin_mul_overflow(hb - 1, row, &b) ||
        __builtin_add_overflow(a, b, &n) || __builtin_add_overflow(n, wb, &n))
        return 0;
    return n;
}
static void texels(Ctx *c, int write, int strided) {
    const nvn_tex *t = CTX(PN(0, sizeof(nvn_tex)));
    const nvn_view *v = CVW(PN(1, sizeof(nvn_view)));
    const NVNcopyRegion *r = (const NVNcopyRegion *)PN(2, sizeof(NVNcopyRegion));
    int bw, bh;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    const uint64_t wb = (uint64_t)((r->width > 0 ? r->width : 0) + bw - 1) / bw * (uint64_t)(bpb > 0 ? bpb : 0);
    const uint64_t hb = (uint64_t)((r->height > 0 ? r->height : 0) + bh - 1) / bh;
    uint64_t row = strided ? U64(4) : 0, img = strided ? U64(5) : 0;
    if (!row) row = wb;
    if (!img) img = row * hb;
    /* (hookshot) the strides are the game's 64-bit values: the whole span they reach must lie in guest memory, not only
       the buffer's first byte (the renderer reads or writes texels at data + k * img + y * row) */
    const uint64_t extent = texels_extent(wb, hb, r->depth > 1 ? (uint64_t)r->depth : 1, row, img);
    if (!extent) die("%sTexels of %#llx rows of %#llx, %#llx apart", write ? "Write" : "Read", (unsigned long long)hb,
                     (unsigned long long)wb, (unsigned long long)row);
    uint8_t *data = (uint8_t *)PN(3, extent);
    const unsigned level = v && v->set_levels ? v->base_level : 0, layer0 = v && v->set_layers ? v->min_layer : 0;
    if (v && v->set_format && v->format != t->d.format) die("%sTexels through a format view", write ? "Write" : "Read");
    static int texels_log = -1;
    if (texels_log < 0) texels_log = env_on("RX_NVN_TEXELS_LOG");
    if (texels_log) {   /* (debugging: who, from where, and how much of the data is not zero) */
        size_t nz = 0;
        for (uint64_t i = 0; data && i < extent && i < (64u << 20); i++) nz += data[i] != 0;
        uint64_t chain[6] = {c->x[30]}, fp = c->x[29];
        for (int k = 1; k < 6 && fp && fp < 0x8000000000ull && !(fp & 7); k++) chain[k] = G64(fp + 8), fp = G64(fp);
        hos_log("nvn: %sTexels data %#llx (%zu nonzero) from %llx < %llx < %llx < %llx < %llx < %llx",
                write ? "Write" : "Read", (unsigned long long)(P(3) ? (uint8_t *)P(3) - (uint8_t *)GP(0) : 0), nz,
                (unsigned long long)chain[0], (unsigned long long)chain[1], (unsigned long long)chain[2],
                (unsigned long long)chain[3], (unsigned long long)chain[4], (unsigned long long)chain[5]);
    }
    if (write) rx_nvn_write_texels(t, level, layer0, r, data, row, img), watch_texels_source(t, data, img);
    else rx_nvn_read_texels(t, level, layer0, r, data, row, img);
}
NV(nvnTextureReadTexels) { texels(c, 0, 0); }
NV(nvnTextureReadTexelsStrided) { texels(c, 0, 1); }
NV(nvnTextureWriteTexels) { texels(c, 1, 0); }
NV(nvnTextureWriteTexelsStrided) { texels(c, 1, 1); }
NV(nvnTextureFlushTexels) {}
NV(nvnTextureInvalidateTexels) {}

/* ---- samplers ---- */
#define SB(x) (&((nvn_samp_builder *)(x))->p)
#define CSB(x) (&((const nvn_samp_builder *)(x))->p)
#define CSP(x) (&((const nvn_samp *)(x))->p)
NV(nvnSamplerBuilderSetDefaults) {
    nvn_samp_builder *b = (nvn_samp_builder *)P(0);
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->p.minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR;
    b->p.magf = NVN_MAG_FILTER_LINEAR;
    b->p.ws = b->p.wt = b->p.wr = NVN_WRAP_MODE_REPEAT;
    b->p.cmp_func = NVN_COMPARE_FUNC_LESS;
    b->p.lod_min = -1000.f, b->p.lod_max = 1000.f, b->p.aniso = 1.f;
}
NV(nvnSamplerBuilderSetDevice) { ((nvn_samp_builder *)P(0))->dev = P(1); }
NV(nvnSamplerBuilderSetMinMagFilter) { SB(P(0))->minf = (uint8_t)U32(1); SB(P(0))->magf = (uint8_t)U32(2); }
NV(nvnSamplerBuilderSetWrapMode) { nvn_samp_par *p = SB(P(0)); p->ws = (uint8_t)U32(1), p->wt = (uint8_t)U32(2), p->wr = (uint8_t)U32(3); }
NV(nvnSamplerBuilderSetLodClamp) { SB(P(0))->lod_min = F(0); SB(P(0))->lod_max = F(1); }
NV(nvnSamplerBuilderSetLodBias) { SB(P(0))->lod_bias = F(0); }
NV(nvnSamplerBuilderSetCompare) { SB(P(0))->cmp_mode = (uint8_t)U32(1); SB(P(0))->cmp_func = (uint8_t)U32(2); }
NV(nvnSamplerBuilderSetBorderColor) { memcpy(SB(P(0))->border, PN(1, 16), 16); }
NV(nvnSamplerBuilderSetBorderColori) { const int32_t *v = (const int32_t *)P(1); for (int i = 0; i < 4; i++) SB(P(0))->border[i] = (float)v[i]; }
NV(nvnSamplerBuilderSetBorderColorui) { const uint32_t *v = (const uint32_t *)P(1); for (int i = 0; i < 4; i++) SB(P(0))->border[i] = (float)v[i]; }
NV(nvnSamplerBuilderSetMaxAnisotropy) { SB(P(0))->aniso = F(0); }
NV(nvnSamplerBuilderSetLodSnap) { SB(P(0))->lod_snap = F(0); }
NV(nvnSamplerBuilderSetReductionFilter) {
    if (U32(1) > 2) die("sampler reduction %u", U32(1));
    SB(P(0))->reduction = (uint8_t)U32(1);
}
NV(nvnSamplerBuilderGetMinMagFilter) { *(int *)P(1) = CSB(P(0))->minf; *(int *)P(2) = CSB(P(0))->magf; }
NV(nvnSamplerBuilderGetWrapMode) { const nvn_samp_par *p = CSB(P(0)); *(int *)P(1) = p->ws; *(int *)P(2) = p->wt; *(int *)P(3) = p->wr; }
NV(nvnSamplerBuilderGetReductionFilter) { RET(CSB(P(0))->reduction); }
NV(nvnSamplerInitialize) {
    nvn_samp *o = (nvn_samp *)P(0);
    o->magic = NVN_MAGIC('S', 'M', 'P', 0);
    o->p = *CSB(P(1));
    RET(1);
}
NV(nvnSamplerFinalize) { ((nvn_samp *)P(0))->magic = 0; }
NV(nvnSamplerSetDebugLabel) {}
NV(nvnSamplerGetMinMagFilter) { *(int *)P(1) = CSP(P(0))->minf; *(int *)P(2) = CSP(P(0))->magf; }
NV(nvnSamplerGetWrapMode) { const nvn_samp_par *p = CSP(P(0)); *(int *)P(1) = p->ws; *(int *)P(2) = p->wt; *(int *)P(3) = p->wr; }
NV(nvnSamplerGetLodClamp) { *(float *)P(1) = CSP(P(0))->lod_min; *(float *)P(2) = CSP(P(0))->lod_max; }
NV(nvnSamplerGetLodBias) { retf(c, CSP(P(0))->lod_bias); }
NV(nvnSamplerGetCompare) { *(int *)P(1) = CSP(P(0))->cmp_mode; *(int *)P(2) = CSP(P(0))->cmp_func; }
NV(nvnSamplerGetBorderColor) { memcpy(PN(1, 16), CSP(P(0))->border, 16); }
NV(nvnSamplerGetMaxAnisotropy) { retf(c, CSP(P(0))->aniso); }
NV(nvnSamplerGetReductionFilter) { RET(CSP(P(0))->reduction); }
NV(nvnSamplerGetDebugID) { RET(U64(0)); }

/* ---- programs: the stages' code addresses and control sections (translated by the replay) ---- */
NV(nvnProgramInitialize) {
    memset(P(0), 0, sizeof(nvn_prog));
    ((nvn_prog *)P(0))->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    RET(1);
}
NV(nvnProgramFinalize) {
    rx_nvn_program_released((const nvn_prog *)P(0));
    ((nvn_prog *)P(0))->magic = 0;
}
/* HOS_SHADER_DUMP=<dir> (mkw-recomp, debugging): each stage's code (from its GPU address, up to 64 KiB while mapped)
   and control section (0x800 bytes) as <code address>.code / .ctl, for gpu/shadertool */
static void dump_stage(uint64_t code, uint64_t ctl) {
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) dir = getenv("HOS_SHADER_DUMP");
    if (!dir || !code || !ctl) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/%010llx.code", dir, (unsigned long long)code);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    CreateDirectoryA(dir, NULL);
    uint64_t n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (n < 0x10000 && VirtualQuery(GP(code + n), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) n += 0x1000;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(GP(code), 1, (size_t)n, f); fclose(f); }
    snprintf(path, sizeof path, "%s/%010llx.ctl", dir, (unsigned long long)code);
    f = fopen(path, "wb");
    if (f) { fwrite(GP(ctl), 1, 0x800, f); fclose(f); }
}
static int set_shaders(nvn_prog *o, int n, const uint8_t *sd, int stride) {
    if (n < 0 || n > 6) return 0;
    rx_nvn_program_released(o);
    o->nstage = (uint32_t)n;
    for (int i = 0; i < n; i++) {   /* {data (GPU address), control (guest pointer)[, reserved]} */
        memcpy(&o->sd[i].data, sd + stride * i, 8);
        memcpy(&o->sd[i].control, sd + stride * i + 8, 8);
        dump_stage(o->sd[i].data, o->sd[i].control);
        if (stride == 0x18) {
            uint64_t reserved;
            memcpy(&reserved, sd + stride * i + 16, 8);
            if (reserved) hos_log("nvn: ProgramSetShadersExt: stage %d field %#llx", i, (unsigned long long)reserved);
        }
    }
    o->serial = new_serial();
    rx_nvn_program_created(o);   /* (the stages are in memory now: the back end loads what it kept ready for them) */
    return 1;
}
NV(nvnProgramSetShaders) { RET(set_shaders((nvn_prog *)P(0), I(1), (const uint8_t *)P(2), 16)); }
/* NVNshaderDataExt: {data, control, reserved}, 0x18 bytes (seen in TOTK's calls; reserved 0) */
NV(nvnProgramSetShadersExt) { RET(set_shaders((nvn_prog *)P(0), I(1), (const uint8_t *)P(2), 0x18)); }
NV(nvnProgramSetDebugLabel) {}

/* ---- sync, events ---- */
NV(nvnSyncInitialize) { nvn_sync *s = (nvn_sync *)P(0); s->magic = NVN_MAGIC('S', 'Y', 'N', 'C'); s->fenced = 0; s->fence = 0; RET(1); }
NV(nvnSyncFinalize) { ((nvn_sync *)P(0))->magic = 0; }
NV(nvnSyncSetDebugLabel) {}
/* a fence: everything submitted to the queue so far is done */
/* HOS_NVN_SYNC_LOG=1 or the live "!synclog" (debugging): fences, sync waits and event reads, with the caller */
volatile int hos_nvn_sync_log_on;   /* (also the live input file's "!synclog": nn_hid.c) */
static int sync_log(void) {   /* (the first 30000 calls; HOS_NVN_SYNC_LOG=<n>: from the n-th present on, 400 calls) */
    static int v = -1;
    static long long from;
    static volatile LONG n;
    extern volatile LONG64 hos_present_count;   /* display.c */
    if (v < 0) v = getenv("HOS_NVN_SYNC_LOG") != NULL, from = v ? atoll(getenv("HOS_NVN_SYNC_LOG")) : 0;
    if (v && from > 1) return hos_present_count >= from && InterlockedIncrement(&n) <= 400;
    return (v || hos_nvn_sync_log_on) && InterlockedIncrement(&n) <= 30000;
}
/* Submitted command buffers are held until the queue is flushed (nvnQueueFlush, a present, a finish or a wait on a
   fence): NVN starts a submission's work only then, and the game writes what it reads in between. Handed to the replay
   at the submit, a frame's draws read some of their constants before the game had written them: MKW's terrain tiles
   flashed as walls for a frame, we thought; measured, holding made them far more frequent (61 vs 0-9 in a 60 s drive):
   MKW writes some of a frame's data in place right after its submit, as the console's GPU has read it by then. Off by
   default; HOS_NVN_HOLD=1: held. */
typedef struct { int n; uint64_t h[64]; } held_submit;
static held_submit held[256];
static volatile LONG nheld;
static CRITICAL_SECTION held_cs;
static int held_ready, hold_on = -1;
static void held_init(void) {
    if (!held_ready) { InitializeCriticalSection(&held_cs); held_ready = 1; }
    if (hold_on < 0) { const char *e = getenv("HOS_NVN_HOLD"); hold_on = e && atoi(e) != 0; }   /* (off: see below) */
}
static void held_flush(void) {
    held_init();
    if (!nheld) return;
    EnterCriticalSection(&held_cs);
    for (LONG i = 0; i < nheld; i++) rx_nvn_submit(held[i].h, held[i].n);
    nheld = 0;
    LeaveCriticalSection(&held_cs);
}
static uint64_t held_count(void) { return (uint64_t)nheld; }

NV(nvnQueueFenceSync) {
    held_init();
    nvn_sync *s = (nvn_sync *)P(1); s->fenced = 1;
    /* (hookshot) the queue's own work: with nothing held, its last submission (a wait for it no longer waits for another
       queue's work stopped at an event wait) */
    s->fence = held_count() ? rx_nvn_fence() + held_count() : rx_nvn_fence_q((uint64_t)(uintptr_t)P(0));
    if (sync_log()) hos_log("nvn: FenceSync %p -> %llu (lr %#llx)", (void *)s, (unsigned long long)s->fence, (unsigned long long)c->x[30]);
}
/* (the GPU waits the game makes: count and time by kind, logged every 5 s) */
static volatile LONG64 wait_ns[3], wait_n[3];   /* sync waits that blocked, sync polls that timed out, queue finishes */
static void wait_note(int kind, int64_t t0) {
    InterlockedAdd64(&wait_ns[kind], hos_now_ns() - t0);
    InterlockedIncrement64(&wait_n[kind]);
    static volatile LONG64 last;
    const int64_t now = hos_now_ns(), l = last;
    if (now - l >= 5000000000LL && InterlockedCompareExchange64(&last, now, l) == l && l) {
        hos_log("nvn: GPU waits in 5 s: %lld sync waits (%.0f ms), %lld polls timed out (%.0f ms), %lld queue finishes (%.0f ms)",
                (long long)InterlockedExchange64(&wait_n[0], 0), InterlockedExchange64(&wait_ns[0], 0) / 1e6,
                (long long)InterlockedExchange64(&wait_n[1], 0), InterlockedExchange64(&wait_ns[1], 0) / 1e6,
                (long long)InterlockedExchange64(&wait_n[2], 0), InterlockedExchange64(&wait_ns[2], 0) / 1e6);
    } else if (!l) {
        InterlockedCompareExchange64(&last, now, 0);
    }
}
/* never fenced: times out, as on the console; fenced: released once the GPU reached the fence */
NV(nvnSyncWait) {
    const nvn_sync *s = (const nvn_sync *)P(0);
    if (sync_log()) hos_log("nvn: SyncWait %p fence %llu timeout %llu (lr %#llx); latest %llu, done %llu", (void *)s,
                            (unsigned long long)s->fence, (unsigned long long)U64(1), (unsigned long long)c->x[30],
                            (unsigned long long)rx_nvn_fence(), (unsigned long long)rx_nvn_replayed());
    if (!s->fenced) { RET(NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED); return; }
    if (s->fenced == 1 && s->fence > rx_nvn_fence()) held_flush();   /* (a wait flushes what it waits for) */
    if (rx_nvn_wait(s->fence, 0)) { RET(NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED); return; }
    static int block = -1;   /* HOS_NVN_SYNC_BLOCK=1 (experiment): a poll waits for the GPU */
    if (block < 0) block = env_on("HOS_NVN_SYNC_BLOCK");
    const int away = hos_vcore_away();
    const int64_t t0 = hos_now_ns();
    const int ok = rx_nvn_wait(s->fence, block ? ~0ull >> 1 : U64(1));
    wait_note(ok ? 0 : 1, t0);
    if (away) hos_vcore_back();
    RET(ok ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
}
NV(nvnEventBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_event_builder)); }
NV(nvnEventBuilderSetStorage) { nvn_event_builder *b = (nvn_event_builder *)P(0); b->pool = (const nvn_pool *)P(1); b->off = (int64_t)U64(2); }
NV(nvnEventInitialize) {
    const nvn_event_builder *b = (const nvn_event_builder *)P(1);
    if (!b->pool) { RET(0); return; }
    nvn_event *e = (nvn_event *)P(0);
    e->magic = NVN_MAGIC('E', 'V', 'N', 'T');
    e->gpu = POOL(b->pool)->gpu + (uint64_t)b->off;
    RET(1);
}
NV(nvnEventFinalize) { ((nvn_event *)P(0))->magic = 0; }
/* (hookshot) an NVN event's value from the host (hos_thread.c's present poll): 0 when it is no event */
int hos_nvn_event_value(uint64_t guest_event, uint32_t *v) {
    const nvn_event *e = (const nvn_event *)hos_guest_range(guest_event, sizeof(nvn_event));
    if (!guest_event || !e || e->magic != NVN_MAGIC('E', 'V', 'N', 'T')) return 0;
    *v = 0;
    nvn_gpu_read(e->gpu, v, 4);
    return 1;
}
NV(nvnEventGetValue) {
    {   /* (hookshot, experiment) HOS_PRESENT_NOWAIT=1: OoT's present poll (hos_thread.c present_poll) sees its frame's
           event signalled at once: the replay reads memory as submitted, so the game may go on to the next frame
           while this one is replayed, as far as the window's textures and the replay's queue let it */
        static int nowait = -1;
        if (nowait < 0) nowait = getenv("HOS_PRESENT_NOWAIT") && atoi(getenv("HOS_PRESENT_NOWAIT")) != 0;
        if (nowait && (c->x[30] == 0x805904a0 || c->x[30] == 0x80590530)) { RET(1); return; }
    }
    uint32_t v = 0; nvn_gpu_read(((const nvn_event *)P(0))->gpu, &v, 4); RET(v);
    if (sync_log()) hos_log("nvn: EventGetValue %p = %u (lr %#llx)", P(0), v, (unsigned long long)c->x[30]);
}
NV(nvnEventSignal) {
    const nvn_event *e = (const nvn_event *)P(0);
    {   /* (the CPU's signals, 40 then every 5000: what the GPU's waits above wait for) */
        static volatile LONG n;
        const LONG k = InterlockedIncrement(&n);
        if (k <= 40 || k % 5000 == 0)
            hos_log("nvn: EventSignal (CPU) #%ld: event %#llx mode %u value %u (lr %#llx)", (long)k,
                    (unsigned long long)(e ? e->gpu : 0), U32(1), U32(2), (unsigned long long)c->x[30]);
    }
    uint32_t v = 0;
    if (U32(1) == NVN_EVENT_SIGNAL_MODE_ADD) nvn_gpu_read(e->gpu, &v, 4);
    v = U32(1) == NVN_EVENT_SIGNAL_MODE_ADD ? v + U32(2) : U32(2);
    nvn_gpu_write(e->gpu, &v, 4);
    rx_nvn_event_signal(e->gpu, v);
}

/* ---- queue ---- */
#define QB(p) ((nvn_queue_builder *)(p))
NV(nvnQueueBuilderSetDefaults) {
    nvn_queue_builder *b = QB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->cmd_sz = 65536, b->comp_sz = 262144, b->ctl_sz = 16384, b->flush = 4096;   /* the device's defaults */
}
NV(nvnQueueBuilderSetDevice) { QB(P(0))->dev = P(1); }
NV(nvnQueueBuilderSetFlags) { QB(P(0))->flags = U32(1); }
NV(nvnQueueBuilderSetCommandMemorySize) { QB(P(0))->cmd_sz = U64(1); }
NV(nvnQueueBuilderSetComputeMemorySize) { QB(P(0))->comp_sz = U64(1); }
NV(nvnQueueBuilderSetControlMemorySize) { QB(P(0))->ctl_sz = U64(1); }
NV(nvnQueueBuilderSetCommandFlushThreshold) { QB(P(0))->flush = U64(1); }
/* the game allocates this much (we do not use it): the three blocks, rounded to 4 KiB */
NV(nvnQueueBuilderGetQueueMemorySize) { const nvn_queue_builder *b = QB(P(0)); RET((b->cmd_sz + b->comp_sz + b->ctl_sz + 4095) & ~4095ull); }
NV(nvnQueueBuilderSetQueueMemory) { QB(P(0))->mem = U64(1); QB(P(0))->mem_sz = U64(2); }
NV(nvnQueueInitialize) { nvn_queue *q = (nvn_queue *)P(0); q->magic = NVN_MAGIC('Q', 'U', 'E', 'U'); q->submits = 0; RET(1); }
NV(nvnQueueFinalize) { ((nvn_queue *)P(0))->magic = 0; }
NV(nvnQueueGetError) { if (P(1)) memset(P(1), 0, sizeof(NVNqueueErrorInfo)); RET(NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR); }
NV(nvnQueueFlush) { held_flush(); }
NV(nvnQueueFinish) {
    held_flush();
    const int away = hos_vcore_away();
    const int64_t t0 = hos_now_ns();
    rx_nvn_finish();
    wait_note(2, t0);
    if (away) hos_vcore_back();
}
NV(nvnQueueSetDebugLabel) {}

/* ---- command buffers ---- */
#define CB(p) ((nvn_cmd *)(p))
#define OP_RESERVE 24

/* Recording copies and command buffer segments come from power-of-two free lists and go back to them. Through the
   process heap (~13k copies a frame, segments freed at Finalize and made again), the frees decommitted pages that the
   next allocations faulted in again: NtFreeVirtualMemory ~0.6% of the recording thread (ModuleSystemWorker1), more in
   page faults. A class keeps at most POOL_KEEP bytes; the rest goes back to the heap. NVN_NO_POOL=1: the heap. */
#define POOL_MIN 6       /* 64 bytes */
#define POOL_MAX 26      /* 64 MiB: bigger blocks use the heap */
#define POOL_KEEP (64ull << 20)
static SRWLOCK pool_lock = SRWLOCK_INIT;
static void *pool_head[POOL_MAX + 1];
static uint64_t pool_bytes[POOL_MAX + 1];
static int pool_off(void) {
    static int off = -1;
    if (off < 0) off = env_on("NVN_NO_POOL");
    return off;
}
static unsigned pool_class(size_t n) {
    unsigned c = POOL_MIN;
    while (c <= POOL_MAX && ((size_t)1 << c) < n) c++;
    return c;
}
/* a block of at least n bytes; *cls its class (POOL_MAX + 1: from the heap, n bytes) */
static void *pool_get(size_t n, unsigned *cls) {
    const unsigned c = pool_off() ? POOL_MAX + 1 : pool_class(n);
    *cls = c;
    if (c > POOL_MAX) return malloc(n);
    AcquireSRWLockExclusive(&pool_lock);
    void *b = pool_head[c];
    if (b) {
        pool_head[c] = *(void **)b;
        pool_bytes[c] -= (uint64_t)1 << c;
    }
    ReleaseSRWLockExclusive(&pool_lock);
    return b ? b : malloc((size_t)1 << c);
}
static void pool_put(void *b, unsigned cls) {
    if (!b) return;
    if (cls > POOL_MAX) {
        free(b);
        return;
    }
    AcquireSRWLockExclusive(&pool_lock);
    const int keep = pool_bytes[cls] + ((uint64_t)1 << cls) <= POOL_KEEP;
    if (keep) {
        *(void **)b = pool_head[cls];
        pool_head[cls] = b;
        pool_bytes[cls] += (uint64_t)1 << cls;
    }
    ReleaseSRWLockExclusive(&pool_lock);
    if (!keep) free(b);
}

/* (hookshot) A command buffer's recording buffer lives in host memory, and its pointers in this table, not in the
   object (guest memory). The object holds its slot's number; a slot names the object it belongs to, so a slot number
   that is not the object's own is not used: the object gets a new slot, as an object never initialized does. Slots are
   in chunks that are never freed, so a lookup needs no lock (a command buffer records on one thread at a time). */
typedef struct { uint64_t owner; uint8_t *seg, *cur, *end; } cmd_rec;
#define CMD_CHUNK_BITS 10
#define CMD_CHUNKS 4096u                     /* (4M command buffers at once) */
static cmd_rec *volatile cmd_chunks[CMD_CHUNKS];
static SRWLOCK cmd_lock = SRWLOCK_INIT;
static uint32_t cmd_next = 1, *cmd_free;     /* (slot 0: none) */
static size_t ncmd_free, cap_cmd_free;
static cmd_rec *cmd_slot(uint32_t s) {
    if (!s || (s >> CMD_CHUNK_BITS) >= CMD_CHUNKS) return NULL;
    cmd_rec *k = cmd_chunks[s >> CMD_CHUNK_BITS];
    return k ? &k[s & ((1u << CMD_CHUNK_BITS) - 1)] : NULL;
}
static cmd_rec *cmd_adopt(nvn_cmd *o) {
    const uint64_t me = h2g(o);
    AcquireSRWLockExclusive(&cmd_lock);
    const uint32_t s = ncmd_free ? cmd_free[--ncmd_free] : cmd_next++;
    if ((s >> CMD_CHUNK_BITS) >= CMD_CHUNKS) die("too many command buffers");
    if (!cmd_chunks[s >> CMD_CHUNK_BITS]) {
        cmd_rec *k = (cmd_rec *)calloc((size_t)1 << CMD_CHUNK_BITS, sizeof *k);
        if (!k) die("command buffer table");
        cmd_chunks[s >> CMD_CHUNK_BITS] = k;
    }
    cmd_rec *r = cmd_slot(s);
    memset(r, 0, sizeof *r);
    r->owner = me;
    ReleaseSRWLockExclusive(&cmd_lock);
    o->slot = s;
    return r;
}
static cmd_rec *cmd_host(nvn_cmd *o) {
    cmd_rec *r = cmd_slot(o->slot);
    if (r && r->owner == h2g(o)) return r;
    static volatile LONG told;
    if (o->slot && InterlockedIncrement(&told) <= 4)
        hos_log("nvn: command buffer %#llx names slot %u, which is not its own: a new one", (unsigned long long)h2g(o), o->slot);
    return cmd_adopt(o);
}
static volatile LONG64 seg_bytes, seg_count;   /* (the command buffers' segments, for the recordings' log line) */
static void cmd_release(nvn_cmd *o) {   /* the object's slot and recording buffer back */
    cmd_rec *r = cmd_slot(o->slot);
    if (!r || r->owner != h2g(o)) {
        o->slot = 0;
        return;
    }
    if (r->seg) {
        InterlockedAdd64(&seg_bytes, -(LONG64)(r->end - r->seg));
        InterlockedDecrement64(&seg_count);
        pool_put(r->seg, pool_off() ? POOL_MAX + 1 : pool_class((size_t)(r->end - r->seg)));
    }
    AcquireSRWLockExclusive(&cmd_lock);
    memset(r, 0, sizeof *r);
    if (ncmd_free == cap_cmd_free) {
        cap_cmd_free = cap_cmd_free ? cap_cmd_free * 2 : 256;
        cmd_free = (uint32_t *)realloc(cmd_free, cap_cmd_free * sizeof *cmd_free);
        if (!cmd_free) die("command buffer table");
    }
    cmd_free[ncmd_free++] = o->slot;
    ReleaseSRWLockExclusive(&cmd_lock);
    o->slot = 0;
}
NV(nvnCommandBufferInitialize) {
    nvn_cmd *o = CB(P(0));
    cmd_release(o);   /* (initialized again without Finalize: its recording buffer back) */
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('C', 'M', 'D', 'B');
    o->inline_cur = ~0ull;
    o->inline_end = 0;
    cmd_adopt(o);
    RET(1);
}
NV(nvnCommandBufferFinalize) {
    nvn_cmd *o = CB(P(0));
    cmd_release(o);
    o->magic = 0;
}
NV(nvnCommandBufferSetMemoryCallback) { CB(P(0))->cb = U64(1); }
NV(nvnCommandBufferSetMemoryCallbackData) { CB(P(0))->cb_data = U64(1); }
NV(nvnCommandBufferGetMemoryCallback) { RET(CB(P(0))->cb); }
NV(nvnCommandBufferGetMemoryCallbackData) { RET(CB(P(0))->cb_data); }
NV(nvnCommandBufferSetCommandMemoryCallbackEnabled) {}
NV(nvnCommandBufferSetControlMemoryCallbackEnabled) {}
NV(nvnCommandBufferSetDebugLabel) {}

static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { nvn_op_hdr h = {op, size}; memcpy(p, &h, sizeof h); }
static void cmd_reserve_rec(cmd_rec *o, size_t need) {
    if (o->seg && need + OP_RESERVE <= (size_t)(o->end - o->cur)) return;
    const size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0;
    if (!cap) cap = 64 << 10;
    while (used + need + OP_RESERVE > cap) cap *= 2;
    unsigned cls;
    uint8_t *b = (uint8_t *)pool_get(cap, &cls);
    if (!b) die("recording buffer (%zu bytes)", cap);
    if (o->seg) {
        memcpy(b, o->seg, used);
        InterlockedAdd64(&seg_bytes, -(LONG64)(o->end - o->seg));
        pool_put(o->seg, pool_off() ? POOL_MAX + 1 : pool_class((size_t)(o->end - o->seg)));
    } else {
        InterlockedIncrement64(&seg_count);
    }
    InterlockedAdd64(&seg_bytes, (LONG64)cap);
    o->seg = b, o->cur = b + used, o->end = b + cap;
}
static uint8_t *cmd_alloc(nvn_cmd *o, uint32_t size) {
    size = (size + 7) & ~7u;
    cmd_rec *r = cmd_host(o);
    cmd_reserve_rec(r, size);
    uint8_t *p = r->cur;
    r->cur += size;
    o->cmd_used += size;
    o->hw_used += size;
    o->last_rec = size;
    put_hdr(p, 0, size);
    return p;
}
/* the game sums GetCommandMemoryUsed over recordings to size its resource heaps: report the real encoding's size where
   it is known (Maxwell methods of a real recording, from the reference) instead of our record's */
static void hw_fix(nvn_cmd *o, uint64_t bytes) { o->hw_used += bytes; o->hw_used -= o->last_rec; o->last_rec = bytes; }

NV(nvnCommandBufferAddCommandMemory) {
    nvn_cmd *o = CB(P(0));
    o->game_size = U64(3);
    o->cmd_used = o->hw_used = 0;
}

/* Recordings: a handle is a host copy of the finished recording. Games reuse command memory for later recordings while
   still calling earlier handles, so the copy lives until the control memory it was made in is added again (the console
   driver's handle is a descriptor in that control memory). (The reference also drops copies whose control memory was
   reused for other data or unmapped, by a mark written there; not done here yet: long sessions can keep more copies.) */
/* live copies by the 64 KiB block of their control memory position: AddControlMemory releases a range's copies without
   looking at the others (a scan of every copy at each call was ~4% of TOTK's frame threads: ~22000 live copies). By
   4 KiB page a 2 MiB range walked ~500 buckets under the exclusive lock the recording threads also take
   (totk-pc-recomp dc69797: 1.4.3 field walk 72-77 -> 78-80 fps with 64 KiB blocks). HOS_NVN_REC_SHIFT=12: by page. */
typedef struct rec_node { uint64_t ctl; uint8_t *copy; struct rec_node *next; int marked; } rec_node;
#define REC_BUCKETS 65536u
static rec_node **rec_buckets;
static size_t ncopies;
static size_t npins;   /* (tex_copy: the texture copies, logged with the recordings) */
static unsigned rec_shift = 16;   /* (set when the buckets are made) */
static unsigned rec_bucket(uint64_t ctl) { return (unsigned)(((ctl >> rec_shift) * 0x9E3779B97F4A7C15ull) >> 48) & (REC_BUCKETS - 1); }
static SRWLOCK copy_lock = SRWLOCK_INIT;
static volatile LONG64 copy_bytes;
#define COPY_MAGIC 0x59504f43444d43ull   /* "CMDCOPY" */
typedef struct { uint64_t magic, bytes, cls; } copy_prefix;   /* (cls: its pool class) */
/* released copies wait until the replay passed every submit made before their release (the replay runs behind) */
typedef struct { copy_prefix *p; uint64_t after; } dead_copy;
static dead_copy *dead;
static size_t ndead, cap_dead;
/* (hookshot) The live handles, a host set (open addressing, copy_lock held): a handle the game passes back is a host
   address, so it is used only when it is in the set, never on what the memory at that address holds. */
static uint64_t *live_handles;
static size_t live_cap, nlive;
static size_t live_at(uint64_t h, size_t cap) { return (size_t)((h * 0x9E3779B97F4A7C15ull) >> 24) & (cap - 1); }
static void live_handle_add(uint64_t h) {   /* (copy_lock held exclusively) */
    if ((nlive + 1) * 2 > live_cap) {
        const size_t cap = live_cap ? live_cap * 2 : 65536;
        uint64_t *t = (uint64_t *)calloc(cap, sizeof *t);
        if (!t) die("recording handle set");
        for (size_t i = 0; i < live_cap; i++) {
            if (!live_handles[i]) continue;
            size_t k = live_at(live_handles[i], cap);
            while (t[k]) k = (k + 1) & (cap - 1);
            t[k] = live_handles[i];
        }
        free(live_handles);
        live_handles = t, live_cap = cap;
    }
    size_t k = live_at(h, live_cap);
    while (live_handles[k] && live_handles[k] != h) k = (k + 1) & (live_cap - 1);
    if (!live_handles[k]) live_handles[k] = h, nlive++;
}
static int live_handle_has(uint64_t h) {   /* (copy_lock held) */
    if (!h || !live_cap) return 0;
    for (size_t k = live_at(h, live_cap); live_handles[k]; k = (k + 1) & (live_cap - 1))
        if (live_handles[k] == h) return 1;
    return 0;
}
static void live_handle_remove(uint64_t h) {   /* (copy_lock held exclusively; backward shift, no tombstones) */
    if (!h || !live_cap) return;
    size_t k = live_at(h, live_cap);
    while (live_handles[k] && live_handles[k] != h) k = (k + 1) & (live_cap - 1);
    if (!live_handles[k]) return;
    live_handles[k] = 0, nlive--;
    for (size_t j = (k + 1) & (live_cap - 1); live_handles[j]; j = (j + 1) & (live_cap - 1)) {
        const size_t want = live_at(live_handles[j], live_cap);
        /* (move it into the hole when its probe run starts at or before the hole) */
        if (((j - want) & (live_cap - 1)) >= ((j - k) & (live_cap - 1))) {
            live_handles[k] = live_handles[j], live_handles[j] = 0;
            k = j;
        }
    }
}
static void free_dead_copies(void) {   /* (copy_lock held) */
    const uint64_t done = rx_nvn_replayed();
    for (size_t i = 0; i < ndead;) {
        if (dead[i].after <= done) {
            InterlockedAdd64(&copy_bytes, -(LONG64)dead[i].p->bytes);
            pool_put(dead[i].p, (unsigned)dead[i].p->cls);
            dead[i] = dead[--ndead];
        } else {
            i++;
        }
    }
}
static void release_copies(uint64_t lo, uint64_t hi) {
    if (hi <= lo || !rec_buckets) return;
    const uint64_t after = rx_nvn_pushed() + held_count();   /* (held submits name them too) */
    AcquireSRWLockExclusive(&copy_lock);
    free_dead_copies();
    /* (a range over 4 GiB: every bucket once instead of every block) */
    const uint64_t pages = ((hi - 1) >> rec_shift) - (lo >> rec_shift) + 1;
    const uint64_t steps = pages > REC_BUCKETS ? REC_BUCKETS : pages;
    for (uint64_t k = 0; k < steps; k++) {
        const unsigned b = pages > REC_BUCKETS ? (unsigned)k : rec_bucket(((lo >> rec_shift) + k) << rec_shift);
        for (rec_node **pn = &rec_buckets[b]; *pn;) {
            rec_node *n = *pn;
            if (n->ctl < lo || n->ctl >= hi) {
                pn = &n->next;
                continue;
            }
            copy_prefix *p = (copy_prefix *)n->copy - 1;
            p->magic = 0;   /* (no submit names it any more) */
            live_handle_remove((uint64_t)(uintptr_t)n->copy);
            if (ndead == cap_dead) {
                cap_dead = cap_dead ? cap_dead * 2 : 1024;
                dead = (dead_copy *)realloc(dead, cap_dead * sizeof *dead);
            }
            dead[ndead].p = p;
            dead[ndead].after = after;
            ndead++;
            *pn = n->next;
            free(n);
            ncopies--;
        }
    }
    ReleaseSRWLockExclusive(&copy_lock);
}
/* Copies whose control memory the game reused for other data: the console driver's handle is a descriptor written in
   the 64 bytes of control memory a recording takes; a mark written there in its place (the copy's address, and a magic)
   that the game has overwritten means it reused that memory, and the handle is dead on the console too. Copies whose
   control memory was never added again (MKW frees and reallocates its control memory) were kept for good otherwise:
   ~40 per 1000 recordings, ~100,000 after 20 minutes. Swept every 25,000 recordings. HOS_NVN_NO_SWEEP=1: not. */
#define COPY_MARK 0x4b52414d59504f43ull   /* "COPYMARK" */
static void sweep_overwritten_copies(void) {   /* (copy_lock held) */
    static int off = -1;
    if (off < 0) off = env_on("HOS_NVN_NO_SWEEP");
    if (off || !rec_buckets) return;
    free_dead_copies();
    const uint64_t after = rx_nvn_pushed() + held_count();
    size_t swept = 0;
    for (unsigned b = 0; b < REC_BUCKETS; b++) {
        for (rec_node **pn = &rec_buckets[b]; *pn;) {
            rec_node *n = *pn;
            uint64_t mark[2];
            if (!n->marked) {
                pn = &n->next;
                continue;
            }
            void *at = hos_guest_range(n->ctl, sizeof mark);
            if (!at) {
                pn = &n->next;
                continue;
            }
            memcpy(mark, at, sizeof mark);
            if (mark[0] == COPY_MARK && mark[1] == (uint64_t)(uintptr_t)n->copy) {
                pn = &n->next;
                continue;
            }
            copy_prefix *pre = (copy_prefix *)n->copy - 1;
            pre->magic = 0;
            live_handle_remove((uint64_t)(uintptr_t)n->copy);
            if (ndead == cap_dead) {
                cap_dead = cap_dead ? cap_dead * 2 : 1024;
                dead = (dead_copy *)realloc(dead, cap_dead * sizeof *dead);
            }
            dead[ndead].p = pre;
            dead[ndead].after = after;
            ndead++;
            *pn = n->next;
            free(n);
            ncopies--;
            swept++;
        }
    }
    static int logged;
    if (swept && logged++ < 12) hos_log("nvn: %zu copies of recordings whose control memory was reused let go (%zu live)", swept, ncopies);
}
NV(nvnCommandBufferAddControlMemory) {
    nvn_cmd *o = CB(P(0));
    release_copies(U64(1), U64(1) + U64(2));
    o->ctl = U64(1);
    o->ctl_size = U64(2);
    o->ctl_used = 0;
}
NV(nvnCommandBufferGetControlMemorySize) { RET(CB(P(0))->ctl_size); }
NV(nvnCommandBufferGetControlMemoryUsed) { const nvn_cmd *o = CB(P(0)); RET(o->ctl_used < o->ctl_size ? o->ctl_used : o->ctl_size); }
NV(nvnCommandBufferGetControlMemoryFree) { const nvn_cmd *o = CB(P(0)); RET(o->ctl_size - (o->ctl_used < o->ctl_size ? o->ctl_used : o->ctl_size)); }
NV(nvnCommandBufferGetCommandMemorySize) { RET(CB(P(0))->game_size); }
NV(nvnCommandBufferGetCommandMemoryUsed) { const nvn_cmd *o = CB(P(0)); RET(o->hw_used < o->game_size ? o->hw_used : o->game_size); }
NV(nvnCommandBufferGetCommandMemoryFree) { const nvn_cmd *o = CB(P(0)); RET(o->game_size - (o->hw_used < o->game_size ? o->hw_used : o->game_size)); }

NV(nvnCommandBufferBeginRecording) {
    nvn_cmd *o = CB(P(0));
    if (o->recording) hos_log("nvn: BeginRecording while recording");
    o->recording = 1;
    cmd_rec *r = cmd_host(o);
    r->cur = r->seg;
    o->rec_start = o->cmd_used;
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, NOP_BEGIN, 8);
    o->hw_used -= 8;   /* BEGIN / END have no hardware equivalent */
    o->hw_start = o->hw_used;
}
/* the recording, BEGIN to END, is contiguous in the command buffer's segment: one copy of it */
static uint8_t *flatten(const uint8_t *p, size_t n) {
    unsigned cls;
    uint8_t *out = (uint8_t *)pool_get(sizeof(copy_prefix) + n, &cls);
    if (!out) die("recording copy (%zu bytes)", n);
    memcpy(out + sizeof(copy_prefix), p, n);
    copy_prefix *pre = (copy_prefix *)out;
    pre->cls = cls;
    pre->magic = COPY_MAGIC;
    pre->bytes = sizeof(copy_prefix) + n;
    InterlockedAdd64(&copy_bytes, (LONG64)pre->bytes);
    return out + sizeof(copy_prefix);
}
/* (the watches below) the guest's call chain, by frame pointers: "lr < caller < ..." */
static void guest_chain(Ctx *c, char *b, size_t cap) {
    uint64_t lr = c->x[30], fp = c->x[29];
    int n = snprintf(b, cap, "%llx", (unsigned long long)lr);
    for (int k = 1; k < 12 && fp && fp < 0x8000000000ull && !(fp & 7) && n < (int)cap; k++)
        n += snprintf(b + n, cap - n, " < %llx", (unsigned long long)G64(fp + 8)), fp = G64(fp);
}
/* (HOS_NVN_WATCH_DRAW: watch_draw below) */
static void *watch_draw_cmd;
static uint64_t watch_handles[16];
static int nwatch_handles;
static void watch_handle_called(Ctx *c, uint64_t h, const char *how) {
    static volatile LONG logged;
    for (int i = 0; i < nwatch_handles; i++) {
        if (watch_handles[i] != h || InterlockedIncrement(&logged) > 60) continue;
        char b[400];
        guest_chain(c, b, sizeof b);
        hos_log("nvn: watched recording %llx %s at %.3f s from %s", (unsigned long long)h, how, hos_now_ns() * 1e-9, b);
    }
}
NV(nvnCommandBufferEndRecording) {
    nvn_cmd *o = CB(P(0));
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, NOP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    const cmd_rec *r = cmd_host(o);
    uint8_t *copy = flatten(r->seg, (size_t)(r->cur - r->seg));
    /* the copy is tied to the control memory position the console driver's descriptor would take */
    const uint64_t at = o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    o->ctl_used += 64;
    rec_node *n = (rec_node *)malloc(sizeof *n);
    n->ctl = o->ctl ? at : ~0ull;
    n->copy = copy;
    n->marked = 0;
    if (o->ctl && o->ctl_used <= o->ctl_size) {   /* (the mark in the descriptor's place: see sweep_overwritten_copies) */
        const uint64_t mark[2] = {COPY_MARK, (uint64_t)(uintptr_t)copy};
        memcpy(gw(at, sizeof mark), mark, sizeof mark);
        n->marked = 1;
    }
    AcquireSRWLockExclusive(&copy_lock);
    if (!rec_buckets) {
        const char *e = getenv("HOS_NVN_REC_SHIFT");
        if (e && atoi(e) >= 12 && atoi(e) <= 24) rec_shift = (unsigned)atoi(e);
        rec_buckets = (rec_node **)calloc(REC_BUCKETS, sizeof *rec_buckets);
    }
    const unsigned b = rec_bucket(n->ctl);
    n->next = rec_buckets[b];
    rec_buckets[b] = n;
    ncopies++;
    live_handle_add((uint64_t)(uintptr_t)copy);
    static uint64_t made;
    if (++made % 25000 == 0) sweep_overwritten_copies();
    if (made % 50000 == 0)
        hos_log("nvn: %llu recordings made, %zu live copies (%.1f MB), %zu texture copies; %lld command buffers' segments "
                "(%.1f MB)", (unsigned long long)made, ncopies, copy_bytes / 1048576.0, npins, (long long)seg_count,
                seg_bytes / 1048576.0);
    ReleaseSRWLockExclusive(&copy_lock);
    if (watch_draw_cmd && watch_draw_cmd == (void *)o && nwatch_handles < 16) {
        watch_handles[nwatch_handles++] = (uint64_t)(uintptr_t)copy;
        watch_draw_cmd = NULL;
        hos_log("nvn: watched draw's recording is %llx", (unsigned long long)(uintptr_t)copy);
    }
    RET((uint64_t)(uintptr_t)copy);
}
NV(nvnCommandBufferIsRecording) { RET(CB(P(0))->recording != 0); }
static void check_handle(uint64_t h) {
    AcquireSRWLockShared(&copy_lock);
    const int live = live_handle_has(h);
    ReleaseSRWLockShared(&copy_lock);
    if (!live || ((const copy_prefix *)(uintptr_t)h - 1)->magic != COPY_MAGIC)
        die("command handle %#llx is not a live recording", (unsigned long long)h);
}
/* a called recording is copied into the caller at the call (it is complete now; the game may retire it before the
   caller is submitted) */
static void inline_recording(nvn_cmd *o, uint64_t h) {
    check_handle(h);
    const copy_prefix *pre = (const copy_prefix *)(uintptr_t)h - 1;
    const size_t total = (size_t)pre->bytes - sizeof *pre;   /* BEGIN, the records, END: the records in one copy */
    if (total <= 16) return;
    const size_t n = total - 16;
    cmd_rec *r = cmd_host(o);
    cmd_reserve_rec(r, n);
    memcpy(r->cur, (const uint8_t *)(uintptr_t)h + 8, n);
    r->cur += n;
    o->cmd_used += n;
    o->hw_used += n;
    o->last_rec = 0;
}
NV(nvnCommandBufferCallCommands) {
    const uint64_t *h = (const uint64_t *)PN(2, 8ull * (uint32_t)I(1));
    for (int i = 0; i < I(1); i++) {
        if (nwatch_handles) watch_handle_called(c, h[i], "called");
        inline_recording(CB(P(0)), h[i]);
    }
}
NV(nvnCommandBufferCopyCommands) {
    const uint64_t *h = (const uint64_t *)PN(2, 8ull * (uint32_t)I(1));
    for (int i = 0; i < I(1); i++) {
        if (nwatch_handles) watch_handle_called(c, h[i], "copied");
        inline_recording(CB(P(0)), h[i]);
    }
}

/* records: payload after the header, zeroed */
static void *rec(nvn_cmd *o, uint32_t op, size_t payload) {
    const uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(o, OP, T) ((T *)rec((o), (OP), sizeof(T)))
#define RECN(o, OP, T, ARR, N) ((T *)rec((o), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))

NV(nvnCommandBufferSignalEvent) {
    {   /* (the GPU's signals as recorded, 40 then every 5000) */
        static volatile LONG n;
        const LONG k = InterlockedIncrement(&n);
        if (k <= 40 || k % 5000 == 0)
            hos_log("nvn: CommandBufferSignalEvent #%ld: event %#llx mode %u value %u (lr %#llx)", (long)k,
                    (unsigned long long)((const nvn_event *)P(1))->gpu, U32(2), U32(5), (unsigned long long)c->x[30]);
    }
    nvn_p_signal *s = REC(CB(P(0)), NOP_SIGNAL_EVENT, nvn_p_signal);
    s->gpu = ((const nvn_event *)P(1))->gpu;
    s->mode = U32(2);
    s->value = U32(5);
}
/* recorded: the Vulkan back end replays the game's queues on one, and a command buffer that waits for another queue's
   event goes on once that queue's work has signalled it (vk_replay.cpp, submit_job). MK World's worker queue and its
   presentation queue pass stages back and forth this way several times a frame. (Logged, 40 calls then every 5000) */
NV(nvnCommandBufferWaitEvent) {
    static volatile LONG n;
    const LONG k = InterlockedIncrement(&n);
    const nvn_event *e = (const nvn_event *)P(1);
    if (k <= 40 || k % 5000 == 0) {
        uint32_t now = 0;
        if (e) nvn_gpu_read(e->gpu, &now, 4);
        hos_log("nvn: CommandBufferWaitEvent #%ld: event %#llx mode %u value %u (now %u) (lr %#llx)", (long)k,
                (unsigned long long)(e ? e->gpu : 0), U32(2), U32(3), now, (unsigned long long)c->x[30]);
    }
    if (!e) return;
    nvn_p_wait_event *w = REC(CB(P(0)), NOP_WAIT_EVENT, nvn_p_wait_event);
    w->gpu = e->gpu, w->mode = U32(2), w->value = U32(3);
}
NV(nvnCommandBufferWaitSync) {
    static volatile LONG n;
    const LONG k = InterlockedIncrement(&n);
    if (k <= 40 || k % 5000 == 0) hos_log("nvn: CommandBufferWaitSync #%ld (lr %#llx)", (long)k, (unsigned long long)c->x[30]);
}
/* (hookshot) nvnCommandBufferPushComputeConstants(cmdbuf, int ?, offset, size, const void* data): compute push
   constants, which NVN2 compute programs read from their driver bank (c0) after the workgroup counts. OoT pushes a
   colour matrix (0x40 bytes) for the shader that turns its movie's NV12 planes into RGBA. Copied at record time, as
   UpdateUniformBuffer; the replay keeps them in the compute state (vk_bank.cpp serves them). */
NV(nvnCommandBufferPushComputeConstants) {
    const uint64_t off = U64(2), n = U32(3);
    if (!P(4) || !n || n > 0x400 || off > 0x400) return;
    nvn_cmd *o = CB(P(0));
    const uint32_t sz = (uint32_t)(8 + sizeof(nvn_p_push_const) + n);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, NOP_PUSH_CONST, (sz + 7) & ~7u);
    nvn_p_push_const u = {(uint32_t)off, (uint32_t)n};
    memcpy(p + 8, &u, sizeof u);
    memcpy(p + 8 + sizeof u, PN(4, n), n);
}
/* (hookshot) nvnQueueWaitSync(queue, sync): the queue's later work waits on the GPU for the sync's fence. Every
   NVN queue replays in one stream in submit order, so a fence submitted before this already came first; a sync not
   fenced yet (a wait on a fence another queue submits later) cannot be honored that way and is logged. */
NV(nvnQueueWaitSync) {
    const nvn_sync *s = (const nvn_sync *)P(1);
    static volatile LONG n;
    if (s && !s->fenced && InterlockedIncrement(&n) <= 8)
        hos_log("nvn: QueueWaitSync on a sync not fenced yet (%p, lr %#llx): not waited for", (void *)s,
                (unsigned long long)c->x[30]);
}
NV(nvnCommandBufferResetCounter) { REC(CB(P(0)), NOP_RESET_COUNTER, nvn_p_counter)->type = U32(1); }
/* HOS_COND_RECORD (a test, default 0: conditions read at replay). 1: words seen holding 1 read at record, 2: every condition, 3 and 4:
   and reports written at record. All of them gave a wrong picture here: most conditions read reports earlier in the same stream
   and gate single lights, so an early read leaves the scene white or throws white blobs over it. */
#define COND_FLAGS 4096
static uint64_t cond_flags[COND_FLAGS];
static int cond_on_cpu_timeline(void) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_COND_RECORD") ? atoi(getenv("HOS_COND_RECORD")) : 0;   /* (2: every condition, 3: and every report at one, 4: every condition and the flags' reports) */
    return on;
}
NV(nvnCommandBufferReportCounter) {
    nvn_p_counter *r = REC(CB(P(0)), NOP_REPORT_COUNTER, nvn_p_counter);
    r->type = U32(1);
    {   /* (which counters the game reads: the first report of each type, with its caller) */
        static volatile LONG seen[64];
        if (U32(1) < 64 && !InterlockedExchange(&seen[U32(1)], 1))
            hos_log("nvn: the game reports counter type %u (lr %#llx)", U32(1), (unsigned long long)c->x[30]);
    }
    r->addr = U64(2);
    if (cond_on_cpu_timeline() && cond_on_cpu_timeline() != 2 && __atomic_load_n(&cond_flags[(r->addr >> 4) & (COND_FLAGS - 1)], __ATOMIC_RELAXED) == r->addr) {
        const uint64_t zero = 0;   /* (a clear flag: reset now; the replay leaves this word alone) */
        nvn_gpu_write(r->addr, &zero, 8);
        r->type |= NVN_COUNTER_WRITTEN;
    }
}
/* (hookshot) nvnCommandBufferClearBuffer(cmdbuf, BufferAddress dst, size_t size, uint32_t value): OoT's gameplay
   clears small GPU buffers (counters, 8 bytes) with it */
NV(nvnCommandBufferClearBuffer) {
    nvn_p_clear_buf *r = REC(CB(P(0)), NOP_CLEAR_BUF, nvn_p_clear_buf);
    r->dst = U64(1), r->size = U64(2), r->value = U32(3), r->pad = 0;
}
NV(nvnCommandBufferCopyBufferToBuffer) {
    nvn_p_copy_bb *r = REC(CB(P(0)), NOP_COPY_B2B, nvn_p_copy_bb);
    r->src = U64(1), r->dst = U64(2), r->size = U64(3);
}
/* the data is copied at record time (the caller may change it afterwards); written in GPU order at replay */
NV(nvnCommandBufferUpdateUniformBuffer) {
    const uint64_t n = U64(4);
    /* (hookshot) NVN's limit is 64 KiB an update; a larger size is not recorded (its record size is 32-bit) */
    if (n > 0x10000) die("UpdateUniformBuffer of %#llx bytes", (unsigned long long)n);
    nvn_cmd *o = CB(P(0));
    const uint32_t sz = (uint32_t)(8 + sizeof(nvn_p_update_ub) + n);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, NOP_UPDATE_UB, (sz + 7) & ~7u);
    nvn_p_update_ub u = {U64(1) + U64(3), n};
    memcpy(p + 8, &u, sizeof u);
    memcpy(p + 8 + sizeof u, PN(5, n), n);
}

static void set_view(nvn_view *dst, uint8_t *has, uint64_t v) {
    *has = v != 0;
    if (v) memcpy(dst, gw(v, sizeof *dst), sizeof *dst);
}
static void tex_copy(const nvn_tex **dst, uint64_t t);
/* HOS_NVN_WATCH_RT=<guest hex> (debugging): who sets that render target, as guest call chains (each new one once),
   with the texture's description and view */
static void watch_rt(Ctx *c, const uint64_t *ct, const uint64_t *cv, int n) {
    static uint64_t want = ~0ull;
    if (want == ~0ull) want = getenv("HOS_NVN_WATCH_RT") ? strtoull(getenv("HOS_NVN_WATCH_RT"), NULL, 16) : 0;
    if (!want || !ct) return;
    for (int i = 0; i < n; i++) {
        if (!ct[i] || nvn_gpu_guest(((const nvn_tex *)GP(ct[i]))->gpu) != want) continue;
        char b[400];
        guest_chain(c, b, sizeof b);
        static uint64_t seen[64];
        static int nseen;
        uint64_t key = 0;
        for (const char *q = b; *q; q++) key = key * 0x100000001B3ull ^ (uint8_t)*q;
        for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
        if (nseen < 64) seen[nseen++] = key;
        const nvn_tex *t = (const nvn_tex *)GP(ct[i]);
        const nvn_view *v = cv && cv[i] ? (const nvn_view *)GP(cv[i]) : NULL;
        hos_log("nvn: render target %llx (target %d format %#x %ux%ux%u levels %u; view format %d:%#x layers %d:%u+%u) "
                "set from %s", (unsigned long long)want, t->d.target, t->d.format, t->d.width, t->d.height, t->d.depth,
                t->d.levels, v ? v->set_format : -1, v ? v->format : 0, v ? v->set_layers : -1, v ? v->min_layer : 0,
                v ? v->nlayers : 0, b);
        return;
    }
}
NV(nvnCommandBufferSetRenderTargets) {
    nvn_cmd *o = CB(P(0));
    int n = I(1);
    if (n < 0 || n > 8) die("SetRenderTargets with %d targets", n);
    const uint64_t *ct = (const uint64_t *)PN(2, 8 * (size_t)n), *cv = (const uint64_t *)PN(3, 8 * (size_t)n);
    watch_rt(c, ct, cv, n);
    nvn_p_set_rt *r = REC(o, NOP_SET_RT, nvn_p_set_rt);
    r->n = n;
    for (int i = 0; i < n; i++) {
        tex_copy(&r->color[i], ct ? ct[i] : 0);
        set_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : 0);
    }
    tex_copy(&r->depth, U64(4));
    set_view(&r->dview, &r->has_dview, U64(5));
}
NV(nvnCommandBufferClearColor) {
    nvn_p_clear_color *r = REC(CB(P(0)), NOP_CLEAR_COLOR, nvn_p_clear_color);
    r->index = I(1);
    memcpy(r->rgba, PN(2, 16), 16);
    r->mask = I(3);
}
NV(nvnCommandBufferClearDepthStencil) {
    nvn_p_clear_ds *r = REC(CB(P(0)), NOP_CLEAR_DS, nvn_p_clear_ds);
    r->depth = F(0);
    r->depth_mask = BOOL(1);
    r->stencil = I(2);
    r->stencil_mask = I(3);
}
NV(nvnCommandBufferSetViewports) {
    const int first = I(1), n = I(2);
    if (n < 0 || first < 0 || first + n > 16) die("SetViewports %d+%d", first, n);
    nvn_p_viewports *r = REC(CB(P(0)), NOP_VIEWPORTS, nvn_p_viewports);
    r->first = first, r->n = n;
    memcpy(r->v, PN(3, 16 * (size_t)n), 16 * (size_t)n);
}
NV(nvnCommandBufferSetViewport) {
    nvn_p_viewports *r = REC(CB(P(0)), NOP_VIEWPORTS, nvn_p_viewports);
    r->first = 0, r->n = 1;
    r->v[0][0] = (float)I(1), r->v[0][1] = (float)I(2), r->v[0][2] = (float)I(3), r->v[0][3] = (float)I(4);
}
NV(nvnCommandBufferSetScissor) {
    nvn_p_scissor *r = REC(CB(P(0)), NOP_SCISSOR, nvn_p_scissor);
    r->x = I(1), r->y = I(2), r->w = I(3), r->h = I(4);
}
/* one scissor and depth range are replayed (index 0); differing others are reported */
NV(nvnCommandBufferSetScissors) {
    const int first = I(1), n = I(2);
    const int *v = (const int *)P(3);
    for (int i = 1; i < n; i++)
        if (memcmp(v + 4 * i, v, 16)) { static int once; if (!once++) hos_log("nvn: SetScissors: scissor %d differs (index 0 used)", first + i); }
    if (first == 0 && n > 0) {
        nvn_p_scissor *r = REC(CB(P(0)), NOP_SCISSOR, nvn_p_scissor);
        r->x = v[0], r->y = v[1], r->w = v[2], r->h = v[3];
    }
}
NV(nvnCommandBufferSetDepthRange) {
    nvn_p_depth_range *r = REC(CB(P(0)), NOP_DEPTH_RANGE, nvn_p_depth_range);
    r->n = F(0), r->f = F(1);
}
NV(nvnCommandBufferSetDepthRanges) {
    const int first = I(1), n = I(2);
    const float *v = (const float *)P(3);
    if (first == 0 && n > 0) {
        nvn_p_depth_range *r = REC(CB(P(0)), NOP_DEPTH_RANGE, nvn_p_depth_range);
        r->n = v[0], r->f = v[1];
    }
}
NV(nvnCommandBufferSetDepthClamp) { REC(CB(P(0)), NOP_DEPTH_CLAMP, nvn_p_flag)->value = BOOL(1) != 0; }
NV(nvnCommandBufferSetRenderEnable) { REC(CB(P(0)), NOP_RENDER_ENABLE, nvn_p_flag)->value = BOOL(1) != 0; }
/* rendering on when the two NVNcounterData.counter values at addr, addr + 16 are equal (mode 0) / differ (1). TOTK gates
   clears with it: the CPU writes counter = 1 to ask for a clear and the TIMESTAMP_TOP report after it resets it */
NV(nvnCommandBufferSetRenderEnableConditional) {
    if (I(1) != 0 && I(1) != 1) die("SetRenderEnableConditional mode %d", I(1));
    nvn_p_render_cond *r = REC(CB(P(0)), NOP_RENDER_COND, nvn_p_render_cond);
    r->mode = I(1);
    r->addr = U64(2);
    r->snapped = 0;
    r->x = r->y = 0;
    if (cond_on_cpu_timeline()) {
        uint64_t *flag = &cond_flags[(r->addr >> 4) & (COND_FLAGS - 1)];
        nvn_gpu_read(r->addr, &r->x, 8);
        nvn_gpu_read(r->addr + 16, &r->y, 8);
        if ((r->x == 1 && r->y == 0) || cond_on_cpu_timeline() == 3) __atomic_store_n(flag, r->addr, __ATOMIC_RELAXED);
        r->snapped = cond_on_cpu_timeline() >= 2 || __atomic_load_n(flag, __ATOMIC_RELAXED) == r->addr;
    }
}
NV(nvnCommandBufferSetRasterizerDiscard) { REC(CB(P(0)), NOP_RASTER_DISCARD, nvn_p_flag)->value = BOOL(1) != 0; }
NV(nvnCommandBufferBindProgram) {
    nvn_p_bind_program *r = REC(CB(P(0)), NOP_BIND_PROGRAM, nvn_p_bind_program);
    r->prog = (const nvn_prog *)P(1);
    r->stages = I(2);
}
static void bind_state(Ctx *c, int kind, size_t n) {
    nvn_p_bind_state *r = REC(CB(P(0)), NOP_BIND_STATE, nvn_p_bind_state);
    r->kind = kind;
    memcpy(r->bytes, PN(1, n), n);
}
NV(nvnCommandBufferBindBlendState) { bind_state(c, 0, sizeof(nvn_blend_st)); }
NV(nvnCommandBufferBindColorState) { bind_state(c, 1, sizeof(nvn_color_st)); }
NV(nvnCommandBufferBindChannelMaskState) { bind_state(c, 2, sizeof(nvn_chmask_st)); }
NV(nvnCommandBufferBindMultisampleState) { bind_state(c, 3, sizeof(nvn_ms_st)); }
NV(nvnCommandBufferBindPolygonState) { bind_state(c, 4, sizeof(nvn_poly_st)); }
NV(nvnCommandBufferBindDepthStencilState) { bind_state(c, 5, sizeof(nvn_ds_st)); }
NV(nvnCommandBufferBindVertexAttribState) {
    const int n = I(1);
    if (n < 0 || n > 16) die("BindVertexAttribState %d", n);
    nvn_cmd *o = CB(P(0));
    nvn_p_bind_vattr *r = RECN(o, NOP_BIND_VATTR, nvn_p_bind_vattr, st, n);
    r->first = 0, r->n = n;
    memcpy(r->st, PN(2, 4 * (size_t)n), 4 * (size_t)n);
    hw_fix(o, 68);   /* the driver writes all 16 attribute formats: 17 words */
}
NV(nvnCommandBufferBindVertexStreamState) {
    const int n = I(1);
    if (n < 0 || n > 16) die("BindVertexStreamState %d", n);
    nvn_cmd *o = CB(P(0));
    nvn_p_bind_vstream *r = RECN(o, NOP_BIND_VSTREAM, nvn_p_bind_vstream, st, n);
    r->first = 0, r->n = n;
    memcpy(r->st, PN(2, 8 * (size_t)n), 8 * (size_t)n);
    hw_fix(o, 4 * (16 + (uint64_t)n));
}
static void bind_vbufs(nvn_cmd *o, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first < 0 || first + n > 16) die("BindVertexBuffers %d+%d", first, n);
    nvn_p_bind_vbuf *r = RECN(o, NOP_BIND_VBUF, nvn_p_bind_vbuf, r, n);
    r->first = first, r->n = n;
    memcpy(r->r, rg, sizeof *rg * (size_t)n);
    hw_fix(o, 24 * (uint64_t)n);
}
NV(nvnCommandBufferBindVertexBuffers) { bind_vbufs(CB(P(0)), I(1), I(2), (const NVNbufferRange *)P(3)); }
NV(nvnCommandBufferBindVertexBuffer) { NVNbufferRange r = {U64(2), U64(3)}; bind_vbufs(CB(P(0)), I(1), 1, &r); }
static void bind_buf(nvn_cmd *o, uint32_t op, int stage, int index, uint64_t addr, uint64_t size) {
    nvn_p_bind_buf *r = REC(o, op, nvn_p_bind_buf);
    r->stage = stage, r->index = index, r->addr = addr, r->size = size;
    hw_fix(o, 20);
}
NV(nvnCommandBufferBindUniformBuffer) { bind_buf(CB(P(0)), NOP_BIND_UBO, I(1), I(2), U64(3), U64(4)); }
NV(nvnCommandBufferBindStorageBuffer) { bind_buf(CB(P(0)), NOP_BIND_SSBO, I(1), I(2), U64(3), U64(4)); }
static void bind_bufs(Ctx *c, uint32_t op) {
    const NVNbufferRange *r = (const NVNbufferRange *)PN(4, I(3) > 0 ? sizeof *r * (uint64_t)I(3) : 0);
    for (int i = 0; i < I(3); i++) bind_buf(CB(P(0)), op, I(1), I(2) + i, r[i].address, r[i].size);
}
NV(nvnCommandBufferBindUniformBuffers) { bind_bufs(c, NOP_BIND_UBO); }
NV(nvnCommandBufferBindStorageBuffers) { bind_bufs(c, NOP_BIND_SSBO); }
/* deferred: the ranges are read from the game's array at submit (TOTK fills it for the frame just before) */
static void bind_bufs_deferred(Ctx *c, int ssbo) {
    const int n = I(3);
    if (n < 0 || n > 16) die("deferred bind of %d buffers", n);
    nvn_p_bind_bufs *r = RECN(CB(P(0)), NOP_BIND_BUFS, nvn_p_bind_bufs, r, n);
    r->ssbo = ssbo, r->stage = I(1), r->first = I(2), r->n = n, r->src = U64(4);
    memcpy(r->r, PN(4, sizeof(NVNbufferRange) * (size_t)n), sizeof(NVNbufferRange) * (size_t)n);
    hw_fix(CB(P(0)), 16 + 16 * (uint64_t)n);
}
NV(nvnCommandBufferBindUniformBuffersDeferred) { bind_bufs_deferred(c, 0); }
NV(nvnCommandBufferBindStorageBuffersDeferred) { bind_bufs_deferred(c, 1); }
static void bind_handles(nvn_cmd *o, uint32_t op, int stage, int first, int n, const uint64_t *h, uint64_t src,
                         uint64_t hw) {
    if (n < 0 || n > 32) die("bind of %d handles", n);
    nvn_p_bind_handles *r = RECN(o, op, nvn_p_bind_handles, h, n);
    r->stage = stage, r->first = first, r->n = n, r->src = src;
    memcpy(r->h, h, 8 * (size_t)n);
    hw_fix(o, hw ? hw : 16 + 4 * (uint64_t)n);
}
/* HOS_NVN_WATCH_TEX=<handle hex> (debugging): who binds that texture handle, as guest call chains (frame pointers),
   each new chain logged once */
static void watch_tex(Ctx *c, const uint64_t *h, int n) {
    static uint64_t want = ~0ull;
    if (want == ~0ull) want = getenv("HOS_NVN_WATCH_TEX") ? strtoull(getenv("HOS_NVN_WATCH_TEX"), NULL, 16) : 0;
    if (!want) return;
    for (int i = 0; i < n; i++) {
        if ((h[i] & 0xFFFFFFFFull) != want) continue;
        uint64_t chain[8] = {c->x[30]}, fp = c->x[29];
        for (int k = 1; k < 8 && fp && fp < 0x8000000000ull && !(fp & 7); k++) chain[k] = G64(fp + 8), fp = G64(fp);
        static uint64_t seen[64];
        static int nseen;
        uint64_t key = 0;
        for (int k = 0; k < 8; k++) key = key * 0x9E3779B97F4A7C15ull + chain[k];
        for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
        if (nseen < 64) seen[nseen++] = key;
        hos_log("nvn: texture %llx bound from %llx < %llx < %llx < %llx < %llx < %llx < %llx < %llx",
                (unsigned long long)want, (unsigned long long)chain[0], (unsigned long long)chain[1],
                (unsigned long long)chain[2], (unsigned long long)chain[3], (unsigned long long)chain[4],
                (unsigned long long)chain[5], (unsigned long long)chain[6], (unsigned long long)chain[7]);
        return;
    }
}
NV(nvnCommandBufferBindTexture) { const uint64_t h = U64(3); watch_tex(c, &h, 1); bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), 1, &h, 0, 0); }
NV(nvnCommandBufferBindTextures) { watch_tex(c, (const uint64_t *)P(4), I(3)); bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 0); }
NV(nvnCommandBufferBindImage) { const uint64_t h = U64(3); bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), 1, &h, 0, 24); }
NV(nvnCommandBufferBindImages) { bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateTexture) { const uint64_t h = U64(3); bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), 1, &h, 0, 24); }
NV(nvnCommandBufferBindSeparateTextures) { bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateSampler) { const uint64_t h = U64(3); bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), 1, &h, 0, 24); }
NV(nvnCommandBufferBindSeparateSamplers) { bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 16 + 8 * (uint64_t)I(3)); }
/* NVN 55 *Deferred binds (what the inline fast paths call: always, see nvn_cmd.inline_cur): the console driver only
   records the game's handle array and reads it at submit; the game fills material tables after recording */
NV(nvnCommandBufferBindTexturesDeferred) { watch_tex(c, (const uint64_t *)P(4), I(3)); bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 0); }
NV(nvnCommandBufferBindImagesDeferred) { bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateTexturesDeferred) { bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateSamplersDeferred) { bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 16 + 8 * (uint64_t)I(3)); }
static void stencil(Ctx *c, int which) {
    nvn_p_stencil *r = REC(CB(P(0)), NOP_STENCIL, nvn_p_stencil);
    r->face = I(1), r->which = which;
    if (which == 0) r->value_mask = I(2);
    else if (which == 1) r->mask = I(2);
    else r->ref = I(2);
}
NV(nvnCommandBufferSetStencilValueMask) { stencil(c, 0); }
NV(nvnCommandBufferSetStencilMask) { stencil(c, 1); }
NV(nvnCommandBufferSetStencilRef) { stencil(c, 2); }
NV(nvnCommandBufferSetPolygonOffsetClamp) {
    nvn_p_poly_offset *r = REC(CB(P(0)), NOP_POLY_OFFSET, nvn_p_poly_offset);
    r->factor = F(0), r->units = F(1), r->clamp = F(2);
}
NV(nvnCommandBufferSetPrimitiveRestart) {
    nvn_p_restart *r = REC(CB(P(0)), NOP_PRIMITIVE_RESTART, nvn_p_restart);
    r->enable = BOOL(1), r->index = I(2);
}
NV(nvnCommandBufferDrawArrays) {
    nvn_p_draw *r = REC(CB(P(0)), NOP_DRAW, nvn_p_draw);
    r->prim = I(1), r->first = I(2), r->count = I(3), r->instances = 1;
}
NV(nvnCommandBufferDrawArraysInstanced) {
    if (I(5) <= 0) return;   /* no instances: nothing */
    nvn_p_draw *r = REC(CB(P(0)), NOP_DRAW, nvn_p_draw);
    r->prim = I(1), r->first = I(2), r->count = I(3), r->base_instance = I(4), r->instances = I(5);
}
/* HOS_NVN_WATCH_DRAW=<index count> (debugging): who records indexed draws of that size (the first 60, call chains);
   the recording that holds one is followed: each call of it is logged (the first 60) */
static void watch_draw(Ctx *c, int count) {
    static int want = -1;
    static int64_t after;
    static volatile LONG logged;
    if (want < 0) {   /* (<count>,<after s>: from that much time on) */
        const char *e = getenv("HOS_NVN_WATCH_DRAW");
        want = e ? atoi(e) : 0;
        after = hos_now_ns() + (e && strchr(e, ',') ? (int64_t)(atof(strchr(e, ',') + 1) * 1e9) : 0);
    }
    if (!want || count != want || hos_now_ns() < after) return;
    const char *only = getenv("HOS_NVN_WATCH_DRAW_THREAD");   /* (only draws recorded on threads of that name) */
    if (only && (!hos_self() || !strstr(hos_thread_name(hos_self()), only))) return;
    if (InterlockedIncrement(&logged) > 60) return;
    watch_draw_cmd = P(0);
    char b[400];
    guest_chain(c, b, sizeof b);
    hos_log("nvn: draw of %d indices recorded from %s", count, b);
}
static void draw_idx(Ctx *c, int base_vertex, int base_instance, int instances, uint64_t hw) {
    nvn_cmd *o = CB(P(0));
    watch_draw(c, I(3));
    nvn_p_draw_idx *r = REC(o, NOP_DRAW_IDX, nvn_p_draw_idx);
    r->prim = I(1), r->itype = I(2), r->count = I(3), r->ibuf = U64(4);
    r->base_vertex = base_vertex, r->base_instance = base_instance, r->instances = instances;
    hw_fix(o, hw);
}
NV(nvnCommandBufferDrawElements) { draw_idx(c, 0, 0, 1, 48); }
NV(nvnCommandBufferDrawElementsBaseVertex) { draw_idx(c, I(5), 0, 1, 44); }
NV(nvnCommandBufferDrawElementsInstanced) { if (I(7) > 0) draw_idx(c, I(5), I(6), I(7), 44); }
NV(nvnCommandBufferDispatchCompute) {
    nvn_p_dispatch *r = REC(CB(P(0)), NOP_DISPATCH, nvn_p_dispatch);
    r->x = I(1), r->y = I(2), r->z = I(3);
}
NV(nvnCommandBufferBarrier) { *(int32_t *)rec(CB(P(0)), NOP_BARRIER, 8) = I(1); }
NV(nvnCommandBufferSetTexturePool) { REC(CB(P(0)), NOP_SET_TEX_POOL, nvn_p_set_pool)->pool = (const nvn_dpool *)P(1); }
NV(nvnCommandBufferSetSamplerPool) { REC(CB(P(0)), NOP_SET_SAMP_POOL, nvn_p_set_pool)->pool = (const nvn_dpool *)P(1); }
NV(nvnCommandBufferSetShaderScratchMemory) {}

/* Records keep textures by pointer to an immutable copy: the game may finalize or reuse a texture object after
   recording, before the submit. One copy per distinct content (the object with its serial left out): TOTK initializes
   its transient targets again every frame, the same texture under a new serial, and one copy per serial filled the
   table in a long session ("texture copies table full", 2026-10-20). The replay reads the copies' contents only.
   Copies live for the session (~100 bytes each); the table doubles when half full. */
static nvn_tex **pins;
static size_t pin_slots;
static SRWLOCK pin_lock = SRWLOCK_INIT;
static uint64_t pin_hash(const nvn_tex *o) {   /* (FNV-1a over the object, its serial as 0) */
    nvn_tex k = *o;
    k.serial = 0;
    const uint8_t *p = (const uint8_t *)&k;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static int pin_same(const nvn_tex *a, const nvn_tex *b) {
    nvn_tex x = *a, y = *b;
    x.serial = y.serial = 0;
    return !memcmp(&x, &y, sizeof x);
}
static void pins_grow(void) {
    const size_t n = pin_slots ? pin_slots * 2 : (1u << 16);
    nvn_tex **t = (nvn_tex **)calloc(n, sizeof *t);
    if (!t) die("texture copies table (%zu slots)", n);
    for (size_t i = 0; i < pin_slots; i++)
        if (pins[i]) {
            size_t s = (size_t)(pin_hash(pins[i]) >> 20) & (n - 1);
            while (t[s]) s = (s + 1) & (n - 1);
            t[s] = pins[i];
        }
    free(pins);
    pins = t, pin_slots = n;
}
static void tex_copy(const nvn_tex **dst, uint64_t t) {
    if (!t) { *dst = NULL; return; }
    const nvn_tex *o = (const nvn_tex *)gw(t, sizeof(nvn_tex));
    if (!o->serial) die("recording refers to an uninitialized texture object %#llx", (unsigned long long)t);
    const uint64_t h = pin_hash(o);
    AcquireSRWLockExclusive(&pin_lock);
    if (npins * 2 >= pin_slots) pins_grow();
    size_t s = (size_t)(h >> 20) & (pin_slots - 1);
    while (pins[s] && !pin_same(pins[s], o)) s = (s + 1) & (pin_slots - 1);
    if (!pins[s]) {
        pins[s] = (nvn_tex *)malloc(sizeof(nvn_tex));
        *pins[s] = *o;
        npins++;
    }
    *dst = pins[s];
    ReleaseSRWLockExclusive(&pin_lock);
}
NV(nvnCommandBufferCopyBufferToTexture) {
    nvn_p_copy_bt *r = REC(CB(P(0)), NOP_COPY_B2T, nvn_p_copy_bt);
    r->buf = U64(1);
    tex_copy(&r->tex, U64(2));
    set_view(&r->view, &r->has_view, U64(3));
    r->r = *(const NVNcopyRegion *)P(4);
    r->flags = I(5);
    {   /* (hookshot, RX_VK_LOG_B2T=1) who records the big copies: the caller's chain, each new source once */
        static int on = -1;
        if (on < 0) on = env_on("RX_VK_LOG_B2T");
        static uint64_t seen[16];
        static volatile LONG nseen;
        int k;
        for (k = 0; k < nseen && seen[k] != r->buf; k++) {}
        if (on && r->r.width >= 960 && k == nseen && nseen < 16) {
            seen[InterlockedIncrement(&nseen) - 1] = r->buf;
            char b[400];
            guest_chain(c, b, sizeof b);
            hos_log("nvn: CopyBufferToTexture from %#llx (%dx%d): %s", (unsigned long long)r->buf, r->r.width, r->r.height, b);
        }
    }
}
NV(nvnCommandBufferCopyTextureToBuffer) {
    nvn_p_copy_bt *r = REC(CB(P(0)), NOP_COPY_T2B, nvn_p_copy_bt);
    tex_copy(&r->tex, U64(1));
    set_view(&r->view, &r->has_view, U64(2));
    r->r = *(const NVNcopyRegion *)P(3);
    r->buf = U64(4);
    r->flags = I(5);
}
NV(nvnCommandBufferCopyTextureToTexture) {
    nvn_p_copy_tt *r = REC(CB(P(0)), NOP_COPY_T2T, nvn_p_copy_tt);
    tex_copy(&r->src, U64(1));
    set_view(&r->sv, &r->has_sv, U64(2));
    r->sr = *(const NVNcopyRegion *)P(3);
    tex_copy(&r->dst, U64(4));
    set_view(&r->dv, &r->has_dv, U64(5));
    r->dr = *(const NVNcopyRegion *)P(6);
    r->flags = I(7);
}
static void clear_tex(Ctx *c, int is_uint) {
    nvn_p_clear_tex *r = REC(CB(P(0)), NOP_CLEAR_TEX, nvn_p_clear_tex);
    tex_copy(&r->tex, U64(1));
    set_view(&r->view, &r->has_view, U64(2));
    r->r = *(const NVNcopyRegion *)P(3);
    memcpy(r->c.f, PN(4, 16), 16);
    r->mask = I(5);
    r->is_uint = (uint8_t)is_uint;
}
NV(nvnCommandBufferClearTexture) { clear_tex(c, 0); }
NV(nvnCommandBufferClearTextureui) { clear_tex(c, 1); }
NV(nvnCommandBufferSetCopyRowStride) { REC(CB(P(0)), NOP_COPY_STRIDE, nvn_p_copy_stride)->stride = (int64_t)U64(1); }
/* (hookshot) the buffer stride between layers of a 3D / array copy; OoT sets 0 (tightly packed, the default the
   replay assumes). A real stride is logged: the replay would need it. */
NV(nvnCommandBufferSetCopyImageStride) {
    static int once;
    if (U64(1) && !once++) hos_log("nvn: SetCopyImageStride(%lld): not replayed (layers copied tightly packed)", (long long)U64(1));
}
NV(nvnCommandBufferSetBlendColor) { memcpy(REC(CB(P(0)), NOP_BLEND_COLOR, nvn_p_blend_color)->rgba, PN(1, 16), 16); }

/* hints and state with no effect on the picture here: lines and points come out of the shaders, ZCull and the tiled
   cache are Tegra hardware; non-default values are reported once */
NV(nvnCommandBufferSetLineWidth) { if (F(0) != 1.0f) { static int once; if (!once++) hos_log("nvn: SetLineWidth %g: lines are one pixel", F(0)); } }
NV(nvnCommandBufferSetPointSize) {}
NV(nvnCommandBufferSetDepthBounds) {
    if (BOOL(1) && (F(0) > 0.0f || F(1) < 1.0f)) { static int once; if (!once++) hos_log("nvn: SetDepthBounds [%g, %g]: no depth bounds test", F(0), F(1)); }
}
NV(nvnCommandBufferSetTiledCacheAction) {}
NV(nvnCommandBufferSetTiledCacheTileSize) {}
NV(nvnCommandBufferSetSubpixelPrecisionBias) {}
NV(nvnCommandBufferDiscardColor) {}
NV(nvnCommandBufferDiscardDepthStencil) {}
NV(nvnCommandBufferSaveZCullData) {}
NV(nvnCommandBufferRestoreZCullData) {}
NV(nvnCommandBufferSetAlphaRef) {}
NV(nvnCommandBufferSetSampleMask) {}
NV(nvnCommandBufferSetConservativeRasterEnable) {}
NV(nvnCommandBufferSetConservativeRasterDilate) {}
NV(nvnCommandBufferSetPatchSize) {}
NV(nvnCommandBufferPushDebugGroupStatic) {}
NV(nvnCommandBufferPushDebugGroupDynamic) {}
NV(nvnCommandBufferPushDebugGroup) {}
NV(nvnCommandBufferPopDebugGroup) {}
NV(nvnCommandBufferPopDebugGroupId) {}
NV(nvnCommandBufferInsertDebugMarkerStatic) {}
NV(nvnCommandBufferInsertDebugMarkerDynamic) {}
NV(nvnCommandBufferInsertDebugMarker) {}

/* ---- state objects: packed in place ---- */
#define ST(T, p) ((T *)(p))
NV(nvnBlendStateSetDefaults) {
    nvn_blend_st *b = ST(nvn_blend_st, P(0));
    memset(b, 0, sizeof *b);
    b->src_rgb = NVN_BLEND_FUNC_ONE, b->dst_rgb = NVN_BLEND_FUNC_ZERO, b->src_a = NVN_BLEND_FUNC_ONE, b->dst_a = NVN_BLEND_FUNC_ZERO;
    b->eq_rgb = NVN_BLEND_EQUATION_ADD, b->eq_a = NVN_BLEND_EQUATION_ADD;
}
NV(nvnBlendStateSetBlendTarget) { ST(nvn_blend_st, P(0))->target = U32(1); }
NV(nvnBlendStateSetBlendFunc) {
    nvn_blend_st *b = ST(nvn_blend_st, P(0));
    b->src_rgb = U32(1), b->dst_rgb = U32(2), b->src_a = U32(3), b->dst_a = U32(4);
}
NV(nvnBlendStateSetBlendEquation) { ST(nvn_blend_st, P(0))->eq_rgb = U32(1); ST(nvn_blend_st, P(0))->eq_a = U32(2); }
NV(nvnBlendStateSetAdvancedMode) { if (I(1)) die("advanced blend mode %d", I(1)); }
NV(nvnBlendStateSetAdvancedOverlap) {}
NV(nvnBlendStateSetAdvancedPremultipliedSrc) {}
NV(nvnBlendStateSetAdvancedNormalizedDst) {}
NV(nvnColorStateSetDefaults) {
    nvn_color_st *s = ST(nvn_color_st, P(0));
    memset(s, 0, sizeof *s);
    s->logic_op = NVN_LOGIC_OP_COPY;
    s->alpha_func = NVN_ALPHA_FUNC_ALWAYS;
}
NV(nvnColorStateSetBlendEnable) {
    nvn_color_st *s = ST(nvn_color_st, P(0));
    const int i = I(1);
    if (i < 0 || i > 7) return;
    s->blend_en = BOOL(2) ? (s->blend_en | 1u << i) : (s->blend_en & ~(1u << i));
}
NV(nvnColorStateSetLogicOp) { ST(nvn_color_st, P(0))->logic_op = U32(1); }
NV(nvnColorStateSetAlphaTest) { ST(nvn_color_st, P(0))->alpha_func = U32(1); }
NV(nvnChannelMaskStateSetDefaults) { ST(nvn_chmask_st, P(0))->mask = 0xffffffffu; }
NV(nvnChannelMaskStateSetChannelMask) {
    static int chmask_log = -1;   /* (asked once: getenv on every call was 15% of the render thread) */
    if (chmask_log < 0) chmask_log = env_on("HOS_NVN_CHMASK_LOG");
    if (chmask_log) {   /* (debugging: NVN2's arguments) */
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 40)
            hos_log("nvn: SetChannelMask(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) lr %#llx", (unsigned long long)c->x[0],
                    (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                    (unsigned long long)c->x[4], (unsigned long long)c->x[5], (unsigned long long)c->x[30]);
    }
    const int i = I(1);
    if (i < 0 || i > 7) return;
    const uint32_t m = (BOOL(2) ? 1 : 0) | (BOOL(3) ? 2 : 0) | (BOOL(4) ? 4 : 0) | (BOOL(5) ? 8 : 0);
    nvn_chmask_st *s = ST(nvn_chmask_st, P(0));
    s->mask = (s->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NV(nvnMultisampleStateSetDefaults) { memset(P(0), 0, sizeof(nvn_ms_st)); ST(nvn_ms_st, P(0))->enable = 1; }
NV(nvnMultisampleStateSetMultisampleEnable) { ST(nvn_ms_st, P(0))->enable = BOOL(1) != 0; }
NV(nvnMultisampleStateSetSamples) { ST(nvn_ms_st, P(0))->samples = (uint8_t)I(1); }
NV(nvnMultisampleStateSetAlphaToCoverageEnable) { ST(nvn_ms_st, P(0))->a2c = BOOL(1) != 0; }
NV(nvnMultisampleStateSetAlphaToCoverageDither) { ST(nvn_ms_st, P(0))->a2c_dither = BOOL(1) != 0; }
NV(nvnMultisampleStateSetRasterSamples) { ST(nvn_ms_st, P(0))->raster_samples = (uint8_t)I(1); }
NV(nvnMultisampleStateSetSampleLocationsEnable) { ST(nvn_ms_st, P(0))->sl_en = BOOL(1) != 0; }
NV(nvnMultisampleStateSetSampleLocations) {}
NV(nvnPolygonStateSetDefaults) {
    nvn_poly_st *s = ST(nvn_poly_st, P(0));
    memset(s, 0, sizeof *s);
    s->cull = NVN_FACE_NONE, s->front = NVN_FRONT_FACE_CCW, s->mode = NVN_POLYGON_MODE_FILL;
}
NV(nvnPolygonStateSetCullFace) { ST(nvn_poly_st, P(0))->cull = U32(1); }
NV(nvnPolygonStateSetFrontFace) { ST(nvn_poly_st, P(0))->front = U32(1); }
NV(nvnPolygonStateSetPolygonMode) { ST(nvn_poly_st, P(0))->mode = U32(1); }
NV(nvnPolygonStateSetPolygonOffsetEnables) { ST(nvn_poly_st, P(0))->offset_en = U32(1); }
NV(nvnDepthStencilStateSetDefaults) {
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    memset(s, 0, sizeof *s);
    s->dw = 1, s->df = NVN_DEPTH_FUNC_LESS, s->sf_f = s->sf_b = NVN_STENCIL_FUNC_ALWAYS;
    s->fail_f = s->zfail_f = s->zpass_f = s->fail_b = s->zfail_b = s->zpass_b = NVN_STENCIL_OP_KEEP;
}
NV(nvnDepthStencilStateSetDepthTestEnable) { ST(nvn_ds_st, P(0))->dt = BOOL(1) != 0; }
NV(nvnDepthStencilStateSetDepthWriteEnable) { ST(nvn_ds_st, P(0))->dw = BOOL(1) != 0; }
NV(nvnDepthStencilStateSetDepthFunc) { ST(nvn_ds_st, P(0))->df = U32(1); }
NV(nvnDepthStencilStateSetStencilTestEnable) { ST(nvn_ds_st, P(0))->st = BOOL(1) != 0; }
NV(nvnDepthStencilStateSetStencilFunc) {
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    if (U32(1) & NVN_FACE_FRONT) s->sf_f = U32(2);
    if (U32(1) & NVN_FACE_BACK) s->sf_b = U32(2);
}
NV(nvnDepthStencilStateSetStencilOp) {
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    if (U32(1) & NVN_FACE_FRONT) s->fail_f = U32(2), s->zfail_f = U32(3), s->zpass_f = U32(4);
    if (U32(1) & NVN_FACE_BACK) s->fail_b = U32(2), s->zfail_b = U32(3), s->zpass_b = U32(4);
}
NV(nvnVertexAttribStateSetDefaults) { memset(P(0), 0, sizeof(nvn_va_st)); }
NV(nvnVertexAttribStateSetFormat) {
    if (U64(2) >= (1u << 19)) hos_log("nvn: vertex attribute offset %#llx too large", (unsigned long long)U64(2));
    ST(nvn_va_st, P(0))->fmt = U32(1);
    ST(nvn_va_st, P(0))->offset = (uint32_t)U64(2);
}
NV(nvnVertexAttribStateSetStreamIndex) { ST(nvn_va_st, P(0))->stream = U32(1); }
NV(nvnVertexStreamStateSetDefaults) { memset(P(0), 0, sizeof(nvn_vs_st)); }
NV(nvnVertexStreamStateSetStride) { ST(nvn_vs_st, P(0))->stride = (uint32_t)U64(1); }
NV(nvnVertexStreamStateSetDivisor) { ST(nvn_vs_st, P(0))->divisor = U32(1); }

/* ---- submit ---- */
/* deferred binds read the game's handle arrays now (it fills them for the next frame as soon as the submit returns) */
static void resolve_deferred(uint8_t *p) {
    for (;;) {
        nvn_op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= NOP_COUNT_) die("corrupt recording at submit (op %u size %u)", h.op, h.size);
        if (h.op == NOP_END) return;
        if (h.op == NOP_BIND_TEX || h.op == NOP_BIND_IMG || h.op == NOP_BIND_SAMP) {
            nvn_p_bind_handles *b = (nvn_p_bind_handles *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) memcpy(b->h, gw(b->src, 8 * (size_t)b->n), 8 * (size_t)b->n);
        }
        if (h.op == NOP_BIND_BUFS) {
            nvn_p_bind_bufs *b = (nvn_p_bind_bufs *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 16) memcpy(b->r, gw(b->src, sizeof(NVNbufferRange) * (size_t)b->n), sizeof(NVNbufferRange) * (size_t)b->n);
        }
        p += h.size;
    }
}
NV(nvnQueueSubmitCommands) {
    { void hos_renderer_wait(void); static volatile LONG up; if (!up) { hos_renderer_wait(); up = 1; } }   /* (display.c) */
    nvn_queue *q = (nvn_queue *)P(0);
    const int n = I(1);
    uint64_t h[64];
    if (n < 0 || n > 64) die("submit of %d handles", n);
    memcpy(h, PN(2, 8 * (size_t)n), 8 * (size_t)n);
    q->submits++;
    {   /* (which queues submit, 60 lines then every 20000: the game's queues are replayed on one, in arrival order) */
        static volatile LONG k;
        const LONG i = InterlockedIncrement(&k);
        if (i <= 60 || i % 20000 == 0)
            hos_log("nvn: QueueSubmitCommands #%ld: queue %p (its #%llu), %d handles (lr %#llx)", (long)i, (void *)q,
                    (unsigned long long)q->submits, n, (unsigned long long)c->x[30]);
    }
    for (int i = 0; i < n; i++) {
        check_handle(h[i]);
        resolve_deferred((uint8_t *)(uintptr_t)h[i]);
    }
    held_init();
    if (!hold_on) {
        /* (the replay runs here, on this thread, for ~9 ms a frame (vk_replay.cpp submit_q, RX_VK_ASYNC off): the virtual
           core is given up for it, as Present and AcquireTexture do, so the game's other threads run on it meanwhile.
           Nothing in the replay runs guest code or needs the core) */
        const int away = hos_vcore_away();
        rx_nvn_submit_q(h, n, (uint64_t)(uintptr_t)q);
        if (away) hos_vcore_back();
        return;
    }
    EnterCriticalSection(&held_cs);
    if (nheld == (LONG)(sizeof held / sizeof *held)) {   /* (full: what is held goes now) */
        for (LONG i = 0; i < nheld; i++) rx_nvn_submit(held[i].h, held[i].n);
        nheld = 0;
    }
    held[nheld].n = n;
    memcpy(held[nheld].h, h, 8 * (size_t)n);
    nheld++;
    LeaveCriticalSection(&held_cs);
}

/* ---- window ---- */
#define WB(p) ((nvn_win_builder *)(p))
/* (hookshot) a window texture: a guest address, checked at each use (the window object is guest memory) */
static const nvn_tex *win_tex(const nvn_win *o, int i) {
    if (i < 0 || i >= 4 || !o->tex[i]) die("window texture %d", i);
    return (const nvn_tex *)gw(o->tex[i], sizeof(nvn_tex));
}
void hos_vi_native_acquire(int active);
int hos_vi_present_trace_enabled(void);
void hos_vi_native_presented(const int64_t phases[4]);
NV(nvnWindowBuilderSetDefaults) {
    nvn_win_builder *b = WB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->interval = 1;
}
NV(nvnWindowBuilderSetDevice) { WB(P(0))->dev = P(1); }
NV(nvnWindowBuilderSetNativeWindow) { WB(P(0))->native = U64(1); }
NV(nvnWindowBuilderSetTextures) {
    nvn_win_builder *b = WB(P(0));
    int n = I(1);
    if (n < 0 || n > 4) die("window with %d textures", n);
    b->ntex = n;
    memcpy(b->tex, PN(2, 8 * (size_t)n), 8 * (size_t)n);
}
NV(nvnWindowBuilderSetNumActiveTextures) { if (I(1) < 2 || I(1) > 4) die("%d active window textures", I(1)); WB(P(0))->active = I(1); }
NV(nvnWindowBuilderSetPresentInterval) { WB(P(0))->interval = I(1); }
NV(nvnWindowBuilderGetNativeWindow) { RET(WB(P(0))->native); }
NV(nvnWindowBuilderGetPresentInterval) { RET((uint32_t)WB(P(0))->interval); }
NV(nvnWindowInitialize) {
    const nvn_win_builder *b = WB(P(1));
    nvn_win *o = (nvn_win *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('W', 'I', 'N', 'D');
    o->ntex = b->ntex;
    o->interval = b->interval;
    o->active = b->active ? b->active : b->ntex;
    if (o->ntex < 2 || o->ntex > 4 || o->active < 2 || o->active > o->ntex) { RET(0); return; }
    for (int i = 0; i < o->ntex; i++) o->tex[i] = b->tex[i];
    const nvn_tex *t0 = win_tex(o, 0);
    hos_log("nvn: window of %d textures (%d active), %dx%d format %#x", o->ntex, o->active, (int)t0->d.width,
            (int)t0->d.height, t0->d.format);
    RET(1);
}
NV(nvnWindowFinalize) {}
NV(nvnWindowSetDebugLabel) {}
NV(nvnWindowSetNumActiveTextures) { nvn_win *o = (nvn_win *)P(0); if (I(1) < 2 || I(1) > o->ntex) die("%d active window textures", I(1)); o->active = I(1); }
NV(nvnWindowGetNumActiveTextures) { const nvn_win *o = (const nvn_win *)P(0); RET((uint32_t)(o->active ? o->active : o->ntex)); }
NV(nvnWindowGetPresentInterval) { RET((uint32_t)((const nvn_win *)P(0))->interval); }
/* the game picks its interval from its frame time; in real-time mode (nn_vi.c) frames are paced by our limiter */
NV(nvnWindowSetPresentInterval) {
    nvn_win *o = (nvn_win *)P(0);
    {   /* (OoT changes it several times a second: logged with a backtrace only the first times, it was a syscall-heavy cost) */
        static int told;
        if (o->interval != I(1) && told++ < 6) hos_log("nvn: the game sets present interval %d -> %d", o->interval, I(1)), hos_backtrace();
    }
    extern volatile uint64_t hos_mkw_pacer;   /* (nn_vi.c: its frame pacer, which calls this from 0x806bce24) */
    if (hos_game_is_mkw() && c->x[30] == 0x806bce28) hos_mkw_pacer = c->x[19];
    int hos_vi_native_interval(Ctx *c, int requested);
    o->interval = hos_vi_native_interval(c, I(1));
}
NV(nvnWindowSetCrop) {
    nvn_win *o = (nvn_win *)P(0);
    const int v[4] = {I(1), I(2), I(3), I(4)};
    if (memcmp(o->crop, v, sizeof v)) hos_log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
    memcpy(o->crop, v, sizeof v);
}
NV(nvnWindowGetCrop) {
    const nvn_win *o = (const nvn_win *)P(0);
    NVNrectangle *r = (NVNrectangle *)P(1);
    if (!o->crop[2] && !o->crop[3] && o->ntex) {
        const nvn_tex *t0 = win_tex(o, 0);
        r->x = r->y = 0, r->width = (int)t0->d.width, r->height = (int)t0->d.height;
        return;
    }
    r->x = o->crop[0], r->y = o->crop[1], r->width = o->crop[2], r->height = o->crop[3];
}
/* frames are paced here (the console blocks here when every window texture waits for display), not in the present:
   the game presents with locks held */
NV(nvnWindowAcquireTexture) {
    nvn_win *o = (nvn_win *)P(0);
    nvn_sync *s = (nvn_sync *)P(1);
    *(int *)P(2) = o->active ? o->next++ % o->active : 0;
    {
        const int away = hos_vcore_away();
        hos_vi_native_acquire(o->active ? o->active : o->ntex);
        if (away) hos_vcore_back();
    }
    /* the acquire sync: this texture is no longer read by the display path (presents copy it in GPU order) */
    if (s) s->fenced = 3, s->fence = 0;
    RET(NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
}
NV(nvnQueuePresentTexture) {
    nvn_win *o = (nvn_win *)P(1);
    const int i = I(2);
    if (i < 0 || i >= o->ntex) die("present of window texture %d", i);
    HOS_CHAIN(HOS_CHAIN_PRESENT, (uint64_t)i);
    const int trace = hos_vi_present_trace_enabled();
    int64_t phases[4] = {0};
    if (trace) phases[0] = hos_now_ns();
    held_flush();   /* (a present flushes the queue) */
    {
        const int away = hos_vcore_away();
        if (trace) phases[1] = hos_now_ns();
        rx_nvn_present(win_tex(o, i), o->crop);
        if (trace) phases[2] = hos_now_ns();
        if (away) hos_vcore_back();
        if (trace) phases[3] = hos_now_ns();
    }
    o->presents++;
    extern volatile LONG64 hos_present_count;   /* display.c */
    InterlockedIncrement64(&hos_present_count);
    hos_vi_native_presented(phases);
}

/* ---- NVN2 (mkw-recomp): what the Switch 2's NVN adds, as MKW uses it ---- */

/* program builders: {device, stage count, NVNshaderData* (16-byte entries)}; the program takes the stages at
   InitializeFromBuilder */
typedef struct { void *dev; int32_t n, pad; uint64_t data; } nvn_prog_builder;
NV(nvnProgramBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_prog_builder)); }
NV(nvnProgramBuilderSetDevice) { ((nvn_prog_builder *)P(0))->dev = P(1); }
NV(nvnProgramBuilderSetShaders) { ((nvn_prog_builder *)P(0))->n = I(1); ((nvn_prog_builder *)P(0))->data = U64(2); }
NV(nvnProgramInitializeFromBuilder) {
    const nvn_prog_builder *b = (const nvn_prog_builder *)P(1);
    nvn_prog *o = (nvn_prog *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    RET(b->data ? set_shaders(o, b->n, (const uint8_t *)gw(b->data, b->n > 0 && b->n <= 6 ? 16ull * (uint32_t)b->n : 0), 16) : 1);
}

/* command buffer modes the replay has no use for */
NV(nvnCommandBufferSetRecordingMode) {}
NV(nvnCommandBufferSetComputeOverlapMode) {}

/* Native images: the graphics system's display buffers (nn::gs), which the game's window textures are made from.
   The graphics system is not emulated (nn_gs.c: no handles), so a native image is a linear texture of the display's
   size in memory of its own, NVN_WINDOW_W x NVN_WINDOW_H; the replay presents it like any window texture. Builder:
   {device, format, permission, handle}. */
#define NVN_WINDOW_W 1920
#define NVN_WINDOW_H 1080
typedef struct { void *dev; int32_t format, permission; uint64_t handle; } nvn_native_image_builder;
typedef struct { uint32_t magic; int32_t format, width, height; uint64_t storage, size; } nvn_native_image;
NV(nvnNativeImageBuilderSetDefaults) {
    nvn_native_image_builder *b = (nvn_native_image_builder *)P(0);
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->format = NVN_FORMAT_RGBA8;
}
NV(nvnNativeImageBuilderSetDevice) { ((nvn_native_image_builder *)P(0))->dev = P(1); }
NV(nvnNativeImageBuilderSetFormat) { ((nvn_native_image_builder *)P(0))->format = I(1); }
NV(nvnNativeImageBuilderSetMemoryPermission) { ((nvn_native_image_builder *)P(0))->permission = I(1); }
NV(nvnNativeImageBuilderSetNativeImageHandle) { ((nvn_native_image_builder *)P(0))->handle = U64(1); }
NV(nvnNativeImageBuilderGetDevice) { RETP(((const nvn_native_image_builder *)P(0))->dev); }
NV(nvnNativeImageBuilderGetFormat) { RET((uint32_t)((const nvn_native_image_builder *)P(0))->format); }
NV(nvnNativeImageBuilderGetMemoryPermission) { RET((uint32_t)((const nvn_native_image_builder *)P(0))->permission); }
NV(nvnNativeImageBuilderGetNativeImageHandle) { RET(((const nvn_native_image_builder *)P(0))->handle); }
NV(nvnNativeImageInitialize) {
    const nvn_native_image_builder *b = (const nvn_native_image_builder *)P(1);
    nvn_native_image *o = (nvn_native_image *)P(0);
    uint64_t hos_region_alloc(uint64_t size, uint64_t align);   /* hos_mem.c */
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('N', 'I', 'M', 'G');
    o->format = b->format, o->width = NVN_WINDOW_W, o->height = NVN_WINDOW_H;
    o->size = (uint64_t)NVN_WINDOW_W * NVN_WINDOW_H * 4;
    o->storage = hos_region_alloc(o->size, 0x10000);
    if (o->storage) hos_gpu_track(o->storage, o->size, 1);
    hos_log("nvn: native image %dx%d format %#x (handle %#llx) at %#llx", o->width, o->height, o->format,
            (unsigned long long)b->handle, (unsigned long long)o->storage);
    RET(o->storage != 0);
}
NV(nvnNativeImageFinalize) { ((nvn_native_image *)P(0))->magic = 0; }
/* nvnTextureInitializeFromNativeImage(NVNtexture*, NVNnativeImage*): a pitch-linear 2D render target over it */
NV(nvnTextureInitializeFromNativeImage) {
    const nvn_native_image *img = (const nvn_native_image *)P(1);
    nvn_tex *o = TX(P(0));
    if (o->magic == NVN_MAGIC('T', 'E', 'X', 0) && o->serial) {
        live_del(o);
        rx_nvn_texture_released(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('T', 'E', 'X', 0);
    o->d.target = NVN_TEXTURE_TARGET_2D;
    o->d.format = img->format;
    o->d.flags = NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET;
    o->d.width = (uint32_t)img->width, o->d.height = (uint32_t)img->height, o->d.depth = o->d.levels = 1;
    o->d.stride = (uint32_t)img->width * 4;
    o->d.swizzle_r = 2, o->d.swizzle_g = 3, o->d.swizzle_b = 4, o->d.swizzle_a = 5;
    nvn_tex_layout(&o->d, &o->L);
    o->gpu = img->storage;
    o->serial = new_serial();
    live_put(o);
    rx_nvn_texture_created(o);
    RET(img->magic == NVN_MAGIC('N', 'I', 'M', 'G'));
}
/* nvnSyncInitializeFromNativeSyncHandle(NVNsync*, NVNdevice*, nn::gs::NativeSyncHandle): signaled */
NV(nvnSyncInitializeFromNativeSyncHandle) {
    nvn_sync *s = (nvn_sync *)P(0);
    s->magic = NVN_MAGIC('S', 'Y', 'N', 'C'), s->fenced = 3, s->fence = 0;
    RET(1);
}

/* nvnTextureGetSparseTileLayout(NVNtexture const*, NVNtextureSparseTileLayout* {int numTiledLevels, tileWidth,
   tileHeight, tileDepth, reserved[4]}): 64 KiB tiles, shaped by the format's bytes per block (128 x 128 texels at
   4 bytes); the levels at least one tile in size are tiled, the rest live in the mip tail. With nothing written the
   game sized its tile table 0 and read through a null table. */
NV(nvnTextureGetSparseTileLayout) {
    const nvn_tex *t = CTX(P(0));
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    int tw = 128, th = 128;   /* (in blocks: 64 KiB / bpb, as square as a power of two allows) */
    switch (bpb) {
    case 1: tw = 256, th = 256; break;
    case 2: tw = 256, th = 128; break;
    case 8: tw = 128, th = 64; break;
    case 16: tw = 64, th = 64; break;
    }
    tw *= bw, th *= bh;
    int levels = 0;
    for (uint32_t l = 0, w = t->d.width, h = t->d.height; l < t->d.levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        if (w >= (uint32_t)tw && h >= (uint32_t)th) levels++;
    int32_t *out = (int32_t *)P(1);
    memset(out, 0, 32);
    out[0] = levels, out[1] = tw, out[2] = th, out[3] = 1;
}


/* queue and command buffer settings and debug annotations with no effect on the replay */
NV(nvnQueueBuilderSetPrimaryQueue) {}
NV(nvnQueueBuilderSetQueuePriority) {}
NV(nvnCommandBufferSetGraphicsWorkloadPriority) {}
NV(nvnCommandBufferSetComputeWorkloadPriority) {}
NV(nvnCommandBufferInsertDebugMarkerGraphics) {}
NV(nvnCommandBufferInsertDebugMarkerCompute) {}
NV(nvnCommandBufferPushDebugGroupGraphics) {}
NV(nvnCommandBufferPushDebugGroupCompute) {}
NV(nvnCommandBufferPopDebugGroupGraphics) {}
NV(nvnCommandBufferPopDebugGroupCompute) {}


NV(nvnTextureGetMemoryPool) { RETP(CTX(P(0))->pool); }
NV(nvnTextureBuilderGetMemoryPool) { RETP(CTB(P(0))->pool); }
NV(nvnBufferBuilderGetMemoryPool) { RETP(((const nvn_buffer_builder *)P(0))->pool); }
NV(nvnBufferBuilderGetMemoryOffset) { RET((uint64_t)((const nvn_buffer_builder *)P(0))->off); }
NV(nvnBufferBuilderGetSize) { RET(((const nvn_buffer_builder *)P(0))->size); }
NV(nvnTexturePoolGetMemoryPool) { RETP(((const nvn_dpool *)P(0))->pool); }
NV(nvnTexturePoolGetMemoryOffset) { RET((uint64_t)((const nvn_dpool *)P(0))->off); }
NV(nvnTexturePoolGetSize) { RET((uint32_t)((const nvn_dpool *)P(0))->n); }
NV(nvnSamplerPoolGetMemoryPool) { RETP(((const nvn_dpool *)P(0))->pool); }
NV(nvnSamplerPoolGetMemoryOffset) { RET((uint64_t)((const nvn_dpool *)P(0))->off); }
NV(nvnSamplerPoolGetSize) { RET((uint32_t)((const nvn_dpool *)P(0))->n); }
NV(nvnWindowGetNumTextures) { RET((uint32_t)((const nvn_win *)P(0))->ntex); }


/* GPU-driven draws (NVN2): the arguments are read at replay (rx_nvn.cpp), after the GPU's writes reached memory */
static void draw_indirect(Ctx *c, int indexed, int prim, int itype, uint64_t ibuf, uint64_t indirect, uint64_t count_addr,
                          int max, int stride) {
    nvn_p_draw_indirect *r = REC(CB(P(0)), NOP_DRAW_INDIRECT, nvn_p_draw_indirect);
    r->indexed = indexed, r->prim = prim, r->itype = itype, r->max = max, r->stride = stride;
    r->ibuf = ibuf, r->indirect = indirect, r->count_addr = count_addr;
}
/* DrawArraysIndirect(cmd, prim, indirect); DrawElementsIndirect(cmd, prim, index type, index buffer, indirect) */
NV(nvnCommandBufferDrawArraysIndirect) { draw_indirect(c, 0, I(1), 0, 0, U64(2), 0, 1, 16); }
NV(nvnCommandBufferDrawElementsIndirect) { draw_indirect(c, 1, I(1), I(2), U64(3), U64(4), 0, 1, 20); }
/* MultiDraw*IndirectCount(cmd, prim, [index type, index buffer,] indirect, count address, max draws, stride) */
NV(nvnCommandBufferMultiDrawArraysIndirectCount) { draw_indirect(c, 0, I(1), 0, 0, U64(2), U64(3), I(4), I(5)); }
NV(nvnCommandBufferMultiDrawElementsIndirectCount) { draw_indirect(c, 1, I(1), I(2), U64(3), U64(4), U64(5), I(6), I(7)); }
NV(nvnCommandBufferDispatchComputeIndirect) { REC(CB(P(0)), NOP_DISPATCH_INDIRECT, nvn_p_dispatch_indirect)->addr = U64(1); }
/* mesh shading: recorded for the Vulkan replay (VK_EXT_mesh_shader); the WebGPU one has none and leaves it out */
static void draw_mesh(Ctx *c, int kind) {
    nvn_p_draw_mesh *r = REC(CB(P(0)), NOP_DRAW_MESH, nvn_p_draw_mesh);
    memset(r, 0, sizeof *r);
    r->kind = kind;
    if (kind == 0) r->x = I(1), r->y = I(2), r->z = I(3);
    else if (kind == 1) r->indirect = U64(1), r->max = 1;
    else r->indirect = U64(1), r->count_addr = U64(2), r->max = I(3), r->stride = I(4);
    static volatile LONG64 n;
    const LONG64 k = InterlockedIncrement64(&n);
    if (k <= 3 || k % 20000 == 0)
        hos_log("nvn: DrawMeshTasks%s: %lld so far (x1 %#llx x2 %#llx x3 %#llx x4 %#llx)",
                kind == 0 ? "" : kind == 1 ? "Indirect" : " (multi, indirect count)", (long long)k,
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                (unsigned long long)c->x[4]);
}
NV(nvnCommandBufferDrawMeshTasks) { draw_mesh(c, 0); }
NV(nvnCommandBufferDrawMeshTasksIndirect) { draw_mesh(c, 1); }
NV(nvnCommandBufferMultiDrawMeshTasksIndirectCount) { draw_mesh(c, 2); }
/* state the replay does without: variable rate shading, the geometry pipeline's mode, stencil culling */
NV(nvnCommandBufferBindShadingRateImage) {}
NV(nvnCommandBufferShadingRateImagePalette) {}
NV(nvnCommandBufferSetGeometryProcessingMode) {}
NV(nvnCommandBufferSetStencilCullCriteria) {}


/* images share the texture pool's ids: the replay finds an image's texture as it finds a texture's */
NV(nvnTexturePoolRegisterImage) { rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3)); }

/* ---- the procs ---- */
typedef struct { const char *name; void (*fn)(Ctx *, void *); } nvn_impl;
static const nvn_impl impls[] = {
/* table: every NV() above (regenerated from the NV() lines of this file) */
    {"nvnDeviceBuilderSetDefaults", n_nvnDeviceBuilderSetDefaults},
    {"nvnDeviceBuilderSetFlags", n_nvnDeviceBuilderSetFlags},
    {"nvnDeviceInitialize", n_nvnDeviceInitialize},
    {"nvnDeviceFinalize", n_nvnDeviceFinalize},
    {"nvnDeviceGetProcAddress", n_nvnDeviceGetProcAddress},
    {"nvnDeviceGetInteger", n_nvnDeviceGetInteger},
    {"nvnDeviceRegisterFastClearColor", n_nvnDeviceRegisterFastClearColor},
    {"nvnDeviceRegisterFastClearColori", n_nvnDeviceRegisterFastClearColori},
    {"nvnDeviceRegisterFastClearColorui", n_nvnDeviceRegisterFastClearColorui},
    {"nvnDeviceRegisterFastClearDepth", n_nvnDeviceRegisterFastClearDepth},
    {"nvnDeviceGetImageHandle", n_nvnDeviceGetImageHandle},
    {"nvnDeviceGetTextureHandle", n_nvnDeviceGetTextureHandle},
    {"nvnDeviceGetTexelFetchHandle", n_nvnDeviceGetTexelFetchHandle},
    {"nvnDeviceGetSeparateTextureHandle", n_nvnDeviceGetSeparateTextureHandle},
    {"nvnDeviceGetSeparateSamplerHandle", n_nvnDeviceGetSeparateSamplerHandle},
    {"nvnDeviceInstallDebugCallback", n_nvnDeviceInstallDebugCallback},
    {"nvnDeviceSetWindowOriginMode", n_nvnDeviceSetWindowOriginMode},
    {"nvnDeviceSetDepthMode", n_nvnDeviceSetDepthMode},
    {"nvnDeviceFinalizeCommandHandle", n_nvnDeviceFinalizeCommandHandle},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", n_nvnDeviceGetCurrentTimestampInNanoseconds},
    {"nvnDeviceGetTimestampInNanoseconds", n_nvnDeviceGetTimestampInNanoseconds},
    {"nvnDeviceSetDebugLabel", n_nvnDeviceSetDebugLabel},
    {"nvnDeviceIsExternalDebuggerAttached", n_nvnDeviceIsExternalDebuggerAttached},
    {"nvnDeviceSetIntermediateShaderCache", n_nvnDeviceSetIntermediateShaderCache},
    {"nvnMemoryPoolBuilderSetDefaults", n_nvnMemoryPoolBuilderSetDefaults},
    {"nvnMemoryPoolBuilderSetDevice", n_nvnMemoryPoolBuilderSetDevice},
    {"nvnMemoryPoolBuilderSetStorage", n_nvnMemoryPoolBuilderSetStorage},
    {"nvnMemoryPoolBuilderSetFlags", n_nvnMemoryPoolBuilderSetFlags},
    {"nvnMemoryPoolInitialize", n_nvnMemoryPoolInitialize},
    {"nvnMemoryPoolFinalize", n_nvnMemoryPoolFinalize},
    {"nvnMemoryPoolGetFlags", n_nvnMemoryPoolGetFlags},
    {"nvnMemoryPoolGetSize", n_nvnMemoryPoolGetSize},
    {"nvnMemoryPoolMap", n_nvnMemoryPoolMap},
    {"nvnMemoryPoolGetBufferAddress", n_nvnMemoryPoolGetBufferAddress},
    {"nvnMemoryPoolFlushMappedRange", n_nvnMemoryPoolFlushMappedRange},
    {"nvnMemoryPoolInvalidateMappedRange", n_nvnMemoryPoolInvalidateMappedRange},
    {"nvnMemoryPoolSetDebugLabel", n_nvnMemoryPoolSetDebugLabel},
    {"nvnMemoryPoolMapVirtual", n_nvnMemoryPoolMapVirtual},
    {"nvnBufferBuilderSetDevice", n_nvnBufferBuilderSetDevice},
    {"nvnBufferBuilderSetDefaults", n_nvnBufferBuilderSetDefaults},
    {"nvnBufferBuilderSetStorage", n_nvnBufferBuilderSetStorage},
    {"nvnBufferInitialize", n_nvnBufferInitialize},
    {"nvnBufferFinalize", n_nvnBufferFinalize},
    {"nvnBufferMap", n_nvnBufferMap},
    {"nvnBufferGetAddress", n_nvnBufferGetAddress},
    {"nvnBufferGetMemoryPool", n_nvnBufferGetMemoryPool},
    {"nvnBufferGetMemoryOffset", n_nvnBufferGetMemoryOffset},
    {"nvnBufferGetSize", n_nvnBufferGetSize},
    {"nvnBufferFlushMappedRange", n_nvnBufferFlushMappedRange},
    {"nvnBufferInvalidateMappedRange", n_nvnBufferInvalidateMappedRange},
    {"nvnBufferSetDebugLabel", n_nvnBufferSetDebugLabel},
    {"nvnTexturePoolInitialize", n_nvnTexturePoolInitialize},
    {"nvnTexturePoolFinalize", n_nvnTexturePoolFinalize},
    {"nvnTexturePoolSetDebugLabel", n_nvnTexturePoolSetDebugLabel},
    {"nvnTexturePoolRegisterTexture", n_nvnTexturePoolRegisterTexture},
    {"nvnSamplerPoolInitialize", n_nvnSamplerPoolInitialize},
    {"nvnSamplerPoolFinalize", n_nvnSamplerPoolFinalize},
    {"nvnSamplerPoolSetDebugLabel", n_nvnSamplerPoolSetDebugLabel},
    {"nvnSamplerPoolRegisterSampler", n_nvnSamplerPoolRegisterSampler},
    {"nvnTextureBuilderSetDefaults", n_nvnTextureBuilderSetDefaults},
    {"nvnTextureBuilderSetDevice", n_nvnTextureBuilderSetDevice},
    {"nvnTextureBuilderSetFlags", n_nvnTextureBuilderSetFlags},
    {"nvnTextureBuilderSetTarget", n_nvnTextureBuilderSetTarget},
    {"nvnTextureBuilderSetWidth", n_nvnTextureBuilderSetWidth},
    {"nvnTextureBuilderSetHeight", n_nvnTextureBuilderSetHeight},
    {"nvnTextureBuilderSetDepth", n_nvnTextureBuilderSetDepth},
    {"nvnTextureBuilderSetSize1D", n_nvnTextureBuilderSetSize1D},
    {"nvnTextureBuilderSetSize2D", n_nvnTextureBuilderSetSize2D},
    {"nvnTextureBuilderSetSize3D", n_nvnTextureBuilderSetSize3D},
    {"nvnTextureBuilderSetLevels", n_nvnTextureBuilderSetLevels},
    {"nvnTextureBuilderSetFormat", n_nvnTextureBuilderSetFormat},
    {"nvnTextureBuilderSetDepthStencilMode", n_nvnTextureBuilderSetDepthStencilMode},
    {"nvnTextureBuilderSetSwizzle", n_nvnTextureBuilderSetSwizzle},
    {"nvnTextureBuilderSetSamples", n_nvnTextureBuilderSetSamples},
    {"nvnTextureBuilderSetStride", n_nvnTextureBuilderSetStride},
    {"nvnTextureBuilderSetStorage", n_nvnTextureBuilderSetStorage},
    {"nvnTextureBuilderSetPackagedTextureLayout", n_nvnTextureBuilderSetPackagedTextureLayout},
    {"nvnTextureBuilderSetPackagedTextureData", n_nvnTextureBuilderSetPackagedTextureData},
    {"nvnTextureBuilderGetPackagedTextureData", n_nvnTextureBuilderGetPackagedTextureData},
    {"nvnTextureBuilderGetStorageClass", n_nvnTextureBuilderGetStorageClass},
    {"nvnTextureBuilderGetFlags", n_nvnTextureBuilderGetFlags},
    {"nvnTextureBuilderGetStorageSize", n_nvnTextureBuilderGetStorageSize},
    {"nvnTextureBuilderGetStorageAlignment", n_nvnTextureBuilderGetStorageAlignment},
    {"nvnTextureBuilderGetZCullStorageSize", n_nvnTextureBuilderGetZCullStorageSize},
    {"nvnTextureBuilderGetTarget", n_nvnTextureBuilderGetTarget},
    {"nvnTextureBuilderGetWidth", n_nvnTextureBuilderGetWidth},
    {"nvnTextureBuilderGetHeight", n_nvnTextureBuilderGetHeight},
    {"nvnTextureBuilderGetDepth", n_nvnTextureBuilderGetDepth},
    {"nvnTextureBuilderGetLevels", n_nvnTextureBuilderGetLevels},
    {"nvnTextureBuilderGetFormat", n_nvnTextureBuilderGetFormat},
    {"nvnTextureBuilderGetSamples", n_nvnTextureBuilderGetSamples},
    {"nvnTextureBuilderGetStride", n_nvnTextureBuilderGetStride},
    {"nvnTextureBuilderGetMemoryOffset", n_nvnTextureBuilderGetMemoryOffset},
    {"nvnTextureBuilderGetViewOffset", n_nvnTextureBuilderGetViewOffset},
    {"nvnTextureInitialize", n_nvnTextureInitialize},
    {"nvnTextureFinalize", n_nvnTextureFinalize},
    {"nvnTextureGetStorageClass", n_nvnTextureGetStorageClass},
    {"nvnTextureGetFlags", n_nvnTextureGetFlags},
    {"nvnTextureGetWidth", n_nvnTextureGetWidth},
    {"nvnTextureGetHeight", n_nvnTextureGetHeight},
    {"nvnTextureGetDepth", n_nvnTextureGetDepth},
    {"nvnTextureGetLevels", n_nvnTextureGetLevels},
    {"nvnTextureGetFormat", n_nvnTextureGetFormat},
    {"nvnTextureGetSamples", n_nvnTextureGetSamples},
    {"nvnTextureGetTarget", n_nvnTextureGetTarget},
    {"nvnTextureGetStride", n_nvnTextureGetStride},
    {"nvnTextureGetStorageSize", n_nvnTextureGetStorageSize},
    {"nvnTextureGetZCullStorageSize", n_nvnTextureGetZCullStorageSize},
    {"nvnTextureGetTextureAddress", n_nvnTextureGetTextureAddress},
    {"nvnTextureGetMemoryOffset", n_nvnTextureGetMemoryOffset},
    {"nvnTextureGetDebugID", n_nvnTextureGetDebugID},
    {"nvnTextureSetDebugLabel", n_nvnTextureSetDebugLabel},
    {"nvnTextureGetSwizzle", n_nvnTextureGetSwizzle},
    {"nvnTextureCompare", n_nvnTextureCompare},
    {"nvnTextureGetRawStorageClass", n_nvnTextureGetRawStorageClass},
    {"nvnTextureViewSetDefaults", n_nvnTextureViewSetDefaults},
    {"nvnTextureViewSetLevels", n_nvnTextureViewSetLevels},
    {"nvnTextureViewSetLayers", n_nvnTextureViewSetLayers},
    {"nvnTextureViewSetFormat", n_nvnTextureViewSetFormat},
    {"nvnTextureViewSetTarget", n_nvnTextureViewSetTarget},
    {"nvnTextureViewSetSwizzle", n_nvnTextureViewSetSwizzle},
    {"nvnTextureViewSetDepthStencilMode", n_nvnTextureViewSetDepthStencilMode},
    {"nvnTextureViewGetLevels", n_nvnTextureViewGetLevels},
    {"nvnTextureViewGetLayers", n_nvnTextureViewGetLayers},
    {"nvnTextureViewGetFormat", n_nvnTextureViewGetFormat},
    {"nvnTextureViewGetTarget", n_nvnTextureViewGetTarget},
    {"nvnTextureGetViewOffset", n_nvnTextureGetViewOffset},
    {"nvnTextureReadTexels", n_nvnTextureReadTexels},
    {"nvnTextureReadTexelsStrided", n_nvnTextureReadTexelsStrided},
    {"nvnTextureWriteTexels", n_nvnTextureWriteTexels},
    {"nvnTextureWriteTexelsStrided", n_nvnTextureWriteTexelsStrided},
    {"nvnTextureFlushTexels", n_nvnTextureFlushTexels},
    {"nvnTextureInvalidateTexels", n_nvnTextureInvalidateTexels},
    {"nvnSamplerBuilderSetDefaults", n_nvnSamplerBuilderSetDefaults},
    {"nvnSamplerBuilderSetDevice", n_nvnSamplerBuilderSetDevice},
    {"nvnSamplerBuilderSetMinMagFilter", n_nvnSamplerBuilderSetMinMagFilter},
    {"nvnSamplerBuilderSetWrapMode", n_nvnSamplerBuilderSetWrapMode},
    {"nvnSamplerBuilderSetLodClamp", n_nvnSamplerBuilderSetLodClamp},
    {"nvnSamplerBuilderSetLodBias", n_nvnSamplerBuilderSetLodBias},
    {"nvnSamplerBuilderSetCompare", n_nvnSamplerBuilderSetCompare},
    {"nvnSamplerBuilderSetBorderColor", n_nvnSamplerBuilderSetBorderColor},
    {"nvnSamplerBuilderSetBorderColori", n_nvnSamplerBuilderSetBorderColori},
    {"nvnSamplerBuilderSetBorderColorui", n_nvnSamplerBuilderSetBorderColorui},
    {"nvnSamplerBuilderSetMaxAnisotropy", n_nvnSamplerBuilderSetMaxAnisotropy},
    {"nvnSamplerBuilderSetLodSnap", n_nvnSamplerBuilderSetLodSnap},
    {"nvnSamplerBuilderSetReductionFilter", n_nvnSamplerBuilderSetReductionFilter},
    {"nvnSamplerBuilderGetMinMagFilter", n_nvnSamplerBuilderGetMinMagFilter},
    {"nvnSamplerBuilderGetWrapMode", n_nvnSamplerBuilderGetWrapMode},
    {"nvnSamplerBuilderGetReductionFilter", n_nvnSamplerBuilderGetReductionFilter},
    {"nvnSamplerInitialize", n_nvnSamplerInitialize},
    {"nvnSamplerFinalize", n_nvnSamplerFinalize},
    {"nvnSamplerSetDebugLabel", n_nvnSamplerSetDebugLabel},
    {"nvnSamplerGetMinMagFilter", n_nvnSamplerGetMinMagFilter},
    {"nvnSamplerGetWrapMode", n_nvnSamplerGetWrapMode},
    {"nvnSamplerGetLodClamp", n_nvnSamplerGetLodClamp},
    {"nvnSamplerGetLodBias", n_nvnSamplerGetLodBias},
    {"nvnSamplerGetCompare", n_nvnSamplerGetCompare},
    {"nvnSamplerGetBorderColor", n_nvnSamplerGetBorderColor},
    {"nvnSamplerGetMaxAnisotropy", n_nvnSamplerGetMaxAnisotropy},
    {"nvnSamplerGetReductionFilter", n_nvnSamplerGetReductionFilter},
    {"nvnSamplerGetDebugID", n_nvnSamplerGetDebugID},
    {"nvnProgramInitialize", n_nvnProgramInitialize},
    {"nvnProgramFinalize", n_nvnProgramFinalize},
    {"nvnProgramSetShaders", n_nvnProgramSetShaders},
    {"nvnProgramSetShadersExt", n_nvnProgramSetShadersExt},
    {"nvnProgramSetDebugLabel", n_nvnProgramSetDebugLabel},
    {"nvnSyncInitialize", n_nvnSyncInitialize},
    {"nvnSyncFinalize", n_nvnSyncFinalize},
    {"nvnSyncSetDebugLabel", n_nvnSyncSetDebugLabel},
    {"nvnQueueFenceSync", n_nvnQueueFenceSync},
    {"nvnSyncWait", n_nvnSyncWait},
    {"nvnEventBuilderSetDefaults", n_nvnEventBuilderSetDefaults},
    {"nvnEventBuilderSetStorage", n_nvnEventBuilderSetStorage},
    {"nvnEventInitialize", n_nvnEventInitialize},
    {"nvnEventFinalize", n_nvnEventFinalize},
    {"nvnEventGetValue", n_nvnEventGetValue},
    {"nvnEventSignal", n_nvnEventSignal},
    {"nvnQueueBuilderSetDefaults", n_nvnQueueBuilderSetDefaults},
    {"nvnQueueBuilderSetDevice", n_nvnQueueBuilderSetDevice},
    {"nvnQueueBuilderSetFlags", n_nvnQueueBuilderSetFlags},
    {"nvnQueueBuilderSetCommandMemorySize", n_nvnQueueBuilderSetCommandMemorySize},
    {"nvnQueueBuilderSetComputeMemorySize", n_nvnQueueBuilderSetComputeMemorySize},
    {"nvnQueueBuilderSetControlMemorySize", n_nvnQueueBuilderSetControlMemorySize},
    {"nvnQueueBuilderSetCommandFlushThreshold", n_nvnQueueBuilderSetCommandFlushThreshold},
    {"nvnQueueBuilderGetQueueMemorySize", n_nvnQueueBuilderGetQueueMemorySize},
    {"nvnQueueBuilderSetQueueMemory", n_nvnQueueBuilderSetQueueMemory},
    {"nvnQueueInitialize", n_nvnQueueInitialize},
    {"nvnQueueFinalize", n_nvnQueueFinalize},
    {"nvnQueueGetError", n_nvnQueueGetError},
    {"nvnQueueFlush", n_nvnQueueFlush},
    {"nvnQueueFinish", n_nvnQueueFinish},
    {"nvnQueueSetDebugLabel", n_nvnQueueSetDebugLabel},
    {"nvnCommandBufferInitialize", n_nvnCommandBufferInitialize},
    {"nvnCommandBufferFinalize", n_nvnCommandBufferFinalize},
    {"nvnCommandBufferSetMemoryCallback", n_nvnCommandBufferSetMemoryCallback},
    {"nvnCommandBufferSetMemoryCallbackData", n_nvnCommandBufferSetMemoryCallbackData},
    {"nvnCommandBufferGetMemoryCallback", n_nvnCommandBufferGetMemoryCallback},
    {"nvnCommandBufferGetMemoryCallbackData", n_nvnCommandBufferGetMemoryCallbackData},
    {"nvnCommandBufferSetCommandMemoryCallbackEnabled", n_nvnCommandBufferSetCommandMemoryCallbackEnabled},
    {"nvnCommandBufferSetControlMemoryCallbackEnabled", n_nvnCommandBufferSetControlMemoryCallbackEnabled},
    {"nvnCommandBufferSetDebugLabel", n_nvnCommandBufferSetDebugLabel},
    {"nvnCommandBufferAddCommandMemory", n_nvnCommandBufferAddCommandMemory},
    {"nvnCommandBufferAddControlMemory", n_nvnCommandBufferAddControlMemory},
    {"nvnCommandBufferGetControlMemorySize", n_nvnCommandBufferGetControlMemorySize},
    {"nvnCommandBufferGetControlMemoryUsed", n_nvnCommandBufferGetControlMemoryUsed},
    {"nvnCommandBufferGetControlMemoryFree", n_nvnCommandBufferGetControlMemoryFree},
    {"nvnCommandBufferGetCommandMemorySize", n_nvnCommandBufferGetCommandMemorySize},
    {"nvnCommandBufferGetCommandMemoryUsed", n_nvnCommandBufferGetCommandMemoryUsed},
    {"nvnCommandBufferGetCommandMemoryFree", n_nvnCommandBufferGetCommandMemoryFree},
    {"nvnCommandBufferBeginRecording", n_nvnCommandBufferBeginRecording},
    {"nvnCommandBufferEndRecording", n_nvnCommandBufferEndRecording},
    {"nvnCommandBufferIsRecording", n_nvnCommandBufferIsRecording},
    {"nvnCommandBufferCallCommands", n_nvnCommandBufferCallCommands},
    {"nvnCommandBufferCopyCommands", n_nvnCommandBufferCopyCommands},
    {"nvnCommandBufferSignalEvent", n_nvnCommandBufferSignalEvent},
    {"nvnCommandBufferWaitEvent", n_nvnCommandBufferWaitEvent},
    {"nvnCommandBufferWaitSync", n_nvnCommandBufferWaitSync},
    {"nvnQueueWaitSync", n_nvnQueueWaitSync},
    {"nvnCommandBufferPushComputeConstants", n_nvnCommandBufferPushComputeConstants},
    {"nvnCommandBufferResetCounter", n_nvnCommandBufferResetCounter},
    {"nvnCommandBufferReportCounter", n_nvnCommandBufferReportCounter},
    {"nvnCommandBufferCopyBufferToBuffer", n_nvnCommandBufferCopyBufferToBuffer},
    {"nvnCommandBufferClearBuffer", n_nvnCommandBufferClearBuffer},
    {"nvnCommandBufferUpdateUniformBuffer", n_nvnCommandBufferUpdateUniformBuffer},
    {"nvnCommandBufferSetRenderTargets", n_nvnCommandBufferSetRenderTargets},
    {"nvnCommandBufferClearColor", n_nvnCommandBufferClearColor},
    {"nvnCommandBufferClearDepthStencil", n_nvnCommandBufferClearDepthStencil},
    {"nvnCommandBufferSetViewports", n_nvnCommandBufferSetViewports},
    {"nvnCommandBufferSetViewport", n_nvnCommandBufferSetViewport},
    {"nvnCommandBufferSetScissor", n_nvnCommandBufferSetScissor},
    {"nvnCommandBufferSetScissors", n_nvnCommandBufferSetScissors},
    {"nvnCommandBufferSetDepthRange", n_nvnCommandBufferSetDepthRange},
    {"nvnCommandBufferSetDepthRanges", n_nvnCommandBufferSetDepthRanges},
    {"nvnCommandBufferSetDepthClamp", n_nvnCommandBufferSetDepthClamp},
    {"nvnCommandBufferSetRenderEnable", n_nvnCommandBufferSetRenderEnable},
    {"nvnCommandBufferSetRenderEnableConditional", n_nvnCommandBufferSetRenderEnableConditional},
    {"nvnCommandBufferSetRasterizerDiscard", n_nvnCommandBufferSetRasterizerDiscard},
    {"nvnCommandBufferBindProgram", n_nvnCommandBufferBindProgram},
    {"nvnCommandBufferBindBlendState", n_nvnCommandBufferBindBlendState},
    {"nvnCommandBufferBindColorState", n_nvnCommandBufferBindColorState},
    {"nvnCommandBufferBindChannelMaskState", n_nvnCommandBufferBindChannelMaskState},
    {"nvnCommandBufferBindMultisampleState", n_nvnCommandBufferBindMultisampleState},
    {"nvnCommandBufferBindPolygonState", n_nvnCommandBufferBindPolygonState},
    {"nvnCommandBufferBindDepthStencilState", n_nvnCommandBufferBindDepthStencilState},
    {"nvnCommandBufferBindVertexAttribState", n_nvnCommandBufferBindVertexAttribState},
    {"nvnCommandBufferBindVertexStreamState", n_nvnCommandBufferBindVertexStreamState},
    {"nvnCommandBufferBindVertexBuffers", n_nvnCommandBufferBindVertexBuffers},
    {"nvnCommandBufferBindVertexBuffer", n_nvnCommandBufferBindVertexBuffer},
    {"nvnCommandBufferBindUniformBuffer", n_nvnCommandBufferBindUniformBuffer},
    {"nvnCommandBufferBindStorageBuffer", n_nvnCommandBufferBindStorageBuffer},
    {"nvnCommandBufferBindUniformBuffers", n_nvnCommandBufferBindUniformBuffers},
    {"nvnCommandBufferBindStorageBuffers", n_nvnCommandBufferBindStorageBuffers},
    {"nvnCommandBufferBindUniformBuffersDeferred", n_nvnCommandBufferBindUniformBuffersDeferred},
    {"nvnCommandBufferBindStorageBuffersDeferred", n_nvnCommandBufferBindStorageBuffersDeferred},
    {"nvnCommandBufferBindTexture", n_nvnCommandBufferBindTexture},
    {"nvnCommandBufferBindTextures", n_nvnCommandBufferBindTextures},
    {"nvnCommandBufferBindImage", n_nvnCommandBufferBindImage},
    {"nvnCommandBufferBindImages", n_nvnCommandBufferBindImages},
    {"nvnCommandBufferBindSeparateTexture", n_nvnCommandBufferBindSeparateTexture},
    {"nvnCommandBufferBindSeparateTextures", n_nvnCommandBufferBindSeparateTextures},
    {"nvnCommandBufferBindSeparateSampler", n_nvnCommandBufferBindSeparateSampler},
    {"nvnCommandBufferBindSeparateSamplers", n_nvnCommandBufferBindSeparateSamplers},
    {"nvnCommandBufferBindTexturesDeferred", n_nvnCommandBufferBindTexturesDeferred},
    {"nvnCommandBufferBindImagesDeferred", n_nvnCommandBufferBindImagesDeferred},
    {"nvnCommandBufferBindSeparateTexturesDeferred", n_nvnCommandBufferBindSeparateTexturesDeferred},
    {"nvnCommandBufferBindSeparateSamplersDeferred", n_nvnCommandBufferBindSeparateSamplersDeferred},
    {"nvnCommandBufferSetStencilValueMask", n_nvnCommandBufferSetStencilValueMask},
    {"nvnCommandBufferSetStencilMask", n_nvnCommandBufferSetStencilMask},
    {"nvnCommandBufferSetStencilRef", n_nvnCommandBufferSetStencilRef},
    {"nvnCommandBufferSetPolygonOffsetClamp", n_nvnCommandBufferSetPolygonOffsetClamp},
    {"nvnCommandBufferSetPrimitiveRestart", n_nvnCommandBufferSetPrimitiveRestart},
    {"nvnCommandBufferDrawArrays", n_nvnCommandBufferDrawArrays},
    {"nvnCommandBufferDrawArraysInstanced", n_nvnCommandBufferDrawArraysInstanced},
    {"nvnCommandBufferDrawElements", n_nvnCommandBufferDrawElements},
    {"nvnCommandBufferDrawElementsBaseVertex", n_nvnCommandBufferDrawElementsBaseVertex},
    {"nvnCommandBufferDrawElementsInstanced", n_nvnCommandBufferDrawElementsInstanced},
    {"nvnCommandBufferDispatchCompute", n_nvnCommandBufferDispatchCompute},
    {"nvnCommandBufferBarrier", n_nvnCommandBufferBarrier},
    {"nvnCommandBufferSetTexturePool", n_nvnCommandBufferSetTexturePool},
    {"nvnCommandBufferSetSamplerPool", n_nvnCommandBufferSetSamplerPool},
    {"nvnCommandBufferSetShaderScratchMemory", n_nvnCommandBufferSetShaderScratchMemory},
    {"nvnCommandBufferCopyBufferToTexture", n_nvnCommandBufferCopyBufferToTexture},
    {"nvnCommandBufferCopyTextureToBuffer", n_nvnCommandBufferCopyTextureToBuffer},
    {"nvnCommandBufferCopyTextureToTexture", n_nvnCommandBufferCopyTextureToTexture},
    {"nvnCommandBufferClearTexture", n_nvnCommandBufferClearTexture},
    {"nvnCommandBufferClearTextureui", n_nvnCommandBufferClearTextureui},
    {"nvnCommandBufferSetCopyRowStride", n_nvnCommandBufferSetCopyRowStride},
    {"nvnCommandBufferSetCopyImageStride", n_nvnCommandBufferSetCopyImageStride},
    {"nvnCommandBufferSetBlendColor", n_nvnCommandBufferSetBlendColor},
    {"nvnCommandBufferSetLineWidth", n_nvnCommandBufferSetLineWidth},
    {"nvnCommandBufferSetPointSize", n_nvnCommandBufferSetPointSize},
    {"nvnCommandBufferSetDepthBounds", n_nvnCommandBufferSetDepthBounds},
    {"nvnCommandBufferSetTiledCacheAction", n_nvnCommandBufferSetTiledCacheAction},
    {"nvnCommandBufferSetTiledCacheTileSize", n_nvnCommandBufferSetTiledCacheTileSize},
    {"nvnCommandBufferSetSubpixelPrecisionBias", n_nvnCommandBufferSetSubpixelPrecisionBias},
    {"nvnCommandBufferDiscardColor", n_nvnCommandBufferDiscardColor},
    {"nvnCommandBufferDiscardDepthStencil", n_nvnCommandBufferDiscardDepthStencil},
    {"nvnCommandBufferSaveZCullData", n_nvnCommandBufferSaveZCullData},
    {"nvnCommandBufferRestoreZCullData", n_nvnCommandBufferRestoreZCullData},
    {"nvnCommandBufferSetAlphaRef", n_nvnCommandBufferSetAlphaRef},
    {"nvnCommandBufferSetSampleMask", n_nvnCommandBufferSetSampleMask},
    {"nvnCommandBufferSetConservativeRasterEnable", n_nvnCommandBufferSetConservativeRasterEnable},
    {"nvnCommandBufferSetConservativeRasterDilate", n_nvnCommandBufferSetConservativeRasterDilate},
    {"nvnCommandBufferSetPatchSize", n_nvnCommandBufferSetPatchSize},
    {"nvnCommandBufferPushDebugGroupStatic", n_nvnCommandBufferPushDebugGroupStatic},
    {"nvnCommandBufferPushDebugGroupDynamic", n_nvnCommandBufferPushDebugGroupDynamic},
    {"nvnCommandBufferPushDebugGroup", n_nvnCommandBufferPushDebugGroup},
    {"nvnCommandBufferPopDebugGroup", n_nvnCommandBufferPopDebugGroup},
    {"nvnCommandBufferPopDebugGroupId", n_nvnCommandBufferPopDebugGroupId},
    {"nvnCommandBufferInsertDebugMarkerStatic", n_nvnCommandBufferInsertDebugMarkerStatic},
    {"nvnCommandBufferInsertDebugMarkerDynamic", n_nvnCommandBufferInsertDebugMarkerDynamic},
    {"nvnCommandBufferInsertDebugMarker", n_nvnCommandBufferInsertDebugMarker},
    {"nvnBlendStateSetDefaults", n_nvnBlendStateSetDefaults},
    {"nvnBlendStateSetBlendTarget", n_nvnBlendStateSetBlendTarget},
    {"nvnBlendStateSetBlendFunc", n_nvnBlendStateSetBlendFunc},
    {"nvnBlendStateSetBlendEquation", n_nvnBlendStateSetBlendEquation},
    {"nvnBlendStateSetAdvancedMode", n_nvnBlendStateSetAdvancedMode},
    {"nvnBlendStateSetAdvancedOverlap", n_nvnBlendStateSetAdvancedOverlap},
    {"nvnBlendStateSetAdvancedPremultipliedSrc", n_nvnBlendStateSetAdvancedPremultipliedSrc},
    {"nvnBlendStateSetAdvancedNormalizedDst", n_nvnBlendStateSetAdvancedNormalizedDst},
    {"nvnColorStateSetDefaults", n_nvnColorStateSetDefaults},
    {"nvnColorStateSetBlendEnable", n_nvnColorStateSetBlendEnable},
    {"nvnColorStateSetLogicOp", n_nvnColorStateSetLogicOp},
    {"nvnColorStateSetAlphaTest", n_nvnColorStateSetAlphaTest},
    {"nvnChannelMaskStateSetDefaults", n_nvnChannelMaskStateSetDefaults},
    {"nvnChannelMaskStateSetChannelMask", n_nvnChannelMaskStateSetChannelMask},
    {"nvnMultisampleStateSetDefaults", n_nvnMultisampleStateSetDefaults},
    {"nvnMultisampleStateSetMultisampleEnable", n_nvnMultisampleStateSetMultisampleEnable},
    {"nvnMultisampleStateSetSamples", n_nvnMultisampleStateSetSamples},
    {"nvnMultisampleStateSetAlphaToCoverageEnable", n_nvnMultisampleStateSetAlphaToCoverageEnable},
    {"nvnMultisampleStateSetAlphaToCoverageDither", n_nvnMultisampleStateSetAlphaToCoverageDither},
    {"nvnMultisampleStateSetRasterSamples", n_nvnMultisampleStateSetRasterSamples},
    {"nvnMultisampleStateSetSampleLocationsEnable", n_nvnMultisampleStateSetSampleLocationsEnable},
    {"nvnMultisampleStateSetSampleLocations", n_nvnMultisampleStateSetSampleLocations},
    {"nvnPolygonStateSetDefaults", n_nvnPolygonStateSetDefaults},
    {"nvnPolygonStateSetCullFace", n_nvnPolygonStateSetCullFace},
    {"nvnPolygonStateSetFrontFace", n_nvnPolygonStateSetFrontFace},
    {"nvnPolygonStateSetPolygonMode", n_nvnPolygonStateSetPolygonMode},
    {"nvnPolygonStateSetPolygonOffsetEnables", n_nvnPolygonStateSetPolygonOffsetEnables},
    {"nvnDepthStencilStateSetDefaults", n_nvnDepthStencilStateSetDefaults},
    {"nvnDepthStencilStateSetDepthTestEnable", n_nvnDepthStencilStateSetDepthTestEnable},
    {"nvnDepthStencilStateSetDepthWriteEnable", n_nvnDepthStencilStateSetDepthWriteEnable},
    {"nvnDepthStencilStateSetDepthFunc", n_nvnDepthStencilStateSetDepthFunc},
    {"nvnDepthStencilStateSetStencilTestEnable", n_nvnDepthStencilStateSetStencilTestEnable},
    {"nvnDepthStencilStateSetStencilFunc", n_nvnDepthStencilStateSetStencilFunc},
    {"nvnDepthStencilStateSetStencilOp", n_nvnDepthStencilStateSetStencilOp},
    {"nvnVertexAttribStateSetDefaults", n_nvnVertexAttribStateSetDefaults},
    {"nvnVertexAttribStateSetFormat", n_nvnVertexAttribStateSetFormat},
    {"nvnVertexAttribStateSetStreamIndex", n_nvnVertexAttribStateSetStreamIndex},
    {"nvnVertexStreamStateSetDefaults", n_nvnVertexStreamStateSetDefaults},
    {"nvnVertexStreamStateSetStride", n_nvnVertexStreamStateSetStride},
    {"nvnVertexStreamStateSetDivisor", n_nvnVertexStreamStateSetDivisor},
    {"nvnQueueSubmitCommands", n_nvnQueueSubmitCommands},
    {"nvnWindowBuilderSetDefaults", n_nvnWindowBuilderSetDefaults},
    {"nvnWindowBuilderSetDevice", n_nvnWindowBuilderSetDevice},
    {"nvnWindowBuilderSetNativeWindow", n_nvnWindowBuilderSetNativeWindow},
    {"nvnWindowBuilderSetTextures", n_nvnWindowBuilderSetTextures},
    {"nvnWindowBuilderSetNumActiveTextures", n_nvnWindowBuilderSetNumActiveTextures},
    {"nvnWindowBuilderSetPresentInterval", n_nvnWindowBuilderSetPresentInterval},
    {"nvnWindowBuilderGetNativeWindow", n_nvnWindowBuilderGetNativeWindow},
    {"nvnWindowBuilderGetPresentInterval", n_nvnWindowBuilderGetPresentInterval},
    {"nvnWindowInitialize", n_nvnWindowInitialize},
    {"nvnWindowFinalize", n_nvnWindowFinalize},
    {"nvnWindowSetDebugLabel", n_nvnWindowSetDebugLabel},
    {"nvnWindowSetNumActiveTextures", n_nvnWindowSetNumActiveTextures},
    {"nvnWindowGetNumActiveTextures", n_nvnWindowGetNumActiveTextures},
    {"nvnWindowGetPresentInterval", n_nvnWindowGetPresentInterval},
    {"nvnWindowSetPresentInterval", n_nvnWindowSetPresentInterval},
    {"nvnWindowSetCrop", n_nvnWindowSetCrop},
    {"nvnWindowGetCrop", n_nvnWindowGetCrop},
    {"nvnWindowAcquireTexture", n_nvnWindowAcquireTexture},
    {"nvnQueuePresentTexture", n_nvnQueuePresentTexture},
    {"nvnProgramBuilderSetDefaults", n_nvnProgramBuilderSetDefaults},
    {"nvnProgramBuilderSetDevice", n_nvnProgramBuilderSetDevice},
    {"nvnProgramBuilderSetShaders", n_nvnProgramBuilderSetShaders},
    {"nvnProgramInitializeFromBuilder", n_nvnProgramInitializeFromBuilder},
    {"nvnCommandBufferSetRecordingMode", n_nvnCommandBufferSetRecordingMode},
    {"nvnCommandBufferSetComputeOverlapMode", n_nvnCommandBufferSetComputeOverlapMode},
    {"nvnNativeImageBuilderSetDefaults", n_nvnNativeImageBuilderSetDefaults},
    {"nvnNativeImageBuilderSetDevice", n_nvnNativeImageBuilderSetDevice},
    {"nvnNativeImageBuilderSetFormat", n_nvnNativeImageBuilderSetFormat},
    {"nvnNativeImageBuilderSetMemoryPermission", n_nvnNativeImageBuilderSetMemoryPermission},
    {"nvnNativeImageBuilderSetNativeImageHandle", n_nvnNativeImageBuilderSetNativeImageHandle},
    {"nvnNativeImageBuilderGetDevice", n_nvnNativeImageBuilderGetDevice},
    {"nvnNativeImageBuilderGetFormat", n_nvnNativeImageBuilderGetFormat},
    {"nvnNativeImageBuilderGetMemoryPermission", n_nvnNativeImageBuilderGetMemoryPermission},
    {"nvnNativeImageBuilderGetNativeImageHandle", n_nvnNativeImageBuilderGetNativeImageHandle},
    {"nvnNativeImageInitialize", n_nvnNativeImageInitialize},
    {"nvnNativeImageFinalize", n_nvnNativeImageFinalize},
    {"nvnTextureInitializeFromNativeImage", n_nvnTextureInitializeFromNativeImage},
    {"nvnSyncInitializeFromNativeSyncHandle", n_nvnSyncInitializeFromNativeSyncHandle},
    {"nvnTextureGetSparseTileLayout", n_nvnTextureGetSparseTileLayout},
    {"nvnQueueBuilderSetPrimaryQueue", n_nvnQueueBuilderSetPrimaryQueue},
    {"nvnQueueBuilderSetQueuePriority", n_nvnQueueBuilderSetQueuePriority},
    {"nvnCommandBufferSetGraphicsWorkloadPriority", n_nvnCommandBufferSetGraphicsWorkloadPriority},
    {"nvnCommandBufferSetComputeWorkloadPriority", n_nvnCommandBufferSetComputeWorkloadPriority},
    {"nvnCommandBufferInsertDebugMarkerGraphics", n_nvnCommandBufferInsertDebugMarkerGraphics},
    {"nvnCommandBufferInsertDebugMarkerCompute", n_nvnCommandBufferInsertDebugMarkerCompute},
    {"nvnCommandBufferPushDebugGroupGraphics", n_nvnCommandBufferPushDebugGroupGraphics},
    {"nvnCommandBufferPushDebugGroupCompute", n_nvnCommandBufferPushDebugGroupCompute},
    {"nvnCommandBufferPopDebugGroupGraphics", n_nvnCommandBufferPopDebugGroupGraphics},
    {"nvnCommandBufferPopDebugGroupCompute", n_nvnCommandBufferPopDebugGroupCompute},
    {"nvnTextureGetMemoryPool", n_nvnTextureGetMemoryPool},
    {"nvnTextureBuilderGetMemoryPool", n_nvnTextureBuilderGetMemoryPool},
    {"nvnBufferBuilderGetMemoryPool", n_nvnBufferBuilderGetMemoryPool},
    {"nvnBufferBuilderGetMemoryOffset", n_nvnBufferBuilderGetMemoryOffset},
    {"nvnBufferBuilderGetSize", n_nvnBufferBuilderGetSize},
    {"nvnTexturePoolGetMemoryPool", n_nvnTexturePoolGetMemoryPool},
    {"nvnTexturePoolGetMemoryOffset", n_nvnTexturePoolGetMemoryOffset},
    {"nvnTexturePoolGetSize", n_nvnTexturePoolGetSize},
    {"nvnSamplerPoolGetMemoryPool", n_nvnSamplerPoolGetMemoryPool},
    {"nvnSamplerPoolGetMemoryOffset", n_nvnSamplerPoolGetMemoryOffset},
    {"nvnSamplerPoolGetSize", n_nvnSamplerPoolGetSize},
    {"nvnWindowGetNumTextures", n_nvnWindowGetNumTextures},
    {"nvnCommandBufferDrawArraysIndirect", n_nvnCommandBufferDrawArraysIndirect},
    {"nvnCommandBufferDrawElementsIndirect", n_nvnCommandBufferDrawElementsIndirect},
    {"nvnCommandBufferMultiDrawArraysIndirectCount", n_nvnCommandBufferMultiDrawArraysIndirectCount},
    {"nvnCommandBufferMultiDrawElementsIndirectCount", n_nvnCommandBufferMultiDrawElementsIndirectCount},
    {"nvnCommandBufferDispatchComputeIndirect", n_nvnCommandBufferDispatchComputeIndirect},
    {"nvnCommandBufferDrawMeshTasks", n_nvnCommandBufferDrawMeshTasks},
    {"nvnCommandBufferDrawMeshTasksIndirect", n_nvnCommandBufferDrawMeshTasksIndirect},
    {"nvnCommandBufferMultiDrawMeshTasksIndirectCount", n_nvnCommandBufferMultiDrawMeshTasksIndirectCount},
    {"nvnCommandBufferBindShadingRateImage", n_nvnCommandBufferBindShadingRateImage},
    {"nvnCommandBufferShadingRateImagePalette", n_nvnCommandBufferShadingRateImagePalette},
    {"nvnCommandBufferSetGeometryProcessingMode", n_nvnCommandBufferSetGeometryProcessingMode},
    {"nvnCommandBufferSetStencilCullCriteria", n_nvnCommandBufferSetStencilCullCriteria},
    {"nvnTexturePoolRegisterImage", n_nvnTexturePoolRegisterImage},
/* end of table */
};

/* (mkw-recomp) A proc not implemented here: logged at its first call and answered 0 (initializers: 1, success),
   so the game goes on and the log lists what is left. HOS_NVN_STRICT=1: it stops the game, as in TOTKPort. */
static void unimplemented(Ctx *c, void *name) {
    static int strict = -1;
    if (strict < 0) strict = env_on("HOS_NVN_STRICT");
    if (strict)
        die("NVN %s is not implemented (x0 %#llx x1 %#llx x2 %#llx)", (const char *)name, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    static struct { const char *name; } seen[512];
    static volatile LONG nseen;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    LONG i;
    for (i = 0; i < nseen && seen[i].name != name; i++) {}
    if (i == nseen && nseen < 512) {
        seen[nseen++].name = (const char *)name;
        hos_log("nvn: %s is not implemented: answered %d (x0 %#llx x1 %#llx x2 %#llx, lr %#llx)", (const char *)name,
                strstr((const char *)name, "Initialize") ? 1 : 0, (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
    }
    ReleaseSRWLockExclusive(&lock);
    RET(strstr((const char *)name, "Initialize") ? 1 : 0);
}
static int cmp_impl(const void *a, const void *b) { return strcmp(((const nvn_impl *)a)->name, ((const nvn_impl *)b)->name); }
uint64_t nvn_native_proc(const char *name) {
    static nvn_impl *sorted;
    static uint64_t *addr;
    static SRWLOCK lock = SRWLOCK_INIT;
    enum { N = sizeof impls / sizeof *impls };
    AcquireSRWLockExclusive(&lock);
    if (!sorted) {
        sorted = (nvn_impl *)malloc(sizeof impls);
        memcpy(sorted, impls, sizeof impls);
        qsort(sorted, N, sizeof *sorted, cmp_impl);
        addr = (uint64_t *)calloc(N, sizeof *addr);
        hos_log("nvn: native NVN layer: %d procs", (int)N);
    }
    const nvn_impl key = {name, NULL};
    const nvn_impl *f = (const nvn_impl *)bsearch(&key, sorted, N, sizeof *sorted, cmp_impl);
    uint64_t a;
    if (f) {
        const size_t i = (size_t)(f - sorted);
        if (!addr[i]) addr[i] = hos_hostcall(f->name, f->fn, NULL);
        a = addr[i];
    } else {   /* asked for, maybe never called: it stops the game only when called */
        static struct { char *name; uint64_t a; } *un;
        static size_t nun;
        size_t k;
        for (k = 0; k < nun && strcmp(un[k].name, name); k++) {}
        if (k == nun) {
            un = realloc(un, (nun + 1) * sizeof *un);
            un[nun].name = _strdup(name);
            un[nun].a = hos_hostcall(un[nun].name, unimplemented, un[nun].name);
            nun++;
        }
        a = un[k].a;
    }
    ReleaseSRWLockExclusive(&lock);
    return a;
}
/* The native NVN layer is the default on PC since 2026-10-20 (faster than the emulated GPU: castle 70.6 vs 60.4 fps;
   checked on the title, loading screens, prologue, sky island cave, rain open world, castle) and on the web since
   2026-10-23 (castle, shown fps: 39.3/40.4 against the emulated GPU's ~33; threaded build 43.9/44.6; rain 52, sky island
   cave 59; it drew the castle black there until NVN programs shared samplers past the browser's 16 a stage).
   HOS_NVN_NATIVE=0: the lifted NVN driver over the emulated Maxwell GPU (hos/gpu.c). */
int nvn_native_on(void) { return 1; }   /* (mkw-recomp: there is no emulated GPU to fall back to) */

/* An NVN function the SDK exports, called directly (gen/nvn2_exports.c: lift.py --standalone hooks every one, so
   the console driver in the SDK never runs): the same function the proc table hands out */
void nvn2_dispatch(Ctx *c, const char *name, void **cache) {
    if (!cache[0]) {
        uint64_t a = nvn_native_proc(name);
        uint64_t hos_hostcall_target(uint64_t addr, void (**fn)(Ctx *, void *), void **user);   /* hos_hostcall.c */
        void (*fn)(Ctx *, void *) = NULL;
        void *user = NULL;
        hos_hostcall_target(a, &fn, &user);
        cache[1] = user;
        __atomic_store_n(&cache[0], (void *)fn, __ATOMIC_RELEASE);
    }
    ((void (*)(Ctx *, void *))cache[0])(c, cache[1]);
}

/* nvnBootstrapLoader(char const* name) -> the proc */
HLE(nvnBootstrapLoader) {
    const char *name = ARG(0) ? (const char *)GP(ARG(0)) : "";
    RET(nvn_native_proc(name));
}
