# SPDX-License-Identifier: GPL-3.0-or-later
"""A shader preparation the GPU driver stops answering in goes on or ends, and says why (#2107).

Builds on players' cards of all kinds, AMD and NVIDIA, stopped for good in the predicted pipelines, the Builder's details
showing the last line written ("vertex format NVN 0xe -> vk 17"). In one log the driver had faulted in a pipeline compile
(caught: "the driver faulted compiling") just before. Compiled from the real source, with stand-ins for the driver and files:

- predict_run (gpu/vk/vk_draw.cpp): builds every predicted pipeline, its first one alone. Once nothing has finished for
  RX_VK_PREDICT_STALL_S it goes on, leaving the stuck workers in the driver: anywhere in the run after a fault in the driver, and
  otherwise only once every program was handed out (the last stragglers). A slow driver that still finishes pipelines, or one
  stuck early with no fault, is waited for (that is the watchdog's).
- the --prepare-shaders run's watchdog (gpu/vk/vk_main.cpp prepare_watchdog): while nothing moves, what it waits on, even with
  the pipeline steps' own progress lines (#2186) coming every 10 s; the shader programs' count after a quiet spell, and nothing of
  its own while the pipeline steps move (their lines say it); once nothing has moved for RX_VK_PREPARE_STALL_S, the reason,
  hookshot_prepare_skipped and exit status 4.
  The pipeline cache's save is stopped only when workers were left in the driver or it faulted, and after twice that limit (a
  fault AMD's retry recovered from counts, so a slow save there is not stopped as early); otherwise it is reported, and its lines
  end so that make_game.py's silence limit still applies.
- make_game.py says "the GPU driver stopped responding" for exit status 4.

python -I -B tests/test_prepare_stall.py --cxx clang++
"""
import argparse
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(text, marker):
    """The function or block that starts at marker, through its closing brace"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREDICT_HEAD = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); std::_Exit(7); } } while (0)
using VkPipeline = uintptr_t;
using VkPrimitiveTopology = int;
struct NvnState { int unused = 0; };
struct Targets {};
struct PipeNote { uint64_t key, prog, variant; uint32_t topo; NvnState s; };
struct Program { uint64_t key; uint64_t variant; bool mesh = false, fallback = false; };
// (the prediction's own parts, beside the stall guard: its counts, the unfinished-run marker, the sample, the driver cache's size,
// the pool's size, and the duplicate check's key, one per note here (each note its own pipeline))
struct PredictCounts { size_t vertex_types = 0, output_types = 0, variants_unfit = 0, notes_unfit = 0, refit = 0; };
std::string predict_pending_path() {
    static const std::string path = (std::filesystem::temp_directory_path() /
                                     ("hookshot-prepare-stall-" + std::to_string((uintptr_t)&path) + ".predicting")).string();
    return path;
}
void predicted_sample_write(const std::vector<std::unique_ptr<PipeNote>>&) {}
void driver_cache_log(const char*) {}
unsigned usable_cpus() { return std::max(1u, std::thread::hardware_concurrency()); }
inline uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }
struct { uintptr_t device = 1; } dev;
thread_local bool driver_cache_only = false;
static bool env_on(const char* n) { const char* v = std::getenv(n); return v && *v && std::strcmp(v, "0"); }   // (a stand-in for compat/hookshot_env.h)
// 0 quick; 1 one program's pipelines never finish (the last stragglers); 2 each pipeline slow, every program longer than the
// stall; 3 the driver faults, then nothing finishes, early in the run; 4 nothing finishes early in the run, no fault
static int mode;
static int programs_n = 40, per_program = 3;
static constexpr uint64_t kStuck = 7;
static std::atomic<int> built{0}, destroyed{0}, inside{0}, faults{0};
static std::atomic<bool> first_was_alone{true}, first_seen{false}, left_noted{false};
static std::string logged;
static std::mutex log_mu;
void logf(const char* f, ...) {
    char b[2048];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    std::lock_guard lk(log_mu);
    logged += b;
    logged += "\n";
}
bool prepare_stopped() { return false; }
int driver_faults() { return faults.load(); }
void prepare_left_workers_note() { left_noted = true; }
void prepare_progress_log(const char*, size_t, size_t, std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point&) {}
void ub_thread_off() {}
void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::vector<std::unique_ptr<PipeNote>> pipe_notes_read() {
    std::vector<std::unique_ptr<PipeNote>> v;
    v.push_back(std::make_unique<PipeNote>(PipeNote{1, 1, 0, 3, {}}));
    return v;
}
std::vector<std::unique_ptr<PipeNote>> predict_pipelines(const std::vector<std::unique_ptr<PipeNote>>&, size_t* programs,
                                                         PredictCounts*) {
    std::vector<std::unique_ptr<PipeNote>> v;
    for (int p = 1; p <= programs_n; p++)
        for (int k = 0; k < per_program; k++) v.push_back(std::make_unique<PipeNote>(PipeNote{0, (uint64_t)p, 0, 3, {}}));
    v.push_back(std::make_unique<PipeNote>(PipeNote{0, 2, 99, 3, {}}));   // (a variant no kept program answers)
    *programs = (size_t)programs_n;
    return v;
}
void program_cache_borrow(uint64_t key, const std::function<void(Program&)>& fn) {
    Program p{key, 0};
    fn(p);
}
void ensure_layout(Program&) {}
Targets note_targets(const PipeNote&) { return {}; }
uint64_t pipeline_key(const Program&, const NvnState& s, const Targets&, VkPrimitiveTopology) { return (uint64_t)(uintptr_t)&s; }
VkPipeline build_graphics(const Program& p, const NvnState&, const Targets&, VkPrimitiveTopology) {
    CHECK(driver_cache_only);
    ++inside;
    const bool first = !first_seen.exchange(true);
    // (the driver never answers; never the first, which the others wait for)
    const bool stuck = !first && ((mode == 1 && p.key == kStuck) || mode == 3 || mode == 4);
    if (stuck) {
        if (mode == 3) faults++;   // (caught in the driver: "the driver faulted compiling")
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(mode == 2 ? 300 : first ? 200 : 1));
    if (first && inside.load() != 1) first_was_alone = false;   // (another began while the first was being built)
    --inside;
    return (VkPipeline)(0x100 + ++built);
}
VkPrimitiveTopology pipeline_topology(const Program&, VkPrimitiveTopology t) { return t; }
void vkDestroyPipeline(uintptr_t, VkPipeline, const void*) { ++destroyed; }
static std::atomic<size_t> last_done{0}, last_total{0};
void progress(size_t done, size_t total) {
    CHECK(done <= total);
    last_done = done, last_total = total;
}
'''

PREDICT_MAIN = r'''
int main(int argc, char** argv) {
    CHECK(argc == 2);
    mode = std::atoi(argv[1]);
    if (mode == 2) programs_n = 6, per_program = 5;   // (each program 1.5 s, past the 1 s stall: only per pipeline counts)
    if (mode >= 3) programs_n = 400;   // (more programs than workers: the run is stuck before every one is handed out)
    const auto t0 = std::chrono::steady_clock::now();
    predict_run(progress);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int all = programs_n * per_program;
    std::printf("%s", logged.c_str());
    CHECK(mode != 4);   // (no fault and stuck early: waited for, the watchdog's to end; the test stops it)
    CHECK(last_total == (size_t)all + 1 && last_done == last_total);   // (the bar full, what was given up included)
    CHECK(first_was_alone);
    if (mode == 1 || mode == 3) {
        CHECK(logged.find("nothing finished for 1 s") != std::string::npos);
        CHECK(logged.find(mode == 1 ? "the last ones stuck in the driver" : "the driver faulted, and is stuck since") !=
              std::string::npos);
        CHECK(logged.find("gone on without the pipelines the driver did not finish") != std::string::npos);
        CHECK(left_noted);
        CHECK(secs < 20);
        if (mode == 1 && std::thread::hardware_concurrency() > 1)   // (only the stuck program's left: its first, if it was first)
            CHECK(built >= all - per_program && built <= all - per_program + 1);
        std::fflush(stdout);
        std::_Exit(0);   // (its workers are still in the "driver")
    }
    CHECK(logged.find("nothing finished") == std::string::npos && !left_noted);
    CHECK(built == all && destroyed == all);
    return 0;
}
'''

WATCH_HEAD = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
static void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
struct MkwVkHost { void (*log)(const char*); void (*log_flush)(void); };
static void host_log(const char* s) { std::printf("[log] %s\n", s); std::fflush(stdout); }
static void host_flush() { std::fflush(stdout); }
static const MkwVkHost host_v{host_log, host_flush};
const MkwVkHost* host = &host_v;
static int faults;
static bool left;
int driver_faults() { return faults; }
bool prepare_left_workers() { return left; }
std::string skipped_path() { return std::getenv("FIXTURE_SKIPPED"); }
'''

WATCH_MAIN = r'''
int main(int argc, char** argv) {
    if (argc != 2) return 9;
    const std::string c = argv[1];
    if (c == "fault") faults = 1;
    if (c == "save-left") left = true;
    prepare_watchdog();
    if (c == "stuck" || c == "fault") {
        prepare_moved(1, 5, 10);
        Sleep(20000);   // (the watchdog ends the process before this does)
        return 8;
    }
    if (c == "moving") {
        for (size_t i = 1; i <= 15; i++) prepare_moved(0, i, 100), Sleep(300);
        return 0;
    }
    if (c == "save") {   // (a long save, nothing left in the driver: reported, then silent, never stopped)
        prepare_moved(3, 0, 0);
        Sleep(7000);
        return 0;
    }
    if (c == "save-left") {   // (workers were left in the driver: the save waits on them, and is stopped, at twice the parts' limit)
        prepare_moved(3, 0, 0);
        Sleep(20000);
        return 8;
    }
    if (c == "end") prepare_end(0);   // (a run that left workers: done, ended at once)
    if (c == "busy-stuck" || c == "busy-moving") {   // (other lines every 200 ms, as the pipeline steps' own progress lines)
        std::thread([] { for (;;) logf("vk: predicted pipelines: progress line"), Sleep(200); }).detach();
        if (c == "busy-stuck") {   // (stuck: what it waits on is still said, and it stops)
            prepare_moved(1, 5, 10);
            Sleep(20000);
            return 8;
        }
        for (size_t i = 1; i <= 15; i++) prepare_moved(1, i, 100), Sleep(300);   // (moving: the steps' own lines say it)
        std::fflush(stdout);
        std::_Exit(0);
    }
    return 9;
}
'''


def compile_run(cxx, name, code, tmp):
    src = Path(tmp) / (name + ".cpp")
    exe = Path(tmp) / (name + (".exe" if os.name == "nt" else ""))
    src.write_text(code, encoding="utf-8")
    # (-D_CRT_SECURE_NO_WARNINGS: a clang that targets MSVC's C library, CI's, calls getenv and fopen deprecated, as the game's
    # own build says too: toolchain.native_target)
    cmd = [cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-D_CRT_SECURE_NO_WARNINGS", str(src),
           "-o", str(exe)]
    if os.name != "nt":
        cmd.insert(1, "-pthread")
    subprocess.run(cmd, check=True, timeout=180)
    return exe


def check_predict(cxx, tmp):
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    exe = compile_run(cxx, "predict", PREDICT_HEAD + block(draw, "void predict_run(") + PREDICT_MAIN, tmp)
    env = dict(os.environ, RX_VK_PREDICT_STALL_S="1")
    for case, what in (("0", "every predicted pipeline built, the first alone"),
                       ("1", "the last pipelines never finish: the run goes on without them"),
                       ("2", "a slow driver that still finishes pipelines: no stop"),
                       ("3", "the driver faulted, then nothing finishes: the run goes on")):
        r = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=60)
        if r.returncode:
            raise SystemExit("predict_run, %s: exit %d\n%s%s" % (what, r.returncode, r.stdout, r.stderr))
        print("PASS predict_run:", what, flush=True)
    # (stuck early with no fault: the bulk is not given up here; it waits, as before, and the watchdog ends a real hang)
    try:
        r = subprocess.run([str(exe), "4"], env=env, capture_output=True, text=True, timeout=6)
        raise SystemExit("predict_run gave up the bulk of the run with no fault in the driver: exit %d\n%s%s"
                         % (r.returncode, r.stdout, r.stderr))
    except subprocess.TimeoutExpired:
        print("PASS predict_run: stuck early with no fault: waited for", flush=True)


def check_watchdog(cxx, tmp):
    main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    watch = main[main.index("struct PrepareSeen {"):]
    watch = watch[:watch.index(block(watch, "void prepare_watchdog() {")) + len(block(watch, "void prepare_watchdog() {"))]
    pieces = [block(main, "int64_t steady_ms() {"), "std::atomic<int64_t> last_log_ms{0};\n", block(main, "void logf(const char* fmt, ...) {"),
              watch]
    exe = compile_run(cxx, "watchdog", WATCH_HEAD + "\n".join(pieces) + WATCH_MAIN, tmp)
    for case, code, need, never in (
            ("stuck", 4, ["waiting on the GPU driver or the disk", "the predicted pipelines (5 of 10)", "STOPPED"], ["faulted"]),
            ("fault", 4, ["faulted 1 time so far", "waiting on the GPU driver, which faulted (above)", "after it faulted (above)", "STOPPED"],
             []),
            ("moving", 0, ["still working on the shader programs"], ["STOPPED", "waiting:"]),
            ("save", 0, ["still writing the pipeline cache's save", "this is the disk"], ["STOPPED", "(5 s)", "(6 s)"]),
            ("save-left", 4, ["which still has pipelines it did not finish", "(5 s)", "Stopped at 8 s",
                               "STOPPED: the pipeline cache's save waited 8 s"], []),
            ("end", 0, ["@@done shaders"], ["STOPPED"]),
            ("busy-stuck", 4, ["waiting: nothing in the predicted pipelines has finished", "STOPPED"], []),
            ("busy-moving", 0, ["progress line"], ["still working", "waiting:", "STOPPED"])):
        skipped = Path(tmp) / ("skipped-" + case)
        env = dict(os.environ, RX_VK_PREPARE_STALL_S="4", RX_VK_PREPARE_STATUS_S="1", FIXTURE_SKIPPED=str(skipped))
        t0 = time.time()
        r = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=60)
        out = r.stdout
        if r.returncode != code:
            raise SystemExit("watchdog %s: exit %d, expected %d\n%s%s" % (case, r.returncode, code, out, r.stderr))
        for n in need:
            if n not in out:
                raise SystemExit("watchdog %s: the log lacks %r\n%s" % (case, n, out))
        for n in never:
            if n in out:
                raise SystemExit("watchdog %s: the log has %r\n%s" % (case, n, out))
        if (code == 4) != skipped.exists():
            raise SystemExit("watchdog %s: hookshot_prepare_skipped %s" % (case, "missing" if code == 4 else "left by a run that went on"))
        if code == 4 and time.time() - t0 > 15:
            raise SystemExit("watchdog %s: took %.0f s to stop" % (case, time.time() - t0))
        print("PASS watchdog:", case, flush=True)


def check_sources():
    main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    only = main[main.index("if (prepare_only()) {"):]
    only = only[:only.index("\n        return true;\n    }\n")]
    for need, why in (("prepare_watchdog();", "the --prepare-shaders run has no watchdog"),
                      ("prepare_moved(part, done, total);", "the watchdog does not see the parts' progress"),
                      ("prepare_moved(3, 0, 0);", "the watchdog does not know the save from the parts")):
        if need not in only:
            raise SystemExit(why)
    save = only.index("pipeline_cache_save(true);")
    if only.index("prepare_moved(3, 0, 0);") > save:
        raise SystemExit("the watchdog does not know the save from the parts")
    after = only[save:]
    if "prepare_left_workers()" not in after or "prepare_end(0);" not in after:
        raise SystemExit("a run that left workers in the driver ends through exit(), which can wait on them")
    end = block(main, "[[noreturn]] void prepare_end(int code) {")
    if "TerminateProcess(GetCurrentProcess()" not in end or "exit(" in end.replace("_Exit(", ""):
        raise SystemExit("prepare_end ends the run through exit(), which unloads the driver it waits on")
    if "prepare_end(4);" not in block(main, "void prepare_watchdog() {"):
        raise SystemExit("the watchdog does not end a stuck run")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    if draw.count("note_driver_fault();") < 4:
        raise SystemExit("a fault guard does not count its faults for the watchdog")
    if block(draw, "void precompile_run(").count("prepare_left_workers_note();") != 1:
        raise SystemExit("precompile_run goes on from stuck workers without saying so to the save")


def check_make_game():
    spec = importlib.util.spec_from_file_location("make_game_stall", ROOT / "nx2" / "make_game.py")
    make_game = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(make_game)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = make_game.prepare_shaders([sys.executable, "-I", "-c", "import sys; sys.exit(4)"])
    lines = [l for l in out.getvalue().splitlines() if l.startswith(make_game.NOT_PREPARED)]
    if code != 4 or len(lines) != 1 or "(the GPU driver stopped responding)" not in lines[0]:
        raise SystemExit("make_game.py does not say why for exit status 4: %r" % out.getvalue())
    print("PASS make_game: exit status 4", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    check_sources()
    check_make_game()
    if not shutil.which(args.cxx):
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="prepare-stall-") as tmp:
        check_predict(args.cxx, tmp)
        check_watchdog(args.cxx, tmp)
    print("PASS: predict_run and the preparation's watchdog")


if __name__ == "__main__":
    main()
