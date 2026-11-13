#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spirv/unified1/spirv.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"

using namespace Shader;
using Shader::Backend::SPIRV::EmitSPIRV;
static std::filesystem::path output_dir{"work/global16-modules"};

struct TestEnvironment final : Environment {
    explicit TestEnvironment(Stage s = Stage::Compute) { stage = s; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return TexturePixelFormat::R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

[[noreturn]] static void fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
}

static std::vector<u32> emit_program(bool is_signed, u64 address, bool support_int64,
                                     bool lower_address, Stage stage = Stage::Compute, bool with_single_ssbo = false) {
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = stage;
    program.output_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_primitives = stage == Stage::Mesh ? 1 : 0;
    program.mesh.payload_words = stage == Stage::Mesh ? 1 : 0;
    program.workgroup_size = {1, 1, 1};
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    IR::AbstractSyntaxNode ret{};
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.push_back(ret);

    IR::IREmitter ir{block};
    IR::U64 guest_address;
    if (lower_address) {
        guest_address = ir.UConvert(64, ir.Imm32(static_cast<u32>(address)));
    } else {
        guest_address = ir.Imm64(address);
    }
    if (is_signed) {
        const IR::U32 value{ir.LoadGlobalS16(guest_address)};
        (void)ir.IAdd(value, ir.Imm32(1));
    } else {
        const IR::U32 value{ir.LoadGlobalU16(guest_address)};
        (void)ir.IAdd(value, ir.Imm32(1));
    }
    ir.Epilogue();

    TestEnvironment env{stage};
    if (lower_address) Optimization::LowerInt64ToInt32(program);
    Optimization::CollectShaderInfoPass(env, program);
    if (!program.info.uses_global_memory) fail("CollectShaderInfoPass did not see the actual global load");
    if (with_single_ssbo) {
        program.info.constant_buffer_descriptors.push_back({0, 1});
        program.info.storage_buffers_descriptors.push_back({0, 0, 1, false});
        program.info.used_constant_buffer_types |= IR::Type::U32 | IR::Type::U32x2;
    }

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = support_int64;
    if (with_single_ssbo) {
        profile.support_descriptor_aliasing = true;
        profile.min_ssbo_alignment = 16;
    }
    return EmitSPIRV(profile, program);
}

struct Instruction {
    spv::Op op;
    std::vector<u32> words;
};

struct ModuleInfo {
    std::vector<Instruction> instructions;
    std::map<u32, u32> integer_widths;
    std::map<u32, bool> signed_integer_types;
    std::map<u32, u64> constants;
};

static ModuleInfo inspect(const std::vector<u32>& code) {
    if (code.size() < 5 || code[0] != spv::MagicNumber) fail("bad SPIR-V header");
    ModuleInfo result;
    for (size_t at = 5; at < code.size();) {
        const u32 count = code[at] >> 16;
        if (count == 0 || at + count > code.size()) fail("bad SPIR-V instruction length");
        const auto op = static_cast<spv::Op>(code[at] & 0xffffu);
        std::vector<u32> words(code.begin() + at + 1, code.begin() + at + count);
        if (op == spv::Op::OpTypeInt && words.size() >= 3) {
            result.integer_widths[words[0]] = words[1];
            result.signed_integer_types[words[0]] = words[2] != 0;
        } else if (op == spv::Op::OpConstant && words.size() >= 3) {
            const u32 type = words[0], id = words[1];
            const auto width_it = result.integer_widths.find(type);
            const u32 width = width_it == result.integer_widths.end() ? 0 : width_it->second;
            u64 value = words[2];
            if (width == 64 && words.size() >= 4) value |= static_cast<u64>(words[3]) << 32;
            result.constants[id] = value;
        } else if (op == spv::Op::OpConstantNull && words.size() >= 2) {
            result.constants[words[1]] = 0;
        }
        result.instructions.push_back({op, std::move(words)});
        at += count;
    }
    return result;
}

static u64 constant(const ModuleInfo& module, u32 id, const char* name) {
    const auto it = module.constants.find(id);
    if (it == module.constants.end()) fail(name);
    return it->second;
}

static std::vector<u32> save_module(const std::string& name, std::vector<u32> code) {
    std::filesystem::create_directories(output_dir);
    std::ofstream file(output_dir / (name + ".spv"), std::ios::binary);
    if (!file) fail("could not create emitted SPIR-V module");
    file.write(reinterpret_cast<const char*>(code.data()),
               static_cast<std::streamsize>(code.size() * sizeof(u32)));
    if (!file) fail("could not write emitted SPIR-V module");
    return code;
}

struct SingleSsboScenario {
    u64 descriptor_address;
    u32 descriptor_size;
    u32 descriptor_words;
    bool expected_in_range;
};

static void check_single_ssbo_range(const ModuleInfo& module, u64 address,
                                    const SingleSsboScenario& scenario, const char* name) {
    u32 u64_type = 0;
    u32 u32x2_type = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpTypeInt && w.size() >= 3 && w[1] == 64 && w[2] == 0)
            u64_type = w[0];
        if (inst.op == spv::Op::OpTypeVector && w.size() >= 3 && w[2] == 2) {
            const auto component = module.integer_widths.find(w[1]);
            if (component != module.integer_widths.end() && component->second == 32) {
                const auto signedness = module.signed_integer_types.find(w[1]);
                if (signedness != module.signed_integer_types.end() && !signedness->second)
                    u32x2_type = w[0];
            }
        }
    }
    if (!u64_type || !u32x2_type) fail("single-SSBO fixture lacks U64 or U32x2 types");

    u32 input_address = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        const auto width = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpConstant && w.size() >= 3 && width != module.integer_widths.end() &&
            width->second == 64 && constant(module, w[1], "missing IR address constant") == address)
            input_address = w[1];
    }
    if (!input_address) fail("single-SSBO fixture lost its actual IR address constant");

    u32 aligned_call_address = 0;
    u64 helper_argument = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpBitwiseAnd && w.size() >= 4 && w[2] == input_address &&
            constant(module, w[3], "missing 4-byte alignment mask") == ~u64{3}) {
            aligned_call_address = w[1];
            helper_argument = constant(module, w[2], "missing global address") &
                              constant(module, w[3], "missing global alignment mask");
        }
    }
    if (!aligned_call_address) fail("single-SSBO fixture did not align its real U16/S16 address");

    u32 helper_function = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        const auto width = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpFunctionCall && w.size() >= 4 && w[3] == aligned_call_address &&
            width != module.integer_widths.end() && width->second == 32)
            helper_function = w[2];
    }
    if (!helper_function) fail("single-SSBO fixture did not call the global U32 helper");

    std::vector<const Instruction*> body;
    bool in_helper = false;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpFunction && w.size() >= 2 && w[1] == helper_function) {
            in_helper = true;
            continue;
        }
        if (in_helper && inst.op == spv::Op::OpFunctionEnd) break;
        if (in_helper) body.push_back(&inst);
    }
    if (body.empty()) fail("could not inspect the called global U32 helper body");

    std::map<u32, const Instruction*> loads;
    std::map<u32, const Instruction*> converts;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpLoad && w.size() >= 3) loads[w[1]] = inst;
        if (inst->op == spv::Op::OpUConvert && w.size() >= 3) converts[w[1]] = inst;
    }

    u32 unaligned_address = 0;
    u32 address_load = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op != spv::Op::OpBitcast || w.size() < 3 || w[0] != u64_type) continue;
        const auto source = loads.find(w[2]);
        if (source != loads.end() && source->second->words[0] == u32x2_type) {
            unaligned_address = w[1];
            address_load = w[2];
        }
    }
    if (!unaligned_address) fail("could not find the loaded, unaligned SSBO guest address");

    u32 aligned_ssbo_base = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpBitwiseAnd && w.size() >= 4 && w[0] == u64_type &&
            w[2] == unaligned_address && constant(module, w[3], "missing SSBO alignment mask") == ~u64{15})
            aligned_ssbo_base = w[1];
    }
    if (!aligned_ssbo_base) fail("single-SSBO helper did not align the descriptor base to 16 bytes");

    u32 raw_end = 0;
    u32 size_load = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op != spv::Op::OpIAdd || w.size() < 4 || w[0] != u64_type) continue;
        const auto size = converts.find(w[3]);
        if (w[2] == unaligned_address && size != converts.end() && size->second->words[0] == u64_type) {
            raw_end = w[1];
            size_load = size->second->words[2];
        }
    }
    if (!raw_end || !size_load) fail("single-SSBO logical end is not computed from original address plus size");

    u32 array_length = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpArrayLength && w.size() >= 4) array_length = w[1];
    }
    if (!array_length) fail("single-SSBO range no longer caps itself to OpArrayLength");

    u32 byte_length = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpShiftLeftLogical && w.size() >= 4 && w[2] == array_length &&
            constant(module, w[3], "missing descriptor-word shift") == 2)
            byte_length = w[1];
    }
    u32 descriptor_bytes = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpUConvert && w.size() >= 3 && w[2] == byte_length && w[0] == u64_type)
            descriptor_bytes = w[1];
    }
    if (!byte_length || !descriptor_bytes) fail("OpArrayLength is not converted to the descriptor byte length");

    u32 bound_end = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpIAdd && w.size() >= 4 && w[0] == u64_type &&
            w[2] == aligned_ssbo_base && w[3] == descriptor_bytes)
            bound_end = w[1];
    }
    if (!bound_end) fail("descriptor byte length is not added to the aligned SSBO base");

    u32 bound_is_shorter = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpULessThan && w.size() >= 4 &&
            w[2] == bound_end && w[3] == raw_end)
            bound_is_shorter = w[1];
    }
    if (!bound_is_shorter) fail("descriptor cap does not compare its end against the logical end");

    u32 capped_end = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpSelect && w.size() >= 5 && w[0] == u64_type &&
            w[2] == bound_is_shorter && w[3] == bound_end && w[4] == raw_end)
            capped_end = w[1];
    }
    if (!capped_end) fail("single-SSBO helper does not select the lesser of logical and bound ends");

    u32 helper_parameter = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpFunctionParameter && w.size() >= 2 && w[0] == u64_type)
            helper_parameter = w[1];
    }
    u32 lower_check = 0;
    u32 upper_check = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpUGreaterThanEqual && w.size() >= 4 &&
            w[2] == helper_parameter && w[3] == aligned_ssbo_base)
            lower_check = w[1];
        if (inst->op == spv::Op::OpULessThan && w.size() >= 4 &&
            w[2] == helper_parameter && w[3] == capped_end)
            upper_check = w[1];
    }
    u32 membership = 0;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpLogicalAnd && w.size() >= 4 &&
            w[2] == lower_check && w[3] == upper_check)
            membership = w[1];
    }
    bool is_actual_branch = false;
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpBranchConditional && w.size() >= 3 && w[0] == membership)
            is_actual_branch = true;
    }
    if (!helper_parameter || !lower_check || !upper_check || !membership || !is_actual_branch)
        fail("capped end is not used by the called helper's range branch");

    std::map<u32, u64> values;
    const auto get_value = [&](u32 id) -> std::optional<u64> {
        const auto value = values.find(id);
        if (value != values.end()) return value->second;
        const auto literal = module.constants.find(id);
        if (literal != module.constants.end()) return literal->second;
        return std::nullopt;
    };
    for (const Instruction* inst : body) {
        const auto& w = inst->words;
        if (inst->op == spv::Op::OpFunctionParameter && w.size() >= 2 && w[1] == helper_parameter) {
            values[w[1]] = helper_argument;
        } else if (inst->op == spv::Op::OpLoad && w.size() >= 3 && w[1] == address_load) {
            values[w[1]] = scenario.descriptor_address;
        } else if (inst->op == spv::Op::OpLoad && w.size() >= 3 && w[1] == size_load) {
            values[w[1]] = scenario.descriptor_size;
        } else if (inst->op == spv::Op::OpArrayLength && w.size() >= 4 && w[1] == array_length) {
            values[w[1]] = scenario.descriptor_words;
        } else if (inst->op == spv::Op::OpBitcast && w.size() >= 3) {
            if (const auto value = get_value(w[2])) values[w[1]] = *value;
        } else if (inst->op == spv::Op::OpBitwiseAnd && w.size() >= 4) {
            const auto lhs = get_value(w[2]);
            const auto rhs = get_value(w[3]);
            if (lhs && rhs) values[w[1]] = *lhs & *rhs;
        } else if (inst->op == spv::Op::OpUConvert && w.size() >= 3) {
            if (const auto value = get_value(w[2])) values[w[1]] = *value;
        } else if (inst->op == spv::Op::OpIAdd && w.size() >= 4) {
            const auto lhs = get_value(w[2]);
            const auto rhs = get_value(w[3]);
            if (lhs && rhs) values[w[1]] = *lhs + *rhs;
        } else if (inst->op == spv::Op::OpShiftLeftLogical && w.size() >= 4) {
            const auto lhs = get_value(w[2]);
            const auto rhs = get_value(w[3]);
            if (lhs && rhs && *rhs < 64) values[w[1]] = *lhs << *rhs;
        } else if (inst->op == spv::Op::OpULessThan && w.size() >= 4) {
            const auto lhs = get_value(w[2]);
            const auto rhs = get_value(w[3]);
            if (lhs && rhs) values[w[1]] = *lhs < *rhs;
        } else if (inst->op == spv::Op::OpUGreaterThanEqual && w.size() >= 4) {
            const auto lhs = get_value(w[2]);
            const auto rhs = get_value(w[3]);
            if (lhs && rhs) values[w[1]] = *lhs >= *rhs;
        } else if (inst->op == spv::Op::OpLogicalAnd && w.size() >= 4) {
            const auto lhs = get_value(w[2]);
            const auto rhs = get_value(w[3]);
            if (lhs && rhs) values[w[1]] = *lhs && *rhs;
        } else if (inst->op == spv::Op::OpSelect && w.size() >= 5) {
            const auto condition = get_value(w[2]);
            const auto yes = get_value(w[3]);
            const auto no = get_value(w[4]);
            if (condition && yes && no) values[w[1]] = *condition ? *yes : *no;
        }
    }
    const auto in_range = get_value(membership);
    if (!in_range) fail("could not evaluate the called helper's emitted range branch");
    const bool actual_in_range = *in_range != 0;
    if (actual_in_range != scenario.expected_in_range) {
        fail("single-SSBO helper range branch disagrees with the concrete descriptor scenario");
    }

    std::printf("PASS: %s helper branch is %s with descriptor %016llx+%x and %u words\n",
                name, actual_in_range ? "in range" : "out of range",
                static_cast<unsigned long long>(scenario.descriptor_address),
                scenario.descriptor_size, scenario.descriptor_words);
}

