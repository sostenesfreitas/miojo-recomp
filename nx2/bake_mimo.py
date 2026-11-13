"""Bake-everything, end to end and CPU-only: the game's shader archives in, a bank of translated programs out.

    python3 -I nx2/bake_mimo.py all --romfs <dump>/romfs --out work/bake
    python3 -I nx2/bake_mimo.py enumerate --romfs <dump>/romfs --out work/bake   # ROM -> seed + manifest only
    python3 -I nx2/bake_mimo.py translate --out work/bake [--limit N]            # seed -> SPIR-V (builds the tool)
    python3 -I nx2/bake_mimo.py bank --out work/bake                             # seed + results -> bank/

Three stages, each usable alone (the stop-lines are in BAKE.md §3):

  enumerate  nx2/shader_census.py's parsers over the dump's romfs (Shader/, Layout2Shader/, Lib/, Movie/,
             Effect/; BNSH, BAHS, NDSR, NDVE and the SARC around them), deduped to stages the way the back end
             keys them: the FNV-1a code_key of gpu/vk/vk_shader.cpp:135-137 over control(0xd8) + code(end).
             Writes the runtime's seed (HKSEED01, read by seed_open, vk_shader.cpp:1844) and a manifest that
             says where each stage came from. Stable IDs: the stage's code_key (same as RX_VK_DUMP_STAGES
             filenames and seed keys at run time); a program's is the tuple of its stage IDs, plus its exact
             runtime program key once translate has run (compute keys are exact here already: a compute
             program's key is its stage's code_key, vk_shader.cpp:1698).

  translate  builds gpu/shaderbatch.cpp against the vendored recompiler (gpu/vk/eden, gpu/vk/sirit: clang++/g++,
             no Vulkan headers, no device) and runs it over the seed: every stage it can lower and emit becomes
             <out>/translate/spv/<progkey>_<stage>.spv, with stages.tsv / programs.tsv saying what each one
             did. This is the offline half of prepare_run (gpu/vk/vk_draw.cpp:5423): translation only, no
             VkShaderModule, no pipeline.

  bank       puts the two together: bank/bank.json (counts + HOOKSHOT_SHADER_STAMP, computed exactly as
             nx2/build.py:691-699, so a Vulkan machine can check its hookshot_programs.bin is still good),
             bank/stages.jsonl, bank/programs.jsonl and bank/spv/. Every pipeline in it is PENDING_GPU:
             building VkPipelines is the one part of the bake this pass does not do.

Nothing here reads the game folder unless --romfs points at it; outputs go under --out (default work/bake).
The dump is the player's own: see nx2/kit.py:3-5 for where the tools normally find it.
"""
import argparse
import collections
import datetime
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import shader_census as sc  # noqa: E402  (the parsers this reuses)

SEED_MAGIC = b"HKSEED01"
CONTROL_MAGIC = b"\x34\x12\x76\x98"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
CONTROL_SIZE = 0xD8
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
M64 = (1 << 64) - 1


def fnv1a(data, h=FNV_OFFSET):
    """gpu/vk/vk_shader.cpp:99-103 hash_bytes: the identity of a stage there and here."""
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def code_key(control, code):
    """vk_shader.cpp:135-137 code_key: FNV-1a over the 0xd8 control bytes, then the code area they size."""
    return fnv1a(code, fnv1a(control))


def stage_id(control, code):
    end = struct.unpack_from("<I", control, 0x44)[0]
    control = control[:CONTROL_SIZE]
    code = code[:end] if end <= len(code) else code
    return "%016x" % code_key(control, code), control, code


# ---- enumerate: the archives in, the seed and a manifest out ----


