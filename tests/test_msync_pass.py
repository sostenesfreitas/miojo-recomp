"""The mirror syncs' pages from the captures by the write-back diff's pass (gpu/vk/vk_memory.cpp, RX_VK_MSYNC_PASS).

A mirror sync copied a capture buffer's pages into the pool's mirror as copy regions; with the switch on (the default) they
go as chunks of the write-back diff's pass (shaders/wb_diff.comp), whose shadow is the destination itself, so the pass is
a copy. Cuts msync_pass_add, wbd_split, WbdChunk and msync_pass_on out of the source as they are, over fake buffer
addresses and fake memory, and checks:
- applying the chunks as the shader does (the source's words to the destination) gives the destination the copy regions
  give it, byte for byte, nothing outside them touched: page runs and partial pages, the source and the destination alike
  or not within 16 bytes, runs past 64 KB;
- every chunk is whole: at most 64 KB, its shadow is its destination, a chunk of vectors 16-byte aligned at all three
  addresses;
- a run not in whole words, a run of no size, a buffer without an address and a pass without its pipeline: nothing
  taken (false), the caller copies them as before;
- the switch: on when unset, off for 0, false, off and no (any case, spaces around), on for any other value.
Then, from the text: every capture buffer is made with cap_buf_usage() (the pass reads them by address), the pass's push
constants are the write-back diff's (one shader, one layout) and the shader's Chunk has WbdChunk's fields in its order.

python -I -B tests/test_msync_pass.py [--cxx g++] [--source path/to/vk_memory.cpp]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
ENV_ON_H = ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h"
SHADER = ROOT / "gpu" / "vk" / "shaders" / "wb_diff.comp"
SWITCH = "RX_VK_MSYNC_PASS"


def block(text, marker):
    """The definition that starts at marker, through its closing brace (a declaration of the same name, ending at its ';'
    before any '{', is passed over)"""
    start = text.find(marker)
    while start >= 0 and text.find(';', start) < text.find('{', start):
        start = text.find(marker, start + 1)
    if start < 0:
        raise SystemExit(f"FAIL: no definition of {marker.strip(' {(')} in the source (the mirror syncs' pass is not there)")
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS   // (getenv, with an MSVC-target {cxx}: as the tests' own .cpp files)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "hookshot_env.h"

typedef uint64_t VkDeviceAddress;
typedef struct VkBuffer_T* VkBuffer;
typedef struct VkDevice_T* VkDevice;
#define VK_NULL_HANDLE nullptr
enum { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO = 1000244001 };
struct VkBufferCopy { uint64_t srcOffset, dstOffset, size; };
struct VkBufferDeviceAddressInfo { int sType; const void* pNext; VkBuffer buffer; };

namespace mkwvk {
static void logf(const char*, ...) {}
struct { VkDevice device; } dev{};
std::map<VkBuffer, VkDeviceAddress> addresses;   // (a buffer's device address; 0 for one without)
VkDeviceAddress vkGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* i) {
    auto it = addresses.find(i->buffer);
    return it == addresses.end() ? 0 : it->second;
}
bool pipe_ready = true;
bool wbd_pipe_ready() { return pipe_ready; }
struct Pool { VkBuffer mirror = VK_NULL_HANDLE; };

@SWITCH_ON@
@WBD_SPLIT@
@WBD_CHUNK@;
std::vector<WbdChunk> msync_chunks;
@MSYNC_COPY@;
std::vector<MsyncCopy> msync_copies;
@PASS_ADD@
}  // namespace mkwvk
using namespace mkwvk;

static int failures;
#define CHECK(ok, ...) do { if (!(ok)) { std::fprintf(stderr, "failed: %s: ", #ok); std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\n"); failures++; } } while (0)

// (the fake memory: a capture buffer and a mirror, each at a device address; a buffer's bytes by its address)
constexpr uint64_t kSrcBytes = 1u << 20, kDstBytes = 1u << 20;
static std::vector<uint8_t> src_mem(kSrcBytes), dst_mem(kDstBytes);
static VkDeviceAddress src_addr, dst_addr;

static void fill(std::vector<uint8_t>& m, uint32_t seed) {
    for (size_t i = 0; i < m.size(); i++) m[i] = (uint8_t)((i * 2654435761u + seed) >> 13);
}

/// The chunks applied as shaders/wb_diff.comp does: the source's words or vectors to the destination (where they differ,
/// which leaves it the same as writing them all), the shadow being the destination
static bool apply(const std::vector<WbdChunk>& chunks, std::vector<uint8_t>& dst, const char* what) {
    bool ok = true;
    for (const WbdChunk& c : chunks) {
        const uint64_t unit = c.vecs ? 16 : 4, bytes = (uint64_t)c.count * unit;
        CHECK(c.vecs <= 1 && c.count > 0 && bytes <= (64u << 10), "%s: a chunk of %u %s", what, c.count, c.vecs ? "vectors" : "words");
        CHECK(c.shadow == c.guest, "%s: a chunk's shadow is not its destination", what);
        if (c.vecs) CHECK(c.mirror % 16 == 0 && c.shadow % 16 == 0 && c.guest % 16 == 0, "%s: a vector chunk not 16-byte aligned", what);
        CHECK(c.mirror % 4 == 0 && c.guest % 4 == 0, "%s: a chunk not in words", what);
        const bool in_src = c.mirror >= src_addr && c.mirror + bytes <= src_addr + kSrcBytes;
        const bool in_dst = c.guest >= dst_addr && c.guest + bytes <= dst_addr + kDstBytes;
        CHECK(in_src && in_dst, "%s: a chunk outside its buffers", what);
        if (!in_src || !in_dst || bytes > (64u << 10)) { ok = false; continue; }
        std::memcpy(dst.data() + (c.guest - dst_addr), src_mem.data() + (c.mirror - src_addr), bytes);
    }
    return ok;
}

static void check_runs(const std::vector<VkBufferCopy>& rs, const char* what) {
    Pool p;
    p.mirror = (VkBuffer)(uintptr_t)0x2000;
    const VkBuffer cap = (VkBuffer)(uintptr_t)0x1000;
    msync_chunks.clear(), msync_copies.clear();
    fill(dst_mem, 7);
    std::vector<uint8_t> want = dst_mem;
    for (const VkBufferCopy& r : rs) std::memcpy(want.data() + r.dstOffset, src_mem.data() + r.srcOffset, r.size);
    const bool taken = msync_pass_add(p, cap, rs);
    CHECK(taken, "%s: not taken", what);
    if (!taken) return;
    CHECK(msync_copies.size() == 1 && msync_copies[0].src == cap && msync_copies[0].dst == p.mirror &&
          msync_copies[0].rs.size() == rs.size(), "%s: its copies not kept for a sync with no table room", what);
    std::vector<uint8_t> got = dst_mem;
    if (!apply(msync_chunks, got, what)) return;
    size_t first = got.size();
    for (size_t i = 0; i < got.size(); i++)
        if (got[i] != want[i]) { first = i; break; }
    CHECK(first == got.size(), "%s: the mirror differs from the copies' at offset %#zx", what, first);
}

static void check_refused(const std::vector<VkBufferCopy>& rs, const char* what) {
    Pool p;
    p.mirror = (VkBuffer)(uintptr_t)0x2000;
    msync_chunks.clear(), msync_copies.clear();
    msync_chunks.push_back({1, 2, 2, 1, 0});   // (an earlier sync's, still to go: left as it is)
    const bool taken = msync_pass_add(p, (VkBuffer)(uintptr_t)0x1000, rs);
    CHECK(!taken && msync_chunks.size() == 1 && msync_copies.empty(), "%s: taken (%zu chunks), not left to the copies", what,
          msync_chunks.size());
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "switch")) {
        std::printf("%d\n", msync_pass_on() ? 1 : 0);
        return 0;
    }
    fill(src_mem, 3);
    for (uint64_t src_base : {0x7f0000000000ull, 0x7f0000000004ull, 0x7f0000000008ull, 0x7f000000000cull}) {
        src_addr = src_base, dst_addr = 0x10000000ull;   // (a mirror's address; the source's alike within 16 bytes or not)
        addresses = {{(VkBuffer)(uintptr_t)0x1000, src_addr}, {(VkBuffer)(uintptr_t)0x2000, dst_addr}};
        char what[96];
        // (a sync's runs: whole pages from capture slots, joined where contiguous, as append_capture_copy leaves them)
        std::snprintf(what, sizeof what, "pages (source at +%llu)", (unsigned long long)(src_base & 15));
        check_runs({{0, 0x3000, 0x1000}, {0x5000, 0x8000, 0x3000}, {0x2000, 0x20000, 0x1000}}, what);
        // (a pool that starts or ends inside a page: the first and last runs are part of a page)
        for (uint64_t lead : {4ull, 8ull, 12ull, 20ull, 0xffcull}) {
            std::snprintf(what, sizeof what, "partial pages (source at +%llu, %llu into the page)", (unsigned long long)(src_base & 15),
                          (unsigned long long)lead);
            check_runs({{lead, 0, 0x1000 - lead}, {0x1000, 0x1000 - lead, 0x1000}, {0x9000, 0x40000, lead}}, what);
        }
        // (long contiguous runs: past 64 KB, several of its chunks; odd words at both ends)
        std::snprintf(what, sizeof what, "long runs (source at +%llu)", (unsigned long long)(src_base & 15));
        check_runs({{4, 0x10004, (64u << 10) + 8}, {0x40000, 0x80000, 3 * (64u << 10)}, {0x90008, 0xf0000, 0x7ff8}}, what);
        for (uint64_t size = 4; size <= 80; size += 4)
            for (uint64_t at = 0; at < 32; at += 4) {
                std::snprintf(what, sizeof what, "%llu bytes at %llu (source at +%llu)", (unsigned long long)size,
                              (unsigned long long)at, (unsigned long long)(src_base & 15));
                check_runs({{0x100 + at, 0x300 + at, size}, {0x700, 0x704 + at, size}}, what);
            }
    }
    src_addr = 0x7f0000000000ull, dst_addr = 0x10000000ull;
    addresses = {{(VkBuffer)(uintptr_t)0x1000, src_addr}, {(VkBuffer)(uintptr_t)0x2000, dst_addr}};
    check_refused({{0, 0, 0x1000}, {0x1000, 0x1002, 0x100}}, "a run not at a whole word");
    check_refused({{0, 0, 0x1000}, {0x1000, 0x1000, 0x102}}, "a run of no whole words");
    check_refused({{0, 0, 0x1000}, {0x1000, 0x1000, 0}}, "a run of no size");
    check_refused({}, "no runs");
    pipe_ready = false;
    check_refused({{0, 0, 0x1000}}, "the pass's pipeline not made");
    pipe_ready = true;
    addresses.erase((VkBuffer)(uintptr_t)0x1000);
    check_refused({{0, 0, 0x1000}}, "a capture buffer without an address");
    if (failures) {
        std::fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    std::puts("ok");
    return 0;
}
'''


def text_checks(text, shader):
    errors = []
    for fn in ('void cap_spare_make(', 'bool cap_buf_get('):
        body = block(text, fn)
        calls = re.findall(r'create_buffer_cached\(\s*[^,]+,\s*([^,]+),', body)
        if not calls:
            errors.append(f"{fn.split('(')[0].split()[-1]}: no create_buffer_cached call found")
        for usage in calls:
            if usage.strip() != 'cap_buf_usage()':
                errors.append(f"{fn.split('(')[0].split()[-1]} makes a capture buffer with {usage.strip()}, not cap_buf_usage(): "
                              "the pass reads capture buffers by address")
    every = re.findall(r'create_buffer_cached\(\s*[^,]+,\s*([^,]+),', text)
    capture_like = [u.strip() for u in every if 'TRANSFER_SRC' in u and 'STORAGE' not in u]
    if capture_like:
        errors.append(f"a buffer made with {capture_like[0]} alone: if it is a capture's, make it with cap_buf_usage()")
    pc = re.compile(r'const struct \{[^}]*\}\s*pc\{')
    flush = block(text, 'void msync_pass_flush(')
    end = block(text, 'void memory_end_submission(')
    ours, theirs = pc.findall(flush), pc.findall(end)
    norm = lambda s: re.sub(r'\s+', ' ', s)
    if len(ours) != 1 or len(theirs) != 1 or norm(ours[0]) != norm(theirs[0]):
        errors.append("msync_pass_flush's push constants are not the write-back diff's (one shader, one layout): "
                      f"{ours} vs {theirs}")
    fields = re.findall(r'^\s*(?:uvec2|uint)\s+(\w+);', block(shader, 'struct Chunk'), re.M)
    chunk = block(text, 'struct WbdChunk')
    ours_fields = re.findall(r'\b(mirror|shadow|guest|count|vecs)\b', chunk)
    if fields != ['mirror', 'shadow', 'guest', 'count', 'vecs'] or ours_fields[:5] != fields:
        errors.append(f"the shader's Chunk fields {fields} are not WbdChunk's {ours_fields[:5]}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    text = args.source.read_text(encoding='utf-8')
    shader = SHADER.read_text(encoding='utf-8')
    errors = text_checks(text, shader)
    fixture = (FIXTURE.replace('@SWITCH_ON@', block(text, 'bool msync_pass_on()'))
               .replace('@WBD_SPLIT@', block(text, 'template <class F> void wbd_split('))
               .replace('@WBD_CHUNK@', block(text, 'struct WbdChunk'))
               .replace('@MSYNC_COPY@', block(text, 'struct MsyncCopy'))
               .replace('@PASS_ADD@', block(text, 'bool msync_pass_add(Pool& p, VkBuffer buf, const std::vector<VkBufferCopy>& rs) {')))
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'msync_pass.cpp'
        exe = Path(tmp) / 'msync_pass.exe'
        src.write_text(fixture, encoding='utf-8')
        # (-Wno-missing-field-initializers: the source fills Vulkan's structs by their sType alone, as Vulkan code does)
        subprocess.run([args.cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                        '-I', str(ENV_ON_H.parent), str(src), '-o', str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode:
            errors.append('the chunks: ' + (run.stderr.strip() or f'exit {run.returncode}'))
        for value, want in ((None, 1), ('0', 0), ('false', 0), (' OFF ', 0), ('No', 0), ('1', 1), ('yes', 1), ('2', 1)):
            env = {k: v for k, v in os.environ.items() if k != SWITCH}
            if value is not None:
                env[SWITCH] = value
            got = subprocess.run([str(exe), 'switch'], capture_output=True, text=True, env=env).stdout.strip()
            if got != str(want):
                errors.append(f"{SWITCH}={value!r}: {'on' if got == '1' else 'off'}, wanted {'on' if want else 'off'}")
    if errors:
        for e in errors:
            print('FAIL:', e)
        raise SystemExit(1)
    print('PASS: the mirror syncs\' pass takes each capture run as the copies did, whole chunks, the switch by the one rule, '
          'capture buffers made for it')


if __name__ == '__main__':
    main()
