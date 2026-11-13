# SPDX-License-Identifier: MIT
# The reference below is a line-by-line port of RenoDX (github.com/clshortfuse/renodx); Copyright (c) 2025 Carlos Lopez Jr.,
# MIT: gpu/vk/eden/shader_recompiler/ir_opt/LICENSE-renodx.md
"""PsychoV35 as the scene tone map pass emits it (gpu/vk/eden/shader_recompiler/ir_opt/psychov35.h) gives what RenoDX's
zelda-oot-oz mod gives at its defaults.

The header's math is written over its operations; here they are floats. The reference is RenoDX's path, as written in
scene_tonemap.slangi and psychov/test35.hlsl (github.com/clshortfuse/renodx, MIT), ported line by line: the log-domain
response, the residual's normalization before its length, the source and target matrices from their XYZ definitions. The
header has the reduced form (one anchor power, the logistic) and its matrices baked; both are checked here.

python -I -B tests/test_psychov35.py [--cxx c++]
"""
import argparse
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "gpu" / "vk" / "eden" / "shader_recompiler" / "ir_opt" / "psychov35.h"


def mul(m, v):
    return [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(v):
    return math.sqrt(dot(v, v))


BT709_TO_XYZ = [[.4123907993, .3575843394, .1804807884], [.2126390059, .7151686788, .0721923154],
                [.0193308187, .1191947798, .9505321522]]
XYZ_TO_BT709 = [[3.2409699419, -1.5373831776, -.4986107603], [-.9692436363, 1.8759675015, .0415550574],
                [.0556300797, -.2039769589, 1.0569715142]]
XYZ_TO_BT2020 = [[1.7166511880, -.3556707838, -.2533662814], [-.6666843518, 1.6164812366, .0157685458],
                 [.0176398574, -.0427706133, .9421031212]]
XYZ_TO_LMS = [[.2670502842655792, .8471990148492798, -.03470416612462053],
              [-.38706882411220156, 1.165429935890458, .10302286696614202],
              [.026727793989083093, -.02729131667566509, .5333267257603284]]
_det = dot(XYZ_TO_LMS[0], cross(XYZ_TO_LMS[1], XYZ_TO_LMS[2]))
_cols = [cross(XYZ_TO_LMS[1], XYZ_TO_LMS[2]), cross(XYZ_TO_LMS[2], XYZ_TO_LMS[0]), cross(XYZ_TO_LMS[0], XYZ_TO_LMS[1])]
LMS_TO_XYZ = [[_cols[j][i] / _det for j in range(3)] for i in range(3)]   # (transpose(float3x3(rows)) / det)
BT709_TO_LMS = matmul(XYZ_TO_LMS, BT709_TO_XYZ)
LMS_TO_BT709 = matmul(XYZ_TO_BT709, LMS_TO_XYZ)
LMS_TO_BT2020 = matmul(XYZ_TO_BT2020, LMS_TO_XYZ)
WHITE = mul(BT709_TO_LMS, [1, 1, 1])


def mean(v):
    return v[0] + ((v[1] - v[0]) + (v[2] - v[0])) / 3


def target_residual(n, target):
    return mul(LMS_TO_BT709 if target == 0 else LMS_TO_BT2020, [n[i] * WHITE[i] for i in range(3)])


def response_logs(log_q, k, n, h):
    latent = math.log(k) - math.log1p(-k) + n * log_q
    correction = math.log1p(math.exp(-abs(latent)))
    log_u = (min(latent, 0) - correction) / h
    log_tail = -max(latent, 0) - correction
    if latent > 20:
        return log_u, log_tail - math.log(h)
    d = -log_u * (1 + log_u * (.5 + log_u * (1 / 6 + log_u / 24))) if abs(log_u) < .01 else 1 - math.exp(log_u)
    return log_u, math.log(d)


def psychotm_test35(x, peak, cone, a_in, a_out):
    """test35.hlsl's psychotm_test35 as zelda-oot-oz calls it: exposure, highlights, shadows, contrast and purity 1, BT.2020
    target, gamut compression 1, compression 1, no conditioning, exact fit"""
    lms = mul(BT709_TO_LMS, x)
    anchor_in = mul(BT709_TO_LMS, [a_in] * 3)
    anchor_out = mul(BT709_TO_LMS, [a_out] * 3)
    sr = [lms[i] / anchor_in[i] for i in range(3)]
    k = [anchor_out[i] / (WHITE[i] * peak) for i in range(3)]
    n = [cone / (1 - k[i]) for i in range(3)]
    if all(v == 0 for v in sr):
        return [0.0] * 3
    drive = max(abs(v) for v in sr)
    ref = [v / drive for v in sr]
    stops = math.log2(drive)
    resp, defi = [0.0] * 3, [0.0] * 3
    for i in range(3):
        if ref[i] == 0:
            resp[i], defi[i] = 0.0, 1.0
        else:
            lu, ld = response_logs(math.log(ref[i]) + stops * math.log(2), k[i], n[i], 1.0)
            resp[i], defi[i] = math.exp(lu), math.exp(ld)
    rm = mean(resp)
    rres = [mean(defi) - d for d in defi] if rm > .5 else [r - rm for r in resp]
    scale = max(abs(v) for v in rres)
    sres = [v / scale for v in rres] if scale > 0 else [0.0] * 3
    srad = norm(sres)
    radius = scale * srad
    direction = [v / srad for v in sres] if srad > 0 else [0.0] * 3
    source = [v - mean(ref) for v in ref]
    source_radius = norm(source)
    if source_radius > 0 and radius > 0:
        middle = [source[i] / source_radius + direction[i] for i in range(3)]
        if norm(middle) > 0:
            direction = [v / norm(middle) for v in middle]
    level, headroom = peak * rm, peak * mean(defi)
    residual = [peak * radius * d for d in direction]
    if all(v == 0 for v in residual):
        return [level] * 3
    td = target_residual(residual, 1)
    s = 1.0
    for c in range(3):
        if td[c] > 0:
            s = min(s, headroom / td[c])
        elif td[c] < 0:
            s = min(s, level / -td[c])
    out = target_residual([v * s for v in residual], 0)
    return [level + v for v in out]


def aces(m):
    return min(1.0, max(0.0, m * (2.51 * m + .03) / (m * (2.43 * m + .59) + .14)))


def scene_tonemap(scene, gain, peak):
    """scene_tonemap.slangi's ApplySceneToneMap at the defaults: hue clip 50%, vanilla midgray and slope, exposure 1, white
    and graphics white the same; the scene pass's clamp at 0 after it"""
    u = list(scene)
    m = [abs(v) for v in u]
    r = [aces(v) for v in m]
    lo, rng = min(r), max(r) - min(r)
    neutral = rng == 0
    t = [min(m) + (r[i] - lo) / (1 if neutral else rng) * (max(m) - min(m)) for i in range(3)]
    t = [math.copysign(t[i], u[i]) for i in range(3)]
    if neutral:
        t = u
    bent = [u[i] + (t[i] - u[i]) * .5 for i in range(3)]
    w = [.2126390059, .7151686788, .0721923154]
    y, yb = dot(w, u), dot(w, bent)
    if y > 0 and yb > 0:
        u = [v * (y / yb) for v in bent]
    a_out = .18 * gain * aces(.18) / .18
    out = psychotm_test35(u, peak, 1.3355037565264822, .18, a_out)
    return [max(0.0, v) for v in out]


def header_checks():
    text = HEADER.read_text(encoding="utf-8")
    to_ratio = [[BT709_TO_LMS[i][j] / (.18 * WHITE[i]) for j in range(3)] for i in range(3)]
    to_2020 = [[LMS_TO_BT2020[i][j] * WHITE[j] for j in range(3)] for i in range(3)]
    to_709 = [[LMS_TO_BT709[i][j] * WHITE[j] for j in range(3)] for i in range(3)]
    for name, want in (("kToRatio", to_ratio), ("kToBt2020", to_2020), ("kToBt709", to_709)):
        start = text.index("inline constexpr float %s[3][3]" % name)
        got = [float(v.rstrip("f")) for v in
               __import__("re").findall(r"-?\d+\.\d+f", text[start:text.index(";", start)])]
        flat = [v for row in want for v in row]
        if len(got) != 9 or any(abs(a - b) > 1e-7 * max(1, abs(b)) for a, b in zip(got, flat)):
            raise SystemExit("FAIL: %s is not the matrix test35.hlsl's definitions give: %s" % (name, flat))
    start = text.index("inline constexpr float kLuminance[3]")
    got = [float(v.rstrip("f")) for v in __import__("re").findall(r"-?\d+\.\d+f", text[start:text.index(";", start)])]
    if got != BT709_TO_XYZ[1]:
        raise SystemExit("FAIL: kLuminance is not BT.709's Y row %s" % BT709_TO_XYZ[1])


FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS   /* (Windows: scanf is not deprecated here) */
#include <cmath>
#include <cstdio>
#include "psychov35.h"
struct Ops {
    using F = float;
    using B = bool;
    F Imm(float v) { return v; }
    F Add(F a, F b) { return a + b; }
    F Sub(F a, F b) { return a - b; }
    F Mul(F a, F b) { return a * b; }
    F Fma(F a, F b, F c) { return std::fma(a, b, c); }
    F Div(F a, F b) { return a * (1.0f / b); }   // (as the pass: a reciprocal)
    F Neg(F a) { return -a; }
    F Abs(F a) { return std::fabs(a); }
    F Min(F a, F b) { return std::fmin(a, b); }
    F Max(F a, F b) { return std::fmax(a, b); }
    F Sqrt(F a) { return std::sqrt(a); }
    F Log2(F a) { return std::log2(a); }
    F Exp2(F a) { return std::exp2(a); }
    F Select(B c, F a, F b) { return c ? a : b; }
    B Gt(F a, F b) { return a > b; }
    B Lt(F a, F b) { return a < b; }
    B Eq(F a, F b) { return a == b; }
    B And(B a, B b) { return a && b; }
};
int main() {
    float r, g, b, gain, peak;
    Ops o;
    while (std::scanf("%f %f %f %f %f", &r, &g, &b, &gain, &peak) == 5) {
        const auto out = Shader::Optimization::PsychoV35::SceneToneMap(o, {r, g, b}, gain, peak);
        std::printf("%.9g %.9g %.9g\n", out[0], out[1], out[2]);
    }
}
'''


def fixture_checks(cxx):
    rng = random.Random(35)
    cases = [(.18, .18, .18, 1, 1000 / 203), (1, 1, 1, 1, 1000 / 203), (10, 10, 10, 1, 1000 / 203), (5, 1, .2, 1, 4),
             (0, 0, 0, 1, 5), (0, 2, 0, 1.5, 5), (100, 0, 0, .7, 10), (1e-6, 2e-6, 0, 1, 5), (2e3, 1e3, 5e3, 1, 20)]
    for _ in range(3000):
        scale = 10 ** rng.uniform(-3, 2.5)
        c = [rng.random() ** 3 * scale for _ in range(3)]
        if rng.random() < .1:
            c[rng.randrange(3)] = 0.0
        cases.append((*c, 10 ** rng.uniform(-.5, .8), rng.uniform(2, 50)))
    with tempfile.TemporaryDirectory(prefix="psychov35-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(HEADER.parent), str(src), "-o",
                        str(exe)], check=True)
        inp = "".join("%.9g %.9g %.9g %.9g %.9g\n" % c for c in cases)
        lines = subprocess.run([str(exe)], input=inp, capture_output=True, text=True, check=True).stdout.splitlines()
    if len(lines) != len(cases):
        raise SystemExit("FAIL: %d results for %d cases" % (len(lines), len(cases)))
    worst = 0.0
    for case, line in zip(cases, lines):
        got = [float(v) for v in line.split()]
        want = scene_tonemap(case[:3], case[3], case[4])
        if any(math.isnan(v) for v in got):
            raise SystemExit("FAIL: NaN for %s" % (case,))
        err = max(abs(a - b) for a, b in zip(got, want)) / max(1e-3, max(abs(v) for v in want))
        worst = max(worst, err)
        if err > 2e-4:
            raise SystemExit("FAIL: %s gives %s, RenoDX's path %s" % (case, got, want))
    midgray = [float(v) for v in lines[0].split()]
    if any(abs(v - aces(.18)) > 1e-5 for v in midgray):
        raise SystemExit("FAIL: midgray is not the game's (%s, not %s)" % (midgray, aces(.18)))
    print("PASS: PsychoV35 as RenoDX's zelda-oot-oz has it (%d colours, worst relative difference %.2g)" % (len(cases), worst))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="c++")
    a = p.parse_args()
    header_checks()
    fixture_checks(a.cxx)


if __name__ == "__main__":
    main()
