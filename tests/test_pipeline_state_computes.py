"""The pipeline state list's compute records: the compute programs play dispatched, named by their compute stage's code key in a
version 2 record of kind kCompute (vk_draw.cpp StateRec2), in the same lists, with the same tools, as the pipelines.

- nx2/pipeline_states.py: the record's layout, what describe and the merge make of it (each program once, with version 1 lists
  too), the checks (a compute record that holds more than its key, a kind no build knows, more compute programs than the release's check
  allows: it stops and says so, nothing is dropped), the tour's and a contributed source, and nx2/make_game.py shipping them beside the game;
- a list someone sent (add, --check): taken only when it is exactly a list. Refused, and nothing written: one cut short, one with
  a kind no build knows or a compute record that holds more than its key, one that names more compute programs than the
  game has, one larger than a list is (as a file, or once unpacked), an xz file cut short or with more after it, a list of
  another version. A list from before the kinds, and a version 1 one, are taken as they were;
- the game's own code, cut out of vk_draw.cpp and compiled with stubs for the folder: state_record_compute writes each program
  once (and once over two runs); state_list_read gives the pipelines alone, state_list_computes the programs alone; a list
  from before the kinds, and a version 1 one, read as they did; a build from before the kinds drops a compute record (targets_ok);
  computes_plan keeps ready the shipped list's and the learned ones (hookshot_computes.bin), each once, whatever is missing
  or damaged, and every one a shipped list names however many; on a first start, with nothing learned, every kept program
  as before (the shipped list's alone with RX_VK_PRECOMPILE_COMPUTES=listed); =learned / all / none;
- a list the game wrote reads in the tool and one the tool wrote reads in the game;
- precompile_run takes its compute programs from that plan, and only dispatch() records one;
- its compute arm, cut out and run against a made-up driver: a pipeline the caches hold is asked for and counted as cached, one
  they lack is compiled and counted so, one the driver refuses is not kept, and a driver that cannot say makes each with one call.
Synthetic data only. python -I -B tests/test_pipeline_state_computes.py [--cxx <compiler>]
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import contextlib
import importlib.util
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "nx2" / "pipeline_states.py"
K1, K2, K3 = 0x1111222233334444, 0xAAAABBBBCCCCDDDD, 0x0123456789ABCDEF


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def block(text, marker, semicolon=False):
    start = text.index(marker)
    brace = text.index("{", start)
    end = brace + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + (";" if semicolon else "")


def stops(fn, *args):
    """What fn stops with (None when it does not), its own output kept off the test's"""
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            fn(*args)
    except SystemExit as e:
        return str(e)
    return None


def graphics(tool, vs, fs=0):
    """A version 2 graphics record (kind 0: what every list held before the kinds)"""
    r = bytearray(tool.SIZE2)
    struct.pack_into("<QQ", r, 0, vs, fs)
    struct.pack_into("<4I", r, 44, 3, 1, 1, 0)   # (topology, one target, one sample, no depth)
    return bytes(r)


