"""Above a 60 cap a movie's frame count grows by the frames' time, so a cutscene's cues stay with its movie (issue 1742).

The game's movie player counts its updates while a movie plays, and a cutscene that shows a movie places its narration, its
rumble and its end by that count. The movie's picture and sound follow the clock, so at 120 fps the cues came twice too early:
narration lines started over each other. nx2/oot_patches.py makes the count's step hos_movie_frames(player), and host/nn_vi.c
answers 1 while the host gives no frame time (a cap of 60: as shipped) and the frames' time in 60ths, the rest carried to the
player's next update, while it does (HOS_FPS_MOVIE=0: 1 then too, for comparisons). A game version whose code differs keeps
its own code and the build goes on with a warning.

python -I -B tests/test_movie_frame_count.py [--cc gcc]
"""
import argparse
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

# (the shape of the lifted update: the count loaded, one added, stored; the same lines in another function stay as they are)
STORE = "    recomp_store32(c,c->x[20] + 760,c->x[8]);\n"
LIFTED = ('#include "a64.h"\n'
          "void f_83b7c710(Ctx *__restrict A64_NOESC c) {\n"
          "    c->x[8] = 0x1ULL;\n" + oot_patches.MOVIE_OLD + STORE + "}\n"
          "void f_83b7c870(Ctx *__restrict A64_NOESC c) {\n" + oot_patches.MOVIE_OLD + STORE + "}\n")
HOST_ANCHOR = "uint32_t hos_movie_frames(uint64_t player) {"


