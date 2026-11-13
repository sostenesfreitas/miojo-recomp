"""The memory trims keep what they must.

A recording's stream may move to a bigger buffer in the middle of a recording: what was kept as an offset into "the stream" (the
bank cache, the c1 uploads, the uniform overlay's mapping) has to name its buffer or reserve its room first. The BC3 scratch is made
when an upload needs it: every user asks for it first and stops when it cannot be made. A capture's buffer is sized by class, with
a floor, and the spare ones are counted out as well as in.
python -I -B tests/test_mem_trim.py
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def need(cond, why):
    if not cond:
        raise SystemExit(why)


def main():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    uniform = block(draw, "VkDescriptorBufferInfo uniform_range(")
    need("cur->c1_at[key] = {cur->stream, at};" in uniform, "a c1 upload forgets which stream it is in")
    need("return {it->second.first, it->second.second, n};" in uniform, "a c1 upload is served from the current stream")
    need("c.buf = cur->stream" in uniform and uniform.count("return {c.buf, c.at, n};") == 2,
         "a bank-cache hit names the stream it was kept in")
    overlay = uniform.index("ub_overlay(u.addr")
    need("stream_reserve(n, align)" in uniform[:overlay], "the overlay takes its mapping before its room is reserved")
    nxt = block(draw, "bool stream_next(")
    need("r.streams[r.si].buffer" in nxt and "r.before += r.used" in nxt, "a stream move does not switch the buffer")
    begin = block(draw, "void draw_begin_recording(")
    need("r.si = 0" in begin and "r.used = 0, r.before = 0" in begin, "a recording begins in a stream that is not its first")
    need(begin.index("r.streams.resize(1)") < begin.index("r.si = 0"), "the streams given back are used after")

    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    astc = block(tex, "bool upload_astc_gpu(")
    copy = block(tex, "bool texture_copy_into_bc(")
    need("if (!bc_scratch_layout(cmd, tex.bc_resized))" in astc,   # (its failure also names the cause: test_bc_upload_retry)
         "an ASTC upload does not check scratch allocation with its resize mode")
    need("if (!bc_scratch_layout(cmd, dst->bc_resized)) return false;" in copy,
         "a BC3 copy does not check scratch allocation with its destination resize mode")
    need("bc_have = false" in block(tex, "void bc_scratch_idle("), "the scratch given back is still taken for made")
    need("dead.push_back" in block(tex, "void bc_scratch_idle("), "the scratch is freed with no wait for the GPU")
    need("total_bytes -= std::min(total_bytes, tex.bytes + tex.pad)" in tex, "a texture leaves the budget by its texels only")
    need("x.pad = " in tex and "total_bytes += x.bytes + x.pad" in tex, "a texture enters the budget by its texels only")

    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    need("cap_free_bytes -= size;" in block(mem, "bool cap_buf_get("), "a spare buffer taken is still counted")
    need("cap_free_bytes += size;" in block(mem, "void cap_buf_free("), "a spare buffer kept is not counted")
    size = block(mem, "uint64_t cap_buf_size(")
    need("s < need" in size and "(need + (1u << 20) - 1)" in size, "a capture buffer is smaller than its capture")
    print("ok")


if __name__ == "__main__":
    main()
