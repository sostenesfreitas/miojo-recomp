"""A vertex stream bound earlier is not looked up again until its buffer can change.

python -I -B tests/test_vertex_bind.py [--cxx g++]
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
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    mem = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    fn = block(draw, "bool vertex_bind_reuse(")
    streams = block(draw, "for (uint32_t b = 0; b < 16; b++)")
    ask = streams.index("vertex_bind_reuse(")
    look = streams.index("gpu_buffer_bind(")
    if ask > look:
        raise SystemExit("vertex streams look the pool up before checking the stream already bound")
    made = block(mem, "bool mirror_make(Pool& pool) {")
    on = made.index("p->mstate = Pool::MIRROR_ON;")
    if "pools_gen.fetch_add" not in made[on:on + 240]:
        raise SystemExit("a new mirror leaves a remembered vertex buffer pointing at guest memory")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
int main() {{
    CHECK(vertex_bind_reuse(0x1000, 64, 3, 0x1000, 64, 3, 1));
    CHECK(!vertex_bind_reuse(0x1000, 64, 4, 0x1000, 64, 3, 1));
    CHECK(!vertex_bind_reuse(0x1000, 32, 3, 0x1000, 64, 3, 1));
    CHECK(!vertex_bind_reuse(0x2000, 64, 3, 0x1000, 64, 3, 1));
    CHECK(!vertex_bind_reuse(0x1000, 64, 3, 0x1000, 64, 3, 0));
    CHECK(!vertex_bind_reuse(0, 64, 3, 0, 64, 3, 1));
    std::printf("PASS: a vertex stream stays bound until the pool's buffer changes\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="vertex-bind-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
