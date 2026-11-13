"""The OptimizeChangeBank thread's allocations are tried again when their heap is full (host/oot_heap_retry.c, nx2/oot_patches.py).

Issue 1697 (thread OptimizeChangeBanc writes guest 0x8 a few frames after loading a save) and TomShafford/Hookshot's crash at
backflip + attack: a model's CPU buffer (a heap's tryAlloc) or GPU memory (f_80522050) came back null from a full heap and the
game wrote through it. Builds host/oot_heap_retry.c, as it is, over a guest memory of its own with made-up sead heaps (name,
size, parent, a vtable with tryAlloc and getFreeSize) and a stand-in for the game's f_80522050, then checks:

- an allocation that succeeds is the game's alone: one call, nothing logged, nothing changed;
- a GPU allocation that fails is tried again in each parent with room, then in the game's default heap (no waits:
  it never sleeps); the request names its own heap again afterwards; nothing with room: it fails as before;
- the CPU buffer is tried in each parent with room, returned in x0, the other registers and the flags as they were;
- HOS_GPU_ALLOC_RETRY=0: the game's call alone, as shipped; HOS_GPU_ALLOC_TEST_FAIL=<n>: the first n refused by the named
  heap without calling it (nothing leaks), and the retry makes them;
- nx2/oot_patches.py makes both edits on lifted text shaped as the lift writes it, once, and a second run changes nothing.

python -I -B tests/test_oot_heap_retry.py [--cc gcc]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

HOS_H = r'''#pragma once
#include <stdint.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint8_t n, z, c, v; } Ctx;
typedef void (*A64Fn)(Ctx *);
#define GUEST 0x80000000ull
extern unsigned char mem[0x100000];
static inline uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return mem[a - GUEST]; }
static inline uint64_t recomp_load64(Ctx *c, uint64_t a) { uint64_t v; (void)c; memcpy(&v, mem + (a - GUEST), 8); return v; }
static inline void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c; memcpy(mem + (a - GUEST), &v, 8); }
A64Fn a64_lookup(uint64_t a);
void a64_icall(Ctx *c, uint64_t a);
void hos_log(const char *fmt, ...);
void hos_sleep_ns(int64_t ns);
'''

FIXTURE = r'''#include "hos.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
unsigned char mem[0x100000];
static int logs, sleeps, game_calls;
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); logs++; }
void hos_sleep_ns(int64_t ns) { (void)ns; sleeps++; }

/* heaps: at H(i), +0 the vtable, +0x20 its name, +0x30 its size, +0x38 its parent; the room left in room[i] */
#define VT 0x80001000ull
#define FN_TRY 0x80002000ull
#define FN_FREE 0x80002100ull
#define H(i) (0x80010000ull + 0x100ull * (i))
#define NAME(i) (0x80020000ull + 0x40ull * (i))
static uint64_t room[8], next_block = 0x80080000ull, free_after_calls[8];
static uint64_t heap_index(uint64_t h) { return (h - H(0)) / 0x100; }
static void try_alloc(Ctx *c) {   /* tryAlloc(heap, size, align): the block or 0 */
    const uint64_t i = heap_index(c->x[0]), size = c->x[1];
    c->x[1] = c->x[2] = c->x[3] = 0xdead;   /* (a callee's scratch registers) */
    c->n = c->z = c->c = c->v = 1;
    if (room[i] < size) { c->x[0] = 0; return; }
    room[i] -= size;
    c->x[0] = next_block, next_block += (size + 15) & ~15ull;
}
static void get_free(Ctx *c) { c->x[0] = room[heap_index(c->x[0])]; c->x[5] = 0xdead; }
A64Fn a64_lookup(uint64_t a) { return a == FN_TRY ? try_alloc : a == FN_FREE ? get_free : NULL; }
void a64_icall(Ctx *c, uint64_t a) { a64_lookup(a)(c); }
static void make_heap(int i, int parent, uint64_t free_room, const char *name) {
    recomp_store64(NULL, H(i), VT);
    strcpy((char *)mem + (NAME(i) - GUEST), name);
    recomp_store64(NULL, H(i) + 0x20, NAME(i));
    recomp_store64(NULL, H(i) + 0x30, 0x100000);
    recomp_store64(NULL, H(i) + 0x38, parent < 0 ? 0 : H(parent));
    room[i] = free_room;
}

/* the game's f_80522050: x1 the request (+0 count, +8 the heap, 0: the default, heap 3), x2 the size of one; x0 bit 0 */
static uint64_t default_heap = H(3);
static void game_gpu_alloc(Ctx *c) {
    game_calls++;
    uint64_t heap = recomp_load64(c, c->x[1] + 8);
    if (!heap) heap = default_heap;
    const uint64_t i = heap_index(heap), size = recomp_load64(c, c->x[1]) * c->x[2];
    if (free_after_calls[i] && game_calls >= (int)free_after_calls[i]) room[i] += size;   /* (a free landed) */
    c->x[0] = room[i] >= size ? (room[i] -= size, 1) : 0;
    c->x[9] = 0xdead;
}
void hos_oot_gpu_alloc(Ctx *c, void (*game)(Ctx *));
int hos_oot_heap_refuse(void);
void hos_oot_heap_retry(Ctx *c, uint64_t heap, uint64_t size, uint64_t align, uint64_t lr);

#define REQ 0x80030000ull
static void request(Ctx *c, uint64_t heap, uint64_t count) {
    memset(c, 0, sizeof *c);
    recomp_store64(c, REQ, count);
    recomp_store64(c, REQ + 8, heap);
    recomp_store64(c, REQ + 0x20, NAME(7));
    c->x[0] = 0x80040000ull, c->x[1] = REQ, c->x[2] = 0x100, c->x[30] = 0x80456f1cull;
    game_calls = logs = sleeps = 0;
}
static void reset(void) {
    memset(free_after_calls, 0, sizeof free_after_calls);
    make_heap(0, 1, 0, "OptimizeChangeBank");   /* (the named heap; its parent 1, whose parent is the root 2) */
    make_heap(1, 2, 0, "Model");
    make_heap(2, -1, 0, "Root");
    make_heap(3, -1, 0, "Default");
}
int main(int argc, char **argv) {
    Ctx c;
    recomp_store64(NULL, VT + 0x30, FN_TRY);
    recomp_store64(NULL, VT + 0x80, FN_FREE);
    strcpy((char *)mem + (NAME(7) - GUEST), "gsys::ModelResource(MatBlock)");
    const char *mode = argc > 1 ? argv[1] : "";

    if (!strcmp(mode, "off")) {   /* HOS_GPU_ALLOC_RETRY=0: the game's call alone */
        reset(), room[1] = 1 << 20;
        request(&c, H(0), 4);
        hos_oot_gpu_alloc(&c, game_gpu_alloc);
        CHECK(!(c.x[0] & 1) && game_calls == 1 && !logs);
        c.x[0] = 0;
        hos_oot_heap_retry(&c, H(0), 0x4198, 8, 0x80456910ull);
        CHECK(c.x[0] == 0 && room[1] == 1 << 20);
        CHECK(!hos_oot_heap_refuse());
        puts("PASS: HOS_GPU_ALLOC_RETRY=0: the game's own call alone");
        return 0;
    }
    if (!strcmp(mode, "off-refuse")) {   /* HOS_GPU_ALLOC_RETRY=0 HOS_GPU_ALLOC_TEST_FAIL=1: the full heap's failure, as shipped */
        reset(), room[0] = room[1] = 1 << 20;
        request(&c, H(0), 4);
        hos_oot_gpu_alloc(&c, game_gpu_alloc);
        CHECK(!(c.x[0] & 1) && game_calls == 0 && room[0] == 1 << 20 && room[1] == 1 << 20);
        CHECK(hos_oot_heap_refuse() && !hos_oot_heap_refuse());
        c.x[0] = 0;
        hos_oot_heap_retry(&c, H(0), 0x4198, 8, 0x80456910ull);
        CHECK(c.x[0] == 0 && room[1] == 1 << 20);
        puts("PASS: HOS_GPU_ALLOC_RETRY=0 with HOS_GPU_ALLOC_TEST_FAIL=1: the failure the game crashes on, not tried again");
        return 0;
    }
    if (!strcmp(mode, "refuse")) {   /* HOS_GPU_ALLOC_TEST_FAIL=2 */
        reset(), room[0] = room[1] = 1 << 20;
        for (int k = 0; k < 3; k++) {
            request(&c, H(0), 4);
            const uint64_t named = room[0], parent = room[1];
            hos_oot_gpu_alloc(&c, game_gpu_alloc);
            CHECK(c.x[0] & 1);
            if (k < 2) CHECK(room[0] == named && room[1] == parent - 0x400 && !sleeps);   /* (refused: made in the parent) */
            else CHECK(room[0] == named - 0x400 && game_calls == 1);                 /* (then the game's own again) */
        }
        CHECK(hos_oot_heap_refuse() && hos_oot_heap_refuse() && !hos_oot_heap_refuse());
        puts("PASS: HOS_GPU_ALLOC_TEST_FAIL=2: two refused by their heap without a call, made by the retry, then as shipped");
        return 0;
    }
    if (!strcmp(mode, "split")) {   /* HOS_GPU_ALLOC_TEST_FAIL=0,1: the model buffer alone */
        reset(), room[0] = 1 << 20;
        request(&c, H(0), 4);
        hos_oot_gpu_alloc(&c, game_gpu_alloc);
        CHECK((c.x[0] & 1) && game_calls == 1 && !logs);
        CHECK(hos_oot_heap_refuse() && !hos_oot_heap_refuse());
        puts("PASS: HOS_GPU_ALLOC_TEST_FAIL=0,1: the GPU memory as shipped, one model buffer refused");
        return 0;
    }

    /* made at once: the game's alone */
    reset(), room[0] = 1 << 20;
    request(&c, H(0), 4);
    hos_oot_gpu_alloc(&c, game_gpu_alloc);
    CHECK((c.x[0] & 1) && game_calls == 1 && !logs && !sleeps && recomp_load64(&c, REQ + 8) == H(0));

    /* full, its parent with room: made there at once, no sleep */
    reset(), room[1] = 1 << 20;
    request(&c, H(0), 4);
    hos_oot_gpu_alloc(&c, game_gpu_alloc);
    CHECK((c.x[0] & 1) && game_calls == 2 && !sleeps && room[1] == (1 << 20) - 0x400 && recomp_load64(&c, REQ + 8) == H(0));

    /* full, its parent too, the root has room: made there; the request names its heap again */
    reset(), room[2] = 1 << 20;
    request(&c, H(0), 4);
    hos_oot_gpu_alloc(&c, game_gpu_alloc);
    CHECK((c.x[0] & 1) && room[2] == (1 << 20) - 0x400 && recomp_load64(&c, REQ + 8) == H(0));
    CHECK(c.x[30] == 0x80456f1cull);

    /* no parent with room: the game's default heap */
    reset(), room[3] = 1 << 20;
    request(&c, H(0), 4);
    hos_oot_gpu_alloc(&c, game_gpu_alloc);
    CHECK((c.x[0] & 1) && room[3] == (1 << 20) - 0x400 && recomp_load64(&c, REQ + 8) == H(0));

    /* nothing has room: it fails, as before, and says so */
    reset();
    request(&c, H(0), 4);
    hos_oot_gpu_alloc(&c, game_gpu_alloc);
    CHECK(!(c.x[0] & 1) && logs >= 3 && recomp_load64(&c, REQ + 8) == H(0));
    CHECK(!sleeps);
    puts("PASS: GPU memory from a full heap: in a parent with room, in the default heap, or not at all; never a sleep");

    /* the CPU buffer: in the nearest parent with room; x0 the block, everything else as it was */
    reset(), room[2] = 1 << 20;
    memset(&c, 0, sizeof c);
    for (int r = 1; r < 31; r++) c.x[r] = 0x1000 + r;
    c.n = 1, c.z = 0, c.c = 1, c.v = 0;
    Ctx before = c;
    hos_oot_heap_retry(&c, H(0), 0x4198, 8, 0x80456910ull);
    CHECK(c.x[0] >= 0x80080000ull && room[2] == (1 << 20) - 0x4198);
    for (int r = 1; r < 31; r++) CHECK(c.x[r] == before.x[r]);
    CHECK(c.n == 1 && c.z == 0 && c.c == 1 && c.v == 0);
    reset(), room[1] = 0x4198 + 0x1000;   /* (a parent without the margin is passed over) */
    c = before;
    hos_oot_heap_retry(&c, H(0), 0x4198, 8, 0x80456910ull);
    CHECK(c.x[0] == 0 && room[1] == 0x4198 + 0x1000);
    reset(), room[1] = 1 << 20;
    c = before;
    hos_oot_heap_retry(&c, H(0), 0x4198, 8, 0x80456910ull);
    CHECK(c.x[0] && room[1] == (1 << 20) - 0x4198);
    reset(), room[1] = 1 << 20;   /* (the second site, the +168 buffer, the same way) */
    c = before;
    hos_oot_heap_retry(&c, H(0), 0x800, 8, 0x80456da4ull);
    CHECK(c.x[0] && room[1] == (1 << 20) - 0x800);
    CHECK(!hos_oot_heap_refuse());
    puts("PASS: the model buffer from a full heap: made in its nearest parent with room, x0 alone changed");
    return 0;
}
'''

# (lifted text shaped as the lift writes it: the four places' exact lines, and another caller of f_80522050)
LIFTED = '''#include "a64.h"
void f_80522050(Ctx *A64_NOESC);
void f_804566e0(Ctx *__restrict A64_NOESC c) {
    c->x[8]=recomp_load64(c,c->x[8] + 48);
{ uint64_t _t = c->x[8]; c->x[30] = 0x80456910ULL; A64_ICALL(c, _t); }
    recomp_store64_sp(c,c->x[27] + 200,c->x[0]);
{ uint64_t _t = c->x[8]; c->x[30] = 0x80456da4ULL; A64_ICALL(c, _t); }
    recomp_store64_sp(c,c->x[27] + 168,c->x[0]);
c->x[30] = 0x80456f1cULL; f_80522050(c);
c->x[30] = 0x80457098ULL; f_80522050(c);
}
void f_80dfe000(Ctx *__restrict A64_NOESC c) {
c->x[30] = 0x80dfe6a4ULL; f_80522050(c);
}
void f_80522050(Ctx *__restrict A64_NOESC c) {
    c->x[0] = 0x1ULL;
return;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    out, found = oot_patches.patch_heap_retry(LIFTED)
    assert found == [2, 2], found
    assert out.count("hos_oot_gpu_alloc(c, f_80522050)") == 2
    assert "c->x[30] = 0x80dfe6a4ULL; f_80522050(c);" in out, "another caller of f_80522050 was changed"
    assert "void f_80522050(Ctx *__restrict A64_NOESC c) {" in out, "f_80522050 itself was changed"
    for lr in ("0x80456910ULL", "0x80456da4ULL"):
        assert out.count("hos_oot_heap_retry(c, _hh, _hn, _ha, %s)" % lr) == 1, lr
    assert out.count(oot_patches.GPU_ALLOC_DECL) == 1 and out.count(oot_patches.HEAP_ALLOC_DECL) == 1
    assert oot_patches.patch_heap_retry(out) == (out, [2, 2]), "a second run changed the text"
    other = LIFTED.replace("0x80456910ULL", "0x80456914ULL").replace("0x80456da4ULL", "0x80456da8ULL") \
                  .replace("0x80456f1cULL", "0x80456f20ULL").replace("0x80457098ULL", "0x8045709cULL")
    assert oot_patches.patch_heap_retry(other) == (other, [0, 0]), "another version's text was edited"
    import inspect
    assert "patch_heap_retry(out) if protect_hearts else" in inspect.getsource(oot_patches.patch_file), \
        "the edits are made on builds other than the supported one"
    print("PASS: nx2/oot_patches.py: the four places edited once (f_80522050's other callers left), a second run changes "
          "nothing, other text is left alone, the supported build only")

    cc = shutil.which(a.cc) or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        print("SKIP: no C compiler")
        return
    with tempfile.TemporaryDirectory(prefix="heap-retry-") as tmp:
        tmp = Path(tmp)
        (tmp / "hos.h").write_text(HOS_H, encoding="utf-8")
        (tmp / "fixture.c").write_text(FIXTURE, encoding="utf-8")
        (tmp / "oot_heap_retry.c").write_text((ROOT / "host" / "oot_heap_retry.c").read_text(encoding="utf-8"),
                                              encoding="utf-8")
        exe = tmp / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), str(tmp / "fixture.c"),
                        str(tmp / "oot_heap_retry.c"), "-o", str(exe)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_GPU_ALLOC")}
        for mode, extra in (("", {}), ("off", {"HOS_GPU_ALLOC_RETRY": "0"}), ("refuse", {"HOS_GPU_ALLOC_TEST_FAIL": "2"}),
                            ("off-refuse", {"HOS_GPU_ALLOC_RETRY": "0", "HOS_GPU_ALLOC_TEST_FAIL": "1"}),
                            ("split", {"HOS_GPU_ALLOC_TEST_FAIL": "0,1"})):
            r = subprocess.run([str(exe), mode], env=dict(env, **extra), capture_output=True, text=True)
            print("\n".join(line for line in r.stdout.splitlines() if line.startswith("PASS")))
            if r.returncode:
                raise SystemExit("FAIL (%s):\n%s%s" % (mode or "retry", r.stdout, r.stderr))


if __name__ == "__main__":
    main()
