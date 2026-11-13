// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot) The scene's tone map as PsychoV35 (psychov35.h), in the game's scene mapping programs: HDR set to Native and its
// Tone mapper row on (vk_shader.cpp). After RenoDX's zelda-oot-oz mod (github.com/clshortfuse/renodx, MIT), which replaces
// the same programs' fit at their decompiled source; here they are found in the IR, and nothing of the game's code is kept.
//
// What a scene mapping program does at its end, per channel: the scene (bloom added) times the exposure, a constant buffer
// word; that through the game's Narkowicz fit, saturated; the channels' luminance (0.2989, 0.5866, 0.1144) for its native
// HDR expansion; the colour written to target 0 under the peak, a word of the same bank. The pass finds each piece, and
// replaces only target 0's colour: PsychoV35 of the scene before the exposure, the exposure its midgray, the game's peak.

#pragma once

#include <array>
#include <bit>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/psychov35.h"

namespace Shader::Optimization {

namespace scene_tonemap_detail {

inline bool ImmBits(const IR::Value& v, u32& bits) {
    if (!v.IsImmediate()) return false;
    const IR::Value r{v.Resolve()};
    if (r.Type() == IR::Type::F32) return bits = std::bit_cast<u32>(r.F32()), true;
    if (r.Type() == IR::Type::U32) return bits = r.U32(), true;
    return false;
}
inline bool IsImm(const IR::Value& v, float f) {
    u32 bits;
    return ImmBits(v, bits) && bits == std::bit_cast<u32>(f);
}
/// A constant buffer word read as a float (GetCbufF32, or GetCbufU32 cast): its bank and offset
inline bool Cbuf(const IR::Value& v, u32& bank, u32& offset) {
    if (v.IsImmediate()) return false;
    const IR::Inst* inst{v.InstRecursive()};
    if (inst->GetOpcode() == IR::Opcode::BitCastF32U32) {
        if (inst->Arg(0).IsImmediate()) return false;
        inst = inst->Arg(0).InstRecursive();
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32) return false;
    } else if (inst->GetOpcode() != IR::Opcode::GetCbufF32) {
        return false;
    }
    if (!inst->Arg(0).IsImmediate() || !inst->Arg(1).IsImmediate()) return false;
    bank = inst->Arg(0).U32(), offset = inst->Arg(1).U32();
    return true;
}
/// Whether to is among what value is made of, through no phi (so to's block dominates where value is used)
inline bool MadeOf(const IR::Value& value, const IR::Inst* to) {
    std::vector<const IR::Inst*> stack;
    std::unordered_set<const IR::Inst*> seen;
    if (!value.IsImmediate()) stack.push_back(value.InstRecursive());
    while (!stack.empty() && seen.size() < 4096) {
        const IR::Inst* inst{stack.back()};
        stack.pop_back();
        if (inst == to) return true;
        if (!seen.insert(inst).second || inst->GetOpcode() == IR::Opcode::Phi) continue;
        for (size_t i = 0; i < inst->NumArgs(); i++)
            if (!inst->Arg(i).IsImmediate()) stack.push_back(inst->Arg(i).InstRecursive());
    }
    return false;
}

struct IrOps {
    IR::IREmitter& ir;
    using F = IR::F32;
    using B = IR::U1;
    F Imm(float v) { return ir.Imm32(v); }
    F Add(F a, F b) { return ir.FPAdd(a, b); }
    F Sub(F a, F b) { return ir.FPAdd(a, ir.FPNeg(b)); }
    F Mul(F a, F b) { return ir.FPMul(a, b); }
    F Fma(F a, F b, F c) { return ir.FPFma(a, b, c); }
    F Div(F a, F b) { return ir.FPMul(a, F{ir.FPRecip(b)}); }
    F Neg(F a) { return ir.FPNeg(a); }
    F Abs(F a) { return ir.FPAbs(a); }
    F Min(F a, F b) { return ir.FPMin(a, b); }
    F Max(F a, F b) { return ir.FPMax(a, b); }
    F Sqrt(F a) { return ir.FPSqrt(a); }
    F Log2(F a) { return ir.FPLog2(a); }
    F Exp2(F a) { return ir.FPExp2(a); }
    F Select(B c, F a, F b) { return F{ir.Select(c, a, b)}; }
    B Gt(F a, F b) { return ir.FPGreaterThan(a, b); }
    B Lt(F a, F b) { return ir.FPLessThan(a, b); }
    B Eq(F a, F b) { return ir.FPEqual(a, b); }
    B And(B a, B b) { return ir.LogicalAnd(a, b); }
};

} // namespace scene_tonemap_detail

