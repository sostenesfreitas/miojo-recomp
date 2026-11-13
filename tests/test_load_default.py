# SPDX-License-Identifier: GPL-3.0-or-later
"""When kept programs are loaded by default (gpu/vk/load_default.h): RX_VK_LOAD always wins; without it, at their create calls
only where the GPU has mesh shaders and no earlier session left the marker; the safety net's limits against the measured
sessions; the marker written and found. And the source: the choice is made once the device is up, a first use's time with a
load goes through the safety net, and a preparation removes the marker. Compiled and run alone: no game or GPU is needed.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    made = main.index("vkCreateDevice(dev.physical, &ci, nullptr, &dev.device)")
    if "programs_load_decide(dev.mesh_shader && !dev.mesh_fallback && !dev.task_mesh_fallback);" not in main[made:]:
        raise SystemExit("the load default is not decided after the device is made, from its mesh shaders")
    progs = (ROOT / "gpu/vk/vk_programs.cpp").read_text(encoding="utf-8")
    wait = progs[progs.index("void programs_load_wait(uint64_t key) {"):]
    wait = wait[:wait.index("\n}\n")]
    if "load_wait_note(ns);" not in wait:
        raise SystemExit("a first use's time with a load does not reach the safety net")
    # (the compressed program cache: the rule is told of it by the switch disk_init reads, and the two readers that take a
    # record by its parts' places step aside for it, so every read of a compressed record is program_cache_read's, unpacked)
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    if 'disk_zstd = env_on("RX_VK_PROGRAM_CACHE_ZSTD");' not in cache or 'env_on("RX_VK_PROGRAM_CACHE_ZSTD")' not in progs:
        raise SystemExit("the load default is not told of the compressed program cache by the switch that turns it on")
    for head, leave in (("std::unique_ptr<Program> program_cache_read_light(FILE* in, std::mutex* mu, uint64_t key, const DiskRecord& d) {",
                         "if (disk_zstd) return nullptr;"),
                        ("bool program_cache_variant(FILE* in, uint64_t key, const DiskRecord& d, uint64_t* variant) {",
                         "if (disk_zstd) return false;")):
        body = cache[cache.index(head):]
        if leave not in body[:body.index("fread(")]:
            raise SystemExit("a compressed record could be read by its parts' places: " + head.split("(")[0])
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    note = progs[progs.index("void programs_frames_note() {"):]
    note = note[:note.index("\n}\n")]
    if "programs_frames_note();" not in replay or "load_default::caches_lacked(lacked)" not in note or \
            "census.load_pipes_cold" not in note or "census.load_computes_cold" not in note:
        raise SystemExit("what the loads found in no cache does not reach the safety net")
    # (draws left out and pipeline waits are not this mode's alone: a state no list names is left out in start mode too)
    for other in ("pskip", "pwait", "left_out", "pending_skips"):
        if other in note:
            raise SystemExit("the safety net counts draws left out again, which a healthy session has in either mode: " + other)
    if "load_default::choose(getenv(\"RX_VK_LOAD\")" not in progs or "load_default::too_slow(" not in progs:
        raise SystemExit("vk_programs.cpp does not use load_default.h's rule")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    built = draw.index("remove(precompile_built_path().c_str());   // (notes are added here")
    if '"hookshot_load_start.txt").c_str());' not in draw[built:built + 400]:
        raise SystemExit("a preparation does not remove hookshot_load_start.txt")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="load-default-") as tmp:
        exe = Path(tmp) / "load-default"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        str(ROOT / "tests/load_default_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe), tmp], check=True, timeout=60)


if __name__ == "__main__":
    main()
