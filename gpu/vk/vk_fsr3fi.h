// (hookshot) AMD FSR 3.1 frame interpolation, and the optical flow it runs on, recorded as our own Vulkan compute passes:
// the made frame between two of the game's when the Frame generation row is 2x.
//
// The renderer already makes frames: vk_framegen.cpp interpolates between the last two presented frames from colour alone.
// This answers the same question with AMD's passes, which use the scene's depth and motion as well (vk_fsr3_inputs.h):
// seventeen compute passes -- seven of optical flow over two frames' luma, ten of interpolation over the game's motion and
// depth -- all of them our own dispatches over AMD's vendored GLSL, with no FidelityFX host library linked and nothing of
// AMD's loaded at run time.
//
// Where it cannot run -- not asked for, a device without the storage formats, image features or subgroup operations the
// passes need, a frame without its inputs, a reset -- the renderer's own interpolation runs exactly as before. Nothing here
// changes what the game submits.
//
// FSR 3.1's interpolation has no phase input (ffx_frameinterpolation.h fixes t at 0.5): it makes the midpoint and only the
// midpoint, so it is used for one made frame a frame (2x). At 3x and above the renderer's own makes the frames, as more
// copies of one midpoint would show the same picture twice and judder.
#pragma once

#include <cstdint>

#include "fsr3_math.h"
#include "vk_internal.h"

namespace mkwvk::fsr3fi {

/// One made frame's inputs.
///
/// The colour is the frame the game has just finished, at the size it is presented, and it is the only colour this backend
/// is given: the frame before it is kept here, because the interpolation needs both ends of the interval and the present
/// path only ever hands over the newest one. The depth (reversed) and the motion are the scene's at the render size
/// (vk_fsr3_inputs.h), with `motion_scale` taking a vector to render pixels, as the SDK's
/// FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS permutation the vendored GLSL was compiled under expects.
struct Inputs {
    VkImageView color = VK_NULL_HANDLE;
    VkImage color_image = VK_NULL_HANDLE;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;

    VkImageView depth = VK_NULL_HANDLE;
    VkImageView motion = VK_NULL_HANDLE;
    uint32_t render_width = 0, render_height = 0;

    /// Turns a motion vector into render pixels (1 for vectors already in them).
    float motion_scale[2]{1.0f, 1.0f};
    float jitter[2]{};

    /// The scene's camera: the device-to-view-depth factors the disocclusion passes compare depths with, and the tangent
    /// of half the field of view (fsr3_math.h, from the scene's projection).
    fsr3_math::Camera camera;
    float frame_time_ms = 16.667f;

    /// A cut, a load or a resize: the preparation passes are skipped for this frame, as the SDK's own dispatch skips them,
    /// because every history they read belongs to a scene that is gone.
    bool reset = false;

    /// The caller's count of its frames (vk_framegen.cpp: one a present). The frame before this one is kept here as the
    /// interval's start; when the last frame given was not the one before this (a present this backend did not see), that
    /// start is not this frame's predecessor and the call is a reset.
    uint64_t sequence = 0;
};

/// Whether this device can run the passes: push descriptors, every storage format the SDK's internal images need, and the
/// image features and subgroup operations the vendored passes compiled down to. Asked once; the one line saying why it
/// cannot is logged once.
[[nodiscard]] bool capable();

/// Whether a made frame is asked for: the Frame generation row at 2x (RX_VK_FSR_FG=0 never, =1 over the row), on a device
/// that is capable(). Read every frame: the row changes while the game runs.
[[nodiscard]] uint32_t frames_wanted();

/// Records the made frame between the frame before and this one into made_image(), making the pipelines and the images for
/// these sizes first (again when the sizes or the presented format change). False when it cannot (the inputs not usable, a
/// reset frame, anything not made), which leaves the caller to show whatever it would otherwise have shown.
[[nodiscard]] bool generate(VkCommandBuffer cmd, const Inputs& in);

/// The made frame of the last `generate`, or null.
[[nodiscard]] VkImage made_image();

/// Forgets the sequence, so the next frame is made with the reset flag set: a cut, a load, a resize.
void reset();

} // namespace mkwvk::fsr3fi
