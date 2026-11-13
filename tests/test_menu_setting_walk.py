"""The control bridge's option_set reaches the choice it was asked for on the rows whose choices are not in number order.

menu_setting_set used to step the Upscaler, Render resolution and Output resolution rows towards the number asked for and
stop at the first step past it. Their numbers are not in the arrows' order: the output sizes go by width (1920x1080 is
choice 3, after 2048x1152's 0), the Render resolution's slider by size, and from DLSS Auto the first step right is Native,
which keeps the value. So 1920x1080 from 2048x1152 saved 5120x1440, and any Render resolution from DLSS Auto gave DLAA.

Compile the menu's real walk_to with its real steps (step_output_choice from vk_menu.cpp, upscaler_rows.h's step_engine
and step_render) and set_row's choice of the next value, and walk every start to every target. No game or GPU is needed.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, start):
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated block at %d" % start)


def definition(text, name):
    match = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not match:
        raise SystemExit("missing definition: " + name)
    return block(text, match.start())


FIXTURE = r"""
#define _CRT_SECURE_NO_WARNINGS   // (the Windows CRT marks getenv and sscanf deprecated: an error under -Werror)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>
#include <string>
#include <vector>
#include "hos_wide_size.h"
#include "upscaler_rows.h"
#include "display_modes.h"
namespace ur = mkwvk::upscaler_rows;
using namespace mkwvk;
// (the screen's display modes: none, so the sizes offered before them, 16:9 but 5120x1440, 32:9)
std::vector<display_modes::Size> screen_modes() { return {}; }
bool screen_size(uint32_t*, uint32_t*) { return false; }   // (the main screen: not known here)
bool host_screen_size(uint32_t*, uint32_t*) { return false; }

#define CHECK(x, ...) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s: ", __LINE__, #x); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); failed = 1; } } while (0)
/* OUTPUTS */
dm::Size size_of(Out o) { return {o.w, o.h}; }   // (vk_menu.cpp's, after the part cut out)
// (the shape the arrows walk: the size's own; the monitor's shape for a 16:9 size kept at it is test_output_resolution's)
std::string step_shape(int id) { return dm::ratio_name(size_of(output_at(id))); }
// (the size each choice shows: its own here, no screen to widen a 16:9 one to)
Out picture_for(int v, bool*) { return output_at(output_choice(v)); }
/* STEP_OUTPUT */
/* WALK */

// (set_row's next value for the three rows, as vk_menu.cpp has it: the Upscaler by engine, the Render resolution through
// the engine's slider and Native, the output by size; a step that changes nothing changes nothing)
struct Row { int* value; int lo, hi; };
int dlss = 0, resolution = 0, output = 0;
unsigned available = 0;
int upscaler() { return available & ur::bit(dlss) ? dlss : (int)ur::GAME; }   // (the one that runs: a saved one that cannot, the game's)
void set_row(Row& r, int v, int dir) {
    if (r.value == &dlss) v = ur::step_engine(upscaler(), dir, available);
    if (r.value == &resolution) {
        int up = upscaler(), res = resolution;
        ur::step_render(upscaler(), resolution, dir, &up, &res);
        if (up != upscaler()) dlss = up;
        v = res;
    }
    if (r.value == &output) {
        v = step_output_choice(output, dir);
        if (v == output) return;
    }
    *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
    if (r.value == &dlss) resolution = ur::snap_render(upscaler(), resolution);
}
bool set(Row& r, int want) {   // (menu_setting_set's call)
    // (a size of another shape: menu_setting_set moves the Aspect ratio to its shape first, which takes the output to one of
    // that shape's sizes, the largest with no screen known; the arrows keep to one shape)
    const auto shape = [](int v) { return display_modes::ratio_name(size_of(output_at(v))); };
    if (r.value == &output && output_choice(want) == want && shape(want) != shape(output)) {
        int to = -1;
        for (int v = 0; v <= custom_index(); v++)
            if (output_choice(v) == v && shape(v) == shape(want) &&
                (to < 0 || (uint64_t)output_at(v).w * output_at(v).h > (uint64_t)output_at(to).w * output_at(to).h)) to = v;
        output = to;
    }
    const auto native = [&] { return r.value == &resolution && ur::full_scale(upscaler()); };
    const int render = resolution, was = dlss;
    const bool ok = walk_to([&] { return std::pair(native() ? -1 : *r.value, dlss); }, [&] { return *r.value == want && !native(); },
                            [&](int dir) { set_row(r, *r.value + dir, dir); }, 64);
    if (r.value == &dlss) resolution = dlss == was ? render : ur::snap_render(upscaler(), render);
    return ok;
}

