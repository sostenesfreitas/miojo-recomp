"""a64-kit lifter: AArch64 code image -> C.

Model (ppc-kit's, see ppc-kit/lifter/ppc_lift.py)
  * Every guest function becomes `void f_XXXXXXXX(Ctx *restrict c)`. A "function" is an entry address plus
    everything reachable from it without crossing another entry. Any address can be an entry and running from it
    is exactly the original behaviour, so extra entries only cost code size. Branches and fallthrough into another
    entry become tail calls.
  * B/B.cond/CBZ/TBZ = goto; BL = C call (LR written first); BLR = A64_ICALL (a64_icall with a cache per site);
    RET = C return.
  * BR: a recovered jump table becomes a switch on the target address; any other BR is an indirect tail call
    (A64_IJUMP). PLT stubs (adrp x16 / ldr x17 / add x16 / br x17) are resolved through the already-relocated
    GOT, so a call to one calls its target directly.
  * Every other instruction is translated by suyu's emitter (a64emit), which writes C against the context.
  * SVC calls a64_svc; BRK, HLT and anything the emitter cannot translate stop the thread through a64_trap.

Input (image): .entries (set), .hooks {addr: c_name}, .word(addr), .in_code(addr), optional .names {addr: str}.
Output: <out>/code_NNN.c, <out>/a64_table.c, <out>/a64_hooks.c, <out>/a64_fpx.h, <out>/functions.txt
"""
import bisect
import os
import re
import struct
import sys
from collections import Counter

import a64emit

COND = ["(c->z)", "(!c->z)", "(c->c)", "(!c->c)", "(c->n)", "(!c->n)", "(c->v)", "(!c->v)",
        "(c->c && !c->z)", "(!(c->c && !c->z))", "(c->n == c->v)", "(c->n != c->v)",
        "((c->n == c->v) && !c->z)", "(!((c->n == c->v) && !c->z))", "(1)", "(1)"]
CHUNK_BYTES = 24 << 20


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def classify(w, pc):
    """(kind, ...) for control flow and specials; ("op",) for everything the emitter translates."""
    if (w & 0x7C000000) == 0x14000000:
        return ("bl" if w & 0x80000000 else "b", pc + 4 * sx(w & 0x3FFFFFF, 26))
    if (w & 0xFF000010) == 0x54000000:
        return ("bcond", pc + 4 * sx((w >> 5) & 0x7FFFF, 19), w & 15)
    if (w & 0x7E000000) == 0x34000000:
        return ("cbz", pc + 4 * sx((w >> 5) & 0x7FFFF, 19), w & 31, w >> 31, (w >> 24) & 1)
    if (w & 0x7E000000) == 0x36000000:
        return ("tbz", pc + 4 * sx((w >> 5) & 0x3FFF, 14), w & 31, ((w >> 31) << 5) | ((w >> 19) & 31), (w >> 24) & 1)
    if (w & 0xFFFFFC1F) == 0xD61F0000:
        return ("br", (w >> 5) & 31)
    if (w & 0xFFFFFC1F) == 0xD63F0000:
        return ("blr", (w >> 5) & 31)
    if (w & 0xFFFFFC1F) == 0xD65F0000:
        return ("ret", (w >> 5) & 31)
    if (w & 0xFFE0001F) == 0xD4000001:
        return ("svc", (w >> 5) & 0xFFFF)
    if (w & 0xFFE0001F) in (0xD4200000, 0xD4400000):
        return ("trap",)
    if (w & 0xFFFFF0FF) == 0xD50330DF:
        return ("isb",)
    if (w & 0xFFFFFFE0) == 0xD50B7520:
        return ("ic",)
    return ("op",)


def pc_relative(w):
    return (w & 0x1F000000) == 0x10000000 or (w & 0x3B000000) == 0x18000000


# suyu's FPX1 guard around every FP lane op: native fast path, exact integer body as the fallback. Inline at every
# site it is most of the IR of FP-heavy functions (87k guards in TOTK's physics chunk, clang -O1 ran for >10 minutes
# on 20 functions), so each guard becomes one call of a helper per (fast op, exact op) pair, written to a64_fpx.h.
FPX_GUARD = re.compile(r"\{if\(RECOMP_FPX_OPEN\(c\)&&RECOMP_LIKELY\(recomp_fpx_(\w+)\(([^;]*?),RECOMP_FPX_FZ\(c\),"
                       r"&(\w+)\)\)\)\{RECOMP_FPX_PROBE\(\d+\);\}else\{RECOMP_FPX_PROBE\(\d+\);(\w+)=recomp_fpe_(\w+)"
                       r"\(c,([^;]*?)\);\}\}")


def outline_fpx(text, used):
    def sub(m):
        fast, fast_args, out, out2, exact, exact_args = m.groups()
        nf, ne = fast_args.count(",") + 1, exact_args.count(",") + 1
        if out != out2 or exact_args.split(",")[:nf] != fast_args.split(","):
            return m.group(0)
        used.add((fast, exact, nf, ne))
        return "%s=a64_fpx_%s_%s(c,%s);" % (out, fast, exact, exact_args)
    return FPX_GUARD.sub(sub, text) if "RECOMP_FPX_OPEN" in text else text


# Straight 2- and 4-lane NEON FADD / FSUB and 4-lane FMUL (.2S / .4S) become one SSE op with the scalar fast path's
# exactness tests on every lane (a64_vfpx_* in a64_fpx.h); when a lane needs the exact body, one out-of-line call runs
# each lane's helper as before. They were one helper call per lane (TOTK: ~124k such ops, ~500k of its 690k FP calls).
VFPX_ADD = re.compile(r"\{ uint32_t _n\[4\],_m\[4\],_r\[4\]=\{0\};memcpy\(_n,c->vreg\[(\d+)\],16\);memcpy\(_m,c->vreg\[(\d+)\],16\);\s*"
                      r"((?:\{uint64_t _a=_n\[\d\],_b=_m\[\d\],_v=0;_v=a64_fpx_(?:add32_add32|sub32_sub32)\(c,_a,_b\);"
                      r"_r\[\d\]=\(uint32_t\)_v;\}\s*)+)memcpy\(c->vreg\[(\d+)\],_r,16\);\}")
VFPX_LANE = re.compile(r"_a=_n\[(\d)\],_b=_m\[(\d)\],_v=0;_v=a64_fpx_(add32_add32|sub32_sub32)\(c,_a,_b\);_r\[(\d)\]")
VFPX_MUL = re.compile(r"\{uint32_t _n\[4\],_m\[4\],_d\[4\];memcpy\(_n,c->vreg\[(\d+)\],16\);memcpy\(_m,c->vreg\[(\d+)\],16\);"
                      r"for\(unsigned _j=0;_j<4;\+\+_j\)\{uint64_t _a=_n\[_j\],_b=_m\[_j\],_z=\(_a\^_b\)&0x80000000ULL,_v;\s*"
                      r"_v=a64_fpx_mul32_fma32\(c,_a,_b,_z\);\s*_d\[_j\]=\(uint32_t\)_v;\}memcpy\(c->vreg\[(\d+)\],_d,16\);\s*\}")


# Vector float compares (FCMGE / FCMGT / FCMEQ, .2S / .4S) were exact integer code per lane (NaN and FZ checks, FPSR
# flags): one SSE compare when no lane is a NaN (and, under FZ, none is subnormal), the exact block otherwise.
VFCMP = re.compile(r"\{ uint32_t _n\[4\],_m\[4\],_r\[4\]=\{0\}; memcpy\(_n,c->vreg\[(\d+)\],16\); memcpy\(_m,c->vreg\[(\d+)\],16\); "
                   r"for\(int _i=0;_i<([24]);_i\+\+\) \{ uint64_t _a=_n\[_i\],_b=_m\[_i\],_v=0; const uint64_t _sign=0x80000000ULL,"
                   r"_exp=0x7f800000ULL,_frac=0x7fffffULL,_quiet=0x400000ULL; .*?"
                   r"if\(_an\|\|_bn\) \{ if\([^;]*\) c->fpsr\|=1; \} else \{ int _eq=_a==_b\|\|!\(\(_a\|_b\)&~_sign\); "
                   r"int _gt=!_eq&&\(\(\(_a\^_b\)&_sign\)\?!\(_a&_sign\):\(\(_a&_sign\)\?_a<_b:_a>_b\)\); \(void\)_gt; \(void\)_eq; "
                   r"if\((_eq\|\|_gt|_gt|_eq)\) _v=~\(uint64_t\)0; \} _r\[_i\]=\(uint32_t\)_v; \} memcpy\(c->vreg\[(\d+)\],_r,16\); \}")
VFCMP_OP = {"_eq||_gt": 0, "_gt": 1, "_eq": 2}
# ...and against zero (FCMxx #0.0): one source, the other operand the constant 0 (on either side)
VFCMPZ = re.compile(r"\{ uint32_t _n\[4\],_r\[4\]=\{0\}; memcpy\(_n,c->vreg\[(\d+)\],16\); "
                    r"for\(int _i=0;_i<([24]);_i\+\+\) \{ uint64_t (_a=0,_b=_n\[_i\]|_a=_n\[_i\],_b=0),_v=0; const uint64_t _sign=0x80000000ULL,"
                    r"_exp=0x7f800000ULL,_frac=0x7fffffULL,_quiet=0x400000ULL; .*?"
                    r"if\(_an\|\|_bn\) \{ if\([^;]*\) c->fpsr\|=1; \} else \{ int _eq=_a==_b\|\|!\(\(_a\|_b\)&~_sign\); "
                    r"int _gt=!_eq&&\(\(\(_a\^_b\)&_sign\)\?!\(_a&_sign\):\(\(_a&_sign\)\?_a<_b:_a>_b\)\); \(void\)_gt; \(void\)_eq; "
                    r"if\((_eq\|\|_gt|_gt|_eq)\) _v=~\(uint64_t\)0; \} _r\[_i\]=\(uint32_t\)_v; \} memcpy\(c->vreg\[(\d+)\],_r,16\); \}")


# Scalar FCMP / FCMPE (S and D, against a register or #0.0): exact integer code too (FZ, NaN and ordering tests, ~5
# branches; ~65k sites in TOTK). One native compare sets the flags when neither operand is a NaN (and, under FZ, neither
# is subnormal); the exact block stays behind it. A64_NO_SFCMP=1 at lift time: off.
SFCMP = re.compile(r"\{ uint64_t _a=0,_b=0; memcpy\(&_a,([^;]+?),([48])\);(?: memcpy\(&_b,([^;]+?),\2\);)? "
                   r"const uint64_t _sign=0x80+ULL,[^\n]*? c->n=\(uint8_t\)_lt; c->z=\(uint8_t\)_eq; "
                   r"c->c=\(uint8_t\)!_lt; c->v=0; \} \}")


def vectorize_fcmp(text):
    if "_quiet=" not in text or os.environ.get("A64_NO_VFPX"):
        return text
    if not os.environ.get("A64_NO_SFCMP"):   # (braced: FCCMP writes "if (cond) <block> else ...")
        if local_flags():   # (the flags stored in the text, where the passes see them: dead ones dropped, locals)
            text = SFCMP.sub(lambda m: "{ const uint32_t _fk = a64_fcmp%dk(c,%s,%s); if (_fk) { c->n=(uint8_t)(_fk&1); "
                             "c->z=(uint8_t)(_fk>>1&1); c->c=(uint8_t)!(_fk&1); c->v=0; } else %s }"
                             % (32 if m.group(2) == "4" else 64, m.group(1), m.group(3) or "a64_vzero", m.group(0)), text)
        else:
            text = SFCMP.sub(lambda m: "{ if (!a64_fcmp%d(c,%s,%s)) %s }" % (32 if m.group(2) == "4" else 64, m.group(1),
                                                                          m.group(3) or "a64_vzero", m.group(0)), text)
    if "_quiet=0x400000ULL" not in text:
        return text
    text = VFCMP.sub(lambda m: "if (!a64_vfcmp(c,c->vreg[%s],c->vreg[%s],c->vreg[%s],%s,%d)) %s"
                     % (m.group(5), m.group(1), m.group(2), m.group(3), VFCMP_OP[m.group(4)], m.group(0)), text)

    def zero(m):
        src = "c->vreg[%s]" % m.group(1)
        a, b = ("a64_vzero", src) if m.group(3).startswith("_a=0") else (src, "a64_vzero")
        return "if (!a64_vfcmp(c,c->vreg[%s],%s,%s,%s,%d)) %s" % (m.group(5), a, b, m.group(2), VFCMP_OP[m.group(4)],
                                                                  m.group(0))
    return VFCMPZ.sub(zero, text)


# Writes of a whole guest vector register as one 16-byte store. The translation writes a scalar result as two 8-byte
# stores (value, then the zeroed top) and an element insert as load-modify-store, which the compiler narrows to a 4-byte
# store; a 16-byte read of that register soon after (a vector op, a q-register store) then cannot take its data from
# those stores (a store-forwarding stall: ~a quarter of the hottest castle function's samples sat on two such reads).
VSET_PAIR = re.compile(r"c->vreg\[(\d+)\]\[0\] ?= ?([^;]+); ?c->vreg\[\1\]\[1\] ?= ?([^;]+);")
VSET_LOW = re.compile(r"c->vreg\[(\d+)\]\[0\]=0; ?c->vreg\[\1\]\[1\]=0; ?memcpy\(c->vreg\[\1\],(&?\w+),([48])\);")
VSET_FULL = re.compile(r"memcpy\(c->vreg\[(\d+)\],(\w+),16\);")


def widen_vstores(text):
    if "c->vreg[" not in text or os.environ.get("A64_NO_VSET"):
        return text
    text = VSET_LOW.sub(lambda m: "a64_vset_lo(c->vreg[%s],%s,%s);" % m.groups(), text)

    def pair(m):   # (both halves from the old value: the top half's expression must not read the new bottom half;
        d, lo, hi = m.groups()   # with two expressions no calls in them: their order as arguments is unspecified)
        if hi.strip() == "0":
            return "a64_vset64(c->vreg[%s],%s);" % (d, lo)
        if "c->vreg[%s][0]" % d in hi or re.search(r"\w\s*\(", lo + hi):
            return m.group(0)
        return "a64_vset128(c->vreg[%s],%s,%s);" % (d, lo, hi)
    text = VSET_PAIR.sub(pair, text)
    return VSET_FULL.sub(lambda m: "a64_vst16(c->vreg[%s],%s);" % m.groups(), text)


# Stores through SP (register 31 as a base is always SP) go to the guest stack, which the GPU never reads: the
# _sp variants (rt/a64.h) leave out the page marks of the soft write watch (two byte stores each, a quarter of TOTK's
# store sites, nearly all prologue STPs). A64_MARK_STACK=1 at lift time: marked like any store.
SP_STORE_PRE = re.compile(r"(\{ uint64_t _b=c->x\[31\]; int64_t _o=-?\d+; )recomp_(store8|store16|store32|store64|stp32|stp64)\(")
SP_STORE = re.compile(r"recomp_(store8|store16|store32|store64|stp32|stp64)\(c,(\(?)c->x\[31\]")   # ((sp + imm), sp + imm)


# Stores through x29 go to the stack too where x29 is the frame pointer: in a function whose every write of x29 is the
# frame setup (x29 = sp + imm) or its restore from the stack (LDP from SP), emit_function gives its x29-based stores
# the _sp variants as well (88k sites in TOTK; a few functions use x29 as an ordinary register and keep their marks).
# Castle, 3 rounds: 66.9 -> 67.4 fps (within noise), exe -2 MB. A64_MARK_FRAME=1 at lift time: marked like any store.
FP_WRITE = re.compile(r"c->x\[29\]\s*=(?!=)|&c->x\[29\]")
FP_SETUP = re.compile(r"\{ uint64_t _a=c->x\[31\], _b=\d+ULL; uint64_t _r=_a\+_b; c->x\[29\]=_r; \}")
FP_RESTORE = re.compile(r"\{ uint64_t _b=c->x\[31\]; int64_t _o=-?\d+; \{ uint64_t _p0,_p1; recomp_ldp64\(c,[^;]*;"
                        r" c->x\[29\]=_p0; c->x\[30\]=_p1; \}")
