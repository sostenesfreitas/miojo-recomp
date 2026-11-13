#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace fixture {
std::atomic<int> fail_after{-1};
template<class T> struct FaultAllocator : std::allocator<T> {
    using value_type = T;
    FaultAllocator() = default;
    template<class U> FaultAllocator(const FaultAllocator<U>&) {}
    template<class U> struct rebind { using other = FaultAllocator<U>; };
    T* allocate(size_t n) {
        const int left = fail_after.load();
        if (left == 0) { fail_after = -1; throw std::bad_alloc(); }
        if (left > 0) fail_after--;
        return std::allocator<T>{}.allocate(n);
    }
};
template<class K, class V> using FaultMap = std::map<K, V, std::less<K>, FaultAllocator<std::pair<const K, V>>>;
template<class T> T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
struct nvn_tex { uint64_t gpu = 0x1000; struct { uint32_t width = 16; } d; };
struct nvn_view { int format = 42; };
struct BufferRef {
    VkBuffer buffer{}; uint64_t offset = 0, size = 64; VkDeviceAddress address = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};
struct { VkDevice device{}; VkPhysicalDeviceProperties props{}; } dev;
uint64_t frame_no = 1, mem_rec_value = 1, mem_completed = 0, mapping = 0x1000;
bool mapped = true;
std::atomic<unsigned> lookups{0};
unsigned creates = 0, destroyed_views = 0, destroyed_buffers = 0, writes = 0;
uint64_t written_gpu = 0, written_size = 0;
VkResult create_result = VK_SUCCESS;
bool fail_view_commit = false;
std::map<VkBufferView, VkBufferViewCreateInfo> live;
std::mutex driver_mu;
std::vector<char> destruction_order;
struct Pool {
    uint64_t lo = 0, hi = 0, last_value = 0, retired_value = 0;
    VkBuffer buffer{}; VkDeviceMemory memory{}; std::vector<VkDeviceMemory> pieces;
    VkDeviceAddress address = 0;
};
Pool first_pool{0x1000, 0x2000, 0, 0, handle<VkBuffer>(1), {}, {}, 0x100000};
Pool second_pool{0x2000, 0x3000, 0, 0, handle<VkBuffer>(2), {}, {}, 0x200000};
std::mutex pools_mu;
void note_in_place_read(bool = false) {}   // (the readback's part wait: tests/test_readback_part.py)
Pool* pool_of(uint64_t guest, uint64_t) {
    lookups++;
    return guest < second_pool.lo ? &first_pool : &second_pool;
}
struct Host {
    const uint8_t* gpu_ptr(uint64_t, uint64_t* avail) {
        static uint8_t bytes[1024]{};
        *avail = mapped ? sizeof bytes : 0;
        return mapped ? bytes : nullptr;
    }
    uint64_t gpu_guest(uint64_t) { return mapping; }
} host_value;
Host* host = &host_value;
int view_nvn_format(const nvn_tex*, const nvn_view* v) { return v ? v->format : 42; }
struct Format { VkFormat format; };
Format vk_format(int nf) {
    return {nf == 42 ? VK_FORMAT_R32_UINT : nf == 43 ? VK_FORMAT_R32_SFLOAT :
            nf == 44 ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_UNDEFINED};
}
VkFormatProperties format_props(VkFormat f) {   // (format 44: the device has no texel buffers of it)
    VkFormatProperties fp{};
    if (f != VK_FORMAT_R32G32B32_SFLOAT)
        fp.bufferFeatures = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
    return fp;
}
int nvn_block_size(int, int* w, int* h) { *w = *h = 1; return 4; }
void texture_gpu_wrote(uint64_t gpu, uint64_t size) { writes++; written_gpu = gpu; written_size = size; }
using VmaAllocation = void*;
struct DeadMirror { uint64_t value; VkBuffer buffer; VmaAllocation alloc; };
std::deque<DeadMirror> dead_mirrors;
/* DEAD IMPORT */
std::deque<DeadImport> dead_imports;
uint64_t st_retired_imports_freed = 0, st_retired_import_bytes_freed = 0;
uint64_t dead_import_bytes = 0;   // Retired allocations still count until their GPU work completes.
std::deque<std::map<uint64_t, Pool>::node_type> evicted_pools;
void logf(const char*, ...) {}
const char* getenv(const char*) { return nullptr; }   // (unrelated diagnostic toggle in reap_mirrors)
bool env_on(const char* name) { const char* v = getenv(name); return v && !env_value_off(v); }   // (the switches through the stand-in)
VkDeviceAddress vkGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo*) { return 0; }
void destroy_buffer(VkBuffer, VmaAllocation) {}
void vkDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    std::lock_guard lk(driver_mu);
    for (const auto& [view, ci] : live) { (void)view; assert(ci.buffer != buffer); }
    destroyed_buffers++; destruction_order.push_back('B');
}
void vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
VkResult vkCreateBufferView(VkDevice, const VkBufferViewCreateInfo* ci, const VkAllocationCallbacks*, VkBufferView* out) {
    std::lock_guard lk(driver_mu);
    creates++;
    *out = handle<VkBufferView>(0xdead);
    if (create_result != VK_SUCCESS) return create_result;
    *out = handle<VkBufferView>(100 + creates);
    live.emplace(*out, *ci);
    // MSVC's map also allocates its sentinel. Arm the allocator only after the
    // Vulkan resource exists, so every STL fails the same cache commit operation.
    if (fail_view_commit) { fail_view_commit = false; fail_after = 0; }
    return VK_SUCCESS;
}
void vkDestroyBufferView(VkDevice, VkBufferView view, const VkAllocationCallbacks*) {
    std::lock_guard lk(driver_mu);
    assert(live.erase(view) == 1);   // (undefined error output and duplicate destruction both fail)
    destroyed_views++; destruction_order.push_back('V');
}
/* ACTUAL */

