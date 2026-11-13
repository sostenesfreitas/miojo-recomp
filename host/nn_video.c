/* nn::codec's hardware video decoders (VP8/VP9 here) and their image pools, on FFmpeg's libavcodec (the static
   libraries in Eglantine's package cache: work/eg/.cache/cpm/ffmpeg). MKW's movie library (subsdk0, movie::) plays
   its WebM movies through them: the cup openings, the courses' billboards, the emblems.

   How the library uses them (subsdk0, movie::detail::VideoDecoderBuffer): each picture buffer is an image pool of
   one image, sized by MakeDecodeImagePreference + CalculateDecoderImagePoolBuffer, registered with the decoder by
   index (RegisterXDecoderBuffer). It submits each compressed frame (SubmitXDecoderInputBuffer), takes finished
   pictures by buffer index (AcquireXDecoderOutputBuffer, GetXDecoderOutputBufferProperties), reads them back to
   its own memory as RGBA or NV12 (ReadDecoderOutputImageRgba / Nv12) and gives the buffer back (Release).

   Here a submission is decoded at once; a finished picture waits in a queue until an Acquire puts it in the first
   free registered buffer (host memory: the pool's guest memory is not used). The video service itself (nn::gsv) is
   not emulated: nn::codec::video::Initialize does nothing. */
#include "hos.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef _WIN32
#include <shellapi.h>
#endif
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

#define RESULT_NO_OUTPUT 0x2a1ca   /* (a failure: "no picture yet"; the library polls again) */
#define POOL_MAGIC 0x4c4f4f50u     /* 'POOL' */
#define DEC_MAGIC 0x43454456u      /* 'VDEC' */
#define PREF_MAGIC 0x46455250u     /* 'PREF' */
#define MAX_SLOTS 16
#define MAX_QUEUE 8

static int video_log(void) {   /* HOS_VIDEO_LOG=1: submissions and acquisitions */
    static int on = -1;
    if (on < 0) on = env_on("HOS_VIDEO_LOG");
    return on;
}

void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);

/* an image pool: its pictures, w x h, as RGBA and as NV12 (both kept: the reader asks for either) */
typedef struct {
    int w, h, format, count;
    uint8_t *rgba, *nv12;
    uint64_t mem, mem_size;   /* (hookshot) the pool's guest memory: where the hardware decoder writes its pictures */
    int rgba_stale;           /* (hookshot) its NV12 picture not converted to RGBA yet (pool_rgba, when it is asked for) */
    int read_by_api;          /* (hookshot) read through ReadDecoderOutputImage*: no copy into its memory (dec_acquire) */
    struct SwsContext *sws;   /* (that conversion's) */
} Pool;
/* the preference MakeDecodeImagePreference returns (the caller's buffer is larger: only these fields are ours) */
typedef struct { uint32_t magic; uint16_t w, h; uint32_t format, tiling; } Pref;

/* (rgba NULL: a picture decoded as NV12 is kept as that alone; its pool makes the RGBA if a reader wants it) */
typedef struct { uint8_t *rgba, *nv12; int w, h; int64_t pts; int last; } Picture;
/* a submission waiting to be decoded (pkt NULL: the stream's end) */
typedef struct Pending { AVPacket *pkt; struct Pending *next; } Pending;
#ifdef _WIN32
/* (hookshot, issue 380) once, when Windows has no AV1 decoder: OoT's movies are AV1, and Windows decodes it only with
   Microsoft's free "AV1 Video Extension" (preinstalled on most PCs, not all). Yes opens its Store page */
static DWORD WINAPI av1_notice(LPVOID unused) {
    (void)unused;
    if (MessageBoxA(NULL, "The game's movies need Microsoft's free \"AV1 Video Extension\", which this PC doesn't have, so they "
                          "show black (the sound plays).\n\nOpen its page in the Microsoft Store now? Install it, then start the game again.",
                    "Hookshot", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
        ShellExecuteA(NULL, "open", "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V", NULL, NULL, SW_SHOWNORMAL);
    return 0;
}
#endif
/* (hookshot) host/mf_av1.c: AV1 on Media Foundation, as the static FFmpeg has no AV1 decoder */
typedef void (*mf_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);
void *mf_av1_open(int w, int h);
void mf_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user);
void mf_av1_flush(void *m);
void mf_av1_close(void *m);
/* (hookshot) an AV1 decoder of our own, when FFmpeg has none: dav1d (host/dav1d_av1.c, when the build has it, issue 2316), else
   Media Foundation. The same calls for both */
typedef struct {
    const char *name;
    void *(*open)(int w, int h);
    void (*decode)(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user);
    void (*flush)(void *m);
    void (*close)(void *m);
} Av1Decoder;
#ifdef HOOKSHOT_DAV1D
void *dav1d_av1_open(int w, int h);
void dav1d_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user);
void dav1d_av1_flush(void *m);
void dav1d_av1_close(void *m);
#endif
static const Av1Decoder av1_decoders[] = {
#ifdef HOOKSHOT_DAV1D
    {"dav1d", dav1d_av1_open, dav1d_av1_decode, dav1d_av1_flush, dav1d_av1_close},
#endif
    {"Media Foundation", mf_av1_open, mf_av1_decode, mf_av1_flush, mf_av1_close},
};

typedef struct {
    AVCodecContext *ctx;
    void *mf;                        /* (hookshot) an AV1 decoder of our own instead of ctx (av1: which one) */
    const Av1Decoder *av1;
    int64_t decode_ns, decode_worst_ns, decoded, skipped;   /* (hookshot) the movie's decoding, logged when it stops */
    int64_t first_pts, end_pts;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    int sws_w, sws_h, sws_fmt;
    uint64_t slots[MAX_SLOTS];       /* registered pools (guest pointers), by buffer index */
    int busy[MAX_SLOTS];             /* acquired, not released */
    int64_t slot_pts[MAX_SLOTS];
    int slot_last[MAX_SLOTS];        /* the stream's last picture (the submission marked last, all decoded) */
    Picture queue[MAX_QUEUE];
    int qhead, qn;
    int eos;                         /* the last submission is in and decoded: an empty queue is the end */
    int stopped;                     /* (hookshot) Stop or Flush since the last submission: no picture is coming (dec_acquire) */
    Pending *phead, *ptail;          /* submissions not decoded yet, oldest first */
    int64_t submits, acquires;
    int clock_set;                   /* (hookshot) the movie's clock: the picture at clock_pts was due at clock_ns */
    int64_t clock_ns, clock_pts;
    int64_t last_pts;                /* the pts of the last picture handed out */
    int64_t log_submits;             /* (HOS_VIDEO_LOG) submissions still to log, after a flush or the end */
    SRWLOCK lock;
} Dec;

