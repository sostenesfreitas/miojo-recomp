// (miojo) The scene as ray-tracing acceleration structures (vk_rt.cpp): the first step of path tracing. Off unless the menu's
// Ray tracing row is on (ray_tracing=1 in hookshot_options.ini, read at the start; RX_VK_RT=0 / 1 over it) and the
// device has VK_KHR_acceleration_structure and VK_KHR_ray_query.
#pragma once

#include <vector>

#include "vk_internal.h"
#include "vk_temporal.h"

namespace mkwvk {

void rt_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
void rt_device_features(void**& tail);
void rt_device_created();
/// Asked for and the device can
bool rt_on();

/// An opaque indexed triangle-list draw into the scene, not skinned: its mesh where the game keeps it, and its object's
/// buffer (vertex uniform buffer 1: a 3 x 4 row-major world matrix, an origin added at 0x40)
struct RtDraw {
    uint64_t vbuf = 0, vsize = 0;   // (vertex stream 0)
    uint32_t stride = 0, pos = 0;   // (the position's byte offset in a vertex)
    bool half = false;              // (the position: RGBA16F, else RGB32F)
    uint64_t ibuf = 0;              // (the first index's address)
    int itype = 0, count = 0, base_vertex = 0;
    uint64_t object = 0;
    uint64_t tex[4]{};              // (the fragment stage's first texture handles: the colour texture is among them)
};
void rt_note_draw(const RtDraw& d, bool shadow_pass = false);
/// A scene draw's fragment uniform buffer 4, the game's lights: the sun's direction at 0 and its colour at 0x40 (read once a
/// frame, and taken only when its direction is the shadow maps')
void rt_note_lights(uint64_t address, uint64_t size);
/// A draw into a depth-only target with the vertex stage's camera at `camera`: when that is one of the game's sun shadow
/// maps (an orthographic camera), the sun's direction is taken from it and the draw's mesh (k, or null when the rays
/// cannot have it) is an instance for the rays, in view or not. True: the Sun shadows row is on and the rays have this
/// draw (k, or `skinned`), so the game's shadow map is to go without it
bool rt_note_shadow_draw(const RtDraw* k, bool skinned, uint64_t camera, uint64_t camera_size);
/// A skinned draw into the scene (vk_draw.cpp's, as the upscaler's actor motion takes them): skinned on the CPU with this
/// frame's bones, all of a frame's into one structure built anew
void rt_note_skinned(const SkinnedDraw& k);

/// Once a frame, from the temporal upscaler's passes (outside a render pass): the new meshes' structures built, the frame's
/// instances into the top-level one, and with RX_VK_RT_DEBUG=1 or 2 the check drawn over `color` (rgba16f, w x h, GENERAL);
/// else, ao > 0 or gi > 0, the ray-traced ambient occlusion and one bounce of light over it (their strengths to 1; seed: a
/// number that changes each frame; scene: the scene's colour as the upscaler samples it, where the bounce's light is read).
/// view_proj: the camera's, row major; depth: the scene's depth as a sampled view
void rt_frame(VkCommandBuffer cmd, const float view_proj[16], VkImageView color, VkImageView depth, VkSampler point,
              uint32_t w, uint32_t h, float ao, float seed, VkImageView scene, float gi);

} // namespace mkwvk
