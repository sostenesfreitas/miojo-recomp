# SPDX-License-Identifier: GPL-3.0-or-later
"""A fault inside vkCreateShaderModule leaves that program out, on Windows.

Extracts guarded_shader_module. The stand-in driver raises an access violation after writing a handle; the process
must return -77 and a null module, which is what the log's igvk64 read of address 0x170 did not do.

python -I -B tests/test_shader_module_guard.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"linux": "Windows structured exceptions"}


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


HEAD = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct Dev { uintptr_t device; } dev{0x1234};
using VkResult = int;
using VkShaderModule = uintptr_t;
constexpr VkResult VK_SUCCESS = 0;
constexpr uintptr_t VK_NULL_HANDLE = 0;
struct VkShaderModuleCreateInfo { int sType; const void* pNext; unsigned flags; size_t codeSize; const uint32_t* pCode; };
int mode = 0, calls = 0, logged = 0, faults_noted = 0;
void logf(const char*, ...) { ++logged; }
void note_driver_fault() { ++faults_noted; }
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); std::exit(7); } } while (0)
VkResult vkCreateShaderModule(uintptr_t device, const VkShaderModuleCreateInfo* ci, const void* allocator, VkShaderModule* m) {
    CHECK(device == dev.device && ci && ci->codeSize == 16 && allocator == nullptr && m);
    ++calls;
    if (mode == 1) { *m = 0x170; RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }
    *m = 0xcafe;
    return VK_SUCCESS;
}
'''


def windows_target(cxx):
    machine = subprocess.run([cxx, "-dumpmachine"], capture_output=True, text=True, timeout=30).stdout.strip()
    if "mingw" in machine or "windows-gnu" in machine:
        return ["--target=x86_64-w64-mingw32", "-fms-extensions"]
    return ["--target=x86_64-pc-windows-msvc"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--linker", type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("Windows structured exceptions required")
    source = (args.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    wrapper = block(source, "__declspec(noinline) VkResult guarded_shader_module(")
    cpp = HEAD + wrapper + r'''
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    CHECK(argc == 2);
    VkShaderModuleCreateInfo ci{};
    ci.codeSize = 16;
    VkShaderModule m = 0;
    mode = std::atoi(argv[1]);
    const VkResult r = guarded_shader_module(&ci, &m);
    CHECK(calls == 1);
    if (mode == 0) CHECK(r == VK_SUCCESS && m == 0xcafe && logged == 0 && faults_noted == 0);
    if (mode == 1) CHECK(r == -77 && m == 0 && logged == 1 && faults_noted == 1);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="shader-module-seh-") as tmp:
        path = Path(tmp)
        unit = path / "fixture.cpp"
        exe = path / "fixture.exe"
        unit.write_text(cpp, encoding="utf-8")
        cmd = [args.cxx, *windows_target(args.cxx), "-std=c++20", str(unit), "-o", str(exe), "-fuse-ld=lld"]
        if args.linker:
            cmd += ["-B" + str(args.linker.parent)]
        subprocess.run(cmd, check=True, timeout=120)
        for which in ("0", "1"):
            result = subprocess.run([str(exe), which], capture_output=True, text=True, timeout=30)
            if result.returncode:
                print(result.stdout, end="")
                print(result.stderr, end="")
                raise SystemExit("guarded_shader_module did not survive an access violation (mode %s)" % which)
    print("a fault creating a shader module leaves that program out: ok")


if __name__ == "__main__":
    main()
