// (hookshot) NVIDIA DLSS Super Resolution, one of the temporal upscaler's engines (vk_dlss.h). The frame chain it draws
// with (the scene's camera and depth, the inputs, the actor motion, the jitter, the picture into the network's output) is
// vk_temporal.cpp's; here are NGX, DLSS's feature and its evaluation.
//
// RX_VK_DLSS=0 / 1 / 2: the game's own, DLSS, DLAA over the Insert menu's Upscaler row (0 also leaves NGX's extensions
// off). DLSS's runtime (Windows: nvngx_dlss.dll; Linux: libnvidia-ngx-dlss.so.*), and DLSS Frame Generation's beside it
// (nvngx_dlssg.dll; libnvidia-ngx-dlssg.so.*: without one there NGX takes the driver's own copy, which may be older), is
// looked for in RX_VK_DLSS_DIR=<folder>
// when set, the game folder's ngx folder, and %LOCALAPPDATA%\Hookshot\ngx (Linux: $XDG_DATA_HOME/hookshot/ngx): NGX takes
// the newest one it finds in them. Windows' paths are wide all the way (GetModuleFileNameW, _wgetenv), so any folder name
// works. On Linux NGX takes only ASCII paths: a folder named otherwise goes to it as /proc/self/fd/<n> (wide()), and the
// runtime itself has to be in a folder whose real path is ASCII (the log and the menu say so when it is not). The Insert
// menu's DLSS model row (opts.dlss_model, upscaler_rows.h) chooses DLSS's preset: K by default, M, L, or NVIDIA's default
// (0). RX_VK_DLSS_PRESET=<letter> (or 0) over it. RX_VK_DLSS_LOG=1: NGX's own log in ours.
//
// NVIDIA's NGX SDK is not part of Hookshot (NVIDIA's license): a local build fetches it (scripts/ngx_sdk.py) and builds with
// HOOKSHOT_NGX_SDK, which compiles this file with HOOKSHOT_NGX. Without it only the stubs at the end are here.
// NGX's core is the driver's (Windows: _nvngx.dll, which NVIDIA's loader finds in the driver's own folder; Linux:
// libnvidia-ngx.so). It is never put beside the game: a copy there would be loaded in place of the driver's.
#include "vk_dlss.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>   // (wide: a folder's descriptor for NGX)
#endif

#include "dlss_checks.h"
#include "upscaler_rows.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_reactive.h"
#include "vk_temporal.h"
#include "vk_texture.h"

#ifdef HOOKSHOT_NGX
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssg_vk.h"   // (DLSS Frame Generation: its parameters and the create/evaluate helpers)
#include <cfloat>
#include <deque>
#include "dlss_math.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {

extern std::atomic<bool> present_native_pq;   // (vk_replay.cpp do_present: the game presents its own HDR10 picture, Native)

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
}  // namespace

// (what is known of DLSS on this device: -1 not yet, else a DlssWhy. Set once the device is made and when NGX answers)
std::atomic<int> g_why{-1};

DlssWhy dlss_why() {
    if (!dlss_built()) return DLSS_NOT_BUILT;
    static const bool env_off = env_switch("RX_VK_DLSS") == 0;
    if (env_off) return DLSS_ENV_OFF;
    const int w = g_why.load(std::memory_order_relaxed);
    return w < 0 ? DLSS_OK : (DlssWhy)w;
}

#ifdef HOOKSHOT_NGX

namespace {

/// NGX and DLSS's feature. The feature is used with the temporal upscaler's lock held; NGX's start (ngx_init) also from the
/// menu's thread (dlss_probe), under mu
struct State {
    std::mutex mu;
    bool ngx_tried = false, ngx_ok = false;
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t feature_rw = 0, feature_rh = 0, feature_ow = 0, feature_oh = 0;   // (its largest render size, its output)
    uint32_t min_rw = 0, min_rh = 0;
    unsigned feature_preset = ~0u;   // (the preset it was made with: another one from the menu makes it again)
    // (made for the game's native HDR (the game presenting its own HDR10 picture: present_native_pq): its scene colour goes
    // past 1 there, and an LDR feature fed the colour clamped to 1 cut every highlight at the paper white, a flat, SDR-looking
    // picture. Then the feature takes HDR colour, linear, with its own exposure, and the chain does not clamp it: hdr())
    bool feature_hdr = false;
    bool feature_aa = false;
    uint64_t features_made = 0;      // (how many times: a feature made again in play costs a stall, so each says why)
    // (the preset, output and HDR a creation failed for: not tried again each frame, but once any of them changes, so the
    // menu's DLSS rows bring DLSS back without a restart. 0: none failed)
    uint64_t refused = 0;
    // (the last evaluation's result, and how many times in a row it failed that way: what its log line is spaced by)
    NVSDK_NGX_Result eval_result = NVSDK_NGX_Result_Success;
    uint64_t eval_repeats = 0;
};
State& st() {
    static State s;
    return s;
}

NVSDK_NGX_Resource_VK resource(const TemporalImage& im, bool write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, im.format,
                                                  im.width, im.height, write);
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string m(message ? message : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    logf("ngx: %s", m.c_str());
}

namespace fs = std::filesystem;
#ifdef _WIN32
/// The game folder (game_module_path's rule: bin\hookshot.exe beside image.bin is the folder above bin), from
/// GetModuleFileNameW: a folder with characters outside the ANSI code page is still the right one
fs::path game_dir() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (!n || n >= exe.size()) return {};
    exe.resize(n);
    fs::path dir = fs::path(exe).parent_path();
    if (!_wcsicmp(dir.filename().c_str(), L"bin") && GetFileAttributesW((dir.parent_path() / L"image.bin").c_str()) != INVALID_FILE_ATTRIBUTES)
        dir = dir.parent_path();
    return dir;
}
/// An environment variable as a path (_wgetenv: not through the ANSI code page); empty when unset
fs::path env_path(const wchar_t* name) {
    const wchar_t* e = _wgetenv(name);
    return e && *e ? fs::path(e) : fs::path();
}
/// A path for our log (UTF-8)
std::string shown(const fs::path& p) {
    const std::wstring& w = p.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
/// A path for NGX, which takes wide strings: Windows' own
std::wstring wide(const fs::path& p) { return p.native(); }
#else
fs::path game_dir() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    return fs::path(std::string(exe, n)).parent_path();
}
fs::path env_path(const char* name) {
    const char* e = getenv(name);
    return e && *e ? fs::path(e) : fs::path();
}
std::string shown(const fs::path& p) { return p.native(); }
/// A path for NGX, which takes wide strings. NGX's Linux library turns them back into bytes in the "C" locale
/// (WStrToUTF8Str: newlocale(LC_ALL, "C") and wcsrtombs), which fails on any character past ASCII, and the path comes out
/// empty. Before, each byte was also widened alone ("CafÃ©"). So a folder whose path is not all ASCII goes to NGX as
/// /proc/self/fd/<n>, the folder opened and kept open for the process's life: an ASCII name for the same folder, nothing
/// made on the disk. NGX writes its own files (the data path) and finds the runtime through it; the driver's part then
/// loads the runtime by its real path (realpath), which has to be ASCII too: ngx_init says so when that is all there is.
/// When the folder cannot be opened (not there), its path decoded from UTF-8 (dlss_checks.h, not through the locale; bytes
/// that are not UTF-8 become U+FFFD), for an NGX that takes it. Each such folder is said in the log
std::wstring wide(const fs::path& p) {
    const std::string& s = p.native();
    if (dlss_checks::ascii(s)) return std::wstring(s.begin(), s.end());
    if (const int fd = open(s.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); fd >= 0) {
        const std::string alias = "/proc/self/fd/" + std::to_string(fd);
        logf("vk: NVIDIA DLSS: %s is not all ASCII, which NGX's Linux library does not take: given to it as %s", s.c_str(),
             alias.c_str());
        return std::wstring(alias.begin(), alias.end());
    }
    bool bad = false;
    std::wstring w = dlss_checks::utf8_to_wide(s, &bad);
    logf("vk: NVIDIA DLSS: %s is not all ASCII and could not be opened: given to NGX as it is%s", s.c_str(),
         bad ? " (not UTF-8: U+FFFD for its bad bytes)" : "");
    return w;
}
#endif