/// PsychoV35 for target 0's colour of a scene mapping program; nullptr when made, else why this program is not one
inline const char* SceneToneMapPass(IR::Program& program) {
    using namespace scene_tonemap_detail;
    if (program.stage != Stage::Fragment) return "not a fragment program";
    std::unordered_map<const IR::Inst*, std::vector<IR::Inst*>> users;
    std::vector<IR::Inst*> fits;
    std::array<IR::Inst*, 3> writes{};
    std::array<IR::Block*, 3> write_blocks{};
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            for (size_t i = 0; i < inst.NumArgs(); i++)
                if (!inst.Arg(i).IsImmediate()) users[inst.Arg(i).InstRecursive()].push_back(&inst);
            if (inst.GetOpcode() == IR::Opcode::FPFma32 && IsImm(inst.Arg(1), 2.51f) && IsImm(inst.Arg(2), 0.03f) &&
                !inst.Arg(0).IsImmediate()) {
                fits.push_back(&inst);
            }
            if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(0).IsImmediate() && inst.Arg(0).U32() == 0 &&
                inst.Arg(1).IsImmediate() && inst.Arg(1).U32() < 3) {
                IR::Inst*& w{writes[inst.Arg(1).U32()]};
                if (w) return "target 0 written more than once";
                w = &inst;
                write_blocks[inst.Arg(1).U32()] = block;
            }
        }
    }
    if (fits.size() != 3) return "not three channels of the fit";
    for (IR::Inst* w : writes)
        if (!w || w->Arg(2).IsImmediate()) return "target 0's colour not written";
    IR::Block* const block{write_blocks[0]};
    if (write_blocks[1] != block || write_blocks[2] != block) return "target 0's colour written apart";

    // each fit's channel: the luminance weight it meets first after it
    static constexpr float kWeights[3] = {0.29890000820159912109375f, 0.586600005626678466796875f,
                                          0.1143999993801116943359375f};
    std::array<IR::Inst*, 3> by_channel{};
    for (IR::Inst* fit : fits) {
        int channel = -1;
        std::vector<const IR::Inst*> level{fit};
        std::unordered_set<const IR::Inst*> seen{fit};
        for (int depth = 0; depth < 16 && channel < 0 && !level.empty(); depth++) {
            std::vector<const IR::Inst*> next;
            for (const IR::Inst* inst : level) {
                for (IR::Inst* user : users[inst]) {
                    for (size_t i = 0; i < user->NumArgs() && channel < 0; i++)
                        for (int c = 0; c < 3; c++)
                            if (IsImm(user->Arg(i), kWeights[c])) channel = c;
                    if (channel >= 0) break;
                    if (seen.insert(user).second) next.push_back(user);
                }
                if (channel >= 0) break;
            }
            level.swap(next);
        }
        if (channel < 0) return "a fit's channel not found by the luminance";
        if (by_channel[channel]) return "two fits of one channel";
        by_channel[channel] = fit;
    }

    // the fit's input, the scene times the exposure; the colour under the peak
    std::array<IR::F32, 3> scene;
    IR::F32 gain, peak;
    u32 bank = ~0u, gain_offset = ~0u;
    for (int c = 0; c < 3; c++) {
        const IR::Value x{by_channel[c]->Arg(0)};
        const IR::Inst* mul{x.InstRecursive()};
        if (mul->GetOpcode() != IR::Opcode::FPMul32) return "the fit's input is not a product";
        u32 b, o;
        int at = Cbuf(mul->Arg(1), b, o) ? 1 : Cbuf(mul->Arg(0), b, o) ? 0 : -1;
        if (at < 0) return "the fit's input is not times a constant";
        if (c > 0 && (b != bank || o != gain_offset)) return "the channels' exposures differ";
        bank = b, gain_offset = o;
        gain = IR::F32{mul->Arg(at)};
        scene[c] = IR::F32{mul->Arg(1 - at)};
        if (!MadeOf(writes[c]->Arg(2), by_channel[c])) return "target 0's colour not made of its channel's fit";
    }
    {
        const IR::Inst* clamp{writes[0]->Arg(2).InstRecursive()};
        u32 b, o;
        if (clamp->GetOpcode() == IR::Opcode::FPMin32 && Cbuf(clamp->Arg(1), b, o) && b == bank)
            peak = IR::F32{clamp->Arg(1)};
        else if (clamp->GetOpcode() == IR::Opcode::FPMin32 && Cbuf(clamp->Arg(0), b, o) && b == bank)
            peak = IR::F32{clamp->Arg(0)};
        else if (clamp->GetOpcode() == IR::Opcode::FPClamp32 && Cbuf(clamp->Arg(2), b, o) && b == bank)
            peak = IR::F32{clamp->Arg(2)};
        else
            return "target 0's colour not under a peak of the exposure's bank";
    }

    // (before the last of the three writes: each channel's fit, the exposure and the peak are made by then, in any order;
    // the other two writes move down to it, after what they now write)
    IR::Inst* last{writes[0]};
    for (IR::Inst& inst : block->Instructions())
        if (&inst == writes[0] || &inst == writes[1] || &inst == writes[2]) last = &inst;
    auto& list{block->Instructions()};
    IR::IREmitter ir{*block, list.iterator_to(*last)};
    IrOps ops{ir};
    // (a peak under the paper white, or none: the game's SDR range)
    const IR::F32 safe_peak{ir.FPMax(peak, ir.Imm32(1.0f))};
    const auto out = PsychoV35::SceneToneMap(ops, scene, gain, safe_peak);
    for (int c = 0; c < 3; c++) {
        if (writes[c] != last) {
            list.erase(list.iterator_to(*writes[c]));
            list.insert(list.iterator_to(*last), *writes[c]);
        }
        writes[c]->SetArg(2, out[c]);
    }
    return nullptr;
}

} // namespace Shader::Optimization
