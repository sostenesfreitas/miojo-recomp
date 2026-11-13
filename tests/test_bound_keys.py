"""The keys-only controls (the left stick's and the camera's directions) and the mouse's buttons are bindings like
the Switch's buttons (host/nn_hid.c).

host/nn_hid.c's binding table and its bound_down are compiled alone, against fake key states and a fake
hookshot_options.ini: the defaults (WASD, the arrows, the mouse's left button Y and right button ZL), a mouse button
only while the mouse is captured and only once the click that captured it was let go, key_move_up from the file, a
pad_ line for a keys-only control ignored, a key taken from the control that had it, and Enter with Alt still no key.
Gamepad checks cover old single bindings, a saved alternate, either slot pressing the same button, reassignment and clearing.
python -I -B tests/test_bound_keys.py   (needs a C compiler)
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

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
    return text[start:text.index(last)] + block(text, last)


PRELUDE = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vk_api.h"
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
#define VK_SPACE 0x20
#define VK_NUMPAD0 0x60
#define VK_NUMPAD2 0x62
#define VK_NUMPAD4 0x64
#define VK_NUMPAD6 0x66
#define VK_NUMPAD8 0x68
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define MAX_PATH 260
#define NULL_MODULE 0
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG c) { LONG o = *p; if (o == c) *p = v; return o; }
static void Sleep(int ms) { (void)ms; }
static void hos_log(const char *f, ...) { (void)f; }
static const char *ini_dir;
/* (the separator the system's own path has: nn_hid.c looks for a backslash first, so a mixed path finds the wrong folder) */
#ifdef _WIN32
#define EXE_SEP "\\"
#else
#define EXE_SEP "/"
#endif
static DWORD GetModuleFileNameA(void *m, char *b, DWORD n) { (void)m; return (DWORD)snprintf(b, n, "%s" EXE_SEP "hookshot.exe", ini_dir); }
#define hos_game_path_a(b,n) GetModuleFileNameA(0,b,n)
static int held[256];
static int key(int vk) { return held[vk & 255]; }
"""

