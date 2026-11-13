"""A GPU that stops finishing work ends the game with a report (gpu/vk/vk_replay.cpp gpu_hang_watch_start).

NVIDIA's Xid 109 (CTX SWITCH TIMEOUT) ended the game's GPU context and no Vulkan call came back VK_ERROR_DEVICE_LOST
afterwards: device_lost() never ran and the game froze with no report. The watchdog reads the timeline once a second and
goes to device_lost() when the device reads as lost, or when work sent to the queue makes no progress for
RX_VK_GPU_HANG_SEC seconds. gpu_hang_watch_start() is cut into a fixture with the timeline, the sent value and
device_lost() as stubs, and run three ways: a timeline stuck behind what was sent must end the process after the limit,
a lost device at once, and a timeline that keeps moving never.

python -I -B tests/test_gpu_hang_watch.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("marker not found once: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
typedef int VkResult;
typedef void* VkDevice;
typedef void* VkSemaphore;
const VkResult VK_SUCCESS = 0, VK_ERROR_DEVICE_LOST = -4;
struct { VkDevice device; } dev;
VkSemaphore timeline = (VkSemaphore)1;
std::atomic<uint64_t> timeline_sent{10};
static const char* mode = "";
static std::atomic<uint64_t> now_done{5};
VkResult vkGetSemaphoreCounterValue(VkDevice, VkSemaphore, uint64_t* v) {
    if (!std::strcmp(mode, "lost")) return VK_ERROR_DEVICE_LOST;
    if (!std::strcmp(mode, "moving")) now_done++, timeline_sent = now_done + 5;
    *v = now_done;
    return VK_SUCCESS;
}
void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    std::printf("\n");
    std::fflush(stdout);
}
typedef int HANDLE;
static HANDLE GetCurrentThread() { return 0; }
static void SetThreadDescription(HANDLE, const wchar_t*) {}
[[noreturn]] void device_lost(const char* why = nullptr) {
    std::printf("device_lost: %s\n", why ? why : "(VK_ERROR_DEVICE_LOST)");
    std::fflush(stdout);
    std::_Exit(3);
}
@@WATCH@@
int main(int argc, char** argv) {
    mode = argc > 1 ? argv[1] : "";
    gpu_hang_watch_start();
    gpu_hang_watch_start();   // (once only)
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    std::printf("still running\n");
    return 0;
}
"""


def run(exe, mode, limit):
    env = dict(os.environ, RX_VK_GPU_HANG_SEC=str(limit))
    t0 = time.monotonic()
    r = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=30, env=env)
    return r.returncode, r.stdout, time.monotonic() - t0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    if "if (presents == 1) hang_watch_start(), gpu_hang_watch_start();" not in replay:
        raise SystemExit("FAIL: the GPU hang watch is not started at the first present")
    for site in ("note_timeline_sent(pend.signal)", "note_timeline_sent(signal.value)"):
        if site not in replay:
            raise SystemExit("FAIL: a submission that signals the timeline is not counted as sent: " + site)
    with tempfile.TemporaryDirectory(prefix="gpu-hang-watch-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        src.write_text(FIXTURE.replace("@@WATCH@@", block(replay, "void gpu_hang_watch_start() {")), encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-O1", str(src), "-o", str(exe)] + ([] if os.name == "nt" else ["-pthread"])
        r = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")

        code, out, took = run(exe, "stuck", 2)
        print(out, end="")
        if code != 3 or "device_lost: the GPU hung" not in out or not 1.5 < took < 3.4:
            raise SystemExit("FAIL: a stuck timeline did not end the process after the limit (exit %d, %.1f s)" % (code, took))
        if "ends the game (RX_VK_GPU_HANG_SEC)" not in out:
            raise SystemExit("FAIL: the hang watch does not log the limit it settled on")
        code, out, took = run(exe, "lost", 30)
        if code != 3 or took > 2.5 or "(VK_ERROR_DEVICE_LOST)" not in out:
            raise SystemExit("FAIL: a lost device did not end the process at once (exit %d, %.1f s)" % (code, took))
        code, out, _ = run(exe, "moving", 2)
        if code != 0 or "device_lost" in out:
            raise SystemExit("FAIL: a timeline that keeps moving was taken as a hang")
        code, out, _ = run(exe, "stuck", 0)
        if code != 0 or "device_lost" in out or "only logged" not in out:
            raise SystemExit("FAIL: RX_VK_GPU_HANG_SEC=0 ended the process")
    print("PASS: a stuck GPU timeline or a lost device ends the process with the report; a moving one and =0 do not")


if __name__ == "__main__":
    main()
