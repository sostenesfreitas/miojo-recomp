/* nn::hid: one Pro Controller (FullKey style) on Npad No.1, from every XInput gamepad and the keyboard and mouse at
   once. Buttons map by position (the Xbox bottom face button is the Switch's bottom one, B).

   (hookshot) What presses each button, on a gamepad and on the keyboard, is the player's: the Insert menu's Controls
   page (gpu/vk/vk_menu.cpp) changes it, kept in hookshot_options.ini (binds_load). The defaults:
   Keyboard (totk-pc-recomp's layout): WASD left stick, arrows or IJKL right stick (camera), Space X (jump), E A
   (action), Shift B (run/cancel), Q L (abilities), R R, Z ZL, X or F ZR (bow), Ctrl left-stick click (crouch), C
   right-stick click (first person), Tab or Enter Plus, M Minus (map), 1 2 3 4 d-pad left, up, right, down. Only
   while the game's window is in front (the key state is global).
   Mouse (after a click in the window, display.c; Esc releases): its motion is the right stick (mouse panning,
   mouse_stick), or with the Insert menu's "Mouse camera" set to Native (HOS_MOUSE_NATIVE=1 or 0 overrides it) turns
   the camera itself (mouse_native.c); left button Y (attack), right button ZL (shield, lock on). - and = change its sensitivity (x0.8 /
   x1.25, shown in the title bar, kept by the Insert menu's Mouse sensitivity row); HOS_MOUSE_SENS=<x> sets it,
   HOS_MOUSE_INVERT_Y=1 (or the menu's Invert mouse vertical) flips up/down.

   Controllers count only while the game's window is in front, like the keyboard, unless the Insert menu's "Controller in
   background" is on (or HOS_INPUT_BACKGROUND=1; =0 keeps them to the window in front whatever the menu says): then a
   game streamed from a screen of its own, or left running behind another window, still follows the pad. The keyboard
   and mouse stay with the window in front either way.

   HOS_INPUT_SCRIPT (test runs): timed presses added to the input, "<seconds>:<button>[+<button>...][/<ms held>],..."
   with buttons A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN and the stick directions LUP LDOWN LLEFT
   LRIGHT RUP RDOWN RLEFT RRIGHT; held 200 ms unless given. Example: "50:A,52:A,80:LUP/4000".

   Npad states share one layout (NpadCommonState): { s64 sampling; u64 buttons; s32 lx, ly; s32 rx, ry; u32 attributes;
   pad } = 0x28 bytes. */
#include "hos.h"
#include "hos_paths.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include "../gpu/vk/vk_api.h"   /* (the controls' names: HOS_SW_*, HOS_PAD_*) */
#include "hos_motion.h"

enum { B_A = 0, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN };
#define STATE_SIZE 0x28
#define NPAD_NO1 0
#define STYLE_FULLKEY 1u
#define ATTR_CONNECTED 1u

typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
static XInputGetStateFn xinput_get;
static int64_t sampling;

int rx_option(int id, int otherwise);
/* (the Insert menu takes the keyboard while it is open) */
static int key(int vk) { return !rx_option(1, 0) && (GetAsyncKeyState(vk) & 0x8000) != 0; }

int hos_mouse_take(int32_t *dx, int32_t *dy);   /* display.c */

/* The captured mouse's sensitivity (mouse_stick below): - and = change it, kept in <user>/mouse_sens.txt */
double hos_mouse_sens = 0;   /* (display.c shows it in the title) */
extern const char *hos_user_dir;

static void mouse_sens_save(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/mouse_sens.txt", hos_user_dir ? hos_user_dir : "user");
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%.3f\n", hos_mouse_sens); fclose(f); }
}

/* The Insert menu keeps the sensitivity (its Mouse sensitivity row, option 7, in percent), read with each read; - and =
   change it there. HOS_MOUSE_SENS still wins, and then - and = keep it in <user>/mouse_sens.txt as before. A value in
   mouse_sens.txt from before is moved into the menu once, while the menu's is still its default; the file is renamed
   mouse_sens.txt.moved then, so the menu's value is not replaced by it again at a later start */
void rx_option_set(int id, int value);
static int sens_from_file;   /* (mouse_setup: <user>/mouse_sens.txt was there) */
static void mouse_sens_keys(void) {
    static int prev_minus, prev_plus, from_env = -1;
    if (from_env < 0) {
        from_env = getenv("HOS_MOUSE_SENS") && atof(getenv("HOS_MOUSE_SENS")) > 0;
        if (!from_env && sens_from_file && rx_option(7, -1) >= 0) {   /* (the menu is there to take it) */
            if (hos_mouse_sens > 0 && fabs(hos_mouse_sens - 1.0) > 0.01 && rx_option(7, 100) == 100)
                rx_option_set(7, (int)lround(hos_mouse_sens * 100));
            char from[512], to[520];
            snprintf(from, sizeof from, "%s/mouse_sens.txt", hos_user_dir ? hos_user_dir : "user");
            snprintf(to, sizeof to, "%s.moved", from);
            remove(to);
            if (rename(from, to)) hos_log("hid: %s could not be renamed after its move into the menu", from);
        }
    }
    if (!from_env) hos_mouse_sens = rx_option(7, 100) / 100.0;
    const int minus = key(VK_OEM_MINUS) || key(VK_SUBTRACT), plus = key(VK_OEM_PLUS) || key(VK_ADD);
    if ((minus && !prev_minus) || (plus && !prev_plus)) {
        hos_mouse_sens *= minus && !prev_minus ? 0.8 : 1.25;
        if (hos_mouse_sens < 0.1) hos_mouse_sens = 0.1;
        if (hos_mouse_sens > 5) hos_mouse_sens = 5;
        if (from_env) mouse_sens_save();
        else rx_option_set(7, (int)lround(hos_mouse_sens * 100));
    }
    prev_minus = minus;
    prev_plus = plus;
}

/* The mouse as the right stick, "mouse panning" as MacNativeGames does it (games/totk/runtime/mouse_look.h,
   github.com/ly05010419/MacNativeGames, MIT; the idea is yuzu's): every movement adds to a held deflection, and a
   tick every 10 ms shows it to the game and then lets it decay. The camera keeps turning while the mouse moves and
   settles smoothly when it stops, whatever the mouse's report rate, the game's frame rate or how often it reads the
   pad (the old speed-per-8-ms stick jumped between reads). The decay grows with the deflection (1 - length^2 *
   decay), at least min_decay a tick; the held length is cut to 1.5 (past the stick's edge, so a steady turn is easy
   to hold) and shown at most 1. A movement that leaves less than 20% is lifted to 20%, past the game's dead zone.
   MacNativeGames' decay (18%, at least 6%) suits a trackpad: with a mouse the camera drifted on for 0.4 s after it
   stopped. Here 30%, at least 20% (HOS_MOUSE_DECAY / HOS_MOUSE_MIN_DECAY, percent): it stops within ~0.1 s, and one
   raw count moves the stick by sensitivity * 0.03, so at 1 the deflection follows 100-700 counts/s (0.2 to ~0.9)
   and is full from ~1000. */
enum { MOUSE_TICK_NS = 10000000 };
static double look_x, look_y;   /* (the held deflection, stick units, y up) */

static double mouse_percent(const char *name, double fallback) {
    const char *e = getenv(name);
    return e && atof(e) >= 0 && atof(e) <= 100 ? atof(e) / 100 : fallback;
}

static void mouse_look_move(double dx, double dy) {
    const double k = hos_mouse_sens * 0.03;
    look_x += dx * k;
    look_y += dy * k;
    const double length = sqrt(look_x * look_x + look_y * look_y), least = 0.2;
    if (length > 0 && length < least) look_x *= least / length, look_y *= least / length;
}

static void mouse_look_tick(void) {
    static double decay_rate = -1, min_decay;
    if (decay_rate < 0) {
        decay_rate = mouse_percent("HOS_MOUSE_DECAY", 0.30);
        min_decay = mouse_percent("HOS_MOUSE_MIN_DECAY", 0.20);
    }
    double length = sqrt(look_x * look_x + look_y * look_y);
    if (length > 1.5) look_x *= 1.5 / length, look_y *= 1.5 / length, length = 1.5;
    const double c = length < 1 ? length : 1, decay = 1 - c * c * decay_rate, most = 1 - min_decay;
    const double keep = decay < most ? (decay > 0 ? decay : 0) : most;
    look_x *= keep;
    look_y *= keep;
    if (fabs(look_x) < 1e-6 && fabs(look_y) < 1e-6) look_x = look_y = 0;
}

