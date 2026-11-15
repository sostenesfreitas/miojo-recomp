"""The Insert menu's Skip title screen row, and the host that carries it out.

The row (gpu/vk/vk_menu.cpp) is Off by default and saves as skip_title_screen; the renderer answers HOS_OPT_SKIP_TITLE
(gpu/vk/vk_api.h) with it. host/oot_autoload.c finds the newest progress.sav under user/save/<profile>/slot_* and taps A
through the title only while the option is on, a save exists, the Insert menu is closed, the player is not yet running,
and 120 s have not passed. The host is compiled and run against a temporary save tree whose slots have known mtimes, and
with fakes for the clock, the option, the player and the tap it sends.

    python -I -B tests/test_skip_title.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
MENU = ROOT / "gpu" / "vk" / "vk_menu.cpp"
HEADER = ROOT / "gpu" / "vk" / "vk_menu.h"
REPLAY = ROOT / "gpu" / "vk" / "vk_replay.cpp"
API = ROOT / "gpu" / "vk" / "vk_api.h"
AUTOLOAD = ROOT / "host" / "oot_autoload.c"

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "oot_autoload.h"
#include "vk_api.h"

static int64_t fake_now_ns;
static int option, menu, player, taps;
static int64_t tap_at[128];
const char *hos_user_dir = "";

int64_t hos_now_ns(void) { return fake_now_ns; }
int rx_option(int id, int otherwise) {
    if (id == HOS_OPT_SKIP_TITLE) return option;
    if (id == 1) return menu;
    return otherwise;
}
int hos_oot_player_seen(void) { return player; }
void hos_hid_tap(const char *presses) {
    (void)presses;
    if (taps < (int)(sizeof tap_at / sizeof tap_at[0])) tap_at[taps++] = fake_now_ns / 1000000;
}
void hos_log(const char *fmt, ...) { (void)fmt; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void at(int64_t ms) { fake_now_ns = ms * 1000000; }

int main(int argc, char **argv) {
    CHECK(argc == 3);
    if (argc != 3) return 2;

    /* the newest save, by progress.sav's mtime, not the slot's name */
    char path[260] = "";
    CHECK(hos_oot_newest_save(argv[1], path, sizeof path) > 0);
    CHECK(strstr(path, "slot_02") != NULL);
    CHECK(hos_oot_newest_save(argv[2], path, sizeof path) == 0);

    /* off: no press, now or later */
    hos_user_dir = argv[1], option = 0, menu = player = 0;
    hos_oot_autoload_reset(), taps = 0;
    at(0), hos_oot_autoload_input();
    at(30000), hos_oot_autoload_input();
    CHECK(taps == 0);

    /* on: the title gets A at 20 s, then every 2 s; the Insert menu pauses it */
    option = 1, hos_oot_autoload_reset(), taps = 0;
    at(0), hos_oot_autoload_input();
    at(19999), hos_oot_autoload_input();
    CHECK(taps == 0);
    at(20000), hos_oot_autoload_input();
    CHECK(taps == 1 && tap_at[0] == 20000);
    at(21999), hos_oot_autoload_input();
    CHECK(taps == 1);
    at(22000), hos_oot_autoload_input();
    CHECK(taps == 2 && tap_at[1] == 22000);
    menu = 1, at(24000), hos_oot_autoload_input();
    CHECK(taps == 2);
    menu = 0, at(24000), hos_oot_autoload_input();
    CHECK(taps == 3);

    /* the player runs: loaded, and nothing more is pressed */
    player = 1, at(26000), hos_oot_autoload_input();
    CHECK(taps == 3);
    at(28000), hos_oot_autoload_input();
    CHECK(taps == 3);

    /* turned off while waiting: it stops */
    option = 1, player = 0, hos_oot_autoload_reset(), taps = 0;
    at(0), hos_oot_autoload_input();
    at(20000), hos_oot_autoload_input();
    CHECK(taps == 1);
    option = 0, at(20001), hos_oot_autoload_input();
    at(22000), hos_oot_autoload_input();
    CHECK(taps == 1);

    /* no save yet: nothing is pressed, so a new game can still start */
    option = 1, hos_user_dir = argv[2], hos_oot_autoload_reset(), taps = 0;
    at(0), hos_oot_autoload_input();
    at(30000), hos_oot_autoload_input();
    CHECK(taps == 0);

    /* the give-up time */
    hos_user_dir = argv[1], option = 1, hos_oot_autoload_reset(), taps = 0;
    at(0), hos_oot_autoload_input();
    at(120000), hos_oot_autoload_input();
    CHECK(taps == 0);

    return failures ? 1 : 0;
}
"""


def check_menu():
    menu = MENU.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    api = API.read_text(encoding="utf-8")
    replay = REPLAY.read_text(encoding="utf-8")
    row = re.search(r'\{TAB_GAMEPLAY, "Skip title screen", &opts\.skip_title, 0, 1, 1, tx_onoff,', menu)
    if not row:
        raise SystemExit("vk_menu.cpp: no Gameplay row for the Skip title screen option")
    if "int skip_title = 0;" not in header:
        raise SystemExit("vk_menu.h: Options has no skip_title, Off by default")
    if "enum { HOS_OPT_HDR_NATIVE = 40, HOS_OPT_HDR_PEAK, HOS_OPT_HDR_WHITE, HOS_OPT_DISCORD, HOS_OPT_SKIP_TITLE }" not in api:
        raise SystemExit("vk_api.h: HOS_OPT_SKIP_TITLE is not the next option id")
    if "if (id == HOS_OPT_SKIP_TITLE) return opts.skip_title != 0;" not in replay:
        raise SystemExit("vk_replay.cpp: the renderer does not answer HOS_OPT_SKIP_TITLE")
    if menu.count("HOS_OPT_SKIP_TITLE") or header.count("HOS_OPT_SKIP_TITLE"):
        raise SystemExit("the menu should use opts.skip_title, not the host's option id")
    if "Skip title screen" not in (ROOT / "readme.md").read_text(encoding="utf-8"):
        raise SystemExit("readme.md: the menu's Skip title screen row is not documented")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    a = p.parse_args()
    check_menu()
    with tempfile.TemporaryDirectory(prefix="skip-title-") as tmp:
        root = Path(tmp).resolve()
        profile = root / "user" / "save" / ("0" * 32)
        for slot, age in (("slot_00", 30), ("slot_01", 60), ("slot_02", 10)):
            d = profile / slot
            d.mkdir(parents=True)
            (d / "progress.sav").write_bytes(b"save")
            when = time.time() - age
            os.utime(d / "progress.sav", (when, when))
        empty = root / "empty"
        (empty / "user" / "save" / ("0" * 32) / "slot_00").mkdir(parents=True)
        src = root / "skip_title_test.c"
        exe = root / "skip_title_test"
        src.write_text(HARNESS, encoding="utf-8")
        windows = os.name == "nt"
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if windows else 0
        extra = {"creationflags": flags} if windows else {}
        build = subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                                "-I", str(ROOT / "host"), "-I", str(ROOT / "gpu" / "vk"),
                                str(src), str(AUTOLOAD), "-o", str(exe)], **extra)
        if build.returncode:
            raise SystemExit("the host did not compile: see the compiler's output above")
        run = subprocess.run([str(exe), str(root / "user"), str(empty / "user")], capture_output=True, text=True, **extra)
        print(run.stdout, end="")
        print(run.stderr, end="")
        if run.returncode:
            raise SystemExit("the Skip title screen host check failed")
    print("PASS: Skip title screen is the menu's own option, and the host presses only when it should")


if __name__ == "__main__":
    main()