FP_STORE_PRE = re.compile(r"(\{ uint64_t _b=c->x\[29\]; int64_t _o=-?\d+; )recomp_(store8|store16|store32|store64|stp32|stp64)\(")
FP_STORE = re.compile(r"recomp_(store8|store16|store32|store64|stp32|stp64)\(c,(\(?)c->x\[29\]")


def fp_is_frame(text):
    """an instruction's C text writes x29 only as frame setup or restore (or not at all)"""
    if not FP_WRITE.search(text):
        return True
    rest = FP_RESTORE.sub("", FP_SETUP.sub("", text))
    return not FP_WRITE.search(rest)


def frame_stores(text):
    text = FP_STORE_PRE.sub(r"\1recomp_\2_sp(", text)
    return FP_STORE.sub(r"recomp_\1_sp(c,\2c->x[29]", text)


def stack_stores(text):
    if "c->x[31]" not in text or os.environ.get("A64_MARK_STACK"):
        return text
    text = SP_STORE_PRE.sub(r"\1recomp_\2_sp(", text)
    return SP_STORE.sub(r"recomp_\1_sp(c,\2c->x[31]", text)


# Page marks per group (A2). In a straight-line run of a function's C the marked stores through the same base register
# plus a constant keep their data stores but share one A64_MARK(base + lo, hi - lo) for their byte span [lo, hi) (when it
# is at most 4 KB), placed after the group's last store and before the first later line that writes the base register or
# ends the run. The pages marked are the same: every store of the group lies in [base+lo, base+hi), at most two pages,
# which the stores at lo and ending at hi mark themselves. Each data store is still followed, in program order and before
# any guest synchronisation, by a release store of a mark of its page, so a collection that clears the mark sees the data
# (rt/a64.h A64_MARK).
# It runs on the finished C files, last (nx2/lift.py: after nx2/oot_patches.py and every other edit of the lifted text,
# which match the lift's exact text), so it is not part of emit_function, and the kit's own write() leaves it out: a
# lift without nx2 keeps a mark per store unless it calls group_marks itself. A64_MARK_EACH=1 at lift time: a mark per
# store. Running it again changes nothing (its own marks are recognised, and are not run ends).
# A run ends at anything not shown to be straight-line code without synchronisation: a label, goto, return, a line
# whose braces do not balance (a multi-line block opens or closes: a mark never moves into or out of one), STLR / LDAR
# in suyu's spelling (a plain recomp_storeN(c,c->x[N],...) / recomp_loadN(c,c->x[N])), the register file as a whole,
# a local register (A64_LOCAL_REGS), and every call-like token but MG_PLAIN's: calls (f_*, TAILCALL, hos_* from the
# patches), polls, atomics, barriers, exclusives, SVCs, traps and any helper this list does not know.
MG_STORE = re.compile(r"recomp_(store8|store16|store32|store64|stp32|stp64)\(c,")
MG_SIZE = {"store8": 1, "store16": 2, "store32": 4, "store64": 8, "stp32": 8, "stp64": 16}
# address forms (the text right after "recomp_X(c,")
MG_ADDR = [
    (re.compile(r"c->x\[(\d+)\] \+ (-?\d+),"), "x"),          # c->x[N] + IMM,
    (re.compile(r"\(c->x\[(\d+)\]\+(\d+)ULL\),"), "x"),        # (c->x[N]+IMMULL),
    (re.compile(r"\(_b\+_o\)\+(\d+),"), "bo+"),                # (_b+_o)+IMM,
    (re.compile(r"\(_b\+_o\),"), "bo"),                        # (_b+_o),
    (re.compile(r"_b,"), "b"),                                 # _b,
    (re.compile(r"_a\+(\d+),"), "a+"),                         # _a+IMM,
    (re.compile(r"_a,"), "a"),                                 # _a,
]
MG_BDEF = re.compile(r"uint64_t _b=c->x\[(\d+)\]; int64_t _o=(-?\d+);")
MG_ADEF = re.compile(r"uint64_t _a=c->x\[(\d+)\];")
# (call-like tokens that are plain: keywords, guest loads and stores without ordering (the stores are what is grouped),
# and helpers that only compute or move values between locals, registers and vector registers)
MG_PLAIN = re.compile(r"(?:if|for|while|sizeof|memcpy|memset|UINT64_C|INT64_C|fabsf|fabs|__builtin_add_overflow|"
                      r"recomp_(?:load|ldp|store|stp)(?:8|16|32|64)(?:_sp)?|"
                      r"recomp_(?:fixed_to_fp|fp_to_fixed|fp_to_int|fp_estimate|umulh|smulh|aes_sbox)|"
                      r"a64_(?:vset64|vset128|vset_lo|vst16|fcmp32|fcmp64|vfcmp)|a64_v?fpx_\w+)\Z")
MG_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_mg_plain = {}
# (the other run ends: a label, goto, return, STLR and LDAR, the register file as a whole)
MG_END = re.compile(r"^\s*L_[0-9a-f]+:|\bgoto\b|\breturn\b|recomp_(?:store|stp)\d+\(c,c->x\[\d+\],|"
                    r"recomp_load\d+\(c,c->x\[\d+\]\)|c->x(?!\[)")
MG_LOCAL = re.compile(r"\bx\d+_ = c->x\[")   # (localize_regs's locals: such a function is left as it is)
MG_BRACE = re.compile(r"[{}]")
# a line whose stores may not all run, or run more than once: its stores keep their own marks
MG_NOT_STRAIGHT = re.compile(r"\bif\b|\?|\bfor\b|\bwhile\b|\bswitch\b|\bcase\b")
MG_NOT_STRAIGHT_WORDS = ("if", "?", "for", "while", "switch", "case")
MG_MARK = "    A64_MARK((c->x[%d] + (uint64_t)(int64_t)(%d)), %d);"
MG_OWN_MARK = re.compile(r"    A64_MARK\(\(c->x\[\d+\] \+ \(uint64_t\)\(int64_t\)\(-?\d+\)\), \d+\);\Z")
MG_HEAD = re.compile(r"\nvoid f_[0-9a-f]+\(")   # (a definition or declaration: its line begins after the newline)


def mg_balanced(line):
    """the line's braces close what it opens, in order (a block on one line, or none)"""
    if "{" not in line and "}" not in line:
        return True
    d = 0
    for b in MG_BRACE.findall(line):
        d += 1 if b == "{" else -1
        if d < 0:
            return False
    return d == 0


def mg_ends_run(line):
    """the line ends a straight-line run (each test only on a line that has its text: this runs on most lines of the lift)"""
    if ("{" in line or "}" in line) and not mg_balanced(line):
        return True
    if "(" in line:
        for t in MG_CALL.findall(line):
            p = _mg_plain.get(t)
            if p is None:
                p = _mg_plain[t] = MG_PLAIN.match(t) is not None
            if not p:
                return True
    if "goto" in line or "return" in line or "L_" in line or "(c,c->x[" in line or line.count("c->x") != line.count("c->x["):
        return MG_END.search(line) is not None
    return False


def mg_parse_store(line, m):
    """(base register, offset, size) of the marked store at match m, or None"""
    rest = line[m.end():]
    size = MG_SIZE[m.group(1)]
    for rx, kind in MG_ADDR:
        a = rx.match(rest)
        if not a:
            continue
        if kind == "x":
            return int(a.group(1)), int(a.group(2)), size
        d = None   # (the nearest definition before the store)
        for d in (MG_BDEF if kind in ("bo", "bo+", "b") else MG_ADEF).finditer(line, 0, m.start()):
            pass
        if d is None:
            return None
        if kind in ("bo", "bo+", "b"):
            off = 0 if kind == "b" else int(d.group(2)) + (int(a.group(1)) if kind == "bo+" else 0)
            return int(d.group(1)), off, size
        return int(d.group(1)), (int(a.group(1)) if kind == "a+" else 0), size
    return None


def mark_groups_lines(lines, st):
    """the lines of a function (or of any C text) with the page marks of each group shared"""
    inserts = {}    # line index -> mark lines to put before it
    unmark = {}     # line index -> set of match start offsets to turn into _sp
    groups = {}     # base register -> [(line index, match start, off, size)]

    def flush(reg, at):
        g = groups.pop(reg, None)
        if not g:
            return
        lo = min(o for _, _, o, _ in g)
        hi = max(o + n for _, _, o, n in g)
        st["stores"] += len(g)
        if len(g) < 2 or hi - lo > 4096:
            return
        for li, ms, _, _ in g:
            unmark.setdefault(li, set()).add(ms)
        inserts.setdefault(at, []).append(MG_MARK % (reg, lo, hi - lo))
        st["groups"] += 1
        st["grouped_stores"] += len(g)

    for i, line in enumerate(lines):
        if not groups and "recomp_st" not in line:
            continue   # (nothing to end or to add to)
        if line.startswith("    A64_MARK((") and MG_OWN_MARK.match(line):
            continue
        if mg_ends_run(line):
            for r in list(groups):
                flush(r, i)
            continue
        writes = {int(a or b) for a, b in REG_WRITE.findall(line)} if (
            "=" in line or "&c->x" in line or "++" in line or "--" in line) else ()
        stores = list(MG_STORE.finditer(line)) if "recomp_st" in line else ()
        if stores and not writes and not (any(w in line for w in MG_NOT_STRAIGHT_WORDS) and MG_NOT_STRAIGHT.search(line)):
            for m in stores:
                p = mg_parse_store(line, m)
                if p is None or p[0] == 31:
                    continue   # (an address this pass does not follow, or SP: the store keeps what it has)
                groups.setdefault(p[0], []).append((i, m.start(), p[1], p[2]))
            continue
        for r in writes:   # (the base changes here: its group's mark goes before this line)
            if r in groups:
                flush(r, i)
    for r in list(groups):
        flush(r, len(lines))
    if not unmark:
        return lines
    out = []
    for i, line in enumerate(lines):
        out.extend(inserts.get(i, []))
        if i in unmark:
            for ms in sorted(unmark[i], reverse=True):
                m = MG_STORE.match(line, ms)
                line = line[:m.start()] + "recomp_%s_sp(c," % m.group(1) + line[m.end():]
        out.append(line)
    out.extend(inserts.get(len(lines), []))
    return out


def mark_groups(text, stats=None):
    """text (a C file of lifted functions, or one function) with the page marks of each group shared. stats: counts of
    the stores looked at, the groups and their stores"""
    st = stats if stats is not None else Counter()
    for k in ("stores", "groups", "grouped_stores"):
        st[k] = st.get(k, 0)
    starts = [0] + [m.start() + 1 for m in MG_HEAD.finditer(text)]
    starts.append(len(text))
    out = []
    for a, b in zip(starts, starts[1:]):
        part = text[a:b]
        if part.count("recomp_st") < 2 or len(MG_STORE.findall(part)) < 2 or (
                "_ = c->x[" in part and MG_LOCAL.search(part)):   # (no group possible: most functions)
            out.append(part)
            continue
        out.append("\n".join(mark_groups_lines(part.split("\n"), st)))
    return "".join(out)


def group_marks(paths, stats=None):
    """mark_groups over C files, in place (nx2/lift.py: the lift's last step). A64_MARK_EACH=1: none. The files changed"""
    if os.environ.get("A64_MARK_EACH"):
        return 0
    changed = 0
    for path in paths:
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
        new = mark_groups(text, stats)
        if new != text:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(new)
            changed += 1
    return changed


# Dead flags. Each flag-setting instruction stores all of N, Z, C and V in the context, where the compiler must keep
# them: a callee or the caller could read them. The guest's ABI says otherwise (calls clobber NZCV, and nothing reads
# flags across a return), so emit_function computes per function which flags are live after each instruction and
# drops the stores of the dead ones (the computations go with them). A call (BL, BLR) clobbers all four; RET reads
# none; anything else that leaves the function (tail calls, which also join the pieces of a split function, indirect
# jumps, traps, SVC) reads all four. A64_KEEP_FLAGS=1 at lift time: every store kept.
FLAG_BIT = {"n": 8, "z": 4, "c": 2, "v": 1}
FLAG_WRITE = re.compile(r"c->([nzcv])\s*=(?!=)[^;]*;")
FLAG_READ = re.compile(r"c->([nzcv])\b(?!\s*=(?!=))")
COND_READS = [4, 4, 2, 2, 8, 8, 1, 1, 6, 6, 9, 9, 13, 13, 0, 0]   # (EQ NE CS CC MI PL VS VC HI LS GE LT GT LE AL NV)
_flag_masks = {}


def flag_masks(text):
    """(flags read, flags written) by an instruction's C text; control flow inside it: reads all, kills none"""
    m = _flag_masks.get(text)
    if m is None:
        if "goto" in text or "return" in text or "TAILCALL" in text:
            m = (15, 0)
        else:
            w = 0
            for f in FLAG_WRITE.findall(text):
                w |= FLAG_BIT[f]
            r = 0
            for f in FLAG_READ.findall(text):
                r |= FLAG_BIT[f]
            m = (r, w)
        _flag_masks[text] = m
    return m


def drop_flag_stores(text, dead):
    return FLAG_WRITE.sub(lambda m: "" if FLAG_BIT[m.group(1)] & dead else m.group(0), text)


# A64_LOCAL_REGS=<k> (experiment, lift time): a function's k most used guest registers (and SP) as C locals, written
# back to the context only where other code can see them. All of them (k >= 31) measured no gain on 10-21: the
# compiler spills them and the syncs stay; totk-pc-recomp (recomp/regs.py, MIT) found 4-6 the best over 300 hot
# functions (1.4.3 fight 13.5 -> 11.3 ms). The compiler had to store every changed register before each call and reload every
# register it reads after one. The guest's ABI needs less: x9-x15 are dead at calls and returns, and x19-x29 and SP come
# back from a call unchanged. emit_function marks each place code outside the function sees the registers, renames
# c->x[N] to xN_ and expands the marks: before a call or a return, every register but x9-x15 is written back; after a
# call x0-x18 and x30 are read again; around tail calls, indirect jumps, SVCs and traps everything (after an SVC, read
# again too). Functions with code spliced in (build.py SPLICE_AFTER) keep their registers in the context.
SYNC_CALL, SYNC_BACK, SYNC_RET, SYNC_ALL, SYNC_RELOAD = "\x01C%d\x01", "\x01B%d\x01", "\x01R%d\x01", "\x01A%d\x01", "\x01L%d\x01"
SYNC_TAIL = "\x01T%d\x01"   # (control leaving after the instruction: what it wrote too)
SYNC_MARK = re.compile("\x01([CBRALT])(\\d+)\x01")
REG_REF = re.compile(r"c->x\[(\d+)\]")
# (a write of a guest register: its address taken (LDP's outputs), ++ / --, = and the compound assignments)
REG_WRITE = re.compile(r"(?:(?:&|\+\+\s*|--\s*)c->x\[(\d+)\])|(?:c->x\[(\d+)\]\s*(?:(?:[-+*/%&|^]|<<|>>)?=(?!=)|\+\+|--))")
SCRATCH = 0xFE00   # x9-x15: dead at calls and returns
_reg_writes = {}


def reg_writes(text):
    """the guest registers an instruction's C text may write (a mask)"""
    m = _reg_writes.get(text)
    if m is None:
        m = 0
        for a, b in REG_WRITE.findall(text):
            m |= 1 << int(a or b)
        _reg_writes[text] = m
    return m


