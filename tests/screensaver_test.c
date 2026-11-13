/* All APIs are mocks: no window, screen saver, security or power setting is accessed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void *HWND;
typedef void *HRAWINPUT;
typedef unsigned UINT;
typedef int BOOL;
typedef int32_t LONG;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define LOWORD(v) ((unsigned)(uintptr_t)(v) & 0xffff)
#define HIWORD(v) (((unsigned)(uintptr_t)(v) >> 16) & 0xffff)
enum {
    WM_TIMER = 0x113, WM_SYSCOMMAND = 0x112, WM_INPUT = 0xff, WM_LBUTTONDOWN = 0x201,
    WM_KEYDOWN = 0x100, WM_SYSKEYDOWN = 0x104, WM_SYSCHAR = 0x106, WM_APP = 0x8000,
    WM_DPICHANGED = 0x2e0, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_CANCELMODE = 0x1f, WM_SETCURSOR = 0x20,
    WM_MOVE = 3, WM_SIZE = 5, WM_PAINT = 0xf, WM_ERASEBKGND = 0x14, WM_CLOSE = 0x10,
    VK_ESCAPE = 0x1b, VK_F11 = 0x7a, VK_RETURN = 0xd, HTCLIENT = 1,
    VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    SC_SCREENSAVE = 0xf140, SCF_ISSECURE = 1, SPI_GETSCREENSAVESECURE = 0x76,
    SC_CLOSE = 0xf060, SC_MONITORPOWER = 0xf170, SC_MAXIMIZE = 0xf030,
    SWP_NOZORDER = 4, SWP_NOACTIVATE = 0x10, RID_INPUT = 0x10000003,
    RIM_TYPEMOUSE = 0, MOUSE_MOVE_ABSOLUTE = 1
};
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { UINT dwType; } RAWINPUTHEADER;
typedef struct {
    RAWINPUTHEADER header;
    struct { struct { unsigned short usFlags; LONG lLastX, lLastY; } mouse; } data;
} RAWINPUT;

#define CHECK(name, condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__); exit(1); } } while (0)
static HWND game = (HWND)(uintptr_t)1, foreground;
static int renderer, fullscreen, mouse_captured, pointer_tracking;
static int esc_had_capture __attribute__((unused));   /* (Esc's note: host/display.c sets it here) */
static volatile LONG mouse_dx, mouse_dy;
static int iconic, query_ok, query_writes, policy_secure, query_calls, forwarded, cases;
static UINT forwarded_msg;
static WPARAM forwarded_wp;
static LPARAM forwarded_lp;
static const LRESULT default_result = 1234;

HWND GetForegroundWindow(void) { return foreground; }
short GetKeyState(int vkey) { (void)vkey; return 0; }
BOOL IsIconic(HWND w) { CHECK("iconic query targets game", w == game); return iconic; }
/* (#1532: the display's idle timer reset on WM_TIMER while the game is in front) */
typedef unsigned long long ULONGLONG;
typedef unsigned EXECUTION_STATE;
#define ES_SYSTEM_REQUIRED 0x1u
#define ES_DISPLAY_REQUIRED 0x2u
static ULONGLONG tick = 100000;
static int awake_calls;
static EXECUTION_STATE awake_flags;
ULONGLONG GetTickCount64(void) { return tick; }
EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE s) { awake_calls++, awake_flags = s; return 0; }
BOOL SystemParametersInfoA(UINT action, UINT parameter, void *value, UINT flags) {
    CHECK("only read the secure screen saver policy", action == SPI_GETSCREENSAVESECURE);
    CHECK("no profile write/notification flags", parameter == 0 && flags == 0 && value != NULL);
    query_calls++;
    if (query_writes) *(BOOL *)value = policy_secure;
    return query_ok;
}
static LRESULT DefWindowProcA(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    CHECK("forward original window", w == game);
    forwarded++;
    forwarded_msg = msg; forwarded_wp = wp; forwarded_lp = lp;
    return default_result;
}

