/* Synthetic PCM through the actual resampler, filter setters, update queue and splitter routes. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t x[32], vreg[32][2]; } Ctx;
typedef int CRITICAL_SECTION;
static void InitializeCriticalSection(int *lock) { *lock = 0; }
static void EnterCriticalSection(int *lock) { CHECK(!*lock); *lock = 1; }
static void LeaveCriticalSection(int *lock) { CHECK(*lock); *lock = 0; }
static void hos_log(const char *format, ...) { (void)format; }
#define HLE(name) void imp_##name(Ctx *c)
#define ARG(n) (c->x[n])
#define RET(value) (c->x[0] = (uint64_t)(value))
#define GP(value) ((void *)(uintptr_t)(value))
#define G64(value) (*(uint64_t *)GP(value))
#define G32(value) (*(uint32_t *)GP(value))
#include "audio_biquad.inc"

#define VOICE_FLOAT imp__ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_22TBiquadFilterParameterIfEE
#define VOICE_Q14 imp__ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_21BiquadFilterParameterE
#define SPLIT_SUB imp__ZN2nn5audio32SetSplitterBiquadFilterParameterEPNS0_12SplitterTypeEiPNS0_10SubMixTypeEiRKNS0_22TBiquadFilterParameterIfEE
#define SPLIT_FINAL imp__ZN2nn5audio32SetSplitterBiquadFilterParameterEPNS0_12SplitterTypeEiPNS0_12FinalMixTypeEiRKNS0_22TBiquadFilterParameterIfEE
#define SPLIT_ADD imp__ZN2nn5audio15AcquireSplitterEPNS0_19AudioRendererConfigEPNS0_12SplitterTypeEiii
#define SPLIT_RELEASE imp__ZN2nn5audio15ReleaseSplitterEPNS0_19AudioRendererConfigEPNS0_12SplitterTypeE
#define SPLIT_DEST imp__ZN2nn5audio22SetSplitterDestinationEPNS0_19AudioRendererConfigEPNS0_12SplitterTypeEiPNS0_10SubMixTypeE
typedef struct { uint8_t enabled, padding[3]; float b[3], a[2]; } Parameter;
static uint64_t vo, sp, mo[2];
static float pcm[FRAME * 4][2];
static void near(float actual, double expected) { CHECK(fabs(actual - expected) < 0.00001); }

static void voice_filter(int f, Parameter *p) {
    Ctx c = {0}; c.x[0] = (uintptr_t)&vo; c.x[1] = f; c.x[2] = (uintptr_t)p; VOICE_FLOAT(&c);
}
static void split_filter(int d, int f, Parameter *p) {
    Ctx c = {0}; c.x[0] = (uintptr_t)&sp; c.x[1] = d; c.x[2] = (uintptr_t)&mo[d];
    c.x[3] = f; c.x[4] = (uintptr_t)p;
    if (d == 0) SPLIT_SUB(&c); else SPLIT_FINAL(&c);
}
static void source(int alternating) {
    for (int i = 0; i < FRAME * 4; i++) {
        pcm[i][0] = alternating && i % 2 ? -1.0f : 1.0f;
        pcm[i][1] = 0.5f;
    }
    Voice *v = &voices[0];
    v->nwb = 1; v->pos = 1; v->started_wb = 1; v->frac = 0;
    v->wbc[0] = (WB){.buffer = (uintptr_t)pcm, .size = sizeof pcm, .end = FRAME * 4};
    memcpy(v->cur, pcm[0], sizeof pcm[0]);
}
static void setup(void) {
    memset(voices, 0, sizeof voices); memset(splitters, 0, sizeof splitters); memset(mixes, 0, sizeof mixes);
    ncmds = 0;
    voices[0].used = 1; voices[0].rate = RATE; voices[0].channels = 2; voices[0].format = FMT_FLOAT;
    voices[0].volume = voices[0].ramp_volume = voices[0].pitch = 1;
    voices[0].dest_kind = DEST_SPLITTER;
    put_handle((uintptr_t)&vo, K_VOICE, 0);
    Ctx c = {0}; c.x[1] = (uintptr_t)&sp; c.x[2] = RATE; c.x[3] = 2; c.x[4] = 2; SPLIT_ADD(&c);
    CHECK(c.x[0] == 1);
    for (int d = 0; d < 2; d++) {
        mixes[d].used = 1; mixes[d].buffers = 3;
        put_handle((uintptr_t)&mo[d], K_MIX, d);
        c.x[1] = (uintptr_t)&sp; c.x[2] = d; c.x[3] = (uintptr_t)&mo[d]; SPLIT_DEST(&c);
        splitters[0].vol[d][0][0] = splitters[0].ramp[d][0][0] = 1;
        splitters[0].vol[d][1][1] = splitters[0].ramp[d][1][1] = 1;
        splitters[0].vol[d][0][2] = splitters[0].ramp[d][0][2] = 0.5f;
    }
    commit(); source(0);
}
static void frame(void) { for (int i = 0; i < 2; i++) memset(mixes[i].buf, 0, sizeof mixes[i].buf); render_voice(&voices[0]); }

int main(void) {
    CHECK(sizeof(Parameter) == 24);
    setup();
    Parameter low = {1, {0xFF, 0xFF, 0xFF}, {0.25f, 0, 0}, {0.75f, 0}};
    voice_filter(0, &low);
    frame(); near(mixes[0].buf[0][0], 1); /* queued, not yet applied */
    low.b[0] = 9; /* parameter storage reused before update */
    commit(); frame();
    near(mixes[0].buf[0][0], 0.25); /* old no-op failed here */
    double y = 0;
    for (int i = 0; i < FRAME; i++) { y = 0.25 + 0.75 * y; near(mixes[0].buf[0][i], y); near(mixes[0].buf[1][i], y * 0.5); }
    frame(); near(mixes[0].buf[0][0], 1); /* history spans frames */

    /* Keep history when updating enabled coefficients; reset on disable/re-enable. */
    Parameter change = {1, {0}, {0.125f, 0, 0}, {0.5f, 0}};
    voice_filter(0, &change); commit(); source(0); frame(); near(mixes[0].buf[0][0], 0.875);
    change.enabled = 0; voice_filter(0, &change); commit(); frame(); near(mixes[0].buf[0][0], 1);
    change.enabled = 1; voice_filter(0, &change); commit(); frame(); near(mixes[0].buf[0][0], 0.125);

    /* An alternating input is attenuated; the DC channel keeps its amplitude. */
    setup(); source(1); low.b[0] = 0.25f; voice_filter(0, &low); commit(); frame();
    near(fabsf(mixes[0].buf[0][FRAME - 1]), 1.0 / 7); near(mixes[0].buf[1][FRAME - 1], 0.5);

    /* Exercise all five coefficients against a separate direct-form-I recurrence. */
    setup();
    Parameter general = {1, {0}, {0.5f, -0.125f, 0.0625f}, {0.4f, -0.1f}};
    for (int i = 0; i < FRAME * 4; i++) pcm[i][0] = (float)sin(i * 0.13);
    voices[0].cur[0] = pcm[0][0];
    voice_filter(0, &general); commit();
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int f = 0; f < 2; f++) {
        frame();
        for (int i = 0; i < FRAME; i++) {
            const double x = pcm[f * FRAME + i][0];
            const double expected = general.b[0] * x + general.b[1] * x1 + general.b[2] * x2 + general.a[0] * y1 + general.a[1] * y2;
            near(mixes[0].buf[0][i], expected);
            x2 = x1; x1 = x; y2 = y1; y1 = expected;
        }
    }

    /* Two serial voice filters, with the original Q14 overload still supported. */
    setup(); voice_filter(0, &low);
    int16_t q14[6] = {1, 8192, 0, 0, 0, 0}; Ctx c = {0};
    c.x[0] = (uintptr_t)&vo; c.x[1] = 1; c.x[2] = (uintptr_t)q14; VOICE_Q14(&c);
    commit(); frame(); near(mixes[0].buf[0][0], 0.125); near(mixes[0].buf[0][FRAME - 1], 0.5);

    /* Route 0 filters once per source channel, before panning; route 1 stays dry. */
    setup(); split_filter(0, 0, &low); frame(); near(mixes[0].buf[0][0], 1);
    commit(); frame(); y = 0;
    for (int i = 0; i < FRAME; i++) {
        y = 0.25 + 0.75 * y;
        near(mixes[0].buf[0][i], y); near(mixes[0].buf[1][i], y * 0.5); near(mixes[0].buf[2][i], y * 0.5);
        near(mixes[1].buf[0][i], 1); near(mixes[1].buf[1][i], 0.5);
    }
    Parameter half = {1, {0}, {0.5f, 0, 0}, {0, 0}};
    split_filter(0, 1, &half); split_filter(1, 0, &half); commit(); frame();
    near(mixes[0].buf[0][0], 0.5); near(mixes[1].buf[0][0], 0.5);
    split_filter(0, 0, &change); commit(); source(0); frame(); near(mixes[0].buf[0][0], 0.4375);
    change.enabled = 0; split_filter(0, 0, &change); commit(); frame(); near(mixes[0].buf[0][0], 0.5);
    change.enabled = 1; split_filter(0, 0, &change); commit(); frame(); near(mixes[0].buf[0][0], 0.0625);

    /* A second voice using the splitter must not inherit another source's history. */
    setup(); split_filter(0, 0, &low); commit(); frame();
    voices[1] = voices[0]; /* fixture only: keep the first voice's decoded PCM state, with fresh filter histories */
    memset(&voices[0], 0, sizeof voices[0]);
    voices[0].used = 1; voices[0].rate = RATE; voices[0].channels = 2; voices[0].format = FMT_FLOAT;
    voices[0].volume = voices[0].ramp_volume = voices[0].pitch = 1; voices[0].dest_kind = DEST_SPLITTER;
    source(0); frame(); near(mixes[0].buf[0][0], 0.25);
    memset(mixes[0].buf, 0, sizeof mixes[0].buf);
    render_voice(&voices[1]); near(mixes[0].buf[0][0], 1); /* first source's history remains intact */

    /* Release and reacquire the same guest object before commit must use a fresh slot. */
    setup(); split_filter(0, 0, &low); commit(); frame();
    c = (Ctx){0}; c.x[1] = (uintptr_t)&sp; SPLIT_RELEASE(&c);
    c.x[1] = (uintptr_t)&sp; c.x[2] = RATE; c.x[3] = 2; c.x[4] = 2; SPLIT_ADD(&c); CHECK(c.x[0] == 1);
    CHECK(get_handle((uintptr_t)&sp, K_SPLITTER) == 1); commit(); CHECK(!splitters[0].used && splitters[1].used);
    c.x[1] = (uintptr_t)&sp; SPLIT_RELEASE(&c); commit();
    c.x[1] = (uintptr_t)&sp; c.x[2] = RATE; c.x[3] = 2; c.x[4] = 2; SPLIT_ADD(&c);
    CHECK(get_handle((uintptr_t)&sp, K_SPLITTER) == 0);
    c.x[1] = (uintptr_t)&sp; c.x[2] = 0; c.x[3] = (uintptr_t)&mo[0]; SPLIT_DEST(&c);
    splitters[0].vol[0][0][0] = splitters[0].ramp[0][0][0] = 1;
    split_filter(0, 0, &low); commit(); source(0); frame(); near(mixes[0].buf[0][0], 0.25);

    /* Invalid filter indices must not enqueue writes to another voice or destination. */
    setup(); voice_filter(-1, &low); voice_filter(2, &low);
    c = (Ctx){0}; c.x[0] = (uintptr_t)&sp; c.x[1] = 32; c.x[2] = (uintptr_t)&mo[0]; c.x[4] = (uintptr_t)&low; SPLIT_SUB(&c);
    CHECK(ncmds == 0); frame(); near(mixes[0].buf[0][0], 1);
    puts("audio biquad: queued float/Q14 parameters, response/history, stereo/routes and reuse passed");
    free(cmds);
    return 0;
}
