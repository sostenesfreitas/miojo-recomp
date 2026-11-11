// (hookshot) A temporal upscaler in place of OoT's gameplay upscale network: the frame chain (vk_temporal.h), shared by the
// engines that draw with it, NVIDIA DLSS (vk_dlss.cpp) and AMD FSR 4 (vk_fsr4.cpp).
// The chain is hckrman101's, from their NVIDIA DLSS (#841): the camera, the inputs pass, the actor motion and the jitter's
// state, moved here from vk_dlss.cpp unchanged in what they do, so that DLSS and FSR share them.
//
// OoT has no temporal upscaler of its own: its scene is drawn at a lower size and a network of its own (the program
// fd029a317a9a4b94 at 3/4, another for each other share; one dispatch, cooperative matrices) scales it up, from the
// square root of the scene's tonemapped colour, into the frame's B10G11R11 picture (before the HUD). Here that dispatch is replaced:
//  - the scene's camera is taken from the draws into the scene's colour target: their vertex stage's uniform buffer 3
//    holds the target's size at 0x120, this frame's view-projection at 0x30 (row major, clip = M * position; the projection's w row ends -1, 0 at 0xa8). The largest such target in a frame is
//    the scene (reflections and half-size passes have the same layout). The scene's depth is that draw's depth target.
//  - shaders/dlss_prep.comp makes the engine's inputs at the render size: the colour as the network samples it (LDR), the
//    depth, and motion vectors from the depth reprojected through the two cameras. The game has no motion vectors of its
//    own (no velocity target: the scene pass draws into one B10G11R11 target, and its upscale network samples the colour alone),
//    so that is the camera's motion, and what moves by itself would ghost. The scene camera's sub-pixel jitter
//    (nx2/oot_jitter.py, while an engine draws) is read back from its projection, given to the engine and taken off the motion.
//  - actor motion (actor_motion, shaders/dlss_actor.vert): the skinned draws into the scene (vk_draw.cpp's skin_check:
//    Link, the other characters, the animals, what the game animates with bones) are drawn again over that motion, each
//    vertex skinned with this frame's bones and the frame before's, so their pixels get their own motion. Their poses are
//    copied on the GPU from where the game has them into two buffers that take turns; a pixel is the draw's where its depth is
//    the scene's (within RX_VK_DLSS_ACTOR_TOL steps, default 64). What moves without bones (particles, water, foliage moved in
//    its vertex shader, mesh-shader draws) keeps the camera's motion. RX_VK_DLSS_ACTORS=0: camera motion alone.
//  - the engine scales them to the output's size; shaders/dlss_out.comp squares that back to linear colour into the
//    network's output.
// Anything unexpected (no camera this frame, other sizes, the engine not there or failing) leaves the network to run, the
// reason in the log when it changes.
//
// The Insert menu's Upscaler row (opts.dlss, upscaler_rows.h) chooses the engine; RX_VK_DLSS=1 / 2 (DLSS, DLAA) or
// RX_VK_FSR4=1 / 2 (FSR 4, its native anti-aliasing) over it, and =0 takes that engine out for the run. AMD FSR 3.1
// (vk_fsr3.cpp, the row's value 3 or RX_VK_FSR=1) has inputs of its own (vk_fsr3_inputs.cpp): while it upscales, this chain
// stands aside.
// RX_VK_DLSS_DEBUG=motion: the motion vectors shown instead of the picture (|x|, |y| over 16 px as red and green, the actors'
// in it); =alternate: every other frame the game's own upscale (a test's reference beside the engine's). (The names are
// DLSS's from before FSR came: they apply to either engine.)
#include "vk_temporal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dlss_actor.h"
#include "dlss_checks.h"
#include "dlss_math.h"
#include "temporal_jitter.h"
#include "upscaler_rows.h"
#include "vk_dlss.h"
#include "vk_draw.h"
#include "vk_fsr3.h"   // (fsr3_wanted: AMD FSR 3.1 upscales with inputs of its own)
#include "vk_fsr4.h"
#include "vk_menu.h"
#include "vk_rt.h"
#include "vk_texture.h"

#include "shaders/dlss_actor_frag_spv.h"
#include "shaders/dlss_actor_vert_spv.h"
#include "shaders/dlss_out_comp_spv.h"
#include "shaders/dlss_prep_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {

namespace ur = upscaler_rows;

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
}  // namespace

namespace {
int env_dlss() { static const int v = env_switch("RX_VK_DLSS"); return v; }   // (read at the first ask, as the others)
int env_fsr4() { static const int v = env_switch("RX_VK_FSR4"); return v; }
}  // namespace

bool temporal_env_chosen() {
    // (only where the engine asked for runs: else a saved FSR 3.1 stays, not the game's own. DLSS before FSR 4, as below)
    if (env_dlss() > 0) return dlss_why() == DLSS_OK;
    return env_fsr4() > 0 && fsr4_why() == FSR_OK;
}

int temporal_mode() {
    const int dlss = env_dlss(), fsr4 = env_fsr4();
    int v = opts.dlss;
    if (dlss > 0) v = dlss >= 2 ? ur::DLAA : ur::DLSS;   // (2 and up: DLAA, as RX_VK_DLSS was clamped before)
    else if (fsr4 > 0) v = fsr4 >= 2 ? ur::FSR4_AA : ur::FSR4;
    // (FSR 3.1, the row's 3, is not an engine of this chain: the game's own as far as it is concerned. Nor anything while
    // FSR 3.1 upscales: one upscaler at a time, and one camera jitter. A switch above goes over the row's FSR 3.1 too
    // (fsr3_wanted asks temporal_env_chosen); RX_VK_FSR=1 goes over them)
    if (!ur::is_dlss(v) && !ur::is_fsr4(v)) return ur::GAME;
    if (fsr3_wanted()) return ur::GAME;
    if (ur::is_dlss(v) && dlss_why() != DLSS_OK) return ur::GAME;
    if (ur::is_fsr4(v) && fsr4_why() != FSR_OK) return ur::GAME;
    return v;
}
bool temporal_wanted() { return temporal_mode() != ur::GAME; }
bool temporal_full_scale() { return ur::full_scale(temporal_mode()); }

namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    TemporalImage shown() const { return {image, view, format, width, height}; }
};

struct Camera {
    float current[16]{};      // (its view-projection)
    float jitter[2]{};        // (the projection's offset of the picture's centre, in clip space: nx2/oot_jitter.py)
    float near_z = 0, far_z = 0, fov_y = 0;   // (its projection's, 0 when it is not a plain perspective one)
    float proj[4]{};          // (miojo: the projection's [0][0], [1][1], [2][2], [2][3]: dlss_prep.comp's ambient occlusion)
    uint32_t width = 0, height = 0;
    Texture* depth = nullptr;
    uint64_t depth_gen = 0;   // (texture_destroy_generation when taken: a depth texture destroyed since is not used)
    uint64_t frame = 0;       // (texture_frame_no when taken)
    bool valid = false;
};

/// An engine as the chain keeps it: started once, failed for good once
struct Engine {
    const TemporalEngine* e = nullptr;
    bool started = false, ok = false;
    std::atomic<bool> failed{false};   // (atomic: temporal_jitter_size reads it on the game's thread, without the lock)
};

struct State {
    std::mutex mu;
    Engine engines[2];               // (DLSS, FSR: engine_of)
    const TemporalEngine* drawing = nullptr;   // (the engine that drew last: another one starts a new history)
    Image color, depth, motion, output;
    VkDescriptorSetLayout prep_set = VK_NULL_HANDLE, out_set = VK_NULL_HANDLE;
    VkPipelineLayout prep_layout = VK_NULL_HANDLE, out_layout = VK_NULL_HANDLE;
    VkPipeline prep = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    Camera cam;               // (this frame's, since the last upscale)
    // (the gameplay upscale's output, as last seen: the scene sizes the upscaler can take follow from it,
    // temporal_scene_size. Read on the game's draws without the lock)
    std::atomic<uint32_t> out_w{0}, out_h{0};
    std::atomic<uint64_t> last_frame{0};   // (the frame an engine last drew: read on the game's thread too)
    float last_jitter[2]{};   // (its camera's jitter: the frame before's, for the next one)
    float last_vp[16]{};      // (its camera's view-projection: the frame before's)
    TemporalJitter jitter;    // (one completed scene size, sequence and frame)
    std::unordered_map<VkImage, VkImageView> depth_views;
    uint64_t depth_views_gen = 0;
    const char* reason = nullptr;
    int reason_repeats = 0;
    const TemporalEngine* told_first = nullptr;
    bool told_camera = false;
    // (actor motion: the skinned draws into the scene since the last upscale; the poses the engine last drew, by draw: the
    // frame before's, where they were copied to in one of the two pose buffers (each frame's poses go to the other one))
    struct Skin {
        SkinnedDraw k;
        uint64_t key = 0;
        uint32_t nbones = 0;
    };
    std::vector<Skin> skins;
    struct Pose {
        uint32_t at = 0, nbones = 0;   // (bytes into its buffer: the origin, then the bones from at + 16)
        uint64_t frame = 0;
    };
    std::unordered_map<uint64_t, Pose> poses;
    std::unordered_map<uint64_t, uint32_t> model_bones;   // (by model: the bones its vertices use, 0 when unreadable)
    struct PoseBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        uint64_t address = 0;
    } pose_buffers[2];
    int pose_at = 0;            // (this frame's)
    VkPipelineLayout actor_layout = VK_NULL_HANDLE;
    VkPipeline actor = VK_NULL_HANDLE;
    bool actor_tried = false;
    struct Upload {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* map = nullptr;
    } uploads[6];               // (the cameras: a ring, one in use stays so for a few frames, as what later() frees)
    int upload_at = 0;
    uint64_t actor_draws = 0, actor_frames = 0, actor_skipped = 0;
    int64_t actor_ns = 0;       // (the CPU's time in it, since the last status line)
    uint64_t status_draws = 0, status_frames = 0;   // (actor_draws and _frames at the last status line: temporal_status)
    uint64_t status_drawn = 0, status_passed = 0;   // (since the last status line: frames an engine drew, and frames a network
                                                    // of the upscale's shape ran without it (its scene not this frame's))
    uint64_t engines_made = 0;  // (prepare's new histories: a feature or context made again, each a stall in play)
    float status_jitter[2]{};   // (the last evaluation's jitter, in render pixels, and whether it had motion)
    bool status_motion = false;
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;   // (what work in flight may still use: freed a few frames on)
    uint64_t asked = 0;       // (the frame an upscale last asked for an engine: temporal_idle)
    std::atomic<bool> holding{false};   // (an engine's state, images or frees still to do: temporal_idle has something to let go)
};
State& st() {
    // (never destroyed: Hookshot leaves through ExitProcess / exit() with the device alive, and `dead` may still hold an
    // engine's state then (an FSR context), whose destructor would call into a library already gone)
    static State& s = *new State;
    return s;
}

/// The gameplay upscale's output size, as it is seen: the scene sizes the upscaler can take (temporal_scene_size). Until the
/// first one, none: that frame keeps the game's upscale
void note_output(const Texture* out, uint32_t level) {
    State& s = st();
    s.out_w.store(std::max(1u, out->width >> level), std::memory_order_relaxed);
    s.out_h.store(std::max(1u, out->height >> level), std::memory_order_relaxed);
}

