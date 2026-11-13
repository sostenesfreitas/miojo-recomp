// SPDX-License-Identifier: GPL-3.0-or-later
// The scene tone map pass (ir_opt/scene_tonemap_pass.h) on synthetic scene mapping programs: the game's shape (the scene times
// a constant buffer exposure, the Narkowicz fit, the luminance, target 0 under a peak of the same bank), its channels' fits
// emitted out of order. The pass's IR is interpreted here and compared with psychov35.h on floats; the shapes it must not
// take are left as they are. tests/test_scene_tonemap_pass.py builds and runs it.
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/ir_opt/scene_tonemap_pass.h"

using namespace Shader;
namespace PV = Shader::Optimization::PsychoV35;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

constexpr u32 kBank = 3, kGain = 0x38, kPeak = 0x40;

enum class Shape { Game, NoLuminance, TwoExposures, GainFirst, RedFirst };

struct Built {
    ObjectPool<IR::Inst> insts{256};
    ObjectPool<IR::Block> blocks{4};
    IR::Program program;
    std::array<IR::Inst*, 3> writes{};
};

void build(Built& b, Shape shape) {
    IR::Block* block = b.blocks.Create(b.insts);
    b.program.blocks.push_back(block);
    b.program.post_order_blocks.push_back(block);
    b.program.stage = Stage::Fragment;
    IR::IREmitter ir{*block};
    const IR::F32 gain = ir.GetFloatCbuf(ir.Imm32(kBank), ir.Imm32(kGain));
    const IR::F32 other_gain = ir.GetFloatCbuf(ir.Imm32(kBank), ir.Imm32(kGain + 4));
    std::array<IR::F32, 3> fitted;
    // (RedFirst: red's colour written before blue's and green's fits are made, then theirs after the luminance)
    const IR::F32 early_peak = shape == Shape::RedFirst ? ir.GetFloatCbuf(ir.Imm32(kBank), ir.Imm32(kPeak)) : IR::F32{};
    for (const int c : shape == Shape::RedFirst ? std::array<int, 3>{0, 2, 1} : std::array<int, 3>{2, 0, 1}) {
        // (the fits out of the channels' order: the luminance names them)
        const IR::F32 scene = ir.GetAttribute(IR::Attribute(int(IR::Attribute::Generic0X) + c));
        const IR::F32 g = shape == Shape::TwoExposures && c == 1 ? other_gain : gain;
        const IR::F32 x = shape == Shape::GainFirst ? IR::F32{ir.FPMul(g, scene)} : IR::F32{ir.FPMul(scene, g)};
        const IR::F32 num = ir.FPMul(x, ir.FPFma(x, ir.Imm32(2.51f), ir.Imm32(0.03f)));
        const IR::F32 den = ir.FPFma(x, ir.FPFma(x, ir.Imm32(2.43f), ir.Imm32(0.59f)), ir.Imm32(0.14f));
        fitted[c] = ir.FPSaturate(ir.FPMul(num, IR::F32{ir.FPRecip(den)}));
        if (shape == Shape::RedFirst && c == 0) ir.SetFragColor(0, 0, ir.FPMin(ir.FPMax(ir.Imm32(0.0f), fitted[0]), early_peak));
    }
    IR::F32 luminance = ir.Imm32(0.0f);
    if (shape != Shape::NoLuminance)
        luminance = ir.FPFma(fitted[2], ir.Imm32(0.1143999993801116943359375f),
                             ir.FPFma(fitted[0], ir.Imm32(0.29890000820159912109375f),
                                      ir.FPMul(fitted[1], ir.Imm32(0.586600005626678466796875f))));
    const IR::F32 peak = ir.GetFloatCbuf(ir.Imm32(kBank), ir.Imm32(kPeak));
    for (u32 c = shape == Shape::RedFirst ? 1 : 0; c < 3; c++) {
        const IR::F32 expanded = ir.FPFma(luminance, ir.Imm32(0.25f), fitted[c]);
        ir.SetFragColor(0, c, ir.FPMin(ir.FPMax(ir.Imm32(0.0f), expanded), peak));
    }
    ir.SetFragColor(0, 3, ir.Imm32(1.0f));
    for (IR::Inst& inst : block->Instructions())
        if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(1).U32() < 3) b.writes[inst.Arg(1).U32()] = &inst;
}

