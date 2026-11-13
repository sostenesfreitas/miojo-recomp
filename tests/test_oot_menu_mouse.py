"""The mouse in the game's own menus (host/oot_menu.c): where it points, what it asks of the game.

The file is compiled on its own against a fake guest memory holding menu screens as the game lays them out: a screen's
list of entries (+136 the head, +144 the first node, +156 the entry's offset from its node; entry+24 the button), a
button's focus at +56 and its hit box at *(button+0x58) (its matrix at +0x28: the scale at +0x28 and +0x38, the
translation at +0x4c and +0x50; its rectangle at +0x84, about the origin; a second box's scale at +0xd8 and centre at
+0xfc). hos_oot_menu_tick is what a menu screen's update calls each frame (f_802f1380), hos_oot_menu_focus what the pause
menu's focus handler calls (f_802ed660; nx2/oot_patches.py). The game's focus setter (f_8088ffc0) is a fake that records
its button; the pad's presses (hos_hid_tap) are recorded as their script.

In the pause menu the mouse moved onto a button gives it the focus through the setter, a click on the focused button is
A and the right button B, a click on the "Back" prompt is B even with a button's box under it, the L and R signs are L
and R and the wheel turns the pages. A slot whose hit box covers a side of its picture is hit on the other side too. A
focus change the game reports from another thread while the setter runs (the mouse's lock held) does not wait for the
lock, and is the mouse's at its next tick. On a
list of the title's 60 x 60 marks a row is hit across the line and in the gaps between rows, and the wheel moves the
focus down a row with the setter, without the pad. On the R button's item choice (a carousel) the wheel presses the
D-pad's right (down) and left (up), a notch each, one after another, and the mouse is not freed (it stays the
camera's). With the cursor hidden (R held counts as the pad in use) the wheel still moves a list, and moving the mouse
points at nothing. Once frames go on with no menu holding the mouse, the window is told at once.
python -I -B tests/test_oot_menu_mouse.py   (Windows: the feature's; elsewhere it is skipped)
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HOS_H = r"""
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct Ctx { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
typedef void (*A64Fn)(Ctx *);
A64Fn a64_lookup(uint64_t addr);
extern uint8_t *fake_heap;
#define FAKE_BASE 0x100000ULL
#define HOS_GUEST_SPACE (FAKE_BASE + (4ULL << 20))
#define GP(a) ((void *)(fake_heap + ((uint64_t)(a) - FAKE_BASE)))
void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);
"""

VK_API_H = r"""
#pragma once
enum { HOS_MOUSE_LEFT = 1, HOS_MOUSE_RIGHT = 2, HOS_MOUSE_MIDDLE = 4 };
"""

FIXTURE = r"""
#include "hos.h"
#include "../gpu/vk/vk_api.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
uint8_t *fake_heap;
static uint64_t heap_top = FAKE_BASE + 0x1000;
static int64_t clock_ns = 1000000000LL;
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc(10, stderr); }
int64_t hos_now_ns(void) { return clock_ns; }
void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr);
void hos_oot_menu_tick(Ctx *c, uint64_t screen);

/* the window: 3840 x 2160, so a pixel is a unit of the layout's (x right of the centre, y up) */
static int ptr_x = 1920, ptr_y = 1080, wheel_next;
static uint32_t pressed_next;
static int pointer_hidden;   /* (the cursor hidden while the pad is in use: the wheel only) */
static int menu_frees, menu_gone;   /* (the game's menus' mouse: freed for a menu's frame; their close told) */
void hos_mouse_game_menu(void) { menu_frees++; }
void hos_mouse_game_menu_gone(void) { menu_gone++; }
int hos_mouse_wheel_take(void) { const int w = wheel_next; wheel_next = 0; return w; }   /* (the wheel only, mouse kept) */
int hos_mouse_game_pointer(int *x, int *y, int *w, int *h, uint32_t *pressed, int *wheel) {
    *x = ptr_x, *y = ptr_y, *w = 3840, *h = 2160, *pressed = pressed_next, *wheel = wheel_next;
    pressed_next = 0, wheel_next = 0;
    return pointer_hidden ? 2 : 1;
}
static char taps[1024];
void hos_hid_tap(const char *press) { strncat(taps, press, sizeof taps - strlen(taps) - 2), strcat(taps, ";"); }
void hos_hid_stick(int lx, int ly, int rx, int ry, int hold_ms) { (void)lx, (void)ly, (void)rx, (void)ry, (void)hold_ms; }
void hos_hid_drag(int dx, int dy, int right) { (void)dx, (void)dy, (void)right; }
static uint64_t focus_asked, focus_screen;
static int focus_calls;
/* (a focus change reported from another of the game's threads while the setter runs: the hook there must not wait for
   the mouse's lock, held around this call) */
