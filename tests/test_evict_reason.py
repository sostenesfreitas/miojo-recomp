"""Compile and run why the eviction pass lets a texture go, and whether its texels go with it (gpu/vk/evict_reason.h; no game/GPU)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The eviction pass still counts its textures by reason and still logs the breakdown"""
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "evict_reason.h"', "evict_reason::classify(e.shifted, e.overwritten), lost, tex->bytes, tex->released)",
                   "evict_reason::texels_lost(tex->gpu_written, e.overwritten, e.flush && flushed_ok.count(tex))",
                   "evict_reason::describe(tally, why, sizeof why)", "flushed_ok.insert(f.t)"):
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="evict-reason-") as tmp:
        exe = Path(tmp) / "evict-reason"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_evict_reason.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
