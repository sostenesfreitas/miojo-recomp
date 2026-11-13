/* (hookshot) The game's zstd stream decode on the host's libzstd (third_party/zstd 1.5.7): HOS_NATIVE_ZSTD=1.

   The game decodes its .zs files on ParallelDecompThread: a job loop (f_80115bf0) hands each compressed chunk it has read to
   f_80115c80, the game's own wrapper around zstd's buffer-less streaming API (decompressBegin_usingDDict,
   nextSrcSizeToDecompress, decompressContinue, inlined in it), with a staging buffer for a block split between two chunks.
   Lifted, that is 11-17% of a core while the game streams, and during loads it runs for more than 250 ms with no OS call,
   holding a virtual core.

   nx2/oot_patches.py puts hos_zstd_stream(c) first in f_80115c80. Off (the default) it answers 0 and the lifted function
   runs. On, it does what f_80115c80 does with the same calls into libzstd and answers 1 (x0 the result):
     - the stream object it is given (x0, S) is the game's, and every field the game reads is written as the game writes it:
       state byte (+0: 1 start, 2 decoding, 3 failed, 4 done), output pointer (+8), output room (+16, signed 32-bit), bytes
       still to come (+20, the result), staging buffer (+32), its size (+40) and fill (+44), and the context pointer (+48:
       the workspace at +24, as the game sets it);
     - results: what is still to come, 0 once the frame's content is all out, -1 on an error, -2 when the output room ran
       out (zstd's dstSize_tooSmall), -3 for input after the frame's end; a bad header or an unknown content size fails at
       the start, as the game's does;
     - dictionaries: the game's DDict (x3) gives its content, size and kind (entropy tables or raw content); a libzstd DDict
       made from the same bytes in place (by reference) is used, and its dictID must be the game's;
     - checksums are checked as the game checks them (libzstd's default);
     - the game's context workspace is not written: only the decoder's own functions read it.
   A libzstd context lives with each stream object that started on the host (by its guest address), so a stream never
   changes decoder in its middle. A stream that cannot start on the host (a misaligned workspace, no free entry, a
   dictionary libzstd does not take) runs lifted from its start, as before.

   HOS_NATIVE_ZSTD=1 also gives the thread's virtual core up while libzstd runs (as a sleep does: hos_vcore_away/back), so
   the game's other threads can have it; HOS_NATIVE_ZSTD=2 decodes on the host and keeps the core.

   What differs from the game's decoder:
     - the up to 32 bytes of output room just past what is decoded: both decoders' fast copies may over-write there (the
       next block overwrites them), by different amounts, so they can hold other leftover bytes than the game's leaves;
     - frames the format forbids: libzstd 1.5.7 refuses a block larger than the frame's window (the game's older copy
       decodes it), and a raw block with a NULL output fails as dstBuffer_null (-1) where the game's says dstSize_tooSmall
       (-2). A zstd compressor makes neither. */
#include "hos.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"

#define ZS_STREAMS 64   /* stream objects decoding on the host at once (the game has a few decode threads) */
#define ZS_DICTS 64     /* libzstd DDicts kept for the game's */

/* the game's DDict: +8 its content, +16 the size, +0x18 up to +0x6840 the entropy tables it built (repeat offsets at
   +0x6834), +0x6840 the dictID, +0x6844 whether it has entropy tables */
#define ZS_TABLES 0x18
#define ZS_TABLES_END 0x6840
typedef struct ZsDict {
    uint64_t guest, content, size;   /* the game's DDict, its content and size */
    uint32_t id, entropy;            /* its dictID and entropy flag */
    uint8_t *tables;                 /* its tables as they were: the same ones, the same dictionary (a DDict made again in
                                        place for another dictionary with the same size and ID has other tables) */
    ZSTD_DDict *dd;
    int refs, live;                  /* streams using it; live 0 once the game's DDict at that address is another */
} ZsDict;
typedef struct ZsStream {
    uint64_t guest;   /* the stream object's guest address; 0: a free entry */
    ZSTD_DCtx *z;
    ZsDict *dict;
} ZsStream;

