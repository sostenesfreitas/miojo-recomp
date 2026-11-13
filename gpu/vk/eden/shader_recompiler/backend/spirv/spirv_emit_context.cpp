// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <map>
#include <array>
#include <bit>
#include <climits>
#include <cstdlib>

#include <boost/container/static_vector.hpp>

#include <fmt/ranges.h>

#include "common/common_types.h"
#include "common/div_ceil.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif

namespace Shader::Backend::SPIRV {
namespace {
enum class Operation {
    Increment,
    Decrement,
    FPAdd,
    FPMin,
    FPMax,
};

Id ImageType(EmitContext& ctx, const TextureDescriptor& desc) {
    const spv::ImageFormat format{spv::ImageFormat::Unknown};
    const Id type{desc.is_integer ? ctx.U32[1] : ctx.F32[1]};
    const bool depth{desc.is_depth};
    const bool ms{desc.is_multisample};
    switch (desc.type) {
    case TextureType::Color1D:
        return ctx.TypeImage(type, spv::Dim::Dim1D, depth, false, false, 1, format);
    case TextureType::ColorArray1D:
        return ctx.TypeImage(type, spv::Dim::Dim1D, depth, true, false, 1, format);
    case TextureType::Color2D:
    case TextureType::Color2DRect:
        return ctx.TypeImage(type, spv::Dim::Dim2D, depth, false, ms, 1, format);
    case TextureType::ColorArray2D:
        return ctx.TypeImage(type, spv::Dim::Dim2D, depth, true, ms, 1, format);
    case TextureType::Color3D:
        return ctx.TypeImage(type, spv::Dim::Dim3D, depth, false, false, 1, format);
    case TextureType::ColorCube:
        return ctx.TypeImage(type, spv::Dim::Cube, depth, false, false, 1, format);
    case TextureType::ColorArrayCube:
        return ctx.TypeImage(type, spv::Dim::Cube, depth, true, false, 1, format);
    case TextureType::Buffer:
        break;
    }
    throw InvalidArgument("Invalid texture type {}", desc.type);
}

spv::ImageFormat GetImageFormat(ImageFormat format) {
    switch (format) {
    case ImageFormat::Typeless:
        return spv::ImageFormat::Unknown;
    case ImageFormat::R8_UINT:
        return spv::ImageFormat::R8ui;
    case ImageFormat::R8_SINT:
        return spv::ImageFormat::R8i;
    case ImageFormat::R16_UINT:
        return spv::ImageFormat::R16ui;
    case ImageFormat::R16_SINT:
        return spv::ImageFormat::R16i;
    case ImageFormat::R32_UINT:
        return spv::ImageFormat::R32ui;
    case ImageFormat::R32G32_UINT:
        return spv::ImageFormat::Rg32ui;
    case ImageFormat::R32G32B32A32_UINT:
        return spv::ImageFormat::Rgba32ui;
    }
    throw InvalidArgument("Invalid image format {}", format);
}

Id ImageType(EmitContext& ctx, const ImageDescriptor& desc, Id sampled_type) {
    const spv::ImageFormat format{GetImageFormat(desc.format)};
    switch (desc.type) {
    case TextureType::Color1D:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim1D, false, false, false, 2, format);
    case TextureType::ColorArray1D:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim1D, false, true, false, 2, format);
    case TextureType::Color2D:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim2D, false, false, false, 2, format);
    case TextureType::ColorArray2D:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim2D, false, true, false, 2, format);
    case TextureType::Color3D:
        return ctx.TypeImage(sampled_type, spv::Dim::Dim3D, false, false, false, 2, format);
    case TextureType::Buffer:
        throw NotImplementedException("Image buffer");
    default:
        break;
    }
    throw InvalidArgument("Invalid texture type {}", desc.type);
}

Id DefineVariable(EmitContext& ctx, Id type, std::optional<spv::BuiltIn> builtin,
                  spv::StorageClass storage_class, std::optional<Id> initializer = std::nullopt) {
    const Id pointer_type{ctx.TypePointer(storage_class, type)};
    const Id id{ctx.AddGlobalVariable(pointer_type, storage_class, initializer)};
    if (builtin) {
        ctx.Decorate(id, spv::Decoration::BuiltIn, *builtin);
    }
    ctx.interfaces.push_back(id);
    return id;
}

u32 NumVertices(InputTopology input_topology) {
    switch (input_topology) {
    case InputTopology::Points:
        return 1;
    case InputTopology::Lines:
        return 2;
    case InputTopology::LinesAdjacency:
        return 4;
    case InputTopology::Triangles:
        return 3;
    case InputTopology::TrianglesAdjacency:
        return 6;
    }
    throw InvalidArgument("Invalid input topology {}", input_topology);
}

Id DefineInput(EmitContext& ctx, Id type, bool per_invocation,
               std::optional<spv::BuiltIn> builtin = std::nullopt) {
    switch (ctx.stage) {
    case Stage::TessellationControl:
    case Stage::TessellationEval:
        if (per_invocation) {
            type = ctx.TypeArray(type, ctx.Const(32u));
        }
        break;
    case Stage::Geometry:
        if (per_invocation) {
            const u32 num_vertices{NumVertices(ctx.runtime_info.input_topology)};
            type = ctx.TypeArray(type, ctx.Const(num_vertices));
        }
        break;
    default:
        break;
    }
    return DefineVariable(ctx, type, builtin, spv::StorageClass::Input);
}

Id DefineOutput(EmitContext& ctx, Id type, std::optional<u32> invocations,
                std::optional<spv::BuiltIn> builtin = std::nullopt,
                std::optional<Id> initializer = std::nullopt) {
    if (invocations && ctx.stage == Stage::TessellationControl) {
        type = ctx.TypeArray(type, ctx.Const(*invocations));
    }
    return DefineVariable(ctx, type, builtin, spv::StorageClass::Output, initializer);
}

void DefineGenericOutput(EmitContext& ctx, size_t index, std::optional<u32> invocations) {
    static constexpr std::string_view swizzle{"xyzw"};
    const size_t base_attr_index{static_cast<size_t>(IR::Attribute::Generic0X) + index * 4};
    u32 element{0};
    while (element < 4) {
        const u32 remainder{4 - element};
        const TransformFeedbackVarying* xfb_varying{};
        const size_t xfb_varying_index{base_attr_index + element};
        if (xfb_varying_index < ctx.runtime_info.xfb_count) {
            xfb_varying = &ctx.runtime_info.xfb_varyings[xfb_varying_index];
            xfb_varying = xfb_varying->components > 0 ? xfb_varying : nullptr;
        }
        const u32 num_components{xfb_varying ? xfb_varying->components : remainder};

        const Id id{DefineOutput(ctx, ctx.F32[num_components], invocations)};
        ctx.Decorate(id, spv::Decoration::Location, static_cast<u32>(index));
        if (element > 0) {
            ctx.Decorate(id, spv::Decoration::Component, element);
        }
        if (xfb_varying) {
            ctx.Decorate(id, spv::Decoration::XfbBuffer, xfb_varying->buffer);
            ctx.Decorate(id, spv::Decoration::XfbStride, xfb_varying->stride);
            ctx.Decorate(id, spv::Decoration::Offset, xfb_varying->offset);
            if (ctx.stage == Stage::Geometry && xfb_varying->stream != 0) {
                ctx.Decorate(id, spv::Decoration::Stream, xfb_varying->stream);
            }
        }
        if (num_components < 4 || element > 0) {
            const std::string_view subswizzle{swizzle.substr(element, num_components)};
            ctx.Name(id, fmt::format("out_attr{}_{}", index, subswizzle));
        } else {
            ctx.Name(id, fmt::format("out_attr{}", index));
        }
        const GenericElementInfo info{
            .id = id,
            .first_element = element,
            .num_components = num_components,
        };
        std::fill_n(ctx.output_generics[index].begin() + element, num_components, info);
        element += num_components;
    }
}

Id GetAttributeType(EmitContext& ctx, AttributeType type) {
    switch (type) {
    case AttributeType::Float:
        return ctx.F32[4];
    case AttributeType::SignedInt:
        return ctx.TypeVector(ctx.TypeInt(32, true), 4);
    case AttributeType::UnsignedInt:
        return ctx.U32[4];
    case AttributeType::SignedScaled:
        return ctx.profile.support_scaled_attributes ? ctx.F32[4]
                                                     : ctx.TypeVector(ctx.TypeInt(32, true), 4);
    case AttributeType::UnsignedScaled:
        return ctx.profile.support_scaled_attributes ? ctx.F32[4] : ctx.U32[4];
    case AttributeType::Disabled:
        break;
    }
    throw InvalidArgument("Invalid attribute type {}", type);
}

InputGenericInfo GetAttributeInfo(EmitContext& ctx, AttributeType type, Id id) {
    switch (type) {
    case AttributeType::Float:
        return InputGenericInfo{id, ctx.input_f32, ctx.F32[1], InputGenericLoadOp::None};
    case AttributeType::UnsignedInt:
        return InputGenericInfo{id, ctx.input_u32, ctx.U32[1], InputGenericLoadOp::Bitcast};
    case AttributeType::SignedInt:
        return InputGenericInfo{id, ctx.input_s32, ctx.TypeInt(32, true),
                                InputGenericLoadOp::Bitcast};
    case AttributeType::SignedScaled:
        return ctx.profile.support_scaled_attributes
                   ? InputGenericInfo{id, ctx.input_f32, ctx.F32[1], InputGenericLoadOp::None}
                   : InputGenericInfo{id, ctx.input_s32, ctx.TypeInt(32, true),
                                      InputGenericLoadOp::SToF};
    case AttributeType::UnsignedScaled:
        return ctx.profile.support_scaled_attributes
                   ? InputGenericInfo{id, ctx.input_f32, ctx.F32[1], InputGenericLoadOp::None}
                   : InputGenericInfo{id, ctx.input_u32, ctx.U32[1], InputGenericLoadOp::UToF};
    case AttributeType::Disabled:
        return InputGenericInfo{};
    }
    throw InvalidArgument("Invalid attribute type {}", type);
}

std::string_view StageName(Stage stage) {
    switch (stage) {
    case Stage::VertexA:
        return "vs_a";
    case Stage::VertexB:
        return "vs";
    case Stage::TessellationControl:
        return "tcs";
    case Stage::TessellationEval:
        return "tes";
    case Stage::Geometry:
        return "gs";
    case Stage::Fragment:
        return "fs";
    case Stage::Compute:
        return "cs";
    case Stage::Task:
        return "ts";
    case Stage::Mesh:
        return "ms";
    }
    throw InvalidArgument("Invalid stage {}", stage);
}

template <typename... Args>
void Name(EmitContext& ctx, Id object, std::string_view format_str, Args&&... args) {
    ctx.Name(object, fmt::format(fmt::runtime(format_str), StageName(ctx.stage),
                                 std::forward<Args>(args)...)
                         .c_str());
}

void DefineConstBuffers(EmitContext& ctx, const Info& info, Id UniformDefinitions::*member_type,
                        u32 binding, Id type, char type_char, u32 element_size) {
    const Id array_type{ctx.TypeArray(type, ctx.Const(65536U / element_size))};
    ctx.Decorate(array_type, spv::Decoration::ArrayStride, element_size);

    const Id struct_type{ctx.TypeStruct(array_type)};
    Name(ctx, struct_type, "{}_cbuf_block_{}{}", ctx.stage, type_char, element_size * CHAR_BIT);
    ctx.Decorate(struct_type, spv::Decoration::Block);
    ctx.MemberName(struct_type, 0, "data");
    ctx.MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);

    const Id struct_pointer_type{ctx.TypePointer(spv::StorageClass::Uniform, struct_type)};
    const Id uniform_type{ctx.TypePointer(spv::StorageClass::Uniform, type)};
    ctx.uniform_types.*member_type = uniform_type;

    for (const ConstantBufferDescriptor& desc : info.constant_buffer_descriptors) {
        const Id id{ctx.AddGlobalVariable(struct_pointer_type, spv::StorageClass::Uniform)};
        ctx.Decorate(id, spv::Decoration::Binding, binding);
        ctx.Decorate(id, spv::Decoration::DescriptorSet, 0U);
        ctx.Name(id, fmt::format("c{}", desc.index));
        for (size_t i = 0; i < desc.count; ++i) {
            ctx.cbufs[desc.index + i].*member_type = id;
        }
        if (ctx.profile.supported_spirv >= 0x00010400) {
            ctx.interfaces.push_back(id);
        }
        binding += desc.count;
    }
}

void DefineSsbos(EmitContext& ctx, StorageTypeDefinition& type_def,
                 Id StorageDefinitions::*member_type, const Info& info, u32 binding, Id type,
                 u32 stride) {
    const Id array_type{ctx.TypeRuntimeArray(type)};
    ctx.Decorate(array_type, spv::Decoration::ArrayStride, stride);

    const Id struct_type{ctx.TypeStruct(array_type)};
    ctx.Decorate(struct_type, spv::Decoration::Block);
    ctx.MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);

    const Id struct_pointer{ctx.TypePointer(spv::StorageClass::StorageBuffer, struct_type)};
    type_def.array = struct_pointer;
    type_def.element = ctx.TypePointer(spv::StorageClass::StorageBuffer, type);

    u32 index{};
    size_t table{};
    for (const StorageBufferDescriptor& desc : info.storage_buffers_descriptors) {
        if (desc.count > 1) {   // (hookshot: a storage buffer table, Info::global_tables)
            const Id array{ctx.TypeArray(struct_type, ctx.Const(desc.count))};
            const Id id{ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::StorageBuffer, array),
                                              spv::StorageClass::StorageBuffer)};
            ctx.Decorate(id, spv::Decoration::Binding, binding);
            ctx.Decorate(id, spv::Decoration::DescriptorSet, 0U);
            ctx.Name(id, fmt::format("ssbo_table{}", table));
            if (ctx.profile.supported_spirv >= 0x00010400) {
                ctx.interfaces.push_back(id);
            }
            if (ctx.ssbo_tables.size() <= table) {
                ctx.ssbo_tables.push_back({desc.cbuf_index, desc.cbuf_offset, desc.count, {}});
            }
            ctx.ssbo_tables[table++].vars.*member_type = id;
            binding += 1;
            continue;
        }
        const Id id{ctx.AddGlobalVariable(struct_pointer, spv::StorageClass::StorageBuffer)};
        ctx.Decorate(id, spv::Decoration::Binding, binding);
        ctx.Decorate(id, spv::Decoration::DescriptorSet, 0U);
        ctx.Name(id, fmt::format("ssbo{}", index));
        if (ctx.profile.supported_spirv >= 0x00010400) {
            ctx.interfaces.push_back(id);
        }
        for (size_t i = 0; i < desc.count; ++i) {
            ctx.ssbos[index + i].*member_type = id;
        }
        index += desc.count;
        binding += desc.count;
    }
}

Id CasFunction(EmitContext& ctx, Operation operation, Id value_type) {
    const Id func_type{ctx.TypeFunction(value_type, value_type, value_type)};
    const Id func{ctx.OpFunction(value_type, spv::FunctionControlMask::MaskNone, func_type)};
    const Id op_a{ctx.OpFunctionParameter(value_type)};
    const Id op_b{ctx.OpFunctionParameter(value_type)};
    ctx.AddLabel();
    Id result{};
    switch (operation) {
    case Operation::Increment: {
        const Id pred{ctx.OpUGreaterThanEqual(ctx.U1, op_a, op_b)};
        const Id incr{ctx.OpIAdd(value_type, op_a, ctx.Constant(value_type, 1))};
        result = ctx.OpSelect(value_type, pred, ctx.u32_zero_value, incr);
        break;
    }
    case Operation::Decrement: {
        const Id lhs{ctx.OpIEqual(ctx.U1, op_a, ctx.Constant(value_type, 0u))};
        const Id rhs{ctx.OpUGreaterThan(ctx.U1, op_a, op_b)};
        const Id pred{ctx.OpLogicalOr(ctx.U1, lhs, rhs)};
        const Id decr{ctx.OpISub(value_type, op_a, ctx.Constant(value_type, 1))};
        result = ctx.OpSelect(value_type, pred, op_b, decr);
        break;
    }
    case Operation::FPAdd:
        result = ctx.OpFAdd(value_type, op_a, op_b);
        break;
    case Operation::FPMin:
        result = ctx.OpNMin(value_type, op_a, op_b);   // (amd-fix, Lichii #432)
        break;
    case Operation::FPMax:
        result = ctx.OpNMax(value_type, op_a, op_b);   // (amd-fix, Lichii #432)
        break;
    default:
        break;
    }
    ctx.OpReturnValue(result);
    ctx.OpFunctionEnd();
    return func;
}

