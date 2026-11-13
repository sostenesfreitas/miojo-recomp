"""(hookshot) The fast float regions' ops (a64_lift.py's FASTFP_HEADER: the FMA3 multiply-add, the multiply, divide,
add and subtract, and the vector add, subtract and multiply) against the exact bodies, game-free.

Builds what the real build builds (a64_fpx.h, a64_suyu.h, a64_suyu.c, through tests/test_fpx_sticky.py's generate) plus
FASTFP_HEADER, and compiles tests/fastfp_fma_test.c at -march=x86-64-v3 with the build's FP flags. The op runs alone
inside the regions' MXCSR window (a64_ff_begin / a64_ff_end); a result the window keeps must equal recomp_fpe_fma32's,
value and FPSR, under FPCR.FZ off and on and FPSR with IXC only or every flag set:
  - an edge list: 0 * inf + a quiet NaN (x86: the NaN, no flag; Arm: the default NaN, IOC), a product rounded up to the
    smallest normal (x86 detects tininess after rounding, Arm before: UFC, and a zero under FZ), a subnormal addend or
    operand, an exact cancellation, overflow, quiet NaN operands;
  - 2M random triples: zeros, subnormals, inf, NaN, the extreme normals, products near the underflow and overflow edges
    (a quarter of them rounding near 2^-126), cancellations.

It has teeth (each by hand, then restored, 2026-11-11):
  - the NaN test dropped (`m > 0x7f800000u ||` removed): 0 * inf + qNaN kept with the addend's NaN and no IOC;
  - the smallest-normal test dropped (`|| m == 0x00800000u` removed): UFC missing, and 2^-126 where FZ wants a zero;
  - the addend's place changed (vfmadd231ss made vfmadd213ss: z * a + b): wrong values.
  (a and b swapped is not caught, and need not be: the product commutes, and a NaN result always falls back.)

Every other op is held to the same. Before a64_ff_guard_, the multiply kept 15348 of 8M results that differ from
Arm's: a product rounded up to 2^-126 (UFC missing, and under FZ 2^-126 where Arm gives a zero), and under FZ a
subnormal times a quiet NaN (Arm flushes the subnormal and sets IDC; x86 raises no denormal flag next to a NaN).

    python -I -B tests/test_fastfp_fma.py [--cc clang] [--cxx clang++]
"""
import argparse
import os
import platform
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "third_party", "a64-kit")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_fpx_sticky as S  # noqa: E402

N, SEED = 2000000, 20261111


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    a = p.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        print("fastfp fma: skipped (not an x86-64 host)")
        return
    cc = S.compiler(a.cc, "CC", ("clang", "gcc", "cc"), "clang")
    cxx = S.compiler(a.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
    if not cc or not cxx:
        print("fastfp fma: skipped (no C/C++ compiler)")
        return
    t0 = time.time()
    with tempfile.TemporaryDirectory(prefix="hookshot-fastfp-fma-") as tmp:
        S.generate(cxx, tmp, S.target_flags(cxx, tmp))
        import a64_lift
        with open(os.path.join(tmp, "a64_fastfp.h"), "w", newline="\n") as f:
            f.write(a64_lift.FASTFP_HEADER)
        exe = os.path.join(tmp, "fastfp_fma_test.exe")
        S.build([cc] + S.target_flags(cc, tmp) + ["-march=x86-64-v3"] + S.FLAGS +
                ["-D_CRT_SECURE_NO_WARNINGS", "-I" + os.path.join(KIT, "rt"), "-I" + tmp,
                 os.path.join(ROOT, "tests", "fastfp_fma_test.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe],
                "the driver")
        failed = []
        for op in ("fma", "mul", "div", "add", "sub", "vmul", "vadd", "vsub"):
            r = S.run([exe, op, str(N if op in ("fma", "mul") else N // 4), str(SEED)])
            print(r.stdout.rstrip())
            if r.returncode == 3:
                print("fastfp fma: skipped (this CPU is not x86-64-v3)")
                return
            if r.returncode:
                failed.append(op)
    if failed:
        raise SystemExit("fastfp fma: FAIL (%s) in %.1f s" % (", ".join(failed), time.time() - t0))
    print("fastfp fma: pass in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
