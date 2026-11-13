#define _Static_assert static_assert
#include "host/display_pointer.h"
#include "host/hos_sdl.h"
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <initializer_list>
#ifndef WM_MOUSEMOVE
#define WM_MOUSEMOVE 0x0200
#define WM_MOUSELEAVE 0x02a3
#define WM_MOUSEWHEEL 0x020a
#endif
#ifndef MAKELPARAM
#define MAKELPARAM(lo, hi) ((LPARAM)((uint16_t)(lo) | ((uint32_t)(uint16_t)(hi) << 16)))
#define MAKEWPARAM(lo, hi) ((WPARAM)MAKELPARAM(lo, hi))
#endif
#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#endif
using std::floor;
using std::isfinite;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
[[maybe_unused]] static const auto ui_thread = std::this_thread::get_id();
[[maybe_unused]] static void ui_only() { CHECK(std::this_thread::get_id() == ui_thread); }

namespace win {
[[maybe_unused]] static HosPointer pointer = HOS_POINTER_INIT;
[[maybe_unused]] static int mouse_captured, pointer_tracking, cursor_hidden;
static HosPointer game_pointer = HOS_POINTER_INIT;
static int menu_open = 1, game_menu_open;
static LONG gm_mx, gm_my, gm_shown, play_wheel;
static LONG64 play_wheel_ns;
static int captured_before_menu, esc_had_capture;   // (Esc's note: host/display.c)
static int64_t hos_now_ns() { return 100000000; }
static void gm_cursor_off(HWND) {}

static bool shown = true;
static void gm_mouse_used() { shown = true; }
static int gm_cursor_shown() { return shown; }
static LONG mouse_dx, mouse_dy;
static unsigned queries, tracks, cancels;
static bool fail_tracking;
[[maybe_unused]] static HWND hwnd = (HWND)(uintptr_t)1;
[[maybe_unused]] static LONG ptr_x, ptr_y, ptr_w = 1280, ptr_h = 720, ptr_in = 1, ptr_pressed, ptr_wheel;
[[maybe_unused]] static int origin_x, origin_y, cursor_x = 100, cursor_y = 200, width = 1280, height = 720;
[[maybe_unused]] static bool focus = true, iconic = false;
[[maybe_unused]] static HWND foreground() { return focus ? hwnd : nullptr; }
[[maybe_unused]] static short key(int) { queries++; return 0; }
[[maybe_unused]] static BOOL client(HWND, RECT* r) { queries++; ui_only(); *r = {0, 0, width, height}; return TRUE; }
[[maybe_unused]] static BOOL cursor(POINT* p) { queries++; ui_only(); p->x = cursor_x, p->y = cursor_y; return TRUE; }
[[maybe_unused]] static BOOL to_client(HWND, POINT* p) { ui_only(); p->x -= origin_x, p->y -= origin_y; return TRUE; }
static BOOL to_screen(HWND, POINT* p) { ui_only(); p->x += origin_x, p->y += origin_y; return TRUE; }
static HWND set_capture(HWND w) { return w; }
[[maybe_unused]] static BOOL minimized(HWND) { ui_only(); return iconic; }
[[maybe_unused]] static HWND capture() { return nullptr; }
[[maybe_unused]] static bool covered = false;
[[maybe_unused]] static HWND under_cursor(POINT) { queries++; ui_only(); return covered ? nullptr : hwnd; }
struct Tracking { unsigned size, flags; HWND window; unsigned hover; };
static BOOL track(Tracking* t) { if(t->flags & 0x80000000u)cancels++;else tracks++;return !fail_tracking; }
static int map_points(HWND,HWND,POINT*,unsigned){return 0;}
static BOOL clip(const RECT*){return TRUE;}
static int show(BOOL){return 0;}
#define TRACKMOUSEEVENT Tracking
#ifndef TME_LEAVE
#define TME_LEAVE 2
#endif
#ifndef TME_CANCEL
#define TME_CANCEL 0x80000000u
#endif
#define TrackMouseEvent track
#define MapWindowPoints map_points
#define ClipCursor clip
#define ShowCursor show
#define GetForegroundWindow foreground
#define GetAsyncKeyState key
#define GetClientRect client
#define GetCursorPos cursor
#define ScreenToClient to_client
#define ClientToScreen to_screen
#define SetCapture set_capture
#define IsIconic minimized
#define GetCapture capture
#define WindowFromPoint under_cursor
/* ACTUAL_WINDOWS */
/* ACTUAL_MOVE */
/* ACTUAL_WHEEL */
#undef GetForegroundWindow
#undef GetAsyncKeyState
#undef GetClientRect
#undef GetCursorPos
#undef ScreenToClient
#undef ClientToScreen
#undef SetCapture
#undef IsIconic
#undef GetCapture
#undef WindowFromPoint
#undef TRACKMOUSEEVENT
#undef TrackMouseEvent
#undef MapWindowPoints
#undef ClipCursor
#undef ShowCursor
}

