"""Game counters that add the frame step, or the step times a whole number, cut to whole frames keep counting above 60 fps
(host/frame_whole.h, nx2/oot_patches.py patch_whole_frames).

The pick-up indicator above Link's head (f_80a87e50) moves the count it shows toward the wallet's by (int)(step * per) and fades
when they meet; the time of day (f_803edf70) adds (int)(2 * step) game seconds a frame. Under a Frame rate cap above 60 the step is
the frame's own time, under 1 (0.42 at 144 fps): cut to 0 every frame, the count never moved and the indicator stayed, and the
clock stood still. With the frames' own time
as the step (carry), hos_frame_whole adds the amount to the counter's own remainder and hands out its whole frames: 144 frames of
60/144 are 60; an amount of 1 or more keeps its fraction too; two counters keep their own remainders; NaN and amounts below zero
give 0. Without it, the amount is cut as the game cuts it (60 fps as before). The water renderer (f_800672e0) runs its ripple
simulation (int)step times a frame the same way, its remainder kept per water object: forty surfaces stepped in turn each still
count their 60 frames a second. The patch replaces the one cut of each site, once,
declares the hook, and leaves a file patched before and a function whose code differs as they are.

python -I -B tests/test_frame_whole.py
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

FIXTURE = r"""
#include "frame_whole.h"
#include <math.h>
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
int main(void) {
    static FrameWhole w;
    const uint64_t a = 0x1000, b = 0x2000;
    uint32_t n = 0;
    for (int i = 0; i < 144; i++) n += frame_whole(&w, a, bits(60.0f / 144.0f), 1);
    CHECK(n == 60 || n == 59);   /* (a second at 144 fps: 60 frames, give or take the float's last one) */
    n = 0;
    for (int i = 0; i < 1440; i++) n += frame_whole(&w, a, bits(60.0f / 144.0f), 1);
    CHECK(n >= 599 && n <= 601);
    n = 0;
    for (int i = 0; i < 30; i++) n += frame_whole(&w, b + 16, bits(2.0f), 1);
    CHECK(n == 60);   /* (30 fps: one second still advances sixty counter ticks) */
    CHECK(frame_whole(&w, b, bits(0.42f), 1) == 0 && frame_whole(&w, b, bits(0.42f), 1) == 0 && frame_whole(&w, b, bits(0.42f), 1) == 1);
    n = 0;
    for (int i = 0; i < 4; i++) n += frame_whole(&w, b + 8, bits(1.25f), 1);   /* (the step times 3: its fractions kept) */
    CHECK(n == 5);
    n = 0;
    for (int i = 0; i < 144; i++) n += frame_whole(&w, b + 16, bits(2.0f * 60.0f / 144.0f), 1);   /* (the time of day) */
    CHECK(n == 120 || n == 119);   /* (a second at 144 fps: the 120 game seconds of one at 60) */
    CHECK(frame_whole(&w, a, bits(0.42f), 0) == 0 && frame_whole(&w, a, bits(1.5f), 0) == 1 && frame_whole(&w, a, bits(2.0f), 0) == 2);
    CHECK(frame_whole(&w, a, bits(-1.0f), 1) == 0 && frame_whole(&w, a, bits(NAN), 1) == 0 && frame_whole(&w, a, bits(0.0f), 1) == 0);
    for (uint64_t k = 0; k < 100; k++) frame_whole(&w, 0x9000 + k, bits(0.5f), 1);   /* (more counters than slots: the oldest go) */
    CHECK(frame_whole(&w, 0x9000 + 99, bits(0.5f), 1) == 1);   /* (a slot still held: 0.5 + 0.5) */
    CHECK(frame_whole(&w, 0x9000, bits(0.5f), 1) == 0);   /* (the oldest's slot was reused: it starts again from 0) */
    /* (a scene's water surfaces, each stepped once a frame at 144 fps: 40 of them, more than the old 16 slots, each still counts
       its 60 frames a second) */
    {
        static FrameWhole water;
        uint32_t passes[40] = {0};
        for (int f = 0; f < 144; f++)
            for (int k = 0; k < 40; k++) passes[k] += frame_whole(&water, 0x7000000 + (uint64_t)k * 0x400, bits(60.0f / 144.0f), 1);
        for (int k = 0; k < 40; k++) CHECK(passes[k] == 60 || passes[k] == 59);
    }
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""

LIFTED = ['#include "a64.h"\n' + head + "    c->x[9] = recomp_load32(c,(c->x[19] + 864));\n" + old + "return;\n}\n"
          for head, old, _ in oot_patches.WHOLE_SITES]


class FrameWholeTests(unittest.TestCase):
    def test_counter_keeps_counting_under_one(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-frame-whole-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t.exe"
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-O1", "-I", str(ROOT / "host"), str(tmp / "t.c"),
                                    "-o", str(binary), "-lm"], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_patch_replaces_the_cut_once(self):
        for lifted, (_, old, key) in zip(LIFTED, oot_patches.WHOLE_SITES):
            out, n = oot_patches.patch_whole_frames(lifted)
            self.assertEqual(n, 1)
            self.assertEqual(out.count("=hos_frame_whole(%s, (uint32_t)c->vreg[0][0]);" % key), 1)
            self.assertNotIn("recomp_fp_to_int", out)
            self.assertIn(oot_patches.WHOLE_DECL, out)
            self.assertEqual(oot_patches.patch_whole_frames(out), (out, 0))   # (patched before: as it is)
            other = lifted.replace("32,32,", "32,31,")   # (another game version's code)
            self.assertEqual(oot_patches.patch_whole_frames(other), (other, 0))
            none = lifted.replace("void f_8", "void f_9")
            self.assertEqual(oot_patches.patch_whole_frames(none), (none, 0))
        # (a lift where the function is hot: its float helpers are a64_fpx_hot_*, the cut the same)
        _, old0, _ = oot_patches.WHOLE_SITES[0]
        hot = LIFTED[0].replace(old0, "    _v=a64_fpx_hot_mul32_fma32(c,_a,_b,_z);\n    a64_vset64(c->vreg[0],_v);}\n" + old0)
        self.assertEqual(oot_patches.patch_whole_frames(hot)[1], 1)

    def test_clock_patches_its_seconds_alone(self):
        # (f_803edf70 cuts into x8 the same way once more, in the branch that sets the time from another source: that cut
        #  stays the game's)
        i = next(i for i, (h, _, _) in enumerate(oot_patches.WHOLE_SITES) if "f_803edf70" in h)
        cut = "    c->x[8]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);\n"
        lifted = LIFTED[i].replace("return;\n", cut + "    recomp_store32(c,c->x[19] + 12,c->x[8]);\nreturn;\n")
        out, n = oot_patches.patch_whole_frames(lifted)
        self.assertEqual(n, 1)
        self.assertIn("=hos_frame_whole(c->x[19] + 60, (uint32_t)c->vreg[0][0]);", out)
        self.assertEqual(out.count(cut), 1)
        self.assertIn(cut + "    recomp_store32(c,c->x[19] + 12,", out)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
