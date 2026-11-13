"""Compile the runtime's actual exclusive monitor (rt/a64_rt.c, and rt/a64.h's A64_INLINE_RT copy) and check what the
soft write watch sees of LDXR/STXR and LDXP/STXP.

The renderer takes a page's mark and then reads the page (gpu/render/rx_buffer.cpp), so a store must be in memory
before its mark: a mark set first can be taken with the old bytes, and the store then reaches the GPU never. Each
mark here records the bytes it covers at that moment; after a successful store they must be the stored ones, and
the pages of its first and last byte must be marked. Directed cases pin the monitor's rules (same address and size,
one store per reservation, CLREX, another writer), and a seeded random run compares both copies, step by step,
with the model a64_rt.c documents: STXR succeeds when the reservation is valid for that address and size and memory
still holds the value LDXR read. No game, SDK or GPU is used.
"""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cc", "clang"]   # (the compiler the build uses for the runtime, nx2/build.py)

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A64_INLINE static inline
enum { PAGE = 4096, PAGES = 4 };
static _Alignas(PAGE) uint8_t window[PAGES * PAGE];
static uint8_t *guest = window;   /* (a pointer, as A64_MEM is: the compiler knows no alignment beyond the guest's) */
#define A64_PTR(a) (guest + (uint32_t)(a))
typedef struct GuestContext {
    uint64_t excl_addr, excl_lo, excl_hi;
    uint32_t excl_size;
    int excl_valid;
} GuestContext;

/* every mark: where, how long, and the bytes it covered when it was set */
typedef struct { uint64_t a; uint32_t n; uint8_t bytes[16]; } Mark;
static Mark marks[4];
static int nmarks;
static uint8_t dirty[PAGES];
static void mark(uint64_t a, uint32_t n) {
    if (nmarks == 4 || n > 16) { printf("FAIL: mark %d of %u bytes\n", nmarks, n); exit(1); }
    Mark *m = &marks[nmarks++];
    m->a = a; m->n = n; memcpy(m->bytes, A64_PTR(a), n);
    dirty[(uint32_t)a / PAGE] = 1; dirty[(uint32_t)(a + n - 1) / PAGE] = 1;
}
#define A64_MARK(a, n) mark((a), (n))

#include "actual_exclusive.inc"

typedef struct {
    const char *name;
    uint64_t (*ldxr)(GuestContext *, uint64_t, uint32_t);
    uint32_t (*stxr)(GuestContext *, uint64_t, uint32_t, uint64_t);
    void (*clrex)(GuestContext *);
} Impl;
static const Impl impls[] = {
    {"a64_rt.c", recomp_ldxr, recomp_stxr, recomp_clrex},
    {"a64.h A64_INLINE_RT", a64_i_ldxr, a64_i_stxr, a64_i_clrex},
};

static int failures;
static void check(int ok, const char *impl, const char *what, uint64_t a, uint32_t n) {
    if (!ok && failures++ < 20) printf("FAIL %s: %s (address %#llx, %u bytes)\n", impl, what, (unsigned long long)a, n);
}
static void quiet(void) { nmarks = 0; memset(dirty, 0, sizeof dirty); }
static uint64_t peek(uint64_t a, uint32_t n) { uint64_t v = 0; memcpy(&v, A64_PTR(a), n); return v; }
static void poke(uint64_t a, uint32_t n, uint64_t v) { memcpy(A64_PTR(a), &v, n); }
static uint64_t lane(uint64_t v, uint32_t n) { return n == 8 ? v : v & ((1ull << (8 * n)) - 1); }

/* a store that succeeded is published: both end pages marked, and no mark set before the bytes it covers */
static void published(const char *impl, uint64_t a, uint32_t n) {
    check(dirty[a / PAGE] && dirty[(a + n - 1) / PAGE], impl, "store left its page unmarked", a, n);
    for (int i = 0; i < nmarks; i++)
        check(!memcmp(marks[i].bytes, A64_PTR(marks[i].a), marks[i].n), impl,
              "page marked before the store (a scan in between reads the old bytes)", a, n);
}

static void directed(const Impl *m) {
    static const uint32_t sizes[] = {1, 2, 4, 8};
    for (int s = 0; s < 4; s++) {
        const uint32_t n = sizes[s];
        const uint64_t a = 2 * PAGE - n;   /* (the last slot of a page: the mark must name that page) */
        const uint64_t v = lane(0x8877665544332211ull, n), w = lane(0x0123456789ABCDEFull, n);
        GuestContext c = {0};
        poke(a, n, 0); quiet();
        check(m->stxr(&c, a, n, v) == 1 && peek(a, n) == 0, m->name, "STXR without LDXR stored", a, n);
        m->ldxr(&c, a, n); quiet();
        check(m->stxr(&c, a, n, v) == 0 && peek(a, n) == v, m->name, "STXR after LDXR failed", a, n);
        published(m->name, a, n);
        quiet();
        check(m->stxr(&c, a, n, w) == 1 && peek(a, n) == v, m->name, "second STXR on one reservation stored", a, n);
        m->ldxr(&c, a, n);
        poke(a, n, w); quiet();
        check(m->stxr(&c, a, n, v) == 1 && peek(a, n) == w, m->name, "STXR after another writer's store stored", a, n);
        m->ldxr(&c, a, n);
        m->clrex(&c); quiet();
        check(m->stxr(&c, a, n, v) == 1 && peek(a, n) == w, m->name, "STXR after CLREX stored", a, n);
        if (n < 8) {
            m->ldxr(&c, a - n, n); quiet();
            check(m->stxr(&c, a - n, 2 * n, v) == 1, m->name, "STXR of another size than LDXR stored", a - n, 2 * n);
        }
        m->ldxr(&c, a - 8, n); quiet();
        check(m->stxr(&c, a, n, v) == 1 && peek(a, n) == w, m->name, "STXR to another address than LDXR stored", a, n);
    }
}

