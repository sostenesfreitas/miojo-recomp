"""Run the production buffer/image copy layout against synthetic mapped pool spans."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_xfer.cpp")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--gpu", action="store_true", help="opt-in bounded Vulkan upload/readback on a compatible physical device")
    parser.add_argument("--gpu-device", help="case-insensitive device-name substring for the --gpu probe")
    args = parser.parse_args()
    args.gpu = args.gpu or bool(args.gpu_device)
    source = args.source.read_text(encoding="utf-8")
    fixture = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while (0)
struct Texture {};
struct NvnState { int64_t copy_stride = 0; };
struct BufferRef { VkBuffer buffer{}; uint64_t offset=0, size=0; explicit operator bool() const { return buffer != VK_NULL_HANDLE; } };
uint64_t pool_size, requested;
unsigned binds;
bool expect_written;
void warn(int, const char*) {}
BufferRef gpu_buffer_bind(uint64_t addr, uint64_t size, bool written) {
    ++binds; requested=size;
    CHECK(written == expect_written);
    return addr && size <= pool_size ? BufferRef{(VkBuffer)1, 0, size} : BufferRef{};
}
''' + block(source, "struct Sub {") + ";\n" + "\n".join(
        block(source, marker) for marker in ("VkImageSubresourceLayers layers(", "VkOffset3D offset(",
                                             "VkExtent3D extent(", "bool buffer_side(")) + r'''
void run(uint32_t w, uint32_t h, uint32_t depth, uint32_t layers_n, int bw, int bh,
         uint32_t bpb, int64_t stride, uint64_t available, uint64_t expected, bool valid) {
    Sub s; s.w=w; s.h=h; s.d=depth; s.layers=layers_n; s.bw=bw; s.bh=bh; s.bpb=bpb;
    s.aspect=VK_IMAGE_ASPECT_COLOR_BIT; s.level=2; s.layer=3; s.x=4; s.y=8;
    for (bool written : {false,true}) {
        NvnState state{stride}; BufferRef span; VkBufferImageCopy copy{}; uint64_t size=0;
        pool_size=available; requested=0; binds=0; expect_written=written;
        const bool ok=buffer_side(state,s,0x1000,size,copy,span,written);
        if(ok!=valid) std::fprintf(stderr,"copy %ux%u: requested %llu bytes, pool has %llu, expected span %llu\n",
            w,h,(unsigned long long)requested,(unsigned long long)available,(unsigned long long)expected);
        CHECK(ok == valid);
        if (!valid) continue;
        CHECK(binds==1 && requested==expected && size==expected);
        CHECK(copy.bufferOffset==0 && copy.bufferRowLength==(stride ? stride/bpb*bw : ((w+bw-1)/bw)*bw));
        CHECK(copy.bufferImageHeight==((h+bh-1)/bh)*bh);
        CHECK(copy.imageSubresource.mipLevel==2 && copy.imageSubresource.baseArrayLayer==3 && copy.imageSubresource.layerCount==layers_n);
        CHECK(copy.imageOffset.x==4 && copy.imageOffset.y==8 && copy.imageExtent.width==w && copy.imageExtent.height==h && copy.imageExtent.depth==depth);
    }
}
int main() {
    run(2,2,1,1,1,1,4,16,24,24,true); // no padding is accessed after the final row
    run(2,1,1,1,1,1,4,16,8,8,true);
    run(2,2,1,3,1,1,4,16,88,88,true); // intermediate layers retain their padding
    run(2,2,3,1,1,1,4,16,88,88,true);
    run(5,5,1,1,4,4,16,48,80,80,true); // partial BC edge blocks
    run(2,2,1,1,1,1,4,0,16,16,true);
    run(2,2,1,1,1,1,4,16,23,24,false); // genuinely truncated pool
    run(2,2,1,1,1,1,4,7,32,0,false); // row too short/misaligned
    std::puts("PASS: 8 copy layouts, upload/readback, exact spans and truncated pools");
}
'''
    if args.gpu:
        production = block(source, "struct Sub {") + ";\n" + "\n".join(
            block(source, marker) for marker in ("VkImageSubresourceLayers layers(", "VkOffset3D offset(",
                                                 "VkExtent3D extent(", "bool buffer_side("))
        fixture = (ROOT / "tests/vulkan_copy_span_gpu.cpp").read_text(encoding="utf-8").replace(
            "// PRODUCTION_COPY_LAYOUT", production)
    with tempfile.TemporaryDirectory(prefix="hookshot-copy-span-") as directory:
        cpp, exe = Path(directory) / "test.cpp", Path(directory) / "test.exe"
        cpp.write_text(fixture, encoding="utf-8")
        flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if args.sanitize else []
        if args.gpu:
            flags += ["-Wno-missing-field-initializers"]
            flags += ["-static"] if os.name == "nt" and "mingw" in args.cxx.lower() else []
        libraries = ["-ldl"] if args.gpu and os.name != "nt" else []
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(args.headers), str(cpp), "-o", str(exe), *libraries], check=True)
        command = [str(exe)]
        if args.gpu_device:
            command += ["--device", args.gpu_device]
        subprocess.run(command, check=True, timeout=20 if args.gpu else 10)


if __name__ == "__main__":
    main()
