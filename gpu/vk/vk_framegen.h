// Frame interpolation (vk_framegen.cpp): made frames shown between the game's (in a window; RX_VK_FRAMEGEN=0: off).
#pragma once

#include "vk_internal.h"

namespace mkwvk {

bool framegen_on();
/// Records the frame's processing into the present's recording (src: the window texture's view, sw x sh, the crop
/// x, y, w, h; cover: wide_sides_tick's, this frame's); false when interpolation cannot run (the present goes to the window
/// as without it)
bool framegen_record(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], float cover);
/// (hookshot) Who made the made frames of the sets shown lately (the last 64, about a second at 60): what the Insert menu's
/// Frame generation by row and the FPS counter name. None: none made (Off, nothing to blend from, none for half a second)
enum class FgMaker { None, BuiltIn, Dlss, Fsr3 };
FgMaker framegen_maker();
/// The first frame the last framegen_record made (null when it made none), w x h, GENERAL
VkImage framegen_made_image(uint32_t* w, uint32_t* h);
/// After that recording's submit (value): the display thread shows its frames
void framegen_queue(uint64_t value);
/// The window's present of an image (GENERAL, w x h; crop as above or null), vk_replay.cpp: the display thread's
void present_image(VkImage src, uint32_t w, uint32_t h, const int crop[4], VkImageView view = VK_NULL_HANDLE);
/// (hookshot) Present ahead (vk_framegen.cpp display_main, #1702): while present_defer_into is set on a thread, present_image
/// submits the frame's copy into its swapchain image and stops there, with what present_commit needs to show it later. The
/// copy is then on the queue before the game's next frame, so its present at its time does not wait for that frame's rendering
struct PendingPresent {
    bool used = false;
    uint32_t index = 0;
    VkFence done = VK_NULL_HANDLE;           // (the copy's fence)
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t reflex_frame = ~0ull;           // (NVIDIA Reflex: the frame it shows, vk_reflex.h reflex_present_frame)
    uint64_t antilag_frame = ~0ull;          // (AMD Anti-Lag 2: the frame it shows, vk_antilag.h antilag_present_frame)
    uint64_t swapchain_generation = 0;
};
extern thread_local PendingPresent* present_defer_into;
bool present_commit(const PendingPresent& p);   // (false: not shown, nothing deferred or a swapchain made again since)
/// The Insert menu's upscaling of a frame to the window (dw x dh), recorded: the image to show, or null when it is off.
/// pq: src is the game's own HDR10 picture (Native HDR), scaled into an RGB10A2 image
VkImage framegen_scale(VkCommandBuffer cmd, VkImageView src, VkImage image, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh,
                       bool pq = false);
VkImageView framegen_scaled_view();   // (hookshot) the view of framegen_scale's last image
/// (hookshot) The HDR10 picture of src's crop at dw x dh (the Insert menu's HDR), GENERAL; null: none (the SDR picture then;
/// Native's scaled picture, else black: present_image).
/// pq: src is the game's own HDR10 picture (Native), its PQ codes as they are at 100% contrast, else graded and written again;
/// else the SDR picture, expanded
VkImage framegen_hdr(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh,
                     bool pq = false);
/// Each present: ultrawide's menus (the Insert menu's setting to the game's side, the sides blurred or black while a menu shows);
/// cover: how far the loading cover was closed when the game presented this frame (wide_cover_sample), the sides closed by as much
void wide_sides_tick(float cover);
/// How far the loading cover between two areas is closed now, 0 to 1 (0 unless ultrawide's menus are in the middle): asked as the
/// game presents a frame, on its thread, and carried with that frame to wide_sides_tick, since the game goes on to the next one.
/// The larger of this reading and the last (wide_cover_held): the game may already have moved the cover on for its next frame
float wide_cover_sample();
/// Whether ultrawide's menu sides can show at all: Ultrawide with a margin and the Insert menu's setting on
bool wide_sides_possible();

} // namespace mkwvk
