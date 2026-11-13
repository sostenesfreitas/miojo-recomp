"""Opaque ASTC blocks from their headers (gpu/vk/astc_opaque.h): the textures kept as BC1 instead of BC3 (vk_texture.cpp).

Blocks are made bit by bit here: one partition with each colour endpoint mode (0-15; 4, 5, 10, 12 and 13 carry alpha),
partitions sharing a mode, partitions of different modes (maybe alpha: not read whole), void extents in LDR with alpha
0xFFxx and lower, an HDR void extent, and a block of zeros (no texels yet). Then the scan's counts, its stop at the first
block with alpha, and the choice for a whole upload (every block with texels opaque, and they are most of it). The texture
side: the encoder's mode follows the image (BC1 for a BC1 image), a BC1 image's views are BC1, and RX_VK_ASTC_BC1=0 is read.
python -I -B tests/test_astc_opaque.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include "astc_opaque.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace astc_opaque;
struct B { uint8_t b[16] = {}; void set(int at, int n, uint64_t v) { for (int i = 0; i < n; i++) if (v >> i & 1) b[(at + i) / 8] |= (uint8_t)(1u << ((at + i) % 8)); } };
static B one(uint32_t cem) { B x; x.set(0, 11, 0x042); x.set(11, 2, 0); x.set(13, 4, cem); x.b[15] = 0x5a; return x; }   // (a 2D block mode, one partition)
static B shared(uint32_t parts, uint32_t cem) { B x; x.set(0, 11, 0x042); x.set(11, 2, parts - 1); x.set(13, 10, 0x155); x.set(23, 2, 0); x.set(25, 4, cem); return x; }
static B mixed(uint32_t parts, uint32_t cls) { B x; x.set(0, 11, 0x042); x.set(11, 2, parts - 1); x.set(23, 2, cls); return x; }
static B void_extent(bool hdr, uint8_t a_hi, uint8_t a_lo) { B x; x.set(0, 9, 0x1FC); x.set(9, 1, hdr); x.set(10, 2, 3); for (int i = 12; i < 64; i++) x.set(i, 1, 1); x.b[14] = a_lo, x.b[15] = a_hi; return x; }
int main() {
    for (uint32_t cem = 0; cem < 16; cem++) {
        const bool alpha = cem == 4 || cem == 5 || cem == 10 || cem == 12 || cem == 13;
        CHECK(block(one(cem).b) == (alpha ? kAlpha : kOpaque));
        for (uint32_t p = 2; p <= 4; p++) CHECK(block(shared(p, cem).b) == (alpha ? kAlpha : kOpaque));
    }
    for (uint32_t p = 2; p <= 4; p++)
        for (uint32_t c = 1; c <= 3; c++) CHECK(block(mixed(p, c).b) == kAlpha);   /* (modes not read whole: BC3 stays) */
    CHECK(block(void_extent(false, 0xFF, 0xFF).b) == kOpaque);
    CHECK(block(void_extent(false, 0xFF, 0x10).b) == kOpaque);
    CHECK(block(void_extent(false, 0x80, 0xFF).b) == kAlpha);
    CHECK(block(void_extent(true, 0xFF, 0xFF).b) == kAlpha);    /* (HDR) */
    B zero;
    CHECK(block(zero.b) == kEmpty);

    uint8_t buf[16 * 8];
    const B o = one(8), a = one(12);
    for (int i = 0; i < 8; i++) std::memcpy(buf + 16 * i, o.b, 16);
    std::memcpy(buf + 16 * 6, zero.b, 16);
    Scan s = scan(buf, sizeof buf);
    CHECK(s.opaque == 7 && s.empty == 1 && s.alpha == 0 && whole_opaque(s));
    std::memcpy(buf + 16 * 2, a.b, 16);
    s = scan(buf, sizeof buf);
    CHECK(s.alpha == 1 && s.opaque == 2 && !whole_opaque(s));       /* (stops at the first block with alpha) */
    s = scan(buf, sizeof buf, false);
    CHECK(s.alpha == 1 && s.opaque == 6 && s.empty == 1);
    for (int i = 0; i < 8; i++) std::memcpy(buf + 16 * i, i < 3 ? o.b : zero.b, 16);
    CHECK(!whole_opaque(scan(buf, sizeof buf)));                    /* (mostly zeros: storage still being filled) */
    for (int i = 0; i < 8; i++) std::memcpy(buf + 16 * i, zero.b, 16);
    CHECK(!whole_opaque(scan(buf, sizeof buf)));                    /* (nothing written: not decided) */
    CHECK(scan(buf, 15).empty == 0);                                /* (a partial block left out) */
    std::printf("PASS: opaque ASTC blocks told from their headers, and the BC1 choice of a whole upload\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
    # the texture side, as written
    for needle in ('dst.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 0u : 1u',   # (the encoder's mode follows the image)
                   'else if (image == VK_FORMAT_BC1_RGBA_UNORM_BLOCK)',   # (a BC1 image's views are BC1)
                   'getenv("RX_VK_ASTC_BC1")',
                   'else if (all && astc_opaque::whole_opaque(sc) && bc_reformat(tex, VK_FORMAT_BC1_RGBA_UNORM_BLOCK)) tex.bc1 = 2;',
                   'return upload_astc_gpu(tex, total, nullptr);'):   # (back to BC3: uploaded whole)
        if needle not in source:
            raise SystemExit("vk_texture.cpp: missing %r" % needle)
    with tempfile.TemporaryDirectory(prefix="astc-opaque-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
