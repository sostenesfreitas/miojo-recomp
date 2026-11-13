/* Which renderer the rx_* calls go to (gpu/vk/vk_api.h): the WebGPU one in the executable (gpu/render, its
   functions renamed wgr_* by rx_wgpu_names.h) or, unset or RX_BACKEND=vulkan, the Vulkan one in hookshot_vk.dll
   (hookshot_vk.so) beside the executable. Decided at rx_start, before any other rx_* call; a DLL that is missing or finds no usable Vulkan
   device leaves the WebGPU renderer in place (logged). */
#include "hos.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../gpu/vk/vk_api.h"
#include "../gpu/vk/dlss_math.h"
#include "control.h"

/* the WebGPU renderer (gpu/render, renamed) */
int wgr_start(void *hwnd);
void wgr_resize(int width, int height);
const char *wgr_describe(void);
void wgr_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines);
void wgr_nvn_submit(const uint64_t *handles, int n);
uint64_t wgr_nvn_fence(void);
uint64_t wgr_nvn_pushed(void);
uint64_t wgr_nvn_replayed(void);
int wgr_nvn_wait(uint64_t fence, uint64_t timeout_ns);
void wgr_wait_writebacks_reading(const void *p, uint64_t n);
void wgr_nvn_finish(void);
void wgr_nvn_present(const nvn_tex *t, const int crop[4]);
void wgr_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v);
void wgr_nvn_register_sampler(int id, const nvn_samp_par *p);
void wgr_nvn_set_texture_pool_size(int n);
void wgr_nvn_texture_released(const nvn_tex *t);
void wgr_nvn_program_released(const nvn_prog *p);
void wgr_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                         uint64_t row, uint64_t img);
void wgr_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                          const uint8_t *src, uint64_t row, uint64_t img);
void wgr_nvn_invalidate(uint64_t gpu, uint64_t size);

/* what the Vulkan back end is given */
uint8_t *hos_guest_base(void);
uint64_t hos_guest_size(void);
uint8_t *hos_gpu_ptr(uint64_t va, uint64_t *avail);
uint64_t nvn_gpu_guest(uint64_t gpu);
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n);
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n);
uint32_t *rx_collect_written(int64_t *count);   /* gpu/render/rx_core.cpp: the soft write watch's pages */
void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);
int nvn_depth_minus_w(void);
int hos_gpu_pools(uint64_t (*out)[2], int max);
uint32_t hos_gpu_maps_generation(void);
uint32_t nvn_vmap_generation(void);
uint64_t hos_reserved_cpus(void);   /* hos_thread.c */
extern float hos_aspect_ratio;      /* hos_main.c: the guest camera aspect applied at startup, or 0 */
uint32_t hos_pad_raw(void);         /* nn_hid.c: the gamepads and the controls, for the Insert menu */
int hos_binding(int set, int button, int slot);
int hos_mic_count(void);            /* nn_audio_in.c: the microphones, for the Insert menu */
const char *hos_mic_name(int index);
void hos_mic_select(int mode, const char *name, int index);
void hos_bind(int set, int button, int slot, int code);
int hos_prompts_face(void);         /* prompts.c */
int hos_prompts_live_parts(void);   /* prompts.c */
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
void hos_mark_written(const void *p, size_t n);
#endif

static void host_log(const char *line) { hos_log("%s", line); }
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
int hos_gpu_guest_ranges(uint64_t (*out)[2], int max);
/* The soft write watch's marks (rt/a64.h A64_MARK, hos_mark_written) over the GPU-visible ranges, read and cleared:
   the Vulkan back end's own collection (gpu/render's rx_collect_written works only with the WebGPU renderer started).
   One consumer: with Vulkan the WebGPU renderer never runs. */
