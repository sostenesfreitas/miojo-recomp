/* tests/test_fpx_sticky.py's driver: the generated FPX1 wrappers a64_fpx_mul32_fma32 (FMUL/FNMUL S) and
   a64_fpx_fma32_fma32 (FMLA family S) against the exact body they fall back to (recomp_fpe_fma32), value and FPSR.
   Built against the real a64.h with the a64_suyu.h / a64_suyu.c / a64_fpx.h the generators write, at -march=x86-64-v3
   (-DEXPECT_FMA=1: a hardware FMA, both ops rescued) or -march=x86-64-v2 (-DEXPECT_FMA=0: only FMUL is; FMLA's rescue
   must decline every case). The rescues are counted inside the generated wrappers (A64_FPX_STICKY_PROBE, empty in the
   real build), and one under a closed gate (an FPCR control bit but FZ, or IXC clear) counts as a mismatch: the exact
   body models none of FIZ/AH/NEP, so only the gate keeps the rescue away from them.

   fpx_sticky_test lifted              guest FMUL/FNMUL/FMADD/FMLA S words as the lifter emits them (fpx_lifted.h: the
                                       outlined and the hot-function form) against the same words with FPX off, on
                                       inputs that must be rescued and inputs that must fall back: whole Ctx and the
                                       rescue count
   fpx_sticky_test fast <n> <seed>     the inline fast paths of FMADD, FRECPS and FRSQRTS S (recomp_fpx_fma32, _recps32,
                                       _rsqrts32) on n random operands and states, value and FPSR; and binary32
                                       midpoints, which only a fused FMA keeps: kept exactly when EXPECT_FMA (exit 4)
   fpx_sticky_test random <n> <seed>   n random operand triples (both ops each) over random FPCR/FPSR states
   fpx_sticky_test sweep <stride>      every a with biased exponent <= 4 or >= 250 whose fraction is below 2^12, above
                                       2^23 - 2^12 or a multiple of stride, x a fixed set of b and addends, x FZ on/off,
                                       x flags all set / IXC only, and one more state per a (partial flags, IXC
                                       clear, FIZ/AH/NEP)
   Exit 0: no mismatch, the wrappers rescued FMUL cases, and FMLA ones exactly when EXPECT_FMA. 1: a mismatch.
   3: the CPU lacks this build's -march (x86-64-v3: FMA/AVX2; x86-64-v2: SSSE3/SSE4.2/POPCNT). 4: rescue counts wrong. */
static unsigned long long g_sticky;   /* the generated wrappers' rescues */
#define A64_FPX_STICKY_PROBE() ((void)g_sticky++)
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
#include <cpuid.h>
#if !defined(EXPECT_FMA) || RECOMP_FPX_HOST != 2 || RECOMP_FPX_FMA64 != EXPECT_FMA
#error "build with -DEXPECT_FMA=1 at x86-64-v3 or -DEXPECT_FMA=0 at x86-64-v2 (RECOMP_FPX_FMA64 must agree)"
#endif
/* The FPCR bits the rescue's soundness needs clear (the test's own copy of RECOMP_FPX_FPCR_MASK): every control bit
   but FZ (FIZ AH NEP, trap enables, FZ16, RMode, DN, AHP) and the host's kill switch. */
#define GATE_FPCR (UINT64_C(0x06C8FF07) | (UINT64_C(1) << 32))
#include "fpx_lifted.h"

