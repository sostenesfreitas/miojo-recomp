/* nn::audio: an HLE of the audio renderer at the API level, and its output.

   The game builds its sound graph through nn::audio calls on objects it owns (VoiceType, SubMixType, FinalMixType,
   SplitterType, DeviceSinkType, MemoryPoolType, effect types): each gets our handle in its first 8 bytes (AU_MAGIC,
   kind, index). A mixer thread renders one audio frame (240 samples at 48 kHz) every 5 ms:
     voices (PCM16, PCM float, DSP-ADPCM; resampled by sample rate x pitch, voice volume)
       -> their destination mix or splitter (per channel pair volumes)
       -> sub mixes in dependency order (sub mix volume, then to their destination)
       -> the final mix (volume) -> the device sink (its channel map: 5.1 as it is to a 5.1 device, else down to stereo)
       -> waveOut
   and signals the renderer event the game's audio thread waits on. I3DL2 reverb is processed locally; Aux effects
   exchange planar PCM32 with the game's CPU effect workers. Other effects remain incomplete.
   Played sample counts and released wave buffers go back to the game as on hardware.
   The game's parameter changes take effect together at RequestUpdateAudioRenderer, and volume changes ramp over a
   frame, as the console's renderer does them (see "parameter updates").
   HOS_AUDIO_DUMP=<file.wav>: also write what is played (checking without speakers); HOS_AUDIO=0: silence;
   HOS_AUDIO_DEVICE=<n>: that waveOut device first; HOS_AUDIO_MUTE=1: the device fed at volume 0 (test runs);
   HOS_AUDIO_CHANNELS=2 or 6: stereo or 5.1 whatever the default output has and the Insert menu's Audio output say
   (see au_speakers). */
#include "hos.h"
#include "../gpu/vk/vk_api.h"   /* (the Audio tab's options: HOS_OPT_VOLUME, HOS_OPT_MUTE_BACKGROUND) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#ifdef _WIN32
#include <mmreg.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#else
#include "hos_sdl.h"
#endif
#include "hos_paths.h"   /* (the game folder: hookshot_options.ini, au_menu_output) */
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

void hos_system_event_init(uint64_t key, int autoclear);
void hos_system_event_signal(uint64_t key);

#define FRAME 240            /* samples per audio frame (5 ms at 48 kHz) */
#define RATE 48000
#define AU_MAGIC 0x41554448u /* the high word of our handles in the game's objects */
#define MAX_VOICES 512
#define MAX_MIXES 96
#define MAX_SPLITTERS 128
#define MAX_SPLIT_DESTS 32
#define MAX_CH 8             /* channels of a voice */
#define MAX_BUF 24           /* buffers of a mix */
#define MAX_WB 32            /* queued wave buffers per voice */
#define MAX_REL 128

enum { K_VOICE = 1, K_MIX, K_SPLITTER, K_SINK, K_POOL, K_EFFECT, K_CONFIG, K_AUX };
enum { FMT_INVALID, FMT_PCM8, FMT_PCM16, FMT_PCM24, FMT_PCM32, FMT_FLOAT, FMT_ADPCM };
enum { PLAY_STARTED, PLAY_STOPPED, PLAY_PAUSED };
enum { DEST_NONE, DEST_MIX, DEST_SPLITTER };
typedef struct { uint64_t buffer, size; int32_t start, end; int loop; uint64_t context; } WB;

typedef struct {
    int used, released, final_mix, buffers;    /* (released: by the game, until the next update applies it) */
    float volume;
    int dest_kind, dest;                       /* (sub mixes) */
    float to[MAX_BUF][MAX_BUF];                /* [src buffer][dst buffer] volume to the destination mix */
    float buf[MAX_BUF][FRAME];
    int depth;                                 /* (processing order: deeper first) */
} Mix;

typedef struct {
    int used, released, channels, dests;
    int dest_mix[MAX_SPLIT_DESTS];
    float vol[MAX_SPLIT_DESTS][MAX_CH][MAX_BUF];
    float ramp[MAX_SPLIT_DESTS][MAX_CH][MAX_BUF];   /* the volumes the last frame ended with */
    int bq_on[MAX_SPLIT_DESTS][2];
    float bq_b[MAX_SPLIT_DESTS][2][3], bq_a[MAX_SPLIT_DESTS][2][2];
    uint64_t bq_epoch[MAX_SPLIT_DESTS][2];
} Splitter;

typedef struct {
    int used, released, rate, channels, format, state;
    uint64_t guest, param;                     /* the VoiceType; AdpcmParameter */
    int16_t coefs[16];                         /* ADPCM coefficients, copied when acquired (the game reuses the
                                                  parameter memory) */
    float volume, pitch;
    int dest_kind, dest;
    float vol[MAX_CH][MAX_BUF];                /* to the destination mix */
    float ramp_volume, ramp_vol[MAX_CH][MAX_BUF];   /* the volumes the last frame ended with (ramped to the above) */
    int snap;                                  /* started or a new destination: no ramp from the old volumes */
    uint64_t wb[MAX_WB];                       /* queued WaveBuffer*, oldest first (their identity) */
    WB wbc[MAX_WB];                            /* and their contents when appended */
    int nwb, pending_wb;                       /* (pending: appended, not yet applied by an update) */
    uint64_t rel[MAX_REL];                     /* released, for GetReleasedWaveBuffer */
    int nrel, nrel_seen;                       /* (seen: the first nrel_seen, released by the last update) */
    int64_t played;
    int64_t pos;                               /* sample frame within the current wave buffer */
    int started_wb;                            /* the current one's start was set up */
    double frac;
    float prev[MAX_CH], cur[MAX_CH];           /* resampler: the last two source frames */
    int16_t yn1, yn2;                          /* ADPCM history (mono) */
    int bq_on[2];                              /* the voice's two biquad filters (BiquadFilterParameter, Q14) */
    float bq_b[2][3], bq_a[2][2], bq_z[2][MAX_CH][2];
    /* Each source keeps its own history on each splitter route. Filtering one route must not change another's input. */
    float split_bq_z[MAX_SPLIT_DESTS][2][MAX_CH][2];
    uint64_t split_bq_epoch[MAX_SPLIT_DESTS][2];
    int dropped;
} Voice;

static CRITICAL_SECTION lock;
static int lock_ready;
static Voice voices[MAX_VOICES];
static Mix mixes[MAX_MIXES];
static Splitter splitters[MAX_SPLITTERS];
static uint64_t split_bq_epoch;
static int sink_mix = -1, sink_n;
static float lim_in = 1.0f, lim_out = 1.0f, lim_threshold = 1.0f, lim_env = 1.0f;   /* the light limiter */
static int lim_enabled;
static int8_t sink_in[8];

static void au_lock(void) {
    if (!lock_ready) { InitializeCriticalSection(&lock); lock_ready = 1; }
    EnterCriticalSection(&lock);
}
static void au_unlock(void) { LeaveCriticalSection(&lock); }

static void put_handle(uint64_t obj, int kind, int index) {
    if (obj) G64(obj) = (uint64_t)AU_MAGIC << 32 | (uint64_t)kind << 24 | (uint32_t)index;
}
static int au_trace = -1;   /* HOS_AUDIO_TRACE=1: the graph setup calls */
#define AUT(...) do { if (au_trace < 0) au_trace = env_on("HOS_AUDIO_TRACE"); if (au_trace) hos_log(__VA_ARGS__); } while (0)
static int get_handle(uint64_t obj, int kind) {
    if (!obj) return -1;
    const uint64_t h = G64(obj);
    if ((uint32_t)(h >> 32) != AU_MAGIC || (int)((h >> 24) & 0xFF) != kind) return -1;
    return (int)(h & 0xFFFFFF);
}
static int pool_index(uint64_t obj);   /* (a memory pool's handle, checked against the pool table: below) */
static float farg(Ctx *c, int n) {
    float f;
    memcpy(&f, &c->vreg[n][0], 4);
    return f;
}
/* The game's objects, by the handle in their first word; what the game released (until an update applies it) no
   longer counts. */
static int voice_index(uint64_t obj) {
    const int i = get_handle(obj, K_VOICE);
    return i >= 0 && i < MAX_VOICES && voices[i].used && !voices[i].released ? i : -1;
}
static Voice *voice_of(uint64_t obj) {
    const int i = voice_index(obj);
    return i >= 0 ? &voices[i] : NULL;
}
static int mix_index(uint64_t obj) {
    const int i = get_handle(obj, K_MIX);
    return i >= 0 && i < MAX_MIXES && mixes[i].used && !mixes[i].released ? i : -1;
}
static Mix *mix_of(uint64_t obj) {
    const int i = mix_index(obj);
    return i >= 0 ? &mixes[i] : NULL;
}
static int splitter_index(uint64_t obj) {
    const int i = get_handle(obj, K_SPLITTER);
    return i >= 0 && i < MAX_SPLITTERS && splitters[i].used && !splitters[i].released ? i : -1;
}
static Splitter *splitter_of(uint64_t obj) {
    const int i = splitter_index(obj);
    return i >= 0 ? &splitters[i] : NULL;
}

/* ---- wave buffers ---------------------------------------------------------------------------------------------
   nn::audio::WaveBuffer: buffer @0, size @8, startSampleOffset @0x10, endSampleOffset @0x14, loop @0x18,
   isEndOfStream @0x19, pContext @0x20 (AdpcmContext: predScale, history[2]), contextSize @0x28. Copied when
   appended (TOTK appends every buffer through one struct it refills); the pointer is only its identity for
   GetReleasedWaveBuffer. */
static WB read_wb(uint64_t p) {
    WB w;
    w.buffer = G64(p);
    w.size = G64(p + 8);
    w.start = (int32_t)G32(p + 0x10);
    w.end = (int32_t)G32(p + 0x14);
    w.loop = *(uint8_t *)GP(p + 0x18);
    w.context = G64(p + 0x20);
    return w;
}

/* one source frame (all channels) of the voice's current wave buffer at sample frame s; 0 when out of range */
static void adpcm_seek(Voice *v, const WB *w);
static int16_t adpcm_sample(Voice *v, const WB *w, int64_t s) {
    /* 8-byte frames of 14 samples: a header byte (coefficient pair index in 4-6, scale exponent in 0-3), then
       nibbles high first. Decoded in order: s must follow the previous call (adpcm_seek resets it). */
    const uint8_t *data = (const uint8_t *)GP(w->buffer);
    const int64_t frame = s / 14, in = s % 14;
    if ((uint64_t)(frame * 8 + 8) > w->size) return 0;
    const uint8_t header = data[frame * 8];
    const uint8_t byte = data[frame * 8 + 1 + in / 2];
    int nibble = in & 1 ? byte & 0xF : byte >> 4;
    if (nibble >= 8) nibble -= 16;
    const int idx = (header >> 4) & 7, scale = 1 << (header & 0xF);
    const int c1 = v->coefs[idx * 2], c2 = v->coefs[idx * 2 + 1];
    int32_t val = ((nibble * scale) << 11) + c1 * v->yn1 + c2 * v->yn2;
    val = (val + 1024) >> 11;
    if (val > 32767) val = 32767;
    if (val < -32768) val = -32768;
    v->yn2 = v->yn1;
    v->yn1 = (int16_t)val;
    return (int16_t)val;
}
static void adpcm_seek(Voice *v, const WB *w) {
    if (w->context) {   /* AdpcmContext { u16 predScale; s16 history[2]; } */
        const int16_t *ctx = (const int16_t *)GP(w->context);
        v->yn1 = ctx[1];
        v->yn2 = ctx[2];
    }
}
static void source_frame(Voice *v, const WB *w, int64_t s, float *out) {
    for (int ch = 0; ch < v->channels; ch++) out[ch] = 0;
    switch (v->format) {
    case FMT_PCM16: {
        const uint64_t off = (uint64_t)s * v->channels * 2;
        if (off + (uint64_t)v->channels * 2 > w->size) return;
        const int16_t *p = (const int16_t *)GP(w->buffer + off);
        for (int ch = 0; ch < v->channels; ch++) out[ch] = p[ch] * (1.0f / 32768.0f);
        break;
    }
    case FMT_FLOAT: {
        const uint64_t off = (uint64_t)s * v->channels * 4;
        if (off + (uint64_t)v->channels * 4 > w->size) return;
        const float *p = (const float *)GP(w->buffer + off);
        for (int ch = 0; ch < v->channels; ch++) out[ch] = p[ch];
        break;
    }
    case FMT_PCM8: {
        const uint64_t off = (uint64_t)s * v->channels;
        if (off + (uint64_t)v->channels > w->size) return;
        const int8_t *p = (const int8_t *)GP(w->buffer + off);
        for (int ch = 0; ch < v->channels; ch++) out[ch] = p[ch] * (1.0f / 128.0f);
        break;
    }
    case FMT_ADPCM:
        out[0] = adpcm_sample(v, w, s) * (1.0f / 32768.0f);
        break;
    default:
        break;
    }
}

static void release_wb(Voice *v, uint64_t wb) {
    if (v->nrel == MAX_REL) {
        memmove(v->rel, v->rel + 1, (size_t)(MAX_REL - 1) * sizeof v->rel[0]);
        v->nrel--;
        if (v->nrel_seen) v->nrel_seen--;
    }
    v->rel[v->nrel++] = wb;
}

