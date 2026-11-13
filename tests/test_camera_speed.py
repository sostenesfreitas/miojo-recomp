"""The Insert menu's Camera speed scales the right stick's camera turn speeds, and only when the player asks for it.

nx2/oot_patches.py makes each return of the turn-speed functions (f_80643f10, f_80643b60 and their level-2 pair f_82ea21f0,
f_82ea2220) multiply s0 by hos_camera_scale, which host/nn_vi.c sets from the row (HOS_OPT_CAMERA_SPEED) at each present.
At 100% the lifted code runs as shipped. The functions are found by name alone, so only the supported build's lift is
patched, and a lift patched again keeps one scale. The lifted text here is made up in the lift's shape, not the game's.

python -I -B tests/test_camera_speed.py [--cc gcc]
"""
import argparse
import contextlib
import io
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

# (two of the four in one file, with a return in a branch and a function after them that is none of them, nor is the one whose
# name only starts like one; the level-2 pair in another file, as chunks.py may cut them)
LIFTED = '''#include "a64.h"
void f_80643f10(Ctx *A64_NOESC c);
void f_80643f10(Ctx *__restrict A64_NOESC c) {
    c->x[8]=recomp_load32(c,c->x[0] + 596);
if ((uint32_t)c->x[8] == 0) { goto L_1; }
    a64_vset64(c->vreg[0],0x3f800000ULL);
    return;
L_1:;
    a64_vset64(c->vreg[0],0x40000000ULL);
    return;
}
void f_80643b60(Ctx *__restrict A64_NOESC c) {
if ((uint32_t)c->x[1] == 0) return;
    a64_vset64(c->vreg[0],0x3f000000ULL);
    return;
}
void f_80643f100(Ctx *__restrict A64_NOESC c) {
    return;
}
void f_806440a0(Ctx *__restrict A64_NOESC c) {
    return;
}
'''
LEVEL2 = '''#include "a64.h"
void f_82ea21f0(Ctx *__restrict A64_NOESC c) {
    a64_vset64(c->vreg[0],0x3f800000ULL);
    return;
}
void f_82ea2220(Ctx *__restrict A64_NOESC c) {
    a64_vset64(c->vreg[0],0x3f000000ULL);
    return;
}
'''


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(text, name):
    at = text.index("void %s(Ctx *__restrict" % name)
    end = text.find("\nvoid f_", at + 1)
    return text[at:end if end >= 0 else len(text)]


def patch_checks():
    ret = oot_patches.CAMERA_SPEED_RETURN
    out, n = oot_patches.patch_camera_speed(LIFTED)
    check(n == 2, "the two turn-speed functions of the file are not both patched (%d)" % n)
    check(function(out, "f_80643f10").count(ret) == 2 and function(out, "f_80643b60").count(ret) == 2,
          "a return of a turn-speed function does not scale s0")
    check("if ((uint32_t)c->x[1] == 0) " + ret in out, "a return in a branch is not scaled as one statement")
    check(ret not in function(out, "f_80643f100") and ret not in function(out, "f_806440a0"), "another function was patched")
    check(out.count(oot_patches.CAMERA_SPEED_DECL) == 1 and out.index(oot_patches.CAMERA_SPEED_DECL) < out.index("void f_"),
          "the lifted file does not declare hos_camera_scale once, before its functions")
    check(oot_patches.patch_camera_speed(out) == (out, 2), "a second run changed a patched file, or lost count of it")
    other = LIFTED.replace("f_80643f10(", "f_80643f20(").replace("f_80643b60(", "f_80643b70(")
    check(oot_patches.patch_camera_speed(other) == (other, 0), "a file without the functions changed")
    bare = '#include "a64.h"\nvoid f_80643f10(Ctx *__restrict A64_NOESC c) {\n    f_80643f20(c);\n}\n'
    check(oot_patches.patch_camera_speed(bare) == (bare, 0), "a function without a return was counted or changed")
    level2, n2 = oot_patches.patch_camera_speed(LEVEL2)
    check(n2 == 2 and level2.count(ret) == 2, "the level-2 pair is not patched")

    with tempfile.TemporaryDirectory(prefix="camera-speed-") as tmp:
        first, second = Path(tmp) / "code_000.c", Path(tmp) / "code_001.c"
        first.write_bytes(LIFTED.encode())   # (the lift's newlines, as it writes them: not translated on Windows)
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            oot_patches.apply(tmp)
        check(first.read_text(encoding="utf-8") == LIFTED, "a lift of another build was patched by the functions' names")
        check("camera" not in log.getvalue(), "a lift of another build was told about the camera's turn speed")
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            oot_patches.apply(tmp, protect_hearts=True)
        check(first.read_text(encoding="utf-8") == out, "the supported build's lift is not patched")
        check("WARNING: the camera's turn speed was found in 2 of 4 functions" in log.getvalue(),
              "no warning for the functions not found")
        second.write_bytes(LEVEL2.encode())
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            oot_patches.apply(tmp, protect_hearts=True)
        check(first.read_text(encoding="utf-8") == out and second.read_text(encoding="utf-8") == level2,
              "a lift patched again changed, or the level-2 pair is not patched")
        check("camera's turn speed was found" not in log.getvalue(), "a warning with all four functions found")


