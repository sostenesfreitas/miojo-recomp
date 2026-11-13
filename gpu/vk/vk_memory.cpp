// Guest memory for the GPU: each NVN memory pool imported as host memory (VK_EXT_external_memory_host) with one
// buffer over it. The GPU reads the game's memory where it is and its writes land there: no mirrored copies, no
// write-backs, no upload streams. A GPU address (a guest address for the native pools) maps to (buffer, offset).
//
// Pools are imported the first time something on the GPU touches them, not all at once: MKW has ~2,400 pools at the
// title and importing each is a buffer, a memory object and a device address (tens of seconds taken at one submit,
// on the render thread, in the first frame). The table of pools is cheap to keep current (host->gpu_pools: two
// numbers each) and is refreshed when its generation changes, at a submit.
#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <memory>
#include <new>
#include <mutex>
#include <vector>
#include <condition_variable>
#include "vk_menu.h"
#include <deque>
#include <unordered_map>
#include <thread>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "vk_internal.h"
#include "vk_texture.h"   // (upload_command_buffer)
#include "vk_gprof.h"      // (RX_VK_PASS_TIMING)
#include "import_hold.h"
#include "mirror_trim.h"
#include "wb_learn.h"
#include "mirror_inherit.h"
#include "retired_give_back.h"
#ifdef __linux__
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "shaders/wb_diff_comp_spv.h"
#include "vram_pressure.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

// Every raw imported-buffer destruction goes through here, including unpublished allocation rollback.
// Published imports reach it only at the existing completion/eviction gates; no additional GPU wait.
void destroy_import_buffer(VkBuffer buffer) {
    release_texel_buffer_views(buffer);
    vkDestroyBuffer(dev.device, buffer, nullptr);
}

struct Pool {
    uint64_t lo = 0, hi = 0;   // guest range imported (aligned outwards)
    uint64_t range_lo = 0, range_hi = 0;   // (the game's pool range it was found for: pool_of's last hit)
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> pieces;   // (a pool imported as several dma-bufs: the ones after `memory`)
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    // its VRAM mirror (see "the VRAM mirror" below)
    enum : uint8_t { MIRROR_NONE, MIRROR_WANTED, MIRROR_ON, MIRROR_REFUSED } mstate = MIRROR_NONE;
    VkBuffer mirror = VK_NULL_HANDLE;
    VmaAllocation mirror_alloc = nullptr;
    uint32_t synced_gen = 0;     // every page stamped up to this generation is in the mirror
    uint64_t synced_epoch = 0;   // the submission that last brought it up to date
    uint64_t sync_seg = 0;       // (and the parallel replay's segment whose uploads did)
    uint64_t last_value = 0;     // (the newest submission that bound it: its mirror is in use until that one is done)
    uint64_t direct_write_epoch = 0;   // (the part that last wrote it in guest memory, before it had a mirror)
    uint64_t retired_value = 0;  // (retired: the submission that may still use it)
    bool was_mirrored = false;   // (its mirror was given back: its pages stay in the captures, for the next one)
    // (hookshot) the GPU wrote it past what is copied back (memory_note_gpu_write: a bare entry's guessed range, the pointer
    // table's, a wide binding's): that is in its mirror alone, so the mirror is never given back (mirrors_give_back)
    bool gpu_only = false;
    // (and the GPU wrote it through a binding of its mirror, copied back or not: after a save reload, the mirrors of such pools
    // given back and made again left cyan in play and over the Start menu (RX 5500M, gone with RX_VK_MIRROR_KEEP=1); kept too)
    bool gpu_written = false;
    bool inherit_lost = false;   // (retired, and the pool that took it in made no mirror in time to copy its GPU contents)
    // (EvilOOT) retired with its mirror on: the pool that took it in copies the GPU's contents from it when its own mirror is
    // made (mirror_make), and until then the mirror stays (mirrors_give_back)
    bool inherit_pending = false;
    uint64_t retired_epoch = 0;
    int64_t retired_ms = 0;      // (retired: when, import_now_ms; where its import waits before going back, the wait counts from here)
    bool prefetched = false;     // (amd-fix) imported ahead by the background thread, not bound yet
    // (hookshot, mirror_trim.h) mostly outside every game pool since this submission (0: not); how often its range was trimmed
    // before; and, retired by a trim, its mirror's contents wanted by each of the pools made from its live parts
    uint64_t dead_since = 0;
    uint8_t trims = 0;
    uint64_t trim_epoch = 0;   // (the submission its range was last trimmed in)
    bool trimmed = false;
    // (hookshot) the write-back's diff: what guest memory holds of the pool, in VRAM, and the pages of it known to be so
    VkBuffer shadow = VK_NULL_HANDLE;
    VmaAllocation shadow_alloc = nullptr;
    VkDeviceAddress shadow_address = 0;
    std::vector<uint64_t> shadow_known;   // (a bit a page)
    bool shadow_wanted = false;   // (its write-back was worth one: one given back under VRAM pressure is made again)
};
uint64_t mem_rec_value = 0, mem_completed = 0;   // (the recording being made, and the GPU's progress)

struct GuestPool {
    uint64_t lo = 0, hi = 0;   // (aligned outwards the same way, so an import covers what the lookup found)
};

std::vector<GuestPool> guest_pools;   // by lo, from the host
std::atomic<uint64_t> guest_pools_gen{1};   // (bumped when that table is replaced: a span cached without the lock goes stale)
std::map<uint64_t, Pool> imported;    // by lo, made on first use
Pool* last_pool = nullptr;            // (the last pool_of hit: draws in a row bind buffers of the same pools)
extern Pool* pool_cache[4096];
void retire_pool(std::map<uint64_t, Pool>::iterator it);
Pool* pool_lookup(uint64_t guest, uint64_t size);
struct SrwMutex {   // (a slim reader/writer lock: half std::mutex's cost, taken for every buffer a draw binds)
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
};
SrwMutex pools_mu;
uint32_t generation = ~0u;
uint64_t align = 4096;
uint64_t max_import_size = UINT64_MAX;
int failures = 0;
ImportHolds import_holds;          // (issue 218: a failed import is not tried again for ImportHolds::kGapMs)
std::mutex import_holds_mu;
thread_local bool import_cooling = false;
thread_local VkResult import_result = VK_SUCCESS;

int64_t import_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool import_on_hold(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_cooling = !import_holds.allow(lo, hi, import_now_ms());
    return import_cooling;
}
void import_remember_failure(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_holds.deny_for(lo, hi, import_now_ms());
}

constexpr VkBufferUsageFlags kUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

#ifdef __linux__
// Guest memory handed to the GPU as a dma-buf over the game's own pages: the window is a memfd's (host/hos_main.c),
// and /dev/udmabuf makes a dma-buf of a range of it, which the device imports like any other. The pages are the same ones
// the game reads and writes (nothing is copied), pinned once when it is made. A host-pointer import (VK_EXT_external_memory_host)
// is a userptr buffer object to amdgpu, whose pages the kernel looks up again at every command submission, and RADV puts every
// buffer object in every submission: with ~1.5 GB of pools imported that was 15-45 ms a submission in Kokiri Forest,
// 3-8 fps with 11 ms of GPU work. A pool larger than udmabuf's size limit (/sys/module/udmabuf/parameters/size_limit_mb,
// 64 MB unless raised) is made of several dma-bufs of at most that size, bound one after another into a sparse buffer, so the
// pool is still one buffer with one device address and nobody has to raise the limit. RX_VK_NO_DMABUF=1: host pointers only,
// as before; RX_VK_DMABUF_PIECE_MB=<n>: pieces of at most n MB (to try the sparse path with the limit raised)
struct UdmabufCreate { uint32_t memfd, flags; uint64_t offset, size; };
constexpr unsigned long kUdmabufCreate = (1ul << 30) | (sizeof(UdmabufCreate) << 16) | ('u' << 8) | 0x42;   // (_IOW('u', 0x42, ...))
int dmabuf_memfd = -2, dmabuf_dev = -1, dmabuf_failures = 0;
uint64_t dmabuf_limit = 0;

/// The memory of guest [lo, lo + size) as an imported dma-buf; VK_NULL_HANDLE when udmabuf or the import refused it
VkDeviceMemory dma_buf_memory(uint64_t lo, uint64_t size) {
    UdmabufCreate c{(uint32_t)dmabuf_memfd, 1 /* UDMABUF_FLAGS_CLOEXEC */, lo, size};
    const int fd = ioctl(dmabuf_dev, kUdmabufCreate, &c);
    if (fd < 0) {
        if (dmabuf_failures++ < 8)
            logf("vk: guest %#llx..%#llx: no udmabuf (errno %d)", (unsigned long long)lo, (unsigned long long)(lo + size), errno);
        return VK_NULL_HANDLE;
    }
    static uint32_t type = UINT32_MAX;   // (the same for every dma-buf of the window: asked once)
    if (type == UINT32_MAX) {
        VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vkGetMemoryFdPropertiesKHR(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) == VK_SUCCESS) {
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent, first)
                if ((fp.memoryTypeBits >> i & 1) &&
                    (dev.memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                    type = i;
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)
                if (fp.memoryTypeBits >> i & 1) type = i;
        }
        if (type == UINT32_MAX) {
            close(fd);
            if (dmabuf_failures++ < 8) logf("vk: a dma-buf of guest memory has no memory type");
            return VK_NULL_HANDLE;
        }
    }
    VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    im.fd = fd;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult r = vkAllocateMemory(dev.device, &ai, nullptr, &memory);
    if (r != VK_SUCCESS) {   // (the fd is the caller's again when the import fails)
        close(fd);
        if (dmabuf_failures++ < 8)
            logf("vk: guest %#llx..%#llx: its dma-buf not imported (VkResult %d)", (unsigned long long)lo,
                 (unsigned long long)(lo + size), (int)r);
        return VK_NULL_HANDLE;
    }
    return memory;
}

/// A pool larger than a dma-buf may be: a sparse buffer, each `piece` bytes of it bound to its own dma-buf
bool import_dma_buf_pieces(uint64_t lo, uint64_t hi, uint64_t piece, Pool& p) {
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    bi.size = hi - lo;
    bi.usage = kUsage;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) {
        if (dmabuf_failures++ < 8) logf("vk: pool %#llx..%#llx: no sparse buffer for its dma-bufs", (unsigned long long)lo, (unsigned long long)hi);
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    // (each piece starts at a multiple of the sparse block size; the last runs to the end of what the buffer needs, past hi
    // when the size rounds up: the window's next pages, which the buffer never reaches)
    const uint64_t align = std::max<uint64_t>(req.alignment, 4096);
    piece = piece / align * align;
    const uint64_t end = lo + req.size;
    if (!piece || (req.size & 4095) || end > host->guest_size()) {
        destroy_import_buffer(buffer);
        if (dmabuf_failures++ < 8)
            logf("vk: pool %#llx..%#llx: sparse block %llu, size %llu: not in dma-bufs of %llu", (unsigned long long)lo,
                 (unsigned long long)hi, (unsigned long long)req.alignment, (unsigned long long)req.size, (unsigned long long)piece);
        return false;
    }
    std::vector<VkDeviceMemory> memories;
    std::vector<VkSparseMemoryBind> binds;
    for (uint64_t off = 0; off < req.size; off += piece) {
        const uint64_t n = std::min(piece, req.size - off);
        const VkDeviceMemory m = dma_buf_memory(lo + off, n);
        if (!m) break;
        memories.push_back(m);
        binds.push_back({off, n, m, 0, 0});
    }
    VkResult res = VK_ERROR_UNKNOWN;
    if (binds.size() == (req.size + piece - 1) / piece) {
        VkSparseBufferMemoryBindInfo bb{buffer, (uint32_t)binds.size(), binds.data()};
        VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        si.bufferBindCount = 1;
        si.pBufferBinds = &bb;
        static VkFence fence = VK_NULL_HANDLE;   // (the background importer and the recording thread can both import)
        static std::mutex fence_mu;
        std::lock_guard fk(fence_mu);
        if (!fence) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(dev.device, &fi, nullptr, &fence);
        }
        {   // (on the queue, as the sparse textures' binds: nothing uses the new buffer yet, so no wait for earlier work)
            std::lock_guard qk(dev.present_mu);
            queue_flush_locked();
            res = vkQueueBindSparse(dev.queue, 1, &si, fence);
        }
        if (res == VK_SUCCESS) {
            res = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(dev.device, 1, &fence);
        }
    }
    if (res != VK_SUCCESS) {
        destroy_import_buffer(buffer);
        for (VkDeviceMemory m : memories) vkFreeMemory(dev.device, m, nullptr);
        if (dmabuf_failures++ < 8)
            logf("vk: pool %#llx..%#llx: its %zu dma-bufs not bound (VkResult %d)", (unsigned long long)lo, (unsigned long long)hi,
                 memories.size(), (int)res);
        return false;
    }
    p.lo = lo, p.hi = hi, p.buffer = buffer, p.memory = memories[0];
    p.pieces.assign(memories.begin() + 1, memories.end());
    return true;
}

bool import_dma_buf(uint64_t lo, uint64_t hi, Pool& p) {
    static uint64_t piece = 0;
    if (dmabuf_memfd == -2) {
        dmabuf_memfd = -1;
        typedef int (*MemfdFn)(void);
        const MemfdFn fn = env_on("RX_VK_NO_DMABUF") ? nullptr : (MemfdFn)dlsym(RTLD_DEFAULT, "hos_posix_guest_memfd");
        const int fd = fn ? fn() : -1;
        // (NVIDIA's driver offers a dma-buf of the window two memory types, host visible and coherent (dma_buf_memory's choice)
        // and one with no flags, and refuses the first: VK_ERROR_OUT_OF_DEVICE_MEMORY for every pool, the sparse ones unbound,
        // and its kernel module logs "NVRM: memMap_IMPL: CPU mapping not supported for addressSpace: 0x1" at each start. Every
        // pool was a host-pointer import after all, which on NVIDIA is not slow, so the attempt is not made there. RTX 4090,
        // 615.78.08, Kokiri Forest uncapped, two runs each in turn: 81.3 and 80.4 fps with the attempt, 81.5 and 79.6 without
        // it, no difference beyond the 1 to 2 % between identical runs)
        const bool nvidia = dev.driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
        if (fd >= 0 && dev.dma_buf_import && vkGetMemoryFdPropertiesKHR && !nvidia) dmabuf_dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (fd >= 0 && dmabuf_dev >= 0) {
            dmabuf_memfd = fd;
            dmabuf_limit = 64ull << 20;
            if (FILE* f = fopen("/sys/module/udmabuf/parameters/size_limit_mb", "r")) {
                unsigned long long mb = 0;
                if (fscanf(f, "%llu", &mb) == 1 && mb) dmabuf_limit = mb << 20;
                fclose(f);
            }
            piece = dmabuf_limit;
            if (const char* e = getenv("RX_VK_DMABUF_PIECE_MB"))
                if (atoll(e) > 0) piece = std::min<uint64_t>(piece, (uint64_t)atoll(e) << 20);
        }
        logf("vk: guest memory as dma-bufs: %s", dmabuf_memfd >= 0 ? "yes" : !fn ? "no (no memfd window, or RX_VK_NO_DMABUF)"
                 : fd < 0 ? "no (the window is anonymous memory)" : !dev.dma_buf_import ? "no (VK_EXT_external_memory_dma_buf)"
                 : nvidia ? "no (NVIDIA's driver refuses them: host pointers)" : "no (/dev/udmabuf cannot be opened)");
        if (dmabuf_memfd >= 0)
            logf("vk: dma-bufs of at most %llu MB (udmabuf's limit %llu MB); larger pools %s", (unsigned long long)(piece >> 20),
                 (unsigned long long)(dmabuf_limit >> 20),
                 dev.sparse_buffers ? "are several, in a sparse buffer" : "are host-pointer imports (no sparse binding)");
    }
    if (dmabuf_memfd < 0 || (lo & 4095) || (hi & 4095)) return false;
    if (hi - lo > piece) {
        if (!dev.sparse_buffers || !import_dma_buf_pieces(lo, hi, piece, p)) return false;
    } else {
        VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.pNext = &eb;
        bi.size = hi - lo;
        bi.usage = kUsage;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
        const VkDeviceMemory memory = dma_buf_memory(lo, hi - lo);
        // Allocation alone does not make the buffer usable. A failed bind owns both resources,
        // but must leave no partial import for the host-pointer fallback to publish.
        if (!memory || vkBindBufferMemory(dev.device, buffer, memory, 0) != VK_SUCCESS) {
            destroy_import_buffer(buffer);
            if (memory) vkFreeMemory(dev.device, memory, nullptr);
            return false;
        }
        p.lo = lo, p.hi = hi, p.buffer = buffer, p.memory = memory;
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.buffer;
    p.address = vkGetBufferDeviceAddress(dev.device, &da);
    if (env_on("RX_VK_LOG_ADDR"))   // (as import_pool's: which buffer a device fault's address is in)
        logf("vk: addr dma-buf %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)p.address, (unsigned long long)(p.address + (hi - lo)));
    static int64_t n = 0, bytes = 0, sparse = 0;
    n++, bytes += (int64_t)(hi - lo), sparse += !p.pieces.empty();
    if (n <= 4 || n % 64 == 0 || (!p.pieces.empty() && sparse <= 4))
        logf("vk: pool %#llx..%#llx a dma-buf%s (%lld so, %lld in pieces, %lld MB)", (unsigned long long)lo, (unsigned long long)hi,
             p.pieces.empty() ? "" : "s in a sparse buffer", (long long)n, (long long)sparse, (long long)(bytes >> 20));
    return true;
}
#endif

bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {
    import_cooling = false;
    import_result = VK_SUCCESS;
    if (import_on_hold(lo, hi)) return false;   // (tried recently and failed: not again until the hold ends)
    if (hi <= lo || hi - lo > max_import_size) {
        logf("vk: import %#llx..%#llx exceeds the device's allocation/buffer limit", (unsigned long long)lo,
             (unsigned long long)hi);
        import_remember_failure(lo, hi);
        return false;
    }
    p.lo = lo;
    p.hi = hi;
    uint8_t* ptr = host->guest_base() + lo;
    const uint64_t bytes = hi - lo;
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    clk::time_point t1, t2, t3, t4;
    struct Report {   // (where an import's time goes: logged for slow ones, 12 times)
        const clk::time_point &a, &b, &c, &d, &e;
        ~Report() {
            if (e.time_since_epoch().count() == 0) return;
            const double total = std::chrono::duration<double, std::milli>(e - a).count();
            static int logged;
            if (total < 2.0 || logged++ >= 12) return;
            auto ms = [](clk::time_point x, clk::time_point y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
            logf("vk: import %.1f ms: commit+properties %.1f, buffer %.1f, allocate %.1f, bind+address %.1f", total, ms(a, b),
                 ms(b, c), ms(c, d), ms(d, e));
        }
    } report{t0, t1, t2, t3, t4};
    // (pages the game has not touched yet may be only reserved: committed now, as the pool's memory is usable)
    // (hookshot) One commit avoids the slow query walk, but resets protection on committed pages too. Keep the walk
    // for CPU write watches: commit only reserved pages, leaving the watched pages read-only.
    // ponytail: add new watch switches here; query the host if watches stop requiring a startup switch.
    static const bool page_watches = getenv("RX_NVN_WATCH_TEX") || getenv("RX_NVN_WATCH_TEXELS_SRC") ||
                                     getenv("RX_VK_WATCH_CPU") || getenv("RX_VK_VERIFY_DIM");
    if (page_watches || VirtualAlloc(ptr, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) != ptr)
        for (uint8_t* q = ptr; q < ptr + bytes;) {
            MEMORY_BASIC_INFORMATION mi{};
            if (VirtualQuery(q, &mi, sizeof mi) != sizeof mi) break;
            uint8_t* end = std::min((uint8_t*)mi.BaseAddress + mi.RegionSize, ptr + bytes);
            if (mi.State == MEM_RESERVE) VirtualAlloc(q, (size_t)(end - q), MEM_COMMIT, PAGE_READWRITE);
            q = end;
        }
#ifdef __linux__
    if (import_dma_buf(lo, hi, p)) {
        t1 = t2 = t3 = t4 = clk::now();
        return true;
    }
#endif
    // (the host allocations' memory type: the same for every pointer, asked once)
    static uint32_t type = UINT32_MAX;
    if (type == UINT32_MAX) {
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr,
                                                &hp) != VK_SUCCESS) {
            import_remember_failure(lo, hi);
            return false;
        }
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent)
            if ((hp.memoryTypeBits >> i & 1) &&
                (dev.memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                type = i;
        if (type == UINT32_MAX) {
            import_remember_failure(lo, hi);
            return false;
        }
    }
    t1 = clk::now();
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.size = bytes;
    bi.usage = kUsage;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &p.buffer) != VK_SUCCESS) {
        import_remember_failure(lo, hi);
        return false;
    }
    t2 = clk::now();
    VkImportMemoryHostPointerInfoEXT im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    im.pHostPointer = ptr;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = bytes;
    ai.memoryTypeIndex = type;
    const VkResult allocated = vkAllocateMemory(dev.device, &ai, nullptr, &p.memory);
    t3 = clk::now();
    const VkResult bound = allocated == VK_SUCCESS ? vkBindBufferMemory(dev.device, p.buffer, p.memory, 0) : allocated;
    if (bound != VK_SUCCESS) {
        import_result = bound;
        destroy_import_buffer(p.buffer);
        if (allocated == VK_SUCCESS) vkFreeMemory(dev.device, p.memory, nullptr);
        p.buffer = VK_NULL_HANDLE, p.memory = VK_NULL_HANDLE;
        import_remember_failure(lo, hi);
        return false;
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.buffer;
    p.address = vkGetBufferDeviceAddress(dev.device, &da);
    if (env_on("RX_VK_LOG_ADDR"))   // (diagnostics: which buffer a device fault's address is in)
        logf("vk: addr import %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)p.address, (unsigned long long)(p.address + (hi - lo)));
    t4 = clk::now();
    return true;
}

} // namespace

// (hookshot) guest memory's page table, for the pointers shaders follow (spirv_emit_context.cpp): a page's device address, 0 for none. Written as
// pools come and go, before the submissions that can reach them
uint64_t* guest_pages = nullptr;
uint64_t guest_pages_address = 0;
VkBuffer guest_pages_buffer = VK_NULL_HANDLE;
// (hookshot, 11-07) A change to the table made while a submission is recorded goes into that submission's upload commands, after
// the mirror copies it records there, instead of into the table at once: the table is read by the GPU, and work submitted
// before (still running, or queued) read the new entries early. A pool's range zeroed at its merge read as unmapped (zero
// counts: the culling's foliage gone for a frame), and a mirror made but not yet filled read as garbage: the foliage
// culling 53caac5f looped on a count from it until Windows reset the driver (the device lost of ~1 start in 5, always
// after pool merges, 2 s after the GPU stopped)
struct TableRun { uint64_t first, count, address; };
thread_local std::vector<TableRun> table_pending;
thread_local bool table_defer = false;
void guest_pages_set(uint64_t lo, uint64_t hi, uint64_t address) {
    if (!guest_pages) return;
    // (on by default since 11-08: 14 of 14 starts without the device lost, against ~1 in 7 written at once; one-frame flashes
    // 0-1 bright / 4 dark in two 40 s Kokiri runs, as many as without it, and the same fps. One earlier run with 21 dark ones was
    // the runs' spread, not this. RX_VK_TABLE_NOW=1: written at once, as before)
    static const bool now = env_on("RX_VK_TABLE_NOW");
    // (two windows, as spirv_emit_context.cpp reads them: 8 GB from 0x1000000000, then 4 GB from 0x7f00000000)
    const struct { uint64_t base, bytes, first; } windows[] = {{0x1000000000ull, 8ull << 30, 0}, {0x7f00000000ull, 4ull << 30, (8ull << 30) >> 12}};
    for (const auto& w : windows) {
        const uint64_t a0 = std::max(lo, w.base) & ~0xFFFull, a1 = std::min(hi, w.base + w.bytes);
        if (a0 >= a1) continue;
        const uint64_t first = w.first + ((a0 - w.base) >> 12), count = (a1 - a0 + 0xFFF) >> 12;
        const uint64_t at = address ? address + (a0 - lo) : 0;
        if (table_defer && !now) {
            table_pending.push_back({first, count, at});
            continue;
        }
        for (uint64_t i = 0; i < count; i++) guest_pages[first + i] = at ? at + (i << 12) : 0;
    }
}
void memory_table_begin() {
    if (!table_pending.empty()) {   // (left by a recording that never ended: written now)
        table_defer = false;
        for (const TableRun& r : table_pending)
            for (uint64_t i = 0; i < r.count; i++) guest_pages[r.first + i] = r.address ? r.address + (i << 12) : 0;
        table_pending.clear();
    }
    table_defer = guest_pages != nullptr;
}
// (at the end of the recording's uploads: the changes, the last one for each page, as transfers into the table; the
// upload command buffer's barriers order them after the work submitted before and before this submission's)
bool memory_table_flush() {
    table_defer = false;
    if (table_pending.empty()) return false;
    std::vector<std::pair<uint64_t, uint64_t>> pages;   // (page, value), in the order they were set
    for (const TableRun& r : table_pending)
        for (uint64_t i = 0; i < r.count; i++) pages.push_back({r.first + i, r.address ? r.address + (i << 12) : 0});
    table_pending.clear();
    std::stable_sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> run;
    VkCommandBuffer cmd = upload_command_buffer();
    GprofSpan gp(cmd, "ptable update (guest page table)");
    gp.items = 0;
    uint64_t run_first = 0;
    const auto emit = [&] {
        if (!run.empty()) vkCmdUpdateBuffer(cmd, guest_pages_buffer, run_first * 8, run.size() * 8, run.data()), gp.items++, gp.bytes += run.size() * 8;
        run.clear();
    };
    for (size_t i = 0; i < pages.size(); i++) {
        if (i + 1 < pages.size() && pages[i + 1].first == pages[i].first) continue;   // (a later change of the same page wins)
        if (!run.empty() && (pages[i].first != run_first + run.size() || run.size() == 8192)) emit();
        if (run.empty()) run_first = pages[i].first;
        run.push_back(pages[i].second);
    }
    emit();
    return true;
}

uint64_t memory_guest_pages() {
    static std::mutex mu;
    std::lock_guard lk(mu);
    static bool tried = false;
    if (tried) return guest_pages_address;
    tried = true;
    if (env_on("RX_VK_NO_FOLLOW")) return 0;   // (on: Kokiri's lighting. The menus' blank text was the rule in graphics stages, now compute only)
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    void* mapped = nullptr;
    const uint64_t bytes = ((8ull << 30 >> 12) + (4ull << 30 >> 12)) * 8;
    if (!create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       true, b, a, &mapped, true) || !mapped) {
        logf("vk: no page table of guest memory: pointers in no bound buffer read as 0");
        return 0;
    }
    std::memset(mapped, 0, bytes);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    guest_pages_address = vkGetBufferDeviceAddress(dev.device, &da);
    guest_pages_buffer = b;
    std::lock_guard pk(pools_mu);
    guest_pages = (uint64_t*)mapped;
    for (auto& kv : imported) guest_pages_set(kv.second.lo, kv.second.hi, kv.second.address);
    logf("vk: page table of guest memory at %#llx (%zu pools)", (unsigned long long)guest_pages_address, imported.size());
    return guest_pages_address;
}

bool memory_init() {
    if (!dev.external_memory_host) {
        logf("vk: no VK_EXT_external_memory_host: the guest memory cannot be imported (needed for now)");
        return false;
    }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    hp.pNext = &p11;
    p11.pNext = &p13;
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    align = std::max<uint64_t>(hp.minImportedHostPointerAlignment, 4096);
    max_import_size = std::min(p11.maxMemoryAllocationSize, p13.maxBufferSize);
    logf("vk: guest memory imported in place (alignment %llu)", (unsigned long long)align);
    return true;
}

/// Refresh the guest's pool table (no Vulkan work: pools are imported as they are used)
std::atomic<int64_t> memsync_lock_wait_ns{0};
std::atomic<int64_t> memsync_part_ns[3];
void memory_sync() {
    static std::mutex sync_mu;   // (called from every thread that touches memory)
    const auto w0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> sync_lk(sync_mu);
    struct Waited {   // (diagnostics: only the lock waits, added up; nothing else changes)
        std::chrono::steady_clock::time_point t;
        ~Waited() {}
    } waited{w0};
    memsync_lock_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waited.t).count();
    const uint32_t g = host->maps_generation();
    if (g == generation) return;
    generation = g;
    static thread_local std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<GuestPool> next;
    next.reserve(n > 0 ? (size_t)n : 0);
    for (int i = 0; i < n; i++) {
        if (!list[i][1]) continue;
        GuestPool p;
        p.lo = list[i][0] & ~(align - 1);
        p.hi = (list[i][0] + list[i][1] + align - 1) & ~(align - 1);
        next.push_back(p);
    }
    std::sort(next.begin(), next.end(), [](const GuestPool& a, const GuestPool& b) { return a.lo < b.lo; });
    const auto w1 = std::chrono::steady_clock::now();
    std::lock_guard lk(pools_mu);
    memsync_lock_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w1).count();
    guest_pools.swap(next);
    guest_pools_gen.fetch_add(1, std::memory_order_relaxed);
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8) logf("vk: %zu pools mapped", guest_pools.size());
}

// ---- page write generations ----
// Two levels over the guest's 512 GB (2^27 pages of 4 KB): chunks of 64K page generations made when a page of the
// chunk is first stamped, and the newest generation of each 2 MB region (512 pages), which lets a check skip a big
// texture's clean regions without looking at their pages. 0: never written.
namespace {
constexpr uint64_t kPageShift = 12, kChunkPages = 1u << 16, kRegionPages = 512;
constexpr uint64_t kPages = 1ull << 27;
std::vector<std::unique_ptr<uint32_t[]>> gen_chunks(kPages / kChunkPages);
std::vector<uint32_t> region_gen(kPages / kRegionPages);
// (and the generation every page of a region was written at, at least: a large stamp (a storage buffer a dispatch may
// write, hundreds of MB) marks whole regions, not 512 pages each; it took dispatches 8-150 ms)
std::vector<uint32_t> region_full(kPages / kRegionPages);
inline uint32_t load_u32(uint32_t& x) {
    return std::atomic_ref<uint32_t>(x).load(std::memory_order_acquire);
}
inline void store_u32(uint32_t& x, uint32_t v) {
    std::atomic_ref<uint32_t>(x).store(v, std::memory_order_release);
}
inline uint32_t page_gen(uint64_t p) {
    const auto& c = gen_chunks[p / kChunkPages];
    return std::max(c ? load_u32(c[p % kChunkPages]) : 0u, load_u32(region_full[p / kRegionPages]));
}
uint32_t gen_now = 1;
uint64_t pages_collected = 0;

void stamp_page(uint64_t p, uint32_t g) {
    if (p >= kPages) return;
    auto& c = gen_chunks[p / kChunkPages];
    if (!c) {
        c.reset(new uint32_t[kChunkPages]);
        std::fill(c.get(), c.get() + kChunkPages, 0u);
    }
    store_u32(c[p % kChunkPages], g);
    region_gen[p / kRegionPages] = g;
}
} // namespace