/* the next source frame of the voice into v->cur; 0 when it ran out of wave buffers */
static int next_source_frame(Voice *v) {
    for (int guard = 0; guard < MAX_WB + 1; guard++) {
        if (!v->nwb) return 0;
        const WB w = v->wbc[0];
        if (!v->started_wb) {
            v->pos = w.start;
            v->started_wb = 1;
            if (v->format == FMT_ADPCM) {
                adpcm_seek(v, &w);
                for (int64_t s = (w.start / 14) * 14; s < w.start; s++) adpcm_sample(v, &w, s);   /* (mid-frame start) */
            }
        }
        if (v->pos < w.end) {
            float f[MAX_CH];
            source_frame(v, &w, v->pos, f);
            memcpy(v->cur, f, sizeof f);
            v->pos++;
            v->played++;
            return 1;
        }
        if (w.loop) {   /* (looping: back to the start, the same buffer stays) */
            v->started_wb = 0;
            if (w.end <= w.start) return 0;
            continue;
        }
        /* this buffer is done: released for the game */
        release_wb(v, v->wb[0]);
        memmove(v->wb, v->wb + 1, (size_t)(v->nwb - 1) * sizeof v->wb[0]);
        memmove(v->wbc, v->wbc + 1, (size_t)(v->nwb - 1) * sizeof v->wbc[0]);
        v->nwb--;
        v->started_wb = 0;
    }
    return 0;
}

/* ---- I3DL2 reverb (on a sub mix: its input buffers in place to its output buffers) ------------------------------
   Parameters as the game sets them (gains in millibels, times as nn::TimeSpan); the processing is ours, a small
   feedback delay network in the I3DL2 spirit: a pre-delay line feeding early reflection taps, then 4 delay lines
   (Hadamard feedback, high-frequency damping from the HF decay ratio), wet = room gain x (reflections + late),
   out = dry gain x in + wet. HOS_AUDIO_NO_REVERB=1: left out (dry). */
#define MAX_EFFECTS 16
#define REV_PRE (RATE / 2)          /* pre-delay line: 0.5 s */
#define REV_LINE 4096               /* each late line, at most ~85 ms */
typedef struct {
    int used, mix, enabled, n;
    uint64_t order;
    int8_t in[MAX_CH], out[MAX_CH];
    float room, room_hf, refl, reverb, hf_ratio, diffusion, density, hf_ref, dry;   /* (mB for the gains) */
    int64_t decay_ns, refl_delay_ns, late_delay_ns;
    float pre[REV_PRE];
    int pre_pos;
    float line[4][REV_LINE];
    int pos[4], len[4];
    float lp[4];                    /* damping filter states */
} Reverb;
static Reverb reverbs[MAX_EFFECTS];
static uint64_t effect_order;
static float mb_gain(float mb) { return powf(10.0f, mb / 2000.0f); }
static void reverb_frame(Reverb *rv, Mix *m) {
    static int off = -1;
    if (off < 0) off = env_on("HOS_AUDIO_NO_REVERB");
    if (off || !rv->enabled || rv->n <= 0) return;
    static const int base_len[4] = {1559, 1617, 1491, 1422};
    const float density = rv->density > 0 ? rv->density / 100.0f : 1.0f;
    for (int k = 0; k < 4; k++) {
        const int L = (int)(base_len[k] * (0.6f + 0.4f * density));
        rv->len[k] = L < 64 ? 64 : L >= REV_LINE ? REV_LINE - 1 : L;
        if (rv->pos[k] >= rv->len[k]) rv->pos[k] = 0;
    }
    const float decay = rv->decay_ns > 0 ? (float)rv->decay_ns / 1e9f : 1.5f;
    const float hf_ratio = rv->hf_ratio > 0.05f ? rv->hf_ratio : 0.5f;
    const float hf_ref = rv->hf_ref > 0 ? rv->hf_ref : 5000.0f;
    /* damping: a one-pole low pass whose cutoff falls with the HF decay ratio */
    const float cutoff = fminf(hf_ref * (0.5f + hf_ratio), 20000.0f);
    const float damp = expf(-2.0f * 3.14159265f * cutoff / RATE);
    const float room = rv->room > -10000 ? mb_gain(rv->room) : 0.0f;
    const float refl = rv->refl > -10000 ? mb_gain(rv->refl) : 0.0f;
    const float late = rv->reverb > -10000 ? mb_gain(rv->reverb) : 0.0f;
    const float dry = rv->dry;
    /* (hookshot) the game's delays are any 64-bit TimeSpan: clamped as doubles, before they become ints, and from both
       sides (a negative late delay read before the line) */
    const double pre_f = (double)rv->refl_delay_ns * RATE / 1e9, late_f = (double)rv->late_delay_ns * RATE / 1e9;
    const int pre_d = pre_f > 0 ? (pre_f < REV_PRE / 2 - 1 ? (int)pre_f : REV_PRE / 2 - 1) : 0;
    const double late_sum = pre_d + (late_f > 0 ? late_f : 0);
    const int late_d = late_sum < REV_PRE - 1 ? (int)late_sum : REV_PRE - 1;
    float g[4];
    for (int k = 0; k < 4; k++) g[k] = powf(10.0f, -3.0f * rv->len[k] / (RATE * decay));
    static const int er_tap[6] = {113, 397, 709, 1013, 1301, 1663};
    static const float er_gain[6] = {0.84f, -0.71f, 0.62f, -0.52f, 0.43f, -0.35f};
    const float diff = rv->diffusion > 0 ? fminf(rv->diffusion / 100.0f, 1.0f) : 1.0f;
    for (int i = 0; i < FRAME; i++) {
        float x = 0;
        for (int c = 0; c < rv->n; c++) {
            const int b = rv->in[c];
            if (b >= 0 && b < m->buffers) x += m->buf[b][i];
        }
        x /= (float)rv->n;
        if (!isfinite(x)) x = 0;   /* (a NaN would stay in the feedback lines for good) */
        rv->pre[rv->pre_pos] = x;
        float er = 0;
        for (int t = 0; t < 6; t++)
            er += er_gain[t] * rv->pre[(rv->pre_pos - pre_d - er_tap[t] + 2 * REV_PRE) % REV_PRE];
        er *= 0.4f;
        const float into_late = rv->pre[(rv->pre_pos - late_d + REV_PRE) % REV_PRE];
        rv->pre_pos = (rv->pre_pos + 1) % REV_PRE;
        float o[4];
        for (int k = 0; k < 4; k++) o[k] = rv->line[k][rv->pos[k]];
        /* Hadamard mix (orthonormal), damping, decay */
        const float hm[4] = {(o[0] + o[1] + o[2] + o[3]) * 0.5f, (o[0] - o[1] + o[2] - o[3]) * 0.5f,
                             (o[0] + o[1] - o[2] - o[3]) * 0.5f, (o[0] - o[1] - o[2] + o[3]) * 0.5f};
        for (int k = 0; k < 4; k++) {
            const float v = diff * hm[k] + (1.0f - diff) * o[k];
            rv->lp[k] = v * (1.0f - damp) + rv->lp[k] * damp;
            rv->line[k][rv->pos[k]] = into_late * 0.5f + rv->lp[k] * g[k];
            rv->pos[k] = (rv->pos[k] + 1) % rv->len[k];
        }
        for (int c = 0; c < rv->n; c++) {
            const int bi = rv->in[c], bo = rv->out[c];
            if (bo < 0 || bo >= m->buffers) continue;
            const float in_s = bi >= 0 && bi < m->buffers ? m->buf[bi][i] : 0.0f;
            const float sign = (c & 1) ? -1.0f : 1.0f;
            const float l = sign * o[c % 4] + o[(c + 1) % 4] * 0.5f;
            m->buf[bo][i] = dry * in_s + room * (refl * er * sign + late * l * 0.7f);
        }
    }
}
/* ---- Aux: the game supplies the processing, the renderer exchanges complete planar frames --------------------
   An enabled Aux REPLACES its output with the return signal. Passing its input through instead makes an effect
   send audible as another dry source. Each sample is an int32 in PCM16 units (1.0 in our mixer is 32768).
   All access is under the audio lock. The two guest allocations reserve 128 bytes before their sample data;
   cursor/count bookkeeping lives here because both the renderer and SDK operations are implemented by this HLE.
   Never replay old return data on underrun, and never enqueue part of a multichannel frame on send overflow. */
typedef struct {
    int32_t *data;
    int capacity, read, write, count;
} AuxRing;
typedef struct {
    int used, released, mix, enabled, active_enabled, n, active_n, sample_count;
    int8_t in[MAX_BUF], out[MAX_BUF], active_in[MAX_BUF], active_out[MAX_BUF];
    uint64_t order, sent_frames, returned_frames, underruns, overruns;
    AuxRing send, ret;
} Aux;
static Aux auxes[MAX_EFFECTS];

static void aux_ring_reset(AuxRing *r) { r->read = r->write = r->count = 0; }
static int aux_ring_read(AuxRing *r, int32_t *dst, int count) {
    if (!dst || count <= 0) return 0;
    if (count > r->count) count = r->count;
    const int first = count < r->capacity - r->read ? count : r->capacity - r->read;
    memcpy(dst, r->data + r->read, (size_t)first * sizeof *dst);
    memcpy(dst + first, r->data, (size_t)(count - first) * sizeof *dst);
    r->read = r->capacity ? (r->read + count) % r->capacity : 0;
    r->count -= count;
    return count;
}
static int aux_ring_write(AuxRing *r, const int32_t *src, int count) {
    if (!src || count <= 0) return 0;
    if (count > r->capacity - r->count) count = r->capacity - r->count;
    const int first = count < r->capacity - r->write ? count : r->capacity - r->write;
    memcpy(r->data + r->write, src, (size_t)first * sizeof *src);
    memcpy(r->data, src + first, (size_t)(count - first) * sizeof *src);
    r->write = r->capacity ? (r->write + count) % r->capacity : 0;
    r->count += count;
    return count;
}
static void aux_reset(Aux *a) { aux_ring_reset(&a->send); aux_ring_reset(&a->ret); }
static int32_t aux_pcm(float sample) {
    const double pcm = (double)sample * 32768.0;
    if (!isfinite(pcm)) return 0;
    if (pcm >= INT32_MAX) return INT32_MAX;
    if (pcm <= INT32_MIN) return INT32_MIN;
    return (int32_t)pcm;
}
static void aux_frame(Aux *a, Mix *m) {
    const int n = a->active_n, count = n * FRAME;
    if (n <= 0) return;
    /* Snapshot all inputs before overwriting any output, including cross-channel maps. */
    float input[MAX_BUF][FRAME];
    int32_t pcm[MAX_BUF * FRAME];
    for (int ch = 0; ch < n; ch++) {
        const int b = a->active_in[ch];
        if (b >= 0 && b < m->buffers) memcpy(input[ch], m->buf[b], sizeof input[ch]);
        else memset(input[ch], 0, sizeof input[ch]);
    }
    if (!a->active_enabled) {
        for (int ch = 0; ch < n; ch++) {
            const int b = a->active_out[ch];
            if (b >= 0 && b < m->buffers) memcpy(m->buf[b], input[ch], sizeof input[ch]);
        }
        return;
    }
    if (a->send.capacity - a->send.count >= count) {
        for (int ch = 0; ch < n; ch++)
            for (int i = 0; i < FRAME; i++) pcm[ch * FRAME + i] = aux_pcm(input[ch][i]);
        aux_ring_write(&a->send, pcm, count);
        a->sent_frames++;
    } else a->overruns++;
    if (a->ret.count >= count) {
        aux_ring_read(&a->ret, pcm, count);
        a->returned_frames++;
    } else {
        memset(pcm, 0, (size_t)count * sizeof *pcm);
        a->underruns++;
    }
    for (int ch = 0; ch < n; ch++) {
        const int b = a->active_out[ch];
        if (b >= 0 && b < m->buffers)
            for (int i = 0; i < FRAME; i++) m->buf[b][i] = pcm[ch * FRAME + i] * (1.0f / 32768.0f);
    }
}
static void effects_frame(int mix) {
    /* Preserve insertion order across effect types, e.g. reverb followed by a CPU delay. */
    uint64_t previous = 0;
    for (;;) {
        uint64_t next = UINT64_MAX;
        int reverb = -1, aux = -1;
        for (int e = 0; e < MAX_EFFECTS; e++) {
            if (reverbs[e].used && reverbs[e].mix == mix && reverbs[e].order > previous && reverbs[e].order < next)
                next = reverbs[e].order, reverb = e, aux = -1;
            if (auxes[e].used && auxes[e].mix == mix && auxes[e].order > previous && auxes[e].order < next)
                next = auxes[e].order, aux = e, reverb = -1;
        }
        if (reverb < 0 && aux < 0) break;
        if (reverb >= 0) reverb_frame(&reverbs[reverb], &mixes[mix]);
        else aux_frame(&auxes[aux], &mixes[mix]);
        previous = next;
    }
}

/* ---- the mixer ------------------------------------------------------------------------------------------------- */
static void biquad_frame(float *samples, const float b[3], const float a[2], float z[2]) {
    float z0 = z[0], z1 = z[1];
    for (int i = 0; i < FRAME; i++) {
        const float x = samples[i], y = b[0] * x + z0;
        /* The denominator is already negated: a one-pole low pass has b0 = 1 - p, a1 = p. */
        z0 = b[1] * x + a[0] * y + z1;
        z1 = b[2] * x + a[1] * y;
        samples[i] = y;
    }
    if (!isfinite(z0) || !isfinite(z1)) {
        z0 = z1 = 0;
        memset(samples, 0, FRAME * sizeof *samples);
    }
    z[0] = z0;
    z[1] = z1;
}

