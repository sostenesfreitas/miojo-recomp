"""Local bake-everything skeleton: ROM seed in, batch combo list + bank out.

End-to-end CPU-only pass (no Vulkan device, no game files needed for the
skeleton itself):

    python3 nx2/bake.py bake --seed <hookshot_shaders.bin> --bank <dir>
        [--limit N] [--resume] [--max-pipelines K]

Stages:
  1. enumerate  parse HKSEED01 (nx2/shader_census.py format) or run the census
                wrapper (needs the player's romfs + zstandard; not run here).
  2. expand     per-program combos: input variants x3 (as prepare_run's
                programs*3, gpu/vk/vk_draw.cpp:5441) + plausible pipeline
                states (tight cap, default 8/program).
  3. translate  (needs a C++ compiler + SPIR-V headers; still no GPU) build
                gpu/shaderbatch.cpp (adopted from bake-mimo c60fd1d, MiMo)
                and run it over the seed: exact runtime program keys +
                SPIR-V modules. Merges into the manifest (translation: OK/
                LOWER_ERROR/...; pipelines stay PENDING_GPU).
  4. emit       deduped bank manifest (JSON, atomic rename) with resume cursor.
  5. verify      read-back verify of the manifest (adapts bake_ds's verify
                concept to the portable format): seed sha, stamp currency,
                SPIR-V magic + spirv-val WITH --uniform-buffer-standard-layout
                (the shipping device's layout) over every listed module, plus
                seed coverage (partial banks are expected).

What this does NOT do (exact stop-line, needs a Vulkan GPU on the target rig):
  - VkShaderModule creation (make_module needs dev.device, :542-549).
  - Driver pipeline builds (build_graphics, gpu/vk/vk_draw.cpp:1046).
  Every emitted combo keeps pipelines PENDING_GPU; translation is now real
  (step 3) instead of assumed. prepare_run/precompile_run on a Vulkan machine
  consume the same seed + note inputs this produces.

Keying mirrors the C++ exactly where the inputs are available in Python:
  - code_key      FNV-1a over control(0xd8) then code area (vk_shader.cpp:99-137)
  - mix/answers   vk_draw.cpp:132-136, vk_shader.cpp:642-647 (exact)
  - program key   APPROXIMATE: exact graphics_key (vk_shader.cpp:733-742) needs
                  the SPH used-generic mask (Eden struct); the skeleton assumes
                  generics 0-3 used (+6/+7 for bone variants) and flags it.
  - pipeline key  same mix chain as graphics_pipeline (:978-1001) over the
                  canonical state (assumes eds3 dynamic state on; noted).

Repo only; never reads the game folder, saves, or caches. Outputs go to --bank.
Synthetic-data test: tests/test_bake_skeleton.py.
"""

import argparse
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CONTROL_SIZE = 0xD8
SEED_MAGIC = b"HKSEED01"

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
M64 = (1 << 64) - 1


def fnv1a(data: bytes, h: int = FNV_OFFSET) -> int:
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def mix(h: int, v: int) -> int:
    h ^= (v + 0x9E3779B97F4A7C15 + ((h << 6) & M64) + (h >> 2)) & M64
    return (h * 0xFF51AFD7ED558CCD) & M64


def answers_hash(answers) -> int:
    """Exact mirror of answers_hash (vk_shader.cpp:642-647)."""
    h = 0
    for stage, desc, typ, image in answers:
        h = ((h ^ (((stage & 0xFFFF) << 32) | ((desc & 0xFFFF) << 16)
                   | ((typ & 0xFF) << 1) | (image & 1))) * 1099511628211) & M64
    return h


def code_key(control: bytes, area: bytes) -> int:
    """Exact mirror of code_key (vk_shader.cpp:135-137)."""
    return fnv1a(bytes(area), fnv1a(bytes(control[:CONTROL_SIZE])))


