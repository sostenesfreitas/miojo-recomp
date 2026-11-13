// drawn_writeback (gpu/vk/drawn_writeback.h): which textures the GPU drew go into guest memory, and when.
// tests/test_drawn_writeback.py
#include "../gpu/vk/drawn_writeback.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    using drawn_writeback::due;
    using drawn_writeback::noted;
    using drawn_writeback::When;
    const uint64_t tile = 214016;   // (260 x 260 RG8, nine levels, as the game lays it out: 209 KB)

    expect(noted(0x116aec2000ull, tile, 9), "a terrain normal tile");
    expect(!noted(0x116aec2000ull, tile, 1), "one level: a frame's render target, not a baked map");
    expect(!noted(0x116aec2000ull, 8ull << 20, 9), "8 MB with a mip chain: too large");
    expect(noted(0x116aec2000ull, 1ull << 20, 2), "1 MB with two levels: the limit itself");
    expect(!noted(0x10003c78600ull, tile, 9), "a virtual pool's texture");
    expect(!noted(0x116aec2000ull, 0, 9), "no storage");

    // (frame, first_written, last_written, done_for, gpu_written, only_cleared, other_written)
    expect(due(100, 100, 100, 0, true, false, 0) == When::Wait, "the frame it was drawn in");
    expect(due(101, 100, 100, 0, true, false, 0) == When::Wait, "one frame later");
    expect(due(102, 100, 100, 0, true, false, 0) == When::Now, "two frames later");
    expect(due(500, 100, 100, 0, true, false, 0) == When::Now, "long after, never written back");
    expect(due(102, 100, 100, 100, true, false, 0) == When::Never, "written back for this drawing already");
    expect(due(302, 300, 300, 100, true, false, 0) == When::Now, "drawn anew after a finalize: written back again");
    expect(due(102, 99, 100, 0, true, false, 0) == When::Never, "drawn in two frames: a render target in use");
    expect(due(102, 100, 100, 0, false, false, 0) == When::Never, "uploaded over since: its memory is its content");
    expect(due(102, 100, 100, 0, true, true, 0) == When::Never, "only cleared: a font cache, its glyphs come from the CPU");
    expect(due(102, 100, 100, 0, true, false, 101) == When::Never, "another image drawn over its storage the frame after");
    expect(due(102, 100, 100, 0, true, false, 100) == When::Never, "another image drawn over its storage in the same frame");
    expect(due(102, 100, 100, 0, true, false, 40) == When::Now, "another image there, drawn before it: its own texels are the newest");
    expect(due(101, 100, 100, 0, true, false, 40) == When::Wait, "the same, a frame early");
    using drawn_writeback::all_zero;
    uint8_t mem[4096] = {};
    expect(all_zero(mem, sizeof mem), "a cleared slot: the texels go in");
    expect(all_zero(mem, 0), "nothing to look at");
    mem[4095] = 1;
    expect(!all_zero(mem, sizeof mem), "one byte the CPU wrote, at the end: left alone");
    expect(all_zero(mem, sizeof mem - 1), "that byte past the texture's own");
    mem[4095] = 0, mem[0] = 0x80;
    expect(!all_zero(mem, sizeof mem), "one byte at the start");
    using drawn_writeback::hash;
    using drawn_writeback::may_write;
    uint8_t texels[64], other[64];
    for (int i = 0; i < 64; i++) texels[i] = (uint8_t)(0x7f + (i & 1)), other[i] = (uint8_t)i;
    const uint64_t own = hash(texels, sizeof texels);
    uint8_t zeros[64] = {};
    expect(own != 0 && hash(zeros, sizeof zeros) != 0, "a hash is never the value that stands for none");
    uint8_t odd[13] = {}, odd_last[13] = {};
    odd_last[12] = 1;
    expect(drawn_writeback::all_zero(odd, sizeof odd) && !drawn_writeback::all_zero(odd_last, sizeof odd_last),
           "a byte past the last whole word is seen");
    expect(hash(odd, sizeof odd) != hash(odd_last, sizeof odd_last), "and hashed");
    expect(may_write(zeros, sizeof zeros, 0), "cleared memory, first write-back");
    expect(may_write(zeros, sizeof zeros, own), "cleared memory, a later drawing");
    expect(may_write(texels, sizeof texels, own), "its own last write-back still there: drawn again with no clear");
    expect(!may_write(texels, sizeof texels, 0), "bytes there and no write-back of its own: the CPU's");
    expect(!may_write(other, sizeof other, own), "other bytes over its last write-back: the CPU's since");

    // the write itself: compare and write against the snapshot the check saw
    using drawn_writeback::swap_words;
    alignas(8) uint8_t slot[64] = {};
    uint8_t snap[64] = {}, tex[64];
    for (int i = 0; i < 64; i++) tex[i] = (uint8_t)(i + 1);
    expect(swap_words(slot, snap, tex, 64) == 0 && !std::memcmp(slot, tex, 64), "untouched memory takes every texel");
    expect(swap_words(slot, tex, snap, 64) == 0 && all_zero(slot, 64), "and gives them back when asked");

    slot[17] = 0x42;   // (a guest thread's write after the snapshot was taken)
    expect(swap_words(slot, snap, tex, 64) == 8, "a word the guest wrote since is counted as left");
    expect(slot[17] == 0x42 && slot[16] == 0 && slot[23] == 0, "and keeps the guest's bytes, all eight");
    expect(slot[15] == tex[15] && slot[24] == tex[24], "the words beside it went in");
    expect(swap_words(slot, tex, snap, 64) == 8, "the undo leaves that word too");
    slot[17] = 0;
    expect(all_zero(slot, 64), "and takes out everything the write put in: the memory is the guest's alone");

    std::memset(slot, 0, 64);
    swap_words(slot, snap, tex, 64);
    slot[40] = 0x99;   // (a guest write that landed after the texels)
    swap_words(slot, tex, snap, 64);
    expect(slot[40] == 0x99 && slot[41] == tex[41], "the undo does not touch a word the guest wrote after the texels");
    slot[40] = 0, std::memset(slot + 41, 0, 7);
    expect(all_zero(slot, 64), "(the rest of that undo was complete)");

    std::memset(slot, 0, 64);   // (not on a word's edge, and not whole words: the bytes at both ends one at a time)
    expect(swap_words(slot + 3, snap, tex, 19) == 0 && !std::memcmp(slot + 3, tex, 19), "an odd start and length");
    expect(slot[2] == 0 && slot[22] == 0, "nothing outside the range");
    slot[3] = 0x55, slot[21] = 0x66;
    std::memset(slot + 4, 0, 17);
    expect(swap_words(slot + 3, snap, tex, 19) == 2, "guest bytes at both odd ends are left");
    expect(slot[3] == 0x55 && slot[21] == 0x66 && slot[4] == tex[1] && slot[20] == tex[17], "and only those");

    // (a guest thread writes the texel's own value after the check: the write leaves that word, so the undo must too)
    std::memset(slot, 0, 64);
    std::memcpy(slot + 8, tex + 8, 8);
    uint8_t went[64] = {};
    expect(swap_words(slot, snap, tex, 64, went) == 8 && !went[8] && went[0] && went[16], "the guest's word is not marked as gone in");
    expect(swap_words(slot, tex, snap, 64, nullptr, went) == 0, "the undo tries the marked words only");
    expect(!std::memcmp(slot + 8, tex + 8, 8), "and keeps the guest's write, though it holds the texels' value");
    std::memset(slot + 8, 0, 8);
    expect(all_zero(slot, 64), "(the rest of that undo was complete)");

    uint8_t same_from[16] = {7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0, 0, 0, 0, 0, 0}, same_to[16] = {7, 7, 7, 7, 7, 7, 7, 7, 1, 2, 3, 4, 5, 6, 7, 8};
    alignas(8) uint8_t pad[16] = {9, 9, 9, 9, 9, 9, 9, 9, 0, 0, 0, 0, 0, 0, 0, 0};
    expect(swap_words(pad, same_from, same_to, 16) == 0 && pad[0] == 9 && pad[8] == 1,
           "a word the texels do not change (padding) is not written, whatever is there");

    if (failures) return EXIT_FAILURE;
    std::printf("drawn writeback: all checks passed\n");
    return EXIT_SUCCESS;
}
