"""The Insert menu's Rumble strength row (Controls): Off, 25% to 150% in steps of 25, stopping at its ends.

gpu/vk/vk_menu.cpp's row, set_row (the arrows, Enter and a click on the value) and the ini's snap to the row's steps are
compiled from the source with stubs and run: one step below Off stays Off, one above 150% stays 150%, however it is
asked. The mouse wheel never changes a value (it scrolls the rows): menu_tick is checked for that. The option id the host reads it by (9, host/pad_sdl.c) is checked against the
renderer's option table (gpu/vk/vk_replay.cpp) and the other ids. Rumble changes do not reset temporal failures.

    python -I -B tests/test_rumble_menu.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MENU = ROOT / "gpu" / "vk" / "vk_menu.cpp"


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def block(text, at):
    """text from at to the end of the first {...} after it"""
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise SystemExit("no end for the block at %d" % at)


def definition(text, head):
    found = re.search(re.escape(head) + r"\([^;{]*\)\s*\{", text)
    if not found:
        raise SystemExit("vk_menu.cpp: no definition of " + head)
    return block(text, found.start())


def one_line(text, head):
    found = re.search(r"^" + re.escape(head) + r"\(.*\}\s*$", text, re.M)
    if not found:
        raise SystemExit("vk_menu.cpp: no " + head)
    return found.group(0)


FIXTURE = r"""
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %%d %%s\n", __LINE__, #x); return 1; } } while (0)
%(options)s
Options opts;
%(tabs)s
%(row)s
int saves = 0, presets = 0;
void save() { saves++; }
void apply_pad_layout(int) {}
void apply_preset(int) { presets++; }
void sync_preset_budgets(int) {}
void env_release(const int*) {}   // (the environment's hold on a row: none here)
void vram_budget_sync() {}
constexpr int kVramMinimumOnly = 1;
/* (stand-ins for the rest of set_row, which the Rumble strength row never reaches: the Upscaler's rows, the restart offer) */
bool row_fixed(const int*) { return false; }
int upscaler() { return 0; }
namespace ur {
int step_upscaler(int, int, bool) { return 0; }
int step(int, int v, int dir) { return v + dir; }
int snap(int, int v) { return v; }
int snap_render(int, int v) { return v; }   // (the slider)
int step_engine(int, int, unsigned) { return 0; }   // (the Upscaler by engine)
void step_render(int up, int v, int dir, int* u, int* r) { *u = up, *r = v + dir; }
}
namespace mr { constexpr int kMax = 8; }
/* (the Game language row's hook in set_row: language.txt, not written here) */
namespace i18n { inline void set_game_language(int) {} }
void restart_values(int*) {}
struct { void note(const int*) {} } restart_state;
%(is_slider)s
%(stops)s
%(snap)s
%(ini_key)s
/* (the Microphone row's hook in set_row: no microphones here) */
bool can_mic() { return false; }
/* (the Upscaler row's choices, which set_row steps through: the game's own only, here) */
unsigned upscalers_available() { return 1; }
void mic_chosen() {}
void samplers_changed() {}
int temporal_resets = 0;
void temporal_reset_failure() { ++temporal_resets; }
void prompts_at_once(const Row&) {}
// (set_row steps the Output row through vk_menu.cpp's step_output_choice (#1144): not what this checks)
int step_output_choice(int id, int dir) { return id + dir; }
// (the Aspect ratio row moves the Output resolution to one of its shape's sizes, output_for_aspect: not what this checks)
int output_for_aspect(int) { return opts.output_res; }
%(set_row)s
/* (menu_tick's arrows: Left, Right, and Enter as Right; a click on the value's left or right half presses them) */
void arrow(Row& r, bool right) { %(arrow)s }
int main() {
    Row r = {TAB_CONTROLS, "Rumble strength", &opts.rumble, %(lo)s, %(hi)s, %(step)s, nullptr, nullptr%(exempt)s};
    CHECK(r.lo == 0 && r.hi == 150 && r.step == 25);
    CHECK(opts.rumble == 100);
    CHECK(!std::strcmp(ini_key(r), "rumble_strength"));
    opts.preset = 3;
    /* the arrows (and clicks): Off stays Off, 150%% stays 150%% */
    opts.rumble = 0;
    arrow(r, false);
    CHECK(opts.rumble == 0);
    arrow(r, true);
    CHECK(opts.rumble == 25);
    for (int i = 0; i < 10; i++) arrow(r, true);
    CHECK(opts.rumble == 150);
    arrow(r, false);
    CHECK(opts.rumble == 125);
    for (int i = 0; i < 10; i++) arrow(r, false);
    CHECK(opts.rumble == 0);
    set_row(r, 175, 1);
    CHECK(opts.rumble == 150);
    set_row(r, -25, -1);
    CHECK(opts.rumble == 0);
    /* it is a control: the quality preset stays */
    CHECK(opts.preset == 3 && presets == 0 && saves > 0);
    CHECK(temporal_resets == 0);
    /* an ini's value: on the steps, within the range */
    static const int in[] = {-10, 0, 12, 13, 60, 63, 100, 137, 138, 150, 151, 999};
    static const int out[] = {0, 0, 0, 25, 50, 75, 100, 125, 150, 150, 150, 150};
    for (int i = 0; i < 12; i++) CHECK(rumble_step(in[i]) == out[i]);
    return 0;
}
"""


def wheel_free(menu):
    """the wheel scrolls the rows, the Controls page and the lists, and changes no value, selection or tab"""
    tick = menu[menu.index("void menu_tick() {"):]
    if not re.search(r"if \(ptr\.wheel && drag_row < 0\) wheel_rows\[cur_tab\] \+= ptr\.wheel, changed = true;", tick):
        raise SystemExit("FAIL: the wheel on the tabs does not scroll the rows")
    if "if (ptr.wheel) wheel_rows[kControlsSlot] += ptr.wheel" not in tick:
        raise SystemExit("FAIL: the wheel on the Controls page does not scroll it")
    lists = menu[menu.index("void inventory_mouse("):menu.index("void cheat_press(")]
    if ("inventory_page = std::max(0, inventory_page - ptr.wheel)" not in lists or "teleport_wheel += ptr.wheel" not in lists
            or re.search(r"(item_choice|teleport_choice)\s*=[^;]*ptr\.wheel", lists)):
        raise SystemExit("FAIL: the wheel on a list page does not scroll it, or moves the choice")
    for line in (menu[menu.index("void inventory_mouse("):]).splitlines():
        if "ptr.wheel" in line and re.search(r"set_row|press\[[34]\]|ask_sel|tab_left|tab_right|\bsel =|csel =", line):
            raise SystemExit("FAIL: the wheel still changes something: " + line.strip())
    scroll = menu[menu.index("float scroll_rows("):menu.index("void scroll_bar(")]
    if "if (n <= fit) want = 0;" not in scroll or "want -= (float)wheel;" not in scroll:
        raise SystemExit("FAIL: scroll_rows does not scroll by the wheel, or scrolls a list that fits")


def menu_fixture():
    menu = MENU.read_text(encoding="utf-8")
    header = read("gpu", "vk", "vk_menu.h")
    row = re.search(r'\{TAB_CONTROLS, "Rumble strength", &opts\.rumble, (-?\d+), (\d+), (\d+), tx_rumble,', menu)
    if not row:
        raise SystemExit("the menu lost its Rumble strength row")
    exempt = block(menu, row.start()).rstrip().endswith(", true}")   # (Row::preset_exempt: a control, not the preset's)
    load = menu.index("bool has_resolution = false;")   # (the ini's load, not save()'s read of its other lines)
    load = block(menu, menu.rindex('if (FILE* f = fopen(ini_path().c_str(), "r"))', 0, load))
    if "opts.rumble = rumble_step(opts.rumble);" not in load:
        raise SystemExit("the ini's Rumble strength is not put on the row's steps")
    wheel_free(menu)
    arrow = re.search(r"\n\s*(set_row\(r, \*r\.value \+ \(press\[4\] \? r\.step : -r\.step\), press\[4\] \? 1 : -1\);)", menu)
    if not arrow:
        raise SystemExit("the arrows no longer step a row with set_row")
    return FIXTURE % {
        "options": re.search(r"struct Options \{.*?\n\};", header, re.S).group(0),
        "tabs": re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0),
        "row": re.search(r"struct Row \{.*?\n\};", menu, re.S).group(0),
        "is_slider": one_line(menu, "bool is_slider"),
        "stops": one_line(menu, "bool stops_at_ends"),
        "snap": one_line(menu, "int rumble_step"),
        "ini_key": definition(menu, "const char* ini_key"),
        "set_row": definition(menu, "void set_row"),
        "arrow": "bool press[5] = {false, false, false, !right, right}; " + arrow.group(1),
        "exempt": ", nullptr, false, true" if exempt else "",   # (after the help: no hint, no restart)
        "lo": row.group(1), "hi": row.group(2), "step": row.group(3),
    }


def option_ids():
    """option 9 is the menu's Rumble strength, and no other option has its id"""
    replay, api = read("gpu", "vk", "vk_replay.cpp"), read("gpu", "vk", "vk_api.h")
    value = {}   # (every HOS_OPT_* enum: the cheats', the motion rows', DLSS's jitter and one-name ones as Text size's)
    for first, start, rest in re.findall(r"enum \{ (HOS_OPT_\w+) = (\d+)([^}]*)\}", api):
        names = [first] + [n.strip() for n in rest.split(",") if n.strip()]
        value.update({n: int(start) + i for i, n in enumerate(names)})
    table = block(replay, replay.index("t.option = [](int id) -> int {"))
    ids = []
    for name in re.findall(r"\bid == (\w+)", table):
        ids.append(int(name) if name.isdigit() else value[name])
    if len(ids) != len(set(ids)):
        raise SystemExit("vk_replay.cpp: an option id is read twice: %s" % sorted(ids))
    if not re.search(r"\bid == 9 \? opts\.rumble\b", table):
        raise SystemExit("vk_replay.cpp: option 9 is not the menu's Rumble strength")
    base = int(re.search(r"#define OOT_CHEAT_ITEM_BASE (\d+)", read("gpu", "vk", "oot_cheat_items.h")).group(1))
    if any(i >= base for i in ids):
        raise SystemExit("vk_replay.cpp: an option id is among the cheat items'")
    users = []
    for folder in ("host", "gpu"):
        for path in (ROOT / folder).rglob("*.c*"):
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"\brx_option(_set)?\(\s*9\s*,", text):
                users.append(path.relative_to(ROOT).as_posix())
    if users != ["host/pad_sdl.c"]:
        raise SystemExit("option 9 is read or set outside host/pad_sdl.c: %s" % users)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    option_ids()
    with tempfile.TemporaryDirectory(prefix="rumble-menu-") as tmp:
        src, exe = Path(tmp) / "rumble_menu.cpp", Path(tmp) / "rumble_menu"
        src.write_text(menu_fixture(), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the menu's Rumble strength stops at Off and 150%, and option 9 is its own")


if __name__ == "__main__":
    main()
