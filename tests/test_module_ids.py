# SPDX-License-Identifier: GPL-3.0-or-later
"""The side file of shader module identifiers (gpu/vk/module_ids.h, used by the load at a program's create call): what is
learned is found again in a later run, a file of another program cache or another driver is not used, and one cut short keeps
what read. And the source: a load job asks for a compute pipeline by its module's identifier only without compiling, and goes
back to reading the code when the caches lack it; the dispatch needs no module of a program whose pipeline is there. Compiled
and run alone: no game or GPU is needed.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    by_id = block(draw, "VkPipeline compute_pipeline_by_module_id(")
    for needle in ("VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT", "ci.stage.module = VK_NULL_HANDLE",
                   "VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT", "cached_compute("):
        if needle not in by_id:
            raise SystemExit("compute_pipeline_by_module_id no longer has: " + needle)
    progs = (ROOT / "gpu/vk/vk_programs.cpp").read_text(encoding="utf-8")
    work = block(progs, "void load_work(LoadJob& job) {")
    ask = work.find("compute_pipeline_by_module_id(")
    if ask < 0 or not ask < work.index("program_cache_complete_unshared(", ask):
        raise SystemExit("load_work does not try a compute pipeline by its module identifier before it reads the code")
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    if "module_id_learned(" not in block(cache, "bool complete_from(Program& p, FILE* in, std::mutex* mu) {"):
        raise SystemExit("a compute module made from its code is not learned by its identifier")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="module-ids-") as tmp:
        exe = Path(tmp) / "module-ids"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        str(ROOT / "tests/module_ids_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe), tmp], check=True, timeout=60)


if __name__ == "__main__":
    main()
