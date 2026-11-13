// (the target image pool's timing, cut from vk_texture.cpp by test_target_image_reuse.py: a released render target's image,
// the GPU done with it, is the pool's as it is evicted, so a target of the same create info made in the next frame takes
// it; the eviction skips those in flight, so destroy_later is only handed images the GPU is done with)
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

// ---- the fixture's stubs: the GPU, the allocator, and the clock ----
using VmaAllocation = void*;
struct Device {
    VkDevice device = reinterpret_cast<VkDevice>(1);
};
Device dev;
uint64_t frame_no = 0, total_bytes = 0, destroy_gen = 0;
int destroyed_images = 0, destroyed_views = 0;
void destroy_image(VkImage, VmaAllocation) {
    destroyed_images++;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {
    destroyed_views++;
}
// (the real Texture is larger; destroy_later reads these)
struct Texture {
    uint64_t last_value = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::vector<VkDeviceMemory> sparse_chunks;
    std::unordered_map<uint64_t, VkImageView> views;
    uint64_t pool_key = 0;
    uint64_t bytes = 0, pad = 0;
    struct BlockAttachment {
        VkImage image;
        VmaAllocation allocation;
        VkFormat format;
        uint32_t level;
    };
    std::vector<BlockAttachment> block_attachments;
};

/* DEAD */
std::deque<Dead> dead;

/* ACTUAL */

static int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            fails++;                                                      \
        }                                                                 \
    } while (0)

static uint64_t key_for(uint32_t w, uint32_t h) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return pool_key_of(ci);
}

static Texture target(uint64_t key, VkImage image, VmaAllocation allocation, VkImageView view, uint64_t bytes) {
    Texture t;
    t.pool_key = key;
    t.image = image;
    t.allocation = allocation;
    t.views[1] = view;
    t.bytes = bytes;
    t.pad = 4096;
    return t;
}

int main() {
    {   // a released target the GPU is done with: the next frame's target of its shape takes its image, not a new one
        const uint64_t key = key_for(3840, 2160);
        CHECK(key == key_for(3840, 2160));   // (the same create info, the same key: same size and format)
        CHECK(key != key_for(2560, 1440));
        const VkImage image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(0x1000));
        const VmaAllocation allocation = reinterpret_cast<VmaAllocation>(static_cast<uintptr_t>(0x2000));
        const VkImageView view = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0x3000));
        Texture t = target(key, image, allocation, view, 64ull << 20);
        total_bytes = t.bytes + t.pad;
        destroy_later(t);
        CHECK(dead.empty());   // (it did not wait for the next present)
        CHECK(pool_n == 1);
        CHECK(pool_trim() == 0);   // (nor does the present's trim take it: it was just pooled)
        CHECK(pool_n == 1);
        VkImage taken_image = VK_NULL_HANDLE;
        VmaAllocation taken_allocation = nullptr;
        std::unordered_map<uint64_t, VkImageView> taken_views;
        CHECK(pool_take(key, taken_image, taken_allocation, taken_views));
        CHECK(taken_image == image);
        CHECK(taken_allocation == allocation);
        CHECK(taken_views.size() == 1 && taken_views.begin()->second == view);
        CHECK(st_pool_reused == 1 && st_pool_destroyed == 0);
        CHECK(destroyed_images == 0 && destroyed_views == 0);
        CHECK(pool_n == 0 && pool_bytes == 0);
        CHECK(total_bytes == 0);
    }
    std::printf("target image reuse: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