static int64_t collect_in(const uint64_t (*ranges)[2], int nranges, uint32_t **pages) {
    size_t n = 0, cap = 4096;
    uint32_t *out = malloc(cap * sizeof *out);
    for (int r = 0; r < nranges; r++) {
        const uint64_t lo = ranges[r][0], hi = ranges[r][1] < hos_guest_size() ? ranges[r][1] : hos_guest_size();
        uint64_t p1 = (hi + 4095) >> 12;
        if (p1 > A64_DIRTY_PAGES) p1 = A64_DIRTY_PAGES;
        for (uint64_t p = lo >> 12; p < p1;) {
            if (!(p & 31) && p + 32 <= p1) {   /* (32 pages at a time while none is marked) */
                uint64_t w[4];
                memcpy(w, &a64_dirty[p], 32);
                if (!(w[0] | w[1] | w[2] | w[3])) {
                    p += 32;
                    continue;
                }
            }
            if (!(p & 7) && p + 8 <= p1) {   /* (eight pages a load while none is marked) */
                uint64_t w;
                memcpy(&w, &a64_dirty[p], 8);
                if (!w) {
                    p += 8;
                    continue;
                }
            }
            if (__atomic_load_n(&a64_dirty[p], __ATOMIC_RELAXED) && __atomic_exchange_n(&a64_dirty[p], 0, __ATOMIC_ACQUIRE)) {
                if (n == cap) out = realloc(out, (cap *= 2) * sizeof *out);
                out[n++] = (uint32_t)p;
            }
            p++;
        }
    }
    *pages = out;
    return (int64_t)n;
}
static int64_t host_collect(uint32_t **pages) {
    static uint64_t (*ranges)[2];
    if (!ranges) ranges = malloc(sizeof *ranges * 4096);
    return collect_in(ranges, hos_gpu_guest_ranges(ranges, 4096), pages);
}
/* (only within the given guest ranges: the submit-time capture of the mirrored pools, a small part of the 20 ms the
   whole collection takes) */
static int64_t host_collect_in(const uint64_t (*ranges)[2], int n, uint32_t **pages) { return collect_in(ranges, n, pages); }
#else
static int64_t host_collect(uint32_t **pages) {
    int64_t n = -1;
    *pages = rx_collect_written(&n);
    return n;
}
static int64_t host_collect_in(const uint64_t (*ranges)[2], int n, uint32_t **pages) { (void)ranges, (void)n; *pages = 0; return 0; }
#endif
static void host_free_pages(uint32_t *pages) { free(pages); }
static void host_mark_written(const void *p, uint64_t n) {
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
    hos_mark_written(p, (size_t)n);
#else
    (void)p, (void)n;
#endif
}

/* (the renderer's own guest memory: the address space above the heap, as the game's own regions, committed and
   zeroed; the region allocator never hands it to the game) */
static uint64_t host_guest_alloc(uint64_t size) {
    const uint64_t a = hos_region_alloc(size, 0x10000);
    if (a) memset(hos_guest_base() + a, 0, size);
    return a;
}

void hos_watch_guest_writes(const uint8_t *data, uint64_t size);   /* nvn_native.c */
static void host_watch_guest_writes(uint64_t guest, uint64_t size) { hos_watch_guest_writes(hos_guest_base() + guest, size); }
static float host_aspect_ratio(void) { return hos_aspect_ratio; }
#ifndef _WIN32
int hos_sdl_desktop_size(int *width, int *height);   /* hos_sdl.c */
#endif
/* (hookshot) the main screen's size, as host/hos_main.c takes its shape (MkwVkHost.screen_size) */
static int host_screen_size(int *width, int *height) {
#ifdef _WIN32
    const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return 0;
    *width = w, *height = h;
    return 1;
#else
    return hos_sdl_desktop_size(width, height);
#endif
}
static float host_wide_menus(uint32_t keep_mask) {   /* (MkwVkHost.wide_menus) */
    extern volatile uint32_t hos_wide_keep_mask;
    extern volatile float hos_wide_margin;
    static const char *forced;
    static int looked;
    if (!looked) looked = 1, forced = getenv("HOS_WIDE_KEEP");
    hos_wide_keep_mask = forced ? (uint32_t)strtoul(forced, NULL, 16) : keep_mask;
    return hos_wide_margin;
}
int hos_mouse_pointer(MkwVkPointer *out);   /* display.c, posix/display_sdl.c */
int hos_sdl_gyro_status(void);   /* pad_sdl.c */
extern const char *hos_romfs_dir;   /* nn_fs.c, set by hos_main.c */
static const char *host_romfs_dir(void) { return hos_romfs_dir; }

