"""Frame generation's Auto: a frame for each of the display's refreshes, at its place between the game's frames.

The real auto_cap, multiplier and auto_slots of gpu/vk/vk_framegen.cpp, with the display's refresh set by the fixture:
45 fps on 60 Hz gives one refresh to two game frames in three and two to the third (60 shown a second, three of four
made, at 3/4, 1/2 and 1/4), 60 fps on 60 Hz one refresh a frame at the game's own frame, also with the frames
some ms early or late (a refresh within a quarter period of a game frame shows it), faster than the display some
game frames none, 45 fps on 120 Hz 120 a second; the most a game frame may be given is the refresh over 30 fps (2 to
4), a timeline outside the interval starts again at the game's frame, and display_refresh_mhz is declared where
vk_main.cpp defines it (in the unnamed namespace the shared library linked with it undefined).
python -I -B tests/test_framegen_auto.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREFIX = r'''#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
constexpr int kMaxMult = 8;
struct { int framegen = 1; } opts;
static int refresh_mhz = 60000;
int display_refresh_mhz() { return refresh_mhz; }
'''

MAIN = r'''
/* a second of game frames at fps on a display at hz: the refreshes given, and the made ones' places */
struct Run { int shown = 0, made = 0, most = 0, none = 0; float min_t = 1, max_t = 0; };
static Run second(double fps, int hz, int64_t jitter = 0) {
    refresh_mhz = hz * 1000;
    const int64_t period = (int64_t)(1e12 / refresh_mhz), frame = (int64_t)(1e9 / fps);
    int64_t next = 0, last = 1000000000, steady = last;
    uint32_t seed = 12345;
    Run r;
    for (int i = 0; i < (int)fps; i++) {
        steady += frame;
        seed = seed * 1664525u + 1013904223u;
        const int64_t now = steady + (jitter ? (int64_t)(seed >> 8) % (2 * jitter + 1) - jitter : 0);
        float t[kMaxMult];
        int64_t at[kMaxMult];
        const int n = auto_slots(last, now, period, next, t, at, auto_cap());
        for (int k = 0; k < n; k++) {
            CHECK(t[k] > 0 && t[k] <= 1.0001f);
            CHECK(at[k] > last && at[k] <= now);
            if (k && t[k] <= 0.999f) CHECK(at[k] - at[k - 1] > period * 3 / 4 && at[k] - at[k - 1] < period * 5 / 4);   /* a refresh apart (the game frame: its own time) */
            if (t[k] <= 0.999f) r.made++, r.min_t = std::min(r.min_t, t[k]), r.max_t = std::max(r.max_t, t[k]);
        }
        r.shown += n, r.most = std::max(r.most, n), r.none += n == 0;
        last = now;
    }
    return r;
}
int main() {
    refresh_mhz = 60000;
    CHECK(auto_cap() == 2 && multiplier() == 2);
    Run r = second(45, 60);
    CHECK(r.shown >= 59 && r.shown <= 61);                   /* the display's 60, not 2x's 90 */
    CHECK(r.most == 2 && r.none == 0);
    CHECK(r.made >= 43 && r.made <= 46);                     /* three of four made */
    CHECK(r.min_t > 0.2f && r.max_t < 0.8f);                 /* at 1/4, 1/2, 3/4 */
    r = second(60, 60);
    CHECK(r.shown >= 59 && r.shown <= 61 && r.most == 1);    /* none made, or one a frame at most */
    for (int64_t j : {200000, 1000000, 2000000}) {             /* the game's frames come early or late (present jitter) */
        r = second(60, 60, j);
        CHECK(r.made == 0 && r.none == 0 && r.most == 1);     /* each game frame shown once, as itself */
        r = second(45, 60, j);
        CHECK(r.shown >= 58 && r.shown <= 62 && r.none == 0);
    }
    r = second(75, 60);
    CHECK(r.shown >= 59 && r.shown <= 61 && r.none > 0);     /* faster than the display: some frames given none */
    refresh_mhz = 120000;
    CHECK(auto_cap() == 4);
    r = second(45, 120);
    CHECK(r.shown >= 117 && r.shown <= 121 && r.most == 3);   /* (the first frame starts the timeline: one) */
    refresh_mhz = 240000;
    CHECK(auto_cap() == 4);                                  /* the most is 4, whatever the refresh */
    r = second(30, 240);
    CHECK(r.shown >= 117 && r.shown <= 121 && r.most == 4);  /* capped: 4 of the 8 refreshes a frame */
    refresh_mhz = 0;
    CHECK(auto_cap() == 2);                                  /* unknown: as 60 Hz */

    int64_t next = 5;                                        /* a timeline behind the interval (a hitch) */
    float t[kMaxMult];
    int64_t at[kMaxMult];
    CHECK(auto_slots(1000, 2000, 300, next, t, at, 4) == 1 && at[0] == 2000 && t[0] == 1.0f && next == 2300);
    CHECK(auto_slots(2000, 2000, 300, next, t, at, 4) == 0); /* no interval */

    opts.framegen = 3;
    CHECK(multiplier() == 3);                                /* a fixed setting is not Auto's */
    opts.framegen = 0;
    CHECK(multiplier() == 1);
    /* a fixed multiplier: as many frames a game frame as the display shows, the asked one at most (the game keeps its rate) */
    const int64_t f60 = 16666667, f30 = 33333333, f45 = 22222222;
    CHECK(fixed_frames(8, 180, f60) == 3);                   /* 8x at 60 fps on 180 Hz: 3 shown, 2 made */
    CHECK(fixed_frames(8, 180, 15873016) == 3);              /* 63 fps: still 3 */
    CHECK(fixed_frames(2, 180, f60) == 2);                   /* below the display: as asked */
    CHECK(fixed_frames(8, 60, f60) == 1);                    /* at the refresh: the game's frame alone */
    CHECK(fixed_frames(4, 60, f30) == 2);                    /* 30 fps on 60 Hz: 2x */
    CHECK(fixed_frames(8, 144, f45) == 3);                   /* 45 fps on 144 Hz: 3 (135 a second) */
    CHECK(fixed_frames(8, 240, f30) == 8);                   /* 30 fps on 240 Hz: all 8 */
    CHECK(fixed_frames(8, 0, f60) == 8 && fixed_frames(8, 180, 0) == 8);   /* unknown refresh or frame time: as asked */
    CHECK(fixed_frames(1, 180, f60) == 1 && fixed_frames(0, 180, f60) == 1);
    std::printf("PASS: Auto gives each of the display's refreshes a frame, and no more; a fixed multiplier no more than shown\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_framegen.cpp").read_text(encoding="utf-8")
    decl = source.index("int display_refresh_mhz();")
    if source.find("namespace {") < decl:
        raise SystemExit("display_refresh_mhz is declared in the unnamed namespace: vk_main.cpp's is another function")
    fixture = PREFIX + "\n".join(block(source, m) for m in ("int auto_cap()", "int multiplier()", "int fixed_frames(", "int auto_slots(")) + MAIN
    with tempfile.TemporaryDirectory(prefix="framegen-auto-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
