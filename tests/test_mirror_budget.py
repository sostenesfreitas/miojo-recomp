"""Compile and run the VRAM mirrors' budget rules (gpu/vk/mirror_budget.h; no game/GPU required), and check that vk_memory.cpp's
mirror_budget still uses them: Auto's by the card, and a Quality preset's capped at Auto's, so Low's 1536 MB (every 6 GB card's from
its first start) does not reach a 4 GB card that preset_caps_memory lets Low's budgets through on.

python -I -B tests/test_mirror_budget.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    if marker not in text:
        raise SystemExit("gpu/vk/vk_memory.cpp no longer has: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    if '#include "mirror_budget.h"' not in mem:
        raise SystemExit("vk_memory.cpp no longer includes mirror_budget.h")
    budget = block(mem, "uint64_t mirror_budget() {")
    for needle in ['getenv("RX_VK_MIRROR_MB")',
                   "mirror_rule::auto_bytes(device_vram_bytes())",
                   "mirror_rule::budget_bytes((uint64_t)opts.mirror_mb << 20, opts.budget > 0, device_vram_bytes())"]:
        if needle not in budget:
            raise SystemExit("vk_memory.cpp: mirror_budget() no longer has: " + needle)
    if "vram / 8" in budget or "vram / 4" in budget:
        raise SystemExit("vk_memory.cpp: mirror_budget() works Auto's size out itself again, apart from mirror_budget.h")
    # (the preset's mirrors still reach mirror_budget only on a card the preset is for)
    if "preset_caps_memory()" not in budget:
        raise SystemExit("vk_memory.cpp: mirror_budget() no longer asks preset_caps_memory")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="mirror-budget-") as tmp:
        exe = Path(tmp) / "mirror-budget"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_mirror_budget.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
