// Authored fixture: execute production precompile graphics publication with no GPU.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkPipeline = uint64_t;
constexpr VkPipeline VK_NULL_HANDLE = 0;
using VkPrimitiveTopology = unsigned;
#include "pipe_entry.inc"
static thread_local bool note_locked = false;
struct CacheMutex {
    std::mutex mutex;
    void lock() { mutex.lock(); note_locked = true; }
    void unlock() { note_locked = false; mutex.unlock(); }
} note_mu;
static std::unordered_map<uint64_t, std::unique_ptr<PipeEntry>> pipelines;
static std::unordered_map<VkPipeline, unsigned> destroyed;
static std::unordered_set<VkPipeline> live, in_flight;
static struct { unsigned device = 7; } dev;
struct Program {};
struct PipeNote { uint64_t key = 42; unsigned s = 3, topo = 4; };
struct Pre { std::mutex first_graphics; std::atomic<bool> graphics_begun{false}; };
static std::mutex gate_mu;
static std::condition_variable gate;
static bool delay_build = false, building = false, resume_build = false;
static VkPipeline next_pipeline = 100;
static unsigned builds = 0;
static unsigned note_targets(const PipeNote&) { return 5; }
static VkPipeline build_graphics(const Program&, unsigned s, unsigned targets, VkPrimitiveTopology topo) {
    CHECK(s == 3 && targets == 5 && topo == 4);
    CHECK(!note_locked); // Drivers may take arbitrarily long; do not hold the cache lock.
    ++builds;
    if (delay_build) {
        std::unique_lock lock(gate_mu);
        building = true;
        gate.notify_all();
        gate.wait(lock, [] { return resume_build; });
    }
    if (next_pipeline) CHECK(live.insert(next_pipeline).second);
    return next_pipeline;
}
void vkDestroyPipeline(unsigned device, VkPipeline pipeline, const void* allocator) {
    CHECK(device == 7 && allocator == nullptr);
    CHECK(note_locked); // Collision ownership is resolved under the publication lock.
    CHECK(pipeline != VK_NULL_HANDLE);
    CHECK(in_flight.count(pipeline) == 0);
    for (const auto& item : pipelines) CHECK(item.second->pipeline.load() != pipeline);
    CHECK(live.erase(pipeline) == 1);
    CHECK(++destroyed[pipeline] == 1);
}
static void run(Pre* pre, const Program* p, PipeNote& n) {
#include "precompile_graphics.inc"
}
static PipeEntry* occupied(bool ready, VkPipeline pipeline = 900) {
    auto entry = std::make_unique<PipeEntry>();
    entry->pipeline = pipeline;
    entry->ready = ready;
    auto* result = entry.get();
    pipelines.emplace(42, std::move(entry));
    if (pipeline) live.insert(pipeline), in_flight.insert(pipeline);
    return result;
}
static void preserved(PipeEntry* old, bool ready, VkPipeline pipeline = 900) {
    CHECK(pipelines.size() == 1 && pipelines.at(42).get() == old);
    CHECK(old->pipeline.load() == pipeline && old->ready.load() == ready);
    CHECK(destroyed.count(pipeline) == 0);
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string mode = argv[1];
    Pre pre;
    Program program;
    PipeNote note;
    if (mode == "insert" || mode == "null-insert") {
        if (mode == "null-insert") next_pipeline = 0;
        run(&pre, &program, note);
        CHECK(pipelines.size() == 1 && pipelines.at(42)->ready.load());
        CHECK(pipelines.at(42)->pipeline.load() == next_pipeline);
        CHECK(destroyed.empty());
        CHECK(live.size() == (next_pipeline ? 1u : 0u));
    } else if (mode == "delayed-worker") {
        delay_build = true;
        std::thread worker([&] { run(&pre, &program, note); });
        {
            std::unique_lock lock(gate_mu);
            gate.wait(lock, [] { return building; });
        }
        // Play publishes an in-flight fast pipeline while precompile is stuck in the driver.
        PipeEntry* old;
        { std::lock_guard lock(note_mu); old = occupied(false); }
        { std::lock_guard lock(gate_mu); resume_build = true; }
        gate.notify_all();
        worker.join();
        preserved(old, false);
        CHECK(destroyed[100] == 1 && live.count(100) == 0);
        CHECK(live.size() == 1);
    } else {
        const bool ready = mode != "pending";
        PipeEntry* old = occupied(ready);
        if (mode == "null-collision") next_pipeline = 0;
        run(&pre, &program, note);
        preserved(old, ready);
        if (mode == "null-collision") CHECK(destroyed.empty());
        else CHECK(destroyed[100] == 1 && live.count(100) == 0);
        if (mode == "duplicate") {
            next_pipeline = 101;
            run(&pre, &program, note);
            preserved(old, ready);
            CHECK(destroyed[100] == 1 && destroyed[101] == 1);
        }
        CHECK(live.size() == 1);
    }
    CHECK(builds == (mode == "duplicate" ? 2u : 1u));
    CHECK(pre.graphics_begun.load());
    std::puts("PASS precompile pipeline ownership");
}
