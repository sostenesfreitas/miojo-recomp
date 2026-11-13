// (hookshot) The compaction's rules (on unless RX_VK_COMPACT=0; vk_texture.cpp's compact_step and the texture budget), apart from Vulkan so
// that tests/test_vram_compact.py can run them.
//
// The driver counts VMA's blocks whole. An evicted texture's memory stays inside its 128 MB block, so the count keeps what the
// cache gave up: at DLAA 2560 x 1440 on a 12 GB card the count sat at 10.0 of the 10.9 GB the driver allows with 1.4-1.7 GB of it
// free inside the blocks. There the GPU's frame went from ~10 ms to 13-20 ms (the driver itself short, and the write-back
// shadows given back at 95%): 34-49 frames over 33 ms in a five-minute walk across Hyrule Field, a 1% low of 26-29 fps.
// Compacted, the textures' free space goes back to the driver, and the budget can hold the count below that.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace vram_compact {

/// Whether the textures are compacted, by RX_VK_COMPACT as the environment has it: on when it is not set, off for 0 (or empty).
/// It was opt-in (RX_VK_COMPACT=1) while its hold could let go of textures in use; that is what hold_keeps and hold_met are for
inline bool enabled(const char* env) { return !env || atoi(env) != 0; }

/// The driver's count over 90% of what it allows: a run starts for less free space, and the next one sooner
inline bool pressed(uint64_t used, uint64_t allowed) { return allowed && used > allowed / 10 * 9; }

/// The free space in the textures' pool from which a run starts: RX_VK_COMPACT_MB (512), 192 MB while pressed. The cache grows
/// into what a run gave back while the next one waits: under a 9.8 GB allowance the count reached 10.2 GB with 1.1-1.6 GB free
inline uint64_t run_from(uint64_t min_free, bool is_pressed) { return is_pressed ? std::min<uint64_t>(min_free, 192ull << 20) : min_free; }

/// The blocks a pool starts with (#2438: each block made in play is a vkAllocateMemory on the replay thread, ~170 ms on AMD's
/// Windows driver): 15% of the card's memory, 2 to 24, in the pool the sampled textures are made in (holds_textures) and in no
/// other. They were the images' pool's, where the textures went when #2438 was measured; compacted, the textures have a pool of
/// their own, and the images' pool is left DLSS's, the frame interpolation's and the scratch images. There the blocks stayed
/// empty: the driver counted them (1152 MB on an 8 GB card), no texture could take them, so reclaimable() rightly left them in
/// the count and the hold cut the texture budget by as much (#2690: an RTX 3070 in Castle Town Market, 31-36 fps against 54-56
/// without them), and the textures' blocks were made in play as before. In the textures' pool the scene's textures fill them,
/// and what they hold free is that pool's free space, which the budget takes off the count already
inline uint64_t blocks_at_start(uint64_t heap, uint64_t block, bool holds_textures) {
    return holds_textures && block ? std::clamp<uint64_t>(heap / 100 * 15 / block, 2, 24) : 0;
}

/// The free space of the textures' pool a run can give back: not what the blocks made at the start hold (kept), which VMA
/// keeps whatever is in them. Counted, a scene with fewer textures than those blocks hold started a run every time one was due
inline uint64_t returnable(uint64_t block_bytes, uint64_t free_bytes, uint64_t kept) {
    return std::min(free_bytes, block_bytes - std::min(block_bytes, kept));
}

/// Whether the textures' pool is down to its blocks made at the start. VMA keeps those, so nothing an eviction frees comes off
/// the driver's count: the rule that squeezes by the count alone (97% of the allowance) waits, as it does for room in the blocks
inline bool kept_only(uint64_t block_bytes, uint64_t kept) { return kept && block_bytes <= kept; }

/// The presents between the end of a run and the next look
inline uint64_t rest(bool is_pressed) { return is_pressed ? 120 : 600; }

