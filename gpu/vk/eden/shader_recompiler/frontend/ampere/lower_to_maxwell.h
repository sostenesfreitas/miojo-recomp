#pragma GCC diagnostic ignored "-Wshadow"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.
#pragma once

#include <cstdlib>
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <optional>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif

namespace Shader::Ampere {

struct Instruction {
    std::array<u32, 4> words;

    u32 Bits(u32 offset, u32 count) const {
        u64 bits = words[offset / 32];
        if (offset / 32 + 1 < words.size()) {
            bits |= u64{words[offset / 32 + 1]} << 32;
        }
        return static_cast<u32>((bits >> (offset % 32)) & ((u64{1} << count) - 1));
    }
};
static_assert(sizeof(Instruction) == 16);

// (hookshot) set by the Vulkan back end when the GPU's cooperative matrices passed its self-test
// (vk_coopmat.cpp): HMMA.16816.F16 then lowers to the HMMA_nx2 pseudo instruction (one subgroup
// matrix multiply on the host's tensor cores) instead of the shuffle expansion.
inline thread_local bool native_matrix_multiply = false;

// (hookshot) Native programs whose GPRs reach the lowering's reserved registers (the uniform bank at R216-R245, R239's
// uniform predicates, the scratch R246-R254): OoT's big vertex and mesh programs use ~249 GPRs. Their high GPRs (from
// the reserved base up) live in IR registers 256 + n instead: each lowered word of a native instruction that names
// them is preceded by REGMAP_nx2 words (Maxwell 0xefe0, frontend/maxwell/translate/impl/register_map_nx2.cpp), and
// the translator reads those numbers in the next word as the extended registers. A high GPR in the scratch range
// (R246 up) or R239, which the lowering's own words name too, is renamed first: its field gets a marker, a number of
// the uniform bank the program never uses, and the prefix maps the marker to the register. Two runs a prefix: numbers
// at bits 0-7 and 20-27, targets at 8-15 and 28-35 (16-19 is the predicate, PT), lengths - 1 at 36-39, the run count
// at 40-41.
struct RegisterRun { u32 number, target, length; };
inline u64 RegisterMapPrefix(const RegisterRun* runs, u32 count) {
    u64 w = 0xefe0000000000000ULL | (u64{7} << 16) | (u64{count} << 40);
    constexpr u32 number_at[2]{0, 20}, target_at[2]{8, 28};
    for (u32 i = 0; i < count; ++i) {
        w |= u64{runs[i].number & 0xff} << number_at[i];
        w |= u64{runs[i].target & 0xff} << target_at[i];
        w |= u64{(runs[i].length - 1) & 3} << (36 + 2 * i);
    }
    return w;
}
// (hookshot) Set while a program that failed with the rules above is lowered again (LowerToMaxwell): more fields
// renamed and their exact widths. Only for those, so that every program that lowered before lowers to the same words.
inline thread_local bool extended_renaming = false;
// Fields that hold GPRs in the opcodes whose high GPRs may be renamed (ALU forms as operand() reads them, and the
// global loads and stores): 1 dest, 2 Ra, 4 Rb at 32, 8 at 64 (Rc, or Rb in forms 2, 3 and 7)
inline u32 RenameableFields(u32 opcode) {
    const u32 family = opcode & 0x1ff, form = (opcode >> 9) & 7;
    switch (opcode) {
    case 0x381: case 0x981: return 1 | 2;   // LDG Rd, [Ra] (0x381: Ra a register pair)
    case 0x986: return 2 | 4;          // STG [Ra], Rb
    case 0x593: return 1 | 2 | 4;      // SUATOM: scalar destination, coordinate and addend
    default: break;
    }
    if (extended_renaming) {
        switch (opcode) {
        case 0x805: return 1;                   // (hookshot) CS2R.64 Rd, SRZ: the pair it zeroes
        case 0x308: return 1 | 4;               // (hookshot) MUFU Rd, Rb
        case 0x387: case 0x987: return 2 | 4;   // (hookshot) STL [Ra..], Rb
        case 0x983: return 1 | 2;               // (hookshot) LDL Rd, [Ra..]
        default: break;
        }
        // (hookshot) ISETP and FSETP: Ra, and Rb in the register form (their destinations are predicates)
        if (family == 0x00c || family == 0x00b) return form >= 1 && form <= 6 ? 2 | (form == 1 ? 4 : 0) : 0;
    }
    const bool three = family == 0x010 || family == 0x011 || family == 0x012 || family == 0x016 || family == 0x019 ||
                       family == 0x023 || family == 0x024 || family == 0x025 || family == 0x027;
    const bool two = family == 0x020 || family == 0x021 || family == 0x008 || family == 0x007 || family == 0x009 ||
                     family == 0x017;
    // (hookshot: extended, form 6 too, a uniform register at 32 that is no GPR field: Rd, Ra and Rc are; OoT's mesh
    // programs computed into R226 while UR10 lived there)
    if ((!three && !two) || form < 1 || form > (extended_renaming ? 6U : 5U)) return 0;
    u32 fields = 1 | 2;
    if (form == 1) fields |= 4;                          // Rb a register
    if (three || form == 2 || form == 3) fields |= 8;     // Rc, or Rb at 64
    return fields;
}
// (hookshot, extended) How many GPRs a field names, where the opcode tells: the 32-bit ALU forms one each, IMAD.WIDE's
// Rd and Rc pairs, CS2R.64's pair; 0 where it does not (a pair or quad by the register's alignment, as before). A
// 32-bit operand taken for a quad reached R239 or the scratch from R236 or R244 and failed whole mesh programs (OoT's
// foliage).
inline u32 FieldSpan(u32 opcode, u32 field) {
    if (opcode == 0x593) return field < 3 ? 1 : 0;
    if (!extended_renaming) return 0;
    const u32 family = opcode & 0x1ff, form = (opcode >> 9) & 7;
    if (opcode == 0x805) return field == 0 ? 2 : 0;
    if (opcode == 0x308) return 1;
    // (hookshot) R2UR URd, Ra: one 32-bit register. Taken for a pair by its alignment, R2UR UR6, R238 counted R239, the
    // lowering's own scratch, and one of OoT's vertex programs was refused
    if (opcode == 0x3c2) return field == 1 ? 1 : 0;
    if (opcode == 0x387 || opcode == 0x987 || opcode == 0x983) return field == 1 ? 1 : 0;   // (the address register)
    if (form < 1 || form > 7) return 0;
    if (family == 0x025) return field == 0 || field == 3 ? 2 : 1;
    switch (family) {
    case 0x010: case 0x011: case 0x012: case 0x016: case 0x019: case 0x023: case 0x024: case 0x027:
    case 0x020: case 0x021: case 0x007: case 0x008: case 0x009: case 0x017: case 0x00b: case 0x00c:
        return 1;
    default:
        return 0;
    }
}
struct LoweredProgram {
    std::vector<u64> instructions;
    std::vector<u32> source_instruction_offsets;
    u32 source_size{};
    u32 required_texture_buffer = ~0U;
    // UR28 and up seen without a mirror: lowering again with this many registers below R216
    // reserved for them (even, so pairs stay adjacent) maps them.
    u32 high_uniform_registers{};
    // (hookshot) the reserved numbers uniform_register gave, all instructions: what renamed registers' markers avoid
    std::array<bool, 256> used_reserved{};
    u32 register_map_words{};   // (hookshot: the REGMAP_nx2 prefixes emitted)
    u32 error_offset{};
    std::string_view error;

