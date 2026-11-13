"""The translator is told the truth about a GPU whose graphics stages run wider than NVN's 32-lane warps.

AMD's GPUs pin only compute to 32 lanes (requiredSubgroupSizeStages 0x20 on an RX 5500M): a fragment stage runs at 64, so the
profile asks for the long path that keeps each 32-lane guest warp to its half (warp_size_potentially_larger_than_guest), and
the broadcast fallbacks walk all the host's lanes. On AMD's proprietary driver fragment-stage shuffles take the broadcast
fallback (Lichii #273: a basket in Link's house drew as a white blob), and a depth-compare texture is never an integer image
(#273: the shadow samples of the lighting). The profile's choices are in the program cache's stamp.

The long path is per stage: a graphics stage the GPU pins to 32 lanes as well (RDNA2/RDNA3 pin fragment, task and mesh:
requiredSubgroupSizeStages 0xf0) keeps main's short path, so only the stages that really run at 64 lanes change.

python -I -B tests/test_amd_fragment_warp.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def _with_cache(path):
    """vk_shader.cpp with the program cache it held before (vk_progcache.cpp and .h beside it, outside the translator stamp): definitions first"""
    parts = [path.read_text(encoding="utf-8")]
    for name in ("vk_progcache.cpp", "vk_progcache.h"):
        if (path.parent / name).exists():
            parts.append((path.parent / name).read_text(encoding="utf-8"))
    return "\n".join(parts)

VK = ROOT / "gpu" / "vk"
SPIRV = VK / "eden" / "shader_recompiler" / "backend" / "spirv"


def main():
    shader = _with_cache(VK / "vk_shader.cpp")
    profile = shader[shader.index("Shader::Profile make_profile()"):shader.index("Shader::HostTranslateInfo host_info()")]
    for needle, why in (
        ("p.warp_size_potentially_larger_than_guest = dev.subgroup_max > 32",
         "an unpinned stage's maximum subgroup width is not told to the translator"),
        ("p.max_subgroup_size = p.warp_size_potentially_larger_than_guest ? dev.subgroup_max : 32",
         "the broadcast fallbacks do not walk the host's upper lanes"),
        ("p.has_broken_spirv_fragment_shuffle = dev.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY",
         "AMD's proprietary driver keeps native fragment-stage shuffles"),
    ):
        if needle not in profile:
            raise SystemExit(why)
    # (stages pinned to 32 lanes keep the short path: the long one in every compute program was new, longer SPIR-V that AMD
    # compiled for hours, and they need none of it)
    if "EmitSPIRV(profile_pinned(), rt, t->program, bindings)" not in shader:
        raise SystemExit("compute programs take the long warp path though they are pinned to 32 lanes")
    if shader.count("EmitSPIRV(fallback ? profile_pinned() : profile_graphics(") != 2:
        raise SystemExit("the mesh fallback's compute stages take the long warp path though they are pinned to 32 lanes")
    # (per graphics stage: pinned ones keep the short path, as on main; RX_VK_WARP64 still sets them all)
    graphics = shader[shader.index("const Shader::Profile& profile_graphics("):]
    graphics = graphics[:graphics.index("\n}\n")]
    for needle, why in (
        ("q.warp_size_potentially_larger_than_guest = false, q.max_subgroup_size = 32", "a pinned graphics stage takes the long path"),
        ("!forced && (dev.sg32_stages & stage) ? pinned : profile()", "the stage's pinning does not pick its path"),
        ('getenv("RX_VK_WARP64")', "RX_VK_WARP64 no longer sets every graphics stage"),
    ):
        if needle not in graphics:
            raise SystemExit(why)
    if "EmitSPIRV(profile()," in shader:
        raise SystemExit("a stage is translated with the shared profile, not its own stage's")
    for stage, n in (("VK_SHADER_STAGE_VERTEX_BIT", 2), ("VK_SHADER_STAGE_FRAGMENT_BIT", 2), ("VK_SHADER_STAGE_TASK_BIT_EXT", 1),
                     ("VK_SHADER_STAGE_MESH_BIT_EXT", 1)):
        if shader.count("profile_graphics(%s)" % stage) != n:
            raise SystemExit("the %s programs do not take their stage's profile" % stage)
    stamp = shader[shader.index("uint64_t translator_stamp()"):]
    if "profile().warp_size_potentially_larger_than_guest" not in stamp.split("return")[0]:
        raise SystemExit("programs translated for another GPU's warps are kept in the program cache")
    if "(uint32_t)dev.sg32_stages" not in stamp.split("return")[0]:
        raise SystemExit("programs translated for another GPU's pinned stages are kept in the program cache")
    main_cpp = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    if "dev.driver_id = drv.driverID" not in main_cpp or "dev.subgroup_size = sg.subgroupSize" not in main_cpp:
        raise SystemExit("the driver or its subgroup size is not read")
    warp = (SPIRV / "emit_spirv_warp.cpp").read_text(encoding="utf-8")
    if "ctx.profile.has_broken_spirv_fragment_shuffle && ctx.stage == Stage::Fragment" not in warp:
        raise SystemExit("fragment-stage shuffles do not take the broadcast fallback")
    if warp.count("UseShuffleFallback(ctx)") < 3:
        raise SystemExit("absolute, relative and butterfly shuffles do not all take the fallback")
    # (not the quad swap: a program with quad shuffles alone has no lane id input nor ballot capability for it)
    quad = warp[warp.index("Id EmitQuadSwap("):]
    if "UseShuffleFallback(ctx)" in quad[:quad.index("\n}\n")]:
        raise SystemExit("the quad swap takes the broadcast fallback, which its programs cannot have")
    texture = (VK / "eden" / "shader_recompiler" / "ir_opt" / "texture_pass.cpp").read_text(encoding="utf-8")
    if "!flags.is_depth && is_texture_pixel_format_integer(cbuf)" not in texture:
        raise SystemExit("a depth-compare texture can be an integer image")
    print("AMD fragment warps, shuffles and depth-compare textures ok")


if __name__ == "__main__":
    main()