def iter_programs(romfs, zs, dicts, counters, notes):
    """Every (source, program) the census's parsers find: [(stages [(nvn, control, code)...], source)...]"""
    for path in sc.archives(romfs):
        rel = os.path.relpath(path, romfs)
        counters["files"] += 1
        try:
            raw = open(path, "rb").read()
        except OSError as e:
            notes.append("%s: %s" % (rel, e))
            continue
        try:
            if raw[:4] == ZSTD_MAGIC and zs is None:
                counters["compressed_without_zstandard"] += 1
                continue
            data = sc.unpack(raw, dicts, zs)
        except Exception as e:
            counters["skipped"] += 1
            notes.append("%s: unpack: %s" % (rel, e))
            continue
        try:
            blobs = list(sc.blobs(data, dicts, zs) if data[:4] == b"SARC" else [data])
        except Exception as e:
            counters["skipped"] += 1
            notes.append("%s: inner: %s" % (rel, e))
            continue
        for k, blob in enumerate(blobs):
            try:
                found = list(sc.programs_in(blob, counters["slots"]))
            except Exception as e:
                counters["containers_skipped"] += 1
                notes.append("%s container %d %r: %s" % (rel, k, blob[:4], e))
                continue
            for j, stages in enumerate(found):
                yield stages, "%s#%d#%d" % (rel.replace(os.sep, "/"), k, j)


def kind_of(stages):
    kinds = {s & 7 for s in stages}
    if 5 in kinds:
        return "compute"
    if 7 in kinds:
        return "mesh"
    if 0 in kinds:
        return "graphics"
    return "unknown"


def enumerate_romfs(romfs, out_dir, limit=0):
    """The miner: romfs -> seed (HKSEED01) + manifest.json with stable IDs. Returns the manifest."""
    if not os.path.isdir(romfs):
        raise SystemExit("%s: not a folder: the miner walks <dump>/romfs (nx2/kit.py:3-5)" % romfs)
    zs = None
    try:
        import zstandard as zs_mod
        zs = zs_mod
    except ImportError:
        pass
    dicts = []
    dic_path = os.path.join(romfs, "Pack", "ZsDic.pack.zs")
    if zs is not None and os.path.isfile(dic_path):
        dicts = sc.dictionaries(romfs, zs)

    counters = collections.Counter(files=0, programs=0, stages=0, skipped=0, containers_skipped=0,
                                   compressed_without_zstandard=0)
    counters["slots"] = collections.Counter()
    notes = []
    index, stage_list, programs, seen = {}, [], [], {}   # seen: stage-id tuple -> its program's row
    started = time.time()
    for stages, source in iter_programs(romfs, zs, dicts, counters, notes):
        ids = []
        for nvn, control, code in stages:
            if control[:4] != CONTROL_MAGIC or len(control) < CONTROL_SIZE:
                continue
            ident, control, code = stage_id(control, code)
            if ident not in index:
                index[ident] = len(stage_list)
                stage_list.append({"id": ident, "nvn_stage": nvn, "control": control, "code": code,
                                   "census_key": "%016x" % int.from_bytes(
                                       hashlib.sha1(control + code, usedforsecurity=False).digest()[:8], "little"),
                                   "sources": [source]})
            else:
                srcs = stage_list[index[ident]]["sources"]
                if source not in srcs and len(srcs) < 16:
                    srcs.append(source)
            ids.append(index[ident])
        ids = tuple(sorted(set(ids)))
        if not ids:
            continue
        if ids not in seen:
            seen[ids] = len(programs)
            programs.append((ids, [source]))
            counters["programs"] += 1
            if limit and len(programs) >= limit:
                break
        else:   # (the same program in a second archive: both places say so)
            srcs = programs[seen[ids]][1]
            if source not in srcs and len(srcs) < 16:
                srcs.append(source)
    counters["stages"] = len(stage_list)

    os.makedirs(out_dir, exist_ok=True)
    seed = os.path.join(out_dir, "hookshot_shaders.bin")
    with open(seed, "wb") as f:
        f.write(SEED_MAGIC + struct.pack("<II", len(programs), len(stage_list)))
        for st in stage_list:   # (the stage key the runtime recomputes anyway, seed_open :1872: this is it already)
            f.write(struct.pack("<QIII", code_key(st["control"], st["code"]), st["nvn_stage"],
                                len(st["control"]), len(st["code"])) + st["control"] + st["code"])
        for ids, _ in programs:
            f.write(struct.pack("<I%dI" % len(ids), len(ids), *ids))

    def compute_key(ids):
        for k in ids:
            if stage_list[k]["nvn_stage"] & 7 == 5:
                return stage_list[k]["id"]
        return None

    manifest = {
        "format": "HKBKM01",
        "created": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "romfs": os.path.abspath(romfs),
        "seed": os.path.relpath(seed, out_dir),
        "seed_bytes": os.path.getsize(seed),
        "elapsed_s": round(time.time() - started, 1),
        "zstandard": zs is not None,
        "dictionaries": len(dicts),
        "counters": {"files": counters["files"], "programs": len(programs), "stages": len(stage_list),
                     "skipped": counters["skipped"], "containers_skipped": counters["containers_skipped"],
                     "compressed_without_zstandard": counters["compressed_without_zstandard"]},
        "slots": {str(k): v for k, v in sorted(counters["slots"].items())},
        "notes": notes[:64],
        "stages": [{"id": st["id"], "nvn_stage": st["nvn_stage"], "control_bytes": len(st["control"]),
                    "code_bytes": len(st["code"]), "census_key": st["census_key"], "sources": st["sources"]}
                   for st in stage_list],
        "programs": [{"i": i, "kind": kind_of([stage_list[k]["nvn_stage"] for k in ids]),
                      "stage_ids": [stage_list[k]["id"] for k in ids],
                      "runtime_key": (compute_key(ids) if kind_of(
                          [stage_list[k]["nvn_stage"] for k in ids]) == "compute" else None),
                      "runtime_key_note": ("a compute program's key is its stage's code_key (vk_shader.cpp:1698)"
                                           if kind_of([stage_list[k]["nvn_stage"] for k in ids]) == "compute" else
                                           "filled in by translate: graphics_key/mesh_key (vk_shader.cpp:733-752)"),
                      "sources": sources}
                     for i, (ids, sources) in enumerate(programs)],
    }
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f)
    return manifest


