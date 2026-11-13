#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "import_hold.h"

namespace fixture {
template<class T> T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
enum { HostMemory, DmaMemory, EveryMemory };
enum class Fault { None, Create, Allocate, HostOOM, DeviceOOM, Unknown, Properties };
Fault fault = Fault::None;
int fail_kind = EveryMemory, allocation_kind = HostMemory;
struct Pool {
    uint64_t lo = 0, hi = 0;
    VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceAddress address = 0;
    std::vector<VkDeviceMemory> pieces;
};
struct { VkDevice device{}; VkPhysicalDeviceMemoryProperties memory{}; bool dma_buf_import = true, sparse_buffers = true; VkDriverId driver_id{}; } dev;
struct Host { uint8_t* guest_base() { static uint8_t bytes[4096]{}; return bytes; } } host_value;
Host* host = &host_value;
uint64_t max_import_size = UINT64_MAX;
bool import_cooling = false;
VkResult import_result = VK_SUCCESS;
mkwvk::ImportHolds import_holds;
std::mutex import_holds_mu;
int64_t now_ms = 100;
int64_t import_now_ms() { return now_ms; }
unsigned creates = 0, allocations = 0, binds = 0, address_queries = 0, destroyed_buffers = 0, freed_memory = 0;
unsigned sparse_calls = 0;
unsigned owner_releases = 0;
uintptr_t next_id = 1;
std::map<VkBuffer, int> buffers;
std::set<VkBuffer> bound_buffers;
std::set<VkDeviceMemory> memories;
std::vector<char> release_order;
constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
bool failing(Fault f, int kind) { return fault == f && (fail_kind == EveryMemory || fail_kind == kind); }
void logf(const char*, ...) {}
const char* getenv(const char*) { return nullptr; }
bool env_on(const char* name) { const char* v = getenv(name); return v && !env_value_off(v); }   // (the switches through the stand-in)
using SIZE_T = size_t;
constexpr int MEM_COMMIT = 1, MEM_RESERVE = 2, PAGE_READWRITE = 4;
struct MEMORY_BASIC_INFORMATION { uint8_t* BaseAddress; size_t RegionSize; int State; };
void* VirtualAlloc(void* p, size_t, int, int) { return p; }
size_t VirtualQuery(void*, MEMORY_BASIC_INFORMATION*, size_t) { return 0; }
VkResult vkGetMemoryHostPointerPropertiesEXT(VkDevice, VkExternalMemoryHandleTypeFlagBits, const void*, VkMemoryHostPointerPropertiesEXT* hp) {
    if (fault == Fault::Properties) return VK_ERROR_OUT_OF_HOST_MEMORY;
    hp->memoryTypeBits = 1; return VK_SUCCESS;
}
VkResult vkCreateBuffer(VkDevice, const VkBufferCreateInfo* ci, const VkAllocationCallbacks*, VkBuffer* out) {
    creates++;
    const auto* external = static_cast<const VkExternalMemoryBufferCreateInfo*>(ci->pNext);
    allocation_kind = external->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? DmaMemory : HostMemory;
    *out = handle<VkBuffer>(0xdead);
    if (failing(Fault::Create, allocation_kind)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<VkBuffer>(next_id++); buffers.emplace(*out, allocation_kind); return VK_SUCCESS;
}
VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    allocations++; *out = handle<VkDeviceMemory>(0xbeef);
    if (failing(Fault::Allocate, allocation_kind)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = handle<VkDeviceMemory>(next_id++); memories.insert(*out); return VK_SUCCESS;
}
VkResult vkBindBufferMemory(VkDevice, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
    binds++; assert(buffers.count(b) && memories.count(m) && offset == 0);
    const int kind = buffers.at(b);
    if (failing(Fault::HostOOM, kind)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (failing(Fault::DeviceOOM, kind)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (failing(Fault::Unknown, kind)) return VK_ERROR_UNKNOWN;
    bound_buffers.insert(b); return VK_SUCCESS;
}
VkDeviceAddress vkGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* ci) {
    assert(bound_buffers.count(ci->buffer));   // (never query an unbound buffer, including an ignored failed bind)
    address_queries++; return 0x100000;
}
void vkDestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) {
    assert(buffers.erase(b) == 1); bound_buffers.erase(b); destroyed_buffers++; release_order.push_back('B');
}
void vkFreeMemory(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) {
    assert(memories.erase(m) == 1); freed_memory++; release_order.push_back('M');
}
void release_texel_buffer_views(VkBuffer b) {
    assert(buffers.count(b));   // Owner invalidation precedes raw buffer destruction.
    owner_releases++;
}
int dmabuf_memfd = -2, dmabuf_dev = -1;
uint64_t dmabuf_limit = 0;
bool vkGetMemoryFdPropertiesKHR = true;
constexpr int O_RDWR = 1, O_CLOEXEC = 2;
void* RTLD_DEFAULT = nullptr;
int memfd_stub() { return 4; }
void* dlsym(void*, const char*) { return reinterpret_cast<void*>(&memfd_stub); }
unsigned opens = 0;
int open(const char*, int) { opens++; return 5; }
FILE* fopen(const char*, const char*) { return nullptr; }
int mock_fscanf(FILE*, const char*, ...) { return 0; }
VkDeviceMemory dma_buf_memory(uint64_t, uint64_t) {
    VkDeviceMemory memory{};
    return vkAllocateMemory(dev.device, nullptr, nullptr, &memory) == VK_SUCCESS ? memory : VK_NULL_HANDLE;
}
bool import_dma_buf_pieces(uint64_t lo, uint64_t hi, uint64_t, Pool& p) {
    // Only delegation is modeled: the sparse queue/fence implementation is outside this binding change.
    sparse_calls++; p.lo = lo; p.hi = hi;
    p.buffer = handle<VkBuffer>(next_id++); p.memory = handle<VkDeviceMemory>(next_id++);
    buffers.emplace(p.buffer, DmaMemory); bound_buffers.insert(p.buffer); memories.insert(p.memory);
    return true;
}
#define fscanf mock_fscanf
/* ACTUAL */
#undef fscanf

void empty() { assert(buffers.empty() && memories.empty() && bound_buffers.empty()); }
void release(Pool& p) {
    vkDestroyBuffer(dev.device, p.buffer, nullptr); vkFreeMemory(dev.device, p.memory, nullptr);
    for (auto m : p.pieces) vkFreeMemory(dev.device, m, nullptr);
    empty();
}
bool call(const std::string& route, Pool& p, uint64_t hi = 4096) {
    return route == "dma" ? import_dma_buf(0, hi, p) : import_pool(0, hi, p);
}
void run(const std::string& route, const std::string& test) {
    dev.memory.memoryTypeCount = 1;
    dev.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (route == "host") dmabuf_memfd = -1;   // (the same host-pointer route on both test platforms)
    if (test == "create-error" || test == "dma-create-fallback") fault = Fault::Create;
    if (test == "allocate-error" || test == "dma-allocate-fallback") fault = Fault::Allocate;
    if (test == "host-oom") fault = Fault::HostOOM;
    if (test == "device-oom" || test == "dma-bind-fallback" || test == "both-bind-fail" || test == "hold-retry") fault = Fault::DeviceOOM;
    if (test == "unknown-bind") fault = Fault::Unknown;
    if (test == "properties-error") fault = Fault::Properties;
    if (route == "pool" && test != "both-bind-fail") fail_kind = DmaMemory;
    Pool p;
    if (test == "nvidia") {   // (NVIDIA's driver refuses the window's dma-bufs: no udmabuf opened, no dma-buf tried, host pointers)
        dev.driver_id = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
        if (route == "dma") {
            assert(!call(route, p) && opens == 0 && creates == 0 && allocations == 0 && dmabuf_memfd == -1); empty(); return;
        }
        assert(call(route, p) && opens == 0 && creates == 1 && allocations == 1 && buffers.at(p.buffer) == HostMemory);
        release(p); return;
    }
    if (test == "sparse-delegation") {
        assert(call(route, p, 128ull << 20)); assert(sparse_calls == 1 && binds == 0 && address_queries == 1);
        release(p); return;
    }
    const bool success = call(route, p);
    assert(owner_releases == destroyed_buffers);   // Every factory rollback preserves main's owner hook.
    if (test == "healthy" || (route == "pool" && test != "both-bind-fail")) {
        assert(success && p.buffer && p.memory && p.address && address_queries == 1);
        assert(buffers.size() == 1 && memories.size() == 1 && import_holds.rows.empty());
        if (route == "pool") {
            assert(creates == 2 && buffers.at(p.buffer) == HostMemory);
            assert(destroyed_buffers == (test == "dma-create-fallback" ? 0u : 1u));
            assert(freed_memory == (test == "dma-bind-fallback" ? 1u : 0u));
        }
        release(p); return;
    }
    assert(!success && address_queries == 0); empty();
    if (test == "create-error" || test == "properties-error") {
        assert(destroyed_buffers == 0 && freed_memory == 0 && binds == 0);
    } else if (test == "allocate-error") {
        assert(destroyed_buffers == 1 && freed_memory == 0 && binds == 0);
    } else {
        unsigned attempts = route == "pool" ? 2u : 1u;
        assert(destroyed_buffers == attempts && freed_memory == attempts && binds == attempts);
        assert(!p.buffer && !p.memory && !p.address);
        assert(release_order == (route == "pool" ? std::vector<char>({'B', 'M', 'B', 'M'}) : std::vector<char>({'B', 'M'})));
        if (route != "dma") assert(import_result == (test == "host-oom" ? VK_ERROR_OUT_OF_HOST_MEMORY : test == "unknown-bind" ? VK_ERROR_UNKNOWN : VK_ERROR_OUT_OF_DEVICE_MEMORY));
    }
    if (route != "dma") {
        assert(import_holds.rows.size() == 1);
        unsigned prior = creates; fault = Fault::None; assert(!call(route, p) && import_cooling && creates == prior);
        now_ms += mkwvk::ImportHolds::kGapMs; assert(call(route, p) && !import_cooling);
        assert(import_holds.rows.empty()); release(p);
    } else assert(import_holds.rows.empty());   // (a DMA failure must not suppress host fallback)
}
} // namespace fixture
int main(int argc, char** argv) {
    assert(argc == 3); fixture::run(argv[1], argv[2]); return 0;
}
