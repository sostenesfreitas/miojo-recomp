// The import trim's rules (gpu/vk/mirror_trim.h): tests/test_mirror_trim.py compiles and runs this.
#include "../gpu/vk/mirror_trim.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct GuestPool {
    uint64_t lo, hi;
};

int main() {
    using namespace mirror_trim;
    const uint64_t MB = 1ull << 20, chunk = 4 * MB;
    {   // the Market entered from Hyrule Field: the field's 224 MB import with the Market's 45 MB pool inside it
        const uint64_t lo = 0x108c000000, hi = 0x109a000000;
        const std::vector<GuestPool> pools{{0x1000000000, 0x1001000000}, {0x108c711000, 0x108c711000 + 45 * MB}, {0x10a0000000, 0x10a1000000}};
        const auto parts = live(lo, hi, chunk, kJoin, pools);
        CHECK(parts.size() == 1);
        CHECK(parts[0].lo == 0x108c400000 && parts[0].hi == 0x108f800000);   // (out to its 4 MB chunks)
        CHECK(bytes(parts) == 52 * MB);
        CHECK(worth(hi - lo, parts));
    }
    {   // an import its game pools fill, or nearly: left alone
        const uint64_t lo = 0x1178400000, hi = lo + 104 * MB;
        const std::vector<GuestPool> full{{lo + 0x1000, hi - 0x1000}};
        CHECK(!worth(hi - lo, live(lo, hi, chunk, kJoin, full)));
        const std::vector<GuestPool> nearly{{lo, lo + 88 * MB}};   // (16 MB dead: under kMinDead)
        CHECK(!worth(hi - lo, live(lo, hi, chunk, kJoin, nearly)));
        const std::vector<GuestPool> fifth{{lo, lo + 500 * MB}};   // (a pool past its end: clipped to the import)
        CHECK(bytes(live(lo, hi, chunk, kJoin, fifth)) == hi - lo);
    }
    {   // a large import with a small dead share: at least kMinDead, but under a quarter
        const uint64_t lo = 0x7fb0000000, hi = lo + 200 * MB;
        const std::vector<GuestPool> pools{{lo, lo + 160 * MB}};
        const auto parts = live(lo, hi, chunk, kJoin, pools);
        CHECK(bytes(parts) == 160 * MB && !worth(hi - lo, parts));
    }
    {   // no game pool in it at all: nothing binds it, its mirror goes by the idle rules, not here
        const uint64_t lo = 0x1072c00000, hi = lo + 68 * MB;
        const std::vector<GuestPool> pools{{0x1000000000, 0x1001000000}, {hi, hi + 8 * MB}, {lo - 8 * MB, lo}};
        const auto parts = live(lo, hi, chunk, kJoin, pools);
        CHECK(parts.empty() && !worth(hi - lo, parts));
    }
    {   // live parts: near ones are one import, far ones their own; overlapping game pools count once
        const uint64_t lo = 0x7f00000000, hi = lo + 256 * MB;
        const std::vector<GuestPool> pools{{lo + 1 * MB, lo + 9 * MB},     // (chunks 0..12 MB)
                                           {lo + 2 * MB, lo + 6 * MB},     // (inside the first)
                                           {lo + 18 * MB, lo + 19 * MB},   // (chunk 16..20: 4 MB from the first part, joined)
                                           {lo + 100 * MB, lo + 110 * MB}, // (chunks 100..112: far)
                                           {lo + 250 * MB, lo + 300 * MB}};   // (clipped at the import's end)
        const auto parts = live(lo, hi, chunk, kJoin, pools);
        CHECK(parts.size() == 3);
        CHECK(parts[0].lo == lo && parts[0].hi == lo + 20 * MB);
        CHECK(parts[1].lo == lo + 100 * MB && parts[1].hi == lo + 112 * MB);
        CHECK(parts[2].lo == lo + 248 * MB && parts[2].hi == hi);
        CHECK(bytes(parts) == 40 * MB && worth(hi - lo, parts));
        // with a join of 0 the two near parts stay apart; touching ones are still one
        CHECK(live(lo, hi, chunk, 0, pools).size() == 4);
    }
    {   // too many live parts: each would be an import of its own
        const uint64_t lo = 0x7e00000000, hi = lo + 512 * MB;
        std::vector<GuestPool> pools;
        for (uint64_t i = 0; i < 6; i++) pools.push_back({lo + i * 64 * MB, lo + i * 64 * MB + 4 * MB});
        const auto parts = live(lo, hi, chunk, kJoin, pools);
        CHECK(parts.size() == 6 && !worth(hi - lo, parts));
    }
    {   // no chunks (imports by whole pool): nothing to trim to
        const std::vector<GuestPool> pools{{0x1000, 0x2000}};
        CHECK(live(0, 64 * MB, 0, kJoin, pools).empty());
    }
    // the wait grows fourfold each time a range is trimmed again, then it is left alone
    CHECK(wait(0) == kWait && wait(1) == 4 * kWait && wait(2) == 16 * kWait && wait(kMaxTrims) == 0 && wait(200) == 0);
    // a range's trims count only while they are recent: a scene change minutes later starts over
    CHECK(counted(2, 100) == 2 && counted(3, kForget) == 3 && counted(3, kForget + 1) == 0 && counted(0, 5) == 0);
    CHECK(wait(counted(kMaxTrims, 1000)) == 0 && wait(counted(kMaxTrims, kForget + 1)) == kWait);
    // only while VRAM is short
    CHECK(!pressed(5000 * MB, 7000 * MB) && pressed(5700 * MB, 7000 * MB) && !pressed(5700 * MB, 0));
    std::printf("mirror_trim: ok\n");
    return 0;
}