/// Whether every instruction's operands are made before it in the block (the pass's insertion point dominates its uses)
bool defined_before_use(const IR::Block& block) {
    std::unordered_map<const IR::Inst*, size_t> at;
    size_t i = 0;
    for (const IR::Inst& inst : block.Instructions()) at[&inst] = i++;
    for (const IR::Inst& inst : block.Instructions())
        for (size_t a = 0; a < inst.NumArgs(); a++)
            if (!inst.Arg(a).IsImmediate()) {
                const auto it = at.find(inst.Arg(a).InstRecursive());
                if (it == at.end() || it->second >= at[&inst]) return false;
            }
    return true;
}

struct Eval {
    std::array<float, 3> scene;
    float gain, other_gain, peak;
    std::unordered_map<const IR::Inst*, double> memo;
    double operator()(const IR::Value& v) {
        if (v.IsImmediate()) {
            const IR::Value r = v.Resolve();
            if (r.Type() == IR::Type::U1) return r.U1() ? 1.0 : 0.0;
            if (r.Type() == IR::Type::F32) return r.F32();
            return r.U32();
        }
        const IR::Inst* n = v.InstRecursive();
        if (const auto it = memo.find(n); it != memo.end()) return it->second;
        auto a = [&](size_t i) { return float((*this)(n->Arg(i))); };
        float r;
        switch (n->GetOpcode()) {
        case IR::Opcode::GetAttribute: r = scene[int(n->Arg(0).Attribute()) - int(IR::Attribute::Generic0X)]; break;
        case IR::Opcode::GetCbufF32: {
            const u32 at = n->Arg(1).U32();
            r = at == kGain ? gain : at == kGain + 4 ? other_gain : at == kPeak ? peak : throw std::runtime_error("cbuf");
            break;
        }
        case IR::Opcode::FPAdd32: r = a(0) + a(1); break;
        case IR::Opcode::FPMul32: r = a(0) * a(1); break;
        case IR::Opcode::FPFma32: r = std::fma(a(0), a(1), a(2)); break;
        case IR::Opcode::FPNeg32: r = -a(0); break;
        case IR::Opcode::FPAbs32: r = std::fabs(a(0)); break;
        case IR::Opcode::FPMin32: r = std::fmin(a(0), a(1)); break;
        case IR::Opcode::FPMax32: r = std::fmax(a(0), a(1)); break;
        case IR::Opcode::FPRecip32: r = 1.0f / a(0); break;
        case IR::Opcode::FPLog2: r = std::log2(a(0)); break;
        case IR::Opcode::FPExp2: r = std::exp2(a(0)); break;
        case IR::Opcode::FPSqrt: r = std::sqrt(a(0)); break;
        case IR::Opcode::FPSaturate32: r = std::fmin(1.0f, std::fmax(0.0f, a(0))); break;
        case IR::Opcode::SelectF32: r = a(0) != 0.0f ? a(1) : a(2); break;
        case IR::Opcode::FPOrdGreaterThan32: r = a(0) > a(1); break;
        case IR::Opcode::FPOrdLessThan32: r = a(0) < a(1); break;
        case IR::Opcode::FPOrdEqual32: r = a(0) == a(1); break;
        case IR::Opcode::LogicalAnd: r = a(0) != 0.0f && a(1) != 0.0f; break;
        default: throw std::runtime_error("an opcode the fixture does not evaluate");
        }
        return memo[n] = r;
    }
};

struct FloatOps {
    using F = float;
    using B = bool;
    F Imm(float v) { return v; }
    F Add(F a, F b) { return a + b; }
    F Sub(F a, F b) { return a + -b; }
    F Mul(F a, F b) { return a * b; }
    F Fma(F a, F b, F c) { return std::fma(a, b, c); }
    F Div(F a, F b) { return a * (1.0f / b); }
    F Neg(F a) { return -a; }
    F Abs(F a) { return std::fabs(a); }
    F Min(F a, F b) { return std::fmin(a, b); }
    F Max(F a, F b) { return std::fmax(a, b); }
    F Sqrt(F a) { return std::sqrt(a); }
    F Log2(F a) { return std::log2(a); }
    F Exp2(F a) { return std::exp2(a); }
    F Select(B c, F a, F b) { return c ? a : b; }
    B Gt(F a, F b) { return a > b; }
    B Lt(F a, F b) { return a < b; }
    B Eq(F a, F b) { return a == b; }
    B And(B a, B b) { return a && b; }
};

