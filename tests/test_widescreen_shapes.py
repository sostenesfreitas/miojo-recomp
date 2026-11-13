"""host/hos_main.c's widescreen sizes, game-free: the picture, scene, camera ratio and HUD for screens wider and
narrower than 16:9 (a 4:3 CRT, 16:10, 5:4 and narrower, 21:9, 32:9), and the game's own picture for 16:9.

    python -I -B tests/test_widescreen_shapes.py [--cc gcc]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def block():
    """The sizes, from the 16:9 constant to the else branch's log line, as hos_main.c has them"""
    src = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    start = src.index("        const uint32_t output_w = hos_output_size(0), output_h = hos_output_size(1);")
    end = src.index("\n", src.index("hos_log(\"boot: widescreen: a %.4f screen", start))
    return src[start:end] + "\n        }\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()

    fixture = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/hos_wide_size.h"

volatile uint64_t hos_wide_out, hos_wide_scene;
volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
float hos_aspect_ratio;
static unsigned out_w, out_h = 1440;
uint32_t hos_output_size(int height) { return height ? out_h : out_w; }
static void hos_log(const char *f, ...) { (void)f; }

static void wide(float want) {
''' + block() + r'''
}

int main(int argc, char **argv) {
    out_h = (unsigned)atoi(argv[2]);
    out_w = argc > 3 ? (unsigned)atoi(argv[3]) : out_h * 16 / 9;
    wide((float)atof(argv[1]));
    printf("%llux%llu %llux%llu %.4f %.4f %.4f\n",
           (unsigned long long)(hos_wide_out & 0xffffffffu), (unsigned long long)(hos_wide_out >> 32),
           (unsigned long long)(hos_wide_scene & 0xffffffffu), (unsigned long long)(hos_wide_scene >> 32),
           (double)hos_wide_ratio, (double)hos_wide_squeeze, (double)hos_wide_margin);
    return 0;
}
'''
    # (screen ratio, output height) -> picture, scene, camera ratio, HUD width, movie margin
    cases = (
        ("1.3333", 1440, "1920x1440 1440x1080 1.3333 1.0000 0.0000"),   # 4:3 (2048 x 1536, 1600 x 1200 CRTs)
        ("1.6", 1440, "2304x1440 1728x1080 1.6000 1.0000 0.0000"),     # 16:10
        ("1.25", 1440, "1800x1440 1352x1080 1.2500 1.0000 0.0000"),    # 5:4
        ("1.0", 1440, "0x0 0x0 0.0000 0.0000 0.0000"),                 # square: no ratio from the screen
        ("1.1", 1440, "1800x1440 1352x1080 1.2500 1.0000 0.0000"),     # narrower than 5:4: kept at 5:4
        ("1.3333", 2160, "2880x2160 2160x1620 1.3333 1.0000 0.0000"),  # 4:3 at the 4K output
        ("1.7778", 1440, "0x0 0x0 0.0000 0.0000 0.0000"),              # 16:9: the game's own picture
        ("2.3889", 1440, "3440x1440 2584x1080 2.3889 0.7442 0.1279"),  # 21:9 (3440 x 1440), as before
        ("3.5556", 1440, "5120x1440 3840x1080 3.5556 0.5000 0.2500"),  # 32:9, as before
        ("5.333333", 1080, "5760x1080 4320x810 5.3333 0.3333 0.3333"),
        ("5.333333", 1440, "7680x1440 5760x1080 5.3333 0.3333 0.3333"),   # 48:9 at 1440: 5120 x 2160's pixels, full size
        ("5.3333", 1440, "7680x1440 5760x1080 5.3333 0.3333 0.3333"),
        ("0", 1440, "0x0 0x0 0.0000 0.0000 0.0000"),                   # Ultrawide off
        # (at the 4K output: a 21:9 screen's native picture, 5120 x 2160, host/hos_wide_size.h's most pixels; a 32:9 one kept
        # to those pixels, its shape kept)
        ("2.3704", 2160, "5120x2160 3840x1620 2.3704 0.7500 0.1250"),  # 21:9 (5120 x 2160): native
        ("3.5556", 2160, "6256x1760 4696x1320 3.5545 0.5001 0.2499"),  # 32:9 (7680 x 2160): 5120 x 2160's pixels
        ("5.333333", 2160, "7680x1440 5760x1080 5.3333 0.3333 0.3333"),  # 48:9 (11520 x 2160): 5120 x 2160's pixels, exactly 48:9
    )
    with tempfile.TemporaryDirectory(prefix="widescreen-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.c", work / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-I", str(ROOT), str(source), "-o", str(exe), "-lm"], check=True)
        env = {k: v for k, v in os.environ.items() if k != "HOS_WIDE_MAX_PX"}
        bad = []
        for want, height, expect in cases:
            got = subprocess.run([str(exe), want, str(height)], capture_output=True, text=True, check=True, env=env).stdout.strip()
            if got != expect:
                bad.append("ratio %s at %u: %s, expected %s" % (want, height, got, expect))
        for width, height, expect in (
            (5760, 1080, "5760x1080 4320x810 5.3333 0.3333 0.3333"),
            (7680, 1440, "7680x1440 5760x1080 5.3333 0.3333 0.3333"),
        ):
            got = subprocess.check_output([str(exe), "1.7778", str(height), str(width)], text=True, env=env).strip()
            if got != expect:
                bad.append("explicit surround %ux%u: %s, expected %s" % (width, height, got, expect))
        # (HOS_WIDE_MAX_PX over the most pixels: 3840 x 2160's, the cap before, keeps 21:9 at the 4K output below native,
        # and 48:9 at 1440 at its aligned 6528 x 1224)
        old_cap = dict(env, HOS_WIDE_MAX_PX="8294400")
        for args_, expect in (
            (("2.3704", "2160"), "4416x1864 3312x1398 2.3691 0.7504 0.1248"),
            (("5.333333", "1440"), "6528x1224 4896x918 5.3333 0.3333 0.3333"),
            (("5.3333", "1440"), "6528x1224 4896x918 5.3333 0.3333 0.3333"),
            (("1.7778", "1440", "7680"), "6528x1224 4896x918 5.3333 0.3333 0.3333"),
        ):
            got = subprocess.check_output([str(exe), *args_], text=True, env=old_cap).strip()
            if got != expect:
                bad.append("HOS_WIDE_MAX_PX=8294400, %s: %s, expected %s" % (" ".join(args_), got, expect))
        if bad:
            raise SystemExit("FAIL:\n  " + "\n  ".join(bad))
    print("PASS: widescreen sizes for 4:3, 16:10, 5:4, 16:9, 21:9, 32:9 and 48:9 screens")


if __name__ == "__main__":
    main()
