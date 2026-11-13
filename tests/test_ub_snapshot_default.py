"""The uniform-buffer snapshot default, game-free: the actual selection code from gpu/vk/vk_draw.cpp, compiled.

On an integrated GPU (or Intel) whose buffer pools have no VRAM mirror, a uniform buffer bound in place is the game's own
memory, which it rewrites right after the submit; the copy as submitted is the default there. With mirrors, or on a
discrete GPU without them (GCN since #2343, RX_VK_MIRROR=0), it stays in place. Set, RX_VK_UB_SNAPSHOT reads as env_on does
(gpu/vk/compat/hookshot_env.h): 1, on, true, yes force the copy; 0, off, false, no, empty force the pool.

    python -I -B tests/test_ub_snapshot_default.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "the fixture sets the switch with POSIX setenv: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
MARKER = "static const bool snapshot = [] {"


def selection(source):
    start = source.index(MARKER) + len("static const bool snapshot = ")
    end = source.index("}();", start) + 3
    return source[start:end]


FIXTURE = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
enum { VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2 };
static struct { struct { uint32_t vendorID; int deviceType; } props; } dev;
static bool mirrors;
static char logged[256];
[[maybe_unused]] static bool pools_mirrored() { return mirrors; }
static void logf(const char* f, ...) { std::snprintf(logged, sizeof logged, "%s", f); }
static bool pick() { return SELECTION; }
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    mirrors = argv[1][0] == '1';
    dev.props.deviceType = argv[2][0] == 'i' ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    dev.props.vendorID = (uint32_t)std::strtoul(argv[3], nullptr, 16);
    if (std::strcmp(argv[4], "unset")) setenv("RX_VK_UB_SNAPSHOT", argv[4], 1); else unsetenv("RX_VK_UB_SNAPSHOT");
    const bool on = pick();
    if (!std::strstr(logged, "uniform buffers")) return 3;
    std::puts(on ? "copy" : "pool");
    return 0;
}
'''

# (mirrors, integrated or discrete, vendor, RX_VK_UB_SNAPSHOT, expected)
CASES = [("0", "i", "1002", "unset", "copy"),    # (an AMD APU, the Steam Deck: no mirrors)
         ("0", "i", "8086", "unset", "copy"),    # (an Intel iGPU, #2464)
         ("0", "d", "8086", "unset", "copy"),    # (an Intel discrete card without mirrors, #2464)
         ("0", "d", "1002", "unset", "pool"),    # (GCN, mirrorless since #2343: not measured)
         ("1", "d", "10de", "unset", "pool"), ("1", "i", "1002", "unset", "pool"),
         ("0", "d", "1002", "1", "copy"), ("0", "d", "1002", "on", "copy"), ("0", "d", "1002", "yes", "copy"),
         ("1", "d", "10de", "true", "copy"),
         ("0", "i", "1002", "0", "pool"), ("0", "i", "1002", "off", "pool"), ("0", "i", "1002", "no", "pool"),
         ("0", "i", "1002", "false", "pool"), ("0", "i", "1002", "", "pool")]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    a = p.parse_args()
    code = FIXTURE.replace("SELECTION", selection(a.source.read_text(encoding="utf-8")))
    with tempfile.TemporaryDirectory(prefix="ub-snapshot-") as tmp:
        cpp, exe = Path(tmp) / "t.cpp", Path(tmp) / "t"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Werror", "-Wno-format-security",
                        "-include", str(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h"), str(cpp), "-o", str(exe)],
                       check=True)
        for mirrors, kind, vendor, value, want in CASES:
            got = subprocess.run([str(exe), mirrors, kind, vendor, value], capture_output=True, text=True,
                                 check=True).stdout.strip()
            if got != want:
                raise SystemExit("mirrors=%s %s vendor %s RX_VK_UB_SNAPSHOT=%r: %s, expected %s" %
                                 (mirrors, kind, vendor, value, got, want))
    print("PASS: uniform buffers copied by default on integrated GPUs (and Intel) without mirrors; the switch forces either way")


if __name__ == "__main__":
    main()