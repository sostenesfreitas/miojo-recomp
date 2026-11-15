// (hookshot) A VRAM mirror's blocks that hold memory (vk_memory.cpp, RX_VK_MIRROR_SPARSE), apart from Vulkan so that
// tests/test_sparse_mirror.py can run it.
//
// A pool's mirror covers its import's whole range, and an import is made by chunk around what was bound: the mirrors
// held 243-533 MB of blocks outside every game pool (padding, and pools the game has let go; most after an area change),
// of 1.1-1.6 GB, at DLAA 2560 x 1440 with 4 MB chunks. A sparse mirror gives memory only to the blocks a game pool is in.
// The others read 0 and drop writes (residencyNonResidentStrict), and nothing of the game's reaches them: the page table
// points their pages at guest memory, and a binding that reaches one where a game pool is is served from guest memory
// until the block is given memory at the next submission (one whose blocks with none are all outside every game pool, a
// guessed size run past a pool's end, is served from the mirror as it is). A block a game pool has left is given back
// once the GPU is past it. On by default (RX_VK_MIRROR_SPARSE=0: whole mirrors).
#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace sparse_mirror {

/// The blocks of a mirror of [lo, hi), `block` bytes each, that any of the game's pools [first, last) reaches (sorted by
/// lo, as guest_pools; an earlier pool may reach past later ones): 1, the others 0. Into `live`, whose storage is kept
template <class It>
void live_blocks(uint64_t lo, uint64_t hi, uint64_t block, It first, It last, std::vector<uint8_t>& live) {
    live.assign(hi > lo && block ? (size_t)((hi - lo + block - 1) / block) : 0, 0);
    if (live.empty()) return;
    // (every pool that starts below hi: one that starts far below lo can still reach into it)
    const It end = std::upper_bound(first, last, hi - 1, [](uint64_t a, const auto& p) { return a < p.lo; });
    for (It it = first; it != end; ++it) {
        if (it->hi <= lo || it->hi <= it->lo) continue;
        const uint64_t a = std::max(it->lo, lo) - lo, b = std::min(it->hi, hi) - lo;
        for (uint64_t i = a / block; i < (b + block - 1) / block; i++) live[(size_t)i] = 1;
    }
}
/// The same, returned
template <class It>
std::vector<uint8_t> live_blocks(uint64_t lo, uint64_t hi, uint64_t block, It first, It last) {
    std::vector<uint8_t> live;
    live_blocks(lo, hi, block, first, last, live);
    return live;
}

/// The blocks [off, off + size) reaches: [*first, *end)
inline void block_span(uint64_t off, uint64_t size, uint64_t block, size_t nblocks, size_t* first, size_t* end) {
    *first = (size_t)std::min<uint64_t>(off / block, nblocks);
    *end = size ? (size_t)std::min<uint64_t>((off + size + block - 1) / block, nblocks) : *first;
}

/// Runs of consecutive blocks whose state is `want` (get(i)): emit(first, count) each
template <class Get, class Emit>
void runs(size_t nblocks, Get get, bool want, Emit emit) {
    for (size_t i = 0; i < nblocks;) {
        if ((get(i) != 0) != want) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < nblocks && (get(j) != 0) == want) j++;
        emit(i, j - i);
        i = j;
    }
}

struct Copy { uint64_t src, dst, size; };

/// The copies clipped to their parts whose source block (src_has(src offset)) and destination block (dst_has(dst
/// offset)) both hold memory, `block` bytes each, cut where either changes. A copy from a block with none read 0 and
/// put those zeros over what guest memory gave the destination; one into such a block was lost
template <class SrcHas, class DstHas>
std::vector<Copy> clip(const std::vector<Copy>& in, uint64_t block, SrcHas src_has, DstHas dst_has) {
    std::vector<Copy> out;
    for (const Copy& c : in) {
        uint64_t at = 0;
        while (at < c.size) {
            const uint64_t s = c.src + at, d = c.dst + at;
            // (to the next block end on either side)
            const uint64_t step = std::min({c.size - at, block - s % block, block - d % block});
            if (src_has(s) && dst_has(d)) {
                if (!out.empty() && out.back().src + out.back().size == s && out.back().dst + out.back().size == d)
                    out.back().size += step;
                else
                    out.push_back({s, d, step});
            }
            at += step;
        }
    }
    return out;
}

/// The copies with every part whose source lies in one of `ranges` ([lo, hi) source offsets) cut out
inline std::vector<Copy> cut_src(const std::vector<Copy>& in, const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    std::vector<Copy> cur = in;
    for (const auto& [lo, hi] : ranges) {
        if (hi <= lo) continue;
        std::vector<Copy> next;
        for (const Copy& c : cur) {
            const uint64_t s = c.src, e = c.src + c.size;
            if (e <= lo || s >= hi) {
                next.push_back(c);
                continue;
            }
            if (s < lo) next.push_back({s, c.dst, lo - s});
            if (e > hi) next.push_back({hi, c.dst + (hi - s), e - hi});
        }
        cur.swap(next);
    }
    return cur;
}

} // namespace sparse_mirror
