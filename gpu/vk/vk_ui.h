// (hookshot) How the overlays look (vk_menu.cpp draws the Insert menu, the FPS counter and the start-up screen with these): in
// the style of the game's own pause and options screens, so they sit naturally over it. Our own drawing on ImGui's draw lists
// (no texture, icon or ornament of the game's); the font is the game's own menu font from the player's files, else Libertinus
// (vk_imgui.cpp). Sizes are points of a 720-line window times the unit (unit()). Every text sits on its own background at a
// contrast of 4.5:1 or more (WCAG), a white game pixel behind at worst: gold only for the selected value, with dark text on it.
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "imgui.h"
#include "vk_imgui.h"
#include "vk_menu_i18n.h"   // (every text below is looked up in the menu's language first: i18n::tr)

namespace mkwvk::ui {

constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline ImU32 alpha(ImU32 c, float k) {
    const int a = (int)((c >> IM_COL32_A_SHIFT & 0xFF) * k);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)std::clamp(a, 0, 255) << IM_COL32_A_SHIFT);
}

// ---- the palette ----
constexpr ImU32 kInk = rgba(240, 232, 214), kInkBright = rgba(255, 250, 240), kSoft = rgba(214, 204, 184), kDim = rgba(168, 156, 136),
                kGold = rgba(222, 152, 40), kGoldHi = rgba(238, 168, 52), kGoldInk = rgba(36, 24, 8), kAccent = rgba(214, 170, 96),
                kEdge = rgba(122, 98, 64, 150), kCellDark = rgba(30, 25, 19, 246), kCellName = rgba(60, 47, 32, 240),
                kCellNameSel = rgba(92, 70, 42, 248), kBronze = rgba(84, 64, 38), kColumn = rgba(34, 26, 17, 228),
                kBannerTop = rgba(184, 146, 96, 246), kBanner = rgba(176, 138, 88, 246), kBannerInk = rgba(46, 30, 12),
                kBannerEdge = rgba(70, 48, 24), kTabInset = rgba(84, 56, 30, 250), kBox = rgba(36, 28, 19, 248),
                kBoxEdge = rgba(122, 98, 64, 220), kBar = rgba(22, 17, 11, 232), kListen = rgba(150, 120, 40);
constexpr int kDimAlpha = 185;   // (the game behind the menu)

/// The unit: pixels per point. By the window's height and the Menu size (1 Small, 2 Medium, 3 Large; anything else, as the
/// 0 Auto of older options files: Medium, whatever the input); *margin: the edge kept clear (5% at Large, the TV size)
inline float unit(float h, int size, float* margin) {
    if (size < 1 || size > 3) size = 2;
    const float k = size == 1 ? 0.85f : size == 3 ? 1.3f : 1.0f;
    *margin = h * (size == 3 ? 0.05f : 0.025f);
    return h / 720.0f * k;
}

struct Ctx {
    ImDrawList* d;
    Fonts f;
    float s;      // (pixels per point)
    float w, h;   // (the window)
    float safe;   // (the margin kept clear at each edge, pixels)
    bool pad;     // (a controller was the last input: its buttons in the prompts)
    int confirm_dot = 3, back_dot = 1;   // (the face buttons that confirm and go back: vk_menu_keys.h dot, 3 the bottom, 1 the right)
};

