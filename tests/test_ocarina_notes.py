"""The ocarina note layer (host/nn_hid.c): while the game's note wait is on (hos_oc_wait 1), a note row the player bound
drives hos_oc_note_bits and its keys and gamepad inputs are consumed, so they press nothing else (the Switch-key loop,
pads_read's pads, and the Move/Cam stick keys all let the consumed inputs through as nothing). A row left unset takes
nothing over: that Switch button keeps its own binding. The microphone wait (hos_oc_wait 2) and wait off
change nothing.
python -I -B tests/test_ocarina_notes.py   (needs a C compiler)
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
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_NUMPAD0 0x60
#define VK_NUMPAD2 0x62
#define VK_NUMPAD4 0x64
#define VK_NUMPAD6 0x66
#define VK_NUMPAD8 0x68
#define MAX_PATH 260
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
typedef int SRWLOCK;   /* (host/nn_hid.c guards the pad table with these since #1990) */
#define SRWLOCK_INIT 0
static void AcquireSRWLockExclusive(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockExclusive(SRWLOCK *p) { (void)p; }
static void AcquireSRWLockShared(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockShared(SRWLOCK *p) { (void)p; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG c) { LONG o = *p; if (o == c) *p = v; return o; }
static void Sleep(int ms) { (void)ms; }
static void hos_log(const char *f, ...) { (void)f; }
static int64_t oc_now = 0;
static int64_t hos_now_ns(void) { return oc_now; }
static const char *ini_dir;
#ifdef _WIN32
#define EXE_SEP "\\"
#else
#define EXE_SEP "/"
#endif
static DWORD GetModuleFileNameA(void *m, char *b, DWORD n) { (void)m; return (DWORD)snprintf(b, n, "%s" EXE_SEP "hookshot.exe", ini_dir); }
#define hos_game_path_a(b,n) GetModuleFileNameA(0,b,n)
static int held[256];
static int key(int vk) { return held[vk & 255]; }
static int oc_menu, oc_front = 1, oc_background;   /* (oc_pad_word shares read_input's gates: the Insert menu, the pads held
                                                      after it, the window, and the pads behind it: Controller in background) */
int rx_option(int id, int otherwise) { return id == 1 ? oc_menu : id == 4 ? oc_background : otherwise; }
static int game_in_front(void) { return oc_front; }
"""

