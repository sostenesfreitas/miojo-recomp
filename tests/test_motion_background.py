"""Game-free: a pad's motion counts only when its buttons do (host/nn_hid.c motion_counts, GetSixAxisSensorStates).

The buttons stopped while the game's window was behind another one, but the gyro kept turning the camera: the motion
path did not look at the window. Now it follows the buttons' rule: with the game's window in front, or behind another
window when the player asked for the pads there (the Insert menu's "Controller in background", or
HOS_INPUT_BACKGROUND=1; =0 keeps them to the window in front whatever the menu says). Otherwise the pad reads at rest.

pads_in_background and motion_counts are cut out of host/nn_hid.c unchanged and compiled with a table of cases, and
GetSixAxisSensorStates is checked to ask motion_counts before it reads the pad's motion.

python -I -B tests/test_motion_background.py [--cc gcc]
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

# (front, HOS_INPUT_BACKGROUND or None, the menu's row, whether motion counts)
CASES = [
    (1, None, 0, 1),     # in front: counts
    (1, "0", 0, 1),      # in front, the variable set to 0: still counts (it only decides the background)
    (0, None, 0, 0),     # behind another window: the pad at rest
    (0, None, 1, 1),     # behind, "Controller in background" on: counts
    (0, "1", 0, 1),      # behind, HOS_INPUT_BACKGROUND=1: counts whatever the menu says
    (0, "0", 1, 0),      # behind, HOS_INPUT_BACKGROUND=0: at rest whatever the menu says
]


def function(text, name):
    m = re.search(r"static int " + name + r"\([^)]*\) \{", text)
    if not m:
        sys.exit(f"FAIL: {name} not found in host/nn_hid.c")
    end, depth = m.end(), 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[m.start():end]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()
    src = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")

    six = src[src.index("HLE(_ZN2nn3hid22GetSixAxisSensorStates"):]
    six = six[:six.index("\n}\n")]
    gate = six.find("motion_counts(game_in_front()")
    read = six.find("hos_sdl_motion_history(h, count)")
    if gate < 0 or read < 0 or gate > read or "count && counts ?" not in six:
        sys.exit("FAIL: GetSixAxisSensorStates reads the pad's motion without asking motion_counts first")

    table = ",\n".join(f'    {{{f}, {"NULL" if e is None else chr(34) + e + chr(34)}, {m}, {want}}}'
                       for f, e, m, want in CASES)
    fixture = f"""#include <stdio.h>
#include <stdlib.h>
{function(src, "pads_in_background")}
{function(src, "motion_counts")}
int main(void) {{
    static const struct {{ int front; const char *env; int menu, want; }} cases[] = {{
{table}
    }};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++)
        if (motion_counts(cases[i].front, cases[i].env, cases[i].menu) != cases[i].want) {{
            fprintf(stderr, "FAIL: front %d, HOS_INPUT_BACKGROUND %s, menu %d: %d, not %d\\n", cases[i].front,
                    cases[i].env ? cases[i].env : "unset", cases[i].menu, !cases[i].want, cases[i].want);
            return 1;
        }}
    return 0;
}}
"""
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "t.c", Path(tmp) / "t"
        c.write_text(fixture)
        r = subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("FAIL: compile\n" + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr.strip())
    print(f"ok: motion counts only when the pad's buttons do ({len(CASES)} cases)")


if __name__ == "__main__":
    main()
