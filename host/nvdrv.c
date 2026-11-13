/* nvdrv: NVIDIA's kernel-driver interface, below the real NVN driver.

   The NVN driver and NVIDIA's resource manager (NvRm*) are lifted SDK code and run unchanged. They reach the GPU only
   through the exported NvOs layer: NvOsDrvOpen / NvOsDrvIoctl{,2,3} / NvOsDrvClose / NvOsDrvMapSharedMem /
   NvOsDrvQueryEvent, which on hardware talk to the nvdrv service. Those are hooked here and emulate the devices:

     /dev/nvmap            memory handles; the backing memory is the game's own (guest addresses)
     /dev/nvhost-as-gpu    the GPU's virtual address space: GPU VA -> guest address mappings
     /dev/nvhost-gpu       channels: GPFIFO submissions run on the GPU (hos/gpu.c) as they are made, so a
                           submission's fence has passed by the time the ioctl returns
     /dev/nvhost-ctrl      sync points and their events
     /dev/nvhost-ctrl-gpu  Tegra X1 (GM20B) characteristics, zcull/zbc, TPC masks, GPU time

   Ioctl numbers are Linux-style: nr bits 0-7, group 8-15, size 16-29, direction 30-31. Structures and values follow
   suyu's nvdrv (src/core/hle/service/nvdrv, GPL-2.0-or-later) and switchbrew "NV services". */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

void hos_system_event_init(uint64_t key, int autoclear);
void hos_system_event_signal(uint64_t key);

enum { NV_OK = 0, NV_NOT_IMPLEMENTED = 1, NV_NOT_SUPPORTED = 2, NV_BAD_PARAMETER = 4, NV_TIMEOUT = 5,
       NV_INSUFFICIENT_MEMORY = 6, NV_BAD_VALUE = 0xB, NV_CONFIG_NOT_FOUND = 0x30006 };
enum { DEV_NONE, DEV_NVMAP, DEV_AS_GPU, DEV_GPU, DEV_CTRL, DEV_CTRL_GPU, DEV_OTHER };

static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)

/* ---- file descriptors ---------------------------------------------------------------------------------------- */
typedef struct { int type; char path[48]; int syncpt; } Fd;   /* syncpt: a channel's sync point */
static Fd fds[256];

static const struct { const char *path; int type; } devices[] = {
    {"/dev/nvmap", DEV_NVMAP}, {"/dev/nvhost-as-gpu", DEV_AS_GPU}, {"/dev/nvhost-gpu", DEV_GPU},
    {"/dev/nvhost-ctrl", DEV_CTRL}, {"/dev/nvhost-ctrl-gpu", DEV_CTRL_GPU}, {"/dev/nvhost-nvdec", DEV_OTHER},
    {"/dev/nvhost-vic", DEV_OTHER}, {"/dev/nvhost-nvjpg", DEV_OTHER}, {"/dev/nvdisp-disp0", DEV_OTHER},
    {"/dev/nvdisp-disp1", DEV_OTHER}, {"/dev/nvhost-display", DEV_OTHER}, {"/dev/nvhost-prof-gpu", DEV_OTHER},
    {"/dev/nvhost-dbg-gpu", DEV_OTHER}, {"/dev/nvsched-ctrl", DEV_OTHER}};

/* ---- sync points: 192 counters. syncpt is the value the GPU reached, syncpt_max the value the work submitted so far
   will reach (the fences handed out). With the GPU thread (default; HOS_GPU_SYNC=1: synchronous, they move together)
   submissions run on it (hos/gpu.c) and sync points advance when they finish; waits block, and events are signalled
   then. ------------------------------------------------------------------------------------------------------- */
#define SYNCPTS 192
static volatile uint32_t syncpt[SYNCPTS], syncpt_max[SYNCPTS];
static SRWLOCK sync_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE sync_cv = CONDITION_VARIABLE_INIT;
int hos_gpu_async(void);                                         /* gpu.c */
static uint64_t ctrl_events[64];   /* user event id -> the SystemEventType the game queried for it */
static uint8_t ctrl_early[64];     /* a fence on that event passed before the game queried the event: signal it then */
/* event waits that had not passed: signal the slot's event when the sync point reaches the value */
static struct { int used; uint32_t slot, id, value; } pending[512];
static int sync_trace = -1;   /* HOS_SYNC_TRACE=1: log fences and sync point waits */
#define SYNC_LOG(...) do { if (sync_trace < 0) sync_trace = env_on("HOS_SYNC_TRACE");                            if (sync_trace) hos_log(__VA_ARGS__); } while (0)
static int next_syncpt = 24;   /* low ids belong to the system */

/* ---- nvmap handles ---------------------------------------------------------------------------------------------- */
typedef struct { uint32_t size, align, flags, kind, id; uint64_t address; int used, allocated; } Handle;
static Handle handles[8192];
static uint32_t next_id = 1;

static Handle *handle(uint32_t h) { return h && h < 8192 && handles[h].used ? &handles[h] : NULL; }
/* Guest address of an nvmap handle's memory (0 if none): the display reads window buffers through it. */
uint64_t hos_nvmap_address(uint32_t h) { Handle *x = handle(h); return x ? x->address : 0; }