#ifdef _WIN32
constexpr const char* kRuntime = "nvngx_dlss.dll";   // (DLSS's runtime, as scripts/ngx_sdk.py fetches it)
constexpr const fs::path::value_type* kRuntimeName = L"nvngx_dlss.dll";
#else
constexpr const char* kRuntime = "libnvidia-ngx-dlss.so";
constexpr const fs::path::value_type* kRuntimeName = "libnvidia-ngx-dlss.so";
#endif
#ifdef _WIN32
constexpr const char* kFgRuntime = "nvngx_dlssg.dll";   // (DLSS Frame Generation's runtime, as scripts/ngx_sdk.py fetches it)
constexpr const fs::path::value_type* kFgRuntimeName = L"nvngx_dlssg.dll";
#else
constexpr const char* kFgRuntime = "libnvidia-ngx-dlssg.so";
constexpr const fs::path::value_type* kFgRuntimeName = "libnvidia-ngx-dlssg.so";
#endif
// (DLSS-G's sets refused in a row before it is given up, as after three failed creations of its feature: an older runtime,
// the driver's own 310.2.1 on an RTX 5080, refused every set at 2x, each refusal a wasted evaluation, and the game ran at
// half its rate with no frame made)
constexpr int kFgRefusalsOff = 3;

/// The folders NGX looks for its runtimes in (DLSS's, kRuntime, and frame generation's): RX_VK_DLSS_DIR, the game folder's
/// ngx, then the user's
std::vector<fs::path> runtime_dirs(const fs::path& game) {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    if (fs::path e = env_path(L"RX_VK_DLSS_DIR"); !e.empty()) dirs.push_back(e);
    dirs.push_back(game / L"ngx");
    if (fs::path l = env_path(L"LOCALAPPDATA"); !l.empty()) dirs.push_back(l / L"Hookshot" / L"ngx");
#else
    if (fs::path e = env_path("RX_VK_DLSS_DIR"); !e.empty()) dirs.push_back(e);
    dirs.push_back(game / "ngx");
    if (fs::path x = env_path("XDG_DATA_HOME"); !x.empty()) dirs.push_back(x / "hookshot" / "ngx");
    else if (fs::path h = env_path("HOME"); !h.empty()) dirs.push_back(h / ".local" / "share" / "hookshot" / "ngx");
#endif
    return dirs;
}

/// (a project id of Hookshot's own: NGX wants one with the custom engine type, any GUID)
constexpr const char* kProjectId = "7b2f5d0c-6a1e-4c39-9f84-3d5e2a1b0c96";

/// NGX on this device, once: false when DLSS cannot run here (the reason logged)
bool ngx_init() {
    State& s = st();
    std::lock_guard lk(s.mu);
    if (s.ngx_tried) return s.ngx_ok;
    s.ngx_tried = true;
    // (the game folder or nothing: an empty one made "ngx" relative, and NGX looked for its runtime in the current folder)
    const fs::path game = game_dir();
    if (game.empty() || game.is_relative()) {
        logf("vk: NVIDIA DLSS: the game's folder could not be found: no DLSS");
        g_why = DLSS_NO_RUNTIME;
        return false;
    }
    // where DLSS's runtime (kRuntime) is looked for (NGX takes the newest it finds), and its own files go
    const std::vector<fs::path> dirs = runtime_dirs(game);
    std::vector<std::wstring> wdirs;
    for (const auto& d : dirs) wdirs.push_back(wide(d));
    std::vector<const wchar_t*> paths;
    for (const auto& d : wdirs) paths.push_back(d.c_str());
    const fs::path data = game / "ngx";
    std::error_code made;   // (not the throwing overload: a folder that cannot be made, read-only or so, must not end the game)
    fs::create_directories(data, made);   // (NGX's logs and its own files: the game folder's, not /usr/share)
    const std::wstring wdata = wide(data);
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths.data();
    info.PathListInfo.Length = (unsigned)paths.size();
    static const bool verbose = env_switch("RX_VK_DLSS_LOG") > 0;
    info.LoggingInfo.LoggingCallback = verbose ? ngx_log : nullptr;
    info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    // (NGX also writes its own log files into the data folder; with RX_VK_DLSS_LOG=1 its lines go to ours instead)
    info.LoggingInfo.DisableOtherLoggingSinks = verbose;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                              "1.0", wdata.c_str(), dev.instance, dev.physical, dev.device,
                                                              instance_proc(), vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: NVIDIA DLSS: NGX did not start (%#x): not an NVIDIA GPU, or its driver has no NGX", (unsigned)r);
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.caps)) || !s.caps) {
        logf("vk: NVIDIA DLSS: NGX has no capabilities for this device");
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    // (NGX's C functions, not NVSDK_NGX_Parameter's virtual ones: its object comes from NVIDIA's MSVC-built library, whose
    // vtable order is not the Itanium ABI's that llvm-mingw calls with. The overloads only lined up by luck)
    int available = 0, needs_driver = 0;
    unsigned major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    std::string where;
    for (const auto& d : dirs) where += (where.empty() ? "" : ", ") + shown(d);
    if (!available) {
        // (for the menu: a runtime found in none of the folders is the likely reason; with one there, the GPU (a GTX))
        int init = 0;
        NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init);
        // (Linux: NGX loads the runtime by its real path, ASCII only (wide()): one only in folders named otherwise is not
        // loaded, FeatureInitResult 0xbad00004, though the GPU runs DLSS)
        bool runtime = false, loadable = false;
        std::error_code ec;
        for (const auto& d : dirs)
            for (const auto& f : fs::directory_iterator(d, ec))
                if (f.path().filename().native().rfind(kRuntimeName, 0) == 0) {
                    runtime = true;
#ifdef _WIN32
                    loadable = true;
#else
                    std::error_code real;
                    loadable |= dlss_checks::ascii(fs::canonical(f.path(), real).native()) && !real;
#endif
                }
        if (needs_driver) logf("vk: NVIDIA DLSS: needs NVIDIA's driver %u.%u or newer", major, minor);
        else if (!runtime) logf("vk: NVIDIA DLSS: not available: no %s in %s (scripts/ngx_sdk.py fetches it)", kRuntime, where.c_str());
        else if (!loadable)
            logf("vk: NVIDIA DLSS: not available: %s is only in folders whose path is not all ASCII (%s), and NVIDIA's NGX on "
                 "Linux loads it only from one that is: put it in $XDG_DATA_HOME/hookshot/ngx, or RX_VK_DLSS_DIR=<an ASCII folder>",
                 kRuntime, where.c_str());
        else logf("vk: NVIDIA DLSS: not available on this GPU (%#x): DLSS needs an NVIDIA RTX GPU", (unsigned)init);
        g_why = needs_driver ? DLSS_NEEDS_DRIVER : !runtime ? DLSS_NO_RUNTIME : !loadable ? DLSS_RUNTIME_NOT_ASCII : DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&s.params)) || !s.params) return false;
    logf("vk: NVIDIA DLSS: NGX started (DLSS's runtime: the newest %s in %s)", kRuntime, where.c_str());
    s.ngx_ok = true;
    g_why = DLSS_OK;
    return true;
}

