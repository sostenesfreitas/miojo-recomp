"""Game-free: the button prompts following a change while the game runs (host/prompts.c). put_runs over a copy of a game
file in a guest window: the game's own bytes come back where the runs before were, the new runs are written in their order
(a later one over an earlier), a run past the range found is left alone and counted, and every write is marked
(HOS_WRITTEN: the renderer uploads it again). Guarded (the layouts, whose offsets the game makes pointers where they lie):
a run over bytes the game changed is left alone, the rest written, and only those written are put back at the next change.
The textures' pool finalized (nvn_native.c's nvnMemoryPoolFinalize: hos_prompts_pool_gone): not written any more (put_runs
over a range let go writes nothing), it is let go under the lock a change is written under, and another pool's finalize
leaves it be.

python -I -B tests/test_prompts_live.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def span(text, first, last):
    """from first to the end of the block that starts at last"""
    start = text.index(first)
    return text[start:text.index(last, start)] + block(text[text.index(last, start):], last)


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PATH 260
typedef long LONG;
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static void hos_log(const char *fmt, ...) { (void)fmt; }
enum { WINDOW = 1 << 20 };
static uint8_t window[WINDOW];   /* (guest addresses: offsets into it, from 0x1000) */
static void *hos_guest_range(uint64_t a, uint64_t n) {
    return a >= 0x1000 && a < WINDOW && n <= WINDOW - a ? window + a : NULL;
}
static size_t marked, marked_bytes;
#define HOS_WRITTEN(p, n) (marked++, marked_bytes += (n))
%(run)s
%(live)s
%(keep)s
%(put)s
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%%d %%s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    (void)argc;
    /* the game's file: 4096 bytes of i * 7; the guest's copy at 0x2000 as the start left it, a run of 0xAA at 100 (16) */
    enum { N = 4096, AT = 0x2000 };
    uint8_t game[N];
    for (int i = 0; i < N; i++) game[i] = (uint8_t)(i * 7);
    FILE *f = fopen(argv[1], "wb");
    fwrite(game, 1, N, f);
    fclose(f);
    memcpy(window + AT, game, N);
    memset(window + AT + 100, 0xAA, 16);
    static Live l;
    snprintf(l.path, sizeof l.path, "%%s", argv[1]);
    l.base = AT, l.lo = AT, l.hi = AT + N;
    const uint8_t aa[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    Run before[1] = {{100, 16, 0, aa}};
    keep_ranges(&l, before, 1);
    /* the change: 0xBB at 200 (8), then 0xCC at 204 (8) over its end, and a run past the file's range */
    const uint8_t bb[8] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB}, cc[8] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    Run now[3] = {{200, 8, 0, bb}, {204, 8, 1, cc}, {N + 10, 8, 2, bb}};
    size_t written, put_back;
    const size_t outside = put_runs(&l, now, 3, &written, &put_back);
    CHECK(outside == 1 && written == 2 && put_back == 1);
    CHECK(!memcmp(window + AT + 100, game + 100, 16));   /* (the game's own bytes back) */
    CHECK(!memcmp(window + AT + 200, bb, 4) && !memcmp(window + AT + 204, cc, 8));   /* (the later run over the earlier) */
    CHECK(!memcmp(window + AT, game, 100) && !memcmp(window + AT + 212, game + 212, N - 212));
    CHECK(window[AT + N + 10] == 0);   /* (past the range: not written) */
    CHECK(marked == 3 && marked_bytes == 16 + 8 + 8);
    CHECK(l.nranges == 2 && l.ranges[0][0] == 200 && l.ranges[1][0] == 204);   /* (the one not written: not kept) */
    /* guarded: the game made a pointer at 300 (8 bytes); a run over it is left alone, one beside it written */
    l.guarded = 1;
    memset(window + AT + 300, 0x7f, 8);
    Run guarded[2] = {{296, 8, 0, bb}, {400, 8, 1, cc}};
    CHECK(put_runs(&l, guarded, 2, &written, &put_back) == 1 && written == 1 && put_back == 2);
    CHECK(!memcmp(window + AT + 200, game + 200, 12));   /* (the change before: the game's bytes back) */
    CHECK(window[AT + 296] == game[296] && window[AT + 300] == 0x7f && !memcmp(window + AT + 400, cc, 8));
    CHECK(l.nranges == 1 && l.ranges[0][0] == 400);
    /* the next change puts back only what was written: the game's pointer stays */
    Run none[1] = {{N + 10, 8, 0, bb}};
    CHECK(put_runs(&l, none, 1, &written, &put_back) == 1 && put_back == 1 && window[AT + 300] == 0x7f &&
          !memcmp(window + AT + 400, game + 400, 8));
    /* the memory let go (base 0, hos_prompts_pool_gone): nothing written, not even at a run's offset from 0 */
    uint8_t before_all[WINDOW / 16];
    memcpy(before_all, window, sizeof before_all);
    l.base = 0;
    Run low[1] = {{0x1000, 8, 0, bb}};
    CHECK(put_runs(&l, low, 1, &written, &put_back) == 0 && written == 0 && put_back == 0);
    CHECK(!memcmp(before_all, window, sizeof before_all));
    puts("ok");
    return 0;
}
'''

GONE = r'''
#include <stdint.h>
static void hos_log(const char *fmt, ...) { (void)fmt; }
typedef int SRWLOCK;
static SRWLOCK tex_lock;
static int held, taken;
static void AcquireSRWLockExclusive(SRWLOCK *l) { held += l == &tex_lock, taken += l == &tex_lock; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { held -= l == &tex_lock; }
static struct { struct { volatile uint64_t base; uint64_t lo, hi; } l; } tex;
%(gone)s
int main(void) {
    tex.l.lo = 0x5000, tex.l.hi = 0x9000, tex.l.base = 0x4f00;
    hos_prompts_pool_gone(0x6000);   /* (another pool) */
    if (tex.l.base != 0x4f00) return 1;
    hos_prompts_pool_gone(0x5000);
    return tex.l.base != 0 ? 2 : held || !taken ? 3 : 0;
}
'''


def pool_gone(cc, src, tmp):
    nvn = (ROOT / "host" / "nvn_native.c").read_text(encoding="utf-8")
    check("hos_prompts_pool_gone(o->mem);" in block(nvn, "NV(nvnMemoryPoolFinalize)"),
          "nvnMemoryPoolFinalize does not tell the prompts a pool is gone")
    c, exe = Path(tmp) / "gone.c", Path(tmp) / "gone"
    c.write_text(GONE % {"gone": block(src, "void hos_prompts_pool_gone(")}, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)]).returncode
    check(r == 0, "hos_prompts_pool_gone: %s" % {1: "another pool's finalize dropped the textures' pool",
                                                   2: "the textures' pool finalized is still written",
                                                   3: "the pool let go outside tex_lock (a change being written does not wait)"}.get(r, r))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    parts = {
        "run": src[src.index("typedef struct { uint32_t off, len, idx; const uint8_t *data; } Run;"):].split("\n", 1)[0],
        "live": span(src, "typedef struct {\n    char path[MAX_PATH];", "typedef struct {") .rsplit("}", 1)[0] + "} Live;",
        "keep": block(src, "static void keep_ranges("),
        "put": block(src, "static size_t put_runs("),
    }
    harness = HARNESS % parts
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "live.c", Path(tmp) / "live"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(c), "-o", str(exe)],
                       check=True)
        out = subprocess.run([str(exe), str(Path(tmp) / "game.bin")], capture_output=True, text=True)
        pool_gone(a.cc, src, tmp)
    check(out.returncode == 0 and out.stdout.strip() == "ok", "exit %d: %s%s" % (out.returncode, out.stdout, out.stderr))
    print("ok: put_runs puts the game's bytes back, writes the runs in order, leaves one past the range; guarded, never over "
          "bytes the game changed; the textures' pool finalized is not written, under its lock")


if __name__ == "__main__":
    main()
