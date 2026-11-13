"""The movement and camera keys count as the player's input (host/nn_hid.c read_input), as the pads and the bound
Switch buttons do: the game menus' cursor hides again once they are used (host/display.c gm_cursor_shown).

python -I -B tests/test_keys_player_input.py
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HID = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class KeysPlayerInputTests(unittest.TestCase):
    def test_the_keys_stamp_the_players_input(self):
        body = function(HID, "static void read_input(")
        at = body.index("stick_max(stick, keys);")
        self.assertRegex(body[at:], r"^stick_max\(stick, keys\);\s*if \(keys\[0\] \|\| keys\[1\] \|\| keys\[2\] \|\| keys\[3\]\) player_used\(\);")

    def test_the_cursor_reads_that_stamp(self):
        display = (ROOT / "host" / "display.c").read_text(encoding="utf-8")
        self.assertIn("hos_player_input_ns() > InterlockedCompareExchange64(&gm_mouse_ns, 0, 0)", function(display, "static int gm_cursor_shown("))


if __name__ == "__main__":
    unittest.main()
