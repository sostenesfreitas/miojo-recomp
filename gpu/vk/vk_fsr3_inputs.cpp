// (hookshot) AMD FSR 3.1's inputs: the scene's camera and depth, made into colour, depth and motion at the render size by
// one pass (shaders/fsr3_prep.comp); see vk_fsr3_inputs.h for what comes from #841 and what does not yet.
//
// The camera is taken exactly as vk_temporal.cpp's temporal_note_scene_draw takes it, from the draws into the scene's
// colour target: their vertex stage's uniform buffer 3 holds the target's size at 0x120, this frame's view-projection at 0x30 (row major,
// clip = M * position) and the projection at 0x70, its w row (0, 0, -1, 0) at 0xa0. The jitter the host put on the
// projection (nx2/oot_jitter.py: P[0][2] and P[1][2]) is read back from it, given to FSR in render pixels and taken off the
// motion, with dlss_math.h's functions: the same jitter, read the same way, as DLSS's.
#include "vk_fsr3_inputs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "dlss_math.h"
#include "temporal_jitter.h"
#include "vk_draw.h"   // (descriptors_disturbed)
#include "vk_fsr3.h"
#include "vk_fsr3fi.h"
#include "vk_texture.h"

#include "shaders/fsr3_prep_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk::fsr3_inputs {
namespace {

int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

struct Camera {
    float current[16]{};      // (its view-projection, jittered)
    float projection[16]{};   // (its projection, jittered: the near plane and the field of view come from it)
    float jitter[2]{};        // (the projection's offset of the picture's centre, in clip space: nx2/oot_jitter.py)
    uint32_t width = 0, height = 0;
    Texture* depth = nullptr;
    uint64_t depth_gen = 0;   // (texture_destroy_generation when taken: a depth texture destroyed since is not used)
    uint64_t depth_at = 0;    // (the depth texture's address and creation number, for texture_alive)
    uint32_t depth_id = 0;
    uint64_t frame = 0;       // (texture_frame_no when taken)
    bool valid = false;
};

struct State {
    std::mutex mu;
    Camera cam;               // (this frame's, since the last prepare)
    Image color, depth, motion;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE;
    bool tried = false;
    std::unordered_map<VkImage, VkImageView> depth_views;
    uint64_t depth_views_gen = 0;
    float last_vp[16]{};      // (the frame before's view-projection, and its jitter)
    float last_jitter[2]{};
    uint64_t last_frame = 0;  // (the frame prepare last ran, 0: none)
    std::chrono::steady_clock::time_point last_time{};
    Frame frame;              // (the last one prepared: take())
    bool have_frame = false;
    TemporalJitter jitter;
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;   // (what work in flight may still use: freed a few frames on)
    const char* reason = nullptr;
    int reason_repeats = 0;
    bool told = false;
};
State& st() {
    static State s;
    return s;
}

bool fail(const char* why) {   // (why, when it changes and every 600th time)
    State& s = st();
    if (why != s.reason || ++s.reason_repeats % 600 == 0) logf("vk: fsr3 inputs: not made this frame (%s)", why);
    if (why != s.reason) s.reason = why, s.reason_repeats = 0;
    return false;
}

void later(std::function<void()> f) { st().dead.push_back({texture_frame_no(), std::move(f)}); }
void free_dead() {
    State& s = st();
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < s.dead.size();) {
        if (now - s.dead[i].frame > 4) {
            s.dead[i].free();
            s.dead[i] = std::move(s.dead.back());
            s.dead.pop_back();
        } else {
            i++;
        }
    }
}

bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h) {
    if (im.image && im.format == format && im.width == w && im.height == h) return true;
    if (im.image) {
        const Image old = im;
        later([old] {
            vkDestroyImageView(dev.device, old.view, nullptr);
            destroy_image(old.image, old.memory);
        });
        im = {};
    }
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

/// fsr3_prep.comp's pipeline, once: two sampled inputs, three storage outputs, dlss_prep.comp's push constants
bool make_pipeline() {
    State& s = st();
    if (s.pipe) return true;
    if (s.tried || !dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    s.tried = true;
    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 5; i++)
        b[i] = {i, i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 5, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &s.set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 84};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &s.set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &s.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof FSR3_PREP_COMP_SPV, mi.pCode = FSR3_PREP_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r;
    {
        IntelDriverGate gate;
        if (extern_guarded_shader_module(&mi, &m) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                    nullptr};
        ci.layout = s.layout;
        r = extern_guarded_compute(VK_NULL_HANDLE, &ci, &s.pipe);
    }
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) {
        logf("vk: fsr3 inputs: their pipeline was not made (%d)", (int)r);
        return s.pipe = VK_NULL_HANDLE, false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev.device, &si, nullptr, &s.point) != VK_SUCCESS) {
        vkDestroyPipeline(dev.device, s.pipe, nullptr);
        return s.pipe = VK_NULL_HANDLE, false;
    }
    return true;
}

/// A view of a depth texture's depth alone, for sampling (made once per image: vk_temporal.cpp's depth_view)
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

} // namespace

bool wanted() { return fsr3_wanted() || fsr3fi::frames_wanted() != 0; }

void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                     uint64_t camera_size) {
    if (!color0 || !depth || !camera || camera_size < 0x230 || width < 640 || height < 360) return;
    if (color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !wanted()) return;
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
    std::memcpy(k.projection, c + 0x70 / 4, sizeof k.projection);
    // (the offset sead's projection gave the frustum, in its third column: 2 * offset, which moves the picture by -that)
    k.jitter[0] = -c[0x78 / 4], k.jitter[1] = -c[0x88 / 4];
    k.width = width, k.height = height, k.depth = depth, k.depth_gen = texture_destroy_generation(), k.frame = now;
    k.depth_at = depth ? depth->gpu : 0, k.depth_id = depth ? depth->id : 0;
    k.valid = true;
}

bool is_scene(const Texture* in) {
    State& s = st();
    std::lock_guard lk(s.mu);
    return in && s.cam.valid && texture_frame_no() - s.cam.frame <= 1 && in->width == s.cam.width &&
           in->height == s.cam.height;
}

