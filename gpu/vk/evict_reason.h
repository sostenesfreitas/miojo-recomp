// Why the eviction pass let a texture go, and whether its texels went with it (vk_texture.cpp). tests/test_evict_reason.py
//
// The "N textures retired" line said how many went, not why: a 9070 XT's log at 3.3 GB of its 6 GB budget retired 28,000
// textures with the ground garbled until a restart (#bug-reporting, worklist "ground and outdoor textures garble"), and the
// log could not tell budget evictions from the render-target sweeps, nor say whether a texture the GPU drew left with the only
// copy of its texels. Each pass now counts its textures by reason, and the ones whose texels exist nowhere else.
#pragma once
#include <cstdint>
#include <cstdio>

namespace evict_reason {

enum Reason : int {
    BUDGET,       // over the budget: the least recently used
    DRAWN_OVER,   // a render target another image has drawn over at the same start since its last use
    DEAD,         // a transient render target whose storage another one drew over from another start (target_overlap.h)
    COUNT
};

// (the most specific first: a shifted one is also "overwritten")
inline Reason classify(bool shifted, bool overwritten) { return shifted ? DEAD : overwritten ? DRAWN_OVER : BUDGET; }

// A texture the GPU drew, that nothing drew over, going without a write-back into its memory: its image held the only copy, and
// one made again over that storage is not uploaded (the storage counts as the GPU's). That is how a terrain chunk's normal map
// came back empty before (issues 107, 386)
inline bool texels_lost(bool gpu_written, bool overwritten, bool flushed) { return gpu_written && !overwritten && !flushed; }

// A write-back before eviction puts an image's texels over all of its storage and marks those pages written. It is stale when
// another image was drawn into that storage after this one's last drawing (other_written: the newest such drawing's frame, 0 for
// none; vk_texture.cpp's drawn_others): that memory is the other image's now, and a finalized image there is uploaded from what
// was written. A drawing in the same frame keeps the write-back: which of the two came last is not known
inline bool write_back_stale(uint64_t last_written, uint64_t other_written) { return other_written > last_written; }

struct Tally {
    uint32_t n[COUNT] = {};
    uint32_t lost = 0, lost_finalized = 0;   // (of lost: the ones the game had finalized, which keep_finalized is for)
    uint64_t lost_bytes = 0;
    void add(Reason r, bool lost_texels, uint64_t bytes, bool finalized = false) {
        n[r]++;
        if (lost_texels) lost++, lost_bytes += bytes, lost_finalized += finalized;
    }
    uint32_t total() const { return n[BUDGET] + n[DRAWN_OVER] + n[DEAD]; }
};

// "budget 3, drawn over 1, dead targets 40; GPU-drawn with no copy kept: 2 live, 5 finalized, 1 MB": what follows the count in
// the log line
inline int describe(const Tally& t, char* out, size_t size) {
    const int n = std::snprintf(out, size, "budget %u, drawn over %u, dead targets %u", t.n[BUDGET], t.n[DRAWN_OVER], t.n[DEAD]);
    if (n < 0 || (size_t)n >= size || !t.lost) return n;
    return n + std::snprintf(out + n, size - (size_t)n, "; GPU-drawn with no copy kept: %u live, %u finalized, %llu MB",
                             t.lost - t.lost_finalized, t.lost_finalized, (unsigned long long)(t.lost_bytes >> 20));
}

} // namespace evict_reason