/* (hookshot) the host objects of the game's pool and decoder objects, by the guest object's address: kept here, not in
   the guest object (guest memory). Few at a time (a movie's decoder and its pools). */
typedef struct { uint64_t obj; uint32_t magic; void *p; } HostObj;
static HostObj *host_objs;
static size_t nhost_objs, cap_host_objs;
static SRWLOCK host_objs_lock = SRWLOCK_INIT;
static void *host_of(uint64_t obj, uint32_t magic) {
    void *p = NULL;
    if (!obj) return NULL;
    AcquireSRWLockShared(&host_objs_lock);
    for (size_t i = 0; i < nhost_objs; i++)
        if (host_objs[i].obj == obj && host_objs[i].magic == magic) p = host_objs[i].p;
    ReleaseSRWLockShared(&host_objs_lock);
    return p;
}
static void set_host(uint64_t obj, uint32_t magic, void *p) {
    if (!obj) return;
    AcquireSRWLockExclusive(&host_objs_lock);
    size_t i = 0;
    while (i < nhost_objs && host_objs[i].obj != obj) i++;
    if (!p) {
        if (i < nhost_objs) host_objs[i] = host_objs[--nhost_objs];
    } else {
        if (i == nhost_objs) {
            if (nhost_objs == cap_host_objs) {
                cap_host_objs = cap_host_objs ? cap_host_objs * 2 : 16;
                host_objs = (HostObj *)realloc(host_objs, cap_host_objs * sizeof *host_objs);
                if (!host_objs) hos_fatal(NULL, "video: out of memory");
            }
            nhost_objs++;
        }
        host_objs[i] = (HostObj){obj, magic, p};
    }
    ReleaseSRWLockExclusive(&host_objs_lock);
}

/* ---- the service ---- */
HLE(_ZN2nn5codec5video10InitializeEv) {
    static int logged;
    if (!logged++) hos_log("video: nn::codec::video on libavcodec");
    RET(0);
}
HLE(_ZN2nn5codec5video8FinalizeEv) { RET(0); }

/* ---- image pools ---- */
/* MakeDecodeImagePreference(u16 w, u16 h, OutputFormat, TilingMode) -> the preference (returned through x8) */
HLE(_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE) {
    const uint64_t out = c->x[8];
    Pref p = {PREF_MAGIC, (uint16_t)ARG(0), (uint16_t)ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3)};
    if (out) memcpy(GP(out), &p, sizeof p);
    RET(out);
}
/* CalculateDecoderImagePoolBuffer(u64* size, u64* alignment, Preference const&, u32 count) */
HLE(_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj) {
    const Pref *p = (const Pref *)GP(ARG(2));
    const uint64_t one = p->magic == PREF_MAGIC ? (uint64_t)p->w * p->h * 4 : 0x400000;
    if (ARG(0)) G64(ARG(0)) = ((one * (uint32_t)ARG(3)) + 0xFFF) & ~0xFFFull;
    if (ARG(1)) G64(ARG(1)) = 0x1000;
    RET(0);
}
/* InitializeDecoderImagePool(pool, void* memory, size, Preference const&, u32 count) */
HLE(_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj) {
    const Pref *p = (const Pref *)GP(ARG(3));
    Pool *o = (Pool *)calloc(1, sizeof *o);
    if (!o) hos_fatal(NULL, "video: out of memory for an image pool");
    o->w = p->magic == PREF_MAGIC && p->w ? p->w : 1280, o->h = p->magic == PREF_MAGIC && p->h ? p->h : 720;
    o->format = p->magic == PREF_MAGIC ? (int)p->format : 2;
    o->count = (int)ARG(4) > 0 ? (int)ARG(4) : 1;
    o->rgba = (uint8_t *)calloc((size_t)o->count, (size_t)o->w * o->h * 4);
    /* (hookshot, issue 342: an allocation that failed went on as a NULL picture, and the movie thread read address 0) */
    o->nv12 = (uint8_t *)calloc((size_t)o->count, (size_t)o->w * o->h * 3 / 2);
    /* (hookshot) video black, not zeros. A buffer the game takes before the decoder's first picture is shown as it is
       (dec_acquire: "nothing decoded, the buffer again", up to ~100 ms into a movie's start), and NV12 zeros are bright
       green (luma 0 with no chroma): one green frame at each scene change that opens a movie. Luma 16, chroma 128 is
       black; the RGBA opaque black */
    if (o->nv12) {
        for (int i = 0; i < o->count; i++) {
            uint8_t *img = o->nv12 + (size_t)i * o->w * o->h * 3 / 2;
            memset(img, 0x10, (size_t)o->w * o->h);
            memset(img + (size_t)o->w * o->h, 0x80, (size_t)o->w * o->h / 2);
        }
    }
    if (o->rgba)
        for (size_t k = 3; k < (size_t)o->count * o->w * o->h * 4; k += 4) o->rgba[k] = 0xff;
    o->mem = ARG(1), o->mem_size = ARG(2);
    if (video_log())
        hos_log("video: image pool %#llx: %dx%d format %d, %d images, memory %#llx+%#llx", (unsigned long long)ARG(0),
                o->w, o->h, o->format, o->count, (unsigned long long)o->mem, (unsigned long long)o->mem_size);
    set_host(ARG(0), POOL_MAGIC, o);
    RET(0);
}
HLE(_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    if (o && o->sws) sws_freeContext(o->sws);
    if (o) free(o->rgba), free(o->nv12), free(o);
    set_host(ARG(0), POOL_MAGIC, NULL);
}
/* (hookshot) the pool's picture as RGBA, from its NV12, if it was not made yet. Each 1080p frame of OoT's movies was
   converted on the movie thread, in plain C, though OoT reads only the NV12: nearly half that thread's time, which put the
   picture behind the sound */
