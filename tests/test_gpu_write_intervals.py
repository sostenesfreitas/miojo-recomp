"""A GPU buffer write into a texture's storage is found under a later, smaller write (gpu/vk/vk_texture.cpp).

texture_gpu_wrote notes each GPU buffer write into guest memory (start -> end, the submission), and gpu_wrote_into finds the
finished ones a texture was not uploaded after: it looks back at only the one record before the texture's address and walks
forward. The writer left records that overlap: after [0x100000, 0x140000) at submission 10 and [0x110000, 0x120000) at 11, a
texture at 0x130000 saw only the second, which ends before it, and kept its old texels. The writer now keeps the records apart.

Both functions and the map are cut from the source and run on such writes: the texture past the inner write finds 10, one
inside it 11, one before it 10; with only 10 finished, the outer ones are found and the inner one waits; a write that runs past
the end of a record leaves the record's first part as it was; a write seen once is not found again; and after each write no
two records overlap, and the map stays within its 65536 records. No game or GPU required.

python -I -B tests/test_gpu_write_intervals.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#define _CRT_SECURE_NO_WARNINGS 1
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>
static int fails;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static void logf(const char*, ...) {}
static uint64_t frame_no, rec_value, completed_value, tl_watch_prog;
struct Texture { uint64_t gpu, guest_size, gpu_wrote_seen; uint32_t id, width, height, nvn_format; };
std::map<uint64_t, std::pair<uint64_t, uint64_t>> gpu_wrote;
std::mutex gpu_wrote_mu;
/* WRITER */
/* READER */

static void write(uint64_t lo, uint64_t hi, uint64_t value) {
    rec_value = value;
    texture_gpu_wrote(lo, hi - lo);
    uint64_t end = 0;
    for (const auto& [start, r] : gpu_wrote) {
        if (start < end) std::printf("FAIL: records overlap at %#llx after the write [%#llx, %#llx)\n",
                                     (unsigned long long)start, (unsigned long long)lo, (unsigned long long)hi), fails++;
        end = std::max(end, r.first);
    }
}
// (the submission gpu_wrote_into finds for a texture of 0x1000 bytes at gpu, seen nothing yet; 0: none)
static uint64_t found(uint64_t gpu) {
    Texture t{gpu, 0x1000, 0, 1, 16, 16, 0};
    return gpu_wrote_into(t) ? t.gpu_wrote_seen : 0;
}

int main() {
    completed_value = 100;
    write(0x100000, 0x140000, 10);
    write(0x110000, 0x120000, 11);
    CHECK(found(0x130000) == 10);   // (past the inner write: the outer one)
    CHECK(found(0x118000) == 11);
    CHECK(found(0x108000) == 10);
    CHECK(found(0x150000) == 0);
    completed_value = 10;   // (11 not finished yet)
    CHECK(found(0x130000) == 10 && found(0x108000) == 10 && found(0x118000) == 0);
    completed_value = 100;
    {   // (seen once, not found again; a later write is)
        Texture t{0x130000, 0x1000, 0, 1, 16, 16, 0};
        CHECK(gpu_wrote_into(t) && t.gpu_wrote_seen == 10);
        CHECK(!gpu_wrote_into(t));
        write(0x130800, 0x130900, 12);
        CHECK(gpu_wrote_into(t) && t.gpu_wrote_seen == 12);
    }
    gpu_wrote.clear();
    write(0x200000, 0x210000, 20);
    write(0x208000, 0x220000, 21);   // (starts inside it and runs past its end)
    CHECK(found(0x204000) == 20 && found(0x20c000) == 21 && found(0x218000) == 21);
    write(0x1f8000, 0x204000, 22);   // (ends inside one)
    CHECK(found(0x1f8000) == 22 && found(0x206000) == 20);
    write(0x1f0000, 0x230000, 23);   // (covers them all)
    CHECK(gpu_wrote.size() == 1 && found(0x20c000) == 23);
    write(0x1f0000, 0x200000, 24);   // (same start, inside: the record takes the new submission, as before)
    CHECK(found(0x1f4000) == 24);
    gpu_wrote.clear();
    for (uint64_t i = 0; i < 65536; i++) gpu_wrote[0x10000000 + i * 0x1000] = {0x10000000 + i * 0x1000 + 0x1000, 30};
    for (uint64_t i = 0; i < 100; i++) write(0x10000000 + i * 0x1000 + 0x400, 0x10000000 + i * 0x1000 + 0x800, 31);
    CHECK(gpu_wrote.size() <= 65536);   // (a write inside one adds two records: the cap still holds)
    return fails ? 1 : 0;
}
'''


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    code = (DRIVER.replace("/* WRITER */", block(src, "void texture_gpu_wrote(uint64_t gpu, uint64_t size) {"))
            .replace("/* READER */", block(src, "bool gpu_wrote_into(Texture& tex) {")))
    with tempfile.TemporaryDirectory(prefix="gpu-write-intervals-") as tmp:
        cpp, exe = Path(tmp) / "intervals.cpp", Path(tmp) / "intervals.exe"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        str(cpp), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: a GPU buffer write is missed by the textures it wrote:\n" + r.stdout + r.stderr)
    print("PASS: the GPU writes are kept apart, and each texture finds the newest finished write into it")


if __name__ == "__main__":
    main()
