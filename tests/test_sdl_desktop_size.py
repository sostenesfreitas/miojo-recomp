"""host/hos_sdl.c hos_sdl_desktop_size, the main screen's size on Linux (MkwVkHost.screen_size: the Insert menu's Aspect
ratio and Output resolution rows ask for it at startup and while the game runs). A windowless start (HOS_HEADLESS) loads no
SDL: the screen is unknown, not a call through an empty slot (#2566: exit 139 before the first frame on Linux). With SDL3,
the desktop mode's size in pixels (its screen units times its pixel density: 1536x864 at 1.25 is 1920x1080); with SDL2, the
mode's size as it gives it. The function is cut from hos_sdl.c and run against a table of stand-ins. No game, no SDL.

    python -I -B tests/test_sdl_desktop_size.py [--cc gcc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)

static int version;
static struct { void *GetPrimaryDisplay, *GetDesktopDisplayMode; } f;   /* (empty until SDL is loaded, as in hos_sdl.c) */
#define CALL(type, fn) ((type)f.fn)

static uint8_t sdl3_mode[32];
static uint32_t sdl3_primary(void) { return 1; }
static const uint8_t *sdl3_desktop(uint32_t id) { return id == 1 ? sdl3_mode : NULL; }
static const uint8_t *sdl3_none(uint32_t id) { (void)id; return NULL; }   /* (no video: SDL3 gives no mode) */
struct sdl2_mode { uint32_t format; int w, h, refresh_rate; void *driverdata; };
static int sdl2_desktop(int index, void *out) {
    struct sdl2_mode *m = out;
    if (index != 0) return -1;
    m->w = 2560, m->h = 1080;
    return 0;
}
static int sdl2_fail(int index, void *out) { (void)index, (void)out; return -1; }

FUNCTION

int main(void) {
    int w = -1, h = -1;
    /* no SDL loaded (a windowless start): unknown, the outputs untouched */
    CHECK(hos_sdl_desktop_size(&w, &h) == 0 && w == -1 && h == -1);
    /* SDL3: 1536x864 screen units at a pixel density of 1.25, the 1920x1080 of a Wayland desktop at 125 % */
    version = 3, f.GetPrimaryDisplay = (void *)sdl3_primary, f.GetDesktopDisplayMode = (void *)sdl3_desktop;
    const int sw = 1536, sh = 864;
    const float density = 1.25f;
    memcpy(sdl3_mode + 8, &sw, 4), memcpy(sdl3_mode + 12, &sh, 4), memcpy(sdl3_mode + 16, &density, 4);
    CHECK(hos_sdl_desktop_size(&w, &h) == 1 && w == 1920 && h == 1080);
    f.GetDesktopDisplayMode = (void *)sdl3_none;
    w = h = -1;
    CHECK(hos_sdl_desktop_size(&w, &h) == 0 && w == -1);
    /* SDL2: the mode as it is */
    version = 2, f.GetDesktopDisplayMode = (void *)sdl2_desktop;
    CHECK(hos_sdl_desktop_size(&w, &h) == 1 && w == 2560 && h == 1080);
    f.GetDesktopDisplayMode = (void *)sdl2_fail;
    w = h = -1;
    CHECK(hos_sdl_desktop_size(&w, &h) == 0 && w == -1);
    puts("PASS: the screen's size from SDL3 or SDL2, unknown with no SDL loaded");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    src = (ROOT / "host" / "hos_sdl.c").read_text(encoding="utf-8")
    m = re.search(r'^int hos_sdl_desktop_size\(int \*width, int \*height\) \{\n.*?^\}\n', src, re.S | re.M)
    if not m:
        raise SystemExit("FAIL: host/hos_sdl.c has no hos_sdl_desktop_size")
    with tempfile.TemporaryDirectory(prefix="sdl-desktop-size-") as tmp:
        c, exe = Path(tmp) / "desktop.c", Path(tmp) / "desktop"
        c.write_text(FIXTURE.replace("FUNCTION", m.group(0)), encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-pedantic", str(c), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
