"""tests/srccut.py, the one source cutter the checks share (the dev-loop proposal's D2): a block's braces are counted outside
string and character literals (raw ones too), comments and preprocessor lines; a missing or repeated marker stops the check
with one message naming it; between() and unique=False; and the comparison with the old brace counter that a migration runs.

python -I -B tests/test_srccut.py
"""
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
import srccut  # noqa: E402

SOURCE = r'''
static int other(void) { return 1; }

void menu_row(const char *label) {
    const char *open = "{ not a brace";   // nor this one: }
    char c = '}';
    /* a comment with { and } in it
       } */
    std::string raw = R"x(a raw }} string)x";
#define WRAP(x) { x }
    long big = 1'000'000;
    if (label) {
        draw("}\"");
    }
}

int after(void) { return 2; }
'''


class Cut(unittest.TestCase):
    def test_block_skips_braces_in_literals_comments_and_preprocessor_lines(self):
        cut = srccut.block(SOURCE, "void menu_row(", "menu.cpp")
        self.assertTrue(cut.startswith("void menu_row(const char *label) {"))
        self.assertTrue(cut.endswith('        draw("}\\"");\n    }\n}'), cut[-60:])
        self.assertNotIn("after", cut)

    def test_the_old_counter_cut_that_wrongly_and_plain_code_the_same(self):
        try:   # (it counted the braces in the string, the comments and the literals)
            old = srccut._naive(SOURCE, "void menu_row(")
        except (ValueError, IndexError):
            old = None
        self.assertNotEqual(old, srccut.block(SOURCE, "void menu_row("))
        plain = "void f(void) {\n    if (x) {\n        y();\n    }\n}\n"
        self.assertEqual(srccut.block(plain, "void f("), srccut._naive(plain, "void f("))

    def test_a_missing_marker_names_the_source_and_the_marker(self):
        with self.assertRaises(srccut.CutError) as e:
            srccut.block(SOURCE, "void renamed(", "gpu/vk/vk_menu.cpp")
        self.assertIn("'void renamed(' is not in gpu/vk/vk_menu.cpp", str(e.exception))

    def test_a_repeated_marker_stops_unless_asked_for_the_first(self):
        with self.assertRaises(srccut.CutError) as e:
            srccut.block(SOURCE, "(void) {", "menu.cpp")
        self.assertIn("more than once (lines 2 and 17)", str(e.exception))
        self.assertEqual(srccut.block(SOURCE, "(void) {", unique=False), "(void) { return 1; }")

    def test_between(self):
        self.assertEqual(srccut.between(SOURCE, "char c", ";"), "char c = '}'")
        with self.assertRaises(srccut.CutError):
            srccut.between(SOURCE, "char c", "no such end")

    def test_an_unclosed_block_says_so(self):
        with self.assertRaises(srccut.CutError) as e:
            srccut.block("void f(void) {\n    if (x) {\n", "void f(")
        self.assertIn("does not close", str(e.exception))

    def test_the_migration_check_compares_with_the_old_counter(self):
        plain = "void f(void) { { } }\n"
        with mock.patch.dict(os.environ, {"SRCCUT_CHECK_NAIVE": "1"}):
            self.assertEqual(srccut.block(plain, "void f("), "void f(void) { { } }")
            with self.assertRaises(srccut.CutError) as e:
                srccut.block('void g(void) { s = "}"; }\n', "void g(")
        self.assertIn("cuts differently from the old brace counter", str(e.exception))


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