VkBufferView get(nvn_tex& tex, const nvn_view* v = nullptr, bool storage = false) {
    return texel_buffer_view(&tex, v, storage);
}
void release_all() {
    release_texel_buffer_views(first_pool.buffer);
    release_texel_buffer_views(second_pool.buffer);
    assert(live.empty());
}
void run(const std::string& test) {
    dev.props.limits.minTexelBufferOffsetAlignment = 4;
    dev.props.limits.maxTexelBufferElements = 1024;
    nvn_tex tex;
    if (test == "late-map") {
        mapped = false; assert(!get(tex)); mapped = true; assert(get(tex));
    } else if (test == "host-oom" || test == "device-oom" || test == "other-error") {
        create_result = test == "host-oom" ? VK_ERROR_OUT_OF_HOST_MEMORY :
                        test == "device-oom" ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_UNKNOWN;
        assert(!get(tex)); assert(!get(tex)); assert(creates == 1 && live.empty());
        create_result = VK_SUCCESS; frame_no++; assert(get(tex)); assert(creates == 2);
    } else if (test == "registry-commit" || test == "view-commit") {
        fail_after = test == "registry-commit" ? 0 : -1;
        fail_view_commit = test == "view-commit";
        assert(!get(tex)); assert(live.empty());
        assert(destroyed_views == (test == "view-commit" ? 1u : 0u));
        unsigned attempts = creates; assert(!get(tex)); assert(creates == attempts);
        frame_no++; assert(get(tex));
    } else if (test == "alignment") {
        mapping++; assert(!get(tex)); assert(creates == 0);
        mapping += 3; auto v = get(tex); assert(v && live.at(v).offset == 4);
    } else if (test == "zero-limit") {
        dev.props.limits.maxTexelBufferElements = 0; assert(!get(tex)); assert(creates == 0);
    } else if (test == "clamped-range") {
        dev.props.limits.maxTexelBufferElements = 8; auto v = get(tex); assert(v && live.at(v).range == 32);
        tex.d.width = 32; assert(get(tex) == v && creates == 1);
    } else if (test == "unsupported") {
        nvn_view v{44}; assert(!get(tex, &v)); assert(!get(tex, &v, true));
        assert(creates == 0 && lookups == 0 && writes == 0);
    } else if (test == "invalid") {
        assert(!texel_buffer_view(nullptr, nullptr, false));
        tex.gpu = 0; assert(!get(tex)); tex.gpu = 0x1000;
        nvn_view v{0}; assert(!get(tex, &v)); assert(creates == 0 && lookups == 0);
    } else if (test == "concurrent") {
        // The pool stamp protects published backings. The worker drops only unpublished buffers;
        // exercise that concurrent registry access, and simultaneous lookups of one live backing.
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; i++) threads.emplace_back([&] {
            for (int j = 0; j < 100; j++) assert(get(tex));
        });
        threads.emplace_back([] {
            for (int j = 0; j < 100; j++) destroy_import_buffer(second_pool.buffer);
        });
        for (auto& t : threads) t.join();
        assert(creates == 1 && lookups == 400 && live.size() == 1 && destroyed_buffers == 100);
    } else {
        auto first = get(tex); assert(first && live.at(first).buffer == first_pool.buffer);
        if (test == "remap") {
            mapping = second_pool.lo; auto second = get(tex);
            assert(second && second != first && live.at(second).buffer == second_pool.buffer);
        } else if (test == "offset") {
            mapping += 4; auto second = get(tex);
            assert(second && second != first && live.at(second).offset == 4);
        } else if (test == "format") {
            nvn_view view{43}; auto second = get(tex, &view);
            assert(second && second != first && live.at(second).format == VK_FORMAT_R32_SFLOAT);
        } else if (test == "range") {
            tex.d.width = 8; auto second = get(tex);
            assert(second && second != first && live.at(second).range == 32);
        } else if (test == "liveness") {
            mem_rec_value = 1001; assert(get(tex) == first);
            assert(first_pool.last_value == mem_rec_value && lookups == 2);
        } else if (test == "cached-after-failure") {
            tex.d.width++; create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY; assert(!get(tex));
            tex.d.width--; assert(get(tex) == first); assert(live.size() == 1 && creates == 2);
        } else if (test == "independent") {
            tex.d.width++; create_result = VK_ERROR_OUT_OF_HOST_MEMORY; assert(!get(tex));
            create_result = VK_SUCCESS; mapping = second_pool.lo; assert(get(tex)); assert(creates == 3);
        } else if (test == "retirement" || test == "reuse-handle") {
            dead_imports.push_back({first_pool.buffer, {}, 4, 64, {}});
            dead_import_bytes = 64;
            mem_completed = 3; reap_mirrors(); assert(live.count(first) && destroyed_buffers == 0);
            assert(dead_import_bytes == 64);
            mem_completed = 4; reap_mirrors();
            assert(live.empty() && destroyed_buffers == 1 && destroyed_views == 1);
            assert(dead_import_bytes == 0);
            assert(destruction_order == std::vector<char>({'V', 'B'}));
            if (test == "reuse-handle") { auto next = get(tex); assert(next && next != first && creates == 2); }
        } else if (test == "eviction") {
            std::map<uint64_t, Pool> pools; first_pool.retired_value = 5;
            pools.emplace(first_pool.lo, first_pool); evicted_pools.push_back(pools.extract(pools.begin()));
            mem_completed = 4; imports_give_back(); assert(live.count(first) && destroyed_buffers == 0);
            mem_completed = 5; imports_give_back();
            assert(live.empty() && destroyed_buffers == 1 && evicted_pools.empty());
            assert(destruction_order == std::vector<char>({'V', 'B'}));
        } else if (test == "storage") {
            assert(get(tex, nullptr, true) == first); assert(creates == 1 && writes == 1);
            assert(written_gpu == tex.gpu && written_size == 64 && lookups == 2);
        } else {
            assert(test == "healthy" && get(tex) == first && creates == 1 && lookups == 2);
        }
    }
    release_all();
}
} // namespace fixture
int main(int argc, char** argv) {
    assert(argc == 2); fixture::run(argv[1]);
    return 0;
}
