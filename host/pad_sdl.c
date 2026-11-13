/* Game controllers through SDL (host/hos_sdl.c: SDL3, else SDL2, third_party/sdl2/SDL2.dll; zlib license, loaded at
   run time): DualSense, DualShock 4, Switch Pro, Xbox and most other pads, over USB or Bluetooth. XInput alone sees
   only Xbox-style pads.

   A thread of its own (on Linux the window's, which runs SDL there too) opens every controller SDL knows (and ones
   plugged in later), polls them every 4 ms and keeps
   the merged state; nn_hid.c ORs it into the input with XInput, keyboard and mouse. Without SDL nothing
   changes. HOS_NO_SDL=1 leaves SDL out. The inputs go by position (HOS_PAD_*: the bottom face button
   first, cross on a DualSense, A on an Xbox pad); nn_hid.c's controls make them the Switch's buttons.

   (hookshot) Motion: with the Insert menu's Motion controls on (the default), the open pad with a gyroscope and an
   accelerometer (Switch Pro and pads in Switch mode, DualSense, DualShock 4, others) that was used last has them read
   each poll and turned into the six-axis state OoT aims with (host/hos_motion.h), which nn_hid.c's
   GetSixAxisSensorStates gives the game; Motion sensitivity X/Y and Invert motion X/Y apply there. Off, or with no such pad, the game sees the pad
   lying still, as before. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "hos_sdl.h"
#include "hd_rumble.h"
#include "hos_motion.h"
#include "steam_pads.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_PAD_*) */
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

void hos_host_thread_register_self(const char *name);   /* hos_prof.c */

/* (BTN_A ... BTN_RIGHT are HOS_PAD_SOUTH ... HOS_PAD_RIGHT) */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_BACK, BTN_GUIDE, BTN_START, BTN_LSTICK, BTN_RSTICK, BTN_LSHOULDER, BTN_RSHOULDER,
       BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_MISC1, BTN_RPADDLE1, BTN_LPADDLE1, BTN_RPADDLE2, BTN_LPADDLE2,
       BTN_TOUCHPAD = 20, BTN_N = 21 };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT };

#define MAX_PADS 8
/* (motion: whether it has the sensors, whether they are on, when to try again after they would not turn on; used_ns:
   its last button or stick change; serial: which opening this is, so a pad reconnecting into the slot is a new one;
   bias: its gyroscope's learned resting rate, kept while another pad has motion) */
static struct {
    void *gc;
    int32_t id;
    uint16_t rumble_lo, rumble_hi;
    int64_t rumble_ns;
    uint8_t hd, hd_kind, hd_ever;   /* HD rumble (hd_rumble.h): HD_RUMBLE_UNKNOWN / ON / OFF, the pad's kind, sent once */
    uint8_t hd_sent[HD_RUMBLE_PACKET_SIZE];
    int64_t hd_ns;
    int motion, motion_on;
    int64_t motion_retry_ns, used_ns;
    uint64_t input_sig;
    unsigned rested;   /* (hookshot) the sticks' axes seen at rest since it was opened, a bit each: only those steer */
    signed char held[4];   /* (the way each axis was last held, -1, 0 or 1: one held the other way has passed its rest) */
    uint32_t serial;
    float bias[3], bias_t;
    int has_bias;
    uint16_t vendor;      /* (hookshot) its USB vendor, and its Steam Input handle: not 0 for a Steam stand-in pad */
    uint64_t steam;
    int motion_only;      /* (host/steam_pads.h) a real Valve pad while a Steam stand-in pad is open: motion, no input */
} pads[MAX_PADS];
static uint32_t pads_opened;
static SRWLOCK state_lock = SRWLOCK_INIT;
static uint64_t state_buttons;
static int32_t state_stick[4];
static int state_count;
static char state_name[128];   /* the first controller's name (host/prompts.c: the button prompts' Auto) */
static volatile LONG rumble_low, rumble_high;   /* 0..65535, set by the vibration HLE */
/* the game's own vibration values per side (0 left, 1 right: {amp_low, freq_low, amp_high, freq_high}), for the pads
   that play HD rumble; silence until the game sends one */
static SRWLOCK hd_lock = SRWLOCK_INIT;
static float hd_values[2][4] = {{0.0f, 160.0f, 0.0f, 320.0f}, {0.0f, 160.0f, 0.0f, 320.0f}};
/* (motion) the pad read and its filter (the polling thread's own), then what the game is handed (under state_lock): the
   last HOS_MOTION_HISTORY readings, newest at motion_hist[motion_head]; gyro_status for the menu */