/* ---- the GPU address space: mappings GPU VA -> guest address, sorted by VA ------------------------------------ */
typedef struct { uint64_t va, size, guest; } Map;
static Map *maps;
static int nmaps, capmaps;
static uint64_t va_next = 0x400000000ULL, va_end = 1ULL << 40;   /* 40-bit GPU VA, as GM20B reports */

static int map_trace = -1;   /* HOS_MAP_TRACE=1: log every GPU mapping change */
#define MAP_LOG(...) do { if (map_trace < 0) map_trace = env_on("HOS_MAP_TRACE");                           if (map_trace) hos_log(__VA_ARGS__); } while (0)

/* index of the last mapping starting at or below va, or -1 */
static int map_floor(uint64_t va) {
    int lo = 0, hi = nmaps - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (maps[mid].va <= va) lo = mid + 1;
        else hi = mid - 1;
    }
    return hi;
}
/* a new mapping replaces whatever started inside its range (a remap of the same VA, sparse rebinding) */
static volatile LONG maps_changed = 1;   /* the mappings changed since hos_gpu_guest_ranges last merged them */
volatile LONG maps_generation = 1;   /* bumped by every change (hos_gpu_ptr's per-thread cache, the renderer's gpu_read) */
static void map_add(uint64_t va, uint64_t size, uint64_t guest) {
    maps_changed = 1;
    InterlockedIncrement(&maps_generation);
    MAP_LOG("nvdrv: map GPU %#llx..%#llx -> %#llx", (unsigned long long)va, (unsigned long long)(va + size),
            (unsigned long long)guest);
    int i = map_floor(va);
    if (i >= 0 && maps[i].va == va) { maps[i].size = size; maps[i].guest = guest; return; }
    int at = i + 1, end = at;
    while (end < nmaps && maps[end].va < va + size) end++;
    if (end > at) { memmove(&maps[at], &maps[end], (nmaps - end) * sizeof *maps); nmaps -= end - at; }
    if (nmaps == capmaps) {
        capmaps = capmaps ? capmaps * 2 : 256;
        maps = (Map *)realloc(maps, capmaps * sizeof *maps);
    }
    memmove(&maps[at + 1], &maps[at], (nmaps - at) * sizeof *maps);
    maps[at] = (Map){va, size, guest};
    nmaps++;
}
static void map_remove(uint64_t va) {
    maps_changed = 1;
    InterlockedIncrement(&maps_generation);
    MAP_LOG("nvdrv: unmap GPU %#llx", (unsigned long long)va);
    int i = map_floor(va);
    if (i >= 0 && maps[i].va == va) { memmove(&maps[i], &maps[i + 1], (nmaps - i - 1) * sizeof *maps); nmaps--; }
}
static uint64_t va_alloc(uint64_t size, uint64_t align) {
    if (align < 0x10000) align = 0x10000;
    uint64_t va = (va_next + align - 1) & ~(align - 1);
    va_next = va + ((size + 0xFFFF) & ~0xFFFFULL);
    return va;
}

/* (mkw-recomp) The memory pools one by one, as {guest start, size} pairs (at most max; the count is returned), and a
   number that changes with them: the Vulkan back end imports each pool as host memory (gpu/vk/vk_memory.cpp) */
int hos_gpu_pools(uint64_t (*out)[2], int max) {
    LOCK();
    int n = 0;
    for (int i = 0; i < nmaps && n < max; i++)
        if (maps[i].va == maps[i].guest) out[n][0] = maps[i].guest, out[n][1] = maps[i].size, n++;
    UNLOCK();
    return n;
}
uint32_t hos_gpu_maps_generation(void) { return (uint32_t)maps_generation; }

/* The native NVN layer's memory pools (GPU address = guest address): mapped here only for the renderer's change
   tracking (hos_gpu_ptr resolves native addresses itself) */
/* (and, for the change tracking, a list of their own counted by range: in the GPU mappings, keyed by start, a pool made
   over another's memory replaced its entry or dropped the ones inside it, and the old pool's finalize then removed the
   new one's. The CPU's writes to such a pool were never collected again: MK World's font cache kept the glyphs it had
   first, the menus lost letters. The mappings stay as they were for the pools' imports, hos_gpu_pools) */
typedef struct { uint64_t lo, hi; int n; } Tracked;
static Tracked *tracked;
static int ntracked, captracked;
void hos_gpu_track(uint64_t guest, uint64_t size, int on) {
    const uint64_t lo = guest & ~0xFFFULL, hi = (guest + size + 0xFFF) & ~0xFFFULL;
    LOCK();
    if (on) map_add(guest, size, guest);
    else map_remove(guest);
    int i = 0;
    while (i < ntracked && (tracked[i].lo != lo || tracked[i].hi != hi)) i++;
    if (on) {
        if (i < ntracked) tracked[i].n++;
        else {
            if (ntracked == captracked) {
                captracked = captracked ? captracked * 2 : 256;
                tracked = (Tracked *)realloc(tracked, (size_t)captracked * sizeof *tracked);
            }
            tracked[ntracked++] = (Tracked){lo, hi, 1};
        }
    } else if (i < ntracked && --tracked[i].n <= 0) {
        tracked[i] = tracked[--ntracked];
    }
    maps_changed = 1;
    InterlockedIncrement(&maps_generation);
    UNLOCK();
}