namespace { void cap_view_bump(); }   // (defined with cap_view_gen below, in this file's anonymous namespace)
const std::vector<std::pair<uint64_t, uint64_t>>& capture_ranges_merged();
void shadow_written(const uint32_t* pages, int64_t n);   // (the write-back's diff, below: pages the captures took)
void shadow_written_collected(const uint32_t* pages, int64_t n);   // (the write-back's diff, below: pages the collection found)
void memory_collect_written() {
    uint32_t* pages = nullptr;
    const int64_t n = host->collect_written(&pages);
    if (n > 0) {
        const uint32_t g = ++gen_now;
        // (the captured ranges once a call: a lock, a pool-list refresh and three searches for every dirty page cost the
        // replay thousands of lock cycles a submit, the cost that retired gpu_pages' lock below)
        const auto& owned = capture_ranges_merged();
        const auto in_capture = [&owned](uint64_t guest) {
            auto it = std::upper_bound(owned.begin(), owned.end(), std::make_pair(guest, ~uint64_t(0)));
            return it != owned.begin() && guest < std::prev(it)->second;
        };
        for (int64_t i = 0; i < n; i++) {
            const uint64_t guest = (uint64_t)pages[i] << kPageShift;
            if (!owned.empty() && in_capture(guest)) {   // (written after its submit's capture: the next capture takes it, as it is then)
                uint64_t avail = 0;
                if (const uint8_t* q = host->gpu_ptr(guest, &avail)) host->mark_written(q, 4096);
                continue;
            }
            stamp_page(pages[i], g);
        }
        pages_collected += (uint64_t)n;
        cap_view_bump();
        // (hookshot) every page the CPU wrote, captured later or not: unknown to the write-back diff's shadows before this
        // recording's write-back. With RX_VK_CAPTURE=0 this is the only report of the game's writes (capture_make returns
        // first); with captures, it is the pages written after this submission's capture (PR 1109's review)
        shadow_written_collected(pages, n);
    }
    if (pages) host->free_pages(pages);
}
uint32_t memory_generation() {
    return gen_now;
}
bool memory_written_since(uint64_t guest, uint64_t size, uint32_t gen) {
    if (!size) return false;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        if (load_u32(region_full[r]) > gen) return true;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++)
            if (page_gen(p) > gen) return true;
    }
    return false;
}
void memory_written_runs(uint64_t guest, uint64_t size, uint32_t gen, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    if (!size) return;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++) {
            if (page_gen(p) <= gen) continue;
            const uint64_t lo = std::max(p << kPageShift, guest) - guest;
            const uint64_t hi = std::min((p + 1) << kPageShift, guest + size) - guest;
            if (!out.empty() && out.back().second == lo) out.back().second = hi;
            else out.push_back({lo, hi});
        }
    }
}
void memory_drop_shadow(uint64_t guest, uint64_t size);
// (a GPU write: RX_VK_GPU_PAGES=1 notes the pages, and the mirror syncs that copy the CPU's version of one over the
// mirror are counted (the GPU's bytes in that page replaced by what guest memory or a capture held: a page the CPU
// and the GPU both write). ~1M a race, no effect seen; off by default: its lock on every dirty page cost the replay)
std::mutex gpu_pages_mu;
std::unordered_map<uint32_t, uint32_t> gpu_pages;   // page -> the generation of the GPU's last write
int64_t st_sync_gpu_pages, st_sync_gpu_live;
bool gpu_pages_on() {
    static const bool v = env_on("RX_VK_GPU_PAGES");
    return v;
}
void memory_stamp_gpu(uint64_t guest, uint64_t size) {
    if (!size) return;
    memory_stamp(guest, size);
    if (!gpu_pages_on()) return;
    std::lock_guard lk(gpu_pages_mu);
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    if (p1 - p0 > 4096) return;   // (big copies: not tracked)
    for (uint64_t p = p0; p <= p1; p++) gpu_pages[(uint32_t)p] = gen_now;
    if (gpu_pages.size() > 2000000) gpu_pages.clear();
}
bool gpu_page(uint64_t page) {
    if (!gpu_pages_on()) return false;
    std::lock_guard lk(gpu_pages_mu);
    return gpu_pages.count((uint32_t)page) != 0;
}

// (RX_VK_PROTECT_BARE=1, an experiment for the one-frame foliage flicker and the floating trees) The ranges a shader writes
// through a table pointer (vk_draw.cpp: a bare entry, only its first 64 bytes marked written) are GPU-written pages the
// mirror syncs know nothing of: a CPU write anywhere on such a page copied the CPU's whole page over the GPU's records
// (NX2navi found the same: "trees drawn from records lost that way stretched into columns reaching the sky", and culled
// foliage drew garbage for a frame). Pages noted here keep the GPU's version when the CPU also wrote them since the
// pool's last sync. Nothing is copied back to guest memory (that overwrote the CPU's own data beside them once).
std::mutex prot_mu;
std::unordered_map<uint32_t, uint32_t> prot_pages;   // page -> gen_now when a shader last wrote through it
std::atomic<int64_t> st_prot_kept{0};
bool protect_bare_on() {
    static const bool v = getenv("RX_VK_PROTECT_BARE") && getenv("RX_VK_PROTECT_BARE")[0] == '1';
    return v;
}
bool prot_page_since(uint64_t page, uint32_t synced) {
    std::lock_guard lk(prot_mu);
    const auto it = prot_pages.find((uint32_t)page);
    return it != prot_pages.end() && it->second >= synced;
}
void memory_protect_gpu(uint64_t guest, uint64_t size) {
    if (!size || !protect_bare_on()) return;
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    if (p1 - p0 > 16384) return;   // (64 MB at most: a guess past that is not the GPU's records)
    std::lock_guard lk(prot_mu);
    for (uint64_t p = p0; p <= p1; p++) prot_pages[(uint32_t)p] = gen_now;
    if (prot_pages.size() > 4000000) prot_pages.clear();
}

void memory_stamp(uint64_t guest, uint64_t size) {
    if (!size) return;
    {   // (RX_VK_LOG_STAMP=<lo>:<hi> (hex guest addresses): the stamps there, 300 lines, with their callers)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_LOG_STAMP")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && guest < range.second && guest + size > range.first && logged++ < 300)
            logf("vk: stamp %#llx +%#llx (gen %u) from %p", (unsigned long long)guest, (unsigned long long)size, gen_now + 1,
                 __builtin_return_address(0));
    }
    memory_drop_shadow(guest, size);   // (a write the capture did not take: guest memory is newer than its copy)
    const uint32_t g = ++gen_now;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t p = p0; p <= p1;) {
        if (p % kRegionPages == 0 && p + kRegionPages - 1 <= p1) {   // (a whole region)
            store_u32(region_full[p / kRegionPages], g);
            region_gen[p / kRegionPages] = g;
            p += kRegionPages;
            continue;
        }
        stamp_page(p++, g);
    }
    cap_view_bump();   // (a replay read's cached page is this generation's answer, not the one from before the write)
}
uint64_t memory_pages_collected() {
    return pages_collected;
}

// ---- submit-time capture ----
// MKW writes some of a frame's data in place right after its submit (the console's GPU has read it by then). The
// replay records a submit later, and the mirrors were filled from guest memory as it was then: a frame's terrain
// tiles read the next one's constants and flashed as walls. The pages are now copied as they are at the submit.
namespace {
struct Capture {
    std::vector<uint32_t> pages;     // (written since the last submit)
    std::vector<uint32_t> copied;    // (those copied, in order)
    std::vector<uint32_t> slots;     // (and where each is in the buffer: 4 KB a slot, the page's index in pages)
    // (copied by the submitting thread straight into the buffer the GPU copies the mirrors' pages from, in cached host
    // memory: the replay's own copy of them into one was ~0.6 ms a submit once the game's small pools were captured)
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t bsize = 0;
    size_t nbytes = 0;
    std::vector<uint32_t> late;      // (indices into pages: copied again by the replay once late_value is done)
    uint64_t late_value = 0;
    // (hookshot) "shadow staging": the texture pages of this capture, copied from guest memory into ordinary cached RAM too, for
    // take_capture's texture shadow to read instead of the buffer above (often write-combined here: CPU reads ~0.3 GB/s).
    // sidx[i] = the staging slot of pages[i] (4 KB a slot), or -1: that page is read from the buffer. Empty sidx: no staging.
    std::unique_ptr<uint8_t[]> stage;
    size_t stage_bytes = 0;
    std::vector<int32_t> sidx;
    bool buf_cached = false;   // (its capture buffer's memory is HOST_CACHED: for the staging log line)
};
// (the staging blocks, kept for the next capture: a fresh 20 MB block is ~5000 page faults on the game's thread)
std::mutex stage_mu;   // (a leaf lock)
std::vector<std::pair<std::unique_ptr<uint8_t[]>, size_t>> stage_free;
size_t stage_free_bytes = 0;
bool stage_on() {   // (RX_VK_SHADOW_STAGE=0 / off / false / no: the shadow reads the capture buffer, as before)
    static const bool on = [] { const char* e = getenv("RX_VK_SHADOW_STAGE"); return !e || !env_value_off(e); }();   // (on unless an off value: env_value_off)
    return on;
}
bool stage_get(Capture& c, size_t need) {   // (a block of at least need bytes into c: a kept one, else a new one)
    {
        std::lock_guard lk(stage_mu);
        size_t best = stage_free.size();
        for (size_t i = 0; i < stage_free.size(); i++)
            if (stage_free[i].second >= need && (best == stage_free.size() || stage_free[i].second < stage_free[best].second)) best = i;
        if (best < stage_free.size()) {
            c.stage = std::move(stage_free[best].first), c.stage_bytes = stage_free[best].second;
            stage_free_bytes -= c.stage_bytes;
            stage_free.erase(stage_free.begin() + best);
            return true;
        }
    }
    const size_t sz = (need + (1u << 20) - 1) & ~size_t((1u << 20) - 1);   // (rounded to 1 MB: blocks of near sizes fit each other)
    c.stage.reset(new (std::nothrow) uint8_t[sz]);
    c.stage_bytes = c.stage ? sz : 0;
    return c.stage != nullptr;
}
void stage_put(Capture& c) {   // (the block back to the kept ones, at most four and 96 MB; else freed)
    c.sidx.clear();
    if (!c.stage) return;
    std::lock_guard lk(stage_mu);
    static const bool low_spec = env_on("RX_VK_LOW_SPEC");   // (RAM the tight resource there: no block kept)
    if (!low_spec && stage_free.size() < 4 && stage_free_bytes + c.stage_bytes <= (96ull << 20)) {
        stage_free_bytes += c.stage_bytes;
        stage_free.emplace_back(std::move(c.stage), c.stage_bytes);
    }
    c.stage.reset(), c.stage_bytes = 0;
}
std::mutex cap_mu;
std::deque<std::unique_ptr<Capture>> captures;
std::vector<std::unique_ptr<Capture>> spare;   // (taken ones, their buffers kept)
// ---- the captures' pages, for the mirror syncs ----
// A capture takes the pages written since the last submit; a pool its submission does not bind is synced at a later
// one, whose capture does not have those pages: copied from guest memory as it was at that replay, by when MKW had
// rewritten them for a later frame (it writes several submits ahead of the replay), a frame's objects drew with
// another one's transforms and instance data (the one-frame flashes). Each page a capture took is in a table by page:
// the capture (the generation its take stamped the pages with) and the page's slot in that capture's buffer. The
// entry holds while the page's generation is still that one (a later capture re-takes the page, a write the captures
// did not take (memory_stamp) makes guest memory newer) and the capture's buffer is kept (the last kKeep captures, and
// until the work that copies from it is done); a sync copies a dirty page from there, else from guest memory.
// (A flat table and generations: the hash maps' node allocations took ~0.7 ms a submit, the replay's.)
// (RX_VK_SYNC_LIVE=1: dirty pages not in their submission's capture read from guest memory, as before)
struct CapEntry { uint32_t gen, slot; };
constexpr uint64_t kCapChunkPages = 1u << 16;   // (a chunk of the table: 256 MB of guest memory, made when first used)
std::vector<std::unique_ptr<CapEntry[]>> cap_table;
inline CapEntry cap_entry(uint64_t page) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size() || !cap_table[c]) return {0, 0};
    return cap_table[c][page % kCapChunkPages];
}
inline void cap_entry_set(uint64_t page, CapEntry e) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size()) cap_table.resize(c + 1);
    if (!cap_table[c]) {
        cap_table[c].reset(new CapEntry[kCapChunkPages]);
        std::memset(cap_table[c].get(), 0, kCapChunkPages * sizeof(CapEntry));
    }
    cap_table[c][page % kCapChunkPages] = e;
}
// (each kept capture's copy on the GPU, by its generation)
struct CapBuf { uint64_t value; VkBuffer buffer; VmaAllocation alloc; uint64_t size; void* map; uint64_t last_use = 0; };
std::map<uint32_t, CapBuf> cap_bufs;
uint64_t cap_bytes = 0;
constexpr size_t kKeep = 24;
constexpr uint64_t kCapBytesMax = 768ull << 20;   // (and no more than this in them: loads capture hundreds of MB)
// (retired buffers, for the next captures: making and freeing a 5 MB buffer every submit cost the replay too)
struct FreeBuf { VkBuffer buffer; VmaAllocation alloc; void* map; uint64_t size; };
std::vector<FreeBuf> cap_free;
uint64_t cur_cap_value = 0;   // (the submission being recorded: what its syncs' copies wait for)
uint32_t cur_cap_gen = 0;     // (its capture's generation)
bool sync_live() {
    static const bool v = env_on("RX_VK_SYNC_LIVE");
    return v;
}
std::atomic<bool> cap_shutting_down{false};     // (set under cap_free_mu when the process starts to exit: after it no worker touches cap_free)
std::mutex cap_free_mu;   // (the submitting threads take buffers from cap_free, the replay gives them back)
uint64_t cap_free_bytes = 0;   // (cap_free_mu)
// (the spare buffers: 12 and, on a card of 6 GB or less, 32 MB of them; RX_VK_CAP_SPARE_MB=<n>: that many, 0: as before.
// After a load the retired ones, 40-60 MB each, stayed as spares for good)
uint64_t cap_spare_max() {
    static const uint64_t v = [] {
        if (const char* e = getenv("RX_VK_CAP_SPARE_MB")) return e[0] && atoll(e) ? (uint64_t)atoll(e) << 20 : ~0ull;
        uint64_t used = 0, allowed = 0;
        return vram_usage(&used, &allowed) && allowed <= (6ull << 30) + (512ull << 20) ? 32ull << 20 : ~0ull;
    }();
    return v;
}
void cap_buf_free(VkBuffer buffer, VmaAllocation alloc, void* map, uint64_t size, bool from_worker = false) {
    std::lock_guard lk(cap_free_mu);
    if (from_worker && cap_shutting_down.load()) return;
    // (hookshot, RX 7800 XT: buffers up to 256 MB are kept for the next capture, not only up to 64 MB: a new one costs ~100 ms of
    // vkAllocateMemory on AMD's driver. RX_VK_CAPTURE_KEEP_OLD=1: as before)
    static const bool keep_old = env_on("RX_VK_CAPTURE_KEEP_OLD");
    if (cap_free.size() < 12 && size <= (keep_old ? 64ull << 20 : 256ull << 20) &&
        cap_free_bytes + size <= std::min<uint64_t>(cap_spare_max(), keep_old ? ~0ull : 768ull << 20)) {
        cap_free.push_back({buffer, alloc, map, size});
        cap_free_bytes += size;
    } else {
        destroy_buffer(buffer, alloc);
    }
}
void cap_buf_retire(std::map<uint32_t, CapBuf>::iterator it) {
    cap_bytes -= it->second.size;
    cap_buf_free(it->second.buffer, it->second.alloc, it->second.map, it->second.size);
    cap_bufs.erase(it);
}
// (a capture's buffer by its size class: 2, 4, 8, then 16 MB and up to the MB. It was never under 16 MB, for captures of ~1 MB:
// 25 kept ones were 413 MB of host-cached system memory, all resident, and three to a 64 MB block left 14 MB of each unused.
// RX_VK_CAP_MIN_MB=<n>: the smallest (2), 16: as before. From 16 MB each has a memory object of its own: RX_VK_CAP_BLOCKS=1: not)
uint64_t cap_buf_size(uint64_t need) {
    static const uint64_t min = (getenv("RX_VK_CAP_MIN_MB") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_CAP_MIN_MB"))) : 2u) << 20;
    uint64_t s = min;
    while (s < need && s < (16ull << 20)) s <<= 1;
    if (s >= need) return s;
    // (hookshot: above 16 MB in classes of 32, 64, 128 and 256 MB, so that a buffer given back fits the next capture)
    static const bool keep_old = env_on("RX_VK_CAPTURE_KEEP_OLD");
    if (!keep_old)
        for (const uint64_t cm : {32, 64, 128, 192, 256})   // (stutter: 192 added, a 130-158 MB capture took the 256 MB class)
            if ((uint64_t)cm << 20 >= need) return (uint64_t)cm << 20;
    return (need + (1u << 20) - 1) & ~((1ull << 20) - 1);
}
// (hookshot, RX_VK_CAP_PREWARM, on by default since the stutter fix; =0: off) The big capture buffers made ahead, on a worker thread. A capture of a scene load holds
// 16 to 256 MB of the pages the game wrote (texture storage), and its buffer was made on the game's thread, inside capture_make:
// AMD's driver commits and clears a host-visible memory object when it is allocated, ~0.8 ms a MB, and the log's isolated spikes
// ("memory sync ... capture 66.8", "capture 107.7", "capture 22.6", "capture 17.4": 5 in a session, each at a load, none in steady
// play) are that, in proportion to the class (128, 64 and 32 MB classes, and 128, 64 MB). The spares only come back from a capture
// retired past the 24 kept ones, of whatever size that one was, so a big class is rarely there when a load asks for it. With this
// a spare of each class of 32, 64 and 128 MB is made in the background once, and again as soon as one is taken.
bool cap_prewarm_on() {
    // (stutter: on by default now; RX_VK_CAP_PREWARM=0 turns it off. The log at the scene loads: "capture ... the buffer of 32/64/256 MB 13-41 ms")
    static const bool v = [] {
        const char* e = getenv("RX_VK_CAP_PREWARM");
        const bool on = !e || atoi(e) != 0;
        logf("vk: capture buffer prewarm (32, 64, 128 MB spares made on a worker) %s (RX_VK_CAP_PREWARM)", on ? "on" : "off");
        return on;
    }();
    return v;
}
std::atomic<int> cap_prewarm_busy{0};
std::atomic<int> cap_prewarm_class_busy[4]{};   // (32, 64, 128 and 192 MB spares being made)
void cap_exit_note() {
    std::lock_guard lk(cap_free_mu);   // (a worker inside cap_buf_free finishes first; one after this sees the flag under the same mutex)
    cap_shutting_down = true;
}
int cap_prewarm_index(uint64_t size) { return size <= (32ull << 20) ? 0 : size <= (64ull << 20) ? 1 : size <= (128ull << 20) ? 2 : size <= (192ull << 20) ? 3 : -1; }
void cap_spare_make(uint64_t size, int index) {
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    void* m = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    if (!cap_shutting_down.load() && create_buffer_cached(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, b, a, &m, !env_on("RX_VK_CAP_BLOCKS")) && m) {
        static std::atomic<int> told{0};
        if (told++ < 8)
            logf("vk: a %llu MB capture buffer made ahead in %.1f ms, off the game's thread (RX_VK_CAP_PREWARM)", (unsigned long long)(size >> 20),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        // (a spare, within the same limits as any other: past them it is let go. Not once the process is exiting: the list and the device
        // may be going, and the memory goes with the process)
        cap_buf_free(b, a, m, size, true);
    }
    cap_prewarm_class_busy[index]--;
    cap_prewarm_busy--;
}
/// A spare of at least `size` (32, 64 or 128 MB classes only) is there, or one of its class is being made (cap_free_mu held)
void cap_prewarm_class(uint64_t size) {
    if (!cap_prewarm_on()) return;
    const int index = cap_prewarm_index(size);
    if (index < 0 || size > cap_spare_max()) return;   // (not the 256 MB class, and not past what spares may hold)
    static const uint64_t kCls[4] = {32, 64, 128, 192};
    const uint64_t cls = kCls[index];
    const uint64_t bytes = cls << 20;
    for (const FreeBuf& f : cap_free)
        if (f.size >= bytes) return;
    if (cap_prewarm_class_busy[index].load() > 0 || cap_prewarm_busy.load() >= 3) return;   // (three at a time: each commits its memory)
    static const bool exit_hook = [] { return std::atexit(cap_exit_note) == 0; }();
    (void)exit_hook;
    cap_prewarm_busy++;
    cap_prewarm_class_busy[index]++;
    try {
        std::thread(cap_spare_make, bytes, index).detach();
    } catch (...) {   // (no thread: no spare, the capture goes on)
        cap_prewarm_class_busy[index]--;
        cap_prewarm_busy--;
    }
}
bool cap_buf_get(uint64_t need, VkBuffer& buffer, VmaAllocation& alloc, void*& map, uint64_t& size) {
    std::lock_guard lk(cap_free_mu);
    if (cap_prewarm_on()) {
        static bool first = true;
        if (first) {   // (the first capture: the classes a load asks for)
            first = false;
            for (uint64_t c : {32ull << 20, 64ull << 20, 128ull << 20, 192ull << 20}) cap_prewarm_class(c);
        }
    }
    size_t best = cap_free.size();
    for (size_t i = 0; i < cap_free.size(); i++)
        if (cap_free[i].size >= need && (best == cap_free.size() || cap_free[i].size < cap_free[best].size)) best = i;
    if (best < cap_free.size()) {
        buffer = cap_free[best].buffer, alloc = cap_free[best].alloc, map = cap_free[best].map, size = cap_free[best].size;
        cap_free_bytes -= size;
        cap_free.erase(cap_free.begin() + (ptrdiff_t)best);
        return map != nullptr;
    }
    size = cap_buf_size(need);
    map = nullptr;
    return create_buffer_cached(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, buffer, alloc, &map,
                                size >= (16ull << 20) && !env_on("RX_VK_CAP_BLOCKS")) && map;
}
std::vector<std::pair<uint64_t, uint64_t>> cap_ranges;   // (the mirrored pools the captures take, as of the last one)
std::mutex cap_ranges_mu;
uint64_t cap_ranges_ver = 1;   // (any of the captures' range lists changed: cap_ranges_mu)
// (and the game's pools up to RX_VK_CAPTURE_POOL_MB (default 64; 0: none) from when they are made, mirrored or not:
// pools are imported, and mirrored, at the replay's first binding of them, two presents behind the game, and the
// captures taken before that left them out. MK World moves its per-frame data to a new pool every so many frames:
// the new pool's mirror was filled and synced from the game's memory as it was at the replay, its next frames' data
// in it already, until a mirror made the slow way published the list again (seconds): shadows dark below a line,
// the picture cyan below one, in bursts every 30 or 60 frames. The bigger pools hold textures, whose storage the
// capture takes anyway.)
// (RX_VK_CAPTURE_EAGER=<n>: only for their first n captures. Off: pools made at the load are bound by the menus much
// later, and they flashed (30 in the main menu); and the pages the captures copy are almost all the mirrored pools'
// anyway)
std::vector<std::pair<uint64_t, uint64_t>> pool_cap_ranges;   // (merged; cap_ranges_mu)
uint32_t pool_cap_gen = 0;
uint64_t captures_made = 0, pool_cap_built = 0;              // (cap_ranges_mu)
std::unordered_map<uint64_t, uint64_t> pool_first_seen;     // (a pool's start -> the capture it was first seen at)
uint64_t capture_pool_max() {
    static const uint64_t v = (getenv("RX_VK_CAPTURE_POOL_MB") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_POOL_MB")) : 64) << 20;
    return v;
}
uint64_t eager_captures() {
    static const uint64_t v = getenv("RX_VK_CAPTURE_EAGER") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_EAGER")) : 0;
    return v;
}
void refresh_pool_ranges_locked() {   // (cap_ranges_mu held)
    if (!capture_pool_max()) return;
    const uint32_t g = host->maps_generation();
    if (g == pool_cap_gen && captures_made - pool_cap_built < 64) return;   // (and every 64: the old ones out)
    pool_cap_gen = g, pool_cap_built = captures_made;
    static std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<std::pair<uint64_t, uint64_t>> v;
    std::unordered_map<uint64_t, uint64_t> seen;
    seen.reserve((size_t)std::max(n, 0));
    for (int i = 0; i < n; i++) {
        if (!list[i][1] || list[i][1] > capture_pool_max() || list[i][0] >= (1ull << 40)) continue;
        auto it = pool_first_seen.find(list[i][0]);
        const uint64_t first = it != pool_first_seen.end() ? it->second : captures_made;
        seen[list[i][0]] = first;
        if (!eager_captures() || captures_made - first < eager_captures())
            v.push_back({list[i][0] & ~4095ull, (list[i][0] + list[i][1] + 4095) & ~4095ull});
    }
    pool_first_seen.swap(seen);
    cap_ranges_ver++;
    std::sort(v.begin(), v.end());
    std::vector<std::pair<uint64_t, uint64_t>> m;
    for (const auto& r : v) {
        if (!m.empty() && r.first <= m.back().second) m.back().second = std::max(m.back().second, r.second);
        else m.push_back(r);
    }
    pool_cap_ranges.swap(m);
}
bool capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_CAPTURE");
        const bool v = e ? atoi(e) != 0 : true;
        logf("vk: submit-time capture of written pages %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::atomic<bool> captures_seen{false};
std::atomic<uint64_t> cap_view_gen{1};   // (bumped when a capture is published or a page is stamped: cached reads go stale)
void cap_view_bump() { cap_view_gen.fetch_add(1, std::memory_order_release); }
double st_tk_all, st_tk_stamp, st_tk_copy, st_tk_late, st_tk_lock;   // (memory_take_capture's parts, ms)
int64_t st_tk_late_n;   // (the takes that waited for the GPU for their late pages)   // (a capture was ever made: the replay thread's; without it nothing owns pages)

// ---- texture storage in the capture ----
// The textures' storage the replay uploads from joins the capture, and what the capture took of it is kept (the
// shadow) for the uploads to read instead of guest memory: the storage as it was at its submit. MKW writes its terrain's
// height tiles (68 x 68 RGBA16, at new addresses every frame) for a frame and then rewrites them right after the
// submit; uploaded when the replay recorded the submit, from guest memory as it was by then, a frame's tiles could take
// another's heights. Measured at a snowy spot it is rare: of 107,000 pages the uploads read from the copies, 13 had
// been rewritten since their submit. In a Grand Prix it is what flashed: ~100 one-frame flashes a minute (giant
// stretched shapes over most of the picture, black or fog-coloured bands, a 30-frame rhythm), 9-18 rewritten pages of
// 800,000, and none with it on. On by default now that the guarded copy holds (a call, not inlined). (A page the game
// wrote and then freed is still marked: the copy commits it again, zeros, on first touch (commit_on_touch), which is
// what the mirrors must see of it; clearing the marks at the free lost that, and a Grand Prix loaded to a black
// screen.) RX_VK_TEX_CAPTURE=0: off.
bool tex_capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_TEX_CAPTURE");
        const bool v = (e ? atoi(e) != 0 : true) && capture_on();
        logf("vk: texture storage in the submit-time capture %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::vector<std::pair<uint64_t, uint64_t>> tex_cap_ranges;   // (texture storage the capture takes, merged; cap_ranges_mu)
std::mutex shadow_mu;   // (after pools_mu, before cap_ranges_mu)
struct ShadowPage {
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;   // (the capture that took it)
};
std::unordered_map<uint32_t, ShadowPage> shadow;   // guest page -> its 4 KB as the last capture taken had it
std::unordered_map<uint32_t, uint32_t> shadow_regions;   // (2 MB region -> its pages in the shadow)
std::atomic<size_t> shadow_n{0};
uint64_t shadow_seq = 0;   // (captures taken)
// (256 MB: past it the pages no capture took in the last 64 go, the oldest first: those the game rewrites every frame
// stay; if none can go, a page is read from guest memory as before)
constexpr size_t kShadowMax = 65536;
std::map<uint64_t, std::pair<uint64_t, uint64_t>> tex_spans;   // lo -> (hi, the present a texture was last uploaded from it)
std::vector<std::pair<uint64_t, uint64_t>> tex_merged;          // (tex_spans merged; shadow_mu)
bool tex_spans_changed = false;
uint64_t tex_merged_bytes = 0;

bool in_tex_merged(uint64_t guest) {   // (shadow_mu held)
    auto it = std::upper_bound(tex_merged.begin(), tex_merged.end(), std::make_pair(guest, ~uint64_t(0)));
    return it != tex_merged.begin() && guest < std::prev(it)->second;
}
void shadow_erase(uint32_t page) {   // (shadow_mu held)
    if (!shadow.erase(page)) return;
    auto r = shadow_regions.find(page >> 9);
    if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
    shadow_n--;
}
void shadow_drop_range(uint64_t lo, uint64_t hi) {   // (shadow_mu held)
    if (hi <= lo) return;
    for (uint64_t r = lo >> 21; r <= (hi - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(lo >> 12, r << 9), p1 = std::min((hi + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) shadow_erase((uint32_t)p);
    }
}
/// (the spans unused for 1800 presents dropped with their shadow pages; the rest published for the capture)
void publish_tex_spans(uint64_t frame) {
    std::lock_guard lk(shadow_mu);
    // (a span ages by the present: looked over once a present, not at each of its ~20 submissions, which walked
    // the ~300 spans every time, 1 ms a frame on a Steam Deck)
    static uint64_t swept = ~0ull;
    if (frame != swept) {
        swept = frame;
        for (auto it = tex_spans.begin(); it != tex_spans.end();) {
            if (frame > it->second.second + 1800) {
                shadow_drop_range(it->first, it->second.first);
                it = tex_spans.erase(it);
                tex_spans_changed = true;
            } else {
                ++it;
            }
        }
    }
    if (!tex_spans_changed) return;
    tex_spans_changed = false;
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, e] : tex_spans) {
        if (!v.empty() && lo <= v.back().second) v.back().second = std::max(v.back().second, e.first);
        else v.push_back({lo, e.first});
    }
    tex_merged = v;
    tex_merged_bytes = 0;
    for (const auto& r : v) tex_merged_bytes += r.second - r.first;
    std::lock_guard lk2(cap_ranges_mu);
    tex_cap_ranges.swap(v);
    cap_ranges_ver++;
}
} // namespace

void memory_note_texture_upload(uint64_t guest, uint64_t size) {
    if (!size || guest >= (1ull << 40) || size > (1ull << 40) || !tex_capture_on()) return;   // (hookshot: no wrap below)
    uint64_t lo, hi;
    {   // (the pool it is in, when that is at most 256 MB; else the 2 MB regions it spans)
        std::lock_guard lk(pools_mu);
        auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                                   [](uint64_t g, const GuestPool& p) { return g < p.lo; });
        if (it != guest_pools.begin() && guest >= std::prev(it)->lo && guest + size <= std::prev(it)->hi &&
            std::prev(it)->hi - std::prev(it)->lo <= (256ull << 20)) {
            lo = std::prev(it)->lo, hi = std::prev(it)->hi;
        } else {
            lo = guest & ~((2ull << 20) - 1), hi = (guest + size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
        }
    }
    const uint64_t frame = texture_frame_no();
    std::lock_guard lk(shadow_mu);
    auto [it, fresh] = tex_spans.try_emplace(lo, hi, frame);
    if (!fresh) {
        it->second.second = frame;
        if (it->second.first >= hi) return;
        it->second.first = hi;
    }
    tex_spans_changed = true;
}

/// A replay read of one page can reuse what the last read of that page decided, while the page's write generation and
/// the capture table are the ones that decision was made from.
bool submitted_page_reuse(uint64_t page, uint32_t page_gen, uint64_t view, uint64_t cached_page, uint32_t cached_gen,
                          uint64_t cached_view) {
    return cached_view != 0 && page == cached_page && page_gen == cached_gen && view == cached_view;
}
struct SubmittedPage {   // (a page no capture holds, as of pgen and view: the read from guest memory stands)
    uint64_t page = ~0ull;
    uint32_t pgen = 0;
    uint64_t view = 0;
};
// (hookshot) 32 of them a thread, by page: a handle program reads a uniform word on one page and a storage buffer word on
// another, and with one page kept nearly every read missed and took pools_mu (program_for's answer handles). A page a
// capture holds is not kept: its words are copied from the capture under pools_mu at each read, not the whole 4 KB page
// into a copy of the thread's own (that copy was a third of the replay's thread in Kokiri Forest on NVIDIA Linux)
constexpr int kSubmittedPages = 32;
thread_local SubmittedPage submitted_pages[kSubmittedPages];
// (RX_VK_SUBMITTED_PAGES=1: one slot, every page in it, as before #1762)
inline SubmittedPage& submitted_page_for(uint64_t page) {   // (a page and its neighbours, and pages 32 apart, in different slots)
    static const bool one = getenv("RX_VK_SUBMITTED_PAGES") && atoi(getenv("RX_VK_SUBMITTED_PAGES")) == 1;
    return submitted_pages[one ? 0 : (page + (page >> 5) * 13) & (kSubmittedPages - 1)];
}

bool memory_read_as_submitted(uint64_t gpu, void* dst, uint64_t n) {
    // (once a capture exists, every uniform word took pools_mu to ask whether its page was in one. A page in none keeps
    // that answer until a stamp or a new capture)
    if (!host->gpu_read(gpu, dst, n)) return false;
    if (!n || !capture_on() || !captures_seen.load(std::memory_order_relaxed)) return true;
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return true;
    const uint64_t page0 = guest >> kPageShift, page1 = (guest + n - 1) >> kPageShift;
    const uint64_t view = cap_view_gen.load(std::memory_order_acquire);
    SubmittedPage& submitted_page = submitted_page_for(page0);
    if (page0 == page1 && submitted_page_reuse(page0, page_gen(page0), view, submitted_page.page, submitted_page.pgen,
                                               submitted_page.view))
        return true;
    static std::atomic<int64_t> reads{0}, pages_from{0};
    int64_t from = 0;
    {
        std::lock_guard lk(pools_mu);
        for (uint64_t page = guest >> kPageShift; page < (guest + n + 4095) >> kPageShift; page++) {
            const CapEntry e = cap_entry(page);
            if (!e.gen || e.gen != page_gen(page)) continue;
            auto b = cap_bufs.find(e.gen);
            if (b == cap_bufs.end() || !b->second.map) continue;
            const uint64_t a = std::max(page << kPageShift, guest), z = std::min((page + 1) << kPageShift, guest + n);
            std::memcpy((uint8_t*)dst + (a - guest), (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096 + (a - (page << kPageShift)), z - a);
            from++;
        }
        if (page0 == page1 && from == 0) {   // (in no capture: guest memory stands for its next words too)
            submitted_page.page = page0;
            submitted_page.pgen = page_gen(page0);
            submitted_page.view = cap_view_gen.load(std::memory_order_relaxed);
        }
    }
    pages_from += from;
    if (++reads % 20000 == 0)
        logf("vk: replay reads of guest memory as submitted: %lld (%lld pages from the captures)", (long long)reads.load(),
             (long long)pages_from.load());
    return true;
}

uint64_t memory_capture_view() { return cap_view_gen.load(std::memory_order_acquire); }
uint32_t memory_page_stamp(uint64_t gpu) {
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return 0;
    return page_gen(guest >> kPageShift);
}

bool memory_has_shadow(uint64_t guest, uint64_t n) {
    if (!n || !shadow_n.load(std::memory_order_relaxed)) return false;
    std::lock_guard lk(shadow_mu);
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++)
            if (shadow.count((uint32_t)p)) return true;
    }
    return false;
}

/// (RX_VK_TEX_CAPTURE_OFF_FILE=<path> (A/B): while that file exists the uploads read guest memory as before)
bool overlay_off() {
    static const char* path = getenv("RX_VK_TEX_CAPTURE_OFF_FILE");
    if (!path) return false;
    static std::atomic<int64_t> next{0};
    static std::atomic<bool> off{false};
    const int64_t now = GetTickCount64();
    if (now >= next.load(std::memory_order_relaxed)) {
        next.store(now + 200, std::memory_order_relaxed);
        const bool v = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        if (v != off.exchange(v)) logf("vk: texture storage as submitted: %s (A/B file)", v ? "OFF" : "on");
    }
    return off.load(std::memory_order_relaxed);
}
std::atomic<int64_t> st_overlaid{0}, st_differed{0};   // (pages read from the shadow; of them, ones guest memory had changed)

const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, std::vector<uint8_t>& scratch) {
    if (!n || !shadow_n.load(std::memory_order_relaxed) || overlay_off()) return live;
    std::lock_guard lk(shadow_mu);
    bool any = false;
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) {
            auto it = shadow.find((uint32_t)p);
            if (it == shadow.end()) continue;
            if (!any) {
                any = true;
                if (scratch.data() != live) scratch.assign(live, live + n);
            }
            const uint64_t a = std::max(p << 12, guest), b = std::min((p + 1) << 12, guest + n);
            st_overlaid++;
            if (std::memcmp(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a)) st_differed++;
            std::memcpy(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a);
        }
    }
    static std::atomic<int64_t> served{0};
    if (any && served.fetch_add(1) % 2000 == 0)
        logf("vk: texture storage read as submitted (%lld times; %lld pages from the copies, %lld of them rewritten in "
             "guest memory since their submit; %zu pages kept, %zu spans, %llu MB)", (long long)served.load(),
             (long long)st_overlaid.load(), (long long)st_differed.load(), shadow.size(), tex_spans.size(),
             (unsigned long long)(tex_merged_bytes >> 20));
    return any ? scratch.data() : live;
}

