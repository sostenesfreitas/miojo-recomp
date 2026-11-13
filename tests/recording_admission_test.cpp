#define _CRT_SECURE_NO_WARNINGS
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
bool fail_stream_commit = false, fail_pool_commit = false;
// Replace only the container allocator boundary, so actual ownership-commit code can fail.
template<class T> struct FaultVector : std::vector<T> {
    using Base = std::vector<T>;
    using typename Base::iterator;
    bool failing() const { return std::is_same_v<T, VkDescriptorPool> ? fail_pool_commit : fail_stream_commit; }
    iterator insert(iterator at, const T& value) {
        if (failing()) throw std::bad_alloc();
        return Base::insert(at, value);
    }
    void push_back(const T& value) {
        if (failing()) throw std::bad_alloc();
        Base::push_back(value);
    }
};
struct Allocation { std::vector<uint8_t> bytes; };
using VmaAllocation = Allocation*;
struct { VkDevice device{}; VkPhysicalDeviceProperties props{}; } dev;
std::map<VkBuffer, Allocation> buffers;
std::set<VkDescriptorPool> pools;
std::map<VkDescriptorSet, VkDescriptorPool> sets;
uintptr_t serial = 100;
template<class T> T handle() { return reinterpret_cast<T>(++serial); }
bool fail_buffer = false, fail_map = false, fail_pool = false;
int fail_sets = 0, allocation_calls = 0, invalid_pool_calls = 0, destroys = 0, updates = 0, binds = 0;
uint64_t largest_request = 0;
std::mutex par_mutex;
struct ParLock { std::lock_guard<std::mutex> lock{par_mutex}; };
void logf(const char*, ...) {}
bool create_buffer(uint64_t bytes, VkBufferUsageFlags, bool, VkBuffer& buffer, VmaAllocation& allocation, void** map, bool = false) {
    ++allocation_calls; largest_request = std::max(largest_request, bytes);
    if (fail_buffer || bytes > (16ull << 20)) return false;
    buffer = handle<VkBuffer>(); auto& a = buffers[buffer]; a.bytes.assign((size_t)bytes, 0);
    allocation = &a; *map = fail_map ? nullptr : a.bytes.data(); return true;
}
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation) {
    require(buffer && buffers.count(buffer) && &buffers.at(buffer) == allocation, "buffer ownership/double free");
    buffers.erase(buffer); ++destroys;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* pool) {
    // Vulkan output handles must not be trusted after a failing creation result.
    if (fail_pool) { *pool = reinterpret_cast<VkDescriptorPool>(uintptr_t(0xdead)); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    *pool = handle<VkDescriptorPool>(); pools.insert(*pool); return VK_SUCCESS;
}
void drop_sets(VkDescriptorPool pool) {
    for (auto it = sets.begin(); it != sets.end();) {
        if (it->second == pool) it = sets.erase(it); else ++it;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice, VkDescriptorPool pool, VkDescriptorPoolResetFlags) {
    require(pools.count(pool), "reset invalid pool"); drop_sets(pool); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice, VkDescriptorPool pool, const VkAllocationCallbacks*) {
    require(pools.erase(pool) == 1, "pool ownership/double free"); drop_sets(pool);
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* set) {
    if (!pools.count(info->descriptorPool)) { ++invalid_pool_calls; return VK_ERROR_UNKNOWN; }
    if (fail_sets > 0) { --fail_sets; return VK_ERROR_OUT_OF_POOL_MEMORY; }
    *set = handle<VkDescriptorSet>(); sets[*set] = info->descriptorPool; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t n, const VkWriteDescriptorSet* writes, uint32_t, const VkCopyDescriptorSet*) {
    for (uint32_t i = 0; i < n; ++i) require(sets.count(writes[i].dstSet), "update failed/destroyed set");
    ++updates;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t n, const VkDescriptorSet* bound, uint32_t, const uint32_t*) {
    require(n == 1 && sets.count(bound[0]), "bind failed/destroyed set"); ++binds;
}
/* RES */
std::deque<RecordingRes> res;
thread_local RecordingRes* cur = nullptr;
const uint64_t kStream = 1024;
constexpr VkBufferUsageFlags kStreamUse = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
std::atomic<uint64_t> stream_extra{0}, stream_grown{0}, stream_peak{0};
bool grow_off = false;
bool stream_grow_off() { return grow_off; }
struct BufferRef { VkBuffer buffer{}; uint64_t offset = 0, size = 0, address = 0; };
/* POOL */
/* STREAM */
thread_local VkCommandBuffer rec_cmd{};
thread_local uint64_t rec_value = 0, targets_serial = 0;
// Global defaults are outside this per-recording allocation contract.
VkBuffer zero_buffer = reinterpret_cast<VkBuffer>(uintptr_t(99)); VmaAllocation zero_alloc{};
struct Pass {}; Pass& pass() { static thread_local Pass p; return p; }
bool work_since_barrier = false, last_dyn_valid = false, last_desc_valid = false, last_vb_valid = false;
VkPipeline last_pipeline{};
struct { bool on = false; } vertex_set, last_ib;
void descriptors_disturbed() {}
std::unordered_map<int, int>& handle_hits() { static thread_local std::unordered_map<int, int> h; return h; }
struct Program {};
constexpr int kNvnStages = 8;
int desc_reuse_level() { return 2; }   // (vk_draw.cpp: RX_VK_DESC_REUSE and the Descriptor reuse row; the banks are kept)
/* BANK_CACHE */
std::array<std::array<BankCache, kNvnStages>, 2>& bank_cache() { static thread_local std::array<std::array<BankCache, kNvnStages>, 2> b; return b; }
void make_dummies() {}
/* BEGIN */
#define FINE_SCOPE(...) ((void)0)
#define FINE_N(...) ((void)0)
struct NvnState { uint64_t bind_gen = 1, tex_gen[kNvnStages]{}, ssbo_gen[kNvnStages]{}; uint32_t grid[3]{1, 1, 1}; uint8_t value = 17; };
struct Stage { int nvn_stage = 0; };
thread_local bool grid_patch_want = false;
thread_local uint64_t grid_patch_at = UINT64_MAX;
thread_local VkBuffer grid_patch_buf{};
thread_local int bank_fills = 0;
void driver_bank_fill(const NvnState& s, int, int, void* out, size_t n) { ++bank_fills; std::memset(out, s.value, n); }
uint32_t driver_bank_word(const NvnState& s, int, int, uint32_t) { return uint32_t(s.value) * 0x01010101u; }
VkDescriptorBufferInfo cached_bank(const NvnState& s, const Stage& st) {
    const uint32_t index = 0, used = 16;
    const uint64_t align = 256;
    VkDescriptorBufferInfo bi{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    /* BANK_BRANCH */
    return bi;
}
struct Writes { std::vector<VkWriteDescriptorSet> writes{VkWriteDescriptorSet{}}; } writes;
Writes& dw() { return writes; }
bool bind_tail() {
    VkCommandBuffer cmd{}; VkPipelineBindPoint bp = VK_PIPELINE_BIND_POINT_GRAPHICS;
    struct { VkDescriptorSetLayout set_layout{}; VkPipelineLayout layout{}; } p;
    /* DESCRIPTORS */
}
void begin(int slot = 0, uint64_t value = 1) { draw_begin_recording(slot, VkCommandBuffer{}, value); }
void cleanup() {
    for (auto& r : res) {
        for (auto& s : r.streams) destroy_buffer(s.buffer, s.alloc);
        for (auto p : r.extra) vkDestroyDescriptorPool(dev.device, p, nullptr);
        if (r.pool) vkDestroyDescriptorPool(dev.device, r.pool, nullptr);
    }
    require(buffers.empty() && pools.empty() && sets.empty(), "owned resources leaked");
}
void copied(size_t bytes, uint8_t value) {
    std::vector<uint8_t> input(bytes, value);
    const auto b = stream_copy(input.data(), input.size());
    require(b.buffer && b.size == bytes && b.offset <= buffers.at(b.buffer).bytes.size() - bytes, "copy binding bounds");
    require(std::equal(input.begin(), input.end(), buffers.at(b.buffer).bytes.begin() + b.offset), "CPU copy data");
}
int main(int argc, char** argv) {
    require(argc == 2, "case argument"); const std::string mode = argv[1];
    dev.props.limits.minUniformBufferOffsetAlignment = 256;
    dev.props.limits.minStorageBufferOffsetAlignment = 64;
    if (mode == "no-recording") {
        require(stream_alloc(16, 256) == UINT64_MAX && allocation_calls == 0, "absent recording authorizes write");
        cleanup(); return 0;
    }
    if (mode == "initial-buffer" || mode == "initial-map" || mode == "initial-commit") {
        fail_buffer = mode == "initial-buffer"; fail_map = mode == "initial-map"; fail_stream_commit = mode == "initial-commit";
        begin();
        require(res[0].streams.empty() && !cur->stream && !cur->mapped && cur->size == 0, "incomplete initial stream published");
        require(stream_alloc(16, 256) == UINT64_MAX && buffers.empty(), "missing initial stream authorizes write/leaks");
        fail_buffer = fail_map = fail_stream_commit = false; begin(0, 2); copied(31, 17);
        require(res[0].streams.size() == 1, "initial retry must create exactly one owned stream");
    } else if (mode == "pool-retry") {
        fail_pool = true; begin(); const auto first = cur->stream;
        require(!bind_tail() && !invalid_pool_calls, "null initial pool reached Vulkan");
        begin(0, 2); require(res[0].streams.size() == 1 && cur->stream == first, "pool retry duplicated/replaced stream");
        fail_pool = false; begin(0, 3); require(bind_tail(), "pool retry did not recover"); copied(63, 51);
    } else if (mode == "parallel") {
        // Actual TLS selection and serialized resource admission, without concurrent growth of fixture maps.
        std::thread a([] { begin(0); std::vector<uint8_t> input(31, 17); require(stream_copy(input.data(), input.size()).buffer != VK_NULL_HANDLE, "worker0 copy"); });
        std::thread b([] { begin(1); std::vector<uint8_t> input(63, 51); require(stream_copy(input.data(), input.size()).buffer != VK_NULL_HANDLE, "worker1 copy"); });
        a.join(); b.join(); require(res.size() == 2 && res[0].stream != res[1].stream, "worker slot ownership");
    } else {
        begin(); copied(127, 23);
        const auto old = cur->stream; const auto used = cur->used; const auto count = buffers.size();
        if (mode == "bank-same") {
            NvnState state; state.ssbo_gen[0] = 1;
            const auto first = cached_bank(state, Stage{});
            state.ssbo_gen[0] = 2; state.bind_gen = 2; state.grid[0] = 7;
            const auto second = cached_bank(state, Stage{}); const int fills = bank_fills;
            const auto third = cached_bank(state, Stage{});
            require(first.buffer == second.buffer && first.offset == second.offset && third.buffer == first.buffer &&
                    bank_fills == fills, "identical bank must retain its binding and cache the new generation");
        } else if (mode == "bank-retry") {
            NvnState state; state.ssbo_gen[0] = 1;
            const auto first = cached_bank(state, Stage{});
            require(first.buffer, "initial shader bank");
            // Fill the remaining stream so the changed bank must grow it.
            const auto remaining = cur->size - ((cur->used + 255) & ~255ull);
            if (remaining) copied((size_t)remaining, 91);
            state.ssbo_gen[0] = 2; state.bind_gen = 2; state.value = 29;
            fail_buffer = true;
            require(!cached_bank(state, Stage{}).buffer, "failed shader-bank allocation accepted");
            fail_buffer = false;
            const auto retry = cached_bank(state, Stage{});
            require(retry.buffer, "shader bank retry did not recover");
            const auto& bytes = buffers.at(retry.buffer).bytes;
            require(std::all_of(bytes.begin() + retry.offset, bytes.begin() + retry.offset + retry.range,
                                [](uint8_t v) { return v == 29; }), "bank retry returned stale shader bytes");
            require(buffers.at(first.buffer).bytes[first.offset] == 17, "old shader bank binding lost");
        } else if (mode == "growth-buffer" || mode == "growth-map" || mode == "growth-commit") {
            fail_buffer = mode == "growth-buffer"; fail_map = mode == "growth-map"; fail_stream_commit = mode == "growth-commit";
            std::vector<uint8_t> input(4096, 9);
            require(!stream_copy(input.data(), input.size()).buffer, "failed growth accepted");
            require(cur->stream == old && cur->used == used && cur->si == 0 && buffers.size() == count && stream_extra == 0 && stream_grown == 0, "failed growth changed ownership/cursor/counters");
            fail_buffer = fail_map = fail_stream_commit = false; copied(4096, 9); copied(1100000, 7);
            require(cur->si == 2 && buffers.count(old) && stream_grown == 2, "growth retry retains old bindings");
        } else if (mode == "overflow" || mode == "rounding" || mode == "alignment-zero" || mode == "alignment-three") {
            const auto calls = allocation_calls;
            uint64_t n = mode == "overflow" ? UINT64_MAX : mode == "rounding" ? UINT64_MAX - 1024 : 16;
            uint64_t align = mode == "alignment-zero" ? 0 : mode == "alignment-three" ? 3 : 256;
            require(stream_alloc(n, align) == UINT64_MAX && cur->stream == old && cur->used == used && allocation_calls == calls, "invalid/overflow range changed or admitted");
        } else if (mode == "grow-off") {
            grow_off = true; require(!stream_reserve(4096, 256), "disabled growth accepted");
            require(cur->stream == old && cur->used == used && buffers.size() == count, "disabled growth changed current stream");
        } else if (mode == "retention" || mode == "reuse") {
            copied(4096, 91); const auto grown = cur->stream;
            begin(0, 2); require(cur->stream == old && cur->used == 0 && cur->streams.size() == 2, "recording reuse reset");
            copied(4096, 73); require(cur->stream == grown && buffers.size() == count + 1, "cached grown stream reuse");
            if (mode == "retention") {
                begin(0, 3); for (int i = 0; i < 120; ++i) begin(0, uint64_t(i + 4));
                require(cur->streams.size() == 1 && !buffers.count(grown) && stream_extra == 0, "idle grown stream retirement");
            }
        } else if (mode == "descriptor-retained") {
            fail_sets = 1; require(bind_tail() && cur->extra.size() == 1, "first extra pool");
            const auto retained = cur->extra[0]; const auto count_before = pools.size();
            fail_sets = 2; require(!bind_tail(), "second pool allocation failure");
            require(cur->extra.size() == 1 && cur->extra[0] == retained && pools.count(retained) && pools.size() == count_before,
                    "rollback destroyed or displaced a retained pool");
            require(bind_tail() && cur->extra.size() == 1, "retained pool could not retry");
        } else if (mode == "descriptor-pool" || mode == "descriptor-set" || mode == "descriptor-commit") {
            const auto pool_count = pools.size(); fail_sets = mode == "descriptor-set" ? 2 : 1;
            fail_pool = mode == "descriptor-pool"; fail_pool_commit = mode == "descriptor-commit";
            require(!bind_tail() && !updates && !binds && !invalid_pool_calls, "failed descriptor allocation updated/bound/invalid pool");
            require(cur->extra.empty() && pools.size() == pool_count, "failed descriptor pool ownership retained/leaked");
            fail_pool = fail_pool_commit = false; fail_sets = 1;
            require(bind_tail() && cur->extra.size() == 1 && updates == 1 && binds == 1, "descriptor retry did not recover");
        } else if (mode == "healthy") {
            require(bind_tail() && cur->extra.empty() && updates == 1 && binds == 1, "healthy primary descriptor binding");
            copied(17, 51); require(cur->stream == old && cur->used == 273, "alignment/prefix preservation");
        } else require(false, "unknown case");
    }
    require(!invalid_pool_calls, "invalid pool call escaped"); cleanup();
    std::printf("PASS %s actual allocation/ownership/retry\n", argv[1]);
}