# ---- translate: build gpu/shaderbatch.cpp, run it over the seed ----


def spirv_headers():
    """The SPIR-V headers the recompiler's sirit needs: HOOKSHOT_SPIRV_HEADERS, else a CPM cache."""
    if os.environ.get("HOOKSHOT_SPIRV_HEADERS"):
        return os.environ["HOOKSHOT_SPIRV_HEADERS"]
    probe = os.path.join("spirv", "unified1", "spirv.hpp11")
    for cpm in (os.environ.get("RX_CPM"), os.path.join(ROOT, "buildext", "cpm"),
                os.path.join(ROOT, "build-linux", "deps", "cpm"),
                os.path.join(ROOT, "cpm"), os.path.join(ROOT, "..", "buildext", "cpm"),
                os.path.join(ROOT, "..", "build-linux", "deps", "cpm")):
        if not cpm or not os.path.isdir(os.path.join(cpm, "spirv-headers")):
            continue
        base = os.path.join(cpm, "spirv-headers")
        for ver in sorted(os.listdir(base)):
            inc = os.path.join(base, ver, "include")
            if os.path.isfile(os.path.join(inc, probe)):
                return inc
    return None


def cxx():
    want = os.environ.get("BAKE_CXX")
    if want:
        return want
    for name in ("clang++", "g++", "c++"):
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit("no C++ compiler: set BAKE_CXX (the bake builds gpu/shaderbatch.cpp)")


def cxx_std(compiler):
    """-std=c++23 where the compiler has it, else -std=c++2b (gpu/vk/compat/common/assert.h needs the static-in-
    constexpr rule of C++23, and the recompiler is written against it)."""
    cached = os.path.join(work_dir(), "cxx_std")
    if os.path.isfile(cached):
        return open(cached).read().strip()
    for std in ("-std=c++23", "-std=c++2b"):
        # Match assert.h: a static on the non-constant failure path, while a
        # successful call remains a valid constant expression (also on Clang 18).
        probe = "constexpr int f(int n){if(n<0){static int x=0;return ++x;}return n;} static_assert(f(1)==1);"
        r = subprocess.run([compiler, std, "-x", "c++", "-fsyntax-only", "-"], input=probe, text=True,
                           capture_output=True)
        if r.returncode == 0:
            os.makedirs(work_dir(), exist_ok=True)
            with open(cached, "w") as f:
                f.write(std)
            return std
    raise SystemExit("%s: no C++23 mode: the recompiler needs one (BAKE_CXX to point at a newer compiler)" % compiler)


