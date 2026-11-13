/* Authored packets and samples around actual source bodies. No audio device or game is opened. */
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
#define ARG(n) (c->x[n])
#define HLE(sym) void imp_##sym(Ctx *c)
#define A64_WEAK
#define AUT(...) ((void)0)

/* Single-threaded test boundaries. Scheduling, allocation failure and output devices are outside this fixture. */
static int lock_depth;
static void au_lock(void) { CHECK(lock_depth++ == 0); }
static void au_unlock(void) { CHECK(--lock_depth == 0); }
static const char *fixture_getenv(const char *name) { (void)name; return NULL; }
#define getenv fixture_getenv
#define env_on(name) (getenv(name) && !env_value_off(getenv(name)))   /* (the switches read through the stand-in above) */

/* The extracted production types precede these globals and the actual queue functions. */
#define FLOAT_SETTER imp__ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_22TBiquadFilterParameterIfEE
#define LEGACY_SETTER imp__ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_21BiquadFilterParameterE

/* The generated section keeps real Voice/Mix/Splitter types ahead of its fixture globals and production functions. */
#include "actual_audio_biquad.inc"

enum { OBJECT0 = 0x40, OBJECT1 = 0x50, PACKET = 0x200 };
static void close_to(float actual, float expected) { CHECK(fabsf(actual - expected) < 0.000001f); }
static void clear_samples(float out[MAX_CH][FRAME]) { memset(out, 0, sizeof(float) * MAX_CH * FRAME); }
static void reset(void) {
    memset(guest, 0, sizeof guest);
    memset(voices, 0, sizeof voices);
    ncmds = 0;
    for (int i = 0; i < 2; i++) voices[i].used = 1, voices[i].channels = 2;
    put_handle(OBJECT0, K_VOICE, 0);
    put_handle(OBJECT1, K_VOICE, 1);
}
static void float_packet(int enabled, float b0, float b1, float b2, float a1, float a2) {
    /* Authored 24-byte packet: bool, three padding bytes, three f32 numerators, two additive f32 feedback values. */
    uint8_t packet[24];
    memset(packet, 0xa5, sizeof packet);
    packet[0] = (uint8_t)enabled;
    const float values[] = {b0, b1, b2, a1, a2};
    memcpy(packet + 4, values, sizeof values);
    memcpy(GP(PACKET), packet, sizeof packet);
}
static void set_float(uint64_t object, int filter, uint64_t packet) {
    Ctx c = {0};
    c.x[0] = object; c.x[1] = (uint64_t)(int64_t)filter; c.x[2] = packet;
    FLOAT_SETTER(&c);
    CHECK(lock_depth == 0);
}
static void configure(float b0, float b1, float b2, float a1, float a2) {
    float_packet(1, b0, b1, b2, a1, a2);
    set_float(OBJECT0, 0, PACKET);
    CHECK(ncmds == 1);  /* The old import fallback must fail here, after compiling and returning normally. */
    commit();
}
static void identity_and_packet_lifetime(void) {
    reset();
    float_packet(1, 1, 0, 0, 0, 0);
    set_float(OBJECT0, 0, PACKET);
    CHECK(ncmds == 1);
    CHECK(!voices[0].bq_on[0]);
    memset(GP(PACKET), 0xa5, 24);  /* The caller may immediately reuse its packet before RequestUpdate commits. */
    commit();
    CHECK(ncmds == 0 && voices[0].bq_on[0]);
    close_to(voices[0].bq_b[0][0], 1);
    float out[MAX_CH][FRAME] = {{0}};
    for (int i = 0; i < FRAME; i++) out[0][i] = (float)(i % 5) * 0.125f, out[1][i] = -out[0][i];
    filter_samples(&voices[0], out);
    for (int i = 0; i < FRAME; i++) {
        close_to(out[0][i], (float)(i % 5) * 0.125f);
        close_to(out[1][i], -(float)(i % 5) * 0.125f);
    }
}
static void attenuation_and_additive_feedback(void) {
    reset();
    configure(0.25f, 0, 0, 0, 0);
    float out[MAX_CH][FRAME] = {{0}};
    out[0][0] = 1;
    filter_samples(&voices[0], out);
    close_to(out[0][0], 0.25f);
    close_to(out[0][1], 0);
    reset();
    configure(0.75f, 0.25f, 0.125f, 0.5f, -0.25f);
    clear_samples(out);
    out[0][0] = 1;
    filter_samples(&voices[0], out);
    /* Fixed analytic impulse values, not another implementation of the production DSP loop. */
    const float expected[] = {0.75f, 0.625f, 0.25f, -0.03125f, -0.078125f};
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; i++) close_to(out[0][i], expected[i]);
}
static void state_and_reenable(void) {
    reset();
    configure(1, 0, 0, 0.5f, 0);
    float out[MAX_CH][FRAME] = {{0}};
    out[0][FRAME - 1] = 1;
    filter_samples(&voices[0], out);
    close_to(voices[0].bq_z[0][0][0], 0.5f);
    close_to(voices[0].bq_z[0][1][0], 0);
    clear_samples(out);
    filter_samples(&voices[0], out);
    close_to(out[0][0], 0.5f);
    close_to(out[0][1], 0.25f);
    close_to(out[1][0], 0);
    clear_samples(out);
    out[0][FRAME - 1] = 1; out[1][FRAME - 1] = 2;
    filter_samples(&voices[0], out);
    close_to(voices[0].bq_z[0][0][0], 0.5f);
    close_to(voices[0].bq_z[0][1][0], 1);
    float_packet(0, 1, 0, 0, 0.5f, 0);
    set_float(OBJECT0, 0, PACKET); commit();
    out[0][0] = 0.75f;
    filter_samples(&voices[0], out);
    close_to(out[0][0], 0.75f);
    float_packet(1, 1, 0, 0, 0.5f, 0);
    set_float(OBJECT0, 0, PACKET); commit();
    CHECK(voices[0].bq_on[0]);
    for (int ch = 0; ch < MAX_CH; ch++)
        CHECK(voices[0].bq_z[0][ch][0] == 0 && voices[0].bq_z[0][ch][1] == 0);
}
static void invalid_inputs(void) {
    reset();
    float_packet(1, 1, 0, 0, 0, 0);
    set_float(OBJECT0, -1, PACKET); set_float(OBJECT0, 2, PACKET);
    set_float(0, 0, PACKET); set_float(OBJECT0, 0, 0);
    voices[0].released = 1; set_float(OBJECT0, 0, PACKET); voices[0].released = 0;
    voices[0].used = 0; set_float(OBJECT0, 0, PACKET); voices[0].used = 1;
    put_handle(OBJECT0, K_MIX, 0); set_float(OBJECT0, 0, PACKET);
    CHECK(ncmds == 0 && !voices[0].bq_on[0] && !voices[0].bq_on[1]);
}
static void voice_filter_channel_isolation(void) {
    reset();
    float_packet(1, 0.5f, 0, 0, 0, 0); set_float(OBJECT0, 0, PACKET);
    float_packet(1, 0.25f, 0, 0, 0, 0); set_float(OBJECT1, 1, PACKET);
    CHECK(ncmds == 2); commit();
    CHECK(voices[0].bq_on[0] && !voices[0].bq_on[1]);
    CHECK(!voices[1].bq_on[0] && voices[1].bq_on[1]);
    float out[MAX_CH][FRAME] = {{0}};
    out[0][0] = 1; out[1][0] = 2;
    filter_samples(&voices[0], out);
    close_to(out[0][0], 0.5f); close_to(out[1][0], 1);
    clear_samples(out); out[0][0] = 1; out[1][0] = 3;
    filter_samples(&voices[1], out);
    close_to(out[0][0], 0.25f); close_to(out[1][0], 0.75f);
    float_packet(1, 0.25f, 0, 0, 0, 0); set_float(OBJECT0, 1, PACKET); commit();
    clear_samples(out); out[0][0] = 1;
    filter_samples(&voices[0], out); close_to(out[0][0], 0.125f);
}
static void legacy_q14(void) {
    reset();
    uint8_t packet[12];
    memset(packet, 0xa5, sizeof packet); packet[0] = 1;
    const int16_t coefs[] = {8192, 0, 0, 8192, 0};
    memcpy(packet + 2, coefs, sizeof coefs); memcpy(GP(PACKET), packet, sizeof packet);
    Ctx c = {0}; c.x[0] = OBJECT0; c.x[1] = 0; c.x[2] = PACKET;
    LEGACY_SETTER(&c);
    CHECK(ncmds == 1 && !voices[0].bq_on[0]);
    memset(GP(PACKET), 0xa5, sizeof packet); commit();
    close_to(voices[0].bq_b[0][0], 0.5f); close_to(voices[0].bq_a[0][0], 0.5f);
    float out[MAX_CH][FRAME] = {{0}}; out[0][0] = 1;
    filter_samples(&voices[0], out);
    close_to(out[0][0], 0.5f); close_to(out[0][1], 0.25f); close_to(out[0][2], 0.125f);
}
int main(void) {
    identity_and_packet_lifetime();
    attenuation_and_additive_feedback();
    state_and_reenable();
    invalid_inputs();
    voice_filter_channel_isolation();
    legacy_q14();
    free(cmds);
    puts("PASS: actual float/Q14 voice setters, queued commit, biquad response/history/reset and isolation");
    return 0;
}