int main() {
    int failed = 0;
    // (the output sizes: every size from every size, with and without a size of the player's own)
    for (int custom = 0; custom < 2; custom++) {
        custom_output = custom ? Out{3440, 1440} : Out{0, 0};
        Row r{&output, 0, custom_index()};
        for (int from = 0; from <= custom_index(); from++) {
            if (output_choice(from) != from) continue;
            for (int want = 0; want <= custom_index(); want++) {
                output = from;
                const bool ok = set(r, want);
                if (output_choice(want) == want) CHECK(ok && output == want, "custom %d: %d to %d gave %d", custom, from, want, output);
                else CHECK(!ok && output == from, "custom %d: %d to the missing %d gave %d", custom, from, want, output);
            }
        }
    }
    // (the Render resolution with DLSS and with FSR 4: Auto and every slider stop from every place on the row, Native
    // included; reached on the scaled engine, not its Native)
    available = ur::bit(ur::GAME) | ur::bit(ur::FSR3) | ur::bit(ur::OFF) | ur::dlss_bits(true) | ur::fsr4_bits(true);
    for (const int engine : {(int)ur::DLSS, (int)ur::FSR4}) {
        // (Native keeps the value it came with: Left from the first stop leaves that stop's)
        int places[ur::kSliderN + 3][2] = {{engine, 0}, {ur::native_of(engine), 0}, {ur::native_of(engine), ur::kSlider[0]}};
        for (int i = 0; i < ur::kSliderN; i++) places[i + 3][0] = engine, places[i + 3][1] = ur::kSlider[i];
        Row r{&resolution, 0, ur::kValues - 1};
        for (const auto& from : places) {
            for (const auto& to : places) {
                if (to[0] != engine) continue;
                dlss = from[0], resolution = from[1];
                const bool ok = set(r, to[1]);
                if (from[0] == engine && from[1] == to[1]) continue;   // (there already)
                CHECK(ok && resolution == to[1] && dlss == engine, "upscaler %d: %d/%d to %d gave %d/%d", engine, from[0], from[1],
                      to[1], dlss, resolution);
            }
            // (a value that is no stop of the slider: 1 is 100 %, Native's size, which the row reaches only as Native)
            dlss = from[0], resolution = from[1];
            const bool ok = set(r, 1);
            CHECK(!ok && dlss == from[0] && (ur::full_scale(dlss) || resolution == from[1]), "upscaler %d: %d/%d to 1 gave %d/%d",
                  engine, from[0], from[1], dlss, resolution);
        }
    }
    // (a saved DLAA or FSR 4 native AA where neither runs: the game's own runs, and every one of its values is reached)
    available = ur::bit(ur::GAME) | ur::bit(ur::OFF);
    for (const int saved : {(int)ur::DLAA, (int)ur::FSR4_AA}) {
        Row r{&resolution, 0, ur::kValues - 1};
        for (const int from : ur::kGameSteps)
            for (const int want : ur::kGameSteps) {
                dlss = saved, resolution = from;
                const bool ok = set(r, want);
                CHECK(ok && resolution == want && dlss == saved, "saved %d not running: %d to %d gave %d/%d", saved, from, want, dlss,
                      resolution);
            }
    }
    available = ur::bit(ur::GAME) | ur::bit(ur::FSR3) | ur::bit(ur::OFF) | ur::dlss_bits(true) | ur::fsr4_bits(true);
    // (the Upscaler: every engine from every engine, with all of them here and with DLSS alone; one not here changes nothing)
    for (const unsigned here : {available, ur::bit(ur::GAME) | ur::bit(ur::FSR3) | ur::bit(ur::OFF) | ur::dlss_bits(true),
                                ur::bit(ur::GAME) | ur::bit(ur::OFF) | ur::dlss_bits(true)}) {
        available = here;
        const int engines[] = {ur::GAME, ur::DLSS, ur::FSR3, ur::FSR4, ur::OFF};
        Row r{&dlss, 0, ur::kUpscalers - 1};
        for (const int from : engines) {
            if (!(here & ur::bit(from))) continue;
            for (const int want : engines) {
                // (and the Render resolution as one step from there to want would leave it, not as the engines passed on
                // the way left it: FSR 3.1 to DLSS through Off and the game's own took 58 % to 50 %)
                for (int render = 0; render < ur::kValues; render++) {
                    dlss = from, resolution = render;
                    const int kept = here & ur::bit(want) && want != from ? ur::snap_render(want, render) : render;
                    const bool ok = set(r, want);
                    if (here & ur::bit(want)) CHECK(ok && dlss == want, "available %#x: %d to %d gave %d", here, from, want, dlss);
                    else CHECK(!ok && dlss == from, "available %#x: %d to the missing %d gave %d", here, from, want, dlss);
                    CHECK(resolution == kept, "available %#x: %d to %d took the render resolution %d to %d, not %d", here, from, want,
                          render, resolution, kept);
                }
            }
        }
    }
    if (!failed) std::puts("ok");
    return failed;
}
"""


def fixture(source):
    menu = (source / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    outputs_from = menu.index("struct Out {")
    outputs_to = menu.index("dm::Size size_of(Out o)", outputs_from)
    # (a size of another shape than the one chosen: menu_setting_set moves the Aspect ratio to its shape before the walk)
    setter_text = definition(menu, "int menu_setting_set")
    if "aspect_for_choice(want)" not in setter_text:   # (its shape: test_output_resolution checks aspect_for_choice)
        raise SystemExit("menu_setting_set no longer moves the Aspect ratio to a size of another shape before the walk")
    walk = re.search(r"template <class State, class At, class Step>\s*bool walk_to\(", menu)
    if not walk:
        raise SystemExit("missing definition: walk_to")
    setter = definition(menu, "int menu_setting_set")
    if "walk_to(" not in setter:
        raise SystemExit("menu_setting_set no longer walks the list rows with walk_to")
    replacements = {
        "/* OUTPUTS */": menu[outputs_from:outputs_to],
        "/* STEP_OUTPUT */": definition(menu, "int step_output_choice"),
        "/* WALK */": block(menu, walk.start()),
    }
    result = FIXTURE
    for marker, value in replacements.items():
        result = result.replace(marker, value)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="menu-walk-") as tmp:
        src = Path(tmp) / "menu_walk.cpp"
        exe = Path(tmp) / ("menu_walk.exe" if os.name == "nt" else "menu_walk")
        src.write_text(fixture(args.source), encoding="utf-8")
        includes = [args.source / "host", args.source / "gpu" / "vk"]
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/D_CRT_SECURE_NO_WARNINGS"] + ["/I" + str(i) for i in includes] + [
                str(src), "/Fo" + str(Path(tmp) / "menu_walk.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror"] + [a for i in includes for a in ("-I", str(i))] + [
                str(src), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
