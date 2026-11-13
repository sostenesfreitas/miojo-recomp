"""Compile and run the rule for how long an unused texture the GPU drew is kept (gpu/vk/keep_rendered.h; no game/GPU)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The eviction pass still takes its time from the rule, and the switch back is still read"""
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "keep_rendered.h"', "keep_rendered::presents(known, used, allowed, keep_over_only)",
                   'env_on("RX_VK_KEEP_RENDERED_OVER")', "frame_no - last < keep_rendered"):
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="keep-rendered-") as tmp:
        exe = Path(tmp) / "keep-rendered"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_keep_rendered.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