// (hookshot, issue 17) A ceiling on a compare-exchange loop's attempts. Every attempt that fails does so because
// another invocation's succeeded, so a contended word is always written in far fewer tries than this; the ceiling
// only matters when the exchange never returns what it was given, as for a word out of bounds under robust buffer
// access (seen on Intel Arc B390: no device loss, the frame never came back). One attempt counts: the condition to
// leave the loop is success, or this many attempts made.
constexpr u32 CAS_LOOP_MAX_TRIES = 1024;

Id CasTriesVariable(EmitContext& ctx) {
    const Id tries_var{ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, ctx.U32[1]),
                                            spv::StorageClass::Function)};
    ctx.OpStore(tries_var, ctx.u32_zero_value);
    return tries_var;
}

Id CasLoopDone(EmitContext& ctx, Id tries_var, Id success) {
    const Id tries{ctx.OpIAdd(ctx.U32[1], ctx.OpLoad(ctx.U32[1], tries_var), ctx.Const(1U))};
    ctx.OpStore(tries_var, tries);
    return ctx.OpLogicalOr(ctx.U1, success,
                           ctx.OpUGreaterThanEqual(ctx.U1, tries, ctx.Const(CAS_LOOP_MAX_TRIES)));
}

Id CasLoop(EmitContext& ctx, Operation operation, Id array_pointer, Id element_pointer,
           Id value_type, spv::Scope scope) {
    const bool is_shared{scope == spv::Scope::Workgroup};
    const bool is_struct{!is_shared || ctx.uses_explicit_workgroup_layout};
    const Id cas_func{CasFunction(ctx, operation, value_type)};
    const Id zero{ctx.u32_zero_value};
    const Id scope_id{ctx.Const(static_cast<u32>(scope))};

    const Id loop_header{ctx.OpLabel()};
    const Id continue_block{ctx.OpLabel()};
    const Id merge_block{ctx.OpLabel()};
    const Id func_type{is_shared
                           ? ctx.TypeFunction(value_type, ctx.U32[1], value_type)
                           : ctx.TypeFunction(value_type, ctx.U32[1], value_type, array_pointer)};

    const Id func{ctx.OpFunction(value_type, spv::FunctionControlMask::MaskNone, func_type)};
    const Id index{ctx.OpFunctionParameter(ctx.U32[1])};
    const Id op_b{ctx.OpFunctionParameter(value_type)};
    const Id base{is_shared ? ctx.shared_memory_u32 : ctx.OpFunctionParameter(array_pointer)};
    ctx.AddLabel();
    // Each attempt expects the word the previous compare-exchange returned. A plain reload can
    // keep returning a stale copy (AMD), and the invocations that lost would never match again.
    const Id expected_var{ctx.AddLocalVariable(
        ctx.TypePointer(spv::StorageClass::Function, ctx.U32[1]), spv::StorageClass::Function)};
    // (the attempt count's variable next: a function's OpVariables come before any other instruction)
    const Id tries_var{CasTriesVariable(ctx)};
    const Id word_pointer{is_struct ? ctx.OpAccessChain(element_pointer, base, zero, index)
                                    : ctx.OpAccessChain(element_pointer, base, index)};
    ctx.OpStore(expected_var, ctx.OpLoad(ctx.U32[1], word_pointer));
    ctx.OpBranch(loop_header);
    ctx.AddLabel(loop_header);

    ctx.OpLoopMerge(merge_block, continue_block, spv::LoopControlMask::MaskNone);
    ctx.OpBranch(continue_block);

    ctx.AddLabel(continue_block);
    // The word holds a u32, an f32, an f16x2, or an f32x2 packed as two halves.
    const bool packed_half{value_type.value == ctx.F32[2].value};
    const bool is_u32{value_type.value == ctx.U32[1].value};
    const auto from_word{[&](Id word) {
        if (packed_half) {
            return ctx.OpUnpackHalf2x16(ctx.F32[2], word);
        }
        return is_u32 ? word : ctx.OpBitcast(value_type, word);
    }};
    const Id expected{ctx.OpLoad(ctx.U32[1], expected_var)};
    const Id new_value{ctx.OpFunctionCall(value_type, cas_func, from_word(expected), op_b)};
    const Id new_word{packed_half ? ctx.OpPackHalf2x16(ctx.U32[1], new_value)
                      : is_u32    ? new_value
                                  : ctx.OpBitcast(ctx.U32[1], new_value)};
    const Id atomic_res{ctx.OpAtomicCompareExchange(ctx.U32[1], word_pointer, scope_id, zero, zero,
                                                    new_word, expected)};
    ctx.OpStore(expected_var, atomic_res);
    const Id success{ctx.OpIEqual(ctx.U1, atomic_res, expected)};
    ctx.OpBranchConditional(CasLoopDone(ctx, tries_var, success), merge_block, loop_header);

    ctx.AddLabel(merge_block);
    ctx.OpReturnValue(from_word(atomic_res));
    ctx.OpFunctionEnd();
    return func;
}

template <typename Desc>
std::string NameOf(Stage stage, const Desc& desc, std::string_view prefix) {
    if (desc.count > 1) {
        return fmt::format("{}_{}{}_{:02x}x{}", StageName(stage), prefix, desc.cbuf_index,
                           desc.cbuf_offset, desc.count);
    } else {
        return fmt::format("{}_{}{}_{:02x}", StageName(stage), prefix, desc.cbuf_index,
                           desc.cbuf_offset);
    }
}

Id DescType(EmitContext& ctx, Id sampled_type, Id pointer_type, u32 count) {
    if (count > 1) {
        const Id array_type{ctx.TypeArray(sampled_type, ctx.Const(count))};
        return ctx.TypePointer(spv::StorageClass::UniformConstant, array_type);
    } else {
        return pointer_type;
    }
}
} // Anonymous namespace

void VectorTypes::Define(Sirit::Module& sirit_ctx, Id base_type, std::string_view name) {
    defs[0] = sirit_ctx.Name(base_type, name);

    std::array<char, 6> def_name;
    for (int i = 1; i < 4; ++i) {
        const std::string_view def_name_view(
            def_name.data(),
            fmt::format_to_n(def_name.data(), def_name.size(), "{}x{}", name, i + 1).size);
        defs[static_cast<size_t>(i)] =
            sirit_ctx.Name(sirit_ctx.TypeVector(base_type, i + 1), def_name_view);
    }
}

EmitContext::EmitContext(const Profile& profile_, const RuntimeInfo& runtime_info_,
                         IR::Program& program, Bindings& bindings)
    : Sirit::Module(profile_.supported_spirv), profile{profile_}, runtime_info{runtime_info_},
      stage{program.stage}, texture_rescaling_index{bindings.texture_scaling_index},
      image_rescaling_index{bindings.image_scaling_index} {
    const bool is_unified{profile.unified_descriptor_binding};
    u32& uniform_binding{is_unified ? bindings.unified : bindings.uniform_buffer};
    u32& storage_binding{is_unified ? bindings.unified : bindings.storage_buffer};
    u32& texture_binding{is_unified ? bindings.unified : bindings.texture};
    u32& image_binding{is_unified ? bindings.unified : bindings.image};
    AddCapability(spv::Capability::Shader);
    if (runtime_info.mesh_fallback || runtime_info.mesh_fallback_vertex) {
        wants_mesh_fallback = true;   // (before any DefineRescalingInputPushConstant, which is made once)
    }
    DefineCommonTypes(program.info);
    DefineCommonConstants();
    DefineInterfaces(program);
    // (RX_VK_MESH_FIRST=1, with RX_VK_MESH_ARGS=1: off, one task program with it hung or ended the game at its first draw)
    if (stage == Stage::Task && !runtime_info.mesh_fallback && env_on("RX_VK_MESH_FIRST") &&
        profile.unified_descriptor_binding && profile.support_int64) {
        // (hookshot) the draw's first task workgroup, read through a pushed address (EmitIsbeRead)
        if (!Sirit::ValidId(U64)) { AddCapability(spv::Capability::Int64); U64 = Name(TypeInt(64, false), "u64"); }
        wants_mesh_first = true;
        AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        AddExtension("SPV_KHR_physical_storage_buffer");
        SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
        if (!Sirit::ValidId(draw_index)) {
            draw_index = DefineInput(*this, U32[1], true, spv::BuiltIn::DrawIndex);
        }
        const Id block{TypeStruct(U32[1])};
        Decorate(block, spv::Decoration::Block);
        MemberDecorate(block, 0, spv::Decoration::Offset, 0U);
        mesh_first_block_pointer = TypePointer(spv::StorageClass::PhysicalStorageBuffer, block);
        mesh_first_member_pointer = TypePointer(spv::StorageClass::PhysicalStorageBuffer, U32[1]);
        DefineRescalingInputPushConstant();
    }
    if (runtime_info.mesh_fallback && (stage == Stage::Task || stage == Stage::Mesh)) {
        mesh_fallback = true;
        wants_mesh_fallback = true;
        wants_mesh_first = stage == Stage::Task;
        // (the push block is made below, before DefineGlobalMemoryFunctions can ask for the page table: a fallback stage
        // that follows pointers needs its member now, or the block would be made without it)
        wants_guest_pages = program.info.uses_global_memory && profile.support_int64;
        if (!Sirit::ValidId(U64)) {
            AddCapability(spv::Capability::Int64);
            U64 = Name(TypeInt(64, false), "u64");
        }
        AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        AddExtension("SPV_KHR_physical_storage_buffer");
        SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
        DefineRescalingInputPushConstant();
    }
    if (runtime_info.mesh_fallback_vertex) {   // (the generated vertex stage: pushes the records' address)
        AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        AddExtension("SPV_KHR_physical_storage_buffer");
        SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
        if (!Sirit::ValidId(U64)) {
            AddCapability(spv::Capability::Int64);
            U64 = Name(TypeInt(64, false), "u64");
        }
        mesh_info = program.mesh;
        DefineRescalingInputPushConstant();
    }
    DefineMesh(program);
    DefineLocalMemory(program);
    DefineSharedMemory(program);
    DefineSharedMemoryFunctions(program);
    DefineConstantBuffers(program.info, uniform_binding);
    DefineConstantBufferIndirectFunctions(program.info);
    DefineStorageBuffers(program.info, storage_binding);
    DefineTextureBuffers(program.info, texture_binding);
    DefineImageBuffers(program.info, image_binding);
    DefineTextures(program.info, texture_binding, bindings.texture_scaling_index);
    DefineImages(program.info, image_binding, bindings.image_scaling_index);
    DefineAttributeMemAccess(program.info);
    DefineWriteStorageCasLoopFunction(program.info);
    DefineGlobalMemoryFunctions(program.info);
    DefineRescalingInput(program.info);
    DefineRenderArea(program.info);
}

EmitContext::~EmitContext() = default;

Id EmitContext::Def(const IR::Value& value) {
    if (!value.IsImmediate()) {
        return value.InstRecursive()->Definition<Id>();
    }
    switch (value.Type()) {
    case IR::Type::Void:
        // Void instructions are used for optional arguments (e.g. texture offsets)
        // They are not meant to be used in the SPIR-V module
        return Id{};
    case IR::Type::U1:
        return value.U1() ? true_value : false_value;
    case IR::Type::U32:
        return Const(value.U32());
    case IR::Type::U64:
        return Constant(U64, value.U64());
    case IR::Type::F32:
        return Const(value.F32());
    case IR::Type::F64:
        return Constant(F64[1], value.F64());
    default:
        throw NotImplementedException("Immediate type {}", value.Type());
    }
}

Id EmitContext::BitOffset8(const IR::Value& offset) {
    if (offset.IsImmediate()) {
        return Const((offset.U32() % 4) * 8);
    }
    return OpBitwiseAnd(U32[1], OpShiftLeftLogical(U32[1], Def(offset), Const(3u)), Const(24u));
}

Id EmitContext::BitOffset16(const IR::Value& offset) {
    if (offset.IsImmediate()) {
        return Const(((offset.U32() / 2) % 2) * 16);
    }
    return OpBitwiseAnd(U32[1], OpShiftLeftLogical(U32[1], Def(offset), Const(3u)), Const(16u));
}

void EmitContext::DefineCommonTypes(const Info& info) {
    void_id = TypeVoid();

    U1 = Name(TypeBool(), "u1");

    F32.Define(*this, TypeFloat(32), "f32");
    U32.Define(*this, TypeInt(32, false), "u32");
    S32.Define(*this, TypeInt(32, true), "s32");

    private_u32 = Name(TypePointer(spv::StorageClass::Private, U32[1]), "private_u32");

    input_f32 = Name(TypePointer(spv::StorageClass::Input, F32[1]), "input_f32");
    input_u32 = Name(TypePointer(spv::StorageClass::Input, U32[1]), "input_u32");
    input_s32 = Name(TypePointer(spv::StorageClass::Input, TypeInt(32, true)), "input_s32");

    output_f32 = Name(TypePointer(spv::StorageClass::Output, F32[1]), "output_f32");
    output_u32 = Name(TypePointer(spv::StorageClass::Output, U32[1]), "output_u32");

    if (info.uses_int8 && profile.support_int8) {
        AddCapability(spv::Capability::Int8);
        U8 = Name(TypeInt(8, false), "u8");
        S8 = Name(TypeInt(8, true), "s8");
    }
    if (info.uses_int16 && profile.support_int16) {
        AddCapability(spv::Capability::Int16);
        U16 = Name(TypeInt(16, false), "u16");
        S16 = Name(TypeInt(16, true), "s16");
    }
    if (info.uses_int64 && profile.support_int64) {
        AddCapability(spv::Capability::Int64);
        U64 = Name(TypeInt(64, false), "u64");
    }
    if (info.uses_fp16) {
        AddCapability(spv::Capability::Float16);
        F16.Define(*this, TypeFloat(16), "f16");
    }
    if (info.uses_fp64) {
        AddCapability(spv::Capability::Float64);
        F64.Define(*this, TypeFloat(64), "f64");
    }
}

void EmitContext::DefineCommonConstants() {
    true_value = ConstantTrue(U1);
    false_value = ConstantFalse(U1);
    u32_zero_value = Const(0U);
    f32_zero_value = Const(0.0f);
}

void EmitContext::DefineInterfaces(const IR::Program& program) {
    DefineInputs(program);
    DefineOutputs(program);
}

void EmitContext::DefineLocalMemory(const IR::Program& program) {
    if (program.local_memory_size == 0) {
        return;
    }
    const u32 num_elements{Common::DivCeil(program.local_memory_size, 4U)};
    const Id element_type{TypeStruct(U32[1])};
    const Id type{TypeArray(element_type, Const(num_elements))};
    const Id pointer{TypePointer(spv::StorageClass::Private, type)};
    // (NX2navi ee6fbaf, maxwell.md 3.12) Local memory read before the shader writes it: the game's code finds 0 there on the
    // Switch; a Private array with no initializer is undefined, and NVIDIA's driver hands such a read another warp lane's
    // values (Ocarina's clustered-light loop then added other pixels' light: objects lit neon, floors in 4x8 tiles). It
    // starts at zero now. EDEN_NX2_LOCAL_ZERO=0: no initializer (the old behaviour)
    static const bool zero_local = [] {
        const char* const value = std::getenv("EDEN_NX2_LOCAL_ZERO");
        return value == nullptr || value[0] != '0';
    }();
    local_memory = zero_local ? AddGlobalVariable(pointer, spv::StorageClass::Private, ConstantNull(type))
                              : AddGlobalVariable(pointer, spv::StorageClass::Private);
    if (profile.supported_spirv >= 0x00010400) {
        interfaces.push_back(local_memory);
    }
}

