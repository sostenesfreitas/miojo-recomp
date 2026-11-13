// Production consumers inserted by test_present_source_crop.py; no GPU or game data.
#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#define VK_NO_PROTOTYPES
#define VK_USE_64_BIT_PTR_DEFINES 0
#include <vulkan/vulkan.h>
#include "vk_present_rect.h"
using namespace mkwvk;
using VmaAllocation = void*;
constexpr int kMaxMult = 8;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
/* ACTUAL_STATE */
struct { int framegen = 2, hdr_white = 200, hdr_peak = 1000, hdr_contrast = 100; } opts;
struct { bool push_descriptor = true; std::mutex present_mu; } dev;
constexpr bool vkCmdPushDescriptorSetKHR = true;
constexpr uint32_t HDR_ENCODE_COMP_SPV[] = {0};
bool init_scale() { std::abort(); }
bool make_pipe(Pipe&, const uint32_t*, size_t, std::initializer_list<VkDescriptorType>, int) { std::abort(); }
void logf(const char*, ...) { std::abort(); }   // (framegen_record logs only when the frames asked change: not here)
void device_wait_idle_locked() { std::abort(); }
void drop_img(Img&) { std::abort(); }
bool make_img(Img&, uint32_t, uint32_t, VkFormat, VkImageUsageFlags) { std::abort(); }
void to_general(VkCommandBuffer, VkImage) { std::abort(); }
float hdr_uv[4]{};
int blits = 0, clears = 0, initializations = 0, sizes = 0, dispatches = 0, copies = 0;
uint32_t allocated_w = 0, allocated_h = 0;
namespace fsr3fi { bool wanted = false; bool frames_wanted() { return wanted; } }
// (NVIDIA DLSS Frame Generation through NGX, vk_dlss.cpp: not asked for here, so the sets are the renderer's own)
bool dlss_fg_wanted() { return false; }
int dlss_fg_frames(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t, int, const VkImage*, const VkImageView*, bool,
                   uint64_t) { return 0; }   // (asked every present at 2x and up: makes none here)
