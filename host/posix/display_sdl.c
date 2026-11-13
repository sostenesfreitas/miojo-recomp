/* (linux) The game's window on SDL (host/hos_sdl.c: SDL3, Wayland when there is one, else X11; or SDL2): what
   host/display.c is on Windows. The window lives on the process's main thread, as SDL wants it (hos_main.c runs the
   game on a thread of its own on Linux); the Vulkan back end makes its surface through SDL (gpu/vk/vk_api.h
   MkwVkWindow), on a thread of its own so the window keeps answering while the shaders are prepared.

   The keyboard's state goes to hos_posix_keys (GetAsyncKeyState, for nn_hid.c and the Insert menu), the window's focus
   and the screen's size to hos_posix_window (GetForegroundWindow, GetSystemMetrics).
   HOS_HEADLESS=1: no window. HOS_WINDOW_SIZE=<w>x<h>: its size (default 1280x720). F11: fullscreen (HOS_FULLSCREEN=1 to
   start so). Mouse: a click in the window captures it (relative motion, for nn_hid.c), Esc or leaving releases it.
   While the Insert menu is open the mouse is free (the cursor shown, for the menu's clicks: hos_mouse_pointer) and
   the game gets none of it; the capture it had comes back when the menu closes. */
#include "../hos.h"
#include "../hos_sdl.h"

/* (the window's app id: the applications menu's entry names it as its StartupWMClass, builder/linux/hookshot_builder.py) */
#define HOS_APP_ID "io.github.NotRexed.Hookshot"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../../gpu/vk/vk_api.h"
#include "../display_pointer.h"
#include <math.h>
#include <limits.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../../gpu/vk/compat/hookshot_env.h"
#endif

int rx_start(void *hwnd);
void rx_resize(int width, int height);
void rx_close_asked(void);
void hos_sdl_pads_pump(void);   /* host/pad_sdl.c: controllers, on this thread, as SDL's queue wants */
void hos_relaunch_ask(void);        /* (host/hos_relaunch.c: the Insert menu's Restart now) */
void hos_window_closed_exit(void);

volatile LONG64 hos_present_count;   /* (nvn_native.c: the game's presents) */
__attribute__((visibility("default"))) volatile uint8_t hos_posix_keys[256];
__attribute__((visibility("default"))) volatile int hos_posix_window[4];   /* focused, screen width, screen height, fullscreen */

static volatile LONG mouse_dx, mouse_dy, mouse_captured;
int hos_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = InterlockedExchange(&mouse_dx, 0);
    *dy = InterlockedExchange(&mouse_dy, 0);
    return mouse_captured;
}

/* (mouse_native.c) motion taken and not used, put back for the next taker (the player camera's) */
void hos_mouse_give_back(int32_t dx, int32_t dy) {
    InterlockedExchangeAdd(&mouse_dx, dx);
    InterlockedExchangeAdd(&mouse_dy, dy);
}

/* ---- the free mouse, for the Insert menu (gpu/vk/vk_api.h MkwVkPointer): set on the main thread, read on the renderer's ---- */
static HosPointer pointer = HOS_POINTER_INIT;
static int ptr_in;
static uint32_t ptr_held;
static int menu_open;   /* window-thread only */
static void *window;
int hos_mouse_pointer(MkwVkPointer *out) { return hos_pointer_take(&pointer, out); }

