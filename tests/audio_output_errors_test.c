/* Synthetic device calls around the real nn_audio.c output section. No device or game is opened. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *HWAVEOUT;
typedef char *LPSTR;
typedef unsigned MMRESULT;
typedef struct {
    unsigned wFormatTag, nChannels, nSamplesPerSec, wBitsPerSample, nBlockAlign, nAvgBytesPerSec;
} WAVEFORMATEX;
typedef struct { LPSTR lpData; unsigned dwBufferLength, dwFlags; } WAVEHDR;
#define MMSYSERR_NOERROR 0
#define WHDR_DONE 1
#define WHDR_PREPARED 2
#define WHDR_INQUEUE 16
#define WAVE_MAPPER ((unsigned)-1)
#define WAVE_FORMAT_PCM 1
#define CALLBACK_NULL 0
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)

static const char *scenario;
static int prepare_calls, write_calls, accepted, unprepare_calls, open_calls, close_calls, reset_calls, logs;
static unsigned last_device, last_channels;
static uint64_t now_ms;
static int fail_prepare, fail_write, fail_unprepare, fail_open, fail_open6;
static int gone_write_at, gone_unprepare, opens_fail;   /* (the device gone: "no driver" 6, "invalid handle" 5) */
static uint32_t default_key = 11;                       /* (Windows' default output, as au_default_key says) */
static int queued[10];
static WAVEHDR *known[10];
static char last_log[512];