namespace sdl {
[[maybe_unused]] static HosPointer pointer = HOS_POINTER_INIT;
[[maybe_unused]] static int mouse_captured, ptr_in = 1, hos_posix_window[4] = {1, 0, 0, 0};
[[maybe_unused]] static uint32_t ptr_held;
static int menu_open=1, renderer;
static void mouse_capture(int on){mouse_captured=on;}
[[maybe_unused]] static LONG ptr_x, ptr_y, ptr_pressed, ptr_wheel;
[[maybe_unused]] static void* window = (void*)(uintptr_t)1;
[[maybe_unused]] static int ww = 1280, wh = 720, pw = 1280, ph = 720;
[[maybe_unused]] static float x = 100, y = 200;
[[maybe_unused]] static void hos_sdl_window_size(void*, int* w, int* h) { ui_only(); *w = ww, *h = wh; }
[[maybe_unused]] static void hos_sdl_pixel_size(void*, int* w, int* h) { ui_only(); *w = pw, *h = ph; }
[[maybe_unused]] static void hos_sdl_mouse_position(float* a, float* b) { ui_only(); *a = x, *b = y; }
static int esc_had_capture, captured_before_menu;
static int fullscreen_asked=-1, menu_asked=-1, restart_asked, close_asked;
#ifndef WM_APP
constexpr uint32_t WM_APP=0x8000;
#endif
#ifndef WM_CLOSE
constexpr uint32_t WM_CLOSE=0x0010;
#endif
/* ACTUAL_SDL */
static void request_menu(bool open, bool escape) {
    hos_posix_post_message(WM_APP+8,open,escape);
    const int request=__atomic_exchange_n(&menu_asked,-1,__ATOMIC_ACQ_REL);
    if (request>=0) menu_mouse(request);
}
/* ACTUAL_DOWN */
}

namespace adapter {
static int version;
static struct { void* GetMouseState; } f;
[[maybe_unused]] static uint32_t floats(float* x, float* y) { *x = 101.75f, *y = -0.25f; return 1; }
[[maybe_unused]] static uint32_t integers(int* x, int* y) { *x = 101, *y = -1; return 1; }
#define CALL(type, name) ((type)f.name)
/* ACTUAL_SDL_ADAPTER */
#undef CALL
}

