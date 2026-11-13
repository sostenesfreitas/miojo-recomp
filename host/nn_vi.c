/* nn::vi for MKW (TOTKPort's nn_vi.c is bound to its renderer and TOTK's frame pacing): the default display, one
   layer, and the display's vsync event, signaled at 60 Hz by a host thread. HDR and VRR are reported unsupported,
   so the game takes its SDR path. Frames are paced at the native NVN layer's window acquire (the end of this file). */
#include "hos.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_OPT_HDR_*, HOS_OPT_CAMERA_SPEED: the Insert menu's HDR and Camera speed, answered by the renderer) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);

static uint64_t display, layer;
static uint64_t vsync_events[8];
static volatile LONG nvsync;
static HANDLE vsync_thread;

static int fps_max(void);   /* (below: HOS_FPS_MAX) */
static DWORD WINAPI vsync_main(void *p) {
    (void)p;
    const int64_t period = 1000000000LL / fps_max();
    int64_t next = hos_now_ns() + period;
    for (;;) {
        const int64_t now = hos_now_ns();
        if (next > now) hos_sleep_ns(next - now);
        next += period;
        if (next < hos_now_ns()) next = hos_now_ns() + period;   /* (fell behind: no burst of vsyncs) */
        for (LONG i = 0; i < nvsync; i++) hos_system_event_signal(vsync_events[i]);
    }
    return 0;
}

/* nn::vi::Initialize() */
HLE(_ZN2nn2vi10InitializeEv) {}

/* nn::vi::OpenDefaultDisplay(nn::vi::Display**) -> Result */
HLE(_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE) {
    if (!display) display = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = display;
    RET(0);
}

/* nn::vi::CreateLayer(nn::vi::Layer**, nn::vi::Display*) -> Result */
HLE(_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE) {
    if (!layer) layer = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = layer;
    RET(0);
}

/* nn::vi::GetDisplayVsyncEvent(nn::os::SystemEventType*, nn::vi::Display*) -> Result */
HLE(_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE) {
    hos_system_event_init(ARG(0), 1);
    {   /* (hookshot) under a lock: two threads checked the count at once and wrote past the table */
        static SRWLOCK vsync_lock = SRWLOCK_INIT;
        AcquireSRWLockExclusive(&vsync_lock);
        if (nvsync < (LONG)(sizeof vsync_events / sizeof vsync_events[0])) {
            vsync_events[nvsync] = ARG(0);
            InterlockedIncrement(&nvsync);
        }
        ReleaseSRWLockExclusive(&vsync_lock);
    }
    if (!vsync_thread) vsync_thread = CreateThread(NULL, 0, vsync_main, NULL, 0, NULL);
    RET(0);
}

/* nn::vi::QueryDisplayHdrStatus(bool* on, HdrOutputFormat*, unsigned short* max nits, Display const*) -> Result */
/* (hookshot) the Insert menu's HDR set to Native on an HDR10 swapchain: an HDR10 display (format 1, which makes the game present PQ
   codes in 10 bits) of the menu's peak; otherwise SDR, as a desktop window is */
HLE(_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE) {
    const int native = rx_option(HOS_OPT_HDR_NATIVE, 0);
    *(uint8_t *)GP(ARG(0)) = (uint8_t)native;
    if (ARG(1)) G32(ARG(1)) = native ? 1 : 0;
    if (ARG(2)) *(uint16_t *)GP(ARG(2)) = native ? (uint16_t)rx_option(HOS_OPT_HDR_PEAK, 1000) : 0;
    static int logged = -1;
    if (logged != native) logged = native, hos_log("vi: QueryDisplayHdrStatus -> %s", native ? "HDR10 (the game's native HDR)" : "SDR");
    RET(0);
}
/* nn::vi::QueryDisplayHdrPaperWhite(unsigned short*, Display const*) -> Result */
HLE(_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE) {
    *(uint16_t *)GP(ARG(0)) = rx_option(HOS_OPT_HDR_NATIVE, 0) ? (uint16_t)rx_option(HOS_OPT_HDR_WHITE, 203) : 203;
    RET(0);
}
/* nn::vi::QueryDisplayVrrSupported(bool*, Display const*) -> Result */
HLE(_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE) {
    *(uint8_t *)GP(ARG(0)) = 0;
    RET(0);
}
HLE(_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb) { RET(0); }
HLE(_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii) { RET(0); }
HLE(_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE) { RET(0); }

