"""Clicking a DualShock 4's or DualSense's touchpad opens the Insert menu, as F1 does.

SDL numbers the touchpad click button 20 (SDL_CONTROLLER_BUTTON_TOUCHPAD in SDL2, SDL_GAMEPAD_BUTTON_TOUCHPAD in SDL3),
past the buttons host/pad_sdl.c reads for the game. Its poll turns that button into HOS_PAD_TOUCHPAD, and
gpu/vk/vk_menu.cpp presses F1 (press[6]) with it: the menu opens, and closes again, as the key does. The touchpad is not
one of the inputs the Controls page binds, so its bit is past HOS_PAD_N.

The poll's button block is compiled over a stand-in SDL, which reports one button at a time.
It also checks that all four paddle buttons have their own bindable bits.

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
    /* (its own bit, past the positions a Switch button can be bound to) */
    CHECK((int)HOS_PAD_TOUCHPAD >= (int)HOS_PAD_N && (int)HOS_PAD_TOUCHPAD < 32);
    CHECK(poll_button(-1) == 0);
    CHECK(poll_button(BTN_TOUCHPAD) == 1ULL << HOS_PAD_TOUCHPAD);
    CHECK(poll_button(BTN_A) == 1ULL << HOS_PAD_SOUTH);
    CHECK(poll_button(16) == 1ULL << HOS_PAD_RPADDLE1);
    CHECK(poll_button(17) == 1ULL << HOS_PAD_LPADDLE1);
    CHECK(poll_button(18) == 1ULL << HOS_PAD_RPADDLE2);
    CHECK(poll_button(19) == 1ULL << HOS_PAD_LPADDLE2);
    printf("PASS: SDL's touchpad click sets its own bit, past the bindable inputs\n");
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
    need(tick, r"if \(pad\(HOS_PAD_TOUCHPAD\)\) press\[6\] = true;",
         "vk_menu.cpp: a touchpad click does not open the menu as F1")
    if re.search(r"press\[(0|5|7)\] \|=? ?pad\(HOS_PAD_TOUCHPAD\)", tick):
        raise SystemExit("FAIL: vk_menu.cpp: the touchpad also acts as Insert, confirm or back")
    # (a touchpad click while a binding listens is not one: it cannot be bound)
    need(tick, r"pad_press & ~\(1u << HOS_PAD_GUIDE\) & ~\(1u << HOS_PAD_TOUCHPAD\)",
         "vk_menu.cpp: the Controls page binding can take the touchpad click")
    print("PASS: a DualShock 4 / DualSense touchpad click opens the menu as F1")


if __name__ == "__main__":
    main()
