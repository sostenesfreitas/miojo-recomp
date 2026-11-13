"""Requested slow-frame logs include multi-second frames without changing their other gates.

python -I -B tests/test_slow_frame_limit.py [--cxx clang++]
MSVC: run from a configured developer prompt with --cxx cl.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source):
    start = source.index("void log_slow_frame() {")
    return source[start:source.index("\n}", start) + 2]


PREFIX = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
static int64_t clock_ns, draws;
static int clock_reads, stats_reads;
static std::vector<std::string> logs;
namespace std { namespace chrono {
struct fixture_clock {
    using duration = nanoseconds;
    using time_point = std::chrono::time_point<fixture_clock, duration>;
    static time_point now() { ++clock_reads; return time_point(duration(clock_ns)); }
};
}}
#define steady_clock fixture_clock
enum { PROF_SUBMIT, PROF_DRAW, PROF_DESC, PROF_PIPE, PROF_PROGRAM, PROF_CMDS, PROF_MEMSYNC, PROF_QSUBMIT, PROF_SLOTWAIT, PROF_COUNT };
static int64_t prof_ns[PROF_COUNT], presents;
static void draw_stats(int64_t *d, int64_t *sk, int64_t *di, int64_t *pl) { ++stats_reads; *d = draws; *sk = *di = *pl = 0; }
static void shader_stats(int64_t *tr, int64_t *fl) { *tr = *fl = 0; }
static void texture_stats(int64_t *up, int64_t *astc, int64_t *upb) { *up = *astc = *upb = 0; }
static void logf(const char *format, ...) {
    char line[2048]; va_list args; va_start(args, format); vsnprintf(line, sizeof line, format, args); va_end(args);
    logs.emplace_back(line);
}
'''

SUFFIX = r'''
static void step(int64_t elapsed_ns, int64_t added_draws) { clock_ns += elapsed_ns; draws += added_draws; ++presents; log_slow_frame(); }
int main(int argc, char **argv) {
    if (argc != 2) return 90;
    log_slow_frame();
    if (!strcmp(argv[1], "disabled")) {
        step(7000000000LL, 1000);
        return logs.empty() && !clock_reads && !stats_reads ? 0 : 1;
    }
    if (!strcmp(argv[1], "cap")) {
        for (int i = 0; i < 801; ++i) step(35000000, 500);
        return logs.size() == 800 ? 0 : 1;
    }
    step(35000000, 499); /* original startup/draw-count filter retained */
    if (!logs.empty()) return 2;
    step(35000000, 500); /* existing ordinary slow frame retained */
    if (logs.size() != 1 || logs[0].find("35.0 ms") == std::string::npos) return 3;
    step(7000000000LL, 500); /* the previously invisible multi-second hitch */
    if (logs.size() != 2 || logs[1].find("7000.0 ms") == std::string::npos) return 1;
    step(10000000, 500); /* existing configured lower threshold retained */
    if (logs.size() != 2) return 4;
    for (const auto &line : logs) puts(line.c_str());
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    actual = block((args.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="slow-frame-limit-") as tmp:
        folder = Path(tmp)
        cpp, exe = folder / "fixture.cpp", folder / "fixture.exe"
        cpp.write_text(PREFIX + actual + "\n" + SUFFIX, encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", str(cpp),
                       "/Fo" + str(folder / "fixture.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)]
        subprocess.run(command, cwd=folder, check=True)
        for case in ("hitch", "disabled", "cap"):
            env = os.environ.copy()
            env.pop("RX_VK_SLOW_LOG", None)
            if case != "disabled":
                env["RX_VK_SLOW_LOG"] = "35"
            subprocess.run([str(exe), case], env=env, cwd=folder, check=True)
    print("PASS: 7000 ms frame logged; default-off, startup filter, threshold and 800-record cap retained")


if __name__ == "__main__":
    main()
