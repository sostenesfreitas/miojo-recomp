/* Authored memory, clock and API spies; input_actual.inc contains the unmodified production path. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#include "input_constants.inc"
/* API spies must not collide with Windows SDK/import-library names. */
#define GetCurrentProcessId fixture_GetCurrentProcessId
#define InterlockedCompareExchange64 fixture_InterlockedCompareExchange64

typedef void *HWND;
typedef uint32_t DWORD;
typedef intptr_t LONG_PTR;
typedef struct { uint64_t x[32]; } Ctx;
enum { GWL_EXSTYLE = -20, WS_EX_TOOLWINDOW = 0x80, WS_EX_NOACTIVATE = 0x8000000,
       VK_MENU = 0x12, VK_RETURN = 0x0d, VK_LEFT = 0x25, VK_UP, VK_RIGHT, VK_DOWN,
       VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4, VK_XBUTTON1 = 5, VK_XBUTTON2 = 6 };
static int front, menu_background, bootstrap_polls, pad_merges, key_queries, mouse_queries, focus_queries, commands;
typedef int64_t LONG64;
volatile LONG64 hos_present_count = 1000;
static int frame_reads;
LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 value, LONG64 expected) {
    const LONG64 old = *p;
    frame_reads++;
    if (old == expected) *p = value;
    return old;
}
static int64_t now_ns = 10000000000LL, sampling;
typedef long LONG;   /* (the note layer's words: production gets it from windows.h) */
static int key_bind[HOS_KEY_N][2], pad_raw_now;   /* (the keys-only controls after the Switch's buttons) */
typedef int SRWLOCK;   /* (host/nn_hid.c guards the pad table with these since #1990) */
#define SRWLOCK_INIT 0
static void AcquireSRWLockShared(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockShared(SRWLOCK *p) { (void)p; }
static int pad_bind[HOS_KEY_N][2];   /* (the ocarina note layer reads it; hos_oc_wait stays 0, so nothing is consumed) */
static SRWLOCK pad_bind_lock = SRWLOCK_INIT;
static int pad_after_menu;
static LONG oc_consumed_keys[256];
static uint64_t oc_consumed_pads;
volatile uint64_t hos_oc_note_bits; volatile int hos_oc_wait; volatile int hos_oc_last_kind; volatile unsigned hos_oc_beat;
static unsigned char *memory;
static char last_log[256];
static int origin_logs, script_logs;
static long long logged_origin;
volatile int64_t hos_hid_last_read;
volatile int hos_nvn_sync_log_on;
int hos_control_input(uint64_t *buttons, int32_t stick[4]) { (void)buttons; (void)stick; return 0; }   /* (host/control.c: off) */
static void *at(uint64_t address) { CHECK(address <= 512 - 8); return memory + address; }
#define GP(a) at(a)
#define G32(a) (*(uint32_t *)at(a))
#define G64(a) (*(uint64_t *)at(a))
#define ARG(n) (c->x[n])
#define RET(value) (c->x[0] = (uint64_t)(value))

int rx_option(int id, int otherwise) { return id == 4 ? menu_background : id == 1 ? 0 : otherwise; }
int64_t hos_now_ns(void) { return now_ns; }
void hos_prompts_live(void) {}   /* (host/prompts.c: the button prompts follow the controls) */
volatile int hos_prompts_row = -1;   /* (host/prompts.c: "!prompts" in the live input) */
void hos_bind(int set, int button, int slot, int code) { (void)set, (void)button, (void)slot, (void)code; }   /* ("!bind") */
void hos_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof last_log, format, args); va_end(args);
    if (!strncmp(last_log, "hid: input script of ", 21)) {
        const char *stamp = strstr(last_log, "; origin_ns=");
        long long origin;
        CHECK(stamp && sscanf(stamp, "; origin_ns=%lld", &origin) == 1);
        CHECK(origin == now_ns);
        CHECK(++script_logs == 1);
    }
    if (!strncmp(last_log, "hid: input frame clock origin=", 29)) {
        origin_logs++;
        CHECK(sscanf(last_log, "hid: input frame clock origin=%lld native presents; rate=60", &logged_origin) == 1);
        CHECK(logged_origin == 1000);
        CHECK(!strcmp(last_log, "hid: input frame clock origin=1000 native presents; rate=60"));
    }
}
HWND GetForegroundWindow(void) { focus_queries++; return (HWND)(uintptr_t)(front ? 1 : 2); }
DWORD GetWindowThreadProcessId(HWND window, DWORD *pid) { *pid = window == (HWND)(uintptr_t)1 ? 10 : 20; return 1; }
DWORD GetCurrentProcessId(void) { return 10; }
LONG_PTR GetWindowLongPtrA(HWND window, int index) { (void)window; (void)index; return 0; }
int GetClassNameA(HWND window, char *text, int size) { (void)window; return snprintf(text, (size_t)size, "fixture"); }
short GetAsyncKeyState(int key) { key_queries++; return key == 'P' || key == 'D' ? (short)-32768 : 0; }
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) {
    bootstrap_polls++; *buttons = 1ULL << B_Y; stick[0] = -31000; return 1;
}
void pads_read(uint64_t *buttons, int32_t stick[4]) {
    /* The physical backend boundary is a spy; actual routing merges its result with the script below. */
    pad_merges++; *buttons |= 1ULL << B_B; stick[0] = -12000; stick[2] = 8000;
}
void binds_load(void) {
    key_bind[B_X][0] = 'P';
    /* (the left stick's and the camera's keys are bindings too: the defaults, WASD and the arrows) */
    key_bind[HOS_KEY_MOVE_UP][0] = 'W', key_bind[HOS_KEY_MOVE_DOWN][0] = 'S', key_bind[HOS_KEY_MOVE_LEFT][0] = 'A';
    key_bind[HOS_KEY_MOVE_RIGHT][0] = 'D', key_bind[HOS_KEY_CAM_UP][0] = VK_UP, key_bind[HOS_KEY_CAM_DOWN][0] = VK_DOWN;
    key_bind[HOS_KEY_CAM_LEFT][0] = VK_LEFT, key_bind[HOS_KEY_CAM_RIGHT][0] = VK_RIGHT;
}
/* the native mouse look (no camera found: the motion goes to mouse_stick) and the mouse's sensitivity, which
   read_input calls */
