// (hookshot) A reactive mask for the temporal upscalers, NVIDIA DLSS (vk_dlss.cpp) and AMD FSR 3.1 (vk_fsr3.cpp), not yet
// AMD FSR 4 (vk_fsr4.cpp takes none: it runs as it does without RX_VK_REACTIVE): where the scene's transparent draws
// (water, smoke, fire, glows) changed it. They write no depth and have no motion of their own,
// so an upscaler takes them for the surface behind and smears or ghosts them; where the mask is set it leans on this frame
// and not on its history.
//
// The game does not say which draws they are. vk_draw.cpp's draw tells them by their state (reactive_policy.h) and has the
// scene's colour copied before them (copy_scene); at the upscale, what the scene's target holds now is compared with
// that copy (shaders/reactive_mask.comp), and the difference is the mask.
//
// Off unless RX_VK_REACTIVE=1: which draws are the transparent ones is a guess from their state, and it is not yet checked
// in the game. RX_VK_REACTIVE_SCALE (1) multiplies the mask, RX_VK_REACTIVE_MAX (0.9) limits it, RX_VK_REACTIVE_DEBUG=1
// shows it in the picture (magenta where it is set).
#pragma once

#include <cstdint>

#include "vk_internal.h"

namespace mkwvk {

struct Texture;

namespace reactive {

/// A frame's mask: R8_UNORM, of the upscaler's inputs' size, set in its top left width x height
struct Mask {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t image_width = 0, image_height = 0;
};

/// RX_VK_REACTIVE=1, and the device can make the mask
bool wanted();

/// Before a transparent draw into the scene's colour target (reactive_policy.h says which): its level's colour as it is
/// now, copied aside. Called outside a pass. False when it is not a target this can copy (the reason is logged once)
bool copy_scene(VkCommandBuffer cmd, Texture* color, uint32_t level, uint32_t layer, uint32_t width, uint32_t height);

/// At the upscale: the mask of this frame's copy against the scene's target as it is now, width x height in an image of
/// image_width x image_height (DLSS's inputs are larger than the scene). `color` is the upscaler's RGBA16F colour input,
/// a storage image of at least that size, written only by RX_VK_REACTIVE_DEBUG. False when there is no mask this frame:
/// nothing transparent was drawn, or the scene's target is not the one copied (the upscaler then runs without one)
bool mask(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t image_width, uint32_t image_height,
          VkImageView color, Mask& out);

} // namespace reactive
} // namespace mkwvk
