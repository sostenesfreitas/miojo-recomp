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
    // (a terrain patch: a grid of two-float vertices its vertex stage places and lifts by a height map, rt_terrain.comp.
    // patch and ground: the vertex stage's uniform buffers 5 and 4, the patch's own and the terrain's; heights: the map's
    // view, good while no texture was destroyed since heights_gen. tp: what rt_note_draw reads of the two buffers)
    bool terrain = false;
    bool strip = false;             // (a triangle strip, as the terrain's patches are drawn; restart: the index that ends a run, when on)
    bool restart_on = false;
    uint32_t restart = 0;
    uint64_t patch = 0, ground = 0;
    VkImageView heights = VK_NULL_HANDLE;
    uint64_t heights_gen = 0;
    float tp[10]{};
    // (a mesh-shader draw's mesh, as OoT packs its buildings and rocks: vbuf / vsize the packed mesh (the task stage's
    // storage buffer 0), lod the level the task stage picks; vk_rt.cpp decode_meshlets)
    bool meshlets = false;
    uint32_t lod = 0;
    bool front_ccw = false;         // (the draw's front face: the rays pass a triangle's back)
    uint64_t tex[4]{};              // (tex[0]: the colour texture's handle, from the lit pass; rt_note_material)
};
void rt_note_draw(const RtDraw& d, bool shadow_pass = false);
/// The same draw in the scene's lit pass, d.tex[0] its colour texture's handle: the mesh's material, looked for once
void rt_note_material(const RtDraw& d);
/// A scene draw's fragment uniform buffer 4, the game's lights: the sun's direction at 0 and its colour at 0x40 (read once a
/// frame, and taken only when its direction is the shadow maps')
void rt_note_lights(uint64_t address, uint64_t size);
/// A lit draw's fragment storage buffer 1 (98368 bytes: the game's lights, 96 bytes each) and uniform buffer 3 (its
/// exposure at 0x380), for the Light shadows row; once a frame
void rt_note_local_lights(uint64_t lights, uint64_t frame_buffer);
/// A draw into a depth-only target with the vertex stage's camera at `camera`: when that is one of the game's sun shadow
/// maps (an orthographic camera), the sun's direction is taken from it and the draw's mesh (k, or null when the rays
/// cannot have it) is an instance for the rays, in view or not. True: the Sun shadows row is on and the rays have this
/// draw (k, or `skinned`), so the game's shadow map is to go without it. cutout: its fragment stage samples a texture (a
/// leaf's card, cut by its alpha): the rays, which have no textures, would cast the whole card's shadow, so the object at
/// `object` (its buffer) is kept out of the rays this frame and its shadow left to the game's map
/// The object whose buffer is at `object` is a cutout (a card cut by its texture's alpha: a grate, a leaf, a banner's
/// fringe): kept out of the rays, which have no textures and would take the whole card as solid, for 240 frames
void rt_note_cutout(uint64_t object, uint64_t vbuf);
/// The object whose buffer is at `object` is drawn two-sided: kept for 240 frames, its instance one a ray starting on it passes
void rt_note_two_sided(uint64_t object, uint64_t vbuf);
bool rt_note_shadow_draw(const RtDraw* k, bool skinned, uint64_t camera, uint64_t camera_size, bool cutout, uint64_t object,
                         uint64_t vbuf);
/// A skinned draw into the scene (vk_draw.cpp's, as the upscaler's actor motion takes them): skinned on the CPU with this
/// frame's bones, all of a frame's into one structure built anew
void rt_note_skinned(const SkinnedDraw& k);

/// Once a frame, from the temporal upscaler's passes (outside a render pass): the new meshes' structures built, the frame's
/// instances into the top-level one, and with RX_VK_RT_DEBUG=1 or 2 the check drawn over `color` (rgba16f, w x h, GENERAL);
/// else, ao > 0 or gi > 0, the ray-traced ambient occlusion and one bounce of light over it (their strengths to 1; seed: a
/// number that changes each frame; scene: the scene's colour as the upscaler samples it, where the bounce's light is read).
/// view_proj: the camera's, row major; depth: the scene's depth as a sampled view; depth_kept: a copy of it that stays as it
/// is until the next frame's call (the upscaler's own float image of it), for rt_ao_replace at the start of the next frame
void rt_frame(VkCommandBuffer cmd, const float view_proj[16], VkImageView color, VkImageView depth, VkSampler point,
              uint32_t w, uint32_t h, float ao, float seed, VkImageView scene, float gi, VkImageView depth_kept);

/// The game's own ambient occlusion pass (a compute dispatch at the start of a frame, from the frame before's depth, into
/// an R8 image w x h that the game then blurs and lights with): written from rays instead, into `out`. True: done, the
/// game's dispatch is to be left out. False (Ray tracing off, the Ambient occlusion row at 0, no scene of the frame before
/// yet): the game's own runs
bool rt_ao_replace(VkCommandBuffer cmd, VkImageView out, uint32_t w, uint32_t h);

/// The game's shadow mask (a screen-size RGBA8 target without depth, its green what of the sun reaches a pixel: drawn
/// from the shadow maps before the scene is lit) was drawn into: its view and size, the largest of the frame kept
void rt_note_mask(VkImageView view, uint32_t w, uint32_t h);
/// The scene's camera (the vertex stage's uniform buffer 0 of a scene draw: its view-projection at 0x30), once a frame
void rt_note_camera(uint64_t address, uint64_t size);
/// Whether rt_sun_shadows has something to do now: the Sun shadows row on, a mask drawn this frame and not traced yet
bool rt_sun_pending();
/// The sun's shadows traced into this frame's mask (outside a render pass), from this frame's depth (`depth`: the scene's,
/// as the draw about to light it has it) and the structure of the frame before
void rt_sun_shadows(VkCommandBuffer cmd, VkImageView depth);

} // namespace mkwvk
