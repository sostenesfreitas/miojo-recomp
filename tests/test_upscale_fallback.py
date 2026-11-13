"""Exercise the actual generic upscale fallback without Vulkan or a GPU.

python tests/test_upscale_fallback.py [--source gpu/vk/vk_draw.cpp] [--cxx clang++]
Use --cxx g++ on Linux, or --cxx cl in a configured Visual Studio compiler shell. No SDK needed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'gpu/vk/vk_draw.cpp').is_file())

PREFIX = r'''
#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Only the Vulkan declarations used by upscale_stub; no Vulkan SDK or runtime.
using VkCommandBuffer = uintptr_t;
using VkImage = uintptr_t;
using VkImageView = uintptr_t;
using VkPhysicalDevice = uintptr_t;
using VkFormat = uint32_t;
using VkFormatFeatureFlags = uint32_t;
using VkImageAspectFlags = uint32_t;
using VkImageType = uint32_t;
using VkImageViewType = uint32_t;
constexpr uint32_t VK_IMAGE_VIEW_TYPE_2D = 1;
using VkSampleCountFlagBits = uint32_t;
using VkImageLayout = uint32_t;
using VkFilter = uint32_t;
constexpr uint32_t VK_FORMAT_UNDEFINED = 0, VK_FORMAT_B10G11R11_UFLOAT_PACK32 = 122,
                   VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 = 123, VK_FORMAT_R8G8B8A8_UNORM = 37;
constexpr uint32_t VK_IMAGE_TYPE_2D = 1, VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_SAMPLE_COUNT_1_BIT = 1,
                   VK_IMAGE_LAYOUT_GENERAL = 1, VK_FILTER_LINEAR = 1;
constexpr uint32_t VK_FORMAT_FEATURE_BLIT_SRC_BIT = 1, VK_FORMAT_FEATURE_BLIT_DST_BIT = 2,
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT = 4,
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT = 8, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT = 16;
struct VkFormatProperties {
    VkFormatFeatureFlags optimalTilingFeatures{};
};
struct Subresource {
    uint32_t aspectMask{}, mipLevel{}, baseArrayLayer{}, layerCount{};
};
struct Offset {
    int32_t x{}, y{}, z{};
};
struct VkImageBlit {
    Subresource srcSubresource;
    Offset srcOffsets[2];
    Subresource dstSubresource;
    Offset dstOffsets[2];
};
constexpr int NVN_FORMAT_RGB9E5F = 64, NVN_FORMAT_R11G11B10F = 63;
constexpr unsigned NVN_TEXTURE_SWIZZLE_R = 2, NVN_TEXTURE_SWIZZLE_G = 3, NVN_TEXTURE_SWIZZLE_B = 4;
struct Desc {
    int target = 2, format = 64;
    unsigned width = 64, height = 32, depth = 1;
    unsigned swizzle_r = 2, swizzle_g = 3, swizzle_b = 4;
};
struct nvn_tex {
    Desc d;
    uint64_t gpu = 10, serial = 99;
};
struct nvn_view {};
struct NvnState {
    uint64_t tex[6][128]{}, img[6][32]{};
};
struct Texture {
    VkImage image = (VkImage)11;
    VkFormat format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    unsigned width = 64, height = 32, depth = 1, layers = 1, levels = 1, skip = 0;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    bool bc = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};
Texture src, dst;
nvn_tex source, destination;
bool override_view = false, movie = false, pipeline_ok = true;
bool missing_storage = false, missing_sample = false, missing_linear = false;
bool missing_input_view = false, missing_output_view = false;
unsigned blits = 0, typed_calls = 0, views = 0, movie_calls = 0, writes = 0, gameplay_logs = 0, gameplay_warnings = 0;
struct {
    VkPhysicalDevice physical{};
} dev;
void cs_slot_counts(const NvnState &, uint32_t *t, uint32_t *i) { *t = *i = 1; }
bool texture_of(int h, nvn_tex *t, nvn_view *v, bool *hv) {
    assert(h == 1 || h == 2);
    *t = h == 1 ? source : destination;
    *v = {};
    *hv = override_view;
    return true;
}
Texture *texture_get(const nvn_tex *t, bool) { return t->gpu == 10 ? &src : &dst; }
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat format, VkFormatProperties *p) {
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (missing_storage && format == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
        p->optimalTilingFeatures &= ~VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (missing_sample && format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
        p->optimalTilingFeatures &= ~VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (missing_linear && format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
        p->optimalTilingFeatures &= ~VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}
void end_pass(VkCommandBuffer) {}
// (the temporal upscaler, off here: upscale_stub offers it the frame first, vk_draw.cpp temporal_rejected_network)
bool temporal_wanted() { return false; }
bool temporal_off() { return false; }
namespace fsr3_inputs { bool wanted() { return false; } }   // (FSR 3.1, off here too)
static bool temporal_rejected_network(VkCommandBuffer, uint64_t, Texture *, const nvn_tex *, Texture *, const nvn_tex *) {
    return false;
}
void full_barrier(VkCommandBuffer) {}
bool dump_on() { return false; }
void logf(const char *format, ...) {
    if (std::strstr(format, "gameplay RGB9E5 upscale uses the typed fallback"))
        gameplay_logs++;
    if (std::strstr(format, "gameplay RGB9E5 upscale unavailable"))
        gameplay_warnings++;
}
void texture_written(Texture *t) {
    assert(t == &dst);
    writes++;
}
void dump_image(VkCommandBuffer, Texture *, unsigned, unsigned, const char *) {}
bool movie_upscale(VkCommandBuffer, uint64_t, uint64_t, Texture *) {
    movie_calls++;
    return movie;
}
bool gameplay_network(uint64_t key) {
    return key == 0xfd029a317a9a4b94ull || key == 0x9dc98871eb7fc101ull;
}
enum class ViewUse { Sample, Storage };
VkImageView texture_view(Texture *t, const nvn_tex *, const nvn_view *v, VkImageViewType type,
                         ViewUse use, VkFormat *f, VkFormat forced = VK_FORMAT_UNDEFINED) {
    assert(!v && type == VK_IMAGE_VIEW_TYPE_2D);
    views++;
    if (t == &src) {
        assert(use == ViewUse::Sample && forced == VK_FORMAT_UNDEFINED);
        *f = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        return missing_input_view ? 0 : (VkImageView)101;
    }
    assert(t == &dst && use == ViewUse::Storage && forced == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    *f = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    return missing_output_view ? 0 : (VkImageView)102;
}
static bool typed_upscale(VkCommandBuffer, VkImageView input, VkImageView output, Texture *t,
                          uint32_t level, uint64_t, const char *, bool temporal) {
    assert(input == (VkImageView)101 && output == (VkImageView)102 && t == &dst && level == 0 &&
           !temporal);
    typed_calls++;
    return pipeline_ok;
}
void vkCmdBlitImage(VkCommandBuffer, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl,
                    uint32_t n, const VkImageBlit *r, VkFilter filter) {
    assert(a == src.image && b == dst.image && al == VK_IMAGE_LAYOUT_GENERAL &&
           bl == VK_IMAGE_LAYOUT_GENERAL && n == 1 && filter == VK_FILTER_LINEAR);
    assert(r->srcSubresource.mipLevel == 0 && r->srcSubresource.baseArrayLayer == 0 &&
           r->dstSubresource.mipLevel == 0 && r->dstSubresource.baseArrayLayer == 0);
    assert(r->srcOffsets[1].x == 64 && r->srcOffsets[1].y == 32 && r->dstOffsets[1].x == 128 &&
           r->dstOffsets[1].y == 64);
    blits++;
}
'''
SUFFIX = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    uint64_t key = 0x0775215f8da720e6ull;
    destination.gpu = 20;
    destination.d.format = 63;
    destination.d.width = 128;
    destination.d.height = 64;
    dst.width = 128;
    dst.height = 64;
    dst.image = (VkImage)22;
    if (mode == "r11" || mode == "fd-r11")
        source.d.format = 63;
    if (mode == "movie") {
        dst.format = VK_FORMAT_R8G8B8A8_UNORM;
        movie = true;
    }
    if (mode == "view" || mode == "fd-view")
        override_view = true;
    if (mode == "skip")
        src.skip = 1;
    if (mode == "mip")
        src.levels = 2;
    if (mode == "swizzle")
        source.d.swizzle_r = 4;
    if (mode.compare(0, 2, "fd") == 0)
        key = 0xfd029a317a9a4b94ull;
    if (mode == "9dc")
        key = 0x9dc98871eb7fc101ull;
    if (mode == "pipeline-failure" || mode == "fd-pipeline-failure")
        pipeline_ok = false;
    if (mode == "no-storage" || mode == "fd-no-storage")
        missing_storage = true;
    if (mode == "no-e5-sample" || mode == "fd-no-e5-sample")
        missing_sample = true;
    if (mode == "no-e5-linear" || mode == "fd-no-e5-linear")
        missing_linear = true;
    if (mode == "null-input-view" || mode == "fd-null-input-view")
        missing_input_view = true;
    if (mode == "null-output-view" || mode == "fd-null-output-view")
        missing_output_view = true;
    NvnState state;
    state.tex[5][0] = 1;
    state.img[5][0] = 2;
    assert(upscale_stub((VkCommandBuffer)5, state, key));
    if (mode == "captured" || mode == "fd") {
        if (typed_calls != 1 || views != 2 || blits != 0) {
            puts("RED: RGB9E5 input still used physical-format blit");
            return 7;
        }
        assert(writes == 1);
        assert(gameplay_logs == (mode == "fd" ? 1u : 0u) && gameplay_warnings == 0);
    } else if (mode == "movie") {
        assert(movie_calls == 1 && typed_calls == 0 && blits == 0 && writes == 1);
    } else if (mode == "pipeline-failure" || mode == "fd-pipeline-failure") {
        assert(typed_calls == 1 && views == 2 && blits == 1 && writes == 1);
    } else if (missing_input_view || missing_output_view) {
        assert(typed_calls == 0 && views == 2 && blits == 1 && writes == 1);
    } else {
        assert(typed_calls == 0 && views == 0 && blits == 1 && writes == 1);
    }
    if (mode == "fd-view" || mode == "fd-pipeline-failure" || mode == "fd-no-storage" ||
        mode == "fd-no-e5-sample" || mode == "fd-no-e5-linear" ||
        mode == "fd-null-input-view" || mode == "fd-null-output-view")
        assert(gameplay_warnings == 1 && gameplay_logs == 0);
    if (mode == "fd-r11" || mode == "9dc")
        assert(gameplay_warnings == 0 && gameplay_logs == 0);
    printf("PASS: %s\n", mode.c_str());
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_draw.cpp')
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    body = block(args.source.read_text(encoding='utf-8'), 'bool upscale_stub(')
    env = os.environ.copy()
    env.pop('RX_VK_NO_UPSCALE_STUB', None)
    with tempfile.TemporaryDirectory(prefix='upscale-fallback-') as tmp:
        tmp = Path(tmp)
        source = tmp / 'check.cpp'
        exe = tmp / ('check.exe' if os.name == 'nt' else 'check')
        source.write_text(PREFIX + body + SUFFIX, encoding='utf-8')
        if Path(args.cxx).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'):
            command = [args.cxx, '/nologo', '/std:c++20', "/FI" + ENV_ON_H, '/EHsc', '/UNDEBUG', str(source),
                       '/Fo' + str(tmp / 'check.obj'), '/Fe' + str(exe)]
            cwd = tmp
        else:
            command = [args.cxx, '-std=c++20', '-include', ENV_ON_H, '-UNDEBUG', str(source), '-o', str(exe)]
            cwd = None
        subprocess.run(command, cwd=cwd, env=env, check=True)
        cases = ('captured', 'r11', 'movie', 'view', 'skip', 'mip', 'swizzle', 'fd', 'fd-r11',
                 'fd-view', 'fd-pipeline-failure', 'fd-no-storage', 'fd-no-e5-sample',
                 'fd-no-e5-linear', 'fd-null-input-view', 'fd-null-output-view', '9dc',
                 'pipeline-failure', 'no-storage', 'no-e5-sample', 'no-e5-linear',
                 'null-input-view', 'null-output-view')
        for case in cases:
            subprocess.run([str(exe), case], env=env, check=True)

if __name__ == '__main__':
    main()