static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32_t r32(void) { return (uint32_t)(rnd() >> 16); }
static uint32_t rbelow(uint32_t n) { return (uint32_t)(((rnd() >> 32) * n) >> 32); }
static uint32_t mkf(uint32_t sign, int e, uint32_t frac) { return (sign << 31) | ((uint32_t)e << 23) | (frac & 0x7fffff); }
static float F(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static uint32_t B(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }

/* ---- operand classes and states (fpstub's harness) ---- */
static uint32_t sparse_frac(void) {   /* fractions with few bits: exact products, ties, cancellations */
    switch (rbelow(4)) {
    case 0: return 0;
    case 1: return (uint32_t)1 << rbelow(23);
    case 2: return r32() & 0x7ff000;
    default: return r32() & 0x7fffff;
    }
}
static uint32_t any_operand(void) {
    const uint32_t s = r32() & 1;
    switch (rbelow(16)) {
    case 0: return s << 31;                                          /* zero */
    case 1: return mkf(s, 0, r32() | 1);                             /* subnormal */
    case 2: return mkf(s, 0, rbelow(8) + 1);                         /* tiny subnormal */
    case 3: return mkf(s, 0, 0x7fffff - rbelow(8));                  /* large subnormal */
    case 4: return mkf(s, 1 + rbelow(3), sparse_frac());             /* smallest normals */
    case 5: return mkf(s, 254 - rbelow(3), sparse_frac());           /* largest normals */
    case 6: return mkf(s, 255, 0);                                   /* inf */
    case 7: return mkf(s, 255, (r32() | 1) & 0x7fffff);              /* NaN, quiet or signalling */
    case 8: return mkf(s, 255, 0x400000 | (r32() & 0x3fffff));       /* quiet NaN */
    case 9: case 10: return mkf(s, 120 + rbelow(15), sparse_frac()); /* near 1 */
    default: return mkf(s, 1 + rbelow(254), r32());                  /* any normal */
    }
}
static void tiny_pair(uint32_t *a, uint32_t *b) {   /* a product near the bottom of the normal range, or the overflow edge */
    const int target = (rbelow(8) == 0) ? 255 - (int)rbelow(3) : (int)rbelow(60) - 30;   /* biased product exponent */
    int ea = 1 + (int)rbelow(253);
    int eb = target - ea + 127;
    if (eb < 1 || eb > 254) { eb = 1 + (int)rbelow(254); ea = target - eb + 127; }
    if (ea < 1) ea = rbelow(4) ? 1 : 0;
    if (ea > 254) ea = 254;
    *a = mkf(r32() & 1, ea, rbelow(2) ? sparse_frac() : r32());
    *b = mkf(r32() & 1, eb, rbelow(2) ? sparse_frac() : r32());
}
static uint32_t addend_for(uint32_t a, uint32_t b) {
    const double p = (double)F(a) * (double)F(b);
    switch (rbelow(10)) {
    case 0: return any_operand();
    case 1: return B((float)-p);                                                      /* near (often exact) cancellation */
    case 2: return B((float)-p) + (uint32_t)((int)rbelow(9) - 4);                     /* a few ulps off */
    case 3: return r32() & 0x80000000u;                                               /* a zero */
    case 4: return B((float)p) ^ (rbelow(2) ? 0x80000000u : 0);
    case 5: return mkf(r32() & 1, 0, r32());                                          /* subnormal */
    case 6: return mkf(r32() & 1, 1 + rbelow(4), r32());                              /* near the smallest normal */
    default: return mkf(r32() & 1, 1 + rbelow(254), r32());
    }
}
static void gen_state(uint64_t *fpcr, uint64_t *fpsr) {
    uint64_t cr = 0, sr = 0;
    if (rbelow(2)) cr |= 1u << 24;                        /* FZ */
    if (rbelow(10) == 0) cr |= 1u << 25;                  /* DN (gate closed) */
    if (rbelow(10) == 0) cr |= (uint64_t)rbelow(4) << 22; /* RMode */
    if (rbelow(40) == 0) cr |= r32() & 0x07ff9f07u;       /* anything else (FIZ, AH, NEP included) */
    if (rbelow(16) == 0) cr |= 1u << rbelow(3);           /* FIZ, AH or NEP alone (the gate must stay closed) */
    if (rbelow(200) == 0) cr |= RECOMP_FPX_INHIBIT;
    static const uint32_t bits[6] = {1, 2, 4, 8, 16, 128};   /* IOC DZC OFC UFC IXC IDC */
    for (int k = 0; k < 6; k++)
        if (k == 4 ? rbelow(10) != 0 : rbelow(3) != 0) sr |= bits[k];   /* IXC 90%, the others 67% */
    if (rbelow(8) == 0) sr |= 1u << 27;                   /* QC */
    *fpcr = cr;
    *fpsr = sr;
}

/* ---- one check: both wrappers against the exact body ---- */
static uint64_t checks, mismatches, rescued[2], closed_checks;
static int report_left = 20;
static void check(uint64_t cr, uint64_t sr, uint32_t a, uint32_t b, uint32_t z, int mul) {
    Ctx ref, got;
    memset(&ref, 0, sizeof ref);
    ref.fpcr = cr; ref.fpsr = sr;
    got = ref;
    const uint64_t r = recomp_fpe_fma32(&ref, a, b, z);
    const unsigned long long before = g_sticky;
    const uint64_t v = mul ? a64_fpx_mul32_fma32(&got, a, b, z) : a64_fpx_fma32_fma32(&got, a, b, z);
    const int kept = g_sticky != before, closed = (cr & GATE_FPCR) || !(sr & 16u);
    checks++;
    rescued[mul] += kept;
    closed_checks += closed;
    if (kept && closed) {
        mismatches++;
        if (report_left-- > 0)
            printf("GATE %s a=%08x b=%08x z=%08x fpcr=%llx fpsr=%llx: rescued under a closed gate\n", mul ? "FMUL" : "FMLA",
                   a, b, z, (unsigned long long)cr, (unsigned long long)sr);
    } else if (v != r || got.fpsr != ref.fpsr) {
        mismatches++;
        if (report_left-- > 0)
            printf("MISMATCH %s a=%08x b=%08x z=%08x fpcr=%llx fpsr=%llx: exact %08llx/%llx wrapper %08llx/%llx\n",
                   mul ? "FMUL" : "FMLA", a, b, z, (unsigned long long)cr, (unsigned long long)sr, (unsigned long long)r,
                   (unsigned long long)ref.fpsr, (unsigned long long)v, (unsigned long long)got.fpsr);
    }
}
static void both(uint64_t cr, uint64_t sr, uint32_t a, uint32_t b, uint32_t z) {
    check(cr, sr, a, b, (a ^ b) & 0x80000000u, 1);   /* FMUL: the lifter passes a zero of the product's sign */
    check(cr, sr, a, b, z, 0);
}

static void random_cases(uint64_t n, uint64_t seed) {
    rs = seed * 0x9E3779B97F4A7C15ULL + 1;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t a, b; uint64_t cr, sr;
        if (rbelow(2)) tiny_pair(&a, &b);
        else { a = any_operand(); b = any_operand(); }
        const uint32_t z = addend_for(a, b);
        gen_state(&cr, &sr);
        both(cr, sr, a, b, z);
    }
}