/* The complete procedure compiles, but these unrelated branches must never run in this fixture. */
static void unexpected(void) { fputs("FAIL: unrelated window action executed\n", stderr); abort(); }
static UINT GetRawInputData(HRAWINPUT h, UINT command, void *data, UINT *size, UINT header) {
    (void)h; (void)command; (void)data; (void)size; (void)header; unexpected(); return (UINT)-1;
}
static LONG InterlockedExchangeAdd(volatile LONG *target, LONG value) {
    (void)target; (void)value; unexpected(); return 0;
}
static void mouse_capture(HWND w, int on) { (void)w; (void)on; unexpected(); }
static void set_fullscreen(HWND w, int on) { (void)w; (void)on; unexpected(); }
static BOOL SetWindowPos(HWND w, HWND after, int x, int y, int width, int height, UINT flags) {
    (void)w; (void)after; (void)x; (void)y; (void)width; (void)height; (void)flags; unexpected(); return FALSE;
}
static void *SetCursor(void *cursor) { (void)cursor; unexpected(); return NULL; }
static void rx_resize(unsigned width, unsigned height) { (void)width; (void)height; unexpected(); }
static BOOL ValidateRect(HWND w, const RECT *rect) { (void)w; (void)rect; unexpected(); return FALSE; }
static void hos_relaunch_ask(void) { unexpected(); }         /* (host/hos_relaunch.c: WM_APP + 9 and WM_CLOSE end the process) */
static void hos_window_closed_exit(void) { unexpected(); }
static void rx_close_asked(void) { unexpected(); }   /* (host/rx_backend.c: WM_CLOSE, not one of the checked messages) */
void hos_vk_note_key_press(unsigned key) { (void)key; }   /* (host/rx_backend.c: the menu notes key presses; nothing here) */

/* (PR 946: the Insert menu takes the mouse while it is open; these branches never run here either) */
enum {
    WM_MOUSEMOVE = 0x200, WM_MOUSELEAVE = 0x2a3, WM_MOUSEWHEEL = 0x20a, WM_LBUTTONUP = 0x202,
    WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208,
    MK_LBUTTON = 1, MK_RBUTTON = 2, MK_MBUTTON = 0x10, TME_LEAVE = 2,
    HOS_MOUSE_LEFT = 1, HOS_MOUSE_RIGHT = 2, HOS_MOUSE_MIDDLE = 4
};
typedef struct { UINT cbSize; UINT dwFlags; HWND hwndTrack; UINT dwHoverTime; } TRACKMOUSEEVENT;
#define GET_WHEEL_DELTA_WPARAM(v) ((short)HIWORD(v))
static int pointer, game_pointer;
static void hos_pointer_reset(int *p) { (void)p; unexpected(); }
static int pointer_updates;
static void pointer_update(HWND w, uint32_t pressed, int wheel) {
    CHECK("timer refresh targets game without input edges", w == game && pressed == 0 && wheel == 0);
    pointer_updates++;
}
static int menu_open;
static BOOL TrackMouseEvent(TRACKMOUSEEVENT *t) { (void)t; unexpected(); return FALSE; }
static HWND GetCapture(void) { unexpected(); return NULL; }
static BOOL ReleaseCapture(void) { unexpected(); return FALSE; }
static void menu_press(HWND w, LPARAM lp, LONG bit) { (void)w; (void)lp; (void)bit; unexpected(); }
static void menu_mouse(HWND w, int open, int opened_by_escape) { (void)w; (void)open; (void)opened_by_escape; unexpected(); }
/* (the game's own menus take the mouse while one of them is shown (host/oot_menu.c); never here either) */
typedef int64_t LONG64;
typedef struct { LONG x, y; } POINT;
static int game_menu_open;
static volatile LONG64 game_menu_seen_ns;
static volatile LONG play_wheel;
static volatile LONG64 play_wheel_ns;
static LONG gm_mx, gm_my;
static LONG64 InterlockedCompareExchange64(volatile LONG64 *target, LONG64 value, LONG64 compare) {
    const LONG64 was = *target;
    if (was == compare) *target = value;
    return was;
}
static int64_t hos_now_ns(void) { return 0; }
static void gm_mouse_used(void) { unexpected(); }
static int gm_cursor_shown(void) { unexpected(); return 1; }
static void game_menu_press(LPARAM lp, LONG bit) { (void)lp; (void)bit; unexpected(); }
static void menus_mouse(HWND w, int insert_open, int game_open) { (void)w; (void)insert_open; (void)game_open; unexpected(); }
static void gm_cursor_off(HWND w) { (void)w; unexpected(); }

