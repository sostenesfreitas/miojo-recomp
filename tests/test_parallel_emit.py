"""The lift's emit in several processes gives what one process gives: the same texts in the same order, and the same counters.

python -I -B tests/test_parallel_emit.py
"""
import collections
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import chunks  # noqa: E402


class FakeLifter:
    def __init__(self, n):
        self.funcs = {0x80000000 + 16 * i: None for i in range(n)}
        self.stats = collections.Counter()
        self.unhandled = collections.Counter()
        self.ijump = collections.Counter()
        self.polls = {"functions": 0, "polls": 0}

    def emit_function(self, e, texts):
        self.stats["emitted"] += 1
        if e % 48 == 0:
            self.stats["bad_target"] += 1
            self.unhandled[e % 7] += 1
        if e % 80 == 0:
            self.ijump[e] += 1
            self.polls["polls"] += 2
        self.refs = {"f_%08x" % (e + 16)}
        return "void f_%08x(Ctx *c) { %s }\n" % (e, texts[e % 3]), self.refs


def run(jobs):
    L = FakeLifter(9000)
    texts = {0: "a;", 1: "b;", 2: "c;"}
    out = list(chunks.parallel_emit(L.emit_function, sorted(L.funcs), texts,
                                    [L.stats, L.unhandled, L.ijump, L.polls], jobs, run=500))
    return out, L.stats, L.unhandled, L.ijump, L.polls


def main():
    one = run(1)
    if "fork" not in chunks.multiprocessing.get_all_start_methods():
        print("no fork here: nothing to compare (tests/test_spawn_emit.py runs the new-process emit)")
        return
    many = run(4)
    if [o[:2] for o in one[0]] != [o[:2] for o in many[0]] or [o[2] for o in one[0]] != [o[2] for o in many[0]]:
        raise SystemExit("the parallel emit's texts differ from one process's")
    for name, a, b in zip(("stats", "unhandled", "ijump", "polls"), one[1:], many[1:]):
        if dict(a) != dict(b):
            raise SystemExit("the parallel emit's %s differ: %s vs %s" % (name, dict(a), dict(b)))
    os.environ["HOOKSHOT_LIFT_JOBS"] = "3"
    if chunks.emit_jobs() != 3:
        raise SystemExit("HOOKSHOT_LIFT_JOBS is not followed")
    print("ok: %d functions, the same texts and counters from 1 and 4 processes" % len(one[0]))


if __name__ == "__main__":
    main()
