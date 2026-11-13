"""The overlays (the start-up screen, the menu, the FPS counter) over the window's image: menu_record draws them with Dear ImGui
in one frame and one recording, the start-up screen under the menu and the counter over both; nothing at all (no ImGui frame,
no render pass, only ui::idle letting old textures go) while none shows (but one frame that bakes the glyphs after a resize), and no "0 FPS" over the start-up screen.

python -I -B tests/test_fps_overlay_order.py [--cxx g++] [--source path/to/vk_menu.cpp]
Executes the real menu_record with recording stubs; no GPU or game data.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_menu.cpp")
    a = p.parse_args()
    fn = block(a.source.read_text(encoding="utf-8"), "void menu_record(")
    fixture = r'''#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkCommandBuffer = int;
struct { bool show_fps = false; } opts;
std::atomic<bool> open_now{false}, startup_on{false};
std::string calls;
bool can_draw = true;
float ease_step = 0;
int64_t prof_now() { return 1000; }
namespace ui {
struct Ctx {};
bool begin(uint32_t, uint32_t) { calls += "b"; return can_draw; }
void idle() { calls += "i"; }
void record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) { CHECK(cmd == 1 && index == 2 && w == 640 && h == 360); calls += "r"; }
}
namespace mkwvk::rec {   // (RX_VK_RECORDER: the boundary before each Dear ImGui recording, logged as "d")
enum Bound { B_DIRECT };
void boundary(Bound) { calls += "d"; }
}
ui::Ctx ui_ctx(uint32_t, uint32_t) { return {}; }
void prebake(const ui::Ctx&) { calls += "p"; }
bool resized = false;
bool prebake_needed(uint32_t, uint32_t) { return resized; }
void draw_startup(ui::Ctx&) { calls += "s"; }
void draw_menu(ui::Ctx&) { calls += "m"; }
void draw_fps(ui::Ctx&, bool under_banner) { calls += under_banner ? "F" : "f"; }
''' + fn + r'''
int main() {
    for (bool startup : {false, true}) for (bool fps : {false, true}) for (bool menu : {false, true}) for (bool can : {true, false}) {
        opts.show_fps = fps;
        open_now = menu;
        startup_on = startup;
        can_draw = can;
        calls.clear();
        menu_record(1, 2, 640, 360);
        if (!startup && !fps && !menu) {   // (nothing shows: no ImGui frame at all, only the reaping; after a resize, one bake)
            CHECK(calls == "i");
            calls.clear(), resized = true;
            menu_record(1, 2, 640, 360);
            CHECK(calls == (can ? "bpdr" : "bi"));
            resized = false;
            continue;
        }
        if (!can) { CHECK(calls == "b"); continue; }                           // (no ImGui here: nothing recorded)
        std::string want = "bp";
        if (startup) want += "s";
        if (menu) want += "m";
        if (fps && !startup) want += menu ? "F" : "f";
        want += "dr";   // (the recorder boundary right before the recording)
        CHECK(calls == want);
    }
    std::printf("PASS: one ImGui frame and recording for whatever shows, in order, nothing while nothing shows\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="fps-overlay-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