void memory_drop_shadow(uint64_t guest, uint64_t size) {
    if (!size || !shadow_n.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(shadow_mu);
    shadow_drop_range(guest, guest + size);
}

// ---- (EvilBoyce) GPU results by page: the submission that last wrote a page into guest memory ----
// For invalidate (vk_replay.cpp): a CPU read of GPU results waits until the submission that writes them there is done, as
// the console's coherent memory has them once that work is. Guest page number -> the recording's timeline value (write-backs
// from a mirror, and small bindings written in place). RX_VK_NO_READBACK_WAIT=1: none
namespace {
std::mutex wb_pages_mu;
// (each page's value: the timeline value of its last write-back, 0 for never, in chunks of kChunkPages made when a page of
// the chunk is first written back, as the page generations above. Write-backs are of imported pools only, which lie in the
// GPU's windows, so few chunks are ever made. Values are never lowered or dropped: the readers (memory_writeback_value's
// "ever written back" and its value against the GPU's progress, and the capture's wait, the same compare) read one the GPU
// is past as "written back, nothing to wait for", whatever it is. tests/test_wb_pages.py)
struct WbPages {
    std::vector<std::unique_ptr<uint64_t[]>> chunks = std::vector<std::unique_ptr<uint64_t[]>>(kPages / kChunkPages);
    bool any = false;
    void set(uint64_t page, uint64_t value) {
        if (page >= kPages) return;
        auto& c = chunks[page / kChunkPages];
        if (!c) {
            c.reset(new uint64_t[kChunkPages]);
            std::fill(c.get(), c.get() + kChunkPages, 0ull);
        }
        c[page % kChunkPages] = value;
        any = true;
    }
    uint64_t get(uint64_t page) const {
        if (page >= kPages) return 0;
        const auto& c = chunks[page / kChunkPages];
        return c ? c[page % kChunkPages] : 0;
    }
    /// the largest value of pages first..last (0: none), and whether any had one; a chunk never made is skipped whole
    uint64_t max_in(uint64_t first, uint64_t last, bool* ever) const {
        uint64_t v = 0;
        for (uint64_t p = first; p <= last && p < kPages;) {
            const uint64_t end = (p / kChunkPages + 1) * kChunkPages;   // (the first page past p's chunk)
            const auto& c = chunks[p / kChunkPages];
            if (!c) {
                p = end;
                continue;
            }
            for (; p <= last && p < end; p++)
                if (const uint64_t pv = c[p % kChunkPages]) {
                    if (ever) *ever = true;
                    v = std::max(v, pv);
                }
        }
        return v;
    }
    bool empty() const { return !any; }
} wb_pages;
// (hookshot) and the part of that submission the write-back was recorded in (vk_replay.cpp's Recording::part), for a page
// written back from its mirror: a submission is recorded in parts of ~120 draws (maybe_split), each part's end copies back
// what the GPU wrote in it, and the CPU's read needs that part done, not the submission's last one. Hyrule Field: the
// readbacks waited 5.5 ms a frame for last parts, the GPU's whole frame. 0 for a page written in place (note_direct_write:
// no mirror; the later parts of its submission may still read or write it in guest memory, so the read waits for all of it).
// A page's part is replaced with its value, by every note: the two are of the same write
WbPages wb_parts;
void wb_pages_note(uint64_t guest_lo, uint64_t guest_hi, uint64_t value, uint64_t part) {
    static const bool off = env_on("RX_VK_NO_READBACK_WAIT");
    if (off || !value || guest_hi <= guest_lo) return;
    std::lock_guard lk(wb_pages_mu);
    for (uint64_t p = guest_lo >> kPageShift; p <= (guest_hi - 1) >> kPageShift; p++) {
        wb_pages.set(p, value);
        if (part || !wb_parts.empty()) wb_parts.set(p, part);
    }
}
} // namespace
/// The newest write-back value of the range's pages (0: none; *ever: whether any page was ever written back). With part
/// and whole: *part is the newest part that wrote any of them back from a mirror (0: none), *whole the newest value among
/// the pages with no part (written in place: their whole submission is what a read waits for; 0: none)
uint64_t memory_writeback_value(uint64_t guest, uint64_t size, bool* ever, uint64_t* part, uint64_t* whole) {
    uint64_t v = 0;
    if (ever) *ever = false;
    if (part) *part = 0;
    if (whole) *whole = 0;
    if (!size) return 0;
    std::lock_guard lk(wb_pages_mu);
    if (wb_pages.empty()) return 0;
    const uint64_t first = guest >> kPageShift, last = (guest + size - 1) >> kPageShift;
    v = wb_pages.max_in(first, last, ever);
    if (v && (part || whole)) {
        uint64_t newest = 0, in_place = 0;
        for (uint64_t p = first; p <= last && p < kPages;) {
            if (!wb_pages.chunks[p / kChunkPages]) {   // (a chunk never made: none of its pages, as max_in)
                p = (p / kChunkPages + 1) * kChunkPages;
                continue;
            }
            const uint64_t pv = wb_pages.get(p), pp = pv ? wb_parts.get(p) : 0;
            if (pp) newest = std::max(newest, pp);
            else if (pv) in_place = std::max(in_place, pv);
            p++;
        }
        if (part) *part = newest;
        if (whole) *whole = in_place;
    }
    return v;
}

/// A copy that fails instead of faulting on a page unmapped meanwhile (structured exception handling: the common case
/// costs a memcpy; ReadProcessMemory per page was ~2.3 us each). Not inlined, and the copy a call: without
/// -fasync-exceptions clang catches a fault in a __try block only across a call, and a 4 KB memcpy inlined into the
/// block (the per-page retry) faulted past the handler: the crash that kept the texture storage out of the capture.
/// (hookshot) The capture's copy with non-temporal stores (x86-64; opt-in, RX_VK_CAPTURE_NT=1; memcpy otherwise). A capture's
/// buffer comes from a pool that cycles through some thirty buffers (cap_bufs, cap_free): memory last written several
/// frames ago, in no cache by now. A plain store reads each line before it overwrites it (the read for ownership: the
/// copy asks for twice the memory traffic it needs), and the copy then stays in the caches, where nothing reads it
/// again: the GPU takes the buffer from memory, and ~28 MB a frame (Kokiri Forest: ~5 MB a submit, six submits) went
/// through the L3, pushing the game's own data out of it. A non-temporal store writes the line straight to memory
/// through the write-combining buffers: no read, nothing in the caches. Whole 64-byte lines only: the head up to the
/// destination's next line and the tail are plain copies. A fault on the source (a page unmapped meanwhile) leaves
/// through guarded_copy's handler as memcpy's does: not inlined, a call inside the __try block.
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((noinline)) void* copy_nt(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (n < 256) return std::memcpy(dst, src, n);
    if (const size_t head = (size_t)(0 - (uintptr_t)d) & 63) {
        std::memcpy(d, s, head);
        d += head, s += head, n -= head;
    }
    for (size_t lines = n >> 6; lines; lines--, d += 64, s += 64) {
        // (the source: written by the game this frame, read in order: a page ahead of the hardware's prefetch, which
        // stops at a page's end)
        _mm_prefetch((const char*)s + 512, _MM_HINT_T0);
#ifdef __AVX2__
        const __m256i a = _mm256_loadu_si256((const __m256i*)s), b = _mm256_loadu_si256((const __m256i*)(s + 32));
        _mm256_stream_si256((__m256i*)d, a);
        _mm256_stream_si256((__m256i*)(d + 32), b);
#else
        const __m128i a = _mm_loadu_si128((const __m128i*)s), b = _mm_loadu_si128((const __m128i*)(s + 16)),
                      c = _mm_loadu_si128((const __m128i*)(s + 32)), e = _mm_loadu_si128((const __m128i*)(s + 48));
        _mm_stream_si128((__m128i*)d, a);
        _mm_stream_si128((__m128i*)(d + 16), b);
        _mm_stream_si128((__m128i*)(d + 32), c);
        _mm_stream_si128((__m128i*)(d + 48), e);
#endif
    }
    _mm_sfence();   // (the stores in memory before whoever reads the buffer next: the replay's thread, the GPU)
    if (n & 63) std::memcpy(d, s, n & 63);
    return dst;
}
// (opt-in since 2026-11-10: four interleaved field runs on an RTX desktop (main + #1619) showed no lower capture time with
// it, 0.41-0.44 ms a submit either way, and more stutters: 3.43% and 4.22% of frames over 39-41 ms with it, 3.08% and 2.89%
// without; p99 61.9/65.8 ms against 58.5/54.7. RX_VK_CAPTURE_NT=1 turns it on, for CPUs where it helps)
bool capture_nt() {
    static const bool on = getenv("RX_VK_CAPTURE_NT") && atoi(getenv("RX_VK_CAPTURE_NT")) != 0;
    return on;
}
void* (*volatile guarded_memcpy)(void*, const void*, size_t) = capture_nt() ? copy_nt : std::memcpy;
/// (a copy that left at a fault did not reach its fence, and the next reader is another thread)
inline void copy_fence() {
    if (capture_nt()) _mm_sfence();
}
#else
void* (*volatile guarded_memcpy)(void*, const void*, size_t) = std::memcpy;
inline void copy_fence() {}
#endif
#ifdef _WIN32
__declspec(noinline) bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        guarded_memcpy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
#else
// (linux: the guest window is one mapping that stays readable, decommitted pages read as zero: nothing to fault on)
bool guarded_copy(void* dst, const void* src, size_t n) {
    guarded_memcpy(dst, src, n);
    return true;
}
#endif
// ---- a capture's copies: on the submitting thread and two helpers, four from 12 CPU threads (RX_VK_CAPTURE_THREADS=<n>, 0-4): ~9 MB of the
// game's dynamic data a submit, four submits a frame, ~1.4 ms on the game's thread each with one ----
// (a page written and then unmapped before the submit faulted a plain copy: ReadProcessMemory fails on it instead.
// VirtualQuery per page took ~1 ms each in this address space: 22 ms a submit for 18 pages) (runs of adjacent pages
// in one copy each, page by page only where a run fails; page i into slot i)
struct CopyPair { uint8_t* dst; const uint8_t* src; };   // (a 4 KB copy between two of our own buffers: copy_pairs)
struct CopyPart {
    const uint32_t* pages;
    int64_t i0, i1;
    uint8_t* out;
    uint8_t* ok;
    const CopyPair* pairs = nullptr;
    int* left = nullptr;   // (the caller's parts not done yet (copy_helpers.mu): a caller waits for its own, not another's too)
    uint8_t* stage = nullptr;       // (guest copies only: the pages with sidx[i] >= 0 are copied into stage + sidx[i] * 4096 first, and out from there)
    const int32_t* sidx = nullptr;
};
void copy_part(const CopyPart& w) {
    if (w.pairs) {
        for (int64_t i = w.i0; i < w.i1; i++) std::memcpy(w.pairs[i].dst, w.pairs[i].src, 4096);
        return;
    }
    uint8_t* const base = (uint8_t*)host->guest_base();   // (guest addresses: no GPU translation per page)
    const uint64_t gsize = host->guest_size();
    for (int64_t i = w.i0; i < w.i1;) {
        const uint64_t ga = (uint64_t)w.pages[i] << kPageShift;
        if (ga + 4096 > gsize) { i++; continue; }
        int64_t j = i + 1;
        while (j < w.i1 && w.pages[j] == w.pages[j - 1] + 1 && ((uint64_t)w.pages[j] << kPageShift) + 4096 <= gsize) j++;
        // (a run in out alone; a run of staged pages: guest memory into the staging first, then out from the staging, so the two
        // hold the same bytes even if the game writes the page meanwhile; a page that fails is ok = 0 in both and not used)
        auto plain = [&](int64_t a, int64_t b) {
            if (guarded_copy(w.out + (uint64_t)a * 4096, base + ((uint64_t)w.pages[a] << kPageShift), (SIZE_T)(b - a) * 4096)) {
                std::memset(w.ok + a, 1, (size_t)(b - a));
            } else {
                for (int64_t q = a; q < b; q++)
                    w.ok[q] = guarded_copy(w.out + (uint64_t)q * 4096, base + ((uint64_t)w.pages[q] << kPageShift), 4096);
                copy_fence();
            }
        };
        auto staged = [&](int64_t a, int64_t b) {
            uint8_t* const st = w.stage + (uint64_t)w.sidx[a] * 4096;
            if (guarded_copy(st, base + ((uint64_t)w.pages[a] << kPageShift), (SIZE_T)(b - a) * 4096)) {
                std::memcpy(w.out + (uint64_t)a * 4096, st, (size_t)(b - a) * 4096);
                std::memset(w.ok + a, 1, (size_t)(b - a));
            } else {
                for (int64_t q = a; q < b; q++) {
                    uint8_t* const sq = w.stage + (uint64_t)w.sidx[q] * 4096;
                    const bool good = guarded_copy(sq, base + ((uint64_t)w.pages[q] << kPageShift), 4096);
                    if (good) std::memcpy(w.out + (uint64_t)q * 4096, sq, 4096);
                    w.ok[q] = good;
                }
                copy_fence();
            }
        };
        if (!w.sidx) {
            plain(i, j);
        } else {
            for (int64_t q = i; q < j;) {   // (split at the staged / not staged edges; staged slots are consecutive for adjacent pages)
                int64_t r = q + 1;
                if (w.sidx[q] >= 0) {
                    while (r < j && w.sidx[r] == w.sidx[r - 1] + 1) r++;
                    staged(q, r);
                } else {
                    while (r < j && w.sidx[r] < 0) r++;
                    plain(q, r);
                }
                q = r;
            }
        }
        i = j;
    }
}
struct CopyHelpers {
    std::mutex mu;
    std::condition_variable cv, done;
    std::deque<CopyPart> parts;
    bool started = false;
} copy_helpers;
void copy_helper_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk capture copy");
    for (;;) {
        CopyPart w;
        {
            std::unique_lock lk(copy_helpers.mu);
            copy_helpers.cv.wait(lk, [] { return !copy_helpers.parts.empty(); });
            w = copy_helpers.parts.front();
            copy_helpers.parts.pop_front();
        }
        copy_part(w);
        std::lock_guard lk(copy_helpers.mu);
        if (--*w.left == 0) copy_helpers.done.notify_all();
    }
}
int capture_copy_helpers() {   // (copy_pages and copy_pairs share one pool of helpers, started by the first call that splits)
    static const int helpers = getenv("RX_VK_CAPTURE_THREADS")
        ? std::clamp(atoi(getenv("RX_VK_CAPTURE_THREADS")), 0, 4)
        : (env_on("RX_VK_LOW_SPEC") ? 0 : std::thread::hardware_concurrency() >= 12 ? 4 : 2);   // (four where the threads are spare)
    return helpers;
}
/// (hookshot) whole's n copies in helpers + 1 parts: the first on the calling thread, the others queued for the helpers.
/// Under 128 copies (512 KB) all on the calling thread: waking the helpers costs more than they save. Its own parts no
/// helper has taken yet the caller runs itself once its first is done, rather than waiting behind what another caller
/// queued before them (copy_pairs runs with pools_mu and shadow_mu held, copy_pages' guest copies can take milliseconds)
void copy_split(CopyPart whole, int64_t n) {
    const int helpers = capture_copy_helpers();
    if (!helpers || n < 128) { whole.i0 = 0, whole.i1 = n; return copy_part(whole); }
    const int k = helpers + 1;
    int left = k - 1;
    auto part = [&](int i) {
        CopyPart w = whole;
        w.i0 = n * i / k, w.i1 = n * (i + 1) / k, w.left = &left;
        return w;
    };
    {
        std::lock_guard lk(copy_helpers.mu);
        if (!copy_helpers.started) {
            copy_helpers.started = true;
            for (int i = 0; i < helpers; i++) std::thread(copy_helper_main).detach();
        }
        for (int i = 1; i < k; i++) copy_helpers.parts.push_back(part(i));
    }
    copy_helpers.cv.notify_all();
    copy_part(part(0));
    std::unique_lock lk(copy_helpers.mu);
    for (;;) {
        auto it = std::find_if(copy_helpers.parts.begin(), copy_helpers.parts.end(), [&](const CopyPart& w) { return w.left == &left; });
        if (it == copy_helpers.parts.end()) break;
        const CopyPart w = *it;
        copy_helpers.parts.erase(it);
        lk.unlock();
        copy_part(w);
        lk.lock();
        --left;
    }
    copy_helpers.done.wait(lk, [&] { return left == 0; });
}
void copy_pages(const uint32_t* pages, int64_t n, uint8_t* out, uint8_t* ok, uint8_t* stage = nullptr, const int32_t* sidx = nullptr) {   // (one capture at a time: submit_order_mu)
    CopyPart w{pages, 0, n, out, ok};
    w.stage = stage, w.sidx = sidx;
    copy_split(w, n);
}
/// (hookshot) n 4 KB copies on the capture's copy helpers too: the texture shadow's (take_capture), thousands of pages a submit
/// for OoT's game thread, which copied them one after another
void copy_pairs(const CopyPair* pairs, int64_t n) {
    CopyPart w{nullptr, 0, n, nullptr, nullptr};
    w.pairs = pairs;
    copy_split(w, n);
}

std::unique_ptr<Capture> capture_make();
void shadow_free(Pool& p);
void shadow_forget(Pool& p, uint64_t at, uint64_t len);   // (the write-back's diff, below: a range its shadow no longer knows)
void memory_capture_submit() {
    std::unique_ptr<Capture> c = capture_make();
    if (!c) return;
    std::lock_guard lk(cap_mu);
    captures.push_back(std::move(c));
}
/// (the pages the CPU wrote since the last capture, copied now: a submit's capture, or memory_capture_now's)
std::unique_ptr<Capture> capture_make() {
    if (!capture_on()) return nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (!spare.empty()) c = std::move(spare.back()), spare.pop_back();
    }
    if (!c) c = std::make_unique<Capture>();
    c->pages.clear(), c->copied.clear(), c->slots.clear(), c->nbytes = 0, c->late.clear(), c->late_value = 0;
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    stage_put(*c);
    // (only the pools mirrored in VRAM, the ones the mirrors are filled from, and only their part of the write marks:
    // every written page was taken at first (hundreds of MB a submit where the game streams its world), and the whole
    // collection, over every GPU range, is ~20 ms: 4 of them a frame on the game's thread made it crawl. The replay's
    // collection takes the rest, as before)
    // (the list the replay publishes when it makes mirrors: pools_mu here waited ~40 ms a submit for the replay)
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> mirrored;
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> texr;   // (tex_cap_ranges, sorted and merged: the staged pages)
    {   // (and the texture storage the replay uploads from: see "texture storage in the capture"; and the small pools)
        std::lock_guard lk(cap_ranges_mu);
        captures_made++;
        refresh_pool_ranges_locked();
        static thread_local uint64_t ver = 0;
        if (ver != cap_ranges_ver) {   // (the three lists overlap: their union, sorted, once each time one changes)
            ver = cap_ranges_ver;
            std::vector<std::pair<uint64_t, uint64_t>> all = cap_ranges;
            all.insert(all.end(), tex_cap_ranges.begin(), tex_cap_ranges.end());
            all.insert(all.end(), pool_cap_ranges.begin(), pool_cap_ranges.end());
            std::sort(all.begin(), all.end());
            mirrored.clear();
            for (const auto& r : all) {
                if (!mirrored.empty() && r.first <= mirrored.back().second)
                    mirrored.back().second = std::max(mirrored.back().second, r.second);
                else
                    mirrored.push_back(r);
            }
            texr = tex_cap_ranges;
            std::sort(texr.begin(), texr.end());
            size_t m = 0;
            for (const auto& r : texr) {
                if (m && r.first <= texr[m - 1].second) texr[m - 1].second = std::max(texr[m - 1].second, r.second);
                else texr[m++] = r;
            }
            texr.resize(m);
        }
    }
    captures_seen.store(true, std::memory_order_relaxed);
    const auto t_lock = std::chrono::steady_clock::now();
    uint32_t* pages = nullptr;
    const int64_t n = mirrored.empty() || !host->collect_written_in ? 0
                    : host->collect_written_in(reinterpret_cast<const uint64_t(*)[2]>(mirrored.data()), (int)mirrored.size(), &pages);
    const auto t_scan = std::chrono::steady_clock::now();
    // (hookshot) A page the CPU wrote that a submission still on the GPU writes back: the copy waits for that write-back.
    // OoT's exposure meter page is written by both: the GPU adds the frame's light into it and the CPU sets the sums back to
    // 0 and fills in the rest. The CPU runs a frame ahead, and a copy taken before the write-back had landed had the GPU's
    // words from the frame before; the mirror sync of the page put them back over the GPU's newer ones, and the exposure
    // stepped for a frame (one-frame flashes, 40-120 a minute in Kokiri Forest). Only the copies of such pages wait, for the
    // last submission writing them back: not every frame's present. That wait held the game's thread (~8% of its time at a