/// The chain's slot for the engine of an Upscaler value (DLSS and DLAA: DLSS's; FSR 4 and its native AA: FSR's)
Engine& engine_of(int mode) {
    State& s = st();
    Engine& k = s.engines[ur::is_fsr4(mode) ? 1 : 0];
    if (!k.e) k.e = ur::is_fsr4(mode) ? fsr4_engine() : dlss_engine();
    return k;
}
const char* name_of(int mode) {
    switch (mode) {
    case ur::DLSS: return "NVIDIA DLSS";
    case ur::DLAA: return "NVIDIA DLAA";
    case ur::FSR4: return "AMD FSR 4";
    case ur::FSR4_AA: return "AMD FSR 4 Native AA";
    default: return "the game's own";
    }
}

bool fail(const char* why) {   // (the network runs: why, when it changes and every 600th time)
    State& s = st();
    if (why != s.reason || ++s.reason_repeats % 600 == 0)
        logf("vk: %s: the game's upscale runs (%s)", name_of(temporal_mode()), why);
    if (why != s.reason) s.reason = why, s.reason_repeats = 0;
    return false;
}

void later(std::function<void()> f) { st().dead.push_back({texture_frame_no(), std::move(f)}); }
void free_dead(bool all) {
    State& s = st();
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < s.dead.size();) {
        if (all || now - s.dead[i].frame > 4) {
            s.dead[i].free();
            s.dead[i] = std::move(s.dead.back());
            s.dead.pop_back();
        } else {
            i++;
        }
    }
}

void drop_image(Image& im) {
    if (!im.image) return;
    const Image old = im;
    later([old] {
        vkDestroyImageView(dev.device, old.view, nullptr);
        destroy_image(old.image, old.memory);
    });
    im = {};
}

bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags extra = 0) {
    if (im.image && im.format == format && im.width == w && im.height == h) return true;
    drop_image(im);
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
    if (!create_image(ic, im.image, im.memory)) return im = {}, false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = im.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &im.view) != VK_SUCCESS) {
        destroy_image(im.image, im.memory);
        return im = {}, false;
    }
    im.format = format, im.width = w, im.height = h;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

/// One compute pipeline with push descriptors; what it made is destroyed again when a step fails
bool make_pipeline(const uint32_t* code, size_t size, const VkDescriptorType* types, uint32_t n, uint32_t push,
                   VkDescriptorSetLayout& set, VkPipelineLayout& layout, VkPipeline& pipe) {
    const auto undo = [&] {
        if (layout) vkDestroyPipelineLayout(dev.device, layout, nullptr);
        if (set) vkDestroyDescriptorSetLayout(dev.device, set, nullptr);
        set = VK_NULL_HANDLE, layout = VK_NULL_HANDLE, pipe = VK_NULL_HANDLE;
        return false;
    };
    VkDescriptorSetLayoutBinding b[8]{};
    for (uint32_t i = 0; i < n; i++) b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = n, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return set = VK_NULL_HANDLE, undo();
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout) != VK_SUCCESS) return layout = VK_NULL_HANDLE, undo();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size, mi.pCode = code;
    VkShaderModule m{};
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return undo();
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = layout;
    const bool ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS;
    vkDestroyShaderModule(dev.device, m, nullptr);
    return ok || undo();
}

/// The two passes' pipelines and the samplers, once (temporal_upscale stops trying when this fails)
bool make_pipelines() {
    State& s = st();
    if (s.prep && s.out && s.point && s.linear) return true;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType prep_types[5] = {S, S, I, I, I}, out_types[2] = {S, I};
    if (!s.prep && !make_pipeline(DLSS_PREP_COMP_SPV, sizeof DLSS_PREP_COMP_SPV, prep_types, 5, 104, s.prep_set, s.prep_layout, s.prep))
        return false;
    if (!s.out && !make_pipeline(DLSS_OUT_COMP_SPV, sizeof DLSS_OUT_COMP_SPV, out_types, 2, 8, s.out_set, s.out_layout, s.out))
        return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!s.point && vkCreateSampler(dev.device, &si, nullptr, &s.point) != VK_SUCCESS) return s.point = VK_NULL_HANDLE, false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (!s.linear && vkCreateSampler(dev.device, &si, nullptr, &s.linear) != VK_SUCCESS) return s.linear = VK_NULL_HANDLE, false;
    return true;
}

/// A view of a depth texture's depth alone, for sampling (made once per image)
VkImageView depth_view(Texture* t) {
    State& s = st();
    const uint64_t gen = texture_destroy_generation();
    if (gen != s.depth_views_gen) {   // (a texture was destroyed: its image's view may be stale)
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
        s.depth_views_gen = gen;
    }
    if (auto it = s.depth_views.find(t->image); it != s.depth_views.end()) return it->second;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t->image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = t->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vc, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    s.depth_views[t->image] = v;
    return v;
}

// ---- actor motion: the skinned draws drawn again into the motion, each pixel they show moved by its own bones ----

using ActorPush = dlss_actor::Push;

