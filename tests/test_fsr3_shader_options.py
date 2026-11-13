"""The options scripts/fsr3_shaders.py compiles the FSR 3.1 passes under are names the vendored SDK reads.

Each -D the script passes is a promise about the inputs (the depth is reversed, the motion is at the render size), and
AMD's GLSL keeps it only where an #if asks for that exact name. A misspelt one is dropped without a word: glslang
compiles the pass with the option off, and the checked-in headers still match their sources, so
tests/test_fsr3_shaders.py cannot see it. Frame interpolation was built that way: it was given the upscaler's spelling
(..._LOW_RESOLUTION_MOTION_VECTORS) of its own ..._LOW_RES_MOTION_VECTORS, so fi_reconstruct_and_dilate read the
render-size motion at display-size pixels and smeared the top-left render-size corner of every made frame.

This reads the preprocessor conditions of the SDK files glslang compiles (no glslang needed) and checks every option
name the script passes against them, and that every option the interpolation passes ask about is one the script sets.

Run with: python -I -B tests/test_fsr3_shader_options.py
"""
import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fsr3_shaders_under_test", ROOT / "scripts" / "fsr3_shaders.py")
fsr3_shaders = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fsr3_shaders)

CONDITION = re.compile(r"^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif)\b(.*)$", re.M)
IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)


def asked(*files):
    """Every identifier an #if, #ifdef, #ifndef or #elif in the files tests"""
    names = set()
    for path in files:
        text = path.read_text(encoding="utf-8").replace("\\\n", " ")
        for m in CONDITION.finditer(text):
            names.update(IDENTIFIER.findall(m.group(1)))
    return names


def compiled_files(passes):
    """The SDK files a family's passes compile from: its pass GLSL and every header they include, followed through
    their own includes (looked up beside the including file, then in the GPU headers' folder and its family folders,
    as the script's -I paths do). Only this family's files, so an option of another family's spelling passed to it
    is caught too. AMD's CMake lists are not compiled, and the _hlsl.h callbacks are the HLSL build's (FFX_GLSL picks
    the _glsl.h ones)"""
    gpu = fsr3_shaders.GPU
    roots = [gpu] + sorted(d for d in gpu.iterdir() if d.is_dir())
    todo, seen = sorted(passes.glob("*.glsl")), set()
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        for name in INCLUDE.findall(path.read_text(encoding="utf-8")):
            if name.endswith("_hlsl.h"):
                continue
            for base in [path.parent] + roots:
                found = (base / name).resolve()
                if found.is_file():
                    todo.append(found)
                    break
    return sorted(seen)


def defined(options):
    """The NAME of each -DNAME or -DNAME=value"""
    return [o[2:].split("=", 1)[0] for o in options if o.startswith("-D")]


class OptionNameTests(unittest.TestCase):
    def test_every_option_is_one_the_sdk_reads(self):
        families = [
            ("upscaler", fsr3_shaders.OPTIONS + ["-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0"]
             + [d for _, _, extra in fsr3_shaders.VARIANTS for d in extra], fsr3_shaders.PASSES),
            ("frame interpolation", fsr3_shaders.FI_OPTIONS
             + [d for _, _, extra in fsr3_shaders.FI_VARIANTS for d in extra], fsr3_shaders.FI_PASSES),
            ("optical flow", fsr3_shaders.OF_OPTIONS
             + [d for _, _, extra in fsr3_shaders.OF_VARIANTS for d in extra], fsr3_shaders.OF_PASSES),
        ]
        for family, options, passes in families:
            read = asked(*compiled_files(passes))
            self.assertTrue(read, "%s: no #if found under %s" % (family, passes))
            for name in defined(options):
                with self.subTest(family=family, option=name):
                    self.assertTrue(name in read, "%s: scripts/fsr3_shaders.py passes -D%s, which no #if in the SDK "
                                    "files its passes compile reads: a misspelt option is dropped without a word"
                                    % (family, name))

    def test_every_interpolation_option_is_set(self):
        read = {n for n in asked(*compiled_files(fsr3_shaders.FI_PASSES))
                if n.startswith("FFX_FRAMEINTERPOLATION_OPTION_")}
        self.assertTrue(read)
        self.assertEqual(sorted(read - set(defined(fsr3_shaders.FI_OPTIONS))), [],
                         "options the interpolation passes ask about that scripts/fsr3_shaders.py leaves unset")

    def test_interpolation_motion_is_render_size(self):
        """The motion vk_fsr3fi.cpp hands the passes is the render size's (vk_fsr3fi.h, Inputs), so the one pass that
        reads it, reconstruct-and-dilate, is compiled with the SDK's low-resolution option on"""
        header = (fsr3_shaders.GPU / "frameinterpolation"
                  / "ffx_frameinterpolation_reconstruct_dilated_velocity_and_previous_depth.h")
        self.assertIn("FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS", asked(header))
        self.assertIn("-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1", fsr3_shaders.FI_OPTIONS)


if __name__ == "__main__":
    unittest.main()
