// The Insert menu. Drawn with Dear ImGui over the window's image at the last step of a present (menu_record; vk_imgui.h, in the
// game's style: vk_ui.h), so it costs nothing while closed and it never touches the game's picture, nor what frame interpolation
// sees. The FPS counter and the start-up screen are drawn the same way. F1 always opens and closes it, Insert too by default.
// Up and Down choose, Left and Right change, Esc closes; the game gets no keyboard input while it is open.
// (hookshot) A gamepad's Home button, or Select and Start together, opens it too; the D-pad chooses and changes, the button
// the Switch's A is bound to changes too (and opens a page), the one B is bound to goes back (vk_menu_keys.h). The game gets no gamepad input while it is open either.
// Its Controls page binds the Switch's buttons to gamepad inputs and keys.
// (hookshot) The mouse is free while it is open (the window lets it go: WM_APP + 8, host/display.c and
// host/posix/display_sdl.c): the row under the cursor is selected, a click on a tab shows it, on a value's left or right
// half changes it down or up, on a toggle flips it, on a slider's track sets it (or drag it); the wheel scrolls the rows
// (never a value, wherever the cursor is); a right click is Esc. On the Cheats page and its lists a click is Enter (on a
// choice's or an amount's left or right half, Left or Right) and a right click goes back. The drawing records each control's
// place and the mouse is tested against that (vk_menu_hit.h).
// (hookshot) The rows that take a restart (Row::restart) are tagged "After restart" while their value is not the one the game
// started with, and the Graphics tab's help lists them. Closing the menu then asks whether to restart now (menu_restart.h):
// Later keeps the saved settings for the next start and is not asked again until they change; Restart now saves and
// closes the window as the player would, and the game starts again (WM_APP + 9: host/hos_relaunch.c).
#include <atomic>
#include <chrono>
#include <cmath>
#include <utility>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vk_paths.h"
#include "vk_menu.h"
#include "vk_menu_hit.h"
#include "vk_draw.h"   // (pipeline_cache_save: Restart now keeps what was built)
#include "oot_cheat_items.h"
#include "vk_progress.h"   // (StartupView, version_text: the start-up screen)
#include "vk_ui.h"
#include "vk_menu_keys.h"   // (which gamepad buttons confirm and go back: those A and B are bound to)         // (how it looks: Dear ImGui, vk_imgui.h)
#include "vk_dlss.h"
#include "../../host/hos_wide_size.h"   // (the widescreen picture's size, as host/hos_main.c works it out)
#include "vk_fsr3.h"      // (the Upscaler row: whether AMD FSR 3.1 runs on this GPU)
#include "vk_fsr3fi.h"    // (the Frame generation row: whether FSR 3.1 makes the frame)
#include "vk_fsr4.h"
#include "vk_temporal.h"
#include "vk_reflex.h"
#include "vk_antilag.h"
#include "upscaler_rows.h"
#include "menu_restart.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)
#ifndef HOOKSHOT_VERSION   // (nx2/build.py gives it to this file: nx2/version.py)
#define HOOKSHOT_VERSION "unknown"
#endif

