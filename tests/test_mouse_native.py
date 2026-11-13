"""Native mouse look turns the player camera's own angles and holds them there (host/mouse_native.c).

The file is compiled on its own against a fake guest memory that holds a player camera: its pitch limits at +0x178, its
pitch at +0x18c, its yaw at +0x1a4 and its state at +0x204. hos_mouse_camera is what the camera's update (f_80644c80,
nx2/oot_patches.py) calls each frame. Until it has run the mouse's motion is the right stick's (hos_mouse_native gives
-1); then it turns yaw and pitch by 0.1 degrees a count at sensitivity 1, pitch kept within the limits and yaw wrapped at
180, up and down swapped with the menu's Invert mouse vertical (option 8), and the controller reads leave the motion to
the camera. The game eases the angles back toward Link's by itself inside a frame (its follow camera): the mouse's angles
are put back every frame, so the view does not drift, and a move adds to the mouse's angles, not to the game's. The game's
own right stick is let through (hos_mouse_native_stick: the camera's keys, a pad): its angles become the mouse's new
start. Without the menu's Mouse camera (option 6), or on an object that is not a camera, it does nothing. The game gives
the camera free control only in its states 3 (it follows Link, the right stick takes it over) and 4 (the player's), and
while no button it takes the right stick for is held (ZR, the bow's aim; L, the item wheel; R: hos_mouse_native_free);
150 ms without the camera (the pause menu, another camera) give the motion back to the stick as well.
python -I -B tests/test_mouse_native.py   (Linux: host/posix's windows.h)
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]

HOS_H = r"""
#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint8_t *fake_heap;
#define HOS_HEAP_BASE 0x1000000000ULL
#define GP(a) ((void *)(fake_heap + ((uint64_t)(a) - HOS_HEAP_BASE)))
extern uint64_t hos_heap_size;
void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);
"""

FIXTURE = r"""
#include "hos.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
uint8_t *fake_heap;
uint64_t hos_heap_size = 64 * 4096;
double hos_mouse_sens = 1.0;
static int64_t clock_ns = 1000000000LL;
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc(10, stderr); }
int64_t hos_now_ns(void) { return clock_ns; }
static int32_t move_x, move_y;
static int captured = 1, menu_invert, menu_native = 1;
int rx_option(int id, int otherwise) { return id == 8 ? menu_invert : id == 6 ? menu_native : otherwise; }
int hos_mouse_take(int32_t *dx, int32_t *dy) { *dx = move_x, *dy = move_y; move_x = move_y = 0; return captured; }
void hos_mouse_give_back(int32_t dx, int32_t dy) { move_x += dx, move_y += dy; }
int hos_mouse_native(void);
void hos_mouse_native_free(int free);
void hos_mouse_native_stick(int held);
void hos_mouse_camera(uint64_t camera);
static const uint64_t CAM = HOS_HEAP_BASE + 10 * 4096 + 0x40, CAM2 = HOS_HEAP_BASE + 30 * 4096 + 0x40,
                      OTHER = HOS_HEAP_BASE + 20 * 4096;
