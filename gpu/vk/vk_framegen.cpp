// Frame interpolation (RX_VK_FRAMEGEN=1; off by default): frames made between the game's, from its last two, shown in
// between by a display thread, so a game at 60 fps is shown at 120 (RX_VK_FRAMEGEN_MULT=3: 180). The WebGPU
// renderer's (gpu/render/rx_framegen.cpp, whose comments explain it), on Vulkan:
//   1. each frame is copied (it may be drawn over next frame) and reduced to quarter-size luma;
//   2. block matching: a vector per 16x16 pixels within +-32 pixels (RX_VK_FRAMEGEN_RADIUS quarter texels, default
//      8), then a 3x3 median of them;
//   3. each in-between frame at time t blends the last frame moved back along t and the new one forward along 1 - t.
// Recorded into the present's own recording, after the game's work; the display thread shows the made frames and
// then the game's, evenly over the game's recent frame time, about one frame behind the game. Loading hitches
// (frames over 100 ms apart) are not interpolated.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <cmath>

#include "vk_framegen.h"
#include "vk_antilag.h"   // (AMD Anti-Lag 2: which game frame a set shows, antilag_frame / antilag_present_frame)
#include "vk_dlss.h"   // (NVIDIA DLSS Frame Generation through NGX: dlss_fg_frames)
#include "vk_reflex.h"   // (NVIDIA Reflex: which game frame a set shows, reflex_frame / reflex_present_frame)
#include "vk_fsr3_inputs.h"   // (AMD FSR 3.1 frame interpolation's depth and motion)
#include "vk_fsr3fi.h"
#include "vk_menu.h"
#include "vk_present_rect.h"
#include "vk_texture.h"

#include "shaders/fg_copy_comp_spv.h"
#include "shaders/fg_flow_comp_spv.h"
#include "shaders/fg_luma_comp_spv.h"
#include "shaders/fg_median_comp_spv.h"
#include "shaders/fg_scale_comp_spv.h"
#include "shaders/fg_scale_pq_comp_spv.h"   // (fg_scale.comp with FG_SCALE_PQ: an RGB10A2 destination)
#include "shaders/fg_synth_comp_spv.h"
#include "shaders/hdr_encode_comp_spv.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION   // (older Windows SDKs lack it: Windows 10 1803 and later take it)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace mkwvk {
int display_refresh_mhz();   // (vk_main.cpp: the window's display, mHz; outside the unnamed namespace, where it is defined)
extern std::atomic<uint32_t> present_kind;   // (vk_replay.cpp: the frame log's)
namespace {

constexpr int kMaxMult = 8;   // (up to 8x: players asked; the made frames are allocated for the multiplier chosen only)

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
};
struct OutSet {
    Img frame[kMaxMult];
    int made = 0;   // (frame[0..made) are made frames; frame[made] is the game's)
    bool busy = false;
    // (Auto: frame[0..shown) each at its own time, due[k] (steady clock), game[k] the game's own frame; shown < 0: the
    // fixed multipliers' frames, made + 1 of them, shown evenly)
    int shown = -1;
    int64_t due[kMaxMult] = {};
    bool game[kMaxMult] = {};
    uint64_t reflex_id = 0;   // (NVIDIA Reflex: the game frame it was made at, 0 none: its game frame is presented with it)
    uint64_t antilag_id = 0;  // (AMD Anti-Lag 2: the same, its game frame's present stage)
};
struct Job {
    OutSet* set;
    int64_t at;
    uint64_t value;   // (the recording that makes its frames)
};
struct Pipe {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
};

struct State {
    bool init = false;
    std::atomic<bool> failed{false};   // also read by Reflex's game-thread eligibility check
    uint32_t w = 0, h = 0;
    Pipe copy, luma, flow, median, synth;
    VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    Img frames[2], lumas[2], flows[2];
    OutSet sets[3];   // (one shown, one waiting its place on the line, one being made: the pacer never holds the game back)
    int cur = 0, next_set = 0;
    bool have_prev = false;
    int64_t last_at = 0;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool thread_started = false;
    OutSet* recorded = nullptr;   // (the set the last framegen_record filled, for framegen_queue)
    int64_t recorded_at = 0;
    int mult = 0;   // (the multiplier the sets' frames were made for)
    int64_t game_ns = 0;     // (Auto: the game's frame time, smoothed: the delay its frames are shown at)
    int64_t slot_next = 0;   // (Auto: the display's next refresh to be given a frame, on the game's timeline)
} fg;

// Used only by present_image, whose lock serializes direct and interpolated presents.
struct ScaleState {
    bool init = false, failed = false;
    Pipe pipe;
    Pipe pipe_pq;           // (fg_scale_pq, for the game's own HDR10 picture: framegen_scale's pq; made at its first use)
    bool pq_tried = false;
    VkSampler linear = VK_NULL_HANDLE;
    Img cropped, scaled, small;   // (small: the picture at 1/8, for ultrawide's blurred menu sides)
    uint32_t cropped_w = 0, cropped_h = 0, scaled_w = 0, scaled_h = 0, small_w = 0, small_h = 0;
    bool pq = false;        // (the images above are RGB10A2, for pq, else RGBA8)
} scale;

