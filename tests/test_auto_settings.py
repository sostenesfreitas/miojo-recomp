"""Compile and run the Quality preset Auto (this PC)'s rules (gpu/vk/auto_settings.h; no game/GPU), and check that the menu and
the renderer still use them: the preset row reaches Auto, a first start starts on it, and the hardware is read once the window is
known.

python -I -B tests/test_auto_settings.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def body(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    need("vk_menu.cpp", menu, ['{TAB_GRAPHICS, "Quality preset", &opts.preset, 0, 6, 1, tx_preset,', '"Auto (this PC)"',
                               "if (first) opts.preset = kAutoPreset;",
                               "::auto_settings::choose(hw)", "if (fp == auto_for)", 'fprintf(f, "auto_for=%016llx\\n"',
                               # (save() keeps the lines it does not write: each auto_ key must be one it writes, or the old
                               # line, kept after the new one, wins at the next start)
                               '|| key == "memory_budget" || key == "auto_for" ||',
                               'key == "auto_tuned" || key == "auto_output" || key == "auto_steps";',
                               # (the measured tuning in play: from the replay's windows, finished once for this PC, its steps
                               # kept across restarts)
                               "::auto_settings::tune(tuning,", 'fprintf(f, "auto_tuned=%016llx\\n"', 'fprintf(f, "auto_steps=%d,%d\\n"',
                               'if (!strcmp(line, "auto_steps"))', 'fprintf(f, "auto_output=%ux%u\\n"',
                               # (the hardware changed: the exempt rows the player set under Auto stay theirs)
                               "apply_auto(!first);", "apply_auto(false);"])
    need("vk_menu.h", (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8"), ["constexpr int kAutoPreset = 6;"])
    if "constexpr int kAutoPreset" in menu:
        raise SystemExit("vk_menu.cpp defines kAutoPreset again: vk_menu.h has it")
    # (the upscaler set before the tier's rows: apply_preset snaps the Render resolution to it)
    auto = body(menu, "void apply_auto(bool keep_own) {")
    if not auto.index("opts.dlss = c.upscaler;") < auto.index("apply_preset(c.tier);"):
        raise SystemExit("apply_auto: opts.dlss must be set before apply_preset")
    # (the Output resolution Auto set is kept by its size: the choices' numbers follow the screen's modes, #2566)
    need("apply_auto", auto, ["auto_output.w = o.w, auto_output.h = o.h;",
                              "!(set.w == auto_output.w && set.h == auto_output.h)"])
    # (the CPU holding the frame rate ends the tuning for this run only: no auto_tuned for it)
    tune = body(menu, "void menu_auto_tune(double gpu_ms, double draws, double fps) {")
    need("menu_auto_tune", tune, ["::auto_settings::resolution_for(c, ur::engine(u), v,"])
    cpu = tune[tune.index("verdict == ::auto_settings::CPU_BOUND) {"):]
    cpu = cpu[:cpu.index("} else {")]
    if "auto_tuned" in cpu or "return;" not in cpu:
        raise SystemExit("menu_auto_tune: the CPU-bound verdict must end this run's tuning without auto_tuned")
    # (the tiers' own Render resolution in auto_settings.h is apply_preset's)
    preset = body(menu, "void apply_preset(int p) {")
    own = {int(n): int(r) for n, r in re.findall(r"case (\d): //[^\n]*\n\s*opts\.resolution = (\d);", preset)}
    rules = (ROOT / "gpu/vk/auto_settings.h").read_text(encoding="utf-8")
    table = re.search(r"constexpr int kTierResolution\[\] = \{([^}]*)\};", rules)
    if not table or len(own) != 5:
        raise SystemExit("auto_settings.h kTierResolution or apply_preset's cases not found")
    values = [int(x) for x in table.group(1).split(",")]
    if any(values[p] != own[p] for p in own):
        raise SystemExit("auto_settings.h kTierResolution %s is not apply_preset's %s" % (values, own))
    main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    need("vk_main.cpp", main, ["auto_profile();   // (the Quality preset Auto (this PC)", "menu_auto_for_gpu(p, hw);",
                               "hw.mesh_native = dev.mesh_capable;", "hw.mesh_as_compute = dev.mesh_capable && !dev.mesh_shader;"])
    # (the card's mesh shaders from its feature query, before the Mesh shaders mode turns them off for this run)
    if not main.index("dev.mesh_capable = m.meshShader && m.taskShader;") < main.index(
            "if (mesh_mode_used == 2) dev.mesh_shader = false;"):
        raise SystemExit("vk_main.cpp: dev.mesh_capable must be read before the Mesh shaders mode is applied")
    need("vk_replay.cpp", (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8"), ["menu_auto_tune(gpu_ms, draws, 120.0 / secs);"])
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    need("vk_api.h", api, ["#define MKWVK_WINDOW_VERSION 3", "void (*desktop_size)(void *window, int *width, int *height);"])
    host = (ROOT / "host/posix/display_sdl.c").read_text(encoding="utf-8")
    need("host/posix/display_sdl.c", host, ["refresh_mhz, desktop_size};"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="auto-settings-") as tmp:
        exe = Path(tmp) / "auto-settings"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_auto_settings.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