static uint64_t other_screen, other_pane;
static DWORD other_wait = WAIT_TIMEOUT + 1;
static DWORD WINAPI other_thread(LPVOID arg) {
    Ctx other = {0};
    (void)arg;
    hos_oot_menu_focus(&other, other_screen, other_pane, 0);
    return 0;
}
static void focus_setter(Ctx *c) {
    focus_screen = c->x[0], focus_asked = c->x[1], focus_calls++, c->x[0] = 1;
    if (other_pane) {
        HANDLE t = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
        other_wait = t ? WaitForSingleObject(t, 5000) : WAIT_FAILED;
        if (t) CloseHandle(t);
        other_pane = 0;
    }
}
A64Fn a64_lookup(uint64_t addr) { return addr == 0x8088ffc0ULL ? focus_setter : NULL; }

static uint64_t alloc(size_t n) { const uint64_t a = heap_top; heap_top += (n + 15) & ~(size_t)15; return a; }
static void put64(uint64_t a, uint64_t v) { memcpy(GP(a), &v, 8); }
static void putf(uint64_t a, float f) { memcpy(GP(a), &f, 4); }
static uint64_t get64(uint64_t a) { uint64_t v; memcpy(&v, GP(a), 8); return v; }

/* a screen: its vtable, its focus manager (*(screen+32)+96), an empty list */
static uint64_t screen_new(uint64_t vtable) {
    const uint64_t s = alloc(1024), mgr = alloc(128);
    put64(s, vtable), put64(s + 32, mgr), put64(mgr + 96, alloc(16));
    put64(s + 144, s + 136);   /* (no entries: the first node is the head) */
    return s;
}
/* a button with a hit box w x h centred at (x, y), last in the screen's list */
static uint64_t button_new(uint64_t s, float x, float y, float w, float h) {
    const uint64_t entry = alloc(64), button = alloc(128), box = alloc(0x200);
    put64(entry + 24, button), put64(entry + 8, s + 136), put64(button + 0x58, box);
    putf(box + 0x28, 1), putf(box + 0x38, 1), putf(box + 0x4c, x), putf(box + 0x50, y);
    putf(box + 0x84, -w / 2), putf(box + 0x88, -h / 2), putf(box + 0x8c, w), putf(box + 0x90, h);
    uint64_t node = s + 144;   /* (the link to the head: the last node's +8, or the first's at +144) */
    while (get64(node) != s + 136) node = get64(node) + 8;
    put64(node, entry);
    return button;
}
static void focused(uint64_t button, int on) { *(uint8_t *)GP(button + 56) = (uint8_t)on; }

static Ctx ctx;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (taps \"%s\", focus %#llx)\n", __LINE__, #c, taps, \
                                           (unsigned long long)focus_asked); fails++; } } while (0)
/* a frame of the screen's with the mouse at (x, y) in the layout's units */
static void frame(uint64_t s, float x, float y) {
    ptr_x = (int)(x + 1920), ptr_y = (int)(1080 - y);
    clock_ns += 16666667;
    hos_oot_menu_tick(&ctx, s);
}
static void reset(void) { taps[0] = 0, focus_asked = 0, focus_calls = 0; }
/* the game takes the focus the setter asked for: its handler runs (the pause menu's), the button shows it */
static void take_focus(uint64_t s, uint64_t from, uint64_t to, int pause) {
    focused(from, 0), focused(to, 1);
    if (pause) hos_oot_menu_focus(&ctx, s, to, 0);
}

