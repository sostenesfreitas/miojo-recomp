"""The prepare's worker pools follow the CPUs the process may use, not every CPU the PC has.

A prepare started on 12 of 24 CPUs (an affinity mask: taskset, a launcher, Process Lasso) ran the prediction and the precompile on
24 threads each. usable_cpus (gpu/vk/vk_shader.cpp) counts the process's affinity mask (GetProcessAffinityMask; on Linux
host/posix's, from sched_getaffinity); half_cpus keeps the translation's half of the PC's CPUs, but never more than usable_cpus.

Checked here: usable_cpus and half_cpus, extracted unmodified, against a stand-in mask and CPU count; and in the source, that the
prepare's pools (prepare_run, state_list_prepare, warm_libraries, predict_run, precompile_run, the program cache readers) are sized
by them and no longer by std::thread::hardware_concurrency alone.

No Vulkan, GPU or game. python -I -B tests/test_prepare_cpus.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

def _with_cache(path):
    """vk_shader.cpp with the program cache it held before (vk_progcache.cpp and .h beside it, outside the translator stamp): definitions first"""
    parts = [path.read_text(encoding="utf-8")]
    for name in ("vk_progcache.cpp", "vk_progcache.h"):
        if (path.parent / name).exists():
            parts.append((path.parent / name).read_text(encoding="utf-8"))
    return "\n".join(parts)



def body(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("FAIL: expected one %r" % marker)
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


CHECK = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
typedef uintptr_t DWORD_PTR;
typedef void* HANDLE;
static unsigned fake_hw = 24;
static uint64_t fake_mask = 0;
static bool fake_ok = true;
static HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
static int GetProcessAffinityMask(HANDLE, DWORD_PTR* proc, DWORD_PTR* sys) {
    *proc = (DWORD_PTR)fake_mask, *sys = ~(DWORD_PTR)0;
    return fake_ok;
}
#include "cpus.inc"
#define CHECK(c) do { if (!(c)) { std::printf("CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main() {
    // every CPU: as before
    fake_hw = 24, fake_mask = (1ull << 24) - 1;
    CHECK(usable_cpus() == 24 && half_cpus() == 12);
    // one core complex of a 7900X (0-5, 12-17): 12 usable, the translation keeps its 12
    fake_mask = 0x3Full | 0x3Full << 12;
    CHECK(usable_cpus() == 12 && half_cpus() == 12);
    // six CPUs: six of each
    fake_mask = 0x3F;
    CHECK(usable_cpus() == 6 && half_cpus() == 6);
    // one CPU: one worker, never none
    fake_mask = 1ull << 7;
    CHECK(usable_cpus() == 1 && half_cpus() == 1);
    // no mask (the call failed, or an empty one): every CPU
    fake_mask = 0;
    CHECK(usable_cpus() == 24);
    fake_ok = false, fake_mask = 0xF;
    CHECK(usable_cpus() == 24 && half_cpus() == 12);
    fake_ok = true;
    // more than 64 CPUs: a mask is one processor group, so it says nothing of the others
    fake_hw = 128, fake_mask = ~0ull;
    CHECK(usable_cpus() == 128 && half_cpus() == 64);
    // a mask wider than the count (hardware_concurrency 0 or wrong): never more than the count
    fake_hw = 4, fake_mask = 0xFF;
    CHECK(usable_cpus() == 4 && half_cpus() == 2);
    fake_hw = 0, fake_mask = 0xFF;
    CHECK(usable_cpus() == 1 && half_cpus() == 1);
    std::printf("usable and half CPUs: ok\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++") or "g++")
    a = p.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    shader = _with_cache(ROOT / "gpu/vk/vk_shader.cpp")
    pieces = [body(shader, "unsigned usable_cpus() {"), body(shader, "unsigned half_cpus() {")]
    code = "\n".join(pieces).replace("std::thread::hardware_concurrency()", "fake_hw")
    if "fake_hw" not in code:
        raise SystemExit("FAIL: usable_cpus and half_cpus no longer read the PC's CPU count")

    for fn, need in (("bool prepare_run(", "half_cpus()"), ("void state_list_prepare(", "half_cpus()"),
                     ("bool warm_libraries(", "half_cpus()"), ("void predict_run(", "usable_cpus()"),
                     ("void precompile_run(", "usable_cpus()")):
        text = body(draw, fn)
        if need not in text:
            raise SystemExit("FAIL: %s does not size its workers by %s" % (fn.split("(")[0], need))
        if "const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());" in text:
            raise SystemExit("FAIL: %s counts every CPU of the PC, not those the process may use" % fn.split("(")[0])
    for fn in ("size_t program_cache_load_some(", "void program_cache_each("):
        if "const unsigned cpus = usable_cpus();" not in body(shader, fn):
            raise SystemExit("FAIL: %s does not size its readers by usable_cpus" % fn.split("(")[0])

    with tempfile.TemporaryDirectory(prefix="hookshot-prepare-cpus-") as temp:
        temp = Path(temp)
        (temp / "cpus.inc").write_text(code + "\n", encoding="utf-8")
        (temp / "check.cpp").write_text(CHECK, encoding="utf-8")
        exe = temp / "check.exe"
        cmd = [a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(temp), str(temp / "check.cpp"), "-o", str(exe)]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode:
            print(result.stdout + result.stderr)
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            return 1
    print("PASS: the prepare's workers follow the CPUs the process may use")
    return 0


if __name__ == "__main__":
    sys.exit(main())
