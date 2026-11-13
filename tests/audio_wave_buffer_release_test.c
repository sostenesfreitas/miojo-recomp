/* Authored wave buffers and passes around actual source bodies. No audio device or game is opened. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t x[32]; } Ctx;
static _Alignas(8) uint8_t guest[4096];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static void *guest_ptr(uint64_t address) { CHECK(address < sizeof guest); return guest + address; }
#define GP(a) guest_ptr(a)
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(sym) void imp_##sym(Ctx *c)
#define AUT(...) ((void)0)
static void hos_log(const char *format, ...) { (void)format; }

/* Single-threaded test boundaries: the mixer's frames run between the game's calls, as its thread can. */
static int lock_depth;
static void au_lock(void) { CHECK(lock_depth++ == 0); }
static void au_unlock(void) { CHECK(--lock_depth == 0); }
static const char *fixture_getenv(const char *name) { (void)name; return NULL; }
#define getenv fixture_getenv
#define env_on(name) (getenv(name) && !env_value_off(getenv(name)))   /* (the switches read through the stand-in above) */

#include "actual_audio_wave_buffers.inc"

#define APPEND imp__ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE
#define RELEASED imp__ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE
#define COUNT imp__ZN2nn5audio18GetWaveBufferCountEPKNS0_9VoiceTypeE
#define STATE imp__ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE

enum { VOICES = 6, OBJECT = 0x40, BUFFER = 0x200, SAMPLES = 0x400, LENGTH = 64 };
static uint64_t object(int i) { return OBJECT + (uint64_t)i * 16; }
static uint64_t call(void (*hle)(Ctx *), uint64_t a0, uint64_t a1) {
    Ctx c = {0};
    c.x[0] = a0; c.x[1] = a1;
    hle(&c);
    CHECK(lock_depth == 0);
    return c.x[0];
}
static int count(int i) { return (int)call(COUNT, object(i), 0); }
static uint64_t released(int i) { return call(RELEASED, object(i), 0); }
static void reset(void) {
    memset(guest, 0, sizeof guest);
    memset(voices, 0, sizeof voices);
    ncmds = 0;
    /* One authored WaveBuffer struct for every append, as the game does: PCM16, LENGTH samples. */
    G64(BUFFER) = SAMPLES; G64(BUFFER + 8) = LENGTH * 2;
    G32(BUFFER + 0x10) = 0; G32(BUFFER + 0x14) = LENGTH;
    for (int i = 0; i < VOICES; i++) {
        voices[i].used = 1, voices[i].channels = 1, voices[i].format = FMT_PCM16, voices[i].rate = RATE;
        put_handle(object(i), K_VOICE, i);
    }
}
/* the mixer plays voice i through its first queued buffer and into the next: that one is finished */
static void finish_one(int i) {
    for (int s = 0; s <= LENGTH; s++) CHECK(next_source_frame(&voices[i]));
}

static void finished_between_updates(void) {
    reset();
    for (int n = 0; n < 4; n++)
        for (int i = 0; i < VOICES; i++) CHECK(call(APPEND, object(i), BUFFER) == 1);
    for (int i = 0; i < VOICES; i++) CHECK(count(i) == 4);   /* (appended counts at once, before the update) */
    commit();
    for (int i = 0; i < VOICES; i++) CHECK(count(i) == 4 && !released(i));

    /* The game's pass reads voices 0 and 1; the mixer finishes a buffer of voice 2; the pass goes on. */
    CHECK(count(0) == 4 && count(1) == 4);
    finish_one(2);
    CHECK(count(2) == 4);    /* the old way: 3, the game appended voice 2 an extra buffer and drifted from there */
    CHECK(!released(2));
    for (int i = 3; i < VOICES; i++) CHECK(count(i) == 4);

    /* The update: the finished buffer is the game's now, the count one less. */
    commit();
    CHECK(count(2) == 3);
    CHECK(released(2) == BUFFER);
    CHECK(!released(2) && count(2) == 3);
    for (int i = 0; i < VOICES; i++) if (i != 2) CHECK(count(i) == 4 && !released(i));

    /* Every voice finishing one in the same frame: all reach the game together, at the next update. */
    for (int i = 0; i < VOICES; i++) if (i != 2) finish_one(i);
    for (int i = 0; i < VOICES; i++) if (i != 2) CHECK(count(i) == 4 && !released(i));
    commit();
    for (int i = 0; i < VOICES; i++) {
        if (i != 2) CHECK(released(i) == BUFFER);
        CHECK(count(i) == 3 && !released(i));
    }
}

static void stopped_releases_at_the_update(void) {
    reset();
    for (int n = 0; n < 3; n++) CHECK(call(APPEND, object(0), BUFFER) == 1);
    commit();
    call(STATE, object(0), 1);   /* Stopped: every queued buffer released, when the update applies it */
    CHECK(count(0) == 3 && !released(0));
    commit();
    CHECK(count(0) == 0);
    for (int n = 0; n < 3; n++) CHECK(released(0) == BUFFER);
    CHECK(!released(0));
}

int main(void) {
    finished_between_updates();
    stopped_releases_at_the_update();
    free(cmds);
    puts("PASS: actual wave-buffer calls: buffers the mixer finishes reach the game at the update, never mid-pass");
    return 0;
}
