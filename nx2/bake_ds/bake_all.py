"""The local bake, end to end on this machine: game tree -> seed -> SPIR-V -> hookshot_programs.bin.

    python3 nx2/bake_ds/bake_all.py all --romfs <dir> --out <dir> [--dir <build dir>] [--stamp <hex16>]
                                    [--depth 0|1] [--variants all|0|1|2] [--limit N]
                                    [--target plain|tensor|tensor-software|rdna2|gfx1201|query]
                                    [--features <list>] [--variant-cap N] [--ssbo-align N] [--spv-dir <dir>]
                                    [--no-spirv-val]

Steps (each also a subcommand):
  1. enumerate  nx2/bake_ds/romenum.py: BNSH/BAHS/NDSR/NDVE/.vsetb, SARC, zstd+dictionaries -> hookshot_shaders.bin
  2. translate  nx2/bake_ds/bake_ds (built by nx2/bake_ds/build.sh): Ampere -> Maxwell -> Eden IR -> SPIR-V,
                written as <spv-dir>/*.spv and in the back end's own cache format <out>/hookshot_programs.bin
  3. verify     bake_ds verify (the game's reader over the bank, with the target's stamp check) + spirv-val

--target picks the feature profile: the switches the game must run with for this bank's SPIR-V to be the
translation it makes. They enter the cache stamp exactly as live folds EDEN_*/RX_VK_* from its environment
(vk_shader.cpp:837-864), so the loader's `kept == stamp` check (:902) refuses a bank baked for another profile.
`query` reads the local machine's Vulkan physical devices (no logical device, no game) and picks the profile
from the device class; its minimum storage-buffer offset alignment is what the SPIR-V's address masks are
baked with.

CPU only: no game directory. The only paths written are --out, --spv-dir and the build directory.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import romenum   # noqa: E402
import fixture   # noqa: E402

TARGETS = ("plain", "tensor", "tensor-emulate", "eden-tensor", "tensor-software", "software", "rdna2", "gfx1030",
           "gfx1201", "rdna4")


def translator_stamp(root=ROOT):
    """The sha1 half of HOOKSHOT_SHADER_STAMP, exactly as nx2/build.py:693-699 computes it: every file under
    gpu/vk/{eden,sirit,compat} plus vk_shader.cpp, vk_bank.cpp, vk_state.h, vk_api.h, relative path + CRLF-
    normalized bytes, sorted, first 16 hex. The cache stamp also mixes in sizeof(Shader::Info)/sizeof(Program),
    the library marker and RX_VK_*/EDEN_* env; bake_ds does that half (disk_stamp)."""
    vk = os.path.join(root, "gpu", "vk")
    files = [os.path.join(d, f) for top in ("eden", "sirit", "compat")
             for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
    files = sorted(files) + [os.path.join(vk, n) for n in ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h")]
    h = hashlib.sha1(usedforsecurity=False)
    for f in files:
        with open(f, "rb") as src:
            h.update(os.path.relpath(f, vk).replace(os.sep, "/").encode() + src.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()[:16]


def run(cmd, **kw):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, **kw)


def enumerate_step(romfs, out):
    os.makedirs(out, exist_ok=True)
    seed = os.path.join(out, "hookshot_shaders.bin")
    manifest = os.path.join(out, "enum.json")
    r = romenum.enumerate_tree(romfs)
    romenum.write_seed(seed, r)
    with open(manifest, "w") as f:
        json.dump({"romfs": os.path.abspath(romfs), "dicts": r["dicts"], "stats": r["stats"],
                   "programs": len(r["programs"]), "stages": len(r["stages"]),
                   "kinds": {"+".join(map(str, k)): v for k, v in romenum.kinds_of(r).most_common()},
                   "files": r["files"], "stage_sources": r["sources"]}, f, indent=1)
    print("enumerate: %d programs from %d stages -> %s" % (len(r["programs"]), len(r["stages"]), seed))
    return seed


def build_tool(build_dir, tool):
    if tool and os.path.exists(tool):
        return tool
    exe = os.path.join(build_dir, "bake_ds")
    if not os.path.exists(exe):
        run([os.path.join(HERE, "build.sh")], env={**os.environ, "OUT": build_dir}, check=True)
    return exe


def query_devices(tool):
    """The local Vulkan physical devices, as bake_ds query prints them (instance only: no logical device)."""
    p = subprocess.run([tool, "query"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=False)
    devices = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                devices.append(json.loads(line))
            except ValueError:
                pass
    if p.returncode or not devices:
        raise SystemExit("bake_all: --target query: no Vulkan physical device (bake_ds query rc %d)" % p.returncode)
    return devices


def preset_for(device):
    """The device class -> profile name. Both device profiles pin the tensor form to the software multiplies
    (bake_ds.cpp target_preset): a bank's stamp cannot see the device's capabilities, so the form the bank was
    baked with has to be a switch the game sets, not an inference."""
    if device.get("vendor") == 0x1002:   # AMD
        device_id = int(device.get("device_id") or 0)
        if device_id == 0x7550:          # Navi 48 (RX 9070 / 9070 XT)
            return "gfx1201"
        if 0x73A0 <= device_id <= 0x73FF:   # Navi 2x
            return "rdna2"
    return "tensor-software"


def resolve_target(tool, name, ssbo_align=None):
    """(preset, extra translate/verify args, device record or None) for a --target; `query` needs the tool."""
    if name != "query":
        if name not in TARGETS:
            raise SystemExit("bake_all: unknown --target %s (plain, tensor, tensor-software, rdna2, gfx1201, query)" % name)
        return name, ([] if ssbo_align is None else ["--ssbo-align", str(ssbo_align)]), None
    devices = query_devices(tool)
    # (a discrete GPU first: a laptop may also list its integrated one)
    devices.sort(key=lambda d: (d.get("type") != 2, d.get("device") or ""))
    device = devices[0]
    preset = preset_for(device)
    align = int(device.get("ssbo_align") or 16)
    extra = ["--ssbo-align", str(ssbo_align if ssbo_align is not None else align)]
    print("query: %s -> target %s, ssbo-align %s%s" % (
        device.get("device"), preset, align, "" if ssbo_align is None or ssbo_align == align else
        " (--ssbo-align %s given)" % ssbo_align))
    return preset, extra, {"devices": devices, "selected": device, "preset": preset,
                           "ssbo_align": ssbo_align if ssbo_align is not None else align}


def translate_step(tool, seed, out, stamp=None, depth=0, variants="all", limit=0, ssbo_align=16, target="plain",
                   features=None, variant_cap=64, spv_dir=None):
    os.makedirs(out, exist_ok=True)
    if stamp is None:
        stamp = translator_stamp()   # (the build of this checkout; --stamp overrides)
        print("translator stamp:", stamp)
    cmd = [tool, "translate", "--seed", seed, "--out", out, "--depth", str(depth), "--variants", variants,
           "--ssbo-align", str(ssbo_align), "--target", target, "--variant-cap", str(variant_cap)]
    if spv_dir:
        cmd += ["--spv-dir", spv_dir]
    if features:
        cmd += ["--feature", features]
    if stamp:
        cmd += ["--stamp", stamp]
    if limit:
        cmd += ["--limit", str(limit)]
    rc = run(cmd, check=False).returncode
    return rc, stamp


def verify_step(tool, out, stamp=None, target=None, features=None, ssbo_align=None, spv_dir=None, spirv_val=True):
    bank = os.path.join(out, "hookshot_programs.bin")
    cmd = [tool, "verify", "--bank", bank]
    if target and stamp:
        # (the same target + stamp the translation used: the loader's own check, vk_shader.cpp:902)
        cmd += ["--target", target, "--stamp", stamp]
        if features:
            cmd += ["--feature", features]
    rc = run(cmd, check=False).returncode
    spv = spv_dir or os.path.join(out, "spv")
    val = shutil.which("spirv-val") if spirv_val else None
    checked = invalid = 0
    # (the device enables uniformBufferStandardLayout (vk_main.cpp:253-360), so the modules are judged with
    # it, as the back end's own CI does; strict validity is not the contract this bank was built for)
    val_args = [val, "--target-env", "vulkan1.3", "--uniform-buffer-standard-layout"]
    if val and os.path.isdir(spv):
        files = [os.path.join(spv, n) for n in sorted(os.listdir(spv)) if n.endswith(".spv")]

        def check(path):
            p = subprocess.run(val_args + [path], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
            return path, p.returncode, p.stderr

        checked = len(files)
        workers = min(16, os.cpu_count() or 4, max(1, len(files)))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for path, rc, err in pool.map(check, files):
                if rc:
                    print("invalid:", os.path.basename(path))
                    invalid += 1
    summary = {"bank_ok": rc == 0, "stamp_check": bool(target and stamp),
               "spirv_val": val is not None, "spirv_layout": "uniformBufferStandardLayout",
               "spirv_files": checked, "spirv_invalid": invalid}
    with open(os.path.join(out, "verify.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print("verify:", summary)
    return 0 if rc == 0 and invalid == 0 else 1


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("command", choices=["all", "enumerate", "translate", "verify", "fixture", "query"])
    p.add_argument("--romfs")
    p.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "bake_ds_out"))
    p.add_argument("--dir", dest="build_dir", default=os.path.join(tempfile.gettempdir(), "hookshot-ds"))
    p.add_argument("--tool")
    p.add_argument("--stamp")
    p.add_argument("--depth", type=int, default=0)
    p.add_argument("--variants", default="all")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--ssbo-align", type=int, default=None,
                   help="device minStorageBufferOffsetAlignment the SPIR-V is baked with (default 16, the Vulkan "
                        "minimum; --target query takes it from the device)")
    p.add_argument("--target", default="plain",
                   help="feature profile: plain, tensor, tensor-software, rdna2, gfx1201, or query for this machine")
    p.add_argument("--features", help="comma list on top of the preset: tensor[=emulate|software|stub], integer, no-integer")
    p.add_argument("--variant-cap", type=int, default=64,
                   help="compute answer variants per program, baseline included (live's prepare_run uses 64)")
    p.add_argument("--spv-dir", help="where the .spv files go (default <out>/spv; the bank stays in --out)")
    p.add_argument("--no-spirv-val", action="store_true")
    p.add_argument("--with-dict", action="store_true")
    a = p.parse_args(argv)

    if a.command == "fixture":
        if not a.romfs:
            a.romfs = os.path.join(a.out, "romfs")
        os.makedirs(a.romfs, exist_ok=True)
        print(fixture.build(a.romfs, a.with_dict))
        return 0
    tool = build_tool(a.build_dir, a.tool)
    if a.command == "query":
        print(json.dumps(query_devices(tool), indent=1))
        return 0
    if not a.romfs:
        p.error("--romfs is needed")

    target, target_args, target_info = resolve_target(tool, a.target, a.ssbo_align)
    ssbo_align = a.ssbo_align if a.ssbo_align is not None else 16
    for k in range(0, len(target_args), 2):
        if target_args[k] == "--ssbo-align":
            ssbo_align = int(target_args[k + 1])
    if target_info:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "target.json"), "w") as f:
            json.dump(target_info, f, indent=1)

    if a.command == "enumerate":
        enumerate_step(a.romfs, a.out)
        return 0
    if a.command == "translate":
        seed = os.path.join(a.romfs, "hookshot_shaders.bin")
        rc, _ = translate_step(tool, seed, a.out, a.stamp, a.depth, a.variants, a.limit, ssbo_align, target,
                               a.features, a.variant_cap, a.spv_dir)
        return rc
    if a.command == "verify":
        return verify_step(tool, a.out, a.stamp, target if a.target != "plain" or a.features else None,
                           a.features, ssbo_align, a.spv_dir, not a.no_spirv_val)

    seed = enumerate_step(a.romfs, a.out)
    rc, stamp = translate_step(tool, seed, a.out, a.stamp, a.depth, a.variants, a.limit, ssbo_align, target,
                               a.features, a.variant_cap, a.spv_dir)
    if rc not in (0, 1):   # (1: some programs failed to translate; still verify what was written)
        return rc
    return verify_step(tool, a.out, stamp, target, a.features, ssbo_align, a.spv_dir, not a.no_spirv_val)


if __name__ == "__main__":
    sys.exit(main())
