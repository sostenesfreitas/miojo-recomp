"""A texture the GPU writes in a virtual pool keeps its texels when the game finalizes it (gpu/vk/vk_texture.cpp).

A texture in a virtual pool (a GPU address at or above 1 TB) is checked page by page through its mapping: texture_dirty
keeps each 64 KB page's guest address, and a later check takes the pages remapped since as written. texture_touch notes
that map before a texture's first upload. One the GPU writes first is marked uploaded by texture_written with no upload,
so it had no map, and its first check after the game finalized it took all of its storage: the whole image was uploaded
from guest memory over what the GPU drew (Ocarina of Time's HUD button labels blank after a zone change).

First, that every reason texture_written is given in gpu/vk is one NOTED_BY lists, given by one writer, in the file
NOTED_BY names (see there). Then cuts texture_written, texture_released, texture_dirty and texture_touch's checks and
first-upload seed out of the source as they are, over a fake GPU mapping and fake page generations, and checks, then again
with RX_VK_NO_DRAWN_PAGE_MAP=1 (as before: all of the storage at the first check of a virtual-pool texture the GPU wrote
first):
- a texture drawn in a virtual pool and finalized, nothing written since: not uploaded again, also after other pages
  were remapped, when it is drawn and finalized a second time, and when a writer that gives no reason wrote it before
  the pass;
- what the CPU wrote after the finalize, a page remapped since, and what the CPU wrote over a texture the GPU still owns
  (RX_VK_CPU_OVER_GPU=1): those alone are taken for written (a finalized texture's upload stays whole);
- a writer that gives no reason (the upscaler's stand-ins, a texture clear), a texture below 1 TB, one already noted and
  one of no size: no page walk, as before. A texture clear's texture has its map from its first upload (resolve() samples
  it before the clear), so what the CPU writes after the clear is taken alone, as before.
Of texture_touch only those lines run: not its epoch test, gpu_dirty (a GPU buffer's write over the storage),
RX_VK_REUPLOAD_DIM, upload_seg (a parallel replay's later segment) or the guard of its live check.

python -I -B tests/test_drawn_page_map.py [--cxx g++] [--source path/to/vk_texture.cpp]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
ENV_ON_H = ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h"
SWITCH = "RX_VK_NO_DRAWN_PAGE_MAP"

# The reasons texture_written is given, each by one writer: the file it is in, and where that writer's lock is. With a
# reason, texture_written notes the page map of a virtual-pool texture that has none, and texture_dirty changes the map
# (page_map, runs, map_sig) there: each of these writers was checked by hand to hold the ParLock of a parallel replay
# (RX_VK_PARALLEL), or to be its dry pass, which runs on the replay thread before the workers. Another writer given one of
# these reasons is one more to check: it fails the check until it has a reason of its own here. The writers that give no
# reason ("?") note none: the upscaler's stand-ins (vk_draw.cpp upscale_stub, temporal_rejected_network), which run without
# that lock, and the texture clear (vk_xfer.cpp clear_tex), which needs none: resolve() samples its texture first
# (texture_get(t, true)).
NOTED_BY = {
    "a storage image of a draw": ("gpu/vk/vk_draw.cpp", "build_descriptors: a ParLock on the same line"),
    "a pass's colour target": ("gpu/vk/vk_draw.cpp", "begin_pass: ParLock(marks), left out only when every target is "
                               "marked this frame (texture_marked: a pass drew it this frame, so its map is noted)"),
    "a pass's depth target": ("gpu/vk/vk_draw.cpp", "begin_pass: as the colour targets"),
    "the dry pass (a draw's target)": ("gpu/vk/vk_draw.cpp", "dry_mark_written: the dry pass"),
    "the dry pass (a draw's depth)": ("gpu/vk/vk_draw.cpp", "dry_mark_written: the dry pass"),
    "the dry pass (a dispatch's storage image)": ("gpu/vk/vk_draw.cpp", "dry_mark_written: the dry pass"),
    "a buffer-to-texture copy": ("gpu/vk/vk_xfer.cpp", "copy_b2t: the ParLock of NOP_COPY_B2T and NOP_COPY_T2B "
                                 "(vk_replay.cpp)"),
    "a copy into a BC texture": ("gpu/vk/vk_xfer.cpp", "copy_t2t: the ParLock of NOP_COPY_T2T (vk_replay.cpp)"),
    "a texture-to-texture copy": ("gpu/vk/vk_xfer.cpp", "copy_t2t: the ParLock of NOP_COPY_T2T (vk_replay.cpp)"),
}

FIXTURE = r'''#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
// (texture_dirty's Texture::Run& last = runs.empty() ? runs.emplace_back(Run{...}) : runs.back(): GCC 13's
// -Wdangling-reference can take it for a reference to the temporary Run. It is the vector's element)
#pragma GCC diagnostic ignored "-Wdangling-reference"
#endif
namespace mkwvk {
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
struct Texture {   // (the fields the code cut out uses, as gpu/vk/vk_texture.h has them)
    uint32_t width = 64, height = 64, layers = 1;
    bool sparse = false;
    uint64_t gpu = 0, guest = 0, guest_size = 0;
    int nvn_format = 0x25;
    bool uploaded = false;
    uint32_t upload_gen = 0;
    uint64_t map_sig = 0;
    std::vector<uint64_t> page_map;
    struct Run { uint64_t off, guest, n; };
    std::vector<Run> runs;
    uint32_t runs_maps_gen = ~0u;
    bool gpu_written = false, only_cleared = false;
    uint32_t gpu_gen = 0;
    bool released = false;
    uint32_t release_gen = 0;
    uint64_t last_written = 0, first_written = 0, upload_value = 0, upload_seg = 0, bytes = 0;
    uint32_t id = 0;
    uint64_t first_seen = 0;
};
struct nvn_tex { uint64_t gpu = 0; };
std::unordered_map<uint32_t, std::unique_ptr<Texture>> textures;
std::unordered_multimap<uint64_t, Texture*> by_gpu;
uint64_t frame_no = 1, rec_value = 0;
thread_local uint64_t tl_seg = 0;
void logf(const char*, ...) {}
void vw_note(Texture&) {}
void watch_tex(const Texture*, const char*) {}
void drawn_writeback_note(Texture*) {}

// (the host's GPU mappings: each 64 KB virtual page's guest address, and their generations. `calls` counts what the code
// cut out asks of them and of the page generations: a page walk or a memory check)
std::map<uint64_t, uint64_t> vpages;
uint32_t maps_now = 1, vmaps_now = 1;
int calls = 0;
uint64_t gpu_guest(uint64_t gpu) {
    calls++;
    const auto it = vpages.find(gpu & ~0xFFFFull);
    return it == vpages.end() ? 0 : it->second + (gpu & 0xFFFF);
}
uint32_t maps_generation() {
    calls++;
    return maps_now;
}
uint32_t vmap_generation() {
    return vmaps_now;
}
struct Host {
    uint64_t (*gpu_guest)(uint64_t gpu);
    uint32_t (*maps_generation)();
    uint32_t (*vmap_generation)();
};
const Host the_host{gpu_guest, maps_generation, vmap_generation};
const Host* host = &the_host;

// (guest memory's page generations as vk_memory.cpp keeps them: 4 KB pages, a generation for each collection of the
// CPU's writes)
std::map<uint64_t, uint32_t> page_gen;
uint32_t gen_now = 1;
uint32_t memory_generation() {
    return gen_now;
}
void cpu_write(uint64_t guest, uint64_t n) {
    gen_now++;
    for (uint64_t p = guest >> 12; p <= (guest + n - 1) >> 12; p++) page_gen[p] = gen_now;
}
void memory_written_runs(uint64_t guest, uint64_t size, uint32_t gen, Ranges& out) {
    calls++;
    if (!size) return;
    for (uint64_t p = guest >> 12; p <= (guest + size - 1) >> 12; p++) {
        const auto it = page_gen.find(p);
        if (it == page_gen.end() || it->second <= gen) continue;
        const uint64_t lo = std::max(p << 12, guest) - guest, hi = std::min((p + 1) << 12, guest + size) - guest;
        if (!out.empty() && out.back().second == lo) out.back().second = hi;
        else out.push_back({lo, hi});
    }
}

CUT_MIX
CUT_DIRTY
CUT_DECLARATION

CUT_WRITTEN
CUT_RELEASED
// (texture_touch, its lines as they are: what a check took for written is left in `dirty`, and an upload sets what it
// sets there)
Ranges dirty;
Ranges& cpu = dirty;
void upload_done(Texture* tex, uint32_t gen) {
    tex->uploaded = true, tex->upload_gen = gen, tex->gpu_written = tex->released = false;
}
// (its first upload of a texture, with the seed of the texture's page map: what a texture clear's resolve() makes,
// texture_get(t, true), before the clear)
void first_upload(Texture* tex) {
    const uint32_t gen = memory_generation();
CUT_SEED
    upload_done(tex, gen);
}
// (its check of a texture uploaded, or marked so by texture_written: the texture back when it is not uploaded again,
// nullptr when it is)
Texture* touch(Texture* tex) {
    const bool gpu_dirty = false;   // (no buffer the GPU wrote over its storage)
CUT_SINCE
CUT_CHECK
    upload_done(tex, memory_generation());
    return nullptr;
}
// (its check of a texture the GPU owns, made for one only cleared or with RX_VK_CPU_OVER_GPU=1: the texture back when it
// takes nothing, nullptr when what the CPU wrote is uploaded)
Texture* live(Texture* tex) {
    CUT_LIVE
    return nullptr;
}
}  // namespace mkwvk

using namespace mkwvk;
namespace {
int failed = 0;
Ranges span(uint64_t lo, uint64_t hi) {
    return Ranges{{lo, hi}};
}
std::string text(const Ranges& r) {
    std::string s;
    for (const auto& [lo, hi] : r) {
        char b[64];
        std::snprintf(b, sizeof b, "%s{%#llx, %#llx}", s.empty() ? "" : " ", (unsigned long long)lo, (unsigned long long)hi);
        s += b;
    }
    return s.empty() ? "nothing" : s;
}
void expect(bool ok, const char* what, const Ranges* got = nullptr) {
    if (ok) return;
    failed++;
    std::printf("FAIL: %s%s%s\n", what, got ? "; taken for written: " : "", got ? text(*got).c_str() : "");
}
Texture* make(uint64_t gpu, uint64_t guest, uint64_t size) {
    auto t = std::make_unique<Texture>();
    t->id = (uint32_t)textures.size() + 1, t->gpu = gpu, t->guest = guest, t->guest_size = t->bytes = size;
    for (uint64_t off = 0; gpu >= (1ull << 40) && off < size; off += 0x10000) vpages[gpu + off] = guest + off;
    Texture* p = t.get();
    by_gpu.emplace(gpu, p);
    textures[p->id] = std::move(t);
    return p;
}
void finalize(Texture* t) {   // (the game finalizes it: texture_released, as the replay calls it)
    nvn_tex n;
    n.gpu = t->gpu;
    texture_released(&n);
    frame_no++;
}
}  // namespace

int main(int argc, char**) {
    const bool old_rule = argc > 1;   // (RX_VK_NO_DRAWN_PAGE_MAP=1)
    const uint64_t vpool = 1ull << 40;
    cpu_write(0x80003000, 0x100);   // (the game wrote some of that memory once, before any of this)

    // A HUD label's kind: 48 KB in a virtual pool, drawn by a pass before anything uploaded it, finalized by the game and
    // sampled again with nothing written since
    Texture* label = make(vpool + 0x1f000000, 0x80000000, 0xC000);
    texture_written(label, "a pass's colour target");
    const bool noted = !label->page_map.empty();
    finalize(label);
    Texture* kept = touch(label);
    if (old_rule) expect(!kept && dirty == span(0, 0xC000), "as before: a drawn texture, finalized, all of it", &dirty);
    else expect(kept == label, "a texture the GPU drew in a virtual pool, finalized, nothing written since: not uploaded again",
                &dirty);
    expect(old_rule ? !noted : noted,
           old_rule ? "as before: no page map noted at the GPU's write" : "its page map noted at the GPU's first write");

    // The same after other pages were mapped anew (a zone change's): its own as they were
    Texture* icon = make(vpool + 0x1f010000, 0x80010000, 0x8000);
    texture_written(icon, "a pass's colour target");
    finalize(icon);
    vpages[vpool + 0x2f000000] = 0x81000000, maps_now++;
    kept = touch(icon);
    if (old_rule) expect(!kept && dirty == span(0, 0x8000), "as before: other pages remapped, all of it", &dirty);
    else expect(kept == icon, "other pages remapped, its own as they were: not uploaded again", &dirty);

    // What the CPU wrote after the finalize still counts: that page alone
    Texture* rewritten = make(vpool + 0x1f020000, 0x80020000, 0x4000);
    texture_written(rewritten, "a pass's colour target");
    finalize(rewritten);
    cpu_write(0x80021000, 0x10);
    kept = touch(rewritten);
    expect(!kept && dirty == (old_rule ? span(0, 0x4000) : span(0x1000, 0x2000)),
           old_rule ? "as before: written after the finalize, all of it" : "written by the CPU after the finalize: that page",
           &dirty);

    // A page remapped after the finalize: that page alone
    Texture* depth = make(vpool + 0x1f100000, 0x80100000, 0x20000);
    texture_written(depth, "a pass's depth target");
    finalize(depth);
    vpages[vpool + 0x1f110000] = 0x80300000, vmaps_now++;
    kept = touch(depth);
    expect(!kept && dirty == (old_rule ? span(0, 0x20000) : span(0x10000, 0x20000)),
           old_rule ? "as before: a page remapped, all of it" : "its second 64 KB page remapped: that page", &dirty);

    // RX_VK_CPU_OVER_GPU=1 checks a texture the GPU still owns too: what the CPU wrote since the GPU's write, that page alone
    Texture* target = make(vpool + 0x1f600000, 0x80700000, 0x8000);
    texture_written(target, "the dry pass (a draw's target)");
    cpu_write(0x80704000, 0x10);
    kept = live(target);
    expect(!kept && dirty == (old_rule ? span(0, 0x8000) : span(0x4000, 0x5000)),
           old_rule ? "as before: RX_VK_CPU_OVER_GPU=1, the CPU's write over a texture the GPU owns, all of it"
                    : "RX_VK_CPU_OVER_GPU=1, the CPU's write over a texture the GPU owns: that page", &dirty);

    // Drawn and finalized twice: one page walk, at the first write, and nothing taken either time
    Texture* twice = make(vpool + 0x1f400000, 0x80500000, 0x10000);
    texture_written(twice, "a pass's colour target");
    finalize(twice);
    kept = touch(twice);
    if (old_rule) expect(!kept && dirty == span(0, 0x10000), "as before: drawn and finalized, all of it", &dirty);
    else expect(kept == twice, "drawn and finalized: not uploaded again", &dirty);
    int before = calls;
    texture_written(twice, "a pass's colour target");
    expect(calls == before, "drawn again: no second page walk");
    finalize(twice);
    kept = touch(twice);
    expect(kept == twice, "drawn and finalized again, nothing written since: not uploaded again", &dirty);

    // A writer that gives no reason ("?", the declaration's default: the upscaler's stand-ins, a texture clear) notes no
    // map. A texture only such a writer wrote (a stand-in's output, which it does not sample first), finalized: all of it
    // at its first check, as before
    Texture* scene = make(vpool + 0x1f300000, 0x80400000, 0x10000);
    before = calls;
    texture_written(scene);
    texture_written(scene, "?");
    expect(calls == before && scene->page_map.empty(), "a writer that gives no reason: no page walk, no map");
    finalize(scene);
    kept = touch(scene);
    expect(!kept && dirty == span(0, 0x10000), "a texture only a writer with no reason wrote, finalized: all of it, as before",
           &dirty);

    // Such a texture (a stand-in's output) that a pass then draws: its map noted at the pass's write; finalized, not uploaded
    // again
    Texture* later = make(vpool + 0x1f700000, 0x80800000, 0x10000);
    texture_written(later);
    texture_written(later, "a pass's colour target");
    const bool later_noted = !later->page_map.empty();
    finalize(later);
    kept = touch(later);
    if (old_rule) expect(!kept && dirty == span(0, 0x10000), "as before: no reason, then a pass, finalized: all of it", &dirty);
    else expect(kept == later, "written with no reason, then drawn by a pass, finalized: not uploaded again", &dirty);
    expect(old_rule ? !later_noted : later_noted,
           old_rule ? "as before: no page map noted at the pass's write" : "written with no reason, then drawn by a pass: "
                                                                          "its page map noted at the pass's write");

    // A texture clear's texture: resolve() samples it before the clear (texture_get(t, true)), and that first upload noted
    // its map. The clear walks nothing, and what the CPU writes after it (a font cache's letters) is taken alone at the
    // check of a texture only cleared, as before
    Texture* glyphs = make(vpool + 0x1f200000, 0x80200000, 0x10000);
    first_upload(glyphs);
    expect(!glyphs->page_map.empty(), "a texture sampled before its clear: its page map noted at that first upload");
    before = calls;
    texture_written(glyphs);
    glyphs->only_cleared = true;   // (as clear_tex sets it, after the call)
    expect(calls == before, "a texture clear (no reason): no page walk");
    cpu_write(0x80205000, 0x40);
    kept = live(glyphs);
    expect(!kept && dirty == span(0x5000, 0x6000), "a texture only cleared, then written by the CPU: that page alone, as before",
           &dirty);

    // Below 1 TB the pages are checked where they are: no map
    Texture* low = make(0x90000000, 0x90000000, 0x8000);
    before = calls;
    texture_written(low, "a pass's colour target");
    expect(calls == before && low->page_map.empty(), "below 1 TB: no page walk, no map");
    finalize(low);
    kept = touch(low);
    expect(kept == low, "below 1 TB, finalized, nothing written since: not uploaded again, as before", &dirty);

    // A texture of no size: no page walk
    Texture* none = make(vpool + 0x1f500000, 0x80600000, 0);
    before = calls;
    texture_written(none, "a pass's colour target");
    expect(calls == before, "a texture of no size: no page walk");

    if (failed) return 1;
    std::printf("PASS: %s\n", old_rule ? "RX_VK_NO_DRAWN_PAGE_MAP=1: no page map noted at the GPU's write, all of a finalized "
                                         "texture taken at its first check, as before"
                                       : "a texture the GPU drew in a virtual pool has its page map noted at that write: after "
                                         "a finalize, only what was remapped or written since is taken for written");
    return 0;
}
'''

# texture_touch's lines the fixture runs: its first-upload seed of a virtual-pool texture's page map, the generation and
# the check of a texture already uploaded, and the check of one the GPU owns
SEED = "    if (!tex->uploaded && tex->gpu >= (1ull << 40) && tex->page_map.empty()) {"
SINCE = "    const uint32_t since = "
CHECK = "    if (tex->uploaded && !gpu_dirty && !texture_dirty(*tex, since, dirty)) return tex;"
LIVE = "if (!texture_dirty(*tex, std::max(tex->upload_gen, tex->gpu_gen), cpu)) return tex;"

# (C++ comments, and the string and character literals that may hold what looks like one: blanked, each character but a
# newline a space, so that offsets and line numbers stay)
LITERAL = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^()\\\s]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])\'', re.S)
# (texture_written named in the code, and a call to it with its arguments, string literals with parentheses in them included)
NAME = re.compile(r"\btexture_written\b")
CALL = re.compile(r'texture_written\s*\(((?:[^()"]|"[^"]*")*)\)')


def blank(text, strings):
    """text with its comments blanked, and its string and character literals too when strings is true"""
    def sub(m):
        return m.group() if not strings and not m.group().startswith("/") else re.sub(r"[^\n]", " ", m.group())
    return LITERAL.sub(sub, text)


def cut(text, start, what):
    """From start, found once, to the end of its function: the first closing brace at the start of a line"""
    if text.count(start) != 1:
        raise SystemExit("%s: %r is there %d times, not once: update this check" % (what, start, text.count(start)))
    at = text.index(start)
    return text[at:text.index("\n}\n", at) + 3]


def block(text, start, what):
    """From start, found once, to the closing brace of its block, at start's own indentation"""
    if text.count(start) != 1:
        raise SystemExit("%s: %r is there %d times, not once: update this check" % (what, start, text.count(start)))
    at = text.index(start)
    indent = start[:len(start) - len(start.lstrip())]
    return text[at:text.index("\n" + indent + "}\n", at) + len(indent) + 3]


