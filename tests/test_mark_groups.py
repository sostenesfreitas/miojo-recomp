"""a64_lift.mark_groups: the page marks of a straight-line run of stores through one base register shared by one A64_MARK
after the group's last store, every case where a store must keep its own marks, and its place in the lift: after
nx2/oot_patches.py, whose exact-text edits a group would break (the supported build's lift fails when one is missing).

python -I -B tests/test_mark_groups.py      (the compile-and-run case needs emsdk's clang: EMSDK, else it is skipped)
"""
import contextlib
import glob
import importlib.util
import inspect
import io
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from collections import Counter
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = [os.path.join(ROOT, "third_party", "a64-kit", d) for d in ("lift", "tools")] + [os.path.join(ROOT, "nx2")]
import a64_lift  # noqa: E402
import oot_patches  # noqa: E402
import chunks  # noqa: E402

HEAD = "void f_80000000(Ctx *__restrict A64_NOESC c) {"


def mark(reg, lo, n):
    return "    A64_MARK((c->x[%d] + (uint64_t)(int64_t)(%d)), %d);" % (reg, lo, n)


def run(*body, stats=None):
    return a64_lift.mark_groups("\n".join((HEAD,) + body + ("}",)), stats).split("\n")[1:-1]


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def quiet(fn, *a, **k):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a, **k)


