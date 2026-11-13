/* (hookshot) NVIDIA DLSS's camera math, as plain functions: the jitter's sequence (host/rx_backend.c, in C) and the motion
   vectors' reprojection (gpu/vk/vk_dlss.cpp). No Vulkan, no NGX: tests/test_dlss_math.cpp checks them. */
#pragma once

#ifdef __cplusplus
#include <cmath>
#include <limits>
#include <utility>
#endif

/* The radical inverse of index in base: the Halton sequence's index-th value, in [0, 1) */
static inline float dlss_halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

/* The frame's sub-pixel jitter, in pixels, each in [-0.5, 0.5): the Halton (2, 3) sequence over `phases` frames (from 1: its
   first value, 0, would put every phase's start on the pixel's corner) */
static inline void dlss_jitter_pixels(unsigned frame, int phases, float out[2]) {
    const int i = (int)(frame % (unsigned)(phases > 0 ? phases : 1)) + 1;
    out[0] = dlss_halton(i, 2) - 0.5f;
    out[1] = dlss_halton(i, 3) - 0.5f;
}

/* The frame's jitter for a projection matrix p (row major, as the game fills its views' uniform buffers: nx2/oot_jitter.py),
   in clip space (what p[0][2] and p[1][2] get) into out, for a scene of w x h pixels; 0 (none) when p is not one of the
   picture's: not perspective (its w row is not 0 0 -1 0: a shadow map's, the HUD's) or not of 16:9 or the window's ratio
   wide (0: 16:9 only), or when there is no scene size (DLSS not drawing) */
static inline int dlss_jitter_clip(const float p[16], float wide, unsigned frame, int phases, float w, float h, float out[2]) {
    if (!(w >= 1.0f) || !(h >= 1.0f)) return 0;
    if (p[12] != 0.0f || p[13] != 0.0f || p[14] != -1.0f || p[15] != 0.0f || !(p[0] > 0.0f)) return 0;
    const float r = p[5] / p[0], d = r - 16.0f / 9.0f, e = wide > 0.0f ? r - wide : 1.0f;
    if (!((d <= 0.01f && d >= -0.01f) || (e <= 0.01f && e >= -0.01f))) return 0;
    float px[2];
    dlss_jitter_pixels(frame, phases, px);
    out[0] = 2.0f * px[0] / w, out[1] = 2.0f * px[1] / h;
    return 1;
}

/* How many frames the jitter's sequence takes: NVIDIA's guide, 8 times the share of pixels DLSS fills in, (output / render)
   squared, so each output pixel is covered about 8 times; 8 at least (DLAA), 64 at most (a long sequence is seen less of
   before the camera moves) */
static inline int dlss_jitter_phases(unsigned render_w, unsigned render_h, unsigned out_w, unsigned out_h) {
    if (!render_w || !render_h) return 8;
    const double area = (double)out_w * (double)out_h / ((double)render_w * (double)render_h);
    const int n = (int)(8.0 * area + 0.5);
    return n < 8 ? 8 : n > 64 ? 64 : n;
}

#ifdef __cplusplus
namespace mkwvk::dlss_math {

/// This frame's clip space to the frame before's: previous * inverse(current), both row major (clip = M * position); false
/// when either input is not finite, current is singular, or the result cannot be represented as finite floats
inline bool reprojection(const float* current, const float* previous, float out[16]) {
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(current[i]) || !std::isfinite(previous[i])) return false;
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? current[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {   // (Gauss-Jordan with partial pivoting)
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c)
            for (int k = 0; k < 8; k++) std::swap(a[p][k], a[c][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0.0) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += previous[r * 4 + k] * a[k][c + 4];
            if (!std::isfinite(v) || std::fabs(v) > std::numeric_limits<float>::max()) return false;
            out[r * 4 + c] = (float)v;
        }
    return true;
}

/// (hookshot) The camera of a perspective view-projection m (row major, clip = m * position): its position, the point clip x, y
/// and w are all 0 at (rows 0, 1 and 3 of m, solved for x, y, z), and the direction it looks, the w row's first three (w is
/// the distance in front of the camera). A jitter adds a multiple of the w row to rows 0 and 1, so it moves neither. False for
/// a matrix that has no such point (not a perspective one, or not finite)
inline bool camera_pose(const float m[16], double pos[3], double fwd[3]) {
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(m[i])) return false;
    const int rows[3] = {0, 1, 3};
    double a[3][4];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) a[r][c] = c < 3 ? m[rows[r] * 4 + c] : -m[rows[r] * 4 + 3];
    for (int c = 0; c < 3; c++) {   // (Gauss-Jordan with partial pivoting)
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c)
            for (int k = 0; k < 4; k++) std::swap(a[p][k], a[c][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 4; k++) a[c][k] *= inv;
        for (int r = 0; r < 3; r++) {
            if (r == c || a[r][c] == 0.0) continue;
            const double f = a[r][c];
            for (int k = 0; k < 4; k++) a[r][k] -= f * a[c][k];
        }
    }
    const double len = std::sqrt((double)m[12] * m[12] + (double)m[13] * m[13] + (double)m[14] * m[14]);
    if (!(len > 1e-12)) return false;
    for (int i = 0; i < 3; i++) pos[i] = a[i][3], fwd[i] = m[12 + i] / len;
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}

/// (hookshot) A camera cut between two frames' view-projections, ms apart: a move over 5 (the game's units, metres) or a turn over
/// 30 degrees with a move over 0.5, per 60 Hz frame (scaled by the time between them, a quarter to four frames). Play moves
/// the camera about 0.05 m and 0.08 degrees a frame and a quick swing round Link 11-18 degrees and 0.9-1.4 m; an area load or
/// a spawn moves it 5 to 180 m at once. *move and *turn: what was measured (0 when either matrix has no camera)
inline bool camera_cut(const float previous[16], const float current[16], double ms, double* move, double* turn) {
    double p0[3], f0[3], p1[3], f1[3];
    *move = *turn = 0;
    if (!camera_pose(previous, p0, f0) || !camera_pose(current, p1, f1)) return false;
    const double dx = p1[0] - p0[0], dy = p1[1] - p0[1], dz = p1[2] - p0[2];
    *move = std::sqrt(dx * dx + dy * dy + dz * dz);
    double dot = f0[0] * f1[0] + f0[1] * f1[1] + f0[2] * f1[2];
    dot = dot > 1 ? 1 : dot < -1 ? -1 : dot;
    *turn = std::acos(dot) * (180.0 / 3.14159265358979323846);
    double k = ms / (1000.0 / 60);
    k = k < 0.25 ? 0.25 : k > 4 ? 4 : k;
    return *move > 5.0 * k || (*turn > 30.0 * k && *move > 0.5 * k);
}

/// A view-projection's jitter taken off (row major): a jitter moves clip x and y by jitter * w, so jitter times the w row
/// comes off the first two rows
inline void unjitter(float m[16], const float jitter[2]) {
    for (int c = 0; c < 4; c++) {
        m[0 * 4 + c] -= jitter[0] * m[3 * 4 + c];
        m[1 * 4 + c] -= jitter[1] * m[3 * 4 + c];
    }
}

/// A jitter in clip space as DLSS takes it: in render pixels, x right and y down (clip y is up)
inline void jitter_to_pixels(const float clip[2], unsigned width, unsigned height, float out[2]) {
    out[0] = clip[0] * 0.5f * (float)width;
    out[1] = -clip[1] * 0.5f * (float)height;
}

} // namespace mkwvk::dlss_math
#endif
