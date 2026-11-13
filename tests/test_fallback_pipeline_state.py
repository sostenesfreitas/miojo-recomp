# SPDX-License-Identifier: GPL-3.0-or-later
"""Run build_graphics from vk_draw.cpp with pipeline creation intercepted, without a GPU.

The fallback vertex stage reads records by vertex index, without vertex bindings. It must
not require dynamic binding stride (VUID-vkCmdDrawIndirect-pStrides-04913), but still needs
dynamic topology and primitive restart. Ordinary vertex input keeps all three states;
native mesh pipelines use none. Check whole pipelines and vertex-input library construction
(the fallback library case is called directly here; runtime fallback programs do not use GPL).
Also replay the production pipeline-bind and vertex-cache decisions for ordinary, fallback,
ordinary draws with the same buffers. The Vulkan state lifetime is modeled, not driver-tested.

python -I -B tests/test_fallback_pipeline_state.py --cxx g++ --headers <Vulkan include>
--source <vk_draw.cpp> also runs the same checks against an unfixed or mutated source.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
ROOT = Path(__file__).resolve().parents[1]

HEAD = r"""
#define _CRT_SECURE_NO_WARNINGS
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "compat/hookshot_env.h"

// Only the collaborators outside pipeline-state construction are stood in.
namespace Shader {
struct Info {
    struct Loads {
        uint32_t mask = 0;
        bool Generic(uint32_t i) const { return (mask & (1u << i)) != 0; }
    } loads;
};
}
struct Stage {
    VkShaderStageFlagBits vk_stage;
    VkShaderModule module = VK_NULL_HANDLE;
    Shader::Info info{};
};
struct Program {
    uint64_t key = 1;
    bool mesh = false, fallback = false;
    std::vector<Stage> stages;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};
// Plain fields model the state consumed below; NVN's packed command ABI is not under test.
struct nvn_va_st { uint32_t fmt = 0, stream = 0, offset = 0; };
struct nvn_blend_st { uint32_t src_rgb = 0, dst_rgb = 0, eq_rgb = 0, src_a = 0, dst_a = 0, eq_a = 0; };
struct NvnState {
    nvn_va_st vattr[16]{};
    int nvattr = 0;
    struct Stream { uint32_t stride = 0, divisor = 0; } vstream[16];
    struct Buffer { uint64_t address = 0, size = 0; } vbuf[16];
    bool depth_clamp = false;
    struct { uint32_t mode = 2; } poly;
    struct { bool a2c = false; } ms;
    struct { uint32_t blend_en = 0; } color;
    nvn_blend_st blend[8]{};
};
struct Texture;
/* TARGETS */
struct {
    bool vertex_divisor = false, eds3_clamp = false, eds3_polygon = false;
    bool provoking_vertex = false, eds3_a2c = false, eds3_blend = false;
    bool task_mesh_fallback = false;
    VkPhysicalDeviceProperties props{};
    VkDriverId driver_id = VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
} dev;
void logf(const char*, ...) {}
void pin_subgroup(VkPipelineShaderStageCreateInfo&) {}
VkFormat vertex_format(int fmt) { return fmt == 1 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_UNDEFINED; }
VkFormat supported_vertex_format(VkFormat fmt) { return fmt; }
bool no_a2c() { return false; }
VkBlendFactor blend_factor(int) { return VK_BLEND_FACTOR_ONE; }
VkBlendOp blend_op(int) { return VK_BLEND_OP_ADD; }
uint32_t target_write_mask(const Program&, const NvnState&, int) { return 15; }
bool depth_aspect(VkFormat) { return false; }
bool has_stencil(VkFormat) { return false; }
bool gpl_libs_cached() { return false; }
bool driver_faults_on() { return false; }
namespace driver_faults {
struct Quarantine {
    enum { kReady, kBusy, kFaulted };
    int begin(uint64_t, bool) { return kReady; }
    bool end(uint64_t, bool) { return false; }
};
}
driver_faults::Quarantine& fault_quarantine() { static driver_faults::Quarantine q; return q; }
std::string driver_faults_path() { return {}; }
void driver_fault_note(uint64_t) {}
constexpr VkResult kDriverFault = static_cast<VkResult>(-77);
bool driver_cache_only = false;
VkPipelineCache pcache = VK_NULL_HANDLE, driver_ask_cache = VK_NULL_HANDLE;
uint64_t pcache_built_since_save = 0, st_pipelines = 0, st_pipeline_us = 0;

// The Windows-only optimizer workaround is not the behavior under test.
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*) {}

