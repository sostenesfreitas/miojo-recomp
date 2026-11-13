"""Ultrawide's loading cover (issue 2220), game-free: host/hos_main.c's hos_wide_cover and nx2/oot_wide.py's HUD site,
compiled around an authored layout.

With the menus in the middle 16:9 the game draws the loading cover between two areas there too, so the renderer closes
the sides by as much as the cover is closed. That is read from the cover screen's layout, which the HUD site notes as
the screen's projection is set up. Checked here: the cover's opacity is the product down its panes; a 16:9 or narrower
picture, the cover kept over the whole picture, no screen yet and every layout that is not the one expected (another
vtable, another pane size or tree, a null pointer or an address outside the guest's window, an opacity that is no
opacity) give 0, the
sides as they were; the site notes the loading cover's screen and no other, and squeezes it as before.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_wide  # noqa: E402


def cut(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a) + len(end)]


FIXTURE = r"""
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define BASE 0x1080000000ULL
#define SIZE 0x10000ULL
static uint8_t mem[SIZE];
/* (the game's: any address in the guest's window is given, and the first MB of it is never mapped, so a read there is a crash) */
static void *hos_guest_range(uint64_t a, uint64_t n) {
    if (a < 0x100000) { fprintf(stderr, "FAIL: a read at %#llx, in the first MB\n", (unsigned long long)a); exit(1); }
    return a >= BASE && n <= SIZE && a - BASE <= SIZE - n ? mem + (a - BASE) : NULL;
}
typedef struct { uint64_t x[32]; float vreg[32][4]; } Ctx;
static uint64_t recomp_load64(Ctx *c, uint64_t a) { uint64_t v; (void)c; memcpy(&v, hos_guest_range(a, 8), 8); return v; }
volatile float hos_wide_squeeze;
volatile uint32_t hos_wide_keep_mask = 0xf;
HOST_C
static void site(Ctx *c) {
SITE_C
}
enum { SCREEN = 0x4000, HOLDER = 0x3000, LAYOUT = 0x2000, PANES = 0x1000, TREE = 0x800 };
static void put64(uint64_t at, uint64_t v) { memcpy(mem + at, &v, 8); }
static void putf(uint64_t at, float v) { memcpy(mem + at, &v, 4); }
static void put16(uint64_t at, uint16_t v) { memcpy(mem + at, &v, 2); }
/* the layout as the game has it: three panes down to the black picture, then the icon's */
static void layout(float root, float group, float picture) {
    memset(mem, 0, sizeof mem);
    put64(SCREEN, COVER_VTABLE), put64(SCREEN + 0x28, BASE + HOLDER), put64(HOLDER + 0x80, BASE + LAYOUT);
    put64(LAYOUT + 0x100, BASE + PANES), put64(LAYOUT + 0x110, BASE + TREE);
    const float opacity[4] = {root, group, picture, 0.25f};
    const uint16_t parent[4] = {0xffff, 0, 1, 0};
    for (int k = 0; k < 4; k++) {
        put64(PANES + k * 0xb0 + 0x18, BASE + LAYOUT), putf(PANES + k * 0xb0 + 0x74, opacity[k]);
        put16(TREE + k * 8, k == 2 ? 3 : 0xc), put16(TREE + k * 8 + 2, parent[k]);
    }
    putf(PANES + 2 * 0xb0 + 0x58, 3840.0f), putf(PANES + 2 * 0xb0 + 0x5c, 2160.0f);
    putf(PANES + 3 * 0xb0 + 0x58, 200.0f), putf(PANES + 3 * 0xb0 + 0x5c, 200.0f);
    hos_wide_cover_screen = BASE + SCREEN, hos_wide_squeeze = 0.744186f, hos_wide_keep_mask = 0x5;
}
static int near(float a, float b) { return fabsf(a - b) < 1e-6f; }
/* the site as the UI's projection row is set for a screen: x0 the canvas, 0x1b8 into the screen */
static float squeezed(uint64_t vtable, uint64_t screen) {
    Ctx c;
    memset(&c, 0, sizeof c);
    put64(screen, vtable);
    c.x[0] = BASE + screen + 0x1b8;
    c.vreg[1][0] = UI_M00, c.vreg[4][1] = UI_M11;
    site(&c);
    return c.vreg[1][0] / UI_M00;
}
int main(void) {
    layout(1.0f, 1.0f, 1.0f);
    CHECK(hos_wide_cover() == 1.0f);
    layout(1.0f, 0.5f, 1.0f);
    CHECK(hos_wide_cover() == 0.5f);
    layout(0.5f, 0.5f, 0.8f);
    CHECK(near(hos_wide_cover(), 0.2f));
    layout(1.0f, 0.0f, 1.0f);   /* open: the group's opacity ends at 0 */
    CHECK(hos_wide_cover() == 0.0f);

    /* no sides to close: a 16:9 picture (0), a narrower one (1); the cover kept over the whole picture; no screen yet */
    layout(1.0f, 1.0f, 1.0f), hos_wide_squeeze = 0.0f;
    CHECK(hos_wide_cover() == 0.0f);
    layout(1.0f, 1.0f, 1.0f), hos_wide_squeeze = 1.0f;
    CHECK(hos_wide_cover() == 0.0f);
    layout(1.0f, 1.0f, 1.0f), hos_wide_keep_mask = 0xf;
    CHECK(hos_wide_cover() == 0.0f);
    layout(1.0f, 1.0f, 1.0f), hos_wide_cover_screen = 0;
    CHECK(hos_wide_cover() == 0.0f);

    /* not the layout expected: 0 */
    layout(1.0f, 1.0f, 1.0f), put64(SCREEN, COVER_VTABLE + 0x398);
    CHECK(hos_wide_cover() == 0.0f);
    layout(1.0f, 1.0f, 1.0f), putf(PANES + 2 * 0xb0 + 0x58, 200.0f);
    CHECK(hos_wide_cover() == 0.0f);
    layout(1.0f, 1.0f, 1.0f), put16(TREE + 2 * 8 + 2, 0);
    CHECK(hos_wide_cover() == 0.0f);
    layout(1.0f, 1.0f, 1.0f), put16(TREE + 1 * 8 + 2, 3);
    CHECK(hos_wide_cover() == 0.0f);
    layout(1.0f, 1.0f, 1.0f), put64(PANES + 2 * 0xb0 + 0x18, BASE + HOLDER);
    CHECK(hos_wide_cover() == 0.0f);
    /* (a null pointer anywhere down the chain, one into the first MB, and ones past the window's ends) */
    const uint64_t outside[] = {0, 0x10, 0xfff80, BASE - 8, BASE + SIZE - 4, BASE + SIZE, ~0ULL - 3};
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        layout(1.0f, 1.0f, 1.0f), hos_wide_cover_screen = outside[i];
        CHECK(hos_wide_cover() == 0.0f);
        layout(1.0f, 1.0f, 1.0f), put64(SCREEN + 0x28, outside[i]);
        CHECK(hos_wide_cover() == 0.0f);
        layout(1.0f, 1.0f, 1.0f), put64(HOLDER + 0x80, outside[i]);
        CHECK(hos_wide_cover() == 0.0f);
        layout(1.0f, 1.0f, 1.0f), put64(LAYOUT + 0x100, outside[i]);
        CHECK(hos_wide_cover() == 0.0f);
        layout(1.0f, 1.0f, 1.0f), put64(LAYOUT + 0x110, outside[i]);
        CHECK(hos_wide_cover() == 0.0f);
    }
    const float bad[] = {-0.25f, 1.5f, NAN, INFINITY};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        for (int k = 0; k < 3; k++) {
            layout(1.0f, 1.0f, 1.0f), putf(PANES + k * 0xb0 + 0x74, bad[i]);
            CHECK(hos_wide_cover() == 0.0f);
        }

    /* the site: the loading cover's screen is noted, in the middle or over the whole picture; no other screen is */
    const uint64_t keep[] = {KEEP_VTABLES};
    layout(1.0f, 1.0f, 1.0f), hos_wide_cover_screen = 0;
    CHECK(near(squeezed(COVER_VTABLE, 0x6000), 0.744186f) && hos_wide_cover_screen == BASE + 0x6000);
    hos_wide_cover_screen = 0, hos_wide_keep_mask = 0xf;
    CHECK(squeezed(COVER_VTABLE, 0x6000) == 1.0f && hos_wide_cover_screen == BASE + 0x6000);
    hos_wide_cover_screen = 0;
    for (int k = 0; k < 3; k++) CHECK(squeezed(keep[k], 0x6000) == 1.0f && hos_wide_cover_screen == 0);
    hos_wide_keep_mask = 0x5;
    CHECK(near(squeezed(keep[1], 0x6000), 0.744186f) && squeezed(keep[2], 0x6000) == 1.0f && hos_wide_cover_screen == 0);
    CHECK(near(squeezed(COVER_VTABLE - 0x398, 0x6000), 0.744186f) && hos_wide_cover_screen == 0);   /* the HUD's */
    hos_wide_squeeze = 0.0f;   /* 16:9: the game's own */
    CHECK(squeezed(COVER_VTABLE, 0x6000) == 1.0f && hos_wide_cover_screen == 0);
    printf("PASS: the loading cover's opacity is read down its panes, 0 for anything else; the site notes its screen alone\n");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    host = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    host_c = cut(host, "volatile uint64_t hos_wide_cover_screen;", "    return level;\n}\n")
    if oot_wide.KEEP_VTABLES[3] != oot_wide.MAIN + 0x4be7b30 or "0x80020000ULL + 0x4be7b30" not in host_c:
        raise SystemExit("FAIL: the cover's vtable in host/hos_main.c is not nx2/oot_wide.py's KEEP_VTABLES[3]")
    src = (FIXTURE.replace("HOST_C", host_c).replace("SITE_C", oot_wide.HUD)
           .replace("COVER_VTABLE", "0x%xULL" % oot_wide.KEEP_VTABLES[3])
           .replace("KEEP_VTABLES", ", ".join("0x%xULL" % v for v in oot_wide.KEEP_VTABLES))
           .replace("UI_M00", oot_wide.UI_M00).replace("UI_M11", oot_wide.UI_M11))
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "cover.c"
        exe = Path(tmp) / ("cover.exe" if os.name == "nt" else "cover")
        c.write_text(src, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-O1", "-Wall", "-Wno-unused-function", str(c), "-o", str(exe), "-lm"], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
