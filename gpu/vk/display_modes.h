/* The Insert menu's Aspect ratio and Output resolution rows (vk_menu.cpp): the sizes the screen offers, as the game can use
   them, and the shape each one has. Plain code, no Vulkan, no Windows: tests/test_display_modes.py checks it.

   The screen's modes (Windows' display settings, or HOS_DISPLAY_MODES=1920x1080,2560x1080,... for a run) become the sizes
   offered: each side rounded down to a multiple of 8, as host/nn_applet.c takes an output size, only those it accepts
   (hos_output_size_valid: 640 x 360 up to 5120 x 2880's pixels, 5:4 to 48:9), each once, by width then height. Their shapes
   are named as screens are sold (16:9, 21:9, 16:10, 4:3...) so the Aspect ratio row groups them. */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../host/hos_wide_size.h"   // (hos_output_size_valid: the sizes the game accepts)

namespace mkwvk::display_modes {

struct Size {
    uint32_t w, h;
};
inline bool operator==(Size a, Size b) { return a.w == b.w && a.h == b.h; }

/// The screen's modes as the game can take them: each side rounded down to 8, the ones it accepts, each size once, by
/// width then height
inline std::vector<Size> offered(const std::vector<Size>& modes) {
    std::vector<Size> v;
    for (Size m : modes) {
        const Size s{m.w & ~7u, m.h & ~7u};
        if (hos_output_size_valid(s.w, s.h) && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    }
    std::sort(v.begin(), v.end(), [](Size a, Size b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
    return v;
}

/// Common sizes of each shape, beyond what a screen lists: 1440p, 4K and 5K at 16:9, 16:10's and 4:3's, 21:9's and 32:9's.
/// Each side a multiple of 8 (1366x768, 1600x900 and 1680x1050 are not: a screen that has them lists them, rounded)
inline const std::vector<Size>& common_sizes() {
    static const std::vector<Size> v = {
        {1280, 720},  {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}, {5120, 2880},   // 16:9
        {1280, 800},  {1920, 1200}, {2560, 1600}, {2880, 1800}, {3840, 2400},                 // 16:10
        {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440}, {2048, 1536}, {2880, 2160},   // 4:3
        {1280, 1024},                                                                          // 5:4
        {1920, 1280}, {2160, 1440}, {3000, 2000},                                              // 3:2
        {2560, 1080}, {3440, 1440}, {3840, 1600}, {5120, 2160},                                // 21:9
        {3840, 1080}, {5120, 1440}};                                                           // 32:9
    return v;
}
/// sizes (offered's) and the common sizes of the shapes they have: a screen's 16:9 gets 1440p and 4K though it lists
/// neither; a shape it has none of gets none. Each once, by width then height
inline std::vector<Size> with_common(const std::vector<Size>& sizes);

/// "1920x1080,2560x1080" (HOS_DISPLAY_MODES): the sizes in it, as a screen's modes (anything else between them skipped)
inline std::vector<Size> parse_list(const char* text) {
    std::vector<Size> v;
    for (const char* p = text; p && *p;) {
        unsigned w = 0, h = 0;
        if (sscanf(p, "%u%*[xX]%u", &w, &h) == 2) v.push_back({w, h});
        while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
        while (*p == ',' || *p == ';' || *p == ' ') p++;
    }
    return v;
}

inline double ratio(Size s) { return s.h ? (double)s.w / s.h : 0.0; }

/// The shape's name, as screens are sold: 5:4, 4:3, 3:2, 16:10, 5:3, 16:9, 21:9 (64:27, 43:18 and 12:5 all are), 32:9, 48:9;
/// within 2 % (sizes rounded to 8 come out a little off: 1360x768 is 16:9). Another shape by its smallest whole numbers when
/// they are small (1600x1024: 25:16), else as its ratio to 1 ("1.85:1")
inline std::string ratio_name(Size s) {
    const double r = ratio(s);
    if (r >= 2.30 && r <= 2.45) return "21:9";
    static const struct {
        const char* name;
        double r;
    } named[] = {{"5:4", 5.0 / 4}, {"4:3", 4.0 / 3}, {"3:2", 3.0 / 2},  {"16:10", 16.0 / 10}, {"5:3", 5.0 / 3},
                 {"16:9", 16.0 / 9}, {"32:9", 32.0 / 9}, {"48:9", 48.0 / 9}};
    const char* best = nullptr;
    double off = 0.02;
    for (const auto& n : named)
        if (std::fabs(r / n.r - 1.0) <= off) off = std::fabs(r / n.r - 1.0), best = n.name;
    if (best) return best;
    uint32_t a = s.w, b = s.h;
    while (b) {   // (their greatest common divisor)
        const uint32_t t = a % b;
        a = b, b = t;
    }
    char text[24];
    if (a && s.w / a <= 64 && s.h / a <= 64) snprintf(text, sizeof text, "%u:%u", s.w / a, s.h / a);
    else snprintf(text, sizeof text, "%.2f:1", r);
    return text;
}

inline std::vector<Size> with_common(const std::vector<Size>& sizes) {
    std::vector<std::string> have;
    for (Size s : sizes) have.push_back(ratio_name(s));
    std::vector<Size> all = sizes;
    for (Size c : common_sizes())
        if (std::find(have.begin(), have.end(), ratio_name(c)) != have.end()) all.push_back(c);
    return offered(all);
}

/// A shape the Aspect ratio row lists: its name and the ratio it is ordered by (the first size of it that was seen)
struct Shape {
    std::string name;
    double r;
};
/// The shapes of sizes, each once, narrowest first
inline std::vector<Shape> shapes(const std::vector<Size>& sizes) {
    std::vector<Shape> v;
    for (Size s : sizes) {
        const std::string n = ratio_name(s);
        if (std::none_of(v.begin(), v.end(), [&](const Shape& x) { return x.name == n; })) v.push_back({n, ratio(s)});
    }
    std::sort(v.begin(), v.end(), [](const Shape& a, const Shape& b) { return a.r < b.r; });
    return v;
}
/// The place of s's shape in shapes, 0 when it is not there
inline int shape_index(const std::vector<Shape>& shapes, Size s) {
    const std::string n = ratio_name(s);
    for (int i = 0; i < (int)shapes.size(); i++)
        if (shapes[i].name == n) return i;
    return 0;
}
/// Of the sizes with shape name, the one to take when that shape is chosen: the screen's own size if it is one; else the one
/// whose height is nearest the screen's, the lower of two as near (21:9 on a 1080p screen: 2560x1080; 3:2 there: 1920x1280,
/// not 720x480; DSR's larger sizes stay a choice, not where a shape starts); with no screen size known, the largest. -1 when
/// none has it
inline int pick(const std::vector<Size>& sizes, const std::string& name, Size screen) {
    int best = -1;
    const auto px = [&](int i) { return (uint64_t)sizes[i].w * sizes[i].h; };
    const auto off = [&](int i) { return sizes[i].h > screen.h ? sizes[i].h - screen.h : screen.h - sizes[i].h; };
    for (int i = 0; i < (int)sizes.size(); i++) {
        if (ratio_name(sizes[i]) != name) continue;
        if (sizes[i] == screen) return i;
        if (best < 0) best = i;
        else if (!screen.h) best = px(i) > px(best) ? i : best;
        else if (off(i) < off(best) || (off(i) == off(best) && sizes[i].h < sizes[best].h)) best = i;
    }
    return best;
}

}  // namespace mkwvk::display_modes
