// Authored SDL and thread stubs. Production startup code is extracted by the
// driver; no SDL library, OS window, renderer, game, or real thread is started.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define HOS_APP_ID "authored.app"
#define HOS_GAME_TITLE "authored window"
using HANDLE = void*;
constexpr int TRUE = 1, FALSE = 0;
constexpr unsigned INFINITE = ~0u;
constexpr unsigned HOS_SDL_VIDEO = 1, HOS_SDL_EVENTS = 2;
struct HosSdlEvent {};
struct ProcessExit { int code; };
static int sdl_ready, start_asked, visible_window_started;
static int game_argc, game_token, event_token;
static char** game_argv;
static int hos_posix_window[4];
[[maybe_unused]] static HANDLE renderer_up;
static void* window;
float hos_aspect_ratio = 16.0f / 9;
static std::string mode;
static unsigned loads, inits, windows, threads, offscreen, signals, sleeps, flushes, waited, prepares;
static std::vector<std::string> logs;
void hos_discord_start() {} // Inert optional IPC startup; the fixture exercises display startup only.

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void hos_log(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    logs.emplace_back(text);
}
[[maybe_unused]] static void hos_log_flush() { ++flushes; }
static const char* hos_sdl_error() { return "authored missing Vulkan loader"; }
static int hos_sdl_load() { ++loads; return mode == "load_failure" || mode.starts_with("prepare_") ? 0 : 3; }
static void hos_sdl_identity(const char*, const char*) {}
static void hos_sdl_hint(const char*, const char*) {}
static int hos_sdl_init(unsigned flags) {
    require(flags == (HOS_SDL_VIDEO | HOS_SDL_EVENTS), "changed SDL startup flags");
    ++inits;
    return mode != "init_failure";
}
static int hos_sdl_desktop_size(int* w, int* h) { *w = 2560; *h = 1440; return 1; }
static void* hos_sdl_window(const char*, int w, int h) {
    require(w == 1280 && h == 720, "changed default window size");
    ++windows;
    return mode == "window_failure" ? nullptr : &event_token;
}
static void hos_sdl_wait_event(HosSdlEvent*, int) {
    throw std::runtime_error("unexpected startup event wait");
}
static HANDLE CreateEventA(void*, int, int, void*) { return &event_token; }
[[maybe_unused]] static void SetEvent(HANDLE h) {
    require(h == &event_token, "wrong readiness event");
    ++signals;
}
[[noreturn]] static void Sleep(unsigned) {
    ++sleeps;
    throw std::runtime_error("startup entered the hidden-window sleep loop");
}
[[maybe_unused, noreturn]] static void ExitProcess(unsigned code) { throw ProcessExit{static_cast<int>(code)}; }
static int rx_start(void* hwnd) {
    require(!hwnd, "unexpected renderer start in authored fixture");
    ++offscreen;
    return 1;
}
void hos_display_start(void);
static unsigned game_thread(void*) {
    if (mode.starts_with("prepare_")) {
        // Authored builder worker: verify actual POSIX main forwards the flag,
        // and let its ordinary completion or no-Vulkan exit reach the caller.
        require(game_argc == 3 && !std::strcmp(game_argv[2], "--prepare-shaders"), "lost preparation flag");
        ++prepares;
        if (mode == "prepare_no_vulkan") throw ProcessExit{3};
        return 0;
    }
    hos_display_start();
    return 0;
}
static HANDLE CreateThread(void*, unsigned, unsigned (*entry)(void*), void* arg, unsigned, void*) {
    if (mode == "thread_failure") return nullptr;
    ++threads;
    entry(arg); // Deterministic startup handshake only; not a concurrency test.
    return &game_token;
}
static void SetThreadDescription(HANDLE h, const wchar_t*) { require(h == &game_token, "wrong game thread"); }
static void WaitForSingleObject(HANDLE h, unsigned timeout) {
    require(h == &game_token && timeout == INFINITE, "changed game-thread wait");
    ++waited;
}
static void load_hookshot_env() {}
static void driver_cache_room() {}   // (host/hos_main.c: the drivers' shader caches' size, #2033)
static void driver_cache_folder() {}   // (host/hos_main.c: the folder __GL_SHADER_DISK_CACHE_PATH names, made)

#include "display_startup_actual.inc"

static void headless_setting(const char* value) {
#ifdef _WIN32
    require(_putenv_s("HOS_HEADLESS", value ? value : "") == 0, "authored child environment failed");
    _putenv_s("HOS_WINDOW_SIZE", "");
#else
    require((value ? setenv("HOS_HEADLESS", value, 1) : unsetenv("HOS_HEADLESS")) == 0,
            "authored child environment failed");
    unsetenv("HOS_WINDOW_SIZE");
#endif
}

int main(int argc, char** argv) {
    mode = argc == 2 ? argv[1] : "";
    try {
        headless_setting(mode == "headless" ? "1" : mode == "headless_zero" ? "0" : nullptr);
        char prepare_flag[] = "--prepare-shaders";
        char* prepare_argv[] = {argv[0], argv[1], prepare_flag, nullptr};
        char romfs_flag[] = "--romfs", user_flag[] = "--user";
        char* path_argv[] = {argv[0], mode == "flag_as_romfs" ? romfs_flag : user_flag, prepare_flag, nullptr};
        const bool preparing = mode.starts_with("prepare_");
        const bool path_value = mode == "flag_as_romfs" || mode == "flag_as_user";
        const int app_argc = preparing || path_value ? 3 : argc;
        char** app_argv = preparing ? prepare_argv : path_value ? path_argv : argv;
        int status;
        try { status = app_main(app_argc, app_argv); }
        catch (const ProcessExit& exit) { status = exit.code; }
        require(game_argc == app_argc && game_argv == app_argv, "changed argument handoff");
        if (mode == "load_failure" || mode == "init_failure" || mode == "window_failure") {
            require(status == 1, "unexpected display failure did not exit nonzero");
            require(flushes == 1 && logs.size() == 1, "startup error not logged and flushed once");
            require(logs[0].find(mode == "load_failure" ? "no SDL" : hos_sdl_error()) != std::string::npos,
                    "missing actual startup error detail");
            require(!offscreen && !signals && !sleeps && !visible_window_started,
                    "unexpected startup failure continued offscreen or signaled success");
            require(threads == (mode == "window_failure" ? 1u : 0u),
                    "SDL initialization failure started game work");
        } else if (mode == "headless") {
            require(status == 0 && threads == 1 && offscreen == 1 && waited == 1,
                    "changed explicit headless/offscreen behavior");
            require(!loads && !inits && !windows && !visible_window_started && !flushes,
                    "explicit headless used the display");
        } else if (mode == "visible_success" || mode == "headless_zero" || path_value) {   // (HOS_HEADLESS=0: off, a window)
            require(status == 0 && threads == 1 && waited == 1 && loads && inits == 1 && windows == 1 &&
                    visible_window_started == 1 && !offscreen && !flushes && !sleeps,
                    "changed successful visible startup handshake");
        } else if (mode == "thread_failure") {
            require(status == 1 && !threads && !windows && !offscreen && !waited,
                    "changed game-thread creation failure behavior");
        } else if (preparing) {
            require(status == (mode == "prepare_no_vulkan" ? 3 : 0) && threads == 1 && prepares == 1,
                    "display startup intercepted the preparation worker's exit");
            require(!loads && !inits && !windows && !visible_window_started && !offscreen && !flushes,
                    "shader preparation tried to use the display");
        } else throw std::runtime_error("unknown fixture case");
        std::printf("PASS %s\n", mode.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s: %s\n", mode.c_str(), e.what());
        return 1;
    }
}