// ~2150-draw scene), so those pages are copied again by the replay, before it uses this capture, once the submission is
// done: the replay waits instead, while the game goes on. RX_VK_CAPTURE_WAIT=1: the game's thread waits, 0: no wait
    if (n > 0) {
        static const int cap_wait = getenv("RX_VK_CAPTURE_WAIT") ? atoi(getenv("RX_VK_CAPTURE_WAIT")) : 2;
        uint64_t v = 0;
        if (cap_wait) {
            const uint64_t done = vk_timeline_completed();
            std::lock_guard lk(wb_pages_mu);
            if (!wb_pages.empty())
                for (int64_t i = 0; i < n; i++) {
                    const uint64_t pv = wb_pages.get(pages[i]);
                    if (pv > done) {
                        v = std::max(v, pv);
                        if (cap_wait == 2) c->late.push_back((uint32_t)i);
                    }
                }
        }
        if (v && cap_wait == 2) c->late_value = v;
        else if (v) vk_timeline_wait(v);
    }
    if (n > 0) shadow_written(pages, n);
    if (n > 0) {
        c->pages.assign(pages, pages + n);
        if ((uint64_t)n * 4096 <= (256ull << 20) &&   // (a load writes more: those are read later, as before)
            cap_buf_get((uint64_t)n * 4096, c->buffer, c->alloc, c->map, c->bsize)) {
            static thread_local std::vector<uint8_t> ok;
            ok.assign((size_t)n, 0);
            // (shadow staging: the pages in the texture ranges are also copied into c->stage, from guest memory first and the buffer
            // from there. Not the late pages: take_capture copies those again into the buffer after the GPU wait, so the buffer is
            // where their newer bytes are, and they stay unstaged (sidx -1: the shadow reads them from the buffer; a few a take))
            // (only where the capture buffer is not cached memory: a cached one the shadow reads as fast as the staging, and the
            // extra copy would be the game thread's for nothing)
            c->buf_cached = host_cached(c->alloc);
            if (stage_on() && !texr.empty() && !c->buf_cached) {
                c->sidx.assign((size_t)n, -1);
                int32_t tn = 0;
                size_t lj = 0;
                for (int64_t i = 0; i < n; i++) {
                    while (lj < c->late.size() && c->late[lj] < (uint32_t)i) lj++;
                    if (lj < c->late.size() && c->late[lj] == (uint32_t)i) continue;
                    const uint64_t g = (uint64_t)pages[i] << kPageShift;
                    auto it = std::upper_bound(texr.begin(), texr.end(), std::make_pair(g, ~uint64_t(0)));
                    if (it != texr.begin() && g < std::prev(it)->second) c->sidx[(size_t)i] = tn++;
                }
                if (!tn || !stage_get(*c, (size_t)tn * 4096)) c->sidx.clear();
            }
            copy_pages(pages, n, (uint8_t*)c->map, ok.data(), c->sidx.empty() ? nullptr : c->stage.get(),
                       c->sidx.empty() ? nullptr : c->sidx.data());
            for (int64_t i = 0; i < n; i++)
                if (ok[i]) c->copied.push_back(pages[i]), c->slots.push_back((uint32_t)i);
            c->nbytes = (size_t)n * 4096;
            flush_mapped(c->alloc, c->nbytes);
        }
    }
    if (pages) host->free_pages(pages);
    {   // (what it cost, every 600 submits)
        static int64_t subs, written, copied;
        static double ms, ms_lock, ms_scan;
        static uint64_t span;
        subs++, written += n > 0 ? n : 0, copied += (int64_t)c->copied.size();
        const auto now = std::chrono::steady_clock::now();
        ms += std::chrono::duration<double, std::milli>(now - t0).count();
        ms_lock += std::chrono::duration<double, std::milli>(t_lock - t0).count();
        ms_scan += std::chrono::duration<double, std::milli>(t_scan - t_lock).count();
        if (subs % 600 == 0) {
            span = 0;
            for (const auto& r : mirrored) span += r.second - r.first;
            logf("vk: capture: a submit %.2f ms (lock %.2f, scan %.2f over %zu ranges, %llu MB), %.0f pages written, %.0f "
                 "copied (%.1f MB)", ms / 600, ms_lock / 600, ms_scan / 600, mirrored.size(),
                 (unsigned long long)(span >> 20), written / 600.0, copied / 600.0, copied * 4096.0 / 600 / 1048576);
            ms = ms_lock = ms_scan = 0, written = copied = 0;
        }
    }
    return c;
}

/// The ranges the captures take (the mirrored pools, the texture storage, the small pools): their union, sorted and
/// merged, as of now; empty while no capture was made (memory_collect_written: a page in them written after its
/// submit's capture is left for the next capture). Built again only when one of the three lists changed, as
/// capture_make keeps its own union; the reference is the calling thread's until its next call
const std::vector<std::pair<uint64_t, uint64_t>>& capture_ranges_merged() {
    static const std::vector<std::pair<uint64_t, uint64_t>> none;
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> merged;
    static thread_local uint64_t ver = ~uint64_t(0);
    // (no capture is ever made without the replay thread (RX_VK_ASYNC=0): the mirrored pools' writes were left for one
    // that never came, and the pools kept their first contents: a grey world)
    if (!capture_on() || !captures_seen.load(std::memory_order_relaxed)) return none;
    std::lock_guard lk(cap_ranges_mu);
    refresh_pool_ranges_locked();   // (a pool made since the last capture: the next one takes its pages, not this)
    if (ver != cap_ranges_ver) {
        ver = cap_ranges_ver;
        std::vector<std::pair<uint64_t, uint64_t>> all = cap_ranges;
        all.insert(all.end(), tex_cap_ranges.begin(), tex_cap_ranges.end());
        all.insert(all.end(), pool_cap_ranges.begin(), pool_cap_ranges.end());
        std::sort(all.begin(), all.end());
        merged.clear();
        for (const auto& r : all) {
            if (!merged.empty() && r.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, r.second);
            else
                merged.push_back(r);
        }
    }
    return merged;
}
// (hookshot) see vk_internal.h. Raised where a recording gets a read of guest memory itself, at the GPU's time: a pool
// bound with no mirror (gpu_buffer_bind), a mirror's dirty pages no capture holds (mirror_sync), a mirror's first copy,
// and the pool handed out whole (gpu_buffer: a large texture unswizzled on the GPU, a texel buffer's view). With all
// pools mirrored and captured (the usual case on a card with room) it stays 0. outside: a binding made outside a
// recording's uploads belongs to no part begun yet: counted for the next one, so every part begun so far is older.
// Two reads of guest memory at the GPU's time are not noted:
//  - memory_rb_refresh's copies of the readback pages into their mirrors, in every part's uploads. They are how a write
//    of the CPU's to a page it reads back reaches the mirror at all, so a part after the readback's taking that write is
//    what they are for (and what the console's one memory does); a part that writes such a page again has it noted with
//    its own part, which the read then waits for. Noted, they would be in every part and no read would ever wait for
//    less than its whole submission. What they do carry along is whatever else shares a 4 KB page with a readback range;
//  - the shaders' own pointers into a pool that was never bound and so has no mirror (the page table's entry is the
//    pool's own address): no call of ours is made for those reads.
// Both are as they were before a value's recording became its last part's (v2.0.14 and before)
namespace {
std::atomic<uint64_t> mem_rec_part{0}, mem_in_place_part{0};
void note_in_place_read(bool outside = false) {
    const uint64_t part = mem_rec_part.load() + (outside ? 1 : 0);
    uint64_t seen = mem_in_place_part.load();
    while (seen < part && !mem_in_place_part.compare_exchange_weak(seen, part)) {}
}
} // namespace
void memory_recording_part(uint64_t part) { mem_rec_part.store(part); }
uint64_t memory_in_place_part() { return mem_in_place_part.load(); }
void memory_recording_value(uint64_t value, uint64_t completed) {
    mem_rec_value = value;
    if (completed > mem_completed) mem_completed = completed;
}
/// (hookshot, RX_VK_FRAME_TRACE) The time the replay has waited for the GPU in take_capture, for the pages a submission still
/// on the GPU writes back: part of the trace's memsync_ms, and the part that is a wait. Frames of 36-201 ms of memory sync in a
/// played session could not be told from a slow copy
std::atomic<int64_t> st_late_wait_ns{0};
int64_t memory_late_wait_ns() { return st_late_wait_ns.load(std::memory_order_relaxed); }
bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {
    const auto tk0 = std::chrono::steady_clock::now();
    if (c->late_value && c->map) {   // (the pages a submission still on the GPU writes back: see capture_make)
        // (timed apart: the replay thread waits here for the GPU, the more often the fewer pools have a mirror. With
        // RX_VK_MIRROR_MB=1024 on an 8 GB card it was most of a capture's 2.4-3 ms, six submits a frame: Hyrule Field at
        // 35-45 fps instead of 55, issue 995)
        const auto tw = std::chrono::steady_clock::now();
        vk_timeline_wait(c->late_value);
        st_tk_late += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw).count();
        st_late_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tw).count(),
                                  std::memory_order_relaxed);   // (hookshot: the frame trace's late_wait_ms)
        st_tk_late_n++;
        uint8_t* const base = (uint8_t*)host->guest_base();
        for (uint32_t i : c->late)
            guarded_copy((uint8_t*)c->map + (uint64_t)i * 4096, base + ((uint64_t)c->pages[i] << kPageShift), 4096);
        flush_mapped(c->alloc, c->nbytes);
    }
    const auto tl = std::chrono::steady_clock::now();
    std::lock_guard lk(pools_mu);
    const auto tk_locked = std::chrono::steady_clock::now();
    st_tk_lock += std::chrono::duration<double, std::milli>(tk_locked - tl).count();
    cur_cap_value = value;
    cur_cap_gen = 0;
    // (captures past the last kKeep, or past the byte budget, retired once the work that read them is done)
    while (!cap_bufs.empty()) {
        auto oldest = cap_bufs.begin();
        const bool over = cap_bufs.size() > kKeep || cap_bytes > kCapBytesMax;
        if (!over || std::max(oldest->second.value, oldest->second.last_use) > completed) break;
        cap_buf_retire(oldest);
    }
    uint32_t g = 0;
    if (!c->pages.empty()) {
        g = ++gen_now;
        for (uint32_t p : c->pages) stamp_page(p, g);
        pages_collected += c->pages.size();
    }
    if (!c->pages.empty() && tex_capture_on()) {   // (the texture storage it took: the shadow, as of this submit)
        std::lock_guard sk(shadow_mu);
        shadow_seq++;
        // (the copies of pages no capture took in the last 256 submits go, over 32 MB of them, every 256 captures: such a page is what guest
        // memory holds (a write drops its copy), and the cap's sweep below came only at 256 MB. RX_VK_SHADOW_SWEEP_OFF=1: the cap's alone)
        static const bool sweep_off = env_on("RX_VK_SHADOW_SWEEP_OFF");
        if (!sweep_off && shadow_seq % 256 == 0 && shadow_n.load(std::memory_order_relaxed) > 8192)
            for (auto it = shadow.begin(); it != shadow.end();) {
                if (it->second.seq + 256 >= shadow_seq) { ++it; continue; }
                auto r = shadow_regions.find(it->first >> 9);
                if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
                it = shadow.erase(it);
                shadow_n--;
            }
        if (!tex_merged.empty()) {
            // (every page written since the last capture: its old copy is stale; the ones copied now replace it)
            // (hookshot: those copied now are overwritten in place below, their buffers kept: erasing every written page and
            // inserting the copied ones again was a free, an allocation and two hash operations a page, for the pages the
            // game rewrites every frame. c->slots[i] is the index in c->pages of c->copied[i], in order)
            {
                size_t j = 0;
                for (size_t i = 0; i < c->pages.size(); i++) {
                    if (j < c->slots.size() && c->slots[j] == i) {
                        j++;
                        continue;
                    }
                    const uint32_t p = c->pages[i];
                    if (in_tex_merged((uint64_t)p << 12)) shadow_erase(p);
                }
            }
            bool swept = false;
            static int64_t st_takes, st_from_stage, st_from_map, st_cached;   // (the shadow copies by source and the takes whose buffer was cached, logged every 600 takes; shadow_mu)
            int64_t from_stage = 0, from_map = 0;
            static thread_local std::vector<CopyPair> pairs;   // (the copies, made together once the slots are known)
            pairs.clear();
            size_t i = 0;
            for (; i < c->copied.size(); i++) {
                const uint32_t p = c->copied[i];
                if (!in_tex_merged((uint64_t)p << 12)) continue;
                if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax && !shadow.count(p)) {
                    if (swept) break;
                    swept = true;
                    for (auto it = shadow.begin(); it != shadow.end();) {
                        if (it->second.seq + 64 >= shadow_seq) { ++it; continue; }
                        auto r = shadow_regions.find(it->first >> 9);
                        if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
                        it = shadow.erase(it);
                        shadow_n--;
                    }
                    if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax) break;
                }
                ShadowPage& slot = shadow[p];
                if (!slot.b) {
                    slot.b.reset(new uint8_t[4096]);
                    shadow_regions[p >> 9]++;
                    shadow_n++;
                }
                slot.seq = shadow_seq;
                // (the staging, ordinary RAM, when it holds the page; else the capture buffer, maybe write-combined)
                const int32_t sx = c->sidx.empty() ? -1 : c->sidx[c->slots[i]];
                if (sx >= 0) pairs.push_back({slot.b.get(), (const uint8_t*)c->stage.get() + (uint64_t)sx * 4096}), from_stage++;
                else pairs.push_back({slot.b.get(), (const uint8_t*)c->map + (uint64_t)c->slots[i] * 4096}), from_map++;
            }
            // (shadow_mu still held: no slot taken here goes before its copy; the sweep above drops only older ones)
            if (!pairs.empty()) copy_pairs(pairs.data(), (int64_t)pairs.size());
            st_from_stage += from_stage, st_from_map += from_map, st_cached += c->buf_cached ? 1 : 0;
            if (++st_takes % 600 == 0) {
                logf("vk: shadow staging (600 takes): %lld shadow copies from staging, %lld from the capture buffer; capture buffers: %lld "
                     "host-cached, %lld not (write-combined: staged)%s", (long long)st_from_stage, (long long)st_from_map,
                     (long long)st_cached, (long long)(600 - st_cached), stage_on() ? "" : " (RX_VK_SHADOW_STAGE=0)");
                st_from_stage = st_from_map = 0, st_cached = 0;
            }
            for (; i < c->copied.size(); i++)   // (the shadow full: the copied pages not replaced lose their old copies)
                if (in_tex_merged((uint64_t)c->copied[i] << 12)) shadow_erase(c->copied[i]);
        }
    }
    const auto tk1 = std::chrono::steady_clock::now();
    if (!c->copied.empty() && g && c->map) {   // (its buffer kept with the captures, for the mirror syncs)
        CapBuf b{value, c->buffer, c->alloc, c->bsize, c->map};
        c->buffer = VK_NULL_HANDLE, c->alloc = nullptr, c->map = nullptr, c->bsize = 0;
        cap_bufs.emplace(g, b);
        cap_bytes += b.size;
        cur_cap_gen = g;
        for (size_t i = 0; i < c->copied.size(); i++) cap_entry_set(c->copied[i], CapEntry{g, c->slots[i]});
    }
    cap_view_bump();
    stage_put(*c);   // (the shadow's copies from it are done: copy_pairs returned)
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    {
        std::lock_guard lk(cap_mu);
        if (spare.size() < 8) spare.push_back(std::move(c));
    }
    {   // (every 600 takes: where a take's time went)
        const auto tk2 = std::chrono::steady_clock::now();
        static int64_t n;
        st_tk_stamp += std::chrono::duration<double, std::milli>(tk1 - tk_locked).count();
        st_tk_all += std::chrono::duration<double, std::milli>(tk2 - tk0).count();
        if (++n % 600 == 0) {
            logf("vk: a capture taken (ms): all %.3f; GPU wait for late pages %.3f (%lld of 600 takes), pools lock %.3f, retires "
                 "and stamps %.3f, bytes copied %.3f; %zu kept (%llu MB), %zu spare buffers, %zu texture pages copied (%llu MB)",
                 st_tk_all / 600, st_tk_late / 600, (long long)st_tk_late_n, st_tk_lock / 600, st_tk_stamp / 600, st_tk_copy / 600,
                 cap_bufs.size(), (unsigned long long)(cap_bytes >> 20), cap_free.size(), shadow_n.load(std::memory_order_relaxed),
                 (unsigned long long)(shadow_n.load(std::memory_order_relaxed) >> 8));
            st_tk_all = st_tk_stamp = st_tk_copy = st_tk_late = st_tk_lock = 0, st_tk_late_n = 0;
        }
    }
    return true;
}
bool memory_take_capture(uint64_t value, uint64_t completed) {
    if (completed > mem_completed) mem_completed = completed;
    if (!capture_on()) return false;
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (captures.empty()) return false;
        c = std::move(captures.front());
        captures.pop_front();
    }
    return take_capture(std::move(c), value, completed);
}
/// (hookshot) A capture made and taken at once, for a submission going on from a wait the CPU's signal met
/// (vk_replay.cpp resume_queue): the CPU's writes up to that signal, which the console's GPU reads after its wait
bool memory_capture_now(uint64_t value, uint64_t completed) {
    if (completed > mem_completed) mem_completed = completed;
    std::unique_ptr<Capture> c = capture_make();
    return c && take_capture(std::move(c), value, completed);
}

namespace {

/// The imported pool holding [guest, guest + size) (imported now if it was not yet); null outside the pools.
/// pools_mu held.
Pool* pool_cache[4096];   // (by 64 KB page: the pools buffer bindings hit lately)
std::atomic<uint64_t> pools_gen{1};   // (bumped when a pool leaves the table: the recording threads' caches go stale)
struct TlPool {
    Pool* p;
    uint64_t gen;
};
thread_local TlPool tl_pools[1024];   // (a parallel replay thread's: by 64 KB page, read without the lock)

Pool* pool_of(uint64_t guest, uint64_t size) {
    if (!size || size > UINT64_MAX - guest) return nullptr;
    if (last_pool && guest >= last_pool->lo && guest + size <= last_pool->hi) return last_pool;
    Pool*& slot = pool_cache[(guest >> 16) & 4095];
    if (slot && guest >= slot->lo && guest + size <= slot->hi) return last_pool = slot;
    Pool* found = pool_lookup(guest, size);
    if (found) slot = found;
    return found;
}

// ---- pools imported in the background: an import took 5-45 ms (the driver locks the pages; the game's threads keep
// the address space busy committing theirs), and the game makes ~13 pools a second. The host says when the game makes
// one (memory_pool_created), a thread of ours imports it, and a lookup that finds one under way waits for it rather
// than importing it again. Imports made are handed over in a list (the lookup holds pools_mu, the thread never). ----
std::mutex bg_mu;
std::condition_variable bg_cv, bg_done_cv;
std::deque<std::pair<uint64_t, uint64_t>> bg_queue;
std::vector<uint64_t> bg_inflight;           // (lo of the ranges queued or being imported)
std::vector<std::pair<uint64_t, Pool>> bg_ready;
int64_t st_bg_imports, st_bg_waits, st_prefetch_sent, st_prefetch_used, st_prefetch_dropped;
void bg_main();
void bg_start() {   // (bg_mu held)
    static bool started;
    if (!started) {
        started = true;
        std::thread(bg_main).detach();
    }
}

void bg_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk imports");
    for (;;) {
        std::pair<uint64_t, uint64_t> r;
        {
            std::unique_lock lk(bg_mu);
            bg_cv.wait(lk, [] { return !bg_queue.empty(); });
            r = bg_queue.front();
            bg_queue.pop_front();
        }
        Pool p;
        const bool ok = import_pool(r.first, r.second, p);
        std::lock_guard lk(bg_mu);
        p.prefetched = true;
        if (ok) bg_ready.push_back({r.first, p}), st_bg_imports++;
        std::erase(bg_inflight, r.first);
        bg_done_cv.notify_all();
    }
}

/// The imports the thread made, into the table (pools_mu held); one already there makes the new one redundant
void bg_drain() {
    std::vector<std::pair<uint64_t, Pool>> ready;
    {
        std::lock_guard lk(bg_mu);
        if (bg_ready.empty()) return;
        ready.swap(bg_ready);
    }
    for (auto& [lo, p] : ready) {
        auto imp = imported.find(lo);
        if (imp != imported.end() && imp->second.hi >= p.hi) {
            destroy_import_buffer(p.buffer);
            vkFreeMemory(dev.device, p.memory, nullptr);
            for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);
            continue;
        }
        bool overlap = false;   // (amd-fix: a chunk imported ahead that an import made since covers in part: the pools stay disjoint)
        for (auto o = imported.lower_bound(p.hi); o != imported.begin();) {
            --o;
            if (o == imp) continue;
            if (o->second.hi <= p.lo) {
                if (o->second.hi + (1ull << 30) < p.lo) break;
                continue;
            }
            overlap = true;
            break;
        }
        if (overlap) {
            st_prefetch_dropped++;
            destroy_import_buffer(p.buffer);
            vkFreeMemory(dev.device, p.memory, nullptr);
            for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);
            continue;
        }
        if (imp != imported.end()) retire_pool(imp);
        imported.emplace(lo, p);
        guest_pages_set(p.lo, p.hi, p.address);
    }
}

/// (amd-fix) An import on the replay thread stalled it 20-75 ms on an RX 9060 XT (the driver locks the chunk's pages, the
/// commit of fresh ones), a tiny uniform buffer's binding as long, ~40 times entering Kokiri: hitches. The chunks next to
/// one imported on demand, inside the game's pools, are imported ahead by the background thread; a lookup that needs one
/// still under way waits for it (less than importing it), and a chunk imported ahead and first bound sends for its own
/// neighbours. RX_VK_PREFETCH_CHUNKS: how many on each side (default 0: off, the background imports slowed the replay thread's own to 80-127 ms on AMD). (pools_mu held)
void prefetch_chunks(uint64_t lo, uint64_t hi, uint64_t chunk) {
    static const int n = getenv("RX_VK_PREFETCH_CHUNKS") ? std::max(0, atoi(getenv("RX_VK_PREFETCH_CHUNKS"))) : 0;
    if (n <= 0 || !chunk || !dev.external_memory_host || hi <= lo) return;
    auto in_game_pool = [](uint64_t a, uint64_t b) {
        auto g = std::lower_bound(guest_pools.begin(), guest_pools.end(), b, [](const GuestPool& p, uint64_t v) { return p.lo < v; });
        return g != guest_pools.begin() && std::prev(g)->hi > a;
    };
    auto imported_over = [](uint64_t a, uint64_t b) {
        for (auto o = imported.lower_bound(b); o != imported.begin();) {
            --o;
            if (o->second.hi > a) return true;
            if (o->second.hi + (1ull << 30) < a) break;
        }
        return false;
    };
    std::vector<std::pair<uint64_t, uint64_t>> want;
    for (int k = 0; k < n; k++) {
        const uint64_t up = hi + (uint64_t)k * chunk;
        if (up < UINT64_MAX - chunk) want.push_back({up, up + chunk});
        const uint64_t down = (uint64_t)(k + 1) * chunk;
        if (lo >= down) want.push_back({lo - down, lo - down + chunk});
    }
    std::lock_guard lk(bg_mu);
    for (const auto& [a, b] : want) {
        if (b > (1ull << 40) || !in_game_pool(a, b) || imported_over(a, b)) continue;
        if (std::find(bg_inflight.begin(), bg_inflight.end(), a) != bg_inflight.end()) continue;
        if (bg_inflight.size() >= 16) break;
        bg_start();
        bg_inflight.push_back(a);
        bg_queue.push_back({a, b});
        st_prefetch_sent++;
    }
    bg_cv.notify_one();
}

/// (imports by chunk (RX_VK_IMPORT_CHUNK_MB), not by pool: each import cost 5-45 ms in the driver, more as their number grew,
/// and the game makes ~13 pools a second; a chunk covers the pools after it at no cost)
/// (hookshot) 4 MB, not 16: a pool's VRAM mirror is its import's whole range, and with 16 MB chunks 28-39% of the mirrors'
/// bytes were outside every game pool (DLAA 2560 x 1440: 459 of 1,666 MB in a Hyrule Field walk, up to 925 MB after Castle
/// Town to the field). At 4 MB the mirrors were 1,236-1,268 MB against 1,645-1,657 in the same walk, at the same frame rate
/// (58.1-58.6 against 58.2-59.0 fps); in Kokiri Forest 843 MB against 1,154; with a 6 GB allowance the VRAM peak stayed under
/// it (5,865-5,887 MB against 6,129-6,135) and the GPU's longest frame was 12.5-13.5 ms against 21.7-24.1 (the frame rate
/// went either way: 53.0 / 52.4 fps against 50.3 / 54.9). It takes ~50% more imports (91-94 against 59-65 in five minutes,
/// 0.8 s of driver time in all against 0.7, each one shorter): span_game_pools still takes a pool across a chunk's end into
/// one import. Only where the pools have mirrors (mirror_on): without them a chunk's padding costs no VRAM, and smaller chunks
/// would only mean more imports. RX_VK_IMPORT_CHUNK_MB=16: as before
bool mirror_on();
uint64_t import_chunk() {
    static const uint64_t chunk =
        (uint64_t)(getenv("RX_VK_IMPORT_CHUNK_MB") ? std::max(0, atoi(getenv("RX_VK_IMPORT_CHUNK_MB"))) : mirror_on() ? 4 : 16) << 20;
    return chunk;
}
/// (hookshot) A game pool across a chunk's end had each chunk imported on its own, at the first binding in it, and a later
/// binding across the end imported the two again as one, on the replay thread: OoT's 16 MB pool at 0x1021745000 bound at
/// 0x1021ffff00+0x160 across 0x1022000000, 130-147 ms, the one hitch of a panning run in Kokiri. Up to this size an import
/// takes the chunks of the game pools across its ends too (RX_VK_IMPORT_SPAN_MB, default 64; 0: not)
uint64_t import_span() {
    static const uint64_t span = (uint64_t)(getenv("RX_VK_IMPORT_SPAN_MB") ? std::max(0, atoi(getenv("RX_VK_IMPORT_SPAN_MB"))) : 64) << 20;
    return span;
}
/// [lo, hi), chunk-aligned, grown over the game pools that run across its ends, while it stays within import_span (pools_mu held)
void span_game_pools(uint64_t& lo, uint64_t& hi, uint64_t chunk) {
    const uint64_t span = import_span();
    if (!chunk || hi - lo >= span) return;
    for (bool grew = true; grew;) {
        grew = false;
        for (const uint64_t e : {lo, hi}) {
            // (a pool across e that fits starts above e - span: the pools from there to e, as the host's may overlap)
            auto g = std::upper_bound(guest_pools.begin(), guest_pools.end(), e, [](uint64_t v, const GuestPool& p) { return v < p.lo; });
            while (g != guest_pools.begin()) {
                --g;
                if (g->lo + span <= e) break;
                if (g->lo >= e || g->hi <= e) continue;
                const uint64_t nlo = std::min(lo, g->lo / chunk * chunk), nhi = std::max(hi, (g->hi + chunk - 1) / chunk * chunk);
                if (nhi - nlo > span || (nlo == lo && nhi == hi)) continue;
                lo = nlo, hi = nhi, grew = true;
            }
        }
    }
}

Pool* pool_lookup(uint64_t guest, uint64_t size) {
    if (guest_pools.empty()) return nullptr;
    bg_drain();
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                               [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return nullptr;
    --it;
    if (guest < it->lo || guest >= it->hi || !size || size > it->hi - guest) return nullptr;
    const uint64_t chunk = import_chunk();
    const GuestPool range = chunk ? GuestPool{guest, guest + size} : *it;
    auto covering = [&]() {
        auto c = imported.upper_bound(range.lo);
        if (c == imported.begin()) return imported.end();
        --c;
        return c->second.lo <= range.lo && c->second.hi >= range.hi ? c : imported.end();
    };
    auto imp = chunk ? covering() : imported.find(range.lo);
    if (chunk && range.hi > UINT64_MAX - (chunk - 1)) return nullptr;
    const uint64_t want_lo = chunk ? range.lo / chunk * chunk : range.lo;
    const uint64_t want_hi = chunk ? (range.hi + chunk - 1) / chunk * chunk : range.hi;
    if (chunk && imp == imported.end()) imp = imported.find(want_lo);   // (a smaller one there: made again, larger)
    if (imp == imported.end() || imp->second.hi < range.hi) {   // (under way in the background: waited for)
        bool waited = false;
        {
            std::unique_lock lk(bg_mu);
            auto under_way = [&] {   // (the pool's own import, or a chunk of the range imported ahead)
                for (const uint64_t x : bg_inflight)
                    if (x == range.lo || (chunk && x >= want_lo && x < want_hi)) return true;
                return false;
            };
            if (under_way()) {
                bg_done_cv.wait(lk, [&] { return !under_way(); });
                waited = true;
                st_bg_waits++;
            }
        }
        if (waited) {
            bg_drain();
            imp = chunk ? covering() : imported.find(range.lo);
            if (chunk && imp == imported.end()) imp = imported.find(want_lo);
        }
    }
    if (imp == imported.end() || imp->second.hi < range.hi || imp->second.lo > range.lo) {
        // (the new pool takes in every imported pool its range overlaps, and they are retired: the imported pools stay
        // disjoint. A range crossing a chunk's end made the chunk's pool again, larger, over the next chunk, whose own
        // pool stayed: two mirrors of the same memory. MKW's GPU culling wrote its instance data (a 2.4 MB storage
        // buffer) through one, the draws read it through the other, synced from guest memory before the submission's
        // write-back: the instances of a frame before or older, objects popping in and out, one-frame flashes.)
        uint64_t lo = 0, hi = 0;
        std::vector<uint64_t> overlapped;
        auto plan = [&](bool spanned) {   // (the range, the imported pools it takes in; spanned: span_game_pools's chunks too)
            lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
            if (spanned && chunk) span_game_pools(lo, hi, chunk);
            for (bool grew = true; grew;) {
                grew = false;
                overlapped.clear();
                auto it = imported.lower_bound(hi);   // (the pools starting below hi, the last ones first)
                while (it != imported.begin()) {
                    --it;
                    if (it->second.hi <= lo) {
                        if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
                        continue;
                    }
                    overlapped.push_back(it->first);
                    if (it->second.lo < lo) lo = it->second.lo, grew = true;
                    if (it->second.hi > hi) hi = it->second.hi, grew = true;
                }
            }
            if (hi - lo > (1ull << 30)) {   // (no larger than 1 GB: then as before, the chunk's pool alone, overlaps logged)
                static int logged_big;
                if (logged_big++ < 8)
                    logf("vk: pool %#llx..%#llx would take in %zu pools, %llu MB: left overlapping", (unsigned long long)lo,
                         (unsigned long long)hi, overlapped.size(), (unsigned long long)((hi - lo) >> 20));
                lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
                overlapped.clear();
                if (imp != imported.end()) overlapped.push_back(imp->first);
            }
        };
        plan(true);
        const uint64_t spanned_lo = lo, spanned_hi = hi;
        Pool p;
        bool ok = false;
        for (int pass = 0; pass < 2 && !ok; pass++) {
            if (pass) {   // (a spanned import that failed: the range without the span, as imported before)
                plan(false);
                if (lo == spanned_lo && hi == spanned_hi) break;
            }
            const auto t0 = std::chrono::steady_clock::now();
            ok = import_pool(lo, hi, p);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            static double total_ms;
            static int n, logged;
            total_ms += ms, n++;
            if ((ms >= 2.0 && logged++ < 200) || n % 256 == 0)
                logf("vk: pool %#llx+%#llx imported in %.1f ms as %#llx..%#llx (%d imports, %.0f ms in all)",
                     (unsigned long long)range.lo, (unsigned long long)(range.hi - range.lo), ms, (unsigned long long)lo,
                     (unsigned long long)hi, n, total_ms);
        }
        if (!ok) {
            if (!import_cooling && failures++ < 16) {
                if (import_result != VK_SUCCESS)
                    logf("vk: pool %#llx+%#llx not imported (VkResult %d)", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo), (int)import_result);
                else
                    logf("vk: pool %#llx+%#llx not imported", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo));
            }
            return nullptr;
        }
        for (uint64_t k : overlapped) {   // (the pools it took in)
            auto o = imported.find(k);
            if (o != imported.end())   // (their mirrors' contents come with them; and how often the range was trimmed: mirror_trim.h)
                p.gpu_only |= o->second.gpu_only, p.gpu_written |= o->second.gpu_written, p.trims = std::max(p.trims, o->second.trims),
                    p.trim_epoch = std::max(p.trim_epoch, o->second.trim_epoch), retire_pool(o);
        }
        imp = imported.emplace(p.lo, p).first;
        guest_pages_set(p.lo, p.hi, p.address);
        if (chunk) prefetch_chunks(p.lo, p.hi, chunk);
        if (overlapped.size() > 1) {
            static int logged_merge;
            if (logged_merge++ < 16)
                logf("vk: pool %#llx..%#llx took in %zu pools", (unsigned long long)p.lo, (unsigned long long)p.hi,
                     overlapped.size());
        }
        static int logged;
        if (logged++ < 8 || imported.size() % 256 == 0) logf("vk: %zu pools imported", imported.size());
    }
    if (imp->second.prefetched) {   // (first bound: its neighbours ahead in turn)
        imp->second.prefetched = false;
        st_prefetch_used++;
        prefetch_chunks(imp->second.lo, imp->second.hi, chunk);
        static int64_t logged_at;
        if (st_prefetch_used / 16 != logged_at) {
            logged_at = st_prefetch_used / 16;
            logf("vk: chunks imported ahead: %lld sent, %lld bound, %lld dropped; %lld waits", (long long)st_prefetch_sent,
                 (long long)st_prefetch_used, (long long)st_prefetch_dropped, (long long)st_bg_waits);
        }
    }
    imp->second.range_lo = range.lo, imp->second.range_hi = range.hi;
    return last_pool = &imp->second;
}

