#include "../gpu/vk/dlss_actor.h"

#include <array>
#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace da = dlss_actor;
    // names: the same draw keeps its name, another draw of the same model is the next one, another model another name
    const uint64_t a0 = da::key(0x1000, 0x2000, 9702, 0, 0xabc, 0), a1 = da::key(0x1000, 0x2000, 9702, 0, 0xabc, 1);
    CHECK("the same draw, the same name", a0 == da::key(0x1000, 0x2000, 9702, 0, 0xabc, 0));
    CHECK("the second draw of a model is another", a0 != a1 && da::same_model(a0, a1));
    CHECK("another model", !da::same_model(a0, da::key(0x1000, 0x2000, 9702, 3, 0xabc, 0)) &&
                               !da::same_model(a0, da::key(0x1040, 0x2000, 9702, 0, 0xabc, 0)) &&
                               !da::same_model(a0, da::key(0x1000, 0x2000, 9702, 0, 0xabd, 0)));
    CHECK("many draws of one model", da::same_model(a0, da::key(0x1000, 0x2000, 9702, 0, 0xabc, 900)));

    // where a draw's indices start for the shader, which reads 32-bit words: the address 4-aligned down, the first index in it
    uint64_t at = 0;
    uint32_t first = 0;
    da::index_start(0x10002, 1, &at, &first);
    CHECK("16-bit at +2: the second half of the word", at == 0x10000 && first == 1);
    da::index_start(0x10003, 0, &at, &first);
    CHECK("8-bit at +3: the fourth byte", at == 0x10000 && first == 3);
    da::index_start(0x10008, 2, &at, &first);
    CHECK("32-bit, aligned", at == 0x10008 && first == 0);
    da::index_start(0x10004, 1, &at, &first);
    CHECK("16-bit, aligned", at == 0x10004 && first == 0);

    // copies of a model pair with the frame before's copies by place, all at once, the nearest pairs first, within a reach
    using P = da::Placed;
    const auto same = [](const std::vector<int>& a, std::vector<int> b) { return a == b; };
    // (the four Gossip Stones: 2.3 units apart in a row, still)
    const std::vector<P> stones = {{{39.4f, 102.8f, -11.9f}, 0}, {{39.4f, 102.8f, -9.6f}, 1}, {{39.4f, 102.8f, -7.3f}, 2},
                                   {{39.4f, 102.8f, -5.0f}, 3}};
    CHECK("still copies in the same order: themselves", same(da::pair_by_place(stones, stones, 1.0f), {0, 1, 2, 3}));
    const std::vector<P> turned = {stones[2], stones[0], stones[3], stones[1]};
    CHECK("still copies drawn in another order: themselves", same(da::pair_by_place(turned, stones, 1.0f), {2, 0, 3, 1}));
    // (run C, 19.58 s: the first stone comes into view; the frame before had the other three only)
    const std::vector<P> three = {{{39.4f, 102.8f, -9.6f}, 0}, {{39.4f, 102.8f, -7.3f}, 1}, {{39.4f, 102.8f, -5.0f}, 2}};
    CHECK("a copy new in view takes no neighbour's place", same(da::pair_by_place(stones, three, 1.0f), {-1, 0, 1, 2}));
    CHECK("... nor with a reach wider than the row", same(da::pair_by_place(stones, three, 100.0f), {-1, 0, 1, 2}));
    // (a stone drawn by the other model in the frame before: this model's only copy then is another stone)
    const std::vector<P> lone_now = {stones[3]}, lone_then = {stones[1]};
    CHECK("a lone copy further than the reach: none", same(da::pair_by_place(lone_now, lone_then, 1.0f), {-1}));
    const std::vector<P> moved = {{{39.9f, 102.8f, -5.2f}, 0}};
    CHECK("a lone copy that moved a little: it", same(da::pair_by_place(moved, lone_now, 1.0f), {0}));
    // (two of one model passing each other, drawn in the other order than the frame before)
    const std::vector<P> walkers_then = {{{0.1f, 0, 0}, 0}, {{0.4f, 0, 0}, 1}}, walkers_now = {{{0.5f, 0, 0}, 0}, {{0.0f, 0, 0}, 1}};
    CHECK("two passing each other: each itself", same(da::pair_by_place(walkers_now, walkers_then, 1.0f), {1, 0}));
    // (the same geometry drawn twice for one object: both copies at one place, each keeps its own n-th)
    const std::vector<P> twice = {{{5, 5, 5}, 0}, {{5, 5, 5}, 1}}, twice_swapped = {{{5, 5, 5}, 1}, {{5, 5, 5}, 0}};
    CHECK("a tie at one place: the same n-th", same(da::pair_by_place(twice, twice, 1.0f), {0, 1}) &&
                                                   same(da::pair_by_place(twice, twice_swapped, 1.0f), {1, 0}));
    const std::vector<P> once = {{{5, 5, 5}, 0}};
    CHECK("a tie at one place, its own n-th gone: the other", same(da::pair_by_place(once, {twice[1]}, 1.0f), {0}));
    const std::vector<P> lost = {{{std::nanf(""), 0, 0}, 0}};
    CHECK("a place that is not finite: none", same(da::pair_by_place(lost, once, 1.0f), {-1}));
    CHECK("none then, none now", da::pair_by_place(stones, {}, 1.0f) == std::vector<int>(4, -1) &&
                                     da::pair_by_place({}, stones, 1.0f).empty());

    // a draw's place: its origin plus its bones' mean translation (copies at one origin, placed by their bones, differ)
    const auto pose_of = [](float ox, float oy, float oz, std::vector<std::array<float, 3>> moves) {
        std::vector<uint8_t> p(16 + moves.size() * 48, 0);
        const float o[4] = {ox, oy, oz, 0.0f};
        std::memcpy(p.data(), o, sizeof o);
        for (size_t b = 0; b < moves.size(); b++) {
            const float m[12] = {1, 0, 0, moves[b][0], 0, 1, 0, moves[b][1], 0, 0, 1, moves[b][2]};
            std::memcpy(p.data() + 16 + b * 48, m, sizeof m);
        }
        return p;
    };
    float place[3];
    const auto by_origin = pose_of(100, 0, 5, {{0, 0, 0}, {0, 2, 0}});
    da::place_of(by_origin.data(), 2, place);
    CHECK("placed by its origin", place[0] == 100 && place[1] == 1 && place[2] == 5);
    const auto by_bones = pose_of(0, 0, 0, {{300, 0, 7}, {300, 4, 7}});
    da::place_of(by_bones.data(), 2, place);
    CHECK("placed by its bones, at a shared origin", place[0] == 300 && place[1] == 2 && place[2] == 7);
    auto bad = pose_of(10, 0, 0, {{2, 0, 0}, {0, 0, 0}});
    const float nan = std::nanf("");
    std::memcpy(bad.data() + 16 + 48 + 3 * 4, &nan, 4);
    da::place_of(bad.data(), 2, place);
    CHECK("a bone that is not finite is left out", place[0] == 12 && place[1] == 0);
    da::place_of(by_origin.data(), 0, place);
    CHECK("no bones: the origin", place[0] == 100 && place[1] == 0 && place[2] == 5);
    std::puts("dlss_actor: ok");
    return 0;
}
