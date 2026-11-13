// (hookshot) The programs' life: the game's create, set-shaders and free calls, each program's first draw or dispatch, and the
// census of what play had to make. See vk_programs.h.
//
// RX_VK_LOG_PROGRAM_LIFE=1 logs each of those ("vk: program life: ...") and, every 600 presents and when the window closes, one
// census line ("vk: program census ..."). The log answers when the game makes its programs against when it first draws them:
// whether the work a first draw does today (a program read from hookshot_programs.bin, a pipeline made from the caches) could be
// done at the create call instead.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "vk_internal.h"
#include "vk_draw.h"
#include "vk_shader.h"
#include "vk_progcache.h"
#include "vk_pipelines.h"
#include "program_life.h"
#include "module_ids.h"
#include "load_default.h"
#include "vk_programs.h"

namespace mkwvk {

ProgramCensus census;
thread_local UseCounters use_counters;
bool programs_life_flag = false;
std::atomic<bool> programs_in_play{false};

namespace {
/// (the life log) this thread's page faults that waited for the disk so far: what a slow create call was, when it is not ours
long thread_major_faults() {
#ifndef _WIN32
    struct rusage u;
    if (getrusage(RUSAGE_THREAD, &u) == 0) return u.ru_majflt;
#endif
    return 0;
}
using Clock = std::chrono::steady_clock;
const Clock::time_point life_t0 = Clock::now();
double secs(Clock::time_point t) { return std::chrono::duration<double>(t - life_t0).count(); }
double ms_between(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::atomic<int64_t> last_present_ns{0};   // (since life_t0; 0: none yet)
std::atomic<int64_t> game_presents{0};
int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - life_t0).count(); }

/// A guest program between its create call and some seconds after its free call (a draw recorded before the free is replayed
/// after it)
struct Life {
    Clock::time_point created, freed;
    int64_t frame = 0;          // (the game's presents before the create call)
    double since_present = 0;   // (ms, at the create call)
    double frame_ms = -1;       // (the frame the create call was in, known at the next present)
    uint64_t codes[4]{};
    bool ok = false, compute = false, mesh = false, used = false, is_freed = false;
};
std::mutex life_mu;
std::unordered_map<uint64_t, Life> lives;          // by program serial
std::vector<uint64_t> created_this_frame;          // (their frame_ms is set at the next present)
std::atomic<int> created_pending{0};
std::deque<std::pair<Clock::time_point, uint64_t>> freed_serials;   // (let go kFreedKeepS after the free call)
constexpr double kFreedKeepS = 30.0;

const char* kind_name(const Life& l) { return l.compute ? "compute" : l.mesh ? "mesh" : "graphics"; }

// ---- The load when the game makes a program (RX_VK_LOAD=create) ----
// The index, made once before play (programs_index) and only read after: the vertex input and colour output types programs
// were made for (a graphics program's key is its stages and those types, and the create call gives only the stages: each
// signature is tried, and the keys the program cache holds are the program's), every note by its program, and the compute
// programs the lists name.
struct Signature {
    uint32_t vattr[16];
    uint8_t color_types[8];
};
struct NoteAt {
    int64_t at;
    uint64_t variant, key;
};
std::vector<Signature> index_signatures;
std::unordered_map<uint64_t, std::vector<NoteAt>> index_notes;   // by program key
std::unordered_set<uint64_t> index_computes;
std::unordered_set<uint64_t> index_kept;   // (every program key hookshot_programs.bin held when the index was made)
std::unordered_map<uint64_t, std::vector<uint64_t>> index_compute_variants;   // by key: the variants play dispatched before
std::atomic<bool> index_ready{false};

/// What one create call queues: the kept variants of a program's keys and their noted pipelines, read and made by a background
/// worker (or by the first draw that needs it, if no worker began it), then published under shader_mu
struct LoadJob {
    uint64_t unit = 0;
    bool compute = false;
    std::vector<uint64_t> keys;
    std::atomic<int> state{0};   // 0 queued, 1 running, 2 done
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<uint64_t, std::unique_ptr<Program>>> made;
    bool published = false;
};
std::mutex jobs_mu;
std::condition_variable jobs_cv;
std::deque<std::shared_ptr<LoadJob>> job_queue;
std::unordered_map<uint64_t, std::shared_ptr<LoadJob>> job_by_key;   // (until published)
std::atomic<int> jobs_in_flight{0};
bool workers_started = false;
program_life::Table table;   // (life_mu)
std::mutex load_ms_mu;
std::vector<float> load_ms;   // (each job's time, for the census's median and 99th percentile; the first 65536)

struct CacheSource final : PipelineSource {
    const char* name() const override { return "pipeline caches"; }
    VkPipeline graphics(const Program& p, const PipeNote& n) override {
        bool cold = false;
        return pipeline_from_caches(p, n, &cold);
    }
    VkPipeline compute(const Program& p) override { return compute_pipeline_from_caches(p); }
};

/// A handle of this thread's own on a file the loads read by position (never closed: a worker's, or the replay's)
FILE* own_handle(const std::string& path, int which) {
    static thread_local FILE* f[2];
    static thread_local bool tried[2];
    if (!tried[which]) tried[which] = true, f[which] = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    return f[which];
}

/// The job's work, with no lock held that a draw holds: the records read by position, modules and layouts made, the noted
/// pipelines asked of the pipeline source. Nothing is translated and nothing is compiled here
// ---- shader module identifiers (vk_programs.h) ----
module_ids::Store ids;
std::atomic<bool> ids_on{false};
std::atomic<int64_t> ids_learned{0};
PFN_vkGetShaderModuleIdentifierEXT ids_get = nullptr;
/// The side file opened, once, at the first use (the device is up by then). False: off, or the driver lacks the function
bool ids_ready() {
    if (!ids_on.load(std::memory_order_acquire)) return false;
    static std::once_flag once;
    std::call_once(once, [] {
        ids_get = (PFN_vkGetShaderModuleIdentifierEXT)vkGetDeviceProcAddr(dev.device, "vkGetShaderModuleIdentifierEXT");
        if (!ids_get) return ids_on.store(false, std::memory_order_release);
        VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT ip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &ip;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        const std::string cache = program_cache_path();
        ids.open(cache.substr(0, cache.find_last_of("\\/") + 1) + "hookshot_module_ids.bin", program_cache_stamp(),
                 ip.shaderModuleIdentifierAlgorithmUUID);
        logf("vk: module identifiers: %zu known (hookshot_module_ids.bin): a learned compute variant's pipeline is made at its "
             "program's create call without its code (RX_VK_MODULE_IDS=0: off)", ids.size());
    });
    return ids_on.load(std::memory_order_acquire);
}
bool module_id_known(uint64_t key, uint64_t variant, module_ids::Id* out) { return ids_ready() && ids.find(key, variant, out); }

void load_work(LoadJob& job) {
    const auto t0 = Clock::now();
    FILE* in = own_handle(program_cache_path(), 0);
    FILE* notes_in = own_handle(pipe_notes_file(), 1);
    PipelineSource& source = pipeline_source();
    static thread_local std::unique_ptr<PipeNote> note = std::make_unique<PipeNote>();
    int64_t bytes = 0;
    for (const uint64_t key : job.keys) {
        std::vector<DiskRecord> recs = program_cache_records(key);
        if (recs.empty() || !in) continue;
        const auto notes = index_notes.find(key);
        // (one variant, not all: of a key with several kept variants, those a note names. A compute program's are all read:
        // which one a dispatch takes is only known from what is bound then)
        if (!job.compute && recs.size() > 1) {
            std::vector<DiskRecord> named;
            for (const DiskRecord& d : recs) {
                uint64_t variant = 0;
                bool is_named = !program_cache_variant(in, key, d, &variant);   // (a head that does not read that way: read whole)
                if (!is_named && notes != index_notes.end())
                    for (const NoteAt& n : notes->second) is_named = is_named || n.variant == variant;
                if (is_named) named.push_back(d);
            }
            recs.swap(named);
        }
        recs = program_cache_take(key, recs);
        std::vector<std::unique_ptr<Program>> made;
        if (job.compute) {
            // (each variant with what a pick needs and not its code, vk_progcache.cpp: the dispatch that takes one reads no
            // variant's head. The variants play ran before, and a program's only variant, are made whole here, with their
            // pipelines; another is made whole by the dispatch that first takes it, program_cache_complete)
            const auto learned = index_compute_variants.find(key);
            for (const DiskRecord& d : recs) {
                std::unique_ptr<Program> p = program_cache_read_light(in, nullptr, key, d);
                if (!p) {
                    program_cache_read(in, nullptr, key, {d}, made);
                    bytes += (int64_t)d.size;
                    continue;
                }
                bool whole = recs.size() == 1;
                if (!whole && learned != index_compute_variants.end())
                    whole = std::find(learned->second.begin(), learned->second.end(), p->variant) != learned->second.end();
                bytes += (int64_t)(d.size - p->code_words * 4);
                // (by its module's identifier where a run before learned it: the pipeline from the caches with no code read
                // and no module made. The dispatch needs no module of a program whose pipeline is there; where no cache
                // has the pipeline, the code is read as before)
                module_ids::Id id;
                if (whole && module_id_known(key, p->variant, &id)) {
                    program_layout(*p);
                    if (const VkPipeline pl = compute_pipeline_by_module_id(*p, id.bytes, id.n)) {
                        p->compute_pipeline = pl;
                        p->compute_state.store(2, std::memory_order_release);
                        census.load_programs++, census.load_light++, census.load_computes++, census.load_by_id++;
                        job.made.push_back({key, std::move(p)});
                        continue;
                    }
                    census.load_by_id_cold++;
                }
                if (whole) bytes += (int64_t)p->code_words * 4, whole = program_cache_complete_unshared(*p, in);
                if (!whole && !p->failed) {   // (no module yet, so no pipeline; its layout is made of its descriptors alone)
                    program_layout(*p);
                    census.load_programs++, census.load_light++;
                    job.made.push_back({key, std::move(p)});
                } else if (whole) {
                    made.push_back(std::move(p));
                }
            }
        } else {
            program_cache_read(in, nullptr, key, recs, made);
            for (const DiskRecord& d : recs) bytes += (int64_t)d.size;
        }
        for (auto& p : made) {
            program_layout(*p);
            if (p->compute) {
                if (const VkPipeline pl = source.compute(*p)) {
                    p->compute_pipeline = pl;
                    p->compute_state.store(2, std::memory_order_release);
                    census.load_computes++;
                } else {
                    census.load_computes_cold++;   // (its first dispatch asks again, and compiles on a worker: as before)
                }
            } else if (notes != index_notes.end()) {
                for (const NoteAt& n : notes->second) {
                    if (n.variant != p->variant || !pipe_note_at(notes_in, n.at, note.get())) continue;
                    bytes += (int64_t)sizeof(PipeNote);
                    if (const VkPipeline pl = source.graphics(*p, *note)) census.load_pipes += pipeline_keep_ready(n.key, pl);
                    else census.load_pipes_cold++;   // (its first draw makes it, as before)
                }
            }
            census.load_programs++;
            census.load_unlisted += !p->compute && notes == index_notes.end();
            job.made.push_back({key, std::move(p)});
        }
    }
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    census.load_ns += ns, census.load_bytes += bytes;
    if (programs_life_flag)   // (the life log: every load, when it began and how long it took, with what it was)
        logf("vk: program life: load unit %016llx t %.3f ms %.2f %s keys %zu programs %zu bytes %lld by %s", (unsigned long long)job.unit,
             secs(t0), ns / 1e6, job.compute ? "compute" : "graphics", job.keys.size(), job.made.size(), (long long)bytes,
             use_counters.background ? "worker" : "draw");
    std::lock_guard lk(load_ms_mu);
    if (load_ms.size() < 65536) load_ms.push_back((float)(ns / 1e6));
}
/// The job's work is done: the unit is ready (what it read is the job's until a pick asks for one of its keys)
void load_done(LoadJob& job) {
    const bool any = !job.made.empty();
    {
        std::lock_guard lk(life_mu);
        table.loaded(job.unit, any);
    }
    census.units_loaded += any;
    {
        std::lock_guard lk(job.mu);
        job.state.store(2, std::memory_order_release);
    }
    job.cv.notify_all();
}
/// (shader_mu held, by the pick that asks for one of the job's keys) what the job read goes to the programs' lists, once.
/// Only ever here, on the thread that picks: a worker never takes shader_mu, so a pick never waits for that lock behind a
/// worker the system has set aside for busier threads (the workers run below normal priority, and a load is when every
/// processor is busy)
void load_publish(const std::shared_ptr<LoadJob>& job) {
    {
        std::lock_guard lk(job->mu);
        if (job->published) return;
        job->published = true;
        for (auto& [key, p] : job->made) programs[key].push_back(std::move(p));
        job->made.clear();
    }
    std::lock_guard lk(jobs_mu);
    for (const uint64_t key : job->keys)
        if (const auto it = job_by_key.find(key); it != job_by_key.end() && it->second == job) job_by_key.erase(it);
    jobs_in_flight.fetch_sub(1, std::memory_order_release);
}
/// How many jobs may run at once. In play the workers must not take time from the game's threads (an emulator measured 49
/// against 60 frames a second with background builds competing): RX_VK_LOAD_WORKERS (2). While no frame has been presented for
/// kLoadGapMs the game is taken to be loading, and RX_VK_LOAD_WORKERS_LOADING run (the same number unless set: the signal is
/// not proven enough to lift the cap by default)
int workers_cap(bool loading) {
    // (0, a measurement: no worker at all, so only what a draw or dispatch asks for is loaded, by the draw itself)
    static const int play = std::clamp(getenv("RX_VK_LOAD_WORKERS") ? atoi(getenv("RX_VK_LOAD_WORKERS")) : 2, 0, 16);
    static const int load = std::clamp(getenv("RX_VK_LOAD_WORKERS_LOADING") ? atoi(getenv("RX_VK_LOAD_WORKERS_LOADING")) : play, play, 16);
    return loading ? load : play;
}
std::atomic<int> workers_running{0};
std::atomic<int> workers_busy{0};   // (workers inside a job now: the life log)
void load_worker(int index) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ub_thread_off();   // (a background thread: guest memory from the captures only)
    use_counters.background = true;
    for (;;) {
        std::shared_ptr<LoadJob> job;
        {
            std::unique_lock lk(jobs_mu);
            // (a worker past the cap in play sleeps until the game seems to load, looked at again every 20 ms)
            for (;;) {
                if (!job_queue.empty() && index < workers_cap(programs_in_load())) break;
                if (job_queue.empty()) jobs_cv.wait(lk);
                else jobs_cv.wait_for(lk, std::chrono::milliseconds(20));
            }
            job = std::move(job_queue.front());
            job_queue.pop_front();
        }
        int queued = 0;
        if (!job->state.compare_exchange_strong(queued, 1)) continue;   // (a first use took it)
        workers_busy++;
        load_work(*job);
        load_done(*job);
        workers_busy--;
        module_ids_flush();   // (what this job's modules taught, for the next run)
    }
}
} // namespace

