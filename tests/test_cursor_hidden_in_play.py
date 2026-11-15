"""The cursor over the game's window hides in play too, not only in the game's menus, once the pads or the keys are used
(host/display.c gm_cursor_shown), and shows again when the mouse is used. It is shown when the window opens. While it is
hidden a move of a few pixels does not bring it back, and neither does the window moving under it. The Insert menu keeps
its own pointer, and a move under it counts as the mouse used.

The cursor's own functions are compiled from display.c with authored stand-ins for the Win32 calls and run; the window
procedure's three conditions are read as text (tests/test_screensaver.py runs its timer branch).

python -I -B tests/test_cursor_hidden_in_play.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
DISPLAY = (ROOT / "host" / "display.c").read_text(encoding="utf-8")
CC = "gcc"


def case(label):
    start = DISPLAY.index(f"case {label}:")
    end = DISPLAY.index("\n    case ", start + 1)
    return DISPLAY[start:end]


def function(signature):
    start = DISPLAY.index(signature)
    end = DISPLAY.index("{", start) + 1
    depth = 1
    while depth:
        depth += (DISPLAY[end] == "{") - (DISPLAY[end] == "}")
        end += 1
    return DISPLAY[start:end]


FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
typedef long LONG;
typedef int64_t LONG64;
typedef unsigned long DWORD;
typedef unsigned UINT;
typedef void *HWND;
#define LOWORD(v) ((unsigned)(v) & 0xffff)
#define HIWORD(v) (((unsigned)(v) >> 16) & 0xffff)
static LONG64 InterlockedExchange64(volatile LONG64 *t, LONG64 v) { const LONG64 was = *t; *t = v; return was; }
static LONG InterlockedExchange(volatile LONG *t, LONG v) { const LONG was = *t; *t = v; return was; }
static LONG64 InterlockedCompareExchange64(volatile LONG64 *t, LONG64 v, LONG64 c) {
    const LONG64 was = *t;
    if (was == c) *t = v;
    return was;
}
static int64_t now_ns = 1000, input_ns;
static int64_t hos_now_ns(void) { return ++now_ns; }
int64_t hos_player_input_ns(void) { return input_ns; }
static int at_x, at_y;
static UINT dpi = 96;
static DWORD GetMessagePos(void) { return (DWORD)((at_x & 0xffff) | ((unsigned)(at_y & 0xffff) << 16)); }
static UINT window_dpi(HWND w) { (void)w; return dpi; }
static int MulDiv(int a, int b, int c) { return (int)(((int64_t)a * b + c / 2) / c); }
@CURSOR@
static void pad(void) { input_ns = ++now_ns; }
static void move(int x, int y) { at_x = x, at_y = y; gm_mouse_moved(NULL); }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    CHECK(gm_cursor_shown());                       /* the window opens with its cursor shown */
    move(500, 400); CHECK(gm_cursor_shown());
    pad(); CHECK(!gm_cursor_shown());               /* the first pad or key input hides it */
    move(500, 400); CHECK(!gm_cursor_shown());      /* a repeat at rest, or the window moved under it */
    move(503, 396); CHECK(!gm_cursor_shown());      /* within 4 pixels of where it was hidden */
    move(496, 404); CHECK(!gm_cursor_shown());      /* (from that point, not from the last move) */
    move(505, 400); CHECK(gm_cursor_shown());       /* past them: the mouse used */
    pad(); CHECK(!gm_cursor_shown());
    move(505, 395); CHECK(gm_cursor_shown());       /* either axis */
    move(506, 395); pad(); CHECK(!gm_cursor_shown());
    move(-300, 395); CHECK(gm_cursor_shown());      /* a screen left of the first one: negative coordinates */
    move(-301, 395); CHECK(gm_cursor_shown());      /* shown: every move is the mouse in use, */
    pad(); move(-301, 395); CHECK(!gm_cursor_shown());
    move(-302, 395); CHECK(!gm_cursor_shown());     /* and the pad after it hides it again */
    dpi = 192;                                      /* the slack follows the window's dpi: 8 pixels at 200% */
    move(-309, 395); CHECK(!gm_cursor_shown());
    move(-310, 395); CHECK(gm_cursor_shown());
    pad(); CHECK(!gm_cursor_shown());
    gm_mouse_used(); CHECK(gm_cursor_shown());      /* a button pressed in the game's menus */
    CHECK(!gm_move_counts(1, 5, 5, 5, 5, 4) && gm_move_counts(1, 5, 6, 5, 5, 4));
    CHECK(!gm_move_counts(0, 9, 1, 5, 5, 4) && gm_move_counts(0, 10, 5, 5, 5, 4) && gm_move_counts(0, 5, 0, 5, 5, 4));
    printf("PASS\n");
    return 0;
}
'''


class CursorHiddenInPlayTests(unittest.TestCase):
    def test_the_cursor_is_shown_until_the_pads_and_hidden_until_a_real_move(self):
        start = DISPLAY.index("static volatile LONG gm_shown")
        state = DISPLAY[start:DISPLAY.index("static int gm_move_counts(", start)]
        state = state[:state.rindex("/*")]   # (without the next function's comment)
        cursor = "\n".join([state, function("static int gm_move_counts("), function("static void gm_mouse_moved(HWND w)")])
        with tempfile.TemporaryDirectory(prefix="hookshot-cursor-") as temp:
            source, executable = Path(temp) / "fixture.c", Path(temp) / "fixture.exe"
            source.write_text(FIXTURE.replace("@CURSOR@", cursor), encoding="utf-8")
            subprocess.run([CC, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)], check=True)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_set_cursor_hides_it_outside_the_game_menus(self):
        body = case("WM_SETCURSOR")
        self.assertIn("(mouse_captured || (!menu_open && !gm_cursor_shown()))", body)
        self.assertNotIn("game_menu_open", body)

    def test_a_move_counts_in_play_and_under_the_insert_menu(self):
        body = case("WM_MOUSEMOVE")
        self.assertIn("if (mouse_captured) return 0;\n        gm_mouse_moved(w);", body)
        self.assertNotIn("menu_open", body.split("gm_mouse_moved(w);")[0])

    def test_the_pads_hide_it_at_once_in_play(self):
        body = case("WM_TIMER")
        line = next(l for l in body.splitlines() if "gm_cursor_off(w);" in l)
        self.assertNotIn("game_menu_open", line)
        self.assertIn("!menu_open && !gm_cursor_shown()", line)
        self.assertIn("GetForegroundWindow() == w", line)

    def test_a_game_menu_hides_it_at_the_screen_point(self):
        # (the point a move is measured from is a screen point everywhere: not turned into a client point here)
        body = function("static void menus_mouse(HWND w, int insert_open, int game_open)")
        hide = body[body.index("GetCursorPos(&at);"):body.index("gm_my = at.y;")]
        self.assertNotIn("ScreenToClient", hide)
        self.assertIn("InterlockedExchange(&gm_shown, 0), gm_mx = at.x", hide)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args, rest = parser.parse_known_args()
    CC = args.cc
    unittest.main(argv=sys.argv[:1] + rest)
