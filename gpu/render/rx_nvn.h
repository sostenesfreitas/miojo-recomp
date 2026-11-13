/* The native NVN layer: TOTK's graphics API implemented directly (HOS_NVN_NATIVE=1), with no Switch GPU in between.
   hos/nvn_native.c is the API: objects live in place in the memory the game gives NVN objects, state-only and GPU work
   is recorded as commands in host memory, and a submit hands the recordings to gpu/render/rx_nvn.cpp, which replays
   them with WebGPU. This header is shared by both (C and C++).

   Design and TOTK specifics follow the MIT-licensed NVN layer of d2r-mobile / MacNativeGames (jhaizhou), which runs
   TOTK v1.0.0 on Metal: object layouts, command records, deferred texture binds, recording copies, the driver constant
   buffers' layout.

   Addresses: a GPU address is a guest address for ordinary memory pools (identity map). A virtual pool (NVN sparse
   memory) gets GPU addresses above 4 GB, which no guest pointer can reach, and a page table (64 KiB pages) to the
   physical pools' guest addresses; nvn_gpu_* resolve both. Host pointers in the objects below are only read by us. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVN_MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

/* ---- texture storage layout (Tegra X1 block linear; hos/nvn_layout.c, from deko3d) ---- */
typedef struct {
    int target, format, flags, samples;   /* NVNtextureTarget / NVNformat / NVNtextureFlags / samples (0 or 1: one) */
    uint32_t width, height, depth, levels, stride;
    signed int pkg_block_h : 8;           /* packaged layout byte 0 (log2 block height) + 1; 0: the automatic choice */
    unsigned int swizzle_r : 3, swizzle_g : 3, swizzle_b : 3, swizzle_a : 3, swizzle_pad : 12;
} nvn_texdesc;
typedef struct {
    uint64_t size, layer_size;            /* storage bytes, bytes per layer */
    uint32_t align, layers, bpb;          /* alignment, layers, bytes per block */
    unsigned tileH, tileD;                /* block height / depth (log2 GOBs) */
} nvn_texlayout;
void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);   /* bytes per block (0: unknown format), block width / height */
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);
/* LINEAR_RENDER_TARGET (0x40) is pitch storage even without LINEAR (0x10) */
static inline int nvn_tex_is_linear(const nvn_texdesc *d) { return (d->flags & 0x50) != 0; }
uint32_t nvn_tex_linear_stride(const nvn_texdesc *d);

/* ---- objects (in place in the game's NVN object memory; each fits the reserved size of its type) ---- */
#define NVN_FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " is larger than " #H)

typedef struct { uint32_t flags; } nvn_dev_builder;
typedef struct { uint32_t magic, flags, origin, depth_mode; } nvn_dev;
typedef struct { void *dev; uint32_t flags; uint64_t cmd_sz, comp_sz, ctl_sz, flush; uint64_t mem, mem_sz; } nvn_queue_builder;
typedef struct { uint32_t magic; uint64_t submits; } nvn_queue;

/* a memory pool: mem = guest address of its storage (0: a virtual pool), gpu = its GPU address. (hookshot) The object
   is guest memory, so it holds no host pointers: a virtual pool's page table is found by its GPU address. */
typedef struct { void *dev; uint64_t mem; uint64_t size; uint32_t flags; } nvn_pool_builder;   /* mem: guest address */
typedef struct {
    uint32_t magic, flags;
    uint64_t mem;                         /* guest address of the storage (0 for a virtual pool) */
    uint64_t size, gpu;                   /* GPU address: the guest address, or a virtual pool's range above 4 GB */
    uint64_t reserved;
} nvn_pool;
typedef struct { const nvn_pool *pool; int64_t off; int32_t n; } nvn_dpool;   /* texture / sampler descriptor pool */

typedef struct { void *dev; nvn_pool *pool; int64_t off; uint64_t size; } nvn_buffer_builder;
typedef struct { uint32_t magic; nvn_pool *pool; int64_t off; uint64_t size; } nvn_buffer;