/* nn::vi::GetNativeWindow(void**, nn::vi::Layer*) -> Result: a window object (NVN builds its window on it; the
   native NVN layer only keeps the pointer) */
HLE(_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE) {
    static uint64_t window;
    if (!window) window = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = window;
    hos_log("vi: GetNativeWindow -> %#llx", (unsigned long long)window);
    RET(0);
}

/* ---- frame pacing for the native NVN layer's window (nvn_native.c) ----
   The game acquires a window texture before each frame and presents it after; on the console the acquire blocks
   while every texture waits for the display. Here the acquire waits for the frame's display time: the present
   interval's vsyncs (1: 60 fps, 2: 30) after the last present, so the game runs at the display's rate.
   HOS_NO_PACING=1: as fast as it can. */
static volatile int present_interval = 1;
/* (hookshot) The frame rate cap past the console's 60, the Insert menu's Game frame rate cap (frame_rate_cap=): the display pacing's
   interval, the game's own limiter (+0xfc) and the vsync event go up to it, and the game's time step is the frames' own time
   (realtime_step), so the game is not sped up. Uncapped is a cap of 1000 (fps_max's FPS_UNCAPPED), the pacing's 1 ms: the frames
   come as fast as the PC makes them (a display that waits for its refresh, V-Sync On, still holds them to its rate).
   HOS_FPS_MAX=<61-1000> goes over the row for a run, with any number (a 75 or 100 Hz screen) where the row has steps. Unset or 60:
   60, as on the console */
int hos_ini_option(const char *key, int otherwise);   /* (nn_applet.c) */
static int fps_max(void) {
    enum { FPS_UNCAPPED = 1000 };
    static int v = -1;
    if (v < 0) {
        /* (the Insert menu's Frame rate cap, frame_rate_cap=: 0 the console's 60, then 90, 120, 144, 165, 240, 6 Uncapped; a value
           this build does not know is 60. HOS_FPS_MAX over it) */
        static const int caps[] = {60, 90, 120, 144, 165, 240, FPS_UNCAPPED};
        enum { CAPS = sizeof caps / sizeof caps[0] };
        const int i = hos_ini_option("frame_rate_cap", 0);
        const char *e = getenv("HOS_FPS_MAX");
        const int want = e ? atoi(e) : caps[i >= 0 && i < CAPS ? i : 0];
        v = want > 60 ? (want > FPS_UNCAPPED ? FPS_UNCAPPED : want) : 60;
        if (v == FPS_UNCAPPED) hos_log("vi: frame rate uncapped: pacing, the game's limiter and its time step follow the frames");
        else if (v > 60) hos_log("vi: frame rate cap %d: pacing, the game's limiter and its time step follow it", v);
    }
    return v;
}
static int64_t next_frame_at, window_start, frames_in_window;
static volatile LONG64 presents;
int hos_vi_native_interval(Ctx *c, int requested) {
    (void)c;
    /* (hookshot) OoT's frame-rate controller asks for 2, 3 or 4 vsyncs a frame whenever one runs long (480 changes a minute
       in Kokiri): paced to them, frames snapped to 33, 50 and 67 ms, the stutter, and the frame rate sat far under what the
       PC managed. With game time following the clock (the Insert menu's Real time, realtime_step), a frame is shown as soon
       as it is ready, 60 at most; the game still reads back its own interval. HOS_GAME_INTERVAL=1, or Game speed Per frame:
       paced to the game's interval, as before */
    static int game_interval = -1;
    if (game_interval < 0) game_interval = getenv("HOS_GAME_INTERVAL") && atoi(getenv("HOS_GAME_INTERVAL")) != 0;
    int rx_option(int id, int otherwise);
    const int clamped = requested < 1 ? 1 : requested > 4 ? 4 : requested;
    present_interval = game_interval || !rx_option(0, 1) ? clamped : 1;
    return requested;
}
/* (hookshot) The window's textures as the console's display queue has them: each present is shown at its display
   time, the later of its present and one interval after the frame shown before it, and a texture is free again once
   the frame presented after it is shown. The acquire of a frame waits for its texture: with `active` textures, the
   texture of the frame `active` back, freed when the frame after that one is shown. With three (OoT) a frame slower
   than the display waits for nothing and a fast one is held to the display's rate, two frames queued at most;
   the acquire waited for one interval after the last present before (two textures' behaviour), which held every
   frame over 16.7 ms to the next vsync step (gameplay at 27 ms frames, 5 ms of them in this sleep). HOS_VI_DOUBLE=1:
   that behaviour */