/* (host/oot_menu.c) A drag of the free mouse in the game's own menus (the map, the tapestry): panned as
   the camera's mouse is (mouse_look_move, mouse_look_tick: the same sensitivity, least deflection and decay), on the
   left stick (right = 0: the map's and the tapestry's pan) or the right (1: Key Items' stones turned). The pixels it
   moved (y up) come from the game's thread; the read takes them. The camera's 0.03 a count is for raw counts, several
   to a screen pixel at a mouse's usual DPI: a pixel here is HOS_MENU_DRAG_GAIN of them (6) */
/* (display.c hides the free mouse in the game's menus while the player uses a pad or the keys) the last
   time the player's pads (a button, a stick past a third) or bound keys were used */
static volatile LONG64 player_input_ns;
int64_t hos_player_input_ns(void) { return InterlockedCompareExchange64(&player_input_ns, 0, 0); }
static void player_used(void) { InterlockedExchange64(&player_input_ns, hos_now_ns()); }
static volatile LONG drag_dx, drag_dy, drag_right;
static double drag_x, drag_y;
void hos_hid_drag(int dx, int dy, int right) {
    if (right != (int)drag_right) InterlockedExchange(&drag_right, right);
    InterlockedExchangeAdd(&drag_dx, dx);
    InterlockedExchangeAdd(&drag_dy, dy);
}
static void host_drag_read(int32_t stick[4]) {
    static int64_t next_tick;
    const int64_t now = hos_now_ns();
    const int dx = (int)InterlockedExchange(&drag_dx, 0), dy = (int)InterlockedExchange(&drag_dy, 0);
    if (dx || dy) {
        static double gain = -1;
        if (gain < 0) {
            const char *e = getenv("HOS_MENU_DRAG_GAIN");
            gain = e && atof(e) > 0 ? atof(e) : 6.0;
        }
        const double k = (hos_mouse_sens > 0 ? hos_mouse_sens : 1.0) * 0.03 * gain;
        drag_x += dx * k;
        drag_y += dy * k;
        const double length = sqrt(drag_x * drag_x + drag_y * drag_y), least = 0.2;
        if (length > 0 && length < least) drag_x *= least / length, drag_y *= least / length;
    }
    if (drag_x == 0 && drag_y == 0) { next_tick = 0; return; }
    double ox = drag_x, oy = drag_y;
    const double shown = sqrt(ox * ox + oy * oy);
    if (shown > 1) ox /= shown, oy /= shown;
    const int32_t v[2] = {(int32_t)(ox * 32767), (int32_t)(oy * 32767)};
    const int at = drag_right ? 2 : 0;
    for (int i = 0; i < 2; i++)
        if ((v[i] < 0 ? -v[i] : v[i]) > (stick[at + i] < 0 ? -stick[at + i] : stick[at + i])) stick[at + i] = v[i];
    /* (the decay, a tick each 10 ms since the last read, as the camera's) */
    int64_t ticks = next_tick ? (now - next_tick) / MOUSE_TICK_NS + 1 : 1;
    if (ticks > 10) ticks = 10;
    next_tick = now + MOUSE_TICK_NS;
    static double decay_rate = -1, min_decay;
    if (decay_rate < 0) decay_rate = mouse_percent("HOS_MOUSE_DECAY", 0.30), min_decay = mouse_percent("HOS_MOUSE_MIN_DECAY", 0.20);
    for (int64_t t = 0; t < ticks; t++) {
        double length = sqrt(drag_x * drag_x + drag_y * drag_y);
        if (length > 1.5) drag_x *= 1.5 / length, drag_y *= 1.5 / length, length = 1.5;
        const double c = length < 1 ? length : 1, decay = 1 - c * c * decay_rate, most = 1 - min_decay;
        const double keep = decay < most ? (decay > 0 ? decay : 0) : most;
        drag_x *= keep, drag_y *= keep;
    }
    if (fabs(drag_x) < 1e-6 && fabs(drag_y) < 1e-6) drag_x = drag_y = 0;
}

/* While the ability wheel is open (L held, Q on the keyboard), the mouse points instead of panning (MacNativeGames
   2d0f0f0, MIT): the wheel picks the ability the right stick points at, and with panning the stick settled back to
   the centre as soon as the mouse stopped, taking the pick with it. The stick starts at the centre, stays where the
   mouse puts it (cut to length 1) and is centred again when L is let go, so the camera does not turn by itself after
   the wheel closes. */
static int invert = -1;
static void mouse_setup(void) {   /* (once: the sensitivity kept in <user>/mouse_sens.txt, HOS_MOUSE_SENS, the inversion) */
    if (invert < 0) {
        invert = getenv("HOS_MOUSE_INVERT_Y") != NULL;
        hos_mouse_sens = 1.0;
        char path[512];
        snprintf(path, sizeof path, "%s/mouse_sens.txt", hos_user_dir ? hos_user_dir : "user");
        FILE *f = fopen(path, "r");
        if (f) {
            double v;
            if (fscanf(f, "%lf", &v) == 1 && v > 0) hos_mouse_sens = v;
            fclose(f);
            sens_from_file = 1;
        }
        const char *e = getenv("HOS_MOUSE_SENS");
        if (e && atof(e) > 0) hos_mouse_sens = atof(e);
    }
}

static int mouse_stick(int32_t *rx, int32_t *ry, int pointing) {
    static int64_t next_tick;
    static int32_t shown[2];
    static int captured, was_pointing;
    mouse_setup();
    mouse_sens_keys();
    static int env_invert = -1;   /* (HOS_MOUSE_INVERT_Y, or the menu's Invert mouse vertical, option 8) */
    if (env_invert < 0) env_invert = invert;
    invert = env_invert || rx_option(8, 0);
    const int64_t now = hos_now_ns();
    if (now >= next_tick) {
        int32_t dx, dy;
        captured = hos_mouse_take(&dx, &dy);
        if (pointing != was_pointing) look_x = look_y = 0, was_pointing = pointing;
        if (!captured) look_x = look_y = 0;
        else if (pointing) {
            const double k = hos_mouse_sens * 0.03;
            look_x += dx * k;
            look_y += (invert ? dy : -dy) * k;
            const double length = sqrt(look_x * look_x + look_y * look_y);
            if (length > 1) look_x /= length, look_y /= length;
        } else if (dx || dy) mouse_look_move(dx, invert ? dy : -dy);
        /* (the ticks since the last read: the first shows the deflection, the rest only decay it; at most 10, a
           read after a pause starts afresh) */
        const int64_t ticks = next_tick ? (now - next_tick) / MOUSE_TICK_NS + 1 : 1;
        double ox = look_x, oy = look_y;
        const double shown_length = sqrt(ox * ox + oy * oy);
        if (shown_length > 1) ox /= shown_length, oy /= shown_length;
        shown[0] = (int32_t)lround(ox * 32767);
        shown[1] = (int32_t)lround(oy * 32767);
        if (!pointing)
            for (int64_t i = 0; i < (ticks < 10 ? ticks : 10); i++) mouse_look_tick();
        next_tick = (next_tick && ticks <= 10 ? next_tick + ticks * MOUSE_TICK_NS : now + MOUSE_TICK_NS);
    }
    if (!captured) return 0;
    *rx = shown[0];
    *ry = shown[1];
    return 1;
}

/* A bound key is down: Enter held with Alt is no button (Alt+Enter is fullscreen, host/display.c and
   host/posix/display_sdl.c). tests/test_alt_enter.py */
static int kmap_down(int vk, int (*down)(int)) { return down(vk) && !(vk == VK_RETURN && down(VK_MENU)); }

