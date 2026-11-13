"""With HDR set to Native and the PsychoV35 tone mapper the game's scene buffer is RGBA16F, not R11G11B10F; otherwise the game picks it as shipped.

nx2/oot_patches.py makes f_80672260's R11G11B10F choice (26) RGBA16F (44) while hos_scene_rgba16f is set, which host/nn_vi.c
sets from the tone mapper row (HOS_OPT_HDR_TONEMAP: Native HDR with PsychoV35) at each present. The function is found by name alone, so only the supported build's
lift is patched, and a lift patched again is patched once. The lifted text here is made up in the lift's shape, not the game's.

python -I -B tests/test_scene_format.py [--cc gcc]
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

PICK = '''    { uint64_t _r = c->x[9] & 0x8ULL; _r &= 0xFFFFFFFFULL; c->x[9] = _r; }
    c->x[10] = 0x2cULL;
    c->x[10] &= 0xFFFFFFFFULL;
    { uint64_t _r = (c->x[23] | ((uint64_t)(((uint32_t)(c->x[9])) >> 3))); _r &= 0xFFFFFFFFULL; c->x[9] = _r; }
    { uint64_t _a=c->x[9]; c->z=((uint32_t)_a==0); }
    c->x[9] = 0x1aULL;
    c->x[9] &= 0xFFFFFFFFULL;
    { uint64_t _r = (!c->z) ? c->x[10] : c->x[9]; _r &= 0xFFFFFFFFULL; c->x[9] = _r; }
'''
# (the same pick in a function before it, which is not the scene's)
LIFTED = ('#include "a64.h"\nvoid f_80672100(Ctx *__restrict A64_NOESC c) {\n' + PICK + '}\n'
          'void f_80672260(Ctx *__restrict A64_NOESC c) {\n' + PICK + '    return;\n}\n'
          'void f_806729d0(Ctx *__restrict A64_NOESC c) {\n' + PICK + '}\n')


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(text, name):
    at = text.index("void %s(Ctx *__restrict" % name)
    end = text.find("\nvoid f_", at + 1)
    return text[at:end if end >= 0 else len(text)]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def patch_checks():
    out = oot_patches.patch_scene_format(LIFTED)
    check(out is not None and out.count("hos_scene_rgba16f ?") == 1 and "hos_scene_rgba16f ?" in function(out, "f_80672260"),
          "the scene's pick is not switched, or another function's is")
    check(out.count(oot_patches.SCENE_FORMAT_DECL) == 1 and out.index(oot_patches.SCENE_FORMAT_DECL) < out.index("void f_"),
          "the lifted file does not declare hos_scene_rgba16f once, before its functions")
    check(oot_patches.patch_scene_format(out) == out, "a second run changed a patched file")
    check(oot_patches.patch_scene_format(LIFTED.replace("f_80672260(", "f_80672270(")) is None, "a file without the function")
    other = LIFTED.replace("c->x[9] = 0x1aULL;", "c->x[9] = 0x1bULL;")
    check(oot_patches.patch_scene_format(other) == other, "a function of other code (another game version) changed")

    with tempfile.TemporaryDirectory(prefix="scene-format-") as tmp:
        lifted = Path(tmp) / "code_000.c"
        lifted.write_bytes(LIFTED.encode())   # (the lift's newlines, as it writes them: not translated on Windows)
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            oot_patches.apply(tmp)
        check(lifted.read_text(encoding="utf-8") == LIFTED, "a lift of another build was patched by the function's name")
        for _ in range(2):
            log = io.StringIO()
            with contextlib.redirect_stdout(log):
                oot_patches.apply(tmp, protect_hearts=True)
            check(lifted.read_text(encoding="utf-8") == out, "the supported build's lift is not patched, or changed again")
            check("the scene format was found" not in log.getvalue(), "a warning with the function found")


def source_checks():
    vi = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    check("hos_scene_rgba16f = rx_option(HOS_OPT_HDR_TONEMAP, 0) != 0;" in block(vi, "static void realtime_step("),
          "the switch is not set from the tone mapper row at each present (off without the renderer's menu)")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    check("if (id == HOS_OPT_HDR_TONEMAP) return scene_tonemap_on();" in replay,
          "the renderer does not answer the tone mapper's option with scene_tonemap_on (Native HDR on Vanilla must keep R11G11B10F)")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    check("enum { HOS_OPT_HDR_TONEMAP = 47 };" in api, "HOS_OPT_HDR_TONEMAP is not option 47")
    check("volatile int hos_scene_rgba16f = 0;" in vi, "host/nn_vi.c does not define hos_scene_rgba16f, off at start")


def fixture_checks(cc):
    pick = function(oot_patches.patch_scene_format(LIFTED), "f_80672260")
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define A64_NOESC
typedef struct { uint64_t x[32]; int z; } Ctx;
volatile int hos_scene_rgba16f;
''' + pick.replace("void f_80672260(", "static void pick(") + r'''
static uint64_t format(int rgba16f, uint64_t flags, uint64_t x23) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[9] = flags, c.x[23] = x23;
    hos_scene_rgba16f = rgba16f;
    pick(&c);
    return c.x[9];
}
int main(void) {
    CHECK(format(0, 0, 0) == 26 && format(0, 8, 0) == 44 && format(0, 0, 1) == 44);   /* (as shipped) */
    CHECK(format(1, 0, 0) == 44 && format(1, 8, 0) == 44 && format(1, 0, 1) == 44);
    printf("PASS: the scene buffer is RGBA16F with the PsychoV35 tone mapper, the game's pick otherwise\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="scene-format-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    patch_checks()
    source_checks()
    fixture_checks(a.cc)


if __name__ == "__main__":
    main()
