// (hookshot) What of a large GPU-written binding goes without a write-back (RX_VK_WB_MAX_KB), learned from what the CPU touches.
//
// OoT writes tens of MB a frame through the GPU that the CPU never reads (culling lists and per-frame buffers: 2 x 15 MB, 9.5 MB,
// 4 x 6.3 MB ... in Hyrule Field), and all of it was copied back to guest memory at the end of its submission: ~68 MB a frame over
// PCIe. RX_VK_WB_MAX_KB=1024 left out every written binding over 1 MB, but the CPU does touch parts of the large ones:
//  - it reads some (invalidate): four 4.3 MB buffers in turn, each in full after every frame the GPU writes it, and an 8.4 MB range
//    now and then. With the plain cap those reads found what guest memory held before the GPU wrote them;
//  - it writes pages inside most of them, ~12 pages a frame in the 160 MB pool of the culling lists, always the same ones. A mirror
//    sync copies such a page whole from guest memory, so with the plain cap the rest of the page went back to its stale bytes over
//    the GPU's, ~700 pages a second.
// So a large binding is written back whole for its first `probation` writes, the CPU's reads and writes meanwhile noted by range
// (a write as its whole page). After that only the parts the CPU has read or written are written back, and the page at either end
// when the binding starts or ends inside it (a page it shares with what is next to it); the rest is left out. A range
// the CPU first touches after that (read more rarely than every `probation` writes) finds the stale copy that once, and is written
// back from then on: read() says so, for the log. Past max_seen bindings or max_read ranges everything is forgotten and every
// binding starts its probation again.
//
// Plain rules, no locking and no Vulkan: vk_memory.cpp holds them under its own mutex. tests/test_wb_learn.cpp runs them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace wb_learn {

// The cap from RX_VK_WB_MAX_KB=<KB>: unset or 0, none (every written binding written back, the default). Off by default until
// it shows a gain on more than one machine: on an RTX 3060 Ti and an RX 580 it wrote back 50 MB a frame less at the same fps
inline uint64_t max_bytes(const char* env) {
    if (!env || !*env) return 0;
    unsigned long long kb = 0;
    for (const char* c = env; *c >= '0' && *c <= '9'; c++) kb = kb * 10 + (unsigned long long)(*c - '0');
    return (uint64_t)kb << 10;
}

class Table {
public:
    uint32_t probation = 8;     // (submissions writing a binding in which it is written back whole, the CPU's touches noted meanwhile)
    size_t max_seen = 8192;     // (bindings followed)
    size_t max_read = 8192;     // (ranges the CPU touched, merged)

    // A binding [lo, hi) the GPU writes in submission `sub` (its value: several binds of it in one submission count once, so that
    // the probation spans that many frames, not a frame's dispatches). False: write it all back. True: write back only the ranges
    // put in `keep` (the parts the CPU has touched; none: nothing)
    bool plan(uint64_t lo, uint64_t hi, uint64_t cap, uint64_t sub, std::vector<std::pair<uint64_t, uint64_t>>& keep) {
        keep.clear();
        if (!cap || hi <= lo || hi - lo <= cap) return false;
        auto it = seen_.find(lo);
        if (it == seen_.end() || it->second.hi != hi) {   // (new; or another binding at this start: its own probation)
            if (it == seen_.end() && seen_.size() >= max_seen) reset();   // (bindings of scenes long gone: all start again)
            edges(lo, hi);   // (before the entry: a reset there clears the table)
            seen_[lo] = Seen{hi, 1, false, sub};
            return false;
        }
        if (it->second.writes < probation || it->second.sub == sub) {   // (and left out from the submission after its last one)
            if (it->second.sub != sub) it->second.writes++, it->second.sub = sub;
            return false;
        }
        it->second.left_out = true;
        auto r = read_.lower_bound(lo);
        if (r != read_.begin() && std::prev(r)->second > lo) --r;
        for (; r != read_.end() && r->first < hi; ++r) keep.push_back({r->first < lo ? lo : r->first, r->second > hi ? hi : r->second});
        return true;
    }

    // The CPU reads (invalidate) or writes [lo, hi): from now on it is written back wherever a binding over it is planned. True
    // when part of it was new and lay in a binding past its probation: that part's last write-back may have been left out (this
    // touch found the stale copy)
    bool read(uint64_t lo, uint64_t hi) {
        if (hi <= lo) return false;
        const bool known = covered(lo, hi);
        if (!known) add_read(lo, hi);
        if (known) return false;
        for (auto it = seen_.lower_bound(hi); it != seen_.begin();) {
            --it;
            if (it->second.hi > lo && it->second.left_out) return true;
            if (it->first + kFar < lo) break;
        }
        return false;
    }

    // Whether [lo, hi) is in a large binding followed here (a CPU write there is one to note with read(); the CPU's other writes,
    // all over memory every frame, are not)
    bool followed(uint64_t lo, uint64_t hi) const {
        for (auto it = seen_.lower_bound(hi); it != seen_.begin();) {
            --it;
            if (it->second.hi > lo) return true;
            if (it->first + kFar < lo) break;
        }
        return false;
    }

    uint64_t resets() const { return resets_; }
    size_t seen() const { return seen_.size(); }
    size_t reads() const { return read_.size(); }

private:
    static constexpr uint64_t kFar = 1ull << 30;   // (no binding is this large: the backward walks stop past it)
    struct Seen {
        uint64_t hi;
        uint32_t writes;
        bool left_out;   // (past its probation: planned in part)
        uint64_t sub;    // (the submission of its last counted write)
    };
    std::map<uint64_t, Seen> seen_;       // (by start)
    std::map<uint64_t, uint64_t> read_;   // (start -> end, merged, none touching)
    uint64_t resets_ = 0;

    // (past max_seen bindings or max_read ranges: forget all; every binding is written back whole for a new probation, so nothing is
    // left out that the CPU touches)
    void reset() {
        seen_.clear(), read_.clear();
        resets_++;
    }

    // (a binding that starts or ends inside a page shares that page with whatever is next to it, which the CPU may write: the 15 MB
    // culling lists end 0x80 bytes into a page the CPU writes every frame, and a mirror sync copies that page whole. Those pages are
    // always written back)
    void edges(uint64_t lo, uint64_t hi) {
        constexpr uint64_t kPage = 4096;
        const bool first = lo % kPage, last = hi % kPage;
        // (room for both first: a reset between the two kept only the last, and the first shared page was left out (mooki000))
        if (read_.size() + first + last > max_read) reset();
        if (first) add_read(lo / kPage * kPage, lo / kPage * kPage + kPage);
        if (last) add_read(hi / kPage * kPage, hi / kPage * kPage + kPage);
    }
    bool covered(uint64_t lo, uint64_t hi) const {
        auto it = read_.upper_bound(lo);
        if (it == read_.begin()) return false;
        --it;
        return it->first <= lo && it->second >= hi;
    }
    void add_read(uint64_t lo, uint64_t hi) {
        auto it = read_.upper_bound(lo);
        if (it != read_.begin()) {
            auto prev = std::prev(it);
            if (prev->second >= lo) {
                lo = prev->first;
                if (prev->second > hi) hi = prev->second;
                it = read_.erase(prev);
            }
        }
        while (it != read_.end() && it->first <= hi) {
            if (it->second > hi) hi = it->second;
            it = read_.erase(it);
        }
        if (read_.size() >= max_read) {
            reset();
        }
        read_.emplace(lo, hi);
    }
};

}  // namespace wb_learn
