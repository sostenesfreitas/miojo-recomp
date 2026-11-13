"""A mesh fallback program whose compute pipeline the driver refused or faulted on is not compiled again (#1001).

fallback_compute_pipelines (gpu/vk/vk_draw.cpp) made the task and mesh stages' compute pipelines on a program's first
fallback draw and kept them; one that failed kept nothing, so every later draw of that program compiled it again. With a
driver that faults on it, each draw was one more fault caught by guarded_compute: 12,160 for one program in a Kokiri
Forest run on RADV, the frame down to 2-3 fps, and on a GTX 1070 the replay stopped presenting after the first one. A
failure is now remembered by the program's key and variant, and the program's draws are left out without a new try.

python -I -B tests/test_mesh_fallback_failed_once.py
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def main():
    draw = DRAW.read_text(encoding="utf-8")
    body = block(draw, "bool fallback_compute_pipelines(Program& p) {")
    if "static std::set<std::pair<uint64_t, uint64_t>> failed;" not in body:
        raise SystemExit("fallback_compute_pipelines keeps no record of the programs that failed")
    lock = body.index("std::lock_guard lk(mu);")
    check = body.find("if (failed.count({p.key, p.variant})) return false;")
    create = body.index("cached_compute(")   # (the compile: guarded_compute, through the play caches)
    if check < 0 or not lock < check < create:
        raise SystemExit("a program that failed is compiled again: the check must come under the lock, before the compile")
    failure = block(body, "&made[i]); r != VK_SUCCESS) {")
    if "failed.insert({p.key, p.variant});" not in failure:
        raise SystemExit("a failed compute pipeline is not remembered")
    if failure.index("failed.insert") > failure.index("return false;"):
        raise SystemExit("the failure is remembered after the return")
    print("a mesh fallback program whose compute pipeline failed is not compiled again: ok")


if __name__ == "__main__":
    main()
