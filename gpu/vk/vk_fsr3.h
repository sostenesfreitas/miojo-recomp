// (hookshot) AMD FidelityFX Super Resolution 3.1 (the FSR 3.1.4 upscaler of FidelityFX SDK v1.1.4) in place of OoT's
// gameplay upscale network, the same place NVIDIA DLSS takes (vk_dlss.h). Built from AMD's open shader sources
// (third_party/fidelityfx-sdk, compiled into gpu/vk/shaders by scripts/fsr3_shaders.py): every pass is a dispatch of ours,
// and nothing of AMD's is linked or loaded at run time, so it is in every build and runs on any GPU with what supported()
// asks for.
//
// The passes (namespace fsr3) take colour, depth and motion at the render size and write the picture at the output size.
// Those inputs are made in vk_fsr3_inputs.cpp; fsr3_upscale below is the dispatch's side: the network's input in, FSR's
// picture squared back into the network's output, as vk_dlss.cpp's dlss_out.comp does for DLSS.
//
// RX_VK_FSR=0: never (the row stays), =1: FSR 3.1 over the Insert menu's Upscaler row. (hookshot) It draws on vk_temporal.cpp's
// chain by default (fsr3_engine: the chain's inputs, with the skinned draws' motion); RX_VK_FSR3_CHAIN=0: with its own inputs.
#pragma once

#include <cstdint>

#include "fsr3_math.h"
#include "vk_internal.h"

namespace mkwvk {

struct Texture;

namespace fsr3 {

/// One evaluation's inputs, in the SDK's meanings: the colour at the render size, the depth reversed, the motion toward the
/// frame before in render pixels without the jitter, and the jitter in render pixels (input texel k is sampled at
/// k + 0.5 - jitter)
struct Inputs {
    VkImageView color = VK_NULL_HANDLE;
    VkImageView depth = VK_NULL_HANDLE;
    VkImageView motion = VK_NULL_HANDLE;
    /// The reactive mask, or null for none: R8 at the render size, 0..1, how much of the pixel was blended over the scene
    /// without depth or motion of its own (vk_reactive.h)
    VkImageView reactive = VK_NULL_HANDLE;
    uint32_t render_width = 0, render_height = 0;
    uint32_t upscale_width = 0, upscale_height = 0;
    float jitter[2]{};
    uint32_t jitter_phases = 8;   ///< the length of the jitter's sequence (dlss_jitter_phases: the host's own)
    /// Turns a motion vector into render pixels (1 for vectors already in them)
    float motion_scale[2]{1.0f, 1.0f};
    fsr3_math::Camera camera;     ///< device_to_view_depth (fsr3_math.h)
    float frame_time_ms = 16.667f;
    bool reset = false;
};

/// Whether this device can run the passes: push descriptors enough for the widest pass, storage images in every format the
/// SDK's internal images use, the extended storage formats and storage writes without a format (the accumulate and RCAS
/// passes' output). Without subgroup quad operations in compute the two pyramids run as their wave-less variants. Asked
/// once; the reason it cannot is logged once
[[nodiscard]] bool supported();

/// (hookshot) The pipelines and the internal images for these sizes, made again when they change (upscale does the same
/// first): 1 ready as they were, 2 made just now (a new history), 0 an image could not be made (try again later), -1 the
/// passes cannot run here (for good). vk_fsr3.cpp's engine of the temporal chain calls it from its prepare
[[nodiscard]] int prepare_sizes(uint32_t render_width, uint32_t render_height, uint32_t upscale_width, uint32_t upscale_height);

/// Records one evaluation into output_image(), making the pipelines and the internal images at these sizes first (again
/// when they change). False when something could not be made or the inputs are not usable
[[nodiscard]] bool upscale(VkCommandBuffer cmd, const Inputs& in);

/// (hookshot) Lets the internal images go (freed once the GPU's uses of them are done); the next prepare_sizes or upscale makes
/// them again. vk_fsr3.cpp's engine of the temporal chain calls it when the chain stops drawing with FSR 3.1
void release();
/// (hookshot) The video memory its internal images hold now, in bytes (the chain's status line: vk_fsr3.cpp chain_memory)
[[nodiscard]] uint64_t memory();

/// The image an evaluation resolves into: RGBA16F at the upscale size, or null before the first
[[nodiscard]] VkImage output_image();
[[nodiscard]] VkImageView output_view();

} // namespace fsr3

/// FSR 3.1 is chosen: the Insert menu's Upscaler row says FSR 3.1 (upscaler_rows.h FSR3) and neither RX_VK_DLSS nor
/// RX_VK_FSR4 is set over it (vk_temporal.h temporal_env_chosen), or RX_VK_FSR=1; and it runs here
bool fsr3_chosen();
/// (hookshot) FSR 3.1 draws on vk_temporal.cpp's chain, as an engine of it (fsr3_engine): the chain's inputs, the skinned
/// draws' own motion in them. With its own inputs (vk_fsr3_inputs.cpp) the motion was the camera's alone, and what moves with
/// the camera, Link above all, smeared while he walked. RX_VK_FSR3_CHAIN=0: its own inputs, as before
bool fsr3_on_chain();
/// FSR 3.1 upscales with its own inputs (fsr3_upscale below): chosen, and not on the chain
bool fsr3_wanted();
struct TemporalEngine;
/// FSR 3.1 as an engine of the temporal chain (vk_temporal.h)
const TemporalEngine* fsr3_engine();

/// A gameplay upscale's dispatch, as vk_temporal.h's temporal_upscale takes it (vk_draw.cpp's fsr3_stand_in finds them the same
/// way). With FSR 3.1's frame interpolation on, the dispatch's input and this frame's camera become its inputs here
/// (vk_fsr3_inputs.h) whichever upscaler runs. true: FSR 3.1 wrote the output (the dispatch is left out); false: the network
/// (or DLSS) runs, the reason logged when it changes
bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                  VkImageView out_view, uint32_t out_level);

} // namespace mkwvk
