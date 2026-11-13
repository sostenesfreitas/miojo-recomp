"""Native returned-frame tracing keeps stalls and leaves pacing unchanged.

python -I -B tests/test_present_trace.py [--cxx clang++]
For MSVC use --cxx cl in a configured developer prompt. No game or GPU.
"""
import argparse
import csv
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(source, marker):
    start = source.index(marker)
    match = re.search(r"^}", source[start:], re.M)
    if not match:
        raise SystemExit("source function has no closing brace")
    return source[start:start + match.end()]


PREFIX = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long long LONG64;
static volatile int present_interval = 1;
static int64_t next_frame_at, window_start, frames_in_window, shown_at[16];
static volatile LONG64 presents, shown_count;
static volatile uint64_t hos_mkw_pacer;
static uint32_t guest32;
static int scans, steps, sleeps, reads;
static int64_t clock_now;
typedef struct { int unused; } nvn_tex;
typedef struct { int ntex, crop[4], presents; } nvn_win;
static nvn_win window = {1, {0}, 0};
static nvn_tex texture;
volatile LONG64 hos_present_count;
static int has_vcore;
static int64_t renderer_work, back_work;
#define NV(name) static void name(void)
#define P(i) (&window)
#define I(i) 0
static void die(const char *fmt, ...) { (void)fmt; exit(91); }
static void held_flush(void) { clock_now += 2000000; }
#define HOS_CHAIN(kind, key) ((void)(key))   /* (HOS_CHAIN off: host/hos.h) */
static int hos_vcore_away(void) { clock_now += 1000000; return has_vcore; }
static const nvn_tex *win_tex(const nvn_win *o, int i) { (void)o; (void)i; return &texture; }
static void rx_nvn_present(const nvn_tex *t, const int crop[4]) {
    (void)t; (void)crop; clock_now += renderer_work;
}
static void hos_vcore_back(void) { clock_now += back_work; }
#define G32(a) guest32
#define G64(a) ((uint64_t)0)
#define GP(a) ((void *)(uintptr_t)(a))
static int64_t InterlockedIncrement64(volatile LONG64 *p) { return ++*p; }
static int64_t hos_now_ns(void) { ++reads; return clock_now; }
static void hos_sleep_ns(int64_t ns) { (void)ns; ++sleeps; }
static void scan_dt(long long frame) { (void)frame; ++scans; }
static void realtime_step(int64_t now) { (void)now; ++steps; }
static void hos_log(const char *fmt, ...) { (void)fmt; }
static uint64_t hos_ticks(void) { return 0; }
static int fps_max(void) { return 60; }   /* (nn_vi.c's Frame rate cap: the console's 60 here) */
'''

SUFFIX = r'''
int main(int argc, char **argv) {
    if (argc != 3) return 90;
    const int enabled = !strcmp(argv[1], "enabled");
    const int64_t times[] = {1000000000LL, 1016666667LL, 1066666667LL, 9066666667LL, 9083333334LL};
    for (int i = 0; i < 5; ++i) {
        has_vcore = i != 4;
        renderer_work = i == 3 ? 300000000 : 7000000;
        back_work = i == 2 ? 40000000 : 3000000;
        clock_now = times[i] - 3000000 - renderer_work - (has_vcore ? back_work : 0);
        nvnQueuePresentTexture();
        if (enabled && i == 3) {
            FILE *probe = fopen(argv[2], "rb");
            if (!probe) return 1; /* old source is red: no trace */
            int lines = 0, c;
            while ((c = fgetc(probe)) != EOF) if (c == '\n') ++lines;
            fclose(probe);
            if (lines != 5) return 2; /* header + four returned frames flushed after the stall */
        }
    }
    if (presents != 5 || shown_count != 5 || scans != 5 || steps != 5 || sleeps != 0 ||
        reads != (enabled ? 25 : 5) || window.presents != 5 || hos_present_count != 5) return 3;
    printf("STATE %lld %lld %lld %d %d %d\n", (long long)presents, (long long)shown_count,
           (long long)next_frame_at, scans, steps, sleeps);
    if (!enabled) {
        FILE *probe = fopen(argv[2], "rb");
        if (probe) { fclose(probe); return 4; }
    }
    return 0; /* normal CRT exit flushes the remaining buffered row */
}
'''

def check_csv(path):
    with path.open() as stream:
        rows = list(csv.DictReader(stream))
    times = [1000000000, 1016666667, 1066666667, 9066666667, 9083333334]
    assert list(rows[0]) == ["frame", "return_ns", "interval_ns", "present_begin_ns",
                             "renderer_begin_ns", "renderer_end_ns", "vcore_back_ns"]
    assert [int(r["frame"]) for r in rows] == [1, 2, 3, 4, 5]
    assert [int(r["return_ns"]) for r in rows] == times
    assert [int(r["interval_ns"]) for r in rows] == [0, 16666667, 50000000, 8000000000, 16666667]
    assert sum(int(r["interval_ns"]) for r in rows) == times[-1] - times[0]
    for i, row in enumerate(rows):
        renderer = 300000000 if i == 3 else 7000000
        back = 0 if i == 4 else 40000000 if i == 2 else 3000000
        assert int(row["present_begin_ns"]) == times[i] - back - renderer - 3000000
        assert int(row["renderer_begin_ns"]) == times[i] - back - renderer
        assert int(row["renderer_end_ns"]) == times[i] - back
        assert int(row["vcore_back_ns"]) == times[i]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    source = (args.source / "host/nn_vi.c").read_text(encoding="utf-8")
    native = (args.source / "host/nvn_native.c").read_text(encoding="utf-8")
    enable = block(source, "int hos_vi_present_trace_enabled(") if "int hos_vi_present_trace_enabled(" in source else ""
    helper = block(source, "static void present_trace(") if "static void present_trace(" in source else ""
    actual = block(source, "void hos_vi_native_presented(")
    present = block(native, "NV(nvnQueuePresentTexture) {")
    states = []
    with tempfile.TemporaryDirectory(prefix="present-trace-") as tmp:
        folder = Path(tmp)
        cpp, exe = folder / "fixture.cpp", folder / "fixture.exe"
        body = PREFIX + enable + "\n" + helper + "\n" + actual + "\n" + present + "\n" + SUFFIX
        cpp.write_text(body, encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", str(cpp),
                       "/Fo" + str(folder / "fixture.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)]
        subprocess.run(command, cwd=folder, check=True)
        for mode in ("enabled", "disabled", "empty"):
            path = folder / (mode + ".csv")
            env = os.environ.copy()
            env.pop("HOS_PRESENT_TRACE", None)
            env.pop("HOS_FRAME_SLEEP_MS", None)
            if mode == "enabled":
                env["HOS_PRESENT_TRACE"] = str(path)
            elif mode == "empty":
                env["HOS_PRESENT_TRACE"] = ""
            ran = subprocess.run([str(exe), mode, str(path)], env=env, cwd=folder,
                                 capture_output=True, text=True, check=True)
            states.append(ran.stdout)
            if mode == "enabled":
                check_csv(path)
            else:
                assert not path.exists(), "disabled trace created a file"
        assert states[0] == states[1] == states[2], "tracing changed pacing/counters"
        # A missing renderer-end timestamp loses the renderer/core-wait split.
        anchor = "phases[2] = hos_now_ns();"
        assert body.count(anchor) == 1, "missing renderer-end timestamp anchor"
        cpp.write_text(body.replace(anchor, "phases[2] = (hos_now_ns(), phases[1]);"), encoding="utf-8")
        subprocess.run(command, cwd=folder, check=True)
        env["HOS_PRESENT_TRACE"] = str(folder / "broken.csv")
        subprocess.run([str(exe), "enabled", env["HOS_PRESENT_TRACE"]], env=env, cwd=folder, check=True)
        try:
            check_csv(folder / "broken.csv")
        except AssertionError:
            pass
        else:
            raise SystemExit("the phase check accepted a missing renderer-end timestamp")
    print("PASS: native present phases distinguish renderer/core waits;8s stall kept; unset/empty add no clocks; bad phase rejected")


if __name__ == "__main__":
    main()
