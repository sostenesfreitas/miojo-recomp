// (hookshot) Whether ASTC blocks decode with every texel's alpha at 255, from their headers alone (no decode): an image of such
// a texture is kept as BC1 instead of BC3 (vk_texture.cpp), the colour half of BC3's blocks, so half the video memory for the
// same texels. tests/test_astc_opaque.py.
//
// A block is opaque when:
// - it is a void-extent block (one colour) in LDR whose alpha's high byte is 0xFF (the decoder scales the 16-bit alpha to 8);
// - or it is a block whose colour endpoint mode carries no alpha: 0-3, 6-9, 11, 14 and 15. The modes with alpha are 4, 5
//   (luminance + alpha), 10 (RGB scale + two alphas), 12 and 13 (RGBA). The mode is in bits 13-16 for one partition, and in
//   bits 25-28 for partitions that share one (bits 23-24 zero).
// Partitions of different modes would need the weights' size to read their modes whole: counted as maybe-alpha, as are an
// HDR void extent and an alpha mode (which could still decode to 255 everywhere: a decode would tell; here BC3 stays).
// A block of zeros is counted apart: the storage before the game wrote the texture, or the padding between levels, says
// nothing about its texels.
#pragma once
#include <cstdint>
#include <cstring>

namespace astc_opaque {

enum Kind { kAlpha = 0, kOpaque = 1, kEmpty = 2 };

inline bool cem_alpha(uint32_t cem) { return cem == 4 || cem == 5 || cem == 10 || cem == 12 || cem == 13; }

inline Kind block(const uint8_t* b) {
    uint64_t lo, hi;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    if (!lo && !hi) return kEmpty;
    const uint32_t mode = (uint32_t)(lo & 0x7FF);
    if ((mode & 0x1FF) == 0x1FC) {   // void extent: bit 9 HDR, its RGBA as 16-bit values in bits 64-127 (alpha in 112-127)
        if (mode & 0x200) return kAlpha;
        return b[15] == 0xFF ? kOpaque : kAlpha;
    }
    const uint32_t parts = (uint32_t)(lo >> 11 & 3) + 1;
    uint32_t cem;
    if (parts == 1) {
        cem = (uint32_t)(lo >> 13 & 0xF);
    } else {
        const uint32_t enc = (uint32_t)(lo >> 23 & 0x3F);
        if (enc & 3) return kAlpha;   // (partitions of different modes: not read whole here)
        cem = enc >> 2 & 0xF;
    }
    return cem_alpha(cem) ? kAlpha : kOpaque;
}

struct Scan {
    uint64_t opaque = 0, alpha = 0, empty = 0;
};

/// The 16-byte blocks of [p, p + n) counted by kind (n's last partial block, if any, left out); stops at the first block that
/// may carry alpha when stop_at_alpha
inline Scan scan(const uint8_t* p, uint64_t n, bool stop_at_alpha = true) {
    Scan s;
    for (uint64_t i = 0; i + 16 <= n; i += 16) {
        const Kind k = block(p + i);
        if (k == kOpaque) s.opaque++;
        else if (k == kEmpty) s.empty++;
        else if (s.alpha++, stop_at_alpha) break;
    }
    return s;
}

/// The choice for a whole upload of a texture not yet decided: BC1 when every block with texels is opaque and they are most of
/// the storage (a texture the game has written, not storage still being filled)
inline bool whole_opaque(const Scan& s) { return !s.alpha && s.opaque && s.opaque >= s.empty; }

}  // namespace astc_opaque
