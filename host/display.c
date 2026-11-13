/* The game's window (Windows): the renderer's surface (gpu/render/, WebGPU). The native NVN layer (nvn_native.c)
   presents the game's window textures into it (rx_nvn_present). The title bar carries live numbers.
   TOTKPort's hos/display.c, without its emulated-GPU fallback (the window buffer read from guest memory): MKW has
   no emulated GPU, so the renderer is the only way to a picture.
   HOS_HEADLESS=1: no window (scripted runs; the renderer draws offscreen). Closing the window ends the run.
   HOS_WINDOW_NOACTIVATE=1: the window opens behind the one in front (test runs do not take the keyboard).
   HOS_WINDOW_SIZE=<w>x<h>: the client area (default 1280x720 at 96 dpi). F11 or Alt+Enter: fullscreen.
   Mouse: a click in the window captures the mouse (hidden, kept in the window), Esc or leaving the window releases
   it (nn_hid.c reads its motion). While the Insert menu is open the mouse is free (the cursor shown, for the menu's
   clicks: hos_mouse_pointer) and the game gets none of it; the capture it had comes back when the menu closes. The same
   while one of the game's own menus is open (host/oot_menu.c: hos_mouse_game_menu each frame it runs), its clicks and
   wheel for that menu (hos_mouse_game_pointer). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../gpu/render/rx.h"
void rx_close_asked(void);   /* host/rx_backend.c */
#include "../gpu/vk/vk_api.h"
#include "display_pointer.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

void hos_relaunch_ask(void);        /* (host/hos_relaunch.c) */
void hos_window_closed_exit(void);
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

volatile LONG64 hos_present_count;   /* (nvn_native.c: the game's presents) */
static HWND hwnd;
static int renderer;   /* the renderer presents into the window */

/* mouse capture: raw motion accumulated here, taken by nn_hid.c */
static volatile LONG mouse_dx, mouse_dy;
static volatile LONG mouse_captured;
static int cursor_hidden;

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

/* the free mouse, for the Insert menu (gpu/vk/vk_api.h MkwVkPointer): set on the window's thread, read on the renderer's */
static HosPointer pointer = HOS_POINTER_INIT;
static int menu_open, captured_before_menu;
static int pointer_tracking;
/* Esc let the captured mouse go just before it opened the Insert menu (WM_KEYDOWN; Menu shortcut "Insert,
   F1 or Esc", gpu/vk/vk_menu.cpp): the capture that comes back when the menu closes. Spent by the next capture or menu */
static int esc_had_capture;
/* the game's own menus: open while one of their screens runs with the mouse (stamped each frame on the
   game's thread), their clicks */
static int game_menu_open;
static volatile LONG64 game_menu_seen_ns;
static HosPointer game_pointer = HOS_POINTER_INIT;
/* The cursor over the game's menus is hidden until the mouse is used (moved or a button pressed) and again once the
   pads or the keys are (nn_hid.c: hos_player_input_ns): a player on a controller never sees it, and a mouse at rest
   takes no part (no pointing or clicks while hidden). The wheel works hidden too, and does not show it: a button held
   (R, for the item choice) counts as the pad in use, and hid the cursor again at each notch */
int64_t hos_player_input_ns(void);
static volatile LONG gm_shown, gm_mx = -1, gm_my = -1;
static volatile LONG64 gm_mouse_ns;
static void gm_mouse_used(void) { InterlockedExchange64(&gm_mouse_ns, (LONG64)hos_now_ns()); InterlockedExchange(&gm_shown, 1); }
static int gm_cursor_shown(void) {
    if (gm_shown && hos_player_input_ns() > InterlockedCompareExchange64(&gm_mouse_ns, 0, 0)) InterlockedExchange(&gm_shown, 0);
    return (int)gm_shown;
}

int hos_mouse_pointer(MkwVkPointer *out) { return hos_pointer_take(&pointer, out); }

