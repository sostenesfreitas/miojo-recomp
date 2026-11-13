"""Compile AMD's FSR 3.1 upscaler passes into the SPIR-V headers gpu/vk uses.

The FidelityFX SDK ships each pass as GLSL that includes the SDK's own GPU headers, so the eleven
variants below are the same eight passes compiled with different options. They are checked in like
every other shader in gpu/vk/shaders, so a clone builds without glslang; this script regenerates
them, and --check proves the checked-in files still match their sources (tests/test_fsr3_shaders.py).

    python -I -B scripts/fsr3_shaders.py            # write gpu/vk/shaders/fsr3_*_comp_spv.h
    python -I -B scripts/fsr3_shaders.py --check    # compare instead of writing
"""
import argparse
import difflib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "third_party/fidelityfx-sdk"
GPU = SDK / "include/FidelityFX/gpu"
PASSES = SDK / "shaders/fsr3upscaler"
FI_PASSES = SDK / "shaders/frameinterpolation"
OF_PASSES = SDK / "shaders/opticalflow"
OUT = ROOT / "gpu/vk/shaders"

# The option set the renderer records these passes under. Each one is a promise about the inputs:
# the colour is HDR, the motion is low-resolution and not jittered, and the depth is reversed. They
# have to match what gpu/vk/vk_fsr3.cpp hands the passes, which is why they live in one place.
OPTIONS = [
    "-DFFX_GPU=1",
    "-DFFX_GLSL=1",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1",
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1",
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0",
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1",
]

# name, the pass GLSL it compiles, extra defines. The _nowave variants are for devices without the
# subgroup operations the single-pass downsampler prefers.
VARIANTS = [
    ("prepare_inputs", "prepare_inputs", []),
    ("luma_pyramid", "luma_pyramid", []),
    ("luma_pyramid_nowave", "luma_pyramid", ["-DFFX_SPD_NO_WAVE_OPERATIONS=1"]),
    ("shading_change_pyramid", "shading_change_pyramid", []),
    ("shading_change_pyramid_nowave", "shading_change_pyramid", ["-DFFX_SPD_NO_WAVE_OPERATIONS=1"]),
    ("shading_change", "shading_change", []),
    ("prepare_reactivity", "prepare_reactivity", []),
    ("luma_instability", "luma_instability", []),
    ("accumulate", "accumulate", []),
    ("accumulate_sharpen", "accumulate", ["-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=1"]),
    ("rcas", "rcas", []),
]


# (hookshot) Frame interpolation and the optical flow it runs on, the same arrangement: AMD ships
# each pass as GLSL over the SDK's own GPU headers, and the renderer records them as SPIR-V headers
# beside the upscaler's. The options are the same promises about the same inputs -- the depth is
# reversed and the motion is at the render extent -- because the inputs are the ones the upscaler is
# already given (gpu/vk/vk_fsr3fi.cpp). The names are interpolation's own, not the upscaler's: its
# low-resolution motion option is LOW_RES_, where the upscaler's is LOW_RESOLUTION_. A name the SDK
# does not read is dropped without a word, so tests/test_fsr3_shader_options.py checks each one.
FI_OPTIONS = [
    "-DFFX_GPU=1",
    "-DFFX_GLSL=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS=0",
]
OF_OPTIONS = ["-DFFX_GPU=1", "-DFFX_GLSL=1"]

# (name, the pass file, extra defines). The file is named outright rather than composed: AMD's
# optical-flow v5 passes end "_pass_v5.glsl" and the interpolation pass itself is just
# "ffx_frameinterpolation_pass.glsl", so a composed name is wrong for four of the eighteen.
FI_VARIANTS = [
    ("fi_setup", "ffx_frameinterpolation_setup_pass.glsl", []),
    ("fi_reconstruct_previous_depth", "ffx_frameinterpolation_reconstruct_previous_depth_pass.glsl", []),
    ("fi_reconstruct_and_dilate", "ffx_frameinterpolation_reconstruct_and_dilate_pass.glsl", []),
    ("fi_game_motion_vector_field", "ffx_frameinterpolation_game_motion_vector_field_pass.glsl", []),
    ("fi_optical_flow_vector_field", "ffx_frameinterpolation_optical_flow_vector_field_pass.glsl", []),
    ("fi_disocclusion_mask", "ffx_frameinterpolation_disocclusion_mask_pass.glsl", []),
    ("fi_game_vector_field_inpainting_pyramid",
     "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.glsl", []),
    ("fi_inpainting_pyramid", "ffx_frameinterpolation_compute_inpainting_pyramid_pass.glsl", []),
    ("fi_interpolate", "ffx_frameinterpolation_pass.glsl", []),
    ("fi_inpainting", "ffx_frameinterpolation_inpainting_pass.glsl", []),
]
OF_VARIANTS = [
    ("of_prepare_luma", "ffx_opticalflow_prepare_luma_pass.glsl", []),
    ("of_luminance_pyramid", "ffx_opticalflow_compute_luminance_pyramid_pass.glsl", []),
    ("of_generate_scd_histogram", "ffx_opticalflow_generate_scd_histogram_pass.glsl", []),
    ("of_compute_scd_divergence", "ffx_opticalflow_compute_scd_divergence_pass.glsl", []),
    ("of_compute_optical_flow_v5", "ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl", []),
    ("of_filter_optical_flow_v5", "ffx_opticalflow_filter_optical_flow_pass_v5.glsl", []),
    ("of_scale_optical_flow_v5", "ffx_opticalflow_scale_optical_flow_advanced_pass_v5.glsl", []),
]