// ---- the VRAM mirror (after totk-pc-recomp's vk_memory.c) ----
// An imported pool keeps vertices, indices, constants and storage buffers in system memory, so a discrete GPU reads
// them over PCIe on every draw (TOTK measured ~110 ms of GPU time a frame on a 3090 Ti). A pool bound as a buffer
// gets a device-local copy, made at the start of the next submission, and from then on its bindings use the copy at
// the same offsets:
// - CPU -> mirror: at a submission's first use of a mirror, the pages the CPU wrote since its last sync (the page
//   write generations) are copied in, in the recording's upload command buffer (submitted right before it).
// - GPU -> guest: what the submission's shaders and copies may write into a mirror is copied back into the imported
//   pool at the submission's end, so the CPU (and everything that reads guest memory) sees it.
// RX_VK_MIRROR=0: no mirrors; RX_VK_MIRROR_MB: their budget (default 3072).
bool mirror_on() {
    static const int on = [] {
        const char* e = getenv("RX_VK_MIRROR");
        const bool discrete = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        // (k10) off by default on GCN (no stage pinned to 32 lanes, 64-lane waves: RX 400/500, Vega): an RX 580 ran Kokiri Forest
        // at 20 fps with them and 24.5 without, and an RX 590 (#1001) played with them off
        const bool gcn = dev.sg32_stages == 0 && dev.subgroup_size > 32;
        const int v = e ? atoi(e) != 0 : discrete && !gcn;
        logf("vk: VRAM mirror of buffer pools %s%s", v ? "on" : "off", !e && discrete && gcn ? " (GCN)" : "");
        return v;
    }();
    return on != 0;
}

/// (hookshot, smoothness) A read binding in a chunk not imported yet: its import goes to the background thread and the binding is
/// served from a copy in the recording's stream, rather than the replay waiting for the import. Imports on the replay thread were
/// the main hitch left in play (54-61 a run of the verification walks, 5-380 ms each, 2026-11-09). True: an import of the range is
/// under way (sent now or before) and nothing covers it yet; false: the synchronous path as before (covered, outside the game's
/// pools, or next to an import that would have to grow). RX_VK_ASYNC_IMPORT=1 (opt-in: background imports slowed AMD before) (pools_mu held)
int64_t st_async_sent, st_async_copies;
bool import_in_background(uint64_t guest, uint64_t size) {
    if (guest_pools.empty() || !size || !dev.external_memory_host) return false;
    bg_drain();
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest, [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return false;
    --it;
    if (guest < it->lo || guest >= it->hi || size > it->hi - guest) return false;
    const uint64_t chunk = import_chunk();
    if (!chunk || guest + size > UINT64_MAX - chunk) return false;
    if (auto c = imported.upper_bound(guest); c != imported.begin() && std::prev(c)->second.lo <= guest &&
                                              std::prev(c)->second.hi >= guest + size)
        return false;   // (covered)
    const uint64_t lo = guest / chunk * chunk, hi = (guest + size + chunk - 1) / chunk * chunk;
    for (auto o = imported.lower_bound(hi); o != imported.begin();) {   // (an import there: the synchronous path grows it)
        --o;
        if (o->second.hi <= lo) {
            if (o->second.hi + (1ull << 30) < lo) break;
            continue;
        }
        return false;
    }
    std::lock_guard lk(bg_mu);
    for (const uint64_t x : bg_inflight)
        if (x >= lo && x < hi) return true;   // (under way)
    if (bg_inflight.size() >= 16) return false;
    bg_start();
    bg_inflight.push_back(lo);
    bg_queue.push_back({lo, hi});
    bg_cv.notify_one();
    if (st_async_sent++ % 32 == 0)
        logf("vk: imports in the background: %lld sent, %lld bindings served from a copy meanwhile", (long long)st_async_sent,
             (long long)st_async_copies);
    return true;
}
} // namespace
bool pools_mirrored() { return mirror_on(); }
uint64_t device_vram_bytes() {
    uint64_t vram = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)dev.memory.memoryHeaps[h].size);
    return vram;
}
bool preset_caps_memory() {
    static const uint64_t nominal_gb[] = {0, 4, 6, 8, 12, 16};
    // (by the budgets' preset, which Custom keeps: a smaller preset on a bigger card stays the card's own after a row is tuned too)
    if (opts.budget <= 0 || opts.budget > 5) return true;   // (none: what the file says)
    const uint64_t vram = device_vram_bytes();
    return !vram || vram <= ((nominal_gb[opts.budget] << 30) + (768ull << 20));
}
namespace {
uint64_t mirror_budget() {
    // (a quarter of the card, at most the 3 GB it was for all: on a 6 GB laptop those and the textures were past what the card has)
    static const uint64_t b = [] {
        if (const char* e = getenv("RX_VK_MIRROR_MB")) return (uint64_t)atoi(e) << 20;
        uint64_t vram = 0;
        for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
            if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)dev.memory.memoryHeaps[h].size);
        // (cards of 6 GB or less only: on 8 GB ones, a 3070 or a 4060, the game's memory read over PCIe instead left the GPU at a few
        // percent and the frame stuttering)
        if (vram && vram <= (6ull << 30) + (512ull << 20)) return vram / 8;
        return vram ? std::min<uint64_t>(vram / 4, 3072ull << 20) : (uint64_t)(3072ull << 20);
    }();
    // (the Insert menu's quality preset sets one, 256 MB at Minimum (4GB): it was never read, and an 8 GB card kept ~2 GB of
    // mirrors with the 4GB preset on, 2026-11-08)
    // (only on a card the preset is for: preset_caps_memory)
    if (opts.mirror_mb > 0 && !getenv("RX_VK_MIRROR_MB") && preset_caps_memory()) return (uint64_t)opts.mirror_mb << 20;
    return b;
}
uint64_t mirror_bytes = 0, mirror_epoch = 0;
bool mirror_upload = false;   // (a submission is being recorded: its upload command buffer takes the syncs)
std::vector<Pool*> mirror_wanted, mirror_synced;
struct WriteBack {
    Pool* pool;
    uint64_t lo, hi;   // offsets in the pool
};
std::vector<WriteBack> writebacks;
// (hookshot, 11-07) The counter reports the replay writes into guest memory on the CPU (vk_replay.cpp NOP_REPORT_COUNTER), by
// guest address: a mirrored pool's write-back copied the GPU's mirror over them at the end of the submission, the mirror's
// stale words over the replay's newer ones, and the next render condition read those: a lighting pass left out, the whole lit
// scene black for a frame (the one-frame dark flashes, every 15-30 frames; 17 in 40 s of Kokiri, 0 with mirrors off). They are
// left out of the write-back now. RX_VK_WB_REPORTS=1: written back with the rest, as before
std::mutex cpu_words_mu;
// (two sorted sets with no word in both, and the words noted since the last merge: those go into the recent set, which
// goes into the big one only once it is an eighth of its size. A std::set took a node a word and freed 65536 of them at
// once when it filled, ~4% of the replay thread in a Kokiri profile; one sorted vector with every write-back's new words
// merged in moved the whole of it each time, ~2.4% on a 3900X there (11-08): report words come at new addresses all the time)
std::vector<uint64_t> cpu_words, cpu_words_recent, cpu_words_new;
void sorted_merge(std::vector<uint64_t>& into, const uint64_t* b, const uint64_t* e) {
    const size_t mid = into.size();
    into.insert(into.end(), b, e);
    std::inplace_merge(into.begin(), into.begin() + (ptrdiff_t)mid, into.end());
}
void cpu_words_merge() {   // (holding cpu_words_mu)
    if (cpu_words_new.empty()) return;
    std::sort(cpu_words_new.begin(), cpu_words_new.end());
    cpu_words_new.erase(std::unique(cpu_words_new.begin(), cpu_words_new.end()), cpu_words_new.end());
    // (only the words not kept already: the same report words come every frame)
    size_t fresh = 0;
    for (const uint64_t w : cpu_words_new)
        if (!std::binary_search(cpu_words.begin(), cpu_words.end(), w) &&
            !std::binary_search(cpu_words_recent.begin(), cpu_words_recent.end(), w))
            cpu_words_new[fresh++] = w;
    if (fresh) sorted_merge(cpu_words_recent, cpu_words_new.data(), cpu_words_new.data() + fresh);
    if (cpu_words_recent.size() > std::max<size_t>(4096, cpu_words.size() / 8)) {
        sorted_merge(cpu_words, cpu_words_recent.data(), cpu_words_recent.data() + cpu_words_recent.size());
        cpu_words_recent.clear();
    }
    cpu_words_new.clear();
}
} // namespace
void memory_cpu_owned(uint64_t guest, uint64_t size) {
    static const bool off = env_on("RX_VK_WB_REPORTS");
    if (off || !guest) return;
    std::lock_guard lk(cpu_words_mu);
    if (cpu_words.size() + cpu_words_recent.size() + cpu_words_new.size() > 65536) {   // (as before: forgotten when it gets this big)
        cpu_words_merge();   // (which leaves cpu_words_new empty)
        if (cpu_words.size() + cpu_words_recent.size() > 65536) cpu_words.clear(), cpu_words_recent.clear();
    }
    for (uint64_t a = guest & ~7ull; a < guest + size; a += 8) cpu_words_new.push_back(a);
    // (the write itself, guest_cpu_write, has told the write-back diff's shadows)
}
namespace {
// (a write-back's runs with the replay's own words cut out of them: runs of p, offsets in the pool)
void cut_cpu_words(const Pool& p, std::vector<VkBufferCopy>& runs) {
    std::lock_guard lk(cpu_words_mu);
    cpu_words_merge();
    if (cpu_words.empty() && cpu_words_recent.empty()) return;
    std::vector<VkBufferCopy> out;
    for (const VkBufferCopy& r : runs) {
        uint64_t at = r.srcOffset;
        const uint64_t end = r.srcOffset + r.size;
        // A run can start inside an owned word: include that word when cutting the overlap.
        const uint64_t first = (p.lo + at) & ~7ull, last = p.lo + end;
        auto a = std::lower_bound(cpu_words.begin(), cpu_words.end(), first);
        auto b = std::lower_bound(cpu_words_recent.begin(), cpu_words_recent.end(), first);
        for (;;) {   // (both sets' words in the run, in order)
            const bool in_a = a != cpu_words.end() && *a < last, in_b = b != cpu_words_recent.end() && *b < last;
            if (!in_a && !in_b) break;
            const uint64_t w = (in_a && (!in_b || *a < *b) ? *a++ : *b++) - p.lo;
            if (w > at) out.push_back({at, at, w - at});
            at = std::max(at, w + 8);
        }
        if (end > at) out.push_back({at, at, end - at});
    }
    runs.swap(out);
}

// ---- a pool remade larger (a 16 MB chunk the game's next pool runs past): the old one leaves the table but stays
// alive, as this submission's bindings, its mirror syncs and its write-backs still use it. Erasing it left those
// lists pointing into a freed map node: the write-back copied with garbage handles and the device was lost (the
// daytime title scene, whose pools cross chunk ends). Retired pools are kept (a few, never reused). ----
// (Extracted from the table, not erased: the node, and the Pool in it, stay where they were, so the mirror's lists and
// a parallel replay thread's pool cache still point at a live one.)
std::deque<std::map<uint64_t, Pool>::node_type> retired_pools;

void retire_pool(std::map<uint64_t, Pool>::iterator it) {
    Pool* old = &it->second;
    shadow_free(*old);   // (its write-backs in this submission, if any, copied whole)
    guest_pages_set(old->lo, old->hi, 0);
    old->retired_value = std::max(old->last_value, mem_rec_value);
    old->retired_ms = import_now_ms();   // (the wait before its import is given back counts from here)
    std::erase(mirror_wanted, old);   // (the new pool gets the mirror)
    if (old->mstate == Pool::MIRROR_WANTED) old->mstate = Pool::MIRROR_NONE;
    // (EvilOOT, 11-07) what the GPU wrote into its mirror and nowhere else (the culling's bare table entries past their first
    // 64 bytes, stores through the page table) was lost here: the new pool's mirror is filled from guest memory. Kokiri's
    // foliage records, written by the GPU while the scene loaded and its pools were remade larger, came out zero: every
    // count 0, the trees' leaf clusters never drawn. RX_VK_NO_INHERIT=1: as before
    static const bool no_inherit = env_on("RX_VK_NO_INHERIT");
    if (old->mstate == Pool::MIRROR_ON && !no_inherit) old->inherit_pending = true, old->retired_epoch = mirror_epoch;
    pools_gen++;
    retired_pools.push_back(imported.extract(it));
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8 || retired_pools.size() % 64 == 0)
        logf("vk: pool %#llx remade larger (%zu retired)", (unsigned long long)old->lo, retired_pools.size());
}
int64_t st_sync_bytes, st_sync_copies, st_wb_bytes, st_mirrors;
int64_t st_sync_live_pages, st_sync_cap_pages, st_sync_old_pages;   // (mirror syncs' dirty pages: live / this submit's capture / an earlier one's)
std::unordered_map<uint64_t, int64_t> live_by_pool;
std::unordered_map<uint64_t, std::pair<int64_t, int>> direct_by_pool;   // (bindings the GPU reads in guest memory: count, state)
std::unordered_map<uint64_t, uint64_t> direct_bytes_by_pool;           // (and the bytes of those bindings: what the GPU may read over PCIe, a binding's whole range)
int64_t live_seen = 0;   // (pool -> pages its syncs read live, since the last report)

void transfer_barrier(VkCommandBuffer cmd) {
    full_barrier(cmd, "barrier (write-back or sync)");   // (the same barrier; one right after another is left out)
}

// Captured pages in a run are copied from one buffer to one mirror. Combining only contiguous source and
// destination bytes leaves gaps, overlaps and the order of the remaining regions unchanged.
void append_capture_copy(std::vector<VkBufferCopy>& runs, const VkBufferCopy& next) {
    if (!runs.empty()) {
        VkBufferCopy& last = runs.back();
        if (last.size && next.size && last.size <= UINT64_MAX - last.srcOffset &&
            last.size <= UINT64_MAX - last.dstOffset && next.size <= UINT64_MAX - last.size &&
            next.size <= UINT64_MAX - next.srcOffset && next.size <= UINT64_MAX - next.dstOffset &&
            last.srcOffset + last.size == next.srcOffset && last.dstOffset + last.size == next.dstOffset) {
            last.size += next.size;
            return;
        }
    }
    runs.push_back(next);
}

/// The pages of the pool the CPU wrote since its mirror's sync, copied in (the upload command buffer)
void mirror_sync(Pool& p) {
    p.synced_epoch = mirror_epoch;
    p.sync_seg = texture_segment();
    mirror_synced.push_back(&p);
    std::vector<VkBufferCopy> runs;   // (from guest memory)
    std::map<VkBuffer, std::vector<VkBufferCopy>> cap_runs;   // (from the captures' copies)
    uint64_t run_lo = 0, run_hi = 0;
    const uint64_t end_page = (p.hi + 4095) >> kPageShift;
    const bool note_writes = memory_wb_followed(p.lo, p.hi);   // (wb_learn.h: once a pool, not a page)
    for (uint64_t page = p.lo >> kPageShift; page < end_page;) {
        if (region_gen[page / kRegionPages] <= p.synced_gen) {   // (a clean 2 MB region, or its rest when the pool starts in it)
            page = std::min(end_page, (page / kRegionPages + 1) * kRegionPages);
            continue;
        }
        const uint32_t pg = page_gen(page);
        const bool dirty = pg > p.synced_gen;
        if (dirty && gpu_page(page)) {   // (diagnostic: see memory_stamp_gpu)
            st_sync_gpu_pages++;
            const CapEntry e0 = cap_entry(page);
            const bool live = !(e0.gen && e0.gen == pg && cap_bufs.count(e0.gen));
            st_sync_gpu_live += live;
            static int logged;
            if (logged++ < 40)
                logf("vk: a mirror sync copies the CPU's version of page %#llx over the GPU's (pool %#llx, %s, page gen %u, "
                     "pool synced at %u, gen now %u)", (unsigned long long)(page << kPageShift), (unsigned long long)p.lo,
                     live ? "live" : "from a capture", pg, (unsigned)p.synced_gen, gen_now);
        }
        // (hookshot) the CPU wrote this page: in a large binding whose write-back may be left out, the copy below would bring
        // guest memory's stale rest of the page over the GPU's: that binding is written back from now on (wb_learn.h)
        if (dirty && note_writes) memory_note_cpu_access(page << kPageShift, 1ull << kPageShift, true);
        if (dirty && protect_bare_on() && prot_page_since(page, p.synced_gen)) {   // (the GPU's records kept: see prot_pages)
            st_prot_kept++;
            page++;
            continue;
        }
        if (dirty && !sync_live()) {   // (as a capture took it, if the page was not written since)
            const CapEntry e = cap_entry(page);
            if (e.gen && e.gen == pg) {
                auto b = cap_bufs.find(e.gen);
                if (b != cap_bufs.end()) {
                    const uint64_t ga = std::max(page << kPageShift, p.lo), gb = std::min((page + 1) << kPageShift, p.hi);
                    append_capture_copy(cap_runs[b->second.buffer],
                        {(uint64_t)e.slot * 4096 + (ga - (page << kPageShift)), ga - p.lo, gb - ga});
                    b->second.last_use = std::max(b->second.last_use, cur_cap_value);
                    (e.gen == cur_cap_gen ? st_sync_cap_pages : st_sync_old_pages)++;
                    page++;
                    continue;
                }
            }
        }
        if (dirty) {
            st_sync_live_pages++;   // (dirty, not in this submit's capture: guest memory as it is now)
            live_by_pool[p.lo]++;
            {   // (RX_VK_LOG_LIVE=1: why, 300 lines: the page's generation, its entry's, the newest capture's)
                static const bool log_live = env_on("RX_VK_LOG_LIVE");
                static int logged;
                if (log_live && logged < 300 && (++live_seen % 97) == 0) {
                    logged++;
                    const CapEntry e = cap_entry(page);
                    logf("vk: live page %#llx: gen %u, entry gen %u (buffer %s), current capture %u, pool synced at %u, gen now %u",
                         (unsigned long long)(page << kPageShift), pg, e.gen,
                         e.gen ? (cap_bufs.count(e.gen) ? "kept" : "gone") : "-", cur_cap_gen, (unsigned)p.synced_gen, gen_now);
                }
            }
            const uint64_t a = std::max(page << kPageShift, p.lo) - p.lo, b = std::min((page + 1) << kPageShift, p.hi) - p.lo;
            if (run_hi == a && run_hi) run_hi = b;
            else {
                if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
                run_lo = a, run_hi = b;
            }
        }
        page++;
    }
    if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
    for (const auto& [buf, rs] : cap_runs) {
        GprofSpan gp(upload_command_buffer(), "msync copies (CPU pages from a capture -> mirror)");
        gp.items = (int)rs.size();
        vkCmdCopyBuffer(upload_command_buffer(), buf, p.mirror, (uint32_t)rs.size(), rs.data());
        for (const VkBufferCopy& r : rs) st_sync_bytes += (int64_t)r.size, gp.bytes += r.size;
        st_sync_copies += (int64_t)rs.size();
    }
    if (runs.empty()) return;
    checkpoint_mark(upload_command_buffer(), p.lo, 'm');
    GprofSpan gp(upload_command_buffer(), "msync copies (CPU pages live from guest memory -> mirror)");
    gp.items = (int)runs.size();
    note_in_place_read();   // (these pages as guest memory has them when the GPU gets here)
    vkCmdCopyBuffer(upload_command_buffer(), p.buffer, p.mirror, (uint32_t)runs.size(), runs.data());
    for (const VkBufferCopy& r : runs) st_sync_bytes += (int64_t)r.size, gp.bytes += r.size;
    st_sync_copies += (int64_t)runs.size();
}

} // namespace

// ---- (hookshot) Large written bindings the CPU does not read: no write-back (RX_VK_WB_MAX_KB, wb_learn.h) ----
// The table is fed from the bindings (wb_leave_out, the replay's threads, pools_mu held), the CPU's reads (memory_note_cpu_read,
// invalidate on the game's threads) and the CPU's writes that a mirror sync copies (mirror_sync); its own mutex, taken last.
// RX_VK_WB_MAX_KB=<KB>: the size from which a binding may be left out (unset or 0: every written binding written back, the
// default); RX_VK_WB_PROBATION=<submissions>: how many submissions write it whole before it is (8)
std::mutex wb_learn_mu;
wb_learn::Table wb_table;
int64_t st_wb_left, st_wb_left_bytes, st_wb_stale;
uint64_t wb_cap() {
    static const uint64_t v = [] {
        const uint64_t b = wb_learn::max_bytes(getenv("RX_VK_WB_MAX_KB"));
        wb_table.probation = (uint32_t)std::max(1, env_int("RX_VK_WB_PROBATION", (int)wb_table.probation));
        if (b) logf("vk: written bindings over %llu KB are written back only where the CPU reads or writes them, after %u submissions "
                    "whole (RX_VK_WB_MAX_KB)", (unsigned long long)(b >> 10), wb_table.probation);
        return b;
    }();
    return v;
}
// (a written binding [lo, hi): false, write it all back; true, only the guest ranges in keep, the parts the CPU has touched)
bool wb_leave_out(uint64_t lo, uint64_t hi, std::vector<std::pair<uint64_t, uint64_t>>& keep) {
    if (!wb_cap()) return false;
    std::lock_guard lk(wb_learn_mu);
    if (!wb_table.plan(lo, hi, wb_cap(), mirror_epoch, keep)) return false;   // (mirror_epoch: the submission)
    uint64_t kept = 0;
    for (const auto& k : keep) kept += k.second - k.first;
    st_wb_left++, st_wb_left_bytes += (int64_t)(hi - lo - kept);
    return true;
}
void memory_note_cpu_access(uint64_t guest, uint64_t size, bool write) {
    if (!size || !wb_cap()) return;
    bool stale;
    {
        std::lock_guard lk(wb_learn_mu);
        // (only in a large binding followed: the CPU's other reads and writes, all over memory, would only fill the table)
        if (!wb_table.followed(guest, guest + size)) return;
        stale = wb_table.read(guest, guest + size);
        if (stale) st_wb_stale++;
    }
    static std::atomic<int> told{0};
    if (stale && told++ < 20)
        logf("vk: the CPU %s %#llx +%#llx, in a binding whose write-back was left out: written back from now on (RX_VK_WB_MAX_KB=0: "
             "every one)", write ? "wrote" : "read", (unsigned long long)guest, (unsigned long long)size);
}
void memory_note_cpu_read(uint64_t guest, uint64_t size) { memory_note_cpu_access(guest, size, false); }
bool memory_wb_followed(uint64_t lo, uint64_t hi) {
    if (!wb_cap()) return false;
    std::lock_guard lk(wb_learn_mu);
    return wb_table.followed(lo, hi);
}
static void wb_learn_report() {   // (memory_end_submission, every 600 submits)
    if (!wb_cap()) return;
    std::lock_guard lk(wb_learn_mu);
    logf("vk: write-backs left out (600 submits): %lld, %.1f MB; %zu bindings followed, %zu ranges the CPU read, %llu resets; CPU reads "
         "or writes of one left out: %lld so far", (long long)st_wb_left, st_wb_left_bytes / 1048576.0, wb_table.seen(), wb_table.reads(),
         (unsigned long long)wb_table.resets(), (long long)st_wb_stale);
    st_wb_left = st_wb_left_bytes = 0;
}
// (hookshot, diagnostics) RX_VK_LOG_WB_SKIPS=1: the written bindings left out, by guest range, every 600 submits
struct WbSkip { uint64_t hi, n, bytes; };
std::mutex wb_skips_mu;
std::map<uint64_t, WbSkip> wb_skips;   // (by guest start)
static bool wb_skips_log() {
    static const bool v = env_on("RX_VK_LOG_WB_SKIPS");
    return v;
}
static void wb_skip_note(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(wb_skips_mu);
    WbSkip& s = wb_skips[lo];
    s.hi = std::max(s.hi, hi), s.n++, s.bytes += hi - lo;
}
static void wb_skips_report() {
    std::vector<std::pair<uint64_t, uint64_t>> top;   // (bytes, lo)
    uint64_t all = 0;
    std::lock_guard lk(wb_skips_mu);
    for (const auto& [lo, s] : wb_skips) top.push_back({s.bytes, lo}), all += s.bytes;
    std::sort(top.rbegin(), top.rend());
    std::string t;
    for (size_t i = 0; i < top.size() && i < 12; i++) {
        const WbSkip& s = wb_skips[top[i].second];
        char b[112];
        snprintf(b, sizeof b, " %#llx+%#llx: %llu, %.1f MB;", (unsigned long long)top[i].second,
                 (unsigned long long)(s.hi - top[i].second), (unsigned long long)s.n, s.bytes / 1048576.0);
        t += b;
    }
    logf("vk: write-backs skipped (RX_VK_WB_MAX_KB, 600 submits): %zu ranges, %.1f MB; the largest:%s", wb_skips.size(),
         all / 1048576.0, t.c_str());
    for (auto& kv : wb_skips) kv.second.bytes = 0, kv.second.n = 0;
}