    explicit operator bool() const { return error.empty(); }
};

// Lower only operations whose semantics are representable by the existing
// Maxwell frontend. These are new instructions, not reinterpretations of the
// native bytes. Scheduling/control words are added by the environment adapter.
inline LoweredProgram LowerToMaxwellWith(std::span<const Instruction> code, u32 high_uniforms,
                                         const std::array<bool, 256>& avoid = {}, bool survey = false) {
    LoweredProgram result;
    // UR28 and up (when used) live just below R216: UR28 at R(216 - high_uniforms).
    const u32 high_uniform_base = 216 - high_uniforms;
    struct Relocation { u32 output_index; s64 target; };
    std::vector<Relocation> relocations;
    std::vector<u32> barrier_tokens;
    std::vector<u32> call_targets;
    std::vector<u32> call_links;
    bool main_exited = false;
    bool computed_branch = false; // a jump table: the program ends at its self-branch, not an EXIT
    bool uses_uniform_bank = false;
    for (const auto& instruction : code) {
        const u32 op = instruction.Bits(0, 12);
        uses_uniform_bank |= op == 0x3c4 || op == 0x2bd || op == 0x499 || op == 0x4a4 || op == 0xc07 || op == 0xc08 || op == 0x896 || op == 0xf60 || op == 0xc11 || op == 0xd08 || op == 0xc0c || op == 0x986 || op == 0x9a8 || op == 0xc12 || op == 0xc17 || op == 0xe24 || op == 0xc24 || op == 0xd00 || op == 0xd09 || op == 0x9c3 || op == 0xc45 || op == 0x2a4 || op == 0x891 || op == 0x890 || op == 0x290 || op == 0x28c || op == 0x88c || op == 0x287 || op == 0x887 || op == 0x958 || op == 0xab9 || op == 0xabb || op == 0x899 || op == 0xc20 || op == 0xe21 || op == 0xe23 || op == 0xc02 ||
                             op == 0x886 || op == 0x3c2 || op == 0xc82 || op == 0x882 || op == 0x292 || op == 0x892 || op == 0x291 || op == 0x981 || op == 0xc13 || op == 0xc10 || op == 0xc23 || op == 0x526 || op == 0x2be || op == 0x8a4 || op == 0xd05 || (op == 0x984 && instruction.Bits(91, 1)) || op == 0x89c || op == 0x883 || op == 0x2a5 || op == 0xd06 || op == 0xf63 || op == 0xf69 || op == 0xc0b || op == 0x8a5 || op == 0x299 || op == 0xc09 || (op == 0x81c && instruction.Bits(67, 1)) || op == 0xc0a ||
                             ((op == 0x987 || op == 0x983) && instruction.Bits(91, 1));
    }
    const auto address_of = [](u32 index) { return 0x50U + index * 8 + ((index + 1) / 3) * 8; };
    const auto fail = [&](u32 index, std::string_view reason) {
        result.instructions.clear();
        result.source_instruction_offsets.clear();
        result.error_offset = index * sizeof(Instruction);
        result.error = reason;
        return result;
    };
    const auto finish = [&](u32 index) {
        for (const auto& relocation : relocations) {
            if (relocation.target > index) return fail(index, "native branch or call leaves captured program");
            const u32 target = result.source_instruction_offsets[static_cast<size_t>(relocation.target)];
            const s64 relative = s64{address_of(target)} - address_of(relocation.output_index) - 8;
            if (relative < -0x800000 || relative > 0x7fffff) return fail(index, "lowered branch distance");
            result.instructions[relocation.output_index] |= (static_cast<u64>(relative) & 0xffffff) << 20;
        }
        result.source_size = (index + 1) * sizeof(Instruction);
        return result;
    };
    if (code.empty() || code.size() > 65536) return fail(0, "invalid native program size");
    // (hookshot) A shared memory store (STS) not yet followed by a barrier: an LDS after it may read what another lane of the
    // warp stored (see LDS below)
    bool shared_store_pending = false;
    for (u32 index = 0; index < code.size(); ++index) {
        auto insn = code[index];
        result.source_instruction_offsets.push_back(static_cast<u32>(result.instructions.size()));
        u32 opcode = insn.Bits(0, 12);
        if (insn.Bits(12, 4) == 15) continue; // @!PT: never executes (compiler padding)
        u32 f16_dest = 255, f16_second = 255, f16_components = 0;
        std::optional<u32> active_link;
        u32 active_entry = 0;
        if (main_exited) {
            for (size_t call = 0; call < call_targets.size(); ++call) {
                if (call_targets[call] <= index && call_targets[call] >= active_entry) {
                    active_entry = call_targets[call];
                    active_link = call_links[call];
                }
            }
            // RET, BRA, NOP and ISETP/FSETP (predicate outputs) have no destination GPR; bits 16..23
            // are not a write to R0. IMAD.WIDE writes a register pair.
            const bool no_destination = opcode == 0x950 || opcode == 0x947 || opcode == 0x918 ||
                                        (opcode & 0x1ff) == 0x00c || (opcode & 0x1ff) == 0x00b;
            const bool pair = opcode == 0x225 || opcode == 0x625 || opcode == 0x825;
            if (active_link && !no_destination &&
                (insn.Bits(16, 8) == *active_link || (pair && insn.Bits(16, 8) + 1 == *active_link)))
                return fail(index, "native callee overwrites link register");
        }
        // Scratch values and emulated uniform state occupy reserved high GPRs. (hookshot) A native access that could
        // alias them makes the instruction's high GPRs extended registers (RegisterMapPrefix): every field that may
        // be one, each as the first of four (vectors), the over-counted kept apart from the lowering's own below
        const u32 reserved_base = uses_uniform_bank ? high_uniform_base : 246U;
        std::array<bool, 256> native_high{};
        bool any_high = false, high_in_scratch = false, high_239 = false;
        RegisterRun renamed[8];
        u32 nrenamed = 0;
        std::array<bool, 256> marker_taken{};
        std::array<bool, 256> native_occupied{};
        // (RX_VK_MARKERS_V205=1: markers avoid only the native high registers, as in v2.0.5: an A/B for the AMD stripes report)
        static const bool markers_v205 = env_on("RX_VK_MARKERS_V205");
        {
            const u32 renameable = RenameableFields(opcode);
            constexpr u32 bits[4]{16, 24, 32, 64};
            // (a pair or quad starts at an even or a 4-aligned register: an odd one is one register, R4n+2 at most two)
            const auto span_of = [](u32 reg) { return (reg & 1) ? 1U : (reg & 2) ? 2U : 4U; };
            for (u32 f = 0; f < 4; ++f) {   // what may be native high GPRs, and which are to be renamed
                const u32 bit = bits[f], reg = insn.Bits(bit, 8);
                // Survey all original operands before rewriting any of them. A marker chosen for
                // an early field must not alias a later source (including GPRs below reserved_base).
                // Keep the same conservative pair/quad spans used for high-register accesses.
                for (u32 k = 0; k < span_of(reg) && reg + k < 255; ++k)
                    native_occupied[reg + k] = true;
                // (ISBEWR's data too: not counted, a mesh program's output held in a high GPR was read from the plain register,
                // never written, while what computes it wrote the extended one: vertices past the first 32 came out undefined.
                // SUST and SURED's data as well, the same field. (amd-fix, Lichii #149) RED's too: a reduction's data in a high GPR
                // read the plain register, never written. (Lichii #588) TXD's four gradients (0xb6c) and the global atomic's
                // data (0x9a8) too: both are registers at bit 32)
                const bool register_field = bit != 32 || insn.Bits(9, 3) == 1 || opcode == 0xb60 || opcode == 0xb66 ||
                                            opcode == 0x927 || opcode == 0xb9b || opcode == 0x59f || opcode == 0x983 ||
                                            opcode == 0x98e || opcode == 0xb6c || opcode == 0x9a8 || (renameable & (1U << f));
                if (reg >= 255 || reg + 3 < reserved_base) continue;   // (a quad below may reach up)
                if (register_field && reg >= reserved_base) any_high = true;
            }
            // (hookshot) An ALU instruction's immediate form (form 4) has a 32-bit immediate at bit 32, no register: its low
            // byte was marked as a high GPR. LOP3.LUT R231, R231, 0xfffffffe made it R254, a scratch register, and two of OoT's
            // compute programs were refused. Every other field and opcode as before (TLD4, TMML, TEX, SHFL.IDX and STL read
            // a register at bit 32 that the field test above does not name)
            const u32 family = opcode & 0x1ff, form = (opcode >> 9) & 7;
            const bool alu = family == 0x010 || family == 0x011 || family == 0x012 || family == 0x016 || family == 0x019 ||
                             family == 0x023 || family == 0x024 || family == 0x025 || family == 0x027 || family == 0x020 ||
                             family == 0x021 || family == 0x008 || family == 0x007 || family == 0x009 || family == 0x017 ||
                             family == 0x00b || family == 0x00c;
            for (u32 f = 0; f < 4 && any_high; ++f) {
                const u32 bit = bits[f], reg = insn.Bits(bit, 8);
                if (bit == 32 && alu && form == 4) continue;   // (the immediate)
                if (reg >= 255 || reg + 3 < reserved_base) continue;
                const u32 exact = FieldSpan(opcode, f);
                const u32 span = exact ? exact : span_of(reg);
                if (exact && reg + span <= reserved_base) continue;   // (wholly below the reserved registers)
                bool reaches_scratch = reg + span > 246 || (reg <= 239 && reg + span > 239);
                for (u32 k = 0; k < span && reg + k < 255; ++k)   // (or a number a uniform register has somewhere)
                    reaches_scratch |= avoid[reg + k];
                if ((renameable & (1U << f)) && reaches_scratch && reg >= reserved_base) {
                    // a marker block, aligned as the register, among the uniform bank's numbers nobody uses here
                    u32 marker = 255;
                    for (u32 m = 216; m + span <= 246 && marker == 255; m += span) {
                        bool ok = true;
                        for (u32 k = 0; k < span && ok; ++k)
                            ok = m + k != 239 && !avoid[m + k] && !marker_taken[m + k] &&
                                 !(markers_v205 ? native_high[m + k] : native_occupied[m + k]);
                        if (ok) marker = m;
                    }
                    if (marker != 255 && nrenamed < 8) {
                        bool same = false;   // (the same register in two fields: one marker)
                        for (u32 r = 0; r < nrenamed; ++r)
                            if (renamed[r].target == reg) {
                                same = true;
                                insn.words[bit / 32] = (insn.words[bit / 32] & ~(0xffU << (bit % 32))) | (renamed[r].number << (bit % 32));
                            }
                        if (!same) {
                            for (u32 k = 0; k < span; ++k) marker_taken[marker + k] = true;
                            renamed[nrenamed++] = {marker, reg, span};
                            insn.words[bit / 32] = (insn.words[bit / 32] & ~(0xffU << (bit % 32))) | (marker << (bit % 32));
                        }
                        continue;
                    }
                }
                for (u32 k = 0; k < span && reg + k < 255; ++k) {
                    if (reg + k < reserved_base) continue;
                    native_high[reg + k] = true;
                    high_in_scratch |= reg + k >= 246;
                    high_239 |= reg + k == 239;
                }
            }
        }
        if (!any_high) native_high = {};
        static const bool no_extended = env_on("RX_VK_NO_EXTREGS");   // (as before them: rejected)
        if (any_high && no_extended) return fail(index, "native program uses lowering scratch registers");
        std::array<bool, 256> lowering_used{};   // (the reserved numbers uniform_register gave this instruction)
        const size_t words_before = result.instructions.size();
        const size_t relocations_before = relocations.size();
        // TLD/TLD4 .F16 (bit 79): the components come packed in pairs, (0, 1) in Rd and (2, 3) in
        // the second destination. Lowered as the F32 form into R248/R249 and R246/R247, then packed.
        if ((opcode == 0xb66 || opcode == 0xb63) && insn.Bits(79, 2) == 1) {
            f16_dest = insn.Bits(16, 8);
            f16_second = insn.Bits(64, 8);
            f16_components = static_cast<u32>(std::popcount(insn.Bits(72, 4)));
            if (f16_dest >= 246 || (f16_second != 255 && f16_second >= 246) || insn.Bits(61, 3) == 5 ||
                (f16_second == 255 && f16_components > 2))
                return fail(index, "native half-precision texture result registers");
            insn.words[2] &= ~(1U << 15);
            insn.words[0] = (insn.words[0] & ~0x00ff0000U) | (f16_dest == 255 ? 0x00ff0000U : (248U << 16));
            if (f16_second != 255) insn.words[2] = (insn.words[2] & ~0xffU) | 246U;
        }
        // UR0-UR21 live in R216-R237 and UR22-UR27 in R240-R245 (R239 holds the uniform
        // predicates), so even/odd pairs stay adjacent; UR28 and up below R216. Sites that
        // still check reg < 16 rely on UR0-UR15 being R216-R231.
        const auto uniform_register_of = [&](u32 reg) -> u32 {
            if (reg < 22) return 216 + reg;
            if (reg < 28) return 218 + reg;
            if (reg == 63) return 255;
            if (reg < 63 && reg - 28 < high_uniforms) return high_uniform_base + (reg - 28);
            if (reg < 63) result.high_uniform_registers = (std::max)(result.high_uniform_registers, (reg - 26) & ~1U);
            return ~0U;
        };
        const auto uniform_register = [&](u32 reg) -> u32 {
            const u32 r = uniform_register_of(reg);
            if (r < 255) lowering_used[r] = true;
            return r;
        };
        bool rewritten_uniform = false;
        bool uniform_guard = false;
        u32 uniform_output = 7; // ULOP3's UPn output: computed in P(6-n), then merged into R239
        if (opcode == 0x2a4 || opcode == 0x8a4 || opcode == 0x891 || opcode == 0x890 || opcode == 0x290 ||
            opcode == 0x292 || opcode == 0x892 || opcode == 0x291 || opcode == 0x896 || opcode == 0x8a5 ||
            opcode == 0x4a4) {   // (hookshot: 0x4a4, UIMAD URd, URa, imm, URc, as IMAD 0x424)
            // (hookshot) a UPn guard on any of these that writes no predicate (no carry out: bits 81-83 and 84-86
            // PT), as ULOP3's: swapped into P(6-n) around the op (guarded_uniform below)
            // (ULEA, 0x291/0x891, has one carry out: its bits 84-86 are no predicate)
            const bool lea = opcode == 0x291 || opcode == 0x891;
            const bool no_predicate_out = insn.Bits(81, 3) == 7 && (lea || insn.Bits(84, 3) == 7);
            // (hookshot) and a guarded ULEA with a carry out: the LEA below starts that UPn from R239 when guarded
            if (insn.Bits(12, 4) != 7 && opcode != 0x292 && opcode != 0x892 && !no_predicate_out && !lea)
                return fail(index, "native uniform arithmetic predication");
            uniform_guard = insn.Bits(12, 4) != 7;
            rewritten_uniform = true;
            const u32 dst = uniform_register(insn.Bits(16, 8));
            const u32 a = uniform_register(insn.Bits(24, 8));
            const u32 c = uniform_register(insn.Bits(64, 8));
            if (dst == ~0U || a == ~0U || c == ~0U) return fail(index, "native uniform arithmetic register");
            insn.words[0] = (insn.words[0] & 0xffff) | (dst << 16) | (a << 24);
            insn.words[2] = (insn.words[2] & ~0x080000ffU) | c;
            if (opcode == 0x2a4 || opcode == 0x290 || opcode == 0x292 || opcode == 0x291) {
                const u32 b = uniform_register(insn.Bits(32, 8));
                if (b == ~0U) return fail(index, "native uniform arithmetic second register");
                insn.words[1] = (insn.words[1] & ~0xffU) | b;
            }
            if ((opcode == 0x292 || opcode == 0x892) && insn.Bits(80, 4) != 0xe) {
                if (insn.Bits(80, 1)) return fail(index, "native uniform logic predicate output");
                uniform_output = insn.Bits(81, 3);
                insn.words[2] = (insn.words[2] & ~(7U << 17)) | ((6 - uniform_output) << 17);
            }
            opcode = opcode == 0x2a4 ? 0x224 : opcode == 0x8a4 ? 0x824 : opcode == 0x4a4 ? 0x424 : opcode == 0x891 ? 0x811 : opcode == 0x890 ? 0x810
                   : opcode == 0x292 ? 0x212 : opcode == 0x892 ? 0x812 : opcode == 0x291 ? 0x211
                   : opcode == 0x896 ? 0x816 : opcode == 0x8a5 ? 0x825 : 0x210;
        }
        // (hookshot) and the uniform shifts, whose guard is swapped in the same way
        const bool uniform_predication = opcode == 0xabb || opcode == 0xab9 || opcode == 0xc82 || opcode == 0x882 ||
                                         opcode == 0x899 || opcode == 0x299 || opcode == 0x499;
        const u32 native_predicate = insn.Bits(12, 4);
        const bool guarded_uniform = (uniform_predication || uniform_guard) && native_predicate != 7;
        // A uniform guard UPn lives in R239 bit 6-n and is swapped into P(6-n) for the op.
        const u32 guard_slot = 6 - (native_predicate & 7);
        u64 predicate = u64{guarded_uniform ? ((native_predicate & 8) | guard_slot) : native_predicate} << 16;
        const u64 dest = insn.Bits(16, 8);
        const auto base = [&](u64 opcode_bits) { return opcode_bits | predicate | dest; };
        // NVN2 fullscreen vertex shaders read bank 24, which the game never binds (the console reads
        // zeros from an unbound bank); Maxwell has 18, so read it from bank 17, also never bound.
        const auto bank = [&]() -> u32 {
            const u32 value = insn.Bits(54, 5);
            return value == 24 ? 17U : value;
        };
        const auto cbuf = [&]() -> u64 {
            return (u64{insn.Bits(38, 16) / 4} << 20) | (u64{bank()} << 34);
        };
        const auto valid_cbuf = [&] {
            return !insn.Bits(91, 1) && bank() < 18 &&
                   (insn.Bits(38, 16) % 4) == 0;
        };
        std::string_view operand_error;
        const auto emit = [&](u64 word) { result.instructions.push_back(word); };
        // Bound Maxwell texture instructions share one descriptor constant buffer per program.
        const auto bind_texture_buffer = [&](u32 binding) {
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return false;
            result.required_texture_buffer = binding;
            return true;
        };
        // P(guard slot) while a uniform guard is swapped in; P6 for lowerings that use it as
        // scratch (R2UR, MATCH, HFMA2.RELU), which save and restore it with the defaults below.
        const u32 guard_mask = guard_slot < 7 ? 1U << guard_slot : 0U;
        const auto save_predicate = [&](u32 mask = 1U << 6) {
            emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
        };
        const auto restore_predicate = [&](u32 mask = 1U << 6) {
            emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
        };
        if (guarded_uniform) {
            save_predicate(guard_mask);
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{guard_mask} << 20));
        }
        const u32 output_mask = uniform_output == 7 ? 0U : 1U << (6 - uniform_output);
        if (output_mask) {
            // Keep the guest P(6-n) in R254 and start from the current UPn (a false guard keeps it).
            emit(0x38e8000000070000ULL | 254 | (u64{255} << 8) | (u64{output_mask} << 20));
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{output_mask} << 20));
        }
        // Pd = LUT(a, b, c) over predicate operands (index | negate << 3): each input becomes
        // 0 or ~0 (SEL), and LOP3 sets Pd when the result is nonzero.
        const auto predicate_lut = [&](u32 lut, u32 a, u32 b, u32 c, u32 target, bool uniform_c = false) {
            const u32 inputs[3]{a, b, c};
            for (u32 i = 0; i < 3; ++i) {
                if (i == 2 && uniform_c) {
                    // UPn from R239 bit 6-n as 0 or ~0 (UPT is ~0); a negation flips the table.
                    if ((c & 7) == 7) {
                        emit(0x0100000000000000ULL | predicate | 252 | (u64{15} << 12) | (u64{~0U} << 20));
                    } else {
                        emit(0x3800000000000000ULL | predicate | 252 | (u64{239} << 8) |
                             (u64{(6 - (c & 7)) | (1U << 8)} << 20));
                        emit(0x5c10000000000000ULL | predicate | 252 | (u64{255} << 8) |
                             (u64{252} << 20) | (u64{1} << 48));
                    }
                    if (c & 8) lut = ((lut & 0x55) << 1) | ((lut & 0xaa) >> 1);
                    continue;
                }
                emit(0x38a0000000000000ULL | predicate | (250 + i) | (u64{255} << 8) |
                     (u64{0x7ffff} << 20) | (u64{1} << 56) | (u64{inputs[i] ^ 8U} << 39));
            }
            emit(0x5be0000000000000ULL | predicate | 255 | (u64{250} << 8) | (u64{251} << 20) |
                 (u64{lut} << 28) | (u64{3} << 36) | (u64{252} << 39) | (u64{target} << 48));
        };
        const auto mov_imm = [&](u32 reg, u32 value) {
            emit(0x0100000000000000ULL | predicate | reg | (u64{15} << 12) | (u64{value} << 20));
        };
        const auto materialize = [&](u32 kind, u32 reg, u32 scratch, bool absolute, bool negate) -> u32 {
            if (kind == 3) {
                const u32 mapped = uniform_register(reg);
                if (mapped == ~0U) { operand_error = "native uniform source register"; return 255; }
                reg = mapped;
            } else if (kind == 1) {
                mov_imm(scratch, insn.words[1]);
                reg = scratch;
            } else if (kind == 2) {
                if (!valid_cbuf()) { operand_error = "native constant buffer address"; return 255; }
                emit(0x4c98000000000000ULL | predicate | scratch | (u64{15} << 39) | cbuf());
                reg = scratch;
            }
            if (absolute || negate) {
                if (reg != scratch) emit(0x5c98000000000000ULL | predicate | scratch | (u64{reg} << 20) | (u64{15} << 39));
                if (absolute) emit(0x0400000000000000ULL | predicate | scratch | (u64{scratch} << 8) | (u64{0x7fffffff} << 20));
                if (negate) emit(0x0400000000000000ULL | predicate | scratch | (u64{scratch} << 8) | (u64{0x80000000} << 20) | (u64{2} << 53));
                reg = scratch;
            }
            return reg;
        };
        const auto operand = [&](u32 which, bool floating = true) -> u32 {
            const u32 form = insn.Bits(9, 3);
            u32 reg{}, kind{}, abs{}, neg{};
            if (which == 0) {
                reg = insn.Bits(24, 8); abs = insn.Bits(73, 1); neg = insn.Bits(72, 1);
            } else {
                if (form != 1 && form != 2 && form != 3 && form != 4 && form != 5 && form != 6 && form != 7) {
                    operand_error = "native ALU source form"; return 255;
                }
                const bool wide = which == 1 ? (form == 4 || form == 5 || form == 6) : (form == 2 || form == 3 || form == 7);
                if (wide) {
                    kind = form == 6 || form == 7 ? 3 : (form == 4 || form == 2) ? 1 : 2;
                    if (kind == 3) reg = insn.Bits(32, 8);
                    if (kind == 2 || kind == 3) { abs = insn.Bits(62, 1); neg = insn.Bits(63, 1); }
                } else {
                    const bool upper = which == 1 ? (form == 2 || form == 3 || form == 7) : true;
                    reg = insn.Bits(upper ? 64 : 32, 8);
                    abs = insn.Bits(upper ? 74 : 62, 1); neg = insn.Bits(upper ? 75 : 63, 1);
                }
            }
            return materialize(kind, reg, 250 + which, floating && abs, floating && neg);
        };
        // (hookshot) Ampere SHF in general: (hi:lo) shifted by an immediate or a register, bits 73-74 the type (0 S64,
        // 1 U64, 2 S32, 3 U32), 75 wrap, 76 right, 80 the high word of the result. Maxwell's SHF.R gives the low
        // word of (hi:lo) >> s and SHF.L the high word of (hi:lo) << s (max shift at 37-38: U32 0, U64 2, S64 3;
        // wrap at 50, as Eden's decoder reads them); the other two words are a plain SHR of hi or SHL of lo
        // (.W at 39). False: a shift by an immediate past the type's width.
        const auto funnel = [&](u32 dst, u32 lo, u32 hi, bool shift_in_register, u32 shift, u32 type, bool right,
                                bool hi_word, bool wrap) -> bool {
            const bool wide = type < 2, sign = type == 0 || type == 2;
            if (!shift_in_register && shift >= (wide ? 64U : 33U)) return false;
            const u64 amount = u64{shift} << 20, w39 = u64{wrap} << 39;
            if (right != hi_word) {
                // (hookshot, from Lichii) an immediate's bits 20-38 are all the amount: the clamp at 37-38 made every shift 63
                if (!shift_in_register && wide && shift >= 32) {
                    emit((right ? 0x3828000000000000ULL : 0x3848000000000000ULL) | predicate | dst |
                         (u64{right ? hi : lo} << 8) | (u64{shift - 32} << 20) | (u64{right && sign} << 48));
                    return true;
                }
                const u64 op = shift_in_register ? (right ? 0x5cf8000000000000ULL : 0x5bf8000000000000ULL)
                                                 : (right ? 0x38f8000000000000ULL : 0x36f8000000000000ULL);
                const u64 max_shift = !shift_in_register || !wide ? 0U : sign ? 3U : 2U;
                emit(op | predicate | dst | (u64{lo} << 8) | amount | (max_shift << 37) | (u64{hi} << 39) |
                     (u64{wrap} << 50));
                return true;
            }
            if (!shift_in_register && shift >= 32) {   // (the word shifted out entirely)
                if (right && sign) emit(0x3828000000000000ULL | predicate | dst | (u64{hi} << 8) | (u64{31} << 20) |
                                        (u64{1} << 48));
                else emit(0x5c98000000000000ULL | predicate | dst | (u64{255} << 20) | (u64{15} << 39));
                return true;
            }
            if (right) emit((shift_in_register ? 0x5c28000000000000ULL : 0x3828000000000000ULL) | predicate | dst |
                            (u64{hi} << 8) | amount | w39 | (u64{sign} << 48));
            else emit((shift_in_register ? 0x5c48000000000000ULL : 0x3848000000000000ULL) | predicate | dst |
                      (u64{lo} << 8) | amount | w39);
            return true;
        };
        struct HalfSource { u32 reg, swizzle; bool absolute, negate; };
        const auto half_source = [&](u32 which) -> HalfSource {
            if (which == 0) return {insn.Bits(24, 8), insn.Bits(74, 2),
                                    insn.Bits(73, 1) != 0, insn.Bits(72, 1) != 0};
            const u32 form = insn.Bits(9, 3);
            const bool wide = which == 1 ? (form == 4 || form == 5) : (form == 2 || form == 3);
            if (wide) {
                const bool immediate = form == 2 || form == 4;
                const u32 reg = materialize(immediate ? 1 : 2, 255, 250 + which, false, false);
                return {reg, immediate ? 0U : insn.Bits(60, 2),
                        !immediate && insn.Bits(62, 1), !immediate && insn.Bits(63, 1)};
            }
            const bool upper = which == 2 || form == 2 || form == 3;
            return {insn.Bits(upper ? 64 : 32, 8), insn.Bits(upper ? 81 : 60, 2),
                    insn.Bits(upper ? 83 : 62, 1) != 0, insn.Bits(upper ? 84 : 63, 1) != 0};
        };
        const auto extract_half = [&](HalfSource source, u32 lane, u32 target) {
            const bool high = source.swizzle == 3 || (source.swizzle == 0 && lane == 1);
            emit(0x5ca8000000000000ULL | predicate | target | (u64{2} << 8) |
                 (u64{source.swizzle == 1 ? 2U : 1U} << 10) | (u64{source.reg} << 20) |
                 (u64{high} << 41) | (u64{source.negate} << 45) | (u64{source.absolute} << 49));
        };
        const auto xmad = [&](u32 target, u32 a, u32 b, u32 c, bool a_high,
                              bool b_high, u32 mode, bool immediate) {
            emit((immediate ? 0x3600000000000000ULL : 0x5b00000000000000ULL) |
                 predicate | target | (u64{a} << 8) |
                 (u64{immediate ? ((b >> (b_high ? 16 : 0)) & 0xffff) : b} << 20) |
                 (u64{!immediate && b_high} << 35) | (u64{c} << 39) |
                 (u64{mode} << 50) | (u64{a_high} << 53));
        };
        const auto multiply_low = [&](u32 a, u32 b, u32 c, bool immediate) {
            xmad(250, a, b, c, false, false, 0, immediate);
            xmad(251, a, b, 255, true, false, 0, immediate);
            xmad(251, a, b, 251, false, true, 0, immediate);
            emit(base(0x5c18000000000000ULL) | (u64{251} << 8) |
                 (u64{250} << 20) | (u64{16} << 39));
        };
        switch (opcode) {
        case 0x304: { // F2F.F16.F32, scalar round-to-nearest conversion.
            if (insn.words[2] != 0x00200800 || insn.Bits(24, 8) ||
                (insn.words[1] & ~0xc00000ffU))
                return fail(index, "native scalar float conversion mode");
            emit(base(0x5ca8000000000000ULL) | (u64{1} << 8) | (u64{2} << 10) |
                 (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(62, 1)} << 49) |
                 (u64{insn.Bits(63, 1)} << 45));
            break;
        }
        case 0x23e: case 0xa3e: { // F2FP.PACK_AB / MERGE_C, round-to-nearest
            if (insn.Bits(62, 2) || insn.Bits(73, 5) || insn.Bits(79, 2) ||
                insn.Bits(81, 10) || insn.Bits(91, 1)) {
                return fail(index, "native float packing modifiers");
            }
            const bool merge = insn.Bits(78, 1);
            if (insn.Bits(72, 1)) return fail(index, "native float packing selector");
            const u32 a = merge ? operand(2, false) : operand(0, false);
            const u32 b = operand(1, false);
            // Canonical F2F writes the converted half in its low 16 bits.
            if (merge) {
                emit(0x5c98000000000000ULL | predicate | 250 | (u64{a} << 20) | (u64{15} << 39));
            } else {
                emit(0x5ca8000000000000ULL | predicate | 250 | (u64{1} << 8) |
                     (u64{2} << 10) | (u64{a} << 20));
            }
            emit(0x5ca8000000000000ULL | predicate | 251 | (u64{1} << 8) |
                 (u64{2} << 10) | (u64{b} << 20));
            emit(base(0x5c18000000000000ULL) | (u64{250} << 8) | (u64{251} << 20) | (u64{16} << 39));
            break;
        }
        case 0x232: case 0x832: case 0xa32: { // HMUL2
            if (insn.Bits(78, 2) || insn.Bits(81, 10) || insn.Bits(91, 1))
                return fail(index, "native half multiply mode");
            const auto a = half_source(0), b = half_source(1);
            const u32 precision = insn.Bits(76, 1) ? 2 : insn.Bits(80, 1);
            emit(base(0x5d08000000000000ULL) | (u64{a.reg} << 8) | (u64{b.reg} << 20) |
                 (u64{b.swizzle} << 28) | (u64{b.absolute} << 30) | (u64{a.negate != b.negate} << 31) |
                 (u64{insn.Bits(77, 1)} << 32) | (u64{precision} << 39) |
                 (u64{a.absolute} << 44) | (u64{a.swizzle} << 47));
            break;
        }
        case 0x230: case 0x430: case 0x630: { // HADD2
            if (insn.Bits(76, 1) || insn.Bits(79, 1) || insn.Bits(81, 10) || insn.Bits(91, 1))
                return fail(index, "native half add mode");
            const auto a = half_source(0);
            const auto b = half_source(opcode == 0x230 ? 1 : 2);
            if (insn.Bits(78, 1)) {
                if (a.reg != 255 || !a.negate || a.absolute || a.swizzle != 0 ||
                    b.swizzle < 2 || insn.Bits(77, 1) || insn.Bits(80, 1))
                    return fail(index, "native float-output half add mode");
                extract_half(b, 0, static_cast<u32>(dest));
            } else {
                emit(base(0x5d10000000000000ULL) | (u64{a.reg} << 8) | (u64{b.reg} << 20) |
                     (u64{b.swizzle} << 28) | (u64{b.absolute} << 30) | (u64{b.negate} << 31) |
                     (u64{insn.Bits(77, 1)} << 32) | (u64{insn.Bits(80, 1)} << 39) |
                     (u64{a.negate} << 43) | (u64{a.absolute} << 44) | (u64{a.swizzle} << 47));
            }
            break;
        }
        case 0x231: case 0x431: case 0x631: case 0x831: case 0xa31: { // HFMA2
            const bool relu = insn.Bits(79, 1) != 0;
            if (insn.Bits(78, 1) || insn.Bits(85, 1) || insn.Bits(91, 1) ||
                (relu ? (insn.Bits(87, 4) != 7 ||
                         insn.Bits(77, 1)) : insn.Bits(87, 4) != 0))
                return fail(index, "native half fused multiply-add mode");
            if (relu && native_predicate != 7) {
                // Guard the whole expansion before its temporary P6 updates.
                relocations.push_back({static_cast<u32>(result.instructions.size()), index + 1});
                emit(0xe24000000000000fULL | (u64{native_predicate ^ 8U} << 16));
                predicate = u64{7} << 16;
            }
            auto a = half_source(0);
            auto b = half_source(1);
            const auto c = half_source(2);
            if (insn.Bits(86, 1)) { // H0_NH1: negate only the upper packed component.
                if (b.swizzle != 0 || b.absolute || b.reg >= 250)
                    return fail(index, "native half mixed-sign source mode");
                emit(0x0400000000000000ULL | predicate | 251 | (u64{b.reg} << 8) |
                     (u64{0x80000000} << 20) | (u64{2} << 53));
                b.reg = 251;
            }
            if (b.absolute || c.absolute || (a.absolute && a.reg >= 248))
                return fail(index, "native half fused multiply-add absolute source");
            if (a.absolute) {
                emit(0x0400000000000000ULL | predicate | 250 | (u64{a.reg} << 8) |
                     (u64{a.swizzle == 1 ? 0x7fffffffU : 0x7fff7fffU} << 20));
                a.reg = 250;
                a.absolute = false;
            }
            const u32 precision = insn.Bits(76, 1) ? 2 : insn.Bits(80, 1);
            emit(0x5d00000000000000ULL | predicate | (relu ? 254U : dest) | (u64{a.reg} << 8) | (u64{b.reg} << 20) |
                 (u64{b.swizzle} << 28) | (u64{c.negate} << 30) | (u64{a.negate != b.negate} << 31) |
                 (u64{insn.Bits(77, 1)} << 32) | (u64{c.swizzle} << 35) |
                 (u64{precision} << 37) | (u64{c.reg} << 39) | (u64{a.swizzle} << 47));
            if (relu) {
                // Clamp the rounded binary16 lanes, preserving positive infinity
                // and canonicalizing either sign of NaN without saturating to one.
                save_predicate();
                for (u32 lane = 0; lane < 2; ++lane) {
                    emit(0x3828000000070000ULL | 251 | (u64{254} << 8) | (u64{lane * 16} << 20));
                    emit(0x0400000000070000ULL | 251 | (u64{251} << 8) | (u64{0xffff} << 20));
                    emit(0x0400000000070000ULL | 252 | (u64{251} << 8) | (u64{0x7fff} << 20));
                    mov_imm(250, 0x7c00);
                    emit(0x5b60000000070000ULL | 7 | (u64{6} << 3) | (u64{250} << 8) |
                         (u64{252} << 20) | (u64{7} << 39) | (u64{1} << 49));
                    emit(0x3828000000070000ULL | 252 | (u64{251} << 8) | (u64{15} << 20));
                    mov_imm(250, 1);
                    emit(0x5c10000000070000ULL | 252 | (u64{252} << 8) | (u64{250} << 20) | (u64{1} << 48));
                    emit(0x5c40000000070000ULL | 251 | (u64{251} << 8) | (u64{252} << 20) | (u64{7} << 48));
                    mov_imm(250, 0x7fff);
                    emit(0x5ca0000000070000ULL | 251 | (u64{250} << 8) | (u64{251} << 20) | (u64{6} << 39));
                    if (lane == 0) {
                        emit(base(0x5c98000000000000ULL) | (u64{251} << 20) | (u64{15} << 39));
                    } else {
                        emit(0x3848000000070000ULL | 251 | (u64{251} << 8) | (u64{16} << 20));
                        emit(base(0x5c40000000000000ULL) | (dest << 8) | (u64{251} << 20) |
                             (u64{1} << 41) | (u64{7} << 48));
                    }
                }
                restore_predicate();
            }
            break;
        }
        case 0x240: case 0x840: { // HMNMX2, exact half promotion for min/max
            if (insn.Bits(76, 11) || insn.Bits(91, 1))
                return fail(index, "native half min/max mode");
            const auto a = half_source(0);
            const auto b = opcode == 0x840 ? HalfSource{255, 0, false, false} : half_source(1);
            if (a.swizzle == 1 || b.swizzle == 1 || (b.reg >= 250 && b.reg != 255))
                return fail(index, "native half min/max source mode");
            for (u32 lane = 0; lane < 2; ++lane) {
                extract_half(a, lane, 250);
                if (opcode == 0x840) {
                    const u32 half = (insn.words[1] >> (lane * 16)) & 0xffff;
                    mov_imm(251, half);
                    extract_half(HalfSource{251, 2, false, false}, 0, 251);
                } else {
                    extract_half(b, lane, 251);
                }
                const u32 target = lane == 0 ? 252 : 250;
                emit(0x5c60000000000000ULL | predicate | target | (u64{250} << 8) | (u64{251} << 20) |
                     (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                emit(0x5ca8000000000000ULL | predicate | target | (u64{1} << 8) |
                     (u64{2} << 10) | (u64{target} << 20));
            }
            emit(base(0x5c18000000000000ULL) | (u64{250} << 8) | (u64{252} << 20) | (u64{16} << 39));
            break;
        }
        case 0xc24: case 0xe24: { // IMAD low word with a uniform multiplier or addend.
            // 0xe24 keeps the uniform addend in bits 32-37; bit 63 negates it ("RZ, RZ, -UR4").
            const bool negate_c = opcode == 0xe24 && insn.Bits(63, 1) != 0;
            if ((insn.words[2] & ~0x080002ffU) != 0x078e0000 ||
                (insn.words[1] & ~(0xffU | (negate_c ? 0x80000000U : 0U))))
                return fail(index, "native uniform-addend multiply mode");
            const u32 a = operand(0, false), b = operand(1, false);
            u32 c = operand(2, false);
            if (negate_c) {
                emit(0x5c10000000000000ULL | predicate | 252 | (u64{255} << 8) | (u64{c} << 20) |
                     (u64{1} << 48));
                c = 252;
            }
            multiply_low(a, b, c, false);
            break;
        }
        case 0x224: { // IMAD register / zero-product move
            if (insn.Bits(24, 8) == 255 && insn.words[1] == 255 && insn.Bits(75, 1) &&
                (insn.words[2] & ~0xaffU) == 0x078e0000) {
                emit(base(0x5c10000000000000ULL) | (u64{255} << 8) |
                    (u64{insn.Bits(64, 8)} << 20) | (u64{1} << 48));
                break;
            }
            if (insn.Bits(24, 8) == 255 && insn.words[1] == 255 &&
                (insn.words[2] & ~0xffU) == 0x078e0000) {
                emit(base(0x5c98000000000000ULL) | (u64{15} << 39) | (u64{insn.Bits(64, 8)} << 20));
                break;
            }
            if (insn.Bits(24, 8) == 255 && insn.words[1] == 255 && (insn.words[2] & ~0x07800affU) == 0x000e0400 &&
                (!rewritten_uniform || insn.Bits(87, 3) == 7)) {
                // IMAD.X RZ, RZ, c, Pn: the high word of a 64-bit add (bit 75 inverts c).
                u32 c = insn.Bits(64, 8);
                mov_imm(250, 1);
                emit(0x5ca0000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                     (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                if (insn.Bits(75, 1)) {
                    emit(0x0400000000000000ULL | predicate | 251 | (u64{c} << 8) | (u64{0xffffffffU} << 20) | (u64{2} << 53));
                    c = 251;
                }
                emit(base(0x5c10000000000000ULL) | (u64{c} << 8) | (u64{250} << 20));
                break;
            }
            if ((insn.words[2] & ~0x2ffU) != 0x078e0000 || (insn.words[1] & ~0xffU))
                return fail(index, "native integer register multiply mode");
            multiply_low(insn.Bits(24, 8), insn.Bits(32, 8), insn.Bits(64, 8), false);
            break;
        }
        case 0x2a5: { // UIMAD.WIDE.U32 UR(d):UR(d+1) = URa * URb + URc:UR(c+1)
            const u32 a = uniform_register(insn.Bits(24, 8)), b = uniform_register(insn.Bits(32, 8));
            const u32 c = insn.Bits(64, 8);
            const u32 c_low = uniform_register(c), c_high = c == 63 ? 255U : uniform_register(c + 1);
            const u32 d_low = uniform_register(insn.Bits(16, 8)), d_high = uniform_register(insn.Bits(16, 8) + 1);
            if (native_predicate != 7 || (insn.words[1] & ~0xffU) || (insn.words[2] & ~0xffU) != 0x0f8e0000 ||
                (insn.Bits(16, 8) & 1) || (c != 63 && (c & 1)) || a == ~0U || b == ~0U || c_low == ~0U ||
                c_high == ~0U || d_low == ~0U || d_high == ~0U || d_low == 255)
                return fail(index, "native uniform wide multiply mode");
            // High word in R253 (base-2^16 long multiplication, as IMAD.HI), low word in R250.
            xmad(250, a, b, 255, false, false, 0, false);
            xmad(251, a, b, 250, true, false, 2, false);
            xmad(250, a, b, 251, false, true, 1, false);
            xmad(252, a, b, 251, true, true, 2, false);
            emit(0x3828000000070000ULL | 250 | (u64{250} << 8) | (u64{16} << 20));
            emit(0x5c10000000070000ULL | 253 | (u64{250} << 8) | (u64{252} << 20));
            xmad(250, a, b, 255, false, false, 0, false);
            xmad(251, a, b, 255, true, false, 0, false);
            xmad(251, a, b, 251, false, true, 0, false);
            emit(0x5c18000000070000ULL | 250 | (u64{251} << 8) | (u64{250} << 20) | (u64{16} << 39));
            emit(0x5c10000000070000ULL | 250 | (u64{250} << 8) | (u64{c_low} << 20) | (u64{1} << 47));
            emit(0x5c10000000070000ULL | d_high | (u64{253} << 8) | (u64{c_high} << 20) | (u64{1} << 43));
            emit(0x5c10000000070000ULL | d_low | (u64{250} << 8) | (u64{255} << 20));
            break;
        }
        case 0x226: { // IDP.2A.LO.U16.U8: a.lo16 * b.byte0 + a.hi16 * b.byte1 + c
            if ((insn.words[2] & ~0xffU) != 0x1000 || (insn.words[1] & ~0xffU))
                return fail(index, "native packed dot-product mode");
            const u32 a = insn.Bits(24, 8), b = insn.Bits(32, 8), c = insn.Bits(64, 8);
            for (u32 component = 0; component < 2; ++component) {
                emit(0x3828000000000000ULL | predicate | (250 + component) | (u64{b} << 8) | (u64{component * 8} << 20));
                emit(0x0400000000000000ULL | predicate | (250 + component) | (u64{250 + component} << 8) | (u64{0xff} << 20));
            }
            xmad(252, a, 250, c, false, false, 0, false);
            xmad(static_cast<u32>(dest), a, 251, 252, true, false, 0, false);
            break;
        }
        case 0x225: case 0x227: case 0x625: case 0x825: case 0x827: case 0xa27: {
            // IMAD.HI Rd / IMAD.WIDE Rd:Rd+1 = a * b + c:c+1 (.U32 or signed, bit 73), with an optional
            // carry-out (bits 81-83). b is a register, an immediate (0x8xx) or a cbuf (0xa27); 0x625 takes
            // b from bits 64-71 and adds a cbuf pair.
            const bool wide = opcode == 0x225 || opcode == 0x625 || opcode == 0x825;
            const bool immediate = opcode == 0x825 || opcode == 0x827, signed_product = insn.Bits(73, 1);
            const u32 c = opcode == 0x625 ? 255U : insn.Bits(64, 8), carry_output = insn.Bits(81, 3);
            if ((insn.words[2] & ~0x000e02ffU) != 0x07800000 ||
                ((opcode == 0x225 || opcode == 0x227) && (insn.words[1] & ~0xffU)) ||
                ((opcode == 0xa27 || opcode == 0x625) && (!valid_cbuf() || insn.Bits(62, 2))) ||
                (opcode == 0x625 && insn.Bits(38, 16) > 0xfff8) || (c != 255 && (c & 1)) ||
                (wide && (dest == 255 || (dest & 1))) || (signed_product && carry_output != 7))
                return fail(index, "native high multiply mode");
            const u32 a = insn.Bits(24, 8);
            const u32 b = immediate ? insn.words[1] : opcode == 0xa27 ? 254U : insn.Bits(opcode == 0x625 ? 64 : 32, 8);
            if (opcode == 0xa27) emit(0x4c98000000000000ULL | predicate | 254 | (u64{15} << 39) | cbuf());
            if (signed_product) {
                // The signed high word is the unsigned one minus (a < 0 ? b : 0) + (b < 0 ? a : 0),
                // which R253 holds (R252 is free until the multiplication).
                const u32 b_register = immediate ? 254U : b;
                if (immediate) mov_imm(254, b);
                emit(0x3828000000000000ULL | predicate | 253 | (u64{a} << 8) | (u64{31} << 20) | (u64{1} << 48));
                emit(0x5c40000000000000ULL | predicate | 253 | (u64{253} << 8) | (u64{b_register} << 20) | (u64{7} << 48));
                emit(0x3828000000000000ULL | predicate | 252 | (u64{b_register} << 8) | (u64{31} << 20) | (u64{1} << 48));
                emit(0x5c40000000000000ULL | predicate | 252 | (u64{252} << 8) | (u64{a} << 20) | (u64{7} << 48));
                emit(0x5c10000000000000ULL | predicate | 253 | (u64{253} << 8) | (u64{252} << 20));
            }
            // Base-2^16 long multiplication. Each intermediate sum fits U32.
            xmad(250, a, b, 255, false, false, 0, immediate);
            xmad(251, a, b, 250, true, false, 2, immediate);
            xmad(250, a, b, 251, false, true, 1, immediate);
            xmad(252, a, b, 251, true, true, 2, immediate);
            emit(0x3828000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{16} << 20));
            if (signed_product)
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{253} << 20) | (u64{1} << 48));
            if (!wide && c == 255 && carry_output == 7) {
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{252} << 20));
                break;
            }
            // High word in R253 and low word in R250 (as UIMAD.WIDE.U32), then c:c+1 is added.
            emit(0x5c10000000000000ULL | predicate | 253 | (u64{250} << 8) | (u64{252} << 20));
            xmad(250, a, b, 255, false, false, 0, immediate);
            xmad(251, a, b, 255, true, false, 0, immediate);
            xmad(251, a, b, 251, false, true, 0, immediate);
            emit(0x5c18000000000000ULL | predicate | 250 | (u64{251} << 8) | (u64{250} << 20) | (u64{16} << 39));
            u32 c_low = c, c_high = c == 255 ? 255U : c + 1;
            if (opcode == 0x625) {
                emit(0x4c98000000000000ULL | predicate | 252 | (u64{15} << 39) | cbuf());
                emit(0x4c98000000000000ULL | predicate | 254 | (u64{15} << 39) | (cbuf() + (u64{1} << 20)));
                c_low = 252;
                c_high = 254;
            }
            const u32 high = wide ? static_cast<u32>(dest) + 1 : static_cast<u32>(dest);
            emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{c_low} << 20) | (u64{1} << 47));
            if (carry_output == 7) {
                emit(0x5c10000000000000ULL | predicate | high | (u64{253} << 8) | (u64{c_high} << 20) | (u64{1} << 43));
                if (wide) emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{255} << 20));
                break;
            }
            // Maxwell IADD has no .X with .CC: R251 takes the low carry, R254 counts the high word's.
            emit(0x5c10000000000000ULL | predicate | 251 | (u64{255} << 8) | (u64{255} << 20) | (u64{1} << 43));
            emit(0x5c10000000000000ULL | predicate | 253 | (u64{253} << 8) | (u64{c_high} << 20) | (u64{1} << 47));
            emit(0x5c10000000000000ULL | predicate | 254 | (u64{255} << 8) | (u64{255} << 20) | (u64{1} << 43));
            emit(0x5c10000000000000ULL | predicate | 253 | (u64{253} << 8) | (u64{251} << 20) | (u64{1} << 47));
            emit(0x5c10000000000000ULL | predicate | 254 | (u64{254} << 8) | (u64{255} << 20) | (u64{1} << 43));
            emit(0x5c98000000000000ULL | predicate | high | (u64{253} << 20) | (u64{15} << 39));
            if (wide) emit(base(0x5c98000000000000ULL) | (u64{250} << 20) | (u64{15} << 39));
            emit(0x5b60000000000000ULL | predicate | 7 | (u64{carry_output} << 3) | (u64{254} << 8) |
                 (u64{255} << 20) | (u64{7} << 39) | (u64{5} << 49));
            break;
        }
        case 0xa24: { // IMAD, constant-buffer multiplier
            if ((insn.words[2] & ~0x2ffU) != 0x078e0000 || !valid_cbuf() || insn.Bits(62, 2))
                return fail(index, "native integer constant-buffer multiply mode");
            emit(0x4c98000000000000ULL | predicate | 252 | (u64{15} << 39) | cbuf());
            multiply_low(insn.Bits(24, 8), 252, insn.Bits(64, 8), false);
            break;
        }
        case 0x424: case 0x624: { // IMAD.MOV.U32: zero product plus immediate/cbuf
            if (opcode == 0x624 && insn.Bits(24, 8) == 255 &&
                (insn.words[2] & ~0x07800000U) == 0x000e06ffU) {
                const u32 source = operand(2, false);
                mov_imm(250, 1);
                emit(0x5ca0000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                    (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                emit(base(0x5c10000000000000ULL) | (u64{source} << 8) | (u64{250} << 20));
                break;
            }
            if (insn.Bits(24, 8) != 255 || insn.Bits(64, 8) != 255 || insn.words[2] != 0x078e00ff) {
                if ((insn.words[2] & ~0x2ffU) != 0x078e0000 || (opcode == 0x624 && insn.Bits(62, 2)))
                    return fail(index, "native multiply wide addend mode");
                const u32 a = operand(0, false), b = operand(1, false), c = operand(2, false);
                multiply_low(a, b, c, false);
                break;
            }
            if (insn.Bits(24, 8) != 255 || insn.Bits(64, 8) != 255 ||
                insn.words[2] != 0x078e00ff)
                return fail(index, "native integer move mode");
            if (opcode == 0x624) {
                if (!valid_cbuf() || insn.Bits(62, 2))
                    return fail(index, "native integer move constant buffer");
                emit(base(0x4c98000000000000ULL) | (u64{15} << 39) | cbuf());
            } else {
                emit(base(0x0100000000000000ULL) | (u64{15} << 12) |
                     (u64{insn.words[1]} << 20));
            }
            break;
        }
        case 0x824: { // Captured low-word IMAD forms; bit 75 negates the addend.
            const bool negate_c = insn.Bits(75, 1) != 0;
            if (insn.words[1] == 1 && (insn.words[2] & ~(0x07800800U | 0xffU)) == 0x000e0600U) {
                // IMAD.X Rd, Ra, 1, Rc, Pn: Ra + Rc plus the carry predicate in bits 87-90 (~Rc with bit 75).
                u32 c = insn.Bits(64, 8);
                mov_imm(252, 1);
                emit(0x5ca0000000000000ULL | predicate | 252 | (u64{252} << 8) | (u64{255} << 20) |
                     (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                if (negate_c) {
                    emit(0x0400000000000000ULL | predicate | 251 | (u64{c} << 8) | (u64{0xffffffffU} << 20) | (u64{2} << 53));
                    c = 251;
                }
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{c} << 20));
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{252} << 20));
                break;
            }
            if (insn.words[1] == 1 && (insn.words[2] & 0xfffff700U) == 0x078e0200) {
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{insn.Bits(64, 8)} << 20) | (u64{negate_c} << 48));
                break;
            }
            const u32 multiplier = insn.words[1];
            if (insn.Bits(64, 8) == 255 && !negate_c && multiplier > 1 && !(multiplier & (multiplier - 1)) &&
                (insn.words[2] & ~0x2ffU) == 0x078e0000) {
                // IMAD.SHL by a power of two: one shift (also the form Eden's jump-table tracking
                // expects between the clamp and the table load).
                u32 shift = 0;
                while ((1U << shift) != multiplier) ++shift;
                emit(base(0x3848000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{shift} << 20));
                break;
            }
            if (insn.words[1] != 48 || insn.words[2] != 0x078e02ff) {
                if ((insn.words[2] & ~0xaffU) != 0x078e0000)
                    return fail(index, "native integer multiply mode");
                u32 c = insn.Bits(64, 8);
                if (negate_c && c != 255) {
                    emit(0x5c10000000000000ULL | predicate | 252 | (u64{255} << 8) | (u64{c} << 20) |
                         (u64{1} << 48));
                    c = 252;
                }
                multiply_low(insn.Bits(24, 8), insn.words[1], c, true);
                break;
            }
            const u64 source = insn.Bits(24, 8);
            emit(0x5c18000000000000ULL | predicate | 250 | (source << 8) | (source << 20) | (u64{1} << 39));
            emit(base(0x3848000000000000ULL) | (u64{250} << 8) | (u64{4} << 20));
            break;
        }
        case 0x806: { // VOTE: register ballot, ordinary predicate input and output.
            if (insn.words[1] || insn.Bits(72, 2) > 2 || (insn.words[2] & ~0x078e0300U))
                return fail(index, "native vote mode");
            emit(base(0x50d8000000000000ULL) | (u64{insn.Bits(87, 4)} << 39) |
                 (u64{insn.Bits(81, 3)} << 45) | (u64{insn.Bits(72, 2)} << 48));
            break;
        }
        case 0x886: { // VOTEU ballot and independent uniform predicate output.
            const u32 output = insn.Bits(81, 3), target = insn.Bits(16, 8);
            if ((target != 63 && uniform_register(target) == ~0U) || insn.words[1] ||   // (hookshot: any UR)
                insn.Bits(72, 2) > 2 || native_predicate != 7)
                return fail(index, "native uniform vote mode");
            const u32 temporary = output == 7 ? 7 : 6 - output;
            const u32 mask = 1U << temporary;
            if (output != 7) emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
            emit(0x50d8000000070000ULL | uniform_register(target) |
                (u64{insn.Bits(87, 4)} << 39) | (u64{temporary} << 45) | (u64{insn.Bits(72, 2)} << 48));
            if (output != 7) {
                emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{mask} << 20));
                emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
            }
            break;
        }
        case 0x3c2: { // R2UR: broadcast the first lane satisfying the execution predicate.
            const u32 target = uniform_register(static_cast<u32>(dest));
            if (target == ~0U || target == 255 || insn.words[1] || insn.words[2] != 0x000e0000)
                return fail(index, "native register-to-uniform mode");
            save_predicate();
            emit(0x50d8000000070000ULL | 250 | (u64{native_predicate} << 39) |
                 (u64{6} << 45) | (u64{1} << 48));
            emit(0x5c10000000070000ULL | 251 | (u64{250} << 8) | (u64{255} << 20) | (u64{1} << 49));
            emit(0x5c40000000070000ULL | 250 | (u64{250} << 8) | (u64{251} << 20) | (u64{7} << 48));
            emit(0x5c30000000070000ULL | 250 | (u64{250} << 20));
            emit(0xef10000000070000ULL | 251 | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{250} << 20) | (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            if (native_predicate == 7) {
                // Every lane executing it is in the ballot, so no lane-less fallback can be taken.
                // Leaving the old value selectable would make it a source of the result for the
                // texture and global memory tracking (a handle traced to the uniform's last load).
                emit(0x5c98000000070000ULL | target | (u64{15} << 39) | (u64{251} << 20));
            } else {
                emit(0x5ca0000000070000ULL | target | (u64{251} << 8) |
                     (u64{target} << 20) | (u64{6} << 39));
            }
            restore_predicate();
            break;
        }
        case 0x81c: { // PLOP3.LUT; bit 67 makes the third source a uniform predicate.
            const u32 target = insn.Bits(81, 3);
            const u32 lut = insn.Bits(64, 3) | (insn.Bits(72, 5) << 3);
            if (target == 7 || insn.Bits(84, 3) != 7 ||
                insn.Bits(16, 8) || insn.words[1])
                return fail(index, "native predicate LUT mode");
            if (!insn.Bits(67, 1) && (lut == 0xfe || lut == 0xa8)) {
                emit(0x5090000000000000ULL | predicate | (u64{target} << 3) | 7 |
                     (u64{insn.Bits(87, 4)} << 12) | (u64{1} << 24) |
                     (u64{insn.Bits(77, 4)} << 29) | (u64{insn.Bits(68, 4)} << 39) |
                     (u64{lut == 0xfe} << 45));
                break;
            }
            if (!insn.Bits(67, 1) && (lut == 0x80 ||
                (lut == 8 && insn.Bits(68, 4) == 7 && insn.Bits(77, 4) == 7))) {
                emit(0x5090000000000000ULL | predicate | (u64{target} << 3) | 7 |
                     (u64{insn.Bits(87, 4) ^ (lut == 8 ? 8U : 0U)} << 12) | (u64{insn.Bits(77, 4)} << 29) |
                     (u64{insn.Bits(68, 4)} << 39));
                break;
            }
            predicate_lut(lut, insn.Bits(87, 4), insn.Bits(77, 4), insn.Bits(68, 4), target,
                          insn.Bits(67, 1) != 0);
            break;
        }
        case 0x89c: { // UPLOP3.LUT: load the uniform predicates into P0-P6, combine, merge back.
            const u32 target = insn.Bits(81, 3);
            const u32 lut = insn.Bits(64, 3) | (insn.Bits(72, 5) << 3);
            if (insn.Bits(84, 3) != 7 || insn.Bits(67, 1) || insn.Bits(16, 8) || insn.words[1])
                return fail(index, "native uniform predicate LUT mode");
            if (target == 7) break;
            const auto slot = [](u32 operand) { return (operand & 7) == 7 ? operand : (operand & 8) | (6 - (operand & 7)); };
            predicate = u64{slot(native_predicate)} << 16; // a UPn guard is loaded as well
            emit(0x38e8000000070000ULL | 254 | (u64{255} << 8) | (u64{0x7f} << 20));
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{0x7f} << 20));
            predicate_lut(lut, slot(insn.Bits(87, 4)), slot(insn.Bits(77, 4)), slot(insn.Bits(68, 4)), 6 - target);
            emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{1U << (6 - target)} << 20));
            emit(0x38f0000000070000ULL | (u64{254} << 8) | (u64{0x7f} << 20));
            break;
        }
        case 0x883: { // UP2UR UR, UPR, URZ, mask: bit k of the result is UPk (R239 bit 6-k).
            const u32 target = uniform_register(insn.Bits(16, 8)), mask = insn.words[1];
            if (target == ~0U || insn.Bits(24, 8) != 63 || native_predicate != 7 ||
                insn.words[2] != 0x08000000 || mask > 0x7f)
                return fail(index, "native uniform predicate move mode");
            if (target == 255) break;
            mov_imm(target, 0);
            for (u32 k = 0; k < 7; ++k) {
                if (!((mask >> k) & 1)) continue;
                emit(0x3800000000070000ULL | 250 | (u64{239} << 8) | (u64{(6 - k) | (1U << 8)} << 20));
                emit(0x36f0000000070000ULL | target | (u64{250} << 8) | (u64{k | (1U << 8)} << 20) |
                     (u64{target} << 39));
            }
            break;
        }
        case 0x882: { // UMOV immediate. (hookshot: any UR, through uniform_register)
            const u32 d = uniform_register(dest);
            if (d == ~0U || insn.words[2]) return fail(index, "native uniform immediate move mode");
            if (d != 255) mov_imm(d, insn.words[1]);
            break;
        }
        case 0xc82: { // UMOV UR, UR (or URZ), including uniform execution predicates.
            const u32 source = insn.words[1] < 64 ? uniform_register(insn.words[1]) : ~0U;
            const u32 d = uniform_register(dest);   // (hookshot: any UR)
            if (d == ~0U || d == 255 || source == ~0U || insn.words[2] != 0x08000000)
                return fail(index, "native uniform move mode");
            emit(0x5c98000000000000ULL | predicate | d |
                 (u64{source} << 20) | (u64{15} << 39));
            break;
        }
        case 0xab9: case 0xabb: { // ULDC U32/U64, direct or indexed.
            const u32 uniform_dest = insn.Bits(16, 8), index_reg = insn.Bits(24, 8);
            const u32 width = insn.Bits(73, 3);
            const u32 target = uniform_register(uniform_dest);
            const u32 index_target = opcode == 0xabb ? uniform_register(index_reg) : 255U;
            // (hookshot) U8/S8/U16/S16 too (0-3, Maxwell LDC's sizes), each aligned to its own size
            const u32 align = width >= 5 ? 8U : width == 4 ? 4U : width >= 2 ? 2U : 1U;
            if (target == ~0U || target == 255 || width > 5 ||
                (width == 5 && ((uniform_dest & 1) || uniform_register(uniform_dest + 1) != target + 1)) ||
                bank() >= 18 || (opcode == 0xab9 && insn.Bits(38, 16) % (align < 4 ? align : 4U)) || insn.Bits(62, 2) ||
                (insn.words[2] & ~0xe00U) != (opcode == 0xab9 ? 0U : 0x08000000U) ||
                index_target == ~0U)
                return fail(index, "native uniform constant load mode");
            if (opcode == 0xab9 && width == 4 && insn.Bits(38, 16) % 4 == 0) {
                emit(0x4c98000000000000ULL | predicate | target | (u64{15} << 39) | cbuf());
            } else {
                emit(0xef90000000000000ULL | predicate | target |
                     (u64{index_target} << 8) | (u64{insn.Bits(38, 16)} << 20) |
                     (u64{bank()} << 36) | (u64{width} << 48));
            }
            break;
        }
        case 0x899: { // USHF.L.U32; accept a zero word outside the selected half.
            const u32 dst = insn.Bits(16, 8), low = insn.Bits(24, 8), high = insn.Bits(64, 8);
            const u32 dst_reg = uniform_register(dst), low_reg = uniform_register(low);
            const u32 high_reg = uniform_register(high);
            const bool dst_ok = dst != 63 && dst_reg != ~0U;
            const bool low_ok = low != 63 && low_reg != ~0U, high_ok = high != 63 && high_reg != ~0U;
            if (insn.words[2] == (0x08011400U | high) && low == 63 && high_ok &&
                dst_ok && (native_predicate == 7 || guarded_uniform) && insn.words[1] <= 32) {
                emit(0x3828000000000000ULL | predicate | dst_reg |
                     (u64{high_reg} << 8) | (u64{insn.words[1]} << 20) | (u64{1} << 48));
                break;
            }
            // Logical right shifts of one register (bit 76 right, bit 80 high word, bits 73-74
            // U32): USHF.R.U32 URd, URa, n, URZ and USHF.R.U32.HI URd, URZ, n, URb.
            const bool right_low = insn.words[2] == 0x0800163f && low_ok;
            const bool right_high = insn.words[2] == (0x08011600U | high) && low == 63 && high_ok;
            if ((right_low || right_high) && dst_ok && (native_predicate == 7 || guarded_uniform) && insn.words[1] <= 32) {
                emit(0x3828000000000000ULL | predicate | dst_reg |
                     (u64{right_high ? high_reg : low_reg} << 8) | (u64{insn.words[1]} << 20));
                break;
            }
            const bool high_result = insn.words[2] == (0x08010600U | high) && low == 63 && high_ok;
            const bool low_result = insn.words[2] == 0x0800063f && low_ok;
            if (dst_ok && (native_predicate == 7 || guarded_uniform) && !high_result && !low_result && !insn.Bits(75, 1) &&
                (insn.words[2] & ~0x000116ffU) == 0x08000000U) {
                // (hookshot) the general funnel shift by an immediate, both words: bits 73-74 the type (0 S64,
                // 1 U64, 2 S32, 3 U32), 76 right, 80 the high word of the result. Maxwell's SHF.R gives the
                // low word of (high:low) >> s and SHF.L the high word of (high:low) << s (bits 37-38 the
                // maximum shift, U64 2 / S64 3); the other two words are plain shifts of one source.
                const u32 type = insn.Bits(73, 2), shift = insn.words[1];
                const bool wide = type < 2, sign = type == 0 || type == 2, right = insn.Bits(76, 1) != 0;
                const bool hi = insn.Bits(80, 1) != 0;
                const u32 lo_r = low == 63 ? 255U : low_reg, hi_r = high == 63 ? 255U : high_reg;
                const bool lo_valid = low == 63 || low_ok, hi_valid = high == 63 || high_ok;
                if (lo_valid && hi_valid && shift < (wide ? 64U : 33U)) {
                    const u64 max_shift = 0;   // (hookshot, from Lichii: not with an immediate, as in funnel)
                    if (wide && shift >= 32 && right != hi) {
                        emit((right ? 0x3828000000000000ULL : 0x3848000000000000ULL) | predicate | dst_reg |
                             (u64{right ? hi_r : lo_r} << 8) | (u64{shift - 32} << 20) | (u64{right && sign} << 48));
                        break;
                    }
                    if (right && !hi) {
                        emit(0x38f8000000000000ULL | predicate | dst_reg | (u64{lo_r} << 8) | (u64{shift} << 20) |
                             (max_shift << 37) | (u64{hi_r} << 39));
                        break;
                    }
                    if (!right && hi) {
                        emit(0x36f8000000000000ULL | predicate | dst_reg | (u64{lo_r} << 8) | (u64{shift} << 20) |
                             (max_shift << 37) | (u64{hi_r} << 39));
                        break;
                    }
                    if (shift < 32) {
                        if (right) emit(0x3828000000000000ULL | predicate | dst_reg | (u64{hi_r} << 8) |
                                        (u64{shift} << 20) | (u64{sign} << 48));
                        else emit(0x3848000000000000ULL | predicate | dst_reg | (u64{lo_r} << 8) | (u64{shift} << 20));
                        break;
                    }
                }
            }
            if (!dst_ok || (native_predicate != 7 && !guarded_uniform) || insn.words[1] > 32 || (!high_result && !low_result))
                return fail(index, "native uniform shift mode");
            emit(0x3848000000000000ULL | predicate | dst_reg |
                 (u64{high_result ? high_reg : low_reg} << 8) | (u64{insn.words[1]} << 20));
            break;
        }
        case 0xb82: { // LDC.64 indexed byte offset; bits 78-79 the mode as Maxwell's (2 = IS)
            // .IS: Ra + offset addresses all constant buffers as one space, the buffer in its high
            // half (Kirby Air Riders' vertex shaders), which Eden's Maxwell LDC implements.
            const u32 width = insn.Bits(73, 3), mode = insn.Bits(78, 2);
            // (hookshot) U8/S8/U16/S16 (0-3) too: Maxwell's LDC has the same size codes, and Eden's decoder
            // implements them (OoT's fragment and mesh programs read byte tables from banks 8 and 9).
            if (width > 5 || (mode != 0 && mode != 2) || insn.Bits(91, 1) ||
                bank() >= 18 ||
                (width == 5 && ((dest & 1) || dest >= 254))) return fail(index, "native indexed constant load mode");
            emit(base(0xef90000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(38, 16)} << 20) | (u64{bank()} << 36) | (u64{mode} << 44) | (u64{width} << 48));
            break;
        }
        case 0xf89: { // SHFL with immediate lane and segmentation/clamp mask, any mode.
            const u32 lane = insn.Bits(53, 5), mask = insn.Bits(40, 13), mode = insn.Bits(58, 2);
            if (insn.words[2] != 0x000e0000 || insn.Bits(32, 8) || insn.Bits(60, 4))
                return fail(index, "native immediate shuffle mode");
            emit(base(0xef10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{lane} << 20) | (u64{1} << 28) | (u64{1} << 29) |
                 (u64{mode} << 30) | (u64{mask} << 34) | (u64{7} << 48));
            break;
        }
        case 0x822: { // FSWZADD: per lane 0 = a+b, 1 = a-b, 2 = b-a, 3 = b (nvdisasm PP/PN/NP/ZP).
            const u32 swizzle = insn.words[1];
            if (swizzle > 0xff || insn.words[2] > 255)
                return fail(index, "native swizzled add mode");
            // Maxwell's lane codes 1 and 2 are the other way round (1 = b-a, 2 = a-b).
            const u32 maxwell = ((swizzle & 0x55) << 1) | ((swizzle & 0xaa) >> 1);
            emit(base(0x50f8000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(64, 8)} << 20) | (u64{maxwell} << 28));
            break;
        }
        case 0xb6c: { // TXD 2D or ARRAY_2D, any channels; the two destination pairs anywhere.
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            const u32 coordinates = insn.Bits(24, 8), derivatives = insn.Bits(32, 8);
            // Ampere dimensions: 1 = 2D, 5 = ARRAY_2D; Maxwell TXD's are 2 and 3. The coordinates
            // (x, y[, layer]) and the four gradients sit in the same registers for both.
            const u32 dimension = insn.Bits(61, 3);
            const u32 high = insn.Bits(64, 8); // destination of the third and fourth channels
            // Bit 90 is forwarded below (Maxwell's NODEP); Splatoon Raiders sets it. Bits 72-75 are
            // the channel mask: the first two enabled channels go to Rd, Rd+1, the others to the
            // second destination (RZ drops them; a third channel alone may be in an odd register).
            const u32 mask = insn.Bits(72, 4), channels = static_cast<u32>(std::popcount(mask));
            if ((insn.words[2] & ~(0xfffU | (1U << 26))) != 0x001e0000 || mask == 0 || insn.Bits(59, 2) ||
                (dimension != 1 && dimension != 5) || binding >= 18 || offset > 0x1fff ||
                (channels > 1 && (dest & 1)) || dest >= 248 || (channels <= 2 && high != 255) ||
                (high != 255 && ((channels > 3 && (high & 1)) || high >= 248)) ||
                coordinates >= 248 || derivatives >= 247)
                return fail(index, "native gradient texture mode");
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return fail(index, "native texture descriptor buffer conflict");
            result.required_texture_buffer = binding;
            // Maxwell writes the channels to consecutive registers: when the second destination
            // doesn't follow Rd (Splatoon Raiders: R12/R18, R18/R44), write them to scratch and move
            // each into place.
            const bool consecutive = channels <= 2 || high == dest + 2;
            const u64 target = consecutive ? dest : 248;
            emit(0xde00000000000000ULL | predicate | target | (u64{coordinates} << 8) |
                 (u64{derivatives} << 20) | (u64{dimension == 5 ? 3U : 2U} << 28) | (u64{mask} << 31) |
                 (u64{offset} << 36) | (u64{insn.Bits(90, 1)} << 49) | (u64{7} << 51));
            if (!consecutive) {
                for (u32 i = 0; i < channels; ++i) {
                    if (i >= 2 && high == 255) break;
                    const u32 to = i < 2 ? static_cast<u32>(dest) + i : high + (i - 2);
                    emit(0x5c98000000000000ULL | predicate | to | (u64{15} << 39) | (u64{248 + i} << 20));
                }
            }
            break;
        }
        case 0x989: { // SHFL, immediate lane and register clamp/segmentation mask, any mode.
            if ((insn.words[1] & 0xf01fffffU) || (insn.words[2] & ~0x000e00ffU))
                return fail(index, "native register-mask shuffle mode");
            emit(base(0xef10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(53, 5)} << 20) | (u64{1} << 28) | (u64{insn.Bits(58, 2)} << 30) |
                 (u64{insn.Bits(64, 8)} << 39) | (u64{insn.Bits(81, 3)} << 48));
            break;
        }
        case 0x589: { // SHFL.IDX, register lane and immediate segmentation mask.
            if ((insn.words[2] & ~0x000e0000U) || insn.Bits(53, 11))
                return fail(index, "native shuffle mode");
            emit(0xef10000000000000ULL | predicate | dest |
                (u64{insn.Bits(24, 8)} << 8) | (u64{insn.Bits(32, 8)} << 20) |
                (u64{1} << 29) | (u64{insn.Bits(40, 13)} << 34) | (u64{insn.Bits(81, 3)} << 48));
            break;
        }
        case 0x98e: { // RED.E.{ADD..XOR}.STRONG.GPU U32 (op in bits 87-89), register address.
            {   // (hookshot) any type Maxwell's RED has (bits 73-75: U32 0, S32 1, U64 2, F32 3, its bits 20-22) and a
                // register address with a signed offset, or a uniform one ([URa + offset])
                const u32 type = insn.Bits(73, 3);
                const u32 rest = insn.words[2] & ~(0x0380003fU | 0x00000e00U);
                const bool uniform_address = rest == 0x0c10e100 && insn.Bits(24, 8) == 255;
                const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
                if ((type != 0 || insn.Bits(40, 24)) && type <= 3 && (uniform_address || rest == 0x0010e100) &&
                    offset >= -0x80000 && offset <= 0x7ffff && (type != 3 || insn.Bits(87, 3) == 0)) {
                    const u32 address = uniform_address ? uniform_register(insn.Bits(64, 6)) : insn.Bits(24, 8);
                    if (address != ~0U && (address == 255 || !(address & 1))) {
                        emit(0xebf8000000000000ULL | predicate | insn.Bits(32, 8) | (u64{address} << 8) |
                             (u64{type} << 20) | (u64{insn.Bits(87, 3)} << 23) |
                             (u64{static_cast<u32>(offset) & 0xfffff} << 28) | (u64{1} << 48));
                        break;
                    }
                }
            }
            if ((insn.words[2] & ~0x0380003fU) == 0x0c10e100 && insn.Bits(24, 8) == 255) {
                // [URa + offset]: a uniform 64-bit address with a signed byte offset.
                const u32 address = uniform_register(insn.Bits(64, 6));
                const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
                if (address == ~0U || (address & 1) || offset < -0x80000 || offset > 0x7ffff)
                    return fail(index, "native global reduction uniform address");
                emit(0xebf8000000000000ULL | predicate | insn.Bits(32, 8) | (u64{address} << 8) |
                     (u64{insn.Bits(87, 3)} << 23) | (u64{static_cast<u32>(offset) & 0xfffff} << 28) |
                     (u64{1} << 48));
                break;
            }
            if ((insn.words[2] & ~0x03800000U) != 0x0010e100 || insn.Bits(40, 24) || (insn.Bits(24, 8) & 1))
                return fail(index, "native global reduction mode");
            emit(0xebf8000000000000ULL | predicate | insn.Bits(32, 8) |
                (u64{insn.Bits(24, 8)} << 8) | (u64{insn.Bits(87, 3)} << 23) | (u64{1} << 48));
            break;
        }
        case 0x2bd: { // UFLO.U32, the highest set-bit index (.SH, bit 74: its shift amount). (hookshot: any UR)
            const u32 d = uniform_register(dest), a = insn.words[1] < 64 ? uniform_register(insn.words[1]) : ~0U;
            if (d == ~0U || d == 255 || a == ~0U || (insn.words[2] & ~0x400U) != 0x080e0000 || native_predicate != 7)
                return fail(index, "native uniform leading bit mode");
            emit(0x5c30000000000000ULL | predicate | d | (u64{a} << 20) | (u64{insn.Bits(74, 1)} << 41));
            break;
        }
        case 0x2be: { // UBREV, as a bit-reversing zero shift like BREV (hookshot: any UR, through uniform_register)
            const u32 d = uniform_register(dest), a = insn.words[1] < 64 ? uniform_register(insn.words[1]) : ~0U;
            if (d == ~0U || d == 255 || a == ~0U || insn.words[2] != 0x08000000 || native_predicate != 7)
                return fail(index, "native uniform bit reverse mode");
            emit(0x3828000000000000ULL | predicate | d | (u64{a} << 8) | (u64{1} << 39) | (u64{1} << 40));
            break;
        }
        case 0x2bf: { // UPOPC, as POPC of the uniform's register (hookshot: any UR)
            const u32 d = uniform_register(dest), a = insn.words[1] < 64 ? uniform_register(insn.words[1]) : ~0U;
            if (d == ~0U || d == 255 || a == ~0U || insn.words[2] != 0x08000000 || native_predicate != 7)
                return fail(index, "native uniform population count mode");
            emit(0x5c08000000000000ULL | predicate | d | (u64{a} << 20));
            break;
        }
        case 0x3a1: { // MATCH.ALL (.U32, or .U64 on bit 73) with only the all-equal predicate retained.
            const u32 source = insn.Bits(24, 8), target = insn.Bits(81, 3);
            const u32 words = insn.Bits(73, 1) ? 2 : 1;
            const u32 scratch = target == 6 ? 5U : 6U; // the comparison, saved around the match
            if (dest != 255 || source >= 254 || (words == 2 && (source & 1)) || target == 7 ||
                native_predicate != 7 || insn.words[1] || (insn.words[2] & ~0xe0200U) != 0)
                return fail(index, "native warp match mode");
            save_predicate(1U << scratch);
            // Compare every lane with the lowest active one, then require all to agree.
            emit(0x50d8000000070000ULL | 250 | (u64{7} << 39) | (u64{7} << 45) | (u64{1} << 48));
            emit(0x5c10000000070000ULL | 251 | (u64{250} << 8) | (u64{255} << 20) | (u64{1} << 49));
            emit(0x5c40000000070000ULL | 250 | (u64{250} << 8) | (u64{251} << 20) | (u64{7} << 48));
            emit(0x5c30000000070000ULL | 250 | (u64{250} << 20));
            for (u32 word = 0; word < words; ++word)
                emit(0xef10000000070000ULL | (251 + word) | (u64{source + word} << 8) |
                    (u64{250} << 20) | (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            emit(0x5b60000000070000ULL | 7 | (u64{scratch} << 3) | (u64{source} << 8) |
                (u64{251} << 20) | (u64{7} << 39) | (u64{2} << 49));
            if (words == 2)
                emit(0x5b60000000070000ULL | 7 | (u64{scratch} << 3) | (u64{source + 1} << 8) |
                    (u64{252} << 20) | (u64{scratch} << 39) | (u64{2} << 49));
            emit(0x50d8000000070000ULL | 255 | (u64{scratch} << 39) | (u64{target} << 45));
            restore_predicate(1U << scratch);
            break;
        }
        case 0x80d: { // CSMTEST: a test of shading-mode state Eden does not model, taken as false.
            // Kirby Air Riders' vertex-side shaders run "CSMTEST.A.FCSM_TR P0" then "@P0 EXIT":
            // never leave early. Outputs of PT discard the result.
            const u32 first = insn.Bits(81, 3), second = insn.Bits(84, 3);
            if (insn.Bits(87, 4) != 7 || insn.Bits(16, 16))
                return fail(index, "native CSM test mode");
            for (const u32 target : {first, second})
                if (target != 7)
                    emit(0x5090000000000000ULL | predicate | (u64{target} << 3) | 7 | (u64{7} << 12) |
                         (u64{1} << 15) | (u64{7} << 29) | (u64{7} << 39));
            break;
        }
        case 0x3c4: { // REDUX.SUM (modulo 2^32), .MIN/.MAX (unsigned), .OR or .XOR over the actual active lanes.
            const u32 source = insn.Bits(24, 8), target = uniform_register(static_cast<u32>(dest));
            // (hookshot) bit 73: .S32. The sum, OR and XOR are the same; MIN and MAX are taken unsigned on the values
            // with their sign bits flipped, and the result flipped back
            const u32 mode = insn.words[2] & ~0x200U;
            const bool maximum = mode == 0x14000, minimum = mode == 0x10000;
            const bool flip = insn.Bits(73, 1) && (maximum || minimum);
            // OR and XOR: inactive lanes contribute zero, as for the sum. Maxwell LOP: 1 = OR, 2 = XOR.
            const u32 logical = mode == 0x4000 ? 1U : mode == 0x8000 ? 2U : 0U;
            if (target == ~0U || target == 255 || insn.words[1] ||
                (mode != 0xc000 && !maximum && !minimum && !logical) || native_predicate != 7)
                return fail(index, "native warp sum mode");
            const auto flip_sign = [&](u32 reg) {   // (LOP32I.XOR reg, 0x80000000)
                emit(0x0400000000070000ULL | reg | (u64{reg} << 8) | (u64{0x80000000U} << 20) | (u64{2} << 53));
            };
            emit(0x50d8000000070000ULL | 250 | (u64{7} << 39) | (u64{7} << 45) | (u64{1} << 48));
            mov_imm(target, minimum ? ~0U : 0U);
            for (u32 lane = 0; lane < 32; ++lane) {
                emit(0xef10000000070000ULL | 251 | (u64{source} << 8) | (u64{lane} << 20) |
                    (u64{1} << 28) | (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
                if (flip) flip_sign(251);
                emit(0x3828000000070000ULL | 252 | (u64{250} << 8) | (u64{lane} << 20));
                emit(0x0400000000070000ULL | 252 | (u64{252} << 8) | (u64{1} << 20));
                emit(0x5c10000000070000ULL | 252 | (u64{255} << 8) | (u64{252} << 20) | (u64{1} << 48));
                // Inactive lanes: 0, or ~0 for the minimum (OR with the inverted lane mask).
                emit(0x5c40000000070000ULL | 251 | (u64{251} << 8) | (u64{252} << 20) | (u64{7} << 48) |
                     (u64{minimum} << 40) | (u64{minimum} << 41));
                if (minimum) {
                    emit(0x5c20000000070000ULL | target | (u64{target} << 8) | (u64{251} << 20) | (u64{7} << 39));
                } else if (maximum) {
                    emit(0x5c20000000070000ULL | target | (u64{target} << 8) | (u64{251} << 20) | (u64{15} << 39));
                } else if (logical) {
                    emit(0x5c40000000070000ULL | target | (u64{target} << 8) | (u64{251} << 20) |
                        (u64{logical} << 41) | (u64{7} << 48));
                } else {
                    emit(0x5c10000000070000ULL | target | (u64{target} << 8) | (u64{251} << 20));
                }
            }
            if (flip) flip_sign(target);
            break;
        }
        case 0xf8c: { // ATOMS.POPC.INC.32 with no returned value: one increment per active lane.
            const u32 stride = insn.Bits(78, 2), offset = insn.Bits(40, 24);
            if (dest != 255 || (insn.words[1] & 0xff) || (offset & 3) ||
                (insn.words[2] & ~0xc000U) != 0x0d00003f)
                return fail(index, "native shared increment mode");
            u32 address = insn.Bits(24, 8);
            if (stride && address != 255) {
                emit(0x3848000000000000ULL | predicate | 251 | (u64{address} << 8) |
                    (u64{stride + 1} << 20));
                address = 251;
            }
            mov_imm(250, 1);
            emit(0xec00000000000000ULL | predicate | 255 | (u64{address} << 8) | (u64{250} << 20) |
                (u64{offset >> 2} << 30));
            break;
        }
        case 0x38c: { // ATOMS (ADD/MIN/MAX/INC/DEC/AND/OR/XOR/EXCH, U32 or S32), aligned byte offset.
            // The operation (bits 87-90) is numbered as in Maxwell ATOMS (52-55); bit 73 is S32.
            const u32 offset = insn.Bits(40, 24), operation = insn.Bits(87, 4);
            if ((insn.words[2] & ~0x07800200U) || operation > 8 || (offset & 3))
                return fail(index, "native shared atomic mode");
            emit(0xec00000000000000ULL | predicate | dest |
                (u64{insn.Bits(24, 8)} << 8) | (u64{insn.Bits(32, 8)} << 20) |
                (u64{insn.Bits(73, 1)} << 28) | (u64{offset >> 2} << 30) | (u64{operation} << 52));
            break;
        }
        case 0x3a8: { // ATOMG.E.ADD.STRONG.GPU U32 at a 64-bit register address plus an offset.
            const u32 address = insn.Bits(24, 8);
            const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
            if (insn.words[2] != 0x001ee100 || (address & 1) || address >= 246 ||
                offset < -0x80000 || offset > 0x7ffff)
                return fail(index, "native global atomic register mode");
            emit(0xed00000000000000ULL | predicate | dest | (u64{address} << 8) |
                (u64{insn.Bits(32, 8)} << 20) | (u64{static_cast<u32>(offset) & 0xfffff} << 28) |
                (u64{1} << 48));
            break;
        }
        case 0x3a9: { // (hookshot) ATOMG.E.CAS.STRONG.GPU PT, Rd, [Ra], Rb, Rc: U32, Rb the comparator, Rc the value.
            // Maxwell's ATOM.E.CAS takes them as the pair Rb, Rb+1: copied to R250/R251 unless they are one already.
            const u32 address = insn.Bits(24, 8), comparator = insn.Bits(32, 8), value = insn.Bits(64, 8);
            const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
            if ((insn.words[2] & ~0xffU) != 0x001ee100 || (address & 1) || address >= 246 ||
                offset < -0x80000 || offset > 0x7ffff)
                return fail(index, "native global compare-and-swap mode");
            u32 pair = comparator;
            if ((comparator & 1) || value != comparator + 1 || comparator >= 246) {
                emit(0x5c98000000000000ULL | predicate | 250 | (u64{15} << 39) | (u64{comparator} << 20));
                emit(0x5c98000000000000ULL | predicate | 251 | (u64{15} << 39) | (u64{value} << 20));
                pair = 250;
            }
            emit(0xeef0000000000000ULL | predicate | dest | (u64{address} << 8) | (u64{pair} << 20) |
                (u64{static_cast<u32>(offset) & 0xfffff} << 28) | (u64{1} << 48));
            break;
        }
        case 0x9a8: { // ATOMG.E.ADD.STRONG.GPU U32 through a uniform pointer plus an offset.
            const u32 address = uniform_register(insn.Bits(64, 6));
            const s32 offset = static_cast<s32>(insn.Bits(40, 24) << 8) >> 8;
            if (insn.Bits(24, 8) != 255 || offset < -0x80000 || offset > 0x7ffff ||
                (insn.words[2] & ~0x3fU) != 0x081ee140 || address == ~0U || (address & 1))
                return fail(index, "native global atomic mode");
            emit(0xed00000000000000ULL | predicate | dest | (u64{address} << 8) |
                (u64{insn.Bits(32, 8)} << 20) | (u64{static_cast<u32>(offset) & 0xfffff} << 28) |
                (u64{1} << 48));
            break;
        }
        case 0xb97: { // SULD.P.1D/2D/2D_ARRAY/3D.IGN, direct descriptor, channel mask at 72-75.
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            const u32 coordinate = insn.Bits(24, 8);
            const u32 channels = insn.Bits(72, 4);
            // Maxwell's codes: 0 = 1D, 3 = 2D, 4 = 2D_ARRAY, 5 = 3D. (hookshot) 1D, one coordinate register: 17 of
            // OoT's compute programs stopped at it (1D_BUFFER and 1D_ARRAY, 1 and 2, are not seen and stay refused)
            const u32 dimension = insn.Bits(61, 3);
            if ((insn.words[2] & ~0xf00U) != 0x001e0000 || channels == 0 || (dimension != 0 && dimension < 3) ||
                dimension > 5 || binding >= 18 || offset > 0x1fff || insn.Bits(59, 2) || dest == 255 ||
                coordinate > (dimension >= 4 ? 252U : dimension == 0 ? 254U : 253U))
                return fail(index, "native formatted surface load mode");
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return fail(index, "native surface descriptor buffer conflict");
            result.required_texture_buffer = binding;
            emit(base(0xeb00000000000000ULL) | (u64{coordinate} << 8) |
                (u64{channels} << 20) | (u64{dimension} << 33) | (u64{offset} << 36) | (u64{1} << 51));
            break;
        }
        case 0xb9b: { // SUST.P.2D/3D.IGN, direct descriptor and all four channels.
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            const u32 data = insn.Bits(32, 8), coordinate = insn.Bits(24, 8);
            const u32 dimension = insn.Bits(61, 3);
            const bool strong = insn.words[2] == 0x0010af00; // .STRONG.GPU: fenced on both sides
            if ((insn.words[2] != 0x00100f00 && !strong) || dimension < 3 || dimension > 5 ||
                binding >= 18 || offset > 0x1fff || insn.Bits(59, 2) || data > 252 || coordinate > (dimension >= 4 ? 252U : 253U))
                return fail(index, "native formatted surface store mode");
            // A second descriptor buffer: SUST.B with the handle, loaded from that buffer, in R250.
            const bool bindless = !bind_texture_buffer(binding);
            if (bindless) {
                emit(0x4c98000000070000ULL | 250 | (u64{15} << 39) | (u64{offset} << 20) | (u64{binding} << 34));
            }
            if (strong) emit(0xef98000000000000ULL | predicate | (u64{1} << 8)); // MEMBAR.GL
            emit(0xeb20000000000000ULL | predicate | data | (u64{coordinate} << 8) |
                (u64{15} << 20) | (u64{dimension} << 33) |
                (bindless ? u64{250} << 39 : (u64{offset} << 36) | (u64{1} << 51)));
            if (strong) emit(0xef98000000000000ULL | predicate | (u64{1} << 8));
            break;
        }
        case 0x593: { // SUATOM.D.1D.ADD.U32.STRONG.GPU.IGN, direct descriptor, no fault predicate.
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            if (insn.words[2] != 0x001ee000 || insn.Bits(59, 5) || binding >= 18 || offset > 0x1fff ||
                (insn.words[3] & 0xc00001ffU))
                return fail(index, "native direct surface atomic mode");
            if (!bind_texture_buffer(binding))
                return fail(index, "native surface descriptor buffer conflict");
            // Strong(GPU) is the atomic's coherence/scope, not acquire/release. The existing image
            // atomic backend uses Device/Relaxed; explicit native MEMBARs are lowered separately.
            emit(base(0xea00000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                (u64{insn.Bits(32, 8)} << 20) | (u64{offset} << 36));
            break;
        }
        case 0x59f: { // SURED.D.2D.<op>.STRONG.GPU.IGN U32, direct descriptor (title 1's race compute).
            const u32 binding = insn.Bits(54, 5), offset = insn.Bits(40, 14);
            const u32 data = insn.Bits(32, 8), coordinate = insn.Bits(24, 8);
            // (hookshot) bits 87-90: ADD MIN MAX INC DEC AND OR XOR, as Maxwell SURED's 24-26 (OoT's TAAU ORs
            // its tile flags)
            const u32 reduction = insn.Bits(87, 4);
            if ((insn.words[2] & ~(0xfU << 23)) != 0x0010e000 || reduction > 7 || insn.Bits(61, 3) != 3 || binding >= 18 ||
                offset > 0x1fff || insn.Bits(59, 2) || coordinate > 253)
                return fail(index, "native surface reduction mode");
            if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                return fail(index, "native surface descriptor buffer conflict");
            result.required_texture_buffer = binding;
            // Maxwell SURED (as the frontend decodes it): operand 0-7, coordinates 8-15, ADD and
            // U32 are zero at 24 and 20, type 33 (3 = 2D), bound offset 36, bound 51.
            emit(0xeb40000000000000ULL | predicate | data | (u64{coordinate} << 8) | (u64{reduction} << 24) |
                (u64{3} << 33) | (u64{offset} << 36) | (u64{1} << 51));
            break;
        }
        case 0x243: { // (hookshot) F2IP.U8.F32 Rd, Ra, Rb, Rc: {Rc[15:0], sat_u8(Ra), sat_u8(Rb)} (PTX cvt.pack.sat.u8)
            // Bits 78-79 round as I2FP's (RN RM RP RZ, Maxwell F2I's 39-40). OoT packs colours scaled by 255 so.
            if ((insn.words[2] & ~(0xffU | (3U << 14))) != 0x400 || (insn.words[1] & ~0xffU))
                return fail(index, "native float-to-packed-integer mode");
            const u32 a = insn.Bits(24, 8), b = insn.Bits(32, 8), c = insn.Bits(64, 8), rounding = insn.Bits(78, 2);
            if ((a != 255 && a >= 246) || (b != 255 && b >= 246) || (c != 255 && c >= 246))
                return fail(index, "native float-to-packed-integer registers");
            // max(x, 0) (a NaN gives 0), min(x, 255), then F2I.U32
            const auto byte_of = [&](u32 source, u32 target) {
                if (source == 255) { mov_imm(target, 0); return; }
                mov_imm(252, 0);
                emit(0x5c60000000000000ULL | predicate | target | (u64{source} << 8) | (u64{252} << 20) | (u64{15} << 39));
                mov_imm(252, 0x437f0000);
                emit(0x5c60000000000000ULL | predicate | target | (u64{target} << 8) | (u64{252} << 20) | (u64{7} << 39));
                emit(0x5cb0000000000000ULL | predicate | target | (u64{2} << 8) | (u64{2} << 10) | (u64{target} << 20) |
                     (u64{rounding} << 39));
            };
            byte_of(a, 250);
            byte_of(b, 251);
            emit(0x3848000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{8} << 20));      // a << 8
            if (c != 255) {
                emit(0x5c40000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{251} << 20) | (u64{1} << 41) |
                     (u64{7} << 48));
                emit(0x3848000000000000ULL | predicate | 251 | (u64{c} << 8) | (u64{16} << 20));    // Rc << 16
            }
            emit(base(0x5c40000000000000ULL) | (u64{250} << 8) | (u64{251} << 20) | (u64{1} << 41) | (u64{7} << 48));
            break;
        }
        case 0x301: // BREV register.
            if (insn.words[2] || insn.words[1] > 255) return fail(index, "native bit reverse mode");
            emit(base(0x3828000000000000ULL) | (u64{insn.words[1]} << 8) |
                (u64{1} << 39) | (u64{1} << 40));
            break;
        case 0x219: case 0xa19: case 0xc19: { // U32 register (or cbuf, 0xa19, or uniform, 0xc19) shifts with a zero word outside the selected result.
            const u32 low = insn.Bits(24, 8), high = insn.Bits(64, 8);
            const bool constant = opcode == 0xa19;
            // A constant shift amount is loaded into R251 once the form is known.
            const u32 shift = opcode == 0xc19 ? uniform_register(insn.Bits(32, 8)) : constant ? 251U : insn.Bits(32, 8);
            const bool shift_operand = constant ? valid_cbuf() && !insn.Bits(62, 2) : insn.words[1] <= 255;
            const auto load_shift = [&] { if (constant) materialize(2, 0, 251, false, false); };
            const bool right = insn.Bits(76, 1) != 0, high_result = insn.Bits(80, 1) != 0;
            // SHF.R.U32 of the low word with a zero high word: a plain logical right shift.
            if (right && !high_result && high == 255 && insn.Bits(73, 2) == 3 && shift != ~0U &&
                shift_operand &&
                (insn.words[2] & ~(0x000118ffU | (opcode == 0xc19 ? 0x08000000U : 0U))) == 0x00000600U) {
                load_shift();
                emit(base(0x5c28000000000000ULL) | (u64{low} << 8) | (u64{shift} << 20));
                break;
            }
            if (shift_operand && shift != ~0U &&
                !(insn.words[2] & ~(0x00011effU | (opcode == 0xc19 ? 0x08000000U : 0U))) &&
                (insn.Bits(73, 2) != 3 || (insn.words[2] & 0x800U) ||
                 (right ? (!high_result || low != 255) : (high_result || high != 255)))) {
                load_shift();   // (hookshot) any type, both words, wrap: the general funnel shift
                funnel(static_cast<u32>(dest), low, high, true, shift, insn.Bits(73, 2), right, high_result,
                       insn.Bits(75, 1) != 0);
                break;
            }
            if (!shift_operand || insn.Bits(73, 2) != 3 || shift == ~0U ||
                (insn.words[2] & ~(0x000118ffU | (opcode == 0xc19 ? 0x08000000U : 0U))) != 0x00000600U ||
                (right ? (!high_result || low != 255) : (high_result || high != 255)))
                return fail(index, "native register shift mode");
            load_shift();
            emit(base(right ? 0x5cf8000000000000ULL : 0x5bf8000000000000ULL) |
                (u64{right ? high : 255U} << 8) | (u64{shift} << 20) |
                (u64{right ? 255U : low} << 39) | (u64{insn.Bits(75, 1)} << 50));
            break;
        }
        case 0x300: case 0xd00: { // FLO, including a uniform source.
            if ((insn.words[2] & ~(0x600U | (opcode == 0xd00 ? 0x08000000U : 0U))) != 0x000e0000U)
                return fail(index, "native leading bit mode");
            const u32 source = operand(1, false);
            emit(base(0x5c30000000000000ULL) | (u64{source} << 20) |
                (u64{insn.Bits(63, 1)} << 40) | (u64{insn.Bits(74, 1)} << 41) |
                (u64{insn.Bits(73, 1)} << 48));
            break;
        }
        case 0x309: case 0xd09: { // POPC, including a uniform source.
            if (insn.words[2] != (opcode == 0xd09 ? 0x08000000U : 0U))
                return fail(index, "native population count mode");
            const u32 source = operand(1, false);
            emit(base(0x5c08000000000000ULL) | (u64{source} << 20) | (u64{insn.Bits(63, 1)} << 40));
            break;
        }
        case 0x918:
            if (insn.words[1] || insn.words[2]) return fail(index, "native NOP mode");
            emit(0x50b0000000000f00ULL | predicate);
            break;
        case 0x992: { // MEMBAR.SC: preserve workgroup/device visibility.
            const u32 scope = insn.Bits(76, 3);
            if (insn.words[1] || insn.words[2] != (scope << 12) ||
                (scope != 0 && scope != 2 && scope != 3 && scope != 5))
                return fail(index, "native memory barrier scope");
            const u32 legacy_scope = scope == 0 ? 0 : scope == 2 ? 1 : scope == 3 ? 2 : 3;
            emit(0xef98000000000000ULL | predicate | (u64{legacy_scope} << 8));
            break;
        }
        case 0x9ab: // ERRBAR: wait for prior memory operations on the host backend.
            if (insn.words[1] || insn.words[2]) return fail(index, "native error barrier mode");
            emit(0xef98000000000000ULL | predicate | (u64{1} << 8));
            break;
        case 0xb1d: // BAR.SYNC 0 (bit 80: .DEFER_BLOCKING), full workgroup.
            if (insn.words[1] || (insn.words[2] & ~0x10000U) || native_predicate != 7)
                return fail(index, "native workgroup barrier mode");
            shared_store_pending = false;
            emit(0xf0a8000000000000ULL | predicate | (u64{1} << 43) |
                (u64{1} << 44) | (u64{1} << 39) | (u64{7} << 39));
            break;
        case 0x388: case 0x984: { // Shared memory load/store with scaled register offsets.
            const u32 width = insn.Bits(73, 3), stride = insn.Bits(78, 2);
            // LDS [Ra.Xn + URb + offset]: bit 91 adds a uniform register (bits 32-39).
            const bool uniform_base = opcode == 0x984 && insn.Bits(91, 1);
            const u32 base_register = uniform_base ? uniform_register(insn.Bits(32, 8)) : 255U;
            if (width > 6 || (insn.words[2] & ~(uniform_base ? 0x08000000U : 0U)) != ((width << 9) | (stride << 14)) ||
                (opcode == 0x984 && !uniform_base && insn.Bits(32, 8)) || base_register == ~0U)
                return fail(index, "native shared memory addressing mode");
            u32 address = insn.Bits(24, 8);
            if (stride && address != 255) {
                emit(0x3848000000000000ULL | predicate | 250 |
                    (u64{address} << 8) | (u64{stride + 1} << 20));
                address = 250;
            }
            if (base_register != 255) {
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{address} << 8) | (u64{base_register} << 20));
                address = 250;
            }
            const u32 data = opcode == 0x388 ? insn.Bits(32, 8) : static_cast<u32>(dest);
            // (hookshot) A warp stores to shared memory (STS) and reads it back (LDS) with no barrier between: the Switch runs a
            // warp's shared memory accesses in order, Vulkan orders another lane's store and this lane's load only through a
            // barrier, and the driver's compiler may move them past each other (OoT's 4/3 gameplay network on NVIDIA's cooperative
            // matrix path: 16x2 bright bars, Hookshot issue 2455). As for LDSM: a subgroup barrier before the first LDS after an
            // STS, only where the LDS is not predicated (every lane of the warp reaches it). RX_VK_SHARED_NO_LDS_BARRIER=1: none, as before
            static const bool lds_barrier = !env_on("RX_VK_SHARED_NO_LDS_BARRIER");
            if (opcode == 0x388) {
                shared_store_pending = true;
            } else if (shared_store_pending && lds_barrier && native_predicate == 7) {
                emit(0xef98000000000000ULL | predicate | (u64{1} << 20));   // (MEMBAR, bit 20: barrier_operations.cpp)
                shared_store_pending = false;
            }
            emit((opcode == 0x388 ? 0xef58000000000000ULL : 0xef48000000000000ULL) |
                predicate | data | (u64{address} << 8) | (u64{insn.Bits(40, 24)} << 20) |
                (u64{width} << 48));
            break;
        }
        case 0x23a: { // MOVM.16.MT88: transpose an 8x8 16-bit matrix held one row pair per lane.
            // Lane t holds row t/4, columns 2(t%4)..+1; the result is the transposed matrix in the
            // same layout, so each half comes from lane 8(t%4) + t/8 (+4) at half (t/4)%2.
            const u32 source = insn.Bits(24, 8);
            if (insn.words[1] || insn.words[2] || source >= 246 || dest >= 246)
                return fail(index, "native matrix move mode");
            emit(0xf0c8000000000000ULL | predicate | 246);                                        // laneid
            emit(0x0400000000000000ULL | predicate | 249 | (u64{246} << 8) | (u64{3} << 20));
            emit(0x3848000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{3} << 20));     // 8(t%4)
            emit(0x3828000000000000ULL | predicate | 247 | (u64{246} << 8) | (u64{3} << 20));     // t/8
            emit(0x5c10000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{247} << 20));
            emit(0xef10000000000000ULL | predicate | 247 | (u64{source} << 8) | (u64{249} << 20) |
                 (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            emit(0x1c00000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{4} << 20));
            emit(0xef10000000000000ULL | predicate | 248 | (u64{source} << 8) | (u64{249} << 20) |
                 (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            emit(0x3828000000000000ULL | predicate | 249 | (u64{246} << 8) | (u64{2} << 20));
            emit(0x0400000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{1} << 20));
            emit(0x3848000000000000ULL | predicate | 249 | (u64{249} << 8) | (u64{4} << 20));     // 0 or 16
            emit(0x5c28000000000000ULL | predicate | 247 | (u64{247} << 8) | (u64{249} << 20));
            emit(0x0400000000000000ULL | predicate | 247 | (u64{247} << 8) | (u64{0xffff} << 20));
            emit(0x5c28000000000000ULL | predicate | 248 | (u64{248} << 8) | (u64{249} << 20));
            emit(0x5c18000000000000ULL | predicate | dest | (u64{248} << 8) | (u64{247} << 20) | (u64{16} << 39));
            break;
        }
        case 0x23c: { // HMMA.16816.F16: D(16x8) = A(16x16) * B(16x8) + C, f16 fragments.
            // Lane t (group g = t/4, i = t%4): A regs hold rows g / g+8 at k 2i..2i+1 (a0, a1) and
            // 8+2i.. (a2, a3); B regs hold k 2i.. (b0) / 8+2i.. (b1) of column g; D holds rows
            // g / g+8 at columns 2i, 2i+1. Rebuilt from shuffles with HFMA2, accumulating in R252/R253.
            // The expansion is correct but large: title 4's 372-HMMA network takes the host driver
            // minutes to compile, so it stays opt-in (the QMD still completes when rejected).
            // EDEN_NX2_TENSOR=stub keeps the rest of the program but makes every HMMA D = C.
            static const char* const mode = std::getenv("EDEN_NX2_TENSOR");
            const u32 a = insn.Bits(24, 8), b = insn.Bits(32, 8), c = insn.Bits(64, 8);
            if ((insn.words[1] & ~0xffU) || (insn.words[2] & ~0xffU) != 0x800 || dest + 2 > 246 ||
                a + 4 > 246 || b + 2 > 246 || (c != 255 && c + 2 > 246))
                return fail(index, "native matrix multiply mode");
            // HMMA_nx2: dest 0-7, A 8-15, B 20-27, C 39-46 (255: zero), the predicate as usual
            if (native_matrix_multiply && !(mode && *mode)) {
                emit(0xefc8000000000000ULL | predicate | dest | (u64{a} << 8) | (u64{b} << 20) | (u64{c} << 39));
                break;
            }
            if (!mode) return fail(index, "native matrix multiply (set EDEN_NX2_TENSOR to emulate)");
            if (std::string_view{mode} == "stub") {
                for (u32 row = 0; row < 2; ++row) {
                    if (c == 255) mov_imm(static_cast<u32>(dest) + row, 0);
                    else if (c != dest) emit(0x5c98000000000000ULL | predicate | (dest + row) | (u64{15} << 39) | (u64{c + row} << 20));
                }
                break;
            }
            const auto shuffle = [&](u32 target, u32 source_reg) {
                emit(0xef10000000000000ULL | predicate | target | (u64{source_reg} << 8) | (u64{246} << 20) |
                     (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
            };
            const auto lane_a = [&](u32 i) { // 4g + i
                emit(0xf0c8000000000000ULL | predicate | 246);
                emit(0x0400000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{0x1c} << 20));
                if (i) emit(0x1c00000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{i} << 20));
            };
            const auto lane_b = [&](u32 add) { // 8(t%4) + add: column 2(t%4) (+1 when add >= 4)
                emit(0xf0c8000000000000ULL | predicate | 246);
                emit(0x0400000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{3} << 20));
                emit(0x3848000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{3} << 20));
                if (add) emit(0x1c00000000000000ULL | predicate | 246 | (u64{246} << 8) | (u64{add} << 20));
            };
            const auto hfma2 = [&](u32 accumulator, u32 swizzle_a, u32 pair) {
                emit(0x5d00000000000000ULL | predicate | accumulator | (u64{251} << 8) | (u64{pair} << 20) |
                     (u64{accumulator} << 39) | (u64{swizzle_a} << 47));
            };
            for (u32 row = 0; row < 2; ++row) {
                if (c == 255) mov_imm(252 + row, 0);
                else emit(0x5c98000000000000ULL | predicate | (252 + row) | (u64{15} << 39) | (u64{c + row} << 20));
            }
            for (u32 i = 0; i < 4; ++i) {
                for (u32 k_block = 0; k_block < 2; ++k_block) {
                    lane_b(i);
                    shuffle(247, b + k_block);
                    lane_b(4 + i);
                    shuffle(248, b + k_block);
                    // P0 = (B[2i][2t], B[2i][2t+1]), P1 = (B[2i+1][2t], B[2i+1][2t+1])
                    emit(0x0400000000000000ULL | predicate | 249 | (u64{247} << 8) | (u64{0xffff} << 20));
                    emit(0x5c18000000000000ULL | predicate | 249 | (u64{248} << 8) | (u64{249} << 20) | (u64{16} << 39));
                    emit(0x3828000000000000ULL | predicate | 250 | (u64{247} << 8) | (u64{16} << 20));
                    emit(0x0400000000000000ULL | predicate | 248 | (u64{248} << 8) | (u64{0xffff0000} << 20));
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{248} << 20));
                    for (u32 row = 0; row < 2; ++row) {
                        lane_a(i);
                        shuffle(251, a + k_block * 2 + row);
                        hfma2(252 + row, 2, 249); // A.H0 broadcast * P0
                        hfma2(252 + row, 3, 250); // A.H1 broadcast * P1
                    }
                }
            }
            for (u32 row = 0; row < 2; ++row)
                emit(0x5c98000000000000ULL | predicate | (dest + row) | (u64{15} << 39) | (u64{252 + row} << 20));
            break;
        }
        case 0x83b: { // LDSM.16.M88.{1,2,4} (ldmatrix): 8x8 16-bit matrices from shared memory.
            // Lanes 8m..8m+7 give the row addresses of matrix m; lane t receives the 32 bits at
            // row t/4, byte (t%4)*4 of each matrix in Rd+m. Emulated with SHFL.IDX + LDS.32.
            const u32 count = 1U << insn.Bits(72, 2);
            const bool transpose = insn.Bits(78, 1) != 0;
            if (insn.Bits(72, 2) == 3 || (insn.words[2] & ~0x4300U) || insn.Bits(32, 8) ||
                dest + count > 246)
                return fail(index, "native matrix load mode");
            u32 address = insn.Bits(24, 8);
            const u64 offset = insn.Bits(40, 24);
            // (Fritchii f130f0c50) An address the results overwrite is copied first, to R254: the paths below use R246-R253,
            // and R253 (the copy's old place) is where each matrix's row address is shuffled to, so matrices 1-3 of an .x2/.x4
            // shuffled matrix 0's address and read the wrong rows (OoT has one such LDSM.x4, gpu/vk/tests/ldsm_address_test.cpp)
            if (address != 255 && address >= dest && address < dest + count) {
                emit(0x5c10000000000000ULL | predicate | 254 | (u64{address} << 8) | (u64{255} << 20));
                address = 254;
            }
            // (hookshot) A warp stores its tiles (STS) and reads them back with LDSM, often with no BAR between: the Switch runs a
            // warp's shared memory accesses in order, Vulkan orders another lane's store and this lane's load only through a
            // barrier. A subgroup barrier before the reads (the stores have landed) and after them (no lane's next store reaches
            // the rows before every lane has read them); LDSM is warp-collective, so every lane reaches both. nxnative's finding
            // (seedds/nxnative, MIT: "LDSM reads rows other lanes stored", white blotches through agl's upscaler).
            // RX_VK_MMA_NO_LDSM_BARRIER=1: none, as before
            static const bool ldsm_barrier = !env_on("RX_VK_MMA_NO_LDSM_BARRIER");
            const auto subgroup_barrier = [&] {
                if (ldsm_barrier) emit(0xef98000000000000ULL | predicate | (u64{1} << 20));   // (MEMBAR, bit 20: barrier_operations.cpp)
            };
            subgroup_barrier();
            if (transpose) {
                // .MT88: lane t receives stored rows 2(t%4) and 2(t%4)+1 at column t/4 (16 bits each).
                emit(0xf0c8000000000000ULL | predicate | 246);                                    // laneid
                emit(0x0400000000000000ULL | predicate | 247 | (u64{246} << 8) | (u64{3} << 20));
                emit(0x3848000000000000ULL | predicate | 247 | (u64{247} << 8) | (u64{1} << 20)); // row 2(t%4)
                emit(0x3828000000000000ULL | predicate | 248 | (u64{246} << 8) | (u64{2} << 20));
                emit(0x3848000000000000ULL | predicate | 248 | (u64{248} << 8) | (u64{1} << 20)); // byte 2(t/4)
                for (u32 matrix = 0; matrix < count; ++matrix) {
                    for (u32 half = 0; half < 2; ++half) {
                        emit(0x1c00000000000000ULL | predicate | 249 | (u64{247} << 8) |
                             (u64{matrix * 8 + half} << 20));
                        emit(0xef10000000000000ULL | predicate | 250 | (u64{address} << 8) | (u64{249} << 20) |
                             (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
                        emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{248} << 20));
                        emit(0xef48000000000000ULL | predicate | (251 + half) | (u64{250} << 8) |
                             (offset << 20) | (u64{2} << 48));                                  // LDS.U16
                    }
                    emit(0x5c18000000000000ULL | predicate | (dest + matrix) | (u64{252} << 8) |
                         (u64{251} << 20) | (u64{16} << 39));
                }
                subgroup_barrier();
                break;
            }
            emit(0xf0c8000000000000ULL | predicate | 250);                                    // S2R laneid
            emit(0x3828000000000000ULL | predicate | 251 | (u64{250} << 8) | (u64{2} << 20)); // row
            emit(0x0400000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{3} << 20));
            emit(0x3848000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{2} << 20)); // column bytes
            for (u32 matrix = 0; matrix < count; ++matrix) {
                u32 lane = 251;
                if (matrix) {
                    emit(0x1c00000000000000ULL | predicate | 252 | (u64{251} << 8) | (u64{matrix * 8} << 20));
                    lane = 252;
                }
                emit(0xef10000000000000ULL | predicate | 253 | (u64{address} << 8) | (u64{lane} << 20) |
                     (u64{1} << 29) | (u64{31} << 34) | (u64{7} << 48));
                emit(0x5c10000000000000ULL | predicate | 253 | (u64{253} << 8) | (u64{250} << 20));
                emit(0xef48000000000000ULL | predicate | (dest + matrix) | (u64{253} << 8) |
                     (offset << 20) | (u64{4} << 48));
            }
            subgroup_barrier();
            break;
        }
        case 0x919: case 0x9c3: { // S2R / S2UR: special registers the Maxwell S2R handles.
            // Same numbering as Maxwell: lane id (0), invocation id/info, Y direction, thread kill
            // (17-19, 29), W scale factors (30-31, Kirby Air Riders), thread and workgroup ids,
            // lane masks (56-60).
            const u32 special = insn.Bits(72, 8);
            if (special == 132 && !insn.words[1] && insn.words[2] == (special << 8)) {
                // SR_VARIABLE_RATE: without variable-rate shading every fragment is 1x1, its width
                // and height in bytes 1 and 2 (Nintendo Switch Sports Resort multiplies them).
                const u32 target = opcode == 0x9c3 ? uniform_register(static_cast<u32>(dest)) : static_cast<u32>(dest);
                if (target == ~0U) return fail(index, "native uniform special destination");
                mov_imm(target, 0x00010100);
                break;
            }
            if (insn.words[1] || insn.words[2] != (special << 8) ||
                (special != 0 && (special < 17 || special > 19) && (special < 29 || special > 35) &&
                 special != 37 && special != 38 && special != 39 &&
                 special != 56 && special != 57 && special != 58 && special != 59 && special != 60))
                return fail(index, "native special register");
            const u32 target = opcode == 0x9c3 ? uniform_register(static_cast<u32>(dest)) : static_cast<u32>(dest);
            if (target == ~0U) return fail(index, "native uniform special destination");
            emit(0xf0c8000000000000ULL | predicate | target | (u64{special} << 20));
            break;
        }
        case 0x28c: case 0x88c: { // UISETP, signed (bit 73) or unsigned, with UPT accumulation.
            const u32 output = insn.Bits(81, 3);
            const u32 a = uniform_register(insn.Bits(24, 8)), b = opcode == 0x88c ? 252U : uniform_register(insn.Bits(32, 8));
            // (hookshot) a UPn guard: R239 kept in R249 first; where the guard (read from that copy: the compare may
            // write the very UPn that guards it) is false, R239 gets the copy back
            const u32 guard = insn.Bits(12, 4);
            const bool guarded_compare = guard != 7 && (guard & 7) != 7;
            // (hookshot) an accumulator UPn (bits 87-90) and boolean op (74-75): loaded from R239 into its own slot
            // P(6-n) (the result's, when it is the output) and given to ISETP as its combining input
            const u32 accumulator = insn.Bits(87, 4), bop = insn.Bits(74, 2);
            const u32 acc_slot = (accumulator & 7) == 7 ? 7U : 6 - (accumulator & 7);
            if (output > 6 || insn.Bits(84, 3) != 7 || bop > 2 ||
                (insn.Bits(72, 4) & ~0xeU) || (guard != 7 && !guarded_compare) || a == ~0U || b == ~0U)
                return fail(index, "native uniform comparison mode");
            const u32 temporary = 6 - output, mask = (1U << temporary) | (acc_slot < 7 ? 1U << acc_slot : 0U);
            if (guarded_compare) emit(0x5c98000000070000ULL | 249 | (u64{239} << 20) | (u64{15} << 39));
            if (opcode == 0x88c) emit(0x0100000000000000ULL | (u64{7} << 16) | 252 | (u64{15} << 12) | (u64{insn.words[1]} << 20));   // (hookshot, from Lichii: unguarded, its guard is a uniform predicate)
            emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
            if (acc_slot < 7) emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{1U << acc_slot} << 20));
            emit(0x5b60000000070000ULL | 7 | (u64{temporary} << 3) |
                (u64{a} << 8) | (u64{b} << 20) | (u64{acc_slot} << 39) | (u64{(accumulator >> 3) & 1} << 42) |
                (u64{bop} << 45) | (u64{insn.Bits(73, 1)} << 48) | (u64{insn.Bits(76, 3)} << 49));
            emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{1U << temporary} << 20));
            emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
            if (guarded_compare) {
                const u32 slot = 6 - (guard & 7), guard_bit = 1U << slot;
                emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{guard_bit} << 20));
                emit(0x38f0000000070000ULL | (u64{249} << 8) | (u64{guard_bit} << 20));
                emit(0x5ca0000000070000ULL | 239 | (u64{239} << 8) | (u64{249} << 20) | (u64{slot} << 39) |
                     (u64{(guard >> 3) & 1} << 42));
                emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{guard_bit} << 20));
            }
            break;
        }
        case 0x287: case 0x887: { // USEL, UP0/UP1; preserve ordinary predicates.
            const u32 condition = insn.Bits(87, 3);
            const u32 target = uniform_register(static_cast<u32>(dest));
            const u32 a = uniform_register(insn.Bits(24, 8)), b = opcode == 0x887 ? 252U : uniform_register(insn.Bits(32, 8));
            // (hookshot) a UPn guard (bits 12-15): the select into R249, then kept only where the guard holds
            const u32 guard = insn.Bits(12, 4);
            const bool guarded_select = guard != 7 && (guard & 7) != 7;
            if (condition > 6 || target == ~0U || a == ~0U || b == ~0U || (guard != 7 && !guarded_select) ||
                (insn.words[2] & ~0x07800000U) != 0x08000000U)
                return fail(index, "native uniform select mode");
            // The clamp's UISETP may sit a few instructions back, with only instructions between
            // that write no uniform state and do not branch (Nintendo Switch Sports' jump tables),
            // and no branch seen so far landing between them.
            u32 compare = index;
            for (u32 back = 1; back <= 8 && back <= index; ++back) {
                const u32 earlier = code[index - back].Bits(0, 12), datapath = earlier & 0x1ff;
                if (earlier == 0x88c) {
                    compare = index - back;
                    break;
                }
                if ((datapath >= 0x080 && datapath <= 0x0bf) || (datapath >= 0x1c2 && datapath <= 0x1c4) ||
                    (earlier >= 0x940 && earlier <= 0x95f))
                    break;
            }
            const bool target_between = std::any_of(relocations.begin(), relocations.end(),
                [&](const Relocation& relocation) { return relocation.target > compare && relocation.target <= index; });
            if (!guarded_select && opcode == 0x887 && compare < index && !target_between && condition == 0 && insn.words[1] <= 0x7ffff &&
                code[compare].Bits(12, 4) == 7 && code[compare].Bits(24, 8) == insn.Bits(24, 8) &&
                code[compare].words[1] == insn.words[1] && code[compare].words[2] == 0x0bf01070 &&
                insn.words[2] == 0x08000000) {
                // Preserve the unsigned clamp idiom for indirect-table analysis.
                emit(0x3820000000070000ULL | target | (u64{a} << 8) |
                     (u64{insn.words[1]} << 20) | (u64{7} << 39));
                break;
            }
            if (opcode == 0x887) emit(0x0100000000000000ULL | (u64{7} << 16) | 252 | (u64{15} << 12) | (u64{insn.words[1]} << 20));   // (hookshot, from Lichii)
            const u32 temporary = 6 - condition, mask = 1U << temporary;
            emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20));
            emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{mask} << 20));
            emit(0x5ca0000000070000ULL | (guarded_select ? 249U : target) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{temporary} << 39) | (u64{insn.Bits(90, 1)} << 42));
            emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20));
            if (guarded_select) {
                const u32 slot = 6 - (guard & 7), guard_bit = 1U << slot;
                emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{guard_bit} << 20));
                emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{guard_bit} << 20));
                emit(0x5ca0000000070000ULL | target | (u64{249} << 8) | (u64{target} << 20) | (u64{slot} << 39) |
                     (u64{(guard >> 3) & 1} << 42));
                emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{guard_bit} << 20));
            }
            break;
        }
        case 0x381: case 0x981: { // LDG.E of 8 to 128 bits, register or uniform 64-bit address.
            // Sizes as Maxwell's: U8, S8, U16, S16, 32, 64, 128.
            const u32 size = insn.Bits(73, 3);
            u32 address = insn.Bits(24, 8);
            // (hookshot) bits 77-79 all set: .STRONG.GPU, as the atomics in this file carry it (RED, ATOMG). A Maxwell LDG has
            // no ordering: fenced on both sides with MEMBAR.GL, as SUST.STRONG.GPU (0xb9b) is (hckrman101, PR 1778)
            const bool strong = opcode == 0x981 && insn.Bits(78, 1) != 0;
            if (size > 6 || (opcode == 0x381 && insn.Bits(32, 8)) ||
                (size == 5 && (dest & 1)) || (size == 6 && (dest & 3)))
                return fail(index, "native global load mode");
            if (opcode == 0x381) {
                // Bit 77 (strong/scoped ordering) needs nothing extra from a Maxwell LDG.
                if ((insn.words[2] & ~0x2e00U) != 0x001e8100 || (address & 1) || address >= 246)
                    return fail(index, "native global load mode");
            } else {
                // The uniform base sits where a register source would (bits 32-37).
                const u32 uniform_address =
                    insn.Bits(38, 2) ? ~0U : uniform_register(insn.Bits(32, 6));
                // desc[URn][Ra.64]: the uniform pair is a memory (cache policy) descriptor and
                // Ra the whole address.
                const bool descriptor_address = (insn.words[2] & ~(0x3fU | 0xe00U)) == 0x0c1e1100;
                // (hookshot) bits 77-78, the load's memory ordering, as the register form's bit 77: nothing for a Maxwell
                // LDG. Two of OoT's compute programs load [URn+imm] with both set and stopped here
                if ((!descriptor_address &&
                     (insn.words[2] & ~(0x04000000U | 0x3fU | 0xe00U | 0x6000U)) != 0x081e8100) ||
                    uniform_address == ~0U || (uniform_address & 1))
                    return fail(index, "native global load uniform addressing mode");
                if (descriptor_address) {
                    if ((address & 1) || address >= 246)
                        return fail(index, "native global load register address pair");
                } else if (address == 255) {
                    address = uniform_address;
                } else {
                    if (insn.Bits(90, 1) && ((address & 1) || address >= 246))
                        return fail(index, "native global load register address pair");
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{uniform_address} << 8) |
                         (u64{address} << 20) | (u64{1} << 47));
                    emit(0x5c10000000000000ULL | predicate | 251 | (u64{uniform_address + 1} << 8) |
                         (u64{insn.Bits(90, 1) ? address + 1 : 255} << 20) | (u64{1} << 43));
                    address = 250;
                }
            }
            if (strong) emit(0xef98000000000000ULL | predicate | (u64{1} << 8));   // MEMBAR.GL
            emit(base(0xeed0000000000000ULL) | (u64{address} << 8) |
                 (u64{insn.Bits(40, 24)} << 20) | (u64{1} << 45) | (u64{size} << 48));
            if (strong) emit(0xef98000000000000ULL | predicate | (u64{1} << 8));
            break;
        }
        case 0x386: case 0x986: { // Global store of 32/64/128 bits, 64-bit address.
            u32 data = insn.Bits(32, 8);
            const u32 size = insn.Bits(73, 3);
            if (size > 6 ||
                (data != 255 && ((size == 5 && (data & 1)) || (size == 6 && (data & 3)) ||
                                 data + (size < 4 ? 1U : 1U << (size - 4)) > 246)))
                return fail(index, "native global store size");
            if (data == 255 && size > 4) {
                // Zeros wider than one register: store from zeroed scratch registers (aligned to the
                // access: R248-R251 for 128 bits, R250-R251 for 64).
                data = size == 6 ? 248 : 250;
                for (u32 i = 0; i < (1U << (size - 4)); ++i) mov_imm(data + i, 0);
            }
            u32 address = insn.Bits(24, 8);
            bool extended = insn.Bits(72, 1) != 0;
            const bool strong = opcode == 0x986 && insn.Bits(78, 1) != 0;   // (.STRONG.GPU: fenced, as the LDG above)
            if (opcode == 0x986) {
                const u32 uniform_address = uniform_register(insn.Bits(64, 6));
                // desc[URn][Ra.64]: the uniform pair is a memory (cache policy) descriptor and
                // Ra the whole address.
                const bool descriptor_address = (insn.words[2] & ~(0x3fU | 0xe00U)) == 0x0c101100;
                // (hookshot) bit 78 beside 77, the store's memory ordering, as for LDG: nothing for a Maxwell STG (the
                // second of those compute programs stores [URn+imm] so)
                if ((!descriptor_address && (insn.words[2] & ~(0x04000000U | 0x3fU | 0xe00U | 0x4000U)) != 0x0810a100) ||
                    uniform_address == ~0U || (uniform_address & 1) || !extended)
                    return fail(index, "native global store uniform addressing mode");
                if (descriptor_address) {
                    if ((address & 1) || address >= 246)
                        return fail(index, "native global store register address pair");
                } else if (address == 255) address = uniform_address;
                else {
                    if (insn.Bits(90, 1) && ((address & 1) || address >= 254))
                        return fail(index, "native global store register address pair");
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{uniform_address} << 8) |
                        (u64{address} << 20) | (u64{1} << 47));
                    emit(0x5c10000000000000ULL | predicate | 251 | (u64{uniform_address + 1} << 8) |
                        (u64{insn.Bits(90, 1) ? address + 1 : 255} << 20) | (u64{1} << 43));
                    address = 250;
                }
            } else if ((insn.words[2] & ~0xe00U) != 0x0010a100) {
                return fail(index, "native global store register addressing mode");
            }
            if (strong) emit(0xef98000000000000ULL | predicate | (u64{1} << 8));   // MEMBAR.GL
            emit(0xeed8000000000000ULL | predicate | data |
                (u64{address} << 8) | (u64{insn.Bits(40, 24)} << 20) |
                (u64{extended} << 45) | (u64{size} << 48));
            if (strong) emit(0xef98000000000000ULL | predicate | (u64{1} << 8));
            break;
        }
        case 0x95b: // Fragment discard; execution predicate is preserved.
            if (insn.words[1] || insn.words[2] != 0x03800000)
                return fail(index, "native fragment discard mode");
            emit(0xe33000000000000fULL | predicate);
            break;
        case 0x311: { // F2I.FTZ.U64.TRUNC, register source, destination pair
            if (insn.words[2] != 0x0021d800 || (insn.words[1] & ~0xffU) || insn.Bits(24, 8) ||
                (dest & 1) || dest >= 246)
                return fail(index, "native unsigned 64-bit float conversion mode");
            emit(base(0x5cb0000000000000ULL) | (u64{3} << 8) | (u64{2} << 10) |
                 (u64{insn.Bits(32, 8)} << 20) | (u64{3} << 39) | (u64{1} << 44));
            break;
        }
        case 0x312: { // I2F.F32.(U|S)64, register pair source; sign (74) and rounding (78-79) as Maxwell's
            if ((insn.words[2] & ~0xc400U) != 0x00301000 || (insn.words[1] & ~0xffU) || insn.Bits(24, 8) ||
                (insn.Bits(32, 8) & 1) || insn.Bits(32, 8) >= 254)
                return fail(index, "native 64-bit integer-to-float mode");
            emit(base(0x5cb8000000000000ULL) | (u64{2} << 8) | (u64{3} << 10) | (u64{insn.Bits(74, 1)} << 13) |
                 (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(78, 2)} << 39));
            break;
        }
        case 0x305: case 0x905: case 0xb05: case 0xd05: { // F2I NTZ, register/immediate/constant-buffer/uniform source
            const bool immediate = opcode == 0x905;
            const u32 mode = insn.words[2] & ~(opcode == 0xd05 ? 0x08000000U : 0U);
            // F16 source: bit 72 signs the result as in the other forms, bits 60-61 pick H0/H1.
            const bool half = opcode == 0x305 && (mode & ~0x100U) == 0x0010f000;
            const bool small = mode == 0x00212800;
            const bool floor = (mode & ~0x10100U) == 0x00207000;
            const bool nearest = (mode & ~0x10100U) == 0x00203000;
            const bool ceil = (mode & ~0x10100U) == 0x0020b000;   // (hookshot) round up: Maxwell's mode 2
            // (hookshot) a negated or absolute register source (bits 63, 62), made first
            const bool source_mod = opcode == 0x305 && !half && (insn.words[1] & 0x3fffff00U) == 0;
            if ((!half && !small && !floor && !nearest && !ceil && (mode & ~0x10100U) != 0x0020f000) ||
                (opcode != 0xb05 && !immediate && !source_mod && (insn.words[1] & (half ? 0xefffff00U : 0xffffff00U))) ||
                (immediate && insn.Bits(24, 8)) ||
                (opcode == 0xd05 && uniform_register(insn.Bits(32, 8)) == ~0U)) {
                return fail(index, "native float-to-integer conversion mode");
            }
            const bool is_signed = insn.Bits(72, 1);
            u32 source = immediate ? materialize(1, 0, 252, false, false)
                       : opcode == 0xb05 ? materialize(2, 0, 252, false, false)
                       : opcode == 0xd05 ? uniform_register(insn.Bits(32, 8)) : insn.Bits(32, 8);
            if (source_mod && insn.Bits(62, 2)) source = materialize(0, insn.Bits(32, 8), 249, insn.Bits(62, 1), insn.Bits(63, 1));
            if (half) {
                extract_half({source, insn.Bits(60, 1) ? 3U : 2U, false, false}, 0, 252);
                source = 252;
            }
            if (!is_signed) {
                // Clear NaNs without consuming a guest predicate. Clamp the
                // conversion below 2^32, retaining an overflow mask for saturation.
                emit(0x5800000000000000ULL | predicate | 250 | (u64{source} << 8) |
                     (u64{source} << 20) | (u64{7} << 39) | (u64{7} << 48));
                emit(0x5c40000000000000ULL | predicate | 250 | (u64{source} << 8) |
                     (u64{250} << 20) | (u64{7} << 48));
                if (!small) {
                mov_imm(252, 0x4f800000);
                emit(0x5800000000000000ULL | predicate | 251 | (u64{250} << 8) |
                     (u64{252} << 20) | (u64{7} << 39) | (u64{6} << 48));
                mov_imm(252, 0x4f7fffff);
                emit(0x5c60000000000000ULL | predicate | 250 | (u64{250} << 8) |
                     (u64{252} << 20) | (u64{7} << 39));
                }
                source = 250;
            }
            emit(base(0x5cb0000000000000ULL) | (u64{small ? 1U : 2U} << 8) | (u64{2} << 10) |
                 (u64{is_signed} << 12) | (u64{source} << 20) |
                 (u64{small || nearest ? 0U : floor ? 1U : ceil ? 2U : 3U} << 39) | (u64{insn.Bits(80, 1)} << 44));
            if (!is_signed && !small) {
                emit(base(0x5c40000000000000ULL) | (dest << 8) | (u64{251} << 20) |
                     (u64{1} << 41) | (u64{7} << 48));
            }
            break;
        }
        case 0xb6f: case 0x370: { // TXQ texture dimensions (0x370: TXQ.B).
            // TXQ.B takes the handle in Ra and the level in Ra+1, as Maxwell's. Up to two outputs
            // go to Rd, Rd+1 directly; more, or a second destination (bits 64-71) for the third and
            // fourth, come out packed in scratch and are copied as for TEX.
            const bool bindless = opcode == 0x370;
            const u32 binding = insn.Bits(54, 5); // descriptor buffer, as for TLD
            const u32 mask = insn.Bits(72, 4), components = static_cast<u32>(std::popcount(mask));
            const u32 second_dest = insn.Bits(64, 8);
            const bool direct = second_dest == 255;
            if (insn.Bits(59, 5) != (bindless ? 1U : 0U) || mask == 0 || (direct && components > 2) ||
                insn.Bits(76, 14) || insn.Bits(91, 1) || (!bindless && binding >= 18) ||
                (bindless && (insn.Bits(32, 27) || insn.Bits(24, 8) >= 245)) ||
                (!direct && (second_dest >= 245 || dest >= 245)) ||
                insn.Bits(40, 14) > 0x1fff || (direct && components == 2 && (dest & 1)) || dest >= 254)
                return fail(index, "native texture dimension query mode");
            if (!bindless) {
                if (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding)
                    return fail(index, "native texture descriptor buffer conflict");
                result.required_texture_buffer = binding;
            }
            emit((bindless ? 0xdf50000000000000ULL : 0xdf48000000000000ULL) | predicate | (direct ? dest : 250) |
                 (u64{insn.Bits(24, 8)} << 8) | (u64{1} << 22) | (u64{mask} << 31) |
                 (u64{bindless ? 0U : insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49));
            for (u32 i = 0; !direct && i < components; ++i) {
                const u32 target = i < 2 ? (dest == 255 ? 255U : static_cast<u32>(dest) + i) : second_dest + (i - 2);
                if (target != 255)
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{250 + i} << 20));
            }
            break;
        }
        case 0xb66: { // TLD.SCR, direct texel fetch at level zero (.LZ) or a given level (.LL)
            const u32 mask = insn.Bits(72, 4);
            const bool level = insn.Bits(87, 3) == 3; // .LL: the level follows the coordinates
            // Bit 76: a texel offset after the coordinates and level. 2D only, as Maxwell TLDS
            // 2D.LZ.AOFFI (x, y in Ra, Ra+1, offsets in Rb) or 2D.LL.AOFFI (level, offsets in Rb, Rb+1).
            const bool offsets = insn.Bits(76, 1) != 0;
            // (hookshot) Bit 78: a multisampled texture's texel (.MS), the sample index in Rb. 2D at level zero only, as
            // Maxwell TLDS 2D.LZ.MS (x, y in Ra, Ra+1, the sample in Rb): three of OoT's fragment programs stopped here
            const bool multisample = insn.Bits(78, 1) != 0;
            const u32 dimension = insn.Bits(61, 3);
            const bool array = dimension == 5;
            const bool volume = dimension == 2;
            u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            const u32 second_dest = insn.Bits(64, 8);
            const bool split = second_dest != 255;
            constexpr std::array<u32, 8> rg_masks{1, 2, 4, 8, 3, 9, 10, 12};
            constexpr std::array<u32, 5> rgba_masks{7, 11, 13, 14, 15};
            u32 swizzle = ~0U;
            if (split) {
                for (u32 i = 0; i < rgba_masks.size(); ++i) if (rgba_masks[i] == mask) swizzle = i;
            } else {
                for (u32 i = 0; i < rg_masks.size(); ++i) if (rg_masks[i] == mask) swizzle = i;
            }
            if (!insn.Bits(60, 1) || (dimension != 0 && dimension != 1 && !array && !volume) ||
                swizzle == ~0U || (insn.Bits(76, 5) & ~5U) ||
                (multisample && (dimension != 1 || level || offsets || (source_a & 1) || source_a >= 254 ||
                                 source_b == 255)) ||
                (offsets && (dimension != 1 || (source_a & 1) || source_a >= 254 || source_b == 255 ||
                             (level && ((source_b & 1) || source_b >= 254)))) ||
                insn.Bits(81, 3) != 7 || (insn.Bits(87, 3) != 1 && !level) || (level && volume) ||
                (level && dimension == 1 && ((source_a & 1) || source_a >= 254)) ||
                (level && array && ((source_b & 1) || source_b >= 254)) ||
                insn.Bits(59, 1) ||
                insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) >= 18 ||
                dest == 255 || (mask & (mask - 1) && ((dest & 1) || dest >= 254)) ||
                (split && mask == 15 && ((second_dest & 1) || second_dest >= 254)) ||
                (array && ((source_a & 1) || source_a >= 249)) ||
                (volume && ((source_a & 1) || source_a >= 254))) {
                return fail(index, "native texel fetch mode or descriptor binding");
            }
            const u32 binding = insn.Bits(54, 5);
            if (!bind_texture_buffer(binding)) {
                if (level || offsets || multisample)
                    return fail(index, "native texture descriptor buffer conflict");
                // A second descriptor buffer: TLD.B (LZ) with the coordinates in R250.. (array
                // index or Z after Ra) and the handle, loaded from that buffer, in R253.
                const auto copy = [&](u32 target, u32 source) {
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{source} << 20));
                };
                copy(250, source_a);
                if (array || volume) copy(251, source_a + 1);
                if (dimension != 0) copy(array || volume ? 252 : 251, source_b);
                emit(0x4c98000000070000ULL | 253 | (u64{15} << 39) | (u64{insn.Bits(40, 14)} << 20) |
                     (u64{binding} << 34));
                emit(0xdd00000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{253} << 20) |
                     (u64{array ? 3U : volume ? 4U : dimension == 0 ? 0U : 2U} << 28) | (u64{mask} << 31) |
                     (u64{insn.Bits(90, 1)} << 49) | (u64{7} << 51));
                const u32 components = static_cast<u32>(std::popcount(mask));
                for (u32 i = 0; i < components; ++i)
                    copy(i < 2 || !split ? static_cast<u32>(dest) + i : second_dest + (i - 2), 250 + i);
                break;
            }
            if (array && level) {
                // TLDS has no array form with a level: TLD takes (layer, x, y) from Ra and the level
                // from Rb (layer, x in Ra, Ra+1; y, level in Rb, Rb+1 here). Results come out packed,
                // then go where the Ampere destinations (Rd, Rd+1, then the second pair) take them.
                for (u32 i = 0; i < 2; ++i) {
                    emit(0x5c98000000000000ULL | predicate | (250 + i) | (u64{15} << 39) | (u64{source_a + i} << 20));
                    emit(0x5c98000000000000ULL | predicate | (252 + i) | (u64{15} << 39) | (u64{source_b + i} << 20));
                }
                emit(0xdc00000000000000ULL | predicate | 246 | (u64{250} << 8) | (u64{253} << 20) |
                     (u64{3} << 28) | (u64{mask} << 31) | (u64{insn.Bits(40, 14)} << 36) |
                     (u64{insn.Bits(90, 1)} << 49) | (u64{7} << 51) | (u64{1} << 55));
                const u32 components = static_cast<u32>(std::popcount(mask));
                for (u32 i = 0; i < components; ++i) {
                    const u32 target = !split || i < 2 ? static_cast<u32>(dest) + i : second_dest + (i - 2);
                    if (target != 255 && (split || i < 2))
                        emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{246 + i} << 20));
                }
                break;
            }
            if (array) {
                emit(0x5c98000000000000ULL | predicate | 250 | (u64{15} << 39) | (u64{source_a + 1} << 20));
                emit(0x5c98000000000000ULL | predicate | 251 | (u64{15} << 39) | (u64{source_b} << 20));
                source_b = 250;
            }
            emit(0xda00000000000000ULL | predicate | dest | (u64{source_a} << 8) |
                 (u64{source_b} << 20) | (u64{insn.Bits(64, 8)} << 28) |
                 (u64{insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49) |
                 (u64{swizzle} << 50) |
                 (u64{offsets ? (level ? 12U : 4U) : multisample ? 6U
                      : array ? 8U : volume ? 7U : dimension == 0 ? (level ? 1U : 0U) : (level ? 5U : 2U)} << 53));
            break;
        }
        case 0x367: { // TLD.B at level zero (.LZ), the handle in a register.
            // The values are the coordinates, then the handle, split over Ra and Rb as for TEX.B
            // (ARRAY_2D: layer, x | y, handle); Maxwell TLD.B takes the handle first in Rb.
            constexpr std::array<u32, 8> coordinate_count{1, 2, 3, 0, 0, 3, 0, 0};
            constexpr std::array<u32, 8> maxwell_type{0, 2, 4, 0, 0, 3, 0, 0};
            const u32 mask = insn.Bits(72, 4), second_dest = insn.Bits(64, 8);
            const u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            const u32 dimension = insn.Bits(61, 3), coordinates = coordinate_count[dimension];
            const u32 count = coordinates + 1, in_a = (count + 1) / 2;
            if (coordinates == 0 || !insn.Bits(59, 1) || !insn.Bits(60, 1) || insn.Bits(40, 19) ||
                insn.Bits(76, 5) || insn.Bits(81, 3) != 7 || insn.Bits(87, 3) != 1 || insn.Bits(91, 1) ||
                mask == 0 || source_a == 255 || source_a + in_a > 246 || source_b == 255 ||
                source_b + (count - in_a) > 246)
                return fail(index, "native bindless texel fetch mode");
            const auto value = [&](u32 i) { return i < in_a ? source_a + i : source_b + (i - in_a); };
            const auto copy = [&](u32 target, u32 source) {
                emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{source} << 20));
            };
            for (u32 i = 0; i < coordinates; ++i) copy(250 + i, value(i));
            copy(246, value(coordinates));
            emit(0xdd00000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{246} << 20) |
                 (u64{maxwell_type[dimension]} << 28) | (u64{mask} << 31) | (u64{insn.Bits(90, 1)} << 49) |
                 (u64{7} << 51));
            const u32 components = static_cast<u32>(std::popcount(mask));
            const bool split = second_dest != 255;
            for (u32 i = 0; i < components; ++i) {
                u32 target = 255;
                if (!split || i < 2) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                else target = second_dest + (i - 2);
                if (target != 255) copy(target, 250 + i);
            }
            break;
        }
        case 0x91a: // DEPBAR.LE SB5, 1: scoreboard wait, represented by IR data dependencies.
            // (hookshot) any scoreboard and count (OoT's mesh programs wait on SB0 at 14 or 30): a wait is no work here
            if (insn.words[0] != 0x791a || (insn.words[1] & ~0xffffU) || insn.words[2]) {
                return fail(index, "native dependency barrier mode");
            }
            break;
        case 0xb69: case 0xf69: case 0x36a: { // TMML (LOD query), 1D/2D/3D/CUBE. Results are 8.8 fixed point, as on Maxwell.
            // 0xf69: the handle in a uniform register (bit 91); 0x36a, TMML.B: the handle in Rb and
            // every coordinate in Ra. Both become Maxwell TMML.B with the handle in Rb.
            const bool bindless = opcode != 0xb69;
            const u32 handle = opcode == 0xf69 ? uniform_register(insn.Bits(40, 6))
                             : opcode == 0x36a ? insn.Bits(32, 8) : 255;
            const u32 mask = insn.Bits(72, 4), second_dest = insn.Bits(64, 8);
            const u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            const u32 binding = insn.Bits(54, 5), dimension = insn.Bits(61, 3);
            // Coordinates: all from Ra when Rb is RZ, else split as for TEX (2D: Ra, Rb).
            constexpr std::array<u32, 4> coordinate_count{1, 2, 3, 3};
            const u32 coordinates = dimension < 4 ? coordinate_count[dimension] : 0;
            const u32 in_a = source_b == 255 || opcode == 0x36a ? coordinates : (coordinates + 1) / 2;
            if (coordinates == 0 || insn.Bits(76, 4) || insn.Bits(80, 10) || mask == 0 ||
                insn.Bits(59, 1) != (opcode == 0x36a ? 1U : 0U) || source_a + in_a > 246 ||
                (in_a < coordinates && source_b + (coordinates - in_a) > 246) ||
                (opcode == 0xf69 && (handle >= 246 || insn.Bits(46, 13) || !insn.Bits(91, 1))) ||
                (opcode == 0x36a && (handle >= 246 || insn.Bits(40, 19) || insn.Bits(91, 1))) ||
                (!bindless && (insn.Bits(40, 14) > 0x1fff || binding >= 18 ||
                               (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding))))
                return fail(index, "native texture LOD query mode");
            if (!bindless) result.required_texture_buffer = binding;
            for (u32 i = 0; i < coordinates; ++i)
                emit(0x5c98000000000000ULL | predicate | (250 + i) | (u64{15} << 39) |
                     (u64{i < in_a ? source_a + i : source_b + (i - in_a)} << 20));
            if (bindless) emit(0x5c98000000000000ULL | predicate | 246 | (u64{15} << 39) | (u64{handle} << 20));
            emit((bindless ? 0xdf60000000000000ULL : 0xdf58000000000000ULL) | predicate | 250 | (u64{250} << 8) |
                 (u64{bindless ? 246U : 255U} << 20) | (u64{dimension * 2} << 28) | (u64{mask} << 31) |
                 (u64{bindless ? 0U : insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49));
            const u32 components = static_cast<u32>(std::popcount(mask));
            const bool split = second_dest != 255;
            for (u32 i = 0; i < components; ++i) {
                u32 target = 255;
                if (!split || i < 2) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                else target = second_dest + (i - 2);
                if (target != 255)
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{250 + i} << 20));
            }
            break;
        }
        case 0xb63: case 0xf63: { // TLD4 (gather), 2D without offsets or depth compare; component at bits 87-88.
            // 0xf63: the handle in a uniform register (bit 91), Maxwell TLD4.B with the handle in Rb.
            const bool bindless = opcode == 0xf63;
            const u32 handle = bindless ? uniform_register(insn.Bits(40, 6)) : 255;
            const u32 mask = insn.Bits(72, 4), second_dest = insn.Bits(64, 8);
            const u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            const u32 binding = insn.Bits(54, 5);
            // Bit 76: a texel offset after the coordinates (x, y in Ra, Ra+1, offsets in Rb, as
            // for TEX); Maxwell TLD4 takes it as AOFFI (bits 54-55) from Rb.
            // (hookshot) bits 76-77 the offset mode: 1 one offset (AOFFI), 2 four per-pixel offsets (PTP,
            // textureGatherOffsets) in Rb, Rb+1; Maxwell TLD4 takes them the same way (mode 2 at bits 54-55, R246-R247)
            const bool ptp = insn.Bits(76, 2) == 2;
            const bool offsets = insn.Bits(76, 1) != 0 || ptp;
            // (hookshot) 2D arrays (bits 61-63 = 5) without offsets too: (layer, x) in Ra, Ra+1 and y in Rb, as
            // TLD reads them; Maxwell TLD4's ARRAY_2D (3 at bits 28-30) takes layer, x, y from R250..R252.
            const bool gather_array = insn.Bits(61, 3) == 5;
            // (hookshot) bit 78: depth compare (TLD4.DC, a shadow gather): x, y in Ra, Ra+1 and the reference in Rb,
            // as an offset would be; Maxwell TLD4.DC (bit 50) reads it from the meta register
            const bool depth_compare = insn.Bits(78, 2) == 1;
            if (depth_compare && (offsets || gather_array || bindless || source_a >= 245 || source_b == 255 ||
                                  insn.Bits(87, 2)))
                return fail(index, "native texture gather mode");
            if ((insn.Bits(61, 3) != 1 && !(gather_array && !offsets && source_a < 245)) || (insn.Bits(78, 2) && !depth_compare) ||
                (insn.Bits(77, 1) && !ptp) || (ptp && (bindless || source_b >= 245)) ||
                insn.Bits(81, 3) != 7 || insn.Bits(59, 1) ||
                (offsets && (bindless || source_b == 255 || (source_a != 255 && source_a >= 245))) ||
                insn.Bits(89, 1) || mask == 0 ||
                (bindless ? (handle >= 246 || insn.Bits(46, 13) || !insn.Bits(91, 1))
                          : (insn.Bits(40, 14) > 0x1fff || binding >= 18 ||
                             (result.required_texture_buffer != ~0U && result.required_texture_buffer != binding))))
                return fail(index, "native texture gather mode");
            if (!bindless) result.required_texture_buffer = binding;
            // Two coordinates: x in Ra, y in Rb (the coordinate list splits evenly across them);
            // with an offset, x and y in Ra, Ra+1 and the offset in Rb.
            const u32 y = !offsets && !depth_compare ? source_b : source_a == 255 ? 255U : source_a + 1;
            const u64 gather_type = gather_array ? 3U : 2U;
            emit(0x5c98000000000000ULL | predicate | 250 | (u64{15} << 39) | (u64{source_a} << 20));
            if (gather_array) {
                emit(0x5c98000000000000ULL | predicate | 251 | (u64{15} << 39) | (u64{source_a + 1} << 20));
                emit(0x5c98000000000000ULL | predicate | 252 | (u64{15} << 39) | (u64{source_b} << 20));
            } else
            emit(0x5c98000000000000ULL | predicate | 251 | (u64{15} << 39) | (u64{y} << 20));
            if (offsets || depth_compare) {
                emit(0x5c98000000000000ULL | predicate | 246 | (u64{15} << 39) | (u64{source_b} << 20));
                if (ptp) emit(0x5c98000000000000ULL | predicate | 247 | (u64{15} << 39) | (u64{source_b + 1} << 20));
            }
            if (bindless) {
                emit(0x5c98000000000000ULL | predicate | 246 | (u64{15} << 39) | (u64{handle} << 20));
                emit(0xdec0000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{246} << 20) |
                     (gather_type << 28) | (u64{mask} << 31) | (u64{insn.Bits(87, 2)} << 38) |
                     (u64{insn.Bits(90, 1)} << 49) | (u64{7} << 51));
            } else {
            emit(0xc800000000000000ULL | predicate | 250 | (u64{250} << 8) |
                 (u64{offsets || depth_compare ? 246U : 255U} << 20) | (gather_type << 28) | (u64{mask} << 31) |
                 (u64{insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49) | (u64{depth_compare} << 50) | (u64{7} << 51) |
                 (u64{ptp ? 2U : offsets ? 1U : 0U} << 54) | (u64{insn.Bits(87, 2)} << 56));
            }
            const u32 components = static_cast<u32>(std::popcount(mask));
            const bool split = second_dest != 255;
            for (u32 i = 0; i < components; ++i) {
                u32 target = 255;
                if (!split || i < 2) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                else target = second_dest + (i - 2);
                if (target != 255)
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{250 + i} << 20));
            }
            break;
        }
        case 0xb60: case 0xf60: case 0x361: { // TEX; 0xf60: handle in a uniform register; 0x361: TEX.B
            // Bindless forms become Maxwell TEX.B, the handle first among the meta registers (the
            // texture pass tracks it back to its constant buffer words). 0x361 lists the handle
            // right after the coordinates, then the LOD and the offsets (see general_tex).
            const bool bindless = opcode != 0xb60;
            const u32 handle = opcode == 0xf60 ? uniform_register(insn.Bits(40, 6)) : 255;
            if (bindless && ((opcode == 0xf60 && (handle == ~0U || handle >= 246 || insn.Bits(46, 13))) ||
                             (opcode == 0x361 && (insn.Bits(40, 19) || insn.Bits(87, 3) > 3))))
                return fail(index, "native bindless texture mode");
            const u32 mask = insn.Bits(72, 4);
            const u32 second_dest = insn.Bits(64, 8);
            constexpr std::array<u32, 8> rg_masks{1, 2, 4, 8, 3, 9, 10, 12};
            constexpr std::array<u32, 5> rgba_masks{7, 11, 13, 14, 15};
            u32 swizzle = ~0U;
            const bool split = second_dest != 255;
            const bool depth_compare = insn.Bits(78, 1);
            const u32 dimension = insn.Bits(61, 3), lod = insn.Bits(87, 3);
            const u32 source_a = insn.Bits(24, 8), source_b = insn.Bits(32, 8);
            if (!bindless && dimension == 5 && depth_compare && lod == 0 && !split && mask == 1) {
                if (!insn.Bits(60, 1) || insn.Bits(76, 2) || insn.Bits(81, 3) != 7 ||
                    insn.Bits(59, 1) || insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) != 2 ||
                    (source_a & 1) || (source_b & 1) || source_a >= 248 || source_b >= 248 || dest >= 248)
                    return fail(index, "native array depth comparison sample mode");
                if (!bind_texture_buffer(2)) return fail(index, "native texture descriptor buffer conflict");
                for (u32 i = 0; i < 3; ++i) {
                    const u32 source = i < 2 ? source_a + i : source_b;
                    emit(0x5c98000000000000ULL | predicate | (250 + i) |
                         (u64{15} << 39) | (u64{source} << 20));
                }
                emit(0xc000000000000000ULL | predicate | dest | (u64{250} << 8) |
                     (u64{source_b + 1} << 20) | (u64{3} << 28) | (u64{1} << 31) |
                     (u64{insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49) |
                     (u64{1} << 50) | (u64{7} << 51));
                break;
            }
            // General form: gather the native operand list (array index first, coordinates,
            // then LOD/bias and the depth reference) into Maxwell TEX's coordinate and meta
            // registers, sample into scratch and copy the packed components out. Native packs
            // up to four values as halves of Ra and Rb (1+1, 2+1, 2+2), five or more as 4+n.
            const auto general_tex = [&]() -> bool {
                constexpr std::array<u32, 8> coordinate_count{1, 2, 3, 3, 2, 3, 0, 4};
                constexpr std::array<u32, 8> maxwell_type{0, 2, 4, 6, 1, 3, 0, 7};
                const bool offsets = insn.Bits(76, 1) != 0;
                // Bit 59 is .B, set in 0x361's encoding.
                if (dimension == 6 || insn.Bits(77, 1) || insn.Bits(81, 3) != 7 ||
                    insn.Bits(59, 1) != (opcode == 0x361 ? 1U : 0U) ||
                    (!bindless && (insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) >= 18)) ||
                    (lod > 3 && lod != 6 && lod != 7) || (depth_compare && mask != 1) || mask == 0)
                    return false;
                const u32 coordinates = coordinate_count[dimension];
                const bool has_lod = lod == 2 || lod == 3 || lod == 6 || lod == 7;
                // 0x361's handle is one more value after the coordinates: 2D.DC is (x, y | handle,
                // reference), ARRAY_2D is (layer, x | y, handle); with five or more values the
                // coordinates keep Ra (CUBE.LL is (x, y, z | handle, LOD)).
                const u32 handle_slot = opcode == 0x361 ? 1 : 0;
                const u32 count = coordinates + handle_slot + (has_lod ? 1 : 0) + (offsets ? 1 : 0) +
                                  (depth_compare ? 1 : 0);
                // Unpacked 0x361 (bit 60 clear): Ra holds the coordinates, Rb the handle and the rest.
                const u32 in_a = opcode == 0x361 && !insn.Bits(60, 1) ? coordinates
                               : count <= 4 ? (count + 1) / 2 : opcode == 0x361 ? coordinates : 4;
                const auto value = [&](u32 i) -> u32 {
                    if (i < in_a) return source_a == 255 ? 255U : source_a + i;
                    return source_b == 255 ? 255U : source_b + (i - in_a);
                };
                if ((source_a != 255 && source_a + in_a > 246) ||
                    (count > in_a && source_b != 255 && source_b + (count - in_a) > 246))
                    return false;
                const u32 texture_handle = opcode == 0x361 ? value(coordinates) : handle;
                if (bindless && texture_handle >= 246) return false;
                // A second descriptor buffer is read bindless: TEX.B takes the handle, loaded from
                // that buffer, as its first meta register.
                const u32 binding = insn.Bits(54, 5);
                const bool second_buffer = !bindless && !bind_texture_buffer(binding);
                const bool tex_b = bindless || second_buffer;
                const auto copy = [&](u32 target, u32 source) {
                    emit(0x5c98000000000000ULL | predicate | target | (u64{15} << 39) | (u64{source} << 20));
                };
                // Coordinates to R250.., then LOD/bias, offsets and reference to R246...
                for (u32 i = 0; i < coordinates; ++i) copy(250 + i, value(i));
                u32 meta = 246;
                u32 next = coordinates + handle_slot;
                if (bindless) copy(meta++, texture_handle);
                else if (second_buffer)
                    emit(0x4c98000000070000ULL | meta++ | (u64{15} << 39) | (u64{insn.Bits(40, 14)} << 20) |
                         (u64{binding} << 34));
                if (has_lod) copy(meta++, value(next++));
                if (offsets) copy(meta++, value(next++));
                if (depth_compare) copy(meta++, value(next++));
                if (tex_b) {
                    // Maxwell TEX.B: AOFFI at 36 and the LOD mode at 37 instead of 54 and 55.
                    emit(0xde80000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{246} << 20) |
                         (u64{maxwell_type[dimension]} << 28) | (u64{mask} << 31) | (u64{offsets} << 36) |
                         (u64{lod} << 37) | (u64{insn.Bits(90, 1)} << 49) | (u64{depth_compare} << 50) |
                         (u64{7} << 51));
                } else {
                emit(0xc000000000000000ULL | predicate | 250 | (u64{250} << 8) |
                     (u64{meta == 246 ? 255U : 246U} << 20) | (u64{maxwell_type[dimension]} << 28) |
                     (u64{mask} << 31) | (u64{insn.Bits(40, 14)} << 36) |
                     (u64{insn.Bits(90, 1)} << 49) | (u64{depth_compare} << 50) | (u64{7} << 51) |
                     (u64{offsets} << 54) | (u64{lod} << 55));
                }
                const u32 components = static_cast<u32>(std::popcount(mask));
                if (insn.Bits(79, 1)) {
                    // FP16 results: (c0, c1) packed into the first destination, (c2, c3) into
                    // the second (F2F writes the half in the low 16 bits; ISCADD merges).
                    const auto pack = [&](u32 target, u32 low, u32 high) {
                        if (target == 255) return;
                        emit(0x5ca8000000000000ULL | predicate | 246 | (u64{1} << 8) | (u64{2} << 10) | (u64{low} << 20));
                        emit(0x5ca8000000000000ULL | predicate | 247 | (u64{1} << 8) | (u64{2} << 10) | (u64{high} << 20));
                        // (hookshot: Rd = (Ra << 16) + Rb: the second of the pair, R247, is Ra. These two were the other
                        // way round, swapping every pair: red and green, and blue in the high half with the low one
                        // empty, which lost the blue of OoT's upscaler networks: the yellow cast. As Lichii has it)
                        emit(0x5c18000000000000ULL | predicate | target | (u64{247} << 8) | (u64{246} << 20) | (u64{16} << 39));
                    };
                    pack(static_cast<u32>(dest), 250, components > 1 ? 251U : 255U);
                    if (components > 2) {
                        pack(split ? second_dest : (dest == 255 ? 255U : static_cast<u32>(dest) + 1),
                             252, components > 3 ? 253U : 255U);
                    }
                    return true;
                }
                for (u32 i = 0; i < components; ++i) {
                    u32 target = 255;
                    if (!split) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                    else if (i < 2) target = dest == 255 ? 255U : static_cast<u32>(dest) + i;
                    else target = second_dest + (i - 2);
                    if (target != 255) copy(target, 250 + i);
                }
                return true;
            };
            if (split) {
                for (u32 i = 0; i < rgba_masks.size(); ++i) if (rgba_masks[i] == mask) swizzle = i;
            } else {
                for (u32 i = 0; i < rg_masks.size(); ++i) if (rg_masks[i] == mask) swizzle = i;
            }
            // SM70 scalar coordinates and split RG/BA destinations match TEXS.
            // Descriptor offsets are words in both encodings, but only native
            // explicitly encodes the constant-buffer binding.
            if (bindless || !insn.Bits(60, 1) || (dimension != 1 && dimension != 5) ||
                swizzle == ~0U || insn.Bits(76, 2) ||
                insn.Bits(81, 3) != 7 || (lod > 1 && (lod != 3 || dimension != 1 || depth_compare)) ||
                (lod == 3 && ((source_a & 1) || source_a >= 249)) ||
                insn.Bits(59, 1) ||
                insn.Bits(40, 14) > 0x1fff || insn.Bits(54, 5) != 2 ||
                dest == 255 || (mask & (mask - 1) && ((dest & 1) || dest >= 254)) ||
                (split && mask == 15 && ((second_dest & 1) || second_dest >= 254)) ||
                (depth_compare && (dimension != 1 || split || mask != 1 || (source_a & 1) || source_a >= 249)) ||
                (dimension == 5 && ((source_a & 1) || source_a >= 249)) || bindless ||
                !bind_texture_buffer(2)) {
                if (!general_tex()) return fail(index, "native texture mode or descriptor binding");
                break;
            }
            emit((0xd800000000000000ULL & ~(u64{insn.Bits(79, 1)} << 59)) | predicate | dest | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(64, 8)} << 28) |
                 (u64{insn.Bits(40, 14)} << 36) | (u64{insn.Bits(90, 1)} << 49) |
                 (u64{swizzle} << 50) | (u64{dimension == 5 ? (lod ? 8U : 7U) : depth_compare ? (lod ? 6U : 4U) : (lod == 3 ? 3U : lod ? 2U : 1U)} << 53));
            break;
        }
        case 0x816: { // Captured PRMT half-word concatenation
            if (insn.words[1] > 0xffff || (insn.words[2] & ~0xffU)) return fail(index, "native permutation mode");
            if (insn.words[1] != 0x5410 && insn.words[1] != 0x5432 && insn.words[1] != 0x7610 && insn.words[1] != 0x7632) {
                // Byte i of the result is byte (s & 7) of c:a, or its sign bit repeated when s & 8.
                const u32 a = insn.Bits(24, 8), c = insn.Bits(64, 8);
                if (a >= 250 && a != 255) return fail(index, "native permutation source");
                mov_imm(250, 0);
                for (u32 i = 0; i < 4; ++i) {
                    const u32 select = (insn.words[1] >> (i * 4)) & 0xf;
                    const u32 source = select & 4 ? c : a, position = (select & 3) * 8;
                    emit(0x3800000000000000ULL | predicate | 251 | (u64{source} << 8) |
                         (u64{select & 8 ? (position + 7) | (1U << 8) : position | (8U << 8)} << 20) |
                         (u64{(select >> 3) & 1} << 48));
                    emit(0x36f0000000000000ULL | predicate | 250 | (u64{251} << 8) |
                         (u64{(i * 8) | (8U << 8)} << 20) | (u64{250} << 39));
                }
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{255} << 20));
                break;
            }
            const u32 a = insn.Bits(24, 8), b = insn.Bits(64, 8);
            if (insn.words[1] != 0x5432 && insn.words[1] != 0x7632) {
                emit(0x0400000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{0xffff} << 20));
            } else {
                emit(0x3828000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{16} << 20));
            }
            if (insn.words[1] == 0x7610 || insn.words[1] == 0x7632) {
                emit(0x0400000000000000ULL | predicate | 251 | (u64{b} << 8) | (u64{0xffff0000} << 20));
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{251} << 20));
            } else {
                emit(base(0x5c18000000000000ULL) | (u64{b} << 8) | (u64{250} << 20) | (u64{16} << 39));
            }
            break;
        }
        case 0x307: case 0xb07: { // FRND.F32, round to an integral floating-point value (0xb07: cbuf)
            if (insn.Bits(75, 2) != 2 || insn.Bits(84, 2) != 2 || insn.Bits(81, 3) ||
                insn.Bits(86, 6)) return fail(index, "native floating round format");
            if (opcode == 0xb07 && !valid_cbuf()) return fail(index, "native constant buffer address");
            emit(base(opcode == 0xb07 ? 0x4ca8000000000000ULL : 0x5ca8000000000000ULL) | (u64{2} << 8) | (u64{2} << 10) |
                 (opcode == 0xb07 ? cbuf() : u64{insn.Bits(32, 8)} << 20) | (u64{8 + insn.Bits(78, 2)} << 39) |
                 (u64{insn.Bits(80, 1)} << 44) | (u64{insn.Bits(63, 1)} << 45) |
                 (u64{insn.Bits(62, 1)} << 49));
            break;
        }
        case 0x321: // ALD
        case 0x322: { // AST
            if (insn.Bits(76, 2) || insn.Bits(79, 1)) return fail(index, "native indexed/patch attribute access");
            const u32 address = insn.Bits(40, 10);
            const u32 components = insn.Bits(74, 2) + 1;
            if (address % 4 || address + components * 4 > 0x400 || insn.Bits(24, 8) != 255) {
                return fail(index, "native attribute address or indexing");
            }
            // a[0x5c] has no Maxwell meaning (Ampere uses it for a per-primitive hint such as the
            // shading rate). Nothing reads it back; dropping the store keeps full-rate shading.
            if (opcode == 0x322 && address == 0x5c && components == 1) break;
            u32 value_reg = opcode == 0x321 ? static_cast<u32>(dest) : insn.Bits(32, 8);
            if (value_reg == 255) {
                if (opcode == 0x321) break; // load into RZ: nothing to do
                // AST of RZ writes zeros: store them from zeroed scratch registers.
                for (u32 i = 0; i < components; ++i) mov_imm(250 + i, 0);
                value_reg = 250;
            }
            if (value_reg + components > 255) return fail(index, "native attribute register range");
            const u32 vertex = opcode == 0x321 ? insn.Bits(32, 8) : insn.Bits(64, 8);
            const u64 translated = (opcode == 0x321 ? 0xefd8000000000000ULL : 0xeff0000000000000ULL) |
                predicate | value_reg | (u64{255} << 8) | (u64{address} << 20) |
                (u64{vertex} << 39) | (u64{components - 1} << 47);
            result.instructions.push_back(translated);
            break;
        }
        case 0x387: case 0x983: { // Local spill: 32, 64 or 128 bits at [Ra + immediate byte offset].
            // The size is bits 73-75 (4, 5, 6 = 32, 64, 128 bits); wider accesses become 32-bit ones.
            // A load's bit 85 (.LU) is a cache hint.
            const bool store = opcode == 0x387;
            const u32 size = insn.Bits(73, 3);
            const u32 data = insn.Bits(32, 8), offset = insn.Bits(40, 24);
            u32 address = insn.Bits(24, 8);
            const u32 words = size == 6 ? 4 : size == 5 ? 2 : 1, value = store ? data : static_cast<u32>(dest);
            // (hookshot) LDL [Ra + URb + immediate]: bit 91 adds a uniform register at 32 (as LDS)
            const bool uniform_base = !store && insn.Bits(91, 1);
            const u32 base_register = uniform_base ? (insn.Bits(38, 2) ? ~0U : uniform_register(insn.Bits(32, 6))) : 255U;
            if ((address != 255 && address >= 246) || (store && insn.Bits(16, 8)) || (!store && !uniform_base && data) ||
                (value != 255 && (value + words > 248 || (value & (words - 1)))) || (!store && value == 255) ||
                (offset & 3) || offset + 4 * words > 0x800000 || size < 4 || size > 6 || base_register == ~0U ||
                (insn.words[2] & ~(store ? 0xe00U : (0x200e00U | (uniform_base ? 0x08000000U : 0U)))) != 0x00100000U)
                return fail(index, "native local scalar spill mode");
            if (base_register != 255) {
                if (address == 255) {
                    address = base_register;
                } else {
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{address} << 8) | (u64{base_register} << 20));
                    address = 250;
                }
            }
            for (u32 word = 0; word < words; ++word)
                emit((store ? 0xef50000000000000ULL : 0xef40000000000000ULL) | predicate |
                     (value == 255 ? 255U : value + word) | (u64{address} << 8) |
                     (u64{offset + 4 * word} << 20) | (u64{4} << 48));
            break;
        }
        case 0x987: { // (hookshot) STL [Ra + URc + immediate], Rb: a uniform base at 64 (bit 91), else as 0x387
            const u32 size = insn.Bits(73, 3);
            const u32 data = insn.Bits(32, 8), offset = insn.Bits(40, 24), address_register = insn.Bits(24, 8);
            const u32 words = size == 6 ? 4 : size == 5 ? 2 : 1;
            const u32 uniform_base = insn.Bits(91, 1) ? uniform_register(insn.Bits(64, 6)) : ~0U;
            if (uniform_base == ~0U || insn.Bits(16, 8) || size < 4 || size > 6 || (offset & 3) ||
                offset + 4 * words > 0x800000 || (data != 255 && (data + words > 248 || (data & (words - 1)))) ||
                (address_register != 255 && address_register >= 246) ||
                (insn.words[2] & ~(0xe00U | 0x3fU)) != 0x08100000U)
                return fail(index, "native local store uniform addressing mode");
            u32 address = uniform_base;
            if (address_register != 255) {
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{address_register} << 8) | (u64{uniform_base} << 20));
                address = 250;
            }
            for (u32 word = 0; word < words; ++word)
                emit(0xef50000000000000ULL | predicate | (data == 255 ? 255U : data + word) | (u64{address} << 8) |
                     (u64{offset + 4 * word} << 20) | (u64{4} << 48));
            break;
        }
        case 0x803: { // P2R PR, register merge source, immediate mask, byte select (76-77)
            if (insn.words[1] > 0x7f || (insn.words[2] & ~0x3000U))
                return fail(index, "native predicate-to-register mode");
            emit(base(0x38e8000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.words[1]} << 20) | (u64{insn.Bits(76, 2)} << 41));
            break;
        }
        case 0x804: { // R2P PR, register, immediate mask, byte select (76-77)
            if (insn.Bits(16, 8) || insn.words[1] > 0x7f || (insn.words[2] & ~0x3000U))
                return fail(index, "native register-to-predicate mode");
            emit(0x38f0000000000000ULL | predicate | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.words[1]} << 20) | (u64{insn.Bits(76, 2)} << 41));
            break;
        }
        case 0xc02: // MOV uniform register
        case 0x202: // MOV register
        case 0x802: // MOV immediate
        case 0xa02: { // MOV constant buffer
            if (insn.Bits(72, 4) != 15 || (insn.Bits(76, 16) & ~(opcode == 0xc02 ? 0x8000U : 0U))) return fail(index, "native MOV modifiers");
            if (opcode == 0x802) {
                result.instructions.push_back(base(0x0100000000000000ULL) | (u64{15} << 12) |
                    (u64{insn.words[1]} << 20));
            } else if (opcode == 0xc02) {
                const u32 src = insn.words[1] < 64 ? uniform_register(insn.words[1]) : ~0U;   // (hookshot: any UR)
                if (src == ~0U || insn.words[2] != 0x08000f00) return fail(index, "native uniform move source");
                emit(base(0x5c98000000000000ULL) | (u64{15} << 39) | (u64{src} << 20));
            } else if (opcode == 0x202) {
                if (insn.Bits(40, 24)) return fail(index, "native MOV register modifiers");
                result.instructions.push_back(base(0x5c98000000000000ULL) | (u64{15} << 39) |
                    (u64{insn.Bits(32, 8)} << 20));
            } else {
                if (!valid_cbuf() || insn.Bits(62, 2)) return fail(index, "native MOV constant buffer");
                result.instructions.push_back(base(0x4c98000000000000ULL) | (u64{15} << 39) | cbuf());
            }
            break;
        }
        case 0x223: case 0x423: case 0x623: case 0x823: case 0xa23: case 0xc23: case 0xe23: { // FFMA
            if (insn.Bits(76, 1) || (insn.Bits(81, 11) & ~(opcode == 0xe23 || opcode == 0xc23 ? 0x400U : 0U))) return fail(index, "native FFMA denormal/source mode");
            const u32 a = operand(0), b = operand(1), c = operand(2);
            emit(base(0x5980000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) | (u64{c} << 39) |
                (u64{insn.Bits(77, 1)} << 50) | (u64{insn.Bits(78, 2)} << 51) | (u64{insn.Bits(80, 1)} << 53));
            break;
        }
        case 0x220: case 0x820: case 0xa20: case 0xc20: { // FMUL
            const u32 scale = insn.Bits(84, 3);
            if (insn.Bits(76, 1) || scale == 0 || insn.Bits(81, 3) ||
                insn.Bits(87, 4) || insn.Bits(91, 1) != static_cast<u32>(opcode == 0xc20) ||
                (scale != 4 && (!insn.Bits(80, 1) || insn.Bits(78, 2))))
                return fail(index, "native FMUL scale/mode");
            const u32 a = operand(0), b = operand(1);
            emit(base(0x5c68000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(78, 2)} << 39) | (u64{std::array<u32, 8>{0, 3, 2, 1, 0, 6, 5, 4}[scale]} << 41) | (u64{insn.Bits(80, 1)} << 44) | (u64{insn.Bits(77, 1)} << 50));
            break;
        }
        case 0x221: case 0x421: case 0x621: case 0x821: case 0xa21: case 0xe21: { // FADD
            if (insn.Bits(81, 10) || insn.Bits(91, 1) != static_cast<u32>(opcode == 0xe21))
                return fail(index, "native FADD mode");
            const u32 a = operand(0), b = operand(insn.Bits(9, 3) == 2 || insn.Bits(9, 3) == 3 || insn.Bits(9, 3) == 7 ? 2 : 1);
            emit(base(0x5c58000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(78, 2)} << 39) | (u64{insn.Bits(80, 1)} << 44) | (u64{insn.Bits(77, 1)} << 50));
            break;
        }
        case 0x209: case 0x809: case 0xa09: case 0xc09: { // FMNMX, including a uniform source
            if (insn.Bits(74, 6) || insn.Bits(82, 5) || (opcode != 0xc09 && insn.Bits(91, 1)))
                return fail(index, "native floating min/max mode");
            const u32 a = operand(0), b = operand(1);
            // .NAN (bit 81): an unordered pair gives the canonical NaN. R252 = ~0 when both are numbers.
            const bool propagate_nan = insn.Bits(81, 1);
            if (propagate_nan)
                emit(0x5800000000000000ULL | predicate | 252 | (u64{a} << 8) | (u64{b} << 20) | (u64{7} << 39) | (u64{7} << 48));
            emit(base(0x5c60000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                 (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42) |
                 (u64{insn.Bits(80, 1)} << 44));
            if (propagate_nan) {
                emit(base(0x5c40000000000000ULL) | (dest << 8) | (u64{252} << 20) | (u64{7} << 48));
                emit(0x0400000000000000ULL | predicate | 252 | (u64{252} << 8) | (u64{0xffffffffU} << 20) | (u64{2} << 53));
                emit(0x0400000000000000ULL | predicate | 252 | (u64{252} << 8) | (u64{0x7fffffffU} << 20));
                emit(base(0x5c40000000000000ULL) | (dest << 8) | (u64{252} << 20) | (u64{1} << 41) | (u64{7} << 48));
            }
            break;
        }
        case 0x308: case 0xb08: case 0xd08: { // MUFU (register, constant or uniform register source)
            const u32 operation = insn.Bits(74, 6);
            // Bit 91 is part of the uniform-register form's encoding.
            if (operation > 8 || operation == 6 || operation == 7 ||
                insn.Bits(80, 12) != (opcode == 0xd08 ? 0x800U : 0U))
                return fail(index, "native MUFU mode");
            // SM70+ MUFU.COS/SIN take the angle in turns (compilers multiply by 1/2pi first);
            // Eden's Maxwell MUFU takes radians (its RRO is a plain move), so scale by 2pi.
            const bool turns = operation == 0 || operation == 1;
            const auto to_radians = [&](u32 source) {
                emit(0x1e00000000000000ULL | predicate | 250 | (u64{source} << 8) |
                     (u64{0x40c90fdbU} << 20));
                return 250U;
            };
            if (insn.Bits(73, 1)) {
                if (opcode != 0x308 || insn.Bits(60, 2) > 1)
                    return fail(index, "native half special-function source");
                emit(0x5ca8000000000000ULL | predicate | 250 | (u64{2} << 8) | (u64{1} << 10) |
                     (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(60, 1)} << 41) |
                     (u64{insn.Bits(63, 1)} << 45) | (u64{insn.Bits(62, 1)} << 49));
                if (turns) to_radians(250);
                emit(0x5080000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{operation} << 20));
                emit(base(0x5ca8000000000000ULL) | (u64{1} << 8) | (u64{2} << 10) | (u64{250} << 20));
            } else {
                u32 src = opcode == 0xd08 ? uniform_register(insn.Bits(32, 8)) : operand(1);
                if (src == ~0U) return fail(index, "native MUFU uniform source");
                if (turns) src = to_radians(src);
                emit(base(0x5080000000000000ULL) | (u64{src} << 8) | (u64{operation} << 20));
            }
            break;
        }
        case 0x526: { // IPA.PASS / CONSTANT with the attribute indexed by a uniform register
            const u32 index_register = uniform_register(insn.Bits(24, 8));
            if (index_register == ~0U || insn.Bits(32, 8) != 255 ||
                (insn.Bits(76, 4) != 0 && insn.Bits(76, 4) != 4) || insn.Bits(81, 3) != 7)
                return fail(index, "native indexed IPA mode");
            if (index_register == 255) {
                emit(base(0xe000000000000000ULL) | (u64{255} << 8) | (u64{255} << 20) |
                     (u64{insn.Bits(64, 8)} << 30) | (u64{insn.Bits(78, 1)} << 55));
                break;
            }
            // Maxwell IPA.IDX reads the whole byte address from the index register.
            emit(0x1c00000000000000ULL | predicate | 250 | (u64{index_register} << 8) |
                 (u64{insn.Bits(64, 8) * 4} << 20));
            emit(base(0xe000000000000000ULL) | (u64{250} << 8) | (u64{255} << 20) |
                 (u64{insn.Bits(64, 8)} << 30) | (u64{1} << 38) | (u64{insn.Bits(78, 1)} << 55));
            break;
        }
        case 0x326: { // IPA.PASS / CONSTANT, default or centroid sample location
            // Bits 76-77 are the sample mode (title 1 interpolates most varyings at the centroid),
            // bit 78 the constant mode; Maxwell keeps them at bits 52-53 and 55.
            if (insn.Bits(32, 8) != 255 || insn.Bits(76, 2) > 1 || insn.Bits(79, 1) ||
                insn.Bits(81, 3) != 7)
                return fail(index, "native IPA interpolation mode");
            emit(base(0xe000000000000000ULL) | (u64{255} << 8) | (u64{255} << 20) | (u64{insn.Bits(64, 8)} << 30) |
                 (u64{insn.Bits(76, 2)} << 52) | (u64{insn.Bits(78, 1)} << 55));
            break;
        }
        case 0x81a: { // SGXT Rd, Ra, n: the low n bits, zero- (.U32) or (hookshot) sign-extended (bit 73): BFE at 0
            if ((insn.words[2] & ~0x200U) || insn.words[1] > 32 || dest >= 255)
                return fail(index, "native sign extend mode");
            emit(base(0x3800000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{insn.words[1] << 8} << 20) | (u64{insn.Bits(73, 1)} << 48));
            break;
        }
        case 0x299: { // USHF by a uniform amount, one register shifted (SHL/SHR, bit 75 wraps).
            const u32 dst = uniform_register(insn.Bits(16, 8)), low = uniform_register(insn.Bits(24, 8));
            const u32 shift = uniform_register(insn.Bits(32, 8)), high = uniform_register(insn.Bits(64, 8));
            const bool right = insn.Bits(76, 1) != 0, high_result = insn.Bits(80, 1) != 0;
            if (dst == ~0U || dst == 255 || low == ~0U || shift == ~0U || high == ~0U ||
                (native_predicate != 7 && !guarded_uniform) || (insn.words[2] & ~0x00011effU) != 0x08000000U)
                return fail(index, "native uniform register shift mode");
            // (hookshot) a 64-bit type, or both words: the general funnel shift (OoT's mesh programs: USHF.R.U64)
            if (insn.Bits(73, 2) != 3 || !((high_result && low == 255) || (!high_result && high == 255))) {
                funnel(dst, low, high, true, shift, insn.Bits(73, 2), right, high_result, insn.Bits(75, 1) != 0);
                break;
            }
            u32 source;
            if (high_result && low == 255) source = high;        // (high:0) >> or << s, high word
            else if (!high_result && high == 255) source = low;  // (0:low) >> or << s, low word
            else return fail(index, "native uniform funnel shift");
            emit((right ? 0x5c28000000000000ULL : 0x5c48000000000000ULL) | predicate | dst |
                 (u64{source} << 8) | (u64{shift} << 20) | (u64{insn.Bits(75, 1)} << 39));
            break;
        }
        case 0x499: { // (hookshot) USHF URd, URa, URc (shift), imm (the high word): as 0x419, uniform
            const u32 d = uniform_register(dest), lo = uniform_register(insn.Bits(24, 8));
            const u32 shift = uniform_register(insn.Bits(64, 8));
            if (d == ~0U || d == 255 || lo == ~0U || shift == ~0U || (native_predicate != 7 && !guarded_uniform) ||
                (insn.words[2] & ~(0x00011effU | 0x08000000U)))
                return fail(index, "native uniform funnel shift immediate mode");
            mov_imm(250, insn.words[1]);
            funnel(d, lo, 250, true, shift, insn.Bits(73, 2), insn.Bits(76, 1) != 0, insn.Bits(80, 1) != 0,
                   insn.Bits(75, 1) != 0);
            break;
        }
        case 0x419: { // SHF.L.W.U32.HI Rd, RZ, Rb, imm (high word of imm:0 << Rb) = imm << (Rb & 31)
            if ((insn.Bits(24, 8) != 255 || (insn.words[2] & ~0xffU) != 0x00010e00) && !(insn.words[2] & ~0x00011effU)) {
                // (hookshot) the general form: the immediate is the high word, Rc (bits 64-71) the shift
                mov_imm(250, insn.words[1]);
                funnel(static_cast<u32>(dest), insn.Bits(24, 8), 250, true, insn.Bits(64, 8), insn.Bits(73, 2),
                       insn.Bits(76, 1) != 0, insn.Bits(80, 1) != 0, insn.Bits(75, 1) != 0);
                break;
            }
            if (insn.Bits(24, 8) != 255 || (insn.words[2] & ~0xffU) != 0x00010e00)
                return fail(index, "native funnel shift immediate mode");
            mov_imm(250, insn.words[1]);
            emit(base(0x5c48000000000000ULL) | (u64{250} << 8) | (u64{insn.Bits(64, 8)} << 20) |
                 (u64{1} << 39));
            break;
        }
        case 0x805: { // CS2R SRZ
            if (insn.Bits(72, 8) != 255 || !insn.Bits(80, 1) || dest >= 249) return fail(index, "native special-register read");
            mov_imm(static_cast<u32>(dest), 0);
            mov_imm(static_cast<u32>(dest) + 1, 0);
            break;
        }
        case 0x306: case 0xd06: { // I2F.F32.U8, round-to-nearest (0xd06: uniform source)
            const u32 source = opcode == 0xd06 ? uniform_register(insn.Bits(32, 8)) : insn.Bits(32, 8);
            const u32 form16 = insn.words[2] & ~(opcode == 0xd06 ? 0x08000400U : 0x400U);
            if (form16 == 0x00101000 && !(insn.words[1] & 0xcfffff00U) && source != ~0U) {
                // I2F.U16 / S16 Rd, Rs(.H1): Maxwell's 16-bit source format, byte selector 0 or 2, bit 74: signed
                // (hookshot: S16 and the uniform form stopped 240 of the game's programs, models and skinned meshes among them,
                // from translating at all).
                emit(base(0x5cb8000000000000ULL) | (u64{2} << 8) | (u64{1} << 10) | (u64{insn.Bits(74, 1)} << 13) | (u64{source} << 20) |
                     (u64{insn.Bits(60, 2) * 2} << 41));
                break;
            }
            // I2F.U8 Rd, Rs.B0-B3: the byte selector (bits 60-61) as Maxwell's (41-42).
            if (insn.words[2] != (opcode == 0xd06 ? 0x08001000U : 0x1000U) ||
                (insn.words[1] & 0xcfffff00U) || source == ~0U)
                return fail(index, "native byte-to-float conversion mode");
            emit(base(0x5cb8000000000000ULL) | (u64{2} << 8) | (u64{source} << 20) |
                 (u64{insn.Bits(60, 2)} << 41));
            break;
        }
        case 0x245: case 0xa45: case 0xc45: { // I2FP.F32.U32
            // Bits 78-79: rounding (RN/RM/RP/RZ), as Maxwell I2F's bits 39-40.
            if ((insn.words[2] & ~(0x400U | 0xc000U | (opcode == 0xc45 ? 0x08000000U : 0U))) != 0x00201000) return fail(index, "native packed integer-to-float format");
            const u32 src = operand(1, false);
            emit(base(0x5cb8000000000000ULL) | (u64{2} << 8) | (u64{2} << 10) | (u64{insn.Bits(74, 1)} << 13) | (u64{src} << 20) |
                 (u64{insn.Bits(78, 2)} << 39));
            break;
        }
        case 0x207: case 0x807: case 0xa07: case 0xc07:   // (hookshot: 0xc07/0xc08, a uniform b)
        case 0x208: case 0x808: case 0xa08: case 0xc08: { // Bitwise SEL / FSEL
            // FSEL applies the floating operand modifiers (negate/absolute) before selecting.
            const bool floating = (opcode & 0x1ff) == 0x008;
            const u32 a = operand(0, floating), b = operand(1, floating);
            emit(base(0x5ca0000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
            break;
        }
        case 0x20a: case 0x80a: case 0xa0a: case 0xc0a: { // FSET.BF, combined with Pc (bits 87-90) by bits 74-75 (0xc0a: uniform b)
            if (insn.Bits(64, 8) || insn.Bits(74, 2) == 3 || insn.Bits(81, 6) || (opcode != 0xc0a && insn.Bits(91, 1)))
                return fail(index, "native floating comparison output mode");
            const u32 a = operand(0), b = operand(1);
            emit(base(0x5800000000000000ULL) | (u64{a} << 8) | (u64{b} << 20) |
                 (u64{insn.Bits(87, 4)} << 39) | (u64{insn.Bits(74, 2)} << 45) |
                 (u64{insn.Bits(76, 4)} << 48) | (u64{1} << 52) | (u64{insn.Bits(80, 1)} << 55));
            break;
        }
        case 0x234: case 0x434: { // HSETP2 with a register (B swizzle 60-61, abs 62, neg 63) or an
                      // immediate half pair: A swizzle 74-75 (neg 72, abs 73),
                      // combine 69-70, H_AND 71, compare 76-79, FTZ 80, P/Q 81-86, input 87-90.
            u64 source = u64{250} << 20;
            if (opcode == 0x234) {
                if ((insn.words[1] & 0x0fffff00U) || insn.Bits(64, 5))
                    return fail(index, "native half comparison mode");
                source = (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(60, 2)} << 28) |
                         (u64{insn.Bits(62, 1)} << 30) | (u64{insn.Bits(63, 1)} << 31);
            } else {
                mov_imm(250, insn.words[1]);
            }
            emit(0x5d20000000000000ULL | predicate | insn.Bits(84, 3) | (u64{insn.Bits(81, 3)} << 3) |
                 (u64{insn.Bits(80, 1)} << 6) | (u64{insn.Bits(24, 8)} << 8) | source |
                 (u64{insn.Bits(76, 4)} << 35) | (u64{insn.Bits(87, 3)} << 39) |
                 (u64{insn.Bits(90, 1)} << 42) | (u64{insn.Bits(72, 1)} << 43) |
                 (u64{insn.Bits(73, 1)} << 44) | (u64{insn.Bits(69, 2)} << 45) |
                 (u64{insn.Bits(74, 2)} << 47) | (u64{insn.Bits(71, 1)} << 49));
            break;
        }
        case 0x20b: case 0x80b: case 0xa0b: case 0xc0b: // FSETP
        case 0x20c: case 0x80c: case 0xa0c: case 0xc0c: { // ISETP
            const bool floating = (opcode & 0x1ff) == 0xb;
            if (!floating && insn.Bits(72, 1)) {
                // ISETP.EX (bit 72): the high-word half of a 64-bit compare. The low words'
                // result comes in as the predicate at bits 68-70 (negated by 71), e.g.
                // ISETP.NE P0 = lo != 0, then ISETP.NE.EX P0 = (hi != 0) OR P0. EQ: high equal
                // AND low; NE: high different OR low; the orderings: high strictly ordered OR
                // (high equal AND low). One or two Maxwell ISETPs into the destination; only
                // the plain form (no second destination, AND with PT) is handled.
                const u32 compare = insn.Bits(76, 3);
                const u32 destination = insn.Bits(81, 3);
                const u32 low = insn.Bits(68, 4);
                // (hookshot) an accumulator predicate (bits 87-90, not PT) or boolean op (74-75): the
                // compare as below, then PSETP dest = (dest op accumulator) AND PT.
                const u32 accumulator = insn.Bits(87, 4), bop = insn.Bits(74, 2);
                if (insn.Bits(84, 3) != 7 || bop > 2 || compare == 0 || compare == 7 ||
                    (destination != 7 && insn.Bits(12, 3) == destination))
                    return fail(index, "native extended integer comparison mode");
                // (an accumulator that is the destination: the compare into a scratch predicate, saved
                // around it as MATCH does, then combined into the destination)
                const bool combined = destination != 7 && ((accumulator & 7) != 7 || (accumulator & 8) || bop != 0);
                u32 out = destination;
                if (combined && (accumulator & 7) == destination) {
                    out = 6;
                    while (out == destination || out == (low & 7) || out == (accumulator & 7) || out == insn.Bits(12, 3)) out--;
                }
                if (out != destination) save_predicate(1U << out);
                const u32 a = operand(0, false), b = operand(1, false);
                const auto isetp = [&](u32 op, u32 input, u32 combine) {
                    return 0x5b60000000000000ULL | predicate | 7 | (u64{out} << 3) |
                           (u64{a} << 8) | (u64{b} << 20) | (u64{input & 7} << 39) |
                           (u64{input >> 3} << 42) | (u64{combine} << 45) |
                           (u64{insn.Bits(73, 1)} << 48) | (u64{op} << 49);
                };
                constexpr u32 And = 0, Or = 1, Lt = 1, Eq = 2, Gt = 4, Ne = 5;
                if (compare == Eq) {
                    emit(isetp(Eq, low, And));
                } else if (compare == Ne) {
                    emit(isetp(Ne, low, Or));
                } else {
                    emit(isetp(Eq, low, And));
                    emit(isetp(compare <= 3 ? Lt : Gt, out, Or));
                }
                if (combined)
                    emit(0x5090000000000000ULL | predicate | (u64{destination} << 3) | 7 | (u64{out} << 12) |
                         (u64{bop} << 24) | (u64{accumulator} << 29) | (u64{7} << 39));
                if (out != destination) restore_predicate(1U << out);
                break;
            }
            const u32 a = operand(0, floating), b = operand(1, floating);
            const u32 combine = insn.Bits(74, 2);
            if (combine > 2) return fail(index, "native predicate combination");
            u64 out = (floating ? 0x5bb0000000000000ULL : 0x5b60000000000000ULL) | predicate |
                insn.Bits(84, 3) | (u64{insn.Bits(81, 3)} << 3) | (u64{a} << 8) | (u64{b} << 20) |
                (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42) | (u64{combine} << 45);
            if (floating) out |= (u64{insn.Bits(80, 1)} << 47) | (u64{insn.Bits(76, 4)} << 48);
            else out |= (u64{insn.Bits(73, 1)} << 48) | (u64{insn.Bits(76, 3)} << 49);
            emit(out);
            break;
        }
        case 0x212: case 0x812: case 0xa12: case 0xc12: { // LOP3.LUT, any table, optional P = (result != 0)
            const u32 lut = insn.Bits(72, 8);
            const u32 pred_target = insn.Bits(81, 3);
            if (insn.Bits(80, 1) || insn.Bits(87, 4) != 15 ||
                (opcode == 0x212 && insn.Bits(40, 24)) || (opcode == 0xa12 && insn.Bits(62, 2)))
                return fail(index, "native LUT/predicate mode");
            u32 b = 255;
            if (opcode == 0x212) {
                b = insn.Bits(32, 8);
            } else if (opcode == 0x812) {
                mov_imm(251, insn.words[1]);
                b = 251;
            } else if (opcode == 0xc12) {
                b = uniform_register(insn.Bits(32, 8));
                if (b == ~0U) return fail(index, "native uniform logic source");
            } else {
                b = operand(1, false);
            }
            const u32 a = insn.Bits(24, 8), c = insn.Bits(64, 8);
            emit(0x5be0000000000000ULL | predicate | dest | (u64{a} << 8) | (u64{b} << 20) |
                 (u64{lut} << 28) | (u64{pred_target == 7 ? 0U : 3U} << 36) | (u64{c} << 39) |
                 (u64{pred_target} << 48));
            break;
        }
        case 0x213: case 0xa13: case 0xc13: { // IABS: max(x, 0 - x), signed.
            if (insn.words[2] & ~(opcode == 0xc13 ? 0x08000000U : 0U))
                return fail(index, "native integer absolute mode");
            const u32 source = operand(1, false);
            emit(0x5c10000000000000ULL | predicate | 250 | (u64{255} << 8) | (u64{source} << 20) |
                 (u64{1} << 48));
            emit(base(0x5c20000000000000ULL) | (u64{source} << 8) | (u64{250} << 20) |
                 (u64{15} << 39) | (u64{1} << 48));
            break;
        }
        case 0x217: case 0x817: case 0xa17: case 0xc17: { // IMNMX register, immediate, cbuf or uniform operand.
            if ((insn.words[2] & ~(0x04000200U | (opcode == 0xc17 ? 0x08000000U : 0U))) != 0x03800000 ||
                (opcode != 0xa17 && opcode != 0x817 && insn.words[1] > 255U) ||
                (opcode == 0xa17 && (!valid_cbuf() || insn.Bits(62, 2))))
                return fail(index, "native integer min/max mode");
            // An immediate is a signed 20-bit value: 19 bits at 20, the sign in bit 56. (hookshot) A wider one
            // (3 of OoT's vertex programs clamp to 0x7f0004) goes through R251 and the register form
            const bool wide = opcode == 0x817 && insn.words[1] > 0x7ffffU && insn.words[1] < 0xfff80000U;
            if (wide) mov_imm(251, insn.words[1]);
            const bool immediate = opcode == 0x817 && !wide;
            const u32 source = opcode == 0xc17   ? uniform_register(insn.words[1])
                               : opcode == 0xa17 ? operand(1, false)
                               : wide            ? 251U
                               : opcode == 0x817 ? insn.words[1] & 0x7ffffU
                                                 : insn.words[1];
            if (source == ~0U) return fail(index, "native uniform min/max operand");
            emit(base(immediate ? 0x3820000000000000ULL : 0x5c20000000000000ULL) |
                 (u64{insn.Bits(24, 8)} << 8) | (u64{source} << 20) |
                 (u64{immediate ? insn.words[1] >> 31 : 0U} << 56) |
                 (u64{insn.Bits(87, 4)} << 39) | (u64{insn.Bits(73, 1)} << 48));
            break;
        }
        case 0x819: { // SHF.HI, immediate shift
            const bool is_signed = insn.Bits(73, 2) == 2;
            if (!insn.Bits(80, 1) && !insn.Bits(76, 1) && insn.Bits(64, 8) == 255 &&
                insn.Bits(73, 2) == 3 && !insn.Bits(75, 1) && insn.words[1] <= 32) {
                emit(base(0x3848000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{insn.words[1]} << 20));
                break;
            }
            if (!insn.Bits(80, 1) && insn.Bits(76, 1) && insn.Bits(64, 8) == 255 &&
                (insn.Bits(73, 2) == 3 || is_signed) && !insn.Bits(75, 1) && insn.words[1] <= 32) {
                // SHF.R low word with a zero high word: a logical right shift.
                emit(base(0x3828000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{insn.words[1]} << 20));
                break;
            }
            if (((insn.Bits(73, 2) != 3 && !is_signed) || insn.Bits(75, 1) || !insn.Bits(80, 1) || insn.words[1] > 32) &&
                !(insn.words[2] & ~0x00011effU) && insn.words[1] < 64) {   // (hookshot) the general funnel shift
                if (funnel(static_cast<u32>(dest), insn.Bits(24, 8), insn.Bits(64, 8), false, insn.words[1],
                           insn.Bits(73, 2), insn.Bits(76, 1) != 0, insn.Bits(80, 1) != 0, insn.Bits(75, 1) != 0))
                    break;
            }
            if ((insn.Bits(73, 2) != 3 && !is_signed) || insn.Bits(75, 1) || !insn.Bits(80, 1) || insn.words[1] > 32) return fail(index, "native funnel-shift mode");
            const u32 low = insn.Bits(24, 8), high = insn.Bits(64, 8), shift = insn.words[1];
            if (insn.Bits(76, 1)) {
                if (low != 255) return fail(index, "native nonzero low right-shift word");
                emit(base(0x3828000000000000ULL) | (u64{high} << 8) | (u64{shift} << 20) |
                     (u64{is_signed} << 48));
            } else {
                if (is_signed) return fail(index, "native signed left funnel shift");
                emit(base(0x36f8000000000000ULL) | (u64{low} << 8) | (u64{high} << 39) | (u64{shift} << 20));
            }
            break;
        }
        case 0xa11: { // LEA with constant base and carry predicate.
            if ((insn.words[2] & ~(0x07800000U | 0x0000f800U)) == 0x000f06ffU && !insn.Bits(62, 2) &&
                insn.Bits(75, 5) != 0) {
                // LEA.HI.X.SX32, no high source: (sign-extended Ra << shift) >> 32, plus the
                // constant high word and the carry predicate (bits 87-90).
                const u32 b = operand(1, false);
                emit(0x3828000000000000ULL | predicate | 250 | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{32 - insn.Bits(75, 5)} << 20) | (u64{1} << 48));
                mov_imm(252, 1);
                emit(0x5ca0000000000000ULL | predicate | 252 | (u64{252} << 8) | (u64{255} << 20) |
                     (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{b} << 20));
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{252} << 20));
                break;
            }
            if ((insn.words[2] & ~(0x07800000U | 0x0000f800U | 0x00000200U)) == 0x000f00ffU && !insn.Bits(62, 2) &&
                insn.Bits(75, 5) != 0) {
                // (hookshot) LEA.HI Rd, Ra, c[..], RZ, s with no .X and no carry out: the constant plus the high
                // word of (Ra << s), Ra shifted right by 32 - s (arithmetic with .SX32, bit 73), as the register
                // form's high word (0x211). 40 of OoT's vertex programs stopped here ("constant LEA carry mode")
                const u32 b = operand(1, false);
                emit(0x3828000000000000ULL | predicate | 250 | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{32 - insn.Bits(75, 5)} << 20) | (u64{insn.Bits(73, 1)} << 48));
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{b} << 20));
                break;
            }
            if (insn.Bits(72, 1) || insn.Bits(74, 1) || insn.Bits(80, 1) ||
                insn.Bits(81, 3) == 7 || insn.Bits(62, 2))
                return fail(index, "native constant LEA carry mode");
            const u32 b = operand(1, false);
            emit(base(0x5c18000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                (u64{b} << 20) | (u64{insn.Bits(75, 5)} << 39));
            emit(0x5b60000000000000ULL | predicate | 7 | (u64{insn.Bits(81, 3)} << 3) |
                (dest << 8) | (u64{b} << 20) | (u64{7} << 39) | (u64{1} << 49));
            break;
        }
        case 0x211: case 0x811: case 0xc11: { // LEA low word (0xc11: uniform Rb)
            if (insn.Bits(80, 1)) {
                // LEA.HI Rd, Ra, Rb (or an immediate, 0x811), Rc, s: Rb + high word of (Rc:Ra << s);
                // bit 73 sign-extends Ra (Rc is RZ) and .X (bit 74) adds the carry predicate in bits 87-90.
                const bool extended = insn.Bits(74, 1) != 0;
                const u32 high = insn.Bits(64, 8);
                // (hookshot) bit 63 negates Rb, as in the low form (Maxwell IADD's bit 48); not with .X
                const bool negate_b = opcode == 0x211 && insn.Bits(63, 1);
                if (insn.Bits(72, 1) || insn.Bits(81, 3) != 7 ||
                    (high != 255 && insn.Bits(73, 1)) || (opcode == 0x211 && insn.Bits(62, 1)) ||
                    (negate_b && extended) || (opcode == 0xc11 && (insn.words[1] & ~0x3fU)))
                    return fail(index, "native high effective address mode");
                u32 b = opcode == 0xc11 ? uniform_register(insn.Bits(32, 8)) : insn.Bits(32, 8);
                if (b == ~0U) return fail(index, "native uniform LEA register");
                if (opcode == 0x811) {
                    b = insn.words[1] ? 251U : 255U;
                    if (b == 251) mov_imm(251, insn.words[1]);
                }
                const u64 shift = (u64{32 - insn.Bits(75, 5)} << 20) | (u64{insn.Bits(73, 1)} << 48);
                // The high word is a right shift of Ra, or with Rc a left funnel shift (SHF.L).
                const auto high_word = [&](u64 target) {
                    if (high == 255) emit(target | 0x3828000000000000ULL | (u64{insn.Bits(24, 8)} << 8) | shift);
                    else emit(target | 0x36f8000000000000ULL | (u64{insn.Bits(24, 8)} << 8) | (u64{high} << 39) |
                              (u64{insn.Bits(75, 5)} << 20));
                };
                if (b == 255 && !extended) {
                    high_word(base(0));
                } else {
                    high_word(predicate | 250);
                    if (extended) {
                        mov_imm(252, 1);
                        emit(0x5ca0000000000000ULL | predicate | 252 | (u64{252} << 8) | (u64{255} << 20) |
                             (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                        emit(0x5c10000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{252} << 20));
                    }
                    emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{b} << 20) | (u64{negate_b} << 48));
                }
                break;
            }
            // A carry out (bits 81-83) is the low half of a 64-bit address; IADD3.X adds it next.
            const u32 carry_out = insn.Bits(81, 3);
            if (insn.Bits(72, 1) || insn.Bits(74, 1) || insn.Bits(80, 1) ||
                (carry_out != 7 && ((opcode != 0x811 && insn.Bits(63, 1)) ||
                                    (rewritten_uniform && (opcode == 0xc11 ||
                                                           (guarded_uniform && 6 - carry_out == guard_slot))))) ||
                (opcode == 0x211 && insn.Bits(62, 1)) || (opcode == 0xc11 && (insn.words[1] & ~0x8000003fU)))
                return fail(index, "native LEA carry/high mode");
            // 0x211 bit 63 negates Rb (LEA Rd, Ra, -Rb, s), as Maxwell ISCADD's bit 48.
            u32 b = opcode == 0xc11 ? uniform_register(insn.Bits(32, 8)) : insn.Bits(32, 8);
            if (b == ~0U) return fail(index, "native uniform LEA register");
            if (opcode == 0x811) { mov_imm(251, insn.words[1]); b = 251; }
            if (carry_out != 7) {
                // The sum in R250, carry = sum < Rb (unsigned), as IADD3's carry out; a uniform
                // predicate goes through P(6-n) into R239.
                const u32 target = rewritten_uniform ? 6 - carry_out : carry_out, mask = 1U << target;
                // (hookshot) guarded (a uniform guard swapped in, its own save in R253): the guest P(6-n) kept in R252,
                // and the UPn started from R239, so that a false guard leaves it as it was
                const u32 keep = guarded_uniform ? 252U : 253U;
                emit(0x5c18000000000000ULL | predicate | 250 | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{b} << 20) | (u64{insn.Bits(75, 5)} << 39));
                if (rewritten_uniform) emit(0x38e8000000070000ULL | keep | (u64{255} << 8) | (u64{mask} << 20));
                if (rewritten_uniform && guarded_uniform) emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{mask} << 20));
                emit(0x5b60000000000000ULL | predicate | 7 | (u64{target} << 3) | (u64{250} << 8) |
                     (u64{b} << 20) | (u64{7} << 39) | (u64{1} << 49));
                if (rewritten_uniform) {
                    emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{mask} << 20));
                    emit(0x38f0000000070000ULL | (u64{keep} << 8) | (u64{mask} << 20));
                }
                emit(base(0x5c98000000000000ULL) | (u64{15} << 39) | (u64{250} << 20));
                break;
            }
            emit(base(0x5c18000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                 (u64{b} << 20) | (u64{insn.Bits(75, 5)} << 39) |
                 (u64{opcode != 0x811 ? insn.Bits(63, 1) : 0U} << 48));
            break;
        }
        case 0x210: case 0x810: case 0xa10: case 0xc10: { // IADD3 with zero third source and no carry outputs
            // 64-bit adds: IADD3 with a carry-out predicate (bits 81-83), then IADD3.X (bit 74) with
            // a carry-in predicate (bits 87-89, negate 90). Uniform forms keep UPn in R239 bit 6-n.
            if ((opcode == 0x210 || opcode == 0x810) && insn.Bits(64, 8) == 255 && !insn.Bits(73, 1) &&
                !insn.Bits(75, 2) && insn.Bits(84, 3) == 7 && insn.Bits(77, 4) == 15 && !insn.Bits(72, 1) &&
                (insn.Bits(81, 3) != 7 || insn.Bits(74, 1)) && (opcode == 0x810 || !insn.Bits(63, 1))) {
                const bool carry_in = insn.Bits(74, 1) != 0;
                const u32 carry_out = insn.Bits(81, 3);
                const u32 a = insn.Bits(24, 8);
                // (hookshot) any UPn (it was UP0/UP1): its slot P(6-n) is saved and restored around the op; only a
                // slot that is also the uniform guard's could not be
                const bool slot_conflict = guarded_uniform &&
                    ((carry_out != 7 && 6 - carry_out == guard_slot) ||
                     (carry_in && insn.Bits(87, 3) != 7 && 6 - insn.Bits(87, 3) == guard_slot));
                if ((carry_in && carry_out != 7) || (opcode == 0x210 && (insn.words[1] & ~0xffU)) ||
                    (rewritten_uniform && carry_out != 7 && carry_out > 6) ||
                    (rewritten_uniform && carry_in && insn.Bits(87, 3) > 6 && insn.Bits(87, 3) != 7) || slot_conflict)
                    return fail(index, "native integer carry mode");
                if (opcode == 0x810) {
                    emit(0x1c00000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{insn.words[1]} << 20));
                } else {
                    emit(0x5c10000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{insn.Bits(32, 8)} << 20));
                }
                const auto save = [&](u32 mask) { emit(0x38e8000000070000ULL | 253 | (u64{255} << 8) | (u64{mask} << 20)); };
                const auto restore = [&](u32 mask) { emit(0x38f0000000070000ULL | (u64{253} << 8) | (u64{mask} << 20)); };
                if (!carry_in) {
                    // carry = (a + b) < a, unsigned
                    const u32 target = rewritten_uniform ? 6 - carry_out : carry_out;
                    const u32 mask = 1U << target;
                    if (rewritten_uniform) save(mask);
                    emit(0x5b60000000000000ULL | predicate | 7 | (u64{target} << 3) | (u64{250} << 8) |
                         (u64{a} << 20) | (u64{7} << 39) | (u64{1} << 49));
                    if (rewritten_uniform) {
                        emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{mask} << 20));
                        restore(mask);
                    }
                    emit(base(0x5c98000000000000ULL) | (u64{15} << 39) | (u64{250} << 20));
                    break;
                }
                const u32 condition = insn.Bits(87, 3);
                const u32 source = rewritten_uniform && condition != 7 ? 6 - condition : condition;
                const u32 mask = 1U << source;
                mov_imm(252, 1);
                if (rewritten_uniform && condition != 7) {
                    save(mask);
                    emit(0x38f0000000070000ULL | (u64{239} << 8) | (u64{mask} << 20));
                }
                emit(0x5ca0000000000000ULL | predicate | 251 | (u64{252} << 8) | (u64{255} << 20) |
                     (u64{source} << 39) | (u64{insn.Bits(90, 1)} << 42));
                if (rewritten_uniform && condition != 7) restore(mask);
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{251} << 20));
                break;
            }
            // Three sources, negations or two carries: the sum builds up in R253 (.CC) with the carry
            // count in R254 (.X). Pu/Pv (bits 81/84) are set when the count reaches 1/2, and IADD3.X
            // adds both carry-ins (bits 87, 77). A negated source is ~x, plus 1 unless extended.
            const bool extended = insn.Bits(74, 1) != 0;
            const bool negate_a = insn.Bits(72, 1), negate_b = opcode != 0x810 && insn.Bits(63, 1), negate_c = insn.Bits(75, 1);
            if ((extended || insn.Bits(81, 6) != 0x3f) &&
                (insn.Bits(64, 8) != 255 || insn.Bits(84, 3) != 7 || negate_a || negate_b || negate_c ||
                 (extended && insn.Bits(77, 4) != 15))) {
                const u32 first_output = insn.Bits(81, 3), second_output = insn.Bits(84, 3);
                if (rewritten_uniform || guarded_uniform || insn.Bits(73, 1) || insn.Bits(76, 1) ||
                    (first_output != 7 && first_output == second_output) ||
                    (!extended && (insn.Bits(77, 4) != 15 || insn.Bits(87, 4) != 15)) ||
                    (opcode == 0x210 && (insn.words[1] & ~0x800000ffU)) || (opcode == 0xa10 && insn.Bits(62, 1)) ||
                    (opcode == 0xc10 && (insn.words[1] & ~0x8000003fU)))
                    return fail(index, "native three-input carry mode");
                u32 a = insn.Bits(24, 8), b = operand(1, false), c = insn.Bits(64, 8);
                const auto invert = [&](u32& source, u32 target) {
                    emit(0x0400000000000000ULL | predicate | target | (u64{source} << 8) | (u64{0xffffffffU} << 20) | (u64{2} << 53));
                    source = target;
                };
                if (negate_a) invert(a, 250);
                if (negate_b) invert(b, 251);
                if (negate_c) invert(c, 252);
                const auto accumulate = [&](u32 source) {
                    emit(0x5c10000000000000ULL | predicate | 253 | (u64{253} << 8) | (u64{source} << 20) | (u64{1} << 47));
                    emit(0x5c10000000000000ULL | predicate | 254 | (u64{254} << 8) | (u64{255} << 20) | (u64{1} << 43));
                };
                emit(0x5c10000000000000ULL | predicate | 253 | (u64{a} << 8) | (u64{b} << 20) | (u64{1} << 47));
                emit(0x5c10000000000000ULL | predicate | 254 | (u64{255} << 8) | (u64{255} << 20) | (u64{1} << 43));
                accumulate(c);
                if (extended) {
                    for (const u32 carry : {insn.Bits(87, 4), insn.Bits(77, 4)}) {
                        if (carry == 15) continue;
                        mov_imm(250, 1);
                        if (carry != 7)
                            emit(0x5ca0000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                                 (u64{carry & 7} << 39) | (u64{carry >> 3} << 42));
                        accumulate(250);
                    }
                } else if (const u32 ones = u32{negate_a} + u32{negate_b} + u32{negate_c}) {
                    mov_imm(250, ones);
                    accumulate(250);
                }
                if (dest != 255) emit(base(0x5c98000000000000ULL) | (u64{253} << 20) | (u64{15} << 39));
                // An output that is also the guard predicate is written last.
                const bool guard_first = (native_predicate & 7) != 7 && first_output == (native_predicate & 7);
                for (const u32 output : {guard_first ? 1U : 0U, guard_first ? 0U : 1U}) {
                    const u32 target = output ? second_output : first_output;
                    if (target == 7) continue;
                    mov_imm(250, output + 1);
                    emit(0x5b60000000000000ULL | predicate | 7 | (u64{target} << 3) | (u64{254} << 8) |
                         (u64{250} << 20) | (u64{7} << 39) | (u64{6} << 49));
                }
                break;
            }
            if ((opcode == 0xa10 || opcode == 0xc10) && insn.Bits(81, 3) != 7) { // cbuf or uniform b
                if (insn.Bits(64, 8) != 255 || insn.Bits(73, 4) || insn.Bits(84, 7) != 0x7f ||
                    insn.Bits(77, 4) != 15 || insn.Bits(63, 1) || insn.Bits(72, 1) ||
                    (opcode == 0xc10 && (insn.words[1] & ~0x3fU)))
                    return fail(index, "native constant add carry mode");
                const u32 b = operand(1, false);
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{b} << 20));
                emit(0x5b60000000000000ULL | predicate | 7 | (u64{insn.Bits(81, 3)} << 3) |
                    (dest << 8) | (u64{b} << 20) | (u64{7} << 39) | (u64{1} << 49));
                break;
            }
            if ((opcode == 0xa10 || opcode == 0xc10) && insn.Bits(74, 1)) { // IADD3.X Ra + cbuf or UR + carry predicate
                if ((insn.words[2] & ~(0x07800000U | (opcode == 0xc10 ? 0x08000000U : 0U))) != 0x007fe4ffU ||
                    (opcode == 0xc10 && (insn.words[1] & ~0x3fU)))
                    return fail(index, "native extended constant add mode");
                const u32 a = insn.Bits(24, 8), b = operand(1, false);
                mov_imm(250, 1);
                emit(0x5ca0000000000000ULL | predicate | 250 | (u64{250} << 8) | (u64{255} << 20) |
                    (u64{insn.Bits(87, 3)} << 39) | (u64{insn.Bits(90, 1)} << 42));
                if (a != 255) emit(0x5c10000000000000ULL | predicate | 250 | (u64{a} << 8) | (u64{250} << 20));
                emit(base(0x5c10000000000000ULL) | (u64{b} << 8) | (u64{250} << 20));
                break;
            }
            // Third source, optionally negated (bit 75; 63 and 72 negate b and a).
            if (insn.Bits(64, 8) != 255 && (insn.Bits(73, 4) & ~4U) == 0 && insn.Bits(77, 7) == 0x7f && insn.Bits(84, 7) == 0x7f) {
                const u32 b = operand(1, false);
                emit(0x5c10000000000000ULL | predicate | 250 | (u64{insn.Bits(24, 8)} << 8) |
                    (u64{b} << 20) | (u64{insn.Bits(63, 1)} << 48) | (u64{insn.Bits(72, 1)} << 49));
                emit(base(0x5c10000000000000ULL) | (u64{250} << 8) | (u64{insn.Bits(64, 8)} << 20) |
                     (u64{insn.Bits(75, 1)} << 48));
                break;
            }
            if (insn.Bits(64, 8) != 255 || insn.Bits(73, 4) || insn.Bits(77, 7) != 0x7f || insn.Bits(84, 7) != 0x7f) return fail(index, "native integer carry/third source");
            if (opcode == 0xa10 || opcode == 0xc10) {
                if (opcode == 0xc10 && (insn.words[1] & ~0x8000003fU)) return fail(index, "native uniform add operand");
                const u32 b = operand(1, false);
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{b} << 20) |
                    (u64{insn.Bits(63, 1)} << 48) | (u64{insn.Bits(72, 1)} << 49));
            } else if (opcode == 0x210) {
                if (insn.words[1] & ~0x800000ffU) return fail(index, "native integer add register modifiers");
                emit(base(0x5c10000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) |
                     (u64{insn.Bits(32, 8)} << 20) | (u64{insn.Bits(63, 1)} << 48) | (u64{insn.Bits(72, 1)} << 49));
            } else {
                emit(base(0x1c00000000000000ULL) | (u64{insn.Bits(24, 8)} << 8) | (u64{insn.words[1]} << 20) | (u64{insn.Bits(72, 1)} << 56));
            }
            break;
        }
        case 0x949: case 0x958: { // Captured BRX / BRXU with absolute native-PC table values
            if (insn.Bits(12, 4) != 7 || (insn.words[2] & ~0x3ffffU) != (opcode == 0x958 ? 0x0b800000U : 0x03800000U))
                return fail(index, "native computed branch mode");
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            if (s64{index} * 16 + 16 + relative != 0)
                return fail(index, "native computed branch relative base");
            const s64 offset = -s64{address_of(static_cast<u32>(result.instructions.size()))} - 8;
            if (offset < -0x800000) return fail(index, "native computed branch range");
            const u32 source = opcode == 0x958 ? uniform_register(insn.Bits(24, 8)) : insn.Bits(24, 8);
            if (source == ~0U) return fail(index, "native uniform computed branch source");
            emit(0xe25000000000000fULL | predicate | (u64{source} << 8) |
                 ((static_cast<u64>(offset) & 0xffffff) << 20));
            computed_branch = true;
            break;
        }
        case 0x944: { // Verified CALL.REL.NOINC with R55 holding the return PC
            // A predicated NOINC forward transfer can terminate a loop at its
            // reconvergence point without a return-link setup or matching RET.
            if (native_predicate != 7 && insn.words[1] == 16 && insn.words[2] == 0x03c00000 &&
                index + 2 < code.size() && code[index + 1].Bits(0, 12) == 0x947) {
                relocations.push_back({static_cast<u32>(result.instructions.size()), index + 2});
                emit(0xe24000000000000fULL | predicate);
                break;
            }
            // (hookshot) the link's MOV (the return PC into Rlink) up to sixteen instructions back, those between
            // writing other registers and not branching (OoT's mesh programs set it ten instructions before)
            // (hookshot: up to 48: OoT's foliage mesh programs set it eighteen instructions before)
            u32 setup = index;
            for (u32 back = 1; back <= 48 && back <= index; ++back) {
                const Instruction& prior = code[index - back];
                if ((prior.words[0] & ~0x00ff0000U) == 0x00007802 && prior.Bits(16, 8) < 250 &&
                    prior.words[1] == (index + 1) * 16 && prior.words[2] == 0xf00) {
                    setup = index - back;
                    break;
                }
                const u32 prior_op = prior.Bits(0, 12);
                // (predicated ones may lie between: the clobber check below sees their writes)
                if (prior_op >= 0x940 && prior_op <= 0x95f) break;
            }
            // (hookshot) what a write may cover, with the wider window: register pairs and quads of the wide
            // multiplies, loads and texture fetches (their second destination at 64 too)
            const auto writes_link = [&](const Instruction& prior, u32 link_reg) {
                const u32 op = prior.Bits(0, 12), d = prior.Bits(16, 8);
                u32 span = 1;
                bool second = false;
                if (op == 0x225 || op == 0x625 || op == 0x825 || op == 0x805) span = 2;
                else if (op == 0x381 || op == 0x981 || op == 0x983 || op == 0x984 || op == 0x980) {
                    const u32 size = prior.Bits(73, 3);
                    span = size == 6 ? 4 : size == 5 ? 2 : 1;
                } else if ((op & 0x1f0) == 0x160) {
                    span = 4;
                    second = true;
                }
                const auto covers = [&](u32 r) { return r != 255 && link_reg >= r && link_reg < r + span; };
                return covers(d) || (second && covers(prior.Bits(64, 8)));
            };
            bool clobbered = setup == index;
            for (u32 k = setup + 1; !clobbered && k < index; ++k)
                clobbered = writes_link(code[k], code[setup].Bits(16, 8));
            if (main_exited || index == 0 || insn.words[0] != 0x7944 ||
                (insn.words[2] & ~0x3ffffU) != 0x03c00000 || clobbered)
                return fail(index, "native call linkage mode");
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16) return fail(index, "native call destination alignment");
            const u32 link = code[setup].Bits(16, 8);
            for (size_t call = 0; call < call_targets.size(); ++call) {
                if (call_targets[call] == target / 16 && call_links[call] != link)
                    return fail(index, "native inconsistent call linkage");
            }
            call_targets.push_back(static_cast<u32>(target / 16));
            call_links.push_back(link);
            relocations.push_back({static_cast<u32>(result.instructions.size()), target / 16});
            emit(0xe26000000007000fULL);
            break;
        }
        case 0x950: { // Verified RET.REL.NODEC epilogue restoring the call link
            const u32 return_reg = insn.Bits(24, 8);
            // The return register is the link itself, or the last write to it in this function
            // is a copy of the link (MOV or IMAD.MOV.U32 Rd, RZ, RZ, Rlink).
            const auto copies_link = [&](const Instruction& prior) {
                if (prior.Bits(16, 8) != return_reg || prior.Bits(12, 4) != 7) return false;
                if ((prior.words[0] & 0xff00ffffU) == 0x00007202U)
                    return prior.words[1] == *active_link && prior.words[2] == 0xf00;
                if ((prior.words[0] & 0xff00ffffU) == 0xff007224U)
                    return prior.words[1] == 0xff && prior.words[2] == (0x078e0000U | *active_link);
                return false;
            };
            bool linked = active_link && return_reg == *active_link;
            // (hookshot) the stores write no register: their bits 16-23 are no destination (OoT's mesh programs spill
            // between the copy and the RET)
            const auto is_store = [](u32 op) {
                return op == 0x385 || op == 0x386 || op == 0x387 || op == 0x388 || op == 0x985 || op == 0x986 ||
                       op == 0x987 || op == 0x988;
            };
            for (u32 prior = index; active_link && !linked && prior-- > active_entry;) {
                if (copies_link(code[prior])) linked = true;
                else if (!is_store(code[prior].Bits(0, 12)) && code[prior].Bits(16, 8) == return_reg) break;
            }
            if (!main_exited || !linked ||
                (insn.words[0] & 0x00ffffffU) != 0x00007950 || return_reg >= 249 ||
                (insn.words[2] & ~0x3ffffU) != 0x03c00000 || !barrier_tokens.empty())
                return fail(index, "native return linkage mode");
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            if (s64{index} * 16 + 16 + relative != 0)
                return fail(index, "native return relative base");
            emit(0xe32000000007000fULL);
            bool complete = !computed_branch;
            for (u32 target : call_targets) complete &= target <= index;
            if (complete) return finish(index);
            break;
        }
        case 0x945: case 0x941: case 0x942: case 0x948: // (+ WARPSYNC)
            // BSSY / BSYNC / BREAK only manage warp reconvergence: BSYNC waits for the other
            // threads and falls through, BREAK drops the thread from that wait. Eden runs threads
            // independently, so all three are no-ops (a SYNC would wrongly jump, and a branch
            // out of a lowered SSY region would leave its token on Eden's flow stack).
            break;
        case 0x947: { // BRA
            u64 branch_predicate = predicate;
            if (opcode == 0x947 && insn.Bits(87, 4) != 7) {
                if (insn.Bits(12, 4) != 7) {
                    relocations.push_back({static_cast<u32>(result.instructions.size()), index + 1});
                    emit(0xe24000000000000fULL | (u64{insn.Bits(12, 4) ^ 8U} << 16));
                }
                branch_predicate = u64{insn.Bits(87, 4)} << 16;
            } else if (insn.Bits(87, 4) != 7) {
                return fail(index, "native extra branch condition");
            }
            const u64 raw = insn.words[1] | (u64{insn.words[2] & 0x3ffff} << 32);
            const s64 relative = static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16) return fail(index, "native branch destination alignment");
            relocations.push_back({static_cast<u32>(result.instructions.size()), target / 16});
            emit(0xe24000000000000fULL | branch_predicate);
            // (hookshot) a loop that leaves only by predicated EXITs: the program ends at the closing self-branch after the
            // loop's unconditional jump back, which nothing falls into (one of OoT's compute programs, its EXITs all @P0)
            const auto unconditional_jump = [&](const Instruction& before) {
                const u32 op = before.Bits(0, 12);
                return before.Bits(12, 4) == 7 && ((op == 0x947 && before.Bits(87, 4) == 7) || op == 0x94d);
            };
            const bool after_jump = index > 0 && call_targets.empty() && unconditional_jump(code[index - 1]);
            if ((computed_branch || after_jump) && target == s64{index} * 16 && branch_predicate == (u64{7} << 16))
                return finish(index); // the closing self-branch
            break;
        }
        case 0x923: case 0x927: { // ISBERD / ISBEWR: task and mesh programs' stage buffer.
            // Ampere: Ra 24-31, offset 40-63, ISBERD dest 16-23, ISBEWR data 32-39; 75 = .32 (else
            // U8), 76-77 = 3 .ATTR (0 the map region), 78 = .SKEW, 79 = .O (the output buffer).
            // Kept as an nx2 pseudo instruction (Maxwell 0xefc0, free there) that the frontend
            // reads for the task or mesh stage: data/dest 0-7, Ra 8-15, offset 20-43, .32 44,
            // .ATTR 45, .SKEW 46, .O 47, write 48. Addresses are linear bytes in the region.
            const bool write = opcode == 0x927;
            const u32 region = insn.Bits(76, 2);
            const bool output = insn.Bits(79, 1) != 0;
            // (hookshot) a read may be of the output region (.O): the program reading back what it wrote (one of OoT's
            // task programs reads its payload so); a write is always to it
            if ((insn.words[2] & ~0xf800U) != 0 || (region != 0 && region != 3) || (write && !output) ||
                (!write && region != 3) || (!write && dest == 255))
                return fail(index, "native stage buffer access mode");
            emit(0xefc0000000000000ULL | predicate | (write ? u64{insn.Bits(32, 8)} : dest) |
                 (u64{insn.Bits(24, 8)} << 8) | (u64{insn.Bits(40, 24)} << 20) |
                 (u64{insn.Bits(75, 1)} << 44) | (u64{region == 3} << 45) |
                 (u64{insn.Bits(78, 1)} << 46) | (u64{output} << 47) | (u64{write} << 48));
            break;
        }
        case 0x94d: { // EXIT
            if (insn.Bits(84, 2)) {
                return fail(index, "native conditional/special EXIT");
            }
            if (insn.Bits(87, 4) != 7) {
                // @Pg EXIT Pc: exits when both hold; skip over a Pc-predicated EXIT unless Pg.
                if (native_predicate != 7) {
                    relocations.push_back({static_cast<u32>(result.instructions.size()), index + 1});
                    emit(0xe24000000000000fULL | (u64{native_predicate ^ 8U} << 16));
                }
                emit(0xe30000000000000fULL | (u64{insn.Bits(87, 4)} << 16));
                break;
            }
            result.instructions.push_back(0xe30000000000000fULL | predicate);
            if (insn.Bits(12, 4) != 7) break;
            if (!barrier_tokens.empty()) return fail(index, "native unclosed reconvergence scope");
            if (call_targets.empty()) {
                const bool pending_branch = std::any_of(relocations.begin(), relocations.end(),
                    [&](const Relocation& relocation) { return relocation.target > index; });
                if (!pending_branch && !computed_branch) return finish(index);
                break;
            }
            if (main_exited) {
                // A library function may end the shader instead of returning (Eden inlines
                // calls, so this is an ordinary exit at that point).
                bool complete = std::none_of(relocations.begin(), relocations.end(),
                    [&](const Relocation& relocation) { return relocation.target > index; });
                for (u32 target : call_targets) complete &= target <= index;
                if (complete && !computed_branch) return finish(index);
                break;
            }
            for (u32 target : call_targets) {
                if (target <= index) return fail(index, "native call target overlaps main program");
            }
            main_exited = true;
            break;
        }
        default:
            return fail(index, "unsupported native opcode");
        }
        if (!operand_error.empty()) return fail(index, operand_error);
        if (f16_components != 0) {
            // Each pair as F2FP.PACK_AB: the second value in the high half.
            const u64 pred = u64{insn.Bits(12, 4)} << 16;
            const auto pack = [&](u32 target, u32 low, u32 high) {
                result.instructions.push_back(0x5ca8000000000000ULL | pred | 250 | (u64{1} << 8) |
                                              (u64{2} << 10) | (u64{high} << 20));
                result.instructions.push_back(0x5ca8000000000000ULL | pred | 251 | (u64{1} << 8) |
                                              (u64{2} << 10) | (u64{low} << 20));
                result.instructions.push_back(0x5c18000000000000ULL | pred | target | (u64{250} << 8) |
                                              (u64{251} << 20) | (u64{16} << 39));
            };
            if (f16_dest != 255) pack(f16_dest, 248, f16_components > 1 ? 249U : 255U);
            if (f16_second != 255 && f16_components > 2)
                pack(f16_second, 246, f16_components > 3 ? 247U : 255U);
        }
        if (output_mask) {
            emit(0x38e8000000070000ULL | 239 | (u64{239} << 8) | (u64{output_mask} << 20));
            emit(0x38f0000000070000ULL | (u64{254} << 8) | (u64{output_mask} << 20));
        }
        if (guarded_uniform) restore_predicate(guard_mask);
        for (u32 r = 0; r < 255; ++r) result.used_reserved[r] = result.used_reserved[r] || lowering_used[r];
        if (any_high) {
            // A reserved number the lowering itself used here as well as the native high GPR: no telling them apart.
            // The scratch registers and R239 are named by the lowering's own words, so an instruction that lowered to
            // more than one word is taken to use them
            const size_t words = result.instructions.size() - words_before;
            bool conflict = (high_in_scratch || high_239) && words > 1;
            for (u32 r = 0; r < 255 && !conflict; ++r) conflict = (native_high[r] || marker_taken[r]) && lowering_used[r];
            if (conflict && !survey) return fail(index, "native program uses lowering scratch registers");
            RegisterRun runs[24];
            u32 nruns = 0;
            for (u32 r = 0; r < nrenamed; ++r) runs[nruns++] = renamed[r];
            for (u32 r = 0; r < 255; ++r) {
                if (!native_high[r]) continue;
                RegisterRun& last = runs[nruns ? nruns - 1 : 0];
                if (nruns > nrenamed && last.number + last.length == r && last.length < 4) last.length++;
                else if (nruns < 24) runs[nruns++] = {r, r, 1};
            }
            std::vector<u64> prefix;
            for (u32 i = 0; i < nruns; i += 2) prefix.push_back(RegisterMapPrefix(runs + i, (std::min)(2U, nruns - i)));
            std::vector<u64> words_out;
            words_out.reserve(words * (prefix.size() + 1));
            std::vector<u32> moved(words);   // (each word's new place, for the relocations made here)
            result.register_map_words += static_cast<u32>(prefix.size() * words);
            for (size_t w = 0; w < words; ++w) {
                words_out.insert(words_out.end(), prefix.begin(), prefix.end());
                moved[w] = static_cast<u32>(words_before + words_out.size());
                words_out.push_back(result.instructions[words_before + w]);
            }
            result.instructions.resize(words_before);
            result.instructions.insert(result.instructions.end(), words_out.begin(), words_out.end());
            for (size_t r = relocations_before; r < relocations.size(); ++r)
                relocations[r].output_index = moved[relocations[r].output_index - words_before];
        }
    }
    return fail(static_cast<u32>(code.size()), main_exited ? "native program lacks captured library return" : "native program has no unconditional EXIT");
}

