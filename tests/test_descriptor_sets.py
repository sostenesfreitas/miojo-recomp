"""Exercise the real game-program layout creation with and without pooled descriptors.

No GPU: Vulkan layout calls record their flags, bindings and push-constant ranges.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--headers", required=True)
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    fixture = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "hookshot_env.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>
struct Descriptor { uint32_t count; };
struct Info {
    std::vector<Descriptor> constant_buffer_descriptors, storage_buffers_descriptors,
        texture_buffer_descriptors, image_buffer_descriptors, texture_descriptors, image_descriptors;
    bool uses_render_area = false, uses_rescaling_uniform = false, uses_global_memory = false;
};
struct Stage { VkShaderStageFlags vk_stage; Info info; };
struct Program {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    std::vector<Stage> stages;
    std::vector<VkDescriptorType> binding_types;
    VkShaderStageFlags push_stages = 0;
    bool push = false, uses_render_area = false, uses_push = false, fallback = false;
};
struct Device { VkDevice device{}; bool push_descriptor = true; uint32_t max_push_descriptors = 32; } dev;
bool vkCmdPushDescriptorSetKHR = true;
constexpr uint32_t kPushSize = 64;
unsigned calls = 0;
VkDescriptorSetLayoutCreateFlags flags;
VkResult vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* p,
                                    const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    ++calls; flags = p->flags;
    assert(p->bindingCount == 2);
    assert(p->pBindings[0].binding == 0 && p->pBindings[0].descriptorCount == 2);
    assert(p->pBindings[0].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    assert(p->pBindings[1].binding == 1 && p->pBindings[1].descriptorCount == 3);
    assert(p->pBindings[1].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    *out = (VkDescriptorSetLayout)(uintptr_t)1;
    return VK_SUCCESS;
}
VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* p,
                               const VkAllocationCallbacks*, VkPipelineLayout* out) {
    assert(p->setLayoutCount == 1 && *p->pSetLayouts == (VkDescriptorSetLayout)(uintptr_t)1);
    assert(p->pushConstantRangeCount == 1 && p->pPushConstantRanges->size == kPushSize);
    assert(p->pPushConstantRanges->stageFlags == VK_SHADER_STAGE_FRAGMENT_BIT);
    *out = (VkPipelineLayout)(uintptr_t)2;
    return VK_SUCCESS;
}
'''
    fixture += block(source, "void ensure_layout(")
    fixture += r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool expect_push = atoi(argv[1]) != 0;
    auto check = [](bool expected) {
        Program p;
        Stage s{}; s.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        s.info.constant_buffer_descriptors.push_back({2});
        s.info.texture_descriptors.push_back({3});
        s.info.uses_render_area = true;
        p.stages.push_back(s);
        ensure_layout(p);
        assert(p.push == expected);
        assert(bool(flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) == expected);
        assert(p.binding_types.size() == 2 && p.uses_push && p.uses_render_area);
        unsigned previous = calls;
        ensure_layout(p);
        assert(calls == previous);
    };
    check(expect_push);
    dev.max_push_descriptors = 5; check(expect_push);
    dev.max_push_descriptors = 4; check(false);
    dev.max_push_descriptors = 32;
    dev.push_descriptor = false; check(false);
    dev.push_descriptor = true;
    vkCmdPushDescriptorSetKHR = false; check(false);
}
'''
    with tempfile.TemporaryDirectory(prefix="descriptor-sets-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-I", args.headers, "-I", str(ROOT / "gpu/vk/compat"),
                        str(cpp), "-o", str(exe)], check=True)
        for value, expected in [(None, "1"), ("0", "1"), ("1", "0")]:
            env = os.environ.copy()
            env.pop("RX_VK_DESCRIPTOR_SETS", None)
            if value is not None:
                env["RX_VK_DESCRIPTOR_SETS"] = value
            subprocess.run([str(exe), expected], env=env, check=True)
    print("PASS: descriptor strategy preserves layout contents, limits and default behavior")


if __name__ == "__main__":
    main()