static void render_voice(Voice *v) {
    /* (a started voice without a destination still plays, as on the DSP: its buffers run out and are released) */
    if (v->state != PLAY_STARTED) return;
    if (v->snap) {   /* (just started, or a new destination: its volumes at once) */
        v->snap = 0;
        v->ramp_volume = v->volume;
        memcpy(v->ramp_vol, v->vol, sizeof v->ramp_vol);
        if (v->dest_kind == DEST_SPLITTER && splitters[v->dest].used)
            memcpy(splitters[v->dest].ramp, splitters[v->dest].vol, sizeof splitters[v->dest].ramp);
    }
    const double step = (double)v->rate * (v->pitch > 0 ? v->pitch : 1.0) / RATE;
    float out[MAX_CH][FRAME];
    int any = 0;
    /* volumes ramp from the last frame's to the new ones over the frame, as the console's DSP does (a jump clicks) */
    const float vol0 = v->ramp_volume, dvol = (v->volume - v->ramp_volume) / FRAME;
    for (int i = 0; i < FRAME; i++) {
        v->frac += step;
        while (v->frac >= 1.0) {
            v->frac -= 1.0;
            memcpy(v->prev, v->cur, sizeof v->prev);
            if (!next_source_frame(v)) memset(v->cur, 0, sizeof v->cur);
            else any = 1;
        }
        const float t = (float)v->frac, gain = vol0 + dvol * (float)(i + 1);
        for (int ch = 0; ch < v->channels; ch++) out[ch][i] = (v->prev[ch] + (v->cur[ch] - v->prev[ch]) * t) * gain;
    }
    v->ramp_volume = v->volume;
    (void)any;
    /* the biquad filters (transposed direct form II, as the DSP's) */
    for (int f = 0; f < 2; f++) {
        if (!v->bq_on[f]) continue;
        for (int ch = 0; ch < v->channels && ch < MAX_CH; ch++)
            biquad_frame(out[ch], v->bq_b[f], v->bq_a[f], v->bq_z[f][ch]);
    }
    if (v->dest_kind == DEST_MIX && mixes[v->dest].used) {
        Mix *m = &mixes[v->dest];
        for (int ch = 0; ch < v->channels; ch++)
            for (int b = 0; b < m->buffers; b++) {
                const float g0 = v->ramp_vol[ch][b], g1 = v->vol[ch][b];
                if (g0 == 0.0f && g1 == 0.0f) continue;
                const float dg = (g1 - g0) / FRAME;
                for (int i = 0; i < FRAME; i++) m->buf[b][i] += out[ch][i] * (g0 + dg * (float)(i + 1));
            }
    } else if (v->dest_kind == DEST_SPLITTER && splitters[v->dest].used) {
        Splitter *s = &splitters[v->dest];
        for (int d = 0; d < s->dests; d++) {
            const int mi = s->dest_mix[d];
            if (mi < 0 || !mixes[mi].used) continue;
            Mix *m = &mixes[mi];
            for (int f = 0; f < 2; f++)
                if (v->split_bq_epoch[d][f] != s->bq_epoch[d][f]) {
                    memset(v->split_bq_z[d][f], 0, sizeof v->split_bq_z[d][f]);
                    v->split_bq_epoch[d][f] = s->bq_epoch[d][f];
                }
            for (int ch = 0; ch < v->channels && ch < MAX_CH; ch++) {
                float filtered[FRAME];
                const float *samples = out[ch];
                if (s->bq_on[d][0] || s->bq_on[d][1]) {
                    memcpy(filtered, out[ch], sizeof filtered);
                    for (int f = 0; f < 2; f++)
                        if (s->bq_on[d][f]) biquad_frame(filtered, s->bq_b[d][f], s->bq_a[d][f], v->split_bq_z[d][f][ch]);
                    samples = filtered;
                }
                for (int b = 0; b < m->buffers; b++) {
                    const float g0 = s->ramp[d][ch][b], g1 = s->vol[d][ch][b];
                    if (g0 == 0.0f && g1 == 0.0f) continue;
                    const float dg = (g1 - g0) / FRAME;
                    for (int i = 0; i < FRAME; i++) m->buf[b][i] += samples[i] * (g0 + dg * (float)(i + 1));
                }
            }
        }
    }
    memcpy(v->ramp_vol, v->vol, sizeof v->ramp_vol);   /* (splitters catch up in render_frame) */
}

static int mix_depth(int i, int guard) {   /* how many sub mixes lie between this one and the final mix */
    if (guard > MAX_MIXES || mixes[i].final_mix || mixes[i].dest_kind != DEST_MIX) return 0;
    return 1 + mix_depth(mixes[i].dest, guard + 1);
}

static void render_frame(int16_t *pcm, int channels) {   /* 240 frames of the device's 2 or 6 channels */
    for (int m = 0; m < MAX_MIXES; m++)
        if (mixes[m].used) {
            const int rows = mixes[m].buffers < 0 ? 0 : mixes[m].buffers > MAX_BUF ? MAX_BUF : mixes[m].buffers;
            memset(mixes[m].buf, 0, (size_t)rows * sizeof mixes[m].buf[0]);
        }
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used) render_voice(&voices[i]);
    for (int i = 0; i < MAX_SPLITTERS; i++)   /* (the splitters' volume ramps: once a frame, whichever voices use them) */
        if (splitters[i].used)
            for (int d = 0; d < splitters[i].dests; d++)
                memcpy(splitters[i].ramp[d], splitters[i].vol[d], sizeof splitters[i].ramp[d]);
    /* sub mixes, deepest first */
    int order[MAX_MIXES], n = 0;
    for (int m = 0; m < MAX_MIXES; m++)
        if (mixes[m].used && !mixes[m].final_mix) { mixes[m].depth = mix_depth(m, 0); order[n++] = m; }
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (mixes[order[b]].depth > mixes[order[a]].depth) { const int t = order[a]; order[a] = order[b]; order[b] = t; }
    for (int k = 0; k < n; k++) {
        Mix *s = &mixes[order[k]];
        effects_frame(order[k]);
        if (s->dest_kind != DEST_MIX || !mixes[s->dest].used) continue;
        Mix *d = &mixes[s->dest];
        for (int src = 0; src < s->buffers; src++)
            for (int dst = 0; dst < d->buffers; dst++) {
                const float g = s->to[src][dst] * s->volume;
                if (g == 0.0f) continue;
                for (int i = 0; i < FRAME; i++) d->buf[dst][i] += s->buf[src][i] * g;
            }
    }
    /* the final mix through the sink's channel map: FL FR FC LFE RL RR, as it is to a 5.1 device (the same order on
       Windows' and SDL's 5.1), else down to stereo */
    const int six = channels == 6;
    float out[6][FRAME];
    memset(out, 0, sizeof out);
    if (sink_mix >= 0 && mixes[sink_mix].used) {
        effects_frame(sink_mix);
        Mix *f = &mixes[sink_mix];
        const float g = f->volume;
        for (int k = 0; k < sink_n && k < 6; k++) {
            const int b = sink_in[k];
            if (b < 0 || b >= f->buffers) continue;
            if (six) {
                for (int i = 0; i < FRAME; i++) out[k][i] += f->buf[b][i] * g;
                continue;
            }
            const float gl = k == 0 ? 1.0f : k == 1 ? 0.0f : k == 2 ? 0.7071f : k == 4 ? 0.7071f : k == 3 ? 0.5f : 0.0f;
            const float gr = k == 1 ? 1.0f : k == 0 ? 0.0f : k == 2 ? 0.7071f : k == 5 ? 0.7071f : k == 3 ? 0.5f : 0.0f;
            for (int i = 0; i < FRAME; i++) {
                out[0][i] += f->buf[b][i] * g * gl;
                out[1][i] += f->buf[b][i] * g * gr;
            }
        }
    }
    const int nch = six ? 6 : 2;
    {   /* HOS_AUDIO_STATS=1: once a second, what plays and where the signal is */
        static int stats = -1, frames;
        if (stats < 0) stats = env_on("HOS_AUDIO_STATS");
        if (stats && ++frames % 200 == 0) {
            int started = 0, with_buffers = 0, to_mix = 0, to_split = 0;
            for (int i = 0; i < MAX_VOICES; i++) {
                const Voice *v = &voices[i];
                if (!v->used) continue;
                started += v->state == PLAY_STARTED;
                with_buffers += v->state == PLAY_STARTED && v->nwb > 0;
                to_mix += v->state == PLAY_STARTED && v->dest_kind == DEST_MIX;
                to_split += v->state == PLAY_STARTED && v->dest_kind == DEST_SPLITTER;
            }
            char b[512];
            int n = snprintf(b, sizeof b, "audio: %d voices started (%d with buffers, %d to a mix, %d to a splitter); mixes:",
                             started, with_buffers, to_mix, to_split);
            for (int m = 0; m < MAX_MIXES && n < 480; m++) {
                if (!mixes[m].used) continue;
                double e = 0;
                for (int k = 0; k < mixes[m].buffers; k++)
                    for (int i = 0; i < FRAME; i++) e += mixes[m].buf[k][i] * mixes[m].buf[k][i];
                n += snprintf(b + n, sizeof b - n, " %d%s(%d->%d v%.2f e%.3g)", m, mixes[m].final_mix ? "F" : "",
                              mixes[m].buffers, mixes[m].dest_kind == DEST_MIX ? mixes[m].dest : -1, mixes[m].volume, e);
            }
            hos_log("%s", b);
            {   /* rate x pitch of the playing voices */
                char h[512];
                int hn = snprintf(h, sizeof h, "audio:   rate/pitch:");
                for (int i = 0; i < MAX_VOICES && hn < 480; i++)
                    if (voices[i].used && voices[i].state == PLAY_STARTED)
                        hn += snprintf(h + hn, sizeof h - hn, " %d/%.3f", voices[i].rate, voices[i].pitch);
                hos_log("%s", h);
            }
            int shown = 0;
            for (int i = 0; i < MAX_VOICES && shown < 4; i++) {
                const Voice *v = &voices[i];
                if (!v->used || v->state != PLAY_STARTED) continue;
                shown++;
                int nz = 0;
                float maxv = 0;
                for (int ch = 0; ch < MAX_CH; ch++)
                    for (int k = 0; k < MAX_BUF; k++)
                        if (v->vol[ch][k] != 0) { nz++; if (fabsf(v->vol[ch][k]) > maxv) maxv = fabsf(v->vol[ch][k]); }
                if (v->dest_kind == DEST_SPLITTER && v->dest >= 0) {
                    const Splitter *sp = &splitters[v->dest];
                    for (int d = 0; d < sp->dests; d++)
                        for (int ch = 0; ch < MAX_CH; ch++)
                            for (int k = 0; k < MAX_BUF; k++)
                                if (sp->vol[d][ch][k] != 0) { nz++; if (fabsf(sp->vol[d][ch][k]) > maxv) maxv = fabsf(sp->vol[d][ch][k]); }
                }
                hos_log("audio:   voice %d fmt %d %d Hz pitch %.2f vol %.2f dest %d/%d, %d mix volumes (max %.2f), %d buffers, pos %lld, played %lld, cur %.4f",
                        i, v->format, v->rate, v->pitch, v->volume, v->dest_kind, v->dest, nz, maxv, v->nwb,
                        (long long)v->pos, (long long)v->played, v->cur[0]);
            }
        }
    }
    if (lim_enabled)   /* fast attack, ~50 ms release (one gain for every channel, so the picture does not move) */
        for (int i = 0; i < FRAME; i++) {
            float x = 0;
            for (int ch = 0; ch < nch; ch++) x = fmaxf(x, fabsf(out[ch][i]));
            x *= lim_in;
            const float want = x > lim_threshold ? lim_threshold / x : 1.0f;
            lim_env = want < lim_env ? want : lim_env + (want - lim_env) * 0.0004f;
            for (int ch = 0; ch < nch; ch++) out[ch][i] *= lim_in * lim_env * lim_out;
        }
    for (int i = 0; i < FRAME; i++)
        for (int ch = 0; ch < nch; ch++) {
            float a = out[ch][i] * 32767.0f;
            if (a != a) a = 0;   /* (NaN) */
            if (a > 32767) a = 32767;
            if (a < -32768) a = -32768;
            pcm[i * nch + ch] = (int16_t)a;
        }
}

/* (the output's platform parts, outside the section tests/audio_output_errors_test.c compiles with its own) */
static unsigned au_device_count(void) {
#ifdef _WIN32
    return waveOutGetNumDevs();
#else
    return 0;   /* (linux: waveOut is SDL's default device) */
#endif
}
static void au_device_name(unsigned device, char *out, size_t size) {
    snprintf(out, size, "device %u", device);
#ifdef _WIN32
    WAVEOUTCAPSA caps;
    if (waveOutGetDevCapsA(device, &caps, sizeof caps) == MMSYSERR_NOERROR) snprintf(out, size, "%s", caps.szPname);
#endif
}
static uint64_t au_ms(void) { return (uint64_t)GetTickCount64(); }
/* (hookshot) Which output Windows has as its default now (the waveOut mapper's preferred device, its number and name):
   a key that changes when the default does; 0 when unknown. A device opened as the default stays on that device when
   Windows moves its default (a Bluetooth headset gone and back), so the output section asks every second */
#ifndef DRVM_MAPPER_PREFERRED_GET
#define DRVM_MAPPER_PREFERRED_GET (0x2000 + 21)   /* (mmddk.h: DRVM_MAPPER + 21) */
#endif
static uint32_t au_default_key(void) {
#ifdef _WIN32
    DWORD id = (DWORD)-1, flags = 0;
    if (waveOutMessage((HWAVEOUT)(UINT_PTR)WAVE_MAPPER, DRVM_MAPPER_PREFERRED_GET, (DWORD_PTR)&id, (DWORD_PTR)&flags) !=
            MMSYSERR_NOERROR || id == (DWORD)-1)
        return 0;
    char name[64];
    au_device_name(id, name, sizeof name);
    uint32_t h = 2166136261u ^ id;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1;
#else
    return 0;   /* (linux: SDL follows the default device itself) */
#endif
}
/* HOS_AUDIO_MUTE=1 (test runs): the device opened and fed as for a player, at volume 0 */
static void au_opened(HWAVEOUT w) {
#ifdef _WIN32
    if (env_on("HOS_AUDIO_MUTE")) waveOutSetVolume(w, 0);
#else
    (void)w;
#endif
}
/* The default output's channels (its shared-mode mix format on Windows, SDL's default device on Linux); 2 when
   unknown. */
