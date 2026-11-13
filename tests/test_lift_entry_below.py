"""A lifted function starts at its entry, also when it has code below its entry (nx2/lift_fixes.py, 4).

a64-kit's emit_function writes a function's instructions in address order, so the C function began at its lowest
instruction. A function is its entry and everything reachable from it without crossing another entry: code it
branches to at a lower address, which is no entry of its own, is part of it and comes first in the C. The game has
such functions (in libcurl, which has one unwind entry for all of its code, and the return address of a setjmp
call), and so has a mod's patch routine that goes on in spare code at a lower address.

A real a64_lift.Lifter over a made-up image of three functions, emitted with made-up texts for the instructions (no
translator, so no compiler):
  - ENTRY branches down to LOW (no entry), which branches back up into it: without the fix its C starts with LOW's
    instruction (the check has teeth: the unwrapped emit is run first), with it the C starts with `goto L_<entry>;`
    and the entry has its label; nothing else in the text changes, and nx2/poll.py's polls are added as before;
  - LOOP branches back to its own entry and has nothing below it: unchanged;
  - PLAIN: unchanged.
Run here the C by hand: the statements a call of each function executes, in order, with and without the fix.

python -I -B tests/test_lift_entry_below.py
"""
import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import a64_lift  # noqa: E402
import lift_fixes  # noqa: E402
import poll  # noqa: E402

BASE = 0x80000000
LOW, ENTRY, LOOP, PLAIN = BASE + 0x1000, BASE + 0x2000, BASE + 0x3000, BASE + 0x4000
RET = 0xd65f03c0


def add_x0(n):
    return 0x91000000 | (n << 10)   # add x0, x0, #n


def b(pc, target):
    return 0x14000000 | (((target - pc) >> 2) & 0x3FFFFFF)


def cbnz_x0(pc, target):
    return 0xb5000000 | ((((target - pc) >> 2) & 0x7FFFF) << 5)


WORDS = {
    LOW: add_x0(1), LOW + 4: b(LOW + 4, ENTRY + 8),                              # spare code below ENTRY, no entry
    ENTRY: add_x0(2), ENTRY + 4: b(ENTRY + 4, LOW), ENTRY + 8: add_x0(3), ENTRY + 12: RET,
    LOOP: add_x0(4), LOOP + 4: cbnz_x0(LOOP + 4, LOOP), LOOP + 8: RET,           # a loop to its own entry
    PLAIN: add_x0(5), PLAIN + 4: RET,
}


class Image:
    entries = {ENTRY, LOOP, PLAIN}
    hooks = {}
    names = {}

    def word(self, pc):
        return WORDS[pc]

    def in_code(self, pc):
        return pc in WORDS

    def read(self, pc, n):
        return b"".join(struct.pack("<I", WORDS[pc + 4 * i]) for i in range(n // 4))


def texts():
    """What the translator would hand emit_function for the plain instructions: here a line that names them"""
    return {w: ("    c->x[0] += %d;" % ((w >> 10) & 0xFFF), 0) for w in WORDS.values() if (w >> 24) == 0x91}


def run(text, start=None):
    """The statements a call of the C function executes, in order: the `c->x[0] += n;` lines, following gotos, up to
    its return (x0 taken as 0 at a cbnz so a loop ends)"""
    lines = text.splitlines()
    at = {m.group(1): i for i, line in enumerate(lines) for m in [re.match(r"(L_[0-9a-f]{8}):;", line)] if m}
    i = 1 if start is None else at[start]
    out = []
    for _ in range(100):
        line = lines[i]
        m = re.search(r"c->x\[0\] \+= (\d+);", line)
        if m:
            out.append(int(m.group(1)))
        if "if (" in line:           # (cbnz x0: not taken)
            i += 1
            continue
        g = re.search(r"goto (L_[0-9a-f]{8});", line)
        if g:
            i = at[g.group(1)]
            continue
        if "return" in line or line == "}":
            return out
        i += 1
    raise AssertionError("no return reached:\n" + text)


class EntryBelow(unittest.TestCase):
    def setUp(self):
        self.lifter = a64_lift.Lifter(Image())
        self.lifter.discover()

    def emit(self, e, fixed=True):
        fn = a64_lift.Lifter.emit_function if fixed else BASE_EMIT
        return fn(self.lifter, e, texts())[0]

    def test_bodies(self):
        funcs = self.lifter.funcs
        self.assertEqual(sorted(funcs), [ENTRY, LOOP, PLAIN])
        self.assertEqual(min(funcs[ENTRY][0]), LOW)       # LOW is ENTRY's own code, below its entry
        self.assertEqual(min(funcs[LOOP][0]), LOOP)
        self.assertEqual(min(funcs[PLAIN][0]), PLAIN)

    def test_without_the_fix_it_starts_below(self):
        text = self.emit(ENTRY, fixed=False)
        self.assertEqual(run(text), [1, 3])               # LOW's instruction first, the entry's own never

    def test_starts_at_its_entry(self):
        before = self.emit(ENTRY, fixed=False)
        text = self.emit(ENTRY)
        head = "void f_%08x(Ctx *__restrict A64_NOESC c) {" % ENTRY
        lines = text.splitlines()
        self.assertEqual(lines[0], head)
        self.assertEqual(lines[1], "goto L_%08x;" % ENTRY)
        self.assertEqual(text.count("L_%08x:;" % ENTRY), 1)
        self.assertEqual(run(text), [2, 1, 3])            # as the instructions run from ENTRY
        # the rest of the text is the unfixed one's: only the goto and the entry's label are new
        rest = [line for line in lines[2:] if line != "L_%08x:;" % ENTRY]
        self.assertEqual(rest, before.splitlines()[1:])
        self.assertEqual(self.lifter.stats["entry_gotos"], 1)

    def test_other_functions_unchanged(self):
        for e in (LOOP, PLAIN):
            self.assertEqual(self.emit(e), self.emit(e, fixed=False))
            self.assertNotEqual(self.emit(e).splitlines()[1], "goto L_%08x;" % e)
        self.assertEqual(run(self.emit(LOOP)), [4])
        self.assertEqual(run(self.emit(PLAIN)), [5])
        self.assertEqual(self.lifter.stats["entry_gotos"], 0)

    def test_polls_see_the_same_loops(self):
        """nx2/poll.py wraps the emit after this fix (nx2/lift.py's order): the goto to the entry is a forward one"""
        for e in (ENTRY, LOOP, PLAIN):
            self.assertEqual(poll.add_polls(self.emit(e))[1], poll.add_polls(self.emit(e, fixed=False))[1])
        text, n = poll.add_polls(self.emit(ENTRY))
        self.assertEqual(text.splitlines()[1], "goto L_%08x;" % ENTRY)
        self.assertEqual(run(text), [2, 1, 3])


BASE_EMIT = a64_lift.Lifter.emit_function   # a64-kit's own
lift_fixes.install()                        # as nx2/lift.py does at its start

if __name__ == "__main__":
    unittest.main()
