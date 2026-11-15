"""Walking on the keyboard (host/nn_hid.c): the movement keys push the left stick to its edge, so Link always ran; while
the Walk control's key is held (Caps Lock by default), or with the Controls tab's Walk mode on Toggle from one press of
it to the next, they tilt it to the Walk speed row's share of the way instead.

host/nn_hid.c's read_input, its binding table and the walk helpers are compiled alone, against fake key states, a fake
window, pad and mouse, and a fake hookshot_options.ini: Caps Lock walks by default; the tilt is the row's percent
(15 to 90, anything else its default), a diagonal by its length; the camera keys, the pads and the mouse keep theirs;
Toggle switches on a press, not while the Insert menu has the keys or the window is behind another one, and a key held
as they come back counts after its release; the key can be rebound (a mouse button too) and taken by another control;
an options file from before the Walk control that gives Caps Lock to another control leaves Walk without a key. The
wiring (option ids, the renderer's answer, the menu's rows and Controls page row, both bind_names copies, the docs) is
read from the source. The compiled fixture uses the real header's option constants to answer the host's live reads.

python -I -B tests/test_walk_key.py   (needs a C compiler)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    while text[end] in ";\n":
        end += 1
        if text[end - 1] == "\n":
            break
    return text[start:end]


def between(text, first, last):
    """from the line that holds first to the end of the block or statement that starts at last"""
    start = text.rindex("\n", 0, text.index(first)) + 1
    end = text.index(last, start)
    return text[start:end] + block(text[end:], last)


PRELUDE = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vk_api.h"
#include "../../host/hold_toggle.h"
#include "../../host/auto_run.h"   /* (Hookshot's Auto run, which read_input applies: off here, rx_option answers 0) */
_Static_assert(HOS_OPT_WALK_MODE == 49, "Walk mode option ID");
_Static_assert(HOS_OPT_WALK_SPEED == 50, "Walk speed option ID");
typedef long LONG;
typedef unsigned long DWORD;
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static void AcquireSRWLockExclusive(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockExclusive(SRWLOCK *p) { (void)p; }
static void AcquireSRWLockShared(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockShared(SRWLOCK *p) { (void)p; }
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_CAPITAL 0x14
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define MAX_PATH 260
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG c) { LONG o = *p; if (o == c) *p = v; return o; }
static void Sleep(int ms) { (void)ms; }
static char logs[8192];   /* (every line logged, one a line) */
static void hos_log(const char *f, ...) {
    const size_t n = strlen(logs);
    va_list a;
    va_start(a, f);
    vsnprintf(logs + n, sizeof logs - n - 1, f, a);
    va_end(a);
    strcat(logs, "\n");
}
static const char *ini_dir;
#ifdef _WIN32
#define EXE_SEP "\\"
#else
#define EXE_SEP "/"
#endif
static DWORD GetModuleFileNameA(void *m, char *b, DWORD n) { (void)m; return (DWORD)snprintf(b, n, "%s" EXE_SEP "hookshot.exe", ini_dir); }
#define hos_game_path_a(b,n) GetModuleFileNameA(0,b,n)
/* the keys held, the Insert menu (option 1), the Walk mode and Walk speed rows, the window in front */
static int held[256], menu_open, walk_mode, walk_speed, front = 1;
int rx_option(int id, int otherwise) {
    return id == 1 ? menu_open : id == HOS_OPT_WALK_MODE ? walk_mode : id == HOS_OPT_WALK_SPEED ? walk_speed : otherwise;
}
static short GetAsyncKeyState(int vk) { return held[vk & 255] ? (short)0x8000 : 0; }
static int64_t hos_now_ns(void) { return 0; }
/* read_input's other sources: no script, no control bridge, the window as front says */
static int npresses;
static int64_t script_end_ms;
static int64_t script_elapsed_ms(int64_t now) { (void)now; return 0; }
static void scripted_input(uint64_t *b, int32_t s[4]) { (void)b; (void)s; }
int hos_control_input(uint64_t *buttons, int32_t stick[4]) { (void)buttons; (void)stick; return 0; }   /* (host/control.c: off) */
static int game_in_front(void) { return front; }
static volatile LONG pad_raw_now;
static int pad_after_menu;   /* (main shares this state with the ocarina note word) */
static void player_used(void) {}
"""