/* SDL's window/mouse queries belong on the main thread, never on the renderer's pointer callback. */
static void pointer_publish_at(float x, float y, uint32_t pressed, int wheel) {
    int ww = 0, wh = 0, pw = 0, ph = 0;
    hos_sdl_window_size(window, &ww, &wh);
    hos_sdl_pixel_size(window, &pw, &ph);
    MkwVkPointer sample = {0};
    sample.width = pw, sample.height = ph;
    const double px = ww > 0 ? floor((double)x * pw / ww) : 0;
    const double py = wh > 0 ? floor((double)y * ph / wh) : 0;
    const int valid = ww > 0 && wh > 0 && isfinite(px) && isfinite(py) &&
                      px >= INT32_MIN && px <= INT32_MAX && py >= INT32_MIN && py <= INT32_MAX;
    if (valid) sample.x = (int32_t)px, sample.y = (int32_t)py;
    sample.held = ptr_held, sample.pressed = pressed, sample.wheel = wheel;
    hos_pointer_publish(&pointer, sample, valid && ptr_in && !mouse_captured && hos_posix_window[0]);
}
static void pointer_update(uint32_t pressed, int wheel) {
    float x = 0, y = 0;
    hos_sdl_mouse_position(&x, &y);
    pointer_publish_at(x, y, pressed, wheel);
}
static uint32_t sdl_button_bit(int b) { return b == 1 ? HOS_MOUSE_LEFT : b == 3 ? HOS_MOUSE_RIGHT : b == 2 ? HOS_MOUSE_MIDDLE : 0; }

/* ---- keys: SDL's scancodes (USB HID usages) as Windows' virtual keys ---- */
static void keys_update(void) {
    int n = 0;
    const uint8_t *k = hos_sdl_keys(&n);
    uint8_t v[256];
    memset(v, 0, sizeof v);
    if (k && hos_posix_window[0]) {
#define K(sc) ((sc) < n && k[sc])
        for (int i = 0; i < 26; i++) v['A' + i] = K(4 + i);
        for (int i = 0; i < 9; i++) v['1' + i] = K(30 + i);
        v['0'] = K(39);
        v[VK_RETURN] = K(40) || K(88), v[VK_ESCAPE] = K(41), v[VK_BACK] = K(42), v[VK_TAB] = K(43), v[VK_SPACE] = K(44);
        v[VK_OEM_MINUS] = K(45), v[VK_OEM_PLUS] = K(46), v[VK_OEM_1] = K(51), v[VK_OEM_3] = K(53);
        v[VK_OEM_COMMA] = K(54), v[VK_OEM_PERIOD] = K(55), v[VK_OEM_2] = K(56), v[VK_CAPITAL] = K(57);
        v[VK_OEM_4] = K(47), v[VK_OEM_6] = K(48), v[VK_OEM_5] = K(49), v[VK_OEM_7] = K(52);   /* ([ ] \ ') */
        for (int i = 0; i < 12; i++) v[VK_F1 + i] = K(58 + i);
        v[VK_PAUSE] = K(72), v[VK_INSERT] = K(73), v[VK_HOME] = K(74), v[VK_PRIOR] = K(75), v[VK_DELETE] = K(76);
        v[VK_END] = K(77), v[VK_NEXT] = K(78), v[VK_RIGHT] = K(79), v[VK_LEFT] = K(80), v[VK_DOWN] = K(81), v[VK_UP] = K(82);
        v[VK_DIVIDE] = K(84), v[VK_MULTIPLY] = K(85), v[VK_SUBTRACT] = K(86), v[VK_ADD] = K(87);
        for (int i = 0; i < 9; i++) v[VK_NUMPAD0 + 1 + i] = K(89 + i);
        v[VK_NUMPAD0] = K(98), v[VK_DECIMAL] = K(99);
        v[VK_LCONTROL] = K(224), v[VK_LSHIFT] = K(225), v[VK_LMENU] = K(226);
        v[VK_RCONTROL] = K(228), v[VK_RSHIFT] = K(229), v[VK_RMENU] = K(230);
#undef K
        v[VK_CONTROL] = v[VK_LCONTROL] || v[VK_RCONTROL];
        v[VK_SHIFT] = v[VK_LSHIFT] || v[VK_RSHIFT];
        v[VK_MENU] = v[VK_LMENU] || v[VK_RMENU];
        const uint32_t b = hos_sdl_mouse_buttons();
        v[VK_LBUTTON] = (b & 1) != 0, v[VK_MBUTTON] = (b & 2) != 0, v[VK_RBUTTON] = (b & 4) != 0;
        v[VK_XBUTTON1] = (b & 8) != 0, v[VK_XBUTTON2] = (b & 16) != 0;   /* (SDL's X1 and X2: Mouse 4 and 5) */
    }
    for (int i = 0; i < 256; i++) hos_posix_keys[i] = v[i];
}