def check_tool(tool, draw):
    # the layout, against the back end's
    if not re.search(r"static constexpr uint32_t kCompute = %d;" % tool.COMPUTE, draw):
        raise SystemExit("the back end's StateRec2::kCompute and the tool's COMPUTE differ")
    rec = block(draw, "struct StateRec2 {")
    if not re.search(r"uint8_t questions\[32\];\s*uint32_t kind;[^\n]*\n\s*uint32_t pad;", rec) or tool.KIND_AT != tool.SIZE2 - 8:
        raise SystemExit("StateRec2's kind is not the record's second last word, where the tool reads it")
    c = tool.compute_record(K1)
    if len(c) != tool.SIZE2 or c[:8] != struct.pack("<Q", K1) or c[tool.KIND_AT:] != struct.pack("<II", 1, 0) or any(c[8:tool.KIND_AT]):
        raise SystemExit("a compute record is not its key, its kind and zeros")
    g = graphics(tool, 5, 6)
    if tool.kind(c) != tool.COMPUTE or tool.kind(g) != 0 or tool.kind(bytes(tool.SIZE)) != 0:
        raise SystemExit("kind(): a compute record, a version 2 pipeline, a version 1 record")
    recs = [g, c, tool.compute_record(K2), c]
    if tool.compute_keys(recs) != [K1, K2] or tool.compute_keys([g]) != []:
        raise SystemExit("compute_keys: each program once, in order")
    said = tool.describe(tool.unique([recs]))
    if "3 pipelines (compute 2, graphics 1) of 3 programs" not in said:
        raise SystemExit("describe: %s" % said)
    # the checks: only records the game writes
    more = bytearray(c)
    struct.pack_into("<I", more, 52, 1)   # (a sample count: a compute record has none)
    unknown = bytearray(g)
    struct.pack_into("<I", unknown, tool.KIND_AT, 7)
    for bad, why in ((bytes(more), "holds more than its key"), (tool.compute_record(0), "holds more than its key"),
                     (bytes(unknown), "kind 7")):
        if why not in (stops(tool.check_kinds, [g, bad], "x") or ""):
            raise SystemExit("check_kinds let through a record that %s" % why)
    if stops(tool.check_kinds, recs, "x") is not None:
        raise SystemExit("check_kinds stopped on records the game writes")
    with tempfile.TemporaryDirectory() as d:
        # the command line: a version 1 list and two version 2 lists that name the same program merge into version 2, it once
        v1 = bytearray(tool.SIZE)
        struct.pack_into("<QQ", v1, 0, 1, 2)
        struct.pack_into("<6I", v1, 40, 0, 3, 1, 1, 0, 0)
        struct.pack_into("<I", v1, 64, 37)   # (RGBA8)
        a, b, old, out, bad = (os.path.join(d, n) for n in ("a.bin", "b.bin", "old.bin", "out.bin.xz", "bad.bin"))
        open(old, "wb").write(tool.MAGIC + struct.pack("<I", tool.SIZE) + bytes(v1))
        tool.write(a, [g, c])
        tool.write(b, [tool.compute_record(K2), c])
        ran = subprocess.run([sys.executable, "-I", str(TOOL), "--out", out, old, a, b], capture_output=True, text=True)
        if ran.returncode:
            raise SystemExit(ran.stdout + ran.stderr)
        merged = tool.read(out)
        if tool.load(out)[:8] != tool.MAGIC2 or merged[1:] != [g, c, tool.compute_record(K2)] or len(merged) != 4:
            raise SystemExit("merge: %d records, expected the version 1 pipeline converted, the pipeline and the two programs" % len(merged))
        ran = subprocess.run([sys.executable, "-I", str(TOOL), "--check", out], capture_output=True, text=True)
        if ran.returncode or "compute 2" not in ran.stdout:
            raise SystemExit("--check of a list with compute records: %s%s" % (ran.stdout, ran.stderr))
        open(bad, "wb").write(tool.MAGIC2 + struct.pack("<I", tool.SIZE2) + g + bytes(more))
        ran = subprocess.run([sys.executable, "-I", str(TOOL), "--check", bad], capture_output=True, text=True)
        if not ran.returncode or "holds more than its key" not in ran.stderr:
            raise SystemExit("--check let through a compute record that holds more than its key")
        # the sources: the tour's list and a contributed one carry them, the check counts them, the merge has each once
        src = os.path.join(d, "sources")
        fields = {"Who": "a test", "GPU and driver": "none", "Build": "none", "Settings": "none", "Route": "none"}
        with contextlib.redirect_stdout(io.StringIO()):
            tool.tour(src, [a], fields)
            tool.contribute(src, "someone-played", [b], fields)
            recs = tool.check_sources(src)
        if recs != [g, c, tool.compute_record(K2)]:
            raise SystemExit("the sources' merge: %d records, expected the tour's two and the contributed list's new program" % len(recs))
        if tool.read(os.path.join(src, tool.CONTRIB, "someone-played" + tool.EXT)) != [tool.compute_record(K2), c]:
            raise SystemExit("a contributed list keeps its own records, the tour's too (the tour's list is replaced whole)")
        # make_game ships them beside the game, unpacked, as the game reads them
        make_game = load("make_game_under_test", ROOT / "nx2" / "make_game.py")
        game = os.path.join(d, "game")
        os.makedirs(game)
        make_game.ROOT = d
        os.makedirs(os.path.join(d, tool.SOURCES[0]))
        os.replace(src, os.path.join(d, *tool.SOURCES))
        with contextlib.redirect_stdout(io.StringIO()):
            make_game.ship_state_list(game)
        shipped = os.path.join(game, "hookshot_pipeline_states_shipped.bin")
        if tool.compute_keys(tool.read(shipped, strict=True)) != [K1, K2] or open(shipped, "rb").read(8) != tool.MAGIC2:
            raise SystemExit("make_game did not ship the sources' compute programs beside the game")
        # more compute programs than play dispatches: the release's check stops (the game keeps a pipeline ready for each)
        # (several lists that each name fewer, or a source put there by hand: add itself refuses such a list, check_sent)
        src = os.path.join(d, *tool.SOURCES)
        many = [tool.compute_record(k) for k in range(1, tool.COMPUTE_MOST + 1)]
        path = os.path.join(src, tool.CONTRIB, "too-many" + tool.EXT)
        tool.write(path, many)
        tool.write_note(tool.note_path(path), "too-many", fields, many)
        said = stops(tool.check_sources, src) or ""
        if "more than COMPUTE_MOST" not in said or "Nothing is dropped" not in said:
            raise SystemExit("check let through a shipped list that names %d compute programs" % (tool.COMPUTE_MOST + 2))
        # (and the merge itself drops none of them: the build ships what the sources hold)
        with contextlib.redirect_stdout(io.StringIO()):
            kept = len(tool.compute_keys(tool.merged(src)))
        if kept != tool.COMPUTE_MOST + 2:
            raise SystemExit("the merge dropped compute programs")


