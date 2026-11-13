"""The Insert menu's Audio output (Auto, Stereo, 5.1) reaches the speakers the game is told about (host/nn_audio.c).

The real au_speakers and au_menu_output from host/nn_audio.c and the menu's ini_key, tx_audio_output and hn_audio_output
from gpu/vk/vk_menu.cpp are compiled together: the fixture saves the row's choice under the key the menu would write, then
asks the host. No device or game is opened: the default output's channel count is a parameter of each case.

python -I -B tests/test_audio_output_menu.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using DWORD = unsigned long;
constexpr int MAX_PATH = 260;
static DWORD hos_game_path_a(char *path, DWORD size) { return snprintf(path, size, "./hookshot.exe"); }
static char logged[256];
static void hos_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(logged, sizeof logged, format, args);
    va_end(args);
}
static void au_lock(void) {}
static void au_unlock(void) {}
static int device_channels, device_asked;
static int au_device_channels(void) { device_asked++; return device_channels; }
HOST
struct Row { int tab; const char* name; int* value; };
struct { int output_res, vsync, resolution, fps_cap, upscale, dlss, handheld, ultrawide, audio_output; } opts;   // (ultrawide: the Aspect ratio row's key)
MENU
int main(int argc, char **argv) {
    assert(argc == 7);
    const char *saved = argv[2];
    device_channels = atoi(argv[3]);
    const int want = atoi(argv[4]), want_asked = atoi(argv[5]);
    /* (plain values: the menu adds " (after restart)" to a restart row's label itself) */
    assert(!strcmp(tx_audio_output(0), "Auto") && !strcmp(tx_audio_output(1), "Stereo"));
    assert(!strcmp(tx_audio_output(2), "5.1") && !strcmp(tx_audio_output(3), "Auto") && !strcmp(tx_audio_output(-1), "Auto"));
    for (int v = 0; v <= 2; v++) assert(hn_audio_output(v, -1) && hn_audio_output(v, 0) && !hn_audio_output(v, 1));
    if (strcmp(saved, "-")) {   /* (the lines as the menu's save writes them: the row's key, around others) */
        const Row row = {1, argv[1], &opts.audio_output};
        FILE *f = fopen("hookshot_options.ini", "w");
        assert(f);
        fprintf(f, "output_res=1\n%s=%s\nmicrophone=1\noptions_version=3\n", ini_key(row), saved);
        fclose(f);
    }
    const int n = au_speakers();
    assert(au_speakers() == n);   /* (asked once: the second answer is the first) */
    if (n != want || device_asked != want_asked || !strstr(logged, argv[6])) {
        fprintf(stderr, "FAIL: saved %s, device %d: %d speakers (want %d), device asked %d (want %d), log \"%s\" (want \"%s\")\n",
                saved, device_channels, n, want, device_asked, want_asked, logged, argv[6]);
        return 1;
    }
    return 0;
}
"""

# (saved audio_output or "-" for no hookshot_options.ini, HOS_AUDIO_CHANNELS or None, the default output's channels,
#  the speakers the game is told, whether the device was asked, what the log says)
CASES = (
    ("-", None, 6, 6, 1, "the default output has 6 channels"),
    ("-", None, 2, 2, 1, "the default output has 2 channels"),
    ("0", None, 8, 6, 1, "the default output has 8 channels"),
    ("0", None, 2, 2, 1, "the default output has 2 channels"),
    ("1", None, 6, 2, 0, "stereo for the game (the Insert menu's Audio output)"),
    ("2", None, 2, 6, 0, "5.1 for the game (the Insert menu's Audio output)"),
    ("3", None, 2, 6, 0, "5.1 for the game (the Insert menu's Audio output)"),   # (the menu shows a hand-edited 3 as 5.1)
    ("-1", None, 6, 6, 1, "the default output has 6 channels"),
    ("2", "2", 6, 2, 0, "stereo for the game (HOS_AUDIO_CHANNELS)"),
    ("1", "6", 2, 6, 0, "5.1 for the game (HOS_AUDIO_CHANNELS)"),
    ("-", "6", 2, 6, 0, "5.1 for the game (HOS_AUDIO_CHANNELS)"),
)


def between(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    audio = (ROOT / "host/nn_audio.c").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    # (tab, name, value, lo, hi, step, text, help, then hint, restart: read once at the start, preset_exempt)
    row = re.search(r'\{TAB_(\w+), "([^"]+)", &opts\.audio_output, (-?\d+), (-?\d+), (\d+), (\w+),\s*"[^{}]*?", '
                    r'(\w+), (true|false), (true|false)\},', menu)
    assert row, "the Insert menu has no Audio output row"
    tab, name, lo, hi, step, text, hint, restart, exempt = row.groups()
    assert (tab, lo, hi, step, text, hint, restart, exempt) == \
        ("AUDIO", "0", "2", "1", "tx_audio_output", "hn_audio_output", "true", "true"), row.groups()
    host = between(audio, "/* (hookshot) The Insert menu's Audio output", "/* The device's format,")
    hints = re.search(r'MENU_HINTS\(hn_audio_output,.*?\)\n(?=\S)', menu, re.S)
    assert hints, "no MENU_HINTS(hn_audio_output, ...)"
    menu_part = between(menu, "/// A row's hints from a table", "const char* tx_framegen(") + hints.group(0) + \
        between(menu, "const char* tx_audio_output(int v) {", "const char* tx_robust(") + \
        between(menu, "const char* ini_key(const Row& r) {", "// (hookshot) the controls, kept by the host")
    with tempfile.TemporaryDirectory(prefix="audio-output-menu-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(FIXTURE.replace("HOST", host).replace("MENU", menu_part), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)], check=True)
        for saved, env_channels, device, want, asked, log in CASES:
            ini = work / "hookshot_options.ini"
            if ini.exists():
                ini.unlink()
            env = os.environ.copy()
            env.pop("HOS_AUDIO_CHANNELS", None)
            if env_channels is not None:
                env["HOS_AUDIO_CHANNELS"] = env_channels
            subprocess.run([str(exe), name, saved, str(device), str(want), str(asked), log], cwd=work, env=env, check=True)
    print("PASS: Audio output: a restart row; Auto follows the device, Stereo and 5.1 are told to the game, HOS_AUDIO_CHANNELS "
          "over them")


if __name__ == "__main__":
    main()