/// DLSS's preset: the DLSS model row's, RX_VK_DLSS_PRESET over it (upscaler_rows.h dlss_preset)
unsigned wanted_preset() {
    static const char* const env = getenv("RX_VK_DLSS_PRESET");
    return upscaler_rows::dlss_preset(opts.dlss_model, env && *env ? env : nullptr);
}

/// DLSS's feature for this output size and preset, made when either changes (cmd: its creation's work): the engine's
/// prepare (vk_temporal.h)
int prepare(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool native_aa,
            uint32_t* in_w, uint32_t* in_h, const char** fail) {
    State& s = st();
    const unsigned preset = wanted_preset();
    // (the game's native HDR: State::feature_hdr. What the game presents, not the option and the swapchain: with Native
    // picked and the game's picture not PQ, the scene is LDR, and an HDR feature would get LDR colour)
    // (RX_VK_DLSS_HDR=0: the LDR feature and the clamped colour with Native too, as before this; for comparisons)
    static const bool hdr_allowed = !getenv("RX_VK_DLSS_HDR") || atoi(getenv("RX_VK_DLSS_HDR")) != 0;
    const bool hdr = hdr_allowed && present_native_pq.load(std::memory_order_relaxed);
    if (s.feature && s.feature_ow == ow && s.feature_oh == oh && rw <= s.feature_rw && rh <= s.feature_rh &&
        rw >= s.min_rw && rh >= s.min_rh && s.feature_preset == preset && s.feature_hdr == hdr &&
        (!native_aa || s.feature_aa))
        return *in_w = s.feature_rw, *in_h = s.feature_rh, 1;
    // (why it is made: in the log with its time, so a stall in play can be matched with one)
    char why[160];
    if (!s.feature && !s.features_made) snprintf(why, sizeof why, "the first");
    else if (!s.feature) snprintf(why, sizeof why, "again after it was let go");
    else if (s.feature_hdr != hdr) snprintf(why, sizeof why, "the game's native HDR turned %s", hdr ? "on" : "off");
    else if (s.feature_ow != ow || s.feature_oh != oh)
        snprintf(why, sizeof why, "the output went from %ux%u to %ux%u", s.feature_ow, s.feature_oh, ow, oh);
    else if (s.feature_preset != preset)
        snprintf(why, sizeof why, "preset %c, was %c (the DLSS model row)", upscaler_rows::preset_letter(preset),
                 upscaler_rows::preset_letter(s.feature_preset));
    else if (native_aa && !s.feature_aa) snprintf(why, sizeof why, "the native anti-aliasing mode was selected");
    else
        snprintf(why, sizeof why, "the render size %ux%u left its range, %ux%u to %ux%u", rw, rh, s.min_rw, s.min_rh,
                 s.feature_rw, s.feature_rh);
    // (the quality mode whose range of render sizes holds this one: the network's 3/4 is DLSS's MaxQuality range; one it
    // misses by a little, a rounded size, takes it too: at most 1/64 of the range's largest size on each axis (2 pixels
    // at the least), dlss_checks.h mode_for. DLAA's when the render size is the output's)
    static const NVSDK_NGX_PerfQuality_Value scaled[] = {NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_DLAA,
                                                        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
                                                        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    const bool aa = rw == ow && rh == oh;
    NVSDK_NGX_PerfQuality_Value mode = NVSDK_NGX_PerfQuality_Value_DLAA;
    unsigned max_w = ow, max_h = oh, min_w = ow, min_h = oh;   // (DLAA: the render size is the output's, one size)
    if (!aa) {
        dlss_checks::ModeRange ranges[5];
        NVSDK_NGX_PerfQuality_Value which[5];
        int n = 0;
        for (const NVSDK_NGX_PerfQuality_Value m : scaled) {
            unsigned w = 0, h = 0, xw = 0, xh = 0, nw = 0, nh = 0;
            float sharp = 0;
            if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(s.caps, ow, oh, m, &w, &h, &xw, &xh, &nw, &nh, &sharp)) || !w) continue;
            ranges[n] = {nw, nh, xw, xh}, which[n++] = m;
        }
        dlss_checks::ModeRange took;
        const int k = dlss_checks::mode_for(rw, rh, ow, oh, ranges, n, &took);
        if (k < 0) {   // (the size in the log; the feature kept for the sizes it takes, not let go and made again)
            // (vk_temporal.cpp's fail() logs a reason again only when its pointer changes: two buffers, the other one for a
            // new size, so each refused size is logged once, by fail() as any reason)
            static char refused[2][112];
            static unsigned last[4];
            static int at = 0;
            if (last[0] != rw || last[1] != rh || last[2] != ow || last[3] != oh) {
                at ^= 1;
                snprintf(refused[at], sizeof refused[at], "no DLSS mode takes the render size %ux%u for %ux%u", rw, rh, ow, oh);
                last[0] = rw, last[1] = rh, last[2] = ow, last[3] = oh;
            }
            return *fail = refused[at], 0;
        }
        mode = which[k], max_w = took.max_w, max_h = took.max_h, min_w = took.min_w, min_h = took.min_h;
        if (took.max_w != ranges[k].max_w || took.max_h != ranges[k].max_h || took.min_w != ranges[k].min_w ||
            took.min_h != ranges[k].min_h)
            logf("vk: NVIDIA DLSS: the render size %ux%u is a little outside quality mode %d's range (%ux%u to %ux%u): taken by "
                 "it (a rounded size)", rw, rh, (int)mode, ranges[k].min_w, ranges[k].min_h, ranges[k].max_w, ranges[k].max_h);
    }
    // (the quality mode with them: the Render resolution row asks for another feature of the same output)
    const uint64_t key = (uint64_t)ow << 44 ^ (uint64_t)oh << 24 ^ (uint64_t)((int)mode & 7) << 8 ^ (uint64_t)preset << 1 ^
                         (hdr ? 1 : 0) ^ 1ull << 63;
    if (s.refused == key) return *fail = "its feature was not made", 0;
    if (s.feature) {   // (the old one, once a new one is to be made)
        NVSDK_NGX_Handle* old = s.feature;
        temporal_later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
        s.feature = nullptr;
    }
    // (the DLSS model row's preset for every mode, K by default: NVIDIA's transformer model. 0: NVIDIA's own per mode)
    static_assert(NVSDK_NGX_DLSS_Hint_Render_Preset_K == 11 && NVSDK_NGX_DLSS_Hint_Render_Preset_L == 12 &&
                  NVSDK_NGX_DLSS_Hint_Render_Preset_M == 13 && NVSDK_NGX_DLSS_Hint_Render_Preset_Default == 0);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(s.params, k, preset);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = max_w, cp.Feature.InHeight = max_h, cp.Feature.InTargetWidth = ow, cp.Feature.InTargetHeight = oh;
    cp.Feature.InPerfQualityValue = mode;
    // (LDR colour, already tonemapped: no exposure; motion at the render size, not jittered; depth not reversed. With the
    // game's native HDR the colour goes past 1: an HDR feature, given linear light, with DLSS's own exposure)
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
    const auto t0 = std::chrono::steady_clock::now();
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (NGX records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1, 1, &s.feature, s.params, &cp);
    const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    note_work();   // (NGX's own commands, which vk_work.h does not see: the next full_barrier is not left out)
    if (NVSDK_NGX_FAILED(r) || !s.feature) {
        logf("vk: NVIDIA DLSS: its feature was not made (%#x) for %ux%u -> %ux%u", (unsigned)r, max_w, max_h, ow, oh);
        s.feature = nullptr;
        // (a transient one too, out of video memory while menu switches had old features still waiting to be let go: the
        // game's upscale until a DLSS setting changes, not for the rest of the run)
        s.refused = key;
        return *fail = "its feature was not made", 0;
    }
    s.feature_rw = max_w, s.feature_rh = max_h, s.feature_ow = ow, s.feature_oh = oh, s.min_rw = min_w, s.min_rh = min_h;
    s.feature_preset = preset;
    s.feature_hdr = hdr;
    s.refused = 0;
    s.feature_aa = mode == NVSDK_NGX_PerfQuality_Value_DLAA;
    s.features_made++;
    logf("vk: NVIDIA DLSS: feature %llu made (%s; frame %llu, %.1f ms of CPU): %ux%u (%ux%u to %ux%u) -> %ux%u, mode %d, "
         "preset %c%s%s", (unsigned long long)s.features_made, why, (unsigned long long)texture_frame_no(), made_ms, rw, rh,
         min_w, min_h, max_w, max_h, ow, oh, (int)mode, upscaler_rows::preset_letter(preset),
         preset ? "" : " (NVIDIA's default)", hdr ? ", HDR colour, linear (the game's native HDR)" : "");
    *in_w = s.feature_rw, *in_h = s.feature_rh;
    return 2;   // (a new history)
}

