"""Overlapping inline GPU writes retain stream order, including with barrier deduplication.

Compile the real gpu_write, full_barrier, note_work and work wrappers with a recording spy.
No GPU or game files. Removing the write's barrier or its work count must fail.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    if source.count(marker) != 1:
        raise SystemExit("source marker not found once: " + marker)
    start = source.index(marker)
    end = source.index("\n}\n", start) + 2
    return source[start:end]


PREFIX = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
namespace mkwvk {
const VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)7;
const VkBuffer buffer = (VkBuffer)(uintptr_t)1;
constexpr uint64_t base = 0x1000;
std::vector<uint8_t> memory(131104);
struct Range { uint64_t lo, hi; };
std::vector<Range> pending;
unsigned hazards, updates, barriers, fallbacks;
std::atomic<uint64_t> work_epoch{0};
std::atomic<bool> par_active{false};
std::atomic<int64_t> st_barriers_left_out{0}, st_barriers_made{0};
VkCommandBuffer last_barrier_cmd{};
uint64_t last_barrier_epoch = 0;
bool env_on(const char*) { return false; }
struct GprofSpan { GprofSpan(VkCommandBuffer, const char*) {} };
struct BufferRef { VkBuffer buffer; uint64_t offset; explicit operator bool() const { return buffer != VK_NULL_HANDLE; } };
BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t n, bool write) {
    assert(write);
    return gpu >= base && gpu - base <= memory.size() && n <= memory.size() - (gpu - base)
        ? BufferRef{buffer, gpu - base} : BufferRef{};
}
void guest_cpu_write(uint64_t, const void*, uint64_t) { ++fallbacks; }
namespace rec {
void rx_vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* d) {
    assert(cmd == cb && d->memoryBarrierCount == 1);
    const auto& b = *d->pMemoryBarriers;
    if ((b.srcStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) &&
        (b.dstStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) &&
        (b.srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT) &&
        (b.dstAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT)) pending.clear();
    ++barriers;
}
void rx_vkCmdUpdateBuffer(VkCommandBuffer cmd, VkBuffer target, VkDeviceSize offset, VkDeviceSize n, const void* data) {
    assert(cmd == cb && target == buffer && n <= 65536 && !(offset & 3) && !(n & 3));
    assert(offset + n <= memory.size());
    for (auto r : pending) if (offset < r.hi && r.lo < offset + n) ++hazards;
    pending.push_back({offset, offset + n}); ++updates;
    std::memcpy(memory.data() + offset, data, n);
}
}
'''

SUFFIX = r'''
void reset() {
    pending.clear(); hazards = updates = barriers = fallbacks = 0;
    last_barrier_cmd = VK_NULL_HANDLE;
    std::fill(memory.begin(), memory.end(), 0);
}
int check() {
    std::vector<uint8_t> first(65536 + 16, 0x11), second(64, 0x22);
    reset();
    full_barrier(cb, "test"); full_barrier(cb, "test");
    assert(barriers == 1);  // the test really exercises deduplication
    gpu_write(cb, base + 64, first.data(), 16);
    gpu_write(cb, base + 72, second.data(), 64);
    const unsigned overlap = hazards;
    assert(updates == 2 && !fallbacks);
    for (unsigned i = 0; i < 8; ++i) assert(memory[64 + i] == 0x11);
    for (unsigned i = 0; i < 64; ++i) assert(memory[72 + i] == 0x22);
    reset();
    gpu_write(cb, base, first.data(), first.size());
    assert(updates == 2 && !hazards && std::equal(first.begin(), first.end(), memory.begin()));
    gpu_write(cb, base + 65536, second.data(), 16);
    const unsigned chunk_overlap = hazards;
    assert(updates == 3 && std::equal(second.begin(), second.begin() + 16, memory.begin() + 65536));
    reset();
    gpu_write(cb, base + 1, first.data(), 4);
    gpu_write(cb, base, first.data(), 3);
    gpu_write(cb, 0, first.data(), 4);
    gpu_write(cb, base, nullptr, 0);
    assert(fallbacks == 3 && !updates && !barriers);
    if (overlap || chunk_overlap) {
        std::puts("FAIL: overlapping inline GPU writes are unordered"); return 1;
    }
    std::puts("PASS: overlapping writes, chunks, fallbacks and barrier deduplication"); return 0;
}
}
int main() { return mkwvk::check(); }
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", required=True, type=Path)
    args = parser.parse_args()
    vk = ROOT / "gpu/vk"
    write = block((vk / "vk_replay.cpp").read_text(encoding="utf-8"), "void gpu_write(")
    barrier = block((vk / "vk_texture.cpp").read_text(encoding="utf-8"), "void full_barrier(VkCommandBuffer cmd, const char* why)")
    declarations = [line for line in (vk / "vk_texture.h").read_text(encoding="utf-8").splitlines()
                    if line.startswith("void full_barrier(")]
    if len(declarations) != 1:
        raise SystemExit("vk_texture.h: full_barrier declaration not found once")
    work = (vk / "vk_work.h").read_text(encoding="utf-8")
    note = block(work, "inline void note_work()")
    wrappers = []
    for name in ("vkCmdPipelineBarrier2", "vkCmdUpdateBuffer"):
        lines = [line for line in work.splitlines() if line.startswith("#define " + name + "(")]
        if len(lines) != 1:
            raise SystemExit("vk_work.h: wrapper not found once: " + name)
        wrappers.append(lines[0])
    source = PREFIX + note + "\n}\n" + "\n".join(wrappers) + "\nnamespace mkwvk {\n" + declarations[0] + "\n" + barrier + write + SUFFIX
    variants = [("current", source, 0),
                ("missing_barrier", source.replace("full_barrier(cmd);", ""), 1),
                ("missing_work_count", source.replace("::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdUpdateBuffer", "::mkwvk::rec::rx_vkCmdUpdateBuffer"), 1)]
    with tempfile.TemporaryDirectory(prefix="inline-write-") as tmp:
        for name, text, expected in variants:
            if name != "current" and text == source:
                raise SystemExit("negative-control source moved: " + name)
            cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + (".exe" if os.name == "nt" else ""))
            cpp.write_text(text, encoding="utf-8")
            cmd = [args.cxx, "-std=c++20", "-O2", "-Wno-missing-field-initializers", "-I" + str(args.headers), str(cpp), "-o", str(exe)]
            if os.name == "nt": cmd.append("-static")
            subprocess.run(cmd, check=True, timeout=60)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            if result.returncode != expected or (expected and "overlapping inline GPU writes are unordered" not in result.stdout):
                raise SystemExit(name + ": unexpected result\n" + result.stdout + result.stderr)
            print(name + ": " + result.stdout.strip())


if __name__ == "__main__":
    main()
