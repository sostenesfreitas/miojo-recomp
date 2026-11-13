"""The temporal upscalers' reactive mask (gpu/vk/vk_reactive.h): which of the scene's draws are its transparent ones and
when the scene is copied before them (gpu/vk/reactive_policy.h, built as C++ and run), that vk_draw.cpp copies outside a
pass and only for the jittered scene, that both upscalers are given the mask and run as before without one, and the
shader's SPIR-V header made from its source as it is now (when glslangValidator is here). No game or GPU required.

python -I -B tests/test_reactive_mask.py [--cxx g++]
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def function(source, start):
    """A function's body from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    # which draws: the scene the upscaler last took, by the jitter's sizes, and only when asked for
    hook = function(draw, "void reactive_scene_draw(VkCommandBuffer cmd, const Program& p, const NvnState& s, const Targets& at) {")
    if hook.index("if (!reactive::wanted()) return;") > hook.index("texture_frame_no()"):
        raise SystemExit("reactive_scene_draw: work is done for every draw before RX_VK_REACTIVE is asked")
    for part in ("temporal_jitter_size(0)", "fsr3_inputs::jitter_size(0)", "upscaler_rows::is_fsr4(temporal_mode())", "at.width != seen.w || at.height != seen.h", "!at.depth",
                 "at.color_format[0] != VK_FORMAT_B10G11R11_UFLOAT_PACK32",
                 "reactive_policy::classify(target_write_mask(p, s, 0) != 0, (s.color.blend_en & 1) != 0, s.ds.dt != 0, s.ds.dw != 0)"):
        if part not in hook:
            raise SystemExit("reactive_scene_draw: %s is gone (the mask is of the jittered scene's transparent draws alone)" % part)
    # a copy cannot be recorded in a pass
    if "end_pass(cmd);" not in hook or hook.index("end_pass(cmd);") > hook.index("reactive::copy_scene("):
        raise SystemExit("reactive_scene_draw: the scene is copied inside its pass")
    if hook.index("reactive_policy::copy_before(") > hook.index("end_pass(cmd);"):
        raise SystemExit("reactive_scene_draw: the pass is ended for draws that need no copy")
    body = function(draw, "void draw(")
    if "reactive_scene_draw(cmd, p, s, at);" not in body or \
            body.index("reactive_scene_draw(cmd, p, s, at);") > body.index("writes = draw_hazards(cmd, p, s, at);"):
        raise SystemExit("draw: the scene is not copied before the draw's pass is settled (draw_hazards)")

    reactive = (VK / "vk_reactive.cpp").read_text(encoding="utf-8")
    wanted = function(reactive, "bool wanted() {")
    if 'getenv("RX_VK_REACTIVE")' not in wanted or "atoi(e) <= 0) return false;" not in wanted:
        raise SystemExit("reactive::wanted: the mask is on without RX_VK_REACTIVE=1")
    if "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT" not in wanted:
        raise SystemExit("reactive::wanted: R8 storage images are not asked of the device")
    copy = function(reactive, "bool copy_scene(")
    if copy.index("full_barrier(cmd);") > copy.index("vkCmdCopyImage("):
        raise SystemExit("copy_scene: the scene is copied without a barrier after its draws")
    for part in ("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", "color->samples != VK_SAMPLE_COUNT_1_BIT", "texture_destroy_generation()"):
        if part not in copy:
            raise SystemExit("copy_scene: %s is gone" % part)
    mask = function(reactive, "bool mask(")
    for part, why in (("if (!wanted()) return false;", "it is made without RX_VK_REACTIVE"),
                      ("s.copied = false;", "a frame's copy is used by the next frame too"),
                      ("s.scene_gen != texture_destroy_generation()", "a destroyed target may be read"),
                      ("s.width != width || s.height != height", "a copy of another size is read"),
                      ("descriptors_disturbed()", "the program's descriptors are not pushed again after its pass")):
        if part not in mask:
            raise SystemExit("reactive::mask: %s" % why)
    if mask.index("s.scene_gen != texture_destroy_generation()") > mask.index("scene_view(s.scene"):
        raise SystemExit("reactive::mask: the target is read before it is known to be alive")
    # the shader's push constants are the ones vk_reactive.cpp pushes
    shader = (VK / "shaders" / "reactive_mask.comp").read_text(encoding="utf-8")
    block = shader[shader.index("uniform P {"):shader.index("} pc;")]
    fields = re.findall(r"^\s*(\w+) (\w+);", block, re.M)
    if fields != [("ivec2", "size"), ("float", "scale"), ("float", "limit"), ("int", "flags")]:
        raise SystemExit("reactive_mask.comp: its push constants are %s" % fields)
    if "int32_t size[2];\n        float scale, limit;\n        int32_t flags;" not in mask or "sizeof pc == 20" not in mask:
        raise SystemExit("reactive::mask: its push constants are not reactive_mask.comp's")

    # the upscalers: given the mask when there is one, as before without
    fsr = (VK / "vk_fsr3.cpp").read_text(encoding="utf-8")
    if "in.reactive ? Resource{in.reactive} : tex(g.default_reactive)," not in fsr:
        raise SystemExit("vk_fsr3.cpp: the reactivity pass does not take the mask (or has none without it)")
    up = function(fsr, "bool fsr3_upscale(")
    if "reactive::mask(cmd, f.width, f.height, f.width, f.height, f.color, m)) ai.reactive = m.view;" not in up or \
            up.index("reactive::mask(") > up.index("fsr3::upscale(cmd, ai)"):
        raise SystemExit("fsr3_upscale: the mask is not made before FSR's passes")
    # (FSR 3.1 on the temporal chain, vk_fsr3.cpp's engine, its default path: the same mask DLSS gets there)
    chain = function(fsr, "bool chain_evaluate(")
    if "reactive::mask(cmd, f.render_w, f.render_h, f.color.width, f.color.height, f.color.view, m))" not in chain or \
            "ai.reactive = m.view;" not in chain or chain.index("reactive::mask(") > chain.index("fsr3::upscale(cmd, ai)"):
        raise SystemExit("vk_fsr3.cpp chain_evaluate: FSR 3.1 on the chain is not given the mask before its passes")
    dlss = (VK / "vk_dlss.cpp").read_text(encoding="utf-8")
    at = dlss.index("NVSDK_NGX_VK_DLSS_Eval_Params ep{};")
    ev = dlss[at:dlss.index("NGX_VULKAN_EVALUATE_DLSS_EXT(", at)]
    if "reactive::mask(cmd, f.render_w, f.render_h, f.color.width, f.color.height, f.color.view, m)" not in ev or \
            "ep.pInBiasCurrentColorMask = &bias;" not in ev:
        raise SystemExit("vk_dlss.cpp: DLSS's evaluation is not given the mask")

    glslang = shutil.which("glslangValidator")
    with tempfile.TemporaryDirectory(prefix="reactive-mask-") as tmp:
        if glslang:
            out = Path(tmp) / "reactive_mask_comp_spv.h"
            subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "--vn", "REACTIVE_MASK_COMP_SPV", "-o", str(out),
                            str(VK / "shaders" / "reactive_mask.comp")], check=True, capture_output=True)
            words = lambda t: re.findall(r"0x[0-9a-f]{8}", t)
            if words(out.read_text()) != words((VK / "shaders" / out.name).read_text()):
                raise SystemExit("shaders/%s is not made from reactive_mask.comp as it is now (glslangValidator -V --target-env "
                                 "vulkan1.3 --vn REACTIVE_MASK_COMP_SPV -o %s reactive_mask.comp)" % (out.name, out.name))
        else:
            print("glslangValidator not found: the SPIR-V header not checked")
        exe = Path(tmp) / "reactive-mask"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_reactive_mask.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