/* Publish client coordinates on the DPI-aware window thread, including the exact button event point. */
static void pointer_publish_at(HWND w, POINT at, uint32_t pressed, int wheel) {
    if (mouse_captured) return;
    RECT rc = {0};
    POINT screen = at;
    const int valid = GetClientRect(w, &rc) && ClientToScreen(w, &screen);
    const int over = valid && WindowFromPoint(screen) == w;
    MkwVkPointer sample = {0};
    sample.x = at.x, sample.y = at.y, sample.width = rc.right, sample.height = rc.bottom;
    sample.held = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? HOS_MOUSE_LEFT : 0) |
                  ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? HOS_MOUSE_RIGHT : 0) |
                  ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? HOS_MOUSE_MIDDLE : 0);
    const int inside = at.x >= 0 && at.y >= 0 && at.x < rc.right && at.y < rc.bottom;
    const int active = valid && !IsIconic(w) && GetForegroundWindow() == w &&
                       ((inside && over) || GetCapture() == w);
    sample.pressed = menu_open ? pressed : 0, sample.wheel = menu_open ? wheel : 0;
    hos_pointer_publish(&pointer, sample, active);
    sample.pressed = pressed, sample.wheel = wheel;
    hos_pointer_publish(&game_pointer, sample, active && game_menu_open && !menu_open);
}

/* Moving/resizing with a stationary cursor also changes its client point. */
static void pointer_update(HWND w, uint32_t pressed, int wheel) {
    if (mouse_captured) return;   /* mouse_capture cleared both mailboxes on entry */
    POINT at = {0};
    if (!GetCursorPos(&at) || !ScreenToClient(w, &at)) {
        hos_pointer_reset(&pointer);
        hos_pointer_reset(&game_pointer);
        return;
    }
    pointer_publish_at(w, at, pressed, wheel);
}

static HANDLE renderer_up;
static int fullscreen;
static WINDOWPLACEMENT windowed_place;

