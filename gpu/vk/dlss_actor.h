// (hookshot) NVIDIA DLSS's actor motion (vk_dlss.cpp): what is worked out on the CPU for a skinned draw drawn again, kept
// apart so tests/test_dlss_actor.cpp checks it without a GPU. Header only, no Vulkan.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dlss_actor {

/// A draw's name across frames: its model (vertex and index buffers, count, base vertex, program) and which of the frame's
/// draws of that model it is (the same model drawn twice: the n-th draw of it is matched with the n-th the frame before)
inline uint64_t key(uint64_t vbuf, uint64_t ibuf, int count, int base_vertex, uint64_t program, int nth) {
    uint64_t h = 0x5bd1e995;
    for (uint64_t v : {vbuf, ibuf, (uint64_t)(uint32_t)count, (uint64_t)(uint32_t)base_vertex, program}) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 0xff51afd7ed558ccdull;
    }
    return (h & ~0xffull) | (uint64_t)std::clamp(nth, 0, 255);
}
inline bool same_model(uint64_t a, uint64_t b) { return a >> 8 == b >> 8; }

/// (hookshot) A copy of a model, for pair_by_place: where it is (place_of) and which draw of the model it is (its n-th)
struct Placed {
    float place[3];
    int nth;
};

/// (hookshot) Which of the frame before's copies of a model each of this frame's copies is, by place: for each of `now`, an
/// index into `then`, or -1 (none: the camera's motion then, never another copy's). All copies are paired at once, the
/// nearest pairs first, and never further apart than `reach` (in world units, moved in one frame). On a tie (the same
/// geometry drawn twice for one object, both at one place) the same n-th draw pairs first.
/// By draw order alone, the four Gossip Stones drawn in another order took each other's places as their own frame before,
/// and DLSS Frame Generation drew a stone part way to the next one. Taking the nearest free copy in draw order also failed:
/// in a row of evenly spaced stones, a stone whose own frame before is missing (not drawn, or drawn by the other model the
/// game uses for them) took its neighbour's
inline std::vector<int> pair_by_place(const std::vector<Placed>& now, const std::vector<Placed>& then, float reach) {
    struct Candidate {
        double d;     // (squared)
        bool other;   // (not the same n-th: on a tie at one place, the same n-th pairs first)
        int i, j;
    };
    std::vector<Candidate> candidates;
    const double r2 = (double)reach * (double)reach;
    for (int i = 0; i < (int)now.size(); i++)
        for (int j = 0; j < (int)then.size(); j++) {
            double d = 0.0;
            for (int c = 0; c < 3; c++) {
                const double e = (double)now[i].place[c] - (double)then[j].place[c];
                d += e * e;
            }
            if (std::isfinite(d) && d <= r2) candidates.push_back({d, now[i].nth != then[j].nth, i, j});
        }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.d != b.d ? a.d < b.d : a.other != b.other ? !a.other : a.i != b.i ? a.i < b.i : a.j < b.j;
    });
    std::vector<int> pairs(now.size(), -1);
    std::vector<char> taken(then.size(), 0);
    for (const Candidate& c : candidates)
        if (pairs[c.i] < 0 && !taken[c.j]) pairs[c.i] = c.j, taken[c.j] = 1;
    return pairs;
}

/// (hookshot) Where a skinned draw is, to tell copies of one model apart: its origin plus the mean of its bones'
/// translations. pose: the origin (12 bytes, then 4 of padding), then nbones 3 x 4 row-major matrices, as the actor pass
/// keeps it (world = sum(w B p) + origin). A model placed by its origin differs there; copies placed by their bones at one
/// origin differ in the bones (the game's copies of one model share an origin on a grid of 10 units, the Gossip Stones
/// among them); the sum is the place either way. Bones with a translation that is not finite are left out
inline void place_of(const uint8_t* pose, uint32_t nbones, float place[3]) {
    float o[3];
    std::memcpy(o, pose, sizeof o);
    double t[3] = {0.0, 0.0, 0.0};
    uint32_t used = 0;
    for (uint32_t b = 0; b < nbones; b++) {
        float m[12];
        std::memcpy(m, pose + 16 + (size_t)b * 48, sizeof m);
        if (!std::isfinite(m[3]) || !std::isfinite(m[7]) || !std::isfinite(m[11])) continue;
        t[0] += m[3], t[1] += m[7], t[2] += m[11];
        used++;
    }
    for (int c = 0; c < 3; c++) place[c] = o[c] + (used ? (float)(t[c] / used) : 0.0f);
}

/// shaders/dlss_actor.vert's push constants (the fragment stage's the same)
struct Push {
    uint64_t vertices;   // (the vertex stream's device address)
    uint64_t indices;    // (the index buffer's, 4-aligned down: first says where the draw's indices start in it)
    uint64_t origins[2]; // (the draw's origin, xyz: this frame's and the frame before's, copied into the pose buffers)
    uint64_t bones[2];   // (its palettes: 3 x 4 row-major matrices, 12 floats a bone)
    uint32_t nbones;
    uint32_t stride;     // (in words)
    uint32_t count;      // (the vertices in the stream: an index past them draws nothing)
    uint32_t first;      // (in indices, from the address)
    uint32_t itype;      // (0: 8-bit, 1: 16-bit, 2: 32-bit indices)
    int32_t base_vertex;
    uint32_t pos, sets;  // (bytes in a vertex: the position; the joint and weight sets, 1 or 2)
    uint32_t joints[2], weights[2];
    uint32_t formats;    // (kHalfPosition, kByteJoints << set)
    uint32_t tolerance;  // (depth steps from the scene's within which a pixel is the draw's)
};
static_assert(sizeof(Push) == 6 * 8 + 14 * 4 && sizeof(Push) <= 128, "dlss_actor.vert's push constants");

/// Where a draw's indices are for the shader: the address 4-aligned down, and its first index from there
inline void index_start(uint64_t address, int itype, uint64_t* aligned, uint32_t* first) {
    *aligned = address & ~3ull;
    *first = (uint32_t)((address & 3) >> (itype < 0 ? 0 : itype > 2 ? 2 : itype));
}

/// Push::formats: the position is RGBA16F (else RGB32F); a set's joints are RGBA8UI (else RGBA16UI)
constexpr uint32_t kHalfPosition = 1, kByteJoints = 2;
/// Push::formats: set n is a single influence (an R8 weight; its joint R8UI with kByteJoints << n, else R16UI)
constexpr uint32_t kSingle = 8;
/// Push::formats: set n has two influences (an RG8 weight pair and an RG8UI joint pair, any byte offset)
constexpr uint32_t kPair = 32;
/// Push::formats: set n has no weights, its one joint weighs 1 (rigid models bound to one bone: Navi's body,
/// props, held items; with kSingle)
constexpr uint32_t kNoWeight = 128;

/// The cameras' buffer, 36 words: this frame's camera (as drawn, jittered), the frame before's (its jitter taken off), this
/// frame's jitter in clip space, the render size
constexpr uint32_t kHeadWords = 36;

} // namespace dlss_actor
