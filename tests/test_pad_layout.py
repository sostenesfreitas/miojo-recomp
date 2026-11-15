"""The Controls page's Layout preset gives the four face buttons the preset's inputs, also after one was cleared.

gpu/vk/vk_menu.cpp apply_pad_layout binds A, B, X and Y's Input 1 one at a time through host/nn_hid.c hos_bind. The host
leaves an empty Input 1 as it is when the input asked for is the only one of another button, so with A cleared, Xbox / PC
asked for South while B still had it: B then moved to East and A stayed unbound, and the row said Xbox / PC. The preset
is now asked again while a face button is not the preset's, and the row takes its value from detect_pad_layout, so a
preset that cannot be had (South the only input of a button outside the four) shows Custom.
The menu's apply_pad_layout and detect_pad_layout are compiled with host/nn_hid.c's real binding table as the host.
python -I -B tests/test_pad_layout.py   (needs a C and a C++ compiler)
"""
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, start):
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated block at %d" % start)


def definition(text, name):
    match = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not match:
        raise SystemExit("missing definition: " + name)
    return block(text, match.start())


# host/nn_hid.c's binding table and hos_bind, alone (as tests/test_bound_keys.py compiles them); no options file
HID = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vk_api.h"
#include "hold_toggle.h"   /* (host/: the hold-to-toggle state the code cut from nn_hid.c keeps) */
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
static void hos_log(const char *f, ...) { (void)f; }
static DWORD hos_game_path_a(char *b, DWORD n) { (void)b, (void)n; return 0; }   /* (no options file: the defaults) */
/* TABLE */
"""

FIXTURE = r"""
#include <cstdio>
/* BINDING ENUMS */
extern "C" int hos_binding(int set, int button, int slot);
extern "C" void hos_bind(int set, int button, int slot, int code);
struct Host { int (*binding)(int, int, int); void (*bind)(int, int, int, int); } host_value{hos_binding, hos_bind}, *host = &host_value;
bool can_bind() { return true; }
struct { int pad_layout = 0; } opts;
/* APPLY */
/* DETECT */

static int fails;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static const int face[4] = {HOS_SW_A, HOS_SW_B, HOS_SW_X, HOS_SW_Y};
static const int want[3][4] = {{HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST},
                               {HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_WEST, HOS_PAD_NORTH},
                               {HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_NORTH, HOS_PAD_WEST}};
static void defaults() { hos_bind(HOS_BIND_PAD, -1, 0, 0); }

int main() {
    // From each preset, each face button's Input 1 cleared, then each preset chosen: the four are the preset's
    for (int from = 0; from < 3; from++)
        for (int cleared = 0; cleared < 4; cleared++)
            for (int layout = 0; layout < 3; layout++) {
                defaults();
                apply_pad_layout(from);
                hos_bind(HOS_BIND_PAD, face[cleared], 0, -1);
                CHECK(hos_binding(HOS_BIND_PAD, face[cleared], 0) == -1);
                apply_pad_layout(layout);
                for (int i = 0; i < 4; i++)
                    if (hos_binding(HOS_BIND_PAD, face[i], 0) != want[layout][i])
                        std::printf("from %d, %d cleared, preset %d: face button %d has %d\n", from, cleared, layout, i,
                                    hos_binding(HOS_BIND_PAD, face[i], 0)), fails++;
                CHECK(detect_pad_layout() == layout);
            }
    // South the only input of ZL and A cleared: Xbox / PC cannot give A South; the row says Custom
    defaults();
    hos_bind(HOS_BIND_PAD, HOS_SW_ZL, 0, HOS_PAD_SOUTH);
    hos_bind(HOS_BIND_PAD, HOS_SW_A, 0, -1);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_ZL, 0) == HOS_PAD_SOUTH && hos_binding(HOS_BIND_PAD, HOS_SW_ZL, 1) == -1);
    apply_pad_layout(1);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_ZL, 0) == HOS_PAD_SOUTH);   // (not taken from ZL)
    CHECK(detect_pad_layout() == 3);
    // (a preset over the defaults leaves the second inputs as they were)
    defaults();
    hos_bind(HOS_BIND_PAD, HOS_SW_A, 1, HOS_PAD_RPADDLE1);
    apply_pad_layout(1);
    CHECK(hos_binding(HOS_BIND_PAD, HOS_SW_A, 1) == HOS_PAD_RPADDLE1 && detect_pad_layout() == 1);
    if (!fails) std::puts("PASS");
    return fails != 0;
}
"""


def sources(root):
    menu = (root / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    api = (root / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    hid = (root / "host" / "nn_hid.c").read_text(encoding="utf-8")
    # (the controls' enums from gpu/vk/vk_api.h: the header itself is C, with C's _Static_assert)
    enums = [re.search(r"enum \{ " + first + r"\b.*?\};", api, re.S) for first in ("HOS_SW_A", "HOS_PAD_SOUTH", "HOS_BIND_PAD", "HOS_KEY_MOVE_UP")]
    if not all(enums):
        raise SystemExit("gpu/vk/vk_api.h: the controls' enums are missing")
    cxx = (FIXTURE.replace("/* BINDING ENUMS */", "\n".join(e.group(0) for e in enums))
           .replace("/* APPLY */", definition(menu, "void apply_pad_layout"))
           .replace("/* DETECT */", definition(menu, "int detect_pad_layout")))
    table_from = hid.rindex("\n", 0, hid.index("static const char *const bind_names[HOS_KEY_N]")) + 1
    table = hid[table_from:hid.index("\nvoid hos_bind(")] + "\n" + definition(hid, "void hos_bind")
    return HID.replace("/* TABLE */", table), cxx, menu


class PadLayoutTests(unittest.TestCase):
    def test_the_row_takes_what_the_preset_gave(self):
        menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
        branch = menu[menu.index("if (r.value == &opts.pad_layout) {", menu.index("void set_row(")):]
        branch = block(branch, 0)
        self.assertIn("apply_pad_layout(", branch)
        self.assertTrue(branch.rstrip("} \n").endswith("*r.value = detect_pad_layout();   // (Custom when the preset could not be had whole)"),
                        "set_row: the Layout preset row no longer takes its value from detect_pad_layout after the preset")

    def test_presets(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        cxx = shutil.which("g++") or shutil.which("clang++")
        if not cc or not cxx:
            self.skipTest("no C or C++ compiler")
        hid, fixture, _ = sources(ROOT)
        with tempfile.TemporaryDirectory(prefix="hookshot-pad-layout-") as tmp:
            tmp = Path(tmp)
            (tmp / "hid.c").write_text(hid, encoding="utf-8")
            (tmp / "menu.cpp").write_text(fixture, encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            build = subprocess.run([cc, "-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-I", str(ROOT / "gpu" / "vk"),
                                    "-I", str(ROOT / "gpu"), "-I", str(ROOT / "host"),
                                    "-c", str(tmp / "hid.c"), "-o", str(tmp / "hid.o")], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            build = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", str(tmp / "menu.cpp"), str(tmp / "hid.o"), "-o", str(binary)],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
