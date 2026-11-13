"""CPU check of the write-back diff's chunks (gpu/vk/vk_memory.cpp wbd_split): a run in whole words comes out as its words
before the first 16-byte vector, the vectors 64 KB at most at a time, and the words after the last vector, covering the run
exactly once and in order, every vector chunk 16-byte aligned and whole.

python -I -B tests/test_wb_diff_split.py [--cxx clang++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    text = args.source.read_text(encoding='utf-8')
    helper = block(text, 'template <class F> void wbd_split(')
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed: %s (at %llu size %llu)\n", #ok, \
    (unsigned long long)at0, (unsigned long long)size0); std::exit(1); } } while (0)
static uint64_t at0, size0;
''' + helper + r'''
static void check(uint64_t at, uint64_t size) {
    at0 = at, size0 = size;
    uint64_t next = at, vec_bytes = 0;
    int pieces = 0, words_before = 0, words_after = 0;
    bool seen_vecs = false;
    wbd_split(at, size, [&](uint64_t a, uint64_t len, bool vecs) {
        CHECK(a == next && len > 0);   // (in order, no gap, no overlap, nothing empty)
        CHECK(a % 4 == 0 && len % 4 == 0);
        if (vecs) {
            CHECK(a % 16 == 0 && len % 16 == 0 && len <= (64u << 10));
            CHECK(!words_after);
            seen_vecs = true, vec_bytes += len;
        } else {
            CHECK(len < 16);   // (only the odd words at an end)
            (seen_vecs ? words_after : words_before)++;
        }
        next = a + len, pieces++;
    });
    CHECK(next == at + size);
    CHECK(words_before + words_after <= 2 && words_after <= 1);   // (with no vector, its head and tail both come "before")
    CHECK(!seen_vecs || words_before <= 1);
    // (every whole vector inside the run went as one)
    const uint64_t first = (at + 15) & ~15ull, last = (at + size) & ~15ull;
    CHECK(vec_bytes == (last > first ? last - first : 0));
}
int main() {
    for (uint64_t at = 0; at < 64; at += 4)
        for (uint64_t size = 4; size <= 300; size += 4) check(0x100000 + at, size);
    for (uint64_t at = 0; at < 32; at += 4)
        for (uint64_t size : {65532ull, 65536ull, 65540ull, 65552ull, 131072ull, 131084ull, (64ull << 20) + 12})
            check(0x40000 + at, size);
    std::puts("PASS: write-back diff chunks cover each run once, in order, vectors aligned and at most 64 KB");
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'split.cpp'
        exe = Path(tmp) / 'split.exe'
        src.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O1', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
