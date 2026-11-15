"""The Insert menu's text fields: a row whose value is typed (the first one is Custom output size, the Output resolution's
size of the player's own, which before could only be set in hookshot_options.ini).

The menu's own functions are cut out of gpu/vk/vk_menu.cpp by their text and driven with key and text events as the host
gives them (MkwVkHost.take_text): typing, the caret's keys, Backspace and Delete, the length limit, printable ASCII with
full-width digits/x folded before filtering, Enter and Esc; then Custom output size's Enter, which sets the Output resolution
row to the size typed, or says why not. The real focused menu_tick branch handles pad-only entry, editing and the wheel.
Raw pad polling and repeats are compiled with the focused branch beside the real hold-to-toggle helper: gameplay
latches cannot become text input, held confirm/shoulder buttons need a new press, and the editor's keys still work.
The host's side, cut out of host/rx_backend.c, is compiled as C: the bounded queue, focus, UTF-8 and first-line paste.
The Windows focused key branch is run with key-state stubs to check paste repeats. Wiring checks allow later API members
after take_text while requiring host_take_text in its matching slot. No GPU, game or window.

Each generated source opts out of Microsoft's secure-CRT deprecation annotations before any header, as the other
fixtures do. The test still treats warnings as errors and executes the same field and queue checks on Windows.

    python -I -B tests/test_menu_text_field.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    """The text from marker to the end of the brace block that follows it (a function, or a struct with its ;)"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + (";" if text[end:end + 1] == ";" else "")


