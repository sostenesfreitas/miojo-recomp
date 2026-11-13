"""The prediction leaves out the pipelines no draw can ask for, builds each predicted pipeline once, and finds every program it predicts.

predict_pipelines (gpu/vk/vk_predict.cpp) gives each kept program variant no note names the vertex layout and targets of notes. A
program's key holds the types of the vertex inputs it reads and of the colour outputs it writes (graphics_key, written_types), and
the seed makes each skinned program three times, for float and for unsigned integer bone inputs (seed_program). The three took the
one layout their signature's notes had most, and a draw with that layout has one of the three keys: the other two pipelines could
never be looked up. Each is a program of its own modules, a full driver compile. Now a candidate whose types differ from what the
module declares (an integer where it has a float, a float where it has an integer, the other signedness) is left out; scaled types
count as integers of their signedness (make_profile turns support_scaled_attributes off on every GPU). The signature fallback picks the most common class that fits (candidates), so the
other variants get a layout of their own types rather than none.

Checked here: Locations, spirv_locations and types_fit, extracted unmodified, on hand-made SPIR-V; and in the source, that the
prediction filters both its vertex and output candidates with them, that predict_run builds each pipeline_key once, and that a
borrowed program's records include those read in already (disk_taken): such programs' predicted pipelines were never built (67,106
predicted, 65,449 made).

No Vulkan, GPU or game. python -I -B tests/test_predict_types.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

def _with_cache(path):
    """vk_shader.cpp with the program cache it held before (vk_progcache.cpp and .h beside it, outside the translator stamp): definitions first"""
    parts = [path.read_text(encoding="utf-8")]
    for name in ("vk_progcache.cpp", "vk_progcache.h"):
        if (path.parent / name).exists():
            parts.append((path.parent / name).read_text(encoding="utf-8"))
    return "\n".join(parts)



def body(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("FAIL: expected one %r" % marker)
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    if text[at:at + 1] == ";":
        at += 1
    return text[start:at]


CHECK = r'''
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
namespace Shader {
enum class AttributeType : uint8_t { Float, SignedInt, UnsignedInt, SignedScaled, UnsignedScaled, Disabled };
}
#include "predict_types.inc"
#define CHECK(c) do { if (!(c)) { std::printf("CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static uint32_t op(uint32_t code, uint32_t n) { return n << 16 | code; }
int main() {
    std::vector<uint32_t> w = {0x07230203, 0x10000, 0, 30, 0};
    const auto add = [&](std::initializer_list<uint32_t> words) { w.insert(w.end(), words); };
    // (decorations first, as in a module: location 0 float, 6 unsigned, 7 signed inputs; output 0 unsigned, output 1 float)
    add({op(71, 4), 20, 30, 0}); add({op(71, 4), 21, 30, 6}); add({op(71, 4), 22, 30, 7});
    add({op(71, 4), 23, 30, 0}); add({op(71, 4), 24, 30, 1}); add({op(71, 4), 25, 30, 3});
    add({op(22, 3), 1, 32});            // float
    add({op(21, 4), 2, 32, 0});         // uint
    add({op(21, 4), 3, 32, 1});         // int
    add({op(23, 4), 4, 1, 4}); add({op(23, 4), 5, 2, 4}); add({op(23, 4), 6, 3, 4});
    add({op(43, 4), 2, 11, 2});         // (a constant: not a type)
    add({op(28, 4), 12, 4, 11});        // float vec4[2]
    add({op(32, 4), 7, 1, 4}); add({op(32, 4), 8, 1, 5}); add({op(32, 4), 9, 1, 6});
    add({op(32, 4), 10, 3, 5}); add({op(32, 4), 13, 3, 4}); add({op(32, 4), 14, 3, 12});
    add({op(59, 4), 7, 20, 1}); add({op(59, 4), 8, 21, 1}); add({op(59, 4), 9, 22, 1});
    add({op(59, 4), 10, 23, 3}); add({op(59, 4), 13, 24, 3}); add({op(59, 4), 14, 25, 3});
    add({op(59, 4), 7, 26, 1});         // (an input without a location: a built-in)
    const Locations in = spirv_locations(w, 1), out = spirv_locations(w, 3);
    CHECK(in.mask == (1u | 1u << 6 | 1u << 7));
    CHECK(in.ints == (1u << 6 | 1u << 7));
    CHECK(in.sign == 1u << 7);
    CHECK(out.mask == (1u | 2u | 8u));
    CHECK(out.ints == 1u && out.sign == 0);
    using T = Shader::AttributeType;
    const auto types = [](std::initializer_list<std::pair<int, T>> at) {
        std::vector<uint8_t> t(16, (uint8_t)T::Float);
        for (const auto& [i, type] : at) t[i] = (uint8_t)type;
        return t;
    };
    std::vector<uint8_t> t = types({{6, T::UnsignedInt}, {7, T::SignedInt}});
    CHECK(types_fit(in, in.mask, t.data(), 16));
    t = types({{7, T::SignedInt}});                                   // (float bone indices, the module reads unsigned ones)
    CHECK(!types_fit(in, in.mask, t.data(), 16));
    t = types({{6, T::UnsignedInt}, {7, T::UnsignedInt}});            // (the other signedness)
    CHECK(!types_fit(in, in.mask, t.data(), 16));
    t = types({{0, T::UnsignedInt}, {6, T::UnsignedInt}, {7, T::SignedInt}});   // (an integer where it reads a float)
    CHECK(!types_fit(in, in.mask, t.data(), 16));
    // (scaled: integers of their signedness, support_scaled_attributes being off on every GPU)
    t = types({{6, T::UnsignedScaled}, {7, T::SignedScaled}});
    CHECK(types_fit(in, in.mask, t.data(), 16));
    t = types({{0, T::SignedScaled}, {6, T::UnsignedInt}, {7, T::SignedInt}});   // (scaled where it reads a float)
    CHECK(!types_fit(in, in.mask, t.data(), 16));
    t = types({{6, T::SignedScaled}, {7, T::SignedInt}});                         // (scaled of the other signedness)
    CHECK(!types_fit(in, in.mask, t.data(), 16));
    t = types({{3, T::UnsignedInt}, {6, T::UnsignedInt}, {7, T::SignedInt}});   // (a location it does not read)
    CHECK(types_fit(in, in.mask, t.data(), 16));
    uint8_t o[8]{(uint8_t)T::UnsignedInt};
    CHECK(types_fit(out, out.mask, o, 8));
    o[0] = (uint8_t)T::Float;                                          // (a float target for an integer output)
    CHECK(!types_fit(out, out.mask, o, 8));
    o[0] = (uint8_t)T::UnsignedInt, o[1] = (uint8_t)T::SignedInt;      // (an integer target for a float output)
    CHECK(!types_fit(out, out.mask, o, 8));
    std::vector<uint32_t> cut(w.begin(), w.begin() + 12);             // (a cut module: what was read)
    cut.push_back(op(59, 9));
    (void)spirv_locations(cut, 1);
    // (the signature fallback: the most common class that fits, not the most common then filtered to nothing)
    Seen sig, exact;
    const Class common{1, 6, 2}, rare{1, 6, 1}, other{3};
    for (uint64_t k = 0; k < 3; k++) sig.add(common, {k, 0});
    sig.add(rare, {9, 0});
    const auto only = [](const Class& want) { return [want](const Class& c) { return c == want; }; };
    const auto all = [](const Class&) { return true; };
    const auto none = [](const Class&) { return false; };
    CHECK(candidates(nullptr, &sig, all) == std::vector<Class>{common});
    CHECK(candidates(nullptr, &sig, only(rare)) == std::vector<Class>{rare});
    CHECK(candidates(nullptr, &sig, none).empty());
    CHECK(candidates(nullptr, nullptr, all).empty());
    exact.add(other, {5, 0}), exact.add(common, {5, 1});
    CHECK(candidates(&exact, &sig, none) == (std::vector<Class>{other, common}));   // (an exact module's: all, filtered after)
    std::printf("PASS: the types of a module's locations and the candidates they fit\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++") or "g++")
    a = p.parse_args()
    predict = (ROOT / "gpu/vk/vk_predict.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    shader = _with_cache(ROOT / "gpu/vk/vk_shader.cpp")
    pieces = [body(predict, "struct Locations {"), body(predict, "Locations spirv_locations("), body(predict, "bool types_fit(")]
    # (the signature fallback's pick, with what it needs: Class, its hashes, Seen)
    pieces += [body(predict, "uint64_t fnv("), "using Class = std::vector<uint32_t>;", body(predict, "struct ClassHash {"),
               body(predict, "struct PairHash {"), body(predict, "struct Seen {"), body(predict, "template <class Fits>")]

    decide = body(predict, "std::vector<std::unique_ptr<PipeNote>> predict_pipelines(")
    for need in ("types_fit(f.vs_in, f.vs_locs", "types_fit(f.fs_out, f.fs_locs", "vertex_fits(f, *vdonor.at(v))",
                 "output_fits(f, *odonor.at(o))", "counts.notes_unfit +=", "candidates(vx, vs, vfit)", "candidates(fx, fs, ofit)", "(vc0.size() - std::min(vc0.size(), vc.size())) * oc0.size()",
                 "attribute_type_of(", "note_color_types("):
        if need not in decide:
            raise SystemExit("FAIL: predict_pipelines does not filter its candidates by their types (%s)" % need)
    run = body(draw, "void predict_run(")
    if "built.insert(mix(pipeline_key(" not in run or run.index("std::unordered_set<uint64_t> built;") > run.index("program_cache_borrow("):
        raise SystemExit("FAIL: predict_run's duplicate check is per borrowed record, not per program key")
    if "built.insert(mix(pipeline_key(" not in run:
        raise SystemExit("FAIL: predict_run builds predictions that are the same pipeline (pipeline_key) again")
    if "void note_color_types(const PipeNote& n, uint8_t types[8])" not in draw or "color_types_of(note_targets(n), types)" not in draw:
        raise SystemExit("FAIL: note_color_types is not the draw's color_types_of")
    load = body(shader, "void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool keep) {")
    keep = load[load.index("if (keep)"):]
    if "disk_taken.find(key)" not in keep[:keep.index("} else {")]:
        raise SystemExit("FAIL: a borrowed program leaves out the records read in already: their predictions are never built")

    with tempfile.TemporaryDirectory(prefix="hookshot-predict-types-") as temp:
        temp = Path(temp)
        (temp / "predict_types.inc").write_text("\n".join(pieces) + "\n", encoding="utf-8")
        (temp / "check.cpp").write_text(CHECK, encoding="utf-8")
        exe = temp / "check.exe"
        cmd = [a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(temp), str(temp / "check.cpp"), "-o", str(exe)]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode:
            print(result.stdout + result.stderr)
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            return 1
    print("PASS: the prediction builds only pipelines a draw can ask for, each once, and finds the programs read in already")
    return 0


if __name__ == "__main__":
    sys.exit(main())
