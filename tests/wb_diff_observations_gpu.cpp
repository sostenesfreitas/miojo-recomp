// Authored buffers only. Execute the actual generated write-back shader, optionally exporting observations.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
// @SHADER_HEADER@
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
#define OK(x) CHECK((x) == VK_SUCCESS)
#define APIS(X) X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkGetBufferDeviceAddress) X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) X(vkCmdPushConstants) X(vkCmdDispatch) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkQueueSubmit) X(vkWaitForFences)
#define DECLARE(n) PFN_##n n;
APIS(DECLARE)
VkDevice device;
VkPhysicalDeviceMemoryProperties memory_properties;

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* map{};
    VkDeviceAddress address{};
};
Buffer buffer(uint64_t bytes) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    OK(vkCreateBuffer(device, &ci, nullptr, &b.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, b.buffer, &requirements);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        const auto need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((requirements.memoryTypeBits & (1u << i)) && (flags & need) == need) { type = i; break; }
    }
    CHECK(type != UINT32_MAX);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &flags;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    OK(vkAllocateMemory(device, &allocation, nullptr, &b.memory));
    OK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    OK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map));
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(device, &info);
    CHECK(b.address);
    return b;
}
void release(Buffer b) {
    vkUnmapMemory(device, b.memory);
    vkDestroyBuffer(device, b.buffer, nullptr);
    vkFreeMemory(device, b.memory, nullptr);
}
struct Chunk { uint64_t mirror, shadow, guest; uint32_t count, vecs; };
struct Push { uint64_t table; uint32_t count, pad; uint64_t observations; };
static_assert(sizeof(Chunk) == 32 && sizeof(Push) == 24);
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
int main(int argc, char** argv) {
    const std::string requested = argc == 2 ? lower(argv[1]) : "";
#ifdef _WIN32
    const auto loader = LoadLibraryW(L"vulkan-1.dll");
    const auto symbol = [loader](const char* n) { return loader ? (void*)GetProcAddress(loader, n) : nullptr; };
#else
    const auto loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    const auto symbol = [loader](const char* n) { return loader ? dlsym(loader, n) : nullptr; };
#endif
    if (!loader) { std::puts("UNAVAILABLE: Vulkan loader"); return 77; }
#define LOAD(n) n = reinterpret_cast<PFN_##n>(symbol(#n)); if (!n) { std::puts("UNAVAILABLE: " #n); return 77; }
    APIS(LOAD)
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Hookshot authored write-back observations";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    OK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t count = 0;
    OK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> cards(count);
    OK(vkEnumeratePhysicalDevices(instance, &count, cards.data()));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    for (auto card : cards) {
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &subgroup;
        vkGetPhysicalDeviceProperties2(card, &properties);
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(card, &features);
        if (properties.properties.apiVersion < VK_API_VERSION_1_3 || !f12.bufferDeviceAddress || !f13.maintenance4 ||
            !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) ||
            lower(properties.properties.deviceName).find(requested) == std::string::npos) continue;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(card, &n, nullptr);
        std::vector<VkQueueFamilyProperties> queues(n);
        vkGetPhysicalDeviceQueueFamilyProperties(card, &n, queues.data());
        for (uint32_t i = 0; i < n; ++i) if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            physical = card; family = i; break;
        }
        if (physical) { std::printf("Device: %s\n", properties.properties.deviceName); break; }
    }
    if (!physical) { vkDestroyInstance(instance, nullptr); std::puts("UNAVAILABLE: matching Vulkan 1.3 compute/BDA/vote device"); return 77; }
    const float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.maintenance4 = true;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = true; f12.pNext = &f13;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f12; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &queue_info;
    OK(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1; layout_info.pPushConstantRanges = &range;
    VkPipelineLayout layout;
    OK(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = sizeof WB_DIFF_COMP_SPV; shader_info.pCode = WB_DIFF_COMP_SPV;
    VkShaderModule shader;
    OK(vkCreateShaderModule(device, &shader_info, nullptr, &shader));
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline_info.stage.module = shader;
    pipeline_info.stage.pName = "main"; pipeline_info.layout = layout;
    VkPipeline pipeline;
    OK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool;
    OK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool; command_info.commandBufferCount = 1;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer command;
    OK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    OK(vkCreateFence(device, &fence_info, nullptr, &fence));
    constexpr uint32_t data_bytes = 2 * 1024 * 1024, sentinel = 0xdeadf00d, grid_count = 65536;
    auto mirror = buffer(data_bytes), shadow = buffer(data_bytes), guest = buffer(data_bytes);
    auto table = buffer(grid_count * sizeof(Chunk)), result = buffer((grid_count + 2) * sizeof(uint32_t));
    const uint32_t offsets[] = {0, 65536, 131076, 131092, 131104};
    const uint32_t counts[] = {4096, 4096, 3, 2, 257}, vectors[] = {1, 1, 0, 0, 1};
    for (unsigned pass = 0; pass != 5; ++pass) {
        auto* m = static_cast<uint32_t*>(mirror.map);
        auto* s = static_cast<uint32_t*>(shadow.map);
        auto* g = static_cast<uint32_t*>(guest.map);
        auto* flags = static_cast<uint32_t*>(result.map);
        for (uint32_t i = 0; i < data_bytes / 4; ++i) m[i] = s[i] = g[i] = i * 2654435761u + 17u;
        if (pass < 2) { m[65536 / 4 + 65] ^= 1; m[131092 / 4] ^= 2; m[131104 / 4 + 1027] ^= 4; }
        std::fill(flags, flags + grid_count + 2, sentinel);
        auto* chunks = static_cast<Chunk*>(table.map);
        for (unsigned i = 0; i != 5; ++i) {
            flags[i] = sentinel;
            chunks[i] = {mirror.address + offsets[i], shadow.address + offsets[i], guest.address + offsets[i], counts[i], vectors[i]};
        }
        if (pass == 4) {
            for (uint32_t i = 0; i < grid_count; ++i)
                chunks[i] = {mirror.address + i * 16, shadow.address + i * 16, guest.address + i * 16, 1, 1};
            m[(grid_count - 1) * 4] ^= 8;
        }
        OK(vkResetCommandPool(device, pool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        OK(vkBeginCommandBuffer(command, &begin));
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const uint32_t count = pass == 3 ? 0 : pass == 4 ? grid_count : 5;
        const Push push{table.address, count, 0, pass ? result.address : 0};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        // The second row crosses the production 65535-wide grid boundary; tail groups must return uniformly.
        vkCmdDispatch(command, pass == 4 ? 65535 : 8, pass == 4 ? 2 : 1, 1);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
        OK(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        OK(vkResetFences(device, 1, &fence));
        OK(vkQueueSubmit(queue, 1, &submit, fence));
        OK(vkWaitForFences(device, 1, &fence, true, 10000000000ull));
        CHECK(std::memcmp(m, g, data_bytes) == 0 && std::memcmp(m, s, data_bytes) == 0);
        const uint32_t changed[] = {0, 1, 0, 1, 1};
        for (uint32_t i = 0; i < grid_count + 2; ++i) {
            const uint32_t expected = pass == 0 || i >= count ? sentinel :
                pass == 1 ? changed[i] : pass == 4 && i == grid_count - 1 ? 1u : 0u;
            CHECK(flags[i] == expected);
        }
        const char* labels[] = {"off", "mixed chunks", "reused clean result", "zero count", "65535-wide 2D grid"};
        std::printf("PASS: %s, guest/shadow parity and feedback guards\n", labels[pass]);
    }
    for (auto b : {mirror, shadow, guest, table, result}) release(b);
    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, shader, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr); vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts("PASS: authored GPU write-back observations");
}
