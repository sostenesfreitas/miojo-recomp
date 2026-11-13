"""vk_draw.cpp's pcache_file_read: a pipeline cache of this device and driver is read byte for byte; one of another (or a
truncated or short file) is dropped by its 32-byte header, before the rest of the file is read.

python -I -B tests/test_pcache_header_read.py --cxx <compiler> --headers <Vulkan-Headers include dir>
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

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
    parser.add_argument("--headers", required=True)
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    reader = function(source, "PipelineCacheFile pcache_file_read(const std::string& path)")
    fixture = r'''
#define _CRT_SECURE_NO_WARNINGS   /* (Windows: fopen is not deprecated here) */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>
#ifndef _WIN32   /* (Windows has _fseeki64 and _ftelli64 themselves, and no fseeko) */
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while (0)
struct { VkPhysicalDeviceProperties props; } dev;
static size_t bytes_read;
#ifdef _WIN32
#include <windows.h>
static BOOL counted_read(HANDLE h, void* p, DWORD n, DWORD* got, OVERLAPPED* o) {
    BOOL result = ReadFile(h, p, n, got, o); if (result) bytes_read += *got; return result;
}
#define ReadFile counted_read
#else
#include <unistd.h>
static ssize_t counted_read(int fd, void* p, size_t n, off_t offset) {
    ssize_t result = pread(fd, p, n, offset); if (result > 0) bytes_read += (size_t)result; return result;
}
#define pread counted_read
#endif
#include "pipeline_cache_file.h"
using mkwvk::PipelineCacheFile;
''' + reader + r'''
static std::vector<uint8_t> cache(uint32_t vendor, uint32_t device, uint8_t uuid0, size_t body, uint32_t head_size = 32) {
    std::vector<uint8_t> d(32 + body);
    const uint32_t version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
    std::memcpy(d.data(), &head_size, 4);
    std::memcpy(d.data() + 4, &version, 4);
    std::memcpy(d.data() + 8, &vendor, 4);
    std::memcpy(d.data() + 12, &device, 4);
    std::memcpy(d.data() + 16, dev.props.pipelineCacheUUID, VK_UUID_SIZE);
    d[16] = uuid0;
    for (size_t i = 32; i < d.size(); i++) d[i] = (uint8_t)(i * 37);
    return d;
}
static std::vector<uint8_t> roundtrip(const std::string& path, const std::vector<uint8_t>& d) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    bytes_read = 0;
    auto mapped = pcache_file_read(path);
    return mapped.empty() ? std::vector<uint8_t>{} : std::vector<uint8_t>(mapped.data(), mapped.data() + mapped.size());
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string path = argv[1];
    dev.props.vendorID = 0x10de;
    dev.props.deviceID = 0x1c8d;
    for (uint32_t i = 0; i < VK_UUID_SIZE; i++) dev.props.pipelineCacheUUID[i] = (uint8_t)(0xa0 + i);
    const uint8_t u0 = dev.props.pipelineCacheUUID[0];
    const size_t big = 1 << 20;

    const std::vector<uint8_t> good = cache(0x10de, 0x1c8d, u0, big);
    CHECK(roundtrip(path, good) == good);

    CHECK(roundtrip(path, cache(0x1002, 0x1c8d, u0, big)).empty() && bytes_read == 32);
    CHECK(roundtrip(path, cache(0x10de, 0x1c8e, u0, big)).empty() && bytes_read == 32);
    CHECK(roundtrip(path, cache(0x10de, 0x1c8d, (uint8_t)(u0 + 1), big)).empty() && bytes_read == 32);
    CHECK(roundtrip(path, cache(0x10de, 0x1c8d, u0, big, 16)).empty() && bytes_read == 32);
    CHECK(roundtrip(path, cache(0x10de, 0x1c8d, u0, 64, 32 + 65)).empty());
    CHECK(roundtrip(path, cache(0x10de, 0x1c8d, u0, 0)).empty());
    CHECK(roundtrip(path, std::vector<uint8_t>(good.begin(), good.begin() + 20)).empty());
    CHECK(roundtrip(path, {}).empty());
    CHECK(pcache_file_read(path + ".missing").empty());
    std::puts("ok");
}
'''
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d) / "t.cpp", Path(d) / "t"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", args.headers, "-I", str(ROOT / "gpu/vk"), str(src), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe), str(Path(d) / "cache.bin")], check=True)
    print("ok")


if __name__ == "__main__":
    main()