static HosMotion motion;
static int64_t motion_ns, motion_reading_ns, motion_log_ns, motion_readings, motion_start_ns;
static int motion_warned;
static int motion_pad = -1;
static uint32_t motion_serial;
static HosMotionSample motion_hist[HOS_MOTION_HISTORY];
static int motion_head, motion_count, motion_live;
static volatile LONG gyro_status;   /* 0 no controller, 1 controllers but none with a gyro, 2 one with a gyro */

void hos_filter_stick(int16_t x, int16_t y, int zone, int32_t out[2]);   /* nn_hid.c */

/* SDL's controller support started; 0 when it could not be */
static int pads_start(void) {
    /* poll in the background, whatever window is in front (nn_hid.c decides when input counts) */
    hos_sdl_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    /* (SDL2: PlayStation pads' rumble asked for; SDL3 turns it on when a rumble is first sent) */
    hos_sdl_hint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
    hos_sdl_hint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
    /* (the map below is by position; SDL2 reports Nintendo pads by their labels unless told not to, which put a Switch Pro
       Controller's A, on the right, at the bottom button's place: A acted as B and X as Y: SkyboundTotk issue 12. SDL3's
       are by position always) */
    hos_sdl_hint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    /* (with no window nothing reads SDL's quit event: SDL's own SIGINT/SIGTERM handlers turned a timeout's or a service
       manager's SIGTERM into that event, and the run went on until SIGKILL. Without them the signals end it as usual) */
    if (env_on("HOS_HEADLESS")) hos_sdl_hint("SDL_NO_SIGNAL_HANDLERS", "1");
    /* (hookshot) Valve's own pads taken out of the ignore list Steam sets for the games it starts, so that their motion
       sensors are read (host/steam_pads.h, #1897). Only with an SDL that tells Steam's stand-in pads apart: without
       that, a real pad and its stand-in would both play. A list emptied or unset in Steam's launch options is left as
       it is */
    {
        const char *list = getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
        char kept[4096];
        if (list && *list && hos_sdl_knows_steam_pads() && steam_pads_without_valve(list, kept, sizeof kept) > 0 &&
            hos_sdl_hint_override("SDL_GAMECONTROLLER_IGNORE_DEVICES", kept))
            hos_log("hid: Valve's controllers (not the Steam Deck's) taken out of Steam's ignore list, for their motion sensors");
    }
    if (!hos_sdl_init(HOS_SDL_GAMEPAD)) {
        hos_log("hid: SDL could not start its controller support: %s", hos_sdl_error());
        return 0;
    }
    hos_log("hid: SDL controller support ready (DualSense, DualShock 4, Switch Pro, Xbox and others)");
    return 1;
}

/* SDL_GameControllerRumble runs only for the duration it is given, then stops, and a repeat of the same numbers was
   skipped. A rumble the game holds, and one that was already playing when this controller opened, never reached it. */
static int rumble_due(unsigned lo, unsigned hi, unsigned sent_lo, unsigned sent_hi, long long now, long long sent_ns) {
    if (lo != sent_lo || hi != sent_hi) return 1;
    if ((lo || hi) && now - sent_ns >= 500000000LL) return 1;   /* (half of the 1 s effect, so it does not lapse) */
    return 0;
}

/* The Insert menu's Rumble strength (percent; 0 Off) on the motors' 0..65535, heavy and light together: up to 100 each
   motor is scaled on its own (100 sends them as they are); above, when the stronger would pass the motor's top, both
   are scaled by the same factor so that it just reaches it, keeping the balance between the motors that the frequency
   split chose. Off stops a rumble that is playing (the poll sends the change). */
static void rumble_strength(unsigned heavy, unsigned light, int percent, uint16_t out[2]) {
    if (percent <= 0) {
        out[0] = out[1] = 0;
        return;
    }
    unsigned long long h = (unsigned long long)heavy * (unsigned)percent / 100u;
    unsigned long long l = (unsigned long long)light * (unsigned)percent / 100u;
    if (h > 0xFFFFu || l > 0xFFFFu) {   /* (from the motors' own values: the same factor for both, the stronger at the top) */
        const unsigned long long top = heavy > light ? heavy : light;
        h = heavy * 0xFFFFull / top;
        l = light * 0xFFFFull / top;
    }
    out[0] = (uint16_t)h;
    out[1] = (uint16_t)l;
}

/* A pad SDL just opened: HD rumble when it is a Switch Pro Controller or a Joy-Con and the SDL has the effect call
   (HOS_HD_RUMBLE=0: never); whether SDL's driver takes it shows with the first packet. */
