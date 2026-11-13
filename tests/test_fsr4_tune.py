"""AMD FSR 4's shader rewrites (gpu/vk/fsr4_tune.h, gpu/vk/spirv_unroll.h) on shaders built as FSR 4's are (DXC, the
modules in tests/fsr4_tune with their HLSL): a network layer in miniature, with a weight table copied in once and one
copied in at every iteration, a writable and a read-only buffer and counted loops three deep; and a table the shader
writes into. Built as C++ and run, each result checked with spirv-val:
- the layer's tables move to the tail of the writable buffer, after what the tail held, each read where it was put, the
  Function arrays gone; the table that is written stays
- every loop of the layer unrolls, and a size limit just above the layer's own keeps loops rolled within it
- the network's layers alone (..._passN) get 64 lanes; the prepass, the postpass and a pass's _post do not
- a binding takes the tail in only at offset 0 with the whole buffer (or a range past the tail), and one of exactly the
  library's size is widened to it
And in gpu/vk/vk_fsr4.cpp's hooks (read as text): the tail is kept per buffer and forgotten when the library destroys it,
its buffer is made a transfer destination, its bindings are checked where descriptors are written, a pipeline whose tail
cannot be read gets AMD's own in its place, and the background builds are counted and stopped when the window closes,
before the pipeline cache's save (gpu/vk/vk_main.cpp).
No game or GPU required.

python -I -B tests/test_fsr4_tune.py [--cxx g++] [--spirv-val spirv-val]
"""
import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--spirv-val", "{spirv_val}"]
RUN_ALL_SKIP = {"windows": "needs spirv-val: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
FIXTURES = ROOT / "tests" / "fsr4_tune"

W = [0x0A0B0C0D, 3, 0xFFFFFFF0, 77, 5, 0x12345678, 9, 1]   # (layer.hlsl's two tables)
V = [11, 22, 33, 44]
BEFORE = [111, 222]   # (what the tail held before: the tables go after it)
BASE = 1000           # (the word where the tail starts in the buffer)

DRIVER = r"""
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "fsr4_tune.h"
#include "spirv_unroll.h"

static std::vector<uint32_t> load(const char* p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint32_t> w(b.size() / 4);
    memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}
static void save(const std::string& p, const std::vector<uint32_t>& w) {
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)w.data(), w.size() * 4);
}
static int loops(const std::vector<uint32_t>& w) {
    int n = 0;
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) n += (w[i] & 0xffff) == 246;   // OpLoopMerge
    return n;
}
// tune <in.spv> <out prefix> <base word> <tail words...>: both rewrites, results to files, facts on stdout
// names <entry>...: which get 64 lanes, which are the postpass
// covers <size> <tail> <offset> <range>...: covers_tail and widened_range for each offset/range pair
int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "covers") {
        const uint64_t size = strtoull(argv[2], nullptr, 10), tail = strtoull(argv[3], nullptr, 10);
        for (int i = 4; i + 1 < argc; i += 2) {
            const uint64_t o = strtoull(argv[i], nullptr, 10), r = strtoull(argv[i + 1], nullptr, 10);
            std::printf("%llu %llu %d %llu\n", (unsigned long long)o, (unsigned long long)r, fsr4_tune::covers_tail(o, r, size, tail),
                        (unsigned long long)fsr4_tune::widened_range(o, r, size, tail));
        }
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "names") {
        for (int i = 2; i < argc; i++) std::printf("%s %d %d\n", argv[i], fsr4_tune::wants_wave64(argv[i]), fsr4_tune::is_postpass(argv[i]));
        return 0;
    }
    if (argc < 4) return 2;
    std::vector<uint32_t> code = load(argv[2]);
    const std::string pre = argv[3];
    std::printf("entry %s lane_dependent %d\n", fsr4_tune::entry_name(code.data(), code.size()).c_str(),
                fsr4_tune::lane_dependent(code.data(), code.size()));
    std::vector<uint32_t> out, tail;
    for (int i = 5; i < argc; i++) tail.push_back((uint32_t)strtoul(argv[i], nullptr, 10));
    const uint32_t base = (uint32_t)strtoul(argv[4], nullptr, 10) + (uint32_t)tail.size();
    const bool moved = fsr4_tune::arrays_to_buffer(code.data(), code.size(), base, out, tail);
    std::printf("moved %d", moved);
    for (uint32_t t : tail) std::printf(" %u", t);
    std::printf("\n");
    if (moved) save(pre + "_tail.spv", out);
    std::vector<uint32_t> un;
    int left = -1;
    const bool unrolled = spirv_unroll::unroll_all(code.data(), code.size(), un, code.size() * 20, 64, &left);
    std::printf("unrolled %d %d %d %zu\n", unrolled, loops(code), left, un.size());   // (done, loops before, left, words)
    if (unrolled) save(pre + "_unrolled.spv", un);
    std::vector<uint32_t> small;
    int left_small = -1;
    const size_t limit = code.size() + 64;
    const bool some = spirv_unroll::unroll_all(code.data(), code.size(), small, limit, 64, &left_small);
    std::printf("capped %d %d %zu %zu\n", some, left_small, some ? small.size() : code.size(), limit);
    return 0;
}
"""


def words(path):
    data = Path(path).read_bytes()
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


def instructions(w):
    i = 5
    while i < len(w):
        n = w[i] >> 16
        yield w[i] & 0xFFFF, w[i:i + n]
        i += n


def names_of(w):
    out = {}
    for op, ins in instructions(w):
        if op == 5:   # OpName
            out[ins[1]] = struct.pack("<%dI" % (len(ins) - 2), *ins[2:]).split(b"\0")[0].decode()
    return out


def uint_constants(w):
    """id -> value of every 32-bit unsigned OpConstant"""
    t_uint = {ins[1] for op, ins in instructions(w) if op == 21 and ins[2:4] == [32, 0]}
    return {ins[2]: ins[3] for op, ins in instructions(w) if op == 43 and len(ins) == 4 and ins[1] in t_uint}


def chains_into(w, var):
    return sum(1 for op, ins in instructions(w) if op == 65 and ins[3] == var)


def function_vars(w):
    return sum(1 for op, ins in instructions(w) if op == 59 and ins[3] == 7)


def facts(out):
    return {line.split()[0]: line.split()[1:] for line in out.splitlines()}


def function_body(text, signature):
    """The body of the function whose definition starts with `signature`, by its braces"""
    at = text.find(signature)
    if at < 0:
        return ""
    i = text.index("{", at)
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    return ""


def source_problems():
    """The hooks' safety in vk_fsr4.cpp and the close in vk_main.cpp, read as text"""
    problems = []
    src = (VK / "vk_fsr4.cpp").read_text(encoding="utf-8")
    proc = function_body(src, "VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(")
    for name, hook in [("vkDestroyBuffer", "destroy_buffer"), ("vkUpdateDescriptorSets", "update_sets"),
                       ("vkCmdPushDescriptorSetKHR", "push_set"), ("vkGetDescriptorEXT", "get_descriptor"),
                       ("vkGetBufferDeviceAddress", "buffer_address"), ("vkDestroyShaderModule", "destroy_shader_module")]:
        if '"%s"' % name not in proc or "&%s;" % hook not in proc:
            problems.append("device_proc does not hand the library %s for %s" % (hook, name))
    create = function_body(src, "VKAPI_ATTR VkResult VKAPI_CALL create_buffer(")
    if "VK_BUFFER_USAGE_TRANSFER_DST_BIT" not in create:
        problems.append("create_buffer does not make the grown buffer a transfer destination (vkCmdUpdateBuffer writes its tail)")
    if "forget_locked(*b)" not in create or "forget_locked(b)" not in function_body(src, "VKAPI_ATTR void VKAPI_CALL destroy_buffer("):
        problems.append("a destroyed buffer (or its handle given out again) is not forgotten with its tail")
    forget = function_body(src, "void forget_locked(")
    if "cur = 0" not in forget or "bigs.erase" not in forget:
        problems.append("forget_locked does not drop the buffer and stop new modules being rewritten against it")
    module = function_body(src, "VKAPI_ATTR VkResult VKAPI_CALL create_shader_module(")
    if "bigs.find(cur)" not in module or "mod.gen = cur" not in module:
        problems.append("create_shader_module does not rewrite against the newest live buffer and remember which")
    binding = function_body(src, "void note_binding_locked(")
    if "fsr4_tune::covers_tail" not in binding or "g.bound = -1" not in binding:
        problems.append("note_binding_locked does not check a binding with covers_tail and mark one without the tail")
    dispatch = function_body(src, "VKAPI_ATTR void VKAPI_CALL cmd_dispatch(")
    if "bound == 1" not in dispatch or "make_plain(" not in dispatch or "real_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, use)" not in dispatch:
        problems.append("cmd_dispatch does not write the tail only for a live, checked buffer, with AMD's own pipeline otherwise")
    if "real_update(cb, big.buffer" not in dispatch:
        problems.append("cmd_dispatch does not write the tail into the pipeline's own buffer")
    pipes = function_body(src, "VKAPI_ATTR VkResult VKAPI_CALL create_compute_pipelines(")
    detach = pipes.find(".detach()")
    if detach >= 0 and pipes.rfind("jobs().running++", 0, detach) < 0:
        problems.append("an unroll thread is started without being counted (fsr4_close waits for the count)")
    if "closing" not in pipes:
        problems.append("create_compute_pipelines starts unroll jobs after the window closed")
    job = function_body(src, "void build_unrolled(")
    if "j.running--" not in job or "if (closing) return;" not in job:
        problems.append("build_unrolled does not end its count or leave when the window closed")
    close = function_body(src, "void close()")
    if "closing.exchange(true)" not in close or "wait_for(" not in close:
        problems.append("tune::close does not stop the jobs and wait for the one building")
    if "void fsr4_close() { tune::close(); }" not in src or src.count("void fsr4_close()") != 2:
        problems.append("fsr4_close is not defined in both builds (with and without FSR)")
    main_cpp = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    closer = function_body(main_cpp, "void mkwvk_close_asked(void)")
    if "fsr4_close();" not in closer or closer.find("fsr4_close();") > closer.find("pipeline_cache_close();"):
        problems.append("mkwvk_close_asked does not stop FSR 4's builds before the pipeline cache's save")
    return problems


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--spirv-val", default="spirv-val")
    a = p.parse_args()
    problems = source_problems()
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        exe = t / "tune.exe"
        (t / "tune.cpp").write_text(DRIVER, encoding="utf-8")
        # (static on Windows: a MinGW compiler's C++ runtime is not on the PATH where the test runs)
        subprocess.run([a.cxx, "-std=c++17", "-O1", "-I", str(VK), str(t / "tune.cpp"), "-o", str(exe)] +
                       (["-static"] if os.name == "nt" else []), check=True)

        def run(*args):
            return subprocess.run([str(exe)] + [str(x) for x in args], capture_output=True, text=True, check=True).stdout

        def valid(path, what):
            r = subprocess.run([a.spirv_val, "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True)
            if r.returncode:
                problems.append("%s is not valid SPIR-V: %s" % (what, (r.stdout + r.stderr).strip()))

        layer = FIXTURES / "layer.spv"
        valid(layer, "tests/fsr4_tune/layer.spv")
        base = words(layer)
        f = facts(run("tune", layer, t / "layer", BASE, *BEFORE))
        if f["entry"] != ["fsr4_model_test_pass7", "lane_dependent", "0"]:
            problems.append("layer.spv's entry point or lane use read wrong: %s" % f["entry"])

        # --- the weights to the tail
        if f["moved"][0] != "1":
            problems.append("the layer's weight tables were not moved to the tail")
        else:
            tail = [int(x) for x in f["moved"][1:]]
            new = tail[len(BEFORE):]
            if tail[:len(BEFORE)] != BEFORE or sorted([new[:len(W)], new[len(W):]]) != sorted([W, V]) and \
                    sorted([new[:len(V)], new[len(V):]]) != sorted([V, W]):
                problems.append("the tail holds %s: not what it held, then the two tables whole" % tail)
            moved = words(t / "layer_tail.spv")
            valid(t / "layer_tail.spv", "the layer with its weights in the tail")
            # (each table read from where it was put: a new constant per table, its first word's place in the buffer)
            added = set(uint_constants(moved).values()) - set(uint_constants(base).values())
            where = {BASE + len(BEFORE) + i for i in range(len(new)) if new[i:i + len(W)] == W or new[i:i + len(V)] == V}
            if not where or not where <= added:
                problems.append("the tables' places in the buffer %s are not among the constants the rewrite added %s" %
                                (sorted(where), sorted(added)))
            if function_vars(moved) != function_vars(base) - 2:
                problems.append("the two tables' Function arrays are not gone (%d Function variables, %d before)" %
                                (function_vars(moved), function_vars(base)))
            names = names_of(base)
            rw = [v for v, n in names.items() if n == "ScratchBuffer"]
            ro = [v for v, n in names.items() if n == "InitializerBuffer"]
            if not rw or not ro:
                problems.append("layer.spv's buffers are not named as expected: %s" % names)
            elif chains_into(moved, rw[0]) <= chains_into(base, rw[0]) or chains_into(moved, ro[0]) != chains_into(base, ro[0]):
                problems.append("the weights are not read from the writable buffer alone (reads of it %d -> %d, of the "
                                "read-only one %d -> %d)" % (chains_into(base, rw[0]), chains_into(moved, rw[0]),
                                                            chains_into(base, ro[0]), chains_into(moved, ro[0])))

        # --- the loops
        done, before, left, size = f["unrolled"]
        if done != "1" or before != "3" or left != "0":
            problems.append("the layer's 3 loops were not all unrolled: done %s, loops before %s, left %s" % (done, before, left))
        else:
            valid(t / "layer_unrolled.spv", "the unrolled layer")
            if int(size) <= len(base):
                problems.append("the unrolled layer (%s words) is no larger than the original (%d)" % (size, len(base)))
        some, left_small, size_small, limit = f["capped"]
        if left_small == "0" or int(size_small) > int(limit):
            problems.append("a size limit of %s words: %s loops left, %s words (some loops must stay rolled, within the "
                            "limit)" % (limit, left_small, size_small))

        # --- a table the shader writes into stays
        written = FIXTURES / "written.spv"
        valid(written, "tests/fsr4_tune/written.spv")
        if facts(run("tune", written, t / "written", BASE))["moved"][0] != "0":
            problems.append("a table the shader writes into was moved to the tail (it must stay)")

        # --- which entry points get 64 lanes
        want = {"fsr4_model_v07_fp8_no_scale_pass12": (1, 0), "fsr4_model_v07_fp8_no_scale_pass1_post": (0, 0),
                "fsr4_model_v07_fp8_no_scale_postpass": (0, 1), "fsr4_model_v07_fp8_no_scale_prepass": (0, 0),
                "fsr4_model_v07_fp8_no_scale_pass": (0, 0), "main": (0, 0)}
        for line in run("names", *want).splitlines():
            entry, w64, post = line.split()
            if (int(w64), int(post)) != want[entry]:
                problems.append("%s: 64 lanes %s, postpass %s; wanted %d, %d" % (entry, w64, post, *want[entry]))

        # --- which bindings take the tail in: (offset, range) -> (covers, widened range)
        size, tail, whole = 100 << 20, 64 << 10, (1 << 64) - 1
        cases = {(0, whole): (1, whole), (0, size + tail): (1, size + tail), (0, size + 2 * tail): (1, size + 2 * tail),
                 (0, size): (0, size + tail), (0, size - 4): (0, size - 4), (256, whole): (0, whole),
                 (256, size): (0, size), (0, size + tail - 4): (0, size + tail - 4)}
        args = [x for pair in cases for x in pair]
        for line in run("covers", size, tail, *args).splitlines():
            o, r, covers, widened = (int(x) for x in line.split())
            if (covers, widened) != cases[(o, r)]:
                problems.append("a binding at offset %d, range %d: covers the tail %d, widened to %d; wanted %d, %d" %
                                (o, r, covers, widened, *cases[(o, r)]))
            elif covers == 0 and o == 0 and widened != r:
                # (what is widened must then cover it)
                if not run("covers", size, tail, o, widened).split()[2] == "1":
                    problems.append("the widened range %d at offset 0 does not cover the tail" % widened)
    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    print("PASS: FSR 4's weight tables move to the writable buffer's tail and are read where they were put, a written "
          "table stays, every loop unrolls valid within a size limit, the layers alone get 64 lanes; bindings are checked "
          "for the tail, the tail goes with its buffer, the background builds stop at the close")


if __name__ == "__main__":
    main()
