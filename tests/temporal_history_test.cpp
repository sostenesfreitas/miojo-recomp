// The production decision fragments and reset assignments are inserted by test_temporal_history.py.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "gpu/vk/dlss_math.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace dlss_math = mkwvk::dlss_math;
static void logf(const char*, ...) {}   // (the camera cut's log line)
struct State {
    uint64_t last_frame = 99;
    float last_vp[16]{}, last_jitter[2]{};
    std::chrono::steady_clock::time_point last_at{};   // (the camera cut's time between frames)
    struct { unsigned width = 1920, height = 1080; } frame;
};
struct Camera { float current[16]{}; };
struct Decision { bool reset, motion; };

Decision temporal(State& s, const Camera& cam, uint64_t now, bool fsr4_engine) {
    const uint64_t last = s.last_frame;
    /* CHAIN_POLICY */
    (void)show_motion;
    struct { bool reset; } f{};
    /* CHAIN_SINK */
    struct { unsigned InReset; } ep{};
    struct { bool reset; } d{};
    /* DLSS_SINK */
    /* FSR4_SINK */
    return {fsr4_engine ? d.reset : ep.InReset != 0, motion};
}
Decision dlss(State& s, const Camera& cam, uint64_t now = 100) { return temporal(s, cam, now, false); }
Decision fsr4(State& s, const Camera& cam, uint64_t now = 100) { return temporal(s, cam, now, true); }

Decision fsr(State& s, const Camera& cam, uint64_t now = 100) {
    const unsigned rw = 1920, rh = 1080;
    /* FSR_POLICY */
    struct { bool reset; } f{};
    /* FSR_SINK */
    return {f.reset, motion};
}

int checks = 0, failures = 0;
void check(bool value, const char* why) {
    ++checks;
    if (!value) { ++failures; std::cerr << why << '\n'; }
}
void identity(float* p) { for (int i = 0; i < 16; ++i) p[i] = i % 5 == 0 ? 1.0f : 0.0f; }
void policies(State& s, const Camera& camera, bool reset, uint64_t now = 100) {
    for (auto call : {dlss, fsr4, fsr}) {
        const Decision d = call(s, camera, now);
        check(d.reset == reset, "wrong reset delivered to an upscaler");
        check(d.motion == !reset, "wrong motion eligibility");
    }
}

int main() {
    State s; Camera camera;
    float current[16], previous[16], out[16];
    identity(s.last_vp); identity(camera.current);
    policies(s, camera, false);                     // ordinary consecutive frame
    s.last_frame = 0; policies(s, camera, true);     // no prior history
    s.last_frame = 98; policies(s, camera, false);   // age 2 remains usable
    s.last_frame = 97; policies(s, camera, true);    // age 3 expires
    s.last_frame = 99;
    s.frame.width = 1720;
    check(fsr(s, camera, 100).reset, "FSR size change lost its reset");
    s.frame.width = 1920;
    s.frame.height = 720;
    check(fsr(s, camera, 100).reset, "FSR height change lost its reset");
    s.frame.height = 1080;

    std::memset(camera.current, 0, sizeof camera.current);
    policies(s, camera, true);                     // singular current: no history
    identity(camera.current);
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        for (int i = 0; i < 16; ++i) {
            identity(camera.current); camera.current[i] = invalid;
            check(!dlss_math::reprojection(camera.current, s.last_vp, out), "accepted non-finite current");
            policies(s, camera, true);
            identity(camera.current); identity(s.last_vp); s.last_vp[i] = invalid;
            check(!dlss_math::reprojection(camera.current, s.last_vp, out), "accepted non-finite previous");
            policies(s, camera, true);
        }
        identity(s.last_vp);
    }
    // An invalid current becomes the next previous matrix; history recovers after a valid pair.
    identity(camera.current); camera.current[0] = std::numeric_limits<float>::quiet_NaN();
    policies(s, camera, true);
    std::memcpy(s.last_vp, camera.current, sizeof s.last_vp);
    identity(camera.current); policies(s, camera, true);
    std::memcpy(s.last_vp, camera.current, sizeof s.last_vp); policies(s, camera, false);

    for (float sign : {-1.0f, 1.0f}) {
        identity(current); identity(previous);
        current[0] = 1e-10f; previous[0] = sign * std::numeric_limits<float>::max();
        check(!dlss_math::reprojection(current, previous, out), "accepted a float-overflow reprojection");
        std::memcpy(camera.current, current, sizeof current); std::memcpy(s.last_vp, previous, sizeof previous);
        policies(s, camera, true);
        current[0] = 1.0f;
        check(dlss_math::reprojection(current, previous, out), "rejected a representable boundary");
        check(std::isfinite(out[0]) && out[0] == previous[0], "wrong representable boundary");
    }
    // Valid perspective transforms across aspect changes and camera translation retain usable motion.
    for (float aspect : {16.0f / 9.0f, 3440.0f / 1440.0f, 32.0f / 9.0f}) {
        for (int frame = 1; frame <= 100; ++frame) {
            identity(s.last_vp); identity(camera.current);
            camera.current[0] = 1.3f / aspect; camera.current[5] = 1.3f;
            camera.current[10] = -1.01f; camera.current[11] = -0.2f;
            camera.current[14] = -1.0f; camera.current[15] = 0.0f;
            s.last_vp[3] = (float)frame * 0.01f;
            policies(s, camera, false);
            check(dlss_math::reprojection(camera.current, s.last_vp, out), "valid perspective rejected");
            for (float v : out) check(std::isfinite(v), "accepted non-finite shader coefficient");
        }
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
