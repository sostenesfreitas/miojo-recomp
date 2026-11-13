"""Whole recompiled functions against Unicorn: random functions of the game's rtld, main, sdk and subsdk*, lifted exactly
as lift.py lifts them, run natively and in Unicorn (ARMv8.x "max" CPU) on the same relocated image.

Two kinds of functions:
  - default: self-contained ones, whose lifted C calls nothing: no BL/BLR, SVC, tail call into another entry,
    indirect BR, trap or untranslated word, no CNTPCT read (the clock differs), 3..600 instructions;
  - --calls: functions with callees. A root qualifies when everything it reaches through BL, tail calls (B or
    fallthrough into another entry) and PLT stubs is liftable the same way (no BLR, SVC, indirect BR, trap, CNTPCT),
    at most --max-tree instructions and 64 functions in all. The whole call tree is lifted and runs natively, so this
    tests calls, returns through LR, tail calls, PLT stubs bound across modules and register state across calls.

The image is laid out as Eglantine loads it (image.py, 0x80000000) and, by default, the scratch area lies above
4 GB (0x10_0000_0000) with the lifted code on the 64-bit guest window (rtgen.py): a 32-bit window would truncate
every scratch pointer. --addr32 runs a64-kit's own 32-bit model instead, with the scratch area below 4 GB.

Each function runs from several random states: X0-X7 a mix of pointers into the scratch area, small integers and
random 64-bit values; the scratch area random qwords of which a quarter are pointers back into it (so pointer
chasing goes a few levels deep); the stack and TPIDR(RO)_EL0 in it too; FPCR 0 or FZ.

Compared after the return: X0-X30, SP, V0-V31, FPSR (cumulative bits, reported apart), and every 4 KB page of the
image's .data/.bss and of the scratch area that ends up different (page set and contents). NZCV is not compared:
flags are dead at a return (the AAPCS64), and the lifter drops flag stores no later instruction reads. With --calls,
X16/X17 are not compared either (counted apart): a call through a PLT stub calls the bound target directly, without
the stub's `adrp x16 / ldr x17` (a64-kit's PLT model); they are the AAPCS64's intra-procedure-call scratch registers,
which any veneer or stub may clobber, so no caller can depend on them after a call. Cases where
Unicorn faults, does not return within 400,000 instructions, or touches an unmapped address are not comparable and
are skipped.

Usage:  python -I nx2/test_leaf.py [--count 3000] [--states 4] [--seed 1] [--calls] [--max-tree 4000]
                                   [--addr32] [--list]
"""
import argparse
import collections
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit
import nso2  # noqa: F401
import insn
import lift_fixes
import suyu_rt_fixes
import poll
import image as eden_image
import rtgen
import a64emit
import a64_lift
import a64_rtgen
import toolchain
import numpy as np
import unicorn
from unicorn import arm64_const as A

# (the game: this runs the lifted code of a real dump against Unicorn)
RUN_ALL_SKIP = {
    "linux": "needs the game's modules (work/dump) and Unicorn",
    "windows": "needs the game's modules (work/dump) and Unicorn",
}

insn.install()
lift_fixes.install()
suyu_rt_fixes.install()
poll.install()

