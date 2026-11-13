// A temporal upscaler in place of OoT's gameplay upscale network (vk_temporal.cpp): the frame chain any such upscaler
// needs, and the engines that draw with it. The chain finds the scene's camera and depth, makes the inputs at the render
// size (the colour as the network samples it, the depth, motion vectors with the skinned draws' own), keeps the camera
// jitter's state for the host (nx2/oot_jitter.py) and writes the engine's picture into the network's output. An engine
// (NVIDIA DLSS: vk_dlss.cpp; AMD FSR 4: vk_fsr4.cpp) only turns those inputs into the picture.
#pragma once

#include <cstdint>
#include <functional>

#include "vk_internal.h"

namespace mkwvk {

struct Texture;

/// What the Upscaler row runs now on this chain (upscaler_rows.h: GAME, DLSS, DLAA, FSR3, FSR4, FSR4_AA): the row's choice
/// (opts.dlss), or RX_VK_DLSS=1 / 2 or RX_VK_FSR4=1 / 2 over it; GAME when the chosen engine cannot run here. AMD FSR 3.1
/// (RX_VK_FSR=1, or the row's FSR 3.1 with neither switch above set) is FSR3 here, an engine of this chain with the skinned
/// draws' motion (vk_fsr3.h fsr3_on_chain); with RX_VK_FSR3_CHAIN=0 it upscales with its own inputs and this is GAME then
int temporal_mode();
/// RX_VK_DLSS=1 / 2 or RX_VK_FSR4=1 / 2 set for this run, and that engine runs here: over the row, its FSR 3.1 too
/// (vk_fsr3.cpp fsr3_wanted). Where it cannot run, a saved FSR 3.1 stays
bool temporal_env_chosen();
bool temporal_wanted();       // (temporal_mode() != GAME)
/// The Upscaler row's Off (upscaler_rows.h OFF) with no engine running over it for this run (temporal_mode() is GAME): no
/// upscaler at all. temporal_upscale then leaves the game's network out too and stretches its input to its output with a plain
/// linear filter
bool temporal_off();
/// DLAA or FSR's native anti-aliasing: the 3D drawn at full scale, whatever the Render resolution row says
bool temporal_full_scale();

/// A draw into a scene-sized target (colour target 0 and the depth target), with the vertex stage's uniform buffer 3 at
/// `camera` (its size): the camera of the frame, when that buffer holds one
void temporal_note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                              uint64_t camera_size);
/// Whether a target of width x height can be the scene the gameplay upscale takes (1 to 3 times smaller than its output, as
/// last seen; dlss_checks.h scene_size_fits): the camera, the actor motion and RX_VK_DLSS_TRACE take only those
bool temporal_scene_size(uint32_t width, uint32_t height);
/// A skinned draw into a scene-sized target (vk_draw.cpp finds them by their vertex layout): what the motion needs to draw
/// it again with this frame's bones and the frame before's (actor motion: Link, the others, what moves by itself)
struct SkinnedDraw {
    const Texture* depth = nullptr;   // (its depth target: only the scene's are taken)
    uint64_t vbuf = 0, vsize = 0;     // (vertex stream 0)
    uint32_t stride = 0;
    uint32_t pos = 0;                 // (byte offsets in a vertex: the position, RGB32F or RGBA16F)
    uint32_t joints[2]{}, weights[2]{};   // (RGBA16UI or RGBA8UI joints and RGBA8 weights, four a set; sets: 1 or 2)
    int sets = 0;
    uint32_t formats = 0;             // (dlss_actor::Push::formats)
    uint64_t ibuf = 0;
    int itype = 0, count = 0, base_vertex = 0;
    uint64_t bones = 0, bones_size = 0;   // (the palette: 3 x 4 row-major matrices, model space to the camera's world)
    uint64_t object = 0, object_size = 0; // (the object's buffer: its origin at 0x40)
    uint64_t program = 0;
};
void temporal_note_skinned_draw(const SkinnedDraw& k);

/// A gameplay upscale's dispatch (program `key`; vk_draw.cpp's temporal_stand_in finds them: the 4/3 network fd029a317a9a4b94
/// at the game's usual 3/4, the same-size pass 9dc98871eb7fc101 at full size, and one network of its own for each other
/// share): its input as it samples it (in, in_view) and its output's storage view (out at level 0). known: one of the
/// gameplay networks by its key; else a network of that shape (the fixed shares' own), taken only when its input is this
/// frame's scene and its output the picture, and left to run without a word otherwise. true: the engine wrote the output
/// (the dispatch is left out); false: the network runs, the reason logged. With the Upscaler Off (temporal_off) the same
/// dispatches get the linear stretch in place of an engine
bool temporal_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                      VkImageView out_view, uint32_t out_level);

/// Once a frame (the game's present): after 120 frames without an upscale by an engine (off, or the upscale not taken) the
/// engine's own state, the chain's images and deferred frees are let go, so their video memory comes back. Nothing to do
/// costs one atomic load
void temporal_idle();
void temporal_reset_failure();

