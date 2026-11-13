"""AcquireXDecoderOutputBuffer decodes without the virtual core and takes it again only after the decoder's lock is let go
(host/nn_video.c dec_acquire, issue 1929).

dec_acquire decodes the pending submissions with libavcodec, and dav1d waits there for its own frame threads: with the
core held, the movie thread kept it 100-270 ms on Linux, and a thread waiting for that core ran without one (vc_acquire's
escape). The actual dec_acquire, with the decoder's and pictures' types cut from the same file, is compiled with
stand-ins that record whether the thread holds its core while it decodes and whether it waits for a core while it holds
d->lock (as hos_cv_sleep, it must not: a thread holding a core may be waiting for that lock in dec_submit). Checks every
way out:

- a picture decoded and handed out; nothing decoded after the last submission; no free buffer; a decode that needs the
  2-ms retries (hos_sleep_ns, which gives up no core it does not hold): the decoding without the core, the core taken
  again after the lock, held when dec_acquire returns;
- a thread that holds no core when it calls dec_acquire takes none.

python -I -B tests/test_vcore_video_acquire.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def through(source, marker, last):
    """from marker to the end of the line holding last (after marker): a typedef with its name"""
    start = source.index(marker)
    end = source.index("\n", source.index(last, start))
    return source[start:end]


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct AVCodecContext AVCodecContext;
typedef struct AVPacket AVPacket;
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
static int64_t clock_ns = 1000000000;
static int64_t hos_now_ns(void) { return clock_ns; }
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)a, (void)n; return NULL; }

@TYPES@

/* the thread's virtual core, as hos_vcore_away / hos_vcore_back keep it (host/hos_thread.c) */
static Dec dec;
static int held, takes, takes_under_lock, decodes, decodes_with_core, sleeps, sleeps_with_core;
int hos_vcore_away(void) { if (!held) return 0; held = 0; return 1; }
void hos_vcore_back(void) {
    if (held) return;
    held = 1, takes++;
    if (dec.lock.locked) takes_under_lock++;
}
static void AcquireSRWLockExclusive(SRWLOCK *l) { l->locked = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { l->locked = 0; }
/* hos_sleep_ns as host/hos_thread.c has it: the core given up for the sleep, if one is held */
static int64_t sleep_takes_ns;   /* (what a sleep really takes, when not what was asked: a 2 ms sleep takes 4-5 ms) */
static void hos_sleep_ns(int64_t ns) {
    sleeps++;
    sleeps_with_core += held;
    const int away = hos_vcore_away();
    clock_ns += sleep_takes_ns ? sleep_takes_ns : ns;
    if (away) hos_vcore_back();
}
static Pool pool;
static void *host_of(uint64_t obj, uint32_t magic) {
    if (magic == DEC_MAGIC) return obj == 1 ? &dec : NULL;
    return magic == POOL_MAGIC && obj == 2 ? &pool : NULL;
}
static void pool_rgba(Pool *o) { (void)o; }
static void picture_free(Picture *p) { free(p->rgba), free(p->nv12); memset(p, 0, sizeof *p); }
/* the decoding (libavcodec and dav1d's waits in the real one): one picture per submission, after `late` calls */
static Pending one;
static int late, end_after;
static int stopped_now;   /* (the decoder was stopped or flushed and nothing was submitted since) */
static void decode_next(Dec *d) {
    decodes++;
    decodes_with_core += held;
    if (!d->phead) return;
    if (late > 0) { late--; return; }
    d->phead = NULL;
    if (end_after) { d->eos = 1; return; }
    Picture *p = &d->queue[(d->qhead + d->qn) % MAX_QUEUE];
    p->w = p->h = 4, p->pts = 100, p->last = 1;
    p->nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    d->qn++;
}
static void decode_pending(Dec *d) { decode_next(d); }

@ACQUIRE@

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
/* one AcquireXDecoderOutputBuffer(&index, &sync, decoder) by a thread that holds a core (or not) */
static uint64_t acquire(int core, int pending, int registered) {
    static uint32_t index;
    static uint64_t sync;
    for (int i = 0; i < dec.qn; i++) picture_free(&dec.queue[(dec.qhead + i) % MAX_QUEUE]);
    memset(&dec, 0, sizeof dec);
    if (!pool.nv12) pool.w = pool.h = 4, pool.count = 1, pool.nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    dec.slots[0] = registered ? 2 : 0;
    dec.phead = dec.ptail = pending ? &one : NULL;
    dec.stopped = stopped_now;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)&index, c.x[1] = (uint64_t)(uintptr_t)&sync, c.x[2] = 1;
    held = core, takes = takes_under_lock = decodes = decodes_with_core = sleeps = sleeps_with_core = 0;
    dec_acquire(&c);
    return c.x[0];
}
int main(void) {
    uint64_t r = acquire(1, 1, 1);
    check(r == 0 && dec.acquires == 1 && dec.qn == 0, "a picture decoded and handed out");
    check(decodes > 0 && decodes_with_core == 0, "the decoding runs without the core");
    check(takes == 1 && takes_under_lock == 0 && held == 1, "the core taken again once, after d->lock is let go");

    end_after = 1;
    r = acquire(1, 1, 1);
    check(r == 0 && dec.eos && decodes_with_core == 0, "nothing decoded after the last submission: decoded without the core");
    check(takes == 1 && takes_under_lock == 0 && held == 1, "and the core back after the lock");
    end_after = 0;

    r = acquire(1, 1, 0);
    check(r == RESULT_NO_OUTPUT && decodes_with_core == 0, "no free buffer: decoded without the core, no picture yet");
    check(takes == 1 && takes_under_lock == 0 && held == 1, "and the core back after the lock");

    late = 3;
    r = acquire(1, 1, 1);
    check(r == 0 && dec.acquires == 1 && sleeps == 3 && decodes_with_core == 0, "a decode after three 2-ms retries, without the core");
    check(sleeps_with_core == 0 && takes == 1 && takes_under_lock == 0 && held == 1,
          "no core taken for the retries' sleeps; taken once, after the lock");
    late = 0;

    r = acquire(0, 1, 1);
    check(r == 0 && held == 0 && takes == 0, "a thread that held no core takes none");

    /* the wait for a picture another thread's submission may bring: ~100 ms by the clock, whatever a sleep takes, and none
       after a Stop or a Flush (a movie's end: the thread its teardown joins sat here for 245-257 ms) */
    r = acquire(1, 0, 1);
    check(r == 0 && sleeps == 50 && sleeps_with_core == 0, "nothing decoded yet: 100 ms of 2-ms sleeps, then the buffer again");
    sleep_takes_ns = 5000000;
    const int64_t before = clock_ns;
    r = acquire(1, 0, 1);
    check(r == 0 && sleeps == 20 && clock_ns - before == 100000000, "sleeps that take 5 ms: still 100 ms, not 50 tries (250 ms)");
    stopped_now = 1;
    r = acquire(1, 0, 1);
    check(r == 0 && sleeps == 0 && dec.acquires == 1, "stopped and nothing submitted since: no wait, the buffer again at once");
    check(takes == 1 && takes_under_lock == 0 && held == 1, "and the core back after the lock");
    stopped_now = 0, sleep_takes_ns = 0;
    late = 3;
    r = acquire(1, 1, 1);
    check(r == 0 && sleeps == 3, "a pending submission is still waited for");
    late = 0;
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
    with tempfile.TemporaryDirectory(prefix="vcore-video-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([compiler, "-std=c++17", "-include", ENV_ON_H, "-O1", "-w", str(folder / "fixture.cpp"), "-o", str(exe)], check=True,
                       timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        if result.returncode or "ok" not in result.stdout:
            raise SystemExit("dec_acquire holds its virtual core through the decoding (issue 1929)")


if __name__ == "__main__":
    main()