MAIN = r"""
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void release(void) { memset(held, 0, sizeof held); }
int main(int argc, char **argv) {
    (void)argc;
    ini_dir = argv[1];
    binds_load();
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_MOVE_UP, 0) == 'Z');   /* from the file */
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_MOVE_LEFT, 0) == 'A');   /* the default */
    CHECK(hos_binding(HOS_BIND_PAD, HOS_KEY_MOVE_UP, 0) == -1);   /* no gamepad input for a keys-only control */
    CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_Y, 0) == VK_LBUTTON && hos_binding(HOS_BIND_KEY, HOS_SW_ZL, 1) == VK_RBUTTON);
    release(), held['Z'] = 1;
    CHECK(bound_down(HOS_KEY_MOVE_UP, 0));
    release(), held[VK_LEFT] = 1;
    CHECK(bound_down(HOS_KEY_CAM_LEFT, 0));
    /* the mouse's left button: held when the capture begins, it counts only after it was let go */
    release(), held[VK_LBUTTON] = 1;
    mouse_buttons_arm(1);
    CHECK(!bound_down(HOS_SW_Y, 1));
    release(), mouse_buttons_arm(1), held[VK_LBUTTON] = 1, mouse_buttons_arm(1);
    CHECK(bound_down(HOS_SW_Y, 1));
    CHECK(!bound_down(HOS_SW_Y, 0));   /* not while the mouse is not captured */
    mouse_buttons_arm(0);
    CHECK(!bound_down(HOS_SW_Y, 1));   /* a new capture arms again */
    /* a key bound to a keys-only control is taken from the button that had it */
    hos_bind(HOS_BIND_KEY, HOS_KEY_CAM_UP, 0, 'E');
    CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_A, 0) == 0 && hos_binding(HOS_BIND_KEY, HOS_KEY_CAM_UP, 0) == 'E');
    hos_bind(HOS_BIND_PAD, HOS_KEY_CAM_UP, 0, HOS_PAD_SOUTH);   /* no gamepad input for it: ignored */
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_B, 0) == HOS_PAD_SOUTH);
    /* Enter with Alt is fullscreen, not Plus */
    release(), held[VK_RETURN] = held[VK_MENU] = 1;
    CHECK(!bound_down(HOS_SW_PLUS, 0));
    const uint64_t face = 1ULL << HOS_PAD_EAST, paddle = 1ULL << HOS_PAD_RPADDLE1, a = 1ULL << HOS_SW_A;
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_A, 0) == HOS_PAD_EAST);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_A, 1) == HOS_PAD_RPADDLE1);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_B, 1) == -1);   /* old one-value entry */
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_X, 1) == -1);   /* invalid second value */
    CHECK(bound_pad_buttons(face) == a && bound_pad_buttons(paddle) == a && bound_pad_buttons(face | paddle) == a);
    CHECK(bound_pad_buttons(1ULL << HOS_PAD_RTRIGGER) == 1ULL << HOS_SW_ZR);
    CHECK(bound_pad_buttons(1ULL << HOS_PAD_TOUCHPAD) == 0);
    hos_bind(HOS_BIND_PAD, HOS_SW_B, 1, HOS_PAD_RPADDLE1);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_A, 1) == -1);
    CHECK(bound_pad_buttons(paddle) == 1ULL << HOS_SW_B && bound_pad_buttons(face) == a);
    hos_bind(HOS_BIND_PAD, HOS_SW_B, 1, -1);
    CHECK(bound_pad_buttons(paddle) == 0 && bound_pad_buttons(face) == a);
    hos_bind(HOS_BIND_PAD, HOS_SW_A, 1, HOS_PAD_RPADDLE1);
    /* another button's only input into an empty Input 2: refused, nothing left unbound */
    const int y0 = hos_binding(HOS_BIND_PAD, HOS_SW_Y, 0);
    hos_bind(HOS_BIND_PAD, HOS_SW_B, 1, y0);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_B, 1) == -1 && hos_binding(HOS_BIND_PAD, HOS_SW_Y, 0) == y0);
    /* from a button with an Input 2: taken, and that button's Input 2 moves up */
    hos_bind(HOS_BIND_PAD, HOS_SW_B, 1, HOS_PAD_EAST);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_B, 1) == HOS_PAD_EAST);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_A, 0) == HOS_PAD_RPADDLE1 && hos_binding(HOS_BIND_PAD, HOS_SW_A, 1) == -1);
    hos_bind(HOS_BIND_KEY, -1, 0, 0);
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_MOVE_UP, 0) == 'W' && hos_binding(HOS_BIND_KEY, HOS_SW_A, 0) == 'E');
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_A, 1) == -1 && bound_pad_buttons(face) == a);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


class BoundKeysTests(unittest.TestCase):
    def test_bindings(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
        code = [block(hid, "static int kmap_down("),
                between(hid, "static const char *const bind_names[HOS_KEY_N]", "\nvoid hos_bind("),   # (its definition)
                block(hid, "static uint64_t bound_pad_buttons("),
                between(hid, "static int mouse_vk_slot(", "static int bound_down(")]
        code = "\n".join(code).replace("GetModuleFileNameA(NULL,", "GetModuleFileNameA(0,")
        read = block(hid, "static void read_input(")
        for needle in ("bound_down(HOS_KEY_MOVE_UP,", "bound_down(HOS_KEY_CAM_RIGHT,", "mouse_buttons_arm(", "bound_down(i, 1)"):
            self.assertIn(needle, read, "read_input does not read the bound keys: " + needle)
        self.assertNotIn("key('W')", read, "read_input still reads WASD itself")
        with tempfile.TemporaryDirectory(prefix="hookshot-bound-keys-") as tmp:
            tmp = Path(tmp)
            (tmp / "hookshot_options.ini").write_text(
                "key_move_up=90,0\npad_move_up=0\npad_a=1,17\npad_b=0\npad_x=3,99\n", encoding="utf-8"
            )
            (tmp / "t.c").write_text(PRELUDE + code + MAIN, encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wno-unused-function", "-I", str(ROOT / "gpu" / "vk"),
                                    "-I", str(ROOT / "gpu"), str(tmp / "t.c"), "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary), str(tmp)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
