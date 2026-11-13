"""OoT's tree-shadow mask is evened out over frames where the TAAU network is left out (#477, rebased as #1581).

The tree-shadow pass 3427a1dbefd5e02c offsets each pixel's shadow lookup by a normal it rebuilds from a gather of the
neighbouring depths; on grass blades and leaf edges those neighbours flip with the camera's sub-pixel shift, and the
pixels flip between lit and shadowed every frame (thin streaks over sunlit ground). The console's TAAU network averages
that away; a card that runs the stand-in instead saw it. Right after that draw its mask is blended with its own history
(shaders/shadow_stab.comp), RX_VK_MASK_BLEND=0 turning it off.

The pass and the two stand-ins push descriptors of their own layouts, so each marks bind_descriptors' cache of the last
push stale (descriptors_disturbed): without it, the next dispatch with the cached layout kept their bindings (a device
lost after a save load).

python -I -B tests/test_tree_shadow_mask.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def body(src, head):
    """The function starting at `head`, up to its closing brace at column 0."""
    start = src.index(head)
    return src[start:src.index("\n}\n", start)]


def main():
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    if '#include "shaders/shadow_stab_comp_spv.h"' not in draw:
        raise SystemExit("the mask blend's shader is not compiled in")
    for f in ("shadow_stab.comp", "shadow_stab_comp_spv.h"):
        if not (VK / "shaders" / f).exists():
            raise SystemExit("gpu/vk/shaders/%s is missing" % f)
    if "SHADOW_STAB_COMP_SPV" not in (VK / "shaders" / "shadow_stab_comp_spv.h").read_text(encoding="utf-8"):
        raise SystemExit("the shader header does not define SHADOW_STAB_COMP_SPV")

    weight = body(draw, "float mask_weight()")
    if '"RX_VK_MASK_BLEND"' not in weight or ": 0.0f, 0.0f, 0.95f" not in weight:
        raise SystemExit("the blend weight is not RX_VK_MASK_BLEND, off by default (a GPU hang near Hyrule Castle), 0 to 0.95")

    # (the tree-shadow pass's draw sets the guard, and only when the weight is on: 0 leaves the frame as main draws it)
    guard = draw[draw.index("struct MaskGuard"):]
    guard = guard[:guard.index("record_draw(")]
    if "p.key == 0x3427a1dbefd5e02cull" not in guard or "mask_weight(); mw > 0" not in guard:
        raise SystemExit("the mask blend does not follow the tree-shadow pass, or ignores RX_VK_MASK_BLEND=0")
    if "~MaskGuard() { if (cmd) mask_stabilize(" not in guard:
        raise SystemExit("the mask is not evened out at the end of that draw")

    # (the pass needs a storage view of an RGBA8 mask and push descriptors; anything else is left as drawn)
    stab = body(draw, "void mask_stabilize(")
    for needle, why in (
        ("tex->format != VK_FORMAT_R8G8B8A8_UNORM", "a mask of another format is blended"),
        ("VK_IMAGE_USAGE_STORAGE_BIT", "a mask without storage use is blended"),
        ("!dev.push_descriptor", "the pass runs without push descriptors"),
        ("end_pass(cmd);", "the blend runs inside the render pass"),
    ):
        if needle not in stab:
            raise SystemExit(why)

    # (every push of a layout of its own marks the cache stale (descriptors_disturbed) before its function ends)
    own = [m.end() for m in re.finditer(r"vkCmdPushDescriptorSetKHR\(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, ", draw)]
    stale = [at for at in own if "descriptors_disturbed();" in draw[at:draw.index("\n}\n", at)]]
    if len(own) < 3 or len(stale) != len(own):
        raise SystemExit("a push of the stand-ins' or the mask's own layout leaves bind_descriptors' cache as it was "
                         "(%d of %d marked stale)" % (len(stale), len(own)))
    print("tree-shadow mask blend and the push-descriptor cache ok")


if __name__ == "__main__":
    main()
