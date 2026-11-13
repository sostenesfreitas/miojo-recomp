// AMD Anti-Lag 2 through VK_AMD_anti_lag (vk_antilag.cpp): the driver delays the start of the game's next frame so that its
// input is sampled later and the frame waits less in the queue, for less delay from input to the screen.
#pragma once

#include <cstdint>
#include <vector>

#include "vk_internal.h"

namespace mkwvk {

/// The extension it needs (VK_AMD_anti_lag), added to `want` when the device has it
void antilag_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// Its feature (antiLag) into the device's feature chain (*tail), before the features are queried
void antilag_device_features(void**& tail);
/// After the device is made: whether Anti-Lag 2 can run (the feature there), its function loaded
void antilag_device_created();
/// Anti-Lag 2 can run on this device: the Insert menu's row offers it only then
bool antilag_available();
/// Before the present: the present stage of the frame it shows (antilag_present_frame), when it shows one
void antilag_present();

// With frame generation the game's frames are still marked at the game's present (the input stage), and the display
// thread shows them later, between made frames: each set keeps the game frame it was made at (antilag_frame), and only
// the set's game frame gives that frame's present stage, as NVIDIA Reflex's present markers (vk_reflex.h).
constexpr uint64_t kAntiLagThisFrame = ~0ull;
/// The frame the calling thread's next present shows: kAntiLagThisFrame (the default) the frame being made, as without
/// frame generation; frame generation's display thread sets a set's game frame for its game frame and 0 for a made frame
extern thread_local uint64_t antilag_present_frame;
/// The game frame whose input stage was given last (the one being made now), 0 while Anti-Lag 2 is off or none was:
/// frame generation keeps it with the set
uint64_t antilag_frame();
/// After the present: the next frame's input stage, where the driver waits until that frame should start
void antilag_input();

} // namespace mkwvk
