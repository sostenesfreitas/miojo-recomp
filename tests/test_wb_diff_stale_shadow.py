"""The write-back diff against the whole copy, after CPU writes of guest memory (gpu/vk/vk_memory.cpp, "the write-back's diff").

PR 1109's review: with the shadow and the mirror at 0, the CPU writes 1 into guest memory and the GPU's result is 0 again. The
whole copy writes 0 back; a diff whose shadow never heard of the CPU's write finds the mirror equal to the shadow and leaves 1.
This runs that sequence, and its relatives, through the functions of vk_memory.cpp that tell the shadows of a write
(guest_cpu_write, guest_cpu_wrote, shadow_written, shadow_written_collected, shadow_forget, shadows_forget_written: taken from
the source as they are), with a pool, guest memory and a host of the test's own. The write-back itself is the test's model of
memory_end_submission: the pages its shadow does not know are read from guest memory, then the words that differ from the
shadow are written (shaders/wb_diff.comp's word path). Each case ends with guest memory as the whole copy leaves it.

python -I -B tests/test_wb_diff_stale_shadow.py [--cxx g++]
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
    helper = chr(10).join(block(text, marker) for marker in (
        'void shadow_forget(Pool& p, uint64_t at, uint64_t len) {',
        'void shadows_forget_written() {',
        'void shadow_written_to(std::vector<uint32_t>& list, const uint32_t* pages, int64_t n) {',
        'void shadow_written(const uint32_t* pages, int64_t n) {',
        'void shadow_written_collected(const uint32_t* pages, int64_t n) {',
        'int guest_cpu_write(uint64_t gpu, const void* src, uint64_t n) {',
        'void guest_cpu_wrote(uint64_t gpu, uint64_t n) {'))
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #ok); std::exit(1); } } while (0)
constexpr uint64_t kPageShift = 12;
// (the test's pool: 8 pages of guest memory at kLo, a mirror and a shadow of it, in words)
constexpr uint64_t kLo = 0x10000000, kPages = 8, kWords = kPages * 1024, kVirtual = 1ull << 40;
struct Pool {
    uint64_t lo = kLo, hi = kLo + kPages * 4096;
    bool shadow = true;
    std::vector<uint64_t> shadow_known = std::vector<uint64_t>(1, 0);
};
std::map<uint64_t, Pool> imported;
uint64_t shadow_bytes = kPages * 4096;
std::vector<uint32_t> guest(kWords), mirror(kWords), shadow(kWords);
// (the host: GPU addresses below 1 TB are guest addresses; the 8 pages from kVirtual are the pool's, in reverse order)
struct Host {
    static uint64_t gpu_guest(uint64_t gpu) {
        if (gpu < kVirtual) return gpu >= kLo && gpu < kLo + kPages * 4096 ? gpu : 0;
        const uint64_t page = (gpu - kVirtual) >> 12;
        return page < kPages ? kLo + (kPages - 1 - page) * 4096 + (gpu & 4095) : 0;
    }
    static int gpu_write(uint64_t gpu, const void* src, uint64_t n) {
        for (uint64_t i = 0; i < n; i++) {   // (byte by byte: a write may cross into another page's mapping)
            const uint64_t g = gpu_guest(gpu + i);
            if (!g) return 0;
            std::memcpy((uint8_t*)guest.data() + (g - kLo), (const uint8_t*)src + i, 1);
        }
        return 1;
    }
};
Host the_host;
Host* host = &the_host;
bool diff_on = true;
bool wb_diff_on() { return diff_on; }
std::mutex shadow_written_mu;
std::vector<uint32_t> shadow_written_pages, shadow_written_collected_list, shadow_written_by_replay;
bool shadow_written_all = false;
int64_t st_wbd_known_game = 0, st_wbd_known_collected = 0, st_wbd_known_replay = 0;
void shadow_written_to(std::vector<uint32_t>& list, const uint32_t* pages, int64_t n);
void guest_cpu_wrote(uint64_t gpu, uint64_t n);
''' + helper + r'''
// A write-back of a run of the pool (offset and bytes, whole words), as memory_end_submission records it: the pages the
// shadow does not know from guest memory first, then the pass. diff false: the whole copy
int64_t filled = 0, written = 0;
void write_back(uint64_t at, uint64_t len, bool diff = true) {
    Pool& p = imported.begin()->second;
    if (!diff) {
        for (uint64_t w = at / 4; w < (at + len) / 4; w++) guest[w] = mirror[w];
        return;
    }
    shadows_forget_written();
    for (uint64_t pg = at >> kPageShift; pg <= (at + len - 1) >> kPageShift; pg++) {
        uint64_t& bits = p.shadow_known[pg >> 6];
        if (bits & (1ull << (pg & 63))) continue;
        bits |= 1ull << (pg & 63);
        std::copy(guest.begin() + pg * 1024, guest.begin() + (pg + 1) * 1024, shadow.begin() + pg * 1024);
        filled++;
    }
    for (uint64_t w = at / 4; w < (at + len) / 4; w++)
        if (mirror[w] != shadow[w]) guest[w] = mirror[w], shadow[w] = mirror[w], written++;
}
void reset() {
    imported.clear();
    imported.emplace(kLo, Pool{});
    std::fill(guest.begin(), guest.end(), 0u), std::fill(mirror.begin(), mirror.end(), 0u), std::fill(shadow.begin(), shadow.end(), 0u);
    shadow_written_pages.clear(), shadow_written_collected_list.clear(), shadow_written_by_replay.clear();
    shadow_written_all = false;
    st_wbd_known_game = st_wbd_known_collected = st_wbd_known_replay = 0;
    write_back(0, kPages * 4096);   // (every page known: the shadow, the mirror and guest memory at 0)
    filled = written = 0;
}
// The review's sequence at word w, the CPU's write made by `cpu`: guest memory ends as the whole copy leaves it
template <class F> void review_case(uint64_t w, F&& cpu, bool want_fixed = true) {
    reset();
    cpu();                    // (guest memory's word is 1 now)
    CHECK(guest[w] == 1);
    mirror[w] = 0;            // (the GPU's result: 0 again, what the shadow holds)
    write_back(w * 4 & ~4095ull, 4096);
    CHECK((guest[w] == 0) == want_fixed);
}
int main() {
    const uint32_t one = 1;
    // the replay's own write, by GPU address (the in-stream clear, the report words, a CPU copy): told by the write itself
    review_case(5, [&] { CHECK(guest_cpu_write(kLo + 5 * 4, &one, 4) == 1); });
    CHECK(st_wbd_known_replay == 1 && st_wbd_known_game == 0 && filled == 1 && written == 1);
    // ... through a virtual GPU address: the page it is mapped to is the one told (the pool's pages in reverse there)
    review_case(7 * 1024 + 9, [&] { CHECK(guest_cpu_write(kVirtual + 9 * 4, &one, 4) == 1); });
    // ... made in pieces by the host's write, then told over the whole range (WriteTexels)
    review_case(2 * 1024 + 3, [&] { Host::gpu_write(kLo + 2 * 4096 + 12, &one, 4); guest_cpu_wrote(kLo + 2 * 4096, 4096); });
    // ... across a page's end: both pages told
    {
        reset();
        const uint32_t two[2] = {1, 1};
        CHECK(guest_cpu_write(kLo + 3 * 4096 - 4, two, 8) == 1);
        write_back(2 * 4096, 2 * 4096);
        CHECK(guest[3 * 1024 - 1] == 0 && guest[3 * 1024] == 0 && filled == 2);
    }
    // the game's write, in a capture's pages, and in the collection's (all of them with RX_VK_CAPTURE=0)
    const uint32_t page4 = (uint32_t)((kLo >> 12) + 4);
    review_case(4 * 1024 + 1, [&] { guest[4 * 1024 + 1] = 1; shadow_written(&page4, 1); });
    CHECK(st_wbd_known_game == 1);
    review_case(4 * 1024 + 1, [&] { guest[4 * 1024 + 1] = 1; shadow_written_collected(&page4, 1); });
    CHECK(st_wbd_known_collected == 1);
    // (a page told by a capture and by the collection is counted once, under the capture)
    review_case(4 * 1024 + 1, [&] { guest[4 * 1024 + 1] = 1; shadow_written(&page4, 1); shadow_written_collected(&page4, 1); });
    CHECK(st_wbd_known_game == 1 && st_wbd_known_collected == 0);
    // a write the shadows are not told of: the mirror equals the stale shadow, and the CPU's word stays (what the reports are for)
    review_case(6, [&] { Host::gpu_write(kLo + 6 * 4, &one, 4); }, false);
    // the GPU's own write past the mirror (a written binding given the guest buffer): its pages forgotten, read again
    {
        reset();
        guest[1024 + 2] = 7;   // (the GPU wrote guest memory itself)
        shadow_forget(imported.begin()->second, 4096 + 8, 4);
        write_back(4096, 4096);
        CHECK(guest[1024 + 2] == 0 && filled == 1);   // (as the whole copy: the mirror's 0)
        reset();
        guest[1024 + 2] = 7;
        write_back(4096, 4096);
        CHECK(guest[1024 + 2] == 7);   // (not forgotten: left, where the whole copy writes 0)
    }
    // a page of another pool, or of none: no shadow's, and nothing breaks
    {
        reset();
        const uint32_t elsewhere = 0x7fff0;
        shadow_written(&elsewhere, 1);
        CHECK(guest_cpu_write(kLo - 4096, &one, 4) == 0);   // (the host refuses it: not mapped)
        write_back(0, 4096);
        CHECK(filled == 0 && written == 0);
    }
    // more pages than the list keeps: every page unknown at the next write-back
    {
        reset();
        guest[9] = 1;   // (a write lost among the overflow)
        std::vector<uint32_t> many(1u << 20, 0x7fff0);
        for (int i = 0; i < 10; i++) shadow_written(many.data(), (int64_t)many.size());
        CHECK(shadow_written_all);
        write_back(0, kPages * 4096);
        CHECK(guest[9] == 0 && filled == (int64_t)kPages && !shadow_written_all);
    }
    // with the diff off nothing is listed
    {
        reset();
        diff_on = false;
        guest_cpu_write(kLo, &one, 4);
        shadow_written(&page4, 1), shadow_written_collected(&page4, 1);
        CHECK(shadow_written_by_replay.empty() && shadow_written_pages.empty() && shadow_written_collected_list.empty());
        diff_on = true;
    }
    // and against the whole copy over random writes: the same guest memory after every write-back
    {
        reset();
        std::vector<uint32_t> g2(kWords, 0);   // (guest memory under whole copies)
        uint64_t seed = 0x9e3779b97f4a7c15ull;
        const auto rnd = [&](uint64_t n) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (seed >> 33) % n; };
        for (int step = 0; step < 4000; step++) {
            for (int k = 0; k < 3; k++) {   // (CPU writes: the replay's and the game's; the mirror gets them at its sync)
                const uint64_t w = rnd(kWords);
                const uint32_t v = (uint32_t)rnd(3);
                if (rnd(2)) {
                    CHECK(guest_cpu_write(kLo + w * 4, &v, 4) == 1);
                } else {
                    guest[w] = v;
                    const uint32_t pg = (uint32_t)((kLo >> 12) + w / 1024);
                    rnd(2) ? shadow_written(&pg, 1) : shadow_written_collected(&pg, 1);
                }
                g2[w] = v, mirror[w] = v;
            }
            for (int k = 0; k < 6; k++) mirror[rnd(kWords)] = (uint32_t)rnd(3);   // (the GPU's results: few values, often the old one)
            const uint64_t pg = rnd(kPages), n = 1 + rnd(kPages - pg);
            write_back(pg * 4096, n * 4096);
            for (uint64_t w = pg * 1024; w < (pg + n) * 1024; w++) g2[w] = mirror[w];
            CHECK(guest == g2);
        }
    }
    std::puts("PASS: after CPU writes the write-back diff leaves guest memory as the whole copy does");
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'stale.cpp'
        exe = Path(tmp) / 'stale.exe'
        src.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