bool start() { return ngx_init(); }

void release();
bool evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {
    State& s = st();
    NVSDK_NGX_Resource_VK color = resource(f.color, false), depth = resource(f.depth, false), mv = resource(f.motion, false),
                          output = resource(f.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color, ep.Feature.pInOutput = &output;
    ep.pInDepth = &depth, ep.pInMotionVectors = &mv;
    ep.InJitterOffsetX = f.jitter[0], ep.InJitterOffsetY = f.jitter[1];   // (render pixels, x right and y down)
    ep.InRenderSubrectDimensions = {f.render_w, f.render_h};
    ep.InReset = f.reset ? 1 : 0;
    ep.InMVScaleX = ep.InMVScaleY = 1.0f;
    // (what the scene's transparent draws changed, when RX_VK_REACTIVE asks for it and there were any (vk_reactive.h):
    // DLSS's mask of where to lean on this frame's colour, as large as the chain's inputs. Without one the parameter stays
    // null, as before)
    NVSDK_NGX_Resource_VK bias{};
    if (reactive::Mask m; reactive::mask(cmd, f.render_w, f.render_h, f.color.width, f.color.height, f.color.view, m)) {
        bias = NVSDK_NGX_Create_ImageView_Resource_VK(m.view, m.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, m.format,
                                                      m.image_width, m.image_height, false);
        ep.pInBiasCurrentColorMask = &bias;
    }
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (NGX records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &ep);
    note_work();   // (as at its creation: the barrier after it, and the game's next one, are recorded)
    if (NVSDK_NGX_FAILED(r)) {
        // (the chain tries again at the next frame, kEngine's retry: one that keeps failing says so when its result changes
        // and every 600th time, as the chain's own reason does, not on every frame)
        if (r != s.eval_result || ++s.eval_repeats % 600 == 0)
            logf("vk: NVIDIA DLSS: its evaluation failed (%#x)", (unsigned)r);
        if (r != s.eval_result) s.eval_result = r, s.eval_repeats = 0;
        // (half a second of them in a row: the feature is let go once, and prepare makes a new one for the next frame)
        if (s.eval_repeats == 30) {
            logf("vk: NVIDIA DLSS: 30 evaluations in a row failed: its feature is made again");
            release();
        }
        return false;
    }
    s.eval_result = NVSDK_NGX_Result_Success;
    return true;
}

void release() {
    State& s = st();
    s.refused = 0;   // (another engine chosen, or the chain idle: DLSS asked again makes its feature again)
    if (!s.feature) return;
    NVSDK_NGX_Handle* old = s.feature;
    temporal_later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
    s.feature = nullptr;
    s.feature_rw = s.feature_rh = s.feature_ow = s.feature_oh = s.min_rw = s.min_rh = 0;
}

uint64_t memory() {
    State& s = st();
    unsigned long long ngx = 0;
    return s.caps && NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(s.caps, &ngx)) ? (uint64_t)ngx : 0;
}

/// Its feature takes HDR colour (made for the game's native HDR): the chain gives it linear light, unclamped
bool hdr() {
    State& s = st();
    return s.feature && s.feature_hdr;
}

const TemporalEngine kEngine{"NVIDIA DLSS", "DLSS", start, prepare, evaluate, release, memory, hdr, false, true};

}  // namespace

bool dlss_built() { return true; }
const TemporalEngine* dlss_engine() { return &kEngine; }

void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (env_switch("RX_VK_DLSS") == 0) return;
    for (const auto& e : all)
        if (!strcmp(e.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            want.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            return;
        }
}

void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    // (NGX's: VK_NVX_binary_import and VK_NVX_image_view_handle, with push descriptors and buffer device addresses,
    // which the replay enables already: core 1.2's bufferDeviceAddress stands for VK_EXT_buffer_device_address. The NVX ones
    // are on whenever the device has them, so the Insert menu's row needs no restart)
    if (env_switch("RX_VK_DLSS") == 0) return;
    bool both = true;
    for (const char* name : {VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME}) {
        bool have = false, listed = false;
        for (const auto& e : all) have |= !strcmp(e.extensionName, name);
        for (const char* w : want) listed |= !strcmp(w, name);
        if (have && !listed) want.push_back(name);
        both &= have;
    }
    // (NVIDIA's own extensions: without them this is not an NVIDIA GPU, or not one NGX runs on. With them, NGX says, in
    // dlss_probe or at the first upscale: a GTX has them too)
    if (!both) g_why = DLSS_NEEDS_RTX;
}

void dlss_retry() { st().refused = 0; }

void dlss_probe() {
    if (dlss_why() != DLSS_OK || g_why.load(std::memory_order_relaxed) >= 0 || !dev.device) return;
    ngx_init();
}

// ---- NVIDIA DLSS Frame Generation through NGX (no Streamline) ----
//
// The renderer's frame generation (vk_framegen.cpp) keeps showing and pacing the frames; DLSS-G makes them, from the window's
// picture and the temporal chain's depth, motion (with the actor motion in it, vk_temporal.cpp) and camera, which are copied
// at the chain's evaluation (dlss_fg_inputs) and handed to NGX at the present (dlss_fg_frames). The Insert menu's Frame
// generation row's 2x and up say how many frames are made (its Auto stays the renderer's own), its Frame generation by row
// whether DLSS-G makes them (Auto, the default) or the renderer's own; RX_VK_DLSS_FG=0 / 1 over it for a run. Frame
// generation's device extensions are asked of NGX before the device exists whenever DLSS may run, so the row needs no
// restart. Its runtime, nvngx_dlssg.dll (libnvidia-ngx-dlssg.so), is looked for where DLSS's is (runtime_dirs). Where it
// does not make a set, the renderer's own methods do, and the log says every 600 presents how many sets DLSS-G made and why
// it did not make the others.

