/* Render targets drawn over from another start (vk_texture.cpp's eviction): a frame's transient targets that the game
   makes in a scratch heap whose offsets shift from frame to frame. */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::target_overlap {

/// The spans of a render target's storage [lo, hi), last used at frame `last_used`, that newer render targets have drawn
/// over, merged: one from another start covers its share, one written before the last use covers nothing, and the ones over
/// its two ends are added up. With a view of a part of it excluded, every candidate over its start clips to `lo` and every
/// one over its end to `hi`, so each side keeps only its longest: the sum differs from one over the whole span only when one
/// newer target covers the head and another the tail (each under an eighth, together an eighth or more). The candidates
/// come in start order (vk_texture.cpp's by_start).
struct DrawnOver {
    uint64_t bytes = 0;   // (the spans closed so far)
    uint64_t start = 0, end = 0;   // (the one being grown)
    bool open = false;

    /// Adds what another render target [olo, ohi), last written at `o_written`, has drawn over of [lo, hi)
    void add(uint64_t lo, uint64_t hi, uint64_t last_used, uint64_t olo, uint64_t ohi, uint64_t o_written) {
        if (hi <= lo || ohi <= olo || o_written <= last_used) return;
        if (olo == lo) return;   // (drawn_over's own case)
        if (olo >= lo && ohi <= hi) return;   // (a view of a part of it, not a new target: 5bfa6aa, PR #899)
        const uint64_t a = std::max(lo, olo), b = std::min(hi, ohi);
        if (b <= a) return;
        if (open && a <= end) {
            end = std::max(end, b);
            return;
        }
        if (open) bytes += end - start;
        start = a, end = b, open = true;
    }

    /// Whether they cover an eighth of it or more: its texels there are gone from that memory on the console too
    bool an_eighth(uint64_t size) const { return size && (bytes + (open ? end - start : 0)) * 8 >= size; }
};

} // namespace mkwvk::target_overlap