// ---- (from PR 430, YunaraDebugs) Readback pages: GPU results the CPU reads (invalidate) and then writes, as OoT's exposure meter (a
// sum the GPU adds into each frame; the CPU reads it and stores 0 back). On the console the GPU adds into that memory as it is when
// its work runs, the CPU's 0 included. Here it adds into the pool's mirror, which takes the CPU's 0 only at a mirror sync of a submit
// made after it: one made just before (the game's threads overlap) added onto the old sum, the CPU read 2x the next frame and 0 the
// one after, and the exposure stepped (the light dimming or flaring for a frame in Kokiri). So each submission first copies those
// pages from guest memory into the mirror, on the GPU as its work starts, last of the uploads: what the CPU wrote by then is what the
// GPU adds to. RX_VK_NO_RB_REFRESH=1: off ----
// (hookshot, 11-09) A parallel replay's recordings share one submission (mirror_epoch) and its one write-back at its end: the part
// before the segments, each segment and the recording after them (vk_replay.cpp replay_handle). A refresh in a later one copied the
// guest's page, which has none of the submission's GPU writes, over what the recordings ahead of it had written in the mirror: OoT's
// exposure sum came back ~0 and every frame was too bright (RX_VK_PARALLEL=4). So a recording that joined an open submission
// refreshes a page only when every recording ahead of it in the queue is made and none of them may have written the page: the copy
// then brings the CPU's writes and wipes nothing (and a page first read in the middle of a submission is refreshed ahead of its first
// use, unless the recordings up to that use were all made before it was read). What they may have written are the GPU write
// hazards: every range bound for writing in the mirror, and the ranges a program may write that are not copied back (a bare table
// entry's guessed range past its first 64 bytes, the pointer table's, a wide binding: vk_draw.cpp, memory_note_gpu_write). A
// recording that opens its submission, or comes after its write-back (frame generation's), refreshes every page as before: serial
// replay records the same copies. With RX_VK_FOLLOW_STORES (stores through the page table, bound nowhere) a joining one refreshes
// none. RX_VK_RB_REFRESH_EACH=1: in every recording, as before
namespace {
std::unordered_map<uint32_t, uint64_t> rb_pages;   // (guest page -> mirror_epoch it was last read at)
std::atomic<int64_t> st_rb_refresh{0}, st_rb_held{0};
// (the open submission the recording this thread is making joined; 0: none: it opens one, or comes after one's end)
thread_local uint64_t rb_joined = 0;
// (the GPU write hazards of submission rb_hazards_epoch, by guest address, with their recording's place in the queue
// (texture_segment); kept past its write-back, for the recording that ends it)
struct RbHazard {
    uint64_t lo, hi, seg;
};
std::vector<RbHazard> rb_hazards;
uint64_t rb_hazards_epoch = 0;
// (a parallel replay's segments: the submission, the first one's order (texture_segment) and how many; the ones made, by order)
struct RbSegments {
    uint64_t epoch = 0, base = 0, n = 0, made = 0;
} rb_segs;
void rb_hazard_locked(uint64_t lo, uint64_t hi) {   // (pools_mu held)
    if (rb_hazards_epoch != mirror_epoch) rb_hazards.clear(), rb_hazards_epoch = mirror_epoch;
    rb_hazards.push_back({lo, hi, texture_segment()});
}
}
void memory_note_readback(uint64_t guest, uint64_t size) {
    static const bool off = env_on("RX_VK_NO_RB_REFRESH");
    if (off || !size || size > 0x4000) return;   // (small results only: meters and counters)
    std::lock_guard lk(pools_mu);
    for (uint64_t pg = guest >> kPageShift; pg <= (guest + size - 1) >> kPageShift; pg++) rb_pages[(uint32_t)pg] = mirror_epoch;
}
void memory_note_gpu_write(uint64_t gpu, uint64_t size) {
    if (!size) return;
    uint64_t guest = gpu;
    if (gpu >= (1ull << 40)) {   // (as gpu_buffer_bind)
        uint64_t avail = 0;
        if (!host->gpu_ptr(gpu, &avail) || avail < size) return;
        guest = host->gpu_guest(gpu);
    }
    std::lock_guard lk(pools_mu);
    rb_hazard_locked(guest, guest + size);
    // (and the pools it reaches keep their mirrors: imports are at most 1 GB, so none further down reaches guest)
    for (auto it = imported.upper_bound(guest + size - 1); it != imported.begin();) {
        --it;
        if (it->second.hi > guest) it->second.gpu_only = true;
        else if (it->second.hi + (1ull << 30) < guest) break;
    }
}
void memory_rb_recording_begun() {
    std::lock_guard lk(pools_mu);
    rb_joined = mirror_upload ? mirror_epoch : 0;
}
void memory_rb_segments_begin(uint64_t base, uint64_t n) {
    std::lock_guard lk(pools_mu);
    rb_segs = {mirror_epoch, base, n, 0};
}
/// Records the refresh copies into the submission's upload command buffer; true when it recorded any
bool memory_rb_refresh(VkCommandBuffer up) {
    std::lock_guard lk(pools_mu);
    static const bool each = env_on("RX_VK_RB_REFRESH_EACH");
    static const bool follow_stores = env_on("RX_VK_FOLLOW_STORES");
    const bool joined = !each && rb_joined && rb_joined == mirror_epoch;
    const uint64_t seg = texture_segment();
    bool ahead_made = !follow_stores;   // (every recording ahead of this one in the queue made, and what they may write known)
    if (joined && rb_segs.epoch == mirror_epoch && seg >= rb_segs.base && seg - rb_segs.base < rb_segs.n) {
        const uint64_t k = seg - rb_segs.base;   // (a segment's: the ones before it made, and this one now)
        const uint64_t below = k < 64 ? (1ull << k) - 1 : ~0ull;
        ahead_made = ahead_made && k < 64 && (rb_segs.made & below) == below;
        if (k < 64) rb_segs.made |= 1ull << k;
    }
    if (rb_pages.empty() || !up) return false;
    auto written_ahead = [&](uint64_t lo, uint64_t hi) {   // (may be, by a recording ahead of this one in its submission)
        if (rb_hazards_epoch != mirror_epoch) return false;
        for (const RbHazard& h : rb_hazards)
            if (h.seg < seg && h.lo < hi && lo < h.hi) return true;
        return false;
    };
    std::map<Pool*, std::vector<VkBufferCopy>> by_pool;
    for (auto it = rb_pages.begin(); it != rb_pages.end();) {
        if (it->second + 600 < mirror_epoch) { it = rb_pages.erase(it); continue; }   // (not read for a while)
        const uint64_t g = (uint64_t)it->first << kPageShift;
        ++it;
        auto pit = imported.upper_bound(g);
        if (pit == imported.begin()) continue;
        --pit;
        Pool& p = pit->second;
        if (g < p.lo || g >= p.hi || p.mstate != Pool::MIRROR_ON || !p.mirror) continue;
        const uint64_t a = g - p.lo, b = std::min(g + 4096, p.hi) - p.lo;
        if (joined && (!ahead_made || written_ahead(p.lo + a, p.lo + b))) {   // (not here: see above)
            st_rb_held++;
            continue;
        }
        by_pool[&p].push_back({a, a, b - a});
    }
    if (by_pool.empty()) return false;
    full_barrier(up);   // (after the older submissions' write-backs into guest memory and this one's other uploads)
    GprofSpan gp(up, "rbrefresh copies (readback pages guest -> mirror)");
    gp.items = 0;
    for (auto& [pp, rs] : by_pool) {
        st_rb_refresh += (int64_t)rs.size();
        // (hookshot) pages next to each other as one copy (~530 a frame in Hyrule Field, a copy a page)
        std::sort(rs.begin(), rs.end(), [](const VkBufferCopy& x, const VkBufferCopy& y) { return x.srcOffset < y.srcOffset; });
        size_t k = 0;
        for (size_t i = 1; i < rs.size(); i++)
            if (rs[k].srcOffset + rs[k].size == rs[i].srcOffset) rs[k].size += rs[i].size;
            else rs[++k] = rs[i];
        rs.resize(k + 1);
        // (guest memory at the GPU's time, and not noted for the readback's part wait: see note_in_place_read)
        vkCmdCopyBuffer(up, pp->buffer, pp->mirror, (uint32_t)rs.size(), rs.data());
        gp.items += (int)rs.size();
        for (const VkBufferCopy& r : rs) gp.bytes += r.size;
    }
    static std::atomic<int64_t> n{0};
    if (++n % 3000 == 1)
        logf("vk: readback pages refreshed from guest memory: %lld pages so far (%lld held: may be written ahead in their submission)",
             (long long)st_rb_refresh.load(), (long long)st_rb_held.load());
    return true;
}

BufferRef gpu_buffer(uint64_t gpu, uint64_t size) {
    uint64_t avail = 0;
    if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
    const uint64_t guest = host->gpu_guest(gpu);
    std::lock_guard lk(pools_mu);
    Pool* p = pool_of(guest, size);
    if (!p) return {};
    if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;   // (in use: not given back under it, imports_give_back)
    note_in_place_read();   // (the pool itself, read by the GPU where the caller's work runs)
    return {p->buffer, guest - p->lo, size, p->address + (guest - p->lo)};
}

void memory_pool_created(uint64_t gpu, uint64_t size) {
    // (RX_VK_BG_IMPORT=1 only: imported eagerly, every pool the game made, most never used by the GPU, the thread's
    // page locking stalled the game's threads (3-7 fps); chunked imports at first use are the default)
    static const bool on = env_on("RX_VK_BG_IMPORT");
    if (!on || !size || gpu >= (1ull << 40) || !dev.external_memory_host) return;   // (virtual pools are not imported)
    const uint64_t lo = gpu & ~(align - 1), hi = (gpu + size + align - 1) & ~(align - 1);
    std::lock_guard lk(bg_mu);
    bg_start();
    if (std::find(bg_inflight.begin(), bg_inflight.end(), lo) != bg_inflight.end()) return;
    bg_inflight.push_back(lo);
    bg_queue.push_back({lo, hi});
    bg_cv.notify_one();
}

int memory_guest_imported(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(pools_mu);
    auto it = imported.upper_bound(hi);   // (pools by lo: those starting below hi, the last ones checked first)
    while (it != imported.begin()) {
        --it;
        if (it->second.hi > lo) return 1;
        if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
    }
    for (const auto& nh : retired_pools)
        if (nh.mapped().memory && nh.mapped().lo < hi && lo < nh.mapped().hi) return 1;
    return 0;
}

bool mirror_make(Pool& pool);   // (below)
/// The mirrored pools, for the submit-time capture (memory_capture_submit) (pools_mu held)
void publish_cap_ranges() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, pool] : imported)
        if (pool.mstate == Pool::MIRROR_ON || pool.was_mirrored) v.push_back({pool.lo, pool.hi});
    std::lock_guard lk(cap_ranges_mu);
    cap_ranges.swap(v);
    cap_ranges_ver++;
}
// (EvilOOT, 11-07) the game's own memory pool holding a GPU address (below 1 TB, where the address is the guest's), not the
// import that happens to cover it: imports are 16 MB chunks around what was bound, and since upstream 1c7c9c7 only around
// the bound range, so the pool's end was a chunk's end: the pointer tables of the task and mesh stages (whole pools) and the
// bare entries stopped there, the grass's programs read their records 12-18 MB further on as 0 and culled every blade
static bool game_pool_of(uint64_t gpu, uint64_t& lo, uint64_t& hi) {   // (pools_mu held)
    if (gpu >= (1ull << 40) || guest_pools.empty()) return false;
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), gpu, [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return false;
    --it;
    if (gpu < it->lo || gpu >= it->hi) return false;
    lo = it->lo, hi = it->hi;
    return true;
}
/// True when gpu sits in [lo, hi) and that span was cached from the guest-pool table this generation still is.
bool guest_span_covers(uint64_t gpu, uint64_t lo, uint64_t hi, uint64_t cached_gen, uint64_t gen) {
    return cached_gen == gen && lo < hi && gpu >= lo && gpu < hi;
}
struct GuestSpan { uint64_t lo = 1, hi = 0, gen = 0; };
thread_local GuestSpan guest_span;
bool guest_span_cached(uint64_t gpu, uint64_t* lo, uint64_t* hi) {
    static const bool imported_only = env_on("RX_VK_POOL_END_IMPORTED");   // (A/B: the import's end, as before)
    if (imported_only || gpu >= (1ull << 40)) return false;
    const uint64_t gen = guest_pools_gen.load(std::memory_order_relaxed);
    if (!guest_span_covers(gpu, guest_span.lo, guest_span.hi, guest_span.gen, gen)) return false;
    *lo = guest_span.lo;
    *hi = guest_span.hi;
    return true;
}
void guest_span_store(uint64_t lo, uint64_t hi) {   // (pools_mu held: the generation matches the table just read)
    guest_span = {lo, hi, guest_pools_gen.load(std::memory_order_relaxed)};
}
uint64_t gpu_pool_end(uint64_t gpu) {
    uint64_t lo, hi;
    // (storage-buffer tables ask this per entry, and the entries of one table sit in one pool: the locked lookup of a
    // pool was ~8% of replay time before buffer binds cached it)
    if (guest_span_cached(gpu, &lo, &hi)) return hi;
    ParLock plk;   // (before pools_mu, always)
    std::lock_guard lk(pools_mu);
    static const bool imported_only = env_on("RX_VK_POOL_END_IMPORTED");   // (A/B: the import's end, as before)
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return hi;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->hi : 0;
}
uint64_t gpu_pool_start(uint64_t gpu) {   // (EvilOOT) where it begins
    uint64_t lo, hi;
    if (guest_span_cached(gpu, &lo, &hi)) return lo;
    ParLock plk;
    std::lock_guard lk(pools_mu);
    static const bool imported_only = env_on("RX_VK_POOL_END_IMPORTED");
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return lo;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->lo : 0;
}

// (pools leaving, a mirror made, or the next submission: a remembered read binding is looked up again)
uint64_t gpu_bind_stamp() {
    return pools_gen.load(std::memory_order_relaxed) ^ (mirror_epoch * 0x9E3779B97F4A7C15ull);
}

uint32_t bind_slot(uint64_t guest, uint64_t size) {
    // (a page-aligned range's low bits are zero: fold the rest in, or every one of them lands in one slot)
    uint64_t h = guest ^ (size * 0x9E3779B97F4A7C15ull);
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    h ^= h >> 16;
    return (uint32_t)(h & 255u);
}

bool bind_memo_hit(uint64_t guest, uint64_t size, uint64_t stamp, uint64_t cached_guest, uint64_t cached_size,
                   uint64_t cached_stamp, int has_buffer) {
    return has_buffer && size && guest == cached_guest && size == cached_size && stamp == cached_stamp;
}

struct BindMemo {
    uint64_t guest = 0, size = 0, stamp = 0;
    BufferRef ref{};
};
thread_local BindMemo bind_memo[256];

void note_direct_write(Pool& p, uint64_t off, uint64_t size) {   // (pools_mu held)
    p.direct_write_epoch = mirror_epoch;
    if (size <= (1u << 20))   // (EvilBoyce: GPU results written in place; small ones: the readbacks are)
        wb_pages_note(p.lo + off, p.lo + std::min(off + size, p.hi - p.lo), mem_rec_value, 0);
}

BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {
    if (written) texture_gpu_wrote(gpu, size);
    // (below 1 TB a GPU address is the guest address itself, and a pool holds the whole range or nothing: the host's
    // two translations are for the virtual pools above)
    uint64_t guest = gpu;
    if (gpu >= (1ull << 40)) {
        uint64_t avail = 0;
        if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
        guest = host->gpu_guest(gpu);
    }
    // (the same read, while that stamp still stands: the buffer and the offset are the ones found before. a miss
    // runs the lookup below, including a mirror sync, and remembers what it returned. a write is not remembered)
    auto remember = [&](BufferRef ref) {
        if (!written && ref.buffer != VK_NULL_HANDLE && size)
            bind_memo[bind_slot(guest, size)] = BindMemo{guest, size, gpu_bind_stamp(), ref};
        return ref;
    };
    if (!written) {
        const BindMemo& m = bind_memo[bind_slot(guest, size)];
        if (bind_memo_hit(guest, size, gpu_bind_stamp(), m.guest, m.size, m.stamp, m.ref.buffer != VK_NULL_HANDLE))
            return m.ref;
    }
    // (a read binding of a pool this thread found before and whose mirror is in use this submission or that has none,
    // without the lock: every thread's, the replay thread's too (each draw binds several uniform buffers: the pool's
    // lookup under the lock was ~8% of the replay's time in a race); the rest under it, as below)
    const bool par = par_active.load(std::memory_order_relaxed);
    TlPool& tl = tl_pools[(guest >> 16) & 1023];
    if (!written && tl.p && tl.gen == pools_gen.load(std::memory_order_relaxed) && guest >= tl.p->lo &&
        guest <= tl.p->hi && size <= tl.p->hi - guest) {   // (hookshot: no wrap-around for a huge size, as pool_of)
        Pool* p = tl.p;
        const uint64_t off = guest - p->lo;
        if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
        if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED || p->mstate == Pool::MIRROR_WANTED) {
            note_in_place_read(!mirror_upload);
            return remember({p->buffer, off, size, 0});
        }
        if (p->mstate == Pool::MIRROR_ON && p->synced_epoch == mirror_epoch && p->sync_seg <= texture_segment())
            return remember({p->mirror, off, size, 0});
    }
    ParLock plk;   // (before pools_mu, always: a mirror sync records into the shared upload command buffer)
    std::lock_guard lk(pools_mu);
    {   // (hookshot, smoothness: import_in_background above. Reads up to 4 MB; not in a parallel replay's segment)
        static const bool async_import = [] { const char* e = getenv("RX_VK_ASYNC_IMPORT"); return e && atoi(e) != 0; }();
        if (async_import && !written && !par && size <= (4u << 20) && guest < (1ull << 40) &&
            !(last_pool && guest >= last_pool->lo && guest + size <= last_pool->hi) && import_in_background(guest, size)) {
            uint64_t avail = 0;
            const uint8_t* src = host->gpu_ptr(gpu, &avail);
            if (src && avail >= size)
                if (const BufferRef r = stream_copy(src, size)) {
                    st_async_copies++;
                    return r;
                }
        }
    }
    Pool* p = pool_of(guest, size);
    if (!p) return {};
    tl = {p, pools_gen.load(std::memory_order_relaxed)};
    if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
    const uint64_t off = guest - p->lo;
    if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED) {
        if (written) note_direct_write(*p, off, size);
        if (mirror_on()) { auto& e = direct_by_pool[p->lo]; e.first++, e.second = mirror_upload ? (int)p->mstate : 9; direct_bytes_by_pool[p->lo] += size; }
        // (hookshot) written by the GPU in guest memory itself, past the mirror (bound outside a recording's uploads): the
        // write-back diff's shadow no longer knows those pages, and reads them again at its next write-back, after this write
        if (written) shadow_forget(*p, off, std::min(size, p->hi - p->lo - off));
        note_in_place_read(!mirror_upload);
        return remember({p->buffer, off, size, 0});
    }
    // (a pool's mirror is made at its first binding, in this part's uploads: bound in guest memory until the next
    // submission, the GPU read it there while the game's threads wrote it for their next frames, and a pass that drew
    // over the moment a write landed came out in two (shadows dark below a line for a frame, the line moving; a new
    // pool every so many frames as the per-frame data moves on). Not when this part already wrote it in guest memory:
    // the copy at the part's start would miss that. Not in a parallel replay's segment either: its copy goes into the
    // uploads of the segment whose worker bound the pool first, which need not be the first in the queue. A segment
    // before it found the mirror in the page table and read it unfilled (houses, rocks and bushes missing, geometry
    // torn, the more the segments the worse; #994 with RX_VK_PARALLEL). RX_VK_MIRROR_DEFER=1: at the next submission, as before)
    static const bool defer = env_on("RX_VK_MIRROR_DEFER");
    if (p->mstate == Pool::MIRROR_NONE && !defer && !par && p->direct_write_epoch != mirror_epoch && mirror_make(*p)) {
        publish_cap_ranges();
        static int64_t made_now;
        if (++made_now % 200 == 1)
            logf("vk: pool %#llx (%llu MB): mirror made at its first binding (%lld so)", (unsigned long long)p->lo,
                 (unsigned long long)((p->hi - p->lo) >> 20), (long long)made_now);
    }
    if (p->mstate == Pool::MIRROR_NONE) {   // (made at the next submission's start)
        p->mstate = Pool::MIRROR_WANTED;
        mirror_wanted.push_back(p);
    }
    if (p->mstate != Pool::MIRROR_ON) {
        auto& e = direct_by_pool[p->lo];
        e.first++, e.second = (int)p->mstate;
        direct_bytes_by_pool[p->lo] += size;
        if (written) note_direct_write(*p, off, size);
        note_in_place_read();
        return remember({p->buffer, off, size, 0});
    }
    if (p->synced_epoch != mirror_epoch || p->sync_seg > texture_segment()) mirror_sync(*p);
    if (written) {
        p->gpu_written = true;   // (its mirror kept: mirrors_give_back)
        // (hookshot) a binding over RX_VK_WB_MAX_KB (off unless set; 1024 is the tested size) that the CPU has not read through its
        // probation queues no write-back: OoT writes tens of MB of culling lists and per-frame buffers through the GPU that
        // the CPU never reads, and copying them back was most of a submit's memory sync (issue 384: 68 MB a frame; Hyrule
        // Field on an RTX 3060 Ti, 68 -> 18 MB written back a frame). Learned, not a plain size cap: the CPU does read
        // some large ones (wb_learn.h)
        const uint64_t wlo = p->lo + off, whi = p->lo + std::min(off + size, p->hi - p->lo);
        static thread_local std::vector<std::pair<uint64_t, uint64_t>> keep;
        if (!wb_leave_out(wlo, whi, keep)) writebacks.push_back({p, off, whi - p->lo});
        else {
            for (const auto& [klo, khi] : keep) writebacks.push_back({p, klo - p->lo, khi - p->lo});
            if (wb_skips_log()) wb_skip_note(wlo, whi);
        }
        rb_hazard_locked(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo));   // (memory_rb_refresh)
    }
    return remember({p->mirror, off, size, 0});
}

// ---- mirrors given back ----
// A mirror was never freed: the retired pools' stayed (every pool remade larger, or taken in by a larger one, kept
// its VRAM), and pools the game no longer used kept theirs until the mirrors' budget was full; the pools after that
// stayed in system memory for good (the GPU read them over PCIe). Now a retired pool's mirror goes once the GPU is done
// with the submissions that bound it; a mirror not bound for kIdleEpochs submissions goes when a wanted one does not
// fit, or after kLongIdle; and a pool refused for the budget is tried again when it is next bound.
constexpr uint64_t kIdleEpochs = 600, kLongIdle = 6000;
int64_t st_mirrors_freed = 0;
uint64_t st_mirror_bytes_freed = 0;
// (hookshot) A freed mirror's buffer is destroyed only once the GPU is past everything recorded until then. Its pool's
// last_value counts the draws and dispatches that bound it, not the shaders that reach it through the guest page table:
// a culling or fog compute pass already running read a mirror destroyed under it (device lost in 53caac5f and ee7764a6,
// issues 132 and 195, AMD after a scene change). The table goes back to guest memory at once, as before
struct DeadMirror { VkBuffer buffer; VmaAllocation alloc; uint64_t value; };
std::deque<DeadMirror> dead_mirrors;
// A retired pool's import, given back the same way. Each import is a userptr buffer object to amdgpu, and RADV lists every
// buffer object in every submission (buffer device addresses): the kernel checks every page of every userptr object at each
// DRM_IOCTL_AMDGPU_CS. Retired pools were never given back, so the pages it checked only grew with each pool merge: in Kokiri
// Forest ~1.3 GB of imports, 15-45 ms a submission (3-4 fps on an RX 7800 XT with 11 ms of GPU time; issues 312, 413). Its
// table entries went at its retirement, and its mirror goes before (mstate), so once the GPU is past retired_value nothing
// reaches it. The node stays (pointers to it are kept elsewhere); only its buffer and memory go. RX_VK_KEEP_RETIRED=1: kept
struct DeadImport { VkBuffer buffer; VkDeviceMemory memory; uint64_t value; uint64_t bytes; std::vector<VkDeviceMemory> pieces; };
std::deque<DeadImport> dead_imports;
uint64_t st_retired_imports_freed = 0, st_retired_import_bytes_freed = 0;
uint64_t dead_import_bytes = 0;   // (queued in dead_imports: still in the heap's use until the GPU is past them)
void reap_mirrors() {   // (pools_mu held)
    while (!dead_mirrors.empty() && dead_mirrors.front().value <= mem_completed) {
        if (env_on("RX_VK_LOG_ADDR")) {
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = dead_mirrors.front().buffer;
            logf("vk: addr mirror freed at gpu %#llx (value %llu, completed %llu)", (unsigned long long)vkGetBufferDeviceAddress(dev.device, &da),
                 (unsigned long long)dead_mirrors.front().value, (unsigned long long)mem_completed);
        }
        destroy_buffer(dead_mirrors.front().buffer, dead_mirrors.front().alloc);
        dead_mirrors.pop_front();
    }
    while (!dead_imports.empty() && dead_imports.front().value <= mem_completed) {
        destroy_import_buffer(dead_imports.front().buffer);
        vkFreeMemory(dev.device, dead_imports.front().memory, nullptr);
        for (VkDeviceMemory m : dead_imports.front().pieces) vkFreeMemory(dev.device, m, nullptr);
        st_retired_imports_freed++, st_retired_import_bytes_freed += dead_imports.front().bytes;
        dead_import_bytes -= std::min(dead_import_bytes, dead_imports.front().bytes);
        dead_imports.pop_front();
    }
}
bool system_heap_usage(uint64_t* used, uint64_t* budget);   // (vk_vma.cpp; said here too: the pressure probe below runs before imports_give_back's own forward of it)
/// Whether retired imports are kept, given back at once, or given back after a wait (retired_give_back.h)
retired_give_back::Mode retired_mode() {
    // (hookshot) RX_VK_KEEP_RETIRED=1 keeps them for the session and 0, off, false or no gives them back as soon as the
    // GPU is past them, on every OS and vendor. Unset or empty, the defaults are main's but for one: off Windows at once
    // (the amdgpu submit above, issues 312 and 413), on Windows NVIDIA at once too (kept, ~20 areas of play left 6.8 GB
    // of retired pools imported on an RTX 3070 (8 GB) and the next area lost the device, twice; issue 1057), and on
    // Windows AMD and Intel after a wait, where they were kept for the session: an Intel Arc B390 lost the device
    // entering Kokiri every time with the give-back and 1 run in 5 without (issue 17, fuxau), and the AMD device losses
    // on the Ally X (309) and RX 6600 (571) came right after retired pools went back. Kept, those two vendors' retired
    // pools only grew, multi-GB in a long session, until the system heap's budget was passed and new imports failed
#ifdef _WIN32
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    static const retired_give_back::Mode m = [] {
        const char* v = getenv("RX_VK_KEEP_RETIRED");
        return retired_give_back::mode(v && *v, env_on("RX_VK_KEEP_RETIRED"), windows, dev.props.vendorID == 0x10DE);
    }();
    return m;
}
void retired_imports_give_back() {   // (pools_mu held)
    using retired_give_back::Mode;
    const Mode mode = retired_mode();
    // (hookshot) The wait (Windows AMD and Intel, retired_mode): a retired pool's import is given back once the GPU is
    // past it and it has been retired for RX_VK_RETIRE_SECS seconds (default 30), oldest first; while the system heap
    // is past RX_VK_IMPORT_HIGH percent of its budget (default 85; 0: off) the wait is RX_VK_RETIRE_MIN_SECS seconds
    // (default 2), until what brings it down to RX_VK_IMPORT_LOW percent (75) is queued. The free itself still waits
    // for the GPU (dead_imports). Elsewhere those switches are not read and nothing waits. Unlike imports_give_back's
    // valve this one is on by default on Windows and on any budget, not only under 12 GB: it frees pools nothing uses
    // any more, where that one takes pools still live but idle
    static const uint64_t idle_ms = (uint64_t)std::max(1, env_int("RX_VK_RETIRE_SECS", 30)) * 1000;
    static const uint64_t floor_ms = (uint64_t)std::max(0, env_int("RX_VK_RETIRE_MIN_SECS", 2)) * 1000;
    static const int high = std::clamp(env_int("RX_VK_IMPORT_HIGH", 85), 0, 100);   // (0: off, as in imports_give_back)
    static const int low = std::clamp(env_int("RX_VK_IMPORT_LOW", 75), 10, 95);
    // (the driver asked every 16th submission, as imports_give_back does. What is already queued counts as given back:
    // the heap's use does not go down until the GPU is past it, and the next probe would queue for it again)
    uint64_t want = 0;
    if (mode == Mode::GRACE && high > 0) {
        static uint64_t calls = 0;
        uint64_t used = 0, budget = 0;
        if (++calls % 16 == 0 && system_heap_usage(&used, &budget))
            want = retired_give_back::want(retired_give_back::settled(used, dead_import_bytes), budget, high, low);
    }
    const int64_t now = mode == Mode::GRACE ? import_now_ms() : 0;
    uint64_t live_bytes = 0, retired_bytes = 0;
    size_t retired_alive = 0;
    for (auto& [lo, p] : imported) live_bytes += p.hi - p.lo;
    for (auto& nh : retired_pools) {   // (in the order they were retired: the oldest first)
        Pool& p = nh.mapped();
        if (!p.memory) continue;
        const uint64_t idle = (uint64_t)std::max<int64_t>(0, now - p.retired_ms);
        // (hookshot) a pool whose mirror was refused (over 1 GB, or no room in VRAM when it was asked for) has no mirror either:
        // its import goes as a MIRROR_NONE pool's does, as imports_give_back already lets it. Left out here, every such retired
        // pool kept its import, pinned system memory the driver counts as the GPU's shared memory, for the rest of the session;
        // the more of them the fuller VRAM was when they were refused
        if ((p.mstate == Pool::MIRROR_NONE || p.mstate == Pool::MIRROR_REFUSED) && !p.inherit_pending &&
            p.retired_value <= mem_completed &&
            p.last_value <= mem_completed && retired_give_back::due(mode, idle, idle_ms, floor_ms, want > 0)) {
            dead_imports.push_back({p.buffer, p.memory, std::max(mem_rec_value, p.last_value), p.hi - p.lo, std::move(p.pieces)});
            dead_import_bytes += p.hi - p.lo;
            p.pieces.clear();
            p.buffer = VK_NULL_HANDLE, p.memory = VK_NULL_HANDLE, p.address = 0;
            pools_gen++;
            want -= std::min(want, p.hi - p.lo);   // (the short wait ends once the low watermark's worth is queued)
            continue;
        }
        retired_alive++, retired_bytes += p.hi - p.lo;
    }
    static uint64_t logged_epoch;
    if (mirror_epoch - logged_epoch >= 1200) {
        logged_epoch = mirror_epoch;
        logf("vk: imports: %zu live (%llu MB), %zu retired still imported (%llu MB), %llu retired given back (%llu MB)",
             imported.size(), (unsigned long long)(live_bytes >> 20), retired_alive, (unsigned long long)(retired_bytes >> 20),
             (unsigned long long)st_retired_imports_freed, (unsigned long long)(st_retired_import_bytes_freed >> 20));
    }
}
void mirror_free(Pool& p) {
    p.was_mirrored = true;
    {   // (the table back to guest memory, for a pool still in use only: a retired one's range is another pool's now, and its
        // address there sent the foliage culling into freed memory, a device lost)
        const auto live = imported.find(p.lo);
        if (live != imported.end() && &live->second == &p) guest_pages_set(p.lo, p.hi, p.address);
    }
    dead_mirrors.push_back({p.mirror, p.mirror_alloc, std::max(mem_rec_value, p.last_value)});
    shadow_free(p);
    p.mirror = VK_NULL_HANDLE, p.mirror_alloc = nullptr;
    mirror_bytes -= p.hi - p.lo;
    st_mirror_bytes_freed += p.hi - p.lo;
    st_mirrors_freed++;
    p.mstate = Pool::MIRROR_NONE;
    pools_gen++;   // (a parallel replay thread's pool cache: looked up again)
}
void mirrors_give_back(uint64_t want_bytes) {   // (pools_mu held)
    static const bool keep = env_on("RX_VK_MIRROR_KEEP");   // (A/B: mirrors kept, as before)
    if (keep) return;
    // (the mirrors still given back, why, and what the GPU did to them: the first 200, to see which went before a picture fault)
    static int logged_frees = 0;
    const auto told = [](const Pool& p, const char* why) {
        if (logged_frees++ < 200)
            logf("vk: mirror of pool %#llx (%llu MB) given back: %s, %llu submissions since its sync%s%s", (unsigned long long)p.lo,
                 (unsigned long long)((p.hi - p.lo) >> 20), why, (unsigned long long)(mirror_epoch - p.synced_epoch),
                 p.gpu_only ? ", GPU-only data" : "", p.gpu_written ? ", GPU-written" : "");
    };
    for (auto& nh : retired_pools) {
        Pool& p = nh.mapped();
        // (EvilOOT: kept while the pool that took it in has not made its mirror, for 600 submissions at most)
        if (p.inherit_pending && mirror_epoch - p.retired_epoch > kIdleEpochs) p.inherit_pending = false, p.inherit_lost = true;
        if (p.mstate == Pool::MIRROR_ON && p.retired_value <= mem_completed && !p.inherit_pending) {
            told(p, p.inherit_lost ? "retired, not inherited in time" : "retired");
            mirror_free(p);
        }
    }
    // (hookshot) not a pool the GPU wrote past what is copied back (gpu_only), nor one it wrote through a binding (gpu_written): made
    // again, its mirror came from guest memory without that, and the GPU went on from stale data. Paused 1-2 minutes in the background, the idle pools' mirrors all went,
    // and the game came back with cyan for a few seconds (RX 5500M; gone with RX_VK_MIRROR_KEEP=1). A pool refused a mirror for
    // the budget meanwhile is read in system memory, slower, not wrong
    std::vector<Pool*> idle;
    uint64_t kept_bytes = 0;
    for (auto& [lo, p] : imported)
        if (p.mstate == Pool::MIRROR_ON && mirror_epoch - p.synced_epoch > kIdleEpochs && p.last_value <= mem_completed) {
            if (p.gpu_only || p.gpu_written) kept_bytes += p.hi - p.lo;
            else idle.push_back(&p);
        }
    std::sort(idle.begin(), idle.end(), [](const Pool* a, const Pool* b) { return a->synced_epoch < b->synced_epoch; });
    for (Pool* p : idle) {
        const bool long_idle = mirror_epoch - p->synced_epoch > kLongIdle;
        if (long_idle || mirror_bytes + want_bytes > mirror_budget()) told(*p, long_idle ? "idle" : "idle, for room"), mirror_free(*p);
    }
    static int64_t logged_at;
    if (st_mirrors_freed / 16 != logged_at) {
        logged_at = st_mirrors_freed / 16;
        logf("vk: %lld mirrors given back (%llu MB in all); %llu MB of mirrors now, %llu MB of them idle and kept for what the GPU "
             "wrote there alone", (long long)st_mirrors_freed, (unsigned long long)(st_mirror_bytes_freed >> 20),
             (unsigned long long)(mirror_bytes >> 20), (unsigned long long)(kept_bytes >> 20));
    }
}

