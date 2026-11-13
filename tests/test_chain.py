"""scripts/chain.py walks a frame back from its present through waits, signals and virtual-core grants (HOS_CHAIN,
host/hos_chain.c). A made-up recording of three threads, written in the recorder's own format, must come out with the
path's time where it was spent: the presenting thread's run after its wake-up, the main thread's run, the worker's run
before its signal, and a wait for a virtual core inside an event wait kept on the waiting thread (with the thread that
gave the core up).
The recorder's record layout and kinds must match the script's.

python -I -B tests/test_chain.py
"""
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import chain  # noqa: E402


def check_layout():
    hdr = (ROOT / "host" / "hos.h").read_text(encoding="utf-8")
    m = re.search(r"enum \{ HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL, HOS_CHAIN_SLEEP, HOS_CHAIN_AWAKE, "
                  r"HOS_CHAIN_PRESENT \};", hdr)
    assert m, "host/hos.h: the HOS_CHAIN kinds changed: update scripts/chain.py and this test"
    assert (chain.WAIT, chain.WOKE, chain.SIGNAL, chain.SLEEP, chain.AWAKE, chain.PRESENT) == (1, 2, 3, 4, 5, 6)
    src = (ROOT / "host" / "hos_chain.c").read_text(encoding="utf-8")
    assert '"%s"' % chain.REC.format in src, "host/hos_chain.c: the record's layout and scripts/chain.py's differ"
    assert chain.REC.size == 32 and "} ChainRec;" in src
    vc = (ROOT / "host" / "hos_thread.c").read_text(encoding="utf-8")
    assert "return 1ULL << 63 | (uint64_t)(uintptr_t)t;" in vc and chain.VCORE == 1 << 63


def write(path, names, recs):
    with open(path, "wb") as f:
        f.write(b"HOSCHAIN1\n")
        for i, n in enumerate(names):
            f.write(("thread %d %s\n" % (i, n)).encode())
        f.write(("records %d\n" % len(recs)).encode())
        for t, kind, th, key, lr in recs:
            f.write(chain.REC.pack(t, key, lr, th, kind))


def main():
    check_layout()
    P, M, W = 0, 1, 2
    Q, E, VC = 0x100, 0x200, chain.VCORE | 0x7000
    W_, K, S = chain.WAIT, chain.WOKE, chain.SIGNAL
    recs = [
        (0, chain.PRESENT, P, 0, 0),
        (1000, W_, P, Q, 0x10), (5000, S, M, Q, 0x20), (5100, K, P, Q, 0x10),     # present thread waits for main
        (1000, W_, M, E, 0x30), (4000, S, W, E, 0x40),                          # main waits for the worker
        (4060, W_, M, VC, 0x30), (4150, S, W, VC, 0x50), (4200, K, M, VC, 0x30),  # then for a core, granted by it
        (4250, K, M, E, 0x30),
        (2000, chain.SLEEP, W, 500, 0x60), (2500, chain.AWAKE, W, 500, 0x60),     # the worker sleeps 0.5 us
        (9000, chain.PRESENT, P, 1, 0),
    ]
    recs.sort()
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "chain.bin")
        write(path, ["Presentation", "Main", "Worker"], recs)
        names, loaded = chain.load(path)
    assert names == {0: "Presentation", 1: "Main", 2: "Worker"}, names
    blocks, signals, presents = chain.build(loaded)
    acc = chain.Acc()
    chain.walk(P, presents[P][1], presents[P][0], blocks, signals, acc)
    got = {k: v for k, v in acc.by.items()}
    # (the wait for a core is the main thread's own time: contention, not work it waited for; the event's signal before
    # it is what the path follows)
    want = {(P, "running"): 9000 - 5100, (P, "signal to running"): 100, (M, "running"): 5000 - 4200,
            (M, "waiting for a virtual core"): 4200 - 4060, (M, "signal to running"): 4060 - 4000,
            (W, "running"): 4000 - 2500 + 2000, (W, "sleep"): 500}
    if got != want:
        raise SystemExit("FAIL: the path's time\n  got  %s\n  want %s" % (sorted(got.items()), sorted(want.items())))
    assert sum(got.values()) == 9000, "the path does not add up to the frame"
    assert acc.cores == {(M, W): 140}, acc.cores
    want = {(P, "woken at lr 0x10", "the present"): 3900, (M, "woken at lr 0x30", "signal at lr 0x20"): 940,
            (W, "the frame before's present", "signal at lr 0x40"): 4000}
    assert dict(acc.stretches) == want, "the stretches: %s" % sorted(acc.stretches.items())
    assert acc.handn == {(P, 0x10, M, 0x20): 1, (M, 0x30, W, 0x40): 1}, acc.handn
    print("PASS: the frame's path through a queue wait, an event wait with a core wait inside it, and a sleep adds up to "
          "the frame, each part on its thread")


if __name__ == "__main__":
    main()
