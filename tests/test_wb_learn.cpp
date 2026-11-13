// The learned write-back cap's rules (gpu/vk/wb_learn.h): tests/test_wb_learn.py compiles and runs this.
#include "../gpu/vk/wb_learn.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

using Keep = std::vector<std::pair<uint64_t, uint64_t>>;

// (each call a submission of its own, unless one is given)
static uint64_t sub_n = 0;
static bool P(wb_learn::Table& t, uint64_t lo, uint64_t hi, uint64_t cap, Keep& keep) { return t.plan(lo, hi, cap, ++sub_n, keep); }

int main() {
    using namespace wb_learn;
    const uint64_t KB = 1ull << 10, MB = 1ull << 20, cap = 1024 * KB;
    Keep keep;
    // the cap: unset or 0 is none (the default: every binding written back), a number that many KB
    CHECK(max_bytes(nullptr) == 0 && max_bytes("") == 0);
    CHECK(max_bytes("0") == 0 && max_bytes("1024") == 1024 * KB);
    CHECK(max_bytes("4096") == 4096 * KB);

    {   // a small binding is always written back whole, and so is every one with no cap
        Table t;
        for (int i = 0; i < 20; i++) CHECK(!P(t, 0x1000000, 0x1000000 + 64 * KB, cap, keep));
        for (int i = 0; i < 20; i++) CHECK(!P(t, 0x2000000, 0x2000000 + 15 * MB, 0, keep));
        CHECK(!P(t, 0x2000000, 0x2000000 + cap, cap, keep));   // (exactly the cap: written back)
    }
    {   // a large binding the CPU never touches: written back whole for its probation, then left out entirely
        Table t;
        const uint64_t lo = 0x117b427000, hi = lo + 0xf33080;   // (Hyrule Field's 15 MB pair)
        for (uint32_t i = 0; i < t.probation; i++) CHECK(!P(t, lo, hi, cap, keep));
        // (it ends 0x80 bytes into a page shared with what follows: that page alone is kept)
        CHECK(P(t, lo, hi, cap, keep) && keep.size() == 1 && keep[0].first == hi / 4096 * 4096 && keep[0].second == hi);
        CHECK(P(t, lo, hi, cap, keep) && keep.size() == 1);
        CHECK(!t.read(hi - 0x40, hi + 0x200));   // (the CPU's write in that page: known, nothing stale)
        // another binding at the same start (the pool reused): its own probation
        CHECK(!P(t, lo, hi + 4 * MB, cap, keep));
    }
    {   // a binding page-aligned at both ends, nobody touching it: left out entirely
        Table t;
        const uint64_t lo = 0x1179e9c000, hi = lo + 0x654000;
        for (uint32_t i = 0; i < t.probation; i++) CHECK(!P(t, lo, hi, cap, keep));
        CHECK(P(t, lo, hi, cap, keep) && keep.empty());
    }
    {   // a binding that starts and ends inside pages: both edge pages kept, clipped to it
        Table t;
        const uint64_t lo = 0x117df8b040, hi = lo + 0x44b040;
        for (uint32_t i = 0; i < t.probation; i++) CHECK(!P(t, lo, hi, cap, keep));
        CHECK(P(t, lo, hi, cap, keep) && keep.size() == 2);
        CHECK(keep[0].first == lo && keep[0].second == lo / 4096 * 4096 + 4096);
        CHECK(keep[1].first == hi / 4096 * 4096 && keep[1].second == hi);
    }
    {   // a large binding the CPU reads in full after every write (the 4.3 MB buffers): always written back in full
        Table t;
        const uint64_t lo = 0x117db40000, hi = lo + 0x44b040;
        for (int i = 0; i < 40; i++) {
            const bool part = P(t, lo, hi, cap, keep);
            CHECK(!part || (keep.size() == 1 && keep[0].first == lo && keep[0].second == hi));
            CHECK(!t.read(lo, hi));   // (nothing new after the first: nothing stale)
        }
    }
    {   // the CPU writes a few pages of a large binding during its probation: after it, those pages alone are written back
        Table t;
        const uint64_t lo = 0x1178000000, hi = lo + 0x654040;
        for (uint32_t i = 0; i < t.probation; i++) {
            CHECK(!P(t, lo, hi, cap, keep));
            CHECK(t.followed(lo + 0x3000, lo + 0x4000));
            CHECK(!t.read(lo + 0x3000, lo + 0x4000));   // (a page the CPU writes every frame)
            CHECK(!t.read(lo + 0x200000, lo + 0x202000));
        }
        CHECK(P(t, lo, hi, cap, keep));
        CHECK(keep.size() == 3);   // (the two touched ranges, and the last page it shares)
        CHECK(keep[0].first == lo + 0x3000 && keep[0].second == lo + 0x4000);
        CHECK(keep[1].first == lo + 0x200000 && keep[1].second == lo + 0x202000);
        CHECK(keep[2].first == lo + 0x654000 && keep[2].second == hi);
        // a touch that reaches past the binding is clipped to it
        CHECK(t.read(hi - 0x1000, hi + 0x1000));   // (new, in a binding planned in part: its last write-back left it out)
        CHECK(P(t, lo, hi, cap, keep) && keep.size() == 3 && keep[2].first == hi - 0x1000 && keep[2].second == hi);
        CHECK(!t.read(hi - 0x800, hi));   // (known: nothing stale)
    }
    {   // a range read before its binding is first written is written back when it is planned
        Table t;
        const uint64_t lo = 0x1163f6e000, hi = lo + 0x7e9000;
        CHECK(!t.read(lo + 0x100000, lo + 0x100040));
        for (uint32_t i = 0; i < t.probation; i++) CHECK(!P(t, lo, hi, cap, keep));
        CHECK(P(t, lo, hi, cap, keep) && keep.size() == 1 && keep[0].first == lo + 0x100000 && keep[0].second == lo + 0x100040);
        // a touch that starts before the binding, clipped at its start
        CHECK(t.read(lo - 0x1000, lo + 0x1000));
        CHECK(P(t, lo, hi, cap, keep) && keep.size() == 2 && keep[0].first == lo && keep[0].second == lo + 0x1000);
    }
    {   // reads merge: touching and overlapping ranges become one; a read inside one already known adds nothing
        Table t;
        CHECK(!t.read(0x1000, 0x2000));
        CHECK(!t.read(0x3000, 0x4000));
        CHECK(t.reads() == 2);
        CHECK(!t.read(0x2000, 0x3000));   // (touches both)
        CHECK(t.reads() == 1);
        CHECK(!t.read(0x1800, 0x3800));
        CHECK(t.reads() == 1);
        CHECK(!t.read(0x0800, 0x5000));   // (covers it)
        CHECK(t.reads() == 1);
    }
    {   // a CPU write is to be noted only in a large binding followed: the CPU's writes elsewhere do not fill the reads
        Table t;
        const uint64_t lo = 0x117cabc000, hi = lo + 0x230040;
        CHECK(!t.followed(lo, lo + 4 * KB));   // (not bound yet)
        P(t, lo, hi, cap, keep);
        CHECK(t.followed(lo, lo + 4 * KB) && t.followed(hi - 1, hi + 4 * KB));
        CHECK(!t.followed(hi, hi + 4 * KB) && !t.followed(lo - 4 * KB, lo));
        P(t, 0x1000000, 0x1000000 + 64 * KB, cap, keep);   // (small: written back, not followed)
        CHECK(!t.followed(0x1000000, 0x1001000));
    }
    {   // a binding bound many times in one submission (a frame's dispatches) counts once toward its probation
        Table t;
        const uint64_t lo = 0x117cabc000, hi = lo + 0x230000;
        for (uint64_t sub = 100; sub < 100 + t.probation; sub++)
            for (int bind = 0; bind < 12; bind++) CHECK(!t.plan(lo, hi, cap, sub, keep));
        CHECK(t.plan(lo, hi, cap, 100 + t.probation, keep) && keep.empty());
    }
    {   // past its bounds the table forgets everything: every binding starts its probation again (written back whole)
        Table t;
        t.max_seen = 2;
        for (uint32_t i = 0; i <= t.probation; i++) P(t, 0x10000000, 0x10000000 + 2 * MB, cap, keep);
        CHECK(P(t, 0x10000000, 0x10000000 + 2 * MB, cap, keep));
        P(t, 0x20000000, 0x20000000 + 2 * MB, cap, keep);
        CHECK(t.resets() == 0 && t.seen() == 2);
        CHECK(!P(t, 0x30000000, 0x30000000 + 2 * MB, cap, keep));   // (a third binding: the table starts over)
        CHECK(t.resets() == 1 && t.seen() == 1);
        CHECK(!P(t, 0x10000000, 0x10000000 + 2 * MB, cap, keep));   // (in its probation again)
        Table r;
        r.max_read = 2;
        r.read(0x1000, 0x2000), r.read(0x5000, 0x6000);
        CHECK(r.resets() == 0 && r.reads() == 2);
        r.read(0x9000, 0xa000);
        CHECK(r.resets() == 1 && r.reads() == 1);
        // a binding with two edge pages when the reads are one short of full: both pages kept, not only the last (mooki000)
        for (size_t before : {size_t(2), size_t(3)}) {
            Table c;
            c.max_read = 4;
            for (size_t i = 0; i < before; i++) c.read(0x1000 + i * 0x10000, 0x2000 + i * 0x10000);
            const uint64_t lo = 0x60000040, hi = lo + 2 * MB;
            for (uint32_t i = 0; i < c.probation; i++) CHECK(!P(c, lo, hi, cap, keep));
            CHECK(P(c, lo, hi, cap, keep) && keep.size() == 2);
            CHECK(keep[0].first == lo && keep[0].second == lo / 4096 * 4096 + 4096);
            CHECK(keep[1].first == hi / 4096 * 4096 && keep[1].second == hi);
        }
        // a binding's edge pages filling the reads start it over too, with the binding still followed after
        Table e;
        e.max_read = 1;
        CHECK(!P(e, 0x50000040, 0x50000040 + 2 * MB, cap, keep));
        CHECK(e.followed(0x50000040, 0x50001000));
    }
    std::printf("wb_learn: ok\n");
    return 0;
}