# A64_LOCAL_FLAGS=1 (experiment, lift time): N, Z, C and V as locals of each function (totk-pc-recomp keeps NZCV in a
# local: recomp/flags.py, MIT; 1.4.3 .text 342 -> 299 MiB). The ABI leaves the flags undefined across calls and
# returns (the dead-flag pass relies on it too), so they are written back only where control leaves otherwise: tail
# calls, indirect jumps, SVCs and traps (the SYNC_ALL / SYNC_TAIL marks), and read again after an SVC.
FLAG_REF = re.compile(r"(?<![\w>.])c->([nzcv])\b")
FLAG_STORE = "c->n = fn_; c->z = fz_; c->c = fc_; c->v = fv_; "
FLAG_LOAD = "fn_ = c->n; fz_ = c->z; fc_ = c->c; fv_ = c->v; "


def local_flags():
    return bool(os.environ.get("A64_LOCAL_FLAGS"))


def local_regs():
    """A64_LOCAL_REGS: how many of a function's guest registers become locals (0: none)"""
    v = os.environ.get("A64_LOCAL_REGS", "")
    return int(v) if v.isdigit() else 0


def localize_regs(text, local, dirty):
    """Expand the context marks (see SYNC_CALL): with `local`, the registers become locals first, and each mark writes
    back what may have changed since the last one (dirty: per instruction index, the masks before and after it) or
    reads again."""
    flags = local_flags() and FLAG_REF.search(text) is not None
    if flags:
        text = FLAG_REF.sub(r"f\1_", text)
    fstore, fload = (FLAG_STORE, FLAG_LOAD) if flags else ("", "")
    fdecl = "\n    uint8_t fn_ = c->n, fz_ = c->z, fc_ = c->c, fv_ = c->v;" if flags else ""
    if not local:
        text = SYNC_MARK.sub(lambda m: fstore if m.group(1) in "AT" else fload if m.group(1) == "L" else "", text)
        head, _, rest = text.partition("\n")
        return head + fdecl + "\n" + rest
    uses = {}
    for n in REG_REF.findall(text):
        uses[int(n)] = uses.get(int(n), 0) + 1
    top = sorted((n for n in uses if n != 31), key=lambda n: (-uses[n], n))[:local_regs()]
    regs = sorted(top + ([31] if 31 in uses else []))
    chosen = set(regs)
    text = REG_REF.sub(lambda m: "x%s_" % m.group(1) if int(m.group(1)) in chosen else m.group(0), text)

    def store(mask):
        return "".join("c->x[%d] = x%d_; " % (n, n) for n in regs if mask >> n & 1)

    def load(mask):
        return "".join("x%d_ = c->x[%d]; " % (n, n) for n in regs if mask >> n & 1)

    def expand(m):
        kind, i = m.group(1), int(m.group(2))
        before, after = dirty[0][i], dirty[1][i]
        if kind == "C":   # a call: what it may read (x30 is set just before it)
            return store((before | 1 << 30) & ~SCRATCH)
        if kind == "R":
            return store(before & ~SCRATCH)
        if kind == "A":
            return store(before) + fstore
        if kind == "T":
            return store(after) + fstore
        if kind == "B":   # after a call: x0-x18 and x30 (x19-x29 and SP come back as they were)
            return load(0x4007FFFF)
        return load(0xFFFFFFFF) + fload
    text = SYNC_MARK.sub(expand, text)
    head, _, rest = text.partition("\n")
    decl = ("\n    uint64_t " + ", ".join("x%d_ = c->x[%d]" % (n, n) for n in regs) + ";") if regs else ""
    return head + decl + fdecl + "\n" + rest


# A64_PACKED_FLAGS=1 (experiment, lift time): the four NZCV stores of a flag-setting instruction become one 32-bit
# store (Ctx keeps n, z, c, v as adjacent bytes); byte stores to the context were ~5% of lifted-code samples.
# Measured 2026-10-21 (castle, 2 interleaved rounds): slower, game threads 39.6 -> 41.0 ms CPU a frame, 72.5 -> 71.1
# fps (exe -12 MB): the compiler no longer forwards a single flag from the packed store. Off.
FLAGS4 = re.compile(r"c->([zn])=([^;]+); ?c->([zn])=([^;]+); ?c->c=([^;]+); ?c->v=([^;]+);")


def pack_flags(text):
    if "c->v=" not in text or not os.environ.get("A64_PACKED_FLAGS"):
        return text

    def sub(m):
        a, ea, b, eb, ec, ev = m.groups()
        if a == b or any("c->" in e for e in (ea, eb, ec, ev)):
            return m.group(0)
        en, ez = (ea, eb) if a == "n" else (eb, ea)
        return "a64_set_nzcv(c,%s,%s,%s,%s);" % (en, ez, ec, ev)
    return FLAGS4.sub(sub, text)


VSET_HEADER = r"""
/* the four flags in one store (pack_flags in a64_lift.py): n, z, c, v are adjacent bytes of the context */
static inline __attribute__((always_inline)) void a64_set_nzcv(Ctx *c, uint32_t n, uint32_t z, uint32_t cf, uint32_t v) {
    const uint32_t w = n | z << 8 | cf << 16 | v << 24;
    memcpy(&c->n, &w, 4);
}
/* ---- whole-register vector writes (widen_vstores in a64_lift.py): one 16-byte store, kept whole (A64_OPAQUE hides
   the value from the compiler's store narrowing) ---- */
#if defined(__x86_64__) || defined(_M_X64)
#define A64_OPAQUE(x) __asm__("" : "+x"(x))
#else
#define A64_OPAQUE(x) ((void)0)
#endif
#if RECOMP_FPX_HOST == 2
static inline __attribute__((always_inline)) void a64_vst16(uint64_t *d, const void *s) {
    __m128i x = _mm_loadu_si128((const __m128i *)s);
    A64_OPAQUE(x);
    _mm_storeu_si128((__m128i *)d, x);
}
static inline __attribute__((always_inline)) void a64_vset64(uint64_t *d, uint64_t v) {
    __m128i x = _mm_set_epi64x(0, (long long)v);
    A64_OPAQUE(x);
    _mm_storeu_si128((__m128i *)d, x);
}
static inline __attribute__((always_inline)) void a64_vset128(uint64_t *d, uint64_t lo, uint64_t hi) {
    __m128i x = _mm_set_epi64x((long long)hi, (long long)lo);
    A64_OPAQUE(x);
    _mm_storeu_si128((__m128i *)d, x);
}
static inline __attribute__((always_inline)) void a64_vst_lo64(uint64_t *d, __m128i r) {   /* the low half, top zeroed */
    r = _mm_move_epi64(r);
    A64_OPAQUE(r);
    _mm_storeu_si128((__m128i *)d, r);
}
#else
static inline void a64_vst16(uint64_t *d, const void *s) { memcpy(d, s, 16); }
static inline void a64_vset64(uint64_t *d, uint64_t v) { d[0] = v; d[1] = 0; }
static inline void a64_vset128(uint64_t *d, uint64_t lo, uint64_t hi) { d[0] = lo; d[1] = hi; }
#endif
static inline __attribute__((always_inline)) void a64_vset_lo(uint64_t *d, const void *s, int n) {
    uint64_t v = 0;
    memcpy(&v, s, n);
    a64_vset64(d, v);
}
"""

VFCMP_HEADER = r"""
/* ---- vector float compares on SSE (vectorize_fcmp): 1 when done (no NaN lane; under FZ no subnormal lane) ---- */
static const uint64_t a64_vzero[2] = {0, 0};   /* (the operand of a compare against zero) */
/* scalar FCMP / FCMPE: the flags from one native compare; 0 for the exact block. (Byte stores, as everywhere: one
   packed store of the four was slower, the compiler no longer forwarded single flags from it.) */
static inline __attribute__((always_inline)) int a64_fcmp_set(Ctx *c, uint32_t lt, uint32_t eq) {
    c->n = (uint8_t)lt;   /* lt: N, eq: Z, else C; V clear */
    c->z = (uint8_t)eq;
    c->c = (uint8_t)!lt;
    c->v = 0;
    return 1;
}
static inline __attribute__((always_inline)) int a64_fcmp32(Ctx *c, const void *pa, const void *pb) {
    uint32_t a, b;
    float fa, fb;
    memcpy(&a, pa, 4);
    memcpy(&b, pb, 4);
    memcpy(&fa, &a, 4);
    memcpy(&fb, &b, 4);
    if (__builtin_isunordered(fa, fb)) return 0;
    if ((c->fpcr & (1ULL << 24)) && ((a & 0x7fffffffu) - 1u < 0x7fffffu || (b & 0x7fffffffu) - 1u < 0x7fffffu)) return 0;
    return a64_fcmp_set(c, fa < fb, fa == fb);
}
static inline __attribute__((always_inline)) int a64_fcmp64(Ctx *c, const void *pa, const void *pb) {
    uint64_t a, b;
    double fa, fb;
    memcpy(&a, pa, 8);
    memcpy(&b, pb, 8);
    memcpy(&fa, &a, 8);
    memcpy(&fb, &b, 8);
    if (__builtin_isunordered(fa, fb)) return 0;
    if ((c->fpcr & (1ULL << 24)) && ((a & 0x7fffffffffffffffULL) - 1u < 0xfffffffffffffULL ||
                                     (b & 0x7fffffffffffffffULL) - 1u < 0xfffffffffffffULL)) return 0;
    return a64_fcmp_set(c, fa < fb, fa == fb);
}
static inline __attribute__((always_inline)) int a64_vfcmp(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k, int op) {
#if RECOMP_FPX_HOST == 2
    const __m128i a = _mm_loadu_si128((const __m128i *)n), b = _mm_loadu_si128((const __m128i *)m);
    const __m128i abs_mask = _mm_set1_epi32(0x7fffffff), inf = _mm_set1_epi32(0x7f800000);
    const __m128i abs_a = _mm_and_si128(a, abs_mask), abs_b = _mm_and_si128(b, abs_mask);
    __m128i bad = _mm_or_si128(_mm_cmpgt_epi32(abs_a, inf), _mm_cmpgt_epi32(abs_b, inf));   /* NaN */
    if (c->fpcr & (1ULL << 24)) {   /* FZ: a subnormal is flushed (and flagged) by the exact block */
        const __m128i z = _mm_setzero_si128();
        const __m128i sub_a = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(a, inf), inf), _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(a, inf), z), _mm_cmpgt_epi32(abs_a, z)));
        const __m128i sub_b = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(b, inf), inf), _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(b, inf), z), _mm_cmpgt_epi32(abs_b, z)));
        bad = _mm_or_si128(bad, _mm_or_si128(sub_a, sub_b));
    }
    if (_mm_movemask_epi8(bad) & (k == 4 ? 0xFFFF : 0x00FF)) return 0;
    const __m128 fa = _mm_castsi128_ps(a), fb = _mm_castsi128_ps(b);
    const __m128i r = _mm_castps_si128(op == 0 ? _mm_cmpge_ps(fa, fb) : op == 1 ? _mm_cmpgt_ps(fa, fb) : _mm_cmpeq_ps(fa, fb));
    if (k == 4) _mm_storeu_si128((__m128i *)d, r); else a64_vst_lo64(d, r);
    return 1;
#else
    (void)c; (void)d; (void)n; (void)m; (void)k; (void)op;
    return 0;
#endif
}
"""


def vectorize_fpx(text):
    text = vectorize_fcmp(text)
    if "a64_fpx_" not in text or os.environ.get("A64_NO_VFPX"):
        return text

    def add(m):
        lanes = VFPX_LANE.findall(m.group(3))
        ops = {op for _, _, op, _ in lanes}
        if len(lanes) not in (2, 4) or len(ops) != 1 or \
                any(not (int(i) == int(j) == int(r) == x) for x, (i, j, _, r) in enumerate(lanes)):
            return m.group(0)
        return "a64_vfpx_%s(c,c->vreg[%s],c->vreg[%s],c->vreg[%s],%d);" % (ops.pop()[:5], m.group(4), m.group(1),
                                                                         m.group(2), len(lanes))

    text = VFPX_ADD.sub(add, text)
    return VFPX_MUL.sub(lambda m: "a64_vfpx_mul32(c,c->vreg[%s],c->vreg[%s],c->vreg[%s],4);"
                        % (m.group(3), m.group(1), m.group(2)), text)


# (hookshot) Fast float regions (A64_FASTFP at lift time: on unless "0"; "hot" the A64_HOT_FUNCS list only; "check"
# both copies every time, compared: see ff_region).
# Each float op on the exact path carries its own tests (~25 instructions around one SSE op). A straight run of
# instructions with no label, store, call, branch or system access inside, holding at least A64_FASTFP_MIN (8) single-
# precision adds, subtracts, multiplies, divides or square roots, is emitted twice: a fast copy whose ops are plain SSE,
# with MXCSR's flags cleared before it and read once after, and the exact copy. A clean fast run is the Arm result: with
# no invalid operation (no NaN made), no denormal operand and no underflow (FTZ on under FPCR.FZ: a tiny result is
# flushed and flagged), IEEE single precision round-to-nearest is the same on both, except for a NaN result or a
# product, quotient or multiply-add of exactly 2^-126, which raise the invalid flag on purpose (a64_ff_guard_; the ops
# are asm with Arm's operand order all the same). Divide-by-zero and overflow give the same infinities; they and inexact
# are or-ed into FPSR. On an invalid, denormal or underflow flag the registers the run names are put back and the exact
# copy runs. Measured
# 2026-11-11 on OoT's f_820e1d60 in game (both run per call, compared): 0 mismatches in 150,000 calls, 1.79x faster.
# HOS_NO_FASTFP (run time, any value): the exact copies only, for comparisons.
FF_CALL = re.compile(r"\b((?:recomp|a64|A64|RECOMP)_\w+)\s*\(")
FF_PURE = {"recomp_load8", "recomp_load16", "recomp_load32", "recomp_load64", "recomp_ldp32", "recomp_ldp64",
           "a64_vset64", "a64_vset128", "a64_vset_lo", "a64_vst16", "a64_fcmp32", "a64_fcmp64", "a64_vfcmp",
           "recomp_fixed_to_fp", "recomp_fp_to_int", "recomp_fp_to_fixed", "recomp_umulh", "recomp_smulh",
           "recomp_fp_estimate", "recomp_aes_sbox", "RECOMP_LIKELY", "RECOMP_UNLIKELY", "RECOMP_FPX_OPEN",
           "RECOMP_FPX_FZ", "a64_fcmp32k", "a64_fcmp64k"}
FF_BAD = ("c->fpcr", "c->fpsr", "c->excl", "c->pc", "c->halted", "c->poll", "c->tpidr", "\x01", "goto ", "return",
          "switch", "A64_PTR", "A64_MARK")
# (the exact blocks' own uses, safe in a region: FPCR read in a test (it doesn't change inside one: no MSR there), and
# FPSR's cumulative bits or-ed in (put back with the registers when the region runs again exactly). An MSR, or an MRS of
# FPSR, which would see a value the fast copy has not finished, still ends the run.)
FF_FP_OK = re.compile(r"c->fpcr\s*&|c->fpsr\s*\|=")
# ((hookshot) fma32_fma32: FMADD/FMSUB/FNMADD/FNMSUB and FMLA/FMLS S lanes, an FMA3 op where the build has one; see
# a64_ff_fma32_fma32 in FASTFP_HEADER)
FF_CONV = re.compile(r"\ba64_vfpx_(add32|sub32|mul32)\(|"
                     r"\ba64_fpx_(add32_add32|sub32_sub32|mul32_fma32|fma32_fma32|div32_div32|sqrt32_sqrt32)\(")
VREG_REF = re.compile(r"c->vreg\[(\d+)\]")


def fastfp_mode():
    """the mode: empty when A64_FASTFP is 0 (off), else its value, "1" by default (every function), hot or check"""
    v = os.environ.get("A64_FASTFP", "1")
    return "" if v == "0" else v


def ff_min():
    v = os.environ.get("A64_FASTFP_MIN", "")
    return int(v) if v.isdigit() else 8