#ifdef _WIN32
static unsigned char menu_key_press[256];
void hos_vk_note_key_press(unsigned key) {
    if (key < sizeof menu_key_press) __atomic_store_n(&menu_key_press[key], 1, __ATOMIC_RELAXED);
}
int hos_vk_take_key_press(int key) {
    return (unsigned)key < sizeof menu_key_press &&
           __atomic_exchange_n(&menu_key_press[key], 0, __ATOMIC_RELAXED);
}
#define host_take_key_press hos_vk_take_key_press
#else
#define host_take_key_press NULL
#endif

/* (hookshot) the FPS counter's Breakdown: this thread's sleeps so far (host/hos_thread.c) */
extern _Thread_local int64_t hos_slept_ns[2];
extern volatile int64_t hos_poll_slept_ns, hos_acquire_slept_ns;
extern volatile int hos_fps_timing;
static void host_fps_timing(int on) { __atomic_store_n(&hos_fps_timing, on != 0, __ATOMIC_RELAXED); }
static void host_slept_ns(int64_t out[2]) {
    out[0] = __atomic_load_n(&hos_poll_slept_ns, __ATOMIC_RELAXED);
    out[1] = hos_slept_ns[1] + __atomic_load_n(&hos_acquire_slept_ns, __ATOMIC_RELAXED);
}

static const MkwVkHost vk_host = {
    MKWVK_API_VERSION, host_log, hos_guest_base, hos_guest_size, hos_gpu_ptr, nvn_gpu_guest, nvn_gpu_read,
    nvn_gpu_write, host_collect, host_free_pages, host_mark_written, hos_ticks, hos_now_ns, nvn_tex_layout,
    nvn_tex_level_offset, nvn_fmt_bpb, nvn_tex_level_tiles, nvn_tex_linear_stride, nvn_depth_minus_w, hos_gpu_pools,
    hos_gpu_maps_generation, nvn_vmap_generation, hos_reserved_cpus, host_collect_in, host_guest_alloc,
    host_watch_guest_writes, hos_pad_raw, hos_binding, hos_bind, host_aspect_ratio, host_take_key_press,
    hos_mouse_pointer, hos_mic_count, hos_mic_name, hos_mic_select, hos_sdl_gyro_status, hos_log_flush, hos_prompts_face,
    host_romfs_dir, host_wide_menus, host_slept_ns, host_fps_timing, hos_prompts_live_parts, host_screen_size,
};

static const MkwVkRenderer *vk;   /* the Vulkan back end, when it was chosen and started */
static HMODULE vk_module;   /* (the last back end loaded: rx_close_asked) */

/* The game registers its first textures and samplers before the window's thread starts the renderer (NVN is up
   first): those calls are kept here and given to the renderer rx_start chose, in order. Without that the Vulkan back
   end never knew the first ~0x60 texture slots, and lit geometry sampled them as its black dummy.
   The texels the game writes then (WriteTexels) are kept too, with a copy of their data: without a renderer they went
   nowhere (Linux has no WebGPU renderer to take them), and a texture filled so at boot kept zeros in its storage. OoT's
   4x4 white placeholder is one: bound in place of the ground fog's depth, it read 0 and hid the ground fog at some camera
   angles. That happened when the window's thread started the renderer after the game had begun (windowed, not headless). */
typedef struct {
    int kind;   /* 0 texture registration, 1 sampler registration, 2 texture pool size, 3 texture finalized, 4 texture
                   made, 5 texels written */
    int id;
    nvn_tex t;
    nvn_view v;
    int has_view;
    nvn_samp_par p;
    /* (5: WriteTexels) */
    unsigned level, layer0;
    NVNcopyRegion r;
    uint64_t row, img;
    uint8_t *data;
} early_call;
static early_call *early;
static size_t n_early, cap_early;
static volatile LONG started;   /* rx_start chose (the calls go straight to the renderer from then on) */
static SRWLOCK early_lock = SRWLOCK_INIT;

