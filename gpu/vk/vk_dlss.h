// NVIDIA DLSS Super Resolution, one of the temporal upscaler's engines (vk_temporal.h; vk_dlss.cpp). Built in only with
// NVIDIA's NGX SDK (HOOKSHOT_NGX, scripts/ngx_sdk.py: Linux and Windows); without it these say DLSS is not there and the
// game's network runs.
#pragma once

#include <vector>

#include "vk_internal.h"

namespace mkwvk {

struct TemporalEngine;

/// Whether this build has DLSS in it (HOOKSHOT_NGX)
bool dlss_built();
/// Why DLSS cannot be chosen here (the Insert menu's Upscaler row says it), DLSS_OK when it can or is not known yet
enum DlssWhy { DLSS_OK, DLSS_NOT_BUILT, DLSS_ENV_OFF, DLSS_NEEDS_RTX, DLSS_NEEDS_DRIVER, DLSS_NO_RUNTIME,
               DLSS_RUNTIME_NOT_ASCII, DLSS_WHY_COUNT };
DlssWhy dlss_why();
/// Asks NGX once whether DLSS runs on this device, so dlss_why knows before the first upscale (the menu, when it opens).
/// Nothing off an NVIDIA device with NGX's extensions, or in a build without DLSS
void dlss_probe();
/// A feature that was not made is asked for again at the next upscale (vk_dlss.cpp prepare's `refused`): the player
/// changed a row the feature is made from (vk_temporal.cpp temporal_reset_failure, with the chain's lock held, as prepare
/// has it)
void dlss_retry();
/// The instance's extensions DLSS needs, added to `want` when the loader has them (all: what it has)
void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// The device's extensions DLSS needs, added to `want` when the device has them
void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// DLSS as the temporal upscaler's engine (DLSS and DLAA on the Upscaler row); nullptr in a build without it
const TemporalEngine* dlss_engine();

// NVIDIA DLSS Frame Generation through NGX (vk_dlss.cpp): the renderer's frame generation (vk_framegen.cpp) shows and paces
// the frames DLSS-G makes from the temporal chain's depth, motion and camera. Asked for with RX_VK_DLSS_FG=1 for a run.
struct TemporalFrame;
/// Whether it is asked for now: the Insert menu's Frame generation by row (Auto), RX_VK_DLSS_FG=0 / 1 over it (and the build
/// has it)
bool dlss_fg_wanted();
/// (hookshot) Why the last set it was asked for went to the renderer's own frame generation, for the Insert menu's Frame
/// generation by row (its texts are the menu's, to be translated): FG_SKIP_NONE when DLSS-G made it or nothing is known.
/// most: its runtime's most made frames a set (FG_SKIP_TOO_MANY)
enum FgSkip {
    FG_SKIP_NONE,
    FG_SKIP_OFF,        // (RX_VK_DLSS_FG=0 for the run)
    FG_SKIP_DEVICE,     // (DLSS does not run on this GPU)
    FG_SKIP_START,      // (NGX did not start it: no runtime, an older GPU)
    FG_SKIP_TOO_MANY,   // (more made frames than its runtime makes)
    FG_SKIP_INPUTS,     // (no picture from a temporal Upscaler this frame)
    FG_SKIP_REFUSED,    // (NGX refused the set)
    FG_SKIP_NOT_BUILT,  // (a build without NVIDIA's SDK)
    FG_SKIP_COUNT
};
int dlss_fg_skip(unsigned* most = nullptr);
/// Whether it makes the sets of `frames` made frames (the multiplier less 1) as far as is known: asked for, on a device DLSS
/// runs on, NGX's answer not a no, its feature not given up and its runtime making that many (an older one makes 1 at the
/// most, so 3x and 4x are the renderer's own). The swapchain's present mode follows it: vk_main.cpp swapchain_dlssg
bool dlss_fg_runs(int frames);
/// Frame generation's device extensions, asked of NGX, added to `want` when the device has them
void dlss_fg_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// The chain's frame (vk_temporal.cpp, after its engine's evaluation): its depth and motion copied, its camera kept, for the
/// next present. projection and view_projection: the scene's, jittered by clip_jitter; motion: the chain had the frame before's
void dlss_fg_inputs(VkCommandBuffer cmd, const TemporalFrame& f, const float projection[16], const float view_projection[16],
                    const float clip_jitter[2], bool motion);
/// At the present (vk_framegen.cpp framegen_record): `count` frames between the last picture and this one (color, the
/// window's, w x h RGBA8) into out[0..count). Returns count, or 0 when it did not make them (the renderer's own methods do).
/// blend: there is a picture before this one; sequence: the present's count (one skipped makes a new history)
int dlss_fg_frames(VkCommandBuffer cmd, VkImage color, VkImageView color_view, uint32_t w, uint32_t h, int count,
                   const VkImage* out, const VkImageView* out_views, bool blend, uint64_t sequence);
/// Frame generation at Off or Auto (no fixed multiplier: dlss_fg_frames is not asked): its feature, if made, let go and its
/// memory back, as when the Frame generation by row changes. Each such present (vk_replay.cpp); nothing to do is one load
void dlss_fg_idle();
/// Before the process exits (mkwvk_close_asked): its feature let go with the queue idle
void dlss_fg_close();

} // namespace mkwvk
