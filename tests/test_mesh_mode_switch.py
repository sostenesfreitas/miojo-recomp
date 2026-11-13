# SPDX-License-Identifier: GPL-3.0-or-later
"""How task and mesh programs run is a launch switch, not a menu row (gpu/vk/mesh_mode.h, gpu/vk/vk_main.cpp).

The Graphics tab's Mesh shaders row is gone: changing it made the next start translate every shader again and drop the
pipeline caches. RX_VK_MESH_SHADERS=native or compute chooses (any other value, or none: Automatic), and
RX_VK_FORCE_MESH_FALLBACK=1 is Compute over it as before. An old mesh_shaders= line in hookshot_options.ini chooses nothing:
it is named in the log, and the menu's save drops it rather than keeping it. The rule is compiled and run alone; the
source checks read vk_main.cpp, vk_menu.cpp and vk_menu.h. No game or GPU is needed.

    python -I -B tests/test_mesh_mode_switch.py
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

RULE = r'''#include "mesh_mode.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    using mesh_rule::choose;
    // (the switch chooses; any other value, or none, is Automatic)
    CHECK(choose(nullptr, nullptr).mode == 0 && !strcmp(choose(nullptr, nullptr).why, "the default"));
    CHECK(choose("native", nullptr).mode == 1 && !strcmp(choose("native", nullptr).why, "RX_VK_MESH_SHADERS"));
    CHECK(choose("compute", nullptr).mode == 2 && !strcmp(choose("compute", nullptr).why, "RX_VK_MESH_SHADERS"));
    CHECK(choose("Native", nullptr).mode == 1 && choose("COMPUTE", nullptr).mode == 2);
    CHECK(choose("", nullptr).mode == 0 && !strcmp(choose("", nullptr).why, "the default"));
    for (const char* other : {"1", "2", "0", "automatic", "nativ", "compute2", " native", "fallback"}) {
        CHECK(choose(other, nullptr).mode == 0);
        CHECK(strstr(choose(other, nullptr).why, "RX_VK_MESH_SHADERS"));
    }
    // (RX_VK_FORCE_MESH_FALLBACK, set and not 0, is Compute over it; 0 leaves the switch to choose)
    CHECK(choose("native", "1").mode == 2 && !strcmp(choose("native", "1").why, "RX_VK_FORCE_MESH_FALLBACK"));
    CHECK(choose(nullptr, "1").mode == 2);
    CHECK(choose("native", "0").mode == 1 && choose(nullptr, "0").mode == 0);
    // (the old ini line: Native and Compute are told of, chosen by nothing; Automatic's 0 and other lines are not)
    int v = -1;
    CHECK(mesh_rule::old_ini_choice("mesh_shaders=1\n", &v) && v == 1);
    CHECK(mesh_rule::old_ini_choice("mesh_shaders=2\n", &v) && v == 2);
    CHECK(!mesh_rule::old_ini_choice("mesh_shaders=0\n", &v));
    CHECK(!mesh_rule::old_ini_choice("mesh_shaders=7\n", &v));
    CHECK(!mesh_rule::old_ini_choice("vram_budget=2\n", &v));
    puts("mesh mode: RX_VK_MESH_SHADERS chooses, RX_VK_FORCE_MESH_FALLBACK over it, the old ini line chooses nothing");
    return 0;
}
'''


def block(text, marker):
    start = text.index(marker)
    depth = 0
    for at in range(text.index("{", start), len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated: " + marker)


def check_source():
    vk = ROOT / "gpu" / "vk"
    main_cpp = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    device = block(main_cpp, "bool create_device() {")
    if 'mesh_rule::choose(getenv("RX_VK_MESH_SHADERS"), getenv("RX_VK_FORCE_MESH_FALLBACK"))' not in device:
        raise SystemExit("FAIL: create_device does not take the mode from RX_VK_MESH_SHADERS through mesh_mode.h's rule")
    # (the old line is only looked at to be named in the log: nothing read from the file reaches the choice)
    if re.search(r"atoi\(line \+ 13\)", device) or "mesh_rule::old_ini_choice(line, &old)" not in device:
        raise SystemExit("FAIL: create_device reads mesh_shaders= from hookshot_options.ini other than to name it in the log")
    if 'logf("vk: mesh shaders: %s, from %s: %s"' not in device or "mesh_choice.why" not in device:
        raise SystemExit("FAIL: the log line does not say where the mesh mode came from")
    # (the Linux AMD rule of Automatic is kept)
    if "if (mesh_mode_used == 0 && dev.props.vendorID == 0x1002) mesh_mode_used = 2;" not in device:
        raise SystemExit("FAIL: Automatic no longer picks Compute on Linux AMD")

    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    table = menu[menu.index("Row rows[] = {"):]
    table = table[:table.index("\n};")]
    if re.search(r'\{TAB_\w+,\s*"Mesh shaders"', table) or "opts.mesh_mode" in menu:
        raise SystemExit("FAIL: the menu still has a Mesh shaders row")
    if '"mesh_shaders"' in block(menu, "const char* ini_key("):
        raise SystemExit("FAIL: ini_key still names mesh_shaders: the game would write it")
    save = block(menu, "void save() {")
    kept = save[:save.index('fopen(part.c_str(), "w")')]
    if 'mine |= key == "mesh_shaders";' not in kept:
        raise SystemExit("FAIL: save() keeps an old mesh_shaders= line among the lines it does not write")
    if "mesh_mode" in (vk / "vk_menu.h").read_text(encoding="utf-8"):
        raise SystemExit("FAIL: the options still have a mesh_mode field")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="mesh-mode-") as tmp:
        src = Path(tmp) / "mesh_mode_test.cpp"
        src.write_text(RULE, encoding="utf-8")
        exe = Path(tmp) / "mesh-mode"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(src),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
    print("PASS: mesh mode is a launch switch; the old ini line chooses nothing and is not written")


if __name__ == "__main__":
    main()