static const char *test_getenv(const char *name) { (void)name; return NULL; }
static void hos_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof last_log, format, args);
    va_end(args);
    logs++;
}
static int slot(WAVEHDR *h) {
    for (int i = 0; i < 10; i++) if (known[i] == h) return i;
    for (int i = 0; i < 10; i++) if (!known[i]) { known[i] = h; return i; }
    CHECK(0);
    return -1;
}
static MMRESULT test_open(HWAVEOUT *out, unsigned device, const WAVEFORMATEX *f,
                          uintptr_t callback, uintptr_t instance, unsigned flags) {
    (void)callback; (void)instance; (void)flags;
    last_device = device;
    CHECK((f->nChannels == 2 || f->nChannels == 6) && f->nSamplesPerSec == 48000 && f->wBitsPerSample == 16);
    CHECK(f->nBlockAlign == 2 * f->nChannels && f->nAvgBytesPerSec == 48000 * f->nBlockAlign);
    last_channels = f->nChannels;
    open_calls++;
    if (opens_fail) return 6;   /* (MMSYSERR_NODRIVER: the audio service down) */
    if (fail_open || (fail_open6 && f->nChannels == 6)) return 7;   /* (MMSYSERR_NOMEM; a device without 5.1: 32) */
    *out = (void *)(uintptr_t)1;
    return 0;
}
static MMRESULT test_prepare(HWAVEOUT out, WAVEHDR *h, unsigned size) {
    (void)out; (void)size;
    CHECK(!queued[slot(h)]);
    prepare_calls++;
    if (fail_prepare && (fail_prepare < 0 || prepare_calls == 1)) return 7;
    h->dwFlags |= WHDR_PREPARED;
    return 0;
}
static MMRESULT test_write(HWAVEOUT out, WAVEHDR *h, unsigned size) {
    (void)out; (void)size;
    const int i = slot(h);
    CHECK(!queued[i]);
    write_calls++;
    if (gone_write_at && write_calls == gone_write_at) return 6;   /* (MMSYSERR_NODRIVER) */
    if (!(h->dwFlags & WHDR_PREPARED)) return 34; /* WAVERR_UNPREPARED: no completion follows. */
    if (fail_write && write_calls == 1) return 7; /* MMSYSERR_NOMEM: no buffer was queued. */
    queued[i] = 1;
    h->dwFlags = (h->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
    accepted++;
    return 0;
}
static MMRESULT test_unprepare(HWAVEOUT out, WAVEHDR *h, unsigned size) {
    (void)out; (void)size;
    CHECK(!queued[slot(h)]);
    unprepare_calls++;
    if (gone_unprepare) return 5;   /* (MMSYSERR_INVALHANDLE, every call until the handle is closed) */
    if (fail_unprepare && unprepare_calls == 1) return 33; /* WAVERR_STILLPLAYING: do not reuse. */
    h->dwFlags &= ~WHDR_PREPARED;
    return 0;
}
static MMRESULT test_reset(HWAVEOUT out) {
    (void)out;
    reset_calls++;
    for (int i = 0; i < 10; i++) if (queued[i]) {   /* (waveOutReset: every queued block marked done) */
        queued[i] = 0;
        known[i]->dwFlags = (known[i]->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
    }
    return 0;
}
static MMRESULT test_close(HWAVEOUT out) { (void)out; close_calls++; gone_unprepare = 0; return 0; }
/* (the platform helpers nn_audio.c defines before its output section) */
static unsigned au_device_count(void) { return 2; }
static void au_device_name(unsigned device, char *out, size_t size) { snprintf(out, size, "test device %u", device); }
static uint64_t au_ms(void) { return now_ms; }
static uint32_t au_default_key(void) { return default_key; }
static void au_opened(HWAVEOUT w) { (void)w; }
static int speakers = 2;   /* (the game's speakers: 6 in the "surround" and "fallback" cases) */
static int au_speakers(void) { return speakers; }
static const WAVEFORMATEX *au_format(const WAVEFORMATEX *f) { return f; }
#define getenv test_getenv
#define env_on(name) (getenv(name) && !env_value_off(getenv(name)))   /* (the switches read through the stand-in above) */
#define waveOutOpen test_open
#define waveOutPrepareHeader test_prepare
#define waveOutWrite test_write
#define waveOutUnprepareHeader test_unprepare
#define waveOutReset test_reset
#define waveOutClose test_close
#include "audio_output.inc"

static void complete_all(void) {
    for (int i = 0; i < 10; i++) if (queued[i]) {
        queued[i] = 0;
        known[i]->dwFlags = (known[i]->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
    }
}
static void frames(int count, int complete) {
    int16_t pcm[FRAME * 6];
    for (int j = 0; j < count; j++) {
        for (int i = 0; i < FRAME * out_channels; i++) pcm[i] = (int16_t)(j * 31 + i);
        output_frame(pcm);
        if (complete) complete_all();
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    scenario = argv[1];
    fail_prepare = !strcmp(scenario, "prepare");
    fail_write = !strcmp(scenario, "write");
    fail_unprepare = !strcmp(scenario, "unprepare");
    fail_open = !strcmp(scenario, "open");
    if (!strcmp(scenario, "bounded")) fail_prepare = -1;
    if (!strcmp(scenario, "surround") || !strcmp(scenario, "fallback")) speakers = 6;
    fail_open6 = !strcmp(scenario, "fallback");
    output_open();
    if (fail_open6) {
        CHECK(open_calls == 4 && last_channels == 2 && last_device == WAVE_MAPPER);   /* (5.1 on all three, then stereo) */
        CHECK(out_channels == 2 && hdr[0].dwBufferLength == FRAME * 2 * 2 * 2);
        CHECK(strstr(last_log, "default device") && strstr(last_log, "stereo"));
        logs = 0;
        frames(40, 1);
        CHECK(accepted == 20 && !logs);
    } else if (fail_open) {
        CHECK(open_calls == 3);   /* (the default, then both devices; HOS_AUDIO_DEVICE unset) */
        CHECK(!wout);
        frames(40, 1);
        CHECK(!prepare_calls && !write_calls && logs == 3);
        CHECK(strstr(last_log, "waveOutOpen") && strstr(last_log, "7"));
    } else if (open_calls != 1 || last_device != WAVE_MAPPER || logs != 1 || !strstr(last_log, "default device")) {
        CHECK(0);   /* (a working default is opened first, and named in the log) */
    } else if ((logs = 0), !strcmp(scenario, "surround")) {
        CHECK(last_channels == 6 && out_channels == 6 && strstr(last_log, "5.1"));
        CHECK(hdr[0].dwBufferLength == FRAME * 2 * 6 * 2);   /* (two frames of six 16-bit channels) */
        frames(40, 1);
        CHECK(accepted == 20 && prepare_calls == 20 && write_calls == 20 && !logs);
        CHECK(blocks[9][0] == 38 * 31 && blocks[9][FRAME * 6] == 39 * 31);
        CHECK(blocks[9][FRAME * 6 - 1] == (int16_t)(38 * 31 + FRAME * 6 - 1));
    } else if (!strcmp(scenario, "lost")) {
        gone_write_at = 25;              /* a Bluetooth headset off: its 25th block, "no driver" */
        frames(60, 1);
        CHECK(reset_calls == 1 && close_calls == 1 && open_calls == 2 && last_device == WAVE_MAPPER);
        CHECK(accepted >= 28);           /* the default took the blocks after it */
    } else if (!strcmp(scenario, "gone")) {
        frames(20, 1);
        gone_unprepare = 1;              /* the audio service restarted: every call on the old handle "invalid" */
        frames(2, 1);
        CHECK(close_calls == 1 && open_calls == 2 && last_device == WAVE_MAPPER);
        const int before = accepted;
        frames(10, 1);
        CHECK(accepted == before + 5);   /* (before, the same block tried again for good: no sound for the run) */
    } else if (!strcmp(scenario, "service")) {
        frames(10, 1);
        opens_fail = 1;                  /* the service down: no device opens */
        gone_write_at = write_calls + 1;
        frames(2, 1);
        CHECK(close_calls == 1 && !wout && open_calls == 4);   /* (the first open, then the default and both devices) */
        frames(5, 1);
        CHECK(open_calls == 4);          /* not again within the second */
        now_ms += 1000;
        frames(1, 1);
        CHECK(open_calls == 7 && !wout); /* a second later: all three again, the service still down */
        opens_fail = 0;
        now_ms += 1000;
        frames(1, 1);
        CHECK(wout && open_calls == 8 && last_device == WAVE_MAPPER);   /* the service back: the default first */
        const int before = accepted;
        frames(4, 1);
        CHECK(accepted == before + 2);
    } else if (!strcmp(scenario, "default")) {
        frames(10, 1);
        default_key = 12;                /* Windows moves its default (the headset back) */
        now_ms += 500;
        frames(1, 1);
        CHECK(open_calls == 1);          /* looked at once a second */
        now_ms += 600;
        frames(1, 1);
        CHECK(close_calls == 1 && open_calls == 2 && last_device == WAVE_MAPPER && out_default_key == 12);
        now_ms += 2000;
        frames(2, 1);
        CHECK(open_calls == 2);          /* the same default: not opened again */
        const int before = accepted;
        frames(4, 1);
        CHECK(accepted == before + 2);
    } else if (!strcmp(scenario, "stall")) {
        frames(20, 0);                   /* every block queued, none ever completes: the device takes nothing */
        now_ms += 1000;
        frames(1, 0);                    /* the stall noticed */
        CHECK(open_calls == 1);
        now_ms += 1600;
        frames(1, 0);                    /* 1.6 s later: closed, the next candidate (device 0) opened */
        CHECK(reset_calls == 1 && close_calls == 1 && open_calls == 2 && last_device == 0);
        CHECK(strstr(last_log, "test device 0"));
        const int before = accepted;
        frames(4, 1);
        CHECK(accepted == before + 2);   /* the new device takes blocks */
    } else if (fail_prepare > 0 || fail_write) {
        frames(40, 1);
        fprintf(stderr, "ring %s: accepted %d, prepare %d, write %d\n", scenario, accepted, prepare_calls, write_calls);
        CHECK(accepted == 19); /* One failed block, then the same slot works on the next ring. */
        CHECK(fill_block == 0 && fill_frames == 0);
        CHECK(logs == 1);
        if (fail_prepare) CHECK(write_calls == 19); /* Never write an unprepared header. */
    } else if (fail_unprepare) {
        frames(20, 1);
        int16_t before[FRAME * 4];
        memcpy(before, blocks[0], sizeof before);
        frames(1, 0);
        CHECK(fill_frames == 0 && fill_block == 0);
        CHECK(!memcmp(before, blocks[0], sizeof before));
        CHECK(hdr[0].dwFlags == (WHDR_DONE | WHDR_PREPARED));
        frames(2, 1);
        CHECK(accepted == 11 && unprepare_calls == 2 && logs == 1);
    } else if (!strcmp(scenario, "busy")) {
        frames(20, 0);
        int16_t before[FRAME * 4];
        memcpy(before, blocks[0], sizeof before);
        frames(5, 0);
        CHECK(accepted == 10 && fill_frames == 0 && fill_block == 0);
        CHECK(!memcmp(before, blocks[0], sizeof before));
        CHECK(!unprepare_calls);
        complete_all();
        frames(2, 1);
        CHECK(accepted == 11);
    } else if (fail_prepare < 0) {
        frames(80, 1);
        CHECK(prepare_calls == 40 && write_calls == 0 && accepted == 0);
        CHECK(logs > 0 && logs <= 8);
    } else {
        CHECK(!strcmp(scenario, "normal"));
        frames(40, 1);
        CHECK(accepted == 20 && prepare_calls == 20 && write_calls == 20 && unprepare_calls == 10);
        CHECK(fill_block == 0 && fill_frames == 0 && !logs);
        CHECK(blocks[9][0] == 38 * 31 && blocks[9][FRAME * 2] == 39 * 31);
    }
    printf("PASS %s: actual output ring\n", scenario);
    return 0;
}