def check_sent(tool):
    """add and --check, given files nobody here made"""
    g, c = graphics(tool, 5, 6), tool.compute_record(K1)
    head2 = tool.MAGIC2 + struct.pack("<I", tool.SIZE2)
    more = bytearray(c)
    struct.pack_into("<I", more, 52, 1)
    unknown = bytearray(g)
    struct.pack_into("<I", unknown, tool.KIND_AT, 2)
    v1 = bytearray(tool.SIZE)
    struct.pack_into("<QQ", v1, 0, 1, 2)
    struct.pack_into("<6I", v1, 40, 0, 3, 1, 1, 0, 0)
    struct.pack_into("<I", v1, 64, 37)
    xz = tool.compress(head2 + g + c)
    bad = {
        "cut short in a record": (head2 + g + c[:-1], "cut short"),
        "cut short in a compute record's key": (head2 + g + c[:4], "cut short"),
        "bytes after the last record": (head2 + g + c + b"\0", "cut short"),
        "a kind no build knows": (head2 + g + bytes(unknown), "kind 2"),
        "a compute record with more than its key": (head2 + bytes(more), "holds more than its key"),
        "a compute record with no key": (head2 + tool.compute_record(0), "holds more than its key"),
        "more compute programs than the game has":
            (head2 + b"".join(tool.compute_record(k) for k in range(1, tool.SENT_COMPUTE_MOST + 2)), "more than the game has"),
        "another version": (b"HKSTATE3" + struct.pack("<I", tool.SIZE2) + g, "not a pipeline state list"),
        "another record size": (tool.MAGIC2 + struct.pack("<I", 560) + bytes(560), "not a pipeline state list"),
        "no records": (head2, "holds no pipelines"),
        "not a list": (b"PK\x03\x04" + bytes(600), "not a pipeline state list"),
        "nothing": (b"", "not a pipeline state list"),
        "an xz file cut short": (xz[:-8], "damaged xz"),
        "an xz file with more after it": (xz + b"more", "damaged xz"),
        "an xz file of something else": (tool.compress(b"x" * 4000), "not a pipeline state list"),
    }
    good = {
        "pipelines and programs": (head2 + g + c + tool.compute_record(K2), 3),
        "the same program twice": (head2 + c + g + c, 2),   # (folded: each record once)
        "a list from before the kinds": (head2 + g + graphics(tool, 7, 8), 2),
        "a version 1 list": (tool.MAGIC + struct.pack("<I", tool.SIZE) + bytes(v1), 1),
        "an xz list": (xz, 2),
    }
    fields = {"Who": "a test", "GPU and driver": "none", "Build": "none", "Settings": "none", "Route": "none"}
    with tempfile.TemporaryDirectory() as d:
        src, sent = os.path.join(d, "sources"), os.path.join(d, "sent.bin")
        contrib = os.path.join(src, tool.CONTRIB)
        for name, (data, why) in bad.items():
            open(sent, "wb").write(data)
            said = stops(tool.contribute, src, "someone", [sent], fields)
            if why not in (said or ""):
                raise SystemExit("add took a sent list that is %s (%s)" % (name, said))
            if os.path.isdir(contrib) and os.listdir(contrib):
                raise SystemExit("add wrote %s for a sent list that is %s" % (os.listdir(contrib), name))
            ran = subprocess.run([sys.executable, "-I", str(TOOL), "--check", sent], capture_output=True, text=True)
            if not ran.returncode or why not in ran.stderr:
                raise SystemExit("--check took a sent list that is %s: %s%s" % (name, ran.stdout, ran.stderr))
        # larger than a list is: as a file (not read at all), and once unpacked (not unpacked past the limit)
        most = tool.SENT_MOST
        try:
            tool.SENT_MOST = 64 * tool.SIZE2
            big = head2 + b"".join(graphics(tool, k, 1) for k in range(1, 400))
            for name, data in (("a file larger than a list", big), ("an xz file that unpacks to more than a list", tool.compress(big))):
                open(sent, "wb").write(data)
                if len(data) > tool.SENT_MOST and name.startswith("an xz"):
                    raise SystemExit("the test's xz file is not smaller than the limit")
                said = stops(tool.contribute, src, "someone", [sent], fields)
                if "more than a pipeline state list holds" not in (said or "") or (os.path.isdir(contrib) and os.listdir(contrib)):
                    raise SystemExit("add took %s (%s)" % (name, said))
        finally:
            tool.SENT_MOST = most
        with open(sent, "wb") as f:   # (the real limit, on a file with nothing in it: a hole of that size, never read)
            f.truncate(tool.SENT_MOST + 1)
        if "more than a pipeline state list holds" not in (stops(tool.read_sent, sent) or ""):
            raise SystemExit("a sent file past %d bytes was read" % tool.SENT_MOST)
        if "no such file" not in (stops(tool.read_sent, os.path.join(d, "none.bin")) or ""):
            raise SystemExit("a sent list that is not there")
        for i, (name, (data, n)) in enumerate(good.items()):
            open(sent, "wb").write(data)
            if stops(tool.contribute, src, "sender-%d" % i, [sent], fields) is not None:
                raise SystemExit("add refused a sent list that is %s" % name)
            got = tool.read(os.path.join(contrib, "sender-%d%s" % (i, tool.EXT)), strict=True) if i in (0, 3) else None
            if got is not None and len(got) != n:
                raise SystemExit("add of %s: %d records, expected %d" % (name, len(got), n))
        with contextlib.redirect_stdout(io.StringIO()):
            recs = tool.check_sources(src)
        # (each record once over all of them, version 1's converted: the pipelines 5/6, 7/8 and 1/2, the programs K1 and K2)
        if len(recs) != 5 or tool.compute_keys(recs) != [K1, K2]:
            raise SystemExit("the sent lists' merge: %d records, programs %s" % (len(recs), tool.compute_keys(recs)))