static void hd_open(int i, void *gc) {
    static int env_off = -1;
    if (env_off < 0) {
        const char *e = getenv("HOS_HD_RUMBLE");
        env_off = e && !strcmp(e, "0");
    }
    pads[i].hd_kind = (uint8_t)hd_rumble_kind(hos_sdl_load(), hos_sdl_pad_type(gc));
    pads[i].hd = (uint8_t)hd_rumble_start(pads[i].hd_kind, hos_sdl_pad_has_effect(), env_off);
    pads[i].hd_ever = 0;
    pads[i].hd_ns = 0;
    const uint8_t n[4] = {0x00, 0x01, 0x40, 0x40};
    hd_rumble_packet(HD_RUMBLE_PRO, n, n, pads[i].hd_sent);
}

/* A pad's HD rumble, each poll: its packet when due (hd_rumble_due). 1 when the pad plays HD rumble (the motor rumble
   is not sent to it); 0 when it does not, or SDL just refused it (then it gets the motor rumble from now on). side:
   the left side's bytes, the right's, and both merged (for a single Joy-Con, which has only one). */
static int hd_poll(int i, const uint8_t side[3][4], long long now) {
    if (pads[i].hd == HD_RUMBLE_OFF) return 0;
    uint8_t packet[HD_RUMBLE_PACKET_SIZE];
    const int single = pads[i].hd_kind == HD_RUMBLE_JOYCON_LEFT || pads[i].hd_kind == HD_RUMBLE_JOYCON_RIGHT;
    hd_rumble_packet(pads[i].hd_kind, single ? side[2] : side[0], single ? side[2] : side[1], packet);
    if (!hd_rumble_due(packet, pads[i].hd_sent, pads[i].hd_ever, now, pads[i].hd_ns)) return 1;
    const int was = pads[i].hd;
    pads[i].hd = (uint8_t)hd_rumble_after_send(hos_sdl_pad_effect(pads[i].gc, packet, sizeof packet));
    if (pads[i].hd == HD_RUMBLE_OFF) {   /* (refused: HD rumble off for this pad until it is closed and opened again.
                                           SDL3 queues the write, so false means the driver does not support the effect,
                                           not a write that failed once) */
        hos_log("hid: \"%s\" did not take HD rumble (%s): its rumble goes to its motors", hos_sdl_pad_name(pads[i].gc),
                hos_sdl_error());
        pads[i].rumble_lo = pads[i].rumble_hi = 0;   /* (the motor path sends what is playing at once) */
        pads[i].rumble_ns = 0;
        return 0;
    }
    if (was == HD_RUMBLE_UNKNOWN) hos_log("hid: HD rumble on \"%s\"", hos_sdl_pad_name(pads[i].gc));
    memcpy(pads[i].hd_sent, packet, sizeof packet);
    pads[i].hd_ns = now;
    pads[i].hd_ever = 1;
    return 1;
}

/* (motion) the sensors on while Motion controls is on (HOS_OPT_GYRO, on without the menu), off otherwise; the pad with
   them that was used last read into the filter. count: the pads open */
