"""nx2/oot_patches.py makes the game's note-wait action (ExecuteWaitOcarinaInputCommon: start f_828a9190, tick
f_828a9280) report itself to the host (hos_oc_wait / hos_oc_last_kind in host/nn_hid.c, for the ocarina's five note
controls). The start decides from the input kind, every running tick re-arms (the input resets between note windows
keep the action running), and the not-running branch, the input-reset branch and the done store clear it. The tick's
three gates (the mode check, the input context, the wait's owner) also clear it: a tick that refuses, say a dialogue
took the buttons, must not leave the host swapping the pads. A file without the functions is left alone; an anchor
that drifted leaves the file wholly untouched (no half-patch, no declaration); a second run changes nothing.
python -I -B tests/test_ocarina_wait.py [--cc gcc]
"""
import argparse
import contextlib
import io
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

# (the shape of the two lifted functions, build e0f3ccfa: entry with the mode load, tick with its running gate, its
# three gates, the done store and the input-reset branch, kept in their real order inside the tick)
LIFTED = '''#include "a64.h"
void f_828a9190(Ctx *__restrict A64_NOESC c) {
''' + oot_patches.OC_ENTRY_OLD + '''
A64_IJUMP(c, c->x[2]); return;
}
void f_828a9280(Ctx *__restrict A64_NOESC c) {
''' + oot_patches.OC_TICK_OLD + '''
''' + oot_patches.OC_TICK_GATES[0][0] + '''
''' + oot_patches.OC_TICK_GATES[1][0] + '''
L_828a92f8:; A64_POLL(c);
''' + oot_patches.OC_TICK_GATES[2][0] + '''
L_828a937c:; A64_POLL(c);
''' + oot_patches.OC_DONE_OLD + '''
''' + oot_patches.OC_RESET_OLD + '''
L_828a92e4:; A64_POLL(c);
return;
}
void f_828a94e0(Ctx *__restrict A64_NOESC c) {
return;
}
'''
INJECTS = [("OC_ENTRY_NEW", oot_patches.OC_ENTRY_NEW), ("OC_TICK_NEW", oot_patches.OC_TICK_NEW),
           ("OC_DONE_NEW", oot_patches.OC_DONE_NEW), ("OC_RESET_NEW", oot_patches.OC_RESET_NEW)]
INJECTS += [("gate %d" % i, g[1]) for i, g in enumerate(oot_patches.OC_TICK_GATES)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    out = oot_patches.patch_ocarina_wait(LIFTED)
    for name, new in INJECTS:
        line = new.splitlines()[-1]
        check(out.count(line) == 1, name + " not injected exactly once")
    check(out.count("hos_oc_beat++") == 2, "the entry and the tick do not stamp the beat once each")
    check(out.count("HOS_OC_BUTTONS(") == 3, "the player's kinds are not one macro at both arming sites")
    check(out.count(oot_patches.OC_DECL) == 1, "the host's words are not declared once")
    check(out.index(oot_patches.OC_DECL) < out.index("void f_828a9190"), "the lifted file does not declare them at its head")
    gate = out.index("HOS_OC_BUTTONS(hos_oc_last_kind)")
    check(gate > out.index("if (!((c->x[8] >> 1) & 1))") and gate < out.index("L_828a937c:;"),
          "the tick's re-arm is not between its running gate and its body")
    check(oot_patches.patch_ocarina_wait(out) == out, "a second run changed the file")
    gone = oot_patches.patch_ocarina_wait(LIFTED.replace("f_828a9190", "f_828a91a0"))
    check(gone is None, "a file without the action's start changed")
    drift = LIFTED.replace("recomp_load8(c,c->x[0] + 24);", "recomp_load8(c,c->x[0] + 25);")
    same = oot_patches.patch_ocarina_wait(drift)
    check(same == drift, "a drifted tick anchor changed the file")
    check("the note wait began" not in same and oot_patches.OC_DECL not in same, "the drifted file was half-patched")

    with tempfile.TemporaryDirectory(prefix="oc-wait-") as tmp:
        (Path(tmp) / "code_000.c").write_text(drift, encoding="utf-8")
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            oot_patches.apply(tmp)
        check("WARNING: the ocarina note wait was found 0 times" in log.getvalue(), "no warning for the drifted anchors")
        check((Path(tmp) / "code_000.c").read_text(encoding="utf-8") == drift, "a missing anchor still changed the file")

    cc = shutil.which(a.cc)
    if cc:
        with tempfile.TemporaryDirectory(prefix="oc-wait-cc-") as tmp:
            src = out.replace('#include "a64.h"', '#include <stdint.h>\n'
                              'typedef struct { uint64_t x[32]; uint8_t z; } Ctx;\n'
                              '#define A64_NOESC\n#define A64_POLL(c) ((void)0)\n#define A64_IJUMP(c, t) ((void)(t))\n'
                              'static uint64_t recomp_load32(Ctx *c, uint64_t a) { (void)c; (void)a; return 3; }\n'
                              'static uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; (void)a; return 3; }\n'
                              'static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { (void)c; (void)a; (void)v; }\n'
                              'static void f_832b35d0(Ctx *c) { (void)c; }\n'
                              'static void f_806aac40(Ctx *c) { (void)c; }\n'
                              'static void f_82856760(Ctx *c) { (void)c; }\n'
                              'volatile int hos_oc_wait, hos_oc_last_kind;\nvolatile unsigned hos_oc_beat;')
            f = Path(tmp) / "t.c"
            f.write_text(src, encoding="utf-8")
            r = subprocess.run([cc, "-std=gnu11", "-Wall", "-fsyntax-only", str(f)], text=True, capture_output=True)
            check(r.returncode == 0, "the injected lines do not compile: " + r.stderr)
    else:
        print("no C compiler: the syntax check skipped")
    print("PASS: the note wait reports itself, re-arms, and only ever patches whole")


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


if __name__ == "__main__":
    main()