// ---- imports given back under pressure. The driver pins every imported pool and counts it in the system heap
// the GPU reaches, whose budget is about half the PC's RAM (7.5 GB of 16, 15.4 GB of 32): imports were kept for the session,
// the heap went past its budget (7758 of 7503 MB on an RTX 3060 with 16 GB) and new imports failed with
// VK_ERROR_OUT_OF_DEVICE_MEMORY: terrain left out, load zones stuck; on 32 GB+ Linux PCs (where the budget is 12 GB or more
// and this valve was off) 200+ idle unmirrored pools piled up over a two-hour session (7.4 GB live), bloating every amdgpu
// submit's buffer list and the submit-time capture scan. Past RX_VK_IMPORT_HIGH percent (85) of the effective budget the pools
// bound least lately give their imports back down to RX_VK_IMPORT_LOW percent (75): only pools without a mirror, not bound for
// kImportIdle submissions and done on the GPU. Under a 12 GB heap budget (16 GB PCs and the Steam Deck) the effective budget is
// the heap's own, as before; at 12 GB and above it is capped at 4500 MB by default (RX_VK_IMPORT_CAP_MB=<MB> sets another cap,
// and RX_VK_IMPORT_CAP_MB=0 turns the >= 12 GB cap off; setting RX_VK_IMPORT_HIGH or RX_VK_IMPORT_LOW explicitly without
// RX_VK_IMPORT_CAP_MB uses the full heap budget instead of the 4500 MB cap). They leave the table as a retired pool does (the
// page table's entries 0, the caches emptied) and are imported again at their next binding; their memory is freed once
// everything recorded until now is done. On by default on Linux; off by default on Windows. RX_VK_IMPORT_HIGH=0: off, =85: on
// anywhere against the full budget. (Caveat: last_value counts bindings, not shaders reaching a pool through the page table; a
// pool only reached that way would read zeros until it is bound again. Not seen in play: the pools given back were long
// unbound.) ----
bool system_heap_usage(uint64_t* used, uint64_t* budget);   // (vk_vma.cpp)
std::deque<std::map<uint64_t, Pool>::node_type> evicted_pools;
int64_t st_imports_evicted = 0;
uint64_t st_import_bytes_evicted = 0;
constexpr uint64_t kImportIdle = 600;

void imports_give_back() {   // (pools_mu held)
    while (!evicted_pools.empty() && evicted_pools.front().mapped().retired_value <= mem_completed) {
        Pool& p = evicted_pools.front().mapped();
        destroy_import_buffer(p.buffer);
        vkFreeMemory(dev.device, p.memory, nullptr);
        for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);   // (a pool imported as several dma-bufs)
        evicted_pools.pop_front();
    }
    // (hookshot) Off by default on Windows (device losses on Intel Arc and AMD right after imports went back, issues
    // 17, 309 and 571): RX_VK_IMPORT_HIGH=85 turns it on there. Retired imports have their own valve at the same
    // watermarks (retired_imports_give_back), on by default on Windows AMD and Intel whatever the budget: that one
    // frees pools nothing uses any more, this one pools still live that have been idle
    static const char* env_high = getenv("RX_VK_IMPORT_HIGH");
    static const char* env_low = getenv("RX_VK_IMPORT_LOW");
    static const int cap_mb = env_int("RX_VK_IMPORT_CAP_MB", -1);
#ifdef _WIN32
    static const int high = std::clamp(env_int("RX_VK_IMPORT_HIGH", 0), 0, 100);
#else
    static const int high = std::clamp(env_int("RX_VK_IMPORT_HIGH", 85), 0, 100);
#endif
    static const int low = std::clamp(env_int("RX_VK_IMPORT_LOW", 75), 10, 95);
    static uint64_t calls = 0;
    if (high <= 0 || ++calls % 16) return;   // (the driver asked every 16 submissions)
    uint64_t used = 0, budget = 0;
    if (!system_heap_usage(&used, &budget)) return;
    const uint64_t cap = retired_give_back::live_budget(budget, (env_high && *env_high) || (env_low && *env_low), cap_mb);
    static const bool logged_cfg = [&] {
        if (!cap)
            logf("vk: imports given back past %d%% of a %llu MB system heap budget: off (RX_VK_IMPORT_CAP_MB=0)", high,
                 (unsigned long long)(budget >> 20));
        else if (cap < budget)
            logf("vk: imports given back past %d%% of a %llu MB cap (%llu MB system heap budget): on", high,
                 (unsigned long long)(cap >> 20), (unsigned long long)(budget >> 20));
        else
            logf("vk: imports given back past %d%% of a %llu MB system heap budget: on", high, (unsigned long long)(budget >> 20));
        return true;
    }();
    (void)logged_cfg;
    // (with both valves on, the other ran just before, on this same submission (the two count alike): what it queued is
    // still in the driver's figure until the GPU is past it, and live pools would go for the same bytes. Only where
    // retired imports wait; elsewhere they go back at every submission and this valve reads what it always read)
    if (retired_mode() == retired_give_back::Mode::GRACE) used = retired_give_back::settled(used, dead_import_bytes);
    const uint64_t want = retired_give_back::want(used, cap, high, low);
    if (!want) return;
    std::vector<std::map<uint64_t, Pool>::iterator> idle;
    for (auto it = imported.begin(); it != imported.end(); ++it) {
        const Pool& p = it->second;
        if ((p.mstate == Pool::MIRROR_NONE || p.mstate == Pool::MIRROR_REFUSED) && p.last_value <= mem_completed &&
            mem_rec_value > p.last_value + kImportIdle)
            idle.push_back(it);
    }
    std::sort(idle.begin(), idle.end(), [](auto a, auto b) { return a->second.last_value < b->second.last_value; });
    uint64_t freed = 0;
    int n = 0;
    for (auto it : idle) {
        if (freed >= want) break;
        Pool& p = it->second;
        guest_pages_set(p.lo, p.hi, 0);
        p.retired_value = mem_rec_value;   // (this submission's recording is being made: freed once it is done)
        freed += p.hi - p.lo;
        n++;
        evicted_pools.push_back(imported.extract(it));
    }
    if (!n) {
        static int logged_none;
        if (logged_none++ < 8)
            logf("vk: system heap %llu of %llu MB: no idle import to give back (%zu imported)", (unsigned long long)(used >> 20),
                 (unsigned long long)(budget >> 20), imported.size());
        return;
    }
    pools_gen++;
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    if (mirror_on()) publish_cap_ranges();
    st_imports_evicted += n, st_import_bytes_evicted += freed;
    static int logged;
    if (logged++ < 40 || logged % 64 == 0)
        logf("vk: system heap %llu of %llu MB: %d idle imports given back (%llu MB); %lld (%llu MB) in all",
             (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20), n, (unsigned long long)(freed >> 20),
             (long long)st_imports_evicted, (unsigned long long)(st_import_bytes_evicted >> 20));
}

// ---- (hookshot) an import trimmed back to the game's live pools (mirror_trim.h) ----
// Every 64 submissions, while VRAM is short: the mirrored import that has been mostly outside every game pool the longest
// (mirror_trim::wait) is retired, and each of its live parts imported again on its own, with a mirror wanted at once: made in
// this same submission's uploads (memory_begin_submission, right after), each takes what the GPU wrote from the old mirror
// (mirror_make's inherit, as for a pool made again larger), and the page table points at the new mirrors before anything is
// recorded. One import a call: each new part is an import in the driver, on the replay thread. Nothing is done unless the new
// mirrors fit the budget beside the old one and the mirrors already wanted, which come first. The old one goes once every part
// has its own mirror and the GPU is done with it (trims_settle). RX_VK_MIRROR_TRIM=0 (or off, false, no): off
int64_t st_trims = 0;
uint64_t st_trim_bytes = 0;
int trims_waiting = 0;   // (retired by a trim, their mirror still wanted by a part without its own: trims_settle)
bool mirrors_trim() {   // (pools_mu held; true when one was trimmed: its parts are in mirror_wanted)
    static const bool on = [] {
        const char* v = getenv("RX_VK_MIRROR_TRIM");
        return !v || !env_value_off(v);   // (on unless set off: the switches' one rule, hookshot_env.h)
    }();
    const uint64_t chunk = import_chunk();
    if (!on || !chunk || mirror_epoch % 64 || par_active.load(std::memory_order_relaxed)) return false;
    uint64_t used = 0, allowed = 0;
    const bool pressed = vram_usage(&used, &allowed) && mirror_trim::pressed(used, allowed);
    Pool* pick = nullptr;
    std::vector<mirror_trim::Range> parts, pick_parts;
    for (auto& [lo, p] : imported) {
        const unsigned trims = mirror_trim::counted(p.trims, mirror_epoch - p.trim_epoch);
        if (!pressed || p.mstate != Pool::MIRROR_ON || !mirror_trim::wait(trims)) {
            p.dead_since = 0;
            continue;
        }
        parts = mirror_trim::live(p.lo, p.hi, chunk, mirror_trim::kJoin, guest_pools);
        if (!mirror_trim::worth(p.hi - p.lo, parts)) {
            p.dead_since = 0;
            continue;
        }
        if (!p.dead_since) p.dead_since = mirror_epoch;
        if (mirror_epoch - p.dead_since < mirror_trim::wait(trims)) continue;
        if (!pick || p.dead_since < pick->dead_since) pick = &p, pick_parts = parts;
    }
    if (!pick) return false;
    const uint64_t kept = mirror_trim::bytes(pick_parts), size = pick->hi - pick->lo;
    // (no room for both yet, beside the mirrors already wanted, which mirror_make takes first: looked at again in 64 submissions.
    // Nor while RX_VK_MIRROR_TIGHT's rule refuses every mirror: the parts would get none)
    uint64_t wanted = 0;
    for (const Pool* w : mirror_wanted) wanted += w->hi - w->lo;
    static const bool tight_rule = env_on("RX_VK_MIRROR_TIGHT");
    if (mirror_bytes + wanted + kept > mirror_budget() || (tight_rule && used > allowed / 100 * 90)) return false;
    std::vector<Pool> made;
    for (const mirror_trim::Range& r : pick_parts) {   // (all of them first: one that fails leaves everything as it was)
        Pool np;
        if (!import_pool(r.lo, r.hi, np)) {
            for (Pool& m : made) {
                destroy_import_buffer(m.buffer);
                vkFreeMemory(dev.device, m.memory, nullptr);
                for (VkDeviceMemory piece : m.pieces) vkFreeMemory(dev.device, piece, nullptr);
            }
            pick->dead_since = mirror_epoch;   // (not again for a while)
            return false;
        }
        made.push_back(std::move(np));
    }
    const uint64_t olo = pick->lo, ohi = pick->hi, waited = mirror_epoch - pick->dead_since;
    const bool gpu_only = pick->gpu_only, gpu_written = pick->gpu_written;
    const uint8_t trims = (uint8_t)(mirror_trim::counted(pick->trims, mirror_epoch - pick->trim_epoch) + 1);
    retire_pool(imported.find(olo));   // (the page table's entries of its range 0, its shadow freed, its mirror kept to inherit)
    retired_pools.back().mapped().trimmed = true;
    trims_waiting++;
    for (Pool& m : made) {
        m.gpu_only = gpu_only, m.gpu_written = gpu_written, m.trims = trims, m.trim_epoch = mirror_epoch;
        auto it = imported.emplace(m.lo, m).first;
        guest_pages_set(it->second.lo, it->second.hi, it->second.address);
        it->second.mstate = Pool::MIRROR_WANTED;
        mirror_wanted.push_back(&it->second);
    }
    st_trims++, st_trim_bytes += size - kept;
    static int logged;
    if (logged++ < 40 || st_trims % 64 == 0)
        logf("vk: pool %#llx..%#llx (%llu MB) trimmed to its %zu live part(s), %llu MB: the rest outside every game pool for %llu "
             "submissions (%lld trims, %llu MB in all; RX_VK_MIRROR_TRIM=0: off)", (unsigned long long)olo, (unsigned long long)ohi,
             (unsigned long long)(size >> 20), made.size(), (unsigned long long)(kept >> 20), (unsigned long long)waited,
             (long long)st_trims, (unsigned long long)(st_trim_bytes >> 20));
    return true;
}

// (pools_mu held, after the submission's mirrors are made) A trimmed import's old mirror waits to be inherited until every pool
// over its range has a mirror of its own. A part refused one (the budget, RX_VK_MIRROR_TIGHT, an allocation that failed) points
// at its import meanwhile; when its mirror is made later, mirror_make still copies in what the GPU wrote there from the old
// mirror, as for a pool made again larger. Let go at once, what the GPU wrote in that part and nowhere else was lost. A part
// refused for a failed allocation is tried again at its next binding while the old mirror waits. kIdleEpochs still bounds the
// wait (mirrors_give_back: inherit_lost)
void trims_settle() {
    if (!trims_waiting) return;
    for (auto& nh : retired_pools) {
        Pool& r = nh.mapped();
        if (!r.trimmed) continue;
        bool all = true;
        if (r.inherit_pending && r.mstate == Pool::MIRROR_ON) {
            auto it = imported.lower_bound(r.lo);
            if (it != imported.begin() && std::prev(it)->second.hi > r.lo) --it;
            for (; it != imported.end() && it->first < r.hi; ++it) {
                Pool& q = it->second;
                if (q.hi <= r.lo || q.mstate == Pool::MIRROR_ON) continue;
                all = false;
                if (q.mstate == Pool::MIRROR_REFUSED && q.hi - q.lo <= (1ull << 30)) q.mstate = Pool::MIRROR_NONE;
            }
        }
        if (!all) {
            static int logged;
            if (logged++ < 16)
                logf("vk: trimmed pool %#llx..%#llx: its mirror kept, a live part has none of its own yet", (unsigned long long)r.lo,
                     (unsigned long long)r.hi);
            continue;
        }
        r.trimmed = false, r.inherit_pending = false;   // (every part has its share, or the wait is over: it goes as any retired one)
        trims_waiting--;
    }
}

void memory_begin_submission() {
    std::lock_guard lk(pools_mu);
    reap_mirrors();
    retired_imports_give_back();
    mirror_epoch++;
    mirror_upload = true;
    mirror_synced.clear();
    writebacks.clear();
    imports_give_back();
    if (tex_capture_on()) publish_tex_spans(texture_frame_no());
    if (!mirror_on()) return;
    mirrors_trim();
    {
        uint64_t want = 0;
        for (Pool* p : mirror_wanted) want += p->hi - p->lo;
        if (mirror_epoch % 64 == 0 || mirror_bytes + want > mirror_budget()) mirrors_give_back(want);
    }
    if (mirror_wanted.empty()) {
        trims_settle();   // (a part's mirror made at its binding since)
        return;
    }
    for (Pool* p : mirror_wanted) mirror_make(*p);
    trims_settle();   // (the old mirror waits for no more only once every part has its own)
    mirror_wanted.clear();
    publish_cap_ranges();
    static uint64_t logged_at;
    if (mirror_bytes >> 28 != logged_at) {
        logged_at = mirror_bytes >> 28;
        logf("vk: %lld pools mirrored in VRAM (%llu MB)", (long long)st_mirrors, (unsigned long long)(mirror_bytes >> 20));
    }
}

/// (hookshot) Before a command buffer's parallel segments: every mirrored pool synced once, into the replay thread's
/// recording, ahead of all of them. Workers record out of order, so a pool a later segment bound first was synced in
/// that segment's upload, which runs after the earlier segments' work, and put the CPU's pages back over what the GPU
/// had just written in them (one frame of wrong lighting or exposure: 5x the flashes of serial replay). A segment
/// finds its pools synced now (synced_epoch, sync_seg 0). RX_VK_NO_PRESYNC=1: as before
void memory_sync_mirrors_before_segments() {
    static const bool off = env_on("RX_VK_NO_PRESYNC");
    if (off || !mirror_on() || !mirror_upload) return;
    std::lock_guard lk(pools_mu);
    for (auto& kv : imported) {
        Pool& p = kv.second;
        if (p.mstate != Pool::MIRROR_ON || p.synced_epoch == mirror_epoch) continue;
        mirror_sync(p);
        p.sync_seg = 0;
    }
}

/// A pool's mirror made, filled in the recording's upload command buffer: all of it, then the pages a capture holds
/// as they were at their submit (pools_mu held). False over the budget (tried again when next bound, as mirrors are
/// given back) or over 1 GB (never)
bool mirror_make(Pool& pool) {
    Pool* p = &pool;
    {
        const uint64_t n = p->hi - p->lo;
        // (hookshot: RX_VK_MIRROR_TIGHT=1, none while VRAM is 90% full. Off: 8 GB cards sit there all the time, and without their mirrors
        // the GPU read game memory over PCIe)
        static const bool tight_rule = env_on("RX_VK_MIRROR_TIGHT");
        uint64_t used = 0, allowed = 0;
        const bool tight = tight_rule && vram_usage(&used, &allowed) && allowed && used > allowed / 100 * 90;
        if (mirror_bytes + n > mirror_budget() || n > (1ull << 30) || tight) {
            p->mstate = n > (1ull << 30) ? Pool::MIRROR_REFUSED : Pool::MIRROR_NONE;
            static int logged;
            if (logged++ < 8)
                logf("vk: pool %#llx (%llu MB) stays in system memory: the mirrors have %llu MB of %llu", (unsigned long long)p->lo,
                     (unsigned long long)(n >> 20), (unsigned long long)(mirror_bytes >> 20),
                     (unsigned long long)(mirror_budget() >> 20));
            return false;
        }
        if (!create_device_buffer(n, kUsage, p->mirror, p->mirror_alloc)) {
            p->mstate = Pool::MIRROR_REFUSED;
            return false;
        }
        {   // (hookshot) the page table points at the mirror: what the GPU wrote is there, and guest memory has it only after the write-back
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = p->mirror;
            const uint64_t ma = vkGetBufferDeviceAddress(dev.device, &da);
            guest_pages_set(p->lo, p->hi, ma);
            if (env_on("RX_VK_LOG_ADDR"))
                logf("vk: addr mirror %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)p->lo, (unsigned long long)p->hi,
                     (unsigned long long)ma, (unsigned long long)(ma + n));
        }
        const VkBufferCopy all{0, 0, n};
        {
            GprofSpan gp(upload_command_buffer(), "mmake copy (a whole pool -> its new mirror)");
            gp.bytes = n;
            note_in_place_read();
            vkCmdCopyBuffer(upload_command_buffer(), p->buffer, p->mirror, 1, &all);   // (all of it, at this generation)
        }
        {   // (EvilOOT) then the mirrors of the pools it took in, over their part: what the GPU wrote there and only there
            // (retire_pool). The captures' pages go over it after, as before: the CPU's newer writes win
            bool barrier_done = false;
            int inherited = 0;
            for (auto& nh : retired_pools) {
                Pool& r = nh.mapped();
                if (!r.inherit_pending || r.mstate != Pool::MIRROR_ON || !r.mirror || r.hi <= p->lo || r.lo >= p->hi) continue;
                if (!barrier_done) {   // (after the full copy, and after the shaders of earlier submissions wrote the old mirror)
                    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
                    vkCmdPipelineBarrier2(upload_command_buffer(), &di);
                    barrier_done = true;
                }
                const uint64_t a = std::max(r.lo, p->lo), b = std::min(r.hi, p->hi);
                // (only the pages not stamped since its last sync: one the CPU wrote after it has the game's newer data in the
                // copy of guest memory above, and the old mirror's over it counted as synced, stale for good (mirror_inherit.h).
                // As mirror_sync, a page a shader wrote through a bare entry keeps the mirror's copy (RX_VK_PROTECT_BARE=1).
                // Synced in this submission: the whole part. Its synced_gen is raised only at the submission's end
                // (memory_end_submission), so what the GPU wrote in this submission's earlier segments (RX_VK_PARALLEL) is
                // stamped above it, and guest memory has that only after the write-back; the CPU's writes were collected
                // before the sync. RX_VK_INHERIT_ALL=1: the whole part always, as before)
                static const bool inherit_all = env_on("RX_VK_INHERIT_ALL");
                std::vector<VkBufferCopy> parts;
                uint64_t from_guest = 0;
                const uint32_t synced = r.synced_gen;
                if (inherit_all || r.synced_epoch == mirror_epoch)
                    parts.push_back({a - r.lo, a - p->lo, b - a});
                else
                    from_guest = mirror_inherit::runs(a, b, r.lo, p->lo, kPageShift, kRegionPages,
                                                      [&](uint64_t region) { return region_gen[region] <= synced; },
                                                      [&](uint64_t page) {
                                                          return page_gen(page) <= synced ||
                                                                 (protect_bare_on() && prot_page_since(page, synced));
                                                      },
                                                      [&](uint64_t src, uint64_t dst, uint64_t size) { parts.push_back({src, dst, size}); });
                if (!parts.empty())
                    vkCmdCopyBuffer(upload_command_buffer(), r.mirror, p->mirror, (uint32_t)parts.size(), parts.data());
                if (from_guest) {
                    static int told;
                    if (told++ < 40)
                        logf("vk: pool %#llx..%#llx: %llu KB the CPU wrote since retired pool %#llx's last sync kept from guest memory, "
                             "not its mirror", (unsigned long long)p->lo, (unsigned long long)p->hi, (unsigned long long)(from_guest >> 10),
                             (unsigned long long)r.lo);
                }
                if (!r.trimmed) r.inherit_pending = false;   // (a trimmed one: each of its live parts' pools takes its own, mirrors_trim)
                r.retired_value = std::max(r.retired_value, mem_rec_value);   // (in use by this upload until it is done)
                inherited++;
            }
            if (inherited) {
                static int logged;
                if (logged++ < 16)
                    logf("vk: pool %#llx..%#llx: mirror made with the GPU's contents of %d retired pool(s)", (unsigned long long)p->lo,
                         (unsigned long long)p->hi, inherited);
            }
        }
        {   // (then the pages a capture holds as they were at their submit: guest memory is the game's frame ahead)
            std::map<VkBuffer, std::vector<VkBufferCopy>> over;
            for (uint64_t page = p->lo >> kPageShift; page < (p->hi + 4095) >> kPageShift; page++) {
                const CapEntry e = cap_entry(page);
                if (!e.gen || e.gen != page_gen(page)) continue;
                auto b = cap_bufs.find(e.gen);
                if (b == cap_bufs.end()) continue;
                const uint64_t ga = std::max(page << kPageShift, p->lo), gb = std::min((page + 1) << kPageShift, p->hi);
                over[b->second.buffer].push_back({(uint64_t)e.slot * 4096 + (ga - (page << kPageShift)), ga - p->lo, gb - ga});
                b->second.last_use = std::max(b->second.last_use, mem_rec_value);
            }
            if (!over.empty()) {
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (after the full copy)
                mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(upload_command_buffer(), &di);
                for (const auto& [buf, rs] : over)
                    vkCmdCopyBuffer(upload_command_buffer(), buf, p->mirror, (uint32_t)rs.size(), rs.data());
            }
        }
        p->mstate = Pool::MIRROR_ON;
        pools_gen.fetch_add(1, std::memory_order_relaxed);   // (the binding's buffer changes: guest memory, then this mirror)
        p->synced_gen = gen_now;
        p->synced_epoch = mirror_epoch;
        p->sync_seg = texture_segment();
        mirror_synced.push_back(p);
        mirror_bytes += n;
        st_mirrors++;
        st_sync_bytes += (int64_t)n;
    }
    return true;
}