def between(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


def need(text, pattern, what):
    if not re.search(pattern, text, re.S):
        raise SystemExit("FAIL: " + what)


MENU_FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include "host/hos_wide_size.h"
#include "host/hold_toggle.h"
#include "gpu/vk/display_modes.h"
#include "gpu/vk/menu_restart.h"
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
constexpr int VK_BACK = 8, VK_TAB = 9, VK_RETURN = 0x0d, VK_ESCAPE = 0x1b, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25,
              VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_INSERT = 0x2d, VK_DELETE = 0x2e;
enum { HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_WEST, HOS_PAD_NORTH, HOS_PAD_BACK, HOS_PAD_GUIDE, HOS_PAD_START,
       HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER, HOS_PAD_RSHOULDER, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT,
       HOS_PAD_RIGHT };
enum { HOS_MOUSE_LEFT = 1, HOS_MOUSE_RIGHT = 2 };
@@TEXT_ENUMS@@
struct { int output_res, preset, ultrawide, handheld, audio_output; } opts;
struct Row { int* value; int hi; bool restart; };
Row rows[] = {{&opts.ultrawide, 0, true}, {&opts.output_res, 0, true}, {&opts.audio_output, 3, true}};
constexpr int kRows = 3;
int restart_slot[kRows] = {0, 1, 2};
namespace mr = menu_restart;
struct RestartState : mr::State {
    int noted = 0;
    void note(const int* now) { noted++; mr::State::note(now); }
} restart_state;
bool menu_refresh = false;
int ultrawide_read = 1;
uint32_t screen_w = 1920, screen_h = 1080;
bool screen_size(uint32_t* w, uint32_t* h) {
    return screen_w && screen_h ? (*w = screen_w, *h = screen_h, true) : false;
}
bool host_screen_size(uint32_t* w, uint32_t* h) { return screen_size(w, h); }
std::vector<display_modes::Size> screen_modes() { return {{1920, 1080}}; }
void logf(const char*, ...) {}
static int saves = 0;
void save() { saves++; }
static const int* released = nullptr;   // (main's env_release: the row the player set is theirs, over HOS_OUTPUT_RES)
void env_release(const int* value) { released = value; }
@@CUT@@
static const uint32_t K = HOS_TEXT_KEY;
static void type(TextField& f, const char* s) {
    for (; *s; s++) CHECK(text_edit(f, (unsigned char)*s) == kTextGoOn);
}
static bool is(const TextField& f, const char* text, int caret) {
    return f.len == (int)strlen(text) && !strcmp(f.text, text) && f.caret == caret;
}
static const char* apply(const char* typed) {
    text_set(output_field, typed);
    return output_field_apply();
}
static void startup(const char* output, int follow = 0) {
    custom_output = {0, 0};
    output_extras.clear();
    (void)output_shapes();
    // A later screen's mode joins the same dynamic list used by outputs_follow_screen.
    if (screen_w && (screen_w != 1920 || screen_h != 1080)) output_extras.push_back({screen_w, screen_h});
    opts.output_res = parse_output(output), opts.ultrawide = follow;
    outputs_sync();
    restart_state = {};
    for (const Row& row : rows) restart_state.add(row_restart_value(row));
    output_field_show();
    saves = 0, menu_refresh = false, released = nullptr;
}
// The real focused branch, with only the outside world stubbed. No keyboard event is used in the pad tests.
std::atomic<bool> open_now{true};
uint32_t raw = 0, pad_press = 0, typed[64] = {};
int typed_n = 0, pad_confirm = HOS_PAD_EAST, pad_back = HOS_PAD_SOUTH;
bool press[13] = {}, changed = false, mouse = false;
int64_t edit_keys_until = 0;
std::vector<uint32_t> scripted;
int cur_tab = 0, tab_selected[1] = {}, wheel_rows[1] = {}, mx = 0, my = 0;
struct { int pressed = 0, wheel = 0; } ptr;
namespace menu_hit { enum { kRow }; struct Hit { int part = kRow, index = 0; }; }
struct { menu_hit::Hit at(float, float) { return {}; } } hits;
bool pad(int button) { return (pad_press >> button & 1) != 0; }
int held_key = 0;
int GetAsyncKeyState(int vk) { return vk == held_key ? 0x8000 : 0; }
int rx_option(int id, int otherwise) { return id == 1 ? open_now.load() : otherwise; }
@@GAME_KEY@@
int64_t now = 100000000LL;
int64_t prof_now() { return now; }
void focused_tick() {
    @@FOCUSED@@
}
void tap(int button, int count = 1) {
    for (int i = 0; i < count; i++) {
        raw = pad_press = 1u << button;
        focused_tick();
        raw = pad_press = 0;
        focused_tick();
    }
}
// The host's raw pad callback and the menu's edge/repeat path, not a latched game-button word.
uint32_t pad_raw_now = 0;
@@PAD_RAW@@
struct { uint32_t (*pad_raw)(); } host_value{hos_pad_raw}, *host = &host_value;
void routed_tick() {
    const bool mine = true;
    bool pad_last = false, held[5] = {};
    const int listening = 0;
    std::fill(std::begin(press), std::end(press), false);
    @@PAD_POLL@@
    @@PAD_EDGE@@
    @@PAD_REPEAT@@
    @@FOCUSED@@
    (void)chord_now;
    (void)pad_last;
}
void test_toggle_text_isolation() {
    startup("1920x800");
    open_now = false;
    HoldToggle toggle{};
    const uint64_t game_r = 1u << 7;   // logical Switch R, distinct from its physical shoulder position
    pad_raw_now = 1u << HOS_PAD_RSHOULDER;
    CHECK(hold_toggle_apply(&toggle, game_r, game_r, 0) == game_r);
    routed_tick();
    pad_raw_now = 0;
    CHECK(hold_toggle_apply(&toggle, 0, game_r, 0) == game_r);
    routed_tick();
    open_now = true;
    edit_start(output_field, output_field_apply);
    routed_tick();   // even before the game observes Insert, its live latch is not a raw pad press
    CHECK(toggle.latched == game_r && is(output_field, "1920x800", 8) && !saves);
    CHECK(hold_toggle_apply(&toggle, 0, game_r, open_now.load()) == 0);
    CHECK(!toggle.latched);
    pad_raw_now = 1u << HOS_PAD_RSHOULDER;
    CHECK(hold_toggle_apply(&toggle, game_r, game_r, open_now.load()) == 0);
    routed_tick();   // a fresh physical shoulder still inserts in the editor while gameplay is blocked
    CHECK(is(output_field, "1920x8000", 8) && !saves);
    routed_tick();   // held shoulder is not another insertion
    CHECK(is(output_field, "1920x8000", 8));
    pad_raw_now = 0;
    routed_tick();
    pad_raw_now = 1u << HOS_PAD_UP;
    routed_tick();
    CHECK(is(output_field, "1920x8001", 8));
    now += 350000000LL;
    routed_tick();   // actual menu repeat still cycles characters, never a gameplay latch
    CHECK(is(output_field, "1920x8002", 8));
    pad_raw_now = 0;
    routed_tick();
    held_key = VK_LEFT;
    CHECK(!key(VK_LEFT));   // the real game's key reader rejects the editor's key
    typed[0] = HOS_TEXT_KEY | VK_LEFT, typed_n = 1;
    routed_tick();
    CHECK(is(output_field, "1920x8002", 7));
    held_key = 0, typed_n = 0;
    edit_end(false);
    // The confirm and shoulder already held when focus begins must not commit or type into the field.
    for (const int button : {pad_confirm, int(HOS_PAD_RSHOULDER), int(HOS_PAD_LSHOULDER)}) {
        pad_raw_now = 1u << button;
        routed_tick();
        edit_start(output_field, output_field_apply);
        routed_tick();
        CHECK(editing && is(output_field, "1920x800", 8) && !saves);
        pad_raw_now = 0;
        routed_tick();
        edit_end(false);
    }
    open_now = false, held_key = VK_LEFT;
    CHECK(key(VK_LEFT));
    held_key = 0, open_now = true;
    std::puts("PASS: test_toggle_text_isolation: raw pad edges/repeats and editing keys, not gameplay latches");
}
void test_pad_only_entry() {
    startup("0");
    edit_start(output_field, output_field_apply);
    CHECK(is(output_field, "", 0));
    for (const char c : std::string("1920x800")) {
        tap(HOS_PAD_RSHOULDER);   // insert 0, then cycle it; no TYPE or text input
        tap(HOS_PAD_UP, c == 'x' ? 10 : c - '0');
        tap(HOS_PAD_RIGHT);
    }
    CHECK(is(output_field, "1920x800", 8) && !saves);
    tap(pad_confirm);
    CHECK(!editing && saves == 1 && output_at(opts.output_res).w == 1920 && output_at(opts.output_res).h == 800);
    std::puts("PASS: test_pad_only_entry: empty field to committed 1920x800");
}
void test_pad_edit_existing() {
    startup("1920x800");
    edit_start(output_field, output_field_apply);
    tap(HOS_PAD_LEFT, 3), tap(HOS_PAD_DOWN, 2);
    CHECK(is(output_field, "1920x600", 5));
    tap(HOS_PAD_RIGHT), tap(HOS_PAD_LSHOULDER);
    CHECK(is(output_field, "1920x60", 6));
    tap(HOS_PAD_RSHOULDER);
    CHECK(is(output_field, "1920x600", 6));
    tap(HOS_PAD_RIGHT, 2), tap(HOS_PAD_UP);
    CHECK(is(output_field, "1920x6000", 8));
    tap(HOS_PAD_RIGHT), tap(HOS_PAD_LSHOULDER);   // end caret removes the previous character
    CHECK(is(output_field, "1920x600", 8));
    tap(pad_confirm);
    CHECK(!editing && saves == 1 && output_at(opts.output_res).h == 600);
    edit_start(output_field, output_field_apply);
    tap(HOS_PAD_LSHOULDER), tap(pad_back);
    CHECK(!editing && saves == 1 && !strcmp(output_field.text, "1920x600"));
    std::puts("PASS: test_pad_edit_existing: cycle, remove, insert, append, commit and cancel");
}
void test_pad_character_limits() {
    startup("0");
    edit_start(output_field, output_field_apply);
    tap(HOS_PAD_DOWN);   // no leading separator
    CHECK(is(output_field, "9", 0));
    tap(HOS_PAD_UP);
    CHECK(is(output_field, "0", 0));
    // D-pad repeats follow menu_tick's existing repeat clock, not keyboard text events.
    raw = 1u << HOS_PAD_UP, press[1] = true;
    focused_tick();
    raw = 0, press[1] = false;
    CHECK(is(output_field, "1", 0));
    text_set(output_field, "12345x67890");
    tap(HOS_PAD_RSHOULDER), tap(HOS_PAD_UP);   // full buffer: no insertion or append
    CHECK(is(output_field, "12345x67890", 11));
    tap(HOS_PAD_LEFT), tap(HOS_PAD_DOWN);
    CHECK(is(output_field, "12345x67899", 10));   // no second separator
    tap(HOS_PAD_LEFT, 5), tap(HOS_PAD_UP);
    CHECK(is(output_field, "12345067899", 5));   // separator cycles to 0
    tap(HOS_PAD_DOWN);
    CHECK(is(output_field, "12345x67899", 5));
    tap(pad_back);
    std::puts("PASS: test_pad_character_limits: separator position, repeats and full buffer");
}
void test_edit_wheel() {
    startup("0");
    edit_start(output_field, output_field_apply);
    mouse = true;
    for (const int wheel : {-3, 1, 4}) {
        ptr.wheel = wheel, changed = false;
        focused_tick();
        CHECK(wheel_rows[0] == 0 && editing == &output_field && !changed && !saves);
    }
    ptr.wheel = 0, mouse = false;
    edit_end(false);
    std::puts("PASS: test_edit_wheel: edited field does not scroll out of view");
}
void test_full_width() {
    text_set(output_field, "");
    for (uint32_t c = 0xff10; c <= 0xff19; c++) text_edit(output_field, c);
    CHECK(is(output_field, "0123456789", 10));
    text_edit(output_field, 0xff58);
    CHECK(is(output_field, "0123456789x", 11));
    TextField digits;
    digits.allow = [](char c) { return c >= '0' && c <= '9'; };
    text_set(digits, "");
    text_edit(digits, 0xff58), text_edit(digits, 0xff11);
    CHECK(is(digits, "1", 1));   // the converted character must still pass allow()
    std::puts("PASS: test_full_width: all ten digits and x folded before the allow check");
}
void test_monitor_shape() {
    for (const auto screen : {display_modes::Size{2560, 1080}, display_modes::Size{5120, 1440}}) {
        screen_w = screen.w, screen_h = screen.h;
        startup("2048x1152", 1);
        const std::string shape = aspect_shape(opts.ultrawide);
        CHECK(shape == (screen.w == 2560 ? "21:9" : "32:9"));
        CHECK(apply("2048x1152") == nullptr && aspect_shape(opts.ultrawide) == shape && aspect_follows_host(opts.ultrawide));
        CHECK(!restart_changed(rows[0]) && !restart_changed(rows[1]));
        startup("2048x1152", 0);
        CHECK(apply("2048x1152") == nullptr && aspect_shape(opts.ultrawide) == "16:9");
    }
    screen_w = 1920, screen_h = 1080;
    std::puts("PASS: test_monitor_shape: 21:9 and 32:9 keep the monitor's shape for typed 2048x1152");
}
void test_audio_restart_with_custom_size() {
    opts.audio_output = 1;
    startup("1920x800");
    opts.audio_output = 3;   // Stereo and Mono share the startup channel layout.
    int now[mr::kMax] = {};
    restart_values(now);
    CHECK(!restart_state.pending(now) && !restart_changed(rows[2]));
    CHECK(apply("1920x808") == nullptr);
    restart_values(now);
    CHECK((restart_state.pending(now) & 2u) && !(restart_state.pending(now) & 4u));
    CHECK(restart_changed(rows[1]) && !restart_changed(rows[2]));
    restart_state.choose_later(now);
    opts.audio_output = 1;
    restart_values(now);
    CHECK(!restart_state.ask(now));   // A live mix switch does not renew the declined size prompt.
    opts.audio_output = 2;
    restart_values(now);
    CHECK(restart_state.ask(now) && (restart_state.pending(now) & 4u));
    CHECK(apply("1920x800") == nullptr);
    restart_values(now);
    CHECK(restart_state.pending(now) == 4u);   // Restoring the size does not clear a channel-layout change.
    opts.audio_output = 3;
    restart_values(now);
    CHECK(!restart_state.pending(now));
    startup("1920x800");   // The reverse live transition, from a Mono startup.
    opts.audio_output = 1;
    restart_values(now);
    CHECK(!restart_state.pending(now));
    opts.audio_output = 0;
    restart_values(now);
    CHECK(restart_state.pending(now) == 4u);   // Auto is not assumed to share the stereo layout.
    std::puts("PASS: test_audio_restart_with_custom_size: live Mono/Stereo, size edits and layout changes keep separate slots");
}
int main() {
    test_toggle_text_isolation();
    test_pad_only_entry();
    test_pad_edit_existing();
    test_pad_character_limits();
    test_edit_wheel();
    test_full_width();
    test_monitor_shape();
    test_audio_restart_with_custom_size();
    TextField f;
    text_set(f, "");
    CHECK(is(f, "", 0));
    type(f, "abc");
    CHECK(is(f, "abc", 3));
    CHECK(text_edit(f, K | VK_LEFT) == kTextGoOn && text_edit(f, K | VK_LEFT) == kTextGoOn && is(f, "abc", 1));
    type(f, "X");   // (typed at the caret)
    CHECK(is(f, "aXbc", 2));
    text_edit(f, K | VK_HOME), text_edit(f, K | VK_DELETE);
    CHECK(is(f, "Xbc", 0));
    text_edit(f, K | VK_BACK);   // (nothing before the caret)
    CHECK(is(f, "Xbc", 0));
    text_edit(f, K | VK_END), text_edit(f, K | VK_BACK);
    CHECK(is(f, "Xb", 2));
    text_edit(f, K | VK_DELETE), text_edit(f, K | VK_RIGHT);   // (nothing after it)
    CHECK(is(f, "Xb", 2));
    text_edit(f, K | VK_LEFT), text_edit(f, K | VK_LEFT), text_edit(f, K | VK_LEFT);   // (stops at the start)
    CHECK(is(f, "Xb", 0));
    text_edit(f, K | VK_TAB);   // (not an editing key: nothing)
    CHECK(is(f, "Xb", 0));
    // (printable ASCII alone: the menu's glyphs are baked for it. Another character typed is left out, whatever the keyboard)
    for (uint32_t c : {0xE9u, 0xFCu, 0x4E2Du, 0x1F600u, 0x09u, 0x0Au, 0x7Fu, 0x00u, 0x80u, 0xFFu}) CHECK(text_edit(f, c) == kTextGoOn);
    CHECK(is(f, "Xb", 0));
    type(f, " ~");   // (the ends of the range)
    CHECK(is(f, " ~Xb", 2));
    // (Enter and Esc end the edit and leave the text as it is: the menu sets it or puts it back)
    CHECK(text_edit(f, K | VK_RETURN) == kTextDone && text_edit(f, K | VK_ESCAPE) == kTextCancel && is(f, " ~Xb", 2));
    // (the length limit)
    TextField small;
    small.max = 5;
    text_set(small, "");
    type(small, "abcdefgh");
    CHECK(is(small, "abcde", 5));
    text_set(small, "0123456789");   // (set whole: cut to what it holds)
    CHECK(is(small, "01234", 5));
    text_set(small, nullptr);
    CHECK(is(small, "", 0));
    small.max = 1000;   // (never past its buffer)
    for (int i = 0; i < 100; i++) text_edit(small, 'z');
    CHECK(small.len == (int)sizeof small.text - 1 && small.text[small.len] == 0);
    small.len = 500, small.caret = -3;   // (out of range: taken back into it)
    text_edit(small, K | VK_END);
    CHECK(small.len == (int)sizeof small.text - 1 && small.caret == small.len);
    // (a field that takes less: Custom output size, digits and the x)
    text_set(output_field, "");
    type(output_field, "3440 x1440a");
    CHECK(!strcmp(output_field.text, "3440x1440") && output_field.max == 11);

    // ---- Custom output size's Enter: the Output resolution row on the size typed ----
    startup("0");
    CHECK(custom_index() != 6);   // (the display's common sizes, not the former fixed six choices)
    opts.preset = 3;
    CHECK(apply("3440x1440") == nullptr);
    CHECK(opts.output_res == custom_index() && custom_output.w == 3440 && custom_output.h == 1440 && saves == 1);
    CHECK(aspect_shape(opts.ultrawide) == "21:9" && rows[0].hi == aspect_last() && rows[1].hi == last_choice());
    CHECK(menu_refresh && restart_changed(rows[0]) && restart_changed(rows[1]));
    // (the size typed is the player's from now on, also over HOS_OUTPUT_RES; the Quality preset stays: the Output
    // resolution is a row no preset sets, preset_exempt, as tests/test_preset_exempt.py has it)
    CHECK(!strcmp(output_field.text, "3440x1440") && opts.preset == 3 && restart_state.noted == 1 && released == &opts.output_res);
    opts.preset = 4;
    CHECK(apply("3440x1440") == nullptr && opts.preset == 4);
    CHECK(apply("1366X768") == nullptr && custom_output.w == 1360 && custom_output.h == 768);   // (sides to a multiple of 8)
    CHECK(!strcmp(output_field.text, "1360x768") && opts.preset == 4);
    CHECK(apply("1920x1080") == nullptr);   // (a size the row offers: chosen there, none of the player's own)
    const int fixed = output_choice_of(1920, 1080);
    CHECK(opts.output_res == fixed && custom_output.w == 0 && !strcmp(output_field.text, ""));
    const int saved = saves;
    for (const char* bad : {"320x200", "9999x9999", "7688x1440", "640x4320"})   // (not a size the game draws)
        CHECK(apply(bad) && strstr(apply(bad), "Not a size") && opts.output_res == fixed);
    for (const char* bad : {"3440x", "x1440", "3440", "123456x1", "34x40x10", "3440xx1440"})   // (not width x height)
        CHECK(apply(bad) && strstr(apply(bad), "Type the width") && opts.output_res == fixed);
    CHECK(saves == saved);   // (refused: nothing saved)
    CHECK(apply("5120x2160") == nullptr && opts.output_res == custom_index());
    CHECK(apply("") == nullptr && opts.output_res == 0 && custom_output.w == 0);   // (emptied: none, the game's own size)
    opts.output_res = fixed;
    CHECK(apply("") == nullptr && opts.output_res == fixed);   // (emptied with an offered size: that one stays)
    // (the file's size of the player's own shows in the field at the start)
    CHECK(parse_output("2560x1080") == custom_index() && custom_index() == last_choice());
    output_field_show();
    CHECK(!strcmp(output_field.text, "2560x1080") && output_field.caret == 9);
    // (sizes discovered after start are ordinary choices too, including the rounded dimensions)
    startup("0");
    output_extras.push_back({1600, 896});
    const int extra = last_choice();
    CHECK(apply("1600x900") == nullptr && opts.output_res == extra && !custom_output.w && !output_field.len);
    CHECK(rows[1].hi == extra && aspect_shape(opts.ultrawide) == "16:9");
    // (adding/removing a narrower custom shape remaps the startup index without resetting what runs)
    startup("0");
    const int original_output = restart_state.start[1];
    CHECK(apply("1440x1080") == nullptr && aspect_shape(opts.ultrawide) == "4:3");
    CHECK(aspect_shape(aspect_started) == "16:9" && restart_state.start[0] == aspect_started);
    CHECK(restart_state.start[1] == original_output && restart_changed(rows[0]) && restart_changed(rows[1]));
    CHECK(apply("2048x1152") == nullptr && !restart_changed(rows[0]) && !restart_changed(rows[1]));
    // (replacing the run's original custom size preserves it once, so returning to its shape still restores it)
    startup("2400x1600", 1);
    CHECK(apply("3440x1440") == nullptr && output_extras.size() == 1 && output_started > custom_index());
    CHECK(apply("2560x1080") == nullptr && output_extras.size() == 1);
    const int back = output_for_aspect(aspect_started);
    CHECK(output_at(back).w == 2400 && output_at(back).h == 1600);
    opts.output_res = back, opts.ultrawide = aspect_started;
    CHECK(!restart_changed(rows[0]) && !restart_changed(rows[1]));
    CHECK(apply("2400x1600") == nullptr && opts.output_res == output_started && !custom_output.w);
    CHECK(!restart_changed(rows[0]) && !restart_changed(rows[1]));
    // (an unknown Monitor's shape is a distinct value, even when a custom shape changes its index)
    screen_w = screen_h = 0;
    startup("0", 1);
    CHECK(apply("2400x1600") == nullptr && aspect_started == aspect_monitor_value());
    opts.output_res = output_started, opts.ultrawide = aspect_started;
    int now[mr::kMax] = {};
    restart_values(now);
    restart_state.choose_later(now);
    CHECK(apply("") == nullptr && opts.ultrawide == aspect_monitor_value() && aspect_started == aspect_monitor_value());
    CHECK(!restart_changed(rows[0]) && !restart_changed(rows[1]) && restart_state.later);
    std::puts("PASS: typing, the caret's keys, the limits, ASCII alone, Enter and Esc; Custom output size sets the Output resolution");
}
'''

HOST_FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef long LONG;
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static int locked;
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; CHECK(!locked); locked = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; CHECK(locked); locked = 0; }
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp) { LONG o = *p; if (o == cmp) *p = v; return o; }
@@TEXT_ENUMS@@
static int pastes;
void hos_display_paste(void) { pastes++; }
@@CUT@@
static int take(int focus, uint32_t *out) { return host_take_text(focus, out, 512); }
int main(void) {
    uint32_t out[512];
    hos_text_put('a');   /* (no field has the keyboard: dropped) */
    CHECK(!hos_text_focus() && take(HOS_TEXT_OFF, out) == 0);
    CHECK(take(HOS_TEXT_ON, out) == 0 && hos_text_focus());
    hos_text_put('h'), hos_text_put('i'), hos_text_put(HOS_TEXT_KEY | 8);
    CHECK(take(HOS_TEXT_ON, out) == 3 && out[0] == 'h' && out[1] == 'i' && out[2] == (HOS_TEXT_KEY | 8));
    CHECK(take(HOS_TEXT_ON, out) == 0);
    /* (UTF-8, as SDL's text input gives it: each character's code point, whatever its length) */
    hos_text_put_utf8("a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80z", 0);
    CHECK(take(HOS_TEXT_ON, out) == 5 && out[0] == 'a' && out[1] == 0xE9 && out[2] == 0x4E2D && out[3] == 0x1F600 && out[4] == 'z');
    /* (a broken sequence left out, the byte that broke it read as a character of its own) */
    hos_text_put_utf8("\xC3(\xE4\xB8x\x80\xFFq", 0);
    CHECK(take(HOS_TEXT_ON, out) == 3 && out[0] == '(' && out[1] == 'x' && out[2] == 'q');
    /* (a paste: its first line alone; a typed line break is a character, which the menu leaves out) */
    hos_text_put_utf8("abc\r\ndef", 1);
    CHECK(take(HOS_TEXT_ON, out) == 3 && out[2] == 'c');
    hos_text_put_utf8("one\ntwo", 0);
    CHECK(take(HOS_TEXT_ON, out) == 7 && out[3] == '\n');
    hos_text_put_utf8(NULL, 1);
    CHECK(take(HOS_TEXT_ON, out) == 0);
    /* (taken in order a few at a time) */
    for (uint32_t c = 0; c < 10; c++) hos_text_put('0' + c);
    CHECK(host_take_text(HOS_TEXT_ON, out, 4) == 4 && out[3] == '3' && host_take_text(HOS_TEXT_ON, out, 64) == 6 && out[0] == '4');
    /* (the queue holds TEXT_QUEUE, the rest dropped) */
    for (int i = 0; i < TEXT_QUEUE + 50; i++) hos_text_put('x');
    CHECK(take(HOS_TEXT_ON, out) == TEXT_QUEUE);
    /* (Paste asks the window for the clipboard; the field losing the keyboard drops what was typed) */
    CHECK(take(HOS_TEXT_PASTE, out) == 0 && pastes == 1 && hos_text_focus());
    hos_text_put('q');
    CHECK(take(HOS_TEXT_OFF, out) == 0 && !hos_text_focus());
    hos_text_put('r');
    CHECK(take(HOS_TEXT_ON, out) == 0 && !locked);
    puts("PASS: the text queue: only while a field has the keyboard, in order, UTF-8 to characters, a paste's first line");
    return 0;
}
'''

WINDOWS_FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
enum { VK_CONTROL = 0x11, VK_SHIFT = 0x10, VK_INSERT = 0x2d, VK_LEFT = 0x25, VK_RIGHT = 0x27,
       VK_HOME = 0x24, VK_END = 0x23, VK_BACK = 8, VK_DELETE = 0x2e, VK_RETURN = 13, VK_ESCAPE = 0x1b };
@@TEXT_ENUMS@@
static int focused = 1, ctrl = 0, shift = 0, pastes = 0, keys = 0;
static uint32_t last_key;
static int hos_text_focus(void) { return focused; }
static int GetKeyState(int k) { return (k == VK_CONTROL ? ctrl : shift) ? -1 : 0; }
static void paste_clipboard(int w) { (void)w; pastes++; }
static void hos_text_put(uint32_t c) { last_key = c; keys++; }
static void keydown(uint32_t wp, intptr_t lp) {
    const int w = 0;
    @@WIN_KEYS@@
}
int main(void) {
    ctrl = 1;
    keydown('V', 0), keydown('V', (intptr_t)1 << 30);
    CHECK(pastes == 1 && keys == 0);
    ctrl = 0, shift = 1;
    keydown(VK_INSERT, 0), keydown(VK_INSERT, (intptr_t)1 << 30);
    CHECK(pastes == 2 && keys == 0);
    shift = 0;
    keydown('V', 0), keydown(VK_INSERT, 0);
    CHECK(pastes == 2 && keys == 0);
    keydown(VK_BACK, 0), keydown(VK_BACK, (intptr_t)1 << 30);
    CHECK(keys == 2 && last_key == (HOS_TEXT_KEY | VK_BACK));
    focused = 0, ctrl = 1;
    keydown('V', 0), keydown(VK_BACK, 0);
    CHECK(pastes == 2 && keys == 2);
    puts("PASS: test_windows_paste_repeats: Ctrl+V and Shift+Insert once per press; editing keys still repeat");
    return 0;
}
'''


def check_host_table(fields, table):
    if "take_text" not in fields or len(table) != len(fields) or table[fields.index("take_text")] != "host_take_text":
        raise SystemExit("FAIL: vk_api.h / rx_backend.c: vk_host does not give host_take_text in take_text's place (%d members, "
                         "%d values)" % (len(fields), len(table)))
    if fields.index("take_text") <= fields.index("screen_size") or table[fields.index("screen_size")] != "host_screen_size":
        raise SystemExit("FAIL: take_text must follow screen_size, with both callbacks in their interface slots")


def compile_run(cxx, lang, std, source, name, tmp):
    """source compiled as lang (c++ or c, the C++ driver given -x) and run"""
    src, exe = Path(tmp) / (name + ".src"), Path(tmp) / name
    src.write_text(source, encoding="utf-8")
    subprocess.run([cxx, "-x", lang, "-std=" + std, "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT), str(src), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    backend = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    enums = "\n".join(re.findall(r"^enum \{ HOS_TEXT_[^\n]*\};$", api, re.M))
    if enums.count("enum") != 2:
        raise SystemExit("FAIL: vk_api.h: no HOS_TEXT_KEY and HOS_TEXT_OFF/ON/PASTE")
    cut = "\n".join([block(menu, "struct TextField {"), between(menu, "enum { kTextGoOn", "\n") + "\n",
                     block(menu, "int text_edit("), block(menu, "void text_set("),
                     between(menu, "struct Out {", "Out shown_output()"),
                     block(menu, "void outputs_sync("), block(menu, "int row_restart_value("),
                     block(menu, "int restart_slot_of("), block(menu, "bool restart_changed("),
                     block(menu, "void restart_values("),
                     between(menu, "bool output_size_char(", "struct FieldRow {"),
                     between(menu, "TextField* editing =", "// ---- (hookshot) the Controls page:")])
    host_cut = between(backend, "enum { TEXT_QUEUE", "static const MkwVkHost vk_host")
    focused = block(menu, "if (open_now.load() && editing) {")
    win = (ROOT / "host/display.c").read_text(encoding="utf-8")
    win_keys = block(win[win.index("case WM_KEYDOWN: {"):], "if (hos_text_focus()) {")
    pad_poll = between(menu, "    static uint32_t pad_held = 0;", "#ifndef VK_RSHIFT")
    pad_edge = between(menu, "    auto pad = ", "\n")
    pad_repeat = block(menu, "    {   // holding an arrow or the D-pad repeats it:")
    with tempfile.TemporaryDirectory(prefix="menu-text-") as tmp:
        compile_run(a.cxx, "c++", "c++20", MENU_FIXTURE.replace("@@TEXT_ENUMS@@", enums).replace("@@CUT@@", cut)
                    .replace("@@PAD_RAW@@", block(hid, "uint32_t hos_pad_raw("))
                    .replace("@@GAME_KEY@@", block(hid, "static int key("))
                    .replace("@@PAD_POLL@@", pad_poll).replace("@@PAD_EDGE@@", pad_edge)
                    .replace("@@PAD_REPEAT@@", pad_repeat).replace("@@FOCUSED@@", focused), "menu", tmp)
        compile_run(a.cxx, "c", "c11", HOST_FIXTURE.replace("@@TEXT_ENUMS@@", enums).replace("@@CUT@@", host_cut), "host", tmp)
        compile_run(a.cxx, "c", "c11", WINDOWS_FIXTURE.replace("@@TEXT_ENUMS@@", enums).replace("@@WIN_KEYS@@", win_keys),
                    "windows", tmp)

    # ---- the wiring ----
    version = int(re.search(r"#define MKWVK_API_VERSION (\d+)", api).group(1))
    if version < 23:
        raise SystemExit("FAIL: vk_api.h: MkwVkHost grew without an MKWVK_API_VERSION bump")
    # (host/rx_backend.c fills MkwVkHost by position: take_text's place in the struct is host_take_text's in vk_host)
    def code(text):   # (comments out: they hold ; and ,)
        return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))
    decls = [d.strip() for d in code(between(api, "typedef struct MkwVkHost {", "} MkwVkHost;")).split("{", 1)[1].split(";")]
    fields = [re.search(r"\(\*(\w+)\)", d).group(1) if "(*" in d else d.split()[-1] for d in decls if d]
    table = [v.strip() for v in code(between(backend, "static const MkwVkHost vk_host = {", "};")).split("{", 1)[1].split(",")]
    table = [v for v in table if v]
    check_host_table(fields, table)
    check_host_table(fields + ["later_callback"], table + ["host_later_callback"])
    before = fields.index("take_text")
    check_host_table(fields[:before] + ["main_callback"] + fields[before:],
                     table[:before] + ["host_main_callback"] + table[before:])
    wrong = table.copy()
    wrong[before] = "host_screen_size"
    try:
        check_host_table(fields, wrong)
    except SystemExit:
        pass
    else:
        raise SystemExit("FAIL: test_api_append: misplaced host_take_text was accepted")
    comment_version = re.search(r"/\* \(hookshot, version (\d+)\) typed text", api)
    if not comment_version or int(comment_version.group(1)) != 23:   # (23 in Hookshot: 22 is the host's wide_cover)
        raise SystemExit("FAIL: text API comment must name its introduction at version 23")
    print("PASS: test_api_append: members may follow screen_size or take_text; positional wiring still required")
    need(win, r"case WM_CHAR: \{.*?hos_text_put\(c\);.*?return 0;", "display.c: WM_CHAR does not reach the menu's text field")
    need(win, r"case WM_KEYDOWN: \{.*?if \(hos_text_focus\(\)\) \{.*?paste_clipboard\(w\);.*?hos_text_put\(HOS_TEXT_KEY \| \(uint32_t\)wp\);",
         "display.c: the editing keys and Ctrl+V do not reach the menu's text field")
    need(win, r"case WM_APP \+ 12:[^\n]*\n\s*paste_clipboard\(w\);", "display.c: the menu's Paste is not WM_APP + 12")
    if len(re.findall(r"case WM_APP \+ 12:", win)) != 1:
        raise SystemExit("FAIL: display.c: WM_APP + 12 is handled twice")
    need(win, r"GetClipboardData\(CF_UNICODETEXT\)", "display.c: the clipboard is not read as Unicode")
    sdl = (ROOT / "host/posix/display_sdl.c").read_text(encoding="utf-8")
    need(sdl, r"case HOS_SDL_EV_TEXT:\s*hos_text_put_utf8\(e\.text, 0\);", "display_sdl.c: SDL's text input does not reach the field")
    need(sdl, r"if \(hos_text_focus\(\)\) text_key\(&e\);", "display_sdl.c: the editing keys do not reach the field")
    need(sdl, r"if \(hos_text_focus\(\) != text_on\) hos_sdl_text_input\(window, text_on = hos_text_focus\(\)\);",
         "display_sdl.c: SDL's text input is not on only while a field has the keyboard")
    sdl_lib = (ROOT / "host/hos_sdl.c").read_text(encoding="utf-8")
    if len(re.findall(r"case 0x303:", sdl_lib)) != 2 or "HOS_SDL_EV_TEXT" not in sdl_lib:
        raise SystemExit("FAIL: hos_sdl.c: SDL3's and SDL2's text input events are not both read")
    tick = block(menu, "void menu_tick() {")
    need(tick, r"host->take_text\(!editing \? HOS_TEXT_OFF : pasting \? HOS_TEXT_PASTE : HOS_TEXT_ON, typed, 64\)",
         "menu_tick: the host is not told whether a field has the keyboard")
    # (while a field has the keyboard, its branch comes before every page's: no key does anything else)
    first = tick.index("if (open_now.load() && editing) {")
    if first > tick.index("} else if (open_now.load() && page == 5) {"):
        raise SystemExit("FAIL: menu_tick: a page's keys are read before the field's")
    need(tick, r"edit_end\(false\);\s*// \(hookshot: a text field being edited", "menu_tick: closing the menu does not end the edit")
    need(tick, r'!strcmp\(key, "TYPE"\)', "menu_tick: RX_VK_MENU_SCRIPT has no TYPE")
    need(tick, r"pad\(pad_paste\)", "menu_tick: a controller cannot paste")
    ui = (ROOT / "gpu/vk/vk_ui.h").read_text(encoding="utf-8")
    for glyph, label in (("kUpDown", "Character"), ("kLeftRight", "Caret"), ("kAddChar", "Add"), ("kRemoveChar", "Remove")):
        need(menu, r'\{ui::' + glyph + r', "' + label + r'"\}', "text field: missing " + label + " prompt")
    need(ui, r"pr.g == kAddChar && \*l == 'L'.*?pr.g == kRemoveChar && \*l == 'R'",
         "text field: Add and Remove do not show right and left shoulders")
    need(menu, r'\{TAB_GRAPHICS, "Custom output size", nullptr,', "vk_menu.cpp: no Custom output size row")
    need(menu, r'const FieldRow field_rows\[\] = \{\{"Custom output size", &output_field, output_field_apply\}\};',
         "vk_menu.cpp: Custom output size is not a text field")
    need(hid, r"static int key\(int vk\) \{ return !rx_option\(1, 0\) &&",
         "nn_hid.c: the game's keys are not held back while the menu (and so a field) has the keyboard")
    print("PASS: the text field's wiring: the interface's take_text, WM_CHAR, SDL's text input, the menu's focus and TYPE")


if __name__ == "__main__":
    main()