static int game_in_front(void) {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    int front = pid == GetCurrentProcessId();
#ifndef __EMSCRIPTEN__
    static int last_normal_was_us = 1, was_front = 1;
    if (w && !front) {
        const LONG_PTR ex = GetWindowLongPtrA(w, GWL_EXSTYLE);
        const int overlay = (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0;
        if (overlay && last_normal_was_us) front = 1;
        else last_normal_was_us = 0;
    } else if (front) {
        last_normal_was_us = 1;
    }
    if (front != was_front) {
        char cls[64] = "";
        if (w) GetClassNameA(w, cls, sizeof cls);
        hos_log("hid: game window %s (foreground window: process %lu, class \"%s\")", front ? "in front" : "not in front",
                (unsigned long)pid, cls);
        was_front = front;
    }
#endif
    return front;
}

/* HOS_INPUT_SCRIPT, and HOS_INPUT_LIVE=<file>: lines appended to that file while the game runs are presses in the
   script's syntax, their times counted from when the line is read ("0:LUP/2000,2:A"): a test can steer the game
   by looking at its screenshots. */
typedef struct { int64_t start_ms, end_ms; uint64_t buttons; int32_t stick[4]; } Press;
#define MAX_PRESSES 1024
static Press presses[MAX_PRESSES];
static int npresses = -1;
static int64_t script_t0, script_end_ms;
static int script_frame_clock;
static int64_t script_frame0;
extern volatile LONG64 hos_present_count;   /* native game presents, not generated frames or HID polls */
/* HOS_INPUT_FRAME_CLOCK=1: the existing script seconds/millisecond holds advance at 60 native frames per second.
   Live-file polling and HID diagnostics keep their wall clocks. Default/0/empty retains wall-clock scripts. */
static int64_t script_elapsed_ms(int64_t now) {
    if (!script_frame_clock) return (now - script_t0) / 1000000;
    const int64_t frames = InterlockedCompareExchange64(&hos_present_count, 0, 0) - script_frame0;
    return frames * 1000 / 60;
}

static int parse_presses(const char *s, int64_t base_ms, int live);

static void parse_script(void) {
    {   /* start SDL's controller thread now: pads are open by the time the game window comes to the front */
        int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]);
        uint64_t b = 0;
        int32_t st[4] = {0};
        hos_sdl_pads(&b, st);
    }
    npresses = 0;
    script_t0 = hos_now_ns();
    {
        const char *e = getenv("HOS_INPUT_FRAME_CLOCK");
        script_frame_clock = e && atoi(e) != 0;
        if (script_frame_clock) script_frame0 = InterlockedCompareExchange64(&hos_present_count, 0, 0);
    }
    parse_presses(getenv("HOS_INPUT_SCRIPT"), 0, 0);
    if (npresses)
        hos_log("hid: input script of %d presses (last at %.0f s; the player's devices count too)", npresses,
                script_end_ms / 1000.0);
}

static void live_input(void) {
    static const char *path;
    static int64_t next_check, offset;
    static int looked;
    if (!looked) { looked = 1; path = getenv("HOS_INPUT_LIVE"); }
    if (!path) return;
    const int64_t now = hos_now_ns();
    if (now < next_check) return;
    next_check = now + 30000000;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, (long)offset, SEEK_SET) == 0) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            const size_t n = strlen(line);
            if (!n || line[n - 1] != '\n') break;   /* a line still being written: read it next time */
            offset += (int64_t)n;
            const int64_t at = script_elapsed_ms(now);
            if (line[0] == '!') {   /* commands: !dump = a frame dump of the next frame (as RX_DUMP_AT) */
                void rx_request_frame_dump(void);   /* gpu/render/rx_debug.cpp */
                if (!strncmp(line, "!dump", 5)) rx_request_frame_dump();
                extern volatile int hos_nvn_sync_log_on;   /* nvn_native.c: "!synclog" */
                if (!strncmp(line, "!synclog", 8)) hos_nvn_sync_log_on = 1;
                void rx_set_screenshot_ms(int64_t ms);   /* gpu/render/rx_core.cpp: "!shots <ms>" */
                if (!strncmp(line, "!shots ", 7)) rx_set_screenshot_ms(atoll(line + 7));
                void rx_request_rdoc_capture(int frames);   /* gpu/render/rx_debug.cpp: "!rdoc [frames]" */
                if (!strncmp(line, "!rdoc", 5)) rx_request_rdoc_capture(atoi(line + 5));
                void rx_flash_dark_arm(void);   /* gpu/render/rx_core.cpp: arms RX_FLASH_DARK_DUMP (RX_FLASH_DARK_AFTER=-1) */
                if (!strncmp(line, "!flasharm", 9)) rx_flash_dark_arm();
                extern volatile int hos_oot_live_action;   /* host/hos_main.c: "!cheat <action>", as the Cheats page sends */
                if (!strncmp(line, "!cheat ", 7)) __atomic_store_n(&hos_oot_live_action, atoi(line + 7), __ATOMIC_RELAXED);
                hos_log("hid: live command at %.1f s: %.*s", at / 1000.0, (int)(n > 1 ? n - 1 : 0), line);
                continue;
            }
            const int got = parse_presses(line, at, 1);
            hos_log("hid: live input at %.1f s: %d presses (%.*s)", at / 1000.0, got, (int)(n > 1 ? n - 1 : 0), line);
        }
    }
    fclose(f);
}

static int parse_presses(const char *s, int64_t base_ms, int live) {
    static const struct { const char *name; int b; int axis; int dir; } names[] = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LS", B_LS}, {"RS", B_RS}, {"L", B_L}, {"R", B_R},
        {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP},
        {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN}, {"LUP", -1, 1, 1}, {"LDOWN", -1, 1, -1}, {"LLEFT", -1, 0, -1},
        {"LRIGHT", -1, 0, 1}, {"RUP", -1, 3, 1}, {"RDOWN", -1, 3, -1}, {"RLEFT", -1, 2, -1}, {"RRIGHT", -1, 2, 1}};
    int got = 0;
    const int64_t now_ms = script_elapsed_ms(hos_now_ns());
    while (s && *s) {
        while (*s == ' ' || *s == '\t') s++;
        Press p;
        memset(&p, 0, sizeof p);
        char *end;
        double t = strtod(s, &end);
        if (end == s || *end != ':') break;
        s = end + 1;
        int64_t hold = 200;
        for (;;) {
            size_t n = strcspn(s, "+/,\r\n");
            for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].name) == n && !strncmp(s, names[i].name, n)) {
                    if (names[i].b >= 0) p.buttons |= 1ULL << names[i].b;
                    else p.stick[names[i].axis] = names[i].dir * 32767;
                }
            s += n;
            if (*s != '+') break;
            s++;
        }
        if (*s == '/') hold = strtol(s + 1, (char **)&s, 10);
        p.start_ms = base_ms + (int64_t)(t * 1000);
        p.end_ms = p.start_ms + hold;
        int slot = npresses < MAX_PRESSES ? npresses : -1;
        if (slot < 0)   /* full: reuse a press that is over */
            for (int i = 0; i < MAX_PRESSES && slot < 0; i++)
                if (presses[i].end_ms < now_ms) slot = i;
        if (slot < 0) break;
        if (!live && p.end_ms > script_end_ms) script_end_ms = p.end_ms;
        presses[slot] = p;
        if (slot == npresses) npresses++;
        got++;
        if (*s == ',') s++;
    }
    return got;
}

/* the host's own presses and stick (host/oot_menu.c: the game's menus' mouse), below: taken with the script's */
static void host_taps(void);
static void host_stick_read(int32_t stick[4]);
static void host_drag_read(int32_t stick[4]);   /* (above) */

static void scripted_input(uint64_t *buttons, int32_t stick[4]) {
    if (npresses < 0) parse_script();
    live_input();
    host_taps();
    const int64_t ms = script_elapsed_ms(hos_now_ns());
    for (int i = 0; i < npresses; i++)
        if (ms >= presses[i].start_ms && ms < presses[i].end_ms) {
            *buttons |= presses[i].buttons;
            for (int k = 0; k < 4; k++)
                if (presses[i].stick[k]) stick[k] = presses[i].stick[k];
        }
    host_stick_read(stick);
    host_drag_read(stick);
}

/* (host/oot_menu.c) presses from the host, in the script's syntax ("0:A/100"), from now: the game's pause
   menu's clicks. Any thread; the next read takes them */