/* ACTUAL_WNDPROC */

static void reset(void) {
    renderer = 1; foreground = game; iconic = 0;
    query_ok = 1; query_writes = 1; policy_secure = FALSE;
    query_calls = forwarded = 0;
}
static void call(const char *name, UINT msg, WPARAM wp, int handled, int queries) {
    const LPARAM lp = 0x5678;
    const LRESULT result = wndproc(game, msg, wp, lp);
    CHECK(name, result == (handled ? 0 : default_result));
    CHECK("forwarding count", forwarded == !handled);
    CHECK("policy query count", query_calls == queries);
    if (!handled) CHECK("forward original message and parameters",
                        forwarded_msg == msg && forwarded_wp == wp && forwarded_lp == lp);
    cases++;
}
int main(void) {
    reset(); call("foreground nonsecure saver is handled", WM_SYSCOMMAND, SC_SCREENSAVE, 1, 1);
    reset(); call("secure request is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE | SCF_ISSECURE, 0, 0);
    reset(); call("secure request flag survives other low bits", WM_SYSCOMMAND, SC_SCREENSAVE | SCF_ISSECURE | 8, 0, 0);
    reset(); policy_secure = TRUE; call("password policy is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 1);
    reset(); policy_secure = 2; call("any true policy is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 1);
    reset(); query_ok = 0; call("failed policy query is forwarded even if it wrote false", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 1);
    reset(); query_ok = 0; query_writes = 0; call("unknown policy is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 1);
    reset(); query_writes = 0; call("unwritten policy stays secure", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 1);
    reset(); renderer = 0; call("no renderer is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 0);
    reset(); foreground = (HWND)(uintptr_t)2; call("background game is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 0);
    reset(); foreground = NULL; call("unknown foreground is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 0);
    reset(); iconic = 1; call("minimized game is forwarded", WM_SYSCOMMAND, SC_SCREENSAVE, 0, 0);
    /* (#1532) the display kept on: in front, at most once in 30 s, not minimized, not in the background, no ES_CONTINUOUS */
    reset(); awake_calls = 0; pointer_updates = 0;
    CHECK("timer handled", wndproc(game, WM_TIMER, 1, 0) == 0);
    CHECK("foreground timer keeps the display on", awake_calls == 1 && awake_flags == (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED));
    wndproc(game, WM_TIMER, 1, 0);
    CHECK("at most once in 30 s", awake_calls == 1);
    tick += 30000; iconic = 1; wndproc(game, WM_TIMER, 1, 0);
    CHECK("not while minimized", awake_calls == 1);
    iconic = 0; foreground = NULL; wndproc(game, WM_TIMER, 1, 0);
    CHECK("not in the background", awake_calls == 1);
    foreground = game; wndproc(game, WM_TIMER, 1, 0);
    CHECK("again after 30 s in front", awake_calls == 2);
    CHECK("every timer still refreshes pointer state", pointer_updates == 5);
    reset(); call("close command is forwarded", WM_SYSCOMMAND, SC_CLOSE, 0, 0);
    reset(); call("monitor power command is forwarded", WM_SYSCOMMAND, SC_MONITORPOWER, 0, 0);
    reset(); call("maximize command is forwarded", WM_SYSCOMMAND, SC_MAXIMIZE, 0, 0);
    reset(); call("unknown command is forwarded", WM_SYSCOMMAND, 0xf2e0, 0, 0);
    reset(); call("zero command is forwarded", WM_SYSCOMMAND, 0, 0, 0);
    reset(); call("unrelated message is forwarded", WM_APP + 99, 0xabc, 0, 0);
    printf("PASS: %d actual-wndproc screensaver/secure-policy/forwarding cases; all APIs mocked\n", cases);
    return 0;
}
