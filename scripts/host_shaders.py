"""Compile the renderer's own texture shaders (gpu/vk/host_shaders/*.comp) into the SPIR-V headers gpu/vk/vk_texture.cpp
includes.

The .comp files are Eglantine's (nx2), with their authors and licenses in each header's opening comment. The headers
are checked in, so a clone builds without glslang. This script makes them again from the sources, keeping each header's
opening comment, and --check proves the checked-in SPIR-V still matches its source (tests/test_host_shaders.py).

    python -I -B scripts/host_shaders.py            # write gpu/vk/host_shaders/*_comp_spv.h
    python -I -B scripts/host_shaders.py --check    # compare instead of writing

glslang 16.6.0 (the version in the headers' first line) gives the checked-in bytes exactly. Another glslang may emit
different but equivalent SPIR-V: --check reports that as a note, not a failure, since it says nothing about the sources.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIR = ROOT / "gpu/vk/host_shaders"
SHADERS = ["astc_decoder", "bc7_encoder", "bc_encoder", "block_linear_unswizzle_2d"]
TARGET_ENV = "vulkan1.1"   # (as Eden builds them: the bytes match only with this target)


def words(text):
    """The SPIR-V words of a glslang --vn header"""
    return re.findall(r"0x[0-9a-fA-F]{8}", text[text.index("{"):])


def generator(text):
    """glslang's version line in a --vn header (`// 1116.6.0`), or None"""
    m = re.search(r"^\t// (\d+\.\d+\.\d+)$", text, re.M)
    return m.group(1) if m else None


def opening(text):
    """A header's own comment: everything above glslang's output, which starts at its tab-indented version line"""
    i = text.find("\t// ")
    return text[:i] if i > 0 else ""


def compile_one(glslang, name, out):
    subprocess.run([glslang, "-V", "--quiet", "--target-env", TARGET_ENV, "--vn", name.upper() + "_COMP_SPV",
                    "-o", str(out), str(DIR / (name + ".comp"))], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--glslang", default=None, help="glslangValidator; found on PATH by default")
    p.add_argument("--check", action="store_true", help="compare with the checked-in headers instead of writing")
    a = p.parse_args()
    glslang = a.glslang or shutil.which("glslangValidator") or shutil.which("glslang")
    if not glslang:
        sys.exit("glslangValidator not found; pass --glslang")

    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        for name in SHADERS:
            header = DIR / (name + "_comp_spv.h")
            made = Path(tmp) / header.name
            compile_one(glslang, name, made)
            new = made.read_text()
            old = header.read_text() if header.exists() else ""
            if a.check:
                if words(new) == words(old):
                    continue
                if generator(new) != generator(old):
                    print(f"{header.name}: glslang {generator(new)} gives different SPIR-V from the checked-in "
                          f"{generator(old)}'s; not a source mismatch (use glslang {generator(old)} to compare)",
                          file=sys.stderr)
                    continue
                bad.append(header.name)
            else:
                header.write_text(opening(old) + new, newline="\n")
    if bad:
        sys.exit("host shaders out of date with their .comp sources: %s (python -I -B scripts/host_shaders.py)"
                 % ", ".join(bad))
    print(f"host shaders: {len(SHADERS)} {'match' if a.check else 'written'}")


if __name__ == "__main__":
    main()