def compile_family(glslang, name, source, extra, out_path, target_env, options, includes,
                   variable=True):
    """One frame-interpolation or optical-flow pass. The upscaler's compile_one is kept as it is:
    its passes are composed by name and its option set is the upscaler's own."""
    cmd = [glslang, "-V", "--quiet", "-S", "comp"]
    for d in includes:
        cmd.append("-I" + str(d))
    cmd += [*options, *extra]
    if variable:
        cmd += ["--variable-name", f"FSR3_{name.upper()}_COMP_SPV"]
    cmd += ["-o", str(out_path), str(source), "--target-env", target_env]
    subprocess.run(cmd, check=True)


def compile_one(glslang, name, pass_name, extra, out_path, target_env, variable=True):
    """Compile one variant. With `variable`, glslang writes a C header; without it, binary SPIR-V,
    which is what spirv-val reads."""
    defines = list(OPTIONS)
    if not any("APPLY_SHARPENING" in d for d in extra):
        defines.append("-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0")
    cmd = [glslang, "-V", "--quiet", "-S", "comp",
           "-I" + str(GPU), "-I" + str(GPU / "fsr3upscaler"),
           *defines, *extra]
    if variable:
        cmd += ["--variable-name", f"FSR3_{name.upper()}_COMP_SPV"]
    cmd += ["-o", str(out_path),
            str(PASSES / f"ffx_fsr3upscaler_{pass_name}_pass.glsl"),
            "--target-env", target_env]
    subprocess.run(cmd, check=True)


def strip_generator(text):
    """The header without glslang's own version comment.

    glslang writes its version into the first line as `// 1116.2.0`, so two glslang builds produce
    byte-different headers from identical SPIR-V. Comparing the whole text made this check fail on
    a CI runner whose glslang was one release behind the one that generated the checked-in files --
    a version bump reported as a source mismatch, which is the kind of red that teaches people to
    ignore red. The words after it are the module, and those are what this is about.
    """
    return [line for line in text.splitlines()
            if not line.strip().startswith("// ") or not line.strip()[3:].replace(".", "").isdigit()]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--glslang", default=None, help="glslangValidator; found on PATH by default")
    p.add_argument("--spirv-val", default=None, help="spirv-val, to validate each module")
    p.add_argument("--target-env", default="spirv1.3")
    p.add_argument("--check", action="store_true", help="compare with the checked-in headers")
    a = p.parse_args()
    glslang = a.glslang or shutil.which("glslangValidator") or shutil.which("glslang")
    if not glslang:
        print("glslangValidator not found; pass --glslang", file=sys.stderr)
        return 2

    bad = []
    with tempfile.TemporaryDirectory(prefix="fsr3-shaders-") as tmp:
        # the upscaler's eleven, then frame interpolation's ten and optical flow's seven
        families = [(name, None, pass_name, extra, None, None) for name, pass_name, extra in VARIANTS]
        families += [(name, FI_PASSES / f, None, extra, FI_OPTIONS,
                      [GPU, GPU / "frameinterpolation", GPU / "opticalflow"])
                     for name, f, extra in FI_VARIANTS]
        families += [(name, OF_PASSES / f, None, extra, OF_OPTIONS, [GPU, GPU / "opticalflow"])
                     for name, f, extra in OF_VARIANTS]
        for name, source, pass_name, extra, options, includes in families:
            header = OUT / f"fsr3_{name}_comp_spv.h"
            made = Path(tmp) / header.name
            if source is None:
                compile_one(glslang, name, pass_name, extra, made, a.target_env)
            else:
                compile_family(glslang, name, source, extra, made, a.target_env, options, includes)
            if a.spirv_val:
                spv = Path(tmp) / f"{name}.spv"
                if source is None:
                    compile_one(glslang, name, pass_name, extra, spv, a.target_env, variable=False)
                else:
                    compile_family(glslang, name, source, extra, spv, a.target_env, options,
                                   includes, variable=False)
                subprocess.run([a.spirv_val, str(spv)], check=True)
            if a.check:
                have = header.read_text() if header.exists() else ""
                want = made.read_text()
                if strip_generator(have) != strip_generator(want):
                    bad.append(name)
                    if not have:
                        print(f"{header.name}: missing", file=sys.stderr)
                    else:
                        diff = difflib.unified_diff(have.splitlines(), want.splitlines(),
                                                    "checked in", "regenerated", n=0, lineterm="")
                        print("\n".join(list(diff)[:12]), file=sys.stderr)
            else:
                shutil.copyfile(made, header)
                print(f"wrote {header.relative_to(ROOT)} ({header.stat().st_size} bytes)")
    if bad:
        print(f"{len(bad)} header(s) do not match their source: {', '.join(bad)}", file=sys.stderr)
        return 1
    print(f"fsr3 shaders: {len(VARIANTS) + len(FI_VARIANTS) + len(OF_VARIANTS)} {'match' if a.check else 'written'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
