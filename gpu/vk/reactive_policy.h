/* The reactive mask's rules (vk_reactive.h), as plain functions: which of the scene's draws are its transparent ones, and
   before which of them the scene's colour is copied. No Vulkan: tests/test_reactive_mask.cpp checks them.

   A temporal upscaler follows a pixel by its depth and its motion. What is blended over the scene without writing depth
   (water, smoke, fire, sparks, glows) has neither: the upscaler takes it for the surface behind, moved as that surface
   moved, and smears or ghosts it. A reactive mask names those pixels, so the upscaler leans on this frame there and not on
   its history. The game does not say which draws they are, so they are told by their state, and the mask is made the way
   AMD's FSR samples make one: the scene's colour is copied before the transparent draws, and where the scene has changed
   since by the time it is scaled up, something was blended over it. */
#pragma once

#include <cstdint>

namespace mkwvk::reactive_policy {

/// What a draw into the scene's target is to the mask
enum Draw {
    kNothing,       ///< writes no colour (the depth passes), or blends and writes depth (cut-out edges): neither side
    kOpaque,        ///< writes colour without blending: part of the picture the mask is measured against
    kTransparent,   ///< blends over the scene, tested against its depth without writing it: what the mask is for
};

/// colour_written: target 0 has a channel written; blend: target 0 blends
inline Draw classify(bool colour_written, bool blend, bool depth_test, bool depth_write) {
    if (!colour_written) return kNothing;
    if (!blend) return kOpaque;
    return depth_test && !depth_write ? kTransparent : kNothing;
}

/// The copies of one frame on one thread. The scene's colour is copied before the first transparent draw, and again before
/// the first one after an opaque draw or a draw into another scene-sized target: what was drawn since the copy would
/// otherwise read as blended over it. So the mask is of the last run of transparent draws, which is where a scene's
/// transparent things are drawn. kCopies at most a frame (each one ends the scene's pass and copies the target): past them
/// the last copy stays, and `capped` says the mask then holds more than that run
struct Run {
    uint64_t frame = ~0ull;
    uint64_t target = 0;
    int copies = 0;
    bool stale = false;    ///< something not transparent was drawn since the last copy, or the target is another
    bool capped = false;
};
constexpr int kCopies = 3;

/// A draw of this kind into the scene's target (any number that names it) in this frame: true when the scene's colour is
/// to be copied before it
inline bool copy_before(Run& r, uint64_t frame, uint64_t target, Draw draw) {
    if (r.frame != frame) r = Run{}, r.frame = frame, r.target = target;
    if (r.target != target) r.target = target, r.stale = true;
    if (draw == kOpaque) r.stale = true;
    if (draw != kTransparent) return false;
    if (r.copies && !r.stale) return false;
    if (r.copies >= kCopies) return r.capped = true, false;
    r.copies++, r.stale = false;
    return true;
}

} // namespace mkwvk::reactive_policy