STUBS = r"""
/* a controller's sticks and the captured mouse as the right stick, as a case sets them */
static int32_t pad_stick[4], mouse_right[2];
static int mouse_captured;
static void pads_read(uint64_t *b, int32_t s[4]) { (void)b; stick_max(s, pad_stick); }
int hos_mouse_native(void) { return -1; }
void hos_mouse_native_free(int free) { (void)free; }
void hos_mouse_native_stick(int held) { (void)held; }
static void mouse_setup(void) {}
static void mouse_sens_keys(void) {}
static int mouse_stick(int32_t *x, int32_t *y, int pointing) {
    (void)pointing;
    if (!mouse_captured) return 0;
    *x = mouse_right[0], *y = mouse_right[1];
    return 1;
}
/* (the Skip title screen row's presses, host/oot_autoload.c: none; Hookshot's Auto run: Link the player's to move) */
void hos_oot_autoload_input(void) {}
int hos_oot_player_free(void) { return 1; }
"""

MAIN = r"""
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint64_t buttons;
static int32_t st[4];
static void poll(void) {
    read_input(&buttons, st);
    CHECK(oc_consumed_pads == 0);   /* the inactive note layer must not consume the controller while walking */
}
static void release(void) { memset(held, 0, sizeof held); }
static int near_share(int32_t v, int percent) {   /* (a straight tilt: percent of 32767, within one step) */
    const int64_t want = 32767LL * percent / 100;
    return v >= want - 1 && v <= want + 1;
}
static int diagonal_share(int32_t x, int32_t y, int percent) {   /* (a diagonal's length: percent of 32767, within 0.5%) */
    const double want = 32767.0 * percent / 100, have2 = (double)x * x + (double)y * y;
    return x != 0 && (x == y || x == -y) && have2 > want * want * 0.99 && have2 < want * want * 1.01;
}
static int logged(const char *text) { return strstr(logs, text) != NULL; }
static void defaults(void) {
    binds_load();
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_WALK, 0) == VK_CAPITAL && hos_binding(HOS_BIND_KEY, HOS_KEY_WALK, 1) == 0);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_KEY_WALK, 0) == -1);   /* keys only */
    CHECK(WALK_SPEED_DEFAULT >= 20 && WALK_SPEED_DEFAULT <= 90);
    /* running: W gives the stick's edge, W and D both axes' (as before) */
    release(), held['W'] = 1, poll();
    CHECK(st[0] == 0 && st[1] == 32767 && st[2] == 0 && st[3] == 0);
    held['D'] = 1, poll();
    CHECK(st[0] == 32767 && st[1] == 32767);
    /* Hold (the default): with Caps Lock held, the row's share; the row unset: its default, logged once */
    release(), held['W'] = held[VK_CAPITAL] = 1, poll();
    CHECK(st[0] == 0 && near_share(st[1], WALK_SPEED_DEFAULT));
    {
        char line[96];
        snprintf(line, sizeof line, "hid: walking: the movement keys tilt the left stick to %d%% (0, %d)", WALK_SPEED_DEFAULT, st[1]);
        CHECK(logged(line));
    }
    held['S'] = 1, held['W'] = 0, poll();
    CHECK(st[0] == 0 && near_share(-st[1], WALK_SPEED_DEFAULT));
    /* a diagonal walks too: its length is the share, not each axis */
    release(), held['W'] = held['A'] = held[VK_CAPITAL] = 1, poll();
    CHECK(st[0] < 0 && diagonal_share(st[0], st[1], WALK_SPEED_DEFAULT));
    /* Caps Lock alone moves nothing; the camera keys keep their edge */
    release(), held[VK_CAPITAL] = 1, poll();
    CHECK(st[0] == 0 && st[1] == 0 && st[2] == 0 && st[3] == 0);
    held[VK_UP] = held[VK_LEFT] = 1, poll();
    CHECK(st[0] == 0 && st[1] == 0 && st[2] == -32767 && st[3] == 32767);
    /* a controller's stick is not scaled, held alone or with a key that walks */
    release(), held[VK_CAPITAL] = 1, pad_stick[1] = 30000, poll();
    CHECK(st[1] == 30000);
    held['S'] = 1, pad_stick[1] = 0, pad_stick[0] = -32767, poll();
    CHECK(st[0] == -32767 && near_share(-st[1], WALK_SPEED_DEFAULT));
    memset(pad_stick, 0, sizeof pad_stick);
    /* nor the mouse's right stick */
    release(), mouse_captured = 1, mouse_right[0] = 12000, mouse_right[1] = -5000, held['W'] = held[VK_CAPITAL] = 1, poll();
    CHECK(st[2] == 12000 && st[3] == -5000 && near_share(st[1], WALK_SPEED_DEFAULT));
    mouse_captured = 0, mouse_right[0] = mouse_right[1] = 0;
    /* the Walk speed row: 15 to 90, anything else its default */
    const int speeds[][2] = {{60, 60}, {15, 15}, {90, 90}, {10, WALK_SPEED_DEFAULT}, {95, WALK_SPEED_DEFAULT}, {0, WALK_SPEED_DEFAULT}};
    for (unsigned i = 0; i < sizeof speeds / sizeof *speeds; i++) {
        walk_speed = speeds[i][0];
        release(), held['D'] = held[VK_CAPITAL] = 1, poll();
        CHECK(near_share(st[0], speeds[i][1]) && st[1] == 0);
        held['W'] = 1, poll();
        CHECK(diagonal_share(st[0], st[1], speeds[i][1]));
    }
    CHECK(logged("hid: walking: the movement keys tilt the left stick to 60% (19660, 0)"));
    walk_speed = 0;
    /* Toggle: a press walks until the next one */
    walk_mode = 1;
    release(), held['W'] = 1, poll();
    CHECK(st[1] == 32767);
    held[VK_CAPITAL] = 1, poll();
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT) && logged("hid: walking on (Walk mode Toggle)"));
    held[VK_CAPITAL] = 0, poll();
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT));
    held[VK_CAPITAL] = 1, poll();
    CHECK(st[1] == 32767 && logged("hid: walking off (Walk mode Toggle)"));
    held[VK_CAPITAL] = 0, poll();
    CHECK(st[1] == 32767);
    /* not while the Insert menu has the keys, and a key held as it closes counts after its release */
    held[VK_CAPITAL] = 1, poll(), held[VK_CAPITAL] = 0, poll();   /* (on) */
    menu_open = 1, held[VK_CAPITAL] = 1, poll();
    menu_open = 0, poll();
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT));   /* (still on: the press inside the menu and the one held out of it) */
    held[VK_CAPITAL] = 0, poll(), held[VK_CAPITAL] = 1, poll();
    CHECK(st[1] == 32767);   /* (off: a press of its own) */
    held[VK_CAPITAL] = 0, poll();
    /* nor while the window is behind another one (Alt+Tab back with the key held) */
    held[VK_CAPITAL] = 1, poll(), held[VK_CAPITAL] = 0, poll();   /* (on) */
    front = 0, held[VK_CAPITAL] = 1, poll();
    front = 1, poll();
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT));
    held[VK_CAPITAL] = 0, poll(), held[VK_CAPITAL] = 1, poll();
    CHECK(st[1] == 32767);
    held[VK_CAPITAL] = 0, poll(), held[VK_CAPITAL] = 1, poll(), held[VK_CAPITAL] = 0, poll();   /* (on, then Hold) */
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT));
    /* back to Hold: walking only while the key is held */
    walk_mode = 0, poll();
    CHECK(st[1] == 32767);
    held[VK_CAPITAL] = 1, poll();
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT));
    /* rebinding: Caps Lock given to another control leaves Walk; another key walks; Reset to defaults */
    hos_bind(HOS_BIND_KEY, HOS_SW_X, 0, VK_CAPITAL);
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_WALK, 0) == 0);
    poll();
    CHECK(st[1] == 32767 && (buttons >> HOS_SW_X & 1));
    hos_bind(HOS_BIND_KEY, HOS_KEY_WALK, 0, 'V');
    release(), held['W'] = held['V'] = 1, poll();
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT));
    /* a mouse button: only while the mouse is captured, once the click that captured it was let go */
    hos_bind(HOS_BIND_KEY, HOS_KEY_WALK, 0, VK_XBUTTON1);
    release(), held['W'] = held[VK_XBUTTON1] = 1, poll();
    CHECK(st[1] == 32767);
    mouse_captured = 1, poll();
    CHECK(st[1] == 32767);   /* (held as the capture began) */
    held[VK_XBUTTON1] = 0, poll(), held[VK_XBUTTON1] = 1, poll();
    CHECK(near_share(st[1], WALK_SPEED_DEFAULT));
    mouse_captured = 0;
    hos_bind(HOS_BIND_KEY, -1, 0, 0);
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_WALK, 0) == VK_CAPITAL);
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    ini_dir = argv[2];
    const char *mode = argv[1];
    if (!strcmp(mode, "defaults")) {
        defaults();
    } else if (!strcmp(mode, "taken")) {   /* key_x=20: Caps Lock is X's, and the file has no key_walk */
        binds_load();
        CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_X, 0) == VK_CAPITAL && hos_binding(HOS_BIND_KEY, HOS_KEY_WALK, 0) == 0);
        CHECK(logged("hid: no Walk key: hookshot_options.ini gives its default key (20) to another control"));
        release(), held['W'] = held[VK_CAPITAL] = 1, poll();
        CHECK(st[1] == 32767 && (buttons >> HOS_SW_X & 1));
    } else if (!strcmp(mode, "named")) {   /* key_x=20 and key_walk=86: both as the file says */
        binds_load();
        CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_X, 0) == VK_CAPITAL && hos_binding(HOS_BIND_KEY, HOS_KEY_WALK, 0) == 'V');
        CHECK(!logged("no Walk key"));
    } else if (!strcmp(mode, "kept")) {   /* a file from before without Caps Lock in it: Walk keeps its default */
        binds_load();
        CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_X, 0) == VK_SPACE && hos_binding(HOS_BIND_KEY, HOS_KEY_WALK, 0) == VK_CAPITAL);
        CHECK(!logged("no Walk key"));
    } else {
        return 2;
    }
    if (!fails) printf("PASS %s\n", mode);
    return fails != 0;
}
"""

