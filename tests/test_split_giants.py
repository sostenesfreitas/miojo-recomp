"""nx2/split_giants.py: giant lifted functions cut at their labels into pieces and a dispatch loop, game-free.

Text checks: what is cut (pieces of about the size asked, every label in exactly one piece, the function's own name kept,
gotos into other pieces become returns of the label's id with their interrupt poll kept, guest returns and tail calls
become ids the dispatcher turns into `return` and TAILCALL), what is left whole and why (a top-level declaration, a label
inside a block, a literal, a goto to a label the function lacks, a function too small or below the size asked), and
build.py's switch (on at 8 KB unless HOOKSHOT_SPLIT_GIANTS says otherwise).

Compiled checks (Clang: the build compiles lifted code with clang on every OS, nx2/build.py `toolchain.find("clang")`),
each a function compiled whole and split into one program against the build's own guest model (nx2/rtgen.py's a64.h),
run from the same random states: the context, the guest memory, the page marks, the calls out (each with a hash of the
context), the setjmp resumes and the interrupt polls must be equal.
  1. Random functions in the lifter's shapes, written here: blocks with their own locals, guest loads and stores,
     conditional gotos forward and back, loops whose back edges poll, a jump-table switch with an indirect jump as its
     default, calls, tail calls, returns, and PR #38's fast-float region shape (one statement with its own label).
  2. Functions emitted by the real lifter: a64_lift.Lifter.emit_function, with nx2/lift_fixes.py and nx2/poll.py
     installed as the lift installs them, over a made-up image (instruction texts made up, as
     tests/test_lift_entry_below.py does; the control flow, labels, polls, calls and setjmp wrapper are the emitter's):
     an entry with code below it (lift_fixes' entry goto), loops, B.cond / TBZ / CBNZ, BL, BLR, a branch into another
     entry (a tail call), and a call of a setjmp owner, whose stub sometimes longjmps back so the caller resumes through
     recomp_sj_setjmp (the host's own assembly setjmp/longjmp, copied from host/hos_setjmp.c) and a64_icall. The same
     function emitted with A64_LOCAL_REGS or A64_LOCAL_FLAGS must be left whole.
  3. nx2/build.py's compile_one with the split flag, as the build calls it (an object cache of its own): it compiles the
     split copy (the object holds the pieces), removes the copy, and the object runs as the whole function does.
PR #38's real fast-float emitter is not on main, so its regions are covered by the written shape in 1 only.

A compiler that is missing or is not Clang fails the check (run_all.py skips it instead when it has no clang, or fails
it with --strict: RUN_ALL_ARGS below); compiled checks are never skipped silently.

    python -I -B tests/test_split_giants.py [--cc clang]
"""
import argparse
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import split_giants as SG  # noqa: E402

RUN_ALL_ARGS = ["--cc", "clang"]

FAILS = []


def check(ok, what):
    if not ok:
        FAILS.append(what)
        print("FAIL: " + what)


