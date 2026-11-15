"""A video decoder takes only so many submissions at a time (host/nn_video.c: the input limit).

OoT's movie player submits compressed pictures and takes decoded ones on one thread. The console's decoder refuses a
submission it has no room for, and the player then goes on with its pictures and submits again later. Here every
submission was taken, so the player put a whole movie in at once and, while it was busy with that, took a picture only
about every 105 ms: the movie's first seconds showed ten pictures a second and the catch-up dropped the rest.

The decoder's own code (the rule, dec_submit, dec_acquire with its catch-up, decode_next, dec_flush, the ready event) is
cut from the source and compiled with a stand-in for libavcodec that holds seven pictures back, as dav1d's frame threads
do. A player like the game's (each frame: submit until refused, take one picture, give the buffer back) plays a movie
of 600 pictures, one frame every 60th of a second:

- the rule: full with the limit's count of submissions waiting, decoded or not; never with no limit;
- with the limit, never more than 16 wait; every Acquire from the first gives the next picture, none is skipped, none
  waits for a picture (the decoder is never starved); the last submission is taken though the decoder is full, and the
  movie ends with its last picture marked;
- the submission-ready event is clear exactly while the decoder is full, and signalled again by the Acquire, by a Flush
  and at the decoder's end, whose event the next decoder gets;
- a refusal returns the result the game expects and changes nothing: no submission id is used up;
- a frame two seconds late: the catch-up skips through what waits and the movie is on its clock again within a
  second, which a limit below the decoder's delay cannot do without starving;
- HOS_VIDEO_INPUT_LIMIT=0: as before, the whole movie taken at once, the event never cleared;
- HOS_VIDEO_INPUT_LIMIT=4, under the pictures the decoder holds back: the first Acquire finds no picture and
  waits (why the limit is above the decoder's delay).

python -I -B tests/test_video_input_limit.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# (env_on, the switches' rule, before the code cut out of the sources)
ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def through(source, marker, last):
    """from marker to the end of the line holding last (after marker): a typedef with its name"""
    start = source.index(marker)
    end = source.index("\n", source.index(last, start))
    return source[start:end]


HARNESS = r"""
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct AVCodec { const char *name; };
typedef struct AVCodecContext { const AVCodec *codec; } AVCodecContext;
typedef struct AVPacket { uint8_t *data; int size; int64_t pts; } AVPacket;
typedef struct AVFrame AVFrame;
struct SwsContext;
typedef long long LONG64;
typedef struct { int locked; } SRWLOCK;
#define SRWLOCK_INIT {0}
typedef struct { uint64_t x[32]; } Ctx;
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(uintptr_t)(a))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define HOS_WRITTEN(p, n) ((void)0)
#define AVERROR(e) (-(e))
#define AV_INPUT_BUFFER_PADDING_SIZE 64
static volatile LONG64 submissions;
static LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 x, LONG64 cmp) { (void)x, (void)cmp; return *p; }
static int video_log(void) { return 0; }
static void hos_log(const char *f, ...) { (void)f; }
static int64_t clock_ns = 1000000000;
static int64_t hos_now_ns(void) { return clock_ns; }
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)n; return GP(a); }
static void AcquireSRWLockExclusive(SRWLOCK *l) { l->locked = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { l->locked = 0; }
static int hos_vcore_away(void) { return 0; }
static void hos_vcore_back(void) {}
static int sleeps;
static void hos_sleep_ns(int64_t ns) { sleeps++, clock_ns += ns; }
/* the system events (host/nn_os_sync.c): made from the system region, never freed */
static int event_state[16], events_made, clears, signals;
static uint64_t hos_sys_alloc(uint64_t size, uint64_t align) { (void)size, (void)align; return (uint64_t)++events_made; }
static void hos_system_event_init(uint64_t key, int autoclear) { (void)autoclear; event_state[key] = 0; }
static void hos_system_event_signal(uint64_t key) { event_state[key] = 1, signals++; }
static void hos_system_event_clear(uint64_t key) { event_state[key] = 0, clears++; }
static uint64_t ids;
static uint64_t submitted(void) { submissions++; return ++ids; }

@TYPES@

static Dec dec;
static Pool pool;
static void *host_of(uint64_t obj, uint32_t magic) {
    if (magic == DEC_MAGIC) return obj == 1 ? &dec : NULL;
    return magic == POOL_MAGIC && obj == 2 ? &pool : NULL;
}
static void pool_rgba(Pool *o) { (void)o; }
static void picture_free(Picture *p) { free(p->rgba), free(p->nv12); memset(p, 0, sizeof *p); }
static void movie_report(Dec *d) { (void)d; }
/* libavcodec's stand-in: a decoder that holds DELAY pictures back, as dav1d's frame threads do; the end lets them out
   (seven: with the one already decoded they fill the queue's eight places at the end) */
#define DELAY 7
static AVCodecContext context;
static int64_t held_pts[DELAY + 1];
static int nheld, out_pts_n;
static int64_t out_pts[DELAY + 1];
static AVPacket *av_packet_alloc(void) { return (AVPacket *)calloc(1, sizeof(AVPacket)); }
static int av_new_packet(AVPacket *p, int n) { p->data = (uint8_t *)malloc((size_t)n), p->size = n; return p->data ? 0 : -1; }
static void av_packet_free(AVPacket **p) { if (*p) free((*p)->data), free(*p); *p = NULL; }
static int avcodec_send_packet(AVCodecContext *x, AVPacket *p) {
    (void)x;
    if (!p) {
        for (int i = 0; i < nheld; i++) out_pts[out_pts_n++] = held_pts[i];
        nheld = 0;
        return 0;
    }
    held_pts[nheld++] = p->pts;
    if (nheld > DELAY) {
        out_pts[out_pts_n++] = held_pts[0];
        memmove(held_pts, held_pts + 1, sizeof held_pts[0] * (size_t)--nheld);
    }
    return 0;
}
static void avcodec_flush_buffers(AVCodecContext *x) { (void)x; nheld = out_pts_n = 0; }
static void mf_picture(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts) {}
static void drain(Dec *d) {
    for (int i = 0; i < out_pts_n; i++) {
        if (d->qn == MAX_QUEUE) { picture_free(&d->queue[d->qhead]); d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--; }
        Picture *p = &d->queue[(d->qhead + d->qn) % MAX_QUEUE];
        memset(p, 0, sizeof *p);
        p->w = p->h = 4, p->pts = out_pts[i];
        p->nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
        d->qn++;
    }
    out_pts_n = 0;
}

@CODE@

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
#define FRAME_NS 16666667
static uint8_t data[16];
static uint64_t submit(int64_t pts, int last, uint64_t *id) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)id, c.x[1] = 1, c.x[2] = (uint64_t)(uintptr_t)data, c.x[3] = sizeof data;
    c.x[4] = (uint64_t)pts, c.x[5] = (uint64_t)last;
    dec_submit(&c);
    return c.x[0];
}
static uint32_t slot;
static uint64_t acquire(void) {
    static uint64_t sync;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)&slot, c.x[1] = (uint64_t)(uintptr_t)&sync, c.x[2] = 1;
    dec_acquire(&c);
    return c.x[0];
}
static void release(uint32_t i) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = 1, c.x[1] = i;
    dec_release(&c);
}
static uint64_t ready(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = 1;
    return ready_event(&c);
}
static void fresh(void) {
    pending_free(&dec);
    for (int i = 0; i < dec.qn; i++) picture_free(&dec.queue[(dec.qhead + i) % MAX_QUEUE]);
    const uint64_t event = dec.ready;
    memset(&dec, 0, sizeof dec);
    dec.ctx = &context, dec.slots[0] = 2, dec.ready = event;
    if (event) hos_system_event_signal(event);
    if (!pool.nv12) pool.w = pool.h = 4, pool.count = 1, pool.nv12 = (uint8_t *)calloc(1, 4 * 4 * 3 / 2);
    nheld = out_pts_n = sleeps = 0;
}
/* the game's player: each frame it submits until it is refused (or the movie is all in), takes one picture and gives
   the buffer back. late_at: the frame that comes two seconds late (-1: none) */