bool prepare(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Frame& out) {
    State& s = st();
    std::lock_guard lk(s.mu);
    free_dead();
    const Camera cam = s.cam;
    s.cam.valid = false;   // (each frame's own camera)
    if (!in || !in_view) return fail("no input view");
    const uint64_t now = texture_frame_no();
    if (!cam.valid || now - cam.frame > 1) return fail("no scene camera this frame");
    // (any texture destroyed since the camera was noted used to send the frame to the game's own upscale: the game lets
    // textures go every few frames while new ones stream in (a turn of the camera in Kakariko's potion shop), and each
    // was a frame without the temporal upscaler, and with Ray tracing a frame without its light shadows and bounce, then
    // the game's own occlusion: shadows that flickered. Now only when the depth texture itself is gone)
    if (cam.depth_gen != texture_destroy_generation() && !texture_alive(cam.depth, cam.depth_at, cam.depth_id))
        return fail("the scene depth is gone");
    const uint32_t rw = in->width, rh = in->height;
    if (rw != cam.width || rh != cam.height) return fail("the input is not the scene's size");
    if (cam.depth->width != rw || cam.depth->height != rh || !(cam.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        cam.depth->samples != VK_SAMPLE_COUNT_1_BIT || !(cam.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return fail("unexpected scene depth");
    // (the projection without this frame's jitter: the field of view and the depth range do not move with it)
    float projection[16];
    std::memcpy(projection, cam.projection, sizeof projection);
    dlss_math::unjitter(projection, cam.jitter);
    fsr3_math::Camera camera;
    if (!fsr3_math::camera_from_projection(projection, camera)) return fail("the scene's projection is not one FSR takes");
    if (!make_pipeline()) return fail("its pipeline was not made");
    const VkImageView dview = depth_view(cam.depth);
    if (!dview) return fail("no view of the scene depth");
    struct Disturbed {   // (from here commands are recorded: the program's descriptor pushes are pushed again after)
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    if (!make_image(cmd, s.color, VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh) || !make_image(cmd, s.depth, VK_FORMAT_R32_SFLOAT, rw, rh) ||
        !make_image(cmd, s.motion, VK_FORMAT_R16G16_SFLOAT, rw, rh))
        return fail("its images were not made");

    bool reset = !s.last_frame || now - s.last_frame > 2 || s.frame.width != rw || s.frame.height != rh;
    struct {
        float reproject[16];
        float jitter[2];
        int32_t size[2];
        int32_t flags;
    } pc{};
    static_assert(sizeof pc == 84, "fsr3_prep.comp's push constants");
    // (the motion without the jitter: from this frame's jittered clip space to the frame before's without its jitter. A jitter
    // moves clip x by jitter * w: taken off the previous matrix's first two rows)
    float previous[16];
    std::memcpy(previous, s.last_vp, sizeof previous);
    dlss_math::unjitter(previous, s.last_jitter);
    const bool motion = !reset && dlss_math::reprojection(cam.current, previous, pc.reproject);
    if (!motion) reset = true;   // (without a valid reprojection, prior history has no mapping to this camera)
    pc.jitter[0] = cam.jitter[0], pc.jitter[1] = cam.jitter[1];
    pc.size[0] = (int32_t)rw, pc.size[1] = (int32_t)rh;
    pc.flags = (motion ? 1 : 0) | (camera.flip_depth ? 2 : 0);

    full_barrier(cmd);   // (the scene's colour and depth, written before)
    {
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
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipe);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 5, w);
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    }
    full_barrier(cmd);

    const auto t = std::chrono::steady_clock::now();
    Frame f;
    f.color = s.color.view, f.depth = s.depth.view, f.motion = s.motion.view;
    f.width = rw, f.height = rh;
    dlss_math::jitter_to_pixels(cam.jitter, rw, rh, f.jitter);   // (in render pixels, x right and y down: FSR's, as DLSS's)
    f.camera = camera;
    f.reset = reset;
    f.frame_ms = s.last_time.time_since_epoch().count() && !reset
                     ? (float)std::min(250.0, std::chrono::duration<double, std::milli>(t - s.last_time).count())
                     : 16.667f;
    f.frame = now;
    s.frame = f, s.have_frame = true;
    s.last_time = t;
    s.last_frame = now;
    std::memcpy(s.last_vp, cam.current, sizeof s.last_vp);
    s.last_jitter[0] = cam.jitter[0], s.last_jitter[1] = cam.jitter[1];
    if (s.reason) logf("vk: fsr3 inputs: made again (frame %llu)", (unsigned long long)now);
    s.reason = nullptr;
    if (!s.told) {
        s.told = true;
        // (what the projection says, once: the planes and the field of view FSR's depth factors are made of)
        logf("vk: fsr3 inputs: %ux%u, from the scene's projection: near %.4f far %.6g fov %.2f deg, depth %s (p22 %.6g p23 "
             "%.6g); camera motion from the depth, jitter %+.3f %+.3f px",
             rw, rh, camera.near_plane, camera.far_plane, camera.fov_y * 57.2957795, camera.flip_depth ? "0 at the near plane: "
             "reversed for FSR" : "reversed already", projection[10], projection[11], f.jitter[0], f.jitter[1]);
    }
    out = f;
    return true;
}

bool take(Frame& out) {
    State& s = st();
    std::lock_guard lk(s.mu);
    const bool have = s.have_frame && texture_frame_no() - s.frame.frame <= 1;
    s.have_frame = false;
    if (have) out = s.frame;
    return have;
}

void upscaled(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
    State& s = st();
    s.jitter.publish(rw, rh, (uint32_t)dlss_jitter_phases(rw, rh, dw, dh), texture_frame_no());
}

MkwVkJitter jitter_sample() {
    // (RX_VK_FSR_JITTER=0: none. 0 too once FSR has not drawn for a while: the camera as the game has it)
    static const bool off = env_switch("RX_VK_FSR_JITTER") == 0;
    State& s = st();
    if (off || !fsr3_wanted()) return {};
    return s.jitter.read(texture_frame_no());
}

uint32_t jitter_size(int axis) {
    const MkwVkJitter sample = jitter_sample();
    return axis == 2 ? sample.phases : axis ? sample.height : sample.width;
}

} // namespace mkwvk::fsr3_inputs
