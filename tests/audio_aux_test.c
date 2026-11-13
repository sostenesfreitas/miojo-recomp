/* Real HLE entry points and DSP code, synthetic memory and a worker that applies a known gain. */
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
#include "audio_aux.inc"

#define AUX_ADD imp__ZN2nn5audio6AddAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_10SubMixTypeEPvS7_m
#define AUX_COUNT imp__ZN2nn5audio17GetAuxSampleCountEPKNS0_7AuxTypeE
#define AUX_READ imp__ZN2nn5audio17ReadAuxSendBufferEPNS0_7AuxTypeEPii
#define AUX_WRITE imp__ZN2nn5audio20WriteAuxReturnBufferEPNS0_7AuxTypeEPKii
#define AUX_MAP imp__ZN2nn5audio17SetAuxInputOutputEPNS0_7AuxTypeEPKaS4_i
#define AUX_ENABLE imp__ZN2nn5audio13SetAuxEnabledEPNS0_7AuxTypeEb
#define AUX_ENABLED imp__ZN2nn5audio12IsAuxEnabledEPKNS0_7AuxTypeE
#define AUX_REMOVE imp__ZN2nn5audio9RemoveAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_10SubMixTypeE
#define AUX_REMOVABLE imp__ZN2nn5audio14IsAuxRemovableEPKNS0_7AuxTypeE

static uint64_t mix_object, aux_object;
static int32_t send_storage[32 + FRAME * 4 + 4], return_storage[32 + FRAME * 4 + 4];
static const size_t buffer_bytes = 128 + FRAME * 4 * sizeof(int32_t);
static int32_t data[FRAME * MAX_BUF];

static uint64_t call(void (*fn)(Ctx *), uint64_t object, uint64_t arg1, uint64_t arg2) {
    Ctx c = {0};
    c.x[0] = object; c.x[1] = arg1; c.x[2] = arg2;
    fn(&c);
    return c.x[0];
}
static void fill(float left, float right) {
    for (int i = 0; i < FRAME; i++) mixes[0].buf[0][i] = left, mixes[0].buf[1][i] = right;
}
static void mapping(int cross) {
    int8_t in[2] = {0, 1}, out[2] = {(int8_t)cross, (int8_t)!cross};
    Ctx c = {0};
    c.x[0] = (uintptr_t)&aux_object; c.x[1] = (uintptr_t)in; c.x[2] = (uintptr_t)out; c.x[3] = 2;
    AUX_MAP(&c);
    /* The caller may reuse these arrays before RequestUpdateAudioRenderer. */
    in[0] = out[0] = -1;
}
static void add(void) {
    Ctx c = {0};
    c.x[1] = (uintptr_t)&aux_object; c.x[2] = (uintptr_t)&mix_object;
    c.x[3] = (uintptr_t)send_storage; c.x[4] = (uintptr_t)return_storage; c.x[5] = buffer_bytes;
    AUX_ADD(&c);
    CHECK(c.x[0] == 0 && aux_object);
}
static void setup(void) {
    memset(mixes, 0, sizeof mixes);
    memset(reverbs, 0, sizeof reverbs);
    memset(auxes, 0, sizeof auxes);
    memset(send_storage, 0x5a, sizeof send_storage);
    memset(return_storage, 0x5a, sizeof return_storage);
    ncmds = 0;
    mixes[0].used = 1; mixes[0].buffers = 2;
    put_handle((uintptr_t)&mix_object, K_MIX, 0);
    aux_object = 0;
    add();
    mapping(1);
}
static int read_samples(int n) { return (int)call(AUX_READ, (uintptr_t)&aux_object, (uintptr_t)data, (uint64_t)n); }
static int write_samples(int n) { return (int)call(AUX_WRITE, (uintptr_t)&aux_object, (uintptr_t)data, (uint64_t)n); }
static void enable(int enabled) { call(AUX_ENABLE, (uintptr_t)&aux_object, enabled, 0); }