static int64_t shown_at[16];
static volatile LONG64 shown_count;
void hos_vi_native_acquire(int active) {
    static int no_pacing = -1, double_buffered = -1;
    if (no_pacing < 0) no_pacing = env_on("HOS_NO_PACING");
    if (double_buffered < 0) double_buffered = env_on("HOS_VI_DOUBLE");
    if (no_pacing || !next_frame_at) return;
    int64_t until = next_frame_at;
    if (!double_buffered && active >= 2) {
        const int64_t n = shown_count, frame = n - active + 1;   /* (the frame whose showing frees this texture) */
        until = frame >= 0 && frame < n ? shown_at[frame & 15] : 0;
    }
    const int64_t now = hos_now_ns();
    if (until > now) {
        extern volatile int64_t hos_acquire_slept_ns;   /* (host/hos_thread.c: the FPS counter's Breakdown, its pacing) */
        extern volatile int hos_fps_timing;
        hos_sleep_ns(until - now);
        if (hos_fps_timing) InterlockedAdd64(&hos_acquire_slept_ns, hos_now_ns() - now);
    }
}
/* (mkw-recomp) MKW's frame pacer (0x806bcbb0, x19 when it sets the interval: nvn_native.c): its measured frame time
   (+0x128, ticks), its running excess (+0x218) and interval (+0x214), logged with the frame rate */
volatile uint64_t hos_mkw_pacer;
/* HOS_SCAN_DT=<file prefix> (diagnostic): writes every float in guest memory between ~0.015 and ~0.04, with its
   address, four times 300 presents apart: the game's frame step is either one exact 1/60 or follows the frames */
static void scan_dt(long long presents) {
    static long long first; static const char *prefix; static int scans, read_env;
    if (!read_env) read_env = 1, prefix = getenv("HOS_SCAN_DT");
    if (!prefix) return;
    if (!first) {   /* (once <prefix>go exists) */
        if (presents % 60) return;
        char go[512];
        snprintf(go, sizeof go, "%sgo", prefix);
        FILE *g = fopen(go, "r");
        if (!g) return;
        fclose(g);
        first = presents;
    }
    if (presents < first + scans * 300 || scans >= 4) return;
    char name[512];
    snprintf(name, sizeof name, "%s%d.txt", prefix, scans++);
    FILE *f = fopen(name, "w");
    if (!f) return;
    size_t hits = 0;
    for (uint64_t a = 0x80000000ULL; a < 0x2000000000ULL && hits < 4000000;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(GP(a), &m, sizeof m)) break;
        const uint64_t size = m.RegionSize;
        if (m.State == MEM_COMMIT && (m.Protect & PAGE_READWRITE)) {
            const uint32_t *w = (const uint32_t *)m.BaseAddress;
            for (size_t i = 0; i < size / 4; i++)
                if (w[i] >= 0x3c800000 && w[i] <= 0x3d230000) {
                    fprintf(f, "%llx %08x\n", (unsigned long long)((uint8_t *)&w[i] - (uint8_t *)GP(0)), w[i]);
                    if (++hits >= 4000000) break;
                }
        }
        a = (uint64_t)((uint8_t *)m.BaseAddress - (uint8_t *)GP(0)) + size;
    }
    fclose(f);
    hos_log("vi: scan %s: %zu floats", name, hits);
}
/* (hookshot) Game time follows the clock. The engine steps every frame by ratio = 60 / <the graphics module's nominal fps,
   an int at +0x2c8> (main+0x6e6cc4, f_83b6e300 makes seconds = ratio / 60 and the rest), so at 50 fps the game ran at 83%
   speed: walking the same 8 s at 52 and 33 fps went as far as those rates. Each present writes the rate of the last 5
   display intervals there, the rounding error carried to the next frame so that no time is lost; a step is at most
   1/15 s, as TotK Recomp's frame_rate.c (slower frames slow the game down, as on the console), at least 1/60 s (the game's
   own limit holds it at 60). HOS_REALTIME_STEP=0: the shipping whole steps. */
