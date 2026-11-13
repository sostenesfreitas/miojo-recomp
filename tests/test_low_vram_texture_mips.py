"""Execute the sampled ASTC mip policy with real NVN/Vulkan types, without a GPU or game.

Catch missing low-budget trimming, ignored overrides, stale preset defaults and
on/off spellings that accidentally keep the automatic quality reduction enabled.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    p.add_argument("--headers", default=os.environ.get("HOOKSHOT_VULKAN_HEADERS"))
    p.add_argument("--sanitize", default="")
    a = p.parse_args()
    source = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8-sig")
    mip = block(source, "{   // huge textures leave out their top levels:")
    policy = block(source, "bool low_vram_texture_mips(") if "bool low_vram_texture_mips(" in source else ""
    helpers = "\n".join(block(source, marker) for marker in (
        "bool is_bc(VkFormat f)", "bool is_astc(VkFormat f)", "uint64_t texture_level_units("))
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vulkan/vulkan.h>
#define _Static_assert static_assert
#include "gpu/render/rx_nvn.h"
#include "gpu/vk/compat/hookshot_env.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while(0)
struct Texture {
    uint32_t width, height, depth = 1, levels, layers = 1, skip = 0;
    bool sparse = false;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageType type = VK_IMAGE_TYPE_2D;
};
struct { bool sparse_images = false; VkPhysicalDevice physical{}; } dev;
struct { int preset = 1, budget = 1; } opts;
constexpr int kAutoPreset = 6;   // (gpu/vk/vk_menu.h: the Quality preset Auto (this PC), its tier's budgets in budget)
uint64_t allowed;
bool query_ok;
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* = nullptr) {
    *used = 2000ull << 20; *budget = allowed; return query_ok;
}
void logf(const char*, ...) {}
@HELPERS@
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
        VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling,
        uint32_t* n, VkSparseImageFormatProperties*) { *n = 0; }
@POLICY@
void trim(Texture& x, const nvn_tex* t, bool astc = true, bool depth = false,
          int bpb = 4, int bw = 1, int bh = 1) {
    const nvn_texdesc& d = t->d;
    @MIP@
}
void check_image(const Texture& x, int skip, int width, int levels) {
    CHECK(x.skip == (uint32_t)skip);
    CHECK(x.width == (uint32_t)width && x.height == (uint32_t)width);
    CHECK(x.levels == (uint32_t)levels);
}
int main(int argc, char** argv) {
    CHECK(argc == 13);
    opts.preset = std::atoi(argv[1]);
    opts.budget = std::atoi(argv[12]);
    allowed = std::strtoull(argv[2], nullptr, 10) << 20;
    query_ok = std::atoi(argv[3]) != 0;
    nvn_tex t{}; t.d.samples = 1;
    Texture single{3072,3072,1,12}; trim(single,&t);
    check_image(single,std::atoi(argv[4]),std::atoi(argv[5]),std::atoi(argv[6]));
    Texture array{1365,1365,1,11,90}; trim(array,&t);
    check_image(array,std::atoi(argv[7]),std::atoi(argv[8]),std::atoi(argv[9]));
    CHECK(array.layers == 90);
    Texture ui{512,512,1,10}; trim(ui,&t); check_image(ui,0,512,10);
    Texture last_mip{3072,3072,1,1}; trim(last_mip,&t); check_image(last_mip,0,3072,1);
    Texture target{4096,4096,1,13}; trim(target,&t,false); check_image(target,0,4096,13);
    Texture depth{12288,12288,1,1}; trim(depth,&t,false,true,2); check_image(depth,0,12288,1);
    if (std::atoi(argv[10])) {
        // The budget is cached, but choosing Minimum in the menu must still
        // change defaults for textures made afterwards, and switching back too.
        opts.preset = 1, opts.budget = 1;
        Texture minimum{3072,3072,1,12}; trim(minimum,&t); check_image(minimum,2,768,10);
        opts.preset = 2, opts.budget = 2;
        Texture low{3072,3072,1,12}; trim(low,&t); check_image(low,0,3072,12);
    }
    std::printf("PASS %s\n",argv[11]);
}
'''.replace("@HELPERS@", helpers).replace("@POLICY@", policy).replace("@MIP@", mip)
    # The preset: a number, its budget the same (as a preset sets it); or (preset, budget), Auto (this PC) with its tier's budgets.
    # Literal outcomes: (skip, width, levels) for a 3072-square image and a
    # 1365-square, 90-layer image. No expected result calls the production policy.
    original = (0, 3072, 12, 0, 1365, 11)
    minimum = (2, 768, 10, 1, 682, 10)
    cases = [
        ("minimum-small-budget", 1, 3674, 1, {}, minimum, 0),
        ("budget-at-4.5-GiB", 1, 4608, 1, {}, minimum, 0),
        ("budget-above-4.5-GiB", 1, 4609, 1, {}, original, 0),
        ("large-budget", 1, 7500, 1, {}, original, 0),
        ("zero-budget", 1, 0, 1, {}, original, 0),
        ("failed-budget-query", 1, 3674, 0, {}, original, 0),
        ("custom-preset", 0, 3674, 1, {}, original, 0),
        ("auto-at-minimum-tier", (6, 1), 3674, 1, {}, minimum, 0),
        ("auto-at-minimum-tier-large-budget", (6, 1), 7500, 1, {}, original, 0),
        ("auto-at-balanced-tier", (6, 3), 3674, 1, {}, original, 0),
        ("auto-with-the-players-vram-budget", (6, 0), 3674, 1, {}, original, 0),
        ("low-preset-and-menu-changes", 2, 3674, 1, {}, original, 1),
        ("dimension-override", 1, 3674, 1, {"RX_VK_TEX_MAX_DIM": "2048"},
         (1, 1536, 11, 0, 1365, 11), 0),
        ("threshold-override", 1, 3674, 1, {"RX_VK_TEX_SKIP_MB": "64"},
         (0, 3072, 12, 1, 682, 10), 0),
        ("aggressive-overrides", 1, 3674, 1,
         {"RX_VK_TEX_MAX_DIM": "512", "RX_VK_TEX_SKIP_MB": "2"}, (3, 384, 9, 2, 341, 9), 0),
        ("overrides-on-large-budget", 1, 7500, 1,
         {"RX_VK_TEX_MAX_DIM": "512", "RX_VK_TEX_SKIP_MB": "2"}, (3, 384, 9, 2, 341, 9), 0),
        ("threshold-without-dimension-override", 2, 3674, 1,
         {"RX_VK_TEX_SKIP_MB": "2"}, (1, 1536, 11, 0, 1365, 11), 0),
    ]
    for value in ("0", "off", "false", "no", "OFF", "FaLsE", " no \t", ""):
        cases.append(("disabled-" + repr(value), 1, 3674, 1,
                      {"RX_VK_LOW_VRAM_MIPS": value}, original, 0))
    for value in ("1", "true", "on"):
        cases.append(("enabled-" + value, 1, 3674, 1,
                      {"RX_VK_LOW_VRAM_MIPS": value}, minimum, 0))
    with tempfile.TemporaryDirectory(prefix="low-vram-mips-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / ("fixture.exe" if os.name == "nt" else "fixture")
        cpp.write_text(fixture, encoding="utf-8")
        cmd = [a.cxx, "-std=c++20", "-O2", "-I", str(ROOT)]
        if a.headers:
            cmd += ["-I", a.headers]
        if a.sanitize:
            cmd += ["-fsanitize=" + a.sanitize, "-fno-omit-frame-pointer"]
        subprocess.run(cmd + [str(cpp), "-o", str(exe)], check=True)
        for name, preset, budget, query, overrides, expected, switch in cases:
            env = os.environ.copy()
            for key in ("RX_VK_LOW_VRAM_MIPS", "RX_VK_TEX_MAX_DIM", "RX_VK_TEX_MAX_MB", "RX_VK_TEX_SKIP_MB"):
                env.pop(key, None)
            env.update(overrides)
            preset, memory_budget = preset if isinstance(preset, tuple) else (preset, preset)
            args = [preset, budget, query, *expected, switch, name, memory_budget]
            subprocess.run([str(exe), *map(str, args)], env=env, check=True)
    print("PASS %d mip-policy configurations" % len(cases))


if __name__ == "__main__":
    main()
