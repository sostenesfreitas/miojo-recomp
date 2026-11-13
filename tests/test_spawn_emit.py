"""The lift's emit in new processes (spawn, as on Windows, where there is no fork) writes what one process writes: a real
a64_lift.Lifter over a small made-up image (4,500 functions of a few instructions: arithmetic, loads and stores, a
PC-relative ADR, a float add, a conditional branch, loops, a word the translator does not handle, calls to the next
function, calls through a register, indirect jumps, and the calls of a function named setjmp), written by chunks.write with
1 process and with 3 spawned ones, file for file, with the same counters. Game-free.

Four configurations, in this order (nx2/poll.py patches the lifter for good once installed):
  - the lift's plain one: no polls, no counters passed, A64_LOCAL_REGS unset;
  - a translator that is not the parent's (as if nx2/insn.py's patch had not reached the workers): the workers stop as they
    start, the parent says so and emits the runs, still the same files;
  - a Lifter subclass, which the gate keeps in one process (the workers would call Lifter.emit_function);
  - what nx2/lift.py does by default: poll.install() and counters=[poll.stats], with A64_LOCAL_REGS=4 and comments on, so
    that the functions without local registers and the names matter. poll.stats must count something.
Each checks that the output has the feature in it (a switch for a jump table, a devirtualized call, a PLT stub, the setjmp
wrapper, an unhandled word, polls, local registers, ...), and that the spawned workers really emitted (the parent printed no
"worker process stopped": else the files would be its own fallback's).

It has teeth for every entry of chunks.EMIT_TABLES, by hand and restored: dropped from the tuple, each makes the workers raise
(AttributeError: no such table), except setjmp_owner, whose lack changes the files (emit_function reads it with a getattr
default); sent empty instead (an empty dict, set or False), each changes the files. And a worker whose translator differs
from the parent's stops at start.

Needs a C++ compiler for the translator (a64emit.cpp, built here into a temporary folder, not under third_party/): skipped
without one.

python -I -B tests/test_spawn_emit.py [--cxx clang++]
"""
import argparse
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import platform
import struct
import sys
import tempfile
import unittest
from unittest import mock

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import build_emit  # noqa: E402

# The spawned workers run this file again (as __mp_main__) before anything else: they must load the translator the parent
# built in its temporary folder, not build one under third_party/ (the parent sets the variable once it has built it)
DLL = os.environ.get("HOOKSHOT_TEST_A64EMIT")
if DLL:
    build_emit.build = lambda force=False: DLL

import a64_lift  # noqa: E402
import chunks  # noqa: E402

BASE = 0x80000000
FUNCS = 4500
LOOP = 5            # a loop in every 5th function
UNHANDLED = 0xb8200000      # ldadd (LSE): not in suyu's translator, nx2/insn.py adds it
BR_X5, BLR_X5 = 0xd61f00a0, 0xd63f00a0


def function_target(i):
    return i + 5


def function(i):
    """A function's words (("bl", j): a call of function j): a few kinds of instruction, some varying with i so the runs'
    words differ"""
    if i % 900 == 17:                            # a PLT stub (nx2 a64_lift.plt_target): x16 = the GOT slot, x17 = target, br x17
        return [0x90000010, ("got", function_target(i)), 0x91000210, 0xd61f0220]
    words = [0x91000400 | ((i % 4095) << 10),   # add x0, x0, #i
             0x10000041,                         # adr x1, #8 (PC-relative: translated per pc)
             0xf9400022,                         # ldr x2, [x1]
             0xf9000402,                         # str x2, [x0, #8]
             0x1e212800]                         # fadd s0, s0, s1
    if i % LOOP == 2:
        words += [0xf1000463, 0x54ffffe1]        # subs x3, x3, #1; b.ne back to it (a backward branch: a loop)
    if i % 400 == 3:
        words.append(UNHANDLED)
    if i % 350 == 9:
        words.append(BLR_X5)
    words.append(0xb4000040)                     # cbz x0, #8
    if i + 1 < FUNCS:
        words.append(("bl", i + 1))
    words.append(BR_X5 if i % 450 == 5 else 0xd65f03c0)   # br x5, else ret
    return words