def block(source, marker):
    if marker not in source:
        raise SystemExit("FAIL: host/nn_vi.c has no '%s'" % marker)
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* (the lock as a flag: taken once, given back once) */
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static void AcquireSRWLockExclusive(SRWLOCK *l) { CHECK(!*l); *l = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { CHECK(*l); *l = 0; }
volatile uint32_t hos_frame_seconds_bits;
@HOST@
/* (the lifted update with the patch, on a small guest: the count at +760 of the player) */
typedef struct { uint64_t x[31]; } Ctx;
static uint32_t guest[2][256];
static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c; return guest[a >> 12][(a & 0xfff) / 4]; }
static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { (void)c; guest[a >> 12][(a & 0xfff) / 4] = (uint32_t)v; }
static uint32_t update(int player) {
    Ctx ctx, *c = &ctx;
    memset(&ctx, 0, sizeof ctx);
    c->x[20] = (uint64_t)player << 12;
@LIFTED@
    return guest[player][760 / 4];
}
static void seconds(double s) {
    const float f = (float)s;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    hos_frame_seconds_bits = s > 0 ? bits : 0;
}
static int within(uint32_t got, uint32_t want) { return got + 1 >= want && got <= want + 1; }
int main(int argc, char **argv) {
    uint32_t n = 0, before;
    if (argc > 1 && !strcmp(argv[1], "off")) {   /* (HOS_FPS_MOVIE=0: the game's own count at any rate) */
        seconds(1.0 / 120);
        for (int i = 0; i < 600; i++) n = update(0);
        CHECK(n == 600);
        printf("PASS: off\n");
        return 0;
    }
    /* no frame time (a cap of 60): one an update, as the game counts */
    seconds(0);
    for (int i = 0; i < 600; i++) n = update(0);
    CHECK(n == 600);
    CHECK(hos_movie_frames(0x5000) == 1 && hos_movie_frames(0x5000) == 1);
    /* 120 fps: half a 60th an update, a whole one every second update */
    seconds(1.0 / 120);
    before = n;
    for (int i = 0; i < 600; i++) {
        const uint32_t was = n;
        n = update(0);
        CHECK(n - was <= 1);
    }
    CHECK(within(n - before, 300));
    /* 240 fps and 40 fps: a quarter and one and a half */
    seconds(1.0 / 240);
    before = n;
    for (int i = 0; i < 960; i++) n = update(0);
    CHECK(within(n - before, 240));
    seconds(1.0 / 40);
    before = n;
    for (int i = 0; i < 400; i++) {
        const uint32_t was = n;
        n = update(0);
        CHECK(n - was == 1 || n - was == 2);
    }
    CHECK(within(n - before, 600));
    /* 30 fps: cues advance 60 movie frames per second, with the picture and sound. */
    seconds(1.0 / 30);
    before = n;
    for (int i = 0; i < 300; i++) n = update(0);
    CHECK(within(n - before, 600));
    /* two movies at once: each its own count (one carry for both would give each half) */
    seconds(1.0 / 120);
    before = n;
    {
        const uint32_t other = update(1) - 0;   /* (its first update) */
        uint32_t m = other;
        for (int i = 0; i < 600; i++) n = update(0), m = update(1);
        CHECK(within(n - before, 300) && within(m - other, 300));
    }
    /* a movie that plays while others come and go keeps its count: 14 more that play with it and a new player every update,
       16 at a time, at 240 fps (places taken in the order they were made gave this one a new start every second update: a
       quarter of a 60th an update then counted 0 or 1 every two, 1200 only on average) */
    seconds(1.0 / 240);
    {
        uint32_t mine = 0, steady = 0;
        for (int i = 0; i < 4800; i++) {
            mine += hos_movie_frames(0x7000000);
            for (int k = 0; k < 14; k++) steady += hos_movie_frames(0x7100000 + (uint64_t)k * 0x1000);
            hos_movie_frames(0x8000000 + (uint64_t)i * 0x1000);
        }
        CHECK(within(mine, 1200) && steady + 14 >= 14 * 1200 && steady <= 14 * 1200 + 14);
    }
    seconds(1.0 / 120);
    /* more movies at once than the host keeps places for, each updated in turn: every update finds its player's place taken and
       starts from a drawn part of a 60th, so 480 updates of half a 60th give 240 on average, 11 either way as one standard
       deviation. Every count within 5 of those, for any number of players: with a new place started at 0 every count stood
       still, and with a fixed step between the starts one player's did among 233 (and 29 players gave 108 to 131 of 120) */
    {
        static const int counts[] = {9, 17, 29, 40, 233, 377};
        static uint32_t sum[377];
        uint32_t least = ~0u, most = 0;
        for (unsigned t = 0; t < sizeof counts / sizeof counts[0]; t++) {
            const int players = counts[t];
            memset(sum, 0, sizeof sum);
            for (int i = 0; i < 480; i++)
                for (int k = 0; k < players; k++) sum[k] += hos_movie_frames(0x100000 + (uint64_t)t * 0x1000000 + (uint64_t)k * 0x1000);
            for (int k = 0; k < players; k++) {
                CHECK(sum[k] >= 240 - 55 && sum[k] <= 240 + 55);
                least = sum[k] < least ? sum[k] : least, most = sum[k] > most ? sum[k] : most;
            }
        }
        printf("more players than places: %u to %u of 240\n", least, most);
    }
    before = n;
    for (int i = 0; i < 600; i++) n = update(0);
    CHECK(within(n - before, 300));
    /* back at a cap of 60: one an update again */
    seconds(0);
    before = n;
    for (int i = 0; i < 10; i++) n = update(0);
    CHECK(n - before == 10);
    printf("PASS\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    out = oot_patches.patch_movie_frames(LIFTED)
    check(out is not None and out.count("_b=hos_movie_frames(c->x[20])") == 1, "the count's step is not the host's, once")
    update = out[out.index("void f_83b7c710"):out.index("void f_83b7c870")]
    check("hos_movie_frames" in update and "hos_movie_frames" not in out[out.index("void f_83b7c870"):],
          "the same lines in another function were patched")
    check(out.startswith('#include "a64.h"\n' + oot_patches.MOVIE_DECL), "the lifted file does not declare the host's function")
    back = out.replace(oot_patches.MOVIE_DECL, "", 1).replace(oot_patches.MOVIE_NEW, oot_patches.MOVIE_OLD)
    check(back == LIFTED, "more than the count's step changed")
    check(oot_patches.patch_movie_frames(out) == out, "a second run changed a patched file")
    check(oot_patches.patch_movie_frames(LIFTED.replace("f_83b7c710", "f_83b7c714")) is None,
          "a file without the function was not left to the caller")
    for what, other in (("another step", LIFTED.replace("_b=1ULL", "_b=2ULL")),
                        ("another field", LIFTED.replace("+ 760", "+ 764")),
                        ("the lines twice", LIFTED.replace(STORE + "}\nvoid f_83b7c870", STORE + oot_patches.MOVIE_OLD + STORE + "}\nvoid f_83b7c870"))):
        check(oot_patches.patch_movie_frames(other) == other, "a function with %s was patched (another game version)" % what)

    with tempfile.TemporaryDirectory(prefix="movie-count-") as tmp:
        f = Path(tmp) / "code_000.c"
        f.write_text(LIFTED, encoding="utf-8", newline="")
        for run in ("first", "second"):   # (the second over the patched file: counted as made, nothing written)
            log = io.StringIO()
            with contextlib.redirect_stdout(log):
                oot_patches.apply(tmp)
            check("WARNING: the movie's frame count" not in log.getvalue(), "the %s run warns though the count is patched" % run)
            check(f.read_text(encoding="utf-8") == out, "the %s run did not leave the patched file" % run)
        other = LIFTED.replace("_b=1ULL", "_b=2ULL")
        f.write_text(other, encoding="utf-8", newline="")
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            oot_patches.apply(tmp)
        check("WARNING: the movie's frame count was found 0 times" in log.getvalue(), "no warning for a count that was not found")
        check(f.read_text(encoding="utf-8") == other, "another version's function was changed")

    vi = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    lifted = oot_patches.MOVIE_NEW + STORE
    fixture = FIXTURE.replace("@HOST@", block(vi, HOST_ANCHOR)).replace("@LIFTED@", lifted)
    with tempfile.TemporaryDirectory(prefix="movie-count-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture.exe"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}
        subprocess.run([str(exe)], env=env, check=True)
        subprocess.run([str(exe)], env=dict(env, HOS_FPS_MOVIE="1"), check=True)   # (read by its value: 1 is on)
        subprocess.run([str(exe), "off"], env=dict(env, HOS_FPS_MOVIE="0"), check=True)
        r = subprocess.run([str(exe), "off"], env=env, capture_output=True, text=True)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "the count is the game's own without HOS_FPS_MOVIE=0")
        # (the negative control: the game's own step of one an update fails the same checks at 120 fps)
        plain = Path(tmp) / "plain.c"
        plain.write_text(FIXTURE.replace("@HOST@", block(vi, HOST_ANCHOR)).replace("@LIFTED@", oot_patches.MOVIE_OLD + STORE),
                         encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(plain), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "the unpatched count passes the checks: they prove nothing")


if __name__ == "__main__":
    main()
