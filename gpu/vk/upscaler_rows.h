/* The Insert menu's scaling rows (vk_menu.cpp), as plain functions: what the Render resolution row steps through for each
   Upscaler, and the names it shows for DLSS's (and FSR 3.1's and FSR 4's) modes. No Vulkan, no menu: tests/test_upscaler_rows.cpp checks them. */
#pragma once

#include <algorithm>
#include <cmath>

namespace mkwvk::upscaler_rows {

/// The Upscaler row (opts.dlss, RX_VK_DLSS): the game's own network, NVIDIA DLSS, NVIDIA DLAA, AMD FSR 3.1 (built from AMD's
/// shader sources: gpu/vk/vk_fsr3.cpp), AMD FSR 4 and its native anti-aliasing (AMD's library, a build with FSR 4:
/// gpu/vk/vk_fsr4.cpp), and Off: no upscaler at all, the game's network left out too and its input stretched to the output
/// with a plain linear filter (vk_temporal.cpp temporal_off: the fastest, and the roughest picture). The saved values keep
/// their meaning: each came after the ones before, so a saved 1 or 2 is still DLSS or DLAA and a saved 3 FSR 3.1
enum { GAME = 0, DLSS = 1, DLAA = 2, FSR3 = 3, FSR4 = 4, FSR4_AA = 5, OFF = 6, kUpscalers = 7 };
/// One bit per upscaler that can run here (1 << GAME and 1 << OFF always: the menu's upscalers_available()): what the row
/// steps through
inline unsigned bit(int upscaler) { return 1u << upscaler; }
/// The DLSS and DLAA bits, for a caller that knows only whether DLSS runs
inline unsigned dlss_bits(bool dlss_ok) { return dlss_ok ? bit(DLSS) | bit(DLAA) : 0u; }
/// FSR 4's two, the same way (fsr4_ok: fsr4_why() == FSR_OK)
inline unsigned fsr4_bits(bool fsr4_ok) { return fsr4_ok ? bit(FSR4) | bit(FSR4_AA) : 0u; }
inline bool is_dlss(int upscaler) { return upscaler == DLSS || upscaler == DLAA; }
inline bool is_fsr4(int upscaler) { return upscaler == FSR4 || upscaler == FSR4_AA; }
/// The 3D drawn at full size, the upscaler anti-aliasing alone (DLAA, FSR 4's native AA): the Render resolution row does not
/// apply
inline bool full_scale(int upscaler) { return upscaler == DLAA || upscaler == FSR4_AA; }
/// FSR 3.1 and FSR 4 scale from a smaller 3D at the shares DLSS's modes use: Quality 1.5x (67%), Balanced 1.7x (58%),
/// Performance 2x (50%) and Ultra Performance 3x (33%) per axis, AMD's own ratios too
inline bool scales_by_mode(int upscaler) { return upscaler == DLSS || upscaler == FSR3 || upscaler == FSR4; }

/// opts.resolution: 0 Auto (the game's dynamic resolution), else a share of the output in percent. 1 to 4 are the values from
/// before (100, 75, 67, 50: saved options keep their meaning); 5 and 6 came with DLSS's Balanced and Ultra Performance. Each
/// is a twelfth of the output (vk_menu.cpp twelfths): 8/12, 7/12, 6/12 and 4/12 are DLSS's own ratios. 7 to 9
/// came with the render-resolution slider (step_render): 5/12, 10/12 and 11/12
constexpr int kPercent[] = {0, 100, 75, 67, 50, 58, 33, 42, 83, 92};
constexpr int kValues = sizeof kPercent / sizeof kPercent[0];
inline int percent(int v) { return v > 0 && v < kValues ? kPercent[v] : 0; }

/// The steps the Render resolution row takes, best first after Auto. DLSS: its Quality, Balanced, Performance and Ultra
/// Performance; Auto too, since DLSS follows the game's dynamic size (vk_dlss.cpp makes its feature again when the size
/// leaves the mode's range; FSR 4's context takes any size up to the output). DLAA, FSR 4's native AA: none (the 3D is drawn at
/// full size)
/// Off: the game's and 33% (no network to have one for each share: any size is stretched the same way), Auto too (the game's
/// dynamic size, stretched as it comes)
/// FSR 3.1 (no Native of its own, so not on step_render's slider): Auto, 100 % (FSR 3.1's native AA: it takes any size up to the
/// output), then the slider's stops, 92 % down to 33 % (players asked for sharper than Quality without going to full size)
constexpr int kGameSteps[] = {0, 1, 2, 3, 4};
constexpr int kDlssSteps[] = {0, 3, 5, 4, 6};
constexpr int kFsr3Steps[] = {0, 1, 9, 8, 2, 3, 5, 4, 6};
constexpr int kOffSteps[] = {0, 1, 2, 3, 4, 6};
inline const int* steps(int upscaler, int& n) {
    if (upscaler == FSR3) return n = sizeof kFsr3Steps / sizeof kFsr3Steps[0], kFsr3Steps;
    if (scales_by_mode(upscaler)) return n = sizeof kDlssSteps / sizeof kDlssSteps[0], kDlssSteps;
    if (upscaler == OFF) return n = sizeof kOffSteps / sizeof kOffSteps[0], kOffSteps;
    return n = sizeof kGameSteps / sizeof kGameSteps[0], kGameSteps;
}

/// DLSS's name for a Render resolution value: nullptr for Auto, or a share that is not one of its modes
inline const char* dlss_mode_name(int v) {
    switch (percent(v)) {
    case 67: return "Quality";
    case 58: return "Balanced";
    case 50: return "Performance";
    case 33: return "Ultra Performance";
    default: return nullptr;
    }
}

/// The Render resolution row's next value from v (dir +1 Right, -1 Left), wrapping. A value that is not one of the upscaler's
/// steps (one saved with another upscaler) moves to its neighbour by size. DLAA, FSR 4's native AA: v, unchanged
inline int step(int upscaler, int v, int dir) {
    if (full_scale(upscaler)) return v;
    int n = 0;
    const int* s = steps(upscaler, n);
    for (int i = 0; i < n; i++)
        if (s[i] == v) return s[(i + (dir > 0 ? 1 : n - 1)) % n];
    // (not a step: where it would sit, Auto first and then larger shares first)
    const int p = percent(v);
    int pos = 1;
    while (pos < n && percent(s[pos]) > p) pos++;
    return dir > 0 ? s[pos % n] : s[pos - 1];
}

/// v as one of the upscaler's steps: itself when it is one, else the step nearest in size (the larger on a tie). Auto stays
/// Auto. DLAA, FSR 4's native AA: v, unchanged (its row does not apply; v comes back with the next upscaler)
inline int snap(int upscaler, int v) {
    if (full_scale(upscaler) || percent(v) == 0) return percent(v) == 0 ? 0 : v;
    int n = 0;
    const int* s = steps(upscaler, n);
    int best = v, best_d = 1000;
    for (int i = 0; i < n; i++) {
        if (s[i] == v) return v;
        if (!percent(s[i])) continue;
        const int d = percent(s[i]) > percent(v) ? percent(s[i]) - percent(v) : percent(v) - percent(s[i]);
        if (d < best_d || (d == best_d && percent(s[i]) > percent(best))) best = s[i], best_d = d;
    }
    return best;
}

/// The Upscaler row's next value from v (dir +1 / -1), wrapping over the choices there are (available: bit() of each that
/// runs here). A value that cannot run here steps as the game's own does
inline int step_upscaler(int v, int dir, unsigned available) {
    available |= bit(GAME);
    int at = v < 0 || v >= kUpscalers || !(available & bit(v)) ? GAME : v;
    for (int i = 0; i < kUpscalers; i++) {
        at = (at + (dir > 0 ? 1 : kUpscalers - 1)) % kUpscalers;
        if (available & bit(at)) return at;
    }
    return GAME;
}
/// The same, where the choices are DLSS and DLAA only when dlss_ok (a build with NVIDIA's SDK, on a GPU DLSS runs on)
inline int step_upscaler(int v, int dir, bool dlss_ok) { return step_upscaler(v, dir, bit(GAME) | dlss_bits(dlss_ok)); }

// ---- Native under its engine. The menu's Upscaler row chooses the engine (the game's own, NVIDIA DLSS,
// AMD FSR 3.1, AMD FSR 4, or Off: none); DLAA is DLSS's Native and FSR 4's native AA FSR 4's, the Render resolution row's first step after
// Auto where the engine has one: Auto, Native, then the engine's modes. The saved values keep their meaning (opts.dlss 2 is
// still DLAA, 5 FSR 4's native AA); Native leaves opts.resolution as it was, for when a mode comes back.
/// The engine of an Upscaler value: DLAA is DLSS's, FSR 4's native AA FSR 4's
inline int engine(int upscaler) { return upscaler == DLAA ? DLSS : upscaler == FSR4_AA ? FSR4 : upscaler; }
/// An engine's Native (its anti-aliasing at full size), or -1 where it has none (the game's own, FSR 3.1)
inline int native_of(int engine) { return engine == DLSS ? DLAA : engine == FSR4 ? FSR4_AA : -1; }

/// The Upscaler row's next value by engine (dir +1 / -1), wrapping over the engines that run here (available: bit() of each
/// value that runs). From a Native to an engine that has one: its Native; else the engine's scaled form. A value that
/// cannot run here steps as the game's own does
inline int step_engine(int upscaler, int dir, unsigned available) {
    constexpr int kEngines[] = {GAME, DLSS, FSR3, FSR4, OFF};   // (Off: no upscaler, last)
    constexpr int n = sizeof kEngines / sizeof kEngines[0];
    available |= bit(GAME);
    const bool native = full_scale(upscaler);
    const auto runs = [&](int e) {   // (the value e is chosen as here, or -1 when it does not run)
        const int nat = native_of(e);
        if (native && nat >= 0 && (available & bit(nat))) return nat;
        if (available & bit(e)) return e;
        if (nat >= 0 && (available & bit(nat))) return nat;
        return -1;
    };
    int at = 0;
    for (int i = 0; i < n; i++)
        if (kEngines[i] == engine(upscaler)) at = i;
    if (upscaler < 0 || upscaler >= kUpscalers || !(available & bit(upscaler))) at = 0;
    for (int i = 0; i < n; i++) {
        at = (at + (dir > 0 ? 1 : n - 1)) % n;
        const int v = runs(kEngines[at]);
        if (v >= 0) return v;
    }
    return GAME;
}

/// The render-resolution slider of the engines with a Native (DLSS, FSR 4), as a PC game's render
/// scale: the twelfths from 11/12 down to 4/12 that DLSS takes, after Auto and Native. DLSS's modes fall on it (Quality 8/12,
/// Balanced 7/12, Performance 6/12, Ultra Performance 4/12); 11/12 to 9/12 are scales between Native and Quality, which DLSS
/// takes in its Quality mode (vk_dlss.cpp prepare: that mode's range is 50-100 %). Not 5/12 (42 %): no DLSS mode takes it
/// (Ultra Performance's range is 33 % alone; in game 2026-11-10, "no DLSS mode takes the render size 2134x900 for 5120x2160")
constexpr int kSlider[] = {9, 8, 2, 3, 5, 4, 6};   // (92, 83, 75, 67, 58, 50, 33 %)
constexpr int kSliderN = sizeof kSlider / sizeof kSlider[0];
/// v as one of the slider's stops: itself, or the nearest by size (100 % is the nearest stop below Native, 92 %). Auto stays
inline int slider_snap(int v) {
    if (!percent(v)) return 0;
    int best = kSlider[0], best_d = 1000;
    for (int s : kSlider) {
        const int d = percent(s) > percent(v) ? percent(s) - percent(v) : percent(v) - percent(s);
        if (d < best_d) best = s, best_d = d;
    }
    return best;
}
/// The Render resolution after a change of Upscaler or a preset: with an engine that has a Native, the slider's stop nearest it
/// (its own value when it is one); else snap() as before
inline int snap_render(int upscaler, int v) { return native_of(engine(upscaler)) >= 0 ? slider_snap(v) : snap(upscaler, v); }

/// The Render resolution row's next step from (upscaler, v), dir +1 Right / -1 Left, wrapping: with an engine that has a
/// Native, Auto, Native, then the slider (Native: *up the engine's Native, *res v unchanged; a stop or Auto: *up the engine's
/// scaled form). A value that is not a stop counts as its nearest. Engines without a Native: step() as before, *up unchanged
inline void step_render(int upscaler, int v, int dir, int* up, int* res) {
    *up = upscaler, *res = v;
    const int e = engine(upscaler), nat = native_of(e);
    if (nat < 0) {
        *res = step(upscaler, v, dir);
        return;
    }
    const int count = kSliderN + 2;   // (Auto, Native, then the stops)
    int pos = 0;
    if (upscaler == nat) {
        pos = 1;
    } else if (percent(v)) {
        const int sv = slider_snap(v);
        for (int i = 0; i < kSliderN; i++)
            if (kSlider[i] == sv) pos = i + 2;
    }
    pos = (pos + (dir > 0 ? 1 : count - 1)) % count;
    if (pos == 1) {
        *up = nat;
        return;
    }
    *up = e;
    *res = pos == 0 ? 0 : kSlider[pos - 2];
}
/// The Render resolution row has nothing to choose only for a full-size upscaler whose engine has no Native step (none now:
/// DLAA and FSR 4's native AA are their engines' Native)
inline bool render_fixed(int upscaler) { return full_scale(upscaler) && native_of(engine(upscaler)) < 0; }

/// The texture LOD bias the upscalers add to the game's world materials (the samplers it biases below 0: -0.5 everywhere, for
/// its network's 3/4 share, about log2(0.75)). share: the Render resolution's in 1/10000 of the output, 0 at Auto (the
/// game's dynamic size, 3/4 of the output in play). offset: DLSS's extra step, below.
///  - DLSS and DLAA: NVIDIA's guide (DLSS Programming Guide 3.5): the game's bias at native size + log2(render / output)
///    - 1.0, so DLSS's network gets the texture detail of the output's size and a little more, which it resolves over its
///    frames. The game's materials are tuned to -0.5 at 3/4, about 0 at native size, so on top of its -0.5 that is
///    log2(share / 0.75) + offset, offset -1 by default (RX_VK_DLSS_LOD_OFFSET): Quality (67 %) -1.17, Performance -1.58,
///    DLAA -0.58, Auto -1. Without the -1 DLSS was given the render size's own texture detail (Quality -0.17, DLAA and Auto
///    nothing), the soft textures it then upscaled
///  - FSR 3.1 and FSR 4: log2(share / 0.75) below 75 % (the mips the output's size wants), 0 at Auto and above
///  - the game's own, Off, FSR 4's native AA: 0
inline float upscale_lod_bias_for(int upscaler, int share, float offset) {
    if (is_dlss(upscaler)) {
        const float r = upscaler == DLAA ? 1.0f : share > 0 ? (float)share / 10000.0f : 0.75f;
        return std::min(0.0f, std::log2(r / 0.75f) + offset);
    }
    if (!scales_by_mode(upscaler) || share <= 0) return 0.0f;
    return std::min(0.0f, std::log2((float)share / 7500.0f));
}

/// The DLSS model row (opts.dlss_model, saved as dlss_model): which of NVIDIA's networks DLSS and DLAA draw with, by its
/// preset (NVSDK_NGX_DLSS_Hint_Render_Preset_*). 0 the transformer, preset K (the default here, the one tested); 1 the second
/// generation transformer, preset M (NVIDIA's own pick for Performance); 2 the same generation as tuned for Ultra
/// Performance, preset L; 3 NVIDIA's default, preset 0 (the runtime picks per mode, and a runtime update can change it)
enum { MODEL_K = 0, MODEL_M = 1, MODEL_L = 2, MODEL_DEFAULT = 3, kModels = 4 };
inline unsigned model_preset(int model) {
    switch (model) {
    case MODEL_M: return 13;
    case MODEL_L: return 12;
    case MODEL_DEFAULT: return 0;
    default: return 11;   // (K, also for a value past the row)
    }
}
/// The preset DLSS is asked for: RX_VK_DLSS_PRESET over the row (env: its value, or nullptr), a letter (A = 1 ... Z = 26,
/// either case) or 0 for NVIDIA's default; anything else leaves the row's
inline bool preset_env(const char* env) {
    return env && ((*env >= 'A' && *env <= 'Z') || (*env >= 'a' && *env <= 'z') || (env[0] == '0' && !env[1]));
}
inline unsigned dlss_preset(int model, const char* env) {
    if (!preset_env(env)) return model_preset(model);
    if (*env == '0') return 0;
    return (unsigned)(*env >= 'a' ? *env - 'a' + 1 : *env - 'A' + 1);
}
/// A preset's letter for the log ('A' + preset - 1), or '0' for NVIDIA's default
inline char preset_letter(unsigned preset) { return preset ? (char)('A' + preset - 1) : '0'; }

/// Rows with nothing else to choose here (shown without arrows; Left and Right leave the saved value alone): the Upscaler
/// where DLSS cannot run (dlss_ok: dlss_why() == DLSS_OK), the Render resolution with DLAA or FSR 4's native AA (the 3D at
/// full size), the DLSS model while DLSS and DLAA are not drawing
inline bool upscaler_fixed(bool dlss_ok) { return !dlss_ok; }
/// The same, from the mask of what can run: fixed when only the game's own can (the menu's mask always has Off too, so there
/// the row is never fixed)
inline bool upscaler_fixed(unsigned available) { return (available & ~bit(GAME)) == 0; }
/// The Upscaler row is there only where something besides the game's own runs on this PC (available: one bit per upscaler
/// that runs, the menu's upscalers_available()): a build without DLSS or FSR 4 on a GPU FSR 3.1 does not run on, or DLSS
/// built for a GPU it cannot run on, has the game's own alone, and the row is hidden rather than shown fixed on it. Off runs
/// everywhere (the menu's mask has it), so the menu always shows the row
inline bool upscaler_shown(unsigned available) { return !upscaler_fixed(available); }
inline bool resolution_fixed(int upscaler) { return full_scale(upscaler); }
/// The DLSS model row: shown only where DLSS runs (dlss_ok: dlss_why() == DLSS_OK; a build without DLSS, or a GPU or
/// driver it cannot run on, hides it), with nothing to choose while the game's own upscales
inline bool model_shown(bool dlss_ok) { return dlss_ok; }
/// (env_set: RX_VK_DLSS_PRESET names a preset, which wins over the row for this run)
inline bool model_fixed(int upscaler, bool env_set) { return !is_dlss(upscaler) || env_set; }
/// The Window scaling filter does something only when the window's size is not the game's picture's (the Output resolution);
/// a size not known yet (0) counts as different
inline bool filter_needed(unsigned picture_w, unsigned picture_h, unsigned window_w, unsigned window_h) {
    return !picture_w || !window_w || picture_w != window_w || picture_h != window_h;
}

} // namespace mkwvk::upscaler_rows