MAIN = r"""
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void release(void) { memset(held, 0, sizeof held); }
/* what read_input()'s Switch-key loop does with bound_down, and pads_read's with the pad table */
static uint64_t switch_bits(void) {
    uint64_t b = 0;
    for (int i = 0; i < HOS_SW_N; i++) if (bound_down(i, 0)) b |= 1ULL << i;
    return b;
}
static uint64_t pad_bits(uint64_t raw) {
    uint64_t b = 0;
    for (int i = 0; i < HOS_SW_N; i++) {
        const int a = pad_bind[i][0], alt = pad_bind[i][1];
        if ((a >= 0 && (raw >> a & 1) && !(oc_consumed_pads >> a & 1)) ||
            (alt >= 0 && (raw >> alt & 1) && !(oc_consumed_pads >> alt & 1))) b |= 1ULL << i;
    }
    return b;
}
int main(int argc, char **argv) {
    (void)argc;
    ini_dir = argv[1];
    binds_load();
    /* (unset) rows the player never bound change nothing: the game's own note buttons play on */
    hos_oc_wait = 1, hos_oc_beat++;
    release(), held['Q'] = held['E'] = 1;   /* the l and a rows' own keys */
    oc_notes();
    CHECK(hos_oc_note_bits == 0);
    const uint64_t own = switch_bits();
    CHECK((own & (1ULL << HOS_SW_L)) && (own & (1ULL << HOS_SW_A)));
    CHECK(oc_pad_word(own) == own);   /* nothing stripped: the Xbox / PC layout's B still puts the ocarina away */
    hos_oc_wait = 0;
    /* the rest of this test is a player who bound the rows: only then does the layer take the buttons over */
    hos_bind(HOS_BIND_KEY, HOS_KEY_OC_NOTE_L, 0, VK_NUMPAD0);
    hos_bind(HOS_BIND_KEY, HOS_KEY_OC_NOTE_R, 0, VK_NUMPAD2);
    hos_bind(HOS_BIND_KEY, HOS_KEY_OC_NOTE_A, 0, VK_NUMPAD4);
    hos_bind(HOS_BIND_KEY, HOS_KEY_OC_NOTE_Y, 0, VK_NUMPAD6);
    hos_bind(HOS_BIND_KEY, HOS_KEY_OC_NOTE_X, 0, VK_NUMPAD8);
    hos_bind(HOS_BIND_PAD, HOS_KEY_OC_NOTE_L, 0, HOS_PAD_LSHOULDER);
    /* (a) NumPad 4 is the (A) note and also bound to D-pad down: during the wait it is only the note */
    hos_bind(HOS_BIND_KEY, HOS_SW_DOWN, 0, VK_NUMPAD4);   /* shared: both rows hold it */
    hos_oc_wait = 1;
    release(), held[VK_NUMPAD4] = held['2'] = 1;
    oc_notes();
    CHECK((hos_oc_note_bits & (1ULL << 2)) != 0);
    CHECK(!(switch_bits() & (1ULL << HOS_SW_DOWN)));            /* consumed as the note */
    CHECK((switch_bits() & (1ULL << HOS_SW_UP)) != 0);          /* the D-pad's own key still works */
    /* (b) a note key shared with a Move/Cam row: its stick contribution reads nothing during the wait */
    hos_bind(HOS_BIND_KEY, HOS_KEY_MOVE_LEFT, 0, VK_NUMPAD2);
    hos_oc_wait = 1;
    release(), held[VK_NUMPAD2] = 1;
    oc_notes();
    CHECK((hos_oc_note_bits & (1ULL << 1)) != 0);
    CHECK(!bound_down(HOS_KEY_MOVE_LEFT, 0));
    /* (c) the (L) note's pad input is the l row's (both HOS_PAD_LSHOULDER by default): during the wait the pad
       gives the note, not L, while l's own key 'Q' still gives L */
    hos_oc_wait = 1;
    release();
    pad_raw_now = 1 << HOS_PAD_LSHOULDER;
    oc_notes();
    CHECK((hos_oc_note_bits & 1ULL) != 0);
    CHECK((pad_bits(1ULL << HOS_PAD_LSHOULDER) & (1ULL << HOS_SW_L)) == 0);
    held['Q'] = 1;
    oc_notes();
    CHECK((switch_bits() & (1ULL << HOS_SW_L)) != 0);
    pad_raw_now = 0;
    /* (d) wait off: the note bits are clear and nothing is consumed */
    hos_oc_wait = 0;
    release(), held[VK_NUMPAD4] = 1;
    oc_notes();
    CHECK(hos_oc_note_bits == 0);
    CHECK((switch_bits() & (1ULL << HOS_SW_DOWN)) != 0);
    /* (e) the microphone wait: notes clear, nothing consumed, humming is the game's own path */
    hos_oc_wait = 2;
    oc_notes();
    CHECK(hos_oc_note_bits == 0);
    CHECK((switch_bits() & (1ULL << HOS_SW_DOWN)) != 0);
    /* (f) the word the game polls: during the button wait the five note buttons are the controls and nothing
       else; native presses of A/X/Y/L/R cannot reach the game's note code */
    hos_oc_wait = 1;
    release();
    held[VK_NUMPAD2] = held['Q'] = held['E'] = 1;   /* the (R) note; keys of the l and a rows */
    oc_notes();
    const uint64_t raww = switch_bits();
    CHECK((raww & (1ULL << HOS_SW_L)) && (raww & (1ULL << HOS_SW_A)));   /* the physical presses are real */
    const uint64_t w = oc_pad_word(raww);
    CHECK((w & (1ULL << HOS_SW_R)) && !(w & (1ULL << HOS_SW_L)) && !(w & (1ULL << HOS_SW_A)));
    CHECK((w & (1ULL << HOS_SW_X)) == 0 && (w & (1ULL << HOS_SW_Y)) == 0);
    /* (g) mic wait: the word is the player's own, untouched */
    hos_oc_wait = 2;
    oc_notes();
    CHECK(oc_pad_word(raww) == raww);
    /* (h) the lesson kind (2) teaches the game's own note buttons: they stay, the remaps go over them */
    hos_oc_wait = 1;
    oc_notes();
    hos_oc_last_kind = 2;
    CHECK((oc_pad_word(raww) & (1ULL << HOS_SW_R)) != 0 && (oc_pad_word(raww) & (1ULL << HOS_SW_L)) != 0);
    hos_oc_last_kind = 1;
    CHECK((oc_pad_word(raww) & (1ULL << HOS_SW_L)) == 0);
    /* (i) a mouse button is never a note: a note row does not accept one */
    hos_bind(HOS_BIND_KEY, HOS_KEY_OC_NOTE_A, 1, VK_LBUTTON);
    CHECK(hos_binding(HOS_BIND_KEY, HOS_KEY_OC_NOTE_A, 1) == 0);
    hos_bind(HOS_BIND_KEY, HOS_SW_Y, 0, VK_LBUTTON);   /* a normal row still takes the mouse button */
    CHECK(hos_binding(HOS_BIND_KEY, HOS_SW_Y, 0) == VK_LBUTTON);
    /* (j) the note window shares read_input's own gates: Insert menu open, pads held after it closed, or the game
       behind another window, and the word the game reads is the player's own */
    release(), held[VK_NUMPAD2] = held['Q'] = held['E'] = 1;
    hos_oc_wait = 1;
    oc_notes();
    const uint64_t raw2 = switch_bits();
    uint64_t w2 = oc_pad_word(raw2);
    CHECK((w2 & (1ULL << HOS_SW_R)) && !(w2 & (1ULL << HOS_SW_L)));
    oc_menu = 1;
    CHECK(oc_pad_word(raw2) == raw2);
    oc_menu = 0;
    pad_after_menu = 1;
    CHECK(oc_pad_word(raw2) == raw2);
    pad_after_menu = 0;
    oc_front = 0;
    CHECK(oc_pad_word(raw2) == raw2);
    oc_front = 1;
    CHECK((oc_pad_word(raw2) & (1ULL << HOS_SW_R)) && !(oc_pad_word(raw2) & (1ULL << HOS_SW_L)));
    /* (k) staleness: the wait only lives while the game's note action stamps its beat; a cancelled action (a pause
       that killed it without a not-running tick) must not hold the wait, or the shared keys stay eaten forever */
    hos_oc_beat++;
    oc_now = 1000000000LL;
    oc_notes();
    CHECK(hos_oc_wait == 1);
    CHECK((oc_pad_word(raw2) & (1ULL << HOS_SW_R)) && !(oc_pad_word(raw2) & (1ULL << HOS_SW_L)));
    oc_now = 1600000000LL;   /* 600 ms with no beat: the wait was left behind */
    oc_notes();
    CHECK(hos_oc_wait == 0);
    CHECK(oc_pad_word(raw2) == raw2);
    CHECK((switch_bits() & (1ULL << HOS_SW_L)) && (switch_bits() & (1ULL << HOS_SW_A)));   /* the eaten keys live again */
    hos_oc_wait = 1, hos_oc_beat++;       /* and a live action re-arms the guard */
    oc_notes();
    CHECK((oc_pad_word(raw2) & (1ULL << HOS_SW_R)) && !(oc_pad_word(raw2) & (1ULL << HOS_SW_L)));
    /* (l) the controller in the background (the menu's row, or HOS_INPUT_BACKGROUND=1): read_input reads the pads there,
       so a note on a pad button plays; the keys are not input there, so a note key does nothing */
    oc_front = 0, oc_background = 1;
    hos_oc_last_kind = 1;
    release();
    pad_raw_now = 1 << HOS_PAD_LSHOULDER;   /* the (L) note's pad input */
    oc_notes();
    CHECK((oc_pad_word(0) & (1ULL << HOS_SW_L)) != 0);
    pad_raw_now = 0;
    held[VK_NUMPAD2] = 1;   /* the (R) note's key, pressed in the window in front */
    oc_notes();
    CHECK((oc_pad_word(0) & (1ULL << HOS_SW_R)) == 0);
    release();
    oc_background = 0;   /* without the row: the word is the player's own, as before */
    pad_raw_now = 1 << HOS_PAD_LSHOULDER;
    oc_notes();
    CHECK(oc_pad_word(0) == 0);
    pad_raw_now = 0, oc_front = 1;
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


class OcarinaNotesTests(unittest.TestCase):
    def test_note_layer(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
        code = [block(hid, "static int kmap_down("),
                between(hid, "static const char *const bind_names[HOS_KEY_N]", "uint32_t hos_pad_raw("),
                between(hid, "static int mouse_vk_slot(", "static int bound_down("),
                block(hid, "static int pads_in_background("),
                block(hid, "static int pads_behind("),
                block(hid, "static void oc_notes(void)"),
                block(hid, "static uint64_t oc_pad_word(")]
        code = "\n".join(code)
        self.assertIn("oc_pad_word(buttons)", block(hid, "static void npad_states("),
                      "npad_states does not substitute the note word")
        self.assertIn("oc_notes();", block(hid, "static void read_input("), "read_input never runs oc_notes")
        self.assertIn("oc_consumed_pads", hid[hid.index("static uint64_t bound_pad_buttons("):hid.index("static volatile LONG pad_raw_now")],
                      "the pad read does not consult the consumed pad mask")
        menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
        for label in ("Ocarina note (L)", "Ocarina note (R)", "Ocarina note (A)", "Ocarina note (Y)", "Ocarina note (X)"):
            self.assertIn('"%s"' % label, menu, "the Controls page has no row for " + label)
        self.assertIn("controls[i].button >= HOS_KEY_MOVE_UP && controls[i].button <= HOS_KEY_CAM_RIGHT", menu,
                      "keys_only does not leave the note rows a gamepad column")
        self.assertIn("i >= HOS_KEY_OC_NOTE_L && i <= HOS_KEY_OC_NOTE_X", menu, "the ini save writes no pad_ line for a note row")
        with tempfile.TemporaryDirectory(prefix="hookshot-oc-notes-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.c").write_text(PRELUDE + code + MAIN, encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wno-unused-function", "-I", str(ROOT / "gpu" / "vk"),
                                    "-I", str(ROOT / "gpu"), str(tmp / "t.c"), "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary), str(tmp)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
