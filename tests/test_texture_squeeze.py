"""The 97% driver's-limit squeeze waits while VMA's blocks hold room.

At the driver's limit, texture_frame set the texture budget below what the textures held every 10 frames. Evicting frees
space inside VMA's blocks, which the driver still counts, so the count stayed at the limit and the budget shrank again: a
ratchet. On a 4 GB RX 5500M a paused game (its scene draws nothing) lost 1700 MB of textures in seconds, made them again on
unpausing (a long wait), and the HUD's GPU-drawn hearts came back as black glyphs until an area change. The rule now waits
while the blocks hold an eighth of the allowance free, but only while the driver's count is within the allowance: past it the
driver pages VRAM out to system memory (#2156). RX_VK_SQUEEZE_ALWAYS=1 squeezes as before.

python -I -B tests/test_texture_squeeze.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def main():
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    compact = (VK / "vram_compact.h").read_text(encoding="utf-8")
    if "const bool room_in_blocks" not in tex:
        raise SystemExit("the 97% squeeze runs whatever room VMA's blocks hold (a ratchet that drains a paused game's textures)")
    squeeze = tex[tex.index("const bool room_in_blocks"):]
    squeeze = squeeze[:squeeze.index("budget = std::min")]
    if ("vram_compact::room_in_blocks(used, allowed, block_free, integrated)" not in squeeze
            or "&& !room_in_blocks)" not in squeeze):
        raise SystemExit("the 97% squeeze runs whatever room VMA's blocks hold (a ratchet that drains a paused game's textures)")
    rule = compact[compact.index("inline bool room_in_blocks("):]
    rule = rule[:rule.index("}")]
    if "block_free >= allowed / 8" not in rule:
        raise SystemExit("the 97% squeeze runs whatever room VMA's blocks hold (a ratchet that drains a paused game's textures)")
    if "used <= allowed" not in rule:
        raise SystemExit("the 97% squeeze waits for room in VMA's blocks even past the driver's allowance, where the driver "
                         "pages VRAM out to system memory (#2156: 7602 MB used of 6982, 14-25 fps)")
    if "!integrated" not in rule:
        raise SystemExit("the squeeze waits for room in VMA's blocks on an integrated GPU, where the driver's own allocations "
                         "and the staging arenas grow past it (UHD Graphics: 6709 of 7172 MB, 1121 MB free in the blocks, "
                         "then 7.5 of 7.0 GB at Hyrule Castle)")
    refused = tex[tex.index("st_driver_out_of_memory.load("):]
    refused = refused[:refused.index("refusals_seen = refusals;")]
    if ("vram_compact::short_excess(used, count_line)" not in refused or "!(kept_only && !squeeze_always)" not in refused
            or "rule_budget = std::min(rule_budget" not in refused):
        raise SystemExit("the textures do not give memory up when the driver refuses a pipeline for want of it (UHD Graphics: "
                         "six refused, then a fault in igvk64.dll)")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    for held_fn in ("note_out_of_memory(guarded_graphics(cache, ci, pl))", "note_out_of_memory(guarded_compute(cache, ci, pl))"):
        if held_fn not in draw:
            raise SystemExit("vk_draw.cpp: a pipeline refused for want of memory is not counted: " + held_fn)
    if "!(kept_only && !squeeze_always)" not in squeeze:
        raise SystemExit("the 97% squeeze runs while the textures' pool is down to its blocks made at the start, past the "
                         "allowance too, where no eviction can bring the count down (#2711)")
    if ("static const bool integrated = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;" not in tex
            or "vram_compact::count_line_pct(integrated)" not in tex
            or "used > count_line && !room_in_blocks)" not in squeeze or "const uint64_t over = used - count_line;" not in tex):
        raise SystemExit("the driver's-limit squeeze holds an integrated GPU at a dedicated card's 97%, where its driver's own "
                         "allocations fail past the allowance (UHD Graphics at Hyrule Castle: 7.5 of 7.0 GB, a fault in igvk64.dll)")
    if 'env_on("RX_VK_SQUEEZE_ALWAYS")' not in tex:
        raise SystemExit("RX_VK_SQUEEZE_ALWAYS=1 does not bring the old squeeze back")
    if "room in the blocks: not squeezed" not in tex:
        raise SystemExit("the VRAM log line does not say when the squeeze waited")
    print("the driver's-limit squeeze waits while VMA's blocks hold room: ok")


if __name__ == "__main__":
    main()
