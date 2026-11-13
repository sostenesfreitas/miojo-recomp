"""FPX1's sticky-flag rescue for single-precision FMUL/FNMUL/FMLA and its FMADD/FRECPS/FRSQRTS S fast paths,
game-free: bit-exact with the exact body.

a64_lift.py's a64_fpx_mul32_fma32 and a64_fpx_fma32_fma32 (the cold wrappers lifted code calls when the inline fast path
cannot prove its result) try a64_fpx_sticky32 before the exact body recomp_fpe_fma32: when every FPSR flag the Arm op
could raise is already set, one hardware FMA (or multiply) is the Arm result. This builds what the real build builds,
a64_fpx.h from a64_lift.fpx_header() and a64_suyu.h / a64_suyu.c from a64_rtgen.py with nx2/suyu_rt_fixes.py (through
suyu's translator, a64emit.cpp, built here at -O0 into a temporary folder), and compiles tests/fpx_sticky_test.c against
the real rt/a64.h at the build's FP flags (-ffp-contract=off -O2) twice: at -march=x86-64-v3 (RECOMP_FPX_FMA64: both
ops rescued) and at -march=x86-64-v2 (what host_march() gives a CPU without AVX2: FMUL's rescue is a plain multiply and
FMLA's must decline every case). The v2 build runs on any x86-64-v2 CPU, whether or not it has AVX2. In each build:
  - the lifted route: guest FMUL, FNMUL, FMADD and FMLA (by element) S words, hand-encoded, through a64emit.translate and
    a64_lift.op_text (the lifter's per-instruction pipeline, so the C calls the outlined wrapper as lifted code does) and
    hot_text (the hot functions' form), against the same words translated with FPX off (the exact body inline): the
    whole Ctx and the rescue count must match, on 5 inputs that must be rescued and 7 that must not (96 runs);
  - the inline fast paths of FMADD, FRECPS and FRSQRTS S (recomp_fpx_fma32, _recps32, _rsqrts32), 8M random cases
    (seeded, mostly with the gate open, a*b near 1, 2 or 3 as in the Newton steps): value and FPSR equal to the exact
    body's; and three binary32 midpoints, which the binary64 form (v2) declines and a fused FMA (v3, suyu_rt_fixes)
    keeps;
and each wrapper's value and FPSR must equal recomp_fpe_fma32's for:
  - 8M random operand triples (16M checks, seeded): zeros, subnormals, inf, NaN (quiet and signalling), the extreme
    normals, products near the underflow and overflow edges, exact and near cancellations, under random FPCR (FZ,
    DN, RMode, FIZ/AH/NEP alone or with other bits, the kill switch) and FPSR states;
  - an edge sweep (~150M checks): every a with biased exponent <= 4 or >= 250 whose fraction is below 2^12, above
    2^23 - 2^12 or a multiple of 97, against 12 fixed b and 4 addend kinds, each under FZ on and off with the flags
    all set or IXC only, and under one more state (by a): partial flags (UFC set and IDC clear, IDC set and UFC clear,
    OFC only, every bit but one of IDC/UFC/OFC, ...), IXC clear, or FIZ/AH/NEP set.
The rescues are counted inside the generated wrappers (an A64_FPX_STICKY_PROBE() the driver defines; empty in the real
build): both builds must see FMUL rescues, v3 FMLA rescues and v2 none, and a rescue under a closed gate (an FPCR
control bit but FZ set, or IXC clear) fails. That last check is what guards FIZ/AH/NEP: the exact body models none of
them, so comparing values could not tell. About 35-50 s here.

It has teeth (each by hand, then restored):
  - suyu_rt_fixes not installed: v3's fast run fails (exit 4, the midpoints fall back);
  - the emitter no longer outlining FMUL/FNMUL S (arm64_to_c.h's EmitFPMulAddValue: `FpxOutline()` made
    `FpxOutline() && !mul`, so the guest FMUL keeps suyu's inline guard): the lifted case fails in both builds (20
    FMUL/FNMUL runs with no rescue), while the random and sweep runs, which call the wrappers by name, still pass;
  - the wrappers' route to the rescue removed (fpx_header's `(fast, exact) in (("mul32", "fma32"), ("fma32", "fma32"))`
    made never true): every run fails with no rescue counted, though every value still matches;
  - FMUL's wrapper passing mul=0 (an FMA with the zero addend, the same value with a hardware FMA): v2 fails (exit 4);
  - FPCR bits 0-2 dropped from RECOMP_FPX_FPCR_MASK (0x06C8FF07 -> 0x06C8FF00): every value still matches, but 190k of
    v3's random checks and 6.1M of its sweep's are rescues under FIZ/AH/NEP (v2: 99k and 2.6M);
  - the helper's UFC test removed (a64_fpx_sticky32's `if (!(s & 8u))` made `if (0)`, so a tiny result is kept with
    UFC clear): 486k and 3.5M mismatch at v3 (FPSR without UFC), 258k and 475k at v2;
  - its IDC test dropped (a flushed subnormal operand with IDC clear): 383k and 10.0M at v3, 146k and 3.8M at v2.

    python -I -B tests/test_fpx_sticky.py [--cc clang] [--cxx clang++]

Skipped (exit 0) on a host that is not x86-64 or has no C/C++ compiler (PATH, $CC/$CXX, or the build's own emsdk /
HOOKSHOT_MINGW toolchain), or on a CPU without x86-64-v2; the v3 build alone is skipped on a CPU without x86-64-v3.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "third_party", "a64-kit")
sys.path.insert(0, os.path.join(KIT, "lift"))
sys.path.insert(0, os.path.join(KIT, "tools"))
sys.path.append(os.path.join(ROOT, "nx2"))
import toolchain  # noqa: E402

RANDOM_TRIPLES, SEED, SWEEP_STRIDE, FAST_CASES = 8000000, 20261109, 97, 8000000
FLAGS = ["-ffp-contract=off", "-fno-strict-aliasing", "-O2"]   # (nx2/build.py's lifted-code FP flags, with -march=)
BUILDS = (("x86-64-v3", 1), ("x86-64-v2", 0))   # (-march, EXPECT_FMA: whether RECOMP_FPX_FMA64 holds there)


def skip(why):
    print("fpx sticky: skipped (%s)" % why)
    sys.exit(0)


def compiler(given, env, names, build_name):
    """--cc/--cxx, else $CC/$CXX, else HOOKSHOT_MINGW's when it is set (target_flags gives its target and runtime, which
    another clang on PATH cannot use), else PATH, else the toolchain the build itself uses (emsdk)"""
    for c in (given, os.environ.get(env)):
        if c:
            return shutil.which(c) or (c if os.path.isfile(c) else None)
    if toolchain.mingw(host=True):
        c = toolchain.find(build_name, host=True)
        if os.path.isfile(c):
            return c
    for n in names:
        if shutil.which(n):
            return shutil.which(n)
    c = toolchain.find(build_name, host=True)
    return c if os.path.isfile(c) else None


def target_flags(cc, tmp):
    """clang on Windows: the build's target and lld (as tools/toolchain.py, with its lld-link copy in tmp, not in tools/)"""
    if os.name != "nt" or "clang" not in os.path.basename(cc).lower():
        return []
    flags = toolchain.native_target(host=True)
    if toolchain.mingw(host=True):
        return flags + toolchain.native_link(host=True)
    here = os.path.dirname(cc)
    link = shutil.which("lld-link", path=here) or shutil.which("lld-link")
    if not link:
        lld = shutil.which("lld", path=here) or shutil.which("lld")
        if not lld:
            return flags
        link = os.path.join(tmp, "lld-link.exe")
        if not os.path.exists(link):
            shutil.copyfile(lld, link)
    return flags + ["-fuse-ld=lld", "-B" + os.path.dirname(link), "-Wl,/nodefaultlib:oldnames"]


