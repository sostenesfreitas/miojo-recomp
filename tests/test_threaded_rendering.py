"""The Insert menu's Threaded rendering (Auto / On / Off): its rule (gpu/vk/threaded_rendering.h) and how the row reaches
the replay, the recorder and the host. A compiled check of the rule plus source checks; no game, no GPU.

The rule is one function every reader shares: On is the render thread and the recorder thread together, Off neither,
and Auto picks them where the process may run on 12 or more logical processors and the mode is safe. All of Auto is
behind one constant, threaded::kAutoEnabled, which is on: turned off, Auto is Off on every PC and the row changes
nobody's default. The source checks: that constant is the only gate and is on; the row is Auto by default,
takes a restart and is in the readme and the development guide; RX_VK_ASYNC is read in one place (vk_replay.cpp
render_thread_mode, the switch over the row), which async_on takes; the recorder asks the row only
with RX_VK_RECORDER unset; and the host's present poll asks the renderer (HOS_OPT_RENDER_THREAD) instead of reading
RX_VK_ASYNC itself, so the row's On answers the poll exactly as RX_VK_ASYNC=1 does.

python -I -B tests/test_threaded_rendering.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include "threaded_rendering.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::threaded;
int main() {
    // (the row's values, in V-Sync's order)
    CHECK(AUTO == 0 && ON == 1 && OFF == 2);
    // (Auto's threshold: 12 logical processors the process may run on; an unknown count, 0, is too few)
    CHECK(kAutoCpus == 12);
    CHECK(!auto_picks(0) && !auto_picks(4) && !auto_picks(8) && !auto_picks(11));
    CHECK(auto_picks(12) && auto_picks(16) && auto_picks(32) && auto_picks(256));
    // (On and Off are the player's word: whatever the CPU, whatever is unsafe, whatever the gate)
    for (const bool gate : {false, true})
        for (const bool unsafe : {false, true})
            for (const unsigned cpus : {0u, 4u, 11u, 12u, 64u}) {
                CHECK(choose(ON, cpus, unsafe, gate).on && choose(ON, cpus, unsafe, gate).why == Why::On);
                CHECK(!choose(OFF, cpus, unsafe, gate).on && choose(OFF, cpus, unsafe, gate).why == Why::Off);
            }
    // (Auto with the gate on: the CPU's rule, and off where the mode is not safe)
    CHECK(choose(AUTO, 12, false, true).on && choose(AUTO, 12, false, true).why == Why::AutoCpus);
    CHECK(choose(AUTO, 24, false, true).on);
    CHECK(!choose(AUTO, 11, false, true).on && choose(AUTO, 11, false, true).why == Why::AutoFewCpus);
    CHECK(!choose(AUTO, 8, false, true).on && !choose(AUTO, 0, false, true).on);
    CHECK(!choose(AUTO, 12, true, true).on && choose(AUTO, 12, true, true).why == Why::AutoUnsafe);
    CHECK(!choose(AUTO, 64, true, true).on && !choose(AUTO, 4, true, true).on);
    // (Auto with the gate off: off on every PC, and said so)
    for (const bool unsafe : {false, true})
        for (const unsigned cpus : {0u, 4u, 11u, 12u, 64u})
            CHECK(!choose(AUTO, cpus, unsafe, false).on && choose(AUTO, cpus, unsafe, false).why == Why::AutoGate);
    // (a value that is none of the row's: Auto)
    CHECK(choose(7, 64, false, true).why == Why::AutoCpus && choose(-1, 64, false, false).why == Why::AutoGate);
    // (as built: the gate is on, so Auto is its rule: on from 12 logical processors where the mode is safe)
    CHECK(kAutoEnabled);
    for (const unsigned cpus : {0u, 8u, 11u}) CHECK(!choose(AUTO, cpus, false).on);
    for (const unsigned cpus : {12u, 16u, 64u}) CHECK(choose(AUTO, cpus, false).on && !choose(AUTO, cpus, true).on);
    CHECK(choose(ON, 4, false).on && !choose(OFF, 64, false).on);
    std::printf("PASS: the Threaded rendering rule\n");
}
'''


def block(text, marker):
    """From marker to the brace closing its first one"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    vk = ROOT / "gpu" / "vk"
    rule = (vk / "threaded_rendering.h").read_text(encoding="utf-8")
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (vk / "vk_menu.h").read_text(encoding="utf-8")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    recorder = (vk / "vk_recorder.h").read_text(encoding="utf-8")
    api = (vk / "vk_api.h").read_text(encoding="utf-8")
    native = (ROOT / "host" / "nvn_native.c").read_text(encoding="utf-8")

    # (the gate: one constant, on, and Auto's rule behind it)
    if len(re.findall(r"constexpr bool \w+ = ", rule)) != 1 or "constexpr bool kAutoEnabled = true;" not in rule:
        raise SystemExit("threaded_rendering.h: Auto's gate is not the one constant kAutoEnabled, on (turning it off changes "
                         "the default for every PC with 12 threads: that is its own change)")
    choose = block(rule, "constexpr Choice choose(")
    if "bool auto_enabled = kAutoEnabled" not in choose or "if (!auto_enabled) return {false, Why::AutoGate};" not in choose:
        raise SystemExit("threaded_rendering.h: choose() no longer keeps Auto behind the gate")

    # (the row: Auto by default, read once so it takes a restart, no preset sets it)
    if "    int threaded_rendering = 0;" not in header:
        raise SystemExit("vk_menu.h: the Threaded rendering option is not Auto (0) out of the box")
    row = re.search(r'\{TAB_GRAPHICS, "Threaded rendering", &opts\.threaded_rendering, 0, 2, 1, tx_vsync,[^{}]*\}', menu)
    if not row or not row.group(0).endswith("hn_threaded, true, true}"):
        raise SystemExit("vk_menu.cpp: no Threaded rendering row (Auto, On, Off), or it no longer takes a restart")
    if "after a restart" not in row.group(0).replace('"\n     "', ""):
        raise SystemExit("vk_menu.cpp: the Threaded rendering row's help does not say it applies after a restart")
    resolved = block(menu, "bool menu_threaded_rendering() {")
    for need in ("static const bool on = [] {", "menu_init();", "usable_cpus()", "threaded::choose(opts.threaded_rendering, cpus,",
                 '"RX_VK_CAPTURE"', '"RX_VK_LOW_SPEC"', '"RX_VK_DUMP"', '"RX_VK_BENCH"', "pools_mirrored()"):
        if need not in resolved:
            raise SystemExit("vk_menu.cpp: menu_threaded_rendering() lost " + need + " (resolved once, from the processors the "
                             "process may run on, and off for Auto where the mode is not safe)")
    if "RX_VK_ASYNC" in resolved or "RX_VK_RECORDER" in resolved:
        raise SystemExit("vk_menu.cpp: menu_threaded_rendering() is the row's answer alone: the switches are read over it by "
                         "their own readers")

    # (the replay: RX_VK_ASYNC in one place, the switch over the row, and the row's On exactly RX_VK_ASYNC=1)
    mode = block(replay, "int render_thread_mode() {")
    if 'const char* e = getenv("RX_VK_ASYNC");' not in mode or "return e ? atoi(e) : menu_threaded_rendering() ? 1 : 0;" not in mode:
        raise SystemExit("vk_replay.cpp: render_thread_mode() is no longer RX_VK_ASYNC when set, else the row as 1 or 0")
    if "render_thread_mode() != 0" not in block(replay, "bool async_on() {"):
        raise SystemExit("vk_replay.cpp: async_on() no longer reads render_thread_mode()")
    for path in sorted(list(vk.glob("*.cpp")) + list(vk.glob("*.h")) + list((ROOT / "host").glob("*.c"))):
        text = path.read_text(encoding="utf-8", errors="replace")
        reads = len(re.findall(r'getenv\("RX_VK_ASYNC"\)', text))
        # (vk_menu.cpp's one is threaded_env, the row's line that says the switch is set: it decides nothing)
        if reads != (1 if path.name in ("vk_replay.cpp", "vk_menu.cpp") else 0):
            raise SystemExit("%s reads RX_VK_ASYNC itself (%d times): only render_thread_mode() decides by it, once, so the row "
                             "and the switch cannot disagree" % (path.relative_to(ROOT).as_posix(), reads))
    if 'getenv("RX_VK_ASYNC")' not in block(menu, "const char* threaded_env() {"):
        raise SystemExit("vk_menu.cpp: RX_VK_ASYNC is read outside threaded_env(), the row's note that the switch is set")

    # (the recorder: the switch when set; unset, the row, through the pointer vk_replay.cpp sets)
    from_env = block(recorder, "inline int mode_from_env(bool* refused) {")
    if 'if (!e && g_setting_thread && g_setting_thread()) e = "thread";' not in from_env:
        raise SystemExit("vk_recorder.h: with RX_VK_RECORDER unset the recorder no longer asks the Threaded rendering row")
    if "inline bool (*g_setting_thread)() = nullptr;" not in recorder or \
            "(rec::g_setting_thread = menu_threaded_rendering, true)" not in replay:
        raise SystemExit("the recorder's question to the row is not set up (vk_recorder.h g_setting_thread, vk_replay.cpp)")

    # (the host: the present poll asks the renderer, which answers async_on())
    if "enum { HOS_OPT_RENDER_THREAD = 58 };" not in api:
        raise SystemExit("vk_api.h: HOS_OPT_RENDER_THREAD is not option 58")
    if "if (id == HOS_OPT_RENDER_THREAD) return async_on();" not in block(replay, "t.option = [](int id) -> int {"):
        raise SystemExit("vk_replay.cpp: the option callback no longer answers HOS_OPT_RENDER_THREAD with async_on()")
    poll = block(native, "NV(nvnEventGetValue) {")
    if "now = e ? atoi(e) != 0 : rx_option(HOS_OPT_RENDER_THREAD, -1);" not in poll or 'getenv("HOS_PRESENT_NOWAIT")' not in poll:
        raise SystemExit("nvn_native.c: the present poll is not HOS_PRESENT_NOWAIT when set, else the renderer's "
                         "HOS_OPT_RENDER_THREAD")

    # (said where the settings and the switches are)
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    docs = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    if "| | Threaded rendering | Auto, On, Off" not in readme or "- **Threaded rendering:**" not in readme:
        raise SystemExit("readme.md: no Threaded rendering line in the option table, or no paragraph on it")
    if "threaded::kAutoEnabled" not in docs or "| Threaded rendering (the Insert menu's row" not in docs:
        raise SystemExit("docs/development.md: no row for Threaded rendering, or it does not name the gate")

    with tempfile.TemporaryDirectory(prefix="threaded-rendering-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(vk), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: Threaded rendering: the rule, the gate (off), and the row's way to the replay, the recorder and the host")


if __name__ == "__main__":
    main()
