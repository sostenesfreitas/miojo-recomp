"""Linux's keys (host/posix/display_sdl.c keys_update) publish all five of SDL's mouse buttons as Windows' virtual keys, so
the Controls page can bind Mouse 4 and Mouse 5 there as on Windows: SDL's mask 1, 2, 4, 8, 16 is VK_LBUTTON, VK_MBUTTON,
VK_RBUTTON, VK_XBUTTON1 (5), VK_XBUTTON2 (6); nothing while the game has no window. keys_update and host/posix/windows.h's
virtual keys are cut from the sources and built with stand-ins for SDL. No SDL, window or game required.

python -I -B tests/test_sdl_mouse_buttons.py [--cc gcc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* VK */
static uint8_t sdl_keys[512];
static uint32_t sdl_buttons;
static const uint8_t *hos_sdl_keys(int *n) { *n = (int)sizeof sdl_keys; return sdl_keys; }
static uint32_t hos_sdl_mouse_buttons(void) { return sdl_buttons; }
static void *hos_posix_window[1];
static volatile uint8_t hos_posix_keys[256];
/* KEYS_UPDATE */
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static int held(int vk) { return hos_posix_keys[vk] != 0; }

int main(void) {
    static const int vk[5] = {0x01, 0x04, 0x02, 0x05, 0x06};   /* (left, middle, right, Mouse 4, Mouse 5) */
    hos_posix_window[0] = &fails;
    for (int i = 0; i < 5; i++) {
        sdl_buttons = 1u << i;
        keys_update();
        for (int j = 0; j < 5; j++)
            if (held(vk[j]) != (i == j))
                printf("FAIL: SDL's button mask %u: virtual key %d is %d\n", sdl_buttons, vk[j], held(vk[j])), fails++;
    }
    sdl_buttons = 8 | 16;
    keys_update();
    CHECK(held(0x05) && held(0x06) && !held(0x01));
    hos_posix_window[0] = NULL;   /* (no window: no keys and no buttons) */
    keys_update();
    CHECK(!held(0x05) && !held(0x06));
    return fails ? 1 : 0;
}
'''


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    sdl = (ROOT / "host/posix/display_sdl.c").read_text(encoding="utf-8")
    win = (ROOT / "host/posix/windows.h").read_text(encoding="utf-8")
    vk = "\n".join(m.group(0) for m in re.finditer(r"^#define VK_\w+ 0x[0-9A-Fa-f]+", win, re.M))
    code = DRIVER.replace("/* VK */", vk).replace("/* KEYS_UPDATE */", block(sdl, "static void keys_update(void) {"))
    with tempfile.TemporaryDirectory(prefix="sdl-mouse-buttons-") as tmp:
        src, exe = Path(tmp) / "keys.c", Path(tmp) / "keys.exe"
        src.write_text(code, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: Linux's mouse buttons as virtual keys:\n" + r.stdout + r.stderr)
    print("PASS: Linux publishes SDL's five mouse buttons as virtual keys, Mouse 4 and 5 included, none without a window")


if __name__ == "__main__":
    main()
