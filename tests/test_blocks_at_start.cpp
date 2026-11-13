// The blocks made at the start (gpu/vk/vram_compact.h): tests/test_blocks_at_start.py compiles and runs this.
#include "../gpu/vk/vram_compact.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
const uint64_t MB = 1ull << 20, GB = 1ull << 30, kBlock = 128 * MB;

/// The texture budget's hold (85% of the allowance) on a card whose driver counts `other` MB for everything that is not a
/// sampled texture and `textures` MB of textures, with `at_start` blocks made at the start in the textures' pool (theirs) or
/// in a pool no texture is made in. The driver counts whole blocks; the budget takes off the textures' pool's free space
uint64_t held(uint64_t allowed, uint64_t other, uint64_t textures, uint64_t at_start, bool theirs) {
    using namespace vram_compact;
    const uint64_t kept = at_start * kBlock;
    const uint64_t pool = theirs ? std::max(kept, textures) : textures;   // (the textures' pool's blocks)
    const uint64_t used = other + pool + (theirs ? 0 : kept);
    const uint64_t block_free = used - other - textures;
    return held_budget(6144 * MB, textures, used, reclaimable(block_free, pool - textures), allowed, 85);
}
}

int main() {
    using namespace vram_compact;
    // 15% of the card's memory in 128 MB blocks, 2 to 24: the same rule for every vendor (the stall it removes was measured on an
    // AMD card; nothing here asks which card it is)
    const struct { uint64_t gb, blocks; } cards[] = {{2, 2}, {4, 4}, {6, 7}, {8, 9}, {12, 14}, {16, 19}, {24, 24}, {32, 24}};
    for (const auto& c : cards) {
        CHECK(blocks_at_start(c.gb * GB, kBlock, true) == c.blocks);
        // and none in a pool the sampled textures are not made in: nothing would fill them
        CHECK(blocks_at_start(c.gb * GB, kBlock, false) == 0);
    }
    CHECK(blocks_at_start(8 * GB, 32 * MB, true) == 24);    // (RX_VK_IMAGE_BLOCK_MB=32: #2690's first table)
    CHECK(blocks_at_start(8 * GB, 0, true) == 0);
    // what a run can give back: not what the blocks made at the start hold free
    CHECK(returnable(1152 * MB, 900 * MB, 1152 * MB) == 0);              // (nine blocks, 252 MB of textures: all nine are kept)
    CHECK(returnable(2560 * MB, 700 * MB, 1152 * MB) == 700 * MB);       // (twenty blocks: eleven can go, 700 MB is free)
    CHECK(returnable(1280 * MB, 700 * MB, 1152 * MB) == 128 * MB);       // (ten blocks: one can go)
    CHECK(returnable(1024 * MB, 700 * MB, 0) == 700 * MB);               // (none made at the start: as before)
    CHECK(returnable(512 * MB, 100 * MB, 1152 * MB) == 0);
    // down to the blocks made at the start, an eviction takes nothing off the driver's count: the 97% rule waits
    CHECK(kept_only(1152 * MB, 1152 * MB));
    CHECK(!kept_only(1280 * MB, 1152 * MB));    // (a tenth block: a run can give it back)
    CHECK(!kept_only(0, 0) && !kept_only(256 * MB, 0));   // (none made at the start: the rule as before)
    // #2690, an 8 GB card in Castle Town Market: about 7000 MB allowed, 3700 MB of render targets, mirrors and buffers, and
    // 2300 MB of textures wanted. With the nine blocks in a pool no texture is made in, the hold leaves the textures 1098 MB;
    // in the textures' pool, what it leaves without any block made at the start
    for (const auto& c : cards) {
        // (about what a driver allows of a card, and what is not a sampled texture: 3700 MB, half the allowance on a small card)
        const uint64_t allowed = c.gb * GB / 100 * 87, other = std::min(3700 * MB, allowed / 2);
        for (uint64_t textures = 256 * MB; textures <= 6144 * MB; textures += 256 * MB) {
            const uint64_t none = held(allowed, other, textures, 0, true);
            CHECK(held(allowed, other, textures, c.blocks, true) == none);
            // (in a pool no texture is made in, the hold cuts by the blocks' size wherever it cuts at all)
            const uint64_t wrong = held(allowed, other, textures, c.blocks, false);
            CHECK(wrong <= none);
            if (none < 6144 * MB) CHECK(none - wrong == std::min(none, c.blocks * kBlock));
        }
    }
    CHECK(held(7000 * MB, 3700 * MB, 2300 * MB, 0, true) == 2250 * MB);
    CHECK(held(7000 * MB, 3700 * MB, 2300 * MB, 9, true) == 2250 * MB);
    CHECK(held(7000 * MB, 3700 * MB, 2300 * MB, 9, false) == 1098 * MB);
    std::printf("PASS\n");
}
