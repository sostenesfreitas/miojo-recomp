"""A DualShock 4's or DualSense's touchpad click: a Controls row can take it, and while none does it opens the Insert menu, as
F1 does.

SDL numbers the touchpad click button 20 (SDL_CONTROLLER_BUTTON_TOUCHPAD in SDL2, SDL_GAMEPAD_BUTTON_TOUCHPAD in SDL3),
past the buttons host/pad_sdl.c reads for the game. Its poll turns that button into HOS_PAD_TOUCHPAD, the gamepad input
after the paddles. gpu/vk/vk_menu.cpp presses F1 (press[6]) with it while no row has it as Input 1 or Input 2.

The poll's button block is compiled over a stand-in SDL, which reports one button at a time; it also checks that all four
paddle buttons have their own bindable bits. Then the menu's Controls table, touchpad_bound and pad_name, its F1 line and
its listening binding's mask are cut from vk_menu.cpp and run with a stand-in host's bindings: unbound, a click is F1;
bound to Minus (or an ocarina note) it is not; a listening binding takes the click; its name is "Touchpad".

    python -I -B tests/test_touchpad_menu.py [--cxx clang++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def need(text, pattern, what):
    if not re.search(pattern, text, re.S):
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    pad = (ROOT / "host/pad_sdl.c").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")

    # (the buttons, exactly as pad_sdl.c numbers them: SDL2 and SDL3 agree up to the touchpad)
    m = re.search(r"^enum \{ BTN_A,.*?BTN_N = \d+ \};$", pad, re.M | re.S)
    if not m:
        raise SystemExit("host/pad_sdl.c: no BTN_A ... BTN_N enum")
    buttons = m.group(0)
    # (the block that builds the polled buttons from SDL's, triggers aside)
    start = pad.index("            uint64_t sig = 0;\n            for (int k = BTN_A; k <= BTN_RIGHT; k++)")
    end = pad.index("            if (hos_sdl_pad_axis(gc, AX_LT)", start)
    block = pad[start:end]
    fixture = r'''#include "gpu/vk/vk_api.h"
#include <stdint.h>
#include <stdio.h>
''' + buttons + r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
static int pressed;
static int hos_sdl_pad_button(void *gc, int button) { (void)gc; return button == pressed; }
static uint64_t poll_button(int button) {
    pressed = button;
    void *gc = NULL;
    uint64_t b = 0;
''' + block + r'''
    return b;
}
int main(void) {
    /* (SDL2's SDL_CONTROLLER_BUTTON_TOUCHPAD and SDL3's SDL_GAMEPAD_BUTTON_TOUCHPAD are both 20) */
    CHECK(BTN_TOUCHPAD == 20);
    /* (the gamepad input after the paddles, which a Switch button can be bound to; the menu's masks are 32 bits) */
    CHECK((int)HOS_PAD_TOUCHPAD == (int)HOS_PAD_LPADDLE2 + 1 && (int)HOS_PAD_TOUCHPAD < (int)HOS_PAD_N && HOS_PAD_N <= 32);
    CHECK(poll_button(-1) == 0);
    CHECK(poll_button(BTN_TOUCHPAD) == 1ULL << HOS_PAD_TOUCHPAD);
    CHECK(poll_button(BTN_A) == 1ULL << HOS_PAD_SOUTH);
    CHECK(poll_button(16) == 1ULL << HOS_PAD_RPADDLE1);
    CHECK(poll_button(17) == 1ULL << HOS_PAD_LPADDLE1);
    CHECK(poll_button(18) == 1ULL << HOS_PAD_RPADDLE2);
    CHECK(poll_button(19) == 1ULL << HOS_PAD_LPADDLE2);
    printf("PASS: SDL's touchpad click sets its own bit, a bindable input\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="touchpad-menu-") as tmp:
        cpp, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        # (the runner finds a C++ compiler; the fixture is C, which its -x c and a C standard compile)
        subprocess.run([a.cxx, "-x", "c", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT), str(cpp),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

    tick = menu[menu.index("void menu_tick() {"):]
    # press[6] is F1 (the key that always opens the menu), which the touchpad presses
    need(tick, r"keys\[K_N\] = \{VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1,",
         "vk_menu.cpp: press[6] is not F1")
    if re.search(r"press\[(0|5|7)\] \|=? ?pad\(HOS_PAD_TOUCHPAD\)", tick):
        raise SystemExit("FAIL: vk_menu.cpp: the touchpad also acts as Insert, confirm or back")
    f1 = re.search(r"^ *if \(pad\(HOS_PAD_TOUCHPAD\)[^\n]*press\[6\] = true;\n", tick, re.M)
    listen = re.search(r"else if \(const uint32_t p = (pad_press [^\n]*?)\) \{", tick)
    if not f1 or not listen:
        raise SystemExit("FAIL: vk_menu.cpp: no touchpad F1 line or no listening binding's mask in menu_tick")
    table = menu[menu.index("struct Control {"):menu.index("const char* key_name(")]
    driver = r"""#define _CRT_SECURE_NO_WARNINGS 1