static void check_case(bool is_signed, u64 address, const char* name, Stage stage = Stage::Compute,
                       std::optional<SingleSsboScenario> ssbo_scenario = std::nullopt) {
    std::string filename;
    for (const char* c = name; *c; ++c) filename += (*c == ' ' ? '-' : *c);
    const auto module = inspect(save_module(
        filename, emit_program(is_signed, address, true, false, stage, ssbo_scenario.has_value())));
    const u64 aligned = address & ~u64{3};
    const u32 expected_bit_offset = static_cast<u32>((address & 3u) << 3);
    u32 address_id = 0, aligned_id = 0, low_lane_id = 0, low_id = 0, bit_offset_id = 0;
    bool saw_s_extract = false, saw_u_extract = false;
    u32 global_word_id = 0;

    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        const auto width_it = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpConstant && w.size() >= 3 && width_it != module.integer_widths.end() && width_it->second == 64 &&
            constant(module, w[1], "missing 64-bit guest address constant") == address) {
            address_id = w[1];
        }
    }
    if (!address_id) fail("actual IR address was not emitted as a 64-bit constant");

    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        const auto width_it = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpBitwiseAnd && w.size() >= 4 && width_it != module.integer_widths.end() && width_it->second == 64 &&
            w[2] == address_id) {
            const u64 mask = constant(module, w[3], "missing address mask");
            if (mask == ~u64{3}) {
                aligned_id = w[1];
                if ((constant(module, address_id, "missing source address") & ~u64{3}) != aligned)
                    fail("aligned helper base changed address high bits");
            } else if (mask == 3) {
                low_lane_id = w[1];
            }
        }
        const auto width32_it = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpUConvert && w.size() >= 3 && w[2] == low_lane_id &&
            width32_it != module.integer_widths.end() && width32_it->second == 32) low_id = w[1];
    }
    if (!aligned_id || !low_lane_id || !low_id)
        fail("missing full-U64 alignment or original-address lane extraction");

    u32 shifted_id = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpShiftLeftLogical && w.size() >= 4 && w[2] == low_id &&
            constant(module, w[3], "missing byte-to-bit shift") == 3) shifted_id = w[1];
        const auto result_width = w.empty() ? module.integer_widths.end()
                                            : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpFunctionCall && w.size() >= 4 && w[3] == aligned_id &&
            result_width != module.integer_widths.end() && result_width->second == 32)
            global_word_id = w[1];
    }
    if (!shifted_id || !global_word_id)
        fail("global32 helper is not called with the aligned original U64 address");

    bit_offset_id = shifted_id;
    if (!bit_offset_id) fail("extract offset is not derived from the original address low bits");

    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if ((inst.op == spv::Op::OpBitFieldSExtract || inst.op == spv::Op::OpBitFieldUExtract) &&
            w.size() >= 5 && w[2] == global_word_id && w[3] == bit_offset_id &&
            constant(module, w[4], "missing 16-bit extract width") == 16) {
            if (inst.op == spv::Op::OpBitFieldSExtract) saw_s_extract = true;
            if (inst.op == spv::Op::OpBitFieldUExtract) saw_u_extract = true;
            if (expected_bit_offset != 0 && expected_bit_offset != 16) fail("test used non-halfword-aligned address");
        }
    }
    if (is_signed ? (!saw_s_extract || saw_u_extract) : (!saw_u_extract || saw_s_extract))
        fail("wrong signed/unsigned extraction operation for the requested IR opcode");

    if (ssbo_scenario) check_single_ssbo_range(module, address, *ssbo_scenario, name);

    std::printf("PASS: %s emitted aligned global32 helper + %s16 extraction (base=%016llx offset=%u)\n",
                name, is_signed ? "signed " : "unsigned ", static_cast<unsigned long long>(aligned),
                expected_bit_offset);
}