class MarkGroupsTests(unittest.TestCase):
    def test_a_group_shares_one_mark_before_the_call(self):
        out = run("    recomp_store64(c,c->x[19] + 8,c->x[1]);",
                  "    recomp_store32(c,c->x[19] + 16,c->x[2]);",
                  "    { uint64_t _b=c->x[19]; int64_t _o=24; recomp_stp64(c,(_b+_o),c->x[3],c->x[4]); }",
                  "    c->x[0]=recomp_load32(c,c->x[20] + 0);",
                  "    TAILCALL(f_80000100);")
        self.assertEqual(out, ["    recomp_store64_sp(c,c->x[19] + 8,c->x[1]);",
                               "    recomp_store32_sp(c,c->x[19] + 16,c->x[2]);",
                               "    { uint64_t _b=c->x[19]; int64_t _o=24; recomp_stp64_sp(c,(_b+_o),c->x[3],c->x[4]); }",
                               "    c->x[0]=recomp_load32(c,c->x[20] + 0);",
                               mark(19, 8, 32),   # [8, 40): every byte the group wrote
                               "    TAILCALL(f_80000100);"])

    def test_a_write_of_the_base_marks_before_it(self):
        for write in ("    c->x[8]=c->x[8] + 16;", "    c->x[8] += 16;", "    c->x[8]++;", "    --c->x[8];",
                      "    { uint64_t _p0,_p1; recomp_ldp64(c,c->x[9] + 0,&c->x[8],&_p1); }"):
            out = run("    recomp_store64(c,c->x[8] + 0,c->x[1]);", "    recomp_store64(c,c->x[8] + 8,c->x[2]);", write,
                      "    recomp_store64(c,c->x[8] + 0,c->x[3]);", "    a64_trap(c, 1);")
            self.assertEqual(out[2], mark(8, 0, 16), write)
            self.assertEqual(out[3], write)
            self.assertEqual(out[4], "    recomp_store64(c,c->x[8] + 0,c->x[3]);", write)   # (alone after it: its own marks)

    def test_every_run_end_marks_before_it(self):
        for end in ("L_80000010:;", "L_80000010:; A64_POLL(c);", "if (!c->z) { goto L_80000010; }", "    recomp_barrier();",
                    "if (!c->z) { { A64_POLL(c); goto L_80000010; } }",   # (a backward goto with its interrupt poll: nx2/poll.py)
                    "    a64_svc(c, 0x60);", "    A64_ICALL(c, c->x[16]);", "    A64_IJUMP(c, c->x[16]); return;",
                    "    __atomic_fetch_add((uint32_t *)A64_PTR(c->x[2]), 1, __ATOMIC_SEQ_CST);",
                    "    c->x[30] = 0x80000020ULL; f_80001000(c);", "    return;",
                    # (store-release and load-acquire as suyu writes them, without nx2/insn.py: a plain store / load at
                    # the bare register)
                    "    recomp_store32(c,c->x[3],c->x[4]);", "    c->x[5] = recomp_load32(c,c->x[3]);",
                    # (host calls the patches put in, and any helper the pass does not know)
                    "    c->x[8] = hos_output_size(0) | ((uint64_t)hos_output_size(1) << 32);",
                    "    hos_oot_lift_player(c, c->x[19]);", "    a64_new_helper(c);", "    memcpy(A64_PTR(c->x[2]), &c->x[3], 8);",
                    # (the register file as a whole; a block opening or closing over several lines)
                    "    memset(c->x, 0, sizeof c->x);", "    {", "    }", "    if (c->z) {", "    } else {"):
            out = run("    recomp_store32(c,c->x[21] + 0,c->x[1]);", "    recomp_store32(c,c->x[21] + 4,c->x[2]);", end,
                      "    recomp_store32(c,c->x[21] + 8,c->x[3]);")
            self.assertEqual(out[2], mark(21, 0, 8), end)
            self.assertEqual(out[3], end)

    def test_plain_lines_do_not_end_a_run(self):
        for plain in ("    c->x[0]=recomp_load64(c,c->x[20] + 8);",
                      "    { uint64_t _p0,_p1; recomp_ldp64(c,(c->x[20]+16ULL),&_p0,&_p1); c->x[0]=_p0; c->x[1]=_p1; }",
                      "    { uint64_t _v=0; memcpy(&_v,&c->vreg[3][0],8); a64_vset64(c->vreg[1],_v); }",
                      "    { extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }",
                      "    c->x[9] = UINT64_C(1) << 3;", "    /* ic ivau: no self-modifying code */"):
            out = run("    recomp_store32(c,c->x[21] + 0,c->x[1]);", plain, "    recomp_store32(c,c->x[21] + 4,c->x[2]);",
                      "    return;")
            self.assertEqual(out[3], mark(21, 0, 8), plain)

    def test_stores_that_keep_their_own_marks(self):
        cases = [
            # (a conditional store: it may not run)
            ("    if (c->z) recomp_store32(c,c->x[19] + 0,c->x[1]);", "    recomp_store32(c,c->x[19] + 4,c->x[2]);"),
            ("    c->x[0] ? recomp_store32(c,c->x[19] + 0,c->x[1]) : (void)0;", "    recomp_store32(c,c->x[19] + 4,c->x[2]);"),
            # (more than 4 KB apart: more than two pages)
            ("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[19] + 4089,c->x[2]);"),
            # (a line that also writes a register; pre- and post-index writeback)
            ("    recomp_store64(c,c->x[19] + 0,c->x[1]); c->x[1]=0;", "    recomp_store64(c,c->x[19] + 8,c->x[2]); c->x[2]=0;"),
            ("    { uint64_t _b=c->x[19]; int64_t _o=16; recomp_store64(c,(_b+_o),c->x[1]); c->x[19]=_b+_o; }",
             "    { uint64_t _b=c->x[19]; int64_t _o=16; recomp_store64(c,_b,c->x[2]); c->x[19]=_b+_o; }"),
            # (an address the pass does not follow: a register offset)
            ("    recomp_store64(c,c->x[19] + c->x[2],c->x[1]);", "    recomp_store64(c,c->x[19] + 8 * c->x[3],c->x[2]);"),
            # (the STLR form: it ends the run besides)
            ("    recomp_store32(c,c->x[19],c->x[1]);", "    recomp_store32(c,c->x[19],c->x[2]);"),
        ]
        for body in cases:
            out = run(*body, "    TAILCALL(f_80000100);")
            self.assertFalse(any("A64_MARK" in l or "_sp(" in l for l in out), body)

    def test_address_forms(self):
        q = "    { { uint64_t _v=0; memcpy(&_v,&c->vreg[3][0],8); recomp_store64(c,(c->x[0]+%dULL),_v); } }"
        cases = [
            # (Q / D stores at (c->x[N]+IMMULL))
            ((q % 16, q % 24), mark(0, 16, 16)),
            # (STP of two Q registers: the second at (_b+_o)+16)
            (("    { uint64_t _b=c->x[22]; int64_t _o=40; recomp_stp64(c,(_b+_o),c->vreg[2][0],c->vreg[2][1]); "
              "recomp_stp64(c,(_b+_o)+16,c->vreg[3][0],c->vreg[3][1]); }",), mark(22, 40, 32)),
            # (ST1 lanes at _a+IMM)
            (("    { uint64_t _a=c->x[12]; { uint64_t _v=0; memcpy(&_v,(const uint8_t*)c->vreg[0]+8,4); recomp_store32(c,_a+0,_v); } "
              "{ uint64_t _v=0; memcpy(&_v,(const uint8_t*)c->vreg[0]+12,4); recomp_store32(c,_a+4,_v); } }",), mark(12, 0, 8)),
            # (STUR: negative offsets, both spellings)
            (("    { uint64_t _b=c->x[8]; int64_t _o=-16; recomp_store64(c,(_b+_o),c->x[1]); }",
              "    recomp_store64(c,c->x[8] + -8,c->x[2]);"), mark(8, -16, 16)),
            # (exactly 4096 bytes: two pages)
            (("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[19] + 4088,c->x[2]);"), mark(19, 0, 4096)),
            # (the nearest _b before a store is its own: x9's store stays alone, x7's two group)
            (("    { uint64_t _b=c->x[9]; int64_t _o=0; recomp_store64(c,(_b+_o),c->x[1]); } "
              "{ uint64_t _b=c->x[7]; int64_t _o=8; recomp_store64(c,(_b+_o),c->x[2]); }",
              "    recomp_store64(c,c->x[7] + 0,c->x[3]);"), mark(7, 0, 16)),
        ]
        for body, want in cases:
            out = run(*body, "    return;")
            self.assertEqual([l for l in out if "A64_MARK" in l], [want], body)
            self.assertEqual(out[-2], want, body)
            left = re.findall(r"recomp_(?:store|stp)\d+\(c,[^,]*", "\n".join(out))
            self.assertEqual(left, ["recomp_store64(c,(_b+_o)"] if "c->x[9]" in body[0] else [], body)

    def test_the_pages_marked_are_the_same(self):
        """random groups at random bases: the pages the marks left behind (and the group marks) mark are the pages a mark per
        store marks"""
        rng = random.Random(912)
        store = re.compile(r"recomp_(store8|store16|store32|store64|stp32|stp64)(_sp)?\(c,c->x\[(\d+)\] \+ (-?\d+),")
        own = re.compile(r"A64_MARK\(\(c->x\[(\d+)\] \+ \(uint64_t\)\(int64_t\)\((-?\d+)\)\), (\d+)\)")

        def pages(lines, base, each):
            got = set()
            for l in lines:
                for m in store.finditer(l):
                    if each or not m.group(2):
                        a, n = base[int(m.group(3))] + int(m.group(4)), a64_lift.MG_SIZE[m.group(1)]
                        got |= {a >> 12, (a + n - 1) >> 12}
                m = own.search(l)
                if m:
                    a, n = base[int(m.group(1))] + int(m.group(2)), int(m.group(3))
                    got |= {a >> 12, (a + n - 1) >> 12}
            return got
        grouped = 0
        for _ in range(3000):
            body = []
            for _ in range(rng.randint(2, 9)):
                kind = rng.choice(sorted(a64_lift.MG_SIZE))
                body.append("    recomp_%s(c,c->x[%d] + %d,c->x[1]%s);" % (
                    kind, rng.choice((19, 20)), rng.randint(-300, 4400), ",c->x[2]" if kind.startswith("stp") else ""))
            out = run(*body, "    return;")
            grouped += any("A64_MARK" in l for l in out)
            for _ in range(4):
                base = {19: rng.randrange(1 << 20, 1 << 30), 20: rng.choice((1 << 24, (1 << 24) - rng.randint(1, 4096)))}
                self.assertEqual(pages(out, base, False), pages(body, base, True), body)
        self.assertGreater(grouped, 1000)

    def test_multi_line_blocks_keep_their_marks_inside(self):
        out = run("    recomp_store32(c,c->x[19] + 0,c->x[1]);", "    recomp_store32(c,c->x[19] + 4,c->x[2]);",
                  "    if (c->z) {",
                  "        recomp_store32(c,c->x[19] + 8,c->x[3]);", "        recomp_store32(c,c->x[19] + 12,c->x[3]);",
                  "    }",
                  "    recomp_store32(c,c->x[19] + 16,c->x[4]);", "    return;")
        self.assertEqual(out, [
            "    recomp_store32_sp(c,c->x[19] + 0,c->x[1]);", "    recomp_store32_sp(c,c->x[19] + 4,c->x[2]);",
            mark(19, 0, 8),   # (before the block: the block may not run)
            "    if (c->z) {",
            "        recomp_store32_sp(c,c->x[19] + 8,c->x[3]);", "        recomp_store32_sp(c,c->x[19] + 12,c->x[3]);",
            mark(19, 8, 8),   # (inside it, with its stores)
            "    }",
            "    recomp_store32(c,c->x[19] + 16,c->x[4]);", "    return;"])

    def test_bases_group_apart_and_sp_is_left_alone(self):
        out = run("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[20] + 0,c->x[2]);",
                  "    recomp_store64(c,c->x[19] + 8,c->x[3]);", "    recomp_store64(c,c->x[20] + 8,c->x[4]);",
                  "    recomp_store64(c,c->x[31] + 8,c->x[5]);", "    recomp_store64(c,c->x[31] + 16,c->x[5]);",
                  "    TAILCALL(f_80000100);")
        self.assertEqual(sorted(l for l in out if "A64_MARK" in l), [mark(19, 0, 16), mark(20, 0, 16)])
        self.assertIn("    recomp_store64(c,c->x[31] + 8,c->x[5]);", out)

    def test_no_group_leaves_the_text_as_it_was(self):
        for body in (("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    TAILCALL(f_80000100);"),
                     # (a function with locals, A64_LOCAL_REGS: left alone)
                     ("    uint64_t x19_ = c->x[19];", "    recomp_store64(c,x19_ + 0,c->x[1]);",
                      "    recomp_store64(c,c->x[20] + 0,c->x[1]);", "    recomp_store64(c,c->x[20] + 8,c->x[1]);")):
            text = "\n".join((HEAD,) + body + ("}",))
            self.assertEqual(a64_lift.mark_groups(text), text)

    def test_twice_is_once(self):
        """its own marks are not run ends: a group the first pass found too wide is not split by them on a second pass"""
        text = "\n".join(('#include "a64.h"', "void f_80000000(Ctx *A64_NOESC);", HEAD,
                          "    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[19] + 8,c->x[1]);",
                          "    recomp_store64(c,c->x[20] + 0,c->x[1]);", "    recomp_store64(c,c->x[20] + 8,c->x[1]);",
                          "    c->x[20] = 0;", "    recomp_store64(c,c->x[19] + 5000,c->x[1]);",
                          "    { uint64_t _b=c->x[22]; int64_t _o=40; recomp_stp64(c,(_b+_o),c->x[1],c->x[2]); }",
                          "    recomp_store8(c,c->x[22] + 39,c->x[1]);", "    return;", "}",
                          "void f_80000100(Ctx *__restrict A64_NOESC c) {", "    recomp_store8(c,c->x[3] + 0,c->x[1]);",
                          "    recomp_store8(c,c->x[3] + 1,c->x[1]);", "}", ""))
        st = Counter()
        once = a64_lift.mark_groups(text, st)
        self.assertEqual(st["groups"], 3)   # (x20, x22, and x3 of the second function; x19's is 5008 bytes)
        self.assertEqual(a64_lift.mark_groups(once), once)
        self.assertIn("void f_80000000(Ctx *A64_NOESC);", once.split("\n"))

    def test_mark_each_and_the_stats(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "code_000.c")
            text = "\n".join((HEAD, "    recomp_store64(c,c->x[19] + 0,c->x[1]);",
                              "    recomp_store64(c,c->x[19] + 8,c->x[1]);", "    return;", "}", ""))
            with open(path, "w", newline="") as f:
                f.write(text)
            st = Counter()
            with mock.patch.dict(os.environ, {"A64_MARK_EACH": "1"}):
                self.assertEqual(a64_lift.group_marks([path], st), 0)
            self.assertEqual(read(path), text)
            self.assertEqual(st["groups"], 0)
            with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if k != "A64_MARK_EACH"}, clear=True):
                self.assertEqual(a64_lift.group_marks([path], st), 1)
            self.assertEqual((st["stores"], st["groups"], st["grouped_stores"]), (2, 1, 2))
            self.assertIn(mark(19, 0, 16), read(path))

    def test_the_lift_key_has_the_switch(self):
        spec = importlib.util.spec_from_file_location("make_game_mg", os.path.join(ROOT, "nx2", "make_game.py"))
        make_game = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(make_game)
        with tempfile.TemporaryDirectory() as d:
            exefs, work = os.path.join(d, "exefs"), os.path.join(d, "work")
            os.makedirs(exefs)
            os.makedirs(work)
            with open(os.path.join(exefs, "main"), "wb") as f:
                f.write(b"NSO0")
            env = {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}
            with mock.patch.dict(os.environ, env, clear=True):
                key = make_game.lift_key(exefs, work, {})
                with mock.patch.dict(os.environ, {"A64_MARK_EACH": "1"}):
                    self.assertNotEqual(make_game.lift_key(exefs, work, {}), key)

    def test_the_lift_groups_once_with_its_stats(self):
        """emit_function leaves the marks as they are; nx2/lift.py calls chunks.edit_c, which groups the written files once (a
        process per file: _edit_marks), with the lift's stats, before its report"""
        self.assertNotIn("mark_groups", inspect.getsource(a64_lift.Lifter.emit_function))
        src = read(os.path.join(ROOT, "nx2", "lift.py"))
        chunks_src = read(os.path.join(ROOT, "nx2", "chunks.py"))
        self.assertEqual(src.count("group_marks(") + src.count("mark_groups(") + chunks_src.count("group_marks("), 0)
        self.assertEqual(chunks_src.count("mark_groups("), 1)
        self.assertIn("mark_groups(", inspect.getsource(chunks._edit_marks))
        self.assertIn("_each(_edit_marks, ", inspect.getsource(chunks.edit_c))
        call = src.find("chunks.edit_c(gen, L.stats, ")
        self.assertGreater(call, 0)
        self.assertLess(call, src.find("L.report()"))


