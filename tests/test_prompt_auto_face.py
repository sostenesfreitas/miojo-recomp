"""Game-free: the button prompts' Auto face for a controller's name (host/prompts.c face_of_name, contains). A Steam Controller
gets the Steam Deck's face, as the Deck does: SDL3 names it "Steam Controller", which fell through to the Xbox's. Under
Steam, Steam Input's stand-in pad for a Steam Controller has the same name, so it gets the Deck's face too. A pad named
"Steam Virtual Gamepad" stays on the Xbox's, since any controller can be behind it. And auto_face, which choose_face and live_face
ask: with no controller, the keyboard's face when the build made it and XInput has no controller either, else the Switch's.

python -I -B tests/test_prompt_auto_face.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (the controller's name as SDL gives it, the face Auto picks)
NAMES = [
    ("", "HOS_PROMPT_SWITCH"),
    ("Steam Controller", "HOS_PROMPT_STEAMDECK"),
    ("Steam Deck", "HOS_PROMPT_STEAMDECK"),
    ("Steam Deck Controller", "HOS_PROMPT_STEAMDECK"),
    ("Steam Virtual Gamepad", "HOS_PROMPT_XBOX"),
    ("DualSense Wireless Controller", "HOS_PROMPT_PS5"),
    ("PS4 Controller", "HOS_PROMPT_PS4"),
    ("Nintendo Switch Pro Controller", "HOS_PROMPT_SWITCH"),
    ("Xbox 360 Controller", "HOS_PROMPT_XBOX"),
    ("8BitDo Ultimate 2C", "HOS_PROMPT_XBOX"),
]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    check("static int face_of_name(" in src, "host/prompts.c has no face_of_name")
    check("static int auto_face(" in src, "host/prompts.c has no auto_face")
    check("auto_face(name)" in block(src, "static int choose_face("), "choose_face does not ask auto_face")
    check("auto_face(name)" in block(src, "static int live_face("), "live_face does not ask auto_face")
    check("face_of_name(name)" in block(src, "static int auto_face("), "auto_face does not ask face_of_name")

    harness = "\n".join([
        "#include <ctype.h>", "#include <stdio.h>", "#include <string.h>", '#include "gpu/vk/vk_api.h"',
        "static int pads_xinput;",
        "static int hos_xinput_pads(void) { return pads_xinput; }",
        "static int keyboard_built;",
        block(src, "static int contains("),
        block(src, "static int face_of_name("),
        block(src, "static int auto_face("),
        "int main(void) {",
        *('    printf("%%d %%d\\n", face_of_name("%s"), %s);' % nf for nf in NAMES),
        # (Auto with no controller: the keyboard's face when the build made it and XInput has none either, else the Switch's;
        # a controller's name: its face, whatever the keyboard)
        "    keyboard_built = 1;",
        '    printf("%d %d\\n", auto_face(""), HOS_PROMPT_KEYBOARD);',
        '    printf("%d %d\\n", auto_face("Xbox 360 Controller"), HOS_PROMPT_XBOX);',
        "    pads_xinput = 1;",
        '    printf("%d %d\\n", auto_face(""), HOS_PROMPT_SWITCH);',
        "    pads_xinput = 0, keyboard_built = 0;",
        '    printf("%d %d\\n", auto_face(""), HOS_PROMPT_SWITCH);',
        "    return 0;",
        "}", ""])
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "face.c", Path(tmp) / "face"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(c), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split("\n")
    for (name, face), line in zip(NAMES, out):
        got, want = line.split()
        check(got == want, '"%s": face %s, not %s (%s)' % (name, got, want, face))
    autos = ["no controller, the keyboard face built: the keyboard's", "a controller: its face, the keyboard face built",
             "no controller through SDL, one through XInput: the Switch's", "no controller, no keyboard face built: the Switch's"]
    for what, line in zip(autos, out[len(NAMES):]):
        got, want = line.split()
        check(got == want, "auto_face, %s: face %s, not %s" % (what, got, want))
    print("ok: %d controller names, each with its face; Auto's keyboard face in %d cases" % (len(NAMES), len(autos)))


if __name__ == "__main__":
    main()