SCRATCH_SIZE = 0x100000
SENTINEL = 0x7F000000      # LR of the outermost call: a mapped page with no code, below the image
PAGE = 0x1000
STATE = struct.Struct("<32Q4B4xQQ64QQQ")   # x0-x30, sp | nzcv | fpcr fpsr | v0-v31 (lo, hi) | tpidr tpidrro
XREG = [getattr(A, "UC_ARM64_REG_X%d" % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
QREG = [getattr(A, "UC_ARM64_REG_Q%d" % i) for i in range(32)]
M64 = (1 << 64) - 1
BAD_TEXT = ("a64_trap(", "A64_IJUMP", "A64_ICALL", "a64_icall", "a64_ijump", "recomp_cntpct", "recomp_unhandled",
            "a64_svc(")
REF = re.compile(r"\bf_([0-9a-f]{8})\(")
SCRATCH = 0x1000000000


def scratch_words(seed):
    """the scratch area of one case (splitmix64; harness.c makes the same)"""
    i = np.arange(SCRATCH_SIZE // 8, dtype=np.uint64)
    with np.errstate(over="ignore"):
        z = np.uint64(seed) + (i + np.uint64(1)) * np.uint64(0x9E3779B97F4A7C15)
        z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        z = z ^ (z >> np.uint64(31))
    ptr = np.uint64(SCRATCH) + ((z >> np.uint64(12)) % np.uint64(SCRATCH_SIZE - 256)) & ~np.uint64(7)
    return np.where((z & np.uint64(3)) == 0, ptr, z)


def page_hash(b):
    """hash of one 4 KB page (harness.c: the same)"""
    w = np.frombuffer(b, dtype=np.uint64)
    i = np.arange(len(w), dtype=np.uint64)
    with np.errstate(over="ignore"):
        return int((w * (i * np.uint64(2) + np.uint64(1))).sum() ^ (w ^ (i * np.uint64(0x9E3779B97F4A7C15))).sum())


class Picker:
    """Walks, translates and emits functions as lift.py does, keeping what it has done for reuse across roots."""

    def __init__(self, img):
        self.img = img
        self.L = a64_lift.Lifter(img)
        self.texts = {}
        self.emitted = {}     # entry -> (text, callee entries) or None (not liftable here)
        self.fpx = set()

    def liftable_body(self, e, calls):
        L = self.L
        if e not in L.funcs:
            L.funcs[e] = L.walk(e)
            t = L.plt_target(e)
            if t is not None:
                L.plt[e] = t
        body = L.funcs[e][0]
        kinds = {k[0] for k, _ in body.values()}
        bad = {"blr", "svc", "trap"} | (set() if calls else {"bl"})
        if not body or kinds & bad:
            return False
        if e not in L.plt and any(k[0] == "br" and pc not in L.jt for pc, (k, _) in body.items()):
            return False
        return True

    def emit(self, entries):
        """translate the words of these functions (one batch) and emit them"""
        L = self.L
        keys = {}
        for e in entries:
            for pc, (k, w) in L.funcs[e][0].items():
                if k[0] == "op":
                    key = (w, pc) if a64_lift.pc_relative(w) else w
                    if key not in self.texts:
                        keys.setdefault(key, (pc, w))
        if keys:
            order = list(keys)
            tx, fl = a64emit.translate([keys[x] for x in order])
            T = lambda t: a64_lift.stack_stores(a64_lift.pack_flags(a64_lift.widen_vstores(a64_lift.vectorize_fpx(
                a64_lift.outline_fpx(t, self.fpx)))))
            self.texts.update(zip(order, zip((T(t) for t in tx), fl)))
        for e in entries:
            text, refs = L.emit_function(e, self.texts)
            if any(b in text for b in BAD_TEXT):
                self.emitted[e] = None
            else:
                self.emitted[e] = (text, {int(r[2:], 16) for r in refs})

    def tree(self, root, calls, max_insns, max_funcs=64):
        """the root's call tree (entries), or None if some function in it is not liftable here"""
        seen, todo = set(), [root]
        while todo:
            fresh = []
            for e in todo:
                if e in seen:
                    continue
                seen.add(e)
                if len(seen) > max_funcs or not self.liftable_body(e, calls):
                    return None
                if e not in self.emitted:
                    fresh.append(e)
            if sum(len(self.L.funcs[e][0]) for e in seen) > max_insns:
                return None
            if fresh:
                self.emit(fresh)
            nxt = []
            for e in todo:
                got = self.emitted.get(e)
                if got is None:
                    return None
                nxt += [r for r in got[1] if r not in seen]
            todo = nxt
        if not calls and self.emitted[root][1]:
            return None
        return seen


def module_shares(img):
    """how many of the drawn functions come from each module: main 55 %, sdk 30 %, the subsdk* modules the rest by
    code size, rtld 2 % (mkw-recomp's split, for whatever modules this exefs has)"""
    size = {n: hi - lo for lo, hi, n in img.code}
    share = {"main": 0.55, "sdk": 0.30, "rtld": 0.02}
    subs = [n for n in size if n.startswith("subsdk")]
    rest = 1.0 - sum(v for k, v in share.items() if k in size)
    for n in subs:
        share[n] = rest * size[n] / max(1, sum(size[s] for s in subs))
    return {k: v for k, v in share.items() if k in size}


def select(P, counts, rng, calls, max_tree):
    img = P.img
    by_mod = collections.defaultdict(list)
    for e in sorted(img.entries):
        by_mod[next(n for lo, hi, n in img.code if lo <= e < hi)].append(e)
    roots, funcs = [], set()
    for mod, want in counts.items():
        cands = by_mod[mod]
        rng.shuffle(cands)
        got = 0
        for e in cands[:want * 40]:
            if got >= want:
                break
            if not calls:
                if e not in P.L.funcs:
                    P.L.funcs[e] = P.L.walk(e)
                if not 3 <= len(P.L.funcs[e][0]) <= 600:
                    continue
            t = P.tree(e, calls, max_tree if calls else 600)
            if t is None or (calls and len(t) < 2):
                continue      # (--calls: only roots that do call something)
            roots.append((mod, e, len(t)))
            funcs |= t
            got += 1
    return roots, funcs


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
static const A64Fn T[] = {%(table)s};
void a64_trap(Ctx *c, uint64_t pc, uint32_t insn) { (void)pc; (void)insn; c->halted = 9; }
void a64_ret_mismatch(Ctx *c, uint64_t pc) { (void)pc; c->halted = 10; }
void recomp_unhandled(GuestContext *c, uint32_t i, uint64_t pc) { (void)i; (void)pc; c->halted = 7; }
void recomp_barrier(void) {}
void recomp_ic_ivau(GuestContext *c, uint64_t a, int v) { (void)c; (void)a; (void)v; }
uint64_t recomp_cntpct(GuestContext *c) { (void)c; return 0; }
uint64_t recomp_ldxr(GuestContext *c, uint64_t a, uint32_t s) { uint64_t v = 0; memcpy(&v, A64_PTR(a), s); c->excl_addr = a; c->excl_valid = 1; return v; }
uint32_t recomp_stxr(GuestContext *c, uint64_t a, uint32_t s, uint64_t v) { if (!c->excl_valid || c->excl_addr != a) return 1; c->excl_valid = 0; memcpy(A64_PTR(a), &v, s); return 0; }
void recomp_ldxp(GuestContext *c, uint64_t a, uint32_t s, uint64_t *lo, uint64_t *hi) { *lo = *hi = 0; memcpy(lo, A64_PTR(a), s); memcpy(hi, A64_PTR(a + s), s); c->excl_addr = a; c->excl_valid = 1; }
uint32_t recomp_stxp(GuestContext *c, uint64_t a, uint32_t s, uint64_t lo, uint64_t hi) { if (!c->excl_valid || c->excl_addr != a) return 1; c->excl_valid = 0; memcpy(A64_PTR(a), &lo, s); memcpy(A64_PTR(a + s), &hi, s); return 0; }
void recomp_clrex(GuestContext *c) { c->excl_valid = 0; }
/* loop-header polls (nx2/poll.py) fire on two of every three loop iterations: they must change nothing. The poll is
   called with no argument (rtgen.py): the context under test is the one run() was given */
static uint64_t polls;
static Ctx *poll_ctx;
void a64_poll_cur(void) { polls++; poll_ctx->poll = (polls %% 3) != 0; }
void a64_poll(Ctx *c) { c->poll = 0; a64_poll_cur(); }

#define IMG_BASE %(img_base)dULL
#define IMG_SIZE %(img_size)dULL
#define SCR_BASE %(scratch_base)dULL
#define SCR_SIZE %(scratch)dULL
static const uint64_t regions[][2] = {%(regions)s};
#define NREG (sizeof regions / sizeof regions[0])
static uint8_t *p_img, *p_scr;
static uint8_t *pristine(uint64_t a) { return a >= SCR_BASE ? p_scr + (a - SCR_BASE) : p_img + (a - IMG_BASE); }
static uint64_t page_hash(const uint8_t *p) {
    const uint64_t *w = (const uint64_t *)p; uint64_t a = 0, b = 0;
    for (uint64_t i = 0; i < 512; i++) { a += w[i] * (2 * i + 1); b += w[i] ^ (i * 0x9E3779B97F4A7C15ULL); }
    return a ^ b;
}
static void make_scratch(uint64_t seed, uint64_t *out) {
    for (uint64_t i = 0; i < SCR_SIZE / 8; i++) {
        uint64_t z = seed + (i + 1) * 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; z ^= z >> 31;
        out[i] = (z & 3) == 0 ? ((SCR_BASE + (z >> 12) %% (SCR_SIZE - 256)) & ~7ULL) : z;
    }
}
static int run(int idx, Ctx *c) {
    poll_ctx = c;
    __try { T[idx](c); } __except (EXCEPTION_EXECUTE_HANDLER) { return (int)GetExceptionCode(); }
    return 0;
}
int main(int argc, char **argv) {
    FILE *img = fopen(argv[1], "rb"), *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    if (VirtualAlloc(A64_PTR(IMG_BASE), IMG_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != A64_PTR(IMG_BASE)) return 3;
    if (VirtualAlloc(A64_PTR(SCR_BASE), SCR_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != A64_PTR(SCR_BASE)) return 4;
    if (fread(A64_PTR(IMG_BASE), 1, IMG_SIZE, img) != IMG_SIZE) return 5;
    p_img = malloc(IMG_SIZE); p_scr = malloc(SCR_SIZE);
    memcpy(p_img, A64_PTR(IMG_BASE), IMG_SIZE);
    static uint8_t state[%(state)d];
    unsigned idx; uint64_t seed;
    while (fread(&idx, 4, 1, in) == 1 && fread(&seed, 8, 1, in) == 1 && fread(state, sizeof state, 1, in) == 1) {
        make_scratch(seed, (uint64_t *)p_scr);
        memcpy(A64_PTR(SCR_BASE), p_scr, SCR_SIZE);
        Ctx c; memset(&c, 0, sizeof c);
        memcpy(c.x, state, 256); memcpy(&c.n, state + 256, 4);
        memcpy(&c.fpcr, state + 264, 8); memcpy(&c.fpsr, state + 272, 8); memcpy(c.vreg, state + 280, 512);
        memcpy(&c.tpidr_el0, state + 792, 8); memcpy(&c.tpidrro_el0, state + 800, 8);
        c.poll = 1;
        int fault = run(idx, &c);
        memcpy(state, c.x, 256); memcpy(state + 256, &c.n, 4);
        memcpy(state + 264, &c.fpcr, 8); memcpy(state + 272, &c.fpsr, 8); memcpy(state + 280, c.vreg, 512);
        fwrite(&c.halted, 4, 1, out); fwrite(&fault, 4, 1, out); fwrite(state, sizeof state, 1, out);
        uint32_t ndiff = 0; static uint64_t diff[2 * 8192];
        for (size_t r = 0; r < NREG; r++)
            for (uint64_t a = regions[r][0]; a < regions[r][1]; a += 4096)
                if (memcmp(A64_PTR(a), pristine(a), 4096)) {
                    if (ndiff < 8192) { diff[2 * ndiff] = a; diff[2 * ndiff + 1] = page_hash(A64_PTR(a)); }
                    ndiff++;
                    memcpy(A64_PTR(a), pristine(a), 4096);
                }
        fwrite(&ndiff, 4, 1, out); fwrite(diff, 16, ndiff < 8192 ? ndiff : 8192, out);
    }
    fclose(out);
    fprintf(stderr, "loop-header polls taken: %%llu\n", (unsigned long long)polls);
    return 0;
}
"""


def writable_regions(meta):
    regs = []
    for m in meta["modules"]:
        data = next(s for s in m["segments"] if s["name"] == ".data")
        lo = data["addr"] & ~(PAGE - 1)
        hi = (max(m["bss"][1], data["addr"] + data["size"]) + PAGE - 1) & ~(PAGE - 1)
        regs.append((lo, hi))
    regs.append((SCRATCH, SCRATCH + SCRATCH_SIZE))
    return regs


def build(tmp, P, roots, funcs, meta, img_size, addr64):
    a64_rtgen.write(tmp)
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(a64_lift.fpx_header(P.fpx))
    rt = os.path.join(kit.KIT, "rt")
    if addr64:
        rt = rtgen.write(os.path.join(tmp, "rt64"))   # (its a64.h shadows the vendored one)
    src = ['#include "a64.h"'] + ["void f_%08x(Ctx *A64_NOESC);" % e for e in sorted(funcs)]
    body = "\n".join(P.emitted[e][0] for e in sorted(funcs))
    src.append(body if os.environ.get("A64_MARK_EACH") else a64_lift.mark_groups(body))   # (nx2/lift.py's last step)
    src.append(HARNESS % {
        "table": ",".join("f_%08x" % e for _, e, _ in roots),
        "regions": ",".join("{%#xULL,%#xULL}" % r for r in writable_regions(meta)),
        "scratch": SCRATCH_SIZE, "scratch_base": SCRATCH, "img_base": meta["base"],
        "img_size": img_size, "state": STATE.size})
    path = os.path.join(tmp, "harness.c")
    open(path, "w", newline="\n").write("\n".join(src))
    exe = os.path.join(tmp, "harness.exe")
    cmd = [toolchain.need(toolchain.find("clang"))] + toolchain.native_target() + toolchain.native_link() + \
          ["-O1", "-g0", "-mcx16", "-Wno-everything"] + (["-DA64_POLLING"] if addr64 else []) + ["-I" + rt, "-I" + tmp, path,
           os.path.join(tmp, "a64_suyu.c"), "-o", exe]
    subprocess.run(cmd, check=True)
    return exe


def rand_state(rng):
    x = []
    for i in range(31):
        r = rng.random()
        if i < 8 and r < 0.45:
            x.append(SCRATCH + 0x1000 + 8 * rng.randrange((SCRATCH_SIZE - 0x22000) // 8))
        elif r < 0.7:
            x.append(rng.choice([0, 1, 2, 3, 4, 7, 8, 16, 31, 32, 64, 100, 255, 0xFFFFFFFF, M64, rng.randrange(1024)]))
        else:
            x.append(rng.getrandbits(64))
    x[30] = SENTINEL
    x.append(SCRATCH + SCRATCH_SIZE - 0x10000 - 16 * rng.randrange(256))   # sp
    v = []
    for _ in range(32):
        if rng.random() < 0.5:
            v += [rng.getrandbits(64), rng.getrandbits(64)]
        else:
            lanes = [struct.unpack("<I", struct.pack("<f", rng.uniform(-100, 100)))[0] for _ in range(2)]
            v += [lanes[0] | lanes[1] << 32, struct.unpack("<Q", struct.pack("<d", rng.uniform(-1e3, 1e3)))[0]]
    fpcr = 0 if rng.random() < 0.75 else 1 << 24
    return x, [0, 0, 0, 0], fpcr, 0, v, SCRATCH + 0x800, SCRATCH + 0x400


class Emu:
    def __init__(self, image, meta, regions):
        uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
        base, size = meta["base"], (len(image) + PAGE - 1) & ~(PAGE - 1)
        uc.mem_map(base, size)
        uc.mem_write(base, bytes(image))
        uc.mem_map(SCRATCH, SCRATCH_SIZE)
        uc.mem_map(SENTINEL, PAGE)
        self.uc, self.dirty, self.regions = uc, set(), regions
        self.pristine = {}
        for lo, hi in regions[:-1]:
            for pg in range(lo, hi, PAGE):
                self.pristine[pg] = bytes(uc.mem_read(pg, PAGE))
        uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, self._write)

    def _write(self, uc, access, addr, size, value, data):
        self.dirty.add(addr & ~(PAGE - 1))
        self.dirty.add((addr + size - 1) & ~(PAGE - 1))

    def run(self, entry, st, scratch):
        uc = self.uc
        x, nzcv, fpcr, fpsr, v, tpidr, tpidrro = st
        uc.mem_write(SCRATCH, scratch)
        self.dirty = set()
        for i in range(31):
            uc.reg_write(XREG[i], x[i])
        uc.reg_write(A.UC_ARM64_REG_SP, x[31])
        uc.reg_write(A.UC_ARM64_REG_NZCV, 0)
        uc.reg_write(A.UC_ARM64_REG_FPCR, fpcr)
        uc.reg_write(A.UC_ARM64_REG_FPSR, fpsr)
        uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, tpidr)
        uc.reg_write(A.UC_ARM64_REG_TPIDRRO_EL0, tpidrro)
        for i in range(32):
            uc.reg_write(QREG[i], v[2 * i] | v[2 * i + 1] << 64)
        try:
            uc.emu_start(entry, SENTINEL, count=400000)
            ok = uc.reg_read(A.UC_ARM64_REG_PC) == SENTINEL
        except unicorn.UcError:
            ok = False
        res = None
        if ok:
            ox = [uc.reg_read(XREG[i]) for i in range(31)] + [uc.reg_read(A.UC_ARM64_REG_SP)]
            ov = []
            for i in range(32):
                q = uc.reg_read(QREG[i])
                ov += [q & M64, q >> 64]
            pages = {}
            for a in self.dirty:
                before = scratch[a - SCRATCH:a - SCRATCH + PAGE] if SCRATCH <= a < SCRATCH + SCRATCH_SIZE \
                    else self.pristine.get(a)
                now = bytes(uc.mem_read(a, PAGE))
                if before is not None and now != before:
                    pages[a] = page_hash(now)
            res = (ox, ov, uc.reg_read(A.UC_ARM64_REG_FPSR), pages)
        for a in self.dirty:          # the image pages put back for the next case
            if a in self.pristine:
                uc.mem_write(a, self.pristine[a])
        return res


def main():
    kit.wait_for_quiet('the function test')
    global SCRATCH
    p = argparse.ArgumentParser()
    p.add_argument("--count", type=int, default=3000)
    p.add_argument("--states", type=int, default=4)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--calls", action="store_true")
    p.add_argument("--max-tree", type=int, default=4000)
    p.add_argument("--addr32", action="store_true")
    p.add_argument("--list", action="store_true")
    a = p.parse_args()
    if a.addr32:
        SCRATCH = 0x0E000000
    rng = random.Random(a.seed)
    t0 = time.time()
    img = eden_image.EdenImage(kit.need_exefs())
    meta = img.write(os.path.join(kit.WORK, "image_test"))
    P = Picker(img)
    share = module_shares(img)
    roots, funcs = select(P, {m: max(1, int(a.count * s)) for m, s in share.items()}, rng, a.calls, a.max_tree)
    print("%d %s, %d functions lifted in all, %d instructions (%.0fs)" % (
        len(roots), "call trees" if a.calls else "self-contained functions", len(funcs),
        sum(len(P.L.funcs[e][0]) for e in funcs), time.time() - t0), flush=True)
    tmp = tempfile.mkdtemp(prefix="oot_leaf_")
    exe = build(tmp, P, roots, funcs, meta, len(img.mem), not a.addr32)
    print("harness built, %s guest window, scratch at %#x (%.0fs)" % ("32-bit" if a.addr32 else "64-bit", SCRATCH,
                                                                     time.time() - t0), flush=True)
    emu = Emu(img.mem, meta, writable_regions(meta))
    cases, expect, skipped = [], [], 0
    for i, (mod, e, _) in enumerate(roots):
        for _ in range(a.states):
            st = rand_state(rng)
            seed = rng.getrandbits(64)
            res = emu.run(e, st, scratch_words(seed).tobytes())
            if res is None:
                skipped += 1
                continue
            cases.append((i, seed, st))
            expect.append(res)
    print("%d comparable cases, %d skipped (Unicorn fault or no return) (%.0fs)" % (
        len(cases), skipped, time.time() - t0), flush=True)
    img_path = os.path.join(kit.WORK, "image_test", "image.bin")
    inp, outp = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    with open(inp, "wb") as f:
        for i, seed, st in cases:
            x, nzcv, fpcr, fpsr, v, tpidr, tpidrro = st
            f.write(struct.pack("<IQ", i, seed) + STATE.pack(*(x + nzcv + [fpcr, fpsr] + v + [tpidr, tpidrro])))
    r = subprocess.run([exe, img_path, inp, outp], timeout=900)
    if r.returncode:
        raise SystemExit("harness exited with %d" % r.returncode)
    fn_fail, fn_pass, fpsr_only = collections.defaultdict(list), collections.Counter(), collections.Counter()
    ip_only = collections.Counter()
    tested = set()
    with open(outp, "rb") as f:
        for (i, seed, st), (ex_x, ex_v, ex_fpsr, ex_pages) in zip(cases, expect):
            halted, fault = struct.unpack("<ii", f.read(8))
            vals = STATE.unpack(f.read(STATE.size))
            ndiff = struct.unpack("<I", f.read(4))[0]
            raw = f.read(16 * min(ndiff, 8192))
            pages = {struct.unpack_from("<Q", raw, 16 * k)[0]: struct.unpack_from("<Q", raw, 16 * k + 8)[0]
                     for k in range(min(ndiff, 8192))}
            mod, e, _ = roots[i]
            tested.add(i)
            d = []
            if halted or fault:
                d.append("halted %d fault %#x" % (halted, fault & 0xFFFFFFFF))
            else:
                ip = a.calls and (vals[16], vals[17]) != (ex_x[16], ex_x[17])
                ip_only[i] += ip
                d += ["%s %#x != %#x" % ("sp" if r == 31 else "x%d" % r, vals[r], ex_x[r]) for r in range(32)
                      if vals[r] != ex_x[r] and not (a.calls and r in (16, 17))]
                d += ["v%d.%s %#x != %#x" % (r // 2, "hi" if r & 1 else "lo", vals[38 + r], ex_v[r]) for r in range(64)
                      if vals[38 + r] != ex_v[r]]
                if pages != ex_pages:
                    d.append("memory: pages %s" % sorted("%x" % p for p in set(pages) ^ set(ex_pages)
                                                         or [p for p in pages if pages[p] != ex_pages.get(p)])[:4])
            if d:
                fn_fail[i].append(d[:4])
            else:
                fn_pass[i] += 1
                if (vals[37] & 0x9F) != (ex_fpsr & 0x9F):
                    fpsr_only[i] += 1
    bad = sorted(fn_fail)
    per = collections.Counter(roots[i][0] for i in tested)
    sizes = [roots[i][2] for i in tested]
    print("%d %s run (%s)%s, %d cases: %d pass every case, %d with differences; FPSR cumulative bits differ in %d" % (
        len(tested), "call trees" if a.calls else "functions", ", ".join("%s %d" % kv for kv in per.most_common()),
        " (%d functions per tree on average, up to %d)" % (sum(sizes) // max(1, len(sizes)), max(sizes or [0]))
        if a.calls else "", len(cases), len(tested) - len(bad), len(bad), len(fpsr_only)))
    if a.calls:
        print("X16/X17 (PLT scratch, not compared) differed in %d cases of %d trees" % (
            sum(ip_only.values()), sum(1 for v in ip_only.values() if v)))
    for i in bad if a.list else bad[:30]:
        mod, e, _ = roots[i]
        print("  FAIL f_%08x %-40s %d/%d: %s" % (e, (img.names.get(e) or "")[:40], len(fn_fail[i]),
                                                 len(fn_fail[i]) + fn_pass[i], "; ".join(fn_fail[i][0])))
    print("PASS" if not bad else "FAIL", "(%.0fs)" % (time.time() - t0))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