int main() {
    float x, y;
    adapter::version = 3; adapter::f.GetMouseState = (void*)adapter::floats;
    adapter::hos_sdl_mouse_position(&x, &y); CHECK(x == 101.75f && y == -0.25f);
    adapter::version = 2; adapter::f.GetMouseState = (void*)adapter::integers;
    adapter::hos_sdl_mouse_position(&x, &y); CHECK(x == 101 && y == -1);
    // No foreign-thread SDL calls, including the Vulkan window-size callback.
    sdl::pointer_update(HOS_MOUSE_LEFT, 60);
    MkwVkPointer out{};
    std::thread first([&] {
        CHECK(sdl::hos_mouse_pointer(&out));
        CHECK(out.x == 100 && out.y == 200 && out.width == 1280 && out.height == 720);
        CHECK(out.pressed == HOS_MOUSE_LEFT && out.wheel == 0);
        int w, h; sdl::drawable_size(sdl::window, &w, &h); CHECK(w == 1280 && h == 720);
    });
    first.join();
    sdl::pointer_update(HOS_MOUSE_RIGHT, 60);
    CHECK(sdl::hos_mouse_pointer(&out) && out.pressed == HOS_MOUSE_RIGHT && out.wheel == 1);
    CHECK(sdl::hos_mouse_pointer(&out) && out.pressed == 0 && out.wheel == 0);
    // Fractional SDL3 coordinates: scale before quantizing, including just outside the top/left.
    for (int dpi : {96, 120, 144, 192}) {
        sdl::pw = 1280 * dpi / 96, sdl::ph = 720 * dpi / 96;
        sdl::x = 101.75f, sdl::y = 203.75f;
        sdl::pointer_update(0, 0);
        CHECK(sdl::hos_mouse_pointer(&out));
        CHECK(out.x == (int)std::floor(101.75 * dpi / 96) && out.y == (int)std::floor(203.75 * dpi / 96));
    }
    sdl::x = -0.25f, sdl::y = -0.25f;
    sdl::pointer_update(0, 0); CHECK(sdl::hos_mouse_pointer(&out) && out.x == -1 && out.y == -1);
    sdl::x = NAN; sdl::pointer_update(HOS_MOUSE_LEFT, 120); CHECK(!sdl::hos_mouse_pointer(&out));
    sdl::x = 0; sdl::wh = 0; sdl::pointer_update(0, 0); CHECK(!sdl::hos_mouse_pointer(&out));
    sdl::wh = 720; sdl::pointer_update(0, 0); CHECK(sdl::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    // A stationary screen cursor must follow the new client origin and dimensions.
    win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, -60);
    CHECK(win::hos_mouse_pointer(&out) && out.x == 100 && out.y == 200);
    win::origin_x = 50, win::origin_y = 80, win::width = 1720, win::height = 720;
    win::pointer_update(win::hwnd, HOS_MOUSE_RIGHT, -60);
    CHECK(win::hos_mouse_pointer(&out) && out.x == 50 && out.y == 120 && out.width == 1720);
    CHECK(out.pressed == HOS_MOUSE_RIGHT && out.wheel == -1);
    win::focus = false; win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, 120); CHECK(!win::hos_mouse_pointer(&out));
    win::focus = true; win::pointer_update(win::hwnd, 0, 0); CHECK(win::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    win::mouse_capture(win::hwnd, 1); win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, 60); CHECK(!win::hos_mouse_pointer(&out));
    win::mouse_capture(win::hwnd, 0); win::pointer_update(win::hwnd, 0, 60); CHECK(win::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    win::iconic = true; win::pointer_update(win::hwnd, 0, 120); CHECK(!win::hos_mouse_pointer(&out));
    win::iconic = false; win::pointer_update(win::hwnd, 0, 0); CHECK(win::hos_mouse_pointer(&out) && !out.wheel);
    win::covered = true; win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, 120); CHECK(!win::hos_mouse_pointer(&out));
    win::covered = false; win::pointer_update(win::hwnd, 0, 0); CHECK(win::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    // Tracking is armed once, rearmed on leave, retried after failure, and cancelled on capture.
    win::move_event(win::hwnd, WM_MOUSEMOVE);win::move_event(win::hwnd, WM_MOUSEMOVE);
    CHECK(win::tracks==1);
    win::move_event(win::hwnd, WM_MOUSELEAVE);win::move_event(win::hwnd, WM_MOUSEMOVE);CHECK(win::tracks==2);
    win::mouse_capture(win::hwnd,1);CHECK(win::cancels==1);
    const auto before=win::queries;
    for(int i=0;i<100;i++){win::move_event(win::hwnd,WM_MOUSEMOVE);win::pointer_update(win::hwnd,0,0);}
    CHECK(win::queries==before && win::tracks==2 && !win::hos_mouse_pointer(&out));
    win::mouse_capture(win::hwnd,0);CHECK(win::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    win::fail_tracking=true;win::move_event(win::hwnd,WM_MOUSEMOVE);CHECK(!win::pointer_tracking);
    win::fail_tracking=false;win::move_event(win::hwnd,WM_MOUSEMOVE);CHECK(win::pointer_tracking);
    // Windows button messages retain their signed client coordinates and event geometry too.
    hos_pointer_reset(&win::pointer);win::origin_x=win::origin_y=0;win::width=1280;win::height=720;
    win::menu_press(win::hwnd, MAKELPARAM(21,32), HOS_MOUSE_LEFT);
    win::cursor_x=900;win::cursor_y=600;win::width=1720;win::pointer_update(win::hwnd,0,0);
    CHECK(win::hos_mouse_pointer(&out) && out.x==21 && out.y==32 && out.width==1280 && out.pressed==HOS_MOUSE_LEFT);
    CHECK(win::hos_mouse_pointer(&out) && out.x==900 && out.width==1720 && !out.pressed);
    // The game thread consumes its own coherent latch, without a window query or stealing Insert input.
    win::menu_open=0;win::game_menu_open=1;win::width=1280;
    win::game_menu_press(MAKELPARAM(41,52),HOS_MOUSE_RIGHT);
    win::width=1720;win::pointer_update(win::hwnd,0,60);win::pointer_update(win::hwnd,0,60);
    MkwVkPointer insert{};insert.width=640;insert.height=480;insert.pressed=HOS_MOUSE_LEFT;insert.wheel=120;
    hos_pointer_publish(&win::pointer,insert,1);
    int gx,gy,gw,gh,wheel;uint32_t pressed;
    std::thread game([&] {
        CHECK(win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel)==1);
        CHECK(gx==41 && gy==52 && gw==1280 && gh==720 && pressed==HOS_MOUSE_RIGHT && wheel==1);
    });game.join();
    CHECK(win::hos_mouse_pointer(&out) && out.pressed==HOS_MOUSE_LEFT && out.wheel==1);
    CHECK(win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel)==1 && gx==900 && gw==1720 && !pressed && !wheel);
    win::shown=false;win::pointer_update(win::hwnd,HOS_MOUSE_LEFT,120);
    CHECK(win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel)==2 && !pressed && wheel==1);
    win::menu_open=1;win::pointer_update(win::hwnd,HOS_MOUSE_RIGHT,120);
    CHECK(!win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel) && !pressed && !wheel);
    win::menu_open=0;win::game_menu_press(MAKELPARAM(10,20),HOS_MOUSE_LEFT);
    win::focus=false;win::pointer_update(win::hwnd,0,0);
    CHECK(!win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel) && !pressed && !wheel);
    win::focus=true;win::menu_open=1;win::game_menu_open=0;
    // Real wheel-message routing and menu transitions preserve priority and discard stale input.
    win::pointer_update(win::hwnd,0,0);win::wheel_event(win::hwnd,MAKEWPARAM(0,120));
    CHECK(win::hos_mouse_pointer(&out) && out.wheel==1);
    CHECK(!win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel) && !wheel);
    win::menus_mouse(win::hwnd,0,1);win::wheel_event(win::hwnd,MAKEWPARAM(0,60));
    CHECK(win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel)==1 && !wheel);
    win::wheel_event(win::hwnd,MAKEWPARAM(0,60));
    CHECK(win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel)==1 && wheel==1);
    CHECK(win::hos_mouse_pointer(&out) && !out.wheel);
    win::game_menu_press(MAKELPARAM(30,40),HOS_MOUSE_LEFT);
    win::menus_mouse(win::hwnd,1,1);
    CHECK(!win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel) && !pressed);
    win::wheel_event(win::hwnd,MAKEWPARAM(0,120));
    CHECK(win::hos_mouse_pointer(&out) && out.wheel==1);
    win::menus_mouse(win::hwnd,0,0);win::wheel_event(win::hwnd,MAKEWPARAM(0,120));
    CHECK(win::play_wheel==120);
    win::play_wheel_ns=-1000000000;win::wheel_event(win::hwnd,MAKEWPARAM(0,120));
    CHECK(win::play_wheel==120);
    win::menus_mouse(win::hwnd,0,1);win::game_menu_press(MAKELPARAM(10,20),HOS_MOUSE_LEFT);
    win::mouse_capture(win::hwnd,1);
    CHECK(!win::hos_mouse_game_pointer(&gx,&gy,&gw,&gh,&pressed,&wheel) && !pressed && !wheel);
    win::mouse_capture(win::hwnd,0);win::menus_mouse(win::hwnd,1,0);

    // Escape can release capture without opening the menu. A later F1 must not restore it.
    win::menu_mouse(win::hwnd,0,0);
    win::esc_had_capture=1;
    win::menu_mouse(win::hwnd,1,0);
    win::menu_mouse(win::hwnd,0,0);
    CHECK(!win::mouse_captured);
    // The opt-in Escape shortcut restores the capture released by that key.
    win::esc_had_capture=1;
    win::menu_mouse(win::hwnd,1,1);
    CHECK(!win::mouse_captured);
    win::menu_mouse(win::hwnd,0,0);
    CHECK(win::mouse_captured);
    // F1 also restores ordinary capture when it was active on entry.
    win::menu_mouse(win::hwnd,1,0);
    CHECK(!win::mouse_captured);
    win::menu_mouse(win::hwnd,0,0);
    CHECK(win::mouse_captured);

    // A click keeps its event position even if SDL's current cursor and geometry advance before a take.
    hos_pointer_reset(&sdl::pointer);sdl::ww=1280;sdl::wh=720;sdl::pw=2560;sdl::ph=1440;
    sdl::x=900;sdl::y=600;sdl::down(101.75f,203.75f);
    sdl::pw=3440;sdl::ph=1440;sdl::pointer_update(0,0);
    int current_w,current_h;sdl::drawable_size(sdl::window,&current_w,&current_h);CHECK(current_w==3440);
    CHECK(sdl::hos_mouse_pointer(&out) && out.pressed==HOS_MOUSE_LEFT);
    CHECK(out.x==203 && out.y==407 && out.width==2560 && out.height==1440);
    CHECK(sdl::hos_mouse_pointer(&out) && !out.pressed && out.width==3440 && out.x==2418 && out.y==1200);
    sdl::down(10,20);sdl::hos_posix_window[0]=0;sdl::pointer_update(0,0);
    CHECK(!sdl::hos_mouse_pointer(&out) && !out.pressed);sdl::hos_posix_window[0]=1;
    sdl::down(10,20);sdl::pointer_publish_at(30,40,HOS_MOUSE_RIGHT,60);sdl::pointer_update(0,60);
    CHECK(sdl::hos_mouse_pointer(&out) && out.pressed==(HOS_MOUSE_LEFT|HOS_MOUSE_RIGHT) && out.wheel==1);
    CHECK(out.x==26 && out.y==40 && out.width==3440);
    CHECK(sdl::hos_mouse_pointer(&out) && !out.pressed && !out.wheel && out.x==2418);
    sdl::request_menu(false,false);
    sdl::esc_had_capture=1;
    sdl::request_menu(true,false);
    sdl::request_menu(false,false);
    CHECK(!sdl::mouse_captured);
    sdl::esc_had_capture=1;
    sdl::request_menu(true,true);
    CHECK(!sdl::mouse_captured);
    sdl::request_menu(false,false);
    CHECK(sdl::mouse_captured);
    sdl::request_menu(true,false);
    CHECK(!sdl::mouse_captured);
    sdl::request_menu(false,false);
    CHECK(sdl::mouse_captured);
    // Concurrent resize/scale samples are never torn. All button edges and wheel steps are drained once.
    HosPointer state = HOS_POINTER_INIT;
    std::atomic<bool> done{false}; std::atomic<int> reads{0};
    std::thread reader([&] {
        MkwVkPointer p{};
        do {
            if (hos_pointer_take(&state, &p)) {
                CHECK(p.y == p.x * 2 && p.width == p.x * 3 && p.height == p.x * 4);
                ++reads;
            }
        } while (!done.load(std::memory_order_acquire));
    });
    for (int i = 1; i <= 100000; ++i) {
        MkwVkPointer p{}; p.x = i, p.y = i * 2, p.width = i * 3, p.height = i * 4;
        hos_pointer_publish(&state, p, 1);
    }
    done.store(true, std::memory_order_release); reader.join(); CHECK(reads > 0);
    MkwVkPointer p{}; p.width = 3440, p.height = 1440, p.pressed = HOS_MOUSE_LEFT, p.wheel = 60;
    hos_pointer_publish(&state, p, 1); p.pressed = HOS_MOUSE_RIGHT; hos_pointer_publish(&state, p, 1);
    CHECK(hos_pointer_take(&state, &out) && out.pressed == (HOS_MOUSE_LEFT | HOS_MOUSE_RIGHT) && out.wheel == 1);
    CHECK(hos_pointer_take(&state, &out) && !out.pressed && !out.wheel);
    hos_pointer_publish(&state, p, 1); hos_pointer_reset(&state);
    int w, h; hos_pointer_size(&state, &w, &h); CHECK(w == 3440 && h == 1440);
    CHECK(!hos_pointer_take(&state, &out) && !out.pressed && !out.wheel && !out.held);
    std::puts("PASS: coherent host pointer samples, main-thread SDL, stationary resize, fractional DPI, captured hot path and event click lifetime");
}