def gen_function(name, rng, blocks=60):
    """A lifted-shaped function: blocks of statements, each led by a label; x28 counts down so every loop ends"""
    labels = ["L_%08x" % (0x1000 + 4 * i) for i in range(blocks)]
    out = ["void %s(Ctx *__restrict A64_NOESC c) {" % name]
    for i, lab in enumerate(labels):
        out.append("%s:;" % lab if rng.random() > 0.15 else "%s:; A64_POLL(c);" % lab)
        for _ in range(rng.randint(3, 12)):
            d, n, m = rng.randint(0, 27), rng.randint(0, 27), rng.randint(0, 27)
            k = rng.random()
            if k < 0.3:
                out.append("    { uint64_t _a=c->x[%d], _b=c->x[%d]; uint64_t _r=_a%s_b; c->x[%d]=_r; }" % (n, m, rng.choice("+-^&|"), d))
            elif k < 0.45:
                out.append("    { uint64_t _r = ((uint64_t)0 | ((c->x[%d]) << %d)); _r &= 0xFFFFFFFFULL; c->x[%d] = _r; }" % (n, rng.randint(0, 5), d))
            elif k < 0.6:
                out.append("    c->x[%d]=recomp_load32(c,(c->x[%d] & 0xFFF0) + %d);" % (d, n, 4 * rng.randint(0, 3)))
            elif k < 0.72:
                out.append("    { uint64_t _b=c->x[%d]; int64_t _o=%d; recomp_store64(c,((_b & 0xFFF0)+_o),c->x[%d]); }" % (n, 8 * rng.randint(0, 1), m))
            elif k < 0.8:
                out.append("    { uint32_t _n[4],_r[4]; memcpy(_n,c->vreg[%d],16); for(unsigned _i=0;_i<4;++_i) _r[_i]=_n[_i]*3u+%du; memcpy(c->vreg[%d],_r,16); }"
                           % (rng.randint(0, 7), rng.randint(0, 9), rng.randint(0, 7)))
            elif k < 0.87:
                out.append("    { uint64_t _a=c->x[%d], _b=c->x[%d]; uint64_t _r=_a-_b; c->n=(int64_t)_r<0; c->z=_r==0; c->c=_a>=_b; c->v=0; }" % (n, m))
            elif k < 0.89:   # (PR #38's fast float region: one statement with its own label and a goto to it)
                r_ = rng.randint(0, 27)
                lab_ff = "L_ff_%08x" % (0x5000000 + 16 * i + _)
                out.append("    { if (c->x[%d] & 1) { const uint64_t _ffx%d = c->x[%d]; c->x[%d] += 7;\n"
                           "if (__builtin_expect((c->x[%d] & 2) != 0, 1)) goto %s;\nc->x[%d] = _ffx%d; }\n"
                           "    c->x[%d] ^= 5;\n%s:; }" % (n, r_, r_, r_, r_, lab_ff, r_, r_, r_, lab_ff))
            elif k < 0.92:
                out.append("    c->x[30] = 0x%xULL; f_callee(c);" % (0x2000 + i))
            else:
                out.append("    { uint64_t _t = c->x[%d]; c->x[30] = 0x%xULL; A64_ICALL(c, _t); }" % (n, 0x3000 + i))
        r = rng.random()
        if i + 1 == blocks:
            out.append("return;")
        elif r < 0.25 and i > 2:   # a loop back
            out.append("{ c->x[28] -= 1; }")
            out.append("if (c->x[28] < 0x10000 && c->x[28] != 0) { { A64_POLL(c); goto %s; } }" % labels[rng.randint(max(0, i - 8), i)])
        elif r < 0.45:
            out.append("if %s { goto %s; }" % (rng.choice(["(c->z)", "(!c->z)", "(c->c && !c->z)", "(c->n != c->v)"]),
                                               labels[rng.randint(i + 1, min(blocks - 1, i + 15))]))
        elif r < 0.52:
            tg = sorted(set(rng.randint(i + 1, blocks - 1) for _ in range(4)))
            out.append("switch (c->x[%d] & 7) { %s default: A64_IJUMP(c, c->x[%d]); return; }"
                       % (rng.randint(0, 27), " ".join("case 0x%xULL: goto %s;" % (j, labels[t]) for j, t in enumerate(tg)),
                          rng.randint(0, 27)))
        elif r < 0.56:
            out.append("if (c->x[%d] & 1) { TAILCALL(f_tail); }" % rng.randint(0, 27))
        elif r < 0.6:
            out.append("if ((uint32_t)c->x[%d] == 0) { return; }" % rng.randint(0, 27))
    out.append("}")
    return "\n".join(out)


# ---- functions emitted by the real lifter (2) -------------------------------------------------------------------------
BASE = 0x80000000
LOW, MAIN = BASE + 0x1000, BASE + 0x2000
CALLEE, OWNER, TAIL = BASE + 0x100000, BASE + 0x100100, BASE + 0x100200
RET = 0xd65f03c0


def _rel(pc, t, bits):
    return ((t - pc) >> 2) & ((1 << bits) - 1)


