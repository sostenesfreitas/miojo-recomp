"""A quality preset's texture budget gives way to RX_VK_VRAM_MB and to an integrated GPU's cap.

texture_frame (gpu/vk/vk_texture.cpp) used a preset's vram_mb in place of `own`, the budget that carries RX_VK_VRAM_MB
and the 2560 MB cap for integrated GPUs, so either was dropped whenever a preset was on. preset_texture_budget is
extracted unmodified and run on the cases below; no GPU or game is involved.

python -I -B tests/test_preset_texture_budget.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>

/* ACTUAL_HELPER */

static int fails;
static void check(const char* what, uint64_t got, uint64_t want_mb) {
    if (got != (want_mb << 20)) std::printf("FAIL %s: %llu MB, expected %llu\n", what, (unsigned long long)(got >> 20),
                                            (unsigned long long)want_mb), fails++;
}

int main() {
    const uint64_t MB = 1ull << 20;
    // (a 4 GB card on Minimum (4GB): the preset's 2200 MB, as before)
    check("preset on its card", preset_texture_budget(2200, true, false, false, 2357 * MB), 2200);
    // (RX_VK_VRAM_MB=1500 with a preset on: the variable's, which `own` holds)
    check("RX_VK_VRAM_MB wins", preset_texture_budget(2200, true, true, false, 1500 * MB), 1500);
    // (an integrated GPU on Balanced (8GB): at most the 2560 MB cap, not 4800)
    check("integrated GPU capped", preset_texture_budget(4800, true, false, true, 2560 * MB), 2560);
    // (an integrated GPU on Minimum: the preset's smaller budget stands)
    check("integrated GPU, smaller preset", preset_texture_budget(2200, true, false, true, 2560 * MB), 2200);
    // (Custom, or a preset for a smaller card than this one: the card's own)
    check("no preset", preset_texture_budget(0, true, false, false, 4300 * MB), 4300);
    check("preset not for this card", preset_texture_budget(2200, false, false, false, 5600 * MB), 5600);
    if (!fails) std::printf("preset texture budget: all cases pass\n");
    return fails != 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    helper = block(source, "uint64_t preset_texture_budget(")
    frame = block(source, "void texture_frame(uint64_t completed)")
    assert "preset_texture_budget(" in frame and "own);" in frame, "texture_frame: the budget does not go through the helper"
    with tempfile.TemporaryDirectory(prefix="hookshot-preset-texture-budget-") as temp:
        src, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        src.write_text(FIXTURE.replace("/* ACTUAL_HELPER */", helper), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
