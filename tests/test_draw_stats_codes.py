"""Record and report native shader keys through the real draw-stat query code; no game or GPU."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
using VkCommandBuffer = uint64_t;
constexpr uint64_t VK_NULL_HANDLE = 0;
struct Program { uint64_t key; uint64_t codes[4]; };
struct DrawArgs {
    bool mesh = false, indexed = false;
    uint64_t indirect = 0, count_addr = 0;
    int32_t count = 3, instances = 1, max = 1, stride = 0;
};
struct NvnState {};
struct Host { void gpu_read(uint64_t, void* p, size_t n) { std::memset(p, 0, n); } } host_value;
Host* host = &host_value;
@META@;
constexpr uint32_t kStatsQueries = 6;
StatsMeta stats_meta[kStatsQueries];
std::atomic<uint32_t> stats_used{0};
std::atomic<int64_t> stats_frame{7};
std::atomic<bool> stats_written{false};
uint64_t stats_pool = 1, frame = 7;
bool enabled = true;
unsigned begins = 0, ends = 0;
const char* stats_dir() { return enabled ? "fixture" : nullptr; }
uint64_t texture_frame_no() { return frame; }
void watch_note(uint64_t, uint64_t) {}
void vkCmdBeginQuery(uint64_t, uint64_t, uint32_t, int) { begins++; }
void vkCmdEndQuery(uint64_t, uint64_t, uint32_t) { ends++; }
@QUERY@;
void StatsQuery::copy_args(VkCommandBuffer, const DrawArgs&, uint32_t) {}
void StatsQuery::copy_watch(VkCommandBuffer, uint32_t) {}
void StatsQuery::gpu_dump(VkCommandBuffer, const NvnState&, uint64_t, bool) {}
@REPORT@
static std::string report(uint32_t n) {
    FILE* f = std::tmpfile();
    assert(f);
    stats_code_keys(f, n);
    std::rewind(f);
    std::string text;
    char b[512];
    while (std::fgets(b, sizeof b, f)) text += b;
    assert(!std::ferror(f));
    std::fclose(f);
    return text;
}
int main() {
    DrawArgs d;
    Program p{0x100, {0x11, 0x22, 0x33, 0x44}};
    enabled = false; { StatsQuery q(1, p, d); }
    enabled = true; stats_pool = 0; { StatsQuery q(1, p, d); }
    stats_pool = 1; frame = 8; { StatsQuery q(1, p, d); }
    frame = 7; stats_written = true; { StatsQuery q(1, p, d); }
    assert(stats_used == 0 && begins == 0 && ends == 0);
    stats_written = false;
    assert(report(0).find("code draw ") == std::string::npos);
    { StatsQuery q(1, p, d); }
    { StatsQuery q(1, p, d); }
    p.codes[0] = 0x55;
    assert(stats_meta[0].codes[0] == 0x11 && stats_meta[0].codes[3] == 0x44);
    { StatsQuery q(1, p, d); } // Same program key, distinct native tuple: preserve both.
    {
        Program mesh{0x200, {0x91, 0x92, 0x93, 0x94}};
        d.mesh = true;
        StatsQuery q(1, mesh, d); // Native mesh keys also survive a generated-vertex fallback.
    }
    {
        Program compute{0x300, {0xa1, 0, 0, 0}};
        StatsQuery q(1, compute, 0, 2, 3, 4);
    }
    {
        Program unknown{0x400, {0, 0, 0, 0}};
        d.mesh = false;
        StatsQuery q(1, unknown, d);
    }
    assert(stats_used == 6 && begins == 6 && ends == 6);
    assert(stats_meta[4].dispatch && stats_meta[4].grid[0] == 2 && stats_meta[4].grid[2] == 4);
    assert(stats_meta[3].codes[3] == 0x94); // Fourth key is the header, not another mesh key.
    { StatsQuery overflow(1, p, d); }
    assert(stats_used == 7 && begins == 6 && ends == 6);
    p.key = 0;
    std::memset(p.codes, 0, sizeof p.codes); // Reporting must not read the original Program.
    const std::string text = report(kStatsQueries);
    assert(text.find("vertex/mesh/compute fragment task header") != std::string::npos);
    assert(text.find("code draw 0000000000000100 2 | 0000000000000011 0000000000000022 0000000000000033 0000000000000044") != std::string::npos);
    assert(text.find("code draw 0000000000000100 1 | 0000000000000055") != std::string::npos);
    assert(text.find("code mesh 0000000000000200 1 | 0000000000000091 0000000000000092 0000000000000093 0000000000000094") != std::string::npos);
    assert(text.find("code comp 0000000000000300 1 | 00000000000000a1 0000000000000000 0000000000000000 0000000000000000") != std::string::npos);
    assert(text.find("code draw 0000000000000400 1 | 0000000000000000 0000000000000000 0000000000000000 0000000000000000") != std::string::npos);
    std::puts("PASS native code snapshots/report, graphics/mesh/compute, lifetime, distinct tuples, inactive/capacity");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    args = parser.parse_args()
    text = args.source.read_text(encoding="utf-8")
    if "void stats_code_keys(FILE* f, uint32_t n)" not in text:
        raise SystemExit("native stage keys are absent from the draw-stat report")
    for call in ("StatsQuery stats_query(cmd, p, d)", "StatsQuery stats_query(cmd, p, indirect, x, y, z)",
                 "if (res == VK_SUCCESS) stats_code_keys(f, n);"):
        assert call in text, "missing production query/report integration: " + call
    source = (FIXTURE.replace("@META@", block(text, "struct StatsMeta {"))
              .replace("@QUERY@", block(text, "struct StatsQuery {"))
              .replace("@REPORT@", block(text, "void stats_code_keys(FILE* f, uint32_t n)")))
    with tempfile.TemporaryDirectory(prefix="draw-stats-codes-") as tmp:
        cpp = Path(tmp) / "check.cpp"
        exe = Path(tmp) / "check.exe"
        cpp.write_text(source, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
                       check=True, timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "PASS native code snapshots/report" in result.stdout, "fixture did not complete"
        print(result.stdout.strip())


if __name__ == "__main__":
    main()
