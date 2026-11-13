/* (hookshot) Native mouse look (the Insert menu's "Mouse camera", off by default; HOS_MOUSE_NATIVE=1 or 0 overrides it):
   the captured mouse turns the player camera's own yaw and pitch, in degrees, with no dead zone, no response curve, no
   smoothing and no top speed, in place of the mouse as the right stick (nn_hid.c's mouse_stick).

   Where the angles are: the player camera's update, f_80644c80 (a virtual method, the camera in x0), calls
   hos_mouse_camera first (nx2/oot_patches.py, patch_mouse_camera), on the game's thread, once a frame while that camera
   runs. The camera holds its pitch limits at +0x178 ({-85, 89} in the field), its pitch at +0x18c and its yaw at +0x1a4.
   The game turns them by the right stick in f_80649c10 (yaw, wrapped at 180) and f_8064b770 (pitch, within the limits),
   which f_80644c80 calls only while the stick is out of its dead zone (found with hardware write watchpoints on the two
   angles in Kokiri Forest). Yaw falls as the view turns right and pitch rises as it tilts down.

   The camera's state at +0x204 is 3 while it follows Link by itself, and then it eases the pitch back toward its rest and,
   as Link walks, turns the yaw back behind him; the right stick makes it 4 (the player's), where the angles stay. The
   mouse does not move the stick, so the game's follow kept easing the view back toward Link's: after a while of playing,
   the camera drifted to where Link faced. So the angles the mouse chose are the host's own (target_yaw and target_pitch
   below), put back into the camera every frame: the camera's update still runs inside the frame -- its easing, its
   follow, its collision -- but what it does to the angles never adds up over the frames. The game's own right stick is
   let through: while the camera's keys or a pad turn it (nn_hid.c, hos_mouse_native_stick), its angles become the mouse's
   new start.

   The motion is added before the camera reads its input, so the game's own collision, lock-on and scripted cameras still
   follow. The camera is the mouse's only while the game gives it free control: the hook ran within the last 150 ms, the
   camera's state at +0x204 is 3 or 4, and the game holds none of the buttons that take the right stick for its own use
   (ZR, the bow's aim; L, the item wheel; R — nn_hid.c says which with hos_mouse_native_free). Otherwise (a menu, an item
   wheel, a lock-on, the bow's aim, the pause menu, cutscenes with another camera, a build the patch did not find its
   place in) the motion is the right stick as before, so the game's own menus, selections and aiming still take it.

   The sensitivity is the mouse's (the menu's Mouse sensitivity, - and =): 0.1 degrees a count at 1. HOS_MOUSE_INVERT_Y=1
   or the menu's Invert mouse vertical flips up and down. */
#include "hos.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

int hos_mouse_take(int32_t *dx, int32_t *dy);                                   /* display.c */
void hos_mouse_give_back(int32_t dx, int32_t dy);                               /* display.c */
int rx_option(int id, int otherwise);
extern double hos_mouse_sens;

enum { LIMITS_AT = 0x178, PITCH_AT = 0x18c, YAW_AT = 0x1a4, STATE_AT = 0x204 };
#define LIVE_NS 150000000LL   /* (the hook ran this recently: the camera takes the mouse) */
#define DEG_PER_COUNT 0.1
/* (the camera's state at +0x204: 3 while it follows Link by itself, where the right stick takes it over, 4 the player's) */
#define STATE_FOLLOWS 3
#define STATE_PLAYER 4

static volatile LONG64 last_hook_ns;
static volatile LONG64 last_camera;
static volatile LONG last_state = -1;   /* (the last camera's state at +0x204) */
static volatile LONG game_free = 1;     /* (nn_hid.c: the game leaves the camera to the player this frame) */
static volatile LONG stick_held;        /* (nn_hid.c: the camera's keys or a pad turn the camera this frame) */
static uint64_t own_camera;             /* (the camera target_yaw and target_pitch belong to) */
static float target_yaw, target_pitch;  /* (the mouse's angles: the game's own follow is undone every frame) */
static int target_ok;
static long long applied, cameras_seen;

static float getf(uint64_t a) { float f; memcpy(&f, GP(a), 4); return f; }
static void putf(uint64_t a, float f) { memcpy(GP(a), &f, 4); }
static int32_t geti(uint64_t a) { int32_t v; memcpy(&v, GP(a), 4); return v; }

/* the menu's Mouse camera (option 6), or HOS_MOUSE_NATIVE */
static int native_on(void) {
    static const char *env;
    static int looked;
    if (!looked) looked = 1, env = getenv("HOS_MOUSE_NATIVE");
    return env ? atoi(env) != 0 : rx_option(6, 0);
}

static int invert_y(void) {
    static int env = -1;
    if (env < 0) env = env_on("HOS_MOUSE_INVERT_Y");
    return env || rx_option(8, 0);   /* (the menu's Invert mouse vertical) */
}

/* a camera's angles as the game keeps them: limits in order within +-90, angles finite */
static int looks_like_camera(uint64_t cam) {
    const float lo = getf(cam + LIMITS_AT), hi = getf(cam + LIMITS_AT + 4), p = getf(cam + PITCH_AT), y = getf(cam + YAW_AT);
    return isfinite(lo) && isfinite(hi) && lo < hi && lo >= -90.0f && hi <= 90.0f && isfinite(p) && isfinite(y) &&
           fabsf(y) <= 360.0f;
}

/* Whether the game gives the camera to the player now: its update ran within the last 150 ms, its state is one the
   right stick turns (3 or 4), and none of the buttons the game takes the stick for is held. Otherwise the mouse is the
   right stick, as in As the stick: in a menu, an item wheel, a lock-on, a scripted camera or the bow's aim. */
