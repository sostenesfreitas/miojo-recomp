"""Above a 60 fps cap the sound job runs at 60 Hz and the physics solver keeps whole-frame steps (host/oot_high_fps.h,
nx2/oot_patches.py patch_sound_job and patch_solver_steps).

Sound: under a Frame rate cap above 60 the frame step is the frame's own time (0.5 at 120 fps). The sound manager's job (f_80435300)
fed steps under 1 left sounds unreleased at area changes until the game's voice budget was full and sounds, then the music,
stopped. The gate runs phase 0 once a whole step has built up (120 frames of 0.5: 60 runs; 144 of 60/144: 60), the other phases
follow phase 0's choice, the engine is handed a whole step then, and a step of 1 or more, 0 or below, NaN, or no frame time at
all (a cap of 60) runs every frame with the game's own step. A step of 1 or more keeps the remainder, so around 60 fps (0.98 and
1.02 alternating, 0.9 and 1.1, or mean 1 with jitter) the sound clock keeps the real one; a cap of 60 runs every phase, before
the first phase 0 too and after a phase 0 that held the job.

Solver: the game cuts frame step * n + 0.001 to its solver steps (n a 60 fps frame: 8 at 60 fps, 3 or 4 at 120 as the step
wanders; with n = 4, 0 above 240). With the frames' own time and a step under 1 it is rounded up (10% leeway for the frame time's
jitter) and at least 1, never under the game's; a step of 1 or more, or no frame time (a cap of 60), keeps the game's count.

The patches put the gate at the job's head and the step through the host, hook both solver places, declare the hooks, and leave
a file patched before, or whose code differs, as it is.

python -I -B tests/test_high_fps_fixes.py
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
#include "oot_high_fps.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint32_t bits(float f) { return oot_float_bits(f); }
static int runs(float step, int frames, int timed) {
    OotSoundGate g = OOT_SOUND_GATE_INIT;
    int n = 0;
    for (int i = 0; i < frames; i++) {
        const int run = oot_sound_gate(&g, 0, step, timed);
        CHECK(oot_sound_gate(&g, 1, step, timed) == run);   /* (phase 1 follows phase 0) */
        n += run;
    }
    return n;
}
/* (the sound engine's time over these frames' steps against the real time: what it is handed each frame the job runs) */
static double sound_clock(const float *steps, int n) {
    OotSoundGate g = OOT_SOUND_GATE_INIT;
    double real = 0, sound = 0;
    for (int i = 0; i < n; i++) {
        real += steps[i];
        if (oot_sound_gate(&g, 0, steps[i], 1)) sound += oot_bits_float(oot_sound_step(oot_float_bits(steps[i]), 1));
    }
    return sound / real;
}
int main(void) {
    CHECK(runs(0.5f, 120, 1) == 60);
    const int n144 = runs(60.0f / 144.0f, 144, 1);
    CHECK(n144 == 60 || n144 == 59);
    CHECK(runs(60.0f / 1000.0f, 1000, 1) >= 59 && runs(60.0f / 1000.0f, 1000, 1) <= 60);
    CHECK(runs(0.5f, 120, 0) == 120);   /* (a cap of 60: every frame) */
    CHECK(runs(1.0f, 60, 1) == 60 && runs(1.5f, 60, 1) == 60);
    CHECK(runs(0.0f, 60, 1) == 60 && runs(-1.0f, 60, 1) == 60 && runs(NAN, 60, 1) == 60);
    {   /* (a whole step after partial ones runs with the game's step and keeps the remainder) */
        OotSoundGate g = OOT_SOUND_GATE_INIT;
        CHECK(oot_sound_gate(&g, 0, 0.75f, 1) == 0 && oot_sound_gate(&g, 2, 0.75f, 1) == 0);
        CHECK(oot_sound_gate(&g, 0, 1.0f, 1) == 1 && g.part == 0.75f && oot_sound_gate(&g, 1, 1.0f, 1) == 1);
        CHECK(oot_sound_gate(&g, 0, 0.75f, 1) == 1 && g.part == 0.5f);
        CHECK(oot_sound_gate(&g, 0, 0.0f, 1) == 1 && g.part == 0.0f);   /* (a step of 0: as the game has it, the remainder cleared) */
    }
    {   /* (around 60 fps the sound clock keeps the real one: dropping the remainder at each whole step made it 0.51 of it) */
        static float steps[20000];
        for (int i = 0; i < 20000; i++) steps[i] = i & 1 ? 1.02f : 0.98f;
        double r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
        for (int i = 0; i < 20000; i++) steps[i] = i & 1 ? 1.1f : 0.9f;
        r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
        uint32_t x = 12345;   /* (mean 1, about 0.03 apart: a sum of uniforms) */
        for (int i = 0; i < 20000; i++) {
            float u = 0;
            for (int k = 0; k < 12; k++) x = x * 1664525u + 1013904223u, u += (float)(x >> 8) / 16777216.0f;
            steps[i] = 1.0f + (u - 6.0f) * 0.03f;
        }
        r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
        for (int i = 0; i < 20000; i++) steps[i] = 0.5f;
        r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
        /* 30 fps: every job phase runs, with two 60ths per frame rather than half-speed sound timers. */
        for (int i = 0; i < 20000; i++) steps[i] = 2.0f;
        r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001 && runs(2.0f, 30, 1) == 30);
    }
    {   /* (a cap of 60 runs every phase: before any phase 0, and after a phase 0 that held the job at a cap above 60) */
        OotSoundGate g = OOT_SOUND_GATE_INIT;
        CHECK(oot_sound_gate(&g, 1, 1.0f, 0) == 1 && oot_sound_gate(&g, 2, 1.0f, 0) == 1);
        OotSoundGate h = OOT_SOUND_GATE_INIT;
        CHECK(oot_sound_gate(&h, 1, 1.0f, 1) == 1);   /* (above 60, before the first phase 0: it runs) */
        CHECK(oot_sound_gate(&h, 0, 0.5f, 1) == 0 && oot_sound_gate(&h, 1, 0.5f, 0) == 1 && h.part == 0.0f);   /* (the cap changed) */
        CHECK(oot_sound_gate(&h, 2, 0.5f, 0) == 1 && oot_sound_gate(&h, 0, 0.5f, 0) == 1);
    }
    CHECK(oot_sound_step(bits(0.5f), 1) == bits(1.0f));
    CHECK(oot_sound_step(bits(0.5f), 0) == bits(0.5f));
    CHECK(oot_sound_step(bits(1.25f), 1) == bits(1.25f) && oot_sound_step(bits(0.0f), 1) == bits(0.0f));
    /* (the game's: (int)(step * n + 0.001), n the steps of a 60 fps frame) */
    #define GAME(step, n) ((int32_t)((step) * (n) + 0.001f))
    #define OURS(step, n, timed) oot_solver_steps(GAME(step, n), bits((step) * (n) + 0.001f), bits(n), timed)
    CHECK(OURS(1.0f, 4.0f, 1) == 4 && OURS(1.0f, 8.0f, 1) == 8 && OURS(1.0f, 8.0f, 0) == 8);
    CHECK(OURS(0.5f, 4.0f, 1) == 2 && OURS(0.49f, 4.0f, 1) == 2 && OURS(0.52f, 4.0f, 1) == 2 && GAME(0.49f, 4.0f) == 1);
    CHECK(OURS(0.49f, 8.0f, 1) == 4 && OURS(0.5f, 8.0f, 1) == 4 && GAME(0.49f, 8.0f) == 3);
    CHECK(OURS(0.25f, 4.0f, 1) == 1 && OURS(0.24f, 4.0f, 1) == 1 && OURS(0.06f, 4.0f, 1) == 1 && GAME(0.24f, 4.0f) == 0);
    CHECK(OURS(60.0f / 144.0f, 4.0f, 1) == 2 && OURS(60.0f / 90.0f, 4.0f, 1) == 3);
    CHECK(OURS(0.49f, 4.0f, 0) == 1 && OURS(0.06f, 4.0f, 0) == 0);   /* (a cap of 60: the game's count) */
    CHECK(OURS(1.6f, 4.0f, 1) == 6 && OURS(1.708f, 8.0f, 1) == 13);   /* (a slow frame: the game's count) */
    CHECK(OURS(2.0f, 8.0f, 1) == 16);   /* (30 fps: the game's whole-step solver count) */
    CHECK(oot_solver_steps(3, bits(NAN), bits(4.0f), 1) == 3 && oot_solver_steps(0, bits(-1.0f), bits(4.0f), 1) == 0);
    CHECK(oot_solver_steps(1, bits(2.0f), bits(0.0f), 1) == 1);   /* (no steps a frame: the game's) */
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""

LIFTED_SOUND = ('#include "a64.h"\n' + oot_patches.SOUND_HEAD + "    /* nop/hint */\nif ((uint32_t)c->x[2] != 0) { goto L_1; }\n"
                + oot_patches.SOUND_OLD + "L_1:;\n    return;\n}\nvoid f_80435360(Ctx *__restrict A64_NOESC c) {\n"
                + oot_patches.SOUND_OLD + "}\n")
LIFTED_SOLVER = '#include "a64.h"\n' + "".join(head + "    c->x[8]=1;\n" + oot_patches.SOLVER_OLD + "}\n"
                                               for head in oot_patches.SOLVER_HEADS)


class HighFpsTests(unittest.TestCase):
    def test_gate_and_steps(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-high-fps-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t.exe"
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-O1", "-I", str(ROOT / "host"), str(tmp / "t.c"),
                                    "-o", str(binary), "-lm"], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_sound_patch(self):
        out, n = oot_patches.patch_sound_job(LIFTED_SOUND)
        self.assertEqual(n, 1)
        head = out.index(oot_patches.SOUND_HEAD) + len(oot_patches.SOUND_HEAD)
        self.assertTrue(out[head:].startswith(oot_patches.SOUND_GATE))   # (before the job's own first instruction)
        self.assertEqual(out.count("hos_oot_sound_gate(c,"), 1)
        self.assertEqual(out.count(oot_patches.SOUND_NEW), 1)   # (the job's own load only, not the next function's)
        self.assertEqual(out.count(oot_patches.SOUND_OLD), 1)
        self.assertIn(oot_patches.SOUND_DECL, out)
        self.assertEqual(oot_patches.patch_sound_job(out), (out, 1))   # (patched before: as it is)
        other = LIFTED_SOUND.replace("+ 2192)", "+ 2200)")   # (another game version's code)
        self.assertEqual(oot_patches.patch_sound_job(other), (other, 0))
        none = LIFTED_SOUND.replace("void f_80435300", "void f_90435300")
        self.assertEqual(oot_patches.patch_sound_job(none), (none, 0))

    def test_solver_patch(self):
        out, n = oot_patches.patch_solver_steps(LIFTED_SOLVER)
        self.assertEqual(n, 2)
        self.assertEqual(out.count(oot_patches.SOLVER_NEW), 2)
        self.assertIn(oot_patches.SOLVER_DECL, out)
        hook = out.index("hos_oot_solver_steps((uint32_t)c->x[1]")
        self.assertLess(out.index("recomp_fp_to_int"), hook)
        self.assertLess(hook, out.index("recomp_store32(c,c->x[0] + 712"))
        self.assertEqual(oot_patches.patch_solver_steps(out), (out, 2))
        other = LIFTED_SOLVER.replace("+ 712", "+ 716")
        self.assertEqual(oot_patches.patch_solver_steps(other), (other, 0))


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
