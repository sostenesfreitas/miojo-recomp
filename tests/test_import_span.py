"""The chunks an import takes (gpu/vk/vk_memory.cpp span_game_pools): a chunk-aligned range grown over the game pools that
run across its ends, while it stays within the span, so that a binding across a chunk's end inside such a pool finds both
chunks in one import instead of importing them again as one on the replay thread.

python -I -B tests/test_import_span.py [--cxx clang++]
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
    span = block(args.source.read_text(encoding='utf-8'), 'void span_game_pools(')
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
struct GuestPool { uint64_t lo, hi; };
std::vector<GuestPool> guest_pools;   // (by lo, as vk_memory.cpp's; they may overlap)
uint64_t span_mb = 64;
uint64_t import_span() { return span_mb << 20; }
''' + span + r'''
constexpr uint64_t MB = 1ull << 20, C = 16 * MB;   // (the chunk)
void pools(std::vector<GuestPool> v) {
    std::sort(v.begin(), v.end(), [](const GuestPool& a, const GuestPool& b) { return a.lo < b.lo; });
    guest_pools = v;
}
// the range [lo, hi) comes out as [want_lo, want_hi)
void check(uint64_t lo, uint64_t hi, uint64_t want_lo, uint64_t want_hi, int line) {
    span_game_pools(lo, hi, C);
    if (lo != want_lo || hi != want_hi) {
        std::fprintf(stderr, "line %d: %#llx..%#llx, wanted %#llx..%#llx\n", line, (unsigned long long)lo, (unsigned long long)hi,
                     (unsigned long long)want_lo, (unsigned long long)want_hi);
        std::exit(1);
    }
}
#define SPAN(lo, hi, wlo, whi) check(lo, hi, wlo, whi, __LINE__)
int main() {
    const uint64_t K = 0x1021000000;   // (OoT's 16 MB pool at 0x1021745000, across 0x1022000000)
    pools({{K + 0x745000, K + 0x745000 + C}, {K - C, K - 4 * MB}});
    SPAN(K, K + C, K, K + 2 * C);                  // (the first chunk: the pool's other chunk with it)
    SPAN(K + C, K + 2 * C, K, K + 2 * C);          // (the second: the first with it)
    pools({{K + MB, K + 2 * MB}});
    SPAN(K, K + C, K, K + C);                      // (no pool across an end: as it was)
    pools({{K - 2 * MB, K}, {K + C, K + C + MB}});
    SPAN(K, K + C, K, K + C);                      // (pools ending at lo, starting at hi: not across them)
    pools({{K + C - MB, K + C + 288 * MB}});
    SPAN(K, K + C, K, K + C);                      // (a pool too big for the span: chunked, as before)
    pools({{K + C - MB, K + 2 * C + MB}, {K + 3 * C - MB, K + 3 * C + MB}, {K + 4 * C - MB, K + 4 * C + MB}});
    SPAN(K, K + C, K, K + 4 * C);                  // (across hi, then across the new hi; the next would be 80 MB: not)
    pools({{K - C / 2, K + C + C / 2}, {K + C - 2 * MB, K + C - MB}});
    SPAN(K, K + C, K - C, K + 2 * C);              // (a long pool behind a later short one that is not across the end)
    pools({{K + 2 * MB, K + C + 2 * MB}, {K + C - 3 * MB, K + C - 2 * MB}});
    SPAN(K, K + C, K, K + 2 * C);                  // (the same at hi: the short one is the nearest below it)
    pools({{K + C - MB, K + C + MB}});
    span_mb = 16;
    SPAN(K, K + C, K, K + C);                      // (a span of one chunk: nothing to grow)
    span_mb = 0;
    SPAN(K, K + C, K, K + C);                      // (RX_VK_IMPORT_SPAN_MB=0: off)
    std::puts("PASS: the chunks of game pools across a range's ends taken in, within the span; the rest left as before");
}
'''
    with tempfile.TemporaryDirectory(prefix='import-span-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)   # (a range that keeps growing never ends: a failure too)


if __name__ == '__main__':
    main()
