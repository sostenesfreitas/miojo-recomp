"""The program cache is kept across builds that change only how it is read or scheduled, and dropped when the translator or the
cache's own format changes.

hookshot_programs.bin (6.9 GB with every program of the game) is good for program_cache_stamp: the translator's stamp
(translator_stamp in gpu/vk/vk_shader.cpp, HOOKSHOT_SHADER_STAMP from nx2/build.py, which hashes vk_shader.cpp and the recompiler)
mixed with kProgramCacheFormat. The cache's reads and the prepare's scheduling are in gpu/vk/vk_progcache.cpp, outside the hashed
files: an edit there kept nothing out before (they were in vk_shader.cpp, so every such change made every player translate the whole
game again). A change to what a record holds raises kProgramCacheFormat instead.

Checked here: nx2/build.py's real stamp block, run on authored files, changes for a vk_shader.cpp edit and not for a vk_progcache.cpp
or vk_progcache.h edit; and in the source, that the program cache's file, index, notes and prepared stamps all come from
program_cache_stamp, which takes translator_stamp and kProgramCacheFormat, and that the cache's reads are no longer in vk_shader.cpp.

No compiler, Vulkan, GPU or game. python -I -B tests/test_program_cache_format.py
"""
import ast
import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def body(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("FAIL: expected one %r" % marker)
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def stamp_block():
    tree = ast.parse((ROOT / "nx2" / "build.py").read_text(encoding="utf-8"))
    assigns = lambda n, name: isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in n.targets)
    branches = [n for n in ast.walk(tree) if isinstance(n, ast.If) and any(assigns(s, "translator") for s in n.body)]
    if len(branches) != 1:
        raise SystemExit("FAIL: nx2/build.py's translator stamp block not found")
    body_ = [n for n in branches[0].body if assigns(n, "stamp") or assigns(n, "translator") or
             isinstance(n, ast.For) and isinstance(n.iter, ast.BinOp) and any(
                 isinstance(x, ast.Name) and x.id == "translator" for x in ast.walk(n.iter))]
    if len(body_) != 3:
        raise SystemExit("FAIL: expected the hash, the translator list and the file loop")
    return compile(ast.Module(body=body_, type_ignores=[]), "nx2/build.py", "exec")


def main():
    code = stamp_block()
    with tempfile.TemporaryDirectory(prefix="program-cache-format-") as tmp:
        vk = Path(tmp) / "gpu/vk"
        for rel in ("eden/a.cpp", "sirit/a.cpp", "compat/a.h", "vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h",
                    "vk_progcache.cpp", "vk_progcache.h"):
            f = vk / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(b"authored input\n")

        def stamp():
            ns = dict(os=os, hashlib=hashlib, vk=str(vk))
            exec(code, ns)
            return ns["stamp"].hexdigest()[:16]

        first = stamp()
        (vk / "vk_progcache.cpp").write_bytes(b"the precompile reads on more threads\n")
        (vk / "vk_progcache.h").write_bytes(b"a reader's helper moved\n")
        if stamp() != first:
            raise SystemExit("FAIL: an edit to the program cache's reads changes the translator stamp: every player translates again")
        (vk / "vk_shader.cpp").write_bytes(b"a translation fix\n")
        if stamp() == first:
            raise SystemExit("FAIL: an edit to the translator keeps the programs it translated before")

    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    cache = (VK / "vk_progcache.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_progcache.h").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    translator = body(shader, "uint64_t translator_stamp() {")
    if "HOOKSHOT_SHADER_STAMP" not in translator:
        raise SystemExit("FAIL: translator_stamp does not take the build's translator stamp")
    if not re.search(r"constexpr uint32_t kProgramCacheFormat = \d+;", cache):
        raise SystemExit("FAIL: the program cache has no format version of its own")
    stamp_fn = body(cache, "uint64_t program_cache_stamp() {")
    if "translator_stamp()" not in stamp_fn or "kProgramCacheFormat" not in stamp_fn:
        raise SystemExit("FAIL: program_cache_stamp is not the translator's stamp and the cache's format")
    init = body(cache, "void disk_init() {")
    if "const uint64_t stamp = program_cache_stamp();" not in init or "kept == stamp" not in init or "is == stamp" not in init:
        raise SystemExit("FAIL: the cache file or its index is not checked against program_cache_stamp")
    if "disk_stamp" in shader + cache + header + draw:
        raise SystemExit("FAIL: a second stamp (disk_stamp) is still used somewhere")
    if draw.count("program_cache_stamp()") < 4:
        raise SystemExit("FAIL: the notes, the state list's notes or hookshot_prepared.bin are no longer stamped with the cache")
    for fn in ("void disk_init(", "void program_cache_save(", "void program_cache_read(", "size_t program_cache_load_some(",
               "void program_cache_borrow(", "void program_cache_each(", "std::vector<uint64_t> program_cache_keys(",
               "void put_info(", "void get_info(", "unsigned usable_cpus(", "unsigned half_cpus("):
        if fn + ")" in shader or re.search(re.escape(fn) + r"[^;]*\)\s*\{", shader):
            raise SystemExit("FAIL: %s is in vk_shader.cpp, inside the translator stamp" % fn.rstrip("("))
        if not re.search(re.escape(fn) + r"[^;]*\)\s*\{", cache):
            raise SystemExit("FAIL: %s is not defined in vk_progcache.cpp" % fn.rstrip("("))
    # (what a record writes as it lies in memory from vk_shader.h, which the translator stamp does not hash: each named in a
    # static_assert beside kProgramCacheFormat, so a layout change stops the build until the format is raised)
    fmt = cache.index("constexpr uint32_t kProgramCacheFormat")
    asserts = cache[fmt:cache.index("\nuint64_t program_cache_stamp()", fmt)]
    written = set(re.findall(r"put(?:_list)?\(o, p\.(\w+)\)", body(cache, "void put_head(")))
    save = body(cache, "void program_cache_save(")
    stage = set(re.findall(r"put(?:_list)?\(o, st\.(\w+)\)", save))
    if not written or not stage:
        raise SystemExit("FAIL: put_head or program_cache_save no longer writes as this test reads it")
    for member in sorted(written):
        if "Program::" + member not in asserts:
            raise SystemExit("FAIL: put_head writes Program::%s as it lies, and no static_assert holds its layout" % member)
    for member in sorted(stage - {"info"}):   # (Shader::Info: the recompiler's, in the translator stamp)
        if "Stage::" + member not in asserts:
            raise SystemExit("FAIL: program_cache_save writes Stage::%s as it lies, and no static_assert holds its layout" % member)
    if "sizeof(Program::Answer) ==" not in asserts or "offsetof(Program::Answer" not in asserts:
        raise SystemExit("FAIL: Program::Answer (the answers, written whole) has no size and offset asserts")
    print("PASS: the program cache survives edits to its own reads, and goes with the translator or its format")
    return 0


if __name__ == "__main__":
    sys.exit(main())
