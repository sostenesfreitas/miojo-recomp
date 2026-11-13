"""The Insert menu's Aspect ratio and Output resolution rows take their sizes from the screen (gpu/vk/display_modes.h): a
screen's modes become the sizes the game can use, each named by its shape, and choosing a shape picks one of its sizes.
Compile the header with a small driver and check those rules, and that host/hos_main.c and the menu agree on which sizes
keep a shape of their own. No game or GPU is needed."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r"""
#ifndef _CRT_SECURE_NO_WARNINGS   // (the Windows CRT marks getenv and sscanf deprecated: an error under -Werror)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <cassert>
#include <cstdio>
#include <string>
#include "gpu/vk/display_modes.h"
using namespace mkwvk::display_modes;

static std::string sizes(const std::vector<Size>& v) {
    std::string s;
    for (Size x : v) s += (s.empty() ? "" : " ") + std::to_string(x.w) + "x" + std::to_string(x.h);
    return s;
}
static std::string names(const std::vector<Shape>& v) {
    std::string s;
    for (const Shape& x : v) s += (s.empty() ? "" : " ") + x.name;
    return s;
}

int main() {
    // (a 21:9 monitor's list as Windows gives it: every refresh rate again, a portrait mode, odd sizes, sizes too small)
    const std::vector<Size> modes = parse_list(
        "640x480,800x600,1024x768,1280x720,1280x1024,1366x768,1600x900,1680x1050,1920x1080,1920x1080,1920x1200,"
        "2560x1080,3440x1440,3440x1440,1080x1920,320x240,7680x4320,5120x1440;3840x1600 2560x1440,junk,720x576");
    assert(modes.size() == 21);   // ("junk" skipped)
    const std::vector<Size> v = offered(modes);
    // (each side down to 8: 1366x768 1360x768, 1600x900 1600x896; the portrait one narrower than 5:4, 320x240 too small,
    // 7680x4320 more than 5120x2880's pixels; each once, by width then height)
    const std::string got = sizes(v);
    std::printf("offered: %s\n", got.c_str());
    assert(got == "640x480 720x576 800x600 1024x768 1280x720 1280x1024 1360x768 1600x896 1680x1048 1920x1080 1920x1200 "
                  "2560x1080 2560x1440 3440x1440 3840x1600 5120x1440");
    // (the shapes as screens are sold; a little off after the rounding is still the shape)
    const struct { unsigned w, h; const char* name; } named[] = {
        {1920, 1080, "16:9"}, {1360, 768, "16:9"}, {1600, 896, "16:9"}, {2048, 1152, "16:9"}, {5120, 2880, "16:9"},
        {2560, 1080, "21:9"}, {3440, 1440, "21:9"}, {3840, 1600, "21:9"}, {5120, 2160, "21:9"},
        {1920, 1200, "16:10"}, {1680, 1048, "16:10"}, {2560, 1600, "16:10"}, {1280, 1024, "5:4"}, {720, 576, "5:4"},
        {1024, 768, "4:3"}, {1600, 1200, "4:3"}, {720, 480, "3:2"}, {1280, 768, "5:3"}, {5120, 1440, "32:9"},
        {3840, 1080, "32:9"}, {5760, 1080, "48:9"}, {2880, 1440, "2:1"}, {1600, 1024, "25:16"}, {1848, 1000, "1.85:1"}};
    for (const auto& n : named) {
        const std::string r = ratio_name({n.w, n.h});
        if (r != n.name) std::fprintf(stderr, "%ux%u: %s, not %s\n", n.w, n.h, r.c_str(), n.name);
        assert(r == n.name);
    }
    // (the Aspect ratio row: each shape once, narrowest first)
    const std::vector<Shape> s = shapes(v);
    const std::string sn = names(s);
    std::printf("shapes: %s\n", sn.c_str());
    assert(sn == "5:4 4:3 16:10 16:9 21:9 32:9");
    assert(shape_index(s, {3440, 1440}) == 4 && shape_index(s, {1920, 1080}) == 3 && shape_index(s, {720, 480}) == 0);
    // (a shape chosen: the screen's own size when it has that shape, else the largest of the shape)
    assert(v[pick(v, "21:9", {3440, 1440})] == (Size{3440, 1440}));
    assert(v[pick(v, "21:9", {1920, 1080})] == (Size{2560, 1080}));   // (the largest no taller than the screen)
    assert(v[pick(v, "21:9", {0, 0})] == (Size{3840, 1600}));   // (no screen known: the largest)
    assert(v[pick(v, "32:9", {1280, 720})] == (Size{5120, 1440}));   // (the only one)
    // (the height nearest the screen's, the lower of two as near: 3:2 on a 1080p screen is 1920x1280, not 720x480)
    const std::vector<Size> c = with_common(offered(parse_list("720x480,1280x720,1920x1080")));
    assert(c[pick(c, "3:2", {1920, 1080})] == (Size{1920, 1280}));
    assert(c[pick(c, "16:9", {1920, 1200})] == (Size{1920, 1080}));   // (a 16:10 screen: 1080 and 1440 are 120 and 240 off)
    assert(c[pick(c, "16:9", {2560, 1260})] == (Size{1920, 1080}));   // (180 off either way: the lower)
    assert(v[pick(v, "16:9", {3440, 1440})] == (Size{2560, 1440}));
    assert(v[pick(v, "4:3", {0, 0})] == (Size{1024, 768}));
    assert(pick(v, "3:2", {3440, 1440}) == -1);
    // (the common sizes of the shapes a screen has: a 1080p 16:9 screen with a 4:3 mode gets 1440p, 4K, 5K and 4:3's, no 21:9)
    const std::string common = sizes(with_common(offered(parse_list("1024x768,1280x720,1920x1080"))));
    std::printf("common: %s\n", common.c_str());
    assert(common == "1024x768 1280x720 1280x960 1440x1080 1600x1200 1920x1080 1920x1440 2048x1536 2560x1440 2880x2160 "
                     "3200x1800 3840x2160 5120x2880");
    for (Size c : common_sizes()) {   // (every common size one the game takes, and of a shape the row names)
        assert(hos_output_size_valid(c.w, c.h) && c.w % 8 == 0 && c.h % 8 == 0);
        assert(ratio_name(c).find('.') == std::string::npos);
    }
    const std::string wide = sizes(with_common(offered(parse_list("3440x1440"))));   // (an ultrawide: 21:9's)
    assert(wide == "2560x1080 3440x1440 3840x1600 5120x2160");
    // (no modes known: nothing offered, no shapes)
    assert(offered({}).empty() && shapes({}).empty() && parse_list(nullptr).empty() && parse_list("").empty());
    // (the host and the menu: a size more than 1 % off 16:9 keeps its own shape, a 16:9 one does not)
    assert(!hos_output_own_shape(1920, 1080) && !hos_output_own_shape(1360, 768) && !hos_output_own_shape(1600, 896));
    assert(!hos_output_own_shape(2048, 1152) && !hos_output_own_shape(0, 0));
    assert(hos_output_own_shape(2560, 1080) && hos_output_own_shape(1920, 1200) && hos_output_own_shape(1024, 768));
    assert(hos_output_own_shape(5120, 1440) && hos_output_own_shape(5760, 1080));
    // (the picture of a size of its own shape is that size: hos_main.c's hos_wide_size at the output's height and ratio)
    for (Size x : v) {
        if (!hos_output_own_shape(x.w, x.h)) continue;
        unsigned w = 0, h = 0;
        hos_wide_size(x.h, hos_wide_clamp((double)x.w / x.h), hos_wide_max_px(), &w, &h);
        if ((uint64_t)x.w * x.h <= 5120ull * 2160) {
            if (w != x.w || h != x.h) std::fprintf(stderr, "%ux%u: picture %ux%u\n", x.w, x.h, w, h);
            assert(w == x.w && h == x.h);
        }
    }
    std::puts("PASS: the screen's sizes, their shapes, a shape's size, and a size's own shape");
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="display-modes-") as tmp:
        src = Path(tmp) / "display_modes.cpp"
        exe = Path(tmp) / ("display_modes.exe" if os.name == "nt" else "display_modes")
        src.write_text(DRIVER, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(ROOT), str(src), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