static void motion_poll(int64_t now, int count) {
    int rx_option(int id, int otherwise);
    static int debug = -1;   /* (HOS_MOTION_DEBUG, read once) */
    if (debug < 0) debug = env_on("HOS_MOTION_DEBUG");
    const int want = rx_option(HOS_OPT_GYRO, 1) != 0;
    int pick = -1, any = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i].gc || !pads[i].motion) continue;
        any = 1;
        if (pads[i].motion_on != want && now >= pads[i].motion_retry_ns) {
            const int ok = hos_sdl_pad_sensor_enable(pads[i].gc, HOS_SDL_SENSOR_ACCEL, want) &
                           hos_sdl_pad_sensor_enable(pads[i].gc, HOS_SDL_SENSOR_GYRO, want);
            if (want && !ok) {   /* (both off again, and another try in two seconds: a pad over Bluetooth may still be
                                     starting) */
                hos_sdl_pad_sensor_enable(pads[i].gc, HOS_SDL_SENSOR_ACCEL, 0);
                hos_sdl_pad_sensor_enable(pads[i].gc, HOS_SDL_SENSOR_GYRO, 0);
                pads[i].motion_on = 0;
                if (!pads[i].motion_retry_ns)
                    hos_log("hid: \"%s\": its motion sensors could not be turned on yet, trying again",
                            hos_sdl_pad_name(pads[i].gc));
                pads[i].motion_retry_ns = now + 2000000000LL;
            } else {
                pads[i].motion_on = want;
                pads[i].motion_retry_ns = 0;
            }
        }
        if (want && pads[i].motion_on && (pick < 0 || pads[i].used_ns > pads[pick].used_ns)) pick = i;
    }
    InterlockedExchange(&gyro_status, any ? 2 : count ? 1 : 0);
    float a[3], g[3];
    const int read = pick >= 0 && hos_sdl_pad_sensor(pads[pick].gc, HOS_SDL_SENSOR_ACCEL, a, 3) &&
                     hos_sdl_pad_sensor(pads[pick].gc, HOS_SDL_SENSOR_GYRO, g, 3);
    /* (a gyroscope reading the very same values for over a second has stopped: a real one always moves a little. Seen with a
       Steam Controller under Steam after Alt+Tab, when Steam's layout switch left its sensors off while SDL still had them on.
       Off and on again, as the menu's Motion controls row does, brings it back) */
    {
        static float last_g[3];
        static int64_t same_since, rearm_ns;
        if (read && !memcmp(g, last_g, sizeof g)) {
            if (!same_since) same_since = now;
            else if (now - same_since > 1200000000LL && now >= rearm_ns) {
                hos_sdl_pad_sensor_enable(pads[pick].gc, HOS_SDL_SENSOR_ACCEL, 0);
                hos_sdl_pad_sensor_enable(pads[pick].gc, HOS_SDL_SENSOR_GYRO, 0);
                pads[pick].motion_on = 0;   /* (turned on again by the loop above on the next poll) */
                hos_log("hid: \"%s\": its gyroscope stopped changing: motion sensors turned off and on again",
                        hos_sdl_pad_name(pads[pick].gc));
                same_since = 0, rearm_ns = now + 3000000000LL;
            }
        } else {
            if (read) memcpy(last_g, g, sizeof g);
            same_since = 0;
        }
    }
    /* (the pad that had motion keeps what its filter learned of its gyroscope's bias, for when it has motion again) */
    if (motion_pad >= 0 && (!read || pick != motion_pad) && pads[motion_pad].gc && pads[motion_pad].serial == motion_serial &&
        motion.learned_t > 0) {
        memcpy(pads[motion_pad].bias, motion.bias, sizeof motion.bias);
        pads[motion_pad].bias_t = motion.learned_t, pads[motion_pad].has_bias = 1;
    }
    if (!read) {
        motion_pad = -1;
        AcquireSRWLockExclusive(&state_lock);
        motion_live = 0, motion_count = 0;
        ReleaseSRWLockExclusive(&state_lock);
        return;
    }
    int fresh = 0;
    if (pick != motion_pad || pads[pick].serial != motion_serial) {   /* (another pad, or one reconnected: a fresh filter) */
        hos_motion_reset(&motion);
        if (pads[pick].has_bias)
            memcpy(motion.bias, pads[pick].bias, sizeof motion.bias), motion.learned_t = pads[pick].bias_t;
        motion_pad = pick, motion_serial = pads[pick].serial, motion_ns = now;
        fresh = 1;
        motion_start_ns = now, motion_warned = 0;
        hos_log("hid: motion controls from \"%s\"", hos_sdl_pad_name(pads[pick].gc));
    }
    /* the filter runs outside the lock (the game's input calls take it); a repeat of the last reading is no reading */
    const int64_t dt_ns = now - motion_ns;
    motion_ns = now;
    {   /* (the Motion deadzone row: the console's tight, standard and loose zero-drift thresholds, turns a second) */
        static const float zone[3] = {HOS_MOTION_THRESHOLD_TIGHT, HOS_MOTION_THRESHOLD, HOS_MOTION_THRESHOLD_LOOSE};
        const int z = rx_option(HOS_OPT_GYRO_DEADZONE, 1);
        motion.threshold = zone[z < 0 ? 0 : z > 2 ? 2 : z];
    }
    if (!hos_motion_update(&motion, a, g, (float)(dt_ns / 1e9), rx_option(HOS_OPT_GYRO_SENS_X, 100) / 100.0f,
                           rx_option(HOS_OPT_GYRO_SENS_Y, 100) / 100.0f, rx_option(HOS_OPT_GYRO_INV_X, 0),
                           rx_option(HOS_OPT_GYRO_INV_Y, 0)) && !fresh)
        return;
    if (!motion_warned && motion.learned_t <= 0 && now - motion_start_ns > 30000000000LL) {
        motion_warned = 1;   /* (a pad noisier than the stillness test allows: its drift is never taken off) */
        hos_log("hid: motion: \"%s\" has not been set down yet, so its gyroscope's drift is not learned: set it on a "
                "table for a second (if it is on a table and this stays, its sensor may be noisier than the stillness "
                "test allows)", hos_sdl_pad_name(pads[pick].gc));
    }
    if (debug && now >= motion_log_ns) {   /* (twice a second: what the game gets) */
        motion_log_ns = now + 500000000LL;
        hos_log("hid: motion: accel (%+.2f, %+.2f, %+.2f) G, gyro (%+.3f, %+.3f, %+.3f) turns/s, direction rows "
                "(%+.2f %+.2f %+.2f / %+.2f %+.2f %+.2f / %+.2f %+.2f %+.2f), bias (%+.4f, %+.4f, %+.4f)", motion.out.accel[0],
                motion.out.accel[1], motion.out.accel[2], motion.out.gyro[0], motion.out.gyro[1], motion.out.gyro[2],
                motion.out.direction[0], motion.out.direction[1], motion.out.direction[2], motion.out.direction[3],
                motion.out.direction[4], motion.out.direction[5], motion.out.direction[6], motion.out.direction[7],
                motion.out.direction[8], motion.bias[0], motion.bias[1], motion.bias[2]);
    }
    HosMotionSample r;
    r.st = motion.out;
    r.n = ++motion_readings;
    /* (the time since the reading before, capped as the filter caps it: a stalled pad's next reading is no jump) */
    r.dt_ns = fresh ? 5000000 : now - motion_reading_ns;
    if (r.dt_ns > (int64_t)(HOS_MOTION_DT_MAX * 1e9)) r.dt_ns = (int64_t)(HOS_MOTION_DT_MAX * 1e9);
    motion_reading_ns = now;
    AcquireSRWLockExclusive(&state_lock);
    motion_head = (motion_head + 1) % HOS_MOTION_HISTORY;
    motion_hist[motion_head] = r;
    if (fresh) motion_count = 0;
    if (motion_count < HOS_MOTION_HISTORY) motion_count++;
    motion_live = 1;
    ReleaseSRWLockExclusive(&state_lock);
}

