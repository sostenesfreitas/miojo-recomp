#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
// rx_nvn.h is shared with C; GCC's C++ frontend needs its C assertion spelling mapped.
#define _Static_assert static_assert
#include "gpu/vk/temporal_jitter.h"
#define dlss_jitter_clip original_dlss_jitter_clip
#include "gpu/vk/dlss_math.h"
#undef dlss_jitter_clip

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
using LONG = int;
using mkwvk::TemporalJitter;
/* HOST_CACHE_LOCKED */
using SRWLOCK = std::mutex;
#define SRWLOCK_INIT {}
std::atomic<int> host_lock_entries{0};
std::mutex host_schedule_mu;
std::condition_variable host_schedule_cv;
bool older_waiting = false, release_older = false;
thread_local bool delay_host_lock = false;
void AcquireSRWLockExclusive(SRWLOCK* lock) {
    host_lock_entries++;
    if (delay_host_lock) {
        std::unique_lock schedule(host_schedule_mu);
        older_waiting = true;
        host_schedule_cv.notify_all();
        host_schedule_cv.wait(schedule, [] { return release_older; });
    }
    lock->lock();
}
void ReleaseSRWLockExclusive(SRWLOCK* lock) { lock->unlock(); }
static_assert(MKWVK_API_VERSION >= 14);
static_assert(offsetof(MkwVkRenderer, jitter_sample) > offsetof(MkwVkRenderer, option_set));
std::atomic<uint64_t> clock_frame{100};
bool env_off = false;
const char* env_off_name = nullptr;   // (one switch set to 0 alone: "fsr-jitter-off" below)
uint64_t texture_frame_no() { return clock_frame.load(); }
int env_switch(const char* name) { return env_off || (env_off_name && std::strcmp(name, env_off_name) == 0) ? 0 : -1; }
std::atomic<bool> dlss_on{true}, fsr_on{false};
std::atomic<bool> fsr4_on{false}, fsr3_chain_on{false};
namespace ur { enum { GAME=0, DLSS=1, FSR3=3, FSR4=4 }; bool is_fsr4(int mode) { return mode==FSR4; } }
int temporal_mode() { return fsr3_chain_on ? ur::FSR3 : fsr4_on ? ur::FSR4 : dlss_on ? ur::DLSS : ur::GAME; }
bool fsr3_wanted() { return fsr_on.load(); }

namespace gpu {
struct State {
    struct Engine { std::atomic<bool> failed{false}; } engines[3];   // (DLSS, FSR 4, FSR 3.1 on the chain)
    std::atomic<uint64_t> last_frame{100};
    TemporalJitter jitter;
    // Baseline adapters use the old fields, allowing the unchanged baseline to compile and fail causally.
    std::atomic<uint32_t> jitter_w{0}, jitter_h{0}, jitter_phases{8};
} state;
State& st() { return state; }
void publish(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
    State& s = st();
    const uint64_t now = texture_frame_no();
    s.last_frame = now;
    /* DLSS_PUBLISH */
}
/* DLSS_READER */

namespace fsr3_inputs {
struct State {
    TemporalJitter jitter;
    std::atomic<uint64_t> upscaled_frame{0};
    std::atomic<uint32_t> jitter_w{0}, jitter_h{0}, jitter_phases{8};
} state;
State& st() { return state; }
/* FSR_PUBLISH */
/* FSR_READER */
}
/* REPLAY_CALLBACK */
}

static std::atomic<int> callbacks{0}, scalar_queries{0};
static bool interleave = true;
std::mutex callback_mu;
std::condition_variable callback_cv;
bool block_callback = false, callback_entered = false, allow_callback = false;
void callback(MkwVkJitter* out) {
    callbacks++;
    if (block_callback) {
        std::unique_lock lock(callback_mu);
        callback_entered = true;
        callback_cv.notify_all();
        callback_cv.wait(lock, [] { return allow_callback; });
    }
    gpu::jitter_sample(out);
    if (interleave) { interleave = false; gpu::publish(1720, 720, 3440, 1440); }
}
int rx_option(int id, int) {
    scalar_queries++;
    const int value = (int)gpu::temporal_jitter_size(id - HOS_OPT_DLSS_JITTER_W);
    if (interleave) { interleave = false; gpu::publish(1720, 720, 3440, 1440); }
    return value;
}
LONG presents = 100;
volatile float hos_wide_ratio = 16.0f / 9.0f;
float projection_matrix[16]{};
#define GP(p) ((void*)(uintptr_t)(p))
MkwVkRenderer table{};
const MkwVkRenderer* vk = &table;
thread_local MkwVkJitter observed{};
int dlss_jitter_clip(const float*, float, unsigned, int phases, float width, float height, float out[2]) {
    observed = {(uint32_t)width, (uint32_t)height, (uint32_t)phases};
    out[0] = out[1] = 0;
    return width && height && phases;
}
/* HOST_TRANSPORT */

