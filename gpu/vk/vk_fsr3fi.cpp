// (hookshot) FSR 3 frame interpolation's pipelines, internal images and dispatches; see vk_fsr3fi.h
// for what it is for and where it slots in.
//
// The resource table, the per-pass binding lists, the pass order and the dispatch sizes are AMD's,
// not ours: ffx_opticalflow.cpp and ffx_frameinterpolation.cpp decide them, and each pass's GLSL
// numbers its own bindings (the FFX_OPTICALFLOW_BIND_* / FFX_FRAMEINTERPOLATION_BIND_* lines at the
// top of each file in third_party/fidelityfx-sdk/shaders). They are written out here in the SDK's
// order rather than derived, because getting one wrong is a validation error at best and a wrong
// picture at worst. vk_fsr3.cpp does the same for the upscaler and this file follows its shape.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include <utility>

#include "vk_fsr3fi.h"
#include "vk_menu.h"    // (the Frame generation row)
#include "vk_gprof.h"   // (RX_VK_PASS_TIMING: a span per pass, so the report names them)
#include "vk_texture.h"

#include "shaders/fsr3_fi_disocclusion_mask_comp_spv.h"
#include "shaders/fsr3_fi_game_motion_vector_field_comp_spv.h"
#include "shaders/fsr3_fi_game_vector_field_inpainting_pyramid_comp_spv.h"
#include "shaders/fsr3_fi_inpainting_comp_spv.h"
#include "shaders/fsr3_fi_inpainting_pyramid_comp_spv.h"
#include "shaders/fsr3_fi_interpolate_comp_spv.h"
#include "shaders/fsr3_fi_optical_flow_vector_field_comp_spv.h"
#include "shaders/fsr3_fi_reconstruct_and_dilate_comp_spv.h"
#include "shaders/fsr3_fi_reconstruct_previous_depth_comp_spv.h"
#include "shaders/fsr3_fi_setup_comp_spv.h"
#include "shaders/fsr3_of_compute_optical_flow_v5_comp_spv.h"
#include "shaders/fsr3_of_compute_scd_divergence_comp_spv.h"
#include "shaders/fsr3_of_filter_optical_flow_v5_comp_spv.h"
#include "shaders/fsr3_of_generate_scd_histogram_comp_spv.h"
#include "shaders/fsr3_of_luminance_pyramid_comp_spv.h"
#include "shaders/fsr3_of_prepare_luma_comp_spv.h"
#include "shaders/fsr3_of_scale_optical_flow_v5_comp_spv.h"
#include "vk_draw.h"   // (extern_guarded_shader_module, extern_guarded_compute)
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk::fsr3fi {
namespace {

// ---- the passes and what each one binds ----

/// A descriptor's kind. The optical-flow passes declare no sampler at all (every load there is a
/// texelFetch, under GL_EXT_samplerless_texture_functions); the interpolation passes declare one, a
/// linear clamp sampler at binding 1000, which is why the sampler is per-pass below and not the
/// fixed pair of samplers vk_fsr3.cpp pushes for every upscaler pass.
enum Kind : uint8_t { T, I, U, B };   // sampled texture, storage image, uniform buffer, storage buffer

/// One descriptor, at the binding number its pass's GLSL fixes it to. The numbers are not an index
/// into this list: three of the seventeen passes leave gaps where the SDK's HLSL root signature
/// needed a slot the GLSL does not declare.
struct Bind {
    uint16_t binding;
    Kind kind;
};

struct PassInfo {
    const char* name;
    const uint32_t* spv;
    size_t spv_bytes;
    const Bind* binds;
    size_t bind_count;
    bool sampler;   ///< declares the linear clamp sampler at binding 1000
};

// Optical flow. Seven passes, no samplers.
constexpr Bind OF_PREPARE_LUMA[]{{0, T}, {1, I}, {2, U}};
constexpr Bind OF_LUMINANCE_PYRAMID[]{{0, I}, {1, I}, {2, I}, {3, I}, {4, I},
                                      {5, I}, {6, I}, {7, U}, {8, U}};
constexpr Bind OF_SCD_HISTOGRAM[]{{0, T}, {1, I}, {2, U}};
constexpr Bind OF_SCD_DIVERGENCE[]{{0, I}, {1, I}, {2, I}, {3, I}, {4, U}};
constexpr Bind OF_SEARCH[]{{0, T}, {1, T}, {2, I}, {3, I}, {4, U}};
constexpr Bind OF_FILTER[]{{0, T}, {1, I}, {2, U}};
constexpr Bind OF_SCALE[]{{0, T}, {1, T}, {2, T}, {3, I}, {4, I}, {5, U}};

// Frame interpolation. Ten passes, each with the linear clamp sampler.
constexpr Bind FI_RECONSTRUCT_AND_DILATE[]{{0, T}, {1, T}, {2, I}, {3, I}, {4, I}, {5, U}};
constexpr Bind FI_SETUP[]{{0, T}, {1, I}, {2, I}, {3, I}, {4, I}, {5, I}, {6, B}, {7, U}};
constexpr Bind FI_RECONSTRUCT_PREVIOUS_DEPTH[]{{0, T}, {1, T}, {2, T}, {3, T}, {4, I}, {5, U}};
constexpr Bind FI_GAME_MOTION_VECTOR_FIELD[]{{0, T}, {1, T}, {2, T}, {3, T},
                                             {4, T}, {5, I}, {6, I}, {7, U}};
/// The game vector field's inpainting pyramid: two textures, the counter buffer, thirteen mip
/// levels, then both constant blocks. The pyramid is a single-pass downsampler, which is why it
/// wants every level bound at once and why it reads back level 5 through the same storage images.
constexpr Bind FI_GAME_VECTOR_FIELD_PYRAMID[]{
    {0, T},  {1, T},  {2, B},  {3, I},  {4, I},  {5, I},
    {6, I},  {7, I},  {8, I},  {9, I},  {10, I}, {11, I},
    {12, I}, {13, I}, {14, I}, {15, I}, {16, U}, {17, U}};
/// Binding 5 is not declared by this pass: the SDK's HLSL has an optical-flow debug view there and
/// the GLSL leaves the slot out, so the list jumps from 4 to 6.
constexpr Bind FI_OPTICAL_FLOW_VECTOR_FIELD[]{{0, T}, {1, T}, {2, T}, {3, T},
                                              {4, T}, {6, I}, {7, I}, {8, U}};
constexpr Bind FI_DISOCCLUSION_MASK[]{{0, T}, {1, T}, {2, T}, {3, T}, {4, T},
                                      {5, T}, {6, T}, {7, I}, {8, U}};
constexpr Bind FI_INTERPOLATE[]{{0, T}, {1, T}, {2, T}, {3, T}, {4, T}, {5, T},
                                {6, T}, {7, T}, {8, B}, {9, I}, {10, U}};
constexpr Bind FI_INPAINTING_PYRAMID[]{{0, T},  {1, B},  {2, I},  {3, I},  {4, I},  {5, I},
                                       {6, I},  {7, I},  {8, I},  {9, I},  {10, I}, {11, I},
                                       {12, I}, {13, I}, {14, I}, {15, U}, {16, U}};
/// Bindings 4 to 6 are the HUD-less and UI inputs this game does not have, which the GLSL does not
/// declare, so the output jumps to 7.
constexpr Bind FI_INPAINTING[]{{0, T}, {1, T}, {2, T}, {3, T}, {7, I}, {8, U}};

#define PASS(n, spv, b, s) PassInfo{n, spv, sizeof spv, b, sizeof b / sizeof b[0], s}

/// The passes, in the order PassId numbers them.
const PassInfo PASSES[]{
    PASS("of prepare_luma", FSR3_OF_PREPARE_LUMA_COMP_SPV, OF_PREPARE_LUMA, false),
    PASS("of luminance_pyramid", FSR3_OF_LUMINANCE_PYRAMID_COMP_SPV, OF_LUMINANCE_PYRAMID, false),
    PASS("of scd_histogram", FSR3_OF_GENERATE_SCD_HISTOGRAM_COMP_SPV, OF_SCD_HISTOGRAM, false),
    PASS("of scd_divergence", FSR3_OF_COMPUTE_SCD_DIVERGENCE_COMP_SPV, OF_SCD_DIVERGENCE, false),
    PASS("of search", FSR3_OF_COMPUTE_OPTICAL_FLOW_V5_COMP_SPV, OF_SEARCH, false),
    PASS("of filter", FSR3_OF_FILTER_OPTICAL_FLOW_V5_COMP_SPV, OF_FILTER, false),
    PASS("of scale", FSR3_OF_SCALE_OPTICAL_FLOW_V5_COMP_SPV, OF_SCALE, false),
    PASS("fi reconstruct_and_dilate", FSR3_FI_RECONSTRUCT_AND_DILATE_COMP_SPV,
         FI_RECONSTRUCT_AND_DILATE, true),
    PASS("fi setup", FSR3_FI_SETUP_COMP_SPV, FI_SETUP, true),
    PASS("fi reconstruct_previous_depth", FSR3_FI_RECONSTRUCT_PREVIOUS_DEPTH_COMP_SPV,
         FI_RECONSTRUCT_PREVIOUS_DEPTH, true),
    PASS("fi game_motion_vector_field", FSR3_FI_GAME_MOTION_VECTOR_FIELD_COMP_SPV,
         FI_GAME_MOTION_VECTOR_FIELD, true),
    PASS("fi game_vector_field_pyramid", FSR3_FI_GAME_VECTOR_FIELD_INPAINTING_PYRAMID_COMP_SPV,
         FI_GAME_VECTOR_FIELD_PYRAMID, true),
    PASS("fi optical_flow_vector_field", FSR3_FI_OPTICAL_FLOW_VECTOR_FIELD_COMP_SPV,
         FI_OPTICAL_FLOW_VECTOR_FIELD, true),
    PASS("fi disocclusion_mask", FSR3_FI_DISOCCLUSION_MASK_COMP_SPV, FI_DISOCCLUSION_MASK, true),
    PASS("fi interpolate", FSR3_FI_INTERPOLATE_COMP_SPV, FI_INTERPOLATE, true),
    PASS("fi inpainting_pyramid", FSR3_FI_INPAINTING_PYRAMID_COMP_SPV, FI_INPAINTING_PYRAMID, true),
    PASS("fi inpainting", FSR3_FI_INPAINTING_COMP_SPV, FI_INPAINTING, true),
};
#undef PASS

enum PassId : size_t {
    P_OF_PREPARE_LUMA,
    P_OF_LUMINANCE_PYRAMID,
    P_OF_SCD_HISTOGRAM,
    P_OF_SCD_DIVERGENCE,
    P_OF_SEARCH,
    P_OF_FILTER,
    P_OF_SCALE,
    P_FI_RECONSTRUCT_AND_DILATE,
    P_FI_SETUP,
    P_FI_RECONSTRUCT_PREVIOUS_DEPTH,
    P_FI_GAME_MOTION_VECTOR_FIELD,
    P_FI_GAME_VECTOR_FIELD_PYRAMID,
    P_FI_OPTICAL_FLOW_VECTOR_FIELD,
    P_FI_DISOCCLUSION_MASK,
    P_FI_INTERPOLATE,
    P_FI_INPAINTING_PYRAMID,
    P_FI_INPAINTING,
    P_COUNT,
};
static_assert(sizeof PASSES / sizeof PASSES[0] == P_COUNT, "PassId must name every pass");

// ---- the constant blocks ----

/// cbFI, exactly as ffx_frameinterpolation_private.h lays out FrameInterpolationConstants and as the
/// std140 block in ffx_frameinterpolation_callbacks_glsl.h reads it. Every pass reads it by offset,
/// so a field out of place is a wrong picture and not a compile error; the size is asserted because
/// that is the one part of the layout a compiler can check for us.
struct Constants {
    int32_t render_size[2];
    int32_t display_size[2];
    float display_size_rcp[2];
    float camera_near;
    float camera_far;
    int32_t upscaler_target_size[2];
    int32_t mode;
    int32_t reset;
    float device_to_view_depth[4];
    float delta_time;
    int32_t hudless_attached_factor;
    int32_t distortion_field_size[2];
    float optical_flow_scale[2];
    int32_t optical_flow_block_size;
    uint32_t dispatch_flags;
    int32_t max_render_size[2];
    int32_t optical_flow_half_res_mode;
    int32_t num_instances;
    int32_t interpolation_rect_base[2];
    int32_t interpolation_rect_size[2];
    float debug_bar_color[3];
    uint32_t back_buffer_transfer_function;
    float min_max_luminance[2];
    float tan_half_fov;
    int32_t pad1;
    float jitter[2];
    float motion_vector_scale[2];
};
static_assert(sizeof(Constants) == 176, "cbFI must match the SDK's layout");

/// cbInpaintingPyramid: the single-pass downsampler's setup, staged twice a frame because the two
/// pyramid passes downsample rectangles of different sizes.
struct PyramidConstants {
    uint32_t mips;
    uint32_t num_work_groups;
    uint32_t work_group_offset[2];
};
static_assert(sizeof(PyramidConstants) == 16, "cbInpaintingPyramid must match the SDK's layout");

/// cbOF, as ffx_opticalflow_private.h lays out OpticalflowConstants. The pyramid level in it changes
/// between dispatches, which is why there is a slot per level below rather than one block a frame.
struct OfConstants {
    int32_t input_luma_resolution[2];
    uint32_t pyramid_level;
    uint32_t pyramid_level_count;
    int32_t frame_index;
    uint32_t back_buffer_transfer_function;
    float min_max_luminance[2];
};
static_assert(sizeof(OfConstants) == 32, "cbOF must match the SDK's layout");

/// cbOF_SPD. Only numWorkGroupOpticalFlowInputPyramid is read by the luma pyramid (its mip count is
/// fixed at six in the shader itself), but the whole block is written because the GLSL declares it.
struct OfSpdConstants {
    uint32_t mips;
    uint32_t num_work_groups;
    uint32_t work_group_offset[2];
    uint32_t num_work_groups_pyramid;
    uint32_t pad0, pad1, pad2;
};
static_assert(sizeof(OfSpdConstants) == 32, "cbOF_SPD must match the SDK's layout");

// ---- state ----

constexpr uint32_t kMaxMade = 1;        ///< one midpoint frame between two of the game's (vk_fsr3fi.h)
constexpr uint32_t kOfLevels = 7;       ///< advancedAlgorithmIterations in ffx_opticalflow.cpp
constexpr uint32_t kOfBlockSize = 8;    ///< opticalFlowBlockSize, likewise
constexpr uint32_t kPyramidMips = 13;   ///< the mip UAVs the two inpainting pyramid passes declare
constexpr uint32_t kScdHistogramWidth = 256 * (3 * 3);   ///< HistogramBins * HistogramsPerDim^2
constexpr uint32_t kCountersBytes = 16;  ///< two uints, rounded up: COUNTER_SPD and the reset index

struct Pipeline {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;    ///< every level
    std::vector<VkImageView> levels;      ///< one per level, for the mipped ones
    uint32_t width = 0, height = 0, level_count = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

/// One descriptor of a dispatch: an image view, or a range of a buffer.
struct Resource {
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct State {
    bool tried = false;
    bool ok = false;
    /// An allocation failed once: the images are not re-made every frame afterwards, because a
    /// device that could not find the memory for them this frame will not find it in sixteen
    /// milliseconds and the retry would be a stall a frame for the rest of the run.
    bool failed = false;
    Pipeline passes[P_COUNT];
    VkSampler linear = VK_NULL_HANDLE;

    // Optical flow's internal images. The luma pyramids ping-pong on frame parity: one holds this
    // frame's luma and the other the frame before it, which is the pair the search works on.
    Image of_luma[2];                    ///< R8_UINT at display size, seven levels
    Image of_flow[2][kOfLevels];         ///< R16G16_SINT, one image a level (see make_sizes)
    Image of_vector;                     ///< the flow the interpolation reads, at level 0's size
    Image of_confidence;                 ///< zero: this flow produces none, and the SDK binds none
    Image of_scd_histogram, of_scd_previous_histogram, of_scd_temp, of_scd_output;

    // Frame interpolation's internal images. The first three are the SDK's "shared" resources, which
    // its own FSR3 pairing fills from the upscaler; here the reconstruct-and-dilate pass fills them.
    Image fi_dilated_depth, fi_dilated_motion_vectors, fi_reconstructed_depth_previous;
    Image fi_reconstructed_depth_interpolated;
    Image fi_game_mv_x, fi_game_mv_y, fi_of_mv_x, fi_of_mv_y;
    Image fi_disocclusion_mask, fi_inpainting_pyramid;
    Image fi_previous_source;            ///< the frame before this one, at display size
    Image fi_distortion_field;           ///< 1x1 and zero: this game has no lens distortion
    Image made[kMaxMade];

    // The counters the two pyramid passes and the setup pass share: a storage buffer, not an image,
    // because that is what the GLSL declares (FrameInterpolationRWCounters_t).
    VkBuffer counters = VK_NULL_HANDLE;
    VmaAllocation counters_memory = nullptr;

    uint32_t width = 0, height = 0, render_width = 0, render_height = 0;
    VkFormat present_format = VK_FORMAT_UNDEFINED;
    uint32_t ready = 0;                  ///< how many of `made` exist
    uint32_t flow_width[kOfLevels]{}, flow_height[kOfLevels]{};

    // Constant block layout; storage belongs to each recording, not a fixed frame ring.
    VkDeviceSize slot_fi = 0, slot_pyramid = 0, slot_of = 0, slot_of_spd = 0, frame_stride = 0;

    uint32_t parity = 0;
    int32_t of_frame_index = 0;
    bool first_frame = true;
    uint64_t sequence = 0;               ///< Inputs::sequence of the frame kept as the previous source
    bool in_general = false;             ///< the UNDEFINED -> GENERAL barriers have been recorded
};

State g;

constexpr VkImageSubresourceRange ALL_LEVELS{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                                             0, 1};

/// The made frame of Frame generation at 2x by this backend: RX_VK_FSR_FG=1 (opt-in). Without it the renderer's own blend
/// makes it, as at 3x and more. FSR 3.1's interpolation has no HUD-less colour here (the game draws its HUD into the same
/// picture), so it moved the HUD with the scene: smeared HUD, a ghost trail behind Link, a see-through rectangle, at 2x only
/// (#1635 and seven duplicates, 2026-11-10). RX_VK_FSR_FG=1 asks for it over the row, =0 never.
uint32_t asked() {
    static const int env = []() {
        const char* e = getenv("RX_VK_FSR_FG");
        return e && *e ? std::max(0, atoi(e)) : -1;
    }();
    if (env >= 0) return env ? 1u : 0u;
    return 0u;   // (opt-in: the renderer's own blend at 2x, see above)
}

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

// Images of an earlier size, released only after their GPU uses complete.
std::vector<std::pair<uint64_t, Image>> dead;

void destroy_now(Image& t) {
    for (VkImageView v : t.levels) vkDestroyImageView(dev.device, v, nullptr);
    if (t.view) vkDestroyImageView(dev.device, t.view, nullptr);
    if (t.image) mkwvk::destroy_image(t.image, t.memory);
    t = Image{};
}

/// Retain every view and its image until a later submission covers the old recordings.
void drop_image(Image& t) {
    if (t.image) dead.push_back({vk_retirement_value(), std::move(t)});
    t = Image{};
}

void free_dead() {
    const uint64_t now = vk_timeline_completed();
    for (size_t i = 0; i < dead.size();) {
        if (dead[i].first && now >= dead[i].first) {
            destroy_now(dead[i].second);
            dead[i] = std::move(dead.back());
            dead.pop_back();
        } else {
            i++;
        }
    }
}

/// One of the SDK's internal images: storage and sampled, GENERAL throughout, with a view per level
/// when it has more than one (both pyramids bind their levels separately while the passes that read
/// a pyramid read it as one mipped texture).
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
    // (TRANSFER_SRC as well as DST: the made frames are blitted out of by the present path, and a
    //  made frame of a repeated set is copied from the first one, so both directions are asked for
    //  everywhere rather than tracked per image)
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!create_image(ci, t.image, t.memory, false)) {
        logf("vk: fsr3fi: a %ux%u image (format %d) was not made", t.width, t.height, (int)format);
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.level_count, 0, 1};
    if (vkCreateImageView(dev.device, &vi, nullptr, &t.view) != VK_SUCCESS) return false;
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
    bindings.reserve(info.bind_count + 1);
    for (size_t i = 0; i < info.bind_count; i++) {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        switch (info.binds[i].kind) {
            case T: type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
            case I: type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
            case U: type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case B: type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
        }
        bindings.push_back({info.binds[i].binding, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    if (info.sampler)
        bindings.push_back({1000, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                            nullptr});
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
    VkResult made;
    {
        IntelDriverGate gate;
        if (extern_guarded_shader_module(&mi, &m) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = p.layout;
        made = extern_guarded_compute(VK_NULL_HANDLE, &ci, &p.pipeline);
    }
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (made != VK_SUCCESS) {
        logf("vk: fsr3fi: the %s pass pipeline was not made (%d)", info.name, (int)made);
        p.pipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/// The one sampler the interpolation passes declare: linear, clamped to the edge. It is the only
/// filtered read in the whole chain (the colour sources and the disocclusion mask are sampled in UV
/// space); every other load is a texelFetch and needs no sampler at all.
bool make_sampler() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f, si.maxLod = VK_LOD_CLAMP_NONE;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    return vkCreateSampler(dev.device, &si, nullptr, &g.linear) == VK_SUCCESS;
}

/// Every storage format the internal images need. A device missing one cannot run the passes, and
/// finding that out here is better than a failed image allocation in the middle of a frame.
bool formats_supported() {
    static const VkFormat needed[]{
        VK_FORMAT_R8_UINT,        VK_FORMAT_R8G8_UNORM,   VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R16G16_SFLOAT,  VK_FORMAT_R32_UINT,     VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R16G16B16A16_SFLOAT,
    };
    for (VkFormat f : needed) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, f, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
            !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            logf("vk: fsr3fi: format %d cannot be a storage and sampled image here", (int)f);
            return false;
        }
    }
    return true;
}

/// The device features the compiled passes ask for by capability, which is why they are checked here
/// rather than assumed. The interpolation and inpainting passes write (and the inpainting pass also
/// reads) the output image through an image declared with no format qualifier, because the SDK lets
/// the application choose the backbuffer's format; that compiles to StorageImageRead/
/// WriteWithoutFormat. Most of the internal images are in formats outside Vulkan's mandatory storage
/// set, which is StorageImageExtendedFormats. And both single-pass downsamplers and the flow search
/// use subgroup arithmetic and quad operations.
bool features_supported() {
    if (!dev.features.shaderStorageImageWriteWithoutFormat ||
        !dev.features.shaderStorageImageReadWithoutFormat) {
        logf("vk: fsr3fi: this GPU cannot read or write a storage image without a format, which the "
             "interpolation and inpainting passes need for the frame they write");
        return false;
    }
    if (!dev.features.shaderStorageImageExtendedFormats) {
        logf("vk: fsr3fi: this GPU has no extended storage image formats");
        return false;
    }
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    const VkSubgroupFeatureFlags want = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                        VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                        VK_SUBGROUP_FEATURE_QUAD_BIT;
    if ((sg.supportedOperations & want) != want ||
        !(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
        logf("vk: fsr3fi: this GPU's compute stage lacks the subgroup arithmetic or quad operations "
             "the pyramid and flow search passes were compiled with (%#x)",
             (unsigned)sg.supportedOperations);
        return false;
    }
    // The widest pass pushes eighteen descriptors and the sampler; a device that cannot take them
    // in one push would need a descriptor pool, which is a different backend from this one.
    uint32_t widest = 0;
    for (const PassInfo& p : PASSES)
        widest = std::max(widest, (uint32_t)p.bind_count + (p.sampler ? 1u : 0u));
    if (dev.max_push_descriptors < widest) {
        logf("vk: fsr3fi: this GPU takes %u push descriptors and a pass needs %u",
             dev.max_push_descriptors, widest);
        return false;
    }
    return true;
}

/// Aligned constant block offsets; the recording supplies storage for each evaluation.
/// Optical flow keeps a distinct block per pyramid level, plus the block for passes before the pyramid.
void make_uniform_layout() {
    const VkDeviceSize align =
        std::max<VkDeviceSize>(dev.props.limits.minUniformBufferOffsetAlignment, 16);
    const auto round_up = [align](VkDeviceSize n) { return (n + align - 1) / align * align; };
    g.slot_fi = round_up(sizeof(Constants));
    g.slot_pyramid = round_up(sizeof(PyramidConstants));
    g.slot_of = round_up(sizeof(OfConstants));
    g.slot_of_spd = round_up(sizeof(OfSpdConstants));
    g.frame_stride = g.slot_fi + 2 * g.slot_pyramid + (kOfLevels + 1) * g.slot_of + g.slot_of_spd;
}

/// Records one pass: its descriptors pushed at the binding numbers its GLSL fixes them to, then the
/// sampler it declares (when it declares one), then the dispatch.
void record_pass(VkCommandBuffer cmd, size_t pass, const Resource* res, size_t count, uint32_t gx,
                 uint32_t gy, uint32_t gz = 1) {
    const PassInfo& info = PASSES[pass];
    const Pipeline& p = g.passes[pass];
    std::vector<VkDescriptorImageInfo> images(count);
    std::vector<VkDescriptorBufferInfo> buffers(count);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(count + 1);
    for (size_t i = 0; i < count; i++) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = info.binds[i].binding;
        w.descriptorCount = 1;
        if (info.binds[i].kind == U || info.binds[i].kind == B) {
            buffers[i] = {res[i].buffer, res[i].offset, res[i].size};
            w.descriptorType = info.binds[i].kind == U ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &buffers[i];
        } else {
            images[i] = {VK_NULL_HANDLE, res[i].view, VK_IMAGE_LAYOUT_GENERAL};
            w.descriptorType = info.binds[i].kind == T ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo = &images[i];
        }
        writes.push_back(w);
    }
    const VkDescriptorImageInfo linear{g.linear, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    if (info.sampler) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = 1000;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &linear;
        writes.push_back(w);
    }
    // (one timed span per pass with RX_VK_PASS_TIMING=1: seventeen dispatches a frame needs a number
    //  per pass name, not another hypothesis about which of them costs)
    const bool timed = gprof_begin(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0,
                              (uint32_t)writes.size(), writes.data());
    vkCmdDispatch(cmd, std::max(1u, gx), std::max(1u, gy), std::max(1u, gz));
    if (timed) gprof_end(cmd, std::string("fsr3fi ") + info.name);
    // (every pass reads what the one before it wrote, and the two pyramids read back their own
    //  levels across workgroups)
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

/// ffxSpdSetup from ffx_spd.h: the thread group count over a rectangle in 64x64 tiles, the offset of
/// the first tile, and how many mips the downsampler is to write. `mips` below zero asks for the
/// count the rectangle's size implies, which is what both inpainting pyramids pass.
void spd_setup(uint32_t width, uint32_t height, uint32_t* groups, uint32_t* offset, uint32_t* mips,
               int fixed_mips = -1) {
    offset[0] = offset[1] = 0;
    groups[0] = (std::max(1u, width) - 1) / 64 + 1;
    groups[1] = (std::max(1u, height) - 1) / 64 + 1;
    if (fixed_mips >= 0) {
        *mips = (uint32_t)fixed_mips;
        return;
    }
    const float resolution = (float)std::max(std::max(1u, width), std::max(1u, height));
    *mips = (uint32_t)std::min(std::floor(std::log2(resolution)), 12.0f);
}

/// Every image this backend owns, in one list, so the one-time layout transition, the reset clears
/// and a failed make all walk the same set and none of them can forget one.
std::vector<Image*> all_images() {
    std::vector<Image*> all{&g.of_luma[0],
                            &g.of_luma[1],
                            &g.of_vector,
                            &g.of_confidence,
                            &g.of_scd_histogram,
                            &g.of_scd_previous_histogram,
                            &g.of_scd_temp,
                            &g.of_scd_output,
                            &g.fi_dilated_depth,
                            &g.fi_dilated_motion_vectors,
                            &g.fi_reconstructed_depth_previous,
                            &g.fi_reconstructed_depth_interpolated,
                            &g.fi_game_mv_x,
                            &g.fi_game_mv_y,
                            &g.fi_of_mv_x,
                            &g.fi_of_mv_y,
                            &g.fi_disocclusion_mask,
                            &g.fi_inpainting_pyramid,
                            &g.fi_previous_source,
                            &g.fi_distortion_field};
    for (uint32_t i = 0; i < 2; i++)
        for (uint32_t l = 0; l < kOfLevels; l++) all.push_back(&g.of_flow[i][l]);
    for (uint32_t i = 0; i < kMaxMade; i++)
        if (g.made[i].image) all.push_back(&g.made[i]);
    return all;
}

void clear_images(VkCommandBuffer cmd, std::initializer_list<const Image*> images) {
    const VkClearColorValue zero{};
    for (const Image* t : images)
        if (t->image)
            vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
}

} // namespace

/// What this device can do, which cannot change while it runs: asked once and kept.
bool capable() {
    if (!dev.device) return false;   // (asked before the device is made, by the menu: not an answer to keep)
    static const bool yes = []() {
        if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) {
            logf("vk: fsr3fi: no push descriptors");
            return false;
        }
        if (!formats_supported() || !features_supported()) return false;
        logf("vk: fsr3fi: AMD FSR 3.1 frame interpolation can run on this GPU");
        return true;
    }();
    return yes;
}

uint32_t frames_wanted() {
    // (the row first: a device is not asked anything, and nothing is logged, while frame generation is not at 2x)
    return asked() && capable() ? 1u : 0u;
}

bool prepare(uint32_t width, uint32_t height, uint32_t render_width, uint32_t render_height,
             VkFormat present_format) {
    if (!capable() || g.failed) return false;
    if (!width || !height || !render_width || !render_height) return false;
    if (present_format == VK_FORMAT_UNDEFINED) return false;
    // The frame this backend writes is in the format it is presented in, because it is shown as a
    // frame and not read back into the game's target, and the interpolation and inpainting passes
    // write it through a storage image. A presented format that cannot be one is the end of it.
    {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, present_format, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
            !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            logf("vk: fsr3fi: the presented format %d cannot be a storage and sampled image here",
                 (int)present_format);
            g.failed = true;
            return false;
        }
    }
    if (!g.tried) {
        g.tried = true;
        // (TRANSFER_DST: the counters are zeroed with vkCmdFillBuffer before the first frame)
        make_uniform_layout();
        g.ok = make_sampler() &&
               create_buffer(kCountersBytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             false, g.counters, g.counters_memory, nullptr);
        for (size_t i = 0; i < P_COUNT && g.ok; i++) g.ok = make_pipeline(g.passes[i], PASSES[i]);
        if (g.ok) logf("vk: fsr3fi: %d pass pipelines ready", (int)P_COUNT);
    }
    if (!g.ok) return false;
    const uint32_t n = kMaxMade;
    if (g.ready == n && g.width == width && g.height == height &&
        g.render_width == render_width && g.render_height == render_height &&
        g.present_format == present_format)
        return true;

    const uint32_t rw = render_width, rh = render_height, uw = width, uh = height;
    // The flow pyramid's levels are halved rounding up, which is not what a mip chain does: the SDK
    // allocates a texture a level for exactly this reason, and so does this. The luma pyramid's
    // levels are halved rounding down, which is a mip chain, so that one is a single mipped image.
    g.flow_width[0] = (uw + kOfBlockSize - 1) / kOfBlockSize;
    g.flow_height[0] = (uh + kOfBlockSize - 1) / kOfBlockSize;
    for (uint32_t l = 1; l < kOfLevels; l++) {
        g.flow_width[l] = (g.flow_width[l - 1] + 1) / 2;
        g.flow_height[l] = (g.flow_height[l - 1] + 1) / 2;
    }
    const uint32_t pyramid_w = std::max(1u, uw / 2), pyramid_h = std::max(1u, uh / 2);
    const uint32_t pyramid_levels = std::min(
        kPyramidMips,
        (uint32_t)std::floor(std::log2((float)std::max(pyramid_w, pyramid_h))) + 1u);

    // Seven luma levels is what the flow asks for, but a window smaller than 64 pixels has fewer
    // mips than that; `level()` clamps the index, so a short chain costs accuracy at the coarsest
    // levels rather than an invalid image.
    const uint32_t luma_levels = std::min(
        kOfLevels, (uint32_t)std::floor(std::log2((float)std::max(uw, uh))) + 1u);

    bool made = true;
    for (uint32_t i = 0; i < 2 && made; i++) {
        made = made && make_image(g.of_luma[i], VK_FORMAT_R8_UINT, uw, uh, luma_levels);
        for (uint32_t l = 0; l < kOfLevels && made; l++)
            made = made && make_image(g.of_flow[i][l], VK_FORMAT_R16G16_SINT, g.flow_width[l],
                                      g.flow_height[l]);
    }
    made = made && make_image(g.of_vector, VK_FORMAT_R16G16_SINT, g.flow_width[0], g.flow_height[0]);
    // The flow this backend records carries no per-block confidence, and neither does the SDK's: its
    // own dispatch leaves the binding null and the pass reads zero through it. A cleared image of the
    // flow's own size is the same answer with every fetch in bounds.
    made = made && make_image(g.of_confidence, VK_FORMAT_R32_UINT, g.flow_width[0], g.flow_height[0]);
    made = made && make_image(g.of_scd_histogram, VK_FORMAT_R32_UINT, kScdHistogramWidth, 1);
    made = made && make_image(g.of_scd_previous_histogram, VK_FORMAT_R32_SFLOAT, kScdHistogramWidth, 1);
    made = made && make_image(g.of_scd_temp, VK_FORMAT_R32_UINT, 3, 1);
    made = made && make_image(g.of_scd_output, VK_FORMAT_R32_UINT, 3, 1);

    made = made && make_image(g.fi_dilated_depth, VK_FORMAT_R32_SFLOAT, rw, rh);
    made = made && make_image(g.fi_dilated_motion_vectors, VK_FORMAT_R16G16_SFLOAT, rw, rh);
    made = made && make_image(g.fi_reconstructed_depth_previous, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.fi_reconstructed_depth_interpolated, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.fi_game_mv_x, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.fi_game_mv_y, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.fi_of_mv_x, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.fi_of_mv_y, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.fi_disocclusion_mask, VK_FORMAT_R8G8_UNORM, rw, rh);
    made = made && make_image(g.fi_inpainting_pyramid, VK_FORMAT_R16G16B16A16_SFLOAT, pyramid_w,
                              pyramid_h, pyramid_levels);
    made = made && make_image(g.fi_previous_source, present_format, uw, uh);
    made = made && make_image(g.fi_distortion_field, VK_FORMAT_R8G8_UNORM, 1, 1);
    for (uint32_t i = 0; i < n && made; i++)
        made = made && make_image(g.made[i], present_format, uw, uh);
    for (uint32_t i = n; i < kMaxMade; i++) drop_image(g.made[i]);
    if (!made) {
        g.failed = true;
        for (Image* t : all_images()) drop_image(*t);
        for (Image& t : g.made) drop_image(t);
        g.ready = 0;
        g.width = g.height = g.render_width = g.render_height = 0;
        return false;
    }
    g.width = uw, g.height = uh, g.render_width = rw, g.render_height = rh;
    g.present_format = present_format;
    g.ready = n;
    g.in_general = false;
    g.first_frame = true;
    g.parity = 0;
    g.of_frame_index = 0;
    logf("vk: fsr3fi: %ux%u from %ux%u inputs, flow %ux%u, pyramid %ux%u with %u levels, %u made "
         "frame(s) (format %d)",
         uw, uh, rw, rh, g.flow_width[0], g.flow_height[0], pyramid_w, pyramid_h, pyramid_levels, n,
         (int)present_format);
    return true;
}

bool generate(VkCommandBuffer cmd, const Inputs& in) {
    free_dead();
    if (!frames_wanted()) return false;
    if (!in.color || !in.color_image || !in.depth || !in.motion) return false;
    if (in.color_format == VK_FORMAT_UNDEFINED) return false;
    if (!in.width || !in.height || !in.render_width || !in.render_height) return false;
    if (!(in.camera.device_to_view_depth[1] > 0.0f)) return false;   // (no camera: vk_fsr3_inputs.cpp)
    if (!prepare(in.width, in.height, in.render_width, in.render_height, in.color_format))
        return false;

    Image& out = g.made[0];
    if (!out.image) return false;

    const UniformUpload uniforms = uniform_upload(g.frame_stride);
    if (!uniforms.binding) return false;

    // Everything this backend owns starts UNDEFINED and lives in GENERAL from here on, as the rest
    // of this renderer's compute resources do.
    if (!g.in_general) {
        g.in_general = true;
        std::vector<VkImageMemoryBarrier2> to_general;
        const std::vector<Image*> all = all_images();
        to_general.reserve(all.size());
        for (const Image* t : all) {
            if (!t->image) continue;
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
        for (const Image* t : all)
            if (t->image)
                vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
        vkCmdFillBuffer(cmd, g.counters, 0, kCountersBytes, 0);
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    const bool reset = in.reset || g.first_frame || in.sequence != g.sequence + 1;
    g.first_frame = false;
    g.sequence = in.sequence;   // (this frame becomes the next one's previous source below, whatever else happens)
    const int32_t rw = (int32_t)g.render_width, rh = (int32_t)g.render_height;
    const int32_t uw = (int32_t)g.width, uh = (int32_t)g.height;

    // ---- the constant blocks ----

    Constants k{};
    k.render_size[0] = rw, k.render_size[1] = rh;
    k.display_size[0] = uw, k.display_size[1] = uh;
    k.display_size_rcp[0] = 1.0f / (float)uw, k.display_size_rcp[1] = 1.0f / (float)uh;
    k.camera_near = in.camera.near_plane;   // (not read by the passes: set as the SDK sets them)
    k.camera_far = in.camera.far_plane;
    k.upscaler_target_size[0] = uw, k.upscaler_target_size[1] = uh;
    k.mode = 0;
    k.reset = reset ? 1 : 0;
    memcpy(k.device_to_view_depth, in.camera.device_to_view_depth, sizeof k.device_to_view_depth);
    k.delta_time = in.frame_time_ms;
    k.hudless_attached_factor = 0;   // (one colour, with whatever the game drew on it)
    k.distortion_field_size[0] = 1, k.distortion_field_size[1] = 1;
    // GetOpticalFlowSize() in the GLSL reads this back as (1 / scale) / block size, so the scale is
    // the reciprocal of the display size and nothing else; it is also what turns a flow vector in
    // luma pixels into the UV motion the vector field packs.
    k.optical_flow_scale[0] = 1.0f / (float)uw, k.optical_flow_scale[1] = 1.0f / (float)uh;
    k.optical_flow_block_size = (int32_t)kOfBlockSize;
    k.dispatch_flags = 0;            // (no debug view, no debug bar)
    k.max_render_size[0] = rw, k.max_render_size[1] = rh;
    k.optical_flow_half_res_mode = 0;
    k.num_instances = 1;
    k.interpolation_rect_base[0] = 0, k.interpolation_rect_base[1] = 0;
    k.interpolation_rect_size[0] = uw, k.interpolation_rect_size[1] = uh;
    // The colour this backend is handed is the window's, which this renderer presents as UNORM with
    // the game's own encoding on it, so the SDK's sRGB transfer function is the right one and the
    // luminance pair the other two functions would need is left at the SDR range.
    k.back_buffer_transfer_function = 0;
    k.min_max_luminance[0] = 0.0f, k.min_max_luminance[1] = 1.0f;
    // fTanHalfFOV is the *horizontal* half angle's tangent, as ffxFrameInterpolationDispatch builds it from the vertical
    // field of view and the render aspect: 1 / p[0][0] of the scene's projection (fsr3_math.h)
    k.tan_half_fov = in.camera.tan_half_fov_x;
    k.jitter[0] = in.jitter[0], k.jitter[1] = in.jitter[1];
    k.motion_vector_scale[0] = in.motion_scale[0] / (float)rw;
    k.motion_vector_scale[1] = in.motion_scale[1] / (float)rh;

    // The two inpainting pyramids downsample different rectangles -- the game vector field's is at
    // the render size and the frame's is at the display size -- so each gets its own block.
    uint32_t game_groups[2], game_offset[2], game_mips = 0;
    spd_setup((uint32_t)rw, (uint32_t)rh, game_groups, game_offset, &game_mips);
    PyramidConstants game_pyramid{};
    game_pyramid.mips = game_mips;
    game_pyramid.num_work_groups = game_groups[0] * game_groups[1];
    game_pyramid.work_group_offset[0] = game_offset[0];
    game_pyramid.work_group_offset[1] = game_offset[1];

    uint32_t frame_groups[2], frame_offset[2], frame_mips = 0;
    spd_setup((uint32_t)uw, (uint32_t)uh, frame_groups, frame_offset, &frame_mips);
    PyramidConstants frame_pyramid{};
    frame_pyramid.mips = frame_mips;
    frame_pyramid.num_work_groups = frame_groups[0] * frame_groups[1];
    frame_pyramid.work_group_offset[0] = frame_offset[0];
    frame_pyramid.work_group_offset[1] = frame_offset[1];

    g.of_frame_index = reset ? 0 : g.of_frame_index + 1;
    OfConstants of{};
    of.input_luma_resolution[0] = uw, of.input_luma_resolution[1] = uh;
    of.pyramid_level = 0;
    of.pyramid_level_count = kOfLevels;
    of.frame_index = g.of_frame_index;
    of.back_buffer_transfer_function = 0;
    of.min_max_luminance[0] = 0.0f, of.min_max_luminance[1] = 1.0f;

    uint32_t luma_groups[2], luma_offset[2], luma_mips = 0;
    // The luma pyramid's own shader hard-codes six mips, so the four the SDK asks ffxSpdSetup for
    // here are never read; the work group count is, and this is where it comes from.
    spd_setup((uint32_t)uw, (uint32_t)uh, luma_groups, luma_offset, &luma_mips, 4);
    OfSpdConstants of_spd{};
    of_spd.mips = luma_mips;
    of_spd.num_work_groups = luma_groups[0] * luma_groups[1];
    of_spd.work_group_offset[0] = luma_offset[0];
    of_spd.work_group_offset[1] = luma_offset[1];
    of_spd.num_work_groups_pyramid = of_spd.num_work_groups;

    // This evaluation owns its constants until its recording has completed.
    const VkDeviceSize base = uniforms.binding.offset;
    uint8_t* const mapped = uniforms.mapped;
    const VkDeviceSize at_fi = 0;
    const VkDeviceSize at_game_pyramid = at_fi + g.slot_fi;
    const VkDeviceSize at_frame_pyramid = at_game_pyramid + g.slot_pyramid;
    const VkDeviceSize at_of = at_frame_pyramid + g.slot_pyramid;
    const VkDeviceSize at_of_spd = at_of + (kOfLevels + 1) * g.slot_of;
    memcpy(mapped + at_fi, &k, sizeof k);
    memcpy(mapped + at_game_pyramid, &game_pyramid, sizeof game_pyramid);
    memcpy(mapped + at_frame_pyramid, &frame_pyramid, sizeof frame_pyramid);
    memcpy(mapped + at_of, &of, sizeof of);   // (slot 0: the passes before the flow's own pyramid)
    for (uint32_t l = 0; l < kOfLevels; l++) {
        OfConstants level = of;
        level.pyramid_level = l;
        memcpy(mapped + at_of + (VkDeviceSize)(l + 1) * g.slot_of, &level, sizeof level);
    }
    memcpy(mapped + at_of_spd, &of_spd, sizeof of_spd);
    flush_mapped_range(uniforms.memory, uniforms.binding.offset, uniforms.binding.size);

    const Resource cb_fi{VK_NULL_HANDLE, uniforms.binding.buffer, base + at_fi, g.slot_fi};
    const Resource cb_game_pyramid{VK_NULL_HANDLE, uniforms.binding.buffer, base + at_game_pyramid,
                                   g.slot_pyramid};
    const Resource cb_frame_pyramid{VK_NULL_HANDLE, uniforms.binding.buffer, base + at_frame_pyramid,
                                    g.slot_pyramid};
    const Resource cb_of_spd{VK_NULL_HANDLE, uniforms.binding.buffer, base + at_of_spd, g.slot_of_spd};
    const auto cb_of = [&](uint32_t slot) {
        return Resource{VK_NULL_HANDLE, uniforms.binding.buffer, base + at_of + (VkDeviceSize)slot * g.slot_of,
                        g.slot_of};
    };
    const Resource counters{VK_NULL_HANDLE, g.counters, 0, kCountersBytes};

    // ---- the clears the SDK's dispatch records ----

    {   // (what the game's draws and the present path wrote has to be visible to the first pass)
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    // The nearest-depth scratch is cleared every frame, to zero because the depth is reversed and
    // the SDK clears it to the far value, which under a reversed depth is zero.
    clear_images(cmd, {&g.fi_reconstructed_depth_previous});
    if (!reset) clear_images(cmd, {&g.fi_reconstructed_depth_interpolated});
    if (reset) {
        // A cut, a load or a resize: every history the flow and the scene-change detector carry
        // belongs to a scene that is gone, and the SDK clears exactly this set.
        clear_images(cmd, {&g.of_scd_temp, &g.of_scd_output, &g.of_scd_histogram,
                           &g.of_scd_previous_histogram, &g.of_luma[0], &g.of_luma[1]});
    }
    {
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    // ---- the optical flow, over this frame's luma and the frame before it ----

    const auto tex = [](const Image& t) { return Resource{t.view}; };
    const auto level = [](const Image& t, uint32_t l) {
        const size_t n = t.levels.size();
        return Resource{n ? t.levels[std::min<size_t>(l, n - 1)] : t.view};
    };
    const auto groups = [](uint32_t size, uint32_t tile) { return (std::max(1u, size) + tile - 1) / tile; };
    const uint32_t p = g.parity, q = p ^ 1u;
    const Resource color{in.color};

    {   // Two pixels across and two down a thread, which is where the double rounding comes from.
        const Resource res[]{color, level(g.of_luma[p], 0), cb_of(0)};
        record_pass(cmd, P_OF_PREPARE_LUMA, res, 3, groups(groups((uint32_t)uw, 2), 16),
                    groups(groups((uint32_t)uh, 2), 16));
    }
    {
        const Resource res[]{level(g.of_luma[p], 0), level(g.of_luma[p], 1),
                             level(g.of_luma[p], 2), level(g.of_luma[p], 3),
                             level(g.of_luma[p], 4), level(g.of_luma[p], 5),
                             level(g.of_luma[p], 6), cb_of(0), cb_of_spd};
        record_pass(cmd, P_OF_LUMINANCE_PYRAMID, res, 9, luma_groups[0], luma_groups[1]);
    }
    {   // Scene change detection: a 3x3 grid of strata, each with its own 256-bin histogram, then
        // one dispatch that compares this frame's histograms with the last frame's.
        const uint32_t strata_width = (uint32_t)uw / 4 / 3;
        const Resource res[]{tex(g.of_luma[p]), tex(g.of_scd_histogram), cb_of(0)};
        record_pass(cmd, P_OF_SCD_HISTOGRAM, res, 3, groups(strata_width, 32), 16, 3 * 3);
    }
    {
        const Resource res[]{tex(g.of_scd_histogram), tex(g.of_scd_previous_histogram),
                             tex(g.of_scd_temp), tex(g.of_scd_output), cb_of(0)};
        record_pass(cmd, P_OF_SCD_DIVERGENCE, res, 5, 3 * 3, 3);
    }
    // The search walks the pyramid from the coarsest level down, and the two flow images swap roles
    // at every level as well as at every frame: the search writes A, the filter reads A and writes B,
    // and the scale reads B and seeds the next level down. The index arithmetic is the SDK's
    // (isOddFrame != isOddLevel), written as one exclusive or.
    for (int32_t l = (int32_t)kOfLevels - 1; l >= 0; l--) {
        const uint32_t a = ((p ^ (uint32_t)l) & 1u), b = a ^ 1u;
        const uint32_t luma_w = std::max(1u, (uint32_t)uw >> l), luma_h = std::max(1u, (uint32_t)uh >> l);
        {
            const Resource res[]{level(g.of_luma[p], (uint32_t)l), level(g.of_luma[q], (uint32_t)l),
                                 tex(g.of_flow[a][l]), tex(g.of_scd_output),
                                 cb_of((uint32_t)l + 1)};
            // 64 threads a group, each covering four luma pixels across and sixteen rows down.
            record_pass(cmd, P_OF_SEARCH, res, 5, (groups(luma_w, 4) * 16 + 63) / 64,
                        groups(luma_h, 16));
        }
        {
            // At the bottom the filter writes the flow the interpolation reads, which is the one
            // resource the two halves of this backend share.
            const Resource out_flow = l == 0 ? tex(g.of_vector) : tex(g.of_flow[b][l]);
            const Resource res[]{tex(g.of_flow[a][l]), out_flow, cb_of((uint32_t)l + 1)};
            record_pass(cmd, P_OF_FILTER, res, 3, groups(g.flow_width[l], 16),
                        groups(g.flow_height[l], 4));
        }
        if (l > 0) {
            const Resource res[]{level(g.of_luma[p], (uint32_t)l), level(g.of_luma[q], (uint32_t)l),
                                 tex(g.of_flow[b][l]), tex(g.of_flow[b][l - 1]),
                                 tex(g.of_scd_output), cb_of((uint32_t)l + 1)};
            record_pass(cmd, P_OF_SCALE, res, 6, groups(g.flow_width[l - 1], 4),
                        groups(g.flow_height[l - 1], 4));
        }
    }

    // ---- frame interpolation ----

    const uint32_t render_x = groups((uint32_t)rw, 8), render_y = groups((uint32_t)rh, 8);
    const uint32_t display_x = groups((uint32_t)uw, 8), display_y = groups((uint32_t)uh, 8);
    // The flow's own grid, eight blocks a group. The SDK divides the display size by the block size
    // in floating point and truncates, which loses the last partial block when the display size is
    // not a multiple of eight; this rounds that block up instead, so the vector field is written
    // over the whole flow image.
    const uint32_t flow_x = groups(g.flow_width[0], 8), flow_y = groups(g.flow_height[0], 8);
    const auto pyramid = [](uint32_t l) {
        const size_t n = g.fi_inpainting_pyramid.levels.size();
        return Resource{n ? g.fi_inpainting_pyramid.levels[std::min<size_t>(l, n - 1)]
                          : g.fi_inpainting_pyramid.view};
    };

    {   // The SDK records this one from its own prepare entry point, alongside the upscaler; here
        // there is no upscaler to ride along with, so it opens the interpolation instead. It is what
        // fills the three resources the SDK calls shared.
        const Resource res[]{Resource{in.motion}, Resource{in.depth},
                             tex(g.fi_reconstructed_depth_previous),
                             tex(g.fi_dilated_motion_vectors), tex(g.fi_dilated_depth), cb_fi};
        record_pass(cmd, P_FI_RECONSTRUCT_AND_DILATE, res, 6, render_x, render_y);
    }
    {   // Zeroes both vector fields and the disocclusion mask, and moves the frame counter the
        // interpolation reads along -- which is why it runs even on a reset.
        const Resource res[]{tex(g.of_scd_output),   tex(g.fi_game_mv_x), tex(g.fi_game_mv_y),
                             tex(g.fi_of_mv_x),      tex(g.fi_of_mv_y),   tex(g.fi_disocclusion_mask),
                             counters,               cb_fi};
        record_pass(cmd, P_FI_SETUP, res, 8, render_x, render_y);
    }
    if (!reset) {
        {
            const Resource res[]{tex(g.fi_dilated_motion_vectors), tex(g.fi_dilated_depth), color,
                                 tex(g.fi_distortion_field),
                                 tex(g.fi_reconstructed_depth_interpolated), cb_fi};
            record_pass(cmd, P_FI_RECONSTRUCT_PREVIOUS_DEPTH, res, 6, render_x, render_y);
        }
        {
            const Resource res[]{tex(g.fi_dilated_motion_vectors), tex(g.fi_dilated_depth),
                                 tex(g.fi_previous_source),        color,
                                 tex(g.fi_distortion_field),       tex(g.fi_game_mv_x),
                                 tex(g.fi_game_mv_y),              cb_fi};
            record_pass(cmd, P_FI_GAME_MOTION_VECTOR_FIELD, res, 8, render_x, render_y);
        }
        {
            const Resource res[]{tex(g.fi_game_mv_x), tex(g.fi_game_mv_y), counters,
                                 pyramid(0),  pyramid(1),  pyramid(2),  pyramid(3),  pyramid(4),
                                 pyramid(5),  pyramid(6),  pyramid(7),  pyramid(8),  pyramid(9),
                                 pyramid(10), pyramid(11), pyramid(12), cb_fi, cb_game_pyramid};
            record_pass(cmd, P_FI_GAME_VECTOR_FIELD_PYRAMID, res, 18, game_groups[0],
                        game_groups[1]);
        }
        {
            const Resource res[]{tex(g.of_vector),           tex(g.of_confidence),
                                 tex(g.fi_dilated_depth),    tex(g.fi_previous_source),
                                 color,                      tex(g.fi_of_mv_x),
                                 tex(g.fi_of_mv_y),          cb_fi};
            record_pass(cmd, P_FI_OPTICAL_FLOW_VECTOR_FIELD, res, 8, flow_x, flow_y);
        }
        {
            const Resource res[]{tex(g.fi_game_mv_x),
                                 tex(g.fi_game_mv_y),
                                 tex(g.fi_reconstructed_depth_previous),
                                 tex(g.fi_dilated_depth),
                                 tex(g.fi_reconstructed_depth_interpolated),
                                 tex(g.fi_inpainting_pyramid),
                                 tex(g.fi_distortion_field),
                                 tex(g.fi_disocclusion_mask),
                                 cb_fi};
            record_pass(cmd, P_FI_DISOCCLUSION_MASK, res, 9, render_x, render_y);
        }
    }
    {   // The frame itself. Where it could not decide a colour it leaves the alpha low, and the two
        // passes after it fill those pixels in from the pyramid they build over this very output.
        const Resource res[]{tex(g.fi_game_mv_x),         tex(g.fi_game_mv_y),
                             tex(g.fi_of_mv_x),           tex(g.fi_of_mv_y),
                             tex(g.fi_previous_source),   color,
                             tex(g.fi_disocclusion_mask), tex(g.fi_inpainting_pyramid),
                             counters,                    tex(out),
                             cb_fi};
        record_pass(cmd, P_FI_INTERPOLATE, res, 11, display_x, display_y);
    }
    {
        const Resource res[]{tex(out), counters,
                             pyramid(0),  pyramid(1),  pyramid(2),  pyramid(3),  pyramid(4),
                             pyramid(5),  pyramid(6),  pyramid(7),  pyramid(8),  pyramid(9),
                             pyramid(10), pyramid(11), pyramid(12), cb_fi, cb_frame_pyramid};
        record_pass(cmd, P_FI_INPAINTING_PYRAMID, res, 17, frame_groups[0], frame_groups[1]);
    }
    {   // The present backbuffer and the interpolation source are the same image here: this game has
        // no separate HUD-less colour, so there is nothing to take the HUD from.
        const Resource res[]{tex(g.of_scd_output), tex(g.fi_inpainting_pyramid), color, color,
                             tex(out), cb_fi};
        record_pass(cmd, P_FI_INPAINTING, res, 6, display_x, display_y);
    }

    // This frame becomes the next one's "previous", and the luma pyramids swap, so the next flow
    // search has the pair it needs. Both happen once a present, not once a made frame.
    {
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                               {g.width, g.height, 1}};
        vkCmdCopyImage(cmd, in.color_image, VK_IMAGE_LAYOUT_GENERAL, g.fi_previous_source.image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    g.parity = q;
    // A reset frame has no history to interpolate from: the preparation passes were skipped, so what
    // the interpolation wrote is the current frame over a previous frame that is not there yet.
    // Everything above still ran, which is what gives the next frame its luma pair and its previous
    // source; this one is refused, and the caller's own colour interpolation takes it.
    return !reset;
}

VkImage made_image() { return g.ready ? g.made[0].image : VK_NULL_HANDLE; }

void reset() { g.first_frame = true; }

} // namespace mkwvk::fsr3fi