def source_checks():
    vi = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    check("hos_camera_scale = camera_scale(rx_option(HOS_OPT_CAMERA_SPEED, 100));" in block(vi, "static void realtime_step("),
          "the scale is not set from the menu's row at each present (100% without the renderer's menu)")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    value = {}   # (every HOS_OPT_* enum)
    for first, start, rest in re.findall(r"enum \{ (HOS_OPT_\w+) = (\d+)(?:, ([^}]*))? \}", api):
        names = [first] + [n.strip() for n in (rest or "").split(",") if n.strip()]
        value.update({n: int(start) + i for i, n in enumerate(names)})
    check("HOS_OPT_CAMERA_SPEED" in value, "vk_api.h has no HOS_OPT_CAMERA_SPEED")
    table = block(replay, "t.option = [](int id) -> int {")
    ids = [int(n) if n.isdigit() else value[n] for n in re.findall(r"\bid == (\w+)", table)]
    check(len(ids) == len(set(ids)), "vk_replay.cpp: an option id is read twice: %s" % sorted(ids))
    check("if (id == HOS_OPT_CAMERA_SPEED) return opts.camera_speed;" in table, "the host cannot see the menu's row")
    users = []
    for folder in ("host", "gpu"):
        for path in (ROOT / folder).rglob("*.c*"):
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"\brx_option(_set)?\(\s*(HOS_OPT_CAMERA_SPEED|%d)\s*," % value["HOS_OPT_CAMERA_SPEED"], text):
                users.append(path.relative_to(ROOT).as_posix())
    check(users == ["host/nn_vi.c"], "the Camera speed option is read or set outside host/nn_vi.c: %s" % users)

    header = (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    check("int camera_speed = 100;" in header, "out of the box the camera does not turn at the game's own speed")
    row = re.search(r'\{TAB_CONTROLS, "Camera speed", &opts\.camera_speed, 50, 400, 25, tx_percent,', menu)
    check(row is not None, "the Controls tab has no Camera speed row (50 to 400%, steps of 25)")
    check(block(menu, '{TAB_CONTROLS, "Camera speed"').rstrip().endswith("nullptr, false, true}"),
          "the Camera speed row takes a restart, or changes the Quality preset")
    for fn in ("bool is_slider(", "bool stops_at_ends("):
        line = menu[menu.index(fn):menu.index("\n", menu.index(fn))]
        check("&opts.camera_speed" in line, "%s does not take the Camera speed row: a scale" % fn.split("(")[0])
    # (its line in hookshot_options.ini: the name's, as ini_key makes it, and no other row's)
    keys = [name.lower().replace(" ", "_") for name in re.findall(r'\{TAB_[A-Z]+, "([^"]+)", &opts\.', menu)]
    ini_key = block(menu, "const char* ini_key(const Row& r) {")
    check("camera_speed" not in ini_key and keys.count("camera_speed") == 1, "the row is not saved as camera_speed")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| | Camera speed | 50% to 400%, in steps of 25 | 100% |" in readme, "readme.md's option table has no Camera speed row")


def fixture_checks(cc):
    vi = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    ret = oot_patches.CAMERA_SPEED_RETURN
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t vreg[32][2]; } Ctx;
static inline void a64_vset64(uint64_t *d, uint64_t v) { d[0] = v; d[1] = 0; }
volatile float hos_camera_scale = 1.0f;
''' + block(vi, "static float camera_scale(") + r'''
/* (a turn-speed function's return, as patched) */
static void speed_return(Ctx *__restrict c) ''' + ret + r'''
static float turn(Ctx *c, float speed) {
    uint32_t u;
    memcpy(&u, &speed, 4);
    c->vreg[0][0] = 0xabcd000000000000ull | u, c->vreg[0][1] = 7;
    speed_return(c);
    float r;
    memcpy(&r, c->vreg[0], 4);
    return r;
}
int main(void) {
    Ctx c;
    CHECK(camera_scale(100) == 1.0f && camera_scale(50) == 0.5f && camera_scale(250) == 2.5f && camera_scale(400) == 4.0f);
    CHECK(camera_scale(0) == 1.0f && camera_scale(49) == 1.0f && camera_scale(401) == 1.0f);   /* (not the row's: the game's) */
    hos_camera_scale = camera_scale(100);
    CHECK(turn(&c, 0.75f) == 0.75f && c.vreg[0][1] == 7 && c.vreg[0][0] >> 48 == 0xabcd);   /* 100%: the register untouched */
    hos_camera_scale = camera_scale(200);
    CHECK(turn(&c, 0.75f) == 1.5f && c.vreg[0][1] == 0 && c.vreg[0][0] >> 32 == 0);   /* (a write of s0 clears the rest of v0) */
    hos_camera_scale = camera_scale(50);
    CHECK(turn(&c, 0.75f) == 0.375f);
    printf("PASS: camera speed only scales the turn speeds when asked\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="camera-speed-") as tmp:
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