uint64_t reflex_frame() { return 0; }   // (vk_reflex.cpp: NVIDIA Reflex not on here)
// (who made each set's frames, for the Insert menu's Frame generation by row: the last one noted)
enum class FgMaker { None, BuiltIn, Dlss, Fsr3 };
FgMaker last_maker = FgMaker::Dlss;
void note_maker(FgMaker m) { last_maker = m; }
uint64_t antilag_frame() { return 0; }   // (vk_antilag.cpp: AMD Anti-Lag 2 not on here)
int fsr_calls = 0, fsr_result = 1;
int fsr3fi_frame(VkCommandBuffer, OutSet&, int, uint32_t w, uint32_t h, uint64_t sequence) {
    CHECK(w == allocated_w && h == allocated_h && sequence > 0);
    ++fsr_calls; return fsr_result;
}
VkImageBlit last_blit{};
struct { int32_t rect[4]; float x0, cover; int32_t pq; } copy{};
std::atomic<bool> present_native_pq{false};
int32_t* const copy_pc = copy.rect;
float margin = 0.0f;   // (wide_margin: the middle 16:9's left edge, 0 on a 16:9 screen or with the setting off)
float wide_margin() { return margin; }
void vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                    uint32_t count, const VkImageBlit* b, VkFilter filter) {
    CHECK(count == 1 && filter == VK_FILTER_LINEAR); ++blits; last_blit = *b;
}
void vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue* c,
                          uint32_t count, const VkImageSubresourceRange* range) {
    CHECK(count == 1 && range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(c->float32[0] == 0 && c->float32[1] == 0 && c->float32[2] == 0 && c->float32[3] == 1); ++clears;
}
bool init() { ++initializations; return true; }
int multiplier() { return 2; }
bool ensure_size(VkCommandBuffer, uint32_t w, uint32_t h) {
    // (the sets are made for the row's multiplier, which framegen_record then takes from fg.mult, as the real one)
    ++sizes; allocated_w = w; allocated_h = h; fg.mult = multiplier(); return true;
}
int64_t now_ns() { return 1000000; }
int display_refresh_mhz() { return 60000; }
int auto_slots(int64_t, int64_t, int64_t, int64_t&, float*, int64_t*, int) { return 0; }
void display_main() { std::abort(); } // Never started: isolated recorder sets thread_started.
void full_barrier(VkCommandBuffer) {}
void pass(VkCommandBuffer, const Pipe& p, std::initializer_list<std::pair<VkImageView, VkSampler>>,
          uint32_t, uint32_t, const void* pc = nullptr, uint32_t bytes = 0) {
    ++dispatches;
    if (&p == &fg.copy) { CHECK(bytes == sizeof copy && bytes == 28); std::memcpy(&copy, pc, bytes); }
    if (&p == &hdr.pipe) { CHECK(bytes == 48); std::memcpy(hdr_uv, pc, sizeof hdr_uv); }
}
void vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                    uint32_t count, const VkImageCopy* c) {
    CHECK(count == 1 && c->extent.width == allocated_w && c->extent.height == allocated_h); ++copies;
}
/* ACTUAL_FUNCTIONS */
void reset() {
    fg.~State(); new (&fg) State;
    fg.thread_started = true;
    blits = clears = initializations = sizes = dispatches = copies = 0;
    allocated_w = allocated_h = 0;
    fsr3fi::wanted = false; fsr_calls = 0; fsr_result = 1;
    copy = {}; margin = 0.0f; present_native_pq = false;
}
void check(uint32_t sw, uint32_t sh, const int* crop, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    reset();
    blit_crop_to_rect(nullptr, 1, sw, sh, crop, 2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {3, 5, 1920, 1080});
    const bool valid = w && h;
    CHECK(blits == int(valid) && clears == int(!valid));
    if (valid) {
        CHECK(last_blit.srcOffsets[0].x == int(x) && last_blit.srcOffsets[0].y == int(y));
        CHECK(last_blit.srcOffsets[1].x == int(x + w) && last_blit.srcOffsets[1].y == int(y + h));
        CHECK(last_blit.dstOffsets[0].x == 3 && last_blit.dstOffsets[1].x == 1923);
    }
    blits = clears = 0;
    blit_crop(nullptr, 1, sw, sh, crop, 2, VK_IMAGE_LAYOUT_GENERAL, 320, 180);
    CHECK(blits == int(valid) && clears == int(!valid));
    if (valid) CHECK(last_blit.dstOffsets[0].x == 0 && last_blit.dstOffsets[1].x == 320);
    const bool recorded = framegen_record(nullptr, 1, sw, sh, crop, 0.0f);
    CHECK(recorded == valid && initializations == int(valid) && sizes == int(valid));
    if (valid) {
        CHECK(allocated_w == w && allocated_h == h && dispatches == 2 && copies == 1);
        CHECK(last_maker == FgMaker::None);   // (nothing to blend from yet: none made)
        CHECK(copy_pc[0] == int(x) && copy_pc[1] == int(y) && copy_pc[2] == int(w) && copy_pc[3] == int(h));
        // Newly merged FSR3 receives the same validated extent; fallback still uses the shared crop.
        fsr3fi::wanted = true;
        CHECK(framegen_record(nullptr, 1, sw, sh, crop, 0.0f) && fsr_calls == 1 && fg.recorded->made == 1);
        CHECK(dispatches == 4 && copies == 2 && last_maker == FgMaker::Fsr3);
        fsr_result = 0;
        CHECK(framegen_record(nullptr, 1, sw, sh, crop, 0.0f) && fsr_calls == 2 && fg.recorded->made == 1);
        CHECK(last_maker == FgMaker::BuiltIn);   // (FSR 3.1 made none: the renderer's own did)
        CHECK(dispatches == 9 && copies == 3);
        CHECK(copy_pc[0] == int(x) && copy_pc[1] == int(y) && copy_pc[2] == int(w) && copy_pc[3] == int(h));
        // Ultrawide's loading cover is closed in the copy, from the middle 16:9's edge; without that middle there is none.
        CHECK(copy.x0 == 0.0f && copy.cover == 0.0f);
        const auto record = [&](float cover) {   // (no display thread here to show a set and free it)
            for (OutSet& set : fg.sets) set.busy = false;
            return framegen_record(nullptr, 1, sw, sh, crop, cover);
        };
        CHECK(record(0.5f) && copy.x0 == 0.0f && copy.cover == 0.0f);
        margin = 0.125f;
        CHECK(record(0.5f) && copy.x0 == 0.125f && copy.cover == 0.5f);
        CHECK(record(3.0f) && copy.x0 == 0.125f && copy.cover == 1.0f && copy.pq == 0);
        present_native_pq = true;   // (the game's own HDR10 picture: its coding for the cover's light)
        CHECK(record(0.5f) && copy.cover == 0.5f && copy.pq == 1);
        present_native_pq = false;
        CHECK(record(0.0f) && copy.x0 == 0.0f && copy.cover == 0.0f);
        CHECK(copy_pc[0] == int(x) && copy_pc[1] == int(y) && copy_pc[2] == int(w) && copy_pc[3] == int(h));
    } else {
        CHECK(dispatches == 0 && copies == 0 && !fg.recorded);
        fsr3fi::wanted = true; fg.have_prev = true;
        CHECK(!framegen_record(nullptr, 1, sw, sh, crop, 0.0f) && fsr_calls == 0);
        CHECK(dispatches == 0 && copies == 0 && !fg.recorded);
    }
    // Execute the actual HDR consumer, keeping resources ready to isolate source validation.
    hdr.init = true; hdr.failed = false; hdr.w = 1920; hdr.h = 1080; hdr.out.image = 77;
    const int before = dispatches;
    CHECK(framegen_hdr(nullptr, 1, sw, sh, crop, 1920, 1080, false) == (valid ? 77 : VK_NULL_HANDLE));
    CHECK(dispatches == before + int(valid));
    if (valid) {
        CHECK(hdr_uv[0] == x / float(sw) && hdr_uv[1] == y / float(sh));
        CHECK(hdr_uv[2] == (x + w) / float(sw) && hdr_uv[3] == (y + h) / float(sh));
    }
}
int main() {
    check(3440, 1440, nullptr, 0, 0, 3440, 1440);
    const int ordinary[] = {160, 90, 2240, 1260}; check(2560, 1440, ordinary, 160, 90, 2240, 1260);
    const int clipped[] = {2500, 1400, 100, 100}; check(2560, 1440, clipped, 2500, 1400, 60, 40);
    const int overflow[] = {1, 1, INT_MAX, INT_MAX}; check(640, 480, overflow, 1, 1, 639, 479);
    const int bad[][4] = {{-1, 0, 40, 40}, {0, -1, 40, 40}, {INT_MIN, 0, INT_MAX, 40},
                         {640, 0, 1, 1}, {0, 480, 1, 1}, {INT_MAX, INT_MAX, 1, 1}};
    for (const auto& c : bad) check(640, 480, c, 0, 0, 0, 0);
    const int disabled[][4] = {{INT_MIN, INT_MIN, 0, 10}, {-1, -1, 10, -1}, {20, 30, -1, -1}};
    for (const auto& c : disabled) check(640, 480, c, 0, 0, 640, 480);
    check(0, 480, nullptr, 0, 0, 0, 0);
    check(640, 0, nullptr, 0, 0, 0, 0);
    check(uint32_t(INT_MAX) + 1, 480, nullptr, 0, 0, 0, 0);
    check(640, uint32_t(INT_MAX) + 1, nullptr, 0, 0, 0, 0);
    uint32_t seed = 42;
    for (int i = 0; i < 4096; ++i) {
        seed = seed * 1664525u + 1013904223u; const int x = int(seed % 640);
        seed = seed * 1664525u + 1013904223u; const int y = int(seed % 480);
        const int c[] = {x, y, INT_MAX, INT_MAX};
        check(640, 480, c, uint32_t(x), uint32_t(y), uint32_t(640-x), uint32_t(480-y));
    }
    reset(); blit_crop_to_rect(nullptr, 1, 640, 480, nullptr, 2, VK_IMAGE_LAYOUT_GENERAL, {0, 0, 0, 100});
    CHECK(blits == 0 && clears == 0);
    std::puts("PASS: presentation/capture/interpolation source crops agree; invalid input clears or skips, no signed overflow");
}
