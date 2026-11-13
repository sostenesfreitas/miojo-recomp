"""Compile and run the render-target overlap rule (vk_texture.cpp's dead transient targets; no game/GPU required)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The eviction pass still takes its time from the rule, and the switch back is still read"""
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "target_overlap.h"', "target_overlap::DrawnOver", "over.add(lo, hi, tex->last_frame",
                   "over.an_eighth(hi - lo)", 'getenv("RX_VK_DEAD_TARGETS")',
                   "const bool overwritten = shifted || drawn_over(tex);", "return !drawn_over(tex) && !drawn_over_shifted(tex);",
                   'getenv("RX_VK_DEAD_MIN_UNUSED")', "if (tex->last_value > completed)"):
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="target-overlap-") as tmp:
        exe = Path(tmp) / "target-overlap"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_target_overlap.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
