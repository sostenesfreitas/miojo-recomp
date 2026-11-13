"""A repeated read-only buffer bind skips the pool lookup.

Draws bind the same guest ranges again: vertex streams, an index buffer, indirect
arguments, uniform buffers, storage-buffer entries. The binding stands until a pool
leaves, a mirror is made, or the next submission starts.
python -I -B tests/test_bind_memo.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    bind = block(source, "BufferRef gpu_buffer_bind(")
    lock = bind.index("ParLock")
    if bind.index("bind_memo_hit(") > lock:
        raise SystemExit("a read bind takes the pool lock before checking the remembered one")
    if "if (!written)" not in bind[: bind.index("bind_memo_hit(")]:
        raise SystemExit("a written bind can be served from the remembered read")
    store = bind.index("bind_memo[bind_slot(guest, size)] =")
    if "gpu_bind_stamp()" not in bind[store : store + 160]:
        raise SystemExit("a remembered bind keeps the stamp from before the lookup")
    if "if (!p) return {};" not in bind:
        raise SystemExit("a miss is remembered")
    stamp = block(source, "uint64_t gpu_bind_stamp(")
    for needle in ("pools_gen", "mirror_epoch"):
        if needle not in stamp:
            raise SystemExit(f"a remembered bind ignores {needle}")
    made = block(source, "bool mirror_make(Pool& pool) {")
    on = made.index("p->mstate = Pool::MIRROR_ON;")
    nxt = made.index("p->synced_gen", on)
    if "pools_gen" not in made[on:nxt]:
        raise SystemExit("a new mirror does not drop remembered bindings")
    slot = block(source, "uint32_t bind_slot(")
    hit = block(source, "bool bind_memo_hit(")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{slot}
{hit}
int main() {{
    CHECK(bind_slot(0x1000, 64) == bind_slot(0x1000, 64));
    CHECK(bind_slot(0x1000, 64) != bind_slot(0x1000, 128));
    CHECK(bind_slot(0, 64) != bind_slot(1ull << 40, 64));
    int seen[256] = {{}};
    int n = 0;
    for (uint64_t i = 0; i < 64; i++) {{
        const uint32_t s = bind_slot(i << 12, 64);
        CHECK(s < 256);
        if (!seen[s]) n++, seen[s] = 1;
    }}
    CHECK(n > 16);
    int near[256] = {{}};
    int n_near = 0;
    for (uint64_t i = 0; i < 32; i++) {{
        const uint32_t s = bind_slot(0x100000000ull + i * 16, 64);
        CHECK(s < 256);
        if (!near[s]) n_near++, near[s] = 1;
    }}
    CHECK(n_near > 8);
    CHECK(bind_memo_hit(0x1000, 64, 7, 0x1000, 64, 7, 1));
    CHECK(bind_memo_hit(0, 16, 1, 0, 16, 1, 1));
    CHECK(!bind_memo_hit(0x1000, 64, 7, 0x1000, 64, 8, 1));
    CHECK(!bind_memo_hit(0x1000, 32, 7, 0x1000, 64, 7, 1));
    CHECK(!bind_memo_hit(0x2000, 64, 7, 0x1000, 64, 7, 1));
    CHECK(!bind_memo_hit(0x1000, 64, 7, 0x1000, 64, 7, 0));
    CHECK(!bind_memo_hit(0, 0, 1, 0, 0, 1, 1));
    std::printf("PASS: a read bind is reused only for that range and stamp\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="bind-memo-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