PipelineSource& pipeline_source() {
    static CacheSource caches;
    return caches;
}

/// RX_VK_LOAD, or the default (load_default.h): create where the GPU has mesh shaders and no earlier session's first uses
/// waited too long for loads, start otherwise. Decided once the device is up (vk_main.cpp); start until then
std::atomic<int> load_mode{-1};
void programs_load_decide(bool native_mesh) {
    const load_default::Choice c = load_default::choose(getenv("RX_VK_LOAD"), native_mesh, load_default::marker_there(load_fallback_path()),
                                                        env_on("RX_VK_PROGRAM_CACHE_ZSTD"));   // (as vk_progcache.cpp disk_init reads it)
    load_mode.store(c.create ? 1 : 0, std::memory_order_release);
    logf("vk: programs are loaded %s: %s (RX_VK_LOAD=create or start says which)",
         c.create ? "when the game makes them (create)" : "before the title (start)", c.why);
}
bool programs_load_at_create() { return load_mode.load(std::memory_order_acquire) == 1; }
namespace {
/// (a first use ran or waited for a load, ns) The safety net: one wait over load_default::kWorstMs, or over kTotalMs in all in
/// this session, and the next start of this install loads before the title again (hookshot_load_start.txt; a preparation
/// removes it). Loads at create are for PCs where a first use hardly ever meets one
std::atomic<int64_t> wait_worst_ns{0};
std::atomic<bool> wait_marked{false};
void load_wait_note(int64_t ns) {
    int64_t worst = wait_worst_ns.load(std::memory_order_relaxed);
    while (ns > worst && !wait_worst_ns.compare_exchange_weak(worst, ns)) {}
    const double worst_ms = std::max(worst, ns) / 1e6, total_ms = census.unit_wait_ns.load() / 1e6;
    if (!load_default::too_slow(worst_ms, total_ms) || wait_marked.exchange(true)) return;
    const bool wrote = load_default::marker_write(load_fallback_path(), worst_ms, total_ms);
    logf("vk: first uses waited for loads for %.0f ms in this session (the longest %.0f ms): over %.0f ms at once or %.0f ms in "
         "all, so the next start loads every listed program before the title again%s", total_ms, worst_ms,
         load_default::kWorstMs, load_default::kTotalMs, wrote ? " (hookshot_load_start.txt)" : "; the marker did not write");
}
}

