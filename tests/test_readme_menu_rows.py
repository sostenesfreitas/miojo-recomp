"""readme.md's option table has a line for every row of the Insert menu's Graphics, Audio, Gameplay and Controls tabs.

Four rows had none: Ultrawide menus, Mesh shaders, Discord status (which is on by default) and the Controls page's
Button prompts. The rows are read from gpu/vk/vk_menu.cpp's table and the lines from the readme's
"| Tab | Option | Choices | Default |" table; nothing runs. Restart and Quit are actions with no choice to list; the
Controls page's two rows have lines of their own among the Controls tab's; the Motion page's rows are on the Motion
line, the Graphics tab's section titles have none, and the Debug tab's lines at the table's end are left out.

    python -I -B tests/test_readme_menu_rows.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MENU = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
README = (ROOT / "readme.md").read_text(encoding="utf-8")
ACTIONS = {"Restart", "Quit"}


# (the Graphics tab's section titles: rows with heading, the last field, set; nothing to choose on them)
HEADING = r'\{TAB_\w+,\s*"([^"]+)",\s*nullptr,[^{}]*,\s*-1,\s*true\}'
HEADINGS = re.findall(HEADING, MENU)
ROWS = re.sub(HEADING, "", MENU)


def menu_rows(tabs="GRAPHICS|AUDIO|GAMEPLAY|CONTROLS|PAGE"):
    return re.findall(r'\{TAB_(%s),\s*"([^"]+)"' % tabs, ROWS)


def table_lines():
    start = README.index("| Tab | Option | Choices | Default |")
    lines = README[start:README.index("\n\n", start)].split("\n")[2:]
    rows = [[cell.strip() for cell in line.strip().strip("|").split("|")] for line in lines]
    debug = next((i for i, cells in enumerate(rows) if cells[0] == "Debug"), len(rows))
    return rows[:debug]   # (the Debug tab's lines end the table: its Developer rows, not checked here)


class ReadmeMenuRowsTests(unittest.TestCase):
    def test_the_tables_are_found(self):
        self.assertGreater(len(menu_rows()), 40)
        self.assertGreater(len(table_lines()), 40)
        self.assertIn(("GRAPHICS", "Quality preset"), menu_rows())
        self.assertIn("Resolution and upscaling", HEADINGS)

    def test_every_row_of_the_four_tabs_has_a_line(self):
        listed = {cells[1] for cells in table_lines()}
        missing = [name for _, name in menu_rows() if name not in listed and name not in ACTIONS]
        self.assertEqual(missing, [], "menu rows with no line in readme.md's option table")

    def test_every_line_is_a_row_the_menu_has(self):
        rows = {name for _, name in menu_rows()}
        gone = [cells[1] for cells in table_lines() if cells[1] not in rows]
        self.assertEqual(gone, [], "lines in readme.md's option table for rows the menu no longer has")

    def test_the_lines_keep_the_menus_order(self):
        page = {name for _, name in menu_rows("PAGE")}   # (the Controls page's rows: listed with the Controls tab's)
        tabs = ["GRAPHICS", "AUDIO", "GAMEPLAY", "CONTROLS"]   # (a tab shows its rows in the table's order, wherever they sit)
        rows = sorted(menu_rows("GRAPHICS|AUDIO|GAMEPLAY|CONTROLS"), key=lambda row: tabs.index(row[0]))
        order = [name for _, name in rows if name not in ACTIONS and name not in page]
        listed = [cells[1] for cells in table_lines() if cells[1] not in page]
        self.assertEqual(listed, order)

    def test_every_line_has_its_four_cells_and_a_default(self):
        for cells in table_lines():
            with self.subTest(option=cells[1]):
                self.assertEqual(len(cells), 4)
                self.assertTrue(cells[2])
                self.assertTrue(cells[3] or cells[1] in ("Motion", "Controls", "Bug report"))   # (they open a page or do a thing: no default)


if __name__ == "__main__":
    unittest.main()