namespace {

struct FgImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct FgHint {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* mapped = nullptr;
    bool pending = false;
};

struct Fg {
    std::mutex mu;
    bool tried = false, ok = false;   // (NGX's answer: once)
    unsigned most = 1;                // (the most frames it makes between two of the game's: MultiFrameCountMax)
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t fw = 0, fh = 0;          // (the picture its feature was made for)
    int create_failures = 0;
    int refusals = 0;                 // (sets NGX refused in a row: kFgRefusalsOff of them give frame generation up)
    bool closed = false;
    // (read without mu, each present: a feature held (dlss_fg_idle), and NGX's answer a no or its feature given up after
    // three failed creations or kFgRefusalsOff refused sets in a row (dlss_fg_runs))
    std::atomic<bool> held{false}, start_failed{false};
    // (read without mu, each present: `most` once NGX has answered (fg_start), 0 before. A multiplier that asks for more is
    // the renderer's own to make (dlss_fg_frames), so the swapchain is not made for DLSS-G: dlss_fg_runs)
    std::atomic<unsigned> most_known{0};
    // this frame's inputs (dlss_fg_inputs): the depth and motion copied, the camera NGX's way
    FgImage depth, motion;
    uint32_t rw = 0, rh = 0;
    float view_to_clip[16], clip_to_view[16], to_prev[16], from_prev[16];
    float pos[3], up[3], right[3], fwd[3];
    float jitter[2]{}, near_z = 0.1f, far_z = 1000.0f, fov = 1.0f, aspect = 16.0f / 9.0f;
    bool inputs_reset = true;
    bool depth_reversed = false;      // (the chain's depth is 1 - d: FSR 3.1 on the chain, TemporalFrame::depth_reversed)
    float last_vp[16]{};
    bool have_last = false;
    uint64_t inputs = 0, used = 0;    // (how many inputs were taken, and the last one a present used)
    uint64_t last_sequence = 0;       // (the present sequence NGX last made a set at: one between them is a new history)
    // (every 600 presents: what made each set)
    uint64_t presents = 0, made = 0, no_inputs = 0, refused = 0, too_many = 0, not_blended = 0, resets = 0, still = 0,
             no_interp = 0;
    uint64_t sets_made = 0;   // (all the run's)
    FgHint hint[4];           // (NGX's "do not show this one" byte, a ring)
};
Fg& fgs() {
    static Fg f;
    return f;
}

/// Whether DLSS-G may run in this process at all: not with DLSS off for the run (RX_VK_DLSS=0) or RX_VK_DLSS_FG=0
bool fg_possible() {
    static const bool yes = env_switch("RX_VK_DLSS_FG") != 0 && env_switch("RX_VK_DLSS") != 0;
    return yes;
}
/// Whether it is asked for now: RX_VK_DLSS_FG=1 for the run, else the Frame generation by row (read each time: it changes
/// while the game runs)
bool fg_asked() {
    static const bool forced = env_switch("RX_VK_DLSS_FG") > 0;
    return fg_possible() && (forced || opts.framegen_by == 0);
}

/// A 4x4 inverse in doubles (a float one of a projection with a far plane in the thousands loses what reprojection needs)
bool fg_inverse(const float* m, float* out) {
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det > -1e-30 && det < 1e-30) return false;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] / det);
    return true;
}
void fg_mul(const float* a, const float* b, float* out) {   // (row major: out = a * b)
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) sum += (double)a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = (float)sum;
        }
}

/// NGX's answer for frame generation, once (under g.mu): its runtime found, this GPU and driver able, how many frames it
/// makes at the most. The reason is logged when not
bool fg_start(Fg& g) {
    if (g.tried) return g.ok;
    g.tried = true;
    g.start_failed = true;   // (until NGX says yes, below)
    // (not on a device DLSS does not run on: an AMD or Intel GPU, without NVIDIA's extensions (dlss_device_extensions), does
    // not start NGX at its first present)
    if (dlss_why() != DLSS_OK) {
        logf("vk: DLSS Frame Gen (NGX): DLSS does not run on this device (%d): off", (int)dlss_why());
        return false;
    }
    if (!ngx_init()) {
        logf("vk: DLSS Frame Gen (NGX): NGX did not start (above): off");
        return false;
    }
    State& s = st();
    int available = 0, needs_driver = 0, init = 0;
    unsigned major = 0, minor = 0, most = 0;
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &minor);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &most);
    if (!available) {
        if (needs_driver) logf("vk: DLSS Frame Gen (NGX): needs NVIDIA's driver %u.%u or newer: off", major, minor);
        else
            logf("vk: DLSS Frame Gen (NGX): not available (%#x): nvngx_dlssg.dll is looked for where DLSS's runtime is (the game "
                 "folder's ngx first), and it needs an RTX 40 or newer: off", (unsigned)init);
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&g.params)) || !g.params) {
        logf("vk: DLSS Frame Gen (NGX): no parameter block: off");
        return false;
    }
    g.most = most ? most : 1;
    // (RX_VK_DLSS_FG_MOST=<n>, for checks: no more than n, as an older runtime that makes 1 frame at most)
    if (const char* cap = getenv("RX_VK_DLSS_FG_MOST"); cap && atoi(cap) >= 1 && (unsigned)atoi(cap) < g.most)
        g.most = (unsigned)atoi(cap);
    // (its runtime: NGX takes the newest nvngx_dlssg in the runtime folders, else the driver's own copy, which may be older
    // and make 1 frame at most, or refuse every set. scripts/ngx_sdk.py puts the SDK's beside DLSS's runtime)
    // (Linux: NGX loads it by its real path, ASCII only, as DLSS's runtime: one only in folders named otherwise is not used)
    std::string where;
    bool here = false, loadable = false;
    std::error_code ec;
    for (const auto& d : runtime_dirs(game_dir())) {
        where += (where.empty() ? "" : ", ") + shown(d);
        for (const auto& f : fs::directory_iterator(d, ec))
            if (f.path().filename().native().rfind(kFgRuntimeName, 0) == 0) {
                here = true;
#ifdef _WIN32
                loadable = true;
#else
                std::error_code real;
                loadable |= dlss_checks::ascii(fs::canonical(f.path(), real).native()) && !real;
#endif
            }
    }
    const std::string runtime =
        loadable ? "its runtime: the newest " + std::string(kFgRuntime) + " in " + where
        : here   ? std::string(kFgRuntime) + " only in folders whose path is not all ASCII (" + where +
                     "), which NGX on Linux does not load from: the driver's own, which may be older"
                 : "no " + std::string(kFgRuntime) + " in " + where +
                     ": the driver's own, which may be older (scripts/ngx_sdk.py fetches it)";
    logf("vk: DLSS Frame Gen (NGX): available, up to %u made frames between two of the game's (%s)", g.most, runtime.c_str());
    g.ok = true;
    g.most_known = g.most;
    g.start_failed = false;
    return true;
}

/// One of its own images (the depth or motion copy), made again at another size; the old one freed once work in flight is done
bool fg_image(VkCommandBuffer cmd, FgImage& im, VkFormat format, uint32_t w, uint32_t h) {
    if (im.image && im.w == w && im.h == h && im.format == format) return true;
    if (im.image) {
        const FgImage old = im;
        temporal_later([old] {
            vkDestroyImageView(dev.device, old.view, nullptr);
            destroy_image(old.image, old.alloc);
        });
        im = {};
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(ci, im.image, im.alloc)) return im = {}, false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vi, nullptr, &im.view) != VK_SUCCESS) {
        destroy_image(im.image, im.alloc);
        return im = {}, false;
    }
    im.w = w, im.h = h, im.format = format;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    b.subresourceRange = vi.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