INI = {"defaults": None, "taken": "key_x=20,0\n", "named": "key_x=20,0\nkey_walk=86,0\n", "kept": "key_x=32,0\n"}


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def names(text):
    return re.findall(r'"([a-z_]+)"', text[text.index("bind_names[HOS_KEY_N] = {"):].split("};")[0])


class WalkKeyTests(unittest.TestCase):
    def test_walk(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        hid = read("host", "nn_hid.c")
        code = [block(hid, "enum { B_A"), block(hid, "static int key("), block(hid, "static int kmap_down("),
                between(hid, "static const char *const bind_names[HOS_KEY_N]", "void hos_bind("),
                between(hid, "static int mouse_vk_slot(", "static int bound_down("), block(hid, "static void stick_max("),
                block(hid, "static void oc_notes("),
                block(hid, "static int pads_in_background("), block(hid, "static int walk_held("),
                block(hid, "enum { WALK_SPEED_DEFAULT"), block(hid, "static int walk_percent("),
                block(hid, "static void move_keys_stick(")]
        # (Hookshot's pads_merge, the pads held after the Insert menu closes, calls the stand-in pads_read)
        source = PRELUDE + "\n".join(code) + STUBS + block(hid, "static int pads_merge(") + "\n" + \
            block(hid, "static void read_input(") + MAIN
        env = {k: v for k, v in os.environ.items() if not k.startswith(("HOS_INPUT_", "HOS_MOUSE_"))}
        with tempfile.TemporaryDirectory(prefix="hookshot-walk-key-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.c").write_text(source, encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            build = subprocess.run([compiler, "-std=gnu11", "-D_CRT_SECURE_NO_WARNINGS", "-include", ENV_ON_H, "-Wall", "-Wextra", "-Werror",
                                    "-Wno-unused-function", "-I", str(ROOT / "gpu" / "vk"), str(tmp / "t.c"), "-o", str(binary)],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for mode, ini in INI.items():
                folder = tmp / mode
                folder.mkdir()
                if ini is not None:
                    (folder / "hookshot_options.ini").write_text(ini, encoding="utf-8")
                run = subprocess.run([str(binary), mode, str(folder)], text=True, capture_output=True, timeout=30, env=env)
                self.assertEqual(run.returncode, 0, mode + ": " + run.stdout + run.stderr)

    def test_wiring(self):
        api, hid, menu = read("gpu", "vk", "vk_api.h"), read("host", "nn_hid.c"), read("gpu", "vk", "vk_menu.cpp")
        header, replay = read("gpu", "vk", "vk_menu.h"), read("gpu", "vk", "vk_replay.cpp")
        self.assertIn("enum { HOS_OPT_WALK_MODE = 49, HOS_OPT_WALK_SPEED };", api)
        self.assertRegex(api, r"HOS_KEY_OC_NOTE_X, HOS_KEY_WALK, HOS_KEY_N \};")
        # (the host's two copies of the names: the ini's key_walk)
        self.assertEqual(names(hid), names(menu))
        self.assertEqual(names(hid)[-1], "walk")
        self.assertIn('{HOS_KEY_WALK, "Walk"}', menu, "the Controls page has no Walk row")
        option = replay[replay.index("t.option = "):]
        option = option[:option.index("};")]
        self.assertIn("if (id == HOS_OPT_WALK_MODE) return opts.walk_mode;", option)
        self.assertIn("if (id == HOS_OPT_WALK_SPEED) return opts.walk_speed;", option)
        # (the rows: Controls tab, kept out of the quality preset; the speed's range is the host's)
        mode = re.search(r'\{TAB_CONTROLS, "Walk mode", &opts\.walk_mode, 0, 1, 1, tx_walk_mode,[^}]*\}', menu)
        speed = re.search(r'\{TAB_CONTROLS, "Walk speed", &opts\.walk_speed, (\d+), (\d+), \d+, tx_percent,[^}]*\}', menu)
        self.assertTrue(mode and speed, "the Controls tab lacks the Walk mode or Walk speed row")
        for row in (mode, speed):
            self.assertTrue(row.group(0).endswith("false, true}"), row.group(0)[:40] + ": not preset-exempt, or a restart row")
        self.assertIn("return v >= %s && v <= %s ? v : WALK_SPEED_DEFAULT;" % speed.groups(), hid)
        default = int(re.search(r"enum \{ WALK_SPEED_DEFAULT = (\d+) \};", hid).group(1))
        self.assertIn("int walk_speed = %d;" % default, header, "the menu's default is not the host's")
        self.assertIn("int walk_mode = 0;", header)
        self.assertIn("rx_option(HOS_OPT_WALK_MODE, 0) == 1", block(hid, "static void read_input("))
        # (documented where the rows and the ini's keys are)
        readme, development = read("readme.md"), read("docs", "development.md")
        lo, hi = speed.groups()
        for text in ("| | Walk mode | Hold, Toggle | Hold |",
                     "| | Walk speed | %s%% to %s%%, in steps of 5 | %d%% |" % (lo, hi, default), "`walk`"):
            self.assertIn(text, readme)
        self.assertIn("| `walk_mode` | 0 Hold, 1 Toggle |", development)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