/* (hookshot) a motion-only pad's own presses (host/steam_pads.h): they are not the game's input, but they still mark it
   the pad used last, which motion_poll reads motion from */
static void motion_only_used(int i, int64_t now) {
    int rx_option(int id, int otherwise);
    const int z = rx_option(11, 0);   /* (the menu's Stick dead zone) */
    uint64_t sig = 0;
    for (int k = BTN_A; k <= BTN_RIGHT; k++)
        if (hos_sdl_pad_button(pads[i].gc, k)) sig |= 1ULL << k;
    int32_t v[4];
    hos_filter_stick(hos_sdl_pad_axis(pads[i].gc, AX_LX), hos_sdl_pad_axis(pads[i].gc, AX_LY), z, v);
    hos_filter_stick(hos_sdl_pad_axis(pads[i].gc, AX_RX), hos_sdl_pad_axis(pads[i].gc, AX_RY), z, v + 2);
    for (int k = 0; k < 4; k++) sig |= (uint64_t)(v[k] > 0 ? 1 : v[k] < 0 ? 2 : 0) << (32 + 2 * k);
    if (sig != pads[i].input_sig) pads[i].input_sig = sig, pads[i].used_ns = now;
}

/* one poll: controllers plugged in or gone (each second), then every open one's buttons, sticks and rumble */
static void pads_poll(void) {
    static int64_t next_scan = 0;
    hos_sdl_pads_update();
        const int64_t now = hos_now_ns();
        if (now >= next_scan) {   /* new controllers (and ones that went away) */
            next_scan = now + 1000000000LL;
            for (int i = 0; i < MAX_PADS; i++)
                if (pads[i].gc && !hos_sdl_pad_connected(pads[i].gc)) {
                    hos_log("hid: controller \"%s\" disconnected (SDL)", hos_sdl_pad_name(pads[i].gc));
                    hos_sdl_pad_close(pads[i].gc);
                    pads[i].gc = NULL;
                    pads[i].rumble_lo = pads[i].rumble_hi = 0;
                    pads[i].rumble_ns = 0;
                    pads[i].motion = pads[i].motion_on = 0;
                }
            int32_t ids[32];
            int opens[32];
            const int nj = hos_sdl_pads_list(ids, opens, 32);
            for (int j = 0; j < nj; j++) {
                const int32_t id = ids[j];
                int have = 0, slot = -1;
                for (int i = 0; i < MAX_PADS; i++) {
                    if (pads[i].gc && pads[i].id == id) have = 1;
                    if (!pads[i].gc && slot < 0) slot = i;
                }
                if (have || slot < 0) continue;
                void *gc = hos_sdl_pad_open(opens[j]);
                if (!gc) continue;
                pads[slot].gc = gc;
                pads[slot].id = hos_sdl_pad_id(gc);
                pads[slot].rumble_lo = pads[slot].rumble_hi = 0;
                pads[slot].rumble_ns = 0;
                hd_open(slot, gc);
                pads[slot].motion = hos_sdl_pad_has_sensor(gc, HOS_SDL_SENSOR_GYRO) && hos_sdl_pad_has_sensor(gc, HOS_SDL_SENSOR_ACCEL);
                pads[slot].motion_on = 0;
                pads[slot].motion_retry_ns = 0;
                pads[slot].used_ns = now, pads[slot].input_sig = 0;   /* (just connected: the one about to be used) */
                pads[slot].rested = 0;
                memset(pads[slot].held, 0, sizeof pads[slot].held);
                pads[slot].serial = ++pads_opened;
                pads[slot].has_bias = 0;
                pads[slot].vendor = hos_sdl_pad_vendor(gc);
                pads[slot].steam = hos_sdl_pad_steam_handle(gc);
                pads[slot].motion_only = 0;   /* (the poll below decides, and says so) */
                hos_sdl_pad_log_connection(gc, pads[slot].motion, pads[slot].steam != 0);
            }
        }
        uint64_t b = 0;
        int32_t s[4] = {0};
        int count = 0;
        int rx_option(int id, int otherwise);
        const int strength = rx_option(9, 100);   /* (the menu's Rumble strength: the motors' and HD rumble's) */
        uint16_t motors[2];
        rumble_strength((uint16_t)rumble_low, (uint16_t)rumble_high, strength, motors);
        const uint16_t lo = motors[0], hi = motors[1];
        uint8_t side[3][4];   /* (the HD rumble pads': each side's bytes, then both merged for a single Joy-Con) */
        {
            float hv[2][4], felt[3][4];
            AcquireSRWLockShared(&hd_lock);
            memcpy(hv, hd_values, sizeof hv);
            ReleaseSRWLockShared(&hd_lock);
            hd_rumble_felt(hv[0], felt[0]);   /* (the game's small amplitudes, as the motors get them) */
            hd_rumble_felt(hv[1], felt[1]);
            hd_rumble_encode(felt[0], strength, side[0]);
            hd_rumble_encode(felt[1], strength, side[1]);
            hd_rumble_merge(felt[0], felt[1], felt[2]);
            hd_rumble_encode(felt[2], strength, side[2]);
        }
        int stand_in = 0;   /* (hookshot) a Steam stand-in pad open (host/steam_pads.h) */
        for (int i = 0; i < MAX_PADS; i++) stand_in |= pads[i].gc && pads[i].steam;
        for (int i = 0; i < MAX_PADS; i++) {
            void *gc = pads[i].gc;
            if (!gc) continue;
            /* (hookshot) a real Valve pad beside its Steam stand-in: its motion is read (motion_poll), its buttons and
               sticks come through the stand-in as the player's Steam layout maps them, so they are not merged twice,
               and its rumble goes through Steam too. Its own presses still mark it the pad used last, for motion */
            const int motion_only = steam_pads_motion_only(pads[i].vendor, pads[i].steam, stand_in);
            if (motion_only != pads[i].motion_only) {
                pads[i].motion_only = motion_only;
                /* (a real Valve pad this SDL opens with no gyroscope: an SDL with no driver for it, so the loosened
                   list gives no motion, said once so the next person with #1897 sees why) */
                if (motion_only && !pads[i].motion)
                    hos_log("hid: \"%s\": no motion sensors through this SDL, so Motion controls can't read it; "
                            "its buttons come through Steam Input's pad", hos_sdl_pad_name(gc));
                else
                    hos_log(motion_only ? "hid: \"%s\": motion only, its buttons come through Steam Input's pad"
                                        : "hid: \"%s\": its buttons count again (no Steam Input pad is open)",
                            hos_sdl_pad_name(gc));
            }
            if (motion_only) {
                motion_only_used(i, now);
                continue;
            }
            count++;
            uint64_t sig = 0;
            for (int k = BTN_A; k <= BTN_RIGHT; k++)   /* (SDL3's gamepad buttons are in SDL2's order) */
                if (hos_sdl_pad_button(gc, k)) sig |= 1ULL << k;
            /* The paddle buttons follow MISC1 in SDL, and the triggers in HOS_PAD_*. */
            for (int k = BTN_RPADDLE1; k <= BTN_LPADDLE2; k++)
                if (hos_sdl_pad_button(gc, k)) sig |= 1ULL << (HOS_PAD_RPADDLE1 + k - BTN_RPADDLE1);
            b |= sig;
            /* (hookshot) a DualShock 4's or DualSense's touchpad click: a Controls row's input, or unbound it opens the
               Insert menu as F1 does (vk_menu.cpp). SDL numbers it 20, past the buttons above; an SDL without it answers 0 */
            if (hos_sdl_pad_button(gc, BTN_TOUCHPAD)) b |= 1ULL << HOS_PAD_TOUCHPAD;
            if (hos_sdl_pad_axis(gc, AX_LT) > 8000) b |= 1ULL << HOS_PAD_LTRIGGER;
            if (hos_sdl_pad_axis(gc, AX_RT) > 8000) b |= 1ULL << HOS_PAD_RTRIGGER;
            const int inv_x = rx_option(16, 0);
            const int inv_y = rx_option(17, 0);
            const int z = rx_option(11, 0);   /* (the menu's Stick dead zone) */
            int32_t v[4];
            hos_filter_stick(hos_sdl_pad_axis(gc, AX_LX), hos_sdl_pad_axis(gc, AX_LY), z, v);
            hos_filter_stick(hos_sdl_pad_axis(gc, AX_RX), hos_sdl_pad_axis(gc, AX_RY), z, v + 2);
            v[1] = -v[1];   /* SDL's Y axes point down */
            v[3] = -v[3];
            /* (hookshot) An axis steers once it has been seen at rest (inside the dead zone) since the pad was opened. The sticks are
               the strongest of all the pads', so a device whose axis reads held for good (a mapping that puts a trigger or an
               unused axis on a stick, a second virtual pad) turned the camera or walked Link in circles on its own, with the
               player's pad at rest (#1673: Xbox 360 pad, CachyOS; #1661: GameSir 2 Lite, wired). A real stick rests at the start. */
            /* (a worn stick resting just past a small dead zone counts once it is pushed the other way: it went past its rest) */
            /* Rest is judged per axis, as before the circle: a phantom axis held past the dead zone keeps its partner
               outside the circle, and the partner must still rest at its own center. */
            for (int k = 0; k < 4; k++) {
                int32_t alone[2];
                hos_filter_stick((int16_t)v[k], 0, z, alone);
                const signed char way = (signed char)(alone[0] > 0 ? 1 : alone[0] < 0 ? -1 : 0);
                if (!way || (pads[i].held[k] && way != pads[i].held[k])) pads[i].rested |= 1u << k;
                pads[i].held[k] = way;
                if (!(pads[i].rested >> k & 1)) v[k] = 0;
            }
            /* An axis held back above no longer counts toward its stick's circle, so its partner's drift stays inside
               the dead zone. Values are already clamped and the circle keeps them, so this changes nothing otherwise. */
            hos_filter_stick((int16_t)v[0], (int16_t)v[1], z, v);
            hos_filter_stick((int16_t)v[2], (int16_t)v[3], z, v + 2);
            /* Camera inversion changes output, not whether the physical stick crossed its rest position. Otherwise
               toggling the setting admits a held phantom axis, which stays admitted after the setting is restored (#2273). */
            if (inv_x) v[2] = -v[2];
            if (inv_y) v[3] = -v[3];
            for (int k = 0; k < 4; k++)
                if ((v[k] < 0 ? -v[k] : v[k]) > (s[k] < 0 ? -s[k] : s[k])) s[k] = v[k];
            {   /* (motion: the pad last used; a stick counts by which way it is pushed, not by how far) */
                for (int k = 0; k < 4; k++) sig |= (uint64_t)(v[k] > 0 ? 1 : v[k] < 0 ? 2 : 0) << (32 + 2 * k);
                if (sig != pads[i].input_sig) pads[i].input_sig = sig, pads[i].used_ns = now;
            }
            if (hd_poll(i, side, now)) continue;
            if (rumble_due(lo, hi, pads[i].rumble_lo, pads[i].rumble_hi, now, pads[i].rumble_ns)) {
                hos_sdl_pad_rumble(gc, lo, hi, lo || hi ? 1000 : 0);
                pads[i].rumble_lo = lo;
                pads[i].rumble_hi = hi;
                pads[i].rumble_ns = now;
            }
        }
        motion_poll(now, count);
        AcquireSRWLockExclusive(&state_lock);
        state_buttons = b;
        memcpy(state_stick, s, sizeof s);
        state_count = count;
        state_name[0] = 0;
        for (int i = 0; i < MAX_PADS; i++)
            if (pads[i].gc && !pads[i].motion_only) {   /* (the stand-in's name, not its motion-only pad's) */
                const char *name = hos_sdl_pad_name(pads[i].gc);
                snprintf(state_name, sizeof state_name, "%s", name ? name : "");
                break;
            }
        ReleaseSRWLockExclusive(&state_lock);
}

