"""A movie whose pictures take longer to decode than they are apart is shown, not skipped away (host/nn_video.c).

dec_acquire used to decode the next packet whenever the picture in hand was late and the queue held only that one, so it
could skip it. The clock is the wall clock: a decode slower than the gap between pictures makes the next acquire later,
and the picture just decoded is thrown away. On a Windows PC (Intel UHD, four logical processors) the opening movie
decoded in 19.5 ms a picture, 16.7 ms apart, and 1933 of 2221 pictures were skipped. A decoder faster than the pictures
still decodes ahead and skips, which is what keeps the picture with the sound under 60 fps (issues 146 and 179).
HOS_VIDEO_NO_CATCHUP still skips nothing.

The decode time is the recent pictures' (recent_ns), not the movie's mean: a slow first decode (426 ms there) no longer
turns the catch-up off for a decoder that keeps up now. Every acquire waits ~100 ms for a picture, the first too: a
longer first wait did not show the first picture (HumanBean's review of #252).

python -I -B tests/test_video_catchup.py [--cxx clang++] [--source host/nn_video.c]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")
RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def through(source, marker, last):
    start = source.index(marker)
    end = source.index("\n", source.index(last, start))
    return source[start:end]


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct AVCodecContext AVCodecContext;
struct AVPacket { int unused; };
typedef struct AVFrame AVFrame;
struct SwsContext;
typedef long long LONG64;
typedef struct { int locked; } SRWLOCK;
typedef struct { uint64_t x[32]; } Ctx;
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(uintptr_t)(a))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define HOS_WRITTEN(p, n) ((void)0)
static LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 x, LONG64 cmp) { (void)x, (void)cmp; return *p; }
static int video_log(void) { return 0; }
static void hos_log(const char *f, ...) { (void)f; }
static int64_t clock_ns = 0;
static int64_t hos_now_ns(void) { return clock_ns; }
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)a, (void)n; return NULL; }
static int hos_vcore_away(void) { return 0; }
static void hos_vcore_back(void) {}
static void AcquireSRWLockExclusive(SRWLOCK *l) { l->locked = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { l->locked = 0; }
static int sleeps;
static void hos_sleep_ns(int64_t ns) { sleeps++; clock_ns += ns; (void)ns; }

@TYPES@

static void picture_free(Picture *p) { free(p->rgba), free(p->nv12); memset(p, 0, sizeof *p); }
static void pool_rgba(Pool *o) { (void)o; }

static Dec dec;
static Pool pool;
static Pending pending;
static AVPacket packet;
static int decode_calls;
static void *host_of(uint64_t obj, uint32_t magic) {
    if (magic == DEC_MAGIC) return obj == 1 ? &dec : NULL;
    return magic == POOL_MAGIC && obj == 2 ? &pool : NULL;
}
static void decode_next(Dec *d) {
    decode_calls++;
    if (!d->phead) return;
    d->phead = NULL;
    Picture *p = &d->queue[(d->qhead + d->qn) % MAX_QUEUE];
    p->w = p->h = 4, p->pts = 1000000000, p->last = 0;
    p->nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    d->qn++;
}
static void decode_pending(Dec *d) { (void)d; }
static void input_room(Dec *d) { (void)d; }

@ACQUIRE@

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
static void reset(void) {
    for (int i = 0; i < MAX_QUEUE; i++) picture_free(&dec.queue[i]);
    memset(&dec, 0, sizeof dec);
    if (!pool.nv12) pool.w = pool.h = 4, pool.count = 1, pool.nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    dec.slots[0] = 2;
    sleeps = decode_calls = 0;
}
static uint64_t acquire(void) {
    static uint32_t index;
    static uint64_t sync;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)&index, c.x[1] = (uint64_t)(uintptr_t)&sync, c.x[2] = 1;
    dec_acquire(&c);
    return c.x[0];
}
int main(void) {
    reset();
    clock_ns = 1000000000;
    uint64_t r = acquire();
    check(r == 0 && sleeps == 50, "the first acquire waits ~100 ms for a picture, as every other");
    dec.busy[0] = 0;
    r = acquire();
    check(r == 0 && sleeps == 100, "an acquire after the first waits ~100 ms");

    reset();
    clock_ns = 200000000;
    dec.clock_set = 1, dec.clock_ns = 0, dec.clock_pts = 0;
    dec.queue[0].w = dec.queue[0].h = 4, dec.queue[0].pts = 16666667, dec.queue[0].last = 0;
    dec.queue[0].nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    dec.qn = 1, dec.decoded = 8, dec.first_pts = 0, dec.end_pts = 7 * 16666667, dec.recent_ns = 20000000;
    pending.pkt = &packet, dec.phead = &pending;
    r = acquire();
    check(r == 0 && decode_calls == 0 && dec.last_pts == 16666667 && dec.skipped == 0,
          "a decoder slower than the pictures shows the late one instead of decoding in order to skip it");

    reset();
    clock_ns = 200000000;
    dec.clock_set = 1, dec.clock_ns = 0, dec.clock_pts = 0;
    dec.queue[0].w = dec.queue[0].h = 4, dec.queue[0].pts = 16666667, dec.queue[0].last = 0;
    dec.queue[0].nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    dec.qn = 1, dec.decoded = 8, dec.first_pts = 0, dec.end_pts = 7 * 16666667;
    dec.decode_ns = 426000000 + 7 * 5000000, dec.recent_ns = 5000000;   /* (a slow first decode, fast ones since) */
    pending.pkt = &packet, pending.next = NULL, dec.phead = &pending;
    r = acquire();
    check(r == 0 && decode_calls == 1 && dec.skipped >= 1 && dec.last_pts == 1000000000,
          "a slow first decode does not turn the catch-up off once the decoder keeps up");

    reset();
    clock_ns = 200000000;
    dec.clock_set = 1, dec.clock_ns = 0, dec.clock_pts = 0;
    dec.queue[0].w = dec.queue[0].h = 4, dec.queue[0].pts = 16666667, dec.queue[0].last = 0;
    dec.queue[0].nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    dec.qn = 1, dec.decoded = 8, dec.first_pts = 0, dec.end_pts = 7 * 16666667, dec.recent_ns = 5000000;
    pending.pkt = &packet, pending.next = NULL, dec.phead = &pending;
    r = acquire();
    check(r == 0 && decode_calls == 1 && dec.skipped >= 1 && dec.last_pts == 1000000000,
          "a faster decoder still decodes ahead and skips the late picture");
    if (!bad) puts("ok");
    return bad;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_video.c")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    defines = "\n".join(re.findall(r"^#define (?:RESULT_NO_OUTPUT|POOL_MAGIC|DEC_MAGIC|MAX_SLOTS|MAX_QUEUE|SYNC_IDLE_TAG|"
                                   r"SYNC_IDLE_MASK)\b.*$", src, re.M))
    types = "\n".join((defines,
                       through(src, "typedef struct {\n    int w, h, format, count;", "} Pool;"),
                       through(src, "typedef struct { uint8_t *rgba, *nv12;", "} Picture;"),
                       through(src, "typedef struct Pending {", "} Pending;"),
                       through(src, "typedef void (*mf_av1_picture_fn)", "int64_t pts);"),
                       through(src, "typedef struct {\n    const char *name;", "} Av1Decoder;"),
                       through(src, "typedef struct {\n    AVCodecContext *ctx;", "} Dec;"),
                       "static volatile LONG64 submissions;"))
    fixture = HARNESS.replace("@TYPES@", types).replace("@ACQUIRE@", block(src, "static void dec_acquire(Ctx *c) {"))
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="video-catchup-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([compiler, "-std=c++17", "-include", ENV_ON_H, "-O1", "-w",
                        str(folder / "fixture.cpp"), "-o", str(exe)], check=True, timeout=120)
        env = os.environ.copy()
        env.pop("HOS_VIDEO_NO_CATCHUP", None)
        env.pop("HOS_VIDEO_LOG", None)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=env)
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="")
        if result.returncode or "ok" not in result.stdout:
            raise SystemExit("a slow movie is still skipped, or a decoder that keeps up does not catch up")


if __name__ == "__main__":
    main()
