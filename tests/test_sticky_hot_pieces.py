"""The hot pieces of the last lift stay as they were: a change to one hot function changes one piece, not most of them.

split_hot.py dealt the hot functions out afresh at every lift, so a few bytes more in one function moved functions between 6 of the
8 pieces and every player compiled all eight again (the longest third of a cold compile). Now the last lift's pieces are kept in
<work>/image/hot_pieces.txt; known functions keep their piece, new ones go to a neighbour's, and a fresh split is used when the
kept one is out of balance. Each piece declares only the functions it names. Made-up functions only.

python -I -B tests/test_sticky_hot_pieces.py
"""
import contextlib
import hashlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import split_hot  # noqa: E402

PIECES = 4


def function(addr, calls, lines):
    return ("void f_%08x(Ctx *__restrict A64_NOESC c) {\n" % addr + "c->x[1] = c->x[2] + 1;\n" * lines +
            "".join("f_%08x(c);\n" % t for t in calls) + "}\n")


def program(extra=None, grow=None):
    addrs = [0x80000000 + 0x100 * i for i in range(40)]
    funcs = {a: function(a, [addrs[(i * 7 + 3) % 40]] if i % 3 else [], 50 + 13 * i) for i, a in enumerate(addrs)}
    if grow:
        funcs[grow] = function(grow, [], 50 + 13 * addrs.index(grow) + 40)
    if extra:
        funcs[extra] = function(extra, [addrs[5]], 200)
    names = sorted(funcs)
    preamble = '#include "a64.h"\n' + "".join("void f_%08x(Ctx *A64_NOESC);\n" % a for a in names) + \
        "extern void hos_oot_menu_tick(Ctx *c, uint64_t screen);\n\n"
    return preamble + "".join(funcs[a] for a in names)


def split(work, text):
    gen = work / "sa" / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    (work / "image").mkdir(exist_ok=True)
    (gen / "code_hot.c").write_text(text, newline="")
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        split_hot.split(str(gen), PIECES, split_hot.pieces_file(str(gen)))
    files = {p.name: p.read_bytes() for p in sorted(gen.glob("code_hot_*.c"))}
    return {n: hashlib.sha1(b).hexdigest() for n, b in files.items()}, files, out.getvalue()


class StickyHotPiecesTests(unittest.TestCase):
    def test_one_function_changed_changes_one_piece(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            first, _, _ = split(work, program())
            self.assertTrue((work / "image" / "hot_pieces.txt").exists())
            again, _, log = split(work, program(grow=0x80000000 + 0x100 * 17))
            self.assertIn("the pieces of the last lift kept", log)
            self.assertEqual(sum(first[n] != again[n] for n in first), 1)

    def test_a_new_function_joins_a_neighbour_and_is_declared_only_there(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            first, _, _ = split(work, program())
            again, files, log = split(work, program(extra=0x81000000))
            self.assertIn("1 functions placed anew", log)
            holder = [n for n, b in files.items() if b"void f_81000000(Ctx *__restrict" in b]
            self.assertEqual(len(holder), 1)
            declared = [n for n, b in files.items() if b"void f_81000000(Ctx *A64_NOESC);" in b]
            self.assertEqual(declared, holder)
            self.assertEqual(sum(first[n] != again[n] for n in first), 1)

    def test_every_piece_declares_what_it_names_and_compiles_the_same_functions(self):
        with tempfile.TemporaryDirectory() as d:
            _, files, _ = split(Path(d), program())
            seen = set()
            for name, text in files.items():
                body_names = set(split_hot.NAME.findall(text.decode().split("\n\n", 1)[1]))
                decls = {line.split("(")[0][5:] for line in text.decode().split("\n") if split_hot.DECL.match(line)}
                self.assertEqual(decls, {n for n in body_names if n.startswith("f_")})
                seen |= set(split_hot.HEAD.findall(text.decode()))
            self.assertEqual(len(seen), 40)

    def test_out_of_balance_or_other_counts_split_afresh(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            split(work, program())
            (work / "image" / "hot_pieces.txt").write_text("".join("f_%08x 0\n" % (0x80000000 + 0x100 * i) for i in range(40)))
            _, _, log = split(work, program())
            self.assertNotIn("kept", log)   # (one piece only: not this number of pieces)
            (work / "image" / "hot_pieces.txt").write_text("".join("f_%08x %d\n" % (0x80000000 + 0x100 * i, 0 if i else 1)
                                                                   for i in range(40)) + "f_7fffff00 2\nf_7fffff10 3\n")
            _, _, log = split(work, program())
            self.assertIn("out of balance", log)


if __name__ == "__main__":
    unittest.main()
