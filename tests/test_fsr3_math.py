"""AMD FSR 3.1's inputs without a game or GPU: its camera read from the scene's projection (gpu/vk/fsr3_math.h, checked by
behaviour in tests/test_fsr3_math.cpp), and shaders/fsr3_prep_comp_spv.h made from shaders/fsr3_prep.comp as it is now (when
glslangValidator is here), with the push constants vk_fsr3_inputs.cpp pushes.

python -I -B tests/test_fsr3_math.py [--cxx g++]
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    # (fsr3_prep.comp keeps dlss_prep.comp's push constants, which vk_fsr3_inputs.cpp pushes as 84 bytes; dlss_prep.comp's
    # ambient occlusion terms come after them and are DLSS's alone)
    block = lambda text: re.sub(r"\s+", " ", re.sub(r"//[^\n]*", "", text[text.index("uniform P {"):text.index("} pc;")]))
    if block((SHADERS / "fsr3_prep.comp").read_text(encoding="utf-8")) != block((SHADERS / "dlss_prep.comp").read_text(encoding="utf-8")).replace("float ao; float proj[4]; ", ""):
        raise SystemExit("fsr3_prep.comp's push constants are not dlss_prep.comp's")
    inputs = (ROOT / "gpu" / "vk" / "vk_fsr3_inputs.cpp").read_text(encoding="utf-8")
    if "static_assert(sizeof pc == 84" not in inputs or "{VK_SHADER_STAGE_COMPUTE_BIT, 0, 84}" not in inputs:
        raise SystemExit("vk_fsr3_inputs.cpp does not push fsr3_prep.comp's 84 bytes")
    glslang = shutil.which("glslangValidator")
    with tempfile.TemporaryDirectory(prefix="fsr3-math-") as tmp:
        if glslang:
            out = Path(tmp) / "fsr3_prep_comp_spv.h"
            subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "--vn", "FSR3_PREP_COMP_SPV", "-o", str(out),
                            str(SHADERS / "fsr3_prep.comp")], check=True, capture_output=True)
            words = lambda t: re.findall(r"0x[0-9a-f]{8}", t)
            if words(out.read_text()) != words((SHADERS / out.name).read_text()):
                raise SystemExit("shaders/fsr3_prep_comp_spv.h is not made from fsr3_prep.comp as it is now (glslangValidator -V "
                                 "--target-env vulkan1.3 --vn FSR3_PREP_COMP_SPV -o fsr3_prep_comp_spv.h fsr3_prep.comp)")
        else:
            print("glslangValidator not found: the SPIR-V header not checked")
        exe = Path(tmp) / "fsr3-math"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_fsr3_math.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
