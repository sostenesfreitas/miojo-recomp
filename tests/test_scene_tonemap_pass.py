"""The scene tone map pass (gpu/vk/eden/shader_recompiler/ir_opt/scene_tonemap_pass.h) on synthetic IR: the game's scene
mapping shape takes PsychoV35 with each channel where its luminance weight says, the exposure and the peak its own constant
buffer words; the shapes it must not take are left as they are (tests/scene_tonemap_ir.cpp). And the wiring: the program's
key and translation agree on it, the picks are dropped when the row changes, the row shows with Native HDR only.

python -I -B tests/test_scene_tonemap_pass.py [--cxx clang++]
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import concurrent.futures
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
ENV_ON_H = str(VK / "compat" / "hookshot_env.h")


def build_and_run(cxx):
    sources = sorted((VK / "eden" / "shader_recompiler" / "frontend" / "ir").glob("*.cpp"))
    flags = ["-std=c++20", "-include", ENV_ON_H, "-O1", "-D_CRT_SECURE_NO_WARNINGS", "-DMKWVK_INCLUDES=2"]
    for header in ("algorithm", "cstdio", "cstdlib", "cstring", "functional", "string", "vector"):
        flags += ["-include", header]
    flags += ["-I" + str(p) for p in (VK / "compat", ROOT / "gpu" / "compat", VK / "eden", ROOT / "gpu" / "third_party" / "suyu")]
    with tempfile.TemporaryDirectory(prefix="scene-tonemap-") as temp:
        folder = Path(temp)
        sources = [ROOT / "tests" / "scene_tonemap_ir.cpp", *sources]
        objs = [folder / ("%03d.o" % i) for i in range(len(sources))]
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
            list(pool.map(lambda job: subprocess.run([cxx, *flags, "-c", str(job[0]), "-o", str(job[1])], check=True),
                          zip(sources, objs)))
        exe = folder / "fixture.exe"
        subprocess.run([cxx, *map(str, objs), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout, end="")
        print(run.stderr, end="")
        if run.returncode != 0 or "PASS" not in run.stdout:
            raise SystemExit("FAIL: the scene tone map pass on synthetic IR")


def wiring():
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    fails = []
    # (a program's key says whether it has PsychoV35, and the program is translated so: each place that makes one decides once)
    if "if (tonemap) key = hash_bytes(\"psychov35\", 9, key);" not in shader:
        fails.append("graphics_key does not keep PsychoV35's programs apart")
    for made in re.finditer(r"graphics_key\(([^;]*)\);", shader):
        args = made.group(1)
        if "const Code& vs" in args:
            continue
        if not re.search(r"(tonemap|scene_tonemap)\s*\)?$", args.strip()) and "fs_head" not in args:
            fails.append("graphics_key(%s) without the tone mapper" % args)
    if shader.count("p->scene_tonemap = ") < 2:
        fails.append("a program made without its scene_tonemap")
    if "if (p.scene_tonemap) {" not in shader or "SceneToneMapPass(fs->program)" not in shader:
        fails.append("translate_into does not run the pass for p.scene_tonemap")
    guard = shader.find("Shader::Optimization::GuardFragmentOutputs(fs->program")
    if not (0 <= shader.find("SceneToneMapPass(fs->program)") < guard):
        fails.append("the pass after the NaN guard: its output unguarded")
    if "picks.clear();\n            prog_fast[0].on = prog_fast[1].on = false;" not in shader:
        fails.append("the picks not dropped when the tone mapper changes")
    if "bool scene_tonemap_on() { return dev.hdr && opts.hdr == 2 && opts.hdr_tonemap == 1; }" not in menu:
        fails.append("scene_tonemap_on is not Native HDR with the row on")
    if not re.search(r'\{TAB_GRAPHICS, "HDR tone mapper", &opts\.hdr_tonemap, 0, 1, 1, tx_tonemap,', menu):
        fails.append("the HDR tone mapper row")
    if "(v != &opts.hdr_tonemap || opts.hdr == 2)" not in menu:
        fails.append("the tone mapper row shown without Native HDR")
    if "int hdr_tonemap = 0;" not in (VK / "vk_menu.h").read_text(encoding="utf-8"):
        fails.append("the tone mapper not Vanilla out of the box")
    if fails:
        raise SystemExit("FAIL: " + "; ".join(fails))
    print("wiring: the key, the translation, the picks and the row agree")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    wiring()
    build_and_run(a.cxx)
    print("PASS: the scene tone map pass")


if __name__ == "__main__":
    main()