namespace mkwvk {
HWND app_window();

Options opts;

namespace {
enum MenuTab {
    TAB_GRAPHICS = 0,
    TAB_AUDIO = 1,
    TAB_GAMEPLAY = 2,
    TAB_CONTROLS = 3,
    TAB_DEBUG = 4,   // (the OoT debug tools of #785, a tab of their own)
    TAB_COUNT = 5,
    TAB_MOTION = TAB_COUNT,   // (hookshot: not a tab of the strip, the Motion page that the Controls tab's Motion row
                              // opens: one past the strip's tabs, so a tab added before TAB_COUNT moves it along)
    TAB_PAGE = TAB_MOTION + 1   // (hookshot: the Controls page's rows, above its grid; no tab shows them)
};

struct Row {
    MenuTab tab;
    const char* name;
    int* value;
    int lo, hi, step;
    const char* (*text)(int v);
    const char* help;   // (shown under the list while the row is selected: what the row is, the same whatever its value)
    // (a line about the value chosen, under the help, or nullptr: hint(v, -1) is the one to show now for v, hint(v, 0),
    // hint(v, 1), ... every one v can have (those that depend on the other rows or on the GPU), until nullptr, so the
    // menu keeps the height of the tallest: measure_help)
    const char* (*hint)(int v, int variant) = nullptr;
    bool restart = false;   // (read once, at the start: a change applies after a restart)
    bool preset_exempt = false;   // (a change here leaves the Quality preset alone: the controls, the window, the counter,
                                  // frame generation)
    signed char cheat = -1;   // (a debug tool, on the Debug tab with the tools on: its index in the cheat_ tables, -1 none)
    bool heading = false;     // (a section's title on its tab, not a row: the selection steps over it)
};
/// A row's hints from a table, the value lo first (nullptr: none for that value)
const char* hint_in(const char* const* t, int n, int lo, int v, int variant) {
    return variant <= 0 && v - lo >= 0 && v - lo < n ? t[v - lo] : nullptr;
}
#define MENU_HINTS(fn, lo, ...)                                                                                              \
    const char* fn(int v, int variant) {                                                                                     \
        static const char* const t[] = {__VA_ARGS__};                                                                        \
        return hint_in(t, (int)(sizeof t / sizeof t[0]), lo, v, variant);                                                    \
    }
const char* tx_framegen(int v) {
    static const char* t[] = {"Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
    return t[v];
}
const char* tx_onoff(int v) { return v ? "On" : "Off"; }
const char* tx_profile(int v) { return v ? "Handheld" : "Docked"; }   // (opts.handheld: the Profile row)
const char* tx_menu_shortcut(int v) {
    // (F1 is never turned off: the menu is always one key away. Esc as well, as a PC game's pause menu)
    static const char* const names[] = {"Insert or F1", "F1 only", "Insert, F1 or Esc"};
    return names[std::clamp(v, 0, 2)];
}
const char* tx_hdr(int v) { return v == 1 ? "Expanded" : v == 2 ? "Native (the game's)" : "Off"; }
const char* tx_nits(int v) {
    static char b[24];
    snprintf(b, sizeof b, "%d nits", v);
    return b;
}
const char* tx_fps_cap(int v) {
    static const char* const t[] = {"60 (the console's)", "90", "120", "144", "165", "240"};
    return t[v < 0 ? 0 : v > 5 ? 5 : v];
}
// (hookshot) Motion controls: the choice, and whether a connected controller has a gyroscope (host/pad_sdl.c). Either can be
// chosen whatever is connected: a pad plugged in later is read when it comes
const char* tx_gyro(int v) {
    const int status = host && host->gyro_status ? host->gyro_status() : -1;
    if (status == 2) return v ? "On (gyro found)" : "Off (gyro found)";
    if (status == 1) return v ? "On (no gyro on this pad)" : "Off (no gyro on this pad)";
    if (status == 0) return v ? "On (no controller)" : "Off (no controller)";
    return v ? "On" : "Off";
}
const char* tx_mousecam(int v) { return v ? "Native" : "As the stick"; }
const char* tx_sens(int v) {
    static char b[12];
    snprintf(b, sizeof b, "%.2f", v / 100.0);
    return b;
}
// (the game's picture and the window it is shown in, as the last present had them: menu_note_present_sizes)
std::atomic<uint64_t> present_sizes{0};
bool filter_needed() {
    const uint64_t v = present_sizes.load(std::memory_order_relaxed);
    return upscaler_rows::filter_needed((unsigned)(v >> 48), (unsigned)(v >> 32) & 0xffff, (unsigned)(v >> 16) & 0xffff,
                                        (unsigned)v & 0xffff);
}
const char* tx_upscale(int v) {
    static const char* t[] = {"Off", "Catmull-Rom", "Lanczos"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];   // (a setting past the row: the label, not the next bytes)
    return t[v];
}
/// The Window scaling filter row's value: its filter, or that none is needed (the saved value stays, for another window size)
const char* tx_filter(int v) { return filter_needed() ? tx_upscale(v) : "Not needed (window = output)"; }
const char* tx_reflex(int v) {
    static const char* t[] = {"Off", "On", "On + Boost"};
    return t[reflex_available() ? std::clamp(v, 0, 2) : 0];
}
/// Reflex: on the value (the row is shown only where Reflex runs: row_shown)
const char* hn_reflex(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver paces the frames for less latency.",
                                    "As On, and the GPU's clocks are kept up as well."};
    return variant <= 0 ? t[std::clamp(v, 0, 2)] : nullptr;
}
const char* tx_antilag(int v) { return antilag_available() && v ? "On" : "Off"; }
/// Anti-Lag 2: on the value (the row is shown only where Anti-Lag 2 runs: row_shown)
const char* hn_antilag(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver delays each frame's start for less latency."};
    return variant <= 0 ? t[v ? 1 : 0] : nullptr;
}
// The Upscaler (opts.dlss): what scales the 3D up to the output. DLSS and DLAA only where dlss_why() lets them run, FSR 3.1
// only where vk_fsr3.cpp says it does, FSR 4 and its native AA only where fsr4_why() does; the menu shows (and steps from)
// what runs, and keeps the saved choice for a PC where it can
namespace ur = upscaler_rows;
/// One bit per upscaler that runs here (upscaler_rows.h bit). Each backend caches its own answer, so this costs nothing
unsigned upscalers_available() {
    return ur::bit(ur::GAME) | ur::dlss_bits(dlss_why() == DLSS_OK) | (fsr3::supported() ? ur::bit(ur::FSR3) : 0u) |
           ur::fsr4_bits(fsr4_why() == FSR_OK);
}
/// (FSR 3.1 has inputs of its own; DLSS and FSR 4 run on the frame chain they share, temporal_mode)
int upscaler() { return fsr3_wanted() ? (int)ur::FSR3 : temporal_mode(); }
const char* tx_upscaler(int) {
    static const char* t[ur::kUpscalers] = {"Game's own", "NVIDIA DLSS", "NVIDIA DLAA", "AMD FSR 3.1", "AMD FSR 4",
                                            "AMD FSR 4 Native AA"};
    // (by engine: DLAA and FSR 4's native AA are the Render resolution row's Native, upscaler_rows.h engine)
    return t[std::clamp(ur::engine(upscaler()), 0, ur::kUpscalers - 1)];
}
/// The Upscaler: the one shown (what runs, upscaler()), and with the game's own, why DLSS and FSR are not offered. Variants
/// (every one): each value, then the game's own with each of DLSS's reasons, then with each of FSR's
const char* hn_upscaler(int v, int variant) {
    static const char* const t[ur::kUpscalers] = {
        "The game's own network.",
        "NVIDIA's, from a smaller 3D. Softer than the game's own: set Sharpness to around 80%. Things that move by "
        "themselves can trail for now.",
        "NVIDIA's, the 3D at full size with its edges smoothed; the slowest. Softer than the game's own: set Sharpness to "
        "around 80%. Things that move by themselves can trail for now.",
        "AMD's, from a smaller 3D, on any GPU: built from AMD's own shaders. Things that move by themselves can trail for "
        "now.",
        "AMD's machine-learned upscaler, from a smaller 3D, with AMD's files beside the game. Things that move by themselves "
        "can trail for now.",
        "AMD's machine-learned upscaler, the 3D at full size with its edges smoothed; the slowest.",
    };
    static const char* const dlss_no[DLSS_WHY_COUNT] = {
        nullptr,
        "DLSS and DLAA: not in this build (built without NVIDIA's DLSS SDK, see the readme).",
        "DLSS and DLAA: off for this run (RX_VK_DLSS=0).",
        "DLSS and DLAA: need an NVIDIA RTX GPU.",
        "DLSS and DLAA: need a newer NVIDIA driver.",
        "DLSS and DLAA: DLSS's runtime was not found (the game folder's ngx folder, see the readme).",
        "DLSS and DLAA: NVIDIA's NGX on Linux loads DLSS's runtime only from a folder named in ASCII (see the readme).",
    };
    static const char* const fsr_no[FSR_WHY_COUNT] = {
        nullptr,
        "AMD FSR 4: not in this build (see the readme).",
        "AMD FSR 4: off for this run (RX_VK_FSR4=0).",
        "AMD FSR 4: AMD's files were not found beside the game (see the readme).",
        "AMD FSR 4: AMD's files have no FSR 4 for this GPU.",
    };
    // (now: the value shown and what runs here, with both engines' reasons; every one: as said above)
    int d = 0, f = 0;
    if (variant < 0) {
        v = std::clamp(upscaler(), 0, ur::kUpscalers - 1), d = (int)dlss_why(), f = (int)fsr4_why();
    } else if (v == ur::GAME) {
        if (variant < DLSS_WHY_COUNT) d = variant;
        else if (variant < DLSS_WHY_COUNT + FSR_WHY_COUNT - 1) f = variant - DLSS_WHY_COUNT + 1;
        else return nullptr;
    } else if (variant > 0) {
        return nullptr;
    }
    v = std::clamp(v, 0, ur::kUpscalers - 1);
    // (the reasons are said only where the engine is in the build: one without FSR does not tell an NVIDIA user why)
    const char* dn = dlss_built() || variant >= 0 ? dlss_no[d] : nullptr;
    const char* fn = fsr4_built() || variant >= 0 ? fsr_no[f] : nullptr;
    if (v != ur::GAME || (!dn && !fn)) return t[v];
    static char b[DLSS_WHY_COUNT][FSR_WHY_COUNT][320];
    snprintf(b[d][f], sizeof b[d][f], "%s%s%s%s%s", t[0], dn ? " " : "", dn ? dn : "", fn ? " " : "", fn ? fn : "");
    return b[d][f];
}
/// The DLSS model row (opts.dlss_model): NVIDIA's network by its preset, or RX_VK_DLSS_PRESET's for this run
const char* dlss_preset_env() {
    static const char* const e = getenv("RX_VK_DLSS_PRESET");
    return ur::preset_env(e) ? e : nullptr;
}
const char* tx_dlss_model(int v) {
    static const char* const t[] = {"Transformer (K)", "Transformer 2 (M)", "Transformer 2 (L)", "NVIDIA default"};
    if (const char* e = dlss_preset_env()) {
        static char b[48];
        const unsigned p = ur::dlss_preset(v, e);
        if (p) snprintf(b, sizeof b, "Preset %c (RX_VK_DLSS_PRESET)", ur::preset_letter(p));
        else snprintf(b, sizeof b, "NVIDIA default (RX_VK_DLSS_PRESET)");
        return b;
    }
    return t[std::clamp(v, 0, ur::kModels - 1)];
}
/// The DLSS model: the value chosen; variant 1: the game's own upscales (the row waits for DLSS or DLAA); variant 2: set for
/// this run by RX_VK_DLSS_PRESET
const char* hn_dlss_model(int v, int variant) {
    static const char* const t[] = {
        "NVIDIA's transformer model: the one tested most here.",
        "NVIDIA's second-generation transformer, its pick for Performance: sharper, at a little more GPU time.",
        "The second-generation transformer as NVIDIA tuned it for Ultra Performance, the smallest Render resolutions.",
        "Lets DLSS's runtime pick for each mode: what it picks can change with a runtime update.",
    };
    static const char* const unused = "Used by NVIDIA DLSS and DLAA: choose one in the Upscaler row.";
    static const char* const env = "Set for this run by RX_VK_DLSS_PRESET.";
    if (variant < 0) variant = dlss_preset_env() ? 2 : upscaler() == ur::GAME ? 1 : 0;
    if (variant == 1) return unused;
    if (variant == 2) return env;
    return variant == 0 ? t[std::clamp(v, 0, ur::kModels - 1)] : nullptr;
}
const char* tx_percent(int v) {
    static char b[8];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
/// The main screen's size, as host/hos_main.c takes its shape at the start (the Ultrawide row): false where it
/// is not known (Linux: the sizes are shown as the 16:9 outputs)
bool screen_size(uint32_t* w, uint32_t* h) {
#ifdef _WIN32
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw > 0 && sh > 0) return *w = (uint32_t)sw, *h = (uint32_t)sh, true;
#endif
    return false;
}
// The output the menu's sizes are shown for: the Output resolution row's (applied at the next start), so the rows below follow a change
// at once; the Handheld profile runs the game's own (1536x864)
struct Out {
    uint32_t w, h;
};
// The Output resolution's choices: the sizes offered (0 the game's own), then, last, a size of the player's own from
// hookshot_options.ini (output_res=<width>x<height>, host/nn_applet.c hos_output_parse: the same rules) when it is none of them.
// The ini keeps the size, not the choice's number: output_res=3840x2160
constexpr Out kOutputs[] = {{2048, 1152}, {2560, 1440}, {3840, 2160}, {1920, 1080}, {5120, 1440}, {5120, 2880}};
constexpr int kCustomOutput = std::size(kOutputs), kLastOutput = kCustomOutput;
Out custom_output{0, 0};   // (the player's own size; 0 x 0: none)
int output_choice(int v) { return v >= 0 && (v < kCustomOutput || (v == kCustomOutput && custom_output.w)) ? v : 0; }
Out output_at(int v) {
    v = output_choice(v);
    return v == kCustomOutput ? custom_output : kOutputs[v];
}
/// output_res= (or HOS_OUTPUT_RES) to a choice: "<width>x<height>" (each side a multiple of 8, rounded down; at least 640 x 360,
/// at most 5120 x 2880's pixels, 5:4 to 32:9), one of the sizes offered or the player's own (custom_output); the numbers saved
/// before (0 to 5) as theirs; anything else the game's own (host/nn_applet.c hos_output_parse: the same rules)
int parse_output(const char* text) {
    unsigned w = 0, h = 0;
    if (text && sscanf(text, "%u%*[xX]%u", &w, &h) == 2) {
        w &= ~7u, h &= ~7u;
        if (w < 640 || h < 360 || (uint64_t)w * h > 5120ULL * 2880 || (uint64_t)w * 4 < (uint64_t)h * 5 || (uint64_t)w * 9 > (uint64_t)h * 32)
            return 0;
        for (int i = 0; i < kCustomOutput; i++)
            if (kOutputs[i].w == w && kOutputs[i].h == h) return i;
        custom_output = {w, h};
        return kCustomOutput;
    }
    const int v = text ? atoi(text) : 0;
    return v >= 0 && v < kCustomOutput ? v : 0;
}
// (the choices from smallest to largest, by width then height: 1920x1080, 2048x1152, 2560x1440, 3840x2160, 5120x1440,
// 5120x2880, and the player's own in its place)
int step_output_choice(int id, int dir) {
    int order[kLastOutput + 1], n = 0;
    for (int i = 0; i <= kLastOutput; i++)
        if (output_choice(i) == i) order[n++] = i;
    std::sort(order, order + n, [](int a, int b) {
        const Out x = output_at(a), y = output_at(b);
        return x.w != y.w ? x.w < y.w : x.h < y.h;
    });
    const int at = (int)(std::find(order, order + n, output_choice(id)) - order);
    return order[std::clamp(at + dir, 0, n - 1)];
}
/// The picture an Output resolution choice makes: with Ultrawide on, at the screen's
/// shape and at most hos_wide_max_px() pixels, as host/hos_main.c works it out at the start (host/hos_wide_size.h); else the
/// 16:9 output. The game's own choice (0) as it is (its size comes from the game's graphics scale). *native: the screen's own size
Out picture_for(int v, bool* native = nullptr) {
    v = output_choice(v);
    Out o = output_at(v);
    uint32_t sw = 0, sh = 0;
    if (native) *native = false;
    if (v && !opts.handheld && opts.ultrawide && screen_size(&sw, &sh)) {
        const double want = (double)sw / sh, game = 16.0 / 9.0;
        if (want > game * 1.01 || (want > 1.0 && want < game * 0.99)) {
            unsigned w = 0, h = 0;
            hos_wide_size(o.h, hos_wide_clamp(want), hos_wide_max_px(), &w, &h);
            o = Out{w, h};
        }
        if (native) *native = o.w == sw && o.h == sh;
    }
    return o;
}
Out shown_output() {
    return opts.handheld ? Out{1536, 864} : picture_for(opts.output_res);   // (the real picture)
}
// The size the game draws the 3D at for a fixed scale of its output, a twelfth of it: every twelfth asked came out exact at the three
// outputs (2048x1152: 0.58 drew 1195x672, 0.6667 1366x768). So the scale the game is given is the twelfth too: 0.67 drew 2615x1471 at
// 3840x2160, not the 2560x1440 of 8/12.
int twelfths(float scale) { return std::min(std::max((int)(scale * 12.0f + 0.5f), 1), 12); }
void game_size(float scale, uint32_t& w, uint32_t& h) {
    const Out o = shown_output();
    const int k = twelfths(scale);
    w = (o.w * k + 11) / 12, h = o.h * k / 12;
}
const char* tx_resolution(int v) {
    static char b[64];
    const int up = upscaler();
    // (each stop with the 3D's real size, the render input: a twelfth of the picture (game_size), Native
    // the picture itself)
    if (up == ur::DLAA || up == ur::FSR4_AA) {
        const Out o = shown_output();
        snprintf(b, sizeof b, "Native (%s, %ux%u)", up == ur::DLAA ? "DLAA" : "FSR 4", o.w, o.h);
        return b;
    }
    if (!ur::percent(v)) return "Auto (dynamic)";
    uint32_t w, h;
    game_size(ur::percent(v) / 100.0f, w, h);
    if (const char* mode = ur::scales_by_mode(up) ? ur::dlss_mode_name(v) : nullptr) {
        snprintf(b, sizeof b, "%s %d%% (%ux%u)", mode, ur::percent(v), w, h);
        return b;
    }
    snprintf(b, sizeof b, "%d%% (%ux%u)", ur::percent(v), w, h);
    return b;
}
/// The Render resolution: the value chosen, for the Upscaler shown (every one: each value with each Upscaler)
const char* hn_resolution(int v, int variant) {
    const int up = variant < 0 ? std::clamp(upscaler(), 0, ur::kUpscalers - 1) : variant;
    if (up >= ur::kUpscalers) return nullptr;
    // (Native is the slider's top: the 3D at the picture's full size)
    if (up == ur::DLAA) return "DLAA: the 3D at the picture's full size, anti-aliased by DLSS. The sharpest, and the heaviest.";
    if (up == ur::FSR4_AA) return "FSR 4's native AA: the 3D at the picture's full size. The sharpest, and the heaviest.";
    const int p = ur::percent(v);
    if (up == ur::DLSS) {
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and DLSS follows. Set Sharpness to around 80%.";
        case 92: case 83: case 75: return "Between Native and Quality: sharper than Quality, a little slower.";
        case 67: return "DLSS's Quality mode: its sharpest from a smaller 3D. Set Sharpness to around 80%.";
        case 58: return "Between Quality and Performance. Set Sharpness to around 80%.";
        case 50: return "Faster, and softer. Set Sharpness to around 80%.";
        case 42: return "Between Performance and Ultra Performance.";
        case 33: return "DLSS's fastest mode, and its softest. Set Sharpness to around 80%.";
        default: return nullptr;   // (not one of DLSS's steps: the row moves to one)
        }
    }
    if (up == ur::FSR3 || up == ur::FSR4) {   // (the same modes, AMD's ratios)
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and FSR follows.";
        case 67: return "FSR's Quality mode, 1.5x: its sharpest.";
        case 58: return "FSR's Balanced mode, 1.7x.";
        case 50: return "FSR's Performance mode, 2x: faster, and softer.";
        case 33: return "FSR's Ultra Performance mode, 3x: its fastest, and its softest.";
        default: return nullptr;   // (not one of FSR's steps: the row moves to one)
        }
    }
    if (!p) return "The game lowers it when the GPU is busy.";
    if (p == 100) return "The 3D at the output's full size: the sharpest, and the slowest.";
    return "A steady size: lower is faster, and softer.";
}
/// A row with nothing else to choose here: shown without its arrows, Left and Right leave it (and the saved value) alone
bool row_fixed(const int* value) {
    return (value == &opts.dlss && ur::upscaler_fixed(upscalers_available())) ||
           (value == &opts.resolution && ur::render_fixed(upscaler())) || (value == &opts.upscale && !filter_needed()) ||
           (value == &opts.dlss_model && ur::model_fixed(upscaler(), dlss_preset_env() != nullptr));
}
const char* tx_output_res(int v) {
    static char b[40];
    v = output_choice(v);
    // (the picture this choice makes (picture_for: at the monitor's shape with Ultrawide on), "(native)"
    // when it is the screen's own size; the game's own choice (0) keeps its name)
    bool native = false;
    const Out o = picture_for(v, &native);
    snprintf(b, sizeof b, "%ux%u%s", o.w, o.h, native ? " (native)" : v ? "" : " (game's)");
    return b;
}
/// Anisotropic filtering: the game's own, or 2x to 16x (vk_draw.cpp aniso_override)
const char* tx_anisotropy(int v) {
    static const char* t[] = {"Game's", "2x", "4x", "8x", "16x"};
    return (unsigned)v < 5 ? t[v] : t[4];
}
const char* tx_speed(int v) { return v ? "Real time" : "Per frame"; }
const char* tx_text_speed(int v) {
    static const char* t[] = {"Normal", "2x", "4x", "Instant"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
const char* tx_text_size(int v) {
    static const char* t[] = {"Original", "110%", "125%", "150%", "175%"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
const char* tx_ultrawide(int v) { return v ? "Monitor's shape" : "16:9"; }
const char* tx_prompts(int v) {
    static const char* t[HOS_PROMPT_N + 1] = {"Auto", "Switch", "Xbox", "PlayStation 4", "PlayStation 5", "Steam Deck"};
    return t[std::min(std::max(v, 0), (int)HOS_PROMPT_N)];
}
const char* tx_gpu(int v) {
    static char buf[300];
    const int idx = v - 1;
    if (v <= 0) snprintf(buf, sizeof buf, "Auto");
    else if (idx < (int)gpu_device_names.size()) snprintf(buf, sizeof buf, "%d: %s", idx, gpu_device_names[idx].c_str());
    else snprintf(buf, sizeof buf, "%d: Device %d", idx, idx);
    // (the whole name shows: the selected row slides through it, see marquee_pos)
    return buf;
}
const char* tx_pad_layout(int v);
// (hookshot) the Microphone row: Off, the system's default, or one of the host's microphones (host/nn_audio_in.c: the
// ocarina's input streams), told to the host by its name (host->mic_select) whenever it changes. The chosen one is kept by
// its name, mic_want, also while it is unplugged: the row then has one more entry for it, "<name> (not connected)", the
// host lets the default stand in and opens it again once it is back, and the name stays in the ini. In the ini the name is
// cut to kMicNameKept bytes (at a character's start), so the line fits the 80-byte readers of the file (host/nn_hid.c,
// nn_applet.c, hos_main.c), and a name is matched by that much of it
constexpr int kMaxMics = 16;
constexpr size_t kMicNameKept = 60;
std::string mic_want;      // (the chosen microphone's name, cut to kMicNameKept; empty: Off or the default chosen)
bool mic_missing = false;  // (mic_want is not plugged in: the row's last entry, 2 + mic_listed, stands for it)
int mic_listed = 0;        // (the microphones as the host last listed them)
bool can_mic() { return host && host->mic_count && host->mic_name && host->mic_select; }
size_t text_char_start(const std::string& text, size_t at) {
    while (at > 0 && at < text.size() && ((unsigned char)text[at] & 0xc0) == 0x80) at--;
    return at;
}
std::string mic_key(const char* name) {
    std::string k(name ? name : "");
    for (char& c : k)   // (a device's name goes into hookshot_options.ini: a line break in it would add lines there)
        if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    if (k.size() <= kMicNameKept) return k;
    return k.substr(0, text_char_start(k, kMicNameKept));
}
/// buf cut back to whole UTF-8 characters (snprintf may have cut one)
void utf8_whole(char* buf) {
    size_t n = strlen(buf), i = n;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) i--;   // (the last character's continuation bytes)
    if (i == 0) return;
    const unsigned char lead = (unsigned char)buf[i - 1];
    const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < want) buf[i - 1] = 0;
}
const char* tx_mic(int v) {
    static char buf[160];
    if (v <= 0) return "Off";
    if (v == 1) return "Default";
    if (mic_missing && v == 2 + mic_listed) {
        const int room = (int)sizeof buf - (int)sizeof " (not connected)";
        snprintf(buf, (size_t)room + 1, "%s", mic_want.c_str());
        utf8_whole(buf);
        strcat(buf, " (not connected)");
        return buf;
    }
    const char* n = can_mic() ? host->mic_name(v - 2) : "";
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "Microphone");
    utf8_whole(buf);
    return buf;
}
/// The Microphone row: Off, the default, a microphone of the list, or the chosen one while it is unplugged (variant 1)
const char* hn_mic(int v, int variant) {
    if (v <= 0) return variant <= 0 ? "The game hears silence." : nullptr;
    if (v == 1)
        return variant <= 0 ? "The system's default recording device. On Windows, desktop apps must be allowed to use the "
                              "microphone (Settings, Privacy & security)."
                            : nullptr;
    const bool missing = variant < 0 ? mic_missing && v == 2 + mic_listed : variant == 1;
    if (variant > 1) return nullptr;
    return missing ? "Not plugged in: Default stands in until it is back." : "This microphone, by its name. A change applies at once.";
}
const char* tx_rumble(int v) { return v ? tx_percent(v) : "Off"; }
// (hookshot) Menu size: how large the overlays are drawn (vk_ui.h unit), Medium unless the player chooses Small or Large. It
// was Auto by default, Large in fullscreen at 1080 lines and up while a controller opened the menu: the menu changed size with
// the input that opened it, and the version in its banner no longer fitted. An options file's menu_size=0 (that Auto): Medium
constexpr float kVersionMinPt = 10;   // (the banner's version at Small and Medium: shrunk to fit, no smaller than this, in points)
bool pad_last = false;   // (a controller was the menu's last input: its buttons in the prompts)
int pad_confirm = 0, pad_back = 1;   // (the face buttons that confirm and go back, HOS_PAD_*: menu_keys::confirm_back, each tick)
const char* tx_menu_size(int v) {
    static const char* t[] = {"Small", "Medium", "Large"};
    return v >= 1 && v <= 3 ? t[v - 1] : t[1];
}
const char* tx_stick_dead_zone(int v) {
    static const char* t[] = {"Default (24%)", "5%", "10%", "15%", "20%", "30%"};
    return (unsigned)v < 6 ? t[v] : t[0];
}
// (hookshot) Audio output: the speakers the game mixes for, read once at its start (host/nn_audio.c au_menu_output)
const char* tx_audio_output(int v) {
    static const char* t[] = {"Auto", "Stereo", "5.1"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
// (hookshot) the motion's deadzone: the console's three zero-drift modes, the rate under which the pad counts as still
const char* tx_deadzone(int v) {
    static const char* t[] = {"Low", "Standard", "High"};
    return t[v < 0 ? 0 : v > 2 ? 2 : v];
}
const char* tx_robust(int v) {
    static const char* t[] = {"Auto", "Off", "On"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
const char* tx_mesh_mode(int v) {
    static const char* t[] = {"Automatic", "Native", "Compute"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
const char* tx_preset(int v) {
    static const char* t[] = {"Custom", "Minimum (4GB)", "Low (6GB)", "Balanced (8GB)", "High (12GB)", "Ultra (16GB+)"};
    return t[std::min(std::max(v, 0), 5)];
}
void sync_preset_budgets(int p) {
    switch (p) {
    case 1: opts.vram_mb = 2200;  opts.mirror_mb = 256;  break;
    case 2: opts.vram_mb = 3600;  opts.mirror_mb = 512;  break;
    case 3: opts.vram_mb = 4800;  opts.mirror_mb = 1024; break;
    case 4: opts.vram_mb = 7500;  opts.mirror_mb = 2048; break;
    case 5: opts.vram_mb = 11000; opts.mirror_mb = 3072; break;
    default: opts.vram_mb = 0;    opts.mirror_mb = 0;    break;
    }
}
// (Sharpness: tuned for the window pass's neighbours one picture pixel apart, each about the old setting's sharpening on a
// 3840 x 2160 window at that preset's picture, 2048 x 1152 or the handheld profile's 1536 x 864: same frame, mean |Laplacian|
// of the window and of the picture, on the slider's steps)
void apply_preset(int p) {
    switch (p) {
    case 1: // Minimum (4GB VRAM / iGPU)
        opts.resolution = 4; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 2: // Low (6GB VRAM)
        opts.resolution = 2; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 3: // Balanced (8GB VRAM)
        opts.resolution = 1; opts.upscale = 1; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 4: // High (12GB VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 40;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 5: // Ultra (16GB+ VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 50;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    default:
        break;
    }
    // (the Upscaler and Frame generation are the player's: a preset never turns DLSS or made frames on or off. With DLSS
    // its resolution is DLSS's nearest mode)
    if (p > 0) opts.resolution = ur::snap_render(upscaler(), opts.resolution);   // (a slider stop kept)
    sync_preset_budgets(p);
}

MENU_HINTS(hn_preset, 0, "Each row as you set it. Changing most rows yourself comes back to Custom.",
           "Resolution 50% (with DLSS its nearest mode), Lanczos, Sharpness 30%, the Handheld Profile.",
           "Resolution 75% (with DLSS its nearest mode), Lanczos, Sharpness 30%, the Handheld Profile.",
           "Resolution 100% (with DLSS its nearest mode), Catmull-Rom, Sharpness 30%.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 40%.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 50%.")
MENU_HINTS(hn_gpu, 0, "The best dedicated GPU.")
/// Frame generation: how many frames are made for each of the game's
const char* hn_framegen(int v, int variant) {
    // (2x: by AMD FSR 3.1 where it runs, from the scene's depth and motion: vk_framegen.cpp. Variant 1 is that text)
    if (v == 2 && (variant < 0 ? fsr3fi::frames_wanted() != 0 : variant == 1))
        return "One made frame between each two of the game's, by AMD FSR 3.1 from the scene's depth and motion.";
    if (variant > 0) return nullptr;
    if (v <= 0) return "Only the game's own frames: no added delay.";
    if (v == 1) return "A frame for each of the display's refreshes: none made when the game keeps up. Frame pacing does not apply.";
    static char b[9][96];
    snprintf(b[v], sizeof b[v], "%s between each two of the game's.", v == 2 ? "One made frame" : v == 3 ? "Two made frames" : v == 4 ? "Three made frames"
             : v == 5 ? "Four made frames" : v == 6 ? "Five made frames" : v == 7 ? "Six made frames" : "Seven made frames");
    return b[std::clamp(v, 2, 8)];
}
MENU_HINTS(hn_pace, 0, "As soon as it is ready.", "At an even interval: less stutter, at up to one frame of delay.")
MENU_HINTS(hn_output_res, 0, "The game's own size.",
           "The sharpest on a 2560x1440 screen: nothing to scale to the window.",
           "The sharpest on a 3840x2160 screen. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 1920x1080 screen: the HUD and text pixel for pixel, nothing to scale to the window.",
           "The sharpest on a 5120x1440 ultrawide screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 5120x2880 (5K) screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "Your own size, from output_res= in hookshot_options.ini.")
/// The Window scaling filter: its filter, or that none is needed at this window's size
const char* hn_filter(int v, int variant) {
    static const char* const t[] = {"The plain copy.", "Sharper.", "Sharper still; can ring on edges."};
    static const char* const none = "The window is the Output resolution's size: nothing to scale. The choice is kept for "
                                    "another size.";
    if (variant < 0) return filter_needed() ? t[std::clamp(v, 0, 2)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 2)] : variant == 1 ? none : nullptr;
}
/// Sharpness: with DLSS or DLAA, the amount that suits them
const char* hn_sharpness(int v, int variant) {
    static const char* const dlss = "DLSS and DLAA look softer than the game's own upscaler: around 80% suits them.";
    const bool with_dlss = variant < 0 ? upscaler() == ur::DLSS || upscaler() == ur::DLAA : variant == 1;
    if (variant > 1) return nullptr;
    return with_dlss ? dlss : v == 0 ? "Off: the picture as it is." : nullptr;
}
MENU_HINTS(hn_handheld, 0, "As on a TV: the game at the Output resolution's size.",
           "As in the hands: the game at the console's handheld size (1536x864), lighter on the GPU.")
MENU_HINTS(hn_ultrawide, 0, "The game's own shape.", "The camera takes the screen's shape, wider or narrower. The HUD keeps its 16:9 shape in the middle.")
const char* tx_wide_menus(int v) {
    static const char* t[] = {"Full width", "Centred, blurred sides", "Centred, black bars"};
    return t[std::min(std::max(v, 0), 2)];
}
MENU_HINTS(hn_wide_menus, 0, "The menus stretched over the whole screen, as before.",
           "The menus in the middle 16:9; beside them, what the game draws there, out of focus.",
           "The menus in the middle 16:9; black beside them.")
MENU_HINTS(hn_robust, 0, "On, on every card.", "Can be faster on some NVIDIA cards.", "Always on.")
MENU_HINTS(hn_mesh_mode, 0, "The GPU's own mesh shaders where it has them, the compute path where it has not.",
           "The GPU's own mesh shaders for every draw. A GPU without them still uses the compute path.",
           "The compute path on every GPU: try it if your card crashes or leaves objects out. Usually a little slower.")
MENU_HINTS(hn_fps, 0, nullptr, "With frame generation on, the game's own rate too.")
MENU_HINTS(hn_menu_size, 1, "Smaller, for a desk.", "The usual size.", "Larger, for a TV across the room.")
MENU_HINTS(hn_speed, 0, "A step each frame: the game slows down whenever it runs below 60 fps.",
           "Normal speed down to 15 fps, by taking longer steps.")
MENU_HINTS(hn_text_speed, 0, "The game's own pace, with its pauses.", "Twice as fast.", "Four times as fast.",
           "Each page at once, as pressing A does.")
MENU_HINTS(hn_audio_output, 0, "5.1 when the default output device has 5.1 or more speakers, else stereo.",
           "Stereo, whatever the device has.", "5.1, whatever the device has. Mixed down on stereo speakers.")
MENU_HINTS(hn_text_size, 0, "The game's original size.", "A little larger.", "Larger.", "Much larger.", "Largest.")
MENU_HINTS(hn_background, 0, "Only while the game's window has the focus.",
           "Also behind other windows, for streaming or other apps. The keyboard and mouse still need the game's window.")
MENU_HINTS(hn_mousecam, 0, "The mouse stands in for the right stick, with its curve and top speed.",
           "The mouse turns the camera itself, one to one, as in a PC game.")
MENU_HINTS(hn_mouse_invert, 0, nullptr, "Moving the mouse forward tilts the camera down.")
MENU_HINTS(hn_gyro, 0, "The sticks alone aim.",
           "Switch Pro controllers and pads in Switch mode, DualSense and DualShock 4. The value says whether the "
           "connected pad has a gyroscope.")
MENU_HINTS(hn_pad_layout, 0, "The Switch's order: A on the right, B at the bottom, X at the top, Y on the left.",
           "A at the bottom (action and talk), B on the right, X on the left, Y at the top.",
           "A at the bottom, B on the right; X and Y as on the Switch.",
           "Bindings of your own, from the Controls page.")
MENU_HINTS(hn_prompts, 0,
           "Those of the first controller, from its name (SDL's). The Switch's when it has none, as with XInput alone.")

Row rows[] = {
    // Graphics (Reflex, Anti-Lag 2, the Upscaler and the DLSS model only where they run here; HDR's paper white and peak
    // brightness only with HDR on: row_shown)
    {TAB_GRAPHICS, "Quality preset", &opts.preset, 0, 5, 1, tx_preset,
     "Sets the scaling rows and memory limits for a GPU with that much memory. The Upscaler and Frame generation stay yours.",
     hn_preset},
    {TAB_GRAPHICS, "Fullscreen", &opts.fullscreen, 0, 1, 1, tx_onoff,
     "Fills the whole screen with the game, without the window's border. The same as F11.", nullptr, false, true},
    {TAB_GRAPHICS, "GPU device", &opts.gpu_device, 0, 8, 1, tx_gpu,
     "Which graphics card the game runs on. Applies after a restart.", hn_gpu, true, true},
    {TAB_GRAPHICS, "Frame generation", &opts.framegen, 0, 8, 1, tx_framegen,
     "Makes frames between the game's own, for smoother motion, at a little input delay. Fast motion or camera cuts can "
     "show artifacts.", hn_framegen, false, true},
    {TAB_GRAPHICS, "Frame pacing", &opts.pace, 0, 1, 1, tx_onoff, "When each frame is shown.", hn_pace},
    {TAB_GRAPHICS, "HDR", &opts.hdr, 0, 2, 1, tx_hdr,
     "HDR10 out on an HDR display (Windows: Use HDR on in the display settings). Expanded: the game's SDR picture, its white at "
     "the paper white and its highlights up to the peak. Native: the game's own HDR, told the display's peak and paper white.",
     nullptr, false, true},
    {TAB_GRAPHICS, "HDR paper white", &opts.hdr_white, 80, 400, 10, tx_nits,
     "How bright the picture's white is with HDR on. Around 200 for a normal room; higher in a bright one.", nullptr, false, true},
    {TAB_GRAPHICS, "HDR peak brightness", &opts.hdr_peak, 400, 4000, 100, tx_nits,
     "The brightest highlights with HDR on: set it to your display's peak brightness.", nullptr, false, true},
    {TAB_GRAPHICS, "Game frame rate cap", &opts.fps_cap, 0, 5, 1, tx_fps_cap,
     "Limits frames rendered by the game. Generated frames are additional. Takes a restart. "
     "Past 60 is experimental: the game keeps real time, but some cutscenes and bosses can misbehave (Gohma, the intro's "
     "narration). 60 is the safe choice.",
     nullptr, true, true},
    {TAB_GRAPHICS, "NVIDIA Reflex", &opts.reflex, 0, 2, 1, tx_reflex,
     "NVIDIA GPUs: each frame starts as late as it can, for less input delay. Not used with frame generation.", hn_reflex},
    {TAB_GRAPHICS, "AMD Anti-Lag 2", &opts.antilag, 0, 1, 1, tx_antilag,
     "AMD GPUs: each frame starts as late as it can, for less input delay. Not used with frame generation.", hn_antilag},
    // (the picture's way, top to bottom: the output's size, what fills it from the 3D, the 3D's size, then the window)
    {TAB_GRAPHICS, "Output resolution", &opts.output_res, 0, kLastOutput, 1, tx_output_res,
     "The game's whole frame, HUD included, before it is scaled to the window. Docked only. Applies after a restart.",
     hn_output_res, true},
    {TAB_GRAPHICS, "Upscaler", &opts.dlss, 0, ur::kUpscalers - 1, 1, tx_upscaler, "What scales the 3D up to the Output resolution.",
     hn_upscaler},
    {TAB_GRAPHICS, "DLSS model", &opts.dlss_model, 0, ur::kModels - 1, 1, tx_dlss_model,
     "Which of NVIDIA's DLSS networks draws the picture.", hn_dlss_model, false, true},
    {TAB_GRAPHICS, "Render resolution", &opts.resolution, 0, ur::kValues - 1, 1, tx_resolution,
     "The share of the output the 3D is drawn at; the Upscaler fills in the rest.", hn_resolution},
    {TAB_GRAPHICS, "Window scaling filter", &opts.upscale, 0, 2, 1, tx_filter,
     "How the finished picture is scaled to the window, when its size differs from the Output resolution.", hn_filter},
    {TAB_GRAPHICS, "Sharpness", &opts.sharpness, 0, 100, 10, tx_percent,
     "Sharpens the picture as it goes to the window, with any filter. Too much shows halos around edges.", hn_sharpness},
    {TAB_GRAPHICS, "Ambient occlusion", &opts.ao, 0, 100, 10, tx_percent,
     "Soft contact shadows in corners and under objects. From the scene's depth, or with Ray tracing on the game's own occlusion traced against its geometry. Needs the DLSS or DLAA upscaler.",
     nullptr, false, true},
    {TAB_GRAPHICS, "Bounce light", &opts.gi, 0, 100, 10, tx_percent,
     "Light bounced once off the surfaces in view: a lit wall tints the ground beside it. Needs Ray tracing on and the DLSS or DLAA upscaler.",
     nullptr, false, true},
    {TAB_GRAPHICS, "Sun shadows", &opts.sun, 0, 100, 10, tx_percent,
     "Experimental. The sun's shadows traced as well, sharp where things touch, added to the game's own. 0: the game's alone. Needs Ray tracing on and the DLSS or DLAA upscaler.",
     nullptr, false, true},
    {TAB_GRAPHICS, "Ray tracing", &opts.rt, 0, 1, 1, tx_onoff,
     "Experimental. Builds the scene for ray tracing on RTX-class cards: Ambient occlusion is then traced. Applies after a restart.",
     nullptr, true, true},
    {TAB_GRAPHICS, "Anisotropic filtering", &opts.anisotropy, 0, 4, 1, tx_anisotropy,
     "Keeps textures sharp on surfaces seen at a slant, such as the ground ahead: higher is sharper in the distance, for a "
     "little GPU time. Game's is the console's own (1x or 2x).", nullptr},
    {TAB_GRAPHICS, "Profile", &opts.handheld, 0, 1, 1, tx_profile,
     "Runs the game as the console docked or handheld. Applies after a restart.", hn_handheld, true},
    {TAB_GRAPHICS, "Ultrawide", &opts.ultrawide, 0, 1, 1, tx_ultrawide,
     "The camera's shape on a screen wider or narrower than 16:9 (4:3, 16:10). Applies after a restart.", hn_ultrawide, true},
    {TAB_GRAPHICS, "Ultrawide menus", &opts.wide_menus, 0, 2, 1, tx_wide_menus,
     "With Ultrawide: the pause, item and map menus, and what shows beside them. Loading screens and pre-rendered movies, "
     "which draw no world either, get the same sides. Changes at once.", hn_wide_menus},
    {TAB_GRAPHICS, "Robust buffers", &opts.robust, 0, 2, 1, tx_robust,
     "Bounds-checked buffer reads: a read past a buffer's end is dropped instead of stopping the GPU. Applies after a "
     "restart.", hn_robust, true},
    {TAB_GRAPHICS, "Mesh shaders", &opts.mesh_mode, 0, 2, 1, tx_mesh_mode,
     "How the game's mesh programs (much of the scenery) run: on the GPU's own mesh shaders, or as compute shaders, which "
     "every GPU has. Applies after a restart.", hn_mesh_mode, true},

    // Audio
    {TAB_AUDIO, "Volume", &opts.volume, 0, 100, 5, tx_percent,
     "How loud the game plays. The game's own sound settings and Windows' volume still apply.", nullptr, false, true},
    {TAB_AUDIO, "Mute in background", &opts.mute_background, 0, 1, 1, tx_onoff,
     "Silences the game while another window has the focus. The sound comes back when you switch to the game.", nullptr,
     false, true},
    {TAB_AUDIO, "Audio output", &opts.audio_output, 0, 2, 1, tx_audio_output,
     "The speakers the game mixes its sound for. Applies after a restart.", hn_audio_output, true, true},

    // Gameplay
    {TAB_GAMEPLAY, "FPS counter", &opts.show_fps, 0, 1, 1, tx_onoff, "Frames per second in the top right corner.", hn_fps,
     false, true},
    {TAB_GAMEPLAY, "Menu size", &opts.menu_size, 1, 3, 1, tx_menu_size,
     "How large this menu, the FPS counter and the start-up screen are drawn.", hn_menu_size, false, true},
    {TAB_GAMEPLAY, "Game speed", &opts.realtime, 0, 1, 1, tx_speed, "How the game keeps time when it runs below 60 fps.",
     hn_speed},
    {TAB_GAMEPLAY, "Text speed", &opts.text_speed, 0, 3, 1, tx_text_speed,
     "How fast the letters of dialogue and other messages appear.", hn_text_speed},
    {TAB_GAMEPLAY, "Text size", &opts.text_size, 0, 4, 1, tx_text_size,
     "How large the text of dialogue, Navi and signs is drawn.", hn_text_size},
    {TAB_GAMEPLAY, "Microphone", &opts.mic, 0, 2 + kMaxMics, 1, tx_mic,
     "What the ocarina hears when the game's microphone option is on (playing an instrument, or humming).", hn_mic, false,
     true},
    {TAB_GAMEPLAY, "Discord status", &opts.discord, 0, 1, 1, tx_onoff,
     "Shows \"Playing Hookshot\" and the time played in your Discord status, while Discord runs on this PC.", nullptr, false,
     true},

    // Controls (12 rows; Motion opens the 6 TAB_MOTION rows, Controls its page with the 2 TAB_PAGE rows)
    {TAB_CONTROLS, "Controller in background", &opts.pad_background, 0, 1, 1, tx_onoff,
     "Whether the controller plays the game while another window has the focus.", hn_background},
    {TAB_CONTROLS, "Mouse camera", &opts.mouse_native, 0, 1, 1, tx_mousecam, "How the captured mouse turns the camera.",
     hn_mousecam, false, true},
    {TAB_CONTROLS, "Disable camera centering", &opts.disable_camera_centering, 0, 1, 1, tx_onoff,
     "Stops the camera gradually turning toward the direction you move, useful with native mouse look. Off keeps the "
     "game's standard behavior. Manual camera controls and recentering still work.", nullptr, false, true},
    {TAB_CONTROLS, "Mouse sensitivity", &opts.mouse_sens, 10, 500, 10, tx_sens,
     "How far the camera turns for a mouse movement, with either Mouse camera: at 1.00 native look turns 0.1 degree a "
     "count. The - and = keys change it in game too.", nullptr, false, true},
    {TAB_CONTROLS, "Invert mouse vertical", &opts.mouse_invert, 0, 1, 1, tx_onoff, "Swaps up and down for the mouse.",
     hn_mouse_invert, false, true},
    {TAB_CONTROLS, "Invert camera X", &opts.invert_cam_x, 0, 1, 1, tx_onoff,
     "Inverts horizontal look (left/right) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Invert camera Y", &opts.invert_cam_y, 0, 1, 1, tx_onoff,
     "Inverts vertical look (up/down) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Stick dead zone", &opts.stick_dead_zone, 0, 5, 1, tx_stick_dead_zone,
     "How far a stick moves before the game sees it, on each axis. Lower for a precise stick, higher for one that drifts. "
     "The game's own dead zone still follows.", nullptr, false, true},
    {TAB_CONTROLS, "Rumble strength", &opts.rumble, 0, 150, 25, tx_rumble,
     "How strongly the controller rumbles, in percent of the game's rumble. Off turns it off.", nullptr, false, true},
    {TAB_MOTION, "Motion controls", &opts.gyro, 0, 1, 1, tx_gyro,
     "Aims and looks with the controller's gyroscope where the game uses motion, as on the Switch. Works with Switch Pro "
     "controllers and pads in Switch mode, DualSense and DualShock 4. Shown beside it: whether the connected pad has one.", hn_gyro,
     false, true},
    {TAB_MOTION, "Motion sensitivity X", &opts.gyro_sens_x, 10, 600, 10, tx_percent,
     "How far the aim moves left and right for a given turn of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Motion sensitivity Y", &opts.gyro_sens_y, 10, 300, 10, tx_percent,
     "How far the aim moves up and down for a given tilt of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Invert motion X", &opts.gyro_inv_x, 0, 1, 1, tx_onoff, "Turns the motion aim's left and right around.",
     nullptr, false, true},
    {TAB_MOTION, "Invert motion Y", &opts.gyro_inv_y, 0, 1, 1, tx_onoff, "Turns the motion aim's up and down around.",
     nullptr, false, true},
    {TAB_MOTION, "Motion deadzone", &opts.gyro_deadzone, 0, 2, 1, tx_deadzone,
     "How slow a turn counts as none, the console's three settings: Standard is its own. High steadies a pad that jitters "
     "when held still; Low keeps the smallest movements.", nullptr, false, true},
    {TAB_CONTROLS, "Motion", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens the Motion page, the TAB_MOTION rows)
     "Motion controls: aiming with the controller's gyroscope, its sensitivity and its inverts."},
    // (hookshot) the Controls page's own rows, above its grid (TAB_PAGE: no tab shows them)
    {TAB_PAGE, "Layout preset", &opts.pad_layout, 0, 3, 1, tx_pad_layout,
     "Which face button is which. Binding one yourself on the Controls page makes it Custom.", hn_pad_layout},
    {TAB_PAGE, "Button prompts", &opts.prompts, 0, HOS_PROMPT_N, 1, tx_prompts,
     "The button icons the game shows, in its texts and menus: those of this controller, for the buttons your controls "
     "put each action on.", hn_prompts, true, true},
    {TAB_CONTROLS, "Controls", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens its page)
     "The layout preset, the button prompts, and which gamepad button and which keys press each of the Switch's "
     "buttons."},
    {TAB_CONTROLS, "Menu shortcut", &opts.menu_shortcut, 0, 2, 1, tx_menu_shortcut,
     "Which keys open and close this menu. F1 always does; F1 only leaves Insert free for an overlay. With Esc, Esc opens it "
     "too, as a PC game's pause menu (open, Esc still goes back a page or closes it).",
     nullptr, false, true},
    // Debug: the tools' switch, the tools themselves below it while they are on (cheat: menu_tick does what they do), then the
    // developer's rows
    {TAB_DEBUG, "Debug tools", &opts.debug_tools, 0, 1, 1, tx_onoff,
     "Testing tools: no-clip flight, a free camera, teleport, Link's age, the inventory, the clock, heart protection and "
     "movement speed. Off puts flight, the free camera and heart protection away and Link back to his own speed.", nullptr, false, true},
    {TAB_DEBUG, "Heart protection", nullptr, 0, 0, 0, nullptr,
     "Protects Link's hearts. Hits can still knock him back.", nullptr, false, true, 0},
    {TAB_DEBUG, "No-clip flight", nullptr, 0, 0, 0, nullptr,
     "Enable double-tap Jump for takeoff and landing. Hold Jump to climb or L to descend.", nullptr, false, true, 1},
    {TAB_DEBUG, "Mark position", nullptr, 0, 0, 0, nullptr,
     "Remember Link's position for this loaded scene.", nullptr, false, true, 2},
    {TAB_DEBUG, "Return to mark", nullptr, 0, 0, 0, nullptr,
     "Teleport Link to the marked position in the current scene.", nullptr, false, true, 3},
    {TAB_DEBUG, "Set time", nullptr, 0, 0, 0, nullptr,
     "Choose an hour with Left or Right. Enter sets the world clock.", nullptr, false, true, 4},
    {TAB_DEBUG, "Inventory item", nullptr, 0, 0, 0, nullptr,
     "Choose a consumable with Left or Right.", nullptr, false, true, 5},
    {TAB_DEBUG, "Change quantity", nullptr, 0, 0, 0, nullptr,
     "Choose how many to add or remove. Enter applies the change.", nullptr, false, true, 6},
    {TAB_DEBUG, "Free camera", nullptr, 0, 0, 0, nullptr,
     "Move and look with your usual controls. Page Up / Page Down or R / L changes height.", nullptr, false, true, 7},
    {TAB_DEBUG, "Teleport", nullptr, 0, 0, 0, nullptr,
     "Opens the map list. Link travels through the game's own map change, in his current age.", nullptr, false, true, 8},
    {TAB_DEBUG, "Switch age", nullptr, 0, 0, 0, nullptr,
     "Swaps Link between child and adult and reloads this map in the other age (the Market when this map has no match).", nullptr, false, true, 9},
    {TAB_DEBUG, "Inventory", nullptr, 0, 0, 0, nullptr,
     "Enter opens the inventory editor: unlock or relock equipment, items, upgrades and quest items, and change amounts.", nullptr, false, true, 10},
    {TAB_DEBUG, "Movement speed", nullptr, 0, 0, 0, nullptr,
     "How fast Link walks, runs and swims, as a multiple of his own speed. Choose with Left or Right.", nullptr, false, true, 11},
    {TAB_DEBUG, "Developer", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    {TAB_DEBUG, "Descriptor reuse", &opts.descriptor_reuse, 0, 1, 1, tx_onoff,
     "A draw keeps its descriptors while the texture slots it samples are unchanged, so streaming elsewhere in the game no longer "
     "rebuilds every set each frame. Applies after restart.", nullptr, true},
    {TAB_DEBUG, "Sampler cache", &opts.sampler_cache, 0, 1, 1, tx_onoff,
     "What a sampler holds is kept beside it on the replay thread, so resolving a sampler that was not registered anew takes no "
     "lock. Helps while the game streams textures. Applies after restart.", nullptr, true},
    // (Restart and Quit stay the table's last two: kRestartRow and kQuitRow count back from its end)
    {TAB_GAMEPLAY, "Restart", nullptr, 0, 0, 0, nullptr,
     "Starts the game again. Save your progress first. Press Enter or A twice to restart. Esc or B "
     "cancels."},
    {TAB_GAMEPLAY, "Quit", nullptr, 0, 0, 0, nullptr,
     "Save your progress first. Press Enter or A twice to quit. Esc or B cancels."},
};
constexpr int kQuitRow = sizeof rows / sizeof rows[0] - 1;
constexpr int kRestartRow = kQuitRow - 1;
const Row* pending = nullptr;   // (Restart or Quit: shows Confirm until its second press)
bool cheats_env = false;   // (HOS_OOT_CHEATS=1: on whatever the Debug tab says)
/// The debug tools: the Debug tab's row, or HOS_OOT_CHEATS=1
bool cheats_on() { return cheats_env || opts.debug_tools; }
std::atomic<int> protect_hearts{-1};
// -1 disables the shortcut, 0 enables it on the ground, and 1 means flying.
std::atomic<int> player_flight{-1};
std::atomic<int> free_camera{0};
std::atomic<int> infinite_ammo{0};
std::atomic<int> time_lock{0};
std::atomic<int> moon_jump{0};
std::atomic<int> cheat_action{0};
std::atomic<int> link_age{-1};
// (Movement speed: the choice, an index of link_speeds; the game side reads it in percent, HOS_OPT_CHEAT_SPEED)
constexpr int link_speeds[] = {100, 125, 150, 200, 300};
constexpr const char* link_speed_names[] = {"1x", "1.25x", "1.5x", "2x", "3x"};
constexpr int kLinkSpeeds = sizeof link_speeds / sizeof link_speeds[0];
std::atomic<int> link_speed{0};   // (the game side's report, OOT_CHEAT_AGE_ID: 0 child, 1 adult, -1 no save loaded)
int clock_hour = 12;
int pouch_item = 0;
int pouch_delta = 1;
constexpr const char* pouch_items[] = {"Deku Seeds", "Deku Nuts", "Deku Sticks", "Bombs", "Arrows"};
constexpr int kPouchItems = sizeof pouch_items / sizeof pouch_items[0];
// (teleport list: generated by work/oot-teleport-tools/write_destinations.py)
// (the order of oot_destinations in host/hos_main.c; Enter sends 1000 + the index)
constexpr const char* teleport_names[] = {
    "Kokiri Forest",
    "Hyrule Field",
    "Kakariko Village",
    "Market",
    "Hyrule Castle",
    "Lost Woods",
    "Sacred Forest Meadow",
    "Lake Hylia",
    "Graveyard",
    "Death Mountain Trail",
    "Death Mountain Crater",
    "Goron City",
    "Zora's River",
    "Zora's Domain",
    "Zora's Fountain",
    "Gerudo Valley",
    "Gerudo Fortress",
    "Haunted Wasteland",
    "Desert Colossus",
    "Inside the Deku Tree",
    "Dodongo's Cavern",
    "Jabu-Jabu's Belly",
    "Forest Temple",
    "Fire Temple",
    "Water Temple",
    "Shadow Temple",
    "Spirit Temple",
    "Bottom of the Well",
    "Ice Cavern",
    "Gerudo Training Ground",
    "Ganon's Castle"};
constexpr const char* teleport_notes[] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "adult only", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
struct TeleportSection { const char* name; int first; };
constexpr TeleportSection teleport_sections[] = {{"Overworld", 0}, {"Dungeons", 19}};
constexpr int kTeleportSections = sizeof teleport_sections / sizeof teleport_sections[0];
// (end of the generated teleport list)
constexpr int kTeleports = sizeof teleport_names / sizeof teleport_names[0];
int teleport_choice = 0;
int teleport_columns = 1, teleport_page_rows = 1;   // (as last drawn: the cards in a row, the rows that fit: Page Up / Down)
float teleport_scroll_px = 0, teleport_want_px = 0;   // (the cards' scroll, eased toward teleport_want_px)
bool teleport_follow = true;   // (the scroll follows the choice; the wheel lets go of it until the choice moves)
int teleport_wheel = 0;        // (the wheel's notches since the last drawing: it scrolls by them)
int teleport_section_of(int i) {
    int section = 0;
    while (section + 1 < kTeleportSections && i >= teleport_sections[section + 1].first) ++section;
    return section;
}
int teleport_section_end(int section) {
    return section + 1 < kTeleportSections ? teleport_sections[section + 1].first : kTeleports;
}
/// The destination rows rows down (up when negative) from i, in the grid the Teleport page draws: each section's cards
/// teleport_columns to a row, under the section's heading; the column kept where the next row has it
int teleport_step_rows(int i, int rows) {
    const int cols = std::max(1, teleport_columns);
    for (; rows > 0; --rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, end = teleport_section_end(section);
        const int p = i - first;
        if (p + cols < end - first) i += cols;
        else if (p / cols < (end - first - 1) / cols) i = end - 1;   // (the section's last row, a short one)
        else if (section + 1 < kTeleportSections) i = std::min(end + p % cols, teleport_section_end(section + 1) - 1);
        else break;
    }
    for (; rows < 0; ++rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, p = i - first;
        if (p >= cols) i -= cols;
        else if (section > 0) {
            const int prev = teleport_sections[section - 1].first, n = first - prev;
            i = std::min(prev + (n - 1) / cols * cols + p, first - 1);
        } else break;
    }
    return i;
}
// the Inventory page (gpu/vk/oot_cheat_items.h): items and their section headings, counts from the game side
#define OOT_MENU_ITEM_LABEL(label, name, kind) label,
#define OOT_MENU_ITEM_KIND(label, name, kind) kind,
#define OOT_MENU_NO_SECTION(name)
constexpr const char* item_names[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_LABEL)};
constexpr unsigned char item_kinds[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_KIND)};
constexpr int kItems = sizeof item_names / sizeof item_names[0];
// (each section's first item: a counter that a section heading reads and an item bumps)
constexpr int item_section_count() {
    int n = 0;
#define OOT_MENU_COUNT_SECTION(title) n++;
#define OOT_MENU_SKIP_ITEM(label, name, kind)
    OOT_CHEAT_ITEMS(OOT_MENU_COUNT_SECTION, OOT_MENU_SKIP_ITEM)
    return n;
}
constexpr int kItemSections = item_section_count();
struct ItemSections { TeleportSection s[kItemSections]; };
constexpr ItemSections make_item_sections() {
    ItemSections r{};
    int section = 0, item = 0;
#define OOT_MENU_ADD_SECTION(title) r.s[section].name = title, r.s[section].first = item, section++;
#define OOT_MENU_ADD_ITEM(label, name, kind) item++;
    OOT_CHEAT_ITEMS(OOT_MENU_ADD_SECTION, OOT_MENU_ADD_ITEM)
    return r;
}
constexpr ItemSections item_sections = make_item_sections();
std::atomic<int> item_counts[kItems];   // (-1 until the game side reports: OOT_CHEAT_ITEM_BASE + i)
int item_choice = 0;
int inventory_columns = 1, inventory_capacity = 1;   // (as last drawn: the cards in a row, on a page)
int inventory_filter = 0;   // (All, then the item sections)
int inventory_page = 0;     // (the page shown: the choice's, or another the wheel turned to)
bool inventory_amount = false;   // (Left and Right change the chosen item's amount)
int inventory_begin() { return inventory_filter ? item_sections.s[inventory_filter - 1].first : 0; }
int inventory_end() { return inventory_filter && inventory_filter < kItemSections ? item_sections.s[inventory_filter].first : kItems; }
void inventory_reveal() { inventory_page = (item_choice - inventory_begin()) / std::max(1, inventory_capacity); }
void choose_inventory_filter(int filter) {
    inventory_filter = filter;
    item_choice = inventory_begin();
    inventory_amount = false;
    inventory_reveal();
}
constexpr int kCheatRows = 12;   // (the debug tools' rows: Row::cheat 0 to 11)
/// A debug tool's value as its row shows it ("" an action: confirm does it)
std::string cheat_value(int i) {
    char t[16];
    switch (i) {
    case 0: return tx_onoff(protect_hearts.load() == 1);
    case 1: return tx_onoff(player_flight.load() >= 0);
    case 4: return snprintf(t, sizeof t, "%02d:00", clock_hour), t;
    case 5: return pouch_items[pouch_item];
    case 6: return snprintf(t, sizeof t, "%+d", pouch_delta), t;
    case 7: return tx_onoff(free_camera.load());
    case 11: return link_speed_names[std::clamp(link_speed.load(), 0, kLinkSpeeds - 1)];
    default: return "";
    }
}
/// The tools that choose with Left and Right (arrows at their value's ends): Set time, Inventory item, Change quantity,
/// Movement speed
bool cheat_chooses(int i) { return i == 4 || i == 5 || i == 6 || i == 11; }
/// Switch age as Kid | Adult, Link's age lit: once the game side has said it (a save loaded)
bool age_shown(int i) { return i == 9 && link_age.load(std::memory_order_relaxed) >= 0; }
/// What confirm does on a tool's row, for its prompt (nullptr: nothing)
const char* cheat_confirm(int i) {
    static const char* const says[kCheatRows] = {"Toggle", "Toggle", "Apply", "Apply", "Set", nullptr, "Apply", "Toggle", "Open list",
                                                 "Switch", "Open", nullptr};
    return i >= 0 && i < kCheatRows ? says[i] : nullptr;
}
constexpr int kRows = sizeof rows / sizeof rows[0];

int cur_tab = TAB_GRAPHICS;
int tab_selected[TAB_COUNT + 1] = {};   // (and the Motion page's)
bool is_motion_row(const Row& r) { return r.tab == TAB_CONTROLS && !r.value && !strcmp(r.name, "Motion"); }

// ---- the rows that take a restart: their values at the start (menu_init, the GPU's at menu_set_gpu_count) ----
namespace mr = menu_restart;
mr::State restart_state;
int restart_slot[kRows];             // (a row's slot in restart_state, -1 none)
const char* restart_names[mr::kMax];
void restart_init() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < kRows; i++) {
        restart_slot[i] = rows[i].restart && rows[i].value ? restart_state.add(*rows[i].value) : -1;
        if (restart_slot[i] >= 0) restart_names[restart_slot[i]] = rows[i].name;
    }
}
int restart_slot_of(const Row& r) { return restart_slot[&r - rows]; }
/// A row that takes a restart, changed from what the game started with (a row past menu_restart.h's kMax has no slot: never)
bool restart_changed(const Row& r) {
    const int slot = restart_slot_of(r);
    return r.value && r.restart && slot >= 0 && *r.value != restart_state.start[slot];
}
void restart_values(int* now) {
    for (int i = 0; i < kRows; i++)
        if (restart_slot[i] >= 0) now[restart_slot[i]] = *rows[i].value;
}
/// The rows whose value is not the one the game runs with (a bit a slot)
unsigned restart_pending() {
    int now[mr::kMax];
    restart_values(now);
    return restart_state.pending(now);
}
/// Whether closing the menu now asks to restart: something pending, not declined with Later, and a window to close
bool restart_ask() {
    int now[mr::kMax];
    restart_values(now);
    return app_window() && restart_state.ask(now);
}
std::string restart_list(const char* last) {
    return mr::names(restart_names, restart_state.n, restart_pending(), ", ", last);
}
int ask_sel = 0;   // (the restart question's choice: 0 Later, 1 Restart now)

/// A row on its tab: the debug tools only with them on; a row for what this PC cannot run not at all (Reflex and Anti-Lag 2
/// only where the driver has them, the Upscaler only where more than the game's own runs, the DLSS model only where DLSS
/// runs); HDR's paper white and peak brightness only with HDR on. A row that comes or goes moves those after it: the
/// selection is a place on its tab (tab_selected), so a row hidden after the selected one leaves the selection where it is,
/// and menu_tick brings back one past the end
bool row_shown(int i) {
    const int* v = rows[i].value;
    return (rows[i].cheat < 0 || cheats_on()) && (v != &opts.reflex || reflex_available()) &&
           (v != &opts.antilag || antilag_available()) &&
           (v != &opts.dlss || ur::upscaler_shown(upscalers_available())) &&
           (v != &opts.dlss_model || ur::model_shown(dlss_why() == DLSS_OK)) &&
           ((v != &opts.hdr_white && v != &opts.hdr_peak) || opts.hdr != 0);
}
int tab_row_count(int t) {
    int c = 0;
    for (int i = 0; i < kRows; i++) if (rows[i].tab == (MenuTab)t && row_shown(i)) c++;
    return c;
}

int tab_row_to_global(int t, int local_idx) {
    int count = 0;
    for (int i = 0; i < kRows; i++) {
        if (rows[i].tab == (MenuTab)t && row_shown(i)) {
            if (count == local_idx) return i;
            count++;
        }
    }
    return 0;
}

std::atomic<bool> open_now{false};
bool menu_refresh = false;
// (a value too long for its row, a GPU's name: the selected row's slides through it, a character a step, resting at
// each end; the other rows end it with "...". The drawing (fit) says how far it can slide, menu_tick moves it)
int marquee_pos = 0, marquee_max = 0;
int64_t marquee_next = 0;
constexpr int64_t kMarqueeStep = 150000000LL, kMarqueeRest = 1200000000LL;   // (ns)
// (hookshot) the mouse (menu_tick): the tab strip's part under the cursor (a tab 0 to TAB_COUNT - 1, TAB_COUNT the previous,
// TAB_COUNT + 1 the next; -1 none); the slider held with the left button (its row in the tab, -1 none)
int hover_tab = -1;
int drag_row = -1;
bool mouse_read = false;   // (the menu has read the mouse since it opened)
bool is_slider(const Row& r) { return r.value == &opts.sharpness || r.value == &opts.mouse_sens || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y || r.value == &opts.volume; }
/// (a scale: the arrows and a click stop at its ends; Rumble strength would go from Off to its strongest)
bool stops_at_ends(const Row& r) { return r.value == &opts.mouse_sens || r.value == &opts.rumble || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y || r.value == &opts.volume; }
/// (the ini's Rumble strength on the menu's steps: 0 to 150, the nearest 25)
int rumble_step(int v) { return (std::min(std::max(v, 0), 150) + 12) / 25 * 25; }
std::string ini_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "hookshot_options.ini";
}
const char* ini_key(const Row& r) {
    if (r.value == &opts.output_res) return "output_res";   // (host/nn_applet.c reads it at the start)
    // (rows renamed since their values were first saved keep their keys: an options file from before still loads)
    if (r.value == &opts.resolution) return "resolution";
    if (r.value == &opts.fps_cap) return "frame_rate_cap";   // (host/nn_vi.c reads it at the start)
    if (r.value == &opts.upscale) return "upscaling";
    if (r.value == &opts.dlss) return "dlss";   // (as opts.dlss and RX_VK_DLSS)
    if (r.value == &opts.handheld) return "handheld_profile";   // (the Profile row; host/nn_applet.c reads it at the start)
    if (r.value == &opts.mesh_mode) return "mesh_shaders";   // (gpu/vk/vk_main.cpp reads it as the device is made)
    static char k[32];
    size_t n = 0;
    for (const char* p = r.name; *p && n + 1 < sizeof k; p++) k[n++] = *p == ' ' ? '_' : (char)tolower(*p);
    k[n] = 0;
    return k;
}
// (hookshot) the controls, kept by the host (host/nn_hid.c reads them back from here): pad_<button>, key_<button>
const char* const bind_names[HOS_KEY_N] = {"a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus", "minus",
                                           "left", "up", "right", "down", "move_up", "move_down", "move_left",
                                           "move_right", "cam_up", "cam_down", "cam_left", "cam_right"};
bool can_bind() { return host && host->binding && host->bind && host->pad_raw; }
const char* tx_pad_layout(int v) {
    static const char* t[] = {"Nintendo (Def)", "Xbox / PC", "Swap Face", "Custom"};
    return t[std::min(std::max(v, 0), 3)];
}
void apply_pad_layout(int layout) {
    if (!can_bind()) return;
    if (layout == 0) { // Nintendo (Default: Bottom=B, Right=A, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    } else if (layout == 1) { // Xbox / PC Standard (Bottom=A, Right=B, Left=X, Top=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_WEST);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_NORTH);
    } else if (layout == 2) { // Swap Face Buttons (Bottom=A, Right=B, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    }
}
int detect_pad_layout() {
    if (!can_bind()) return opts.pad_layout;
    const int a = host->binding(HOS_BIND_PAD, HOS_SW_A, 0);
    const int b = host->binding(HOS_BIND_PAD, HOS_SW_B, 0);
    const int x = host->binding(HOS_BIND_PAD, HOS_SW_X, 0);
    const int y = host->binding(HOS_BIND_PAD, HOS_SW_Y, 0);
    if (a == HOS_PAD_EAST && b == HOS_PAD_SOUTH && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 0;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_WEST && y == HOS_PAD_NORTH) return 1;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 2;
    return 3;
}
// (hookshot) the row's choice to the host: off, the default, or a microphone by its name (and its place in the host's list,
// -1 while it is unplugged: of two with one name, the one chosen). Unplugged, the name is the kept one, cut: the host takes
// a name that starts with it
void mic_tell_host() {
    if (!can_mic()) return;
    if (opts.mic <= 1) host->mic_select(opts.mic <= 0 ? 0 : 1, "", -1);
    else if (mic_missing && opts.mic == 2 + mic_listed) host->mic_select(2, mic_want.c_str(), -1);   // (the kept name, cut)
    else host->mic_select(2, host->mic_name(opts.mic - 2), opts.mic - 2);   // (its whole name: the ini's cut is only the ini's)
}
void mic_row_ends() {
    for (Row& r : rows)
        if (r.value == &opts.mic) r.hi = 1 + mic_listed + (mic_missing ? 1 : 0);
}
// (hookshot) the microphones listed again (one plugged in since shows up), the row on mic_want's: the one at the row's place
// when it still has that name (two microphones of one name: the second stays chosen), else the first of that name, else
// its "(not connected)" entry while it is unplugged
void refresh_mics() {
    if (!can_mic()) {
        if (opts.mic >= 2) opts.mic = 1;
        return;
    }
    mic_listed = std::min(host->mic_count(), kMaxMics);
    int at = -1;
    if (!mic_want.empty()) {
        if (opts.mic >= 2 && opts.mic - 2 < mic_listed && mic_key(host->mic_name(opts.mic - 2)) == mic_want) at = opts.mic - 2;
        for (int i = 0; i < mic_listed && at < 0; i++)
            if (mic_key(host->mic_name(i)) == mic_want) at = i;
        opts.mic = at >= 0 ? at + 2 : 2 + mic_listed;
    } else if (opts.mic >= 2) {
        opts.mic = 1;
    }
    mic_missing = !mic_want.empty() && at < 0;
    mic_row_ends();
    mic_tell_host();
}
// (the row changed by the player: the choice is now this one, by its name; the "(not connected)" entry is kept only while
// it is the one chosen)
void mic_chosen() {
    if (opts.mic >= 2 && !(mic_missing && opts.mic == 2 + mic_listed)) {
        mic_want = mic_key(host->mic_name(opts.mic - 2)), mic_missing = false;
    } else if (opts.mic < 2) {
        mic_want.clear(), mic_missing = false;
    }
    mic_row_ends();
    mic_tell_host();
}
void save() {
    // (hookshot) the lines that are not the menu's are kept: the controls, pad_ and key_ (host/nn_hid.c)
    // (and every other line the menu does not write, such as discord_app_id=: they were dropped at the next save)
    std::string kept, others;
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4)) {
                kept += line;
                continue;
            }
            const char* eq = strchr(line, '=');
            if (!eq || eq == line) continue;
            const std::string key(line, (size_t)(eq - line));
            bool mine = key == "microphone_device" || key == "options_version";
            for (const Row& r : rows)
                if (!mine && r.value && key == ini_key(r)) mine = true;
            if (!mine) others += line, others += others.back() == '\n' ? "" : "\n";
        }
        fclose(f);
    }
    if (FILE* f = fopen(ini_path().c_str(), "w")) {
        for (const Row& r : rows) {
            if (r.value == &opts.output_res) {   // (the size itself: host/nn_applet.c reads it)
                const Out o = output_at(*r.value);
                fprintf(f, "%s=%ux%u\n", ini_key(r), o.w, o.h);
            } else if (r.value) {
                fprintf(f, "%s=%d\n", ini_key(r), *r.value);
            }
        }
        if (!mic_want.empty()) fprintf(f, "microphone_device=%s\n", mic_want.c_str());   // (hookshot: also while it is unplugged)
        // (2: game_speed below 2 was only ever the old default, see menu_init; 3: the mouse's buttons are bindings, see
        // host/nn_hid.c binds_load; 4: Descriptor reuse on by default, see menu_init and vk_draw.cpp desc_slotgen_on)
        fprintf(f, "options_version=4\n");
        fputs(others.c_str(), f);
        if (!can_bind()) fputs(kept.c_str(), f);
        for (int i = 0; i < HOS_KEY_N && can_bind(); i++) {   // (the keys-only controls, HOS_KEY_*, have no pad_ line)
            if (i < HOS_SW_N) fprintf(f, "pad_%s=%d\n", bind_names[i], host->binding(HOS_BIND_PAD, i, 0));
            fprintf(f, "key_%s=%d,%d\n", bind_names[i], host->binding(HOS_BIND_KEY, i, 0), host->binding(HOS_BIND_KEY, i, 1));
        }
        fclose(f);
    }
}

