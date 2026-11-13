"""A menu row that no Quality preset sets leaves the preset alone when it changes.

set_row turns the Quality preset to Custom when a row without preset_exempt changes. That is right for the rows a preset
sets (the resolution, the filter, sharpness, frame pacing, game speed, the profile) and wrong for the others: changing
Anisotropic filtering or Reflex said Custom although every row the preset sets was still the preset's. Read the rows
table and apply_preset from gpu/vk/vk_menu.cpp and check that the two agree. No game or GPU is needed.
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Rows no preset sets that still turn the preset to Custom, with the reason
NOT_EXEMPT = {
    "dlss": "another Upscaler moves Render resolution to its own steps (ur::snap): the preset's resolution may not hold",
}


def block(text, start):
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated block at %d" % start)


def split_args(body):
    """The top-level arguments of an initializer, strings and nesting kept whole."""
    args, depth, quote, start, at = [], 0, False, 0, 0
    while at < len(body):
        ch = body[at]
        if quote:
            if ch == "\\":
                at += 1
            elif ch == '"':
                quote = False
        elif ch == '"':
            quote = True
        elif ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        elif ch == "," and depth == 0:
            args.append(body[start:at].strip())
            start = at + 1
        at += 1
    args.append(body[start:].strip())
    return args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    menu = (args.source / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    preset = block(menu, menu.index("void apply_preset(int p)"))
    set_by_preset = set(re.findall(r"opts\.(\w+)\s*=(?!=)", preset))
    if not {"resolution", "pace", "realtime", "handheld"} <= set_by_preset:
        raise SystemExit("apply_preset: the rows it sets were not found (%s)" % sorted(set_by_preset))

    table = block(menu, menu.index("Row rows[] = {"))
    rows = 0
    wrong = []
    for match in re.finditer(r"\{TAB_\w+,", table):
        fields = split_args(block(table, match.start())[1:-1])
        value = re.fullmatch(r"&opts\.(\w+)", fields[2])
        if not value or value.group(1) == "preset":
            continue
        rows += 1
        field = value.group(1)
        exempt = len(fields) > 10 and fields[10] == "true"
        wanted = field not in set_by_preset and field not in NOT_EXEMPT
        if exempt != wanted:
            wrong.append("%s (opts.%s): preset_exempt %s, wanted %s" % (fields[1], field, exempt, wanted))
    if rows < 30:
        raise SystemExit("read only %d rows from the table: has its shape changed?" % rows)
    stale = [field for field in NOT_EXEMPT if "&opts.%s," % field not in table]
    if stale:
        raise SystemExit("NOT_EXEMPT names a row the table no longer has: " + ", ".join(stale))
    if wrong:
        raise SystemExit("rows whose preset_exempt does not match apply_preset:\n  " + "\n  ".join(wrong))
    print("PASS: %d rows checked: those no preset sets (apply_preset: %s) leave the Quality preset alone" %
          (rows, ", ".join(sorted(set_by_preset))))


if __name__ == "__main__":
    main()