struct HdrState {   // (hookshot) the HDR10 pass (framegen_hdr)
    bool init = false, failed = false;
    Pipe pipe;
    Img out;
    uint32_t w = 0, h = 0;
} hdr;

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// (hookshot) Frame generation's Auto (the menu's 1): a frame for each refresh of the display, no more. Each game frame
// is given the display's refreshes that fall between it and the game's frame before (on the game's own timeline, a
// refresh period apart), and each of them a frame made at its place between the two: 45 fps on a 60 Hz display makes one
// frame for every three of the game's (3 of 4 shown are made, at 3/4, 1/2, 1/4), on 120 Hz two or three a frame; at
// 60 fps on 60 Hz none (a refresh near a game frame shows it), and faster than the display a game frame may be given no refresh and is not shown. Shown one
// game frame time later, each at its refresh's time. The sets' frames are made once for the most a game frame may be
// given (the refresh over 30 fps, 2 to 4). An unknown refresh counts as 60 Hz.
int auto_cap() {
    const int hz = (display_refresh_mhz() + 500) / 1000;
    return std::clamp(hz > 0 ? (hz + 29) / 30 : 2, 2, 4);
}
int multiplier() {   // (the Insert menu; 1 Auto (its most); the game's frames alone at 0)
    return opts.framegen == 1 ? auto_cap() : std::clamp(opts.framegen, 1, kMaxMult);
}
// (hookshot) A fixed multiplier makes as many frames a game frame as the display can show, m at most, so the game keeps its
// own rate. Each made frame is a full-size pass on the game's queue and a present of its own, and a frame past the display's
// refresh never reaches the screen: 8x at 60 fps on a 180 Hz display made 7 frames a game frame for 3 refreshes, the display
// thread got ~220 presents a second out, the game waited on its sets and fell from 60 to ~28 fps (3440x1440, 2026-11-11).
// Now: the display's refreshes in the game's frame time (60 fps on 180 Hz: 3, two of them made), 1 (the game's frame alone)
// at the refresh. An unknown refresh or frame time: m. RX_VK_FRAMEGEN_UNCAPPED=1: m always. (tests/test_framegen_auto.py)
int fixed_frames(int m, int hz, int64_t game_ns) {
    if (m <= 1 || hz <= 0 || game_ns <= 0) return std::max(m, 1);
    const int fit = (int)((double)hz * (double)game_ns / 1e9 + 0.2);   // (+0.2: a game a little over 60 fps on 180 Hz keeps its 3)
    return std::clamp(fit, 1, m);
}
/// Auto: the display's refreshes in the game frame's interval (last, now], next the first not given a frame yet (moved
/// past them). Each one's place between the two game frames in t (1: this one) and its time in at; most of them at the
/// most, those past it skipped. A timeline outside the interval (the first frame, a hitch) starts again at now.
int auto_slots(int64_t last, int64_t now, int64_t period, int64_t& next, float* t, int64_t* at, int most) {
    if (now <= last || period <= 0) return 0;
    if (next <= last || next > now + period) next = now;
    int n = 0;
    for (; next <= now; next += period)
        if (n < most) t[n] = (float)(next - last) / (float)(now - last), at[n++] = next;
    // A refresh within a quarter period of the game's frame shows that frame itself: at the game's rate the frames come
    // a little early or late (jitter), and without this nearly every refresh got a made frame at t near 1 and some game
    // frames none (their neighbour two). The timeline is pulled an eighth of the way to the frame, so that a game at the
    // display's rate (or a whole fraction of it) stays on it while the two clocks drift apart, and other rates hardly
    // change how many frames the display is given
    const int64_t snap = period / 4;   // (not "near": windows.h defines it empty)
    if (next - now < snap) {
        n = std::min(n + 1, most);
        t[n - 1] = 1.0f, at[n - 1] = now, next += period + (now - next) / 8;
    } else if (n > 0 && now - (next - period) < snap) {
        t[n - 1] = 1.0f, at[n - 1] = now, next += (now - (next - period)) / 8;
    }
    return n;
}

void drop_pipe(Pipe& p) {
    if (p.pipe) vkDestroyPipeline(dev.device, p.pipe, nullptr);
    if (p.layout) vkDestroyPipelineLayout(dev.device, p.layout, nullptr);
    if (p.set) vkDestroyDescriptorSetLayout(dev.device, p.set, nullptr);
    p = {};
}

/// A compute pipeline over bindings of these types (push descriptors), push constants of push bytes
bool make_pipe(Pipe& p, const uint32_t* code, size_t bytes, std::initializer_list<VkDescriptorType> types, uint32_t push,
               const VkSpecializationInfo* spec = nullptr) {
    Pipe made;
    const auto undo = [&] { drop_pipe(made); return false; };
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (VkDescriptorType t : types) b.push_back({(uint32_t)b.size(), t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = (uint32_t)b.size();
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &made.set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, std::max(push, 4u)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &made.set;
    pi.pushConstantRangeCount = push ? 1 : 0;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &made.layout) != VK_SUCCESS) {
        made.layout = VK_NULL_HANDLE;
        return undo();
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return undo();
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = spec;
    ci.layout = made.layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &made.pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return undo();
    p = made;
    return true;
}

// The optional interpolation is disabled after setup fails: retain none of its unused setup objects.
void drop_setup() {
    for (Pipe* p : {&fg.copy, &fg.luma, &fg.flow, &fg.median, &fg.synth}) drop_pipe(*p);
    if (fg.nearest) vkDestroySampler(dev.device, fg.nearest, nullptr);
    if (fg.linear) vkDestroySampler(dev.device, fg.linear, nullptr);
    fg.nearest = fg.linear = VK_NULL_HANDLE;
}

bool init() {
    if (fg.init) return !fg.failed;
    fg.init = true;
    fg.failed = true;
    const auto undo = [] { drop_setup(); return false; };
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const int radius = getenv("RX_VK_FRAMEGEN_RADIUS") ? std::clamp(atoi(getenv("RX_VK_FRAMEGEN_RADIUS")), 1, 16) : 8;
    const VkSpecializationMapEntry me{0, 0, 4};
    const VkSpecializationInfo spec{1, &me, 4, &radius};
    if (!make_pipe(fg.copy, FG_COPY_COMP_SPV, sizeof FG_COPY_COMP_SPV, {S, I}, 28) ||
        !make_pipe(fg.luma, FG_LUMA_COMP_SPV, sizeof FG_LUMA_COMP_SPV, {S, I}, 0) ||
        !make_pipe(fg.flow, FG_FLOW_COMP_SPV, sizeof FG_FLOW_COMP_SPV, {S, S, I}, 0, &spec) ||
        !make_pipe(fg.median, FG_MEDIAN_COMP_SPV, sizeof FG_MEDIAN_COMP_SPV, {S, I}, 0) ||
        !make_pipe(fg.synth, FG_SYNTH_COMP_SPV, sizeof FG_SYNTH_COMP_SPV, {S, S, S, I}, 4))
        return undo();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.25f;
    if (vkCreateSampler(dev.device, &si, nullptr, &fg.nearest) != VK_SUCCESS) {
        fg.nearest = VK_NULL_HANDLE;
        return undo();
    }
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(dev.device, &si, nullptr, &fg.linear) != VK_SUCCESS) {
        fg.linear = VK_NULL_HANDLE;
        return undo();
    }
    fg.failed = false;
    if (opts.framegen == 1)
        logf("vk: frame interpolation ready: Auto, a frame for each of the display's refreshes (%d Hz; up to %d a game frame)",
             (display_refresh_mhz() + 500) / 1000, multiplier());
    else
        logf("vk: frame interpolation ready: %dx the game's frames (made from its last two, shown evenly)", multiplier());
    return true;
}

bool init_scale() {
    if (scale.init) return !scale.failed;
    scale.init = scale.failed = true;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (make_pipe(scale.pipe, FG_SCALE_COMP_SPV, sizeof FG_SCALE_COMP_SPV, {S, I, S}, 24)) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.25f;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        if (vkCreateSampler(dev.device, &si, nullptr, &scale.linear) == VK_SUCCESS) {
            scale.failed = false;
            return true;
        }
        scale.linear = VK_NULL_HANDLE;
    }
    drop_pipe(scale.pipe);
    return false;
}