struct Observed {
    unsigned calls = 0;
    bool vertex_input = false, input_assembly = false;
    uint32_t bindings = 0, attributes = 0;
    VkVertexInputBindingDescription binding{};
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    std::vector<VkDynamicState> dynamic;
    std::vector<VkShaderStageFlagBits> stages;
} observed;
std::vector<std::pair<VkPipeline, bool>> stride_modes;
VkResult graphics_with(VkPipelineCache, const VkGraphicsPipelineCreateInfo* ci, VkPipeline* out) {
    ++observed.calls;
    observed.vertex_input = ci->pVertexInputState != nullptr;
    observed.input_assembly = ci->pInputAssemblyState != nullptr;
    if (const auto* vi = ci->pVertexInputState) {
        observed.bindings = vi->vertexBindingDescriptionCount;
        observed.attributes = vi->vertexAttributeDescriptionCount;
        if (observed.bindings) observed.binding = vi->pVertexBindingDescriptions[0];
    }
    if (ci->pInputAssemblyState) observed.topology = ci->pInputAssemblyState->topology;
    observed.dynamic.assign(ci->pDynamicState->pDynamicStates,
                            ci->pDynamicState->pDynamicStates + ci->pDynamicState->dynamicStateCount);
    for (uint32_t i = 0; i < ci->stageCount; ++i) observed.stages.push_back(ci->pStages[i].stage);
    *out = reinterpret_cast<VkPipeline>(uintptr_t{stride_modes.size() + 1});
    const bool static_stride = ci->pVertexInputState &&
        std::find(observed.dynamic.begin(), observed.dynamic.end(), VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) == observed.dynamic.end();
    stride_modes.emplace_back(*out, static_stride);
    return VK_SUCCESS;
}
/* VERTEX_INFO */
/* BUILD_GRAPHICS */
"""

CACHE = r"""
// Replay actual cache decisions; buffer resolution and Vulkan calls are authored stand-ins.
#define FINE_SCOPE(...)
#define FINE_N(...)
VkPipeline last_pipeline = VK_NULL_HANDLE;
/* LAST_VB */
LastVb last_vb{};
bool last_vb_valid = false;
/* VERTEX_SET */
VertexSet vertex_set{};
struct BufferRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};
/* KEPT_VERTEX */
KeptVertex kept_vb[16]{};
/* VERTEX_BIND_REUSE */
/* VERTEX_SET_SAME */
unsigned vertex_binds = 0, pipeline_binds = 0, buffer_lookups = 0, binding_errors = 0;
bool model_stride_valid = false;
VkBuffer zero_buffer = reinterpret_cast<VkBuffer>(uintptr_t{1});
uint64_t gpu_bind_stamp() { return 1; }
BufferRef gpu_buffer_bind(uint64_t addr, uint64_t, bool) {
    ++buffer_lookups;
    return {reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(addr)), 16};
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint point, VkPipeline pl) {
    ++pipeline_binds;
    if (point != VK_PIPELINE_BIND_POINT_GRAPHICS) ++binding_errors;
    const auto found = std::find_if(stride_modes.begin(), stride_modes.end(), [pl](const auto& item) { return item.first == pl; });
    if (found == stride_modes.end()) ++binding_errors;
    else if (found->second) model_stride_valid = false;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer, uint32_t first, uint32_t count,
    const VkBuffer* buffers, const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    ++vertex_binds;
    if (first != 0 || count != 16 || !buffers || !offsets || !sizes || !strides) ++binding_errors;
    else if (buffers[2] != reinterpret_cast<VkBuffer>(uintptr_t{0x1000}) ||
             offsets[2] != 16 || sizes[2] != 64 || strides[2] != 16) ++binding_errors;
    model_stride_valid = true;
}
void replay_vertex_binding(const Program& p, VkPipeline pl, const NvnState& s) {
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(uintptr_t{1});
    /* BIND_PIPELINE */
    if (p.mesh || p.fallback) return; // These draw paths return before ordinary vertex-buffer binding.
    /* BIND_VERTICES */
}
"""

MAIN = r"""
int failures = 0;
void check(bool ok, const char* kind, int part, const char* what) {
    if (!ok) {
        std::printf("FAIL %s part %d: %s\n", kind, part, what);
        ++failures;
    }
}
bool dynamic(VkDynamicState state) {
    return std::find(observed.dynamic.begin(), observed.dynamic.end(), state) != observed.dynamic.end();
}
int main() {
    NvnState state{};
    state.nvattr = 1;
    nvn_va_st attribute{};
    attribute.fmt = 1;
    attribute.stream = 2;
    std::memcpy(&state.vattr[0], &attribute, sizeof attribute);
    state.vstream[2].stride = 16;
    Targets targets{};
    dev.props.vendorID = 0x8086;
    VkPipeline pipelines[3]{};
    for (int kind = 0; kind < 3; ++kind) {
        const char* name = kind == 0 ? "ordinary" : kind == 1 ? "native mesh" : "fallback";
        Program p;
        p.mesh = kind == 1;
        p.fallback = kind == 2;
        if (kind == 1) {
            p.stages = {{VK_SHADER_STAGE_TASK_BIT_EXT}, {VK_SHADER_STAGE_MESH_BIT_EXT}, {VK_SHADER_STAGE_FRAGMENT_BIT}};
        } else {
            if (p.fallback) p.stages = {{VK_SHADER_STAGE_COMPUTE_BIT}, {VK_SHADER_STAGE_COMPUTE_BIT}};
            p.stages.push_back({VK_SHADER_STAGE_VERTEX_BIT});
            p.stages.back().info.loads.mask = 1;
            p.stages.push_back({VK_SHADER_STAGE_FRAGMENT_BIT});
        }
        for (int part : {-1, 0}) {
            if (p.mesh && part == 0) continue; // Native mesh has no vertex-input library.
            observed = {};
            const auto pipeline = build_graphics(p, state, targets, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, part, nullptr);
            if (part == -1) pipelines[kind] = pipeline;
            check(pipeline != VK_NULL_HANDLE && observed.calls == 1, name, part, "pipeline creation reached once");
            check(observed.vertex_input == !p.mesh, name, part, "vertex-input state presence");
            check(observed.input_assembly == !p.mesh, name, part, "input-assembly state presence");
            check(observed.bindings == (kind == 0 ? 1u : 0u), name, part, "vertex binding count");
            check(observed.attributes == (kind == 0 ? 1u : 0u), name, part, "vertex attribute count");
            if (kind == 0)
                check(observed.binding.binding == 2 && observed.binding.stride == 16, name, part, "ordinary vertex binding preserved");
            if (!p.mesh)
                check(observed.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, name, part, "input-assembly topology preserved");
            check(dynamic(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) == (kind == 0), name, part,
                  "dynamic stride only for ordinary vertex input");
            check(dynamic(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) == !p.mesh, name, part,
                  "dynamic topology retained for non-mesh graphics");
            check(dynamic(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE) == !p.mesh, name, part,
                  "dynamic primitive restart retained for non-mesh graphics");
            const std::vector<VkShaderStageFlagBits> expected = part == 0 ? std::vector<VkShaderStageFlagBits>{}
                : p.mesh ? std::vector<VkShaderStageFlagBits>{VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT,
                                                             VK_SHADER_STAGE_FRAGMENT_BIT}
                         : std::vector<VkShaderStageFlagBits>{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
            check(observed.stages == expected, name, part, "graphics stages preserved without fallback compute stages");
        }
    }
    state.vbuf[2].address = 0x1000;
    state.vbuf[2].size = 64;
    Program ordinary, fallback, native_mesh;
    fallback.fallback = true;
    native_mesh.mesh = true;
    replay_vertex_binding(ordinary, pipelines[0], state);
    check(vertex_binds == 1 && model_stride_valid, "sequence", -1, "first ordinary draw sets strides");
    replay_vertex_binding(fallback, pipelines[2], state);
    check(vertex_binds == 1 && !model_stride_valid, "sequence", -1, "fallback invalidates stride without a vertex bind");
    replay_vertex_binding(ordinary, pipelines[0], state);
    check(vertex_binds == 2 && model_stride_valid, "sequence", -1, "third draw rebinds identical buffers after fallback");
    check(buffer_lookups == 1, "sequence", -1, "resolved buffer reference remains cached");
    replay_vertex_binding(ordinary, pipelines[0], state);
    check(vertex_binds == 2 && pipeline_binds == 3, "sequence", -1, "repeated ordinary draw still skips redundant binds");
    replay_vertex_binding(native_mesh, pipelines[1], state);
    replay_vertex_binding(ordinary, pipelines[0], state);
    check(vertex_binds == 2 && model_stride_valid, "sequence", -1, "native mesh without vertex-input state preserves strides");
    check(binding_errors == 0, "sequence", -1, "intercepted bind arguments are intact");
    if (failures) return 1;
    std::puts("fallback pipeline state: ordinary, native mesh and fallback pass (5 pipeline creations)");
    std::puts("vertex cache sequence: ordinary/fallback/ordinary rebinds; ordinary/native reuse retained");
    return 0;
}
"""


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    fixture = HEAD.replace("/* TARGETS */", block(source, "struct Targets {") + ";")
    fixture = fixture.replace("/* VERTEX_INFO */", block(source, "const Shader::Info* vertex_info("))
    fixture = fixture.replace("/* BUILD_GRAPHICS */", block(source, "VkPipeline build_graphics(const Program& p, "
                              "const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part, bool* cold) {"))
    cache = CACHE
    for token, marker in [
            ("LAST_VB", "struct LastVb {"), ("VERTEX_SET", "struct VertexSet {"),
            ("KEPT_VERTEX", "struct KeptVertex {"),
            ("VERTEX_BIND_REUSE", "bool vertex_bind_reuse("),
            ("VERTEX_SET_SAME", "bool vertex_set_same("),
            ("BIND_PIPELINE", "if (pl != last_pipeline) {")]:
        extracted = block(source, marker) + (";" if marker.startswith("struct ") else "")
        cache = cache.replace("/* " + token + " */", extracted)
    start = source.index("    uint64_t vaddr[16]{};")
    end = source.index("    if (g_fine) fine_ns[FT_VERTEX]", start)
    cache = cache.replace("/* BIND_VERTICES */", source[start:end])
    with tempfile.TemporaryDirectory(prefix="hookshot-fallback-pipeline-") as temp:
        cpp, exe = Path(temp) / "pipeline.cpp", Path(temp) / "pipeline.exe"
        cpp.write_text(fixture + cache + MAIN, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", "-I", str(args.headers), "-I", str(ROOT / "gpu/vk"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
