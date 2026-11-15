"""The HUD's pane planner (ozwaldorf's Extended ultrawide menus,
https://github.com/NotRexed/Hookshot/pull/2843) moves a HUD layout's side panes out
to a wider screen's edges, and only while the picture is not 16:9.

nx2/oot_wide.py calls host/hos_wide_ui.c from f_8154cab0, where the game makes a pane's final transform: the parent it is
made from, then the transform stored (the translation at +0x4c). A pane at one side of the 3840-wide canvas whose ancestors
were not moved has its x moved out by the screen's extra half width; its children keep the move they were made with; a
pane spanning the canvas is drawn the screen's width instead; the HUD's gauges move by name, a pointer placed from the world
stays, the minimap's icons move with it and the map page keeps its own. A menu's layout is not moved. A change of the shape
marks every pane to remake its transform.

python -I -B tests/test_wide_ui.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_wide  # noqa: E402

FIXTURE_HEAD = r'''#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
#define BASE 0x10000000ull
static unsigned char mem[0x40000];
volatile float hos_wide_squeeze, hos_wide_ratio;
static uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; if (a < BASE || a >= BASE + sizeof mem) return 0; return mem[a - BASE]; }
static uint64_t recomp_load32(Ctx *c, uint64_t a) { uint32_t v; (void)c; if (a < BASE || a + 4 > BASE + sizeof mem) return 0; memcpy(&v, mem + (a - BASE), 4); return v; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) { uint64_t v; (void)c; if (a < BASE || a + 8 > BASE + sizeof mem) return 0; memcpy(&v, mem + (a - BASE), 8); return v; }
static void recomp_store32(Ctx *c, uint64_t a, uint32_t v) { (void)c; memcpy(mem + (a - BASE), &v, 4); }
static void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c; memcpy(mem + (a - BASE), &v, 8); }
typedef void (*A64Fn)(Ctx *);
A64Fn a64_lookup(uint64_t addr);
static void hos_log(const char *f, ...) { (void)f; }
static int64_t hos_now_ns(void) { return 0; }
'''

FIXTURE_MAIN = r'''
static void putf(uint64_t a, float v) { memcpy(mem + (a - BASE), &v, 4); }
static void put32(uint64_t a, uint32_t v) { memcpy(mem + (a - BASE), &v, 4); }
static void put64(uint64_t a, uint64_t v) { memcpy(mem + (a - BASE), &v, 8); }
static float getf(uint64_t a) { float v; memcpy(&v, mem + (a - BASE), 4); return v; }
static int near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }
static Ctx c;
volatile uint32_t hos_wide_keep_mask;
A64Fn a64_lookup(uint64_t addr) { (void)addr; return NULL; }
#define LAYOUT (BASE + 0x100)
#define WORLD (BASE + 0x300)
#define STATUS (BASE + 0x500)
#define PANES (BASE + 0x1000)
#define NAMES (BASE + 0x8000)
#define STRINGS (BASE + 0x9000)
static uint64_t text_at = STRINGS;
static uint64_t text(const char *s) { const uint64_t a = text_at; memcpy(mem + (a - BASE), s, strlen(s) + 1); text_at += 64; return a; }
/* a layout object and its UIL header: its name, the pane names (one table for every test layout) */
static void layout_named(uint64_t l, const char *name) {
    const uint64_t res = l + 0x180;
    memcpy(mem + (l + 0x100 - BASE), &(uint64_t){PANES}, 8), memcpy(mem + (l + 0x13c - BASE), &(uint32_t){64}, 4);
    memcpy(mem + (l + 0xf8 - BASE), &res, 8);
    memcpy(mem + (res + 0x18 - BASE), &(uint64_t){text(name)}, 8), memcpy(mem + (res + 0x20 - BASE), &(uint64_t){NAMES}, 8);
}
/* pane i of the layout: x scale 1, its x and width; origin across 0 left, 1 centre; kind 0 none, 1 picture, 6 part */
static uint64_t pane_in(uint64_t layout, int i, const char *name, float x, float w, unsigned origin, unsigned type) {
    const uint64_t p = PANES + (uint64_t)i * 0xb0;
    put64(p + 0x18, layout), put64(NAMES + (uint64_t)i * 8, text(name));
    putf(p + 0x28, 1.0f), putf(p + 0x4c, x), putf(p + 0x58, w);
    put32(p + 0xa8, ((origin + 3) << 11) | (type << 22) | 1);
    return p;
}
static uint64_t pane(int i, float x, float w, unsigned origin, unsigned type) { return pane_in(LAYOUT, i, "P_Test_00", x, w, origin, type); }
/* the game making p's transform from parent (0: none); x as the game stores it (its parent's move in it) */
static float place(uint64_t p, uint64_t parent, float x) {
    putf(p + 0x4c, x);
    hos_wide_ui_parent(parent);
    hos_wide_ui_placed(&c, p);
    return getf(p + 0x4c);
}
static float drawn_width(uint64_t p) {
    const float w = getf(p + 0x58);
    memcpy(c.vreg[0], &w, 4);
    hos_wide_ui_size(&c, p);
    float out;
    memcpy(&out, c.vreg[0], 4);
    return out;
}
int main(void) {
    /* (Talk_00, one of the HUD's layouts, for the planner's general cases; Test_Menu_00 a menu's, never moved) */
    layout_named(LAYOUT, "Talk_00"), layout_named(WORLD, "Pointer_00"), layout_named(STATUS, "PlayerStatus_00");
    hos_wide_squeeze = (16.0f / 9.0f) / (3440.0f / 1440.0f);   /* (3440x1440) */
    const float extra = 1920.0f * (1.0f / hos_wide_squeeze - 1.0f);
    const uint64_t root = pane(0, 0, 0, 1, 0), all = pane(1, 0, 200, 1, 0), left = pane(2, -1920, 60, 1, 0),
                   heart = pane(3, -1140, 1300, 1, 6), right = pane(4, 1626, 240, 1, 1), back = pane(5, 0, 3840, 1, 1),
                   page = pane(6, 3840, 0, 1, 0), cell = pane(7, 3840, 200, 1, 1), grid = pane(10, 0, 0, 1, 0),
                   col0 = pane(11, -1750, 200, 1, 1), col1 = pane(12, -900, 200, 1, 1), preview = pane(13, 700, 900, 1, 1),
                   edge = pane(14, -1920, 400, 0, 1);
    /* the HUD by name: the left group stays (the pause menu places part of it), its gauges move; a pointer placed from the
       world stays wherever it is */
    const uint64_t sroot = pane_in(STATUS, 20, "RootPane", 0, 0, 1, 0), scale = pane_in(STATUS, 21, "N_ScaleL_00", -1920, 60, 1, 0),
                   gauge = pane_in(STATUS, 22, "Pa_HeartGauge_00", -1140, 1300, 1, 6),
                   step = pane_in(STATUS, 23, "N_ScaleL_01", 6, 60, 1, 0), mp = pane_in(STATUS, 24, "Pa_MpGauge_00", -1560, 1256, 1, 6),
                   wroot = pane_in(WORLD, 25, "RootPane", 0, 0, 1, 0), arrow = pane_in(WORLD, 26, "P_Arrow_00", -1800, 100, 1, 1);
    /* the game's frame: parents before children; dx the move the plan gave each parent */
    float moved_left = 0, moved_grid0 = 0, moved_grid1 = 0;
#define FRAME() do { \
        place(root, 0, 0); place(all, root, 0); moved_left = place(left, all, -1920) + 1920; \
        place(heart, left, -1140 + moved_left); place(right, all, 1626); place(back, root, 0); \
        place(page, root, 3840); place(cell, page, 3840); place(grid, root, 0); \
        moved_grid0 = place(col0, grid, -1750) + 1750; moved_grid1 = place(col1, grid, -900) + 900; \
        place(preview, grid, 700); place(edge, root, -1920); \
        place(sroot, 0, 0); place(scale, sroot, -1920); place(gauge, scale, -1140); place(step, scale, 6); \
        place(mp, step, -1560); place(wroot, 0, 0); place(arrow, wroot, -1800); \
    } while (0)

    hos_wide_ui_tick();   /* a 16:9 picture: nothing moves */
    FRAME();
    CHECK(moved_left == 0 && getf(right + 0x4c) == 1626 && drawn_width(back) == 3840);

    hos_wide_ratio = 3440.0f / 1440.0f;   /* 3440x1440: noted on the first frame, planned at the present, moved on the next */
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);
    FRAME();
    CHECK(moved_left == 0);
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);   /* (the plan changed: every pane placed again) */
    FRAME();
    CHECK(near(moved_left, -extra));                       /* (the HUD's left side out, at the left edge) */
    CHECK(near(getf(heart + 0x4c), -1140 - extra));        /* (made from it: moved once) */
    CHECK(near(getf(right + 0x4c), 1626 + extra));         /* (far out on the right: out) */
    CHECK(near(drawn_width(back), 3840 / hos_wide_squeeze) && drawn_width(right) == 240);   /* (spans: wider) */
    CHECK(getf(page + 0x4c) == 3840 && getf(cell + 0x4c) == 3840);   /* (off the canvas: left alone) */
    CHECK(near(moved_grid0, -extra) && near(moved_grid1, -extra));   /* (a grid's columns out together) */
    CHECK(getf(preview + 0x4c) == 700);                    /* (its other side stays) */
    CHECK(near(getf(edge + 0x4c), -1920 - extra));         /* (left origin, at the left edge) */
    CHECK(getf(scale + 0x4c) == -1920 && getf(step + 0x4c) == 6);   /* (the HUD's left group: kept) */
    CHECK(near(getf(gauge + 0x4c), -1140 - extra) && near(getf(mp + 0x4c), -1560 - extra));   /* (its gauges: out) */
    CHECK(getf(arrow + 0x4c) == -1800);                    /* (placed from the world: stays) */
    for (int i = 0; i < 4; i++) { hos_wide_ui_tick(); FRAME(); }   /* (stable: the plan no longer changes) */
    CHECK(hos_wide_ui_force == 0 && near(moved_left, -extra) && near(getf(heart + 0x4c), -1140 - extra));

    /* a menu's layout: not moved, even at the canvas's edge, and only its root noted (the plan never walks a menu's panes);
       a HUD part in a menu (the hearts in the pause menu's bar) stays with the menu */
    {
#define MENU (BASE + 0x700)
        layout_named(MENU, "Test_Menu_00");
        const uint64_t mroot = pane_in(MENU, 30, "RootPane", 0, 0, 1, 0), mleft = pane_in(MENU, 31, "P_Column_00", -1920, 400, 0, 1),
                       mheart = pane_in(STATUS, 32, "Pa_HeartGauge_00", -1140, 1300, 1, 6);
        const int noted = nodes_used;
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); place(mroot, 0, 0); place(mleft, mroot, -1920); place(mheart, mroot, -1140); }
        CHECK(getf(mleft + 0x4c) == -1920 && near(moved_left, -extra) && getf(mheart + 0x4c) == -1140);
        CHECK(nodes_used == noted + 1);
    }

    /* the Map page (its icons' group at the canvas's origin): drawn as a menu's, nothing in it moved, its title and its frame
       at the edge included; its icons (placed by code, one by the right edge) and the world map (wider than the screen) as
       they are. The same layout is the HUD's minimap: there its icons are not at the origin and move out with it, and the
       title and the frame are planned */
    {
#define MAPMENU (BASE + 0x4200)
#define MAPICON (BASE + 0x4400)
#define MAPWORLD (BASE + 0x4600)
        layout_named(MAPMENU, "MapMenu_00");
        layout_named(MAPICON, "PaMapIcon_00"), layout_named(MAPWORLD, "PaMapWorld_00");
        const uint64_t mroot2 = pane_in(MAPMENU, 56, "RootPane", 0, 0, 1, 0), title = pane_in(MAPMENU, 57, "T_MapTitle_00", -1702, 1030, 0, 1),
                       icons = pane_in(MAPMENU, 58, "N_MapIcon_00", 0, 60, 1, 0), world = pane_in(MAPMENU, 59, "Pa_WorldMap_00", 0, 9920, 1, 6),
                       iroot = pane_in(MAPICON, 60, "RootPane", 0, 0, 1, 0), icon = pane_in(MAPICON, 61, "P_Icon_00", 1800, 60, 1, 1),
                       wmroot = pane_in(MAPWORLD, 62, "RootPane", 0, 0, 1, 0), wmap = pane_in(MAPWORLD, 63, "P_WorldMap_00", 0, 8448, 1, 1),
                       mframe = pane_in(MAPMENU, 33, "P_Frame_00", -1700, 400, 1, 1);
        float ix = 0;
#define PAGES_FRAME() do { \
            place(mroot2, 0, 0); place(title, mroot2, -1702); place(icons, mroot2, ix); \
            place(iroot, icons, getf(icons + 0x4c)); place(icon, iroot, getf(icons + 0x4c) + 1800 - ix); place(world, mroot2, 0); place(wmroot, world, 0); place(wmap, wmroot, 0); \
            place(mframe, mroot2, -1700); \
        } while (0)
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(getf(title + 0x4c) == -1702 && getf(mframe + 0x4c) == -1700);
        CHECK(getf(icon + 0x4c) == 1800 && getf(wmap + 0x4c) == 0 && drawn_width(wmap) == 8448);
        ix = 1490;   /* the minimap: its icons over it, by the right edge */
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(getf(icons + 0x4c), 1490 + extra));
        CHECK(near(getf(title + 0x4c), -1702 - extra) && near(getf(mframe + 0x4c), -1700 - extra));
    }

    /* a layout let go: its panes are not planned with the ones that take their addresses */
    put32(LAYOUT + 0x13c, 12);
    hos_wide_ui_tick();
    FRAME();
    CHECK(getf(edge + 0x4c) == -1920);
    put32(LAYOUT + 0x13c, 64);

    hos_wide_ratio = 0;   /* back to 16:9: every pane remade as the game makes it */
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);
    FRAME();
    CHECK(moved_left == 0 && getf(right + 0x4c) == 1626 && drawn_width(back) == 3840);

    hos_wide_ratio = 16.0f / 9.0f;   /* a 16:9 screen's own ratio: nothing moves */
    for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); }
    CHECK(moved_left == 0);

    /* the force site: bit 6 of the dirty bits set while forced */
    {
        Ctx *const site = &c;
        site->x[8] = 0x1;
        hos_wide_ui_force = 2;
        { Ctx *c = site; FORCE_SITE }
        CHECK(site->x[8] == 0x41);
        hos_wide_ui_force = 0;
        site->x[8] = 0x1;
        { Ctx *c = site; FORCE_SITE }
        CHECK(site->x[8] == 0x1);
    }
    (void)moved_grid0, (void)moved_grid1;
    printf("PASS: the HUD's planner moves its layouts' side panes out to a wider screen's edges, and back\n");
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    host = (ROOT / "host" / "hos_wide_ui.c").read_text(encoding="utf-8")
    backend = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")

    check("hos_wide_ui_tick();" in backend, "the host does not plan the HUD each present")
    check('getenv("HOS_WIDE_UI")' in host, "HOS_WIDE_UI no longer turns the HUD's moves off")

    words = {off: word for off, word, _ in oot_wide.UI_SITES}
    check(words == {0x152a730: 0xb9400008, 0x152caa0: 0xb9400008, 0x152cd98: 0xad4207e0, 0x152cdd4: 0xb9403008, 0x152ce64: 0xfc04c263,
                    0x152ac40: 0x2d4b0400},
          "the pane transform sites moved")
    check(all(site in oot_wide.SITES for site in oot_wide.UI_SITES), "the pane sites are not installed")
    for name in ("hos_wide_ui_parent(uint64_t parent)", "hos_wide_ui_placed(Ctx *c, uint64_t pane)",
                 "hos_wide_ui_size(Ctx *c, uint64_t pane)"):
        check(("void " + name) in host, "the host's %s has another signature" % name.split("(")[0])

    body = "".join(line for line in host.splitlines(keepends=True) if not line.startswith("#include"))
    for decl in ("extern volatile float hos_wide_ratio;\n",):
        check(decl in body, "the host file no longer declares " + decl.strip())
        body = body.replace(decl, "", 1)
    src = FIXTURE_HEAD + body + FIXTURE_MAIN.replace("FORCE_SITE", oot_wide.UI_FORCE)
    with tempfile.TemporaryDirectory(prefix="wide-ui-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(src, encoding="utf-8")
        cc = subprocess.run([a.cc, "-std=c11", "-O1", "-Wall", "-Wno-unused-function", "-o", str(exe), str(c), "-lm"],
                            capture_output=True, text=True)
        check(cc.returncode == 0, "fixture did not compile:\n" + cc.stdout + cc.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check(run.returncode == 0, "fixture failed:\n" + run.stdout + run.stderr)
        print(run.stdout.strip())


if __name__ == "__main__":
    main()
