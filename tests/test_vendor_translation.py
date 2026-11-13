"""The translation's vendor fixes (gpu/vk/vk_vendor.h): out-of-bounds texel fetches answered with zero, NaN tests on bits.

Both are cut out of the translator (FetchInBounds in backend/spirv/emit_spirv_image.cpp, IsNan in
emit_spirv_floating_point.cpp) into a Sirit fixture: a compute shader that fetches from an image of every type a fetch
reads (texel buffer, 1D, 1D array, 2D, 2D rect, 2D array, 3D, 2D multisampled) through FetchInBounds, gathers from a 2D,
2D rect, 2D array and cube texture through ImageGatherSubpixelOffset, and tests a 16-, 32- and 64-bit float with IsNan,
made once with the fixes off (fetch_bounds, nan_bits false: what NVIDIA gets) and once
on. Both must pass spirv-val. Off, IsNan is OpIsNan alone, as it was; on, there is no OpIsNan left, and every fetch's
coordinates and level are checked against the image's size and levels. The fetch path with fetch_bounds off is checked
to be the translator's old one, word for word. Nothing runs on a GPU.

    python -I -B tests/test_vendor_translation.py --headers <SPIRV-Headers/include> [--cxx g++] [--spirv-val spirv-val]
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"]
# (spirv-val: SPIRV-Tools comes with the Linux runner's packages, not on Windows)
RUN_ALL_SKIP = {"windows": "needs spirv-val: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
SPIRV = ROOT / "gpu" / "vk" / "eden" / "shader_recompiler" / "backend" / "spirv"


def block(text, marker, start=0):
    at = text.index(marker, start)
    end = text.index("{", at) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return at, end


FIXTURE = r"""
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <sirit/sirit.h>
using u32 = std::uint32_t;
using Sirit::Id;
bool nan_bits = false, fetch_bounds = false, fetch_bounds_buffers = false, gather_offset = false;
enum class TextureType { Color1D, ColorArray1D, Color2D, ColorArray2D, Color3D, ColorCube, ColorArrayCube, Buffer, Color2DRect };
namespace IR { struct TextureInstInfo { TextureType type; }; }
struct EmitContext : Sirit::Module {
    using Sirit::Module::Module;
    Id U1{}, u32_zero_value{}, f32_zero_value{};
    std::array<Id, 5> U32{}, F32{};
    Id Const(u32 v) { return Constant(U32[1], v); }
    Id Const(float v) { return Constant(F32[1], v); }
};
@@ISNAN@@
@@FETCH@@
@@GATHER@@
struct Case { TextureType type; spv::Dim dim; bool arrayed, ms, lod; u32 dims; };
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    nan_bits = fetch_bounds = gather_offset = argv[1][0] != '0';
    fetch_bounds_buffers = argv[1][0] == '2';   // (robustBufferAccess2 off: texel buffers checked too)
    EmitContext ctx{0x00010600};
    ctx.AddCapability(spv::Capability::Shader);
    ctx.AddCapability(spv::Capability::ImageQuery);
    ctx.AddCapability(spv::Capability::Sampled1D);
    ctx.AddCapability(spv::Capability::SampledBuffer);
    ctx.AddCapability(spv::Capability::Float16);
    ctx.AddCapability(spv::Capability::Float64);
    ctx.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    ctx.U1 = ctx.TypeBool();
    const Id u32t{ctx.TypeInt(32, false)}, f32t{ctx.TypeFloat(32)}, f16t{ctx.TypeFloat(16)}, f64t{ctx.TypeFloat(64)};
    ctx.U32[1] = u32t, ctx.F32[1] = f32t;
    for (int i = 2; i <= 4; i++) ctx.U32[i] = ctx.TypeVector(u32t, i), ctx.F32[i] = ctx.TypeVector(f32t, i);
    ctx.u32_zero_value = ctx.Const(0u);
    ctx.f32_zero_value = ctx.Const(0.0f);
    // (the inputs, from a storage buffer: words the validator cannot fold)
    const Id words{ctx.TypeRuntimeArray(u32t)};
    ctx.Decorate(words, spv::Decoration::ArrayStride, 4u);
    const Id block_type{ctx.TypeStruct(words)};
    ctx.Decorate(block_type, spv::Decoration::Block);
    ctx.MemberDecorate(block_type, 0, spv::Decoration::Offset, 0u);
    const Id buffer{ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::StorageBuffer, block_type), spv::StorageClass::StorageBuffer)};
    ctx.Decorate(buffer, spv::Decoration::DescriptorSet, 0u);
    ctx.Decorate(buffer, spv::Decoration::Binding, 0u);
    const Id word_ptr{ctx.TypePointer(spv::StorageClass::StorageBuffer, u32t)};
    const std::vector<Case> cases{
        {TextureType::Buffer, spv::Dim::Buffer, false, false, false, 1}, {TextureType::Color1D, spv::Dim::Dim1D, false, false, true, 1},
        {TextureType::ColorArray1D, spv::Dim::Dim1D, true, false, true, 2}, {TextureType::Color2D, spv::Dim::Dim2D, false, false, true, 2},
        {TextureType::Color2DRect, spv::Dim::Dim2D, false, false, true, 2}, {TextureType::ColorArray2D, spv::Dim::Dim2D, true, false, true, 3},
        {TextureType::Color3D, spv::Dim::Dim3D, false, false, true, 3}, {TextureType::Color2D, spv::Dim::Dim2D, false, true, false, 2},
    };
    std::vector<Id> images, interface{buffer};
    for (size_t i = 0; i < cases.size(); i++) {
        const Case& c = cases[i];
        const Id image_type{ctx.TypeImage(f32t, c.dim, 0, c.arrayed, c.ms, 1, spv::ImageFormat::Unknown)};
        const Id var{ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::UniformConstant, image_type), spv::StorageClass::UniformConstant)};
        ctx.Decorate(var, spv::Decoration::DescriptorSet, 0u);
        ctx.Decorate(var, spv::Decoration::Binding, u32(1 + i));
        images.push_back(var);
        interface.push_back(var);
    }
    // (gathers through ImageGatherSubpixelOffset: 2D, 2D rect, 2D array, cube; the profile's gather offset is on with the fixes)
    struct G { TextureType type; spv::Dim dim; bool arrayed; u32 dims; };
    const std::vector<G> gathers{{TextureType::Color2D, spv::Dim::Dim2D, false, 2}, {TextureType::Color2DRect, spv::Dim::Dim2D, false, 2},
                                 {TextureType::ColorArray2D, spv::Dim::Dim2D, true, 3}, {TextureType::ColorCube, spv::Dim::Cube, false, 3}};
    std::vector<Id> gather_vars;
    for (size_t i = 0; i < gathers.size(); i++) {
        const Id image_type{ctx.TypeImage(f32t, gathers[i].dim, 0, gathers[i].arrayed, false, 1, spv::ImageFormat::Unknown)};
        const Id var{ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::UniformConstant, ctx.TypeSampledImage(image_type)),
                                           spv::StorageClass::UniformConstant)};
        ctx.Decorate(var, spv::Decoration::DescriptorSet, 0u);
        ctx.Decorate(var, spv::Decoration::Binding, u32(1 + cases.size() + i));
        gather_vars.push_back(var);
        interface.push_back(var);
    }
    const Id voidt{ctx.TypeVoid()};
    const Id main{ctx.OpFunction(voidt, spv::FunctionControlMask::MaskNone, ctx.TypeFunction(voidt))};
    ctx.AddLabel();
    u32 next = 0;
    const auto input = [&] { return ctx.OpLoad(u32t, ctx.OpAccessChain(word_ptr, buffer, ctx.u32_zero_value, ctx.Const(next++))); };
    const auto output = [&](Id value) { ctx.OpStore(ctx.OpAccessChain(word_ptr, buffer, ctx.u32_zero_value, ctx.Const(next++)), value); };
    for (size_t i = 0; i < cases.size(); i++) {
        const Case& c = cases[i];
        const Id image_type{ctx.TypeImage(f32t, c.dim, 0, c.arrayed, c.ms, 1, spv::ImageFormat::Unknown)};
        const Id image{ctx.OpLoad(image_type, images[i])};
        Id coords{input()};
        if (c.dims > 1) {
            std::vector<Id> parts{coords};
            for (u32 k = 1; k < c.dims; k++) parts.push_back(input());
            coords = ctx.OpCompositeConstruct(ctx.U32[c.dims], parts);
        }
        Id lod{c.lod ? input() : Id{}};
        const Id sample{c.ms ? input() : Id{}};
        Id in_bounds{};
        Id sample_id{sample};
        if (fetch_bounds) in_bounds = FetchInBounds(ctx, IR::TextureInstInfo{c.type}, image, coords, lod, sample_id);
        Id color{};
        if (c.ms) color = ctx.OpImageFetch(ctx.F32[4], image, coords, spv::ImageOperandsMask::Sample, sample_id);
        else if (c.lod) color = ctx.OpImageFetch(ctx.F32[4], image, coords, spv::ImageOperandsMask::Lod, lod);
        else color = ctx.OpImageFetch(ctx.F32[4], image, coords);
        if (Sirit::ValidId(in_bounds)) color = ctx.OpSelect(ctx.F32[4], in_bounds, color, ctx.ConstantNull(ctx.F32[4]));
        output(ctx.OpBitcast(u32t, ctx.OpCompositeExtract(f32t, color, 0u)));
    }
    for (size_t i = 0; i < gathers.size(); i++) {
        const G& g = gathers[i];
        const Id image_type{ctx.TypeImage(f32t, g.dim, 0, g.arrayed, false, 1, spv::ImageFormat::Unknown)};
        const Id sampled{ctx.OpLoad(ctx.TypeSampledImage(image_type), gather_vars[i])};
        std::vector<Id> parts;
        for (u32 k = 0; k < g.dims; k++) parts.push_back(ctx.OpBitcast(f32t, input()));
        Id coords{ctx.OpCompositeConstruct(ctx.F32[g.dims], parts)};
        if (gather_offset) coords = ImageGatherSubpixelOffset(ctx, IR::TextureInstInfo{g.type}, ctx.OpImage(image_type, sampled), coords);
        const Id texels{ctx.OpImageGather(ctx.F32[4], sampled, coords, ctx.u32_zero_value)};
        output(ctx.OpBitcast(u32t, ctx.OpCompositeExtract(f32t, texels, 0u)));
    }
    const Id f32v{ctx.OpBitcast(f32t, input())};
    const Id one{ctx.Const(1u)};
    output(ctx.OpSelect(u32t, IsNan(ctx, ctx.OpFConvert(f16t, f32v), 16), one, ctx.u32_zero_value));
    output(ctx.OpSelect(u32t, IsNan(ctx, f32v, 32), one, ctx.u32_zero_value));
    output(ctx.OpSelect(u32t, IsNan(ctx, ctx.OpFConvert(f64t, f32v), 64), one, ctx.u32_zero_value));
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", interface);
    ctx.AddExecutionMode(main, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    std::vector<u32> code{ctx.Assemble()};
    FILE* f = std::fopen(argv[2], "wb");
    if (!f) return 3;
    std::fwrite(code.data(), 4, code.size(), f);
    std::fclose(f);
    return 0;
}
"""

OP_IS_NAN, OP_IMAGE_FETCH, OP_SELECT, OP_QUERY_SIZE_LOD, OP_QUERY_SIZE, OP_QUERY_LEVELS = 156, 95, 169, 103, 104, 106
OP_U_GREATER_THAN, OP_BITCAST, OP_QUERY_SAMPLES = 172, 124, 107
# (the fetch path with fetch_bounds off: the translator's before the fix, word for word)
OLD_FETCH = """    const ImageOperands operands(lod, ms);
    const Id color{Emit(&EmitContext::OpImageSparseFetch, &EmitContext::OpImageFetch, ctx, inst,
                        result_type, TextureImage(ctx, info, index), coords,
                        operands.MaskOptional(), operands.Span())};
    return is_integer ? ctx.OpBitcast(ctx.F32[4], color) : color;
}"""


def opcodes(path):
    data = path.read_bytes()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    i, ops = 5, []
    while i < len(words):
        ops.append(words[i] & 0xFFFF)
        i += words[i] >> 16
    return ops


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--spirv-val", default="spirv-val")
    parser.add_argument("--keep", type=Path, help="a folder to keep the two modules in (fixes0.spv, fixes1.spv)")
    args = parser.parse_args()
    image = (SPIRV / "emit_spirv_image.cpp").read_text(encoding="utf-8")
    fp = (SPIRV / "emit_spirv_floating_point.cpp").read_text(encoding="utf-8")
    fetch_at, fetch_end = block(image, "Id FetchInBounds(")
    nan_at, nan_end = block(fp, "Id IsNan(EmitContext& ctx, Id value, u32 bits)")
    gather_at, gather_end = block(image, "Id ImageGatherSubpixelOffset(")
    emit_at, emit_end = block(image, "Id EmitImageFetch(")
    emit = image[emit_at:emit_end]
    guard_at, guard_end = block(emit, "if (fetch_bounds) {")
    if not emit[guard_end:].lstrip("\n").startswith(OLD_FETCH):
        raise SystemExit("FAIL: EmitImageFetch's path with fetch_bounds off is not the old one")
    for name in ("EmitFPIsNan16", "EmitFPIsNan32", "EmitFPIsNan64"):
        at, end = block(fp, "Id %s(" % name)
        if "return IsNan(ctx, value, %s);" % name[-2:] not in fp[at:end]:
            raise SystemExit("FAIL: %s does not go through IsNan" % name)
    get_set = (SPIRV / "emit_spirv_context_get_set.cpp").read_text(encoding="utf-8")
    convert = (SPIRV / "emit_spirv_convert.cpp").read_text(encoding="utf-8")
    if "IsNan(ctx, value, 32)" not in get_set[slice(*block(get_set, "void EmitSetFragColor("))]:
        raise SystemExit("FAIL: the colour outputs' NaN guard does not go through IsNan")
    if "IsNan(ctx, result, 16)" not in convert[slice(*block(convert, "Id EmitConvertF16F32("))]:
        raise SystemExit("FAIL: EmitConvertF16F32 does not go through IsNan")
    fixture = FIXTURE.replace("@@ISNAN@@", fp[nan_at:nan_end]).replace("@@FETCH@@", image[fetch_at:fetch_end]).replace(
        "@@GATHER@@", image[gather_at:gather_end])
    sirit = ROOT / "gpu" / "vk" / "sirit"
    with tempfile.TemporaryDirectory(prefix="vendor-translation-") as tmp:
        tmp = Path(tmp)
        cpp, exe = tmp / "fixture.cpp", tmp / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        r = subprocess.run([args.cxx, "-std=c++20", "-O1", "-I" + str(sirit / "include"), "-I" + str(sirit / "src"),
                            "-I" + str(args.headers), str(cpp), *map(str, sorted((sirit / "src").rglob("*.cpp"))), "-o", str(exe)],
                           capture_output=True, text=True, timeout=600)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        ops = {}
        for on in ("0", "1", "2"):
            spv = tmp / ("fixes%s.spv" % on)
            r = subprocess.run([str(exe), on, str(spv)], capture_output=True, text=True, timeout=30)
            if r.returncode:
                raise SystemExit("FAIL: the fixture stopped (%d): %s" % (r.returncode, r.stdout + r.stderr))
            v = subprocess.run([args.spirv_val, "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True, timeout=60)
            if v.returncode:
                raise SystemExit("FAIL: spirv-val, fixes %s:\n%s" % (["off", "on", "on, texel buffers too"][int(on)], v.stdout + v.stderr))
            ops[on] = opcodes(spv)
            if args.keep:
                args.keep.mkdir(parents=True, exist_ok=True)
                (args.keep / spv.name).write_bytes(spv.read_bytes())
        off, on, buffers = ops["0"], ops["1"], ops["2"]
        # (robustBufferAccess2 off: the texel buffer's fetch checked too, by its size: one more size query than with it on)
        if buffers.count(OP_QUERY_SIZE) != on.count(OP_QUERY_SIZE) + 1:
            raise SystemExit("FAIL: with robustBufferAccess2 off the texel buffer's fetch is not checked")
        if off.count(OP_IS_NAN) != 3 or OP_U_GREATER_THAN in off or OP_QUERY_LEVELS in off or OP_QUERY_SIZE_LOD in off:
            raise SystemExit("FAIL: with the fixes off the shader is not what it was (OpIsNan alone, no bounds)")
        if on.count(OP_IS_NAN) or on.count(OP_U_GREATER_THAN) < 3:
            raise SystemExit("FAIL: with nan_bits a NaN test is still OpIsNan")
        # (6 fetches with a level: each its levels and its size at that level; the multisampled image: its size; the texel
        # buffer none (robustBufferAccess2 answers it); 3 gathers nudged by their size (2D, rect, array), the cube's not)
        if OP_QUERY_SIZE_LOD in off:
            raise SystemExit("FAIL: with the fixes off a gather is nudged")
        if on.count(OP_QUERY_LEVELS) != 6 or on.count(OP_QUERY_SIZE_LOD) != 6 + 3 or on.count(OP_QUERY_SIZE) != 1:
            raise SystemExit("FAIL: with fetch_bounds not every fetch is checked (levels %d, sizes %d + %d)" % (
                on.count(OP_QUERY_LEVELS), on.count(OP_QUERY_SIZE_LOD), on.count(OP_QUERY_SIZE)))
        if on.count(OP_QUERY_SAMPLES) != 1 or OP_QUERY_SAMPLES in off:
            raise SystemExit("FAIL: with fetch_bounds the multisampled fetch's sample is not checked")
        if on.count(OP_IMAGE_FETCH) != 8 or off.count(OP_IMAGE_FETCH) != 8:
            raise SystemExit("FAIL: fetches went missing")
    print("ok: fixes off: 3 OpIsNan, 8 plain fetches, 4 plain gathers; on: NaN tests on bits, 7 fetches checked (not the texel "
          "buffer), 3 gathers nudged (not the cube); both valid SPIR-V")


if __name__ == "__main__":
    main()