def check_source(draw):
    """precompile_run keeps ready the plan's programs; a compute record is written by state_record_compute alone, at a dispatch"""
    def body_of(signature):
        at = draw.index(signature)
        return draw[at:draw.index("\n}\n", at)]
    pre = body_of("void precompile_run(")
    for line in ('const ComputePlan plan = computes_plan(getenv("RX_VK_PRECOMPILE_COMPUTES"), !env_on("RX_VK_NO_STATE_LIST"));',
                 "if (!plan.all && !plan.none) wanted.insert(plan.keys.begin(), plan.keys.end());",
                 "}, plan.all, true);"):
        if line not in pre:
            raise SystemExit("precompile_run does not take its compute programs from computes_plan: no `%s`" % line)
    if "computes_read()" in pre or "state_record_compute(" in pre or "state_record_compute(" in body_of("void state_list_prepare("):
        raise SystemExit("precompile_run reads the learned list past the plan, or the prepare writes compute records")
    calls = [m.start() for m in re.finditer(r"(?<![\w:])state_record_compute\(", draw)]
    define = draw.index("\nvoid state_record_compute(uint64_t key) {") + 6
    d = body_of("void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    once = "if (!p.dispatched.exchange(true, std::memory_order_relaxed)) compute_note(p.key), state_record_compute(p.key);"
    if sorted(calls) != sorted([define, draw.index(once) + once.index("state_record_compute(")]) or once not in d:
        raise SystemExit("a compute record is written from somewhere other than a program's first dispatch")
    rec = body_of("void state_record_compute(uint64_t key) {")
    if 'static const bool off = env_on("RX_VK_NO_STATE_LIST");\n    if (off || !key) return;' not in rec:
        raise SystemExit("state_record_compute writes with RX_VK_NO_STATE_LIST=1, or a program with no key")
    for name, text in ((p.name, p.read_text(encoding="utf-8", errors="replace")) for p in (ROOT / "gpu" / "vk").glob("*.cpp")):
        if name != "vk_draw.cpp" and "state_record_compute(" in text:
            raise SystemExit("%s writes compute records" % name)


FIXTURE_HEAD = r'''
#define _CRT_SECURE_NO_WARNINGS 1
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
constexpr uint32_t VK_PRIMITIVE_TOPOLOGY_PATCH_LIST = 10;   // (vulkan_core.h)
enum VkFormat : uint32_t { VK_FORMAT_UNDEFINED = 0 };
typedef uint32_t VkFormatFeatureFlags;
constexpr uint32_t VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT = 0x80, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x200;
struct VkFormatProperties { uint32_t linearTilingFeatures, optimalTilingFeatures, bufferFeatures; };
struct { void* physical = (void*)1; } dev;
// (a device that draws to every format: these records are not about formats)
static void vkGetPhysicalDeviceFormatProperties(void*, VkFormat, VkFormatProperties* p) {
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
}
static bool is_depth_format(VkFormat) { return true; }
// the game folder, and the switches, as the test sets them
static std::string folder;
static std::string state_list_path(const char* name) { return folder + "/" + name; }
static std::string computes_path() { return folder + "/hookshot_computes.bin"; }
static bool no_state_list = false;
static bool env_on(const char* name) { return !std::strcmp(name, "RX_VK_NO_STATE_LIST") && no_state_list; }
static void logf(const char*, ...) {}
'''

FIXTURE_MAIN = r'''
// (the device's part of a record is not what is checked here: every pipeline record resolves, so that a compute record or one of
// an unknown kind that got as far as this would be counted as a pipeline)
static bool state_v1_portable(StateRec&) { return true; }
static bool state_v2_resolve(const StateRec2& v, StateRec& r) {
    r = StateRec{};
    std::memcpy(r.code, v.code, sizeof r.code);
    return true;
}
''' + "@READERS@" + r'''
static void put(const char* name, const char magic[8], uint32_t size, const std::vector<StateRec2>& recs, size_t tail = 0) {
    FILE* f = std::fopen(state_list_path(name).c_str(), "wb");
    CHECK(f);
    std::fwrite(magic, 8, 1, f), std::fwrite(&size, 4, 1, f);
    for (const StateRec2& r : recs) std::fwrite(&r, sizeof r, 1, f);
    if (tail) { StateRec2 r{}; r.kind = StateRec2::kCompute, r.code[0] = 0x7A11; std::fwrite(&r, tail, 1, f); }
    std::fclose(f);
}
static void put2(const char* name, const std::vector<StateRec2>& recs, size_t tail = 0) { put(name, kStateMagic2, sizeof(StateRec2), recs, tail); }
static void learned(const std::vector<uint64_t>& keys, const char* magic = kComputesMagic) {
    FILE* f = std::fopen(computes_path().c_str(), "wb");
    CHECK(f);
    std::fwrite(magic, 1, 8, f);
    for (const uint64_t k : keys) std::fwrite(&k, 8, 1, f);
    std::fclose(f);
}
static void gone(const char* name) { std::remove(state_list_path(name).c_str()); }
static StateRec2 compute(uint64_t key) { StateRec2 r{}; r.code[0] = key, r.kind = StateRec2::kCompute; return r; }
static StateRec2 pipeline(uint64_t vs) { StateRec2 r{}; r.code[0] = vs, r.samples = 1, r.n = 1, r.topo = 3; return r; }
static size_t file_size(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return (size_t)n;
}
using Keys = std::unordered_set<uint64_t>;
int main(int argc, char** argv) {
    CHECK(argc == 2);
    folder = argv[1];
    const uint64_t K1 = 0x1111222233334444ull, K2 = 0xAAAABBBBCCCCDDDDull, K3 = 0x0123456789ABCDEFull;
    const char* own = "hookshot_pipeline_states.bin";
    const char* shipped = "hookshot_pipeline_states_shipped.bin";
    // 1. the record: the list's own size, its kind where the padding was, and nothing a pipeline could be made of
    CHECK(sizeof(StateRec2) == 552 && offsetof(StateRec2, kind) == 544 && StateRec2::kCompute == 1);
    const StateRec2 c1 = compute(K1);
    {
        StateRec2 zero{};
        zero.code[0] = K1, zero.kind = 1;
        CHECK(!std::memcmp(&zero, &c1, sizeof c1));
    }
    // (a build from before the kinds reads it as a pipeline's record and drops it: 0 samples)
    CHECK(!targets_ok(c1.n, c1.samples, c1.topo));
    // 2. play: each program's first dispatch writes it, once; a second run adds to the list and does not repeat it
    state_record_compute(K1), state_record_compute(K2), state_record_compute(K1), state_record_compute(0);
    CHECK(state_file);
    std::fclose(state_file), state_file = nullptr, state_seen.clear();
    CHECK(file_size(state_list_path(own)) == 12 + 2 * sizeof(StateRec2));
    CHECK(state_list_computes(state_list_path(own)) == (std::vector<uint64_t>{K1, K2}));
    state_record_compute(K2), state_record_compute(K3);
    std::fclose(state_file), state_file = nullptr, state_seen.clear();
    CHECK(state_list_computes(state_list_path(own)) == (std::vector<uint64_t>{K1, K2, K3}));
    {
        int version = 0;
        CHECK(state_list_read(state_list_path(own), &version).empty() && version == 2);   // (no pipelines in it)
    }
    {   // (the list the test's Python reads back with nx2/pipeline_states.py)
        FILE* in = std::fopen(state_list_path(own).c_str(), "rb");
        FILE* out = std::fopen(state_list_path("game_wrote.bin").c_str(), "wb");
        CHECK(in && out);
        char b[4096];
        for (size_t n; (n = std::fread(b, 1, sizeof b, in)) > 0;) std::fwrite(b, 1, n, out);
        std::fclose(in), std::fclose(out);
    }
    {   // the programs this install's play dispatched before this build (hookshot_computes.bin) are in the list from its next
        // opening on, each once: the list a player sends names them all
        const uint64_t before = 0x5555666677778888ull;
        learned({K1, before});
        state_record_compute(K2);
        std::fclose(state_file), state_file = nullptr, state_seen.clear();
        CHECK(state_list_computes(state_list_path(own)) == (std::vector<uint64_t>{K1, K2, K3, before}));
        state_record_compute(before);
        std::fclose(state_file), state_file = nullptr, state_seen.clear();
        CHECK(file_size(state_list_path(own)) == 12 + 4 * sizeof(StateRec2));
        std::remove(computes_path().c_str());
        // (and into a new list: a player who moved theirs aside)
        gone(own);
        learned({before});
        state_record_compute(K1);
        std::fclose(state_file), state_file = nullptr, state_seen.clear();
        CHECK(state_list_computes(state_list_path(own)) == (std::vector<uint64_t>{before, K1}));
        std::remove(computes_path().c_str());
        gone(own);
    }
    // 3. a list the tool wrote (a pipeline and a program): each reader takes its own
    CHECK(state_list_computes(state_list_path("tool_wrote.bin")) == (std::vector<uint64_t>{K2}));
    {
        const std::vector<StateRec> got = state_list_read(state_list_path("tool_wrote.bin"));
        CHECK(got.size() == 1 && got[0].code[0] == 5 && got[0].code[1] == 6);
    }
    // 4. pipelines and programs mixed, a kind no build knows yet, a program with no key
    {
        StateRec2 unknown = pipeline(0x77);
        unknown.kind = 7;
        put2("mixed.bin", {pipeline(0x10), compute(K1), unknown, compute(0), pipeline(0x20), compute(K1), compute(K2)});
        int version = 0;
        const std::vector<StateRec> got = state_list_read(state_list_path("mixed.bin"), &version);
        CHECK(version == 2 && got.size() == 2 && got[0].code[0] == 0x10 && got[1].code[0] == 0x20);
        CHECK(state_list_computes(state_list_path("mixed.bin")) == (std::vector<uint64_t>{K1, K1, K2}));
    }
    // 5. lists from before the kinds read as they did: version 2 without programs, version 1
    {
        put2("old2.bin", {pipeline(0x10), pipeline(0x20)});
        CHECK(state_list_read(state_list_path("old2.bin")).size() == 2 && state_list_computes(state_list_path("old2.bin")).empty());
        StateRec v1{};
        v1.code[0] = 0x30, v1.samples = 1, v1.topo = 3;
        v1.pad = 1;   // (version 1's own padding, wherever it is: no kind is read from a version 1 record)
        FILE* f = std::fopen(state_list_path("old1.bin").c_str(), "wb");
        const uint32_t size = sizeof(StateRec);
        std::fwrite(kStateMagic, 8, 1, f), std::fwrite(&size, 4, 1, f), std::fwrite(&v1, sizeof v1, 1, f), std::fclose(f);
        int version = 0;
        CHECK(state_list_read(state_list_path("old1.bin"), &version).size() == 1 && version == 1);
        CHECK(state_list_computes(state_list_path("old1.bin")).empty());
    }
    // 6. lists that are not: missing, another magic, another record size, cut short in a record
    CHECK(state_list_computes(state_list_path("none.bin")).empty());
    put("bad_magic.bin", "HKSTATE9", sizeof(StateRec2), {compute(K1)});
    put("bad_size.bin", kStateMagic2, 600, {compute(K1)});
    CHECK(state_list_computes(state_list_path("bad_magic.bin")).empty() && state_list_computes(state_list_path("bad_size.bin")).empty());
    put2("cut.bin", {compute(K1)}, sizeof(StateRec2) - 1);   // (the last one's key and kind are there, its end is not)
    CHECK(state_list_computes(state_list_path("cut.bin")) == (std::vector<uint64_t>{K1}));
    { FILE* f = std::fopen(state_list_path("empty.bin").c_str(), "wb"); std::fclose(f); }
    CHECK(state_list_computes(state_list_path("empty.bin")).empty());
    // 7. what the precompile keeps ready: the shipped list's and the learned ones, each once (the player's own state list names
    // what the learned list holds, and is not read for it: E)
    const uint64_t A = 0xA, B = 0xB, C = 0xC, D = 0xD, E = 0xE;
    put2(shipped, {pipeline(0x10), compute(A), compute(B), compute(B), compute(C)});
    put2(own, {compute(C), pipeline(0x20), compute(E)});
    learned({C, D});
    {
        const ComputePlan p = computes_plan(nullptr, true);
        CHECK(p.keys == (Keys{A, B, C, D}) && !p.all && !p.none && !p.learned_only);
        CHECK(p.shipped == 3 && p.learned == 2 && p.fresh == 2);
        const ComputePlan q = computes_plan("listed", true);   // (the same once play has dispatched some)
        CHECK(q.keys == p.keys && !q.all && !q.none);
    }
    {   // the switch for an A/B: the learned ones alone, as before the list named any (and with RX_VK_NO_STATE_LIST)
        const ComputePlan p = computes_plan("learned", true), q = computes_plan(nullptr, false);
        CHECK(p.keys == (Keys{C, D}) && p.learned_only && !p.all && !p.none && !p.shipped && !p.fresh && p.learned == 2);
        CHECK(q.keys == (Keys{C, D}) && !q.learned_only && !q.all && !q.none && !q.shipped);
    }
    CHECK(computes_plan("all", true).all && !computes_plan("all", true).none);
    CHECK(computes_plan("none", true).none && !computes_plan("none", true).all);
    {   // a first start, nothing dispatched here yet: every kept program, as before the list named any (a program the list
        // misses is then ready too); =listed: the shipped list's alone
        gone(own), std::remove(computes_path().c_str());
        const ComputePlan p = computes_plan(nullptr, true), q = computes_plan("listed", true);
        CHECK(p.all && !p.none && p.shipped == 3 && !p.learned && p.fresh == 3);
        CHECK(q.keys == (Keys{A, B, C}) && !q.all && !q.none && q.shipped == 3 && !q.learned && q.fresh == 3);
        // (its own play's programs lost, the learned list not one: as a first start)
        learned({C, D}, "NOTALIST");
        CHECK(computes_plan(nullptr, true).all && computes_plan("listed", true).keys == (Keys{A, B, C}));
        // (and without the lists, nothing names any: every kept one, as before)
        CHECK(computes_plan("learned", true).all && computes_plan("learned", true).keys.empty());
    }
    {   // the learned ones alone: a shipped list from before the kinds, a damaged one, none
        learned({C, D});
        put2(shipped, {pipeline(0x10), pipeline(0x20)});
        ComputePlan p = computes_plan(nullptr, true);
        CHECK(p.keys == (Keys{C, D}) && !p.all && !p.shipped && !p.fresh);
        put(shipped, "HKSTATE9", sizeof(StateRec2), {compute(A)});
        p = computes_plan(nullptr, true);
        CHECK(p.keys == (Keys{C, D}) && !p.all && !p.shipped);
        gone(shipped);
        p = computes_plan(nullptr, true);
        CHECK(p.keys == (Keys{C, D}) && !p.all && !p.shipped);
        CHECK(computes_plan("listed", true).keys == (Keys{C, D}));
    }
    {   // a shipped list however long: every program it names, none dropped (the bound is the release's check, not the game)
        std::vector<StateRec2> many;
        for (uint64_t k = 1; k <= 5000; k++) many.push_back(compute(0x100000 + k)), many.push_back(compute(0x100000 + k));
        put2(shipped, many);
        const ComputePlan p = computes_plan(nullptr, true);
        CHECK(p.shipped == 5000 && p.fresh == 5000 && p.keys.size() == 5002 && !p.all);
        CHECK(p.keys.count(0x100001) && p.keys.count(0x100000 + 5000) && p.keys.count(C) && p.keys.count(D));
        gone(shipped);
    }
    {   // nothing anywhere names a compute program: every kept one, as before the list named any
        std::remove(computes_path().c_str());
        const ComputePlan p = computes_plan(nullptr, true);
        CHECK(p.keys.empty() && p.all && !p.none && computes_plan("listed", true).all);
        CHECK(computes_plan("none", true).none && !computes_plan("none", true).all);
    }
    std::printf("PASS: compute records written once, read apart from the pipelines, old lists as before, the plan of shipped, own and learned\n");
    return 0;
}
'''


def check_game(tool, draw, cxx):
    readers = "\n".join((
        block(draw, "bool targets_ok("), block(draw, "bool formats_ok("),
        block(draw, "constexpr char kComputesMagic[8]", True), block(draw, "std::unordered_set<uint64_t> computes_read()"),
        block(draw, "constexpr char kStateMagic[8]", True), block(draw, "constexpr char kStateMagic2[8]", True),
        block(draw, "struct StateRec {", True), block(draw, "struct StateRec2 {", True),
        "template <class R>\n" + block(draw, "uint64_t state_hash(const R& r)"),
        "@STUBS@",
        block(draw, "std::vector<StateRec> state_list_read("), block(draw, "std::vector<uint64_t> state_list_computes("),
        block(draw, "struct ComputePlan {", True), block(draw, "ComputePlan computes_plan("),
        "std::mutex state_mu;\nFILE* state_file = nullptr;\nstd::unordered_set<uint64_t> state_seen;",
        block(draw, "static void cut_part_record(") if "static void cut_part_record(" in draw else "",
        block(draw, "void state_open_locked()"), block(draw, "void state_record_compute(uint64_t key)")))
    head, _, tail = FIXTURE_MAIN.partition("@READERS@")
    fixture = FIXTURE_HEAD + readers.replace("@STUBS@", head) + tail
    g = graphics(tool, 5, 6)
    with tempfile.TemporaryDirectory(prefix="state-computes-") as tmp:
        folder = Path(tmp)
        source, exe = folder / "fixture.cpp", folder / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        tool.write(str(folder / "tool_wrote.bin"), [g, tool.compute_record(K2)])
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-function", "-Werror", str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(folder)], check=True)
        # the list the game's code wrote, in the tool: the records it makes itself, and a list the release's check takes
        wrote = str(folder / "game_wrote.bin")
        recs = tool.check_shipped(wrote)
        if recs != [tool.compute_record(k) for k in (K1, K2, K3)] or tool.compute_keys(recs) != [K1, K2, K3]:
            raise SystemExit("the tool does not read the compute records the game wrote as the ones it makes")


ARM_HEAD = r"""
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkResult = int;
using VkPipeline = uintptr_t;
constexpr VkResult VK_SUCCESS = 0, VK_PIPELINE_COMPILE_REQUIRED = 1000297000;
constexpr uintptr_t VK_NULL_HANDLE = 0;
constexpr int VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 10, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 11;
constexpr int VK_SHADER_STAGE_COMPUTE_BIT = 12, VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT = 0x100;
constexpr int VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT = 0x1;
static bool env_on(const char*) { return false; }
struct VkComputePipelineCreateInfo {
    int sType;
    struct { int sType = 0, stage = 0; uintptr_t module = 0; const char* pName = nullptr; } stage;
    uintptr_t layout = 0;
    int flags = 0;
};
struct { uintptr_t device = 0x1234; struct { bool pipelineCreationCacheControl = true; } vk13; } dev;
struct Program {
    struct Stage { uintptr_t module; };
    std::vector<Stage> stages{{0x2468}};
    unsigned workgroup[3]{32, 1, 1};
    uintptr_t layout = 0x1357;
    VkPipeline compute_pipeline = 0xbeef;
};
struct Pre {
    std::mutex mu;
    bool abandoned = false;
    std::atomic<size_t> computes_cached{0}, computes_compiled{0}, computes_failed{0};
};
// the made-up driver: in_cache, whether its caches hold the pipeline; broken, whether it refuses to compile it
static bool in_cache = false, broken = false;
static int asks = 0, compiles = 0, destroys = 0;
static void pin_subgroup(decltype(VkComputePipelineCreateInfo::stage)&, unsigned) {}
static VkResult cached_compute(const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    CHECK(ci->stage.module == 0x2468 && ci->layout == 0x1357);
    if (ci->flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) {
        asks++;
        if (!in_cache) return *pl = VK_NULL_HANDLE, VK_PIPELINE_COMPILE_REQUIRED;
        return *pl = 0xcafe, VK_SUCCESS;
    }
    compiles++;
    if (broken) return *pl = VK_NULL_HANDLE, -4;
    return *pl = 0xcafe, VK_SUCCESS;
}
static void vkDestroyPipeline(uintptr_t, VkPipeline made, const void*) { CHECK(made == 0xcafe); destroys++; }
"""

ARM_MAIN = r"""
static void run(bool cache_control, bool cached, bool refuses, bool abandoned, int want_asks, int want_compiles, size_t c, size_t b, size_t f,
                VkPipeline kept) {
    Program program;
    Pre pre;
    pre.abandoned = abandoned;
    dev.vk13.pipelineCreationCacheControl = cache_control, in_cache = cached, broken = refuses;
    asks = compiles = destroys = 0;
    actual_arm(program, &pre);
    CHECK(asks == want_asks && compiles == want_compiles);
    CHECK(pre.computes_cached == c && pre.computes_compiled == b && pre.computes_failed == f);
    CHECK(program.compute_pipeline == kept && destroys == (abandoned && !refuses ? 1 : 0));
}
int main() {
    run(true, true, false, false, 1, 0, 1, 0, 0, 0xcafe);     // the caches hold it: asked for, not compiled
    run(true, false, false, false, 1, 1, 0, 1, 0, 0xcafe);    // they do not: compiled now
    run(true, false, true, false, 1, 1, 0, 0, 1, 0);          // and the driver refuses it: not kept, counted
    run(false, true, false, false, 0, 1, 0, 1, 0, 0xcafe);    // a driver that cannot say: one call, as before
    run(false, false, true, false, 0, 1, 0, 0, 1, 0);
    run(true, true, false, true, 1, 0, 1, 0, 0, 0xbeef);      // the game went on meanwhile: let go, the program's own kept
    std::printf("PASS: the precompile's compute arm asks the caches first and counts cached, compiled and not made\n");
    return 0;
}
"""


def check_arm(draw, cxx):
    arm = block(block(draw, "void precompile_run("), "if (i < computes.size()) {")
    # (what the arm makes a compute pipeline from and how it asks the caches: the functions every maker of one goes through)
    helpers = (block(draw, "VkComputePipelineCreateInfo compute_pipeline_info(const Program& q, bool noopt) {") + "\n" +
               block(draw, "VkPipeline compute_pipeline_cached(const Program& q, bool noopt) {") + "\n")
    fixture = (ARM_HEAD + helpers + "\nstatic void actual_arm(Program& program, Pre* pre) {\n    std::vector<Program*> computes{&program};\n"
               "    size_t i = 0;\n    " + arm + "\n}\n" + ARM_MAIN)
    with tempfile.TemporaryDirectory(prefix="state-computes-arm-") as tmp:
        source, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-Werror", str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="clang++")
    args = ap.parse_args()
    tool = load("pipeline_states", TOOL)
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    check_tool(tool, draw)
    check_sent(tool)
    check_source(draw)
    check_game(tool, draw, args.cxx)
    check_arm(draw, args.cxx)
    print("pipeline state list: compute records in the tool, the sources, the shipped list and the game's readers ok")


if __name__ == "__main__":
    main()