def run(cmd, timeout=600):
    kw = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW} if os.name == "nt" \
        else {"preexec_fn": lambda: os.nice(10)}
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                          timeout=timeout, **kw)


def build(cmd, what):
    r = run(cmd)
    if r.returncode:
        print(r.stdout)
        raise SystemExit("fpx sticky: building %s failed" % what)


# Guest instructions for the lifted-route case (hand-written encodings): each runs as the lifter emits it, through
# a64emit.translate and a64_lift.op_text (and hot_text), against the same word translated with FPX off.
GUEST = (("FMUL s0, s1, s2", 0x1E220820, 1), ("FNMUL s0, s1, s2", 0x1E228820, 1),
         ("FMADD s0, s1, s2, s3", 0x1F020C20, 0), ("FMLA s0, s1, v2.s[0]", 0x5F821020, 0))   # (name, word, FMUL)


def lifted_cases(a64emit, a64_lift, used):
    """fpx_lifted.h: per guest word its lifted, hot and exact-only (FPX off) C as functions, and the GUEST table"""
    words = [(0x1000, w) for _, w, _ in GUEST]
    lifted, fl = a64emit.translate(words)
    if any(fl):
        raise SystemExit("fpx sticky: a64emit did not translate the guest words: %s" % fl)
    lifted = [a64_lift.op_text(t, used) for t in lifted]
    a64emit.lib().a64_config(1, 0)   # (FPX off: the exact body inline, the reference)
    try:
        exact, _ = a64emit.translate(words)
    finally:
        a64emit.lib().a64_config(1, 1)
    out = ["/* Generated by tests/test_fpx_sticky.py from guest words, as a64_lift.py lifts them. */"]
    for i, (lt, ex) in enumerate(zip(lifted, exact)):
        out.append("static void g_lifted%d(Ctx *__restrict c) { %s }" % (i, lt))
        out.append("static void g_hot%d(Ctx *__restrict c) { %s }" % (i, a64_lift.hot_text(lt)))
        out.append("static void g_exact%d(Ctx *__restrict c) { %s }" % (i, ex))
    out.append("static const struct { const char *name; int mul; void (*lifted)(Ctx *), (*hot)(Ctx *), (*exact)(Ctx *); }"
               " GUEST[] = {%s};" % ", ".join('{"%s", %d, g_lifted%d, g_hot%d, g_exact%d}' % (n, m, i, i, i)
                                               for i, (n, _, m) in enumerate(GUEST)))
    return "\n".join(out) + "\n"