static SRWLOCK pool_rgba_lock = SRWLOCK_INIT;
static void pool_rgba(Pool *o) {
    AcquireSRWLockExclusive(&pool_rgba_lock);
    if (o->rgba_stale && o->rgba && o->nv12) {
        if (!o->sws)
            o->sws = sws_getContext(o->w, o->h, AV_PIX_FMT_NV12, o->w, o->h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        const uint8_t *src[4] = {o->nv12, o->nv12 + (size_t)o->w * o->h, NULL, NULL};
        const int src_stride[4] = {o->w, o->w, 0, 0};
        uint8_t *dst[4] = {o->rgba, NULL, NULL, NULL};
        int dst_stride[4] = {o->w * 4, 0, 0, 0};
        if (o->sws) sws_scale(o->sws, src, src_stride, 0, o->h, dst, dst_stride);
        o->rgba_stale = 0;
    }
    ReleaseSRWLockExclusive(&pool_rgba_lock);
}
/* ReadDecoderOutputImageRgba(pool, u32 index, u16 x, u16 y, u16 w, u16 h, void* dst, size_t stride, size_t size) */
HLE(_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    const uint32_t i = (uint32_t)ARG(1), x = (uint16_t)ARG(2), y = (uint16_t)ARG(3), w = (uint16_t)ARG(4), h = (uint16_t)ARG(5);
    const uint64_t dst = ARG(6), stride = ARG(7) ? ARG(7) : (uint64_t)w * 4;
    {   /* (hookshot, HOS_VIDEO_LOG) the first reads, with where they go */
        static volatile LONG n;
        if (video_log() && InterlockedIncrement(&n) <= 4)
            hos_log("video: ReadDecoderOutputImageRgba(pool %s, index %u, %u,%u %ux%u, dst %#llx stride %llu)",
                    o ? "ok" : "unknown", i, x, y, w, h, (unsigned long long)dst, (unsigned long long)stride);
    }
    if (!o || !o->rgba || i >= (uint32_t)o->count || !dst || x >= (uint32_t)o->w) { RET(0); return; }
    o->read_by_api = 1;
    if (i == 0 && o->rgba_stale) pool_rgba(o);
    const uint8_t *src = o->rgba + (size_t)i * o->w * o->h * 4;
    const uint32_t cw = x + w <= (uint32_t)o->w ? w : o->w - x;
    for (uint32_t r = 0; r < h && y + r < (uint32_t)o->h; r++) {
        void *row = hos_guest_range(dst + r * stride, (size_t)cw * 4);   /* (hookshot: only into guest memory) */
        if (row) memcpy(row, src + ((size_t)(y + r) * o->w + x) * 4, (size_t)cw * 4);
    }
    HOS_WRITTEN(GP(dst), (size_t)(h ? h - 1 : 0) * stride + (size_t)cw * 4);   /* (the game may give it to the GPU) */
    RET(0);
}
/* ReadDecoderOutputImageNv12(pool, u32 index, u16 x, u16 y, u16 w, u16 h, void* y_dst, size_t y_stride, [stack:]
   size_t y_size, void* uv_dst, size_t uv_stride, size_t uv_size) */
HLE(_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm) {
    Pool *o = (Pool *)host_of(ARG(0), POOL_MAGIC);
    const uint32_t i = (uint32_t)ARG(1), x = (uint16_t)ARG(2), y = (uint16_t)ARG(3), w = (uint16_t)ARG(4), h = (uint16_t)ARG(5);
    const uint64_t ydst = ARG(6), ystride = ARG(7) ? ARG(7) : w;
    const uint64_t uvdst = G64(c->x[31] + 8), uvstride = G64(c->x[31] + 16) ? G64(c->x[31] + 16) : w;
    {   /* (HOS_VIDEO_LOG) each new destination, with the pool and image */
        static uint64_t seen[32];
        static volatile LONG nseen;
        int k;
        for (k = 0; k < nseen && seen[k] != ydst; k++) {}
        if (video_log() && k == nseen && nseen < 32) {
            seen[InterlockedIncrement(&nseen) - 1] = ydst;
            hos_log("video: ReadDecoderOutputImageNv12(pool %#llx %s, index %u, %u,%u %ux%u, y %#llx/%llu, uv %#llx/%llu) "
                    "lr %#llx", (unsigned long long)ARG(0), o ? "ok" : "unknown", i, x, y, w, h, (unsigned long long)ydst,
                    (unsigned long long)ystride, (unsigned long long)uvdst, (unsigned long long)uvstride,
                    (unsigned long long)c->x[30]);
        }
    }
    /* (hookshot) and no NV12 when its allocation failed: o->nv12 + offset was then any host address, copied to the game */
    if (!o || !o->nv12 || i >= (uint32_t)o->count || x >= (uint32_t)o->w) { RET(0); return; }
    o->read_by_api = 1;
    const uint8_t *yp = o->nv12 + (size_t)i * o->w * o->h * 3 / 2, *uvp = yp + (size_t)o->w * o->h;
    const uint32_t cw = x + w <= (uint32_t)o->w ? w : o->w - x;
    for (uint32_t r = 0; ydst && r < h && y + r < (uint32_t)o->h; r++) {
        void *row = hos_guest_range(ydst + r * ystride, cw);   /* (hookshot: only into guest memory) */
        if (row) memcpy(row, yp + (size_t)(y + r) * o->w + x, cw);
    }
    for (uint32_t r = 0; uvdst && r < h / 2 && y / 2 + r < (uint32_t)o->h / 2; r++) {
        void *row = hos_guest_range(uvdst + r * uvstride, cw);
        if (row) memcpy(row, uvp + (size_t)(y / 2 + r) * o->w + (x & ~1u), cw);
    }
    if (ydst) HOS_WRITTEN(GP(ydst), (size_t)(h ? h - 1 : 0) * ystride + cw);
    if (uvdst) HOS_WRITTEN(GP(uvdst), (size_t)(h / 2 ? h / 2 - 1 : 0) * uvstride + cw);
    RET(0);
}

/* ---- decoders ---- */
static void dec_init_sized(Ctx *c, enum AVCodecID id, const char *what, int w, int h);
static void dec_init(Ctx *c, enum AVCodecID id, const char *what) { dec_init_sized(c, id, what, 0, 0); }
static void dec_init_sized(Ctx *c, enum AVCodecID id, const char *what, int w, int h) {
    const AVCodec *codec = NULL;
#ifndef _WIN32   /* (linux) FFmpeg's own AV1 decoder needs a hardware context, and there is no Media Foundation to fall
                    back on: dav1d, else libaom, by name */
    if (id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder_by_name("libdav1d");
    if (!codec && id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder_by_name("libaom-av1");
#endif
    if (!codec) codec = avcodec_find_decoder(id);
    Dec *d = (Dec *)calloc(1, sizeof *d);
    InitializeSRWLock(&d->lock);
    if (codec) {
        d->ctx = avcodec_alloc_context3(codec);
        d->ctx->thread_count = id == AV_CODEC_ID_AV1 ? 8 : 4;   /* (1080p AV1 in software) */
        if (avcodec_open2(d->ctx, codec, NULL) < 0) avcodec_free_context(&d->ctx);
    }
    for (size_t i = 0; !d->ctx && !d->mf && id == AV_CODEC_ID_AV1 && i < sizeof av1_decoders / sizeof av1_decoders[0]; i++)
        if ((d->mf = av1_decoders[i].open(w, h))) d->av1 = &av1_decoders[i];
#ifdef _WIN32
    if (!d->ctx && !d->mf && id == AV_CODEC_ID_AV1) {   /* (issue 380: no AV1 decoder in Windows: the movies were black, with sound) */
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1)) {
            hos_log("video: no AV1 decoder: install Microsoft's free AV1 Video Extension for the movies");
            if (!env_on("HOS_HEADLESS") && !env_on("HOOKSHOT_NO_AV1_NOTICE"))
                CreateThread(NULL, 0, av1_notice, NULL, 0, NULL);
        }
    }
#endif
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    set_host(ARG(0), DEC_MAGIC, d);
    static int logged;
    if (logged++ < 4)
        hos_log("video: %s decoder %s%s (%dx%d)", what, d->ctx || d->mf ? "ready on " : "not available in this FFmpeg",
                d->ctx ? d->ctx->codec->name : d->mf ? d->av1->name : "", w, h);
    RET(0);
}
static void picture_free(Picture *p) {
    free(p->rgba), free(p->nv12);
    memset(p, 0, sizeof *p);
}
static void pending_free(Dec *d) {
    while (d->phead) {
        Pending *n = d->phead;
        d->phead = n->next;
        av_packet_free(&n->pkt);
        free(n);
    }
    d->ptail = NULL;
}
static void movie_report(Dec *d);
static void dec_finalize(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (!d) return;
    movie_report(d);
    pending_free(d);
    for (int i = 0; i < d->qn; i++) picture_free(&d->queue[(d->qhead + i) % MAX_QUEUE]);
    if (d->sws) sws_freeContext(d->sws);
    avcodec_free_context(&d->ctx);
    if (d->mf) d->av1->close(d->mf);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    free(d);
    set_host(ARG(0), DEC_MAGIC, NULL);
}
/* a decoded frame into the queue as NV12, deferring RGBA to pool_rgba() unless eager conversion is forced */
static void queue_frame(Dec *d, const AVFrame *f) {
    const int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return;
    const int nv12_src = f->format == AV_PIX_FMT_NV12;
    const int yuv420 = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVA420P;
    const char *rgba_env = getenv("HOS_VIDEO_RGBA");
    const int force_rgba = rgba_env && atoi(rgba_env) != 0;
    const int lazy_rgba = !force_rgba && (nv12_src || yuv420);
    if (!lazy_rgba && (!d->sws || d->sws_w != w || d->sws_h != h || d->sws_fmt != f->format)) {
        if (d->sws) sws_freeContext(d->sws);
        d->sws = sws_getContext(w, h, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        d->sws_w = w, d->sws_h = h, d->sws_fmt = f->format;
    }
    Picture p = {0};
    p.w = w, p.h = h, p.pts = f->pts;
    const size_t nv12_size = (size_t)w * h * 3 / 2;
    p.rgba = lazy_rgba ? NULL : (uint8_t *)malloc((size_t)w * h * 4);
    p.nv12 = (uint8_t *)malloc(nv12_size);
    if (!p.nv12 || (!lazy_rgba && !p.rgba)) {   /* (out of memory: this frame is dropped, not written through NULL; issue 342) */
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 4) hos_log("video: no memory for a %dx%d frame: dropped", w, h);
        free(p.rgba), free(p.nv12);
        return;
    }
    if ((w | h) & 1) memset(p.nv12, 0, nv12_size);
    uint8_t *dst[4] = {p.rgba, NULL, NULL, NULL};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    if (d->sws && p.rgba) sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, dst_stride);
    if (nv12_src) {   /* (hookshot: Media Foundation's pictures) */
        for (int y = 0; y < h; y++) memcpy(p.nv12 + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        for (int y = 0; y < h / 2; y++)
            memcpy(p.nv12 + (size_t)w * h + (size_t)y * w, f->data[1] + (size_t)y * f->linesize[1], (size_t)w);
    } else if (yuv420) {   /* (planar 4:2:0 -> NV12) */
        for (int y = 0; y < h; y++) memcpy(p.nv12 + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        uint8_t *uv = p.nv12 + (size_t)w * h;
        const uint8_t *u_src = f->data[1], *v_src = f->data[2];
        const int u_stride = f->linesize[1], v_stride = f->linesize[2];
        for (int y = 0; y < h / 2; y++) {
            const uint8_t *u_row = u_src + (size_t)y * u_stride;
            const uint8_t *v_row = v_src + (size_t)y * v_stride;
            uint8_t *uv_row = uv + (size_t)y * w;
            int x = 0;
#if defined(__x86_64__) || defined(_M_X64)
            for (; x + 16 <= w / 2; x += 16) {
                __m128i u16 = _mm_loadu_si128((const __m128i *)(u_row + x));
                __m128i v16 = _mm_loadu_si128((const __m128i *)(v_row + x));
                _mm_storeu_si128((__m128i *)(uv_row + 2 * x), _mm_unpacklo_epi8(u16, v16));
                _mm_storeu_si128((__m128i *)(uv_row + 2 * x + 16), _mm_unpackhi_epi8(u16, v16));
            }
#endif
            for (; x < w / 2; x++) {
                uv_row[2 * x] = u_row[x];
                uv_row[2 * x + 1] = v_row[x];
            }
        }
    } else {
        memset(p.nv12, 0, nv12_size);
    }
    if (d->qn == MAX_QUEUE) {
        picture_free(&d->queue[d->qhead]);
        d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
    }
    d->queue[(d->qhead + d->qn) % MAX_QUEUE] = p;
    d->qn++;
}
static void mf_picture(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts) {
    AVFrame f;
    memset(&f, 0, sizeof f);
    f.width = w, f.height = h, f.format = AV_PIX_FMT_NV12, f.pts = pts;
    f.data[0] = (uint8_t *)y, f.data[1] = (uint8_t *)uv;
    f.linesize[0] = f.linesize[1] = stride;
    queue_frame((Dec *)user, &f);
}
static void drain(Dec *d) {
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        queue_frame(d, d->frame);
        av_frame_unref(d->frame);
    }
}
/* the oldest submissions decoded until a picture comes out (or none are left). The stream's end is taken as soon as
   it is next, pictures queued or not: the last picture decoded must carry the last flag, as the hardware's does. Media
   Foundation's decoder holds nothing back, so waiting for an empty queue gave the flag only to a buffer handed out
   again, with an older picture's pts, after the real last one went out unmarked */
static void decode_next(Dec *d);
static void decode_pending(Dec *d) {
    while (d->phead && (!d->qn || !d->phead->pkt)) decode_next(d);
}
/* one submission decoded (its pictures into the queue, if any) */
static void decode_next(Dec *d) {
    if (d->phead) {
        Pending *n = d->phead;
        d->phead = n->next;
        if (!d->phead) d->ptail = NULL;
        const int64_t t0 = hos_now_ns();
        if (n->pkt) {
            if (!d->decoded++) d->first_pts = n->pkt->pts;
            d->end_pts = n->pkt->pts;
        }
        if (d->mf) {   /* (hookshot) */
            if (n->pkt) {
                d->eos = 0;
                d->av1->decode(d->mf, n->pkt->data, (size_t)n->pkt->size, n->pkt->pts, mf_picture, d);
                av_packet_free(&n->pkt);
            } else {
                d->av1->decode(d->mf, NULL, 0, 0, mf_picture, d);
                d->eos = 1;
                d->log_submits = 8;   /* (data after the end: a restart, worth seeing) */
                if (d->qn) d->queue[(d->qhead + d->qn - 1) % MAX_QUEUE].last = 1;
            }
        } else if (n->pkt) {
            d->eos = 0;
            if (avcodec_send_packet(d->ctx, n->pkt) == AVERROR(EAGAIN)) {   /* (full: its pictures out first) */
                drain(d);
                avcodec_send_packet(d->ctx, n->pkt);
            }
            drain(d);
            av_packet_free(&n->pkt);
        } else {   /* (the end: the rest out, the last picture marked) */
            avcodec_send_packet(d->ctx, NULL);
            drain(d);
            d->eos = 1;
            d->log_submits = 8;
            if (d->qn) d->queue[(d->qhead + d->qn - 1) % MAX_QUEUE].last = 1;
            avcodec_flush_buffers(d->ctx);   /* (a stream after it starts again) */
        }
        const int64_t ns = hos_now_ns() - t0;
        d->decode_ns += ns;
        if (ns > d->decode_worst_ns) d->decode_worst_ns = ns;
        free(n);
    }
}
/* (hookshot, issue 2316) one line when a movie stops: its decoder, the time it took for each submission against the time
   between the movie's pictures, and the pictures the catch-up skipped. A decoder slower than the movie leaves the picture
   behind the sound, whatever the catch-up does: the line says which it was in a report's hookshot_log.txt */
static void movie_report(Dec *d) {
    if (d->decoded > 1) {
        const double each = (double)d->decode_ns / d->decoded / 1e6,
                     every = (double)(d->end_pts - d->first_pts) / (d->decoded - 1) / 1e6;   /* (pts in ns) */
        hos_log("video: a movie of %lld pictures on %s: %.1f ms to decode each (worst %.1f), one shown every %.1f ms%s; "
                "%lld skipped to keep up with its sound", (long long)d->decoded,
                d->ctx ? d->ctx->codec->name : d->mf ? d->av1->name : "no decoder", each, (double)d->decode_worst_ns / 1e6,
                every, each > every ? " (slower than it plays)" : "", (long long)d->skipped);
    }
    d->decode_ns = d->decode_worst_ns = d->decoded = d->skipped = 0;
}
static void pending_push(Dec *d, AVPacket *pkt) {
    Pending *n = (Pending *)calloc(1, sizeof *n);
    n->pkt = pkt;
    if (d->ptail) d->ptail->next = n;
    else d->phead = n;
    d->ptail = n;
}
/* SubmitXDecoderInputBuffer(u64* id, decoder, data, size, TimeSpan pts, bool last): kept, decoded when a picture is
   acquired (the library submits a whole movie at once: its pictures decoded then would not fit any queue) */
static volatile LONG64 submissions;

/* (hookshot) A buffer handed out with nothing decoded into it, by a decoder with nothing left to decode, gets a sync
   handle that is not reached until something is submitted again (anywhere: the count is global). Every other buffer's
   is 0, reached. The hardware's is a fence the decoder signals once the picture is in the buffer, so an idle decoder's
   never is; the movie library's VideoDecoder::Flush recycles buffers until it finds one not reached, and with all of
   them reached it never returned: the movie never stopped, and the game sat on a black screen after its intro.
   The library also waits for the handle as a sync point event (VideoDecoderEventManager::SetFence), and the callback
   that event runs aborts unless TryWaitNativeSync says reached: an event for a handle not reached is signalled at the
   next submission, not when it is requested (hos_video_sync_defer, from nn_syncpt.c). */
#define SYNC_IDLE_TAG 0x5ec0000000000000ull
#define SYNC_IDLE_MASK 0x0000ffffffffffffull
int hos_video_sync_reached(uint64_t handle) {   /* (nn_gs.c: TryWaitNativeSync, TimedWaitNativeSync) */
    if ((handle & ~SYNC_IDLE_MASK) != SYNC_IDLE_TAG) return 1;
    return (handle & SYNC_IDLE_MASK) != ((uint64_t)InterlockedCompareExchange64(&submissions, 0, 0) & SYNC_IDLE_MASK);
}

static SRWLOCK waiter_lock = SRWLOCK_INIT;
static uint64_t waiters[32];   /* sync point events (their guest addresses) waiting for the next submission */
static int nwaiters;

/* RequestSyncPointEvent(event, handle): 1 when the event waits for the next submission, 0 when it is due now */
int hos_video_sync_defer(uint64_t event, uint64_t handle) {
    AcquireSRWLockExclusive(&waiter_lock);   /* (checked under the lock submitted() takes: no submission slips between) */
    int deferred = !hos_video_sync_reached(handle);
    if (deferred) {
        int i = 0;
        while (i < nwaiters && waiters[i] != event) i++;
        if (i == nwaiters && nwaiters < (int)(sizeof waiters / sizeof waiters[0])) waiters[nwaiters++] = event;
        else if (i == nwaiters) deferred = 0;   /* (full: signalled now, as before) */
    }
    ReleaseSRWLockExclusive(&waiter_lock);
    return deferred;
}

/* CancelSyncPointEvent(event) */
void hos_video_sync_cancel(uint64_t event) {
    AcquireSRWLockExclusive(&waiter_lock);
    for (int i = 0; i < nwaiters; i++)
        if (waiters[i] == event) { waiters[i] = waiters[--nwaiters]; break; }
    ReleaseSRWLockExclusive(&waiter_lock);
}

int hos_video_sync_waiting(uint64_t event) {   /* TryWaitSyncPointEvent(event): 1 while it waits for a submission */
    AcquireSRWLockExclusive(&waiter_lock);
    int i = 0;
    while (i < nwaiters && waiters[i] != event) i++;
    const int waiting = i < nwaiters;
    ReleaseSRWLockExclusive(&waiter_lock);
    return waiting;
}

void hos_system_event_signal(uint64_t key);
static uint64_t submitted(void) {   /* a submission: every handle given out before it is reached */
    uint64_t due[32];
    AcquireSRWLockExclusive(&waiter_lock);
    const uint64_t id = (uint64_t)InterlockedIncrement64(&submissions);
    const int n = nwaiters;
    memcpy(due, waiters, sizeof due);
    nwaiters = 0;
    ReleaseSRWLockExclusive(&waiter_lock);
    for (int i = 0; i < n; i++) hos_system_event_signal(due[i]);
    return id;
}

static void dec_submit(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(1), DEC_MAGIC);
    const uint64_t id = submitted();
    if (ARG(0)) G64(ARG(0)) = id;
    if (!d || (!d->ctx && !d->mf)) { RET(0); return; }
    AcquireSRWLockExclusive(&d->lock);
    /* (hookshot) the size is 64-bit: one past what an AVPacket holds was cut to a small packet and copied in whole; and
       the data must lie in guest memory */
    const void *data = ARG(2) && ARG(3) && ARG(3) <= INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ? hos_guest_range(ARG(2), ARG(3)) : NULL;
    if (ARG(2) && ARG(3) && !data) hos_log("video: submit of %#llx bytes at %#llx left out", (unsigned long long)ARG(3),
                                          (unsigned long long)ARG(2));
    if (data) {
        AVPacket *pkt = av_packet_alloc();
        if (pkt && av_new_packet(pkt, (int)ARG(3)) == 0) {
            memcpy(pkt->data, data, (size_t)ARG(3));
            pkt->pts = (int64_t)ARG(4);
            pending_push(d, pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
    d->submits++;
    d->stopped = 0;   /* (hookshot: pictures are coming again: dec_acquire waits for one as before) */
    if (ARG(5) & 1) pending_push(d, NULL);
    const int logged_one = d->log_submits > 0;
    if (logged_one) d->log_submits--;
    if (video_log() && (d->submits <= 8 || (ARG(5) & 1) || logged_one))
        hos_log("video: submit %lld (%llu bytes, pts %lld%s)", (long long)d->submits, (unsigned long long)ARG(3),
                (long long)ARG(4), (ARG(5) & 1) ? ", last" : "");
    ReleaseSRWLockExclusive(&d->lock);
    RET(0);
}
static void dec_flush(Ctx *c, const char *what) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (d->ctx || d->mf)) {
        AcquireSRWLockExclusive(&d->lock);
        if (video_log())
            hos_log("video: %s after %lld submits and %lld acquires (%d queued, %s, lr %#llx)", what, (long long)d->submits,
                    (long long)d->acquires, d->qn, d->eos ? "at the end" : d->phead ? "submissions left" : "none left",
                    (unsigned long long)c->x[30]);
        d->log_submits = 8;
        movie_report(d);
        if (d->ctx) avcodec_flush_buffers(d->ctx);
        else d->av1->flush(d->mf);
        pending_free(d);
        for (int i = 0; i < d->qn; i++) picture_free(&d->queue[(d->qhead + i) % MAX_QUEUE]);
        d->qhead = d->qn = 0;
        d->eos = 0;
        d->clock_set = 0;
        d->stopped = 1;   /* (hookshot: until the next submission, dec_acquire has nothing to wait for) */
        for (int i = 0; i < MAX_SLOTS; i++) d->slot_last[i] = 0;
        ReleaseSRWLockExclusive(&d->lock);
    }
    RET(0);
}
/* RegisterXDecoderBuffer(decoder, u32 index, pool, u32 image) / UnregisterXDecoderBuffer(decoder, u32 index) */
static void dec_register(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->slots[(uint32_t)ARG(1)] = ARG(2), d->busy[(uint32_t)ARG(1)] = 0;
    RET(0);
}
static void dec_unregister(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->slots[(uint32_t)ARG(1)] = 0;
    RET(0);
}
/* AcquireXDecoderOutputBuffer(u32* index, NativeSyncHandle*, decoder): the oldest picture into a free buffer */
static void dec_acquire(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(2), DEC_MAGIC);
    if (ARG(1)) G64(ARG(1)) = 0;
    if (!d) { RET(RESULT_NO_OUTPUT); return; }
    /* (the library aborts on any failure here: the hardware call waits for a picture. Up to ~100 ms for one decoded
       by another thread's submission; then a free buffer as it was, so the caller is never refused) */
    /* (hookshot, #1929) and no virtual core held through it: the decoding blocks (dav1d waits for its own frame
       threads), and with the core held the movie thread kept it 100-270 ms on Linux while a thread waiting for that core
       ran without one. It is taken again only once d->lock is let go, as in hos_cv_sleep: waiting for a core while
       holding the lock could stall a thread that holds a core and waits for the lock (dec_submit) */
    const int away = hos_vcore_away();
    AcquireSRWLockExclusive(&d->lock);
    decode_pending(d);
    /* (hookshot) The wait is for a picture another thread's submission is about to bring, ~100 ms of it. It was 50 tries of a
       2 ms sleep, and such a sleep takes 4-5 ms: 245-257 ms, on the thread a movie's end joins (title to game: the game's main
       thread in hos_thread_join for a frame of ~250 ms, every start). Now by the clock, and none after a Stop or a Flush with
       nothing submitted since: no picture is coming then. HOS_VIDEO_ACQUIRE_TRIES=1: 50 tries, as before */
    static int by_tries = -1;
    if (by_tries < 0) by_tries = env_on("HOS_VIDEO_ACQUIRE_TRIES");
    const int64_t wait_until = hos_now_ns() + 100000000;
    for (int tries = 0; !d->qn && !d->eos && (by_tries ? tries < 50 : !d->stopped && hos_now_ns() < wait_until); tries++) {
        ReleaseSRWLockExclusive(&d->lock);
        hos_sleep_ns(2000000);
        AcquireSRWLockExclusive(&d->lock);
        decode_pending(d);
    }
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS && slot < 0; i++)
        if (d->slots[i] && !d->busy[i] && host_of(d->slots[i], POOL_MAGIC)) slot = i;
    if (slot < 0) {
        ReleaseSRWLockExclusive(&d->lock);
        if (away) hos_vcore_back();
        RET(RESULT_NO_OUTPUT);
        return;
    }
    d->acquires++;
    if (!d->qn) {   /* (nothing decoded: the buffer's last picture again; after the last submission, as the last) */
        d->busy[slot] = 1;
        if (d->eos) d->slot_last[slot] = 1, d->slot_pts[slot] = d->last_pts;   /* (the end at the last picture's time) */
        const int idle = !d->eos && !d->phead;   /* (nothing will come into this buffer: its sync is not reached) */
        if (idle && ARG(1))
            G64(ARG(1)) = SYNC_IDLE_TAG | ((uint64_t)InterlockedCompareExchange64(&submissions, 0, 0) & SYNC_IDLE_MASK);
        if (video_log()) hos_log("video: acquire %lld: nothing decoded%s, buffer %d again", (long long)d->acquires,
                                 d->eos ? " (the end)" : idle ? " (idle: its sync not reached)" : "", slot);
        else if (d->acquires == 1 && !d->eos)   /* (the start: the frame shows the buffer as it was, black since the pool's fill) */
            hos_log("video: the first buffer went out before the first picture was decoded (shown black)");
        ReleaseSRWLockExclusive(&d->lock);
        if (away) hos_vcore_back();
        if (ARG(0)) G32(ARG(0)) = (uint32_t)slot;
        RET(0);
        return;
    }
    {   /* (hookshot) The game takes one picture a frame, and the sound plays on: under 60 fps the movie fell behind its
           sound (the intro "slow motion, audio ahead", issues 146 and 179). Pictures behind the movie's clock are skipped:
           the next ones decoded, as the whole movie is submitted at once. HOS_VIDEO_NO_CATCHUP=1: as before */
        static int off = -1;
        if (off < 0) off = env_on("HOS_VIDEO_NO_CATCHUP");
        const int64_t now = hos_now_ns(), late = 25000000;   /* (a frame and a half at 60 fps) */
        Picture *h = &d->queue[d->qhead];
        if (!d->clock_set || h->pts < d->clock_pts) d->clock_set = 1, d->clock_ns = now, d->clock_pts = h->pts;   /* (a start, a loop) */
        int skipped = 0;
        for (int guard = 0; !off && guard < 600 && !d->queue[d->qhead].last; guard++) {
            const int64_t due = d->clock_pts + (now - d->clock_ns);
            if (d->queue[d->qhead].pts + late >= due) break;
            if (d->qn < 2) {
                if (!d->phead) break;
                decode_next(d);
                continue;
            }
            picture_free(&d->queue[d->qhead]);
            d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
            skipped++;
        }
        d->skipped += skipped;
        if (skipped && video_log()) hos_log("video: %d pictures skipped to catch up with the movie's clock", skipped);
    }
    Picture *p = &d->queue[d->qhead];
    Pool *o = (Pool *)host_of(d->slots[slot], POOL_MAGIC);
    /* (the picture at the pool's size: rows and columns past either are left out) */
    const int ok = o->nv12 != NULL && p->nv12 != NULL;   /* (an allocation that failed: nothing copied; issue 342) */
    const int w = !ok ? 0 : p->w < o->w ? p->w : o->w, h = !ok ? 0 : p->h < o->h ? p->h : o->h;
    for (int y = 0; y < h; y++) {
        if (p->rgba && o->rgba) memcpy(o->rgba + (size_t)y * o->w * 4, p->rgba + (size_t)y * p->w * 4, (size_t)w * 4);
        memcpy(o->nv12 + (size_t)y * o->w, p->nv12 + (size_t)y * p->w, (size_t)w);
    }
    for (int y = 0; y < h / 2; y++)
        memcpy(o->nv12 + (size_t)o->w * o->h + (size_t)y * o->w, p->nv12 + (size_t)p->w * p->h + (size_t)y * p->w,
               (size_t)w);
    o->rgba_stale = !p->rgba;
    /* (hookshot) and into the pool's guest memory, as the hardware decoder leaves it (NV12, pitch = width, chroma right
       after the luma rows; RGBA when the memory has room for it and the pool's format is not NV12's). Not once the
       game reads the pool through ReadDecoderOutputImageNv12 / Rgba, as OoT's movie library does: its pools are
       format 3, so each 1080p picture was made RGBA and copied there for nothing (the movie is the same without it) */
    if (o->mem && o->mem_size && !o->read_by_api) {
        const size_t nv12_size = (size_t)o->w * o->h * 3 / 2, rgba_size = (size_t)o->w * o->h * 4;
        const int rgba = o->format != 0 && o->mem_size >= rgba_size;   /* (VideoDecoderOutputFormat 0: NV12) */
        const size_t need = rgba ? rgba_size : nv12_size;
        void *to = hos_guest_range(o->mem, need);   /* (hookshot: only into guest memory) */
        if (o->mem_size >= need && ok && to && (rgba ? o->rgba != NULL : 1)) {
            if (rgba && o->rgba_stale) pool_rgba(o);
            memcpy(to, rgba ? o->rgba : o->nv12, need);
            HOS_WRITTEN(to, need);
        }
    }
    d->slot_pts[slot] = d->last_pts = p->pts;
    d->slot_last[slot] = p->last;
    d->busy[slot] = 1;
    {   /* (hookshot) HOS_VIDEO_LOG: the picture's time against this decoder's movie clock, to see it fall behind the sound */
        if (video_log() && d->acquires % 60 == 0) {
            const int64_t now = hos_now_ns();
            hos_log("video: acquire %lld: picture at %.2f s, wall clock %.2f s (behind by %.2f s), %d decoded ahead", (long long)d->acquires,
                    (double)(p->pts - d->clock_pts) / 1e9, (double)(now - d->clock_ns) / 1e9,
                    (double)(now - d->clock_ns - (p->pts - d->clock_pts)) / 1e9, d->qn);
        }
    }
    if (video_log() && (d->acquires <= 8 || p->last || d->acquires % 300 == 0)) {
        uint64_t sum = 0, n = 0;   /* (the picture's mean luma and RGB, sampled: a black or garbage decode shows) */
        uint64_t rgb[3] = {0, 0, 0};
        for (int y = 0; y < p->h; y += 37)
            for (int x = 0; x < p->w; x += 41, n++) {
                sum += p->nv12[(size_t)y * p->w + x];
                for (int k = 0; k < 3 && p->rgba; k++) rgb[k] += p->rgba[((size_t)y * p->w + x) * 4 + k];
            }
        hos_log("video: acquire %lld: %dx%d pts %lld%s into buffer %d (mean Y %llu, RGB %llu %llu %llu)",
                (long long)d->acquires, p->w, p->h, (long long)p->pts, p->last ? " (last)" : "", slot,
                (unsigned long long)(n ? sum / n : 0), (unsigned long long)(n ? rgb[0] / n : 0),
                (unsigned long long)(n ? rgb[1] / n : 0), (unsigned long long)(n ? rgb[2] / n : 0));
    }
    picture_free(p);
    d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
    ReleaseSRWLockExclusive(&d->lock);
    if (away) hos_vcore_back();
    if (ARG(0)) G32(ARG(0)) = (uint32_t)slot;
    RET(0);
}
/* ReleaseXDecoderOutputBuffer(decoder, u32 index, NativeSyncHandle) */
static void dec_release(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(0), DEC_MAGIC);
    if (d && (uint32_t)ARG(1) < MAX_SLOTS) d->busy[(uint32_t)ARG(1)] = 0;
    RET(0);
}
/* GetXDecoderOutputBufferProperties(u16* w, u16* h, u16* display w, u16* display h, TimeSpan* pts, bool* last,
   decoder, u32 index) */
