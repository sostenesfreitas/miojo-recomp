"""The late pages' fix's shaders (gpu/vk/shaders/late_fix.comp, and wb_diff.comp, whose stamps it reads): their tables'
entries against the structs vk_memory.cpp writes (late_fix.comp's Fix and LateFix, wb_diff.comp's Chunk and WbdChunk: the
same fields, of the same sizes, in the same order), and the SPIR-V headers made from the sources as they are now (when
glslangValidator is here; else that part is skipped and says so). No game or GPU.

python -I -B tests/test_late_fix_shaders.py [--glslang glslangValidator]
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"
SIZES = {"uint64_t": 8, "uint32_t": 4, "uvec2": 8, "uint": 4}


def fields(text, start, end="}"):
    """The (name, bytes) of a struct's fields: uint64_t a, b; uint c; -> [('a', 8), ('b', 8), ('c', 4)]"""
    body = text[text.index(start) + len(start):]
    body = body[:body.index(end)]
    body = re.sub(r"//[^\n]*", "", body)
    out = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        kind, names = decl.split(None, 1)
        if kind not in SIZES:
            raise SystemExit("%s: a field of a type this check does not know: %s" % (start, decl))
        out += [(n.strip(), SIZES[kind]) for n in names.split(",")]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--glslang", default=shutil.which("glslangValidator"))
    args = parser.parse_args()
    memory = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    for comp, glsl_struct, cpp_struct in (("late_fix.comp", "struct Fix {", "struct LateFix {"),
                                          ("wb_diff.comp", "struct Chunk {", "struct WbdChunk {")):
        got = fields((SHADERS / comp).read_text(encoding="utf-8"), glsl_struct)
        want = fields(memory, cpp_struct)
        if got != want:
            raise SystemExit("shaders/%s's %s is not vk_memory.cpp's %s: %s against %s" % (comp, glsl_struct[7:-2],
                                                                                         cpp_struct[7:-2], got, want))
    if not args.glslang:
        print("glslangValidator not found: the SPIR-V headers not checked")
        return
    with tempfile.TemporaryDirectory(prefix="late-fix-shaders-") as tmp:
        for src in ("late_fix.comp", "wb_diff.comp"):
            var = src.replace(".", "_").upper() + "_SPV"
            out = Path(tmp) / (src.replace(".", "_") + "_spv.h")
            subprocess.run([args.glslang, "-V", "--target-env", "vulkan1.3", "--vn", var, "-o", str(out), str(SHADERS / src)],
                           check=True, capture_output=True)
            words = lambda t: re.findall(r"0x[0-9a-f]{8}", t)
            if words(out.read_text()) != words((SHADERS / out.name).read_text()):
                raise SystemExit("shaders/%s is not made from %s as it is now (glslangValidator -V --target-env vulkan1.3 "
                                 "--vn %s -o %s %s)" % (out.name, src, var, out.name, src))
    print("PASS: late_fix.comp's and wb_diff.comp's tables match vk_memory.cpp, their SPIR-V headers their sources")


if __name__ == "__main__":
    main()
