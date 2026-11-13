// (hookshot) The FPS counter's measurements (vk_fps.cpp): each frame of the game, present to present on the thread that
// presents, and from Breakdown on where its time went (fps_stats.h Part). vk_menu.cpp sets the level and draws them.
// Off and FPS measure nothing here; Frame time and Graph read a clock at each present; Breakdown adds a clock read at the
// renderer's entries and around its waits, and the counters the renderer keeps anyway (the GPU's timestamps, the pipelines
// made, the draws left out). Nothing waits for the GPU's timestamps: they are read when their recording slot comes round.
#pragma once

#include <atomic>
#include <cstdint>

#include "fps_stats.h"

namespace mkwvk {

/// What the counter shows (fps_stats::Level): set by the menu, read at every present
extern std::atomic<int> fps_level_now;
inline bool fps_breakdown_on() { return fps_level_now.load(std::memory_order_relaxed) >= fps_stats::kBreakdown; }

int64_t fps_clock();   // (ns, the clock the frames are stamped with)
/// At the game's present (vk_replay.cpp present), on the thread that presents: the frame since the last one, into the ring
void fps_presented();
/// A wait on this thread (fps_stats::kGpuWait or kPace), ns: subtracted from the renderer's time it is inside
void fps_wait(int part, int64_t ns);
/// The renderer's time on this thread, at its entries (submit, present, finish): what the waits inside leave
struct FpsRender {
    int64_t t0 = 0, w0 = 0;
    bool on;
    FpsRender();
    ~FpsRender();
};
/// A wait timed into a part (around a call that may block): nothing while the Breakdown is off
struct FpsWait {
    int part;
    int64_t t0;
    explicit FpsWait(int p) : part(p), t0(fps_breakdown_on() ? fps_clock() : 0) {}
    ~FpsWait() {
        if (t0) fps_wait(part, fps_clock() - t0);
    }
};

/// The numbers over the last window and the 1% low over the last low_window (fps_stats::summarize), and the graph's
/// columns, under the ring's lock
fps_stats::Summary fps_summary(int64_t now, int64_t window, int64_t low_window);
/// count columns: per column its slowest frame (copied into frames[c], ms 0 for none) and whether it built pipelines or left
/// draws out; returns the slowest frame time shown
float fps_graph(int64_t now, int count, fps_stats::Frame* frames, bool* marks);

} // namespace mkwvk