typedef struct {
    void *dev; nvn_texdesc d; nvn_pool *pool; int64_t off;
    uint8_t has_layout, layout[8];
    uint64_t pkg_data;                    /* SetPackagedTextureData (guest address): copied into the storage at Initialize */
} nvn_tex_builder;
typedef struct {
    uint32_t magic; nvn_texdesc d; uint8_t has_layout, layout[8];
    nvn_texlayout L;
    uint64_t gpu;                         /* GPU address of the storage (pool gpu + offset) */
    const nvn_pool *pool; int64_t off;
    uint64_t serial;                      /* one per initialization: the game copies texture objects by value */
} nvn_tex;
/* TOTK reserves 16 bytes for a view on its stack (main+0x823398): packed */
typedef struct {
    uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, pad0 : 3;
    uint8_t base_level, nlevels, target; uint16_t min_layer, nlayers; uint16_t format; uint8_t swz[4]; uint8_t pad1[2];
} nvn_view;
_Static_assert(sizeof(nvn_view) == 16, "NVNtextureView is 16 bytes in TOTK's NVN");

typedef struct {
    uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction;
    float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap;
} nvn_samp_par;
typedef struct { void *dev; nvn_samp_par p; } nvn_samp_builder;
typedef struct { uint32_t magic; nvn_samp_par p; } nvn_samp;

/* a program: up to 6 stages, each the shader's code address (GPU) and its control section (guest address) */
typedef struct { uint64_t data, control; } nvn_stage_data;
typedef struct { uint32_t magic, nstage; nvn_stage_data sd[6]; uint64_t serial; } nvn_prog;

typedef struct { uint32_t magic, fenced; uint64_t fence; } nvn_sync;
typedef struct { const nvn_pool *pool; int64_t off; } nvn_event_builder;
typedef struct { uint32_t magic; uint64_t gpu; } nvn_event;   /* gpu: the 32-bit value's address */

typedef struct { void *dev; uint64_t native; int32_t ntex, interval, active; uint64_t tex[4]; } nvn_win_builder;   /* tex: guest addresses */
typedef struct {
    uint32_t magic; int32_t ntex, interval, next, active, crop[4];
    uint64_t tex[4];                      /* (hookshot) guest addresses, checked at each use */
    uint64_t presents;
} nvn_win;

/* a command buffer: records go to a growable host buffer (never into the game's command memory) */
/* (hookshot) The object is guest memory: the recording buffer's pointers (seg, cur, end) are kept in a host table
   (host/nvn_native.c cmd_rec), and the object holds only its slot there, which names the object it belongs to. */
typedef struct {
    uint32_t magic, recording;
    uint32_t slot, slot_pad;              /* the host table's slot (0: none yet) */
    uint64_t guest_flags;                 /* +0x10: NVN 55 inline code writes a dirty flag here */
    uint64_t reserved_18;
    /* NVN 55 headers inline fast paths that read the write pointer at +0x20 and its end at +0x28 (main+0x70644c) and
       emit raw GPU methods when 0x30 bytes fit: ~0 / 0 never fits, so every call takes the API (*Deferred) path */
    uint64_t inline_cur, inline_end;
    uint64_t cb, cb_data;                 /* the memory callback (guest), never called: records live in host memory */
    uint64_t reserved_40;
    uint64_t ctl, ctl_size;               /* control memory the game gave (guest): a handle lives as long as it */
    uint64_t game_size;                   /* command memory the game gave (accounting only) */
    uint64_t cmd_used, ctl_used, rec_start;
    uint64_t hw_used, last_rec, hw_start; /* what the real driver would have used (the game sizes heaps from it) */
} nvn_cmd;
_Static_assert(offsetof(nvn_cmd, guest_flags) == 0x10, "NVN inline dirty flag");
_Static_assert(offsetof(nvn_cmd, inline_cur) == 0x20 && offsetof(nvn_cmd, inline_end) == 0x28, "NVN inline fast path");