# The output-size edits' places (nx2/oot_patches.py OUT_EDITS) in small functions, no game code: [0] and [3] each followed by
# another store through the same base, so that a group taken before the patches would change the text they match.
GEN_000 = ('#include "a64.h"\n'
           "void f_81222ce0(Ctx *__restrict A64_NOESC c) {\n"
           "    recomp_store64(c,c->x[21] + 4,c->x[1]);\n"
           + oot_patches.OUT_EDITS[0][0] + "\n"
           "    recomp_store32(c,c->x[21] + 28,c->x[9]);\n"
           + oot_patches.OUT_EDITS[3][0] +
           "    recomp_store32(c,c->x[19] + 3672,c->x[8]);\n"
           "    return;\n"
           "}\n")
GEN_001 = ('#include "a64.h"\n'
           "void f_80000000(Ctx *__restrict A64_NOESC c) {\n"
           + "".join(old for old, _ in oot_patches.OUT_EDITS[1:3]) + "    return;\n" + "".join(
               old + "    recomp_store32(c,c->x[19] + 0,c->x[1]);\n    recomp_store32(c,c->x[19] + 4,c->x[1]);\n"
               for old, _ in oot_patches.OUT_EDITS[4:]) +
           "    return;\n"
           "}\n")


def make_gen(d):
    for name, text in (("code_000.c", GEN_000), ("code_001.c", GEN_001)):
        with open(os.path.join(d, name), "w", newline="") as f:
            f.write(text)
    return sorted(glob.glob(os.path.join(d, "code_*.c")))