def ff_eligible(text):
    """an instruction's C text may sit inside a fast float region: no store, control flow or system state; only pure calls"""
    if any(b in FF_FP_OK.sub("", text) for b in FF_BAD):
        return False
    return all(n in FF_PURE or n.startswith(("a64_fpx_", "a64_vfpx_")) for n in FF_CALL.findall(text))


def ff_region(items):
    """a run's C text ([(pc, text)], every text ff_eligible): the fast and exact copies, or the exact text alone when the
    run has too few float ops to pay for its check. (text, number of ops made fast)"""
    exact = "\n".join(t for _, t in items)
    n = len(FF_CONV.findall(exact))
    if n < ff_min():
        return exact, 0
    fast = FF_CONV.sub(lambda m: "a64_ff_v%s(" % m.group(1) if m.group(1) else "a64_ff_%s(" % m.group(2), exact)
    xs = sorted({int(r) for r in REG_REF.findall(exact)})
    vs = sorted({int(r) for r in VREG_REF.findall(exact)})
    fl = sorted(set(FLAG_REF.findall(exact)))
    save = ("".join("const uint64_t _ffx%d = c->x[%d]; " % (r, r) for r in xs) +
            "".join("const uint64_t _ffv%d0 = c->vreg[%d][0], _ffv%d1 = c->vreg[%d][1]; " % (r, r, r, r) for r in vs) +
            "".join("const uint8_t _fff%s = c->%s; " % (f, f) for f in fl) + "const uint64_t _ffsr = c->fpsr; ")
    restore = ("".join("c->x[%d] = _ffx%d; " % (r, r) for r in xs) +
               "".join("c->vreg[%d][0] = _ffv%d0; c->vreg[%d][1] = _ffv%d1; " % (r, r, r, r) for r in vs) +
               "".join("c->%s = _fff%s; " % (f, f) for f in fl) + "c->fpsr = _ffsr; ")
    label = "L_ff_%08x" % items[0][0]
    if fastfp_mode() == "check":
        # (A64_FASTFP=check: both copies every time, the exact one's result kept, the registers the run names compared;
        # a64_ff_check counts and prints the first mismatches with the run's guest address)
        keep = ("".join("const uint64_t _ffox%d = c->x[%d]; " % (r, r) for r in xs) +
                "".join("const uint64_t _ffov%d0 = c->vreg[%d][0], _ffov%d1 = c->vreg[%d][1]; " % (r, r, r, r) for r in vs) +
                "".join("const uint8_t _ffof%s = c->%s; " % (f, f) for f in fl))
        same = " && ".join(["1"] + ["_ffox%d == c->x[%d]" % (r, r) for r in xs] +
                           ["_ffov%d0 == c->vreg[%d][0] && _ffov%d1 == c->vreg[%d][1]" % (r, r, r, r) for r in vs] +
                           ["_ffof%s == c->%s" % (f, f) for f in fl])
        return ("{ if (A64_FF_ON(c)) { %sconst unsigned _ffm = a64_ff_begin(c);\n%s\n"
                "const int _ffok = a64_ff_end(c, _ffm);\n%s%s\n%s\na64_ff_check(0x%xu, _ffok, %s); } else {\n%s\n} }"
                % (save, fast, keep, restore, exact, items[0][0], same, exact)), n
    return ("{ if (A64_FF_ON(c)) { %sconst unsigned _ffm = a64_ff_begin(c);\n%s\n"
            "if (RECOMP_LIKELY(a64_ff_end(c, _ffm))) goto %s;\n%s}\n%s\n%s:; }"
            % (save, fast, label, restore, exact, label)), n


FASTFP_HEADER = r"""
/* ---- (hookshot) fast float regions (A64_FASTFP, ff_region in a64_lift.py) ---- */
#if RECOMP_FPX_HOST == 2 && defined(__x86_64__) && !defined(__EMSCRIPTEN__)
#include <stdlib.h>
#include <stdio.h>
static int a64_ff_enabled_(void) { static int v = -1; if (v < 0) v = !getenv("HOS_NO_FASTFP"); return v; }
#define A64_FF_ON(c) (RECOMP_FPX_OPEN(c) && a64_ff_enabled_())
/* MXCSR: exceptions masked, round to nearest, DAZ off, FTZ as FPCR.FZ, flags cleared; the old value returned */
static inline __attribute__((always_inline)) unsigned a64_ff_begin(const Ctx *c) {
    unsigned m, n;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    n = (m & ~0xE07Fu) | 0x1F80u | (RECOMP_FPX_FZ(c) ? 0x8000u : 0u);
    __asm__ volatile("ldmxcsr %0" : : "m"(n));
    return m;
}
/* 0: an invalid, denormal or underflow flag (the exact copy runs); else inexact, overflow, divide-by-zero to FPSR */
static inline __attribute__((always_inline)) int a64_ff_end(Ctx *c, unsigned m) {
    unsigned f;
    __asm__ volatile("stmxcsr %0" : "=m"(f));
    __asm__ volatile("ldmxcsr %0" : : "m"(m));
    if (f & 0x13u) return 0;
    c->fpsr |= (f & 0x20u ? 0x10u : 0u) | (f & 0x08u ? 0x04u : 0u) | (f & 0x04u ? 0x02u : 0u);
    return 1;
}
/* (asm volatile: kept inside the MXCSR window and in Arm's operand order, see ff_region) */
#ifdef __AVX__
#define A64_FF_OP(ins, d, a, b) __asm__ volatile("v" ins " %2, %1, %0" : "=x"(d) : "x"(a), "x"(b))
#define A64_FF_SQRT(d, a) __asm__ volatile("vsqrtss %1, %1, %0" : "=x"(d) : "x"(a))
#define A64_FF_SPOIL "vcomiss %0, %0"
#else
#define A64_FF_OP(ins, d, a, b) do { (d) = (a); __asm__ volatile(ins " %1, %0" : "+x"(d) : "x"(b)); } while (0)
#define A64_FF_SQRT(d, a) __asm__ volatile("sqrtss %1, %0" : "=x"(d) : "x"(a))
#define A64_FF_SPOIL "comiss %0, %0"
#endif
/* The invalid flag raised on purpose (a signalling compare of a NaN), sending the run to its exact copy, for two results
   whose value x86 gets right but not Arm's flags, or under FZ not Arm's value (found by tests/test_fastfp_fma.py):
   - a NaN: x86 raises no denormal flag for a subnormal next to a NaN operand, where Arm under FZ flushes it and sets
     IDC; and a multiply-add's 0 * inf + a quiet NaN gives that NaN with no flag (Arm: the default NaN, IOC);
   - exactly 2^-126, the smallest normal, from a multiply, divide or multiply-add: x86 detects tininess after rounding,
     Arm before, so a result rounded up to 2^-126 sets UFC on Arm, and is a zero under FZ. (A sum this small is exact.)
   The tests are integer compares on the bits: a float compare would raise the denormal flag on a subnormal result. */
static inline __attribute__((always_inline)) void a64_ff_spoil_(void) {
    __asm__ volatile(A64_FF_SPOIL : : "x"(_mm_castsi128_ps(_mm_cvtsi32_si128(0x7fc00000))) : "cc");
}
static inline __attribute__((always_inline)) void a64_ff_guard_(uint64_t v, int tiny) {
    const uint32_t m = (uint32_t)v & 0x7fffffffu;
    if (__builtin_expect(m > 0x7f800000u || (tiny && m == 0x00800000u), 0)) a64_ff_spoil_();
}
static inline __attribute__((always_inline)) void a64_ff_vguard_(__m128 r, int k, int tiny) {   /* (the k lanes kept) */
    const __m128i m = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7fffffff));
    __m128i bad = _mm_cmpgt_epi32(m, _mm_set1_epi32(0x7f800000));
    if (tiny) bad = _mm_or_si128(bad, _mm_cmpeq_epi32(m, _mm_set1_epi32(0x00800000)));
    if (__builtin_expect(_mm_movemask_ps(_mm_castsi128_ps(bad)) & (k == 4 ? 15 : 3), 0)) a64_ff_spoil_();
}
/* (2 lanes: the upper two are loaded as zeros, so they raise no flag of their own) */
static inline __attribute__((always_inline)) __m128 a64_ff_vld(const uint64_t *s, int k) {
    return k == 4 ? _mm_loadu_ps((const float *)s) : _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)s));
}
static inline __attribute__((always_inline)) void a64_ff_vst(uint64_t *d, __m128 r, int k) {
    if (k == 4) _mm_storeu_ps((float *)d, r); else _mm_storeu_si128((__m128i *)d, _mm_move_epi64(_mm_castps_si128(r)));
}
#define A64_FF_V(name, ins, tiny) \
static inline __attribute__((always_inline)) void a64_ff_v##name(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { \
    __m128 r; (void)c; A64_FF_OP(ins, r, a64_ff_vld(n, k), a64_ff_vld(m, k)); \
    a64_ff_vguard_(r, k, tiny); a64_ff_vst(d, r, k); }
A64_FF_V(add32, "addps", 0)
A64_FF_V(sub32, "subps", 0)
A64_FF_V(mul32, "mulps", 1)
static inline __attribute__((always_inline)) __m128 a64_ff_ss(uint64_t a) { return _mm_castsi128_ps(_mm_cvtsi32_si128((int)(uint32_t)a)); }
static inline __attribute__((always_inline)) uint64_t a64_ff_bits(__m128 r) { return (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(r)); }
#define A64_FF_S(name, ins, tiny) \
static inline __attribute__((always_inline)) uint64_t a64_ff_##name(Ctx *c, uint64_t a, uint64_t b) { \
    __m128 r; (void)c; A64_FF_OP(ins, r, a64_ff_ss(a), a64_ff_ss(b)); a64_ff_guard_(a64_ff_bits(r), tiny); return a64_ff_bits(r); }
A64_FF_S(add32_add32, "addss", 0)
A64_FF_S(sub32_sub32, "subss", 0)
/* (A subnormal over a zero also takes the exact copy: x86 raises no denormal flag beside divide-by-zero and gives an
   infinity, where Arm under FZ flushes the subnormal first, 0 / 0: the default NaN and IOC.) */
static inline __attribute__((always_inline)) uint64_t a64_ff_div32_div32(Ctx *c, uint64_t a, uint64_t b) {
    __m128 r;
    (void)c;
    if (__builtin_expect(((uint32_t)b << 1) == 0 && ((uint32_t)a << 1) - 1u < 0x00ffffffu, 0)) a64_ff_spoil_();
    A64_FF_OP("divss", r, a64_ff_ss(a), a64_ff_ss(b));
    a64_ff_guard_(a64_ff_bits(r), 1);
    return a64_ff_bits(r);
}
A64_FF_S(mul32_, "mulss", 1)
static inline __attribute__((always_inline)) uint64_t a64_ff_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) { (void)z; return a64_ff_mul32_(c, a, b); }
static inline __attribute__((always_inline)) uint64_t a64_ff_sqrt32_sqrt32(Ctx *c, uint64_t a) { __m128 r; (void)c; A64_FF_SQRT(r, a64_ff_ss(a)); return a64_ff_bits(r); }
/* (hookshot) FMADD/FMSUB/FNMADD/FNMSUB and FMLA/FMLS S lanes: a64_fpx_fma32_fma32(c, a, b, z) is a*b + z, the lifted
   code having already flipped the signs the op negates. FMA3's vfmadd231ss rounds once, as Arm does, and takes its
   NaN in Arm's order (the addend, then a, then b). Two cases differ, and both raise the invalid flag here (a64_ff_guard_),
   which sends the run to its exact copy:
   - 0 * inf + a quiet NaN: x86 returns that NaN with no flag, Arm the default NaN with IOC (so: any NaN result);
   - a result rounded up to the smallest normal, 2^-126: x86 detects tininess after rounding and raises no underflow,
     Arm detects it before, sets UFC, and under FZ returns a zero.
   Without FMA3 (x86-64-v2), the exact helper. */
#ifdef __FMA__
static inline __attribute__((always_inline)) uint64_t a64_ff_fma32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) {
    __m128 r = a64_ff_ss(z);
    (void)c;
    __asm__ volatile("vfmadd231ss %2, %1, %0" : "+x"(r) : "x"(a64_ff_ss(a)), "x"(a64_ff_ss(b)));
    a64_ff_guard_(a64_ff_bits(r), 1);
    return a64_ff_bits(r);
}
#else
#define a64_ff_fma32_fma32 a64_fpx_fma32_fma32
#endif
/* (A64_FASTFP=check) a run's result: counts per file, and the first mismatches of a clean fast run (which must not
   happen) with the run's guest address, on stderr (the game's log) */
static volatile long long a64_ff_n_[3];   /* runs, clean, mismatched when clean */
static __attribute__((noinline, cold)) void a64_ff_check_print_(unsigned pc, long long bad) {
    if (bad < 16) fprintf(stderr, "fastfp check: MISMATCH in the run at %#x (%s)\n", pc, __BASE_FILE__);
}
static inline void a64_ff_check(unsigned pc, int clean, int same) {
    const long long n = __atomic_add_fetch(&a64_ff_n_[0], 1, __ATOMIC_RELAXED);
    if (clean) __atomic_add_fetch(&a64_ff_n_[1], 1, __ATOMIC_RELAXED);
    if (clean && !same) a64_ff_check_print_(pc, __atomic_fetch_add(&a64_ff_n_[2], 1, __ATOMIC_RELAXED));
    if (!(n & ((1LL << 22) - 1)))
        fprintf(stderr, "fastfp check: %s: %lld runs, %lld clean, %lld mismatched when clean\n", __BASE_FILE__, n, a64_ff_n_[1], a64_ff_n_[2]);
}
#else
/* (no x86 MXCSR here: the fast copies are never run, and name the exact helpers) */
#define A64_FF_ON(c) 0
#define a64_ff_begin(c) 0u
#define a64_ff_end(c, m) 1
#define a64_ff_vadd32 a64_vfpx_add32
#define a64_ff_vsub32 a64_vfpx_sub32
#define a64_ff_vmul32 a64_vfpx_mul32
#define a64_ff_add32_add32 a64_fpx_add32_add32
#define a64_ff_sub32_sub32 a64_fpx_sub32_sub32
#define a64_ff_mul32_fma32 a64_fpx_mul32_fma32
#define a64_ff_fma32_fma32 a64_fpx_fma32_fma32
#define a64_ff_div32_div32 a64_fpx_div32_div32
#define a64_ff_sqrt32_sqrt32 a64_fpx_sqrt32_sqrt32
#define a64_ff_check(pc, clean, same) ((void)0)
#endif
"""


# A64_LOCAL_FLAGS: the fast scalar FCMP as a code (0: the exact block; else 4 | lt | eq << 1), appended to a64_fpx.h
FCMPK_HEADER = r"""
static inline __attribute__((always_inline)) uint32_t a64_fcmp32k(Ctx *c, const void *pa, const void *pb) {
    uint32_t a, b;
    float fa, fb;
    memcpy(&a, pa, 4);
    memcpy(&b, pb, 4);
    memcpy(&fa, &a, 4);
    memcpy(&fb, &b, 4);
    if (__builtin_isunordered(fa, fb)) return 0;
    if ((c->fpcr & (1ULL << 24)) && ((a & 0x7fffffffu) - 1u < 0x7fffffu || (b & 0x7fffffffu) - 1u < 0x7fffffu)) return 0;
    return 4u | (uint32_t)(fa < fb) | (uint32_t)(fa == fb) << 1;
}
static inline __attribute__((always_inline)) uint32_t a64_fcmp64k(Ctx *c, const void *pa, const void *pb) {
    uint64_t a, b;
    double fa, fb;
    memcpy(&a, pa, 8);
    memcpy(&b, pb, 8);
    memcpy(&fa, &a, 8);
    memcpy(&fb, &b, 8);
    if (__builtin_isunordered(fa, fb)) return 0;
    if ((c->fpcr & (1ULL << 24)) && ((a & 0x7fffffffffffffffULL) - 1u < 0xfffffffffffffULL ||
                                     (b & 0x7fffffffffffffffULL) - 1u < 0xfffffffffffffULL)) return 0;
    return 4u | (uint32_t)(fa < fb) | (uint32_t)(fa == fb) << 1;
}
"""