static const uint32_t B_SET[] = {
    0x3f800000, /* 1 */ 0x3f000000, /* 0.5 */ 0x3fc00001, /* 1.5+ulp */ 0x00800000, /* min normal */ 0x7f7fffff, /* max */
    0x80000001, /* -min subnormal */ 0x007fffff, /* max subnormal */ 0x7f800000, /* inf */ 0x00000000, /* +0 */
    0x5f000000, /* 2^63 */ 0x3fffffff, /* ~2 */ 0x1f800000 /* 2^-64 */};
/* one more state per a: partial flags (each needed bit clear in turn), IXC clear, and FIZ / AH / NEP (gate closed) */
static const uint32_t MORE[16][2] = {
    {0x01000000u, 0x18u}, {0, 0x18u}, {0x01000000u, 0x90u}, {0x01000000u, 0x94u}, {0, 0x14u}, {0x01000000u, 0x98u},
    {0x01000000u, 0x1cu}, {0, 0x1cu}, {0x01000000u, 0x9bu}, {0x01000000u, 0x97u}, {0x01000000u, 0x1fu},
    {0x01000000u, 0x8fu}, {0x01000002u, 0x9fu}, {0x01000001u, 0x9fu}, {0x01000004u, 0x9fu}, {0x00000002u, 0x9fu}};