bool make_img(Img& i, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    Img made;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    if (!create_image(ci, made.image, made.alloc)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = made.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vi, nullptr, &made.view) != VK_SUCCESS) {
        destroy_image(made.image, made.alloc);
        return false;
    }
    i = made;
    return true;
}
void drop_img(Img& i) {
    if (i.view) vkDestroyImageView(dev.device, i.view, nullptr);
    if (i.image) destroy_image(i.image, i.alloc);
    i = {};
}

void to_general(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// Only after the display jobs and device have drained (ensure_size), or before any frame used these images.
void drop_size() {
    for (int i = 0; i < 2; i++) {
        drop_img(fg.frames[i]), drop_img(fg.lumas[i]), drop_img(fg.flows[i]);
    }
    for (OutSet& set : fg.sets)
        for (Img& t : set.frame) drop_img(t);
    fg.w = fg.h = 0;
    fg.mult = 0;
    fg.have_prev = false;
    fg.recorded = nullptr;
}

bool ensure_size(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    if (fg.w == w && fg.h == h && fg.mult == multiplier()) return true;
    {   // (the display thread is done with the old sets first)
        std::unique_lock lk(fg.mu);
        fg.cv.wait(lk, [] { return fg.jobs.empty() && !fg.sets[0].busy && !fg.sets[1].busy && !fg.sets[2].busy; });
    }
    {
        std::lock_guard qk(dev.present_mu);   // (every queue idle: the present queue's too, device_wait_idle_locked)
        device_wait_idle_locked();
    }
    drop_size();
    const uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4, cw = (qw + 3) / 4, ch = (qh + 3) / 4;
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        ok = ok && make_img(fg.frames[i], w, h, VK_FORMAT_R8G8B8A8_UNORM, rw | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        ok = ok && make_img(fg.lumas[i], qw, qh, VK_FORMAT_R16_SFLOAT, rw);
        ok = ok && make_img(fg.flows[i], cw, ch, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    }
    const int mult = multiplier();
    for (OutSet& set : fg.sets)
        for (int k = 0; k < mult; k++)
            ok = ok && make_img(set.frame[k], w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                rw | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!ok) {
        fg.failed = true;
        drop_size();
        // Older submissions were drained above; ensure_size precedes every pass recorded by framegen_record.
        drop_setup();
        logf("vk: frame interpolation: its images (%ux%u) were not made: off", w, h);
        return false;
    }
    fg.w = w, fg.h = h, fg.mult = mult;
    for (int i = 0; i < 2; i++) {
        to_general(cmd, fg.frames[i].image), to_general(cmd, fg.lumas[i].image), to_general(cmd, fg.flows[i].image);
    }
    for (OutSet& set : fg.sets)
        for (Img& t : set.frame)
            if (t.image) to_general(cmd, t.image);   // (only the multiplier's frames are made: a barrier on no image crashed the driver)
    fg.have_prev = false;
    return true;
}

/// One pass: pipe over w x h in 8x8 groups, the bindings in order (a sampler when one is given: a sampled image)
void pass(VkCommandBuffer cmd, const Pipe& p, std::initializer_list<std::pair<VkImageView, VkSampler>> bindings,
          uint32_t w, uint32_t h, const void* push = nullptr, uint32_t push_bytes = 0) {
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet ws[8];
    uint32_t n = 0;
    for (const auto& [view, sampler] : bindings) {
        ii[n] = {sampler, view, VK_IMAGE_LAYOUT_GENERAL};
        ws[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws[n].dstBinding = n;
        ws[n].descriptorCount = 1;
        ws[n].descriptorType = sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ws[n].pImageInfo = &ii[n];
        n++;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, n, ws);
    if (push_bytes) vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cmd);
}

// (hookshot) The display thread's wait for a frame's time, on a high-resolution timer. sleep_for rests on the system timer,
// whose 1 ms (timeBeginPeriod) Windows 11 does not keep for a process whose window it takes as unseen: with the presents
// no longer blocking (a present queue of their own) the thread slept before most frames and woke up to 20 ms after their time.
void wait_until_near(int64_t ns) {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER d;
    d.QuadPart = -std::max<int64_t>(1, ns / 100);   // (relative, 100 ns units)
    if (timer && SetWaitableTimerEx(timer, &d, 0, nullptr, nullptr, nullptr, 0)) {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

/// Present ahead (display_main): a set of two (one made frame, paced) has its copies submitted together, which holds two
/// swapchain images acquired at once, so only where the swapchain has two to spare (an acquire past them has to time out,
/// the Vulkan spec). Larger sets present one at a time, as before
static bool present_ahead_wanted(bool env_on, bool pace, int shown, uint32_t spare) {
    // (each frame of the set holds a swapchain image from its copy to its present: as many spare images as frames. 2x was
    // the only one at first; at 3x and 4x the made frames' copies queued behind the game's next frame the same way, so their
    // presents reached the screen unevenly. vk_main.cpp asks for the images 3x and 4x need)
    return env_on && pace && shown >= 2 && spare >= (uint32_t)shown;
}

// (the display thread's: the frames it shows have ultrawide's loading cover in their sides already, framegen_record's copy)
thread_local bool wide_cover_in_frame = false;
void display_main() {
    wide_cover_in_frame = true;
    SetThreadDescription(GetCurrentThread(), L"mkw_vk display");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    int64_t interval = 16666667, last_at = 0;
    struct { int on_time, late, skipped; int64_t since; } stat{};
    int64_t next_show = 0, shown_at = 0;
    int hist[6] = {};
    for (;;) {
        Job j;
        {
            std::unique_lock lk(fg.mu);
            fg.cv.wait(lk, [] { return !fg.jobs.empty(); });
            j = fg.jobs.front();
            fg.jobs.pop_front();
        }
        if (last_at) {
            const int64_t d = j.at - last_at;
            if (d > 0 && d < 100000000) interval = (interval * 7 + std::clamp<int64_t>(d, 4000000, 50000000)) / 8;
        }
        last_at = j.at;
        if (!stat.since) stat.since = j.at;
        if (j.at - stat.since > 5000000000LL) {
            logf("vk: frame interpolation: %d frames shown on time, %d late (over 2 ms), %d made ones left out (the game's next "
                 "frame waiting); shown apart: <7 ms %d, 7-9 %d, 9-12 %d, 12-17 %d, 17-25 %d, over 25 %d", stat.on_time, stat.late,
                 stat.skipped, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
            memset(hist, 0, sizeof hist);
            stat = {0, 0, 0, j.at};
        }
        // (hookshot) The pacer. The game hands over frames 12 to 28 ms apart around a mean of 19, and shown as they came that
        // was the stutter, whatever the frame rate. Every frame now has its place on an even line, the recent frame time
        // apart, a frame early waiting for it: never more than one game frame behind what the game handed over, and a line
        // that has fallen behind what came starts again from now. RX_VK_PACE=0: as they come.
        const bool pace = opts.pace != 0;
        const bool timed = j.set->shown >= 0;   // (Auto: each frame at its own refresh's time)
        const int shown = timed ? j.set->shown : j.set->made + 1;
        const int64_t step = interval / std::max(shown, 1);
        if (timed) menu_note_frame(true, 0);   // (the overlay's game rate: Auto counts each game frame, given a refresh or not)
        // (hookshot) Present ahead (#1702; RX_VK_PRESENT_AHEAD=0: off). Each frame's copy into the window was submitted at its
        // time, behind the game's next frame on the same queue, and shown only once that frame's rendering was done: at 2x the
        // game's own frames reached the screen ~3 ms after the made ones, 5.7 and 11 ms apart instead of 8.3 (PresentMon, RTX
        // 5080, 60 fps), and a VRR display's refresh changed every frame (flicker on OLED). Now a set's copies go to the GPU
        // together, the first one's end is waited for, and only the presents keep their times on the line.
        static const bool ahead_env = !getenv("RX_VK_PRESENT_AHEAD") || atoi(getenv("RX_VK_PRESENT_AHEAD")) != 0;
        const bool ahead = present_ahead_wanted(ahead_env, pace, shown, dev.swap_spare.load(std::memory_order_relaxed));
        PendingPresent pending[kMaxMult];
        if (ahead) {
            static bool said;
            if (!said) said = true, logf("vk: present ahead: a set's copies submitted at once, its presents on time (RX_VK_PRESENT_AHEAD=0: off)");
            for (int k = 0; k < shown; k++) {
                present_kind.store(timed ? (j.set->game[k] ? 0 : 1) : k < j.set->made ? 1 : 0);
                reflex_present_frame = (timed ? j.set->game[k] : k == j.set->made) ? j.set->reflex_id : 0;
                antilag_present_frame = (timed ? j.set->game[k] : k == j.set->made) ? j.set->antilag_id : 0;
                present_defer_into = &pending[k];
                present_image(j.set->frame[k].image, fg.w, fg.h, nullptr, j.set->frame[k].view);
                present_defer_into = nullptr;
            }
            if (pending[0].used) vkWaitForFences(dev.device, 1, &pending[0].done, VK_TRUE, 100000000ull);   // (100 ms at most)
        }
        for (int k = 0; k < shown; k++) {
            // (hookshot) A made frame is left out when the game's next frame is already waiting: the display thread behind
            // the game held its sets, and the game waited for them (the game's rate fell, frame generation on). The set's
            // game frame is always shown. Not with present ahead: its copies are already on their way
            if (!ahead && (timed ? !j.set->game[k] : k < j.set->made)) {
                bool newer;
                {
                    std::lock_guard lk(fg.mu);
                    newer = !fg.jobs.empty();
                }
                if (newer) {
                    stat.skipped++;
                    continue;
                }
            }
            int64_t due = timed ? j.set->due[k] : pace ? next_show : 0;
            const int64_t now = now_ns();
            if (due < now) due = now;
            if (!timed && due > j.at + interval + step) due = j.at + interval + step;
            if (timed && due > now + 100000000) due = now;   // (a game frame time gone wrong: not held back)
            if (due - now > 1000000) wait_until_near(due - now - 1000000);
            while (now_ns() < due) SwitchToThread();
            const int64_t t = now_ns();
            if (ahead) {
                if (!present_commit(pending[k])) {   // (not shown: the line goes on, and the overlay and the counts leave it out)
                    next_show = t + step;
                    continue;
                }
            } else {
                present_kind.store(timed ? (j.set->game[k] ? 0 : 1) : k < j.set->made ? 1 : 0);
                // (NVIDIA Reflex: the set's game frame is presented as the game frame it was made at, made frames as none)
                reflex_present_frame = (timed ? j.set->game[k] : k == j.set->made) ? j.set->reflex_id : 0;
                antilag_present_frame = (timed ? j.set->game[k] : k == j.set->made) ? j.set->antilag_id : 0;
                present_image(j.set->frame[k].image, fg.w, fg.h, nullptr, j.set->frame[k].view);
            }
            menu_note_frame(!timed && k == j.set->made);
            if (shown_at) {
                const int64_t d = t - shown_at;
                hist[d < 7000000 ? 0 : d < 9000000 ? 1 : d < 12000000 ? 2 : d < 17000000 ? 3 : d < 25000000 ? 4 : 5]++;
            }
            shown_at = t;
            next_show = t + step;
            (t - due > 2000000 ? stat.late : stat.on_time)++;
        }
        std::lock_guard lk(fg.mu);
        j.set->busy = false;
        fg.cv.notify_all();
    }
}

// (hookshot) The last sets' makers (framegen_maker): each framegen_record notes its set's on the replay thread; the menu's
// thread reads the counts. A ring of kMakerSets, the counts of each maker in it
constexpr int kMakerSets = 64;
std::atomic<int> maker_count[4] = {kMakerSets, 0, 0, 0};
uint8_t maker_ring[kMakerSets] = {};
int maker_pos = 0;
std::atomic<int64_t> maker_at{0};

void note_maker(FgMaker m) {
    uint8_t& slot = maker_ring[maker_pos];
    maker_count[slot].fetch_sub(1, std::memory_order_relaxed);
    slot = (uint8_t)m;
    maker_count[slot].fetch_add(1, std::memory_order_relaxed);
    maker_pos = (maker_pos + 1) % kMakerSets;
    maker_at.store(now_ns(), std::memory_order_relaxed);
}

} // namespace

FgMaker framegen_maker() {
    // (none recorded for half a second: Frame generation off, or the window's presents without it)
    if (now_ns() - maker_at.load(std::memory_order_relaxed) > 500000000) return FgMaker::None;
    const int b = maker_count[(int)FgMaker::BuiltIn].load(std::memory_order_relaxed);
    const int d = maker_count[(int)FgMaker::Dlss].load(std::memory_order_relaxed);
    const int f = maker_count[(int)FgMaker::Fsr3].load(std::memory_order_relaxed);
    if (!b && !d && !f) return FgMaker::None;
    return d >= b && d >= f ? FgMaker::Dlss : f >= b ? FgMaker::Fsr3 : FgMaker::BuiltIn;
}

// The Insert menu's Frame generation is off at 0. While it was ignored, every present still copied the frame and
// reduced it to luma (framegen_record), then a second thread showed it. Open areas paid for that on a full GPU and
// gained no made frames. Auto (1) and 2x and up are the interpolation; RX_VK_FRAMEGEN=0 keeps it off entirely.
bool framegen_wanted(int menu_value, int allow) {
    return allow && menu_value >= 1;   // (1: Auto)
}

bool framegen_on() {
    static const int allow = !getenv("RX_VK_FRAMEGEN") || atoi(getenv("RX_VK_FRAMEGEN")) != 0;
    return framegen_wanted(opts.framegen, allow) && !dev.offscreen && !fg.failed;
}

// Ultrawide's menus (the Insert menu's Ultrawide menus): which of nx2/oot_wide.py's keep-list screens keep the whole picture, told
// to the game's side each present, and, while a menu shows, the sides of the middle 16:9 to show out of focus (1)
// or black (2), as fractions of the width. A menu's layout is set up once, when it is first opened: what tells one is
// that the frame draws no world (vk_draw.cpp world_drawn_at), as a pause, the map, a loading screen or a movie
// does not (3 presents' grace for a frame the world skips). The middle is centred: its right edge is 1 - x0. One 8-byte atomic: the
// frame generation's display thread reads it (framegen_scale) while the replay thread writes it (wide_sides_tick)
struct WideSides { float x0 = 0.0f; uint16_t side = 0, cover = 0; };   // (cover: of 65535)
// (KEEP_VTABLES 1 and 3: the pause and map menus. 0, the item wheel's background, and 2, the fades, keep covering the whole picture)
constexpr uint32_t kMenus = 0xa;
std::atomic<bool> wide_possible{false};   // (a margin and the setting on: vk_replay.cpp takes the scale pass's path for the sides)
WideSides wide_sides(float cover) {
    WideSides w;
    if (!host || !host->wide_menus) return w;
    const float margin = host->wide_menus(opts.wide_menus ? 0xfu & ~kMenus : 0xfu);
    extern std::atomic<int64_t> world_drawn_at;
    extern int64_t presents_now;
    extern std::atomic<int64_t> wheel_drawn_at;
    // (no world: a menu. But not the item wheel, whose background is the last frame of play over the whole picture, as on the
    // console: its sides are left as the game draws them)
    const bool no_world = presents_now - world_drawn_at.load(std::memory_order_relaxed) > 3 &&
                          presents_now - wheel_drawn_at.load(std::memory_order_relaxed) > 3;
    wide_possible.store(opts.wide_menus && margin > 0.0f, std::memory_order_relaxed);
    if (opts.wide_menus && margin > 0.0f && no_world) w.x0 = margin, w.side = (uint16_t)opts.wide_menus;
    // (the loading cover between two areas is drawn in the middle like the menus, and the world is drawn again under it before
    // it opens: the sides showed the new area beside a black middle, issue 2220. While it shows, the sides are closed by as
    // much as the cover is, fg_scale.comp's cover, with the world drawn or not)
    if (opts.wide_menus && margin > 0.0f && cover > 0.0f) w.x0 = margin, w.cover = (uint16_t)std::lround(std::min(cover, 1.0f) * 65535.0f);
    static bool covered = false;
    if ((w.cover != 0) != covered) {
        covered = w.cover != 0;
        logf("vk: ultrawide loading cover %s at present %lld (the world last drawn at present %lld)", covered ? "shows" : "gone",
             (long long)presents_now, (long long)world_drawn_at.load(std::memory_order_relaxed));
    }
    static int told = 0;
    if (w.side != told) {   // (each change logged: a report of blurred sides in play says when they came and went)
        logf("vk: ultrawide menu sides %s at present %lld (the world last drawn at present %lld)", w.side ? "on" : "off",
             (long long)presents_now, (long long)world_drawn_at.load(std::memory_order_relaxed));
        told = w.side;
    }
    return w;
}
std::atomic<WideSides> wide_now;   // (wide_sides_tick's, each present)
// (the middle 16:9's left edge while Ultrawide menus has the menus there and the picture is wider, as wide_sides has it; else 0)
float wide_margin() { return opts.wide_menus && host && host->wide_menus ? host->wide_menus(0xfu & ~kMenus) : 0.0f; }
bool wide_sides_possible() { return wide_possible.load(std::memory_order_relaxed); }

VkImage framegen_scale(VkCommandBuffer cmd, VkImageView src, VkImage image, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh,
                       bool pq) {
    WideSides wide = wide_now.load(std::memory_order_relaxed);   // (wide_sides_tick, each present)
    if (wide_cover_in_frame) wide.cover = 0;
    if (!opts.upscale && !opts.sharpness && !wide.side && !wide.cover) return VK_NULL_HANDLE;
    if (!src || !image || !sw || !sh || !dw || !dh) return VK_NULL_HANDLE;
    const PresentSourceRect source = present_source_rect(sw, sh, crop);
    if (!source.w || !source.h) return VK_NULL_HANDLE;
    if (!init_scale()) return VK_NULL_HANDLE;
    // (pq: the game's own HDR10 picture (Native HDR), PQ codes in RGB10A2. Scaled into RGB10A2 images by fg_scale_pq: through
    // the RGBA8 ones its codes lost two bits, banding in the dark. Without that pass it is shown unscaled. The images made for
    // the other format are made again)
    if (pq && !scale.pq_tried) {
        scale.pq_tried = true;
        const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (!make_pipe(scale.pipe_pq, FG_SCALE_PQ_COMP_SPV, sizeof FG_SCALE_PQ_COMP_SPV, {S, I, S}, 24))
            logf("vk: the 10-bit window scaling pass could not be made: Native HDR's picture is shown unscaled");
    }
    if (pq && !scale.pipe_pq.pipe) return VK_NULL_HANDLE;
    if (scale.pq != pq) {
        scale.pq = pq;
        scale.cropped_w = scale.cropped_h = scale.scaled_w = scale.scaled_h = scale.small_w = scale.small_h = 0;
    }
    const VkFormat format = pq ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM;
    const Pipe& pipe = pq ? scale.pipe_pq : scale.pipe;
    // (the game draws into part of its window textures, 2048 x 1152 of 2560 x 1440 here: the filter was given the whole texture, and the
    // picture came out in the top left corner of the window with black around it. The crop first, then the filter)
    if (source.x || source.y || source.w != sw || source.h != sh) {
        const uint32_t cw = source.w, ch = source.h;
        if (scale.cropped_w != cw || scale.cropped_h != ch) {
            {
                std::lock_guard qk(dev.present_mu);
                device_wait_idle_locked();
            }
            drop_img(scale.cropped);
            scale.cropped_w = scale.cropped_h = 0;
            if (!make_img(scale.cropped, cw, ch, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
                drop_img(scale.cropped);
                return VK_NULL_HANDLE;
            }
            scale.cropped_w = cw, scale.cropped_h = ch;
            to_general(cmd, scale.cropped.image);
        }
        if (pq) {   // (the bits as they are: a blit converts from the image's own format, which is not always the picture's
                    // (vk_texture.cpp format_class shares images by class); a copy keeps the game's 10-bit codes either way)
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.srcOffset = {(int)source.x, (int)source.y, 0};
            c.extent = {cw, ch, 1};
            vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, scale.cropped.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        } else {
            VkImageBlit b{};
            b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[0] = {(int)source.x, (int)source.y, 0};
            b.srcOffsets[1] = {(int)(source.x + cw), (int)(source.y + ch), 1};
            b.dstOffsets[1] = {(int)cw, (int)ch, 1};
            vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, scale.cropped.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
        }
        full_barrier(cmd);
        src = scale.cropped.view;
    }
    if (scale.scaled_w != dw || scale.scaled_h != dh) {
        {
            std::lock_guard qk(dev.present_mu);   // (the queue is idle only while nothing else submits to it)
            device_wait_idle_locked();
        }
        drop_img(scale.scaled);
        scale.scaled_w = scale.scaled_h = 0;
        // (sampled too: with HDR on, framegen_hdr reads the scaled picture through a sampler. Without the usage that read is
        // undefined, and an upscaler or a Sharpness above 0 (every preset) gave a black, grey or purple HDR picture where 0 did not)
        if (!make_img(scale.scaled, dw, dh, format,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            drop_img(scale.scaled);
            return VK_NULL_HANDLE;
        }
        scale.scaled_w = dw, scale.scaled_h = dh;
        to_general(cmd, scale.scaled.image);
    }
    struct { int mode; float sharp; float x0, x1; int side; float cover; } pc{opts.upscale, opts.sharpness / 100.0f, wide.x0, 1.0f - wide.x0, wide.side,
                                                                              wide.cover / 65535.0f};
    if (wide.side == 1) {   // (the blurred sides: first the picture at 1/8, each pixel the mean of its 8 x 8, which they blur)
        // (from #1448's checked source rectangle, as the crop above: the picture src now holds)
        const uint32_t smw = std::max(1u, source.w / 8), smh = std::max(1u, source.h / 8);
        if (scale.small_w != smw || scale.small_h != smh) {
            {
                std::lock_guard qk(dev.present_mu);
                device_wait_idle_locked();
            }
            drop_img(scale.small);
            scale.small_w = scale.small_h = 0;
            if (!make_img(scale.small, smw, smh, format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
                drop_img(scale.small);
                return VK_NULL_HANDLE;
            }
            scale.small_w = smw, scale.small_h = smh;
            to_general(cmd, scale.small.image);
        }
        pc.side = 3;   // (fg_scale.comp: make the small copy)
        pass(cmd, pipe, {{src, scale.linear}, {scale.small.view, VK_NULL_HANDLE}, {src, scale.linear}}, smw, smh, &pc, sizeof pc);
        full_barrier(cmd);
        pc.side = 1;
    }
    pass(cmd, pipe, {{src, scale.linear}, {scale.scaled.view, VK_NULL_HANDLE}, {wide.side == 1 ? scale.small.view : src, scale.linear}},
         dw, dh, &pc, sizeof pc);
    static const bool report = env_on("RX_VK_UPSCALE_LOG");
    static bool reported = false;
    if (report && !reported) {
        reported = true;
        logf("vk: upscale dispatched: mode %d, sharpness %d%% (of RCAS's strongest, neighbours one source pixel apart), source %ux%u, "
             "crop request %d,%d %dx%d, destination %ux%u",
             opts.upscale, opts.sharpness, sw, sh, crop ? crop[0] : 0, crop ? crop[1] : 0,
             crop ? crop[2] : (int)sw, crop ? crop[3] : (int)sh, dw, dh);
    }
    return scale.scaled.image;
}

VkImageView framegen_scaled_view() { return scale.scaled.view; }

/// (hookshot) The Insert menu's HDR: src's crop to a dw x dh HDR10 image (PQ, Rec. 2020, 10 bits), to be copied into an HDR10
/// swapchain's image as the SDR picture is (shaders/hdr_encode.comp); null when it cannot be made (then the SDR picture;
/// Native's scaled picture, else black).
/// src is the SDR picture, expanded, or with pq the game's own HDR10 picture (Native); either graded by the HDR contrast row
VkImage framegen_hdr(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh,
                     bool pq) {
    if (!src || !sw || !sh || !dw || !dh) return VK_NULL_HANDLE;
    const PresentSourceRect source = present_source_rect(sw, sh, crop);
    if (!source.w || !source.h) return VK_NULL_HANDLE;
    if (!hdr.init) {
        hdr.init = hdr.failed = true;
        const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (dev.push_descriptor && vkCmdPushDescriptorSetKHR && init_scale() &&
            make_pipe(hdr.pipe, HDR_ENCODE_COMP_SPV, sizeof HDR_ENCODE_COMP_SPV, {S, I}, 48))
            hdr.failed = false;
        else
            logf("vk: HDR: the HDR pass could not be made: SDR");
    }
    if (hdr.failed) return VK_NULL_HANDLE;
    if (hdr.w != dw || hdr.h != dh) {
        {
            std::lock_guard qk(dev.present_mu);
            device_wait_idle_locked();
        }
        drop_img(hdr.out);
        hdr.w = hdr.h = 0;
        if (!make_img(hdr.out, dw, dh, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            drop_img(hdr.out);
            static bool told;
            if (!told) told = true, logf("vk: HDR: no 10-bit storage image on this GPU: SDR");
            return VK_NULL_HANDLE;
        }
        hdr.w = dw, hdr.h = dh;
        to_general(cmd, hdr.out.image);
    }
    const float x0 = source.x / (float)sw, y0 = source.y / (float)sh;
    const float x1 = (source.x + source.w) / (float)sw, y1 = (source.y + source.h) / (float)sh;
    // (hdr_encode.comp's push constants: the crop, paper white, peak, knee, mode, contrast, exposure (1: none), two pads)
    const struct { float uv[4]; float white, peak, knee, mode, contrast, exposure, pad0, pad1; } pc{
        {x0, y0, x1, y1}, (float)std::clamp(opts.hdr_white, 80, 500), (float)std::clamp(opts.hdr_peak, 400, 10000), 0.6f,
        pq ? 1.0f : 0.0f, std::clamp(opts.hdr_contrast, 50, 200) / 100.0f, 1.0f, 0.0f, 0.0f};
    pass(cmd, hdr.pipe, {{src, scale.linear}, {hdr.out.view, VK_NULL_HANDLE}}, dw, dh, &pc, sizeof pc);
    return hdr.out.image;
}

VkImage framegen_made_image(uint32_t* w, uint32_t* h) {
    if (!fg.recorded || !fg.recorded->made) return VK_NULL_HANDLE;
    *w = fg.w, *h = fg.h;
    return fg.recorded->frame[0].image;
}

/// (hookshot) The made frame of a 2x set by AMD FSR 3.1's frame interpolation (vk_fsr3fi.h), from this frame's colour and
/// the scene's depth and motion (vk_fsr3_inputs.h): 1 when it made it into set.frame[0], 0 when it did not (no inputs this
/// present, a reset, anything not made), which leaves the set to the interpolation below. sequence: framegen_record's count
/// of its presents (one it did not hand over, a gap among them, makes the next one a reset: vk_fsr3fi.h Inputs::sequence)
int fsr3fi_frame(VkCommandBuffer cmd, OutSet& set, int cur, uint32_t uw, uint32_t uh, uint64_t sequence) {
    // (every 600 presents asked: how many FSR made, and how many it left to the interpolation below and why)
    static uint64_t asked = 0, made = 0, no_inputs = 0;
    if (++asked % 600 == 0)
        logf("vk: fsr3fi: %llu of the last 600 made frames by FSR 3.1, %llu without its inputs (the renderer's own made them)",
             (unsigned long long)made, (unsigned long long)no_inputs),
            made = no_inputs = 0;
    fsr3_inputs::Frame f;
    if (!fsr3_inputs::take(f)) return no_inputs++, 0;
    fsr3fi::Inputs in;
    in.color = fg.frames[cur].view;
    in.color_image = fg.frames[cur].image;
    in.color_format = VK_FORMAT_R8G8B8A8_UNORM;   // (ensure_size, for fg.frames)
    in.width = uw, in.height = uh;
    in.depth = f.depth, in.motion = f.motion;
    in.render_width = f.width, in.render_height = f.height;
    in.jitter[0] = f.jitter[0], in.jitter[1] = f.jitter[1];
    in.camera = f.camera;
    in.frame_time_ms = f.frame_ms;
    in.reset = f.reset;
    in.sequence = sequence;
    if (!fsr3fi::generate(cmd, in) || !fsr3fi::made_image()) return 0;
    const VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                        {uw, uh, 1}};
    vkCmdCopyImage(cmd, fsr3fi::made_image(), VK_IMAGE_LAYOUT_GENERAL, set.frame[0].image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    made++;
    static bool told = false;
    if (!told) {
        told = true;
        logf("vk: fsr3fi: AMD FSR 3.1 makes the frame between each two of the game's (%ux%u, its depth and motion %ux%u)", uw,
             uh, f.width, f.height);
    }
    return 1;
}

bool framegen_record(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], float cover) {
    const PresentSourceRect source = present_source_rect(sw, sh, crop);
    const int x0 = (int)source.x, y0 = (int)source.y, w = (int)source.w, h = (int)source.h;
    if (!src || w <= 0 || h <= 0 || !init() || !ensure_size(cmd, (uint32_t)w, (uint32_t)h)) return false;
    if (!fg.thread_started) {
        fg.thread_started = true;
        std::thread(display_main).detach();
    }
    const int64_t now = now_ns(), before = fg.last_at;
    const bool gap = fg.last_at && now - fg.last_at > 100000000;   // (a loading hitch: nothing to blend)
    if (fg.last_at && !gap)
        fg.game_ns = fg.game_ns ? (fg.game_ns * 7 + (now - fg.last_at)) / 8 : now - fg.last_at;
    fg.last_at = now;
    OutSet* set;
    {
        std::unique_lock lk(fg.mu);
        fg.cv.wait(lk, [] { return !fg.sets[fg.next_set].busy; });
        set = &fg.sets[fg.next_set];
        set->busy = true;
        fg.next_set = (fg.next_set + 1) % 3;
    }
    // (the multiplier the sets were made for, ensure_size above: the row read again here could be one step up by now, the
    // menu runs on the display thread, and the set's last image would be one that was never made)
    const int cur = fg.cur, prev = cur ^ 1, m = fg.mult;
    const bool auto_rate = opts.framegen == 1;   // (read once, for the same reason)
    const bool blend = fg.have_prev && !gap;
    set->reflex_id = reflex_frame();   // (NVIDIA Reflex: the game frame this set shows, vk_reflex.cpp)
    set->antilag_id = antilag_frame();   // (AMD Anti-Lag 2: the same, vk_antilag.cpp)
    static uint64_t sequence = 0;
    sequence++;
    const uint32_t uw = (uint32_t)w, uh = (uint32_t)h;
    const uint32_t qw = (uw + 3) / 4, qh = (uh + 3) / 4, cw = (qw + 3) / 4, ch = (qh + 3) / 4;
    full_barrier(cmd);
    // (ultrawide's loading cover, wide_sides: the sides closed in the copy, before the frames between are made from it, so
    // that whichever interpolation makes them treats the sides as it does the middle; the display thread's scale pass then
    // leaves them as they are, wide_cover_in_frame)
    const float edge = cover > 0.0f ? wide_margin() : 0.0f;
    extern std::atomic<bool> present_native_pq;   // (vk_replay.cpp do_present: the game presents its own HDR10 picture, Native)
    const struct { int32_t rect[4]; float x0, cover; int32_t pq; } pc = {{x0, y0, w, h}, edge, edge > 0.0f ? std::min(cover, 1.0f) : 0.0f,
                                                                         present_native_pq.load(std::memory_order_relaxed)};
    pass(cmd, fg.copy, {{src, fg.nearest}, {fg.frames[cur].view, VK_NULL_HANDLE}}, uw, uh, &pc, sizeof pc);
    pass(cmd, fg.luma, {{fg.frames[cur].view, fg.nearest}, {fg.lumas[cur].view, VK_NULL_HANDLE}}, qw, qh);
    set->made = 0;
    set->shown = -1;
    if (auto_rate) {   // (Auto: a frame for each of the display's refreshes, at its own time)
        const int mhz = display_refresh_mhz();
        const int64_t period = (int64_t)(1e12 / (mhz > 0 ? mhz : 60000));
        float t[kMaxMult];
        int64_t at[kMaxMult];
        int n;
        if (blend) {
            n = auto_slots(before, now, period, fg.slot_next, t, at, std::min(fg.mult, kMaxMult));
        } else {   // (nothing to blend from: the game's frame, and the timeline starts at it)
            n = 1, t[0] = 1.0f, at[0] = now, fg.slot_next = now + period;
        }
        bool flow = false;
        for (int k = 0; k < n; k++) {
            set->game[k] = t[k] > 0.999f;
            set->due[k] = at[k] + fg.game_ns;
            if (set->game[k]) {
                const VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    {0, 0, 0}, {uw, uh, 1}};
                vkCmdCopyImage(cmd, fg.frames[cur].image, VK_IMAGE_LAYOUT_GENERAL, set->frame[k].image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                continue;
            }
            if (!flow) {
                flow = true;
                pass(cmd, fg.flow, {{fg.lumas[prev].view, fg.nearest}, {fg.lumas[cur].view, fg.nearest},
                                    {fg.flows[0].view, VK_NULL_HANDLE}}, cw, ch);
                pass(cmd, fg.median, {{fg.flows[0].view, fg.nearest}, {fg.flows[1].view, VK_NULL_HANDLE}}, cw, ch);
            }
            pass(cmd, fg.synth, {{fg.frames[prev].view, fg.linear}, {fg.frames[cur].view, fg.linear},
                                 {fg.flows[1].view, fg.nearest}, {set->frame[k].view, VK_NULL_HANDLE}},
                 uw, uh, &t[k], 4);
        }
        set->shown = n;
        set->made = n > 0 && !set->game[n - 1] ? n : std::max(0, n - 1);   // (for framegen_made_image: any made)
        note_maker(flow ? FgMaker::BuiltIn : FgMaker::None);   // (Auto: the renderer's own, DLSS-G is not asked)
        full_barrier(cmd);
        fg.have_prev = true;
        fg.cur = prev;
        fg.recorded = set;
        fg.recorded_at = now;
        return true;
    }
    // (hookshot) NVIDIA DLSS Frame Generation through NGX where it runs and the Frame generation by row asks for it (vk_dlss.cpp):
    // every made frame of the set, from the scene's own depth, motion and camera. A set it does not make goes on below as before
    // (asked every present: turned off, it lets its feature go)
    if (m > 1) {
        VkImage outs[kMaxMult];
        VkImageView views[kMaxMult];
        for (int k = 0; k < m - 1; k++) outs[k] = set->frame[k].image, views[k] = set->frame[k].view;
        set->made = dlss_fg_frames(cmd, fg.frames[cur].image, fg.frames[cur].view, uw, uh, m - 1, outs, views, blend, sequence);
    }
    FgMaker maker = set->made ? FgMaker::Dlss : FgMaker::None;
    // (hookshot) At 2x, AMD FSR 3.1's frame interpolation where it runs; it makes the midpoint only, so 3x and more (and Auto,
    // above) stay with the interpolation below, as does any present it does not take
    // (hookshot) the frames this game frame gets: as many as the display shows (fixed_frames), DLSS's own above excepted
    static const bool uncapped = getenv("RX_VK_FRAMEGEN_UNCAPPED") && atoi(getenv("RX_VK_FRAMEGEN_UNCAPPED")) != 0;
    const int mk = uncapped ? m : fixed_frames(m, (display_refresh_mhz() + 500) / 1000, fg.game_ns);
    {
        static int said = -1;
        static int64_t said_at = 0;
        if (mk != said && (said < 0 || now - said_at > 5000000000LL)) {
            if (mk < m)
                logf("vk: frame interpolation: %dx asked, %dx made: the display's %d Hz over the game's %.0f fps (the game keeps its "
                     "rate; RX_VK_FRAMEGEN_UNCAPPED=1: all)", m, mk, (display_refresh_mhz() + 500) / 1000,
                     fg.game_ns > 0 ? 1e9 / (double)fg.game_ns : 0.0);
            else if (said >= 0)
                logf("vk: frame interpolation: %dx made", mk);
            said = mk, said_at = now;
        }
    }
    if (set->made == 0 && blend && mk == 2 && fsr3fi::frames_wanted()) set->made = fsr3fi_frame(cmd, *set, cur, uw, uh, sequence);
    if (maker == FgMaker::None && set->made) maker = FgMaker::Fsr3;
    if (set->made == 0 && blend && mk > 1) {
        pass(cmd, fg.flow, {{fg.lumas[prev].view, fg.nearest}, {fg.lumas[cur].view, fg.nearest},
                            {fg.flows[0].view, VK_NULL_HANDLE}}, cw, ch);
        pass(cmd, fg.median, {{fg.flows[0].view, fg.nearest}, {fg.flows[1].view, VK_NULL_HANDLE}}, cw, ch);
        for (int k = 1; k < mk; k++) {
            const float t = (float)k / (float)mk;
            pass(cmd, fg.synth, {{fg.frames[prev].view, fg.linear}, {fg.frames[cur].view, fg.linear},
                                 {fg.flows[1].view, fg.nearest}, {set->frame[k - 1].view, VK_NULL_HANDLE}},
                 uw, uh, &t, 4);
        }
        set->made = mk - 1;
        maker = FgMaker::BuiltIn;
    }
    note_maker(maker);
    const VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                        {uw, uh, 1}};
    vkCmdCopyImage(cmd, fg.frames[cur].image, VK_IMAGE_LAYOUT_GENERAL, set->frame[set->made].image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cmd);
    fg.have_prev = true;
    fg.cur = prev;
    fg.recorded = set;
    fg.recorded_at = now;
    return true;
}

void framegen_queue(uint64_t value) {
    if (!fg.recorded) return;
    std::lock_guard lk(fg.mu);
    fg.jobs.push_back({fg.recorded, fg.recorded_at, value});
    fg.recorded = nullptr;
    fg.cv.notify_all();
}

/// Each present (vk_replay.cpp): ultrawide's menu setting to the game's side, and the sides to treat now
void wide_sides_tick(float cover) { wide_now = wide_sides(cover); }
// (the reading is of the frame the game works on, which is the presented one or the one after it as its threads fall (measured:
// an opening cover read a present ahead of the picture through one area's entry, and by none or one through another's). So a
// frame's sides are closed by the larger of its reading and the one before: never more open than the middle, at most a frame
// behind it)
float wide_cover_held(float now) {
    static float before = 0.0f;   // (the game's presenting thread's alone)
    const float cover = std::max(now, before);
    before = now;
    return cover;
}
float wide_cover_sample() {
    return wide_cover_held(host && host->wide_cover && wide_possible.load(std::memory_order_relaxed) ? host->wide_cover() : 0.0f);
}

} // namespace mkwvk