// ---- the write-back's diff (hookshot, 2026-11-09) ----
// A mirrored pool's write-back copied every written run whole from the mirror to guest memory: ~69 MB a frame in Hyrule Field
// (an i7-14700K and an RTX 5070 Ti), 4.4 ms of the GPU's ~13, at the end of the frame's last submission, which the pages of the
// next capture wait for (the capture's write-back wait above). The runs are the written bindings' whole ranges: ~82% of what
// they copied was zeros, and of the rest little changed from one frame to the next. Now a pool written back keeps a shadow in
// VRAM, what guest memory holds there as far as it is known, page by page. A compute pass (shaders/wb_diff.comp) copies the
// words of a run that differ from the shadow, into guest memory and the shadow both; a page the CPU wrote since (the captures'
// pages and the replay's own words, shadow_written) is read from guest memory into the shadow first. Guest memory comes out as
// with the whole copy, or, for words the GPU left as they were and the CPU wrote since, with the CPU's words, as on the console:
// the pass only ever writes fewer words than the copy did. The pass takes ~0.7 ms there, the GPU's frame went from ~13.1 to
// ~9.2 ms, and running there the game went from ~53.9 to ~57.9 fps with 4x frame interpolation (from ~55.2 to ~58.7 without).
// (Word by word it was far slower than the copies: the GPU's writes to system memory went at ~1 GB/s; a subgroup's whole span
// written at once goes at the copies' speed or better.)
// RX_VK_WB_DIFF=0: the whole copies, as before. RX_VK_WB_SHADOW_MB: the shadows' VRAM at most (default 1/8 of it)
// What it rests on: a shadow page it knows equals guest memory. So every CPU write of guest memory reaches shadow_written before
// the next write-back reads the list: the captures' pages (capture_make), every page the collection finds (memory_collect_written:
// all of the game's writes with RX_VK_CAPTURE=0), and the replay's own writes, through guest_cpu_write / guest_cpu_wrote: the
// in-stream clears of the counters the game's conditions read, the copies, clears and updates done on the CPU, the textures
// written back into their memory, WriteTexels, the report words (memory_cpu_owned). One the shadows never heard of left the
// CPU's value in guest memory where the whole copy wrote the GPU's, whenever the GPU's result equalled the shadow's old word:
// a counter cleared in the stream and counted to last frame's value again stayed cleared, and what it switched stayed off
// (PR 1109's review). The 600-submit line counts the pages each kind of report made the shadows read again.
// The GPU's own writes of guest memory past the mirror do the same: a written binding given the pool's guest buffer
// (gpu_buffer_bind outside a recording's uploads), and a whole copy in place of the pass, forget the shadow's pages (shadow_forget).
std::mutex shadow_written_mu;
std::vector<uint32_t> shadow_written_pages;   // (guest pages the CPU wrote: unknown to the shadows at the next write-back)
std::vector<uint32_t> shadow_written_collected_list;   // (the same, of the collection: memory_collect_written)
std::vector<uint32_t> shadow_written_by_replay;   // (the same, of the replay's own CPU writes: guest_cpu_wrote)
bool shadow_written_all = false;              // (more than those lists hold: every page unknown at the next write-back)
int64_t st_wbd_known_game = 0, st_wbd_known_collected = 0, st_wbd_known_replay = 0;   // (pages the shadows knew when reported)
struct DeadShadow {
    VkBuffer buffer;
    VmaAllocation alloc;
    uint64_t value;
};
std::deque<DeadShadow> dead_shadows;
uint64_t shadow_bytes = 0;
int64_t st_wbd_runs = 0, st_wbd_bytes = 0, st_wbd_fill_pages = 0;
bool wb_diff_on() {
    // (hookshot) On by default again (v2.0.11). v2.0.9 turned it off with the presents' own queue after the device lost on an RTX 4090
    // with NVIDIA's 621 driver (issue 1161); the reporter's A/B then put that on the present queue (black screen with it alone), and the
    // write-back diff alone ran there and on drivers 615 and 621 for others: ~20% fps (an RTX 3080 laptop went 40 -> 25 fps without
    // it). RX_VK_WB_DIFF=0: off
    // (integrated GPUs too: an Ally X's hard reboots stopped with v2.0.8 and came back with v2.0.9's off, issue 997: the whole copies
    // are long submissions there. Its shadows are the system's RAM on them, so at most 512 MB: shadow_room)
    static const bool on = !getenv("RX_VK_WB_DIFF") || atoi(getenv("RX_VK_WB_DIFF")) != 0;
    return on;
}
void shadow_free(Pool& p) {   // (pools_mu held: destroyed once the submissions so far are done)
    if (!p.shadow) return;
    dead_shadows.push_back({p.shadow, p.shadow_alloc, std::max(mem_rec_value, p.last_value)});
    shadow_bytes -= p.hi - p.lo;
    p.shadow = VK_NULL_HANDLE, p.shadow_alloc = nullptr, p.shadow_address = 0;
    p.shadow_known.clear();
}
void shadows_reap() {
    while (!dead_shadows.empty() && dead_shadows.front().value <= mem_completed) {
        destroy_buffer(dead_shadows.front().buffer, dead_shadows.front().alloc);
        dead_shadows.pop_front();
    }
}
// (hookshot) The shadows' two rules read what is really allocated (the driver's count less the free space inside VMA's blocks,
// as the texture budget's 90% rule does: vram_pressure.h), not the count alone. By the count alone they went at 95% and
// came back only under 85%. On a 12 GB card the texture cache fills VRAM to 93-98% of what the driver allows within minutes
// of play, gigabytes of it free space inside the blocks that evicting cannot give back, so the shadows went (272-448 MB) and
// never returned: the write-backs were whole copies again for the rest of the session. In a player's log (RTX 4070 SUPER,
// 2560 x 1440, 10,462 of 10,732 MB used, 3.5 GB of it free inside the blocks) "the shadows given back" is where 58 fps
// became 26. Here, on v2.0.11's code with 9.8 GB allowed, five minutes across Hyrule Field, two runs each: 56.6 and 58.8 fps
// with 32 and 8 of 120 two-second windows under 55 fps before, 59.8 and 59.6 fps with 1 and 2 after; the GPU's frame
// 11.7-12.0 -> 9.8-10.1 ms, the capture's wait in a frame 2.1-2.4 -> 1.1 ms. (The free space is of no use to the shadows
// themselves, which are allocations of their own: they are made only under the textures' lines. vram_pressure.h)
// RX_VK_SHADOW_RAW=1: by the count alone, as before
bool shadow_rules_raw() {
    static const bool v = env_on("RX_VK_SHADOW_RAW");
    return v;
}
bool shadow_room(uint64_t n) {   // (the shadows' budget, and room in VRAM: the textures and the mirrors come first)
    static const uint64_t cap = [] {
        if (const char* e = getenv("RX_VK_WB_SHADOW_MB")) return (uint64_t)atoll(e) << 20;
        uint64_t used = 0, allowed = 0;
        const uint64_t eighth = vram_usage(&used, &allowed) && allowed ? allowed / 8 : (uint64_t)512 << 20;   // (unsigned long on Linux)
        // (an integrated GPU's VRAM is the system's RAM: 512 MB at most there)
        return dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? std::min<uint64_t>(eighth, (uint64_t)512 << 20) : eighth;
    }();
    if (shadow_bytes + n > cap) return false;
    uint64_t used = 0, allowed = 0, block_free = 0;
    if (!vram_usage(&used, &allowed, &block_free) || !allowed) return true;
    if (shadow_rules_raw()) return used + n <= allowed / 100 * 85;
    return vram_pressure::shadow_fits(n, used, allowed, block_free);
}
bool shadow_make(Pool& p) {
    const uint64_t n = p.hi - p.lo;
    if (!p.mirror || !shadow_room(n)) return false;
    if (!create_device_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, p.shadow, p.shadow_alloc))
        return false;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.shadow;
    p.shadow_address = vkGetBufferDeviceAddress(dev.device, &da);
    p.shadow_known.assign((size_t)((n >> kPageShift) + 63) / 64, 0);
    shadow_bytes += n;
    static int logged;
    if (logged++ < 16)
        logf("vk: write-back diff: a shadow for pool %#llx (%llu MB; %llu MB of shadows)", (unsigned long long)p.lo,
             (unsigned long long)(n >> 20), (unsigned long long)(shadow_bytes >> 20));
    return true;
}
// (pools_mu held) every 120 write-back submissions: every shadow given back while VRAM is really 95% full, or the driver's count
// at 99%, and the shadows of the pools that had one made again once there is room. A pool's own write-backs may each stay under
// the 256 KB a shadow is first made at, for good after a scene change, so waiting for one never gave it its shadow back: in a
// player's log (issue 2238) 48 of 352 MB stayed whole copies for the rest of the session while the others were made again.
void shadows_check_room() {
    static int64_t n;
    if (++n % 120) return;
    bool wanted = false;
    for (const auto& [lo, p] : imported) wanted = wanted || (p.shadow_wanted && !p.shadow && p.mirror);
    if (!shadow_bytes && !wanted) return;
    uint64_t used = 0, allowed = 0, block_free = 0;
    if (!vram_usage(&used, &allowed, &block_free) || !allowed) return;
    if (!(shadow_rules_raw() ? used > allowed / 100 * 95 : vram_pressure::shadows_go(used, allowed, block_free))) {
        int made = 0, tries = 0;
        uint64_t made_bytes = 0;   // (what this check made, not what all the shadows are)
        for (auto& [lo, p] : imported) {
            if (!p.mirror || p.shadow || !p.shadow_wanted) continue;   // (mirror_free'd: cannot be made again, not a try)
            const uint64_t n = p.hi - p.lo;
            if (!shadow_room(n)) continue;   // (too big to fit now, or past the budget: no attempt, a later check has room)
            if (tries++ == 8) break;         // (hard cap on attempts per check: each is an allocation of the pool's whole size)
            if (shadow_make(p)) made++, made_bytes += n;
        }
        static int logged;
        if (made && logged++ < 8)
            logf("vk: write-back diff: %d shadows made again (%llu MB)", made, (unsigned long long)(made_bytes >> 20));
        return;
    }
    if (!shadow_bytes) return;
    const uint64_t had = shadow_bytes;
    for (auto& [lo, p] : imported) shadow_free(p);
    for (auto& nh : retired_pools) shadow_free(nh.mapped());
    static int logged;
    if (logged++ < 8)
        logf("vk: write-back diff: VRAM %llu of %llu MB: the shadows given back (%llu MB)", (unsigned long long)(used >> 20),
             (unsigned long long)(allowed >> 20), (unsigned long long)(had >> 20));
}
// (pools_mu held) a range of the pool (offsets) unknown to its shadow again
void shadow_forget(Pool& p, uint64_t at, uint64_t len) {
    if (!p.shadow || !len) return;
    for (uint64_t pg = at >> kPageShift; pg <= (at + len - 1) >> kPageShift; pg++) p.shadow_known[pg >> 6] &= ~(1ull << (pg & 63));
}
// (pools_mu held) the pages written since: unknown to their pools' shadows
void shadows_forget_written() {
    static thread_local std::vector<uint32_t> game, collected, replay;
    bool all;
    {
        std::lock_guard lk(shadow_written_mu);
        game.clear(), collected.clear(), replay.clear();
        game.swap(shadow_written_pages);
        collected.swap(shadow_written_collected_list);
        replay.swap(shadow_written_by_replay);
        all = shadow_written_all, shadow_written_all = false;
    }
    if (all)   // (written for a long while with no write-back to read the list: nothing of guest memory is known)
        for (auto& [lo, pool] : imported)
            if (pool.shadow) std::fill(pool.shadow_known.begin(), pool.shadow_known.end(), 0ull);
    if (!shadow_bytes) return;
    // (in this order, each page counted under the first that reports it: the collection's and the replay's counts are what the
    // captures alone would have left known)
    for (int which = 0; which < 3; which++) {
        std::vector<uint32_t>& pages = which == 0 ? game : which == 1 ? collected : replay;
        int64_t& known = which == 0 ? st_wbd_known_game : which == 1 ? st_wbd_known_collected : st_wbd_known_replay;
        if (pages.empty()) continue;
        std::sort(pages.begin(), pages.end());
        Pool* p = nullptr;
        for (uint32_t pg : pages) {
            const uint64_t a = (uint64_t)pg << kPageShift;
            if (!p || a < p->lo || a >= p->hi) {
                p = nullptr;
                auto it = imported.upper_bound(a);
                if (it == imported.begin()) continue;
                --it;
                if (a < it->second.hi) p = &it->second;
                if (!p) continue;
            }
            if (!p->shadow) continue;
            const uint64_t i = (a - p->lo) >> kPageShift;
            uint64_t& bits = p->shadow_known[i >> 6];
            if (bits & (1ull << (i & 63))) known++;
            bits &= ~(1ull << (i & 63));
        }
    }
}
// (the pass's pipeline: 24 bytes of push constants and no descriptors, its buffers by address; made once)
struct WbdPipe {
    bool tried = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
} wbd_pipe;
bool wbd_pipe_ready() {
    if (wbd_pipe.tried) return wbd_pipe.pipe != VK_NULL_HANDLE;
    wbd_pipe.tried = true;
    // (the pass votes across a subgroup (shaders/wb_diff.comp): a GPU without votes in compute keeps the whole copies)
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    if (!(sg.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) || !(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
        logf("vk: write-back diff off: the GPU has no subgroup votes in compute shaders");
        return false;
    }
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev.device, &li, nullptr, &wbd_pipe.layout) == VK_SUCCESS) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof WB_DIFF_COMP_SPV;
        mi.pCode = WB_DIFF_COMP_SPV;
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) == VK_SUCCESS) {
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = m;
            ci.stage.pName = "main";
            ci.layout = wbd_pipe.layout;
            if (vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &wbd_pipe.pipe) != VK_SUCCESS)
                wbd_pipe.pipe = VK_NULL_HANDLE;
            vkDestroyShaderModule(dev.device, m, nullptr);
        }
    }
    const bool ok = wbd_pipe.pipe != VK_NULL_HANDLE;
    logf("vk: write-back diff %s", ok ? "on (RX_VK_WB_DIFF=0: off)" : "off: its pipeline was not made");
    return ok;
}
void shadow_written_to(std::vector<uint32_t>& list, const uint32_t* pages, int64_t n) {
    if (n <= 0 || !wb_diff_on()) return;
    std::lock_guard lk(shadow_written_mu);
    if (shadow_written_pages.size() + shadow_written_collected_list.size() + shadow_written_by_replay.size() > (8u << 20)) {
        shadow_written_all = true;   // (no write-back has read them for a long while: all of them, then)
        shadow_written_pages.clear(), shadow_written_collected_list.clear(), shadow_written_by_replay.clear();
        return;
    }
    list.insert(list.end(), pages, pages + n);
}
void shadow_written(const uint32_t* pages, int64_t n) {
    shadow_written_to(shadow_written_pages, pages, n);
}
void shadow_written_collected(const uint32_t* pages, int64_t n) {
    shadow_written_to(shadow_written_collected_list, pages, n);
}
int guest_cpu_write(uint64_t gpu, const void* src, uint64_t n) {
    const int ok = host->gpu_write(gpu, src, n);
    guest_cpu_wrote(gpu, n);
    return ok;
}
void guest_cpu_wrote(uint64_t gpu, uint64_t n) {
    if (!n || !wb_diff_on()) return;
    uint32_t pages[64];
    int k = 0;
    for (uint64_t a = gpu & ~4095ull; a < gpu + n; a += 4096) {   // (GPU pages are mapped to guest memory page by page)
        const uint64_t g = host->gpu_guest(a);
        if (!g) continue;
        pages[k++] = (uint32_t)(g >> kPageShift);
        if (k == 64) shadow_written_to(shadow_written_by_replay, pages, k), k = 0;
    }
    if (k) shadow_written_to(shadow_written_by_replay, pages, k);
}
// A run of a pool (offsets, in whole words) as the pass's chunks, in order: the words before its first 16-byte vector, the
// vectors 64 KB at most at a time, the words after the last; emit(offset, bytes, vectors). (tests/test_wb_diff_split.py)
template <class F> void wbd_split(uint64_t at, uint64_t size, F&& emit) {
    const uint64_t end = at + size;
    const uint64_t body = std::min<uint64_t>(end, (at + 15) & ~(uint64_t)15), body_end = std::max<uint64_t>(body, end & ~(uint64_t)15);
    if (body > at) emit(at, body - at, false);
    for (uint64_t a = body; a < body_end;) {
        const uint64_t len = std::min<uint64_t>(body_end - a, 64u << 10);
        emit(a, len, true);
        a += len;
    }
    if (end > body_end) emit(body_end, end - body_end, false);
}
struct WbdChunk {   // (shaders/wb_diff.comp's Chunk)
    uint64_t mirror, shadow, guest;
    uint32_t count, vecs;   // (16-byte vectors when vecs is 1, else words)
};
bool wbd_observe_on() {
    static const bool on = env_on("RX_VK_WB_OBSERVE");
    return on;
}
constexpr uint64_t kWbdObserveChunks = 256u << 10; // At most 1 MiB of optional flags per table.
struct WbdObserved {
    uint64_t batches = 0, chunks = 0, changed = 0, bytes = 0, changed_bytes = 0, missing = 0, last_value = 0;
} wbd_observed;
void wbd_observe_missed(uint64_t chunks, const char* reason) {
    wbd_observed.missing += chunks;
    static unsigned said;
    if (said++ < 8) logf("vk: write-back observations unavailable: %s; ordinary write-back kept", reason);
}
// (the pass's tables, mapped, a ring: one is taken again once the submission that read it is done)
struct WbdTable {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    uint64_t bytes = 0, value = 0;
    uint64_t observed_offset = 0;
    uint32_t observed_count = 0;
};
WbdTable wbd_tables[64];
int wbd_table_next = 0;
void wbd_observe_collect(WbdTable& t) {
    if (!t.observed_count || t.value > mem_completed) return;
    const uint64_t count = t.observed_count, offset = t.observed_offset;
    t.observed_count = 0; // Consume once, including a failed read. Never turn missing data into clean chunks.
    if (!t.value || !t.map || count > kWbdObserveChunks || offset != count * sizeof(WbdChunk) ||
        offset > t.bytes || count * sizeof(uint32_t) > t.bytes - offset ||
        !invalidate_mapped_checked(t.alloc, offset + count * sizeof(uint32_t))) {
        wbd_observe_missed(count, "result bounds or host visibility");
        return;
    }
    const auto* chunks = reinterpret_cast<const WbdChunk*>(t.map);
    const auto* flags = reinterpret_cast<const uint32_t*>(t.map + offset);
    uint64_t changed = 0, bytes = 0, changed_bytes = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t size = (uint64_t)chunks[i].count * (chunks[i].vecs ? 16u : 4u);
        if (flags[i] > 1 || chunks[i].vecs > 1 || !size || size > (64u << 10)) {
            wbd_observe_missed(count, "incomplete or invalid GPU result");
            return;
        }
        bytes += size;
        if (flags[i]) ++changed, changed_bytes += size;
    }
    ++wbd_observed.batches;
    wbd_observed.chunks += count;
    wbd_observed.changed += changed;
    wbd_observed.bytes += bytes;
    wbd_observed.changed_bytes += changed_bytes; // Bytes covered by changed chunks, not exact changed-byte counts.
    wbd_observed.last_value = std::max(wbd_observed.last_value, t.value);
}
bool wbd_table_make(uint64_t bytes, WbdTable& t) {
    if (!bytes || bytes > UINT64_MAX - 65535) return false;
    const uint64_t n = std::max<uint64_t>(128u << 10, (bytes + 65535) & ~65535ull);
    void* map = nullptr;
    if (create_buffer(n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      true, t.buf, t.alloc, &map) && map) {
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = t.buf;
        t.address = vkGetBufferDeviceAddress(dev.device, &da);
        if (t.address) {
            t.map = (uint8_t*)map;
            t.bytes = n;
            return true;
        }
    }
    if (t.buf) destroy_buffer(t.buf, t.alloc);
    t = {};
    return false;
}
WbdTable* wbd_table_get(uint64_t bytes) {
    WbdTable& t = wbd_tables[wbd_table_next];
    if (t.value > mem_completed) return nullptr;   // (still read by a submission the GPU has not done)
    wbd_observe_collect(t); // Before its input table or its trailing flags can be overwritten or freed.
    const uint64_t count = bytes / sizeof(WbdChunk);
    const uint64_t extra = wbd_observe_on() && mem_rec_value && bytes % sizeof(WbdChunk) == 0 &&
                           count <= kWbdObserveChunks ? count * sizeof(uint32_t) : 0;
    const uint64_t wanted = bytes <= UINT64_MAX - extra ? bytes + extra : bytes;
    if (t.bytes < wanted) {
        WbdTable next;
        if (wbd_table_make(wanted, next)) {
            if (t.buf) destroy_buffer(t.buf, t.alloc);
            t = next;
        } else if (t.bytes < bytes) {
            // Optional growth failed. Keep an existing input table when it fits; otherwise retry the ordinary
            // size after releasing the retired old buffer, as the old growth path did.
            const bool retry = t.buf || wanted != bytes;
            if (t.buf) destroy_buffer(t.buf, t.alloc);
            t = {};
            if (!retry || !wbd_table_make(bytes, t)) return nullptr;
        }
    }
    wbd_table_next = (wbd_table_next + 1) % 64;
    t.value = mem_rec_value;
    return &t;
}
VkDeviceAddress wbd_observe_begin(WbdTable& t, uint64_t count) {
    if (!wbd_observe_on()) return 0;
    const uint64_t offset = count * sizeof(WbdChunk);
    if (!count || count > kWbdObserveChunks || !t.value || !t.map || !t.address ||
        offset > t.bytes || count * sizeof(uint32_t) > t.bytes - offset || t.address > UINT64_MAX - offset) {
        wbd_observe_missed(count, "result capacity");
        return 0;
    }
    // Sentinel detects missing stores too. Input and results share this slot's existing allocation and lifetime.
    memset(t.map + offset, 0xff, (size_t)count * sizeof(uint32_t));
    if (!flush_mapped_checked(t.alloc, offset + count * sizeof(uint32_t))) {
        wbd_observe_missed(count, "host flush");
        return 0;
    }
    t.observed_offset = offset;
    t.observed_count = (uint32_t)count;
    return t.address + offset;
}

void memory_end_submission(VkCommandBuffer cmd, uint64_t part) {
    std::lock_guard lk(pools_mu);
    {   // (every 600 submissions: how the mirror syncs' dirty pages were read)
        static int64_t n, l_live, l_cap;
        if (++n % 600 == 0) {
            wb_learn_report();
            if (wb_skips_log()) wb_skips_report();
            static int64_t l_old;
            logf("vk: mirror syncs (600 submits): %lld dirty pages from their submits' captures, %lld from earlier ones', "
                 "%lld from guest memory as it was at the replay; %zu captures kept; %lld pages the GPU wrote copied over "
                 "(%lld live); %lld kept for the GPU (RX_VK_PROTECT_BARE)", (long long)(st_sync_cap_pages - l_cap),
                 (long long)(st_sync_old_pages - l_old), (long long)(st_sync_live_pages - l_live), cap_bufs.size(),
                 (long long)st_sync_gpu_pages, (long long)st_sync_gpu_live, (long long)st_prot_kept.load());
            l_cap = st_sync_cap_pages, l_live = st_sync_live_pages, l_old = st_sync_old_pages;
            if (shadow_bytes || st_wbd_runs)
                logf("vk: write-back diff (600 submits): %lld runs, %.1f MB compared; %lld pages read into the shadows; %llu MB of shadows; "
                     "pages the shadows knew that the CPU wrote: %lld in captures, %lld only in the collection, %lld only by the replay itself",
                     (long long)st_wbd_runs, st_wbd_bytes / 1048576.0, (long long)st_wbd_fill_pages, (unsigned long long)(shadow_bytes >> 20),
                     (long long)st_wbd_known_game, (long long)st_wbd_known_collected, (long long)st_wbd_known_replay);
            st_wbd_runs = st_wbd_bytes = st_wbd_fill_pages = 0;
            st_wbd_known_game = st_wbd_known_collected = st_wbd_known_replay = 0;
            if (wbd_observe_on()) {
                logf("vk: write-back observations (completed since last report): %llu batches, %llu chunks, %llu changed; "
                     "%.1f MB compared, %.1f MB covered by changed chunks; %llu unavailable chunks; last completed value %llu",
                     (unsigned long long)wbd_observed.batches, (unsigned long long)wbd_observed.chunks,
                     (unsigned long long)wbd_observed.changed, wbd_observed.bytes / 1048576.0,
                     wbd_observed.changed_bytes / 1048576.0, (unsigned long long)wbd_observed.missing,
                     (unsigned long long)wbd_observed.last_value);
                wbd_observed = {};
            }
            std::vector<std::pair<int64_t, uint64_t>> top;
            for (const auto& [lo, k] : live_by_pool) top.push_back({k, lo});
            std::sort(top.rbegin(), top.rend());
            std::string s;
            for (size_t i = 0; i < top.size() && i < 8; i++) {
                const auto it = imported.find(top[i].second);
                char b[160];
                bool captured = false;
                {
                    std::lock_guard ck(cap_ranges_mu);
                    for (const auto& r : cap_ranges) captured |= top[i].second >= r.first && top[i].second < r.second;
                }
                snprintf(b, sizeof b, " %#llx (%llu MB%s, state %d): %lld;", (unsigned long long)top[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         captured ? ", captured" : ", NOT captured", it != imported.end() ? (int)it->second.mstate : -1,
                         (long long)top[i].first);
                s += b;
            }
            logf("vk:   read live, by pool:%s", s.c_str());
            live_by_pool.clear();
            // (and the bindings the GPU read in guest memory itself: pools with no mirror yet, or none (state 9: bound
            // outside a recording))
            std::vector<std::pair<int64_t, uint64_t>> dtop;
            for (const auto& [lo, e] : direct_by_pool) dtop.push_back({e.first, lo});
            std::sort(dtop.rbegin(), dtop.rend());
            std::string ds;
            for (size_t i = 0; i < dtop.size() && i < 8; i++) {
                const auto it = imported.find(dtop[i].second);
                char b[160];
                snprintf(b, sizeof b, " %#llx (%llu MB, state %d): %lld binds, %.0f MB;", (unsigned long long)dtop[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         direct_by_pool[dtop[i].second].second, (long long)dtop[i].first,
                         direct_bytes_by_pool[dtop[i].second] / 1048576.0);
                ds += b;
            }
            if (!dtop.empty()) {
                double all = 0;
                for (const auto& [lo, bytes] : direct_bytes_by_pool) all += (double)bytes;
                logf("vk:   bound in guest memory, by pool (the GPU reads these over PCIe; binds and the bytes of their ranges over these 600 submits, "
                     "%.1f MB in all, a range counted once a submit): %s", all / 1048576.0, ds.c_str());
            }
            direct_by_pool.clear(), direct_bytes_by_pool.clear();
        }
    }
    static const bool no_writeback = env_on("RX_VK_NO_WRITEBACK");   // (an experiment: none)
    if (no_writeback) writebacks.clear();
    if (!writebacks.empty()) {
        // per pool, the ranges merged, one copy each (mirror -> the guest's pool)
        std::sort(writebacks.begin(), writebacks.end(), [](const WriteBack& a, const WriteBack& b) {
            return a.pool != b.pool ? a.pool < b.pool : a.lo < b.lo;
        });
        GprofSpan gp(cmd, "wb copies (mirror -> guest memory, with its two barriers)");   // (RX_VK_PASS_TIMING)
        gp.items = 0;
        transfer_barrier(cmd);
        std::vector<VkBufferCopy> runs;
        // (the write-back's diff: the runs of pools with a shadow, as chunks of 64 KB at most, for one pass after the loop)
        const bool diff = wb_diff_on() && wbd_pipe_ready();
        if (diff) shadows_forget_written(), shadows_reap(), shadows_check_room();
        static thread_local std::vector<WbdChunk> chunks;
        struct ChunkSrc { Pool* p; uint64_t at, len; };
        static thread_local std::vector<ChunkSrc> chunk_src;   // (for the whole copy, when no table is free)
        static thread_local std::vector<VkBufferCopy> fills, odd;
        chunks.clear(), chunk_src.clear();
        bool filled = false;
        for (size_t i = 0; i < writebacks.size();) {
            Pool* p = writebacks[i].pool;
            runs.clear();
            for (; i < writebacks.size() && writebacks[i].pool == p; i++) {
                const WriteBack& w = writebacks[i];
                if (!runs.empty() && w.lo <= runs.back().srcOffset + runs.back().size)
                    runs.back().size = std::max(runs.back().size, w.hi - runs.back().srcOffset);
                else
                    runs.push_back({w.lo, w.lo, w.hi - w.lo});
            }
            cut_cpu_words(*p, runs);
            if (runs.empty()) continue;
            checkpoint_mark(cmd, p->lo, 'b');
            uint64_t run_bytes = 0;
            for (const VkBufferCopy& r : runs) run_bytes += r.size;
            if (diff && !p->shadow && run_bytes >= (256u << 10)) {
                // (a pool whose write-back is worth one, as VRAM allows; wanted stays after VRAM takes it, for shadows_check_room)
                p->shadow_wanted = true;
                shadow_make(*p);
            }
            if (diff && p->shadow && p->address && p->mirror) {
                VkBufferDeviceAddressInfo mda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                mda.buffer = p->mirror;
                const VkDeviceAddress mirror_address = vkGetBufferDeviceAddress(dev.device, &mda);
                // (a run not in whole words, as a copy of the game's may be: copied whole, its pages then unknown to the shadow)
                odd.clear();
                for (const VkBufferCopy& r : runs)
                    if ((r.dstOffset | r.size) & 3) odd.push_back(r);
                // the pages of the runs the shadow does not know: from guest memory, whole, first
                fills.clear();
                const uint64_t pool_n = p->hi - p->lo;
                for (const VkBufferCopy& r : runs)
                    for (uint64_t pg = r.dstOffset >> kPageShift; !((r.dstOffset | r.size) & 3) && pg <= (r.dstOffset + r.size - 1) >> kPageShift; pg++) {
                        uint64_t& bits = p->shadow_known[pg >> 6];
                        if (bits & (1ull << (pg & 63))) continue;
                        bits |= 1ull << (pg & 63);
                        const uint64_t at = pg << kPageShift, len = std::min<uint64_t>(4096, pool_n - at);
                        if (!fills.empty() && fills.back().srcOffset + fills.back().size == at)
                            fills.back().size += len;
                        else
                            fills.push_back({at, at, len});
                        st_wbd_fill_pages++;
                    }
                if (!fills.empty()) {
                    vkCmdCopyBuffer(cmd, p->buffer, p->shadow, (uint32_t)fills.size(), fills.data());
                    filled = true;
                }
                if (!odd.empty()) {
                    vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)odd.size(), odd.data());
                    for (const VkBufferCopy& r : odd) shadow_forget(*p, r.dstOffset, r.size);
                }
                for (const VkBufferCopy& r : runs) {
                    if ((r.dstOffset | r.size) & 3) continue;
                    wbd_split(r.dstOffset, r.size, [&](uint64_t at, uint64_t len, bool vecs) {
                        chunks.push_back({mirror_address + at, p->shadow_address + at, p->address + at,
                                          (uint32_t)(len / (vecs ? 16 : 4)), vecs ? 1u : 0u});
                        chunk_src.push_back({p, at, len});
                    });
                }
                st_wbd_runs += (int64_t)runs.size(), st_wbd_bytes += (int64_t)run_bytes;
            } else {
                vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)runs.size(), runs.data());
            }
            for (const VkBufferCopy& r : runs) st_wb_bytes += (int64_t)r.size, gp.bytes += r.size;
            gp.items += (int)runs.size();
            for (const VkBufferCopy& r : runs) wb_pages_note(p->lo + r.dstOffset, p->lo + r.dstOffset + r.size, mem_rec_value, part);   // (EvilBoyce)
            {   // (hookshot) RX_VK_LOG_WRITEBACK=1: the pools written back the most, every 600 write-back submissions
                static const bool log_wb = env_on("RX_VK_LOG_WRITEBACK");
                if (log_wb) {
                    static std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> by_pool;   // lo -> bytes, runs
                    static int64_t subs;
                    auto& e = by_pool[p->lo];
                    for (const VkBufferCopy& r : runs) e.first += r.size;
                    e.second += runs.size();
                    if (i == writebacks.size() && ++subs % 600 == 0) {
                        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> v(by_pool.begin(), by_pool.end());
                        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                        std::string s;
                        for (size_t k = 0; k < v.size() && k < 8; k++) {
                            char b[120];
                            const auto it = imported.find(v[k].first);
                            snprintf(b, sizeof b, " %#llx (%llu MB pool): %.1f MB in %.0f runs;", (unsigned long long)v[k].first,
                                     it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                                     v[k].second.first / 600.0 / 1048576.0, v[k].second.second / 600.0);
                            s += b;
                        }
                        logf("vk: write-back a submission, by pool:%s", s.c_str());
                        by_pool.clear();
                    }
                }
            }
        }
        if (!chunks.empty()) {
            WbdTable* t = wbd_table_get(chunks.size() * sizeof(WbdChunk));
            if (!t) {   // (no table free: the whole copies, the shadows forgotten for those pages)
                if (wbd_observe_on()) wbd_observe_missed(chunks.size(), "chunk table busy or unavailable");
                for (const ChunkSrc& c : chunk_src) {
                    const VkBufferCopy r{c.at, c.at, c.len};
                    vkCmdCopyBuffer(cmd, c.p->mirror, c.p->buffer, 1, &r);
                    shadow_forget(*c.p, c.at, c.len);
                }
                static int logged;
                if (logged++ < 8) logf("vk: write-back diff: no chunk table free (%zu chunks): this one copied whole", chunks.size());
            }
            if (filled) transfer_barrier(cmd);   // (the shadows' pages from guest memory before the pass reads them)
            if (t) {
                memcpy(t->map, chunks.data(), chunks.size() * sizeof(WbdChunk));
                const VkDeviceAddress observations = wbd_observe_begin(*t, chunks.size());
                if (!observations) flush_mapped(t->alloc, chunks.size() * sizeof(WbdChunk));
                GprofSpan gd(cmd, "wb diff (mirror -> guest memory where it differs from the shadow)");
                gd.items = (int)chunks.size();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wbd_pipe.pipe);
                const struct { uint64_t table; uint32_t count, pad; uint64_t observations; }
                    pc{t->address, (uint32_t)chunks.size(), 0, observations};
                vkCmdPushConstants(cmd, wbd_pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                const uint32_t n = (uint32_t)chunks.size();
                vkCmdDispatch(cmd, std::min<uint32_t>(n, 65535u), (n + 65534u) / 65535u, 1);
                if (observations) {
                    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dependency.memoryBarrierCount = 1;
                    dependency.pMemoryBarriers = &barrier;
                    vkCmdPipelineBarrier2(cmd, &dependency);
                }
            }
        }
        transfer_barrier(cmd);
        writebacks.clear();
    }
    // (what this submission synced is current as of now: the GPU's own writes, stamped while it was recorded, are
    // in the mirror and written back; CPU writes collected at the next submission get a newer generation)
    for (Pool* p : mirror_synced) p->synced_gen = gen_now;
    mirror_synced.clear();
    mirror_upload = false;
}

void memory_stats(int64_t* mirrors, uint64_t* mirror_mb, int64_t* sync_bytes, int64_t* sync_copies, int64_t* wb_bytes) {
    *mirrors = st_mirrors, *mirror_mb = mirror_bytes >> 20, *sync_bytes = st_sync_bytes, *sync_copies = st_sync_copies;
    *wb_bytes = st_wb_bytes;
}
} // namespace mkwvk
