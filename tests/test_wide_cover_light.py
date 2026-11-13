"""Ultrawide's loading cover closes the picture's sides by scaling their light, not their codes (gpu/vk/shaders/fg_scale.comp
for a frame shown as it is, fg_copy.comp for frame generation's copy): the game blends its cover over the picture's light,
so under a cover that is 1 - k closed a pixel's light is k of its light without it at every brightness, and the sides have to
do the same to open in step with the middle. The shaders' own to_light(), to_code() and dimmed(), compiled as C++, against
the curves written out again in double precision: sRGB, and SMPTE ST 2084 for the game's own HDR10 picture (FG_SCALE_PQ in
fg_scale.comp, pc.pq in fg_copy.comp).

python -I -B tests/test_wide_cover_light.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"

# (just enough of GLSL for the three functions)
SHIM = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct vec3 {
    float r, g, b;
    vec3(float x, float y, float z) : r(x), g(y), b(z) {}
};
static float max(float a, float b) { return std::max(a, b); }
static float pow(float a, double b) { return std::pow(a, (float)b); }
static struct { int pq; } pc;
'''

FIXTURE = r'''
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s (pq %d)\n", __LINE__, #x, PQ); std::exit(1); } } while (0)
static double light(double c) {
    if (!PQ) return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    const double p = std::pow(c, 32.0 / 2523.0);
    return std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
}
static double code(double l) {
    if (!PQ) return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    const double y = std::pow(l, 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * y) / (1.0 + 2392.0 / 128.0 * y), 2523.0 / 32.0);
}
int main() {
    pc.pq = PQ;
    // (a code's step is 1/255 or 1/1023: the shader's single precision stays well inside it)
    const double tol = PQ ? 4e-4 : 1e-4;
    if (PQ) {   // (100, 1000 and 10,000 nits)
        CHECK(std::fabs(to_code(0.01f) - 0.5081) < 1e-3 && std::fabs(to_code(0.1f) - 0.7518) < 1e-3 && std::fabs(to_code(1.0f) - 1.0) < 1e-3);
    } else {
        CHECK(std::fabs(to_light(0.5f) - 0.21404) < 1e-4 && std::fabs(to_code(0.5f) - 0.73536) < 1e-4);
    }
    for (int i = 0; i <= 1023; i++) {
        const float c = (float)i / 1023.0f;
        CHECK(std::fabs(to_light(c) - light(c)) < (PQ ? 2e-3 * light(c) + 1e-7 : 1e-5));
        float last = -1.0f;
        for (int j = 0; j <= 64; j++) {
            const float k = (float)j / 64.0f;
            const vec3 d = dimmed(vec3(c, c, 1.0f - c), k);
            CHECK(std::isfinite(d.r) && d.r == d.g && d.r >= 0.0f && d.r <= c + tol);
            CHECK(std::fabs(d.r - code(light(c) * k)) < tol);                        // the light scaled, not the code
            CHECK(std::fabs(d.b - code(light(1.0 - (double)c) * k)) < tol);          // (each channel by itself)
            CHECK(d.r >= last - tol);                                                // opens steadily
            last = d.r;
            if (j == 0) CHECK(d.r < 1e-5f);                                          // closed: black (ST 2084's code for it is 7e-7)
            if (j == 64) CHECK(std::fabs(d.r - c) < tol);                            // open: the picture
            // what was measured in the game: the same fraction of the light at every brightness
            CHECK(std::fabs(light(d.r) - light(c) * k) < 0.02 * light(c) * k + 1e-5);
        }
    }
    // a half-open cover leaves more than half of a code: scaling the codes had the sides darker than the middle
    CHECK(dimmed(vec3(0.8f, 0.8f, 0.8f), 0.5f).r > 0.8f * 0.5f + 0.1f);
    std::printf("PASS: %s: dimmed() scales the light (%s)\n", NAME, PQ ? "SMPTE ST 2084" : "sRGB");
}
'''


def functions(text, start, end="vec3 dimmed("):
    """from the line with start to the end of dimmed()'s line"""
    i = text.index(start)
    j = text.index("\n", text.index(end, i)) + 1
    return text[i:j]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    scale = (SHADERS / "fg_scale.comp").read_text(encoding="utf-8")
    copy = (SHADERS / "fg_copy.comp").read_text(encoding="utf-8")
    scale_fn = functions(scale, "#ifdef FG_SCALE_PQ\n// (SMPTE ST 2084")
    copy_fn = functions(copy, "float to_light(")
    for marker, text, name in (("? dimmed(c, 1.0 - pc.cover) : c", scale, "fg_scale.comp pixel()"),
                               ("? dimmed(fill, 1.0 - pc.cover) : fill", scale, "fg_scale.comp side_fill()"),
                               ("? dimmed(c, 1.0 - pc.cover) : c", copy, "fg_copy.comp")):
        if marker not in text:
            raise SystemExit("%s does not close the sides with dimmed()" % name)
    if "* (1.0 - pc.cover)" in scale or "* (1.0 - pc.cover)" in copy:
        raise SystemExit("a code is scaled by the cover (not its light)")
    with tempfile.TemporaryDirectory(prefix="wide-cover-light-") as tmp:
        for name, fn, defines, pq in (("fg_scale.comp", scale_fn, [], 0), ("fg_scale.comp FG_SCALE_PQ", scale_fn, ["-DFG_SCALE_PQ"], 1),
                                      ("fg_copy.comp", copy_fn, [], 0), ("fg_copy.comp pq", copy_fn, [], 1)):
            cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
            cpp.write_text(SHIM + fn.replace("\nfloat ", "\nstatic float ").replace("\nvec3 ", "\nstatic vec3 ") +
                           '#define PQ %d\n#define NAME "%s"\n' % (pq, name) + FIXTURE, encoding="utf-8")
            subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                            *defines, str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
