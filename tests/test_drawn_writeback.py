"""Compile and run the rule for writing GPU-drawn textures back into guest memory (gpu/vk/drawn_writeback.h; no game/GPU)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The texture cache still notes a texture when its GPU contents begin, asks the rule each frame, finishes a batch only
    once the GPU is past its copies and the texture is as it was, writes by compare and write with the undo, and reads the
    switch"""
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "drawn_writeback.h"',
                   "if (!tex->gpu_written || tex->released) tex->first_written = frame_no, drawn_writeback_note(tex);",
                   "drawn_writeback::noted(tex->gpu, tex->guest_size, tex->levels)",
                   "drawn_writeback::due(frame_no, t->first_written, t->last_written, t->written_back_for,",
                   "t->gpu_written, t->only_cleared, other[i]);",
                   "if (t && o != t && o->gpu < t->gpu + t->guest_size && o->gpu + o->guest_size > t->gpu)",
                   "while (!drawn_batches.empty() && drawn_batches.front().value <= completed_value) {",
                   "ts.push_back(drawn_alive(batch.fs[i].clip_lo, t) && t->id == batch.was[i].id ? t : nullptr);",
                   "if (!ts[i] || !f.t->gpu_written || f.t->only_cleared || f.t->last_written != batch.was[i].last_written ||",
                   "(other[i] && other[i] >= f.t->last_written)) {",
                   "VkCommandBuffer cmd = upload_cmd();",
                   "if (!flush_record(f, cmd)) continue;",
                   "f.t->last_value = std::max(f.t->last_value, rec_value);",
                   "f.only_untouched = true;",
                   "!drawn_writeback::may_write(was.data(), was.size(), t.written_back_hash)",
                   "all &= drawn_writeback::swap_words(p, from + at, to + at, (size_t)k, went ? went + at : nullptr, only ? only + at : nullptr) == 0;",
                   "same &= guest_swap(p.gpu, was.data() + (p.gpu - t.gpu), staged.data() + p.at, p.n, went.data() + p.at);",
                   "guest_swap(p.gpu, staged.data() + p.at, was.data() + (p.gpu - t.gpu), p.n, nullptr, went.data() + p.at);",
                   "f.written_hash = drawn_writeback::hash(again.data(), again.size());",
                   "f.t->written_back_hash = f.written_hash;",
                   "    frame_no++;\n    drawn_writeback_frame();",
                   "f.t->written_back_for = f.t->last_written;",
                   'env_on("RX_VK_NO_DRAWN_WRITEBACK")'):
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="drawn-writeback-") as tmp:
        exe = Path(tmp) / "drawn-writeback"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_drawn_writeback.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