/* ---- the window ---- */
static volatile int renderer;   /* the renderer presents into the window */
static HANDLE renderer_up;   /* (set once rx_start has returned, ready or not: hos_renderer_wait) */
static int fullscreen;

static int create_surface(void *w, void *instance, void *surface) { return hos_sdl_vulkan_surface(w, instance, surface); }
static void drawable_size(void *w, int *width, int *height) {
    (void)w;
    hos_pointer_size(&pointer, width, height);
}
/* (SDL's display calls belong on this thread: the loop asks every second, the renderer reads what it found) */
static volatile int refresh_now;
static int refresh_mhz(void *w) { (void)w; return __atomic_load_n(&refresh_now, __ATOMIC_RELAXED); }
/* (the desktop's size in pixels, read on this thread once the window is made: the Quality preset Auto (this PC)) */
static volatile int desktop_w, desktop_h;
static void desktop_size(void *w, int *width, int *height) {
    (void)w;
    *width = __atomic_load_n(&desktop_w, __ATOMIC_RELAXED), *height = __atomic_load_n(&desktop_h, __ATOMIC_RELAXED);
}
static MkwVkWindow vk_window = {MKWVK_WINDOW_VERSION, create_surface, drawable_size, NULL, refresh_mhz, desktop_size};

/* Esc let the captured mouse go just before it opened the Insert menu (Menu shortcut "Insert, F1 or Esc"):
   the capture that comes back when the menu closes. Spent by the next capture or menu */
static int esc_had_capture;
static void mouse_capture(int on) {
    if (on == (int)mouse_captured) return;
    mouse_captured = on;
    InterlockedExchange(&mouse_dx, 0);
    InterlockedExchange(&mouse_dy, 0);
    if (on) esc_had_capture = 0;
    hos_sdl_relative_mouse(window, on);
}
static void set_fullscreen(int on) {
    if (on == fullscreen) return;
    fullscreen = on;
    hos_posix_window[3] = on;
    hos_sdl_fullscreen(window, on);
    hos_log("display: %s", on ? "fullscreen (F11 for a window)" : "windowed");
}

static DWORD WINAPI renderer_main(void *p) {
    (void)p;
    const int ok = rx_start(&vk_window);
    if (!ok) hos_log("display: the renderer did not start: the window stays black");
    renderer = ok;
    if (ok) {   /* (a size change while it started) */
        int w = 0, h = 0;
        hos_pointer_size(&pointer, &w, &h);
        if (w > 0 && h > 0) rx_resize(w, h);
    }
    SetEvent(renderer_up);
    return 0;
}

/* (hookshot) host/display.c's: the game's first work for the GPU waits for the renderer (nvn_native.c), so nothing it submits while
   the shaders are prepared is lost. Without a window the renderer starts on the game's own thread, before this is asked */
void hos_renderer_wait(void) {
    { const char *e = getenv("HOS_NO_RENDERER_WAIT"); if (e && atoi(e) != 0) return; }
    if (renderer_up) WaitForSingleObject(renderer_up, INFINITE);
}

static volatile int start_asked;
static volatile int fullscreen_asked = -1;   /* (the Insert menu's row, from the renderer's thread: applied on this one) */
static volatile int menu_asked = -1;         /* (the Insert menu opened or closed, likewise) */
static volatile int restart_asked;           /* (the Insert menu's Restart now: the window closes as by the player) */
static volatile int close_asked;             /* (the Insert menu's Quit: likewise, without the relaunch) */

/* PostMessageA to the window (host/posix/win32_posix.c): WM_APP + 7, the Insert menu's Fullscreen row, WM_APP + 8 the
   menu opened or closed, WM_APP + 9 its Restart now, WM_CLOSE its Quit (host/display.c's) */
