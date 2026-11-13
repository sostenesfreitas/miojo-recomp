// Draws and dispatches (vk_draw.cpp).
#pragma once

#include <memory>
#include <vector>

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {
/// Something other than the draw path pushed or bound descriptors in the recording (its next push is whole)
void descriptors_disturbed();

/// A graphics pipeline as hookshot_pipelines.bin keeps it (the precompile's notes): what built it
struct PipeNote {
    uint64_t key, prog, variant;
    uint32_t topo, n, samples, depth_format, has_depth;
    uint32_t color_format[8];
    NvnState s;
};
/// (vk_predict.cpp) What the prediction left out: pipelines whose vertex layout or targets have types the program was not made for
/// (no draw asks for them), and the program variants left with none; and the notes whose own types do not fit (0: else the rule
/// that leaves them out is wrong)
struct PredictCounts {
    size_t vertex_types = 0, output_types = 0, variants_unfit = 0, notes_unfit = 0;
    size_t refit = 0;   // (program variants whose signature fallback took a less common class, the most common not fitting)
};
/// (vk_predict.cpp) The pipelines the notes predict for the kept graphics program variants no note names (key 0: not made yet)
std::vector<std::unique_ptr<PipeNote>> predict_pipelines(const std::vector<std::unique_ptr<PipeNote>>& notes, size_t* programs,
                                                         PredictCounts* left_out = nullptr);
/// The colour output types a note's targets give a program (Shader::AttributeType, as a draw's: color_types_of)
void note_color_types(const PipeNote& n, uint8_t types[8]);
}

#include <functional>

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {

struct DrawArgs {
    int prim = 0;
    bool indexed = false;
    int itype = 0;            // NVNindexType: byte, short, int
    int count = 0, first = 0, base_vertex = 0, base_instance = 0, instances = 1;
    uint64_t ibuf = 0;
    // indirect: arguments at indirect (stride, up to max), their count at count_addr (or 0)
    uint64_t indirect = 0, count_addr = 0;
    int max = 0, stride = 0;
    // mesh shading (NVN2 task / mesh programs): workgroups, or indirect as above
    bool mesh = false;
    int groups[3] = {0, 1, 1};
    uint32_t first_task = 0;   // (hookshot: a direct mesh draw's first task workgroup, NVN's DrawMeshTasks(first, count))
};

/// A recording starts (its slot's descriptor pool and upload stream are reset); value: the timeline value it signals
void draw_begin_recording(int slot, VkCommandBuffer cmd, uint64_t value);
/// Publish CPU writes from this slot's used streams before its commands reach the GPU (also for worker slots).
void draw_flush_recording(int slot);
/// The recording's stream still holds [at, at + n) in the mapping at base (a reset starts used over)
bool stream_holds(const uint8_t* base, uint64_t at, uint64_t n);
/// The open render pass ends (before transfers, clears, barriers and the recording's end)
void end_pass(VkCommandBuffer cmd);
void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d);
/// Clears of the bound targets, inside the open pass (NOP_CLEAR_COLOR, NOP_CLEAR_DS)
void clear_color_target(VkCommandBuffer cmd, NvnState& s, int index, const float rgba[4], uint32_t mask);
void clear_depth_stencil(VkCommandBuffer cmd, NvnState& s, float depth, bool depth_mask, uint32_t stencil,
                         uint32_t stencil_mask);
void clear_stats(int64_t* clears, int64_t* masked);
/// indirect: the workgroup counts' GPU address (x, y, z unused)
void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect);
void dry_mark_written(NvnState& s, bool compute, bool first);   // (hookshot: the parallel replay's dry pass)
void dry_marks_begin();   // (at the start of each dry pass: the last targets it skipped by are forgotten)
void draw_stats(int64_t* draws, int64_t* skipped, int64_t* dispatches, int64_t* pipelines);
void draw_stats2(int64_t* passes, int64_t* mesh, double* pipeline_ms);
void draw_stats3(int64_t* pending_skips, int64_t* waits, double* wait_ms);
/// (hookshot) pipelines linked from stage libraries in the draw, and the libraries asked of the driver for them: had, and cold;
/// of the cold, those a worker has built since, and the workers' time in them. False: no stage libraries (RX_VK_GPL)
bool draw_stats4(int64_t* links, double* link_ms, int64_t* lib_hits, int64_t* lib_cold, int64_t* lib_built, double* lib_build_ms);
void draw_skip_report(double frames);   // (hookshot) skips by the line that made them
int64_t draw_desc_reused();
/// (Descriptor reuse) Of those, the reuses the global registrations' generation check would have rebuilt for
int64_t draw_slotgen_saves();
/// RX_VK_FLASH_RECORD=<key>,...: the last frames' draws of those programs, written to the log for frames around a flash
void draw_records_log(uint64_t frame_lo, uint64_t frame_hi);
void pass_traces_log(uint64_t frame_lo, uint64_t frame_hi);   // (RX_VK_PASS_TRACE=1)
/// Changes with every uniform update the stream makes (vk_replay.cpp): descriptors bound before may be stale
uint64_t ub_generation();
/// A background thread (shader translation, pipeline builds) reads guest memory from the captures only, never the stream's
/// uniform updates: that list is the replay's, which grows and is pruned while such a thread runs
void ub_thread_off();
/// The pipeline cache to disk (force: whatever was built since the last save; else only after a few new ones)
void pipeline_cache_save(bool force);
void pipeline_cache_save_play();   // what play made since the last save, into a play file now
// (the window closed: what play made since the last save, a few ms; the start's cache is not written there, as before)
void pipeline_cache_close();
void pipeline_cache_play();   // (the start-up done: what play makes goes into play files of its own)
/// (hookshot) hookshot_pipeline_states.bin's bytes now, no record half written (the menu's Save my pipeline list); *present: it is there
std::string state_list_snapshot(bool* present);

/// The stream's uniform updates (UPDATE_UB, vk_replay.cpp) that overlap [addr, addr + n) and may not be in guest
/// memory yet when the GPU reads it: a copy of the range with them applied, at the offset alloc returns in the
/// recording's stream (mapped at base), or UINT64_MAX when none overlaps
uint64_t ub_overlay(uint64_t addr, uint64_t n, uint8_t* base, const std::function<uint64_t(uint64_t)>& alloc);
/// Guest memory as the stream's place sees it: as submitted, the stream's uniform updates before it over it
bool ub_read(uint64_t addr, void* dst, uint64_t n);

} // namespace mkwvk