static void sweep(uint32_t stride) {
    for (unsigned bi = 0; bi < sizeof B_SET / sizeof B_SET[0]; bi++) {
        const uint32_t b = B_SET[bi];
        for (uint32_t hi = 0; hi < 2 * 256; hi++) {   /* sign and biased exponent of a */
            const uint32_t e = hi & 255;
            if (!(e <= 4 || e >= 250)) continue;
            for (uint32_t f = 0; f < (1u << 23); f++) {
                if (f >= 4096 && f < (1u << 23) - 4096 && f % stride) continue;
                const uint32_t a = (hi >> 8) << 31 | e << 23 | f;
                const uint64_t cr = (f & 1) ? 0x01000000u : 0;   /* FZ */
                const uint64_t sr = (f & 2) ? 0x9fu : 0x10u;     /* every sticky bit set, or IXC only */
                uint32_t z;
                switch ((f >> 2) & 3) {   /* addend: cancellation, near the smallest normal, subnormal, zero */
                case 0: z = B(-(float)((double)F(a) * (double)F(b))); break;
                case 1: z = 0x00800000u ^ (a & 0x80000000u); break;
                case 2: z = 0x00000003u ^ (f << 31); break;
                default: z = 0x80000000u; break;
                }
                both(cr, sr, a, b, z);
                both(cr ^ 0x01000000u, sr ^ 0x8fu, a, b, z);   /* the other FZ and flag state, same operands */
                const uint32_t *m = MORE[((f >> 4) + hi + bi) & 15];
                both(m[0], m[1], a, b, z);
            }
        }
    }
}

/* ---- the lifted route: guest words through a64emit and a64_lift.op_text ---- */
#define FZ 0x01000000u
static const struct { uint32_t a, b, z; uint32_t fpcr, fpsr; int rescue; const char *what; } LIFTED[] = {
    {0x1c800000u, 0x1c800000u, 0, FZ, 0x9fu, 1, "tiny product, FZ, flags set: a zero"},
    {0x1c800000u, 0x9c800000u, 0x80000000u, FZ, 0x9fu, 1, "tiny negative product, FZ, flags set: -0"},
    {0x1c800000u, 0x1e000000u, 0, 0, 0x9fu, 1, "tiny product, no FZ, UFC set: the subnormal"},
    {0x71800000u, 0x71800000u, 0x3f800000u, FZ, 0x9fu, 1, "overflow, OFC set: inf"},
    {0x00000005u, 0x3fc00000u, 0x3f800000u, FZ, 0x9fu, 1, "subnormal operand, FZ, IDC set: flushed"},
    {0x1c800000u, 0x1c800000u, 0, FZ, 0x10u, 0, "tiny product, FZ, IXC only: falls back"},
    {0x1c800000u, 0x1e000000u, 0, 0, 0x97u, 0, "tiny product, no FZ, UFC clear: falls back"},
    {0x71800000u, 0x71800000u, 0x3f800000u, FZ, 0x9bu, 0, "overflow, OFC clear: falls back"},
    {0x00000005u, 0x3fc00000u, 0x3f800000u, FZ, 0x1fu, 0, "subnormal operand, FZ, IDC clear: falls back"},
    {0x7fc00001u, 0x3f800000u, 0x3f800000u, FZ, 0x9fu, 0, "NaN operand: falls back"},
    {0x1c800000u, 0x1c800000u, 0, FZ | 2u, 0x9fu, 0, "tiny product, FZ|AH (gate closed): falls back"},
    {0x3fc00000u, 0x40000000u, 0x3e800000u, FZ, 0x9fu, 0, "normal (the fast path keeps it)"},
};
static int lifted_cases(void) {
    unsigned bad = 0, n = 0, kept = 0;
    for (unsigned g = 0; g < sizeof GUEST / sizeof GUEST[0]; g++)
        for (unsigned k = 0; k < sizeof LIFTED / sizeof LIFTED[0]; k++)
            for (int hot = 0; hot < 2; hot++) {
                Ctx ref, got;
                memset(&ref, 0, sizeof ref);
                for (int r = 0; r < 32; r++) { ref.vreg[r][0] = 0x0123456789abcdefULL * (r + 1); ref.vreg[r][1] = ~ref.vreg[r][0]; }
                ref.vreg[1][0] = (ref.vreg[1][0] & ~0xffffffffULL) | LIFTED[k].a;
                ref.vreg[2][0] = (ref.vreg[2][0] & ~0xffffffffULL) | LIFTED[k].b;
                ref.vreg[3][0] = (ref.vreg[3][0] & ~0xffffffffULL) | LIFTED[k].z;   /* FMADD's addend */
                ref.vreg[0][0] = (ref.vreg[0][0] & ~0xffffffffULL) | LIFTED[k].z;   /* FMLA's accumulator */
                ref.fpcr = LIFTED[k].fpcr;
                ref.fpsr = LIFTED[k].fpsr;
                got = ref;
                GUEST[g].exact(&ref);
                const unsigned long long before = g_sticky;
                (hot ? GUEST[g].hot : GUEST[g].lifted)(&got);
                const unsigned long long rescued = g_sticky - before;
                const unsigned want = LIFTED[k].rescue && (GUEST[g].mul || EXPECT_FMA);
                n++;
                kept += (unsigned)rescued;
                if (rescued != want || memcmp(&ref, &got, sizeof ref)) {
                    bad++;
                    printf("LIFTED %s (%s), %s: rescued %llu, want %u; s0 exact %08x/fpsr %llx, lifted %08x/fpsr %llx%s\n",
                           GUEST[g].name, hot ? "hot" : "outlined", LIFTED[k].what, rescued, want, (uint32_t)ref.vreg[0][0],
                           (unsigned long long)ref.fpsr, (uint32_t)got.vreg[0][0], (unsigned long long)got.fpsr,
                           memcmp(&ref, &got, sizeof ref) ? " (Ctx differs)" : "");
                }
            }
    printf("lifted (%s): %u runs of %u guest words, %u mismatches, %u rescued in the wrappers\n",
           EXPECT_FMA ? "x86-64-v3" : "x86-64-v2", n, (unsigned)(sizeof GUEST / sizeof GUEST[0]), bad, kept);
    return bad ? 1 : 0;
}

