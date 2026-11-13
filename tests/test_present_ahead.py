"""Frame generation 2x: a set's copies go to the GPU together, and only the presents wait for their times (#1702).

Each frame's copy into the window used to be submitted at its time, behind the game's next frame on the same queue, so the
game's own frames reached the screen ~3 ms after the made ones: 5.7 and 11 ms apart instead of 8.3, and a VRR display's
refresh changed every frame. Present ahead runs for a paced set of two or more, only where the swapchain can hold all of its
images acquired at once (an acquire past its spare images has to time out), and RX_VK_PRESENT_AHEAD=0 turns it off.

python -I -B tests/test_present_ahead.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def require(cond, message):
    if not cond:
        raise SystemExit(message)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    framegen = (VK / "vk_framegen.cpp").read_text(encoding="utf-8")
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    main_cpp = (VK / "vk_main.cpp").read_text(encoding="utf-8")

    display = block(framegen, "void display_main(")
    require('getenv("RX_VK_PRESENT_AHEAD")' in display, "present ahead has no RX_VK_PRESENT_AHEAD switch")
    require("present_ahead_wanted(" in display and "dev.swap_spare" in display,
            "present ahead is not decided by present_ahead_wanted with the swapchain's spare images")
    require("if (!present_commit(" in display, "the display thread no longer shows deferred presents, or counts one not shown")

    image = block(replay, "void present_image(VkImage src")
    submit, defer, present = image.find("vkQueueSubmit2("), image.find("if (present_defer_into)"), image.find("present_out(index, qk)")
    require(0 <= submit < defer < present, "present_image must submit the copy, then stop when deferred, before it presents")
    commit = block(replay, "bool present_commit(")
    require(0 <= commit.find("p.swapchain != dev.swapchain") < commit.find("present_out("),
            "present_commit must skip an image of a swapchain made again since its copy")
    spare = [line for line in main_cpp.splitlines() if "dev.swap_spare = " in line]
    require(any("caps.minImageCount" in line for line in spare),
            "the swapchain's spare images are not counted from the surface's minImageCount")
    # (tests/test_present_semaphores.py compiles the swapchain's making and checks the count, and that it is 0 without one)

    fn = block(framegen, "static bool present_ahead_wanted(")
    fixture = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
''' + fn + r'''
int main() {
    CHECK(present_ahead_wanted(true, true, 2, 2));     /* 2x, paced, a swapchain with two spare images (NVIDIA: 4 of min 2) */
    CHECK(present_ahead_wanted(true, true, 2, 3));
    CHECK(!present_ahead_wanted(false, true, 2, 2));   /* RX_VK_PRESENT_AHEAD=0 */
    CHECK(!present_ahead_wanted(true, false, 2, 2));   /* frame pacing off: frames go out as they come */
    CHECK(!present_ahead_wanted(true, true, 1, 2));    /* no made frame */
    CHECK(present_ahead_wanted(true, true, 3, 3));     /* 3x on a swapchain with three spare images (5 of min 2) */
    CHECK(present_ahead_wanted(true, true, 4, 4));     /* 4x with four */
    CHECK(!present_ahead_wanted(true, true, 3, 2));    /* 3x on a chain made for 2x (the menu changed after it): one at a time */
    CHECK(!present_ahead_wanted(true, true, 4, 3));
    CHECK(!present_ahead_wanted(true, true, 2, 1));    /* one spare image: a second acquire would have to time out */
    CHECK(!present_ahead_wanted(true, true, 2, 0));
    std::printf("PASS: present ahead for a paced set on a swapchain with a spare image for each of its frames\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="present-ahead-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
