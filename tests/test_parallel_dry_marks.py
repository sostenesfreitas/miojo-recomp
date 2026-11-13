"""The parallel replay's dry pass (gpu/vk/vk_replay.cpp, RX_VK_PARALLEL) marks what the GPU will write before any worker
records: source checks, no game or GPU required.

A worker ahead of the stream uploads a texture it finds "never written"; that upload runs after the earlier segments' work.
So the dry pass has to mark, in stream order, every target a segment writes: a draw's (dry_mark_written), and a clear's,
which opens a pass on the worker just as a draw does. And a worker has to count the uniform updates exactly as the dry
pass appended them.

python -I -B tests/test_parallel_dry_marks.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


def main():
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    # (a clear in the dry pass marks its pass's targets, colour and depth-stencil clears both)
    for op, nxt in (("case NOP_CLEAR_COLOR:", "case NOP_CLEAR_DS:"), ("case NOP_CLEAR_DS:", "case NOP_DRAW:")):
        part = body(replay, op, nxt)
        if "dry && dry_clears_on()) dry_mark_written(s, false, true);" not in part:
            raise SystemExit("vk_replay.cpp: the dry pass no longer marks the targets of " + op)
        if "!dry" not in part:
            raise SystemExit("vk_replay.cpp: " + op + " is no longer left out of the dry pass itself")
    if 'env_on("RX_VK_NO_DRY_CLEARS")' not in replay:
        raise SystemExit("vk_replay.cpp: the switch RX_VK_NO_DRY_CLEARS is gone")
    # (a worker counts a uniform update by the same test the dry pass appends it by)
    part = body(replay, "case NOP_UPDATE_UB:", "case NOP_SET_RT:")
    test = "u.size && u.size <= avail - sizeof u"
    if part.count(test) < 3 or "if (" + test + ") ub_limit++, ub_gen++;" not in part:
        raise SystemExit("vk_replay.cpp: a worker no longer counts a uniform update by the dry pass's own test")
    # (the targets of the draw before are skipped within one dry pass only)
    if "dry_marks_begin();" not in body(replay, "const uint8_t* replay_handle(", "rmode = Mode::Dry;"):
        raise SystemExit("vk_replay.cpp: dry_marks_begin() is no longer called before the dry pass")
    part = body(draw, "void dry_mark_written(NvnState& s, bool compute, bool first) {", "const Targets at = targets(s);")
    if "if (!first && dry_last_valid && !std::memcmp(&last, &s.rt, sizeof last)) return;" not in part:
        raise SystemExit("vk_draw.cpp: dry_mark_written skips by the last targets without asking whether they are this pass's")
    print("parallel dry marks: ok")


if __name__ == "__main__":
    main()
