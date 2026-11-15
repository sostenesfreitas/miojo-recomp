"""Damage taken belongs beside Heart protection in Debug's Player group, not Developer.

Compile the real Debug rows, visibility, group order and keyboard traversal without a GPU.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    table = block(menu, "Row rows[] = {")
    entries = re.findall(r'\{TAB_DEBUG,[^{}]*\}', table)
    assert len(entries) >= 18
    assert '{TAB_GAMEPLAY, "Damage taken"' not in table
    assert sum('"Damage taken"' in entry for entry in entries) == 1
    source = '#include <cassert>\n#include <cstring>\n#include <iterator>\n'
    source += block(header, "struct Options {") + ';\nOptions opts;\n'
    source += block(menu, "enum MenuTab {") + ';\n'
    source += block(menu, "struct Row {") + ';\n'
    for name in ("tx_onoff", "tx_damage_taken", "tx_robust", "tx_intel_compiles"):
        source += block(menu, "const char* " + name + "(") + '\n'
    # Help text is not used by this navigation fixture.
    source += 'const char* hn_robust(int, int) { return nullptr; }\n'
    source += 'const char* hn_astc_rgba8(int, int) { return nullptr; }\n'
    source += 'const char* hn_intel_compiles(int, int) { return nullptr; }\n'
    source += 'Row rows[] = {' + ',\n'.join(entries) + '};\n'
    source += 'constexpr int kRows = sizeof rows / sizeof rows[0];\n'
    source += '''
bool cheats_env = false;
bool reflex_available() { return false; }
bool antilag_available() { return false; }
bool intel_compiler_row() { return false; }   // (not Intel graphics on Windows: the Developer section's rows as before)
bool laptop_row() { return false; }   // (a dedicated card: no Set up for a weak laptop button)
int upscalers_available() { return 1; }
constexpr int DLSS_OK = 0;
int dlss_why() { return 1; }
namespace ur {
bool upscaler_shown(int n) { return n > 1; }
bool model_shown(bool available) { return available; }
}
'''
    for name in ("bool cheats_on(", "bool is_laptop_row(", "bool row_shown(", "int tab_row_count(", "int tab_row_to_global(",
                 "int menu_damage_taken("):
        source += block(menu, name) + '\n'
    source += menu[menu.index("struct DebugGroup {"):menu.index("/// (hookshot) The Debug tab's rows from top down")]
    source += r'''
int main() {
    int order[64], dev, heading;
    const char* expected[] = {"Debug tools", "Heart protection", "Damage taken", "No-clip flight",
        "Movement speed", "Switch age", "Inventory", "Infinite ammo", "Infinite rupees", "Infinite magic", "Moon jump", "Teleport", "Mark position", "Return to mark",
        "Free camera", "Set time", "Lock time"};
    for (int env = 0; env < 2; ++env) for (int enabled = 0; enabled < 2; ++enabled) {
        cheats_env = env;
        opts.debug_tools = enabled;
        opts.damage_taken = 3;
        const int n = debug_rows(order, &dev, &heading);
        assert(heading >= 0);
        assert(dev == (env || enabled ? (int)std::size(expected) : 1));
        assert(n == dev + 5); // Only the existing Developer rows follow.
        for (int i = 0; i < dev; ++i) {
            const Row& row = rows[tab_row_to_global(TAB_DEBUG, order[i])];
            assert(!std::strcmp(row.name, expected[i]));
            assert(debug_step(order[i], 1) == order[i + 1]);
            assert(debug_step(order[i + 1], -1) == order[i]);
            if (i == 2) {
                assert(row.value == &opts.damage_taken && row.cheat == -1);
                assert(row.lo == 0 && row.hi == 3 && row.step == 1);
                assert(!row.restart && row.preset_exempt);
                assert(!std::strcmp(row.text(1), "Normal"));
                assert(!std::strcmp(row.text(3), "Quadruple"));
            }
        }
        assert(debug_step(order[n - 1], 1) == order[0]);
        assert(debug_step(order[0], -1) == order[n - 1]);
        assert(opts.damage_taken == 3); // Hiding the tools must not reset the saved option.
        assert(menu_damage_taken() == (env || enabled ? 3 : 1)); // Hidden means Normal for the host.
    }
    assert(kDebugGroups == 3 && kDebugTools == 16);   // (miojo: Infinite ammo, Infinite rupees, Infinite magic, Moon jump in Player; Lock time in Camera & world)
    assert(!std::strcmp(debug_groups[0].title, "Player"));
    assert(debug_groups[0].from == 0 && debug_groups[0].to == 10);
    assert(debug_groups[1].from == 10 && debug_groups[1].to == 13);
    assert(debug_groups[2].from == 13 && debug_groups[2].to == 16);
}
'''
    with tempfile.TemporaryDirectory(prefix="damage-debug-menu-") as tmp:
        cpp = Path(tmp) / "menu.cpp"
        exe = Path(tmp) / "menu.exe"
        cpp.write_text(source, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: Debug Player order, visibility, choices, default and keyboard traversal")


if __name__ == "__main__":
    main()
