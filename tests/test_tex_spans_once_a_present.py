"""The texture spans' ageing (gpu/vk/vk_memory.cpp publish_tex_spans): a span unused for 1800 presents is dropped, and the
spans are looked over for that once a present, not at each of the present's submissions. A span noted between two
submissions of one present is still published by the next.

python -I -B tests/test_tex_spans_once_a_present.py [--cxx clang++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    publish = block(args.source.read_text(encoding='utf-8'), 'void publish_tex_spans(uint64_t frame) {')
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
// (the spans, counting each time they are gone through from the start)
struct Spans : std::map<uint64_t, std::pair<uint64_t, uint64_t>> {
    int walks = 0;
    iterator begin() { walks++; return std::map<uint64_t, std::pair<uint64_t, uint64_t>>::begin(); }
};
Spans tex_spans;
std::vector<std::pair<uint64_t, uint64_t>> tex_merged, tex_cap_ranges, dropped;
bool tex_spans_changed = false;
uint64_t tex_merged_bytes = 0, cap_ranges_ver = 0;
std::mutex shadow_mu, cap_ranges_mu;
void shadow_drop_range(uint64_t lo, uint64_t hi) { dropped.push_back({lo, hi}); }
''' + publish + r'''
int main() {
    tex_spans.try_emplace(0x1000, 0x3000, 100);   // (lo -> hi, the present it was last uploaded from)
    tex_spans.try_emplace(0x8000, 0x9000, 100);
    tex_spans_changed = true;
    publish_tex_spans(100);   // (the present's first submission: looked over, and published)
    CHECK(tex_cap_ranges.size() == 2 && tex_merged_bytes == 0x3000 && cap_ranges_ver == 1 && !tex_spans_changed);
    const int after_first = tex_spans.walks;
    for (int i = 0; i < 19; i++) publish_tex_spans(100);   // (its other submissions: nothing to do)
    CHECK(tex_spans.walks == after_first && cap_ranges_ver == 1 && dropped.empty());
    tex_spans.try_emplace(0x20000, 0x21000, 100);   // (a texture uploaded between two of them)
    tex_spans_changed = true;
    publish_tex_spans(100);
    CHECK(tex_cap_ranges.size() == 3 && tex_merged_bytes == 0x4000 && cap_ranges_ver == 2 && dropped.empty());
    const int same_present = tex_spans.walks;
    publish_tex_spans(101);   // (the next present: looked over again, once)
    publish_tex_spans(101);
    CHECK(tex_spans.walks == same_present + 1 && cap_ranges_ver == 2);
    tex_spans[0x8000].second = 1500;   // (used again later than the others)
    publish_tex_spans(1900);   // (1800 presents after 100: kept, the bound is past 1800)
    CHECK(dropped.empty() && tex_cap_ranges.size() == 3);
    publish_tex_spans(1901);   // (the spans last used at 100 go, with their shadow pages; the one used at 1500 stays)
    CHECK(dropped.size() == 2 && dropped[0] == std::make_pair(uint64_t(0x1000), uint64_t(0x3000)) &&
          dropped[1] == std::make_pair(uint64_t(0x20000), uint64_t(0x21000)));
    CHECK(tex_spans.size() == 1 && tex_cap_ranges.size() == 1 && tex_cap_ranges[0].first == 0x8000 && tex_merged_bytes == 0x1000 &&
          cap_ranges_ver == 3 && !tex_spans_changed);
    std::puts("PASS: spans aged once a present, a span noted mid-present published, unused spans dropped after 1800");
}
'''
    with tempfile.TemporaryDirectory(prefix='tex-spans-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
