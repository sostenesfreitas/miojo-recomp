// target_overlap::DrawnOver (gpu/vk/target_overlap.h): what newer render targets have drawn over one's storage.
// tests/test_target_overlap.py
#include "../gpu/vk/target_overlap.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::target_overlap::DrawnOver;
    // (OoT's 768 x 432 RGBA16F transient targets: 2.6 MB each, made ~1.2 MB apart in a scratch heap)
    const uint64_t size = 768ull * 432 * 8, step = 0x128200;
    const uint64_t a = 0x10005db1600, b = a + step;
    const auto one = [&](uint64_t lo, uint64_t hi, uint64_t last_used, uint64_t olo, uint64_t ohi, uint64_t written) {
        DrawnOver o;
        o.add(lo, hi, last_used, olo, ohi, written);
        return o.an_eighth(hi - lo);
    };

    CHECK("a newer target from a shifted start kills it", one(a, a + size, 100, b, b + size, 101));
    CHECK("and one that starts before it", one(b, b + size, 100, a, a + size, 101));
    CHECK("not when it was written before the last use", !one(a, a + size, 100, b, b + size, 100));
    CHECK("not when it was written before the last use (older)", !one(a, a + size, 100, b, b + size, 40));
    CHECK("the same start is drawn_over's own case, not this one", !one(a, a + size, 100, a, a + size / 2, 101));
    CHECK("a view inside it is not a new target", !one(a, a + size, 100, a + 4096, a + size - 4096, 101));
    CHECK("nor a small view inside it", !one(a, a + size, 100, a + 4096, a + 4096 + size / 16, 101));
    CHECK("one covering it whole is", one(a + 4096, a + size, 100, a, a + 2 * size, 101));
    CHECK("an overlap of an eighth is enough", one(a, a + 8 * 4096, 100, a + 7 * 4096, a + 20 * 4096, 101));
    CHECK("less than an eighth is not", !one(a, a + 8 * 4096, 100, a + 7 * 4096 + 1, a + 20 * 4096, 101));
    {   // (two newer targets over its edges, each under an eighth of 64 KB: an eighth together)
        DrawnOver o;
        const uint64_t end = a + 64 * 1024;
        o.add(a, end, 100, a - 4 * 1024, a + 4 * 1024, 101);   // (4 KB at its start)
        CHECK("several parts under an eighth are not enough before the second", !o.an_eighth(64 * 1024));
        o.add(a, end, 100, end - 4 * 1024, end + 4 * 1024, 101);   // (4 KB more, at its end: 8 KB together)
        CHECK("several newer targets over parts of it add up", o.an_eighth(64 * 1024));
    }
    {   // (a view inside it adds nothing to a shifted overlap, and two shifted edges still add up around it.
        // keep_finalized's 260 x 260 normal maps are drawn, finalized and later sampled again: a view of one is not a
        // new target and must not lose it the keep)
        DrawnOver o;
        const uint64_t end = a + size;
        o.add(a, end, 100, a - size / 16, a + size / 16, 101);   // (1/16 at its start)
        o.add(a, end, 100, a + 4096, a + size / 2, 101);   // (a view inside it: nothing)
        CHECK("a view inside it adds nothing to a shifted overlap", !o.an_eighth(size));
        o.add(a, end, 100, end - size / 16, end + size / 16, 101);   // (1/16 more, at its end)
        CHECK("shifted overlaps over its edges add up around views", o.an_eighth(size));
    }
    {   // (two that start before it and overlap: the overlap counts once, 5 KB not 9)
        DrawnOver o;
        const uint64_t end = a + 64 * 1024;
        o.add(a, end, 100, a - 5 * 1024, a + 5 * 1024, 101);
        o.add(a, end, 100, a - 2 * 1024, a + 4 * 1024, 101);
        CHECK("parts overlapping are merged, not counted twice", !o.an_eighth(64 * 1024));
    }
    CHECK("apart is not", !one(a, a + size, 100, a + size, a + 2 * size, 101));
    CHECK("empty ranges are not", !one(a, a, 100, a - 1, a + size, 101) && !one(a, a + size, 100, b, b, 101));
    std::puts("target overlap: all checks passed");
}
