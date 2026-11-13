"""GPU-wait accounting, without a game or Vulkan driver.

Execute the actual five recorder-boundary macros against fake driver calls, plus
exact-clock tests of the production counter. --source MAIN uses MAIN's old macros
with this test's counter header: it compiles and fails behaviorally (unmeasured
fence/timeline/idle/query calls), rather than failing for a header MAIN does not have.

python -I -B tests/test_gpu_wait_stats.py [--cxx clang++] [--source path/to/main]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
CALLS = ("vkWaitForFences", "vkWaitSemaphores", "vkQueueWaitIdle", "vkDeviceWaitIdle", "vkGetQueryPoolResults")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--out", type=Path)
    p.add_argument("--syntax-only", action="store_true")
    a = p.parse_args()
    work = (a.source / "gpu/vk/vk_work.h").read_text(encoding="utf-8")
    macros = []
    for name in CALLS:
        found = re.findall(r"^#define " + name + r"\(\.\.\.\) .+$", work, re.M)
        if len(found) != 1:
            raise SystemExit("FAIL: missing or ambiguous boundary macro " + name)
        macros.append(found[0])
    with tempfile.TemporaryDirectory(prefix="gpu-wait-stats-") as tmp:
        out = a.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        (out / "wait_macros.h").write_text("\n".join(macros) + "\n", encoding="utf-8")
        command = [a.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-I" + str(ROOT / "gpu/vk"), "-I" + str(out), str(ROOT / "tests/gpu_wait_stats_test.cpp")]
        if a.syntax_only:
            subprocess.run(command + ["-fsyntax-only"], check=True)
            print("PASS: GPU wait fixture syntax")
            return
        exe = out / "gpu-wait-stats.exe"
        subprocess.run(command + ["-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    reflex = (a.source / "gpu/vk/vk_reflex.cpp").read_text(encoding="utf-8")
    if not re.search(r"const gpu_wait_stats::Exclude pacing;[^\n]*\n\s*vkWaitSemaphores\(dev.device, &w,", reflex):
        raise SystemExit("FAIL: Reflex's latency wait is not excluded as pacing")
    replay = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "GPU-bound wait %.1f ms (recording slots only)" not in replay or "host time in GPU resource waits:" not in replay:
        raise SystemExit("FAIL: old slot meaning and new resource-wait total are not distinguished")
    print("PASS: GPU resource waits, routing, nesting, polls, pacing and concurrent accounting")


if __name__ == "__main__":
    main()