/* state objects (the game copies them by value): packed in place */
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } nvn_color_st;
typedef struct { uint32_t mask; } nvn_chmask_st;   /* 4 bits a target: r 1 g 2 b 4 a 8 (set: written) */
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } nvn_blend_st;
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } nvn_ms_st;
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } nvn_poly_st;
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} nvn_ds_st;
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } nvn_va_st;
typedef struct { uint32_t stride, divisor; } nvn_vs_st;

/* ---- command records: {op, size (header included, 8-aligned)} + payload ---- */
typedef struct { uint32_t op, size; } nvn_op_hdr;
enum {
    NOP_END, NOP_JUMP, NOP_BEGIN, NOP_CALL,
    NOP_SIGNAL_EVENT, NOP_REPORT_COUNTER, NOP_COPY_B2B, NOP_UPDATE_UB,   /* CPU-visible side effects */
    NOP_SET_RT, NOP_CLEAR_COLOR, NOP_CLEAR_DS, NOP_VIEWPORTS, NOP_SCISSOR, NOP_DEPTH_RANGE,
    NOP_BIND_PROGRAM, NOP_BIND_STATE, NOP_BIND_VATTR, NOP_BIND_VSTREAM, NOP_BIND_VBUF,
    NOP_BIND_UBO, NOP_BIND_SSBO, NOP_BIND_TEX, NOP_BIND_SAMP,
    NOP_STENCIL, NOP_POLY_OFFSET, NOP_DRAW, NOP_DRAW_IDX, NOP_DISPATCH, NOP_BARRIER,
    NOP_SET_TEX_POOL, NOP_SET_SAMP_POOL, NOP_COPY_B2T, NOP_COPY_STRIDE, NOP_BLEND_COLOR, NOP_COPY_T2B, NOP_BIND_IMG,
    NOP_COPY_T2T, NOP_RESET_COUNTER, NOP_CLEAR_TEX, NOP_DEPTH_CLAMP, NOP_RENDER_ENABLE, NOP_RASTER_DISCARD,
    NOP_RENDER_COND, NOP_PRIMITIVE_RESTART, NOP_BIND_BUFS,
    NOP_DRAW_INDIRECT, NOP_DISPATCH_INDIRECT,   /* (mkw-recomp: NVN2's GPU-driven draws) */
    NOP_DRAW_MESH,                              /* (mkw-recomp: NVN2 mesh shading; the WebGPU replay leaves it out) */
    NOP_WAIT_EVENT,                             /* (mkw-recomp: a wait for an event another queue's work signals) */
    NOP_PUSH_CONST,                             /* (hookshot: nvnCommandBufferPushComputeConstants) */
    NOP_CLEAR_BUF,                              /* (hookshot: nvnCommandBufferClearBuffer) */
    NOP_COUNT_
};
typedef struct { int32_t n; const nvn_tex *color[8]; nvn_view cview[8]; uint8_t has_cview[8]; const nvn_tex *depth; nvn_view dview; uint8_t has_dview; } nvn_p_set_rt;
typedef struct { int32_t index, mask; float rgba[4]; } nvn_p_clear_color;
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } nvn_p_clear_ds;
typedef struct { int32_t first, n; float v[16][4]; } nvn_p_viewports;
typedef struct { int32_t x, y, w, h; } nvn_p_scissor;
typedef struct { int32_t value; int32_t index; } nvn_p_flag;
typedef struct { int32_t mode; int32_t snapped; uint64_t addr, x, y; } nvn_p_render_cond;   /* render when the counters at addr, addr + 16 are equal (0) / differ (1) */
typedef struct { float n, f; } nvn_p_depth_range;
typedef struct { const nvn_prog *prog; int32_t stages; } nvn_p_bind_program;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } nvn_p_bind_state;   /* kind: 0 blend 1 color 2 channel mask 3 multisample 4 polygon 5 depth-stencil */
typedef struct { int32_t first, n; uint32_t st[16]; } nvn_p_bind_vattr;
typedef struct { int32_t first, n; nvn_vs_st st[16]; } nvn_p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } nvn_p_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } nvn_p_bind_buf;   /* uniform / storage buffer */
/* src: deferred binds keep the game's handle array (guest address) and read it at submit; 0: immediate */
typedef struct { int32_t stage, first, n, pad_; uint64_t src; uint64_t h[32]; } nvn_p_bind_handles;
/* deferred uniform / storage buffer ranges: read from the game's array (src, guest address) at submit */
typedef struct { int32_t ssbo, stage, first, n; uint64_t src; NVNbufferRange r[16]; } nvn_p_bind_bufs;
typedef struct { int32_t face, value_mask, mask, ref, which; } nvn_p_stencil;   /* which: 0 value mask 1 write mask 2 reference */
typedef struct { float factor, units, clamp; } nvn_p_poly_offset;
typedef struct { int32_t prim, first, count, base_instance, instances; } nvn_p_draw;
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf; } nvn_p_draw_idx;
typedef struct { int32_t x, y, z, pad; } nvn_p_dispatch;
typedef struct { const nvn_dpool *pool; } nvn_p_set_pool;
typedef struct { uint64_t buf; const nvn_tex *tex; nvn_view view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } nvn_p_copy_bt;   /* buffer <-> texture */
typedef struct { const nvn_tex *src, *dst; nvn_view sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } nvn_p_copy_tt;
typedef struct { const nvn_tex *tex; nvn_view view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } nvn_p_clear_tex;
typedef struct { int64_t stride; } nvn_p_copy_stride;
typedef struct { float rgba[4]; } nvn_p_blend_color;
typedef struct { uint64_t gpu; uint32_t mode, value; } nvn_p_signal;   /* event: its 32-bit value's GPU address */
typedef struct { uint64_t gpu; uint32_t mode, value; } nvn_p_wait_event;   /* (the same, and the wait's condition) */
typedef struct { uint64_t type, addr; } nvn_p_counter;
#define NVN_COUNTER_WRITTEN (1ull << 32)   /* (type: the report's counter word is in memory already: a clear flag, host/nvn_native.c) */
typedef struct { uint64_t src, dst, size; } nvn_p_copy_bb;
typedef struct { uint64_t dst, size; /* data follows */ } nvn_p_update_ub;
/* (hookshot) compute push constants: the bytes at offset in the compute driver bank's push area */
typedef struct { uint32_t offset, size; /* data follows */ } nvn_p_push_const;
/* (hookshot) a buffer range filled with a 32-bit value */
typedef struct { uint64_t dst, size; uint32_t value, pad; } nvn_p_clear_buf;
typedef struct { int32_t enable, index; } nvn_p_restart;
/* (mkw-recomp) indirect draws: up to max draws of `stride`-byte arguments at `indirect` (indexed: count, instances,
   first index, base vertex, base instance; arrays: count, instances, first, base instance), as many as the 32-bit
   value at count_addr says when there is one; read at replay, after the GPU's writes there reached guest memory */
