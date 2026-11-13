"""A flush with no submit held does not take dev.present_mu (gpu/vk/vk_replay.cpp's queue_flush_for, queue_flush_poll).

A present holds dev.present_mu for as long as it takes (4 ms a frame in Hyrule Field). Held submits exist only with batched
submits (RX_VK_BATCH, on with RADV), but every flush took the mutex to find that out: a readback whose recording was long
done, and the host's copy bookkeeping asking how far the replay is under its own lock (rx_nvn_replayed), waited there for
the present, and the game's other job threads behind them. pend_held says whether anything is held. Reads the source:

  - both flushes test it before they take the mutex;
  - submit_recording raises it when the submit is in the list of held ones, under the mutex, and only then puts the
    recording where waiters find it (value_rec / part_rec): a thread that finds a recording and waits for it flushes
    first, and one found before its submit was in the list had nothing to flush and waited for a signal still held;
    without batched submits the recording is put there before the submit, as it was;
  - only queue_flush_locked lowers it, after it has sent what was held.

python -I -B tests/test_flush_nothing_held.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(src, signature):
    """the text of the function that starts with signature, up to its closing brace at column 0"""
    start = src.index(signature)
    return src[start:src.index("\n}\n", start)]


def main():
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    test = "if (!pend_held.load(std::memory_order_acquire)) return;"
    for name in ("void queue_flush_for(int why) {", "void queue_flush_poll() {"):
        f = body(replay, name)
        if test not in f or f.index(test) > f.index("std::lock_guard lk(dev.present_mu);"):
            raise SystemExit("%s takes dev.present_mu before it knows a submit is held" % name.split("(")[0][5:])
    submit = body(replay, "void submit_recording(")
    up = "pend_held.store(true, std::memory_order_release);"
    if replay.count(up) != 1 or up not in submit:
        raise SystemExit("pend_held is raised somewhere else than where submit_recording holds a submit")
    lock, push, raised = (submit.index(x) for x in ("std::lock_guard lk(dev.present_mu);", "pend.cbs.push_back(cb[i]);", up))
    mapped = submit.index("if (seq) map_recording();", raised)
    if not lock < push < raised < mapped:
        raise SystemExit("a held submit must be in the list, and said to be, under dev.present_mu, before its recording "
                         "can be found and waited for")
    if "if (!batch_on()) map_recording();" not in submit[:lock] or submit.count("map_recording();") != 2:
        raise SystemExit("a recording is put in value_rec / part_rec at another place: a held one before it is in the list?")
    maps = submit[submit.index("const auto map_recording = [&] {"):lock]
    if "value_rec" not in maps or "part_rec.emplace_hint" not in maps or "rec_map_cv.notify_all();" not in maps:
        raise SystemExit("map_recording no longer holds all of the recording's bookkeeping")
    lowered = "pend_held.store(false, std::memory_order_release);"
    if replay.count(lowered) != 1:
        raise SystemExit("pend_held is lowered somewhere else than queue_flush_locked")
    flush = body(replay, "void queue_flush_locked() {")
    if lowered not in flush or flush.index(lowered) < flush.index("pend.cbs.clear()"):
        raise SystemExit("queue_flush_locked lowers pend_held before it has sent what was held")
    print("PASS: a flush with nothing held leaves dev.present_mu alone, and a held submit is known before it can be waited for")


if __name__ == "__main__":
    main()
