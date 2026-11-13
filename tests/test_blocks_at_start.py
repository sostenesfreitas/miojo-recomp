"""The blocks a pool starts with are made in the pool the sampled textures are made in, and in no other.

#2438 made 15% of the card's memory (2 to 24 blocks of 128 MB) at the start in the images' pool, so that play makes fewer of
them (a vkAllocateMemory each, ~170 ms on AMD's Windows driver). Compacted, which is the default, the textures are made in a
pool of their own: the blocks made at the start stayed empty in the images' pool, counted by the driver and of no use to a
texture, and the budget's hold cut the textures by as much. On an 8 GB RTX 3070 Castle Town Market ran at 31-36 fps against
54-56 with RX_VK_IMAGE_PREALLOC=0 (#2690). The rule (gpu/vk/vram_compact.h) is compiled and run for 2 to 32 GB cards, with
what the budget's hold alone leaves the textures either way, and vk_vma.cpp and vk_texture.cpp are checked to use it. No GPU or game.

python -I -B tests/test_blocks_at_start.py [--cxx g++]
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


def check_source():
    vma = (ROOT / "gpu/vk/vk_vma.cpp").read_text(encoding="utf-8")
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="blocks-at-start-") as tmp:
        exe = Path(tmp) / "blocks-at-start"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_blocks_at_start.cpp"), "-o",
                        str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