static int most_waiting, wrong_event, taken, out_of_order, starved_frames, late_recovered;
static int64_t last_pts;
static void play(int pictures, int late_at) {
    const uint64_t event = ready();
    int next = 0;
    most_waiting = wrong_event = taken = out_of_order = starved_frames = 0, late_recovered = -1, last_pts = -1;
    for (int frame = 0; frame < pictures + 400 && !(dec.eos && !dec.qn && taken); frame++) {
        while (next < pictures) {
            uint64_t id = 0;
            const uint64_t before = ids;
            const uint64_t r = submit((int64_t)next * FRAME_NS, next == pictures - 1, &id);
            if (r == RESULT_INPUT_FULL) {
                if (ids != before || id) check(0, "a refused submission used a submission id");
                break;
            }
            if (r) { check(0, "a submission failed with another result"); return; }
            next++;
        }
        const int waiting = dec.npending + dec.qn;
        if (waiting > most_waiting) most_waiting = waiting;
        if (event_state[event] != !input_full(dec.npending, dec.qn, input_limit())) wrong_event++;
        const int slept = sleeps;
        const int64_t handed = dec.acquires;
        if (acquire() != 0) { check(0, "an Acquire failed"); return; }
        if (sleeps != slept) starved_frames++;
        if (dec.last_pts != last_pts || (dec.acquires != handed && !dec.eos && dec.last_pts > last_pts)) {
            if (dec.last_pts < last_pts) out_of_order++;
            if (dec.last_pts != last_pts) taken++;
            last_pts = dec.last_pts;
        }
        if (late_at >= 0 && frame > late_at && late_recovered < 0 &&
            (clock_ns - dec.clock_ns) - (dec.last_pts - dec.clock_pts) < 25000000) late_recovered = frame - late_at;
        if (event_state[event] != !input_full(dec.npending, dec.qn, input_limit())) wrong_event++;
        release(slot);
        clock_ns += frame == late_at ? 2000000000 : FRAME_NS;
    }
}
int main(int argc, char **argv) {
    const int limit = input_limit();
    if (argc > 1 && !strcmp(argv[1], "off")) {
        check(limit == 0, "HOS_VIDEO_INPUT_LIMIT=0 is no limit");
        fresh();
        play(600, -1);
        check(dec.refused == 0 && most_waiting >= 600, "no limit: the whole movie taken at once, as before");
        check(clears == 0 && wrong_event == 0, "no limit: the ready event never cleared");
        check(taken == 600 && dec.eos, "no limit: the movie plays to its end");
        if (!bad) puts("ok");
        return bad;
    }
    if (argc > 1 && !strcmp(argv[1], "small")) {
        check(limit == 4, "HOS_VIDEO_INPUT_LIMIT=4");
        fresh();
        play(600, -1);
        check(starved_frames > 0, "a limit under the decoder's delay starves the first Acquire (it waits for a picture)");
        if (!bad) puts("ok");
        return bad;
    }
    check(limit == INPUT_LIMIT && INPUT_LIMIT > DELAY, "the default limit, above the pictures a decoder holds back");
    check(!input_full(INPUT_LIMIT - 1, 0, INPUT_LIMIT) && input_full(INPUT_LIMIT, 0, INPUT_LIMIT), "full at the limit, not under it");
    check(input_full(INPUT_LIMIT - 3, 3, INPUT_LIMIT) && !input_full(INPUT_LIMIT - 4, 3, INPUT_LIMIT), "decoded pictures not taken count too");
    check(!input_full(100000, 8, 0), "no limit: never full");

    fresh();
    play(600, -1);
    /* (the stream's last submission and its end mark are always taken: two over the limit, once) */
    check(dec.refused > 0 && most_waiting <= INPUT_LIMIT + 2, "never more than the limit waiting");
    check(taken == 600 && out_of_order == 0 && dec.skipped == 0, "every picture taken, in order, none skipped");
    check(starved_frames == 0, "no Acquire waited for a picture: the decoder is never starved");
    check(dec.eos && dec.slot_last[slot], "the last submission taken though the decoder was full, the end marked");
    check(wrong_event == 0 && clears > 0, "the ready event clear exactly while the decoder is full");
    check(event_state[dec.ready] == 1, "and signalled when the movie has ended");

    /* a Flush while it is full (a movie stopped early): room again, the next submission taken */
    fresh();
    const uint64_t event = ready();
    uint64_t id = 0;
    int n = 0;
    while (n < 1000 && submit((int64_t)n * FRAME_NS, 0, &id) == 0) n++;
    check(n == INPUT_LIMIT && event_state[event] == 0 && dec.refused == 1, "full after the limit's count of submissions");
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = 1;
    dec_flush(&c, "Flush");
    check(dec.npending == 0 && event_state[event] == 1, "a Flush empties it: the event signalled again");
    check(submit(0, 0, &id) == 0 && dec.npending == 1, "and the next movie's first submission is taken");

    /* a frame two seconds late: the catch-up works through what waits, a limit's worth at a time */
    fresh();
    play(600, 100);
    check(dec.skipped > 100 && late_recovered >= 0 && late_recovered <= 60, "after a 2 s frame the movie is on its clock again within a second");
    check(starved_frames == 0 && out_of_order == 0 && dec.eos, "without starving the decoder, to the movie's end");

    /* the decoder's end gives its event back, signalled; the next decoder gets it */
    fresh();
    const uint64_t first = ready();
    for (n = 0; n < 1000 && submit(0, 0, &id) == 0; n++) {}
    check(event_state[first] == 0, "full: clear");
    const int made = events_made;
    ready_event_give_back(dec.ready);
    dec.ready = 0;
    check(event_state[first] == 1, "a decoder that ends leaves its event signalled");
    fresh();
    check(ready() == first && events_made == made, "the next decoder takes the same event: none is leaked per movie");
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
    defines = "\n".join(re.findall(r"^#define (?:RESULT_NO_OUTPUT|RESULT_INPUT_FULL|INPUT_LIMIT|POOL_MAGIC|DEC_MAGIC|MAX_SLOTS|"
                                   r"MAX_QUEUE|SYNC_IDLE_TAG|SYNC_IDLE_MASK)\b.*$", src, re.M))
    types = "\n".join((defines,
                       through(src, "typedef struct {\n    int w, h, format, count;", "} Pool;"),
                       through(src, "typedef struct { uint8_t *rgba, *nv12;", "} Picture;"),
                       through(src, "typedef struct Pending {", "} Pending;"),
                       through(src, "typedef void (*mf_av1_picture_fn)", "int64_t pts);"),
                       through(src, "typedef struct {\n    const char *name;", "} Av1Decoder;"),
                       through(src, "typedef struct {\n    AVCodecContext *ctx;", "} Dec;")))
    code = "\n".join((block(src, "static int input_limit(void) {"),
                      block(src, "static int input_full(int pending, int queued, int limit) {"),
                      block(src, "static void input_room(Dec *d) {"),
                      block(src, "static void pending_free(Dec *d) {"),
                      "static void decode_next(Dec *d);",
                      block(src, "static void decode_pending(Dec *d) {"),
                      block(src, "static void decode_next(Dec *d) {"),
                      block(src, "static void pending_push(Dec *d, AVPacket *pkt) {"),
                      block(src, "static int submit_refused(Dec *d, int last) {"),
                      block(src, "static void dec_submit(Ctx *c) {"),
                      block(src, "static void dec_flush(Ctx *c, const char *what) {"),
                      block(src, "static void dec_acquire(Ctx *c) {"),
                      block(src, "static void dec_release(Ctx *c) {"),
                      through(src, "static SRWLOCK ready_lock", "static int nready_spare;"),
                      block(src, "static void ready_event_give_back(uint64_t event) {"),
                      block(src, "static uint64_t ready_event_new(void) {"),
                      block(src, "static uint64_t ready_event(Ctx *c) {")))
    # (the source's own checks: the refusal comes before a submission id is used, and all three codecs ask the decoder's event)
    submit = block(src, "static void dec_submit(Ctx *c) {")
    if submit.index("RESULT_INPUT_FULL") > submit.index("submitted()"):
        raise SystemExit("dec_submit uses a submission id before it refuses")
    if src.count("{ RET(ready_event(c)); }") != 3:
        raise SystemExit("a codec's submission-ready event is not its decoder's")
    fixture = HARNESS.replace("@TYPES@", types).replace("@CODE@", code)
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="video-input-limit-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([compiler, "-std=c++17", "-include", ENV_ON_H, "-O1", "-w", str(folder / "fixture.cpp"), "-o",
                        str(exe)], check=True, timeout=120)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_VIDEO_")}
        for mode, value in (("", None), ("off", "0"), ("small", "4")):
            run_env = dict(env)
            if value is not None:
                run_env["HOS_VIDEO_INPUT_LIMIT"] = value
            result = subprocess.run([str(exe)] + ([mode] if mode else []), capture_output=True, text=True, timeout=60, env=run_env)
            print(result.stdout, end="")
            if result.returncode or "ok" not in result.stdout:
                raise SystemExit("the video decoder's input limit (%s)" % (mode or "default"))


if __name__ == "__main__":
    main()