void programs_index(std::vector<NoteRef> notes, const std::unordered_set<uint64_t>& compute_keys) {
    const auto t0 = Clock::now();
    std::unordered_set<uint64_t> seen;
    for (const NoteRef& n : notes) {
        index_notes[n.prog].push_back({n.at, n.variant, n.key});
        // (the types alone make the key: one signature for all the formats of the same types)
        uint64_t h = 0xCBF29CE484222325ull;
        for (int i = 0; i < 16; i++) h = (h ^ (uint64_t)attribute_type_of(n.vattr[i])) * 1099511628211ull;
        for (int i = 0; i < 8; i++) h = (h ^ n.color_types[i]) * 1099511628211ull;
        if (!seen.insert(h).second) continue;
        Signature sig;
        std::memcpy(sig.vattr, n.vattr, sizeof sig.vattr), std::memcpy(sig.color_types, n.color_types, 8);
        index_signatures.push_back(sig);
    }
    index_computes = compute_keys;
    index_kept = program_cache_kept_keys();
    for (const auto& [key, variant] : compute_variants_learned()) index_compute_variants[key].push_back(variant);
    index_ready.store(true, std::memory_order_release);
    {
        std::lock_guard lk(jobs_mu);
        if (!workers_started) {
            workers_started = true;
            const int n = workers_cap(true);
            for (int i = 0; i < n; i++) std::thread(load_worker, i).detach();
        }
    }
    logf("vk: programs: load at create: %zu notes of %zu programs indexed, %zu type signatures, %zu compute programs listed, in "
         "%.1f ms (%zu kept program keys); %d workers in play, %d while the game loads; pipelines from: %s",
         notes.size(), index_notes.size(), index_signatures.size(), index_computes.size(), ms_between(t0, Clock::now()), index_kept.size(),
         workers_cap(false), workers_cap(true), pipeline_source().name());
}