#ifdef _WIN32
static int au_device_channels(void) {
    static const GUID clsid_enumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    static const GUID iid_enumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    static const GUID iid_audio_client = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
    int n = 2;
    const HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* (a thread in another apartment keeps it) */
    IMMDeviceEnumerator *e = NULL;
    if (SUCCEEDED(CoCreateInstance(&clsid_enumerator, NULL, CLSCTX_ALL, &iid_enumerator, (void **)&e)) && e) {
        IMMDevice *d = NULL;
        if (SUCCEEDED(e->lpVtbl->GetDefaultAudioEndpoint(e, eRender, eConsole, &d)) && d) {
            IAudioClient *a = NULL;
            if (SUCCEEDED(d->lpVtbl->Activate(d, &iid_audio_client, CLSCTX_ALL, NULL, (void **)&a)) && a) {
                WAVEFORMATEX *mix = NULL;
                if (SUCCEEDED(a->lpVtbl->GetMixFormat(a, &mix)) && mix) {
                    n = mix->nChannels;
                    CoTaskMemFree(mix);
                }
                a->lpVtbl->Release(a);
            }
            d->lpVtbl->Release(d);
        }
        e->lpVtbl->Release(e);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return n;
}
#else
static int au_device_channels(void) { return hos_sdl_audio_channels(); }
#endif
/* (hookshot) The Insert menu's Audio output (gpu/vk/vk_menu.cpp): audio_output= in hookshot_options.ini beside the game,
   0 Auto, 1 Stereo, 2 5.1 (a number past them is the nearest, as the menu reads it). Read from the file, as
   host/nn_applet.c reads the Handheld profile: the game asks for its speakers only once, at its start */
static int au_menu_output(void) {
    char path[MAX_PATH];
    const DWORD len = hos_game_path_a(path, MAX_PATH);
    char *slash = len ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;   /* (linux: '/') */
    if (!slash) return 0;
    snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "hookshot_options.ini");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[80];
    int v = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "audio_output=", 13)) v = atoi(line + 13);
    fclose(f);
    return v < 1 ? 0 : v > 2 ? 2 : v;
}
/* The speakers the game mixes for: the Insert menu's Audio output, Stereo 2 or 5.1 6, HOS_AUDIO_CHANNELS=2 or 6 over it;
   Auto: 6 when the default output has 5.1 or more, else 2. The game asks once, at its start
   (GetActiveAudioDeviceChannelCountForOutput), and pans for that many, so a change takes a restart. Told 2 (always,
   before), it put nothing behind the player, and a 5.1 system played the whole game from the front speakers (issue
   1124). Its device sink has the six channels either way; with two speakers they are mixed down. */
static int au_speakers(void) {
    static volatile int known;
    if (!known) {
        au_lock();   /* (the game's thread and the mixer's ask at about the same time: one of them looks) */
        if (!known) {
            const char *want = getenv("HOS_AUDIO_CHANNELS");
            const int menu = want && *want ? 0 : au_menu_output();
            int n;
            if (want && *want) {
                n = atoi(want) >= 6 ? 6 : 2;
                hos_log("audio: %s for the game (HOS_AUDIO_CHANNELS)", n == 6 ? "5.1" : "stereo");
            } else if (menu) {
                n = menu == 2 ? 6 : 2;
                hos_log("audio: %s for the game (the Insert menu's Audio output)", n == 6 ? "5.1" : "stereo");
            } else {
                const int device = au_device_channels();
                n = device >= 6 ? 6 : 2;
                hos_log("audio: %s for the game (the default output has %d channels)", n == 6 ? "5.1" : "stereo", device);
            }
            known = n;
        }
        au_unlock();
    }
    return known;
}
/* The device's format, 16-bit PCM at 48 kHz: Windows takes more than two channels only as WAVE_FORMAT_EXTENSIBLE,
   with the speakers named (5.1: FL FR FC LFE BL BR, the device sink's order). SDL takes the channel count as it is. */
static const WAVEFORMATEX *au_format(const WAVEFORMATEX *f) {
#ifdef _WIN32
    if (f->nChannels <= 2) return f;
    static const GUID pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    static WAVEFORMATEXTENSIBLE x;
    memset(&x, 0, sizeof x);
    x.Format = *f;
    x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x.Format.cbSize = sizeof x - sizeof x.Format;
    x.Samples.wValidBitsPerSample = f->wBitsPerSample;
    x.dwChannelMask = 0x3F;   /* front left, right, centre, low frequency, back left, right */
    x.SubFormat = pcm;        /* (KSDATAFORMAT_SUBTYPE_PCM) */
    return &x.Format;
#else
    return f;
#endif
}

/* ---- output: waveOut, 10 ms blocks; a WAV file with HOS_AUDIO_DUMP ---------------------------------------------- */
#define NBLOCKS 10
#define OUT_MAX_CH 6
static HWAVEOUT wout;
static WAVEHDR hdr[NBLOCKS];
static int16_t blocks[NBLOCKS][FRAME * 2 * OUT_MAX_CH];
static int fill_block, fill_frames;
static int out_channels = 2;   /* the device's (and the dump's): 2, or 6 for 5.1 */
static FILE *dump;
static uint32_t dump_bytes;

static void output_error(const char *operation, MMRESULT result) {
    static unsigned logged;
    if (logged < 8) {
        logged++;
        hos_log("audio: %s failed (MMRESULT %u)", operation, (unsigned)result);
    }
}
/* The device: HOS_AUDIO_DEVICE=<n> (a waveOut device number) first, then the Windows default (WAVE_MAPPER), then each
   device in turn. Players had no sound at all while every other program played ("no sound in 2.0.5"): the log now
   names the device; one that takes no block for 1.5 s (an unplugged headset, a default bound to a dead output) is
   closed and the next candidate opened (at most 6 times a run); 10 s after opening the log says how many blocks the
   device took and how loud the game's sound was, which tells a silent game from a deaf device. */
static int out_next;           /* the next candidate: 0 = HOS_AUDIO_DEVICE, 1 = the default, 2.. = device n-2 */
static uint64_t out_opened, out_stall_since;
static unsigned out_played, out_peak, out_reported;
/* (hookshot) A device gone (a Bluetooth headset off, the audio service restarted: "no driver", "invalid handle" from the
   next call) is closed and the default opened again; none opening, the default is tried every second until one does.
   Opened as the default, the output follows Windows': its key (au_default_key) looked at every second, a change reopens
   there. Before, a call that failed this way was only logged, the same block tried again for good, and the game had no
   sound for the rest of the run, on any device (RX 5500M laptop, a WH-CH720N headset) */
static int out_retry, out_reopens;
static uint64_t out_retry_at, out_default_seen;
static uint32_t out_default_key;   /* (0: not opened as the default, or Windows did not say which it is) */
static int au_device_gone(MMRESULT r) { return r == 2 || r == 5 || r == 6; }   /* BADDEVICEID, INVALHANDLE, NODRIVER */
static int output_try(int candidate, const WAVEFORMATEX *f) {
    const char *want = getenv("HOS_AUDIO_DEVICE");
    unsigned device;
    if (candidate == 0) {
        if (!want || !*want) return 0;
        device = (unsigned)atoi(want);
    } else device = candidate == 1 ? WAVE_MAPPER : (unsigned)(candidate - 2);
    const MMRESULT result = waveOutOpen(&wout, device, au_format(f), 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        output_error("waveOutOpen", result);
        wout = NULL;
        return 0;
    }
    au_opened(wout);
    out_default_key = device == WAVE_MAPPER ? au_default_key() : 0;
    out_default_seen = au_ms();
    char name[64];
    if (device == WAVE_MAPPER) snprintf(name, sizeof name, "the default device");
    else au_device_name(device, name, sizeof name);
    hos_log("audio: output on %s (%u devices), %s", name, au_device_count(), f->nChannels == 6 ? "5.1" : "stereo");
    return 1;
}
/* The channels: the game's speakers (au_speakers). A 5.1 format no device takes is stereo instead: the device sink's
   six channels are then mixed down, nothing is lost. */
static void output_open(void) {
    static int dump_done;
    int channels = au_speakers();
    if (!dump_done) {
        const char *d = getenv("HOS_AUDIO_DUMP");
        if (d && (dump = fopen(d, "wb")) != NULL) {
            static const uint8_t z[44];
            fwrite(z, 1, 44, dump);
        }
        out_channels = channels;
        dump_done = 1;
    }
    if (getenv("HOS_AUDIO") && atoi(getenv("HOS_AUDIO")) == 0) return;
    if (dump) channels = out_channels;   /* (a dump keeps one layout: no stereo fallback) */
    for (;;) {
        WAVEFORMATEX f = {0};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = channels;
        f.nSamplesPerSec = RATE;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2 * channels;
        f.nAvgBytesPerSec = RATE * 2 * channels;
        const int candidates = 2 + (int)au_device_count();
        for (int tried = 0; tried < candidates && !wout; tried++) {
            const int c = out_next;
            out_next = (out_next + 1) % candidates;
            output_try(c, &f);
        }
        if (wout || channels == 2 || dump) break;
        hos_log("audio: no device took 5.1: stereo instead");
        channels = 2;
    }
    out_channels = channels;
    out_retry = !wout, out_retry_at = au_ms();   /* (none opened: tried again in a second, output_frame) */
    if (!wout) return;
    for (int i = 0; i < NBLOCKS; i++) {
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)blocks[i];
        hdr[i].dwBufferLength = FRAME * 2 * channels * sizeof blocks[0][0];   /* (two frames) */
        hdr[i].dwFlags = WHDR_DONE;
    }
    fill_block = fill_frames = 0;
    out_opened = au_ms();
    out_stall_since = 0;
    out_played = out_peak = out_reported = 0;
}
static void output_close(void) {   /* (what fails here is not looked at: a device gone takes none of these calls) */
    waveOutReset(wout);
    for (int i = 0; i < NBLOCKS; i++)
        if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wout, &hdr[i], sizeof hdr[i]);
    waveOutClose(wout);
    wout = NULL;
}
static void output_reopen(void) {   /* (a device that takes nothing: the next candidate, 6 times; again after 30 s of sound) */
    out_stall_since = 0;
    if (++out_reopens > 6) return;
    hos_log("audio: the output device took no sound for 1.5 s: trying the next one");
    output_close();
    output_open();
}
static void output_open_default(void) {   /* (HOS_AUDIO_DEVICE first when it is set, as at the start) */
    const char *want = getenv("HOS_AUDIO_DEVICE");
    out_next = want && *want ? 0 : 1;
    output_open();
}
static void output_move(const char *why) {
    hos_log("audio: %s: the output opened again on the default device", why);
    output_close();
    output_open_default();
}
static void dump_finish(void) {
    if (!dump) return;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    const uint16_t pcm = 1, ch = (uint16_t)out_channels, align = (uint16_t)(2 * out_channels), bits = 16;
    const uint32_t riff = 36 + dump_bytes, fmt = 16, rate = RATE, brate = RATE * align, data = dump_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &brate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(dump, 0, SEEK_SET);
    fwrite(h, 1, 44, dump);
    fflush(dump);
}
static void output_frame(const int16_t *pcm) {
    if (dump) {
        fwrite(pcm, (size_t)(2 * out_channels), FRAME, dump);
        dump_bytes += (uint32_t)(FRAME * 2 * out_channels);
        static int n;
        if (++n % 200 == 0) {   /* (the header kept valid every second: the process may end without notice) */
            const long at = ftell(dump);
            dump_finish();
            fseek(dump, at, SEEK_SET);
        }
    }
    if (!wout) {
        if (!out_retry || au_ms() - out_retry_at < 1000) return;
        output_open_default();   /* (the service back, a device plugged in) */
        if (!wout) return;
    }
    if (out_default_key && au_ms() - out_default_seen >= 1000) {
        out_default_seen = au_ms();
        const uint32_t now = au_default_key();
        if (now && now != out_default_key) {
            output_move("Windows' default output changed");
            if (!wout) return;
        }
    }
    for (int i = 0; i < FRAME * out_channels; i++) {
        const unsigned a = (unsigned)(pcm[i] < 0 ? -pcm[i] : pcm[i]);
        if (a > out_peak) out_peak = a;
    }
    if (!out_reported && au_ms() - out_opened > 10000) {
        out_reported = 1;
        hos_log("audio: %u blocks played in the first 10 s, loudest sample %u of 32767%s", out_played, out_peak,
                out_played == 0 ? " (the device takes nothing)" : out_peak == 0 ? " (the game sent silence)" : "");
    }
    WAVEHDR *h = &hdr[fill_block];
    if (fill_frames == 0) {
        if (!(h->dwFlags & WHDR_DONE)) {   /* the device is behind: this frame is dropped */
            const uint64_t now = au_ms();
            if (!out_stall_since) out_stall_since = now;
            else if (now - out_stall_since > 1500) output_reopen();
            return;
        }
        out_stall_since = 0;
        if (h->dwFlags & WHDR_PREPARED) {
            const MMRESULT result = waveOutUnprepareHeader(wout, h, sizeof *h);
            if (result != MMSYSERR_NOERROR) {
                output_error("waveOutUnprepareHeader", result);
                if (au_device_gone(result)) output_move("the output device went away");
                return;   /* the driver has not released the header: do not change its samples or flags */
            }
        }
    }
    memcpy(blocks[fill_block] + fill_frames * FRAME * out_channels, pcm, (size_t)(FRAME * 2 * out_channels));
    if (++fill_frames == 2) {
        h->dwFlags = 0;
        const char *operation = "waveOutPrepareHeader";
        MMRESULT result = waveOutPrepareHeader(wout, h, sizeof *h);
        if (result == MMSYSERR_NOERROR) {
            operation = "waveOutWrite";
            result = waveOutWrite(wout, h, sizeof *h);
            if (result == MMSYSERR_NOERROR) {
                out_played++;
                if (out_reopens && au_ms() - out_opened > 30000) out_reopens = 0;   /* (sound again: the stall rule's 6 anew) */
            }
        }
        if (result != MMSYSERR_NOERROR) {
            output_error(operation, result);
            if (au_device_gone(result)) {
                output_move("the output device went away");
                return;
            }
            /* A failed submission has no completion to set DONE. Reuse only a host-owned block; a prepared header
               is unprepared on its next turn. Without this, returning to this slot drops every later audio frame. */
            if (!(h->dwFlags & WHDR_INQUEUE)) h->dwFlags |= WHDR_DONE;
        }
        fill_frames = 0;
        fill_block = (fill_block + 1) % NBLOCKS;
    }
}