static std::vector<u32> emit_write_program(bool is_signed, u64 address, bool support_int64, u32 bits,
                                           Stage stage = Stage::Compute) {
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = stage;
    program.output_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_primitives = stage == Stage::Mesh ? 1 : 0;
    program.mesh.payload_words = stage == Stage::Mesh ? 1 : 0;
    program.workgroup_size = {1, 1, 1};
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    IR::AbstractSyntaxNode ret{};
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.push_back(ret);

    IR::IREmitter ir{block};
    const IR::U64 guest_address{ir.Imm64(address)};
    const IR::U32 value{ir.Imm32(bits == 8 ? 0xA5u : 0xA5A5u)};
    if (is_signed) {
        if (bits == 8) ir.WriteGlobalS8(guest_address, value);
        else ir.WriteGlobalS16(guest_address, value);
    } else {
        if (bits == 8) ir.WriteGlobalU8(guest_address, value);
        else ir.WriteGlobalU16(guest_address, value);
    }
    ir.Epilogue();

    TestEnvironment env{stage};
    Optimization::CollectShaderInfoPass(env, program);
    if (!program.info.stores_global_memory) fail("CollectShaderInfoPass did not see the actual global store");
    if (!program.info.uses_global_memory) fail("CollectShaderInfoPass did not see the global store's memory");

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = support_int64;
    return EmitSPIRV(profile, program);
}