def work_dir(out=None):
    return out or os.environ.get("BAKE_WORK") or os.path.join(ROOT, "work", "bake")


def translator_stamp():
    """nx2/build.py:691-699 HOOKSHOT_SHADER_STAMP, byte for byte: what makes hookshot_programs.bin valid."""
    vk = os.path.join(ROOT, "gpu", "vk")
    stamp = hashlib.sha1(usedforsecurity=False)
    translator = [os.path.join(d, f) for top in ("eden", "sirit", "compat")
                  for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
    for f in sorted(translator) + [os.path.join(vk, n) for n in
                                   ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h")]:
        with open(f, "rb") as src:
            stamp.update(os.path.relpath(f, vk).replace("\\", "/").encode()
                         + src.read().replace(bytes((13, 10)), bytes((10,))))
    return stamp.hexdigest()[:16]


def build_tool(out_dir, jobs=0):
    """gpu/shaderbatch.cpp + the vendored recompiler -> <out>/shaderbatch (objects cached under work)."""
    inc = spirv_headers()
    if not inc:
        raise SystemExit("no SPIR-V headers: set HOOKSHOT_SPIRV_HEADERS or RX_CPM (sirit needs "
                         "spirv/unified1/spirv.hpp11)")
    compiler = cxx()
    std = cxx_std(compiler)
    vk = os.path.join(ROOT, "gpu", "vk")
    sources = []
    for top in ("eden", os.path.join("sirit", "src")):
        for d, _, fs in os.walk(os.path.join(vk, top)):
            sources += [os.path.join(d, f) for f in fs if f.endswith(".cpp")]
    sources.append(os.path.join(ROOT, "gpu", "shaderbatch.cpp"))
    sources.sort()
    flags = [std, "-O1", "-g0", "-w", "-DNDEBUG", "-I" + os.path.join(vk, "eden"),
             "-I" + os.path.join(vk, "sirit", "include"), "-I" + os.path.join(vk, "sirit", "src"),
             "-I" + os.path.join(vk, "compat"), "-I" + os.path.join(ROOT, "gpu", "compat"), "-I" + inc]
    objs_dir = os.path.join(out_dir, "obj")
    exe = os.path.join(out_dir, "shaderbatch")
    stamp = "\x00".join(flags)
    stamp_path = os.path.join(out_dir, "obj.flags")
    if os.path.isfile(stamp_path) and open(stamp_path).read() != stamp:   # (flags changed: everything rebuilds)
        shutil.rmtree(objs_dir, ignore_errors=True)
    os.makedirs(objs_dir, exist_ok=True)
    if not os.path.isfile(stamp_path):
        with open(stamp_path, "w") as f:
            f.write(stamp)

    def obj_of(src):
        name = hashlib.sha1(os.path.relpath(src, ROOT).encode(), usedforsecurity=False).hexdigest()[:16] + ".o"
        return os.path.join(objs_dir, name)

    jobs = jobs or min(os.cpu_count() or 4, 24)
    pending = [(src, obj_of(src)) for src in sources
               if not os.path.isfile(obj_of(src)) or os.path.getmtime(obj_of(src)) < os.path.getmtime(src)]
    newest_obj = max((os.path.getmtime(o) if os.path.isfile(o) else 0
                      for o in (obj_of(s) for s in sources)), default=0)
    if pending or not os.path.isfile(exe) or os.path.getmtime(exe) < newest_obj:
        started = time.time()
        import concurrent.futures

        def compile_one(job):
            src, obj = job
            r = subprocess.run([compiler] + flags + ["-c", src, "-o", obj], capture_output=True, text=True)
            return src, r.returncode, r.stderr

        failed = None
        with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
            for src, rc, err in ex.map(compile_one, pending):
                if rc and failed is None:
                    failed = "%s:\n%s" % (src, err[-4000:])
        if failed:
            raise SystemExit("building shaderbatch failed:\n" + failed)
        objs = sorted(obj_of(s) for s in sources)
        r = subprocess.run([compiler] + flags + objs + ["-o", exe], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("linking shaderbatch failed:\n" + r.stderr[-4000:])
        print("shaderbatch: %d objects (%d rebuilt) in %.0f s -> %s"
              % (len(objs), len(pending), time.time() - started, exe), flush=True)
    return exe


def run_tool(out_dir, seed=None, limit=0, jobs=0):
    """Build and run the translator over the seed; returns the translate output directory."""
    seed = seed or os.path.join(out_dir, "hookshot_shaders.bin")
    if not os.path.isfile(seed):
        raise SystemExit("%s: no seed: run enumerate first" % seed)
    exe = build_tool(out_dir, jobs)
    tr = os.path.join(out_dir, "translate")
    os.makedirs(os.path.join(tr, "spv"), exist_ok=True)
    cmd = [exe, seed, tr] + (["--limit", str(limit)] if limit else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stderr.write(r.stderr)
    if r.returncode:
        raise SystemExit("shaderbatch failed (%d): %s" % (r.returncode, r.stdout + r.stderr))
    print(r.stdout.strip(), flush=True)
    return tr


# ---- bank: everything the two stages know, as one directory ----


def read_tsv(path):
    rows = []
    if not os.path.isfile(path):
        return rows
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            rows.append(line.split("\t"))
    return rows


def make_bank(out_dir, manifest_path=None):
    out_dir = os.path.abspath(out_dir)
    manifest_path = manifest_path or os.path.join(out_dir, "manifest.json")
    tr = os.path.join(out_dir, "translate")
    manifest = json.load(open(manifest_path)) if os.path.isfile(manifest_path) else None
    stages_tsv = read_tsv(os.path.join(tr, "stages.tsv"))
    programs_tsv = read_tsv(os.path.join(tr, "programs.tsv"))

    bank = os.path.join(out_dir, "bank")
    os.makedirs(os.path.join(bank, "spv"), exist_ok=True)
    stamp = translator_stamp()

    stage_rows = []
    by_id = {}
    for st in (manifest["stages"] if manifest else []):
        by_id[st["id"]] = st
    for row in stages_tsv:
        key, nvn, status, detail, cbytes, nbytes, native, spirv, cbuf, textures, storage, images = (row + [""] * 12)[:12]
        st = by_id.get(key, {})
        stage_rows.append({"id": key, "nvn_stage": int(nvn), "status": status, "detail": detail,
                           "control_bytes": int(cbytes), "code_bytes": int(nbytes), "native_insns": int(native),
                           "spirv_words": int(spirv), "cbuf_mask": int(cbuf, 16), "texture_descriptors": int(textures),
                           "storage_buffers": int(storage), "images": int(images),
                           "sources": st.get("sources", [])})

    programs_rows = []
    seen_spv = set()
    for row in programs_tsv:
        idx, inputs, key, kind, stages_s, keys_s, status, spirv, detail = (row + [""] * 9)[:9]
        files = []
        for name in [s for s in spirv.split(",") if s]:
            dst = os.path.join(bank, "spv", os.path.basename(name))
            src = os.path.join(tr, name)
            if os.path.isfile(src) and os.path.basename(name) not in seen_spv:
                shutil.copyfile(src, dst)
                seen_spv.add(os.path.basename(name))
                files.append("spv/" + os.path.basename(name))
        entry = {"i": int(idx), "inputs": int(inputs), "runtime_key": key, "kind": kind,
                 "stage_ids": keys_s.split(",") if keys_s else [],
                 "stages": [int(s) for s in stages_s.split(",") if s != ""],
                 "status": {"OK": "TRANSLATED"}.get(status, status),
                 "detail": detail, "spv": files, "pipelines": "PENDING_GPU"}
        if manifest and int(idx) < len(manifest["programs"]):
            entry["sources"] = manifest["programs"][int(idx)]["sources"]
        programs_rows.append(entry)

    if not programs_rows and manifest:   # (enumerate only: the bank says what it is waiting for)
        for m in manifest["programs"]:
            programs_rows.append({"i": m["i"], "inputs": 0, "runtime_key": m["runtime_key"], "kind": m["kind"],
                                  "stage_ids": m["stage_ids"], "stages": [], "status": "NOT_TRANSLATED",
                                  "detail": "translate has not run on this seed", "spv": [],
                                  "pipelines": "PENDING_GPU", "sources": m["sources"]})
    translated = sum(1 for p in programs_rows if p["status"] == "TRANSLATED")
    with open(os.path.join(bank, "stages.jsonl"), "w") as f:
        for st in stage_rows:
            f.write(json.dumps(st) + "\n")
    with open(os.path.join(bank, "programs.jsonl"), "w") as f:
        for p in programs_rows:
            f.write(json.dumps(p) + "\n")
    header = {
        "format": "HKBANK01",
        "created": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "generator": "nx2/bake_mimo.py",
        "translator_stamp": stamp,   # (== HOOKSHOT_SHADER_STAMP of nx2/build.py:691-699 when this tree is that tree)
        "seed": manifest["seed"] if manifest else "hookshot_shaders.bin",
        "enumerate": manifest["counters"] if manifest else None,
        "translate": {"translated": translated, "rows": len(programs_rows),
                      "stages": {s: sum(1 for r in stage_rows if r["status"] == s)
                                 for s in sorted({r["status"] for r in stage_rows})}},
        "pipelines": "PENDING_GPU: no VkPipeline is built here (no device); the program bank is what the "
                     "first start on a Vulkan machine builds pipelines from (prepare_run/precompile_run).",
        "statuses": {"TRANSLATED": "every stage of the program emitted SPIR-V under bank/spv/",
                     "LOWER_ERROR/TRANSLATE_ERROR/EMIT_ERROR": "the recompiler refused this one: see detail",
                     "task/mesh rows": "NVN2 task/mesh translated through the runtime's own mesh path "
                                      "(translate_mesh_into, prim=null as the seed path passes it)",
                     "HEADER_ONLY": "a control section that sizes a header and no instructions",
                     "NOT_ATTEMPTED": "no program of the seed asked for this stage"},
    }
    with open(os.path.join(bank, "bank.json"), "w") as f:
        json.dump(header, f, indent=1)
    print("bank: %d programs (%d translated), %d stages, %d SPIR-V files -> %s"
          % (len(programs_rows), translated, len(stage_rows), len(seen_spv), bank), flush=True)
    return header


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("enumerate", help="romfs -> seed + manifest (the miner)")
    e.add_argument("--romfs", required=True)
    e.add_argument("--limit", type=int, default=0, help="stop after N programs (a smoke run)")
    t = sub.add_parser("translate", help="seed -> SPIR-V through gpu/shaderbatch (builds it first)")
    t.add_argument("--limit", type=int, default=0)
    t.add_argument("--jobs", type=int, default=0)
    b = sub.add_parser("bank", help="manifest + translate results -> bank/")
    a = sub.add_parser("all", help="enumerate, translate, bank")
    a.add_argument("--romfs", required=True)
    a.add_argument("--limit", type=int, default=0)
    a.add_argument("--jobs", type=int, default=0)
    a.add_argument("--no-translate", action="store_true", help="stop after enumerate (no compiler needed)")
    for x in (e, t, b, a):
        x.add_argument("--out", default=os.path.join(ROOT, "work", "bake"),
                       help="where the seed, the tool's output and the bank go (default work/bake)")
    args = p.parse_args()
    out = args.out
    if args.cmd in ("enumerate", "all"):
        m = enumerate_romfs(args.romfs, out, args.limit)
        c = m["counters"]
        print("enumerate: %d programs, %d stages from %d files (%.0f s) -> %s"
              % (c["programs"], c["stages"], c["files"], m["elapsed_s"], out), flush=True)
        if c["compressed_without_zstandard"]:
            print("  %d compressed files left unread: the zstandard package is not installed"
                  % c["compressed_without_zstandard"], flush=True)
    if args.cmd in ("translate", "all") and not (args.cmd == "all" and args.no_translate):
        run_tool(out, limit=args.limit, jobs=args.jobs)
    if args.cmd in ("bank", "all"):
        make_bank(out)


if __name__ == "__main__":
    sys.exit(main())
