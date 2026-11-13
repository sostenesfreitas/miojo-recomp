// (hookshot) An import trimmed back to the game's live pools (RX_VK_MIRROR_TRIM).
//
// Imports only ever grew: a binding across an import's end makes it again, larger, over what it overlaps, and a pool the game
// binds whole is imported whole. A pool's VRAM mirror (and its write-back shadow) is its import's whole range. So after the game
// frees a scene's large pools and makes the next scene's smaller ones at the same addresses, the import keeps the old range,
// bound every frame through the small live pool inside it, and its mirror holds the dead rest in VRAM for the session. Castle
// Town Market entered from Hyrule Field on an 8 GB card (RTX 3060 Ti): the field's 213 MB pool at 0x108c711000 leaves a 224 MB
// mirror where the Market, loaded from a save, has 48 MB, and the culling lists' pool 152 MB where it has 104: ~225 MB of
// mirrors and ~50 MB of shadows over what the scene binds. That is the difference between the textures fitting (54 fps) and
// being evicted and made again in a loop (45 fps, 16-25 frames over 50 ms in 30 s).
//
// So an import whose mirror is mostly outside every game pool, and has stayed so for a while, is retired and its live parts
// imported again on their own, their mirrors taking the GPU's contents from the old one as a pool made again larger does.
// Rules only, no Vulkan: vk_memory.cpp does it (mirrors_trim). tests/test_mirror_trim.cpp runs them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirror_trim {

struct Range {
    uint64_t lo, hi;
};

inline constexpr uint64_t kMinDead = 24ull << 20;   // (the least a trim gives back)
inline constexpr uint64_t kJoin = 8ull << 20;       // (live parts closer than this stay one import)
inline constexpr size_t kMaxParts = 4;              // (more live parts than this: left alone, each is an import in the driver)
inline constexpr uint64_t kWait = 1800;             // (submissions an import stays mostly dead before it is trimmed: ~5 s)
inline constexpr unsigned kMaxTrims = 3;            // (an import made again over a trimmed one waits 4x longer each time, then not)
inline constexpr uint64_t kForget = 36000;          // (submissions with no trim of a range after which its count starts over: ~2 min)
inline constexpr uint64_t kPressedPct = 80;         // (only while VRAM is this full: with room to spare nothing is gained)

/// The parts of the import [lo, hi) that game pools cover, each out to its chunks and kept inside the import, in order; parts
/// closer than `join` are one. `pools` is the game's pools sorted by lo (they may overlap), each with .lo and .hi
template <class Pools>
inline std::vector<Range> live(uint64_t lo, uint64_t hi, uint64_t chunk, uint64_t join, const Pools& pools) {
    std::vector<Range> out;
    if (!chunk || hi <= lo) return out;
    for (const auto& g : pools) {
        if (g.lo >= hi) break;
        if (g.hi <= lo || g.hi <= g.lo) continue;
        uint64_t a = g.lo / chunk * chunk, b = (g.hi + chunk - 1) / chunk * chunk;
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        if (!out.empty() && a <= out.back().hi + join) {   // (sorted by lo: only the last part can be near)
            if (b > out.back().hi) out.back().hi = b;
        } else {
            out.push_back({a, b});
        }
    }
    return out;
}

inline uint64_t bytes(const std::vector<Range>& parts) {
    uint64_t n = 0;
    for (const Range& r : parts) n += r.hi - r.lo;
    return n;
}

/// Whether an import of `size` bytes with these live parts is worth making again smaller: it gives back at least kMinDead and a
/// quarter of the import, and leaves 1 to kMaxParts parts. With no live part nothing binds it: its mirror goes by the idle rules
/// (mirrors_give_back), which keep what the GPU wrote
inline bool worth(uint64_t size, const std::vector<Range>& parts) {
    if (parts.empty() || parts.size() > kMaxParts) return false;
    const uint64_t kept = bytes(parts);
    if (kept >= size) return false;
    const uint64_t dead = size - kept;
    return dead >= kMinDead && dead >= size / 4;
}

/// How long an import must have stayed worth trimming (submissions), by how often its range was trimmed before: 0 not now
inline uint64_t wait(unsigned trims) { return trims >= kMaxTrims ? 0 : kWait << (2 * trims); }

/// The trims that count against a range: none once kForget submissions passed since the last. A range the game binds across
/// right after each trim (made again larger, trimmed again) is slowed and then left alone, three imports in two minutes at
/// most; a scene change every few minutes over a session is not that, and each one's dead rest goes after the first wait
inline unsigned counted(unsigned trims, uint64_t since_last) { return since_last > kForget ? 0 : trims; }

/// Whether VRAM is full enough for trims to run at all
inline bool pressed(uint64_t used, uint64_t allowed) { return allowed && used > allowed / 100 * kPressedPct; }

}  // namespace mirror_trim
