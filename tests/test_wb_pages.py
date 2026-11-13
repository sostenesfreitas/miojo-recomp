"""The write-back page table (gpu/vk/vk_memory.cpp WbPages): values per page in chunks made on first use, and max_in, which
reads a range of pages skipping whole chunks never made. Built with small chunks so their edges are easy to reach.

python -I -B tests/test_wb_pages.py [--cxx clang++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    table = block(args.source.read_text(encoding='utf-8'), 'struct WbPages {')
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
constexpr uint64_t kChunkPages = 16, kPages = 64;   // (four chunks of 16 pages: vk_memory.cpp has 2048 of 65536)
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
''' + table + r''';
uint64_t range(const WbPages& t, uint64_t first, uint64_t last, bool want_ever) {
    bool ever = false;
    const uint64_t v = t.max_in(first, last, &ever);
    CHECK(ever == want_ever);
    CHECK(t.max_in(first, last, nullptr) == v);
    return v;
}
int main() {
    WbPages t;
    CHECK(t.empty() && t.get(5) == 0 && range(t, 0, 63, false) == 0);
    t.set(3, 7);   // (chunk 0)
    CHECK(!t.empty() && t.get(3) == 7 && t.get(4) == 0);
    CHECK(range(t, 0, 63, true) == 7 && range(t, 4, 63, false) == 0 && range(t, 3, 3, true) == 7);
    t.set(40, 9);   // (chunk 2; chunk 1 never made)
    CHECK(range(t, 3, 40, true) == 9 && range(t, 4, 39, false) == 0 && range(t, 16, 47, true) == 9);
    t.set(15, 11), t.set(16, 2);   // (the last page of chunk 0, the first of chunk 1)
    CHECK(range(t, 15, 15, true) == 11 && range(t, 16, 16, true) == 2 && range(t, 14, 17, true) == 11);
    CHECK(range(t, 16, 39, true) == 2);
    t.set(15, 1);   // (a later write-back's value replaces the page's, lower or not)
    CHECK(t.get(15) == 1 && range(t, 15, 15, true) == 1 && range(t, 0, 63, true) == 9);
    t.set(64, 5);   // (past the table: left out)
    CHECK(t.get(64) == 0 && t.get(1000) == 0);
    t.set(63, 4);   // (the table's last page; a range reaching past the end stops there)
    CHECK(range(t, 63, 1000, true) == 4 && range(t, 41, 62, false) == 0 && range(t, 41, 1000, true) == 4);
    std::puts("PASS: values per page, ranges over chunks never made, chunk edges and the table's end");
}
'''
    with tempfile.TemporaryDirectory(prefix='wb-pages-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)   # (a range read that stops moving hangs: a failure too)


if __name__ == '__main__':
    main()
