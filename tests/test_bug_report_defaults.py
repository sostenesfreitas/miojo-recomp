"""Drive the real report row formatter with authored display/upscaler contexts.

No game or Vulkan device. --source checks the same behavior against an older
checkout; the released/current pre-fix formatter compiles and fails the cases.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def definition(text, name):
    match = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not match:
        raise SystemExit("missing definition: " + name)
    opening = text.index("{", match.start())
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if not depth:
            return text[match.start():at + 1]
    raise SystemExit("unterminated definition: " + name)


def fixture(source):
    menu = (source / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (source / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    options = re.search(r"struct Options \{.*?\n\};", header, re.S).group()
    row = re.search(r"struct Row \{.*?\n\};", menu, re.S).group()
    tabs = re.search(r"enum MenuTab \{.*?\};", menu, re.S).group()
    names = dict((member, name) for name, member in re.findall(r'\{TAB_\w+, "([^"]+)", &opts\.(\w+),', menu))
    controls = "\n".join(definition(menu, signature) for signature in (
        "void apply_preset", "void apply_auto", "void sync_preset_budgets", "void vram_budget_sync"))
    auto_members = sorted(set(re.findall(r"opts\.(\w+)\s*=", controls)) & names.keys())
    text_functions = {
        "ultrawide": "tx_ultrawide", "output_res": "tx_output_res", "resolution": "tx_resolution",
        "dlss": "tx_upscaler", "dlss_model": "tx_dlss_model", "preset": "tx_preset", "gpu_device": "tx_gpu", "mic": "tx_mic",
    }
    # (the rows a preset sets whose texts are fixed: the built-in defaults for a Custom player Auto never chose for)
    preset_rows = ["sharpness", "pace", "realtime", "handheld", "vram_budget"]
    members = list(dict.fromkeys(["ultrawide", "output_res", "resolution", "dlss", *auto_members, *preset_rows,
                                 "framegen_by", "game_lang", "dlss_model", "gpu_device", "mic", "fov"]))
    rows = "\n".join('    {TAB_GRAPHICS, "%s", &opts.%s, 0, 100, 1, %s, nullptr},' %
                     (names[member], member, text_functions.get(member, "tx_number")) for member in members)
    formatter = definition(menu, "std::string changed_rows_text")
    modern = "std::string&" in formatter.split("{")[0]
    policy = definition(menu, "bool report_default_depends_on_pc") if modern else ""
    known = definition(menu, "int report_default") if "int report_default(" in menu else ""
    auto_for = re.search(r"^uint64_t auto_for = 0;", menu, re.M)
    auto_for = auto_for.group() if auto_for else "uint64_t auto_for = 0;"
    defaults = dict((member, int(value)) for member, value in re.findall(r"\bint (\w+)\s*=\s*(-?\d+);", options))
    # (each preset row moved off its built-in default, and the line a report gives it then)
    moved = dict((member, 1 if defaults[member] == 0 else 0) for member in preset_rows)
    custom_set = " ".join("opts.%s = %d;" % (member, moved[member]) for member in preset_rows)
    custom_lines = ", ".join('"%s: %d (default: %d)"' % (names[member], moved[member], defaults[member]) for member in preset_rows)
    custom_names = ", ".join('"%s: "' % names[member] for member in preset_rows)
    call = "changed_rows_text(pc)" if modern else "changed_rows_text()"
    source_parts = [definition(menu, signature) for signature in (
        "const char* tx_upscaler", "const char* dlss_preset_env", "const char* tx_dlss_model",
        "const char* tx_output_res", "const char* tx_ultrawide", "const char* tx_preset",
        "const char* tx_gpu", "const char* tx_mic")]
    unchanged = " && ".join("opts.%s == before.%s" % (member, member) for member in re.findall(r"\bint (\w+)\s*=", options))
    auto_checks = "\n".join('        CHECK(pc.find("%s: ") != std::string::npos, "Auto-controlled %s missing");' %
                            (names[member], member) for member in auto_members)
    return r'''#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include "host/hos_wide_size.h"
#include "gpu/vk/display_modes.h"
#include "gpu/vk/upscaler_rows.h"
using namespace mkwvk;
namespace ur = upscaler_rows;
''' + options + r'''
Options opts;
''' + auto_for + r'''
namespace mkwvk::i18n {
int lang_saved; // language.txt's language, as the stand-in reads it
int game_lang_index() { return lang_saved; }
}
static uint32_t screen_w, screen_h;
bool screen_size(uint32_t* w, uint32_t* h) {
    if (!screen_w || !screen_h) return false;
    *w = screen_w; *h = screen_h; return true;
}
bool host_screen_size(uint32_t* w, uint32_t* h) { return screen_size(w, h); }
std::vector<display_modes::Size> screen_modes() { return {}; }
int upscaler() { return opts.dlss; } // authored available engine; no backend/GPU call
std::vector<std::string> gpu_device_names{"Authored GPU"};
int mic_calls;
struct Host { const char* (*mic_name)(int); };
const char* mic_name(int) { ++mic_calls; return "PRIVATE_MICROPHONE_NAME"; }
Host host_storage{mic_name}; Host* host = &host_storage;
bool mic_missing = false; int mic_listed = 1; std::string mic_want;
bool can_mic() { return true; }
void utf8_whole(char*) {} // the missing-device path is not used; names must be suppressed first
void logf(const char*, ...) {}
''' + menu[menu.index("struct Out {"):menu.index("/// The Render resolution: the value chosen")] + "\n" + "\n".join(source_parts) + "\n" + tabs + "\n" + row + r'''
const char* tx_number(int value) { static char text[24]; std::snprintf(text, sizeof text, "%d", value); return text; }
Row rows[] = {
''' + rows + r'''
};
int ultrawide_read = 1;
''' + definition(menu, "void outputs_sync") + r'''
static bool pending_restart;
bool restart_changed(const Row& r) { return pending_restart && r.value == &opts.output_res; }
''' + policy + "\n" + known + "\n" + formatter + r'''
int failures;
const char* case_name;
#define CHECK(condition, why) do { if (!(condition)) { ++failures; std::printf("FAIL %s: %s\n", case_name, why); } } while (0)
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    case_name = argv[1];
    const std::string which = case_name;
    if (which == "output-context" || which == "aspect-changed-wide") screen_w = 3440, screen_h = 1440;
    if (which == "reordered-shapes") screen_w = 1280, screen_h = 1024;
    opts = Options{};
    outputs_sync(); // the real startup conversion, not a guessed display-list index
    // subject: a row whose default depends on this PC; at_default: rows at their known defaults, in neither list;
    // known: the lines a row away from its known default gives, among the changes
    std::string subject, expected_current, fresh_default;
    std::vector<std::string> at_default, known;
    if (which == "aspect-default" || which == "reordered-shapes") {
        at_default = {"Aspect ratio: "}; // the monitor's shape, as outputs_sync made it
    } else if (which == "aspect-changed" || which == "aspect-changed-wide") {
        const std::string monitors = tx_ultrawide(opts.ultrawide);
        opts.ultrawide = aspect_index("16:9");
        const std::string now = tx_ultrawide(opts.ultrawide);
        CHECK(now != monitors, "the authored screen's shape is 16:9");
        known = {"Aspect ratio: " + now + " (default: " + monitors + ")"};
    } else if (which == "custom-never-auto" || which == "custom-after-auto") {
        // Custom, the rows a preset sets moved: their built-in defaults hold until Auto (this PC) has chosen for the file
        ''' + custom_set + r'''
        if (which == "custom-after-auto") auto_for = 1;
        if (which == "custom-never-auto") known = {''' + custom_lines + r'''};
    } else if (which == "framegen-by") {
        opts.framegen_by = 1;
        known = {"''' + names["framegen_by"] + r''': 1 (default: Auto)"}; // not the row's live text at Auto
    } else if (which == "lang-saved") {
        i18n::lang_saved = 3; opts.game_lang = 3;
        at_default = {"''' + names["game_lang"] + r''': ", "''' + names["framegen_by"] + r''': "};
    } else if (which == "lang-changed") {
        i18n::lang_saved = 3; opts.game_lang = 0;
        known = {"''' + names["game_lang"] + r''': 0 (default: 3)"};
    } else if (which == "output-context") {
        subject = "Output resolution";
        fresh_default = tx_output_res(opts.output_res);
        opts.ultrawide = aspect_index("16:9");
        opts.output_res = parse_output("3840x2160");
        expected_current = tx_output_res(opts.output_res);
        pending_restart = true;
    } else if (which == "render-context") {
        subject = "Render resolution";
        fresh_default = tx_resolution(opts.resolution);
        opts.dlss = ur::DLAA; opts.resolution = 1;
        expected_current = tx_resolution(opts.resolution);
    } else if (which == "upscaler-context") {
        subject = "Upscaler";
        fresh_default = tx_upscaler(opts.dlss);
        opts.dlss = ur::DLSS;
        expected_current = tx_upscaler(opts.dlss);
    } else if (which == "auto-profile") {
        opts.preset = 6; opts.dlss = ur::DLSS; opts.resolution = 3;
        opts.output_res = parse_output("1920x1080"); opts.upscale = 2;
        opts.sharpness = 50; opts.pace = 1; opts.handheld = 0; opts.vram_budget = 7;
    } else if (which == "stable-sentinels") {
        opts.gpu_device = 1; opts.mic = 2; opts.fov = 110;
    } else if (which == "model-env" || which == "model-fixed") {
        opts.dlss_model = 1;
    } else return 2;
    const Options before = opts;
    std::string pc;
    const std::string changed = ''' + call + r''';
    CHECK(''' + unchanged + r''', "report mutated live options");
    if (!subject.empty()) {
        CHECK(changed.find(subject + ": ") == std::string::npos, "context-dependent row falsely called changed");
        CHECK(pc.find(subject + ": " + expected_current + " (default depends on this PC") != std::string::npos,
              "current value/context note missing");
        CHECK(pc.find("(default: ") == std::string::npos, "invented a numeric/context-free default");
        std::printf("CONTEXT %s: fresh=<%s> current=<%s>\n", case_name, fresh_default.c_str(), expected_current.c_str());
    }
    for (const std::string& name : at_default)
        CHECK(changed.find(name) == std::string::npos && pc.find(name) == std::string::npos, "a row at its known default listed");
    for (const std::string& line : known) {
        CHECK(changed.find(line + "\n") != std::string::npos, "a known default not reported as the change it is");
        const std::string name = line.substr(0, line.find(": ") + 2);
        CHECK(pc.find(name) == std::string::npos, "a row with a known default put in the PC section");
    }
    if (which == "custom-after-auto") {
        for (const char* name : {''' + custom_names + r'''}) {
            CHECK(changed.find(name) == std::string::npos, "a row Auto chose for counted as a change from the built-in default");
            CHECK(pc.find(std::string(name) + "1 (default depends on this PC") != std::string::npos ||
                  pc.find(std::string(name) + "0 (default depends on this PC") != std::string::npos, "a row Auto chose for missing");
        }
    }
    if (which == "auto-profile") {
        CHECK(changed.empty(), "Auto-controlled settings counted as fixed-default changes");
''' + auto_checks + r'''
    }
    if (which == "stable-sentinels") {
        CHECK(changed.find("GPU device: 0: Authored GPU (default: Auto)") != std::string::npos, "GPU Auto sentinel changed");
        CHECK(changed.find("Microphone: a microphone by its name (default: Default)") != std::string::npos, "microphone sentinel/privacy changed");
        CHECK(changed.find("Field of view: 110 (default: 100)") != std::string::npos, "fixed default stopped reporting a real change");
        CHECK(pc.find("GPU device: ") == std::string::npos && pc.find("Microphone: ") == std::string::npos, "stable sentinel put in PC section");
        CHECK(mic_calls == 0 && changed.find("PRIVATE_MICROPHONE_NAME") == std::string::npos, "microphone name leaked");
    }
    if (which == "model-env") {
        CHECK(changed.find("DLSS model: ") == std::string::npos && pc.find("DLSS model: Preset M") != std::string::npos,
              "environment-dependent model default invented");
    }
    if (which == "model-fixed") {
        CHECK(changed.find("DLSS model: Transformer 2 (M) (default: Transformer (K))") != std::string::npos,
              "fixed model default not retained without override");
    }
    if (pending_restart) CHECK(pc.find("takes effect after a restart") != std::string::npos, "restart annotation lost");
    if (failures) std::printf("CHANGED\n%sDEPENDS ON PC\n%s", changed.c_str(), pc.c_str());
    else std::printf("PASS %s\n", case_name);
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    cases = ("aspect-default", "reordered-shapes", "aspect-changed", "aspect-changed-wide", "output-context", "render-context",
             "upscaler-context", "auto-profile", "custom-never-auto", "custom-after-auto", "framegen-by", "lang-saved",
             "lang-changed", "stable-sentinels", "model-env", "model-fixed")
    failed = []
    with tempfile.TemporaryDirectory(prefix="report-defaults-") as tmp:
        cpp = Path(tmp) / "defaults.cpp"
        exe = Path(tmp) / ("defaults.exe" if os.name == "nt" else "defaults")
        cpp.write_text(fixture(args.source), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        "-I", str(args.source), str(cpp), "-o", str(exe)], check=True)
        for case in cases:
            env = {key: value for key, value in os.environ.items() if not key.startswith(("HOS_", "RX_", "HOOKSHOT_"))}
            env["HOS_DISPLAY_MODES"] = "1920x1080,5120x1440"
            if case in ("output-context", "aspect-changed-wide"): env["HOS_DISPLAY_MODES"] = "1920x1080,3440x1440,3840x2160"
            if case == "reordered-shapes": env["HOS_DISPLAY_MODES"] = "1280x1024,1600x1200,1920x1080,3440x1440,5120x1440"
            if case == "model-env": env["RX_VK_DLSS_PRESET"] = "M"
            result = subprocess.run([str(exe), case], cwd=tmp, env=env, text=True, capture_output=True, timeout=30)
            print(result.stdout, end="")
            if result.stderr: print(result.stderr, end="")
            if result.returncode: failed.append(case)
    print("report defaults: %d cases, %d failed%s" % (len(cases), len(failed), ": " + ", ".join(failed) if failed else ""))
    if failed: raise SystemExit(1)


if __name__ == "__main__":
    main()