static void dec_properties(Ctx *c) {
    Dec *d = (Dec *)host_of(ARG(6), DEC_MAGIC);
    const uint32_t i = (uint32_t)ARG(7);
    const Pool *o = d && i < MAX_SLOTS ? (const Pool *)host_of(d->slots[i], POOL_MAGIC) : NULL;
    const uint16_t w = o ? (uint16_t)o->w : 0, h = o ? (uint16_t)o->h : 0;
    if (ARG(0)) *(uint16_t *)GP(ARG(0)) = w;
    if (ARG(1)) *(uint16_t *)GP(ARG(1)) = h;
    if (ARG(2)) *(uint16_t *)GP(ARG(2)) = w;
    if (ARG(3)) *(uint16_t *)GP(ARG(3)) = h;
    if (ARG(4)) G64(ARG(4)) = d && i < MAX_SLOTS ? (uint64_t)d->slot_pts[i] : 0;
    if (ARG(5)) *(uint8_t *)GP(ARG(5)) = d && i < MAX_SLOTS ? (uint8_t)d->slot_last[i] : 0;
    {   /* (HOS_VIDEO_LOG) the first times the game is told a picture is the last */
        static volatile LONG n;
        if (video_log() && d && i < MAX_SLOTS && d->slot_last[i] && InterlockedIncrement(&n) <= 8)
            hos_log("video: properties of buffer %u: the last picture, pts %lld (lr %#llx)", i, (long long)d->slot_pts[i],
                    (unsigned long long)c->x[30]);
    }
    RET(0);
}
static uint64_t ready_event(void) {
    static uint64_t event;
    if (!event) {
        event = hos_sys_alloc(0x40, 16);
        hos_system_event_init(event, 0);
        hos_system_event_signal(event);
    }
    return event;
}