typedef struct { int32_t indexed, prim, itype, max, stride, pad; uint64_t ibuf, indirect, count_addr; } nvn_p_draw_indirect;
typedef struct { uint64_t addr; } nvn_p_dispatch_indirect;   /* three 32-bit group counts */
/* DrawMeshTasks (x, y, z: the call's arguments as they are), DrawMeshTasksIndirect (indirect, max 1),
   MultiDrawMeshTasksIndirectCount (indirect, count_addr, max, stride) */
typedef struct { int32_t x, y, z, max, stride, kind; uint64_t indirect, count_addr; } nvn_p_draw_mesh;

/* ---- the API layer's services the replay uses (hos/nvn_native.c) ---- */
/* host pointer of a GPU address (virtual pools resolved through their page table); null when not mapped */
uint8_t *nvn_gpu_host(uint64_t gpu);
/* how many bytes from gpu on are contiguous in host memory (at most n) */
uint64_t nvn_gpu_contiguous(uint64_t gpu, uint64_t n);
/* the guest address of a GPU address (0: not mapped) */
uint64_t nvn_gpu_guest(uint64_t gpu);
/* copies n bytes at a GPU address (page by page for virtual pools); 0 when part of it is not mapped */
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n);
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n);
/* the texture currently living at a storage address (the game re-initialises transient targets every frame at the
   same memory without registering them again): 1 and *out when there is one */