/// Its pipeline, once (false: none, the motion stays the camera's)
bool make_actor_pipeline() {
    State& s = st();
    if (s.actor) return true;
    if (s.actor_tried) return false;
    s.actor_tried = true;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    if (!dev.vk12.bufferDeviceAddress || !f.features.shaderInt64) {   // (its shaders read the game's buffers by their addresses)
        logf("vk: upscaler: no buffer device addresses or 64-bit integers in shaders: camera motion alone");
        return false;
    }
    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2, li.pBindings = b;
    VkDescriptorSetLayout set{};
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ActorPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &s.actor_layout) != VK_SUCCESS) return false;
    VkShaderModule m[2]{};
    const uint32_t* code[2] = {DLSS_ACTOR_VERT_SPV, DLSS_ACTOR_FRAG_SPV};
    const size_t size[2] = {sizeof DLSS_ACTOR_VERT_SPV, sizeof DLSS_ACTOR_FRAG_SPV};
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size[i], mi.pCode = code[i];
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m[i]) != VK_SUCCESS) return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = m[i], stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2, ds.pDynamicStates = dyn;
    const VkFormat format = VK_FORMAT_R16G16_SFLOAT;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2, gi.pStages = stages;
    gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia, gi.pViewportState = &vp, gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms, gi.pColorBlendState = &cb, gi.pDynamicState = &ds, gi.layout = s.actor_layout;
    const bool ok = vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &gi, nullptr, &s.actor) == VK_SUCCESS;
    for (VkShaderModule x : m) vkDestroyShaderModule(dev.device, x, nullptr);
    if (!ok) s.actor = VK_NULL_HANDLE, logf("vk: upscaler: the actor motion's pipeline was not made: camera motion alone");
    return ok;
}

/// A range of guest memory as this recording binds it, by its device address (the pools and their mirrors have them)
uint64_t device_address(const BufferRef& r) {
    if (!r) return 0;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = r.buffer;
    const VkDeviceAddress base = vkGetBufferDeviceAddress(dev.device, &da);
    return base ? base + r.offset : 0;
}

constexpr uint64_t kPoseBytes = 4 << 20;   // (a pose buffer: 280 bones of 48 bytes are 13 KB a draw, 300 of them)