static SRWLOCK tap_lock = SRWLOCK_INIT;
static char tap_text[256];
void hos_hid_tap(const char *press) {
    AcquireSRWLockExclusive(&tap_lock);
    const size_t n = strlen(tap_text);
    if (n + strlen(press) + 2 < sizeof tap_text) snprintf(tap_text + n, sizeof tap_text - n, "%s%s", n ? "," : "", press);
    ReleaseSRWLockExclusive(&tap_lock);
}
/* a stick from the host (the pause menu's map: a drag pans, the wheel zooms): the four axes (left x, y,
   right x, y; +-32767) until hold_ms from now, the latest call's */
static volatile LONG host_stick[4];
static volatile LONG64 host_stick_until;
void hos_hid_stick(int lx, int ly, int rx, int ry, int hold_ms) {
    InterlockedExchange(&host_stick[0], lx), InterlockedExchange(&host_stick[1], ly);
    InterlockedExchange(&host_stick[2], rx), InterlockedExchange(&host_stick[3], ry);
    InterlockedExchange64(&host_stick_until, hos_now_ns() + (int64_t)hold_ms * 1000000);
}
static void host_stick_read(int32_t stick[4]) {
    if (hos_now_ns() >= InterlockedCompareExchange64(&host_stick_until, 0, 0)) return;
    {   /* (HOS_MENU_TRACE: the stick as the game reads it, at most 4 a second) */
        static int trace = -1;
        static int64_t logged;
        if (trace < 0) trace = getenv("HOS_MENU_TRACE") != NULL;
        if (trace && hos_now_ns() - logged > 250000000)
            logged = hos_now_ns(), hos_log("hid: the host's stick %ld %ld %ld %ld read", (long)host_stick[0], (long)host_stick[1],
                                          (long)host_stick[2], (long)host_stick[3]);
    }
    for (int i = 0; i < 4; i++) {
        const int32_t v = (int32_t)host_stick[i];
        if ((v < 0 ? -v : v) > (stick[i] < 0 ? -stick[i] : stick[i])) stick[i] = v;
    }
}
static void host_taps(void) {
    char text[sizeof tap_text];
    AcquireSRWLockExclusive(&tap_lock);
    memcpy(text, tap_text, sizeof text);
    tap_text[0] = 0;
    ReleaseSRWLockExclusive(&tap_lock);
    if (text[0]) parse_presses(text, script_elapsed_ms(hos_now_ns()), 1);
}

static void stick_max(int32_t stick[4], const int32_t v[4]) {
    for (int i = 0; i < 4; i++)
        if ((v[i] < 0 ? -v[i] : v[i]) > (stick[i] < 0 ? -stick[i] : stick[i])) stick[i] = v[i];
}

/* ---- (hookshot) the controls: the gamepad input (by position) and the keys that press each Switch button, from
   hookshot_options.ini beside the executable when it has them: pad_<button>=<HOS_PAD_* position, -1 none> and
   key_<button>=<key code>,<key code> (Windows key codes, 0 none), <button> one of bind_names. Read once, at the
   first use; what the file leaves out keeps its default. Keys only, after the Switch's buttons (HOS_KEY_*): the left
   stick's and the camera's directions. A key can be a mouse button (1 2 4 5 6: left, right, middle, 4, 5), which counts
   only while the mouse is captured ---- */
static const char *const bind_names[HOS_KEY_N] = {"a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus",
                                                  "minus", "left", "up", "right", "down", "move_up", "move_down",
                                                  "move_left", "move_right", "cam_up", "cam_down", "cam_left",
                                                  "cam_right"};
static const signed char pad_default[HOS_SW_N] = {
    HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST, HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER,
    HOS_PAD_RSHOULDER, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_START, HOS_PAD_BACK, HOS_PAD_LEFT, HOS_PAD_UP,
    HOS_PAD_RIGHT, HOS_PAD_DOWN};
/* (the keys of totk-pc-recomp's docs/controls.md, github.com/valentderah/totk-pc-recomp, MIT; plus Enter for Plus; the
   mouse's left button Y, attack, and right button ZL, shield and lock-on, as before; WASD the left stick, the arrows or
   IJKL the camera) */
static const unsigned char key_default[HOS_KEY_N][2] = {
    {'E'}, {VK_SHIFT}, {VK_SPACE}, {VK_LBUTTON}, {VK_CONTROL}, {'C'}, {'Q'}, {'R'}, {'Z', VK_RBUTTON}, {'X', 'F'},
    {VK_TAB, VK_RETURN}, {'M'}, {'1'}, {'2'}, {'3'}, {'4'}, {'W'}, {'S'}, {'A'}, {'D'}, {VK_UP, 'I'}, {VK_DOWN, 'K'},
    {VK_LEFT, 'J'}, {VK_RIGHT, 'L'}};
static volatile LONG pad_bind[HOS_SW_N], key_bind[HOS_KEY_N][2];   /* (the menu changes them as the game reads them) */

static void binds_default(void) {
    for (int i = 0; i < HOS_KEY_N; i++) {
        if (i < HOS_SW_N) InterlockedExchange(&pad_bind[i], pad_default[i]);
        for (int k = 0; k < 2; k++) InterlockedExchange(&key_bind[i][k], key_default[i][k]);
    }
}

static void binds_load(void) {
    static volatile LONG state;   /* 0 not read, 1 being read, 2 read */
    if (state == 2) return;
    if (InterlockedCompareExchange(&state, 1, 0) != 0) {
        while (state != 2) Sleep(0);
        return;
    }
    binds_default();
    char path[MAX_PATH];
    const DWORD len = hos_game_path_a(path, MAX_PATH);
    char *slash = len ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;   /* (linux: '/') */
    FILE *f = NULL;
    if (slash) {
        snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "hookshot_options.ini");
        f = fopen(path, "r");
    }
    int changed = 0;
    if (f) {
        char line[80];
        int version = 0, had_keys = 0;   /* (options_version: 3 since the mouse's buttons are bindings) */
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (!strcmp(line, "options_version")) version = atoi(eq + 1);
            const int pad = !strncmp(line, "pad_", 4), keys = !strncmp(line, "key_", 4);
            had_keys |= keys;
            for (int i = 0; (pad || keys) && i < HOS_KEY_N; i++) {
                if (strcmp(line + 4, bind_names[i]) || (pad && i >= HOS_SW_N)) continue;
                changed++;
                if (pad) {
                    const int v = atoi(eq + 1);
                    pad_bind[i] = v >= 0 && v < HOS_PAD_N ? v : -1;
                } else {
                    const char *comma = strchr(eq + 1, ',');
                    const int a = atoi(eq + 1), b = comma ? atoi(comma + 1) : 0;
                    key_bind[i][0] = a > 0 && a < 256 ? a : 0;
                    key_bind[i][1] = b > 0 && b < 256 ? b : 0;
                }
            }
        }
        fclose(f);
        /* An options file from before version 3 wrote key_y and key_zl without the mouse's buttons, which pressed Y and ZL
           by themselves then: they go into the second slot when it is empty and no other control has them */
        if (version < 3 && had_keys) {
            static const int mouse_to[2][2] = {{HOS_SW_Y, VK_LBUTTON}, {HOS_SW_ZL, VK_RBUTTON}};
            for (int m = 0; m < 2; m++) {
                int taken = 0;
                for (int i = 0; i < HOS_KEY_N; i++)
                    for (int k = 0; k < 2; k++) taken |= key_bind[i][k] == mouse_to[m][1];
                if (!taken && !key_bind[mouse_to[m][0]][1]) key_bind[mouse_to[m][0]][1] = mouse_to[m][1];
            }
        }
    }
    if (changed) hos_log("hid: %d controls from hookshot_options.ini", changed);
    InterlockedExchange(&state, 2);
}
int hos_binding(int set, int button, int slot) {
    binds_load();
    if (button < 0 || button >= (set == HOS_BIND_PAD ? HOS_SW_N : HOS_KEY_N) || slot < 0 || slot > 1)
        return set == HOS_BIND_PAD ? -1 : 0;
    return set == HOS_BIND_PAD ? (slot ? -1 : (int)pad_bind[button]) : (int)key_bind[button][slot];
}
/* A gamepad input moved to another button gives that one this button's old input (one button an input, none left
   without); a key moved to it is taken from the others */