/// A row's new value v, as the arrows set it (dir: 1 up, -1 down, for the rows that skip or wrap), saved unless !keep
/// (a slider's drag: saved once it is let go)
void set_row(Row& r, int v, int dir, bool keep = true) {
    // (nothing else to choose: no change, no save. A saved choice that cannot run here is kept)
    if (row_fixed(r.value)) return;
    // (the scaling rows: only what can run here; the Render resolution row through the Upscaler's own steps)
    // (the Upscaler by engine; the Render resolution's Native is the engine's full-size form: upscaler_rows.h)
    if (r.value == &opts.dlss) v = ur::step_engine(upscaler(), dir, upscalers_available());
    if (r.value == &opts.resolution) {
        int up = upscaler(), res = *r.value;
        ur::step_render(upscaler(), *r.value, dir, &up, &res);
        if (up != upscaler()) opts.dlss = up;   // (Native in or out: DLAA / DLSS, FSR 4's native AA / FSR 4)
        v = res;
    }
    if (r.value == &opts.output_res) {   // (smallest to largest: step_output_choice)
        v = step_output_choice(*r.value, dir);
        if (v == *r.value) return;
    }
    if (stops_at_ends(r)) v = std::min(std::max(v, r.lo), r.hi);   // (a scale: it stops at its ends)
    if (r.value == &opts.pad_layout) {
        if (v > 2) v = dir > 0 ? 0 : 2;
        if (v < 0) v = 2;
        *r.value = v;
        apply_pad_layout(*r.value);
    } else if (r.value == &opts.preset) {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        apply_preset(*r.value);
    } else {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        // (another Upscaler: the Render resolution moves to the nearest of its steps, DLSS's modes or the game's)
        if (r.value == &opts.dlss) opts.resolution = ur::snap_render(upscaler(), opts.resolution);   // (a slider stop kept)
        if (!r.preset_exempt) {   // (the controls, the window and the counter are not the preset's)
            opts.preset = 0; // Custom
            sync_preset_budgets(0);
        }
    }
    if (r.value == &opts.mic && can_mic()) mic_chosen();   // (hookshot)
    // (hookshot: the samplers made again, at once: their anisotropy, and the upscalers' LOD bias, vk_draw.cpp upscale_lod_bias)
    if (r.value == &opts.anisotropy || r.value == &opts.dlss || r.value == &opts.resolution || r.value == &opts.preset)
        samplers_changed();
    {   // (a row that takes a restart, or a preset that sets one: asked again at the next close)
        int now[mr::kMax];
        restart_values(now);
        restart_state.note(now);
    }
    if (keep) save();
}