def ungroup(text):
    """the text as it was before the groups: their marks gone, their stores marked again"""
    text = "\n".join(l for l in text.split("\n") if not a64_lift.MG_OWN_MARK.match(l))
    return re.sub(r"recomp_(store8|store16|store32|store64|stp32|stp64)_sp\(c,", r"recomp_\1(c,", text)




class OutputSizeOrderTests(unittest.TestCase):
    def test_the_patches_then_the_groups(self):
        """the lift's edits of its C (nx2/lift.py edit_c) for the supported build's whole lift: every output-size edit made
        once, then the groups"""
        with tempfile.TemporaryDirectory() as d:
            files = make_gen(d)
            st = Counter()
            quiet(chunks.edit_c, d, st, True)
            text = "".join(read(f) for f in files)
            self.assertIn(oot_patches.OUT_DECL, text)
            for old, new in oot_patches.OUT_EDITS:   # (each made once, and only groups changed it since)
                self.assertEqual(ungroup(text).count(new), 1, new)
            # (the output size reaches the window's create arg: x21+20 is a store of x8 just after the patched lines, now in a
            # group with x21+28, whose mark covers it)
            f0 = read(files[0])
            self.assertIn("    c->x[8] = hos_output_size(0) | ((uint64_t)hos_output_size(1) << 32);\n", f0)
            self.assertIn("{ uint64_t _b=c->x[21]; int64_t _o=20; recomp_store64_sp(c,(_b+_o),c->x[8]); }", f0)
            self.assertIn(mark(21, 20, 12), f0)
            self.assertIn("    recomp_store64(c,c->x[21] + 4,c->x[1]);", f0)   # (the host call between: its own marks)
            self.assertEqual(st["groups"], 5)   # (x21 [20, 32) and the four pairs of code_001.c)

    def test_patches_made_before_are_found_once(self):
        with tempfile.TemporaryDirectory() as d:
            make_gen(d)
            quiet(oot_patches.apply_output_size, d, required=True)
            once = [read(f) for f in sorted(glob.glob(os.path.join(d, "code_*.c")))]
            quiet(oot_patches.apply_output_size, d, required=True)
            self.assertEqual([read(f) for f in sorted(glob.glob(os.path.join(d, "code_*.c")))], once)

    def test_each_place_counts(self):
        """made places and raw ones are both counted, in one file or apart: made once is accepted; made twice, or made and raw,
        is not (also for [4], whose new text holds its old one)"""
        made = '#include "a64.h"\n' + oot_patches.OUT_DECL + "\n".join(new for _, new in oot_patches.OUT_EDITS) + "\n"
        cases = [("made once", [made], None),
                 ("[3] made twice", [made + oot_patches.OUT_EDITS[3][1]], 3),
                 ("[3] made and raw", [made + oot_patches.OUT_EDITS[3][0]], 3),
                 ("[4] made twice", [made + oot_patches.OUT_EDITS[4][1]], 4),
                 ("[4] made and raw", [made + oot_patches.OUT_EDITS[4][0]], 4),
                 ("[0] raw in another file", [made, '#include "a64.h"\n' + oot_patches.OUT_EDITS[0][0] + "\n"], 0)]
        for what, files, bad in cases:
            with tempfile.TemporaryDirectory() as d:
                for n, text in enumerate(files):
                    with open(os.path.join(d, "code_%03d.c" % n), "w", newline="") as f:
                        f.write(text)
                before = [read(os.path.join(d, "code_%03d.c" % n)) for n in range(len(files))]
                if bad is None:
                    quiet(oot_patches.apply_output_size, d, required=True)
                    self.assertEqual([read(os.path.join(d, "code_%03d.c" % n)) for n in range(len(files))], before, what)
                    continue
                with self.assertRaises(SystemExit, msg=what) as e:
                    quiet(oot_patches.apply_output_size, d, required=True)
                self.assertIn("OUT_EDITS [%d]" % bad, str(e.exception), what)

    def test_a_rebuild_over_an_earlier_lifts_hot_pieces(self):
        """make_game's lift twice with a hot list (chunks.write, edit_c, then split_hot): the second lift's edits run while the
        first one's patched code_hot_*.c are still there, and must find each place once in what this lift wrote"""
        import chunks
        import split_hot
        bodies = {0x81222ce0: GEN_000.split("\n", 2)[2], 0x80000000: GEN_001.split("\n", 2)[2]}
        bodies.update({0x80100000 + 0x40 * i: "    c->x[1] = 1;\n    return;\n}\n" for i in range(12)})

        class Lift:
            write = a64_lift.Lifter.write
            _write_table = a64_lift.Lifter._write_table
            fname = a64_lift.Lifter.fname

            def __init__(self):
                self.funcs = {a: ({}, None) for a in bodies}
                self.hooks = {}
                self.stats, self.unhandled, self.ijump = Counter(), Counter(), Counter()

            def emit_function(self, e, texts):
                return "void f_%08x(Ctx *__restrict A64_NOESC c) {\n" % e + bodies[e], set()
        with tempfile.TemporaryDirectory() as work:
            gen, img = os.path.join(work, "gen"), os.path.join(work, "image")
            os.makedirs(img)
            hot = os.path.join(work, "hot_funcs.txt")
            with open(hot, "w") as f:
                f.write("f_81222ce0\nf_80000000\nf_80100040\n")
            env = {"A64_HOT_CHUNK": "1", "A64_HOT_FUNCS": hot}
            with mock.patch.dict(os.environ, env), mock.patch.object(a64_lift.a64emit, "translate", lambda keys: ([], [])):
                for build in (1, 2):
                    st = Counter()
                    quiet(chunks.write, Lift(), gen, img, rechunk=True, jobs=1)
                    quiet(chunks.edit_c, gen, st, True)   # (SystemExit: a place counted twice)
                    quiet(split_hot.split, gen, 2)
                    names = sorted(glob.glob(os.path.join(gen, "code_*.c")))
                    self.assertTrue(any("code_hot_" in n for n in names), build)
                    text = ungroup("".join(read(n) for n in names))
                    for _, new in oot_patches.OUT_EDITS:
                        self.assertEqual(text.count(new), 1, (build, new))
                    self.assertEqual(st["groups"], 5, build)

    def test_groups_before_the_patches_fail_the_lift(self):
        """the order before this change (sabotage control): the groups break OUT_EDITS[0] and [3], and the supported build's
        lift stops instead of ignoring the Output resolution"""
        with tempfile.TemporaryDirectory() as d:
            files = make_gen(d)
            a64_lift.group_marks(files)
            with self.assertRaises(SystemExit) as e:
                quiet(oot_patches.apply_output_size, d, required=True)
            self.assertIn("OUT_EDITS [0, 3]", str(e.exception))
        with tempfile.TemporaryDirectory() as d:   # (another build or part of a lift: a warning, as before)
            files = make_gen(d)
            a64_lift.group_marks(files)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                oot_patches.apply_output_size(d, required=False)
            self.assertIn("WARNING: the output size places were found [0, 1, 1, 0, 1, 1, 1, 1] times", out.getvalue())

    def test_processes_make_the_one_process_edits(self):
        """edit_c's two passes, a process per file, write the files, print the lines and count the stats of the edits made one
        after the other in one process (oot_patches.apply, then a64_lift.group_marks), also with a place missing"""
        filler = ('#include "a64.h"\n' + "".join(
            "void f_%08x(Ctx *__restrict A64_NOESC c) {\n" % (0x80200000 + 0x40 * i) +
            "    recomp_store64(c,c->x[19] + %d,c->x[1]);\n    recomp_store32(c,c->x[19] + %d,c->x[2]);\n    return;\n}\n"
            % (8 * i, 8 * i + 12) for i in range(5)))

        def gen(d, broken):
            make_gen(d)
            for n in range(2, 10):
                with open(os.path.join(d, "code_%03d.c" % n), "w", newline="") as f:
                    f.write(filler.replace("c->x[19]", "c->x[%d]" % (19 + n % 3)))
            if broken:
                path = os.path.join(d, "code_001.c")
                text = read(path).replace("L_836cea54:;\n", "L_836cea58:;\n")
                with open(path, "w", newline="") as f:
                    f.write(text)
            return sorted(glob.glob(os.path.join(d, "code_*.c")))

        for supported, broken in ((True, False), (False, False), (False, True)):
            runs = []
            for how in ("serial", 1, 4):
                with tempfile.TemporaryDirectory() as d:
                    files = gen(d, broken)
                    st, out = Counter(), io.StringIO()
                    with contextlib.redirect_stdout(out):
                        if how == "serial":
                            oot_patches.apply(d, protect_hearts=supported, required=supported)
                            a64_lift.group_marks(files, st)
                        else:
                            chunks.edit_c(d, st, supported, jobs=how)
                    runs.append(([read(f) for f in files], out.getvalue(), st))
            self.assertGreater(runs[0][2]["groups"], 8)
            self.assertEqual(runs[1], runs[0], (supported, broken))
            self.assertEqual(runs[2], runs[0], (supported, broken))

    def test_a_missing_place_fails_once_required(self):
        """the supported build's whole lift stops on a missing place; another build's, or part of a lift, warns as before"""
        for supported, whole, fails in ((True, True, True), (False, True, False), (True, False, False)):
            with tempfile.TemporaryDirectory() as d:
                make_gen(d)
                path = os.path.join(d, "code_001.c")
                text = read(path).replace("L_836cea54:;\n", "L_836cea58:;\n")
                with open(path, "w", newline="") as f:
                    f.write(text)
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    if fails:
                        with self.assertRaises(SystemExit) as e:
                            chunks.edit_c(d, Counter(), supported, whole)
                        self.assertIn("OUT_EDITS [4]", str(e.exception))
                    else:
                        chunks.edit_c(d, Counter(), supported, whole)
                        self.assertIn("WARNING: the output size places were found [1, 1, 1, 1, 0, 1, 1, 1] times", out.getvalue())

    @unittest.skipUnless(os.path.isfile(os.path.join(os.environ.get("EMSDK", r"C:\emsdk"), "upstream", "bin", "clang.exe"))
                         or os.name != "nt" and shutil.which("clang"), "no clang")
    def test_the_patched_function_runs_the_same_grouped(self):
        """f_81222ce0's output size compiled from the patched C, without and with the groups: the same guest memory (the
        output size at x21+20) and the same pages marked"""
        import toolchain
        cc = toolchain.need(toolchain.find("clang"))
        harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint8_t a64_test_mem[1 << 16];
