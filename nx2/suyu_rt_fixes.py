"""Changes to suyu's runtime header (a64-kit/lift/a64_rtgen.py's a64_suyu.h), applied by wrapping a64_rtgen.runtime_h.

1. FPX1's binary32 FMA, FRECPS and FRSQRTS fast paths (recomp_fpx_fma32, _recps32, _rsqrts32) use the host's fused
   multiply-add on an x86-64 host that has one (RECOMP_FPX_FMA64: x86-64-v3), as they do on an AArch64 host. suyu's
   header gives that to AArch64 alone, and an x86-64-v3 build computed them in binary64 and rounded twice, falling
   back to the exact body on every binary32 midpoint. One fused operation rounds once, so the result is the same where
   both keep it (tests/test_fpx_sticky.py's "fast" run), and the midpoints are kept too.

Each change is a text substitution required to match exactly once, so a refreshed a64-kit that moved these lines fails
here loudly.
"""
import a64_rtgen

_FMA_GUARD = "#if RECOMP_FPX_HOST == 1\n"
SUBST = [(sig + _FMA_GUARD, sig + "#if RECOMP_FPX_HOST == 1 || RECOMP_FPX_FMA64\n") for sig in (
    "static RECOMP_INLINE int recomp_fpx_fma32(uint64_t a, uint64_t b, uint64_t z, uint32_t fz, uint64_t* v) {\n",
    "static RECOMP_INLINE int recomp_fpx_recps32(uint64_t a, uint64_t b, uint32_t fz, uint64_t* v) {\n",
    "static RECOMP_INLINE int recomp_fpx_rsqrts32(uint64_t a, uint64_t b, uint32_t fz, uint64_t* v) {\n")]


def patch(h):
    for old, new in SUBST:
        n = h.count(old)
        if n != 1:
            raise SystemExit("suyu_rt_fixes: %r found %d times in a64_suyu.h (expected once): update nx2/suyu_rt_fixes.py"
                             % (old[:70], n))
        h = h.replace(old, new)
    return h


_installed = False


def install():
    global _installed
    if _installed:
        return
    _installed = True
    base_runtime_h = a64_rtgen.runtime_h
    a64_rtgen.runtime_h = lambda: patch(base_runtime_h())
