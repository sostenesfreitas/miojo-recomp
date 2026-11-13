// (hookshot) RX_VK_TRANSLATE_WAIT_MS: how long, in all, each recording thread waits in a frame for shaders still translating
// before leaving their draws on earlier frames' targets out (vk_draw.cpp, draw). Off (-1, each translation waited out as
// #1573 does) unless set.
#pragma once
#include <cmath>
#include <cstdlib>

namespace translate_wait {

constexpr double kOff = -1;

/// The switch's value: a number of milliseconds ("8", "2.5"; a negative one is off), spaces around it allowed. false for
/// anything else ("", "8ms", "off", "nan", "inf"), which the caller ignores with a warning, so that a typo does not read
/// as 0 (every untranslated draw left out at once, RX_VK_PIPE_SKIP=2 without saying so)
inline bool parse(const char* s, double& out) {
    if (!s) return false;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end || !std::isfinite(v)) return false;
    out = v;
    return true;
}

}  // namespace translate_wait
