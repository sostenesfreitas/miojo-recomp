// (hookshot) The presenter thread's queue (vk_replay.cpp, RX_VK_PRESENT_THREAD): which family and queue index create_device asks for.
// A plain function over the queue families, so tests/presenter_queue_test.cpp checks it on NVIDIA's and RADV's layouts.
#pragma once

#include <cstdint>

namespace mkwvk {

/// One queue family as vkGetPhysicalDeviceQueueFamilyProperties gives it: its VkQueueFlags and its queueCount
struct QueueFamilyInfo {
    uint32_t flags;
    uint32_t count;
};
constexpr uint32_t kQueueGraphics = 0x1, kQueueCompute = 0x2;   // (VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_COMPUTE_BIT)

/// A queue no other thread uses: a second queue of the replay's family (`graphics`, whose queue 0 is the replay's) where it has one,
/// else a queue of a compute-only family that no other queue of the device takes (`compute_taken`: the family whose queue 0 the
/// presents' own queue, RX_VK_PRESENT_QUEUE, takes; UINT32_MAX for none), its next index. False: no queue free, the presents stay on
/// the game's thread. (Whether the family can present to the window is checked once the surface is made.)
inline bool presenter_queue_pick(const QueueFamilyInfo* families, uint32_t n, uint32_t graphics, uint32_t compute_taken,
                                 uint32_t& family, uint32_t& index) {
    if (graphics < n && families[graphics].count >= 2) {
        family = graphics, index = 1;
        return true;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (i == graphics || !(families[i].flags & kQueueCompute) || (families[i].flags & kQueueGraphics)) continue;
        const uint32_t used = i == compute_taken ? 1 : 0;
        if (families[i].count > used) {
            family = i, index = used;
            return true;
        }
    }
    return false;
}

}  // namespace mkwvk
