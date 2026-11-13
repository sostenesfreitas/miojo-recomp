"""The two ways a start prepares the shaders (gpu/vk/vk_main.cpp) run the same steps: the preparing screen's worker
(prepare_with_screen) and the start with console progress (started_up). With RX_VK_PREPARE_BACKGROUND=1 an update builds
what the notes name before the game and the rest behind it; both must make the shipped state list's pipelines first
(state_list_run, before precompile_run). The screen's worker left them out, so they were built when first drawn, in play.

Both step sequences are cut from the source and run against stand-ins that record each call:
- an update: state_list_run once, before precompile_run, then prepare_background; no prepare_run, no predict_run
- not an update: no state_list_run, no prepare_background
- the screen's worker stopped (the window closed): no state list and no precompile
No game or GPU required.

python -I -B tests/test_prepare_screen_sequence.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
static int fails;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static std::string calls;
static bool is_update, stopped;
typedef void (*Progress)(size_t, size_t);
static bool prepare_is_update() { return is_update; }
static void prepare_guess(size_t* g) { g[0] = g[1] = g[2] = 0; }
static bool prepare_run(Progress, bool) { calls += "prepare "; return true; }
static bool predict_pending() { return false; }
static bool prepare_stopped() { return stopped; }
static void predict_run(Progress) { calls += "predict "; }
static void state_list_run() { calls += "state_list "; }
static void precompile_run(Progress) { calls += "precompile "; }
static void pipeline_cache_save(bool) { calls += "save "; }
static void prepare_background() { calls += "background "; }
static void SetEvent(void*) {}
static void* prep_ended;
static std::atomic<size_t> prep_guess[3], prep_done, prep_total;
static std::atomic<bool> prep_skip[2], prep_finished;
static std::atomic<int> prep_stage;
static void prep_progress(size_t, size_t) {}
static void progress(size_t, size_t) {}

static void screen() {
/* SCREEN */
}
static void console() {
/* CONSOLE */
}

static std::string run(void (*f)(), bool update, bool stop) {
    calls.clear(), is_update = update, stopped = stop;
    f();
    return calls;
}

int main() {
    for (auto f : {screen, console}) {
        const char* name = f == screen ? "screen" : "console";
        const std::string u = run(f, true, false);
        if (u.find("state_list precompile ") == std::string::npos || u.find("background") == std::string::npos ||
            u.find("prepare ") != std::string::npos || u.find("predict ") != std::string::npos)
            std::printf("FAIL %s, an update: %s\n", name, u.c_str()), fails++;
        const std::string n = run(f, false, false);
        if (n.find("state_list") != std::string::npos || n.find("background") != std::string::npos ||
            n.find("precompile ") == std::string::npos)
            std::printf("FAIL %s, not an update: %s\n", name, n.c_str()), fails++;
    }
    const std::string s = run(screen, true, true);
    CHECK(s.find("state_list") == std::string::npos && s.find("precompile") == std::string::npos);
    return fails ? 1 : 0;
}
'''


def between(source, start, end):
    i = source.index(start)
    j = source.index(end, i) + len(end)
    return source[i:j]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    src = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    worker = src[src.index("void prepare_with_screen() {"):]
    screen = between(worker, "const bool update = prepare_is_update();", "SetEvent(prep_ended);")
    console = between(src[src.index("bool started_up() {"):], "const bool update = prepare_is_update();",
                      "if (update) prepare_background();")
    code = DRIVER.replace("/* SCREEN */", screen).replace("/* CONSOLE */", console)
    with tempfile.TemporaryDirectory(prefix="prepare-sequence-") as tmp:
        cpp, exe = Path(tmp) / "sequence.cpp", Path(tmp) / "sequence.exe"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        str(cpp), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: the preparing screen's steps and the console start's differ:\n" + r.stdout + r.stderr)
    print("PASS: an update makes the state list's pipelines before the rest, behind the preparing screen as with the console")


if __name__ == "__main__":
    main()