/// For the renderer's status line (vk_replay.cpp status_tick): while an engine draws, its details since the last call into b
/// ("; DLSS: jitter ..., actor motion ..., its images ... MB") and temporal_mode(); else 0 and b left alone
int temporal_status(char* b, size_t n);

/// The scene's width (axis 0) or height (1) for the host's camera jitter (nx2/oot_jitter.py, HOS_OPT_DLSS_JITTER_W and _H):
/// 0 when there is to be none (no engine drawing, RX_VK_DLSS_JITTER=0, or RX_VK_FSR_JITTER=0 with FSR 3.1 on the chain). Axis 2: the jitter sequence's length for the
/// engine's scale (dlss_jitter_phases, HOS_OPT_DLSS_JITTER_PHASES)
uint32_t temporal_jitter_size(int axis);
/// One completed sample shared by DLSS, FSR 4 and FSR 3.1 on the chain; all zero while inactive.
MkwVkJitter temporal_jitter_sample();

// ---- what an engine is given and does ----

/// One of the chain's images: GENERAL before and after an engine's evaluation
struct TemporalImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};
/// One frame's evaluation. color: the scene as the network samples it (tonemapped, square-root encoded: LDR, RGBA16F; linear
/// light past 1 with the game's native HDR for an engine whose hdr() says it takes that, its output then linear as well);
/// depth: R32F in [0, 1], not reversed (reversed, 1 - d, for an engine with fsr_depth where the projection puts the near
/// plane at 0: FSR 3.1; depth_reversed says so); motion: RG16F in render pixels, towards the frame before, the jitter taken off. They
/// are as large as the engine asked (in_w x in_h), the frame's part at the top left. output: RGBA16F, at the output's size
/// (DLAA, FSR's native AA: the render size)
struct TemporalFrame {
    TemporalImage color, depth, motion, output;
    uint32_t render_w = 0, render_h = 0;
    float jitter[2]{};          // (the scene's sub-pixel offset this frame, in render pixels, x right and y down)
    bool reset = false;         // (no history: the first frame, or one after a gap or a camera cut the chain saw)
    float near_z = 0.1f, far_z = 1000.0f, fov_y = 1.0f;   // (the scene's projection's, when it is a plain perspective one)
    float projection[16]{};     // (hookshot) the scene's projection without this frame's jitter: FSR 3.1's depth constants
    uint32_t jitter_phases = 8; // (hookshot) the length of the jitter's sequence the host runs (dlss_jitter_phases)
    /// (hookshot) depth holds 1 - d (the engine's fsr_depth, where the projection puts the near plane at 0: FSR 3.1). Anything
    /// else that reads this frame's depth (frame generation, other passes) must look here, not assume the plain depth
    bool depth_reversed = false;
};
/// An engine: plain functions, each called with the chain's lock held, on the recording thread
struct TemporalEngine {
    const char* name;           // (for the log and the status line: "NVIDIA DLSS", "AMD FSR 4")
    const char* short_name;     // ("DLSS", "FSR")
    /// Once, before its first upscale: false when it cannot run here (it says why in the log)
    bool (*start)();
    /// Ready to scale rw x rh into ow x oh, made again when its sizes or settings changed (cmd: what that records). in_w and
    /// in_h: how large the inputs are to be (the most the render size can be for what it made). 1 ready, 2 ready and made
    /// just now (a new history), 0 not this frame (*why: the reason), -1 failed for good (*why too).
    /// native_aa requests the engine's native anti-aliasing feature even if an upscale feature accepts these dimensions.
    int (*prepare)(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool native_aa,
                   uint32_t* in_w, uint32_t* in_h, const char** why);
    /// Records the frame: false when it failed (for good: the game's upscale from then on)
    bool (*evaluate)(VkCommandBuffer cmd, const TemporalFrame& f);
    /// What it made, let go (its feature or context): the chain unused for a while, or another engine chosen. Work in flight
    /// may still use it, so what it frees goes through later
    void (*release)();
    /// Its own video memory for the status line, in bytes (0: not known)
    uint64_t (*memory)();
    /// Whether what prepare made takes HDR colour (past 1: the game's native HDR), which the chain then gives it as linear
    /// light, unclamped, and takes back linear; null or false: LDR colour, square-root encoded, clamped to 0..1 (an LDR
    /// engine fed more would misread it)
    bool (*hdr)() = nullptr;
    /// (hookshot) The depth as FSR's own passes take it: reversed (1 at the near plane) where the game's projection puts the near
    /// plane at 0 (fsr3_math.h camera_from_projection's flip_depth). AMD FSR 3.1 (vk_fsr3.cpp): its passes are built for that
    bool fsr_depth = false;
    /// (hookshot) An evaluation that fails is for that frame only (the network draws it, and the next frame starts a new
    /// history), not for good: FSR 3.1, whose constants come from the frame's stream
    bool retry = false;
};
/// Runs f a few frames on, to free what work in flight may still use. Called by the engines with the chain's lock held
void temporal_later(std::function<void()> f);

} // namespace mkwvk
