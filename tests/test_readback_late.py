"""RX_VK_READBACK_LATE's rules (gpu/vk/readback_late.h; no game or GPU needed), and that vk_replay.cpp's invalidate asks them.

Off unless the switch is set: every readback waits for its write-back, as before. `newest` names the ranges whose writer is
the newest submission handed out, a hex size the ranges of exactly that many bytes; either rule is enough. The by-range
log that finds such ranges has a switch of its own, RX_VK_LOG_READBACK_RANGES, so nothing is noted or logged without it.
The wait itself, with and without a rule, is run in tests/test_readback_part.py.

python -I -B tests/test_readback_late.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include "readback_late.h"
static int failures;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
int main() {
    using namespace readback_late;
    const Rules none = parse(nullptr), empty = parse("");
    check(!none.any() && !empty.any(), "not set, or set to nothing: no rule");
    check(!wanted(none, 0x3980, true) && !wanted(empty, 0x3980, true), "no rule: no readback is read late");
    const Rules newest = parse("newest");
    check(newest.any() && newest.newest && newest.sizes.empty(), "newest alone");
    check(wanted(newest, 64, true) && wanted(newest, 0x3980, true), "newest: a range the newest submission writes, of any size");
    check(!wanted(newest, 64, false) && !wanted(newest, 0x3980, false), "newest: not a range an older submission writes");
    const Rules size = parse("3980");
    check(size.any() && !size.newest && size.sizes.size() == 1 && size.sizes[0] == 0x3980, "a size is read as hex");
    check(wanted(size, 0x3980, false) && wanted(size, 0x3980, true), "a size: the ranges of that size, whoever wrote them");
    check(!wanted(size, 0x3981, true) && !wanted(size, 0x397f, true) && !wanted(size, 3980, true), "a size: no other size");
    const Rules both = parse("newest,3980,c0");
    check(both.newest && both.sizes.size() == 2 && both.sizes[1] == 0xc0, "rules are separated by commas");
    check(wanted(both, 0x40, true) && wanted(both, 0xc0, false) && !wanted(both, 0x40, false), "either rule is enough");
    const Rules zero = parse("0"), junk = parse("off,,x");
    check(!zero.any() && !junk.any(), "0 and words that are no rule name nothing");
    if (failures) return 1;
    std::printf("PASS: RX_VK_READBACK_LATE's rules\n");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    inv = replay[replay.index("void invalidate(uint64_t gpu, uint64_t size) {"):]
    for needle in ('readback_late::parse(e)', 'getenv("RX_VK_READBACK_LATE")', "return readback_late::wanted(rules, size, newest);"):
        if needle not in replay:
            raise SystemExit("vk_replay.cpp no longer has: " + needle)
    # (a range is read late only when its write-back is still pending and the rule names it; the wait is the other branch)
    if "const bool late = pending && readback_late_wanted(size, newest);" not in inv or "if (pending && !late) {" not in inv:
        raise SystemExit("invalidate no longer asks the late rule before its wait")
    # (the by-range notes take a lock at every readback: only with their switch)
    notes = inv[inv.index('static const bool log_ranges = env_on("RX_VK_LOG_READBACK_RANGES");'):]
    if not notes.startswith('static const bool log_ranges = env_on("RX_VK_LOG_READBACK_RANGES");\n            if (log_ranges)\n                rb_range_note('):
        raise SystemExit("the readbacks are noted by range without RX_VK_LOG_READBACK_RANGES")
    if "if (log_ranges) rb_ranges_log();" not in notes[:1200]:
        raise SystemExit("the by-range log is written without RX_VK_LOG_READBACK_RANGES")
    with tempfile.TemporaryDirectory(prefix="readback-late-") as temp:
        src = Path(temp) / "readback_late.cpp"
        exe = Path(temp) / "readback_late.exe"   # (the name llvm-mingw writes on Windows; any name runs on Linux)
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(src), "-o", str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], text=True, capture_output=True, timeout=20)
        print(r.stdout, end="")
        raise SystemExit(r.returncode)


if __name__ == "__main__":
    main()