extern volatile uint64_t hos_gfx_module;
volatile int hos_fixed_resolution = 0;   /* (the lifted code reads these: nx2/oot_patches.py) */
volatile uint32_t hos_fixed_scale_bits = 0x3f800000;   /* (HOS_RES_SCALE: the fixed path's scale) */
volatile int hos_dynres_runs = 0;   /* (the dynamic path's frames: it fills fields the 3D reads, +8 +16 +168) */
volatile float hos_text_scale = 1.0f;   /* (the letters' clock of a text pane, f_80b1e140: nx2/oot_patches.py) */
/* The Insert menu's Text speed (0 Normal, 1 2x, 2 4x, 3 Instant), HOS_TEXT_SPEED=<0..3> over it: what the time of a letter
   animation's step is multiplied by. Instant: the page at once, as the game's own skip on A */
static float text_scale(const char *env, int menu) {
    const int v = env && *env ? atoi(env) : menu;
    return v == 1 ? 2.0f : v == 2 ? 4.0f : v >= 3 ? 1e6f : 1.0f;
}
volatile float hos_camera_scale = 1.0f;   /* (the right stick's camera turn speeds, f_80643f10 and three more: nx2/oot_patches.py) */
/* The Insert menu's Camera speed, in percent of the game's own turn speed (50 to 400; anything else, as without the Vulkan
   renderer's menu: the game's): what the turn speeds are multiplied by. Even the fastest of the game's own camera speeds turned
   slowly for players; it still picks the speed this multiplies */
static float camera_scale(int percent) {
    return (percent >= 50 && percent <= 400 ? percent : 100) / 100.0f;
}
volatile int hos_scene_rgba16f = 0;   /* (the scene buffer RGBA16F, not R11G11B10F, f_80672260: nx2/oot_patches.py; HDR set to
                                         Native with the PsychoV35 tone mapper: the temporal upscalers take only
                                         R11G11B10F, so Vanilla keeps it) */
volatile uint64_t hos_step_probe = 0;   /* (f_80706be0's object, set by the lifted code when the frame-time hook is in this build; its
                                            +0xd58 is the game clock in 1/60 s: nx2/oot_patches.py) */
volatile uint32_t hos_frame_seconds_bits = 0;   /* (the frame's seconds as a float's bits for that hook, 0: the game's own step) */
/* A movie's frame count while the frames' time is given (a Frame rate cap above 60): how many 60ths of a second this update of
   that movie player is worth, the rest carried to its next one, so the count the cutscene's cues are placed by keeps the clock
   as the movie's picture does (issue 1742: the narration ran ahead above 60 fps). 1, the game's own step, otherwise, and with
   HOS_FPS_MOVIE=0 (for comparisons). Called by the lifted f_83b7c710 (nx2/oot_patches.py).
   The step is more than 1 when a frame takes longer than a 60th (up to 4, realtime_step's longest), and the count then passes
   values over. The timeline does not need each one: it fires the events whose time lies between its last frame and the new one,
   walking a list sorted by time (f_8124cfc0, which the in-engine timelines run with a rate times the time step). Measured at 36
   fps under a cap of 120, steps of 1 and 2 all the way: every narration line and the rumble as at 60. A step held at 1 would
   lose that time instead, and the cues would come late, as they do at a cap of 60 under 60 fps. */
