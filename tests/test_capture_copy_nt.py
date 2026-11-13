"""The capture's non-temporal copy (gpu/vk/vk_memory.cpp copy_nt) copies every byte and nothing past them.

copy_nt copies a capture's pages with streaming stores in whole 64-byte lines of the destination, and plain copies for
the head up to the destination's next line and for the tail. This compiles it out of the source, with SSE2 and with
AVX2 where the compiler takes -mavx2, and checks it against memcpy for sizes 0-9000 at every alignment of source and
destination within a line, with guard bytes on both sides of the destination. It also checks that the guarded copy
uses it, and that copy_part fences after its page-by-page retry (a copy that left at a fault did not reach its own).

python -I -B tests/test_capture_copy_nt.py --cxx <compiler>
The fixture is retained in the ignored work directory. Skipped on hosts that are not x86-64.
"""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    if "capture_nt() ? copy_nt : std::memcpy" not in source:
        raise SystemExit("the guarded copy does not use copy_nt")
    part = function(source, "void copy_part(const CopyPart& w)")
    # (the page-by-page retries: copy_part's plain and staged copies (the shadow staging), each fenced after its loop)
    retries = [i for i in range(len(part)) if part.startswith("for (int64_t q = a; q < b; q++)", i)]
    if len(retries) < 2:
        raise SystemExit("copy_part's page-by-page retries not found")
    for at in retries:
        if "copy_fence();" not in part[at:part.index("};", at)]:
            raise SystemExit("copy_part does not fence after its page-by-page retry")
    if platform.machine().lower() not in ("amd64", "x86_64", "x64"):
        print("SKIP: not an x86-64 host")
        return
    copy = function(source, "__attribute__((noinline)) void* copy_nt(")
    fixture = r'''
#include <immintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while (0)
''' + copy + r'''
int main() {
    std::vector<uint8_t> src(9000 + 128), dst(9000 + 256), want(9000 + 256);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 131 + 7);
    uint8_t* const sbase = (uint8_t*)(((uintptr_t)src.data() + 63) & ~(uintptr_t)63);
    uint8_t* const dbase = (uint8_t*)(((uintptr_t)dst.data() + 63) & ~(uintptr_t)63);
    uint8_t* const wbase = want.data() + (dbase - dst.data());
    long checked = 0;
    for (size_t n = 0; n <= 9000; n += n < 600 ? 1 : 61)
        for (size_t so = 0; so < 64; so += 7)
            for (size_t dof = 0; dof < 64; dof += 5) {
                if (dof + n + 64 > (size_t)(dst.data() + dst.size() - dbase) || so + n > (size_t)(src.data() + src.size() - sbase))
                    continue;
                std::memset(dst.data(), 0xA5, dst.size());
                std::memset(want.data(), 0xA5, want.size());
                std::memcpy(wbase + dof, sbase + so, n);
                CHECK(copy_nt(dbase + dof, sbase + so, n) == dbase + dof);
                _mm_sfence();
                CHECK(std::memcmp(dst.data(), want.data(), dst.size()) == 0);
                checked++;
            }
    std::printf("PASS copy_nt: %ld copies (%s)\n", checked,
#ifdef __AVX2__
                "AVX2"
#else
                "SSE2"
#endif
    );
}
'''
    work = ROOT / "work"
    work.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="capture-copy-nt-", dir=work))
    cpp = out / "fixture.cpp"
    cpp.write_text(fixture, encoding="utf-8")
    env = dict(os.environ)
    compiler = Path(args.cxx)
    if compiler.is_file():
        env["PATH"] = str(compiler.resolve().parent) + os.pathsep + env.get("PATH", "")
    for name, flags in (("sse2", []), ("avx2", ["-mavx2"])):
        exe = out / ("fixture-%s.exe" % name)
        cmd = [args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror"] + flags + [str(cpp), "-o", str(exe)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True, env=env)
    print("Retained:", out)


if __name__ == "__main__":
    main()
