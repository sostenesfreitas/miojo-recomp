"""A mesh fallback's depth-only draws are held while the colour draw of the same culled list waits for its build
(gpu/vk/vk_draw.cpp): the draw roles, RX_VK_FALLBACK_DEPTH_HOLD=0, the hold's 120-frame release and draw()'s order."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
struct Desc { bool is_written = false; };
struct Info {
    bool stores_global_memory = false;
    std::vector<Desc> storage_buffers_descriptors, image_buffer_descriptors, image_descriptors;
};
struct Stage { Info info; };
struct Program { uint32_t mask = 0; std::atomic<int> compute_state{0}; std::vector<Stage> stages; };
struct NvnState { struct { bool dw = false; } ds; };
struct Targets { int n = 0; const void* color[1] = {}; const void* depth = nullptr; };
struct DrawArgs { uint64_t indirect = 0; };
uint32_t target_write_mask(const Program& p, const NvnState&, int) { return p.mask; }
static uint64_t frame = 10;
uint64_t texture_frame_no() { return frame; }
@HELPERS@
#define CHECK(c) if (!(c)) return std::printf("FAIL line %d: %s\n", __LINE__, #c), 1
int main(int argc, char**) {
    using R = FallbackRole;
    int t = 0;
    Program colour{0xf}, depth{0};
    NvnState writes{{true}}, tests{{false}};
    Targets both{1, {&t}, &t}, depth_only{0, {}, &t};
    DrawArgs listed{0x1000}, bare{0};
    if (argc > 1) { CHECK(fallback_role_of(depth, writes, both, listed) == R::None); return 0; }   // switch off
    CHECK(fallback_role_of(colour, tests, both, listed) == R::Colour);
    CHECK(fallback_role_of(depth, writes, both, listed) == R::DepthOnly);
    CHECK(fallback_role_of(depth, writes, depth_only, listed) == R::DepthOnly);
    CHECK(fallback_role_of(depth, tests, both, listed) == R::None);
    CHECK(fallback_role_of(depth, writes, both, bare) == R::None);
    Program writer;
    writer.stages.resize(1);
    writer.stages[0].info.stores_global_memory = true;
    CHECK(writes_memory(writer) && fallback_role_of(writer, writes, both, listed) == R::None);
    writer.stages[0].info = {};
    writer.stages[0].info.storage_buffers_descriptors.push_back({true});
    CHECK(writes_memory(writer) && fallback_role_of(writer, writes, both, listed) == R::None);
    writer.stages[0].info = {};
    writer.stages[0].info.image_buffer_descriptors.push_back({true});
    CHECK(writes_memory(writer) && fallback_role_of(writer, writes, both, listed) == R::None);
    writer.stages[0].info = {};
    writer.stages[0].info.image_descriptors.push_back({true});
    CHECK(writes_memory(writer) && fallback_role_of(writer, writes, both, listed) == R::None);
    CHECK(!fallback_list_held(0x1000));
    fallback_list_hold(0x1000, true);
    CHECK(fallback_list_held(0x1000) && !fallback_list_held(0x2000));
    frame = 130; CHECK(fallback_list_held(0x1000));
    frame = 131; CHECK(!fallback_list_held(0x1000));
    fallback_list_hold(0x1000, true);
    fallback_list_hold(0x1000, false);
    CHECK(!fallback_list_held(0x1000));
    return 0;
}
"""

STEPS = [  # draw(): hold depth, then hold colour only for a failed record whose compute build is still running
    "if (fallback_role == FallbackRole::DepthOnly && fallback_list_held(d.indirect)) SKIP_HERE();",
    "if (p.fallback && d.mesh && !fallback_record(cmd, p, d)) {",
    "if (fallback_role == FallbackRole::Colour && p.compute_state.load(std::memory_order_acquire) == 1)",
    "fallback_list_hold(d.indirect, true);",
    "if (fallback_role == FallbackRole::Colour) fallback_list_hold(d.indirect, false);",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    cxx = parser.parse_args().cxx
    src = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    draw = src.index("void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    helpers = src[src.index("static bool writes_memory"):draw]
    at = draw
    for step in STEPS:
        at = src.index(step, at)   # (ValueError: missing, or out of order)
    with tempfile.TemporaryDirectory(prefix="depth-hold-") as tmp:
        cpp, exe = Path(tmp) / "t.cpp", Path(tmp) / "t.exe"
        cpp.write_text(FIXTURE.replace("@HELPERS@", helpers), encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wno-unused-function", str(cpp), "-o", str(exe)], check=True)
        env = {k: v for k, v in os.environ.items() if k != "RX_VK_FALLBACK_DEPTH_HOLD"}
        subprocess.run([str(exe)], check=True, timeout=10, env=env)
        subprocess.run([str(exe), "off"], check=True, timeout=10, env=dict(env, RX_VK_FALLBACK_DEPTH_HOLD="0"))
    print("PASS: fallback depth hold")


if __name__ == "__main__":
    main()