static int free_look(void) {
    if (hos_now_ns() - InterlockedCompareExchange64(&last_hook_ns, 0, 0) >= LIVE_NS) return 0;
    const int state = last_state;
    return (state == STATE_FOLLOWS || state == STATE_PLAYER) && game_free != 0;
}

/* The Controls row "Disable camera centering" (option 45), independent of the mouse mode. The game's state-3 yaw-follow
   branch asks before it runs (nx2/oot_patches.py); 0 keeps the stock behavior, including without a renderer. (#2617 took it
   out with its rewrite of this file; the lifted game still calls it, and the link failed without it) */
int hos_disable_camera_centering(void) { return rx_option(45, 0); }

/* nn_hid.c, with each read of the controller: 1 while the game leaves the camera to the player, 0 while it takes the
   right stick for its own use (ZR, the bow's aim; L, the item wheel; R) */
void hos_mouse_native_free(int free) { InterlockedExchange(&game_free, free ? 1 : 0); }

/* nn_hid.c, with each read of the controller: 1 while the player's own right stick (the camera's keys, a pad) turns the
   camera this frame. The game has then moved the angles itself, so they are the mouse's new start: the stick and the
   mouse both turn the camera instead of undoing each other. */
void hos_mouse_native_stick(int held) { InterlockedExchange(&stick_held, held ? 1 : 0); }

/* f_80644c80's first line (nx2/oot_patches.py): cam is the player camera, the game's thread */
void hos_mouse_camera(uint64_t cam) {
    if (!cam || !native_on() || !looks_like_camera(cam)) return;
    /* Only while it is the view: following Link (3) or turned by the player (4). Aiming the slingshot in first person
       leaves this camera updating with its state 0 while another one draws (traced at Hyrule Castle); shops are reported
       the same. In any other state the mouse is the right stick, which those views turn by (the hook counts as not
       run, so hos_mouse_native gives -1) */
    uint32_t state;
    memcpy(&state, GP(cam + STATE_AT), 4);
    if (state != 3 && state != 4) {
        /* (at once: the stick takes the next motion, not one burst once the 150 ms run out) */
        InterlockedExchange64(&last_hook_ns, 0);
        target_ok = 0;   /* the game's angles may change while another camera owns the view */
        /* (each state seen leaving the mouse to the stick, once: what another view, Z-targeting, climbing or swimming,
           is, to add here if one of them should turn with the mouse) */
        static uint32_t said[8];
        static int nsaid;
        int seen = 0;
        for (int i = 0; i < nsaid; i++) seen |= said[i] == state;
        if (!seen && nsaid < 8) {
            said[nsaid++] = state;
            hos_log("mouse native: camera state %u: the mouse is the right stick", state);
        }
        return;
    }
    InterlockedExchange64(&last_hook_ns, hos_now_ns());
    InterlockedExchange(&last_state, geti(cam + STATE_AT));
    if (!free_look()) {   /* (a menu, an item wheel, a lock-on, the bow: the game's camera, and the motion is the right stick's, nn_hid.c) */
        target_ok = 0;
        return;
    }
    if (InterlockedExchange64(&last_camera, (LONG64)cam) != (LONG64)cam && cameras_seen++ < 8)
        hos_log("mouse native: the player camera at %#llx turns with the mouse", (unsigned long long)cam);
    /* (a new camera, or one the game's own stick turned: its angles now are where the mouse starts from) */
    if (!target_ok || own_camera != cam || stick_held) {
        own_camera = cam;
        target_yaw = getf(cam + YAW_AT);
        target_pitch = getf(cam + PITCH_AT);
        target_ok = 1;
    }
    int32_t dx, dy;
    if (hos_mouse_take(&dx, &dy) && (dx || dy)) {
        const double k = DEG_PER_COUNT * hos_mouse_sens;
        target_yaw -= (float)(dx * k);
        target_pitch += (float)((invert_y() ? -dy : dy) * k);
        const float lo = getf(cam + LIMITS_AT), hi = getf(cam + LIMITS_AT + 4);
        while (target_yaw > 180.0f) target_yaw -= 360.0f;
        while (target_yaw < -180.0f) target_yaw += 360.0f;
        if (target_pitch < lo) target_pitch = lo;
        if (target_pitch > hi) target_pitch = hi;
        if ((++applied & 4095) == 1)
            hos_log("mouse native: %lld moves applied; pitch %.2f yaw %.2f", applied, target_pitch, target_yaw);
    }
    /* (put back every frame: the camera eases and follows by itself inside one, but none of that adds up to a view that
       drifts back toward Link while the mouse is the camera) */
    putf(cam + YAW_AT, target_yaw), putf(cam + PITCH_AT, target_pitch);
}

/* With each read of the controller (nn_hid.c, the game's threads): 0 if the mouse is not captured, 1 if the camera takes
   its motion (hos_mouse_camera), -1 if the game does not give the camera free control now (a menu, an item wheel, a
   lock-on, the bow, the wheels, no player camera: the right stick takes it) */
int hos_mouse_native(void) {
    static int said;
    if (!said) said = 1, hos_log("mouse native: on (the menu's Mouse camera, or HOS_MOUSE_NATIVE)");
    if (!free_look()) return -1;
    int32_t dx, dy;
    const int captured = hos_mouse_take(&dx, &dy);   /* (only to know the capture: the motion is the camera's) */
    if (dx || dy) hos_mouse_give_back(dx, dy);
    return captured ? 1 : 0;
}
