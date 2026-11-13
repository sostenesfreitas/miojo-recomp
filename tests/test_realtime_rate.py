"""Frame-time jitter must not make the real-time game clock run fast."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    if "realtime_rate(" not in block(source, "static void realtime_step("):
        raise SystemExit("the present path does not use the tested clock quantizer")
    fixture = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); exit(1); } } while(0)
'''
    fixture += block(source, "static int realtime_rate(")
    fixture += r'''
int main(void) {
    double carry = 0, elapsed = 0, game = 0;
    // The old clamp erased the short frame's debt and sped this clock up by ~8%.
    for (int i = 0; i < 6000; i++) {
        double dt = i % 2 ? 1.0 / 30.0 - 0.014 : 0.014;
        int fps = realtime_rate(dt * 60, 60, &carry);
        CHECK(fps >= 15 && fps <= 60);
        elapsed += dt; game += 1.0 / fps;
    }
    CHECK(fabs(game - elapsed) <= 1.0 / 60 + 1e-8);
    // Integer-rate rounding retains elapsed time at a sustained non-integer rate.
    carry = elapsed = game = 0;
    for (int i = 0; i < 6000; i++) {
        double dt = 1.0 / 47.3;
        int fps = realtime_rate(dt * 60, 60, &carry);
        elapsed += dt; game += 1.0 / fps;
    }
    CHECK(fabs(game - elapsed) < 0.001);
    // A slow frame followed by shorter ones does not leave extra game time behind.
    carry = elapsed = game = 0;
    for (int i = 0; i < 6000; i++) {
        double dt = i % 4 == 0 ? 1.0 / 30.0 : 1.0 / 90.0;
        int fps = realtime_rate(dt * 60, 60, &carry);
        elapsed += dt; game += 1.0 / fps;
    }
    CHECK(fabs(game - elapsed) <= 1.0 / 60 + 1e-8);
    // High display caps use the same debt accounting and do not force a 60 Hz step.
    carry = 0;
    CHECK(realtime_rate(0.5, 120, &carry) == 120);
    CHECK(fabs(carry) < 1e-12);
    // The default five-frame window must also preserve time for both jitter patterns.
    for (int pattern = 0; pattern < 2; pattern++) {
        double samples[5] = {0};
        int count = 0;
        carry = elapsed = game = 0;
        for (int i = 0; i < 6000; i++) {
            const double dt = pattern ? (i % 4 == 0 ? 1.0 / 30 : 1.0 / 90)
                                      : (i % 2 ? 1.0 / 30 - 0.014 : 0.014);
            samples[i % 5] = dt;
            if (count < 5) count++;
            double sum = 0;
            for (int j = 0; j < count; j++) sum += samples[j];
            const int fps = realtime_rate(sum / count * 60, 60, &carry);
            CHECK(fps >= 15 && fps <= 60 && carry >= -4.0);
            elapsed += dt; game += 1.0 / fps;
        }
        CHECK(fabs(game - elapsed) <= 1.0 / 30 + 1e-8);
        printf("default window pattern %d: elapsed %.9f s, game %.9f s\n", pattern, elapsed, game);
    }
    // Ten minutes of unpaced 120 Hz presents at a 60 cap must not accumulate minutes of debt.
    carry = 0;
    for (int i = 0; i < 72000; i++) {
        CHECK(realtime_rate(0.5, 60, &carry) == 60);
        CHECK(carry >= -4.0);
    }
    CHECK(carry == -4.0);
    int recovered = 0;
    for (int i = 0; i < 10; i++) {
        if (realtime_rate(1.5, 60, &carry) == 40) { recovered = 1; break; }
    }
    CHECK(recovered);
    // A long stall drops excess time instead of scheduling an oversized catch-up.
    carry = 0;
    CHECK(realtime_rate(12.0, 60, &carry) == 15);
    CHECK(fabs(carry) < 1e-12);
    CHECK(realtime_rate(1.0, 60, &carry) == 60);
    puts("PASS: jitter preserves elapsed time; slow and high-rate steps stay bounded");
}
'''
    with tempfile.TemporaryDirectory(prefix="realtime-rate-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
