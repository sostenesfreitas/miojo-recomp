/* (hookshot) How much VRAM the game's memory pools may be mirrored in (vk_memory.cpp mirror_budget), by the card: Auto's rule, and
   the cap on what a Quality preset asks. No Vulkan here. tests/test_mirror_budget.py */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::mirror_rule {

/// What a card counts as small: 4.5 GB or less, where Auto gives an eighth
inline bool small_card(uint64_t vram) { return vram && vram <= (4ull << 30) + (512ull << 20); }

/// Auto's mirrors: a quarter of the card, at most 3 GB, and an eighth on a card of 4.5 GB or less (512 MB on 4 GB). A pool left out
/// is read by the GPU over PCIe: on 8 GB cards, a 3070 or a 4060, that left the GPU at a few percent and the frame stuttering, and
/// on a 6 GB RTX 3060 Laptop at an eighth (768 MB) or the Low preset's 512 MB, 30-32 GB of bound ranges went over PCIe every 600
/// submits in Hyrule Field, the GPU's frame 27 ms against 19 at a quarter. 6 GB cards had an eighth because, with a quarter, the
/// mirrors and the textures went past what the card has; the image blocks made at the start were then 896 MB of it held empty,
/// and now they are made in the textures' pool, where the textures take them (vram_compact.h blocks_at_start). The mirrors are
/// not squeezed when VRAM runs short (RX_VK_MIRROR_TIGHT is opt-in): the texture cache is. On that laptop at the Low preset's
/// own render size the process then used 95% of what the driver allows (82% at 512 MB, 89% at 1024), the textures it holds
/// about the same (1.56 GB against 1.62), 39 fps against 18, and 31 at 1024, where the pools are filled first come, first served
inline uint64_t auto_bytes(uint64_t vram) {
    if (!vram) return 3072ull << 20;
    if (small_card(vram)) return vram / 8;
    return std::min<uint64_t>(vram / 4, 3072ull << 20);
}

/// The mirrors a budget asks for, by the card. Set by a Quality preset (by_preset): never more than Auto gives the card, so a
/// preset made for a larger card (Low's 1536 MB on a 4 GB one, preset_caps_memory lets Low's budgets through up to 6.75 GB) keeps
/// a small card at Auto's. Chosen on the VRAM budget row itself: the player's, except on a small card, where it is capped at Auto's
/// too (the row's 3.6 GB step on a 4 GB card was 512 MB before Low's mirrors went to 1536; its steps of 4.8 GB and more, past
/// what such a card has, now keep 512 as well)
inline uint64_t budget_bytes(uint64_t asked, bool by_preset, uint64_t vram) {
    return vram && (by_preset || small_card(vram)) ? std::min(asked, auto_bytes(vram)) : asked;
}

}  // namespace mkwvk::mirror_rule