void hos_bind(int set, int button, int slot, int code) {
    binds_load();
    if (button < 0) {
        binds_default();
        return;
    }
    if (button >= (set == HOS_BIND_PAD ? HOS_SW_N : HOS_KEY_N) || slot < 0 || slot > 1) return;
    if (set == HOS_BIND_PAD) {
        if (slot || code < -1 || code >= HOS_PAD_N) return;
        const LONG old = pad_bind[button];
        for (int i = 0; i < HOS_SW_N; i++)
            if (i != button && code >= 0 && pad_bind[i] == code) InterlockedExchange(&pad_bind[i], old);
        InterlockedExchange(&pad_bind[button], code);
    } else {
        if (code < 0 || code > 255) return;
        for (int i = 0; i < HOS_KEY_N; i++)
            for (int k = 0; k < 2; k++)
                if (code && key_bind[i][k] == code) InterlockedExchange(&key_bind[i][k], 0);
        InterlockedExchange(&key_bind[button][slot], code);
    }
}

static volatile LONG pad_raw_now;   /* the gamepads' inputs at the last read, 1 << HOS_PAD_* */
uint32_t hos_pad_raw(void) { return (uint32_t)pad_raw_now; }

/* A stick axis cut at the dead zone, as XInput's and SDL's pads both are (pad_sdl.c): zone is the Insert menu's "Stick
   dead zone" (option 11), 0 XInput's own 7849 (~24%, the only one before issue 752), 1 to 5 5%, 10%, 15%,
   20%, 30%. Per axis, as before: the game's own dead zone still follows */
int32_t hos_stick_axis(int16_t v, int zone) {
    static const int32_t cut[6] = {7849, 1638, 3277, 4915, 6553, 9830};
    const int32_t a = v < -32767 ? -32767 : v, t = cut[(unsigned)zone < 6 ? zone : 0];
    return (a > t || a < -t) ? a : 0;
}

static void pads_read(uint64_t *buttons, int32_t stick[4]) {
    static int64_t next_read, retry[XUSER_MAX_COUNT];
    static int was_connected[XUSER_MAX_COUNT];
    static uint64_t cached_buttons;
    static int32_t cached_stick[4];
    const int64_t now = hos_now_ns();
    if (now < next_read) {
        *buttons |= cached_buttons;
        stick_max(stick, cached_stick);
        return;
    }
    next_read = now + 2000000;
    uint64_t raw = 0;   /* (by position: 1 << HOS_PAD_*) */
    int32_t s[4] = {0};
    int sdl_pads = 0;
    {   /* every controller SDL knows (DualSense, DualShock 4, Switch Pro, ...: hos/pad_sdl.c) */
        int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]);
        static int was;
        const int n = hos_sdl_pads(&raw, s);
        if (n != was) hos_log("hid: %d controller%s through SDL", n, n == 1 ? "" : "s");
        was = n;
        sdl_pads = n;
    }
#ifdef __EMSCRIPTEN__
    xinput_get = XInputGetState;
#endif
    /* Polling XInput alongside SDL causes double-reads and conflicting button mappings when Steam Input
       or controller wrappers emulate an XInput controller for a pad SDL is already reading (#1480, #1583).
       Only poll XInput when SDL has no open controllers, unless HOS_XINPUT=1 is explicitly requested. */
    static int poll_xinput_env = -1;
    if (poll_xinput_env < 0) poll_xinput_env = getenv("HOS_XINPUT") != NULL;
    if (!sdl_pads || poll_xinput_env) {
        if (!xinput_get) {
            static int64_t load_retry;
            if (now >= load_retry) {
                load_retry = now + 5000000000LL;
#ifdef _WIN32   /* (hookshot: System32's, not one beside the game, in the current folder or on PATH) */
                HMODULE x = LoadLibraryExA("xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!x) x = LoadLibraryExA("xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
                HMODULE x = LoadLibraryA("xinput1_4.dll");
                if (!x) x = LoadLibraryA("xinput9_1_0.dll");
#endif
                if (x) xinput_get = (XInputGetStateFn)GetProcAddress(x, "XInputGetState");
            }
        }
        static const struct { WORD x; int b; } map[] = {
            {XINPUT_GAMEPAD_A, HOS_PAD_SOUTH}, {XINPUT_GAMEPAD_B, HOS_PAD_EAST}, {XINPUT_GAMEPAD_X, HOS_PAD_WEST},
            {XINPUT_GAMEPAD_Y, HOS_PAD_NORTH}, {XINPUT_GAMEPAD_LEFT_THUMB, HOS_PAD_LSTICK},
            {XINPUT_GAMEPAD_RIGHT_THUMB, HOS_PAD_RSTICK}, {XINPUT_GAMEPAD_LEFT_SHOULDER, HOS_PAD_LSHOULDER},
            {XINPUT_GAMEPAD_RIGHT_SHOULDER, HOS_PAD_RSHOULDER}, {XINPUT_GAMEPAD_START, HOS_PAD_START},
            {XINPUT_GAMEPAD_BACK, HOS_PAD_BACK}, {XINPUT_GAMEPAD_DPAD_LEFT, HOS_PAD_LEFT},
            {XINPUT_GAMEPAD_DPAD_UP, HOS_PAD_UP}, {XINPUT_GAMEPAD_DPAD_RIGHT, HOS_PAD_RIGHT},
            {XINPUT_GAMEPAD_DPAD_DOWN, HOS_PAD_DOWN}};
        for (int p = 0; p < XUSER_MAX_COUNT && xinput_get; p++) {
            if (now < retry[p]) continue;
            XINPUT_STATE xs;
            if (xinput_get((DWORD)p, &xs) != ERROR_SUCCESS) {
                if (was_connected[p]) hos_log("hid: controller %d disconnected", p);
                was_connected[p] = 0;
                retry[p] = now + 1000000000LL;
                continue;
            }
            if (!was_connected[p]) hos_log("hid: controller %d connected", p);
            was_connected[p] = 1;
            const WORD w = xs.Gamepad.wButtons;
            for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
                if (w & map[i].x) raw |= 1ULL << map[i].b;
            if (xs.Gamepad.bLeftTrigger > 64) raw |= 1ULL << HOS_PAD_LTRIGGER;
            if (xs.Gamepad.bRightTrigger > 64) raw |= 1ULL << HOS_PAD_RTRIGGER;
            const int16_t thumb[4] = {xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY, xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY};
            int32_t v[4];
            int rx_option(int id, int otherwise);
            const int zone = rx_option(11, 0);   /* (the menu's Stick dead zone) */
            for (int i = 0; i < 4; i++) v[i] = hos_stick_axis(thumb[i], zone);
            if (rx_option(16, 0)) v[2] = -v[2];
            if (rx_option(17, 0)) v[3] = -v[3];
            stick_max(s, v);
        }
    }
    InterlockedExchange(&pad_raw_now, (LONG)raw);
    /* Back and Start together open the Insert menu (gpu/vk/vk_menu.cpp, for pads whose Home button the system
       keeps): they reach the game only one at a time */
    const uint64_t chord = 1ULL << HOS_PAD_BACK | 1ULL << HOS_PAD_START;
    if ((raw & chord) == chord) raw &= ~chord;
    binds_load();
    uint64_t b = 0;
    for (int i = 0; i < HOS_SW_N; i++)
        if (pad_bind[i] >= 0 && (raw >> pad_bind[i] & 1)) b |= 1ULL << i;
    cached_buttons = b;
    memcpy(cached_stick, s, sizeof s);
    *buttons |= b;
    stick_max(stick, s);
}

/* Whether the controllers count while another window is in front: HOS_INPUT_BACKGROUND=1 or 0 decides when it is set,
   otherwise the Insert menu's "Controller in background" row */
static int pads_in_background(const char *env, int menu) {
    return env ? atoi(env) != 0 : menu != 0;
}

#ifndef VK_XBUTTON1
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#endif
/* A mouse button counts only while the mouse is captured, and one held when the capture began only after it was let go
   (the click that captures never reaches the game) */
static int mouse_vk_slot(int vk) {
    return vk == VK_LBUTTON ? 0 : vk == VK_RBUTTON ? 1 : vk == VK_MBUTTON ? 2 : vk == VK_XBUTTON1 ? 3 : vk == VK_XBUTTON2 ? 4 : -1;
}
static int mouse_armed[5];
static void mouse_buttons_arm(int captured) {   /* (once a read) */
    static const int vks[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int i = 0; i < 5; i++) mouse_armed[i] = captured && (mouse_armed[i] || !key(vks[i]));
}
/* whether a control's key is down: a key always (kmap_down: not Enter with Alt), a mouse button only with mouse (captured
   and armed) */