int main() {
    {
        Built b;
        build(b, Shape::Game);
        const char* why = Optimization::SceneToneMapPass(b.program);
        if (why) std::fprintf(stderr, "the game's shape refused: %s\n", why);
        CHECK(why == nullptr);
        const float cases[][5] = {{0.18f, 0.18f, 0.18f, 1.0f, 4.926f}, {5.0f, 1.0f, 0.2f, 1.0f, 4.0f},
                                  {0.0f, 2.0f, 0.0f, 1.5f, 5.0f},      {40.0f, 3.0f, 9.0f, 0.7f, 10.0f},
                                  {0.0f, 0.0f, 0.0f, 1.0f, 5.0f},      {0.3f, 0.2f, 0.1f, 1.0f, 0.5f}};
        for (const auto& k : cases) {
            Eval e{{k[0], k[1], k[2]}, k[3], 0.0f, k[4], {}};
            FloatOps o;
            const auto want = PV::SceneToneMap(o, std::array<float, 3>{k[0], k[1], k[2]}, k[3], std::fmax(k[4], 1.0f));
            for (int c = 0; c < 3; c++) {
                const float got = float(e(b.writes[c]->Arg(2)));
                if (!(std::fabs(got - want[c]) <= 1e-5f * std::fmax(1.0f, std::fabs(want[c])))) {
                    std::fprintf(stderr, "channel %d of (%g %g %g) gain %g peak %g: %g, PsychoV35 %g\n", c, k[0], k[1], k[2],
                                 k[3], k[4], got, want[c]);
                    return 1;
                }
            }
        }
        std::printf("the game's shape: PsychoV35, channels by their luminance weights, the peak its own\n");
    }
    {
        Built b;
        build(b, Shape::GainFirst);
        CHECK(Optimization::SceneToneMapPass(b.program) == nullptr);
        Eval e{{2.0f, 1.0f, 0.5f}, 1.0f, 0.0f, 5.0f, {}};
        FloatOps o;
        const auto want = PV::SceneToneMap(o, std::array<float, 3>{2.0f, 1.0f, 0.5f}, 1.0f, 5.0f);
        for (int c = 0; c < 3; c++) CHECK(std::fabs(float(e(b.writes[c]->Arg(2))) - want[c]) < 1e-5f);
        std::printf("the exposure first in the product: the same\n");
    }
    for (const Shape shape : {Shape::Game, Shape::RedFirst}) {
        Built b;
        build(b, shape);
        CHECK(Optimization::SceneToneMapPass(b.program) == nullptr);
        CHECK(defined_before_use(*b.program.blocks[0]));
        Eval e{{0.6f, 3.0f, 0.1f}, 1.2f, 0.0f, 6.0f, {}};
        FloatOps o;
        const auto want = PV::SceneToneMap(o, std::array<float, 3>{0.6f, 3.0f, 0.1f}, 1.2f, 6.0f);
        for (int c = 0; c < 3; c++) CHECK(std::fabs(float(e(b.writes[c]->Arg(2))) - want[c]) < 1e-5f);
    }
    std::printf("red written before the other channels' fits: PsychoV35 made before the last write, each value before its use\n");
    for (const Shape shape : {Shape::NoLuminance, Shape::TwoExposures}) {
        Built b;
        build(b, shape);
        std::array<IR::Value, 3> before;
        for (int c = 0; c < 3; c++) before[c] = b.writes[c]->Arg(2);
        size_t count = 0;
        for (const IR::Inst& inst : b.program.blocks[0]->Instructions()) (void)inst, count++;
        const char* why = Optimization::SceneToneMapPass(b.program);
        CHECK(why != nullptr);
        size_t after = 0;
        for (const IR::Inst& inst : b.program.blocks[0]->Instructions()) (void)inst, after++;
        CHECK(after == count);
        for (int c = 0; c < 3; c++) CHECK(b.writes[c]->Arg(2) == before[c]);
        std::printf("%s: left as it is (%s)\n", shape == Shape::NoLuminance ? "no luminance" : "two exposures", why);
    }
    std::printf("PASS\n");
}
