"""Native ASTC and BC texture accounting: whole blocks, including part blocks and 1x1 mip levels.

The original ASTC bug counted 16-byte blocks as 16-byte texels. BC1 and BC4 still counted one byte per texel instead
of eight bytes per 4x4 block, and the pre-creation estimate rounded bytes per texel before multiplying. Extract the
actual helper, estimate and final accounting from create, compile them, and compare with independent block arithmetic.
All 14 ASTC block sizes and all 16 Vulkan BC formats are covered, plus decoded ASTC, re-encoded BC3, uncompressed and
widened images, layers, depth, samples, sparse accounting and the allocator's floor. No GPU or game is involved.

python -I -B tests/test_native_astc_texture_count.py [--cxx g++] [--source gpu/vk/vk_texture.cpp]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#define _CRT_SECURE_NO_WARNINGS   // (the Windows CRT marks scanf deprecated: an error under -Werror)
#include <algorithm>
#include <cstdint>
#include <cstdio>

/* ACTUAL_HELPER */

int main() {
    unsigned w, h, d, bw, bh;
    while (std::scanf("%u %u %u %u %u", &w, &h, &d, &bw, &bh) == 5)
        std::printf("%llu\n", (unsigned long long)texture_level_units(w, h, d, bw, bh));
    return 0;
}
"""

ACCOUNTING_FIXTURE = r"""
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cstdint>
#include <cstdio>
enum VkFormat : int {
    VK_FORMAT_BC1_RGB_UNORM_BLOCK = 131, VK_FORMAT_BC3_UNORM_BLOCK = 137, VK_FORMAT_BC7_SRGB_BLOCK = 146,
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK = 157, VK_FORMAT_ASTC_12x12_SRGB_BLOCK = 184
};
/* HELPERS */
struct Desc { int samples; };
struct Image {
    uint32_t width, height, depth, levels, layers;
    VkFormat format;
    bool bc, sparse;
    int samples;
    uint64_t allocation, bytes = 0, pad = 0;
};
uint64_t allocation_size(uint64_t allocation) { return allocation; }
bool env_on(const char*) { return false; }
uint64_t total_bytes;
uint64_t estimate(Image& x, const Desc& d, bool astc, int bw, int bh, int bpb, uint32_t skip) {
    (void)bw; (void)bh;
    /* ESTIMATE */
    return bytes_at(skip);
}
uint64_t count(Image& x, bool astc, int bw, int bh, int bpb, int from) {
    /* COUNT */
    return x.bytes + x.pad;
}
int main() {
    unsigned w, h, z, levels, layers, format, astc, bw, bh, bpb, skip, bc, from, samples, sparse;
    unsigned long long allocation;
    while (std::scanf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %llu",
                      &w, &h, &z, &levels, &layers, &format, &astc, &bw, &bh, &bpb, &skip,
                      &bc, &from, &samples, &sparse, &allocation) == 16) {
        Image x{w, h, z, levels, layers, (VkFormat)format, false, false, (int)samples, allocation};
        const uint64_t e = estimate(x, Desc{(int)samples}, astc != 0, bw, bh, bpb, skip);
        Image made{std::max(1u, w >> skip), std::max(1u, h >> skip), std::max(1u, z >> skip), levels - skip, layers,
                   bc ? VK_FORMAT_BC3_UNORM_BLOCK : (VkFormat)format, bc != 0, sparse != 0, (int)samples, allocation};
        const uint64_t c = count(made, astc != 0, bw, bh, bpb, from);
        std::printf("%llu %llu %llu\n", (unsigned long long)e, (unsigned long long)made.bytes, (unsigned long long)c);
    }
}
"""