int main(void) {
    fake_heap = (uint8_t *)calloc(1, 4u << 20);
    ctx.x[31] = FAKE_BASE + (3u << 20);

    /* The pause menu: two slots, a box over the "Back" prompt's corner, a slot whose hit box is its picture's right side */
    const uint64_t pause = screen_new(0x84c0baa8ULL);
    const uint64_t a = button_new(pause, -500, 200, 200, 200), b = button_new(pause, 0, 200, 200, 200);
    const uint64_t corner = button_new(pause, 1750, -1020, 400, 200);
    const uint64_t side = button_new(pause, -1120, -400, 100, 260);
    {   /* (its picture's box, centred 80 to the left of the hit box's) */
        const uint64_t box = get64(side + 0x58);
        putf(box + 0xd8, 1), putf(box + 0xfc, -1200), putf(box + 0x100, -400);
    }
    focused(b, 1);
    hos_oot_menu_focus(&ctx, pause, b, 0);
    frame(pause, 0, 200);   /* (the screen seen, its focus found: the mouse is for it) */
    reset();
    frame(pause, -500, 210);
    CHECK(focus_calls == 1 && focus_asked == a && focus_screen == pause);   /* hover: the setter, on that button */
    CHECK(taps[0] == 0);   /* silently: no pad */
    take_focus(pause, b, a, 1);
    reset();
    frame(pause, -505, 210);
    CHECK(focus_calls == 0);   /* already focused: nothing asked */
    pressed_next = HOS_MOUSE_LEFT;
    frame(pause, -505, 210);
    CHECK(strstr(taps, "A/80") != NULL);   /* a click on it: A */
    reset();
    pressed_next = HOS_MOUSE_RIGHT;
    frame(pause, -505, 210);
    CHECK(!strcmp(taps, "0:B/80;"));   /* the right button: B */
    reset();
    frame(pause, 1700, -1020);
    pressed_next = HOS_MOUSE_LEFT;
    frame(pause, 1700, -1020);
    CHECK(focus_asked != corner && !strcmp(taps, "0:B/80;"));   /* the Back prompt: B, the box under it not focused */
    reset();
    pressed_next = HOS_MOUSE_LEFT;
    frame(pause, 1000, 1000);
    CHECK(!strcmp(taps, "0:R/120;"));   /* the R sign */
    reset();
    pressed_next = HOS_MOUSE_LEFT;
    frame(pause, -1000, 1000);
    CHECK(!strcmp(taps, "0:L/120;"));   /* the L sign */
    reset();
    wheel_next = -1;
    frame(pause, -1000, 1000);
    CHECK(!strcmp(taps, "0.00:R/60;"));   /* the wheel down: the next page */
    reset();
    frame(pause, -1260, -400);
    CHECK(focus_asked == side);   /* the slot's picture's other side: hit */
    take_focus(pause, a, side, 1);
    reset();
    frame(pause, -1400, -400);
    CHECK(focus_calls == 0);   /* past it: nothing */
    /* the game reports the focus from another thread while the setter runs: the hook returns at once, and the change
       is the mouse's at its next tick (a click then is A at once: the focus it asked for has come) */
    reset();
    other_screen = pause, other_pane = b;
    frame(pause, 0, 210);
    CHECK(focus_asked == b && other_wait == WAIT_OBJECT_0);
    focused(side, 0), focused(b, 1);
    reset();
    pressed_next = HOS_MOUSE_LEFT;
    frame(pause, 0, 210);
    CHECK(focus_calls == 0 && !strcmp(taps, "0:A/80;"));

    /* A list of the title's marks (60 x 60, a line's middle), opened over it; the pause menu stopped */
    clock_ns += 300000000LL;
    const uint64_t title = screen_new(0x84c106e8ULL);
    const uint64_t r0 = button_new(title, -600, 200, 60, 60), r1 = button_new(title, -600, 0, 60, 60),
                   r2 = button_new(title, -600, -200, 60, 60);
    focused(r0, 1);
    frame(title, -600, 200);
    reset();
    frame(title, -200, 0);
    CHECK(focus_asked == r1);   /* across the line, 400 from its mark */
    take_focus(title, r0, r1, 0);
    reset();
    frame(title, -600, 110);
    CHECK(focus_asked == r0);   /* in the gap between rows: the nearer */
    take_focus(title, r1, r0, 0);
    reset();
    frame(title, -600, -290);
    CHECK(focus_asked == r2);   /* below the last row by as much as the gap's half */
    take_focus(title, r0, r2, 0);
    reset();
    frame(title, -600, -400);
    CHECK(focus_calls == 0);   /* further: nothing */
    take_focus(title, r2, r0, 0);
    reset();
    wheel_next = -1;
    frame(title, -600, -400);
    CHECK(focus_asked == r1 && taps[0] == 0);   /* the wheel down: the focus a row down, by the setter, no pad */

    /* R held (the pad in use) over the list: the cursor hidden; the mouse moved over another row takes no focus, the
       wheel still moves it */
    take_focus(title, r0, r1, 0);
    pointer_hidden = 1;
    reset();
    frame(title, -600, -200);
    CHECK(focus_calls == 0);
    wheel_next = -1;
    frame(title, -600, -200);
    CHECK(focus_asked == r2);
    pointer_hidden = 0;

    /* The R button's item choice (a carousel in a row, in play), opened next: the mouse is not freed (the camera's), the
       wheel is the D-pad's right and left */
    clock_ns += 300000000LL;
    const uint64_t choice = screen_new(0x84c0d430ULL);
    const uint64_t c0 = button_new(choice, -300, 0, 200, 200);
    const uint64_t c1 = button_new(choice, 300, 0, 200, 200);
    focused(c0, 1);
    frame(choice, -300, 0);
    reset();
    menu_frees = 0;
    frame(choice, 300, 0);
    CHECK(menu_frees == 0 && focus_calls == 0 && focus_asked != c1);   /* no freed mouse, no pointing */
    wheel_next = -1;
    frame(choice, -300, 0);
    CHECK(!strcmp(taps, "0.000:RIGHT/60;") && focus_calls == 0);   /* down: right, a press */
    reset();
    wheel_next = 2;
    frame(choice, -300, 0);   /* (the next frame: each press after the last has been let go, 120 ms on) */
    CHECK(!strcmp(taps, "0.103:LEFT/60,0.223:LEFT/60;") && focus_calls == 0);   /* up two notches: left twice */
    reset();
    wheel_next = -20;
    frame(choice, -300, 0);   /* (a long spin: no more than 0.72 s of presses queued) */
    CHECK(!strcmp(taps, "0.327:RIGHT/60,0.447:RIGHT/60,0.567:RIGHT/60,0.687:RIGHT/60;"));
    clock_ns += 2000000000LL;   /* (2 s on, all let go: the next press at once) */
    reset();
    wheel_next = -1;
    frame(choice, -300, 0);
    CHECK(!strcmp(taps, "0.000:RIGHT/60;"));

    /* Back in play (R let go): only the HUD's screen runs, no menu's. The window is told the menus have closed once
       50 ms of frames have had none: the camera gets its mouse back then, not 300 ms on */
    clock_ns += 300000000LL;
    const uint64_t hud = screen_new(0x84c08300ULL);
    menu_gone = 0;
    frame(hud, 0, 0);
    frame(hud, 0, 0);
    CHECK(menu_gone == 0);   /* (the first frames: not yet 50 ms) */
    for (int i = 0; i < 4; i++) frame(hud, 0, 0);
    CHECK(menu_gone > 0);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


class OotMenuMouseTests(unittest.TestCase):
    def test_points_and_clicks_the_menus(self):
        if sys.platform != "win32":
            self.skipTest("the game's menus' mouse is Windows' (host/oot_menu.c: _WIN32)")
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-oot-menu-") as tmp:
            tmp = Path(tmp)
            (tmp / "host").mkdir()
            (tmp / "gpu" / "vk").mkdir(parents=True)
            # (beside the stand-in hos.h, and its "../gpu/vk/vk_api.h" the stand-in's)
            (tmp / "host" / "hos.h").write_text(HOS_H, encoding="utf-8")
            (tmp / "gpu" / "vk" / "vk_api.h").write_text(VK_API_H, encoding="utf-8")
            # (and the real "../gpu/vk/compat/hookshot_env.h": env_on, the switches' rule)
            (tmp / "gpu" / "vk" / "compat").mkdir()
            shutil.copy(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h", tmp / "gpu" / "vk" / "compat" / "hookshot_env.h")
            shutil.copy(ROOT / "host" / "oot_menu.c", tmp / "host" / "oot_menu.c")
            (tmp / "host" / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t.exe"
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-O1", "-g", "-D_WIN32_WINNT=0x0601",
                                    str(tmp / "host" / "t.c"), str(tmp / "host" / "oot_menu.c"), "-o", str(binary)],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
