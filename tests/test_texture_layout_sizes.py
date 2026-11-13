"""A texture's storage size covers what the renderer makes of it (host/nvn_layout.c, gpu/vk/vk_texture.cpp).

The width, height and level count of a texture come from the game, which reads them from its files. The layout did its
sums in 32 bits: a width of 0x40000001 RGBA8 texels came out as 512 bytes of storage, and the renderer then made an image
4 GB wide and buffers to match. A level count near 2^32 looped for minutes. The renderer also passed sizes past the GPU's
limits to vkCreateImage.

Builds host/nvn_layout.c as it is and checks the sizes; cuts texture_size_ok out of gpu/vk/vk_texture.cpp and checks it
against a GPU's limits. No GPU or game.
python -I -B tests/test_texture_layout_sizes.py [--cc gcc] [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


LAYOUT = r'''
#include "gpu/render/rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    nvn_texdesc d = {0};
    nvn_texlayout L;
    d.target = 1, d.height = 1, d.depth = 1, d.levels = 1;   /* (2D) */
    /* RGBA8 (4 bytes a texel) and RGBA32F (16): widths whose bytes wrapped past 2^32 */
    const struct { int format; uint32_t width, height, bpb; } wide[] = {
        {0x25, 0x40000001u, 1, 4}, {0x2E, 0x10000004u, 1, 16}, {0x25, 0x40000001u, 0x40000001u, 4}, {0x25, 0xFFFFFFFFu, 8, 4}};
    for (size_t i = 0; i < sizeof wide / sizeof *wide; i++) {
        d.format = wide[i].format, d.width = wide[i].width, d.height = wide[i].height;
        nvn_tex_layout(&d, &L);
        CHECK(L.bpb == wide[i].bpb);
        CHECK(L.size >= (uint64_t)wide[i].width * wide[i].bpb * (wide[i].height / 8 ? wide[i].height : 1));
    }
    /* a level count from the game: past 32 every level is 1x1, so the size is 32 levels' */
    d.format = 0x25, d.width = 256, d.height = 256, d.levels = 32;
    nvn_tex_layout(&d, &L);
    const uint64_t at32 = L.size;
    d.levels = 0xFFFFFFFFu;
    nvn_tex_layout(&d, &L);
    CHECK(L.size == at32 && nvn_tex_level_offset(&d, 0xFFFFFFFFu) == nvn_tex_level_offset(&d, 32));
    /* a linear texture's stride: whole rows, not a wrapped few bytes */
    d.flags = 0x10, d.levels = 1, d.width = 0x40000001u, d.stride = 0;
    CHECK(nvn_tex_linear_stride(&d) >= 0xFFFFFF00u);
    /* the ordinary sizes as before */
    d.flags = 0, d.width = 1920, d.height = 1080, d.levels = 1;
    nvn_tex_layout(&d, &L);
    CHECK(L.size == 8847360);
    printf("PASS: texture storage covers the game's sizes, and a level count cannot loop\n");
    return 0;
}
'''

LIMITS = r'''
#include <cstdint>
#include <cstdio>
enum VkImageType { VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D };
struct VkPhysicalDeviceLimits { uint32_t maxImageDimension1D, maxImageDimension2D, maxImageDimension3D, maxImageArrayLayers; };
struct { struct { VkPhysicalDeviceLimits limits; } props; } dev = {{{32768, 32768, 16384, 2048}}};
FN
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    CHECK(texture_size_ok(VK_IMAGE_TYPE_2D, 32768, 32768, 1, 2048, 1));
    CHECK(!texture_size_ok(VK_IMAGE_TYPE_2D, 0x40000001u, 1, 1, 1, 1));
    CHECK(!texture_size_ok(VK_IMAGE_TYPE_2D, 16, 16, 1, 2049, 1));
    CHECK(!texture_size_ok(VK_IMAGE_TYPE_3D, 16384, 16385, 4, 1, 1));
    CHECK(texture_size_ok(VK_IMAGE_TYPE_2D, 1920, 1080, 1, 1, 8));
    CHECK(!texture_size_ok(VK_IMAGE_TYPE_2D, 1920, 1080, 1, 1, 3) && !texture_size_ok(VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 64));
    std::printf("PASS: a texture past the GPU's image limits is not made\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    texture = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
    fn = block(texture, "bool texture_size_ok(")
    check_call = "if (!texture_size_ok(x.type, x.width, x.height, x.depth, x.layers, d.samples))"
    if check_call not in block(texture, "Texture* create("):
        raise SystemExit("FAIL: create() makes a texture without texture_size_ok")
    with tempfile.TemporaryDirectory(prefix="texture-sizes-") as tmp:
        tmp = Path(tmp)
        (tmp / "layout.c").write_text(LAYOUT, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-O1", "-Wall", "-I", str(ROOT), str(tmp / "layout.c"),
                        str(ROOT / "host" / "nvn_layout.c"), "-o", str(tmp / "layout")], check=True)
        subprocess.run([str(tmp / "layout")], check=True, timeout=60)
        (tmp / "limits.cpp").write_text(LIMITS.replace("FN", fn), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", str(tmp / "limits.cpp"), "-o", str(tmp / "limits")],
                       check=True)
        subprocess.run([str(tmp / "limits")], check=True)


if __name__ == "__main__":
    main()