VFPX_HEADER = r"""
/* ---- 2/4-lane single-precision ops on SSE (vectorize_fpx in a64_lift.py): a lane kept only where the scalar fast
   path would keep it (the same tests, on every lane); otherwise each lane goes through its scalar helper ---- */
#if RECOMP_FPX_HOST == 2
static inline __m128i a64_v_ult(__m128i x, uint32_t k) {   /* unsigned x < k, per lane */
    return _mm_cmplt_epi32(_mm_xor_si128(x, _mm_set1_epi32((int)0x80000000u)), _mm_set1_epi32((int)(k ^ 0x80000000u)));
}
static inline __m128i a64_v_abs(__m128i x) { return _mm_and_si128(x, _mm_set1_epi32(0x7fffffff)); }
static inline __m128i a64_v_big(__m128i t) { return a64_v_ult(_mm_sub_epi32(a64_v_abs(t), _mm_set1_epi32(0x00800001)), 0x7effffffu); }
static inline __m128i a64_v_zero(__m128i x) { return _mm_cmpeq_epi32(a64_v_abs(x), _mm_setzero_si128()); }
static inline __m128i a64_v_nonfin(__m128i t) { const __m128i e = _mm_set1_epi32(0x7f800000); return _mm_cmpeq_epi32(_mm_and_si128(t, e), e); }
static inline __m128i a64_v_subn(__m128i x) { return a64_v_ult(_mm_sub_epi32(_mm_slli_epi32(x, 1), _mm_set1_epi32(1)), 0x00ffffffu); }
/* lanes that overflowed (an infinite result of finite operands), kept once FPSR.OFC is raised (FPX_FZ_HEADER) */
static inline __m128i a64_v_ovf(const Ctx *c, __m128i a, __m128i b, __m128i t) {
    if (!(c->fpsr & 4u)) return _mm_setzero_si128();
    const __m128i inf = _mm_cmpeq_epi32(a64_v_abs(t), _mm_set1_epi32(0x7f800000));
    return _mm_andnot_si128(_mm_or_si128(a64_v_nonfin(a), a64_v_nonfin(b)), inf);
}
#define A64_VFPX_SUM(name, op, scalar) \
static __attribute__((noinline, cold)) void a64_vfpx_##name##_lanes(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { \
    uint32_t a[4], b[4], r[4] = {0}; memcpy(a, n, 16); memcpy(b, m, 16); \
    for (int j = 0; j < k; j++) r[j] = (uint32_t)scalar(c, a[j], b[j]); \
    memcpy(d, r, 16); } \
static inline __attribute__((always_inline)) void a64_vfpx_##name(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { \
    if (RECOMP_LIKELY(RECOMP_FPX_OPEN(c))) { \
        const __m128i a = _mm_loadu_si128((const __m128i *)n), b = _mm_loadu_si128((const __m128i *)m); \
        const __m128i t = _mm_castps_si128(op(_mm_castsi128_ps(a), _mm_castsi128_ps(b))); \
        __m128i bad = a64_v_nonfin(t), r = t; \
        if (RECOMP_FPX_FZ(c)) { \
            bad = _mm_or_si128(bad, _mm_or_si128(a64_v_subn(a), a64_v_subn(b))); \
            if (c->fpsr & 8u) { /* (flags raised: a sum below the normal range, exact there, is a zero of its sign) */ \
                const __m128i tiny = _mm_cmplt_epi32(a64_v_abs(t), _mm_set1_epi32(0x00800000)); \
                r = _mm_or_si128(_mm_andnot_si128(tiny, t), _mm_and_si128(tiny, _mm_and_si128(t, _mm_set1_epi32((int)0x80000000u)))); \
            } else { \
                bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_or_si128(a64_v_big(t), a64_v_zero(t)), _mm_set1_epi32(-1))); \
            } \
        } \
        const int lanes = k == 4 ? 0xFFFF : 0x00FF; /* (overflowed lanes looked at only once a lane is out) */ \
        if (RECOMP_LIKELY(!(_mm_movemask_epi8(bad) & lanes)) || \
            !(_mm_movemask_epi8(_mm_andnot_si128(a64_v_ovf(c, a, b, t), bad)) & lanes)) { \
            if (k == 4) _mm_storeu_si128((__m128i *)d, r); else a64_vst_lo64(d, r); \
            return; } } \
    a64_vfpx_##name##_lanes(c, d, n, m, k); }
#else
#define A64_VFPX_SUM(name, op, scalar) \
static __attribute__((noinline)) void a64_vfpx_##name(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { \
    uint32_t a[4], b[4], r[4] = {0}; memcpy(a, n, 16); memcpy(b, m, 16); \
    for (int j = 0; j < k; j++) r[j] = (uint32_t)scalar(c, a[j], b[j]); \
    memcpy(d, r, 16); }
#endif
"""
VFPX_MUL_HEADER = r"""
static __attribute__((noinline, cold)) void a64_vfpx_mul32_lanes(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m) {
    uint32_t a[4], b[4], r[4]; memcpy(a, n, 16); memcpy(b, m, 16);
    for (int j = 0; j < 4; j++) r[j] = (uint32_t)a64_fpx_mul32_fma32(c, a[j], b[j], (a[j] ^ b[j]) & 0x80000000u);
    memcpy(d, r, 16); }
static inline __attribute__((always_inline)) void a64_vfpx_mul32(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) {
    (void)k;
#if RECOMP_FPX_HOST == 2
    if (RECOMP_LIKELY(RECOMP_FPX_OPEN(c))) {
        const __m128i a = _mm_loadu_si128((const __m128i *)n), b = _mm_loadu_si128((const __m128i *)m);
        const __m128i t = _mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b)));
        /* kept: big, or a finite product of a zero (RECOMP_FPX_KEEP32); under FZ no subnormal operand */
        const __m128i keep = _mm_or_si128(a64_v_big(t), _mm_andnot_si128(a64_v_nonfin(t), _mm_or_si128(a64_v_zero(a), a64_v_zero(b))));
        __m128i bad = _mm_andnot_si128(keep, _mm_set1_epi32(-1)), r = t;
        if (RECOMP_FPX_FZ(c)) {
            bad = _mm_or_si128(bad, _mm_or_si128(a64_v_subn(a), a64_v_subn(b)));
            if (c->fpsr & 8u) {   /* (flags raised: a tiny product is a zero of its sign; the smallest normal: exact) */
                const __m128i abs_t = a64_v_abs(t), minn = _mm_set1_epi32(0x00800000);
                const __m128i tiny = _mm_cmplt_epi32(abs_t, minn);
                const __m128i fin = _mm_andnot_si128(a64_v_nonfin(t), _mm_set1_epi32(-1));
                bad = _mm_andnot_si128(_mm_and_si128(tiny, fin), bad);   /* (still bad if a subnormal operand) */
                bad = _mm_or_si128(bad, _mm_or_si128(a64_v_subn(a), a64_v_subn(b)));
                bad = _mm_or_si128(bad, _mm_cmpeq_epi32(abs_t, minn));
                r = _mm_or_si128(_mm_andnot_si128(tiny, t), _mm_and_si128(tiny, _mm_and_si128(t, _mm_set1_epi32((int)0x80000000u))));
            }
        }
        if (RECOMP_LIKELY(!_mm_movemask_epi8(bad)) || !_mm_movemask_epi8(_mm_andnot_si128(a64_v_ovf(c, a, b, t), bad))) {
            _mm_storeu_si128((__m128i *)d, r);
            return;
        }
    }
#endif
    a64_vfpx_mul32_lanes(c, d, n, m);
}
"""


# Results past the fast path's range once their flags are already raised (TOTK's threads: FPSR = 0x9f, every cumulative
# flag set; FPCR.FZ = 1). FADD/FSUB S (a64_fpx_fz_add32 / a64_fpx_fz_sub32, OFC or UFC raised): an infinite result of
# finite operands is the native one, the round-to-nearest ±inf (~1M multiplies a second at the castle went through the
# exact body for it); under FZ with UFC raised a result below the normal range is the native one flushed to a zero of its
# sign. Sums are exact there (a tiny sum of normal numbers is a subnormal exactly). No subnormal operand (those flush
# first). FMUL/FNMUL/FMLA-family S: a64_fpx_sticky32 below.
FPX_FZ_HEADER = r"""
static inline __attribute__((always_inline)) int a64_fpx_fz_result(const Ctx *c, uint64_t a, uint64_t b, uint64_t t, uint64_t *v) {
    const uint32_t m = (uint32_t)t & 0x7fffffffu;
    if (m == 0x7f800000u) return (c->fpsr & 4u) && RECOMP_FPX_FIN32(a) && RECOMP_FPX_FIN32(b) ? (*v = t, 1) : 0;
    if (!RECOMP_FPX_FZ(c) || !(c->fpsr & 8u) || m > 0x7f800000u || RECOMP_FPX_SUBN32(a) || RECOMP_FPX_SUBN32(b)) return 0;
    if (m >= 0x00800000u) return (*v = t, 1);
    *v = t & 0x80000000u;
    return 1;
}
#if RECOMP_FPX_HOST
static inline __attribute__((always_inline)) int a64_fpx_fz_add32(const Ctx *c, uint64_t a, uint64_t b, uint64_t *v) {
    return a64_fpx_fz_result(c, a, b, recomp_fpx_fb(recomp_fpx_f(a) + recomp_fpx_f(b)), v); }
static inline __attribute__((always_inline)) int a64_fpx_fz_sub32(const Ctx *c, uint64_t a, uint64_t b, uint64_t *v) {
    return a64_fpx_fz_result(c, a, b, recomp_fpx_fb(recomp_fpx_f(a) - recomp_fpx_f(b)), v); }
#else
static inline int a64_fpx_fz_add32(const Ctx *c, uint64_t a, uint64_t b, uint64_t *v) { (void)c; (void)a; (void)b; (void)v; return 0; }
static inline int a64_fpx_fz_sub32(const Ctx *c, uint64_t a, uint64_t b, uint64_t *v) { (void)c; (void)a; (void)b; (void)v; return 0; }
#endif
/* FMUL/FNMUL/FMLA-family S past the fast path (the "sticky" rescue of the outlined wrappers a64_fpx_mul32_fma32 and
   a64_fpx_fma32_fma32, inside their RECOMP_FPX_OPEN gate): one hardware multiply or fused multiply-add, kept when every
   FPSR bit the Arm op could raise is already set and the value is the Arm one.
   FZ: a subnormal operand is flushed to a zero of its sign (IDC). NaN: never. Infinite: exact with an infinite operand,
   else an overflow (OFC). A zero product or an exact cancellation (a*b == -z in binary64, where the product is exact):
   exact. Otherwise a result at most the smallest normal may be tiny (UFC): FZ clear, the IEEE gradual-underflow result is
   Arm's; FZ set, below the smallest normal the exact value is tiny and Arm returns a zero of its sign, and exactly the
   smallest normal is ambiguous (falls back). Rejected before computing: a NaN operand, a subnormal operand under FZ with
   IDC clear, and FMUL of finite operands with neither OFC nor UFC set and nothing flushed (past the fast path, which keeps
   a large product and a finite product of a zero, such a product is tiny or infinite). Without a hardware FMA
   (RECOMP_FPX_FMA64; binary32 fmaf is the same instruction class) only the multiply is rescued. Returning 0 is always
   sound: the wrapper then takes the exact body. */
#ifndef A64_FPX_STICKY_PROBE
#define A64_FPX_STICKY_PROBE() ((void)0) /* (tests/test_fpx_sticky.py counts the rescues the wrappers keep here) */
#endif
static inline __attribute__((always_inline)) int a64_fpx_sticky32(const Ctx *c, uint64_t a, uint64_t b, uint64_t z, int mul, uint64_t *v) {
#if RECOMP_FPX_HOST
    const uint64_t s = c->fpsr;
    uint32_t x = (uint32_t)a, y = (uint32_t)b, w = (uint32_t)z, t;
#if !RECOMP_FPX_FMA64
    if (!mul) return 0;
#endif
    if ((x & 0x7fffffffu) > 0x7f800000u || (y & 0x7fffffffu) > 0x7f800000u || (w & 0x7fffffffu) > 0x7f800000u) return 0;
    if (RECOMP_FPX_FZ(c) && (RECOMP_FPX_SUBN32(x) || RECOMP_FPX_SUBN32(y) || (!mul && RECOMP_FPX_SUBN32(w)))) {
        if (!(s & 0x80u)) return 0;
        if (RECOMP_FPX_SUBN32(x)) x &= 0x80000000u;
        if (RECOMP_FPX_SUBN32(y)) y &= 0x80000000u;
        if (RECOMP_FPX_SUBN32(w)) w &= 0x80000000u;
    } else if (mul && !(s & 0xCu) && RECOMP_FPX_FIN32(x) && RECOMP_FPX_FIN32(y)) {
        return 0;
    }
    /* (FMUL: w is the zero of the product's sign, so x*y + w is x*y) */
#if RECOMP_FPX_FMA64
    t = (uint32_t)recomp_fpx_fb(mul ? recomp_fpx_f(x) * recomp_fpx_f(y)
                                    : __builtin_fmaf(recomp_fpx_f(x), recomp_fpx_f(y), recomp_fpx_f(w)));
#else
    t = (uint32_t)recomp_fpx_fb(recomp_fpx_f(x) * recomp_fpx_f(y));
#endif
    const uint32_t m = t & 0x7fffffffu;
    if (m > 0x7f800000u) return 0;
    if (m == 0x7f800000u) {
        if (RECOMP_FPX_FIN32(x) && RECOMP_FPX_FIN32(y) && RECOMP_FPX_FIN32(w) && !(s & 4u)) return 0;
    } else if (m <= 0x00800000u && !RECOMP_FPX_ZERO32(x) && !RECOMP_FPX_ZERO32(y)) {
        if (!(s & 8u)) {   /* UFC clear: only an exact cancellation (+0, no bit) is kept */
            if (mul || m != 0 || (double)recomp_fpx_f(x) * (double)recomp_fpx_f(y) != -(double)recomp_fpx_f(w)) return 0;
        } else if (RECOMP_FPX_FZ(c)) {
            if (m == 0x00800000u) return 0;
            t &= 0x80000000u;
        }
    }
    *v = t;
    return 1;
#else
    (void)c; (void)a; (void)b; (void)z; (void)mul; (void)v; return 0;
#endif
}
"""


# Hot functions (A64_HOT_FUNCS=<file>: one function name a line, e.g. the hottest few hundred of a HOS_PROFILE_PCS or
# HOS_PROFILE_ORDER profile; ~70% of TOTK's lifted-code time is in its 300 hottest) call the scalar FP helpers' inline
# copy: a call makes the caller store and reload the guest registers it keeps in host registers around every float
# op; inlining the fast path everywhere made the exe 40% bigger and slower (RX_FPX_INLINE, build.py).
HOT_FPX = re.compile(r"\ba64_fpx_(?!hot_|fz_)(\w+)\(c,")


def hot_text(text):
    """a hot function's text: its FPX helper calls go through the copies with the fast path inline"""
    return HOT_FPX.sub(r"a64_fpx_hot_\1(c,", text)


def hot_functions():
    path = os.environ.get("A64_HOT_FUNCS")
    if not path:
        return set()
    return {w for line in open(path) for w in line.split()[:1] if w.startswith("f_")}


