"""Quality presets preserve the player's frame-generation choice.

Compile the menu's actual preset and row handlers with small stand-ins.
Check that the game FPS cap keeps its saved key when its label changes. Preset
and frame-generation changes do not reset temporal failures; the three scaling
rows do, using their real row definitions and scaling helpers. No game or GPU
is needed.
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
#include "upscaler_rows.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
int saves = 0;
void save() { ++saves; }
void apply_pad_layout(int) {}
void sync_preset_budgets(int) {}
constexpr int kAutoPreset = 6;   /* (the Quality preset Auto (this PC): apply_auto, not reached here) */
void apply_auto(bool) {}
void env_release(const int*) {}   // (the environment's hold on a row: none here)
void vram_budget_sync() {}
constexpr int kVramMinimumOnly = 1;
bool row_fixed(const int*) { return false; }
namespace ur = mkwvk::upscaler_rows;
int upscaler() { return opts.dlss; }
unsigned upscalers_available() { return ur::bit(ur::GAME) | ur::dlss_bits(true); }
namespace mr { constexpr int kMax = 8; }
/* (the Game language row's hook in set_row: language.txt, not written here) */
namespace i18n { inline void set_game_language(int) {} }
void restart_values(int*) {}
struct { void note(const int*) {} } restart_state;
bool can_mic() { return false; }
void mic_chosen() {}
void samplers_changed() {}
int temporal_resets = 0;
void temporal_reset_failure() { ++temporal_resets; }
void prompts_at_once(const Row&) {}
int step_output_choice(int id, int dir) { return id + dir; }
// (the Aspect ratio row moves the Output resolution to one of its shape's sizes, output_for_aspect: not what this checks)
int output_for_aspect(int) { return opts.output_res; }
const char* tx_framegen(int) { return ""; }
const char* hn_framegen(int, int) { return ""; }
const char* tx_preset(int) { return ""; }
const char* hn_preset(int, int) { return ""; }
const char* tx_fps_cap(int) { return ""; }
const char* tx_upscaler(int) { return ""; }
const char* hn_upscaler(int, int) { return ""; }
const char* tx_resolution(int) { return ""; }
const char* hn_resolution(int, int) { return ""; }
const char* tx_dlss_model(int) { return ""; }
const char* hn_dlss_model(int, int) { return ""; }
/* INI_KEY */
/* STOPS */
/* PRESET */
/* SET_ROW */

int main() {
    Row framegen = /* FRAMEGEN_ROW */;
    Row quality = /* QUALITY_ROW */;
    Row fps_cap = /* FPS_CAP_ROW */;
    CHECK(std::strcmp(fps_cap.name, "Game frame rate cap") == 0);
    CHECK(std::strcmp(ini_key(fps_cap), "frame_rate_cap") == 0);
    CHECK(std::strstr(fps_cap.help, "Generated frames are additional") != nullptr);
    CHECK(opts.framegen == 0);
    for (int previous = 0; previous <= 8; ++previous) {
        for (int preset = 0; preset <= 5; ++preset) {
            opts = Options{};
            opts.framegen = previous;
            set_row(quality, preset, 1);
            CHECK(opts.preset == preset);
            CHECK(opts.framegen == previous);
        }
    }
    CHECK(framegen.preset_exempt && !framegen.restart);
    for (int preset = 1; preset <= 5; ++preset) {
        opts = Options{};
        opts.preset = preset;
        set_row(framegen, 2, 1);
        CHECK(opts.framegen == 2 && opts.preset == preset);
        set_row(framegen, 0, -1);
        CHECK(opts.framegen == 0 && opts.preset == preset);
    }
    CHECK(std::strstr(quality.help, "Frame generation") != nullptr);
    CHECK(std::strstr(quality.help, "stay yours") != nullptr);
    CHECK(saves > 0);
    CHECK(temporal_resets == 0);

    // Only the Upscaler, Render resolution and DLSS model rows reset temporal failures.
    Row temporal_rows[] = {/* UPSCALER_ROW */, /* RESOLUTION_ROW */, /* DLSS_MODEL_ROW */};
    for (Row& r : temporal_rows) {
        opts = Options{};
        if (r.value == &opts.dlss_model) opts.dlss = ur::DLSS;
        const int previous = *r.value, resets = temporal_resets;
        set_row(r, previous + r.step, 1);
        CHECK(*r.value != previous);
        CHECK(temporal_resets == resets + 1);
    }
    std::puts("PASS: quality presets preserve frame generation and its row preserves the preset");
}
"""


def fixture(source):
    menu = (source / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (source / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    options = re.search(r"struct Options \{.*?\n\};", header, re.S)
    tabs = re.search(r"enum MenuTab \{.*?\};", menu, re.S)
    row = re.search(r"struct Row \{.*?\n\};", menu, re.S)
    if not options or not tabs or not row:
        raise SystemExit("the menu's options, tabs, or row definition is missing")
    fps_cap = re.search(r'\{TAB_GRAPHICS, "[^"]+", &opts\.fps_cap,', menu)
    if not fps_cap:
        raise SystemExit("missing game frame rate cap row")
    replacements = {
        "/* OPTIONS */": options.group(0),
        "/* TABS */": tabs.group(0),
        "/* ROW */": row.group(0),
        "/* STOPS */": definition(menu, "bool stops_at_ends"),
        "/* INI_KEY */": definition(menu, "const char* ini_key"),
        "/* FPS_CAP_ROW */": block(menu, fps_cap.start()),
        "/* PRESET */": definition(menu, "void apply_preset"),
        "/* SET_ROW */": definition(menu, "void set_row"),
        "/* FRAMEGEN_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Frame generation"')),
        "/* QUALITY_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Quality preset"')),
        "/* UPSCALER_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Upscaler"')),
        "/* RESOLUTION_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Render resolution"')),
        "/* DLSS_MODEL_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "DLSS model"')),
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
    includes = str((args.source / "gpu" / "vk").resolve())
    with tempfile.TemporaryDirectory(prefix="framegen-preset-") as tmp:
        src = Path(tmp) / "framegen_preset.cpp"
        exe = Path(tmp) / ("framegen_preset.exe" if os.name == "nt" else "framegen_preset")
        src.write_text(fixture(args.source), encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/I" + includes, str(src),
                       "/Fo" + str(Path(tmp) / "framegen_preset.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", includes,
                       str(src), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
