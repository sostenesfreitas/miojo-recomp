/* (hookshot) An AV1 decoder on dav1d, for nn::codec's AV1 decoder (host/nn_video.c), on Windows.

   OoT's movies (romfs/Movie: the intro, ...) are 1080p AV1, and the static FFmpeg in the CPM cache has no AV1 decoder. Without
   this they go to Media Foundation (host/mf_av1.c): the synchronous decoder of Microsoft's AV1 Video Extension, its software
   one, whatever the GPU, and a Store download where it is missing (issue 380). Movies stutter and fall behind their sound
   there (issue 2316, an RTX 4060; a GTX 1050). dav1d is what Linux decodes them with (through FFmpeg). Built in when the build
   finds dav1d in the CPM cache (HOOKSHOT_DAV1D: nx2/build.py, scripts/windows/dav1d.sh); without it, Media Foundation as before.

   The same interface as mf_av1.c: dav1d_av1_decode takes one temporal unit (or NULL at the end, which drains) and calls back
   once per picture with NV12, which nn_video.c keeps as it is (no RGBA unless the game reads one).
   HOS_DAV1D_THREADS=<n>: its threads (default: dav1d's, one per logical core). HOS_NO_DAV1D=1: Media Foundation instead. */
#include <dav1d/dav1d.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../gpu/vk/compat/hookshot_env.h"   /* (env_on, env_int: the switches' one rule) */

void hos_log(const char *fmt, ...);

/* a picture: luma rows and the interleaved chroma rows, both `stride` bytes apart */
typedef void (*dav1d_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);

typedef struct {
    Dav1dContext *ctx;
    uint8_t *nv12;   /* (the picture handed to the callback, made again only when the size changes) */
    size_t nv12_size;
} Dav1dAv1;

void *dav1d_av1_open(int w, int h) {
    (void)w, (void)h;
    if (env_on("HOS_NO_DAV1D")) return NULL;
    Dav1dSettings s;
    dav1d_default_settings(&s);
    if (env_int("HOS_DAV1D_THREADS", 0) > 0) s.n_threads = env_int("HOS_DAV1D_THREADS", 0);
    s.logger.callback = NULL;   /* (its messages are not the game's log) */
    Dav1dAv1 *d = (Dav1dAv1 *)calloc(1, sizeof *d);
    if (!d) return NULL;
    if (dav1d_open(&d->ctx, &s) < 0) {
        hos_log("video: dav1d %s did not start: AV1 on Media Foundation instead", dav1d_version());
        free(d);
        return NULL;
    }
    static int logged;
    if (!logged++) {
        if (s.n_threads) hos_log("video: AV1 on dav1d %s (%d threads)", dav1d_version(), s.n_threads);
        else hos_log("video: AV1 on dav1d %s (a thread per logical core)", dav1d_version());
    }
    return d;
}

