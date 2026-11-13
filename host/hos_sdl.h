/* SDL3 or SDL2 behind one set of calls (host/hos_sdl.c): the window (host/posix/display_sdl.c), the sound
   (host/posix/win32_posix.c's waveOut) and the controllers (host/pad_sdl.c) call these, whichever SDL loaded. One
   library for the whole process, loaded at run time: SDL3 beside the executable, else the system's, else SDL2 the same
   way (SDL3.dll and SDL2.dll; on Linux libSDL3.so.0 and libSDL2-2.0.so.0). HOS_SDL_VERSION=2 takes SDL2 even with an
   SDL3 there. */
#pragma once
#include <stdint.h>

int hos_sdl_load(void);   /* 3 or 2, the SDL loaded (the first call loads it); 0 when there is none */

enum { HOS_SDL_AUDIO = 0x10, HOS_SDL_VIDEO = 0x20, HOS_SDL_GAMEPAD = 0x2000, HOS_SDL_EVENTS = 0x4000 };   /* (both's) */
int hos_sdl_init(uint32_t subsystems);   /* 1 when started */
void hos_sdl_hint(const char *name, const char *value);   /* (a hint the loaded SDL does not know is nothing) */
int hos_sdl_hint_override(const char *name, const char *value);   /* (over the environment's: SDL_HINT_OVERRIDE) */
const char *hos_sdl_error(void);
/* the app's name and id, before the window and the sound start: Wayland's app_id and X11's WM_CLASS (what the desktop
   matches the window to its menu entry by, for its icon and name), the name the sound mixer shows */
void hos_sdl_identity(const char *name, const char *app_id);

/* ---- the window: centred, resizable, at the display's pixel density, for Vulkan ---- */
void *hos_sdl_window(const char *title, int width, int height);
void hos_sdl_fullscreen(void *window, int on);   /* (the desktop's mode, borderless) */
void hos_sdl_relative_mouse(void *window, int on);
int hos_sdl_desktop_size(int *width, int *height);
int hos_sdl_refresh_mhz(void *window);   /* the refresh rate of the window's display in mHz, 0 when not known (the main thread) */
int hos_sdl_vulkan_surface(void *window, void *instance, void *surface_out);   /* (VkInstance, VkSurfaceKHR *): 1 if made */
void hos_sdl_pixel_size(void *window, int *width, int *height);
const uint8_t *hos_sdl_keys(int *count);   /* the keyboard by scancode (USB HID usage), 1 a key down */
uint32_t hos_sdl_mouse_buttons(void);   /* 1 left, 2 middle, 4 right, 8 and 16 the side buttons */
void hos_sdl_mouse_position(float *x, float *y);   /* main thread, window-coordinate units */
void hos_sdl_window_size(void *window, int *width, int *height);   /* (in the units of the mouse's position, not pixels) */
void hos_sdl_icon(void *window, const char *png);   /* (SDL 3.4's PNG loader: nothing on SDL2 or an older SDL3) */

/* ---- events, as the window's loop wants them ---- */
enum {
    HOS_SDL_EV_OTHER, HOS_SDL_EV_QUIT, HOS_SDL_EV_CLOSE, HOS_SDL_EV_RESIZED, HOS_SDL_EV_FOCUS_GAINED,
    HOS_SDL_EV_FOCUS_LOST, HOS_SDL_EV_MOUSE_LEAVE, HOS_SDL_EV_KEY_DOWN, HOS_SDL_EV_MOUSE_DOWN, HOS_SDL_EV_MOUSE_MOTION,
    HOS_SDL_EV_MOUSE_UP, HOS_SDL_EV_MOUSE_WHEEL, HOS_SDL_EV_MOUSE_ENTER
};
typedef struct {
    int type;
    int scancode, repeat;   /* (key down) */
    uint16_t mod;           /* (key down: 0x300 an Alt key) */
    float dx, dy;           /* (mouse motion, relative) */
    float x, y;             /* (mouse motion, down and up: the cursor in the window, hos_sdl_window_size's units) */
    int button;             /* (mouse down and up: 1 left, 2 middle, 3 right, as SDL numbers them) */
    float wheel;            /* (mouse wheel: notches, up or away from the player positive) */
} HosSdlEvent;
int hos_sdl_wait_event(HosSdlEvent *e, int timeout_ms);   /* 1 when there was one */
int hos_sdl_poll_event(HosSdlEvent *e);

