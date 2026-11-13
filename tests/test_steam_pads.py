"""Valve's own pads under Steam, for their motion sensors (host/steam_pads.h, used by host/pad_sdl.c; #1897).

Started from Steam, the game gets an SDL_GAMECONTROLLER_IGNORE_DEVICES list with Valve's controllers in it, so SDL
opens only Steam Input's stand-in pad, which has no gyroscope. The header is compiled and checked:

- Valve's entries (0x28de, either case) come out of the list and the others stay, in order, as Steam wrote them;
- the Steam Deck's built-in controls (0x28de/0x1205, either case) stay in the list, as Steam set them;
- a list with no Valve entry, an empty one and none at all come back with nothing taken out;
- a list SDL reads from a file (@path), and one too long for the buffer, are left as they are (-1, nothing written);
- a real Valve pad gives its motion only while a Steam stand-in pad is open; a stand-in pad, another maker's pad, or
  a Valve pad with no stand-in open plays as before.

python -I -B tests/test_steam_pads.py [--cc clang]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

# (a list as Steam sets it, shortened: Valve's pads, then a Flydigi and a Sony pad Steam also reads itself)
STEAM_LIST = "0x28de/0x1002,0x28de/0x1205,0x28de/0x1304,0x28DE/0x11E0,0x2dc8/0x6000,0x054c/0x0ce6"

FIXTURE = r'''#include <stdio.h>
#include <string.h>
#include "steam_pads.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    char out[256];
    /* Steam's list: three Valve entries out, the Steam Deck's and the rest kept in order */
    CHECK(steam_pads_without_valve("STEAM_LIST", out, sizeof out) == 3);
    CHECK(!strcmp(out, "0x28de/0x1205,0x2dc8/0x6000,0x054c/0x0ce6"));
    /* the Steam Deck's on its own, either case: kept */
    CHECK(steam_pads_without_valve("0x28DE/0x1205", out, sizeof out) == 0 && !strcmp(out, "0x28DE/0x1205"));
    /* only Valve's: an empty list */
    CHECK(steam_pads_without_valve("0x28de/0x1304", out, sizeof out) == 1 && !out[0]);
    /* none of Valve's: the same list back */
    CHECK(steam_pads_without_valve("0x2dc8/0x6000,0x054c/0x0ce6", out, sizeof out) == 0);
    CHECK(!strcmp(out, "0x2dc8/0x6000,0x054c/0x0ce6"));
    /* empty and missing */
    CHECK(steam_pads_without_valve("", out, sizeof out) == 0 && !out[0]);
    CHECK(steam_pads_without_valve(NULL, out, sizeof out) == 0 && !out[0]);
    /* empty entries and a trailing comma drop out */
    CHECK(steam_pads_without_valve("0x054c/0x0ce6,,0x28de/0x1304,", out, sizeof out) == 1);
    CHECK(!strcmp(out, "0x054c/0x0ce6"));
    /* a list from a file stays as it is */
    CHECK(steam_pads_without_valve(" @/tmp/list.txt", out, sizeof out) == -1 && !out[0]);
    /* too long for the buffer: left as it is */
    char small[16];
    CHECK(steam_pads_without_valve("0x28de/0x1304,0x2dc8/0x6000,0x054c/0x0ce6", small, sizeof small) == -1 && !small[0]);
    CHECK(steam_pads_without_valve("0x28de/0x1304", small, 0) == -1);
    /* motion only: a real Valve pad while a stand-in is open */
    CHECK(steam_pads_motion_only(0x28de, 0, 1) == 1);
    CHECK(steam_pads_motion_only(0x28de, 0, 0) == 0);                     /* no stand-in: plays as before */
    CHECK(steam_pads_motion_only(0x28de, 0xfa9962907718ULL, 1) == 0);    /* the stand-in itself */
    CHECK(steam_pads_motion_only(0x054c, 0, 1) == 0);                     /* another maker's pad */
    puts("ok: Steam's ignore list without Valve's pads, and their motion-only rule");
    return 0;
}
'''.replace("STEAM_LIST", STEAM_LIST)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d) / "steam_pads_test.c", Path(d) / "steam_pads_test"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(src), "-o",
                        str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip())
        if r.returncode:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
