"""Compile and run the import trim's rules (gpu/vk/mirror_trim.h; no game/GPU required), and check that vk_memory.cpp still uses
them: a mirrored import mostly outside every game pool for long enough is retired and its live parts imported again with a
mirror wanted at once, each taking its part of the old mirror; nothing is trimmed unless the new mirrors fit the budget beside
the old one and the mirrors already wanted; the old mirror waits to be inherited until every live part has its own (a part
refused its mirror keeps it: trims_settle, compiled and run here); RX_VK_MIRROR_TRIM=0 (off, false, no) turns it off.

python -I -B tests/test_mirror_trim.py [--cxx g++]
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


def order(where, text, needles):
    at = -1
    for needle in needles:
        nxt = text.find(needle, at + 1)
        if nxt < 0:
            raise SystemExit(where + " no longer has, in this order: " + needle)
        at = nxt


def check_source():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    need("vk_memory.cpp", mem, ['#include "mirror_trim.h"'])
    trim = block(mem, "bool mirrors_trim()")
    need("vk_memory.cpp: mirrors_trim()", trim,
         ['getenv("RX_VK_MIRROR_TRIM")', "return !v || !env_value_off(v);", "mirror_trim::pressed(used, allowed)",
          "mirror_trim::live(p.lo, p.hi, chunk, mirror_trim::kJoin, guest_pools)", "mirror_trim::worth(p.hi - p.lo, parts)",
          "mirror_trim::counted(p.trims, mirror_epoch - p.trim_epoch)", "mirror_trim::wait(trims)",
          "mirror_trim::counted(pick->trims, mirror_epoch - pick->trim_epoch) + 1",
          # (only an import with a mirror, and never inside a parallel replay's segments)
          "p.mstate != Pool::MIRROR_ON", "par_active.load(std::memory_order_relaxed)",
          # (room for the new mirrors beside the old one, which stays until the GPU is done with it)
          "for (const Pool* w : mirror_wanted) wanted += w->hi - w->lo;",
          "if (mirror_bytes + wanted + kept > mirror_budget() || (tight_rule && used > allowed / 100 * 90)) return false;"])
    if "atoi(" in trim:
        raise SystemExit("vk_memory.cpp: mirrors_trim() reads RX_VK_MIRROR_TRIM with atoi again: on/true/yes would turn it off")
    # (every live part is imported before anything is retired: a failed import leaves the old one as it was; then the old one is
    # retired, marked, and each part gets its flags, its page table entries and a mirror wanted now)
    order("vk_memory.cpp: mirrors_trim()", trim,
          ["if (!import_pool(r.lo, r.hi, np)) {", "retire_pool(imported.find(olo));", "retired_pools.back().mapped().trimmed = true;", "trims_waiting++;",
           "m.gpu_only = gpu_only, m.gpu_written = gpu_written, m.trims = trims, m.trim_epoch = mirror_epoch;",
           "guest_pages_set(it->second.lo, it->second.hi, it->second.address);", "it->second.mstate = Pool::MIRROR_WANTED;",
           "mirror_wanted.push_back(&it->second);"])
    # (the parts' mirrors are made in the same submission's uploads, before anything is recorded, and only then is the old mirror
    # let go, by trims_settle, when every part has its own)
    begin = block(mem, "void memory_begin_submission()")
    order("vk_memory.cpp: memory_begin_submission()", begin,
          ["mirrors_trim();", "if (mirror_wanted.empty()) {", "trims_settle();", "return;",
           "for (Pool* p : mirror_wanted) mirror_make(*p);", "trims_settle();", "mirror_wanted.clear();"])
    if "inherit_pending = false" in begin:
        raise SystemExit("vk_memory.cpp: memory_begin_submission() lets a trimmed mirror go itself again, not through trims_settle")
    # (a trimmed pool's mirror is inherited by each of its parts: the first does not end it)
    need("vk_memory.cpp: mirror_make()", block(mem, "bool mirror_make(Pool& pool) {"), ["if (!r.trimmed) r.inherit_pending = false;"])
    # (an import made again over trimmed ones remembers it, so the wait grows)
    need("vk_memory.cpp: pool_lookup()", block(mem, "Pool* pool_lookup(uint64_t guest, uint64_t size) {"),
         ["p.trims = std::max(p.trims, o->second.trims)", "p.trim_epoch = std::max(p.trim_epoch, o->second.trim_epoch)"])


SETTLE_TEST = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <map>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Pool {
    uint64_t lo = 0, hi = 0;
    enum : uint8_t { MIRROR_NONE, MIRROR_WANTED, MIRROR_ON, MIRROR_REFUSED } mstate = MIRROR_NONE;
    bool inherit_pending = false, trimmed = false;
};
std::map<uint64_t, Pool> imported;
std::deque<std::map<uint64_t, Pool>::node_type> retired_pools;
static void logf(const char*, ...) {}
/*TRIMS_WAITING*/
/*SETTLE*/
const uint64_t MB = 1ull << 20, LO = 0x7f00000000, HI = LO + 256 * MB;
Pool& add(uint64_t lo, uint64_t hi, int mstate) {
    Pool p;
    p.lo = lo, p.hi = hi, p.mstate = (decltype(p.mstate))mstate;
    return imported.emplace(lo, p).first->second;
}
Pool& old_one() { return retired_pools.front().mapped(); }
void trim() {   // (the old import retired, as mirrors_trim leaves it, with its mirror wanted by each part)
    imported.clear(), retired_pools.clear(), trims_waiting = 0;
    Pool& o = add(LO, HI, Pool::MIRROR_ON);
    o.inherit_pending = true;
    retired_pools.push_back(imported.extract(LO));
    old_one().trimmed = true;
    trims_waiting++;
    add(LO - 64 * MB, LO - 32 * MB, Pool::MIRROR_NONE);   // (a pool outside its range: not counted)
    add(HI + 4 * MB, HI + 8 * MB, Pool::MIRROR_NONE);
}
int main() {
    {   // every part has its mirror: the old one waits for no more
        trim();
        add(LO, LO + 20 * MB, Pool::MIRROR_ON), add(LO + 100 * MB, LO + 112 * MB, Pool::MIRROR_ON);
        trims_settle();
        CHECK(!old_one().inherit_pending && !old_one().trimmed && trims_waiting == 0);
    }
    for (int refused : {(int)Pool::MIRROR_NONE, (int)Pool::MIRROR_WANTED, (int)Pool::MIRROR_REFUSED}) {
        // a part refused its mirror (the budget, RX_VK_MIRROR_TIGHT, or a failed allocation): the old mirror is kept for it
        trim();
        add(LO, LO + 20 * MB, Pool::MIRROR_ON);
        Pool& part = add(LO + 100 * MB, LO + 112 * MB, refused);
        trims_settle();
        CHECK(old_one().inherit_pending && old_one().trimmed && trims_waiting == 1);
        // (a failed allocation is tried again at the part's next binding, not refused for good)
        CHECK(part.mstate == (refused == (int)Pool::MIRROR_REFUSED ? (int)Pool::MIRROR_NONE : refused));
        trims_settle();
        CHECK(old_one().inherit_pending && trims_waiting == 1);
        // its mirror made later (with the old one's contents, mirror_make): now the old one goes
        part.mstate = Pool::MIRROR_ON;
        trims_settle();
        CHECK(!old_one().inherit_pending && !old_one().trimmed && trims_waiting == 0);
        trims_settle();   // (nothing left waiting: nothing done)
        CHECK(trims_waiting == 0);
    }
    {   // a pool reaching into the range from below counts too
        trim();
        add(LO - 8 * MB, LO + 20 * MB, Pool::MIRROR_NONE);
        trims_settle();
        CHECK(old_one().inherit_pending && trims_waiting == 1);
    }
    {   // the wait timed out (mirrors_give_back's kIdleEpochs: inherit_lost), or the mirror went: no longer counted
        trim();
        add(LO, LO + 20 * MB, Pool::MIRROR_NONE);
        old_one().inherit_pending = false;
        trims_settle();
        CHECK(!old_one().trimmed && trims_waiting == 0);
    }
    std::printf("trims_settle: ok\n");
    return 0;
}
"""


def settle_test(cxx, tmp):
    """trims_settle cut from vk_memory.cpp and run: a live part refused its mirror keeps the old one waiting to be inherited"""
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    src = SETTLE_TEST.replace("/*TRIMS_WAITING*/", "int trims_waiting = 0;").replace("/*SETTLE*/", block(mem, "void trims_settle() {"))
    need("vk_memory.cpp", mem, ["int trims_waiting = 0;"])
    cpp = Path(tmp) / "trims_settle.cpp"
    cpp.write_text(src, encoding="utf-8")
    exe = Path(tmp) / "trims-settle"
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="mirror-trim-") as tmp:
        exe = Path(tmp) / "mirror-trim"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_mirror_trim.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
        settle_test(args.cxx, tmp)


if __name__ == "__main__":
    main()
