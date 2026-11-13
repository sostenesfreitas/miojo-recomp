// Which textures the GPU drew have their texels written into guest memory soon after, and when (vk_texture.cpp).
// tests/test_drawn_writeback.py
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace drawn_writeback {

// On the console a texture the GPU drew is in its memory as soon as the drawing is done, and Ocarina of Time relies on
// it: its terrain bakes each chunk's normal tile on the GPU (260 x 260 RG8, nine levels), and later moves the chunk's
// tiles into another block with a CPU copy of their memory. Here the drawn texels were only in the Vulkan image, so
// the copy of a drawn tile was all zero, and the ground under it came out garbled until a restart (issue 1717).
constexpr uint64_t kMaxBytes = 1ull << 20;   // (the tiles are 209 KB; a frame's render targets are far larger)
constexpr uint64_t kFrames = 2;              // (frames after the drawing: the game's work that wrote it is done)

// A texture whose GPU contents begin now: noted for a write-back when it is small, has a mip chain (a baked map, not
// a frame's render target) and lies in a pool's own memory (not a virtual pool's)
inline bool noted(uint64_t gpu, uint64_t guest_size, uint32_t levels) {
    return gpu < (1ull << 40) && guest_size && guest_size <= kMaxBytes && levels > 1;
}

enum class When { Wait, Now, Never };

// A noted texture at a frame's start. Never: the GPU's contents are gone (uploaded over, or only a clear, which a font
// cache's glyphs follow from the CPU), it was drawn again in a later frame (a render target in use), its texels went
// into memory for this drawing already, or another image whose storage overlaps its own was drawn in that frame or
// since (other_written: the newest such drawing's frame, 0 for none; that memory is the other image's now). Now: drawn
// in one frame, kFrames ago or more
inline When due(uint64_t frame, uint64_t first_written, uint64_t last_written, uint64_t done_for, bool gpu_written,
                bool only_cleared, uint64_t other_written) {
    if (!gpu_written || only_cleared || done_for == last_written || first_written != last_written) return When::Never;
    if (other_written && other_written >= last_written) return When::Never;
    return frame >= last_written + kFrames ? When::Now : When::Wait;
}

// The write-back itself goes only into memory that is still as the drawing left it, read right before the write: all
// zero (the game clears a tile's slot before it draws the tile), or the texture's own texels from its last write-back
// (a tile drawn again with no clear between). Anything else there was written by the CPU since, and stays: the page
// generations the write-back otherwise goes by know a write only once its capture was taken, and old texels over
// newer scene data crashed the game before
inline bool all_zero(const uint8_t* p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w) return false;
    }
    for (; i < n; i++)
        if (p[i]) return false;
    return true;
}
inline uint64_t hash(const uint8_t* p, size_t n) {   // (FNV-1a over 8-byte words; 0 stands for "none", so never returned)
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h ? h : 1;
}
inline bool may_write(const uint8_t* now, size_t n, uint64_t own_hash) {
    return all_zero(now, n) || (own_hash && hash(now, n) == own_hash);
}

// The write is a compare and write, a word at a time: memory that still holds `from` (what the check above saw) takes
// `to`, and a word a guest thread wrote in between keeps the guest's bytes. The game's threads run while the replay
// works, so a check of a snapshot alone leaves the time up to the write open. Returns the bytes that were left.
// went (n bytes), when given: 1 for each byte this call put in. Called a second time with from and to exchanged and that
// as `only`, it takes the texels out again where it put them and they still are: a write-back that met a guest write is
// undone as a whole, and the memory is as the guest has it. Only those: a guest write of the texel's own value, which
// the first call left, stays
inline size_t swap_words(uint8_t* mem, const uint8_t* from, const uint8_t* to, size_t n, uint8_t* went = nullptr,
                         const uint8_t* only = nullptr) {
    size_t left = 0, i = 0;
    const auto byte = [&] {
        uint8_t was = from[i];
        if (was != to[i] && (!only || only[i])) {
            const bool in = std::atomic_ref<uint8_t>(mem[i]).compare_exchange_strong(was, to[i]);
            if (!in) left++;
            if (went) went[i] = in;
        }
        i++;
    };
    while (i < n && ((uintptr_t)(mem + i) & 7)) byte();
    for (; i + 8 <= n; i += 8) {
        uint64_t was, now;
        std::memcpy(&was, from + i, 8);
        std::memcpy(&now, to + i, 8);
        if (was == now || (only && !only[i])) continue;
        const bool in = std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(mem + i)).compare_exchange_strong(was, now);
        if (!in) left += 8;
        if (went) std::memset(went + i, in, 8);
    }
    while (i < n) byte();
    return left;
}

} // namespace drawn_writeback