__attribute__((visibility("default"))) void hos_posix_post_message(uint32_t msg, uintptr_t wp, intptr_t lp) {
    if (msg == WM_APP + 7) __atomic_store_n(&fullscreen_asked, wp ? 1 : 0, __ATOMIC_RELEASE);
    if (msg == WM_APP + 8) {
        /* Keep the opening reason in the same atomic as the requested state. */
        __atomic_store_n(&menu_asked, !wp ? 0 : 1 + (lp != 0), __ATOMIC_RELEASE);
    }
    if (msg == WM_APP + 9) __atomic_store_n(&restart_asked, 1, __ATOMIC_RELEASE);
    if (msg == WM_CLOSE) __atomic_store_n(&close_asked, 1, __ATOMIC_RELEASE);
}
/* the menu opened: the mouse let go (its capture kept for later); closed: the capture back, if the window still has the focus */
static int captured_before_menu;
static void menu_mouse(int request) {
    const int open = request != 0;
    if (open == menu_open) return;
    menu_open = open;
    if (open) {
        captured_before_menu = (int)mouse_captured || (request == 2 && esc_had_capture);   /* (opened by Esc, which let it go) */
        esc_had_capture = 0;
        mouse_capture(0);
        hos_pointer_reset(&pointer);
        pointer_update(0, 0);
    } else {
        if (captured_before_menu && hos_posix_window[0]) mouse_capture(1);
        captured_before_menu = 0;
    }
}
static int sdl_ready;
int hos_display_owns_sdl(void) { return sdl_ready; }   /* (controllers stay on this thread while it runs SDL) */

/* main(), before the game: SDL's video on this, the main, thread; the screen's size for GetSystemMetrics.
   Returns 1 when ready, 0 for explicit headless, -1 for startup failure. */
static const char *window_title(const char *fallback) {
    const char *e = getenv("HOS_WINDOW_TITLE");
    return (e && e[0]) ? e : fallback;
}

int hos_display_init(void) {
    if (env_on("HOS_HEADLESS")) return 0;
    if (!hos_sdl_load()) {
        hos_log("display: no SDL: no window");
        fprintf(stderr, "display: no SDL: no window\n");
        return -1;
    }
    const char *env_class = getenv("HOS_WM_CLASS");
    const char *app_id = (env_class && env_class[0]) ? env_class : HOS_APP_ID;
    hos_sdl_identity(window_title("Hookshot"), app_id);
    hos_sdl_hint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
    hos_sdl_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    /* (bundled SDL tries x11 before wayland. NVIDIA's X11 surface has no mailbox, and an immediate
       present then drops vblank for every output. Wayland has mailbox. SDL_VIDEODRIVER (SDL3: SDL_VIDEO_DRIVER) still wins.
       Under gamescope (XDG_CURRENT_DESKTOP names it: Steam Deck game mode) SDL keeps its own order.) */
    const char* desktop = getenv("XDG_CURRENT_DESKTOP");
    const int gamescope = desktop && strstr(desktop, "gamescope");
    if (!getenv("SDL_VIDEODRIVER") && !getenv("SDL_VIDEO_DRIVER") && !gamescope)
        hos_sdl_hint(hos_sdl_load() == 3 ? "SDL_VIDEO_DRIVER" : "SDL_VIDEODRIVER", "wayland,x11");
    /* (SDL2 on Wayland: libdecor from the start. SDL2 2.32 asks for the compositor's decorations before the window's first
       configure, and a compositor that answers "the client draws them" (COSMIC) has it unmap and map the window again
       with libdecor inside that wait, which then waits forever on a field the switch cleared: no window, no renderer
       (#992). libdecor takes the compositor's decorations itself where they are offered; with no libdecor SDL keeps
       its own. SDL3 waits on the window's state and is left alone; SDL_VIDEO_WAYLAND_PREFER_LIBDECOR still wins) */
    if (hos_sdl_load() == 2) hos_sdl_hint("SDL_VIDEO_WAYLAND_PREFER_LIBDECOR", "1");
    if (!hos_sdl_init(HOS_SDL_VIDEO | HOS_SDL_EVENTS)) {
        hos_log("display: SDL_Init: %s: no window", hos_sdl_error());
        fprintf(stderr, "display: SDL_Init: %s: no window\n", hos_sdl_error());
        return -1;
    }
    int w, h;
    if (hos_sdl_desktop_size(&w, &h)) hos_posix_window[1] = w, hos_posix_window[2] = h;
    sdl_ready = 1;
    return 1;
}