void EmitContext::DefineSharedMemory(const IR::Program& program) {
    uses_explicit_workgroup_layout =
        profile.support_explicit_workgroup_layout &&
        (!program.info.uses_int8 || profile.support_workgroup_layout_8bit_access) &&
        (!program.info.uses_int16 || profile.support_workgroup_layout_16bit_access);
    if (program.shared_memory_size == 0) {
        return;
    }
    const auto make{[&](Id element_type, u32 element_size) {
        const u32 num_elements{Common::DivCeil(program.shared_memory_size, element_size)};
        const Id array_type{TypeArray(element_type, Const(num_elements))};
        Decorate(array_type, spv::Decoration::ArrayStride, element_size);

        const Id struct_type{TypeStruct(array_type)};
        MemberDecorate(struct_type, 0U, spv::Decoration::Offset, 0U);
        Decorate(struct_type, spv::Decoration::Block);

        const Id pointer{TypePointer(spv::StorageClass::Workgroup, struct_type)};
        const Id element_pointer{TypePointer(spv::StorageClass::Workgroup, element_type)};
        const Id variable{AddGlobalVariable(pointer, spv::StorageClass::Workgroup)};
        Decorate(variable, spv::Decoration::Aliased);
        interfaces.push_back(variable);

        return std::make_tuple(variable, element_pointer, pointer);
    }};
    if (uses_explicit_workgroup_layout) {
        AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
        AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
        if (program.info.uses_int8 && profile.support_int8) {
            AddCapability(spv::Capability::WorkgroupMemoryExplicitLayout8BitAccessKHR);
            std::tie(shared_memory_u8, shared_u8, std::ignore) = make(U8, 1);
        }
        if (program.info.uses_int16 && profile.support_int16) {
            AddCapability(spv::Capability::WorkgroupMemoryExplicitLayout16BitAccessKHR);
            std::tie(shared_memory_u16, shared_u16, std::ignore) = make(U16, 2);
        }
        if (program.info.uses_int64 && profile.support_int64) {
            std::tie(shared_memory_u64, shared_u64, std::ignore) = make(U64, 8);
        }
        std::tie(shared_memory_u32, shared_u32, shared_memory_u32_type) = make(U32[1], 4);
        std::tie(shared_memory_u32x2, shared_u32x2, std::ignore) = make(U32[2], 8);
        std::tie(shared_memory_u32x4, shared_u32x4, std::ignore) = make(U32[4], 16);
        return;
    }
    const u32 num_elements{Common::DivCeil(program.shared_memory_size, 4U)};
    const Id type{TypeArray(U32[1], Const(num_elements))};
    shared_memory_u32_type = TypePointer(spv::StorageClass::Workgroup, type);

    shared_u32 = TypePointer(spv::StorageClass::Workgroup, U32[1]);
    shared_memory_u32 = AddGlobalVariable(shared_memory_u32_type, spv::StorageClass::Workgroup);
    interfaces.push_back(shared_memory_u32);

    const Id func_type{TypeFunction(void_id, U32[1], U32[1])};
    const auto make_function{[&](u32 mask, u32 size) {
        const Id loop_header{OpLabel()};
        const Id continue_block{OpLabel()};
        const Id merge_block{OpLabel()};

        const Id func{OpFunction(void_id, spv::FunctionControlMask::MaskNone, func_type)};
        const Id offset{OpFunctionParameter(U32[1])};
        const Id insert_value{OpFunctionParameter(U32[1])};
        AddLabel();
        const Id expected_var{AddLocalVariable(TypePointer(spv::StorageClass::Function, U32[1]),
                                               spv::StorageClass::Function)};
        const Id tries_var{CasTriesVariable(*this)};   // (with the other variable, before any other instruction)
        const Id word_offset{OpShiftRightArithmetic(U32[1], offset, Const(2U))};
        const Id shift_offset{OpShiftLeftLogical(U32[1], offset, Const(3U))};
        const Id bit_offset{OpBitwiseAnd(U32[1], shift_offset, Const(mask))};
        const Id count{Const(size)};
        const Id word_pointer{OpAccessChain(shared_u32, shared_memory_u32, word_offset)};
        OpStore(expected_var, OpLoad(U32[1], word_pointer));
        OpBranch(loop_header);

        AddLabel(loop_header);
        OpLoopMerge(merge_block, continue_block, spv::LoopControlMask::MaskNone);
        OpBranch(continue_block);

        AddLabel(continue_block);
        const Id old_value{OpLoad(U32[1], expected_var)};
        const Id new_value{OpBitFieldInsert(U32[1], old_value, insert_value, bit_offset, count)};
        const Id atomic_res{OpAtomicCompareExchange(U32[1], word_pointer, Const(1U), u32_zero_value,
                                                    u32_zero_value, new_value, old_value)};
        OpStore(expected_var, atomic_res);
        const Id success{OpIEqual(U1, atomic_res, old_value)};
        OpBranchConditional(CasLoopDone(*this, tries_var, success), merge_block, loop_header);

        AddLabel(merge_block);
        OpReturn();
        OpFunctionEnd();
        return func;
    }};
    if (program.info.uses_int8) {
        shared_store_u8_func = make_function(24, 8);
    }
    if (program.info.uses_int16) {
        shared_store_u16_func = make_function(16, 16);
    }
}

void EmitContext::DefineSharedMemoryFunctions(const IR::Program& program) {
    if (program.info.uses_shared_increment) {
        increment_cas_shared = CasLoop(*this, Operation::Increment, shared_memory_u32_type,
                                       shared_u32, U32[1], spv::Scope::Workgroup);
    }
    if (program.info.uses_shared_decrement) {
        decrement_cas_shared = CasLoop(*this, Operation::Decrement, shared_memory_u32_type,
                                       shared_u32, U32[1], spv::Scope::Workgroup);
    }
}

void EmitContext::DefineAttributeMemAccess(const Info& info) {
    const auto make_load{[&] {
        const bool is_array{stage == Stage::Geometry};
        const Id end_block{OpLabel()};
        const Id default_label{OpLabel()};

        const Id func_type_load{is_array ? TypeFunction(F32[1], U32[1], U32[1])
                                         : TypeFunction(F32[1], U32[1])};
        const Id func{OpFunction(F32[1], spv::FunctionControlMask::MaskNone, func_type_load)};
        const Id offset{OpFunctionParameter(U32[1])};
        const Id vertex{is_array ? OpFunctionParameter(U32[1]) : Id{}};

        AddLabel();
        const Id base_index{OpShiftRightArithmetic(U32[1], offset, Const(2U))};
        const Id masked_index{OpBitwiseAnd(U32[1], base_index, Const(3U))};
        const Id compare_index{OpShiftRightArithmetic(U32[1], base_index, Const(2U))};
        std::vector<Sirit::Literal> literals;
        std::vector<Id> labels;
        if (info.loads.AnyComponent(IR::Attribute::PositionX)) {
            literals.push_back(static_cast<u32>(IR::Attribute::PositionX) >> 2);
            labels.push_back(OpLabel());
        }
        const u32 base_attribute_value = static_cast<u32>(IR::Attribute::Generic0X) >> 2;
        for (u32 index = 0; index < static_cast<u32>(IR::NUM_GENERICS); ++index) {
            if (!info.loads.Generic(index)) {
                continue;
            }
            literals.push_back(base_attribute_value + index);
            labels.push_back(OpLabel());
        }
        OpSelectionMerge(end_block, spv::SelectionControlMask::MaskNone);
        OpSwitch(compare_index, default_label, literals, labels);
        AddLabel(default_label);
        OpReturnValue(Const(0.0f));
        size_t label_index{0};
        if (info.loads.AnyComponent(IR::Attribute::PositionX)) {
            AddLabel(labels[label_index]);
            const Id pointer{[&]() {
                if (need_input_position_indirect) {
                    if (is_array)
                        return OpAccessChain(input_f32, input_position, vertex, u32_zero_value,
                                             masked_index);
                    else
                        return OpAccessChain(input_f32, input_position, u32_zero_value,
                                             masked_index);
                } else {
                    if (is_array)
                        return OpAccessChain(input_f32, input_position, vertex, masked_index);
                    else
                        return OpAccessChain(input_f32, input_position, masked_index);
                }
            }()};
            const Id result{OpLoad(F32[1], pointer)};
            OpReturnValue(result);
            ++label_index;
        }
        for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
            if (!info.loads.Generic(index)) {
                continue;
            }
            AddLabel(labels[label_index]);
            const auto& generic{input_generics.at(index)};
            const Id generic_id{generic.id};
            if (!ValidId(generic_id)) {
                OpReturnValue(Const(0.0f));
                ++label_index;
                continue;
            }
            const Id pointer{
                is_array ? OpAccessChain(generic.pointer_type, generic_id, vertex, masked_index)
                         : OpAccessChain(generic.pointer_type, generic_id, masked_index)};
            const Id value{OpLoad(generic.component_type, pointer)};
            const Id result{[this, generic, value]() {
                switch (generic.load_op) {
                case InputGenericLoadOp::Bitcast:
                    return OpBitcast(F32[1], value);
                case InputGenericLoadOp::SToF:
                    return OpConvertSToF(F32[1], value);
                case InputGenericLoadOp::UToF:
                    return OpConvertUToF(F32[1], value);
                default:
                    return value;
                };
            }()};
            OpReturnValue(result);
            ++label_index;
        }
        AddLabel(end_block);
        OpUnreachable();
        OpFunctionEnd();
        return func;
    }};
    const auto make_store{[&] {
        const Id end_block{OpLabel()};
        const Id default_label{OpLabel()};

        const Id func_type_store{TypeFunction(void_id, U32[1], F32[1])};
        const Id func{OpFunction(void_id, spv::FunctionControlMask::MaskNone, func_type_store)};
        const Id offset{OpFunctionParameter(U32[1])};
        const Id store_value{OpFunctionParameter(F32[1])};
        AddLabel();
        const Id base_index{OpShiftRightArithmetic(U32[1], offset, Const(2U))};
        const Id masked_index{OpBitwiseAnd(U32[1], base_index, Const(3U))};
        const Id compare_index{OpShiftRightArithmetic(U32[1], base_index, Const(2U))};
        std::vector<Sirit::Literal> literals;
        std::vector<Id> labels;
        if (info.stores.AnyComponent(IR::Attribute::PositionX)) {
            literals.push_back(static_cast<u32>(IR::Attribute::PositionX) >> 2);
            labels.push_back(OpLabel());
        }
        const u32 base_attribute_value = static_cast<u32>(IR::Attribute::Generic0X) >> 2;
        for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
            if (!info.stores.Generic(index)) {
                continue;
            }
            literals.push_back(base_attribute_value + static_cast<u32>(index));
            labels.push_back(OpLabel());
        }
        if (info.stores.ClipDistances()) {
            if (profile.max_user_clip_distances >= 4) {
                literals.push_back(static_cast<u32>(IR::Attribute::ClipDistance0) >> 2);
                labels.push_back(OpLabel());
            }
            if (profile.max_user_clip_distances >= 8) {
                literals.push_back(static_cast<u32>(IR::Attribute::ClipDistance4) >> 2);
                labels.push_back(OpLabel());
            }
        }
        OpSelectionMerge(end_block, spv::SelectionControlMask::MaskNone);
        OpSwitch(compare_index, default_label, literals, labels);
        AddLabel(default_label);
        OpReturn();
        size_t label_index{0};
        if (info.stores.AnyComponent(IR::Attribute::PositionX)) {
            AddLabel(labels[label_index]);
            const Id pointer{OpAccessChain(output_f32, output_position, masked_index)};
            OpStore(pointer, store_value);
            OpReturn();
            ++label_index;
        }
        for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
            if (!info.stores.Generic(index)) {
                continue;
            }
            if (output_generics[index][0].num_components != 4) {
                throw NotImplementedException("Physical stores and transform feedbacks");
            }
            AddLabel(labels[label_index]);
            const Id generic_id{output_generics[index][0].id};
            const Id pointer{OpAccessChain(output_f32, generic_id, masked_index)};
            OpStore(pointer, store_value);
            OpReturn();
            ++label_index;
        }
        if (info.stores.ClipDistances()) {
            if (profile.max_user_clip_distances >= 4) {
                AddLabel(labels[label_index]);
                const Id pointer{OpAccessChain(output_f32, clip_distances, masked_index)};
                OpStore(pointer, store_value);
                OpReturn();
                ++label_index;
            }
            if (profile.max_user_clip_distances >= 8) {
                AddLabel(labels[label_index]);
                const Id fixed_index{OpIAdd(U32[1], masked_index, Const(4U))};
                const Id pointer{OpAccessChain(output_f32, clip_distances, fixed_index)};
                OpStore(pointer, store_value);
                OpReturn();
                ++label_index;
            }
        }
        AddLabel(end_block);
        OpUnreachable();
        OpFunctionEnd();
        return func;
    }};
    if (info.loads_indexed_attributes) {
        indexed_load_func = make_load();
    }
    if (info.stores_indexed_attributes) {
        indexed_store_func = make_store();
    }
}

void EmitContext::DefineWriteStorageCasLoopFunction(const Info& info) {
    if (profile.support_int8 && profile.support_int16) {
        return;
    }
    if (!info.uses_int8 && !info.uses_int16) {
        return;
    }

    AddCapability(spv::Capability::VariablePointersStorageBuffer);

    const Id ptr_type{TypePointer(spv::StorageClass::StorageBuffer, U32[1])};
    const Id func_type{TypeFunction(void_id, ptr_type, U32[1], U32[1], U32[1])};
    const Id func{OpFunction(void_id, spv::FunctionControlMask::MaskNone, func_type)};
    const Id pointer{OpFunctionParameter(ptr_type)};
    const Id value{OpFunctionParameter(U32[1])};
    const Id bit_offset{OpFunctionParameter(U32[1])};
    const Id bit_count{OpFunctionParameter(U32[1])};

    AddLabel();
    // Each attempt expects the word the previous compare-exchange returned (see CasLoop).
    const Id expected_var{
        AddLocalVariable(TypePointer(spv::StorageClass::Function, U32[1]), spv::StorageClass::Function)};
    const Id tries_var{CasTriesVariable(*this)};   // (with the other variable, before any other instruction)
    OpStore(expected_var, OpLoad(U32[1], pointer));
    const Id scope_device{Const(1u)};
    const Id ordering_relaxed{u32_zero_value};
    const Id body_label{OpLabel()};
    const Id continue_label{OpLabel()};
    const Id endloop_label{OpLabel()};
    const Id beginloop_label{OpLabel()};
    OpBranch(beginloop_label);

    AddLabel(beginloop_label);
    OpLoopMerge(endloop_label, continue_label, spv::LoopControlMask::MaskNone);
    OpBranch(body_label);

    AddLabel(body_label);
    const Id expected_value{OpLoad(U32[1], expected_var)};
    const Id desired_value{OpBitFieldInsert(U32[1], expected_value, value, bit_offset, bit_count)};
    const Id actual_value{OpAtomicCompareExchange(U32[1], pointer, scope_device, ordering_relaxed,
                                                  ordering_relaxed, desired_value, expected_value)};
    OpStore(expected_var, actual_value);
    const Id store_successful{OpIEqual(U1, expected_value, actual_value)};
    OpBranchConditional(CasLoopDone(*this, tries_var, store_successful), endloop_label, continue_label);

    AddLabel(endloop_label);
    OpReturn();

    AddLabel(continue_label);
    OpBranch(beginloop_label);

    OpFunctionEnd();

    write_storage_cas_loop_func = func;
}

// (hookshot) The page table of guest memory for pointers no bound buffer holds: 4 KB pages from this base, a device address each
constexpr unsigned long long kGuestPagesBase = 0x1000000000ull;
constexpr unsigned long long kGuestPagesBytes = 8ull << 30;
// (a second window, its entries after the first's: the pools the game's GPU allocator puts at 0x7fxxxxxxxx, where its storage
// buffers are. Pointers there read 0 with the first alone)
constexpr unsigned long long kGuestPagesBase2 = 0x7f00000000ull;
constexpr unsigned long long kGuestPagesBytes2 = 4ull << 30;