class Image:
    SETJMP = 10     # the function named _setjmp: its callers are the setjmp owners, and theirs get the wrapper

    def __init__(self):
        self.code, self.hooks, self.data = {}, {}, {}
        sizes = [len(function(i)) + 1 for i in range(FUNCS)]
        starts, at = [], BASE
        for n in sizes:
            starts.append(at)
            at += 4 * n
        for i, start in enumerate(starts):
            for k, w in enumerate(function(i) + [0xd503201f]):   # (a nop between functions)
                pc = start + 4 * k
                if isinstance(w, tuple) and w[0] == "bl":
                    w = 0x94000000 | (((starts[w[1]] - pc) // 4) & 0x3ffffff)
                elif isinstance(w, tuple):   # ldr x17, [x16, #slot] with the slot at the stub's own address (8-aligned)
                    w = 0xf9400211 | (((start & 0xfff) >> 3) << 10)
                    self.data[start & ~7] = starts[function_target(i)]
                self.code[pc] = w
        self.entries = set(starts)
        self.names = {starts[self.SETJMP]: "_setjmp"}

    def in_code(self, pc):
        return pc in self.code

    def word(self, pc):
        return self.code[pc]

    def read(self, addr, size):
        if addr in self.data:
            return struct.pack("<Q", self.data[addr])[:size]
        return b"".join(struct.pack("<I", self.code.get(a, 0)) for a in range(addr, addr + size, 4))[:size]


def lifter(comments):
    """A discovered Lifter over the image, with the tables the new processes are sent filled in: code after an instruction,
    names, functions without local registers, a jump table for half of the BRs, a usual target for half of the BLRs"""
    L = a64_lift.Lifter(Image(), comments=comments)
    L.discover()
    order = sorted(L.funcs)
    for i, e in enumerate(order):
        if i % 300 == 7:
            L.after[e + 8] = "/* after %#x */" % e
        if i % 500 == 11:
            L.names[e] = "named_%d" % i
        if i % 7 == 1:
            L.no_local.add(e)
    for kind in ("br", "blr"):
        sites = [pc for e in order for pc, (k, w) in L.funcs[e][0].items() if k[0] == kind]
        for n, pc in enumerate(sites[::2]):
            if kind == "br":
                L.jt[pc] = [order[(n * 7 + 1) % len(order)], order[(n * 7 + 2) % len(order)]]
            else:
                L.devirt[pc] = order[(n * 5 + 3) % len(order)]
    return L


def write(jobs, gen, comments=False, counters=(), spawn=False, cls=None):
    """chunks.write over a fresh lifter: (files' bytes by name, the counters, what the parent printed)"""
    L = lifter(comments)
    if cls:
        L.__class__ = cls
    out = io.StringIO()
    with contextlib.ExitStack() as stack:
        stack.enter_context(contextlib.redirect_stdout(out))
        if spawn:
            stack.enter_context(mock.patch.object(chunks, "_no_fork", return_value=True))
        chunks.write(L, gen, gen, jobs=jobs, counters=counters)
    texts = {}
    for name in sorted(os.listdir(gen)):
        if name.endswith(".c") or name.endswith(".h"):
            with open(os.path.join(gen, name), "rb") as f:
                texts[name] = f.read()
    return texts, (dict(L.stats), dict(L.unhandled), dict(L.ijump), [dict(c) for c in counters]), out.getvalue(), L


@contextlib.contextmanager
def quiet_stderr():
    """File descriptor 2 to the null device (the new processes inherit it)"""
    sys.stderr.flush()
    saved = os.dup(2)
    null = os.open(os.devnull, os.O_WRONLY)
    try:
        os.dup2(null, 2)
        yield
    finally:
        os.dup2(saved, 2)
        os.close(saved)
        os.close(null)


class Sub(a64_lift.Lifter):
    def emit_function(self, e, texts):
        text, refs = super().emit_function(e, texts)
        return "/* sub */\n" + text, refs


class SpawnEmitTests(unittest.TestCase):
    tmp = None

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("test_fpx_sticky", ROOT / "tests" / "test_fpx_sticky.py")
        fpx = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fpx)   # (its compiler lookup and build helper, which put the translator in a temporary folder)
        if platform.machine().lower() not in ("x86_64", "amd64"):
            raise unittest.SkipTest("not an x86-64 host")
        cxx = fpx.compiler(ARGS.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
        if not cxx:
            raise unittest.SkipTest("no C++ compiler")
        cls.tmp = tempfile.TemporaryDirectory(prefix="hookshot-spawn-emit-")
        dll = os.path.join(cls.tmp.name, "a64emit.dll" if os.name == "nt" else "liba64emit.so")
        fpx.build([cxx, "-std=c++20", "-O0", "-shared", "-Wno-everything", "-o", dll,
                   os.path.join(str(ROOT), "third_party", "a64-kit", "lift", "a64emit.cpp")]
                  + fpx.target_flags(cxx, cls.tmp.name) + ([] if os.name == "nt" else ["-fPIC"]), "a64emit")
        os.environ["HOOKSHOT_TEST_A64EMIT"] = dll   # (inherited by the spawned workers: see the top of this file)
        build_emit.build = lambda force=False: dll
        cls.dll = dll

    @classmethod
    def tearDownClass(cls):
        os.environ.pop("HOOKSHOT_TEST_A64EMIT", None)
        import a64emit
        if a64emit._lib is not None:   # (unloaded, so the folder can go)
            import _ctypes
            (_ctypes.FreeLibrary if os.name == "nt" else _ctypes.dlclose)(a64emit._lib._handle)
            a64emit._lib = None
        if cls.tmp:
            cls.tmp.cleanup()

    def folder(self, name):
        path = os.path.join(self.tmp.name, name)
        os.makedirs(path)
        return path

    def compare(self, name, spawn_used=True, **kw):
        """chunks.write with 1 process and with 3 spawned: the same files and counters. Returns the 1-process run"""
        one = write(1, self.folder(name + "-one"), **kw)
        with mock.patch.object(chunks, "spawn_emit", wraps=chunks.spawn_emit) as spawned:
            many = write(3, self.folder(name + "-many"), spawn=True, **kw)
        self.assertEqual(spawned.called, spawn_used, "%s: the spawned emit %s" % (name, "was not used" if spawn_used else "was used"))
        self.assertGreaterEqual(len(one[3].funcs), 4000)
        self.assertEqual(sorted(one[0]), sorted(many[0]), name)
        for f in one[0]:
            self.assertEqual(one[0][f], many[0][f], "%s: %s" % (name, f))
        self.assertEqual(one[1], many[1], name)
        return one, many

    def has(self, texts, *needles):
        for n in needles:
            self.assertTrue(any(n in t for t in texts.values()), "%r is in no file: the made-up image does not reach it" % n)

    def test_spawned_processes_write_what_one_writes(self):
        import poll
        # the lift's plain configuration (--no-polls)
        (texts, counts, _, L), (_, _, printed, _) = self.compare("plain")
        self.assertNotIn("worker process stopped", printed)   # (else the files above are the parent's fallback, not the workers')
        self.has(texts, b"switch (c->x[5])", b"if (RECOMP_LIKELY(_t == 0x", b"recomp_sj_push(", b"/* after 0x", b"A64_ICALL(",
                 b"A64_IJUMP(", b"0xb8200000u); return;", b"c->x[17] = 0x")
        self.assertTrue(counts[0].get("devirt_sites") and counts[0].get("setjmp_call_sites") and counts[0].get("after_patches"))
        self.assertTrue(counts[1] and counts[2], "no unhandled word or indirect jump counted")
        self.assertTrue(L.setjmp_owner and L.plt and L.no_local and L.names)
        # a translator that is not the parent's: the workers stop as they start, and the parent emits the runs
        import a64emit
        real_translate = a64emit.translate

        def other(items, batch=65536):
            return real_translate(items, batch)
        with mock.patch.object(a64emit, "translate", other), quiet_stderr():   # (the workers log why they stop)
            _, (_, _, printed, _) = self.compare("mismatch")
        self.assertIn("worker process stopped or could not start", printed)
        # a subclass: its own emit_function, which the workers would not call
        self.compare("subclass", spawn_used=False, cls=Sub)
        # what nx2/lift.py does: poll.install() and counters=[poll.stats]; the registers local and comments on
        poll.install()
        polls = poll.stats

        def reset():
            polls["functions"] = polls["polls"] = 0
        with mock.patch.dict(os.environ, {"A64_LOCAL_REGS": "4"}):
            reset()
            one = write(1, self.folder("full-one"), comments=True, counters=[polls])
            reset()
            with mock.patch.object(chunks, "spawn_emit", wraps=chunks.spawn_emit) as spawned:
                many = write(3, self.folder("full-many"), comments=True, counters=[polls], spawn=True)
        self.assertTrue(spawned.called, "the spawned emit was not used")
        self.assertNotIn("worker process stopped", many[2])
        self.assertEqual(sorted(one[0]), sorted(many[0]))
        for f in one[0]:
            self.assertEqual(one[0][f], many[0][f], f)
        self.assertEqual(one[1], many[1])
        self.assertGreater(one[1][3][0]["polls"], 0, "no loop was polled: the made-up image has no backward branch")
        self.assertGreater(one[1][3][0]["functions"], 0)
        self.has(one[0], b"A64_POLL(c)", b"/* named_", b"/* 80", b"x0_")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx")   # (run_all.py's compiler: the translator is built with it)
    ARGS, rest = ap.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + rest)
