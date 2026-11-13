"""A command buffer too short for two segments is recorded on the replay thread, without the parallel replay's dry pass
(gpu/vk/vk_replay.cpp, RX_VK_PARALLEL). Source checks plus a compiled check of the stream count, no game or GPU required.

With RX_VK_PARALLEL set, every command buffer first paid a dry pass - the whole stream applied for state only - and then
its one segment was recorded on the replay thread anyway: the same stream walked twice for nothing. A submission of a
few hundred draws is one segment, and on a title that submits six a frame that was most of them. The work is counted
from the stream's op headers, with the op test the dry pass counts dry_work with; RX_VK_NO_DRY_SHORT=1: the dry pass
comes back.

python -I -B tests/test_parallel_small_cb.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def body(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


def block(text, marker):
    """From marker to the brace closing its first one"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")

    # (the stream's work is counted by the op test the dry pass counts dry_work with)
    work = block(replay, "bool op_is_work(uint32_t op)")
    for op in ("NOP_DRAW", "NOP_DRAW_IDX", "NOP_DRAW_INDIRECT", "NOP_DRAW_MESH", "NOP_DISPATCH", "NOP_DISPATCH_INDIRECT"):
        if op not in work:
            raise SystemExit("vk_replay.cpp: op_is_work no longer counts " + op)
    dry = body(replay, "if (op_is_work(h.op)) {", "p += h.size;")
    if "++dry_work" not in dry:
        raise SystemExit("vk_replay.cpp: the dry pass no longer counts its work with op_is_work")
    # (the short command buffer is decided before the dry pass, and recorded as the serial path records one)
    handle = body(replay, "const uint8_t* replay_handle(", "rmode = Mode::Dry;")
    if "stream_work(h, 2 * per) < 2 * per" not in handle:
        raise SystemExit("vk_replay.cpp: a command buffer's work is not counted before the dry pass")
    if handle.index("stream_work(h, 2 * per)") > handle.index("dry_marks_begin();"):
        raise SystemExit("vk_replay.cpp: the dry pass still runs before the short command buffer is decided")
    short = body(handle, "if (!no_short && stream_work(h, 2 * per)", "// the dry pass")
    if "replay(cmd, state, h);" not in short:
        raise SystemExit("vk_replay.cpp: a short command buffer is not recorded on the replay thread")
    if "split_off = true;" not in short or "split_off = was_split;" not in short:
        raise SystemExit("vk_replay.cpp: a short command buffer's recording is left to be split, or the flag is not restored")
    if 'env_on("RX_VK_NO_DRY_SHORT")' not in replay:
        raise SystemExit("vk_replay.cpp: the switch RX_VK_NO_DRY_SHORT is gone")
    # (a segment is not split by the chunking the serial path does)
    split = body(replay, "void maybe_split(VkCommandBuffer& cmd) {", "end_pass(cmd);")
    if "if (split_off ||" not in split:
        raise SystemExit("vk_replay.cpp: maybe_split no longer leaves a short command buffer's recording whole")

    count = block(replay, "int64_t stream_work(const uint8_t* p, int64_t most)")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
enum {{ NOP_END, NOP_JUMP, NOP_BEGIN, NOP_CALL, NOP_STENCIL, NOP_POLY_OFFSET, NOP_DRAW, NOP_DRAW_IDX, NOP_DISPATCH,
       NOP_BARRIER, NOP_DRAW_INDIRECT, NOP_DRAW_MESH, NOP_DISPATCH_INDIRECT }};
typedef struct {{ uint32_t op, size; }} nvn_op_hdr;
{work}
{count}
static uint8_t s[4096];
static size_t at;
static void op(uint32_t o, uint32_t payload = 0) {{
    const nvn_op_hdr h{{o, 8 + payload}};
    std::memcpy(s + at, &h, sizeof h);
    at += sizeof h + payload;
}}
int main() {{
    op(NOP_DRAW), op(NOP_POLY_OFFSET, 24), op(NOP_DRAW_IDX), op(NOP_DRAW_INDIRECT), op(NOP_DRAW_MESH, 16);
    op(NOP_STENCIL, 8), op(NOP_DISPATCH), op(NOP_DISPATCH_INDIRECT), op(NOP_BARRIER);
    op(NOP_END), op(NOP_DRAW), op(NOP_DRAW);
    CHECK(stream_work(s, 1000) == 6);   // (the ops past the end are not counted)
    CHECK(stream_work(s, 4) == 4);      // (enough: stops there)
    CHECK(stream_work(s, 6) == 6);
    CHECK(stream_work(s, 7) == 6);
    at = 0;
    op(NOP_POLY_OFFSET, 4), op(NOP_END);
    CHECK(stream_work(s, 2) == 0);      // (state alone: no segments)
    std::printf("PASS: a short command buffer is counted without the dry pass\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="parallel-small-cb-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