/// The skinned draws into the scene (cam's depth) with a pose of the frame before (the engine's last), drawn into the motion
/// image over the camera's motion. Each one's bones and origin are copied on the GPU, from where the game has them, into
/// this frame's pose buffer, for this frame and the next. before: the frame before's camera without its jitter. history:
/// the engine has the frame before. Returns the draws drawn
int actor_motion(VkCommandBuffer cmd, const Camera& cam, const float* before, uint32_t rw, uint32_t rh, VkImageView dview,
                 bool history) {
    State& s = st();
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    std::vector<State::Skin> skins = std::move(s.skins);
    s.skins.clear();
    std::unordered_map<uint64_t, State::Pose> old = std::move(s.poses);
    s.poses.clear();
    static const bool off = env_switch("RX_VK_DLSS_ACTORS") == 0;
    if (off || skins.empty() || !make_actor_pipeline()) return 0;
    for (auto& b : s.pose_buffers)
        if (!b.buffer) {
            if (!create_device_buffer(kPoseBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      b.buffer, b.memory))
                return b = {}, 0;
            b.address = device_address({b.buffer, 0, kPoseBytes, 0});
        }
    const uint64_t now = texture_frame_no();
    const State::PoseBuffer& here = s.pose_buffers[s.pose_at];
    const State::PoseBuffer& there = s.pose_buffers[s.pose_at ^ 1];
    static const uint32_t tolerance = env_switch("RX_VK_DLSS_ACTOR_TOL") >= 0 ? (uint32_t)env_switch("RX_VK_DLSS_ACTOR_TOL") : 64;
    std::vector<ActorPush> draws;
    std::vector<uint32_t> counts;
    uint32_t at = 0;
    for (const auto& k : skins) {
        if (k.k.depth != cam.depth) continue;
        const uint32_t bytes = 16 + k.nbones * 48;
        if (at + bytes > kPoseBytes) break;
        // (this frame's pose, from where the game has it: its bones, then its origin at the object's 0x40)
        const BufferRef bones = gpu_buffer_bind(k.k.bones, (uint64_t)k.nbones * 48, false);
        const BufferRef origin = gpu_buffer_bind(k.k.object + 0x40, 12, false);
        if (!bones || !origin) {
            s.actor_skipped++;
            continue;
        }
        const VkBufferCopy c0{origin.offset, at, 12}, c1{bones.offset, at + 16, (uint64_t)k.nbones * 48};
        vkCmdCopyBuffer(cmd, origin.buffer, here.buffer, 1, &c0);
        vkCmdCopyBuffer(cmd, bones.buffer, here.buffer, 1, &c1);
        s.poses[k.key] = {at, k.nbones, now};
        const uint32_t mine = at;
        at += (bytes + 15) & ~15u;
        const auto it = old.find(k.key);
        if (!history || it == old.end() || it->second.frame != s.last_frame || it->second.nbones != k.nbones) {
            s.actor_skipped++;
            continue;
        }
        // (the vertices and indices where the game has them: its buffers as this recording binds them)
        const uint64_t va = device_address(gpu_buffer_bind(k.k.vbuf, k.k.vsize, false));
        const uint64_t ia = device_address(gpu_buffer_bind(k.k.ibuf, (uint64_t)k.k.count << k.k.itype, false));
        if (!va || !ia || va % 4 || k.k.count <= 0 || k.k.itype < 0 || k.k.itype > 2) {
            s.actor_skipped++;
            continue;
        }
        ActorPush a{};
        a.vertices = va, a.count = (uint32_t)std::min<uint64_t>(k.k.vsize / k.k.stride, UINT32_MAX);
        dlss_actor::index_start(ia, k.k.itype, &a.indices, &a.first);
        a.origins[0] = here.address + mine, a.origins[1] = there.address + it->second.at;
        a.bones[0] = a.origins[0] + 16, a.bones[1] = a.origins[1] + 16;
        a.nbones = k.nbones;
        a.itype = (uint32_t)k.k.itype, a.base_vertex = k.k.base_vertex;
        a.stride = k.k.stride / 4, a.pos = k.k.pos, a.sets = (uint32_t)k.k.sets;
        for (int i = 0; i < 2; i++) a.joints[i] = k.k.joints[i], a.weights[i] = k.k.weights[i];
        a.formats = k.k.formats;
        a.tolerance = tolerance;
        draws.push_back(a);
        counts.push_back((uint32_t)k.k.count);
    }
    s.pose_at ^= 1;
    if (draws.empty()) return 0;
    // (the cameras: this frame's as drawn, the frame before's without its jitter, the jitter, the render size)
    State::Upload& u = s.uploads[s.upload_at];
    s.upload_at = (s.upload_at + 1) % (int)(sizeof s.uploads / sizeof s.uploads[0]);
    if (!u.buffer && !create_buffer_cached(dlss_actor::kHeadWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, u.buffer, u.memory, &u.map))
        return u = {}, 0;
    float* head = (float*)u.map;
    std::memcpy(head, cam.current, 64);
    std::memcpy(head + 16, before, 64);
    head[32] = cam.jitter[0], head[33] = cam.jitter[1], head[34] = (float)rw, head[35] = (float)rh;
    flush_mapped(u.memory, dlss_actor::kHeadWords * 4);
    full_barrier(cmd);   // (the poses copied)
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = s.motion.view, ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rw, rh}}, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.actor);
    const VkViewport viewport{0, 0, (float)rw, (float)rh, 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {rw, rh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkDescriptorBufferInfo bi{u.buffer, 0, dlss_actor::kHeadWords * 4};
    const VkDescriptorImageInfo ii{s.point, dview, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[0].pBufferInfo = &bi;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &ii;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.actor_layout, 0, 2, w);
    for (size_t i = 0; i < draws.size(); i++) {   // (vertex i of a draw is its i-th index)
        vkCmdPushConstants(cmd, s.actor_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof draws[i], &draws[i]);
        vkCmdDraw(cmd, counts[i], 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    s.actor_draws += draws.size(), s.actor_frames++;
    return (int)draws.size();
}

/// The bones a skinned draw's vertices use (the largest joint with a weight, plus one), read once a model; 0 when its
/// buffers cannot be read
uint32_t bones_used(const SkinnedDraw& k) {
    if (k.count <= 0 || k.count > (1 << 20) || k.itype < 0 || k.itype > 2 || !k.stride) return 0;
    std::vector<uint8_t> idx((size_t)k.count << k.itype);
    if (!memory_read_as_submitted(k.ibuf, idx.data(), idx.size())) return 0;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < k.count; i++) {
        uint32_t v = 0;
        std::memcpy(&v, idx.data() + ((size_t)i << k.itype), (size_t)1 << k.itype);
        lo = std::min(lo, v), hi = std::max(hi, v);
    }
    const int64_t first = (int64_t)k.base_vertex + lo, n = (int64_t)hi - lo + 1;
    if (first < 0 || n > (1 << 20) || (uint64_t)(first + n) * k.stride > k.vsize) return 0;
    std::vector<uint8_t> v((size_t)n * k.stride);
    if (!memory_read_as_submitted(k.vbuf + (uint64_t)first * k.stride, v.data(), v.size())) return 0;
    uint32_t most = 0;
    for (int64_t i = 0; i < n; i++) {
        const uint8_t* q = v.data() + (size_t)i * k.stride;
        for (int set = 0; set < k.sets; set++)
            for (int j = 0; j < 4; j++) {
                if (!q[k.weights[set] + j]) continue;
                uint32_t joint = 0;
                if (k.formats & (dlss_actor::kByteJoints << set)) joint = q[k.joints[set] + j];
                else std::memcpy(&joint, q + k.joints[set] + 2 * j, 2);
                most = std::max(most, joint + 1);
            }
    }
    return most;
}

/// The chain's images and buffers let go (the engine's own: its release)
void drop_all() {
    State& s = st();
    for (Image* im : {&s.color, &s.depth, &s.motion, &s.output}) drop_image(*im);
    for (auto& pb : s.pose_buffers)   // (the actor motion's poses and cameras)
        if (pb.buffer) {
            const State::PoseBuffer gone = pb;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            pb = {};
        }
    for (auto& u : s.uploads)
        if (u.buffer) {
            const State::Upload gone = u;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            u = {};
        }
    s.poses.clear(), s.skins.clear(), s.model_bones.clear();
    if (!s.depth_views.empty()) {
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
    }
}

}  // namespace

void temporal_later(std::function<void()> f) { later(std::move(f)); }

void temporal_note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                              uint64_t camera_size) {
    // (only a size the gameplay upscale's output can come from: 1 to 3 times smaller. A smaller target with a camera (the
    // pause screen's model views, a reflection) is never the scene; one this size that is not either is left by the checks
    // below: the format, the camera's own size and perspective, the largest in the frame, and the upscale's input)
    if (!color0 || !depth || !camera || camera_size < 0x230 || !temporal_scene_size(width, height)) return;
    if (color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !temporal_wanted()) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (s.cam.valid && s.cam.frame == now && (uint64_t)width * height <= (uint64_t)s.cam.width * s.cam.height)
        return;   // (the largest one of the frame wins)
    float c[0x230 / 4];
    if (!memory_read_as_submitted(camera, c, sizeof c)) return;
    // (the target's size at 0x120; the projection at 0x70, its w row (0, 0, -1, 0) at 0xa0)
    if (c[0x120 / 4] != (float)width || c[0x124 / 4] != (float)height || c[0xa0 / 4] != 0.0f || c[0xa4 / 4] != 0.0f ||
        c[0xa8 / 4] != -1.0f || c[0xac / 4] != 0.0f)
        return;
    Camera& k = s.cam;
    std::memcpy(k.current, c + 0x30 / 4, sizeof k.current);
    // (the offset sead's projection gave the frustum, in its third column: 2 * offset, which moves the picture by -that)
    k.jitter[0] = -c[0x78 / 4], k.jitter[1] = -c[0x88 / 4];
    // (its planes and field of view, for an engine that wants them (FSR's depth heuristics): an OpenGL-style projection, depth
    // -1 to 1, P[2][2] = -(f + n) / (f - n) and P[2][3] = -2fn / (f - n); 0 when they do not read as one)
    const float p11 = c[0x84 / 4], p22 = c[0x98 / 4], p23 = c[0x9c / 4];
    const float n = p23 / (p22 - 1.0f), f = p23 / (p22 + 1.0f);
    const bool plain = std::isfinite(n) && n > 0.0f && p11 > 0.0f && (!std::isfinite(f) || f > n);
    k.near_z = plain ? n : 0.0f, k.far_z = plain ? (std::isfinite(f) ? f : 1e9f) : 0.0f;
    k.fov_y = plain ? 2.0f * std::atan(1.0f / p11) : 0.0f;
    k.proj[0] = c[0x70 / 4], k.proj[1] = p11, k.proj[2] = p22, k.proj[3] = p23;
    k.width = width, k.height = height, k.depth = depth, k.depth_gen = texture_destroy_generation(), k.frame = now;
    k.valid = true;
}

