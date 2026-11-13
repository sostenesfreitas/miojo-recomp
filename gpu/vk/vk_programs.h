// (hookshot) The programs' life (vk_programs.cpp): what the game does with its programs (makes them, gives them their shaders,
// frees them), what each needed when it was first drawn or dispatched, and one set of counters for all of it (the census).
// Not part of the translator stamp: a change here keeps every player's translated programs.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "vk_internal.h"

namespace mkwvk {

struct Program;
struct PipeNote;
struct NoteRef;

// ---- the load when the game makes a program (RX_VK_LOAD=create) ----
/// RX_VK_LOAD: start (every listed program and pipeline kept ready before the title, precompile_run) or create (nothing kept
/// ready by a list: a program's kept variants and noted pipelines are loaded when the game makes it). Without the switch:
/// load_default.h
bool programs_load_at_create();
/// (vk_main.cpp, once the device is up) RX_VK_LOAD or the default decided. native_mesh: the GPU draws mesh programs itself, so
/// a start keeps no mesh fallback programs ready
void programs_load_decide(bool native_mesh);
/// (the replay thread, with each frame line) the safety net's second part: the listed pipelines this session's loads found in
/// no cache (load_default.h caches_lacked). Draws left out for pipelines no list names are not this mode's and not counted
void programs_frames_note();
/// (precompile_run, before play) what a create call looks its program up in: every note by its place, and the compute
/// programs the lists name
void programs_index(std::vector<NoteRef> notes, const std::unordered_set<uint64_t>& compute_keys);
/// (program_cache_load, shader_mu held) a load queued for that program key is run here if no worker has begun it, or waited for,
/// and what it read is published before this returns
void programs_load_wait(uint64_t key);

/// (the life log) a texture upload of over 15 ms on the replay thread: when it began, on the load units' clock, and how many
/// load jobs were running at its end
void programs_slow_upload(double ms, uint32_t width, uint32_t height, uint32_t nvn_format);

// ---- shader module identifiers (VK_EXT_shader_module_identifier, module_ids.h) ----
/// (the device's start) the extension is on: a compute module made from its code is learned by its identifier from now on, and
/// a load at a create call asks for a learned variant's pipeline by it, with no code read and no module made.
/// RX_VK_MODULE_IDS=0: off
void module_ids_on();
/// A compute program's module made from its code: its identifier kept for the next run (hookshot_module_ids.bin)
void module_id_learned(uint64_t key, uint64_t variant, VkShaderModule module);
/// What was learned, written (a load worker after its job, and the close)
void module_ids_flush();

/// Where a ready pipeline for a program variant and its state comes from, nothing compiled. One source today: the pipeline
/// cache objects (the start's cache in memory, then the driver's own). Another (pipeline binaries, shader module identifiers)
/// would be asked first and fall back to this one where the driver lacks it or its key does not match
struct PipelineSource {
    virtual ~PipelineSource() = default;
    virtual const char* name() const = 0;
    /// The pipeline a note names for its program, or null: this source does not have it
    virtual VkPipeline graphics(const Program& p, const PipeNote& n) = 0;
    /// A compute program's pipeline, or null
    virtual VkPipeline compute(const Program& p) = 0;
};
PipelineSource& pipeline_source();

// ---- the game's side (its own threads) ----
/// The game gave a program its shaders (MkwVkRenderer::nvn_program_created): its stages are in the game's memory now
void programs_created(const nvn_prog* p);
/// The game frees a program, or gives it other shaders (MkwVkRenderer::nvn_program_released, before the replay hears of it)
void programs_freed(const nvn_prog* p);
/// The game presents a frame (vk_replay.cpp present, the presenting thread)
void programs_presented();
/// Whether the game seems to be in a load: no frame presented for kLoadGapMs or more. *since_ms: how long since the last one
constexpr double kLoadGapMs = 100.0;
bool programs_in_load(double* since_ms = nullptr);

// ---- the census: one set of counters, whoever does the work ----
struct ProgramCensus {
    std::atomic<int64_t> created{0}, created_ok{0}, freed{0}, freed_undrawn{0};
    std::atomic<int64_t> created_in_load{0};       // (no present for kLoadGapMs before the call)
    std::atomic<int64_t> created_long_frame{0};    // (in a frame that took kLoadGapMs or more, as the next present told)
    std::atomic<int64_t> create_ns{0};             // (the game's thread's time in the create call)
    // first uses (a program's first draw or dispatch): what the program needed
    std::atomic<int64_t> used{0}, used_ready{0}, used_loaded{0}, used_translated{0}, used_failed{0};
    std::atomic<int64_t> used_unknown{0};          // (no create call was seen for it: made before the back end started)
    std::atomic<int64_t> used_codes_differ{0};     // (the stages the draw read are not those read at the create call)
    // ... and its pipeline
    std::atomic<int64_t> pipe_ready{0}, pipe_cache{0}, pipe_compiled{0}, pipe_made{0}, pipe_left_out{0}, pipe_none{0};
    // how long after its create call the first use came
    std::atomic<int64_t> after_16{0}, after_100{0}, after_1000{0}, after_more{0};
    // ... for the first uses that needed something (a load, a translation, a pipeline made)
    std::atomic<int64_t> needy{0}, needy_after_100{0}, needy_in_load{0};
    // everything play makes, first use or not
    std::atomic<int64_t> play_loads{0}, play_load_ns{0};   // (program records read from hookshot_programs.bin in play)
    std::atomic<int64_t> play_pipes_new{0};                // (graphics pipelines play asked for that were not ready)
    std::atomic<int64_t> play_from_driver{0}, play_from_start{0}, play_compiled{0};   // (create_cached, graphics and compute)
    // the load at create (RX_VK_LOAD=create)
    std::atomic<int64_t> units_queued{0}, units_loaded{0}, units_nothing{0};   // (nothing: no kept program for those stages)
    std::atomic<int64_t> units_early{0};        // (create calls before the index was there)
    std::atomic<int64_t> units_run_by_draw{0}, units_waited{0}, unit_wait_ns{0};   // (a first use ran the job itself, or waited)
    std::atomic<int64_t> load_programs{0}, load_pipes{0}, load_pipes_cold{0}, load_computes{0}, load_computes_cold{0};
    std::atomic<int64_t> load_light{0};         // (compute variants read without their code)
    std::atomic<int64_t> load_by_id{0}, load_by_id_cold{0};   // (compute pipelines made by a module identifier; asked, in no cache)
    std::atomic<int64_t> load_unlisted{0};      // (graphics programs loaded that no note names: no pipeline is known for them)
    std::atomic<int64_t> load_ns{0}, load_bytes{0};
    std::atomic<int64_t> pipes_taken{0};        // (pipelines made at create that a draw then took)
};
extern ProgramCensus census;
/// One line of the census (every 600 presents with RX_VK_LOG_PROGRAM_LIFE=1, and when the window closes)
void programs_census_log(const char* when);
bool programs_life_on();   // (RX_VK_LOG_PROGRAM_LIFE=1)

// ---- a program's first draw or dispatch (the thread replaying), for the log and the census ----
enum class NeedProgram : uint8_t { Ready, Loaded, Translated, Failed };
enum class NeedPipeline : uint8_t { None, Ready, Cache, Compiled, Made, LeftOut };   // (Made: by another thread, how not known)
/// What vk_progcache.cpp and vk_draw.cpp count on the thread that does the work, read around a first use
struct UseCounters {
    int64_t loads = 0, load_ns = 0;   // (program records read from disk by this thread)
    int64_t pipes_new = 0;            // (pipelines this thread asked for that no one had)
    uint8_t made_how = 0;             // (the last pipeline this thread made: 1 the driver's cache, 2 the start's, 3 compiled)
    bool background = false;          // (a worker of the load at create: what it reads is not a load a draw paid for)
};
extern thread_local UseCounters use_counters;
/// Around a draw or dispatch: reports the first use of the bound program's serial (once a serial, whichever thread). Costs one
/// compare a draw when the log is off
extern bool programs_life_flag;   // (programs_life_on, read once the back end starts)
struct ProgramUse {
    bool on = false, compute = false;
    uint64_t serial = 0;
    const Program* p = nullptr;
    std::chrono::steady_clock::time_point t0;
    UseCounters before;
    int64_t translated0 = 0;
    bool program_ready = false, pipe_new = false, pipe_was_ready = false, left_out = false, has_pipe = false;
    uint8_t pipe_how = 0;
    ProgramUse(const nvn_prog& prog, bool has_prog, bool is_compute) {
        if (programs_life_flag && has_prog) begin(prog, is_compute);
    }
    ~ProgramUse() {
        if (on) end();
    }
    void program(const Program* cp, bool ready) { p = cp, program_ready = ready; }   // (after program_for: as it came back)
    void pipeline(bool was_ready, bool is_new) { has_pipe = true, pipe_was_ready = was_ready, pipe_new = is_new; }
    void pipeline_made(uint8_t how) { pipe_how = how; }   // (how a pipeline that was not ready was made: UseCounters::made_how)
    void skipped() { left_out = true; }                   // (left out: its program or pipeline was not ready)
    void begin(const nvn_prog& prog, bool is_compute);
    void end();
};
/// (vk_main.cpp, once the start-up's work is done) what is made from now on is made in play
void programs_play_begin();
extern std::atomic<bool> programs_in_play;

} // namespace mkwvk
