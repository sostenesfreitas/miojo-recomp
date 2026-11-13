"""The pipeline state list's version 2 records are the same on every GPU: the real code that maps them (vk_draw.cpp state_targets,
state_v2_resolve, state_v1_portable, color_types_of, pipeline_topology; vk_texture.cpp attachment_format and the format rules texture_get
and texture_view follow; vk_shader.cpp answers_portable / answers_here / portable_variant; vk_formats.cpp vk_format), cut out of the
sources and compiled with stubs for the device, checked on two made-up GPUs: one that samples ASTC and has mesh shaders, one that
decodes ASTC and runs mesh programs through the compute fallback.

- a target's format as a draw makes it (texture_get + texture_view) and as the prepare makes it from the record's NVN formats agree,
  for every NVN format, on both GPUs;
- a mesh program recorded on either GPU gives the same record (answers, variant, topology), and on the other GPU the answers land on
  the stages its own program has, and the variant found is that GPU's;
- a version 1 record made with the mesh fallback reads as the native one; a list of each version round-trips through the reader.
Synthetic data only. python -I -B tests/test_pipeline_state_portable.py --headers <Vulkan headers>
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, semicolon=False):
    start = text.index(marker)
    brace = text.index('{', start)
    end = brace + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + (';' if semicolon else '')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--headers', required=True, type=Path)
    args = parser.parse_args()
    draw = (ROOT / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    tex = (ROOT / 'gpu/vk/vk_texture.cpp').read_text(encoding='utf-8')
    fmts = (ROOT / 'gpu/vk/vk_formats.cpp').read_text(encoding='utf-8')
    shader = (ROOT / 'gpu/vk/vk_shader.cpp').read_text(encoding='utf-8')
    # vk_formats.cpp: the ASTC tables, vk_format (astc_native: the made-up device's answer below)
    astc_tables = block(fmts, 'namespace {\nconstexpr int kAstcBw', True)
    formats = '\n'.join([astc_tables, block(fmts, 'VkFormatInfo vk_format(int nvn_format)'),
                         block(fmts, 'bool is_depth_format(VkFormat f)'), block(fmts, 'bool has_stencil(VkFormat f)')])
    texture = '\n'.join(block(tex, m) for m in (
        'bool is_bc(VkFormat f)', 'bool is_astc(VkFormat f)', 'VkFormat linear_twin(VkFormat f)', 'VkFormat widened(VkFormat f, int* from_bpb)',
        'VkFormat storage_view_format(VkFormat f)', 'VkFormat image_format_of(', 'VkFormat view_format_of(', 'VkFormat attachment_format('))
    shaders = '\n'.join(block(shader, m) for m in (
        'uint64_t answers_hash(const Program& p)', 'uint64_t answers_hash_of('))
    shaders += '\nnamespace {\n' + block(shader, 'size_t generated_stage(const Program& p)') + '\n}\n'
    shaders += '\n'.join(block(shader, m) for m in (
        'std::vector<Program::Answer> answers_portable(const Program& p)', 'std::vector<Program::Answer> answers_here(',
        'uint64_t portable_variant(const Program& p)'))
    targets = block(draw, 'bool targets_ok(') + '\n' + block(draw, 'bool formats_ok(')
    states = '\n'.join([
        block(draw, 'struct Targets {', True), block(draw, 'void color_types_of('),
        'constexpr VkPrimitiveTopology kMeshDrawTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;',
        block(draw, 'VkPrimitiveTopology pipeline_topology('), targets,
        block(draw, 'constexpr char kStateMagic[8]', True), block(draw, 'constexpr char kStateMagic2[8]', True),
        block(draw, 'struct StateRec {', True), block(draw, 'struct StateRec2 {', True),
        block(draw, 'Targets state_targets('), block(draw, 'bool state_v1_portable('), block(draw, 'bool state_v2_resolve('),
        block(draw, 'std::vector<StateRec> state_list_read(')])
    fixture = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "nvn_api.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
// the made-up GPUs: A samples ASTC and has mesh shaders; B decodes ASTC and runs mesh programs as the compute fallback
bool gpu_b = false;
struct { void* physical = (void*)1; } dev;
static void vkGetPhysicalDeviceFormatProperties(void*, VkFormat f, VkFormatProperties* p) {
    *p = {};
    const bool astc = f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    if (astc) { if (!gpu_b) p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT; return; }
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                               VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}
static VkFormatProperties format_props(VkFormat f) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(nullptr, f, &p); return p; }
struct VkFormatInfo { VkFormat format; bool astc; int block_w, block_h; };
namespace { bool astc_native() { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(nullptr, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &p);
    return p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT; } }
enum class ViewUse { Sample, Attachment, Storage };
struct Texture { int nvn_format = 0; };
struct Stage { VkShaderStageFlagBits vk_stage; };
struct Program {
    bool mesh = false, fallback = false;
    VkPrimitiveTopology fallback_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    std::vector<Stage> stages;
    struct Answer { uint16_t stage, desc; uint8_t type; bool image = false; };
    std::vector<Answer> answers;
    uint64_t variant = 0;
};
namespace Shader { enum class AttributeType : uint8_t { Float, SignedInt, UnsignedInt, Disabled }; }
''' + formats + '\n' + texture + '\n' + shaders + '\n' + states + r'''
// (what a draw makes of a target: texture_get's image (create: image_format_of) and texture_view's view)
static VkFormat drawn(int tex_nvn, int view_nvn) {
    const VkFormatInfo fi = vk_format(tex_nvn);
    if (fi.format == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;
    int from = 0;
    const VkFormat image = image_format_of(widened(fi.format, &from), fi.astc);
    return view_format_of(image, fi.astc, false, view_nvn, ViewUse::Attachment, VK_FORMAT_UNDEFINED);
}
// a task + mesh program as each GPU has it: A (task, mesh, fragment), B (task, mesh, generated vertex, fragment)
static Program mesh_program(bool fallback) {
    Program p;
    p.mesh = !fallback, p.fallback = fallback, p.fallback_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    p.stages = fallback ? std::vector<Stage>{{VK_SHADER_STAGE_COMPUTE_BIT}, {VK_SHADER_STAGE_COMPUTE_BIT}, {VK_SHADER_STAGE_VERTEX_BIT},
                                             {VK_SHADER_STAGE_FRAGMENT_BIT}}
                        : std::vector<Stage>{{VK_SHADER_STAGE_TASK_BIT_EXT}, {VK_SHADER_STAGE_MESH_BIT_EXT}, {VK_SHADER_STAGE_FRAGMENT_BIT}};
    const uint16_t frag = fallback ? 3 : 2;
    p.answers = {{1, 0, 1}, {frag, 0, 2}, {frag, 1, 0x81}, {frag, 2, 0, true}};
    p.variant = answers_hash(p);
    return p;
}
static uint32_t pack(const Program::Answer& a) { return (uint32_t)a.stage << 28 | (uint32_t)a.desc << 12 | (a.image ? 1u : 0u) << 8 | a.type; }
// a version 2 record of a draw into an R11G11B10F target and an ASTC 4x4 one (no GPU renders into that), D24S8 depth
static StateRec2 record2(const Program& p) {
    StateRec2 r{};
    r.code[0] = 0x11, r.code[1] = 0x22, r.code[2] = 0x33;
    r.mesh = 1, r.topo = kMeshDrawTopology, r.n = 2, r.samples = 1, r.has_depth = 1, r.depth_nvn = NVN_FORMAT_DEPTH24_STENCIL8;
    r.color_nvn[0] = r.view_nvn[0] = NVN_FORMAT_R11G11B10F;
    r.color_nvn[1] = r.view_nvn[1] = 0x79;
    const std::vector<Program::Answer> a = answers_portable(p);
    r.nanswers = (uint32_t)a.size();
    for (size_t i = 0; i < a.size(); i++) r.answers[i] = pack(a[i]);
    r.variant = portable_variant(p);
    return r;
}
int main(int argc, char** argv) {
    // 1. a target's format: the draw's and the prepare's, every NVN format and an sRGB view of an RGBA8 texture, both GPUs
    for (int b = 0; b < 2; b++) {
        gpu_b = b;
        for (int f = 0; f < 0x100; f++) CHECK(drawn(f, f) == attachment_format(f, f));
        CHECK(attachment_format(NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8_SRGB) == VK_FORMAT_R8G8B8A8_SRGB);
        CHECK(attachment_format(NVN_FORMAT_RGB8, NVN_FORMAT_RGB8) == VK_FORMAT_R8G8B8A8_UNORM);   // (widened)
        CHECK(attachment_format(NVN_FORMAT_DEPTH24_STENCIL8, NVN_FORMAT_DEPTH24_STENCIL8) == VK_FORMAT_D24_UNORM_S8_UINT);
    }
    // ASTC: A keeps it as ASTC (no target), B decodes it to RGBA8 (a target)
    gpu_b = false; CHECK(attachment_format(0x79, 0x79) == VK_FORMAT_UNDEFINED);
    gpu_b = true; CHECK(attachment_format(0x79, 0x79) == VK_FORMAT_R8G8B8A8_UNORM);
    // 2. the mesh program: one record whichever GPU made it
    const Program a = mesh_program(false), b = mesh_program(true);
    CHECK(a.variant != b.variant);   // (each GPU's own variants differ: the stages are numbered differently)
    CHECK(portable_variant(a) == portable_variant(b) && portable_variant(a) == a.variant);
    const StateRec2 ra = record2(a), rb = record2(b);
    CHECK(std::memcmp(&ra, &rb, sizeof ra) == 0);
    // and back: the answers land on each GPU's own stages
    for (const Program* p : {&a, &b}) {
        const std::vector<Program::Answer> here = answers_here(*p, answers_portable(*p));
        CHECK(here.size() == p->answers.size());
        for (size_t i = 0; i < here.size(); i++) CHECK(here[i].stage == p->answers[i].stage && here[i].desc == p->answers[i].desc);
        CHECK(answers_hash_of(here) == p->variant);
    }
    // the pipeline's topology: the mesh draw's on A, the fallback's points on B
    CHECK(pipeline_topology(a, (VkPrimitiveTopology)ra.topo) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    CHECK(pipeline_topology(b, (VkPrimitiveTopology)ra.topo) == VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    // resolved: A has no ASTC target (left out, as its draw leaves it), B renders it as RGBA8; the colour types follow
    for (int g = 0; g < 2; g++) {
        gpu_b = g;
        StateRec r{};
        CHECK(state_v2_resolve(ra, r));
        CHECK(r.color_format[0] == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
        CHECK(r.color_format[1] == (g ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED));
        CHECK(r.depth_format == VK_FORMAT_D24_UNORM_S8_UINT && r.has_depth == 1 && r.n == 2);
        CHECK(r.variant == ra.variant && r.mesh == 1 && r.nanswers == 4);
        CHECK(r.color_types[0] == (uint8_t)Shader::AttributeType::Float);
    }
    // an integer target: its colour type
    { StateRec2 v = ra; v.n = 1; v.color_nvn[0] = v.view_nvn[0] = NVN_FORMAT_RGBA8UI; StateRec r{};
      CHECK(state_v2_resolve(v, r) && r.color_types[0] == (uint8_t)Shader::AttributeType::UnsignedInt); }
    // counts a record cannot have
    { StateRec2 v = ra; v.nanswers = 25; StateRec r; CHECK(!state_v2_resolve(v, r)); v = ra; v.n = 9; CHECK(!state_v2_resolve(v, r)); }
    // 3. a version 1 record made on B (the fallback's numbering and topology) reads as A's
    StateRec v1{};
    v1.code[0] = 0x11, v1.code[1] = 0x22, v1.code[2] = 0x33, v1.mesh = 1, v1.topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST, v1.samples = 1;
    v1.variant = b.variant, v1.nanswers = 4;
    for (size_t i = 0; i < 4; i++) v1.answers[i] = pack(b.answers[i]);
    StateRec native = v1;
    native.topo = kMeshDrawTopology, native.variant = a.variant;
    for (size_t i = 0; i < 4; i++) native.answers[i] = pack(a.answers[i]);
    { StateRec r = v1; CHECK(state_v1_portable(r) && std::memcmp(&r, &native, sizeof r) == 0); }
    { StateRec r = native; CHECK(state_v1_portable(r) && std::memcmp(&r, &native, sizeof r) == 0); }   // (A's: as it is)
    { StateRec r = v1; r.nanswers = 0; CHECK(!state_v1_portable(r)); }   // (its answers left out: cannot be told)
    // 4. files of each version through the reader
    const std::string dir = argv[1];
    {
        FILE* f = std::fopen((dir + "/v2.bin").c_str(), "wb");
        const uint32_t size = sizeof(StateRec2);
        std::fwrite(kStateMagic2, 8, 1, f), std::fwrite(&size, 4, 1, f), std::fwrite(&ra, sizeof ra, 1, f);
        StateRec2 bad = ra; bad.nquestions = 99; std::fwrite(&bad, sizeof bad, 1, f);
        std::fclose(f);
        int version = 0;
        gpu_b = true;
        const std::vector<StateRec> got = state_list_read(dir + "/v2.bin", &version);
        CHECK(version == 2 && got.size() == 1 && got[0].color_format[1] == VK_FORMAT_R8G8B8A8_UNORM);
    }
    {
        FILE* f = std::fopen((dir + "/v1.bin").c_str(), "wb");
        const uint32_t size = sizeof(StateRec);
        std::fwrite(kStateMagic, 8, 1, f), std::fwrite(&size, 4, 1, f), std::fwrite(&v1, sizeof v1, 1, f);
        std::fclose(f);
        int version = 0;
        const std::vector<StateRec> got = state_list_read(dir + "/v1.bin", &version);
        CHECK(version == 1 && got.size() == 1 && std::memcmp(&got[0], &native, sizeof native) == 0);
    }
    std::printf("PASS: targets mapped as the draw maps them on both GPUs; mesh records the same from native and fallback; v1 and v2 read\n");
    (void)argc;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='state-portable-') as tmp:
        folder = Path(tmp)
        source, exe = folder / 'fixture.cpp', folder / 'fixture.exe'
        source.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-function', '-Werror', '-D_CRT_SECURE_NO_WARNINGS', '-I', str(args.headers),
                        '-I', str(ROOT / 'gpu/render'), str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(folder)], check=True)


if __name__ == '__main__':
    main()