int nvn_tex_live_at(uint64_t gpu, nvn_tex *out);
/* NVN_DEPTH_MODE_NEAR_IS_MINUS_W (GL clip z) was selected */
int nvn_depth_minus_w(void);

/* ---- the replay (gpu/render/rx_nvn.cpp) ---- */
/* the recordings of a submit, in order (handles: host pointers of NOP_BEGIN records), replayed now */
void rx_nvn_submit(const uint64_t *handles, int n);
/* (the same with the queue it came from: the game's queues have their own state and order each other with events) */
void rx_nvn_submit_q(const uint64_t *handles, int n, uint64_t queue);
/* fence value of everything submitted so far; waits until the GPU reached a fence (1: reached, 0: timed out) */
uint64_t rx_nvn_fence(void);
uint64_t rx_nvn_fence_q(uint64_t queue);   /* (hookshot) a fence of one queue */
/* submits and presents queued so far / replayed so far (recordings released after a submit stay until it replayed) */
uint64_t rx_nvn_pushed(void);
uint64_t rx_nvn_replayed(void);
int rx_nvn_wait(uint64_t fence, uint64_t timeout_ns);
/* (the threaded web build) waits while a render target writeback not in guest memory yet will write [p, p + n): the
   game's memcpy reads the targets it copies with the CPU through this, so fences need not wait for them */
void rx_wait_writebacks_reading(const void *p, uint64_t n);
void rx_nvn_finish(void);
/* a window texture to the screen (crop: x, y, w, h; w = 0 whole) */
void rx_nvn_present(const nvn_tex *t, const int crop[4]);
/* texture / sampler descriptor registrations (copies are taken) */
void rx_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v);
void rx_nvn_register_sampler(int id, const nvn_samp_par *p);
void rx_nvn_set_texture_pool_size(int n);
/* objects that went away: what the replay made for them can go */
void rx_nvn_texture_released(const nvn_tex *t);
void rx_nvn_texture_created(const nvn_tex *t);
void rx_nvn_program_released(const nvn_prog *p);
/* (hookshot) a program was given its shaders (the Vulkan back end loads what it kept for it; WebGPU: nothing) */
void rx_nvn_program_created(const nvn_prog *p);
/* CPU reads / writes of a texture (ReadTexels / WriteTexels) */
void rx_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                        size_t row, size_t img);
void rx_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                         size_t row, size_t img);
/* the CPU is about to read memory the GPU may have written: GPU results there go to guest memory now */
void rx_nvn_invalidate(uint64_t gpu, uint64_t size);
void rx_nvn_pool_created(uint64_t gpu, uint64_t size);   /* (the Vulkan back end imports it early; WebGPU: nothing) */
/* virtual pages [gpu, gpu + size) mapped now are about to be unmapped or remapped: what the GPU drew into textures
   there goes into their memory first (the Vulkan back end; WebGPU: nothing) */
void rx_nvn_vmap_changing(uint64_t gpu, uint64_t size);
/* the CPU set an event's value (nvnEventSignal): in order with the submits, for the GPU's waits on it */
void rx_nvn_event_signal(uint64_t gpu, uint32_t value);

#ifdef __cplusplus
}
#endif
