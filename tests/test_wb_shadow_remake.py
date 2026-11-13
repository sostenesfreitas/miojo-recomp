"""The write-back shadows given back under VRAM pressure are made again when room returns (gpu/vk/vk_memory.cpp,
shadows_check_room, shadow_make). A pool's write-backs may each stay under the 256 KB a shadow is first made at, for good
after a scene change, and it then stayed whole copies for the rest of the session (issue 2238: 48 of 352 MB never back).
This runs the real functions, taken from the source as they are, over a pool set and a VRAM the test sets: a shadow gone
under pressure comes back on the next room check, only for the pools that had one (shadow_wanted) and only under the
make lines (90% really allocated, 97% of the count), a few per check, a pool whose mirror is gone (mirror_free) or whose
shadow cannot fit now never uses up a try, and a retired pool is never remade.

python -I -B tests/test_wb_shadow_remake.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def check_source(text):
    need("vk_memory.cpp: Pool", text, ["bool shadow_wanted = false;"])
    need("vk_memory.cpp: shadows_check_room()", block(text, "void shadows_check_room() {"),
         ["p.shadow_wanted", "!p.mirror", "shadow_make(p)", "tries++ == 8"])
    at = text.index("run_bytes >= (256u << 10)")
    if "p->shadow_wanted = true;" not in text[at:at + 400]:
        raise SystemExit("vk_memory.cpp: a write-back worth a shadow no longer marks its pool shadow_wanted")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    text = args.source.read_text(encoding='utf-8')
    check_source(text)
    helper = chr(10).join(block(text, marker) for marker in (
        'void shadow_free(Pool& p) {',
        'bool shadow_rules_raw() {',
        'bool shadow_room(uint64_t n) {',
        'bool shadow_make(Pool& p) {',
        'void shadows_check_room() {'))
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <vector>
#include "gpu/vk/vram_pressure.h"
#include "gpu/vk/compat/hookshot_env.h"
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #ok); std::exit(1); } } while (0)
constexpr uint64_t kPageShift = 12;
using VkBuffer = uint64_t;
struct VmaAllocationT {};
using VmaAllocation = VmaAllocationT*;
using VkDevice = uint64_t;
using VkBufferUsageFlags = uint32_t;
constexpr VkBuffer VK_NULL_HANDLE = 0;
constexpr uint32_t VK_BUFFER_USAGE_TRANSFER_DST_BIT = 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 2,
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT = 4;
constexpr int VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO = 0;
constexpr int VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1;
struct VkBufferDeviceAddressInfo {
    int sType;
    VkBuffer buffer = 0;
};
// (vk_memory.cpp's Pool, the fields these functions read; shadow_wanted is the source's own, checked above)
struct Pool {
    uint64_t lo = 0, hi = 0;
    uint64_t last_value = 0;
    VkBuffer mirror = 0;
    VkBuffer shadow = 0;
    VmaAllocation shadow_alloc = nullptr;
    uint64_t shadow_address = 0;
    std::vector<uint64_t> shadow_known;
    bool shadow_wanted = false;
};
struct DeadShadow {
    VkBuffer buffer;
    VmaAllocation alloc;
    uint64_t value;
};
std::deque<DeadShadow> dead_shadows;
uint64_t shadow_bytes = 0, mem_rec_value = 0, mem_completed = 0;
std::map<uint64_t, Pool> imported;
struct Retired {
    Pool p;
    Pool& mapped() { return p; }
};
std::deque<Retired> retired_pools;
struct VkPhysicalDeviceProperties {
    int deviceType = 0;
};
struct Dev {
    VkDevice device = 7;
    VkPhysicalDeviceProperties props;
} dev;
// (vram_usage as vk_vma.cpp reports it, set by the test)
bool vram_known = true;
uint64_t vram_used = 0, vram_allowed = 0, vram_block_free = 0;
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free = nullptr) {
    if (!vram_known) return false;
    *used = vram_used, *budget = vram_allowed;
    if (block_free) *block_free = vram_block_free;
    return true;
}
int makes = 0;   // (shadow_make's allocations)
bool create_device_buffer(uint64_t, VkBufferUsageFlags, VkBuffer& buffer, VmaAllocation& allocation, bool = false) {
    static VmaAllocationT one;
    buffer = (VkBuffer)++makes;
    allocation = &one;
    return true;
}
uint64_t vkGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* info) { return info->buffer; }
void logf(const char*, ...) {}
namespace mkwvk {   // (the functions as vk_memory.cpp has them: there they are in mkwvk, which finds vram_pressure)
''' + helper + r'''
}   // namespace mkwvk
using mkwvk::shadow_make;
using mkwvk::shadows_check_room;
const uint64_t MB = 1ull << 20;
void vram(uint64_t used, uint64_t allowed) { vram_used = used, vram_allowed = allowed, vram_block_free = 0; }
void vram3(uint64_t used, uint64_t allowed, uint64_t block_free) {
    vram_used = used, vram_allowed = allowed, vram_block_free = block_free;
}
void tick() {                                                                 // (120 write-back submissions: one check)
    for (int i = 0; i < 120; i++) shadows_check_room();
}
void reset() {
    imported.clear(), retired_pools.clear(), dead_shadows.clear();
    shadow_bytes = 0, makes = 0, vram_known = true;
}
Pool& pool(uint64_t lo, uint64_t mb, bool wanted) {                          // (a live pool, mirrored)
    Pool p;
    p.lo = lo, p.hi = lo + mb * MB, p.mirror = 1, p.shadow_wanted = wanted;
    return imported.emplace(lo, p).first->second;
}
int main() {
    // (given back once under pressure: made again when room returns, though no write-back of it reached 256 KB since)
    reset();
    vram(9600 * MB, 10000 * MB);                                              // (96% really allocated: the shadows go)
    Pool& a = pool(0x10000000, 16, true);
    Pool& b = pool(0x20000000, 8, false);
    tick();
    CHECK(!a.shadow && !b.shadow && makes == 0);                              // (no shadow to take, and none is worth one)
    vram(6000 * MB, 10000 * MB);                                              // (room again)
    tick();
    CHECK(a.shadow && shadow_bytes == 16 * MB && makes == 1);
    CHECK(!b.shadow);                                                         // (a pool whose write-backs were small stays unshadowed)
    tick();
    CHECK(makes == 1 && shadow_bytes == 16 * MB);                             // (made once, not every check)
    // (a shadow given back under pressure comes back the same way: the exact sequence of issue 2238's log)
    reset();
    vram(6000 * MB, 10000 * MB);
    Pool& c = pool(0x10000000, 16, true);
    CHECK(shadow_make(c));
    CHECK(c.shadow && shadow_bytes == 16 * MB);
    vram(9600 * MB, 10000 * MB);
    tick();
    CHECK(!c.shadow && shadow_bytes == 0 && dead_shadows.size() == 1);        // (the shadows given back)
    CHECK(c.shadow_wanted);                                                   // (it stays wanted: a small write-back is enough)
    vram(6000 * MB, 10000 * MB);
    tick();
    CHECK(c.shadow && shadow_bytes == 16 * MB && makes == 2);
    // (a few per check: each is an allocation of its pool's whole size)
    reset();
    vram(6000 * MB, 10000 * MB);
    for (int i = 0; i < 10; i++) pool(0x10000000 + (uint64_t)i * 0x1000000, 4, true);
    tick();
    CHECK(makes == 8 && shadow_bytes == 32 * MB);
    tick();
    CHECK(makes == 10 && shadow_bytes == 40 * MB);
    // (a retired pool's shadow is given back with the others, and never made again: only the table is scanned)
    reset();
    vram(6000 * MB, 10000 * MB);
    retired_pools.push_back(Retired{});
    Pool& r = retired_pools.back().p;
    r.lo = 0x30000000, r.hi = r.lo + 8 * MB, r.mirror = 1, r.shadow_wanted = true;
    CHECK(shadow_make(r));
    vram(9600 * MB, 10000 * MB);
    tick();
    CHECK(!r.shadow && shadow_bytes == 0);
    vram(6000 * MB, 10000 * MB);
    tick();
    CHECK(!r.shadow && makes == 1);
    // (a pool mirror_free'd keeps shadow_wanted but cannot be made again: skipped, it must not use up the tries that
    //  the pools after it in the table need)
    reset();
    vram(6000 * MB, 10000 * MB);
    for (int i = 0; i < 9; i++)
        pool(0x10000000 + (uint64_t)i * 0x1000000, 4, true).mirror = 0;       // (the flag outlives the mirror)
    Pool& h = pool(0x20000000, 16, true);
    tick();
    CHECK(makes == 1 && h.shadow && shadow_bytes == 16 * MB);                  // (the ninth try is not spent on a mirror-less pool)
    // (a pool too big to fit now is skipped without spending a try: the smaller pools after it in the table are made)
    reset();
    vram(8900 * MB, 10000 * MB);                                              // (100 MB to the make line: a 200 MB shadow does not fit)
    for (int i = 0; i < 9; i++) pool(0x10000000 + (uint64_t)i * 0x1000000, 200, true);
    Pool& small = pool(0x20000000, 16, true);
    tick();
    CHECK(small.shadow && shadow_bytes == 16 * MB && makes == 1);
    // (the make lines: nothing above 90% really allocated or 97% of the driver's count, made below them)
    reset();
    vram(9100 * MB, 10000 * MB);                                              // (91% really allocated: above the line)
    Pool& m = pool(0x10000000, 16, true);
    tick();
    CHECK(!m.shadow && makes == 0);
    vram(8900 * MB, 10000 * MB);                                              // (89%: under it)
    tick();
    CHECK(m.shadow && shadow_bytes == 16 * MB && makes == 1);
    reset();
    vram3(9800 * MB, 10000 * MB, 1000 * MB);                                  // (88% really allocated, 98% of the count)
    Pool& k = pool(0x10000000, 16, true);
    tick();
    CHECK(!k.shadow && makes == 0);                                           // (above the 97% count line)
    vram3(9680 * MB, 10000 * MB, 1000 * MB);                                  // (86.8% really allocated, 96.96% of the count)
    tick();
    CHECK(k.shadow && shadow_bytes == 16 * MB && makes == 1);
    // (VRAM unknown: nothing is made or given back)
    reset();
    vram(6000 * MB, 10000 * MB);
    Pool& d = pool(0x10000000, 16, true);
    vram_known = false;
    tick();
    CHECK(!d.shadow && makes == 0);
    std::puts("PASS: the shadows given back under VRAM pressure are made again when room returns");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'remake.cpp'
        exe = Path(tmp) / 'remake.exe'
        src.write_text(fixture, encoding='utf-8')
        # (-D_CRT_SECURE_NO_WARNINGS: with MSVC's headers getenv is deprecated, an error under -Werror; as the other fixtures)
        subprocess.run([args.cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-D_CRT_SECURE_NO_WARNINGS',
                        '-I', str(ROOT), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