static ZsStream zs_stream[ZS_STREAMS];
static ZsDict zs_dict[ZS_DICTS];
static ZSTD_DCtx *zs_spare[ZS_STREAMS];   /* finished streams' contexts, for the next ones */
static int zs_nspare, zs_used, zs_peak;   /* (entries in use, and the most so far: logged) */
static volatile int zs_lock_word;   /* (the tables only: never held while decoding or across a core change) */
static void zs_lock(void) {
    while (__atomic_exchange_n(&zs_lock_word, 1, __ATOMIC_ACQUIRE)) {
    }
}
static void zs_unlock(void) { __atomic_store_n(&zs_lock_word, 0, __ATOMIC_RELEASE); }

static int zs_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const int m = env_on("HOS_NATIVE_ZSTD") ? (env_int("HOS_NATIVE_ZSTD", 1) == 2 ? 2 : 1) : 0;
        if (m)
            hos_log("zstd: the game's stream decode on the host's libzstd %s (HOS_NATIVE_ZSTD=%d%s)", ZSTD_versionString(), m,
                    m == 1 ? ": the core given up while it decodes" : "");
        mode = m;
    }
    return mode;
}

static void zs_note(const char *why) {   /* (each kind of fallback logged once) */
    static const char *seen[8];
    for (int i = 0; i < 8; i++) {
        if (seen[i] == why) return;
        if (!seen[i]) {
            seen[i] = why;
            hos_log("zstd: a stream decodes lifted: %s", why);
            return;
        }
    }
}

/* (holding zs_lock) */
static void zs_dict_free(ZsDict *d) {
    ZSTD_freeDDict(d->dd);
    free(d->tables);
    memset(d, 0, sizeof *d);
}
static void zs_dict_put(ZsDict *d) {
    if (d && --d->refs == 0 && !d->live) zs_dict_free(d);
}
static void zs_free(ZsStream *s) {
    zs_used--;
    zs_dict_put(s->dict);
    if (zs_nspare < ZS_STREAMS) zs_spare[zs_nspare++] = s->z;
    else ZSTD_freeDCtx(s->z);
    memset(s, 0, sizeof *s);
}

/* (holding zs_lock) the game's DDict at guest address g as a libzstd one, a reference taken; NULL when it cannot be */
static ZsDict *zs_dict_get(uint64_t g) {
    const uint64_t content = G64(g + 8), size = G64(g + 16);
    const uint32_t id = G32(g + 0x6840), entropy = G32(g + 0x6844);
    const uint8_t *tables = (const uint8_t *)GP(g + ZS_TABLES);
    ZsDict *spot = NULL;
    for (int i = 0; i < ZS_DICTS; i++) {
        ZsDict *d = &zs_dict[i];
        if (!d->dd) {
            if (!spot) spot = d;
            continue;
        }
        if (!d->live || d->guest != g) continue;
        if (d->content == content && d->size == size && d->id == id && d->entropy == entropy &&
            !memcmp(d->tables, tables, ZS_TABLES_END - ZS_TABLES)) {
            d->refs++;
            return d;
        }
        d->live = 0;   /* the game made another DDict here: this one goes with its last stream */
        if (!d->refs) {
            zs_dict_free(d);
            if (!spot) spot = d;
        }
    }
    if (!spot) return NULL;
    ZSTD_DDict *dd = ZSTD_createDDict_advanced(GP(content), (size_t)size, ZSTD_dlm_byRef,
                                               entropy ? ZSTD_dct_fullDict : ZSTD_dct_rawContent, ZSTD_defaultCMem);
    uint8_t *copy = malloc(ZS_TABLES_END - ZS_TABLES);
    if (!dd || !copy || ZSTD_getDictID_fromDDict(dd) != id) {
        ZSTD_freeDDict(dd);
        free(copy);
        return NULL;
    }
    memcpy(copy, tables, ZS_TABLES_END - ZS_TABLES);
    *spot = (ZsDict){g, content, size, id, entropy, copy, dd, 1, 1};
    return spot;
}

/* stream object g's entry, or NULL (it decodes lifted). start: a new frame, the entry made or reset and its dictionary
   (ddict, the game's, or 0) taken; *why says why not.
   An entry goes when its stream fails or ends, or is reset when its stream object starts the next frame; nothing else
   frees one (a stream whose object the game no longer uses keeps its entry: only the game knows, and its memory may be
   gone). With all of them in use a new stream decodes lifted, as before. One thread calls for a stream object at a time
   (the game's job loop), so an entry is never used by two calls at once. */
