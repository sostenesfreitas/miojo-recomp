"""Test for the compact menu bar (bottom HUD bar).

Checks that:
1. vk_ui.h defines kCompact glyph and draws configurable RShift/RCtrl keycap or pad West button dot.
2. vk_menu.h exposes opts.compact_menu_key.
3. vk_menu.cpp implements configurable "Compact menu key" in Controls (Right Shift or Right Ctrl),
   draw_tune_bar, toggles on key release without modifier overlap (Option A) and tune key + Insert/F1 (Option B),
   draws the compact bar without full-screen dimming on page 0 while preserving dimming on subpages,
   and includes the dynamic Compact / Full menu prompts and Restart/Quit confirm prompts.
4. Cuts the real toggle release and dim conditions from vk_menu.cpp into a compiled C++ test fixture.

python -I -B tests/test_menu_tune.py
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()

    ui_h = (ROOT / "gpu/vk/vk_ui.h").read_text(encoding="utf-8")
    menu_h = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    menu_cpp = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")

    # 1. Glyph and prompt
    check("kCompact" in ui_h, "vk_ui.h: enum Glyph must include kCompact")
    check('pr.key ? pr.key : "RShift"' in ui_h, 'vk_ui.h: kCompact must draw dynamic keycap or RShift for keyboard')

    # 2. Configurable Compact menu key in Controls
    check("int compact_menu_key = 0;" in menu_h, "vk_menu.h: Options must declare compact_menu_key")
    check("const char* tx_compact_menu_key" in menu_cpp, "vk_menu.cpp: must implement tx_compact_menu_key")
    check(bool(re.search(r'\{TAB_CONTROLS, "Compact menu key", &opts\.compact_menu_key, 0, 1, 1, tx_compact_menu_key', menu_cpp)),
          "vk_menu.cpp: TAB_CONTROLS must include Compact menu key row")

    # 3. draw_tune_bar exists and is called in draw_menu
    check("void draw_tune_bar(ui::Ctx& c)" in menu_cpp, "vk_menu.cpp: must define draw_tune_bar")
    check("if (tune_mode) draw_tune_bar(k);" in menu_cpp, "vk_menu.cpp: draw_menu must call draw_tune_bar when tune_mode is active")
    check("if (!(tune_mode && page == 0)) c.d->AddRectFilled" in menu_cpp,
          "vk_menu.cpp: draw_menu must dim for subpages and only skip dim on page 0")

    # 4. Prompts in draw_options and draw_tune_bar
    check('{ui::kCompact, "Compact", compact_key_name}' in menu_cpp,
          'vk_menu.cpp: draw_options must offer {ui::kCompact, "Compact", compact_key_name} prompt')
    check('{ui::kCompact, "Full menu", compact_key_name}' in menu_cpp,
          'vk_menu.cpp: draw_tune_bar must offer {ui::kCompact, "Full menu", compact_key_name} prompt')
    # (Save my pipeline list and the Bug report, actions rather than pages, say Save: tests/test_pipeline_share.py,
    # tests/test_bug_report.py)
    check('tune_prompts[1] = {ui::kConfirm, confirms ? (pending == &r ? "Confirm" : r.name) : &r == &rows[kBugReportRow] || is_share_row(r) ? "Save" : "Open"};' in menu_cpp,
          "vk_menu.cpp: draw_tune_bar must use row name or Confirm for Restart/Quit")

    # 5. Motion tab title in compact bar
    check('cur_tab == TAB_MOTION ? "Motion" : tab_title(cur_tab)' in menu_cpp,
          'vk_menu.cpp: draw_tune_bar must label Motion tab as "Motion"')

    # 6. Option A (toggle via key release or pad West alone)
    check("const bool tune_toggle = tune_release || pad(HOS_PAD_WEST);" in menu_cpp,
          "vk_menu.cpp: menu_tick must detect tune_toggle from tune_release or gamepad West alone")
    check("HOS_PAD_NORTH" not in menu_cpp[menu_cpp.index("tune_toggle ="):menu_cpp.index("if (tune_toggle")],
          "vk_menu.cpp: tune_toggle must not include HOS_PAD_NORTH")

    # 7. Option B (direct entry from game via tune key + Insert / F1)
    check("if (open_now.load()) tune_mode = tune_down;" in menu_cpp,
          "vk_menu.cpp: opening menu with tune key held must directly activate tune_mode (Option B)")

    # 8. FPS counter stays in safe corner during tune mode
    check("under_banner && !tune_mode && page != 5" in menu_cpp,
          "vk_menu.cpp: draw_fps must not offset under banner during tune_mode")

    # 9. Cut the real toggle and dim logic from vk_menu.cpp and compile unit fixture
    dim_match = re.search(r'if \((!\(tune_mode && page == 0\))\) c\.d->AddRectFilled', menu_cpp)
    check(bool(dim_match), "vk_menu.cpp: could not find dim condition")
    dim_expr = dim_match.group(1)

    toggle_start = menu_cpp.index("bool tune_release = false;")
    toggle_end = menu_cpp.index("if (tune_release) pad_last = false;")
    toggle_raw = menu_cpp[toggle_start:toggle_end]
    toggle_code = "\n".join("        " + line.strip() for line in toggle_raw.strip().splitlines())

    fixture_src = f"""#include <atomic>
#include <cassert>
#include <cstdio>

static int g_tab_state = 0;
#define VK_TAB 0x09
static short GetAsyncKeyState(int v) {{
    return (v == VK_TAB && g_tab_state) ? (short)0x8000 : (short)0;
}}

static bool should_dim(bool tune_mode, int page) {{
    return {dim_expr};
}}

struct ToggleState {{
    bool tune_held = false;
    bool tune_cancelled = false;
    std::atomic<bool> open_now{{true}};
    int page = 0;
    int listening = 0;

    bool step(bool tune_down, bool tab_down) {{
        g_tab_state = tab_down ? 1 : 0;
{toggle_code}
        return tune_release;
    }}
}};

int main() {{
    // 1. Verify dimming (cut directly from vk_menu.cpp)
    assert(!should_dim(true, 0));   // Compact bar: no dim
    assert(should_dim(true, 1));    // Controls subpage: dimmed
    assert(should_dim(true, 3));    // Teleport subpage: dimmed
    assert(should_dim(true, 4));    // Inventory subpage: dimmed
    assert(should_dim(true, 5));    // Restart dialog: dimmed
    assert(should_dim(false, 0));   // Full menu: dimmed

    // 2. Verify toggle release (cut directly from vk_menu.cpp)
    ToggleState s;
    // Tap alone: press down, release -> fires toggle
    assert(!s.step(true, false));
    assert(s.step(false, false));

    // Shift + Tab: press down, tab pressed, release -> cancelled, does NOT toggle
    assert(!s.step(true, false));
    assert(!s.step(true, true));
    assert(!s.step(false, false));

    // Page != 0 or listening: does NOT toggle
    s.page = 1;
    assert(!s.step(true, false));
    assert(!s.step(false, false));
    s.page = 0;

    std::puts("PASS: compact toggle release and dim conditions cut from vk_menu.cpp verified");
    return 0;
}}
"""

    with tempfile.TemporaryDirectory(prefix="menu-tune-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture"
        cpp.write_text(fixture_src, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

    print("PASS: compact menu bar (Option A & Option B, release toggle, dim & prompts) checks passed")


if __name__ == "__main__":
    main()