static void pairs(void) {
    for (uint32_t n = 4; n <= 8; n += 4) {
        const uint64_t a = 2 * PAGE - 2 * n;   /* (the pair ends the page) */
        const uint64_t lo = lane(0x1122334455667788ull, n), hi = lane(0x99AABBCCDDEEFF00ull, n);
        GuestContext c = {0};
        uint64_t l, h;
        poke(a, n, 0); poke(a + n, n, 0);
        recomp_ldxp(&c, a, n, &l, &h); quiet();
        check(recomp_stxp(&c, a, n, lo, hi) == 0 && peek(a, n) == lo && peek(a + n, n) == hi, "a64_rt.c",
              "STXP after LDXP failed", a, 2 * n);
        published("a64_rt.c", a, 2 * n);
        recomp_ldxp(&c, a, n, &l, &h);
        poke(a + n, n, lo); quiet();
        check(recomp_stxp(&c, a, n, hi, lo) == 1 && peek(a, n) == lo, "a64_rt.c",
              "STXP after another writer changed the high half stored", a, 2 * n);
    }
}

/* both copies against the documented model, on one seeded stream of LDXR, STXR, CLREX and plain stores by another
   writer (which may store the value already there) over the 32 bytes either side of a page boundary */
static void differential(const Impl *m, uint32_t steps) {
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    GuestContext c = {0};
    struct { int valid; uint64_t a, v; uint32_t n; } model = {0};
    static uint8_t before[sizeof window];
    memset(guest, 0x5A, sizeof window);
    for (uint32_t i = 0; i < steps; i++) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t r = (uint32_t)(rng >> 33), n = 1u << (r & 3);
        const uint64_t a = 2 * PAGE - 32 + (uint64_t)((r >> 2) % (64 / n)) * n;
        const uint64_t v = lane(rng * 0xD1B54A32D192ED03ull, n);
        quiet();
        memcpy(before, guest, sizeof window);
        switch ((r >> 8) & 3) {
        case 0: {
            const uint64_t got = m->ldxr(&c, a, n);
            check(got == peek(a, n), m->name, "LDXR value", a, n);
            model.valid = 1; model.a = a; model.n = n; model.v = got;
            break;
        }
        case 1: {
            const int ok = model.valid && model.a == a && model.n == n && peek(a, n) == model.v;
            model.valid = 0;
            check(m->stxr(&c, a, n, v) == (ok ? 0u : 1u), m->name, "STXR status differs from the model", a, n);
            if (ok) {
                memcpy(&before[a], &v, n);   /* (the model's memory: as before, with this store) */
                published(m->name, a, n);
            }
            check(!memcmp(before, guest, sizeof window), m->name,
                  ok ? "STXR stored other bytes than the model" : "a failed STXR changed memory", a, n);
            break;
        }
        case 2:
            m->clrex(&c);
            model.valid = 0;
            break;
        default:
            poke(a, n, (r >> 10) & 1 ? v : peek(a, n));   /* (another writer; half the time the same value) */
            break;
        }
    }
}

int main(void) {
    for (int i = 0; i < 2; i++) {
        directed(&impls[i]);
        differential(&impls[i], 200000);
    }
    pairs();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    puts("PASS: LDXR/STXR and LDXP/STXP follow the monitor's rules, both copies agree with its model over 200000 "
         "steps, and every successful store is marked after it is written");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    rt = (ROOT / "third_party/a64-kit/rt/a64_rt.c").read_text(encoding="utf-8")
    header = (ROOT / "third_party/a64-kit/rt/a64.h").read_text(encoding="utf-8")
    actual = [block(rt, marker) for marker in (
        "static uint64_t load_n(", "static int cas_n(", "uint64_t recomp_ldxr(", "uint32_t recomp_stxr(",
        "void recomp_ldxp(", "uint32_t recomp_stxp(", "void recomp_clrex(")]
    actual += [block(header, marker) for marker in (
        "A64_INLINE uint64_t a64_i_load_n(", "A64_INLINE int a64_i_cas_n(", "A64_INLINE uint64_t a64_i_ldxr(",
        "A64_INLINE uint32_t a64_i_stxr(", "A64_INLINE void a64_i_clrex(")]
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="exclusive-marks-") as temporary:
        folder = Path(temporary)
        (folder / "actual_exclusive.inc").write_text("\n\n".join(actual), encoding="utf-8")
        (folder / "fixture.c").write_text(HARNESS, encoding="utf-8")
        executable = folder / "fixture.exe"
        # (-mcx16 lets clang inline the 128-bit compare-exchange STXP needs, as the build does; it is an x86-64 flag)
        x86 = ["-mcx16"] if platform.machine().lower() in ("amd64", "x86_64") else []
        compile_ = [a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *x86, str(folder / "fixture.c"),
                    "-o", str(executable)]
        for command in (compile_, [str(executable)]):
            result = subprocess.run(command, creationflags=flags, capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", timeout=60)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
