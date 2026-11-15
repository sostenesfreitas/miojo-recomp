"""RangeWalk (gpu/vk/vk_memory.cpp): whether a written page falls in the captured ranges or the texture storage, asked for
every page a submit collects, by a cursor that walks the sorted ranges once instead of a binary search for each page.

Its answers must be the binary search's for any order of pages: ascending (as the collections give them), random,
descending and repeated, over no range, one, and many, with gaps between the ranges, from address 0, and touching (one's
end the next one's start). memory_collect_written, capture_make's staging and take_capture's texture shadow must ask it,
not search again. The fixture also prints both ways' time for a scan (a measure on this PC, not a check).

python -I -B tests/test_vk_range_walk.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, tail=""):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if tail:
        end = text.index(tail, end) + len(tail)
    return text[start:end]


FIXTURE = r'''#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
@WALK@

static bool searched(const Ranges& r, uint64_t a) {   // (the binary search it replaced)
    auto it = std::upper_bound(r.begin(), r.end(), std::make_pair(a, ~uint64_t(0)));
    return it != r.begin() && a < std::prev(it)->second;
}

// (sorted, disjoint, page-aligned. shape 0: gaps before and between; 1: the first from address 0, gaps between; 2: from 0,
// every one touching the next (hi == the next lo); 3: from 0, touching or not at random)
static Ranges ranges(std::mt19937_64& rng, int n, int shape = 0) {
    Ranges r;
    uint64_t at = shape ? uint64_t(0) : (rng() % 64) << 12;
    for (int i = 0; i < n; i++) {
        const bool gap = shape == 0 || (shape == 1 && i) || (shape == 3 && i && rng() % 2);
        if (gap) at += (1 + rng() % 256) << 12;
        const uint64_t hi = at + ((1 + rng() % 512) << 12);
        r.push_back({at, hi});
        at = hi;
    }
    return r;
}

int main() {
    std::mt19937_64 rng(2515);
    long checked = 0;
    for (int shape = 0; shape < 4; shape++)
    for (int n : {0, 1, 2, 3, 40, 700})
        for (int round = 0; round < (shape ? 50 : 200); round++) {
            const Ranges r = ranges(rng, n, shape);
            const uint64_t top = (r.empty() ? 1ull << 24 : r.back().second + (64ull << 12)) >> 12;
            std::vector<uint64_t> pages(1 + rng() % 3000);
            for (auto& p : pages) p = (rng() % top) << 12;
            if (shape) {   // (address 0 and every range's ends asked, so the start at 0 and each touching end are seen)
                pages.push_back(0);
                for (const auto& [lo, hi] : r) pages.push_back(lo), pages.push_back(hi);
            }
            for (int order = 0; order < 4; order++) {
                if (order == 0) std::sort(pages.begin(), pages.end());
                else if (order == 1) std::shuffle(pages.begin(), pages.end(), rng);
                else if (order == 2) std::sort(pages.rbegin(), pages.rend());
                else {   // (ascending with each page asked twice, then a step back: take_capture's second loop)
                    std::sort(pages.begin(), pages.end());
                    std::vector<uint64_t> twice;
                    for (uint64_t p : pages) twice.push_back(p), twice.push_back(p);
                    twice.push_back(pages[pages.size() / 2]);
                    pages = twice;
                }
                RangeWalk w{r};
                for (uint64_t a : pages) {
                    for (uint64_t at : {a, a + 4095}) {
                        if (at < a) continue;
                        const bool want = searched(r, at);
                        if (w.in(at) != want) {
                            std::printf("FAIL: %d ranges, shape %d, order %d, address %llx: %d, the search says %d\n", n,
                                        shape, order, (unsigned long long)at, !want, want);
                            return 1;
                        }
                        checked++;
                    }
                }
            }
        }
    // (a scan as memory_collect_written makes one: pages in ascending order over the captured ranges)
    for (int n : {16, 256}) {
        const Ranges r = ranges(rng, n);
        std::vector<uint64_t> pages;
        for (const auto& [lo, hi] : r)
            for (uint64_t p = lo - (8ull << 12); p < hi; p += 4096)
                if (rng() % 4 == 0) pages.push_back(p);
        std::sort(pages.begin(), pages.end());
        long a = 0, b = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 50; k++)
            for (uint64_t p : pages) a += searched(r, p);
        const auto t1 = std::chrono::steady_clock::now();
        for (int k = 0; k < 50; k++) {
            RangeWalk w{r};
            for (uint64_t p : pages) b += w.in(p);
        }
        const auto t2 = std::chrono::steady_clock::now();
        if (a != b) return std::printf("FAIL: the scans disagree\n"), 1;
        const double per = 50.0 * (double)pages.size();
        std::printf("a scan of %zu pages over %d ranges: %.1f ns a page searched, %.1f ns walked\n", pages.size(), n,
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / per,
                    std::chrono::duration<double, std::nano>(t2 - t1).count() / per);
    }
    std::printf("PASS: RangeWalk answers as the binary search, %ld addresses in every order\n", checked);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    collect = block(source, "void memory_collect_written(")
    if "RangeWalk in_capture{capture_ranges_merged()}" not in collect or "upper_bound" in collect:
        raise SystemExit("memory_collect_written: the captured ranges are not walked with RangeWalk")
    make = block(source, "std::unique_ptr<Capture> capture_make() {")
    if "RangeWalk in_texr{texr}" not in make or "in_texr.in(" not in make or "upper_bound(texr" in make:
        raise SystemExit("capture_make: the staging's texture ranges are not walked with RangeWalk")
    take = block(source, "bool take_capture(")
    if take.count("in_tex.in(") != 3 or "upper_bound" in take:
        raise SystemExit("take_capture: the texture shadow's three page tests are not RangeWalk's")
    with tempfile.TemporaryDirectory(prefix="vk-range-walk-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / ("fixture.exe" if os.name == "nt" else "fixture")
        cpp.write_text(FIXTURE.replace("@WALK@", block(source, "struct RangeWalk {", ";")), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == "__main__":
    main()