/* ---- parameter updates ------------------------------------------------------------------------------------------
   The game changes a voice's play state, wave buffers, volumes, filters and destination (and its mixes' and
   splitters') with separate calls, then commits them together with RequestUpdateAudioRenderer; the console's renderer
   only ever sees whole updates. Applied as each call came, the mixer thread rendered frames between them: a voice
   started before its volumes were set (1.0, or the previous sound's), or still sent to its old mix, played a short
   loud blip. The calls queue their change instead (with the arguments copied: TOTK refills the structs it passes),
   and the update applies the queue under the mixer's lock. Should no update come for 50 ms, the mixer applies the
   queue itself. Acquisitions stay immediate (they return the object); releases are queued like the rest, the object
   invalid to the game at once. Objects are queued as our slot indices, looked up when the game calls: the game reuses
   its VoiceType for the next sound right after releasing it, so its handle names the new voice by the time the
   update comes (looked up then, a queued release ended the new sound: no footsteps or hits). A released slot stays
   taken until the update applies the release, so the indices hold for the queue's life.
   The other way too: the wave buffers the mixer finishes reach the game at the next update (GetReleasedWaveBuffer
   returns them, GetWaveBufferCount stops counting them), as the console's renderer reports them. Seen as the mixer
   finished them, a pass over the voices could find the first ones full and a later one a buffer short: Ocarina's sound
   thread feeds its six speaker voices that way, appended the short one an extra buffer, then one more each pass, until
   the queue was full and that speaker went silent for good (a run in five or so).
   HOS_AUDIO_IMMEDIATE=1: every change applied as it comes (as before). */
enum { OP_VOICE_RELEASE, OP_VOICE_DEST, OP_VOICE_MIX_VOLUME, OP_VOICE_VOLUME, OP_VOICE_PITCH, OP_VOICE_BIQUAD,
       OP_VOICE_STATE, OP_VOICE_APPEND, OP_MIX_RELEASE, OP_MIX_VOLUME, OP_MIX_DEST, OP_MIX_MIX_VOLUME, OP_SPLIT_RELEASE,
       OP_SPLIT_DEST, OP_SPLIT_VOLUME, OP_SPLIT_BIQUAD, OP_AUX_ENABLE, OP_AUX_MAP, OP_AUX_RELEASE };
typedef struct {
    int op, idx, tgt;          /* our voice / mix / splitter; a destination mix or splitter (-1: none) */
    int i0, i1, i2;            /* (op-specific: a kind, an index, channels, a state) */
    uint64_t wbp;              /* an appended WaveBuffer's identity */
    float f, b[3], a[2];
    WB wb;
    int8_t in[MAX_BUF], out[MAX_BUF];
} Cmd;
static Cmd *cmds;
static int ncmds, cmds_cap;
static int64_t last_update_ns;

static void apply(const Cmd *k) {   /* (with the lock held) */
    if (k->op <= OP_VOICE_APPEND) {
        Voice *v = &voices[k->idx];
        if (!v->used) return;
        switch (k->op) {
        case OP_VOICE_RELEASE: v->used = 0; break;
        case OP_VOICE_DEST:
            if (v->dest != k->tgt || v->dest_kind != k->i0)
                memset(v->split_bq_epoch, 0, sizeof v->split_bq_epoch);
            v->dest = k->tgt;
            v->dest_kind = k->tgt < 0 ? DEST_NONE
                         : k->i0 == DEST_MIX ? (mixes[k->tgt].used ? DEST_MIX : DEST_NONE)
                         : k->i0 == DEST_SPLITTER ? (splitters[k->tgt].used ? DEST_SPLITTER : DEST_NONE) : DEST_NONE;
            v->snap = 1;
            break;
        case OP_VOICE_MIX_VOLUME: v->vol[k->i0][k->i1] = k->f; break;
        case OP_VOICE_VOLUME: v->volume = k->f; break;
        case OP_VOICE_PITCH: v->pitch = k->f; break;
        case OP_VOICE_BIQUAD:
            if (k->i1 && !v->bq_on[k->i0]) memset(v->bq_z[k->i0], 0, sizeof v->bq_z[k->i0]);
            v->bq_on[k->i0] = k->i1;
            memcpy(v->bq_b[k->i0], k->b, sizeof k->b);
            memcpy(v->bq_a[k->i0], k->a, sizeof k->a);
            break;
        case OP_VOICE_STATE:
            if (k->i0 == PLAY_STOPPED && v->state != PLAY_STOPPED) {   /* stopped: every queued buffer is released */
                for (int i = 0; i < v->nwb; i++) release_wb(v, v->wb[i]);
                v->nwb = 0;
                v->started_wb = 0;
                v->frac = 0;
                memset(v->prev, 0, sizeof v->prev);
                memset(v->cur, 0, sizeof v->cur);
                v->yn1 = v->yn2 = 0;
            }
            if (k->i0 == PLAY_STARTED && v->state != PLAY_STARTED) v->snap = 1;
            v->state = k->i0;
            break;
        case OP_VOICE_APPEND:
            if (v->pending_wb > 0) v->pending_wb--;
            if (v->nwb < MAX_WB) {
                v->wbc[v->nwb] = k->wb;
                v->wb[v->nwb++] = k->wbp;
            } else {
                release_wb(v, k->wbp);
            }
            break;
        }
        return;
    }
    const int tgt_mix = k->tgt >= 0 && k->tgt < MAX_MIXES && mixes[k->tgt].used ? k->tgt : -1;
    if (k->op <= OP_MIX_MIX_VOLUME) {
        Mix *m = &mixes[k->idx];
        if (!m->used) return;
        switch (k->op) {
        case OP_MIX_RELEASE:
            m->used = 0;
            if (sink_mix == k->idx) sink_mix = -1;
            for (int e = 0; e < MAX_EFFECTS; e++)
                if (auxes[e].used && auxes[e].mix == k->idx) auxes[e].mix = -1;
            break;
        case OP_MIX_VOLUME: m->volume = k->f; break;
        case OP_MIX_DEST:   /* (a sub mix into a splitter: not used by TOTK so far) */
            m->dest = k->i0 == DEST_MIX ? tgt_mix : -1;
            m->dest_kind = m->dest >= 0 ? DEST_MIX : DEST_NONE;
            break;
        case OP_MIX_MIX_VOLUME: m->to[k->i0][k->i1] = k->f; break;
        }
        return;
    }
    if (k->op >= OP_AUX_ENABLE) {
        Aux *a = &auxes[k->idx];
        if (!a->used) return;
        switch (k->op) {
        case OP_AUX_ENABLE:
            a->active_enabled = k->i0;
            aux_reset(a);
            break;
        case OP_AUX_MAP:
            if (a->active_n != k->i0 || memcmp(a->active_in, k->in, (size_t)k->i0) ||
                memcmp(a->active_out, k->out, (size_t)k->i0)) aux_reset(a);
            a->active_n = k->i0;
            memcpy(a->active_in, k->in, (size_t)k->i0);
            memcpy(a->active_out, k->out, (size_t)k->i0);
            a->send.capacity = a->ret.capacity = k->i0 ? a->sample_count / (FRAME * k->i0) * (FRAME * k->i0) : 0;
            break;
        case OP_AUX_RELEASE: memset(a, 0, sizeof *a); break;
        }
        return;
    }
    Splitter *s = &splitters[k->idx];
    if (!s->used) return;
    switch (k->op) {
    case OP_SPLIT_RELEASE: s->used = 0; break;
    case OP_SPLIT_DEST:
        if (s->dest_mix[k->i0] != tgt_mix)
            for (int f = 0; f < 2; f++) s->bq_epoch[k->i0][f] = ++split_bq_epoch;
        s->dest_mix[k->i0] = tgt_mix;
        break;
    case OP_SPLIT_VOLUME:
        s->vol[k->i0][k->i1][k->i2] = k->f;
        if (s->dest_mix[k->i0] < 0) s->dest_mix[k->i0] = tgt_mix;
        break;
    case OP_SPLIT_BIQUAD:
        if (tgt_mix < 0 || s->dest_mix[k->i0] != tgt_mix) break;
        if (s->bq_on[k->i0][k->i1] != k->i2) s->bq_epoch[k->i0][k->i1] = ++split_bq_epoch;
        s->bq_on[k->i0][k->i1] = k->i2;
        memcpy(s->bq_b[k->i0][k->i1], k->b, sizeof k->b);
        memcpy(s->bq_a[k->i0][k->i1], k->a, sizeof k->a);
        break;
    }
}
static void submit(const Cmd *k) {   /* (with the lock held) */
    static int immediate = -1;
    if (immediate < 0) immediate = env_on("HOS_AUDIO_IMMEDIATE");
    if (ncmds == cmds_cap && !immediate) {
        const int cap = cmds_cap ? cmds_cap * 2 : 4096;
        Cmd *grown = (Cmd *)realloc(cmds, (size_t)cap * sizeof *grown);
        if (grown) cmds = grown, cmds_cap = cap;
    }
    if (immediate || ncmds == cmds_cap) apply(k);
    else cmds[ncmds++] = *k;
}
static void commit(void) {   /* (with the lock held) */
    for (int i = 0; i < ncmds; i++) apply(&cmds[i]);
    ncmds = 0;
    for (int i = 0; i < MAX_VOICES; i++) voices[i].nrel_seen = voices[i].nrel;   /* (the finished buffers, to the game) */
}

/* The mixer's next deadline. `next` was the one just woken for; one frame is `period` later, on that same grid.
   A wake more than 100 ms late used to move the grid to now. The frames in between were never mixed, so each stall
   left the sound that far behind the picture: a little on the intro, and by seconds after a longer one. The loop
   mixes the owed frames instead (a late wake returns at once until the grid has caught up). A pause of more than a
   few seconds still jumps, so a suspend is not mixed out all at once. */
static int64_t audio_mix_deadline(int64_t now, int64_t next, int64_t period) {
    next += period;
    if (now - next > 3000000000LL) next = now + period;
    return next;
}

/* ---- the player's volume (the Insert menu's Audio tab, gpu/vk/vk_menu.cpp) ------------------------------------- */
/* The frame's samples (frames x channels, interleaved) scaled from `from`, the gain the last frame ended at, to `want`,
   step by step over the frame, so a change does not click. The gains are 0 to 1: nothing clips. Returns the gain the
   frame ended at. tests/test_audio_volume.py */
static float audio_apply_gain(int16_t *pcm, int frames, int channels, float from, float want) {
    if (from == 1.0f && want == 1.0f) return want;
    for (int i = 0; i < frames; i++) {
        const float g = from + (want - from) * (float)(i + 1) / (float)frames;
        for (int c = 0; c < channels; c++) pcm[i * channels + c] = (int16_t)lrintf((float)pcm[i * channels + c] * g);
    }
    return want;
}

/* Whether the game's window counts as in front, as host/nn_hid.c's game_in_front decides it: the game's own window does,
   another normal window does not, and an overlay (a tool or no-activate window: Steam's, Discord's, but also a toast or
   a tray flyout) counts as the game's only while the last normal window in front was the game's. *last_normal_was_us
   keeps that between calls. tests/test_audio_volume.py */
