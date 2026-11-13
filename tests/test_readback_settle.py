"""A CPU readback of GPU results lets the met stopped work on first, with per-queue fences too (gpu/vk/vk_replay.cpp's invalidate).

A range's write-back value is known only once the work that writes it is recorded. With per-queue fences (the default) the
readback skipped the catch-up, written when a catch-up forced stopped work past its waits; d6c5ddcc made it let on only
the met waits, but the readback kept skipping it. Work stopped at a wait that was met by then (a CPU event, signalled,
not applied yet) was not recorded, its write-back unknown, and the CPU read the range before it, waiting for nothing.
Reads the source: the readback settles (never forcing) before it looks the value up.
python -I -B tests/test_readback_settle.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(src, signature):
    """the text of the function that starts with signature, up to its closing brace at column 0"""
    start = src.index(signature)
    return src[start:src.index("\n}\n", start)]


def main():
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    inv = body(replay, "void invalidate(uint64_t gpu, uint64_t size) {")
    lookup = "const uint64_t v = memory_writeback_value(host->gpu_guest(gpu), size, nullptr, &part, &whole);"
    if lookup not in inv:
        raise SystemExit("invalidate no longer looks up the range's write-back value: update this test")
    before = inv[:inv.index(lookup)]
    if "!no_catchup && !unforced" in before:
        raise SystemExit("the readback skips its catch-up with per-queue fences: stopped work that writes the range is "
                         "read before it is recorded")
    # (sync_settle_met: sync_settle when the CPU has signalled an event since the queues were last looked at, which is the
    # only way a queue is stopped at a met wait with nobody to let it on; tests/test_readback_part.py runs it)
    settle = before.rfind("else if (sync_settle_met())")
    if settle < 0 or "if (behind0 && !no_catchup) {" not in before[:settle]:
        raise SystemExit("the readback's catch-up before the write-back lookup is gone, or no longer settles with "
                         "per-queue fences (sync_settle: the met waits only, never forced)")
    met = body(replay, "bool sync_settle_met() {")
    if "sync_settle();" not in met:
        raise SystemExit("sync_settle_met no longer settles")
    print("PASS: a readback settles the met stopped work before it looks up the write-back")


if __name__ == "__main__":
    main()