static DWORD WINAPI pad_main(void *arg) {
    (void)arg;
    hos_host_thread_register_self("gamepads (SDL)");
    if (!pads_start()) return 0;
    for (;;) {
        pads_poll();
        Sleep(4);
    }
}
#ifndef _WIN32
int hos_display_owns_sdl(void);   /* host/posix/display_sdl.c */
/* (linux) SDL also runs the window (host/posix/display_sdl.c), and its event queue is not for two threads. Polled
   from the window's thread, between its events. Two threads pumping it corrupted the heap once a controller was
   attached. */
static volatile LONG pads_loaded;
void hos_sdl_pads_pump(void) {
    static int state;   /* 0 not started, 1 polling, -1 not available */
    if (!state && pads_loaded) state = pads_start() ? 1 : -1;
    if (state > 0) pads_poll();
}
#endif

/* The controllers' merged state (nn_hid.c): ORs their inputs in (1 << HOS_PAD_*), keeps the larger stick
   deflections; returns how many controllers SDL has open. Starts the SDL thread on the first call. */
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) {
    static volatile LONG started;
    if (!started && !InterlockedExchange(&started, 1)) {
        if (env_on("HOS_NO_SDL")) return 0;
        if (!hos_sdl_load()) {
            hos_log("hid: no SDL beside the exe or on the system: only XInput controllers (Xbox pads) work");
            return 0;
        }
#ifdef _WIN32
        CreateThread(NULL, 0, pad_main, NULL, 0, NULL);
#else
        if (hos_display_owns_sdl())
            InterlockedExchange(&pads_loaded, 1);   /* (the window's thread polls: hos_sdl_pads_pump) */
        else
            CreateThread(NULL, 0, pad_main, NULL, 0, NULL);   /* (no window: nothing else pumps SDL) */
#endif
    }
    AcquireSRWLockShared(&state_lock);
    const int n = state_count;
    *buttons |= state_buttons;
    for (int k = 0; k < 4; k++)
        if ((state_stick[k] < 0 ? -state_stick[k] : state_stick[k]) > (stick[k] < 0 ? -stick[k] : stick[k]))
            stick[k] = state_stick[k];
    ReleaseSRWLockShared(&state_lock);
    return n;
}