static void check_write_case(bool is_signed, u64 address, u32 bits, const char* name,
                             Stage stage = Stage::Compute) {
    std::string filename;
    for (const char* c = name; *c; ++c) filename += (*c == ' ' ? '-' : *c);
    const auto module = inspect(save_module(filename, emit_write_program(is_signed, address, true, bits, stage)));
    const u32 expected_bit_offset = static_cast<u32>((address & 3u) << 3);
    bool saw_insert = false, saw_cas = false, saw_call = false, saw_shift = false;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpBitFieldInsert && w.size() >= 6) saw_insert = true;
        if (inst.op == spv::Op::OpAtomicCompareExchange) saw_cas = true;
        // (the compare-exchange helper's call: address, value, bit offset, width)
        if (inst.op == spv::Op::OpFunctionCall && w.size() >= 7 &&
            constant(module, w[6], "missing narrow-store width") == bits)
            saw_call = true;
        if (inst.op == spv::Op::OpShiftLeftLogical && w.size() >= 4 &&
            constant(module, w[3], "missing byte-to-bit shift") == 3)
            saw_shift = true;
    }
    if (!saw_insert) fail("narrow global store did not insert into its aligned word");
    if (!saw_cas) fail("narrow global store did not use a compare-exchange loop");
    if (!saw_call) fail("narrow global store did not call the compare-exchange helper with its width");
    if (!saw_shift) fail("narrow global store did not derive its bit offset from the address low bits");
    std::printf("PASS: %s emitted aligned-word compare-exchange + %u-bit insert (offset=%u)\n", name, bits,
                expected_bit_offset);
}

