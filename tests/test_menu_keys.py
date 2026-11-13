"""The Insert menu's confirm and back on a controller: the buttons the Switch's A and B are bound to (gpu/vk/vk_menu_keys.h),
so A opens a page and B goes back as in the game. A Pro Controller's A (the right face button, the Nintendo layout's default)
went back instead of opening the debug tools, Motion or Controls.

The mapping is compiled and run for each layout preset's A and B (host/nn_hid.c's defaults: Nintendo, Xbox / PC, Swap Face)
and for bindings the menu cannot use. Then gpu/vk/vk_menu.cpp is checked for how menu_tick uses it: the pad's confirm and back
are those positions (press[7], press[5]), RX_VK_MENU_SCRIPT's PADS/PADE/PADW/PADN go through the same mapping, and every page
link answers to confirm (Enter, A, a click) and back (Esc, B): Motion, Controls, and the debug tools' rows on the Debug tab,
whose Teleport and Inventory open the map list and the inventory, which go back to the tab. The prompts light the same buttons.
The real keyboard polling block is also compiled to exercise both shortcut keys with queued presses and with polling.
With it, menu_tick's Esc block, gpu/vk/vk_menu.h's real Options and the Menu shortcut row's names: out of the box (no
options file, or one without menu_shortcut=) the shortcut is "Insert, F1 or Esc", and Esc opens a closed menu, as a PC
game's pause menu, without that press also going back a page; open, the press is left to go back or close it. And
menu_init's reading of hookshot_options.ini: a file from before options_version 5 wrote Insert or F1, the default then,
whether chosen or not, and loads as Insert, F1 or Esc; F1 only stays, and a file from version 5 on, up to the version
save() writes now, keeps each choice.

    python -I -B tests/test_menu_keys.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include "gpu/vk/vk_menu_keys.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <initializer_list>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::menu_keys;
static void expect(int a, int b, int confirm, int back, int line) {
    int c = -9, k = -9;
    confirm_back(a, b, &c, &k);
    if (c != confirm || k != back) {
        std::fprintf(stderr, "FAIL:%d A at %d, B at %d: confirm %d back %d, wanted %d and %d\n", line, a, b, c, k, confirm, back);
        std::exit(1);
    }
}
// KEYBOARD_FIXTURE
int main(int argc, char** argv) {
    CHECK(argc == 2);   // (the options file to write: in the test's own folder)
    ini_file = argv[1];
    test_keyboard();
    test_esc();
    test_saved();
    expect(kEast, kSouth, kEast, kSouth, __LINE__);    // (Nintendo, the default: A right, B bottom)
    expect(kSouth, kEast, kSouth, kEast, __LINE__);    // (Xbox / PC)
    expect(kWest, kNorth, kWest, kNorth, __LINE__);    // (bound by hand elsewhere on the face)
    expect(-1, -1, kSouth, kEast, __LINE__);           // (none: the bottom confirms, the right goes back)
    expect(9, 10, kSouth, kEast, __LINE__);            // (shoulders: the menu's own tabs, not usable)
    expect(kEast, -1, kEast, kSouth, __LINE__);        // (A usable alone: B the other of the two)
    expect(-1, kSouth, kSouth, kEast, __LINE__);       // (B on the default confirm: moved off it)
    expect(kNorth, kNorth, kNorth, kEast, __LINE__);   // (both on one button: B moved off it)
    for (int a = -1; a < 12; a++)
        for (int b = -1; b < 12; b++) {
            int c, k;
            confirm_back(a, b, &c, &k);
            CHECK(c >= kSouth && c <= kNorth && k >= kSouth && k <= kNorth && c != k);
        }
    CHECK(dot(kSouth) == 3 && dot(kEast) == 1 && dot(kWest) == 2 && dot(kNorth) == 0);
    std::printf("PASS: confirm and back follow A and B\n");
}
'''


KEYBOARD = r'''
enum {
    VK_INSERT = 0x2d, VK_UP = 0x26, VK_DOWN = 0x28, VK_LEFT = 0x25, VK_RIGHT = 0x27,
    VK_ESCAPE = 0x1b, VK_F1 = 0x70, VK_RETURN = 0x0d, VK_DELETE = 0x2e, VK_BACK = 8,
    VK_TAB = 9, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_HOME = 0x24, VK_END = 0x23
};
using HWND = int;
using DWORD = unsigned;
static int down_key, queued_key;
static bool focused, pad_last;
// OPTIONS
static Options opts;
static HWND GetForegroundWindow() { return 1; }
static DWORD GetCurrentProcessId() { return 1; }
static void GetWindowThreadProcessId(HWND, DWORD* pid) { *pid = focused ? 1 : 2; }
static int GetAsyncKeyState(int key) { return key == down_key ? 0x8000 : 0; }
static bool take_key_press(int key) {
    if (key != queued_key) return false;
    queued_key = 0;
    return true;
}
static struct { bool (*take_key_press)(int); float (*aspect_ratio)(); } queue_host{take_key_press, nullptr}, *host;

static bool keyboard_toggle(int mode, int key, bool queued, bool has_focus = true) {
    opts.menu_shortcut = mode;
    focused = has_focus;
    pad_last = true;
    host = queued ? &queue_host : nullptr;
    down_key = queued ? 0 : key;
    queued_key = queued ? key : 0;
    // KEY_POLL
    CHECK(!queued_key);   // Disabled keys still have to be consumed from the host queue.
    const bool toggle = press[0] || press[6];
    CHECK(pad_last != toggle);
    return toggle;
}

static void test_keyboard() {
    for (bool queued : {false, true}) {
        for (int mode = 0; mode <= 2; mode++) {   // (2: Insert, F1 or Esc)
            CHECK(!keyboard_toggle(mode, 0, queued));
            CHECK(keyboard_toggle(mode, VK_INSERT, queued) == (mode != 1));
            CHECK(!keyboard_toggle(mode, VK_INSERT, queued, false));
            CHECK(keyboard_toggle(mode, VK_F1, queued));   // (F1 always opens the menu)
            CHECK(!keyboard_toggle(mode, VK_F1, queued, false));
        }
    }
    CHECK(!keyboard_toggle(1, VK_INSERT, false));
    CHECK(!keyboard_toggle(0, VK_INSERT, false));   // A held key still needs a new press after re-enabling it.
    CHECK(!keyboard_toggle(0, 0, false));
    CHECK(keyboard_toggle(0, VK_INSERT, false));
    std::puts("PASS: menu shortcuts respect the saved mode, focus and key edges");
}

// (menu_tick's Esc block, after the keys are read: what it reaches, stubbed)
static std::atomic<bool> open_now;
static int page, ask_sel, listening;
static bool changed, tune_mode;   // (tune_mode: the compact bar, open; it opens when its key is held as the menu opens)
static bool compact_key_held;     // (menu_tick's tune_down: Right Shift, or Right Ctrl, held)
static bool restart_ask() { return false; }
static int detect_pad_layout() { return 0; }
static void dlss_probe() {}
static void fsr4_probe() {}
static void refresh_mics() {}
// TX_MENU_SHORTCUT

// One tick with key pressed (0: none) and the options o, the menu open before it or not: whether it is open after. *back:
// the Esc press left for menu_tick's pages below (back a page, or close the menu)
static bool tick_key(const Options& o, int key, bool queued, bool open_before, bool* back) {
    opts = o;
    focused = true;
    host = queued ? &queue_host : nullptr;
    down_key = queued ? 0 : key;
    queued_key = queued ? key : 0;
    // KEY_POLL
    open_now = open_before;
    page = 0, listening = 0, changed = false, tune_mode = false;
    const bool tune_down = compact_key_held;
    auto later = [] {};
    // ESC_BLOCK
    CHECK(changed == (open_now.load() != open_before) && page == 0 && listening == 0);
    // (a menu Esc opens is the full menu, or the compact bar with its key held, as with Insert and F1)
    CHECK(tune_mode == (open_now.load() && !open_before && compact_key_held));
    (void)ask_sel;
    *back = press[5];
    return open_now.load();
}
// Esc pressed and let go
static bool esc(const Options& o, bool queued, bool open_before, bool* back) {
    const bool open = tick_key(o, VK_ESCAPE, queued, open_before, back);
    bool none = true;
    CHECK(tick_key(o, 0, queued, open, &none) == open && !none);
    return open;
}

static void test_esc() {
    const Options fresh{};   // (no options file, or none of its lines menu_shortcut=: the struct's defaults, menu_init)
    CHECK(!std::strcmp(tx_menu_shortcut(fresh.menu_shortcut), "Insert, F1 or Esc"));
    for (bool queued : {false, true}) {
        bool back = false;
        CHECK(esc(fresh, queued, false, &back) && !back);   // (Esc opens a closed menu; that press does not go back too)
        CHECK(esc(fresh, queued, true, &back) && back);     // (open: the press goes back a page or closes it, further down)
        for (int mode = 0; mode <= 2; mode++) {   // (each choice as loaded: Esc opens the menu with 2 alone)
            Options saved = fresh;
            saved.menu_shortcut = mode;
            CHECK(esc(saved, queued, false, &back) == (mode == 2) && back == (mode != 2));
            CHECK(esc(saved, queued, true, &back) && back);
        }
        compact_key_held = true;   // (the compact menu key held: Esc opens the compact bar, tick_key checks tune_mode)
        CHECK(esc(fresh, queued, false, &back) && !back);
        compact_key_held = false;
    }
    std::puts("PASS: out of the box, Esc opens the closed menu (Insert, F1 or Esc); Insert or F1 and F1 only leave it closed");
}

// (menu_init's reading of hookshot_options.ini, stubbed around: the Menu shortcut row as the only row)
static std::string ini_file;
static std::string ini_path() { return ini_file; }
static int parse_output(const char*) { return 0; }
static std::string mic_want;
[[maybe_unused]] static uint64_t auto_for = 0, auto_tuned = 0;   // (the Quality preset Auto (this PC)'s fingerprints, which save() writes)
[[maybe_unused]] static struct { uint32_t w = 0, h = 0; } auto_output;   // (and the Output resolution it set, by its size)
static struct { int steps = 0; bool went_down = false; } auto_tuning;   // (and its tuning's steps)
static std::string mic_key(const char* name) { return name; }
static void mic_tell_host() {}
static int rumble_step(int v) { return v; }
struct Row { int tab; const char* name; int* value; int lo, hi; };
// INI_KEY
static Row rows[] = {{0, "Menu shortcut", &opts.menu_shortcut, 0, 2}};

// The Menu shortcut after menu_init reads a file with menu_shortcut=<shortcut> and options_version=<version> (-1: no line)
static int loaded_as(int shortcut, int version) {
    if (FILE* w = fopen(ini_file.c_str(), "w")) {
        fputs("output_res=2560x1440\n", w);
        if (shortcut >= 0) fprintf(w, "menu_shortcut=%d\n", shortcut);
        if (version >= 0) fprintf(w, "options_version=%d\n", version);
        fclose(w);
    }
    opts = Options{};
    mic_want.clear();
    bool has_options = false, has_vram_budget = false, has_memory_budget = false;
    // LOAD_OPTIONS
    CHECK(has_options && !has_vram_budget && !has_memory_budget);
    return opts.menu_shortcut;
}

static void test_saved() {
    // (from before the version Esc came with, ESC_FROM (-1, no line: before 2): Insert or F1 was the default, written
    // whether chosen or not, and moves to Insert, F1 or Esc; F1 only was chosen and stays; no line: the default)
    for (int old = -1; old < ESC_FROM; old++) {
        CHECK(loaded_as(0, old) == 2);
        CHECK(loaded_as(1, old) == 1);
        CHECK(loaded_as(2, old) == 2);
        CHECK(loaded_as(-1, old) == 2);
    }
    // (a file from ESC_FROM on, up to the version save() writes now: each choice kept, Insert or F1 too, picked after the
    // change; a later bump of save()'s version for another row must not move it again)
    for (int v = ESC_FROM; v <= SAVED_VERSION; v++) {
        for (int mode = 0; mode <= 2; mode++) CHECK(loaded_as(mode, v) == mode);
        CHECK(loaded_as(-1, v) == 2);
    }
    std::puts("PASS: an older file's Insert or F1 moves to Insert, F1 or Esc; F1 only and a file saved now keep theirs");
}
'''


def need(text, pattern, what):
    if not re.search(pattern, text, re.S):
        raise SystemExit("FAIL: vk_menu.cpp: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    tick = menu[menu.index("void menu_tick() {"):]
    key_poll = tick[tick.index("    enum { K_N ="):tick.index("    // (hookshot) the gamepads")]
    esc_at = tick.index("    const bool esc_opens")
    esc_block = tick[esc_at:tick.index("    if (open_now.load() && page == 5) {", esc_at)]
    tx_at = menu.index("const char* tx_menu_shortcut(int v) {")
    tx = menu[tx_at:menu.index("\n}\n", tx_at) + 3]
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    options = re.search(r"struct Options \{.*?\n\};", header, re.S)
    if not options:
        raise SystemExit("FAIL: vk_menu.h: the menu's Options are missing")
    # (menu_init's reading of the options file, its moves of old defaults included, and the version save() writes now)
    init = menu[menu.index("void menu_init() {"):]
    load_at = init.index('    if (FILE* f = fopen(ini_path().c_str(), "r")) {')
    load = init[load_at:init.index("    first_start = !has_options;", load_at)]
    key_at = menu.index("const char* ini_key(const Row& r) {")
    ini_key = menu[key_at:menu.index("\n}\n", key_at) + 3]
    saved_version = re.search(r'fprintf\(f, "options_version=(\d+)\\n"\);', menu[menu.index("void save() {"):])
    if not saved_version:
        raise SystemExit("FAIL: vk_menu.cpp: save() does not write options_version")
    # (the version from which a file's Menu shortcut was chosen: menu_init moves an older file's Insert or F1)
    esc_from = re.search(r"if \(version < (\d+) && opts\.menu_shortcut == 0\) opts\.menu_shortcut = 2;", load)
    if not esc_from:
        raise SystemExit("FAIL: vk_menu.cpp: menu_init does not move an older file's Insert or F1 to Insert, F1 or Esc")
    if int(esc_from.group(1)) > int(saved_version.group(1)):
        raise SystemExit("FAIL: vk_menu.cpp: save() writes a version below the Menu shortcut's move: a choice would move again")
    keyboard = (KEYBOARD.replace("// OPTIONS", options.group(0)).replace("// TX_MENU_SHORTCUT", tx)
                .replace("    // KEY_POLL", key_poll).replace("    // ESC_BLOCK", esc_block)
                .replace("// INI_KEY", ini_key).replace("    // LOAD_OPTIONS", load)
                .replace("SAVED_VERSION", saved_version.group(1)).replace("ESC_FROM", esc_from.group(1)))
    fixture = FIXTURE.replace("// KEYBOARD_FIXTURE", keyboard)
    with tempfile.TemporaryDirectory(prefix="menu-keys-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        # (_CRT_SECURE_NO_WARNINGS: the MSVC CRT's fopen deprecation, as the game's own Windows build passes)
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(ROOT), str(cpp),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(Path(tmp) / "hookshot_options.ini")], check=True)

    need(menu, r'\{TAB_CONTROLS, "Menu shortcut", &opts\.menu_shortcut, 0, 2, 1, tx_menu_shortcut,[^}]*nullptr, false, true\}',
         "the shortcut choice must be saved in Controls and exempt from presets")
    # (Insert, F1 or Esc: Esc on a closed menu opens it, and that press does not also go back a page)
    need(tick, r"const bool esc_opens = opts\.menu_shortcut == 2 && press\[5\] && !open_now\.load\(\);\s*"
               r"if \(esc_opens\) press\[5\] = false;", "Esc does not open a closed menu with the Esc shortcut")
    need(tick, r"\} else if \(press\[0\] \|\| press\[6\] \|\| esc_opens\) \{\s*open_now = !open_now\.load\(\)",
         "the Esc shortcut's press does not open the menu")
    # (the pad: confirm and back from A's and B's bindings, every tick, not fixed positions)
    need(tick, r"menu_keys::confirm_back\(host->binding\(HOS_BIND_PAD, HOS_SW_A, 0\), host->binding\(HOS_BIND_PAD, HOS_SW_B, 0\)",
         "confirm and back are not taken from A's and B's bindings")
    need(tick, r"press\[5\] \|= pad\(pad_back\)", "the pad's back is not B's button")
    need(tick, r"press\[7\] \|= pad\(pad_confirm\)", "the pad's confirm is not A's button")
    if re.search(r"press\[[57]\] \|= pad\(HOS_PAD_(SOUTH|EAST)\)", tick):
        raise SystemExit("FAIL: vk_menu.cpp: a fixed face button still confirms or goes back")
    need(tick, r'"PADS", "PADE", "PADW", "PADN".*?press\[5\] \|= p == pad_back, press\[7\] \|= p == pad_confirm',
         "RX_VK_MENU_SCRIPT's pad presses do not go through the same mapping")
    # (the page links on the tabs: confirm opens them)
    need(tick, r"else if \(r\.cheat >= 0\) cheat_press\(r\.cheat, press, changed\)", "the debug tools' rows do not act on the Debug tab")
    need(tick, r"press\[7\] && is_motion_row\(r\)\) cur_tab = TAB_MOTION", "Motion does not open on confirm")
    need(tick, r"press\[7\] && !r\.value && can_bind\(\)\) page = 1", "Controls does not open on confirm")
    need(tick, r"press\[5\] && cur_tab == TAB_MOTION\) cur_tab = TAB_CONTROLS", "back does not leave the Motion page")
    # (a click on a link row is confirm)
    need(tick, r"else if \(!d\.value\) press\[7\] = true;", "a click on a page row does not open it")
    # (the debug tools, rows of the Debug tab: confirm on Teleport opens its list, on Inventory the editor; the list pages go
    # back to the tab, which kept its selection; no page of their own any more)
    cheats = menu[menu.index("void cheat_press(int i,"):menu.index("void menu_tick() {")]
    need(cheats, r"press\[7\] && i == 8\) page = 3", "confirm on Teleport does not open the map list")
    need(cheats, r"press\[7\] && i == 10\) page = 4", "confirm on Inventory does not open the editor")
    if "page == 2" in tick or "page = 2" in tick:
        raise SystemExit("FAIL: vk_menu.cpp: the debug tools' old page is still reached")
    for page in ("4", "3"):
        block = tick[tick.index("open_now.load() && page == %s" % page):]
        need(block[:1500], r"if \(press\[5\]\) page = 0", "back from page %s does not return to the Debug tab" % page)
    need(menu, r'\{TAB_DEBUG, "Developer", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true\}',
         "the Debug tab has no Developer heading")
    for name in ("Descriptor reuse", "Sampler cache", "Robust buffers", "ASTC as RGBA8"):
        need(menu, r'\{TAB_DEBUG, "%s"' % name, name + " is not on the Debug tab")
    need(tick, r"while \(heading_at\(selected\)\)", "the selection does not step over a heading")
    # (the prompts: the buttons lit are the same ones, and the rows say what confirm does)
    need(menu, r"c\.confirm_dot = menu_keys::dot\(pad_confirm\), c\.back_dot = menu_keys::dot\(pad_back\)",
         "the prompts do not light A's and B's buttons")
    need(menu, r'"Open list"', "the Teleport row's prompt does not say it opens the list")
    print("PASS: A and B, Enter and Esc, and clicks open and leave every page")


if __name__ == "__main__":
    main()