void fg_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &di);
}

/// NGX's matrices are rows of a row-vector convention (position * M, as Streamline's float4x4): the game's, transposed
void fg_put(float out[4][4], const float* m) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r][c] = m[c * 4 + r];
}

/// The feature let go: the queue idle first (it may be in a set the display thread has not shown yet)
void fg_release(Fg& g) {
    if (!g.feature) return;
    {
        std::lock_guard qk(dev.present_mu);
        device_wait_idle_locked();
    }
    NVSDK_NGX_VULKAN_ReleaseFeature(g.feature);
    g.feature = nullptr;
    g.held = false;
    g.fw = g.fh = 0;
}

// (hookshot) Why the last set DLSS-G was asked for went to the renderer's own (dlss_fg_skip); none when DLSS-G made it.
// A set with nothing to blend from (a loading hitch) leaves it as it was
std::atomic<int> fg_skip{FG_SKIP_NONE};
std::atomic<unsigned> fg_skip_most{0};
int skip(int why) {
    fg_skip.store(why, std::memory_order_relaxed);
    return 0;
}

}  // namespace

int dlss_fg_skip(unsigned* most) {
    if (most) *most = fg_skip_most.load(std::memory_order_relaxed);
    return fg_skip.load(std::memory_order_relaxed);
}

bool dlss_fg_wanted() { return fg_asked(); }

bool dlss_fg_runs(int frames) {
    if (!fg_asked() || dlss_why() != DLSS_OK) return false;
    const Fg& g = fgs();
    if (g.start_failed.load(std::memory_order_relaxed)) return false;
    // (more frames a set than its runtime makes: the renderer's own makes them all (dlss_fg_frames' too_many). Known once
    // NGX has answered, at the first present with a fixed multiplier; until then it is taken to make them)
    const unsigned most = g.most_known.load(std::memory_order_relaxed);
    return !most || frames <= (int)most;
}

void dlss_fg_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (!fg_possible() || !dev.instance || !dev.physical) return;   // (whenever it may run: the row then needs no restart)
    // (only on a device DLSS may run on: without NVIDIA's NVX extensions (dlss_device_extensions' check) NGX is not asked, so
    // an AMD or Intel GPU's device is made as without DLSS-G)
    for (const char* name : {VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME}) {
        bool have = false;
        for (const auto& e : all) have |= !strcmp(e.extensionName, name);
        if (!have) return;
    }
    const fs::path game = game_dir();
    if (game.empty() || game.is_relative()) return;
    const std::vector<fs::path> dirs = runtime_dirs(game);
    std::vector<std::wstring> wdirs;
    for (const auto& d : dirs) wdirs.push_back(wide(d));
    std::vector<const wchar_t*> paths;
    for (const auto& d : wdirs) paths.push_back(d.c_str());
    const std::wstring wdata = wide(game / "ngx");
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths.data();
    info.PathListInfo.Length = (unsigned)paths.size();
    NVSDK_NGX_FeatureDiscoveryInfo d{};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    d.ApplicationDataPath = wdata.c_str();
    d.FeatureInfo = &info;
    uint32_t n = 0;
    VkExtensionProperties* props = nullptr;
    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(dev.instance, dev.physical, &d, &n, &props);
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: DLSS Frame Gen (NGX): NGX did not say which device extensions it needs (%#x)", (unsigned)r);
        return;
    }
    static std::deque<std::string> names;   // (the names vkCreateDevice is given: kept for the process's life)
    std::string added, missing;
    for (uint32_t i = 0; i < n && props; i++) {
        const char* name = props[i].extensionName;
        bool have = false, listed = false;
        for (const auto& e : all) have |= !strcmp(e.extensionName, name);
        for (const char* w : want) listed |= !strcmp(w, name);
        if (!have) {
            missing += (missing.empty() ? "" : ", ") + std::string(name);
            continue;
        }
        if (!listed) want.push_back(names.emplace_back(name).c_str());
        added += (added.empty() ? "" : ", ") + std::string(name);
    }
    logf("vk: DLSS Frame Gen (NGX): device extensions: %s%s%s", added.empty() ? "none" : added.c_str(),
         missing.empty() ? "" : "; the device lacks: ", missing.c_str());
}