static int bound_down(int i, int mouse) {
    for (int k = 0; k < 2; k++) {
        const int vk = (int)key_bind[i][k];
        if (vk <= 0) continue;
        const int m = mouse_vk_slot(vk);
        if (m >= 0 ? mouse && mouse_armed[m] && key(vk) : kmap_down(key_bind[i][k], key)) return 1;
    }
    return 0;
}

static uint64_t flight_input;
static uint64_t free_camera_axes;
static int free_camera_height;
static int move_stick_held;   /* (the left stick pushed as the game got it: hos_move_stick_held) */

uint64_t hos_flight_input(void) {
    return __atomic_load_n(&flight_input, __ATOMIC_RELAXED);
}

/* The left stick as the game got it is pushed (the Debug tab's Movement speed scales Link only while the player steers him: not a
   scene's walk) */
int hos_move_stick_held(void) { return __atomic_load_n(&move_stick_held, __ATOMIC_RELAXED); }

uint64_t hos_free_camera_input(int *height) {
    *height = __atomic_load_n(&free_camera_height, __ATOMIC_RELAXED);
    return __atomic_load_n(&free_camera_axes, __ATOMIC_RELAXED);
}

static int flight_jump_shortcut(uint64_t *buttons, int mode, int menu_open, int free_camera) {
    int hos_oot_flight_shortcut_ready(void);
    static int64_t first_press;
    static int held, consume_jump;
    const uint64_t jump = 1ULL << B_B;
    const int down = !!(*buttons & jump);
    if (mode < 0 || menu_open || free_camera || !hos_oot_flight_shortcut_ready() || !game_in_front()) {
        first_press = 0;
        held = 1;
        consume_jump = 0;
        return mode > 0;
    }
    if (down && !held) {
        const int64_t now = hos_now_ns();
        if (first_press && now - first_press <= 300000000) {
            mode = !mode;
            rx_option_set(HOS_OPT_CHEAT_FLIGHT, mode);
            mode = rx_option(HOS_OPT_CHEAT_FLIGHT, -1);
            first_press = 0;
            consume_jump = mode == 0;
        } else {
            first_press = now;
        }
    } else if (!down) {
        consume_jump = 0;
    }
    held = down;
    if (consume_jump) *buttons &= ~jump;
    return mode > 0;
}

static void read_input(uint64_t *buttons, int32_t stick[4]) {
    *buttons = 0;
    memset(stick, 0, 4 * sizeof *stick);
    scripted_input(buttons, stick);
    /* A scripted run (a test) takes no input from the player's devices until 5 s after its last press (a variable
       left set in a shell no longer leaves the game without controls), and otherwise the pads count only
       while the game's window is in front, like the keyboard: XInput is global, so a test run in the background
       used to follow the player's pad (and pressed things in its own menus). */
    /* (hookshot) The player's devices count during a script too (the user plays along in test runs, 2026-11-05):
       the presses add to theirs. HOS_INPUT_EXCLUSIVE=1: the script alone until 5 s after its last press, as before */
    static int exclusive = -1;
    if (exclusive < 0) exclusive = getenv("HOS_INPUT_EXCLUSIVE") != NULL;
    if (exclusive && npresses > 0 && script_elapsed_ms(hos_now_ns()) < script_end_ms + 5000)
        return;
    /* (hookshot, issue 389) A scripted press (HOS_INPUT_SCRIPT, HOS_INPUT_LIVE) is the test's own, not the
       player's: it counts with the game's window behind another one, so a test run can drive the game on one
       monitor while the player works in the front window. HOS_INPUT_BACKGROUND still decides the pads. */
    static int scripted_background = -1;
    if (scripted_background < 0) {
        scripted_background = getenv("HOS_INPUT_SCRIPT_BACKGROUND") != NULL;
        if (scripted_background) hos_log("hid: scripted input runs with the game in the background (HOS_INPUT_SCRIPT_BACKGROUND)");
    }
    if (!game_in_front() && !scripted_background) {   /* (the pads alone, when the player asked for them behind other windows) */
        static const char *env;
        static int looked;
        if (!looked) looked = 1, env = getenv("HOS_INPUT_BACKGROUND");
        if (pads_in_background(env, rx_option(4, 0))) pads_read(buttons, stick);
        return;
    }
    {   /* (hookshot) while the Insert menu is open the pads are its own, as the keyboard is (key()), and after it
           closed until they are let go (the press that closed it never reaches the game) */
        uint64_t pb = 0;
        int32_t ps[4] = {0};
        pads_read(&pb, ps);
        static int after_menu;
        if (rx_option(1, 0)) after_menu = 1;
        else if (after_menu && !pad_raw_now) after_menu = 0;
        if (!after_menu) {
            *buttons |= pb;
            stick_max(stick, ps);
        }
        if (pb || ps[0] > 12000 || ps[0] < -12000 || ps[1] > 12000 || ps[1] < -12000 || ps[2] > 12000 || ps[2] < -12000 ||
            ps[3] > 12000 || ps[3] < -12000)
            player_used();
    }
    binds_load();   /* (the keys: key_default's, or hookshot_options.ini's; the mouse's buttons once its capture is known) */
    for (int i = 0; i < HOS_SW_N; i++)
        if (bound_down(i, 0)) *buttons |= 1ULL << i, player_used();
    int32_t mouse[4] = {0};
    /* the mouse's motion: the camera itself (mouse_native.c) with the menu's Mouse camera on Native, else the right
       stick; also the stick while the player camera does not run (the pause menu, other cameras: mouse_native.c) */
    static const char *native_env;
    static int native_looked;
    if (!native_looked) native_looked = 1, native_env = getenv("HOS_MOUSE_NATIVE");
    const int native = native_env ? atoi(native_env) != 0 : rx_option(6, 0);
    int hos_mouse_native(void);
    int captured = 0, as_stick = !native;
    if (native) {
        mouse_setup();
        mouse_sens_keys();
        captured = hos_mouse_native();
        if (captured < 0) captured = 0, as_stick = 1;
    }
    if (as_stick) {
        captured = mouse_stick(&mouse[2], &mouse[3], (int)(*buttons >> B_L & 1));
        if (captured) stick_max(stick, mouse);
    }
    mouse_buttons_arm(captured);
    if (captured)
        for (int i = 0; i < HOS_SW_N; i++)
            if (bound_down(i, 1)) *buttons |= 1ULL << i;
    /* (the left stick's and the camera's keys: WASD and the arrows or IJKL by default) */
    const int cx = bound_down(HOS_KEY_CAM_RIGHT, captured) - bound_down(HOS_KEY_CAM_LEFT, captured),
              cy = bound_down(HOS_KEY_CAM_UP, captured) - bound_down(HOS_KEY_CAM_DOWN, captured);
    const int32_t keys[4] = {(bound_down(HOS_KEY_MOVE_RIGHT, captured) - bound_down(HOS_KEY_MOVE_LEFT, captured)) * 32767,
                             (bound_down(HOS_KEY_MOVE_UP, captured) - bound_down(HOS_KEY_MOVE_DOWN, captured)) * 32767,
                             cx * 32767, cy * 32767};
    stick_max(stick, keys);
    if (keys[0] || keys[1] || keys[2] || keys[3]) player_used();
}

static void vibration_send(uint64_t handle, uint64_t value);
volatile int64_t hos_hid_last_read;