# Profile-guided devirtualization (A64_DEVIRT=<file>: "site target" a line, hex, from scripts/devirt.py over an
# A64_SITE_PROFILE run): a BLR at a listed site compares its target with the one the profile saw there almost always and
# calls that function directly, so the ThinLTO link can inline it (C++ virtual calls: getters, small overrides); any
# other target still goes through A64_ICALL. Measured 2026-10-22 (TOTK castle, 3 interleaved rounds): ~100k indirect
# calls a frame, 7799 sites listed covering 88% of them; 73.6 -> 73.5 fps, game threads -0.3 ms CPU a frame (noise):
# a site's cached indirect call is already well predicted. Not on by default.
def devirt_sites():
    path = os.environ.get("A64_DEVIRT")
    if not path:
        return {}
    sites = {}
    for line in open(path):
        p = line.split()
        if len(p) >= 2 and not line.startswith("#"):
            sites[int(p[0], 16)] = int(p[1], 16)
    return sites


def fpx_header(used):
    out = ["/* Generated by a64_lift.py: one helper per FPX1 guard shape (see FPX_GUARD). A64_FPX_FN decides whether",
           "   the guard is inlined (fast code, slow compile) or called (default). */",
           "#ifndef A64_FPX_FN", "#define A64_FPX_FN static __attribute__((noinline))", "#endif",
           FPX_FZ_HEADER]
    for fast, exact, nf, ne in sorted(used):
        params = ", ".join("uint64_t a%d" % i for i in range(ne))
        # past the fast path, still inside its gate (tested once): the rescues above, else the exact body
        rescue = ""
        if (fast, exact) in (("mul32", "fma32"), ("fma32", "fma32")):
            rescue = "if (a64_fpx_sticky32(c, a0, a1, a2, %d, &v)) { A64_FPX_STICKY_PROBE(); return v; } " % (fast == "mul32")
        elif fast in ("add32", "sub32"):
            rescue = "if ((c->fpsr & 0xCu) && a64_fpx_fz_%s(c, a0, a1, &v)) return v; " % fast
        out.append("A64_FPX_FN uint64_t a64_fpx_%s_%s(Ctx *c, %s) { uint64_t v; "
                   "if (RECOMP_FPX_OPEN(c)) { if (RECOMP_LIKELY(recomp_fpx_%s(%s, RECOMP_FPX_FZ(c), &v))) return v; %s} "
                   "return recomp_fpe_%s(c, %s); }" % (fast, exact, params, fast,
                                                       ", ".join("a%d" % i for i in range(nf)), rescue, exact,
                                                       ", ".join("a%d" % i for i in range(ne))))
        # (the hot functions' copy, see HOT_FPX: the fast path inline, everything else through the helper)
        out.append("static inline __attribute__((always_inline)) uint64_t a64_fpx_hot_%s_%s(Ctx *c, %s) { uint64_t v; "
                   "if (RECOMP_FPX_OPEN(c) && RECOMP_LIKELY(recomp_fpx_%s(%s, RECOMP_FPX_FZ(c), &v))) return v; "
                   "return a64_fpx_%s_%s(c, %s); }" % (fast, exact, params, fast, ", ".join("a%d" % i for i in range(nf)),
                                                       fast, exact, ", ".join("a%d" % i for i in range(ne))))
    out.append(VSET_HEADER)
    out.append(VFCMP_HEADER)
    if any(e[:2] in (("add32", "add32"), ("sub32", "sub32"), ("mul32", "fma32")) for e in used):
        out.append(VFPX_HEADER)
        if ("add32", "add32", 2, 2) in used:
            out.append("A64_VFPX_SUM(add32, _mm_add_ps, a64_fpx_add32_add32)")
        if ("sub32", "sub32", 2, 2) in used:
            out.append("A64_VFPX_SUM(sub32, _mm_sub_ps, a64_fpx_sub32_sub32)")
        if ("mul32", "fma32", 2, 3) in used:
            out.append(VFPX_MUL_HEADER)
    return "\n".join(out) + "\n"


def op_text(text, fpx):
    """one instruction's emitted C as lifted code has it: FPX guards outlined (their shapes added to fpx, which
    fpx_header turns into a64_fpx.h), NEON FP vectorized, stores widened, flags packed"""
    return stack_stores(pack_flags(widen_vstores(vectorize_fpx(outline_fpx(text, fpx)))))