static void forward(const early_call *c);
/* Keeps a call made before rx_start; 0 when the renderer is chosen already (the caller forwards it) */
static int keep_early(const early_call *c) {
    if (started) return 0;
    AcquireSRWLockExclusive(&early_lock);
    if (started) {
        ReleaseSRWLockExclusive(&early_lock);
        return 0;
    }
    if (n_early == cap_early) {
        cap_early = cap_early ? cap_early * 2 : 1024;
        early = (early_call *)realloc(early, cap_early * sizeof *early);
    }
    early[n_early++] = *c;
    ReleaseSRWLockExclusive(&early_lock);
    return 1;
}
/* rx_start: the kept calls given to the renderer chosen, then the gate opened */
static void flush_early(void) {
    AcquireSRWLockExclusive(&early_lock);
    size_t texels = 0;
    for (size_t i = 0; i < n_early; i++) {
        forward(&early[i]);
        if (early[i].kind == 5) texels++, free(early[i].data);
    }
    if (n_early)
        hos_log("rx: %zu texture and sampler calls made before the renderer started (%zu texel writes), given to it", n_early,
                texels);
    free(early);
    early = NULL, n_early = cap_early = 0;
    InterlockedExchange(&started, 1);
    ReleaseSRWLockExclusive(&early_lock);
}

/* dll: hookshot_vk.dll (hookshot_vk.so on Linux), the Vulkan back end: one export */
static const MkwVkRenderer *load_vulkan(const char *dll) {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n ? (strrchr(path, '\\') ? strrchr(path, '\\') : strrchr(path, '/')) : NULL;   /* (linux: '/') */
    if (!slash || (size_t)(slash + 1 - path) + strlen(dll) >= sizeof path) return NULL;   /* (a path near MAX_PATH) */
    memcpy(slash + 1, dll, strlen(dll) + 1);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        hos_log("rx: RX_BACKEND=vulkan, but %s did not load (%lu): WebGPU", path, GetLastError());
        return NULL;
    }
    vk_module = m;
    mkwvk_init_fn init = (mkwvk_init_fn)(void *)GetProcAddress(m, "mkwvk_init");
    if (!init) {
        hos_log("rx: %s has no mkwvk_init: WebGPU", path);
        return NULL;
    }
    const MkwVkRenderer *r = init(&vk_host);
    if (!r) {
        hos_log("rx: mkwvk_init returned NULL: WebGPU");
        return NULL;
    }
    if (r->version != MKWVK_API_VERSION) {
        hos_log("rx: mkwvk_init version mismatch (%u vs %u): WebGPU", r->version, MKWVK_API_VERSION);
        return NULL;
    }
    return r;
}

int rx_start(void *hwnd) {
    const char *b = getenv("RX_BACKEND");
    if (!b || !_stricmp(b, "vulkan")) {   /* (hookshot: Vulkan unless told otherwise; a started game has no RX_BACKEND) */
        const char *dll = "hookshot_vk.dll";
        const MkwVkRenderer *r = load_vulkan(dll);
        if (r && r->start(hwnd)) {
            vk = r;
            hos_log("rx: renderer: Vulkan (%s)", dll);
            flush_early();
            return 1;
        }
        if (r) hos_log("rx: the Vulkan back end could not start on the window: WebGPU");
    }
    const int ok = wgr_start(hwnd);
    flush_early();
    return ok;
}
/* (hookshot) hookshot --prepare-shaders: the Vulkan back end started with no window and no game, which prepares
   every shader in its start (gpu/vk/vk_main.cpp started_up) and is then let go. 0 when there is no usable Vulkan */
int rx_prepare_shaders(void) {
    const MkwVkRenderer *r = load_vulkan("hookshot_vk.dll");
    return r && r->start(NULL);
}
/* (hookshot) the window is being closed: a back end still preparing its shaders stops and keeps what it made first (a few
   seconds at most). One without the export (an older one) has nothing to keep */