inline ImVec2 size_of(ImFont* font, float px, const char* t, float wrap = 0) {
    return font->CalcTextSizeA(px, FLT_MAX, wrap, i18n::tr(t));
}
/// (hookshot) Shrink to fit: the size a text is drawn at to be at most room wide. px when it fits there; else the largest size
/// below it, on steps of step, down to min_px at the least; 0 when it fits at none (not drawn). width(size): the text's width
/// at a size (size_of's .x). The first guess scales px by room over the width, as a text's width grows with its size; from it
/// a step at a time, up while the next still fits, down while it does not, as the glyphs' rounding can put the guess either
/// side. (Sizes on steps: few sizes for the font atlas to bake)
template <class Width>
float fit_px(float px, float min_px, float step, float room, Width width) {
    const float w = width(px);
    if (w <= room) return px;
    if (room <= 0 || w <= 0 || step <= 0) return 0;
    float f = std::floor(px * room / w / step) * step;
    while (f + step < px && width(f + step) <= room) f += step;
    while (f >= min_px && width(f) > room) f -= step;
    return f >= min_px ? f : 0;
}
inline float text(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t, float wrap = 0) {
    t = i18n::tr(t);
    c.d->AddText(font, px, p, col, t, nullptr, wrap);
    return font->CalcTextSizeA(px, FLT_MAX, wrap, t).x;
}
/// Text wrapped at wrap pixels, its lines 1.3 em apart (ImGui's own wrap sets them 1 em apart: too tight for reading); its
/// height. draw false: measured only
inline float para(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t, float wrap, bool draw = true) {
    t = i18n::tr(t);
    const char* end = t + strlen(t);
    float y = 0;
    while (t < end) {
        const char* cut = font->CalcWordWrapPosition(px, t, end, wrap);
        if (cut == t) {   // (a word wider than the line: a letter at least, a whole UTF-8 one)
            cut = t + 1;
            while (cut < end && ((unsigned char)*cut & 0xC0) == 0x80) cut++;
        }
        if (draw) c.d->AddText(font, px, ImVec2(p.x, p.y + y), col, t, cut);
        y += px * 1.3f;
        t = cut;
        while (t < end && (*t == ' ' || *t == '\n')) t++;
    }
    return y > 0 ? y - px * 0.3f : 0;
}
/// The top to draw at for capitals centred on y: from the font's own ascent (fonts differ), a cap height of 0.7 em
inline float mid_top(ImFont* font, float px, float y) { return y - font->GetFontBaked(px)->Ascent + 0.35f * px; }
inline float text_mid(Ctx& c, ImFont* font, float px, float x, float y, ImU32 col, const char* t) {
    return text(c, font, px, ImVec2(x, mid_top(font, px, y)), col, t);
}
inline void text_mid_centre(Ctx& c, ImFont* font, float px, float cx, float y, ImU32 col, const char* t) {
    text_mid(c, font, px, cx - size_of(font, px, t).x * 0.5f, y, col, t);
}
inline void text_mid_right(Ctx& c, ImFont* font, float px, float x, float y, ImU32 col, const char* t) {
    text_mid(c, font, px, x - size_of(font, px, t).x, y, col, t);
}
inline void text_centre(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t) {
    text(c, font, px, ImVec2(p.x - size_of(font, px, t).x * 0.5f, p.y), col, t);
}
/// Capitals with extra space between the letters (gap in pixels), centred on x
inline void spaced(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t, float gap) {
    // (a letter at a time, each a whole UTF-8 character: a translated title has accents and other scripts)
    t = i18n::tr(t);
    const auto next = [](const char* q) {
        const char* e = q + 1;
        while (*e && ((unsigned char)*e & 0xC0) == 0x80) e++;
        return e;
    };
    float total = 0;
    for (const char* q = t; *q; q = next(q)) total += font->CalcTextSizeA(px, FLT_MAX, 0, q, next(q)).x + gap;
    float x = p.x - (total - gap) * 0.5f;
    for (const char* q = t; *q; q = next(q)) {
        c.d->AddText(font, px, ImVec2(x, p.y), col, q, next(q));
        x += font->CalcTextSizeA(px, FLT_MAX, 0, q, next(q)).x + gap;
    }
}
/// A soft shadow under a rectangle: layers growing outwards, fading
inline void shadow(Ctx& c, ImVec2 a, ImVec2 b, float spread, float strength, float dy) {
    const int n = 16;
    for (int i = n; i >= 1; i--) {
        const float k = (float)i / n, e = spread * k;
        c.d->AddRectFilled(a - ImVec2(e, e) + ImVec2(0, dy), b + ImVec2(e, e) + ImVec2(0, dy),
                           rgba(0, 0, 0, (int)(255 * strength * (1 - k) * (1 - k) / 3.5f)), e);
    }
}
inline void triangle(Ctx& c, ImVec2 m, float size, int dir, ImU32 col) {   // (dir: 0 left, 1 right, 2 up, 3 down)
    const float a = size * 0.5f, b = size * 0.42f;
    switch (dir) {
    case 0: c.d->AddTriangleFilled(m + ImVec2(b, -a), m + ImVec2(-b, 0), m + ImVec2(b, a), col); break;
    case 1: c.d->AddTriangleFilled(m + ImVec2(-b, -a), m + ImVec2(b, 0), m + ImVec2(-b, a), col); break;
    case 2: c.d->AddTriangleFilled(m + ImVec2(-a, b), m + ImVec2(0, -b), m + ImVec2(a, b), col); break;
    default: c.d->AddTriangleFilled(m + ImVec2(-a, -b), m + ImVec2(a, -b), m + ImVec2(0, b), col); break;
    }
}
inline void diamond(Ctx& c, ImVec2 m, float r, ImU32 col) {
    c.d->AddQuadFilled(m + ImVec2(0, -r), m + ImVec2(r, 0), m + ImVec2(0, r), m + ImVec2(-r, 0), col);
}
/// A divider: a fine faint line with a small flourish of two mirrored curls at its middle
inline void flourish(Ctx& c, float x0, float x1, float y, ImU32 col) {
    const float s = c.s, m = (x0 + x1) * 0.5f, t = std::max(1.0f, 0.8f * s);
    c.d->AddLine(ImVec2(x0, y), ImVec2(m - 30 * s, y), alpha(col, 0.35f), t);
    c.d->AddLine(ImVec2(m + 30 * s, y), ImVec2(x1, y), alpha(col, 0.35f), t);
    for (int side = -1; side <= 1; side += 2) {
        const float k = (float)side;
        c.d->AddBezierCubic(ImVec2(m + k * 4 * s, y), ImVec2(m + k * 11 * s, y - 7 * s), ImVec2(m + k * 21 * s, y - 6 * s),
                            ImVec2(m + k * 19 * s, y), alpha(col, 0.8f), 1.3f * s);
        c.d->AddBezierCubic(ImVec2(m + k * 19 * s, y), ImVec2(m + k * 17 * s, y + 5 * s), ImVec2(m + k * 11 * s, y + 4 * s),
                            ImVec2(m + k * 12 * s, y + 1 * s), alpha(col, 0.8f), 1.3f * s);
    }
    diamond(c, ImVec2(m, y), 2.2f * s, alpha(col, 0.8f));
}
/// The selection: a bright outline with a soft warm-white glow around it
inline void glow(Ctx& c, ImVec2 a0, ImVec2 b0) {
    const float s = c.s;
    for (int i = 6; i >= 1; i--)
        c.d->AddRect(a0 - ImVec2(i * 1.5f * s, i * 1.5f * s), b0 + ImVec2(i * 1.5f * s, i * 1.5f * s), rgba(255, 244, 220, 12 + 4 * (6 - i)),
                     2 * s, 2.2f * s);
    c.d->AddRect(a0 - ImVec2(1.5f * s, 1.5f * s), b0 + ImVec2(1.5f * s, 1.5f * s), kInkBright, 2 * s, 3 * s);
}
/// A gold fill, lighter at its top
inline void gold(Ctx& c, ImVec2 a0, ImVec2 b0) { c.d->AddRectFilledMultiColor(a0, b0, kGoldHi, kGoldHi, kGold, kGold); }

