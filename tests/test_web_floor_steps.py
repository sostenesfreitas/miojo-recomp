"""The floor web keeps its 60-Hz simulation above a 60 frame-rate cap (#2667, #2271).

Compile the production patch applied to an authored update with its 0..3 clamp.
Without this site the same executable checks fail: quarter-frame updates never
run a step. Each floor needs its own carry, and the clamp and non-carry path stay
unchanged. A long pause cannot leave whole steps queued behind the clamp.
No game files or lifted function body are used.

python -I -B tests/test_web_floor_steps.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

CUT = "    c->x[22]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);\n"
LIFTED = ('#include "a64.h"\n'
          'void f_80652cb0(Ctx *__restrict A64_NOESC c) {\n' + CUT +
          '    const int32_t steps = (int32_t)c->x[22];\n'
          '    c->x[0] = (uint32_t)(steps < 0 ? 0 : steps > 3 ? 3 : steps);\n'
          '}\n'
          'void f_90000000(Ctx *__restrict A64_NOESC c) {\n' + CUT + '}\n')
CONTEXT = """#pragma once
#define A64_NOESC
typedef struct {
    uint64_t x[32], vreg[32][2];
    uint32_t fpcr, fpsr;
} Ctx;
"""
FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include "frame_whole.h"
#include <stdio.h>
#include "a64.h"

static FrameWhole fractions;
static int carry = 1;
static int failures;
#define CHECK(test, what) do { if (!(test)) { printf("FAIL: %s\n", what); failures++; } } while (0)

uint32_t hos_frame_whole(uint64_t at, uint32_t amount_bits) {
    return frame_whole(&fractions, at, amount_bits, carry);
}
uint64_t recomp_fp_to_int(uint64_t bits, int in_width, int out_width, int sign,
                         int mode, uint32_t fpcr, uint32_t *fpsr) {
    float amount;
    uint32_t low = (uint32_t)bits;
    memcpy(&amount, &low, sizeof amount);
    (void)in_width; (void)out_width; (void)sign; (void)mode; (void)fpcr; (void)fpsr;
    return (uint64_t)(int64_t)(int32_t)amount;
}
@UPDATE@

static uint32_t step(Ctx *ctx, uint64_t floor, float amount) {
    uint32_t bits;
    memcpy(&bits, &amount, sizeof bits);
    ctx->x[19] = floor;
    ctx->vreg[0][0] = bits;
    f_80652cb0(ctx);
    return (uint32_t)ctx->x[0];
}
static void reset(void) { memset(&fractions, 0, sizeof fractions); }

int main(void) {
    Ctx ctx = {0};
    const uint64_t a = 0x10000, b = 0x20000;
    uint32_t first = 0, second = 0;
    for (int frame = 0; frame < 240; frame++) {
        first += step(&ctx, a, 0.25f);
        second += step(&ctx, b, 0.25f);
    }
    CHECK(first == 60, "240-Hz first floor must run 60 steps, not zero or 240");
    CHECK(second == 60, "240-Hz second floor must keep its own fraction");

    reset();
    CHECK(step(&ctx, a, 0.25f) == 0, "quarter-frame sequence 1");
    CHECK(step(&ctx, a, 0.25f) == 0, "quarter-frame sequence 2");
    CHECK(step(&ctx, a, 0.25f) == 0, "quarter-frame sequence 3");
    CHECK(step(&ctx, a, 0.25f) == 1, "quarter-frame sequence 4 carries one step");
    reset(); first = 0;
    for (int frame = 0; frame < 120; frame++) first += step(&ctx, a, 0.5f);
    CHECK(first == 60, "120-Hz floor must run 60 steps");
    reset(); first = 0;
    for (int frame = 0; frame < 4; frame++) first += step(&ctx, a, 1.25f);
    CHECK(first == 5, "updates over one frame keep their fraction too");

    reset();
    CHECK(step(&ctx, a, 0.75f) == 0 && step(&ctx, b, 0.25f) == 0,
          "two floors must not share a remainder");
    CHECK(step(&ctx, a, 0.25f) == 1 && step(&ctx, b, 0.25f) == 0,
          "one floor advancing must not consume the other floor's fraction");
    CHECK(step(&ctx, b, 0.5f) == 1, "the other floor keeps its pending fraction");

    reset();
    CHECK(step(&ctx, a, 4.75f) == 3, "the existing upper clamp stays three");
    CHECK(step(&ctx, a, -1.0f) == 0, "the existing lower clamp stays zero");
    CHECK(step(&ctx, a, 0.0f) == 0, "zero time delivers no steps");
    reset();
    CHECK(step(&ctx, a, 0.5f) == 0 && step(&ctx, a, 600.25f) == 3,
          "a long pause is still clamped to three");
    CHECK(step(&ctx, a, 0.25f) == 1, "only the fractional pause remainder survives");
    for (int frame = 0; frame < 8; frame++)
        CHECK(step(&ctx, a, 0.25f) == (frame % 4 == 3 ? 1u : 0u),
              "a long pause leaves no whole-step backlog");
    carry = 0; reset(); first = 0;
    for (int frame = 0; frame < 60; frame++) first += step(&ctx, a, 1.0f);
    CHECK(first == 60, "the ordinary 60-Hz path is unchanged");
    CHECK(step(&ctx, a, 0.25f) == 0 && step(&ctx, a, 1.75f) == 1,
          "without carry the original truncation remains");
    if (!failures) puts("PASS: web floor steps");
    return failures != 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    patched, count = oot_patches.patch_whole_frames(LIFTED)
    with tempfile.TemporaryDirectory(prefix="web-floor-steps-") as directory:
        directory = Path(directory)
        (directory / "a64.h").write_text(CONTEXT, encoding="utf-8")
        source, binary = directory / "fixture.c", directory / "fixture.exe"
        command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"),
                   str(source), "-o", str(binary)]
        source.write_text(FIXTURE.replace("@UPDATE@", patched), encoding="utf-8")
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
        # The negative control must compile, then fail the same behavioral checks.
        source.write_text(FIXTURE.replace("@UPDATE@", LIFTED), encoding="utf-8")
        subprocess.run(command, check=True)
        native = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
        if native.returncode == 0 or "240-Hz first floor" not in native.stdout:
            raise SystemExit("FAIL: the native truncation did not fail the 240-Hz check")
        print("PASS: native truncation rejected by the same 240-Hz checks")
    assert count == 1 and patched.count("hos_frame_whole(c->x[19] + 2296,") == 1
    assert patched.count(CUT) == 1  # the authored unrelated function is left alone
    assert oot_patches.patch_whole_frames(patched) == (patched, 0)
    other = LIFTED.replace("c->x[22]=recomp_fp_to_int", "c->x[23]=recomp_fp_to_int")
    assert oot_patches.patch_whole_frames(other) == (other, 0)


if __name__ == "__main__":
    main()