def generate(cxx, tmp, flags):
    """a64_fpx.h, a64_suyu.h, a64_suyu.c and fpx_lifted.h, by the generators the lift runs (a64emit built into tmp)"""
    dll = os.path.join(tmp, "a64emit.dll" if os.name == "nt" else "liba64emit.so")
    build([cxx, "-std=c++20", "-O0", "-shared", "-Wno-everything", "-o", dll, os.path.join(KIT, "lift", "a64emit.cpp")]
          + flags + ([] if os.name == "nt" else ["-fPIC"]), "a64emit")
    import build_emit
    build_emit.build = lambda force=False: dll   # (not a64emit.dll beside the source)
    import a64_lift
    import a64_rtgen
    import a64emit
    import suyu_rt_fixes
    suyu_rt_fixes.install()   # (as nx2/lift.py does)
    used = set()
    try:
        a64_rtgen.write(tmp)
        lifted = lifted_cases(a64emit, a64_lift, used)
    finally:
        if a64emit._lib is not None:   # (unloaded, so the folder can go)
            import _ctypes
            (_ctypes.FreeLibrary if os.name == "nt" else _ctypes.dlclose)(a64emit._lib._handle)
            a64emit._lib = None
    with open(os.path.join(tmp, "fpx_lifted.h"), "w", newline="\n") as f:
        f.write(lifted)
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:   # (the guest words' shapes, and the two the
        f.write(a64_lift.fpx_header(used | {("mul32", "fma32", 2, 3), ("fma32", "fma32", 3, 3)}))   # runs call by name)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    a = p.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        skip("not an x86-64 host: %s" % platform.machine())
    cc = compiler(a.cc, "CC", ("clang", "gcc", "cc"), "clang")
    cxx = compiler(a.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
    if not cc or not cxx:
        skip("no C compiler" if not cc else "no C++ compiler")
    t0 = time.time()
    failed, ran = [], []
    with tempfile.TemporaryDirectory(prefix="hookshot-fpx-sticky-") as tmp:
        generate(cxx, tmp, target_flags(cxx, tmp))
        for march, fma in BUILDS:
            exe = os.path.join(tmp, "fpx_sticky_test_%s.exe" % march)
            build([cc] + target_flags(cc, tmp) + ["-march=" + march] + FLAGS +
                  ["-DEXPECT_FMA=%d" % fma, "-D_CRT_SECURE_NO_WARNINGS", "-I" + os.path.join(KIT, "rt"), "-I" + tmp,
                   os.path.join(ROOT, "tests", "fpx_sticky_test.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe],
                  "the driver at " + march)
            for args in (["lifted"], ["fast", str(FAST_CASES), str(SEED)], ["random", str(RANDOM_TRIPLES), str(SEED)],
                         ["sweep", str(SWEEP_STRIDE)]):
                r = run([exe] + args)
                print(r.stdout.rstrip())
                if r.returncode == 3:
                    print("fpx sticky: %s skipped (this CPU is not %s)" % (march, march))
                    break
                ran.append(march)
                if r.returncode:
                    failed.append("%s %s" % (march, args[0]))
    if not ran:
        skip("this CPU is not x86-64-v2")
    if failed:
        raise SystemExit("fpx sticky: FAIL (%s) in %.1f s" % (", ".join(failed), time.time() - t0))
    print("fpx sticky: pass in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