/* a decoded picture as NV12, to the callback: 8 bits per sample as it is, deeper ones shifted down; 4:0:0 with grey chroma */
static void deliver(Dav1dAv1 *d, Dav1dPicture *p, dav1d_av1_picture_fn fn, void *user) {
    const int w = p->p.w & ~1, h = p->p.h & ~1, shift = p->p.bpc > 8 ? p->p.bpc - 8 : 0;
    if (w <= 0 || h <= 0) return;
    const size_t need = (size_t)w * h * 3 / 2;
    if (need > d->nv12_size) {
        uint8_t *n = (uint8_t *)realloc(d->nv12, need);
        if (!n) return;   /* (out of memory: this picture is dropped) */
        d->nv12 = n, d->nv12_size = need;
    }
    uint8_t *y = d->nv12, *uv = d->nv12 + (size_t)w * h;
    const ptrdiff_t ys = p->stride[0], cs = p->stride[1];
    for (int r = 0; r < h; r++) {
        const uint8_t *src = (const uint8_t *)p->data[0] + r * ys;
        if (!shift) memcpy(y + (size_t)r * w, src, (size_t)w);
        else for (int x = 0; x < w; x++) y[(size_t)r * w + x] = (uint8_t)(((const uint16_t *)src)[x] >> shift);
    }
    const int cw = w / 2, ch = h / 2;
    const int sub_x = p->p.layout != DAV1D_PIXEL_LAYOUT_I444, sub_y = p->p.layout == DAV1D_PIXEL_LAYOUT_I420;
    for (int r = 0; r < ch; r++) {
        uint8_t *row = uv + (size_t)r * w;
        if (p->p.layout == DAV1D_PIXEL_LAYOUT_I400) { memset(row, 0x80, (size_t)w); continue; }
        const int sr = sub_y ? r : 2 * r;
        const uint8_t *u = (const uint8_t *)p->data[1] + sr * cs, *v = (const uint8_t *)p->data[2] + sr * cs;
        for (int x = 0; x < cw; x++) {
            const int sx = sub_x ? x : 2 * x;
            row[2 * x] = shift ? (uint8_t)(((const uint16_t *)u)[sx] >> shift) : u[sx];
            row[2 * x + 1] = shift ? (uint8_t)(((const uint16_t *)v)[sx] >> shift) : v[sx];
        }
    }
    fn(user, y, uv, w, h, w, p->m.timestamp);
}

/* up to `max` pictures to the callback (max < 0: all dav1d still holds), how many came. (One per submission: a second
   dav1d_get_picture with no data sent in between drains, waiting for every frame in flight, and its frame threads then
   decode one picture at a time. A temporal unit shows one picture, so one per submission keeps up) */
static int pictures(Dav1dAv1 *d, dav1d_av1_picture_fn fn, void *user, int max) {
    int n = 0;
    for (; max < 0 || n < max; n++) {
        Dav1dPicture p;
        memset(&p, 0, sizeof p);
        const int r = dav1d_get_picture(d->ctx, &p);
        if (r < 0) {
            if (r != DAV1D_ERR(EAGAIN)) {
                static int logged;
                if (logged++ < 4) hos_log("video: dav1d could not decode a picture (%d)", r);
            }
            break;
        }
        deliver(d, &p, fn, user);
        dav1d_picture_unref(&p);
    }
    return n;
}

void dav1d_av1_decode(void *h, const uint8_t *data, size_t n, int64_t pts, dav1d_av1_picture_fn fn, void *user) {
    Dav1dAv1 *d = (Dav1dAv1 *)h;
    if (!d) return;
    if (!data || !n) {   /* (the end: the pictures dav1d still holds) */
        pictures(d, fn, user, -1);
        return;
    }
    Dav1dData in;
    memset(&in, 0, sizeof in);
    uint8_t *buf = dav1d_data_create(&in, n);
    if (!buf) return;
    memcpy(buf, data, n);
    in.m.timestamp = pts;
    for (int tries = 0; in.sz; tries++) {
        const int r = tries < 64 ? dav1d_send_data(d->ctx, &in) : DAV1D_ERR(EINVAL);
        if (r == DAV1D_ERR(EAGAIN)) {   /* (its queue full: a picture out first, waiting for one if none is ready, then the rest) */
            if (!pictures(d, fn, user, 1)) pictures(d, fn, user, 1);
            continue;
        }
        if (r < 0) {
            static int logged;
            if (logged++ < 4) hos_log("video: dav1d refused %zu bytes (%d)", n, r);
            dav1d_data_unref(&in);
            break;
        }
    }
    pictures(d, fn, user, 1);
}

void dav1d_av1_flush(void *h) {
    Dav1dAv1 *d = (Dav1dAv1 *)h;
    if (d) dav1d_flush(d->ctx);
}

void dav1d_av1_close(void *h) {
    Dav1dAv1 *d = (Dav1dAv1 *)h;
    if (!d) return;
    dav1d_close(&d->ctx);
    free(d->nv12);
    free(d);
}
