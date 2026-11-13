"""The storage-buffer table tracker stops looking once it has seen enough entries.

A game that never logs a binding flip used to keep a mutex and a map on every table
entry of every draw. python -I -B tests/test_table_track.py [--cxx g++]
"""
import argparse
import re
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
    fn = block(source, "bool table_track_open(")
    limit = int(re.search(r"kTableTrackLooks = (\d+)", fn).group(1))
    site = source[source.index("static std::atomic<int> table_told"):source.index("static std::mutex tm")]
    if "table_track_open(" not in site or "table_seen.fetch_add" not in site:
        raise SystemExit("the draw path does not count looks")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
int main() {{
    constexpr int limit = {limit};
    CHECK(table_track_open(0, 0, 0));
    CHECK(table_track_open(59, limit - 1, 0));
    CHECK(!table_track_open(60, 0, 0));
    CHECK(!table_track_open(0, limit, 0));
    CHECK(!table_track_open(0, limit + 1000000, 0));
    CHECK(table_track_open(0, limit + 1000000, 1));
    CHECK(!table_track_open(60, 0, 1));
    std::printf("PASS: the tracker stops after 60 lines or %d looks\\n", limit);
}}
'''
    with tempfile.TemporaryDirectory(prefix="table-track-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