void rx_close_asked(void) {
    if (!vk_module) return;
    void (*close_asked)(void) = (void (*)(void))(void *)GetProcAddress(vk_module, "mkwvk_close_asked");
    if (close_asked) close_asked();
}
void rx_resize(int width, int height) { vk ? vk->resize(width, height) : wgr_resize(width, height); }
const char *rx_describe(void) { return vk ? vk->describe() : wgr_describe(); }
void rx_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) {
    vk ? vk->stats(draws, skipped, pipelines) : wgr_stats(draws, skipped, pipelines);
}
void rx_nvn_submit(const uint64_t *handles, int n) { vk ? vk->nvn_submit(handles, n) : wgr_nvn_submit(handles, n); }
void rx_nvn_submit_q(const uint64_t *handles, int n, uint64_t queue) {
    if (vk && vk->nvn_submit_q) vk->nvn_submit_q(handles, n, queue);
    else rx_nvn_submit(handles, n);
}
uint64_t rx_nvn_fence(void) { return vk ? vk->nvn_fence() : wgr_nvn_fence(); }
uint64_t rx_nvn_fence_q(uint64_t queue) { return vk && vk->nvn_fence_q ? vk->nvn_fence_q(queue) : rx_nvn_fence(); }
uint64_t rx_nvn_pushed(void) { return vk ? vk->nvn_pushed() : wgr_nvn_pushed(); }
uint64_t rx_nvn_replayed(void) { return vk ? vk->nvn_replayed() : wgr_nvn_replayed(); }
int rx_nvn_wait(uint64_t fence, uint64_t timeout_ns) {
    return vk ? vk->nvn_wait(fence, timeout_ns) : wgr_nvn_wait(fence, timeout_ns);
}
void rx_wait_writebacks_reading(const void *p, uint64_t n) {
    vk ? vk->wait_writebacks_reading(p, n) : wgr_wait_writebacks_reading(p, n);
}
void rx_nvn_finish(void) { vk ? vk->nvn_finish() : wgr_nvn_finish(); }
static volatile LONG presents;   /* (the frames shown, for the camera jitter below) */
void rx_nvn_present(const nvn_tex *t, const int crop[4]) {
    InterlockedIncrement(&presents);
    vk ? vk->nvn_present(t, crop) : wgr_nvn_present(t, crop);
}
/* (hookshot, nx2/oot_jitter.py) the temporal upscaler's camera jitter (NVIDIA DLSS, AMD FSR 4: gpu/vk/vk_temporal.cpp): this frame's sub-pixel offset of the scene's projection, in clip
   space (what P[0][2] and P[1][2] get), into out, for the projection matrix at guest address projection (row major, as the
   game fills its views' uniform buffers from it); 0 when there is none: DLSS and FSR off (or a renderer without them: rx_option
   HOS_OPT_DLSS_JITTER_W and _H, the scene's size in pixels, are 0), or a projection that is not one of the picture's
   (gpu/vk/dlss_math.h dlss_jitter_clip: a shadow map's, the HUD's). The renderer reads the offset back from
   the camera's uniform buffer (gpu/vk/vk_temporal.cpp), so this only has to pick sub-pixel offsets that cover a pixel evenly:
   the Halton (2, 3) sequence, over as many frames as the upscaler's scale wants (HOS_OPT_DLSS_JITTER_PHASES: gpu/vk/dlss_math.h
   dlss_jitter_phases, 8 for DLAA up to 64). */