def translator_stamp(vk_dir: str = None) -> str:
    """Mirror of the HOOKSHOT_SHADER_STAMP block (nx2/build.py:693-699)."""
    vk = vk_dir or os.path.join(ROOT, "gpu", "vk")
    stamp = hashlib.sha1(usedforsecurity=False)
    translator = [os.path.join(d, f) for top in ("eden", "sirit", "compat")
                  for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
    names = sorted(translator) + [os.path.join(vk, n) for n in
                                  ("vk_shader.cpp", "vk_bank.cpp",
                                   "vk_state.h", "vk_api.h")]
    for f in names:
        with open(f, "rb") as src:
            stamp.update(os.path.relpath(f, vk).replace("\\", "/").encode()
                         + src.read().replace(bytes((13, 10)), bytes((10,))))
    return stamp.hexdigest()[:16]


def parse_seed(path: str):
    """Parse HKSEED01 into (stages, programs). stages: list of
    (key, stage, control, code); programs: list of stage-index lists."""
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:8] != SEED_MAGIC:
        raise ValueError("not a HKSEED01 seed: %r" % path)
    nprograms, nstages = struct.unpack_from("<II", blob, 8)
    at = 16
    stages = []
    for _ in range(nstages):
        key, stage, csize, cbytes = struct.unpack_from("<QIII", blob, at)
        at += 20
        control, code = blob[at:at + csize], blob[at + csize:at + csize + cbytes]
        at += csize + cbytes
        stages.append((key, stage, control, code))
    programs = []
    for _ in range(nprograms):
        (n,) = struct.unpack_from("<I", blob, at)
        ids = list(struct.unpack_from("<%dI" % n, blob, at + 4))
        at += 4 + 4 * n
        programs.append(ids)
    return stages, programs


def program_kind(stages, ids) -> str:
    kinds = {stages[i][1] & 7 for i in ids}
    if 5 in kinds:
        return "compute"
    if 6 in kinds or 7 in kinds:
        return "mesh"
    return "graphics"


def program_key_skeleton(stages, ids, inputs: int = 0) -> int:
    """Deterministic program key. APPROXIMATE vs graphics_key/mesh_key
    (vk_shader.cpp:733-752): the exact key folds the SPH used-generic mask
    and written output types; without Eden headers the skeleton assumes
    generics 0-3 used (+6/+7 for bone input variants 1/2, mirroring
    seed_program's inputs param, vk_shader.cpp:1893-1916) and float outputs."""
    keys = sorted(code_key(stages[i][2], stages[i][3]) for i in ids)
    h = FNV_OFFSET
    for k in keys:
        h = ((h ^ (k & 0xFFFFFFFF)) * FNV_PRIME) & M64
        h = ((h ^ (k >> 32)) * FNV_PRIME) & M64
    h = ((h ^ (inputs & 0xFF)) * FNV_PRIME) & M64
    return h


TOPO_CLASSES = (2,)  # triangles dominate; 0/1/3 are fuzz residue
SAMPLES = (1, 4)
OUTPUT_TYPES = (0,)  # float; integer-target variants are fuzz residue


def pipeline_key_skeleton(prog_key: int, variant: int, topo_class: int,
                          samples: int, formats, depth: int) -> int:
    """Same mix chain shape as graphics_pipeline (vk_draw.cpp:978-1001),
    over the canonical state. Assumes eds3 dynamic state on (blend/poly/
    clamp/a2c dynamic, so not in the key)."""
    key = mix(mix(prog_key, variant), ((topo_class & 0xFF) << 8) | (samples & 0xFF))
    for fmt in formats:
        key = mix(key, fmt & 0xFFFFFFFF)
    key = mix(key, depth & 0xFFFFFFFF)
    return key


def expand_program(stages, ids, max_pipelines: int = 8):
    """Yield combo dicts for one seed program. Graphics: 3 input variants x
    pipeline states (capped). Compute/mesh: baseline record(s), texture
    questions unknown without translation -> PENDING_GPU."""
    kind = program_kind(stages, ids)
    if kind == "compute":
        # seed_compute (vk_shader.cpp:1966-2082) enumerates texture answers by
        # trial translation; without a device we emit the baseline only.
        pkey = program_key_skeleton(stages, ids)
        yield {"program": "%016x" % pkey, "kind": kind, "inputs": 0,
               "variant": "%016x" % 0, "pipeline": None,
               "status": "PENDING_GPU",
               "note": "compute texture-variant enumeration needs translation"}
        return
    if kind == "mesh":
        pkey = program_key_skeleton(stages, ids)
        yield {"program": "%016x" % pkey, "kind": kind, "inputs": 0,
               "variant": "%016x" % 0, "pipeline": None,
               "status": "PENDING_GPU",
               "note": "mesh pipeline build needs a Vulkan device"}
        return
    n = 0
    for inputs in (0, 1, 2):
        # (as prepare_run's programs*3: each input set is its own translate
        # with its own program key, seed_program(i/3, i%3) -> graphics_key
        # over different vattr types)
        pkey = program_key_skeleton(stages, ids, inputs)
        variant = answers_hash([])  # plain variant: nothing bound at bake
        for topo in TOPO_CLASSES:
            for samples in SAMPLES:
                for otype in OUTPUT_TYPES:
                    if n >= max_pipelines:
                        return
                    formats = [1 if otype == 0 else 2]  # canonical 1-target
                    pk = pipeline_key_skeleton(pkey, variant, topo, samples,
                                               formats, 0)
                    yield {"program": "%016x" % pkey, "kind": kind,
                           "inputs": inputs, "variant": "%016x" % variant,
                           "pipeline": "%016x" % pk,
                           "topo_class": topo, "samples": samples,
                           "status": "PENDING_GPU",
                           "note": "pipeline build needs a Vulkan device"}
                    n += 1