void EmitContext::DefineGlobalMemoryFunctions(const Info& info) {
    if (!info.uses_global_memory || !profile.support_int64) {
        return;
    }
    // (hookshot) A pointer that is in no bound buffer. Eden gave such a load 0, and the game's foliage culling follows pointers read from its
    // storage buffers to all of a forest's data: it read zeros, and left out every tree, fern and blade of grass. The console's GPU just reads
    // memory: here a page table (vk_memory.cpp) takes the guest address to its pool's device address.
    const char* const gp_test{getenv("RX_VK_GP_TEST")};   // (1: the addressing model alone, 2: and the table's load, 3 or unset: all of it)
    const int gp_level{gp_test ? atoi(gp_test) : 3};
    // (the culling; a graphics stage with it hung the renderer at a load. RX_VK_NO_FOLLOW: none of it, as before)
    static const bool no_follow{env_on("RX_VK_NO_FOLLOW")};
    // (not the task and mesh stages: tried for the foliage, which it did not bring back)
    // (EvilOOT: vertex and fragment stages too, as the fallback only: an access no bound buffer holds read 0 there (Kokiri's
    // pond came out grey, not green). The pointer pass leaves graphics stages as they were, so what was found before still is,
    // and the menus' text stays. Not where the stage has the render area's push block: a stage has one push block.
    // RX_VK_FOLLOW_GRAPHICS=0: off)
    static const bool graphics{!getenv("RX_VK_FOLLOW_GRAPHICS") || atoi(getenv("RX_VK_FOLLOW_GRAPHICS")) != 0};
    const bool graphics_stage{stage == Stage::VertexB || stage == Stage::Fragment};
    const bool follow_pointers{(stage == Stage::Compute || (graphics && graphics_stage && !info.uses_render_area)) && !no_follow};
    Id ptr_u64{};
    if (follow_pointers) {
        wants_guest_pages = true;
        if (profile.unified_descriptor_binding && rescaling_push_constants.value == 0) DefineRescalingInputPushConstant();   // (the table's address is in it)
        AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        AddExtension("SPV_KHR_physical_storage_buffer");
        SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
        ptr_u64 = TypePointer(spv::StorageClass::PhysicalStorageBuffer, U64);
    }
    // (a load through an address as glslang makes one: a pointer to a laid-out block, its member read. A pointer to a bare scalar crashed
    // NVIDIA's shader compiler)
    std::map<u32, std::pair<Id, Id>> psb_types;   // (by the type's id)
    const auto psb_load = [&](Id type, Id address, u32 align) {
        auto it = psb_types.find(type.value);
        if (it == psb_types.end()) {
            const Id block{TypeStruct(type)};
            Decorate(block, spv::Decoration::Block);
            MemberDecorate(block, 0, spv::Decoration::Offset, 0U);
            it = psb_types.emplace(type.value, std::make_pair(TypePointer(spv::StorageClass::PhysicalStorageBuffer, block),
                                                        TypePointer(spv::StorageClass::PhysicalStorageBuffer, type))).first;
        }
        const Id member{OpAccessChain(it->second.second, OpConvertUToPtr(it->second.first, address), Const(0U))};
        return OpLoad(type, member, spv::MemoryAccessMask::Aligned, align);
    };
    const auto psb_member = [&](Id type, Id address) {   // (a pointer to a value at an address, for stores and atomics)
        auto it = psb_types.find(type.value);
        if (it == psb_types.end()) {
            const Id block{TypeStruct(type)};
            Decorate(block, spv::Decoration::Block);
            MemberDecorate(block, 0, spv::Decoration::Offset, 0U);
            it = psb_types.emplace(type.value, std::make_pair(TypePointer(spv::StorageClass::PhysicalStorageBuffer, block),
                                                        TypePointer(spv::StorageClass::PhysicalStorageBuffer, type))).first;
        }
        return OpAccessChain(it->second.second, OpConvertUToPtr(it->second.first, address), Const(0U));
    };
    // (the page table's lookup of addr: hit(target) is called in the block where the page is mapped, its device address in target,
    // and must end that block (a return). Falls through after it)
    // (RX_VK_FOLLOW_STORES=1: writes and atomics through the table too; off by default, see the storage buffer pass)
    static const bool follow_stores{env_on("RX_VK_FOLLOW_STORES")};
    // (hookshot) size: the access's bytes. An access that runs past its 4 KB page is left to the miss path: the table maps pages one
    // by one, the next page may be another allocation's or none, and a 16-byte read at a page's end faulted the device (the culling
    // passes 53caac5f, b6122162 at a save's load)
    const auto guest_lookup = [&](Id addr, u32 size, auto&& hit) {
        if (!follow_pointers || gp_level < 2) return;
        const Id table{wants_guest_pages && rescaling_push_constants.value != 0
                           ? OpLoad(U64, OpAccessChain(TypePointer(spv::StorageClass::PushConstant, U64), rescaling_push_constants,
                                                       Const(guest_pages_member_index)))
                           : Constant(U64, u64{0})};   // (pushed: the table's device address, 0 for none)
        const Id offset_a{OpISub(U64, addr, Constant(U64, u64{kGuestPagesBase}))};
        const Id offset_b{OpISub(U64, addr, Constant(U64, u64{kGuestPagesBase2}))};
        const Id in_a{OpULessThan(U1, offset_a, Constant(U64, u64{kGuestPagesBytes}))};
        const Id in_b{OpULessThan(U1, offset_b, Constant(U64, u64{kGuestPagesBytes2}))};
        const Id page_index{OpSelect(U64, in_a, OpShiftRightLogical(U64, offset_a, Const(12U)),
                                     OpIAdd(U64, OpShiftRightLogical(U64, offset_b, Const(12U)), Constant(U64, u64{kGuestPagesBytes >> 12})))};
        const Id fits{OpULessThanEqual(U1, OpBitwiseAnd(U64, addr, Constant(U64, u64{0xfff})), Constant(U64, u64{0x1000 - size}))};
        const Id inside{OpLogicalAnd(U1, OpLogicalAnd(U1, OpLogicalOr(U1, in_a, in_b), OpINotEqual(U1, table, Constant(U64, u64{0}))), fits)};
        const Id then_label{OpLabel()};
        const Id else_label{OpLabel()};
        OpSelectionMerge(else_label, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(inside, then_label, else_label);
        AddLabel(then_label);
        const Id slot{OpIAdd(U64, table, OpShiftLeftLogical(U64, page_index, Const(3U)))};
        const Id page{psb_load(U64, slot, 8U)};
        const Id mapped{OpINotEqual(U1, page, Constant(U64, u64{0}))};   // (before the merge: it must stand right before its branch)
        const Id hit_label{OpLabel()};
        const Id miss_label{OpLabel()};
        OpSelectionMerge(miss_label, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(mapped, hit_label, miss_label);
        AddLabel(hit_label);
        hit(OpIAdd(U64, page, OpBitwiseAnd(U64, addr, Constant(U64, u64{0xfff}))));
        AddLabel(miss_label);
        OpBranch(else_label);
        AddLabel(else_label);
    };
    using DefPtr = Id StorageDefinitions::*;
    const Id zero{u32_zero_value};
    // (EvilOOT) the structure pointer type of a storage buffer of that member's elements, for OpArrayLength
    const auto struct_pointer_of{[&](DefPtr m) -> Id {
        if (m == &StorageDefinitions::U32) return storage_types.U32.array;
        if (m == &StorageDefinitions::F32) return storage_types.F32.array;   // (hookshot: the native float add's, as U32's)
        if (m == &StorageDefinitions::U32x2) return storage_types.U32x2.array;
        if (m == &StorageDefinitions::U32x4) return storage_types.U32x4.array;
        return Id{};
    }};
    // (EvilOOT, RX_VK_MISS_LOG) the miss log's descriptor (translate_program.cpp: c0+0xff0), -1 none. It is a single storage
    // buffer like the real ones, so the searches skip it; an address none of them holds is recorded in it: word 0 counts
    // the misses, then the first 63 addresses (low, high)
    size_t miss_index{SIZE_MAX};
    for (size_t i = 0; i < info.storage_buffers_descriptors.size(); ++i) {
        const auto& d{info.storage_buffers_descriptors[i]};
        if (d.count == 1 && d.cbuf_index == 0 && d.cbuf_offset == 0xff0U) {
            miss_index = i;
        }
    }
    if (miss_index != SIZE_MAX && Sirit::ValidId(ssbos[miss_index].U32)) {
        probe_log = ssbos[miss_index].U32;   // (RX_VK_BLOCK_PROBE, emit_spirv.cpp Traverse)
    }
    const auto record_miss{[&](Id addr) {
        if (miss_index == SIZE_MAX || !Sirit::ValidId(ssbos[miss_index].U32)) {
            return;
        }
        const Id log{ssbos[miss_index].U32};
        const Id counter{OpAccessChain(storage_types.U32.element, log, zero, zero)};
        const Id scope{Const(static_cast<u32>(spv::Scope::Device))};
        const Id slot{OpAtomicIAdd(U32[1], counter, scope, zero, Const(1U))};
        // (the condition before the merge: OpSelectionMerge must come right before its branch; the driver crashed on it)
        const Id room{OpULessThan(U1, slot, Const(63U))};
        const Id then_label{OpLabel()};
        const Id end_label{OpLabel()};
        OpSelectionMerge(end_label, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(room, then_label, end_label);
        AddLabel(then_label);
        const Id at{OpIAdd(U32[1], OpShiftLeftLogical(U32[1], slot, Const(1U)), Const(1U))};
        OpStore(OpAccessChain(storage_types.U32.element, log, zero, at), OpUConvert(U32[1], addr));
        OpStore(OpAccessChain(storage_types.U32.element, log, zero, OpIAdd(U32[1], at, Const(1U))),
                OpUConvert(U32[1], OpShiftRightLogical(U64, addr, Const(32U))));
        OpBranch(end_label);
        AddLabel(end_label);
    }};
    const auto define_body{[&](DefPtr ssbo_member, Id addr, Id element_pointer, u32 shift,
                               auto&& callback, bool add_label = true) {
        if (add_label) AddLabel();
        // (hookshot, global_table_loop: the tables' entries in a loop, its counter here, where the function's variables must be.
        // The CAS loops' callers open the block themselves: theirs stay written out)
        const Id table_counter{global_table_loop && add_label && !ssbo_tables.empty()
                                   ? AddLocalVariable(TypePointer(spv::StorageClass::Function, U32[1]), spv::StorageClass::Function)
                                   : Id{}};
        if (Sirit::ValidId(table_counter)) AddCapability(spv::Capability::StorageBufferArrayDynamicIndexing);   // (the tables by k)
        const size_t num_buffers{info.storage_buffers_descriptors.size()};
        // (from Fritchii) Info::native_global_bounds: the candidate pointers the storage buffer pass bound for accesses it
        // left global (any single descriptor not in the driver bank) carry no NVN size word: the address goes to the
        // candidate whose base is the nearest at or below it, within what is bound (the binding's length, vk_draw.cpp
        // binds them bare). An address none of them holds goes on to the searches below.
        std::vector<bool> candidate(num_buffers);
        if (info.native_global_bounds && Sirit::ValidId(struct_pointer_of(ssbo_member))) {
            const u64 align_mask{~(profile.min_ssbo_alignment - 1U)};
            std::vector<Id> bases(num_buffers);
            Id best_index{Const(~0U)};
            Id best_distance{Constant(U64, ~u64{0})};
            for (size_t index = 0; index < num_buffers; ++index) {
                const auto& ssbo{info.storage_buffers_descriptors[index]};
                if (index == miss_index || ssbo.count != 1 || ssbo.cbuf_index == 0 || ssbo.cbuf_offset % 8 != 0 ||
                    !Sirit::ValidId(ssbos[index].*ssbo_member) || !Sirit::ValidId(cbufs[ssbo.cbuf_index].U32x2)) {
                    continue;
                }
                candidate[index] = true;
                const Id words_pointer{OpAccessChain(uniform_types.U32x2, cbufs[ssbo.cbuf_index].U32x2, zero,
                                                     Const(ssbo.cbuf_offset / 8))};
                bases[index] = OpBitwiseAnd(U64, OpBitcast(U64, OpLoad(U32[2], words_pointer)), Constant(U64, align_mask));
                const Id elements{OpArrayLength(U32[1], ssbos[index].*ssbo_member, 0U)};
                const Id bound_end{
                    OpIAdd(U64, bases[index], OpUConvert(U64, OpShiftLeftLogical(U32[1], elements, Const(shift))))};
                const Id distance{OpISub(U64, addr, bases[index])};
                const Id inside{OpLogicalAnd(U1, OpUGreaterThanEqual(U1, addr, bases[index]), OpULessThan(U1, addr, bound_end))};
                const Id better{OpLogicalAnd(U1, inside, OpULessThan(U1, distance, best_distance))};
                best_distance = OpSelect(U64, better, distance, best_distance);
                best_index = OpSelect(U32[1], better, Const(static_cast<u32>(index)), best_index);
            }
            for (size_t index = 0; index < num_buffers; ++index) {
                if (!candidate[index]) {
                    continue;
                }
                const Id cond{OpIEqual(U1, best_index, Const(static_cast<u32>(index)))};
                const Id then_label{OpLabel()};
                const Id else_label{OpLabel()};
                OpSelectionMerge(else_label, spv::SelectionControlMask::MaskNone);
                OpBranchConditional(cond, then_label, else_label);
                AddLabel(then_label);
                const Id ssbo_offset{OpUConvert(U32[1], OpISub(U64, addr, bases[index]))};
                const Id ssbo_index{OpShiftRightLogical(U32[1], ssbo_offset, Const(shift))};
                callback(OpAccessChain(element_pointer, ssbos[index].*ssbo_member, zero, ssbo_index));
                AddLabel(else_label);
            }
        }
        for (size_t index = 0; index < num_buffers; ++index) {
            if (index == miss_index || candidate[index]) {
                continue;
            }
            // (EvilOOT) every single storage buffer the program has is searched by its address range. The old test,
            // nvn_buffer_used[index], took a descriptor's place in the list for an NVN slot number: with the tracked
            // buffers first, the slots' own descriptors past them were skipped and their reads gave 0. A match is exact,
            // so searching a buffer the access never falls in costs a comparison and nothing else.
            if (info.storage_buffers_descriptors[index].count > 1) {
                continue;
            }
            const auto& ssbo{info.storage_buffers_descriptors[index]};
            const Id ssbo_addr_cbuf_offset{Const(ssbo.cbuf_offset / 8)};
            const Id ssbo_size_cbuf_offset{Const(ssbo.cbuf_offset / 4 + 2)};
            const Id ssbo_addr_pointer{OpAccessChain(
                uniform_types.U32x2, cbufs[ssbo.cbuf_index].U32x2, zero, ssbo_addr_cbuf_offset)};
            const Id ssbo_size_pointer{OpAccessChain(uniform_types.U32, cbufs[ssbo.cbuf_index].U32,
                                                     zero, ssbo_size_cbuf_offset)};

            const u64 ssbo_align_mask{~(profile.min_ssbo_alignment - 1U)};
            const Id unaligned_addr{OpBitcast(U64, OpLoad(U32[2], ssbo_addr_pointer))};
            const Id ssbo_addr{OpBitwiseAnd(U64, unaligned_addr, Constant(U64, ssbo_align_mask))};
            const Id ssbo_size{OpUConvert(U64, OpLoad(U32[1], ssbo_size_pointer))};
            Id ssbo_end{OpIAdd(U64, unaligned_addr, ssbo_size)};
            // (EvilOOT) no further than what is bound: a buffer the back end could not bind is a null descriptor (length 0), and an
            // address in its range read 0 through it instead of going on to the page table (OoT's ambient light probes: 0, the
            // foliage and signs black)
            if (Sirit::ValidId(struct_pointer_of(ssbo_member)) && Sirit::ValidId(ssbos[index].*ssbo_member)) {
                const Id elements{OpArrayLength(U32[1], ssbos[index].*ssbo_member, 0U)};
                const Id bound_end{OpIAdd(U64, ssbo_addr, OpUConvert(U64, OpShiftLeftLogical(U32[1], elements, Const(shift))))};
                ssbo_end = OpSelect(U64, OpULessThan(U1, bound_end, ssbo_end), bound_end, ssbo_end);
            }
            const Id cond{OpLogicalAnd(U1, OpUGreaterThanEqual(U1, addr, ssbo_addr),
                                       OpULessThan(U1, addr, ssbo_end))};
            const Id then_label{OpLabel()};
            const Id else_label{OpLabel()};
            OpSelectionMerge(else_label, spv::SelectionControlMask::MaskNone);
            OpBranchConditional(cond, then_label, else_label);
            AddLabel(then_label);
            const Id ssbo_id{ssbos[index].*ssbo_member};
            const Id ssbo_offset{OpUConvert(U32[1], OpISub(U64, addr, ssbo_addr))};
            const Id ssbo_index{OpShiftRightLogical(U32[1], ssbo_offset, Const(shift))};
            const Id ssbo_pointer{OpAccessChain(element_pointer, ssbo_id, zero, ssbo_index)};
            callback(ssbo_pointer);
            AddLabel(else_label);
        }
        for (const GlobalTableVars& table : ssbo_tables) {   // (hookshot: Info::global_tables)
            const Id table_id{table.vars.*ssbo_member};
            if (!Sirit::ValidId(table_id) || !Sirit::ValidId(cbufs[table.cbuf_index].U32x2)) {
                continue;
            }
            if (Sirit::ValidId(table_counter)) {
                // (mma_layout.h global_table_loop) the same test as the written-out one below, entry k of the loop's counter, the
                // driver asked not to unroll it. k is the same in every invocation that runs it: dynamic indexing, not non-uniform.
                // The written-out form below is left as it was: other GPUs' SPIR-V, and their drivers' caches of it, do not change
                const Id header{OpLabel()};
                const Id check{OpLabel()};
                const Id body{OpLabel()};
                const Id next{OpLabel()};
                const Id done{OpLabel()};
                OpStore(table_counter, zero);
                OpBranch(header);
                AddLabel(header);
                OpLoopMerge(done, next, spv::LoopControlMask::DontUnroll);
                OpBranch(check);
                AddLabel(check);
                const Id k{OpLoad(U32[1], table_counter)};
                OpBranchConditional(OpULessThan(U1, k, Const(table.count)), body, done);
                AddLabel(body);
                // (entry k at cbuf_offset + 16k: its address in the U32x2 word cbuf_offset / 8 + 2k, its size in the next)
                const Id word{OpIAdd(U32[1], Const(table.cbuf_offset / 8), OpShiftLeftLogical(U32[1], k, Const(1U)))};
                const Id entry_pointer{OpAccessChain(uniform_types.U32x2, cbufs[table.cbuf_index].U32x2, zero, word)};
                const Id size_pointer{OpAccessChain(uniform_types.U32x2, cbufs[table.cbuf_index].U32x2, zero,
                                                    OpIAdd(U32[1], word, Const(1U)))};
                const u64 ssbo_align_mask{~(profile.min_ssbo_alignment - 1U)};
                const Id unaligned_addr{OpBitcast(U64, OpLoad(U32[2], entry_pointer))};
                const Id ssbo_addr{OpBitwiseAnd(U64, unaligned_addr, Constant(U64, ssbo_align_mask))};
                const Id size_word{OpCompositeExtract(U32[1], OpLoad(U32[2], size_pointer), 0U)};
                Id ssbo_end{OpIAdd(U64, unaligned_addr, OpUConvert(U64, size_word))};
                if (const Id struct_pointer{struct_pointer_of(ssbo_member)}; Sirit::ValidId(struct_pointer)) {
                    const Id elements{OpArrayLength(U32[1], OpAccessChain(struct_pointer, table_id, k), 0U)};
                    const Id bound_end{
                        OpIAdd(U64, ssbo_addr, OpUConvert(U64, OpShiftLeftLogical(U32[1], elements, Const(shift))))};
                    const Id capped{OpSelect(U64, OpULessThan(U1, bound_end, ssbo_end), bound_end, ssbo_end)};
                    ssbo_end = OpSelect(U64, OpIEqual(U1, size_word, zero), bound_end, capped);
                }
                const Id cond{OpLogicalAnd(U1, OpUGreaterThanEqual(U1, addr, ssbo_addr), OpULessThan(U1, addr, ssbo_end))};
                const Id then_label{OpLabel()};
                const Id else_label{OpLabel()};
                OpSelectionMerge(else_label, spv::SelectionControlMask::MaskNone);
                OpBranchConditional(cond, then_label, else_label);
                AddLabel(then_label);
                const Id ssbo_offset{OpUConvert(U32[1], OpISub(U64, addr, ssbo_addr))};
                const Id ssbo_index{OpShiftRightLogical(U32[1], ssbo_offset, Const(shift))};
                callback(OpAccessChain(element_pointer, table_id, k, zero, ssbo_index));   // (ends in a return)
                AddLabel(else_label);
                OpBranch(next);
                AddLabel(next);
                OpStore(table_counter, OpIAdd(U32[1], k, Const(1U)));
                OpBranch(header);
                AddLabel(done);
                continue;
            }
            for (u32 k = 0; k < table.count; ++k) {
                const u32 at{table.cbuf_offset + k * 16};
                const Id entry_pointer{OpAccessChain(uniform_types.U32x2, cbufs[table.cbuf_index].U32x2, zero,
                                                     Const(at / 8))};
                const Id size_pointer{OpAccessChain(uniform_types.U32x2, cbufs[table.cbuf_index].U32x2, zero,
                                                    Const(at / 8 + 1))};
                const u64 ssbo_align_mask{~(profile.min_ssbo_alignment - 1U)};
                const Id unaligned_addr{OpBitcast(U64, OpLoad(U32[2], entry_pointer))};
                const Id ssbo_addr{OpBitwiseAnd(U64, unaligned_addr, Constant(U64, ssbo_align_mask))};
                const Id size_word{OpCompositeExtract(U32[1], OpLoad(U32[2], size_pointer), 0U)};
                Id ssbo_end{OpIAdd(U64, unaligned_addr, OpUConvert(U64, size_word))};
                // (EvilOOT) an entry without a size (OoT's culling setup keeps bare pointers, the size word 0: the back end
                // binds those up to 1 MB of their pool, vk_draw.cpp): the binding's own length is the end
                if (const Id struct_pointer{struct_pointer_of(ssbo_member)}; Sirit::ValidId(struct_pointer)) {
                    const Id elements{OpArrayLength(U32[1], OpAccessChain(struct_pointer, table_id, Const(k)), 0U)};
                    const Id bound_end{
                        OpIAdd(U64, ssbo_addr, OpUConvert(U64, OpShiftLeftLogical(U32[1], elements, Const(shift))))};
                    // (EvilOOT: and an entry with a size no further than what is bound either: an entry the back end could
                    // not bind (a whole pool past its imports) is a null descriptor, and its range read 0 instead of the page table)
                    const Id capped{OpSelect(U64, OpULessThan(U1, bound_end, ssbo_end), bound_end, ssbo_end)};
                    ssbo_end = OpSelect(U64, OpIEqual(U1, size_word, zero), bound_end, capped);
                }
                const Id cond{OpLogicalAnd(U1, OpUGreaterThanEqual(U1, addr, ssbo_addr), OpULessThan(U1, addr, ssbo_end))};
                const Id then_label{OpLabel()};
                const Id else_label{OpLabel()};
                OpSelectionMerge(else_label, spv::SelectionControlMask::MaskNone);
                OpBranchConditional(cond, then_label, else_label);
                AddLabel(then_label);
                const Id ssbo_offset{OpUConvert(U32[1], OpISub(U64, addr, ssbo_addr))};
                const Id ssbo_index{OpShiftRightLogical(U32[1], ssbo_offset, Const(shift))};
                callback(OpAccessChain(element_pointer, table_id, Const(k), zero, ssbo_index));
                AddLabel(else_label);
            }
        }
        record_miss(addr);   // (EvilOOT, RX_VK_MISS_LOG: here no buffer held the address)
    }};
    // (hookshot) the helpers below are DontInline: inlined at each of a culling program's hundreds of global accesses, with EvilOOT's
    // fuller buffer search, NVIDIA's compiler took 41 s on 53caac5f and the driver reset. Lichii PR 408 found the same (161 s for 68 programs)
    const auto define_load{[&](DefPtr ssbo_member, Id element_pointer, Id type, u32 shift) {
        const Id function_type{TypeFunction(type, U64)};
        // (#994, the floating trees) the task and mesh stages' load helpers are inlined (EDEN_NX2_GM_INLINE=0: not; an EDEN_
        // switch, so in the program cache's stamp). Not inlined, NVIDIA's compiler (RTX 3070, Windows) gave OoT's tree task
        // 3cb76827 a wrong address for its third load: n = load(file + 4), code = load(cells[e]), then
        // load(file + (n + 9 + e) * 4) went to file + (code + 9 + e) * 4, the second call's result in place of the first's,
        // though the SPIR-V names the first. That load, the cell's end in the placement file's prefix sums, read 0 outside
        // every buffer, and min(end - start, 256) made every visible cell 256 trees, the next cells' trees drawn in its place:
        // the wall of trees in the sky. The culling's compute programs, with hundreds of global loads, keep the helper
        // (NVIDIA took 41 s on one inlined, above)
        static const bool gm_inline{!getenv("EDEN_NX2_GM_INLINE") || getenv("EDEN_NX2_GM_INLINE")[0] != '0'};
        const bool inline_here{gm_inline && (stage == Stage::Task || stage == Stage::Mesh ||
                                            (shift == 4 && runtime_info.inline_global_128_loads))};
        const Id func_id{OpFunction(type, inline_here ? spv::FunctionControlMask::Inline : spv::FunctionControlMask::DontInline,
                                    function_type)};
        const Id addr{OpFunctionParameter(U64)};
        define_body(ssbo_member, addr, element_pointer, shift,
                    [&](Id ssbo_pointer) { OpReturnValue(OpLoad(type, ssbo_pointer)); });
        guest_lookup(addr, 1U << shift, [&](Id target) {
            if (gp_level >= 3) OpReturnValue(psb_load(type, target, 4U));
            else OpReturnValue(ConstantNull(type));
        });
        OpReturnValue(ConstantNull(type));
        OpFunctionEnd();
        return func_id;
    }};
    const auto define_write{[&](DefPtr ssbo_member, Id element_pointer, Id type, u32 shift) {
        const Id function_type{TypeFunction(void_id, U64, type)};
        const Id func_id{OpFunction(void_id, spv::FunctionControlMask::DontInline, function_type)};
        const Id addr{OpFunctionParameter(U64)};
        const Id data{OpFunctionParameter(type)};
        define_body(ssbo_member, addr, element_pointer, shift, [&](Id ssbo_pointer) {
            OpStore(ssbo_pointer, data);
            OpReturn();
        });
        if (follow_stores) guest_lookup(addr, 1U << shift, [&](Id target) {   // (hookshot: through the page table, as the loads: the culling's outputs)
            if (gp_level >= 3) OpStore(psb_member(type, target), data, spv::MemoryAccessMask::Aligned, 4U);
            OpReturn();
        });
        OpReturn();
        OpFunctionEnd();
        return func_id;
    }};
    const auto define{
        [&](DefPtr ssbo_member, const StorageTypeDefinition& type_def, Id type, size_t size) {
            const Id element_type{type_def.element};
            const u32 shift{static_cast<u32>(std::countr_zero(size))};
            const Id load_func{define_load(ssbo_member, element_type, type, shift)};
            const Id write_func{define_write(ssbo_member, element_type, type, shift)};
            return std::make_pair(load_func, write_func);
        }};
    std::tie(load_global_func_u32, write_global_func_u32) =
        define(&StorageDefinitions::U32, storage_types.U32, U32[1], sizeof(u32));
    std::tie(load_global_func_u32x2, write_global_func_u32x2) =
        define(&StorageDefinitions::U32x2, storage_types.U32x2, U32[2], sizeof(u32[2]));
    std::tie(load_global_func_u32x4, write_global_func_u32x4) =
        define(&StorageDefinitions::U32x4, storage_types.U32x4, U32[4], sizeof(u32[4]));
    // (hookshot) the 32-bit atomics' fallbacks, as the loads': the old value, 0 where no buffer holds the address
    using Atomic = Id (Sirit::Module::*)(Id, Id, Id, Id, Id);
    static constexpr std::array<Atomic, 9> atomics{
        &Sirit::Module::OpAtomicIAdd, &Sirit::Module::OpAtomicSMin, &Sirit::Module::OpAtomicUMin,
        &Sirit::Module::OpAtomicSMax, &Sirit::Module::OpAtomicUMax, &Sirit::Module::OpAtomicAnd,
        &Sirit::Module::OpAtomicOr,   &Sirit::Module::OpAtomicXor,  &Sirit::Module::OpAtomicExchange};
    if (info.stores_global_memory) {
        for (size_t i = 0; i < atomics.size(); ++i) {
            const Id function_type{TypeFunction(U32[1], U64, U32[1])};
            const Id func_id{OpFunction(U32[1], spv::FunctionControlMask::DontInline, function_type)};
            const Id addr{OpFunctionParameter(U64)};
            const Id value{OpFunctionParameter(U32[1])};
            define_body(&StorageDefinitions::U32, addr, storage_types.U32.element, 2, [&](Id ssbo_pointer) {
                const Id scope{Const(static_cast<u32>(spv::Scope::Device))};
                OpReturnValue((this->*atomics[i])(U32[1], ssbo_pointer, scope, u32_zero_value, value));
            });
            if (follow_stores) guest_lookup(addr, 4U, [&](Id target) {   // (hookshot: the culling's counters, through the page table)
                const Id scope{Const(static_cast<u32>(spv::Scope::Device))};
                if (gp_level >= 3) OpReturnValue((this->*atomics[i])(U32[1], psb_member(U32[1], target), scope, u32_zero_value, value));
                else OpReturnValue(u32_zero_value);
            });
            OpReturnValue(u32_zero_value);
            OpFunctionEnd();
            atomic_global_func_u32[i] = func_id;
        }
        // (hookshot) ATOMG.CAS's: the comparator, then the value stored where the word equals it
        const Id function_type{TypeFunction(U32[1], U64, U32[1], U32[1])};
        const Id func_id{OpFunction(U32[1], spv::FunctionControlMask::DontInline, function_type)};
        const Id addr{OpFunctionParameter(U64)};
        const Id comparator{OpFunctionParameter(U32[1])};
        const Id value{OpFunctionParameter(U32[1])};
        const auto cas{[&](Id pointer) {
            const Id scope{Const(static_cast<u32>(spv::Scope::Device))};
            return OpAtomicCompareExchange(U32[1], pointer, scope, u32_zero_value, u32_zero_value, value, comparator);
        }};
        define_body(&StorageDefinitions::U32, addr, storage_types.U32.element, 2,
                    [&](Id ssbo_pointer) { OpReturnValue(cas(ssbo_pointer)); });
        if (follow_stores) guest_lookup(addr, 4U, [&](Id target) {
            if (gp_level >= 3) OpReturnValue(cas(psb_member(U32[1], target)));
            else OpReturnValue(u32_zero_value);
        });
        OpReturnValue(u32_zero_value);
        OpFunctionEnd();
        atomic_global_cas_func_u32 = func_id;
    }
    // (hookshot) 8- and 16-bit global stores: the aligned 32-bit word is changed in place with a compare-exchange loop, as
    // the storage buffers' narrow stores are (emit_spirv_memory.cpp). A plain load/store pair loses the other invocations'
    // halves of the word; the console's byte-enabled store does not.
    if (info.stores_global_memory) {
        const Id func_type{TypeFunction(void_id, U64, U32[1], U32[1], U32[1])};
        const Id func{OpFunction(void_id, spv::FunctionControlMask::MaskNone, func_type)};
        const Id addr{OpFunctionParameter(U64)};
        const Id value{OpFunctionParameter(U32[1])};
        const Id bit_offset{OpFunctionParameter(U32[1])};
        const Id bit_count{OpFunctionParameter(U32[1])};
        AddLabel();
        // (one variable in the entry block: OpVariable with Function storage must lead the function)
        const Id expected_var{AddLocalVariable(TypePointer(spv::StorageClass::Function, U32[1]),
                                               spv::StorageClass::Function)};
        const Id tries_var{AddLocalVariable(TypePointer(spv::StorageClass::Function, U32[1]),
                                            spv::StorageClass::Function)};
        const auto cas_loop{[&](Id pointer) {
            OpStore(expected_var, OpLoad(U32[1], pointer, spv::MemoryAccessMask::Aligned, 4U));
            OpStore(tries_var, u32_zero_value);
            const Id scope_device{Const(static_cast<u32>(spv::Scope::Device))};
            const Id ordering_relaxed{u32_zero_value};
            const Id begin_label{OpLabel()};
            const Id body_label{OpLabel()};
            const Id continue_label{OpLabel()};
            const Id end_label{OpLabel()};
            OpBranch(begin_label);
            AddLabel(begin_label);
            OpLoopMerge(end_label, continue_label, spv::LoopControlMask::MaskNone);
            OpBranch(body_label);
            AddLabel(body_label);
            const Id expected{OpLoad(U32[1], expected_var)};
            const Id desired{OpBitFieldInsert(U32[1], expected, value, bit_offset, bit_count)};
            const Id actual{OpAtomicCompareExchange(U32[1], pointer, scope_device, ordering_relaxed,
                                                    ordering_relaxed, desired, expected)};
            OpStore(expected_var, actual);
            const Id success{OpIEqual(U1, expected, actual)};
            OpBranchConditional(CasLoopDone(*this, tries_var, success), end_label, continue_label);
            AddLabel(end_label);
            OpReturn();
            AddLabel(continue_label);
            OpBranch(begin_label);
        }};
        define_body(&StorageDefinitions::U32, addr, storage_types.U32.element, 2,
                    [&](Id ssbo_pointer) { cas_loop(ssbo_pointer); }, false);
        guest_lookup(addr, 4U, [&](Id target) {
            if (gp_level >= 3) {
                cas_loop(psb_member(U32[1], target));
            } else {
                OpReturn();
            }
        });
        OpReturn();
        OpFunctionEnd();
        write_global_cas_loop_func = func;
    }
    // (hookshot) RED/ATOM.E.ADD.F32 at an address no buffer was tracked to: the 32-bit atomics' fallback, with the float
    // sum made in a compare-exchange loop as the storage buffers' f32_add_cas does, or the GPU's own float add where
    // the storage buffers' adds are (native_f32_add: the f32 views, searched as the u32 ones)
    if (info.stores_global_memory) {
        const Id func_type{TypeFunction(F32[1], U64, F32[1])};
        const Id func{OpFunction(F32[1], spv::FunctionControlMask::DontInline, func_type)};
        const Id addr{OpFunctionParameter(U64)};
        const Id value{OpFunctionParameter(F32[1])};
        AddLabel();
        const Id expected_var{native_f32_add ? Id{} : AddLocalVariable(TypePointer(spv::StorageClass::Function, U32[1]),
                                                                       spv::StorageClass::Function)};
        const Id tries_var{native_f32_add ? Id{} : AddLocalVariable(TypePointer(spv::StorageClass::Function, U32[1]),
                                                                    spv::StorageClass::Function)};
        const auto cas_loop{[&](Id pointer) {
            OpStore(expected_var, OpLoad(U32[1], pointer, spv::MemoryAccessMask::Aligned, 4U));
            OpStore(tries_var, u32_zero_value);   // (a ceiling of attempts, as every compare-exchange loop has: #548)
            const Id scope_device{Const(static_cast<u32>(spv::Scope::Device))};
            const Id ordering_relaxed{u32_zero_value};
            const Id begin_label{OpLabel()};
            const Id body_label{OpLabel()};
            const Id continue_label{OpLabel()};
            const Id end_label{OpLabel()};
            OpBranch(begin_label);
            AddLabel(begin_label);
            OpLoopMerge(end_label, continue_label, spv::LoopControlMask::MaskNone);
            OpBranch(body_label);
            AddLabel(body_label);
            const Id expected{OpLoad(U32[1], expected_var)};
            const Id desired{OpBitcast(U32[1], OpFAdd(F32[1], OpBitcast(F32[1], expected), value))};
            const Id actual{OpAtomicCompareExchange(U32[1], pointer, scope_device, ordering_relaxed,
                                                    ordering_relaxed, desired, expected)};
            OpStore(expected_var, actual);
            const Id success{OpIEqual(U1, expected, actual)};
            OpBranchConditional(CasLoopDone(*this, tries_var, success), end_label, continue_label);
            AddLabel(end_label);
            OpReturnValue(OpBitcast(F32[1], actual));
            AddLabel(continue_label);
            OpBranch(begin_label);
        }};
        const auto native_add{[&](Id pointer) {
            OpReturnValue(OpAtomicFAddEXT(F32[1], pointer, Const(static_cast<u32>(spv::Scope::Device)), u32_zero_value, value));
        }};
        if (native_f32_add) {
            define_body(&StorageDefinitions::F32, addr, storage_types.F32.element, 2, native_add, false);
        } else {
            define_body(&StorageDefinitions::U32, addr, storage_types.U32.element, 2,
                        [&](Id ssbo_pointer) { cas_loop(ssbo_pointer); }, false);
        }
        if (follow_stores) guest_lookup(addr, 4U, [&](Id target) {
            if (gp_level < 3) OpReturnValue(ConstantNull(F32[1]));
            else if (native_f32_add) native_add(psb_member(F32[1], target));
            else cas_loop(psb_member(U32[1], target));
        });
        OpReturnValue(ConstantNull(F32[1]));
        OpFunctionEnd();
        atomic_global_add_f32_func = func;
    }
}

void EmitContext::DefineRescalingInput(const Info& info) {
    if (!info.uses_rescaling_uniform || rescaling_push_constants.value != 0) {   // (hookshot: or made already, for the page table)
        return;
    }
    if (profile.unified_descriptor_binding) {
        DefineRescalingInputPushConstant();
    } else {
        DefineRescalingInputUniformConstant();
    }
}

void EmitContext::DefineRescalingInputPushConstant() {
    if (Sirit::ValidId(rescaling_push_constants)) {
        return;   // (the page table's call and the fallback's: one block)
    }
    boost::container::static_vector<Id, 12> members{};
    u32 member_index{0};

    rescaling_textures_type = TypeArray(U32[1], Const(4u));
    Decorate(rescaling_textures_type, spv::Decoration::ArrayStride, 4u);
    members.push_back(rescaling_textures_type);
    rescaling_textures_member_index = member_index++;

    rescaling_images_type = TypeArray(U32[1], Const(NUM_IMAGE_SCALING_WORDS));
    Decorate(rescaling_images_type, spv::Decoration::ArrayStride, 4u);
    members.push_back(rescaling_images_type);
    rescaling_images_member_index = member_index++;

    if (stage != Stage::Compute) {
        members.push_back(F32[1]);
        rescaling_downfactor_member_index = member_index++;
    }
    if (wants_guest_pages) {
        members.push_back(U64);
        guest_pages_member_index = member_index++;
    }
    if (wants_mesh_first) {
        members.push_back(mesh_fallback ? U32[1] : U64);
        mesh_first_member_index = member_index++;
    }
    if (wants_mesh_fallback) {
        members.push_back(U64);
        mesh_fallback_payload_member_index = member_index++;
        members.push_back(U64);
        mesh_fallback_records_member_index = member_index++;
        if (stage == Stage::Task) {
            members.push_back(U64);
            mesh_fallback_indirect_input_member_index = member_index++;
        }
    }
    const Id push_constant_struct{TypeStruct(std::span(members.data(), members.size()))};
    Decorate(push_constant_struct, spv::Decoration::Block);
    Name(push_constant_struct, "ResolutionInfo");

    MemberDecorate(push_constant_struct, rescaling_textures_member_index, spv::Decoration::Offset,
                   static_cast<u32>(offsetof(RescalingLayout, rescaling_textures)));
    MemberName(push_constant_struct, rescaling_textures_member_index, "rescaling_textures");

    MemberDecorate(push_constant_struct, rescaling_images_member_index, spv::Decoration::Offset,
                   static_cast<u32>(offsetof(RescalingLayout, rescaling_images)));
    MemberName(push_constant_struct, rescaling_images_member_index, "rescaling_images");

    if (stage != Stage::Compute) {
        MemberDecorate(push_constant_struct, rescaling_downfactor_member_index,
                       spv::Decoration::Offset,
                       static_cast<u32>(offsetof(RescalingLayout, down_factor)));
        MemberName(push_constant_struct, rescaling_downfactor_member_index, "down_factor");
    }
    if (wants_guest_pages) {
        MemberDecorate(push_constant_struct, guest_pages_member_index, spv::Decoration::Offset, GUEST_PAGES_PUSH_OFFSET);
        MemberName(push_constant_struct, guest_pages_member_index, "guest_pages");
    }
    if (wants_mesh_first) {
        MemberDecorate(push_constant_struct, mesh_first_member_index, spv::Decoration::Offset, MESH_FIRST_PUSH_OFFSET);
        MemberName(push_constant_struct, mesh_first_member_index, "mesh_first");
    }
    if (wants_mesh_fallback) {
        MemberDecorate(push_constant_struct, mesh_fallback_payload_member_index, spv::Decoration::Offset,
                       MESH_FALLBACK_PAYLOAD_PUSH_OFFSET);
        MemberDecorate(push_constant_struct, mesh_fallback_records_member_index, spv::Decoration::Offset,
                       MESH_FALLBACK_RECORDS_PUSH_OFFSET);
        MemberName(push_constant_struct, mesh_fallback_payload_member_index, "mesh_fallback_payload");
        MemberName(push_constant_struct, mesh_fallback_records_member_index, "mesh_fallback_records");
        if (stage == Stage::Task) {
            MemberDecorate(push_constant_struct, mesh_fallback_indirect_input_member_index, spv::Decoration::Offset,
                           MESH_FALLBACK_INDIRECT_INPUT_PUSH_OFFSET);
        }
    }
    const Id pointer_type{TypePointer(spv::StorageClass::PushConstant, push_constant_struct)};
    rescaling_push_constants = AddGlobalVariable(pointer_type, spv::StorageClass::PushConstant);
    Name(rescaling_push_constants, "rescaling_push_constants");

    if (profile.supported_spirv >= 0x00010400) {
        interfaces.push_back(rescaling_push_constants);
    }
}

Id EmitContext::MeshFallbackWord(Id address) {
    if (!Sirit::ValidId(mesh_fallback_word_ptr)) {
        const Id block{TypeStruct(U32[1])};
        Decorate(block, spv::Decoration::Block);
        MemberDecorate(block, 0, spv::Decoration::Offset, 0U);
        mesh_fallback_word_ptr = TypePointer(spv::StorageClass::PhysicalStorageBuffer, block);
    }
    const Id pointer_type{TypePointer(spv::StorageClass::PhysicalStorageBuffer, U32[1])};
    return OpAccessChain(pointer_type, OpConvertUToPtr(mesh_fallback_word_ptr, address), Const(0U));
}

void EmitContext::DefineRescalingInputUniformConstant() {
    const Id pointer_type{TypePointer(spv::StorageClass::UniformConstant, F32[4])};
    rescaling_uniform_constant =
        AddGlobalVariable(pointer_type, spv::StorageClass::UniformConstant);
    Decorate(rescaling_uniform_constant, spv::Decoration::Location, 0u);

    if (profile.supported_spirv >= 0x00010400) {
        interfaces.push_back(rescaling_uniform_constant);
    }
}

void EmitContext::DefineRenderArea(const Info& info) {
    if (!info.uses_render_area) {
        return;
    }

    if (profile.unified_descriptor_binding) {
        boost::container::static_vector<Id, 1> members{};
        u32 member_index{0};

        members.push_back(F32[4]);
        render_are_member_index = member_index++;

        const Id push_constant_struct{TypeStruct(std::span(members.data(), members.size()))};
        Decorate(push_constant_struct, spv::Decoration::Block);
        Name(push_constant_struct, "RenderAreaInfo");

        MemberDecorate(push_constant_struct, render_are_member_index, spv::Decoration::Offset, 0);
        MemberName(push_constant_struct, render_are_member_index, "render_area");

        const Id pointer_type{TypePointer(spv::StorageClass::PushConstant, push_constant_struct)};
        render_area_push_constant =
            AddGlobalVariable(pointer_type, spv::StorageClass::PushConstant);
        Name(render_area_push_constant, "render_area_push_constants");

        if (profile.supported_spirv >= 0x00010400) {
            interfaces.push_back(render_area_push_constant);
        }
    }
}

void EmitContext::DefineConstantBuffers(const Info& info, u32& binding) {
    if (info.constant_buffer_descriptors.empty()) {
        return;
    }
    if (!profile.support_descriptor_aliasing) {
        DefineConstBuffers(*this, info, &UniformDefinitions::U32x4, binding, U32[4], 'u',
                           sizeof(u32[4]));
        for (const ConstantBufferDescriptor& desc : info.constant_buffer_descriptors) {
            binding += desc.count;
        }
        return;
    }
    IR::Type types{info.used_constant_buffer_types | info.used_indirect_cbuf_types};
    if (True(types & IR::Type::U8)) {
        if (profile.support_int8 && profile.support_uniform_and_storage_buffer_8bit) {
            DefineConstBuffers(*this, info, &UniformDefinitions::U8, binding, U8, 'u', sizeof(u8));
            DefineConstBuffers(*this, info, &UniformDefinitions::S8, binding, S8, 's', sizeof(s8));
        } else {
            types |= IR::Type::U32;
        }
    }
    if (True(types & IR::Type::U16)) {
        if (profile.support_int16 && profile.support_uniform_and_storage_buffer_16bit) {
            DefineConstBuffers(*this, info, &UniformDefinitions::U16, binding, U16, 'u',
                               sizeof(u16));
            DefineConstBuffers(*this, info, &UniformDefinitions::S16, binding, S16, 's',
                               sizeof(s16));
        } else {
            types |= IR::Type::U32;
        }
    }
    if (True(types & IR::Type::U32)) {
        DefineConstBuffers(*this, info, &UniformDefinitions::U32, binding, U32[1], 'u',
                           sizeof(u32));
    }
    if (True(types & IR::Type::F32)) {
        DefineConstBuffers(*this, info, &UniformDefinitions::F32, binding, F32[1], 'f',
                           sizeof(f32));
    }
    if (True(types & IR::Type::U32x2)) {
        DefineConstBuffers(*this, info, &UniformDefinitions::U32x2, binding, U32[2], 'u',
                           sizeof(u32[2]));
    }
    binding += static_cast<u32>(info.constant_buffer_descriptors.size());
}

void EmitContext::DefineConstantBufferIndirectFunctions(const Info& info) {
    if (!info.uses_cbuf_indirect) {
        return;
    }
    const auto make_accessor{[&](Id buffer_type, Id UniformDefinitions::*member_ptr) {
        const Id func_type{TypeFunction(buffer_type, U32[1], U32[1])};
        const Id func{OpFunction(buffer_type, spv::FunctionControlMask::MaskNone, func_type)};
        const Id binding{OpFunctionParameter(U32[1])};
        const Id offset{OpFunctionParameter(U32[1])};

        AddLabel();

        const Id merge_label{OpLabel()};
        const Id uniform_type{uniform_types.*member_ptr};

        std::array<Id, Info::MAX_INDIRECT_CBUFS> buf_labels;
        std::array<Sirit::Literal, Info::MAX_INDIRECT_CBUFS> buf_literals;
        for (u32 i = 0; i < Info::MAX_INDIRECT_CBUFS; i++) {
            buf_labels[i] = OpLabel();
            buf_literals[i] = Sirit::Literal{i};
        }
        OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
        OpSwitch(binding, buf_labels[0], buf_literals, buf_labels);
        for (u32 i = 0; i < Info::MAX_INDIRECT_CBUFS; i++) {
            AddLabel(buf_labels[i]);
            const Id cbuf{cbufs[i].*member_ptr};
            const Id access_chain{OpAccessChain(uniform_type, cbuf, u32_zero_value, offset)};
            const Id result{OpLoad(buffer_type, access_chain)};
            OpReturnValue(result);
        }
        AddLabel(merge_label);
        OpUnreachable();
        OpFunctionEnd();
        return func;
    }};
    IR::Type types{info.used_indirect_cbuf_types};
    bool supports_aliasing = profile.support_descriptor_aliasing;
    if (supports_aliasing && True(types & IR::Type::U8)) {
        if (profile.support_int8 && profile.support_uniform_and_storage_buffer_8bit) {
            load_const_func_u8 = make_accessor(U8, &UniformDefinitions::U8);
        } else {
            types |= IR::Type::U32;
        }
    }
    if (supports_aliasing && True(types & IR::Type::U16)) {
        if (profile.support_int16 && profile.support_uniform_and_storage_buffer_16bit) {
            load_const_func_u16 = make_accessor(U16, &UniformDefinitions::U16);
        } else {
            types |= IR::Type::U32;
        }
    }
    if (supports_aliasing && True(types & IR::Type::F32)) {
        load_const_func_f32 = make_accessor(F32[1], &UniformDefinitions::F32);
    }
    if (supports_aliasing && True(types & IR::Type::U32)) {
        load_const_func_u32 = make_accessor(U32[1], &UniformDefinitions::U32);
    }
    if (supports_aliasing && True(types & IR::Type::U32x2)) {
        load_const_func_u32x2 = make_accessor(U32[2], &UniformDefinitions::U32x2);
    }
    if (!supports_aliasing || True(types & IR::Type::U32x4)) {
        load_const_func_u32x4 = make_accessor(U32[4], &UniformDefinitions::U32x4);
    }
}

void EmitContext::DefineStorageBuffers(const Info& info, u32& binding) {
    // (hookshot) RED/ATOM.ADD.F32 as the GPU's own float add where it has one, on an f32 view of the buffer: the
    // compare-exchange loop gives up after CAS_LOOP_MAX_TRIES attempts, and an add that gives up is lost. Decided before
    // the early return: DefineGlobalMemoryFunctions' fallback reads it too
    native_f32_add = profile.support_float32_atomic_add && profile.support_descriptor_aliasing && info.uses_atomic_f32_add;
    if (native_f32_add) {
        AddExtension("SPV_EXT_shader_atomic_float_add");
        AddCapability(spv::Capability::AtomicFloat32AddEXT);
    }
    if (info.storage_buffers_descriptors.empty()) {
        return;
    }
    AddExtension("SPV_KHR_storage_buffer_storage_class");

    IR::Type used_types{profile.support_descriptor_aliasing ? info.used_storage_buffer_types
                                                            : IR::Type::U32};
    used_types |= IR::Type::U32;
    if (native_f32_add) {
        used_types |= IR::Type::F32;
    }
    if (profile.support_int8 && profile.support_storage_buffer_8bit &&
        True(used_types & IR::Type::U8)) {
        DefineSsbos(*this, storage_types.U8, &StorageDefinitions::U8, info, binding, U8,
                    sizeof(u8));
        DefineSsbos(*this, storage_types.S8, &StorageDefinitions::S8, info, binding, S8,
                    sizeof(u8));
    }
    if (profile.support_int16 && profile.support_storage_buffer_16bit &&
        True(used_types & IR::Type::U16)) {
        DefineSsbos(*this, storage_types.U16, &StorageDefinitions::U16, info, binding, U16,
                    sizeof(u16));
        DefineSsbos(*this, storage_types.S16, &StorageDefinitions::S16, info, binding, S16,
                    sizeof(u16));
    }
    if (True(used_types & IR::Type::U32)) {
        DefineSsbos(*this, storage_types.U32, &StorageDefinitions::U32, info, binding, U32[1],
                    sizeof(u32));
    }
    if (True(used_types & IR::Type::F32)) {
        DefineSsbos(*this, storage_types.F32, &StorageDefinitions::F32, info, binding, F32[1],
                    sizeof(f32));
    }
    if (True(used_types & IR::Type::U64)) {
        DefineSsbos(*this, storage_types.U64, &StorageDefinitions::U64, info, binding, U64,
                    sizeof(u64));
    }
    if (True(used_types & IR::Type::U32x2)) {
        DefineSsbos(*this, storage_types.U32x2, &StorageDefinitions::U32x2, info, binding, U32[2],
                    sizeof(u32[2]));
    }
    if (True(used_types & IR::Type::U32x4)) {
        DefineSsbos(*this, storage_types.U32x4, &StorageDefinitions::U32x4, info, binding, U32[4],
                    sizeof(u32[4]));
    }
    for (const StorageBufferDescriptor& desc : info.storage_buffers_descriptors) {
        binding += desc.count > 1 ? 1 : desc.count;   // (a table: one binding, as the back end lays it out)
    }
    const bool needs_function{
        info.uses_global_increment || info.uses_global_decrement || info.uses_atomic_f32_add ||
        info.uses_atomic_f16x2_add || info.uses_atomic_f16x2_min || info.uses_atomic_f16x2_max ||
        info.uses_atomic_f32x2_add || info.uses_atomic_f32x2_min || info.uses_atomic_f32x2_max};
    if (needs_function) {
        AddCapability(spv::Capability::VariablePointersStorageBuffer);
    }
    if (info.uses_global_increment) {
        increment_cas_ssbo = CasLoop(*this, Operation::Increment, storage_types.U32.array,
                                     storage_types.U32.element, U32[1], spv::Scope::Device);
    }
    if (info.uses_global_decrement) {
        decrement_cas_ssbo = CasLoop(*this, Operation::Decrement, storage_types.U32.array,
                                     storage_types.U32.element, U32[1], spv::Scope::Device);
    }
    if (info.uses_atomic_f32_add && !native_f32_add) {
        f32_add_cas = CasLoop(*this, Operation::FPAdd, storage_types.U32.array,
                              storage_types.U32.element, F32[1], spv::Scope::Device);
    }
    if (info.uses_atomic_f16x2_add) {
        f16x2_add_cas = CasLoop(*this, Operation::FPAdd, storage_types.U32.array,
                                storage_types.U32.element, F16[2], spv::Scope::Device);
    }
    if (info.uses_atomic_f16x2_min) {
        f16x2_min_cas = CasLoop(*this, Operation::FPMin, storage_types.U32.array,
                                storage_types.U32.element, F16[2], spv::Scope::Device);
    }
    if (info.uses_atomic_f16x2_max) {
        f16x2_max_cas = CasLoop(*this, Operation::FPMax, storage_types.U32.array,
                                storage_types.U32.element, F16[2], spv::Scope::Device);
    }
    if (info.uses_atomic_f32x2_add) {
        f32x2_add_cas = CasLoop(*this, Operation::FPAdd, storage_types.U32.array,
                                storage_types.U32.element, F32[2], spv::Scope::Device);
    }
    if (info.uses_atomic_f32x2_min) {
        f32x2_min_cas = CasLoop(*this, Operation::FPMin, storage_types.U32.array,
                                storage_types.U32.element, F32[2], spv::Scope::Device);
    }
    if (info.uses_atomic_f32x2_max) {
        f32x2_max_cas = CasLoop(*this, Operation::FPMax, storage_types.U32.array,
                                storage_types.U32.element, F32[2], spv::Scope::Device);
    }
}

void EmitContext::DefineTextureBuffers(const Info& info, u32& binding) {
    if (info.texture_buffer_descriptors.empty()) {
        return;
    }
    const spv::ImageFormat format{spv::ImageFormat::Unknown};
    image_buffer_type = TypeImage(F32[1], spv::Dim::Buffer, 0U, false, false, 1, format);

    const Id type{TypePointer(spv::StorageClass::UniformConstant, image_buffer_type)};
    texture_buffers.reserve(info.texture_buffer_descriptors.size());
    for (const TextureBufferDescriptor& desc : info.texture_buffer_descriptors) {
        if (desc.count != 1) {
            throw NotImplementedException("Array of texture buffers");
        }
        const Id id{AddGlobalVariable(type, spv::StorageClass::UniformConstant)};
        Decorate(id, spv::Decoration::Binding, binding);
        Decorate(id, spv::Decoration::DescriptorSet, 0U);
        Name(id, NameOf(stage, desc, "texbuf"));
        texture_buffers.push_back({
            .id = id,
            .count = desc.count,
        });
        if (profile.supported_spirv >= 0x00010400) {
            interfaces.push_back(id);
        }
        ++binding;
    }
}

void EmitContext::DefineImageBuffers(const Info& info, u32& binding) {
    image_buffers.reserve(info.image_buffer_descriptors.size());
    for (const ImageBufferDescriptor& desc : info.image_buffer_descriptors) {
        const spv::ImageFormat format{GetImageFormat(desc.format)};
        const Id sampled_type{desc.is_integer ? U32[1] : F32[1]};
        const Id image_type{
            TypeImage(sampled_type, spv::Dim::Buffer, false, false, false, 2, format)};
        const Id pointer_type{TypePointer(spv::StorageClass::UniformConstant, image_type)};
        const Id id{AddGlobalVariable(pointer_type, spv::StorageClass::UniformConstant)};
        Decorate(id, spv::Decoration::Binding, binding);
        Decorate(id, spv::Decoration::DescriptorSet, 0U);
        Name(id, NameOf(stage, desc, "imgbuf"));
        image_buffers.push_back({
            .id = id,
            .image_type = image_type,
            .pointer_type = pointer_type,
            .count = desc.count,
            .is_integer = desc.is_integer,
        });
        if (profile.supported_spirv >= 0x00010400) {
            interfaces.push_back(id);
        }
        ++binding;
    }
}

void EmitContext::DefineTextures(const Info& info, u32& binding, u32& scaling_index) {
    textures.reserve(info.texture_descriptors.size());
    for (const TextureDescriptor& desc : info.texture_descriptors) {
        const Id image_type{ImageType(*this, desc)};
        const Id sampled_type{TypeSampledImage(image_type)};
        const Id pointer_type{TypePointer(spv::StorageClass::UniformConstant, sampled_type)};
        const Id desc_type{DescType(*this, sampled_type, pointer_type, desc.count)};
        const Id id{AddGlobalVariable(desc_type, spv::StorageClass::UniformConstant)};
        Decorate(id, spv::Decoration::Binding, binding);
        Decorate(id, spv::Decoration::DescriptorSet, 0U);
        Name(id, NameOf(stage, desc, "tex"));
        textures.push_back({
            .id = id,
            .sampled_type = sampled_type,
            .pointer_type = pointer_type,
            .image_type = image_type,
            .count = desc.count,
            .is_multisample = desc.is_multisample,
            .is_integer = desc.is_integer,
        });
        if (profile.supported_spirv >= 0x00010400) {
            interfaces.push_back(id);
        }
        ++binding;
        ++scaling_index;
    }
    if (info.uses_atomic_image_u32) {
        image_u32 = TypePointer(spv::StorageClass::Image, U32[1]);
    }
}

void EmitContext::DefineImages(const Info& info, u32& binding, u32& scaling_index) {
    images.reserve(info.image_descriptors.size());
    for (const ImageDescriptor& desc : info.image_descriptors) {
        const Id sampled_type{desc.is_integer ? U32[1] : F32[1]};
        const Id image_type{ImageType(*this, desc, sampled_type)};
        const Id pointer_type{TypePointer(spv::StorageClass::UniformConstant, image_type)};
        const Id id{AddGlobalVariable(pointer_type, spv::StorageClass::UniformConstant)};
        Decorate(id, spv::Decoration::Binding, binding);
        Decorate(id, spv::Decoration::DescriptorSet, 0U);
        Name(id, NameOf(stage, desc, "img"));
        images.push_back({
            .id = id,
            .image_type = image_type,
            .pointer_type = pointer_type,
            .count = desc.count,
            .is_integer = desc.is_integer,
        });
        if (profile.supported_spirv >= 0x00010400) {
            interfaces.push_back(id);
        }
        ++binding;
        ++scaling_index;
    }
}

void EmitContext::DefineInputs(const IR::Program& program) {
    const Info& info{program.info};
    const VaryingState loads{info.loads.mask | info.passthrough.mask};
    const bool noperspective_as_smooth{
        stage == Stage::Fragment && runtime_info.scaled_noperspective_inputs &&
        std::ranges::any_of(info.interpolation,
                            [](Interpolation mode) {
                                return mode == Interpolation::NoPerspective;
                            })};

    if (info.uses_workgroup_id) {
        workgroup_id = DefineInput(*this, U32[3], false, spv::BuiltIn::WorkgroupId);
    }
    if (info.uses_local_invocation_id) {
        local_invocation_id = DefineInput(*this, U32[3], false, spv::BuiltIn::LocalInvocationId);
    }
    if (info.uses_invocation_id) {
        invocation_id = DefineInput(*this, U32[1], false, spv::BuiltIn::InvocationId);
    }
    if (info.uses_invocation_info &&
        (stage == Shader::Stage::TessellationControl || stage == Shader::Stage::TessellationEval)) {
        patch_vertices_in = DefineInput(*this, U32[1], false, spv::BuiltIn::PatchVertices);
    }
    if (info.uses_sample_id) {
        sample_id = DefineInput(*this, U32[1], false, spv::BuiltIn::SampleId);
        if (stage == Stage::Fragment) {
            Decorate(sample_id, spv::Decoration::Flat);
        }
    }
    if (info.uses_is_helper_invocation) {
        is_helper_invocation = DefineInput(*this, U1, false, spv::BuiltIn::HelperInvocation);
    }
    if (info.uses_subgroup_mask && profile.SupportsSubgroupStage(stage)) {
        subgroup_mask_eq = DefineInput(*this, U32[4], false, spv::BuiltIn::SubgroupEqMaskKHR);
        subgroup_mask_lt = DefineInput(*this, U32[4], false, spv::BuiltIn::SubgroupLtMaskKHR);
        subgroup_mask_le = DefineInput(*this, U32[4], false, spv::BuiltIn::SubgroupLeMaskKHR);
        subgroup_mask_gt = DefineInput(*this, U32[4], false, spv::BuiltIn::SubgroupGtMaskKHR);
        subgroup_mask_ge = DefineInput(*this, U32[4], false, spv::BuiltIn::SubgroupGeMaskKHR);
        if (stage == Stage::Fragment) {
            Decorate(subgroup_mask_eq, spv::Decoration::Flat);
            Decorate(subgroup_mask_lt, spv::Decoration::Flat);
            Decorate(subgroup_mask_le, spv::Decoration::Flat);
            Decorate(subgroup_mask_gt, spv::Decoration::Flat);
            Decorate(subgroup_mask_ge, spv::Decoration::Flat);
        }
    }
    if ((info.uses_fswzadd || info.uses_subgroup_invocation_id || info.uses_subgroup_shuffles ||
         (profile.warp_size_potentially_larger_than_guest &&
          (info.uses_subgroup_vote || info.uses_subgroup_mask))) &&
        profile.SupportsSubgroupStage(stage)) {
        AddCapability(spv::Capability::GroupNonUniform);
        subgroup_local_invocation_id =
            DefineInput(*this, U32[1], false, spv::BuiltIn::SubgroupLocalInvocationId);
        if (stage == Stage::Fragment) {
            Decorate(subgroup_local_invocation_id, spv::Decoration::Flat);
        }
    }
    if (info.uses_fswzadd) {
        const Id f32_one{Const(1.0f)};
        const Id f32_minus_one{Const(-1.0f)};
        const Id f32_zero{Const(0.0f)};
        fswzadd_lut_a = ConstantComposite(F32[4], f32_minus_one, f32_one, f32_minus_one, f32_zero);
        fswzadd_lut_b =
            ConstantComposite(F32[4], f32_minus_one, f32_minus_one, f32_one, f32_minus_one);
    }
    if (loads[IR::Attribute::PrimitiveId]) {
        primitive_id = DefineInput(*this, U32[1], false, spv::BuiltIn::PrimitiveId);
        if (stage == Stage::Fragment) {
            Decorate(primitive_id, spv::Decoration::Flat);
        }
    }
    if (loads[IR::Attribute::Layer]) {
        AddCapability(spv::Capability::Geometry);
        layer = DefineInput(*this, U32[1], false, spv::BuiltIn::Layer);
        Decorate(layer, spv::Decoration::Flat);
    }
    if (loads.AnyComponent(IR::Attribute::PositionX) ||
        info.passthrough.AnyComponent(IR::Attribute::PositionX) || noperspective_as_smooth) {
        const bool is_fragment{stage == Stage::Fragment};
        if (!is_fragment && profile.has_broken_spirv_position_input) {
            need_input_position_indirect = true;

            const Id input_position_struct = TypeStruct(F32[4]);
            input_position = DefineInput(*this, input_position_struct, true);

            MemberDecorate(input_position_struct, 0, spv::Decoration::BuiltIn,
                           static_cast<unsigned>(spv::BuiltIn::Position));
            Decorate(input_position_struct, spv::Decoration::Block);
        } else {
            const spv::BuiltIn built_in{is_fragment ? spv::BuiltIn::FragCoord
                                                    : spv::BuiltIn::Position};
            input_position = DefineInput(*this, F32[4], true, built_in);

            if (profile.support_geometry_shader_passthrough) {
                if (info.passthrough.AnyComponent(IR::Attribute::PositionX)) {
                    Decorate(input_position, spv::Decoration::PassthroughNV);
                }
            }
        }
    }
    if (loads[IR::Attribute::InstanceId]) {
        if (profile.support_vertex_instance_id) {
            instance_id = DefineInput(*this, U32[1], true, spv::BuiltIn::InstanceId);
            if (loads[IR::Attribute::BaseInstance]) {
                base_instance = DefineInput(*this, U32[1], true, spv::BuiltIn::BaseInstance);
            }
        } else {
            instance_index = DefineInput(*this, U32[1], true, spv::BuiltIn::InstanceIndex);
            base_instance = DefineInput(*this, U32[1], true, spv::BuiltIn::BaseInstance);
        }
    } else if (loads[IR::Attribute::BaseInstance]) {
        base_instance = DefineInput(*this, U32[1], true, spv::BuiltIn::BaseInstance);
    }
    if (loads[IR::Attribute::VertexId]) {
        if (profile.support_vertex_instance_id) {
            vertex_id = DefineInput(*this, U32[1], true, spv::BuiltIn::VertexId);
            if (loads[IR::Attribute::BaseVertex]) {
                base_vertex = DefineInput(*this, U32[1], true, spv::BuiltIn::BaseVertex);
            }
        } else {
            vertex_index = DefineInput(*this, U32[1], true, spv::BuiltIn::VertexIndex);
            base_vertex = DefineInput(*this, U32[1], true, spv::BuiltIn::BaseVertex);
        }
    } else if (loads[IR::Attribute::BaseVertex]) {
        base_vertex = DefineInput(*this, U32[1], true, spv::BuiltIn::BaseVertex);
    }
    if (loads[IR::Attribute::DrawID]) {
        draw_index = DefineInput(*this, U32[1], true, spv::BuiltIn::DrawIndex);
    }
    if (loads[IR::Attribute::FrontFace]) {
        front_face = DefineInput(*this, U1, true, spv::BuiltIn::FrontFacing);
    }
    if (loads[IR::Attribute::PointSpriteS] || loads[IR::Attribute::PointSpriteT] ||
        (stage == Stage::Fragment && runtime_info.point_coord_generics.any())) {
        point_coord = DefineInput(*this, F32[2], true, spv::BuiltIn::PointCoord);
    }
    if (loads[IR::Attribute::TessellationEvaluationPointU] ||
        loads[IR::Attribute::TessellationEvaluationPointV]) {
        tess_coord = DefineInput(*this, F32[3], false, spv::BuiltIn::TessCoord);
    }
    for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
        const AttributeType input_type{runtime_info.generic_input_types[index]};
        if (!runtime_info.previous_stage_stores.Generic(index)) {
            continue;
        }
        if (!loads.Generic(index)) {
            continue;
        }
        if (input_type == AttributeType::Disabled) {
            continue;
        }
        const Id type{GetAttributeType(*this, input_type)};
        const Id id{DefineInput(*this, type, true)};
        Decorate(id, spv::Decoration::Location, static_cast<u32>(index));
        Name(id, fmt::format("in_attr{}", index));
        input_generics[index] = GetAttributeInfo(*this, input_type, id);

        if (info.passthrough.Generic(index) && profile.support_geometry_shader_passthrough) {
            Decorate(id, spv::Decoration::PassthroughNV);
        }
        if (stage != Stage::Fragment) {
            continue;
        }
        const bool is_integer = input_type == AttributeType::SignedInt ||
                                input_type == AttributeType::UnsignedInt;
        if (runtime_info.per_primitive_generics.test(index)) {
            // Written per primitive by a mesh program: constant over the primitive.
            AddExtension("SPV_EXT_mesh_shader");
            AddCapability(spv::Capability::MeshShadingEXT);
            Decorate(id, spv::Decoration::PerPrimitiveEXT);
            if (is_integer) {
                Decorate(id, spv::Decoration::Flat);
            }
            continue;
        }
        if (is_integer) {
            Decorate(id, spv::Decoration::Flat);
        } else {
            switch (info.interpolation[index]) {
            case Interpolation::Smooth:
                break;
            case Interpolation::NoPerspective:
                // The previous stage may write it times Position.w (noperspective_outputs).
                if (!runtime_info.scaled_noperspective_inputs) {
                    Decorate(id, spv::Decoration::NoPerspective);
                } else {
                    noperspective_as_smooth_inputs[index] = true;
                }
                break;
            case Interpolation::Flat:
                Decorate(id, spv::Decoration::Flat);
                break;
            }
        }
    }
    if (runtime_info.mesh_fallback_vertex) {
        vertex_index = DefineInput(*this, U32[1], true, spv::BuiltIn::VertexIndex);
    }
    if (stage == Stage::TessellationEval) {
        for (size_t index = 0; index < info.uses_patches.size(); ++index) {
            if (!info.uses_patches[index]) {
                continue;
            }
            const Id id{DefineInput(*this, F32[4], false)};
            Decorate(id, spv::Decoration::Patch);
            Decorate(id, spv::Decoration::Location, static_cast<u32>(index));
            patches[index] = id;
        }
    }
}

void EmitContext::DefineMesh(const IR::Program& program) {
    if (stage != Stage::Task && stage != Stage::Mesh) {
        return;
    }
    mesh_fallback = runtime_info.mesh_fallback;
    if (!mesh_fallback) {
        AddExtension("SPV_EXT_mesh_shader");
        AddCapability(spv::Capability::MeshShadingEXT);
    }
    mesh_info = program.mesh;
    mesh_threads = program.workgroup_size[0];
    // The task's output region, which its mesh workgroups read as their input region.
    const Id payload_type{TypeArray(U32[1], Const(mesh_info.payload_words))};
    if (mesh_fallback) {
        // Workgroup memory still holds the stage buffer while the program runs; the epilogue copies it out. The map
        // and record buffers live in the pushed fallback buffers, one slot a workgroup. A mesh program with no task
        // reads the workgroup index from its own payload, as NVN's hardware provides it.
        const Id wg_pointer{TypePointer(spv::StorageClass::Workgroup, U32[1])};
        if (stage == Stage::Task || !runtime_info.mesh_fallback_task) {
            mesh_payload = DefineVariable(*this, payload_type, std::nullopt, spv::StorageClass::Workgroup);
            Name(mesh_payload, "isbe_payload");
            mesh_payload_u32 = wg_pointer;
        }
        if (stage == Stage::Task) {
            return;
        }
    } else {
        mesh_payload = DefineVariable(*this, payload_type, std::nullopt,
                                      spv::StorageClass::TaskPayloadWorkgroupEXT);
        Name(mesh_payload, "task_payload");
        mesh_payload_u32 = TypePointer(spv::StorageClass::TaskPayloadWorkgroupEXT, U32[1]);
        if (stage == Stage::Task) {
            return;
        }
    }
    // Output attribute region: groups of 32 vertices, then groups of 32 primitives, one 32-word
    // line per attribute. The map region keeps one map byte per word.
    const u32 vertex_lines{static_cast<u32>(mesh_info.vertex_lines.size())};
    const u32 primitive_lines{static_cast<u32>(mesh_info.primitive_lines.size())};
    mesh_primitive_base = Common::DivCeil(mesh_info.max_vertices, 32U) * vertex_lines * 32;
    mesh_attribute_words = (std::max)(
        mesh_primitive_base + Common::DivCeil(mesh_info.max_primitives, 32U) * primitive_lines * 32,
        1U);
    // Whole blocks of 32 primitives, as the attributes: the program writes the index bytes of every primitive block, lanes past
    // max_primitives too (a count of 0 wraps its min(count - 1, lane) to every lane). Sized to max_primitives, those writes were
    // clamped onto the last byte, the last primitive's last corner: with 126 primitives (most of OoT's) a stray vertex, flashing
    // triangles over Kokiri's foliage and ground (as nxnative found: "primitive index bytes for every primitive block")
    mesh_map_bytes = 4 + mesh_info.Corners() * Common::DivCeil(mesh_info.max_primitives, 32U) * 32;
    mesh_shared_u32 = TypePointer(spv::StorageClass::Workgroup, U32[1]);
    mesh_attributes = DefineVariable(*this, TypeArray(U32[1], Const(mesh_attribute_words)),
                                     std::nullopt, spv::StorageClass::Workgroup);
    Name(mesh_attributes, "isbe_attributes");
    mesh_map = DefineVariable(*this, TypeArray(U32[1], Const(mesh_map_bytes)), std::nullopt,
                              spv::StorageClass::Workgroup);
    Name(mesh_map, "isbe_map");
    mesh_primitive_count =
        DefineVariable(*this, U32[1], std::nullopt, spv::StorageClass::Workgroup);
    Name(mesh_primitive_count, "isbe_primitive_count");

    if (mesh_fallback) {
        return;   // (no mesh outputs here: the epilogue writes records)
    }

    const Id vertex_count{Const(mesh_info.max_vertices)};
    const Id primitive_count{Const(mesh_info.max_primitives)};
    const u32 corners{mesh_info.Corners()};
    mesh_primitive_indices =
        DefineVariable(*this, TypeArray(U32[corners], primitive_count),
                       corners == 1   ? spv::BuiltIn::PrimitivePointIndicesEXT
                       : corners == 2 ? spv::BuiltIn::PrimitiveLineIndicesEXT
                                      : spv::BuiltIn::PrimitiveTriangleIndicesEXT,
                       spv::StorageClass::Output);
    std::array<Id, IR::NUM_GENERICS> generics[2]{};
    Id point_size{}, primitive_layer{}, primitive_viewport{}, primitive_id_output{};
    const auto per_primitive_builtin = [&](Id& variable, spv::BuiltIn builtin) {
        if (!Sirit::ValidId(variable)) {
            variable = DefineVariable(*this, TypeArray(U32[1], primitive_count), builtin,
                                      spv::StorageClass::Output);
            Decorate(variable, spv::Decoration::PerPrimitiveEXT);
        }
        return MeshLineTarget{variable, ~0U, false};
    };
    const auto target = [&](u32 line, bool per_primitive) -> MeshLineTarget {
        const IR::Attribute attribute{line};
        if (IR::IsGeneric(attribute)) {
            const u32 index{IR::GenericAttributeIndex(attribute)};
            Id& variable{generics[per_primitive][index]};
            if (!Sirit::ValidId(variable)) {
                variable = DefineVariable(
                    *this, TypeArray(F32[4], per_primitive ? primitive_count : vertex_count),
                    std::nullopt, spv::StorageClass::Output);
                Decorate(variable, spv::Decoration::Location, index);
                if (per_primitive) {
                    Decorate(variable, spv::Decoration::PerPrimitiveEXT);
                }
            }
            return {variable, IR::GenericAttributeElement(attribute), true};
        }
        if (per_primitive) {
            switch (attribute) {
            case IR::Attribute::PrimitiveId:
                return per_primitive_builtin(primitive_id_output, spv::BuiltIn::PrimitiveId);
            case IR::Attribute::Layer:
                return per_primitive_builtin(primitive_layer, spv::BuiltIn::Layer);
            case IR::Attribute::ViewportIndex:
                return per_primitive_builtin(primitive_viewport, spv::BuiltIn::ViewportIndex);
            default:
                return {};
            }
        }
        switch (attribute) {
        case IR::Attribute::PositionX:
        case IR::Attribute::PositionY:
        case IR::Attribute::PositionZ:
        case IR::Attribute::PositionW:
            if (!Sirit::ValidId(output_position)) {
                output_position = DefineVariable(*this, TypeArray(F32[4], vertex_count),
                                                 spv::BuiltIn::Position, spv::StorageClass::Output);
                if (position_invariant) Decorate(output_position, spv::Decoration::Invariant);   // (amd-fix, mma_layout.h)
            }
            return {output_position,
                    static_cast<u32>(attribute) - static_cast<u32>(IR::Attribute::PositionX), true};
        case IR::Attribute::PointSize:
            if (!Sirit::ValidId(point_size)) {
                point_size = DefineVariable(*this, TypeArray(F32[1], vertex_count),
                                            spv::BuiltIn::PointSize, spv::StorageClass::Output);
            }
            return {point_size, ~0U, true};
        default:
            return {}; // per-primitive values in vertex lines have no vertex output
        }
    };
    for (const u32 line : mesh_info.vertex_lines) {
        mesh_vertex_targets.push_back(target(line, false));
    }
    // (hookshot) A component of a generic output no line writes was undefined at the fragment stage (the console's: 0, and 1 for .w,
    // as the vertex stage's defaults here). The foliage is drawn by mesh programs: neon plants. RX_VK_MESH_NO_DEFAULTS=1: as before
    if (!env_on("RX_VK_MESH_NO_DEFAULTS")) {
        std::array<u32, IR::NUM_GENERICS> written{};
        for (const u32 line : mesh_info.vertex_lines) {
            const IR::Attribute attribute{line};
            if (IR::IsGeneric(attribute)) {
                written[IR::GenericAttributeIndex(attribute)] |= 1U << IR::GenericAttributeElement(attribute);
            }
        }
        for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
            if (Sirit::ValidId(generics[0][index]) && written[index] != 0xF) {
                mesh_vertex_defaults.push_back({generics[0][index], 0xFU & ~written[index]});
            }
        }
    }
    for (const u32 line : mesh_info.primitive_lines) {
        mesh_primitive_targets.push_back(target(line, true));
    }
}

void EmitContext::DefineOutputs(const IR::Program& program) {
    const Info& info{program.info};
    if (stage == Stage::Task || stage == Stage::Mesh) {
        return; // DefineMesh
    }
    const std::optional<u32> invocations{program.invocations};
    if (runtime_info.convert_depth_mode || info.stores.AnyComponent(IR::Attribute::PositionX) ||
        stage == Stage::VertexB) {
        output_position = DefineOutput(*this, F32[4], invocations, spv::BuiltIn::Position);
        if (position_invariant) Decorate(output_position, spv::Decoration::Invariant);   // (amd-fix, mma_layout.h)
    }
    if (info.stores[IR::Attribute::PointSize] || runtime_info.fixed_state_point_size) {
        if (stage == Stage::Fragment) {
            throw NotImplementedException("Storing PointSize in fragment stage");
        }
        output_point_size = DefineOutput(*this, F32[1], invocations, spv::BuiltIn::PointSize);
    }
    if (info.stores.ClipDistances()) {
        if (stage == Stage::Fragment) {
            throw NotImplementedException("Storing ClipDistance in fragment stage");
        }
        if (profile.max_user_clip_distances > 0) {
            const u32 used{(std::min)(profile.max_user_clip_distances, 8u)};
            const std::array<Id, 8> zero{f32_zero_value, f32_zero_value, f32_zero_value,
                                         f32_zero_value, f32_zero_value, f32_zero_value,
                                         f32_zero_value, f32_zero_value};
            const Id type{TypeArray(F32[1], Const(used))};
            const Id initializer{ConstantComposite(type, std::span(zero).subspan(0, used))};
            clip_distances =
                DefineOutput(*this, type, invocations, spv::BuiltIn::ClipDistance, initializer);
        }
    }
    if (info.stores[IR::Attribute::Layer] &&
        (profile.support_viewport_index_layer_non_geometry || stage == Stage::Geometry)) {
        if (stage == Stage::Fragment) {
            throw NotImplementedException("Storing Layer in fragment stage");
        }
        layer = DefineOutput(*this, U32[1], invocations, spv::BuiltIn::Layer);
    }
    if (info.stores[IR::Attribute::ViewportIndex] &&
        (profile.support_viewport_index_layer_non_geometry || stage == Stage::Geometry)) {
        if (stage == Stage::Fragment) {
            throw NotImplementedException("Storing ViewportIndex in fragment stage");
        }
        viewport_index = DefineOutput(*this, U32[1], invocations, spv::BuiltIn::ViewportIndex);
    }
    if (info.stores[IR::Attribute::ViewportMask] && profile.support_viewport_mask) {
        viewport_mask = DefineOutput(*this, TypeArray(U32[1], Const(1u)), std::nullopt,
                                     spv::BuiltIn::ViewportMaskNV);
    }
    for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
        if (info.stores.Generic(index)) {
            DefineGenericOutput(*this, index, invocations);
        }
    }
    switch (stage) {
    case Stage::TessellationControl:
        if (info.stores_tess_level_outer) {
            const Id type{TypeArray(F32[1], Const(4U))};
            output_tess_level_outer =
                DefineOutput(*this, type, std::nullopt, spv::BuiltIn::TessLevelOuter);
            Decorate(output_tess_level_outer, spv::Decoration::Patch);
        }
        if (info.stores_tess_level_inner) {
            const Id type{TypeArray(F32[1], Const(2U))};
            output_tess_level_inner =
                DefineOutput(*this, type, std::nullopt, spv::BuiltIn::TessLevelInner);
            Decorate(output_tess_level_inner, spv::Decoration::Patch);
        }
        for (size_t index = 0; index < info.uses_patches.size(); ++index) {
            if (!info.uses_patches[index]) {
                continue;
            }
            const Id id{DefineOutput(*this, F32[4], std::nullopt)};
            Decorate(id, spv::Decoration::Patch);
            Decorate(id, spv::Decoration::Location, static_cast<u32>(index));
            patches[index] = id;
        }
        break;
    case Stage::Fragment:
        for (u32 index = 0; index < 8; ++index) {
            const bool need_dual_source = runtime_info.dual_source_blend && index <= 1;
            if (!need_dual_source && !info.stores_frag_color[index] &&
                !profile.need_declared_frag_colors) {
                continue;
            }
            const Id type{GetAttributeType(*this, runtime_info.color_output_types[index])};
            frag_color[index] = DefineOutput(*this, type, std::nullopt);
            // Correct mapping for dual-source blending
            if (runtime_info.dual_source_blend && index <= 1) {
                Decorate(frag_color[index], spv::Decoration::Location, 0u);
                Decorate(frag_color[index], spv::Decoration::Index, index);
                Name(frag_color[index], index == 0 ? "frag_color0" : "frag_color0_secondary");
            } else {
                Decorate(frag_color[index], spv::Decoration::Location, index);
                Name(frag_color[index], fmt::format("frag_color{}", index));
            }
        }
        if (info.stores_frag_depth) {
            frag_depth = DefineOutput(*this, F32[1], std::nullopt);
            Decorate(frag_depth, spv::Decoration::BuiltIn, spv::BuiltIn::FragDepth);
        }
        if (info.stores_sample_mask) {
            const Id array_type{TypeArray(U32[1], Const(1U))};
            sample_mask = DefineOutput(*this, array_type, std::nullopt);
            Decorate(sample_mask, spv::Decoration::BuiltIn, spv::BuiltIn::SampleMask);
        }
        break;
    default:
        break;
    }
}

} // namespace Shader::Backend::SPIRV
