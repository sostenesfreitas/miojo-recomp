"""CPU check of write-back runs cut around replay-owned report words.

python -I -B tests/test_report_words.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    text = args.source.read_text(encoding='utf-8')
    helper = (block(text, 'void sorted_merge(') + chr(10) + block(text, 'void cpu_words_merge(') + chr(10) +
              block(text, 'void memory_cpu_owned(') + chr(10) + block(text, 'void cut_cpu_words('))
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <set>
#include <vector>
struct Pool { uint64_t lo; };
struct VkBufferCopy { uint64_t srcOffset, dstOffset, size; };
std::mutex cpu_words_mu;
std::vector<uint64_t> cpu_words, cpu_words_recent, cpu_words_new;   // (the big and the recent sorted sets, and noted since: vk_memory.cpp)
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
''' + helper + r'''
// after a merge: both sets sorted with no repeats, no word in both, nothing left noted, and together exactly the words in want
bool sets_hold(const std::set<uint64_t>& want) {
    if (!std::is_sorted(cpu_words.begin(), cpu_words.end()) || !std::is_sorted(cpu_words_recent.begin(), cpu_words_recent.end()))
        return false;
    if (std::adjacent_find(cpu_words.begin(), cpu_words.end()) != cpu_words.end() ||
        std::adjacent_find(cpu_words_recent.begin(), cpu_words_recent.end()) != cpu_words_recent.end())
        return false;
    std::vector<uint64_t> both;
    std::set_intersection(cpu_words.begin(), cpu_words.end(), cpu_words_recent.begin(), cpu_words_recent.end(),
                          std::back_inserter(both));
    std::set<uint64_t> all(cpu_words.begin(), cpu_words.end());
    all.insert(cpu_words_recent.begin(), cpu_words_recent.end());
    return both.empty() && cpu_words_new.empty() && all == want;
}
void reset() {
    cpu_words.clear();
    cpu_words_recent.clear();
    cpu_words_new.clear();
}
// owned: words noted since the last merge; kept: words merged before (in the big set already)
void check(uint64_t start, uint64_t end, std::initializer_list<uint64_t> owned, std::initializer_list<uint64_t> kept = {}) {
    const Pool pool{0x1000};
    cpu_words.clear();
    cpu_words_recent.clear();
    cpu_words_new.clear();
    std::set<uint64_t> owned_words;
    for (uint64_t offset : kept) cpu_words.push_back(pool.lo + offset), owned_words.insert(pool.lo + offset);
    std::sort(cpu_words.begin(), cpu_words.end());
    for (uint64_t offset : owned) cpu_words_new.push_back(pool.lo + offset), owned_words.insert(pool.lo + offset);
    std::vector<VkBufferCopy> runs{{start, start, end - start}};
    cut_cpu_words(pool, runs);
    CHECK(sets_hold(owned_words));
    std::set<uint64_t> copied;
    uint64_t previous_end = start;
    for (const auto& run : runs) {
        CHECK(run.srcOffset == run.dstOffset && run.size);
        CHECK(run.srcOffset >= previous_end && run.srcOffset + run.size <= end);
        for (uint64_t byte = run.srcOffset; byte < run.srcOffset + run.size; ++byte)
            CHECK(copied.insert(byte).second);
        previous_end = run.srcOffset + run.size;
    }
    for (uint64_t byte = 0; byte < 96; ++byte) {
        const bool expected = byte >= start && byte < end && !owned_words.count((pool.lo + byte) & ~7ull);
        CHECK(copied.count(byte) == expected);
    }
}
// the recent set goes into the big one once it is past max(4096, an eighth of the big one), and is emptied
void check_promotion() {
    reset();
    std::set<uint64_t> want;
    for (uint64_t i = 0; i < 4096; i++) cpu_words_new.push_back(0x100000 + 16 * i), want.insert(0x100000 + 16 * i);
    cpu_words_merge();   // (4096: not past it)
    CHECK(cpu_words.empty() && cpu_words_recent.size() == 4096 && sets_hold(want));
    cpu_words_new.push_back(0x100008), want.insert(0x100008);   // (one more, between two kept ones)
    cpu_words_merge();
    CHECK(cpu_words.size() == 4097 && cpu_words_recent.empty() && sets_hold(want));
    reset();
    want.clear();
    for (uint64_t i = 0; i < 40000; i++) cpu_words.push_back(0x400000 + 8 * i), want.insert(0x400000 + 8 * i);
    for (uint64_t i = 0; i < 5000; i++) cpu_words_new.push_back(0x800000 + 8 * i), want.insert(0x800000 + 8 * i);
    cpu_words_merge();   // (an eighth of 40000 is 5000: not past it)
    CHECK(cpu_words.size() == 40000 && cpu_words_recent.size() == 5000 && sets_hold(want));
    cpu_words_new.push_back(0x900000), want.insert(0x900000);
    cpu_words_merge();
    CHECK(cpu_words.size() == 45001 && cpu_words_recent.empty() && sets_hold(want));
}
// memory_cpu_owned forgets both sets once they hold more than 65536 words, and keeps the word it was given
void check_clear() {
    reset();
    for (uint64_t i = 0; i < 40000; i++) cpu_words.push_back(0x400000 + 8 * i);
    for (uint64_t i = 0; i < 3000; i++) cpu_words_recent.push_back(0x800000 + 8 * i);
    memory_cpu_owned(0x900000, 8);   // (43000: kept as they are)
    CHECK(cpu_words.size() == 40000 && cpu_words_recent.size() == 3000 && cpu_words_new == std::vector<uint64_t>{0x900000});
    reset();
    for (uint64_t i = 0; i < 62000; i++) cpu_words.push_back(0x400000 + 8 * i);
    for (uint64_t i = 0; i < 3000; i++) cpu_words_recent.push_back(0x800000 + 8 * i);
    for (uint64_t i = 0; i < 600; i++) cpu_words_new.push_back(0x900000 + 8 * i);
    // (65600 words: merged first, the recent set staying recent (3600, under an eighth of 62000), then both forgotten)
    memory_cpu_owned(0xA00000, 16);
    CHECK(cpu_words.empty() && cpu_words_recent.empty() && cpu_words_new == (std::vector<uint64_t>{0xA00000, 0xA00008}));
}
// a word repeated in one batch, and one noted again while it is in the recent set: each kept once
void check_repeats() {
    reset();
    std::set<uint64_t> want{0x2000, 0x2008};
    cpu_words_new = {0x2008, 0x2000, 0x2008, 0x2000};   // (repeats in one batch)
    cpu_words_merge();
    CHECK(cpu_words.empty() && cpu_words_recent.size() == 2 && sets_hold(want));
    cpu_words_new = {0x2008, 0x2010};   // (0x2008 is in the recent set already)
    want.insert(0x2010);
    cpu_words_merge();
    CHECK(cpu_words.empty() && cpu_words_recent.size() == 3 && sets_hold(want));
}
// The merges keep the two sets together the set of every word noted, whether the new ones are all known already (the same
// report words every frame: nothing merged), all new, or mixed, with duplicates among them. (800 rounds over 16384 addresses:
// the recent set goes past 4096 words and is promoted on the way)
void merge_matches_a_set() {
    reset();
    std::set<uint64_t> want;
    uint64_t seed = 12345;
    auto next = [&seed] { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return seed >> 33; };
    for (int round = 0; round < 800; ++round) {
        const int kind = round % 4;   // 0 known only, 1 new only, 2 mixed, 3 none
        const int n = kind == 3 ? 0 : 1 + (int)(next() % 64);
        for (int i = 0; i < n; ++i) {
            uint64_t w;
            if ((kind == 0 || (kind == 2 && next() % 2)) && !want.empty()) {
                auto it = want.begin();
                std::advance(it, (long)(next() % want.size()));
                w = *it;
            } else {
                w = (next() % 16384) * 8;
            }
            cpu_words_new.push_back(w), want.insert(w);
            if (next() % 4 == 0) cpu_words_new.push_back(w);   // (a word noted twice before the merge)
        }
        cpu_words_merge();
        CHECK(sets_hold(want));
    }
    CHECK(!cpu_words.empty());   // (the recent set was promoted)
}
int main() {
    merge_matches_a_set();
    check(20, 24, {16});        // run starts inside an owned word
    check(20, 32, {16});        // keep the nonowned suffix
    check(12, 20, {16});        // partial end: keep the nonowned prefix
    check(16, 24, {16});        // entire word
    check(0, 48, {8, 16, 32});  // adjacent words, then a nonowned gap
    check(24, 32, {16});        // touching is not overlap
    check(0, 8, {16});          // nonoverlap
    check(0, 48, {});           // no ownership
    check(0, 48, {8, 32}, {16, 40});     // new words and kept ones, interleaved: both sets walked in order
    check(0, 48, {16, 24}, {16});        // a new word already kept: left out of the recent set, cut once
    check(0, 48, {}, {0, 8, 16, 24, 32, 40});   // everything owned, all kept
    for (uint64_t start = 0; start < 80; start += 4)
        for (uint64_t end = start + 4; end <= 80; end += 4) {
            check(start, end, {16});
            check(start, end, {8, 16, 32, 40});
            check(start, end, {8, 32}, {16, 40});
            check(start, end, {40}, {8, 16, 32});
        }
    check_promotion();
    check_clear();
    check_repeats();
    std::puts("PASS: partial report-word overlaps excluded; every nonowned byte preserved; both sets cut, kept apart, promoted "
              "and forgotten as they should be");
}
'''
    with tempfile.TemporaryDirectory(prefix='report-words-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-include', ENV_ON_H, '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        # (memory_cpu_owned does nothing with RX_VK_WB_REPORTS set: not from the caller's environment)
        env = {k: v for k, v in os.environ.items() if k != 'RX_VK_WB_REPORTS'}
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == '__main__':
    main()
