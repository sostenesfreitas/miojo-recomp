"""A stall under a few seconds is mixed out, so the sound catches the picture.

A wake more than 100 ms late used to move the mixer's deadline to now. Those frames were never mixed, and the
sound stayed behind by that much.

python -I -B tests/test_audio_deadline.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "host" / "nn_audio.c").read_text(encoding="utf-8")
    loop = source[source.index("static DWORD WINAPI renderer_main"):source.index("/* ---- renderer lifecycle")]
    if "audio_mix_deadline(" not in loop or "100000000" in loop:
        raise SystemExit("the mixer still drops the frames it woke late for")
    fn = block(source, "int64_t audio_mix_deadline(")
    fixture = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
''' + fn + r'''
/* frames mixed before the deadline is in the future again, with `now` held still */
static int caught_up(int64_t late) {
    const int64_t period = 5000000, start = 1000000000, now = start + late;
    int64_t next = start;
    int n = 0;
    while (next <= now && n < 10000) {
        next = audio_mix_deadline(now, next, period);
        n++;
    }
    return n;
}
int main() {
    const int64_t period = 5000000, start = 1000000000;
    /* 40 ms late: still on the old grid, nine frames (the last one lands on now) */
    CHECK(audio_mix_deadline(start + 40000000, start, period) == start + period);
    CHECK(caught_up(40000000) == 9);
    /* 150 ms late: the old 100 ms jump mixed one frame and left the sound behind */
    CHECK(caught_up(150000000) == 31);
    /* a four-second pause is not mixed out */
    CHECK(caught_up(4000000000LL) == 1);
    CHECK(audio_mix_deadline(start + 4000000000LL, start, period) == start + 4000000000LL + period);
    std::printf("PASS: a hitch is mixed out, a long pause is not\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="audio-deadline-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
