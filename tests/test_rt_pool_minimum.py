"""Compile the production render-target pool sizing function without game data.

The 305 MiB default crashed in Castle Town's guard house; 512 MiB resolved the
reported conversation/camera reproduction. Keep existing 4K/ultrawide growth
and explicit HOS_RT_POOL_MB overrides, including overrides below the floor; an output
with more pixels than 3840x2160 keeps its pool to 900 MiB.
"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="C compiler (otherwise clang, gcc or cc from PATH)")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_applet.c")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("null_pool", ROOT / "tests/test_nvn_null_pool.py")
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    body = helpers.block(args.source.read_text(), "uint64_t hos_rt_pool_size(uint64_t game)")
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIB (UINT64_C(1) << 20)
volatile uint64_t hos_wide_out;
static unsigned output_width, output_height;
static const char *override;
static uint32_t hos_output_size(int height) { return height ? output_height : output_width; }
static void hos_log(const char *fmt, ...) { (void)fmt; }
static const char *fixture_env(const char *name) {
    if (strcmp(name, "HOS_RT_POOL_MB")) exit(2);
    return override;
}
#define getenv fixture_env
''' + body + r'''
#define WIDE(w, h) ((uint64_t)(h) << 32 | (w))
static const struct {
    unsigned width, height;
    uint64_t wide, game_mib;
    const char *env;
    uint64_t expected_mib;
} cases[] = {
    {2560, 1440, 0, 305, NULL, 512},
    {3840, 2160, 0, 305, NULL, 687},
    {2560, 1440, WIDE(3440, 1440), 305, NULL, 512},
    {2560, 1440, WIDE(5120, 1440), 305, NULL, 610},
    {3840, 2160, WIDE(7680, 2160), 305, NULL, 1373},
    {3840, 2160, WIDE(5120, 2160), 305, NULL, 915},   /* (a 21:9 screen's native picture at the 4K output: host/hos_wide_size.h) */
    {3840, 2160, WIDE(7680, 1440), 305, NULL, 915},   /* (48:9 at the 4K output: kept to 7680 x 1440, as many pixels) */
    {2560, 1440, WIDE(1920, 1080), 305, NULL, 512},
    {2560, 1440, 0, 305, "256", 256},
    {3840, 2160, WIDE(7680, 2160), 305, "768", 768},
    {2560, 1440, 0, 305, "0x200", 512},
    {2560, 1440, 0, 305, "0", 0},
    {2560, 1440, 0, 768, NULL, 768},
    {3840, 2160, 0, 768, NULL, 1728},
    {5120, 2880, 0, 305, NULL, 900},
    {7680, 4320, 0, 305, NULL, 900},
    {5120, 2880, 0, 305, "1220", 1220},
};
int main(void) {
    int failed = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        output_width = cases[i].width; output_height = cases[i].height;
        hos_wide_out = cases[i].wide; override = cases[i].env;
        uint64_t got = hos_rt_pool_size(cases[i].game_mib * MIB);
        if (got != cases[i].expected_mib * MIB) {
            fprintf(stderr, "case %u: got %llu bytes, expected %llu MiB\n", i,
                    (unsigned long long)got, (unsigned long long)cases[i].expected_mib);
            failed = 1;
        }
    }
    return failed;
}
'''
    compiler = shutil.which(args.cc) if args.cc else next(
        (path for name in ("clang", "gcc", "cc") if (path := shutil.which(name))), None)
    if not compiler:
        raise SystemExit("No C compiler found (pass --cc)")
    with tempfile.TemporaryDirectory(prefix="rt-pool-minimum-") as directory:
        path = Path(directory)
        source, binary = path / "fixture.c", path / "fixture.exe"
        source.write_text(fixture)
        subprocess.run([compiler, "-std=c11", "-O2", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)
    print("PASS: 512 MiB floor, 4K/ultrawide growth and explicit overrides, 900 MiB above 4K's pixels (17 cases)")


if __name__ == "__main__":
    main()
