// NVIDIA Reflex through VK_NV_low_latency2 (vk_reflex.cpp): the driver paces the game's frames so that the CPU starts one
// as late as it can and the frame waits less in the queue, for less delay from input to the screen.
#pragma once

#include <vector>

#include "vk_internal.h"

namespace mkwvk {

/// The extensions it needs (VK_NV_low_latency2, VK_KHR_present_id), added to `want` when the device has them
void reflex_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// Its feature (presentId) into the device's feature chain (*tail), before the features are queried
void reflex_device_features(void**& tail);
/// After the device is made: whether Reflex can run (the feature there), its functions loaded
void reflex_device_created();
/// Reflex can run on this device (VK_NV_low_latency2 and its functions): the Insert menu's row offers it only then
bool reflex_available();
/// Into a swapchain's creation: latency mode on while the row is on, nothing while it is off (the chained struct lives until
/// the next call)
void reflex_swapchain_info(VkSwapchainCreateInfoKHR& ci);
/// The row was turned on or off since the swapchain was made: it is to be made again (present_image)
bool reflex_swapchain_stale();
/// A swapchain made: its sleep mode set as the menu has it
void reflex_swapchain_created(VkSwapchainKHR swapchain);
/// Before destroying the current swapchain, including a failed replacement: stop using it for latency calls
void reflex_swapchain_destroyed();
/// The frame's markers, at the game's present: its simulation ended and its rendering's submission starts
void reflex_present_begins();
/// Before the present's submit: the rendering's submission ended
void reflex_submit_ends();
/// The present: a VkPresentIdKHR for VkPresentInfoKHR's pNext (or null), and the present's start marker
const void* reflex_present_id(const void* next);
/// Its present-id feature struct when its extensions put one in the device's feature chain (null: none; the presenter thread's
/// present waits then add their own, vk_main.cpp create_device)
const VkPhysicalDevicePresentIdFeaturesKHR* reflex_present_id_features();
void reflex_presented();
/// After the present: the driver's sleep until the next frame should start, then that frame's start markers
void reflex_sleep();

// With frame generation the game's frames are still marked and paced (the sleep, the simulation and submission markers, at
// the game's present), and the display thread shows them later, between made frames: each set keeps the game frame it was
// made at (reflex_frame), and only the set's game frame is presented with that present id and the present markers.
constexpr uint64_t kReflexThisFrame = ~0ull;
/// The Reflex frame the calling thread's next present shows: kReflexThisFrame (the default) the frame being made, as without
/// frame generation; frame generation's display thread sets a set's game frame for its game frame and 0 for a made frame
/// (no present id, no present markers)
extern thread_local uint64_t reflex_present_frame;
/// The game frame Reflex is marking now (its present id), 0 when Reflex is not on: frame generation keeps it with the set
uint64_t reflex_frame();
/// Frame generation: the game frame's rendering submitted (do_present, the set queued): its submission's end
void reflex_game_submitted();
/// A queue submit's VkLatencySubmissionPresentIdNV, chained in front of `next` for VkSubmitInfo2's pNext: the game frame its
/// GPU work belongs to, so the driver times that frame's rendering (its GPU end) from it. frame: kReflexThisFrame the frame
/// being made (the game's rendering); a frame reflex_frame gave (a batch held since, frame generation's set); 0 none (a made
/// frame's copy). `next` itself when Reflex is off, the frame is not marked or was presented already, or
/// RX_VK_REFLEX_SUBMIT_IDS=0 (or false, off, no). The struct is the calling thread's until its next call: take it right before the submit
const void* reflex_submit_id(const void* next, uint64_t frame = kReflexThisFrame);

} // namespace mkwvk