/* (motion) the latest readings for nn_hid.c's GetSixAxisSensorStates, newest first: up to max, how many there are; 0
   when there are none (Motion controls off, or no pad with a gyroscope) */
int hos_sdl_motion_history(HosMotionSample *out, int max) {
    AcquireSRWLockShared(&state_lock);
    const int n = !motion_live ? 0 : motion_count < max ? motion_count : max;
    for (int i = 0; i < n; i++) out[i] = motion_hist[(motion_head - i + HOS_MOTION_HISTORY) % HOS_MOTION_HISTORY];
    ReleaseSRWLockShared(&state_lock);
    return n;
}
/* (the Insert menu's Motion controls row) 0 no controller, 1 controllers with no gyroscope, 2 one with a gyroscope */
int hos_sdl_gyro_status(void) { return (int)gyro_status; }
/* The first open controller's name into out ("" with none) */
void hos_sdl_pad_first_name(char *out, size_t cap) {
    AcquireSRWLockShared(&state_lock);
    snprintf(out, cap, "%s", state_name);
    ReleaseSRWLockShared(&state_lock);
}

/* Vibration from the game (0..1 amplitudes of the low and high bands), for the pads that rumble. */
void hos_sdl_rumble(float low, float high) {
    if (low < 0) low = 0;
    if (low > 1) low = 1;
    if (high < 0) high = 0;
    if (high > 1) high = 1;
    InterlockedExchange(&rumble_low, (LONG)(low * 65535));
    InterlockedExchange(&rumble_high, (LONG)(high * 65535));
}

/* The game's own vibration value for one side (0 left, 1 right; {amp_low, freq_low, amp_high, freq_high}), for the
   pads that play HD rumble (hd_rumble.h); nn_hid.c's vibration_send, beside hos_sdl_rumble. */
void hos_sdl_rumble_hd(int side, const float value[4]) {
    AcquireSRWLockExclusive(&hd_lock);
    memcpy(hd_values[side & 1], value, sizeof hd_values[0]);
    ReleaseSRWLockExclusive(&hd_lock);
}