/* the game's thread, when NVN is up (hos_main.c): the window is made on the main thread, which waits for this */
void hos_display_start(void) {
    if (env_on("HOS_HEADLESS") || !sdl_ready) {   /* no window: the renderer still draws, offscreen (screenshots) */
        rx_start(NULL);
        return;
    }
    renderer_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    __atomic_store_n(&start_asked, 1, __ATOMIC_RELEASE);
}

/* the main thread's loop, until the process ends: the window once asked for, then its events */
void hos_display_loop(void) {
    if (!sdl_ready) {
        for (;;) Sleep(1000);
    }
    HosSdlEvent e;
    while (!__atomic_load_n(&start_asked, __ATOMIC_ACQUIRE)) hos_sdl_wait_event(&e, 10);   /* (nothing to show yet) */
    extern float hos_aspect_ratio;
    /* (the picture's shape when Monitor's shape set one, wider or narrower than 16:9: 960 x 720 for 4:3, 1720 x 720 for 21:9) */
    int win_w = hos_aspect_ratio > 0.0f ? (int)(720 * hos_aspect_ratio + 0.5f) : 1280, win_h = 720;
    if (getenv("HOS_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(getenv("HOS_WINDOW_SIZE"), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) win_w = w, win_h = h;
    }
    window = hos_sdl_window(window_title(HOS_GAME_TITLE), win_w, win_h);
    if (!window) {
        hos_log("display: SDL_CreateWindow: %s: no window", hos_sdl_error());
        fprintf(stderr, "display: SDL_CreateWindow: %s: no window\n", hos_sdl_error());
        hos_log_flush();
        ExitProcess(1);
    }
    vk_window.window = window;
    hos_posix_window[0] = 1;
    {   /* the window's icon: hookshot.png beside the game (make_game.py puts it there), for X11 and the compositors that
           take one from the window; the others find it through the menu entry */
        char png[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, png, MAX_PATH);
        while (n && png[n - 1] != '/') n--;
        snprintf(png + n, sizeof png - n, "hookshot.png");
        hos_sdl_icon(window, png);
    }
    if (getenv("HOS_FULLSCREEN") && atoi(getenv("HOS_FULLSCREEN"))) set_fullscreen(1);
    __atomic_store_n(&refresh_now, hos_sdl_refresh_mhz(window), __ATOMIC_RELAXED);
    {   /* (the desktop's size, before the renderer starts: the Quality preset Auto (this PC) reads it once, desktop_size) */
        int dw, dh;
        if (hos_sdl_desktop_size(&dw, &dh))
            __atomic_store_n(&desktop_w, dw, __ATOMIC_RELAXED), __atomic_store_n(&desktop_h, dh, __ATOMIC_RELAXED);
    }
    pointer_update(0, 0);
    CloseHandle(CreateThread(NULL, 0, renderer_main, NULL, 0, NULL));
    DWORD refresh_asked = GetTickCount();
    for (;;) {
        hos_sdl_pads_pump();
        if (GetTickCount() - refresh_asked >= 1000) {   /* (the window may have moved to another display) */
            refresh_asked = GetTickCount();
            __atomic_store_n(&refresh_now, hos_sdl_refresh_mhz(window), __ATOMIC_RELAXED);
        }
        if (hos_sdl_wait_event(&e, 4)) {
            do {
                switch (e.type) {
                case HOS_SDL_EV_QUIT:
                case HOS_SDL_EV_CLOSE:
                    rx_close_asked();   /* (a start still preparing the shaders keeps what it made) */
                    hos_window_closed_exit();
                case HOS_SDL_EV_RESIZED:
                    pointer_update(0, 0);   /* publish pixels before waking the renderer's size consumer */
                    if (renderer) {
                        int w = 0, h = 0;
                        hos_pointer_size(&pointer, &w, &h);
                        rx_resize(w, h);
                    }
                    break;
                case HOS_SDL_EV_FOCUS_GAINED:
                    hos_posix_window[0] = 1;
                    break;
                case HOS_SDL_EV_FOCUS_LOST:
                    hos_posix_window[0] = 0;
                    esc_had_capture = 0;
                    mouse_capture(0);
                    ptr_in = 0, ptr_held = 0;
                    break;
                case HOS_SDL_EV_MOUSE_LEAVE:
                    /* (Wayland: turning the relative mode on sends a leave in the same millisecond (Mutter), and
                       releasing here dropped the capture before the first motion: the mouse turned nothing and its
                       buttons did nothing. The capture goes with the window's focus, not with the pointer: Windows
                       releases on WM_KILLFOCUS alone, host/display.c.) */
                    ptr_in = 0;
                    break;
                case HOS_SDL_EV_MOUSE_ENTER:
                    ptr_in = 1;
                    break;
                case HOS_SDL_EV_KEY_DOWN: {
                    const int scancode = e.scancode, mod = e.mod;
                    if (e.scancode == 41) {   /* Esc (it may open the Insert menu too: the capture comes back) */
                        if (!menu_open && mouse_captured) esc_had_capture = 1;
                        mouse_capture(0);
                    }
                    if (!e.repeat && ((scancode == 68 && !(mod & 0x0FC3)) || (scancode == 40 && (mod & 0x300)))) set_fullscreen(!fullscreen);   /* F11, Alt+Enter */
                    break;
                }
                case HOS_SDL_EV_MOUSE_DOWN:
                    ptr_in = 1;
                    ptr_held |= sdl_button_bit(e.button);
                    if (menu_open) pointer_publish_at(e.x, e.y, sdl_button_bit(e.button), 0);   /* (the menu's click) */
                    else if (renderer && !mouse_captured) mouse_capture(1);
                    break;
                case HOS_SDL_EV_MOUSE_UP:
                    ptr_held &= ~sdl_button_bit(e.button);
                    break;
                case HOS_SDL_EV_MOUSE_WHEEL:
                    if (menu_open) pointer_update(0, (int)(e.wheel * 120));
                    break;
                case HOS_SDL_EV_MOUSE_MOTION:
                    if (!mouse_captured) ptr_in = 1;
                    if (mouse_captured) {   /* (SDL3's motion comes in fractions of a pixel: the remainders kept for the next) */
                        static float rx, ry;
                        rx += e.dx, ry += e.dy;
                        const LONG dx = (LONG)rx, dy = (LONG)ry;
                        rx -= (float)dx, ry -= (float)dy;
                        InterlockedExchangeAdd(&mouse_dx, dx);
                        InterlockedExchangeAdd(&mouse_dy, dy);
                    }
                    break;
                }
                pointer_update(0, 0);
            } while (hos_sdl_poll_event(&e));
        }
        const int asked = __atomic_exchange_n(&fullscreen_asked, -1, __ATOMIC_ACQ_REL);
        if (asked >= 0) set_fullscreen(asked);
        const int menu = __atomic_exchange_n(&menu_asked, -1, __ATOMIC_ACQ_REL);
        if (menu >= 0) menu_mouse(menu);
        if (__atomic_load_n(&restart_asked, __ATOMIC_ACQUIRE)) {
            hos_relaunch_ask();
            rx_close_asked();
            hos_window_closed_exit();
        }
        if (__atomic_load_n(&close_asked, __ATOMIC_ACQUIRE)) {
            rx_close_asked();
            hos_window_closed_exit();
        }
        pointer_update(0, 0);
        keys_update();
    }
}
