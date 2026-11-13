"""The Insert menu's sections: heading rows group a tab's rows, and none can be left empty (gpu/vk/vk_menu.cpp).

A heading row (Row::heading) is a section's title: draw_options draws it (a gold title and a rule, nothing to click) and
menu_tick's keys step over it. The Graphics tab is grouped as Resolution and upscaling, Frame generation and latency,
Screen, HDR, Detail, and GPU and memory, after the Quality preset that sets rows in all of them. Each section must keep a
row on every PC: a row row_shown can hide (Reflex, Anti-Lag 2, the Upscaler, the DLSS model, HDR's levels) cannot be a
section's only row, or a GPU without it would show a title over nothing. The rows table is read from the source; nothing
runs.

    python -I -B tests/test_menu_sections.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

GRAPHICS = [
    ("Resolution and upscaling", ["Aspect ratio", "Output resolution", "Profile", "Upscaler", "DLSS model", "Render resolution", "Sharpness"]),
    ("Frame generation and latency", ["Frame generation", "Frame generation by", "Frame pacing", "Game frame rate cap",
                                      "NVIDIA Reflex", "AMD Anti-Lag 2"]),
    ("Screen", ["Fullscreen", "V-Sync", "Window scaling filter", "Ultrawide menus"]),
    ("HDR", ["HDR", "HDR paper white", "HDR peak brightness", "HDR contrast", "HDR tone mapper"]),
    ("Detail", ["Texture detail", "Anisotropic filtering"]),
    ("GPU and memory", ["GPU device", "VRAM budget"]),
]


def function(text, marker):
    start = text.index(marker)
    depth = 0
    for at in range(text.index("{", start), len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated: " + marker)


def rows_of(menu):
    """The rows table's entries in order: (tab, name, value expression, heading)"""
    table = menu[menu.index("Row rows[] = {"):]
    table = table[:table.index("\n};")]
    out = []
    for m in re.finditer(r'\n    \{(TAB_[A-Z]+), "([^"]+)", ([^,]+),', table):
        entry = table[m.start():table.find("\n    {", m.end())]
        heading = bool(re.search(r"nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true\}", entry))
        out.append((m.group(1), m.group(2), m.group(3).strip(), heading))
    return out


def main():
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    rows = rows_of(menu)
    if len(rows) < 40:
        raise SystemExit("FAIL: the rows table was not found (%d rows read)" % len(rows))

    # the values row_shown can hide: a section needs a row that is not one of them
    hidable = set(re.findall(r"&opts\.\w+", function(menu, "bool row_shown(int i) {")))
    if "&opts.reflex" not in hidable or "&opts.hdr_white" not in hidable:
        raise SystemExit("FAIL: row_shown no longer reads as expected (Reflex, HDR's levels)")

    tabs = {}
    for tab, name, value, heading in rows:
        tabs.setdefault(tab, []).append((name, value, heading))
    for tab, entries in tabs.items():
        for i, (name, value, heading) in enumerate(entries):
            if not heading:
                continue
            section = []
            for later in entries[i + 1:]:
                if later[2]:
                    break
                section.append(later)
            if not section:
                raise SystemExit("FAIL: %s: the section %r has no rows" % (tab, name))
            if all(v in hidable for _, v, _ in section):
                raise SystemExit("FAIL: %s: every row of the section %r can be hidden (row_shown): a GPU without them "
                                 "would show its title over nothing" % (tab, name))

    graphics = tabs.get("TAB_GRAPHICS", [])
    if not graphics or graphics[0][0] != "Quality preset" or graphics[0][2]:
        raise SystemExit("FAIL: the Graphics tab does not start with the Quality preset")
    titles = [name for name, _, heading in graphics if heading]
    want = [title for title, _ in GRAPHICS]
    if titles != want:
        raise SystemExit("FAIL: the Graphics tab's sections are %r, not %r" % (titles, want))
    section_of, current = {}, None
    for name, value, heading in graphics:
        if heading:
            current = name
        elif current:
            section_of[name] = current
    for title, members in GRAPHICS:
        for name in members:
            if section_of.get(name) != title:
                raise SystemExit("FAIL: Graphics: %r is under %r, not %r" % (name, section_of.get(name), title))

    # the heading rows are drawn as titles and stepped over by the keys (no code of their own per tab)
    draw = function(menu, "void draw_options(ui::Ctx& c) {")
    if "if (r.heading) {" not in draw:
        raise SystemExit("FAIL: draw_options no longer draws heading rows as titles")
    tick = function(menu, "void menu_tick() {")
    if "while (heading_at(selected));" not in tick:
        raise SystemExit("FAIL: menu_tick's keys no longer step over heading rows")
    print("menu sections: OK (%d headings; Graphics in %d sections)" % (sum(r[3] for r in rows), len(titles)))


if __name__ == "__main__":
    main()