bool temporal_scene_size(uint32_t width, uint32_t height) {
    const State& s = st();
    return dlss_checks::scene_size_fits(width, height, s.out_w.load(std::memory_order_relaxed),
                                        s.out_h.load(std::memory_order_relaxed));
}

MkwVkJitter temporal_jitter_sample() {
    static const bool off = env_switch("RX_VK_DLSS_JITTER") == 0;
    State& s = st();
    const int mode = temporal_mode();
    if (off || mode == ur::GAME) return {};
    const uint64_t last = s.last_frame.load(std::memory_order_relaxed);
    if (s.engines[ur::is_fsr4(mode) ? 1 : 0].failed.load(std::memory_order_relaxed) || !last) return {};
    return s.jitter.read(texture_frame_no());
}

uint32_t temporal_jitter_size(int axis) {
    const MkwVkJitter sample = temporal_jitter_sample();
    return axis == 2 ? sample.phases : axis ? sample.height : sample.width;
}

int temporal_status(char* b, size_t n) {
    State& s = st();
    const uint64_t last = s.last_frame.load(std::memory_order_relaxed);
    const int mode = temporal_mode();
    if (mode == ur::GAME || !last || texture_frame_no() - last > 30) return 0;
    std::lock_guard lk(s.mu);
    const Engine& k = engine_of(mode);
    if (k.failed || !k.e) return 0;
    // (the chain's own video memory: the images and buffers it made here, and what the engine says it holds, when it says)
    uint64_t own = 0;
    for (const Image* im : {&s.color, &s.depth, &s.motion, &s.output})
        if (im->image) own += allocation_size(im->memory);
    for (const auto& pb : s.pose_buffers)
        if (pb.buffer) own += allocation_size(pb.memory);
    for (const auto& u : s.uploads)
        if (u.buffer) own += allocation_size(u.memory);
    const uint64_t engine = k.e->memory();
    const uint64_t draws = s.actor_draws - s.status_draws, frames = s.actor_frames - s.status_frames;
    char feature[48] = "";
    if (engine) snprintf(feature, sizeof feature, ", its own %llu MB", (unsigned long long)(engine >> 20));
    char passed[64] = "";
    if (s.status_passed) snprintf(passed, sizeof passed, ", %llu left to the game's network", (unsigned long long)s.status_passed);
    snprintf(b, n, "; %s: %llu frames drawn%s, %llu made, jitter %+.3f %+.3f px%s, actor motion %llu draws in %llu "
             "frames (%.3f ms CPU a frame), its images %llu MB%s",
             name_of(mode), (unsigned long long)s.status_drawn, passed, (unsigned long long)s.engines_made,
             s.status_jitter[0], s.status_jitter[1], s.status_motion ? "" : " (no motion)",
             (unsigned long long)draws, (unsigned long long)frames, frames ? s.actor_ns / 1e6 / (double)frames : 0.0,
             (unsigned long long)(own >> 20), feature);
    s.status_draws = s.actor_draws, s.status_frames = s.actor_frames, s.actor_ns = 0, s.status_drawn = s.status_passed = 0;
    return mode;
}

void temporal_idle() {
    State& s = st();
    if (!s.holding.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (now - s.asked <= 120) return;   // (in use, or off for under 120 frames (2 s): kept, a row stepped through comes back at once)
    // (hookshot, a stutter: kept too while DLSS or FSR 4 is still the Upscaler's choice. Each area change's loading screen has no
    // gameplay upscale for 2 s and more, and DLSS's feature let go then was made again at the next area's first frame: 44-78 ms
    // of the replay thread at every area entry (12 times in a 15-minute walk, 2026-11-10, #1520). Let go once the row leaves them.
    // RX_VK_DLSS_IDLE_RELEASE=1: let go after 120 frames unused, as before)
    static const bool release_idle = getenv("RX_VK_DLSS_IDLE_RELEASE") && atoi(getenv("RX_VK_DLSS_IDLE_RELEASE")) != 0;
    if (!release_idle && temporal_wanted()) return;
    if (s.drawing) {
        logf("vk: %s: not used for 120 frames (the last at frame %llu, now %llu): what it made and its images let go; "
             "the next upscale with it makes them again", s.drawing->name, (unsigned long long)s.asked, (unsigned long long)now);
        s.drawing->release();
        s.drawing = nullptr;
    }
    drop_all();
    s.last_frame = 0;   // (a new history when it comes back)
    free_dead(false);
    s.holding = !s.dead.empty();
}

void temporal_note_skinned_draw(const SkinnedDraw& k) {
    static const bool off = env_switch("RX_VK_DLSS_ACTORS") == 0;
    if (off || !temporal_wanted() || !k.depth || !k.bones || k.bones_size < 48 || !k.object || k.object_size < 0x4c) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (s.skins.size() >= 512) s.skins.clear();   // (no upscale for a while: what was kept is stale)
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    State::Skin d;
    d.k = k;
    const uint64_t model = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, 0);
    // (only the bones its vertices use are taken: a palette holds up to 280, a model uses a few dozen)
    auto mb = s.model_bones.find(model);
    if (mb == s.model_bones.end()) {
        if (s.model_bones.size() > 4096) s.model_bones.clear();
        mb = s.model_bones.emplace(model, bones_used(k)).first;
    }
    d.nbones = (uint32_t)std::min<uint64_t>(mb->second, std::min<uint64_t>(k.bones_size / 48, 280));
    if (!d.nbones) return;
    int nth = 0;   // (the same model drawn twice in a frame: the n-th draw of it is the n-th the frame before)
    for (const auto& o : s.skins) nth += dlss_actor::same_model(o.key, model);
    d.key = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, nth);
    s.skins.push_back(std::move(d));
}