static void set_fullscreen(HWND w, int on) {
    if (on == fullscreen) return;
    const LONG style = GetWindowLongA(w, GWL_STYLE);
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        windowed_place.length = sizeof windowed_place;
        if (!GetWindowPlacement(w, &windowed_place) ||
            !GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        fullscreen = 1;
        SetWindowLongA(w, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        fullscreen = 0;
        SetWindowLongA(w, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(w, &windowed_place);
        SetWindowPos(w, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    hos_log("display: %s", on ? "fullscreen (F11 for a window)" : "windowed");
}

static void dpi_aware_thread(void) {
    if (env_on("HOS_DPI_UNAWARE")) return;
    typedef void *(WINAPI * SetCtx)(void *);
    SetCtx set = (SetCtx)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext");
    if (set) set((void *)(intptr_t)-4);
}

static UINT window_dpi(HWND w) {
    typedef UINT(WINAPI * GetDpi)(HWND);
    GetDpi get = (GetDpi)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    const UINT dpi = get ? get(w) : 96;
    return dpi ? dpi : 96;
}

static void mouse_capture(HWND w, int on) {
    if (on == (int)mouse_captured) return;
    mouse_captured = on;
    InterlockedExchange(&mouse_dx, 0);
    InterlockedExchange(&mouse_dy, 0);
    if (on) esc_had_capture = 0;   /* (captured again: Esc's note is spent) */
    if (on) {
        hos_pointer_reset(&pointer);
        hos_pointer_reset(&game_pointer);
        if (pointer_tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE | TME_CANCEL, w, 0};
            TrackMouseEvent(&t);
            pointer_tracking = 0;
        }
        RECT rc;
        GetClientRect(w, &rc);
        MapWindowPoints(w, NULL, (POINT *)&rc, 2);
        ClipCursor(&rc);
        if (!cursor_hidden) { ShowCursor(FALSE); cursor_hidden = 1; }
    } else {
        ClipCursor(NULL);
        if (cursor_hidden) { ShowCursor(TRUE); cursor_hidden = 0; }
    }
    pointer_update(w, 0, 0);
}

/* the cursor hidden now while it is over the window (not at the next move's WM_SETCURSOR) */
static void gm_cursor_off(HWND w) {
    POINT p;
    RECT rc;
    if (GetCursorPos(&p) && ScreenToClient(w, &p) && GetClientRect(w, &rc) && PtInRect(&rc, p)) SetCursor(NULL);
}
/* a menu opened (the Insert menu or the game's): the mouse let go (its capture kept for later); both closed: the capture
   back, if the window still has the focus */
static void menus_mouse(HWND w, int insert_open, int game_open) {
    const int was = menu_open || game_menu_open, now = insert_open || game_open, game_was = game_menu_open;
    menu_open = insert_open, game_menu_open = game_open;
    /* (hidden till used; a menu back within a second, a moment between its screens, keeps the cursor as it was) */
    static int64_t game_closed_ns;
    if (!game_open && game_was) game_closed_ns = hos_now_ns();
    if (game_open && !game_was && (!game_closed_ns || hos_now_ns() - game_closed_ns > 1000000000LL)) {
        POINT at = {0};
        GetCursorPos(&at);
        ScreenToClient(w, &at);
        InterlockedExchange(&gm_shown, 0), gm_mx = at.x, gm_my = at.y;
    }
    if (now && !was) {
        captured_before_menu = (int)mouse_captured || (insert_open && esc_had_capture);   /* (opened by Esc, which let it go) */
        esc_had_capture = 0;
        mouse_capture(w, 0);
    } else if (!now && was) {
        if (captured_before_menu && GetForegroundWindow() == w) mouse_capture(w, 1);
        captured_before_menu = 0;
    }
    /* (a game menu opened: its cursor hidden till the mouse is used, at once; the release above showed it) */
    if (game_open && !game_was && !insert_open) gm_cursor_off(w);
    hos_pointer_reset(&pointer);
    hos_pointer_reset(&game_pointer);
    pointer_update(w, 0, 0);
}
static void menu_mouse(HWND w, int open, int opened_by_escape) {
    if (open == menu_open) return;
    if (!opened_by_escape) esc_had_capture = 0;
    menus_mouse(w, open, game_menu_open);
}

/* (host/oot_menu.c, the game's thread) one of the game's menus ran this frame */
void hos_mouse_game_menu(void) {
    const LONG64 before = InterlockedExchange64(&game_menu_seen_ns, (LONG64)hos_now_ns());
    if (!before && hwnd) PostMessageA(hwnd, WM_APP + 10, 1, 0);
}
/* (host/oot_menu.c, the game's thread) frames go on with none of the game's menus holding the mouse: closed now */
void hos_mouse_game_menu_gone(void) {
    const LONG64 seen = InterlockedCompareExchange64(&game_menu_seen_ns, 0, 0);
    if (seen && hwnd && InterlockedCompareExchange64(&game_menu_seen_ns, 0, seen) == seen) PostMessageA(hwnd, WM_APP + 11, 0, 0);
}
/* (host/oot_menu.c, the game's thread) the wheel's notches since the last call, for a screen that takes only the wheel
   and leaves the mouse as it is (the R button's item choice, in play: the mouse the camera's). Notches count only while
   such a screen reads them (a call in the last 200 ms): a wheel turned in play does not wait for the next one */
static volatile LONG play_wheel;
static volatile LONG64 play_wheel_ns;
int hos_mouse_wheel_take(void) {
    InterlockedExchange64(&play_wheel_ns, (LONG64)hos_now_ns());
    const LONG wl = InterlockedExchange(&play_wheel, 0);
    if (!hwnd || menu_open || GetForegroundWindow() != hwnd) return 0;
    if (wl % WHEEL_DELTA) InterlockedExchangeAdd(&play_wheel, wl % WHEEL_DELTA);
    return wl / WHEEL_DELTA;
}
/* the free mouse over the game's menus: its position (client pixels) and the window's size, the buttons pressed
   (HOS_MOUSE_*) and the wheel's notches since the last call. 0 when the Insert menu has it or it is outside the window;
   2 while the cursor is hidden: the wheel only (no presses; the position is not one to point with) */
int hos_mouse_game_pointer(int *x, int *y, int *w, int *h, uint32_t *pressed, int *wheel) {
    *pressed = 0, *wheel = 0;
    MkwVkPointer sample = {0};
    const int active = hos_pointer_take(&game_pointer, &sample);
    *x = sample.x, *y = sample.y, *w = sample.width, *h = sample.height;
    if (!active) return 0;
    const int shown = gm_cursor_shown();
    if (shown) *pressed = sample.pressed;
    *wheel = sample.wheel;
    return shown ? 1 : 2;
}
static void game_menu_press(LPARAM lp, LONG bit) {
    const POINT at = {(short)LOWORD(lp), (short)HIWORD(lp)};
    gm_mouse_used();
    pointer_publish_at(hwnd, at, (uint32_t)bit, 0);
}
static void menu_press(HWND w, LPARAM lp, LONG bit) {   /* (a click while the menu is open: the menu's) */
    const POINT at = {(short)LOWORD(lp), (short)HIWORD(lp)};
    SetCapture(w);   /* a drag past the window edge still reaches it */
    pointer_publish_at(w, at, (uint32_t)bit, 0);
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (mouse_captured) return 0;
        const int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (game_menu_open && !menu_open && (x != gm_mx || y != gm_my)) {   /* (a move: not a repeat at rest) */
            gm_mx = x, gm_my = y;
            gm_mouse_used();
        }
        if (!pointer_tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, w, 0};
            pointer_tracking = TrackMouseEvent(&t) != 0;
        }
        pointer_update(w, 0, 0);
        return 0;
    }
    case WM_MOUSELEAVE:
        pointer_tracking = 0;
        pointer_update(w, 0, 0);
        return 0;
    case WM_MOUSEWHEEL:
        if (menu_open || game_menu_open) pointer_update(w, 0, GET_WHEEL_DELTA_WPARAM(wp));   /* (hidden or not) */
        else if ((LONG64)hos_now_ns() - InterlockedCompareExchange64(&play_wheel_ns, 0, 0) < 200000000LL)
            InterlockedExchangeAdd(&play_wheel, GET_WHEEL_DELTA_WPARAM(wp));   /* (a screen that takes the wheel only) */
        return 0;
    case WM_RBUTTONDOWN:
        if (menu_open) menu_press(w, lp, HOS_MOUSE_RIGHT);
        else if (game_menu_open) game_menu_press(lp, HOS_MOUSE_RIGHT);
        return 0;
    case WM_MBUTTONDOWN:
        if (menu_open) menu_press(w, lp, HOS_MOUSE_MIDDLE);
        else if (game_menu_open) game_menu_press(lp, HOS_MOUSE_MIDDLE);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (GetCapture() == w && !(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        pointer_update(w, 0, 0);
        return 0;
    case WM_APP + 8:   /* (the Insert menu opened or closed: gpu/vk/vk_menu.cpp) */
        menu_mouse(w, (int)wp, wp && lp);
        return 0;
    case WM_APP + 10:   /* (a game menu opened: hos_mouse_game_menu) */
        if (!game_menu_open) menus_mouse(w, menu_open, 1);
        return 0;
    case WM_APP + 11:   /* (the game's menus closed: hos_mouse_game_menu_gone) */
        if (game_menu_open) menus_mouse(w, menu_open, 0);
        return 0;
    case WM_TIMER: {
        pointer_update(w, 0, 0);
        /* (hookshot, #1532) a controller is not input to Windows: playing with one, the display went off at the power
           plan's timeout. While the game's window is in front and not minimized, the display's idle timer is reset every
           30 s (ES_DISPLAY_REQUIRED without ES_CONTINUOUS: nothing is held when the game is left or closed).
           HOS_KEEP_DISPLAY=0: Windows' timeout as before */
        static ULONGLONG last_awake;
        static int keep = -1;
        if (keep < 0) keep = !getenv("HOS_KEEP_DISPLAY") || atoi(getenv("HOS_KEEP_DISPLAY")) != 0;
        const ULONGLONG now = GetTickCount64();
        if (keep && renderer && now - last_awake >= 30000 && GetForegroundWindow() == w && !IsIconic(w)) {
            last_awake = now;
            SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        }
        /* (the game's menus closed: no frame of one for 300 ms) */
        const LONG64 seen = InterlockedCompareExchange64(&game_menu_seen_ns, 0, 0);
        if (game_menu_open && seen && (LONG64)hos_now_ns() - seen > 300000000LL &&
            InterlockedCompareExchange64(&game_menu_seen_ns, 0, seen) == seen)
            menus_mouse(w, menu_open, 0);
        if (game_menu_open && !menu_open && !gm_cursor_shown()) gm_cursor_off(w);   /* (hidden by the pads or keys: at once) */
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SCREENSAVE && !(wp & SCF_ISSECURE) &&
            renderer && GetForegroundWindow() == w && !IsIconic(w)) {
            BOOL secure = TRUE;   /* unknown or protected policy keeps Windows' normal handling */
            if (SystemParametersInfoA(SPI_GETSCREENSAVESECURE, 0, &secure, 0) && !secure) return 0;
        }
        break;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (mouse_captured && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedExchangeAdd(&mouse_dx, ri.data.mouse.lLastX);
            InterlockedExchangeAdd(&mouse_dy, ri.data.mouse.lLastY);
        }
        return DefWindowProcA(w, msg, wp, lp);
    }
    case WM_LBUTTONDOWN:
        if (menu_open) menu_press(w, lp, HOS_MOUSE_LEFT);
        else if (game_menu_open) game_menu_press(lp, HOS_MOUSE_LEFT);
        else if (renderer && !mouse_captured) mouse_capture(w, 1);
        return 0;
    case WM_KEYDOWN: {
        extern void hos_vk_note_key_press(unsigned key);
        if (!(lp & (1 << 30))) hos_vk_note_key_press((unsigned)wp);
        if (wp == VK_ESCAPE) {   /* (it may open the Insert menu too: a capture let go here comes back) */
            if (!menu_open && !game_menu_open && mouse_captured) esc_had_capture = 1;
            mouse_capture(w, 0);
        }
        if (wp == VK_F11 && !(lp & (1 << 30))) {
            const int mod = (GetKeyState(VK_SHIFT) | GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU)) & 0x8000;
            if (!mod) set_fullscreen(w, !fullscreen);
        }
        return 0;
    }
    case WM_SYSKEYDOWN:   /* Alt+Enter, as on Linux (display_sdl.c); other Alt keys (Alt+F4) go on to Windows */
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            if (!(lp & (1 << 30))) set_fullscreen(w, !fullscreen);
            return 0;
        }
        break;
    case WM_SYSCHAR:   /* (its character: without this Windows beeps, as for an Alt key with no menu) */
        if (wp == '\r') return 0;
        break;
    case WM_APP + 7:   /* (the Insert menu's row) */
        set_fullscreen(w, (int)wp);
        return 0;
    case WM_DPICHANGED:
        if (!fullscreen) {
            const RECT *s = (const RECT *)lp;
            SetWindowPos(w, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        pointer_update(w, 0, 0);
        return 0;
    case WM_SETFOCUS:
        pointer_update(w, 0, 0);
        break;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        esc_had_capture = 0;
        mouse_capture(w, 0);
        hos_pointer_reset(&pointer);
        hos_pointer_reset(&game_pointer);
        break;
    case WM_SETCURSOR:
        if ((mouse_captured || (game_menu_open && !menu_open && !gm_cursor_shown())) && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_MOVE:
        if (mouse_captured) { mouse_capture(w, 0); mouse_capture(w, 1); }   /* (the clip follows the window) */
        pointer_update(w, 0, 0);
        break;
    case WM_SIZE:
        pointer_update(w, 0, 0);
        if (mouse_captured) { mouse_capture(w, 0); mouse_capture(w, 1); }
        if (renderer) rx_resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_PAINT:
        if (renderer) {   /* the renderer's surface: nothing to paint */
            ValidateRect(w, NULL);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return renderer ? 1 : DefWindowProcA(w, msg, wp, lp);
    case WM_APP + 9:   /* (the Insert menu's Restart now: closed as by the player, then started again: host/hos_relaunch.c) */
        hos_relaunch_ask();
        /* fall through */
    case WM_CLOSE:
        rx_close_asked();   /* (a start still preparing the shaders keeps what it made) */
        hos_window_closed_exit();
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static void display_start_failed(const char *operation) {
    const DWORD error = GetLastError();
    hos_log("display: %s failed (error %lu)", operation, (unsigned long)error);
    hos_log_flush();
    ExitProcess(1);
}

static DWORD WINAPI display_main(void *p) {
    (void)p;
    {   /* (nothing is typed here. Windows' text services, set up under DefWindowProc as the window was sized, ended the process: a
           fail-fast in CoreMessaging, called from textinputframework) */
        HMODULE imm = LoadLibraryExA("imm32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);   /* (hookshot: System32's only) */
        BOOL (WINAPI *ime_off)(DWORD) = imm ? (BOOL (WINAPI *)(DWORD))GetProcAddress(imm, "ImmDisableIME") : NULL;
        if (ime_off) ime_off((DWORD)-1);
    }
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));   /* (nx2/make_icon.py; none in a build without the picture) */
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "Hookshot";
    RegisterClassA(&wc);
    dpi_aware_thread();
    extern float hos_aspect_ratio;
    /* (the picture's shape when Monitor's shape set one, wider or narrower than 16:9: 960 x 720 for 4:3, 1720 x 720 for 21:9) */
    int win_w = hos_aspect_ratio > 0.0f ? (int)(720 * hos_aspect_ratio + 0.5f) : 1280, win_h = 720;
    if (getenv("HOS_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(getenv("HOS_WINDOW_SIZE"), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) win_w = w, win_h = h;
    }
    RECT r = {0, 0, win_w, win_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const int noactivate = env_on("HOS_WINDOW_NOACTIVATE");
    hwnd = CreateWindowA("Hookshot", HOS_GAME_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                         CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) display_start_failed("CreateWindowA");
    if (window_dpi(hwnd) != 96) {
        const UINT dpi = window_dpi(hwnd);
        RECT s = {0, 0, (LONG)(win_w * dpi / 96), (LONG)(win_h * dpi / 96)};
        AdjustWindowRect(&s, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, s.right - s.left, s.bottom - s.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    /* (no activate: shown at the bottom of the z-order, so a game in front keeps its focus) */
    if (noactivate)
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        ShowWindow(hwnd, SW_SHOW);
    if (getenv("HOS_FULLSCREEN") && atoi(getenv("HOS_FULLSCREEN"))) set_fullscreen(hwnd, 1);
    RAWINPUTDEVICE rid = {1, 2, 0, hwnd};   /* generic desktop mouse: WM_INPUT while the window is in front */
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    pointer_update(hwnd, 0, 0);
    renderer = rx_start(hwnd);
    if (!renderer) hos_log("display: no WebGPU device: the window stays black");
    SetEvent(renderer_up);
    SetTimer(hwnd, 1, 250, NULL);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

/* (hookshot) The game's first work for the GPU waits for the renderer. It ran beside the renderer's start, and what it submitted while the
   shaders were being prepared (80 s on a first start) was lost: that run had no ambient light, black foliage and dark characters. The
   game itself goes on starting as it did: held at the start instead, it left holes in the ground */
void hos_renderer_wait(void) {
    { const char *e = getenv("HOS_NO_RENDERER_WAIT"); if (e && atoi(e) != 0) return; }   /* (=0: the wait, as without it) */   /* (a test: the game's first GPU work no longer waits for the renderer, as before lighting-fix) */
    if (renderer_up) WaitForSingleObject(renderer_up, INFINITE);
}

void hos_display_start(void) {
    if (env_on("HOS_HEADLESS")) {   /* no window: the renderer still draws, offscreen (screenshots, frame dumps) */
        rx_start(NULL);
        return;
    }
    renderer_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!renderer_up) display_start_failed("CreateEventA");
    HANDLE thread = CreateThread(NULL, 0, display_main, NULL, 0, NULL);
    if (!thread) display_start_failed("CreateThread");
    CloseHandle(thread);
}
