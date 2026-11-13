// (hookshot) The temporal upscalers' reactive mask: the scene's colour copied before its transparent draws, and compared
// at the upscale with what its target holds then (shaders/reactive_mask.comp). See vk_reactive.h.
//
// One copy is kept, the last made: the draws after it are the scene's last run of transparent ones (reactive_policy.h).
// The copy and the mask are recorded in the frame's order, so with the parallel replay (RX_VK_PARALLEL), where a frame is
// recorded in pieces at once, what this file remembers of the copy may be a piece ahead or behind: the mask is then of
// another copy of the same target, or left out for a frame, never of another image.
#include "vk_reactive.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>

#include "vk_draw.h"   // (descriptors_disturbed)
#include "vk_texture.h"

#include "shaders/reactive_mask_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk::reactive {
namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

struct State {
    std::mutex mu;
    Image opaque;             // (the scene's colour before its transparent draws, in the scene's format)
    Image mask;
    // (the copy in `opaque`: the target it is of, and when)
    Texture* scene = nullptr;
    uint32_t level = 0, layer = 0, width = 0, height = 0;
    uint64_t scene_gen = 0;   // (texture_destroy_generation when copied: a target destroyed since is not read)
    uint64_t frame = 0;       // (texture_frame_no when copied)
    bool copied = false;      // (and not yet made into a mask)
    Image scene_view;         // (a view of the target's level for the mask's pass: .image is the target's, not ours)
    uint32_t view_level = 0, view_layer = 0;
    uint64_t view_gen = 0;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE;
    bool tried = false;
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;   // (what work in flight may still use: freed a few frames on)
    const char* reason = nullptr;
    bool told = false;
    uint64_t masks = 0;
};
State& st() {
    static State s;
    return s;
}

bool fail(const char* why) {   // (why, when it changes)
    State& s = st();
    if (why != s.reason) logf("vk: reactive mask: none (%s)", why), s.reason = why;
    return false;
}

float env_float(const char* name, float otherwise, float lo, float hi) {
    const char* e = getenv(name);
    if (!e || !*e) return otherwise;
    const float v = (float)atof(e);
    return v >= lo && v <= hi ? v : otherwise;   // (and a NaN is neither)
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

/// An image of ours, made again when its format or size changes, cleared when made (the mask's is larger than what is
/// written of it)
bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
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
    ic.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    const VkClearColorValue zero{};
    vkCmdClearColorImage(cmd, im.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &vc.subresourceRange);
    return true;
}

/// reactive_mask.comp's pipeline, once: two sampled inputs, the mask and the upscaler's colour as storage images
bool make_pipeline() {
    State& s = st();
    if (s.pipe) return true;
    if (s.tried || !dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    s.tried = true;
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; i++)
        b[i] = {i, i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 4, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &s.set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &s.set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &s.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof REACTIVE_MASK_COMP_SPV, mi.pCode = REACTIVE_MASK_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = s.layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) {
        logf("vk: reactive mask: its pipeline was not made (%d)", (int)r);
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

/// A view of the scene target's level, for sampling: kept while the target, the level and the textures alive are the same
VkImageView scene_view(const Texture* t, uint32_t level, uint32_t layer) {
    State& s = st();
    const uint64_t gen = texture_destroy_generation();
    Image& v = s.scene_view;
    if (v.view && v.image == t->image && s.view_level == level && s.view_layer == layer && s.view_gen == gen) return v.view;
    if (v.view) {
        const VkImageView old = v.view;
        later([old] { vkDestroyImageView(dev.device, old, nullptr); });
        v = {};
    }
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t->image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = t->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &v.view) != VK_SUCCESS) return v = {}, VK_NULL_HANDLE;
    v.image = t->image, s.view_level = level, s.view_layer = layer, s.view_gen = gen;
    return v.view;
}

} // namespace

bool wanted() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_REACTIVE");
        if (!e || atoi(e) <= 0) return false;
        // (the mask is written by a compute pass: R8 is not one of the formats every device stores into)
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_R8_UNORM, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !dev.push_descriptor) {
            logf("vk: reactive mask: RX_VK_REACTIVE is set, but this device cannot make one (%s)",
                 dev.push_descriptor ? "no R8 storage images" : "no push descriptors");
            return false;
        }
        logf("vk: reactive mask: on (RX_VK_REACTIVE): the scene's blended draws without depth writes are masked for DLSS "
             "and FSR 3.1");
        return true;
    }();
    return on;
}

