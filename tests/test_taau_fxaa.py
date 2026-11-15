"""The gameplay TAAU stand-in's FXAA: on for the stand-in only, its push-constant bits kept apart, the SPIR-V current.

shaders/taau_upscale.comp reads pc.valid as bits: 1 the history bound, 2 FXAA, 4 its diagnostic colours. The bound must
test bit 1 alone (FXAA's bit would otherwise turn the bound on, which smears what moves), and only the gameplay stand-in
(typed_upscale's temporal call) sets FXAA's bits. When glslangValidator of the header's version is found, the header is
compiled again from the source and compared, so the two cannot drift apart.
python -I -B tests/test_taau_fxaa.py
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"


def main():
    comp = (SHADERS / "taau_upscale.comp").read_text(encoding="utf-8")
    if "(pc.valid & 1) != 0" not in comp or re.search(r"pc\.valid\s*!=\s*0", comp):
        raise SystemExit("the history bound does not test its own bit: FXAA's bit would turn it on")
    if "(pc.valid & 2) != 0" not in comp or "fxaa(" not in comp:
        raise SystemExit("the stand-in has no FXAA behind its bit")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    sets = re.findall(r"if \(([^)]*)\) size\[2\] \|= [^;]*;", draw)
    if not sets or any("temporal" not in c or "fxaa" not in c for c in sets):
        raise SystemExit("FXAA's bits are set outside the gameplay stand-in (typed_upscale's temporal call)")
    if 'getenv("RX_VK_TAAU_FXAA")' not in draw:
        raise SystemExit("RX_VK_TAAU_FXAA does not switch it")

    header = (SHADERS / "taau_upscale_comp_spv.h").read_text(encoding="utf-8")
    glslang = shutil.which("glslangValidator")
    generated = header.split('\n', 1)[1] if header.startswith('// Generated from') else header
    version = generated.split("\n", 1)[0].strip().lstrip("/ ").strip()
    if glslang:
        got = subprocess.run([glslang, "--version"], capture_output=True, text=True).stdout
        m = re.search(r"Glslang Version: \d+:(\d+)\.(\d+)\.(\d+)", got)
        same = m and version == "11%02d.%s.%s" % (int(m.group(1)), m.group(2), m.group(3))
        if same:
            with tempfile.TemporaryDirectory() as d:
                out = Path(d) / "h.h"
                subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "--vn", "TAAU_UPSCALE_COMP_SPV", "-o", str(out),
                                str(SHADERS / "taau_upscale.comp")], check=True, capture_output=True)
                if out.read_text(encoding="utf-8") != generated:
                    raise SystemExit("taau_upscale_comp_spv.h is not taau_upscale.comp compiled: run glslangValidator again")
            print("(the SPIR-V header matches its source)")
        else:
            print("(glslangValidator is not version %s: the header was not compiled again)" % version)
    print("PASS: the TAAU stand-in's FXAA is the gameplay stand-in's alone, behind its own bits")


if __name__ == "__main__":
    main()
