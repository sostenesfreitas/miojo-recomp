"""The V-Sync row chooses the swapchain's present mode, at once; RX_VK_FIFO and RX_VK_IMMEDIATE go over it for a run.

Auto is what the renderer chose before the row (mailbox where the driver has it; on Windows immediate when it has no
mailbox), On is FIFO and Off immediate. The row is saved as vsync= and no Quality preset sets it. The environment's
switches apply where the mode is chosen (menu_vsync), so they never reach hookshot_options.ini. Compile menu_vsync with
the menu's Options, run create_swapchain's own choice of mode over the lists drivers give (as on Windows and on Linux),
and check the swapchain and present code by their text. No game or GPU is needed.

V-Sync Auto is FIFO for NVIDIA DLSS Frame Generation only while DLSS-G makes the frames (swapchain_dlssg and vk_dlss.cpp's
dlss_fg_runs, the real ones, compiled into the choice): asked for 3 made frames (4x) of a runtime that makes 1 at most, the
renderer's own makes them and Auto keeps its mailbox. It stayed FIFO, which held the game at the refresh over the multiplier
(issue 2695: 36 fps on 144 Hz).
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated block: " + marker)


FIXTURE = r"""
#include <cstdio>
#include <cstdlib>
#include "hookshot_env.h"
#ifdef _WIN32
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
/* OPTIONS */
Options opts;
/* VSYNC */
int main() {
    CHECK(opts.vsync == 0);   // (Auto: the renderer's choice before the row)
    for (int v = 0; v <= 2; v++) {
        opts.vsync = v;
        CHECK(menu_vsync() == v);
    }
    setenv("RX_VK_FIFO", "0", 1);
    CHECK(menu_vsync() == 2);   // (=0: off, the row's: env_on)
    setenv("RX_VK_FIFO", "1", 1);
    CHECK(menu_vsync() == 1 && opts.vsync == 2);   // (for the run; the row keeps the player's)
    unsetenv("RX_VK_FIFO");
    setenv("RX_VK_IMMEDIATE", "1", 1);
    opts.vsync = 1;
    CHECK(menu_vsync() == 2 && opts.vsync == 1);
    std::puts("PASS: V-Sync: the row's choice, the environment's over it for a run");
}
"""


# (create_swapchain's choice of mode, the real lines, over what drivers list: Windows and Linux)
CHOICE = r"""
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>
enum VkPresentModeKHR { VK_PRESENT_MODE_IMMEDIATE_KHR = 0, VK_PRESENT_MODE_MAILBOX_KHR = 1, VK_PRESENT_MODE_FIFO_KHR = 2 };
enum DlssWhy { DLSS_OK, DLSS_NEEDS_RTX };
struct Options { int framegen = 0; } opts;   // (the Frame generation row: 0 off, 1 Auto, 2 to 8 the multiplier)
int row = 0;
bool asked = false;         // (Frame generation by: DLSS-G asked for)
DlssWhy why = DLSS_OK;      // (whether DLSS runs on this device)
// (vk_dlss.cpp's Fg, what dlss_fg_runs reads: NGX's no or its feature given up, and the most frames its runtime makes once
// NGX has answered, 0 before)
struct Fg { std::atomic<bool> start_failed{false}; std::atomic<unsigned> most_known{0}; };
Fg& fgs() { static Fg f; return f; }
int menu_vsync() { return row; }
bool fg_asked() { return asked; }
bool dlss_fg_wanted() { return asked; }
DlssWhy dlss_why() { return why; }
void logf(const char*, ...) {}
/* RUNS */
/* FRAMEGEN */
/* DLSSG */
VkPresentModeKHR choose(const std::vector<VkPresentModeKHR>& modes) {
/* CHOICE */
    return mode;
}
int main() {
    const VkPresentModeKHR I = VK_PRESENT_MODE_IMMEDIATE_KHR, M = VK_PRESENT_MODE_MAILBOX_KHR, F = VK_PRESENT_MODE_FIFO_KHR;
    const std::vector<VkPresentModeKHR> all{F, M, I}, no_mailbox{F, I}, fifo_only{F};
    const bool windows = HOOKSHOT_TEST_WINDOWS;
    struct Case { int row; const std::vector<VkPresentModeKHR>* modes; VkPresentModeKHR want; } cases[] = {
        {0, &all, M}, {0, &no_mailbox, windows ? I : F}, {0, &fifo_only, F},   // (Auto: as before the row)
        {1, &all, F}, {1, &no_mailbox, F}, {1, &fifo_only, F},                 // (On: FIFO)
        {2, &all, I}, {2, &no_mailbox, I}, {2, &fifo_only, F},                 // (Off: immediate where there is one)
    };
    for (const Case& c : cases) {
        row = c.row;
        if (choose(*c.modes) != c.want) {
            std::fprintf(stderr, "FAIL: V-Sync %d chose mode %d, wanted %d\n", c.row, (int)choose(*c.modes), (int)c.want);
            return 1;
        }
    }
    // (the renderer's own frame generation at 2x and up, or DLSS-G that does not run here: as without it)
    opts.framegen = 3;
    for (const Case& c : cases) {
        row = c.row;
        if (choose(*c.modes) != c.want) {
            std::fprintf(stderr, "FAIL: V-Sync %d with the renderer's own frame generation chose mode %d, wanted %d\n", c.row,
                         (int)choose(*c.modes), (int)c.want);
            return 1;
        }
    }
    // (DLSS Frame Generation at 2x and up: Auto is FIFO, mailbox or not; On and Off as without it)
    asked = true;
    const Case with_fg[] = {
        {0, &all, F}, {0, &no_mailbox, F}, {0, &fifo_only, F},
        {1, &all, F}, {2, &all, I}, {2, &no_mailbox, I},
    };
    for (const Case& c : with_fg) {
        row = c.row;
        if (choose(*c.modes) != c.want) {
            std::fprintf(stderr, "FAIL: V-Sync %d with frame generation chose mode %d, wanted %d\n", c.row,
                         (int)choose(*c.modes), (int)c.want);
            return 1;
        }
    }
    // (DLSS-G asked for, but the renderer's own makes the frames: Auto as without DLSS-G. `most`: what NGX said its runtime
    // makes between two game frames, 0 before its answer (the first present); given_up: NGX's no, or its feature given up)
    struct Made { int framegen; unsigned most; bool given_up; DlssWhy why; bool dlssg; } made[] = {
        {4, 0, false, DLSS_OK, true},    // (NGX not asked yet: taken to make them, the swapchain made again once if not)
        {2, 1, false, DLSS_OK, true},    // (2x of a runtime that makes 1: DLSS-G's)
        {3, 1, false, DLSS_OK, false},   // (3x and 4x of it: the renderer's own, issue 2695)
        {4, 1, false, DLSS_OK, false},
        {4, 3, false, DLSS_OK, true},    // (a runtime that makes 3: up to 4x)
        {5, 3, false, DLSS_OK, false},   // (5x and up asks for more, though the swapchain's images stop at 4x's)
        {8, 3, false, DLSS_OK, false},
        {2, 1, true, DLSS_OK, false},    // (given up)
        {2, 1, false, DLSS_NEEDS_RTX, false},   // (a device DLSS does not run on)
        {1, 3, false, DLSS_OK, false},   // (Auto and off: no fixed multiplier)
        {0, 3, false, DLSS_OK, false},
    };
    for (const Made& c : made) {
        opts.framegen = c.framegen, fgs().most_known = c.most, fgs().start_failed = c.given_up, why = c.why;
        const VkPresentModeKHR auto_all = c.dlssg ? F : M, auto_none = c.dlssg ? F : windows ? I : F;
        row = 0;
        if (swapchain_dlssg() != c.dlssg || choose(all) != auto_all || choose(no_mailbox) != auto_none) {
            std::fprintf(stderr, "FAIL: frame generation %dx, DLSS-G's runtime making %u, given up %d, DLSS %d: the swapchain "
                         "for DLSS-G %d (wanted %d), V-Sync Auto chose mode %d (wanted %d)\n", c.framegen, c.most,
                         (int)c.given_up, (int)c.why, (int)swapchain_dlssg(), (int)c.dlssg, (int)choose(all), (int)auto_all);
            return 1;
        }
        row = 1;
        if (choose(all) != F) return std::fprintf(stderr, "FAIL: V-Sync On is not FIFO at %dx\n", c.framegen), 1;
        row = 2;
        if (choose(all) != I) return std::fprintf(stderr, "FAIL: V-Sync Off is not immediate at %dx\n", c.framegen), 1;
    }
    std::puts(windows ? "PASS: the present mode for each V-Sync choice (Windows)" : "PASS: the present mode for each V-Sync choice (Linux)");
}
"""


def need(text, pattern, why):
    if not re.search(pattern, text, re.S):
        raise SystemExit("FAIL: " + why)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    main_cpp = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")

    need(menu, r'\{TAB_GRAPHICS, "V-Sync", &opts\.vsync, 0, 2, 1, tx_vsync,[^}]*hn_vsync, false, true\}',
         "no V-Sync row on the Graphics tab (Auto, On, Off; applied at once; leaves the Quality preset alone)")
    need(menu, r'if \(r\.value == &opts\.vsync\) return "vsync";', "the V-Sync row is not saved as vsync=")
    swap = block(main_cpp, "bool create_swapchain(")
    need(swap, r"menu_vsync\(\)", "create_swapchain does not ask the V-Sync row")
    if "RX_VK_FIFO" in swap.replace("RX_VK_FIFO=1 On", "") or 'getenv("RX_VK_IMMEDIATE")' in swap:
        raise SystemExit("FAIL: create_swapchain reads RX_VK_FIFO / RX_VK_IMMEDIATE itself: menu_vsync does")
    need(swap, r"if \(vsync != 1\)", "V-Sync On does not keep FIFO")
    need(swap, r"want_immediate = vsync == 2", "V-Sync Off does not ask for immediate")
    present = block(replay, "void present_image(")
    need(present, r"vsync_changed", "a V-Sync change does not make the swapchain again")
    need(present, r"vsync_now = [^;]*swapchain_dlssg\(\)", "DLSS-G starting or stopping to make the frames does not make the "
         "swapchain again (V-Sync Auto's mode follows it)")
    # (the present mode follows whether DLSS-G makes the frames: the real swapchain_dlssg and dlss_fg_runs go into the choice)
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    if "bool dlss_fg_runs(int frames) {" not in dlss:
        raise SystemExit("FAIL: dlss_fg_runs is not asked for a number of made frames: V-Sync Auto stays FIFO when DLSS-G's "
                         "runtime makes fewer than the multiplier asks and the renderer's own makes them")
    runs = block(dlss, "bool dlss_fg_runs(int frames) {")
    framegen = re.search(r"int swapchain_framegen\(\) \{[^\n]*\}", main_cpp)
    dlssg = re.search(r"bool swapchain_dlssg\(\) \{[^\n]*\}", main_cpp)
    if not framegen or not dlssg:
        raise SystemExit("FAIL: vk_main.cpp: no swapchain_framegen or swapchain_dlssg")
    start = block(dlss, "bool fg_start(Fg& g) {")
    need(start, r"g\.most_known = g\.most;\n    g\.start_failed = false;",
         "fg_start does not publish the most frames the runtime makes (dlss_fg_runs reads it)")
    need(block(dlss, "int dlss_fg_frames("), r"if \(\(unsigned\)count > g\.most\) \{",
         "dlss_fg_frames does not leave a set of more frames than the runtime makes to the renderer's own")

    choice = swap[swap.index("    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;"):swap.index("    VkSwapchainCreateInfoKHR ci{")]
    choice = choice.replace("#ifdef _WIN32", "#if HOOKSHOT_TEST_WINDOWS")
    with tempfile.TemporaryDirectory(prefix="vsync-choice-") as tmp:
        for windows in (0, 1):
            src = Path(tmp) / "choice.cpp"
            exe = Path(tmp) / ("choice%d%s" % (windows, ".exe" if os.name == "nt" else ""))
            src.write_text(CHOICE.replace("/* CHOICE */", choice).replace("/* RUNS */", runs)
                           .replace("/* FRAMEGEN */", framegen.group(0)).replace("/* DLSSG */", dlssg.group(0)),
                           encoding="utf-8")
            if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
                command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/DHOOKSHOT_TEST_WINDOWS=%d" % windows, str(src),
                           "/Fo" + str(Path(tmp) / "choice.obj"), "/Fe" + str(exe)]
            else:
                command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-DHOOKSHOT_TEST_WINDOWS=%d" % windows,
                           str(src), "-o", str(exe)]
            subprocess.run(command, cwd=tmp, check=True)
            subprocess.run([str(exe)], check=True)

    options = re.search(r"struct Options \{.*?\n\};", header, re.S)
    vsync = re.search(r"int menu_vsync\(\) \{[^\n]*\}", menu)
    if not options or not vsync:
        raise SystemExit("FAIL: the menu's Options or menu_vsync is missing")
    with tempfile.TemporaryDirectory(prefix="vsync-row-") as tmp:
        src = Path(tmp) / "vsync.cpp"
        exe = Path(tmp) / ("vsync.exe" if os.name == "nt" else "vsync")
        src.write_text(FIXTURE.replace("/* OPTIONS */", options.group(0)).replace("/* VSYNC */", vsync.group(0)),
                       encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(ROOT / "gpu" / "vk" / "compat"), str(src),
                       "/Fo" + str(Path(tmp) / "vsync.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                       "-I", str(ROOT / "gpu" / "vk" / "compat"), str(src), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_FIFO", "RX_VK_IMMEDIATE")}
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
