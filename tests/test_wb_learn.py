"""Compile and run the learned write-back cap's rules (gpu/vk/wb_learn.h; no game/GPU required), and check that vk_memory.cpp and
vk_replay.cpp still use them: a written binding's write-back is left out only when the table says so, the CPU's reads (invalidate)
and the writes a mirror sync copies feed it, and without RX_VK_WB_MAX_KB every binding is written back as before.

python -I -B tests/test_wb_learn.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def check_source():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    rep = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    need("vk_memory.cpp", mem, ['#include "wb_learn.h"'])
    # (the cap from RX_VK_WB_MAX_KB through the header's rule, 0: none; the table's probation from RX_VK_WB_PROBATION)
    need("vk_memory.cpp: wb_cap()", block(mem, "uint64_t wb_cap()"),
         ['wb_learn::max_bytes(getenv("RX_VK_WB_MAX_KB"))', 'env_int("RX_VK_WB_PROBATION", (int)wb_table.probation)'])
    need("vk_memory.cpp: wb_leave_out()", block(mem, "bool wb_leave_out(uint64_t lo, uint64_t hi"),
         ["if (!wb_cap()) return false;", "std::lock_guard lk(wb_learn_mu);", "wb_table.plan(lo, hi, wb_cap(), mirror_epoch, keep)"])
    # (a CPU read or write counts only in a large binding the table follows)
    access = block(mem, "void memory_note_cpu_access(uint64_t guest, uint64_t size, bool write)")
    need("vk_memory.cpp: memory_note_cpu_access()", access,
         ["if (!wb_table.followed(guest, guest + size)) return;", "wb_table.read(guest, guest + size)"])
    # (a written binding queues its whole write-back, or, planned in part, the ranges the CPU touched; the GPU write hazard is
    # noted either way)
    bind = mem[mem.index("p->gpu_written = true;   // (its mirror kept: mirrors_give_back)"):]
    bind = bind[:bind.index("return remember({p->mirror, off, size, 0});")]
    need("vk_memory.cpp: the written binding", bind,
         ["if (!wb_leave_out(wlo, whi, keep)) writebacks.push_back({p, off, whi - p->lo});",
          "for (const auto& [klo, khi] : keep) writebacks.push_back({p, klo - p->lo, khi - p->lo});",
          "rb_hazard_locked(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo));"])
    if "wb_max" in bind:
        raise SystemExit("vk_memory.cpp: the plain size cap is back beside the learned one")
    # (each dirty page a mirror sync copies is a CPU write the table hears of, before the copy is queued)
    sync = block(mem, "void mirror_sync(Pool& p)")
    note = "if (dirty && note_writes) memory_note_cpu_access(page << kPageShift, 1ull << kPageShift, true);"
    need("vk_memory.cpp: mirror_sync()", sync, ["const bool note_writes = memory_wb_followed(p.lo, p.hi);", note])
    if not sync.index(note) < sync.index("st_sync_live_pages++;"):
        raise SystemExit("vk_memory.cpp: mirror_sync() notes the CPU's writes only after it copies them")
    # (the CPU's reads: every invalidate, before its wait for the write-back)
    inv = block(rep, "void invalidate(uint64_t gpu, uint64_t size)")
    call = "memory_note_cpu_read(host->gpu_guest(gpu), size);"
    need("vk_replay.cpp: invalidate()", inv, [call])
    if not inv.index(call) < inv.index("memory_writeback_value(host->gpu_guest(gpu), size, &ever)"):
        raise SystemExit("vk_replay.cpp: invalidate() tells the table of the read only after its wait")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="wb-learn-") as tmp:
        exe = Path(tmp) / "wb-learn"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_wb_learn.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