#define _Static_assert static_assert   // (rx_nvn.h is C)
#include "gpu/vk/vk_api.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static int fails;
static const MkwVkHost* host;
static struct { int prompts; } opts;
static int bound[HOS_KEY_N][2];
static int binding(int set, int button, int slot) { return set == HOS_BIND_PAD ? bound[button][slot] : 0; }
""" + table + r"""
static bool f1(uint32_t pad_press, int listening) {
    bool press[8] = {};
    auto pad = [&](int p) { return listening != 1 && (pad_press >> p & 1) != 0; };
""" + f1.group(0) + r"""    return press[6];
}
static int listened(uint32_t pad_press) {   // (the input a listening binding takes, -1 none)
    if (const uint32_t p = """ + listen.group(1) + r""") {
        int bit = 0;
        while (!(p >> bit & 1)) bit++;
        return bit;
    }
    return -1;
}
int main() {
    MkwVkHost h{};
    h.binding = binding;
    host = &h;
    for (auto& b : bound) b[0] = b[1] = -1;
    bound[HOS_SW_MINUS][0] = HOS_PAD_BACK;
    const uint32_t click = 1u << HOS_PAD_TOUCHPAD;
    CHECK(f1(click, 0));   // (unbound: the menu, as now)
    CHECK(!f1(click, 1));   // (not while a binding listens)
    CHECK(listened(click) == HOS_PAD_TOUCHPAD);
    CHECK(listened(click | 1u << HOS_PAD_GUIDE) == HOS_PAD_TOUCHPAD);
    bound[HOS_SW_MINUS][1] = HOS_PAD_TOUCHPAD;
    CHECK(!f1(click, 0));   // (Minus's Input 2: the click is Minus's)
    bound[HOS_SW_MINUS][1] = -1, bound[HOS_KEY_OC_NOTE_A][0] = HOS_PAD_TOUCHPAD;
    CHECK(!f1(click, 0));   // (an ocarina note's)
    bound[HOS_KEY_OC_NOTE_A][0] = -1;
    CHECK(f1(click, 0));   // (unbound again)
    CHECK(!std::strcmp(pad_name(HOS_PAD_TOUCHPAD), "Touchpad"));
    CHECK(!std::strcmp(pad_name(HOS_PAD_LPADDLE2), "L5"));
    return fails ? 1 : 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="touchpad-bind-") as tmp:
        cpp, exe = Path(tmp) / "menu.cpp", Path(tmp) / "menu.exe"
        cpp.write_text(driver, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        "-I" + str(ROOT), str(cpp), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: the touchpad click and the Controls page:\n" + r.stdout + r.stderr)
    print("PASS: a touchpad click is a Controls input; unbound, it opens the menu as F1")


if __name__ == "__main__":
    main()
