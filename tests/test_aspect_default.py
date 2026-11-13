"""The Aspect ratio's default for options saved before it (host/hos_wide_size.h hos_aspect_on), game-free.

An options file from before options_version 6 wrote ultrawide=0, the 16:9 that was the default then, whether chosen or not.
On a screen narrower than 16:9 (16:10, the Steam Deck's 1280x800, 4:3) that moves to the monitor's shape, where 16:9 only adds
bars; an ultrawide keeps its 16:9, and a file from version 6 on keeps whichever it says. No ultrawide= line is the monitor's
shape. host/hos_main.c starts the game with hos_aspect_on, and the Insert menu's row follows what the host started with
(gpu/vk/vk_menu.cpp menu_init), so the two agree on Linux too, where the menu has no screen size of its own. With
HOS_ASPECT set (a debugging switch host/hos_main.c takes instead of the file) the menu does not move the row: the shape
the host started with is the switch's, not the move's.

    python -I -B tests/test_aspect_default.py [--cc gcc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "host/hos_wide_size.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
    /* no ultrawide= line: the monitor's shape, on any screen */
    CHECK(hos_aspect_on(-1, 0, 1920, 1080) == 1 && hos_aspect_on(-1, 5, 3440, 1440) == 1 && hos_aspect_on(-1, 4, 0, 0) == 1);
    /* the monitor's shape saved: kept */
    CHECK(hos_aspect_on(1, 4, 1280, 800) == 1 && hos_aspect_on(1, 5, 3440, 1440) == 1);
    /* an older file's 16:9 (no options_version, or 2 to 5) */
    for (int v = 0; v <= 5; v++) {
        CHECK(hos_aspect_on(0, v, 1280, 800) == 1);    /* the Steam Deck, 16:10 */
        CHECK(hos_aspect_on(0, v, 1920, 1200) == 1);   /* 16:10 */
        CHECK(hos_aspect_on(0, v, 2560, 1600) == 1);   /* 16:10 */
        CHECK(hos_aspect_on(0, v, 1024, 768) == 1);    /* 4:3 */
        CHECK(hos_aspect_on(0, v, 1280, 1024) == 1);   /* 5:4 */
        CHECK(hos_aspect_on(0, v, 1920, 1080) == 0);   /* 16:9: nothing to move */
        CHECK(hos_aspect_on(0, v, 1366, 768) == 0);    /* 16:9 within 1%, as host/hos_main.c takes it */
        CHECK(hos_aspect_on(0, v, 3440, 1440) == 0);   /* an ultrawide keeps its 16:9 */
        CHECK(hos_aspect_on(0, v, 5120, 1440) == 0);
        CHECK(hos_aspect_on(0, v, 1080, 1920) == 0);   /* taller than wide: the game's own picture either way */
        CHECK(hos_aspect_on(0, v, 0, 0) == 0);         /* no screen size */
    }
    /* a file saved from version 6 on: its 16:9 was chosen */
    CHECK(hos_aspect_on(0, 6, 1280, 800) == 0 && hos_aspect_on(0, 7, 1024, 768) == 0);
    puts("PASS: an older file's 16:9 moves to the monitor's shape on screens narrower than 16:9 only");
    return 0;
}
'''


def need(text, pattern, why):
    if not re.search(pattern, text, re.S):
        raise SystemExit("FAIL: " + why)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="aspect-default-") as tmp:
        src, exe = Path(tmp) / "aspect.c", Path(tmp) / "aspect"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT), str(src), "-o", str(exe), "-lm"],
                       check=True)
        subprocess.run([str(exe)], check=True)

    host = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    need(host, r'if \(!strncmp\(line, "options_version=", 16\)\) version = atoi\(line \+ 16\);',
         "host/hos_main.c does not read options_version with ultrawide=")
    need(host, r"const int on = hos_aspect_on\(saved, version, sw, sh\);", "host/hos_main.c does not start with hos_aspect_on")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    init = menu[menu.index("void menu_init() {"):]
    init = init[:init.index("    restart_init();")]
    need(init, r"if \(version < 6 && opts\.ultrawide == 0 && !getenv\(\"HOS_ASPECT\"\) && host && host->aspect_ratio\) \{\s*"
               r"const float started = host->aspect_ratio\(\);\s*"
               r"if \(started > 1\.0f && started < 16\.0f / 9\.0f \* 0\.99f\) opts\.ultrawide = 1;",
         "menu_init does not follow the host's start for an older file's 16:9 (before restart_init), "
         "or follows it under HOS_ASPECT")
    saved = re.search(r'fprintf\(f, "options_version=(\d+)\\n"\);', menu[menu.index("void save() {"):])
    if not saved or int(saved.group(1)) < 6:
        raise SystemExit("FAIL: save() writes an options_version below 6: a 16:9 chosen now would move at the next start")
    print("aspect default: all checks passed")


if __name__ == "__main__":
    main()
