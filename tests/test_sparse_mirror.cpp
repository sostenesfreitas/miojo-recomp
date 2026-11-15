// tests/test_sparse_mirror.py: gpu/vk/sparse_mirror.h, the blocks of a sparse VRAM mirror that hold memory
#include "../gpu/vk/sparse_mirror.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) failures++, std::printf("FAIL: %s\n", what);
}
struct Pool { uint64_t lo, hi; };
constexpr uint64_t K = 64 * 1024;
std::vector<uint8_t> live_of(uint64_t lo, uint64_t hi, std::vector<Pool> pools) {
    return sparse_mirror::live_blocks(lo, hi, K, pools.begin(), pools.end());
}
}  // namespace

int main() {
    // a mirror of 8 blocks; the game's pools in blocks 1 and 3-4 (the second crosses a block's end)
    const uint64_t lo = 0x10000000;
    auto live = live_of(lo, lo + 8 * K, {{lo + K + 100, lo + K + 200}, {lo + 3 * K + 4096, lo + 5 * K - 4096}});
    check(live == std::vector<uint8_t>({0, 1, 0, 1, 1, 0, 0, 0}), "the blocks a pool is in, and only those");

    // a pool that starts far below the mirror and reaches into it, with a later small one before the mirror
    live = live_of(lo, lo + 4 * K, {{lo - 64 * K, lo + 2 * K - 1}, {lo - 4 * K, lo - 2 * K}});
    check(live == std::vector<uint8_t>({1, 1, 0, 0}), "an earlier, longer pool still counts");

    // a pool past the mirror's end, one ending at its start, an empty one
    live = live_of(lo, lo + 2 * K, {{lo - K, lo}, {lo + K, lo + K}, {lo + 2 * K, lo + 3 * K}});
    check(live == std::vector<uint8_t>({0, 0}), "pools touching the ends but not in it, and an empty one, reach nothing");

    // a mirror whose size is not whole blocks: its last block is still a block
    live = live_of(lo, lo + K + 4096, {{lo + K, lo + K + 4096}});
    check(live.size() == 2 && live[1] == 1 && live[0] == 0, "a last partial block");
    check(live_of(lo, lo, {{lo, lo + K}}).empty(), "an empty mirror has no blocks");

    // block_span
    size_t a = 0, e = 0;
    sparse_mirror::block_span(K - 1, 2, K, 8, &a, &e);
    check(a == 0 && e == 2, "a range across a block's end reaches both");
    sparse_mirror::block_span(3 * K, K, K, 8, &a, &e);
    check(a == 3 && e == 4, "a whole block reaches it alone");
    sparse_mirror::block_span(7 * K, 4 * K, K, 8, &a, &e);
    check(a == 7 && e == 8, "clamped to the mirror's blocks");
    sparse_mirror::block_span(2 * K, 0, K, 8, &a, &e);
    check(a == e, "an empty range reaches none");

    // runs
    const std::vector<uint8_t> has = {1, 1, 0, 1, 0, 0, 1, 1};
    std::vector<std::pair<size_t, size_t>> got;
    sparse_mirror::runs(has.size(), [&](size_t i) { return has[i]; }, true, [&](size_t f, size_t c) { got.push_back({f, c}); });
    check(got == std::vector<std::pair<size_t, size_t>>({{0, 2}, {3, 1}, {6, 2}}), "runs of blocks with memory");
    got.clear();
    sparse_mirror::runs(has.size(), [&](size_t i) { return has[i]; }, false, [&](size_t f, size_t c) { got.push_back({f, c}); });
    check(got == std::vector<std::pair<size_t, size_t>>({{2, 1}, {4, 2}}), "runs of blocks with none");

    // clip: a copy from one mirror into another, cut where either side's block has no memory
    const auto src_has = [&](uint64_t o) { return has[o / K] != 0; };
    const auto all = [](uint64_t) { return true; };
    auto out = sparse_mirror::clip({{0, 0, 8 * K}}, K, src_has, all);
    check(out.size() == 3 && out[0].src == 0 && out[0].size == 2 * K && out[1].src == 3 * K && out[1].size == K &&
              out[2].src == 6 * K && out[2].size == 2 * K,
          "a whole copy keeps the blocks with memory, merged");
    // offsets that differ between source and destination: cut at both sides' block ends
    out = sparse_mirror::clip({{K / 2, 0, 2 * K}}, K, all, [&](uint64_t d) { return d < K; });
    check(out.size() == 1 && out[0].src == K / 2 && out[0].dst == 0 && out[0].size == K,
          "the destination's block with none cut off, though the source's block end is elsewhere");
    out = sparse_mirror::clip({{0, 0, 3 * K}}, K, [&](uint64_t s) { return s / K != 1; }, all);
    check(out.size() == 2 && out[0].size == K && out[1].src == 2 * K && out[1].size == K, "a hole in the middle");
    out = sparse_mirror::clip({{4096, 4096, 100}}, K, all, all);
    check(out.size() == 1 && out[0].src == 4096 && out[0].size == 100, "a small copy inside one block, kept whole");
    out = sparse_mirror::clip({{0, 0, K}}, K, [](uint64_t) { return false; }, all);
    check(out.empty(), "nothing from a source with no memory");

    // cut_src: a pending refill's range cut out of the copies by their source offsets
    out = sparse_mirror::cut_src({{0, 100, 3 * K}}, {{K, 2 * K}});
    check(out.size() == 2 && out[0].src == 0 && out[0].dst == 100 && out[0].size == K && out[1].src == 2 * K &&
              out[1].dst == 100 + 2 * K && out[1].size == K,
          "a range in the middle cut out, the destinations kept in step");
    out = sparse_mirror::cut_src({{0, 0, K}, {2 * K, 2 * K, K}}, {{K / 2, 5 * K / 2}});
    check(out.size() == 2 && out[0].size == K / 2 && out[1].src == 5 * K / 2 && out[1].dst == 5 * K / 2 && out[1].size == K / 2,
          "a range over two copies' ends");
    out = sparse_mirror::cut_src({{0, 0, K}}, {{0, K}});
    check(out.empty(), "a range over the whole copy");
    out = sparse_mirror::cut_src({{0, 0, K}}, {{2 * K, 3 * K}, {K, K}});
    check(out.size() == 1 && out[0].size == K, "ranges past it, and an empty one, leave it whole");
    if (failures) return 1;
    std::printf("sparse mirror blocks: ok\n");
    return 0;
}
