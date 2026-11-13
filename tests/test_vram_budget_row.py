"""The VRAM budget is a menu row of its own: a preset sets it, and a budget chosen on it is the player's on any card.

The texture and mirror budgets came from the Quality preset alone (#1358 keeps them when another row makes the preset
Custom), with no way to choose a limit (Discord #dev-work, 2026-11-10). The VRAM budget row holds them now. This builds
the menu's own functions from vk_menu.cpp and preset_caps_memory from vk_memory.cpp and checks: each preset gives the
budgets it gave before, the row's values, and that a budget chosen on the row (opts.budget 0) applies on any card while a
preset's own still applies only on the card it is for. Source checks: the row-change handler clears opts.budget and sets
the budgets from the row, the row leaves the preset alone, and an options file from before the row takes its preset's.
A restart keeps a budget chosen on the row with a preset on: save() writes memory_budget=0 for it, and the preset's only
comes back for a file with no memory_budget line at all (the fixture saves and loads with vk_menu.cpp's own lines).
python -I -B tests/test_vram_budget_row.py [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, start):
    """the definition starting at start, through its closing brace"""
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        depth += text[j] == "{"
        depth -= text[j] == "}"
        if depth == 0:
            return text[start:j + 1]
    raise SystemExit("no end of the definition at %d" % start)


def definition(text, signature):
    return block(text, text.index(signature))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    memory = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    fail = []

    # (source checks)
    handler = menu[menu.index("} else if (r.value == &opts.preset) {"):]
    handler = handler[:handler.index("if (r.value == &opts.mic")]
    if "sync_preset_budgets(0)" in handler:
        fail.append("changing a row (Custom) still resets the budgets")
    vb = handler[handler.index("if (r.value == &opts.vram_budget) {"):] if "if (r.value == &opts.vram_budget) {" in handler else ""
    if "opts.budget = 0;" not in vb[:200] or "vram_budget_sync();" not in vb[:200]:
        fail.append("changing the VRAM budget row does not make it the player's (opts.budget 0) and set the budgets")
    row = re.search(r'\{TAB_GRAPHICS, "VRAM budget", &opts\.vram_budget,.*?\},', menu, re.S)
    if not row or not row.group(0).rstrip("},").rstrip().endswith("hn_vram_budget, false, true"):
        fail.append("no VRAM budget row that leaves the preset alone (preset_exempt)")
    if "if (r.value == &opts.vram_budget && v == kVramMinimumOnly) v += dir;" not in menu:
        fail.append("the row's own steps do not go past 2.2 GB (the Minimum preset's only)")
    if '"2.2 GB (4GB preset)"' not in menu:
        fail.append("2.2 GB is not shown as the 4GB preset's")
    if "if (!has_vram_budget) opts.vram_budget = preset_vram_budget(opts.budget);" not in menu:
        fail.append("an options file from before the row does not take its preset's budgets")

    def line_of(needle):
        found = [l.strip() for l in menu.splitlines() if needle in l]
        if len(found) != 1:
            fail.append("vk_menu.cpp: not one line with " + needle)
            return ""
        return found[0]
    save_line = line_of('fprintf(f, "memory_budget=%d\\n", opts.budget);')
    parse_line = line_of('if (!strcmp(line, "memory_budget")) opts.budget =')
    fallback_line = line_of("if (opts.preset > 0 && !has_memory_budget) opts.budget = opts.preset;")
    row_fallback = line_of("if (!has_vram_budget) opts.vram_budget = preset_vram_budget(opts.budget);")
    if fail:
        print("FAIL: " + "\n".join(fail))
        sys.exit(1)

    options = re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)
    tables = menu[menu.index("constexpr int kVramBudgets"):menu.index("int preset_vram_budget")]
    fixture = r'''#include <algorithm>
#include <cstdint>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
OPTIONS
Options opts;
TABLES
OF_PRESET
SYNC
SYNC_PRESET
static uint64_t card_bytes = 16ull << 30;
uint64_t device_vram_bytes() { return card_bytes; }
CAPS
#include <cstring>
#include <cstdlib>
// (a restart: the preset row, the VRAM budget row and memory_budget as save() writes them, read back as menu_init does)
static void restart(bool write_memory_budget) {
    FILE* f = std::tmpfile();
    std::fprintf(f, "quality_preset=%d\nvram_budget=%d\n", opts.preset, opts.vram_budget);
    if (write_memory_budget) SAVE_LINE
    std::rewind(f);
    opts = Options{};
    bool has_vram_budget = false, has_memory_budget = false;
    char line[80];
    while (std::fgets(line, sizeof line, f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!std::strcmp(line, "quality_preset")) opts.preset = std::atoi(eq + 1);
        if (!std::strcmp(line, "vram_budget")) opts.vram_budget = std::atoi(eq + 1);
        PARSE_LINE
        has_vram_budget |= !std::strcmp(line, "vram_budget");
    }
    std::fclose(f);
    FALLBACK_LINE
    ROW_FALLBACK
    vram_budget_sync();
}
int main() {
    (void)kVramMinimumOnly;   // (TABLES carries it, the row's step that reads it does not come along: clang's -Wunused-const-variable)
    // (each preset's budgets as before the row: 2200/256, 3600/512, 4800/1024, 7500/2048, 11000/3072)
    const int texture[] = {0, 2200, 3600, 4800, 7500, 11000}, mirror[] = {0, 256, 512, 1024, 2048, 3072};
    for (int p = 1; p <= 5; p++) {
        opts.preset = opts.budget = p;
        sync_preset_budgets(p);
        CHECK(opts.vram_mb == texture[p] && opts.mirror_mb == mirror[p]);
    }
    opts.budget = 0;
    sync_preset_budgets(0);   // (Custom chosen on the preset row: Auto, as before)
    CHECK(opts.vram_budget == 0 && opts.vram_mb == 0 && opts.mirror_mb == 0);
    // (the row on its own: 4.8 GB)
    opts.vram_budget = 3;
    vram_budget_sync();
    CHECK(opts.vram_mb == 4800 && opts.mirror_mb == 1024);
    // (a preset's own budget applies only on a card it is for; one chosen on the row, opts.budget 0, on any card)
    card_bytes = 16ull << 30;
    opts.preset = opts.budget = 4;
    sync_preset_budgets(4);
    CHECK(!preset_caps_memory());   // (High's 7.5 GB on a 16 GB card: the card's own, as before)
    opts.budget = 0;                // (the row set by the player: the handler)
    opts.vram_budget = 3;
    vram_budget_sync();
    CHECK(preset_caps_memory());    // (4.8 GB chosen with High on: it applies)
    card_bytes = 12ull << 30;
    opts.budget = 4;
    sync_preset_budgets(4);
    CHECK(preset_caps_memory());    // (High on a 12 GB card: its own, as before)
    // (High on, 4.8 GB chosen on the row, a 16 GB card: still the player's after a restart)
    card_bytes = 16ull << 30;
    opts.preset = opts.budget = 4;
    sync_preset_budgets(4);
    opts.budget = 0;
    opts.vram_budget = 3;
    vram_budget_sync();
    restart(true);
    CHECK(opts.preset == 4 && opts.budget == 0 && opts.vram_budget == 3);
    CHECK(opts.vram_mb == 4800 && preset_caps_memory());
    // (a file from before memory_budget, High on: the preset's budgets, as before)
    opts.preset = 4;
    opts.vram_budget = 5;
    restart(false);
    CHECK(opts.budget == 4 && opts.vram_mb == 7500);
    std::printf("PASS: the VRAM budget row sets the budgets, the presets give theirs as before, a restart keeps the row's\n");
    return 0;
}
'''
    for key, value in (("OPTIONS", options), ("TABLES", tables),
                       ("OF_PRESET", definition(menu, "int preset_vram_budget(int p)") +
                        "\nint vram_budget_of_preset(int p) { return preset_vram_budget(p); }"),
                       ("SYNC_PRESET", definition(menu, "void sync_preset_budgets(int p)")),
                       ("SYNC", definition(menu, "void vram_budget_sync()")),
                       ("CAPS", definition(memory, "bool preset_caps_memory()")),
                       ("SAVE_LINE", save_line), ("PARSE_LINE", parse_line), ("FALLBACK_LINE", fallback_line),
                       ("ROW_FALLBACK", row_fallback)):
        fixture = fixture.replace(key, value, 1)
    with tempfile.TemporaryDirectory(prefix="vram-budget-row-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        built = subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
                               capture_output=True, text=True)
        if built.returncode:
            fail.append("the fixture does not build:\n" + built.stderr[-1500:])
        else:
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            if run.returncode:
                fail.append(run.stderr.strip() or "the fixture failed")
            else:
                print(run.stdout.strip())
    if fail:
        print("FAIL: " + "\n".join(fail))
        sys.exit(1)


if __name__ == "__main__":
    main()