inline LoweredProgram LowerToMaxwellRules(std::span<const Instruction> code);
// (hookshot) as before; a program that fails is tried again with the extended renaming (extended_renaming above)
inline LoweredProgram LowerToMaxwell(std::span<const Instruction> code) {
    extended_renaming = false;
    LoweredProgram lowered = LowerToMaxwellRules(code);
    if (lowered) return lowered;
    extended_renaming = true;
    LoweredProgram again = LowerToMaxwellRules(code);
    extended_renaming = false;
    return again ? again : lowered;
}

inline LoweredProgram LowerToMaxwellRules(std::span<const Instruction> code) {
    // Programs using uniform registers above UR27 are lowered again with mirrors for them.
    u32 high_uniforms = 0;
    std::array<bool, 256> avoid{};
    bool surveyed = false;
    for (;;) {
        LoweredProgram lowered = LowerToMaxwellWith(code, high_uniforms, avoid);
        if (!lowered && lowered.high_uniform_registers > high_uniforms) {
            high_uniforms = lowered.high_uniform_registers;
            continue;
        }
        // (hookshot) a renamed register's marker met a uniform register the lowering used later: the uniform numbers
        // the whole program uses first (a pass that goes on past those conflicts), then again
        if (!lowered && !surveyed && lowered.error == std::string_view{"native program uses lowering scratch registers"}) {
            surveyed = true;
            avoid = LowerToMaxwellWith(code, high_uniforms, {}, true).used_reserved;
            continue;
        }
        return lowered;
    }
}

} // namespace Shader::Ampere