int hos_dlss_jitter_clip(uint64_t projection, float out[2]) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static LONG at = -1;
    static MkwVkJitter jitter;
    MkwVkJitter sample;
    AcquireSRWLockExclusive(&lock);
    const LONG frame = presents;
    if (frame != at) {   /* (asked once a frame: the renderer's lock is not taken for every view) */
        at = frame;
        jitter = (MkwVkJitter){0};
        if (vk && vk->jitter_sample) vk->jitter_sample(&jitter);
    }
    sample = jitter;
    ReleaseSRWLockExclusive(&lock);
    float p[16];
    memcpy(p, GP(projection), sizeof p);
    extern volatile float hos_wide_ratio;
    return dlss_jitter_clip(p, hos_wide_ratio, (unsigned)frame, (int)sample.phases, (float)sample.width, (float)sample.height, out);
}
static void forward(const early_call *c) {
    switch (c->kind) {
    case 0: {
        const nvn_view *v = c->has_view ? &c->v : NULL;
        vk ? vk->nvn_register_texture(c->id, &c->t, v) : wgr_nvn_register_texture(c->id, &c->t, v);
        break;
    }
    case 1: vk ? vk->nvn_register_sampler(c->id, &c->p) : wgr_nvn_register_sampler(c->id, &c->p); break;
    case 2: vk ? vk->nvn_set_texture_pool_size(c->id) : wgr_nvn_set_texture_pool_size(c->id); break;
    case 3: vk ? vk->nvn_texture_released(&c->t) : wgr_nvn_texture_released(&c->t); break;
    case 4: if (vk && vk->nvn_texture_created) vk->nvn_texture_created(&c->t); break;
    case 5:
        vk ? vk->nvn_write_texels(&c->t, c->level, c->layer0, &c->r, c->data, c->row, c->img)
           : wgr_nvn_write_texels(&c->t, c->level, c->layer0, &c->r, c->data, c->row, c->img);
        break;
    }
}
void rx_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) {
    if (!started && t) {
        early_call c = {0, id};
        c.t = *t;
        if (v) c.v = *v, c.has_view = 1;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_register_texture(id, t, v) : wgr_nvn_register_texture(id, t, v);
}
void rx_nvn_register_sampler(int id, const nvn_samp_par *p) {
    if (!started && p) {
        early_call c = {1, id};
        c.p = *p;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_register_sampler(id, p) : wgr_nvn_register_sampler(id, p);
}
void rx_nvn_set_texture_pool_size(int n) {
    if (!started) {
        early_call c = {2, n};
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_set_texture_pool_size(n) : wgr_nvn_set_texture_pool_size(n);
}
void rx_nvn_texture_released(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {3, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_texture_released(t) : wgr_nvn_texture_released(t);
}
void rx_nvn_texture_created(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {4, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    if (vk && vk->nvn_texture_created) vk->nvn_texture_created(t);
}
void rx_nvn_program_released(const nvn_prog *p) { vk ? vk->nvn_program_released(p) : wgr_nvn_program_released(p); }
void rx_nvn_program_created(const nvn_prog *p) {
    if (vk && vk->nvn_program_created) vk->nvn_program_created(p);
}
void rx_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                        size_t row, size_t img) {
    vk ? vk->nvn_read_texels(t, level, layer0, r, dst, row, img) : wgr_nvn_read_texels(t, level, layer0, r, dst, row, img);
}
void rx_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                         size_t row, size_t img) {
    if (!started && t && r && src) {   /* (kept with a copy of the data: the caller's buffer is its own again on return) */
        int bw = 1, bh = 1;
        const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (bpb > 0 && bw > 0 && bh > 0 && r->width > 0 && r->height > 0) {
            const uint64_t wb = (uint64_t)((r->width + bw - 1) / bw) * (uint64_t)bpb, hb = (uint64_t)((r->height + bh - 1) / bh);
            const uint64_t depth = r->depth > 0 ? (uint64_t)r->depth : 1;
            /* (the bytes the copy reads: the last row of the last slice ends the data, whatever the strides) */
            const uint64_t n = (depth - 1) * img + (hb - 1) * row + wb;
            early_call c = {5, 0};
            c.t = *t, c.level = level, c.layer0 = layer0, c.r = *r, c.row = row, c.img = img;
            c.data = (uint8_t *)malloc(n);
            if (c.data) {
                memcpy(c.data, src, n);
                if (keep_early(&c)) return;
                free(c.data);
            }
        }
    }
    vk ? vk->nvn_write_texels(t, level, layer0, r, src, row, img)
       : wgr_nvn_write_texels(t, level, layer0, r, src, row, img);
}
void rx_nvn_invalidate(uint64_t gpu, uint64_t size) {
    /* The callback can wait for replay or GPU write-back: let another guest thread use this core meanwhile. */
    const int away = hos_vcore_away();
    vk ? vk->nvn_invalidate(gpu, size) : wgr_nvn_invalidate(gpu, size);
    if (away) hos_vcore_back();
}
int rx_guest_imported(uint64_t lo, uint64_t hi) {
    /* (mkw-recomp 16bc3bc: it takes the back end's pool lock, which the replay holds at times: no virtual core held) */
    const int away = hos_vcore_away();
    const int r = vk && vk->guest_imported ? vk->guest_imported(lo, hi) : 0;
    if (away) hos_vcore_back();
    return r;
}
/* (nvn_native.c: the CPU set an event's value) in order with the submits, for the GPU's waits on it */
int rx_option(int id, int otherwise) { return vk && vk->option ? vk->option(id) : otherwise; }
void rx_option_set(int id, int value) { if (vk && vk->option_set) vk->option_set(id, value); }
/* (hookshot) the control bridge (host/control.c): the Vulkan back end's, none with WebGPU (0) */
int rx_control_frame_info(int64_t *presents, int64_t *draws, int *width, int *height) {
    if (!vk || !vk->frame_info) return 0;
    vk->frame_info(presents, draws, width, height);
    return 1;
}
int rx_control_shot(const char *path, int64_t after, void (*done)(void *ctx, int64_t present, int ok), void *ctx) {
    return vk && vk->shot ? vk->shot(path, after, done, ctx) : 0;
}
#define CTL_SAME(f) (sizeof ((HosControlSetting *)0)->f == sizeof ((MkwVkSetting *)0)->f)
_Static_assert(CTL_SAME(key) && CTL_SAME(name) && CTL_SAME(text) && CTL_SAME(tab), "control.h and vk_api.h: one setting");
static void control_setting_copy(HosControlSetting *to, const MkwVkSetting *from) {   /* (the same fields, field by field) */
    memcpy(to->key, from->key, sizeof to->key), memcpy(to->name, from->name, sizeof to->name);
    memcpy(to->text, from->text, sizeof to->text), memcpy(to->tab, from->tab, sizeof to->tab);
    to->value = from->value, to->lo = from->lo, to->hi = from->hi, to->step = from->step;
    to->restart = from->restart, to->fixed = from->fixed;
}
int rx_control_setting(int index, HosControlSetting *out) {
    MkwVkSetting s;
    if (!vk || !vk->setting || !vk->setting(index, &s)) return 0;
    control_setting_copy(out, &s);
    return 1;
}
int rx_control_setting_set(const char *key, int value, HosControlSetting *out) {
    MkwVkSetting s;
    memset(&s, 0, sizeof s);
    const int r = vk && vk->setting_set ? vk->setting_set(key, value, &s) : 0;
    if (r) control_setting_copy(out, &s);
    return r;
}
void rx_nvn_event_signal(uint64_t gpu, uint32_t value) {
    if (vk && vk->nvn_event_signal) vk->nvn_event_signal(gpu, value);
}
/* (nvn_native.c: virtual pages about to be unmapped or remapped) the Vulkan back end writes what the GPU drew there into
   their memory first */
void rx_nvn_vmap_changing(uint64_t gpu, uint64_t size) {
    /* (mkw-recomp 16bc3bc: it may wait for the replay and the GPU's texels: no virtual core held meanwhile) */
    const int away = hos_vcore_away();
    if (vk && vk->nvn_vmap_changing) vk->nvn_vmap_changing(gpu, size);
    if (away) hos_vcore_back();
}
void rx_nvn_pool_created(uint64_t gpu, uint64_t size) {
    if (vk && vk->nvn_pool_created) vk->nvn_pool_created(gpu, size);
}