uint32_t hos_movie_frames(uint64_t player) {
    enum { PLACES = 16 };
    const uint32_t bits = __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED);   /* (realtime_step writes it at each present) */
    if (!bits) return 1;
    static SRWLOCK lock = SRWLOCK_INIT;
    static int timed = -1;   /* (these four under the lock) */
    static struct { uint64_t player, used; double part; } carry[PLACES];
    static uint64_t calls;
    static unsigned made;
    float secs;
    uint32_t n = 1;
    memcpy(&secs, &bits, 4);
    AcquireSRWLockExclusive(&lock);
    if (timed < 0) timed = !(getenv("HOS_FPS_MOVIE") && !atoi(getenv("HOS_FPS_MOVIE")));
    if (timed) {
        int i = 0;
        while (i < PLACES && carry[i].player != player) i++;
        if (i == PLACES) {
            /* (a player not seen yet takes the place used longest ago, so a movie that plays keeps its own while others come
               and go: its count is exact while no more than PLACES players, itself among them, update in turn. It starts at a part
               of a 60th drawn from a hash of how many places were made: with that many players at once every update is a first
               one, and each count then still grows by the frames' time on average, whatever the number of players, instead of
               standing still. A sequence with a fixed step, the golden ratio's multiples, stood still for one player in 233) */
            uint32_t x = made++;
            x ^= x >> 16, x *= 0x85ebca6bu, x ^= x >> 13, x *= 0xc2b2ae35u, x ^= x >> 16;   /* (MurmurHash3's last step) */
            i = 0;
            for (int k = 1; k < PLACES; k++)
                if (carry[k].used < carry[i].used) i = k;
            carry[i].player = player, carry[i].part = x / 4294967296.0;
        }
        carry[i].used = ++calls;
        carry[i].part += (double)secs * 60.0;
        n = (uint32_t)carry[i].part;
        carry[i].part -= n;
    }
    ReleaseSRWLockExclusive(&lock);
    return n;
}
/* (nx2/oot_patches.py patch_whole_frames) the whole frames a game counter at `at` takes for an amount of the frame step (its
   float's bits): with the frames' own time as the step, their fractions carried per counter; else cut as the game cuts it */
#include "frame_whole.h"
static FrameWhole frame_whole_state;
static SRWLOCK frame_whole_lock = SRWLOCK_INIT;   /* (the game's update threads) */
uint32_t hos_frame_whole(uint64_t at, uint32_t amount_bits) {
    AcquireSRWLockExclusive(&frame_whole_lock);
    const uint32_t whole = frame_whole(&frame_whole_state, at, amount_bits, __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0);
    ReleaseSRWLockExclusive(&frame_whole_lock);
    return whole;
}
volatile uint64_t hos_res_module = 0;   /* (f_831f3200's object, whose +0xe5c is the 3D's scale: nx2/oot_patches.py) */
/* Quantize the time step to the engine's integer nominal rate. Keep debt from a short frame even when the minimum
   step is 1/60: discarding it made alternating short/long frames advance more game time than elapsed wall time.
   Keep at most four nominal frames of debt so unpaced fast presents cannot leave a long recovery. Steps beyond
   1/15 s are still dropped, rather than caught up after a long stall. */
