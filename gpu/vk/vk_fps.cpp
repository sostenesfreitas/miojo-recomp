// (hookshot) The FPS counter's measurements (vk_fps.h): the game's frames, present to present on the thread that presents,
// into a ring of the last seconds; from Breakdown on, where each frame's time went. OoT simulates on its main thread and
// records its frame on its presentation thread, which presents: the parts are that thread's. The renderer's time is its
// entries' (submit, present, finish, which replay on the calling thread), less the waits inside them; the waits for the GPU
// are the recording slots', the fences', the swapchain's frame fence, OoT's own present poll (the host's, MkwVkHost
// slept_ns) and a present that waited for the frame's copy (NVIDIA on Linux: vk_replay.cpp present_out; the presenter
// thread's wait for it); the pacing is the window acquire's sleep, the game's other sleeps, vsync (the swapchain's acquire
// and present) and Reflex's or Anti-Lag 2's wait; the game is the rest, its own code on that thread and its waits for its
// main thread.
#include "vk_fps.h"

#include <algorithm>
#include <mutex>

#include "vk_draw.h"
#include "vk_internal.h"

namespace mkwvk {

std::atomic<int> fps_level_now{0};
int64_t gpu_time_ns();     // (vk_replay.cpp: the GPU's time in the timed recordings so far)
int64_t slot_wait_ns();    // (and the replay's waits for a recording slot: inside the renderer's entries, on this thread)

namespace {

namespace fs = fps_stats;

struct ThreadTimes {
    int64_t render = 0;   // (in the renderer's entries, their waits taken out)
    int64_t wait[fs::kParts] = {};
    int64_t waits = 0;    // (every wait: what an entry takes out)
    int depth = 0;        // (entries open: only the outermost counts)
};
thread_local ThreadTimes tt;

std::mutex ring_mu;
fs::Ring<fs::kRing> ring;

}   // namespace

int64_t fps_clock() { return prof_now(); }

void fps_wait(int part, int64_t ns) {
    if (ns <= 0 || part < 0 || part >= fs::kParts) return;
    tt.wait[part] += ns;
    tt.waits += ns;
}

FpsRender::FpsRender() : on(fps_breakdown_on()) {
    if (!on || tt.depth++) return;
    t0 = fps_clock(), w0 = tt.waits;
}
FpsRender::~FpsRender() {
    if (!on || --tt.depth) return;
    tt.render += fps_clock() - t0 - (tt.waits - w0);
}

void fps_presented() {
    const int level = fps_level_now.load(std::memory_order_relaxed);
    // (the last frame's state, for the next one: on the presenting thread only)
    static int64_t last_at = 0, last_render = 0, last_wait[fs::kParts] = {}, last_slept[2] = {}, last_gpu = 0, last_pipes = 0,
                   last_left = 0, last_slot = 0;
    static int last_level = 0;
    {   // (the host times its sleeps only while the Breakdown shows: told when that changes)
        static int told = 0;
        const int on = level >= fs::kBreakdown;
        if (on != told && host->fps_timing) host->fps_timing(on), told = on;
    }
    if (level < fs::kFrameTime) {
        last_at = 0, last_level = level;
        return;
    }
    const int64_t now = fps_clock();
    const bool breakdown = level >= fs::kBreakdown;
    int64_t slept[2] = {0, 0}, gpu = 0, pipes = 0, left = 0, slot = 0;
    if (breakdown) {
        if (host->slept_ns) host->slept_ns(slept);
        gpu = gpu_time_ns(), slot = slot_wait_ns();
        int64_t d = 0, sk = 0, di = 0, waits = 0;
        double wait_ms = 0;
        draw_stats(&d, &sk, &di, &pipes);
        draw_stats3(&left, &waits, &wait_ms);
    }
    if (last_at) {
        fs::Frame f;
        f.at_ns = now;
        const double ms = (now - last_at) / 1e6;
        f.ms = (float)ms;   // (the Breakdown's first frame: its time, without parts)
        if (breakdown && last_level >= fs::kBreakdown) {   // (the parts from the Breakdown's first whole frame on)
            const auto d = [](int64_t a, int64_t b) { return std::max<int64_t>(a - b, 0) / 1e6; };
            // (the recording slots' waits are inside the renderer's entries, timed by the replay itself: moved to the GPU's)
            const double slots = d(slot, last_slot);
            fs::split(f, ms, d(tt.render, last_render) - slots,
                      d(tt.wait[fs::kGpuWait], last_wait[fs::kGpuWait]) + d(slept[0], last_slept[0]) + slots,
                      d(tt.wait[fs::kPace], last_wait[fs::kPace]) + d(slept[1], last_slept[1]));
            f.gpu_ms = (float)d(gpu, last_gpu);
            f.pipelines = (uint16_t)std::clamp<int64_t>(pipes - last_pipes, 0, 65535);
            f.left_out = (uint16_t)std::clamp<int64_t>(left - last_left, 0, 65535);
        }
        if (f.ms > 0) {   // (two presents in the same clock tick: not a frame)
            std::lock_guard lk(ring_mu);
            ring.push(f);
        }
    }
    last_at = now, last_level = level;
    last_render = tt.render, last_gpu = gpu, last_pipes = pipes, last_left = left, last_slot = slot;
    for (int p = 0; p < fs::kParts; p++) last_wait[p] = tt.wait[p];
    last_slept[0] = slept[0], last_slept[1] = slept[1];
}

fs::Summary fps_summary(int64_t now, int64_t window, int64_t low_window) {
    std::lock_guard lk(ring_mu);
    return fs::summarize(ring, now, window, low_window);
}

float fps_graph(int64_t now, int count, fs::Frame* frames, bool* marks) {
    static thread_local int idx[512];
    count = std::min(count, 512);
    float slowest = 0;
    std::lock_guard lk(ring_mu);
    fs::columns(ring, now, fs::kGraphWindow, count, idx, marks);
    for (int c = 0; c < count; c++) {
        frames[c] = idx[c] >= 0 ? ring.back(idx[c]) : fs::Frame{};
        slowest = std::max(slowest, frames[c].ms);
    }
    return slowest;
}

} // namespace mkwvk
