"""An environment switch over a menu row lasts for its run: hookshot_options.ini keeps the player's value.

RX_VK_FRAMEGEN_MULT, RX_VK_PACE, RX_VK_UPSCALE, HOS_REALTIME_STEP, HOS_DYNRES and HOS_OUTPUT_RES are applied to the
rows in menu_init. They used to be copied into the rows as if chosen, so the next save (any row changed in the menu)
wrote them into the player's options for good. Compile the menu's real save() and the overrides' part of menu_init with
small stand-ins and check what the file gets. On Linux, a save that cannot write a byte leaves the file as it was and no
part file, and an options file that is a link stays one, also to a file not made yet. No game or GPU is needed.
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
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>   // (save() locks since the control bridge, #926)
#include <string>
#include "hookshot_env.h"   // (gpu/vk/compat: env_value_off, the switches' rule)
#include "hos_wide_size.h"   // (host/: the output sizes the menu accepts, hos_output_size_valid)
#include "display_modes.h"   // (gpu/vk/: the sizes the Output resolution offers)
using namespace mkwvk;
// (the screen's display modes: HOS_DISPLAY_MODES names them here)
std::vector<display_modes::Size> screen_modes() { return {}; }
void outputs_sync() {}   // (the rows' ends and the Aspect ratio from the sizes read: not what this checks)
// (the main screen: not known here, so no shape is the monitor's and ultrawide= is saved as read)
bool screen_size(uint32_t*, uint32_t*) { return false; }
bool host_screen_size(uint32_t*, uint32_t*) { return false; }
std::string monitor_shape() { return {}; }
int ultrawide_read = 1, aspect_started = -1;
bool aspect_follows_host(int) { return false; }
#ifdef _WIN32
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // (save(): MoveFileExA)
#endif
#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
static int logged;   // (save()'s line when the options file could not be written)
void logf(const char*, ...) { logged++; }

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
/* OUTPUTS */
const char* tx(int) { return ""; }
Row rows[] = {
    {TAB_GRAPHICS, "Frame generation", &opts.framegen, 0, 8, 1, tx, ""},
    {TAB_GRAPHICS, "Frame pacing", &opts.pace, 0, 1, 1, tx, ""},
    {TAB_GRAPHICS, "Output resolution", &opts.output_res, 0, 0, 1, tx, ""},   // (its end: the sizes offered, unused here)
    {TAB_GRAPHICS, "Render resolution", &opts.resolution, 0, 6, 1, tx, ""},
    {TAB_GRAPHICS, "Window scaling filter", &opts.upscale, 0, 2, 1, tx, ""},
    {TAB_GRAPHICS, "Sharpness", &opts.sharpness, 0, 100, 10, tx, ""},
    {TAB_GAMEPLAY, "Game speed", &opts.realtime, 0, 1, 1, tx, ""},
};
std::string ini_file;
std::string ini_path() { return ini_file; }
std::string mic_want;
uint64_t auto_for = 0, auto_tuned = 0;   // (the Quality preset Auto (this PC)'s fingerprints, which save() writes)
struct { uint32_t w = 0, h = 0; } auto_output;   // (and the Output resolution it set, by its size)
struct { int steps = 0; bool went_down = false; } auto_tuning;   // (and its tuning's steps)
constexpr int HOS_KEY_N = 1, HOS_SW_N = 1, HOS_BIND_PAD = 0, HOS_BIND_KEY = 1;
constexpr int HOS_KEY_OC_NOTE_L = 1, HOS_KEY_OC_NOTE_X = 1;   /* (the note rows: past this stub's one row) */
const char* const bind_names[1] = {"a"};
struct Host { int (*binding)(int, int, int); } host_value{nullptr}, *host = &host_value;
bool can_bind() { return false; }
/* INI_KEY */
/* SAVE */
void env_part() {
/* ENV */
}
std::string slurp() {
    std::string text;
    if (FILE* f = std::fopen(ini_file.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) text += line;
        std::fclose(f);
    }
    return text;
}
bool has(const char* line) { return slurp().find(line) != std::string::npos; }

int main(int argc, char** argv) {
    CHECK(argc == 2);
    ini_file = argv[1];
    setenv("HOS_DISPLAY_MODES", "1920x1080,2560x1440", 1);   // (the screen's sizes: the game's own, these, 16:9's common ones)
    // The player's own: pace off, Per frame, 75%, Bilinear, 3x, a size of their own
    opts.pace = 0, opts.realtime = 0, opts.resolution = 2, opts.upscale = 0, opts.framegen = 3;
    opts.output_res = parse_output("3000x1688");
    CHECK(opts.output_res == custom_index() && custom_index() == 7);   // (2048x1152, 1280x720 to 5120x2880, then the player's)
    setenv("RX_VK_PACE", "1", 1);
    setenv("HOS_REALTIME_STEP", "1", 1);
    setenv("HOS_DYNRES", "1", 1);
    setenv("RX_VK_UPSCALE", "2", 1);
    setenv("RX_VK_FRAMEGEN_MULT", "4", 1);
    setenv("HOS_OUTPUT_RES", "2400x1350", 1);
    setenv("RX_VK_ASTC_RGBA8", "1", 1);
    env_part();
    CHECK(opts.astc_rgba8 == 1);
    // This run follows the environment
    CHECK(opts.pace == 1 && opts.realtime == 1 && opts.resolution == 0 && opts.upscale == 2 && opts.framegen == 4);
    CHECK(output_at(opts.output_res).w == 2400 && output_at(opts.output_res).h == 1344);
    // An unrelated row changed and saved: the file keeps the player's values
    opts.sharpness = 40;
    save();
    CHECK(has("sharpness=40\n"));
    CHECK(has("frame_pacing=0\n"));
    CHECK(has("game_speed=0\n"));
    CHECK(has("resolution=2\n"));
    CHECK(has("upscaling=0\n"));
    CHECK(has("frame_generation=3\n"));
    CHECK(has("output_res=3000x1688\n"));
    CHECK(opts.pace == 1 && opts.framegen == 4);   // (and the run still has the environment's)
    // The player changes an overridden row: theirs from then on, also when it comes back to the environment's value
    opts.pace = 0;
    save();
    CHECK(has("frame_pacing=0\n"));
    opts.pace = 1;
    save();
    CHECK(has("frame_pacing=1\n"));
    opts.framegen = 2;
    save();
    CHECK(has("frame_generation=2\n"));
    CHECK(has("upscaling=0\n"));   // (the others still the player's)
    opts.output_res = parse_output("2560x1440");
    CHECK(opts.output_res == 3 && output_at(3).w == 2560 && output_at(3).h == 1440);
    save();
    CHECK(has("output_res=2560x1440\n"));
    // A preset (or the Upscaler's snap) sets an overridden row to the very value the environment forced: the menu's, saved
    CHECK(has("game_speed=0\n"));
    env_release(&opts.realtime);
    save();
    CHECK(has("game_speed=1\n"));
    CHECK(!logged && !std::fopen((ini_file + ".new").c_str(), "r"));
#ifdef __linux__
    // A save that cannot be written (a full disk: no byte may be written here): the options file as it was, no part file left
    const std::string before = slurp();
    rlimit was{};
    getrlimit(RLIMIT_FSIZE, &was);
    rlimit none = was;
    none.rlim_cur = 0;
    std::signal(SIGXFSZ, SIG_IGN);
    CHECK(setrlimit(RLIMIT_FSIZE, &none) == 0);
    opts.sharpness = 70;
    save();
    setrlimit(RLIMIT_FSIZE, &was);
    CHECK(!before.empty() && slurp() == before && logged == 1);
    CHECK(!std::fopen((ini_file + ".new").c_str(), "r"));
    save();   // (and the next save that can write does)
    CHECK(has("sharpness=70\n"));
    // The options file a link: its target written and the link kept, also when the target is not made yet
    const std::string target = ini_file + ".target";
    std::remove(ini_file.c_str());
    CHECK(symlink(target.c_str(), ini_file.c_str()) == 0);
    struct stat st;
    for (int sharpness : {60, 50}) {   // (the target missing, then there)
        opts.sharpness = sharpness;
        save();
        CHECK(lstat(ini_file.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
        CHECK(has(("sharpness=" + std::to_string(sharpness) + "\n").c_str()) && logged == 1);
        CHECK(!std::fopen((ini_file + ".new").c_str(), "r") && !std::fopen((target + ".new").c_str(), "r"));
    }
#endif
    std::puts("PASS: the environment's rows last for the run; the options file keeps the player's, also when a save fails");
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
    outputs_from = menu.index("struct Out {")
    outputs_to = menu.index("dm::Size size_of(Out o)", outputs_from)
    helpers_from = menu.index("struct EnvOverride {")
    save = definition(menu, "void save")
    init = definition(menu, "void menu_init")
    env_from = init.index("// (the environment over the saved rows")
    env_to = init.index("restart_init();", env_from)
    env = init[env_from:env_to]
    for name in ("RX_VK_FRAMEGEN_MULT", "RX_VK_PACE", "RX_VK_UPSCALE", "HOS_REALTIME_STEP", "HOS_DYNRES", "HOS_OUTPUT_RES",
                 "RX_VK_ASTC_RGBA8"):
        if '"%s"' % name not in env:
            raise SystemExit("menu_init no longer reads %s with the other overrides" % name)
    if re.search(r"opts\.\w+\s*=", env):
        raise SystemExit("an environment override is copied into a row as if chosen: use env_override")
    replacements = {
        "/* OPTIONS */": options.group(0),
        "/* TABS */": tabs.group(0),
        "/* ROW */": row.group(0),
        "/* OUTPUTS */": menu[outputs_from:outputs_to],
        "/* INI_KEY */": definition(menu, "const char* ini_key"),
        "/* SAVE */": menu[helpers_from:menu.index("std::string ini_path() {")] + save,
        "/* ENV */": env,
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
    with tempfile.TemporaryDirectory(prefix="menu-env-") as tmp:
        src = Path(tmp) / "menu_env.cpp"
        exe = Path(tmp) / ("menu_env.exe" if os.name == "nt" else "menu_env")
        src.write_text(fixture(args.source), encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(args.source / "host"), "/I" + str(args.source / "gpu" / "vk" / "compat"), "/I" + str(args.source / "gpu" / "vk"), str(src),
                       "/Fo" + str(Path(tmp) / "menu_env.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(args.source / "host"),
                       "-I", str(args.source / "gpu" / "vk" / "compat"), "-I", str(args.source / "gpu" / "vk"),
                       str(src), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        subprocess.run([str(exe), str(Path(tmp) / "hookshot_options.ini")], check=True)


if __name__ == "__main__":
    main()
