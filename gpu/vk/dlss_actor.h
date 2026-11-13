// (hookshot) NVIDIA DLSS's actor motion (vk_dlss.cpp): what is worked out on the CPU for a skinned draw drawn again, kept
// apart so tests/test_dlss_actor.cpp checks it without a GPU. Header only, no Vulkan.
#pragma once

#include <algorithm>
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