// ---- (hookshot) the Controls page: what presses each of the Switch's buttons, a gamepad input and up to two keys ----
struct Control {
    int button;   // HOS_SW_*, or HOS_KEY_* (keys only)
    const char* name;
};
const Control controls[] = {{HOS_SW_A, "A"}, {HOS_SW_B, "B"}, {HOS_SW_X, "X"}, {HOS_SW_Y, "Y"}, {HOS_SW_L, "L"},
                            {HOS_SW_R, "R"}, {HOS_SW_ZL, "ZL"}, {HOS_SW_ZR, "ZR"}, {HOS_SW_PLUS, "Plus"},
                            {HOS_SW_MINUS, "Minus"}, {HOS_SW_LS, "Left stick press"}, {HOS_SW_RS, "Right stick press"},
                            {HOS_SW_UP, "D-pad up"}, {HOS_SW_DOWN, "D-pad down"}, {HOS_SW_LEFT, "D-pad left"},
                            {HOS_SW_RIGHT, "D-pad right"}, {HOS_KEY_MOVE_UP, "Move forward"},
                            {HOS_KEY_MOVE_DOWN, "Move back"}, {HOS_KEY_MOVE_LEFT, "Move left"},
                            {HOS_KEY_MOVE_RIGHT, "Move right"}, {HOS_KEY_CAM_UP, "Camera up"},
                            {HOS_KEY_CAM_DOWN, "Camera down"}, {HOS_KEY_CAM_LEFT, "Camera left"},
                            {HOS_KEY_CAM_RIGHT, "Camera right"}};
constexpr int kControls = sizeof controls / sizeof controls[0];   // (then "Reset to defaults")
bool keys_only(int i) { return i < kControls && controls[i].button >= HOS_SW_N; }   // (no gamepad input for those)
// a pad input's name on the controller of the button prompts (the Button prompts row's choice; Auto: the face in use),
// else by its place
const char* pad_name(int p) {
    static const char* t[HOS_PROMPT_N + 1][HOS_PAD_N] = {   // (by place, then HOS_PROMPT_*)
        {"Bottom button", "Right button", "Left button", "Top button", "Select / Back", "Home", "Start", "Left stick press",
         "Right stick press", "L1 / LB", "R1 / RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "L2 / LT", "R2 / RT"},
        {"B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "ZL", "ZR"},
        {"A", "B", "X", "Y", "View", "Xbox", "Menu", "Left stick press", "Right stick press", "LB", "RB", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "LT", "RT"},
        {"Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down", "D-pad left",
         "D-pad right", "L2", "R2"}};
    const int face = opts.prompts ? opts.prompts - 1 : host && host->prompt_face ? host->prompt_face() : -1;
    return p >= 0 && p < HOS_PAD_N ? t[face >= 0 && face < HOS_PROMPT_N ? face + 1 : 0][p] : "None";
}
const char* key_name(int vk) {
    static char b[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return b[0] = (char)vk, b[1] = 0, b;
    if (vk >= VK_F1 && vk <= VK_F12) return snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1), b;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return snprintf(b, sizeof b, "Num %d", vk - VK_NUMPAD0), b;
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_CAPITAL: return "Caps Lock";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_MULTIPLY: return "Num *";
    case VK_DIVIDE: return "Num /";
    case VK_DECIMAL: return "Num .";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    case VK_LBUTTON: return "Left click";
    case VK_RBUTTON: return "Right click";
    case VK_MBUTTON: return "Middle click";
    case 0x05: return "Mouse 4";   // (VK_XBUTTON1)
    case 0x06: return "Mouse 5";
    }
    return snprintf(b, sizeof b, "Key %d", vk), b;
}
// the keys a binding can take (Esc cancels, Insert and F1 are the menu's, Delete clears)
const std::vector<int>& bindable_keys() {
    static std::vector<int> k;
    if (k.empty()) {
        for (int c = 'A'; c <= 'Z'; c++) k.push_back(c);
        for (int c = '0'; c <= '9'; c++) k.push_back(c);
        for (int c = VK_F2; c <= VK_F12; c++) k.push_back(c);
        for (int c = VK_NUMPAD0; c <= VK_NUMPAD0 + 9; c++) k.push_back(c);
        for (int c : {VK_SPACE, VK_SHIFT, VK_CONTROL, VK_MENU, VK_TAB, VK_RETURN, VK_BACK, VK_CAPITAL, VK_UP, VK_DOWN,
                      VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_ADD, VK_SUBTRACT, VK_MULTIPLY,
                      VK_DIVIDE, VK_DECIMAL, VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
                      VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                      0x05, 0x06})   // (the mouse's buttons too: they count while the mouse is captured)
            k.push_back(c);
    }
    return k;
}
int page = 0;          // 0 the options, 1 the controls, 3 the teleport list, 4 the inventory, 5 the restart question (2: was the
                       // debug tools' own page, now on the Debug tab)
int csel = 0, ccol = 0;   // the page's row (kTop first: its own rows, then the controls, then reset) and column (0 the
                          // gamepad, 1 the keyboard)
const int kTop = tab_row_count(TAB_PAGE);   // (Layout preset and Button prompts: a preset shows in the grid at once)
int listening = 0;     // waiting for the new input: 1 a gamepad's, 2 a key
bool listen_armed = false;   // (once what started it was let go)
int64_t listen_end = 0;
std::string keys_text(int button) {
    const int a = host->binding(HOS_BIND_KEY, button, 0), b = host->binding(HOS_BIND_KEY, button, 1);
    std::string t = a ? key_name(a) : "";
    if (b) t += std::string(a ? ", " : "") + key_name(b);
    return t.empty() ? "None" : t;
}

// ---- what is drawn: Dear ImGui over the window's image (vk_imgui.h) in the game's own style (vk_ui.h). Built anew at each
// present while something shows, from the menu's state (menu_tick and menu_record run on the presenting thread, one at a time:
// present_image's lock); nothing at all while nothing shows ----
std::mutex startup_mu;   // (the start-up screen's view, set from its own thread)
StartupView startup;
std::atomic<bool> startup_on{false};
std::atomic<float> startup_fade{0};   // (the start-up screen's fade to black at its end, 0 to 1: startup_fade_to)
std::atomic<int64_t> game_frames{0}, shown_frames{0};
char fps_text[48] = "";
char fps_sub[32] = "";
menu_hit::Map hits;      // (the menu's controls as last drawn: the mouse's, menu_tick)
float scroll_at[TAB_MOTION + 2];   // (the first row shown, eased toward the selection: the options' tabs and the Motion page, then
                                   // the Controls page: kControlsSlot)
constexpr int kControlsSlot = TAB_MOTION + 1;
int wheel_rows[TAB_MOTION + 2];   // (the mouse wheel's notches since the last drawing, a slot each: scroll_rows scrolls by them)
int scrolled_to[TAB_MOTION + 2] = {-1, -1, -1, -1, -1, -1};   // (the selection scroll_rows last brought into view)
float scroll_want[TAB_MOTION + 2];   // (where scroll_at is going: the wheel moves it, a new selection too)
float ease_step = 0;     // (this drawing's share of the way: the time since the last one)

ui::Ctx ui_ctx(uint32_t w, uint32_t h) {
    ui::Ctx c{ImGui::GetForegroundDrawList(), ui::fonts(), 1, (float)w, (float)h, 0, pad_last};
    c.confirm_dot = menu_keys::dot(pad_confirm), c.back_dot = menu_keys::dot(pad_back);
    c.s = ui::unit((float)h, opts.menu_size, &c.safe);
    return c;
}
/// The banner's height
float banner_h(const ui::Ctx& c) { return std::max(58 * c.s, c.safe + 40 * c.s); }

/// The tallest help a row of the options can have, in pixels at width: its help and the tallest line any of its values can
/// have: the help box keeps that height whichever row is selected and whatever its value
float measure_help(ui::Ctx& c, float width) {
    float most = 0;
    for (const Row& r : rows) {
        float hint = 0;
        for (int v = r.lo; r.value && r.hint && v <= r.hi; v++)
            for (int k = 0; const char* t = r.hint(v, k); k++)
                hint = std::max(hint, ui::para(c, c.f.body, 16 * c.s, ImVec2(), 0, t, width - 18 * c.s, false));
        if (r.help) most = std::max(most, ui::para(c, c.f.body, 18 * c.s, ImVec2(), 0, r.help, width, false) + (hint ? 14 * c.s + hint : 0));
    }
    return most;
}
/// label in room pixels: as it is when it fits; the selected row's slides through it (marquee_pos, marquee_max for menu_tick);
/// another's ends with "..."
/// (UTF-8: the byte offsets where its characters start, and the end; labels are cut and slid a character at a time, never
/// inside one: a microphone's name can be in any script)
std::vector<size_t> char_starts(const std::string& t) {
    std::vector<size_t> at;
    for (size_t i = 0; i < t.size(); i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) at.push_back(i);
    at.push_back(t.size());
    return at;
}
std::string fit(ui::Ctx& c, ImFont* f, float px, float room, const std::string& label, bool sel) {
    auto width = [&](const std::string& t) { return ui::size_of(f, px, t.c_str()).x; };
    if (width(label) <= room) return label;
    const std::vector<size_t> at = char_starts(label);
    const int chars = (int)at.size() - 1;
    if (sel) {   // (marquee_pos and marquee_max count characters)
        int start = chars;   // (the last start that still shows the label's end)
        while (start > 0 && width(label.substr(at[start - 1])) <= room) start--;
        marquee_max = start;
        const int from = std::min(marquee_pos, marquee_max);
        int end = chars;
        while (end > from + 1 && width(label.substr(at[from], at[end] - at[from])) > room) end--;
        return label.substr(at[from], at[end] - at[from]);
    }
    int end = chars;
    while (end > 1 && width(label.substr(0, at[end]) + "...") > room) end--;
    std::string t = label.substr(0, at[end]);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + "...";
}
/// A list of rows from top to bottom (pixels), n of them pitch apart, the selection sel kept in view: the first one shown,
/// eased toward its place (scroll_at[slot]); *shown how many fit
float scroll_rows(int slot, int n, int sel, float top, float bottom, float pitch, int* shown) {
    const int fit = std::max(1, (int)((bottom - top) / pitch));
    *shown = fit;
    float& at = scroll_at[slot];
    float& want = scroll_want[slot];
    const int wheel = std::exchange(wheel_rows[slot], 0);
    if (n <= fit) want = 0;   // (nothing to scroll: the wheel does nothing)
    else {
        want -= (float)wheel;   // (the wheel scrolls the rows and leaves the selection, and every value, as they are)
        static int fits[sizeof scroll_at / sizeof *scroll_at];   // (and after a resize: the rows that fit changed)
        if (sel != scrolled_to[slot] || fit != std::exchange(fits[slot], fit)) {   // (a new selection, by the keys or the pad: brought
                                                                                  // into view; the mouse's marks itself scrolled to)
            const int margin = fit > 4 ? 1 : 0;   // (a row of context above and below the selection)
            if (sel - margin < want) want = (float)(sel - margin);
            if (sel + margin + 1 > want + fit) want = (float)(sel + margin + 1 - fit);
        }
        want = std::round(std::clamp(want, 0.0f, (float)(n - fit)));
    }
    scrolled_to[slot] = sel;
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.01f) at = want;
    return at;
}
/// The scroll bar at x, beside a list of n rows of which shown fit, the first at `first`
void scroll_bar(ui::Ctx& c, float x, float top, float bottom, int n, int shown, float first) {
    if (n <= shown) return;
    const float s = c.s, span = bottom - top, y0 = top + span * first / n, y1 = top + span * (first + shown) / n;
    c.d->AddRectFilled(ImVec2(x - 2 * s, top), ImVec2(x + 2 * s, bottom), ui::rgba(0, 0, 0, 140), 2 * s);
    c.d->AddRectFilled(ImVec2(x - 2 * s, y0), ImVec2(x + 2 * s, y1), ui::kAccent, 2 * s);
}
/// The banner with a page's title in the middle (the Controls, Cheats and list pages), its height
float title_banner(ui::Ctx& c, const char* title, const char* right) {
    const float hh = banner_h(c), s = c.s;
    ui::banner(c, hh);
    ui::text_mid_centre(c, c.f.heading, 24 * s, c.w * 0.5f, hh * 0.5f + s, ui::kBannerInk, title);
    if (right) ui::text_mid_right(c, c.f.body, 17 * s, c.w - c.safe, hh * 0.5f + s, ui::kBannerInk, right);
    return hh;
}
/// The help box from (x0, y0) to x1: a title, the help, a line set off by a bar, a gold line at its foot. min_h: its height at
/// least (the tallest help the page can have, so it does not jump)
void help_box(ui::Ctx& c, float x0, float x1, float y0, const char* title, const char* help, const char* hint, const char* gold_line,
              float min_h = 0, bool after_restart = false, float y_min = 0, float y_max = FLT_MAX) {
    // (y_max: the box ends above it, the prompt strip's top: moved up as far as y_min (under the banner) when it is taller than
    // the room below y0, its reserved height (min_h) given up first, its text cut at its foot only if it still does not fit)
    const float s = c.s, w = x1 - x0 - 48 * s;
    const float th = help ? ui::para(c, c.f.body, 18 * s, ImVec2(), 0, help, w, false) : 0,
                hh = hint ? ui::para(c, c.f.body, 16 * s, ImVec2(), 0, hint, w - 18 * s, false) : 0;
    const float text_h = th + (hint ? 14 * s + hh : 0), note = after_restart ? 30 * s : 0;
    const float fixed = 22 * s + (title ? 32 * s : 0) + note + (gold_line ? 52 * s : 0) + 22 * s;
    float body = std::max(min_h, text_h);
    if (y0 + fixed + body > y_max) y0 = std::max(y_min, y_max - fixed - body);
    if (y0 + fixed + body > y_max) body = std::max(text_h, y_max - y0 - fixed);
    const float y1 = std::min(y0 + fixed + body, y_max);
    ui::box(c, ImVec2(x0, y0), ImVec2(x1, y1));
    c.d->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    float y = y0 + 22 * s;
    if (title) ui::text(c, c.f.heading, 21 * s, ImVec2(x0 + 24 * s, y), ui::kInk, title), y += 32 * s;
    if (after_restart) {   // (this row's change applies at the next start: said here, not in the row, where it would cut the name)
        ui::diamond(c, ImVec2(x0 + 29 * s, y + 10 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, 16 * s, x0 + 42 * s, y + 10 * s, ui::kGoldHi, "Takes effect after a restart");
        y += note;
    }
    if (help) ui::para(c, c.f.body, 18 * s, ImVec2(x0 + 24 * s, y), ui::kInk, help, w);
    if (hint) {
        const float hy = y + th + 14 * s;
        c.d->AddRectFilled(ImVec2(x0 + 24 * s, hy + 2 * s), ImVec2(x0 + 26.5f * s, hy + hh - 2 * s), ui::kAccent, s);
        ui::para(c, c.f.body, 16 * s, ImVec2(x0 + 40 * s, hy), ui::kSoft, hint, w - 18 * s);
    }
    if (gold_line) {
        y += body + 20 * s;
        c.d->AddLine(ImVec2(x0 + 24 * s, y), ImVec2(x1 - 24 * s, y), ui::alpha(ui::kEdge, 0.8f), std::max(1.0f, s));
        ui::diamond(c, ImVec2(x0 + 29 * s, y + 20 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, 16 * s, x0 + 42 * s, y + 20 * s, ui::kGoldHi, gold_line);
    }
    c.d->PopClipRect();
}
/// The menu's column: from the safe margin to about 58% of the width (wide enough for a row at any unit); the help box beside it,
/// or, on a narrow window, over the column's foot (*narrow)
void columns(const ui::Ctx& c, float* x0, float* x1, float* hx0, float* hx1, bool* narrow) {
    const float s = c.s;
    *x0 = c.safe + 6 * s;
    *x1 = std::min(c.w - c.safe, std::max(c.w * 0.585f, *x0 + 560 * s));
    *hx0 = *x1 + 28 * s, *hx1 = c.w - c.safe;
    *narrow = *hx1 - *hx0 < 260 * s;
    if (*narrow) *hx0 = *x0 + 20 * s, *hx1 = *x1 - 20 * s;
}
void column(ui::Ctx& c, float x0, float x1, float top) {
    const float s = c.s;
    c.d->AddRectFilled(ImVec2(x0, top), ImVec2(x1, c.h), ui::kColumn);
    c.d->AddLine(ImVec2(x0, top), ImVec2(x0, c.h), ui::alpha(ui::kEdge, 1), 2 * s);
    c.d->AddLine(ImVec2(x1, top), ImVec2(x1, c.h), ui::alpha(ui::kEdge, 1), 2 * s);
}
/// A row's name cell, selected or not, its tag (After restart) at its right end
/// A row's name cell, selected or not; pending: a change that applies after a restart, a small gold diamond at its right end
/// (the help says it in words)
void name_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* name, bool sel, bool pending = false, float px = 18.5f) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f;
    c.d->AddRectFilled(a, b, sel ? ui::kCellNameSel : ui::kCellName);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    if (sel) c.d->AddRectFilled(a, ImVec2(a.x + 4 * s, b.y), ui::kGoldHi);
    float room = b.x - a.x - 36 * s;
    if (pending) {
        ui::diamond(c, ImVec2(b.x - 12 * s, mid), 4 * s, ui::kGoldHi);
        room -= 14 * s;
    }
    ImFont* f = sel ? c.f.strong : c.f.body;
    const std::string shown = fit(c, f, px * s, room, name, false);
    ui::text_mid(c, f, px * s, a.x + 20 * s, mid, sel ? ui::kInkBright : ui::kInk, shown.c_str());
}
/// A value cell: a choice with arrows at its ends (left, right: whether it can go that way), gold when selected
void choice_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const std::string& value, bool sel, bool left, bool right, bool fixed = false) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s;
    c.d->AddRectFilled(a, b, ui::kCellDark);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    if (sel && !fixed) ui::gold(c, a + ImVec2(in, in), b - ImVec2(in, in));
    const ImU32 ink = sel && !fixed ? ui::kGoldInk : fixed ? ui::kDim : ui::kInk, arrow = sel && !fixed ? ui::kGoldInk : ui::kDim;
    if (left) ui::triangle(c, ImVec2(a.x + in + 11 * s, mid), 9 * s, 0, arrow);
    if (right) ui::triangle(c, ImVec2(b.x - in - 11 * s, mid), 9 * s, 1, arrow);
    ImFont* f = sel ? c.f.strong : c.f.body;
    const std::string shown = fit(c, f, px, b.x - a.x - 2 * in - (left || right ? 48 : 16) * s, value, sel);   // (room for arrows)
    ui::text_mid_centre(c, f, px, (a.x + b.x) * 0.5f, mid, ink, shown.c_str());
    if (sel) ui::glow(c, a, b);
}
/// A value cell of two halves (a row of two values: On and Off), the chosen one filled
void halves_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* lo, const char* hi, bool high, bool sel, bool fixed) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s, xm = (a.x + b.x) * 0.5f;
    c.d->AddRectFilled(a, b, ui::kCellDark);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    const ImVec2 fa = high ? ImVec2(xm, a.y + in) : a + ImVec2(in, in), fb = high ? b - ImVec2(in, in) : ImVec2(xm, b.y - in);
    if (sel && !fixed) ui::gold(c, fa, fb);
    else c.d->AddRectFilled(fa, fb, ui::kBronze);
    const ImU32 on = sel && !fixed ? ui::kGoldInk : ui::kInk;
    ui::text_mid_centre(c, high ? c.f.body : c.f.strong, px, (a.x + xm) * 0.5f, mid, high ? ui::kDim : on, lo);
    ui::text_mid_centre(c, high ? c.f.strong : c.f.body, px, (xm + b.x) * 0.5f, mid, high ? on : ui::kDim, hi);
    if (sel) ui::glow(c, a, b);
}
/// A slider's value cell: the value, a track with its knob along the cell's foot; *t0, *t1 the track (for the mouse)
void slider_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* value, int permille, bool sel, float* t0, float* t1) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s;
    c.d->AddRectFilled(a, b, ui::kCellDark);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    if (sel) ui::gold(c, a + ImVec2(in, in), b - ImVec2(in, in));
    const float x0 = a.x + in + 24 * s, x1 = b.x - in - 24 * s, xv = x0 + (x1 - x0) * permille / 1000.0f, ty = b.y - in - 5.5f * s;
    const ImU32 track = sel ? ui::rgba(36, 24, 8, 80) : ui::rgba(255, 255, 255, 40), done = sel ? ui::kGoldInk : ui::kAccent;
    c.d->AddRectFilled(ImVec2(x0, ty - 1.5f * s), ImVec2(x1, ty + 1.5f * s), track, 1.5f * s);
    c.d->AddRectFilled(ImVec2(x0, ty - 1.5f * s), ImVec2(xv, ty + 1.5f * s), done, 1.5f * s);
    ui::diamond(c, ImVec2(xv, ty), 4.5f * s, done);
    ui::text_mid_centre(c, c.f.strong, px, (a.x + b.x) * 0.5f, mid - 3 * s, sel ? ui::kGoldInk : ui::kInk, value);
    const ImU32 arrow = sel ? ui::kGoldInk : ui::kDim;
    ui::triangle(c, ImVec2(a.x + in + 11 * s, mid), 9 * s, 0, arrow), ui::triangle(c, ImVec2(b.x - in - 11 * s, mid), 9 * s, 1, arrow);
    if (sel) ui::glow(c, a, b);
    *t0 = x0, *t1 = x1;
}
/// "Restart needed: " and the pending rows, on one line of width: the last ones as "and N more" when they do not fit
std::string restart_line(ui::Ctx& c, float px, float width) {
    const unsigned all = restart_pending();
    std::string line;
    for (int keep = restart_state.n; keep >= 0; keep--) {   // (the first `keep` pending rows by name)
        unsigned shown = 0;
        int named = 0, more = 0;
        for (int i = 0; i < restart_state.n; i++)
            if ((all >> i) & 1) (named < keep ? (shown |= 1u << i, named++) : more++);
        line = "Restart needed: " + mr::names(restart_names, restart_state.n, shown);
        if (more) line += (named ? " and " : "") + std::to_string(more) + (named ? " more" : more > 1 ? " settings" : " setting");
        if (ui::size_of(c.f.strong, px, line.c_str()).x <= width) break;
    }
    return line;
}