/* ---- sound: the default output, pull() asked for len more bytes as it runs low (s16: 16-bit samples, else float).
   NULL when it does not open; hos_sdl_audio_close closes what it opened ---- */
void *hos_sdl_audio_open(int freq, int channels, int s16, void (*pull)(void *user, uint8_t *out, int len), void *user);
void hos_sdl_audio_close(void *out);
int hos_sdl_audio_channels(void);   /* the default output's channels (6 for 5.1); 2 when SDL cannot say */

/* ---- (hookshot) microphones: SDL3's recording devices or SDL2's capture devices, for the game's input streams
   (host/nn_audio_in.c). The device's sound comes out converted to the format asked for (SDL resamples and mixes the
   channels down) ---- */
int hos_sdl_mic_list(uint32_t *ids, char (*names)[128], int max);   /* the recording devices now: their count */
void *hos_sdl_mic_open(uint32_t id, int freq, int channels, uint16_t sdl_format);   /* id 0: the system's default; NULL
                                                                                    (nothing logged): hos_sdl_error */
int hos_sdl_mic_available(void *mic);                    /* bytes ready */
int hos_sdl_mic_read(void *mic, void *dst, int bytes);   /* bytes read */
void hos_sdl_mic_clear(void *mic);
void hos_sdl_mic_close(void *mic);

/* ---- controllers (the button and axis numbers are both's: SDL3's SOUTH, EAST, WEST, NORTH are SDL2's A, B, X, Y) ---- */
int hos_sdl_pads_list(int32_t *ids, int *opens, int max);   /* the ids of those SDL knows, and what opens each */
void *hos_sdl_pad_open(int what);
void hos_sdl_pad_close(void *pad);
int hos_sdl_pad_connected(void *pad);
void hos_sdl_pads_update(void);
int hos_sdl_pad_button(void *pad, int button);
int16_t hos_sdl_pad_axis(void *pad, int axis);
const char *hos_sdl_pad_name(void *pad);
/* Connection diagnostics only: hardware GUID and active mapping name, never a serial number or device path. */
void hos_sdl_pad_log_connection(void *pad, int motion, int steam);
int32_t hos_sdl_pad_id(void *pad);
void hos_sdl_pad_rumble(void *pad, uint16_t low, uint16_t high, uint32_t ms);
/* (HD rumble, host/pad_sdl.c) the pad's SDL_GetGamepadType / SDL_GameControllerGetType, in the loaded SDL's numbers (0
   unknown, or no such call); whether the SDL has the effect call; a raw effect sent (SDL_SendGamepadEffect /
   SDL_GameControllerSendEffect): 1 when SDL took it */
int hos_sdl_pad_type(void *pad);
int hos_sdl_pad_has_effect(void);
int hos_sdl_pad_effect(void *pad, const void *data, int size);
/* motion sensors (SDL_SENSOR_ACCEL m/s^2, SDL_SENSOR_GYRO rad/s, three values each): 0 when the pad or the SDL has none */
enum { HOS_SDL_SENSOR_ACCEL = 1, HOS_SDL_SENSOR_GYRO = 2 };
int hos_sdl_pad_has_sensor(void *pad, int type);
int hos_sdl_pad_sensor_enable(void *pad, int type, int on);   /* 1 when done */
int hos_sdl_pad_sensor(void *pad, int type, float *data, int n);   /* 1 when read */
/* Steam's stand-in pads (host/steam_pads.h): whether the SDL tells them apart (vendor, Steam handle and hint priority
   calls); a pad's USB vendor (0 unknown); its Steam Input handle (0 for a real pad) */
int hos_sdl_knows_steam_pads(void);
uint16_t hos_sdl_pad_vendor(void *pad);
uint64_t hos_sdl_pad_steam_handle(void *pad);
