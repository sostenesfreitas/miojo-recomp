"""The Debug tab's inventory editor's items (gpu/vk/oot_cheat_items.h, read by gpu/vk/vk_menu.cpp and host/hos_main.c):
no two rows share a label or a pouch name, and the probe list names no spelling twice. The Hookshot and the Longshot are
two items of the game, each with its own row: a project rename once made the Hookshot's row a second Longshot
(Item_Hookshot and Weapon_Range_Hookshot gone), so the Debug tab could not give the Hookshot.

The header's own macros are expanded by the compiler (a small program prints each row), so the check sees what the menu
and the game side compile. No game, no GPU.

    python -I -B tests/test_cheat_items.py [--cxx c++]
"""
import argparse
import collections
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

PRINTER = r'''
#include <cstdio>
#include "oot_cheat_items.h"
#define SECTION(label) std::printf("section\t%s\n", label);
#define ITEM(label, name, kind) std::printf("item\t%s\t%s\t%d\n", label, name, kind);
static const char *const probes[] = {OOT_CHEAT_ITEM_PROBES};
int main() {
    OOT_CHEAT_ITEMS(SECTION, ITEM)
    for (const char *p : probes) std::printf("probe\t%s\n", p);
    return 0;
}
'''


def rows(cxx):
    with tempfile.TemporaryDirectory(prefix="cheat-items-") as folder:
        src, exe = Path(folder) / "items.cpp", Path(folder) / "items.exe"
        src.write_text(PRINTER, encoding="utf-8")
        subprocess.run([cxx, "-std=c++17", "-Wall", "-Werror", "-I" + str(ROOT / "gpu" / "vk"), str(src), "-o", str(exe)],
                       check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    return [line.split("\t") for line in out.splitlines()]


def twice(values):
    return sorted(v for v, n in collections.Counter(values).items() if n > 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    lines = rows(args.cxx)
    items = [r[1:] for r in lines if r[0] == "item"]
    probes = [r[1] for r in lines if r[0] == "probe"]
    labels, names = [i[0] for i in items], [i[1] for i in items]
    problems = []
    if len(items) < 50:
        problems.append("only %d items: the header was not read whole" % len(items))
    if twice(labels):
        problems.append("labels twice: %s" % ", ".join(twice(labels)))
    if twice(names):
        problems.append("pouch names twice: %s" % ", ".join(twice(names)))
    if twice(probes):
        problems.append("probes twice: %s" % ", ".join(twice(probes)))
    for label, name in (("Hookshot", "Item_Hookshot"), ("Longshot", "Item_Longshot")):
        if [label, name, "0"] not in items:
            problems.append("no %s row (%s, kind 0)" % (label, name))
    for probe in ("Weapon_Range_Hookshot", "Weapon_Range_Longshot"):
        if probe not in probes:
            problems.append("no %s probe" % probe)
    if "Hookshot" in labels and "Longshot" in labels and labels.index("Hookshot") + 1 != labels.index("Longshot"):
        problems.append("the Longshot's row is not right after the Hookshot's (its upgrade, as the game lists them)")
    for p in problems:
        print("FAIL " + p)
    if not problems:
        print("cheat items: %d items, %d probes; no label, pouch name or probe twice; the Hookshot and the Longshot both there"
              % (len(items), len(probes)))
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
