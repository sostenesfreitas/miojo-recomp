"""The write-back's shadow follows the mirror's sync (gpu/vk/vk_memory.cpp, "The shadow follows the mirror's sync"): a mirror
sync of a pool with a shadow copies the same bytes into the shadow and notes each page (shadow_sync_copy); after the next
write-back's forget those pages are known again (shadow_synced_known), but only while the pool's shadow is the one that
had the copy; the notes of a shadow being freed are dropped first (shadow_synced_freed), as a shadow made after may get
its handle. The functions are taken from the source as they are and run over pools of the test's own, with
vkCmdCopyBuffer recorded. With RX_VK_SHADOW_FOLLOWS_SYNC=0 nothing is copied or noted (the fixture is run again so).

python -I -B tests/test_wb_shadow_follows_sync.py [--cxx g++]
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


def through(text, first, last_block):
    """the text from first to the end of the block that starts at last_block"""
    start = text.index(first)
    end = text.index(last_block, start)
    return text[start:end] + block(text[end:], last_block)


FIXTURE = r'''
#include "gpu/vk/compat/hookshot_env.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #ok); std::exit(1); } } while (0)
constexpr uint64_t kPageShift = 12;
using VkBuffer = uintptr_t;
using VkCommandBuffer = uintptr_t;
struct VmaAllocationT {};
using VmaAllocation = VmaAllocationT*;
constexpr uintptr_t VK_NULL_HANDLE = 0;
struct VkBufferCopy { uint64_t srcOffset, dstOffset, size; };
struct Copy { VkBuffer src, dst; std::vector<VkBufferCopy> rs; };
std::vector<Copy> cmd_copies;
void vkCmdCopyBuffer(VkCommandBuffer, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    cmd_copies.push_back({src, dst, std::vector<VkBufferCopy>(r, r + n)});
}
// (vk_memory.cpp's Pool, the fields these functions read)
struct Pool {
    uint64_t lo = 0, hi = 0;
    VkBuffer shadow = VK_NULL_HANDLE;
    std::vector<uint64_t> shadow_known;
};
std::map<uint64_t, Pool> imported;
struct DeadShadow {
    VkBuffer buffer;
    VmaAllocation alloc;
    uint64_t value;
};
std::deque<DeadShadow> dead_shadows;
@FOLLOWS@
@FREED@
constexpr uint64_t kA = 0x10000000, kB = 0x20000000;   // (two pools of 128 pages)
constexpr VkBuffer kShadowA = 0x51, kShadowB = 0x52, kNewShadow = 0x53, kStage = 0x99;
Pool& pool(uint64_t lo, VkBuffer shadow) {
    Pool p;
    p.lo = lo, p.hi = lo + 128 * 4096, p.shadow = shadow, p.shadow_known.assign(2, 0);
    return imported[lo] = p;
}
void reset() {
    imported.clear(), dead_shadows.clear(), cmd_copies.clear(), shadow_synced.clear();
    pool(kA, kShadowA), pool(kB, kShadowB);
}
bool known(uint64_t lo, uint64_t page) { return imported[lo].shadow_known[page >> 6] >> (page & 63) & 1; }
int count_known(uint64_t lo) {
    int n = 0;
    for (uint64_t pg = 0; pg < 128; pg++) n += known(lo, pg);
    return n;
}
int main(int argc, char** argv) {
    const std::vector<VkBufferCopy> rs = {{0, 0x1000, 0x2000}, {0x3000, 0x5ffc, 8}, {0x4000, 0x7000, 0}};   // (pages 1-2, 5-6, none)
    if (argc > 1 && !std::strcmp(argv[1], "off")) {   // (RX_VK_SHADOW_FOLLOWS_SYNC=0: as before, nothing copied or noted)
        reset();
        shadow_sync_copy(1, kStage, imported[kA], rs);
        CHECK(cmd_copies.empty() && shadow_synced.empty());
        shadow_synced_known();
        CHECK(count_known(kA) == 0);
        std::puts("PASS: with RX_VK_SHADOW_FOLLOWS_SYNC=0 a mirror sync leaves the shadow alone");
        return 0;
    }
    // the sync's regions copied into the shadow too, and each page they touch noted with the pool and that shadow
    reset();
    shadow_sync_copy(1, kStage, imported[kA], rs);
    CHECK(cmd_copies.size() == 1 && cmd_copies[0].src == kStage && cmd_copies[0].dst == kShadowA && cmd_copies[0].rs.size() == 3);
    CHECK(cmd_copies[0].rs[1].dstOffset == 0x5ffc && cmd_copies[0].rs[1].size == 8);
    CHECK(shadow_synced.size() == 4);
    const uint64_t want[4] = {1, 2, 5, 6};
    for (int i = 0; i < 4; i++)
        CHECK(shadow_synced[i].lo == kA && shadow_synced[i].page == want[i] && shadow_synced[i].shadow == kShadowA);
    // (after the write-back's forget) those pages known, and the notes gone
    shadow_synced_known();
    CHECK(shadow_synced.empty() && count_known(kA) == 4 && known(kA, 1) && known(kA, 2) && known(kA, 5) && known(kA, 6));
    CHECK(count_known(kB) == 0);
    // a pool with no shadow, or nothing to copy: nothing
    reset();
    imported[kA].shadow = VK_NULL_HANDLE;
    shadow_sync_copy(1, kStage, imported[kA], rs);
    shadow_sync_copy(1, kStage, imported[kB], {});
    CHECK(cmd_copies.empty() && shadow_synced.empty());
    // the pool's shadow made again since the sync: the new one never had the copy, so its pages stay unknown
    reset();
    shadow_sync_copy(1, kStage, imported[kA], rs);
    shadow_sync_copy(1, kStage, imported[kB], {{0, 0, 4096}});
    imported[kA].shadow = kNewShadow;
    shadow_synced_known();
    CHECK(shadow_synced.empty() && count_known(kA) == 0 && count_known(kB) == 1 && known(kB, 0));
    // ... or the pool gone: nothing marked, nothing breaks
    reset();
    shadow_sync_copy(1, kStage, imported[kA], rs);
    imported.erase(kA);
    shadow_synced_known();
    CHECK(shadow_synced.empty());
    // a shadow being freed: its notes dropped before it goes, so a shadow made with its handle is not marked known
    reset();
    shadow_sync_copy(1, kStage, imported[kA], rs);
    shadow_sync_copy(1, kStage, imported[kB], {{0, 0x2000, 4096}});
    shadow_synced_freed();
    CHECK(shadow_synced.size() == 5);   // (no shadow freed: all kept)
    dead_shadows.push_back({kShadowA, nullptr, 7});
    shadow_synced_freed();
    CHECK(shadow_synced.size() == 1 && shadow_synced[0].lo == kB && shadow_synced[0].shadow == kShadowB);
    imported[kA].shadow = kShadowA;   // (the new shadow, given the freed one's handle)
    shadow_synced_known();
    CHECK(count_known(kA) == 0 && count_known(kB) == 1 && known(kB, 2));
    std::puts("PASS: the shadow follows the mirror's sync, for the shadow that had the copy only");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    text = args.source.read_text(encoding='utf-8')
    fixture = (FIXTURE
               .replace('@FOLLOWS@', through(text, 'bool shadow_follows_sync_on() {', 'void shadow_synced_known() {'))
               .replace('@FREED@', block(text, 'void shadow_synced_freed() {')))
    env = {k: v for k, v in os.environ.items() if k != 'RX_VK_SHADOW_FOLLOWS_SYNC'}
    with tempfile.TemporaryDirectory(prefix='shadow-follows-sync-') as tmp:
        src = Path(tmp) / 'follows.cpp'
        exe = Path(tmp) / 'follows.exe'
        src.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(src), '-o', str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True, env=env, timeout=60)
        subprocess.run([str(exe), 'off'], check=True, env=dict(env, RX_VK_SHADOW_FOLLOWS_SYNC='0'), timeout=60)


if __name__ == '__main__':
    main()