int hos_mouse_native(void) { return -1; }
void hos_mouse_native_free(int free) { (void)free; }
void hos_mouse_native_stick(int held) { (void)held; }
static void mouse_setup(void) {}
static void mouse_sens_keys(void) {}
int mouse_stick(int32_t *x, int32_t *y, int held) { (void)x; (void)y; (void)held; mouse_queries++; return 0; }
void rx_request_frame_dump(void) { commands++; }
void rx_set_screenshot_ms(int64_t ms) { (void)ms; commands++; }
void rx_request_rdoc_capture(int frames) { (void)frames; commands++; }
void rx_flash_dark_arm(void) { commands++; }

#include "input_actual.inc"

static void packet(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx) {
    static int64_t expected_sampling;
    Ctx context = {{0}};
    context.x[0] = 64; context.x[1] = 2; context.x[2] = 16; context.x[30] = 0x1234;
    G32(16) = NPAD_NO1;
    memset(memory + 64, 0xa5, 2 * STATE_SIZE + 8);
    npad_states(&context);
    CHECK(context.x[0] == 2);
    expected_sampling++;
    for (int i = 0; i < 2; i++) {
        const uint64_t address = 64 + (uint64_t)i * STATE_SIZE;
        CHECK(G64(address) == (uint64_t)(expected_sampling - i));
        CHECK(G64(address + 8) == buttons);
        CHECK((int32_t)G32(address + 16) == lx && (int32_t)G32(address + 20) == ly);
        CHECK((int32_t)G32(address + 24) == rx && G32(address + 28) == 0);
        CHECK(G32(address + 32) == ATTR_CONNECTED && G32(address + 36) == 0);
    }
    for (int i = 0; i < 8; i++) CHECK(memory[64 + 2 * STATE_SIZE + i] == 0xa5);
    CHECK(commands == 0);
}

static void append_live(const char *text) {
    const char *path = getenv("HOS_INPUT_LIVE"); CHECK(path);
    FILE *file = fopen(path, "ab"); CHECK(file);
    CHECK(fputs(text, file) >= 0 && fclose(file) == 0);
}

