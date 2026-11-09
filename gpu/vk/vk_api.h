/* The Vulkan back end (mkw_vk.dll) and the game executable: what each gives the other.

   The executable records the game's NVN command buffers (host/nvn_native.c) and calls a renderer through the rx_*
   functions (gpu/render/rx_nvn.h). With RX_BACKEND=vulkan host/rx_backend.c loads mkw_vk.dll, hands it the host
   functions below and routes the rx_* calls to the table it returns; otherwise they go to the WebGPU renderer in
   the executable. The DLL carries Eden's shader recompiler (gpu/vk/eden, SPIR-V back end) and sirit: their C++
   namespaces are the same as those of the WebGPU renderer's suyu copy, so the two live in different binaries. */
#pragma once

#include <stdint.h>

#include "../render/rx_nvn.h"   /* (the NVN objects and command records: C) */

#ifdef __cplusplus
extern "C" {
#endif

#define MKWVK_API_VERSION 14
enum { HOS_OPT_CHEAT_HEARTS = 32, HOS_OPT_CHEAT_FLIGHT, HOS_OPT_CHEAT_ACTION, HOS_OPT_CHEAT_CAMERA, HOS_OPT_CHEAT_SPEED, HOS_OPT_CHEAT_AMMO, HOS_OPT_CHEAT_TIMELOCK, HOS_OPT_CHEAT_MOONJUMP };
/* (hookshot) the Gameplay tab's Text size (host/oot_text_size.c): 0 the game's, 1 110%, 2 125%, 3 150%, 4 175% */
enum { HOS_OPT_TEXT_SIZE = 10 };
/* (hookshot) the Motion page's rows (host/pad_sdl.c reads them, vk_replay.cpp's option answers them): Motion controls on
   or off, the horizontal's and the vertical's sensitivity in percent, each turned around, and the deadzone (0 to 2) */
enum { HOS_OPT_GYRO = 18, HOS_OPT_GYRO_SENS_X, HOS_OPT_GYRO_INV_X, HOS_OPT_GYRO_INV_Y, HOS_OPT_GYRO_SENS_Y, HOS_OPT_GYRO_DEADZONE };
/* (hookshot) NVIDIA DLSS's camera jitter (host/rx_backend.c, gpu/vk/vk_dlss.cpp): the scene's width and height while DLSS
   draws, 0 when there is to be no jitter, and the jitter sequence's length. 24 on: 18 to 23 are the motion rows' */
enum { HOS_OPT_DLSS_JITTER_W = 24, HOS_OPT_DLSS_JITTER_H, HOS_OPT_DLSS_JITTER_PHASES };
/* (hookshot) the Audio tab's rows (host/nn_audio.c reads them every 100 ms): the volume in percent, and no sound while
   another window has the focus */
enum { HOS_OPT_VOLUME = 27, HOS_OPT_MUTE_BACKGROUND };
/* (hookshot) the Insert menu's HDR set to Native (host/nn_vi.c QueryDisplayHdrStatus and QueryDisplayHdrPaperWhite): whether
   the game is told its display is HDR10 (the swapchain is one), and the peak and paper white it is told, in nits */
/* (HOS_OPT_DISCORD, 43: the Insert menu's Discord status row, host/discord_rpc.c: 1 on) */
enum { HOS_OPT_HDR_NATIVE = 40, HOS_OPT_HDR_PEAK, HOS_OPT_HDR_WHITE, HOS_OPT_DISCORD };
/* (HOS_OPT_DISABLE_CAMERA_CENTERING, 45: Controls' movement-follow yaw, host/mouse_native.c: 0 stock, 1 disabled;
   44 is reserved for HOS_OPT_SKIP_TITLE) */
enum { HOS_OPT_DISABLE_CAMERA_CENTERING = 45 };

/* (hookshot) the controls' names (host/nn_hid.c, host/pad_sdl.c, the pad_ and key_ lines of hookshot_options.ini):
   the Switch's buttons in nn::hid's bit order, and a gamepad's inputs by position, SDL's order (the bottom face
   button first: A on an Xbox pad, cross on a DualSense) */
enum { HOS_SW_A, HOS_SW_B, HOS_SW_X, HOS_SW_Y, HOS_SW_LS, HOS_SW_RS, HOS_SW_L, HOS_SW_R, HOS_SW_ZL, HOS_SW_ZR,
       HOS_SW_PLUS, HOS_SW_MINUS, HOS_SW_LEFT, HOS_SW_UP, HOS_SW_RIGHT, HOS_SW_DOWN, HOS_SW_N };
enum { HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_WEST, HOS_PAD_NORTH, HOS_PAD_BACK, HOS_PAD_GUIDE, HOS_PAD_START,
       HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER, HOS_PAD_RSHOULDER, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT,
       HOS_PAD_RIGHT, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_N };
/* (hookshot) a DualShock 4's or DualSense's touchpad click, which opens the Insert menu as F1 does (host/pad_sdl.c,
   gpu/vk/vk_menu.cpp): its own bit past HOS_PAD_N, so it is not one of the inputs the Controls page binds */
enum { HOS_PAD_TOUCHPAD = 31 };
enum { HOS_BIND_PAD, HOS_BIND_KEY };
/* (hookshot) the controller faces of the button prompts (host/prompts.c): the Switch, then nx2/prompt_faces.py FACES' order;
   the Button prompts row's value is one more (0 Auto) */
enum { HOS_PROMPT_SWITCH, HOS_PROMPT_XBOX, HOS_PROMPT_PS4, HOS_PROMPT_PS5, HOS_PROMPT_STEAMDECK, HOS_PROMPT_N };
/* keys only (HOS_BIND_KEY), after the Switch's buttons: the left stick's and the camera's directions */
enum { HOS_KEY_MOVE_UP = HOS_SW_N, HOS_KEY_MOVE_DOWN, HOS_KEY_MOVE_LEFT, HOS_KEY_MOVE_RIGHT, HOS_KEY_CAM_UP,
       HOS_KEY_CAM_DOWN, HOS_KEY_CAM_LEFT, HOS_KEY_CAM_RIGHT, HOS_KEY_N };

/* (hookshot) the mouse over the window while it is free (not captured): the Insert menu's (host/display.c,
   host/posix/display_sdl.c). x and y: the cursor in the window's pixels, of a window width x height pixels (the
   swapchain's, as a rule); held: the buttons down now, pressed: those pressed since the last read (1 left, 2 right,
   4 middle); wheel: the wheel's notches since then, away from the player positive */
enum { HOS_MOUSE_LEFT = 1, HOS_MOUSE_RIGHT = 2, HOS_MOUSE_MIDDLE = 4 };
typedef struct MkwVkPointer {
    int32_t x, y, width, height;
    uint32_t held, pressed;
    int32_t wheel;
} MkwVkPointer;

/* what the executable gives the back end */
typedef struct MkwVkHost {
    uint32_t version;   /* MKWVK_API_VERSION */
    void (*log)(const char *line);
    /* guest memory: the window, and GPU addresses (pools, below 1 TB the guest address itself) */
    uint8_t *(*guest_base)(void);
    uint64_t (*guest_size)(void);
    uint8_t *(*gpu_ptr)(uint64_t gpu, uint64_t *avail);
    uint64_t (*gpu_guest)(uint64_t gpu);
    int (*gpu_read)(uint64_t gpu, void *dst, uint64_t n);
    int (*gpu_write)(uint64_t gpu, const void *src, uint64_t n);
    /* guest pages written since the last call (the soft write watch): count, page numbers in *pages (freed with
       free_pages) */
    int64_t (*collect_written)(uint32_t **pages);
    void (*free_pages)(uint32_t *pages);
    void (*mark_written)(const void *p, uint64_t n);
    /* clocks: the guest tick (31.25 MHz from the start), host ns */
    uint64_t (*ticks)(void);
    int64_t (*now_ns)(void);
    /* NVN texture layouts (nvn_tex.c) */
    void (*tex_layout)(const nvn_texdesc *d, nvn_texlayout *o);
    uint64_t (*tex_level_offset)(const nvn_texdesc *d, unsigned level);
    int (*fmt_bpb)(int format, int *bw, int *bh);
    void (*tex_level_tiles)(const nvn_texdesc *d, unsigned level, unsigned *tile_h, unsigned *tile_d);
    uint32_t (*tex_linear_stride)(const nvn_texdesc *d);
    int (*depth_minus_w)(void);
    /* the memory pools ({guest start, size} each; returns the count, at most max) and a number that changes with
       them (host/nvdrv.c) */
    int (*gpu_pools)(uint64_t (*out)[2], int max);
    uint32_t (*maps_generation)(void);
    /* a number that changes with every mapping of NVN's virtual pools' pages (MapVirtual: sparse textures) */
    uint32_t (*vmap_generation)(void);
    /* the logical processors kept from the game's threads, for the replay thread (0: none) */
    uint64_t (*reserved_cpus)(void);
    /// The soft write watch's marks within [ranges[i][0], ranges[i][1]) only, read and cleared (free with free_pages)
    int64_t (*collect_written_in)(const uint64_t (*ranges)[2], int n, uint32_t **pages);
    /// Zeroed guest memory of the renderer's own (address space above the heap, never freed; 0 when none is left):
    /// what the NVN driver keeps in GPU memory of its own, such as its constant banks (mkw_eden.dll)
    uint64_t (*guest_alloc)(uint64_t size);
    /// (debugging) CPU writes to [guest, guest + size) logged with the writer's guest call stack (a page watch)
    void (*watch_guest_writes)(uint64_t guest, uint64_t size);
    /* (hookshot) the gamepads' inputs held now (1 << HOS_PAD_*), whatever they are bound to: the Insert menu's
       (host/nn_hid.c) */
    uint32_t (*pad_raw)(void);
    /* (hookshot) the controls, for the Insert menu's Controls page. button: the Switch's, HOS_SW_*; set HOS_BIND_PAD:
       the gamepad input that presses it (a HOS_PAD_* position, -1 none; slot 0 only); set HOS_BIND_KEY: the keys that
       do (Windows key codes, 0 none; slots 0 and 1; a mouse button's too, and HOS_KEY_* past the Switch's buttons).
       bind with button -1: all back to their defaults */
    int (*binding)(int set, int button, int slot);
    void (*bind)(int set, int button, int slot, int code);
    /// Display aspect of the presented picture, after crop: the camera aspect applied at startup.
    /// Zero means no override; the renderer then fits the cropped texture's pixel aspect.
    float (*aspect_ratio)(void);
    int (*take_key_press)(int key);
    /* (hookshot) the free mouse (MkwVkPointer), read and its presses and wheel cleared: 1 when the cursor is over the
       window and the window has the focus. The Insert menu releases the mouse while it is open: WM_APP + 8 to the
       window, wParam 1 open, 0 closed (the capture it had then comes back) */
    int (*pointer)(MkwVkPointer *out);
    /* (hookshot) the microphones for the game's input streams (host/nn_audio_in.c), for the Insert menu's Microphone row:
       how many there are (listing them again), one's name, and the row's choice: mode 0 off, 1 the system's default, 2 the
       microphone of that name (index: its place in the list, -1 while it is not plugged in) */
    int (*mic_count)(void);
    const char *(*mic_name)(int index);
    void (*mic_select)(int mode, const char *name, int index);
    /* (hookshot) the controllers' motion sensors, for the Insert menu's Motion controls row (host/pad_sdl.c): 0 no
       controller, 1 controllers with no gyroscope, 2 one with a gyroscope */
    int (*gyro_status)(void);
    /* (hookshot) the log lines queued so far written out now (host/hos_thread.c log_queue): before the back end ends
       the process itself (replay_fatal), so a device lost's report is not lost with it */
    void (*log_flush)(void);
    /* (hookshot) the controller face of the button prompts the game was given at its start (host/prompts.c): HOS_PROMPT_*;
       -1 none (the game's own icons), or not chosen yet */
    int (*prompt_face)(void);
    /* (hookshot) the game's romfs folder (--romfs, or romfs beside the executable): the overlays read the game's own menu font
       from it (gpu/vk/vk_imgui.cpp), in memory only */
    const char *(*romfs_dir)(void);
    /* (hookshot) ultrawide's menus (nx2/oot_wide.py KEEP_VTABLES): keep_mask sets which of those screens keep the whole picture
       (a bit each; HOS_WIDE_KEEP=<hex> over it); returns the margin of the middle 16:9 at either side, a fraction of the
       picture's width (0: the picture is 16:9) */
    float (*wide_menus)(uint32_t keep_mask);
} MkwVkHost;

/* One completed temporal-upscale sample; all zero while inactive. */
typedef struct MkwVkJitter {
    uint32_t width, height, phases;
} MkwVkJitter;

/* what the back end gives the executable: the rx_* functions (gpu/render/rx_nvn.h, rx_core) */
typedef struct MkwVkRenderer {
    uint32_t version;
    int (*start)(void *hwnd);
    void (*resize)(int width, int height);
    const char *(*describe)(void);
    void (*stats)(int64_t *draws, int64_t *skipped, int64_t *pipelines);
    void (*nvn_submit)(const uint64_t *handles, int n);
    uint64_t (*nvn_fence)(void);
    uint64_t (*nvn_pushed)(void);
    uint64_t (*nvn_replayed)(void);
    int (*nvn_wait)(uint64_t fence, uint64_t timeout_ns);
    void (*wait_writebacks_reading)(const void *p, uint64_t n);
    void (*nvn_finish)(void);
    void (*nvn_present)(const nvn_tex *t, const int crop[4]);
    void (*nvn_register_texture)(int id, const nvn_tex *t, const nvn_view *v);
    void (*nvn_register_sampler)(int id, const nvn_samp_par *p);
    void (*nvn_set_texture_pool_size)(int n);
    void (*nvn_texture_released)(const nvn_tex *t);
    void (*nvn_program_released)(const nvn_prog *p);
    void (*nvn_read_texels)(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                            uint64_t row_pitch, uint64_t image_pitch);
    void (*nvn_write_texels)(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                             const uint8_t *src, uint64_t row_pitch, uint64_t image_pitch);
    void (*nvn_invalidate)(uint64_t gpu, uint64_t size);
    void (*nvn_pool_created)(uint64_t gpu, uint64_t size);   /* a memory pool made (non-virtual): imported early */
    /* whether the GPU may use guest memory in [lo, hi) through an import (the host keeps such pages committed) */
    int (*guest_imported)(uint64_t lo, uint64_t hi);
    /* virtual pages [gpu, gpu + size) mapped now are about to be unmapped or remapped (the old mapping still in place):
       what the GPU wrote into textures there goes into their memory first */
    void (*nvn_vmap_changing)(uint64_t gpu, uint64_t size);
    /* a texture initialized (its storage: whether the CPU's reads of a range may need a texture's texels) */
    void (*nvn_texture_created)(const nvn_tex *t);
    /* the CPU set an event's value: in order with the submits (the GPU's waits on it) */
    void (*nvn_event_signal)(uint64_t gpu, uint32_t value);
    int (*option)(int id);   /* (hookshot, the Insert menu) 0: game time follows the clock, 1: the menu is open, 2: dynamic resolution, 3: its fixed scale in percent, 4: the controllers count behind other windows, 5: text speed (0 normal to 3 instant), 6: native mouse look (host/mouse_native.c),
                                7: the mouse's sensitivity in percent, 8: its up and down swapped (host/nn_hid.c),
                                18 to 23: the Motion page (HOS_OPT_GYRO*: motion controls from a pad's gyroscope, host/pad_sdl.c;
                                the horizontal and vertical sensitivity in percent, each turned around, the deadzone),
                                27: the volume in percent, 28: no sound while another window has the focus (host/nn_audio.c) */
    /* a submit with its queue */
    void (*nvn_submit_q)(const uint64_t *handles, int n, uint64_t queue);
    /* (hookshot) a fence of one queue: the value its own last submission signals (nvnQueueFenceSync) */
    uint64_t (*nvn_fence_q)(uint64_t queue);
    /* (hookshot, the Insert menu) a setting changed from the game's side, kept in the options file: 7 the mouse's sensitivity
       in percent (the - and = keys, host/nn_hid.c) */
    void (*option_set)(int id, int value);
    /* (version 14) one coherent camera-jitter sample, without per-axis queries */
    void (*jitter_sample)(MkwVkJitter *out);
} MkwVkRenderer;

/* the DLL's one export: null when Vulkan is not usable (the executable then falls back to WebGPU) */
typedef const MkwVkRenderer *(*mkwvk_init_fn)(const MkwVkHost *host);
#ifdef MKWVK_BUILDING
#ifdef _WIN32
__declspec(dllexport) const MkwVkRenderer *mkwvk_init(const MkwVkHost *host);
#else
__attribute__((visibility("default"))) const MkwVkRenderer *mkwvk_init(const MkwVkHost *host);
#endif
#endif

#ifndef _WIN32
/* (linux) what start() is given in place of a window handle (host/posix/display_sdl.c): the window makes the
   surface (SDL_Vulkan_CreateSurface: X11 or Wayland) and says its size in pixels */
#define MKWVK_WINDOW_VERSION 2
typedef struct MkwVkWindow {
    uint32_t version;   /* MKWVK_WINDOW_VERSION */
    int (*create_surface)(void *window, void *instance, void *surface_out);   /* (VkInstance, VkSurfaceKHR *): 1 if made */
    void (*drawable_size)(void *window, int *width, int *height);
    void *window;
    /* (version 2) the refresh rate of the display the window is on, in mHz; 0 when not known (frame generation's Auto) */
    int (*refresh_mhz)(void *window);
} MkwVkWindow;
#endif

#ifdef __cplusplus
}
#endif
