"""The blocks a pool starts with are made in the pool the sampled textures are made in, and in no other.

#2438 made 15% of the card's memory (2 to 24 blocks of 128 MB) at the start in the images' pool, so that play makes fewer of
them (a vkAllocateMemory each, ~170 ms on AMD's Windows driver). Compacted, which is the default, the textures are made in a
pool of their own: the blocks made at the start stayed empty in the images' pool, counted by the driver and of no use to a
texture, and the budget's hold cut the textures by as much. On an 8 GB RTX 3070 Castle Town Market ran at 31-36 fps against
54-56 with RX_VK_IMAGE_PREALLOC=0 (#2690). The rule (gpu/vk/vram_compact.h) is compiled and run for 2 to 32 GB cards, with
what the budget's hold alone leaves the textures either way, and vk_vma.cpp and vk_texture.cpp are checked to use it. No GPU or game.

python -I -B tests/test_blocks_at_start.py [--cxx g++] [--source /path/to/source/tree]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def check_source(source):
    vma = (source / "gpu/vk/vk_vma.cpp").read_text(encoding="utf-8")
    tex = (source / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    if "static size_t blocks_at_start(bool holds_textures)" not in vma:
        raise SystemExit("vk_vma.cpp: the blocks made at the start are not chosen by the pool that holds the textures (#2690: "
                         "made in the images' pool, they stay empty while the textures are compacted in a pool of their own)")
    need("vk_vma.cpp: blocks_at_start()", block(vma, "static size_t blocks_at_start(bool holds_textures)"),
         ["if (!holds_textures) return 0;", 'getenv("RX_VK_IMAGE_PREALLOC")',
          "vram_compact::blocks_at_start(heap, kImageBlock, holds_textures)"])
    # (the images' pool only when the textures are made there, RX_VK_COMPACT=0; else the textures' own)
    need("vk_vma.cpp: images_pool()", block(vma, "VmaPool images_pool() {"),
         ['make_pool(pi, blocks_at_start(!vram_compact::enabled(getenv("RX_VK_COMPACT"))), image_pool)', "made at the start"])
    need("vk_vma.cpp: textures_pool()", block(vma, "VmaPool textures_pool() {"),
         ["texture_pool_kept = make_pool(pi, blocks_at_start(true), texture_pool) * kImageBlock;", "made at the start"])
    # (no room for the blocks: the pool empty, as before them)
    need("vk_vma.cpp: make_pool()", block(vma, "static size_t make_pool("),
         ["pi.minBlockCount = n;", "pi.minBlockCount = 0;"])
    # (and a compaction run does not start for free space VMA keeps)
    need("vk_vma.cpp: textures_pool_usage()", block(vma, "bool textures_pool_usage("), ["*kept_bytes = texture_pool_kept;"])
    need("vk_texture.cpp: compact_step()", block(tex, "void compact_step(uint64_t completed)"),
         ["textures_pool_usage(&block_bytes, &free_bytes, &kept)",
          "vram_compact::returnable(block_bytes, free_bytes, kept) < vram_compact::run_from(min_free, compact.pressed)"])
    # (the budget still takes the whole of the textures' pool's free space off the count: the textures fill it. And the 97%
    # rule, which goes by the driver's count alone, waits while that pool is down to the blocks VMA keeps)
    need("vk_texture.cpp: texture_frame()", block(tex, "void texture_frame(uint64_t completed)"),
         ["if (textures_pool_usage(&pool_blocks, &pool_free, &pool_kept)) reclaim = vram_compact::reclaimable(block_free, pool_free);",
          "kept_only = vram_compact::kept_only(pool_blocks, pool_kept);",
          "!(kept_only && !squeeze_always) && used > allowed / 100 * 97 && !room_in_blocks)"])


# Compile the real pool constructors, not a second implementation of their policy.
# The allocator refuses only the requested calls; no Vulkan device or memory pressure.
POOL_FIXTURE = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "vram_compact.h"
constexpr int VK_SUCCESS = 0, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 1;
constexpr int VK_IMAGE_TYPE_2D = 1, VK_FORMAT_R8G8B8A8_UNORM = 1, VK_SAMPLE_COUNT_1_BIT = 1;
constexpr int VK_IMAGE_USAGE_SAMPLED_BIT = 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 2, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 4;
constexpr int VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE = 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT = 1;
#define VK_NULL_HANDLE nullptr
struct VkImageCreateInfo {
    explicit VkImageCreateInfo(int) {}
    int imageType{}, format{};
    struct { uint32_t width, height, depth; } extent{};
    uint32_t mipLevels{}, arrayLayers{}, samples{}, usage{};
};
struct VmaAllocationCreateInfo { int usage{}, requiredFlags{}; };
struct VmaPoolCreateInfo { uint32_t memoryTypeIndex{}; uint64_t blockSize{}; size_t minBlockCount{}; };
struct Pool { VmaPoolCreateInfo info; };
using VmaPool = Pool*;
struct { struct {
    uint32_t memoryHeapCount = 1;
    struct { uint64_t size = 8ull << 30; int flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT; } memoryHeaps[1];
} memory; } dev;
int vma, calls, successes;
bool fail_prealloc, fail_all;
size_t requested[8]{};
Pool pools[8]{};
VmaPool image_pool{}, texture_pool{};
bool image_pool_tried{}, texture_pool_tried{};
uint64_t texture_pool_kept{};
const uint64_t kImageBlock = 128ull << 20;
const char* compact_env;
const char* prealloc_env;
const char* test_getenv(const char* name) {
    if (!std::strcmp(name, "RX_VK_COMPACT")) return compact_env;
    if (!std::strcmp(name, "RX_VK_IMAGE_PREALLOC")) return prealloc_env;
    return nullptr;
}
#define getenv test_getenv
void logf(const char*, ...) {}
int vmaFindMemoryTypeIndexForImageInfo(int, const VkImageCreateInfo*, const VmaAllocationCreateInfo*, uint32_t* type) {
    *type = 7; return VK_SUCCESS;
}
int vmaCreatePool(int, const VmaPoolCreateInfo* info, VmaPool* pool) {
    requested[calls++] = info->minBlockCount;
    if (fail_all || (fail_prealloc && info->minBlockCount)) return -2;
    pools[successes].info = *info;
    *pool = &pools[successes++];
    return VK_SUCCESS;
}
@FUNCTIONS@
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s: %s\n", argv[1], #x); return 1; } } while (0)
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const bool compact = std::strcmp(argv[1], "compaction-off") != 0;
    compact_env = compact ? nullptr : "0";
    if (!std::strcmp(argv[1], "override")) prealloc_env = "3";
    if (!std::strcmp(argv[1], "prealloc-off")) prealloc_env = "0";
    fail_prealloc = !std::strcmp(argv[1], "fallback");
    fail_all = !std::strcmp(argv[1], "failed-pool");
    VmaPool utility = images_pool();
    VmaPool sampled = compact ? textures_pool() : utility;
    if (fail_all) {
        CHECK(!utility && !sampled && !texture_pool_kept);
        CHECK(calls == 3 && requested[0] == 0 && requested[1] == 9 && requested[2] == 0);
    } else {
        const size_t wanted = prealloc_env ? size_t(std::atoi(prealloc_env)) : 9;
        CHECK(utility && sampled);
        CHECK(utility->info.minBlockCount == (compact || fail_prealloc ? 0 : wanted));
        CHECK(sampled->info.minBlockCount == (fail_prealloc ? 0 : wanted));
        CHECK(sampled->info.memoryTypeIndex == 7 && sampled->info.blockSize == kImageBlock);
        if (compact) CHECK(texture_pool_kept == sampled->info.minBlockCount * kImageBlock);
        if (fail_prealloc) CHECK(calls == 3 && requested[1] == 9 && requested[2] == 0);
    }
    const int before = calls;
    CHECK(images_pool() == utility);
    if (compact) CHECK(textures_pool() == sampled);
    CHECK(calls == before);
    std::printf("PASS pool constructors: %s\n", argv[1]);
}
'''


def compile_pools(source, cxx, tmp):
    vma = (source / "gpu/vk/vk_vma.cpp").read_text(encoding="utf-8")
    functions = []
    for marker in ("static size_t blocks_at_start(", "static size_t make_pool(",
                   "VmaPool images_pool() {", "VmaPool textures_pool() {"):
        if marker in vma:
            functions.append(block(vma, marker))
    fixture = tmp / "pools.cpp"
    fixture.write_text(POOL_FIXTURE.replace("@FUNCTIONS@", "\n".join(functions)), encoding="utf-8")
    exe = tmp / "pools"
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(source / "gpu/vk"),
                    str(fixture), "-o", str(exe)], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT, help="source tree whose production pool constructors are tested")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="blocks-at-start-") as tmp:
        pools = compile_pools(args.source.resolve(), args.cxx, Path(tmp))
        exe = Path(tmp) / "blocks-at-start"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_blocks_at_start.cpp"), "-o",
                        str(exe)], check=True)
        failures = 0
        for case in ("default", "compaction-off", "override", "prealloc-off", "fallback", "failed-pool"):
            failures += subprocess.run([str(pools), case]).returncode != 0
        if failures:
            raise SystemExit(f"{failures} pool constructor cases failed")
        check_source(args.source.resolve())
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