static void check_unsupported_profile() {
    const auto module = inspect(save_module(
        "unsupported-int64", emit_program(true, 0x0000000100000002ull, false, true)));
    u32 zero_id = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpBitFieldSExtract || inst.op == spv::Op::OpBitFieldUExtract)
            fail("unsupported-int64 profile emitted a halfword extract instead of zero fallback");
        const auto type_width = w.empty() ? module.integer_widths.end()
                                          : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpConstant && w.size() >= 3 && w[2] == 0 &&
            type_width != module.integer_widths.end() && type_width->second == 32)
            zero_id = w[1];
    }
    bool zero_fallback_used = false;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpIAdd && w.size() >= 4 && w[2] == zero_id &&
            constant(module, w[3], "missing fallback's observable addend") == 1)
            zero_fallback_used = true;
    }
    if (!zero_id || !zero_fallback_used)
        fail("unsupported-int64 result was not the zero constant consumed by the fixture");
    std::puts("PASS: unsupported-int64 result is zero and emits no halfword extraction");
}

static void check_original_stub_failure() {
    try {
        (void)emit_program(true, 0x0000000100000000ull, true, false);
    } catch (const NotImplementedException& error) {
        if (std::string(error.what()).find("LoadGlobalS16") == std::string::npos)
            fail("original-stub negative control failed for the wrong IR opcode");
        std::puts("PASS: original S16 stub fails specifically at LoadGlobalS16");
        return;
    }
    fail("original-stub negative control unexpectedly emitted a module");
}