bool temporal_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                      VkImageView out_view, uint32_t out_level) {
    const int mode = temporal_mode();
    if (mode == ur::GAME) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (!known) {   // (another network of the upscale's shape: only the scene into the picture, the rest untouched and unsaid)
        const Camera& c = s.cam;
        // (the gameplay networks' own formats: they sample the scene's B10G11R11 colour and write the B10G11R11 picture.
        // Other cooperative-matrix networks of the same shape (one RGBA8 into A2B10G10R10, RGB9E5 into B10G11R11) are not it)
        if (!in || !out || in->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
            (out->width >> out_level) <= in->width)
            return false;
        note_output(out, out_level);
        // (one in the gameplay networks' formats whose input is not this frame's scene: counted for the status line)
        if (!c.valid || texture_frame_no() - c.frame > 1 || in->width != c.width || in->height != c.height)
            return s.status_passed++, false;
        static std::atomic<uint64_t> told{0};
        if (told.exchange(key) != key) logf("vk: upscaler: network %016llx is the gameplay upscale (%ux%u -> %ux%u)",
                                            (unsigned long long)key, in->width, in->height, out->width >> out_level,
                                            out->height >> out_level);
    }
    free_dead(false);
    if (known && out && out->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) note_output(out, out_level);
    const Camera cam = s.cam;
    s.cam.valid = false;   // (each frame's own camera)
    struct SkinsDone {     // (the skinned draws, each frame's own too: actor_motion takes them, or they go here)
        State& s;
        ~SkinsDone() { s.skins.clear(); }
    } skins_done{s};

    Engine& engine = engine_of(mode);
    if (!engine.e || engine.failed) return fail("it failed earlier");
    s.asked = texture_frame_no(), s.holding = true;   // (not after a failure: what it holds is let go, temporal_idle)
    if (!in || !in_view || !out || !out_view) return fail("no input or output view");
    if (!cam.valid || texture_frame_no() - cam.frame > 1) return fail("no scene camera this frame");
    if (cam.depth_gen != texture_destroy_generation()) return fail("the scene depth may be gone");
    const uint32_t rw = in->width, rh = in->height;
    const uint32_t ow = std::max(1u, out->width >> out_level), oh = std::max(1u, out->height >> out_level);
    if (rw != cam.width || rh != cam.height) return fail("the input is not the scene's size");
    if (cam.depth->width != rw || cam.depth->height != rh || !(cam.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        cam.depth->samples != VK_SAMPLE_COUNT_1_BIT || !(cam.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return fail("unexpected scene depth");
    if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !dlss_checks::scene_size_fits(rw, rh, ow, oh))
        return fail("unexpected output");
    if (!engine.started) engine.started = true, engine.ok = engine.e->start();
    if (!engine.ok) {
        engine.failed = true;
        return fail("not available");
    }
    if (!make_pipelines()) {
        engine.failed = true;
        return fail("its shaders were not made");
    }
    const VkImageView dview = depth_view(cam.depth);
    if (!dview) return fail("no view of the scene depth");
    if (s.drawing != engine.e) {   // (another engine than the one before: that one's state let go, and a new history)
        if (s.drawing) {
            logf("vk: upscaler: %s in place of %s (frame %llu)", engine.e->name, s.drawing->name,
                 (unsigned long long)texture_frame_no());
            s.drawing->release();
        }
        s.drawing = engine.e;
        s.last_frame = 0;
    }
    // (DLAA, FSR's native AA: the engine at the render size, anti-aliasing alone; its picture then scaled to the output's by
    // dlss_out.comp. With them the 3D is drawn at full scale, temporal_full_scale)
    const bool native = ur::full_scale(mode);
    const uint32_t dw = native ? rw : ow, dh = native ? rh : oh;
    // (from here commands are recorded, the engine's own too: whichever way this returns, the program's descriptor pushes are
    // gone from the bind point and are pushed again)
    struct Disturbed {
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    uint32_t iw = rw, ih = rh;
    const char* why = nullptr;
    const int ready = engine.e->prepare(cmd, rw, rh, dw, dh, &iw, &ih, &why);
    if (ready < 0) {
        engine.failed = true;
        return fail(why ? why : "it could not be made");
    }
    if (ready == 0) return fail(why ? why : "not this frame");
    if (ready == 2) s.engines_made++, s.last_frame = 0;   // (made just now: a new history)
    if (!make_image(cmd, s.color, VK_FORMAT_R16G16B16A16_SFLOAT, iw, ih) ||
        !make_image(cmd, s.depth, VK_FORMAT_R32_SFLOAT, iw, ih) ||
        !make_image(cmd, s.motion, VK_FORMAT_R16G16_SFLOAT, iw, ih, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !make_image(cmd, s.output, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh))
        return fail("its images were not made");

    const uint64_t now = texture_frame_no();
    const uint64_t last = s.last_frame;
    bool reset = !last || now - last > 2;
    struct {
        float reproject[16];
        float jitter[2];
        int32_t size[2];
        int32_t flags;
        float ao;         // (miojo: the menu's Ambient occlusion row, 0 to 1)
        float proj[4];
    } pc{};
    static const bool show_motion = getenv("RX_VK_DLSS_DEBUG") && !strcmp(getenv("RX_VK_DLSS_DEBUG"), "motion");
    // (the motion without the jitter: from this frame's jittered clip space to the frame before's without its jitter.
    // A jitter moves clip x by jitter * w: taken off the previous matrix's first two rows)
    // (the frame before's camera as the engine last drew it: the buffer's own slot for it, 0x180, holds zeros in this version)
    float previous[16];
    std::memcpy(previous, s.last_vp, sizeof previous);
    dlss_math::unjitter(previous, s.last_jitter);
    const bool motion = !reset && dlss_math::reprojection(cam.current, previous, pc.reproject);
    if (!motion) reset = true;   // (without a valid reprojection, prior history has no mapping to this camera)
    pc.jitter[0] = cam.jitter[0], pc.jitter[1] = cam.jitter[1];
    pc.size[0] = (int32_t)rw, pc.size[1] = (int32_t)rh, pc.flags = (motion ? 1 : 0) | (show_motion ? 2 : 0);
    static const bool show_ao = getenv("RX_VK_AO_DEBUG") != nullptr;   // (miojo: the occlusion alone)
    if (show_ao) pc.flags |= 8;
    pc.ao = cam.near_z > 0.0f && cam.proj[0] > 0.0f && !rt_on() ? (float)opts.ao / 100.0f : 0.0f;   // (traced with Ray tracing on: rt_frame)
    std::memcpy(pc.proj, cam.proj, sizeof pc.proj);

    const auto prep = [&] {
        const VkDescriptorImageInfo info[5] = {{s.point, in_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {s.point, dview, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.motion.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep_layout, 0, 5, w);
        vkCmdPushConstants(cmd, s.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    };
    full_barrier(cmd);   // (the scene's colour and depth, written before)
    prep();
    full_barrier(cmd);
    rt_frame(cmd, cam.current, s.color.view, dview, s.point, rw, rh, (float)opts.ao / 100.0f,
             cam.jitter[0] * (float)rw * 47.0f + cam.jitter[1] * (float)rh * 91.0f, in_view, (float)opts.gi / 100.0f, s.depth.view);   // (miojo: the scene's ray-tracing structures, RX_VK_RT)
    // (what moves by itself: the skinned draws drawn again over the camera's motion, with their own)
    const int actors = actor_motion(cmd, cam, previous, rw, rh, dview, motion);
    if (actors) full_barrier(cmd);
    if (show_motion && actors) {   // (RX_VK_DLSS_DEBUG=motion: the motion as it is now, the actors' in it)
        pc.flags = 4;
        prep();
        full_barrier(cmd);
    }
    TemporalFrame f;
    f.color = s.color.shown(), f.depth = s.depth.shown(), f.motion = s.motion.shown(), f.output = s.output.shown();
    f.render_w = rw, f.render_h = rh;
    // (the jitter in render pixels, x right and y down)
    dlss_math::jitter_to_pixels(cam.jitter, rw, rh, f.jitter);
    f.reset = reset;
    if (cam.near_z > 0.0f) f.near_z = cam.near_z, f.far_z = cam.far_z, f.fov_y = cam.fov_y;
    if (!s.told_camera && cam.near_z > 0.0f) {
        s.told_camera = true;
        logf("vk: upscaler: the scene's camera: near %.3f, far %.1f, vertical field of view %.1f degrees", cam.near_z,
             cam.far_z, cam.fov_y * 57.29578f);
    }
    if (!engine.e->evaluate(cmd, f)) {
        logf("vk: %s: its evaluation failed: the game's upscale from now on", engine.e->name);
        engine.failed = true;
        return false;   // (the network writes its output over whatever the evaluation did)
    }
    full_barrier(cmd);
    {
        const int32_t opc[2] = {(int32_t)ow, (int32_t)oh};
        const VkDescriptorImageInfo info[2] = {{native ? s.linear : s.point, s.output.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &info[0];
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &info[1];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
        vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);
        vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
    }
    s.last_frame = now;
    s.last_jitter[0] = cam.jitter[0], s.last_jitter[1] = cam.jitter[1];
    // (for the status line, temporal_status: the jitter the engine was given, in render pixels)
    s.status_jitter[0] = f.jitter[0], s.status_jitter[1] = f.jitter[1], s.status_motion = motion;
    s.status_drawn++;

    std::memcpy(s.last_vp, cam.current, sizeof s.last_vp);
    s.jitter.publish(rw, rh, (uint32_t)dlss_jitter_phases(rw, rh, dw, dh), now);
    if (s.reason) logf("vk: %s draws the upscale again (frame %llu)", engine.e->name, (unsigned long long)now);
    s.reason = nullptr;
    // (RX_VK_DLSS_DEBUG=alternate, a test's reference: every other frame the game's own upscale over the engine's, which still
    // draws and keeps its history. A frame of it then has the game's picture of the frames either side to be measured against)
    static const bool alternate = getenv("RX_VK_DLSS_DEBUG") && !strcmp(getenv("RX_VK_DLSS_DEBUG"), "alternate");
    if (alternate && (now & 1)) return false;
    if (s.told_first != engine.e) {
        s.told_first = engine.e;
        logf("vk: %s draws OoT's upscale: %ux%u -> %ux%u%s", name_of(mode), rw, rh, ow, oh,
             show_motion ? " (its motion shown)" : "");
    }
    return true;
}

} // namespace mkwvk