/// (hookshot) The Debug tab's order: the tools' rows by their group (Row::cheat), Up and Down walking them so; Inventory item
/// and Change quantity are not shown (the Inventory page edits every amount)
struct DebugGroup { const char* title; int from, to; };   // (debug_tools_order[from, to))
constexpr int debug_tools_order[] = {0, 1, 11, 9, 10, 8, 2, 3, 7, 4};
constexpr DebugGroup debug_groups[] = {{"Player", 0, 5}, {"Travel", 5, 8}, {"Camera & world", 8, 10}};
constexpr int kDebugGroups = sizeof debug_groups / sizeof debug_groups[0];
constexpr int kDebugTools = sizeof debug_tools_order / sizeof debug_tools_order[0];
/// The Debug tab's rows as shown (its row numbers, tab_row_to_global's): the switch, the tools in debug_tools_order while they
/// are on, then the Developer rows; *dev where those start, *dev_head the Developer heading's row (-1 none). Their count
int debug_rows(int* out, int* dev, int* dev_head) {
    const int count = tab_row_count(TAB_DEBUG);
    int n = 0;
    *dev_head = -1;
    for (int k = 0; k < count; ++k)
        if (rows[tab_row_to_global(TAB_DEBUG, k)].value == &opts.debug_tools) out[n++] = k;
    for (int t = 0; t < kDebugTools; ++t)
        for (int k = 0; k < count; ++k)
            if (rows[tab_row_to_global(TAB_DEBUG, k)].cheat == debug_tools_order[t]) out[n++] = k;
    *dev = n;
    for (int k = 0; k < count; ++k) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        if (r.heading) *dev_head = k;
        else if (*dev_head >= 0 && r.cheat < 0 && r.value != &opts.debug_tools) out[n++] = k;
    }
    return n;
}
/// The Debug tab's row after sel (dir 1) or before it (dir -1), in debug_rows' order
int debug_step(int sel, int dir) {
    int order[64], dev = 0, head = -1;
    const int n = debug_rows(order, &dev, &head);
    int at = 0;
    while (at < n && order[at] != sel) at++;
    if (at == n) return order[0];
    return order[(at + n + dir) % n];
}
/// (hookshot) The Debug tab's rows from top down to bottom, sel selected: Debug tools across the top; the tools under their
/// headings, Player in the first column and Travel and Camera & world in the second when two fit (else one under another);
/// the Developer rows across the foot. Each value cell a column's width, so their edges line up. The hits are the tab's row
/// numbers, as the list's: menu_tick's clicks and keys are the other tabs'
void draw_debug_rows(ui::Ctx& c, float cx0, float cx1, float top, float bottom, int sel) {
    const float s = c.s, px = 18 * s, pitch = 48 * s, rh = 40 * s, gap = 24 * s;   // (the other tabs' rows: 40 high, 48 apart)
    const float left = cx0 + 44 * s, right = cx1 - 44 * s, mid = (left + right) * .5f;
    int order[64], dev = 0, dev_head = -1;
    const int n = debug_rows(order, &dev, &dev_head);
    // (two columns only when each fits its longest name whole (name_cell's room, the selected's bold) and its value cells at
    // their narrowest; the cells then as wide as the room lets them, up to what they ask)
    const float halves_w = 2 * std::max(ui::size_of(c.f.strong, px, "Off").x, ui::size_of(c.f.strong, px, "On").x);
    auto need = [&](const Row& r, bool tight) {
        const float pad = (tight ? 36 : 64) * s;
        if (r.cheat == 9 && age_shown(9))
            return 2 * std::max(ui::size_of(c.f.strong, px, "Kid").x, ui::size_of(c.f.strong, px, "Adult").x) + pad;
        if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7 || r.value) return halves_w + pad;
        const std::string v = r.cheat == 11 ? "1.25x" : cheat_value(r.cheat);   // (Movement speed: its widest, the cell kept still)
        const char* const says[kCheatRows] = {"", "", "Mark", "Return", "", "", "", "", "Choose map", "Switch", "Edit items", ""};
        return ui::size_of(c.f.strong, px, v.empty() ? says[r.cheat] : v.c_str()).x + (cheat_chooses(r.cheat) ? 68 : tight ? 56 : 60) * s;   // (choice_cell keeps 48 for arrows either way)
    };
    float name_need[2] = {0, 0}, cell_full[2] = {halves_w + 64 * s, halves_w + 64 * s}, cell_tight[2] = {halves_w + 36 * s, halves_w + 36 * s};
    for (int i = 1; i < dev; ++i) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, order[i])];
        const int col = i - 1 >= debug_groups[0].to ? 1 : 0;   // (Player in the first, the rest in the second)
        name_need[col] = std::max(name_need[col], ui::size_of(c.f.strong, 18.5f * s, r.name).x + 40 * s);
        cell_full[col] = std::max(cell_full[col], need(r, false)), cell_tight[col] = std::max(cell_tight[col], need(r, true));
    }
    const float col_w = (right - left - gap) * .5f;
    const bool two = col_w >= name_need[0] + cell_tight[0] && col_w >= name_need[1] + cell_tight[1];
    // (where each goes: a row number of the layout and a column)
    int vrow[64] = {}, vcol[64] = {}, head_row[kDebugGroups] = {}, head_col[kDebugGroups] = {};
    int next_row[2] = {1, 1};
    const bool tools = dev > 1;
    for (int g = 0; tools && g < kDebugGroups; ++g) {
        const int col = two && g > 0 ? 1 : 0;
        if (next_row[col] > 1) next_row[col]++;   // (a gap over a heading that follows another group)
        head_row[g] = next_row[col]++, head_col[g] = col;
        for (int t = debug_groups[g].from; t < debug_groups[g].to; ++t) vrow[1 + t] = next_row[col]++, vcol[1 + t] = col;
    }
    int foot = std::max(next_row[0], next_row[1]);
    if (tools) foot++;
    const int dev_row = foot;
    for (int i = dev; i < n; ++i) vrow[i] = dev_row + 1 + (i - dev);
    const int visual_count = n > dev ? dev_row + 1 + (n - dev) : foot;
    int sel_at = 0;
    for (int i = 0; i < n; ++i)
        if (order[i] == sel) sel_at = vrow[i];
    int shown = 0;
    const float first = scroll_rows(TAB_DEBUG, visual_count, sel_at, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), visual_count, shown, first);
    // (the value cells' widths, a column's alike: what they ask, given up toward their narrowest while a name needs the room)
    float cell_w[2];
    if (two) {
        for (int k = 0; k < 2; ++k) cell_w[k] = std::max(cell_tight[k], std::min(cell_full[k], col_w - name_need[k]));
    } else {   // (one column: as wide as any of the tools asks)
        const float w = std::max(cell_full[0], cell_full[1]);
        cell_w[0] = cell_w[1] = std::max(std::min(w, right - left - std::max(name_need[0], name_need[1])), std::max(cell_tight[0], cell_tight[1]));
    }
    auto heading = [&](float x0, float x1, int row, const char* title) {   // (as the list's Developer heading: gold, a rule after)
        const float y = top + (row - first) * pitch, ty = y + rh * 0.62f, tw = ui::size_of(c.f.heading, 19 * s, title).x;
        if (y + pitch <= top || y >= bottom) return;
        ui::text_mid(c, c.f.heading, 19 * s, x0 + 2 * s, ty, ui::kGoldHi, title);
        if (x0 + tw + 16 * s < x1) c.d->AddLine(ImVec2(x0 + tw + 16 * s, ty), ImVec2(x1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
    };
    c.d->PushClipRect(ImVec2(cx0, top), ImVec2(cx1, std::max(top, bottom)), true);
    for (int g = 0; tools && g < kDebugGroups; ++g)
        heading(head_col[g] ? mid + gap * .5f : left, !two || head_col[g] ? right : mid - gap * .5f, head_row[g], debug_groups[g].title);
    if (dev_head >= 0 && n > dev) heading(left, right, dev_row, rows[tab_row_to_global(TAB_DEBUG, dev_head)].name);
    const char* const buttons[kCheatRows] = {"", "", "Mark", "Return", "", "", "", "", "Choose map", "Switch", "Edit items", ""};
    for (int i = 0; i < n; ++i) {
        const int k = order[i];
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        const float y = top + (vrow[i] - first) * pitch;
        if (y + pitch <= top || y >= bottom) continue;
        const bool in_tools = i > 0 && i < dev;
        const float x0 = two && in_tools && vcol[i] ? mid + gap * .5f : left;
        const float x1 = !two || !in_tools || vcol[i] ? right : mid - gap * .5f;
        const bool selected = k == sel;
        const float width = in_tools ? cell_w[vcol[i]] : std::max(need(r, false), cell_w[two ? 1 : 0]);
        const ImVec2 va(x1 - width, y), vb(x1, y + rh);
        name_cell(c, ImVec2(x0, y), ImVec2(va.x, y + rh), r.name, selected, restart_changed(r));   // (boxed, as on the other tabs)
        if (r.value) {   // (Debug tools and the Developer rows: values of the options, Off and On)
            halves_cell(c, va, vb, std::string(r.text(r.lo)).c_str(), std::string(r.text(r.hi)).c_str(), *r.value == r.hi, selected,
                        row_fixed(r.value));
        } else if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7) {
            halves_cell(c, va, vb, "Off", "On", cheat_value(r.cheat) == "On", selected, false);
        } else if (age_shown(r.cheat)) {   // (Link's age now: the other half switches)
            halves_cell(c, va, vb, "Kid", "Adult", link_age.load(std::memory_order_relaxed) == 1, selected, false);
        } else {
            const std::string v = cheat_value(r.cheat);
            choice_cell(c, va, vb, v.empty() ? buttons[r.cheat] : v, selected, cheat_chooses(r.cheat), cheat_chooses(r.cheat), r.cheat == 9);
        }
        hits.add(x0, std::max(y, top), x1, std::min(y + rh, bottom), menu_hit::kRow, k, va.x, vb.x);
    }
    c.d->PopClipRect();
}

void draw_options(ui::Ctx& c) {
    const float s = c.s, hh = banner_h(c);
    ui::banner(c, hh);
    // the tabs in the banner: dark ink on the tan, the shown one light on a dark inset; the tab keys at their ends (a click there
    // is the previous or the next tab)
    const char* const tab_titles[TAB_COUNT] = {"Graphics", "Audio", "Gameplay", "Controls", "Debug"};
    float tw[TAB_COUNT], total = 0;
    for (int i = 0; i < TAB_COUNT; i++) tw[i] = ui::size_of(c.f.heading, 23 * s, tab_titles[i]).x + 56 * s, total += tw[i];
    float x = (c.w - total) * 0.5f;
    const float ty = hh * 0.5f + s;
    const char* const keys[2] = {c.pad ? "L" : "Q", c.pad ? "R" : "E"};
    for (int i = 0; i < 2; i++) {
        const float cw = 26 * s, cx = i ? x + total + 22 * s : x - 22 * s - cw;
        const bool hover = hover_tab == TAB_COUNT + i;
        c.d->AddRectFilled(ImVec2(cx, ty - 13 * s), ImVec2(cx + cw, ty + 13 * s), hover ? ui::kTabInset : ui::kBannerInk, 3 * s);
        ui::text_mid_centre(c, c.f.strong, 14 * s, cx + cw * 0.5f, ty, ui::kInk, keys[i]);
        hits.add(i ? cx - 10 * s : 0, 0, i ? c.w : cx + cw + 10 * s, hh, i ? menu_hit::kTabNext : menu_hit::kTabPrev, -1);
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        const bool shown = i == (cur_tab == TAB_MOTION ? TAB_CONTROLS : cur_tab);   // (the Motion page: under Controls)
        if (shown || i == hover_tab) {
            c.d->AddRectFilled(ImVec2(x, 0), ImVec2(x + tw[i], hh - 3 * s), shown ? ui::kTabInset : ui::alpha(ui::kTabInset, 0.35f));
            if (shown) {
                c.d->AddLine(ImVec2(x + 1, 0), ImVec2(x + 1, hh - 3 * s), ui::rgba(52, 34, 16), std::max(1.0f, s));
                c.d->AddLine(ImVec2(x + tw[i] - 1, 0), ImVec2(x + tw[i] - 1, hh - 3 * s), ui::rgba(52, 34, 16), std::max(1.0f, s));
            }
        }
        ui::text_mid(c, c.f.heading, 23 * s, x + 28 * s, ty, shown ? ui::kInkBright : ui::kBannerInk, tab_titles[i]);
        hits.add(x, 0, x + tw[i], hh, menu_hit::kTab, i);
        x += tw[i];
    }
    {   // (which build, at the banner's left, so a report can say it without opening the log; the release alone when the whole
        // line would run into the tabs)
        std::string ver = std::string("Hookshot ") + HOOKSHOT_VERSION;
        const float room = (c.w - total) * 0.5f - 60 * s - c.safe, px = 15 * s;
        const auto width = [&](float p) { return ui::size_of(c.f.body, p, ver.c_str()).x; };
        if (width(px) > room && ver.find(" (") != std::string::npos) ver.resize(ver.find(" ("));
        // (still too long, as a source build's "v2.0.11-821-g7b81752-dirty" since the Audio tab: at Small and Medium smaller
        // until it fits, on half-point steps, 10 points at the least; at Large, the size for a TV across the room, at its size
        // or not at all, as before)
        const float fit = ui::fit_px(px, opts.menu_size == 3 ? px : kVersionMinPt * s, 0.5f * s, room, width);
        if (fit > 0) ui::text_mid(c, c.f.body, fit, c.safe, ty, ui::kBannerInk, ver.c_str());
    }
    // the rows, in the column; the help beside it
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const int count = tab_row_count(cur_tab), sel = tab_selected[cur_tab];
    const Row& selr = rows[tab_row_to_global(cur_tab, sel)];
    const char* hint = selr.value && selr.hint ? selr.hint(*selr.value, -1) : nullptr;
    const std::string pending_line = (cur_tab == TAB_GRAPHICS || cur_tab == TAB_DEBUG) && restart_pending() ? restart_line(c, 16 * s, hx1 - hx0 - 66 * s) : "";
    static float help_tall = 0, help_tall_w = 0;
    if (help_tall_w != hx1 - hx0) help_tall_w = hx1 - hx0, help_tall = measure_help(c, hx1 - hx0 - 48 * s);
    // (a row that opens a page (Motion, Controls, Open debug tools): A or Enter opens it; Restart and Quit: A or Enter twice)
    const bool confirms = &selr == &rows[kRestartRow] || &selr == &rows[kQuitRow];
    ui::Prompt prompt_list[5] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"}, {ui::kTabs, "Tabs"}, {ui::kBack, "Close"}};
    int prompts_n = cur_tab == TAB_MOTION ? 3 : 4;
    if (!selr.value) prompt_list[1] = {ui::kConfirm, confirms ? (pending == &selr ? "Confirm" : selr.name) : "Open"};
    if (cur_tab == TAB_MOTION) prompt_list[2] = {ui::kBack, "Back"};   // (Esc: to Controls)
    if (selr.cheat >= 0) {   // (a debug tool: Change when Left and Right do something, then what confirm does)
        prompts_n = 1;
        if (selr.cheat <= 1 || selr.cheat == 7 || cheat_chooses(selr.cheat) || age_shown(selr.cheat))
            prompt_list[prompts_n++] = {ui::kLeftRight, "Change"};
        if (const char* says = cheat_confirm(selr.cheat)) prompt_list[prompts_n++] = {ui::kConfirm, says};
        prompt_list[prompts_n++] = {ui::kTabs, "Tabs"}, prompt_list[prompts_n++] = {ui::kBack, "Close"};
    }
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    float bottom = std::min(c.h - c.safe, bar_top) - 10 * s;   // (the rows end above the prompts)
    const bool after = restart_changed(selr);
    if (narrow) {   // (the help over the column's foot, the rows above it: at most half the room, the rows get the rest)
        const float help_h = std::min(22 * s + 32 * s + help_tall + (after ? 30 * s : 0) + (pending_line.empty() ? 0 : 52 * s) + 22 * s,
                                      (bottom - hh) * 0.5f);
        bottom = std::min(bar_top, c.h - c.safe) - help_h - 16 * s;
        help_box(c, hx0, hx1, bottom + 8 * s, selr.name, selr.help, hint, pending_line.empty() ? nullptr : pending_line.c_str(), help_tall,
                 after, bottom + 8 * s, bar_top - 10 * s);
    } else {
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * 0.30f), selr.name, selr.help, hint,
                 pending_line.empty() ? nullptr : pending_line.c_str(), help_tall, after, hh + 18 * s, bar_top - 12 * s);
    }
    const float rh = 40 * s, pitch = 48 * s, top = hh + 24 * s, rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s, split = rx0 + (rx1 - rx0) * 0.55f;
    if (cur_tab == TAB_DEBUG) {   // (the Debug tab: its own layout of the same rows)
        marquee_max = 0;
        draw_debug_rows(c, cx0, cx1, top, bottom, sel);
        ui::prompt_bar(c, prompt_list, prompts_n);
        return;
    }
    int shown = 0;
    const float first = scroll_rows(cur_tab, count, sel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), count, shown, first);
    marquee_max = 0;   // (set again below while the selected row's value is too long)
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int i = 0; i < count; i++) {
        const float y = top + (i - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const Row& r = rows[tab_row_to_global(cur_tab, i)];
        if (r.heading) {   // (a section's title: gold, a rule after it to the column's end; no cell, nothing to click)
            const float ty = y + rh * 0.62f, tw = ui::size_of(c.f.heading, 19 * s, r.name).x;
            ui::text_mid(c, c.f.heading, 19 * s, rx0 + 2 * s, ty, ui::kGoldHi, r.name);
            c.d->AddLine(ImVec2(rx0 + tw + 16 * s, ty), ImVec2(rx1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
            continue;
        }
        const bool selected = i == sel;
        name_cell(c, ImVec2(rx0, y), ImVec2(split, y + rh), r.name, selected, restart_changed(r));
        const ImVec2 va(split, y), vb(rx1, y + rh);
        float t0 = 0, t1 = 0;
        const std::string tool = r.cheat >= 0 ? cheat_value(r.cheat) : "";
        const bool tool_toggle = r.cheat == 0 || r.cheat == 1 || r.cheat == 7;   // (On and Off: halves, as the tabs' toggles)
        if (r.cheat >= 0 && tool_toggle) {
            halves_cell(c, va, vb, "Off", "On", tool == "On", selected, false);
        } else if (r.cheat >= 0 && !tool.empty()) {   // (a debug tool with a value: arrows when Left and Right choose it)
            choice_cell(c, va, vb, tool, selected, cheat_chooses(r.cheat), cheat_chooses(r.cheat));
        } else if (!r.value) {   // (a page, or Restart and Quit: Confirm until the second press)
            if (pending == &r) choice_cell(c, va, vb, "Press again to confirm", selected, false, false);
            else {
                choice_cell(c, va, vb, "", selected, false, false);
                ui::triangle(c, ImVec2(vb.x - 15 * s, y + rh * 0.5f), 10 * s, 1, selected ? ui::kGoldInk : ui::kInk);
            }
        } else {
            const bool fixed = row_fixed(r.value);
            const std::string lo = r.text(r.lo), hi = r.text(r.hi);
            const float half = (vb.x - va.x) * 0.5f - 18 * s;
            if (is_slider(r)) {
                slider_cell(c, va, vb, r.text(*r.value), menu_hit::slider_permille(*r.value, r.lo, r.hi), selected, &t0, &t1);
            } else if (r.hi - r.lo == r.step && !fixed && ui::size_of(c.f.strong, 18 * s, lo.c_str()).x <= half &&
                       ui::size_of(c.f.strong, 18 * s, hi.c_str()).x <= half) {
                halves_cell(c, va, vb, lo.c_str(), hi.c_str(), *r.value == r.hi, selected, fixed);
            } else {
                const bool output_row = r.value == &opts.output_res;
                const bool left = !fixed && (!output_row || step_output_choice(*r.value, -1) != *r.value) && (!stops_at_ends(r) || *r.value > r.lo);
                const bool right = !fixed && (!output_row || step_output_choice(*r.value, 1) != *r.value) && (!stops_at_ends(r) || *r.value < r.hi);
                choice_cell(c, va, vb, r.text(*r.value), selected, left, right, fixed);
            }
        }
        hits.add(rx0, std::max(y, top - 8 * s), rx1, std::min(y + rh, bottom + 8 * s), menu_hit::kRow, i, va.x, vb.x, t0, t1);
    }
    c.d->PopClipRect();
    ui::prompt_bar(c, prompt_list, prompts_n);   // (the prompts, on top of the help and the rows)
}

void draw_controls(ui::Ctx& c) {
    // (the page's own rows first, kTop of them (the layout preset, the button prompts), then a row per control and Reset to
    // defaults: one list, csel its selection)
    const float s = c.s, hh = title_banner(c, "Controls", nullptr);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const int n = kTop + kControls + 1, sel_i = csel - kTop;   // (sel_i: the control's, kControls Reset; negative: a page row)
    const ui::Prompt idle[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Column"}, {ui::kConfirm, "Bind"}, {ui::kKey, "Clear", "Del"},
                               {ui::kBack, "Back"}};
    const ui::Prompt option[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"}, {ui::kBack, "Back"}};
    const ui::Prompt wait[] = {{ui::kBack, "Cancel"}};
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const Row* page_row = sel_i < 0 ? &rows[tab_row_to_global(TAB_PAGE, csel)] : nullptr;
    const char* title = page_row ? page_row->name : sel_i < kControls ? controls[sel_i].name : "Reset to defaults";
    const char* help = listening ? (listening == 1 ? "Press the gamepad button to use. Esc cancels." : "Press the key to use. Esc cancels.")
                       : page_row ? page_row->help
                                  : "Which gamepad button and which keys press each of the Switch's buttons. Enter binds the one "
                                    "selected; Delete clears it. The game's button icons follow after a restart.";
    const char* hint = page_row ? (page_row->hint ? page_row->hint(*page_row->value, -1) : nullptr)
                                : "Gamepad: the D-pad moves, A binds, B goes back.";
    float bottom = std::min(c.h - c.safe, bar_top) - 10 * s;   // (the rows end above the prompts)
    if (narrow) {
        bottom = std::min(bar_top, c.h - c.safe) - 200 * s;
        help_box(c, hx0, hx1, bottom + 8 * s, nullptr, help, hint, nullptr, 0, page_row && restart_changed(*page_row), bottom + 8 * s,
                 bar_top - 10 * s);
    } else {
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * 0.30f), title, help, hint, nullptr, 0, page_row && restart_changed(*page_row),
                 hh + 18 * s, bar_top - 12 * s);
    }
    const float rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s, pad_x = rx0 + (rx1 - rx0) * 0.36f, key_x = rx0 + (rx1 - rx0) * 0.68f;
    float top = hh + 22 * s;
    ui::text_mid(c, c.f.body, 15 * s, rx0 + 20 * s, top + 10 * s, ui::kDim, "Button");
    ui::text_mid(c, c.f.body, 15 * s, pad_x + 14 * s, top + 10 * s, ui::kDim, "Gamepad");
    ui::text_mid(c, c.f.body, 15 * s, key_x + 14 * s, top + 10 * s, ui::kDim, "Keyboard");
    top += 26 * s;
    const float rh = 36 * s, pitch = 42 * s;
    int shown = 0;
    const float first = scroll_rows(kControlsSlot, n, csel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), n, shown, first);
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int j = 0; j < n; j++) {
        const float y = top + (j - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const bool sel = j == csel;
        const int i = j - kTop;
        const float y0 = std::max(y, top - 8 * s), y1 = std::min(y + rh, bottom + 8 * s);
        if (i < 0) {   // (one of the page's own rows: a name and a choice, as on the tabs)
            const Row& r = rows[tab_row_to_global(TAB_PAGE, j)];
            // (their names wider than a control's, as wide as the tabs' rows': "Button prompts" whole at any size)
            const float opt_x = rx0 + (rx1 - rx0) * 0.5f;
            name_cell(c, ImVec2(rx0, y), ImVec2(opt_x, y + rh), r.name, sel, restart_changed(r), 17);
            choice_cell(c, ImVec2(opt_x, y), ImVec2(rx1, y + rh), r.text(*r.value), sel, *r.value > r.lo, *r.value < r.hi);
            hits.add(rx0, y0, rx1, y1, menu_hit::kCtlOption, j, opt_x, rx1);
            continue;
        }
        if (i == kControls) {
            ui::button(c, ImVec2(rx0, y), ImVec2(pad_x - 8 * s, y + rh), "Reset to defaults", sel);
            hits.add(rx0, y0, pad_x - 8 * s, y1, menu_hit::kCtlRow, i);
            continue;
        }
        const Control& ctl = controls[i];
        name_cell(c, ImVec2(rx0, y), ImVec2(pad_x, y + rh), ctl.name, sel, false, 17);
        hits.add(rx0, y0, pad_x, y1, menu_hit::kCtlRow, i);
        for (int col = 0; col < 2; col++) {   // (two value cells, a gap between them, each drawn as the tabs' values are)
            const float a = col ? key_x + 3 * s : pad_x, b = col ? rx1 : key_x - 3 * s;
            std::string v = col == 0 ? (keys_only(i) ? "-" : pad_name(host->binding(HOS_BIND_PAD, ctl.button, 0))) : keys_text(ctl.button);
            const bool here = sel && col == ccol;
            const ImVec2 va(a, y), vb(b, y + rh);
            if (here && listening) {   // (waiting for the new input: amber, the outline as a selected value's)
                const float in = 4 * s;
                c.d->AddRectFilled(va, vb, ui::kCellDark);
                c.d->AddRect(va, vb, ui::kEdge, 0, std::max(1.0f, s));
                c.d->AddRectFilled(va + ImVec2(in, in), vb - ImVec2(in, in), ui::kListen);
                ui::text_mid_centre(c, c.f.strong, 16.5f * s, (a + b) * 0.5f, y + rh * 0.5f, ui::kInkBright,
                                    col == 0 ? "Press a button..." : "Press a key...");
                ui::glow(c, va, vb);
            } else {
                choice_cell(c, va, vb, v, here, false, false, col == 0 && keys_only(i));
            }
            if (!(col == 0 && keys_only(i))) hits.add(a, y0, b, y1, menu_hit::kCtlCell, i, 0, 0, 0, 0, col);
        }
    }
    c.d->PopClipRect();
    (void)(listening ? ui::prompt_bar(c, wait, 1) : sel_i < 0 ? ui::prompt_bar(c, option, 3) : ui::prompt_bar(c, idle, 5));   // (the prompts, on top of the help and the rows)
}