/// The free space the texture budget takes off the driver's count: the textures' pool's alone, which a run gives back. What the
/// other blocks hold free (the render targets', the buffers') stays in the count and no texture can take it; taken off as well,
/// it let the cache grow until the count was at the limit (99% eight times in five minutes, 850 MB free where no texture goes)
inline uint64_t reclaimable(uint64_t block_free, uint64_t pool_free) { return std::min(block_free, pool_free); }

/// The texture budget that holds the count, as it will be once a run has given `reclaim` back, at pct% of the allowance:
/// what the textures hold (total) less the excess, and never more than the budget was
inline uint64_t held_budget(uint64_t budget, uint64_t total, uint64_t used, uint64_t reclaim, uint64_t allowed, uint64_t pct) {
    const uint64_t line = allowed / 100 * pct;
    const uint64_t pressure = used - std::min(used, reclaim);
    if (!allowed || pressure <= line) return budget;
    const uint64_t over = pressure - line;
    return std::min(budget, total > over ? total - over : 0);
}

/// What the hold lets go of has gone unused for this many presents (ten seconds at 60 fps). The eviction's own 60 is right for a
/// budget that is a size: over it, something has to go. The hold's line is crossed for as long as the rest of the game's VRAM
/// leaves the scene's textures no room under it, and then every texture out of view for a second went and came back when the
/// camera turned: with the count held under a 7 GB allowance, a turning camera in Hyrule Field at DLAA let go of 2.3-2.6 GB of live
/// textures every 600 presents and uploaded 6.8 GB in 50 seconds, against 1.4 GB with the compaction off
inline constexpr uint64_t kHoldUnused = 600;

/// Whether the hold's pass keeps a texture no render target has drawn over: one used in the last kHoldUnused presents
inline bool hold_keeps(uint64_t unused) { return unused < kHoldUnused; }

/// The reserve: a second line, kReservePoints points of the allowance under the hold's own (80 for 85). Over it the hold lets go
/// of textures unused for kReserveUnused presents (30 seconds at 60 fps), down to that line. With one line the cache sat at it,
/// and whatever came in next went over: in a walk across Hyrule Field at DLAA with 768 MB of the card held by something else the
/// count was over 85% of the allowance for 47-68% of the time and over 87% for 7-26% (peaks of 91-93%), and in play with a
/// recorder running the GPU's frame went from 11 to 15-23 ms for ten seconds each time it reached 87-90%. With the reserve:
/// over 85% for 5%, over 87% for 0-1%, peaks of 87-88%, no more uploads
inline constexpr uint64_t kReservePoints = 5;
inline constexpr uint64_t kReserveUnused = 1800;
inline uint64_t reserve_pct(uint64_t pct) { return pct > kReservePoints ? pct - kReservePoints : 0; }

/// What the rules that are not the hold's (the budget's size, the 90% rule, the driver's limit at 97%) ask an eviction to go down
/// to on their own: 7/8 of their budget once the textures hold more than it, as a pass over the budget cuts with the compaction
/// off; UINT64_MAX: nothing
inline uint64_t rule_target(uint64_t total, uint64_t rule_budget) { return total > rule_budget ? rule_budget / 8 * 7 : UINT64_MAX; }

