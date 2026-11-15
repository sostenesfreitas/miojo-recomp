"""Upscaler changes borrow a supported size without overwriting the saved render scale.

Compile the real menu rows, set_row, menu_setting_set, preset, save and options-file
reader with hardware/UI stand-ins. Exercise every saved scale through every engine,
including a restart while the game's own upscaler is selected. Auto (this PC) starts
inactive; a separate case checks its fallback scaling and handover to manual choices
with the real apply_auto and menu_auto_tune. The four-row fixture still compiles
the complete shared set_row; unrelated renderer actions must never run. No game or GPU.
"""
import argparse
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
    raise SystemExit("unterminated block")


def definition(text, name):
    match = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not match:
        raise SystemExit("missing definition: " + name)
    return block(text, match.start())


FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include "upscaler_rows.h"
#include "auto_settings.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sys/stat.h>   // (save(): an options file that is a link is written through, not replaced)
#include <mutex>
#include <string>
#include <utility>
#include <vector>
using namespace mkwvk;
namespace ur = mkwvk::upscaler_rows;
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
// set_row also dispatches Debug-row neighbours in the same product function.
// None is among this fixture's four rows: reaching the renderer hook is a failure.
void menu_debug_rows(int, int) { CHECK(false); }
void logf(const char*, ...) {}   // (declared ahead of the options code cut in below: else its logf is C's logf(float))
/* OPTIONS */
/* AUTO_PRESET */
Options opts;
/* TABS */
/* ROW */
unsigned available = (1u << ur::kUpscalers) - 1;
int upscaler() { return available & ur::bit(opts.dlss) ? opts.dlss : (int)ur::GAME; }
unsigned upscalers_available() { return available; }
bool row_fixed(const int*) { return false; }
struct Out { uint32_t w, h; };
Out shown_output() { return {1920, 1080}; }
Out output_at(int) { return shown_output(); }
const std::vector<Out>& outputs() {
    static const std::vector<Out> sizes{shown_output()};
    return sizes;
}
int custom_index() { return 0; }
int output_choice(int v) { return v; }
int parse_output(const char*) { return 0; }
int step_output_choice(int v, int dir) { return v + dir; }
int output_for_aspect(int) { return opts.output_res; }
int aspect_for_choice(int) { return opts.ultrawide; }
int ultrawide_read = 0, aspect_started = 0;
bool aspect_follows_host(int) { return false; }
Out size_of(Out o) { return o; }
namespace dm { int ratio_name(Out) { return 0; } }
/* SCALE */
/* TEXT */
const char* tx_upscaler(int) { return ""; }
const char* hn_upscaler(int, int) { return ""; }
const char* tx_dlss_model(int) { return ""; }
const char* hn_dlss_model(int, int) { return ""; }
const char* tx_preset(int) { return ""; }
const char* hn_preset(int, int) { return ""; }
Row rows[] = {/* UPSCALER_ROW */, /* RESOLUTION_ROW */, /* MODEL_ROW */, /* PRESET_ROW */};
constexpr int kRows = sizeof rows / sizeof rows[0];
/* OVERRIDES */
/* INI_KEY */
std::string ini_file, mic_want;
std::string ini_path() { return ini_file; }
std::string mic_key(const char* v) { return v; }
void mic_tell_host() {}
void refresh_mics() {}
int rumble_step(int v) { return v; }
constexpr int HOS_KEY_N = 1, HOS_SW_N = 1, HOS_SW_PLUS = 10, HOS_SW_MINUS = 11, HOS_BIND_PAD = 0, HOS_BIND_KEY = 1;
constexpr int HOS_KEY_OC_NOTE_L = 1, HOS_KEY_OC_NOTE_X = 1;
const char* bind_names[1] = {"a"};
struct Host { int (*binding)(int, int, int); float (*aspect_ratio)(); } *host = nullptr;
bool can_bind() { return false; }
// No hardware profile or measured tuning owns the manual round trips.
uint64_t auto_for = 0, auto_fp = 0, auto_tuned = 0;
::auto_settings::Choice auto_choice{};
::auto_settings::Tuning auto_tuning{};
/* SAVE */
void restart() {
    opts = Options{};
    env_override_count = 0;
    auto_for = auto_tuned = 0;
    auto_tuning = {};
    // The hardware stand-ins (auto_choice and auto_fp) describe the same PC after a restart.
    bool has_options = false, has_output = false, has_vram_budget = false, has_memory_budget = false;
    /* LOAD */
    CHECK(has_options && !has_output && has_memory_budget && !has_vram_budget);
}
void apply_pad_layout(int) {}
int detect_pad_layout() { return opts.pad_layout; }
void sync_preset_budgets(int) {}
void vram_budget_sync() {}
constexpr int kVramMinimumOnly = 1;
namespace mr { constexpr int kMax = 8; struct State { void note(const int*) {} }; }
mr::State restart_state;
void restart_values(int*) {}
bool can_mic() { return false; }
void mic_chosen() {}
void samplers_changed() {}
void temporal_reset_failure() {}
void prompts_at_once(const Row&) {}
/* STOPS */
void apply_auto(bool keep_own);
/* PRESET */
/* APPLY_AUTO */
/* SET_ROW */
/* RESOLUTION_SCALE */
/* RESOLUTION_AUTO */
/* WALK */
std::recursive_mutex menu_mu;
bool menu_refresh = false;
int env_int(const char*, int fallback) { return fallback; }
double fps_target_ms() { return 1000.0 / 60; }
int display_refresh_mhz() { return 60000; }
/* AUTO_TUNE */
void bridge_probe_first() {}
bool bridge_row(int) { return true; }
struct MkwVkSetting {
    char key[64], name[128], tab[32], text[256];
    int value, lo, hi, step;
    bool restart, fixed;
};
/* BRIDGE_FILL */
/* GET */
/* BRIDGE */
int active_scale() { return ur::full_scale(upscaler()) ? 10000 : resolution_scale(); }
void arrow_to(int engine) {
    for (int n = 0; opts.dlss != engine && n < ur::kUpscalers; ++n) set_row(rows[0], 0, 1);
    CHECK(opts.dlss == engine);
}
void bridge_to(int engine) {
    MkwVkSetting out{};
    CHECK(menu_setting_set("dlss", engine, &out) == 1 && out.value == engine);
}
void fresh(int engine, int value) {
    opts = Options{};
    env_override_count = 0;
    auto_for = auto_fp = auto_tuned = 0;
    auto_choice = {};
    auto_tuning = {};
    opts.dlss = engine;
    opts.resolution = value;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    ini_file = argv[1];
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int engines[] = {ur::GAME, ur::DLSS, ur::FSR3, ur::FSR4, ur::OFF};
    // Every saved value, including all named qualities and hand-chosen slider stops.
    for (int engine : engines) for (int value = 0; value < ur::kValues; ++value) {
        fresh(engine, value);
        const int before = active_scale();
        const std::string text = tx_resolution(value);
        const int away = engine == ur::GAME ? (int)ur::DLSS : (int)ur::GAME;
        arrow_to(away);
        const int via = active_scale();
        arrow_to(engine);
        const int after = active_scale();
        const int stored = opts.resolution;
        CHECK(stored == value && after == before && text == tx_resolution(opts.resolution));
        fresh(engine, value);
        arrow_to(away);
        save();
        restart();
        CHECK(opts.dlss == away && opts.resolution == value);
        arrow_to(engine);
        const int restarted = active_scale();
        CHECK(opts.resolution == value && restarted == before);
        fresh(engine, value);
        bridge_to(away);
        bridge_to(engine);
        const int bridged = active_scale();
        CHECK(opts.resolution == value && bridged == before);
        std::printf("ROUNDTRIP engine=%d value=%d percent=%d before=%d via=%d after=%d stored=%d restart=%d bridge=%d\n",
                    engine, value, ur::percent(value), before, via, after, stored, restarted, bridged);
        // Other intermediates and either arrow direction must also leave the choice alone.
        for (int other : engines) {
            fresh(engine, value);
            bridge_to(other);
            bridge_to(engine);
            CHECK(opts.resolution == value && active_scale() == before);
            for (int dir : {-1, 1}) {
                fresh(engine, value);
                set_row(rows[0], 0, dir);
                set_row(rows[0], 0, -dir);
                CHECK(opts.dlss == engine && opts.resolution == value && active_scale() == before);
            }
        }
    }
    // Fresh preset selections retain their current scales. Custom does not reapply a preset.
    const int preset_values[] = {5, 4, 2, 1, 1, 1};
    for (int engine : engines) for (int preset = 0; preset <= 5; ++preset) {
        fresh(engine, 5);
        set_row(rows[3], preset, 1);
        const int expected = preset ? ur::snap_render(engine, preset_values[preset]) : 5;
        CHECK(opts.resolution == expected && opts.dlss == engine);
        bridge_to(engine == ur::GAME ? (int)ur::DLSS : (int)ur::GAME);
        bridge_to(engine);
        CHECK(opts.resolution == expected);
    }
    // A change to the visible snapped size is deliberate; it replaces the old preference.
    fresh(ur::DLSS, 5);
    bridge_to(ur::GAME);
    CHECK(active_scale() == 5000);
    MkwVkSetting shown{};
    CHECK(menu_setting(1, &shown) == 1 && shown.value == 5);
    CHECK(std::strcmp(shown.key, "resolution") == 0 && std::strcmp(shown.text, "50% (960x540)") == 0);
    bridge_to(ur::DLSS);
    CHECK(menu_setting(1, &shown) == 1 && shown.value == 5);
    CHECK(std::strcmp(shown.text, "Balanced 58% (1120x630)") == 0);
    bridge_to(ur::GAME);
    set_row(rows[1], 0, -1);
    CHECK(opts.resolution == 3);  // Left from the shown 50% is 67%, not the hidden 58%.
    bridge_to(ur::DLSS);
    CHECK(opts.resolution == 3 && active_scale() == 6667);
    // Native changes the mode, not the saved scale: FSR 3.1 100% -> DLSS/FSR 4 92% -> Native -> FSR 3.1 100%.
    for (int engine : {(int)ur::DLSS, (int)ur::FSR4}) {
        fresh(ur::FSR3, 1);
        CHECK(active_scale() == 10000 && std::strcmp(tx_resolution(opts.resolution), "100% (1920x1080)") == 0);
        bridge_to(engine);
        CHECK(opts.resolution == 1 && active_scale() == 9167);
        CHECK(menu_setting(1, &shown) == 1 && shown.value == 1 && std::strcmp(shown.text, "92% (1760x990)") == 0);
        set_row(rows[1], 0, -1);
        CHECK(opts.dlss == ur::native_of(engine) && opts.resolution == 1 && active_scale() == 10000);
        CHECK(menu_setting(1, &shown) == 1 && shown.value == 1 &&
              std::strcmp(shown.text, engine == ur::DLSS ? "Native (DLAA, 1920x1080)" : "Native (FSR 4, 1920x1080)") == 0);
        restart();   // set_row saved the Native mode and the untouched numeric choice.
        CHECK(opts.dlss == ur::native_of(engine) && opts.resolution == 1 && active_scale() == 10000);
        bridge_to(ur::FSR3);
        CHECK(opts.resolution == 1 && active_scale() == 10000);
        CHECK(menu_setting(1, &shown) == 1 && shown.value == 1 && std::strcmp(shown.text, "100% (1920x1080)") == 0);
        std::printf("NATIVE_ROUNDTRIP engine=%d returned=%d stored=%d\n", engine, active_scale(), opts.resolution);
        // Leaving Native for a numbered stop still deliberately replaces the saved choice.
        bridge_to(engine);
        set_row(rows[1], 0, -1);
        set_row(rows[1], 0, 1);
        CHECK(opts.dlss == engine && opts.resolution == 9 && active_scale() == 9167);
        // Entering Native from Auto keeps its sentinel; Left returns to Auto, not a numbered stop.
        fresh(engine, 0);
        set_row(rows[1], 0, 1);
        CHECK(opts.dlss == ur::native_of(engine) && opts.resolution == 0 && active_scale() == 10000);
        set_row(rows[1], 0, -1);
        CHECK(opts.dlss == engine && resolution_auto() && resolution_scale() == 0);
    }
    // Selecting each named quality through the bridge still reaches its exact slider stop.
    for (int engine : {(int)ur::DLSS, (int)ur::FSR3, (int)ur::FSR4}) {
        for (int value : ur::kSlider) {
            fresh(engine, 0);
            MkwVkSetting out{};
            CHECK(menu_setting_set("resolution", value, &out) == 1 && out.value == value);
            CHECK(opts.dlss == engine && opts.resolution == value);
        }
    }
    // Model presets are not quality scales, and do not overwrite one.
    for (int model = 0; model < ur::kModels; ++model) {
        fresh(ur::DLSS, 5);
        set_row(rows[2], model, 1);
        bridge_to(ur::GAME);
        bridge_to(ur::DLSS);
        CHECK(opts.dlss_model == model && opts.resolution == 5);
    }
    // Auto is a sentinel, not a percentage: every engine keeps it through snapping, saving and loading.
    // Native's separate full-size override is unchanged; it does not turn the stored 0 into a scale.
    for (int engine = 0; engine < ur::kUpscalers; ++engine) {
        fresh(engine, 0);
        CHECK(resolution_auto() && resolution_scale() == 0 && ur::snap_render(engine, 0) == 0);
        save();
        restart();
        CHECK(opts.resolution == 0 && resolution_auto() && resolution_scale() == 0);
    }
    // An environment-held Auto must not become a saved scale when only the engine changes.
    fresh(ur::DLSS, 5);
    env_override(&opts.resolution, 0);
    bridge_to(ur::GAME);
    CHECK(resolution_auto() && resolution_scale() == 0 && saved_value(&opts.resolution) == 5);
    restart();
    CHECK(opts.dlss == ur::GAME && opts.resolution == 5 && !resolution_auto());
    bridge_to(ur::DLSS);
    CHECK(opts.resolution == 5 && resolution_scale() == 5833);
    // Unlike manual engine changes, Auto owns fallback scaling until a row hands it back to Custom.
    for (bool via_bridge : {false, true}) {
        fresh(ur::DLSS, 5);
        auto_choice.tier = 3;
        auto_choice.upscaler = ur::DLSS;
        auto_choice.resolution = 5;
        auto_fp = 0x2916;
        set_row(rows[3], kAutoPreset, 1);
        CHECK(opts.preset == kAutoPreset && auto_for == auto_fp && opts.resolution == 5);
        auto_tuned = auto_for;   // Already settled: test engine fallback, not timing-based tuning.
        const unsigned all_engines = available;
        available = ur::bit(ur::GAME);
        menu_auto_tune(10, 200, 60);
        CHECK(opts.preset == kAutoPreset && opts.resolution == 1 && active_scale() == 10000);
        available = all_engines;
        menu_auto_tune(10, 200, 60);
        CHECK(opts.preset == kAutoPreset && opts.resolution == 5 && active_scale() == 5833);
        auto_tuning.steps = 1;
        auto_tuning.went_down = true;
        save();
        restart();
        CHECK(opts.preset == kAutoPreset && opts.resolution == 5);
        CHECK(auto_for == auto_fp && auto_tuned == auto_fp);
        CHECK(auto_tuning.steps == 1 && auto_tuning.went_down);
        if (via_bridge) bridge_to(ur::GAME);
        else arrow_to(ur::GAME);
        CHECK(opts.preset == 0 && opts.resolution == 5 && active_scale() == 5000);
        menu_auto_tune(10, 200, 60);
        CHECK(opts.resolution == 5 && active_scale() == 5000);
        restart();
        menu_auto_tune(10, 200, 60);
        CHECK(opts.preset == 0 && opts.resolution == 5 && active_scale() == 5000);
        if (via_bridge) bridge_to(ur::DLSS);
        else arrow_to(ur::DLSS);
        menu_auto_tune(10, 200, 60);
        CHECK(opts.preset == 0 && opts.resolution == 5 && active_scale() == 5833);
    }
    std::printf("upscaler round trip: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


def fixture(source):
    menu = (source / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (source / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    init = definition(menu, "void menu_init")
    load_at = init.index('    if (FILE* f = fopen(ini_path().c_str(), "r")) {')
    walk_at = menu.index("template <class State, class At, class Step>")
    auto_at = header.index("constexpr int kAutoPreset")
    replacements = {
        "OPTIONS": block(header, header.index("struct Options {")) + ";",
        "AUTO_PRESET": header[auto_at:header.index(";", auto_at) + 1],
        "TABS": block(menu, menu.index("enum MenuTab {")) + ";",
        "ROW": block(menu, menu.index("struct Row {")) + ";",
        "OVERRIDES": menu[menu.index("struct EnvOverride {"):menu.index("std::string ini_path() {")],
        "LOAD": init[load_at:init.index("    first_start = !has_options;", load_at)],
        "SCALE": definition(menu, "int twelfths") + "\n" + definition(menu, "void game_size"),
        "TEXT": definition(menu, "const char* tx_resolution") + "\n" + definition(menu, "const char* hn_resolution"),
        "WALK": block(menu, walk_at),
    }
    for marker, name in (("INI_KEY", "const char* ini_key"), ("SAVE", "void save"),
                         ("STOPS", "bool stops_at_ends"), ("PRESET", "void apply_preset"),
                         ("APPLY_AUTO", "void apply_auto"), ("AUTO_TUNE", "void menu_auto_tune"),
                         ("SET_ROW", "void set_row"), ("BRIDGE", "int menu_setting_set"),
                         ("RESOLUTION_SCALE", "int resolution_scale"), ("RESOLUTION_AUTO", "bool resolution_auto"),
                         ("BRIDGE_FILL", "void bridge_fill"), ("GET", "int menu_setting")):
        replacements[marker] = definition(menu, name)
    for marker, label in (("UPSCALER_ROW", "Upscaler"), ("RESOLUTION_ROW", "Render resolution"),
                          ("MODEL_ROW", "DLSS model"), ("PRESET_ROW", "Quality preset")):
        replacements[marker] = block(menu, menu.index('{TAB_GRAPHICS, "' + label + '"'))
    result = FIXTURE
    for marker, value in replacements.items():
        result = result.replace("/* " + marker + " */", value)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="upscaler-round-trip-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        src.write_text(fixture(args.source), encoding="utf-8")
        include = str((args.source / "gpu/vk").resolve())
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/I" + include,
                       str(src), "/Fo" + str(Path(tmp) / "fixture.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", include,
                       str(src), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        raise SystemExit(subprocess.run([str(exe), str(Path(tmp) / "hookshot_options.ini")], cwd=tmp).returncode)


if __name__ == "__main__":
    main()