/* ---- the inline fast paths of FMADD/FRECPS/FRSQRTS S (a fused FMA when EXPECT_FMA, nx2/suyu_rt_fixes.py) ---- */
static uint32_t fast_op(int op, Ctx *c, uint32_t a, uint32_t b, uint32_t z, int *kept) {
    uint64_t v = 0;
    *kept = RECOMP_FPX_OPEN(c) && (op == 0 ? recomp_fpx_fma32(a, b, z, RECOMP_FPX_FZ(c), &v)
                                   : op == 1 ? recomp_fpx_recps32(a, b, RECOMP_FPX_FZ(c), &v)
                                             : recomp_fpx_rsqrts32(a, b, RECOMP_FPX_FZ(c), &v));
    if (*kept) return (uint32_t)v;
    return (uint32_t)(op == 0 ? recomp_fpe_fma32(c, a, b, z) : op == 1 ? recomp_fpe_recps32(c, a, b) : recomp_fpe_rsqrts32(c, a, b));
}
static const char *const FAST_NAME[3] = {"FMADD", "FRECPS", "FRSQRTS"};
static uint64_t fast_kept[3];
static void fast_check(int op, uint64_t cr, uint64_t sr, uint32_t a, uint32_t b, uint32_t z, int *kept) {
    Ctx ref, got;
    memset(&ref, 0, sizeof ref);
    ref.fpcr = cr; ref.fpsr = sr;
    got = ref;
    const uint32_t r = (uint32_t)(op == 0 ? recomp_fpe_fma32(&ref, a, b, z) : op == 1 ? recomp_fpe_recps32(&ref, a, b)
                                                                                  : recomp_fpe_rsqrts32(&ref, a, b));
    const uint32_t v = fast_op(op, &got, a, b, z, kept);
    checks++;
    fast_kept[op] += *kept;
    if (v != r || got.fpsr != ref.fpsr) {
        mismatches++;
        if (report_left-- > 0)
            printf("MISMATCH %s a=%08x b=%08x z=%08x fpcr=%llx fpsr=%llx: exact %08x/%llx fast %08x/%llx\n", FAST_NAME[op],
                   a, b, z, (unsigned long long)cr, (unsigned long long)sr, r, (unsigned long long)ref.fpsr, v,
                   (unsigned long long)got.fpsr);
    }
}
/* exactly halfway between two binary32 values: the binary64 form declines them, a fused FMA keeps them */
static const uint32_t MIDPOINT[3][3] = {{0x3f800800u, 0x3f800800u, 0}, {0x3f800800u, 0xbf801000u, 0},
                                        {0x3f800800u, 0x3f800800u, 0}};