static float getf(uint64_t a) { float f; memcpy(&f, GP(a), 4); return f; }
static void putf(uint64_t a, float f) { memcpy(GP(a), &f, 4); }
static void puti(uint64_t a, int32_t v) { memcpy(GP(a), &v, 4); }
static int fails;
#define PITCH (CAM + 0x18c)
#define YAW (CAM + 0x1a4)
static void state(uint32_t v) { memcpy(GP(CAM + 0x204), &v, 4); }
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (pitch %g yaw %g)\n", __LINE__, #c, getf(PITCH), getf(YAW)); fails++; } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }
static void frame(int32_t dx, int32_t dy) { move_x += dx, move_y += dy; clock_ns += 16666667; hos_mouse_camera(CAM); }
/* (the camera's angles become the mouse's start: a stick turn of the game's own, or a camera put somewhere by hand) */
static void seed(void) { hos_mouse_native_stick(1); frame(0, 0); hos_mouse_native_stick(0); }
static void seed_at(float pitch, float yaw) { putf(PITCH, pitch), putf(YAW, yaw); seed(); }
int main(void) {
    fake_heap = (uint8_t *)calloc(64, 4096);
    putf(CAM + 0x178, -85), putf(CAM + 0x17c, 89);
    puti(CAM + 0x204, 3);
    putf(PITCH, 10), putf(YAW, 30);
    state(4);   /* turned by the player */
    move_x = 5, move_y = 3;
    CHECK(hos_mouse_native() == -1);   /* no camera has run yet: the stick takes it */
    move_x = move_y = 0;
    frame(10, 4);
    CHECK(near(getf(YAW), 29.0f) && near(getf(PITCH), 10.4f));   /* right: yaw falls; down: pitch rises */
    move_x = 3;
    CHECK(hos_mouse_native() == 1 && move_x == 3);   /* the camera's now: the motion stays for it */
    move_x = 0;
    /* the game's follow camera inside a frame: it eases the pitch and turns the yaw by itself; the mouse's angles are put back */
    putf(YAW, 12), putf(PITCH, 4);
    frame(0, 0);
    CHECK(near(getf(YAW), 29.0f) && near(getf(PITCH), 10.4f));   /* no drift back toward Link */
    putf(YAW, -40), putf(PITCH, 40);
    frame(4, 0);
    CHECK(near(getf(YAW), 28.6f) && near(getf(PITCH), 10.4f));   /* a move adds to the mouse's angles, not the game's */
    frame(0, 100000);
    CHECK(near(getf(PITCH), 89.0f));   /* kept within the limits */
    seed_at(0, 0);
    frame(0, -100000);
    CHECK(near(getf(PITCH), -85.0f));
    seed_at(0, -179.5f);
    frame(10, 0);
    CHECK(near(getf(YAW), 179.5f));   /* wrapped */
    seed_at(0, 0);
    menu_invert = 1;   /* the menu's Invert mouse vertical: down tilts up */
    frame(0, 10);
    CHECK(near(getf(PITCH), -1.0f));
    menu_invert = 0;
    /* the game's own right stick (the camera's keys, a pad): its angles are the mouse's new start, and it still turns */
    putf(YAW, 70), putf(PITCH, 30);
    seed();
    CHECK(near(getf(YAW), 70.0f) && near(getf(PITCH), 30.0f));
    frame(-10, 0);
    CHECK(near(getf(YAW), 71.0f));
    /* another camera: its own angles are the start, not the last camera's */
    putf(CAM2 + 0x178, -85), putf(CAM2 + 0x17c, 89), puti(CAM2 + 0x204, 3);
    putf(CAM2 + 0x18c, -20), putf(CAM2 + 0x1a4, 100);
    move_x = 0;
    clock_ns += 16666667;
    hos_mouse_camera(CAM2);
    CHECK(near(getf(CAM2 + 0x18c), -20.0f) && near(getf(CAM2 + 0x1a4), 100.0f));
    clock_ns += 16666667;
    hos_mouse_camera(CAM2);
    CHECK(near(getf(CAM2 + 0x18c), -20.0f) && near(getf(CAM2 + 0x1a4), 100.0f));
    putf(OTHER + 0x18c, NAN);   /* (no limits, no angles: not a camera) */
    move_x = 50;
    hos_mouse_camera(OTHER);
    CHECK(move_x == 50 && isnan(getf(OTHER + 0x18c)));
    move_x = 0;
    menu_native = 0;   /* the menu's Mouse camera off: the hook does nothing */
    putf(YAW, 0);
    frame(10, 0);
    CHECK(near(getf(YAW), 0.0f));
    menu_native = 1;
    seed_at(10, 30);
    /* the game's own state: only 3 (it follows Link) and 4 (the player's) turn with the mouse */
    puti(CAM + 0x204, 1);
    putf(YAW, 0);
    move_x = 10;
    hos_mouse_camera(CAM);
    CHECK(move_x == 10 && near(getf(YAW), 0.0f));   /* (a lock-on, a menu, first person: the right stick's) */
    CHECK(hos_mouse_native() == -1);
    move_x = 0;
    puti(CAM + 0x204, 3);
    /* a button the game takes the stick for (ZR, the bow's aim; L, the item wheel; R): the game's camera too */
    hos_mouse_native_free(0);
    putf(YAW, 5);
    move_x = 10;
    hos_mouse_camera(CAM);
    CHECK(move_x == 10 && near(getf(YAW), 5.0f));
    CHECK(hos_mouse_native() == -1);
    hos_mouse_native_free(1);
    move_x = 0;
    captured = 0;
    CHECK(hos_mouse_native() == 0);   /* not captured: nothing */
    captured = 1;
    clock_ns += 200000000LL;   /* no camera for 200 ms (the pause menu): the stick again */
    CHECK(hos_mouse_native() == -1);
    /* aiming in first person (the slingshot): the camera keeps updating, state 0, another draws: the stick takes it */
    for (uint32_t resumed = 3; resumed <= 4; resumed++) {
        state(4);
        frame(0, 0);   /* the camera's a moment ago (the hook ran: hos_mouse_native would give 1 for 150 ms) */
        state(0);
        putf(YAW, 0), putf(PITCH, 20);
        frame(10, 5);
        CHECK(near(getf(YAW), 0.0f) && near(getf(PITCH), 20.0f) && move_x == 10 && move_y == 5);
        CHECK(hos_mouse_native() == -1);   /* at once, not 150 ms later */
        move_x = move_y = 0;
        state(resumed);   /* following Link or turned by the player: start at the game's current angles */
        frame(10, 5);
        CHECK(near(getf(YAW), -1.0f));
        CHECK(near(getf(PITCH), 20.5f));
        CHECK(hos_mouse_native() == 1);
    }
    /* the mouse released (capture off): the game's camera is left alone, and the next captured frame starts from it */
    state(4);
    seed_at(10, 30);
    frame(0, 0);
    captured = 0;
    putf(YAW, 10);   /* (the game's follow camera turns it) */
    frame(0, 0);
    CHECK(near(getf(YAW), 10.0f));
    captured = 1;
    frame(-10, 0);
    CHECK(near(getf(YAW), 11.0f));   /* from the game's 10, not the mouse's old 30 */
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


class MouseNativeTests(unittest.TestCase):
    def test_turns_the_player_camera(self):
        if sys.platform == "win32":
            self.skipTest("host/posix's windows.h stands in for Windows here")
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-mouse-native-") as tmp:
            tmp = Path(tmp)
            (tmp / "hos.h").write_text(HOS_H, encoding="utf-8")
            # (beside the stand-in hos.h, so its #include "hos.h" finds that one)
            shutil.copy(ROOT / "host" / "mouse_native.c", tmp / "mouse_native.c")
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t"
            build = subprocess.run([compiler, "-std=gnu11", "-include", ENV_ON_H, "-Wall", "-O1", "-g", "-I", str(tmp), "-I", str(ROOT / "host" / "posix"),
                                    str(tmp / "t.c"), str(tmp / "mouse_native.c"), "-o", str(binary), "-lm"],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