/* GetNpadStates(State* out, int count, unsigned int const& npadId): the latest state first */
static void npad_states(Ctx *c) {
    uint64_t out = ARG(0);
    int count = (int)ARG(1);
    uint32_t id = G32(ARG(2));
    uint64_t buttons = 0;
    int32_t stick[4] = {0};
    int connected = id == NPAD_NO1;
    {
        static int64_t last_read;
        static uint32_t seen_ids[16];
        static int nseen;
        const int64_t now = hos_now_ns();
        if (connected && last_read && now - last_read > 2000000000LL)
            hos_log("hid: the game read no controller for %.1f s (lr %#llx)", (now - last_read) / 1e9,
                    (unsigned long long)c->x[30]);
        if (connected) last_read = hos_hid_last_read = now;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen_ids[i] == id;
        if (!known && nseen < 16) {
            seen_ids[nseen++] = id;
            hos_log("hid: the game reads npad %#x (%s, lr %#llx)", id, connected ? "No.1" : "not connected",
                    (unsigned long long)c->x[30]);
        }
    }
    if (connected) {
        read_input(&buttons, stick);
        uint64_t axes = 0;
        uint64_t flight = 0;
        int height = 0;
        const int menu_open = rx_option(1, 0);
        const int free_camera = rx_option(HOS_OPT_CHEAT_CAMERA, 0);
        const int flying = flight_jump_shortcut(&buttons, rx_option(HOS_OPT_CHEAT_FLIGHT, -1), menu_open, free_camera);
        extern volatile int hos_oot_jump_held;   /* host/hos_main.c: the Cheats page's Moon jump */
        hos_oot_jump_held = !menu_open && !free_camera && (buttons >> B_B & 1);
        if (free_camera && !menu_open) {
            for (int i = 0; i < 4; i++) axes |= (uint64_t)(uint16_t)stick[i] << (16 * i);
            height = (int)(buttons >> B_R & 1) - (int)(buttons >> B_L & 1);
            memset(stick, 0, sizeof stick);
            buttons &= ~((1ULL << B_L) | (1ULL << B_R));
        } else if (flying && !menu_open) {
            const uint64_t climb = (1ULL << B_B) | (1ULL << B_R);
            const int vertical = !!(buttons & climb) - (int)(buttons >> B_L & 1);
            flight = (uint16_t)stick[0] | (uint64_t)(uint16_t)stick[1] << 16 |
                     (uint64_t)(uint16_t)vertical << 32;
            stick[0] = stick[1] = 0;
            buttons &= ~(climb | (1ULL << B_L));
        }
        __atomic_store_n(&flight_input, flight, __ATOMIC_RELAXED);
        __atomic_store_n(&move_stick_held, abs(stick[0]) > 4096 || abs(stick[1]) > 4096, __ATOMIC_RELAXED);
        __atomic_store_n(&free_camera_axes, axes, __ATOMIC_RELAXED);
        __atomic_store_n(&free_camera_height, height, __ATOMIC_RELAXED);
    }
    uint64_t hos_title_input(uint64_t buttons);   /* totk_title.c: the title menu's credits entries */
    if (connected) buttons = hos_title_input(buttons);
    uint64_t hos_frame_step_buttons(uint64_t buttons);   /* nn_vi.c: the frame step of the wheel and quick menus */
    if (connected) buttons = hos_frame_step_buttons(buttons);
    int64_t s = ++sampling;
    for (int i = 0; i < count; i++) {
        uint64_t e = out + (uint64_t)i * STATE_SIZE;
        memset(GP(e), 0, STATE_SIZE);
        G64(e) = (uint64_t)(s - i);
        G64(e + 8) = buttons;
        memcpy(GP(e + 16), stick, 16);
        G32(e + 32) = connected ? ATTR_CONNECTED : 0;
    }
    RET(count);
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj) { npad_states(c); }
/* GetNpadStyleSet(unsigned int const&) -> BitFlagSet<32, NpadStyleTag> */
HLE(_ZN2nn3hid15GetNpadStyleSetERKj) { RET(G32(ARG(0)) == NPAD_NO1 ? STYLE_FULLKEY : 0); }