class Lifter:
    def __init__(self, img, comments=False):
        self.img = img
        self.entries = set(img.entries) | set(img.hooks)
        self.hooks = dict(img.hooks)
        self.names = getattr(img, "names", {}) or {}
        self.comments = comments
        # (hookshot) C emitted right after the instruction at a guest address ({pc: text}): a game patch that adds to an
        # instruction instead of replacing a function (nx2/oot_wide.py). Only after plain instructions (kind "op")
        self.after = {}
        self.funcs = {}       # entry -> (body {pc: kind}, labels set)
        self.jt = {}          # br pc -> [targets]
        self.jt_missing = {}      # br pc -> its register, for BRs whose table address was not found
        self.ijump = Counter()
        self.unhandled = Counter()
        self.plt = {}         # stub entry -> final target
        self.stats = Counter()
        self.no_local = set()   # functions whose registers stay in the context (A64_LOCAL_REGS; code spliced in)
        # function starts from unwind info bound jump-table targets; without them, every entry does
        self.eh_starts = sorted(getattr(img, "eh_starts", None) or self.entries)
        self.devirt = devirt_sites()   # BLR pc -> its usual target (A64_DEVIRT)

    # ---- discovery ----------------------------------------------------------------------------------
    def discover(self, max_insns=0):
        for e in sorted(self.entries):
            if e in self.hooks:
                continue
            self.funcs[e] = self.walk(e)
        # A table found through another switch's edges needs those resolved first: retry the misses once with
        # every function's tables known, and re-walk the functions that contain a newly resolved one.
        missing, self.jt_missing = dict(self.jt_missing), {}
        self.__dict__.pop("_preds", None)
        redo = set()
        for pc, reg in missing.items():
            targets = self.jump_table(pc, reg)
            if targets:
                self.jt[pc] = targets
                redo |= {e for e, (body, _) in self.funcs.items() if pc in body}
        for e in redo:
            self.funcs[e] = self.walk(e)
        self.stats["jt_second_pass"] = len(missing) - len(self.jt_missing)
        self.stats["jt_no_table"] = len(self.jt_missing)
        if max_insns:
            self.split_large(max_insns)
        for e in self.funcs:
            t = self.plt_target(e)
            if t is not None:
                self.plt[e] = t
        self.find_setjmp_owners()   # (after the PLT stubs: their callees are resolved through them)

    SETJMP_NAMES = ("__setjmp", "setjmp", "_setjmp")

    def find_setjmp_owners(self):
        """(hookshot) the functions that call __setjmp, with the call's return addresses: entries (walked here), so
        a longjmp can run the owner's code after its setjmp (emit_function wraps the owner's callers)"""
        self.setjmp_owner = {}
        for e, (body, _) in list(self.funcs.items()):
            for pc, (k, w) in body.items():
                if k[0] == "bl" and self.names.get(self.callee(k[1])) in self.SETJMP_NAMES:
                    self.setjmp_owner.setdefault(e, []).append(pc + 4)
        for rets in self.setjmp_owner.values():
            for r in rets:
                if r not in self.funcs:
                    self.entries.add(r)
                    self.funcs[r] = self.walk(r)
        self.stats["setjmp_owners"] = len(self.setjmp_owner)

    def split_large(self, max_insns):
        """Give functions longer than max_insns extra entries, about every max_insns / 2 instructions, only where
        no branch of the function crosses the cut (so neither half re-walks code of the other). Extra entries are
        always correct in this model: control reaching one tail-calls it. A cut trades one direct tail call for
        compile time, which grows faster than linearly with function size at -O1 (TOTK's largest function is
        18,156 instructions). Functions that are one big loop or switch cannot be cut and stay whole."""
        new = set()
        for e, (body, _) in self.funcs.items():
            if len(body) <= max_insns:
                continue
            pcs = sorted(body)
            diff = [0] * (len(pcs) + 1)
            for pc, (k, _w) in body.items():
                targets = [k[1]] if k[0] in ("b", "bcond", "cbz", "tbz") else self.jt.get(pc, []) if k[0] == "br" else []
                for t in targets:
                    if t not in body or t == pc:
                        continue
                    if t > pc:      # forward: a cut strictly between pc and t splits the edge
                        lo, hi = bisect.bisect_right(pcs, pc), bisect.bisect_left(pcs, t)
                    else:           # backward: a cut in (t, pc] puts the branch after the cut and its target before
                        lo, hi = bisect.bisect_right(pcs, t), bisect.bisect_right(pcs, pc)
                    if lo < hi:
                        diff[lo] += 1
                        diff[hi] -= 1
            step = max_insns // 2
            last, cover = 0, 0
            for i, pc in enumerate(pcs):
                cover += diff[i]
                if i - last >= step and cover == 0 and i and pcs[i - 1] == pc - 4:
                    new.add(pc)
                    last = i
        new -= self.entries
        if not new:
            return
        self.entries |= new
        self.stats["split_entries"] += len(new)
        affected = {e for e, (body, _) in self.funcs.items() if not new.isdisjoint(body.keys())}
        for a in sorted(new | affected):
            self.funcs[a] = self.walk(a)

    def walk(self, entry):
        img, entries = self.img, self.entries
        body, labels, todo = {}, set(), [entry]

        def visit(t):
            if t != entry and t in entries:
                return
            if img.in_code(t):
                labels.add(t)
                todo.append(t)

        while todo:
            pc = todo.pop()
            while pc not in body:
                if (pc != entry and pc in entries) or not img.in_code(pc):
                    break
                w = img.word(pc)
                k = classify(w, pc)
                body[pc] = (k, w)
                kind = k[0]
                if kind == "b":
                    visit(k[1])
                    break
                if kind in ("bcond", "cbz", "tbz"):
                    visit(k[1])
                elif kind == "br":
                    targets = self.jump_table(pc, k[1])
                    if targets:
                        self.jt[pc] = targets
                        self.add_jt_edges(pc, targets)
                        for t in targets:
                            visit(t)
                    break
                elif kind in ("ret", "trap"):
                    break
                pc += 4
        return body, labels

    # ---- jump tables ---------------------------------------------------------------------------------
    def jump_table(self, br_pc, reg, window=32):
        """Symbolically run the straight-line code before a BR. Recognises LLVM's AArch64 tables: the target is
        base + (entry << k), entry loaded from a constant table. Seen in TOTK:
          - the table address hoisted out of the window (loop-invariant): found by scanning further back;
          - the bound on a copy of the index (mov w8, w26 / and x12, x1, #0xff) or set by CCMP;
          - no bound at all (index from cset/bfi): entries are read until one leaves the function.
          - one register holding a different table on each path into a shared dispatch: every constant reaching
            the br through the CFG is a candidate (reaching_constants).
        Targets must lie in the containing function ([start, next .eh_frame function start))."""
        img = self.img
        R = {}
        bound = None
        bounds = {}   # register -> number of table entries its value can index (cmp + b.hi/b.hs, and-mask, copies)
        for pc in range(br_pc - 4 * window, br_pc, 4):
            if not img.in_code(pc):
                R.clear()
                bounds.clear()
                continue
            w = img.word(pc)
            if (w & 0xFC000000) == 0x14000000 or (w & 0xFFFFFC1F) in (0xD65F0000, 0xD61F0000):
                # ret / b / br: what follows is reached from elsewhere, so nothing above is known there
                R.clear()
                bounds.clear()
                continue
            d = w & 31
            n = (w >> 5) & 31
            m = (w >> 16) & 31
            # index bounds, tracked beside the values
            if (w & 0x7F80001F) == 0x7100001F or (w & 0x7FE00C10) == 0x7A400800:      # CMP / CCMP #imm
                last_cmp = (n, (w >> 10) & 0xFFF if (w & 0x7F80001F) == 0x7100001F else (w >> 16) & 31)
                bounds["cmp"] = last_cmp
            elif (w & 0xFF00001F) in (0x54000008, 0x54000002) and "cmp" in bounds:       # B.HI / B.HS
                r, imm = bounds["cmp"]
                bounds[r] = imm + 1 if (w & 15) == 8 else imm
            elif (w & 0x7F800000) == 0x12000000 and not (w >> 16) & 63:                    # AND #(2^k - 1)
                imms, nbit = (w >> 10) & 63, (w >> 22) & 1
                if (nbit or not w >> 31) and imms < 31:
                    bounds[d] = 1 << (imms + 1)
                else:
                    bounds.pop(d, None)
            elif (w & 0x7FE0FFE0) == 0x2A0003E0:                                           # MOV Wd/Xd, Wm/Xm
                if m in bounds:
                    bounds[d] = bounds[m]
                else:
                    bounds.pop(d, None)
            elif d != 31 and self.writes(w, d):
                bounds.pop(d, None)
            if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                R[d] = ("c", (pc & ~0xFFF) + (sx(imm, 21) << 12))
            elif (w & 0x9F000000) == 0x10000000:                                 # ADR
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                R[d] = ("c", pc + sx(imm, 21))
            elif (w & 0xFF800000) == 0x91000000:                                 # ADD Xd, Xn, #imm{, lsl 12}
                v = R.get(n, ("reg", n))
                imm = ((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)
                if v and v[0] == "c":
                    R[d] = ("c", v[1] + imm)
                elif v and v[0] == "reg" and d == n:
                    R.pop(d, None)   # adrp before the window, add inside it: resolved at the br (reaching_constants)
                elif v and v[0] == "reg" and len(v) == 2:
                    R[d] = ("reg", n, imm)
                else:
                    R[d] = None
            elif (w & 0x3B200C00) == 0x38200800 and not (w >> 26) & 1:           # LDR* (register offset)
                size, opc, S = w >> 30, (w >> 22) & 3, (w >> 12) & 1
                v = R.get(n, ("reg", n))                                          # an undefined base: hoisted
                R[d] = ("ld", v, m, 1 << size, opc >= 2, size if S else 0) if opc != 0 and v else None
            elif (w & 0xFF200000) == 0x8B000000 and (w >> 22) & 3 == 0:          # ADD Xd, Xn, Xm, LSL #k
                R[d] = ("add", R.get(n), R.get(m), (w >> 10) & 63)
            elif (w & 0xFFE00000) == 0x8B200000:                                 # ADD Xd, Xn, Wm, <ext> #k
                R[d] = ("add", R.get(n), R.get(m), (w >> 10) & 7)
            elif (w & 0x7F80001F) == 0x7100001F:                                 # CMP Wn/Xn, #imm
                bound = {"imm": (w >> 10) & 0xFFF}
            elif (w & 0x7FE00C10) == 0x7A400800:                                 # CCMP Wn/Xn, #imm5, #nzcv, cond
                bound = {"imm": (w >> 16) & 31}
            elif (w & 0xFF00001F) in (0x54000008, 0x54000002) and bound and "count" not in bound:   # B.HI / B.HS
                bound["count"] = bound["imm"] + 1 if (w & 15) == 8 else bound["imm"]
            elif self.writes(w, d):
                # anything else that writes Xd forgets it (stores, compares, branches and SIMD ops do not)
                R[d] = None
            rt2 = (w >> 10) & 31
            if ((w >> 27) & 7) == 5 and (w >> 22) & 1 and not (w >> 26) & 1:   # LDP also writes Rt2
                R[rt2] = None
                bounds.pop(rt2, None)
        v = R.get(reg)
        if v and v[0] == "c":   # br to a code constant: `adr x19, label ... br x19` (a continuation in a register)
            lo, hi = self.function_extent(br_pc)
            if lo <= v[1] < hi and img.in_code(v[1]):
                self.stats["br_constant"] += 1
                return [v[1]]
            return None
        if not v or v[0] != "add":
            return self.adr_targets(br_pc, reg)
        a, b, k = v[1], v[2], v[3]
        if a and a[0] == "ld":
            a, b = b, a
        if not (a and a[0] == "c" and b and b[0] == "ld"):
            self.stats["jt_unrecognized"] += 1
            self.jt_missing[br_pc] = reg
            return None
        base, (_, table, idx, size, signed, _shift) = a[1], b
        if isinstance(bounds.get(idx), int):        # a bound on the very register that indexes the table wins
            bound = {"count": bounds[idx]}
        if table[0] == "reg":
            # The table address was set outside the window. Every ADRP(+ADD) constant reaching the br through the
            # function's CFG is a candidate (one register can hold a different table on each path into a shared
            # dispatch), plus the address-order guesses; the switch takes the union of their targets.
            reg_t = table[1]
            off = table[2] if len(table) > 2 else 0   # add xT, xN, #off in the window, xN set before it
            tables = set(self.reaching_constants(br_pc, reg_t))
            guess = self.hoisted_constant(br_pc, reg_t)   # from the br itself: the load does not write reg_t
            if guess is None:
                guess = self.function_constant(br_pc, reg_t)
            if guess is not None:
                tables.add(guess)
            tables = {t + off for t in tables}
            if not tables:
                self.stats["jt_no_table"] += 1
                self.jt_missing[br_pc] = reg
                return None
            if len(tables) > 1:
                self.stats["jt_multi_table"] += 1
        else:
            tables = {table[1]}
        lo, hi = self.function_extent(br_pc)
        count = bound.get("count") if bound else None
        if not (count and 0 < count <= 4096):
            count = None
        fmt = {1: "b", 2: "h", 4: "i", 8: "q"}[size]
        fmt = fmt if signed else fmt.upper()

        def read(table, limit, strict):
            out = []
            for i in range(limit):
                try:
                    e = struct.unpack("<" + fmt, img.read(table + i * size, size))[0]
                except ValueError:
                    break
                t = (base + (e << k)) & 0xFFFFFFFFFFFFFFFF
                if not (lo <= t < hi and img.in_code(t)):
                    if strict:
                        return None
                    break
                out.append(t)
            return out

        targets, bounded = [], True
        for table in sorted(tables):
            got = read(table, count, True) if count else None
            if got is None:
                # no bound, or one that over-reads into the next table: read until an entry leaves the function
                if count:
                    self.stats["jt_out_of_function"] += 1
                got = read(table, 256 if size == 1 else 1024, False)
                bounded = False
            targets += [t for t in got if t not in targets]
        if not targets:
            return None
        self.stats["jt_bounded" if bounded and count else "jt_unbounded"] += 1
        return targets

    def preds(self, lo, hi):
        """Predecessor map of the code in [lo, hi): fall-through, direct branches, and jump tables resolved so far.
        Cached per function extent."""
        cache = self.__dict__.setdefault("_preds", {})
        if lo in cache:
            return cache[lo]
        img = self.img
        p = {}
        for pc in range(lo, hi, 4):
            if not img.in_code(pc):
                continue
            k = classify(img.word(pc), pc)
            kind = k[0]
            if kind == "b":
                succ = (k[1],)
            elif kind in ("bcond", "cbz", "tbz"):
                succ = (k[1], pc + 4)
            elif kind == "br":
                succ = tuple(self.jt.get(pc, ()))
            elif kind in ("ret", "trap"):
                succ = ()
            else:
                succ = (pc + 4,)
            for s in succ:
                p.setdefault(s, []).append(pc)
        cache[lo] = p
        return p

    def add_jt_edges(self, br_pc, targets):
        """Keep a cached predecessor map current when a jump table of its function is resolved."""
        cache = self.__dict__.get("_preds")
        if cache:
            lo, _ = self.function_extent(br_pc)
            if lo in cache:
                for t in targets:
                    cache[lo].setdefault(t, []).append(br_pc)

    def reaching_constants(self, at, reg, limit=50000, depth=0):
        """Every ADRP (+ ADD #imm) constant that reaches `at` in `reg` along the function's CFG, walking back from
        `at` until each path writes `reg`. A value copied (mov) or spilled to a stack slot and reloaded (ldr/ldp
        from [sp, #off], LLVM's spill of a hoisted table address under register pressure) is followed to where it
        was set. Paths that write something else, or a call that clobbers a caller-saved `reg`, contribute
        nothing (the switch's default covers them)."""
        return sorted(self._reaching(at, ("reg", reg), limit, depth))

    def _reaching(self, at, loc, limit, depth):
        img = self.img
        lo, hi = self.function_extent(at)
        if hi - lo > 0x40000 or depth > 4:
            return set()
        preds = self.preds(lo, hi)
        found, seen, todo = set(), set(), [at]
        kind_loc, x = loc
        while todo and len(seen) < limit:
            for p in preds.get(todo.pop(), ()):
                if p in seen:
                    continue
                seen.add(p)
                w = img.word(p)
                if kind_loc == "reg":
                    if classify(w, p)[0] in ("bl", "blr") and (x <= 18 or x == 30):
                        continue
                    if not self.writes(w, x):
                        todo.append(p)
                        continue
                    if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                        imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                        found.add((p & ~0xFFF) + (sx(imm, 21) << 12))
                    elif (w & 0xFF800000) == 0x91000000:                                 # ADD Xreg, Xn, #imm
                        v = self.hoisted_constant(p, (w >> 5) & 31, 64)
                        if v is not None:
                            found.add(v + (((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)))
                    elif (w & 0xFFE0FFE0) == 0xAA0003E0:                                 # MOV Xreg, Xm
                        found |= self._reaching(p, ("reg", (w >> 16) & 31), limit, depth + 1)
                    elif (w & 0xFFC003E0) == 0xF94003E0:                                 # LDR Xreg, [sp, #off]
                        found |= self._reaching(p, ("slot", ((w >> 10) & 0xFFF) * 8), limit, depth + 1)
                    elif (w & 0xFFC003E0) == 0xA94003E0:                                 # LDP X, X, [sp, #off]
                        off = sx((w >> 15) & 0x7F, 7) * 8 + (8 if (w & 31) != x else 0)
                        found |= self._reaching(p, ("slot", off), limit, depth + 1)
                    continue
                # a stack slot: the store that last wrote it
                if (w & 0xFFC003E0) == 0xF90003E0 and ((w >> 10) & 0xFFF) * 8 == x:    # STR Xt, [sp, #off]
                    found |= self._reaching(p, ("reg", w & 31), limit, depth + 1)
                    continue
                if (w & 0xFFC003E0) == 0xA90003E0:                                       # STP Xt1, Xt2, [sp, #off]
                    off = sx((w >> 15) & 0x7F, 7) * 8
                    if x in (off, off + 8):
                        found |= self._reaching(p, ("reg", w & 31 if x == off else (w >> 10) & 31), limit, depth + 1)
                        continue
                if ((w & 31) == 31 and (w & 0x7F800000) in (0x51000000, 0x11000000)) or (    # ADD/SUB sp, #imm
                        (w >> 5) & 31 == 31 and ((w & 0x3B200C00) in (0x38000C00, 0x38000400)
                                                 or (w & 0x3A800000) == 0x28800000)):    # [sp] pre/post-index
                    continue                                                             # sp moved: slots differ
                todo.append(p)
        return found

    @classmethod
    def writes(cls, w, reg):
        """True if the instruction w may write X`reg`: Rd/Rt (bits 0-4) of anything but a store, compare or
        branch, and Rt2 of a load pair."""
        if ((w >> 25) & 7) == 7:
            # SIMD & FP data processing: Rd is a V register, except FP<->integer conversions/moves and UMOV/SMOV
            return (w & 31) == reg and ((w & 0x5F20FC00) == 0x1E200000 or (w & 0xBFE0DC00) == 0x0E002C00)
        if (w & 0x0A000000) == 0x08000000 and (w >> 26) & 1:
            return False   # SIMD & FP load/store: Rt (and Rt2) are V registers
        if (w & 31) == reg and not cls.reads_rt_only(w):
            return True
        return ((w >> 27) & 7) == 5 and (w >> 22) & 1 and not (w >> 26) & 1 and ((w >> 10) & 31) == reg

    def function_constant(self, br_pc, reg):
        """The one ADRP(+ADD) constant the function ever assigns to `reg`, or None if it assigns none or several.
        Catches table addresses hoisted far from their use, where a backward scan in address order runs into an
        unrelated path's write first (an epilogue's `ldp x26, ...` after a `ret`)."""
        img = self.img
        lo, hi = self.function_extent(br_pc)
        if hi - lo > 0x40000:
            return None
        page = {}
        found = set()
        for pc in range(lo, hi, 4):
            w = img.word(pc)
            d = w & 31
            if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                page[d] = (pc & ~0xFFF) + (sx(imm, 21) << 12)
            elif (w & 0xFF800000) == 0x91000000 and ((w >> 5) & 31) in page:    # ADD Xd, Xn, #imm
                v = page[(w >> 5) & 31] + (((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0))
                if d == reg:
                    found.add(v)
        return found.pop() if len(found) == 1 else None

    def adr_targets(self, br_pc, reg):
        """Every `adr xreg, label` into the same function: the candidate targets of a `br xreg` whose value the
        straight-line window could not pin down (a continuation set on several paths). The switch keeps a
        default case, so a candidate list that misses one still falls back to an indirect jump."""
        img = self.img
        lo, hi = self.function_extent(br_pc)
        if hi - lo > 0x40000:
            return None
        targets = []
        for pc in range(lo, hi, 4):
            w = img.word(pc)
            if (w & 0x9F00001F) == (0x10000000 | reg):
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                t = pc + sx(imm, 21)
                if lo <= t < hi and img.in_code(t):
                    targets.append(t)
        if targets:
            self.stats["br_adr_candidates"] += 1
        return targets or None

    def hoisted_constant(self, before, reg, scan=1024):
        """The constant ADRP (+ ADD) last wrote to `reg` before address `before`, scanning back in address order.
        Any other write to `reg` on the way makes it unknown. Address order is a heuristic for 'earlier': it is
        how LLVM lays out the hoisted setup of a loop-invariant table address."""
        img = self.img
        pc = before - 4
        for _ in range(scan):
            if not img.in_code(pc):
                return None
            w = img.word(pc)
            if (w & 31) == reg and not self.reads_rt_only(w):
                if (w & 0x9F000000) == 0x90000000:                               # ADRP Xreg, page
                    imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                    return (pc & ~0xFFF) + (sx(imm, 21) << 12)
                if (w & 0xFF800000) == 0x91000000:                               # ADD Xreg, Xn, #imm
                    imm = ((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)
                    v = self.hoisted_constant(pc, (w >> 5) & 31, 64)
                    return None if v is None else v + imm
                return None
            pc -= 4
        return None

    @staticmethod
    def reads_rt_only(w):
        """True for encodings whose bits 0-4 are not a destination: stores, compares, branches."""
        return ((w & 0x3A000000) == 0x38000000 and not (w >> 22) & 1) or (w & 0x1C000000) == 0x14000000 \
            or (w & 0x7E000000) in (0x34000000, 0x36000000) \
            or (((w >> 27) & 7) == 5 and not (w >> 22) & 1)                     # STP / STNP

    def function_extent(self, pc):
        starts = self.eh_starts
        i = bisect.bisect_right(starts, pc)
        lo = starts[i - 1] if i else 0
        hi = starts[i] if i < len(starts) else 1 << 64
        return lo, hi

    # ---- PLT stubs -----------------------------------------------------------------------------------
    def plt_target(self, e):
        img = self.img
        try:
            w0, w1, w2, w3 = (img.word(e + 4 * i) for i in range(4))
        except Exception:
            return None
        if (w0 & 0x9F00001F) != 0x90000010 or (w1 & 0xFFC003FF) != 0xF9400211 or (w2 & 0xFFC003FF) != 0x91000210 \
                or w3 != 0xD61F0220:
            return None
        imm = ((w0 >> 3) & 0x1FFFFC) | ((w0 >> 29) & 3)
        slot = (e & ~0xFFF) + (sx(imm, 21) << 12) + ((w1 >> 10) & 0xFFF) * 8
        t = struct.unpack("<Q", img.read(slot, 8))[0]
        return (t, slot) if t in self.entries else None

    def callee(self, t):
        seen = 0
        while t in self.plt and seen < 4:
            t = self.plt[t][0]
            seen += 1
        return t

    def fname(self, t):
        # Hooked addresses keep their f_ name too (a64_hooks.c defines it as a tail call of the hook), so hooking
        # or unhooking a function changes one small file instead of every call site in the lifted code.
        return "f_%08x" % t

    # ---- emission ------------------------------------------------------------------------------------
    def transfer(self, pc, t, labels, body):
        """C for control reaching t from an instruction at pc (self.cur_i: its index, for the context mark)."""
        if t in body and (t not in self.entries or t == self.cur):
            return "goto L_%08x;" % t
        if t in self.entries:
            name = self.fname(self.callee(t))
            self.refs.add(name)
            return SYNC_TAIL % self.cur_i + "TAILCALL(%s);" % name
        self.stats["bad_target"] += 1
        return SYNC_TAIL % self.cur_i + "a64_trap(c, 0x%xULL, 0); return;" % t

    def dead_flags(self, body, pcs, texts):
        """pc -> the flags an instruction stores that nothing reads (FLAG_BIT bits; see FLAG_WRITE)"""
        e = self.cur

        def inner(t):   # the instruction a branch to t goes to here (a goto), or None: it leaves the function
            return t if t in body and (t not in self.entries or t == e) else None
        n = len(pcs)
        at = {pc: i for i, pc in enumerate(pcs)}
        reads, kills, succ, exits = [0] * n, [0] * n, [()] * n, [0] * n
        for i, pc in enumerate(pcs):
            k, w = body[pc]
            kind = k[0]
            targets, falls = [], True
            if kind == "op":
                text, fl = texts[(w, pc) if pc_relative(w) else w]
                if fl & a64emit.UNHANDLED:
                    reads[i], falls = 15, False
                else:
                    reads[i], kills[i] = flag_masks(text)
            elif kind == "b":
                targets, falls = [k[1]], False
            elif kind == "bcond":
                reads[i], targets = COND_READS[k[2]], [k[1]]
            elif kind in ("cbz", "tbz"):
                targets = [k[1]]
            elif kind in ("bl", "blr"):
                if kind == "bl" and self.callee(k[1]) not in self.entries:
                    reads[i], falls = 15, False   # (a trap)
                else:
                    kills[i] = 15   # a call clobbers NZCV
            elif kind == "ret":
                falls = False
            elif kind == "br":
                targets, falls = list(self.jt.get(pc) or ()), False
                exits[i] = 15   # (the switch's default: an indirect jump)
            elif kind in ("svc", "trap"):
                reads[i] = 15
                falls = kind == "svc"
            if falls:
                targets.append(pc + 4)
            s = []
            for t in targets:
                t2 = inner(t)
                if t2 is None:
                    exits[i] = 15
                else:
                    s.append(at[t2])
            succ[i] = tuple(s)
        live_in = [0] * n
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                out = exits[i]
                for j in succ[i]:
                    out |= live_in[j]
                v = reads[i] | (out & ~kills[i])
                if v != live_in[i]:
                    live_in[i] = v
                    changed = True
        dead = {}
        for i, pc in enumerate(pcs):
            if kills[i] and body[pc][0][0] == "op":
                out = exits[i]
                for j in succ[i]:
                    out |= live_in[j]
                if kills[i] & ~out:
                    dead[pc] = kills[i] & ~out
        return dead

    def dirty_regs(self, body, pcs, texts):
        """A64_LOCAL_REGS: per instruction index, the registers that may differ from the context before it and after it
        (written since the function's entry or the last call or SVC, which write back and read again)"""
        e = self.cur

        def inner(t):
            return t if t in body and (t not in self.entries or t == e) else None
        n = len(pcs)
        at = {pc: i for i, pc in enumerate(pcs)}
        writes, sync, succ = [0] * n, [False] * n, [()] * n
        for i, pc in enumerate(pcs):
            k, w = body[pc]
            kind = k[0]
            targets, falls = [], True
            if kind == "op":
                text, fl = texts[(w, pc) if pc_relative(w) else w]
                if fl & a64emit.UNHANDLED:
                    falls = False
                else:
                    writes[i] = reg_writes(text)
            elif kind == "b":
                targets, falls = [k[1]], False
            elif kind in ("bcond", "cbz", "tbz"):
                targets = [k[1]]
            elif kind in ("bl", "blr", "svc"):
                sync[i] = True
            elif kind in ("ret", "trap"):
                falls = False
            elif kind == "br":
                targets, falls = list(self.jt.get(pc) or ()), False
            if falls:
                targets.append(pc + 4)
            succ[i] = tuple(at[t2] for t2 in (inner(t) for t in targets) if t2 is not None)
        dirty_in = [0] * n
        changed = True
        while changed:
            changed = False
            for i in range(n):
                out = 0 if sync[i] else dirty_in[i] | writes[i]
                for j in succ[i]:
                    if out & ~dirty_in[j]:
                        dirty_in[j] |= out
                        changed = True
        return dirty_in, [0 if sync[i] else dirty_in[i] | writes[i] for i in range(n)]

    def emit_function(self, e, texts):
        """C text of one function, and the set of C names it calls."""
        body, labels = self.funcs[e]
        self.cur = e
        self.refs = set()
        out = ["void f_%08x(Ctx *__restrict A64_NOESC c) {" % e]
        if e in self.names and self.comments:
            out[0] = "/* %s */\n" % self.names[e] + out[0]
        if e in self.plt:   # a PLT stub reached indirectly: x16 = GOT slot, x17 = target, as the stub leaves them
            t, slot = self.plt[e]
            name = self.fname(self.callee(t))
            self.refs.add(name)
            out.append("c->x[16] = 0x%xULL; c->x[17] = 0x%xULL; TAILCALL(%s);\n}" % (slot, t, name))
            return "\n".join(out) + "\n", self.refs
        pcs = sorted(body)
        dead = {} if os.environ.get("A64_KEEP_FLAGS") else self.dead_flags(body, pcs, texts)
        frame = not os.environ.get("A64_MARK_FRAME")
        local = local_regs() and e not in self.no_local
        dirty = self.dirty_regs(body, pcs, texts) if local else None
        # (fast float regions, ff_region: the run of joinable instructions so far, [(pc, text)])
        ff_only = getattr(self, "ff_only", None)
        ff = bool(fastfp_mode()) and (ff_only is None or self.fname(e) in ff_only)
        run = []

        def flush():
            if run:
                text, n = ff_region(run)
                out.append(text)
                if n:
                    self.stats["ff_regions"] = self.stats.get("ff_regions", 0) + 1
                    self.stats["ff_ops"] = self.stats.get("ff_ops", 0) + n
                del run[:]
        for i, pc in enumerate(pcs):
            self.cur_i = i
            (k, w) = body[pc]
            kind = k[0]
            joins = False
            if ff and kind == "op" and pc not in self.after and not (pc in labels or (pc == e and e in labels)):
                t, f = texts[(w, pc) if pc_relative(w) else w]
                if not f & (a64emit.UNHANDLED | a64emit.TERMINATOR):
                    joins = ff_eligible(drop_flag_stores(t, dead[pc]) if pc in dead else t)
            if not joins:
                flush()
            if pc in labels or (pc == e and e in labels):
                out.append("L_%08x:;" % pc)
            if self.comments:
                (run.append((pc, "/* %08x: %08x */" % (pc, w))) if joins else
                 out.append("/* %08x: %08x */" % (pc, w)))
            nxt = pc + 4
            falls = True
            if kind == "op":
                text, flags = texts[(w, pc) if pc_relative(w) else w]
                if flags & a64emit.UNHANDLED:
                    self.unhandled[w] += 1
                    out.append(SYNC_ALL % i + "a64_trap(c, 0x%xULL, 0x%08xu); return;" % (pc, w))
                    falls = False
                elif flags & a64emit.TERMINATOR:
                    raise SystemExit("a64emit closed a block at %#x (%08x): classify() must handle it" % (pc, w))
                else:
                    if frame and not fp_is_frame(text):
                        frame = False
                    if pc in dead:
                        text = drop_flag_stores(text, dead[pc])
                        self.stats["dead_flag_stores"] += bin(dead[pc]).count("1")
                    if joins:
                        run.append((pc, text.rstrip("\n")))
                    else:
                        out.append(text.rstrip("\n"))
                    if pc in self.after:
                        out.append(self.after[pc])
                        self.stats["after_patches"] = self.stats.get("after_patches", 0) + 1
            elif kind == "b":
                out.append(self.transfer(pc, k[1], labels, body))
                falls = False
            elif kind == "bcond":
                out.append("if %s { %s }" % (COND[k[2]], self.transfer(pc, k[1], labels, body)))
            elif kind == "cbz":
                _, t, rt, sf, nz = k
                v = "0" if rt == 31 else ("c->x[%d]" % rt if sf else "(uint32_t)c->x[%d]" % rt)
                out.append("if (%s %s 0) { %s }" % (v, "!=" if nz else "==", self.transfer(pc, t, labels, body)))
            elif kind == "tbz":
                _, t, rt, bit, nz = k
                v = "0" if rt == 31 else "((c->x[%d] >> %d) & 1)" % (rt, bit)
                out.append("if (%s%s) { %s }" % ("" if nz else "!", v, self.transfer(pc, t, labels, body)))
            elif kind == "bl":
                t = self.callee(k[1])
                if t in self.entries and t in getattr(self, "setjmp_owner", {}):
                    # (hookshot) a call of a function that calls __setjmp: this frame takes a host setjmp. A guest
                    # longjmp's BR to one of the owner's setjmp return addresses comes back here (host/hos_setjmp.c),
                    # with the registers it restored, and runs the owner's code from there, as the owner would.
                    # (recomp_sj_setjmp, not __builtin_setjmp: on Win64 clang's frame pointer sits above the frame
                    # address the builtin saves, and its longjmp restored rbp to that, so the frame's spill slots
                    # were read 128 bytes off: c and _sj came back as garbage)
                    rets = (self.setjmp_owner[t] + [0, 0, 0, 0])[:4]
                    out.append("c->x[30] = 0x%xULL; %s{ void **_sj = recomp_sj_push(c, 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL); "
                               "if (recomp_sj_setjmp(_sj) == 0) %s(c); else a64_icall(c, recomp_sj_target(_sj)); "
                               "recomp_sj_pop(_sj); }%s"
                               % (nxt, SYNC_CALL % i, rets[0], rets[1], rets[2], rets[3], self.fname(t), SYNC_BACK % i))
                    self.refs.add(self.fname(t))
                    self.stats["setjmp_call_sites"] = self.stats.get("setjmp_call_sites", 0) + 1
                elif t in self.entries:
                    out.append("c->x[30] = 0x%xULL; %s%s(c);%s" % (nxt, SYNC_CALL % i, self.fname(t), SYNC_BACK % i))
                    self.refs.add(self.fname(t))
                else:
                    self.stats["bad_target"] += 1
                    out.append(SYNC_ALL % i + "a64_trap(c, 0x%xULL, 0x%08xu); return;" % (pc, w))
            elif kind == "blr":
                usual = self.devirt.get(pc)
                if usual in self.entries:
                    name = self.fname(self.callee(usual))
                    self.refs.add(name)
                    self.stats["devirt_sites"] += 1
                    out.append("{ uint64_t _t = c->x[%d]; c->x[30] = 0x%xULL; %sif (RECOMP_LIKELY(_t == 0x%xULL)) %s(c); "
                               "else A64_ICALL(c, _t); %s}" % (k[1], nxt, SYNC_CALL % i, usual, name, SYNC_BACK % i))
                else:
                    out.append("{ uint64_t _t = c->x[%d]; c->x[30] = 0x%xULL; %sA64_ICALL(c, _t); %s}"
                               % (k[1], nxt, SYNC_CALL % i, SYNC_BACK % i))
            elif kind == "ret":
                out.append(SYNC_RET % i + "return;")
                falls = False
            elif kind == "br":
                targets = self.jt.get(pc)
                if targets:
                    cases = " ".join("case 0x%xULL: %s" % (t, self.transfer(pc, t, labels, body))
                                     for t in sorted(set(targets)))
                    out.append("switch (c->x[%d]) { %s default: %sA64_IJUMP(c, c->x[%d]); return; }"
                               % (k[1], cases, SYNC_ALL % i, k[1]))
                else:
                    self.ijump[pc] += 1
                    out.append(SYNC_ALL % i + "A64_IJUMP(c, c->x[%d]); return;" % k[1])
                falls = False
            elif kind == "svc":
                out.append(SYNC_ALL % i + "a64_svc(c, %du);" % k[1] + SYNC_RELOAD % i)
            elif kind == "trap":
                out.append(SYNC_ALL % i + "a64_trap(c, 0x%xULL, 0x%08xu); return;" % (pc, w))
                falls = False
            elif kind == "isb":
                out.append("recomp_barrier();")
            elif kind == "ic":
                out.append("/* ic ivau: no self-modifying code */")
            if falls and (i + 1 == len(pcs) or pcs[i + 1] != nxt):
                flush()
                out.append(self.transfer(pc, nxt, labels, body))
        flush()
        out.append("}")
        text = "\n".join(out)
        if frame:
            text = frame_stores(text)
        return localize_regs(text, local, dirty) + "\n", self.refs

    def write(self, out_dir, chunk_bytes=CHUNK_BYTES):
        os.makedirs(out_dir, exist_ok=True)
        # translate every distinct instruction once (per pc only where the result depends on it)
        keys = {}
        for body, _ in self.funcs.values():
            for pc, (k, w) in body.items():
                if k[0] == "op":
                    keys.setdefault((w, pc) if pc_relative(w) else w, (pc, w))
        order = list(keys)
        tx, fl = a64emit.translate([keys[x] for x in order])
        fpx = set()
        texts = dict(zip(order, zip((op_text(t, fpx) for t in tx), fl)))
        del tx, fl, keys, order
        with open(os.path.join(out_dir, "a64_fpx.h"), "w", newline="\n") as f:
            f.write(fpx_header(fpx))
            if local_flags():
                f.write(FCMPK_HEADER)
            if fastfp_mode():
                f.write(FASTFP_HEADER)

        chunk, size, n, decls, files = [], 0, 0, set(), []

        def flush():
            nonlocal chunk, size, n, decls
            if not chunk:
                return
            path = os.path.join(out_dir, "code_%03d.c" % n)
            with open(path, "w", newline="\n") as f:
                f.write('#include "a64.h"\n')
                text = "".join(chunk)
                if "recomp_sj_setjmp(" in text:
                    f.write("__attribute__((returns_twice)) int recomp_sj_setjmp(void **slot);\n")
                f.write("".join("void %s(Ctx *A64_NOESC);\n" % d for d in sorted(decls)))
                f.write(text)
            files.append(path)
            chunk, size, n, decls = [], 0, n + 1, set()

        hot = hot_functions()
        self.ff_only = hot if fastfp_mode() == "hot" else None   # (A64_FASTFP=hot: the hot functions' regions only)
        # A64_HOT_CHUNK=1 (with A64_HOT_FUNCS): the hot functions in a file of their own, code_hot.c, so that the
        # compiler sees the hot callees' bodies where the hot callers are and can inline them (build.py compiles it
        # with RX_HOT_CFLAGS)
        hot_chunk = [] if hot and os.environ.get("A64_HOT_CHUNK") else None
        hot_decls = set()
        nhot = 0
        for e in sorted(self.funcs):
            text, refs = self.emit_function(e, texts)
            if hot and self.fname(e) in hot:
                text = hot_text(text)
                nhot += 1
                if hot_chunk is not None:
                    hot_chunk.append(text)
                    hot_decls.update(refs)
                    continue
            decls.update(refs)
            chunk.append(text)
            size += len(text)
            if size >= chunk_bytes:
                flush()
        flush()
        if hot_chunk:
            path = os.path.join(out_dir, "code_hot.c")
            with open(path, "w", newline="\n") as f:
                f.write('#include "a64.h"\n')
                text = "".join(hot_chunk)
                if "recomp_sj_setjmp(" in text:
                    f.write("__attribute__((returns_twice)) int recomp_sj_setjmp(void **slot);\n")
                f.write("".join("void %s(Ctx *A64_NOESC);\n" % d for d in sorted(hot_decls)))
                f.write(text)
            files.append(path)
        elif os.path.exists(os.path.join(out_dir, "code_hot.c")):
            os.remove(os.path.join(out_dir, "code_hot.c"))   # (a lift without the hot file: no stale copy linked twice)
        if fastfp_mode():
            print("fast float regions (A64_FASTFP=%s): %d, %d float ops in them" % (
                fastfp_mode(), self.stats.get("ff_regions", 0), self.stats.get("ff_ops", 0)))
        if hot:
            print("hot functions (A64_HOT_FUNCS): %d of %d listed found%s" % (nhot, len(hot),
                                                                         ", in code_hot.c" if hot_chunk else ""))
        self._write_table(out_dir)
        with open(os.path.join(out_dir, "functions.txt"), "w") as f:
            for e in sorted(self.funcs):
                f.write("%08x %d\n" % (e, len(self.funcs[e][0])))
        return files

    def _write_table(self, out_dir):
        fns = sorted(set(self.funcs) | set(self.hooks))
        with open(os.path.join(out_dir, "a64_table.c"), "w", newline="\n") as f:
            f.write('#include "a64.h"\n')
            f.write("".join("void %s(Ctx *A64_NOESC);\n" % self.fname(a) for a in fns))
            f.write("const uint32_t a64_fn_count = %d;\n" % len(fns))
            f.write("const uint32_t a64_fn_addr[] = {\n%s};\n" % "".join("0x%08x,\n" % a for a in fns))
            f.write("const A64Fn a64_fn_ptr[] = {\n%s};\n" % "".join("%s,\n" % self.fname(a) for a in fns))
        with open(os.path.join(out_dir, "a64_hooks.c"), "w", newline="\n") as f:
            f.write("/* Hooked guest addresses: f_<address> is the host function (an HLE or a generated stub). */\n")
            f.write('#include "a64.h"\n')
            f.write("".join("void %s(Ctx *A64_NOESC);\n" % n for n in sorted(set(self.hooks.values()))))
            f.write("".join("void f_%08x(Ctx *A64_NOESC c) { TAILCALL(%s); }\n" % (a, n) for a, n in sorted(self.hooks.items())))

    def report(self):
        nins = sum(len(b) for b, _ in self.funcs.values())
        br = sum(1 for b, _ in self.funcs.values() for k, _ in b.values() if k[0] == "br")
        print("functions %d, instructions %d (with shared tails), jump tables %d, indirect BR %d of %d, "
              "PLT stubs %d, unhandled words %d (%d sites), bad targets %d" % (
                  len(self.funcs), nins, len(self.jt), len(self.ijump), br, len(self.plt),
                  len(self.unhandled), sum(self.unhandled.values()), self.stats["bad_target"]))
        print("jump tables: %d bounded, %d bounded by the function, %d rejected (target outside), %d table not found" % (
            self.stats["jt_bounded"], self.stats["jt_unbounded"], self.stats["jt_out_of_function"],
            self.stats["jt_no_table"]))
        if self.stats["jt_unrecognized"]:
            print("table-shaped BRs whose load was not recognized: %d" % self.stats["jt_unrecognized"])
        if self.stats["jt_multi_table"]:
            print("jump tables with several candidate table addresses: %d" % self.stats["jt_multi_table"])
        print("br to a code constant: %d; br with adr candidates: %d" % (self.stats["br_constant"],
                                                                         self.stats["br_adr_candidates"]))
        if self.stats["split_entries"]:
            print("split: %d extra entries in long functions; largest function now %d instructions" % (
                self.stats["split_entries"], max(len(b) for b, _ in self.funcs.values())))
        if self.stats["stores"]:   # (group_marks, when the lift ran it before this report)
            print("page marks: %d groups share one mark for %d of %d stores through a base register" % (
                self.stats["groups"], self.stats["grouped_stores"], self.stats["stores"]))