/// A small square page arrow at a (its size side): dir 0 back, 1 on; dim when it cannot go that way
void page_arrow(ui::Ctx& c, ImVec2 a, float side, int dir, bool can) {
    const float s = c.s;
    const ImVec2 b = a + ImVec2(side, side);
    c.d->AddRectFilled(a, b, can ? ui::kCellName : ui::alpha(ui::kCellName, 0.4f), 3 * s);
    c.d->AddRect(a, b, can ? ui::kEdge : ui::alpha(ui::kEdge, 0.4f), 3 * s, std::max(1.0f, s));
    ui::triangle(c, (a + b) * 0.5f, 10 * s, dir, can ? ui::kInkBright : ui::kDim);
}

/// A tag at the right end of a card (its right edge x, its middle y): the text on an accent plate; its width
float card_tag(ui::Ctx& c, float x, float y, const char* tag, bool draw = true) {
    const float s = c.s, tw = ui::size_of(c.f.strong, 13 * s, tag).x + 14 * s;
    if (draw) {
        const ImVec2 g(x - tw, y - 10 * s);
        c.d->AddRectFilled(g, g + ImVec2(tw, 20 * s), ui::kAccent, 2 * s);
        ui::text_mid(c, c.f.strong, 13 * s, g.x + 7 * s, y, ui::kGoldInk, tag);
    }
    return tw;
}

void draw_inventory(ui::Ctx& c) {
    const float s = c.s;
    const int begin = inventory_begin(), end = inventory_end();
    bool loaded = false;   // (a save's counts reported: -1 for all until the game side has a save to read)
    for (int i = 0; i < kItems && !loaded; ++i) loaded = item_counts[i].load(std::memory_order_relaxed) >= 0;
    char count_text[48];
    snprintf(count_text, sizeof count_text, "%d items", end - begin);
    const float hh = title_banner(c, "Inventory", count_text);
    const int have = item_counts[item_choice].load(std::memory_order_relaxed);
    const char* edit = item_kinds[item_choice] ? (inventory_amount ? "Done" : "Edit amount") : (have ? "Relock" : "Unlock");
    ui::Prompt prompts[5] = {{ui::kUpDown, "Move"}, {ui::kLeftRight, inventory_amount ? "Amount" : "Move"}, {ui::kTabs, "Section"}};
    int prompts_n = 3;
    if (have >= 0) prompts[prompts_n++] = {ui::kConfirm, edit};
    prompts[prompts_n++] = {ui::kBack, inventory_amount ? "Done" : "Back"};
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s;
    const float gap = 10 * s;
    // the sections, as tabs over the grid (Q and E, or a click)
    // (on one line: their padding, then their text, made smaller as far as needed for the column's width)
    float tpx = 16 * s, tpad = 26 * s, tgap = 8 * s;
    auto tabs_w = [&] {
        float w = 0;
        for (int i = 0; i <= kItemSections; ++i) w += ui::size_of(c.f.strong, tpx, i ? item_sections.s[i - 1].name : "All").x + tpad + tgap;
        return w - tgap;
    };
    if (tabs_w() > right - left) tpad = 16 * s, tgap = 6 * s;
    if (const float w = tabs_w(); w > right - left) {
        const float text = w - (kItemSections + 1) * tpad - kItemSections * tgap;
        tpx = std::max(12 * s, tpx * (right - left - (w - text)) / text);
    }
    float x = left, y = hh + 18 * s;
    for (int i = 0; i <= kItemSections; ++i) {
        const char* label = i ? item_sections.s[i - 1].name : "All";
        const float w = ui::size_of(c.f.strong, tpx, label).x + tpad;
        if (x > left && x + w > right) x = left, y += 36 * s;
        const ImVec2 a(x, y), b(std::min(x + w, right), y + 30 * s);
        const bool active = i == inventory_filter;
        if (active) ui::gold(c, a, b);
        else c.d->AddRectFilled(a, b, ui::kCellName, 4 * s);
        c.d->AddRect(a, b, active ? ui::kGoldHi : ui::kEdge, 4 * s, std::max(1.0f, s));
        ui::text_mid_centre(c, c.f.strong, tpx, (a.x + b.x) * .5f, y + 15 * s, active ? ui::kGoldInk : ui::kInkBright, label);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kRow, i);
        x += w + tgap;
    }
    float top = y + 44 * s;
    const float pitch = (loaded ? 62 : 46) * s;   // (the title screen: the names alone, no line under them)
    if (!loaded) {   // (the title screen: one note for the page, not "Load a save" on every card)
        ui::diamond(c, ImVec2(left + 5 * s, top - 2 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.body, 16 * s, left + 18 * s, top - 2 * s, ui::kSoft, "Load a save to edit the inventory.");
        top += 30 * s;
    }
    const float strip = 58 * s, detail = bar - 14 * s - strip, grid_bottom = std::max(top, detail - 8 * s);
    const char* help = have < 0 ? "Load a save to edit this item." : item_kinds[item_choice] ?
        "Choose Edit amount, then use Left and Right to change the quantity." : "Unlock or relock this item.";
    if (!narrow)
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * .30f), item_names[item_choice], help, nullptr, nullptr, 0, false,
                 hh + 18 * s, bar - 12 * s);
    // (as many columns as fit the longest name, at most 5; whole pages of cards, the range and the page under them)
    static float name_w = 0, name_s = 0;
    if (name_s != s) {
        name_s = s, name_w = 0;
        for (int i = 0; i < kItems; ++i) name_w = std::max(name_w, ui::size_of(c.f.strong, 15.5f * s, item_names[i]).x);
    }
    inventory_columns = std::clamp((int)((right - left + gap) / std::max(name_w + 26 * s + gap, 150 * s)), 1, 5);
    inventory_capacity = inventory_columns * std::max(1, (int)((grid_bottom - top + gap) / pitch));
    const int pages_n = std::max(1, (end - begin + inventory_capacity - 1) / inventory_capacity);
    inventory_page = std::clamp(inventory_page, 0, pages_n - 1);
    const int first = begin + inventory_page * inventory_capacity;
    const float width = (right - left + gap) / inventory_columns;
    c.d->PushClipRect(ImVec2(left - 12 * s, top - 12 * s), ImVec2(right + 12 * s, grid_bottom + 12 * s));
    for (int i = first; i < std::min(end, first + inventory_capacity); ++i) {
        const int cell = i - first;
        const ImVec2 a(left + (cell % inventory_columns) * width, top + (cell / inventory_columns) * pitch);
        const ImVec2 b(a.x + width - gap, a.y + pitch - gap);
        const int count = item_counts[i].load(std::memory_order_relaxed);
        const bool selected = i == item_choice, owned = count > 0, editing = selected && inventory_amount;
        c.d->AddRectFilled(a, b, owned ? ui::kCellNameSel : ui::kCellName, 4 * s);
        c.d->AddRect(a, b, owned ? ui::kAccent : ui::alpha(ui::kEdge, count < 0 ? 0.5f : 1.0f), 4 * s, std::max(1.0f, s));
        if (owned) c.d->AddRectFilled(a + ImVec2(0, 4 * s), ImVec2(a.x + 3 * s, b.y - 4 * s), ui::kAccent);
        c.d->PushClipRect(a + ImVec2(8 * s, 0), b - ImVec2(8 * s, 0), true);
        ui::text_mid(c, c.f.strong, 15.5f * s, a.x + 13 * s, loaded ? a.y + 17 * s : (a.y + b.y) * 0.5f, count < 0 ? ui::kDim : ui::kInkBright, item_names[i]);
        char amount[40];
        if (count < 0) snprintf(amount, sizeof amount, "%s", loaded ? "Unavailable" : "");
        else if (item_kinds[i]) snprintf(amount, sizeof amount, "Quantity %d", count);
        else snprintf(amount, sizeof amount, "%s", count ? "Unlocked" : "Locked");
        const float ly = a.y + 37 * s;
        if (editing) {   // (the amount being changed: its arrows, gold)
            ui::triangle(c, ImVec2(a.x + 18 * s, ly), 8 * s, 0, ui::kGoldHi);
            ui::text_mid(c, c.f.strong, 15 * s, a.x + 28 * s, ly, ui::kGoldHi, amount);
            ui::triangle(c, ImVec2(a.x + 38 * s + ui::size_of(c.f.strong, 15 * s, amount).x, ly), 8 * s, 1, ui::kGoldHi);
        } else {
            ui::text_mid(c, c.f.body, 15 * s, a.x + 13 * s, ly, owned ? ui::kAccent : ui::kDim, amount);
        }
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        if (a.y < grid_bottom) hits.add(a.x, a.y, b.x, std::min(b.y, grid_bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the page: its arrows, Page n of m and the range; the selected item's action at the right)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const int pages = (end - begin + inventory_capacity - 1) / inventory_capacity, at = (first - begin) / inventory_capacity;
        const float my = detail + strip * 0.5f + 2 * s, side = 32 * s;
        float px0 = left;
        if (pages > 1) {
            page_arrow(c, ImVec2(px0, my - side * 0.5f), side, 0, at > 0);
            hits.add(px0, my - side * 0.5f, px0 + side, my + side * 0.5f, menu_hit::kButton, 2);
            char page_text[32];
            snprintf(page_text, sizeof page_text, "Page %d of %d", at + 1, pages);
            const float tw = ui::size_of(c.f.strong, 17 * s, page_text).x;
            ui::text_mid(c, c.f.strong, 17 * s, px0 + side + 12 * s, my, ui::kInkBright, page_text);
            const float nx = px0 + side + 24 * s + tw;
            page_arrow(c, ImVec2(nx, my - side * 0.5f), side, 1, at + 1 < pages);
            hits.add(nx, my - side * 0.5f, nx + side, my + side * 0.5f, menu_hit::kButton, 3);
            px0 = nx + side + 18 * s;
        }
        char range[64];
        snprintf(range, sizeof range, "%d–%d of %d", first - begin + 1, std::min(end, first + inventory_capacity) - begin, end - begin);
        ui::text_mid(c, c.f.body, 15 * s, px0, my, ui::kSoft, range);
        if (have >= 0) {
            const float bw = ui::size_of(c.f.strong, 18 * s, "Edit amount").x + 36 * s;
            const ImVec2 a(right - bw, my - 17 * s), b(right, my + 17 * s);
            ui::button(c, a, b, edit, false);
            hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
            if (inventory_amount) {
                char amount[32];
                snprintf(amount, sizeof amount, "%d", have);
                const ImVec2 av(a.x - 118 * s, a.y), bv(a.x - 10 * s, b.y);
                choice_cell(c, av, bv, amount, true, true, true);
                hits.add(av.x, av.y, bv.x, bv.y, menu_hit::kButton, 1, av.x, bv.x);
            }
        }
    }
    ui::prompt_bar(c, prompts, prompts_n);   // (the prompts, on top of the help and the cards)
}

void draw_teleport(ui::Ctx& c) {
    const float s = c.s;
    char count[32];
    snprintf(count, sizeof count, "%d of %d", teleport_choice + 1, kTeleports);
    const float hh = title_banner(c, "Teleport", count);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const ui::Prompt prompts[] = {{ui::kUpDown, "Move"}, {ui::kLeftRight, "Move"}, {ui::kTabs, "Section"},
        {ui::kConfirm, "Travel"}, {ui::kBack, "Back"}};
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s, top = hh + 16 * s;
    const float strip = 58 * s, detail = bar - 14 * s - strip;
    const float bottom = std::max(top, detail - 6 * s), gap = 10 * s, pitch = 48 * s, head = 40 * s;
    const char* const note = teleport_notes[teleport_choice];
    if (!narrow)
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * .30f), teleport_names[teleport_choice],
                 "Select a destination, then Travel (or Enter) to go to its map entrance. Link keeps his current age.", note, nullptr, 0,
                 false, hh + 18 * s, bar - 12 * s);
    // (as many columns as fit the longest name and its tag, at most 4; each section under its heading, the cards scrolled to
    // keep the selected one in view)
    static float card_w = 0, card_s = 0;
    if (card_s != s) {
        card_s = s, card_w = 0;
        for (int i = 0; i < kTeleports; ++i)
            card_w = std::max(card_w, ui::size_of(c.f.body, 17 * s, teleport_names[i]).x +
                                          (teleport_notes[i] ? card_tag(c, 0, 0, teleport_notes[i], false) + 12 * s : 0));
    }
    teleport_columns = std::clamp((int)((right - left + gap) / (card_w + 36 * s + gap)), 1, 4);
    const int cols = teleport_columns;
    const float width = (right - left + gap) / cols;
    // (the y of each section's heading and first row, from the top of the list)
    float section_y[kTeleportSections + 1];
    float total = 0;
    for (int k = 0; k < kTeleportSections; ++k) {
        section_y[k] = total;
        const int n = teleport_section_end(k) - teleport_sections[k].first;
        total += head + (n + cols - 1) / cols * pitch + (k + 1 < kTeleportSections ? 10 * s : 0);
    }
    section_y[kTeleportSections] = total;
    auto card_y = [&](int i) {
        const int k = teleport_section_of(i);
        return section_y[k] + head + (i - teleport_sections[k].first) / cols * pitch;
    };
    const float view = bottom - top;
    teleport_page_rows = std::max(1, (int)(view / pitch) - 1);
    float& at = teleport_scroll_px;
    float& want = teleport_want_px;
    if (teleport_wheel) want -= teleport_wheel * pitch, teleport_wheel = 0;   // (the wheel: the cards move, the choice stays)
    if (total <= view) want = 0;
    else if (!teleport_follow) want = std::clamp(want, 0.0f, total - view);
    else {
        const int k = teleport_section_of(teleport_choice);
        const bool first_row = teleport_choice - teleport_sections[k].first < cols;
        const float y0 = first_row ? section_y[k] : card_y(teleport_choice) - pitch * 0.5f, y1 = card_y(teleport_choice) + pitch * 1.5f;
        if (y0 < want) want = y0;
        if (y1 > want + view) want = y1 - view;
        want = std::clamp(want, 0.0f, total - view);
    }
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.5f) at = want;
    if (total > view) scroll_bar(c, cx0 + 12 * s, top, bottom, (int)total, (int)view, at);
    c.d->PushClipRect(ImVec2(left - 12 * s, top), ImVec2(right + 12 * s, bottom), true);
    for (int k = 0; k < kTeleportSections; ++k) {
        const float hy = top + section_y[k] - at + head * 0.5f;
        if (hy > bottom + head || hy < top - head) continue;
        ui::text_mid(c, c.f.heading, 20 * s, left, hy, ui::kInkBright, teleport_sections[k].name);
        const float tw = ui::size_of(c.f.heading, 20 * s, teleport_sections[k].name).x;
        char n_text[16];
        snprintf(n_text, sizeof n_text, "%d", teleport_section_end(k) - teleport_sections[k].first);
        const float nw = ui::size_of(c.f.body, 15 * s, n_text).x;
        ui::text_mid(c, c.f.body, 15 * s, right - nw, hy, ui::kSoft, n_text);
        c.d->AddLine(ImVec2(left + tw + 14 * s, hy + s), ImVec2(right - nw - 12 * s, hy + s), ui::alpha(ui::kAccent, 0.45f),
                     std::max(1.0f, s));
    }
    for (int i = 0; i < kTeleports; ++i) {
        const int k = teleport_section_of(i), cell = i - teleport_sections[k].first;
        const ImVec2 a(left + cell % cols * width, top + card_y(i) - at);
        const ImVec2 b(a.x + width - gap, a.y + pitch - 8 * s);
        if (b.y < top || a.y > bottom) continue;
        const bool selected = i == teleport_choice;
        c.d->AddRectFilled(a, b, selected ? ui::kCellNameSel : ui::kCellName, 3 * s);
        c.d->AddRect(a, b, ui::kEdge, 3 * s, std::max(1.0f, s));
        if (selected) c.d->AddRectFilled(a, ImVec2(a.x + 4 * s, b.y), ui::kGoldHi);
        float name_end = b.x - 10 * s;
        if (teleport_notes[i]) name_end -= card_tag(c, b.x - 8 * s, (a.y + b.y) * 0.5f, teleport_notes[i]) + 8 * s;
        c.d->PushClipRect(a, ImVec2(name_end, b.y), true);
        ui::text_mid(c, selected ? c.f.strong : c.f.body, 17 * s, a.x + 16 * s, (a.y + b.y) * 0.5f, selected ? ui::kInkBright : ui::kInk,
                     teleport_names[i]);
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        hits.add(a.x, std::max(a.y, top), b.x, std::min(b.y, bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the selection, its section and note, and Travel: what Enter does)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const float my = detail + strip * 0.5f + 2 * s;
        const float bw = ui::size_of(c.f.strong, 20 * s, "Travel").x + 56 * s;
        const ImVec2 a(right - bw, my - 18 * s), b(right, my + 18 * s);
        c.d->PushClipRect(ImVec2(left, detail), ImVec2(a.x - 12 * s, detail + strip), true);
        ui::text_mid(c, c.f.strong, 18 * s, left, my - 10 * s, ui::kInkBright, teleport_names[teleport_choice]);
        char where[64];
        snprintf(where, sizeof where, note ? "%s · %s" : "%s", teleport_sections[teleport_section_of(teleport_choice)].name, note);
        ui::text_mid(c, c.f.body, 15 * s, left, my + 12 * s, ui::kAccent, where);
        c.d->PopClipRect();
        ui::button(c, a, b, "Travel", false);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
    }
    ui::prompt_bar(c, prompts, 5);   // (the prompts, on top of the help and the cards)
}

void draw_ask(ui::Ctx& c) {
    const float s = c.s, W = 560 * s, H = 232 * s;
    const ImVec2 p((c.w - W) * 0.5f, (c.h - H) * 0.5f), q = p + ImVec2(W, H);
    ui::shadow(c, p, q, 30 * s, 1.0f, 8 * s);
    c.d->AddRectFilled(p, q, ui::rgba(36, 28, 19, 250));
    c.d->AddRect(p, q, ui::rgba(122, 98, 64, 230), 0, 2 * s);
    const float hb = 50 * s;   // (a tan title strip, as the banner, its title in dark ink)
    c.d->AddRectFilledMultiColor(p, ImVec2(q.x, p.y + hb), ui::kBannerTop, ui::kBannerTop, ui::kBanner, ui::kBanner);
    c.d->AddRectFilled(ImVec2(p.x, p.y + hb - 3 * s), ImVec2(q.x, p.y + hb), ui::kBannerEdge);
    ui::text_mid_centre(c, c.f.heading, 23 * s, (p.x + q.x) * 0.5f, p.y + hb * 0.5f, ui::kBannerInk, "Restart Needed");
    const std::string ask = "Restart to apply " + restart_list(" and ") + "?";
    ui::para(c, c.f.body, 19 * s, ImVec2(p.x + 28 * s, p.y + 64 * s), ui::kInk, ask.c_str(), W - 56 * s);
    ui::text_mid(c, c.f.body, 16 * s, p.x + 28 * s, p.y + 124 * s, ui::kSoft, "Progress since your last save is lost.");
    const float bw = 210 * s, bh = 46 * s, gap = 24 * s, by = p.y + 160 * s, x0 = (p.x + q.x) * 0.5f - bw - gap * 0.5f;
    const char* labels[2] = {"Later", "Restart Now"};
    for (int i = 0; i < 2; i++) {
        const ImVec2 a(x0 + i * (bw + gap), by), b = a + ImVec2(bw, bh);
        ui::button(c, a, b, labels[i], i == ask_sel);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, i);
    }
    const ui::Prompt prompt_list[] = {{ui::kLeftRight, "Choose"}, {ui::kConfirm, "Confirm"}, {ui::kBack, "Later"}};
    ui::prompt_bar(c, prompt_list, 3);
}

void draw_menu(ui::Ctx& c);
/// The menu's text sizes at this window's unit, baked ahead by vk_imgui.cpp a few a frame: the first open then draws with no
/// glyphs to make (it took 2.6 ms at 1080p)
float prebaked[2] = {0, 0};   // (the unit and the width prebake last did)
/// The menu's unit now for a window w x h: whether prebake has not done it yet
bool prebake_needed(uint32_t w, uint32_t h) {
    float m = 0;
    return ui::unit((float)h, opts.menu_size, &m) != prebaked[0] || (float)w != prebaked[1];
}
void prebake(const ui::Ctx& c) {
    if (c.s == prebaked[0] && c.w == prebaked[1]) return;
    prebaked[0] = c.s, prebaked[1] = c.w;
    static const float points[] = {12, 12.5f, 13, 13.6f, 14, 14.28f, 14.5f, 15, 15.5f, 16, 16.5f, 17, 18, 18.5f, 19, 20, 21, 23, 24};
    constexpr int n = sizeof points / sizeof points[0];
    float px[n];
    for (int i = 0; i < n; i++) px[i] = points[i] * c.s;
    ui::prebake(px, n);
    if (open_now.load(std::memory_order_relaxed)) return;
    // (and the page itself drawn once into a list of its own, thrown away: what the first open would measure and bake (the
    // help's height over every row, the exact sizes) done now, the menu's state as it was)
    const menu_hit::Map saved_hits = hits;
    const int saved_marquee = marquee_max;
    const int saved_tele[2] = {teleport_columns, teleport_page_rows}, saved_inv[3] = {inventory_columns, inventory_capacity, inventory_page};
    const float saved_tele_px[2] = {teleport_scroll_px, teleport_want_px};
    const float saved_ease = ease_step;
    float saved_scroll[sizeof scroll_at / sizeof *scroll_at], saved_want[sizeof scroll_at / sizeof *scroll_at];
    int saved_to[sizeof scroll_at / sizeof *scroll_at];   // (the scratch drawing's scroll, given back: the next is the menu's)
    memcpy(saved_scroll, scroll_at, sizeof scroll_at);
    memcpy(saved_want, scroll_want, sizeof scroll_want);
    memcpy(saved_to, scrolled_to, sizeof scrolled_to);
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch._ResetForNewFrame();
    scratch.PushTexture(ImGui::GetIO().Fonts->TexRef);
    scratch.PushClipRectFullScreen();
    ease_step = 0;
    {
        ui::Ctx dry = c;
        dry.d = &scratch;
        draw_menu(dry);
    }
    hits = saved_hits, marquee_max = saved_marquee, ease_step = saved_ease;
    teleport_columns = saved_tele[0], teleport_page_rows = saved_tele[1], teleport_scroll_px = saved_tele_px[0], teleport_want_px = saved_tele_px[1];
    inventory_columns = saved_inv[0], inventory_capacity = saved_inv[1], inventory_page = saved_inv[2];
    memcpy(scroll_at, saved_scroll, sizeof scroll_at);
    memcpy(scroll_want, saved_want, sizeof scroll_want);
    memcpy(scrolled_to, saved_to, sizeof scrolled_to);
}

