"""The window pass's Sharpness (gpu/vk/shaders/fg_scale.comp): its neighbours are one picture (source) pixel apart, not one
window pixel (inside the same picture pixel when the window is the larger, so Sharpness did next to nothing), and its
per-pixel math, the shader's own sharpen() compiled as C++, is the picture unchanged at 0, never past the lightest or
darkest pixel around it nor outside 0..1 (no halos, no clipping) at any setting, and steeper edges as the setting rises.
fg_scale_comp_spv.h names the hash of the fg_scale.comp it was made from (its first line), checked against the shader as it
is now with or without glslang; with glslangValidator the shader compiles, and with the version the header names, the rest of
the header is its output byte for byte. --write makes the header: glslang's output with that line on top. The same for
fg_scale_pq_comp_spv.h, fg_scale.comp compiled with FG_SCALE_PQ (its destination RGB10A2, for the game's own HDR10 picture:
Native HDR), whose destination and dither this checks too.

python -I -B tests/test_fg_scale_sharpen.py [--cxx g++] [--glslang glslangValidator] [--write]
"""
import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"


# (the headers made from fg_scale.comp: file, variable, glslang's defines)
HEADERS = (("fg_scale_comp_spv.h", "FG_SCALE_COMP_SPV", []),
           ("fg_scale_pq_comp_spv.h", "FG_SCALE_PQ_COMP_SPV", ["-DFG_SCALE_PQ"]))