static void check_cas_functions(const ModuleInfo& module) {
    u32 bounded = 0, single = 0;
    for (size_t first = 0; first < module.instructions.size(); ++first) {
        if (module.instructions[first].op != spv::Op::OpFunction) continue;
        size_t last = first + 1;
        while (last < module.instructions.size() && module.instructions[last].op != spv::Op::OpFunctionEnd) ++last;
        std::map<u32, const Instruction*> defs;
        std::map<u32, size_t> labels;
        bool entry = true, variables = true;
        for (size_t i = first + 1; i < last; ++i) {
            const auto& inst = module.instructions[i];
            const auto& w = inst.words;
            if (inst.op == spv::Op::OpLabel) {
                labels[w[0]] = i;
                if (!entry) variables = false;
                entry = false;
            } else if (inst.op == spv::Op::OpVariable) {
                if (!variables) fail("CAS helper declares a Function variable after an instruction or entry block");
                defs[w[1]] = &inst;
            } else if (inst.op != spv::Op::OpFunctionParameter) {
                variables = false;
                if (inst.op == spv::Op::OpLoad || inst.op == spv::Op::OpIAdd ||
                    inst.op == spv::Op::OpUGreaterThanEqual || inst.op == spv::Op::OpLogicalOr ||
                    inst.op == spv::Op::OpIEqual) defs[w[1]] = &inst;
            }
        }
        const auto def = [&](u32 id, spv::Op op) -> const std::vector<u32>& {
            const auto it = defs.find(id);
            if (it == defs.end() || it->second->op != op) fail("CAS exit lost its success-or-incremented-ceiling dependency");
            return it->second->words;
        };
        std::map<size_t, bool> covered;
        bool has_loop = false;
        for (size_t i = first + 1; i < last; ++i) {
            const auto& loop = module.instructions[i];
            if (loop.op != spv::Op::OpLoopMerge) continue;
            has_loop = true;
            const u32 merge = loop.words[0];
            const size_t end = labels.at(merge);
            u32 header = 0;
            for (const auto& [id, at] : labels) if (at < i && (!header || at > labels.at(header))) header = id;
            size_t preheader = first;
            for (const auto& [id, at] : labels) if (at < labels.at(header) && at > preheader) preheader = at;
            for (size_t a = i + 1; a < end; ++a) {
                const auto& atomic = module.instructions[a];
                if (atomic.op != spv::Op::OpAtomicCompareExchange) continue;
                bool exits = false;
                for (size_t b = a + 1; b < end; ++b) {
                    const auto& branch = module.instructions[b];
                    if (branch.op != spv::Op::OpBranchConditional || branch.words[1] != merge) continue;
                    const auto& either = def(branch.words[0], spv::Op::OpLogicalOr);
                    const auto& success = def(either[2], spv::Op::OpIEqual);
                    const auto& cap = def(either[3], spv::Op::OpUGreaterThanEqual);
                    const auto& add = def(cap[2], spv::Op::OpIAdd);
                    const auto& load = def(add[2], spv::Op::OpLoad);
                    const auto& variable = def(load[2], spv::Op::OpVariable);
                    if (variable[2] != static_cast<u32>(spv::StorageClassFunction) ||
                        constant(module, add[3], "missing attempt increment") != 1 ||
                        constant(module, cap[3], "missing attempt ceiling") != 1024 ||
                        !((success[2] == atomic.words[1] && success[3] == atomic.words[7]) ||
                          (success[3] == atomic.words[1] && success[2] == atomic.words[7])))
                        fail("CAS exit does not compare its exchange result or count exactly one attempt");
                    bool reset = false, incremented = false, feedback = false;
                    const auto& expected = def(atomic.words[7], spv::Op::OpLoad);
                    for (size_t s = first + 1; s < b; ++s) {
                        const auto& store = module.instructions[s];
                        if (store.op != spv::Op::OpStore) continue;
                        if (s > preheader && s < labels.at(header) && store.words[0] == load[2]) {
                            const auto value = module.constants.find(store.words[1]);
                            if (value != module.constants.end() && value->second == 0) reset = true;
                        }
                        if (s > a && store.words[0] == load[2] && store.words[1] == add[1]) incremented = true;
                        if (s > a && store.words[0] == expected[2] && store.words[1] == atomic.words[1]) feedback = true;
                    }
                    const u32 retry = branch.words[2];
                    if (retry != header) {
                        const auto& back = module.instructions[labels.at(retry) + 1];
                        if (back.op != spv::Op::OpBranch || back.words[0] != header)
                            fail("CAS failed-attempt path no longer returns to its loop header");
                    }
                    if (!reset || !incremented || !feedback) fail("CAS loop lost reset, increment or compare-exchange feedback");
                    exits = true;
                }
                if (!exits) fail("CAS retry loop has no success-or-ceiling exit");
                covered[a] = true;
                ++bounded;
            }
        }
        for (size_t i = first + 1; i < last; ++i) {
            const auto& inst = module.instructions[i];
            if (inst.op != spv::Op::OpAtomicCompareExchange || covered.contains(i)) continue;
            bool returned = false;
            for (size_t r = i + 1; r < last; ++r)
                if (module.instructions[r].op == spv::Op::OpReturnValue &&
                    module.instructions[r].words[0] == inst.words[1]) returned = true;
            if (has_loop || !returned) fail("unclassified compare-exchange bypasses the retry-loop checks");
            ++single;
        }
        first = last;
    }
    if (bounded != 7 || single != 2) fail("fixture did not emit all shared/storage/global loops and both one-shot CAS routes");
    std::printf("PASS: %u retry loops reset/count/exit on success or 1024; %u one-shot exchanges return directly\n", bounded, single);
}

