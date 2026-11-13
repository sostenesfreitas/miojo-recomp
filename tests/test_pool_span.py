"""A repeated pool-end lookup in the same guest pool skips the lock.

Storage-buffer entries ask where their pool ends. The locked lookup was about 8% of
replay time before buffer binds grew a cache; the pool-end query still took the lock.
python -I -B tests/test_pool_span.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def before_lock(fn, name):
    lock = fn.index("ParLock")
    head = fn[:lock]
    if "guest_span_cached(" not in head:
        raise SystemExit(f"{name} takes the pool lock before checking the span cache")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    fn = block(source, "bool guest_span_covers(")
    before_lock(block(source, "uint64_t gpu_pool_end("), "gpu_pool_end")
    before_lock(block(source, "uint64_t gpu_pool_start("), "gpu_pool_start")
    sync = block(source, "void memory_sync(")
    swap = sync.index("guest_pools.swap(next);")
    if "guest_pools_gen" not in sync[swap:swap + 200]:
        raise SystemExit("refreshing the guest pools does not drop the span cache")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
int main() {{
    CHECK(guest_span_covers(150, 100, 200, 4, 4));
    CHECK(guest_span_covers(100, 100, 200, 4, 4));
    CHECK(!guest_span_covers(200, 100, 200, 4, 4));
    CHECK(!guest_span_covers(150, 100, 200, 4, 5));
    CHECK(!guest_span_covers(150, 200, 100, 4, 4));
    CHECK(!guest_span_covers(150, 100, 100, 1, 1));
    std::printf("PASS: a pool span is reused only inside that generation\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="pool-span-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
