"""The third-party code's objects, compiled by the release (the AppImage's usr/share/hookshot/third_party_objects, the Windows
zip's HookshotBuilder/tools/third_party_objects), so a player's first build, and a build after a change that empties the work
folder's object cache, does not compile them: Eden's shader recompiler and sirit (gpu/vk/eden, gpu/vk/sirit), Dear ImGui,
zbic's zstd (the Vulkan back end), libopus and zstd's decoder (the runtime), and on Windows suyu's recompiler and texture code
under the WebGPU renderer (gpu/third_party/suyu, compiled when wgpu-native is there). Measured on a full first build: ~200 of the
~325 CPU-seconds that are not the lifted game's own code. The renderer's own files (gpu/vk/*.cpp: DLSS, FSR 4 and the play
profile are the player's), host/ and the lifted game are never in it, and nothing in it is made from the game.

How a mismatch can't link a wrong object: these objects are found by their object key (nx2/build.py object_key: the source,
the flags, the headers), as the work folder's own object cache is, and nothing else. Their keys hold more than the other
objects' do (stamp): the compiler's version, the defaults its driver adds, and what the system headers, the C++ library and the Vulkan, SPIR-V and VMA
headers preprocess to, which the other keys leave to the work folder's system_headers.txt. Their CPU level is x86-64-v3 on
every CPU that has it (build.py THIRD_PARTY_CPU), not the game's -march=native, so the key does not name the CPU. A set made
with another compiler, other headers, other flags or other sources has other keys: build.py finds none of them and compiles
as before. Each object's SHA-256 is checked as it is copied into the work folder's cache (fetch), so a damaged file is
compiled instead too.

  manifest.json: {"format": FORMAT, "stamp": the stamp it was made with, "objects": {key: {"sha256", "size", "source"}}}
  <key>.o:       the objects

Made by the release (scripts/linux/appimage.sh; .forgejo/workflows/build-windows.yml):

  python -I nx2/third_party_objects.py make <out folder> [--jobs N]

which runs nx2/build.py --standalone --third-party-objects <out> with no game, at RX_MARCH=x86-64-v3, and with the build
tools the players' builds use (EMSDK, HOOKSHOT_SYSROOT, RX_CPM; HOOKSHOT_MINGW and RX_TARGET=windows for Windows).

HOOKSHOT_THIRD_PARTY_OBJECTS=0: not used (compiled as before); =<folder>: that set instead of the tools' own.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys

FORMAT = 1
ENV = "HOOKSHOT_THIRD_PARTY_OBJECTS"
FOLDER = "third_party_objects"
MANIFEST = "manifest.json"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (the sources whose objects the set holds, by folder under the tools' root)
# (build.py marks the jobs it builds as third-party, at THIRD_PARTY_CPU and with the stamp in their keys, by ending their headers
# with the stamp: a job is in the set when it is marked so and its source is under one of these. gpu/third_party/suyu: the WebGPU
# renderer's (Windows); the Vulkan back end's copies of its texture decoders are its own files, with its play profile)
SOURCES = ("third_party/opus/", "third_party/zstd/", "third_party/imgui/", "third_party/zbic/", "gpu/vk/eden/", "gpu/vk/sirit/",
           "gpu/third_party/suyu/")

# (what the probe preprocesses: the C library, the C++ library and, on Windows, windows.h. Their contents, not their paths)
PROBE_C = "".join("#if __has_include(<%s>)\n#include <%s>\n#endif\n" % (h, h) for h in
                  ("stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h", "math.h", "limits.h", "assert.h", "pthread.h",
                   "windows.h"))
PROBE_CXX = PROBE_C + "".join("#if __has_include(<%s>)\n#include <%s>\n#endif\n" % (h, h) for h in
                              ("algorithm", "array", "atomic", "bit", "cstdio", "cstdlib", "cstring", "format", "functional",
                               "map", "memory", "mutex", "optional", "span", "string", "string_view", "unordered_map",
                               "utility", "variant", "vector"))
# (the Vulkan, SPIR-V and VMA headers the Vulkan back end's third-party parts include: (package root, file under include/))
OUTSIDE_HEADERS = ("vulkan/vulkan_core.h", "vulkan/vulkan.h", "spirv/unified1/spirv.hpp", "spirv/unified1/spirv.h",
                   "spirv/unified1/GLSL.std.450.h", "vk_mem_alloc.h")


def is_third_party(src, root=ROOT):
    """A source whose object the set holds (SOURCES)"""
    try:   # (Windows: a work folder on another drive than the tools has no path relative to them, and is not theirs)
        rel = os.path.relpath(os.path.abspath(src), root).replace(os.sep, "/")
    except ValueError:
        return False
    return any(rel.startswith(p) for p in SOURCES)


def folder(root=ROOT):
    """The set this build uses: HOOKSHOT_THIRD_PARTY_OBJECTS's folder, else the tools' own; None when it is turned off"""
    raw = os.environ.get(ENV, "").strip()
    if raw.lower() in ("0", "false", "off", "no"):
        return None
    return os.path.abspath(os.path.expanduser(raw)) if raw else os.path.join(root, FOLDER)


def _version(cc):
    """clang's version line (the first of --version: the others name its folder)"""
    quiet = {"creationflags": 0x08000000} if os.name == "nt" else {}   # (CREATE_NO_WINDOW)
    r = subprocess.run([cc, "--version"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
                       **quiet)
    return (r.stdout.splitlines() or [""])[0].strip() if r.returncode == 0 else ""


def _driver(cc, flags, lang):
    """What the driver passes its compiler for these flags (`-###`: the target CPU and features, PIE, the stack protector, the
    C++ library, and every other default a clang build chose), without the words that name a path"""
    quiet = {"creationflags": 0x08000000} if os.name == "nt" else {}
    r = subprocess.run([cc] + flags + ["-###", "-x", lang, "-c", os.devnull, "-o", os.devnull], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=60, **quiet)
    if r.returncode:
        return None
    line = next((l for l in r.stderr.splitlines() if '"-cc1"' in l), None)
    if line is None:
        return None
    words = [w.strip('"') for w in line.split()][1:]   # (the first is the compiler's own path)
    return " ".join(w for w in words if not any(c in w for c in "/\\:") and w != os.devnull).encode()


def _preprocessed(cc, flags, lang, text):
    """What text preprocesses to with these flags, macro definitions included (-dD: the libraries' version macros), without line
    markers (-P: so with no path in it)"""
    quiet = {"creationflags": 0x08000000} if os.name == "nt" else {}
    r = subprocess.run([cc] + flags + ["-E", "-P", "-dD", "-x", lang, "-"], input=text.encode(), capture_output=True, timeout=120,
                       **quiet)
    return r.stdout if r.returncode == 0 else None


def stamp(cc, cxx, target, cpu, header_roots=()):
    """What the third-party objects' keys hold besides their source, flags and own headers: the compiler's version and what its
    driver makes of the flags (_driver: the defaults that clang build chose), the system headers and the C++ library as they
    preprocess for this target and CPU level, and the Vulkan, SPIR-V and VMA headers (their
    package roots, header_roots). Paths are not in it: the AppImage's mount and the build tools' folder change from PC to PC.
    "" when the compiler does not answer: then no set matches, and the objects are compiled"""
    import concurrent.futures
    try:   # (at once: a few seconds on a cold disk one after the other, at every build's start)
        with concurrent.futures.ThreadPoolExecutor(5) as ex:
            asked = [ex.submit(_version, cc), ex.submit(_driver, cc, target + cpu, "c"),
                     ex.submit(_driver, cxx, target + cpu + ["-std=c++20"], "c++"),
                     ex.submit(_preprocessed, cc, target + cpu, "c", PROBE_C),
                     ex.submit(_preprocessed, cxx, target + cpu + ["-std=c++20"], "c++", PROBE_CXX)]
            ver, *parts = [f.result() for f in asked]
    except (OSError, subprocess.SubprocessError):
        return ""
    if not ver or any(p is None for p in parts):
        return ""
    h = hashlib.sha256()
    for part in [ver.encode()] + parts:
        h.update(b"%d:" % len(part) + part)
    for root in header_roots:
        for rel in OUTSIDE_HEADERS:
            p = os.path.join(root or "", "include", *rel.split("/"))
            if root and os.path.isfile(p):
                with open(p, "rb") as f:
                    data = f.read()
                h.update(rel.encode() + b"%d:" % len(data) + data)
    return h.hexdigest()


def read_manifest(path):
    """The set's manifest, or None (none there, unreadable, or of another format)"""
    try:
        with open(os.path.join(path, MANIFEST), encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError, UnicodeDecodeError):
        return None
    if not isinstance(m, dict) or m.get("format") != FORMAT or not isinstance(m.get("objects"), dict):
        return None
    return m


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def matching(path, keys):
    """Of these object keys, the ones the set at path holds (by its manifest; nothing is read but it)"""
    m = read_manifest(path) if path else None
    if not m:
        return set()
    return {k for k in keys if k in m["objects"]}


def fetch(path, cache, keys):
    """The objects of these keys that the set holds and cache lacks, copied into cache (<key>.o, as nx2/build.py cache_put names
    them), each checked against the manifest's SHA-256 and size first. Returns (copied, held: how many of keys the set holds,
    the set's stamp or None)"""
    m = read_manifest(path) if path else None
    if not m:
        return 0, 0, None
    copied = held = 0
    for key in sorted(set(keys)):
        entry = m["objects"].get(key)
        if not isinstance(entry, dict):
            continue
        held += 1
        dst = os.path.join(cache, key + ".o")
        if os.path.exists(dst):
            continue
        src = os.path.join(path, key + ".o")
        tmp = "%s.%d.tmp" % (dst, os.getpid())
        try:
            os.makedirs(cache, exist_ok=True)
            shutil.copyfile(src, tmp)   # (a copy: the AppImage's tree is read-only, and the cache's objects are hard-linked on)
            if os.path.getsize(tmp) != entry.get("size") or _sha256(tmp) != entry.get("sha256"):
                os.remove(tmp)
                held -= 1
                continue
            os.replace(tmp, dst)
            copied += 1
        except OSError:
            held -= 1
            try:
                os.remove(tmp)
            except OSError:
                pass
    return copied, held, m.get("stamp")


def write(out, objects, stamp_value):
    """The set: objects is [(key, object path, source path)]. Written as a whole folder, the manifest last"""
    os.makedirs(out, exist_ok=True)
    entries = {}
    for key, obj, src in sorted(objects):
        dst = os.path.join(out, key + ".o")
        shutil.copyfile(obj, dst)
        entries[key] = {"sha256": _sha256(dst), "size": os.path.getsize(dst),
                        "source": os.path.relpath(os.path.abspath(src), ROOT).replace(os.sep, "/")}
    with open(os.path.join(out, MANIFEST + ".new"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"format": FORMAT, "stamp": stamp_value, "objects": entries}, f, indent=1, sort_keys=True)
    os.replace(os.path.join(out, MANIFEST + ".new"), os.path.join(out, MANIFEST))
    return entries


def make(out, jobs):
    """The set, made by nx2/build.py in a work folder of its own with no game (the module's notes)"""
    import tempfile
    out = os.path.abspath(out)
    if os.path.exists(out):
        shutil.rmtree(out)
    work = tempfile.mkdtemp(prefix="hookshot-third-party-")
    try:
        # (x86-64-v3: the level a player's build compiles them at on every CPU that has it, whatever this PC's CPU is.
        # RX_RELEASE as make_game.py sets it; it is not in these objects' flags, but the build is a player's otherwise)
        env = dict(os.environ, OOT_WORK=work, RX_MARCH="x86-64-v3", RX_RELEASE="1", A64_HOT_CHUNK="1", PYTHONIOENCODING="utf-8")
        env.pop(ENV, None)
        env.pop("HOOKSHOT_OBJ_STORE", None)
        r = subprocess.run([sys.executable, "-I", "-B", "-u", os.path.join(ROOT, "nx2", "build.py"), "--standalone",
                            "--jobs", str(jobs), "--third-party-objects", out], env=env)
        return r.returncode
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv=None):
    import argparse
    p = argparse.ArgumentParser(description="the third-party code's objects a release ships (the module's notes)")
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("out")
    m.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    a = p.parse_args(argv)
    return make(a.out, a.jobs)


if __name__ == "__main__":
    sys.exit(main())