static void check_cas_program() {
    if (!std::getenv("RX_VK_FOLLOW_STORES")) fail("CAS fixture needs RX_VK_FOLLOW_STORES=1 to exercise the page-table route");
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.shared_memory_size = 16;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    IR::AbstractSyntaxNode ret{};
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.push_back(ret);
    IR::IREmitter ir{block};
    ir.WriteShared(8, ir.Imm32(0u), ir.Imm32(0xa5u));
    // Storage IR is the actual lowered form; its descriptor is authored fixture metadata.
    block.AppendNewInst(IR::Opcode::WriteStorageU8, {ir.Imm32(0u), ir.Imm32(0u), ir.Imm32(0xa5u)});
    block.AppendNewInst(IR::Opcode::StorageAtomicAddF32, {ir.Imm32(0u), ir.Imm32(0u), ir.Imm32(1.0f)});
    // Recreate the emitter after appending the lowered instructions, preserving their order.
    IR::IREmitter tail{block};
    (void)tail.GlobalAtomicF32Add(tail.Imm64(u64{0x1000}), tail.Imm32(1.0f));
    (void)tail.GlobalAtomicCompareExchange(tail.Imm64(u64{0x1000}), tail.Imm32(1u), tail.Imm32(2u));
    tail.Epilogue();
    TestEnvironment env;
    Optimization::CollectShaderInfoPass(env, program);
    program.info.constant_buffer_descriptors.push_back({0, 1});
    program.info.storage_buffers_descriptors.push_back({0, 0, 1, true});
    program.info.used_constant_buffer_types |= IR::Type::U32 | IR::Type::U32x2;
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.min_ssbo_alignment = 16;
    const auto module = inspect(save_module("cas-helper-contracts", EmitSPIRV(profile, program)));
    check_cas_functions(module);
}

static void check_vertex_load_controls(bool inline_128, const char* name) {
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = Stage::VertexB;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});

    IR::IREmitter ir{block};
    (void)ir.LoadGlobal32(ir.Imm64(u64{0x1000}));
    (void)ir.LoadGlobal128(ir.Imm64(u64{0x1010}));
    ir.Epilogue();
    TestEnvironment env{program.stage};
    Optimization::CollectShaderInfoPass(env, program);

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.unified_descriptor_binding = true;
    RuntimeInfo runtime_info{};
    runtime_info.inline_global_128_loads = inline_128;
    Backend::Bindings bindings{};
    const auto module = inspect(save_module(name, EmitSPIRV(profile, runtime_info, program, bindings)));

    using Control = spv::FunctionControlMask;
    std::map<u32, Control> function_controls;
    std::vector<Control> load_controls;
    for (const auto& inst : module.instructions) {
        if (inst.op == spv::Op::OpFunction)
            function_controls.emplace(inst.words[1], static_cast<Control>(inst.words[2]));
        if (inst.op == spv::Op::OpFunctionCall)
            load_controls.push_back(function_controls.at(inst.words[2]));
    }
    const Control expected_128 = inline_128 ? spv::FunctionControlInlineMask : spv::FunctionControlDontInlineMask;
    if (load_controls.size() != 2 || load_controls[0] != spv::FunctionControlDontInlineMask ||
        load_controls[1] != expected_128)
        fail("vertex global-load inlining must affect only the 128-bit helper");
    std::printf("PASS: %s\n", name);
}