/* VP9 */
HLE(_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP9, "VP9"); }
HLE(_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP9, "VP9"); }
HLE(_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec26UnregisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEj) { dec_unregister(c); }
HLE(_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE) { RET(ready_event()); }
HLE(_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec36SubmitEncryptedVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb) {
    const uint64_t id = submitted();
    if (ARG(0)) G64(ARG(0)) = id;
    RET(0);
}
HLE(_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj) { dec_properties(c); }

/* (hookshot) AV1: OoT's movies. FFmpeg's AV1 decoder if the build has one, else dav1d (host/dav1d_av1.c) when the build has
   it, else Media Foundation (host/mf_av1.c).
   Initialize...ByTransferMemory(decoder, memory, size, ?, u16 width, u16 height, ...), ...ByRemoteMemory(decoder,
   size, u16 width, u16 height, ...) */
HLE(_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) {
    dec_init_sized(c, AV_CODEC_ID_AV1, "AV1", (int)(uint16_t)ARG(4), (int)(uint16_t)ARG(5));
}
HLE(_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) {
    dec_init_sized(c, AV_CODEC_ID_AV1, "AV1", (int)(uint16_t)ARG(2), (int)(uint16_t)ARG(3));
}
HLE(_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE) { RET(ready_event()); }
HLE(_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj) { dec_properties(c); }

/* VP8 */
HLE(_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP8, "VP8"); }
HLE(_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE) { dec_init(c, AV_CODEC_ID_VP8, "VP8"); }
HLE(_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_finalize(c); }
HLE(_ZN2nn5codec24RegisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj) { dec_register(c); }
HLE(_ZN2nn5codec26UnregisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEj) { dec_unregister(c); }
HLE(_ZN2nn5codec15StartVp8DecoderEPNS0_14Vp8DecoderTypeE) { RET(0); }
HLE(_ZN2nn5codec14StopVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_flush(c, "Stop"); }
HLE(_ZN2nn5codec15FlushVp8DecoderEPNS0_14Vp8DecoderTypeE) { dec_flush(c, "Flush"); }
HLE(_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE) { RET(ready_event()); }
HLE(_ZN2nn5codec27SubmitVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKvmNS_8TimeSpanEb) { dec_submit(c); }
HLE(_ZN2nn5codec29AcquireVp8DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp8DecoderTypeE) { dec_acquire(c); }
HLE(_ZN2nn5codec29ReleaseVp8DecoderOutputBufferEPNS0_14Vp8DecoderTypeEjNS_2gs16NativeSyncHandleE) { dec_release(c); }
HLE(_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj) { dec_properties(c); }
