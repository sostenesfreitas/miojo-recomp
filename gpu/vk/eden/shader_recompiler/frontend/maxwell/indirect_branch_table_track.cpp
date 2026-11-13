// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <optional>

#include "common/common_types.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/indirect_branch_table_track.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/load_constant.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif

namespace Shader::Maxwell {
namespace {
union Encoding {
    u64 raw;
    BitField<0, 8, IR::Reg> dest_reg;
    BitField<8, 8, IR::Reg> src_reg;
    BitField<20, 19, u64> immediate;
    BitField<56, 1, u64> is_negative;
    BitField<20, 24, s64> brx_offset;
};

template <typename Callable>
std::optional<u64> Track(Environment& env, Location block_begin, Location& pos, Callable&& func) {
    while (pos >= block_begin) {
        const u64 insn{env.ReadInstruction(pos.Offset())};
        --pos;
        if (func(insn, Decode(insn))) {
            return insn;
        }
    }
    return std::nullopt;
}

std::optional<u64> TrackLDC(Environment& env, Location block_begin, Location& pos,
                            IR::Reg brx_reg) {
    return Track(env, block_begin, pos, [brx_reg](u64 insn, Opcode opcode) {
        const LDC::Encoding ldc{insn};
        return opcode == Opcode::LDC && ldc.dest_reg == brx_reg && ldc.size == LDC::Size::B32 &&
               ldc.mode == LDC::Mode::Default;
    });
}

std::optional<u64> TrackSHL(Environment& env, Location block_begin, Location& pos,
                            IR::Reg ldc_reg) {
    return Track(env, block_begin, pos, [ldc_reg](u64 insn, Opcode opcode) {
        const Encoding shl{insn};
        return opcode == Opcode::SHL_imm && shl.dest_reg == ldc_reg;
    });
}

std::optional<u64> TrackIMNMX(Environment& env, Location block_begin, Location& pos,
                              IR::Reg shl_reg) {
    return Track(env, block_begin, pos, [shl_reg](u64 insn, Opcode opcode) {
        const Encoding imnmx{insn};
        return opcode == Opcode::IMNMX_imm && imnmx.dest_reg == shl_reg;
    });
}
} // Anonymous namespace

std::optional<IndirectBranchTableInfo> TrackIndirectBranchTable(Environment& env, Location brx_pos,
                                                                Location block_begin) {
    const u64 brx_insn{env.ReadInstruction(brx_pos.Offset())};
    const Opcode brx_opcode{Decode(brx_insn)};
    if (brx_opcode != Opcode::BRX && brx_opcode != Opcode::JMX) {
        throw LogicError("Tracked instruction is not BRX or JMX");
    }
    const IR::Reg brx_reg{Encoding{brx_insn}.src_reg};
    const s32 brx_offset{static_cast<s32>(Encoding{brx_insn}.brx_offset)};

    Location pos{brx_pos};
    const std::optional<u64> ldc_insn{TrackLDC(env, block_begin, pos, brx_reg)};
    if (!ldc_insn) {
        return std::nullopt;
    }
    const LDC::Encoding ldc{*ldc_insn};
    const u32 cbuf_index{static_cast<u32>(ldc.index)};
    const u32 cbuf_offset{static_cast<u32>(static_cast<s32>(ldc.offset.Value()))};
    const IR::Reg ldc_reg{ldc.src_reg};

    const std::optional<u64> shl_insn{TrackSHL(env, block_begin, pos, ldc_reg)};
    if (!shl_insn) {
        return std::nullopt;
    }
    const Encoding shl{*shl_insn};
    const IR::Reg shl_reg{shl.src_reg};

    const Location after_shl{pos};
    const std::optional<u64> imnmx_insn{TrackIMNMX(env, block_begin, pos, shl_reg)};
    u32 max_index{};
    if (imnmx_insn) {
        const Encoding imnmx{*imnmx_insn};
        if (imnmx.is_negative != 0) {
            return std::nullopt;
        }
        max_index = static_cast<u32>(imnmx.immediate.Value());
    } else {
        // (amd-fix) NVN2 programs lowered to Maxwell clamp the index with ISETP + SEL against a register a MOV32I set
        // (SEL R, idx, Rbound, P), not with an IMNMX: OoT's 30 foliage mesh programs were rejected here
        // A bound read from a constant buffer (MOV from cbuf) is data, not code: OoT's 2 remaining foliage mesh
        // programs clamp so. The table is then measured by reading it until an entry is not a plausible target, and
        // capped by the bound when its constant is known at translation time.
        const auto table_count{[&](std::optional<u64> movc, u32 extra) -> u32 {
            const u32 t_index{static_cast<u32>(ldc.index)};
            const s64 base{Decode(brx_insn) == Opcode::JMX ? 0 : static_cast<s64>(brx_pos.Offset())};
            u32 count{0};
            for (u32 i = 0; i < 256; ++i) {
                const s32 rel{static_cast<s32>(env.ReadCbufValue(t_index, cbuf_offset + i * 4))};
                const s64 target{base + rel + brx_offset + 8};
                const bool ok{(rel & 7) == 0 && rel > -0x40000 && rel < 0x40000 &&
                              target >= static_cast<s64>(block_begin.Offset()) && target < 0x400000};
                if (!ok) break;
                ++count;
            }
            u32 bound{0};
            if (movc) {
                bound = env.ReadCbufValue(static_cast<u32>((*movc >> 34) & 0x1f),
                                          static_cast<u32>(((*movc >> 20) & 0x3fff) * 4));
                if (bound > 0 && bound + extra < count) count = bound + extra;
            }
            if (env_on("RX_VK_LOG_BRX")) {
                std::fprintf(stderr, "brx: cbuf-bounded table at %#x: c%u[%#x], %u entries (bound %u)\n",
                             static_cast<unsigned>(brx_pos.Offset()), t_index, cbuf_offset, count, bound);
            }
            return count;
        }};
        const auto find_movc{[&](Location from, IR::Reg reg) {
            return Track(env, block_begin, from, [reg](u64 insn, Opcode opcode) {
                return opcode == Opcode::MOV_cbuf && Encoding{insn}.dest_reg == reg;
            });
        }};
        Location at{after_shl};
        const std::optional<u64> sel_insn{Track(env, block_begin, at, [shl_reg](u64 insn, Opcode opcode) {
            return opcode == Opcode::SEL_reg && Encoding{insn}.dest_reg == shl_reg;
        })};
        u32 count{0};
        bool measured{false};
        if (!sel_insn) {
            measured = true;
            // ISETP.LT against the bound and a branch around the jump: indices 0..bound-1
            Location at2{after_shl};
            const std::optional<u64> isetp{Track(env, block_begin, at2, [shl_reg](u64 insn, Opcode opcode) {
                return opcode == Opcode::ISETP_reg && Encoding{insn}.src_reg == shl_reg;
            })};
            count = table_count(isetp ? find_movc(at2, static_cast<IR::Reg>((*isetp >> 20) & 0xff))
                                      : std::optional<u64>{}, 0);
        } else {
            const IR::Reg bound_reg{static_cast<IR::Reg>((*sel_insn >> 20) & 0xff)};
            Location at_mov{at};
            const std::optional<u64> mov_insn{Track(env, block_begin, at_mov, [bound_reg](u64 insn, Opcode opcode) {
                return (opcode == Opcode::MOV32I || opcode == Opcode::MOV_cbuf) && Encoding{insn}.dest_reg == bound_reg;
            })};
            if (!mov_insn) {
                return std::nullopt;
            }
            if (Decode(*mov_insn) == Opcode::MOV32I) {
                max_index = static_cast<u32>((*mov_insn >> 20) & 0xffffffffULL);
                if (max_index > 255) {   // (a table that large is not a clamp of this kind)
                    return std::nullopt;
                }
            } else {
                measured = true;
                count = table_count(mov_insn, 1);   // SEL clamps to the bound itself: indices 0..bound
            }
        }
        if (count) {
            return IndirectBranchTableInfo{
                .cbuf_index = cbuf_index,
                .cbuf_offset = cbuf_offset,
                .num_entries = count,
                .branch_offset = brx_offset,
                .branch_reg = brx_reg,
            };
        }
        if (measured) {   // (a measured table with no plausible entry)
            return std::nullopt;
        }
    }
    return IndirectBranchTableInfo{
        .cbuf_index = cbuf_index,
        .cbuf_offset = cbuf_offset,
        .num_entries = max_index + 1,
        .branch_offset = brx_offset,
        .branch_reg = brx_reg,
    };
}

} // namespace Shader::Maxwell
