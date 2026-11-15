"""Every Insert menu row that takes a restart has a slot in menu_restart.h's State (kMax of them). Main grew to ten such rows
while kMax was 8: the last two (Audio output, Button prompts) had slot -1, and the menu read start[-1], out of bounds, so the
Button prompts row said "After restart" with nothing changed. Checks the count against kMax, and that the menu asks
restart_changed (which skips a row with no slot) rather than indexing start[] itself.

    python -I -B tests/test_menu_restart_slots.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    kmax = int(re.search(r"constexpr int kMax = (\d+);", (ROOT / "gpu/vk/menu_restart.h").read_text(encoding="utf-8")).group(1))
    table = menu[menu.index("Row rows[] = {"):]
    table = table[:table.index("\n};")]
    restart = []
    for entry in re.split(r"\n    \{TAB_", table)[1:]:
        name = re.match(r'\w+, "([^"]+)"', entry).group(1)
        flags = re.findall(r"\b(true|false)\b", entry)
        if flags and flags[0] == "true":
            restart.append(name)
    if len(restart) > kmax:
        raise SystemExit("FAIL: %d rows take a restart (%s), menu_restart.h kMax is %d" % (len(restart), ", ".join(restart), kmax))
    if re.search(r"restart_state\.start\[restart_slot_of\(", menu):
        raise SystemExit("FAIL: vk_menu.cpp indexes restart_state.start with a slot that can be -1: use restart_changed")
    if "slot >= 0 && row_restart_value(r) != restart_state.start[slot]" not in menu:
        raise SystemExit("FAIL: restart_changed does not skip a row with no slot")
    print("PASS: %d rows take a restart, %d slots" % (len(restart), kmax))


if __name__ == "__main__":
    main()