int main(void) {
    uint32_t parameter[2] = {48000, 240};
    Ctx c = {0};
    c.x[0] = (uintptr_t)parameter; c.x[1] = 6; c.x[2] = 5;
    imp__ZN2nn5audio43GetRequiredBufferSizeForAuxSendReturnBufferEPKNS0_22AudioRendererParameterEii(&c);
    CHECK(c.x[0] == 28928); /* 30 complete channel frames, plus metadata; old stub returned 376. */
    parameter[1] = 241;
    c.x[0] = (uintptr_t)parameter; c.x[1] = 1; c.x[2] = 1;
    imp__ZN2nn5audio43GetRequiredBufferSizeForAuxSendReturnBufferEPKNS0_22AudioRendererParameterEii(&c);
    CHECK(c.x[0] == 1152); /* round 964 bytes up to a cache line */
    c.x[0] = (uintptr_t)parameter; c.x[1] = UINT32_MAX; c.x[2] = 1;
    imp__ZN2nn5audio43GetRequiredBufferSizeForAuxSendReturnBufferEPKNS0_22AudioRendererParameterEii(&c);
    CHECK(c.x[0] == 0);

    setup();
    CHECK(call(AUX_COUNT, (uintptr_t)&aux_object, 0, 0) == FRAME * 4);
    CHECK(call(AUX_ENABLED, (uintptr_t)&aux_object, 0, 0) == 1);
    fill(0.25f, -0.5f);
    effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 0.25f); /* graph change waits for update */
    commit();
    effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 0 && mixes[0].buf[1][0] == 0); /* wet output, never dry leakage */
    CHECK(read_samples(FRAME * 2) == FRAME * 2);
    for (int i = 0; i < FRAME; i++) CHECK(data[i] == 8192 && data[FRAME + i] == -16384);
    for (int i = 0; i < FRAME * 2; i++) data[i] /= 4;
    CHECK(write_samples(FRAME * 2) == FRAME * 2);
    fill(1, 1);
    effects_frame(0);
    for (int i = 0; i < FRAME; i++) CHECK(mixes[0].buf[0][i] == -0.125f && mixes[0].buf[1][i] == 0.0625f);
    CHECK(call(AUX_COUNT, (uintptr_t)&aux_object, 0, 0) == FRAME * 4); /* capacity, not readable count */
    effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 0 && mixes[0].buf[1][0] == 0); /* return consumed once, no loop on a late worker */

    enable(0);
    CHECK(!call(AUX_ENABLED, (uintptr_t)&aux_object, 0, 0));
    CHECK(!call(AUX_REMOVABLE, (uintptr_t)&aux_object, 0, 0));
    commit();
    CHECK(call(AUX_REMOVABLE, (uintptr_t)&aux_object, 0, 0));
    CHECK(read_samples(FRAME * 2) == 0);
    fill(0.25f, -0.5f);
    effects_frame(0);
    CHECK(mixes[0].buf[0][0] == -0.5f && mixes[0].buf[1][0] == 0.25f); /* disabled cross-map must not overwrite input */

    enable(1); commit();
    for (int frame = 1; frame <= 12; frame++) { fill((float)frame, -(float)frame); effects_frame(0); }
    CHECK(read_samples(FRAME * 4) == FRAME * 4);
    CHECK(data[0] == 32768 && data[FRAME] == -32768 && data[FRAME * 2] == 65536 && data[FRAME * 3] == -65536);
    fill(0.125f, -0.25f); effects_frame(0);
    CHECK(read_samples(FRAME * 2) == FRAME * 2 && data[0] == 4096 && data[FRAME] == -8192);
    CHECK(read_samples(-1) == 0 && write_samples(-1) == 0);
    for (int i = 0; i < FRAME * 4; i++) data[i] = i + 1;
    CHECK(write_samples(FRAME * 5) == FRAME * 4); /* bounded write */
    CHECK(write_samples(1) == 0);
    mapping(1); commit(); /* reapplying the same map must not discard pending results */
    fill(0, 0); effects_frame(0);
    CHECK(mixes[0].buf[1][0] == 1.0f / 32768.0f);
    mapping(0); commit(); /* changed topology discards old channel layout */
    fill(1, 2); effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 0 && mixes[0].buf[1][0] == 0);
    CHECK(read_samples(FRAME * 2) == FRAME * 2 && data[0] == 32768 && data[FRAME] == 65536);

    fill(NAN, INFINITY); effects_frame(0);
    CHECK(read_samples(FRAME * 2) == FRAME * 2 && data[0] == 0 && data[FRAME] == 0);
    fill(1e30f, -1e30f); effects_frame(0);
    CHECK(read_samples(FRAME * 2) == FRAME * 2 && data[0] == INT32_MAX && data[FRAME] == INT32_MIN);

    /* Removing and reusing the same guest object before update must not delete the new effect. */
    c = (Ctx){0}; c.x[1] = (uintptr_t)&aux_object;
    AUX_REMOVE(&c);
    CHECK(call(AUX_COUNT, (uintptr_t)&aux_object, 0, 0) == 0);
    add(); mapping(0); commit();
    fill(0.5f, -0.5f); effects_frame(0);
    CHECK(read_samples(FRAME * 2) == FRAME * 2 && data[0] == 16384);
    CHECK(send_storage[32 + FRAME * 4] == 0x5a5a5a5a && return_storage[32 + FRAME * 4] == 0x5a5a5a5a);

    /* Uneven CPU reads cross the storage boundary without losing planar channel order. */
    setup(); commit();
    fill(1, -1); effects_frame(0);
    CHECK(read_samples(100) == 100);
    fill(2, -2); effects_frame(0);
    CHECK(read_samples(600) == 600);
    CHECK(data[139] == 32768 && data[140] == -32768 && data[380] == 65536);
    fill(3, -3); effects_frame(0);
    CHECK(read_samples(740) == 740);
    CHECK(data[19] == 65536 && data[20] == -65536 && data[260] == 98304 && data[500] == -98304);
    /* A worker can return one frame in several calls; never consume an incomplete frame. */
    for (int i = 0; i < FRAME * 2; i++) data[i] = 4096;
    CHECK(write_samples(100) == 100);
    fill(1, 1); effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 0);
    CHECK(write_samples(FRAME * 2 - 100) == FRAME * 2 - 100);
    effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 0.125f && mixes[0].buf[1][FRAME - 1] == 0.125f);

    /* Insertion order across reverb and Aux. A dry-only half-gain reverb is deterministic. */
    setup(); commit();
    Reverb *rv = &reverbs[0];
    rv->used = rv->enabled = 1; rv->mix = 0; rv->n = 2; rv->in[1] = rv->out[1] = 1;
    rv->room = -10000; rv->dry = 0.5f; rv->order = ++effect_order;
    for (int i = 0; i < FRAME * 2; i++) data[i] = 8192;
    CHECK(write_samples(FRAME * 2) == FRAME * 2);
    fill(1, 1); effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 0.125f); /* Aux first, then reverb */
    CHECK(read_samples(FRAME * 2) == FRAME * 2 && data[0] == 32768);
    rv->order = 1;
    fill(1, 1); effects_frame(0);
    CHECK(read_samples(FRAME * 2) == FRAME * 2 && data[0] == 16384); /* reverb first, then Aux */

    free(cmds);
    puts("PASS: Aux sizing, PCM roundtrip, channel maps, update ordering, under/overflow, reset and slot reuse");
    return 0;
}
