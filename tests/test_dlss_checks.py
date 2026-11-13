"""NVIDIA DLSS's small checks (gpu/vk/dlss_checks.h): which scene sizes DLSS takes for an output (the handheld profile's
512x288 at Ultra Performance too, which a fixed 640x360 floor turned away), which quality mode takes a render size (one a
pixel or two outside a mode's range, a rounded size, too), and Linux paths for NGX: which are ASCII (NGX's
Linux library takes only those; the others go to it as /proc/self/fd/<n>), and UTF-8 decoded (accents, CJK, bytes that are
not UTF-8). Built as C++ and run; also that vk_dlss.cpp and vk_draw.cpp use them. No game,
GPU or NVIDIA SDK required.

python -I -B tests/test_dlss_checks.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_sources():
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    # (the frame chain DLSS and FSR share: the scene camera and the actor motion)
    chain = (ROOT / "gpu" / "vk" / "vk_temporal.cpp").read_text(encoding="utf-8")
    # (no fixed floor on the scene's size: it follows the output)
    for name, src in (("vk_dlss.cpp", dlss), ("vk_draw.cpp", draw), ("vk_temporal.cpp", chain)):
        assert not re.search(r"\b(width|height)\s*<\s*(640|360)\b", src), name + ": a fixed scene size floor"
    assert "!temporal_scene_size(width, height)" in chain, "temporal_note_scene_draw: the output's sizes"
    assert "!temporal_scene_size(at.width, at.height)" in draw, "skin_check: the output's sizes"
    # (the Linux wide(): decoded, not each byte widened)
    widened = [l.strip() for l in dlss.splitlines() if "std::wstring(s.begin(), s.end())" in l]
    assert widened == ["if (dlss_checks::ascii(s)) return std::wstring(s.begin(), s.end());"], \
        "wide(): bytes widened one by one only for ASCII paths"
    assert "dlss_checks::utf8_to_wide(s, &bad)" in dlss, "wide(): UTF-8 decoded"
    # (NGX's Linux library converts its wide paths back in the "C" locale, ASCII only: a folder named otherwise goes to it
    # as /proc/self/fd/<n>)
    assert "dlss_checks::ascii(s)" in dlss and '"/proc/self/fd/"' in dlss, "wide(): non-ASCII folders by descriptor"
    # (prepare picks the mode with mode_for, and lets the old feature go only once a mode takes the size: a refused size keeps
    # it, rather than letting it go and making it again a frame later)
    prepare = dlss[dlss.index("int prepare(VkCommandBuffer cmd"):dlss.index("bool start()")]
    assert "dlss_checks::mode_for(rw, rh, ow, oh," in prepare, "prepare: the mode by dlss_checks::mode_for"
    assert prepare.index("dlss_checks::mode_for(") < prepare.index("ReleaseFeature(old)"), \
        "prepare: the old feature let go before a mode was found"
    # (a feature that was not made, out of video memory after quick DLSS model switches, should not be -1: if this
    # setting is on, it would turn DLSS off until a restart. It is the game's upscale until the preset, output or HDR
    # changes, then made again)
    assert '"its feature was not made", -1' not in prepare, "prepare: a failed creation turns DLSS off for the run"
    assert "s.refused = key;" in prepare and "if (s.refused == key)" in prepare, \
        "prepare: a failed creation tried again once a DLSS setting changes"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="dlss-checks-") as tmp:
        exe = Path(tmp) / "dlss-checks"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_dlss_checks.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
