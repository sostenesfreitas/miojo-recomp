"""Sparse VRAM mirrors (RX_VK_MIRROR_SPARSE; gpu/vk/sparse_mirror.h, no game/GPU required): the header's block rules
compiled and run, and checks that vk_memory.cpp uses them where a block with no memory could be reached: the page table
points such blocks at guest memory, a binding that reaches one where a game pool is is served from guest memory and asks
for it (one whose blocks with none are all outside every game pool is served from the mirror), copies from or into a
mirror leave such blocks out, a write-back never reads one, and a freed mirror's blocks go with it. On by default
(RX_VK_MIRROR_SPARSE=0: off), and off where the device lacks strict sparse residency for buffers.

python -I -B tests/test_sparse_mirror.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(text, needles, where):
    for n in needles:
        if n not in text:
            raise SystemExit(f"vk_memory.cpp: {where} no longer has: {n}")


def check_source():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    # (on by default: unset is on, =0 / off / false / no off; never where the device can't)
    need(block(mem, "bool mirror_sparse_on()"), ['getenv("RX_VK_MIRROR_SPARSE")', "const bool asked = !v || !env_value_off(v);",
                                                 "residencyNonResidentStrict", "sparseResidencyBuffer", "return asked && can;"],
         "mirror_sparse_on()")
    make = block(mem, "bool mirror_make(Pool& pool) {")
    need(make, ["sparse_mirror::live_blocks(p->lo, p->hi", "sparse_fill(*p, sparse_buf, sparse_req, live)",
                "sparse_table(*p, ma, 0, p->has.size())", "sparse_mirror::clip(", "sparse_mirror::cut_src(out, pending)", "cap_pages_over(*p, p->lo, p->hi, over)",
                "mirror_bytes += mirror_size(*p);"], "mirror_make()")
    # (the whole copy from guest memory: only the blocks with memory when the mirror is sparse)
    if make.index("if (p->sparse) {   // (the blocks with memory)") > make.index("vkCmdCopyBuffer(upload_command_buffer(), p->buffer, p->mirror, 1, &all)"):
        raise SystemExit("vk_memory.cpp: mirror_make() copies the whole pool before it looks whether the mirror is sparse")
    bind = block(mem, "BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {")
    fast = bind[:bind.index("ParLock plk;")]
    need(fast, ["sparse_has(*p, off, size)", "sparse_touch(*p, off, size);"], "gpu_buffer_bind()'s path without the lock")
    slow = bind[bind.index("ParLock plk;"):]
    at = slow.index("if (p->sparse && !sparse_has(*p, off, size)) {")
    if not at < slow.index("mirror_sync(*p);") < slow.index("return remember({p->mirror, off, size, 0});"):
        raise SystemExit("vk_memory.cpp: gpu_buffer_bind() can hand out a sparse mirror's block with no memory")
    # (a binding whose blocks with no memory are all outside every game pool: served from the mirror, nothing wanted; the
    # others from guest memory, below)
    past = slow.index("if (p->sparse && !sparse_has(*p, off, size) && sparse_gaps_outside_pools(*p, off, size)) {")
    if not past < at < slow.index("mirror_sync(*p);"):
        raise SystemExit("vk_memory.cpp: gpu_buffer_bind() no longer serves a binding past every game pool from the mirror first")
    if "sparse_want(" in slow[past:at]:
        raise SystemExit("vk_memory.cpp: gpu_buffer_bind() wants blocks no game pool is in")
    need(block(mem, "bool sparse_gaps_outside_pools("), ["sparse_mirror::live_blocks(lo, hi, b, guest_pools.begin(), guest_pools.end(), live)",
                                                       "if (!block_has(p, i) && i - a < live.size() && live[i - a]) return false;"],
         "sparse_gaps_outside_pools()")
    need(slow[at:slow.index("mirror_sync(*p);")], ["sparse_want(*p, off, size);", "note_direct_write(*p, off, size)",
                                                  "p->refill.push_back(",
                                                  "shadow_forget(", "return remember({p->buffer, off, size, 0});"],
         "gpu_buffer_bind()'s block with no memory")
    grow = block(mem, "void sparse_grow_all(const std::vector<Pool*>& pools) {")
    need(grow, ["sparse_bind_infos(infos)", "full_barrier(up,", "vkCmdCopyBuffer(up, p.buffer, p.mirror", "cap_pages_over(p,",
                "sparse_table(p, ma, i, i + 1)", "shadow_forget(p, c.dstOffset, c.size)", "p.released[i] = nullptr;", "++p.release_gen[i];",
                "mirror_bytes + taking + b > mirror_budget()", "p.refill.clear();", "late_fix_emit();",
                "vkCmdCopyBuffer(up, buf, p.mirror"], "sparse_grow_all()")
    if not grow.index("vkCmdCopyBuffer(up, p.buffer, p.mirror") < grow.index("sparse_table(p, ma, i, i + 1)"):
        raise SystemExit("vk_memory.cpp: sparse_grow_all() points the page table at blocks before it fills them")
    # (as mirror_sync and mirror_make: the late pages' fix recorded before anything copies out of a capture's buffer)
    if not grow.index("cap_pages_over(p,") < grow.index("late_fix_emit(") < grow.index("vkCmdCopyBuffer(up, buf, p.mirror"):
        raise SystemExit("vk_memory.cpp: sparse_grow_all() copies out of a capture's buffer before the late pages' fix")
    rel = block(mem, "void sparse_release() {")
    need(rel, ["guest_pages_set(a, e, p.address + (a - p.lo))", "dead_blocks.push_back(",
               "std::max(mem_rec_value, p.last_value)", "|| p.wanted[i]",
               "if (mirror_epoch - p.block_dead_since[i] < kSparseIdleEpochs) continue;", "mirror_epoch - last_bound_of(p, i) < kSparseIdleEpochs",
               "p.released[i] = p.blocks[i];"], "sparse_release()")
    need(block(mem, "void sparse_reap() {"), ["dead_blocks.front().value <= mem_completed", "sparse_live.count(d.buffer)",
                                              "d.pool->release_gen[d.index] != d.gen", "sparse_bind_infos(infos, &unbound);", "free_sparse_block("], "sparse_reap()")
    need(block(mem, "void mirror_free(Pool& p) {"), ["dead_sparse.back().blocks.push_back(a)", "std::erase(sparse_wanted, &p);", "if (d.pool == &p) d.pool = nullptr;",
                                                     "mirror_bytes -= std::min(mirror_bytes, size);"], "mirror_free()")
    need(block(mem, "void sparse_reap() {"), ["sparse_live.erase(dead_sparse.front().buffer)", "destroy_sparse_buffer(",
                                              "dead_sparse.front().value <= mem_completed"], "sparse_reap()'s freed mirrors")
    need(block(mem, "void memory_end_submission(VkCommandBuffer cmd, uint64_t part) {"),
         ["if (p->sparse) {   // (hookshot: never from a sparse mirror's block with no memory",
          "if (!sparse_gaps_outside_pools(*p, c.src, c.size)) {"], "memory_end_submission()")
    need(block(mem, "void retire_pool(std::map<uint64_t, Pool>::iterator it) {"), ["std::erase(sparse_wanted, old)"], "retire_pool()")
    begin = block(mem, "void memory_begin_submission() {")
    need(begin, ["if (mirror_sparse_on()) {", "sparse_release();", "sparse_want_live();", "sparse_grow_all(grow);", "sparse_reap();"],
         "memory_begin_submission()")
    need(block(mem, "void sparse_want_live() {"), ["sparse_mirror::live_blocks(", "if (live[i] && !p.blocks[i])"], "sparse_want_live()")
    over = block(mem, "void cap_pages_over(Pool& p, uint64_t lo, uint64_t hi, std::map<VkBuffer, std::vector<VkBufferCopy>>& over) {")
    need(over, ["if (p.sparse) {", "block_has(p, (size_t)blk)"], "cap_pages_over()")
    # (the binds are not waited for on the CPU: every submission of recordings waits for them on the GPU, and unbound memory is
    # freed once the binds' timeline has passed it)
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if replay.count("if (memory_sparse_wait(&sparse_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &sparse_wait;") != 3:
        raise SystemExit("vk_replay.cpp: a submission of recordings (the flush, a submit, a parallel segment) does not wait for the sparse binds")
    binds = block(mem, "bool sparse_bind_infos(")
    need(binds, ["si.pSignalSemaphores = &sparse_tl;", "vkQueueBindSparse(dev.queue, 1, &si, VK_NULL_HANDLE)",
                 'env_on("RX_VK_SPARSE_WAIT")'], "sparse_bind_infos()")
    # (the binds' semaphore is made under dev.present_mu, which memory_sparse_wait reads it under)
    if not binds.index("std::lock_guard qk(dev.present_mu);") < binds.index("vkCreateSemaphore(") < binds.index("vkQueueBindSparse("):
        raise SystemExit("vk_memory.cpp: sparse_bind_infos() makes the binds' semaphore outside dev.present_mu")
    need(block(mem, "void sparse_reap() {"), ["sparse_frees.front().first <= done", "dead_sparse.front().binds <= done"], "sparse_reap()'s frees")
    vma = (ROOT / "gpu/vk/vk_vma.cpp").read_text(encoding="utf-8")
    need(block(vma, "bool create_sparse_buffer("), ["VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT",
                                                    "vkGetBufferMemoryRequirements"], "vk_vma.cpp create_sparse_buffer()")
    need(block(vma, "bool alloc_sparse_block("), ["r.size = req.alignment;", "VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT",
                                                  "VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT"], "vk_vma.cpp alloc_sparse_block()")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="sparse-mirror-") as tmp:
        exe = Path(tmp) / "sparse-mirror"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_sparse_mirror.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
