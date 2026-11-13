"""gpu/vk/menu_i18n.h, the Insert menu's translations: every table's texts are the menu's own (vk_menu.cpp writes each
English text), every translation keeps its text's printf conversions in order and its leading and trailing spaces (the menu
joins pieces), and the language list holds what vk_menu_i18n.cpp expects. The controls and Automatic prompt hints
must remain present in every translated table. No game, no compiler.

Run with: python -I tests/test_menu_i18n.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def c_strings(text):
    """The C string literals of a source, adjacent ones joined, escapes as written"""
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'"\s*\n\s*"', '', text)
    return re.findall(r'"((?:[^"\\\n]|\\.)*)"', text)


def conversions(s):
    """The printf conversions of s in order (as vk_menu_i18n.cpp conversion() reads them; not ' ': "80% suits" is a sign)"""
    return re.findall(r'%[-+#0]*\d*(?:\.\d+)?[lhz]*[sducxgf%]', s)


def tables():
    src = (VK / "menu_i18n.h").read_text(encoding="utf-8")
    out = {}
    for name, body in re.findall(r'inline constexpr Entry (k\w+)\[\] = \{\n(.*?)\n\};', src, re.S):
        pairs = re.findall(r'^    \{"((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\},$', body, re.M)
        out[name] = pairs
    langs = re.findall(r'^    \{"([\w-]+)", "((?:[^"\\]|\\.)*)", (\w+), (\d+)\},$', src, re.M)
    return out, langs


class MenuI18nTests(unittest.TestCase):
    def setUp(self):
        self.tables, self.langs = tables()
        menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
        self.menu = set(c_strings(menu))

    def test_languages(self):
        self.assertEqual(self.langs[0][:3], ("en", "English", "nullptr"))
        codes = [code for code, _, _, _ in self.langs]
        self.assertEqual(len(codes), len(set(codes)), "a language listed twice")
        for code, _, table, count in self.langs[1:]:
            self.assertIn(table, self.tables, code)
            self.assertEqual(len(self.tables[table]), int(count), "%s: its count is not its table's" % code)

    def test_texts_are_the_menus(self):
        for table, pairs in self.tables.items():
            for en, _ in pairs:
                self.assertTrue(en in self.menu, "%s: %r is no text of vk_menu.cpp (renamed? then translate it again)" % (table, en))

    def test_current_hints_are_translated(self):
        # Whole-string lookup needs the complete hint. Dropping a stale entry and lowering its count is not a fix.
        hints = {
            "controls": (
                "Gamepad: the D-pad moves, A binds, B goes back. Ocarina note keys are only notes while "
                "the ocarina waits for one; away from it they do their other jobs."
            ),
            "automatic button prompts": (
                "Those of the first controller, from its name (SDL's). With none, the keyboard's (the Switch's with XInput "
                "alone)."
            ),
        }
        for name, hint in hints.items():
            self.assertTrue(hint in self.menu, "%s: expected complete hint is no text of vk_menu.cpp" % name)
        for table, pairs in self.tables.items():
            keys = {en for en, _ in pairs}
            for name, hint in hints.items():
                self.assertTrue(hint in keys, "%s: missing current %s hint" % (table, name))

    def test_plural_forms(self):
        # (i18n::plural: Russian and Polish ask for a third form, " settings (many)", for 5 and up; the menu shows it
        # nowhere else, so only their tables have it)
        cpp = (VK / "vk_menu_i18n.cpp").read_text(encoding="utf-8")
        for table, code in (("kru", "ru"), ("kpl", "pl")):
            self.assertIn('code == "%s"' % code, cpp)
            self.assertIn(" settings (many)", dict(self.tables[table]), table)
            self.assertIn(" settings", dict(self.tables[table]), table)

    def test_conversions_and_spaces(self):
        for table, pairs in self.tables.items():
            keys = [en for en, _ in pairs]
            self.assertEqual(len(keys), len(set(keys)), "%s: a text twice" % table)
            self.assertEqual(keys, sorted(keys), "%s: not sorted (menu_i18n.h is written sorted)" % table)
            for en, text in pairs:
                self.assertTrue(text, "%s: %r has an empty translation" % (table, en))
                self.assertEqual(conversions(en), conversions(text), "%s: %r and %r" % (table, en, text))
                for side in (lambda s: s[:len(s) - len(s.lstrip(" "))], lambda s: s[len(s.rstrip(" ")):]):
                    self.assertEqual(side(en), side(text), "%s: the spaces around %r" % (table, en))


if __name__ == "__main__":
    unittest.main()