static void frame_clock_case(const char *name) {
    const int hold = !strcmp(name, "frame-hold");
    const int live = !strcmp(name, "live-frame");
    const int reuse = !strcmp(name, "reuse-frame");
    const int wall = !strcmp(name, "wall") || !strcmp(name, "zero") || !strcmp(name, "empty");
    CHECK(hold || live || reuse || wall);
    front = hold || wall;
    packet(hold || wall ? 1ULL << B_A : 0, 0, hold || wall ? 32767 : 0, 0);
    if (hold) {
        now_ns += 7000000000LL; /* no native present returns during a compile */
        packet(1ULL << B_A, 0, 32767, 0);
        CHECK(hos_hid_last_read == now_ns); /* HID diagnostics stay wall-clock */
        hos_present_count = 1011;
        packet(1ULL << B_A, 0, 32767, 0);
        hos_present_count = 1012; /* exactly 200 virtual ms, relative to initial frame 1000 */
        packet(0, 0, 0, 0);
        CHECK(pad_merges == 0 && key_queries == 0 && mouse_queries == 0);
        hos_present_count = 1311;
        packet(0, 0, 0, 0); /* exclusive margin still held at 5183 ms */
        hos_present_count = 1312;
        packet((1ULL << B_B) | (1ULL << B_X), 32767, 0, 8000);
        CHECK(pad_merges > 0 && frame_reads > 0);
    } else if (wall) {
        now_ns += 7000000000LL;
        packet((1ULL << B_B) | (1ULL << B_X), 32767, 0, 8000);
        CHECK(pad_merges > 0 && frame_reads == 0);
    } else if (live) {
        append_live("0:A+LUP/200"); now_ns += 30000000;
        packet(0, 0, 0, 0); /* incomplete live line remains unconsumed */
        hos_present_count = 1060;
        append_live("\n"); now_ns += 30000000;
        packet(1ULL << B_A, 0, 32767, 0);
        CHECK(presses[0].start_ms == 1000 && presses[0].end_ms == 1200);
        append_live("0:B/200\n"); now_ns += 5000000;
        packet(1ULL << B_A, 0, 32767, 0); /* file polling stays on the 30 ms wall clock */
        now_ns += 30000000;
        packet((1ULL << B_A) | (1ULL << B_B), 0, 32767, 0);
        CHECK(presses[1].start_ms == 1000);
        now_ns += 7000000000LL;
        packet((1ULL << B_A) | (1ULL << B_B), 0, 32767, 0);
        hos_present_count = 1072;
        packet(0, 0, 0, 0);
    } else {
        for (int i = 0; i < MAX_PRESSES; i++) CHECK(parse_presses("0:A/200", 0, 1) == 1);
        now_ns += 7000000000LL;
        CHECK(parse_presses("0:B/200", 0, 1) == 0);
        hos_present_count = 1012;
        CHECK(parse_presses("0:B/200", 200, 1) == 0); /* original strict reuse boundary */
        hos_present_count = 1013;
        CHECK(parse_presses("0:B/200", 216, 1) == 1);
        packet(1ULL << B_B, 0, 0, 0);
        CHECK(npresses == MAX_PRESSES);
    }
    CHECK(script_logs == (getenv("HOS_INPUT_SCRIPT") && *getenv("HOS_INPUT_SCRIPT") ? 1 : 0));
    CHECK(bootstrap_polls == 1);
    CHECK(origin_logs == (wall ? 0 : 1));
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(!pads_in_background(NULL, 0));   /* out of the box: the window in front only */
    CHECK(pads_in_background(NULL, 1));    /* the menu's row */
    CHECK(pads_in_background("1", 0));     /* HOS_INPUT_BACKGROUND=1 without the menu */
    CHECK(!pads_in_background("0", 1));    /* HOS_INPUT_BACKGROUND=0 wins over the menu */
    memory = calloc(512, 1); CHECK(memory);
    const char *name = argv[1];
    if (!strcmp(name, "frame-hold") || !strcmp(name, "live-frame") || !strcmp(name, "reuse-frame") ||
        !strcmp(name, "wall") || !strcmp(name, "zero") || !strcmp(name, "empty")) {
        frame_clock_case(name);
        printf("PASS: actual input frame clock %s\n", name);
        free(memory);
        return 0;
    }
    front = !strcmp(name, "foreground") || !strcmp(name, "exclusive-foreground");
    menu_background = !strcmp(name, "pads-menu") || !strcmp(name, "pads-off");
    const int exclusive = !strncmp(name, "exclusive-", 10);
    const int pads = !exclusive && (front || !strcmp(name, "pads-env") || !strcmp(name, "pads-menu"));
    const uint64_t physical = pads ? (1ULL << B_B) | (front ? 1ULL << B_X : 0) : 0;
    if (!strcmp(name, "live-background")) {
        packet(0, 0, 0, 0);
        append_live("0:A+LUP/200"); now_ns += 30000000;
        packet(0, 0, 0, 0); /* incomplete authored line is not consumed */
        append_live("\n"); now_ns += 30000000;
        packet(1ULL << B_A, 0, 32767, 0);
        now_ns += 200000000;
        packet(0, 0, 0, 0); /* consumed once, then released at the actual hold boundary */
    } else if (!strcmp(name, "no-script-background")) {
        packet(0, 0, 0, 0);
    } else {
        packet((1ULL << B_A) | physical, pads ? (front ? 32767 : -12000) : 0, 32767, pads ? 8000 : 0);
        now_ns += 201000000;
        packet(physical, pads ? (front ? 32767 : -12000) : 0, 0, pads ? 8000 : 0);
        if (exclusive) {
            CHECK(pad_merges == 0 && key_queries == 0 && mouse_queries == 0);
            now_ns += 5100000000LL;
            packet(front ? (1ULL << B_B) | (1ULL << B_X) : 0, front ? 32767 : 0, 0, front ? 8000 : 0);
        }
    }
    CHECK(script_logs == (getenv("HOS_INPUT_SCRIPT") && *getenv("HOS_INPUT_SCRIPT") ? 1 : 0));
    CHECK(bootstrap_polls == 1); /* production startup poll is allowed, but its Y/stick result is discarded */
    if (!front) {
        CHECK(key_queries == 0 && mouse_queries == 0);
        if (!pads) CHECK(pad_merges == 0);
    }
    if (!strcmp(name, "script-background")) CHECK(strstr(last_log, "game window not in front"));
    if (front) CHECK(pad_merges > 0 && key_queries > 0 && mouse_queries > 0);
    CHECK(focus_queries > 0);
    CHECK(oc_consumed_pads == 0);   /* hos_oc_wait stays 0: the ocarina note layer took no pad button */
    printf("PASS: actual scripted parse/merge/Npad route %s\n", name);
    free(memory);
    return 0;
}
