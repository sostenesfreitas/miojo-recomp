// tests/test_fps_stats.py: gpu/vk/fps_stats.h, the FPS counter's levels, its ring of frames, the averages, the 1% low, the
// graph's columns and scale
#include "../gpu/vk/fps_stats.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace fs = mkwvk::fps_stats;

static bool near(double a, double b, double e = 1e-3) { return std::fabs(a - b) <= e; }

int main() {
    // the row's values: an options file from before the levels (0 Off, 1 On) keeps its meaning, On is the number alone
    CHECK("old off", fs::level_of(0) == fs::kOff);
    CHECK("old on is FPS", fs::level_of(1) == fs::kFps);
    CHECK("a hand-edited value past the last", fs::level_of(9) == fs::kBreakdown && fs::level_of(-3) == fs::kOff);
    CHECK("names", !std::strcmp(fs::level_name(0), "Off") && !std::strcmp(fs::level_name(1), "FPS") &&
                       !std::strcmp(fs::level_name(2), "Frame time") && !std::strcmp(fs::level_name(3), "Graph") &&
                       !std::strcmp(fs::level_name(4), "Breakdown") && fs::kLevels == 5);

    CHECK("corners", !std::strcmp(fs::corner_name(0), "Top right") && !std::strcmp(fs::corner_name(3), "Bottom left") &&
                         !std::strcmp(fs::corner_name(7), "Bottom left"));

    // the parts add up to the frame: the game's is the rest; more measured than the frame is scaled down
    fs::Frame f;
    fs::split(f, 16.0, 3.0, 1.0, 6.0);
    CHECK("game is the rest", near(f.part[fs::kGame], 6.0) && near(f.part[fs::kRender], 3.0) && near(f.part[fs::kGpuWait], 1.0) &&
                                  near(f.part[fs::kPace], 6.0) && near(f.ms, 16.0));
    fs::split(f, 10.0, 4.0, 4.0, 12.0);
    CHECK("over the frame: scaled to it", near(f.part[fs::kGame], 0) && near(f.part[fs::kRender], 2.0) &&
                                              near(f.part[fs::kPace], 6.0));
    CHECK("they add up", near(f.part[0] + f.part[1] + f.part[2] + f.part[3], 10.0));
    fs::split(f, 8.0, -1.0, 0, 0);
    CHECK("a negative part is none", near(f.part[fs::kRender], 0) && near(f.part[fs::kGame], 8.0));

    // the ring: the newest first, the oldest dropped past its size
    static fs::Ring<8> r8;
    for (int i = 0; i < 11; i++) {
        fs::Frame x;
        x.at_ns = i;
        r8.push(x);
    }
    CHECK("ring size", r8.size() == 8);
    CHECK("ring order", r8.back(0).at_ns == 10 && r8.back(7).at_ns == 3);

    // the 1% low: the rate of the slowest 1% (at least one frame)
    std::vector<float> ms(1000, 10.0f);
    ms[17] = 50.0f, ms[500] = 40.0f, ms[999] = 30.0f;   // (10 slowest: these three and seven of 10 ms)
    const double low = fs::one_percent_low(ms.data(), (int)ms.size());
    CHECK("1% of 1000 frames: the 10 slowest", near(low, 1000.0 * 10 / (50 + 40 + 30 + 7 * 10)));
    std::vector<float> few = {16.0f, 20.0f, 33.0f};
    CHECK("fewer than 100 frames: the slowest one", near(fs::one_percent_low(few.data(), 3), 1000.0 / 33.0));
    CHECK("no frames", fs::one_percent_low(nullptr, 0) == 0);

    // a summary: the half second's frames, and the 1% low over its own window
    static fs::Ring<fs::kRing> r;
    const int64_t ms_ns = 1000000;
    int64_t t = 0;
    for (int i = 0; i < 600; i++) {   // (10 s at 60 fps; one 50 ms frame 8 s ago, a pipeline built in the last half second)
        fs::Frame x;
        t += (i == 120 ? 50 : 16) * ms_ns + (i == 120 ? 0 : 666667);
        x.at_ns = t;
        fs::split(x, i == 120 ? 50.0 : 16.666667, 2.0, 1.0, 8.0);
        x.gpu_ms = 9.0f;
        x.pipelines = i == 590 ? 3 : 0;
        x.left_out = i == 595 ? 7 : 0;
        r.push(x);
    }
    const fs::Summary s = fs::summarize(r, t, fs::kSecond / 2, fs::kLowWindow);
    CHECK("half a second of 60 fps", s.frames == 30 || s.frames == 31);
    CHECK("its mean frame time", near(s.avg_ms, 16.666667, 0.01));
    CHECK("its parts", near(s.part[fs::kRender], 2.0) && near(s.part[fs::kGame], 16.666667 - 11.0, 0.01) && near(s.gpu_ms, 9.0));
    CHECK("the stutter's causes", s.pipelines == 3 && s.left_out == 7);
    CHECK("the 1% low has the 50 ms frame", s.low_fps < 50 && s.low_fps > 40);   // (6 slowest: 50 and 5 x 16.7 ms: 45)
    const fs::Summary late = fs::summarize(r, t + 20 * fs::kSecond, fs::kSecond / 2, fs::kLowWindow);
    CHECK("nothing in the window", late.frames == 0 && late.avg_ms == 0 && late.low_fps == 0);

    // the graph's columns: each its slowest frame, oldest left, marks where shaders were built or draws left out
    int col[100];
    bool marks[100];
    fs::columns(r, t, fs::kGraphWindow, 100, col, marks);
    int filled = 0;
    for (int c = 0; c < 100; c++) filled += col[c] >= 0;
    CHECK("4 s at 60 fps fill every 40 ms column", filled == 100);
    CHECK("the newest frame is in the last column", col[99] == 0 || r.back(col[99]).at_ns > t - fs::kGraphWindow / 100);
    CHECK("marks: the frames that built or left out", marks[99] || marks[98]);
    int marked = 0;
    for (bool m : marks) marked += m;
    CHECK("only those", marked >= 1 && marked <= 2);
    fs::Ring<16> slow;
    for (int i = 0; i < 4; i++) {   // (4 frames in 4 s: most columns empty, a slow frame in its own)
        fs::Frame x;
        x.at_ns = (i + 1) * fs::kSecond;
        x.ms = i == 2 ? 80.0f : 20.0f;
        slow.push(x);
    }
    fs::columns(slow, 4 * fs::kSecond, fs::kGraphWindow, 100, col, marks);
    filled = 0;
    for (int c = 0; c < 100; c++) filled += col[c] >= 0;
    CHECK("sparse frames, sparse columns", filled == 4 && col[99] == 0);
    CHECK("the slow frame where it was", col[74] >= 0 && slow.back(col[74]).ms == 80.0f);
    fs::columns(slow, 4 * fs::kSecond, fs::kGraphWindow, 0, col, marks);   // (no columns: nothing written, no division by 0)

    // the scale: twice the cap's frame time, doubled for a slower frame, 100 ms at most
    CHECK("60 fps", near(fs::target_ms(0), 16.6667, 0.001) && near(fs::target_ms(5), 1000.0 / 240));
    CHECK("a cap this build does not know: 60, as the host", near(fs::target_ms(9), 1000.0 / 60) && near(fs::target_ms(-1), 1000.0 / 60));
    CHECK("Uncapped: no target", fs::cap_rate(6) == 0 && fs::target_ms(6) == 0);
    CHECK("HOS_FPS_MAX over the row", fs::cap_rate(0, 144) == 144 && fs::cap_rate(3, 30) == 60 && fs::cap_rate(0, 1000) == 0);
    CHECK("V-Sync On at a slower display", near(fs::target_ms_for(144, true, 60), 1000.0 / 60) &&
                                               near(fs::target_ms_for(60, true, 144), 1000.0 / 60) &&
                                               near(fs::target_ms_for(0, true, 165), 1000.0 / 165));
    CHECK("V-Sync, refresh not known", near(fs::target_ms_for(90, true, 0), 1000.0 / 90) && fs::target_ms_for(0, true, 0) == 0);
    CHECK("Uncapped graph: from 4 ms", fs::graph_top(0, 3.0) == 8.0 && fs::graph_top(0, 1.0) == 4.0 && fs::graph_top(0, 900) == 100.0);
    CHECK("Uncapped colour: against 60", fs::rate_grade(300, 0) == 0 && fs::rate_grade(50, 0) == 1 && fs::rate_grade(0, 0) == 2);
    CHECK("top at 60 fps", near(fs::graph_top(fs::target_ms(0), 20), 33.333, 0.01));
    CHECK("a slow stretch of 40 ms", near(fs::graph_top(fs::target_ms(0), 40), 66.667, 0.01));
    CHECK("a 2 s stall: four times the cap's at most", near(fs::graph_top(fs::target_ms(0), 2000), 66.667, 0.01));
    CHECK("at 120 fps", near(fs::graph_top(fs::target_ms(2), 8), 1000.0 / 120 * 2, 0.01));
    std::vector<float> cols(100, 16.7f);
    cols[40] = 100.0f;   // (one spike: the usual frame stays 16.7, the scale 33)
    CHECK("one spike does not set the scale", near(fs::typical_ms(cols.data(), 100), 16.7, 0.01) &&
                                                  near(fs::graph_top(fs::target_ms(0), fs::typical_ms(cols.data(), 100)), 33.333, 0.01));
    CHECK("no columns", fs::typical_ms(nullptr, 0) == 0);
    CHECK("rate at the cap", fs::rate_grade(59.0, fs::target_ms(0)) == 0 && fs::rate_grade(144, fs::target_ms(3)) == 0);
    CHECK("under it", fs::rate_grade(50.0, fs::target_ms(0)) == 1 && fs::rate_grade(30.0, fs::target_ms(0)) == 2);

    char b[48];
    fs::frame_time_text(b, sizeof b, 16.666);
    CHECK("ms text", !std::strcmp(b, "16.7 ms"));
    fs::frame_time_text(b, sizeof b, 250.4);
    CHECK("long ms text", !std::strcmp(b, "250 ms"));
    fs::low_text(b, sizeof b, 52.4);
    CHECK("low text", !std::strcmp(b, "1% low 52"));
    std::puts("fps stats: all checks passed");
}