float menu_ox = 0;   // (the menu's column's left edge in the window: more than 0 only on a window wider than 16:9)
/// The menu: the game dimmed across the whole window, the pages in a column as wide as a 16:9 screen of this height, in the
/// middle (a window of 16:9 or narrower: the whole of it). The pages draw from 0 as on 16:9; their vertices and clip
/// rectangles are moved over after, and menu_tick moves the mouse back by menu_ox before it tests the hits
void draw_menu(ui::Ctx& c) {
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, ui::kDimAlpha));   // (the game behind, dimmed)
    ui::Ctx k = c;
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    menu_ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size, cmd0 = c.d->CmdBuffer.Size;
    hits.clear(0, 0, k.w, k.h);
    if (page == 5) draw_ask(k);
    else if (page == 1) draw_controls(k);
    else if (page == 3) draw_teleport(k);
    else if (page == 4) draw_inventory(k);
    else draw_options(k);
    if (menu_ox <= 0) return;
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) c.d->VtxBuffer[i].pos.x += menu_ox;
    // (a clip rectangle of the column's: moved with it; the whole window's (the dim's, and the draw list's own) stays, as it
    // holds the column wherever it is)
    for (int i = std::max(0, cmd0 - 1); i < c.d->CmdBuffer.Size; i++) {
        ImVec4& r = c.d->CmdBuffer[i].ClipRect;
        if (r.x <= 0 && r.z >= c.w) continue;
        r.x += menu_ox, r.z += menu_ox;
    }
    // (the draw list's current header, for whatever is drawn next: the window's again after the pages' pops)
}

/// Text with a dark outline (each side, one step of the scale), as the counter always had: readable over any scene
void outlined_right(ui::Ctx& c, ImFont* font, float px, float right, float top, ImU32 col, const char* t) {
    const float x = right - ui::size_of(font, px, t).x, o = std::max(1.0f, c.s);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) c.d->AddText(font, px, ImVec2(x + dx * o, top + dy * o), ui::rgba(0, 0, 0, 230), t);
    c.d->AddText(font, px, ImVec2(x, top), col, t);
}

/// The FPS counter in the safe area's top right corner (under the banner while the menu is open): green outlined text, the
/// number large and "FPS" beside it small, the frame generation line under them, and no plate. (hookshot) As the counter
/// looked before the menu was drawn with Dear ImGui (v2.0.14's paint_fps: RGB 72 255 110, bold 28 and 13 px a scale step);
/// the plate and the warm colours that came with the new menu were not wanted for it.
void draw_fps(ui::Ctx& c, bool under_banner) {
    const float s = c.s, big = 28 * s, small = 13 * s;
    const ImU32 green = ui::rgba(72, 255, 110);
    const float nw = ui::size_of(c.f.strong, big, fps_text).x, lw = ui::size_of(c.f.strong, small, "FPS").x;
    const float sw = fps_sub[0] ? ui::size_of(c.f.strong, small, fps_sub).x : 0;
    const float bw = std::max(nw + 4 * s + lw, sw);
    // (the window's top right corner, where it is in play; under the banner while the menu is open, unless the window is so
    // much wider than 16:9 that the counter fits beside the centred column's banner, in the corner)
    const bool beside = menu_ox >= bw + 2 * c.safe;
    const float right = c.w - c.safe, top = under_banner && page != 5 && !beside ? banner_h(c) + 10 * s : c.safe;
    outlined_right(c, c.f.strong, small, right, top + 14 * s, green, "FPS");
    outlined_right(c, c.f.strong, big, right - lw - 4 * s, top, green, fps_text);
    if (fps_sub[0]) outlined_right(c, c.f.strong, small, right, top + 32 * s, ui::rgba(226, 232, 226), fps_sub);
}

/// The start-up screen: the logo, the wordmark, what the start is doing, a bar, on a warm dark. A little below the middle; what
/// is under the logo at 0.86 of the logo's scale. startup_fade, 0 to 1, darkens it all to black as it ends
void draw_startup(ui::Ctx& c) {
    StartupView v;
    {
        std::lock_guard lk(startup_mu);
        v = startup;
    }
    const float s = c.s, w = c.w, h = c.h, cx = w * 0.5f, k = s * 0.86f;   // (k: the scale of what is under the logo)
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(14, 10, 6));
    const float lh = 112 * s, top = h * 0.5f - 176 * s;
    for (int i = 40; i >= 1; i--) c.d->AddCircleFilled(ImVec2(cx, top + lh * 0.5f + 70 * s), h * 0.021f * i, ui::rgba(46, 33, 18, 4), 128);
    float aspect = 1;
    ImTextureData* logo = ui::logo(&aspect);
    if (logo) c.d->AddImage(logo->GetTexRef(), ImVec2(cx - lh * aspect * 0.5f, top), ImVec2(cx + lh * aspect * 0.5f, top + lh));
    float y = top + lh + 16 * s;
    ui::spaced(c, c.f.heading, 42 * k, ImVec2(cx, y), ui::rgba(246, 232, 204), "HOOKSHOT", 6 * k);
    ui::flourish(c, cx - 190 * k, cx + 190 * k, y + 66 * k, ui::kAccent);
    ui::text_centre(c, c.f.body, 14.5f * k, ImVec2(cx, y + 78 * k), ui::kDim, v.version.c_str());
    if (!v.title.empty()) {
        y += 120 * k;
        if (!v.step.empty()) {   // ("STEP 2 OF 2" as "Step 2 of 2")
            std::string step = v.step;
            for (size_t i = 1; i < step.size(); i++) step[i] = (char)tolower((unsigned char)step[i]);
            ui::text_centre(c, c.f.body, 15 * k, ImVec2(cx, y), ui::kAccent, step.c_str());
        }
        ui::text_centre(c, c.f.heading, 24 * k, ImVec2(cx, y + 20 * k), ui::kInk, v.title.c_str());
        const float x0 = cx - 270 * k, x1 = cx + 270 * k, by = y + 64 * k;
        float under = by;
        if (v.fraction >= 0) {   // (the bar as a value cell: a dark cell, gold inside, the percentage on it)
            const float bh = 26 * k, in = 4 * k, fx = x0 + in + (x1 - x0 - 2 * in) * (float)std::min(v.fraction, 1.0);
            c.d->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kCellDark);
            c.d->AddRect(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kEdge, 0, std::max(1.0f, s));
            if (fx > x0 + in) ui::gold(c, ImVec2(x0 + in, by + in), ImVec2(fx, by + bh - in));
            // (the percentage in the middle of the whole bar, drawn twice: dark ink where the gold is under it, light where the
            // empty bar is, each clipped to its part, so every letter keeps its contrast as the edge passes through it)
            const float split = std::max(fx, x0 + in);
            c.d->PushClipRect(ImVec2(x0, by), ImVec2(split, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kGoldInk, v.pct.c_str());
            c.d->PopClipRect();
            c.d->PushClipRect(ImVec2(split, by), ImVec2(x1, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kInk, v.pct.c_str());
            c.d->PopClipRect();
            under = by + bh;
        }
        const float px = 16.5f * k;
        ui::text(c, c.f.body, px, ImVec2(v.fraction >= 0 ? x0 : cx - ui::size_of(c.f.body, px, v.left.c_str()).x * 0.5f, under + 12 * k),
                 ui::kSoft, v.left.c_str());
        if (!v.right.empty())
            ui::text(c, c.f.body, px, ImVec2(x1 - ui::size_of(c.f.body, px, v.right.c_str()).x, under + 12 * k), ui::kSoft, v.right.c_str());
        if (!v.hint.empty()) ui::text_centre(c, c.f.body, 15.5f * k, ImVec2(cx, by + 92 * k), ui::kDim, v.hint.c_str());
    }
    if (const float f = startup_fade.load(std::memory_order_relaxed); f > 0)   // (the end: to black, the game's first frame next)
        c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(0, 0, 0, (int)(std::min(f, 1.0f) * 255 + 0.5f)));
}

} // namespace

void startup_fade_to(float f) { startup_fade.store(f, std::memory_order_relaxed); }

// (hookshot, #1886) The Upscaler row's one-time questions: NVIDIA's NGX started (dlss_probe), AMD's FSR 4 loaded
// (fsr4_probe), FSR 3.1's GPU check. They were asked at the menu's first open, on the frame's thread, and NGX's start
// alone held that frame about 3 s (an RTX 5080 at 4K; 2.9 s here): the game stood still and its sound ran on, out of
// step with a cutscene. Now asked once on a thread of their own while the start-up screen shows, where nobody waits on
// them; each is locked and asked once (ngx_init, fsr4's load, fsr3::supported's static), so the menu's own calls at its
// open return at once, or wait for the rest of the one under way.
static void probe_upscalers_soon() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!dev.device) return;
        std::thread([] {
            const auto t0 = std::chrono::steady_clock::now();
            dlss_probe();
            fsr4_probe();
            (void)fsr3::supported();
            logf("menu: the upscalers asked in the background while the start-up screen shows (%.0f ms)",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }).detach();
    });
}

void startup_screen(const StartupView* view) {
    if (!view) {
        startup_on.store(false, std::memory_order_release);
        startup_fade.store(0, std::memory_order_relaxed);
        return;
    }
    StartupView v = *view;
    if (v.version.empty()) v.version = version_text(HOOKSHOT_VERSION);   // ("v2.0.5"; "v2.0.5 dev" between releases)
    {
        std::lock_guard lk(startup_mu);
        startup = v;
    }
    startup_on.store(true, std::memory_order_release);
    probe_upscalers_soon();
}

void menu_init() {
    static bool done;
    if (done) return;
    done = true;
    for (auto& count : item_counts) count.store(-1, std::memory_order_relaxed);
    bool has_options = false;
    const char* cheats = getenv("HOS_OOT_CHEATS");
    cheats_env = cheats && !strcmp(cheats, "1");
    if (cheats_env) {
        const char* hearts = getenv("HOS_OOT_PROTECT_HEARTS");
        protect_hearts.store(hearts && !strcmp(hearts, "1"));
        const char* ammo = getenv("HOS_OOT_INFINITE_AMMO");   // (as HOS_OOT_PROTECT_HEARTS: the row starts on)
        infinite_ammo.store(ammo && !strcmp(ammo, "1"));
        const char* moon = getenv("HOS_OOT_MOON_JUMP");
        moon_jump.store(moon && !strcmp(moon, "1"));
        if (const char* lock = getenv("HOS_OOT_LOCK_TIME")) clock_hour = std::clamp(atoi(lock), 0, 23), time_lock.store(1);   // (the hour)
    }
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        has_options = true;
        char line[80];
        bool has_resolution = false;
        bool v2 = false;
        int version = 0;
        int old_dynres = -1;   // (the row before Resolution: 0, the full scale, is its 100%; 1 is Auto)
        bool menu_auto = false;   // (menu_size=0, the Menu size Auto there was before: Medium, not the Small the row's clamp gives)
        while (fgets(line, sizeof line, f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (Row& r : rows) {
                if (!r.value || strcmp(line, ini_key(r))) continue;
                const int v = atoi(eq + 1);
                *r.value = r.value == &opts.output_res ? parse_output(eq + 1) : std::min(std::max(v, r.lo), r.hi);
            }
            has_resolution |= !strcmp(line, "resolution");
            v2 |= !strcmp(line, "options_version");
            if (!strcmp(line, "options_version")) version = atoi(eq + 1);
            if (!strcmp(line, "dynamic_resolution")) old_dynres = atoi(eq + 1);
            if (!strcmp(line, "menu_size") && atoi(eq + 1) == 0) menu_auto = true;
            if (!strcmp(line, "microphone_device")) {   // (hookshot: the microphone, by its name)
                mic_want = mic_key(eq + 1);
                while (!mic_want.empty() && (mic_want.back() == '\n' || mic_want.back() == '\r')) mic_want.pop_back();
            }
        }
        fclose(f);
        // (the saved microphone by its name, not its place; the host told the row's choice. Off or the default: no list, no SDL
        // audio at the start)
        if (!mic_want.empty()) refresh_mics();
        else {
            if (opts.mic >= 2) opts.mic = 1;
            mic_tell_host();
        }
        if (!has_resolution && old_dynres >= 0) opts.resolution = old_dynres ? 0 : 1;
        if (menu_auto) opts.menu_size = 2;
        opts.rumble = rumble_step(opts.rumble);   // (a hand-edited 60: 50, on the arrows' steps)
        // (Game speed is Real time by default now: Per frame played at half speed at 30 fps, the "slow motion" players
        // reported. An options file from before kept the old default whether chosen or not: Real time for it too)
        if (!v2) opts.realtime = 1;
        // (Descriptor reuse is on by default from options_version 4: a file from before wrote its 0 whether chosen or not, as
        // the off default it then was; fewer stutters and a better 1% low in the field, the flash checks clean, 2026-11-10)
        if (version < 4) opts.descriptor_reuse = 1;
    }
    // An opt-in profile for 4 GB cards: keep existing options authoritative, but make a first run usable on
    // low-VRAM hardware without requiring the menu to render first.
    if (!has_options && getenv("RX_VK_LOW_SPEC") && atoi(getenv("RX_VK_LOW_SPEC")) != 0) {
        apply_preset(1);
        opts.upscale = 0;
        opts.sharpness = 0;
    }
    if (opts.preset > 0) sync_preset_budgets(opts.preset);
    if (const char* e = getenv("RX_VK_FRAMEGEN_MULT")) opts.framegen = std::min(std::max(atoi(e), 2), 8);
    if (const char* e = getenv("RX_VK_PACE")) opts.pace = atoi(e) != 0;
    if (const char* e = getenv("RX_VK_UPSCALE"))
        for (Row& r : rows)
            if (r.value == &opts.upscale) *r.value = std::min(std::max(atoi(e), r.lo), r.hi);
    if (const char* e = getenv("HOS_REALTIME_STEP")) opts.realtime = atoi(e) != 0;
    if (const char* e = getenv("HOS_DYNRES")) opts.resolution = atoi(e) != 0 ? 0 : 1;
    if (const char* e = getenv("HOS_OUTPUT_RES")) opts.output_res = parse_output(e);
    restart_init();   // (what this run started with: the rows that take a restart differ from it once changed)
}

bool resolution_auto() { return opts.resolution == 0; }
int resolution_scale() {
    const float s = upscaler_rows::percent(opts.resolution) / 100.0f;
    return s > 0 ? (int)(twelfths(s) * 10000.0f / 12.0f + 0.5f) : 0;   // (the twelfth game_size labels)
}

void menu_set_gpu_count(int count) {
    for (Row& r : rows) {
        if (r.value == &opts.gpu_device) {
            r.hi = count;
            if (opts.gpu_device > count) opts.gpu_device = 0;
        }
    }
    restart_init();
    for (int i = 0; i < kRows; i++)   // (the row's value the game started with, the cards counted)
        if (rows[i].value == &opts.gpu_device) restart_state.restart_value(restart_slot[i], opts.gpu_device);
}

bool menu_is_open() { return open_now.load(std::memory_order_relaxed); }
// (the tools off on the Debug tab: flight, the free camera and heart protection put away; -1, heart protection never set, is the
// host's own HOS_OOT_PROTECT_HEARTS)
int menu_heart_protection() {
    const int v = protect_hearts.load(std::memory_order_relaxed);
    return cheats_on() || v < 0 ? v : 0;
}
int menu_player_flight() { return cheats_on() ? player_flight.load(std::memory_order_relaxed) : -1; }
int menu_free_camera() { return cheats_on() ? free_camera.load(std::memory_order_relaxed) : 0; }
int menu_link_speed() { return cheats_on() ? link_speeds[std::clamp(link_speed.load(std::memory_order_relaxed), 0, kLinkSpeeds - 1)] : 100; }
int menu_take_cheat_action() { return cheat_action.exchange(0, std::memory_order_relaxed); }

void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h) {
    const auto c = [](uint32_t v) { return (uint64_t)std::min(v, 0xffffu); };
    present_sizes.store(c(picture_w) << 48 | c(picture_h) << 32 | c(window_w) << 16 | c(window_h), std::memory_order_relaxed);
}
void menu_note_frame(bool game_frame, int shown) {
    if (shown) shown_frames.fetch_add(shown, std::memory_order_relaxed);
    if (game_frame) game_frames.fetch_add(1, std::memory_order_relaxed);
}

// The Fullscreen row and F11 are one setting: the row asks the window's thread (host/display.c), F11 shows in the row
void fullscreen_tick() {
    static int applied = 0, settle = 0;
    const HWND main_window = app_window();
    if (!main_window) return;
    static const bool env = getenv("HOS_FULLSCREEN") != nullptr;   // (a test's own choice: the row is left alone)
    if (env) return;
    if (opts.fullscreen != applied) {
        applied = opts.fullscreen;
        settle = 60;
        PostMessageA(main_window, WM_APP + 7, (WPARAM)applied, 0);
        return;
    }
    if (settle > 0) { settle--; return; }
    const int now = !(GetWindowLongA(main_window, GWL_STYLE) & WS_CAPTION);
    if (now != applied) {
        opts.fullscreen = applied = now;
        menu_refresh = true;
        save();
    }
}

/// A rate over the half-second that just ended. The first call only starts that interval: the frames since launch,
/// divided by one second, were shown as the rate the moment the counter was turned on.
bool fps_take(int64_t now, int64_t shown, int64_t game, int64_t* next, int64_t* last_shown, int64_t* last_game,
              double* shown_rate, double* game_rate) {
    if (now < *next) return false;
    const bool have = *next != 0;
    if (have) {
        const double secs = (now - *next + 500000000) / 1e9;
        *shown_rate = (shown - *last_shown) / secs;
        *game_rate = (game - *last_game) / secs;
    }
    *last_shown = shown;
    *last_game = game;
    *next = now + 500000000;
    return have;
}

void menu_option_set(int id, int value) {
    if (id == OOT_CHEAT_AGE_ID) {   // (Link's age: the Switch age row shows it)
        if (link_age.exchange(value, std::memory_order_relaxed) != value && page == 0 && cur_tab == TAB_DEBUG) menu_refresh = true;
        return;
    }
    if (id >= OOT_CHEAT_ITEM_BASE && id < OOT_CHEAT_ITEM_BASE + kItems) {   // (the game side's inventory counts)
        if (item_counts[id - OOT_CHEAT_ITEM_BASE].exchange(value, std::memory_order_relaxed) != value && page == 4)
            menu_refresh = true;
        return;
    }
    // (the game side's Ctrl+number shortcuts: any value switches the row)
    if (id == HOS_OPT_CHEAT_HEARTS || id == HOS_OPT_CHEAT_AMMO || id == HOS_OPT_CHEAT_TIMELOCK || id == HOS_OPT_CHEAT_MOONJUMP) {
        if (!cheats_on()) return;
        std::atomic<int>& row = id == HOS_OPT_CHEAT_HEARTS ? protect_hearts : id == HOS_OPT_CHEAT_AMMO ? infinite_ammo
                              : id == HOS_OPT_CHEAT_TIMELOCK ? time_lock : moon_jump;
        row.store(row.load() == 1 ? 0 : 1);
        menu_refresh = true;
        return;
    }
    if (id == HOS_OPT_CHEAT_FLIGHT) {
        if (cheats_on()) {
            int expected = value ? 0 : 1;
            player_flight.compare_exchange_strong(expected, value != 0, std::memory_order_relaxed);
        }
        return;
    }
    if (id != 7) return;
    opts.mouse_sens = std::min(std::max(value, 10), 500);   // (the keys work while the menu is closed: it shows it when opened)
    save();
}

/// (hookshot) the mouse on the inventory: a section tab chooses it, a card the item (a click selects it, it does not edit),
/// the page arrows turn the page with the choice, the action button and the amount's halves act; the wheel turns the page
/// shown and leaves the choice where it is; a right click goes back
void inventory_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kRow && hit.index >= 0 && hit.index <= kItemSections) choose_inventory_filter(hit.index), changed = true;
        else if (hit.part == menu_hit::kListLine && hit.index >= inventory_begin() && hit.index < inventory_end())
            item_choice = hit.index, inventory_amount = false, changed = true;
        else if (hit.part == menu_hit::kButton && hit.index == 0) press[7] = true;
        else if (hit.part == menu_hit::kButton && hit.index == 1 && hit.side) press[hit.side < 0 ? 3 : 4] = true;
        else if (hit.part == menu_hit::kButton && (hit.index == 2 || hit.index == 3))   // (the page arrows)
            press[hit.index == 2 ? 13 : 14] = true, inventory_amount = false;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) {
        const int was = inventory_page;
        inventory_page = std::max(0, inventory_page - ptr.wheel);   // (drawing keeps it to the pages there are)
        changed |= inventory_page != was;
    }
}
/// (hookshot) the mouse on the teleport list: a click on a card selects it (Travel, or confirm, goes), the Travel button
/// goes; the wheel scrolls the cards and leaves the choice where it is; a right click goes back
void teleport_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kListLine && hit.index >= 0 && hit.index < kTeleports)
            teleport_choice = hit.index, teleport_follow = true, changed = true;
        else if (hit.part == menu_hit::kButton) press[7] = true;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) teleport_wheel += ptr.wheel, teleport_follow = false, changed = true;
}
/// A debug tool's row on the Debug tab (Row::cheat i): Left and Right, and confirm (press[7]: A, Enter, a click). Teleport and
/// Inventory open their pages; back from them comes to the Debug tab, the row still selected
void cheat_press(int i, const bool* press, bool& changed) {
    if (i < 2 && (press[3] || press[4] || press[7])) {   // (Left Off, Right On, confirm flips)
        const bool was = i == 0 ? protect_hearts.load() == 1 : player_flight.load() >= 0;
        const bool on = press[7] ? !was : press[4];
        if (i == 0) protect_hearts.store(on ? 1 : 0);
        else if (on != was) player_flight.store(on ? 0 : -1);
        changed = true;
    }
    if (i == 4 && (press[3] || press[4])) {
        clock_hour = (clock_hour + (press[4] ? 1 : 23)) % 24;
        changed = true;
    }
    if (i == 5 && (press[3] || press[4])) {
        pouch_item = (pouch_item + kPouchItems + (press[4] ? 1 : -1)) % kPouchItems;
        changed = true;
    }
    if (i == 11 && (press[3] || press[4])) {   // (Movement speed: stops at 1x and 3x)
        link_speed.store(std::clamp(link_speed.load() + (press[4] ? 1 : -1), 0, kLinkSpeeds - 1));
        changed = true;
    }
    if (i == 6 && (press[3] || press[4])) {
        const int step = press[4] ? 1 : -1;
        pouch_delta = std::clamp(pouch_delta + step, -10, 10);
        if (!pouch_delta) pouch_delta = step;
        changed = true;
    }
    if (press[7] && i >= 2 && i <= 4) cheat_action.store(i == 4 ? 100 + clock_hour : i);
    if (press[7] && i == 6) cheat_action.store(200 + pouch_item * 32 + pouch_delta + 10);
    if ((press[3] || press[4] || press[7]) && i == 7) {   // (an On / Off as the first two: Left Off, Right On, confirm flips)
        free_camera.store(press[7] ? !free_camera.load() : press[4] ? 1 : 0);
        changed = true;
    }
    if (press[7] && i == 8) page = 3, teleport_follow = true, changed = true;
    if (press[7] && i == 10) page = 4, choose_inventory_filter(0), changed = true;
    if (i == 9 && (press[3] || press[4])) {   // (Kid | Adult: Left Kid, Right Adult, a switch when Link is the other age)
        const int age = link_age.load(std::memory_order_relaxed);
        if (age >= 0 && age != (press[4] ? 1 : 0)) {
            cheat_action.store(999);
            open_now = false;
            changed = true;
        }
    }
    if (press[7] && i == 9) {
        cheat_action.store(999);   // (host/hos_main.c OOT_AGE_ACTION)
        open_now = false;
        changed = true;
    }
}