void programs_slow_upload(double ms, uint32_t width, uint32_t height, uint32_t nvn_format) {
    if (!programs_life_flag) return;
    // (t: when it began, on the clock of the load unit lines, so that the two can be laid over each other)
    logf("vk: program life: slow upload t %.3f ms %.2f %ux%u NVN %#x load jobs running at its end %d",
         secs(Clock::now()) - ms / 1e3, ms, width, height, nvn_format, workers_busy.load(std::memory_order_relaxed));
}
void module_ids_flush() {
    if (ids_on.load(std::memory_order_acquire) && ids.pending() && !ids.flush()) {
        static std::atomic<int> told{0};
        if (told++ < 3) logf("vk: module identifiers: hookshot_module_ids.bin did not write");
    }
}
void module_id_learned(uint64_t key, uint64_t variant, VkShaderModule module) {
    if (!module || !ids_ready() || ids.has(key, variant)) return;
    VkShaderModuleIdentifierEXT got{VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT};
    ids_get(dev.device, module, &got);
    if (ids.add(key, variant, got.identifier, got.identifierSize)) ids_learned++;
}
void module_ids_on() {
    static const bool off = getenv("RX_VK_MODULE_IDS") && !atoi(getenv("RX_VK_MODULE_IDS"));
    if (!off) ids_on.store(true, std::memory_order_release);
}

