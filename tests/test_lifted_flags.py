"""The lifted code's compile flags (nx2/build.py flags): no lifetime markers, for the chunks and the hot pieces alike. Without them a
compiler's peak memory is about 10% lower on a lifted chunk and on a hot piece (clang 23), and the lifted code runs as fast. With
--cxx naming clang, the flags also compile a scoped local to IR with no llvm.lifetime call (the option is a cc1 one: a clang that
dropped it would stop the build).

python -I -B tests/test_lifted_flags.py [--cxx clang++]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import build  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

SOURCE = """extern void use(long *);
long f(long n) {
    long s = 0;
    for (long i = 0; i < n; i++) {
        long tmp[8];
        use(tmp);
        s += tmp[i & 7];
    }
    return s;
}
"""


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def ir(cxx, fl, d):
    src, out = os.path.join(d, "t.c"), os.path.join(d, "t.ll")
    Path(src).write_text(SOURCE, newline=chr(10))
    r = subprocess.run([cxx, "-x", "c"] + fl + ["-S", "-emit-llvm", "-o", out, src], capture_output=True, text=True)
    check("clang compiles with the lifted flags (%s): %s" % (" ".join(fl), r.stderr.strip()), r.returncode == 0)
    return Path(out).read_text()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cxx")
    a = p.parse_args()
    for opt in ("O1", "O2"):
        fl = build.flags("gen", "rt64", opt, True)
        pairs = list(zip(fl, fl[1:]))
        check("-%s: -disable-lifetime-markers goes to the compiler itself (%s)" % (opt, " ".join(fl)),
              ("-Xclang", "-disable-lifetime-markers") in pairs)
        check("-%s: the level is still the one asked for" % opt, "-" + opt in fl)
    if a.cxx and "clang" in os.path.basename(a.cxx).lower():
        fl = [f for f in build.flags("gen", "rt64", "O1", True) if not f.startswith(("-I", "--target=", "--sysroot="))]
        with tempfile.TemporaryDirectory(prefix="hookshot-lifted-flags-") as d:
            check("the reference compile has lifetime markers, so the check below means something",
                  "llvm.lifetime" in ir(a.cxx, [f for f in fl if f not in ("-Xclang", "-disable-lifetime-markers")], d))
            check("the lifted flags leave no lifetime markers", "llvm.lifetime" not in ir(a.cxx, fl, d))
    else:
        print("lifted flags: no clang given (--cxx): the compile is not checked")
    print("lifted flags: ok")


if __name__ == "__main__":
    main()
