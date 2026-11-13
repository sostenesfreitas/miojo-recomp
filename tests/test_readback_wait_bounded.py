"""invalidate's readback wait has a limit, game-free: the actual readback_wait_value from gpu/vk/vk_replay.cpp, compiled.

The CPU's read of a GPU result waits for the write-back's timeline value. On the Steam Deck (queues replayed in submit order)
the game's main thread waited there for a value the timeline did not reach while it waited, and the game stopped for good
after a save loaded. The wait now ends after RX_VK_READBACK_WAIT_MS (default 250), says so, and the CPU reads what memory
holds; RX_VK_READBACK_WAIT_MS=0 waits without a limit, as before. invalidate must use it.

    python -I -B tests/test_readback_wait_bounded.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "the fixture sets the switch with POSIX setenv: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
SIGNATURE = "bool readback_wait_value(uint64_t v) {"


def body(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 2]


FIXTURE = r'''
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
typedef int VkResult;
typedef void* VkSemaphore;
typedef void* VkDevice;
enum { VK_SUCCESS = 0, VK_TIMEOUT = 2, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO = 1 };
struct VkSemaphoreWaitInfo { int sType; const void* pNext; unsigned flags; unsigned semaphoreCount;
                             const VkSemaphore* pSemaphores; const uint64_t* pValues; };
static struct { VkDevice device; } dev;
static VkSemaphore timeline = (VkSemaphore)&dev;
enum { FR_VALUE = 7 };
static uint64_t signalled = 10;   // (the timeline's value: what the GPU reached)
static std::atomic<uint64_t> pushed{10}, q_values{12};
static int flushes, unlimited_waits, waits;
static uint64_t last_timeout;
static char logged[512];
static void queue_flush_for(int why) { if (why == FR_VALUE) flushes++; }
static uint64_t completed() { return signalled; }
static void wait_value(uint64_t) { unlimited_waits++; }
static void logf(const char* f, ...) { va_list a; va_start(a, f); std::vsnprintf(logged, sizeof logged, f, a); va_end(a); }
static VkResult vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo* wi, uint64_t timeout) {
    waits++, last_timeout = timeout;
    if (wi->semaphoreCount != 1 || wi->pSemaphores[0] != timeline) return -1;
    return *wi->pValues <= signalled ? VK_SUCCESS : VK_TIMEOUT;
}
FUNCTION
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    if (std::strcmp(argv[1], "unset")) setenv("RX_VK_READBACK_WAIT_MS", argv[1], 1); else unsetenv("RX_VK_READBACK_WAIT_MS");
    const bool reached = readback_wait_value(std::strtoull(argv[2], nullptr, 10));
    std::printf("%d %d %d %d %llu %s\n", reached ? 1 : 0, flushes, waits, unlimited_waits,
                (unsigned long long)last_timeout, std::strstr(logged, "timed out") ? "logged" : "quiet");
    return 0;
}
'''

# (RX_VK_READBACK_WAIT_MS, value) -> reached, flushes, bounded waits, unlimited waits, timeout ns, log
CASES = [
    ("unset", "9", "1 1 1 0 250000000 quiet"),       # (reached: no log)
    ("unset", "11", "0 1 1 0 250000000 logged"),     # (never signalled: ends after 250 ms, said)
    ("40", "11", "0 1 1 0 40000000 logged"),         # (another limit)
    ("0", "11", "1 0 0 1 0 quiet"),                  # (0: the wait without a limit, as before)
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_replay.cpp")
    a = p.parse_args()
    source = a.source.read_text(encoding="utf-8")
    if "readback_wait_value(v);" not in body(source, "void invalidate(uint64_t gpu, uint64_t size) {"):
        raise SystemExit("invalidate's readback wait does not go through readback_wait_value: it may wait for good")
    code = FIXTURE.replace("FUNCTION", body(source, SIGNATURE))
    with tempfile.TemporaryDirectory(prefix="readback-wait-") as tmp:
        cpp, exe = Path(tmp) / "t.cpp", Path(tmp) / "t"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Werror", "-Wno-format-security", str(cpp), "-o", str(exe)],
                       check=True)
        for limit, value, want in CASES:
            got = subprocess.run([str(exe), limit, value], capture_output=True, text=True, check=True).stdout.strip()
            if got != want:
                raise SystemExit("RX_VK_READBACK_WAIT_MS=%s value %s: %r, expected %r" % (limit, value, got, want))
    print("PASS: the readback wait ends after its limit and says so; 0 waits without one")


if __name__ == "__main__":
    main()