void dlss_fg_inputs(VkCommandBuffer cmd, const TemporalFrame& f, const float projection[16], const float view_projection[16],
                    const float clip_jitter[2], bool motion) {
    if (!fg_asked()) return;
    Fg& g = fgs();
    std::lock_guard lk(g.mu);
    if (g.closed || !fg_start(g)) return;
    if (opts.framegen < 2) return void(g.have_last = false);   // (not a fixed multiplier: the next inputs start a history)
    const uint32_t rw = f.render_w, rh = f.render_h;
    if (!f.depth.image || !f.motion.image || !rw || !rh) return;
    // the camera without the jitter: view = P^-1 * VP, the camera's frame its inverse
    float p[16], vp[16], ip[16], view[16], cam[16];
    std::memcpy(p, projection, sizeof p), std::memcpy(vp, view_projection, sizeof vp);
    dlss_math::unjitter(p, clip_jitter), dlss_math::unjitter(vp, clip_jitter);
    if (!fg_inverse(p, ip)) return;
    fg_mul(ip, vp, view);
    if (!fg_inverse(view, cam)) return;
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const bool history = g.have_last && motion && !f.reset;
    float to_prev[16], from_prev[16];
    if (!history || !dlss_math::reprojection(vp, g.last_vp, to_prev)) std::memcpy(to_prev, id, sizeof id);
    if (!fg_inverse(to_prev, from_prev)) return;
    std::memcpy(g.last_vp, vp, sizeof vp), g.have_last = true;
    std::memcpy(g.view_to_clip, p, sizeof p), std::memcpy(g.clip_to_view, ip, sizeof ip);
    std::memcpy(g.to_prev, to_prev, sizeof to_prev), std::memcpy(g.from_prev, from_prev, sizeof from_prev);
    for (int i = 0; i < 3; i++) {   // (the view's inverse: OpenGL's camera, looking down -z)
        g.pos[i] = cam[i * 4 + 3];
        g.right[i] = cam[i * 4 + 0];
        g.up[i] = cam[i * 4 + 1];
        g.fwd[i] = -cam[i * 4 + 2];
    }
    g.jitter[0] = f.jitter[0], g.jitter[1] = f.jitter[1];
    g.depth_reversed = f.depth_reversed;
    g.near_z = f.near_z > 0 ? f.near_z : 0.1f;
    g.far_z = f.far_z > g.near_z ? f.far_z : 100000.0f;
    g.fov = f.fov_y > 0 ? f.fov_y : 1.0f;
    g.aspect = p[0] != 0 ? std::fabs(p[5] / p[0]) : 16.0f / 9.0f;
    g.inputs_reset |= !history;
    // the depth and the motion, the frame's part (the chain's images are as large as its engine asked): copied, so the
    // present reads this frame's whatever the chain does next
    if (!fg_image(cmd, g.depth, VK_FORMAT_R32_SFLOAT, rw, rh) || !fg_image(cmd, g.motion, VK_FORMAT_R16G16_SFLOAT, rw, rh)) return;
    fg_barrier(cmd);
    const VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                        {rw, rh, 1}};
    vkCmdCopyImage(cmd, f.depth.image, VK_IMAGE_LAYOUT_GENERAL, g.depth.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vkCmdCopyImage(cmd, f.motion.image, VK_IMAGE_LAYOUT_GENERAL, g.motion.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    fg_barrier(cmd);
    g.rw = rw, g.rh = rh;
    g.inputs++;
    static bool told;
    if (!told) {
        told = true;
        logf("vk: DLSS Frame Gen (NGX): inputs %ux%u (near %.3f far %.1f fov %.1f deg)", rw, rh, g.near_z, g.far_z,
             g.fov * 57.29578f);
    }
}

int dlss_fg_frames(VkCommandBuffer cmd, VkImage color, VkImageView color_view, uint32_t w, uint32_t h, int count,
                   const VkImage* out, const VkImageView* out_views, bool blend, uint64_t sequence) {
    if (!fg_possible()) return skip(FG_SKIP_OFF);
    Fg& g = fgs();
    std::lock_guard lk(g.mu);
    if (g.closed) return 0;
    if (!fg_asked()) {   // (turned off in the menu: its feature let go, its memory back)
        if (g.feature) fg_release(g), logf("vk: DLSS Frame Gen (NGX): off (Frame generation by): its feature let go");
        return 0;
    }
    if (++g.presents % 600 == 0) {
        logf("vk: DLSS Frame Gen (NGX): of the last 600 presents %llu made by DLSS-G (%llu told to reset, %llu with the camera "
             "still, %llu NGX said not to show); the renderer's own made the rest: %llu without new inputs (no DLSS/DLAA this "
             "frame), %llu after a gap, %llu asked more than %u frames, %llu refused",
             (unsigned long long)g.made, (unsigned long long)g.resets, (unsigned long long)g.still,
             (unsigned long long)g.no_interp,
             (unsigned long long)g.no_inputs, (unsigned long long)g.not_blended, (unsigned long long)g.too_many, g.most,
             (unsigned long long)g.refused);
        g.made = g.no_inputs = g.not_blended = g.too_many = g.refused = g.resets = g.still = g.no_interp = 0;
    }
    if (!fg_start(g) || count < 1) return skip(dlss_why() != DLSS_OK ? FG_SKIP_DEVICE : FG_SKIP_START);
    if (g.refusals >= kFgRefusalsOff) {   // (given up: dlss_fg_runs says no from here)
        // (the refused sets' feature and its memory, let go once: vk_framegen.cpp still calls at 2x and up)
        if (g.feature) fg_release(g);
        return g.refused++, skip(FG_SKIP_REFUSED);
    }
    if (!blend) return g.not_blended++, 0;
    if ((unsigned)count > g.most) {
        static bool told = false;   // (once a run: the 600-present lines count them)
        if (!told) {
            told = true;
            logf("vk: DLSS Frame Gen (NGX): %d made frames asked, but its runtime makes %u at most (an older %s?): the "
                 "renderer's own makes them, and V-Sync Auto keeps its mode without DLSS-G", count, g.most, kFgRuntime);
        }
        fg_skip_most.store(g.most, std::memory_order_relaxed);
        return g.too_many++, skip(FG_SKIP_TOO_MANY);
    }
    if (g.inputs == g.used || !g.rw) return g.no_inputs++, skip(FG_SKIP_INPUTS);
    g.used = g.inputs;
    if (g.feature && (g.fw != w || g.fh != h)) fg_release(g);
    if (!g.feature) {
        if (g.create_failures >= 3) return g.refused++, skip(FG_SKIP_REFUSED);
        NVSDK_NGX_DLSSG_Create_Params cp{};
        cp.Width = w, cp.Height = h;
        cp.NativeBackbufferFormat = VK_FORMAT_R8G8B8A8_UNORM;   // (the window's picture, vk_framegen.cpp's frames)
        cp.RenderWidth = g.rw, cp.RenderHeight = g.rh;
        // (frame generation's own size keys, which NGX takes over the generic ones the helper sets: it warned about those)
        NVSDK_NGX_Parameter_SetUI(g.params, NVSDK_NGX_DLSSG_Parameter_Width, w);
        NVSDK_NGX_Parameter_SetUI(g.params, NVSDK_NGX_DLSSG_Parameter_Height, h);
        NVSDK_NGX_Parameter_SetUI(g.params, NVSDK_NGX_DLSSG_Parameter_InternalWidth, g.rw);
        NVSDK_NGX_Parameter_SetUI(g.params, NVSDK_NGX_DLSSG_Parameter_InternalHeight, g.rh);
        NVSDK_NGX_Parameter_SetUI(g.params, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
        ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (NGX records into cmd itself)
        const NVSDK_NGX_Result r = NGX_VK_CREATE_DLSSG(cmd, 1, 1, &g.feature, g.params, &cp);
        note_work();
        if (NVSDK_NGX_FAILED(r) || !g.feature) {
            g.feature = nullptr;
            g.create_failures++;
            logf("vk: DLSS Frame Gen (NGX): the feature was not made for %ux%u (%#x)%s", w, h, (unsigned)r,
                 g.create_failures >= 3 ? ": off" : "");
            // (given up: dlss_fg_runs says no from here, so V-Sync Auto goes back to its mode without DLSS-G)
            if (g.create_failures >= 3) g.start_failed = true;
            return g.refused++, skip(FG_SKIP_REFUSED);
        }
        g.fw = w, g.fh = h;
        g.held = true;
        g.last_sequence = 0;
        logf("vk: DLSS Frame Gen (NGX): feature made for %ux%u from %ux%u inputs, %d made frame(s) a set", w, h, g.rw, g.rh,
             count);
    }
    const VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NVSDK_NGX_Resource_VK back = NVSDK_NGX_Create_ImageView_Resource_VK(color_view, color, one, VK_FORMAT_R8G8B8A8_UNORM, w, h, false);
    NVSDK_NGX_Resource_VK depth = NVSDK_NGX_Create_ImageView_Resource_VK(g.depth.view, g.depth.image, one, g.depth.format, g.rw,
                                                                         g.rh, false);
    NVSDK_NGX_Resource_VK mvecs = NVSDK_NGX_Create_ImageView_Resource_VK(g.motion.view, g.motion.image, one, g.motion.format,
                                                                         g.rw, g.rh, false);
    NVSDK_NGX_DLSSG_Opt_Eval_Params o{};
    fg_put(o.cameraViewToClip, g.view_to_clip);
    fg_put(o.clipToCameraView, g.clip_to_view);
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    fg_put(o.clipToLensClip, id);
    fg_put(o.clipToPrevClip, g.to_prev);
    fg_put(o.prevClipToClip, g.from_prev);
    o.jitterOffset[0] = g.jitter[0], o.jitterOffset[1] = g.jitter[1];
    // (the motion as it is, render pixels: scale 1, as DLSS's own evaluation takes it. nvsdk_ngx_defs_dlssg.h says the scale
    // normalizes to [-1, 1], which is Streamline's convention: with 1 / render size here DLSS-G saw no motion and every made
    // frame was a cross-fade of the two game frames (captured 2026-11-10: t = 1/3 and 2/3, 0.2 of 255 off a pure blend); with
    // scale 1 the same scenes came out as moved pictures between them. RX_VK_DLSS_FG_MV_SCALE=<x> multiplies it, for checks)
    static const float mv_scale = getenv("RX_VK_DLSS_FG_MV_SCALE") ? (float)atof(getenv("RX_VK_DLSS_FG_MV_SCALE")) : 1.0f;
    o.mvecScale[0] = o.mvecScale[1] = mv_scale;
    o.cameraPinholeOffset[0] = o.cameraPinholeOffset[1] = 0.0f;
    std::memcpy(o.cameraPos, g.pos, sizeof g.pos), std::memcpy(o.cameraUp, g.up, sizeof g.up);
    std::memcpy(o.cameraRight, g.right, sizeof g.right), std::memcpy(o.cameraFwd, g.fwd, sizeof g.fwd);
    o.cameraNear = g.near_z, o.cameraFar = g.far_z, o.cameraFOV = g.fov, o.cameraAspectRatio = g.aspect;
    o.colorBuffersHDR = false;
    o.depthInverted = g.depth_reversed;   // (the chain's depth: [0, 1], reversed for FSR 3.1 on the chain: depth_reversed)
    o.cameraMotionIncluded = true;      // (its motion is the whole of it: camera and actors)
    // (a new history after a present NGX did not make, a cut the chain saw, or a new feature)
    o.reset = g.inputs_reset || !g.last_sequence || sequence != g.last_sequence + 1;
    g.resets += o.reset;
    {   // (the camera's clip to the frame before's: identity when it did not move, or when the chain had no frame before)
        bool id_to_prev = true;
        for (int i = 0; i < 16; i++) id_to_prev &= std::fabs(g.to_prev[i] - ((i % 5) == 0 ? 1.0f : 0.0f)) < 1e-6f;
        g.still += id_to_prev;
    }
    o.automodeOverrideReset = false;
    o.notRenderingGameFrames = false;
    o.orthoProjection = false;
    // (no value stands for "no motion here": RX_VK_DLSS_FG_INVALID=0 makes zero motion that, as PR 1944's Streamline path has it)
    static const bool zero_invalid = env_switch("RX_VK_DLSS_FG_INVALID") == 0;
    o.motionVectorsInvalidValue = zero_invalid ? 0.0f : FLT_MAX;
    o.motionVectorsDilated = false;
    o.menuDetectionEnabled = false;
    o.multiFrameCount = (unsigned)count;
    NVSDK_NGX_VK_DLSSG_Eval_Params ep{};
    ep.pBackbuffer = &back, ep.pDepth = &depth, ep.pMVecs = &mvecs;
    NVSDK_NGX_Parameter_SetULL(g.params, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, sequence);
    // (NGX's hint that this set should not be shown: a byte it writes, read back a few sets later for the count)
    NVSDK_NGX_Resource_VK no_interp{};
    if (!g.hint[0].buffer)
        for (auto& b : g.hint)
            if (!create_buffer_cached(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, b.buffer, b.alloc,
                                      &b.mapped))
                b = {};
    FgHint& hint = g.hint[g.sets_made % 4];
    if (hint.buffer) {
        const FgHint& old = g.hint[(g.sets_made + 1) % 4];   // (written 3 sets ago: done by now)
        if (old.pending) {
            invalidate_mapped(old.alloc, 16);
            g.no_interp += ((const uint8_t*)old.mapped)[0] != 0;
        }
        vkCmdFillBuffer(cmd, hint.buffer, 0, 16, 0);
        hint.pending = true;
        no_interp = NVSDK_NGX_Create_Buffer_Resource_VK(hint.buffer, 16, true);
        ep.pOutputDisableInterpolation = &no_interp;
    }
    fg_barrier(cmd);
    for (int k = 1; k <= count; k++) {
        NVSDK_NGX_Resource_VK made = NVSDK_NGX_Create_ImageView_Resource_VK(out_views[k - 1], out[k - 1], one,
                                                                            VK_FORMAT_R8G8B8A8_UNORM, w, h, true);
        ep.pOutputInterpFrame = &made;
        o.multiFrameIndex = (unsigned)k;
        ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (NGX records into cmd itself)
        const NVSDK_NGX_Result r = NGX_VK_EVALUATE_DLSSG(cmd, g.feature, g.params, &ep, &o);
        note_work();
        if (NVSDK_NGX_FAILED(r)) {
            static int told = 0;
            if (told++ < 8) logf("vk: DLSS Frame Gen (NGX): frame %d of %d was refused (%#x)", k, count, (unsigned)r);
            g.last_sequence = 0;
            // (frames 1..k-1 are NGX's writes in this command buffer: done before the renderer's own write the same images)
            if (k > 1) fg_barrier(cmd);
            if (++g.refusals == kFgRefusalsOff) {
                g.start_failed = true;   // (dlss_fg_runs says no: V-Sync Auto goes back to its mode without DLSS-G)
                logf("vk: DLSS Frame Gen (NGX): %d sets in a row refused (%#x): off, the renderer's own makes the frames (an "
                     "older %s?)", kFgRefusalsOff, (unsigned)r, kFgRuntime);
            }
            return g.refused++, skip(FG_SKIP_REFUSED);   // (a half-made set: the renderer's own makes all of it)
        }
    }
    fg_barrier(cmd);
    g.refusals = 0;
    g.inputs_reset = false;
    g.last_sequence = sequence;
    g.made++;
    g.sets_made++;
    skip(FG_SKIP_NONE);
    return count;
}

void dlss_fg_idle() {
    Fg& g = fgs();
    if (!g.held.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(g.mu);
    if (g.feature) fg_release(g), logf("vk: DLSS Frame Gen (NGX): frame generation Off or Auto: its feature let go");
}

void dlss_fg_close() {
    if (!fg_asked()) return;
    Fg& g = fgs();
    std::lock_guard lk(g.mu);
    if (g.closed) return;
    g.closed = true;
    if (!g.feature && !g.params) return;
    // (the swapchain's lock for the queue's idle, waited for a while: a present stuck at the exit leaves NGX to the process's
    // end rather than hanging the exit, as PR 1944's Streamline shutdown found)
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::unique_lock qk(dev.present_mu, std::defer_lock);
    while (!qk.try_lock() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!qk.owns_lock()) {
        logf("vk: DLSS Frame Gen (NGX): the queue stayed busy at the exit: its feature left to the process's end");
        return;
    }
    device_wait_idle_locked();
    if (g.feature) NVSDK_NGX_VULKAN_ReleaseFeature(g.feature), g.feature = nullptr;
    if (g.params) NVSDK_NGX_VULKAN_DestroyParameters(g.params), g.params = nullptr;
    logf("vk: DLSS Frame Gen (NGX): let go for the exit");
}

#else  // (no NGX SDK in this build: DLSS is not there)

bool dlss_built() { return false; }
const TemporalEngine* dlss_engine() { return nullptr; }
void dlss_probe() {}
void dlss_retry() {}
void dlss_instance_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void dlss_device_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
bool dlss_fg_wanted() { return false; }
int dlss_fg_skip(unsigned* most) {
    if (most) *most = 0;
    return FG_SKIP_NOT_BUILT;
}
bool dlss_fg_runs(int) { return false; }
void dlss_fg_device_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void dlss_fg_inputs(VkCommandBuffer, const TemporalFrame&, const float*, const float*, const float*, bool) {}
int dlss_fg_frames(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t, int, const VkImage*, const VkImageView*, bool,
                   uint64_t) {
    return 0;
}
void dlss_fg_idle() {}
void dlss_fg_close() {}

#endif

} // namespace mkwvk