# (ASTC block sizes: NVN 0x79.. as gpu/vk/vk_formats.cpp has them)
ASTC = [(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12)]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def expected(w, h, d, bw, bh):
    w, h, d = max(1, w), max(1, h), max(1, d)
    return -(-w // bw) * -(-h // bh) * d


def compile_run(cxx, temp, name, text, rows):
    src, exe = Path(temp) / (name + ".cpp"), Path(temp) / (name + ".exe")
    src.write_text(text, encoding="utf-8")
    subprocess.run([cxx, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    return subprocess.run([str(exe)], input="".join(" ".join(map(str, c)) + "\n" for c in rows),
                          capture_output=True, text=True, check=True).stdout.splitlines()


def accounting_checks(source, create, helper, cxx, temp):
    # Keep the production calculations intact, including the allocator floor. The anchors also work on the base,
    # so --source can demonstrate numeric failures rather than just a missing-string failure.
    at = create.index("\n", create.index("static const uint64_t max_bytes")) + 1
    lam = create.index("auto bytes_at = [&](uint32_t skip)", at)
    estimate = create[at:lam] + block(create[lam:], "auto bytes_at = [&](uint32_t skip)") + ";"
    at = create.index("// what it costs:")
    end = create.index("total_bytes += x.bytes + x.pad;", at) + len("total_bytes += x.bytes + x.pad;")
    count = create[at:end]
    helpers = "\n".join([block(source, "bool is_bc(VkFormat f)"), block(source, "bool is_astc(VkFormat f)"), helper])
    fixture = (ACCOUNTING_FIXTURE.replace("/* HELPERS */", helpers).replace("/* ESTIMATE */", estimate)
               .replace("/* COUNT */", count))
    images = [(1365, 1365, 1, 11, 90), (6144, 6144, 1, 13, 1), (1536, 3072, 1, 12, 6),
              (13, 13, 1, 4, 1), (4, 4, 1, 3, 1), (1, 1, 1, 1, 1), (32, 17, 9, 6, 1)]
    cases = []

    def add(vk, astc, bw, bh, bpb, bc=0, from_bytes=0, samples=1):
        for w, h, z, levels, layers in images:
            for skip in range(min(levels, 3)):
                for sparse, allocation in ((0, 0), (0, 1 << 30), (1, 1 << 30)):
                    cases.append((w, h, z, levels, layers, vk, astc, bw, bh, bpb, skip,
                                  bc, from_bytes, samples, sparse, allocation))

    for i, (bw, bh) in enumerate(ASTC):
        for vk in (157 + 2 * i, 158 + 2 * i):
            add(vk, 0, bw, bh, 16)
        add(37, 1, bw, bh, 16)          # decoded ASTC: RGBA8, not source blocks
        add(37, 1, bw, bh, 16, bc=1)    # re-encoded: always BC3's 4x4 blocks
    bc_formats = range(131, 147)         # is_bc's complete Vulkan range, UNORM/SNORM/sRGB/float variants
    for vk in bc_formats:
        add(vk, 0, 4, 4, 8 if vk in (131, 132, 133, 134, 139, 140) else 16)
    for bpb in (1, 2, 4, 8, 16):
        for samples in (1, 4):
            add(37, 0, 1, 1, bpb, samples=samples)
    for bpb in (3, 6, 12):
        add(37, 0, 1, 1, bpb, from_bytes=bpb)

    out = compile_run(cxx, temp, "accounting", fixture, cases)
    assert len(out) == len(cases), (len(out), len(cases))
    failures = []
    for c, line in zip(cases, out):
        w, h, z, levels, layers, vk, astc, bw, bh, bpb, skip, bc, from_bytes, samples, sparse, allocation = c

        def chain(uw, uh):
            return sum(expected(w >> l, h >> l, z >> l, uw, uh) for l in range(skip, levels)) * layers * samples

        native = not astc and (131 <= vk <= 146 or 157 <= vk <= 184)
        est = chain(bw, bh) * bpb if native else chain(1, 1) * (4 if astc else max(1, bpb // (bw * bh)))
        raw = (chain(4, 4) * 16 if bc else chain(bw, bh) * bpb if native else
               chain(1, 1) * (4 if astc else from_bytes * 4 // 3 if from_bytes else bpb))
        want = (est, 0, 0) if sparse else (est, raw, max(raw, allocation))
        got = tuple(map(int, line.split()))
        if got != want:
            failures.append((c, got, want))
    for c, got, want in failures[:12]:
        print("FAIL %s: estimate/raw/allocated %s, expected %s" % (c, got, want))
    assert not failures, "%d of %d accounting cases fail" % (len(failures), len(cases))
    print("texture accounting: %d cases pass; 16 BC formats, 14 ASTC block sizes, decoded/re-encoded ASTC, "
          "uncompressed/widened, layers/depth/samples, sparse and allocation floor" % len(cases))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_texture.cpp")
    a = p.parse_args()
    source = a.source.read_text(encoding="utf-8")
    helper = block(source, "uint64_t texture_level_units(")
    create = block(source, "Texture* create(const Key& key,")

    cases = []
    # (every other image: unit 1 x 1, the texels as before, levels past 1 x 1 included)
    for w, h, d in [(1365, 1365, 1), (2560, 1440, 1), (32, 32, 32), (0, 7, 0), (1, 1, 1), (6144, 6144, 1)]:
        for level in range(14):
            cases.append((w >> level, h >> level, d >> level, 1, 1))
    # (native ASTC: whole blocks, a part-block counting as one)
    for bw, bh in ASTC:
        for w, h in [(1365, 1365), (13, 13), (6144, 6144), (1536, 3072), (171, 171), (1, 1), (0, 0)]:
            cases.append((w, h, 1, bw, bh))
    with tempfile.TemporaryDirectory(prefix="hookshot-native-astc-count-") as temp:
        out = compile_run(a.cxx, temp, "units", FIXTURE.replace("/* ACTUAL_HELPER */", helper), cases)
        accounting_checks(source, create, helper, a.cxx, temp)
    assert len(out) == len(cases), (len(out), len(cases))
    fails = [(c, int(o), expected(*c)) for c, o in zip(cases, out) if int(o) != expected(*c)]
    for c, got, want in fails:
        print("FAIL %s: %d, expected %d" % (c, got, want))
    assert not fails
    for (w, h, d, bw, bh), o in zip(cases, out):
        if bw == bh == 1:
            assert int(o) == max(1, w) * max(1, h) * max(1, d)   # (unchanged for everything but native ASTC)

    # The array the UHD 770's log named: 1365 x 1365, 90 layers, 11 levels, ASTC 8 x 8. Counted as texels at 16 bytes,
    # 3410.0 MiB; its blocks are 53.8 MiB (the log labels MiB as "MB").
    texels = sum(expected(1365 >> l, 1365 >> l, 1, 1, 1) for l in range(11))
    blocks = sum(expected(1365 >> l, 1365 >> l, 1, 8, 8) for l in range(11))
    assert texels * 90 * 16 == 3575649600 and blocks * 90 * 16 == 56365920, (texels, blocks)
    print("native ASTC texture count: %d cases pass (the 1365 x 1365 x 90 ASTC 8x8 array: %.1f MiB counted before, %.1f MiB of blocks)"
          % (len(cases), texels * 90 * 16 / 2**20, blocks * 90 * 16 / 2**20))


if __name__ == "__main__":
    main()