static int audio_front_decide(int ours, int overlay, int *last_normal_was_us) {
    if (ours) return *last_normal_was_us = 1;
    if (overlay && *last_normal_was_us) return 1;
    *last_normal_was_us = 0;
    return 0;
}
static int audio_window_in_front(void) {   /* (the mixer thread's only: its own state, not game_in_front's) */
    static int last_normal_was_us = 1;
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    const int ours = pid == GetCurrentProcessId();
    const int overlay = !ours && (GetWindowLongPtrA(w, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0;
    return audio_front_decide(ours, overlay, &last_normal_was_us);
}

/* The Volume row's percent (0 to 100) as a gain (0 to 1), on a squared curve so the slider's steps sound about even: 50%
   is a quarter, -12 dB. A straight line left the bottom half of the slider nearly all very quiet. */
static float audio_volume_curve(int percent) {
    const float v = (float)(percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f;
    return v * v;
}
/* The gain the menu asks for: Volume and Mute in background. With no menu (the WebGPU renderer) the game plays at full
   volume. */
static float audio_player_gain(void) {
    int rx_option(int id, int otherwise);
    int volume = rx_option(HOS_OPT_VOLUME, 100);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume && rx_option(HOS_OPT_MUTE_BACKGROUND, 0) && !audio_window_in_front()) volume = 0;
    return audio_volume_curve(volume);
}

/* ---- the renderer thread --------------------------------------------------------------------------------------- */
static uint64_t renderer_event;
static volatile int renderer_running;
static volatile int64_t elapsed_frames;
static HANDLE renderer_thread;

static DWORD WINAPI renderer_main(void *p) {
    (void)p;
    hos_thread_boost(2);
    output_open();
    int64_t period = 5000000, next = hos_now_ns() + period;   /* one audio frame: 240 samples at 48 kHz */
    static int16_t pcm[FRAME * OUT_MAX_CH];
    float gain = 1.0f, gain_want = 1.0f;
    int gain_frames = 0;
    for (;;) {
        hos_sleep_ns(next - hos_now_ns());
        next = audio_mix_deadline(hos_now_ns(), next, period);
        if (!renderer_running) continue;
        au_lock();
        if (ncmds && hos_now_ns() - last_update_ns > 50000000) commit();   /* (no update came: see "parameter updates") */
        render_frame(pcm, out_channels);
        au_unlock();
        if (gain_frames-- <= 0) gain_want = audio_player_gain(), gain_frames = 20;   /* (the menu read every 100 ms) */
        gain = audio_apply_gain(pcm, FRAME, out_channels, gain, gain_want);
        output_frame(pcm);
        elapsed_frames++;
        if (renderer_event) hos_system_event_signal(renderer_event);
    }
}

/* ---- renderer lifecycle ---------------------------------------------------------------------------------------- */
/* OpenAudioRenderer(AudioRendererHandle*, os::SystemEvent*, AudioRendererParameter const&, void*, unsigned long) */
HLE(_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm) {
    G64(ARG(0)) = 1;
    /* nn::os::SystemEvent (the C++ class) starts with its SystemEventType: key the event on that address */
    renderer_event = ARG(1);
    hos_system_event_init(renderer_event, 1);
    au_lock();
    au_unlock();
    if (!renderer_thread) renderer_thread = CreateThread(NULL, 0, renderer_main, NULL, 0, NULL);
    hos_log("audio: renderer opened");
    RET(0);
}
HLE(_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE) { renderer_running = 1; RET(0); }
HLE(_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE) { renderer_running = 0; }
HLE(_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE) { renderer_running = 0; renderer_event = 0; dump_finish(); }
HLE(_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE) {
    au_lock();
    commit();
    last_update_ns = hos_now_ns();
    au_unlock();
    RET(0);
}
HLE(_ZN2nn5audio33GetAudioRendererElapsedFrameCountEPKNS0_19AudioRendererConfigE) { RET(elapsed_frames); }

/* Sizes the game allocates for the renderer: generous, never touched by us. */
HLE(_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE) { RET(0x10000); }
HLE(_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE) { RET(0x10000); }
HLE(_ZN2nn5audio41GetRequiredBufferSizeForPerformanceFramesERKNS0_22AudioRendererParameterE) { RET(0x1000); }
HLE(_ZN2nn5audio35GetRequiredBufferSizeForI3dl2ReverbEii) { RET(0x10000); }
HLE(_ZN2nn5audio30GetRequiredBufferSizeForReverbEii) { RET(0x10000); }
HLE(_ZN2nn5audio29GetRequiredBufferSizeForDelayENS_8TimeSpanEii) { RET(0x10000); }
HLE(_ZN2nn5audio36GetRequiredBufferSizeForLightLimiterENS_8TimeSpanEii) { RET(0x1000); }
HLE(_ZN2nn5audio43GetRequiredBufferSizeForAuxSendReturnBufferEPKNS0_22AudioRendererParameterEii) {
    const int frames = (int)ARG(1), channels = (int)ARG(2);
    const int samples = ARG(0) ? (int)G32(ARG(0) + 4) : 0;
    if (frames <= 0 || channels <= 0 || samples <= 0 ||
        (uint64_t)frames * channels > (INT32_MAX - 191u) / (4u * (uint64_t)samples)) { RET(0); return; }
    const uint64_t bytes = (uint64_t)frames * channels * samples * sizeof(int32_t);
    RET(((bytes + 63) & ~63ULL) + 128);  /* cache-line-aligned samples plus two cursor blocks */
}

/* AudioRendererParameter: sampleRate, sampleCount, mixBufferCount, subMixCount, voiceCount, sinkCount, effectCount,
   performanceFrameCount, isVoiceDropEnabled, splitterCount, splitterSendChannelCount, renderingDevice,
   executionMode, magic: the defaults; the game sets its own values after. */
HLE(_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE) {
    uint32_t *p = (uint32_t *)GP(ARG(0));
    p[0] = RATE;
    p[1] = FRAME;
    p[2] = 1;
    p[3] = 0;
    p[4] = 24;
    p[5] = 1;
    p[6] = 0;
    p[7] = 0;
}
HLE(_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm) {
    put_handle(ARG(0), K_CONFIG, 0);
}

/* The output device: one device, "MainAudioOut", stereo or 5.1 as the PC's default output (au_speakers).
   AudioDeviceName is char[256]. */
HLE(_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE) { hos_system_event_init(ARG(0), 1); }
HLE(_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv) { RET(au_speakers()); }
HLE(_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE) {
    memset(GP(ARG(0)), 0, 256);
    memcpy(GP(ARG(0)), "MainAudioOut", 12);
}
HLE(_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi) {
    if ((int)ARG(1) > 0) {
        memset(GP(ARG(0)), 0, 256);
        memcpy(GP(ARG(0)), "MainAudioOut", 12);
    }
    RET((int)ARG(1) > 0 ? 1 : 0);
}
HLE(_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf) {}

/* GetSampleByteSize(SampleFormat): PcmInt8 1, PcmInt16 2, PcmInt24 3, PcmInt32 4, PcmFloat 4 (ADPCM: no fixed size).
   The game steps through sample buffers by it: 0 kept the main thread looping (f_089d3588) while Great Sky Island
   loaded. */
HLE(_ZN2nn5audio17GetSampleByteSizeENS0_12SampleFormatE) {
    static const uint8_t size[7] = {0, 1, 2, 3, 4, 4, 0};
    const uint32_t f = (uint32_t)ARG(0);
    RET(f < 7 ? size[f] : 0);
}

/* ---- memory pools: bookkeeping only (all guest memory is readable) --------------------------------------------- */
/* (mkw-recomp) a MemoryPoolType is one pointer in the SDK: the pool's address and size live here, the object holds
   a handle only. The three words written into it before overwrote the 16 bytes after it: MKW keeps pool objects
   where those bytes are a heap's links (its "Sound" heap's free list broke). */
static struct { uint64_t addr, size; int used; } au_pools[1024];
/* (hookshot) the handle lives in the game's MemoryPoolType (guest memory): its index is checked against the table */
static int pool_index(uint64_t obj) {
    const int i = get_handle(obj, K_POOL);
    return i >= 0 && i < 1024 && au_pools[i].used ? i : -1;
}
HLE(_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm) {
    au_lock();
    int i = 0;
    while (i < 1024 && au_pools[i].used) i++;
    if (i < 1024) au_pools[i].used = 1, au_pools[i].addr = ARG(2), au_pools[i].size = ARG(3);
    au_unlock();
    if (i == 1024) { RET(0); return; }
    put_handle(ARG(1), K_POOL, i);
    RET(1);
}
HLE(_ZN2nn5audio17ReleaseMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeE) {
    const int i = pool_index(ARG(1));
    if (i >= 0) au_pools[i].used = 0;
    G64(ARG(1)) = 0;
}
HLE(_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE) { RET(1); }
HLE(_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE) { RET(1); }
HLE(_ZN2nn5audio20IsMemoryPoolAttachedEPKNS0_14MemoryPoolTypeE) { RET(1); }
HLE(_ZN2nn5audio20GetMemoryPoolAddressEPKNS0_14MemoryPoolTypeE) {
    const int i = pool_index(ARG(0));
    RET(i >= 0 ? au_pools[i].addr : 0);
}
HLE(_ZN2nn5audio17GetMemoryPoolSizeEPKNS0_14MemoryPoolTypeE) {
    const int i = pool_index(ARG(0));
    RET(i >= 0 ? au_pools[i].size : 0);
}

/* ---- mixes ------------------------------------------------------------------------------------------------------ */
static int new_mix(uint64_t obj, int final_mix, int buffers) {
    au_lock();
    int i = 0;
    while (i < MAX_MIXES && mixes[i].used) i++;
    if (i == MAX_MIXES) { au_unlock(); return 0; }
    memset(&mixes[i], 0, sizeof mixes[i]);
    mixes[i].used = 1;
    mixes[i].final_mix = final_mix;
    mixes[i].buffers = buffers < 1 ? 1 : buffers > MAX_BUF ? MAX_BUF : buffers;
    mixes[i].volume = 1.0f;
    put_handle(obj, K_MIX, i);
    AUT("audio: %s mix %d (%d buffers) at %#llx", final_mix ? "final" : "sub", i, buffers, (unsigned long long)obj);
    au_unlock();
    return 1;
}
/* AcquireFinalMix(config, FinalMixType*, int bufferCount) */
HLE(_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi) { RET(new_mix(ARG(1), 1, (int)ARG(2))); }
/* AcquireSubMix(config, SubMixType*, int sampleRate, int bufferCount) */
HLE(_ZN2nn5audio13AcquireSubMixEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEii) { RET(new_mix(ARG(1), 0, (int)ARG(3))); }
static void release_mix(uint64_t obj) {
    au_lock();
    Mix *m = mix_of(obj);
    if (m) {
        m->released = 1;
        const Cmd k = {.op = OP_MIX_RELEASE, .idx = (int)(m - mixes)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio15ReleaseFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeE) { release_mix(ARG(1)); }
HLE(_ZN2nn5audio13ReleaseSubMixEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeE) { release_mix(ARG(1)); }
static void set_mix_volume(Ctx *c) {
    au_lock();
    if (mix_of(ARG(0))) {
        const Cmd k = {.op = OP_MIX_VOLUME, .idx = mix_index(ARG(0)), .f = farg(c, 0)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio17SetFinalMixVolumeEPNS0_12FinalMixTypeEf) { set_mix_volume(c); }
HLE(_ZN2nn5audio15SetSubMixVolumeEPNS0_10SubMixTypeEf) {
    AUT("audio: sub mix %d volume %.3f", mix_index(ARG(0)), farg(c, 0));
    set_mix_volume(c);
}
static void set_mix_dest(uint64_t obj, int kind, uint64_t dest) {
    au_lock();
    AUT("audio: sub mix %d destination %s %#llx (mix %d, splitter %d)", mix_index(obj), kind == DEST_MIX ? "mix" : "splitter",
        (unsigned long long)dest, mix_index(dest), get_handle(dest, K_SPLITTER));
    if (mix_of(obj)) {
        const Cmd k = {.op = OP_MIX_DEST, .idx = mix_index(obj), .tgt = kind == DEST_MIX ? mix_index(dest) : -1, .i0 = kind};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio20SetSubMixDestinationEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEPNS0_12FinalMixTypeE) { set_mix_dest(ARG(1), DEST_MIX, ARG(2)); }
HLE(_ZN2nn5audio20SetSubMixDestinationEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeES4_) { set_mix_dest(ARG(1), DEST_MIX, ARG(2)); }
HLE(_ZN2nn5audio20SetSubMixDestinationEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEPNS0_12SplitterTypeE) {
    set_mix_dest(ARG(1), DEST_SPLITTER, ARG(2));
}
HLE(_ZN2nn5audio22ClearSubMixDestinationEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeE) { set_mix_dest(ARG(1), DEST_MIX, 0); }
/* SetSubMixMixVolume(SubMixType*, destination, float volume, int src, int dst) */
static void set_submix_volume(Ctx *c) {
    au_lock();
    const int src = (int)ARG(2), dst = (int)ARG(3);
    if (mix_of(ARG(0)) && src >= 0 && src < MAX_BUF && dst >= 0 && dst < MAX_BUF) {
        const Cmd k = {.op = OP_MIX_MIX_VOLUME, .idx = mix_index(ARG(0)), .i0 = src, .i1 = dst, .f = farg(c, 0)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio18SetSubMixMixVolumeEPNS0_10SubMixTypeEPNS0_12FinalMixTypeEfii) { set_submix_volume(c); }
HLE(_ZN2nn5audio18SetSubMixMixVolumeEPNS0_10SubMixTypeES2_fii) { set_submix_volume(c); }

/* ---- splitters -------------------------------------------------------------------------------------------------- */
/* AcquireSplitter(config, SplitterType*, int sampleRate, int sourceChannelCount, int destinationCount) */
HLE(_ZN2nn5audio15AcquireSplitterEPNS0_19AudioRendererConfigEPNS0_12SplitterTypeEiii) {
    au_lock();
    int i = 0;
    while (i < MAX_SPLITTERS && splitters[i].used) i++;
    if (i == MAX_SPLITTERS) { au_unlock(); RET(0); return; }
    memset(&splitters[i], 0, sizeof splitters[i]);
    splitters[i].used = 1;
    splitters[i].channels = (int)ARG(3);
    splitters[i].dests = (int)ARG(4) > MAX_SPLIT_DESTS ? MAX_SPLIT_DESTS : (int)ARG(4);
    for (int d = 0; d < MAX_SPLIT_DESTS; d++) splitters[i].dest_mix[d] = -1;
    put_handle(ARG(1), K_SPLITTER, i);
    au_unlock();
    RET(1);
}
HLE(_ZN2nn5audio15ReleaseSplitterEPNS0_19AudioRendererConfigEPNS0_12SplitterTypeE) {
    au_lock();
    Splitter *s = splitter_of(ARG(1));
    if (s) {
        s->released = 1;
        const Cmd k = {.op = OP_SPLIT_RELEASE, .idx = (int)(s - splitters)};
        submit(&k);
    }
    au_unlock();
}
/* SetSplitterDestination(config, SplitterType*, int destinationIndex, mix) */
static void set_split_dest(Ctx *c) {
    au_lock();
    const int d = (int)ARG(2);
    if (splitter_of(ARG(1)) && d >= 0 && d < MAX_SPLIT_DESTS) {
        const Cmd k = {.op = OP_SPLIT_DEST, .idx = splitter_index(ARG(1)), .tgt = mix_index(ARG(3)), .i0 = d};
        submit(&k);
    }
    AUT("audio: splitter %d destination %d = mix %d", get_handle(ARG(1), K_SPLITTER), d, mix_index(ARG(3)));
    au_unlock();
}
HLE(_ZN2nn5audio22SetSplitterDestinationEPNS0_19AudioRendererConfigEPNS0_12SplitterTypeEiPNS0_10SubMixTypeE) { set_split_dest(c); }
HLE(_ZN2nn5audio22SetSplitterDestinationEPNS0_19AudioRendererConfigEPNS0_12SplitterTypeEiPNS0_12FinalMixTypeE) { set_split_dest(c); }
/* SetSplitterMixVolume(SplitterType*, int destinationIndex, mix, float volume, int src, int dst) */
static void set_split_volume(Ctx *c) {
    au_lock();
    const int d = (int)ARG(1), src = (int)ARG(3), dst = (int)ARG(4);
    if (splitter_of(ARG(0)) && d >= 0 && d < MAX_SPLIT_DESTS && src >= 0 && src < MAX_CH && dst >= 0 && dst < MAX_BUF) {
        const Cmd k = {.op = OP_SPLIT_VOLUME, .idx = splitter_index(ARG(0)), .tgt = mix_index(ARG(2)), .i0 = d, .i1 = src,
                       .i2 = dst, .f = farg(c, 0)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio20SetSplitterMixVolumeEPNS0_12SplitterTypeEiPNS0_10SubMixTypeEfii) { set_split_volume(c); }
HLE(_ZN2nn5audio20SetSplitterMixVolumeEPNS0_12SplitterTypeEiPNS0_12FinalMixTypeEfii) { set_split_volume(c); }

/* Float biquad parameters: bool enable @0, float numerator[3] @4, denominator[2] @16.
   Copy now: callers reuse the parameter memory before RequestUpdateAudioRenderer. */
static void read_float_biquad(Cmd *k, uint64_t p) {
    memcpy(k->b, GP(p + 4), sizeof k->b);
    memcpy(k->a, GP(p + 16), sizeof k->a);
}
static void set_split_biquad(Ctx *c) {
    au_lock();
    const int d = (int)ARG(1), f = (int)ARG(3), mix = mix_index(ARG(2));
    Splitter *s = splitter_of(ARG(0));
    if (s && d >= 0 && d < s->dests && f >= 0 && f < 2 && mix >= 0 && ARG(4)) {
        Cmd k = {.op = OP_SPLIT_BIQUAD, .idx = (int)(s - splitters), .tgt = mix,
                 .i0 = d, .i1 = f, .i2 = *(uint8_t *)GP(ARG(4)) != 0};
        read_float_biquad(&k, ARG(4));
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio32SetSplitterBiquadFilterParameterEPNS0_12SplitterTypeEiPNS0_10SubMixTypeEiRKNS0_22TBiquadFilterParameterIfEE) { set_split_biquad(c); }
HLE(_ZN2nn5audio32SetSplitterBiquadFilterParameterEPNS0_12SplitterTypeEiPNS0_12FinalMixTypeEiRKNS0_22TBiquadFilterParameterIfEE) { set_split_biquad(c); }

/* ---- the device sink: AddDeviceSink(config, DeviceSinkType*, FinalMixType*, const int8* input, int count, name) - */
HLE(_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc) {
    au_lock();
    sink_mix = mix_index(ARG(2));
    sink_n = (int)ARG(4) > 8 ? 8 : (int)ARG(4);
    memcpy(sink_in, GP(ARG(3)), (size_t)(sink_n > 0 ? sink_n : 0));
    put_handle(ARG(1), K_SINK, 0);
    hos_log("audio: device sink on mix %d, %d channels (%d %d %d %d %d %d)", sink_mix, sink_n, sink_in[0], sink_in[1],
            sink_in[2], sink_in[3], sink_in[4], sink_in[5]);
    au_unlock();
    RET(0);
}
HLE(_ZN2nn5audio16RemoveDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeE) {
    au_lock();
    sink_mix = -1;
    au_unlock();
}

/* ---- voices ----------------------------------------------------------------------------------------------------- */
/* AcquireVoiceSlot(config, VoiceType*, int sampleRate, int channelCount, SampleFormat, int priority,
                    const void* parameter, size_t parameterSize, const BehaviorOptions*) */
HLE(_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvmPKNS3_15BehaviorOptionsE) {
    au_lock();
    int i = 0;
    while (i < MAX_VOICES && voices[i].used) i++;
    if (i == MAX_VOICES) { au_unlock(); RET(0); return; }
    Voice *v = &voices[i];
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->guest = ARG(1);
    v->rate = (int)ARG(2) > 0 ? (int)ARG(2) : RATE;
    v->channels = (int)ARG(3) < 1 ? 1 : (int)ARG(3) > MAX_CH ? MAX_CH : (int)ARG(3);
    v->format = (int)ARG(4);
    v->param = ARG(6);
    if (v->param && ARG(7) >= 32) memcpy(v->coefs, GP(v->param), 32);
    v->volume = 1.0f;
    v->pitch = 1.0f;
    v->state = PLAY_STOPPED;
    put_handle(ARG(1), K_VOICE, i);
    static int logged;
    if (logged++ < 12 || au_trace > 0)
        hos_log("audio: voice %d: %d Hz, %d channels, format %d, parameter %#llx (%llu bytes) at %.1f s", i, v->rate, v->channels,
                v->format, (unsigned long long)ARG(6), (unsigned long long)ARG(7), hos_now_ns() / 1e9);
    au_unlock();
    RET(1);
}
HLE(_ZN2nn5audio16ReleaseVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeE) {
    au_lock();
    Voice *v = voice_of(ARG(1));
    if (v) {
        v->released = 1;
        const Cmd k = {.op = OP_VOICE_RELEASE, .idx = (int)(v - voices)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio12IsVoiceValidEPKNS0_9VoiceTypeE) {
    au_lock();
    RET(voice_of(ARG(0)) != NULL);
    au_unlock();
}
static void set_voice_dest(Ctx *c, int kind) {
    au_lock();
    if (voice_of(ARG(1))) {
        const Cmd k = {.op = OP_VOICE_DEST, .idx = voice_index(ARG(1)), .i0 = kind,
                       .tgt = kind == DEST_MIX ? mix_index(ARG(2)) : splitter_index(ARG(2))};
        submit(&k);
    }
    au_unlock();
    RET(1);
}
HLE(_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_10SubMixTypeE) { set_voice_dest(c, DEST_MIX); }
HLE(_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12FinalMixTypeE) { set_voice_dest(c, DEST_MIX); }
HLE(_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12SplitterTypeE) { set_voice_dest(c, DEST_SPLITTER); }
HLE(_ZN2nn5audio21ClearVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeE) {
    au_lock();
    if (voice_of(ARG(1))) {
        const Cmd k = {.op = OP_VOICE_DEST, .idx = voice_index(ARG(1)), .tgt = -1, .i0 = DEST_NONE};
        submit(&k);
    }
    au_unlock();
}
/* SetVoiceMixVolume(VoiceType*, mix, float volume, int src, int dst) */
static void set_voice_mix_volume(Ctx *c) {
    au_lock();
    const int src = (int)ARG(2), dst = (int)ARG(3);
    if (voice_of(ARG(0)) && src >= 0 && src < MAX_CH && dst >= 0 && dst < MAX_BUF) {
        const Cmd k = {.op = OP_VOICE_MIX_VOLUME, .idx = voice_index(ARG(0)), .i0 = src, .i1 = dst, .f = farg(c, 0)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_10SubMixTypeEfii) { set_voice_mix_volume(c); }
HLE(_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii) { set_voice_mix_volume(c); }
HLE(_ZN2nn5audio14SetVoiceVolumeEPNS0_9VoiceTypeEf) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    if (v) {
        if (v->volume != farg(c, 0)) AUT("audio: voice %d volume %.3f", (int)(v - voices), farg(c, 0));
        const Cmd k = {.op = OP_VOICE_VOLUME, .idx = (int)(v - voices), .f = farg(c, 0)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio13SetVoicePitchEPNS0_9VoiceTypeEf) {
    au_lock();
    if (voice_of(ARG(0))) {
        const Cmd k = {.op = OP_VOICE_PITCH, .idx = voice_index(ARG(0)), .f = farg(c, 0)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio16SetVoicePriorityEPNS0_9VoiceTypeEi) {}
/* BiquadFilterParameter: bool enable @0, int16 numerator[3] @2, int16 denominator[2] @8 (Q14) */
HLE(_ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_21BiquadFilterParameterE) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    const int f = (int)ARG(1);
    if (v && f >= 0 && f < 2 && ARG(2)) {
        const uint64_t p = ARG(2);
        Cmd k = {.op = OP_VOICE_BIQUAD, .idx = (int)(v - voices), .i0 = f, .i1 = *(uint8_t *)GP(p) != 0};
        for (int n = 0; n < 3; n++) k.b[n] = *(int16_t *)GP(p + 2 + 2 * n) / 16384.0f;
        for (int n = 0; n < 2; n++) k.a[n] = *(int16_t *)GP(p + 8 + 2 * n) / 16384.0f;
        submit(&k);
        static int logged;
        if (k.i1 && logged++ < 8) AUT("audio: voice biquad %d: b %.3f %.3f %.3f a %.3f %.3f", f, k.b[0], k.b[1], k.b[2], k.a[0], k.a[1]);
    }
    au_unlock();
}
/* TBiquadFilterParameter<float>: bool @0, numerator[3] @4, additive-feedback denominator[2] @16. */
HLE(_ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_22TBiquadFilterParameterIfEE) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    const int f = (int)ARG(1);
    if (v && f >= 0 && f < 2 && ARG(2)) {
        Cmd k = {.op = OP_VOICE_BIQUAD, .idx = (int)(v - voices), .i0 = f, .i1 = *(uint8_t *)GP(ARG(2)) != 0};
        read_float_biquad(&k, ARG(2));
        submit(&k);
    }
    au_unlock();
}
/* SetVoicePlayState(VoiceType*, PlayState): Started 0, Stopped 1, Paused 2 */
HLE(_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    if (v) {
        const int s = (int)ARG(1);
        if (v->state != s) AUT("audio: voice %d %s", (int)(v - voices), s == 0 ? "start" : s == 1 ? "stop" : "pause");
        const Cmd k = {.op = OP_VOICE_STATE, .idx = (int)(v - voices), .i0 = s};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    int ok = 0;
    if (v && v->nwb + v->pending_wb < MAX_WB) {
        {   /* HOS_AUDIO_WB_DUMP=<dir>: the first 40 wave buffers' data and the voice's coefficients (debugging) */
            static const char *dir;
            static int init, nd;
            if (!init) { init = 1; dir = getenv("HOS_AUDIO_WB_DUMP"); }
            if (dir && nd < 40) {
                const WB w = read_wb(ARG(1));
                char path[512];
                snprintf(path, sizeof path, "%s/wb%02d_v%d_f%d_r%d_s%d_e%d.bin", dir, nd++, (int)(v - voices), v->format, v->rate,
                         w.start, w.end);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(v->coefs, 1, 32, f);
                    fwrite(GP(w.buffer), 1, (size_t)w.size, f);
                    fclose(f);
                }
            }
        }
        const Cmd k = {.op = OP_VOICE_APPEND, .idx = (int)(v - voices), .wbp = ARG(1), .wb = read_wb(ARG(1))};
        v->pending_wb++;
        submit(&k);
        ok = 1;
        static int logged;
        if (logged++ < 8) {
            const WB w = read_wb(ARG(1));
            hos_log("audio: wave buffer %#llx: data %#llx size %llu samples %d..%d loop %d context %#llx (voice format %d)",
                    (unsigned long long)ARG(1), (unsigned long long)w.buffer, (unsigned long long)w.size, w.start, w.end,
                    w.loop, (unsigned long long)w.context, v->format);
        }
    }
    au_unlock();
    RET(ok);
}
HLE(_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    uint64_t r = 0;
    if (v && v->nrel_seen) {   /* (those released by the last update: see "parameter updates") */
        r = v->rel[0];
        memmove(v->rel, v->rel + 1, (size_t)(v->nrel - 1) * sizeof v->rel[0]);
        v->nrel--;
        v->nrel_seen--;
    }
    au_unlock();
    RET(r);
}
HLE(_ZN2nn5audio18GetWaveBufferCountEPKNS0_9VoiceTypeE) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    RET(v ? v->nwb + v->pending_wb + (v->nrel - v->nrel_seen) : 0);   /* (finished since the last update: still queued) */
    au_unlock();
}
HLE(_ZN2nn5audio25GetVoicePlayedSampleCountEPKNS0_9VoiceTypeE) {
    au_lock();
    Voice *v = voice_of(ARG(0));
    RET(v ? v->played : 0);
    au_unlock();
}
HLE(_ZN2nn5audio20IsVoiceDroppedFlagOnEPKNS0_9VoiceTypeE) { RET(0); }

/* ---- Aux API --------------------------------------------------------------------------------------------------- */
static Aux *aux_of(uint64_t obj) {
    const int i = get_handle(obj, K_AUX);
    return i >= 0 && i < MAX_EFFECTS && auxes[i].used && !auxes[i].released ? &auxes[i] : NULL;
}
static void add_aux(Ctx *c) {
    au_lock();
    int e = 0;
    while (e < MAX_EFFECTS && auxes[e].used) e++;
    const int m = mix_index(ARG(2));
    const uint64_t bytes = ARG(5);
    if (e == MAX_EFFECTS || m < 0 || !ARG(1) || !ARG(3) || !ARG(4) ||
        bytes < 128 + FRAME * sizeof(int32_t) || bytes > INT32_MAX ||
        (ARG(3) < ARG(4) ? ARG(4) - ARG(3) : ARG(3) - ARG(4)) < bytes) {
        au_unlock();
        RET(0x1A9 << 9 | 153);
        return;
    }
    Aux *a = &auxes[e];
    memset(a, 0, sizeof *a);
    a->used = a->enabled = 1;
    a->mix = m;
    a->order = ++effect_order;
    a->sample_count = (int)(((bytes - 128) / sizeof(int32_t)) & ~15ULL) / FRAME * FRAME;
    memset(GP(ARG(3)), 0, (size_t)bytes);
    memset(GP(ARG(4)), 0, (size_t)bytes);
    a->send.data = (int32_t *)GP(ARG(3) + 128);
    a->ret.data = (int32_t *)GP(ARG(4) + 128);
    put_handle(ARG(1), K_AUX, e);
    const Cmd k = {.op = OP_AUX_ENABLE, .idx = e, .i0 = 1};
    submit(&k);
    AUT("audio: aux %d on mix %d, %d samples", e, m, a->sample_count);
    au_unlock();
    RET(0);
}
HLE(_ZN2nn5audio6AddAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_10SubMixTypeEPvS7_m) { add_aux(c); }
HLE(_ZN2nn5audio6AddAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_12FinalMixTypeEPvS7_m) { add_aux(c); }
static void remove_aux(Ctx *c) {
    au_lock();
    Aux *a = aux_of(ARG(1));
    if (a) {
        a->released = 1;
        const Cmd k = {.op = OP_AUX_RELEASE, .idx = (int)(a - auxes)};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio9RemoveAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_10SubMixTypeE) { remove_aux(c); }
HLE(_ZN2nn5audio9RemoveAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_12FinalMixTypeE) { remove_aux(c); }
HLE(_ZN2nn5audio13SetAuxEnabledEPNS0_7AuxTypeEb) {
    au_lock();
    Aux *a = aux_of(ARG(0));
    if (a) {
        a->enabled = (int)ARG(1) & 1;
        const Cmd k = {.op = OP_AUX_ENABLE, .idx = (int)(a - auxes), .i0 = a->enabled};
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio12IsAuxEnabledEPKNS0_7AuxTypeE) {
    au_lock();
    const Aux *a = aux_of(ARG(0));
    RET(a ? a->enabled : 0);
    au_unlock();
}
HLE(_ZN2nn5audio14IsAuxRemovableEPKNS0_7AuxTypeE) {
    au_lock();
    const Aux *a = aux_of(ARG(0));
    RET(a && !a->enabled && !a->active_enabled);
    au_unlock();
}
HLE(_ZN2nn5audio17SetAuxInputOutputEPNS0_7AuxTypeEPKaS4_i) {
    au_lock();
    Aux *a = aux_of(ARG(0));
    int n = (int)ARG(3);
    if (a && a->mix >= 0 && n >= 0 && n <= MAX_BUF && (n == 0 || (ARG(1) && ARG(2)))) {
        if (n > mixes[a->mix].buffers) n = mixes[a->mix].buffers;
        Cmd k = {.op = OP_AUX_MAP, .idx = (int)(a - auxes), .i0 = n};
        if (n) {
            memcpy(k.in, GP(ARG(1)), (size_t)n);
            memcpy(k.out, GP(ARG(2)), (size_t)n);
        }
        a->n = n;
        memcpy(a->in, k.in, (size_t)n);
        memcpy(a->out, k.out, (size_t)n);
        submit(&k);
    }
    au_unlock();
}
HLE(_ZN2nn5audio17GetAuxInputOutputEPKNS0_7AuxTypeEPaS4_Pii) {
    au_lock();
    const Aux *a = aux_of(ARG(0));
    int n = a ? a->n : 0;
    if (n > (int)ARG(4)) n = (int)ARG(4);
    if (n < 0) n = 0;
    if (n && ARG(1) && ARG(2)) {
        memcpy(GP(ARG(1)), a->in, (size_t)n);
        memcpy(GP(ARG(2)), a->out, (size_t)n);
    }
    if (ARG(3)) G32(ARG(3)) = n;
    au_unlock();
}
HLE(_ZN2nn5audio17ReadAuxSendBufferEPNS0_7AuxTypeEPii) {
    au_lock();
    Aux *a = aux_of(ARG(0));
    RET(a && ARG(1) ? aux_ring_read(&a->send, (int32_t *)GP(ARG(1)), (int)ARG(2)) : 0);
    au_unlock();
}
HLE(_ZN2nn5audio20WriteAuxReturnBufferEPNS0_7AuxTypeEPKii) {
    au_lock();
    Aux *a = aux_of(ARG(0));
    RET(a && ARG(1) ? aux_ring_write(&a->ret, (const int32_t *)GP(ARG(1)), (int)ARG(2)) : 0);
    au_unlock();
}
HLE(_ZN2nn5audio17GetAuxSampleCountEPKNS0_7AuxTypeE) {
    au_lock();
    const Aux *a = aux_of(ARG(0));
    RET(a ? a->sample_count : 0);  /* allocation capacity, not the number currently waiting to be read */
    au_unlock();
}

/* ---- other effects --------------------------------------------------------------------------------------------- */
HLE(_ZN2nn5audio17IsReverbRemovableEPKNS0_10ReverbTypeE) { RET(1); }
HLE(_ZN2nn5audio22IsBufferMixerRemovableEPKNS0_15BufferMixerTypeE) { RET(1); }
HLE(_ZN2nn5audio22IsI3dl2ReverbRemovableEPKNS0_15I3dl2ReverbTypeE) { RET(1); }
HLE(_ZN2nn5audio23IsBiquadFilterRemovableEPKNS0_16BiquadFilterTypeE) { RET(1); }
HLE(_ZN2nn5audio23IsLightLimiterRemovableEPKNS0_16LightLimiterTypeE) { RET(1); }

/* ---- the light limiter on the final mix: input gain, then a peak limiter at the threshold, then output gain ---- */
HLE(_ZN2nn5audio24SetLightLimiterInputGainEPNS0_16LightLimiterTypeEf) { lim_in = farg(c, 0); AUT("audio: limiter input gain %.3f", lim_in); }
HLE(_ZN2nn5audio25SetLightLimiterOutputGainEPNS0_16LightLimiterTypeEf) { lim_out = farg(c, 0); AUT("audio: limiter output gain %.3f", lim_out); }
HLE(_ZN2nn5audio24SetLightLimiterThresholdEPNS0_16LightLimiterTypeEf) { lim_threshold = farg(c, 0); AUT("audio: limiter threshold %.3f", lim_threshold); }
HLE(_ZN2nn5audio22SetLightLimiterEnabledEPNS0_16LightLimiterTypeEb) { lim_enabled = (int)(ARG(1) & 1); AUT("audio: limiter %s", lim_enabled ? "on" : "off"); }
HLE(_ZN2nn5audio21IsLightLimiterEnabledEPKNS0_16LightLimiterTypeE) { RET(lim_enabled); }

/* ---- I3DL2 reverb: AddI3dl2Reverb(config, I3dl2ReverbType*, buffer, size, SubMixType* / FinalMixType*, channels) - */
static Reverb *reverb_of(uint64_t obj) {
    const int i = get_handle(obj, K_EFFECT);
    return i >= 0 && i < MAX_EFFECTS && reverbs[i].used ? &reverbs[i] : NULL;
}
static void add_reverb(Ctx *c) {
    au_lock();
    int e = 0;
    while (e < MAX_EFFECTS && reverbs[e].used) e++;
    const int m = mix_index(ARG(4));
    if (e == MAX_EFFECTS || m < 0) {
        au_unlock();
        AUT("audio: i3dl2 reverb refused (mix %d)", m);
        RET(0x1A9 << 9 | 153);   /* (out of effects: an audio result) */
        return;
    }
    Reverb *rv = &reverbs[e];
    memset(rv, 0, sizeof *rv);
    rv->used = 1;
    rv->order = ++effect_order;
    rv->mix = m;
    rv->n = (int)ARG(5) > MAX_CH ? MAX_CH : (int)ARG(5);
    for (int k = 0; k < MAX_CH; k++) rv->in[k] = rv->out[k] = (int8_t)k;
    /* the I3DL2 defaults */
    rv->room = -1000; rv->room_hf = -100; rv->refl = -2602; rv->reverb = 200; rv->hf_ratio = 0.83f;
    rv->diffusion = 100; rv->density = 100; rv->hf_ref = 5000; rv->dry = 1.0f;
    rv->decay_ns = 1490000000ll; rv->refl_delay_ns = 7000000; rv->late_delay_ns = 11000000;
    put_handle(ARG(1), K_EFFECT, e);
    au_unlock();
    AUT("audio: i3dl2 reverb %d on mix %d, %d channels", e, m, rv->n);
    RET(0);
}
HLE(_ZN2nn5audio14AddI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPvmPNS0_10SubMixTypeEi) { add_reverb(c); }
HLE(_ZN2nn5audio14AddI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPvmPNS0_12FinalMixTypeEi) { add_reverb(c); }
static void ret_float(Ctx *c, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    memset(&c->vreg[0], 0, sizeof c->vreg[0]);
    memcpy(&c->vreg[0][0], &bits, 4);
}
#define REVERB_FLOAT(NAME, field) \
    HLE(_ZN2nn5audio##NAME##EPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->field = farg(c, 0); }
#define REVERB_FLOAT_GET(NAME, field) \
    HLE(_ZN2nn5audio##NAME##EPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->field : 0.0f); }
#define REVERB_TIME(NAME, field) \
    HLE(_ZN2nn5audio##NAME##EPNS0_15I3dl2ReverbTypeENS_8TimeSpanE) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->field = (int64_t)ARG(1); }
#define REVERB_TIME_GET(NAME, field) \
    HLE(_ZN2nn5audio##NAME##EPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); RET(rv ? (uint64_t)rv->field : 0); }
HLE(_ZN2nn5audio22SetI3dl2ReverbRoomGainEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->room = farg(c, 0); }
HLE(_ZN2nn5audio22GetI3dl2ReverbRoomGainEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->room : 0.0f); }
HLE(_ZN2nn5audio24SetI3dl2ReverbRoomHfGainEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->room_hf = farg(c, 0); }
HLE(_ZN2nn5audio24GetI3dl2ReverbRoomHfGainEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->room_hf : 0.0f); }
HLE(_ZN2nn5audio29SetI3dl2ReverbReflectionsGainEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->refl = farg(c, 0); }
HLE(_ZN2nn5audio29GetI3dl2ReverbReflectionsGainEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->refl : 0.0f); }
HLE(_ZN2nn5audio24SetI3dl2ReverbReverbGainEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->reverb = farg(c, 0); }
HLE(_ZN2nn5audio24GetI3dl2ReverbReverbGainEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->reverb : 0.0f); }
HLE(_ZN2nn5audio36SetI3dl2ReverbLateReverbHfDecayRatioEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->hf_ratio = farg(c, 0); }
HLE(_ZN2nn5audio36GetI3dl2ReverbLateReverbHfDecayRatioEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->hf_ratio : 0.0f); }
HLE(_ZN2nn5audio33SetI3dl2ReverbLateReverbDiffusionEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->diffusion = farg(c, 0); }
HLE(_ZN2nn5audio33GetI3dl2ReverbLateReverbDiffusionEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->diffusion : 0.0f); }
HLE(_ZN2nn5audio31SetI3dl2ReverbLateReverbDensityEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->density = farg(c, 0); }
HLE(_ZN2nn5audio31GetI3dl2ReverbLateReverbDensityEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->density : 0.0f); }
HLE(_ZN2nn5audio25SetI3dl2ReverbHfReferenceEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->hf_ref = farg(c, 0); }
HLE(_ZN2nn5audio25GetI3dl2ReverbHfReferenceEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->hf_ref : 0.0f); }
HLE(_ZN2nn5audio21SetI3dl2ReverbDryGainEPNS0_15I3dl2ReverbTypeEf) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->dry = farg(c, 0); }
HLE(_ZN2nn5audio21GetI3dl2ReverbDryGainEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); ret_float(c, rv ? rv->dry : 0.0f); }
HLE(_ZN2nn5audio33SetI3dl2ReverbLateReverbDecayTimeEPNS0_15I3dl2ReverbTypeENS_8TimeSpanE) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->decay_ns = (int64_t)ARG(1); }
HLE(_ZN2nn5audio33GetI3dl2ReverbLateReverbDecayTimeEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); RET(rv ? (uint64_t)rv->decay_ns : 0); }
HLE(_ZN2nn5audio33SetI3dl2ReverbReflectionDelayTimeEPNS0_15I3dl2ReverbTypeENS_8TimeSpanE) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->refl_delay_ns = (int64_t)ARG(1); }
HLE(_ZN2nn5audio33GetI3dl2ReverbReflectionDelayTimeEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); RET(rv ? (uint64_t)rv->refl_delay_ns : 0); }
HLE(_ZN2nn5audio33SetI3dl2ReverbLateReverbDelayTimeEPNS0_15I3dl2ReverbTypeENS_8TimeSpanE) { Reverb *rv = reverb_of(ARG(0)); if (rv) rv->late_delay_ns = (int64_t)ARG(1); }
HLE(_ZN2nn5audio33GetI3dl2ReverbLateReverbDelayTimeEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); RET(rv ? (uint64_t)rv->late_delay_ns : 0); }
HLE(_ZN2nn5audio21SetI3dl2ReverbEnabledEPNS0_15I3dl2ReverbTypeEb) {
    Reverb *rv = reverb_of(ARG(0));
    if (rv) rv->enabled = (int)(ARG(1) & 1);
    AUT("audio: i3dl2 reverb %s", ARG(1) & 1 ? "on" : "off");
}
HLE(_ZN2nn5audio20IsI3dl2ReverbEnabledEPKNS0_15I3dl2ReverbTypeE) { Reverb *rv = reverb_of(ARG(0)); RET(rv ? rv->enabled : 0); }
HLE(_ZN2nn5audio25SetI3dl2ReverbInputOutputEPNS0_15I3dl2ReverbTypeEPKaS4_i) {
    Reverb *rv = reverb_of(ARG(0));
    if (!rv) return;
    const int n = (int)ARG(3) > MAX_CH ? MAX_CH : (int)ARG(3);
    for (int k = 0; k < n; k++) {
        rv->in[k] = *(int8_t *)GP(ARG(1) + k);
        rv->out[k] = *(int8_t *)GP(ARG(2) + k);
    }
    rv->n = n;
    AUT("audio: i3dl2 reverb %d channels: in %d %d out %d %d", n, rv->in[0], rv->in[1], rv->out[0], rv->out[1]);
}
HLE(_ZN2nn5audio25GetI3dl2ReverbInputOutputEPKNS0_15I3dl2ReverbTypeEPaS4_Pii) {
    Reverb *rv = reverb_of(ARG(0));
    const int n = rv ? rv->n : 0;
    for (int k = 0; k < n && k < (int)ARG(4); k++) {
        *(int8_t *)GP(ARG(1) + k) = rv->in[k];
        *(int8_t *)GP(ARG(2) + k) = rv->out[k];
    }
    if (ARG(3)) G32(ARG(3)) = (uint32_t)n;
}
