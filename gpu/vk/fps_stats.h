/* (hookshot) The FPS counter's levels and its numbers (vk_fps.cpp measures, vk_menu.cpp draws): the row's values, the frames
   of the last seconds in a ring, their averages, the 1% low, the graph's columns and its scale. Plain functions:
   tests/test_fps_stats.cpp checks them. */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace mkwvk::fps_stats {

/// The FPS counter row (opts.show_fps, saved as fps_counter=): an options file from before the levels has 0 Off or 1 On,
/// and 1 is the counter as it was, the number alone
enum Level { kOff, kFps, kFrameTime, kGraph, kBreakdown, kLevels };
inline int level_of(int saved) { return std::clamp(saved, (int)kOff, (int)kLevels - 1); }
inline const char* level_name(int v) {
    static const char* const t[] = {"Off", "FPS", "Frame time", "Graph", "Breakdown"};
    return t[level_of(v)];
}

/// The FPS counter position row (opts.fps_corner): bit 0 the left side, bit 1 the bottom
inline const char* corner_name(int v) {
    static const char* const t[] = {"Top right", "Top left", "Bottom right", "Bottom left"};
    return t[std::clamp(v, 0, 3)];
}

/// Where a frame's time went, on the thread that presents (the parts add up to the frame's time): the game's code (what
/// is left), the renderer's recording and submitting, waits for the GPU (a recording slot, a fence, OoT's own poll for its
/// frame's GPU work), and the wait for the frame's turn (the frame pacing, the game's own sleeps, vsync)
enum Part { kGame, kRender, kGpuWait, kPace, kParts };

struct Frame {
    int64_t at_ns = 0;          // (its present)
    float ms = 0;               // (present to present)
    float part[kParts] = {};    // (ms)
    float gpu_ms = 0;           // (the GPU's own time for the recordings timed since the frame before: a few frames late)
    uint16_t pipelines = 0;     // (pipelines made since the frame before: shaders built in play)
    uint16_t left_out = 0;      // (draws left out because their pipeline was not ready)
};

/// The parts of a frame of `ms`: what was measured, scaled down if it is more than the frame (a wait that began before the
/// frame did), and the game's the rest
inline void split(Frame& f, double ms, double render, double gpu_wait, double pace) {
    f.ms = (float)ms;
    render = std::max(render, 0.0), gpu_wait = std::max(gpu_wait, 0.0), pace = std::max(pace, 0.0);
    const double sum = render + gpu_wait + pace;
    const double k = sum > ms && sum > 0 ? std::max(ms, 0.0) / sum : 1.0;
    f.part[kRender] = (float)(render * k), f.part[kGpuWait] = (float)(gpu_wait * k), f.part[kPace] = (float)(pace * k);
    f.part[kGame] = (float)std::max(0.0, ms - (render + gpu_wait + pace) * k);
}

/// The last N frames (N a power of two): back(0) the newest
template <int N>
struct Ring {
    static_assert((N & (N - 1)) == 0, "a power of two");
    Frame f[N];
    uint64_t pushed = 0;
    void push(const Frame& x) { f[pushed++ & (N - 1)] = x; }
    int size() const { return (int)std::min<uint64_t>(pushed, N); }
    const Frame& back(int i) const { return f[(pushed - 1 - (uint64_t)i) & (N - 1)]; }
    void clear() { pushed = 0; }
};
constexpr int kRing = 16384;              // (10 s at 1600 fps: the 1% low's window whole with the cap Uncapped (1000))
constexpr int64_t kSecond = 1000000000;
constexpr int64_t kGraphWindow = 4 * kSecond;   // (the graph's span)
constexpr int64_t kLowWindow = 10 * kSecond;    // (the 1% low's)

/// The 1% low: the frame rate of the slowest 1% of these frames (at least one), as 1000 over their mean time. ms is
/// reordered. 0 for no frames
inline double one_percent_low(float* ms, int n) {
    if (n <= 0) return 0;
    const int k = std::max(1, (n + 99) / 100);
    std::nth_element(ms, ms + (n - k), ms + n);
    double sum = 0;
    for (int i = n - k; i < n; i++) sum += ms[i];
    return sum > 0 ? 1000.0 * k / sum : 0;
}

struct Summary {
    int frames = 0;            // (in the window)
    double avg_ms = 0;         // (the mean frame time over the window)
    double part[kParts] = {};  // (each part's mean)
    double gpu_ms = 0;         // (the GPU's mean time a frame)
    int pipelines = 0, left_out = 0;   // (summed over the window)
    double low_fps = 0;        // (the 1% low over the low window)
};

/// The frames presented in (now - window, now], and the 1% low over (now - low_window, now]
template <int N>
Summary summarize(const Ring<N>& r, int64_t now, int64_t window, int64_t low_window) {
    Summary s;
    static thread_local std::vector<float> low;
    low.clear();
    for (int i = 0; i < r.size(); i++) {
        const Frame& f = r.back(i);
        const int64_t age = now - f.at_ns;
        if (age >= std::max(window, low_window)) break;
        if (age < low_window) low.push_back(f.ms);
        if (age >= window) continue;
        s.frames++;
        s.avg_ms += f.ms;
        for (int p = 0; p < kParts; p++) s.part[p] += f.part[p];
        s.gpu_ms += f.gpu_ms;
        s.pipelines += f.pipelines, s.left_out += f.left_out;
    }
    if (s.frames) {
        s.avg_ms /= s.frames, s.gpu_ms /= s.frames;
        for (double& p : s.part) p /= s.frames;
    }
    s.low_fps = one_percent_low(low.data(), (int)low.size());
    return s;
}

/// The graph's columns, oldest left: column c holds the frames presented in its share of (now - window, now]; out[c] is
/// the back() index of its slowest frame, -1 for none; marks[c] whether one of its frames built pipelines or left draws out
template <int N>
void columns(const Ring<N>& r, int64_t now, int64_t window, int count, int* out, bool* marks) {
    for (int c = 0; c < count; c++) out[c] = -1, marks[c] = false;
    if (count <= 0 || window <= 0) return;
    for (int i = 0; i < r.size(); i++) {
        const Frame& f = r.back(i);
        const int64_t age = now - f.at_ns;
        if (age >= window) break;
        if (age < 0) continue;
        const int c = std::clamp(count - 1 - (int)(age * count / window), 0, count - 1);
        if (out[c] < 0 || f.ms > r.back(out[c]).ms) out[c] = i;
        if (f.pipelines || f.left_out) marks[c] = true;
    }
}

/// The Game frame rate cap row's rate (0 60, then 90, 120, 144, 165, 240; 6 Uncapped: 0, no rate; 7 30); HOS_FPS_MAX over it
/// (env_max, 0 when not set: host/nn_vi.c fps_max's rules, 1000 and over is uncapped)
inline int cap_rate(int fps_cap, int env_max = 0) {
    static const int rates[] = {60, 90, 120, 144, 165, 240, 0, 30};
    if (env_max > 0) return env_max == 30 ? 30 : env_max >= 1000 ? 0 : std::max(env_max, 60);
    return fps_cap >= 0 && fps_cap <= 7 ? rates[fps_cap] : 60;   // (a value this build does not know: 60, as the host)
}
/// The frame time the game aims for, in ms: the cap's, or with V-Sync On (FIFO, no frame generation) the display's refresh
/// when that is slower; 0 when there is none (Uncapped, without V-Sync)
inline double target_ms_for(int cap, bool vsync_on, int refresh_hz) {
    int rate = cap;
    if (vsync_on && refresh_hz > 0) rate = rate ? std::min(rate, refresh_hz) : refresh_hz;
    return rate > 0 ? 1000.0 / rate : 0;
}
inline double target_ms(int fps_cap) { return target_ms_for(cap_rate(fps_cap), false, 0); }
/// The graph's top, in ms: twice the target, doubled until the usual frame fits (typical: a high percentile of the columns,
/// not the slowest, so one spike does not flatten the rest), four times the target at most. With no target (Uncapped):
/// from 4 ms, doubled until half again the usual frame fits, 100 at most. A frame past the top is drawn at it, and marked
inline double graph_top(double target, double typical) {
    if (target <= 0) {
        double top = 4;
        while (top < typical * 1.5 && top < 100) top *= 2;
        return std::min(top, 100.0);
    }
    double top = 2 * target;
    while (top < typical * 1.1 && top < 4 * target) top *= 2;
    return std::min(top, 4 * target);
}
/// The graph's typical frame: the 90th percentile of the columns' frame times (0 for none). ms is reordered
inline double typical_ms(float* ms, int n) {
    if (n <= 0) return 0;
    const int k = std::min(n - 1, n * 9 / 10);
    std::nth_element(ms, ms + k, ms + n);
    return ms[k];
}
/// The counter's colour for a frame rate against the cap's: 0 at it (5% under counts), 1 under it, 2 at under two thirds.
/// No target (Uncapped): against the console's 60
inline int rate_grade(double fps, double target_ms) {
    const double want = target_ms > 0 ? 1000.0 / target_ms : 60.0;
    return fps >= want * 0.95 ? 0 : fps >= want * 2 / 3 ? 1 : 2;
}

/// The text lines: "16.7 ms", "1% low 52"
inline int frame_time_text(char* b, size_t n, double ms) { return snprintf(b, n, ms >= 100 ? "%.0f ms" : "%.1f ms", ms); }
inline int low_text(char* b, size_t n, double fps) { return snprintf(b, n, "1%% low %.0f", fps); }

} // namespace mkwvk::fps_stats