static int fast_cases(uint64_t n, uint64_t seed) {
    static const float K[] = {1.0f, 2.0f, 3.0f, 1.5f, 0.5f};
    rs = seed * 0x9E3779B97F4A7C15ULL + 1;
    for (uint64_t i = 0; i < n; i++) {
        const int op = (int)rbelow(3);
        uint32_t a, b, z = 0; uint64_t cr, sr; int kept;
        if (rbelow(2)) tiny_pair(&a, &b);
        else { a = any_operand(); b = any_operand(); }
        if (op && rbelow(2)) {   /* a*b near 1, 2 or 3: the Newton steps, and the cancellations */
            b = B(K[rbelow(5)] / F(a)) + (uint32_t)((int)rbelow(9) - 4);
            if (rbelow(4) == 0) b ^= 0x80000000u;
        }
        if (op == 0) z = addend_for(a, b);
        gen_state(&cr, &sr);
        if (rbelow(4)) { cr &= 1u << 24; sr |= 16u; }   /* mostly the open gate, which is what is tested */
        fast_check(op, cr, sr, a, b, z, &kept);
    }
    int bad = 0;
    for (int op = 0; op < 3; op++)
        for (int fz = 0; fz < 2; fz++) {
            int kept;
            fast_check(op, fz ? 0x01000000u : 0, 0x10u, MIDPOINT[op][0], MIDPOINT[op][1], MIDPOINT[op][2], &kept);
            if (kept != EXPECT_FMA) {
                bad = 1;
                printf("%s midpoint a=%08x b=%08x fz=%d: %s, want %s\n", FAST_NAME[op], MIDPOINT[op][0], MIDPOINT[op][1], fz,
                       kept ? "kept" : "fell back", EXPECT_FMA ? "kept (a fused FMA)" : "fell back");
            }
        }
    printf("fast (%s): %llu checks, %llu mismatches, kept %llu FMADD / %llu FRECPS / %llu FRSQRTS\n",
           EXPECT_FMA ? "x86-64-v3" : "x86-64-v2", (unsigned long long)checks, (unsigned long long)mismatches,
           (unsigned long long)fast_kept[0], (unsigned long long)fast_kept[1], (unsigned long long)fast_kept[2]);
    return mismatches ? 1 : bad ? 4 : 0;
}

int main(int argc, char **argv) {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx >> 9 & 1) || !(ecx >> 20 & 1) || !(ecx >> 23 & 1)) {
        printf("this CPU has no SSSE3/SSE4.2/POPCNT (x86-64-v2)\n");
        return 3;
    }
    if (EXPECT_FMA && (!(ecx >> 12 & 1) || !__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) || !(ebx >> 5 & 1))) {
        printf("this CPU has no FMA/AVX2 (x86-64-v3)\n");
        return 3;
    }
    if ((_mm_getcsr() & 0xffc0) != 0x1f80) { printf("MXCSR %x: not the FPX1 host contract\n", _mm_getcsr()); return 2; }
    if (argc == 2 && !strcmp(argv[1], "lifted")) return lifted_cases();
    if (argc == 4 && !strcmp(argv[1], "fast")) return fast_cases(strtoull(argv[2], 0, 0), strtoull(argv[3], 0, 0));
    if (argc == 4 && !strcmp(argv[1], "random")) random_cases(strtoull(argv[2], 0, 0), strtoull(argv[3], 0, 0));
    else if (argc == 3 && !strcmp(argv[1], "sweep")) sweep((uint32_t)strtoul(argv[2], 0, 0));
    else { fprintf(stderr, "usage: lifted | fast <n> <seed> | random <n> <seed> | sweep <stride>\n"); return 2; }
    printf("%s (%s): %llu checks (%llu with the gate closed), %llu mismatches, the wrappers rescued %llu FMUL / %llu FMLA\n",
           argv[1], EXPECT_FMA ? "x86-64-v3" : "x86-64-v2", (unsigned long long)checks, (unsigned long long)closed_checks,
           (unsigned long long)mismatches, (unsigned long long)rescued[1], (unsigned long long)rescued[0]);
    if (mismatches) return 1;
    if (!rescued[1] || (EXPECT_FMA ? !rescued[0] : rescued[0] != 0)) {
        printf("rescue counts wrong: want FMUL > 0 and FMLA %s\n", EXPECT_FMA ? "> 0" : "== 0 (no hardware FMA)");
        return 4;
    }
    return 0;
}