def line(text, start, what):
    """From start, found once, to the end of its line"""
    if text.count(start) != 1:
        raise SystemExit("%s: %r is there %d times, not once: update this check" % (what, start, text.count(start)))
    at = text.index(start)
    return text[at:text.index("\n", at)]


def check_writers():
    """Every reason texture_written is given in gpu/vk is in NOTED_BY, given by one writer in the file NOTED_BY names, and
    NOTED_BY names no reason no writer gives (what comments and string literals hold left out)"""
    given, none = {}, 0
    vk = ROOT / "gpu" / "vk"
    for path in sorted(vk.rglob("*.cpp")) + sorted(vk.rglob("*.h")):
        data = path.read_bytes()
        if b"texture_written" not in data:
            continue
        text = data.decode("utf-8")
        code, bare = blank(text, False), blank(text, True)
        name = path.relative_to(ROOT).as_posix()
        for m in NAME.finditer(bare):
            if re.search(r"\bvoid\s+$", bare[max(0, m.start() - 16):m.start()]):
                continue   # (its declaration and its definition)
            where = "%s:%d" % (name, text.count("\n", 0, m.start()) + 1)
            call = CALL.match(code, m.start())
            if not call:
                raise SystemExit("%s: texture_written where this check cannot read the call: update CALL" % where)
            args = call.group(1).strip()
            reason = re.fullmatch(r'[^,"]+,\s*"([^"]*)"', args)
            if not reason and not re.fullmatch(r'[^,"]+', args):
                raise SystemExit("%s: texture_written(%s): a reason that is not a string literal, or one more argument: "
                                 "this check cannot tell whether that writer notes the page map" % (where, args))
            if reason and reason.group(1) != "?":
                given.setdefault(reason.group(1), []).append(where)
            else:
                none += 1
    new = sorted(set(given) - set(NOTED_BY))
    if new:
        raise SystemExit("texture_written is given a reason that is not in tests/test_drawn_page_map.py NOTED_BY:\n" +
                         "".join("  %r (%s)\n" % (r, ", ".join(given[r])) for r in new) +
                         "With a reason it notes a virtual-pool texture's page map, which texture_dirty changes: check that "
                         "the writer holds the parallel replay's ParLock (or is its dry pass), then add it to NOTED_BY with "
                         "its file and where that lock is; or give it no reason")
    stale = sorted(set(NOTED_BY) - set(given))
    if stale:
        raise SystemExit("NOTED_BY names a reason no writer gives any more: " + ", ".join(repr(r) for r in stale))
    # (a writer that reuses a reason NOTED_BY has for another, copied with its call: one more writer to check)
    shared = sorted(r for r in given if [w.rsplit(":", 1)[0] for w in given[r]] != [NOTED_BY[r][0]])
    if shared:
        raise SystemExit("texture_written is given a reason NOTED_BY has for one writer by another writer too, or in another "
                         "file than NOTED_BY's:\n" +
                         "".join("  %r (%s; NOTED_BY: %s)\n" % (r, ", ".join(given[r]), NOTED_BY[r][0]) for r in shared) +
                         "Check that each of them holds the parallel replay's ParLock (or is its dry pass), then give the new "
                         "one a reason of its own in NOTED_BY, with its file and where that lock is; or give it no reason")
    print("PASS: the %d reasons texture_written is given are the ones NOTED_BY lists, each by one writer in its file; %d "
          "writers give none" % (len(given), none))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu" / "vk" / "vk_texture.cpp",
                   help="the vk_texture.cpp to cut the code from (main's: the check fails there)")
    a = p.parse_args()
    check_writers()
    src = a.source.read_text(encoding="utf-8")
    header = (ROOT / "gpu" / "vk" / "vk_texture.h").read_text(encoding="utf-8")
    declaration = line(header, "void texture_written(", "vk_texture.h")
    if 'const char* why = "?")' not in declaration:
        raise SystemExit('vk_texture.h: texture_written no longer has why = "?" for the writers that give no reason: ' +
                         declaration)
    declaration = declaration[:declaration.index(";") + 1]
    touch = cut(src, "Texture* texture_touch(Texture* tex, bool for_sampling) {", "texture_touch")
    code = FIXTURE
    for name, part in (("CUT_MIX", cut(src, "uint64_t mix(uint64_t h, uint64_t v) {", "mix")),
                       ("CUT_DIRTY", cut(src, "bool texture_dirty(Texture& tex, uint32_t gen, Ranges& out) {", "texture_dirty")),
                       ("CUT_DECLARATION", declaration),
                       ("CUT_WRITTEN", cut(src, "void texture_written(Texture* tex, const char* why) {", "texture_written")),
                       ("CUT_RELEASED", cut(src, "void texture_released(const nvn_tex* t) {", "texture_released")),
                       ("CUT_SEED", block(touch, SEED, "texture_touch")),
                       ("CUT_SINCE", line(touch, SINCE, "texture_touch")),
                       ("CUT_CHECK", line(touch, CHECK, "texture_touch")),
                       ("CUT_LIVE", line(touch, LIVE, "texture_touch"))):
        code = code.replace(name, part)
    cxx = shutil.which(a.cxx) or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        print("SKIP: no C++ compiler")
        return
    with tempfile.TemporaryDirectory(prefix="drawn-page-map-") as tmp:
        tmp = Path(tmp)
        (tmp / "f.cpp").write_text(code, encoding="utf-8")
        exe = tmp / ("f.exe" if os.name == "nt" else "f")
        # (env_on from the renderer's own header, ahead of the code cut out; _CRT_SECURE_NO_WARNINGS: Windows' C runtime
        # marks getenv deprecated, an error with -Werror)
        built = subprocess.run([cxx, "-std=c++20", "-D_CRT_SECURE_NO_WARNINGS", "-include", str(ENV_ON_H), "-O0", "-Wall",
                                "-Wextra", "-Werror", str(tmp / "f.cpp"), "-o", str(exe)])
        if built.returncode:
            raise SystemExit("FAIL: the code cut out of %s does not build in this test's FIXTURE: if the error names a "
                             "Texture field, a global or a function that code uses, add it to FIXTURE" % a.source.name)
        env = {k: v for k, v in os.environ.items() if k not in (SWITCH, "RX_VK_LOG_REMAP")}
        env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")   # (a compiler's runtime DLLs, llvm-mingw's)
        for extra in ({}, {SWITCH: "1"}):
            r = subprocess.run([str(exe)] + (["old"] if extra else []), env={**env, **extra}, capture_output=True, text=True)
            print((r.stdout + r.stderr).strip())
            if r.returncode:
                raise SystemExit("FAIL%s: the code cut out of %s" % (" with %s=1" % SWITCH if extra else "", a.source.name))


if __name__ == "__main__":
    main()
