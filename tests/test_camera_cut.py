"""Camera cuts for the temporal chain (gpu/vk/dlss_math.h camera_pose, camera_cut; vk_temporal.cpp).

Real view-projections (a look-at view times a perspective, row major, clip = M * position, the game's convention) at known
cameras: camera_pose gives the camera's position and the direction it looks back, also with a jitter on the matrix; then the
cut test: play (0.05 m, 0.08 degrees a frame) and a quick swing round Link (15 degrees, 1.2 m a frame) are no cut, an area
load (40 m) and a turn of 35 degrees with a 1 m move are, a 5 m move spread over four frames' time is not, and a matrix with
no camera (an orthographic one) is never a cut.
python -I -B tests/test_camera_cut.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include "dlss_math.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::dlss_math;
static void mul(const double a[16], const double b[16], float out[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = (float)v;
        }
}
/* the view-projection of a camera at eye looking toward yaw degrees (about y) and pitch, a 60 degree perspective */
static void vp(const double eye[3], double yaw, double pitch, float out[16], float jx = 0, float jy = 0) {
    const double y = yaw * 3.14159265358979 / 180, p = pitch * 3.14159265358979 / 180;
    const double f[3] = {std::cos(p) * std::sin(y), std::sin(p), -std::cos(p) * std::cos(y)};
    double s[3] = {f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0};   /* f x up(0,1,0) */
    const double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (double& v : s) v /= sl;
    const double u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    const double view[16] = {s[0], s[1], s[2], -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]),
                             u[0], u[1], u[2], -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                             -f[0], -f[1], -f[2], (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]),
                             0, 0, 0, 1};
    const double t = 1 / std::tan(30 * 3.14159265358979 / 180), n = 0.1, fa = 5000;
    const double proj[16] = {t / 1.7778, 0, jx, 0, 0, t, jy, 0, 0, 0, fa / (n - fa), fa * n / (n - fa), 0, 0, -1, 0};
    mul(proj, view, out);
}
int main() {
    const double eye[3] = {120.5, 33.0, -840.25};
    float m[16];
    vp(eye, 37, -12, m);
    double pos[3], fwd[3];
    CHECK(camera_pose(m, pos, fwd));
    for (int i = 0; i < 3; i++) CHECK(std::fabs(pos[i] - eye[i]) < 0.05);
    const double y = 37 * 3.14159265358979 / 180, p = -12 * 3.14159265358979 / 180;
    CHECK(std::fabs(fwd[0] - std::cos(p) * std::sin(y)) < 1e-4 && std::fabs(fwd[1] - std::sin(p)) < 1e-4 &&
          std::fabs(fwd[2] + std::cos(p) * std::cos(y)) < 1e-4);
    float j[16];
    vp(eye, 37, -12, j, 0.0007f, -0.0004f);                        /* a jittered one: the same camera */
    double pj[3], fj[3];
    CHECK(camera_pose(j, pj, fj));
    for (int i = 0; i < 3; i++) CHECK(std::fabs(pj[i] - pos[i]) < 0.05 && std::fabs(fj[i] - fwd[i]) < 1e-6);

    double move, turn;
    float a[16], b[16];
    const double e2[3] = {eye[0] + 0.05, eye[1], eye[2]};
    vp(eye, 37, -12, a), vp(e2, 37.08, -12, b);
    CHECK(!camera_cut(a, b, 16.7, &move, &turn) && move > 0.04 && move < 0.06);   /* play */
    const double e3[3] = {eye[0] + 1.2, eye[1], eye[2]};
    vp(e3, 52, -12, b);
    CHECK(!camera_cut(a, b, 16.7, &move, &turn) && turn > 14 && turn < 16);       /* a quick swing round Link */
    vp(e3, 72, -12, b);
    CHECK(camera_cut(a, b, 16.7, &move, &turn) && turn > 34);                      /* 35 degrees with a 1.2 m move */
    const double e4[3] = {eye[0] + 40, eye[1], eye[2] + 10};
    vp(e4, 37, -12, b);
    CHECK(camera_cut(a, b, 16.7, &move, &turn) && move > 41);                      /* an area load */
    vp(e4, 200, -12, b);
    CHECK(camera_cut(a, b, 16.7, &move, &turn));
    const double e5[3] = {eye[0] + 5.5, eye[1], eye[2]};
    vp(e5, 37, -12, b);
    CHECK(camera_cut(a, b, 16.7, &move, &turn));                                   /* 5.5 m in a frame */
    CHECK(!camera_cut(a, b, 66.7, &move, &turn));                                  /* the same over four frames' time */
    CHECK(!camera_cut(a, a, 16.7, &move, &turn) && move < 1e-3 && turn < 1e-3);
    const float ortho[16] = {0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, -0.001f, 0, 0, 0, 0, 1};
    CHECK(!camera_pose(ortho, pos, fwd) && !camera_cut(ortho, a, 16.7, &move, &turn) && move == 0);
    const float zero[16] = {};
    CHECK(!camera_cut(zero, a, 16.7, &move, &turn));                               /* the first frame: no camera before */
    std::printf("PASS: the camera from a view-projection, and cuts told from play and quick swings\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    temporal = (ROOT / "gpu/vk/vk_temporal.cpp").read_text(encoding="utf-8")
    for needle in ("dlss_math::camera_cut(s.last_vp, cam.current, ms, &move, &turn)", 'getenv("RX_VK_TEMPORAL_CUT")',
                   "if (cut_on && !reset && s.last_at.time_since_epoch().count())"):
        if needle not in temporal:
            raise SystemExit("FAIL: vk_temporal.cpp: missing %r" % needle)
    if temporal.index("dlss_math::camera_cut(") > temporal.index("const bool motion = !reset && dlss_math::reprojection("):
        raise SystemExit("FAIL: the cut is tested after the motion: its reset would not reach this frame")
    with tempfile.TemporaryDirectory(prefix="camera-cut-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
