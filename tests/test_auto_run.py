"""The Insert menu's Auto run (Controls): the sprint goes on after the run button (A) is let go.

host/auto_run.h keeps A held for the game after it was held for 0.4 s while the left stick was pushed and then let go,
until the stick is let go, A is pressed again, Plus or Minus is pressed, or Link is no longer the player's to move (an
event, the pause, the Insert menu, the ocarina's note wait). host/nn_hid.c runs it on each read of the controller with the row
(HOS_OPT_AUTO_RUN, answered by gpu/vk/vk_replay.cpp) and host/hos_main.c's hos_oot_player_free. This compiles the
header alone and plays presses through it, and checks that the row, its answer, the call and the readme are there.

python -I -B tests/test_auto_run.py [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def source_checks():
    menu = read("gpu/vk/vk_menu.cpp")
    check(re.search(r'\{TAB_CONTROLS, "Auto run", &opts.auto_run, 0, 1, 1, tx_auto_run,', menu),
          "the Insert menu has no Auto run row (Off, On) on the Controls tab")
    check('return v ? "On" : "Off";' in menu, "the Auto run row's names")
    check("int auto_run = 0;" in read("gpu/vk/vk_menu.h"), "Auto run is not off by default")
    check(re.search(r"if \(id == HOS_OPT_AUTO_RUN\) return opts.auto_run;", read("gpu/vk/vk_replay.cpp")),
          "HOS_OPT_AUTO_RUN does not answer the Auto run row")
    api = read("gpu/vk/vk_api.h")
    check("enum { HOS_OPT_AUTO_RUN = 55 };" in api, "vk_api.h has no HOS_OPT_AUTO_RUN")
    check(re.findall(r"HOS_OPT_\w+ = (\d+)", api).count("55") == 1, "option 55 is taken twice in vk_api.h")
    hid = read("host/nn_hid.c")
    check('#include "auto_run.h"' in hid, "nn_hid.c does not include auto_run.h")
    check(re.search(r"buttons = hos_auto_run\(&auto_run, buttons, stick\[0\], stick\[1\], on,\s*"
                    r"!menu_open && hos_oc_wait != 1 && hos_oot_player_free\(\),\s*hos_now_ns\(\)\)", hid),
          "nn_hid.c does not run Auto run with the row, the Insert menu, the ocarina's note wait and "
          "hos_oot_player_free")
    main = read("host/hos_main.c")
    check("int hos_oot_player_free(void)" in main, "hos_main.c has no hos_oot_player_free")
    check(re.search(r"__atomic_store_n\(&player_free_seen, oot_event_playing\(c\) \? 0 : hos_now_ns\(\)", main),
          "Link's update does not note whether he is the player's to move")
    readme = read("readme.md")
    check("| | Auto run | Off, On | Off |" in readme, "readme.md's option table has no Auto run row")
    check("- **Auto run:**" in readme, "readme.md does not say what Auto run does")


FIXTURE = r'''
#include <stdio.h>
#include <stdlib.h>
#include "auto_run.h"

#define A (1ULL << 0)
#define B (1ULL << 1)
#define PLUS (1ULL << 10)
#define MS 1000000LL
static int failed;
static void expect(int cond, const char *what) {
    if (!cond) printf("FAIL: %s\n", what), failed = 1;
}

/* one read: the player's buttons and stick at a time; whether the game sees A */
static int see(AutoRun *s, uint64_t buttons, int pushed, int on, int player_free, long long ms) {
    return (hos_auto_run(s, buttons, pushed ? 32767 : 0, 0, on, player_free, ms * MS) & A) != 0;
}

/* A held from 1000 to 1000 + `held` ms with the stick pushed, then let go */
static void sprint_for(AutoRun *s, long long held) {
    for (long long t = 0; t <= held; t += 16) see(s, A, 1, 1, 1, 1000 + t);
}

int main(void) {
    AutoRun s = {0};
    /* a sprint held long enough, let go with the stick pushed: A stays held */
    sprint_for(&s, 500);
    expect(see(&s, 0, 1, 1, 1, 1600), "A held 0.5 s and let go: the sprint did not go on");
    expect(see(&s, 0, 1, 1, 1, 9000), "the kept sprint ended on its own");
    /* the stick let go: it ends, and stays ended when pushed again */
    expect(!see(&s, 0, 0, 1, 1, 9016), "the stick let go: A still held");
    expect(!see(&s, 0, 1, 1, 1, 9032), "the stick pushed again after it ended: A held without a sprint");

    /* a short press (a roll on its own) is never kept */
    AutoRun t = {0};
    sprint_for(&t, 200);
    expect(!see(&t, 0, 1, 1, 1, 1300), "A held 0.2 s: kept");

    /* A pressed again while kept: the player's A from there; letting go of it stops */
    AutoRun u = {0};
    sprint_for(&u, 500);
    expect(see(&u, 0, 1, 1, 1, 1600), "not kept before the press");
    expect(see(&u, A, 1, 1, 1, 1700), "the press itself let go of A");
    expect(!see(&u, 0, 1, 1, 1, 1750), "A pressed again and let go: the sprint went on");
    for (long long t2 = 2000; t2 <= 2500; t2 += 16) see(&u, A, 1, 1, 1, t2);
    expect(see(&u, 0, 1, 1, 1, 2600), "a second long hold was not kept");

    /* Plus (the pause menu) while kept: A let go at once */
    AutoRun v = {0};
    sprint_for(&v, 500);
    expect(see(&v, 0, 1, 1, 1, 1600), "not kept before Plus");
    expect(!see(&v, PLUS, 1, 1, 1, 1616), "Plus while kept: A still held into the pause menu");

    /* an event, the pause, the Insert menu: not the player's to move; ended for good */
    AutoRun w = {0};
    sprint_for(&w, 500);
    expect(!see(&w, 0, 1, 1, 0, 1600), "Link not the player's to move: A still held");
    expect(!see(&w, 0, 1, 1, 1, 1616), "back to the player: kept again without a new sprint");

    /* the row off: the buttons pass as they were */
    AutoRun x = {0};
    for (long long t3 = 0; t3 <= 500; t3 += 16) see(&x, A, 1, 0, 1, 1000 + t3);
    expect(!see(&x, 0, 1, 0, 1, 1600), "the row off: kept");
    expect(hos_auto_run(&x, A | B, 32767, 0, 0, 1, 1700 * MS) == (A | B), "the row off: buttons changed");

    /* B (another button) while kept: the sprint goes on, and B passes */
    AutoRun y = {0};
    sprint_for(&y, 500);
    expect(hos_auto_run(&y, B, 32767, 0, 1, 1, 1600 * MS) == (A | B), "B while kept: the sprint ended or B was lost");

    /* a stick barely pushed (under 30%) counts as let go */
    AutoRun z = {0};
    sprint_for(&z, 500);
    expect(!(hos_auto_run(&z, 0, 6000, 6000, 1, 1, 1600 * MS) & A), "the stick at 26%: A still held");
    /* a diagonal past 30% keeps it */
    AutoRun dg = {0};
    sprint_for(&dg, 500);
    expect((hos_auto_run(&dg, 0, 9000, 9000, 1, 1, 1600 * MS) & A) != 0, "a diagonal at 39%: the sprint ended");

    /* A held while standing still and then moving: counted from when the stick moved */
    AutoRun st = {0};
    for (long long t4 = 0; t4 <= 1000; t4 += 16) see(&st, A, 0, 1, 1, t4);
    see(&st, A, 1, 1, 1, 1016);
    expect(!see(&st, 0, 1, 1, 1, 1100), "A held mostly standing still: kept after 84 ms of moving");

    /* a value from an older options file (2) counts as on */
    AutoRun o = {0};
    for (long long t5 = 0; t5 <= 500; t5 += 16) see(&o, A, 1, 2, 1, 1000 + t5);
    expect(see(&o, 0, 1, 2, 1, 1600), "row value 2 (an older file): not on");
    return failed;
}
'''


def fixture_checks(cc):
    with tempfile.TemporaryDirectory(prefix="auto-run-") as tmp:
        c, exe = Path(tmp) / "auto_run_test.c", Path(tmp) / "auto_run_test.exe"
        c.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(c), "-o", str(exe),
                        "-lm"], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        check(r.returncode == 0, "the stick's pushes (above)")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    source_checks()
    fixture_checks(a.cc)
    print("ok: Auto run")


if __name__ == "__main__":
    main()