def bake_seed(seed_path: str, bank_dir: str, max_pipelines: int = 8,
              limit: int = None, resume: bool = False, progress=None):
    """Full skeleton pass. Returns (manifest_path, stats). Idempotent under
    --resume (done program keys are skipped via the manifest cursor)."""
    os.makedirs(bank_dir, exist_ok=True)
    stages, programs = parse_seed(seed_path)
    with open(seed_path, "rb") as f:
        seed_sha = hashlib.sha1(f.read(), usedforsecurity=False).hexdigest()
    stamp = translator_stamp()
    manifest_path = os.path.join(bank_dir, "bake_bank.json")
    done = set()
    old_combos = []
    if resume and os.path.isfile(manifest_path):
        with open(manifest_path, "r", encoding="utf-8") as f:
            old = json.load(f)
        if old.get("seed_sha") == seed_sha and old.get("stamp") == stamp:
            done = set(old.get("done_programs", []))
            old_combos = old.get("combos", [])
    combos = []
    seen_pipelines = set()
    ordered = programs[:limit] if limit else programs
    for idx, ids in enumerate(ordered):
        pkey = "%016x" % program_key_skeleton(stages, ids)
        if pkey in done:
            continue
        for c in expand_program(stages, ids, max_pipelines):
            pipe = c.get("pipeline")
            if pipe and pipe in seen_pipelines:
                continue
            if pipe:
                seen_pipelines.add(pipe)
            combos.append(c)
        done.add(pkey)
        if progress and idx % 25 == 0:
            progress(idx + 1, len(ordered))
    manifest = {"stamp": stamp, "seed_sha": seed_sha,
                "seed_path_hint": os.path.abspath(seed_path),
                "seed_programs": len(programs), "seed_stages": len(stages),
                "combos": old_combos + combos, "done_programs": sorted(done),
                "status": "PENDING_GPU",
                "stop_line": "SPIR-V translation + pipeline builds need a "
                             "Vulkan device (make_module/build_graphics); "
                             "rerun prepare_run/precompile_run on target GPU."}
    tmp = manifest_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    os.replace(tmp, manifest_path)
    if progress:
        progress(len(ordered), len(ordered))
    stats = {"programs": len(ordered), "stages": len(stages),
             "combos": len(combos), "bank": manifest_path}
    return manifest_path, stats


def translate_seed(seed_path: str, bank_dir: str, tool_dir: str = None,
                   limit: int = 0, jobs: int = 0):
    """Adopted-driver translate (nx2/bake_mimo.py, bake-mimo c60fd1d, MiMo):
    build gpu/shaderbatch.cpp against the vendored recompiler (no Vulkan
    headers, no device) and run it over the seed. Merges the exact runtime
    keys + SPIR-V statuses into bake_bank.json (pipelines stay PENDING_GPU).
    Returns (translate_dir, rows). Raises SystemExit like the driver when the
    toolchain is missing (callers may downgrade to a skip)."""
    sys.path.insert(0, HERE)
    import bake_mimo  # noqa: E402  (adopted; kept verbatim, do not edit)
    if not os.environ.get("HOOKSHOT_SPIRV_HEADERS"):
        # The in-tree headers live at <repo>/buildext/cpm (symlinked CPM
        # cache); nx2/bake_mimo.spirv_headers probes it, but default the env
        # too so direct shaderbatch builds find it. Explicit env still wins.
        in_tree = os.path.join(ROOT, "buildext", "cpm", "spirv-headers")
        if os.path.isdir(in_tree):
            vers = sorted(os.listdir(in_tree))
            inc = os.path.join(in_tree, vers[-1], "include") if vers else None
            if inc and os.path.isfile(
                    os.path.join(inc, "spirv", "unified1", "spirv.hpp11")):
                os.environ["HOOKSHOT_SPIRV_HEADERS"] = inc
    tool_dir = tool_dir or os.path.join(bank_dir, "tool")
    os.makedirs(tool_dir, exist_ok=True)
    tr = bake_mimo.run_tool(tool_dir, seed=seed_path, limit=limit, jobs=jobs)
    # merge programs.tsv rows into the manifest by (seed index, inputs)
    rows = {}
    tsv = os.path.join(tr, "programs.tsv")
    if os.path.isfile(tsv):
        with open(tsv, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                idx, inputs, key, kind, _, _, status, spirv, detail = (
                    line.split("\t") + [""] * 9)[:9]
                rows[(int(idx), int(inputs))] = {
                    "runtime_key": key, "kind": kind, "status": status,
                    "spv": [s for s in spirv.split(",") if s],
                    "detail": detail, "translate_dir": tr}
    manifest_path = os.path.join(bank_dir, "bake_bank.json")
    if os.path.isfile(manifest_path):
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
        manifest["translation"] = {
            "tool": "gpu/shaderbatch.cpp (bake-mimo c60fd1d)",
            "rows": len(rows),
            "translated": sum(1 for r in rows.values()
                              if r["status"] == "OK"),
            # Offline emit assumptions (bake_ds §6.2 / shaderbatch SeedEnv):
            # device limits and game state this pass has no access to.
            "assumptions": {
                "min_ssbo_alignment": 16,
                "convert_depth_mode": False,
                "color_output_types": "all Float (seed no_types)",
                "driver_banks_c0_c2": "zero (c1 exact)",
                "texture_answers": "Color2D/not-integer (unbound)",
            },
            "by_program": {"%d/%d" % k: v for k, v in sorted(rows.items())}}
        tmp = manifest_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=1)
        os.replace(tmp, manifest_path)
    return tr, rows


