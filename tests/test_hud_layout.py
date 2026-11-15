"""The HUD is laid out for the picture's shape, never stretched (host/hos_wide_ui.c hos_wide_ui_tick, nx2/oot_wide.py HUD).

At its 16:9 size (scaled with the picture's height, as at 16:9) its side panes move out to a wider screen's sides and in to
a narrower one's; past 12:5 (a 32:9 screen) they stop at 12:5's sides, and at 48:9 (three screens) they stay on the middle
one. Narrower than 3:2 (4:3, 5:4) the HUD is drawn smaller, by the ratio over 3:2, and what sits at the top or bottom moves
out to the screen's top or bottom. The HUD's layouts are known by their root layout's name; every other layout is a menu's,
kept at its 16:9 shape and not moved: in the middle 16:9 of a wider screen, and on a narrower one drawn smaller from its
root pane to fit the width. The map page (the minimap's layout showing it) is drawn as a menu's; the text box's choices
are held where the game puts them at the HUD's size, and its button guide is the HUD's. At 16:9 nothing changes. The UI
projection's site scales every screen not kept whole.

python -I -B tests/test_hud_layout.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "tests"))
import oot_wide  # noqa: E402
from test_wide_ui import FIXTURE_HEAD  # noqa: E402

FIXTURE_MAIN = r'''
static void putf(uint64_t a, float v) { memcpy(mem + (a - BASE), &v, 4); }
static void put32(uint64_t a, uint32_t v) { memcpy(mem + (a - BASE), &v, 4); }
static void put64(uint64_t a, uint64_t v) { memcpy(mem + (a - BASE), &v, 8); }
static float getf(uint64_t a) { float v; memcpy(&v, mem + (a - BASE), 4); return v; }
static int near(float a, float b) { return a > b - 0.05f && a < b + 0.05f; }
A64Fn a64_lookup(uint64_t addr) { (void)addr; return NULL; }
volatile uint32_t hos_wide_keep_mask;   /* (0: the centred Ultrawide menus modes; 0xf Full width, every bit kept) */
static Ctx c;
#define STATUS (BASE + 0x100)
#define PLUS (BASE + 0x300)
#define FADE (BASE + 0x500)
#define MAP (BASE + 0x700)
#define DIALOG (BASE + 0x900)
#define GUIDE (BASE + 0xb00)
#define PANES (BASE + 0x1000)
#define NAMES (BASE + 0x8000)
#define STRINGS (BASE + 0x9000)
static uint64_t text_at = STRINGS;
static uint64_t text(const char *s) { const uint64_t a = text_at; memcpy(mem + (a - BASE), s, strlen(s) + 1); text_at += 64; return a; }
static void layout_named(uint64_t l, const char *name) {
    const uint64_t res = l + 0x180;
    put64(l + 0x100, PANES), put32(l + 0x13c, 64), put64(l + 0xf8, res);
    put64(res + 0x18, text(name)), put64(res + 0x20, NAMES);
}
/* pane i: centred both ways (origin 4), its place and size; kind 0 none, 1 picture */
static uint64_t pane_at(uint64_t layout, int i, const char *name, float w, float h, unsigned type) {
    const uint64_t p = PANES + (uint64_t)i * 0xb0;
    put64(p + 0x18, layout), put64(NAMES + (uint64_t)i * 8, text(name));
    putf(p + 0x28, 1.0f), putf(p + 0x38, 1.0f), putf(p + 0x58, w), putf(p + 0x5c, h);
    put32(p + 0xa8, (4u << 11) | (type << 22) | 1);
    return p;
}
static float px, py;   /* (where the transform was stored, as moved) */
static void place(uint64_t p, uint64_t parent, float x, float y) {
    putf(p + 0x4c, x), putf(p + 0x50, y);
    hos_wide_ui_parent(parent);
    hos_wide_ui_placed(&c, p);
    px = getf(p + 0x4c), py = getf(p + 0x50);
}
static uint64_t sroot, hearts, rupees, buttons, minimap, banner, mroot, mleft, froot, fpane, ficon;
static uint64_t maproot, mapicons, mapframe, droot, dchoice, lroot, lguide;
static float hx, hy, rx_, ry, bx, mx, my, nx, menux, mrx, mry, mrs, ix, iy;
static float map_ix, maps, mapfx, ds, dcx, dcy, lgx, lgy;   /* (map_ix: the map's icons' group, at the origin on the map page) */
/* the pane's size as drawn (f_8154ac30: its width in v0, its height in v1) */
static void drawn_size(uint64_t p, float *w, float *h) {
    *w = getf(p + 0x58), *h = getf(p + 0x5c);
    memcpy(c.vreg[0], w, 4), memcpy(c.vreg[1], h, 4);
    hos_wide_ui_size(&c, p);
    memcpy(w, c.vreg[0], 4), memcpy(h, c.vreg[1], 4);
}
static void frame(void) {
    place(sroot, 0, 0, 0);
    place(hearts, sroot, -1500, 880); hx = px, hy = py;     /* (top left) */
    place(rupees, sroot, -1600, -900); rx_ = px, ry = py;   /* (bottom left) */
    place(buttons, sroot, 1600, 820); bx = px;              /* (top right) */
    place(minimap, sroot, 1500, -760); mx = px, my = py;    /* (bottom right) */
    place(banner, sroot, 0, 0); nx = px;                    /* (across the canvas: the place's name's band) */
    putf(mroot + 0x28, 1.0f), putf(mroot + 0x38, 1.0f);    /* (a menu's root, as the game makes its transform) */
    place(mroot, 0, 100, 40); mrx = px, mry = py, mrs = getf(mroot + 0x28);
    CHECK(getf(mroot + 0x38) == mrs);                       /* (the same both ways) */
    place(mleft, mroot, -1700, 0); menux = px;              /* (a menu's left column) */
    place(froot, 0, 0, 0); place(fpane, froot, 0, 0); place(ficon, froot, 1650, -880); ix = px, iy = py;   /* (the loading fade: its black pane, its icon) */
    putf(maproot + 0x28, 1.0f), putf(maproot + 0x38, 1.0f);   /* (MapMenu_00: the map page, or the HUD's minimap) */
    place(maproot, 0, 0, 0); maps = getf(maproot + 0x28);
    place(mapicons, maproot, map_ix, 0); place(mapframe, maproot, -1700, 0); mapfx = px;
    putf(droot + 0x28, 1.0f), putf(droot + 0x38, 1.0f);   /* (the text box's choices) */
    place(droot, 0, 0, 0); ds = getf(droot + 0x28);
    place(dchoice, droot, 1500, -500); dcx = px, dcy = py;
    place(lroot, 0, 0, 0); place(lguide, lroot, 1600, -900); lgx = px, lgy = py;   /* (the text box's button guide) */
}
static void at(float ratio) {
    hos_wide_ratio = ratio;
    hos_wide_squeeze = ratio > 0.0f ? (ratio > 16.0f / 9.0f ? (16.0f / 9.0f) / ratio : 1.0f) : 0.0f;
    for (int i = 0; i < 6; i++) { hos_wide_ui_tick(); frame(); }
}
int main(void) {
    layout_named(STATUS, "PlayerStatus_00"), layout_named(PLUS, "Test_Menu_00"), layout_named(FADE, "LoadingFade_00");
    sroot = pane_at(STATUS, 0, "RootPane", 0, 0, 0);
    hearts = pane_at(STATUS, 1, "P_Hearts_00", 500, 120, 1);
    rupees = pane_at(STATUS, 2, "P_Rupee_00", 300, 120, 1);
    buttons = pane_at(STATUS, 3, "P_Buttons_00", 400, 400, 1);
    minimap = pane_at(STATUS, 4, "P_Map_00", 600, 500, 1);
    banner = pane_at(STATUS, 5, "P_Band_00", 3840, 300, 1);
    mroot = pane_at(PLUS, 6, "RootPane", 0, 0, 0);
    mleft = pane_at(PLUS, 7, "P_Column_00", 400, 1500, 1);
    froot = pane_at(FADE, 8, "RootPane", 0, 0, 0);
    fpane = pane_at(FADE, 9, "P_Fade_00", 3840, 2160, 1);
    ficon = pane_at(FADE, 10, "P_Icon_00", 160, 160, 1);
    layout_named(MAP, "MapMenu_00"), layout_named(DIALOG, "MessageDialog_00"), layout_named(GUIDE, "MessageLog_01");
    maproot = pane_at(MAP, 11, "RootPane", 0, 0, 0);
    mapicons = pane_at(MAP, 12, "N_MapIcon_00", 60, 60, 0);
    mapframe = pane_at(MAP, 13, "P_Frame_00", 400, 1500, 1);
    droot = pane_at(DIALOG, 14, "RootPane", 0, 0, 0);
    dchoice = pane_at(DIALOG, 15, "N_BtnIn_00", 300, 100, 1);
    lroot = pane_at(GUIDE, 16, "RootPane", 0, 0, 0);
    lguide = pane_at(GUIDE, 17, "A_GuideAll_00", 300, 80, 1);
    const float G = 16.0f / 9.0f;
    float fw, fh, iw, ih;

    at(0.0f);   /* 16:9: the game's own */
    CHECK(hx == -1500 && hy == 880 && bx == 1600 && mx == 1500 && menux == -1700);
    CHECK(hos_ui_qx == 0.0f && hos_ui_qy == 0.0f && mrs == 1.0f && mrx == 100 && mry == 40);

    at(2560.0f / 1080.0f);   /* 21:9: the HUD's sides out to the screen's, at the 16:9 size; the menus stay in the middle */
    float r = 2560.0f / 1080.0f, e = 1920.0f * (r / G - 1.0f);
    CHECK(near(hos_ui_qx, G / r) && hos_ui_qy == 1.0f && mrs == 1.0f && mrx == 100);
    CHECK(near(hx, -1500 - e) && near(rx_, -1600 - e) && near(bx, 1600 + e) && near(mx, 1500 + e));
    CHECK(hy == 880 && ry == -900 && my == -760 && nx == 0);   /* (up and down as at 16:9) */
    CHECK(menux == -1700);
    /* (the map page: a menu's, not moved; the text box's choices held where the game puts them; its button guide out with
       the minimap) */
    CHECK(mapfx == -1700 && maps == 1.0f && dcx == 1500 && ds == 1.0f && near(lgx, 1600 + e) && lgy == -900);
    /* (the loading fade, its layout in the middle 16:9 as the centred Ultrawide menus modes draw it: its pane over the whole
       picture, its icon as it is, Hookshot #82) */
    drawn_size(fpane, &fw, &fh), drawn_size(ficon, &iw, &ih);
    CHECK(near(fw * hos_ui_qx, 3840.0f) && fh == 2160.0f && iw == 160.0f && ih == 160.0f);
    /* (its icon left in the middle 16:9: beside it, the centred modes' sides cover the picture while the world is not drawn) */
    CHECK(ix == 1650 && iy == -880);
    hos_wide_keep_mask = 0xf;   /* (Full width: the fade's screen kept whole, over the whole picture already: its icon stays) */
    at(2560.0f / 1080.0f);
    CHECK(ix == 1650 && iy == -880 && near(hx, -1500 - e));
    /* (and its pane at the game's own size: the projection stretches it, not the plan too) */
    drawn_size(fpane, &fw, &fh);
    CHECK(fw == 3840.0f && fh == 2160.0f);
    hos_wide_keep_mask = 0;
    at(2560.0f / 1080.0f);
    CHECK(ix == 1650);

    at(3440.0f / 1440.0f);   /* 3440x1440: still to the screen's sides */
    r = 3440.0f / 1440.0f, e = 1920.0f * (r / G - 1.0f);
    CHECK(near(hx, -1500 - e) && near(mx, 1500 + e));

    at(32.0f / 9.0f);   /* 32:9: the sides of a 12:5 screen, in the middle; drawn at the 16:9 size */
    e = 1920.0f * ((12.0f / 5.0f) / G - 1.0f);
    CHECK(near(hos_ui_qx, G / (32.0f / 9.0f)) && hos_ui_qy == 1.0f);
    CHECK(near(hx, -1500 - e) && near(bx, 1600 + e) && menux == -1700);

    at(48.0f / 9.0f);   /* three 16:9 screens: on the middle one, as before */
    CHECK(hx == -1500 && bx == 1600 && mx == 1500);

    at(1.6f);   /* 16:10: the 16:9 size, the sides in to the screen's; the menus smaller, to fit its width */
    r = 1.6f, e = 1920.0f * (r / G - 1.0f);
    CHECK(near(hos_ui_qx, G / r) && hos_ui_qy == 1.0f);
    CHECK(near(hx, -1500 - e) && near(mx, 1500 + e) && e < 0.0f && hy == 880 && my == -760);
    CHECK(near(mrs, r / G) && near(mrx, 100 * r / G) && near(mry, 40 * r / G));
    /* (drawn: the menu's whole width in the screen's) */
    CHECK(near(3840.0f * mrs * hos_ui_qx, 3840.0f));

    at(1.5f);   /* 3:2: still the 16:9 size */
    CHECK(near(hos_ui_qy, 1.0f));

    at(4.0f / 3.0f);   /* 4:3: smaller by the ratio over 3:2; the sides in, the top and bottom out to the screen's */
    r = 4.0f / 3.0f;
    const float k = r / 1.5f, ex = 1920.0f * (r / (k * G) - 1.0f), ey = 1080.0f * (1.0f / k - 1.0f);
    CHECK(near(hos_ui_qy, k) && near(hos_ui_qx, k * G / r) && near(mrs, r / G / k));
    CHECK(near(3840.0f * mrs * hos_ui_qx, 3840.0f) && near(2160.0f * mrs * hos_ui_qy, 2160.0f * r / G));   /* (the menu: the width) */
    CHECK(near(hx, -1500 - ex) && near(hy, 880 + ey) && near(rx_, -1600 - ex) && near(ry, -900 - ey));
    CHECK(near(bx, 1600 + ex) && near(mx, 1500 + ex) && near(my, -760 - ey));
    CHECK(nx == 0);
    /* (the map page fitted to the width with the rest of the pause menu, never moved; the text box's choices at the HUD's
       size, as the box, where the game puts them; its button guide in the corner, as the minimap) */
    CHECK(near(maps, r / G / k) && mapfx == -1700 && ds == 1.0f && dcx == 1500 && dcy == -500);
    CHECK(near(lgx, 1600 + ex) && near(lgy, -900 - ey));
    /* drawn: the hearts' place on the screen, in canvas units of the 16:9 size, at the same distance from the top left corner
       as at 16:9 (the screen's half sizes: 1920 * r / (k * G) across, 1080 / k up) */
    CHECK(near(-1920.0f * r / (k * G) - hx, -1920.0f + 1500) && near(1080.0f / k - hy, 1080.0f - 880));
    /* (the loading fade at the HUD's size: its pane taller, over the whole picture's height; its width already past it) */
    drawn_size(fpane, &fw, &fh), drawn_size(ficon, &iw, &ih);
    CHECK(fw == 3840.0f && near(fh * hos_ui_qy, 2160.0f) && 3840.0f * hos_ui_qx >= 3840.0f && iw == 160.0f && ih == 160.0f);
    /* (its icon in the screen's corner as the HUD's, all of it on the screen: the canvas at the HUD's size is wider than the
       screen, 1920 / hos_ui_qx of its half width showing) */
    CHECK(near(ix, 1650 + ex) && near(iy, -880 - ey) && (ix + 80.0f) * hos_ui_qx <= 1920.0f && (-iy + 80.0f) * hos_ui_qy <= 1080.0f);

    {   /* the loading icon whole on the screen at every shape the game offers, 5:4 to 48:9, and an odd window */
        const float shapes[] = {1280.0f / 1024.0f, 4.0f / 3.0f, 1.5f, 1.6f, 1366.0f / 1000.0f, 2560.0f / 1080.0f,
                                3440.0f / 1440.0f, 2.4f, 32.0f / 9.0f, 48.0f / 9.0f};
        for (int s = 0; s < (int)(sizeof shapes / sizeof shapes[0]); s++) {
            at(shapes[s]);
            CHECK((ix + 80.0f) * hos_ui_qx <= 1920.5f && (-iy + 80.0f) * hos_ui_qy <= 1080.5f);
            if (shapes[s] > G) CHECK(ix == 1650 && ix + 80.0f <= 1920.0f);   /* (wider: in the middle 16:9, never beside it) */
        }
        at(4.0f / 3.0f);
    }
    map_ix = 1490.0f;   /* (the same layout as the HUD's minimap: planned, at the HUD's size) */
    at(4.0f / 3.0f);
    CHECK(maps == 1.0f && near(mapfx, -1700 - ex));
    map_ix = 0.0f;
    at(4.0f / 3.0f);   /* (the map page again) */
    CHECK(near(maps, r / G / k) && mapfx == -1700);

    at(0.0f);   /* back to 16:9: the game's own */
    CHECK(hx == -1500 && hy == 880 && mx == 1500 && menux == -1700 && mrs == 1.0f && mrx == 100);
    drawn_size(fpane, &fw, &fh);
    CHECK(fw == 3840.0f && fh == 2160.0f);
    printf("PASS: the HUD keeps its 16:9 size at each shape, at the screen's edges; the menus keep 16:9\n");
    return 0;
}
'''

SITE_MAIN = r'''
volatile float hos_ui_qx, hos_ui_qy;
volatile uint64_t hos_wide_cover_screen;   /* (Hookshot's loading cover: the site notes its screen) */
volatile uint32_t hos_wide_keep_mask;
static void run(uint64_t vt, float *row0, float *row1) {
    Ctx k;
    memset(&k, 0, sizeof k);
    const float m00 = 2.0f / 3840.0f, m11 = 2.0f / 2160.0f, r0[4] = {m00, 0, 0, 0}, r1[4] = {0, m11, 0, 0};
    memcpy(k.vreg[1], r0, 16), memcpy(k.vreg[4], r1, 16);
    k.x[0] = BASE + 0x1b8;
    memcpy(mem, &vt, 8);
    { Ctx *c = &k; SITE }
    memcpy(row0, k.vreg[1], 16), memcpy(row1, k.vreg[4], 16);
}
int main(void) {
    float a[4], b[4];
    const float m00 = 2.0f / 3840.0f, m11 = 2.0f / 2160.0f;
    hos_wide_squeeze = 0.0f, hos_ui_qx = 1.2f, hos_ui_qy = 0.8f;   /* 16:9: the game's projection */
    run(0x1234, a, b);
    CHECK(a[0] == m00 && b[1] == m11);
    hos_wide_squeeze = 0.75f, hos_ui_qx = hos_ui_qy = 0.0f, hos_wide_keep_mask = 0x5;   /* before the first present */
    run(0x1234, a, b);
    CHECK(a[0] == m00 * 0.75f && b[1] == m11);
    hos_ui_qx = 1.2f, hos_ui_qy = 0.8f;   /* a 4:3 picture's: across and up */
    run(0x1234, a, b);
    CHECK(a[0] == m00 * 1.2f && b[1] == m11 * 0.8f && a[1] == 0.0f && b[0] == 0.0f);
    run(FADES, a, b);   /* (the event fade's screen, kept whole: the game's) */
    CHECK(a[0] == m00 && b[1] == m11);
    run(LOADING, a, b);   /* (the loading fade's, in the centred modes (0x5): scaled as the others; its pane is widened) */
    CHECK(a[0] == m00 * 1.2f && b[1] == m11 * 0.8f);
    hos_wide_keep_mask = 0xf;   /* (Full width: kept whole, the game's) */
    run(LOADING, a, b);
    CHECK(a[0] == m00 && b[1] == m11);
    hos_wide_keep_mask = 0x5;
    printf("PASS: the UI projection's site scales every screen not kept whole across and up, and 16:9 not at all\n");
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def build_run(cc, src, tag):
    with tempfile.TemporaryDirectory(prefix="hud-layout-") as tmp:
        c = Path(tmp) / (tag + ".c")
        exe = Path(tmp) / tag
        c.write_text(src, encoding="utf-8")
        built = subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wno-unused-function", "-o", str(exe), str(c), "-lm"],
                               capture_output=True, text=True)
        check(built.returncode == 0, tag + " did not compile:\n" + built.stdout + built.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check(run.returncode == 0, tag + " failed:\n" + run.stdout + run.stderr)
        print(run.stdout.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    host = (ROOT / "host" / "hos_wide_ui.c").read_text(encoding="utf-8")
    backend = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
    check("hos_wide_keep_mask &= ~2u;" in backend, "the menus could be kept whole, stretched, on a screen narrower than 16:9")
    check(any(site == oot_wide.HUD for _, _, site in oot_wide.SITES),
          "the UI projection's site is not installed")

    body = "".join(line for line in host.splitlines(keepends=True) if not line.startswith("#include"))
    decl = "extern volatile float hos_wide_ratio;\n"
    check(decl in body, "the host file no longer declares " + decl.strip())
    body = body.replace(decl, "", 1)
    build_run(a.cc, FIXTURE_HEAD + body + FIXTURE_MAIN, "plan")

    head = FIXTURE_HEAD.replace("volatile float hos_wide_squeeze, hos_wide_ratio;", "volatile float hos_wide_squeeze;")
    site = (SITE_MAIN.replace("SITE", oot_wide.HUD).replace("FADES", "0x%xULL" % oot_wide.KEEP_VTABLES[2])
            .replace("LOADING", "0x%xULL" % oot_wide.KEEP_VTABLES[3]))
    # (the site's own extern declarations, here defined above it)
    build_run(a.cc, head + site, "site")


if __name__ == "__main__":
    main()
