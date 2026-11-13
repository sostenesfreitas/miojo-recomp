// (hookshot) AMD FSR 3.1's upscaler as our own dispatches over AMD's vendored passes, and its place in the gameplay upscale's
// dispatch (fsr3_upscale, at the end); see vk_fsr3.h.
//
// The resource table and the per-pass binding lists are the SDK's, not ours: ffx_fsr3upscaler.cpp decides the formats and
// sizes, and each pass file numbers its own bindings (FSR3UPSCALER_BIND_*). Getting one wrong is a validation error at best
// and a wrong picture at worst, so they are written out here in the SDK's order rather than derived.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "vk_fsr3.h"
#include "vk_fsr3_inputs.h"
#include "vk_draw.h"      // (descriptors_disturbed)
#include "vk_gprof.h"     // (RX_VK_PASS_TIMING: a span per pass, so the report names them)
#include "vk_menu.h"
#include "vk_reactive.h"
#include "vk_temporal.h"   // (temporal_env_chosen: RX_VK_DLSS / RX_VK_FSR4 over the row's FSR 3.1)
#include "vk_texture.h"
#include "upscaler_rows.h"

#include "shaders/dlss_out_comp_spv.h"   // (the network's output from a picture: its encoding squared back, as for DLSS)
#include "shaders/fsr3_accumulate_comp_spv.h"
#include "shaders/fsr3_luma_instability_comp_spv.h"
#include "shaders/fsr3_luma_pyramid_comp_spv.h"
#include "shaders/fsr3_luma_pyramid_nowave_comp_spv.h"
#include "shaders/fsr3_prepare_inputs_comp_spv.h"
#include "shaders/fsr3_prepare_reactivity_comp_spv.h"
#include "shaders/fsr3_shading_change_comp_spv.h"
#include "shaders/fsr3_shading_change_pyramid_comp_spv.h"
#include "shaders/fsr3_shading_change_pyramid_nowave_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
VkResult shared_compute_pipeline(const VkComputePipelineCreateInfo* ci, VkPipeline* out);   // (vk_draw.cpp: through the pipeline cache kept across runs)
}

