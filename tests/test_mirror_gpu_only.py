"""A pool the GPU wrote keeps its VRAM mirror (gpu/vk/vk_memory.cpp mirrors_give_back).

An idle mirror is given back after kIdleEpochs submissions (at once past kLongIdle, or when a wanted one does not fit), and
made again from guest memory at the pool's next binding. What a shader wrote through a bare entry, the pointer table or a
wide binding is never copied back (memory_note_gpu_write): it was in the old mirror alone, and the GPU went on from stale
data. Paused 1-2 minutes in the background, the idle pools' mirrors all went, and the game came back with cyan for a few
seconds (RX 5500M; gone with RX_VK_MIRROR_KEEP=1). Reads the source: memory_note_gpu_write marks the pools it reaches, the
idle give-back leaves those, and a pool that takes others in keeps their mark.
python -I -B tests/test_mirror_gpu_only.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(src, signature):
    """the text of the function that starts with signature, up to its closing brace at column 0"""
    start = src.index(signature)
    return src[start:src.index("\n}\n", start)]


def main():
    mem = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    if "bool gpu_only = false;" not in mem[mem.index("struct Pool {"):mem.index("};", mem.index("struct Pool {"))]:
        raise SystemExit("Pool has no gpu_only mark: nothing tells which mirrors hold what guest memory never gets")
    if ".gpu_only = true;" not in body(mem, "void memory_note_gpu_write(uint64_t gpu, uint64_t size) {"):
        raise SystemExit("memory_note_gpu_write does not mark the pools it reaches: their mirrors go with the GPU's data")
    give = body(mem, "void mirrors_give_back(uint64_t want_bytes) {")
    idle = give[give.index("std::vector<Pool*> idle;"):give.index("std::sort(idle.begin()")]
    if "if (p.gpu_only || p.gpu_written)" not in idle or "else idle.push_back(&p);" not in idle:
        raise SystemExit("mirrors_give_back gives back a gpu_only pool's idle mirror: made again from guest memory, the GPU's "
                         "data in it is lost (cyan for seconds after a pause in the background)")
    bind = body(mem, "BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {")
    if "p->gpu_written = true;" not in bind[bind.rindex("if (written) {"):]:
        raise SystemExit("gpu_buffer_bind does not mark a pool the GPU writes through its mirror: after a save reload its mirror "
                         "goes and comes back without what the GPU made of it (cyan in play and over the Start menu)")
    lookup = body(mem, "Pool* pool_lookup(uint64_t guest, uint64_t size) {")
    took_in = lookup[lookup.index("for (uint64_t k : overlapped) {   // (the pools it took in)"):]
    merged = took_in[:took_in.index("imported.emplace(p.lo, p)")]
    if "p.gpu_only |= o->second.gpu_only" not in merged or "p.gpu_written |= o->second.gpu_written" not in merged:
        raise SystemExit("a pool that takes others in does not keep their gpu_only and gpu_written marks")
    print("PASS: mirrors the GPU wrote past the write-back are kept")


if __name__ == "__main__":
    main()