static int realtime_rate(double ratio, int maximum, double *carry) {
    ratio += *carry;
    if (ratio > 4.0) ratio = 4.0;
    const double least = 60.0 / maximum;
    const double bounded = ratio < least ? least : ratio;
    int fps = (int)(60.0 / bounded + 0.5);
    fps = fps < 15 ? 15 : fps > maximum ? maximum : fps;
    *carry = ratio - 60.0 / fps;
    if (*carry < -4.0) *carry = -4.0;
    return fps;
}
static void realtime_step(int64_t now) {
    int rx_option(int id, int otherwise);
    /* (Resolution 100% from the start: the fixed path skipped the dynamic block from the first frame, its fields stayed
       unset and the 3D came out black. The dynamic path runs a few frames first, as when the menu turns it on in game) */
    hos_fixed_resolution = !rx_option(2, 1) && hos_dynres_runs >= 30;
    {
        static const char *env;
        static int looked;
        if (!looked) looked = 1, env = getenv("HOS_TEXT_SPEED");
        hos_text_scale = text_scale(env, rx_option(5, 0));
    }
    hos_camera_scale = camera_scale(rx_option(HOS_OPT_CAMERA_SPEED, 100));
    hos_scene_rgba16f = rx_option(HOS_OPT_HDR_TONEMAP, 0) != 0;
    {   /* (the Insert menu's Resolution: the fixed path's scale in 1/10000, live; HOS_RES_SCALE
           below stays over it) */
        const int scale = rx_option(3, 0);
        if (scale > 0 && !getenv("HOS_RES_SCALE")) {
            const float v = scale / 10000.0f;
            memcpy((void *)&hos_fixed_scale_bits, &v, 4);
        }
    }
    {
        static int read_scale;
        if (!read_scale) {
            read_scale = 1;
            if (getenv("HOS_RES_SCALE")) { float v = (float)atof(getenv("HOS_RES_SCALE")); memcpy((void *)&hos_fixed_scale_bits, &v, 4); }
        }
    }
    {   /* HOS_RES_LOG=1: the 3D's scale every 300 presents. +0xe20: flags (bit 3: 0.75, bit 4: 0.5 in place of the scale),
           +0xe2c bit 3: recompute asked, +0xe50/+0xe54/+0xe58: the sizes the scale is taken against, +0xe5c: the scale */
        static int on = -1;
        static long long n;
        if (on < 0) on = env_on("HOS_RES_LOG");
        const uint64_t m = hos_res_module;
        if (on && m && !(n++ % 300)) {
            float f[4], s;
            memcpy(f, GP(m + 0xe50), sizeof f);
            memcpy(&s, (const void *)&hos_fixed_scale_bits, 4);
            hos_log("vi: res: fixed %d (scale %.3f), +0xe20 %llx, +0xe2c %x, +0xe50 %.0f %.0f %.0f, 3D scale %.3f", hos_fixed_resolution, s,
                    (unsigned long long)*(uint64_t *)GP(m + 0xe20), *(uint32_t *)GP(m + 0xe2c), f[0], f[1], f[2], f[3]);
        }
    }
    const uint64_t mod = hos_gfx_module;
    if (!mod) return;
    {   /* (hookshot) The frame limiter (f_8042cd20 sleeps to 1 / +0xfc). The engine's adaptive controller writes +0xfc when
           variable refresh is on (+0x231, f_804fc0f0), any rate from 40 up. HOS_FPS_TARGET=<fps>: that rate kept, 0: left alone;
           HOS_FPS_LOG=1: what the game asks for, every 300 presents */
        static int target = -1, log_on = -1;
        static long long n;
        if (target < 0) target = getenv("HOS_FPS_TARGET") ? atoi(getenv("HOS_FPS_TARGET")) : 0, log_on = env_on("HOS_FPS_LOG");
        int *limit = (int *)GP(mod + 0xfc);
        if (log_on && !(n++ % 300))
            hos_log("vi: frame limiter %d, vrr %d %d, nominal %d, controller +0x58.. %d %d %d %d", *limit, *(uint8_t *)GP(mod + 0x231),
                    *(uint8_t *)GP(mod + 0x232), *(int *)GP(mod + 0x2c8), *(int *)GP(mod + 0x270 + 0x58), *(int *)GP(mod + 0x270 + 0x28),
                    *(int *)GP(mod + 0x270 + 0x2c), *(int *)GP(mod + 0x270 + 0x30));
        if (log_on && !((n - 1) % 300) && hos_step_probe) {   /* (game speed: the game clock's 1/60 s units a second of wall time, 60.0 at speed 1) */
            static double clock_before;
            static int64_t at_before;
            double game_clock;
            memcpy(&game_clock, GP(hos_step_probe + 0xd58), 8);
            if (at_before && now > at_before) {
                const double rate = (game_clock - clock_before) * 1e9 / (double)(now - at_before);
                hos_log("vi: game clock %.1f: %.2f a second of wall time (speed %.3f) at %.1f fps; now_ns=%lld", game_clock, rate, rate / 60.0,
                        300e9 / (double)(now - at_before), (long long)now);
            }
            clock_before = game_clock, at_before = now;
        }
        if (target && *limit > 0 && *limit < target) *limit = target;
        if (fps_max() > 60 && *limit > 0 && *limit < fps_max()) *limit = fps_max();
    }
    /* (the Insert menu's Game speed "per frame": a frame is a 60th of a second of game time. Not past 60 fps: the game would run
       faster than real time, so a cap above 60 keeps the real-time step whatever that row says) */
    if (!rx_option(0, 1) && fps_max() <= 60) {
        int *slot = (int *)GP(mod + 0x2c8);
        if (*slot >= 15 && *slot <= 60) *slot = 60;
        return;
    }
    static int64_t last;
    static double samples[5], carry;
    static unsigned count, next;
    static long long steps;
    const double dt = (now - last) / 1e9;
    last = now;
    if (dt <= 0 || dt > 0.25) { count = next = 0; carry = 0; return; }   /* (a load: the average starts again) */
    /* (HOS_REALTIME_WINDOW=<1-5>: frames averaged for the step; 1: each step the last frame's own time) */
    static int window = -1;
    if (window < 0) {
        window = getenv("HOS_REALTIME_WINDOW") ? atoi(getenv("HOS_REALTIME_WINDOW")) : 5;
        window = window < 1 ? 1 : window > 5 ? 5 : window;
    }
    samples[next++ % window] = dt;
    if (count < (unsigned)window) count++;
    double sum = 0;
    for (unsigned i = 0; i < count; i++) sum += samples[i];
    /* (hookshot) A cap above 60: the engine's own rate is 60 / <whole display intervals>, and its controller puts 60 back over a
       higher +0x2c8 before the step is made from it, so every frame still stepped 1/60 s and the game ran fps / 60 times too fast.
       The step is then the frames' own time at the two places the engine makes it (nx2/oot_patches.py, hos_frame_seconds_bits):
       this same average, at most 1/15 s as below and at least 1/240 s (a 1000th uncapped: a shorter frame stepped a 240th ran the
       game fast), and the rate under it stays the engine's, 60 at most.
       HOS_FPS_STEP=0, or a lift without the hook (hos_step_probe unset): the rate up to the cap, as before */
    static int timed_step = -1;
    if (timed_step < 0) timed_step = !(getenv("HOS_FPS_STEP") && !atoi(getenv("HOS_FPS_STEP")));
    const int timed = fps_max() > 60 && timed_step && hos_step_probe;
    if (timed) {
        const double secs = sum / count, shortest = 1.0 / (fps_max() > 240 ? fps_max() : 240);
        const float f = (float)(secs > 4.0 / 60.0 ? 4.0 / 60.0 : secs < shortest ? shortest : secs);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        __atomic_store_n(&hos_frame_seconds_bits, bits, __ATOMIC_RELAXED);   /* (read by the game's threads) */
    } else {
        __atomic_store_n(&hos_frame_seconds_bits, 0, __ATOMIC_RELAXED);
    }
    const int top = timed ? 60 : fps_max();
    const double ratio = sum / count * 60.0;
    int *slot = (int *)GP(mod + 0x2c8);
    if (*slot < 15 || *slot > fps_max()) {
        static int logged;
        if (!logged++) hos_log("vi: the graphics module's frame rate reads %d: no real-time step", *slot);
        return;
    }
    /* The timed hook already supplies the actual frame time. Its nominal rate tracks this frame without debt. */
    double frame_carry = timed ? 0 : carry;
    const int fps = realtime_rate(ratio, top, &frame_carry);
    carry = timed ? 0 : frame_carry;
    *slot = fps;
    if (!(steps++ % 3000)) hos_log("vi: real-time step on: ratio %.3f, game's frame rate %d", ratio, fps);
}
/* HOS_PRESENT_TRACE=<csv>: native game present-call returns, not GPU completion or display times.
   Uses the existing frame ID/clock; a forced stop can lose the last second of buffered present records. */
