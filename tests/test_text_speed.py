"""The Insert menu's Text speed scales the letters' clock of a text pane, and only when the player asks for it.

nx2/oot_patches.py puts hos_text_scale at the head of the letters' block of f_80b1e140 (a LetterAnimControl's step): its time
argument in s0, multiplied. At Normal the lifted code runs as shipped; a game version whose code differs keeps its own code
and the build goes on with a warning.

python -I -B tests/test_text_speed.py [--cc gcc]
"""
import argparse
import contextlib
import io
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (the shape of the lifted function, build e0f3ccfa: the call that reaches the block, the block's head; the block's
# backward goto carries the interrupt poll (nx2/poll.py), and a lift by hand with nx2/lift.py --no-polls has it bare: both shapes are patched)
LIFTED = '''#include "a64.h"
void f_80b1e140(Ctx *A64_NOESC c);
void f_80b1e140(Ctx *__restrict A64_NOESC c) {
if (((c->x[9] >> 0) & 1)) { goto L_80b1e1b4; }
L_80b1e18c:;
return;
''' + oot_patches.TEXT_OLD_POLLED + '''if (((c->x[8] >> 4) & 1)) { goto L_80b1e460; }
}
void f_80b1e9f0(Ctx *__restrict A64_NOESC c) {
}
'''
LIFTED_NO_POLLS = LIFTED.replace(oot_patches.TEXT_OLD_POLLED, oot_patches.TEXT_OLD)


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    out = oot_patches.patch_text_speed(LIFTED)
    check(out is not None and out.count("*= hos_text_scale") == 1,
          "the scale is not in the letters' block once")
    check(out.index(oot_patches.TEXT_DECL) < out.index("void f_80b1e140"), "the lifted file does not declare hos_text_scale")
    check(out.index("*= hos_text_scale") > out.index("L_80b1e1b4:;"), "the scale is not inside the block")
    check("A64_POLL(c); goto L_80b1e18c;" in out, "the block's poll was lost")
    bare = oot_patches.patch_text_speed(LIFTED_NO_POLLS)
    check(bare is not None and bare.count("*= hos_text_scale") == 1 and "A64_POLL" not in bare,
          "a build without polls is not patched")
    check(oot_patches.patch_text_speed(out) == out and oot_patches.patch_text_speed(bare) == bare,
          "a second run changed a patched file (either shape)")
    check(oot_patches.patch_text_speed(LIFTED.replace("f_80b1e140", "f_80b1e150")) is None, "a file without the function changed")
    other = LIFTED.replace("+ 110);", "+ 112);")   # (another game version)
    check(oot_patches.patch_text_speed(other) == other, "a different function was patched")
    with tempfile.TemporaryDirectory(prefix="text-speed-") as tmp:
        (Path(tmp) / "code_000.c").write_text(other, encoding="utf-8")
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            oot_patches.apply(tmp)
        check("WARNING: the text speed block was found 0 times" in log.getvalue(), "no warning for a missing block")
        check((Path(tmp) / "code_000.c").read_text(encoding="utf-8") == other, "a missing block still changed the file")

    vi = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    check("id == 5 ? opts.text_speed" in option, "the host cannot see the menu's row (option 5)")
    check('"Text speed", &opts.text_speed, 0, 3, 1' in menu, "the Insert menu has no row for it")
    check("hos_text_scale = text_scale(" in block(vi, "static void realtime_step("), "the scale is not set each present")

    snippet = oot_patches.TEXT_NEW[len(oot_patches.TEXT_OLD):]
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
static uint64_t pane_seen;   /* (the text size hook: host/oot_text_size.c, given the LetterAnimControl) */
static void hos_oot_text_pane(Ctx *c, uint64_t anim) { (void)c; pane_seen = anim; }
static inline void a64_vset64(uint64_t *d, uint64_t v) { d[0] = v; d[1] = 0; }
volatile float hos_text_scale = 1.0f;
''' + block(vi, "static float text_scale(") + r'''
static float step(Ctx *c, float dt) {
    uint32_t u;
    memcpy(&u, &dt, 4);
    c->vreg[0][0] = 0xabcd000000000000ull | u, c->vreg[0][1] = 7;
    c->x[19] = 0x1234;
''' + snippet + r'''    float r;
    memcpy(&r, c->vreg[0], 4);
    return r;
}
int main(void) {
    Ctx c;
    CHECK(text_scale(NULL, 0) == 1.0f && text_scale(NULL, 1) == 2.0f && text_scale(NULL, 2) == 4.0f);
    CHECK(text_scale(NULL, 3) >= 1e5f);           /* Instant: a page in one step */
    CHECK(text_scale("0", 3) == 1.0f);            /* HOS_TEXT_SPEED wins over the menu */
    CHECK(text_scale("", 1) == 2.0f);
    hos_text_scale = text_scale(NULL, 0);
    CHECK(step(&c, 0.5f) == 0.5f && c.vreg[0][1] == 7 && c.vreg[0][0] >> 48 == 0xabcd);   /* Normal: the register untouched */
    hos_text_scale = text_scale(NULL, 2);
    CHECK(step(&c, 0.5f) == 2.0f && c.vreg[0][1] == 0);
    CHECK(pane_seen == 0x1234);   /* the text size hook gets x19 */
    printf("PASS: text speed only scales the letters' clock when asked\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="text-speed-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