void menu_tick() {
    fullscreen_tick();
    enum { K_N = 17 };
    static bool held[K_N];
    static const int keys[K_N] = {VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1,   // (F1: for keyboards without Insert)
                                  VK_RETURN, VK_DELETE, VK_BACK,
                                  'Q', 'E', VK_TAB,   // (the tabs: Q and E, Tab and Shift+Tab)
                                  VK_PRIOR, VK_NEXT, VK_HOME, VK_END};   // (the long lists: a page, the ends)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool mine = pid == GetCurrentProcessId();
    bool press[K_N] = {};
    for (int i = 0; i < K_N; i++) {
        const bool down = mine && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        const bool queued = host && host->take_key_press && host->take_key_press(keys[i]);
        // Read both keys even if one is disabled, so old presses cannot toggle the menu after a change.
        // (F1 always opens the menu; Menu shortcut's F1 only leaves Insert to an overlay)
        const bool enabled = keys[i] != VK_INSERT || opts.menu_shortcut != 1;   // (Insert: all but F1 only)
        press[i] = enabled && (host && host->take_key_press ? mine && queued : down && !held[i]);
        held[i] = down;
        if (press[i]) pad_last = false;   // (the keys: their names in the prompts)
    }
    // (hookshot) the gamepads, by position: Home, or Select and Start together, as Insert; the D-pad as the arrows; the
    // buttons the Switch's A and B are bound to as Enter and Esc, as in the game (vk_menu_keys.h; with the Nintendo layout A
    // is the right face button), not while a binding waits for its button
    static uint32_t pad_held = 0;
    const uint32_t raw = mine && host && host->pad_raw ? host->pad_raw() : 0, pad_press = raw & ~pad_held;
    const uint32_t chord = 1u << HOS_PAD_BACK | 1u << HOS_PAD_START;
    const bool chord_now = (raw & chord) == chord && (pad_held & chord) != chord;
    pad_held = raw;
    if (pad_press) pad_last = true;   // (a controller: its buttons in the prompts)
    {   // (RX_VK_UI_PROMPTS=pad or keys: the prompts for screenshots of test runs, which have no controller and no focus)
        static const char* force = getenv("RX_VK_UI_PROMPTS");
        if (force) pad_last = !strcmp(force, "pad");
    }
    auto pad = [&](int p) { return listening != 1 && (pad_press >> p & 1) != 0; };
    // (hookshot) a DualShock 4's or DualSense's touchpad click is F1: it opens and closes the menu as the key does
    if (pad(HOS_PAD_TOUCHPAD)) press[6] = true;
    if (host && host->binding)   // (A and B as bound now: the Controls page or the Layout preset can change them)
        menu_keys::confirm_back(host->binding(HOS_BIND_PAD, HOS_SW_A, 0), host->binding(HOS_BIND_PAD, HOS_SW_B, 0), &pad_confirm,
                                &pad_back);
    if (pad(HOS_PAD_GUIDE) || chord_now) press[0] = true;
    {   // holding an arrow or the D-pad repeats it: after 0.35 s, 12 a second, then 30 a second after 1.5 s held
        static int64_t held_since[5], next_repeat[5];
        const int pads[5] = {0, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT, HOS_PAD_RIGHT};
        const int64_t t = prof_now();
        for (int i = 1; i <= 4; i++) {
            const bool down = held[i] || (open_now.load() && listening != 1 && (raw >> pads[i] & 1));
            if (!down) { held_since[i] = 0; continue; }
            if (!held_since[i]) { held_since[i] = t, next_repeat[i] = t + 350000000LL; continue; }
            if (t >= next_repeat[i] && open_now.load() && listening == 0) {
                press[i] = true;
                next_repeat[i] = t + (t - held_since[i] > 1500000000LL ? 33000000LL : 83000000LL);
            }
        }
    }
    if (open_now.load() && listening != 1) {
        press[1] |= pad(HOS_PAD_UP), press[2] |= pad(HOS_PAD_DOWN), press[3] |= pad(HOS_PAD_LEFT);
        press[4] |= pad(HOS_PAD_RIGHT), press[5] |= pad(pad_back), press[7] |= pad(pad_confirm);
    }
    {   // RX_VK_MENU_SCRIPT (test runs, which never have the focus): timed menu keys, "<seconds>:<INS|UP|DOWN|LEFT|RIGHT|ESC|ENTER|TABL|TABR>,...";
        // and the gamepad's face buttons by position, PADS PADE PADW PADN (bottom, right, left, top), as a controller's press
        static const char* next = getenv("RX_VK_MENU_SCRIPT");
        static const int64_t start = prof_now();
        double at;
        char key[8];
        int n = 0;
        if (next && sscanf(next, "%lf:%7[A-Z]%n", &at, key, &n) == 2 && (prof_now() - start) / 1e9 >= at) {
            static const char* names[] = {"INS", "UP", "DOWN", "LEFT", "RIGHT", "ESC", "", "ENTER", "", "", "TABL", "TABR"};
            for (int i = 0; i < 12; i++) press[i] |= *names[i] && !strcmp(key, names[i]);
            static const char* pads[] = {"PADS", "PADE", "PADW", "PADN"};
            for (int p = 0; p < 4; p++)
                if (!strcmp(key, pads[p]) && open_now.load() && listening != 1) {
                    press[5] |= p == pad_back, press[7] |= p == pad_confirm;
                    pad_last = true;
                }
            next = next[n] == ',' ? next + n + 1 : nullptr;
        }
    }
    bool tab_left = press[10] || pad(HOS_PAD_LSHOULDER) || pad(HOS_PAD_LTRIGGER);
    bool tab_right = press[11] || press[12] || pad(HOS_PAD_RSHOULDER) || pad(HOS_PAD_RTRIGGER);
    if (press[12] && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) tab_left = true, tab_right = false;
    bool changed = menu_refresh;
    menu_refresh = false;
    // (hookshot) the mouse, while the menu is open: the cursor in the window image's pixels (mx, my), tested against the
    // controls as last drawn (hits, vk_menu_hit.h), whether it moved since the last frame, and its clicks and wheel since then
    MkwVkPointer ptr{};
    int mx = -1, my = -1;
    bool mouse = false, moved = false;
    if (open_now.load() && host && host->pointer) {
        mouse = host->pointer(&ptr) != 0;
        int ix = 0, iy = 0;
        menu_hit::to_image(ptr.x, ptr.y, ptr.width, ptr.height, (int)dev.swap_extent.width, (int)dev.swap_extent.height,
                           &ix, &iy);
        if (mouse) mx = ix - (int)menu_ox, my = iy;   // (the menu's column's pixels: draw_menu)
        static int last_x = -1, last_y = -1;
        moved = mouse && mouse_read && (mx != last_x || my != last_y);   // (not the first read: a cursor resting over the
        last_x = mx, last_y = my, mouse_read = true;                     // menu as it opens leaves the selection alone)
        if (moved) pad_last = false;
        if (!mouse) ptr.pressed = 0, ptr.wheel = 0;
    }
    // (closing with a setting that takes a restart changed: the restart question first, Later already chosen on it)
    auto later = [&] {
        int now[mr::kMax];
        restart_values(now);
        restart_state.choose_later(now);
        open_now = false, page = 0, changed = true;
    };
    // (Menu shortcut "Insert, F1 or Esc": Esc on a closed menu opens it, the keyboard's alone (the
    // gamepad's back is Esc only while it is open, above). The press that opened it does not go back a page below. No
    // control can be bound to Esc, so nothing in play changes; the window gives the captured mouse back on the close)
    const bool esc_opens = opts.menu_shortcut == 2 && press[5] && !open_now.load();
    if (esc_opens) press[5] = false;
    if ((press[0] || press[6]) && open_now.load() && page == 5) {
        later();
    } else if ((press[0] || press[6]) && open_now.load() && restart_ask()) {
        page = 5, ask_sel = 0, listening = 0, changed = true;
    } else if (press[0] || press[6] || esc_opens) {
        open_now = !open_now.load(), changed = true;
        page = 0, listening = 0;
        // (the layout row shows the controls as they are: host/nn_hid.c keeps them, the ini's pad_ lines, and a saved
        // layout is not applied over them at start)
        if (open_now.load()) opts.pad_layout = detect_pad_layout();
        if (open_now.load()) dlss_probe(), fsr4_probe();   // (whether the Upscaler row can offer DLSS and FSR here: asked once)
        if (open_now.load()) refresh_mics();   // (the microphones as they are now)
    }
    if (open_now.load() && page == 5) {   // (hookshot) the restart question: Later (the default) or Restart now
        if (mouse) {   // (a button under the cursor chosen, a click on it pressed, a right click is Later; no list: the wheel does nothing)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            if (hit.part == menu_hit::kButton && (moved || ptr.pressed) && hit.index != ask_sel) ask_sel = hit.index, changed = true;
            if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kButton) press[7] = true;
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
        }
        if (press[1] || press[2] || press[3] || press[4] || tab_left || tab_right) ask_sel ^= 1, changed = true;
        if (press[5] || (press[7] && ask_sel == 0)) {
            later();
        } else if (press[7]) {   // (Restart now: everything saved, then the window closes as the player would close it)
            logf("menu: restart now, to apply %s", restart_list(", ").c_str());
            save();
            pipeline_cache_save(true);
            open_now = false, page = 0, changed = true;
            PostMessageA(app_window(), WM_APP + 9, 0, 0);
        }
    } else if (open_now.load() && page == 1) {   // (hookshot) the Controls page
        if (!can_bind()) page = 0;
        else if (listening) {
            if (press[5] || prof_now() > listen_end) listening = 0, changed = true;   // (Esc, or nothing for a while)
            else if (listening == 1) {
                if (!listen_armed) listen_armed = (raw & ~(1u << HOS_PAD_GUIDE)) == 0;
                else if (const uint32_t p = pad_press & ~(1u << HOS_PAD_GUIDE) & ~(1u << HOS_PAD_TOUCHPAD)) {
                    int bit = 0;
                    while (!(p >> bit & 1)) bit++;
                    host->bind(HOS_BIND_PAD, controls[csel - kTop].button, 0, bit);
                    listening = 0, changed = true;
                    opts.pad_layout = detect_pad_layout();
                    save();
                }
            } else {
                int down = 0;
                for (int k : bindable_keys())
                    if (mine && (GetAsyncKeyState(k) & 0x8000)) {
                        down = k;
                        break;
                    }
                if (!listen_armed) listen_armed = !down;
                else if (down) {
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 0, down);
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 1, 0);
                    listening = 0, changed = true;
                    save();
                }
            }
        } else {
            if (mouse) {   // (the mouse: the row under it selected, a click on a binding listens for its new input, on
                           // Reset resets; the wheel scrolls the page, a right click goes back)
                const menu_hit::Hit hit = hits.at((float)mx, (float)my);
                const bool option = hit.part == menu_hit::kCtlOption;
                const bool on = option || hit.part == menu_hit::kCtlRow || hit.part == menu_hit::kCtlCell;
                const int at = option ? hit.index : kTop + hit.index;   // (the page's row: its own rows first)
                if (on && (moved || ptr.pressed)) {
                    if (at != csel) csel = at, changed = true;
                    if (hit.part == menu_hit::kCtlCell && !keys_only(hit.index) && ccol != hit.side) ccol = hit.side, changed = true;
                }
                if (ptr.pressed & HOS_MOUSE_LEFT) {
                    if (option) press[hit.side < 0 ? 3 : 4] = true;   // (a value's halves: Left and Right; the name: Right)
                    else if (hit.part == menu_hit::kCtlCell || (hit.part == menu_hit::kCtlRow && hit.index == kControls))
                        press[7] = true;   // (as Enter)
                }
                if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
                if (ptr.wheel) wheel_rows[kControlsSlot] += ptr.wheel, changed = true;   // (scrolls; the selection stays)
            }
            if (press[5]) page = 0, changed = true;
            const int n = kTop + kControls + 1, i = csel - kTop;   // (i: the control's, or reset's)
            if (press[1]) csel = (csel + n - 1) % n, changed = true;
            if (press[2]) csel = (csel + 1) % n, changed = true;
            if (i < 0 && (press[3] || press[4] || press[7])) {   // (the page's own rows: Enter as Right)
                Row& r = rows[tab_row_to_global(TAB_PAGE, csel)];   // (Custom is only what the bindings are: set_row skips it)
                set_row(r, *r.value + (press[3] ? -r.step : r.step), press[3] ? -1 : 1);
                changed = true;
            } else if (i >= 0) {
                if ((press[3] || press[4]) && i < kControls && !keys_only(i)) ccol ^= 1, changed = true;
                if (keys_only(i)) ccol = 1;   // (the stick's and the camera's directions: keys only)
                if (press[7]) {
                    if (i == kControls) {
                        host->bind(HOS_BIND_PAD, -1, 0, 0);
                        opts.pad_layout = 0;
                        save();
                    }
                    else listening = ccol ? 2 : 1, listen_armed = false, listen_end = prof_now() + 6000000000LL;
                    changed = true;
                }
                if ((press[8] || press[9]) && i < kControls) {
                    if (ccol == 0) {
                        host->bind(HOS_BIND_PAD, controls[i].button, 0, -1);
                        opts.pad_layout = detect_pad_layout();
                    }
                    else for (int k = 0; k < 2; k++) host->bind(HOS_BIND_KEY, controls[i].button, k, 0);
                    changed = true;
                    save();
                }
            }
        }
    } else if (open_now.load() && page == 4) {   // the inventory
        const int chosen = item_choice;   // (the page follows the choice when it moves; the wheel's page stays otherwise)
        if (mouse) inventory_mouse(ptr, mx, my, press, changed);
        if (press[5] && inventory_amount) inventory_amount = false, changed = true;   // (back: out of the amount first)
        else if (press[5]) page = 0, changed = true;   // (to the Debug tab, Inventory still selected)
        else if (tab_left || tab_right) {   // (another section, from editing an amount too: that ends)
            const int filters = kItemSections + 1;
            choose_inventory_filter((inventory_filter + filters + (tab_left ? -1 : 1)) % filters);
            changed = true;
        } else if (inventory_amount) {
            if (press[7]) inventory_amount = false, changed = true;
            else if (press[3] || press[4]) cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 64 + (press[4] ? 1 : -1));
        } else {
            // (the cards as drawn, inventory_columns to a row: the arrows move, Page Up and Down a page, Home and End the ends)
            const int begin = inventory_begin(), end = inventory_end();
            const int step = (press[4] - press[3]) + (press[2] - press[1]) * inventory_columns + (press[14] - press[13]) * inventory_capacity;
            if (step || press[15] || press[16]) {
                item_choice = press[15] ? begin : press[16] ? end - 1 : std::clamp(item_choice + step, begin, end - 1);
                inventory_reveal();
                changed = true;
            } else if (press[7] && item_counts[item_choice].load(std::memory_order_relaxed) >= 0) {
                if (item_kinds[item_choice]) inventory_amount = true, changed = true;   // (an amount: Left and Right change it)
                else cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 200);   // (unlock or relock)
            }
            if (item_choice != chosen) inventory_reveal();   // (a click on a card: its page)
        }
    } else if (open_now.load() && page == 3) {   // the teleport list
        if (mouse) teleport_mouse(ptr, mx, my, press, changed);
        if (press[5]) page = 0, changed = true;   // (to the Debug tab, Teleport still selected)
        else if (press[7]) {
            cheat_action.store(1000 + teleport_choice);
            open_now = false, page = 0;   // (back to the game: the map change starts with an event)
            changed = true;
        } else if (tab_left || tab_right) {   // (the first map of the previous or next section)
            const int section = (teleport_section_of(teleport_choice) + kTeleportSections + (tab_left ? -1 : 1)) % kTeleportSections;
            teleport_choice = teleport_sections[section].first, teleport_follow = true;
            changed = true;
        } else {
            // (the cards as drawn, each section's in rows under its heading: Up and Down a row, the column kept; Left and Right the
            // previous and next card; Page Up and Down the rows that fit; Home and End the ends)
            const int rows = (press[2] - press[1]) + (press[14] - press[13]) * teleport_page_rows;
            const int was = teleport_choice;
            if (press[15]) teleport_choice = 0;
            else if (press[16]) teleport_choice = kTeleports - 1;
            else teleport_choice = std::clamp(teleport_step_rows(teleport_choice, rows) + (press[4] - press[3]), 0, kTeleports - 1);
            if (teleport_choice != was) teleport_follow = true, changed = true;
        }
    } else if (open_now.load()) {
        if (mouse) {   // (the mouse: see the top of this file)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            const int ht = hit.part == menu_hit::kTab ? hit.index : hit.part == menu_hit::kTabPrev ? TAB_COUNT
                         : hit.part == menu_hit::kTabNext ? TAB_COUNT + 1 : -1;
            if (ht != hover_tab) hover_tab = ht, changed = true;
            int& sel = tab_selected[cur_tab];
            const bool on_row = hit.part == menu_hit::kRow || hit.part == menu_hit::kSlider;
            if (drag_row >= 0) {   // (a slider held: it follows the cursor's x until let go)
                Row& d = rows[tab_row_to_global(cur_tab, drag_row)];
                if (!(ptr.held & HOS_MOUSE_LEFT) || !d.value) {
                    drag_row = -1;
                    save();
                } else {
                    const int permille = hits.drag((float)mx, drag_row);
                    const int v = permille < 0 ? *d.value : menu_hit::slider_value(permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false), changed = true;
                }
            } else if (on_row && (moved || ptr.pressed) && hit.index != sel) {
                // (the row under the cursor is in view already: no scrolling to it. Brought into view as the keys' choice
                // is, with its row of context, the top row slid down under a cursor that stayed put, and the next click took
                // the row above: picking the Upscaler changed the Output resolution, and the menu asked to restart)
                sel = hit.index, scrolled_to[cur_tab] = sel, changed = true;
            }
            if ((ptr.pressed & HOS_MOUSE_LEFT) && drag_row < 0) {
                if (hit.part == menu_hit::kTabPrev) tab_left = true;
                else if (hit.part == menu_hit::kTabNext) tab_right = true;
                else if (hit.part == menu_hit::kTab && hit.index != cur_tab) cur_tab = hit.index, changed = true;
                else if (hit.part == menu_hit::kSlider) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    drag_row = hit.index;
                    const int v = menu_hit::slider_value(hit.permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false);
                    changed = true;
                } else if (hit.part == menu_hit::kRow) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    if (!d.value && d.cheat >= 0 && (cheat_chooses(d.cheat) || age_shown(d.cheat) || d.cheat <= 1 || d.cheat == 7) &&
                        hit.side != 0)
                        press[hit.side < 0 ? 3 : 4] = true;   // (a tool's choice: its halves as Left and Right)
                    else if (!d.value) press[7] = true;   // (a page, a tool: as Enter)
                    else switch (menu_hit::row_click(true, d.hi - d.lo == d.step, hit.side, *d.value == d.hi)) {
                    case menu_hit::kClickConfirm: press[7] = true; break;
                    case menu_hit::kClickRight: press[4] = true; break;     // (the right half as Right; a toggle's other half flips it)
                    case menu_hit::kClickLeft: press[3] = true; break;      // (the left half as Left)
                    case menu_hit::kClickSelect: break;   // (the name, or a toggle's half already chosen: selected above, the value as it is)
                    }
                }
            }
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;   // (as Esc)
            // (the wheel scrolls the rows, wherever the cursor is; it never changes a value, the selection or the tab: the
            // values change by a click on their arrows or halves, a slider's drag, Left and Right, A and Enter)
            if (ptr.wheel && drag_row < 0) wheel_rows[cur_tab] += ptr.wheel, changed = true;
        }
        if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        if (press[5] && pending) pending = nullptr, changed = true;   // (a Restart or Quit waiting for its second press: cancelled)
        else if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        else if (press[5] && restart_ask()) page = 5, ask_sel = 0, changed = true;   // (the restart question first)
        else if (press[5]) open_now = false;
        // (the tabs: LB and RB, Q and E, or Tab; from the Motion page, as from Controls)
        if ((tab_left || tab_right) && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS;
        if (tab_left) cur_tab = (cur_tab + TAB_COUNT - 1) % TAB_COUNT, changed = true;
        else if (tab_right) cur_tab = (cur_tab + 1) % TAB_COUNT, changed = true;
        const int tab_count = tab_row_count(cur_tab);
        int& selected = tab_selected[cur_tab];
        if (selected >= tab_count) selected = tab_count - 1;   // (a row gone since: the debug tools turned off, HDR turned off: row_shown)
        auto heading_at = [&](int k) { return rows[tab_row_to_global(cur_tab, k)].heading; };   // (stepped over)
        if (heading_at(selected)) selected = (selected + 1) % tab_count;
        if (cur_tab == TAB_DEBUG && (press[1] || press[2])) {   // (the Debug tab: in its layout's order, debug_rows)
            selected = debug_step(selected, press[2] ? 1 : -1);
            changed = true;
        } else {
            if (press[1]) {
                do selected = (selected + tab_count - 1) % tab_count;
                while (heading_at(selected));
                changed = true;
            }
            if (press[2]) {
                do selected = (selected + 1) % tab_count;
                while (heading_at(selected));
                changed = true;
            }
        }
        Row& r = rows[tab_row_to_global(cur_tab, selected)];
        const bool confirm_selected = &r == &rows[kQuitRow] || &r == &rows[kRestartRow];
        if (pending && (pending != &r || !mine)) pending = nullptr, changed = true;
        if (press[7] && confirm_selected) {
            if (mine && !press[5]) {
                if (pending != &r) {
                    pending = &r, changed = true;
                } else {   // (as Restart now: saved first, as the window's own close does not force the pipeline cache)
                    save();
                    pipeline_cache_save(true);
                    pending = nullptr;
                    if (&r == &rows[kRestartRow]) {   // (the restart question's Restart now: the window closes, then the game starts again)
                        logf("menu: restart");
                        open_now = false, changed = true;
                        if (const HWND window = app_window()) PostMessageA(window, WM_APP + 9, 0, 0);
                    } else if (const HWND window = app_window()) {
                        PostMessageA(window, WM_CLOSE, 0, 0);
                    }
                }
            }
        } else if (r.cheat >= 0) cheat_press(r.cheat, press, changed);   // (a debug tool: on the Debug tab itself)
        else if (press[7] && is_motion_row(r)) cur_tab = TAB_MOTION, tab_selected[TAB_MOTION] = 0, press[7] = false, changed = true;
        else if (press[7] && !r.value && can_bind()) page = 1, csel = 0, ccol = 0, changed = true;
        if ((press[3] || press[4] || press[7]) && r.value) {   // (Enter, or A, as Right)
            if (press[7]) press[4] = true;
            set_row(r, *r.value + (press[4] ? r.step : -r.step), press[4] ? 1 : -1);
            changed = true;
        }
        const int64_t now = prof_now();
        if (changed) {   // (a new row or value: its name from the start, after a rest)
            marquee_pos = 0, marquee_next = now + kMarqueeRest;
        } else if (marquee_max > 0 && now >= marquee_next) {
            if (marquee_pos >= marquee_max) marquee_pos = 0, marquee_next = now + kMarqueeRest;   // (back to the start)
            else if (++marquee_pos >= marquee_max) marquee_next = now + kMarqueeRest;            // (rest at the end)
            else marquee_next = now + kMarqueeStep;
            changed = true;
        }
    }
    {   // (hookshot) the window lets the mouse go while the menu is open, and takes it back after (host/display.c)
        static bool told = false;
        const bool now_open = open_now.load();
        if (!now_open) {
            if (drag_row >= 0) save();   // (a slider let go off the window: kept now)
            hover_tab = -1, drag_row = -1, mouse_read = false;
            pending = nullptr;
        }
        if (now_open != told && app_window()) {
            told = now_open;
            PostMessageA(app_window(), WM_APP + 8, (WPARAM)now_open, 0);
        }
    }
    if (opts.show_fps) {
        static int64_t next = 0, last_game = 0, last_shown = 0;
        const int64_t now = prof_now();
        double shown = 0, game = 0;
        if (fps_take(now, shown_frames.load(), game_frames.load(), &next, &last_shown, &last_game, &shown, &game)) {
            snprintf(fps_text, sizeof fps_text, "%.0f", shown);
            if (opts.framegen >= 1) snprintf(fps_sub, sizeof fps_sub, "game %.0f", game);
            else fps_sub[0] = 0;
        }
    }
}

void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {
    const bool start = startup_on.load(std::memory_order_acquire);
    const bool fps = opts.show_fps && !start;   // (no game frames yet: not "0 FPS" over the start-up screen)
    const bool open = open_now.load(std::memory_order_relaxed);
    if (!start && !fps && !open) {   // (nothing shows: no ImGui frame, no render pass)
        // (but a new window size or Menu size since the last bake: one frame with nothing drawn that bakes the menu's glyphs
        // and measures its page for the new size, so the next open has no hitch whatever was resized while nothing showed)
        if (prebake_needed(w, h) && ui::begin(w, h)) {
            ui::Ctx c = ui_ctx(w, h);
            prebake(c);
            ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
            ui::record(cmd, index, w, h);
            return;
        }
        ui::idle();
        return;
    }
    if (!ui::begin(w, h)) return;
    {   // (the easing's step: the time since the last drawing, a quarter of the way in 1/60 s)
        static int64_t last = 0;
        const int64_t now = prof_now();
        ease_step = last ? std::min(1.0f, (float)((now - last) / 1e9) * 15.0f) : 1.0f;
        last = now;
    }
    ui::Ctx c = ui_ctx(w, h);
    prebake(c);   // (the menu's glyphs at this size, before it opens: no hitch then)
    if (start) draw_startup(c);   // (under the menu)
    if (open) draw_menu(c);
    if (fps) draw_fps(c, open);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record(cmd, index, w, h);
}

void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    const bool open = open_now.load(std::memory_order_relaxed);
    const bool fps = opts.show_fps && !startup_on.load(std::memory_order_acquire);
    if (!open && !fps) return;
    if (!ui::begin(w, h)) return;
    ui::Ctx c = ui_ctx(w, h);
    if (open) draw_menu(c);
    if (fps) draw_fps(c, open);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record_to(cmd, image, view, w, h);
}

} // namespace mkwvk