int hos_vi_present_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *path = getenv("HOS_PRESENT_TRACE");
        enabled = path && *path;
    }
    return enabled;
}
static void present_trace(int64_t frame, int64_t now, const int64_t phases[4]) {
    static FILE *file;
    static char buffer[64 * 1024];
    static int looked;
    static int64_t last, flushed;
    if (!looked) {
        const char *path = getenv("HOS_PRESENT_TRACE");
        looked = 1;
        if (path && *path) {
            file = fopen(path, "w");
            if (!file) hos_log("vi: could not open present trace %s", path);
        }
        if (file) {
            setvbuf(file, buffer, _IOFBF, sizeof buffer);
            fputs("frame,return_ns,interval_ns,present_begin_ns,renderer_begin_ns,renderer_end_ns,vcore_back_ns\n", file);
            flushed = now;
        }
    }
    if (!file) return;
    fprintf(file, "%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", (long long)frame, (long long)now,
            (long long)(frame > 1 ? now - last : 0), (long long)phases[0], (long long)phases[1],
            (long long)phases[2], (long long)phases[3]);
    last = now;
    if (now - flushed >= 1000000000LL) {
        fflush(file);
        flushed = now;
    }
}
void hos_vi_native_presented(const int64_t phases[4]) {
    static long long scan_n;
    scan_dt(++scan_n);
    {   /* HOS_FRAME_SLEEP_MS (diagnostic): slower frames, for game speed against frame rate */
        static int ms = -1;
        if (ms < 0) ms = getenv("HOS_FRAME_SLEEP_MS") ? atoi(getenv("HOS_FRAME_SLEEP_MS")) : 0;
        if (ms) hos_sleep_ns(ms * 1000000LL);
    }
    const int64_t period = 1000000000LL / fps_max() * present_interval, now = hos_now_ns();
    realtime_step(now);
    /* (behind by more than a frame: the next one is paced from now, with no burst to catch up) */
    next_frame_at = next_frame_at && now - next_frame_at < period ? next_frame_at + period : now + period;
    {   /* (the display queue: this frame is shown one interval after the last, or now when the display waits) */
        const int64_t n = shown_count, last = n ? shown_at[(n - 1) & 15] : 0;
        shown_at[n & 15] = last + period > now ? last + period : now;
        shown_count = n + 1;
    }
    const int64_t n = InterlockedIncrement64(&presents);
    present_trace(n, now, phases);
    {   /* HOS_MKW_INTERVAL=<0|1|2>: the interval MKW's pacer is held at, through its own override (+0x210, any
           non-zero value is used as the interval): 1 (the default): a steady 60 fps; 2: 30; 0: the game's choice,
           which with frames between 16.7 and 33 ms swings between the two (25-27 fps, stutters). Its timing counts
           vsyncs, so an interval the renderer cannot keep slows the game down rather than dropping frames */
        static int forced = -1;
        if (forced < 0) forced = getenv("HOS_MKW_INTERVAL") ? atoi(getenv("HOS_MKW_INTERVAL")) : 1;
        if (hos_mkw_pacer && forced >= 0 && forced <= 2) G32(hos_mkw_pacer + 0x210) = (uint32_t)forced;
    }
    if (!window_start) window_start = now;
    frames_in_window++;
    /* (present-to-present intervals of the window, in buckets: which frames miss their vsync) */
    static int64_t last_present;
    static int hist[6];
    if (last_present) {
        const int64_t d = now - last_present;
        hist[d < 15500000 ? 0 : d < 17800000 ? 1 : d < 22000000 ? 2 : d < 30000000 ? 3 : d < 40000000 ? 4 : 5]++;
    }
    last_present = now;
    if (now - window_start >= 5000000000LL) {
        hos_log("vi: %lld frames presented (%.1f fps over the last %.1f s, interval %d)", (long long)n,
                frames_in_window * 1e9 / (double)(now - window_start), (now - window_start) / 1e9, present_interval);
        hos_log("vi: present intervals: <15.5 ms %d, 15.5-17.8 %d, 17.8-22 %d, 22-30 %d, 30-40 %d, over 40 %d", hist[0],
                hist[1], hist[2], hist[3], hist[4], hist[5]);
        memset(hist, 0, sizeof hist);
        window_start = now, frames_in_window = 0;
        const uint64_t pacer = hos_mkw_pacer, freq = pacer ? G64(0x837de940) : 0;   /* (MKW only: nvn_native.c) */
        if (pacer && freq)
            hos_log("vi: the game's pacer: frame work %.1f ms, excess %.1f ms, interval %d",
                    (double)(int64_t)G64(pacer + 0x128) * 1e3 / (double)freq,
                    (double)(int64_t)G64(pacer + 0x218) * 1e3 / (double)freq, (int)G32(pacer + 0x214)),
            hos_log("vi: pacer raw: now %llx f0 %llx 128 %llx e8 %llx 50 %x 210 %x 22c %x freq %llu", (unsigned long long)hos_ticks(),
                    (unsigned long long)G64(pacer + 0xf0), (unsigned long long)G64(pacer + 0x128),
                    (unsigned long long)G64(pacer + 0xe8), G32(pacer + 0x50), G32(pacer + 0x210), *(const uint8_t *)GP(pacer + 0x22c),
                    (unsigned long long)freq);
    }
}
