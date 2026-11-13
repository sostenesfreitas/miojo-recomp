#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fixture {
void require(bool ok, const char* why) { if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
struct VmaAllocation_T { std::vector<uint8_t> cpu, gpu; bool alive = true; };
using VmaAllocation = VmaAllocation_T*;
using VmaAllocator = int;
VmaAllocator vma = 0;
std::vector<std::unique_ptr<VmaAllocation_T>> allocations;
bool coherent = false, fail_buffer = false;
// Keep the production 1-MB rounding, with a smaller authored large-arena threshold to bound test memory.
constexpr uint64_t kModelArena = 2u << 20;
bool fail_large = false, unmap_large = false, unmap_all = false;
std::vector<uint64_t> attempts;
unsigned destroys = 0;
uint64_t rec_value = 1, completed_value = 0;
struct { VkPhysicalDeviceProperties props{}; } dev;
int flushes = 0;
bool create_buffer(uint64_t n, VkBufferUsageFlags, bool, VkBuffer& b, VmaAllocation& a, void** map) {
    attempts.push_back(n);
    if (fail_buffer || (fail_large && n >= kModelArena)) return false;
    allocations.push_back(std::make_unique<VmaAllocation_T>());
    a = allocations.back().get(); a->cpu.resize(n); a->gpu.resize(n);
    b = reinterpret_cast<VkBuffer>(a);
    *map = unmap_all || (unmap_large && n >= kModelArena) ? nullptr : a->cpu.data();
    return true;
}
void destroy_buffer(VkBuffer b, VmaAllocation a) {
    require(a && a->alive && b == reinterpret_cast<VkBuffer>(a), "only an owned unmapped arena is destroyed once");
    a->alive = false; a->cpu.clear(); a->gpu.clear(); destroys++;
}
VkResult vmaFlushAllocation(VmaAllocator, VmaAllocation a, uint64_t offset, uint64_t size) {
    require(a && a->alive && offset <= a->cpu.size() && size <= a->cpu.size() - offset, "allocation-relative flush bounds");
    const uint64_t atom = dev.props.limits.nonCoherentAtomSize;
    const uint64_t lo = offset / atom * atom, hi = std::min<uint64_t>(a->cpu.size(), (offset + size + atom - 1) / atom * atom);
    std::copy(a->cpu.begin() + lo, a->cpu.begin() + hi, a->gpu.begin() + lo); flushes++; return VK_SUCCESS;
}
void logf(const char*, ...) {}
void consume(VkCommandBuffer) {
    for (auto& a : allocations) {
        if (!a->alive) continue;
        if (coherent) a->gpu = a->cpu;
        require(a->cpu == a->gpu, "GPU texture consumer sees all authored staging bytes");
    }
}
VkCommandBuffer upload_cmd() { return VK_NULL_HANDLE; }
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
bool overlaps(const Ranges* r, uint64_t a, uint64_t b) {
    return !r || std::any_of(r->begin(), r->end(), [=](auto x) { return a < x.second && b > x.first; });
}
struct Texture {
    bool astc = false; uint32_t block_w = 1, block_h = 1;
    VkImageType type = VK_IMAGE_TYPE_2D; VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t layers = 2; struct { uint64_t layer_size = 1536; } L;
    uint64_t gpu = 0;
};
struct { uint64_t offset = 0; } geo;
auto level_geo(const Texture&, int) { return geo; }
bool astc_ready() { return true; }
std::vector<uint8_t> guest(8192, 0x5b);
const uint8_t* guest_bytes(uint64_t at, uint64_t n, std::vector<uint8_t>&) {
    require(at <= guest.size() && n <= guest.size() - at, "authored guest bounds"); return guest.data() + at;
}
int64_t st_bytes = 0;
struct { uint32_t w=8, tw=8, d=1; } g;
Texture tex;
uint32_t px_h=2, th=2, level=0, layer=0, y0=0;
VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
std::vector<VkBufferImageCopy> copies;
/* ACTUAL */

void cpu(uint64_t n, uint8_t pattern) {
    auto s = staging(n, 16); require(s.data && s.allocation, "slice owns allocation");
    std::memset(s.data, pattern, n); cpu_slice(s, n); consume({});
    require(copies.back().bufferOffset == s.offset, "copy and publication address same slice");
}
int run(int argc, char** argv) {
    require(argc == 3, "case/cache arguments"); coherent = std::string(argv[2]) == "coherent";
    dev.props.limits.nonCoherentAtomSize = 1024;
    dev.props.limits.minStorageBufferOffsetAlignment = 256;
    const std::string test = argv[1];
    if (test == "cpu") { cpu(37, 0x5b); cpu(513, 0xa7); require(flushes == 2, "one flush per completed slice"); }
    else if (test == "astc") {
        cpu(37, 0x3c); require(upload_astc_gpu(tex, tex.L.layer_size * tex.layers, nullptr), "ASTC gathered layers published");
        Ranges dirty{{tex.L.layer_size, tex.L.layer_size * 2}};
        require(upload_astc_gpu(tex, tex.L.layer_size * tex.layers, &dirty), "dirty layer published");
        require(flushes == 3, "ASTC flushes each gathered allocation slice");
    } else if (test == "atom") {
        auto a = staging(17,16), b = staging(19,16);
        require(a.offset % 1024 == 0 && b.offset % 1024 == 0 && b.offset >= a.offset + 1024, "distinct noncoherent atoms");
    } else if (test == "reuse") {
        cpu(37,0x5b); auto old = arenas.front().buffer; completed_value = rec_value++;
        cpu(37,0xa7); require(arenas.size() == 1 && arenas.front().buffer == old, "completed arena reused with new bytes");
    } else if (test == "inflight-tail") {
        cpu(37, 0x5b);
        require(arenas.size() == 1, "initial arena allocated");
        auto old = arenas.front().buffer;
        auto first_used = arenas.front().used;
        rec_value++; // advance timeline without completing the first slice
        cpu(49, 0xa7);
        require(arenas.size() == 1 && arenas.front().buffer == old, "in-flight arena tail reused without new allocation");
        require(arenas.front().value == rec_value, "in-flight arena bumped to latest rec_value");
        require(arenas.front().used > first_used, "second slice allocated past first slice");
        require(flushes == 2, "both slices flushed independently");
    } else if (test == "multiple") {
        cpu(kModelArena,0x5b); cpu(31,0xa7); require(arenas.size() == 2, "multiple arenas independently published");
    } else if (test == "failure") {
        fail_buffer = true; auto s = staging(64); require(!s.buffer && !s.data && !s.allocation && flushes == 0, "failed slice unpublished");
    } else if (test == "empty-range") {
        flush_mapped_range(nullptr, 0, 0); require(flushes == 0, "empty range no VMA call");
    } else if (test == "small-fallback" || test == "unmapped-fallback") {
        fail_large = test == "small-fallback"; unmap_large = test == "unmapped-fallback";
        cpu(37,0x5b);
        require(attempts == std::vector<uint64_t>({kModelArena, 1u << 20}), "large attempt falls back to rounded requested size");
        require(arenas.size() == 1 && arenas.front().size == (1u << 20), "only mapped fallback arena is committed");
        require(destroys == (unmap_large ? 1u : 0u) && flushes == 1, "fallback cleanup and one publication");
    } else if (test == "unmapped-both") {
        unmap_all = true; auto s = staging(64);
        require(!s.buffer && !s.data && !s.allocation && arenas.empty() && flushes == 0, "no unmapped slice admitted");
        require(attempts.size() == 2 && destroys == 2, "both successfully allocated unmapped attempts released");
    } else if (test == "fallback-astc") {
        fail_large = true; require(upload_astc_gpu(tex, tex.L.layer_size * tex.layers, nullptr), "ASTC gather uses the mapped fallback");
        require(attempts == std::vector<uint64_t>({kModelArena, 1u << 20}) && flushes == 1, "ASTC fallback bytes published once");
    } else if (test == "fallback-reuse") {
        fail_large = true; cpu(37,0x5b); auto old = arenas.front().buffer;
        completed_value = rec_value++; cpu(37,0xa7);
        require(attempts.size() == 2 && arenas.size() == 1 && arenas.front().buffer == old, "completed fallback reused without another allocation");
        require(flushes == 2, "fallback reuse publishes new bytes");
    } else require(false, "known case");
    return 0;
}
} // namespace fixture
int main(int argc, char** argv) { return fixture::run(argc, argv); }