bool copy_scene(VkCommandBuffer cmd, Texture* color, uint32_t level, uint32_t layer, uint32_t width, uint32_t height) {
    State& s = st();
    std::lock_guard lk(s.mu);
    if (!color || !color->image || !width || !height) return false;
    if (color->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || color->type != VK_IMAGE_TYPE_2D ||
        color->samples != VK_SAMPLE_COUNT_1_BIT || level >= color->levels || layer >= color->layers ||
        std::max(1u, color->width >> level) != width || std::max(1u, color->height >> level) != height)
        return fail("the scene's target is not a plain B10G11R11 one");
    if (!(color->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(color->usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return fail("the scene's target cannot be copied and sampled");
    free_dead();
    if (!make_image(cmd, s.opaque, color->format, width, height, 0)) return fail("its copy of the scene was not made");
    full_barrier(cmd);   // (the draws before, into the target)
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {width, height, 1};
    vkCmdCopyImage(cmd, color->image, VK_IMAGE_LAYOUT_GENERAL, s.opaque.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    s.scene = color, s.level = level, s.layer = layer, s.width = width, s.height = height;
    s.scene_gen = texture_destroy_generation(), s.frame = texture_frame_no(), s.copied = true;
    return true;
}

bool mask(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t image_width, uint32_t image_height,
          VkImageView color, Mask& out) {
    if (!wanted()) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    free_dead();
    const bool copied = s.copied;
    s.copied = false;   // (each frame's own copy)
    if (!copied || texture_frame_no() - s.frame > 1) return false;   // (nothing transparent this frame: no mask, no reason)
    if (!color || !width || !height || image_width < width || image_height < height) return fail("no colour input");
    if (s.scene_gen != texture_destroy_generation()) return fail("the scene's target may be gone");
    if (s.width != width || s.height != height) return fail("the copy is not the upscaler's input's size");
    if (!make_pipeline()) return fail("its pipeline was not made");
    const VkImageView now = scene_view(s.scene, s.level, s.layer);
    if (!now) return fail("no view of the scene's target");
    struct Disturbed {   // (from here commands are recorded: the program's descriptor pushes are pushed again after)
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    if (!make_image(cmd, s.mask, VK_FORMAT_R8_UNORM, image_width, image_height, VK_IMAGE_USAGE_STORAGE_BIT))
        return fail("its image was not made");
    static const float scale = env_float("RX_VK_REACTIVE_SCALE", 1.0f, 0.0f, 64.0f);
    static const float limit = env_float("RX_VK_REACTIVE_MAX", 0.9f, 0.0f, 1.0f);
    static const bool show = getenv("RX_VK_REACTIVE_DEBUG") && atoi(getenv("RX_VK_REACTIVE_DEBUG")) > 0;
    struct {
        int32_t size[2];
        float scale, limit;
        int32_t flags;
    } pc{{(int32_t)width, (int32_t)height}, scale, limit, show ? 1 : 0};
    static_assert(sizeof pc == 20, "reactive_mask.comp's push constants");
    full_barrier(cmd);   // (the scene's target and the copy, written before; the colour input, by the upscaler's own pass)
    {
        const VkDescriptorImageInfo info[4] = {{s.point, s.opaque.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {s.point, now, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.mask.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, color, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[4]{};
        for (uint32_t i = 0; i < 4; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipe);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 4, w);
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    }
    full_barrier(cmd);
    out.image = s.mask.image, out.view = s.mask.view, out.format = s.mask.format;
    out.image_width = s.mask.width, out.image_height = s.mask.height;
    if (s.reason) logf("vk: reactive mask: made again (frame %llu)", (unsigned long long)texture_frame_no());
    s.reason = nullptr;
    s.masks++;
    if (!s.told) {
        s.told = true;
        logf("vk: reactive mask: %ux%u from the scene's target #%u level %u, scale %g, at most %g%s", width, height,
             s.scene->id, s.level, scale, limit, show ? ", shown in the picture (RX_VK_REACTIVE_DEBUG)" : "");
    }
    return true;
}

} // namespace mkwvk::reactive
