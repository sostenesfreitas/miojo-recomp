"""The workers of the shader preparation's first part (gpu/vk/prepare_workers.h, vk_draw.cpp prepare_run).

With the NT heap the driver's compiles queue on its one lock, and more workers ran slower: the count stays half the CPUs. With
the segment heap (nx2/win_manifest.py's heapType) every usable CPU gets a worker, as far as the free memory allows. This test
compiles the real prepare_workers.h and checks:
- without the segment heap the count is half_cpus, whatever the memory;
- with it, one worker more for each kPerWorkerMB free past kReserveMB (768 MB past 4 GB), up to the usable CPUs and to kMaxWorkers;
- the heap header's signature: the segment heap's 0xDDEEDDEE at byte 16, the NT heap's (or anything else) is not it.
It also checks that prepare_run sizes its pool with prepare_cpus (in play, behind the game, it keeps its quarter), and that the
game's manifest asks for the segment heap.

python -I -B tests/test_prepare_workers.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include "prepare_workers.h"
#include <cassert>
#include <cstring>

using prepare_workers::count;
using prepare_workers::kMaxWorkers;
using prepare_workers::kPerWorkerMB;
using prepare_workers::kReserveMB;

int main() {
    // (the NT heap: half the CPUs, whatever the memory)
    assert(count(6, 12, 64 * 1024, false) == 6);
    assert(count(6, 12, 0, false) == 6);
    assert(count(0, 1, 64 * 1024, false) == 1);
    assert(count(40, 80, 1 << 20, false) == kMaxWorkers);
    // (the segment heap: every usable CPU when the memory allows it, measured on a 6-core, 12-thread Ryzen with ~10 GB free)
    assert(count(6, 12, 10 * 1024, true) == 12);
    // (a worker more for each kPerWorkerMB past kReserveMB)
    assert(count(6, 12, kReserveMB, true) == 6);
    assert(count(6, 12, kReserveMB - 1, true) == 6);
    assert(count(6, 12, 0, true) == 6);
    assert(count(6, 12, kReserveMB + kPerWorkerMB - 1, true) == 6);
    assert(count(6, 12, kReserveMB + kPerWorkerMB, true) == 7);
    assert(count(6, 12, kReserveMB + 3 * kPerWorkerMB, true) == 9);
    // (never more than the usable CPUs, nor kMaxWorkers)
    assert(count(6, 12, 1 << 30, true) == 12);
    assert(count(6, 6, 1 << 30, true) == 6);
    assert(count(6, 4, 1 << 30, true) == 6);    // (half_cpus is already capped at usable_cpus: as given)
    assert(count(16, 64, 1 << 30, true) == kMaxWorkers);
    assert(count(0, 8, 1 << 30, true) == 8);

    unsigned char heap[32];
    std::memset(heap, 0, sizeof heap);
    const unsigned char segment[4] = {0xEE, 0xDD, 0xEE, 0xDD}, nt[4] = {0xEE, 0xFF, 0xEE, 0xFF};
    std::memcpy(heap + 16, segment, 4);
    assert(prepare_workers::segment_heap_signature(heap));
    std::memcpy(heap + 16, nt, 4);
    assert(!prepare_workers::segment_heap_signature(heap));
    assert(!prepare_workers::segment_heap_signature(nullptr));
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    run = draw[draw.index("bool prepare_run(void (*progress)(size_t done, size_t total), bool background, bool refresh_states) {"):]
    run = run[:run.index("\n}\n")]
    pool = run[run.index("const unsigned quarter"):run.index("threads.emplace_back(worker)")]
    assert "background ? std::min(quarter, cpus) : prepare_cpus()" in pool, \
        "prepare_run sizes its pool before the game with prepare_cpus, in play with its quarter"
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    body = cache[cache.index("unsigned prepare_cpus() {"):]
    body = body[:body.index("\n}\n")]
    assert "prepare_workers::count(half_cpus(), usable_cpus()" in body and "segment_heap()" in body
    manifest = (ROOT / "nx2/win_manifest.py").read_text(encoding="utf-8")
    assert ">SegmentHeap</heapType>" in manifest, "the game's manifest asks for the segment heap"

    with tempfile.TemporaryDirectory(prefix="prepare-workers-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(source),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: prepare workers: half the CPUs on the NT heap; with the segment heap every usable CPU the free memory allows")


if __name__ == "__main__":
    main()
