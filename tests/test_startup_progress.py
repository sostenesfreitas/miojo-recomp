"""The start-up screen's numbers (gpu/vk/vk_progress.h): the count with its separators, the time left from the recent rate (not
from the start, which a resumed preparation's skipped part would have made far too hopeful), and its wording. Also that a windowed
start prepares the shaders behind the screen while a headless one does not, and that a close stops the workers.

python -I -B tests/test_startup_progress.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include "vk_progress.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk;
int main() {
    CHECK(with_commas(0) == "0");
    CHECK(with_commas(999) == "999");
    CHECK(with_commas(1000) == "1,000");
    CHECK(with_commas(38741) == "38,741");
    CHECK(with_commas(1234567) == "1,234,567");

    CHECK(left_text(-1).empty());
    CHECK(left_text(3) == "a few seconds left");
    CHECK(left_text(36) == "about 40 s left");
    CHECK(left_text(40) == "about 40 s left");
    CHECK(left_text(58) == "about 1 min left");
    CHECK(left_text(150) == "about 3 min left");
    CHECK(version_text("v2.0.5 (source 1a2b3c4d)") == "v2.0.5");
    CHECK(version_text("v2.0.5") == "v2.0.5");
    CHECK(version_text("v2.0.5-2-g619c897-dirty (source 2c44105b)") == "v2.0.5 dev");
    CHECK(version_text("movie-sync (source 1a2b3c4d)") == "movie-sync");
    // (Hookshot's names: a release candidate as its tag, a nightly as the launcher names it, a build between tags as "dev")
    CHECK(version_text("v3.0.0-rc.1 (source 1a2b3c4d)") == "v3.0.0-rc.1");
    CHECK(version_text("v3.0.0-rc.12") == "v3.0.0-rc.12");
    CHECK(version_text("v3.0.0-rc.1-5-gabc1234-dirty (source 1a2b3c4d)") == "v3.0.0-rc.1 dev");
    CHECK(version_text("v3.0.0-5-gabc1234") == "v3.0.0 dev");
    CHECK(version_text("v3.10.2") == "v3.10.2");
    CHECK(version_text("nightly-46-434712bb (source 6c70b6d8)") == "Nightly 46 \xc2\xb7 434712bb");
    CHECK(version_text("nightly-46-434712bb-dirty") == "Nightly 46 \xc2\xb7 434712bb dev");
    CHECK(version_text("nightly-46-xyz") == "nightly-46-xyz");
    CHECK(version_text("v3.0") == "v3.0");
    CHECK(version_text("unknown") == "unknown");
    CHECK(percent_text(0, 0).empty());
    CHECK(percent_text(12345, 38809) == "31%");
    CHECK(percent_text(38808, 38809) == "99%");   // (never 100% before the last one)
    CHECK(percent_text(38809, 38809) == "100%");

    // the screen: the wordmark alone for the first second, then the step, the title, the count, the time left and the hint
    {
        StartupState st;
        st.secs = 0.5, st.prepared = true, st.total = 100;
        CHECK(startup_view(st).title.empty());
        st.secs = 30, st.done = 12345, st.total = 38809, st.left = 38;
        StartupView v = startup_view(st);
        CHECK(v.step == "STEP 1 OF 2" && v.title == "Preparing shaders" && v.pct == "31%");
        CHECK(v.left == "12,345 / 38,809 shaders" && v.right == "about 40 s left" && !v.hint.empty());
        CHECK(v.fraction > 0.31 && v.fraction < 0.32);
        st.stage = 2, st.done = 6120, st.total = 10340, st.left = 4;
        v = startup_view(st);
        CHECK(v.step == "STEP 2 OF 2" && v.title == "Building pipelines" && v.left == "6,120 / 10,340 pipelines");
        st.stage = 1, st.total = 0;   // (the predicted pipelines, not counted yet: no bar)
        v = startup_view(st);
        CHECK(v.step == "STEP 2 OF 2" && v.fraction < 0 && v.left == "Reading the pipeline notes");
        // a warm start: the pipelines alone, no step, no "after an update" hint
        StartupState warm;
        warm.secs = 3, warm.stage = 2, warm.done = 100, warm.total = 3822;
        v = startup_view(warm);
        CHECK(v.step.empty() && v.title == "Loading pipelines" && v.hint.empty());
        warm.finished = true;
        CHECK(startup_view(warm).title == "Starting the game");
        warm.secs = 0.4;   // (over before the first second: the wordmark alone, no flash of text)
        CHECK(startup_view(warm).title.empty());
        st.stopping = true;
        v = startup_view(st);
        CHECK(v.title == "Closing" && v.fraction < 0);
    }

    // a steady 400 a second: nothing said for the first seconds nor before a sixth is done, then the rest at that rate
    ProgressClock c;
    for (int i = 0; i <= 10; i++) c.note(i * 0.25, (size_t)(i * 100), 20000);
    CHECK(c.left() < 0);
    for (int i = 11; i <= 24; i++) c.note(i * 0.25, (size_t)(i * 100), 20000);
    CHECK(c.left() < 0);   // (6 s in, 2400 of 20000: under a sixth)
    for (int i = 25; i <= 40; i++) c.note(i * 0.25, (size_t)(i * 100), 20000);
    CHECK(std::fabs(c.left() - 40) < 1);   // (10 s in, 4000 done, 16000 to go at 400/s)
    // a resumed run: 30000 of 40000 already on disk go by in two seconds, then 100 a second. The estimate soon follows the
    // slow part (the burst falls out of its window), where the average from the start would have said far less
    c.reset();
    for (int i = 0; i <= 8; i++) c.note(i * 0.25, (size_t)(i * 3750), 40000);
    double t = 2;
    size_t d = 30000;
    for (; t < 45; t += 0.5) d += 50, c.note(t, d, 40000);
    {
        const double truth = (40000.0 - d) / 100;
        CHECK(std::fabs(c.left() - truth) < truth * 0.1);
        const double from_start = (40000.0 - d) / (d / t);
        CHECK(from_start < truth / 2);   // (what the average from the start would have said)
    }
    // one slow stretch (a few heavy programs) moves what is shown a little, not five times over
    c.reset();
    t = 0, d = 0;
    for (; t < 40; t += 0.5) d += 500, c.note(t, d, 100000);
    const double before = c.left();
    for (int k = 0; k < 16; k++) t += 0.5, d += 20, c.note(t, d, 100000);
    CHECK(c.estimate(100000) > before * 1.8);   // (the unsmoothed rate did jump)
    CHECK(c.left() < c.estimate(100000) * 0.85);
    // done: nothing left; a new stage (the count going back) starts over
    c.note(t + 1, 100000, 100000);
    CHECK(c.left() == 0);
    c.note(t + 1.5, 10, 4000);
    CHECK(c.left() < 0);
    // samples closer than a quarter second are dropped, so a fast caller does not fill the window with one instant
    c.reset();
    for (int i = 0; i < 1000; i++) c.note(i * 0.001, (size_t)i, 2000);
    CHECK(c.n <= 4);
    // nothing moving for a while (a driver busy on one pipeline): no estimate rather than a division by zero
    c.reset();
    for (int i = 0; i < 40; i++) c.note(i * 0.5, 4570, 4571);
    CHECK(c.left() < 0);
    // one bar over the three parts, each in the total from the start: the predicted pipelines are not a second bar after a full one
    {
        PrepareBar b;
        b.guess(0, 38809), b.guess(1, 38809), b.guess(2, 2000);
        b.at(0, 38809, 38809);   // (every program translated: the bar is far from full, the pipelines are still to build)
        const double programs_done = b.fraction();
        CHECK(programs_done > 0.1 && programs_done < 0.5);
        b.reach(1);   // (the prediction deciding: nothing counted yet, the bar holds)
        CHECK(b.fraction() == programs_done);
        b.at(1, 0, 45882);   // (its count replaces the guess; the bar never goes back)
        CHECK(b.fraction() >= programs_done);
        b.at(1, 45881, 45882);
        CHECK(b.fraction() < 1);
        b.at(1, 45882, 45882);
        CHECK(b.fraction() < 1);   // (the noted pipelines are still to come)
        b.at(2, 1000, 2000);
        CHECK(b.fraction() < 1);
        b.at(2, 2000, 2000);
        CHECK(b.fraction() == 1);
        StartupState st;
        st.secs = 30, st.prepared = true, st.stage = 1, st.done = 100, st.total = 45882, st.whole = 0.5;
        StartupView v = startup_view(st);
        CHECK(v.fraction == 0.5 && v.pct == "50%" && v.left == "100 / 45,882 pipelines");
        st.total = 0;   // (the prediction deciding: the bar stays, with the whole's share)
        v = startup_view(st);
        CHECK(v.fraction == 0.5 && v.left == "Reading the pipeline notes");
        // a start with nothing to prepare: the pipelines alone fill the bar
        PrepareBar w;
        w.guess(0, 38809), w.guess(1, 38809), w.guess(2, 100);
        w.skip(0), w.skip(1);
        w.at(2, 50, 100);
        CHECK(std::fabs(w.fraction() - 0.5) < 1e-9);
        CHECK(percent_of(0.999) == "99%" && percent_of(1) == "100%" && percent_of(-1).empty());
    }
    std::printf("PASS: counts, time left and its wording\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()

    main_cpp = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    started = main_cpp[main_cpp.index("bool started_up() {"):main_cpp.index("int start(void* hwnd) {")]
    if "if (!dev.offscreen && screen_wanted())" not in started:
        raise SystemExit("a windowed start no longer prepares the shaders behind the start-up screen")
    if started.index("screen_wanted()") > started.index("if (console) CloseHandle(console)"):
        raise SystemExit("the start-up screen is chosen after the headless preparation")
    screen = main_cpp[main_cpp.index("void prepare_with_screen() {"):main_cpp.index("bool started_up() {")]
    for need, why in (("std::thread worker(", "the preparation runs on the window's own thread"),
                      ("std::thread painter(", "a present that waits for a hidden window holds the window's messages or the game"),
                      ("present_image(nullptr", "nothing is shown while it runs"),
                      ("PeekMessageA", "the Windows window's messages wait meanwhile (not responding)"),
                      ("dev.swap_resize.store", "a resize while it runs is not taken")):
        if need not in screen:
            raise SystemExit(why)
    if 'void mkwvk_close_asked(void)' not in main_cpp or "prepare_stop();" not in main_cpp:
        raise SystemExit("closing the window no longer stops the preparation")
    for fn in ("bool prepare_run(", "bool warm_libraries(", "void predict_run(", "void precompile_run("):
        body = draw[draw.index(fn):]
        body = body[:body.index("\n}\n")]
        if body.count("prepare_stopped()") < 2:
            raise SystemExit(fn + " does not stop its workers and its wait when the window is closed")
    # the predicted pipelines are built in the preparation, all of them, unless RX_VK_PREDICT_S sets a budget; on every CPU
    predict = draw[draw.index("void predict_run("):]
    predict = predict[:predict.index("\n}\n")]
    if 'getenv("RX_VK_PREDICT_S") ? atoi(getenv("RX_VK_PREDICT_S")) : 0;' not in predict:
        raise SystemExit("predict_run has a time budget by default: the predicted pipelines past it are built in play")
    if "THREAD_PRIORITY_BELOW_NORMAL" in predict or "cpus / 2" in predict:
        raise SystemExit("predict_run builds on half the CPUs or below normal priority: nothing else runs before the game")
    for need in ("prepare_guess(", "bar.at(", "st.whole"):
        if need not in screen:
            raise SystemExit("the start-up screen's bar does not count every part from the start (" + need + ")")
    prep = draw[draw.index("bool prepare_run("):]
    prep = prep[:prep.index("\n}\n")]
    stop_at = prep.index("if (prepare_stopped()) {")
    if prep.index('prepared_write(', stop_at) < prep.index("return false;", stop_at):
        raise SystemExit("a stopped preparation writes the stamp that says it is all done")
    for host in ("host/display.c", "host/posix/display_sdl.c"):
        text = (ROOT / host).read_text(encoding="utf-8")
        if text.count("rx_close_asked();") < 1:
            raise SystemExit(host + " ends the process on close without letting the preparation keep its work")

    with tempfile.TemporaryDirectory(prefix="startup-progress-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
