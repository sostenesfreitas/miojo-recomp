"""A draw does not bind vertex streams or an index buffer already on the command buffer.

python -I -B tests/test_draw_binds.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    draw = block(source, "void draw(")
    streams = draw.index("for (uint32_t b = 0; b < 16; b++)")
    if draw.index("vertex_set_same(") > streams:
        raise SystemExit("vertex streams are looked up before checking the set already bound")
    if "vertex_set.on = false" not in draw:
        raise SystemExit("a vertex set is remembered when a stream did not bind")
    if "vsize[i] ? vsize[i] : 1" not in draw:
        raise SystemExit("a vertex set remembers an empty size as empty")
    if "gpu_bind_stamp() == vstamp" not in draw:
        raise SystemExit("a vertex set is remembered across a mirror")
    if draw.count("bind_index(") != 3:
        raise SystemExit("an indexed draw binds its index buffer on its own")
    bound = block(source, "void bind_index(")
    if bound.index("index_bind_same(") > bound.index("vkCmdBindIndexBuffer("):
        raise SystemExit("an index buffer is bound before checking it is already bound")
    if source.count("vkCmdBindIndexBuffer(") != 1:
        raise SystemExit("an index buffer is bound from more than one place")
    begin = block(source, "void draw_begin_recording(")
    if "vertex_set.on = false" not in begin or "last_ib.on = false" not in begin:
        raise SystemExit("a new command buffer keeps the vertex set and index buffer of the last one")
    verts = block(source, "bool vertex_set_same(")
    index = block(source, "bool index_bind_same(")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{verts}
{index}
int main() {{
    uint64_t addr[16]{{}}, size[16]{{}}, caddr[16]{{}}, csize[16]{{}};
    uint32_t stride[16]{{}}, cstride[16]{{}};
    addr[0] = 0x1000; size[0] = 64; stride[0] = 12;
    caddr[0] = 0x1000; csize[0] = 64; cstride[0] = 12;
    CHECK(vertex_set_same(3, addr, size, stride, 3, caddr, csize, cstride, 1));
    CHECK(!vertex_set_same(4, addr, size, stride, 3, caddr, csize, cstride, 1));
    CHECK(!vertex_set_same(3, addr, size, stride, 3, caddr, csize, cstride, 0));
    stride[0] = 16;
    CHECK(!vertex_set_same(3, addr, size, stride, 3, caddr, csize, cstride, 1));
    stride[0] = 12;
    caddr[2] = 0x2000;
    CHECK(!vertex_set_same(3, addr, size, stride, 3, caddr, csize, cstride, 1));
    caddr[2] = 0;
    size[0] = 0; csize[0] = 1;
    CHECK(vertex_set_same(3, addr, size, stride, 3, caddr, csize, cstride, 1));
    csize[0] = 0;
    CHECK(!vertex_set_same(3, addr, size, stride, 3, caddr, csize, cstride, 1));
    size[0] = 64; csize[0] = 64;
    addr[15] = 0x9999;
    CHECK(vertex_set_same(3, addr, size, stride, 3, caddr, csize, cstride, 1));
    addr[15] = 0;
    CHECK(index_bind_same(0x10, 32, 1, 0x10, 32, 1, 1));
    CHECK(!index_bind_same(0x10, 64, 1, 0x10, 32, 1, 1));
    CHECK(!index_bind_same(0x10, 32, 2, 0x10, 32, 1, 1));
    CHECK(!index_bind_same(0x20, 32, 1, 0x10, 32, 1, 1));
    CHECK(!index_bind_same(0x10, 32, 1, 0x10, 32, 1, 0));
    CHECK(!index_bind_same(0, 0, 1, 0, 0, 1, 1));
    std::printf("PASS: a draw keeps the vertex set and index buffer already bound\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="draw-binds-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