void programs_frames_note() {
    if (!programs_load_at_create()) return;
    // (what the loads at create calls asked of the caches and no cache had: load_work counts them)
    const long long lacked = census.load_pipes_cold.load(std::memory_order_relaxed) + census.load_computes_cold.load(std::memory_order_relaxed);
    if (!load_default::caches_lacked(lacked) || wait_marked.exchange(true)) return;
    const bool wrote = load_default::marker_write_lacked(load_fallback_path(), lacked);
    logf("vk: %lld listed pipelines were in no cache when their programs were loaded in this session (over %lld): they are "
         "compiled in play, so the next start loads every listed program before the title again%s", lacked,
         load_default::kNotInCacheMost, wrote ? " (hookshot_load_start.txt)" : "; the marker did not write");
}

void programs_load_wait(uint64_t key) {
    if (!jobs_in_flight.load(std::memory_order_acquire)) return;
    std::shared_ptr<LoadJob> job;
    {
        std::lock_guard lk(jobs_mu);
        if (const auto it = job_by_key.find(key); it != job_by_key.end()) job = it->second;
    }
    if (!job) return;
    if (job->state.load(std::memory_order_acquire) != 2) {
        const auto t0 = Clock::now();
        int queued = 0;
        if (job->state.compare_exchange_strong(queued, 1)) {   // (no worker began it: urgent means run here, as jobs_wait does)
            load_work(*job);
            load_done(*job);
            census.units_run_by_draw++;
        } else {
            std::unique_lock lk(job->mu);
            job->cv.wait(lk, [&] { return job->state.load(std::memory_order_acquire) == 2; });
            census.units_waited++;
        }
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
        census.unit_wait_ns += ns;
        load_wait_note(ns);
        if (programs_life_flag)   // (the life log: every first use that met a load not done yet, with how long it took)
            logf("vk: program life: use met load unit %016llx key %016llx %s ms %.2f", (unsigned long long)job->unit,
                 (unsigned long long)key, queued == 0 ? "ran" : "waited", ns / 1e6);
    }
    load_publish(job);
}

