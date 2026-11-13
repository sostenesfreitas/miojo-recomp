"""Exercise real scripted parsing, routing and Npad serialization with authored files/API spies.

The parser, LIVE reader, input routing and Npad writer are extracted unmodified from production. Clock,
foreground-window, keyboard, mouse and pad APIs are fakes; no real device, desktop or game is accessed.
Each case starts a fresh process because production caches environment settings. Frame-clock cases retain holds
through a seven-second wall pause, test native-frame boundaries/live offsets/slot reuse, and keep default/0/empty controls.

Controllers counted only while the game's window was in front, so a game streamed from a screen of its own (Sunshine
and Moonlight), or left running behind another window, ignored the pad. The Insert menu's "Controller in background"
row, or HOS_INPUT_BACKGROUND=1, lets the pads through; the keyboard and mouse stay with the window in front. Behind
another window as in front, the pads are the Insert menu's while it is open, and after it closed until they are let go.

python -I -B tests/test_input_background.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ("foreground", "script-background", "headless", "noactivate", "live-background",
         "no-script-background", "exclusive-foreground", "exclusive-background", "pads-env", "pads-menu", "pads-off",
         "pads-insert",
         "frame-hold", "live-frame", "reuse-frame", "wall", "zero", "empty")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--case", choices=CASES)
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")

    read = block(hid, "static void read_input(")
    behind = read[read.index("if (!game_in_front()"):]   # (with or without PR 545's scripted-background test)
    behind = behind[:behind.index("return;") + len("return;")]
    merge = block(hid, "static int pads_merge(")   # (the pads with the Insert menu's hold, in front and behind alike)
    if "pads_in_background(" not in behind or "pads_merge(" not in behind or "pads_read(" not in merge:
        raise SystemExit("behind another window the pads are never read")
    if "key(" in behind or "mouse" in behind:
        raise SystemExit("behind another window the keyboard or mouse would reach the game")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    if "id == 4 ? opts.pad_background" not in option:
        raise SystemExit("the host cannot see the menu's row (option 4)")
    if '"Controller in background", &opts.pad_background' not in menu:
        raise SystemExit("the Insert menu has no row for it")

    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    constants = block(hid, "enum { B_A") + ";\n" + block(api, "enum { HOS_SW_A") + ";\n"
    constants += block(api, "enum { HOS_KEY_MOVE_UP") + ";\n"   # (the keys-only controls)
    constants += block(api, "enum { HOS_OPT_CHEAT_HEARTS") + ";\n"
    constants += block(api, "enum { HOS_BIND_PAD") + ";\n"   # (the live input's !bind)
    constants += block(api, "enum { HOS_OPT_AUTO_RUN") + ";\n"   # (Auto run: off here, rx_option answers 0)
    constants += (ROOT / "host/auto_run.h").read_text(encoding="utf-8") + "\n"
    for name in ("STATE_SIZE", "NPAD_NO1", "ATTR_CONNECTED"):
        matches = re.findall(r"^#define " + name + r"\s+[^\n]+", hid, re.M)
        assert len(matches) == 1, name
        constants += matches[0] + "\n"
    start = hid.index("typedef struct { int64_t start_ms, end_ms;")
    script = block(hid, "static void scripted_input(")
    end = hid.index(script) + len(script)
    cheat_state = re.search(r"^static uint64_t flight_input;\nstatic uint64_t free_camera_axes;\nstatic int free_camera_height;\n"
                            r"static int move_stick_held;", hid, re.M).group(0)
    cheat_spies = """
volatile int hos_oot_live_action;
int hos_oot_flight_shortcut_ready(void) { CHECK(!"unexpected cheat readiness query"); return 0; }
void rx_option_set(int id, int value) { (void)id; (void)value; CHECK(!"unexpected cheat option change"); }
int hos_oot_player_free(void) { return 1; }
"""
    # (the host's own presses and stick, for the game's menus' mouse (host/oot_menu.c): none here; the player's input
    # time it hides the mouse by: not looked at)
    host_spies = """
static void host_taps(void) {}
static void host_stick_read(int32_t stick[4]) { (void)stick; }
static void host_drag_read(int32_t stick[4]) { (void)stick; }
static void player_used(void) {}
"""
    actual = "\n".join((cheat_state, cheat_spies, host_spies, block(hid, "static int key("), block(hid, "static int kmap_down("),
        block(hid, "static int game_in_front("), hid[start:end], block(hid, "static void stick_max("),
        block(hid, "static int pads_in_background("), block(hid, "static int pads_merge("),
        block(hid, "static int pads_behind("),
        # (the bound keys' helpers read_input calls: a mouse button counts only while the mouse is captured)
        block(hid, "static int mouse_vk_slot("), re.search(r"^static int mouse_armed\[5\];", hid, re.M).group(0),
        block(hid, "static void mouse_buttons_arm("), block(hid, "static int bound_down("),
        block(hid, "static void oc_notes("), block(hid, "static uint64_t oc_pad_word("),
        block(hid, "static int flight_jump_shortcut("), read, block(hid, "static void npad_states(")))
    glue = (ROOT / "host/mkw_glue.c").read_text(encoding="utf-8")
    actual += "\n" + "\n".join(block(glue, "uint64_t " + name + "(")
                                  for name in ("hos_title_input", "hos_frame_step_buttons"))
    env = os.environ.copy()
    for name in list(env):
        if name.startswith("HOS_INPUT_") or name in ("HOS_HEADLESS", "HOS_WINDOW_NOACTIVATE"):
            del env[name]
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    def run(command, environment=None):
        result = subprocess.run(command, env=environment, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", creationflags=flags)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()
    with tempfile.TemporaryDirectory(prefix="input-background-") as tmp:
        folder = Path(tmp).resolve()
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("input-background-")
        (folder / "input_constants.inc").write_text(constants, encoding="utf-8")
        (folder / "input_actual.inc").write_text(actual, encoding="utf-8")
        exe = folder / "fixture.exe"
        # Production's button table deliberately leaves its unused axis fields zero-initialized.
        run([a.cc, "-std=c11", "-include", ENV_ON_H, "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
             "-D_CRT_SECURE_NO_WARNINGS",
             "-I" + str(folder), str(ROOT / "tests/input_background_test.c"), "-o", str(exe)])
        for case in ((a.case,) if a.case else CASES):
            case_env = dict(env)
            if case not in ("live-background", "no-script-background", "live-frame", "reuse-frame", "pads-insert"):
                case_env["HOS_INPUT_SCRIPT"] = "0:A+LUP/200"
            if case in ("frame-hold", "live-frame", "reuse-frame"):
                case_env["HOS_INPUT_FRAME_CLOCK"] = "1"
            elif case in ("zero", "empty"):
                case_env["HOS_INPUT_FRAME_CLOCK"] = "0" if case == "zero" else ""
            if case.startswith("exclusive-") or case in ("frame-hold", "wall", "zero", "empty"):
                case_env["HOS_INPUT_EXCLUSIVE"] = "1"
            if case in ("pads-env", "pads-off"):
                case_env["HOS_INPUT_BACKGROUND"] = "1" if case == "pads-env" else "0"
            if case == "headless":
                case_env["HOS_HEADLESS"] = "1"
            if case == "noactivate":
                case_env["HOS_WINDOW_NOACTIVATE"] = "1"
            if case in ("live-background", "live-frame"):
                live = folder / (case + ".txt")
                live.write_bytes(b"")
                case_env["HOS_INPUT_LIVE"] = str(live)
            run([str(exe), case], case_env)
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("input-background-")


if __name__ == "__main__":
    main()