uint8_t a64_dirty[1u << 20];
volatile uint64_t hos_wide_out;
uint32_t hos_output_size(int height) { return height ? 2160 : 3840; }
uint64_t hos_rt_pool_size(uint64_t g) { return g; }
#include "code_000.c"
int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[1] = 0x1122334455667788ULL; c.x[9] = 0x99aabbccULL; c.x[21] = 0x2fec; c.x[19] = 0x4000;
    f_81222ce0(&c);
    for (unsigned i = 0; i < sizeof a64_test_mem; i++) if (a64_test_mem[i]) printf("m %x %02x\n", i, a64_test_mem[i]);
    for (unsigned i = 0; i < 64; i++) if (a64_dirty[i]) printf("p %u\n", i);
    return 0;
}
'''
        outs = []
        with tempfile.TemporaryDirectory() as d:
            make_gen(d)
            quiet(oot_patches.apply, d, required=True)
            for f in ("a64_suyu.h", "a64_fpx.h"):
                open(os.path.join(d, f), "w").close()
            with open(os.path.join(d, "harness.c"), "w") as f:
                f.write(harness)
            for grouped in (False, True):
                if grouped:
                    self.assertEqual(a64_lift.group_marks([os.path.join(d, "code_000.c")]), 1)
                exe = os.path.join(d, "h%d.exe" % grouped)
                subprocess.run([cc] + toolchain.native_target() + toolchain.native_link() +
                               ["-O1", "-Wno-everything", "-DA64_WRITE_TRACK", "-DA64_HOST_BASE=((uintptr_t)a64_test_mem)",
                                "-I" + os.path.join(ROOT, "third_party", "a64-kit", "rt"), "-I" + d,
                                os.path.join(d, "harness.c"), "-o", exe], check=True)
                outs.append(subprocess.run([exe], check=True, capture_output=True, text=True).stdout)
        self.assertEqual(outs[0], outs[1])
        mem = {int(a, 16): int(v, 16) for k, a, v in (l.split() for l in outs[0].splitlines() if l[0] == "m")}
        word = lambda a, n: sum(mem.get(a + i, 0) << (8 * i) for i in range(n))
        self.assertEqual(word(0x2fec + 20, 8), 3840 | 2160 << 32)   # (the window's size, from hos_output_size)
        self.assertEqual(word(0x4000 + 3668, 4), 0x45340000)   # (2880.0f: 3/4 of the output's width)
        self.assertEqual(word(0x4000 + 3672, 4), 0x44ca8000)   # (1620.0f)
        self.assertEqual([int(l.split()[1]) for l in outs[0].splitlines() if l[0] == "p"], [2, 3, 4])


if __name__ == "__main__":
    unittest.main()