namespace {
/// (the game's thread, at a create call) the program's unit queued if the prepare kept something for its stages
void load_at_create(const nvn_prog& prog, const CreatedProgram& made) {
    if (!index_ready.load(std::memory_order_acquire)) return void(census.units_early++);
    uint64_t unit = made.compute ? 0x5EED : made.mesh ? 0x3E54 : 0x6A;
    for (const uint64_t c : made.codes) unit = (unit ^ c) * 1099511628211ull;
    unit |= 1;
    std::vector<uint64_t> keys;
    {
        std::lock_guard lk(life_mu);
        if (const program_life::Unit* u = table.find(unit); u && u->state != program_life::State::Released) {
            table.create(prog.serial, unit, false);   // (stages met before: loaded, loading, or nothing kept for them)
            return;
        }
    }
    if (made.compute) {   // (any kept compute program, listed or not: read without its code, it costs little)
        if (index_kept.count(made.codes[0])) keys.push_back(made.codes[0]);
    } else {
        for (const Signature& sig : index_signatures) {
            const uint64_t key = program_created_key(prog.serial, sig.vattr, sig.color_types);
            // (only programs a note names, as RX_VK_LOAD=start keeps ready only those; a kept program no list has a pipeline
            // of is read at its first draw, as there. RX_VK_LOAD_UNLISTED=1 loads those here too: 680 MB more host heap a
            // minute into play on the full tour, and no fewer first uses needing work)
            static const bool unlisted = getenv("RX_VK_LOAD_UNLISTED") && atoi(getenv("RX_VK_LOAD_UNLISTED")) != 0;
            if (key && index_kept.count(key) && (unlisted || index_notes.count(key)) &&
                std::find(keys.begin(), keys.end(), key) == keys.end())
                keys.push_back(key);
        }
    }
    bool queue;
    {
        std::lock_guard lk(life_mu);
        queue = table.create(prog.serial, unit, !keys.empty());
        if (queue) table.loading(unit);
    }
    if (!queue) return void(census.units_nothing += keys.empty());
    auto job = std::make_shared<LoadJob>();
    job->unit = unit, job->compute = made.compute, job->keys = std::move(keys);
    census.units_queued++;
    std::lock_guard lk(jobs_mu);
    for (const uint64_t key : job->keys) job_by_key[key] = job;
    jobs_in_flight.fetch_add(1, std::memory_order_release);
    job_queue.push_back(std::move(job));
    jobs_cv.notify_one();
}
} // namespace

bool programs_life_on() {
    static const bool on = env_on("RX_VK_LOG_PROGRAM_LIFE");
    programs_life_flag = on;
    return on;
}

bool programs_in_load(double* since_ms) {
    const int64_t last = last_present_ns.load(std::memory_order_relaxed);
    const double ms = last ? (now_ns() - last) / 1e6 : 0.0;   // (before the first frame: not a load anyone can see)
    if (since_ms) *since_ms = ms;
    return ms >= kLoadGapMs;
}

