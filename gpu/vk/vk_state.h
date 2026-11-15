// The NVN state a replay keeps (as the WebGPU renderer's NvnState, gpu/render/rx_internal.h, without its WebGPU
// parts), the texture and sampler registrations, and the driver's constant banks programs read.
#pragma once

#include <cstdint>

#include "vk_api.h"

namespace mkwvk {

/// NVN's stages: vertex 0, fragment 1, geometry 2, tessellation 3 / 4, compute 5; NVN2's task 6 and mesh 7
constexpr int kNvnStages = 8;

struct NvnState {
    nvn_prog prog{};
    bool has_prog = false;
    nvn_p_bind_buf ubo[kNvnStages][16]{}, ssbo[kNvnStages][16]{};
    uint64_t tex[kNvnStages][128]{}, samp[kNvnStages][128]{}, img[kNvnStages][32]{};
    nvn_blend_st blend[8]{};
    nvn_color_st color{};
    nvn_chmask_st chmask{0xffffffffu};
    nvn_ms_st ms{};
    nvn_poly_st poly{};
    nvn_ds_st ds{};
    uint32_t vattr[16]{};
    int nvattr = 0;
    nvn_vs_st vstream[16]{};
    NVNbufferRange vbuf[16]{};
    nvn_p_set_rt rt{};
    uint64_t rt_serial = 0;      // bumped at each NOP_SET_RT (the draw path resolves the targets once per change)
    uint64_t bind_gen = 0;       // bumped when a binding the descriptors read changes (a rebind of the same values does not)
    // (hookshot) what bind_gen was when a stage's textures, samplers or images (tex_gen) or its storage buffers (ssbo_gen) last changed: the
    // driver banks c2 and c0 of a graphics stage are made from those alone, so a draw after another uniform buffer's bind keeps its banks
    uint64_t tex_gen[kNvnStages] = {}, ssbo_gen[kNvnStages] = {};
    float viewport[16][4]{};
    float depth_near = 0.0f, depth_far = 1.0f;
    int32_t scissor[4]{0, 0, 0, 0};
    uint32_t stencil_value_mask[2]{0xff, 0xff}, stencil_mask[2]{0xff, 0xff}, stencil_ref[2]{0, 0};
    float poly_offset[3]{};
    float blend_color[4]{};
    bool restart = false;
    uint32_t restart_index = 0;
    bool depth_clamp = false, raster_discard = false, render_enabled = true;
    int64_t copy_stride = 0;
    uint8_t cs_push[0x400]{};   // (hookshot) compute push constants (PushComputeConstants), c0 from 0x10
    uint32_t cs_push_n = 0;
    uint32_t grid[3]{};          // the dispatch being made (compute programs read it from c0)
    uint64_t samples_passed = 0;
    // (hookshot) real occlusion counts (vk_replay.cpp, RX_VK_OCC_QUERIES): the draws from a samples-passed reset to its report
    // each get a query. They are numbered from one counter for every queue (occ_next_all, vk_replay.cpp): occ_next is this
    // state's copy of it while one of its command buffers is replayed (occ_begin_buffer), so a parallel replay's dry pass and
    // its workers, which start from copies of the state, agree on the numbers
    uint64_t occ_next = 0, occ_first = 0;
    uint64_t occ_limit = 0;      // (queries are taken below it: set before a command buffer's replay, when the ring has room)
    bool occ_on = false;         // (a reset was seen and no report yet)
    bool occ_overflow = false;   // (the run's count is not known: too many draws, a colour draw, or split by another queue's
                                 // queries: its report stays "visible")
    bool occ_color = false;      // (a draw of the run writes colour: the game counts a real draw, not a depth-only box)
};

// texture / sampler pool registrations (by handle id), as nvn_register_* keep them in the WebGPU renderer
void register_texture(int id, const nvn_tex* t, const nvn_view* v);
void register_sampler(int id, const nvn_samp_par* p);
/// (the same with a copy already checked: the replay thread's, in order with the submits)
void register_texture_copy(int id, const nvn_tex& t, const nvn_view* v);
void register_sampler_copy(int id, const nvn_samp_par& p);
/// The pointers' checks for a registration's copy (null when refused)
const void* descriptor_ok(const void* p, size_t n);
void texture_released(uint64_t serial);
bool texture_of(int id, nvn_tex* t, nvn_view* v, bool* has_view);
void cs_slot_counts(const NvnState& s, uint32_t* tex, uint32_t* img);   // (hookshot) the compute program's slot counts
/// (hookshot) set while a seed compute program is translated: its control section, which the c0 layout is read from in place of
/// the bound program's (vk_bank.cpp)
extern thread_local const uint8_t* seed_cs_control;
bool sampler_of(int id, nvn_samp_par* p);
/// Changes whenever a texture slot may resolve to another texture (a registration, a texture finalized)
uint64_t registrations_generation();
/// Changes whenever the slot may resolve to another texture (its registrations, its texture finalized)
uint32_t texture_slot_version(int id);
/// (hookshot, RX_VK_SAMPLER_CACHE) Changes whenever the sampler id may resolve to other parameters (its registrations)
uint32_t sampler_slot_version(int id);
/// Changes on any sampler registration: descriptor reuse checks the samplers its last build resolved
uint64_t sampler_registrations_generation();
/// (hookshot) What every sampler is made from changed outside the registrations (the Anisotropic filtering row): moves that
/// generation, so sampler_for's tables and the descriptors kept by reuse make theirs again
void samplers_changed();

/// One word of a driver constant bank (c0 / c2) as the NVN driver lays it out for the bound state
uint32_t driver_bank_word(const NvnState& s, int bank, int stage, uint32_t offset);
/// The first n bytes of a driver bank (what driver_bank_word gives, in one pass)
void driver_bank_fill(const NvnState& s, int bank, int stage, uint8_t* d, uint32_t n);
/// A driver bank whole (0xA00 bytes)
void driver_bank(const NvnState& s, int bank, int stage, uint8_t d[0xA00]);

} // namespace mkwvk