// ---- the button prompts: the keys, or a controller's buttons by position (any layout: the dot lit) ----
enum Glyph { kUpDown, kLeftRight, kTabs, kConfirm, kBack, kKey, kCompact };
struct Prompt {
    Glyph g;
    const char* what;
    const char* key = nullptr;   // (kKey: a key's name, shown with the keys and with a controller alike)
};
/// One glyph at p (its top left; the line px + 10 points high); its width. draw false: measured only
inline float glyph(Ctx& c, float px, ImVec2 p, const Prompt& pr, bool draw) {
    const float s = c.s, hh = px + 10 * s, my = p.y + hh * 0.5f, kpx = px * 0.84f;
    const ImU32 cap = kInk, ink = rgba(40, 30, 20), dim = rgba(150, 140, 126);
    auto key = [&](float x, const char* label) {   // (a key's cap, its label in it)
        const float w = label ? std::max(hh, size_of(c.f.strong, kpx, label).x + 14 * s) : hh;
        if (draw) {
            c.d->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + hh), cap, 4 * s);
            if (label) text_mid_centre(c, c.f.strong, kpx, x + w * 0.5f, my, ink, label);
        }
        return w;
    };
    if (pr.g == kKey) return key(p.x, pr.key);
    if (pr.g == kCompact) {
        if (!c.pad) return key(p.x, pr.key ? pr.key : "RShift");
        const float r = hh * 0.5f;
        const ImVec2 m(p.x + r, my);
        if (draw) {
            c.d->AddCircleFilled(m, r, cap);
            const ImVec2 at[4] = {m + ImVec2(0, -r * 0.5f), m + ImVec2(r * 0.5f, 0), m + ImVec2(-r * 0.5f, 0), m + ImVec2(0, r * 0.5f)};
            for (int i = 0; i < 4; i++) {
                if (i == 2) c.d->AddCircleFilled(at[i], r * 0.27f, ink);
                else c.d->AddCircle(at[i], r * 0.22f, dim, 0, std::max(1.0f, s));
            }
        }
        return 2 * r;
    }
    if (!c.pad) {
        switch (pr.g) {
        case kUpDown:
        case kLeftRight: {
            const bool v = pr.g == kUpDown;
            const float w = key(p.x, nullptr);
            key(p.x + w + 3 * s, nullptr);
            if (draw) triangle(c, ImVec2(p.x + w * 0.5f, my), 8 * s, v ? 2 : 0, ink), triangle(c, ImVec2(p.x + w * 1.5f + 3 * s, my), 8 * s, v ? 3 : 1, ink);
            return 2 * w + 3 * s;
        }
        case kTabs: {
            const float w = key(p.x, "Q");
            return w + 3 * s + key(p.x + w + 3 * s, "E");
        }
        case kConfirm: return key(p.x, "Enter");
        default: return key(p.x, "Esc");
        }
    }
    const float r = hh * 0.5f;
    const ImVec2 m(p.x + r, my);
    if (pr.g == kTabs) {   // (the shoulder buttons)
        float x = p.x;
        for (const char* l : {"L", "R"}) {
            const float w = hh * 1.25f;
            if (draw) {
                c.d->AddRectFilled(ImVec2(x, p.y + 2 * s), ImVec2(x + w, p.y + hh - 2 * s), cap, (hh - 4 * s) * 0.5f);
                text_mid_centre(c, c.f.strong, px * 0.8f, x + w * 0.5f, my, ink, l);
            }
            x += w + 3 * s;
        }
        return x - p.x - 3 * s;
    }
    if (!draw) return 2 * r;
    c.d->AddCircleFilled(m, r, cap);
    if (pr.g == kUpDown || pr.g == kLeftRight) {   // (the D-pad: a cross, the arms in use dark)
        const float arm = r * 0.26f, len = r * 0.68f;
        const bool v = pr.g == kUpDown;
        c.d->AddRectFilled(m - ImVec2(arm, len), m + ImVec2(arm, len), v ? ink : dim, arm * 0.5f);
        c.d->AddRectFilled(m - ImVec2(len, arm), m + ImVec2(len, arm), v ? dim : ink, arm * 0.5f);
        c.d->AddRectFilled(m - ImVec2(arm, arm), m + ImVec2(arm, arm), ink);
    } else {   // (the face buttons, four dots: the bottom one confirms, the right one goes back)
        const int lit = pr.g == kConfirm ? c.confirm_dot : c.back_dot;   // (0 top, 1 right, 2 left, 3 bottom)
        const ImVec2 at[4] = {m + ImVec2(0, -r * 0.5f), m + ImVec2(r * 0.5f, 0), m + ImVec2(-r * 0.5f, 0), m + ImVec2(0, r * 0.5f)};
        for (int i = 0; i < 4; i++) {
            if (i == lit) c.d->AddCircleFilled(at[i], r * 0.27f, ink);
            else c.d->AddCircle(at[i], r * 0.22f, dim, 0, std::max(1.0f, s));
        }
    }
    return 2 * r;
}
/// The prompts, the label before its button as the game's own: their width (draw false: measured only)
inline float prompts(Ctx& c, ImVec2 p, const Prompt* list, int n, bool draw) {
    const float s = c.s, px = 17 * s, hh = px + 10 * s, x0 = p.x;
    for (int i = 0; i < n; i++) {
        if (draw) text_mid(c, c.f.body, px, p.x, p.y + hh * 0.5f, kInk, list[i].what);
        p.x += size_of(c.f.body, px, list[i].what).x + 8 * s;
        p.x += glyph(c, px, p, list[i], draw) + 26 * s;
    }
    return n ? p.x - x0 - 26 * s : 0;
}
/// The prompt strip's top: the same at any prompts (the menu lays out above it before it draws the strip, last, on top)
inline float prompt_bar_top(const Ctx& c) { return c.h - c.safe - (17 * c.s + 10 * c.s) - 12 * c.s; }
/// The prompts at the bottom right on a solid strip, drawn over whatever is under it; its top (prompt_bar_top). Prompts wider
/// than the window between its margins take the whole width, and smaller ones still if they must
inline float prompt_bar(Ctx& c, const Prompt* list, int n) {
    const float s = c.s, px = 17 * s, ph = px + 10 * s, py = c.h - c.safe - ph;
    Ctx k = c;   // (the prompts' own scale: smaller only when they do not fit the width)
    const float room = c.w - 2 * c.safe - 20 * s, full = prompts(c, ImVec2(0, 0), list, n, false);
    if (full > room) k.s = s * room / full;
    const float pw = prompts(k, ImVec2(0, 0), list, n, false);
    const float x0 = std::max(0.0f, c.w - c.safe - pw - 34 * s);
    if (x0 > 0) c.d->AddRectFilledMultiColor(ImVec2(std::max(0.0f, x0 - 80 * s), py - 12 * s), ImVec2(x0, c.h), alpha(kBar, 0), kBar, kBar,
                                             alpha(kBar, 0));
    c.d->AddRectFilled(ImVec2(x0, py - 12 * s), ImVec2(c.w, c.h), kBar);
    c.d->AddLine(ImVec2(x0, py - 12 * s), ImVec2(c.w, py - 12 * s), alpha(kEdge, 0.8f), std::max(1.0f, s));
    const float kh = 17 * k.s + 10 * k.s;   // (smaller prompts: centred in the strip's height)
    prompts(k, ImVec2(c.w - c.safe - pw - 10 * s, py + (ph - kh) * 0.5f), list, n, true);
    return py - 12 * s;
}
/// The tan banner across the top, hh pixels high, its dark lower edge
inline void banner(Ctx& c, float hh) {
    const float s = c.s;
    c.d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(c.w, hh), kBannerTop, kBannerTop, kBanner, kBanner);
    c.d->AddRectFilled(ImVec2(0, hh - 3 * s), ImVec2(c.w, hh), kBannerEdge);
    c.d->AddLine(ImVec2(0, hh - 4 * s), ImVec2(c.w, hh - 4 * s), rgba(226, 192, 138, 140), std::max(1.0f, s));
}
/// A box (the help, the restart question): opaque, a fine edge, a soft shadow
inline void box(Ctx& c, ImVec2 a, ImVec2 b) {
    shadow(c, a, b, 18 * c.s, 0.8f, 4 * c.s);
    c.d->AddRectFilled(a, b, kBox);
    c.d->AddRect(a, b, kBoxEdge, 0, 1.5f * c.s);
}
/// A button as the game's pause menu draws its own: a warm cell with a fine inner frame; selected, gold with the outline
inline void button(Ctx& c, ImVec2 a0, ImVec2 b0, const char* label, bool sel) {
    const float s = c.s;
    if (sel) gold(c, a0, b0);
    else c.d->AddRectFilled(a0, b0, kCellName);
    c.d->AddRect(a0 + ImVec2(4 * s, 4 * s), b0 - ImVec2(4 * s, 4 * s), sel ? rgba(36, 24, 8, 60) : rgba(190, 154, 98, 90), 0,
                 std::max(1.0f, s));
    c.d->AddRect(a0, b0, kBannerEdge, 0, std::max(1.0f, s));
    ImFont* f = sel ? c.f.strong : c.f.body;
    text_mid_centre(c, f, 20 * s, (a0.x + b0.x) * 0.5f, (a0.y + b0.y) * 0.5f, sel ? kGoldInk : kInk, label);
    if (sel) glow(c, a0, b0);
}

} // namespace mkwvk::ui