void programs_presented() {
    const int64_t now = now_ns();
    const int64_t prev = last_present_ns.exchange(now, std::memory_order_relaxed);
    game_presents.fetch_add(1, std::memory_order_relaxed);
    const double frame_ms = prev ? (now - prev) / 1e6 : 0.0;
    if (!created_pending.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(life_mu);
    for (const uint64_t serial : created_this_frame)
        if (const auto it = lives.find(serial); it != lives.end()) it->second.frame_ms = frame_ms;
    if (frame_ms >= kLoadGapMs) census.created_long_frame += (int64_t)created_this_frame.size();
    created_this_frame.clear();
    created_pending = 0;
}

void programs_play_begin() {
    programs_life_on();
    programs_in_play = true;
}

void programs_created(const nvn_prog* p) {
    const bool life = programs_life_on(), load = programs_load_at_create();
    if (!p || (!life && !load)) return;
    const auto t0 = Clock::now();
    double since = 0;
    const bool in_load = programs_in_load(&since);
    const long faults0 = life ? thread_major_faults() : 0;
    const CreatedProgram made = program_created_read(*p);
    const auto t_read = Clock::now();
    if (load && made.ok) load_at_create(*p, made);
    if (!life) return;
    Life l;
    l.created = t0, l.frame = game_presents.load(std::memory_order_relaxed), l.since_present = since;
    std::memcpy(l.codes, made.codes, sizeof l.codes);
    l.ok = made.ok, l.compute = made.compute, l.mesh = made.mesh;
    const auto t1 = Clock::now();
    census.created++, census.created_ok += made.ok, census.created_in_load += in_load;
    census.create_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    logf("vk: program life: create serial %llx t %.3f frame %lld since_present_ms %.1f in_load %d kind %s stages %d codes "
         "%016llx %016llx %016llx %016llx read_us %.0f queue_us %.0f major_faults %ld",
         (unsigned long long)p->serial, secs(t0), (long long)l.frame, since, (int)in_load, made.ok ? kind_name(l) : "none",
         made.stages, (unsigned long long)l.codes[0], (unsigned long long)l.codes[1], (unsigned long long)l.codes[2],
         (unsigned long long)l.codes[3], ms_between(t0, t_read) * 1000.0, ms_between(t_read, t1) * 1000.0,
         thread_major_faults() - faults0);
    std::lock_guard lk(life_mu);
    lives[p->serial] = l;
    created_this_frame.push_back(p->serial);
    created_pending = 1;
}

void programs_freed(const nvn_prog* p) {
    const bool life = programs_life_on(), load = programs_load_at_create();
    if (!p || !p->serial || (!life && !load)) return;
    const auto now = Clock::now();
    program_created_forget(p->serial);
    std::lock_guard lk(life_mu);
    if (load) table.free(p->serial);
    if (!life) return;
    while (!freed_serials.empty() && std::chrono::duration<double>(now - freed_serials.front().first).count() > kFreedKeepS) {
        lives.erase(freed_serials.front().second);
        freed_serials.pop_front();
    }
    const auto it = lives.find(p->serial);
    if (it == lives.end() || it->second.is_freed) return;   // (one made before the back end started, or freed twice)
    Life& l = it->second;
    l.is_freed = true, l.freed = now;
    freed_serials.push_back({now, p->serial});
    census.freed++, census.freed_undrawn += !l.used;
    logf("vk: program life: free serial %llx t %.3f frame %lld age_ms %.1f used %d", (unsigned long long)p->serial, secs(now),
         (long long)game_presents.load(std::memory_order_relaxed), ms_between(l.created, now), (int)l.used);
}

void ProgramUse::begin(const nvn_prog& prog, bool is_compute) {
    static thread_local uint64_t last_serial[2];
    static thread_local std::unordered_set<uint64_t> seen;
    if (prog.serial == last_serial[is_compute]) return;
    last_serial[is_compute] = prog.serial;
    if (!seen.insert(prog.serial).second) return;
    on = true, compute = is_compute, serial = prog.serial;
    before = use_counters;
    translated0 = shader_translated();
    t0 = Clock::now();
}

void ProgramUse::end() {
    const auto t1 = Clock::now();
    NeedProgram np = NeedProgram::Ready;
    if (!p || p->failed) np = NeedProgram::Failed;
    else if (!program_ready || shader_translated() != translated0) np = NeedProgram::Translated;
    else if (use_counters.loads != before.loads) np = NeedProgram::Loaded;
    NeedPipeline pp = NeedPipeline::None;
    if (left_out) pp = NeedPipeline::LeftOut;
    else if (has_pipe && pipe_was_ready && !pipe_new) pp = NeedPipeline::Ready;
    else if (has_pipe)
        pp = pipe_how == 1 || pipe_how == 2 ? NeedPipeline::Cache : pipe_how == 3 ? NeedPipeline::Compiled : NeedPipeline::Made;
    static const char* const prog_names[] = {"ready", "loaded", "translated", "failed"};
    static const char* const pipe_names[] = {"none", "ready", "cache", "compiled", "made", "left_out"};
    const bool needy = np == NeedProgram::Loaded || np == NeedProgram::Translated || pp == NeedPipeline::Cache ||
                       pp == NeedPipeline::Compiled || pp == NeedPipeline::Made || pp == NeedPipeline::LeftOut;
    Life l;
    bool known = false;
    {
        std::lock_guard lk(life_mu);
        const auto it = lives.find(serial);
        if (it != lives.end()) {
            if (it->second.used) return;   // (another thread's first use of it came first)
            it->second.used = true;
            l = it->second, known = true;
        }
    }
    census.used++;
    (np == NeedProgram::Ready ? census.used_ready : np == NeedProgram::Loaded ? census.used_loaded
     : np == NeedProgram::Translated ? census.used_translated : census.used_failed)++;
    (pp == NeedPipeline::None ? census.pipe_none : pp == NeedPipeline::Ready ? census.pipe_ready
     : pp == NeedPipeline::Cache ? census.pipe_cache : pp == NeedPipeline::Compiled ? census.pipe_compiled
     : pp == NeedPipeline::Made ? census.pipe_made : census.pipe_left_out)++;
    const uint64_t* codes = p ? p->codes : nullptr;
    if (!known) {
        census.used_unknown++;
        logf("vk: program life: first %s serial %llx t %.3f created unknown program %s pipeline %s ms %.2f key %016llx variant "
             "%016llx", compute ? "dispatch" : "draw", (unsigned long long)serial, secs(t0), prog_names[(int)np],
             pipe_names[(int)pp], ms_between(t0, t1), (unsigned long long)(p ? p->key : 0), (unsigned long long)(p ? p->variant : 0));
        return;
    }
    const double after = ms_between(l.created, t0);
    (after < 16.7 ? census.after_16 : after < 100.0 ? census.after_100 : after < 1000.0 ? census.after_1000 : census.after_more)++;
    // (the stages a draw reads are those of the create call: the keys of the program it drew with)
    const bool same = !codes || !codes[0] || !l.ok ||
                      (codes[0] == l.codes[0] && (l.compute || (codes[1] == l.codes[1] && codes[2] == l.codes[2])));
    census.used_codes_differ += !same;
    const bool long_frame = l.frame_ms >= kLoadGapMs;
    if (needy) census.needy++, census.needy_after_100 += after >= 100.0, census.needy_in_load += l.since_present >= kLoadGapMs || long_frame;
    logf("vk: program life: first %s serial %llx t %.3f after_create_ms %.1f create_since_present_ms %.1f create_frame_ms %.1f "
         "codes_same %d program %s pipeline %s ms %.2f key %016llx variant %016llx freed %d",
         compute ? "dispatch" : "draw", (unsigned long long)serial, secs(t0), after, l.since_present, l.frame_ms, (int)same,
         prog_names[(int)np], pipe_names[(int)pp], ms_between(t0, t1), (unsigned long long)(p ? p->key : 0),
         (unsigned long long)(p ? p->variant : 0), (int)l.is_freed);
}

void programs_census_log(const char* when) {
    if (!programs_life_on() && !programs_load_at_create()) return;
    const ProgramCensus& c = census;
    int64_t skips = 0, waits = 0;
    double wait_ms = 0;
    draw_stats3(&skips, &waits, &wait_ms);
    const auto n = [](const std::atomic<int64_t>& a) { return (long long)a.load(); };
    logf("vk: program census (%s): created %lld (%lld with stages; %lld with no present for 100 ms before, %lld in a frame of 100 ms or "
         "more; %.1f ms of the game's threads), freed %lld (%lld never used); first uses %lld: program ready %lld, loaded %lld, "
         "translated %lld, failed %lld; pipeline ready %lld, from a cache %lld, compiled %lld, made %lld, left out %lld, none %lld; "
         "%lld with no create call seen, %lld whose stages are not the create call's; after the create call: under 16.7 ms %lld, "
         "under 100 ms %lld, under 1 s %lld, longer %lld; needing work %lld (%lld of them 100 ms or more after the create call, %lld "
         "created in a load or a long frame); in play: %lld program records loaded (%.1f ms), %lld new graphics pipelines, pipelines "
         "from the driver's cache %lld, from the start's cache %lld, compiled %lld; %lld draws and dispatches left out, %lld waits "
         "(%.1f ms)",
         when, n(c.created), n(c.created_ok), n(c.created_in_load), n(c.created_long_frame), c.create_ns.load() / 1e6, n(c.freed),
         n(c.freed_undrawn), n(c.used), n(c.used_ready), n(c.used_loaded), n(c.used_translated), n(c.used_failed), n(c.pipe_ready),
         n(c.pipe_cache), n(c.pipe_compiled), n(c.pipe_made), n(c.pipe_left_out), n(c.pipe_none), n(c.used_unknown),
         n(c.used_codes_differ), n(c.after_16), n(c.after_100), n(c.after_1000), n(c.after_more), n(c.needy), n(c.needy_after_100),
         n(c.needy_in_load), n(c.play_loads), c.play_load_ns.load() / 1e6, n(c.play_pipes_new), n(c.play_from_driver),
         n(c.play_from_start), n(c.play_compiled), (long long)skips, (long long)waits, wait_ms);
    if (!programs_load_at_create()) return;
    double median = 0, p99 = 0, worst = 0;
    {
        std::lock_guard lk(load_ms_mu);
        if (!load_ms.empty()) {
            std::vector<float> v = load_ms;
            std::sort(v.begin(), v.end());
            median = v[v.size() / 2], p99 = v[std::min(v.size() - 1, v.size() * 99 / 100)], worst = v.back();
        }
    }
    size_t units = 0, ready = 0, loading = 0, alive = 0;
    {
        std::lock_guard lk(life_mu);
        units = table.units(), ready = table.count(program_life::State::Ready), loading = table.count(program_life::State::Loading);
        alive = table.programs();
    }
    logf("vk: program loads at create (%s): %lld queued, %lld loaded (median %.2f ms, 99th percentile %.2f ms, worst %.2f ms, %.0f "
         "ms and %.1f MB read in all); %lld programs (%lld of them compute variants without their code, %lld graphics programs no note names), %lld graphics pipelines (%lld not in a cache), %lld compute pipelines (%lld "
         "not in a cache); a first use ran %lld itself and waited for %lld (%.1f ms); %lld pipelines made at create taken by "
         "draws; %lld create calls with nothing kept for their stages, %lld before the index; units %zu (%zu ready, %zu "
         "loading), %zu programs alive",
         when, n(c.units_queued), n(c.units_loaded), median, p99, worst, c.load_ns.load() / 1e6, c.load_bytes.load() / 1048576.0,
         n(c.load_programs), n(c.load_light), n(c.load_unlisted), n(c.load_pipes), n(c.load_pipes_cold), n(c.load_computes), n(c.load_computes_cold),
         n(c.units_run_by_draw), n(c.units_waited), c.unit_wait_ns.load() / 1e6, n(c.pipes_taken), n(c.units_nothing),
         n(c.units_early), units, ready, loading, alive);
    if (ids_on.load(std::memory_order_acquire))
        logf("vk: module identifiers (%s): %zu known, %lld learned in this run; %lld compute pipelines made by them at a create "
             "call, %lld asked that way were in no cache (their code read, as before)",
             when, ids.size(), n(ids_learned.load()), n(c.load_by_id), n(c.load_by_id_cold));
}

} // namespace mkwvk
