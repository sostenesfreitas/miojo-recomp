# SPDX-License-Identifier: GPL-3.0-or-later
"""Game-free check of the Linux driver-fault guard: the actual guarded pipeline calls from vk_draw.cpp, compiled with a fake
Vulkan whose compute and graphics creation fault on demand. A fault inside the guarded call must come back as the call's
failure and leave the process able to compile again; a fault outside it must still kill the process.

python -I -B tests/test_linux_driver_fault_guard.py [--cxx clang++] [--expect-fault]
"""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
# (Linux only)
RUN_ALL_SKIP = {"windows": "POSIX signals: the Linux guard"}

ROOT = Path(__file__).resolve().parents[1]
START = "return (VkResult)-77;   // (a fault in the driver, told apart from what it returns)\n    }\n}\n#else\n"
END = "\n#endif\nVkPipeline build_graphics"

HEAD = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <setjmp.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
using VkResult = int;
using VkPipeline = uintptr_t;
using VkPipelineCache = uintptr_t;
constexpr VkResult VK_SUCCESS = 0;
constexpr VkResult VK_ERROR_UNKNOWN = -13;
constexpr uintptr_t VK_NULL_HANDLE = 0;
struct VkComputePipelineCreateInfo { int sType = 0; };
struct VkGraphicsPipelineCreateInfo { int sType = 0; };
struct { uintptr_t device = 0x1234; } dev;
static int mode;        /* (0: compile, 1 and 2: the driver faults) */
static int calls;
static constexpr VkResult DRIVER_FAULT = -77;
static int logged;
void logf(const char *, ...) { ++logged; }
static int faults_noted;
void note_driver_fault() { ++faults_noted; }   /* (vk_draw.cpp: the preparation's watchdog counts them) */
static void fault_now() { *(volatile int *)0x40 = 1; }
VkResult vkCreateComputePipelines(uintptr_t device, VkPipelineCache cache, unsigned count,
                                  const VkComputePipelineCreateInfo *ci, const void *allocator, VkPipeline *out) {
    if (device != dev.device || cache != 0 || count != 1 || !ci || allocator || !out) return -1;
    ++calls;
    if (mode == 1) { *out = 0xdead; fault_now(); }   /* (a handle written, then the fault: not a pipeline) */
    *out = 0xcafe;
    return VK_SUCCESS;
}
VkResult vkCreateGraphicsPipelines(uintptr_t device, VkPipelineCache cache, unsigned count,
                                   const VkGraphicsPipelineCreateInfo *ci, const void *allocator, VkPipeline *out) {
    if (device != dev.device || cache != 0 || count != 1 || !ci || allocator || !out) return -1;
    ++calls;
    if (mode == 2) { *out = 0xdead; fault_now(); }
    *out = 0xbeef;
    return VK_SUCCESS;
}
"""

MAIN = r"""
int main(int argc, char **argv) {
#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);   // (fault-outside ends it by SIGSEGV on purpose: without a core dump, a crash report)
#endif
    const char *what = argc > 1 ? argv[1] : "ok";
    if (!std::strcmp(what, "fault-outside")) { fault_now(); std::printf("fail: the fault returned\n"); return 8; }
    VkComputePipelineCreateInfo ci;
    VkGraphicsPipelineCreateInfo gci;
    VkPipeline pl = 0, gpl = 0;
    if (!std::strcmp(what, "ok")) {
        if (guarded_compute(0, &ci, &pl) != VK_SUCCESS || pl != 0xcafe) { std::printf("fail: compute\n"); return 3; }
        if (guarded_graphics(0, &gci, &gpl) != VK_SUCCESS || gpl != 0xbeef) { std::printf("fail: graphics\n"); return 4; }
        std::printf("ok: both compiled\n");
        return 0;
    }
    if (!std::strcmp(what, "fault")) {
        mode = 1;
        if (guarded_compute(0, &ci, &pl) != DRIVER_FAULT) { std::printf("fail: the fault was not reported\n"); return 5; }
        if (pl != VK_NULL_HANDLE) { std::printf("fail: a pipeline was published\n"); return 6; }
    } else if (!std::strcmp(what, "fault-graphics")) {
        mode = 2;
        if (guarded_graphics(0, &gci, &gpl) != DRIVER_FAULT) { std::printf("fail: the fault was not reported\n"); return 5; }
        if (gpl != VK_NULL_HANDLE) { std::printf("fail: a pipeline was published\n"); return 6; }
    } else {
        std::printf("fail: unknown case %s\n", what);
        return 9;
    }
    mode = 0;
    calls = 0;
    pl = gpl = 0;
    if (guarded_compute(0, &ci, &pl) != VK_SUCCESS || pl != 0xcafe || calls != 1) {
        std::printf("fail: the next compile after the fault did not reach the driver\n");
        return 7;
    }
    std::printf("ok: the fault was reported and the next compile made\n");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    parser.add_argument("--expect-fault", action="store_true",
                        help="unfixed source: a fault in a guarded call must end the process")
    args = parser.parse_args()
    if os.name == "nt":
        raise SystemExit("POSIX signals required")
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    source = args.source.read_text(encoding="utf-8")
    start, end = source.index(START) + len(START), source.index(END)
    guard = source[start:end]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hookshot-fault-guard-") as tmp:
        cpp = Path(tmp) / "guard.cpp"
        cpp.write_text(HEAD + guard + MAIN, encoding="utf-8")
        binary = Path(tmp) / "guard"
        build = subprocess.run([compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-pthread", "-o", str(binary), str(cpp)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if build.returncode:
            raise SystemExit("the guard did not compile:\n" + build.stdout)
        for case in ("ok", "fault", "fault-graphics", "fault-outside"):
            run = subprocess.run([str(binary), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            killed = run.returncode == -signal.SIGSEGV
            guarded_case = case in ("fault", "fault-graphics")
            if case == "fault-outside" or (args.expect_fault and guarded_case):
                if not killed:
                    failures.append(case + ": the fault did not end the process: " + run.stdout.strip())
            elif run.returncode != 0:
                failures.append(case + ": " + (run.stdout.strip() or "exit " + str(run.returncode)))
            else:
                print(case + ": " + run.stdout.strip())
    if failures:
        print("\n".join(failures), file=sys.stderr)
        raise SystemExit(1)
    print("the guard reports a driver fault as the call's failure" if not args.expect_fault
          else "the unfixed source dies on a driver fault")


if __name__ == "__main__":
    main()
