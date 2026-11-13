"""AMD FSR 4's bind and dispatch hooks (gpu/vk/vk_fsr4.cpp, tune::cmd_bind_pipeline and tune::cmd_dispatch), cut from the
file and built against recording stand-ins for the driver's calls. A dispatch goes by what its bind really bound, also
when a background job publishes the unrolled pipeline between the two:
- the library's pipeline bound, its unrolled one published before the dispatch: the tail is written first (due cleared)
- the unrolled one bound: no tail written (it reads none), due kept for the next reader
- the library's pipeline bound where the tail cannot be read, the unrolled one published before the dispatch: AMD's own
  pipeline is bound in its place
No game or GPU required.

python -I -B tests/test_fsr4_bind_dispatch.py [--cxx g++] --headers <folder holding vulkan/vulkan.h>
"""
RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
static int fails;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static struct { VkDevice device = VK_NULL_HANDLE; } dev;
static void logf(const char*, ...) {}
template <class T> static T handle(uintptr_t v) { return reinterpret_cast<T>(v); }

namespace tune {
struct Plain {};
static const VkPipeline kOwn = handle<VkPipeline>(0x77);   // (AMD's own, as make_plain makes it)
static int plains_made;
VkPipeline make_plain(const Plain&) { return plains_made++, kOwn; }
std::mutex mu;
/* BIG */
/* TAILPIPE */
std::unordered_map<uint64_t, Big> bigs;
std::unordered_set<uint64_t> told;
std::unordered_map<VkPipeline, TailPipe> tail_pipes;
std::atomic<int> tail_count{0};
/* BOUND */
std::unordered_map<VkPipeline, VkPipeline> better;
std::atomic<int> better_count{0};

// (the driver's calls, recorded)
static int updates, barriers, dispatches;
static VkPipeline last_bound;
static void VKAPI_CALL rec_bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline p) { last_bound = p; }
static void VKAPI_CALL rec_dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) { dispatches++; }
static void VKAPI_CALL rec_update(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) { updates++; }
static void VKAPI_CALL rec_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                                   const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                                   const VkImageMemoryBarrier*) { barriers++; }
static void VKAPI_CALL rec_dp(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
PFN_vkCmdBindPipeline real_bind = rec_bind;
PFN_vkCmdDispatch real_dispatch = rec_dispatch;
PFN_vkCmdUpdateBuffer real_update = rec_update;
PFN_vkCmdPipelineBarrier real_barrier = rec_barrier;
PFN_vkDestroyPipeline real_dp = rec_dp;
/* HOOKS */
}  // namespace tune
using namespace tune;

// (one library pipeline that reads the tail of generation 1, its Big as `bound_state` says, and its unrolled one)
static const VkPipeline kLib = handle<VkPipeline>(0x10), kUnrolled = handle<VkPipeline>(0x11);
static void reset(int bound_state) {
    bigs.clear(), told.clear(), tail_pipes.clear(), better.clear(), bound.clear();
    Big& big = bigs[1];
    big.buffer = handle<VkBuffer>(0x20), big.size = 1 << 20, big.tail.assign(1000, 7), big.due = true, big.bound = bound_state;
    tail_pipes[kLib].gen = 1;
    tail_pipes[kLib].plain = std::make_shared<Plain>();
    tail_count = 1, better_count = 0;
    updates = barriers = dispatches = plains_made = 0, last_bound = VK_NULL_HANDLE;
}
static void publish() { better[kLib] = kUnrolled, better_count = 1; }   // (build_unrolled, on its own thread)

int main() {
    const VkCommandBuffer cb = handle<VkCommandBuffer>(0x30);
    // 1. bound before the unrolled one arrived, dispatched after: the library's pipeline runs, so its tail first
    reset(1);
    cmd_bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kLib);
    CHECK(last_bound == kLib);
    publish();
    cmd_dispatch(cb, 1, 1, 1);
    CHECK(updates > 0 && barriers == 1 && !bigs[1].due && dispatches == 1);
    CHECK(last_bound == kLib);

    // 2. the unrolled one bound: it reads no tail, so none is written and the next reader still has it due
    reset(1);
    publish();
    cmd_bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kLib);
    CHECK(last_bound == kUnrolled);
    cmd_dispatch(cb, 1, 1, 1);
    CHECK(updates == 0 && bigs[1].due && dispatches == 1 && last_bound == kUnrolled);

    // 3. the tail cannot be read (its buffer bound without it), the unrolled one arrives between: AMD's own in its place
    reset(-1);
    cmd_bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kLib);
    publish();
    cmd_dispatch(cb, 1, 1, 1);
    CHECK(plains_made == 1 && last_bound == kOwn && updates == 0 && dispatches == 1);
    return fails ? 1 : 0;
}
'''


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", required=True)
    a = p.parse_args()
    src = (ROOT / "gpu/vk/vk_fsr4.cpp").read_text(encoding="utf-8")
    tune = block(src, "namespace tune {")
    # (the per-command-buffer record of binds: the map, and the type of its entries where it has one of its own)
    bound = re.search(r"(?:struct Bind \{.*?\n\};\n)?std::unordered_map<VkCommandBuffer, \w+> bound;", tune, re.S)
    if not bound:
        raise SystemExit("FAIL: tune::bound not found in gpu/vk/vk_fsr4.cpp")
    bound = bound.group(0)
    hooks = "\n".join(block(tune, "VKAPI_ATTR void VKAPI_CALL " + name + "(") for name in ("cmd_bind_pipeline", "cmd_dispatch"))
    code = (DRIVER.replace("/* BIG */", block(tune, "struct Big {") + ";")
            .replace("/* TAILPIPE */", block(tune, "struct TailPipe {") + ";")
            .replace("/* BOUND */", bound).replace("/* HOOKS */", hooks))
    with tempfile.TemporaryDirectory(prefix="fsr4-bind-") as tmp:
        cpp, exe = Path(tmp) / "bind.cpp", Path(tmp) / "bind.exe"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-I" + a.headers, str(cpp), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: FSR 4's bind and dispatch hooks:\n" + r.stdout + r.stderr)
    print("PASS: FSR 4's dispatch goes by what its bind bound: the tail written before the library's pipeline also when "
          "the unrolled one arrives in between, none for the unrolled one, AMD's own where the tail cannot be read")


if __name__ == "__main__":
    main()
