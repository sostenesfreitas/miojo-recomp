"""The replay-phase clock keeps whole nanoseconds and does not read the performance counter per scope.

Ten QueryPerformanceCounter reads a draw were about 4% of the GPU thread. The phase timers
stamp every draw. python -I -B tests/test_prof_clock.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_internal.h").read_text(encoding="utf-8")
    fn = block(source, "inline int64_t perf_ticks_to_ns(")
    clock = block(source, "inline int64_t prof_now()")
    if "return (int64_t)((double)HOOKSHOT_RDTSC() * scale.ns_per);" not in clock:
        raise SystemExit("prof_now still reads the performance counter on the hot path")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
struct LARGE_INTEGER {{ long long QuadPart; }};
static inline void QueryPerformanceCounter(LARGE_INTEGER* t) {{ t->QuadPart = 1; }}
static inline void QueryPerformanceFrequency(LARGE_INTEGER* t) {{ t->QuadPart = 1000000000LL; }}
#define HOOKSHOT_RDTSC() (0ull)
#define HOOKSHOT_PAUSE() ((void)0)
{fn}
{clock}
int main() {{
    const int64_t big = (1LL << 53) + 3;
    CHECK(perf_ticks_to_ns(big, 1000000000LL) == big);
    CHECK(perf_ticks_to_ns(12345, 10000000LL) == 1234500);
    CHECK(perf_ticks_to_ns(6, 3) == 2000000000LL);
    CHECK(perf_ticks_to_ns(50, 0) == 0);
    std::printf("PASS: a nanosecond counter stays exact, and the phase clock uses the timestamp counter\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="prof-clock-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
