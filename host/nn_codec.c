/* nn::codec: the hardware Opus decoder, on libopus (third_party/opus).

   Input packets come as on the hardware decoder: an 8-byte header {u32 big-endian payload size, u32 big-endian
   final range} then the Opus packet. DecodeOpusInterleavedWithHardware decodes one packet into interleaved
   16-bit PCM and says how many bytes it consumed (header + payload) and how many samples (per channel) it made.
   Each HardwareOpusDecoderType gets our handle in its first 8 bytes. */
#include "hos.h"
#include <stdio.h>
#include <string.h>
#include <opus.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define CODEC_MAGIC 0x4F505553u   /* 'OPUS' */
#define MAX_DECODERS 64

typedef struct {
    OpusDecoder *dec;
    int rate, channels;
} Dec;
static Dec decs[MAX_DECODERS];
static volatile LONG decs_lock;

static void dlock(void) { while (InterlockedExchange(&decs_lock, 1)) hos_sleep_ns(1000); }
static void dunlock(void) { InterlockedExchange(&decs_lock, 0); }

static Dec *dec_of(uint64_t obj) {
    if (!obj) return NULL;
    const uint64_t h = G64(obj);
    if ((uint32_t)(h >> 32) != CODEC_MAGIC) return NULL;
    const uint32_t i = (uint32_t)h;
    return i < MAX_DECODERS && decs[i].dec ? &decs[i] : NULL;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* GetHardwareOpusDecoderWorkBufferSize(int sampleRate, int channelCount, ...): the game allocates it; we keep our
   state on the host */
HLE(_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii) { RET(0x1000); }

/* InitializeHardwareOpusDecoder(HardwareOpusDecoderType*, int sampleRate, int channelCount, void* workBuffer,
                                 size_t workBufferSize, int option) -> Result */
HLE(_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi) {
    const int rate = (int)ARG(1), channels = (int)ARG(2);
    int err = 0;
    OpusDecoder *d = opus_decoder_create(rate, channels, &err);
    if (!d || err != OPUS_OK) {
        hos_log("codec: Opus decoder %d Hz %d channels not created (%d)", rate, channels, err);
        RET(0x1F0);   /* (any failure result) */
        return;
    }
    dlock();
    int i = 0;
    while (i < MAX_DECODERS && decs[i].dec) i++;
    if (i == MAX_DECODERS) {
        dunlock();
        opus_decoder_destroy(d);
        RET(0x1F0);
        return;
    }
    decs[i].dec = d;
    decs[i].rate = rate;
    decs[i].channels = channels;
    G64(ARG(0)) = (uint64_t)CODEC_MAGIC << 32 | (uint32_t)i;
    dunlock();
    static int logged;
    if (logged++ < 8) hos_log_at(HOS_LOG_DEBUG, "codec: Opus decoder %d: %d Hz, %d channels", i, rate, channels);
    RET(0);
}
HLE(_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE) {
    dlock();
    Dec *d = dec_of(ARG(0));
    if (d) {
        opus_decoder_destroy(d->dec);
        d->dec = NULL;
        G64(ARG(0)) = 0;
    }
    dunlock();
}
HLE(_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE) {
    Dec *d = dec_of(ARG(0));
    if (d) opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
    RET(0);
}
HLE(_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE) {
    Dec *d = dec_of(ARG(0));
    RET(d ? d->rate : 48000);
}
HLE(_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE) {
    Dec *d = dec_of(ARG(0));
    RET(d ? d->channels : 2);
}

/* GetOpusPacketSize(const uint8* data, size_t size): the size of the packet the header describes, header included */
HLE(_ZN2nn5codec17GetOpusPacketSizeEPKhm) {
    const uint8_t *p = (const uint8_t *)GP(ARG(0));
    const uint64_t size = ARG(1);
    const uint64_t r = size >= 8 ? 8 + (uint64_t)be32(p) : 0;
    static int logged;
    if (logged++ < 6) hos_log_at(HOS_LOG_TRACE, "codec: GetOpusPacketSize(%#llx, %llu): header %08x %08x -> %llu", (unsigned long long)ARG(0),
                              (unsigned long long)size, be32(p), be32(p + 4), (unsigned long long)r);
    RET(r);
}

/* DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType*, size_t* consumed, int* sampleCount, int16* out,
                                     size_t outSize, const void* in, size_t inSize) -> Result */
HLE(_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm) {
    Dec *d = dec_of(ARG(0));
    const uint64_t out_size = ARG(4), in_size = ARG(6);
    /* (hookshot) both buffers in guest memory, and the frame count an int (a 64-bit size was cut to any int) */
    const uint8_t *in = (const uint8_t *)hos_guest_range(ARG(5), in_size);
    if (!d || in_size < 8 || !in) { RET(0x1F0); return; }
    const uint32_t len = be32(in);
    if (len > in_size - 8) { RET(0x1F0); return; }
    const uint64_t room = out_size / (2 * (uint64_t)d->channels);
    const int frames = room < 5760 ? (int)room : 5760;   /* (Opus' longest packet: 120 ms at 48 kHz) */
    opus_int16 *out = (opus_int16 *)hos_guest_range(ARG(3), (uint64_t)frames * 2 * (uint64_t)d->channels);
    if (!out) { RET(0x1F0); return; }
    const int n = opus_decode(d->dec, in + 8, (opus_int32)len, out, frames, 0);
    static int logged;
    if (logged++ < 6)
        hos_log("codec: decode %u bytes (of %llu) -> %d samples (room for %d)", len, (unsigned long long)in_size, n, frames);
    if (n < 0) { RET(0x1F0); return; }
    if (ARG(1)) G64(ARG(1)) = 8 + (uint64_t)len;
    if (ARG(2)) G32(ARG(2)) = (uint32_t)n;
    RET(0);
}