/// The same, with the budget's size (size_budget) told from what the 90% rule and the driver's limit made of it (rule_budget,
/// never more). Those two are not sizes: each is "what the textures hold less the excess over a line", as the hold's is, and is
/// crossed again whenever anything grows. Cut to 7/8 of it, a few MB over the line took an eighth of the cache: Castle Town
/// Market entered from Hyrule Field on an 8 GB card sat 60 MB over the 90% line and lost ~400 MB of textures unused for a
/// second, all on screen again within the camera's turn, made again, over the line again: 2.5 <-> 2.95 GB every few seconds,
/// 8-9 uploads a frame and 16-25 frames over 50 ms in 30 s, where the same scene loaded from a save (just under the line) had
/// none. So for them what goes is the excess and kExcessSlack more, so that not every pass is one; the budget's size still asks
/// for its 7/8.
/// But only a small excess: that is the cache sitting at the line. One of more than a sixteenth of the rule's budget (half of
/// what the cut to 7/8 takes) is not that: an area has come in, or the textures are made again as fast as a pass lets them
/// go, and trimmed to the line the cache stays at it, every pass an eviction. Lake Hylia reached from Kakariko on a 10 GB card
/// with a 4 GB budget set by hand and the hold at 90% (issue 2780) sat 383 MB over the line at each pass, 4,022 MB of textures
/// in 8,288 MB and 6.3 uploads a frame at 12.7 fps, where the cut to 7/8 held 2,946 MB in 6,639 MB with no uploads at 42.4 fps
/// (likely because the passes count presents: the slower the frame, the longer "unused for 60" is and the surer what goes is
/// wanted again, and the trim never took the count far enough under the line for that to stop). So a large excess is cut to
/// 7/8, as before. The bound is a share of the budget and not less: with 128 MB the Market on the 8 GB card had 22-102 passes
/// in 60 s over it (181-199 MB on average) and let go of 11,100 textures, against 6,500 with the sixteenth and 14,500 with the
/// cut at every pass
inline constexpr uint64_t kExcessSlack = 64ull << 20;
inline constexpr uint64_t kExcessSmall = 16;   // (the excess is small up to rule_budget / kExcessSmall)
inline bool excess_small(uint64_t total, uint64_t rule_budget) {
    return total > rule_budget && total - rule_budget <= rule_budget / kExcessSmall;
}
inline uint64_t rule_target(uint64_t total, uint64_t size_budget, uint64_t rule_budget) {
    uint64_t to = rule_target(total, size_budget);
    if (rule_budget < size_budget && total > rule_budget)
        to = std::min(to, excess_small(total, rule_budget) ? rule_budget - std::min(rule_budget, kExcessSlack) : rule_budget / 8 * 7);
    return to;
}

/// What a live texture unused for that many presents may be let go down to in the hold's pass, the lowest of the lines that
/// apply to it; UINT64_MAX: it stays. Three tiers:
/// - unused for need_unused (the eviction's own 60 presents): what the other rules ask for (rule_to). Over 90% of the allowance
///   or over the budget's size something has to go now, and the hold's patience is not theirs: with the hold's two thresholds
///   alone, a pass the 90% rule asked for took nothing used in the last ten seconds, and the reserve's lower budget made every
///   such pass a hold's pass, on the cards whose count sits past 90%
/// - unused for kHoldUnused: the hold's own line too (hold_to, 0 while it is not crossed)
/// - unused for kReserveUnused: and the reserve's (reserve_to)
inline uint64_t hold_target(uint64_t unused, uint64_t need_unused, uint64_t rule_to, uint64_t hold_to, uint64_t reserve_to) {
    uint64_t to = unused >= need_unused ? rule_to : UINT64_MAX;
    if (!hold_keeps(unused) && hold_to) to = std::min(to, hold_to);
    if (unused >= kReserveUnused) to = std::min(to, reserve_to);
    return to;
}

/// Whether the hold's pass is done with live textures: those it has chosen (live) and the dead render targets it lets go in any
/// case (dead) bring what the textures hold (total) down to evict_to. The dead ones count first: they are the youngest of the
/// textures, and a pass that stopped at the first texture past the excess, oldest first, never reached them, so while the hold
/// was on, live textures went in their place and the dead targets piled up (0.9 to 3.5 GB of them in 120 presents as a new
/// stretch of Hyrule Field came in, the count over what the driver allows, frames of 60-128 ms for three seconds)
inline bool hold_met(uint64_t total, uint64_t live, uint64_t dead, uint64_t evict_to) {
    const uint64_t going = live + dead;
    return going >= total || total - going <= evict_to;
}

}   // namespace vram_compact
