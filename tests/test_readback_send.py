"""With batched submits, what is held goes to the GPU while a CPU read of GPU results waits for it.

Without the idle send (the Steam Deck's APU, batch_gpu_idle) the work the replay records while a readback catches up (#2125)
was held until the wait's own flush, so the GPU started it cold and the reader waited for all of it. invalidate() marks the
wait (readback_waiting) and sends what is held at its start; submit_recording sends again each time RX_VK_READBACK_SEND_CBS
command buffers are held. RX_VK_READBACK_SEND=0 turns it off, and the log says which.

python -I -B tests/test_readback_send.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPLAY = ROOT / "gpu" / "vk" / "vk_replay.cpp"


def main():
    s = REPLAY.read_text(encoding="utf-8")
    inval = s[s.index("void invalidate(uint64_t gpu, uint64_t size) {"):]
    inval = inval[:inval.index("\n}\n")]
    for needle, where, why in (
        ("if (on) readback_waiting++, queue_flush_for(FR_READBACK);", inval, "invalidate() does not send what is held when it waits"),
        ("~Waiting() { if (on) readback_waiting--; }", inval, "the wait's mark is not cleared when it ends"),
        ("waiting(readback_send_on() && memory_writeback_value(host->gpu_guest(gpu), size, nullptr) > completed());", inval,
         "the mark is not limited to a read whose write-back is still ahead of the GPU"),
        ("if (!pend.cbs.empty() && readback_send_now()) flush_reason = FR_READBACK, queue_flush_locked();", s,
         "submit_recording does not send while a readback waits"),
        ("return readback_waiting.load(std::memory_order_relaxed) && pend.cbs.size() >= cbs;", s,
         "the send is not by the command buffers held"),
        ('const char* e = getenv("RX_VK_READBACK_SEND");', s, "RX_VK_READBACK_SEND does not choose"),
        ('logf("vk: held submits go out while a readback waits: %s (RX_VK_READBACK_SEND)"', s, "the choice is not logged"),
    ):
        if needle not in where:
            raise SystemExit(f"vk_replay.cpp: {why}")
    print("held submits go out while a readback waits ok")


if __name__ == "__main__":
    main()