HLE(_ZN2nn3hid14InitializeNpadEv) {}
HLE(_ZN2nn3hid18InitializeKeyboardEv) {}
HLE(_ZN2nn3hid15InitializeMouseEv) {}
HLE(_ZN2nn3hid14DisconnectNpadERKj) {}
HLE(_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE) {}
HLE(_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm) {}
HLE(_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {}
HLE(_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE) {}

/* ShowControllerSupport(ControllerSupportResultInfo*, ControllerSupportArg const&) -> Result: one player, No.1 */
HLE(_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE) {
    memset(GP(ARG(0)), 0, 8);
    *(uint8_t *)GP(ARG(0)) = 1;
    G32(ARG(0) + 4) = NPAD_NO1;
    RET(0);
}

/* Handles: six-axis ones are opaque u32s; a vibration handle is {u8 style type (3 = full key), u8 npad id, u8 device
   index (0 left, 1 right), u8 reserved}, which the SDK's vibration classes read. Vibration goes nowhere yet. */
HLE(_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    /* (hookshot) OoT asks for two (a Joy-Con pair) but runs its motion path only when three come back: at two its
       third source (the console's own, through JCMS) stays dead, and that branch is what marks motion active for the
       aim (spluton, issue 112). The count says three; only the handles asked for are written, so a caller's array
       sized for two is never written past (the handles are opaque: GetSixAxisSensorStates does not read them). */
    const int asked = (int)ARG(1) < 0 ? 0 : (int)ARG(1) > 16 ? 16 : (int)ARG(1);
    static int logged;
    if (!logged) logged = 1, hos_log("hid: six-axis handles: %d asked for, %d reported", asked, asked < 3 ? 3 : asked);
    for (int i = 0; i < asked; i++) G32(ARG(0) + 4 * (uint64_t)i) = 0x100u + (uint32_t)i;
    RET(asked < 3 ? 3 : asked);
}
/* SixAxisSensorState, 0x60 bytes: {i64 delta time (ns), i64 sampling number, f32 acceleration[3] (G), angular velocity[3]
   (revolutions a second), angle[3] (revolutions), direction[3][3], u32 attributes (1 connected), pad}. Newest first.
   (hookshot) With Motion controls on and a pad with a gyroscope (host/pad_sdl.c), the state OoT aims with
   (host/hos_motion.h: the full state, as the console and Eden build it); else a pad lying still. */
HLE(_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE) {
    /* (the sampling number goes up once per reading, as the hardware's does: while live it is the readings' own count,
       moved on past the last number handed out; each entry is its own reading, with the time since the one before) */
    static int64_t six_sampling, six_base;
    static int was_live;
    const uint64_t out = ARG(0);
    const int count = (int)ARG(1) < 1 ? 0 : (int)ARG(1) > HOS_MOTION_HISTORY ? HOS_MOTION_HISTORY : (int)ARG(1);
    static const float at_rest[18] = {0, 0, -1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    int hos_sdl_motion_history(HosMotionSample *out, int max);   /* pad_sdl.c */
    HosMotionSample h[HOS_MOTION_HISTORY];
    const int live = count ? hos_sdl_motion_history(h, count) : 0;
    /* (each switch between live and at rest moves the numbers a whole history past the last ones given out, so no entry
       of one ever has a number an entry of the other had) */
    if (live) {
        if (!was_live || six_base + h[0].n < six_sampling) six_base = six_sampling + HOS_MOTION_HISTORY - h[0].n;
        six_sampling = six_base + h[0].n;
    } else {
        six_sampling += was_live ? HOS_MOTION_HISTORY : 1;
    }
    was_live = live > 0;
    const int n = live ? live : count;
    for (int i = 0; i < n; i++) {
        const uint64_t e = out + (uint64_t)i * 0x60;
        float v[18];
        memset(GP(e), 0, 0x60);
        if (live) {
            const HosMotionState *m = &h[i].st;
            memcpy(v, m->accel, 12), memcpy(v + 3, m->gyro, 12), memcpy(v + 6, m->rotation, 12), memcpy(v + 9, m->direction, 36);
            G64(e) = (uint64_t)(h[i].dt_ns > 0 ? h[i].dt_ns : 5000000);
            G64(e + 8) = (uint64_t)(six_base + h[i].n);
        } else {
            memcpy(v, at_rest, sizeof v);
            G64(e) = 5000000;
            G64(e + 8) = (uint64_t)(six_sampling - i);
        }
        memcpy(GP(e + 0x10), v, sizeof v);
        G32(e + 0x58) = 1;
    }
    RET(n);
}
HLE(_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    int n = (int)ARG(1) < 2 ? (int)ARG(1) : 2;
    for (int i = 0; i < n; i++) G32(ARG(0) + 4 * (uint64_t)i) = 3u | (G32(ARG(2)) & 0xFF) << 8 | (uint32_t)i << 16;
    RET(n);
}
HLE(_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) {}
HLE(_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) { vibration_send(ARG(0), ARG(1)); }

/* nn::hid::detail vibration: what the SDK's own classes (VibrationNode, VibrationTarget, VibrationPlayer) call; on
   hardware each is an IPC to hid. A device is an LRA on its side of the controller, always mounted and permitted. */
/* A vibration value for one of the two motors of the pad (handle +2 = device index): the pads that rumble (SDL) get
   the stronger of the two sides' heavy and light motor strengths. (OoT sends both sides the same values.) */
/* (issue 263) An HD rumble amplitude is not a motor's: OoT asks for 0.02-0.06 for most effects, a few percent of a DualSense's or an
   Xbox pad's motor, nothing you feel. Eden's SDL driver's mapping (Eglantine src/input_common/drivers/sdl_driver.cpp, GPL-3.0):
   (a + a^0.35) / 2, then less for higher frequencies, to give some feel of them */
static float rumble_motor(float amp, float freq, float start, float width) {
    if (amp <= 0.0f) return 0.0f;
    float scale = 1.0f;
    if (freq > start) {
        scale = 1.0f - (freq - start) / width;
        if (scale < 0.3f) scale = 0.3f;
    }
    const float boosted = (amp + powf(amp, 0.35f)) * 0.5f;
    return boosted * scale > 1.0f ? 1.0f : boosted * scale;
}
/* How much of a band at this frequency the heavy (low-frequency) motor plays, 0 to 1; the light motor plays the rest.
   The Switch's HD rumble bands overlap (low about 41-626 Hz, high about 82-1253 Hz, 160 and 320 Hz by default; OoT asks
   for about 100-130 Hz low and 250-300 Hz high), so the band a value came in says less than its frequency: up to 160 Hz
   is all heavy, from 200 Hz all light, and between them a crossfade on a log scale (log2(1.25), about a third of an
   octave). The defaults and what OoT sends land on the same motors as before, with room below OoT's high band; a high
   band played low reaches the heavy motor, a low band played high the light one. */
#define RUMBLE_HEAVY_TOP 160.0f
#define RUMBLE_LIGHT_BOTTOM 200.0f
static float rumble_heavy_share(float freq) {
    if (!(freq > RUMBLE_HEAVY_TOP)) return 1.0f;   /* (also a NaN or a 0 Hz value: the heavy motor, as the low band went) */
    if (freq >= RUMBLE_LIGHT_BOTTOM) return 0.0f;
    const float s = 1.0f - log2f(freq / RUMBLE_HEAVY_TOP) / log2f(RUMBLE_LIGHT_BOTTOM / RUMBLE_HEAVY_TOP);
    return s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;   /* (rounding: sqrtf below takes 1 - s) */
}
/* One VibrationValue {amp_low, freq_low, amp_high, freq_high} -> the motors' strengths, 0 to 1: motor[0] heavy, motor[1]
   light. Each band keeps its strength (rumble_motor, with its band's falloff) and is split between the motors by its
   frequency with constant power (sqrt of the shares), so a band in the crossfade is not felt weaker; each motor takes
   the stronger of the two bands' parts. */
static void rumble_map(const float v[4], float motor[2]) {
    const float low = rumble_motor(v[0], v[1], 140.0f, 400.0f), high = rumble_motor(v[2], v[3], 200.0f, 700.0f);
    const float lh = rumble_heavy_share(v[1]), hh = rumble_heavy_share(v[3]);
    const float heavy[2] = {low * sqrtf(lh), high * sqrtf(hh)}, light[2] = {low * sqrtf(1.0f - lh), high * sqrtf(1.0f - hh)};
    motor[0] = heavy[0] > heavy[1] ? heavy[0] : heavy[1];
    motor[1] = light[0] > light[1] ? light[0] : light[1];
}
static void vibration_send(uint64_t handle, uint64_t value) {
    static float amp[2][2];
    float v[4];
    memcpy(v, GP(value), sizeof v);
    const int dev = (G32(handle) >> 16) & 1;
    rumble_map(v, amp[dev]);
    {   /* (hookshot, issue 263) what the game asks for: the first values, then a count now and then */
        static long long n, strong;
        static float peak;
        if (v[0] > peak) peak = v[0];
        if (v[2] > peak) peak = v[2];
        if ((v[0] > 0.01f || v[2] > 0.01f) && strong++ < 6)
            hos_log("hid: vibration #%lld: handle %08x, low %.2f @ %.0f Hz, high %.2f @ %.0f Hz", n, G32(handle), v[0], v[1], v[2], v[3]);
        if ((++n & 4095) == 0) hos_log("hid: %lld vibration values, strongest %.2f since the last count", n, peak), peak = 0;
    }
    void hos_sdl_rumble_hd(int side, const float value[4]);   /* pad_sdl.c: the value as it is, for HD rumble */
    hos_sdl_rumble_hd(dev, v);
    void hos_sdl_rumble(float low, float high);   /* pad_sdl.c */
    hos_sdl_rumble(amp[0][0] > amp[1][0] ? amp[0][0] : amp[1][0], amp[0][1] > amp[1][1] ? amp[0][1] : amp[1][1]);
}
static void vibration_value_idle(uint64_t v) {   /* VibrationValue {f32 amp_low, freq_low, amp_high, freq_high} */
    float idle[4] = {0.0f, 160.0f, 0.0f, 320.0f};
    memcpy(GP(v), idle, sizeof idle);
}
HLE(_ZN2nn3hid6detail15PermitVibrationEb) { RET(0); }
HLE(_ZN2nn3hid6detail20IsVibrationPermittedEPb) { *(uint8_t *)GP(ARG(0)) = 1; RET(0); }
HLE(_ZN2nn3hid6detail27BeginPermitVibrationSessionEv) { RET(0); }
HLE(_ZN2nn3hid6detail25EndPermitVibrationSessionEv) { RET(0); }
HLE(_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) { RET(0); }
/* GetVibrationDeviceInfo(VibrationDeviceInfo* {u32 type; u32 position}, handle const&): LRA (1), left 1 / right 2 */
HLE(_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE) {
    G32(ARG(0)) = 1;
    G32(ARG(0) + 4) = ((G32(ARG(1)) >> 16) & 1) + 1;
    RET(0);
}
HLE(_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) {
    vibration_send(ARG(0), ARG(1));
    RET(0);
}
HLE(_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi) {
    for (int i = 0; i < (int)ARG(2) && i < 8; i++) vibration_send(ARG(0) + 4 * (uint64_t)i, ARG(1) + 16 * (uint64_t)i);
    RET(0);
}
HLE(_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE) {
    vibration_value_idle(ARG(0));
    RET(0);
}
HLE(_ZN2nn3hid6detail25SendVibrationGcErmCommandERKNS0_21VibrationDeviceHandleERKNS0_21VibrationGcErmCommandE) { RET(0); }
HLE(_ZN2nn3hid6detail30GetActualVibrationGcErmCommandEPNS0_21VibrationGcErmCommandERKNS0_21VibrationDeviceHandleE) {
    G32(ARG(0)) = 0;   /* Stop */
    RET(0);
}
HLE(_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE) { *(uint8_t *)GP(ARG(0)) = 1; RET(0); }
HLE(_ZN2nn3hid6detail21EnqueueVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) {
    vibration_send(ARG(0), ARG(1));
    RET(0);
}
HLE(_ZN2nn3hid6detail28FlushEnqueuedVibrationValuesEv) { RET(0); }

/* ---- (mkw-recomp) the touch screen: none touched. GetTouchScreenStates<16>(TouchScreenState<16>*, int count) ->
   states written: one, with no touches (the lifted SDK version read the touch screen's shared memory, which this
   runtime does not have). TouchScreenState<16>: {s64 sampling number, s32 count, s32 reserved, TouchState[16]}. */
HLE(_ZN2nn3hid21InitializeTouchScreenEv) {}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    static volatile LONG64 sampling;
    if ((int)ARG(1) < 1) { RET(0); return; }
    memset(GP(ARG(0)), 0, 16 + 16 * 0x28);
    G64(ARG(0)) = (uint64_t)InterlockedIncrement64(&sampling);
    RET(1);
}
