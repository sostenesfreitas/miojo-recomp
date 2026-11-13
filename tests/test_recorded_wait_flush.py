"""A wait on the recordings' timeline sends the held batch first (gpu/vk/vk_replay.cpp's wait_recorded).

With batched submits (RX_VK_BATCH, on with RADV) a recording's signal on rec_timeline goes to the GPU only when the batch
does. invalidate's readback wait called wait_recorded without a flush: the game job that read the value back waited for
a signal nothing would send, the main thread waited for that job, and its progress polls (the other flush) stopped. The
game hung loading a save. Reads the source: the wait (wait_rec_seq, for wait_recorded and wait_recorded_part) flushes
before vkWaitSemaphores, and invalidate reaches it.
python -I -B tests/test_recorded_wait_flush.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(src, signature):
    """the text of the function that starts with signature, up to its closing brace at column 0"""
    start = src.index(signature)
    return src[start:src.index("\n}\n", start)]


def main():
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    # (the wait itself is wait_rec_seq, which wait_recorded and wait_recorded_part (a submission's part) both end in)
    wait = body(replay, "static void wait_rec_seq(uint64_t seq, uint64_t timeout_ns, int* ok) {")
    if "vkWaitSemaphores(" not in wait:
        raise SystemExit("wait_rec_seq no longer waits on the recordings' timeline: update this test")
    flush = wait.find("queue_flush_for(")
    if flush < 0 or flush > wait.index("vkWaitSemaphores("):
        raise SystemExit("wait_rec_seq waits without sending the held batch: a batched recording's signal never comes")
    for name in ("bool wait_recorded(uint64_t f, uint64_t timeout_ns, int* ok) {", "bool wait_recorded_part("):
        if "wait_rec_seq(seq, timeout_ns, ok);" not in body(replay, name):
            raise SystemExit("%s no longer waits through wait_rec_seq: update this test" % name.split("(")[0][5:])
    if "wait_recorded(v, UINT64_MAX, &ok)" not in body(replay, "void invalidate(uint64_t gpu, uint64_t size) {"):
        raise SystemExit("invalidate's readback wait no longer goes through wait_recorded: update this test")
    print("PASS: wait_recorded sends the held batch before it waits")


if __name__ == "__main__":
    main()