static ZsStream *zs_take(uint64_t g, int start, uint64_t ddict, const char **why) {
    zs_lock();
    ZsStream *s = NULL, *spot = NULL;
    int peak = 0;
    for (int i = 0; i < ZS_STREAMS && !s; i++) {
        if (zs_stream[i].guest == g) s = &zs_stream[i];
        else if (!zs_stream[i].guest && !spot) spot = &zs_stream[i];
    }
    if (!start) {
        zs_unlock();
        return s;
    }
    if (!s) {
        s = spot;
        if (!s) {
            zs_unlock();
            *why = "all host entries in use";
            return NULL;
        }
        s->z = zs_nspare ? zs_spare[--zs_nspare] : ZSTD_createDCtx();
        if (!s->z) {
            zs_unlock();
            *why = "no libzstd context";
            return NULL;
        }
        s->guest = g;
        if (++zs_used > zs_peak) peak = zs_peak = zs_used;
    }
    zs_dict_put(s->dict);
    s->dict = NULL;
    if (ddict && !(s->dict = zs_dict_get(ddict))) {
        zs_free(s);
        zs_unlock();
        *why = "a dictionary libzstd does not take as the game's";
        return NULL;
    }
    zs_unlock();
    if (peak) hos_log("zstd: %d stream(s) decoding on the host at once", peak);   /* (each new most) */
    return s;
}
static void zs_give(ZsStream *s, int finished) {
    if (!finished) return;
    zs_lock();
    zs_free(s);
    zs_unlock();
}
static void zs_drop(uint64_t g) {   /* (stream object g's context, if it has one, goes) */
    ZsStream *s = zs_take(g, 0, 0, NULL);
    if (s) zs_give(s, 1);
}

/* the stream object's fields */
#define S_STATE(g) (*(uint8_t *)GP(g))
#define S_DST(g) G64((g) + 8)
#define S_ROOM(g) G32((g) + 16)
#define S_LEFT(g) G32((g) + 20)
#define S_WORK(g) G64((g) + 24)
#define S_STAGE(g) G64((g) + 32)
#define S_STAGE_CAP(g) G32((g) + 40)
#define S_STAGED(g) G32((g) + 44)
#define S_CTX(g) G64((g) + 48)

/* one decompressContinue of n bytes at src, its output added to the output fields as the game adds it */
static size_t zs_continue(ZSTD_DCtx *z, uint64_t g, uint64_t src, size_t n) {
    const uint64_t dst = S_DST(g);
    const size_t room = (size_t)(int64_t)(int32_t)S_ROOM(g);
    const size_t r = ZSTD_decompressContinue(z, GP(dst), room, GP(src), n);
    /* (marked also when it fails: a block that ran out of room was partly written. libzstd may keep a block's literals in
       the output room past the block, up to two blocks on: marked too) */
    const size_t out = ZSTD_isError(r) ? 0 : r, span = out > (2u << 17) + 64 ? out : (2u << 17) + 64;
    HOS_WRITTEN(GP(dst), span < room ? span : room);
    if (ZSTD_isError(r)) return r;
    S_DST(g) = dst + (uint64_t)(int64_t)(int32_t)r;
    S_ROOM(g) -= (uint32_t)r;
    S_LEFT(g) -= (uint32_t)r;
    return r;
}

