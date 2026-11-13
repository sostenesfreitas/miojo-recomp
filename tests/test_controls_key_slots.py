"""The Insert menu's Controls page gives each control two key cells, Key 1 and Key 2, one for each of the host's two key slots
(issue #2885). Binding or clearing one cell changes its own slot and leaves the other key: before, a new key went into the first
slot and cleared the second, so a control bound by default to two keys kept one after any rebind, and no second key could be
given back. The page's key branches are cut from gpu/vk/vk_menu.cpp and compiled against a host that records the bindings.

    python -I -B tests/test_controls_key_slots.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    """From marker to the end of the brace block it opens"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def cut(menu):
    page = menu[menu.index("} else if (open_now.load() && page == 1) {"):]
    page = page[:page.index("} else if (open_now.load() && page == 4) {")]
    listen = block(page, "else if (down) {")
    column = re.search(r"\n\s*(const int col = [^;]*;)", page)
    if not column:
        raise SystemExit("FAIL: vk_menu.cpp: the Controls page does not choose the selected column")
    clear = block(page, "if ((press[8] || press[9]) && i < kControls) {")
    return listen, column.group(1), clear


HARNESS = r'''
#include "vk_menu_hit.h"
#include <cstdio>
namespace menu_hit = mkwvk::menu_hit;   // (as vk_menu.cpp sees it: the column names, and the helpers only by their namespace)
using enum menu_hit::CtlColumn;
enum { HOS_BIND_PAD, HOS_BIND_KEY };
struct Host {
    void (*bind)(int set, int button, int slot, int code);
};
static int keys[3][2], pads[3][2];
static void record(int set, int button, int slot, int code) { (set == HOS_BIND_KEY ? keys : pads)[button][slot] = code; }
static Host the_host = {record};
static Host* host = &the_host;
struct Control { int button; };
static const Control controls[] = {{0}, {1}, {2}};
static const int kTop = 1, kControls = 3;
static bool keys_only(int i) { return i == 2; }
static int csel, ccol, listening;
static bool changed, press[16];
static struct { int pad_layout; } opts;
static int detect_pad_layout() { return 0; }
static void save() {}

static void bind_key(int down) {
    if (!down) return;
    @LISTEN@
}
static void clear_cell(int i) {
    @COLUMN@
    (void)col;
    @CLEAR@
}

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s\n", name); return 1; } } while (0)

static void start(int i, int col) {
    for (auto& k : keys) k[0] = 'W', k[1] = 'S';
    csel = kTop + i, ccol = col, press[8] = false;
}

int main() {
    start(0, kCtlKeyboard2);
    bind_key('X');
    CHECK("Key 2 bound, Key 1 kept", keys[0][0] == 'W' && keys[0][1] == 'X');
    start(0, kCtlKeyboard);
    bind_key('X');
    CHECK("Key 1 bound, Key 2 kept", keys[0][0] == 'X' && keys[0][1] == 'S');
    start(2, kCtlPrimary);
    bind_key('X');
    CHECK("a keys-only row from a gamepad column binds Key 1", keys[2][0] == 'X' && keys[2][1] == 'S');
    start(1, kCtlKeyboard2);
    press[8] = true;
    clear_cell(1);
    CHECK("Key 2 cleared, Key 1 kept", keys[1][0] == 'W' && keys[1][1] == 0);
    start(1, kCtlKeyboard);
    press[8] = true;
    clear_cell(1);
    CHECK("Key 1 cleared, Key 2 kept", keys[1][0] == 0 && keys[1][1] == 'S');
    start(1, kCtlAlternate);
    pads[1][1] = 5;
    press[8] = true;
    clear_cell(1);
    CHECK("a gamepad cell's clear leaves the keys", pads[1][1] == -1 && keys[1][0] == 'W' && keys[1][1] == 'S');
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    listen, column, clear = cut(menu)
    src = HARNESS.replace("@LISTEN@", listen).replace("@COLUMN@", column).replace("@CLEAR@", clear)
    with tempfile.TemporaryDirectory(prefix="key-slots-") as tmp:
        c, exe = Path(tmp) / "key_slots.cpp", Path(tmp) / "key-slots"
        c.write_text(src, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(ROOT / "gpu" / "vk"),
                        str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: a control's two keys are bound and cleared one at a time")


if __name__ == "__main__":
    main()