def verify_bank(bank_dir: str, strict: bool = False):
    """Read-back verify of bake_bank.json (adapting bake_ds's verify concept,
    nx2/bake_ds via bake-deepseek: the game's Reader over a bank). Checks the
    portable manifest instead of the live format: seed sha, stamp currency,
    translation rows' SPIR-V files (magic + spirv-val when present), and
    coverage of the seed (partial banks are expected: LOWER_ERROR programs
    never enter the bank and translate live at first meet).

    SPIR-V validity is judged WITH --uniform-buffer-standard-layout: the
    shipping build creates its device with every Vulkan 1.2 feature the GPU
    reports (gpu/vk/vk_main.cpp:253,256,360 — f12 chained into
    VkDeviceCreateInfo, never cleared), which includes
    uniformBufferStandardLayout, and CI validates the back end's own modules
    the same way (.github/workflows/source-regressions.yml:147-148). Strict
    (no-flag) validity is NOT the gate; --strict additionally records the
    strict count for information only. Returns (summary, ok). Writes
    verify.json next to the manifest."""
    import shutil
    import subprocess
    manifest_path = os.path.join(bank_dir, "bake_bank.json")
    summary = {"manifest": os.path.isfile(manifest_path), "seed_ok": False,
               "stamp_current": False, "translation_rows": 0,
               "coverage_programs": 0, "seed_programs": 0,
               "coverage_fraction": 1.0, "missing_programs": 0,
               "spv_files": 0, "spv_bad_magic": 0, "spirv_val": None,
               "spirv_layout": "uniformBufferStandardLayout",
               "spirv_invalid": 0, "spirv_strict_invalid": None}
    ok = False
    if summary["manifest"]:
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
        seed = manifest.get("seed_path_hint", "")
        # the seed lives wherever bake ran; re-hash when it is still there,
        # else fall back to presence of the recorded sha.
        if seed and os.path.isfile(seed):
            with open(seed, "rb") as f:
                summary["seed_ok"] = hashlib.sha1(f.read(), usedforsecurity=False).hexdigest() == \
                    manifest.get("seed_sha")
        else:
            summary["seed_ok"] = bool(manifest.get("seed_sha"))
            summary["seed_unchecked"] = seed or True
        summary["stamp_current"] = manifest.get("stamp") == translator_stamp()
        by_program = (manifest.get("translation") or {}).get("by_program", {})
        summary["translation_rows"] = len(by_program)
        # coverage: distinct seed indices translated vs the seed's program
        # count. < 1.0 is normal (LOWER_ERROR / policy-excluded programs stay
        # out and translate live); the missing set is bounded, not a failure.
        covered = set()
        for key in by_program:
            try:
                covered.add(int(str(key).split("/")[0]))
            except ValueError:
                pass
        summary["seed_programs"] = manifest.get("seed_programs", 0)
        summary["coverage_programs"] = len(covered)
        if summary["seed_programs"]:
            summary["coverage_fraction"] = (
                len(covered) / summary["seed_programs"])
            summary["missing_programs"] = (
                summary["seed_programs"] - len(covered))
        val = shutil.which("spirv-val")
        summary["spirv_val"] = val is not None
        for key, r in sorted(by_program.items()):
            tr = r.get("translate_dir", "")
            for name in r.get("spv", []):
                p = os.path.join(tr, name)
                summary["spv_files"] += 1
                try:
                    with open(p, "rb") as f:
                        magic = f.read(4)
                except OSError:
                    magic = b""
                if magic != b"\x03\x02\x23\x07":
                    summary["spv_bad_magic"] += 1
                    continue
                if val:
                    # the gate: layout the shipping device was created with
                    proc = subprocess.run(
                        [val, "--target-env", "vulkan1.3",
                         "--uniform-buffer-standard-layout", p],
                        capture_output=True, text=True)
                    if proc.returncode != 0:
                        summary["spirv_invalid"] += 1
                    if strict:
                        proc = subprocess.run(
                            [val, "--target-env", "vulkan1.3", p],
                            capture_output=True, text=True)
                        if summary["spirv_strict_invalid"] is None:
                            summary["spirv_strict_invalid"] = 0
                        if proc.returncode != 0:
                            summary["spirv_strict_invalid"] += 1
        ok = (summary["seed_ok"] and summary["spv_bad_magic"] == 0
              and summary["spirv_invalid"] == 0
              and (summary["translation_rows"] == 0 or summary["spv_files"] > 0))
    with open(os.path.join(bank_dir, "verify.json"), "w",
              encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    return summary, ok


def census_cmd(romfs: str, out: str):
    """Argv for the real census (needs player romfs + zstandard installed).
    Not run in this environment; provided so bake.py is the single entry."""
    return [sys.executable, "-I", "-u", "-X", "utf8",
            os.path.join(HERE, "shader_census.py"), romfs, "--out", out]


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bake", help="seed -> combos -> bank manifest")
    b.add_argument("--seed", required=True)
    b.add_argument("--bank", required=True)
    b.add_argument("--limit", type=int, default=None)
    b.add_argument("--resume", action="store_true")
    b.add_argument("--max-pipelines", type=int, default=8)
    c = sub.add_parser("census", help="print the census argv (needs romfs)")
    c.add_argument("romfs")
    c.add_argument("--out", required=True)
    s = sub.add_parser("stamp", help="print the translator stamp")
    t = sub.add_parser("translate",
                       help="seed -> exact keys + SPIR-V via gpu/shaderbatch "
                            "(adopted bake-mimo; needs C++ + SPIR-V headers)")
    t.add_argument("--seed", required=True)
    t.add_argument("--bank", required=True)
    t.add_argument("--tool-dir", default=None,
                   help="shaderbatch build cache (default <bank>/tool)")
    t.add_argument("--limit", type=int, default=0)
    t.add_argument("--jobs", type=int, default=0)
    v = sub.add_parser("verify",
                       help="read-back verify of bake_bank.json "
                            "(adapts bake_ds verify to the portable manifest)")
    v.add_argument("--bank", required=True)
    v.add_argument("--strict", action="store_true",
                   help="additionally record the strict-layout (no-flag) "
                        "spirv-val count for information only; the gate is "
                        "the shipping build's uniformBufferStandardLayout")
    a = p.parse_args(argv)

    if a.cmd == "stamp":
        print(translator_stamp())
        return 0
    if a.cmd == "census":
        print(" ".join(census_cmd(a.romfs, a.out)))
        print("note: needs the player's romfs + zstandard; not run here",
              file=sys.stderr)
        return 0
    if a.cmd == "bake":
        def progress(done_n, total):
            print("@@progress %.3f" % (done_n / max(1, total)), flush=True)

        path, stats = bake_seed(a.seed, a.bank, a.max_pipelines, a.limit,
                                a.resume, progress)
        print("@@done bake %s" % path)
        print("%d programs, %d stages -> %d combos -> %s"
              % (stats["programs"], stats["stages"], stats["combos"], path))
        return 0
    if a.cmd == "translate":
        tr, rows = translate_seed(a.seed, a.bank, a.tool_dir, a.limit,
                                  a.jobs)
        ok = sum(1 for r in rows.values() if r["status"] == "OK")
        print("@@done translate %s" % tr)
        print("%d program rows, %d OK -> merged into %s/bake_bank.json"
              % (len(rows), ok, a.bank))
        return 0
    if a.cmd == "verify":
        summary, ok = verify_bank(a.bank, a.strict)
        print("@@done verify %s" % a.bank)
        print(json.dumps(summary))
        return 0 if ok else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
