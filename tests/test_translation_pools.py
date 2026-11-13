"""A stage's translation reuses its thread's emptied pools and commits no 68 MB of IR::Blocks (gpu/vk/vk_shader.cpp; no game/GPU).

Pools, spare_pools, kSparePoolBytes and Translation are cut out of vk_shader.cpp and compiled with Eden's real ObjectPool and stand-ins
for the IR types (an IR::Block of its real ~8 KB). The default chunk of 8192 Blocks was 68 MB committed for every stage,
and fresh pages for each one cost 20-30% of the translation's time.

python -I -B tests/test_translation_pools.py [--cxx clang++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"


def cut(text, start, end):
    """from the line that starts with `start` to the end of the first `end` after it"""
    i = text.index("\n" + start) + 1
    return text[i:text.index(end, i) + len(end)]


FIXTURE = r'''
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>
#include "shader_recompiler/object_pool.h"

static size_t largest_new = 0, big_news = 0;
void* operator new(size_t n) {
    largest_new = n > largest_new ? n : largest_new;
    big_news += n >= 65536;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

static int live = 0;
namespace Shader {
namespace IR {
struct Inst { char bytes[120]; Inst() { live++; } ~Inst() { live--; } };
struct Block {
    char bytes[8288];
    std::vector<Inst*> insts;
    Block() { live++; }
    ~Block() { live--; }
    std::vector<Inst*>& Instructions() { return insts; }
    const std::vector<Inst*>& Instructions() const { return insts; }
};
struct Program { std::vector<Block*> blocks; };
}
namespace Maxwell::Flow {
struct Block { char bytes[112]; Block() { live++; } ~Block() { live--; } };
}
}

#include POOLS

static int failed = 0;
static void check(bool ok, const char* what) {
    if (!ok) failed++, std::fprintf(stderr, "FAIL: %s\n", what);
}
static void fill(Translation& t, int n) {
    for (int i = 0; i < n; i++) {
        auto* block = t.pools->blocks.Create();
        block->insts.push_back(t.pools->insts.Create());
        t.program.blocks.push_back(block);
        (void)t.pools->flow_blocks.Create();
    }
}

int main() {
    Pools* first = nullptr;
    {
        largest_new = 0;
        Translation t;
        first = t.pools.get();
        fill(t, 100);
        check(largest_new <= (size_t)4 << 20, "a stage's pools commit more than 4 MB at once");
    }
    check(live == 0, "a finished translation's IR outlives it");
    {
        big_news = 0;
        Translation vs;
        fill(vs, 100);
        check(vs.pools.get() == first, "the next translation on the thread takes fresh pools");
        check(big_news == 0, "a reused pool allocates again for what it held before");
        Translation fs;
        check(fs.pools.get() != vs.pools.get(), "two translations alive at once share pools");
        fill(fs, 10);
    }
    check(live == 0, "a finished translation's IR outlives it");
    check(spare_pools().size() == 2, "the thread does not keep the pools of both finished translations");
    std::thread([&] {
        Translation t;
        check(t.pools.get() != first, "another thread takes this thread's pools");
        fill(t, 10);
    }).join();
    check(live == 0, "a finished translation's IR outlives it");
    {
        Translation big;
        check(big.pools.get() == first, "the thread's spare pools are not reused");
        fill(big, 1100);
    }
    check(live == 0, "a finished translation's IR outlives it");
    check(spare_pools().size() == 1, "a translation over the size limit keeps its pools");
    check(spare_pools().capacity() >= 3, "the spare list can grow in a destructor");
    if (failed) return 1;
    std::puts("translation pools: ok");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    pools = cut(shader, "struct Pools {", "\n};\n") + cut(shader, "std::vector<std::unique_ptr<Pools>>& spare_pools() {", "\n}\n") + \
        cut(shader, "constexpr size_t kSparePoolBytes", ";\n") + cut(shader, "struct Translation {", "\n};\n")
    with tempfile.TemporaryDirectory(prefix="translation-pools-") as tmp:
        tmp = Path(tmp)
        (tmp / "pools.inc").write_text(pools, encoding="utf-8")
        (tmp / "fixture.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "translation-pools.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-pthread", "-I", str(VK / "eden"), '-DPOOLS="pools.inc"', "-I", str(tmp),
                        str(tmp / "fixture.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