// (mma_layout.h global_table_loop, RX_VK_GM_LOOP=1) a global load whose only storage buffer is a table of 16 entries, searched in
// the loop: CI validates the module, the return inside the loop's body with it
static void check_table_loop() {
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    IR::IREmitter ir{block};
    (void)ir.LoadGlobal32(ir.Imm64(u64{0x0000000100001000}));
    ir.Epilogue();
    TestEnvironment env;
    Optimization::CollectShaderInfoPass(env, program);
    program.info.constant_buffer_descriptors.push_back({0, 1});
    program.info.storage_buffers_descriptors.push_back({0, 0x40, 16, false});
    program.info.used_constant_buffer_types |= IR::Type::U32 | IR::Type::U32x2;
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.min_ssbo_alignment = 16;
    Shader::Backend::SPIRV::global_table_loop = true;
    const auto module = inspect(save_module("ssbo-table-loop", EmitSPIRV(profile, program)));
    Shader::Backend::SPIRV::global_table_loop = false;
    bool loop = false;
    for (const auto& inst : module.instructions)
        if (inst.op == spv::Op::OpLoopMerge && inst.words[2] == spv::LoopControlDontUnrollMask) loop = true;
    if (!loop) fail("the storage buffer table was not searched in a DontUnroll loop");
    std::printf("PASS: storage buffer table searched in a loop\n");
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--cas-loops") {
        output_dir = argv[2];
        check_cas_program();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--expect-original-stub") {
        check_original_stub_failure();
        return 0;
    }
    if (argc == 2) output_dir = argv[1];
    check_case(true, 0x0000000100000000ull, "signed offset 0 high address");
    check_case(false, 0x0000000100000002ull, "unsigned offset 2 high address");
    check_case(true, 0x0000007f00000ffeull, "signed guest-page end 0xffe");
    check_case(false, 0x0000007f00000ffeull, "unsigned guest-page end 0xffe");
    // These supply concrete cbuf/descriptor inputs to the evaluator for the
    // actual emitted helper branch, including the descriptor-length cap.
    const SingleSsboScenario offset_four{0x1004, 2, 4, true};
    check_case(true, 0x0000000000001004ull, "single SSBO signed halfword at offset four", Stage::Compute, offset_four);
    check_case(false, 0x0000000000001004ull, "single SSBO unsigned halfword at offset four", Stage::Compute, offset_four);
    check_case(false, 0x0000000000001008ull, "single SSBO following word misses logical range", Stage::Compute,
               SingleSsboScenario{0x1004, 2, 4, false});
    check_case(true, 0x0000000000001000ull, "single SSBO aligned-base control", Stage::Compute,
               SingleSsboScenario{0x1000, 2, 4, true});
    check_case(false, 0x0000000000001004ull, "single SSBO descriptor cap admits first word", Stage::Compute,
               SingleSsboScenario{0x1004, 0x40, 2, true});
    check_case(false, 0x0000000000001008ull, "single SSBO descriptor cap excludes end", Stage::Compute,
               SingleSsboScenario{0x1004, 0x40, 2, false});
    check_case(false, 0x0000000000001004ull, "single SSBO zero-length descriptor", Stage::Compute,
               SingleSsboScenario{0x1004, 2, 0, false});
    check_case(true, 0x0000000100001004ull, "single SSBO high-64-bit base", Stage::Compute,
               SingleSsboScenario{0x0000000100001004ull, 2, 4, true});
    check_write_case(true, 0x0000000100000000ull, 16, "signed 16-bit store high address");
    check_write_case(false, 0x0000000100000002ull, 16, "unsigned 16-bit store offset 2");
    check_write_case(true, 0x0000000100000003ull, 8, "signed 8-bit store offset 3");
    check_write_case(false, 0x0000007f00000fffull, 8, "unsigned 8-bit store page end");
    check_case(true, 0x0000000100000000ull, "mesh signed offset 0 high address", Stage::Mesh);
    check_write_case(true, 0x0000000100000000ull, 16, "mesh signed 16-bit store high address", Stage::Mesh);
    check_unsupported_profile();
    check_vertex_load_controls(false, "vertex-global-load-default");
    check_vertex_load_controls(true, "vertex-global-load-inline128");
    check_table_loop();
}
