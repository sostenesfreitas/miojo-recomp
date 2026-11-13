"""The core hos_reserved_cpus keeps for the replay (host/hos_thread.c pick_reserved_core): the last core of the fastest kind.

On a CPU of one kind every core has the same EfficiencyClass and the last core is kept, as before; on Intel's hybrid CPUs
the efficiency cores come last and have the lower class, so the last performance core is kept instead. None is kept with
fewer than 8 logical processors, nor on a CPU of more than one kind with fewer than 4 cores of the fastest kind.

On Linux the classes come from /sys/devices/cpu_core/cpus, the performance cores' cpulist (parse_cpu_list), and
hos_reserved_cpus asks last_fastest_core there before it falls back to the last core.

python -I -B tests/test_reserved_core.py [--cxx clang++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "g++"]

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
    parser.add_argument('--source', type=Path, default=ROOT / 'host/hos_thread.c')
    args = parser.parse_args()
    helper = block(args.source.read_text(encoding='utf-8'), 'static uint64_t pick_reserved_core(')
    helper += chr(10) + block(args.source.read_text(encoding='utf-8'), 'static uint64_t pick_cache_cores(')
    helper += chr(10) + block(args.source.read_text(encoding='utf-8'), 'static uint64_t parse_cpu_list(')
    source = args.source.read_text(encoding='utf-8')
    reserved = block(source, 'uint64_t hos_reserved_cpus(void)')
    linux = block(source, 'static uint64_t last_fastest_core(int *cores) {\n    *cores = 0;')
    assert '#elif !defined(__EMSCRIPTEN__)' in reserved and reserved.index('last_fastest_core(&cores)', reserved.index('#elif')) > 0, \
        'hos_reserved_cpus no longer asks last_fastest_core on Linux'
    assert '/sys/devices/cpu_core/cpus' in linux and 'parse_cpu_list(' in linux and 'pick_reserved_core(' in linux, \
        "Linux's last_fastest_core no longer reads the performance cores from /sys/devices/cpu_core/cpus"
    fixture = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
// Stub for hos_log used in pick_reserved_core logging
inline void hos_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
''' + helper + r'''
struct Core { uint64_t mask; unsigned char cls; };
uint64_t pick(const Core* cores, int n) {
    uint64_t masks[256];
    unsigned char classes[256];
    for (int i = 0; i < n; i++) { masks[i] = cores[i].mask; classes[i] = cores[i].cls; }
    return pick_reserved_core(masks, classes, n);
}
struct L3 { uint64_t mask, size; int per = 1; };   // (per: logical processors a core, 2 with SMT)
uint64_t cache(const L3* l3, int nl3) {
    uint64_t masks[64], sizes[64], cores[64];
    int ncores = 0;
    for (int i = 0; i < nl3; i++) {
        masks[i] = l3[i].mask; sizes[i] = l3[i].size;
        for (int bit = 0; bit < 64; bit += l3[i].per)
            if (l3[i].mask >> bit & 1) cores[ncores++] = ((1ull << l3[i].per) - 1) << bit;
    }
    return pick_cache_cores(masks, sizes, nl3, cores, ncores);
}
void add(Core* v, int* idx, int n, int per, int first, unsigned char cls) {
    for (int i = 0; i < n; i++) {
        v[*idx].mask = ((1ull << per) - 1) << (first + i * per);
        v[*idx].cls = cls;
        (*idx)++;
    }
}
void classify(Core* v, int n, uint64_t fast) {   // (Linux: class 1 where the core is in cpu_core's list)
    for (int i = 0; i < n; i++) v[i].cls = (v[i].mask & fast) != 0;
}
int main() {
    {   // Linux cpulists
        CHECK(parse_cpu_list("0-7\n") == 0xFF);
        CHECK(parse_cpu_list("0-11,16-19") == 0xF0FFF);
        CHECK(parse_cpu_list("3") == 0x8);
        CHECK(parse_cpu_list("0,2,4-5\n") == 0x35);
        CHECK(parse_cpu_list("60-70") == 0xF000000000000000ull);   // (past 63: left out)
        CHECK(parse_cpu_list("") == 0);
        CHECK(parse_cpu_list("x") == 0);
        CHECK(parse_cpu_list("5-2") == 0);
        CHECK(parse_cpu_list("1;2") == 0);
    }
    {   // the cores that share the largest L3, when the CPU's L3s differ
        const uint64_t MB = 1 << 20;
        L3 l3_1[2] = {{0xFF, 96 * MB, 1}, {0xFF00, 32 * MB, 1}};
        CHECK(cache(l3_1, 2) == 0xFF);                    // Ryzen 9 9950X3D, SMT off: 8 cores
        L3 l3_2[2] = {{0xFFFF, 96 * MB, 2}, {0xFFFF0000, 32 * MB, 2}};
        CHECK(cache(l3_2, 2) == 0xFFFF);                  // 9950X3D / 7950X3D with SMT: 8 cores
        L3 l3_3[2] = {{0xFFFF0000, 96 * MB, 2}, {0xFFFF, 32 * MB, 2}};
        CHECK(cache(l3_3, 2) == 0xFFFF0000);              // (the cache on the second CCD)
        L3 l3_4[2] = {{0xFFF, 96 * MB, 2}, {0xFFF000, 32 * MB, 2}};
        CHECK(cache(l3_4, 2) == 0);                       // 7900X3D with SMT: 6 cores are too few
        L3 l3_5[2] = {{0x3F, 96 * MB, 1}, {0xFC0, 32 * MB, 1}};
        CHECK(cache(l3_5, 2) == 0);                       // 7900X3D, SMT off: the same 6
        L3 l3_6[2] = {{0xFF, 16 * MB, 2}, {0xFFFF00, 8 * MB, 2}};
        CHECK(cache(l3_6, 2) == 0);                       // Ryzen AI 9 HX 370: 4 Zen 5 + 8 Zen 5c, SMT
        L3 l3_7[2] = {{0xF, 16 * MB, 1}, {0xFF0, 8 * MB, 1}};
        CHECK(cache(l3_7, 2) == 0);                       // (the same with SMT off)
        L3 l3_8[2] = {{0xFF, 16 * MB, 2}, {0xFF00, 8 * MB, 1}};
        CHECK(cache(l3_8, 2) == 0);                       // (shared by as many, but 4 cores on the largest)
        L3 l3_9[2] = {{0xFFFF, 32 * MB, 2}, {0xFFFF0000, 32 * MB, 2}};
        CHECK(cache(l3_9, 2) == 0);                       // Ryzen 9 7950X: both CCDs alike
        L3 l3_10[1] = {{0xFFFF, 96 * MB, 2}};
        CHECK(cache(l3_10, 1) == 0);                      // 9800X3D: one L3 for all
        L3 l3_11[1] = {{0xFFFFFF, 30 * MB, 1}};
        CHECK(cache(l3_11, 1) == 0);                      // Intel hybrid: one L3 for all
        CHECK(cache(nullptr, 0) == 0);
        L3 l3_12[3] = {{0xFF, 96 * MB, 1}, {0xFF00, 96 * MB, 1}, {0xFF0000, 32 * MB, 1}};
        CHECK(cache(l3_12, 3) == 0xFFFF);                 // (two of the largest: both)
        {   // (the cores could not be read: none)
            const uint64_t masks[2] = {0xFF, 0xFF00}, sizes[2] = {96 * MB, 32 * MB};
            CHECK(pick_cache_cores(masks, sizes, 2, nullptr, 0) == 0);
        }
    }
    Core v[64];
    int idx = 0;
    add(v, &idx, 12, 2, 0, 0); CHECK(pick(v, idx) == 0xC00000); idx = 0;  // Ryzen 9 3900X
    add(v, &idx, 6, 2, 0, 0); CHECK(pick(v, idx) == 0xC00); idx = 0;      // Ryzen 5 7600X
    add(v, &idx, 8, 1, 0, 0); CHECK(pick(v, idx) == 0x80); idx = 0;       // 8 cores without SMT
    add(v, &idx, 4, 2, 0, 0); CHECK(pick(v, idx) == 0xC0); idx = 0;       // 4 cores with SMT
    add(v, &idx, 6, 1, 0, 0); CHECK(pick(v, idx) == 0); idx = 0;          // 6 cores without SMT
    add(v, &idx, 8, 2, 0, 1); add(v, &idx, 4, 1, 16, 0); CHECK(pick(v, idx) == 0xC000); idx = 0;  // i7-12700K
    add(v, &idx, 8, 2, 0, 1); add(v, &idx, 16, 1, 16, 0); CHECK(pick(v, idx) == 0xC000); idx = 0;  // i9-13900K
    add(v, &idx, 4, 1, 0, 0); add(v, &idx, 4, 2, 4, 1); CHECK(pick(v, idx) == 0xC00); idx = 0;     // efficiency first
    add(v, &idx, 4, 1, 0, 1); add(v, &idx, 4, 1, 4, 0); CHECK(pick(v, idx) == 0x8); idx = 0;       // Lunar Lake
    add(v, &idx, 2, 2, 0, 1); add(v, &idx, 8, 1, 4, 0); CHECK(pick(v, idx) == 0); idx = 0;         // i5-1235U
    add(v, &idx, 2, 2, 0, 1); add(v, &idx, 4, 1, 4, 0); CHECK(pick(v, idx) == 0); idx = 0;         // i3-1215U
    add(v, &idx, 3, 2, 0, 1); add(v, &idx, 8, 1, 6, 0); CHECK(pick(v, idx) == 0); idx = 0;         // 3 fast cores
    CHECK(pick(nullptr, 0) == 0);                                       // empty
    // Linux, classes from /sys/devices/cpu_core/cpus
    add(v, &idx, 20, 1, 0, 0); classify(v, idx, parse_cpu_list("0-7\n")); CHECK(pick(v, idx) == 0x80); idx = 0;  // Core Ultra 7 265KF
    add(v, &idx, 8, 2, 0, 0); add(v, &idx, 4, 1, 16, 0); classify(v, idx, parse_cpu_list("0-15\n"));
    CHECK(pick(v, idx) == 0xC000); idx = 0;                                                                         // i7-12700K
    add(v, &idx, 2, 2, 0, 0); add(v, &idx, 8, 1, 4, 0); classify(v, idx, parse_cpu_list("0-3\n"));
    CHECK(pick(v, idx) == 0); idx = 0;                                                                              // i5-1235U
    add(v, &idx, 8, 2, 0, 0); v[idx].mask = 0; v[idx].cls = 1; idx++; CHECK(pick(v, idx) == 0xC000); // empty mask
    puts("PASS: the last core of the fastest kind with 4+ of them, the last core on CPUs of one kind, none under 8 "
          "logical processors");
}
'''
    with tempfile.TemporaryDirectory(prefix='reserved-core-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()