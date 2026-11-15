"""A program's descriptor counts are kept with it, packed in one word (gpu/vk/vk_draw.cpp desc_counts_pack, build_descriptors).

The two packing functions are cut out of the source and compiled alone: every count comes back as it went in, the three
do not overlap, and counts that do not fit give the "not counted" word, never another count's. The source is then read
for the rules the build keeps: the kept word is used only when it is not ~0, the counts are kept only for a program
that is ready (its stages are final), the switch RX_VK_DESC_COUNTS=0 counts every time, and the reserve that keeps the
writes' pointers stable is made from the counts either way.

python -I -B tests/test_desc_counts.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstddef>
#include <cstdint>
#include <cstdio>

@PACK@
@UNPACK@

int main() {
    const size_t most = (1u << 21) - 1;
    const size_t values[] = {0, 1, 2, 7, 16, 255, 256, 4095, 65536, most - 1, most};
    for (size_t a : values)
        for (size_t b : values)
            for (size_t c : values) {
                const uint64_t w = desc_counts_pack(a, b, c);
                if (w == ~0ull) { printf("%zu %zu %zu: not kept\n", a, b, c); return 1; }
                size_t x = 99, y = 99, z = 99;
                desc_counts_unpack(w, &x, &y, &z);
                if (x != a || y != b || z != c) { printf("%zu %zu %zu came back %zu %zu %zu\n", a, b, c, x, y, z); return 1; }
            }
    if (desc_counts_pack(most + 1, 0, 0) != ~0ull || desc_counts_pack(0, most + 1, 0) != ~0ull ||
        desc_counts_pack(0, 0, most + 1) != ~0ull || desc_counts_pack((size_t)1 << 40, 1, 1) != ~0ull) {
        printf("a count that does not fit was kept\n");
        return 1;
    }
    return 0;
}
"""


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
    src = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    build = block(src, "void build_descriptors(const NvnState& s, const Program& p) {")
    for needle, why in (
        ("if (kept != ~0ull) {", "the kept word is used only when the program has one"),
        ("p.ready.load(std::memory_order_acquire)", "the counts are kept only for a program that is ready"),
        ("if (packed != ~0ull) p.desc_counts.store(", "counts that do not fit are not kept"),
        ('getenv("RX_VK_DESC_COUNTS")', "the switch is read"),
        ("out.bufs.reserve(nbuf), out.imgs.reserve(nimg), out.views.reserve(nview);", "the reserve is made from the counts"),
    ):
        if needle not in build:
            raise SystemExit("build_descriptors: " + why + " (not found: " + needle + ")")
    if "dw()." in build:
        raise SystemExit("build_descriptors asks for the thread's writer again (dw().): it has it in `out`")
    header = (ROOT / "gpu" / "vk" / "vk_shader.h").read_text(encoding="utf-8")
    if "mutable std::atomic<uint64_t> desc_counts{~0ull};" not in header:
        raise SystemExit("vk_shader.h: Program::desc_counts must start as ~0 (not counted)")
    fixture = (FIXTURE.replace("@PACK@", block(src, "uint64_t desc_counts_pack("))
               .replace("@UNPACK@", block(src, "void desc_counts_unpack(")))
    with tempfile.TemporaryDirectory(prefix="desc-counts-") as tmp:
        c = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("ok: a program's descriptor counts, kept and read back")


if __name__ == "__main__":
    main()
