"""The FPS counter's first sample is not the frames since launch divided by one second.

python -I -B tests/test_fps_rate.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    tick = source[source.index("void menu_tick()"):source.index("void menu_record(")]
    if "fps_take(" not in tick:
        raise SystemExit("the counter still divides the frames since launch by one second")
    fn = block(source, "bool fps_take(")
    fixture = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
''' + fn + r'''
int main() {
    const int64_t half = 500000000;
    int64_t next = 0, last_shown = 0, last_game = 0;
    double shown = -1, game = -1;
    /* turning the counter on after thousands of frames must not publish that total */
    CHECK(!fps_take(1000000000, 4000, 3900, &next, &last_shown, &last_game, &shown, &game));
    CHECK(shown == -1 && game == -1);
    CHECK(next == 1000000000 + half && last_shown == 4000 && last_game == 3900);
    CHECK(!fps_take(1000000000 + half - 1, 4010, 3910, &next, &last_shown, &last_game, &shown, &game));
    CHECK(last_shown == 4000);
    CHECK(fps_take(1000000000 + half, 4030, 3930, &next, &last_shown, &last_game, &shown, &game));
    CHECK(shown > 59.9 && shown < 60.1 && game > 59.9 && game < 60.1);
    /* a sample a half-second late covers the whole second, not another half-second */
    CHECK(fps_take(1000000000 + half * 3, 4090, 3990, &next, &last_shown, &last_game, &shown, &game));
    CHECK(shown > 59.9 && shown < 60.1 && game > 59.9 && game < 60.1);
    const double old = 4000 / 1.0;   /* the previous first sample */
    CHECK(old == 4000);
    std::printf("PASS: the first sample starts the interval, and the next one is a rate\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="fps-rate-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