/* The guest memory the GPU can reach (every mapping's guest range, merged, page aligned), for the renderer's change
   tracking: GetWriteWatch over the whole guest arena cost ~2.5 ms a call, and the GPU only reads these. Returns
   the number of ranges (at most max) written to out as {start, end} pairs. */
static int cmp_range(const void *a, const void *b) {
    const uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
int hos_gpu_guest_ranges(uint64_t (*out)[2], int max) {
    static uint64_t (*merged)[2];
    static int nmerged, capmerged;
    LOCK();
    if (maps_changed) {
        maps_changed = 0;
        const int nall = nmaps + ntracked;   /* (the GPU mappings and the native NVN pools) */
        if (capmerged < nall) {
            capmerged = nall + 64;
            merged = (uint64_t (*)[2])realloc(merged, (size_t)capmerged * sizeof *merged);
        }
        for (int i = 0; i < nmaps; i++) {
            merged[i][0] = maps[i].guest & ~0xFFFULL;
            merged[i][1] = (maps[i].guest + maps[i].size + 0xFFF) & ~0xFFFULL;
        }
        for (int i = 0; i < ntracked; i++) merged[nmaps + i][0] = tracked[i].lo, merged[nmaps + i][1] = tracked[i].hi;
        qsort(merged, (size_t)nall, sizeof *merged, cmp_range);
        nmerged = 0;
        /* ranges less than 16 MB apart are one: a GetWriteWatch call costs about what 16 MB of range does, and
           TOTK maps ~4000 pieces */
        for (int i = 0; i < nall; i++) {
            if (nmerged && merged[i][0] <= merged[nmerged - 1][1] + (16ULL << 20)) {
                if (merged[i][1] > merged[nmerged - 1][1]) merged[nmerged - 1][1] = merged[i][1];
            } else {
                merged[nmerged][0] = merged[i][0];
                merged[nmerged][1] = merged[i][1];
                nmerged++;
            }
        }
    }
    int n = nmerged < max ? nmerged : max;
    memcpy(out, merged, (size_t)n * sizeof *merged);
    UNLOCK();
    return n;
}

/* GPU VA -> guest address. Returns 0 if unmapped. */
uint8_t *hos_gpu_ptr(uint64_t va, uint64_t *avail);
uint64_t nvn_gpu_guest(uint64_t gpu);
int nvn_native_on(void);
uint64_t hos_guest_size(void);
uint64_t hos_gpu_translate(uint64_t va) {
    static int native = -1;
    if (native < 0) native = nvn_native_on();
    if (native) {   /* (no contiguity asked: the guest address alone) */
        const uint64_t g = nvn_gpu_guest(va);
        return g < hos_guest_size() ? g : 0;
    }
    uint64_t avail;
    uint8_t *p = hos_gpu_ptr(va, &avail);
    /* (unsigned: a pointer difference is 32-bit and signed in wasm, where the guest window passes 2 GB) */
    return p ? (uint64_t)((uintptr_t)p - (uintptr_t)GP(0)) : 0;
}
/* GPU VA -> host pointer, and how many bytes from there stay inside the mapping. NULL if unmapped. The renderer and
   the command processor ask tens of thousands of times a frame, nearly always for a mapping they just used: each
   thread keeps its last few mappings, good while no mapping changed. */
uint64_t hos_guest_size(void);
int nvn_native_on(void);                                 /* hos/nvn_native.c */
uint64_t nvn_gpu_guest(uint64_t gpu);
uint64_t nvn_gpu_contiguous(uint64_t gpu, uint64_t n);
uint8_t *hos_gpu_ptr(uint64_t va, uint64_t *avail) {
    /* the native NVN layer (HOS_NVN_NATIVE=1): a GPU address is the guest address (virtual pools: through their page
       tables), with no GPU address space of its own */
    static int native = -1;
    if (native < 0) native = nvn_native_on();
    if (native) {
        const uint64_t g = nvn_gpu_guest(va);
        if (!g || g >= hos_guest_size()) return NULL;
        *avail = va < hos_guest_size() ? hos_guest_size() - g : nvn_gpu_contiguous(va, 1ull << 30);   /* (mkw-recomp: the
                                                                     identity range is the whole 512 GB window) */
        return (uint8_t *)GP(g);
    }
    /* direct-mapped by 64 KB page (a draw touches more mappings than a short list holds: constant buffers in several
       pools, the header pools, vertex data; a miss is a locked search over ~4000 mappings) */
    static _Thread_local struct { uint64_t lo, hi, guest; LONG generation; } recent[1024];
    const LONG generation = maps_generation;
    const unsigned k = (unsigned)(va >> 16) & 1023;
    if (recent[k].generation == generation && va >= recent[k].lo && va < recent[k].hi) {   /* (most calls) */
        *avail = recent[k].hi - va;
        return (uint8_t *)GP(recent[k].guest + (va - recent[k].lo));
    }
    AcquireSRWLockShared(&lock);
    int i = map_floor(va);
    uint8_t *p = NULL;
    if (i >= 0 && va < maps[i].va + maps[i].size) {
        *avail = maps[i].va + maps[i].size - va;
        p = (uint8_t *)GP(maps[i].guest + (va - maps[i].va));
        recent[k].lo = maps[i].va;
        recent[k].hi = maps[i].va + maps[i].size;
        recent[k].guest = maps[i].guest;
        recent[k].generation = generation;
    }
    ReleaseSRWLockShared(&lock);
    return p;
}

/* The GPU (hos/gpu.c) executes every GPFIFO submission: entries are { u64: GPU VA of a command list (bits 0-39),
   words (42-62) }. */
void hos_gpu_submit(int channel_fd, const uint64_t *entries, uint32_t n);
/* the same on the GPU thread (HOS_GPU_ASYNC): then two increments of sync point incr_id (unless -1) */
void hos_gpu_queue_submit(int channel_fd, const uint64_t *entries, uint32_t n, int incr_id);
void hos_gpu_syncpt_incr(uint32_t id) {
    id %= SYNCPTS;
    AcquireSRWLockExclusive(&sync_lock);
    uint32_t v = InterlockedIncrement((volatile LONG *)&syncpt[id]);
    if ((int32_t)(syncpt_max[id] - v) < 0) syncpt_max[id] = v;   /* increments the kernel did not hand out */
    for (int i = 0; i < 512; i++)
        if (pending[i].used && pending[i].id == id && (int32_t)(v - pending[i].value) >= 0) {
            pending[i].used = 0;
            /* (the driver may register a wait before it queries the event: the signal must not be lost, or it
               waits forever: the rare stall while loading) */
            if (ctrl_events[pending[i].slot & 63]) hos_system_event_signal(ctrl_events[pending[i].slot & 63]);
            else ctrl_early[pending[i].slot & 63] = 1;
        }
    ReleaseSRWLockExclusive(&sync_lock);
    WakeAllConditionVariable(&sync_cv);
}

/* Blocks until sync point id reaches value or timeout_ms passes (-1: forever). 1 when it did. */
static int syncpt_wait(uint32_t id, uint32_t value, int32_t timeout_ms) {
    id %= SYNCPTS;
    int64_t end = timeout_ms < 0 ? INT64_MAX : hos_now_ns() + (int64_t)timeout_ms * 1000000;
    AcquireSRWLockExclusive(&sync_lock);
    for (;;) {
        if ((int32_t)(syncpt[id] - value) >= 0) break;
        int64_t left = end - hos_now_ns();
        if (left <= 0) {
            ReleaseSRWLockExclusive(&sync_lock);
            return 0;
        }
        hos_cv_sleep(&sync_cv, &sync_lock, timeout_ms < 0 ? 100 : (DWORD)(left / 1000000 + 1));
    }
    ReleaseSRWLockExclusive(&sync_lock);
    return 1;
}

/* ---- per-device ioctls ------------------------------------------------------------------------------------------ */
static uint32_t ioctl_nvmap(uint32_t nr, uint64_t p) {
    switch (nr) {
    case 0x01: {   /* CREATE {u32 size; u32 handle out} */
        LOCK();
        uint32_t h = 1;
        while (h < 8192 && handles[h].used) h++;
        if (h == 8192) { UNLOCK(); return NV_INSUFFICIENT_MEMORY; }
        handles[h] = (Handle){G32(p), 0x1000, 0, 0, next_id++, 0, 1, 0};
        UNLOCK();
        G32(p + 4) = h;
        return NV_OK;
    }
    case 0x03: {   /* FROM_ID {u32 id; u32 handle out} */
        uint32_t id = G32(p);
        for (uint32_t h = 1; h < 8192; h++)
            if (handles[h].used && handles[h].id == id) { G32(p + 4) = h; return NV_OK; }
        return NV_BAD_VALUE;
    }
    case 0x04: {   /* ALLOC {u32 handle, heap_mask, flags, align; u8 kind; pad[7]; u64 address} */
        Handle *h = handle(G32(p));
        if (!h) return NV_BAD_VALUE;
        h->flags = G32(p + 8);
        h->align = G32(p + 12) ? G32(p + 12) : 0x1000;
        h->kind = *(uint8_t *)GP(p + 16);
        h->address = G64(p + 24);
        h->allocated = 1;
        return NV_OK;
    }
    case 0x05: {   /* FREE {u32 handle; pad; u64 address out; u32 size out; u32 flags out} */
        Handle *h = handle(G32(p));
        if (!h) return NV_BAD_VALUE;
        G64(p + 8) = h->address;
        G32(p + 16) = h->size;
        G32(p + 20) = h->flags;
        h->used = 0;
        return NV_OK;
    }
    case 0x09: {   /* PARAM {u32 handle, param; u32 result} */
        Handle *h = handle(G32(p));
        if (!h) return NV_BAD_VALUE;
        switch (G32(p + 4)) {
        case 1: G32(p + 8) = h->size; break;
        case 2: G32(p + 8) = h->align; break;
        case 3: G32(p + 8) = (uint32_t)h->address; break;
        case 4: G32(p + 8) = h->allocated ? 0x40000000 : 0; break;   /* heap: IOVMM */
        case 5: G32(p + 8) = h->kind; break;
        case 6: G32(p + 8) = 0; break;
        default: return NV_BAD_PARAMETER;
        }
        return NV_OK;
    }
    case 0x0E: {   /* GET_ID {u32 id out; u32 handle} */
        Handle *h = handle(G32(p + 4));
        if (!h) return NV_BAD_VALUE;
        G32(p) = h->id;
        return NV_OK;
    }
    }
    return NV_NOT_IMPLEMENTED;
}

static uint32_t ioctl_as_gpu(uint32_t nr, uint64_t p, uint64_t size) {
    switch (nr) {
    case 0x01: return NV_OK;   /* BIND_CHANNEL {s32 fd} */
    case 0x09: {               /* ALLOC_AS_EX {flags, as_fd, big_page_size, reserved; u64 va_start, va_end, va_split} */
        MAP_LOG("nvdrv: alloc as ex flags %#x big page size %#x va %#llx..%#llx split %#llx", G32(p), G32(p + 8),
                (unsigned long long)G64(p + 16), (unsigned long long)G64(p + 24), (unsigned long long)G64(p + 32));
        if (G64(p + 16)) va_next = G64(p + 16);
        if (G64(p + 24)) va_end = G64(p + 24);
        return NV_OK;
    }
    case 0x02: {               /* ALLOC_SPACE {u32 pages, page_size, flags; pad; u64 offset|align} */
        uint64_t sz = (uint64_t)G32(p) * G32(p + 4);
        uint32_t flags = G32(p + 8);
        MAP_LOG("nvdrv: alloc space %#llx bytes flags %#x at %#llx", (unsigned long long)sz, flags,
                (unsigned long long)G64(p + 16));
        if (flags & 1) return NV_OK;                     /* fixed offset: the VA is already chosen */
        LOCK();
        G64(p + 16) = va_alloc(sz, G64(p + 16) ? G64(p + 16) : G32(p + 4));
        UNLOCK();
        return NV_OK;
    }
    case 0x03: return NV_OK;   /* FREE_SPACE */
    case 0x05: {               /* UNMAP_BUFFER {s64 offset} */
        LOCK();
        map_remove(G64(p));
        UNLOCK();
        return NV_OK;
    }
    case 0x06: {               /* MAP_BUFFER_EX {flags, kind, handle, page_size; s64 buffer_offset; u64 size; s64 offset} */
        uint32_t flags = G32(p);
        if (flags & 0x100) {   /* REMAP: a range of an existing mapping gets another kind (NVN does this for compressed
                                  depth textures: refused, nvnTextureInitialize fails and the game draws its rain
                                  occlusion depth to GPU address 0). The memory behind it stays: just check it */
            const uint64_t va = G64(p + 32);
            LOCK();
            const int i = map_floor(va);
            const int ok = i >= 0 && va + G64(p + 16) + G64(p + 24) <= maps[i].va + maps[i].size;
            UNLOCK();
            MAP_LOG("nvdrv: remap GPU %#llx +%#llx size %#llx kind %#x: %s", (unsigned long long)va,
                    (unsigned long long)G64(p + 16), (unsigned long long)G64(p + 24), G32(p + 4), ok ? "ok" : "no mapping");
            return ok ? NV_OK : NV_BAD_VALUE;
        }
        Handle *h = handle(G32(p + 8));
        if (!h) return NV_BAD_VALUE;
        uint64_t off = G64(p + 16), sz = G64(p + 24);
        if (!sz) sz = h->size - off;
        LOCK();
        uint64_t va = (flags & 1) ? G64(p + 32) : va_alloc(sz, G32(p + 12) ? G32(p + 12) : 0x10000);
        MAP_LOG("nvdrv: map buffer ex flags %#x kind %#x handle align %#x page size %#x size %#llx -> %#llx", flags,
                G32(p + 4), h->align, G32(p + 12), (unsigned long long)sz, (unsigned long long)va);
        map_add(va, sz, h->address + off);
        UNLOCK();
        G64(p + 32) = va;
        return NV_OK;
    }
    case 0x08: {               /* GET_VA_REGIONS {u64 buf; u32 buf_size; u32 reserved; VaRegion[2]} */
        G32(p + 8) = 2 * 0x18;
        uint64_t r = p + 16;
        G64(r) = 0x04000000ULL; G32(r + 8) = 0x1000; G32(r + 12) = 0; G64(r + 16) = 0x3FBFFULL;   /* small pages */
        r += 0x18;
        G64(r) = 0x400000000ULL; G32(r + 8) = 0x10000; G32(r + 12) = 0; G64(r + 16) = 0x1BFFFF0ULL;  /* big pages */
        return NV_OK;
    }
    case 0x14: {               /* REMAP: an array of {u16 flags, kind; u32 handle, handle_offset_bp, as_offset_bp, big_pages} */
        for (uint64_t e = p; e + 20 <= p + size; e += 20) {
            Handle *h = handle(G32(e + 4));
            uint64_t bp = 0x10000;
            uint64_t va = (uint64_t)G32(e + 12) * bp, sz = (uint64_t)G32(e + 16) * bp;
            LOCK();
            if (h) map_add(va, sz, h->address + (uint64_t)G32(e + 8) * bp);
            else map_remove(va);   /* handle 0: unmap (sparse) */
            UNLOCK();
        }
        return NV_OK;
    }
    }
    return NV_NOT_IMPLEMENTED;
}

static uint32_t ioctl_gpu(int fd, uint32_t group, uint32_t nr, uint64_t p, uint64_t size, uint64_t in2, uint64_t in2size) {
    if (group == 0x00 && nr == 0x03) { G32(p + 4) = 0; return NV_OK; }   /* GET_WAITBASE */
    if (group == 0x47 && (nr == 0x14 || nr == 0x15)) return NV_OK;    /* SET/GET_CLIENT_DATA */
    if (group != 0x48) return NV_NOT_IMPLEMENTED;
    switch (nr) {
    case 0x01: case 0x03: case 0x0B: case 0x0C: case 0x0D: case 0x1D:
        return NV_OK;   /* SET_NVMAP_FD, SET_TIMEOUT, ZCULL_BIND, SET_ERROR_NOTIFIER, SET_PRIORITY, SET_TIMESLICE */
    case 0x17:          /* GET_ERROR_NOTIFICATION {u64 timestamp; u32 info32; u16 info16; u16 status}: no error */
        memset(GP(p), 0, 16);
        return NV_OK;
    case 0x09:          /* ALLOC_OBJ_CTX {u32 class, flags; u64 obj_id out} */
        G64(p + 8) = G32(p);
        return NV_OK;
    case 0x18: case 0x1A: {   /* ALLOC_GPFIFO_EX(2) {u32 entries, jobs, flags; NvFence out; reserved} */
        LOCK();
        if (!fds[fd].syncpt) {   /* (hookshot: ids are used again after the last one; they index syncpt[] and syncpt_max[]) */
            fds[fd].syncpt = next_syncpt++;
            if (next_syncpt >= SYNCPTS) next_syncpt = 24;
        }
        UNLOCK();
        G32(p + 12) = (uint32_t)fds[fd].syncpt;
        if ((int32_t)(syncpt_max[fds[fd].syncpt] - syncpt[fds[fd].syncpt]) < 0)
            syncpt_max[fds[fd].syncpt] = syncpt[fds[fd].syncpt];
        G32(p + 16) = syncpt[fds[fd].syncpt];
        SYNC_LOG("nvdrv: channel fd %d gets sync point %d (value %u)", fd, fds[fd].syncpt, syncpt[fds[fd].syncpt]);
        return NV_OK;
    }
    case 0x08: case 0x1B: {   /* SUBMIT_GPFIFO {u64 address; u32 num_entries; u32 flags; NvFence fence} + entries */
        uint32_t n = G32(p + 8), flags = G32(p + 12);
        uint64_t entries = in2 && in2size >= (uint64_t)n * 8 ? in2 : p + 24;   /* Ioctl2: inline; else after it */
        (void)size;
        if (!hos_guest_range(entries, (uint64_t)n * 8)) return NV_BAD_PARAMETER;   /* (hookshot) */
        int sp = fds[fd].syncpt;
        /* add_increment (bit 1): the kernel appends two sync point increments after the work (suyu's
           BuildIncrementCommandList): the fence is the value they reach */
        uint32_t v;
        if (hos_gpu_async()) {
            AcquireSRWLockExclusive(&sync_lock);
            if (flags & 2) syncpt_max[sp] += 2;
            v = syncpt_max[sp];
            ReleaseSRWLockExclusive(&sync_lock);
            hos_gpu_queue_submit(fd, (const uint64_t *)GP(entries), n, (flags & 2) ? sp : -1);
        } else {
            hos_gpu_submit(fd, (const uint64_t *)GP(entries), n);
            if (flags & 2) { hos_gpu_syncpt_incr(sp); hos_gpu_syncpt_incr(sp); }
            v = syncpt[sp];
        }
        SYNC_LOG("nvdrv: submit %u entries flags %#x fence in {%u, %u} -> {%d, %u}", n, flags, G32(p + 16), G32(p + 20),
                 sp, v);
        G32(p + 16) = (uint32_t)sp;
        G32(p + 20) = v;
        return NV_OK;
    }
    }
    return NV_NOT_IMPLEMENTED;
}

static uint32_t ioctl_ctrl(uint32_t nr, uint64_t p) {
    switch (nr) {
    case 0x14:                                                         /* SYNCPT_READ {id; value out} */
        G32(p + 4) = syncpt[G32(p) % SYNCPTS];
        SYNC_LOG("nvdrv: syncpt read id %u: %u", G32(p), G32(p + 4));
        return NV_OK;
    case 0x15:                                                         /* SYNCPT_INCR {id} */
        hos_gpu_syncpt_incr(G32(p));
        SYNC_LOG("nvdrv: syncpt incr id %u (CPU): %u", G32(p), syncpt[G32(p) % SYNCPTS]);
        return NV_OK;
    case 0x16: case 0x19: {                                            /* SYNCPT_WAIT(EX) {id, thresh, timeout[, value]} */
        int ok = syncpt_wait(G32(p), G32(p + 4), (int32_t)G32(p + 8));
        uint32_t v = syncpt[G32(p) % SYNCPTS];
        SYNC_LOG("nvdrv: syncpt wait id %u thresh %u timeout %d: value %u", G32(p), G32(p + 4), (int)G32(p + 8), v);
        if (nr == 0x19) G32(p + 12) = v;
        return ok ? NV_OK : NV_TIMEOUT;
    }
    case 0x1B: return NV_CONFIG_NOT_FOUND;                             /* GET_CONFIG */
    case 0x1C: return NV_OK;                                           /* CLEAR_EVENT_WAIT */
    case 0x1D: case 0x1E: {   /* EVENT_WAIT(_ASYNC) {NvFence fence; u32 timeout; u32 value in/out} */
        uint32_t id = G32(p) % SYNCPTS, thresh = G32(p + 4);
        SYNC_LOG("nvdrv: event wait%s id %u thresh %u timeout %d: value %u", nr == 0x1E ? " async" : "", id, thresh,
                 (int)G32(p + 8), syncpt[id]);
        if ((int32_t)(syncpt[id] - thresh) >= 0) {
            G32(p + 12) = syncpt[id];
            return NV_OK;
        }
        if (hos_gpu_async()) {   /* the event (the value's slot) is signalled when the sync point gets there */
            AcquireSRWLockExclusive(&sync_lock);
            int done = (int32_t)(syncpt[id] - thresh) >= 0;
            int i = 0;
            for (; i < 512 && !done; i++)
                if (!pending[i].used) {
                    pending[i].used = 1;
                    pending[i].slot = G32(p + 12) & 0xFFFF;
                    pending[i].id = id;
                    pending[i].value = thresh;
                    break;
                }
            if (i == 512) hos_log("nvdrv: event wait table full: a fence wait is lost");
            ReleaseSRWLockExclusive(&sync_lock);
            if (done) {
                G32(p + 12) = syncpt[id];
                return NV_OK;
            }
        }
        /* synchronous GPU: work finishes at submission, so a fence still ahead was never submitted */
        return NV_TIMEOUT;
    }
    case 0x1F: case 0x20: case 0x21: return NV_OK;                     /* EVENT_REGISTER / UNREGISTER(_BATCH) */
    }
    return NV_NOT_IMPLEMENTED;
}

static uint32_t ioctl_ctrl_gpu(uint32_t nr, uint64_t p, uint64_t out2, uint64_t out2size) {
    switch (nr) {
    case 0x01: G32(p) = 1; return NV_OK;   /* ZCULL_GET_CTX_SIZE */
    case 0x02: {                           /* ZCULL_GET_INFO */
        static const uint32_t v[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
        memcpy(GP(p), v, sizeof v);
        return NV_OK;
    }
    case 0x03: case 0x04: case 0x07: return NV_OK;   /* ZBC_SET_TABLE, ZBC_QUERY_TABLE, FLUSH_L2 */
    case 0x05: {   /* GET_CHARACTERISTICS {u64 buf_size; u64 buf_addr; characteristics[0xA0]} (Ioctl3: into out2) */
        uint32_t c[40] = {0};
        uint64_t q;
        c[0] = 0x120; c[1] = 0xB; c[2] = 0xA1; c[3] = 1;   /* GM20B, rev A1, 1 GPC */
        q = 0x40000; memcpy(&c[4], &q, 8);                  /* L2 256 KB */
        c[8] = 2; c[9] = 0x20; c[10] = 0x20000; c[11] = 0x20000; c[12] = 0x1B; c[13] = 0x30000; c[14] = 1;
        c[15] = 0x503; c[16] = 0x503; c[17] = 0x80; c[18] = 0x28;
        q = 0x55; memcpy(&c[20], &q, 8);
        c[22] = 0x902D; c[23] = 0xB197; c[24] = 0xB1C0; c[25] = 0xB06F; c[26] = 0xA140; c[27] = 0xB0B5;
        c[28] = 1; c[29] = 0; c[30] = 2; c[31] = 1; c[32] = 0; c[33] = 1; c[34] = 0x21D70; c[35] = 0;
        q = 0x6230326D67ULL; memcpy(&c[36], &q, 8);         /* "gm20b" */
        G64(p) = 0xA0;
        memcpy(GP(out2 && out2size >= 0xA0 ? out2 : p + 16), c, 0xA0);
        return NV_OK;
    }
    case 0x06: {   /* GET_TPC_MASKS {u32 size; pad; u64 addr; u32 tpc_mask; pad} */
        G32(p + 16) = 0x3;
        if (out2 && out2size >= 4) G32(out2) = 0x3;
        return NV_OK;
    }
    case 0x14: G32(p) = 0x07; G32(p + 4) = 0x01; return NV_OK;   /* ZBC_GET_ACTIVE_SLOT_MASK */
    case 0x15: G32(p) = 0; return NV_OK;                         /* PMU_GET_GPU_LOAD */
    case 0x1C: G64(p) = (uint64_t)hos_now_ns(); return NV_OK;     /* GET_GPU_TIME */
    }
    return NV_NOT_IMPLEMENTED;
}

static uint32_t ioctl(Ctx *c, int fd, uint32_t cmd, uint64_t buf, uint64_t x2, uint64_t x2size) {
    if (fd < 0 || fd >= 256 || !fds[fd].type) return NV_BAD_PARAMETER;
    uint32_t nr = cmd & 0xFF, group = (cmd >> 8) & 0xFF;
    uint64_t size = (cmd >> 16) & 0x3FFF;
    uint32_t r;
    /* (hookshot) the argument buffers lie in guest memory (the handlers read fixed fields up to 0x100 past the start) */
    if (!hos_guest_range(buf, size + 0x100) || (x2 && !hos_guest_range(x2, x2size))) return NV_BAD_PARAMETER;
    switch (fds[fd].type) {
    case DEV_NVMAP: r = ioctl_nvmap(nr, buf); break;
    case DEV_AS_GPU: r = ioctl_as_gpu(nr, buf, size); break;
    case DEV_GPU: r = ioctl_gpu(fd, group, nr, buf, size, x2, x2size); break;
    case DEV_CTRL: r = ioctl_ctrl(nr, buf); break;
    case DEV_CTRL_GPU: r = ioctl_ctrl_gpu(nr, buf, x2, x2size); break;
    default: r = NV_NOT_IMPLEMENTED;
    }
    if (r == NV_NOT_IMPLEMENTED || hos_trace)
        hos_log("nvdrv %s ioctl %#x (group %#x nr %#x size %#llx) -> %#x", fds[fd].path, cmd, group, nr,
                (unsigned long long)size, r);
    (void)c;
    return r;
}

/* ---- the NvOs layer ----------------------------------------------------------------------------------------------- */
/* NvOsDrvOpen(char const* path) -> fd, or -1 */
HLE(NvOsDrvOpen) {
    const char *path = ARG(0) ? (const char *)GP(ARG(0)) : "";
    int type = 0;
    for (unsigned i = 0; i < sizeof devices / sizeof *devices; i++)
        if (!strcmp(devices[i].path, path)) type = devices[i].type;
    if (!type) { hos_log("nvdrv: open %s: unknown device", path); RET(-1); return; }
    LOCK();
    int fd = 1;
    while (fd < 256 && fds[fd].type) fd++;
    if (fd < 256) {
        fds[fd] = (Fd){type, "", 0};
        snprintf(fds[fd].path, sizeof fds[fd].path, "%s", path);
    }
    UNLOCK();
    if (hos_trace) hos_log("nvdrv: open %s -> %d", path, fd);
    RET(fd < 256 ? fd : -1);
}
HLE(NvOsDrvClose) { int fd = (int)ARG(0); if (fd > 0 && fd < 256) fds[fd].type = 0; RET(0); }
/* NvOsDrvIoctl(int fd, u32 cmd, void* buf) */
HLE(NvOsDrvIoctl) { RET(ioctl(c, (int)ARG(0), (uint32_t)ARG(1), ARG(2), 0, 0)); }
/* NvOsDrvIoctl2(int fd, u32 cmd, void* buf, ?, void const* in2, size in2) */
HLE(NvOsDrvIoctl2) { RET(ioctl(c, (int)ARG(0), (uint32_t)ARG(1), ARG(2), ARG(4), ARG(5))); }
/* NvOsDrvIoctl3(int fd, u32 cmd, void* buf, ?, void* out2, size out2) */
HLE(NvOsDrvIoctl3) { RET(ioctl(c, (int)ARG(0), (uint32_t)ARG(1), ARG(2), ARG(4), ARG(5))); }
/* NvOsDrvMapSharedMem(int fd, handle, size) */
HLE(NvOsDrvMapSharedMem) { RET(0); }
/* NvOsDrvQueryEvent(int fd, u32 event id, os::SystemEventType*) */
HLE(_Z17NvOsDrvQueryEventijPN2nn2os15SystemEventTypeE) {
    SYNC_LOG("nvdrv: query event fd %d id %#x", (int)ARG(0), (uint32_t)ARG(1));
    hos_system_event_init(ARG(2), 1);
    uint32_t id = (uint32_t)ARG(1) & 63;
    AcquireSRWLockExclusive(&sync_lock);
    ctrl_events[id] = ARG(2);
    const int early = ctrl_early[id];
    ctrl_early[id] = 0;
    ReleaseSRWLockExclusive(&sync_lock);
    if (early || !hos_gpu_async()) hos_system_event_signal(ARG(2));   /* (a synchronous GPU never lags) */
    RET(0);
}