def source_line(comp, var="FG_SCALE_COMP_SPV", defines=()):
    """the header's first line: the shader's hash (line ends as LF, as the checkout has them) and how the header is made"""
    digest = hashlib.sha256(comp.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    return ("// fg_scale.comp sha256 %s (tests/test_fg_scale_sharpen.py --write: glslangValidator -V --target-env vulkan1.2 "
            "%s--vn %s)\n" % (digest, "".join(d + " " for d in defines), var)).encode()


def compile_header(glslang, comp, out, var="FG_SCALE_COMP_SPV", defines=()):
    r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.2", *defines, "--vn", var, "-o", str(out), str(comp)],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("fg_scale.comp does not compile (%s):\n" % (" ".join(defines) or "as it is") + r.stdout + r.stderr)
    return out.read_bytes()


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


# (just enough of GLSL's vec3 for sharpen())
SHIM = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct vec3 {
    float r, g, b;
    vec3() : r(0), g(0), b(0) {}
    explicit vec3(float v) : r(v), g(v), b(v) {}
    vec3(float x, float y, float z) : r(x), g(y), b(z) {}
};
static vec3 map2(vec3 a, vec3 b, float (*f)(float, float)) { return {f(a.r, b.r), f(a.g, b.g), f(a.b, b.b)}; }
static float fadd(float a, float b) { return a + b; }
static float fsub(float a, float b) { return a - b; }
static float fmul(float a, float b) { return a * b; }
static float fdiv(float a, float b) { return a / b; }
static float fmin_(float a, float b) { return std::min(a, b); }
static float fmax_(float a, float b) { return std::max(a, b); }
static vec3 operator+(vec3 a, vec3 b) { return map2(a, b, fadd); }
static vec3 operator-(vec3 a, vec3 b) { return map2(a, b, fsub); }
static vec3 operator*(vec3 a, vec3 b) { return map2(a, b, fmul); }
static vec3 operator/(vec3 a, vec3 b) { return map2(a, b, fdiv); }
static vec3 operator+(float a, vec3 b) { return vec3(a) + b; }
static vec3 operator-(float a, vec3 b) { return vec3(a) - b; }
static vec3 operator*(float a, vec3 b) { return vec3(a) * b; }
static vec3 operator/(vec3 a, float b) { return a / vec3(b); }
static vec3 min(vec3 a, vec3 b) { return map2(a, b, fmin_); }
static vec3 max(vec3 a, vec3 b) { return map2(a, b, fmax_); }
static float min(float a, float b) { return std::min(a, b); }
static float max(float a, float b) { return std::max(a, b); }
static float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
static vec3 clamp(vec3 v, vec3 lo, vec3 hi) { return min(max(v, lo), hi); }
'''

FIXTURE = r'''
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
static unsigned seed = 12345;
static float rnd() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
static vec3 rnd3() { return {rnd(), rnd(), rnd()}; }
static float ch(vec3 v, int i) { return i == 0 ? v.r : i == 1 ? v.g : v.b; }
// a blurred edge (the picture as it reaches the window pass), sampled one picture pixel apart around x
static float edge(float x) { return 0.1f + 0.8f / (1.0f + std::exp(-2.0f * x)); }
int main() {
    const float amounts[] = {0.0f, 0.1f, 0.4f, 0.8f, 1.0f};
    for (int i = 0; i < 200000; i++) {
        const vec3 c = rnd3(), n = rnd3(), s = rnd3(), w = rnd3(), e = rnd3();
        for (float a : amounts) {
            const vec3 o = sharpen(c, n, s, w, e, a);
            for (int k = 0; k < 3; k++) {
                const float lo = std::min({ch(c, k), ch(n, k), ch(s, k), ch(w, k), ch(e, k)});
                const float hi = std::max({ch(c, k), ch(n, k), ch(s, k), ch(w, k), ch(e, k)});
                CHECK(ch(o, k) >= lo - 1e-6f && ch(o, k) <= hi + 1e-6f);   // no halo: inside the neighbourhood
                CHECK(ch(o, k) >= 0.0f && ch(o, k) <= 1.0f);                 // no clipping
                CHECK(std::isfinite(ch(o, k)));
                if (a == 0.0f) CHECK(ch(o, k) == ch(c, k));                  // 0: the picture unchanged
            }
        }
    }
    // flat light, black and white stay as they are at full strength
    for (float v : {0.0f, 0.5f, 1.0f}) {
        const vec3 o = sharpen(vec3(v), vec3(v), vec3(v), vec3(v), vec3(v), 1.0f);
        CHECK(o.r == v && o.g == v && o.b == v);
    }
    // a blurred edge gets steeper as the setting rises: the pixel on the light side lighter, never past the light beyond it
    float last = edge(0.6f);
    for (float a : {0.25f, 0.5f, 0.75f, 1.0f}) {
        const vec3 c(edge(0.6f)), mid(edge(0.6f));
        const vec3 o = sharpen(c, mid, mid, vec3(edge(-0.4f)), vec3(edge(1.6f)), a);
        CHECK(o.r > last && o.r <= edge(1.6f));
        last = o.r;
    }
    CHECK(last - edge(0.6f) > 0.01f);   // (a visible step, not a rounding)
    std::printf("PASS: sharpen() is the picture at 0, stays inside its neighbourhood and 0..1, and steepens edges with the setting\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--glslang", default="glslangValidator")
    p.add_argument("--write", action="store_true", help="make fg_scale_comp_spv.h and fg_scale_pq_comp_spv.h from fg_scale.comp")
    a = p.parse_args()
    comp = SHADERS / "fg_scale.comp"
    if a.write:
        glslang = shutil.which(a.glslang)
        if not glslang:
            raise SystemExit("--write needs %s" % a.glslang)
        with tempfile.TemporaryDirectory(prefix="fg-sharpen-") as tmp:
            for name, var, defines in HEADERS:
                (SHADERS / name).write_bytes(source_line(comp, var, defines) +
                                             compile_header(glslang, comp, Path(tmp) / "out.h", var, defines))
                print("wrote %s" % (SHADERS / name))
        return
    bodies = {}
    for name, var, defines in HEADERS:
        first, _, bodies[name] = (SHADERS / name).read_bytes().partition(b"\n")
        if first + b"\n" != source_line(comp, var, defines):
            raise SystemExit("%s was not made from this fg_scale.comp (its first line names another hash or command): "
                             "python -I -B tests/test_fg_scale_sharpen.py --write" % name)
    text = comp.read_text(encoding="utf-8")
    # (FG_SCALE_PQ: the destination RGB10A2, the dither less than one of its steps; else RGBA8 and 8 bits)
    pq = re.search(r"#ifdef FG_SCALE_PQ\n(.*?)#else\n(.*?)#endif", text, re.S)
    if not pq or "layout(binding = 1, rgb10_a2) uniform writeonly image2D dst;" not in pq.group(1) or \
            "const float kSteps = 1023.0;" not in pq.group(1) or \
            "layout(binding = 1, rgba8) uniform writeonly image2D dst;" not in pq.group(2) or \
            "const float kSteps = 255.0;" not in pq.group(2) or "/ kSteps; }" not in text:
        raise SystemExit("fg_scale.comp: FG_SCALE_PQ does not make the destination RGB10A2 (and its dither 10-bit steps)")
    body = block(text, "void main()")
    sharp = body[body.index("if (pc.sharp > 0.0)"):]
    if "textureSize(src, 0)" not in sharp:
        raise SystemExit("the sharpen's neighbours are not spaced by the picture's size (textureSize(src, 0))")
    spacing = re.search(r"const vec2 e = ([^;]*);", sharp)
    if not spacing or "textureSize(src, 0)" not in spacing.group(1):
        raise SystemExit("the neighbour spacing e is not one picture pixel")
    for tap in ("uv + vec2(0.0, -e.y)", "uv + vec2(0.0, e.y)", "uv + vec2(-e.x, 0.0)", "uv + vec2(e.x, 0.0)"):
        if tap not in sharp:
            raise SystemExit("missing the neighbour at %s" % tap)
    lobe = re.search(r"const float kMaxLobe = [^;]*;", text).group(0)
    fn = block(text, "vec3 sharpen(")
    fixture = SHIM + lobe.replace("const float", "static const float") + "\nstatic " + fn + FIXTURE
    with tempfile.TemporaryDirectory(prefix="fg-sharpen-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        glslang = shutil.which(a.glslang)
        if not glslang:
            print("PASS: the header names this shader (no %s: its compile not checked)" % a.glslang)
            return
        for name, var, defines in HEADERS[1:]:   # (the variants: compiled; byte for byte as below when glslang is the same)
            made = compile_header(glslang, comp, Path(tmp) / name, var, defines)
            if made.splitlines()[0] == bodies[name].splitlines()[0] and made != bodies[name]:
                raise SystemExit("%s is not fg_scale.comp compiled with %s: python -I -B tests/test_fg_scale_sharpen.py "
                                 "--write" % (name, " ".join(defines)))
        hbody = bodies["fg_scale_comp_spv.h"]
        made = compile_header(glslang, comp, Path(tmp) / "fg_scale_comp_spv.h")
        if made.splitlines()[0] != hbody.splitlines()[0]:
            print("PASS: the header names this shader, and it compiles (glslang %s, the header's is %s: bytes not compared)" %
                  (made.splitlines()[0].decode().strip(), hbody.splitlines()[0].decode().strip()))
            return
        if made != hbody:
            raise SystemExit("fg_scale_comp_spv.h is not fg_scale.comp compiled: python -I -B tests/test_fg_scale_sharpen.py --write")
        print("PASS: fg_scale_comp_spv.h and fg_scale_pq_comp_spv.h are fg_scale.comp compiled")


if __name__ == "__main__":
    main()