bool equal(MkwVkJitter a, MkwVkJitter b) {
    return a.width == b.width && a.height == b.height && a.phases == b.phases;
}
void host_read() {
    float out[2];
    hos_dlss_jitter_clip((uint64_t)(uintptr_t)projection_matrix, out);
}

int main(int argc, char** args) {
    env_off = argc > 1 && std::strcmp(args[1], "off") == 0;
    table.jitter_sample = callback;
    gpu::publish(1920, 1080, 1920, 1080);
    gpu::fsr3_inputs::upscaled(1720, 720, 3440, 1440);
    if (argc > 1 && std::strcmp(args[1], "late-host") == 0) {
        // An older invocation reaches the lock after a newer host frame filled its cache.
        // It must not roll the cache back and make two reads of frame101 query different samples.
        interleave = false;
        MkwVkJitter older_sample{};
        std::thread older([&] { delay_host_lock = true; host_read(); older_sample = observed; });
        {
            std::unique_lock schedule(host_schedule_mu);
            CHECK(host_schedule_cv.wait_for(schedule, std::chrono::seconds(5), [] { return older_waiting; }));
        }
        presents = 101;
        host_read();
        const MkwVkJitter first_sample = observed;
        gpu::publish(1720, 720, 3440, 1440);
        { std::lock_guard schedule(host_schedule_mu); release_older = true; }
        host_schedule_cv.notify_all();
        older.join();
        host_read();
        const MkwVkJitter last_sample = observed;
        CHECK(equal(first_sample, last_sample));
        CHECK(equal(older_sample, first_sample));
        CHECK(callbacks == 1);
        std::puts("PASS: delayed host caller cannot roll a newer frame cache back");
        return 0;
    }
    if (argc > 1 && std::strcmp(args[1], "parallel-host") == 0) {
        // The first caller is inside the renderer query after the host marked its cache frame.
        // A second caller must not consume the not-yet-filled cache or query another sample.
        interleave = false;
        block_callback = true;
        MkwVkJitter first_sample{}, second_sample{};
        std::thread first([&] { host_read(); first_sample = observed; });
        {
            std::unique_lock lock(callback_mu);
            CHECK(callback_cv.wait_for(lock, std::chrono::seconds(5), [] { return callback_entered; }));
        }
        std::thread second([&] { host_read(); second_sample = observed; });
#if HOST_CACHE_LOCKED
        while (host_lock_entries.load() < 2) std::this_thread::yield();
#else
        second.join(); // Unlocked baseline returns the partially published cache while the first query is held.
#endif
        { std::lock_guard lock(callback_mu); allow_callback = true; }
        callback_cv.notify_all();
        first.join();
#if HOST_CACHE_LOCKED
        second.join();
#endif
        CHECK(equal(first_sample, {1920, 1080, 8}));
        CHECK(equal(second_sample, first_sample));
        CHECK(callbacks == 1);
        std::puts("PASS: concurrent host callers share one completed frame sample");
        return 0;
    }
    if (env_off) {
        CHECK(equal(gpu::temporal_jitter_sample(), {}));
        fsr_on = true;
        CHECK(equal(gpu::fsr3_inputs::jitter_sample(), {}));
        std::puts("PASS: explicit jitter-off paths are inactive");
        return 0;
    }
    if (argc > 1 && std::strcmp(args[1], "fsr-jitter-off") == 0) {
        // RX_VK_FSR_JITTER=0 alone: FSR 3.1 on the chain gets no jitter, DLSS and FSR 4 keep theirs
        env_off_name = "RX_VK_FSR_JITTER";
        host_read();
        presents++;
        host_read();
        CHECK(equal(observed, {1720, 720, 32}));
        fsr3_chain_on = true;
        CHECK(equal(gpu::temporal_jitter_sample(), {}));
        fsr3_chain_on = false;
        CHECK(equal(gpu::temporal_jitter_sample(), {1720, 720, 32}));
        dlss_on = false; fsr4_on = true;
        CHECK(equal(gpu::temporal_jitter_sample(), {1720, 720, 32}));
        std::puts("PASS: RX_VK_FSR_JITTER=0 turns off FSR 3.1's jitter on the chain only");
        return 0;
    }
    host_read();
    CHECK(equal(observed, {1920, 1080, 8})); // A whole sample, even when B is published before the callback returns.
    CHECK(callbacks == 1 && scalar_queries == 0);
    for (int i = 0; i < 100; i++) host_read();
    CHECK(callbacks == 1 && equal(observed, {1920, 1080, 8}));
    presents++;
    host_read();
    CHECK(equal(observed, {1720, 720, 32}) && callbacks == 2);
    // The same completed producer is available to FSR4, with its own failure gate.
    dlss_on = false; fsr4_on = true;
    CHECK(equal(gpu::temporal_jitter_sample(), {1720, 720, 32}));
    gpu::state.engines[1].failed = true;
    CHECK(equal(gpu::temporal_jitter_sample(), {}));
    gpu::state.engines[1].failed = false;
    fsr4_on = false;
    // FSR 3.1 on the chain (vk_fsr3.h fsr3_on_chain) takes the same producer too, behind its own failure gate (slot 2)
    fsr3_chain_on = true;
    CHECK(equal(gpu::temporal_jitter_sample(), {1720, 720, 32}));
    gpu::state.engines[2].failed = true;
    CHECK(equal(gpu::temporal_jitter_sample(), {}));
    gpu::state.engines[1].failed = true;   // (another engine's gate is not FSR 3.1's)
    gpu::state.engines[2].failed = false;
    CHECK(equal(gpu::temporal_jitter_sample(), {1720, 720, 32}));
    gpu::state.engines[1].failed = false;
    fsr3_chain_on = false;
    dlss_on = false; fsr_on = true;
    gpu::fsr3_inputs::upscaled(2293, 960, 3440, 1440);
    presents++; host_read();
    CHECK(equal(observed, {2293, 960, 18}));
    dlss_on = true; fsr_on = false;
    gpu::state.engines[0].failed = true;
    CHECK(equal(gpu::temporal_jitter_sample(), {}));
    gpu::state.engines[0].failed = false;
    gpu::state.last_frame = 0;
    CHECK(equal(gpu::temporal_jitter_sample(), {}));
    gpu::publish(1920, 1080, 1920, 1080);
    clock_frame = 130;
    CHECK(equal(gpu::temporal_jitter_sample(), {1920, 1080, 8}));
    clock_frame = 131;
    CHECK(equal(gpu::temporal_jitter_sample(), {}));
    fsr_on = true;
    CHECK(equal(gpu::fsr3_inputs::jitter_sample(), {}));
    vk = nullptr; presents++; host_read();
    CHECK(equal(observed, {})); // WebGPU has no temporal-jitter callback.
    vk = &table; table.jitter_sample = nullptr; presents++; host_read();
    CHECK(equal(observed, {}));
    TemporalJitter invalid;
    CHECK(equal(invalid.read(1), {}));
    invalid.publish(0, 1080, 8, 1);
    CHECK(equal(invalid.read(1), {}));
    invalid.publish(1920, 0, 8, 1);
    CHECK(equal(invalid.read(1), {}));
    invalid.publish(1920, 1080, 0, 1);
    CHECK(equal(invalid.read(1), {}));
    invalid.publish(1920, 1080, 8, 0);
    CHECK(equal(invalid.read(1), {}));
    // Exercise actual producer fragments and readers concurrently, not only the standalone mailbox.
    clock_frame = 700; dlss_on = true; fsr_on = true;
    gpu::publish(1920, 1080, 1920, 1080);
    gpu::fsr3_inputs::upscaled(1920, 1080, 1920, 1080);
    std::thread writer([] {
        for (int i = 0; i < 100000; i++) {
            if (i & 1) { gpu::publish(1920, 1080, 1920, 1080); gpu::fsr3_inputs::upscaled(1920, 1080, 1920, 1080); }
            else { gpu::publish(1720, 720, 3440, 1440); gpu::fsr3_inputs::upscaled(1720, 720, 3440, 1440); }
        }
    });
    std::vector<std::thread> readers;
    for (int n = 0; n < 3; n++) readers.emplace_back([] {
        for (int i = 0; i < 100000; i++) {
            const auto a = gpu::temporal_jitter_sample(), b = gpu::fsr3_inputs::jitter_sample();
            CHECK(equal(a, {1920, 1080, 8}) || equal(a, {1720, 720, 32}));
            CHECK(equal(b, {1920, 1080, 8}) || equal(b, {1720, 720, 32}));
        }
    });
    writer.join();
    for (auto& reader : readers) reader.join();
    std::puts("PASS: actual jitter transport, frame latch, providers, freshness, fallback and 100000 concurrent publications");
}
