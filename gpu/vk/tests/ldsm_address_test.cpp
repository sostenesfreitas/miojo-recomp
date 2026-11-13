// Game-free regression for the Ampere -> Maxwell lowering of LDSM (lower_to_maxwell.h, case 0x83b): when the row address
// register is one of the destination registers it is copied to a scratch register first, and every matrix's SHFL.IDX must
// read the address from that copy. The copy went to R253, which the non-transposed path also uses for each matrix's
// shuffled address: matrices 1-3 of an LDSM.x2/.x4 then shuffled matrix 0's row address and read the wrong rows. Found
// for Lichii (Fritchii f130f0c50, Duck912): the copy now goes to R254. OoT has one such LDSM.x4 (an HMMA compute program).
//   clang++ -std=c++20 -I gpu/vk/eden -I gpu/vk/compat gpu/vk/tests/ldsm_address_test.cpp -o ldsm_address_test
#include <cstdio>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;

// LDSM.16.M88.{x1,x2,x4} Rd, [Ra + offset]: count 1 << bits 72-73, transpose bit 78
Instruction ldsm(u32 dest, u32 address, u32 offset, u32 count_log2, bool transpose) {
    return {{{0x83bU | (7U << 12) | (dest << 16) | (address << 24), offset << 8,
              (count_log2 << 8) | (transpose ? 0x4000U : 0U), 0}}};
}

int check(u32 dest, u32 address, u32 count_log2, bool transpose) {
    const Instruction code[2]{ldsm(dest, address, 0x60, count_log2, transpose), {{{0x794d | (7U << 12), 0, 0x03800000, 0}}}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    if (!lowered) {
        std::printf("FAIL: LDSM R%u, [R%u] x%u%s did not lower: %.*s\n", dest, address, 1U << count_log2, transpose ? " .T" : "",
                    int(lowered.error.size()), lowered.error.data());
        return 1;
    }
    // The register the shuffles read the address from, and whether anything writes it after it is set and before the last
    // shuffle (the copy itself is the first write)
    u32 from = ~0U, shuffles = 0;
    int bad = 0;
    bool copied = false;
    for (const u64 w : lowered.instructions) {
        const u32 written = static_cast<u32>(w & 0xff);
        if ((w >> 52) == 0xef1) {   // SHFL.IDX Rd, Ra(bits 8-15), Rb (bits 48-51: the predicate)
            const u32 a = static_cast<u32>((w >> 8) & 0xff);
            if (from == ~0U) from = a;
            if (a != from) bad++;
            shuffles++;
        }
        if ((w >> 52) == 0x5c1 && ((w >> 8) & 0xff) == address && ((w >> 20) & 0xff) == 255 && !copied) {   // the copy (IADD Rt, Ra, RZ)
            copied = true;
            continue;
        }
        if (copied && from != ~0U && written == from && shuffles < (1U << count_log2) * (transpose ? 2U : 1U)) bad++;
    }
    const bool overlap = address >= dest && address < dest + (1U << count_log2);
    if (overlap && !copied) bad++;
    if (!overlap && from != address) bad++;
    if (bad) std::printf("FAIL: LDSM R%u, [R%u] x%u%s: the shuffles read R%u, %d bad\n", dest, address, 1U << count_log2,
                         transpose ? " .T" : "", from, bad);
    return bad ? 1 : 0;
}
}  // namespace

int main() {
    int fails = 0;
    for (const bool transpose : {false, true})
        for (u32 count_log2 = 0; count_log2 < 3; ++count_log2) {
            fails += check(68, 68, count_log2, transpose);       // the address is Rd itself
            fails += check(68, 68 + (1U << count_log2) - 1, count_log2, transpose);   // the last destination
            fails += check(68, 71, count_log2, transpose);       // past the destinations (x4: inside)
            fails += check(68, 20, count_log2, transpose);       // apart: read in place
        }
    if (!fails) std::printf("PASS\n");
    return fails ? 1 : 0;
}
