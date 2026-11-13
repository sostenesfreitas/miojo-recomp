"""Exercise actual write-back observation retirement with authored mapped memory."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include "hookshot_env.h"
// A rejected causal control must exit, not wait for an interactive CRT dialog.
#undef assert
#define assert(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "assertion failed: %s\n", #condition); std::exit(1); \
} } while (0)
using VkBuffer = uintptr_t;
using VmaAllocation = void*;
using VkDeviceAddress = uint64_t;
constexpr uintptr_t VK_NULL_HANDLE = 0;
constexpr int VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 1;
constexpr int VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT = 2;
constexpr int VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO = 0;
struct VkBufferDeviceAddressInfo { int type; void* next = nullptr; VkBuffer buffer = 0; };
struct { int device = 0; } dev;
uint64_t mem_completed = 0, mem_rec_value = 1;
bool flush_ok = true, invalidate_ok = true, allocate_ok = true;
bool mapping_ok = true, address_ok = true;
unsigned invalidations = 0, flushes = 0, live = 0;
unsigned fail_allocations = 0;
void logf(const char*, ...) {}
bool invalidate_mapped_checked(VmaAllocation, uint64_t) { ++invalidations; return invalidate_ok; }
bool flush_mapped_checked(VmaAllocation, uint64_t) { ++flushes; return flush_ok; }
bool create_buffer(uint64_t n, int, bool, VkBuffer& b, VmaAllocation& a, void** p) {
    if (!allocate_ok) return false;
    if (fail_allocations) { --fail_allocations; return false; }
    a = std::calloc(1, size_t(n)); assert(a); *p = mapping_ok ? a : nullptr;
    b = reinterpret_cast<uintptr_t>(a); ++live; return true;
}
void destroy_buffer(VkBuffer, VmaAllocation a) { std::free(a); --live; }
uint64_t vkGetBufferDeviceAddress(int, const VkBufferDeviceAddressInfo* p) { return address_ok ? p->buffer : 0; }
'''

CASES = r'''
int main(int argc, char** argv) {
    if (argc != 2 || wbd_observe_on() != (std::strcmp(argv[1], "1") == 0)) {
        std::fputs("observation switch disagrees with requested mode\n", stderr);
        return 2;
    }
    // Failed staged resources are released, including successful allocation without mapping/BDA.
    WbdTable staged;
    assert(!wbd_table_make(0,staged));
    assert(!wbd_table_make(UINT64_MAX,staged));
    mapping_ok = false; assert(!wbd_table_make(64,staged) && !live && !staged.buf);
    mapping_ok = true; address_ok = false;
    assert(!wbd_table_make(64,staged) && !live && !staged.buf);
    address_ok = true;
    if (wbd_observe_on()) {
        // Preferred optional-size allocation fails; ordinary input allocation still succeeds.
        fail_allocations = 1;
        auto* ordinary = wbd_table_get(128u << 10);
        assert(ordinary && ordinary->bytes == (128u << 10) && live == 1);
        assert(!wbd_observe_begin(*ordinary,(128u << 10)/sizeof(WbdChunk)));
        destroy_buffer(ordinary->buf,ordinary->alloc); *ordinary = {};
        wbd_table_next = 0;
        wbd_observed = {};
    }
    WbdTable* t = wbd_table_get(2 * sizeof(WbdChunk)); assert(t);
    auto* chunks = reinterpret_cast<WbdChunk*>(t->map);
    chunks[0] = {0,0,0,4,1}; chunks[1] = {0,0,0,3,0};
    const auto address = wbd_observe_begin(*t,2);
    if (!wbd_observe_on()) {
        assert(!address && !flushes && !invalidations && !t->observed_count);
    } else {
        assert(address && t->observed_count == 2 && flushes == 1);
        auto* flags = reinterpret_cast<uint32_t*>(address);
        assert(flags[0] == UINT32_MAX && flags[1] == UINT32_MAX);
        flags[0] = 0; flags[1] = 1;
        wbd_observe_collect(*t);
        assert(!invalidations && !wbd_observed.batches && t->observed_count == 2);
        wbd_table_next = 0;
        assert(!wbd_table_get(64) && live == 1);
        mem_completed = 1; mem_rec_value = 2;
        assert(wbd_table_get(64) == t);
        assert(wbd_observed.batches == 1 && wbd_observed.changed == 1);
        assert(wbd_observed.bytes == 76 && wbd_observed.changed_bytes == 12);
        assert(wbd_observed.last_value == 1 && t->value == 2);
        wbd_observe_collect(*t); assert(invalidations == 1);
        // Missing shader stores cannot be mistaken for a clean batch.
        assert(wbd_observe_begin(*t,2)); mem_completed = 2;
        wbd_observe_collect(*t);
        assert(wbd_observed.missing == 2 && wbd_observed.batches == 1);
        wbd_observe_collect(*t); assert(wbd_observed.missing == 2);
        // An invalidate error discards the entire batch, including valid flags.
        assert(wbd_observe_begin(*t,2)); flags[0] = flags[1] = 0;
        invalidate_ok = false; wbd_observe_collect(*t);
        assert(wbd_observed.missing == 4 && wbd_observed.batches == 1);
        invalidate_ok = true;
        // A valid prefix followed by an invalid result must commit no partial totals.
        assert(wbd_observe_begin(*t,2)); flags[0] = 1; flags[1] = 2;
        const auto before = wbd_observed;
        wbd_observe_collect(*t);
        assert(wbd_observed.bytes == before.bytes && wbd_observed.changed == before.changed);
        assert(wbd_observed.missing == before.missing + 2);
        flush_ok = false; assert(!wbd_observe_begin(*t,2));
        assert(!t->observed_count && wbd_observed.missing == 8);
        flush_ok = true;
        assert(!wbd_observe_begin(*t,0));
        assert(!wbd_observe_begin(*t,kWbdObserveChunks+1));
        for (unsigned invalid = 0; invalid != 4; ++invalid) {
            assert(wbd_observe_begin(*t,2)); flags[0] = flags[1] = 0;
            const auto saved_chunk = chunks[1];
            const auto saved_offset = t->observed_offset;
            if (invalid == 0) chunks[1].vecs = 2;
            if (invalid == 1) chunks[1].count = 0;
            if (invalid == 2) chunks[1].count = 65537;
            if (invalid == 3) ++t->observed_offset;
            const auto before_bad = wbd_observed;
            wbd_observe_collect(*t);
            assert(!t->observed_count && wbd_observed.missing == before_bad.missing + 2);
            assert(wbd_observed.bytes == before_bad.bytes && wbd_observed.batches == before_bad.batches);
            chunks[1] = saved_chunk; t->observed_offset = saved_offset;
        }
        const auto saved_address = t->address;
        t->address = UINT64_MAX - sizeof(WbdChunk);
        assert(!wbd_observe_begin(*t,2) && !t->observed_count);
        t->address = saved_address;
        // Failed optional growth preserves an already usable retired input table.
        allocate_ok = false; wbd_table_next = 0;
        const auto old = t->buf; mem_rec_value = 3;
        assert(wbd_table_get(t->bytes) == t && t->buf == old && live == 1);
        assert(!wbd_observe_begin(*t,t->bytes/sizeof(WbdChunk)));
        allocate_ok = true;
        // All ring slots pending: no allocation, invalidation, forced completion or reuse.
        for (auto& slot : wbd_tables) slot.value = mem_completed + 1;
        const auto reads = invalidations;
        for (int i = 0; i < 64; ++i) {
            wbd_table_next = i;
            assert(!wbd_table_get(64));
        }
        assert(invalidations == reads && live == 1 && mem_completed == 2);
    }
    for (auto& slot : wbd_tables) if (slot.buf) destroy_buffer(slot.buf,slot.alloc);
    assert(!live);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--mutations", action="store_true", help="Require causal retirement/visibility regressions to fail")
    args = parser.parse_args()
    flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk/compat")]
    if os.name == "nt":
        flags.append("-D_CRT_SECURE_NO_WARNINGS")  # MSVC's CRT deprecates the standard getenv used by the extracted helper.
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    actual = source[source.index("struct WbdChunk {"):source.index("void memory_end_submission(")]
    with tempfile.TemporaryDirectory(prefix="wb-lifetime-") as tmp:
        cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test.exe"
        cpp.write_text(PRELUDE + actual + CASES, encoding="utf-8")
        subprocess.run([args.cxx, *flags, str(cpp), "-o", str(exe)], check=True, timeout=60)
        for mode, enabled in ((None, False), ("", False), ("0", False), ("false", False), (" Off ", False),
                              ("no", False), ("1", True), ("on", True), ("TRUE", True), ("yes", True)):
            env = dict(os.environ)
            env.pop("RX_VK_WB_OBSERVE", None)
            if mode is not None:
                env["RX_VK_WB_OBSERVE"] = mode
            subprocess.run([str(exe), "1" if enabled else "0"], env=env, check=True, timeout=15)
            print("PASS: production observation lifetime, opt-in=" + repr(mode))
        if args.mutations:
            controls = (
                ("pending completion ignored", "if (!t.observed_count || t.value > mem_completed) return;", "if (!t.observed_count) return;"),
                ("result consumed repeatedly", "t.observed_count = 0; // Consume once", "/* lost one-shot reset */ // Consume once"),
                ("missing GPU flag treated as valid", "flags[i] > 1 ||", "false ||"),
                ("invalidation error ignored", "!invalidate_mapped_checked(t.alloc, offset + count * sizeof(uint32_t))", "false"),
                ("shared switch parser bypassed", 'static const bool on = env_on("RX_VK_WB_OBSERVE");',
                 'static const bool on = [] { const char* s = getenv("RX_VK_WB_OBSERVE"); return s && atoi(s) != 0; }();'),
            )
            for label, before, after in controls:
                assert actual.count(before) == 1, label
                cpp.write_text(PRELUDE + actual.replace(before,after) + CASES, encoding="utf-8")
                subprocess.run([args.cxx, *flags, str(cpp), "-o", str(exe)], check=True, timeout=60)
                result = subprocess.run([str(exe), "1"], env=dict(os.environ,RX_VK_WB_OBSERVE="on"),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
                assert result.returncode != 0, "Undetected mutation: " + label
                print("PASS: causal mutation rejected: " + label)


if __name__ == "__main__":
    main()
