// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot) The NaN guard on float colour outputs (RX_VK_NAN_GUARD=0: off; vk_shader.cpp, nx2/bake_ds, gpu/shaderbatch.cpp).
// It shared a header with the OoT fog and exposure workaround, which is gone.

#pragma once

#include <utility>
#include <vector>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {

/// (hookshot) Not a NaN or an infinity into a float colour target. One such pixel in the scene went through the bloom's chain of
/// averages to the whole picture: the one-frame white flash, the HUD over it (RX_VK_FRAMELOG, RX_VK_PROBE=2 caught it: a NaN in the
/// scene target, the blur after it all at its top). What the console's blend does with one is unknown; nothing here wants it.
/// float_targets: bit i, target i is a float one (an integer target's bits go as they are)
inline size_t GuardFragmentOutputs(IR::Program& program, u32 float_targets) {
    if (program.stage != Stage::Fragment) return 0;
    std::vector<std::pair<IR::Block*, IR::Inst*>> writes;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(0).IsImmediate() &&
                (float_targets >> inst.Arg(0).U32() & 1) != 0 && !inst.Arg(2).IsImmediate()) {
                writes.emplace_back(block, &inst);
            }
        }
    }
    for (const auto& [block, inst] : writes) {
        IR::IREmitter ir{*block, block->Instructions().iterator_to(*inst)};
        const IR::F32 value{inst->Arg(2)};
        const IR::F32 limited{ir.FPClamp(value, ir.Imm32(-1e30f), ir.Imm32(1e30f))};
        const IR::F32 safe{ir.Select(ir.FPIsNan(value), ir.Imm32(0.0f), limited)};
        inst->SetArg(2, safe);
    }
    return writes.size();
}

} // namespace Shader::Optimization
