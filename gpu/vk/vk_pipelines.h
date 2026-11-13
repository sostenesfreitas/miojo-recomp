// (hookshot) What the programs' loader (vk_programs.cpp) asks of the pipelines' code (vk_draw.cpp): the precompile's notes by
// position, a pipeline made from the caches alone, and a place for pipelines made ahead of their first draw.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "vk_internal.h"
#include "vk_draw.h"

namespace mkwvk {

struct Program;

/// A note of hookshot_pipelines.bin by its place in the file, with what the programs' index needs of it: the pipeline's key, its
/// program and variant, and the vertex input formats and colour output types its program's key was made with
struct NoteRef {
    uint64_t key = 0, prog = 0, variant = 0;
    int64_t at = 0;
    uint32_t vattr[16]{};
    uint8_t color_types[8]{};
};
/// Every note's reference, read by position (26 KB a note: none is read whole)
std::vector<NoteRef> pipe_notes_refs();
std::vector<uint64_t> program_cache_compute_keys();   // (vk_progcache.cpp: the kept compute programs, for the first start's warm)
std::string load_fallback_path();   // (hookshot_load_start.txt beside the notes: load_default.h)
std::string pipe_notes_file();   // (hookshot_pipelines.bin's path)
/// The note at a place pipe_notes_refs gave, checked as every note read is. False: it does not read, or is not sane
bool pipe_note_at(FILE* f, int64_t at, PipeNote* out);
/// A note's pipeline for its program from the caches alone, nothing compiled: the start's cache first (what the prepare built is
/// in memory there), then the driver's own. Null: no cache has it (*cold), or it failed
VkPipeline pipeline_from_caches(const Program& p, const PipeNote& n, bool* cold);
/// A compute program's pipeline the same way
VkPipeline compute_pipeline_from_caches(const Program& p);
/// The same for a program read without its code (no module): by its module's identifier. Null: no cache has it
VkPipeline compute_pipeline_by_module_id(const Program& p, const uint8_t* id, uint32_t size);
/// A pipeline made ahead of its first draw: the draw's lookup takes it (graphics_pipeline). False: that key has one already,
/// and pl is destroyed
bool pipeline_keep_ready(uint64_t key, VkPipeline pl);
void program_layout(Program& p);   // (ensure_layout)
/// The compute program variants this install's play dispatched before (hookshot_compute_variants.bin): key, variant
std::vector<std::pair<uint64_t, uint64_t>> compute_variants_learned();
/// Whether every note there is now was built into the pipeline cache by a precompile that finished (precompile_run): a load from
/// the caches alone then never finds one the prepare should have compiled
bool precompile_covers_notes();

} // namespace mkwvk
