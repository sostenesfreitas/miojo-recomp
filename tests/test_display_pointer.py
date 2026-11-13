"""Compile the real pointer mailbox and both hosts' sampling/consumer functions; no SDL, window or game required.

The mocks reject SDL calls on the renderer thread. The threaded stress checks complete
samples while changing dimensions; clicks and fractional wheel motion are consumed once.
python -I -B tests/test_display_pointer.py [--cxx g++] [--source repository]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    win = (args.source / "host/display.c").read_text(encoding="utf-8")
    sdl = (args.source / "host/posix/display_sdl.c").read_text(encoding="utf-8")
    adapter = (ROOT / "host/hos_sdl.c").read_text(encoding="utf-8")
    template = (ROOT / "tests/display_pointer_test.cpp").read_text(encoding="utf-8")
    template = template.replace("/* ACTUAL_SDL_ADAPTER */", function(adapter, "void hos_sdl_mouse_position("))
    for name, source, signatures in (
        ("WINDOWS", win, ("int hos_mouse_pointer(", "static void pointer_publish_at(", "static void pointer_update(", "static void mouse_capture(", "int hos_mouse_game_pointer(", "static void game_menu_press(", "static void menu_press(", "static void menus_mouse(", "static void menu_mouse(")),
        ("SDL", sdl, ("int hos_mouse_pointer(", "static void pointer_publish_at(", "static void pointer_update(", "static void drawable_size(", "static uint32_t sdl_button_bit(", "void hos_posix_post_message(", "static void menu_mouse(")),
    ):
        # The current baseline lacks pointer_update: retain its actual consumer for the causal comparison.
        functions = [function(source, sig) for sig in signatures if sig in source]
        template = template.replace("/* ACTUAL_" + name + " */", "\n".join(functions))
    start = win.index("    case WM_MOUSEMOVE:")
    end = win.index("    case WM_MOUSEWHEEL:", start)
    template = template.replace("/* ACTUAL_MOVE */", "LRESULT move_event(HWND w, UINT msg, LPARAM lp = 0) { switch(msg) {" + win[start:end] + "} return 0; }")
    start = win.index("    case WM_MOUSEWHEEL:")
    end = win.index("    case WM_RBUTTONDOWN:", start)
    template = template.replace("/* ACTUAL_WHEEL */", "LRESULT wheel_event(HWND w, WPARAM wp) { switch((int)WM_MOUSEWHEEL) {" + win[start:end] + "} return 0; }")
    start = sdl.index("                case HOS_SDL_EV_MOUSE_DOWN:")
    end = sdl.index("                case HOS_SDL_EV_MOUSE_UP:", start)
    template = template.replace("/* ACTUAL_DOWN */", "void down(float x, float y) { HosSdlEvent e{};e.x=x;e.y=y;e.button=1; switch((int)HOS_SDL_EV_MOUSE_DOWN) {" + sdl[start:end] + "} pointer_update(0,0); }")
    if "static void pointer_update(" not in sdl:
        template = template[:template.index("int main() {")] + "int main() { MkwVkPointer p{}; std::thread t([&] { int w, h; sdl::hos_mouse_pointer(&p); sdl::drawable_size(sdl::window, &w, &h); }); t.join(); }"
    with tempfile.TemporaryDirectory(prefix="display-pointer-") as temp:
        unit, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        unit.write_text(template, encoding="utf-8")
        header_root = args.source if (args.source / "host/display_pointer.h").is_file() else ROOT
        includes = ["-I" + str(header_root), "-I" + str(ROOT)]
        if os.name != "nt":
            includes.append("-I" + str(ROOT / "host/posix"))
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-pthread", *includes, str(unit), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