/* f_80115c80's loop over one chunk (src, n) of a started stream: its result */
static int32_t zs_chunk(ZSTD_DCtx *z, uint64_t g, uint64_t src, int32_t n) {
    while (n >= 1) {
        const size_t want = ZSTD_nextSrcSizeToDecompress(z);
        if (want == 0) {   /* (input after the frame's end) */
            S_STATE(g) = 3;
            return -3;
        }
        const int32_t w = (int32_t)want;
        int32_t staged = (int32_t)S_STAGED(g);
        size_t r;
        if (staged >= 1) {   /* a piece begun in an earlier chunk: the staging buffer filled up to it */
            if (w > staged) {
                const int32_t need = w - staged, take = need < n ? need : n;
                memcpy(GP(S_STAGE(g) + (uint32_t)staged), GP(src), (uint32_t)take);
                HOS_WRITTEN(GP(S_STAGE(g) + (uint32_t)staged), (uint32_t)take);
                staged += take;
                S_STAGED(g) = (uint32_t)staged;
                src += (uint32_t)take;
                n -= take;
            }
            if (staged < w) continue;
            r = zs_continue(z, g, S_STAGE(g), want);
            if (ZSTD_isError(r)) goto fail;
            staged = (int32_t)S_STAGED(g);
            if (staged != w) {
                memmove(GP(S_STAGE(g)), GP(S_STAGE(g) + want), (size_t)(int64_t)(staged - w));
                HOS_WRITTEN(GP(S_STAGE(g)), (size_t)(int64_t)(staged - w));
            }
            S_STAGED(g) = (uint32_t)(staged - w);
            continue;
        }
        if (n < w) {   /* the piece goes on in the next chunk: what is here to the staging buffer */
            if ((int32_t)S_STAGE_CAP(g) < w) {
                S_STATE(g) = 3;
                return -1;
            }
            memcpy(GP(S_STAGE(g)), GP(src), (uint32_t)n);
            HOS_WRITTEN(GP(S_STAGE(g)), (uint32_t)n);
            S_STAGED(g) = (uint32_t)n;
            break;
        }
        r = zs_continue(z, g, src, want);
        if (ZSTD_isError(r)) goto fail;
        src += want;
        n -= w;
        continue;
    fail:
        S_STATE(g) = 3;
        return ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? -2 : -1;
    }
    if (ZSTD_nextSrcSizeToDecompress(z) == 0) S_STATE(g) = 4;
    return (int32_t)S_LEFT(g);
}

/* (nx2/oot_patches.py: first in f_80115c80) 1 when the call was done here, x0 its result */
int hos_zstd_stream(Ctx *c) {
    const int mode = zs_mode();
    if (!mode) return 0;
    const uint64_t g = c->x[0], src = c->x[1], ddict = c->x[3];
    const int32_t n = (int32_t)c->x[2];
    const uint8_t st = S_STATE(g);
    ZsStream *s;
    if (st == 3 || st == 4) {   /* (the game answers these without its context: a host one goes now) */
        zs_drop(g);
        return 0;
    }
    if (st == 1) {   /* a new frame: its header first, read as the game reads it */
        ZSTD_FrameHeader fh;
        const size_t h = ZSTD_getFrameHeader_advanced(&fh, GP(src), (uint32_t)n, ZSTD_f_zstd1);
        const uint64_t size = h ? (uint64_t)-2 : fh.frameType == ZSTD_skippableFrame ? 0 : fh.frameContentSize;
        const char *why = "the workspace is not 8-byte aligned";
        if (size == (uint64_t)-2 || size == ZSTD_CONTENTSIZE_UNKNOWN) {
            zs_drop(g);
            S_STATE(g) = 3;
            HOS_WRITTEN(GP(g), 1);
            c->x[0] = (uint32_t)-1;
            return 1;
        }
        if ((S_WORK(g) & 7) || !(s = zs_take(g, 1, ddict, &why))) {
            zs_drop(g);   /* (a context left from an earlier frame here) */
            zs_note(why);
            return 0;
        }
        S_LEFT(g) = (uint32_t)size;
        ZSTD_decompressBegin_usingDDict(s->z, s->dict ? s->dict->dd : NULL);
        S_CTX(g) = S_WORK(g);
        S_STATE(g) = 2;
    } else if (!(s = zs_take(g, 0, 0, NULL))) {
        return 0;   /* (a stream that started lifted) */
    }
    const int away = mode == 1 ? hos_vcore_away() : 0;
    const int32_t ret = zs_chunk(s->z, g, src, n);
    if (away) hos_vcore_back();
    HOS_WRITTEN(GP(g), 56);   /* (after its last write: a mark taken by the GPU mirror's scan in between would miss them) */
    zs_give(s, S_STATE(g) == 3 || S_STATE(g) == 4);
    c->x[0] = (uint32_t)ret;
    return 1;
}