class _Image:
    """a made-up code image: the words, every function's entry"""
    hooks = {}
    names = {}

    def __init__(self, words, entries):
        self.words, self.entries = words, set(entries)

    def word(self, pc):
        return self.words[pc]

    def in_code(self, pc):
        return pc in self.words

    def read(self, pc, n):
        return b"".join(struct.pack("<I", self.words[pc + 4 * i]) for i in range(n // 4))


def made_up_program(rng, blocks):
    """(words, texts): MAIN, an entry with code below it (LOW branches back up into it), in blocks of made-up instructions
    each ending in a branch; CALLEE, OWNER (a setjmp owner) and TAIL, each a lone RET. texts: what the translator would
    hand emit_function for each made-up instruction word: C in the emitter's own shapes"""
    texts, words = {}, {}

    def op(word, text):
        texts[word] = (text, 0)
        return word
    work = [r for r in range(3, 28) if r != 9]   # (x1, x2: memory bases, never written; x9: BLR's target; x28: the count)

    def body_op():
        k = rng.random()
        d, n, imm = rng.choice(work), rng.choice(work), rng.randint(1, 4095)
        if k < 0.4:   # ADD Xd, Xn, #imm
            return op(0x91000000 | imm << 10 | n << 5 | d, "    { uint64_t _a=c->x[%d], _b=%dULL; uint64_t _r=_a+_b; c->x[%d]=_r; }" % (n, imm, d))
        if k < 0.6:   # LDR Xd, [X1|X2, #o]
            b, o = rng.choice((1, 2)), 8 * rng.randint(0, 63)
            return op(0xF9400000 | (o // 8) << 10 | b << 5 | d, "    c->x[%d]=recomp_load64(c,c->x[%d] + %d);" % (d, b, o))
        if k < 0.8:   # STR Xn, [X1|X2, #o]
            b, o = rng.choice((1, 2)), 8 * rng.randint(0, 63)
            return op(0xF9000000 | (o // 8) << 10 | b << 5 | n,
                      "    { uint64_t _b=c->x[%d]; int64_t _o=%d; recomp_store64(c,(_b+_o),c->x[%d]); }" % (b, o, n))
        # CMP Xn, #imm
        return op(0xF100001F | imm << 10 | n << 5,
                  "    { uint64_t _a=c->x[%d], _b=%dULL; uint64_t _r=_a-_b; c->n=(int64_t)_r<0; c->z=_r==0; c->c=_a>=_b; "
                  "c->v=((int64_t)_a<0)!=((int64_t)_b<0)&&((int64_t)_r<0)!=((int64_t)_a<0); }" % (n, imm))
    dec = op(0xD1000400 | 28 << 5 | 28, "    if (c->x[28]) c->x[28] -= 1;")   # SUB X28, X28, #1 (stops at 0: every loop ends)
    plan = [[body_op() for _ in range(rng.randint(5, 12))] for _ in range(blocks)]
    starts, pc = [], MAIN + 8   # (MAIN: an op and a branch down to LOW, which comes back up to MAIN + 8)
    for ops in plan:
        starts.append(pc)
        pc += 4 * (len(ops) + 2)
    words[MAIN] = body_op()
    words[MAIN + 4] = 0x14000000 | _rel(MAIN + 4, LOW, 26)
    words[LOW] = body_op()
    words[LOW + 4] = 0x14000000 | _rel(LOW + 4, MAIN + 8, 26)
    for i, ops in enumerate(plan):
        pc = starts[i]
        for w in ops:
            words[pc] = w
            pc += 4
        nxt = starts[i + 1] if i + 1 < blocks else None
        r = rng.random()
        if nxt is None:
            words[pc], words[pc + 4] = body_op(), RET
        elif r < 0.25 and i > 1:   # a loop: x28 counts down, CBNZ back
            words[pc] = dec
            words[pc + 4] = 0xB5000000 | _rel(pc + 4, starts[rng.randint(max(0, i - 4), i)], 19) << 5 | 28
        elif r < 0.45:   # B.cond forward
            words[pc] = body_op()
            words[pc + 4] = 0x54000000 | _rel(pc + 4, starts[rng.randint(i + 1, min(blocks - 1, i + 6))], 19) << 5 | rng.choice((0, 1, 2, 3, 10, 11, 12))
        elif r < 0.55:   # TBZ / TBNZ forward
            words[pc] = body_op()
            words[pc + 4] = (0x36000000 | rng.choice((0, 0x01000000))) | rng.randint(0, 31) << 19 | \
                _rel(pc + 4, starts[rng.randint(i + 1, min(blocks - 1, i + 6))], 14) << 5 | rng.choice(work)
        elif r < 0.68:   # BL CALLEE
            words[pc] = body_op()
            words[pc + 4] = 0x94000000 | _rel(pc + 4, CALLEE, 26)
        elif r < 0.78:   # BL OWNER (the setjmp owner: the emitter wraps the call)
            words[pc] = body_op()
            words[pc + 4] = 0x94000000 | _rel(pc + 4, OWNER, 26)
        elif r < 0.86:   # BLR X9
            words[pc] = body_op()
            words[pc + 4] = 0xD63F0000 | 9 << 5
        elif r < 0.93:   # CBZ Xn into another entry: a tail call
            words[pc] = body_op()
            words[pc + 4] = 0xB4000000 | _rel(pc + 4, TAIL, 19) << 5 | rng.choice(work)
        else:
            words[pc], words[pc + 4] = body_op(), body_op()
    for e in (CALLEE, OWNER, TAIL):
        words[e] = RET
    return words, texts


def emit_real(seed, blocks, env=None):
    """MAIN's C as the lift writes it (the emitter with lift_fixes and poll installed), the names it calls, the lifter"""
    import kit  # noqa: F401  (a64-kit on the path)
    import a64_lift
    import lift_fixes
    import poll
    lift_fixes.install()
    poll.install()
    words, texts = made_up_program(random.Random(seed), blocks)
    L = a64_lift.Lifter(_Image(words, (MAIN, CALLEE, OWNER, TAIL)))
    L.discover()
    L.setjmp_owner = {OWNER: [MAIN + 0x7770]}   # (a setjmp return address inside the owner: its slot's first target)
    old = {k: os.environ.get(k) for k in ("A64_LOCAL_REGS", "A64_LOCAL_FLAGS")}
    try:
        for k in old:
            os.environ.pop(k, None)
        os.environ.update(env or {})
        text, refs = L.emit_function(MAIN, texts)
    finally:
        for k, v in old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    return text, sorted(refs), L


def real_text_checks():
    text, refs, L = emit_real(7, 40)
    name = "f_%08x" % MAIN
    check(text.splitlines()[1] == "goto L_%08x;" % MAIN, "the emitted function starts with lift_fixes' entry goto")
    check("A64_POLL(c)" in text and "recomp_sj_setjmp(" in text and "TAILCALL(f_%08x)" % TAIL in text
          and "A64_ICALL(c" in text, "the emitted function has polls, a setjmp wrapper, a tail call and an indirect call")
    out, st = SG.split_text(text, piece_kb=1, min_kb=0, only={name})
    check(st["functions_split"] == 1 and st["pieces"] >= 4, "the emitted function is cut: %s" % dict(st))
    for env, what in (({"A64_LOCAL_REGS": "8"}, "A64_LOCAL_REGS"), ({"A64_LOCAL_FLAGS": "1"}, "A64_LOCAL_FLAGS")):
        t2, _, _ = emit_real(7, 40, env)
        check(t2 != text, "%s changes the emitted text" % what)
        notes = []
        o, s = SG.split_text(t2, piece_kb=1, min_kb=0, only={name}, notes=notes)
        check(o == t2 and not s["functions_split"] and any("top-level declaration" in n for n in notes),
              "%s: the function is left whole (its locals cross labels): %s" % (what, notes))


# ---- the compile_one integration (3) and the text checks -----------------------------------------------------------
def text_checks():
    rng = random.Random(1)
    fn = gen_function("f_00001000", rng)
    src = '#include "a64.h"\nvoid f_callee(Ctx *A64_NOESC);\nvoid f_tail(Ctx *A64_NOESC);\n' + fn + "\n"
    out, st = SG.split_text(src, piece_kb=1, min_kb=0)
    check(st["functions_split"] == 1 and st["pieces"] >= 4, "a 60-block function cut into pieces: %s" % dict(st))
    check(out.count("\nvoid f_00001000(Ctx *__restrict A64_NOESC c) {") == 1, "the function keeps its name and ABI once")
    for i in range(60):
        check(out.count("L_%08x:;" % (0x1000 + 4 * i)) == 1, "label %d in exactly one piece" % i)
    check(("TAILCALL(f_tail);" in fn) == ("TAILCALL(f_tail);" in out.split("\nvoid f_00001000(")[1]),
          "a tail call leaves from the dispatcher")
    check("TAILCALL" not in out.split("\nvoid f_00001000(")[0], "no tail call left in a piece")
    check("return;" not in out.split("\nvoid f_00001000(")[0], "no bare return left in a piece")
    check("A64_POLL(c); return " in out or "A64_POLL(c); goto" in out, "polls kept on back edges")
    check("L_ff_" in fn and out.count(":; }") == fn.count(":; }"), "fast float regions kept whole, their labels inside them")
    whole, st2 = SG.split_text(src, piece_kb=1, min_kb=1024)
    check(whole == src and not st2["functions_split"], "a function below the size asked stays as it is")
    for bad, why in (("    uint64_t x8_ = c->x[8];", "a top-level declaration"),
                     ('    hos_log(c, "a{");', "literal"),
                     ("    { if (c->z) { L_0000dead:; } }", "label inside a block"),
                     ("    if (c->z) { goto L_ffffffff; }", "label the function does not have"),
                     ("    if (c->z) { goto *c; }", "goto of another shape"),
                     ("    if (c->z) { goto L_ff_00000001; }\n    { c->x[1] = 0; L_ff_00000001:; }", "goto of another shape")):
        lines = fn.split("\n")
        lines.insert(len(lines) // 2, bad)
        notes = []
        o, s = SG.split_text('#include "a64.h"\n' + "\n".join(lines) + "\n", piece_kb=1, min_kb=0, notes=notes)
        check(not s["functions_split"] and s["functions_left_whole"] == 1, "left whole: " + why)
        check(any(why in n for n in notes), "the note says why (%s): %s" % (why, notes))
    # build.py: the switch, and the copy compile_copy makes (compile_one itself: compiled check 3)
    import build
    old = os.environ.get("HOOKSHOT_SPLIT_GIANTS")
    try:
        os.environ["HOOKSHOT_SPLIT_GIANTS"] = "0"
        check(build.split_giants_flags() == [], "HOOKSHOT_SPLIT_GIANTS=0: no flag")
        os.environ.pop("HOOKSHOT_SPLIT_GIANTS", None)
        f = build.split_giants_flags()
        check(len(f) == 1 and f[0].startswith(SG.FLAG + "%d_" % SG.PIECE_KB), "unset: on at %d KB by default: %s" % (SG.PIECE_KB, f))
        os.environ["HOOKSHOT_SPLIT_GIANTS"] = "16"
        f = build.split_giants_flags()
        check(len(f) == 1 and f[0].startswith(SG.FLAG + "16_"), "HOOKSHOT_SPLIT_GIANTS=16: the flag names the size: %s" % f)
    finally:
        if old is None:
            os.environ.pop("HOOKSHOT_SPLIT_GIANTS", None)
        else:
            os.environ["HOOKSHOT_SPLIT_GIANTS"] = old
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "code_000.c")
        open(p, "w", newline="\n").write(big_source("f_00002000"))
        obj = os.path.join(d, "code_000.o")
        copy = SG.compile_copy(p, obj, ["-O1", SG.flag(1)])
        check(copy == obj[:-2] + ".split.c" and os.path.exists(copy), "compile_copy writes the split copy beside the object")
        check(SG.compile_copy(p, obj, ["-O1"]) == p, "no flag: the lifted file itself")
        small = os.path.join(d, "code_001.c")
        open(small, "w", newline="\n").write(src)
        check(SG.compile_copy(small, obj, ["-O1", SG.flag(8)]) == small, "no function past MIN_KB: the lifted file itself")
        check(build.mem_need(copy) < build.mem_need(p), "the memory estimate counts the pieces as functions")


def big_source(name):
    text = '#include "a64.h"\nvoid f_callee(Ctx *A64_NOESC);\nvoid f_tail(Ctx *A64_NOESC);\n' + gen_function(name, random.Random(2), 600) + "\n"
    check(len(text) > SG.MIN_KB * 1024, "the big test function is past MIN_KB")
    return text


# ---- the program each compiled check runs: FN_A whole, FN_B split, from the same states ------------------------------
DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "a64.h"
uint8_t guest_mem[1 << 16];
uint8_t a64_dirty[A64_DIRTY_PAGES];
void FN_A(Ctx *A64_NOESC c);
void FN_B(Ctx *A64_NOESC c);
static uint64_t log_[4096]; static int nlog, polls, resumes;
static uint64_t h(const Ctx *c) { uint64_t x = 1469598103934665603ULL; for (int i = 0; i < 32; i++) x = (x ^ c->x[i]) * 1099511628211ULL;
    for (int i = 0; i < 8; i++) x = (x ^ c->vreg[i][0] ^ c->vreg[i][1]) * 1099511628211ULL; return x ^ c->n ^ c->z << 1 ^ c->c << 2 ^ c->v << 3; }
static void ev(Ctx *c, uint64_t kind) { if (nlog < 4096) log_[nlog++] = kind ^ h(c); c->x[0] = h(c) & 0xffff; }
void a64_icall_site(Ctx *A64_NOESC c, uint64_t t, uint64_t *s) { ev(c, 3 + (t << 8)); }
void a64_ijump_site(Ctx *A64_NOESC c, uint64_t t, uint64_t *s) { ev(c, 4 + (t << 8)); }
void a64_icall(Ctx *A64_NOESC c, uint64_t t) { ev(c, 3 + (t << 8)); }
void a64_ijump(Ctx *A64_NOESC c, uint64_t t) { ev(c, 4 + (t << 8)); }
A64Fn a64_lookup(uint64_t a) { return 0; }
void a64_poll_cur(void) { polls++; }
void a64_poll(Ctx *A64_NOESC c) { polls++; }

/* setjmp in lifted code: host/hos_setjmp.c's own assembly setjmp and longjmp, and a stack of slots */
typedef struct { uint64_t rets[4], target; void *buf[32]; } SjSlot;
static SjSlot sj[64]; static int sjd;
#ifdef _WIN32
__attribute__((naked, returns_twice)) int recomp_sj_setjmp(void **buf) {
    __asm__ volatile("movq %rbx, 0(%rcx)\n\t" "movq %rbp, 8(%rcx)\n\t" "movq %rdi, 16(%rcx)\n\t" "movq %rsi, 24(%rcx)\n\t"
        "movq %r12, 32(%rcx)\n\t" "movq %r13, 40(%rcx)\n\t" "movq %r14, 48(%rcx)\n\t" "movq %r15, 56(%rcx)\n\t"
        "leaq 8(%rsp), %rdx\n\t" "movq %rdx, 64(%rcx)\n\t" "movq (%rsp), %rdx\n\t" "movq %rdx, 72(%rcx)\n\t"
        "movdqu %xmm6, 80(%rcx)\n\t" "movdqu %xmm7, 96(%rcx)\n\t" "movdqu %xmm8, 112(%rcx)\n\t" "movdqu %xmm9, 128(%rcx)\n\t"
        "movdqu %xmm10, 144(%rcx)\n\t" "movdqu %xmm11, 160(%rcx)\n\t" "movdqu %xmm12, 176(%rcx)\n\t" "movdqu %xmm13, 192(%rcx)\n\t"
        "movdqu %xmm14, 208(%rcx)\n\t" "movdqu %xmm15, 224(%rcx)\n\t" "stmxcsr 240(%rcx)\n\t" "fnstcw 244(%rcx)\n\t"
        "xorl %eax, %eax\n\t" "ret\n\t");
}
__attribute__((naked, noreturn)) static void recomp_sj_longjmp(void **buf, int value) {
    __asm__ volatile("movq 0(%rcx), %rbx\n\t" "movq 8(%rcx), %rbp\n\t" "movq 16(%rcx), %rdi\n\t" "movq 24(%rcx), %rsi\n\t"
        "movq 32(%rcx), %r12\n\t" "movq 40(%rcx), %r13\n\t" "movq 48(%rcx), %r14\n\t" "movq 56(%rcx), %r15\n\t"
        "movdqu 80(%rcx), %xmm6\n\t" "movdqu 96(%rcx), %xmm7\n\t" "movdqu 112(%rcx), %xmm8\n\t" "movdqu 128(%rcx), %xmm9\n\t"
        "movdqu 144(%rcx), %xmm10\n\t" "movdqu 160(%rcx), %xmm11\n\t" "movdqu 176(%rcx), %xmm12\n\t" "movdqu 192(%rcx), %xmm13\n\t"
        "movdqu 208(%rcx), %xmm14\n\t" "movdqu 224(%rcx), %xmm15\n\t" "ldmxcsr 240(%rcx)\n\t" "fldcw 244(%rcx)\n\t"
        "movl %edx, %eax\n\t" "testl %eax, %eax\n\t" "jnz 1f\n\t" "incl %eax\n\t" "1:\n\t"
        "movq 64(%rcx), %rsp\n\t" "jmpq *72(%rcx)\n\t");
}
#else
__attribute__((naked, returns_twice)) int recomp_sj_setjmp(void **buf) {
    __asm__ volatile("movq %rbx, 0(%rdi)\n\t" "movq %rbp, 8(%rdi)\n\t" "movq %r12, 32(%rdi)\n\t" "movq %r13, 40(%rdi)\n\t"
        "movq %r14, 48(%rdi)\n\t" "movq %r15, 56(%rdi)\n\t" "leaq 8(%rsp), %rdx\n\t" "movq %rdx, 64(%rdi)\n\t"
        "movq (%rsp), %rdx\n\t" "movq %rdx, 72(%rdi)\n\t" "stmxcsr 240(%rdi)\n\t" "fnstcw 244(%rdi)\n\t"
        "xorl %eax, %eax\n\t" "ret\n\t");
}
__attribute__((naked, noreturn)) static void recomp_sj_longjmp(void **buf, int value) {
    __asm__ volatile("movq 0(%rdi), %rbx\n\t" "movq 8(%rdi), %rbp\n\t" "movq 32(%rdi), %r12\n\t" "movq 40(%rdi), %r13\n\t"
        "movq 48(%rdi), %r14\n\t" "movq 56(%rdi), %r15\n\t" "ldmxcsr 240(%rdi)\n\t" "fldcw 244(%rdi)\n\t"
        "movl %esi, %eax\n\t" "testl %eax, %eax\n\t" "jnz 1f\n\t" "incl %eax\n\t" "1:\n\t"
        "movq 64(%rdi), %rsp\n\t" "jmpq *72(%rdi)\n\t");
}
#endif
void **recomp_sj_push(Ctx *A64_NOESC c, uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3) {
    SjSlot *s = &sj[sjd++]; s->rets[0] = r0, s->rets[1] = r1, s->rets[2] = r2, s->rets[3] = r3, s->target = 0; return s->buf;
}
uint64_t recomp_sj_target(void **buf) { return ((SjSlot *)((char *)buf - offsetof(SjSlot, buf)))->target; }
void recomp_sj_pop(void **buf) { int i = (int)((SjSlot *)((char *)buf - offsetof(SjSlot, buf)) - sj); if (i >= 0 && i < sjd) sjd = i; }
/* the setjmp owner: sometimes its code longjmps back to the newest slot (the caller resumes at the setjmp return) */
static void owner(Ctx *c) {
    ev(c, 9);
    if ((c->x[0] & 3) == 1 && sjd) { SjSlot *s = &sj[sjd - 1]; s->target = s->rets[0]; resumes++; recomp_sj_longjmp(s->buf, 1); }
}
@@STUBS@@

int main(void) {
    static uint8_t mem0[1 << 16], mem1[1 << 16]; static uint64_t log0[4096];
    int bad = 0, resumes_a = 0;
    for (int s = 1; s <= 300; s++) {
        Ctx st; memset(&st, 0, sizeof st); srand(s);
        for (int i = 0; i < 32; i++) st.x[i] = ((uint64_t)rand() << 20) ^ rand();
        for (int i = 0; i < 32; i++) { st.vreg[i][0] = rand(); st.vreg[i][1] = (uint64_t)rand() << 33; }
        st.x[1] = 0x1000 + 8 * (rand() % 512); st.x[2] = 0x6000 + 8 * (rand() % 512);   /* (the made-up loads' and stores' bases) */
        st.x[28] = 40 + rand() % 40; st.n = rand() & 1; st.z = rand() & 1; st.c = rand() & 1; st.v = rand() & 1; st.poll = s & 1;
        for (size_t i = 0; i < sizeof mem0; i++) mem0[i] = (uint8_t)rand();
        Ctx a = st, b = st; int na, pa, pb, ra;
        memcpy(guest_mem, mem0, sizeof mem0); memset(a64_dirty, 0, 64); nlog = 0; polls = 0; resumes = 0; sjd = 0;
        FN_A(&a); na = nlog; pa = polls; ra = resumes; memcpy(log0, log_, sizeof log_); memcpy(mem1, guest_mem, sizeof mem1);
        uint8_t d0[64]; memcpy(d0, a64_dirty, 64);
        memcpy(guest_mem, mem0, sizeof mem0); memset(a64_dirty, 0, 64); nlog = 0; polls = 0; resumes = 0; sjd = 0;
        FN_B(&b); pb = polls;
        if (memcmp(&a, &b, sizeof a) || na != nlog || memcmp(log0, log_, na * 8) || memcmp(mem1, guest_mem, sizeof mem1)
            || memcmp(d0, a64_dirty, 64) || pa != pb || ra != resumes || sjd) { printf("state %d differs\n", s); bad++; }
        resumes_a += ra;
    }
    printf("%d of 300 states differ, %d setjmp resumes\n", bad, resumes_a);
    return bad != 0;
}
"""


def find_cc(asked):
    """Clang: the one asked, $CC, clang on PATH, or the build's HOOKSHOT_MINGW toolchain; None when there is none"""
    cands = [asked] if asked else [os.environ.get("CC"), "clang"]
    m = os.environ.get("HOOKSHOT_MINGW")
    if m and not asked:
        cands += [os.path.join(m, "bin", n) for n in ("x86_64-w64-mingw32-clang.exe", "clang.exe", "clang")]
    for c in cands:
        if not c:
            continue
        exe = shutil.which(c) or (c if os.path.isfile(c) else None)
        if not exe:
            continue
        r = subprocess.run([exe, "--version"], capture_output=True, text=True)
        if r.returncode == 0 and "clang" in r.stdout.lower():
            return exe
        if asked:
            return None
    return None


class Programs:
    """One temporary folder: the build's guest model, and each program compiled and run"""

    def __init__(self, cc, d):
        import rtgen
        import kit
        self.cc, self.d = cc, d
        rt = rtgen.write(os.path.join(d, "rt64"))
        for h in ("a64_suyu.h", "a64_fpx.h"):   # (the generated FP helpers: none of the shapes here call them)
            open(os.path.join(d, h), "w").write("")
        open(os.path.join(d, "pre.h"), "w").write("#include <stdint.h>\nextern uint8_t guest_mem[];\n")
        self.fl = ["-O1", "-ffp-contract=off", "-fno-strict-aliasing", "-I" + rt, "-I" + d, "-I" + os.path.join(kit.KIT, "rt"),
                   "-DA64_POLLING", "-DA64_WRITE_TRACK", "-DA64_HOST_BASE=((uintptr_t)guest_mem)", "-include",
                   os.path.join(d, "pre.h"), "-Wno-everything", "-mllvm", "-rotation-max-header-size=0"]

    def run(self, tag, sources, fn_a, fn_b, stubs, objects=(), want_resumes=False):
        """sources: {file name: text}; FN_A whole and FN_B split; stubs: C for the functions they call"""
        for n, t in sources.items():
            open(os.path.join(self.d, n), "w", newline="\n").write(t)
        open(os.path.join(self.d, "driver.c"), "w", newline="\n").write(DRIVER.replace("@@STUBS@@", stubs))
        exe = os.path.join(self.d, tag + ".exe")
        r = subprocess.run([self.cc] + self.fl + ["-DFN_A=" + fn_a, "-DFN_B=" + fn_b] +
                           [os.path.join(self.d, n) for n in list(sources) + ["driver.c"]] + list(objects) + ["-o", exe],
                           capture_output=True, text=True)
        check(r.returncode == 0, "%s compiles: %s" % (tag, (r.stdout + r.stderr)[-2000:]))
        if r.returncode:
            return
        r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "exit %d" % r.returncode
        print("%s: %s" % (tag, last))
        check(r.returncode == 0, "%s: the split function runs as the whole one (%s)" % (tag, last))
        if want_resumes:
            check(" 0 setjmp resumes" not in last, "%s: some states resumed through setjmp" % tag)


def compiled_checks(cc):
    stubs_written = "void f_callee(Ctx *A64_NOESC c) { ev(c, 1); }\nvoid f_tail(Ctx *A64_NOESC c) { ev(c, 2); }\n"
    head = '#include "a64.h"\nvoid f_callee(Ctx *A64_NOESC);\nvoid f_tail(Ctx *A64_NOESC);\n'
    with tempfile.TemporaryDirectory() as d:
        P = Programs(cc, d)
        # 1. written shapes
        for k in range(4):
            fn = gen_function("f_00000001", random.Random(100 + k), blocks=50 + 20 * k)
            out, st = SG.split_text(head + fn + "\n", piece_kb=1, min_kb=0)
            check(st["functions_split"] == 1, "program %d split" % k)
            P.run("written %d (%d pieces)" % (k, st["pieces"]),
                  {"orig.c": head + fn + "\n", "split.c": out.replace("f_00000001", "f_00000002").replace("s_00000001_", "s_00000002_")},
                  "f_00000001", "f_00000002", stubs_written)
        # 2. emitted by the real lifter
        main_name, other = "f_%08x" % MAIN, "f_%08x" % (MAIN + 0x0F000000)
        for k, (seed, blocks) in enumerate(((11, 40), (12, 70), (13, 100))):
            text, refs, _ = emit_real(seed, blocks)
            decl = '#include "a64.h"\n__attribute__((returns_twice)) int recomp_sj_setjmp(void **slot);\n' + \
                   "".join("void %s(Ctx *A64_NOESC);\n" % r for r in refs)
            out, st = SG.split_text(decl + text, piece_kb=1, min_kb=0, only={main_name})
            check(st["functions_split"] == 1, "emitted program %d split" % k)
            stubs = "".join("void %s(Ctx *A64_NOESC c) { %s }\n" % (r, "owner(c);" if r == "f_%08x" % OWNER else "ev(c, %d);" % (20 + j))
                            for j, r in enumerate(refs))
            P.run("emitted %d (%d pieces)" % (k, st["pieces"]),
                  {"orig.c": decl + text, "split.c": out.replace(main_name, other).replace("s_%08x_" % MAIN, "s_%08x_" % (MAIN + 0x0F000000))},
                  main_name, other, stubs, want_resumes=True)
        # 3. build.py's compile_one with the split flag
        import build
        big = big_source("f_00002000")
        src = os.path.join(d, "code_000.c")
        open(src, "w", newline="\n").write(big)
        obj = os.path.join(d, "code_000.o")
        old_cache = build.OBJ_CACHE
        build.OBJ_CACHE = os.path.join(d, "objcache")
        try:
            _, secs, err = build.compile_one((P.cc, src, obj, P.fl + [SG.flag(SG.PIECE_KB)], b""))
        finally:
            build.OBJ_CACHE = old_cache
        check(err is None and os.path.exists(obj), "compile_one compiles the chunk with the split flag: %s" % (err or "")[-1500:])
        check(not os.path.exists(obj[:-2] + ".split.c"), "compile_one removes the split copy")
        check(os.path.exists(obj) and b"s_00002000_" in open(obj, "rb").read(), "compile_one's object holds the pieces (it compiled the copy)")
        if err is None:
            P.run("compile_one", {"whole.c": big.replace("f_00002000", "f_00002001")}, "f_00002001", "f_00002000",
                  stubs_written, objects=[obj])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", help="Clang (default: $CC, clang on PATH, or HOOKSHOT_MINGW's)")
    a = ap.parse_args()
    text_checks()
    real_text_checks()
    cc = find_cc(a.cc)
    if not cc:
        check(False, "no Clang to compile with (%s): the build compiles lifted code with clang; give --cc, $CC, PATH or "
                     "HOOKSHOT_MINGW" % (a.cc or "none found"))
    elif sys.maxsize <= 2 ** 32:
        check(False, "a 64-bit host is needed (the guest model's window)")
    else:
        compiled_checks(cc)
    print("split_giants: %s" % ("FAILED (%d)" % len(FAILS) if FAILS else "OK"))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
