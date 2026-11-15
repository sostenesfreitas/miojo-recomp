"""The late pages' fix's build (gpu/vk/vk_memory.cpp, "the late pages' fix", late_fix_build): a capture's late page gets a
fix (a LateFix of shaders/late_fix.comp) instead of a wait for the submission still writing it back, or the capture waits
as before (false, counted in st_lf_fell). The section's declarations and helpers, late_fix_build, wb_pages_note (the page
tables) and late_fix_shadows_freed are taken from the source as they are and run over pools, page tables and a capture of
the test's own, with stand-ins for the buffers and the GPU's progress:

  - a diff write-back done by the GPU's progress D needs no fix (the page is copied again from guest memory); one after D
    gives a fix from the pool's shadow through its stamps, after == (uint32)D;
  - D is the newer of the capture's late_done and the GPU's progress now;
  - a page written in place (wb_pages_note's part 0) after D waits; one written in place before D does not;
  - a stamp block filled at recording V (since V + 1) is not read when D is V - 1, and is when D is V;
  - a page whose earlier capture's fix is not recorded yet waits, and is fixed once that one is recorded;
  - a page with a whole copy and a diff both pending waits;
  - a stamp block met by another pool's page is tied to that pool: stamp_pools moves with it (stamps_in);
  - a stamp block whose shadow was freed (late_fix_shadows_freed) is filled again before it is read;
  - a whole copy's page gets a snapshot, then fixes from its pending copies' ranges;
  - late_snap_whole drops the done ranges (at or below mem_completed) once 32 are kept: no unbounded growth;
  - with the 256 snapshot slots taken, snap_page_make gives back the slots of pages not late for kSnapIdle recordings,
    keeping any a recording not done yet writes or reads (used) and any with a late page whose fix is pending.

late_fix_emit is read as text: a full_barrier(up) comes right before the fix's dispatch is bound (no command of the
upload command buffer between them), as it can run several times in one recording, with shadow_sync_copy writing the
shadows it reads in between.

python -I -B tests/test_late_fix_build.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    """the text from marker to the brace that closes its first '{'"""
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def section(text, first, before):
    """the text from first up to (not including) before"""
    start = text.index(first)
    return text[start:text.index(before, start)]


FIXTURE = r'''
#include "gpu/vk/compat/hookshot_env.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #ok); std::exit(1); } } while (0)
constexpr uint64_t kPageShift = 12, kChunkPages = 512, kPages = 1u << 14;   // (64 MB of guest pages: vk_memory.cpp has 512 GB)
using VkBuffer = uintptr_t;
using VkCommandBuffer = uintptr_t;
using VkPipelineLayout = uintptr_t;
using VkPipeline = uintptr_t;
using VkDevice = uintptr_t;
using VkDeviceAddress = uint64_t;
using VkBufferUsageFlags = uint32_t;
struct VmaAllocationT {};
using VmaAllocation = VmaAllocationT*;
constexpr uintptr_t VK_NULL_HANDLE = 0;
constexpr uint32_t VK_BUFFER_USAGE_TRANSFER_DST_BIT = 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 2,
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT = 4;
constexpr int VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO = 0;
struct VkBufferDeviceAddressInfo {
    int sType;
    VkBuffer buffer = 0;
};
struct VkBufferCopy { uint64_t srcOffset, dstOffset, size; };
struct Dev { VkDevice device = 7; } dev;
// (buffers: handles from 0x100 up, each one's device address the handle << 32)
VmaAllocationT one_alloc;
uint64_t made = 0;
bool create_device_buffer(uint64_t, VkBufferUsageFlags, VkBuffer& buffer, VmaAllocation& allocation, bool = false) {
    buffer = (VkBuffer)(0x100 + made++);
    allocation = &one_alloc;
    return true;
}
uint64_t vkGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* info) { return (uint64_t)info->buffer << 32; }
std::vector<std::pair<VkBuffer, VkBufferCopy>> cmd_copies;   // (dst, region)
void vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    for (uint32_t i = 0; i < n; i++) cmd_copies.push_back({dst, r[i]});
}
void logf(const char*, ...) {}
// (vk_memory.cpp's Pool, the fields these functions read)
struct Pool {
    uint64_t lo = 0, hi = 0;
    VkBuffer mirror = VK_NULL_HANDLE;
    VkBuffer shadow = VK_NULL_HANDLE;
    VmaAllocation shadow_alloc = nullptr;
    VkDeviceAddress shadow_address = 0;
    std::vector<uint64_t> shadow_known;
};
std::map<uint64_t, Pool> imported;
uint64_t mem_rec_value = 0, mem_completed = 0;
struct DeadShadow {
    VkBuffer buffer;
    VmaAllocation alloc;
    uint64_t value;
};
std::deque<DeadShadow> dead_shadows;
std::mutex wb_pages_mu;
@WBPAGES@;
WbPages wb_pages, wb_parts, wb_direct;
@WBNOTE@
@SECTION@
@CAPTURE@;
uint64_t completed_now = 0;
uint64_t vk_timeline_completed() { return completed_now; }
struct Host { uint8_t* guest_base() { return nullptr; } } the_host;
Host* host = &the_host;
std::vector<uint64_t> copied_pages;   // (the pages late_fix_build copied again from guest memory)
int flushes = 0;
bool guarded_copy(void*, const void* src, size_t n) {
    CHECK(n == 4096);
    copied_pages.push_back((uint64_t)(uintptr_t)src >> kPageShift);
    return true;
}
void flush_mapped(VmaAllocation, uint64_t) { flushes++; }
bool late_fix_pipe_ready() { return late_fix_pipe.tried = true; }   // (its pipeline made)
@BUILD@
@SHADOWS_FREED@

// ---- the test's pools and captures ----
constexpr uint64_t kLo = 0x1000000;                      // (16 MB: pool A, 1 MB, shadowed and mirrored)
constexpr VkBuffer kShadowA = 0x51, kShadowB = 0x52, kCapBuf = 0x99;
uint8_t cap_mem[4 * 4096];
Pool& add_pool(uint64_t lo, uint64_t size, VkBuffer shadow) {
    Pool p;
    p.lo = lo, p.hi = lo + size, p.mirror = 0x41, p.shadow = shadow, p.shadow_address = shadow ? (uint64_t)shadow << 32 : 0;
    p.shadow_known.assign((size / 4096 + 63) / 64, 0);
    return imported[lo] = p;
}
void reset() {
    imported.clear();
    add_pool(kLo, 1 << 20, kShadowA);
    wb_pages = WbPages{}, wb_parts = WbPages{}, wb_direct = WbPages{}, wb_diff_pages = WbPages{}, wb_whole_pages = WbPages{};
    stamp_blocks.clear(), stamp_pools.clear(), stamp_unfilled.clear(), stamp_bytes = 0;
    late_fixes.clear(), late_pages.clear(), late_built.clear(), late_built_mark = 0;
    snap_pages.clear(), snap_free.clear(), snap_used = 0, late_shadow_pools.clear();
    dead_shadows.clear(), cmd_copies.clear();
    for (int64_t& f : st_lf_fell) f = 0;
    mem_rec_value = mem_completed = 0;
}
Capture cap(std::vector<uint32_t> pages, uint64_t late_done = 0) {   // (every page copied and late)
    Capture c;
    c.pages = c.copied = pages;
    for (uint32_t i = 0; i < pages.size(); i++) c.slots.push_back(i), c.late.push_back(i);
    c.buffer = kCapBuf, c.alloc = &one_alloc, c.map = cap_mem, c.nbytes = pages.size() * 4096;
    c.late_value = 1000, c.late_done = late_done;
    return c;
}
void note(WbKind kind, uint64_t pg, uint64_t value) {   // (a write-back of the page from its mirror, as memory_end_submission notes it)
    wb_kind_note(kind, pg << kPageShift, (pg + 1) << kPageShift, value);
    wb_pages_note(pg << kPageShift, (pg + 1) << kPageShift, value, 1);
}
void in_place(uint64_t pg, uint64_t value) { wb_pages_note(pg << kPageShift, (pg + 1) << kPageShift, value, 0); }   // (note_direct_write)
void filled(uint64_t pg, uint64_t V) {   // (late_fix_emit at recording V: the page's stamp block filled, read from V + 1)
    const auto it = stamp_blocks.find((pg << kPageShift) >> kStampShift);
    CHECK(it != stamp_blocks.end());
    it->second.since = V + 1;
    stamp_unfilled.clear();
}
bool build(Capture& c, uint64_t completed) {
    completed_now = completed;
    copied_pages.clear(), flushes = 0;
    const size_t before = late_fixes.size();
    const bool ok = late_fix_build(c);
    if (ok) {
        CHECK(copied_pages.size() == c.late.size() && flushes == 1);   // (every late page copied again, no wait)
    } else {
        CHECK(copied_pages.empty() && flushes == 0 && late_fixes.size() == before);   // (it waits: nothing of it kept)
    }
    return ok;
}
int fell(int why) { return (int)st_lf_fell[why]; }
const uint64_t pg = (kLo >> kPageShift) + 3;             // (a page of pool A; its stamp block is A's first)
const uint64_t cap_addr = (uint64_t)kCapBuf << 32;

int main() {
    // (a) a diff write-back done by D: no fix; one after D: a fix from the shadow through the stamps, after == D
    reset();
    note(WB_DIFF, pg, 10);
    {
        Capture c = cap({(uint32_t)pg});
        CHECK(!build(c, 9) && fell(3) == 1);              // (no stamps yet: a block made, the capture waits)
        CHECK(stamp_blocks.size() == 1 && stamp_unfilled.size() == 1 && stamps_in(imported[kLo]));
        CHECK(!build(c, 9) && fell(4) == 1);              // (made, not filled yet)
        filled(pg, 5);
        CHECK(build(c, 9) && late_fixes.size() == 1);
        const LateFix& f = late_fixes[0];
        const StampBlock& b = stamp_blocks.begin()->second;
        CHECK(f.dst == cap_addr && f.src == ((uint64_t)kShadowA << 32) + 3 * 4096 && f.stamps == b.address + 3 * 4096);
        CHECK(f.count == 1024 && f.after == 9);
        CHECK(late_pages.count((uint32_t)pg) && late_pages[(uint32_t)pg].fix_value == 0 && late_built.size() == 1);
        CHECK(late_pages[(uint32_t)pg].stamp_buf == b.buf && late_pages[(uint32_t)pg].shadow == kShadowA);
        late_fixes.clear(), late_pages.clear();
        CHECK(build(c, 10) && late_fixes.empty() && late_pages.empty());   // (done by D: the copy again is all it needs)
        CHECK(copied_pages.size() == 1 && copied_pages[0] == pg);
        // (b) D is the newer of the capture's late_done and the GPU's progress now
        note(WB_DIFF, pg, 11);
        Capture c12 = cap({(uint32_t)pg}, 12);
        CHECK(build(c12, 9) && late_fixes.empty());       // (done by the capture's own view)
        Capture c9 = cap({(uint32_t)pg}, 9);
        CHECK(build(c9, 12) && late_fixes.empty());       // (done by now)
        Capture c10 = cap({(uint32_t)pg}, 10);
        CHECK(build(c10, 9) && late_fixes.size() == 1 && late_fixes[0].after == 10);
        late_fixes.clear(), late_pages.clear();
        CHECK(build(c9, 10) && late_fixes.size() == 1 && late_fixes[0].after == 10);
        late_fixes.clear(), late_pages.clear();
        note(WB_DIFF, pg, 0x100000010ull);                // (after: D's low 32 bits, as the stamps are)
        CHECK(build(c9, 0x10000000aull) && late_fixes.size() == 1 && late_fixes[0].after == 0xa);
    }
    // (c) a page written in place by a submission not done by D: which words, unknown, so it waits
    reset();
    note(WB_DIFF, pg, 11);
    {
        Capture c = cap({(uint32_t)pg});
        build(c, 9);
        filled(pg, 5);
        in_place(pg, 11);
        CHECK(wb_direct.get(pg) == 11);
        CHECK(!build(c, 9) && fell(6) == 1);
        in_place(pg, 8), note(WB_DIFF, pg, 11);           // (written in place before D, then diffed after it)
        CHECK(wb_direct.get(pg) == 8 && wb_parts.get(pg) == 1);
        CHECK(build(c, 9) && late_fixes.size() == 1 && late_fixes[0].after == 9);
    }
    // (d) a stamp block filled at recording V (since V + 1): its stamps miss V's write-backs, read only once D >= V
    reset();
    note(WB_DIFF, pg, 25);
    {
        Capture c = cap({(uint32_t)pg});
        build(c, 0);
        filled(pg, 20);
        CHECK(stamp_blocks.begin()->second.since == 21);
        CHECK(!build(c, 19) && fell(4) == 1);
        CHECK(build(c, 20) && late_fixes.size() == 1 && late_fixes[0].after == 20);
        // (e) the page again before that fix is recorded (fix_value 0): one entry a page, so this capture waits
        Capture again = cap({(uint32_t)pg});
        CHECK(!build(again, 20) && fell(2) == 1 && late_fixes.size() == 1);
        CHECK(late_pages.size() == 1 && late_pages[(uint32_t)pg].fix_value == 0);
        late_pages[(uint32_t)pg].fix_value = 30;          // (recorded by late_fix_emit)
        CHECK(build(again, 20) && late_fixes.size() == 2 && late_pages[(uint32_t)pg].fix_value == 0);
    }
    // (f) a whole copy and a diff both pending: no order between their fixes, so it waits; nothing made for it
    reset();
    note(WB_DIFF, pg, 11), note(WB_WHOLE, pg, 11);
    {
        Capture c = cap({(uint32_t)pg});
        CHECK(!build(c, 9) && fell(5) == 1 && stamp_blocks.empty() && snap_pages.empty());
    }
    // (g) a stamp block met by another pool's page: tied to that pool, stamp_pools moved with it
    reset();
    imported.clear();
    add_pool(kLo, 0x8000, kShadowA);
    add_pool(kLo + 0x8000, 0x8000, kShadowB);             // (pool B: the other half of the same 64 KB block)
    {
        const uint64_t pa = kLo >> kPageShift, pb = (kLo + 0x8000) >> kPageShift;
        note(WB_DIFF, pa, 11), note(WB_DIFF, pb, 11);
        Capture ca = cap({(uint32_t)pa}), cb = cap({(uint32_t)pb});
        build(ca, 9);
        filled(pa, 5);
        CHECK(stamp_pools.size() == 1 && stamps_in(imported[kLo]) && !stamps_in(imported[kLo + 0x8000]));
        CHECK(build(ca, 9));
        CHECK(!build(cb, 9) && fell(3) == 2);              // (the second: the block tied to pool B, filled again)
        const StampBlock& b = stamp_blocks.begin()->second;
        CHECK(stamp_blocks.size() == 1 && b.pool == kLo + 0x8000 && b.shadow == kShadowB && b.since == 0);
        CHECK(stamp_unfilled.size() == 1 && stamp_unfilled[0] == stamp_blocks.begin()->first);
        CHECK(!stamps_in(imported[kLo]) && !stamp_pools.count(kLo) && stamps_in(imported[kLo + 0x8000]));
        CHECK(stamp_pools[kLo + 0x8000] == 1);
        filled(pb, 5);
        late_fixes.clear(), late_pages.clear();
        CHECK(build(cb, 9) && late_fixes.size() == 1);
        CHECK(late_fixes[0].src == (uint64_t)kShadowB << 32 && late_fixes[0].stamps == b.address + 0x8000);
    }
    // (h) a stamp block whose shadow was freed: a new shadow may have its handle, so the block is filled again first
    reset();
    note(WB_DIFF, pg, 11);
    {
        Capture c = cap({(uint32_t)pg});
        build(c, 9);
        filled(pg, 5);
        CHECK(build(c, 9));
        late_fixes.clear(), late_pages.clear();
        dead_shadows.push_back({kShadowA, &one_alloc, 3});   // (pool A's shadow given back; its new one has the same handle)
        late_fix_shadows_freed();
        CHECK(stamp_blocks.begin()->second.shadow == VK_NULL_HANDLE && stamp_blocks.begin()->second.since == 0);
        CHECK(!build(c, 9) && fell(3) == 2);   // (the second: filled again for the shadow it has now)
        CHECK(stamp_blocks.begin()->second.shadow == kShadowA && stamp_unfilled.size() == 1 && stamp_pools[kLo] == 1);
    }
    // (i) a whole copy's page: a snapshot made the first time, then fixes from the ranges its pending copies wrote
    reset();
    note(WB_WHOLE, pg, 11);
    {
        Capture c = cap({(uint32_t)pg});
        CHECK(!build(c, 9) && fell(3) == 1 && snap_pages.size() == 1 && late_shadow_pools.count(kLo));
        mem_rec_value = 11;
        late_snap_whole(1, imported[kLo], 3 * 4096 + 64, 128);   // (copy 11: 128 bytes of the page)
        CHECK(cmd_copies.size() == 1 && cmd_copies[0].first == snap_buf && cmd_copies[0].second.dstOffset == 64);
        CHECK(build(c, 9) && late_fixes.size() == 1);
        const LateFix& f = late_fixes[0];
        CHECK(f.dst == cap_addr + 64 && f.src == snap_address + 64 && f.stamps == 0 && f.count == 32);
        late_fixes.clear(), late_pages.clear();
        CHECK(build(c, 11) && late_fixes.empty());        // (that copy done by D)
    }
    // (snapshot ranges) late_snap_whole keeps the pending copies' ranges and drops the done ones once 32 are kept
    reset();
    {
        Capture c = cap({(uint32_t)pg});
        note(WB_WHOLE, pg, 1);
        build(c, 0);
        CHECK(snap_pages.size() == 1);
        SnapPage& s = snap_pages.begin()->second;
        for (mem_rec_value = 1; mem_rec_value <= 40; mem_rec_value++) late_snap_whole(1, imported[kLo], 3 * 4096, 4096);
        CHECK(s.ranges.size() == 40);                     // (none done: every one may still be wanted)
        mem_completed = 40;
        late_snap_whole(1, imported[kLo], 3 * 4096, 4096);   // (recording 41)
        CHECK(s.ranges.size() == 1 && s.ranges[0][2] == 41);   // (40 is done too: at or below mem_completed)
        for (int i = 0; i < 1000; i++, mem_rec_value++) {
            mem_completed = mem_rec_value - 2;
            late_snap_whole(1, imported[kLo], 3 * 4096, 4096);
            CHECK(s.ranges.size() <= 33);
        }
        mem_completed = mem_rec_value;
        for (int i = 0; i < 5; i++) late_snap_whole(1, imported[kLo], 3 * 4096 + 4 * i, 4);
        CHECK(s.ranges.size() <= 33 && s.ranges.back()[0] == 16 && s.ranges.back()[1] == 20);
    }
    // (snapshot slots) all taken: the slots of pages idle for kSnapIdle recordings, nothing on the GPU still writing or reading
    // them and no fix of theirs pending, given back; a new page takes one, with no range of the old page's
    reset();
    {
        mem_rec_value = 10;
        for (uint32_t i = 0; i < kSnapSlots; i++) CHECK(snap_page_make(0x10000 + i));
        CHECK(snap_used == kSnapSlots && snap_free.empty());
        snap_pages[0x10000].used = 2000;                  // (a recording not done yet writes or reads its slot)
        snap_pages[0x10001].late = 1500;                  // (late not long ago)
        late_pages[0x10002] = LatePage{};                 // (a fix not recorded yet)
        late_pages[0x10003].fix_value = 1900;             // (a fix recorded, not done)
        mem_rec_value = 1033, mem_completed = 1000;
        CHECK(!snap_page_make(0x20000) && snap_pages.size() == kSnapSlots);   // (none idle for kSnapIdle yet: it waits)
        mem_rec_value = 2100, mem_completed = 1800;
        CHECK(snap_page_make(0x20000));
        CHECK(snap_pages.size() == 5 && snap_free.size() == kSnapSlots - 5 && snap_used == kSnapSlots);
        for (uint32_t k = 0x10000; k <= 0x10003; k++) CHECK(snap_pages.count(k));
        const uint32_t kept = snap_pages[0x10000].slot, taken = snap_pages[0x20000].slot;
        CHECK(taken < kSnapSlots && snap_pages[0x20000].since == 2101 && snap_pages[0x20000].ranges.empty());
        for (uint32_t s : snap_free) CHECK(s != taken && s != kept);
        mem_rec_value = 2600, mem_completed = 2100;
        for (uint32_t i = 0; i < kSnapSlots - 5; i++) CHECK(snap_page_make(0x30000 + i));
        CHECK(snap_free.empty() && snap_page_make(0x40000));   // (0x10000, 0x10001 and 0x10003 given back now)
        CHECK(snap_free.size() == 2 && snap_pages.count(0x10002) && !snap_pages.count(0x10000));   // (0x10002's fix still pending)
        CHECK(!snap_pages.count(0x10001) && !snap_pages.count(0x10003) && snap_pages.count(0x20000));
    }
    std::puts("PASS: late_fix_build's fixes and waits, its stamp blocks, snapshots, their ranges and slots");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    text = args.source.read_text(encoding='utf-8')
    # (late_fix_emit can run several times in one recording's upload command buffer; between two runs shadow_sync_copy
    # writes the shadows its fixes read: a barrier before the dispatch, after any command recorded before it)
    emit = block(text, 'void late_fix_emit() {')
    bind = emit.index('vkCmdBindPipeline(up, VK_PIPELINE_BIND_POINT_COMPUTE, late_fix_pipe.pipe)')
    barrier = emit.rfind('full_barrier(up', 0, bind)
    if barrier < 0 or barrier < emit.rfind('vkCmd', 0, bind) or barrier < emit.rfind('if (t) {', 0, bind):
        raise SystemExit("late_fix_emit: no full_barrier(up) right before the fix's dispatch is bound: what the upload "
                         "command buffer wrote since its last barrier (shadow_sync_copy's shadows) is not ordered before its reads")
    fixture = (FIXTURE
               .replace('@WBPAGES@', block(text, 'struct WbPages {'))
               .replace('@WBNOTE@', block(text, 'void wb_pages_note(uint64_t guest_lo, uint64_t guest_hi, uint64_t value, uint64_t part) {'))
               .replace('@SECTION@', section(text, "// ---- the late pages' fix (hookshot, 2026-11-13) ----", 'bool late_fix_pipe_ready() {'))
               .replace('@CAPTURE@', block(text, 'struct Capture {'))
               .replace('@BUILD@', block(text, 'bool late_fix_build(Capture& c) {'))
               .replace('@SHADOWS_FREED@', block(text, 'void late_fix_shadows_freed() {')))
    env = {k: v for k, v in os.environ.items()
           if k not in ('RX_VK_LATE_FIX', 'RX_VK_PARALLEL', 'RX_VK_NO_READBACK_WAIT', 'RX_VK_SHADOW_FOLLOWS_SYNC')}
    with tempfile.TemporaryDirectory(prefix='late-fix-build-') as tmp:
        src = Path(tmp) / 'late_fix_build.cpp'
        exe = Path(tmp) / 'late_fix_build.exe'
        src.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(src), '-o', str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True, env=env, timeout=60)


if __name__ == '__main__':
    main()