namespace mkwvk::fsr3 {
namespace {

/// A descriptor's kind, in the binding order each pass file declares. Every pass also takes the two
/// samplers at bindings 1000 (point clamp) and 1001 (linear clamp).
enum Kind : uint8_t { T, I, U };   // sampled texture, storage image, uniform buffer

struct PassInfo {
    const char* name;
    const uint32_t* spv;
    size_t spv_bytes;
    const Kind* bindings;
    size_t binding_count;
};

constexpr Kind PREPARE_INPUTS[]{T, T, T, I, I, I, I, I, U};
constexpr Kind LUMA_PYRAMID[]{T, T, I, I, I, I, I, I, I, I, I, U, U};
constexpr Kind SHADING_CHANGE_PYRAMID[]{T, T, T, T, I, I, I, I, I, I, I, U, U};
constexpr Kind SHADING_CHANGE[]{T, I, U};
constexpr Kind PREPARE_REACTIVITY[]{T, T, T, T, T, T, T, T, T, I, I, I, U};
constexpr Kind LUMA_INSTABILITY[]{T, T, T, T, T, T, T, I, I, U};
constexpr Kind ACCUMULATE[]{T, T, T, T, T, T, T, T, T, I, I, I, U};

#define PASS(n, spv, b) PassInfo{n, spv, sizeof spv, b, sizeof b / sizeof b[0]}

/// The passes, in the order PassId numbers them. The two _nowave pyramids are alternatives to the two before them, for
/// devices without the subgroup operations the single-pass downsampler wants. FSR's own sharpening (accumulate_sharpen and
/// RCAS, compiled with the rest) is not used: the renderer's Sharpness row sharpens the finished picture for every upscaler
const PassInfo PASSES[]{
    PASS("prepare_inputs", FSR3_PREPARE_INPUTS_COMP_SPV, PREPARE_INPUTS),
    PASS("luma_pyramid", FSR3_LUMA_PYRAMID_COMP_SPV, LUMA_PYRAMID),
    PASS("luma_pyramid_nowave", FSR3_LUMA_PYRAMID_NOWAVE_COMP_SPV, LUMA_PYRAMID),
    PASS("shading_change_pyramid", FSR3_SHADING_CHANGE_PYRAMID_COMP_SPV, SHADING_CHANGE_PYRAMID),
    PASS("shading_change_pyramid_nowave", FSR3_SHADING_CHANGE_PYRAMID_NOWAVE_COMP_SPV,
         SHADING_CHANGE_PYRAMID),
    PASS("shading_change", FSR3_SHADING_CHANGE_COMP_SPV, SHADING_CHANGE),
    PASS("prepare_reactivity", FSR3_PREPARE_REACTIVITY_COMP_SPV, PREPARE_REACTIVITY),
    PASS("luma_instability", FSR3_LUMA_INSTABILITY_COMP_SPV, LUMA_INSTABILITY),
    PASS("accumulate", FSR3_ACCUMULATE_COMP_SPV, ACCUMULATE),
};
#undef PASS

enum PassId : size_t {
    P_PREPARE_INPUTS,
    P_LUMA_PYRAMID,
    P_LUMA_PYRAMID_NOWAVE,
    P_SHADING_CHANGE_PYRAMID,
    P_SHADING_CHANGE_PYRAMID_NOWAVE,
    P_SHADING_CHANGE,
    P_PREPARE_REACTIVITY,
    P_LUMA_INSTABILITY,
    P_ACCUMULATE,
    kPassCount
};
static_assert(sizeof PASSES / sizeof PASSES[0] == kPassCount, "PassId must name every pass");
constexpr uint32_t kSpdStoredMips = 6;

struct Pipeline {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;          ///< every level
    std::vector<VkImageView> levels;            ///< one per level, for the mipped ones
    uint32_t width = 0, height = 0, level_count = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

/// cbFSR3Upscaler, exactly as ffx_fsr3upscaler.cpp lays it out. The size is asserted because every
/// pass reads it by offset: a field out of place is a wrong picture, not a compile error.
struct Constants {
    int32_t render_size[2];
    int32_t previous_render_size[2];
    int32_t upscale_size[2];
    int32_t previous_upscale_size[2];
    int32_t max_render_size[2];
    int32_t max_upscale_size[2];
    float device_to_view_depth[4];
    float jitter[2];
    float previous_jitter[2];
    float motion_vector_scale[2];
    float downscale_factor[2];
    float motion_vector_jitter_cancellation[2];
    float tan_half_fov;
    float jitter_phase_count;
    float delta_time;
    float delta_pre_exposure;
    float view_space_to_meters;
    float frame_index;
    float velocity_factor;
    float reactiveness_scale;
    float shading_change_scale;
    float accumulation_added_per_frame;
    float min_disocclusion_accumulation;
};
static_assert(sizeof(Constants) == 148, "cbFSR3Upscaler must match the SDK's layout");

struct SpdConstants {
    uint32_t mips;
    uint32_t num_work_groups;
    uint32_t work_group_offset[2];
    uint32_t render_size[2];
};

/// One descriptor of a dispatch: an image view, or a range of the uniform buffer.
struct Resource {
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

/// A memory dependency over the whole queue, in the synchronization2 form this renderer loads.
void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage, b.srcAccessMask = src;
    b.dstStageMask = dst_stage, b.dstAccessMask = dst;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

constexpr VkImageSubresourceRange ALL_LEVELS{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};

struct State {
    bool tried = false;
    bool ok = false;
    bool wave = true;   ///< the pyramids with subgroup operations (else their _nowave variants)
    Pipeline passes[kPassCount];
    VkSampler point = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    // The SDK's internal images. The pairs ping-pong on frame parity, as its resource tables do.
    Image accumulation[2], luma[2], luma_history[2], internal_upscaled[2];
    Image farthest_depth, shading_change, new_locks, spd_mips, farthest_depth_mip1;
    Image dilated_motion_vectors, dilated_depth, previous_nearest_depth, dilated_reactive_masks;
    Image frame_info, spd_atomic, lanczos_lut, default_reactive, output;
    uint32_t render_width = 0, render_height = 0, upscale_width = 0, upscale_height = 0;
    // Constant block layout; storage belongs to each recording, not a fixed frame ring.
    VkDeviceSize slot_fsr = 0, slot_spd = 0, frame_stride = 0;
    // History, as fsr3upscalerDispatch carries it.
    int32_t previous_render_size[2]{};
    int32_t previous_upscale_size[2]{};
    float previous_jitter[2]{};
    float frame_index = 0.0f;
    uint32_t parity = 0;
    bool first_evaluation = true;   ///< the images are new: their first layout, cleared
    bool lut_uploaded = false;
    // Images of an earlier size, released only after their GPU uses complete.
    std::vector<std::pair<uint64_t, Image>> dead;
};

State g;

void destroy_now(Image& t) {
    for (VkImageView v : t.levels) vkDestroyImageView(dev.device, v, nullptr);
    if (t.view) vkDestroyImageView(dev.device, t.view, nullptr);
    if (t.image) mkwvk::destroy_image(t.image, t.memory);
    t = Image{};
}

/// Retain every view and its image until a later submission covers the old recordings.
void drop_image(Image& t) {
    if (t.image) g.dead.push_back({vk_retirement_value(), std::move(t)});
    t = Image{};
}

void free_dead() {
    const uint64_t now = vk_timeline_completed();
    for (size_t i = 0; i < g.dead.size();) {
        if (g.dead[i].first && now >= g.dead[i].first) {
            destroy_now(g.dead[i].second);
            g.dead[i] = std::move(g.dead.back());
            g.dead.pop_back();
        } else {
            i++;
        }
    }
}

/// One of the SDK's internal images: storage and sampled, GENERAL throughout, with a view per level when it has more than
/// one (the pyramids bind their levels separately).
bool make_image(Image& t, VkFormat format, uint32_t width, uint32_t height, uint32_t levels = 1) {
    drop_image(t);
    t.format = format, t.width = std::max(1u, width), t.height = std::max(1u, height);
    t.level_count = std::max(1u, levels);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {t.width, t.height, 1};
    ci.mipLevels = t.level_count;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // (hookshot: and a copy's source: the output's picture goes into vk_temporal.cpp's chain output, kChainEngine)
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!create_image(ci, t.image, t.memory, false)) {
        logf("vk: fsr3: a %ux%u image (format %d) was not made", t.width, t.height, (int)format);
        t.image = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.level_count, 0, 1};
    if (vkCreateImageView(dev.device, &vi, nullptr, &t.view) != VK_SUCCESS) return t.view = VK_NULL_HANDLE, false;
    if (t.level_count > 1) {
        for (uint32_t level = 0; level < t.level_count; level++) {
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
            VkImageView v = VK_NULL_HANDLE;
            if (vkCreateImageView(dev.device, &vi, nullptr, &v) != VK_SUCCESS) return false;
            t.levels.push_back(v);
        }
    }
    return true;
}

bool make_pipeline(Pipeline& p, const PassInfo& info) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(info.binding_count + 2);
    for (size_t i = 0; i < info.binding_count; i++) {
        const VkDescriptorType type = info.bindings[i] == T   ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : info.bindings[i] == I ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings.push_back({(uint32_t)i, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    // (the two samplers every pass declares, at the bindings the SDK's GLSL fixes them to)
    bindings.push_back({1000, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    bindings.push_back({1001, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = (uint32_t)bindings.size();
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &p.set) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &p.set;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = info.spv_bytes;
    mi.pCode = info.spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = p.layout;
    // (hookshot, a stutter: through the pipeline cache kept across runs. Made with none, FSR 3.1's dozen passes were compiled
    // again at every start, inside the first upscaled frame: a 2.7 s first frame of play against 1.0 s without FSR, 2026-11-10)
    const VkResult made = shared_compute_pipeline(&ci, &p.pipeline);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (made != VK_SUCCESS) {
        logf("vk: fsr3: the %s pass pipeline was not made (%d)", info.name, (int)made);
        p.pipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool make_samplers() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f, si.maxLod = VK_LOD_CLAMP_NONE;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(dev.device, &si, nullptr, &g.point) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    return vkCreateSampler(dev.device, &si, nullptr, &g.linear) == VK_SUCCESS;
}

/// Every storage format the internal images need. A device missing one cannot run the passes, and finding that out here is
/// better than a failed image allocation in the middle of a frame.
bool formats_supported() {
    static const VkFormat needed[]{
        VK_FORMAT_R8_UNORM,      VK_FORMAT_R16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R16_SNORM,
    };
    for (VkFormat f : needed) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, f, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
            !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            logf("vk: fsr3: not on this GPU: format %d cannot be a storage and sampled image here", (int)f);
            return false;
        }
    }
    return true;
}

/// Aligned constant block offsets; the recording supplies storage for each evaluation.
void make_uniform_layout() {
    const VkDeviceSize align = std::max<VkDeviceSize>(dev.props.limits.minUniformBufferOffsetAlignment, 16);
    const auto round_up = [align](VkDeviceSize n) { return (n + align - 1) / align * align; };
    g.slot_fsr = round_up(sizeof(Constants));
    g.slot_spd = round_up(sizeof(SpdConstants));
    g.frame_stride = g.slot_fsr + g.slot_spd + round_up(128 * sizeof(int16_t));
}

/// Records one pass: its descriptors pushed in the order the pass file numbers them, then the two samplers it declares at
/// 1000 and 1001, then the dispatch, and a barrier after it (every pass reads what the one before it wrote).
void record_pass(VkCommandBuffer cmd, size_t pass, const Resource* res, size_t count, uint32_t groups_x, uint32_t groups_y) {
    const PassInfo& info = PASSES[pass];
    const Pipeline& p = g.passes[pass];
    std::vector<VkDescriptorImageInfo> images(count);
    std::vector<VkDescriptorBufferInfo> buffers(count);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(count + 2);
    for (size_t i = 0; i < count; i++) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = (uint32_t)i;
        w.descriptorCount = 1;
        if (info.bindings[i] == U) {
            buffers[i] = {res[i].buffer, res[i].offset, res[i].size};
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &buffers[i];
        } else {
            images[i] = {VK_NULL_HANDLE, res[i].view, VK_IMAGE_LAYOUT_GENERAL};
            w.descriptorType = info.bindings[i] == T ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo = &images[i];
        }
        writes.push_back(w);
    }
    const VkDescriptorImageInfo point{g.point, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo linear{g.linear, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    for (const auto& [binding, info_ptr] : {std::pair<uint32_t, const VkDescriptorImageInfo*>{1000, &point},
                                           std::pair<uint32_t, const VkDescriptorImageInfo*>{1001, &linear}}) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = info_ptr;
        writes.push_back(w);
    }
    const bool timed = gprof_begin(cmd);   // (RX_VK_PASS_TIMING=1: GPU ms by pass name)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, (uint32_t)writes.size(), writes.data());
    vkCmdDispatch(cmd, std::max(1u, groups_x), std::max(1u, groups_y), 1);
    if (timed) gprof_end(cmd, std::string("fsr3 ") + info.name);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

std::vector<Image*> all_images() {
    std::vector<Image*> all{&g.farthest_depth, &g.shading_change, &g.new_locks, &g.spd_mips, &g.farthest_depth_mip1,
                            &g.dilated_motion_vectors, &g.dilated_depth, &g.previous_nearest_depth,
                            &g.dilated_reactive_masks, &g.frame_info, &g.spd_atomic, &g.lanczos_lut, &g.default_reactive,
                            &g.output};
    for (size_t i = 0; i < 2; i++) {
        all.push_back(&g.accumulation[i]);
        all.push_back(&g.luma[i]);
        all.push_back(&g.luma_history[i]);
        all.push_back(&g.internal_upscaled[i]);
    }
    return all;
}

/// The pipelines and the internal images for an evaluation at these sizes, made again when the sizes change
bool prepare(uint32_t render_width, uint32_t render_height, uint32_t upscale_width, uint32_t upscale_height) {
    if (!supported()) return false;
    if (!g.tried) {
        g.tried = true;
        make_uniform_layout();
        g.ok = make_samplers();
        for (size_t i = 0; i < kPassCount && g.ok; i++) {
            const bool needed = g.wave ? (i != P_LUMA_PYRAMID_NOWAVE && i != P_SHADING_CHANGE_PYRAMID_NOWAVE)
                                       : (i != P_LUMA_PYRAMID && i != P_SHADING_CHANGE_PYRAMID);
            if (needed) g.ok = make_pipeline(g.passes[i], PASSES[i]);
        }
        if (g.ok) logf("vk: fsr3: its passes are ready (%s subgroup operations)", g.wave ? "with" : "without");
    }
    if (!g.ok) return false;
    if (g.output.image && g.render_width == render_width && g.render_height == render_height &&
        g.upscale_width == upscale_width && g.upscale_height == upscale_height)
        return true;

    const uint32_t rw = std::max(1u, render_width), rh = std::max(1u, render_height);
    const uint32_t uw = std::max(1u, upscale_width), uh = std::max(1u, upscale_height);
    bool made = true;
    for (size_t i = 0; i < 2 && made; i++) {
        made = made && make_image(g.accumulation[i], VK_FORMAT_R8_UNORM, rw, rh);
        made = made && make_image(g.luma[i], VK_FORMAT_R16_SFLOAT, rw, rh);
        // (the pass files declare the luma history as rgba8; the SDK allocates RGBA16F, which a storage format qualifier
        // does not match, so this allocates what the shaders store)
        made = made && make_image(g.luma_history[i], VK_FORMAT_R8G8B8A8_UNORM, rw, rh);
        made = made && make_image(g.internal_upscaled[i], VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh);
    }
    const uint32_t half_w = std::max(1u, rw / 2), half_h = std::max(1u, rh / 2);
    const uint32_t spd_levels = (uint32_t)std::floor(std::log2((float)std::max(half_w, half_h))) + 1;
    made = made && make_image(g.farthest_depth, VK_FORMAT_R16_SFLOAT, rw, rh);
    made = made && make_image(g.shading_change, VK_FORMAT_R8_UNORM, half_w, half_h);
    made = made && make_image(g.new_locks, VK_FORMAT_R8_UNORM, uw, uh);
    made = made && make_image(g.spd_mips, VK_FORMAT_R16G16_SFLOAT, half_w, half_h, std::max(spd_levels, kSpdStoredMips));
    made = made && make_image(g.farthest_depth_mip1, VK_FORMAT_R16_SFLOAT, half_w, half_h);
    made = made && make_image(g.dilated_motion_vectors, VK_FORMAT_R16G16_SFLOAT, rw, rh);
    made = made && make_image(g.dilated_depth, VK_FORMAT_R32_SFLOAT, rw, rh);
    made = made && make_image(g.previous_nearest_depth, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.dilated_reactive_masks, VK_FORMAT_R8G8B8A8_UNORM, rw, rh);
    made = made && make_image(g.frame_info, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1);
    made = made && make_image(g.spd_atomic, VK_FORMAT_R32_UINT, 1, 1);
    made = made && make_image(g.lanczos_lut, VK_FORMAT_R16_SNORM, 128, 1);
    made = made && make_image(g.default_reactive, VK_FORMAT_R8_UNORM, 1, 1);
    made = made && make_image(g.output, VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh);
    g.first_evaluation = true;   // (new images: their first layout and cleared, the history from nothing)
    g.lut_uploaded = false;
    g.parity = 0;
    if (!made) {
        for (Image* t : all_images()) drop_image(*t);
        g.render_width = g.render_height = g.upscale_width = g.upscale_height = 0;
        return false;
    }
    g.render_width = rw, g.render_height = rh, g.upscale_width = uw, g.upscale_height = uh;
    logf("vk: fsr3: %ux%u -> %ux%u", rw, rh, uw, uh);
    return true;
}

} // namespace

int prepare_sizes(uint32_t render_width, uint32_t render_height, uint32_t upscale_width, uint32_t upscale_height) {
    if (!supported()) return -1;
    free_dead();   // (as upscale does first: a prepare that fails retires what it made, and the next try must not keep it all)
    const bool had = g.output.image && g.render_width == std::max(1u, render_width) &&
                     g.render_height == std::max(1u, render_height) && g.upscale_width == std::max(1u, upscale_width) &&
                     g.upscale_height == std::max(1u, upscale_height);
    if (!prepare(render_width, render_height, upscale_width, upscale_height)) return g.tried && !g.ok ? -1 : 0;
    return had ? 1 : 2;
}

bool supported() {
    if (!dev.device) return false;   // (asked before the device is made, by the menu: not an answer to keep)
    static const bool answer = [] {
        // (the widest pass pushes thirteen descriptors and the two samplers)
        if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || dev.max_push_descriptors < 15) {
            logf("vk: fsr3: not on this GPU: no push descriptors, or fewer than 15 (%u)", dev.max_push_descriptors);
            return false;
        }
        if (!dev.features.shaderStorageImageExtendedFormats || !dev.features.shaderStorageImageWriteWithoutFormat) {
            logf("vk: fsr3: not on this GPU: no extended storage image formats, or no storage writes without a format");
            return false;
        }
        if (!formats_supported()) return false;
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sg;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        const VkSubgroupFeatureFlags want = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
        g.wave = (sg.supportedOperations & want) == want && (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);
        logf("vk: fsr3: AMD FSR 3.1 can run on this GPU");
        return true;
    }();
    return answer;
}

uint64_t memory() {
    uint64_t sum = 0;
    for (const Image* t : all_images())
        if (t->image) sum += allocation_size(t->memory);
    return sum;
}

void release() {
    for (Image* t : all_images()) drop_image(*t);   // (retired: freed once the GPU's uses of them are done)
    g.render_width = g.render_height = g.upscale_width = g.upscale_height = 0;
    free_dead();
}

bool upscale(VkCommandBuffer cmd, const Inputs& in) {
    free_dead();
    if (!in.color || !in.depth || !in.motion) return false;
    if (!prepare(in.render_width, in.render_height, in.upscale_width, in.upscale_height)) return false;

    const UniformUpload uniforms = uniform_upload(g.frame_stride);
    if (!uniforms.binding) return false;

    const bool first = g.first_evaluation;
    const bool reset = in.reset || first;
    g.first_evaluation = false;
    const int32_t rw = (int32_t)g.render_width, rh = (int32_t)g.render_height;
    const int32_t uw = (int32_t)g.upscale_width, uh = (int32_t)g.upscale_height;

    Constants k{};
    k.render_size[0] = rw, k.render_size[1] = rh;
    k.upscale_size[0] = uw, k.upscale_size[1] = uh;
    k.max_render_size[0] = rw, k.max_render_size[1] = rh;
    k.max_upscale_size[0] = uw, k.max_upscale_size[1] = uh;
    for (int i = 0; i < 2; i++) {
        k.previous_render_size[i] = first ? k.render_size[i] : g.previous_render_size[i];
        k.previous_upscale_size[i] = first ? k.upscale_size[i] : g.previous_upscale_size[i];
        k.jitter[i] = in.jitter[i];
        k.previous_jitter[i] = first ? in.jitter[i] : g.previous_jitter[i];
    }
    memcpy(k.device_to_view_depth, in.camera.device_to_view_depth, sizeof k.device_to_view_depth);
    k.tan_half_fov = in.camera.tan_half_fov_x;   // (not read by the 3.1.4 upscaler's passes; set as the SDK sets it)
    k.motion_vector_scale[0] = in.motion_scale[0] / (float)std::max(1, rw);
    k.motion_vector_scale[1] = in.motion_scale[1] / (float)std::max(1, rh);
    k.downscale_factor[0] = (float)rw / (float)std::max(1, uw);
    k.downscale_factor[1] = (float)rh / (float)std::max(1, uh);
    k.jitter_phase_count = (float)std::max(1u, in.jitter_phases);
    k.delta_time = std::clamp(in.frame_time_ms / 1000.0f, 0.0f, 1.0f);
    k.delta_pre_exposure = 1.0f;
    k.view_space_to_meters = 1.0f;
    g.frame_index = reset ? 0.0f : g.frame_index + 1.0f;
    k.frame_index = g.frame_index;
    k.velocity_factor = 1.0f;   // (the SDK's FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_* defaults)
    k.reactiveness_scale = 1.0f;
    k.shading_change_scale = 1.0f;
    k.accumulation_added_per_frame = 1.0f / 3.0f;
    k.min_disocclusion_accumulation = -1.0f / 3.0f;
    for (int i = 0; i < 2; i++) {
        g.previous_render_size[i] = k.render_size[i];
        g.previous_upscale_size[i] = k.upscale_size[i];
        g.previous_jitter[i] = in.jitter[i];
    }

    const uint32_t spd_x = (g.render_width - 1) / 64 + 1;
    const uint32_t spd_y = (g.render_height - 1) / 64 + 1;
    SpdConstants spd{};
    spd.mips = (uint32_t)std::min(std::floor(std::log2((float)std::max(g.render_width, g.render_height))), 12.0f);
    spd.num_work_groups = spd_x * spd_y;
    spd.render_size[0] = g.render_width, spd.render_size[1] = g.render_height;

    // This evaluation owns its constants until its recording has completed.
    const VkDeviceSize base = uniforms.binding.offset;
    uint8_t* const mapped = uniforms.mapped;
    memcpy(mapped, &k, sizeof k);
    memcpy(mapped + g.slot_fsr, &spd, sizeof spd);
    const VkDeviceSize lut_at = g.slot_fsr + g.slot_spd;
    if (!g.lut_uploaded) {   // (the accumulate pass's Lanczos weights, 128 taps as the SDK builds them)
        int16_t weights[128];
        for (size_t i = 0; i < 128; i++) {
            const float x = 2.0f * (float)i / 127.0f;
            const float pi_x = 3.14159265358979f * x;
            const float y = std::abs(x) < FLT_EPSILON ? 1.0f : (std::sin(pi_x) / pi_x) * (std::sin(0.5f * pi_x) / (0.5f * pi_x));
            weights[i] = (int16_t)std::lround(y * 32767.0f);
        }
        memcpy(mapped + lut_at, weights, sizeof weights);
    }
    flush_mapped_range(uniforms.memory, uniforms.binding.offset, uniforms.binding.size);

    const uint32_t p = g.parity, q = p ^ 1u;

    // What was written before (the inputs: vk_fsr3_inputs.cpp) has to be visible, and new images need their first layout.
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const std::vector<Image*> all = all_images();
    if (first) {
        std::vector<VkImageMemoryBarrier2> to_general;
        to_general.reserve(all.size());
        for (const Image* t : all) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t->image;
            b.subresourceRange = ALL_LEVELS;
            to_general.push_back(b);
        }
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = (uint32_t)to_general.size();
        di.pImageMemoryBarriers = to_general.data();
        vkCmdPipelineBarrier2(cmd, &di);
        const VkClearColorValue zero{};
        for (const Image* t : all) vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
    }
    if (!g.lut_uploaded) {
        g.lut_uploaded = true;
        VkBufferImageCopy copy{};
        copy.bufferOffset = base + lut_at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {128, 1, 1};
        vkCmdCopyBufferToImage(cmd, uniforms.binding.buffer, g.lanczos_lut.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    }
    // Per frame: the nearest-depth scratch, the SPD counter and its mips; the history on a reset.
    const VkClearColorValue zero{};
    for (const Image* t : {&g.previous_nearest_depth, &g.spd_atomic, &g.spd_mips})
        vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
    if (reset) {
        vkCmdClearColorImage(cmd, g.accumulation[p].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
        VkClearColorValue info{};
        info.float32[0] = -1.0f, info.float32[1] = 1.0f;
        vkCmdClearColorImage(cmd, g.frame_info.image, VK_IMAGE_LAYOUT_GENERAL, &info, 1, &ALL_LEVELS);
    }
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    const auto tex = [](const Image& t) { return Resource{t.view}; };
    const auto mip = [](uint32_t level) {
        const size_t n = g.spd_mips.levels.size();
        return Resource{g.spd_mips.levels[std::min<size_t>(level, n ? n - 1 : 0)]};
    };
    const Resource color{in.color}, depth{in.depth}, motion{in.motion};
    const Resource exposure = tex(g.frame_info);   // (FSR computes its own exposure)
    const Resource current_luma = tex(g.luma[p]);
    const Resource cb_fsr{VK_NULL_HANDLE, uniforms.binding.buffer, base, g.slot_fsr};
    const Resource cb_spd{VK_NULL_HANDLE, uniforms.binding.buffer, base + g.slot_fsr, g.slot_spd};
    const auto groups = [](int32_t size, uint32_t tile) { return ((uint32_t)std::max(1, size) + tile - 1) / tile; };
    const auto run = [&](size_t pass, std::initializer_list<Resource> res, uint32_t gx, uint32_t gy) {
        record_pass(cmd, pass, res.begin(), res.size(), gx, gy);
    };

    run(P_PREPARE_INPUTS, {motion, depth, color, tex(g.dilated_motion_vectors), tex(g.dilated_depth),
                           tex(g.previous_nearest_depth), tex(g.farthest_depth), current_luma, cb_fsr},
        groups(rw, 8), groups(rh, 8));
    run(g.wave ? P_LUMA_PYRAMID : P_LUMA_PYRAMID_NOWAVE,
        {current_luma, tex(g.farthest_depth), tex(g.spd_atomic), tex(g.frame_info), mip(0), mip(1), mip(2), mip(3), mip(4),
         mip(5), tex(g.farthest_depth_mip1), cb_fsr, cb_spd},
        spd_x, spd_y);
    // The two pyramid passes share one atomic counter, and the single-pass downsampler uses it to elect the workgroup that
    // finishes the chain: it counts up to num_work_groups and the last one through does the tail. Cleared only once a frame,
    // the second pass would start at the total, elect no workgroup, and the ones waiting on the tail would spin
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cmd, g.spd_atomic.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    run(g.wave ? P_SHADING_CHANGE_PYRAMID : P_SHADING_CHANGE_PYRAMID_NOWAVE,
        {current_luma, tex(g.luma[q]), tex(g.dilated_motion_vectors), exposure, tex(g.spd_atomic), mip(0), mip(1), mip(2),
         mip(3), mip(4), mip(5), cb_fsr, cb_spd},
        spd_x, spd_y);
    run(P_SHADING_CHANGE, {tex(g.spd_mips), tex(g.shading_change), cb_fsr}, groups(rw / 2, 8), groups(rh / 2, 8));
    run(P_PREPARE_REACTIVITY,
        {tex(g.previous_nearest_depth), tex(g.dilated_motion_vectors), tex(g.dilated_depth),
         in.reactive ? Resource{in.reactive} : tex(g.default_reactive),
         tex(g.default_reactive), tex(g.accumulation[p]), tex(g.shading_change), current_luma, exposure,
         tex(g.dilated_reactive_masks), tex(g.new_locks), tex(g.accumulation[q]), cb_fsr},
        groups(rw, 8), groups(rh, 8));
    run(P_LUMA_INSTABILITY,
        {exposure, tex(g.dilated_reactive_masks), tex(g.dilated_motion_vectors), tex(g.frame_info), tex(g.luma_history[p]),
         tex(g.farthest_depth_mip1), current_luma, tex(g.luma_history[q]), tex(g.farthest_depth), cb_fsr},
        groups(rw, 8), groups(rh, 8));
    run(P_ACCUMULATE,
        {exposure, tex(g.dilated_reactive_masks), tex(g.dilated_motion_vectors), tex(g.internal_upscaled[p]),
         tex(g.lanczos_lut), tex(g.farthest_depth_mip1), current_luma, tex(g.farthest_depth), color,
         tex(g.internal_upscaled[q]), tex(g.output), tex(g.new_locks), cb_fsr},
        groups(uw, 8), groups(uh, 8));
    g.parity = q;
    return true;
}

VkImage output_image() { return g.output.image; }
VkImageView output_view() { return g.output.view; }

} // namespace mkwvk::fsr3

namespace mkwvk {

extern std::atomic<bool> present_native_pq;   // (vk_replay.cpp do_present: the game presents its own HDR10 picture, Native)

namespace {

int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}

/// The network's output from FSR's picture: dlss_out.comp, the same pass DLSS's picture goes through (the colour FSR was
/// given is the network's perceptual encoding, which this squares back to linear colour, as the network's own output does)
struct Out {
    std::mutex mu;
    bool tried = false;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE;
    const char* reason = nullptr;
    int reason_repeats = 0;
    bool told_first = false;
    uint64_t drawn = 0;
};
Out& out_state() {
    static Out o;
    return o;
}

bool make_out_pipeline() {
    Out& o = out_state();
    if (o.pipe) return true;
    if (o.tried) return false;
    o.tried = true;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &o.set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &o.set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &o.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof DLSS_OUT_COMP_SPV, mi.pCode = DLSS_OUT_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = o.layout;
    const VkResult r = shared_compute_pipeline(&ci, &o.pipe);   // (kept across runs, above)
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return o.pipe = VK_NULL_HANDLE, false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev.device, &si, nullptr, &o.point) != VK_SUCCESS) {
        vkDestroyPipeline(dev.device, o.pipe, nullptr);
        return o.pipe = VK_NULL_HANDLE, false;
    }
    return true;
}

bool fail(const char* why) {   // (the network runs: why, when it changes and every 600th time)
    Out& o = out_state();
    if (why != o.reason || ++o.reason_repeats % 600 == 0) logf("vk: fsr3: the game's upscale runs (%s)", why);
    if (why != o.reason) o.reason = why, o.reason_repeats = 0;
    return false;
}

} // namespace

bool fsr3_chosen() {
    static const int env = env_switch("RX_VK_FSR");
    if (env == 0) return false;
    // (the Upscaler row's FSR 3.1, upscaler_rows.h, read each time: the row changes it while the game runs. RX_VK_DLSS=1 / 2
    // or RX_VK_FSR4=1 / 2 goes over the row, its FSR 3.1 too: vk_temporal.cpp's chain draws then)
    return (env > 0 || (opts.dlss == upscaler_rows::FSR3 && !temporal_env_chosen())) && fsr3::supported();
}

bool fsr3_on_chain() {
    static const bool on = env_switch("RX_VK_FSR3_CHAIN") != 0;
    return on;
}

bool fsr3_wanted() { return !fsr3_on_chain() && fsr3_chosen(); }

// ---- (hookshot) FSR 3.1 as an engine of the temporal chain (vk_temporal.cpp) ----
// The chain's frame into the passes above: its colour (the network's input as it samples it), its depth reversed for FSR
// (the chain's prep takes it so for an engine with fsr_depth), its motion toward the frame before in render pixels with the
// skinned draws' own drawn over the camera's (vk_temporal.cpp actor_motion), and the jitter the host gave the camera. The
// passes' picture (RGBA16F at the output size) is copied into the chain's output, which the chain squares back into the
// network's output with dlss_out.comp, as fsr3_upscale's own last pass does.
// With the game's native HDR the chain gives it linear light past 1 instead, and takes its picture back linear (chain_hdr):
// the passes are built for HDR colour (FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT) and work out their own exposure.
namespace {
struct ChainState {
    std::chrono::steady_clock::time_point last{};
    uint64_t drawn = 0;
    bool told = false;
    // (HDR colour this frame, as chain_prepare decided it; a change of encoding starts a new history: the one kept was in
    // the other)
    bool hdr = false, hdr_changed = false;
};
ChainState& chain_state() {
    static ChainState& c = *new ChainState;   // (never destroyed: as the chain's own state)
    return c;
}
bool chain_start() { return fsr3::supported(); }
int chain_prepare(VkCommandBuffer, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool, uint32_t* in_w, uint32_t* in_h,
                  const char** why) {
    // (the passes' pipelines and images for these sizes, here and not in the evaluation: an image that cannot be made now
    // (VRAM full as the size changes) is "not this frame", and the next frame tries again, as fsr3_upscale's own path did)
    *in_w = rw, *in_h = rh;
    // (the game's native HDR: what the game presents, not the option and the swapchain: with Native picked and the game's
    // picture not PQ, the scene is LDR. RX_VK_FSR3_HDR=0: the colour clamped at the paper white with Native too, as before)
    static const bool hdr_allowed = env_switch("RX_VK_FSR3_HDR") != 0;
    const bool hdr = hdr_allowed && present_native_pq.load(std::memory_order_relaxed);
    ChainState& c = chain_state();
    if (hdr != c.hdr) {
        logf("vk: fsr3 on the chain: %s", hdr ? "HDR colour, linear (the game's native HDR)" : "LDR colour (the game's native HDR off)");
        c.hdr = hdr, c.hdr_changed = true;
    }
    const int r = fsr3::prepare_sizes(rw, rh, ow, oh);
    if (r == 0) *why = "its images could not be made at this size";
    if (r < 0) *why = "its passes could not be made";
    return r;
}
bool chain_evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {
    ChainState& c = chain_state();
    fsr3_math::Camera camera;
    if (!fsr3_math::camera_from_projection(f.projection, camera)) {
        logf("vk: fsr3 on the chain: the scene's projection is not one FSR takes");
        return false;
    }
    if (!f.color.view || !f.depth.view || !f.motion.view || !f.output.image || f.output.format != VK_FORMAT_R16G16B16A16_SFLOAT)
        return false;
    fsr3::Inputs ai;
    ai.color = f.color.view, ai.depth = f.depth.view, ai.motion = f.motion.view;
    ai.render_width = f.render_w, ai.render_height = f.render_h;
    ai.upscale_width = f.output.width, ai.upscale_height = f.output.height;
    ai.jitter[0] = f.jitter[0], ai.jitter[1] = f.jitter[1];
    ai.jitter_phases = f.jitter_phases ? f.jitter_phases : 8;
    ai.camera = camera;
    const auto now = std::chrono::steady_clock::now();
    ai.frame_time_ms = c.last.time_since_epoch().count() && !f.reset
                           ? (float)std::min(250.0, std::chrono::duration<double, std::milli>(now - c.last).count())
                           : 16.667f;
    c.last = now;
    ai.reset = f.reset || c.hdr_changed;
    c.hdr_changed = false;
    // (what the scene's transparent draws changed, when RX_VK_REACTIVE asks for it and there were any: vk_reactive.h. The
    // same mask DLSS gets on this chain, as large as the chain's inputs; without one FSR's default reactivity, as before)
    if (reactive::Mask m; reactive::mask(cmd, f.render_w, f.render_h, f.color.width, f.color.height, f.color.view, m))
        ai.reactive = m.view;
    if (!fsr3::upscale(cmd, ai)) return false;
    const VkImage picture = fsr3::output_image();
    if (!picture) return false;
    full_barrier(cmd);   // (the accumulate and RCAS passes' writes, before the copy reads them)
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {f.output.width, f.output.height, 1};
    vkCmdCopyImage(cmd, picture, VK_IMAGE_LAYOUT_GENERAL, f.output.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    if (++c.drawn % 600 == 0 || !c.told)
        logf("vk: fsr3 on the chain: %llu frames drawn, %ux%u -> %ux%u, jitter %+.3f %+.3f px of %u phases, depth %s",
             (unsigned long long)c.drawn, f.render_w, f.render_h, f.output.width, f.output.height, f.jitter[0], f.jitter[1],
             ai.jitter_phases, camera.flip_depth ? "reversed for FSR by the chain" : "reversed already");
    c.told = true;
    return true;
}
void chain_release() { fsr3::release(); }   // (the passes stay; their images are made again when FSR 3.1 draws next)
uint64_t chain_memory() { return fsr3::memory(); }   // (its internal images, for the chain's status line)
/// It takes HDR colour while the game presents its native HDR (chain_prepare): the chain gives it linear light, unclamped
bool chain_hdr() { return chain_state().hdr; }
// (hdr: linear light past 1 with the game's native HDR, chain_hdr; fsr_depth: its passes are built with
// FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH, scripts/fsr3_shaders.py; retry: an
// evaluation that fails (no room for its constants in the frame's stream) leaves that frame to the network, and the next one
// tries again. Named, so a field added to TemporalEngine cannot shift these into another)
const TemporalEngine kChainEngine{.name = "AMD FSR 3.1", .short_name = "FSR 3.1", .start = chain_start,
                                  .prepare = chain_prepare, .evaluate = chain_evaluate, .release = chain_release,
                                  .memory = chain_memory, .hdr = chain_hdr, .fsr_depth = true, .retry = true};
} // namespace

const TemporalEngine* fsr3_engine() { return &kChainEngine; }

bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                  VkImageView out_view, uint32_t out_level) {
    const bool upscaling = fsr3_wanted();
    if (!upscaling && !fsr3_inputs::wanted()) return false;
    if (!in || !in_view || !out || !out_view) return false;
    // (another network of the upscale's shape, as temporal_upscale takes them: only the scene into the picture. The gameplay
    // networks sample the scene's B10G11R11 colour and write the B10G11R11 picture)
    if (!known && (in->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
                   (out->width >> out_level) <= in->width || !fsr3_inputs::is_scene(in)))
        return false;
    fsr3_inputs::Frame f;
    if (!fsr3_inputs::prepare(cmd, in, in_view, f)) return upscaling ? fail("its inputs could not be made") : false;
    if (!upscaling) return false;   // (frame interpolation's inputs alone: the network or DLSS upscales)
    Out& o = out_state();
    std::lock_guard lk(o.mu);
    const uint32_t ow = std::max(1u, out->width >> out_level), oh = std::max(1u, out->height >> out_level);
    if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || ow < f.width || oh < f.height) return fail("unexpected output");
    if (!make_out_pipeline()) return fail("its output pass was not made");
    struct Disturbed {   // (from here commands are recorded: the program's descriptor pushes are pushed again after)
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    fsr3::Inputs ai;
    ai.color = f.color, ai.depth = f.depth, ai.motion = f.motion;
    ai.render_width = f.width, ai.render_height = f.height;
    ai.upscale_width = ow, ai.upscale_height = oh;
    ai.jitter[0] = f.jitter[0], ai.jitter[1] = f.jitter[1];
    ai.jitter_phases = fsr3_inputs::jitter_size(2) ? fsr3_inputs::jitter_size(2) : 8;
    ai.camera = f.camera;
    ai.frame_time_ms = f.frame_ms;
    ai.reset = f.reset;
    // (what the scene's transparent draws changed, when RX_VK_REACTIVE asks for it and there were any: vk_reactive.h)
    if (reactive::Mask m; reactive::mask(cmd, f.width, f.height, f.width, f.height, f.color, m)) ai.reactive = m.view;
    if (!fsr3::upscale(cmd, ai)) return fail("its passes could not be recorded");
    {
        const int32_t pc[2] = {(int32_t)ow, (int32_t)oh};
        const VkDescriptorImageInfo info[2] = {{o.point, fsr3::output_view(), VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &info[0];
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &info[1];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, o.pipe);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, o.layout, 0, 2, w);
        vkCmdPushConstants(cmd, o.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
    }
    full_barrier(cmd);
    fsr3_inputs::upscaled(f.width, f.height, ow, oh);
    // (every 600 frames: that it still draws, and the jitter it was given, which says the host's jitter is on)
    if (++o.drawn % 600 == 0)
        logf("vk: fsr3: %llu frames drawn, %ux%u -> %ux%u, this one's jitter %+.3f %+.3f px of %u phases", (unsigned long long)o.drawn,
             f.width, f.height, ow, oh, f.jitter[0], f.jitter[1], ai.jitter_phases);
    if (o.reason) logf("vk: fsr3: FSR 3.1 draws the upscale again (frame %llu)", (unsigned long long)f.frame);
    o.reason = nullptr;
    if (!o.told_first) {
        o.told_first = true;
        logf("vk: fsr3 %ux%u -> %ux%u with assembled depth and camera motion (network %016llx; jitter %+.3f %+.3f px, "
             "%u phases)",
             f.width, f.height, ow, oh, (unsigned long long)key, f.jitter[0], f.jitter[1], ai.jitter_phases);
    }
    return true;
}

} // namespace mkwvk
