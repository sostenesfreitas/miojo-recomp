"""Dispatch timing is off unless RX_VK_SLOW_DISPATCH is set.

Every compute dispatch read the clock five times for a log of at most 60 slow
dispatches. python -I -B tests/test_dispatch_clock.py
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    if "bool slow_dispatch_timing()" not in source or "RX_VK_SLOW_DISPATCH" not in source:
        raise SystemExit("dispatch timing has no off switch")
    clock = block(source, "struct DispatchClock {")
    fn = block(source, "void dispatch(")
    bare = []
    seen_guard = False
    for line in clock.splitlines():
        if "if (!slow_dispatch_timing()) return;" in line:
            seen_guard = True
        if "steady_clock::now()" in line and "slow_dispatch_timing()" not in line and not seen_guard:
            bare.append(line.strip())
    if not seen_guard:
        bare.append("DispatchClock reads the clock in its destructor with the log off")
    stamps = [line.strip() for line in fn.splitlines() if "dclock.t_" in line and "now()" in line]
    if len(stamps) != 3:
        raise SystemExit(f"expected the program, pipeline, and descriptor stamps, found {len(stamps)}")
    for line in stamps:
        if "slow_dispatch_timing()" not in line:
            bare.append(line)
    if bare:
        raise SystemExit("unguarded dispatch clock:\n" + "\n".join(bare))
    print("PASS: the dispatch clock stays behind RX_VK_SLOW_DISPATCH")


if __name__ == "__main__":
    main()
