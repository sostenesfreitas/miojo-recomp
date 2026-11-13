"""The Insert menu's upscale label stays inside its three names, and RX_VK_UPSCALE stays inside the row.

python -I -B tests/test_upscale_label.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    fn = block(source, "const char* tx_upscale(int v) {")
    if "return t[0]" not in fn:
        raise SystemExit("tx_upscale reads t[v] for every v")
    env = source[source.index('getenv("RX_VK_UPSCALE")'):source.index('getenv("HOS_REALTIME_STEP")')]
    for need in ("std::min", "std::max", "r.lo", "r.hi"):
        if need not in env:
            raise SystemExit("RX_VK_UPSCALE is stored without the row's %s" % need)
    fixture = r'''#include <cstdio>
#include <cstring>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
''' + fn + r'''
int main() {
    CHECK(std::strcmp(tx_upscale(0), "Off") == 0);
    CHECK(std::strcmp(tx_upscale(1), "Catmull-Rom") == 0);
    CHECK(std::strcmp(tx_upscale(2), "Lanczos") == 0);
    CHECK(std::strcmp(tx_upscale(-1), "Off") == 0);
    CHECK(std::strcmp(tx_upscale(3), "Off") == 0);
    CHECK(std::strcmp(tx_upscale(99), "Off") == 0);
    std::printf("PASS: upscale labels for 0..2 and for a setting past the row\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="upscale-label-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
