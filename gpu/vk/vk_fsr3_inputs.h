// (hookshot) AMD FSR 3.1's inputs (vk_fsr3_inputs.cpp): everything FSR 3.1's upscaler (vk_fsr3.h) and its frame
// interpolation (vk_fsr3fi.h) are given besides the picture, in one place, so they can move onto the renderer's shared
// temporal inputs (#1008) by changing this file alone.
//
// What comes from #841 (NVIDIA DLSS's own inputs, now vk_temporal.cpp: the frame chain DLSS and FSR 4 share) and what does
// not yet:
//  - the camera jitter: #841's, the one jitter in the game (nx2/oot_jitter.py, host/rx_backend.c hos_dlss_jitter_clip),
//    switched on for FSR by jitter_size() as vk_temporal.cpp's temporal_jitter_size switches it on for DLSS and FSR 4, and
//    read back here from the camera's uniform buffer as vk_temporal.cpp reads it;
//  - the camera: the same uniform buffer and the same checks as vk_temporal.cpp's temporal_note_scene_draw; the near plane and the
//    field of view are read from its projection (fsr3_math.h), so nothing in the game is hooked for them;
//  - the motion: the camera's, from the scene depth with dlss_prep.comp's math (shaders/fsr3_prep.comp). #841's actor motion
//    (the skinned draws drawn again with last frame's bones) is in vk_temporal.cpp, in every build since #1008; FSR 3.1
//    gets it when these inputs move onto that chain.
#pragma once

#include <cstdint>

#include "fsr3_math.h"
#include "vk_internal.h"

namespace mkwvk {

struct Texture;

namespace fsr3_inputs {

/// A frame's inputs, at the render size, in images of this file's own (written by prepare)
struct Frame {
    VkImageView color = VK_NULL_HANDLE;    ///< RGBA16F: the network's input as it samples it, 0..1
    VkImageView depth = VK_NULL_HANDLE;    ///< R32F, reversed (fsr3_math.h Camera::flip_depth)
    VkImageView motion = VK_NULL_HANDLE;   ///< RG16F, render pixels toward the frame before, x right and y down, no jitter
    uint32_t width = 0, height = 0;
    float jitter[2]{};                      ///< this frame's, in render pixels as FSR takes it (x right, y down)
    fsr3_math::Camera camera;
    bool reset = false;                     ///< no history: the first frame, a new size, or a gap of more than two frames
    float frame_ms = 16.667f;               ///< since the frame before's
    uint64_t frame = 0;                     ///< texture_frame_no() when prepared
};

/// FSR 3.1 is asked for at all: its upscaler (the Upscaler row) or its frame interpolation (the Frame generation row). The
/// scene's camera is kept only then
bool wanted();

/// Matching scene/network formats for the independent input path. RGBA16F is limited to
/// opt-in Native PsychoV35 frame interpolation; it does not enable the legacy upscaler.
bool scene_format(VkFormat format);
bool colour_pair(VkFormat input, VkFormat output);

/// A draw into a scene-sized target with the vertex stage's uniform buffer 3 at `camera`: what vk_temporal.cpp's
/// temporal_note_scene_draw is given, read the same way (the largest such target of the frame is the scene)
void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                     uint64_t camera_size);

/// The texture is this frame's scene colour (the size of the scene's camera this frame): for a network of the gameplay
/// upscale's shape that is not one of the known ones, as vk_temporal.cpp's temporal_upscale tells them apart
bool is_scene(const Texture* in);

/// At the gameplay upscale's dispatch: its input as the network samples it (in, in_view), this frame's camera and depth into
/// FSR's inputs (shaders/fsr3_prep.comp). False when they cannot be made (no camera this frame, another size, ...): the
/// reason is logged when it changes
bool prepare(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Frame& out);

/// The frame prepare made last, for frame interpolation at the present, once: false when none was made since the last take
/// (or it is older than the frame before), so a present never interpolates with another frame's depth and motion
bool take(Frame& out);

/// FSR 3.1's upscaler drew this frame, rw x rh into dw x dh: the host's jitter follows FSR (jitter_size)
void upscaled(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh);

/// The scene's width (axis 0) or height (1) for the host's camera jitter while FSR 3.1's upscaler draws, 0 otherwise or
/// with RX_VK_FSR_JITTER=0; axis 2: the jitter sequence's length (dlss_jitter_phases). vk_replay.cpp's rx_option gives
/// the host this or vk_temporal.cpp's temporal_jitter_size, whichever upscaler draws
uint32_t jitter_size(int axis);
MkwVkJitter jitter_sample();

} // namespace fsr3_inputs
} // namespace mkwvk
