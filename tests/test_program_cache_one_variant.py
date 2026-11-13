"""A compute program's first dispatch reads its variants without their code, and the code of the one it runs.

The production cache loader (vk_progcache.cpp: program_cache_save, program_cache_read, program_cache_read_light,
program_cache_complete, program_cache_load), cut out of the source and compiled with a made-up Vulkan and a counted fread
(tests/program_cache_one_variant_test.cpp), run both ways: RX_VK_LOAD_ALL_VARIANTS=1 (every kept variant read whole and
made, as before) and the default.

- the programs the load gives are the same either way, in the same order, but for their modules;
- for several bound textures the pick's loop takes the same variant either way (the first that fits, in the file's order);
- by default the load makes no module and reads no SPIR-V, and each variant taken gets its own code read and one module
  made, once; a record that cannot be read that way (a head longer than the first read, two stages) is read whole;
- a key that is not a compute program's and a borrowed one are read whole, as before;
- code that no longer reads when the variant is taken fails the program, with no module;
- vk_draw.cpp's dispatch completes a program before the two places that use its module.
Synthetic data only. python -I -B tests/test_program_cache_one_variant.py [--cxx <compiler>]
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="clang++")
    args = ap.parse_args()
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    cache_h = (ROOT / "gpu/vk/vk_progcache.h").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_shader.h").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    structs = "\n".join(block(header, m) + ";" for m in ("struct Stage {", "struct Program {"))
    structs += "\n" + block(cache_h, "struct DiskRecord {") + ";"
    functions = "\n".join(block(shader, m) for m in ("VkShaderModule make_module(", "uint64_t answers_hash(", "bool answers_fit("))
    functions += "\n" + block(cache, "void idx_put(")
    functions += "\n" + block(cache_h, "struct Reader {") + ";\n"
    functions += cache[cache.index("void put_bytes("):cache.index("void program_cache_save(")]
    functions += block(cache, "void program_cache_save(") + "\n" + block(cache, "bool program_cache_payload(") + "\n"
    functions += block(cache, "void program_cache_read(") + "\n"
    functions += cache[cache.index("namespace {\nstd::mutex light_mu;"):cache.index("/// (shader_mu held) the kept variants of key go to its list")]
    functions += block(cache, "void program_cache_load(").replace("bool keep) {", "bool keep = false) {")
    # the dispatch: a program is made whole before its module is used, in both places
    d = draw[draw.index("void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {"):]
    d = d[:d.index("\n}\n")]
    first = "const bool fresh = p.compute_state.compare_exchange_strong(none, 1) &&\n" \
            "                           (program_cache_complete(p) || (p.compute_state.store(2, std::memory_order_release), false));"
    # (the module is read where the pipeline's create info is made: compute_pipeline_info, and compute_pipeline_cached through it)
    uses = sorted(i for m in ("compute_pipeline_cached(p, true)", "compute_pipeline_info(p, true)")
                  for i in range(len(d)) if d.startswith(m, i))
    info = draw[draw.index("VkComputePipelineCreateInfo compute_pipeline_info(const Program& q, bool noopt) {"):]
    if "ci.stage.module = q.stages[0].module;" not in info[:info.index("\n}\n")] or "stages[0].module" in d:
        raise SystemExit("a compute pipeline's module is not read in compute_pipeline_info alone")
    done = [i for i in range(len(d)) if d.startswith("program_cache_complete(p)", i)]
    if first not in d or len(uses) != 2 or len(done) != 2 or not (done[0] < uses[0] < done[1] < uses[1]):
        raise SystemExit("dispatch() uses a compute program's module before program_cache_complete made it")
    with tempfile.TemporaryDirectory(prefix="hookshot-one-variant-") as tmp:
        folder = Path(tmp).resolve()
        (folder / "cache_types.inc").write_text(structs, encoding="utf-8")
        (folder / "cache_functions.inc").write_text(functions, encoding="utf-8")
        exe = folder / "check.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(folder),
                        str(ROOT / "tests/program_cache_one_variant_test.cpp"), "-o", str(exe)], check=True, cwd=folder)
        out = {}
        for mode in ("whole", "light"):
            ran = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=60)
            if ran.returncode:
                raise SystemExit("%s: %s%s" % (mode, ran.stdout, ran.stderr))
            out[mode] = ran.stdout.splitlines()
        picks = {m: [l for l in out[m] if l.startswith("bound ")] for m in out}
        if picks["whole"] != picks["light"] or len(picks["whole"]) != 9:
            raise SystemExit("the variant taken differs:\nwhole: %s\nlight: %s" % (picks["whole"], picks["light"]))
        want = ["bound 4: variant 3 ", "bound 1: variant 0 ", "bound 11: variant 11 ", "bound 9: variant 9 ", "bound 4: variant 3 ",
                "bound 10: none", "bound 12: none", "bound 3: variant 2 ", "bound 1: variant 0 "]
        if not all(l.startswith(w) for l, w in zip(picks["light"], want)):
            raise SystemExit("the variants taken: %s" % picks["light"])
        print("program cache: %s (whole) / %s (light); the same variant taken for each of 9 picks" % (out["whole"][-1], out["light"][-1]))


if __name__ == "__main__":
    main()
