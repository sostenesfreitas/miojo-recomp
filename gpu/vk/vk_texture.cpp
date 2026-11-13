// Textures: one VkImage per texture storage (vk_texture.h). After totk-pc-recomp's runtime/vk_texture.c (MIT,
// Copyright (c) 2026 the totk-pc-recomp authors): the cache keyed by storage address plus descriptor, uploads on first
// use and when page write generations show the storage written, the GPU-owned rule (an image the GPU wrote is the
// content), every image in GENERAL layout, deferred destruction. The GPU ASTC decode is Eden's
// (video_core/host_shaders/astc_decoder.comp and renderer_vulkan/vk_compute_pass.cpp ASTCDecoderPass, GPL), the CPU
// unswizzle suyu's (video_core/textures/decoders.cpp, with the Switch 2 GOB swizzle).
//
// Formats share images by class: an image is made in the format first asked for (sRGB as its UNORM twin), with
// MUTABLE_FORMAT, so that a texture rendered as RGBA8 and sampled as RGBA8_SRGB, or written as R32UI and read as
// RGBA8, is one image. A class is the bytes per block for uncompressed colour, the format family for BC and ASTC,
// the format itself for depth / stencil.
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#if defined(__linux__)
#include <malloc.h>
#endif
#include <atomic>
#include <new>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "vk_texture.h"
#include "vk_menu.h"
#include "vk_draw.h"
#include "vk_programs.h"
#include "vk_gprof.h"
#include "vk_dump.h"
#include "vk_jobs.h"

#include <span>

#include "video_core/textures/astc.h"
#include "video_core/textures/decoders.h"

#include "host_shaders/astc_decoder_comp_spv.h"
#include "astc_opaque.h"
#include "host_shaders/bc_encoder_comp_spv.h"
#include "host_shaders/bc7_encoder_comp_spv.h"
#include "bc_levels.h"
#include "astc_bc_policy.h"
#include "drawn_writeback.h"
#include "vram_compact.h"
#include "evict_reason.h"
#include "keep_rendered.h"
#include "target_overlap.h"
#include "host_shaders/block_linear_unswizzle_2d_comp_spv.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

// ---- formats ----
std::mutex fmt_mu;
std::unordered_map<int, VkFormatProperties> fmt_cache;
VkFormatProperties format_props(VkFormat f) {
    std::lock_guard lk(fmt_mu);
    auto it = fmt_cache.find((int)f);
    if (it != fmt_cache.end()) return it->second;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, f, &p);
    fmt_cache[(int)f] = p;
    return p;
}
bool is_bc(VkFormat f) {
    return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
}
bool is_astc(VkFormat f) {
    return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}
/// The UNORM twin of an sRGB format (the image's format: storage writes and every view format of the class)
VkFormat linear_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    default:
        if (is_astc(f) && ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1)) return (VkFormat)(f - 1);
        return f;
    }
}
/// 3-component formats are not image formats on NVIDIA: the image holds 4 (alpha one), the upload widens them
VkFormat widened(VkFormat f, int* from_bpb) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R32G32B32_SINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SINT;
    default: *from_bpb = 0; return f;
    }
}
/// A storage-capable view format of the image's class (sRGB views cannot be storage)
VkFormat storage_view_format(VkFormat f) {
    f = linear_twin(f);
    if (format_props(f).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) return f;
    switch (f) {   // (same class, a format storage can be)
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R32_UINT;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SNORM: return VK_FORMAT_R16_UINT;
    case VK_FORMAT_R8_SNORM: return VK_FORMAT_R8_UINT;
    // (compressed: one block a texel, written raw by the game's encoders)
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_R32G32B32A32_UINT;
    default: return f;
    }
}
}  // namespace
/// (hookshot) The format of the image made for a texture of this Vulkan format (vk_format's, widened): depth as it is, decoded ASTC as
/// RGBA8, the rest as their UNORM twin. create's choice, and the pipeline state list's (attachment_format): one rule for both
VkFormat image_format_of(VkFormat vf, bool astc_decoded) {
    return is_depth_format(vf) ? vf : astc_decoded ? VK_FORMAT_R8G8B8A8_UNORM : linear_twin(vf);
}
/// (hookshot) The format a view of an image takes for a use: the view's NVN format as this device has it (widened), the image's own
/// for depth and for a format Vulkan lacks, decoded ASTC as RGBA8, BC targets as their raw block-texel format, ASTC kept as BC3
/// as BC3 (sampled only); UNDEFINED: no such view (ASTC is no target). texture_view's choice, and the pipeline state list's: one rule for both
VkFormat view_format_of(VkFormat image, bool astc_decoded, bool bc, int view_nvn, ViewUse use, VkFormat force_format) {
    int from = 0;
    VkFormat f = widened(vk_format(view_nvn).format, &from);
    if (is_depth_format(image) || f == VK_FORMAT_UNDEFINED) f = image;
    if (astc_decoded) f = (view_nvn >= 0x87 && view_nvn <= 0x94) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (bc) {
        if (use != ViewUse::Sample) return VK_FORMAT_UNDEFINED;
        const bool srgb = f == VK_FORMAT_R8G8B8A8_SRGB;
        if (linear_twin(image) == VK_FORMAT_BC7_UNORM_BLOCK)
            f = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        else if (image == VK_FORMAT_BC1_RGBA_UNORM_BLOCK)   // (hookshot: an opaque texture's image is BC1, astc_opaque.h)
            f = srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        else
            f = srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    }
    if (use == ViewUse::Storage) f = force_format != VK_FORMAT_UNDEFINED ? force_format : storage_view_format(f);
    if (use == ViewUse::Attachment && is_bc(f)) f = storage_view_format(f);
    if (use == ViewUse::Attachment && is_astc(f)) return VK_FORMAT_UNDEFINED;
    return f;
}
/// (hookshot) The format a draw renders in, on this device, into a texture of NVN format tex_nvn through a view of NVN format view_nvn
/// (a depth target: its own). What texture_get and texture_view make of them, without the texture: the pipeline state list's
/// targets are the game's NVN formats, and each GPU builds the pipelines its draws will ask for (vk_draw.cpp state_targets)
VkFormat attachment_format(int tex_nvn, int view_nvn) {
    const VkFormatInfo fi = vk_format(tex_nvn);
    if (fi.format == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;
    int from = 0;
    return view_format_of(image_format_of(widened(fi.format, &from), fi.astc), fi.astc, false, view_nvn, ViewUse::Attachment,
                          VK_FORMAT_UNDEFINED);
}
namespace {
/// The class an image format shares images by (see the top of the file)
uint32_t format_class(VkFormat vf, int nvn_format, int bpb, bool astc_decoded) {
    if (astc_decoded) return 0x30000u | (uint32_t)(nvn_format - 0x79) % 14;
    if (is_depth_format(vf)) return 0x10000u | (uint32_t)vf;
    if (is_bc(vf) || is_astc(vf)) return 0x20000u | (uint32_t)linear_twin(vf);
    int from = 0;
    widened(vf, &from);
    // (RGB10A2 a class of its own (#2038, moosky73): the game's window textures are RGBA8 until it is told its display is HDR10,
    // then RGB10A2 at the same addresses. Shared by class, a window texture first made as RGBA8 stayed RGBA8, the native-HDR present
    // (vk_replay.cpp: the image's format) took it for an SDR picture, and one frame in three was grey)
    return 0x40000u | (uint32_t)bpb | (from ? 0x100u : 0u) | (vf == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 0x200u : 0u);
}

VkComponentSwizzle component(unsigned s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

// ---- the cache ----
struct Key {
    uint64_t gpu;
    uint32_t target, width, height, depth, levels, samples, layout, cls;
    bool operator==(const Key& o) const {
        return gpu == o.gpu && target == o.target && width == o.width && height == o.height && depth == o.depth &&
               levels == o.levels && samples == o.samples && layout == o.layout && cls == o.cls;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = mix(k.gpu, k.target);
        h = mix(h, (uint64_t)k.width << 32 | k.height);
        h = mix(h, (uint64_t)k.depth << 32 | k.levels);
        h = mix(h, (uint64_t)k.samples << 40 | (uint64_t)k.layout << 20 | k.cls);
        return (size_t)h;
    }
};
std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> textures;
std::unordered_multimap<uint64_t, Texture*> by_gpu;
uint64_t frame_no = 1;
uint64_t total_bytes = 0;

// (RX_VK_LOG_REMADE=1) Textures evicted and made again within 600 frames, and those of them that were then sampled
// empty (storage the GPU rendered, not uploaded): what the eviction's minimum age below is measured by
struct Evicted {
    uint64_t frame;
    bool rendered;
};
std::mutex evicted_mu;
std::unordered_map<uint64_t, Evicted> evicted_at;   // (storage, size and format -> when it went)
std::unordered_map<const Texture*, uint64_t> remade;   // (made again: how many frames after it went)
uint64_t evicted_key(uint64_t gpu, uint32_t w, uint32_t h, uint32_t fmt) {
    return gpu ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 32) ^ ((uint64_t)fmt << 20);
}
bool remade_log_on() {
    static const bool on = getenv("RX_VK_LOG_REMADE") && atoi(getenv("RX_VK_LOG_REMADE")) != 0;
    return on;
}
// (hookshot) Textures uploaded from the game's memory (never drawn by the GPU) that the budget evicted: storage, size and format.
// Made again, such a texture is uploaded from that memory whatever ranges render targets once took over it (upload's "storage
// the GPU rendered" skip below). Skipped, its new image was never filled: whatever the VRAM held before, random-colour blocks on
// characters, shop shelves, floors, which spread over play time on 4-8 GB cards (the ones that evict) and changed each time a
// menu was opened and closed (issue 993). RX_VK_REMADE_SKIP=1: as before
std::mutex from_memory_mu;
std::unordered_map<uint64_t, uint64_t> evicted_from_memory;   // (evicted_key -> guest size)
std::atomic<uint64_t> st_remade_uploaded{0};
bool remade_skip_old() {
    static const bool on = getenv("RX_VK_REMADE_SKIP") && atoi(getenv("RX_VK_REMADE_SKIP")) != 0;
    return on;
}

struct Dead {
    uint64_t value;
    VkImage image;
    VmaAllocation allocation;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> memory;   // (a sparse image's)
    VkBuffer buffer = VK_NULL_HANDLE;     // (the BC3 scratch's output buffer: given back with its image)
    VmaAllocation buffer_alloc = nullptr;
    uint64_t pool_key = 0;                // (a render target's image: to the pool below, with its views, not destroyed)
    std::unordered_map<uint64_t, VkImageView> view_map;
    uint64_t bytes = 0;
};
std::deque<Dead> dead;

// ---- (hookshot) Render target images, reused. OoT makes a frame's transient targets in a scratch heap whose offsets shift with
// what came before them in the frame (the dead targets, texture_frame): each shifted one was a texture of its own with a new image,
// and every 60 presents the sweep let go of the dead ones, 300-700 at once, destroyed in the next present: 20-50 ms on the thread
// replaying, once a second while the camera moves (RTX 5070, Kokiri Forest: 10,500 images made and destroyed in 4.5 minutes,
// 17 GB). Now a render target's image whose texture is gone waits in a pool by its create info, views and all, and a new texture
// of the same create info takes it; it is put into GENERAL from UNDEFINED and cleared as a new one is (create), so nothing it held
// shows. An image the GPU is done with is pooled as it is evicted, not at the next present: the next frame, recorded between
// the two, is where a target of its shape would have made a new image instead (issue 2180). Only images made for a target
// (texture_get's !for_sampling) are pooled. The pool is bounded (RX_VK_TARGET_POOL_MB, default
// 256; 0: none, every image destroyed as before; and none while VRAM is near what the driver allows), and an image unused in it for
// 120 presents is destroyed, at most 16 a present ----
struct Pooled {
    VkImage image;
    VmaAllocation allocation;
    std::unordered_map<uint64_t, VkImageView> views;
    uint64_t bytes, since;
};
std::mutex pool_mu;
std::unordered_map<uint64_t, std::vector<Pooled>> target_pool;   // by pool_key_of
uint64_t pool_bytes, pool_n, st_pool_reused, st_pool_made, st_pool_destroyed;
bool pool_pressed;   // (VRAM near what the driver allows: texture_frame's budget)
bool compact_on() {   // (hookshot: the textures in a pool of their own, compacted: compact_step. On unless RX_VK_COMPACT=0)
    static const bool v = vram_compact::enabled(getenv("RX_VK_COMPACT"));
    return v;
}
// (hookshot) the commit the textures leave free for the rest of the system: texture_frame's commit rule. RX_VK_COMMIT_RESERVE_MB
// (default 2560, 0: off). Windows only: Linux overcommits, and its limit is not one commit can reach
static uint64_t commit_reserve() {
    static const uint64_t v = getenv("RX_VK_COMMIT_RESERVE_MB") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_COMMIT_RESERVE_MB"))) << 20
                                                                : 2560ull << 20;
    return v;
}
// (what Windows lets the process commit before its limit, RAM plus the page file: GlobalMemoryStatusEx's ullAvailPageFile, the
// limit less the whole system's commit. false where it cannot be read)
static bool commit_headroom(uint64_t* free_bytes) {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return false;
    *free_bytes = m.ullAvailPageFile;
    return true;
#else
    (void)free_bytes;
    return false;
#endif
}
// (hookshot) and a reserve of commit for the renderer's own heap: at Windows' commit limit an allocation failed on the presenting
// thread (#2941: std::bad_alloc in the page table's flush as Hyrule Field loaded, 32 GB of RAM with a 2 GB page file) and the
// exception left the renderer for the game's code, which nothing catches: the game closed. The commit rule above shrinks the
// textures only on its next pass, and a wait would not help where the presenting thread itself is short. So 128 MB is held
// committed while there is room; the first allocation that fails gives it back (operator new's handler) and is tried again; the
// commit rule then sees the headroom gone and the textures make room, and the reserve is held again once the headroom is back
// above the rule's reserve. Windows only (Linux overcommits); RX_VK_COMMIT_RESERVE_MB=0 turns it off with the rule
namespace {
constexpr size_t kOomReserve = 128u << 20;
std::atomic<void*> oom_block{nullptr};
std::atomic<int> oom_given{0};   // (times given back, for the log)
void oom_handler() {
#if defined(_WIN32)
    if (void* p = oom_block.exchange(nullptr)) {
        VirtualFree(p, 0, MEM_RELEASE);
        oom_given.fetch_add(1);
        return;   // (operator new tries again)
    }
#endif
    throw std::bad_alloc();
}
}   // namespace
static void oom_reserve_tend(bool known, uint64_t commit_free, uint64_t reserve) {
#if defined(_WIN32)
    static bool installed;
    static int said;
    if (!reserve) return;
    if (!installed) installed = true, std::set_new_handler(oom_handler);
    if (const int n = oom_given.load(); n != said) {
        said = n;
        logf("vk: commit limit reached: an allocation took the renderer's 128 MB reserve (%d so far); the textures make room", n);
    }
    if (!oom_block.load() && known && commit_free > reserve + kOomReserve)
        if (void* p = VirtualAlloc(nullptr, kOomReserve, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS)) {
            void* none = nullptr;
            if (!oom_block.compare_exchange_strong(none, p)) VirtualFree(p, 0, MEM_RELEASE);
        }
#else
    (void)known, (void)commit_free, (void)reserve;
#endif
}
uint64_t pool_cap() {
    static const uint64_t mb = getenv("RX_VK_TARGET_POOL_MB") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_TARGET_POOL_MB"))) : 256;
    return mb << 20;
}
uint64_t pool_key_of(const VkImageCreateInfo& ci) {
    uint64_t k = mix((uint64_t)ci.flags, (uint64_t)ci.imageType << 32 | (uint64_t)ci.format);
    k = mix(k, (uint64_t)ci.extent.width << 32 | ci.extent.height);
    k = mix(k, (uint64_t)ci.extent.depth << 32 | ci.mipLevels);
    k = mix(k, (uint64_t)ci.arrayLayers << 32 | (uint64_t)ci.samples);
    k = mix(k, (uint64_t)ci.usage << 8 | (uint64_t)ci.tiling);
    return k | 1;   // (0: not pooled)
}
bool pool_take(uint64_t key, VkImage& image, VmaAllocation& allocation, std::unordered_map<uint64_t, VkImageView>& views) {
    std::lock_guard lk(pool_mu);
    auto it = target_pool.find(key);
    if (it == target_pool.end() || it->second.empty()) return false;
    Pooled& p = it->second.back();
    image = p.image, allocation = p.allocation, views = std::move(p.views);
    pool_bytes -= std::min(pool_bytes, p.bytes), pool_n--;
    it->second.pop_back();
    if (it->second.empty()) target_pool.erase(it);
    st_pool_reused++;
    return true;
}
void pool_destroy(Pooled& p) {
    for (auto& kv : p.views)
        if (kv.second) vkDestroyImageView(dev.device, kv.second, nullptr);
    destroy_image(p.image, p.allocation);
    st_pool_destroyed++;
}
/// (texture_frame) a dead render target's image the GPU is done with, into the pool
void pool_put(Dead& d) {
    std::lock_guard lk(pool_mu);
    target_pool[d.pool_key].push_back(Pooled{d.image, d.allocation, std::move(d.view_map), d.bytes, frame_no});
    pool_bytes += d.bytes, pool_n++;
}
/// (texture_frame, each present) the pool over its bound, or all of it while VRAM is short, and what waits in it unused for 120
/// presents: destroyed, at most 16 a present (64 while VRAM is short). Returns how many
size_t pool_trim() {
    std::lock_guard lk(pool_mu);
    const uint64_t cap = pool_pressed ? 0 : pool_cap();
    const size_t most = pool_pressed ? 64 : 16;
    size_t n = 0;
    for (auto it = target_pool.begin(); it != target_pool.end() && n < most;) {
        auto& v = it->second;
        for (size_t i = 0; i < v.size() && n < most;) {   // (the oldest of a shape first: the front)
            if (pool_bytes <= cap && frame_no - v[i].since <= 120) {
                ++i;
                continue;
            }
            pool_destroy(v[i]);
            pool_bytes -= std::min(pool_bytes, v[i].bytes), pool_n--, n++;
            v.erase(v.begin() + (ptrdiff_t)i);
        }
        it = v.empty() ? target_pool.erase(it) : std::next(it);
    }
    return n;
}

// ---- the recording's upload command buffer ----
thread_local VkCommandBuffer up_cmd = VK_NULL_HANDLE;   // (each recording thread's: a parallel replay's segments
uint64_t rec_value = 0;                                   // record their own uploads)
thread_local bool up_used = false;
thread_local uint64_t tl_seg = 0;
uint32_t epoch = 0;
int64_t st_uploads, st_astc, st_bytes;

VkCommandBuffer upload_cmd() {
    if (!up_used) {
        up_used = true;
        full_barrier(up_cmd);   // (after the work submitted before: it may still read what is replaced)
    }
    return up_cmd;
}

// ---- (parallel replay) a new image's first layout change, in queue order ----
// The segments' workers record at once, and the one that makes an image first need not be the first in the queue to use
// it: a later segment's worker made it (its UNDEFINED -> GENERAL change in its own uploads, submitted after the earlier
// segment), the earlier one used it (a copy into it again, the BC3 encoder's dispatch: its uploads, at the start of its
// command buffers), and the queue reached that use with the image UNDEFINED (VUID-vkCmdDraw-None-09600, RX_VK_PARALLEL
// only). The change goes to the init command buffer of the earliest segment still being recorded instead: those before
// it were done before the image was made, so never used it, and each segment's init is submitted before its uploads.
struct SegInit {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool used = false, open = false;
};
std::mutex seg_init_mu;
std::vector<SegInit> seg_inits;    // (the submission's segments, in queue order; seg_init_mu)
thread_local int tl_seg_index = -1;   // (the segment this thread records; -1: none)
/// While it lives, upload_cmd() is the earliest open segment's init command buffer (a segment's worker only)
struct InitRedirect {
    std::unique_lock<std::mutex> lk;
    SegInit* to = nullptr;
    VkCommandBuffer saved_cmd = VK_NULL_HANDLE;
    bool saved_used = false;
    explicit InitRedirect(bool want = true) {
        if (!want || tl_seg_index < 0) return;
        lk = std::unique_lock<std::mutex>(seg_init_mu);
        for (int i = 0; i <= tl_seg_index && i < (int)seg_inits.size(); i++)
            if (seg_inits[i].open) { to = &seg_inits[i]; break; }
        if (!to) { lk.unlock(); return; }   // (none open: its own is, while it records)
        saved_cmd = up_cmd, saved_used = up_used;
        up_cmd = to->cmd, up_used = to->used;
    }
    void done() {
        if (to) {
            to->used = up_used;
            up_cmd = saved_cmd, up_used = saved_used;
            to = nullptr;
        }
        if (lk.owns_lock()) lk.unlock();
    }
    ~InitRedirect() { done(); }
    InitRedirect(const InitRedirect&) = delete;
    InitRedirect& operator=(const InitRedirect&) = delete;
};

// ---- staging: host-visible arenas, reused once the submission that filled them completed ----
struct Arena {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t size = 0, used = 0, value = 0;   // value: the last submission that reads it
    std::chrono::steady_clock::time_point used_at;   // (when it last staged something)
};
std::vector<Arena> arenas;
constexpr uint64_t kArena = 64ull << 20;

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* data = nullptr;
    VmaAllocation allocation = nullptr;
};
/// The staging arenas kept when idle (texture_frame): 64 MB on a card of 6.5 GB or less, 256 MB above.
/// RX_VK_STAGING_KEEP_MB=<n>: that many
uint64_t staging_keep() {
    static const uint64_t keep = []() -> uint64_t {
        if (const char* e = getenv("RX_VK_STAGING_KEEP_MB")) return (uint64_t)atoi(e) << 20;
        uint64_t used = 0, allowed = 0;
        return (vram_usage(&used, &allowed) && allowed <= (6ull << 30) + (512ull << 20) ? 64ull : 256ull) << 20;
    }();
    return keep;
}

/// An arena of size bytes, made and mapped (false: none could be)
bool arena_make(Arena& a, uint64_t size) {
    void* m = nullptr;
    a.size = size;
    if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, a.buffer, a.allocation,
                       &m))
        return false;
    if (!m) {
        destroy_buffer(a.buffer, a.allocation);   // (made but not mapped: of no use here)
        a.buffer = VK_NULL_HANDLE, a.allocation = nullptr;
        return false;
    }
    a.mapped = (uint8_t*)m;
    return true;
}

// ---- (hookshot, smoothness) a spare staging arena, made and freed off the replay thread ----
// Making a 64 MB arena is a memory allocation and a map: 6-66 ms of the replay thread on an RTX 3060 (Linux, NVIDIA 615.71), up to
// ~360 ms there with VRAM nearly full, and the frame waits for it. The 30 s idle rule and the 256 MB cap below free arenas that the
// next streamed texture then makes again in the middle of play (the "a staging arena of 64 MB took" lines, and the "a frame's uploads
// took" ones they fall in: Kakariko Village, 2026-11-12). On a card that keeps 256 MB of arenas, one 64 MB arena more is held ready:
// staging() takes it at once and a worker makes the next, and an idle arena that would be freed becomes the spare instead (the worker
// frees the others). A card that keeps 64 MB (6.5 GB or less) is as before: its VRAM is what the idle rule gives back.
// RX_VK_STAGING_SPARE=0: off, as before; 1: on whatever the card
struct SpareArenas {
    std::mutex mu;
    std::condition_variable cv;
    Arena spare;
    bool have = false, want = false, started = false;
    std::vector<Arena> to_free;
    std::atomic<int64_t> taken{0}, made_here{0};
};
SpareArenas spare_arenas;
bool spare_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_STAGING_SPARE");
        if (e && *e) return !env_value_off(e);
        return staging_keep() > kArena;
    }();
    return on;
}
void spare_worker() {
    SpareArenas& s = spare_arenas;
    std::unique_lock lk(s.mu);
    for (;;) {
        s.cv.wait(lk, [&] { return (s.want && !s.have) || !s.to_free.empty(); });
        std::vector<Arena> gone;
        gone.swap(s.to_free);
        const bool make = s.want && !s.have;
        s.want = false;
        lk.unlock();
        for (Arena& a : gone) destroy_buffer(a.buffer, a.allocation);
        Arena a;
        const bool ok = make && arena_make(a, kArena);
        lk.lock();
        if (ok) {
            if (!s.have) s.spare = a, s.have = true;
            else s.to_free.push_back(a);   // (one came back as the spare meanwhile)
        }
    }
}
/// (s.mu held) the worker started, and asked for a spare when there is none
void spare_wake(SpareArenas& s, bool want) {
    if (!s.started) {
        s.started = true;
        std::thread(spare_worker).detach();   // (for the process's life: the device is never destroyed before it ends)
    }
    if (want && !s.have) s.want = true;
    s.cv.notify_one();
}
/// The spare, if one is ready (asks the worker for the next either way)
bool spare_take(Arena& out) {
    if (!spare_on()) return false;
    SpareArenas& s = spare_arenas;
    std::lock_guard lk(s.mu);
    const bool got = s.have;
    if (got) out = s.spare, s.have = false, s.spare = {};
    spare_wake(s, true);
    return got;
}
/// An idle arena let go: the spare if there is none (a 64 MB one), else freed on the worker. false: spare_on() is off
bool spare_give_back(const Arena& a) {
    if (!spare_on()) return false;
    SpareArenas& s = spare_arenas;
    std::lock_guard lk(s.mu);
    if (!s.have && a.size == kArena) {
        s.spare = a, s.have = true;
        s.spare.value = 0, s.spare.used = 0;
    } else {
        s.to_free.push_back(a);
    }
    spare_wake(s, false);
    return true;
}

uint64_t completed_value = 0;
// (GPU writes into guest memory by buffers: GPU address -> (end, the submission that wrote it last))
std::map<uint64_t, std::pair<uint64_t, uint64_t>> gpu_wrote;
std::mutex gpu_wrote_mu;
}  // namespace
bool gpu_wrote_into(Texture& tex);
namespace {
Staging staging(uint64_t n, uint64_t align = 256) {
    // Separate slices may already be read by submitted parallel segments. A flush must not share their cache atom.
    align = std::max<uint64_t>(align, dev.props.limits.nonCoherentAtomSize);
    for (Arena& a : arenas) {   // the current one first, then any whose readers completed
        if (a.value == rec_value) {
            const uint64_t at = (a.used + align - 1) & ~(align - 1);
            if (at + n <= a.size) {
                a.used = at + n;
                a.used_at = std::chrono::steady_clock::now();
                return {a.buffer, at, a.mapped + at, a.allocation};
            }
        }
    }
    for (Arena& a : arenas) {
        if (a.value != rec_value && a.value <= completed_value && a.size >= n) {
            a.value = rec_value, a.used = n;
            a.used_at = std::chrono::steady_clock::now();
            return {a.buffer, 0, a.mapped, a.allocation};
        }
    }
    for (Arena& a : arenas) {
        if (a.value < rec_value) {
            const uint64_t at = (a.used + align - 1) & ~(align - 1);
            if (at + n <= a.size) {
                a.value = rec_value, a.used = at + n;
                a.used_at = std::chrono::steady_clock::now();
                return {a.buffer, at, a.mapped + at, a.allocation};
            }
        }
    }
    Arena a;
    const uint64_t need = (n + 0xFFFFF) & ~0xFFFFFull;
    if (need <= kArena && spare_take(a)) {   // (the spare, made off this thread: see SpareArenas)
        const int64_t k = ++spare_arenas.taken;
        if (k <= 4 || k % 100 == 0)
            logf("vk: staging: the spare arena taken (%lld times; %lld made on this thread)", (long long)k,
                 (long long)spare_arenas.made_here.load());
        a.value = rec_value, a.used = n;
        a.used_at = std::chrono::steady_clock::now();
        arenas.push_back(a);
        return {a.buffer, 0, a.mapped, a.allocation};
    }
    if (spare_on()) spare_arenas.made_here++;
    const auto make = [&](uint64_t size) { return arena_make(a, size); };
    const auto t_make = std::chrono::steady_clock::now();
    // (hookshot) and one of the size asked for when a 64 MB one cannot be made: without resizable BAR an arena is new system
    // memory the driver must pin. On a 16 GB laptop with the game's pools imported and RAM nearly gone, no 64 MB arena could
    // be made even for a 36 KB upload, the idle ones having been given back (texture_frame), and every upload failed until one
    // was free: BC3 textures drawn as noise (RX 5500M, 2.9 GB of RAM available)
    if (!make(std::max(kArena, need))) {
        const bool small = need < kArena && make(need);
        static std::atomic<int> told{0};
        if (told++ < 8)
            logf("vk: staging: no %llu MB arena could be made (%zu kept); %s", (unsigned long long)(kArena >> 20), arenas.size(),
                 small ? "one of the size asked for instead" : "the upload waits");
        if (!small) return {};
    }
    {   // (hookshot: what making a staging arena costs the replay thread: on a card with resizable BAR it is VRAM the driver hands
        // out, and a scene change made several right after the idle ones were freed)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_make).count();
        static int slow_logged;
        if (ms > 5 && slow_logged++ < 24) logf("vk: a staging arena of %llu MB took %.1f ms to make", (unsigned long long)(a.size >> 20), ms);
    }
    a.value = rec_value, a.used = n;
    a.used_at = std::chrono::steady_clock::now();
    arenas.push_back(a);
    static int logged;
    if (logged++ < 4 || arenas.size() % 16 == 0)
        logf("vk: staging arenas: %zu (%llu MB in this one)", arenas.size(), (unsigned long long)(a.size >> 20));
    return {a.buffer, 0, a.mapped, a.allocation};
}

/* (hookshot) The staging arenas, made before the game's pools and textures are. Making one later, with ~2.5 GB of pools and
   textures held, took 12-25 ms on the Steam Deck (an 8 MB one could take 116 ms: it is the allocator, not the size), and
   every one of them stalled the replay thread. That was the stutter when turning the camera: the arena the last burst used
   had gone stale and been given back, so the first upload of the next burst made another. On an empty device they are cheap.
   RX_VK_STAGING_PREWARM=<n>: how many, 0: none. */
}  // namespace (the prewarm is called from the replay's start: it has to link)
void texture_staging_prewarm(int n) {
    int made = 0;
    for (int i = 0; i < n; i++) {
        Arena a;
        void* m = nullptr;
        if (!create_buffer(kArena, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, a.buffer,
                           a.allocation, &m)) {
            logf("vk: staging prewarm: only %d of %d arenas could be made", made, n);
            return;
        }
        a.mapped = (uint8_t*)m;
        a.size = kArena, a.used = 0, a.value = 0;
        a.used_at = std::chrono::steady_clock::now();
        arenas.push_back(a);
        made++;
    }
    if (made) logf("vk: %d staging arenas of %llu MB made before the game's memory", made, (unsigned long long)(kArena >> 20));
}

namespace {
// ---- the GPU ASTC decoder (Eden's) ----
struct AstcPush {
    uint32_t block_dims[2], layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
};
VkDescriptorSetLayout astc_set = VK_NULL_HANDLE;
VkPipelineLayout astc_layout = VK_NULL_HANDLE;
VkPipeline astc_pipe = VK_NULL_HANDLE;
bool astc_ready() {
    static int state = 0;   // 0 untried, 1 ready, -1 unavailable
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || env_on("RX_VK_ASTC_CPU")) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &astc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &astc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &astc_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof ASTC_DECODER_COMP_SPV;
    mi.pCode = ASTC_DECODER_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = astc_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &astc_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: ASTC is decoded on the GPU (Eden's decoder, Switch 2 tiling)");
    return true;
}

VkImageView make_view(Texture* tex, VkImageViewType type, VkFormat f, VkImageAspectFlags aspect, uint32_t level0,
                      uint32_t nlevels, uint32_t layer0, uint32_t nlayers, const VkComponentMapping& cm,
                      VkImageUsageFlags usage, VkResult* result = nullptr) {
    if (result) *result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!usage || (usage & tex->usage) != usage) return VK_NULL_HANDLE;
    // (block-texel views of a compressed image: one level; several layers where the driver takes them)
    const bool block_view = is_bc(tex->format) && !is_bc(f);
    if (block_view && nlevels != 1) return VK_NULL_HANDLE;
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    uci.usage = usage & tex->usage;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = &uci;
    vi.image = tex->image;
    vi.viewType = type;
    vi.format = f;
    vi.components = cm;
    vi.subresourceRange = {aspect, level0, nlevels, layer0, nlayers};
    VkImageView v = VK_NULL_HANDLE;
    const VkResult first = vkCreateImageView(dev.device, &vi, nullptr, &v);
    if (result) *result = first;
    if (first == VK_SUCCESS) return v;
    if (!block_view || nlayers == 1) return VK_NULL_HANDLE;
    // (Vulkan 1.1 says a block-texel view is of a single layer; some drivers hold to it)
    vi.subresourceRange.layerCount = 1;
    if (vi.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    const VkResult last = vkCreateImageView(dev.device, &vi, nullptr, &v);
    if (result) *result = last == VK_SUCCESS || (first != VK_ERROR_OUT_OF_HOST_MEMORY && first != VK_ERROR_OUT_OF_DEVICE_MEMORY)
                              ? last : first;
    if (last != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

/// A failed allocation is not a permanent negative cache entry. At most one failing attempt per texture/frame.
template<class Make>
VkImageView cached_image_view(Texture* tex, uint64_t key, Make&& make) {
    if (auto it = tex->views.find(key); it != tex->views.end()) return it->second;
    if (tex->view_failed_frame == frame_no) return VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    const VkImageView view = make(&result);
    if (!view && (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)) {
        tex->view_failed_frame = frame_no;
        return VK_NULL_HANDLE;
    }
    try {
        auto [it, inserted] = tex->views.emplace(key, view);
        if (!inserted && view) vkDestroyImageView(dev.device, view, nullptr);
        return it->second;
    } catch (const std::exception&) {
        // The new view has no owner when its cache entry cannot be committed.
        if (view) vkDestroyImageView(dev.device, view, nullptr);
        tex->view_failed_frame = frame_no;
        return VK_NULL_HANDLE;
    }
}

// ---- big ASTC textures kept as BC3 (8 bits a texel, a quarter of RGBA8): decoded into a scratch image a few
// layers at a time, re-encoded by Eden's encoder (host_shaders/bc_encoder.comp: bounding box along the colours'
// covariance, inset; GPL, as the decoder) into a buffer, copied into the image. Decoded RGBA8 took 6 GB of VRAM at
// the title (the terrain's material arrays, 2048 x 2048 x 68, 1.45 GB each): more than the driver allowed, which
// paged VRAM out and evicted textures that were made and uploaded again. ASTC as RGBA8 (the menu's Developer row) or
// RX_VK_ASTC_RGBA8=1: all as RGBA8.
struct BcPush {
    uint32_t blocks_x, blocks_y, width, height, bc3, out_offset;
};
VkDescriptorSetLayout bc_set = VK_NULL_HANDLE;
VkPipelineLayout bc_layout = VK_NULL_HANDLE;
VkPipeline bc_pipe = VK_NULL_HANDLE;
VkPipeline bc7_pipe = VK_NULL_HANDLE;
bool bc7_ok = true;   // (false once BC7 is unsupported or its pipeline could not be made: BC3 from then on)
constexpr uint32_t kBcDim = 2048, kBcLayers = 8;   // the scratch: the largest level a BC texture may have
/// (hookshot) RX_VK_ASTC_BC7=1: big ASTC textures as BC7 instead of BC3. Both are 8 bits a texel; BC7's mode 6 keeps
/// the colours' gradients and alpha much better (the encoder is host_shaders/bc7_encoder.comp).
bool bc7_enabled() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_ASTC_BC7");
        return e && atoi(e) != 0;
    }();
    return on && bc7_ok;
}
VkImage bc_scratch = VK_NULL_HANDLE;
VmaAllocation bc_scratch_alloc = nullptr;
VkImageView bc_scratch_view = VK_NULL_HANDLE;
VkBuffer bc_out = VK_NULL_HANDLE;
VmaAllocation bc_out_alloc = nullptr;
bool bc_scratch_fresh = false;   // (its layout still to be set, in the next upload command buffer)
// (hookshot-amd) a second scratch for textures re-encoded at another size (uneven block levels on AMD): the decoded level
// is blitted into it, filtered, at the image's size, and encoded from there. Made when an upload of such a texture first
// needs it, given back with the first scratch.
VkImage bc_scratch2 = VK_NULL_HANDLE;
VmaAllocation bc_scratch2_alloc = nullptr;
VkImageView bc_scratch2_view = VK_NULL_HANDLE;
bool bc_scratch2_fresh = false;
// (the scratch and its buffer, 160 MB, are made when an upload first needs them and given back after RX_VK_BC_IDLE_S seconds
// (20) without one: a title or a load decodes hundreds of textures through them, steady gameplay few (the "given back" line says
// whether it did).
// 0: kept for good once made. They have memory objects of their own, so that freeing gives the VRAM back: VMA keeps the empty
// block of its image pool for the next images. RX_VK_BC_POOL=1: from the pool as before)
std::mutex bc_mu;
std::chrono::steady_clock::time_point bc_used_at;
uint64_t bc_last_value = 0;   // (the recording that last used them: they go once it is done)
std::atomic<bool> bc_have{false};
uint32_t bc_idle_s() {
    // (hookshot) 0 by default again, kept once made: given back after 20 s, the garbled/rainbow textures of 4-8 GB cards followed
    // (issue 993): every such log has the "given back" line, an RTX 3050 4 GB player's went away with RX_VK_BC_IDLE_S=0 and not with
    // RX_VK_NO_EVICT=1, and they began with v2.0.3, which brought this give-back. Its cost: 160-288 MB of VRAM held after the first
    // decode. RX_VK_BC_IDLE_S=<seconds>: given back as before
    static const uint32_t v = getenv("RX_VK_BC_IDLE_S") ? (uint32_t)atoi(getenv("RX_VK_BC_IDLE_S")) : 0;
    return v;
}
bool bc_scratch_make() {   // (bc_mu held)
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kBcDim, kBcDim, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kBcLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    static const bool pool = env_on("RX_VK_BC_POOL");
    if (!create_image(ii, bc_scratch, bc_scratch_alloc, !pool)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bc_scratch;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    const uint64_t out = (uint64_t)(kBcDim / 4) * (kBcDim / 4) * kBcLayers * 16;
    const VkBufferUsageFlags out_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const auto make_out = [&] {
        if (create_device_buffer(out, out_usage, bc_out, bc_out_alloc, !pool)) return true;
        // (hookshot) VRAM full, the process at its budget (a 4 GB card, the textures evicted after an alt-tab): in system
        // memory, as create_image puts the scratch image there. Only in VRAM, it could not be made, every BC3 texture made
        // again then failed, and they were drawn as coloured noise
        if (!create_buffer(out, out_usage, false, bc_out, bc_out_alloc, nullptr, !pool)) return false;
        static std::atomic<int> told{0};
        if (told++ < 4) logf("vk: VRAM full: the BC3 scratch's output buffer (%llu MB) in system memory", (unsigned long long)(out >> 20));
        return true;
    };
    if (vkCreateImageView(dev.device, &vi, nullptr, &bc_scratch_view) != VK_SUCCESS || !make_out()) {
        if (bc_scratch_view) vkDestroyImageView(dev.device, bc_scratch_view, nullptr);
        destroy_image(bc_scratch, bc_scratch_alloc);
        bc_scratch = VK_NULL_HANDLE, bc_scratch_view = VK_NULL_HANDLE;
        return false;
    }
    bc_scratch_fresh = true;
    bc_have = true;
    return true;
}

bool bc_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    // (the Developer section's ASTC as RGBA8 row, or RX_VK_ASTC_RGBA8 for a run: no BC3, the AMD texture workaround)
    const char* rgba8 = getenv("RX_VK_ASTC_RGBA8");
    if (!astc_ready() || (rgba8 ? !env_value_off(rgba8) : opts.astc_rgba8 != 0) || !dev.features.textureCompressionBC) return false;
    bool bc7 = bc7_enabled();
    VkFormat bc_format = bc7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, bc_format, &fp);
    if (bc7 && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        // (this GPU has BC but not BC7: the texture is BC3, not RGBA8)
        bc7 = false, bc7_ok = false;
        bc_format = VK_FORMAT_BC3_UNORM_BLOCK;
        vkGetPhysicalDeviceFormatProperties(dev.physical, bc_format, &fp);
    }
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &bc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &bc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &bc_layout) != VK_SUCCESS) return false;
    auto make_pipe = [&](const uint32_t* code, size_t bytes, VkPipeline& pipe) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = bytes;
        mi.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = bc_layout;
        const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
        vkDestroyShaderModule(dev.device, m, nullptr);
        return r == VK_SUCCESS;
    };
    if (!make_pipe(BC_ENCODER_COMP_SPV, sizeof BC_ENCODER_COMP_SPV, bc_pipe)) return false;
    if (bc7 && !make_pipe(BC7_ENCODER_COMP_SPV, sizeof BC7_ENCODER_COMP_SPV, bc7_pipe)) {
        // (the BC7 pipeline could not be made: BC3, not RGBA8. The next bc7_enabled() answers false too, so the encoders
        // and the textures made from now on take bc_pipe)
        bc7_ok = false, bc7 = false;
    }
    state = 1;
    logf("vk: big ASTC textures are kept as %s (re-encoded on the GPU; ASTC as RGBA8 or RX_VK_ASTC_RGBA8=1: as RGBA8)", bc7 ? "BC7" : "BC3");
    return true;
}

/// (hookshot-amd) the second scratch (see bc_scratch2; bc_mu held)
bool bc_scratch2_make() {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kBcDim, kBcDim, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kBcLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    static const bool pool = env_on("RX_VK_BC_POOL");
    if (!create_image(ii, bc_scratch2, bc_scratch2_alloc, !pool)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bc_scratch2;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    if (vkCreateImageView(dev.device, &vi, nullptr, &bc_scratch2_view) != VK_SUCCESS) {
        destroy_image(bc_scratch2, bc_scratch2_alloc);
        bc_scratch2 = VK_NULL_HANDLE, bc_scratch2_view = VK_NULL_HANDLE;
        return false;
    }
    bc_scratch2_fresh = true;
    return true;
}

/// (hookshot-amd) scratch [0, w) x [0, h), layers [0, n), blitted (linear) into scratch2 at ow x oh, then encoded from there
void bc_encode_resized(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t n, Texture& dst,
                       uint32_t level, uint32_t layer);

/// Encodes scratch layers [0, n) (a w x h region at its corner) and copies the blocks into dst's level and layers
void bc_encode_into(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t n, Texture& dst, uint32_t level,
                    uint32_t layer, int dx, int dy, VkImageView from = VK_NULL_HANDLE) {
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc7_enabled() ? bc7_pipe : bc_pipe);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, from ? from : bc_scratch_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi{bc_out, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[0].pImageInfo = &ii;
    wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[1].pBufferInfo = &bi;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_layout, 0, 2, wr);
    descriptors_disturbed();
    // (hookshot: BC1 for an image kept BC1, the colour half alone: with no texel under half alpha the encoder's four-colour
    // blocks, as BC3's colour half)
    const BcPush pc{(w + 3) / 4, (h + 3) / 4, w, h, dst.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 0u : 1u, 0};
    vkCmdPushConstants(cmd, bc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (pc.blocks_x + 7) / 8, (pc.blocks_y + 7) / 8, n);
    full_barrier(cmd);
    const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, n}, {dx, dy, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(cmd, bc_out, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cmd);   // (the scratch and the buffer are the next part's)
}

void bc_encode_resized(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t n, Texture& dst,
                       uint32_t level, uint32_t layer) {
    full_barrier(cmd);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
    bl.srcOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    bl.dstOffsets[1] = {(int32_t)ow, (int32_t)oh, 1};
    vkCmdBlitImage(cmd, bc_scratch, VK_IMAGE_LAYOUT_GENERAL, bc_scratch2, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    bc_encode_into(cmd, ow, oh, n, dst, level, layer, 0, 0, bc_scratch2_view);
}

/// The scratch for the upload recording now: made if it is not there, and its layout set (false: it could not be made).
/// resized: the second scratch too (hookshot-amd)
bool bc_scratch_layout(VkCommandBuffer cmd, bool resized = false) {
    {
        std::lock_guard lk(bc_mu);
        if (!bc_have && !bc_scratch_make()) return false;
        if (resized && !bc_scratch2 && !bc_scratch2_make()) return false;
        bc_used_at = std::chrono::steady_clock::now();
        bc_last_value = rec_value;
    }
    // (a segment's worker: a fresh scratch's layout change in queue order, as a new image's)
    InitRedirect redirect((bc_scratch2_fresh && bc_scratch2) || bc_scratch_fresh);
    if (redirect.to) cmd = upload_cmd();
    if (bc_scratch2_fresh && bc_scratch2) {   // (hookshot-amd: the second scratch, once made)
        bc_scratch2_fresh = false;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = bc_scratch2;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    if (!bc_scratch_fresh) return true;
    bc_scratch_fresh = false;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = bc_scratch;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}
/// (every present: the scratch unused for RX_VK_BC_IDLE_S seconds goes with the images the GPU is done with)
void bc_scratch_idle() {
    if (!bc_have.load(std::memory_order_relaxed) || !bc_idle_s()) return;
    std::lock_guard lk(bc_mu);
    if (!bc_have || std::chrono::steady_clock::now() - bc_used_at < std::chrono::seconds(bc_idle_s())) return;
    Dead d{bc_last_value, bc_scratch, bc_scratch_alloc, {bc_scratch_view}, {}};
    d.buffer = bc_out, d.buffer_alloc = bc_out_alloc;
    dead.push_back(std::move(d));
    bc_scratch = VK_NULL_HANDLE, bc_scratch_alloc = nullptr, bc_scratch_view = VK_NULL_HANDLE, bc_out = VK_NULL_HANDLE, bc_out_alloc = nullptr;
    bc_have = false, bc_scratch_fresh = false;
    const bool second = bc_scratch2 != VK_NULL_HANDLE;
    if (second) {   // (hookshot-amd: the second scratch, 128 MB, with it)
        dead.push_back(Dead{bc_last_value, bc_scratch2, bc_scratch2_alloc, {bc_scratch2_view}, {}});
        bc_scratch2 = VK_NULL_HANDLE, bc_scratch2_alloc = nullptr, bc_scratch2_view = VK_NULL_HANDLE, bc_scratch2_fresh = false;
    }
    logf("vk: the BC3 scratch (%s) unused for %u s: given back", second ? "288 MB, both" : "160 MB", bc_idle_s());
}

/// The storage view of one level (every layer) the ASTC decoder writes
VkImageView astc_target_view(Texture* tex, uint32_t level) {
    const uint64_t key = mix(0xA57C, level);
    return cached_image_view(tex, key, [&](VkResult* result) {
        return make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                         level, 1, 0, tex->layers, {}, VK_IMAGE_USAGE_STORAGE_BIT, result);
    });
}

// ---- uploads ----
struct LevelGeo {
    uint64_t offset;            // from the storage's start
    uint32_t w, h, d;           // texels
    uint32_t tw, th, td;        // blocks
    unsigned tile_h, tile_d;    // block height / depth (log2 GOBs)
    uint64_t swizzled;          // bytes of one layer of it in the storage
};
LevelGeo level_geo(const Texture& tex, uint32_t level) {   // (level: the image's; the storage's is level + skip)
    LevelGeo g{};
    const uint32_t gl = level + tex.skip;
    g.offset = host->tex_level_offset(&tex.d, gl);
    const uint32_t w0 = std::max(1u, tex.d.width);
    const uint32_t h0 = tex.type == VK_IMAGE_TYPE_1D ? 1 : std::max(1u, tex.d.height);
    const uint32_t d0 = tex.type == VK_IMAGE_TYPE_3D ? std::max(1u, tex.d.depth) : 1;
    g.w = std::max(1u, w0 >> gl);
    g.h = std::max(1u, h0 >> gl);
    g.d = std::max(1u, d0 >> gl);
    g.tw = (g.w + tex.block_w - 1) / tex.block_w;
    g.th = (g.h + tex.block_h - 1) / tex.block_h;
    g.td = g.d;
    host->tex_level_tiles(&tex.d, gl, &g.tile_h, &g.tile_d);
    const uint64_t gobs_x = ((uint64_t)g.tw * tex.bpb + 63) / 64;
    const uint64_t rows_y = (g.th + (8ull << g.tile_h) - 1) >> (3 + g.tile_h);
    const uint64_t slices = (g.td + (1ull << g.tile_d) - 1) >> g.tile_d;
    g.swizzled = gobs_x * rows_y * slices * (512ull << (g.tile_h + g.tile_d));
    return g;
}

/// The guest bytes [gpu, gpu + n) in host memory: in place when contiguous, else copied into scratch
const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, std::vector<uint8_t>& scratch) {
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(gpu, &avail);
    // (below 1 TB the GPU address is the guest's: the storage as it was at the submit being replayed, where the game has
    // rewritten it since (vk_memory.cpp, "texture storage in the capture"))
    if (p && avail >= n) return gpu < (1ull << 40) ? memory_as_submitted(gpu, p, n, scratch) : p;
    if (n >= (32u << 20)) {   // (a big sparse texture's pages, gathered on several threads: the course maps' 349 MB)
        scratch.resize(n);
        const uint32_t nt = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
        const uint64_t per = ((n + nt - 1) / nt + 0xFFFF) & ~0xFFFFull;
        std::vector<std::thread> pool;
        auto part = [&](uint32_t t) {
            const uint64_t a = (uint64_t)t * per, b = std::min(n, a + per);
            for (uint64_t at = a; at < b; at += 0x10000)
                if (!host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, b - at)))
                    std::memset(scratch.data() + at, 0, std::min<uint64_t>(0x10000, b - at));
        };
        for (uint32_t t = 1; t < nt; t++) pool.emplace_back(part, t);
        part(0);
        for (std::thread& t : pool) t.join();
        return scratch.data();
    }
    scratch.assign(n, 0);
    if (!host->gpu_read(gpu, scratch.data(), n)) {
        for (uint64_t at = 0; at < n; at += 0x10000) host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, n - at));
    }
    return scratch.data();
}

void widen_rows(uint8_t* dst, const uint8_t* src, size_t texels, int from, int to) {
    const int comp = from / 3;
    for (size_t i = 0; i < texels; i++) {
        std::memcpy(dst + i * to, src + i * from, from);
        uint8_t* a = dst + i * to + from;
        if (comp == 1) a[0] = 0xFF;
        else if (comp == 2) a[0] = 0x00, a[1] = 0x3C;   // (half 1.0; UNORM16 would want 0xFFFF: close enough for alpha)
        else { const float one = 1.0f; std::memcpy(a, &one, 4); }
    }
}

/// Byte ranges of a texture's storage, from its start (what the CPU wrote since its last upload)
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
bool overlaps(const Ranges* dirty, uint64_t a, uint64_t b) {
    if (!dirty) return true;
    for (const auto& r : *dirty)
        if (r.first < b && a < r.second) return true;
    return false;
}

/// The view of one level the ASTC decoder writes: every layer, or one (layer)
VkImageView astc_level_view(Texture* tex, uint32_t level, uint32_t layer) {
    if (layer == ~0u) return astc_target_view(tex, level);
    const uint64_t key = mix(mix(0xA57D, level), layer);
    return cached_image_view(tex, key, [&](VkResult* result) {
        return make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                         level, 1, layer, 1, {}, VK_IMAGE_USAGE_STORAGE_BIT, result);
    });
}

/// ASTC decoded on the GPU (Eden's decoder): the layers dirty overlaps (all of them without dirty), each layer's
/// levels from the first one the image keeps (a texture that leaves out its top levels stages only the tail of
/// every layer's chain: Eden's AssembleInto does the same)
/// (hookshot) why the last upload_astc_gpu on this thread failed, for the log of a BC3 image left to be tried again
thread_local const char* astc_fail = nullptr;

// (hookshot) Big ASTC textures whose every block decodes opaque (astc_opaque.h) are kept as BC1, BC3's colour half: the same
// texels in half the video memory. A texture's first whole upload with texels in it decides (its image made BC3 is made again
// as BC1 then); an upload into a BC1 image with alpha in it makes it BC3 again and uploads it whole. On by default;
// RX_VK_ASTC_BC1=0: all BC3, as before.
bool bc1_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_ASTC_BC1");
        if (e && e[0] == '0') return false;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &fp);
        return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }();
    return on;
}
extern uint64_t destroy_gen;   // (below)
/// (hookshot) tex's image made again in format f (BC1 or BC3, the same size, levels, layers and usage), its old image and views
/// let go once the work that used them is done (as the compaction's move): its views are made again when next asked for
bool bc_reformat(Texture& tex, VkFormat f) {
    if (tex.format == f || tex.sparse || tex.pool_key) return false;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = tex.create_flags;
    ci.imageType = tex.type;
    ci.format = f;
    ci.extent = {tex.width, tex.height, tex.depth};
    ci.mipLevels = tex.levels;
    ci.arrayLayers = tex.layers;
    ci.samples = tex.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = tex.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    if (!(compact_on() && create_image(ci, image, alloc, false, false, true)) && !create_image(ci, image, alloc)) return false;
    destroy_gen++;
    Dead d{tex.last_value, tex.image, tex.allocation, {}, {}};
    for (auto& kv : tex.views)
        if (kv.second) d.views.push_back(kv.second);
    dead.push_back(std::move(d));
    tex.views.clear();
    total_bytes -= std::min(total_bytes, tex.bytes + tex.pad);
    const bool bc1 = f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    const uint64_t was = tex.bytes;
    tex.bytes = bc1 ? tex.bytes / 2 : tex.bytes * 2;   // (BC3: a byte a texel; BC1 half that)
    tex.image = image, tex.allocation = alloc, tex.format = f;
    tex.pad = std::max(tex.bytes, allocation_size(alloc)) - tex.bytes;
    total_bytes += tex.bytes + tex.pad;
    VkCommandBuffer cmd = upload_cmd();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {tex.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    static std::atomic<int64_t> n_bc1{0}, n_back{0}, saved{0};
    if (bc1) n_bc1++, saved += (int64_t)(was - tex.bytes);
    else n_back++, saved -= (int64_t)(tex.bytes - was);
    const int64_t k = bc1 ? n_bc1.load() : n_back.load();
    if (k <= 4 || (k & (k - 1)) == 0)
        logf("vk: texture %#llx %ux%u x%u: %s (%lld opaque textures kept as BC1, %lld back to BC3; %lld MB less than BC3; "
             "RX_VK_ASTC_BC1=0: all BC3)", (unsigned long long)tex.gpu, tex.width, tex.height, tex.layers,
             bc1 ? "every block opaque, kept as BC1" : "alpha in it now, BC3 again", (long long)n_bc1.load(), (long long)n_back.load(),
             (long long)(saved.load() >> 20));
    return true;
}

bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
    astc_fail = nullptr;
    if (tex.type != VK_IMAGE_TYPE_2D || tex.samples != VK_SAMPLE_COUNT_1_BIT || !astc_ready()) {
        astc_fail = "not a 2D image the GPU decoder takes";
        return false;
    }
    const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);
    const uint64_t tail0 = level_geo(tex, 0).offset;
    if (!layer_size || tail0 >= layer_size || (tail0 & 15) || (layer_size & 15)) {
        astc_fail = "its layout";
        return false;
    }
    const uint64_t tail = layer_size - tail0;
    std::vector<uint32_t> layers;
    for (uint32_t layer = 0; layer < tex.layers; layer++)
        if (overlaps(dirty, (uint64_t)layer * layer_size + tail0, (uint64_t)(layer + 1) * layer_size)) layers.push_back(layer);
    if (layers.empty()) return true;
    const bool all = layers.size() == tex.layers;
    const Staging s = staging(tail * layers.size(), std::max<uint64_t>(256, dev.props.limits.minStorageBufferOffsetAlignment));
    if (!s.buffer) {
        astc_fail = "no staging memory for its ASTC blocks";
        return false;
    }
    std::vector<uint8_t> scratch;
    for (size_t k = 0; k < layers.size(); k++) {
        const uint64_t at = (uint64_t)layers[k] * layer_size + tail0;
        if (at >= total) continue;
        const uint64_t n = std::min(tail, total - at);
        std::memcpy(s.data + k * tail, guest_bytes(tex.gpu + at, n, scratch), n);
    }
    const uint64_t staged = tail * layers.size();
    flush_mapped_range(s.allocation, s.offset, staged);   // (encoded blocks read by the GPU decoder, not necessarily coherent)
    st_bytes += (int64_t)staged;
    VkCommandBuffer cmd = upload_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 4);
    if (tex.bc && !tex.bc_resized && linear_twin(tex.format) != VK_FORMAT_BC7_UNORM_BLOCK && bc1_on()) {   // (hookshot: BC1 for a
        // texture whose blocks are all opaque, astc_opaque.h; a BC7 image, RX_VK_ASTC_BC7's, stays BC7)
        const bool is_bc1 = tex.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        const astc_opaque::Scan sc = astc_opaque::scan(s.data, staged);
        if (is_bc1 && sc.alpha) {   // (new contents with alpha: BC3 again, every layer uploaded into it)
            if (bc_reformat(tex, VK_FORMAT_BC3_UNORM_BLOCK)) {
                tex.bc1 = 1;
                return upload_astc_gpu(tex, total, nullptr);
            }
        } else if (!is_bc1 && !tex.bc1) {
            if (sc.alpha) tex.bc1 = 1;
            else if (all && astc_opaque::whole_opaque(sc) && bc_reformat(tex, VK_FORMAT_BC1_RGBA_UNORM_BLOCK)) tex.bc1 = 2;
        }
    }
    if (tex.bc) {   // (decoded into the scratch a run of consecutive layers at a time, re-encoded, copied in)
        if (!bc_scratch_layout(cmd, tex.bc_resized)) {
            astc_fail = "no memory for the BC3 scratch";
            return false;
        }
        for (uint32_t level = 0; level < tex.levels; level++) {
            const LevelGeo g = level_geo(tex, level);
            const uint64_t off = g.offset - tail0;
            if (g.offset < tail0 || off >= tail || (off % align)) continue;
            const uint32_t iw = std::max(1u, tex.width >> level), ih = std::max(1u, tex.height >> level);
            // (hookshot-amd: a resized BC3 image: the level decodes at the game's size, g.w x g.h, and is encoded at iw x ih)
            AstcPush pc{};
            pc.block_dims[0] = (uint32_t)tex.block_w, pc.block_dims[1] = (uint32_t)tex.block_h;
            pc.layer_stride = (uint32_t)tail;
            const uint32_t gobs_x = (g.tw * 16 + 63) >> 6;
            pc.x_shift = 9 + g.tile_h + g.tile_d;
            pc.block_size = gobs_x << pc.x_shift;
            pc.block_height = g.tile_h;
            pc.block_height_mask = (1u << g.tile_h) - 1;
            pc.native_tiling = 1;
            for (size_t k0 = 0; k0 < layers.size();) {
                size_t k1 = k0 + 1;
                // (one layer a run: with runs of up to kBcLayers, an array's layers past the run's first came out wrong in the
                // smaller levels, and Hyrule Field's ground, which samples the terrain's material arrays (1365 x 1365 x 90, ASTC
                // 8x8) at those levels from a distance, had its tiles outlined in dark lines (RX 6600: none with one layer a run,
                // nor with the arrays kept as RGBA8). RX_VK_BC_LAYERS=<n>: up to n a run)
                static const uint32_t run = getenv("RX_VK_BC_LAYERS") ? std::clamp<uint32_t>((uint32_t)atoi(getenv("RX_VK_BC_LAYERS")), 1u, kBcLayers) : 1u;
                while (k1 < layers.size() && k1 - k0 < run && layers[k1] == layers[k1 - 1] + 1) k1++;
                const uint32_t n = (uint32_t)(k1 - k0);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);
                const uint64_t from = k0 * tail + off;
                VkDescriptorBufferInfo bi{s.buffer, s.offset + from, staged - from};
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, bc_scratch_view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
                w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[0].pBufferInfo = &bi;
                w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[1].pImageInfo = &ii;
                vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_layout, 0, 2, w);
                vkCmdPushConstants(cmd, astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cmd, (g.tw + 7) / 8, (g.th + 7) / 8, n);
                if (tex.bc_resized)
                    bc_encode_resized(cmd, g.w, g.h, iw, ih, n, tex, level, layers[k0]);
                else
                    bc_encode_into(cmd, iw, ih, n, tex, level, layers[k0], 0, 0);
                k0 = k1;
            }
        }
        st_astc++;
        return true;
    }
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        const uint64_t off = g.offset - tail0;
        if (g.offset < tail0 || off >= tail || (off % align)) continue;
        AstcPush pc{};
        pc.block_dims[0] = (uint32_t)tex.block_w, pc.block_dims[1] = (uint32_t)tex.block_h;
        pc.layer_stride = (uint32_t)tail;
        const uint32_t gobs_x = (g.tw * 16 + 63) >> 6;
        pc.x_shift = 9 + g.tile_h + g.tile_d;
        pc.block_size = gobs_x << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        for (size_t k = 0; k < (all ? 1 : layers.size()); k++) {
            const VkImageView view = astc_level_view(&tex, level, all ? ~0u : layers[k]);
            if (!view) {
                astc_fail = "no view of a level";
                return false;
            }
            const uint64_t from = (all ? 0 : k * tail) + off;
            VkDescriptorBufferInfo bi{s.buffer, s.offset + from, staged - from};
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[0].pBufferInfo = &bi;
            w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].pImageInfo = &ii;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_layout, 0, 2, w);
            vkCmdPushConstants(cmd, astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cmd, (g.tw + 7) / 8, (g.th + 7) / 8, all ? tex.layers : 1);
        }
    }
    st_astc++;
    return true;
}

// ---- big block-linear textures unswizzled on the GPU (Eden's block_linear_unswizzle_2d.comp, GPL, with the Switch
// 2 tiling): the shader reads the storage where it is, in its imported pool, and writes the image. The course maps
// the title loads at each scene change (six of 349 MB) were copied into staging and unswizzled on the CPU: ~200 ms
// of the replay thread. RX_VK_GPU_UNSWIZZLE=0: on the CPU. ----
struct UnswizzlePush {
    uint32_t origin[3], pad0;
    int32_t destination[3];
    uint32_t bytes_per_block_log2, layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
};
VkDescriptorSetLayout unsw_set = VK_NULL_HANDLE;
VkPipelineLayout unsw_layout = VK_NULL_HANDLE;
VkPipeline unsw_pipe = VK_NULL_HANDLE;
bool unswizzle_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    if (getenv("RX_VK_GPU_UNSWIZZLE") && !atoi(getenv("RX_VK_GPU_UNSWIZZLE"))) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &unsw_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UnswizzlePush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &unsw_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &unsw_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof BLOCK_LINEAR_UNSWIZZLE_2D_COMP_SPV;
    mi.pCode = BLOCK_LINEAR_UNSWIZZLE_2D_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = unsw_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &unsw_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: big block-linear textures are unswizzled on the GPU from their pools (RX_VK_GPU_UNSWIZZLE=0: the CPU)");
    return true;
}

/// The whole texture (every level and layer dirty overlaps) unswizzled on the GPU; false: not a texture for it
bool upload_unswizzle_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
    if (tex.type != VK_IMAGE_TYPE_2D || tex.astc || tex.bc || !(tex.usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
        is_bc(tex.format) || is_astc(tex.format) || is_depth_format(tex.format) || nvn_tex_is_linear(&tex.d))
        return false;
    if (total < (8u << 20) || tex.block_w != 1 || tex.block_h != 1) return false;
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    if (widen_from) return false;
    VkFormat vf;
    switch (tex.bpb) {
    case 1: vf = VK_FORMAT_R8_UINT; break;
    case 2: vf = VK_FORMAT_R16_UINT; break;
    case 4: vf = VK_FORMAT_R32_UINT; break;
    case 8: vf = VK_FORMAT_R32G32_UINT; break;
    case 16: vf = VK_FORMAT_R32G32B32A32_UINT; break;
    default: return false;
    }
    if (!unswizzle_ready()) return false;
    // (hookshot) Only from a pool already imported: an import made here, for one upload, took 22-25 ms on the replay
    // thread (the driver locks the pages): three of them when the camera turns to the Deku Tree, a 60 ms hitch. The CPU
    // path (several threads from 4 MB) does a 15 MB level in a few ms. RX_VK_UNSWIZZLE_IMPORT=1: import as before
    static const bool may_import = env_on("RX_VK_UNSWIZZLE_IMPORT");
    if (!may_import) {   // (a virtual pool's address above 1 TB: its guest address, as gpu_buffer translates it)
        uint64_t avail = 0;
        if (!host->gpu_ptr(tex.gpu, &avail) || avail < total) return false;
        const uint64_t guest = tex.gpu < (1ull << 40) ? tex.gpu : host->gpu_guest(tex.gpu);
        if (!memory_guest_contained(guest, guest + total)) return false;
    }
    const BufferRef src = gpu_buffer(tex.gpu, total);
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 16);
    if (!src || (src.offset % align)) return false;
    for (uint32_t level = 0; level < tex.levels; level++) {   // (every level must be possible before any is done)
        const LevelGeo g = level_geo(tex, level);
        if (g.td != 1 || g.tile_d || (g.offset % align) || g.offset >= total) return false;
        // (the shader reads whole GOB-block rows: a level whose last one runs past the storage, which the CPU path
        // reads from a zero-padded copy, would read past the pool and fault the device)
        if (g.offset + (uint64_t)(tex.layers - 1) * tex.L.layer_size + g.swizzled > total) return false;
    }
    VkCommandBuffer cmd = upload_cmd();
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_pipe);
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        bool any = false;
        for (uint32_t layer = 0; layer < tex.layers && !any; layer++)
            any = overlaps(dirty, g.offset + (uint64_t)layer * tex.L.layer_size,
                           g.offset + (uint64_t)layer * tex.L.layer_size + g.swizzled);
        if (!any) continue;
        const uint64_t key = mix(0x5B1Cu, mix(level, (uint64_t)vf));
        const VkImageView view = cached_image_view(&tex, key, [&](VkResult* result) {
            return make_view(&tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, vf, VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0,
                             tex.layers, {}, VK_IMAGE_USAGE_STORAGE_BIT, result);
        });
        if (!view) return false;
        UnswizzlePush pc{};
        pc.bytes_per_block_log2 = (uint32_t)std::countr_zero((uint32_t)tex.bpb);
        pc.layer_stride = (uint32_t)tex.L.layer_size;
        const uint32_t gobs_x = (uint32_t)(((uint64_t)g.tw * tex.bpb + 63) >> 6);
        pc.x_shift = 9 + g.tile_h;
        pc.block_size = gobs_x << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        VkDescriptorBufferInfo bi{src.buffer, src.offset + g.offset, total - g.offset};
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &ii;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_layout, 0, 2, w);
        vkCmdPushConstants(cmd, unsw_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        checkpoint_mark(cmd, tex.gpu, 'g');
        vkCmdDispatch(cmd, (g.tw + 31) / 32, (g.th + 31) / 32, tex.layers);
        st_bytes += (int64_t)g.swizzled * tex.layers;
    }
    full_barrier(cmd);
    descriptors_disturbed();
    return true;
}

/// The texture from guest memory into its image: all of it, or (dirty) the layers, levels and block rows the CPU
/// wrote. Block-linear levels are rows of blocks a GOB-block high, each self-contained: a written page re-uploads
/// the block rows it falls in.
uint64_t st_made;          // (textures made)
int64_t st_unswizzle_bytes, st_unswizzle_ns;   // (the CPU's unswizzling)
std::map<uint64_t, std::pair<uint64_t, uint32_t>> gpu_owned;   // GPU address -> (end, memory generation then)
thread_local uint64_t tl_watch_prog;
/// (RX_VK_VERIFY_DIM=WxH: the guest memory of textures that size, hashed when uploaded and every 300 frames: a change
/// the write tracking did not report is logged as a missed write)
std::pair<uint32_t, uint32_t> verify_dim() {
    static const std::pair<uint32_t, uint32_t> d = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_VERIFY_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    return d;
}
std::unordered_map<uint32_t, std::vector<uint8_t>> verify_snap;   // (each verified texture's memory when uploaded)
uint64_t guest_hash(const Texture& tex, bool snapshot = false) {
    std::vector<uint8_t> b(tex.guest_size);
    if (!host->gpu_read(tex.gpu, b.data(), b.size())) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    if (snapshot) verify_snap[tex.id] = std::move(b);
    return h;
}
/// (a missed write: which 4 KB pages changed since the upload, and the CPU's next writes to them watched)
void verify_report(const Texture& tex) {
    const auto it = verify_snap.find(tex.id);
    if (it == verify_snap.end()) return;
    std::vector<uint8_t> now(tex.guest_size);
    if (!host->gpu_read(tex.gpu, now.data(), now.size())) return;
    std::string runs;
    int nruns = 0, watched = 0;
    uint64_t changed = 0;
    for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
        const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
        if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
        changed++;
        if (nruns < 12) {
            char b[48];
            snprintf(b, sizeof b, " +%#llx", (unsigned long long)off);
            runs += b, nruns++;
        }
        static int watches;
        if (host->watch_guest_writes && watches < 4 && watched < 2) {
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            if (g) host->watch_guest_writes(g, 4096), watches++, watched++;
        }
    }
    logf("vk: verify: texture %#llx: %llu pages changed since the upload:%s%s", (unsigned long long)tex.gpu,
         (unsigned long long)changed, runs.c_str(), watched ? " (the next CPU writes to two of them watched)" : "");
    {   // (the first changed page as the write tracking has it: ever written, written since the upload, and when)
        for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
            const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
            if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            std::vector<std::pair<uint64_t, uint64_t>> ever, since;
            memory_written_runs(g, 4096, 0, ever);
            memory_written_runs(g, 4096, tex.upload_gen, since);
            logf("vk: verify: its page +%#llx (guest %#llx): stamped ever %d, since the upload (gen %u) %d; generation now %u",
                 (unsigned long long)off, (unsigned long long)g, (int)!ever.empty(), tex.upload_gen, (int)!since.empty(),
                 memory_generation());
            break;
        }
    }
    it->second = std::move(now);
}   // (the draw or dispatch the replay is at, for watch_tex)
/// (RX_VK_WATCH_TEX=addr[,addr...]: what happens to the textures over those GPU addresses, 600 lines)
void watch_tex(const Texture* tex, const char* what) {
    static std::vector<uint64_t> addrs = [] {
        std::vector<uint64_t> v;
        if (const char* e = getenv("RX_VK_WATCH_TEX"))
            for (const char* p = e; *p;) {
                char* end;
                const uint64_t a = strtoull(p, &end, 16);
                if (end == p) break;
                v.push_back(a);
                p = *end ? end + 1 : end;
            }
        return v;
    }();
    static const std::pair<uint32_t, uint32_t> dim = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_WATCH_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    static const std::pair<uint64_t, uint64_t> range = [] {   // (RX_VK_WATCH_RANGE=<lo>:<hi>: textures overlapping it)
        unsigned long long a = 0, b = 0;
        if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
        return std::pair<uint64_t, uint64_t>{a, b};
    }();
    if (tex && range.second && tex->gpu < range.second && tex->gpu + std::max<uint64_t>(tex->guest_size, 1) > range.first) {
        static int rlogged;
        if (rlogged++ < 600)
            logf("vk: watch range frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d size %llu%s%s",
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu,
                 tex->width, tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->d.target,
                 (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "", tex->uploaded ? " uploaded" : "");
    }
    if ((addrs.empty() && !dim.first) || !tex) return;
    {   // (RX_VK_DUMP_ON_WATCH=<frames>: a watched texture made after boot dumps the frames after it, once)
        static const int on_watch = getenv("RX_VK_DUMP_ON_WATCH") ? atoi(getenv("RX_VK_DUMP_ON_WATCH")) : 0;
        static bool requested = false;
        if (on_watch && !requested && frame_no > 1000 && !strcmp(what, "made")) {
            for (const uint64_t a : addrs)
                if (a == tex->gpu) {
                    requested = true;
                    dump_request((int64_t)frame_no + 1, on_watch);
                    logf("vk: watch: texture %#llx made at frame %llu: the next %d frames dumped", (unsigned long long)a,
                         (unsigned long long)frame_no, on_watch);
                }
        }
    }
    static int logged;
    static std::unordered_map<uint32_t, uint64_t> told;   // (a texture's events once a frame)
    const bool by_dim = dim.first && tex->width == dim.first && tex->height == dim.second;
    if (by_dim && !strcmp(what, "written")) {   // ("written" once a frame; the rest each time)
        uint64_t& f = told[tex->id];
        if (f == frame_no || logged >= 20000) return;
        f = frame_no;
    }
    const uint64_t hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
    for (const uint64_t a : by_dim ? std::vector<uint64_t>{tex->gpu} : addrs) {
        if (a < tex->gpu || a >= hi || logged >= (by_dim ? 20000 : 600)) continue;
        logged++;
        logf("vk: watch tex frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x layout %#x class %#x target %d "
             "size %llu%s%s", (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu, tex->width,
             tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->key_layout, tex->key_cls,
             tex->d.target, (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "",
             tex->uploaded ? " uploaded" : "");
        return;
    }
}
/// Whether [gpu, gpu + n) is storage the GPU's images rendered last (covered by ranges render targets took), and the
/// generation from which the CPU's writes would count (0: not the GPU's)
/// (hookshot) The ranges can overlap (texture_rendered drops only the ones inside a new range), so the bytes they cover are
/// counted once each: summed, two overlapping ranges counted their overlap twice and a range with a gap the GPU never drew
/// read as covered, and a texture over it was never uploaded
uint32_t gpu_owned_since(uint64_t gpu, uint64_t n) {
    uint64_t covered = 0, reach = gpu;   // (reach: the end of what is counted so far; the ranges come in address order)
    uint32_t since = UINT32_MAX;
    auto it = gpu_owned.upper_bound(gpu);
    if (it != gpu_owned.begin()) --it;
    for (; it != gpu_owned.end() && it->first < gpu + n; ++it) {
        const uint64_t a = std::max(it->first, gpu), b = std::min(it->second.first, gpu + n);
        if (b <= a) continue;
        since = std::min(since, it->second.second);
        if (b > std::max(a, reach)) covered += b - std::max(a, reach), reach = b;
    }
    return covered >= n && since != UINT32_MAX ? std::max(since, 1u) : 0;
}

// (hookshot, smoothness) What a frame's uploads cost by the way they went: when a frame's uploads take over 50 ms (the stutters at
// area loads and in walking, 180-360 ms frames of 250-450 MB: Discord and the frame trace, 2026-11-09), one log line of the routes
// (ASTC on the GPU, unswizzled on the GPU, on the CPU), their count, MB and ms, and the slowest textures
enum UpRoute { UP_ASTC, UP_GPU, UP_CPU, UP_COUNT };
thread_local int up_route = UP_CPU;
struct UpStat { int64_t n = 0, bytes = 0, ns = 0; };
UpStat up_frame[UP_COUNT];
struct UpSlow { int64_t ns; uint32_t w, h, layers, nvn; int route; uint64_t bytes; };
std::vector<UpSlow> up_slowest;
std::mutex up_mu;   // (uploads may come from a parallel replay's threads)
std::atomic<int64_t> frame_upload_ns{0};   // (this frame's time in upload(), every thread's)
std::atomic<int64_t> st_deferred{0}, st_deferred_frame{0};
void upload_frame_report() {
    std::lock_guard lk(up_mu);
    int64_t ns = 0;
    for (const UpStat& u : up_frame) ns += u.ns;
    static int logged;
    if (ns >= 50000000 && logged < 300) {
        logged++;
        static const char* names[UP_COUNT] = {"ASTC on the GPU", "unswizzled on the GPU", "on the CPU"};
        char line[900];
        int k = snprintf(line, sizeof line, "vk: a frame's uploads took %.0f ms:", ns / 1e6);
        for (int r = 0; r < UP_COUNT; r++)
            if (up_frame[r].n)
                k += snprintf(line + k, sizeof line - k, " %s %lld (%.1f MB, %.0f ms);", names[r], (long long)up_frame[r].n,
                              up_frame[r].bytes / 1048576.0, up_frame[r].ns / 1e6);
        std::sort(up_slowest.begin(), up_slowest.end(), [](const UpSlow& a, const UpSlow& b) { return a.ns > b.ns; });
        for (size_t i = 0; i < up_slowest.size() && i < 3 && k < (int)sizeof line - 80; i++)
            k += snprintf(line + k, sizeof line - k, " [%ux%u x%u NVN %#x %s %.1f MB %.0f ms]", up_slowest[i].w, up_slowest[i].h,
                          up_slowest[i].layers, up_slowest[i].nvn, names[up_slowest[i].route], up_slowest[i].bytes / 1048576.0,
                          up_slowest[i].ns / 1e6);
        logf("%s", line);
    }
    for (UpStat& u : up_frame) u = {};
    up_slowest.clear();
}
bool upload_inner(Texture& tex, const Ranges* dirty);
bool upload(Texture& tex, const Ranges* dirty) {
    const auto t0 = std::chrono::steady_clock::now();
    up_route = UP_CPU;
    const bool ok = upload_inner(tex, dirty);
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    frame_upload_ns.fetch_add(ns, std::memory_order_relaxed);
    std::lock_guard lk(up_mu);
    UpStat& u = up_frame[up_route];
    u.n++, u.bytes += (int64_t)tex.guest_size, u.ns += ns;
    if (ns > 2000000) {
        up_slowest.push_back({ns, tex.width, tex.height, tex.layers, tex.nvn_format, up_route, tex.guest_size});
        if (up_slowest.size() > 64) {
            std::sort(up_slowest.begin(), up_slowest.end(), [](const UpSlow& a, const UpSlow& b) { return a.ns > b.ns; });
            up_slowest.resize(8);
        }
    }
    return ok;
}
/// (hookshot) A big texture whose whole storage is zero: the image cleared to zero on the GPU, not unswizzled on the CPU. All-zero
/// bits are 0 in every format this path takes (not ASTC or BC, not one widened on the way), so the clear is what the unswizzle
/// would have made. The game's shadow map (12288 x 12288 D16, 288 MB, its storage fresh and zero) took 73 ms of the replay
/// thread at a scene change; the check reads the storage once, on several threads, and stops at the first byte that is not 0.
/// RX_VK_ZERO_UPLOAD=0: off
bool upload_all_zero(Texture& tex, uint64_t total, const Ranges* dirty, bool widened_format) {
    static const bool off = getenv("RX_VK_ZERO_UPLOAD") && atoi(getenv("RX_VK_ZERO_UPLOAD")) == 0;
    if (off || total < (32ull << 20) || tex.astc || tex.bc || tex.sparse || widened_format || !tex.image ||
        tex.type == VK_IMAGE_TYPE_3D || is_bc(tex.format) || is_astc(tex.format))
        return false;
    if (dirty) {   // (only an upload of the whole storage: a part of it says nothing of the rest)
        uint64_t covered = 0;
        for (const auto& r : *dirty) covered += std::min(r.second, total) - std::min(r.first, total);
        if (covered < total) return false;
    }
    // (live memory, as the GPU unswizzle reads it: not a storage the capture kept as it was at its submit, which the CPU path reads)
    if (tex.gpu >= (1ull << 40) || memory_has_shadow(tex.gpu, total)) return false;
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(tex.gpu, &avail);
    if (!p || avail < total) return false;
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<bool> found{false};
    const uint32_t nthreads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    const uint64_t per = ((total / nthreads) + 4095) & ~4095ull;
    auto part = [&](uint32_t t) {
        const uint64_t a = std::min<uint64_t>(total, (uint64_t)t * per), b = std::min<uint64_t>(total, a + per);
        for (uint64_t at = a; at < b && !found.load(std::memory_order_relaxed); at += 65536) {
            const uint64_t end = std::min<uint64_t>(b, at + 65536);
            uint64_t acc = 0;
            uint64_t k = at;
            for (; k + 8 <= end; k += 8) {
                uint64_t w;
                std::memcpy(&w, p + k, 8);
                acc |= w;
            }
            for (; k < end; k++) acc |= p[k];
            if (acc) found.store(true, std::memory_order_relaxed);
        }
    };
    if (nthreads > 1) {
        std::vector<std::thread> th;
        for (uint32_t t = 1; t < nthreads; t++) th.emplace_back(part, t);
        part(0);
        for (auto& x : th) x.join();
    } else {
        part(0);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> told{0};
    if (found.load()) {
        if (told.fetch_add(1) < 4)
            logf("vk: upload: %ux%u NVN %#x (%llu MB) is not all zero (checked in %.1f ms): unswizzled as before", tex.width,
                 tex.height, tex.nvn_format, (unsigned long long)(total >> 20), ms);
        return false;
    }
    texture_resident_flush(&tex);
    checkpoint_mark(upload_cmd(), tex.gpu, 'u');
    const VkImageSubresourceRange range{tex.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (tex.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(upload_cmd(), tex.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    } else {
        const VkClearColorValue z{};
        vkCmdClearColorImage(upload_cmd(), tex.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    }
    if (told.fetch_add(1) < 4)
        logf("vk: upload: %ux%u NVN %#x (%llu MB) is all zero: cleared on the GPU (checked in %.1f ms)", tex.width, tex.height,
             tex.nvn_format, (unsigned long long)(total >> 20), ms);
    return true;
}
/// An image no upload takes, whatever memory there is: its upload fails every time, and it is not tried again
bool upload_never(const Texture& tex) {
    return !tex.gpu || tex.samples != VK_SAMPLE_COUNT_1_BIT || !tex.guest_size ||
           (tex.guest_size > (1ull << 31) && !tex.sparse) ||   // (a sparse image: only what holds texels)
           // (depth-stencil: the depth aspect only; packed D24S8 storage is not copied into it as is: such targets are
           // rendered, not uploaded)
           tex.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
}
bool upload_inner(Texture& tex, const Ranges* dirty) {
    PROF(PROF_UPLOAD);
    if (upload_never(tex)) return false;
    const uint64_t total = tex.guest_size;
    st_uploads++;
    {   // RX_VK_LOG_UPLOADS=<present>: the uploads from that present on (200 of them; what churns)
        static const int64_t from = getenv("RX_VK_LOG_UPLOADS") ? atoll(getenv("RX_VK_LOG_UPLOADS")) : -1;
        static int logged;
        if (from >= 0 && (int64_t)frame_no >= from && logged < 200) {
            logged++;
            uint64_t part = 0;
            if (dirty)
                for (const auto& r : *dirty) part += r.second - r.first;
            logf("vk: upload #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s: %llu KB of %llu KB (%s), made at present %llu",
                 tex.id, (unsigned long long)tex.gpu, tex.width, tex.height, tex.depth, tex.layers, tex.levels,
                 tex.nvn_format, tex.astc ? " ASTC" : "", (unsigned long long)((dirty ? part : total) >> 10),
                 (unsigned long long)(total >> 10), !tex.uploaded ? "new" : dirty ? "written" : "whole",
                 (unsigned long long)tex.first_seen);
        }
    }
    if (tex.astc && (up_route = UP_ASTC, upload_astc_gpu(tex, total, dirty))) return true;
    up_route = UP_CPU;
    if (tex.bc) return false;   // (no CPU path into BC3)
    // (the GPU's unswizzle reads guest memory as it is: not where the capture kept the storage as it was at its submit)
    if (!(tex.gpu < (1ull << 40) && memory_has_shadow(tex.gpu, total)) && upload_unswizzle_gpu(tex, total, dirty))
        return up_route = UP_GPU, true;
    const bool linear = nvn_tex_is_linear(&tex.d);
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    if (upload_all_zero(tex, total, dirty, widen_from != 0)) return true;
    const int src_bpb = tex.bpb;
    const int out_bpb = tex.astc ? 4 : widen_from ? (widen_from == 3 ? 4 : widen_from == 6 ? 8 : 16) : src_bpb;
    const uint32_t copy_layers = tex.type == VK_IMAGE_TYPE_3D ? 1 : tex.layers;
    const VkImageAspectFlags aspect = tex.aspect;
    std::vector<VkBufferImageCopy> copies;
    std::vector<uint8_t> scratch, lin;
    VkBuffer buffer = VK_NULL_HANDLE;
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        for (uint32_t layer = 0; layer < copy_layers; layer++) {
            const uint64_t src_off = g.offset + (uint64_t)layer * tex.L.layer_size;
            if (src_off >= total) continue;
            const uint32_t stride = linear ? (tex.d.stride ? tex.d.stride : host->tex_linear_stride(&tex.d)) : 0;
            const uint64_t span = linear ? (uint64_t)stride * g.th : std::max<uint64_t>(g.swizzled, 1);
            if (!overlaps(dirty, src_off, src_off + span)) continue;
            // the block rows to upload: all of them, or those a written page falls in (2D block-linear levels)
            const uint32_t rows_per = 8u << g.tile_h;   // (block rows of a GOB block)
            const uint32_t nrows = (g.th + rows_per - 1) / rows_per;
            const uint64_t gobs_x = ((uint64_t)g.tw * src_bpb + 63) / 64;
            const uint64_t row_bytes = gobs_x * (512ull << (g.tile_h + g.tile_d));
            std::vector<std::pair<uint32_t, uint32_t>> runs;   // (first row, rows)
            if (!dirty || linear || tex.astc || widen_from || g.td > 1 || g.tile_d) {
                runs.push_back({0, nrows});
            } else {
                std::vector<uint8_t> mark(nrows, 0);
                for (const auto& r : *dirty) {
                    const uint64_t lo = std::max(r.first, src_off), hi = std::min(r.second, src_off + span);
                    if (lo >= hi) continue;
                    const uint64_t r0 = (lo - src_off) / row_bytes, r1 = std::min<uint64_t>((hi - 1 - src_off) / row_bytes, nrows - 1);
                    for (uint64_t k = r0; k <= r1; k++) mark[k] = 1;
                }
                for (uint32_t k = 0; k < nrows;) {
                    if (!mark[k]) { k++; continue; }
                    uint32_t e = k;
                    while (e < nrows && mark[e]) e++;
                    runs.push_back({k, e - k});
                    k = e;
                }
            }
            for (const auto& [row0, nrow] : runs) {
                const uint32_t y0 = row0 * rows_per;                       // (blocks)
                const uint32_t th = std::min(g.th - y0, nrow * rows_per);   // (blocks)
                const uint64_t block_bytes = (uint64_t)g.tw * th * g.td * src_bpb;
                const uint32_t px_h = std::min(g.h - y0 * (uint32_t)tex.block_h, th * (uint32_t)tex.block_h);
                const uint64_t out_bytes = tex.astc ? (uint64_t)g.w * px_h * g.d * 4 : (uint64_t)g.tw * th * g.td * out_bpb;
                const Staging st = staging((out_bytes + 15) & ~15ull, 16);
                if (!st.buffer) {
                    // (the sparse binds queued by the rows before are this image's: bound now, not left in sparse_binds
                    // for the next flush, which binds them into another texture's image)
                    texture_resident_flush(&tex);
                    return false;
                }
                if (buffer && st.buffer != buffer && !copies.empty()) {   // (a new arena: copy what the last one holds)
                    texture_resident_flush(&tex);
                    checkpoint_mark(upload_cmd(), tex.gpu, 'u');
                    vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL,
                                           (uint32_t)copies.size(), copies.data());
                    copies.clear();
                }
                buffer = st.buffer;
                uint8_t* out = st.data;
                uint8_t* blocks = (tex.astc || widen_from) ? (lin.resize(block_bytes), lin.data()) : out;
                if (linear) {
                    const uint64_t row = (uint64_t)g.tw * src_bpb;
                    const uint8_t* src = guest_bytes(tex.gpu + src_off, (uint64_t)stride * (g.th - 1) + row, scratch);
                    for (uint32_t y = 0; y < g.th; y++) std::memcpy(blocks + y * row, src + (uint64_t)y * stride, row);
                } else {
                    // (suyu's unswizzle reads the whole swizzled span without bounds checks: a span the storage
                    // ends in is read from a zero-padded copy)
                    const uint64_t from = src_off + (uint64_t)row0 * row_bytes;
                    const uint64_t need = runs.size() == 1 && row0 == 0 && nrow == nrows ? span : (uint64_t)nrow * row_bytes;
                    // (a big 2D level: its GOB-block rows unswizzled on several threads, each row on its own. The
                    // course maps the title loads at each scene change, 6 of 349 MB, took ~100 ms each on one. Their
                    // storage is sparse: each thread gathers its own rows, 8 MB at a time, rather than all of it
                    // being copied first (a 349 MB scratch beside the 349 MB staged))
                    // (hookshot: from 4 MB, not 16: the scene-size targets uploaded when the camera turns to the Deku Tree,
                    // 2779 x 1318 RGBA8 = 15 MB, took ~20 ms each on one thread, three in one frame: a 50 ms hitch)
                    const uint32_t nthreads = block_bytes >= (4u << 20) && g.td == 1 && !g.tile_d
                                                  ? std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u) : 1u;
                    uint64_t avail = 0;
                    const uint8_t* direct = host->gpu_ptr(tex.gpu + from, &avail);
                    const bool gather_parts = nthreads > 1 && !(direct && avail >= need) && from < total;
                    const uint8_t* src = nullptr;
                    if (gather_parts) {
                    } else if (from < total && total - from >= need) {
                        src = guest_bytes(tex.gpu + from, need, scratch);
                    } else {
                        scratch.assign(need, 0);
                        if (from < total) host->gpu_read(tex.gpu + from, scratch.data(), total - from);
                        src = scratch.data();
                    }
                    const auto u0 = std::chrono::steady_clock::now();
                    if (nthreads > 1) {
                        const uint64_t out_row = (uint64_t)g.tw * rows_per * src_bpb;   // (bytes per GOB-block row)
                        const uint32_t per = (nrow + nthreads - 1) / nthreads;
                        const uint32_t chunk = (uint32_t)std::max<uint64_t>(1, (8u << 20) / row_bytes);
                        auto part = [&](uint32_t t) {
                            const uint32_t a0 = t * per, b0 = std::min(nrow, a0 + per);
                            std::vector<uint8_t> mine;
                            for (uint32_t a = a0; a < b0; a += chunk) {
                                const uint32_t b = std::min(b0, a + chunk);
                                const uint32_t h = std::min(th - a * rows_per, (b - a) * rows_per);
                                const uint8_t* rows;
                                uint64_t rows_n;
                                if (gather_parts) {   // (its rows, zero past the storage's end)
                                    rows_n = (uint64_t)(b - a) * row_bytes;
                                    mine.assign(rows_n, 0);
                                    const uint64_t at = from + (uint64_t)a * row_bytes;
                                    for (uint64_t o = 0; o < rows_n && at + o < total; o += 0x10000) {
                                        const uint64_t k = std::min<uint64_t>({0x10000, rows_n - o, total - at - o});
                                        host->gpu_read(tex.gpu + at + o, mine.data() + o, k);
                                    }
                                    rows = mine.data();
                                } else {
                                    rows = src + (uint64_t)a * row_bytes, rows_n = need - (uint64_t)a * row_bytes;
                                }
                                Tegra::Texture::UnswizzleTexture(
                                    std::span<uint8_t>(blocks + a * out_row, (uint64_t)g.tw * h * src_bpb),
                                    std::span<const uint8_t>(rows, rows_n), (u32)src_bpb, g.tw, h, 1, g.tile_h, 0, 1);
                            }
                        };
                        std::vector<std::thread> pool;
                        for (uint32_t t = 1; t < nthreads; t++) pool.emplace_back(part, t);
                        part(0);
                        for (std::thread& t : pool) t.join();
                        static const bool check = env_on("RX_VK_UNSWIZZLE_CHECK");   // (against one thread)
                        if (check && src) {
                            std::vector<uint8_t> ref(block_bytes);
                            Tegra::Texture::UnswizzleTexture(std::span<uint8_t>(ref.data(), block_bytes),
                                                             std::span<const uint8_t>(src, need), (u32)src_bpb, g.tw,
                                                             th, g.td, g.tile_h, g.tile_d, 1);
                            const bool same = !std::memcmp(ref.data(), blocks, block_bytes);
                            logf("vk: unswizzle check %#llx level %u rows %u+%u: %s", (unsigned long long)tex.gpu,
                                 level, row0, nrow, same ? "same" : "DIFFERENT");
                        }
                    } else {
                        Tegra::Texture::UnswizzleTexture(std::span<uint8_t>(blocks, block_bytes),
                                                         std::span<const uint8_t>(src, need), (u32)src_bpb, g.tw, th,
                                                         g.td, g.tile_h, g.tile_d, 1);
                    }
                    st_unswizzle_bytes += (int64_t)block_bytes;
                    st_unswizzle_ns +=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - u0).count();
                }
                if (tex.astc) {
                    Tegra::Texture::ASTC::DecompressSerial(std::span<const uint8_t>(blocks, block_bytes), g.w, px_h, g.d,
                                                           (u32)tex.block_w, (u32)tex.block_h,
                                                           std::span<uint8_t>(out, (size_t)g.w * px_h * g.d * 4));
                } else if (widen_from) {
                    widen_rows(out, blocks, (size_t)g.tw * th * g.td, src_bpb, out_bpb);
                }
                st_bytes += (int64_t)out_bytes;
                // (a sparse image: only its blocks this holds texels other than zero get memory; the others read
                // zero unbound, and what the copy writes into them is dropped)
                if (tex.sparse) {
                    const VkExtent3D gr = tex.sparse_req.formatProperties.imageGranularity;
                    const uint32_t bw = (uint32_t)tex.block_w, bh = (uint32_t)tex.block_h;
                    const uint64_t row = (uint64_t)g.tw * out_bpb;   // (bytes of a row of blocks)
                    const uint32_t tbw = std::max(1u, gr.width / bw);   // (blocks across a sparse block)
                    const uint32_t ntx = (g.tw + tbw - 1) / tbw;
                    std::vector<uint8_t> hit;   // (sparse block rows x columns this reaches)
                    const uint32_t ty0 = y0 * bh / gr.height, ty1 = ((y0 + th) * bh - 1) / gr.height;
                    hit.assign((size_t)(ty1 - ty0 + 1) * ntx, 0);
                    if (level < tex.sparse_req.imageMipTailFirstLod) {
                        for (uint32_t br = 0; br < th; br++) {
                            const uint8_t* p = out + br * row;
                            const uint32_t ty = (y0 + br) * bh / gr.height - ty0;
                            for (uint32_t tx = 0; tx < ntx; tx++) {
                                uint8_t& h = hit[(size_t)ty * ntx + tx];
                                if (h) continue;
                                const uint64_t a = (uint64_t)tx * tbw * out_bpb, e = std::min<uint64_t>(row, a + (uint64_t)tbw * out_bpb);
                                uint64_t any = 0;
                                for (uint64_t i = a; i + 8 <= e; i += 8) {
                                    uint64_t v;
                                    std::memcpy(&v, p + i, 8);
                                    any |= v;
                                }
                                h = any != 0;
                            }
                        }
                    } else {
                        uint64_t any = 0;
                        for (uint64_t i = 0; i + 8 <= out_bytes && !any; i += 8) {
                            uint64_t v;
                            std::memcpy(&v, out + i, 8);
                            any |= v;
                        }
                        if (any) hit.assign(hit.size(), 1);
                    }
                    bool some = false;
                    for (uint32_t ty = 0; ty < ty1 - ty0 + 1; ty++)
                        for (uint32_t tx = 0; tx < ntx; tx++)
                            if (hit[(size_t)ty * ntx + tx]) {
                                some = true;
                                texture_resident_queue(&tex, level, layer, tx * gr.width, (ty0 + ty) * gr.height,
                                                       gr.width, gr.height);
                            }
                    if (!some) continue;
                }
                flush_mapped_range(st.allocation, st.offset, out_bytes);   // (all CPU conversion workers joined before publication)
                VkBufferImageCopy c{};
                c.bufferOffset = st.offset;
                c.bufferRowLength = tex.astc ? g.w : g.tw * tex.block_w;
                c.bufferImageHeight = tex.astc ? px_h : th * tex.block_h;
                c.imageSubresource = {aspect, level, tex.type == VK_IMAGE_TYPE_3D ? 0 : layer, 1};
                c.imageOffset = {0, (int32_t)(y0 * (uint32_t)tex.block_h), 0};
                c.imageExtent = {g.w, px_h, g.d};
                copies.push_back(c);
            }
        }
    }
    if (!copies.empty()) {
        texture_resident_flush(&tex);
        checkpoint_mark(upload_cmd(), tex.gpu, 'u');
        vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)copies.size(),
                               copies.data());
    }
    return true;
}

/// What the CPU wrote of a texture's storage since gen, as byte ranges from its start. A texture in a virtual
/// (sparse) pool is looked at page by page through its mapping, and is all dirty when its mapping changed.
bool texture_dirty(Texture& tex, uint32_t gen, Ranges& out) {
    out.clear();
    constexpr uint64_t kVirtual = 1ull << 40, kVPage = 1ull << 16;
    if (tex.gpu < kVirtual) {
        memory_written_runs(tex.guest, tex.guest_size, gen, out);
        return !out.empty();
    }
    // (the page walk only when the GPU mappings changed: the terrain's sparse arrays span over 1 GB, 64 KB pages,
    // and were walked every recording; between changes, runs of contiguous guest memory are checked)
    const uint32_t maps_gen = host->maps_generation() + host->vmap_generation();
    if (tex.runs_maps_gen != maps_gen) {
        tex.runs_maps_gen = maps_gen;
        tex.runs.clear();
        uint64_t sig = 0x5157;
        // (the pages whose mapping changed: only those are dirty. The course maps' sparse pools are remapped every
        // ~15 s at the title, a few pages at a time, and each remap re-uploaded all of six 349 MB maps)
        const size_t npages = (size_t)((tex.guest_size + kVPage - 1) / kVPage);
        const bool had_map = tex.page_map.size() == npages;
        if (!had_map) tex.page_map.assign(npages, 0);
        Ranges remapped;
        for (uint64_t off = 0; off < tex.guest_size; off += kVPage) {
            const uint64_t n = std::min(kVPage, tex.guest_size - off);
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            sig = mix(sig, g);
            uint64_t& was = tex.page_map[(size_t)(off / kVPage)];
            if (had_map && was != g) {
                if (!remapped.empty() && remapped.back().second == off) remapped.back().second = off + n;
                else remapped.push_back({off, off + n});
            }
            was = g;
            if (!g) continue;
            Texture::Run& last = tex.runs.empty() ? tex.runs.emplace_back(Texture::Run{off, g, 0}) : tex.runs.back();
            if (last.off + last.n == off && last.guest + last.n == g) last.n += n;
            else tex.runs.push_back({off, g, n});
        }
        std::erase_if(tex.runs, [](const Texture::Run& r) { return r.n == 0; });
        {   // (RX_VK_LOG_REMAP=1: a big texture's pages mapped anew: to which guest memory, and what images are there)
            static const bool log_remap = env_on("RX_VK_LOG_REMAP");
            static int logged;
            if (log_remap && had_map && !remapped.empty() && tex.guest_size >= (32ull << 20) && logged < 120) {
                for (size_t i = 0; i < remapped.size() && i < 4 && logged < 120; i++, logged++) {
                    const uint64_t g = host->gpu_guest(tex.gpu + remapped[i].first);
                    std::string there;
                    for (const auto& kv : textures) {   // (images over that guest memory, by their storage)
                        const Texture* o = kv.second.get();
                        if (o == &tex || !g || o->gpu >= (1ull << 40) || g < o->gpu || g >= o->gpu + o->guest_size) continue;
                        char b[160];
                        snprintf(b, sizeof b, " #%u %#llx %ux%u x%u NVN %#x%s%s;", o->id, (unsigned long long)o->gpu, o->width,
                                 o->height, o->layers, o->nvn_format, o->gpu_written ? " rendered" : "",
                                 o->uploaded ? " uploaded" : "");
                        if (there.size() < 400) there += b;
                    }
                    logf("vk: remap of #%u %#llx %ux%u NVN %#x: +%#llx..+%#llx now at guest %#llx%s%s", tex.id,
                         (unsigned long long)tex.gpu, tex.width, tex.height, tex.nvn_format,
                         (unsigned long long)remapped[i].first, (unsigned long long)remapped[i].second, (unsigned long long)g,
                         g ? "" : " (unmapped)", there.empty() ? " (no image there)" : there.c_str());
                }
            }
        }
        if (sig != tex.map_sig) {   // (pages mapped or unmapped since; gen 0 asks what was ever written)
            tex.map_sig = sig;
            if (gen && !had_map) {
                out.assign(1, {0, tex.guest_size});
                return true;
            }
            if (gen) out = remapped;   // (and below, what the CPU wrote in the pages mapped all along)
        }
    }
    static thread_local Ranges in_run;
    const size_t remaps = out.size();
    for (const Texture::Run& r : tex.runs) {
        in_run.clear();
        memory_written_runs(r.guest, r.n, gen, in_run);
        for (const auto& [lo, hi] : in_run) {
            if (out.size() > remaps && out.back().second == r.off + lo) out.back().second = r.off + hi;
            else out.push_back({r.off + lo, r.off + hi});
        }
    }
    if (remaps && out.size() > remaps) {   // (the two lists in order, overlaps merged)
        std::sort(out.begin(), out.end());
        Ranges merged;
        for (const auto& r : out) {
            if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
            else merged.push_back(r);
        }
        out.swap(merged);
    }
    return !out.empty();
}

uint64_t destroy_gen = 1;   // (bumped when textures go: what was resolved to them goes stale)

/// (A render target's image goes to the pool now rather than at the next present: between the two, the next frame is
/// recorded, and a target of its shape made there missed it and made a new image, while the old one was destroyed unused
/// later, 120 presents on (issue 2180's A/B: 65% more targets let go, 83% more images made new). The eviction only hands
/// over one the GPU is done with (it skips those in flight), so the next frame's target takes it)
void destroy_later(Texture& tex) {
    destroy_gen++;
    Dead d{tex.last_value, tex.image, tex.allocation, {}, {}};
    if (tex.pool_key && tex.allocation && tex.sparse_chunks.empty() && pool_cap()) {   // (a render target's: to the pool, its views too)
        d.pool_key = tex.pool_key, d.view_map = std::move(tex.views), d.bytes = tex.bytes + tex.pad;
        tex.views.clear();
        pool_put(d);
    } else {
        for (auto& kv : tex.views)
            if (kv.second) d.views.push_back(kv.second);
        d.memory = tex.sparse_chunks;
        dead.push_back(std::move(d));
    }
    for (const auto& attachment : tex.block_attachments)
        dead.push_back(Dead{tex.last_value, attachment.image, attachment.allocation, {}, {}});
    total_bytes -= std::min(total_bytes, tex.bytes + tex.pad);
}

void vw_forget(const Texture& t);   // (below: a destroyed texture leaves the list of GPU-written ones)
void forget_gpu(Texture* tex) {
    vw_forget(*tex);
    auto [a, b] = by_gpu.equal_range(tex->gpu);
    for (auto it = a; it != b; ++it)
        if (it->second == tex) {
            by_gpu.erase(it);
            break;
        }
}

/// (as_target: texture_get's for a render target or a copy's destination, not for sampling: its image goes to the render targets'
/// blocks, and to the target pool when its texture goes)
/// (hookshot) a size and sample count Vulkan can make on this GPU: they come from the game's texture (its width a u32 it
/// reads from a file), and past the device's limits vkCreateImage is undefined, and the uploads' buffers grew with them
bool texture_size_ok(VkImageType type, uint32_t w, uint32_t h, uint32_t z, uint32_t layers, int samples) {
    const VkPhysicalDeviceLimits& lim = dev.props.limits;
    const uint32_t most = type == VK_IMAGE_TYPE_1D ? lim.maxImageDimension1D
                        : type == VK_IMAGE_TYPE_3D ? lim.maxImageDimension3D : lim.maxImageDimension2D;
    return w <= most && h <= most && z <= lim.maxImageDimension3D && layers <= lim.maxImageArrayLayers &&
           (samples <= 1 || samples == 2 || samples == 4 || samples == 8 || samples == 16);
}
/// One mip level in texels (bw = bh = 1), or whole blocks for a compressed image.
/// Round each mip extent up separately: even a 1x1 BC/ASTC level occupies one block.
/// (Counted as texels, an ASTC image the GPU samples itself was 16 bytes a texel, 16 to 144 times its size, and
/// BC1/BC4 twice theirs: tests/test_native_astc_texture_count.py)
uint64_t texture_level_units(uint32_t w, uint32_t h, uint32_t d, uint32_t bw, uint32_t bh) {
    w = std::max(1u, w), h = std::max(1u, h), d = std::max(1u, d), bw = std::max(1u, bw), bh = std::max(1u, bh);
    return (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * d;
}
/// (hookshot) The Texture detail row (opts.texture_detail): the widest a big decoded ASTC texture is kept (its top levels left out
/// above it) and from how many MB as RGBA8 that applies. Auto: the low-VRAM rule (a driver budget of 4.5 GB or less at the
/// Minimum preset: 1024 from 8 MB), else 2048 from 64 MB; Full: always that; Medium: 1024 from 8 MB; Low: 512 from 8 MB.
/// RX_VK_TEX_MAX_DIM and RX_VK_TEX_SKIP_MB over it.
struct DetailDims { uint32_t max_dim; uint64_t skip_from_mb; };
DetailDims texture_detail_dims(int detail, bool low_vram) {
    if (detail == 3) return {512, 8};
    if (detail == 2 || (detail == 0 && low_vram)) return {1024, 8};
    return {2048, 64};
}
bool low_vram_texture_mips() {
    static const bool disabled = [] {
        const char* e = getenv("RX_VK_LOW_VRAM_MIPS");
        return e && env_value_off(e);
    }();
    // A driver budget, sampled once, not the card's physical capacity. The
    // preset is checked for each texture so changing it in the menu still works.
    static const bool small = [] {
        uint64_t used = 0, allowed = 0;
        return vram_usage(&used, &allowed) && allowed && allowed <= (4ull << 30) + (512ull << 20);
    }();
    // (Minimum, or Auto (this PC) at Minimum's tier: its budgets, opts.budget 1)
    return !disabled && small && (opts.preset == 1 || (opts.preset == kAutoPreset && opts.budget == 1));
}

Texture* create(const Key& key, const nvn_tex* t, VkFormat vf, bool astc, int bw, int bh, int bpb, bool as_target) {
    PROF_EXACT();   // (texture_get's slow path: PROF_TEXGET samples the rest)
    auto tex = std::make_unique<Texture>();
    Texture& x = *tex;
    x.gpu = t->gpu, x.d = t->d, x.L = t->L, x.nvn_format = t->d.format;
    x.guest = host->gpu_guest(t->gpu);
    x.guest_size = t->L.size;
    x.astc = astc, x.block_w = bw, x.block_h = bh, x.bpb = bpb;
    int from = 0;
    vf = widened(vf, &from);
    const bool depth = is_depth_format(vf);
    x.format = image_format_of(vf, astc);
    x.aspect = !depth ? VK_IMAGE_ASPECT_COLOR_BIT
             : vf == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
             : has_stencil(vf) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                               : VK_IMAGE_ASPECT_DEPTH_BIT;
    const nvn_texdesc& d = t->d;
    x.width = std::max(1u, d.width);
    x.height = std::max(1u, d.height);
    switch (d.target) {
    case NVN_TEXTURE_TARGET_1D: x.type = VK_IMAGE_TYPE_1D, x.height = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: x.type = VK_IMAGE_TYPE_1D, x.layers = std::max(1u, d.height), x.height = 1; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: x.layers = std::max(1u, d.depth); break;
    case NVN_TEXTURE_TARGET_CUBEMAP: x.layers = 6; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: x.layers = std::max(6u, d.depth); break;
    case NVN_TEXTURE_TARGET_3D: x.type = VK_IMAGE_TYPE_3D, x.depth = std::max(1u, d.depth); break;
    default: break;
    }
    const uint32_t max_dim = std::max({x.width, x.height, x.depth});
    const uint32_t max_levels = 1 + (uint32_t)std::floor(std::log2((double)max_dim));
    x.levels = nvn_tex_is_linear(&d) ? 1 : std::clamp<uint32_t>(d.levels, 1, max_levels);
    {   // huge textures leave out their top levels: wider than RX_VK_TEX_MAX_DIM (default 2048) once they would take
        // 64 MB, and down to RX_VK_TEX_MAX_MB (default 512) whatever their size (MKW's terrain material arrays are
        // 8192 x 8192 x 68 ASTC: 24 GB as RGBA8)
        const DetailDims dims = texture_detail_dims(opts.texture_detail, low_vram_texture_mips());
        static const char* dim_env = getenv("RX_VK_TEX_MAX_DIM");
        const uint32_t max_dim = dim_env ? (uint32_t)atoi(dim_env) : dims.max_dim;
        static const uint64_t max_bytes = (uint64_t)(getenv("RX_VK_TEX_MAX_MB") ? atoi(getenv("RX_VK_TEX_MAX_MB")) : 1536) << 20;
        const bool blocks = !astc && (is_bc(x.format) || is_astc(x.format));
        const uint32_t texel = astc ? 4 : blocks ? bpb : std::max(1, bpb / std::max(1, bw * bh));
        auto bytes_at = [&](uint32_t skip) {
            uint64_t px = 0;
            for (uint32_t l = skip; l < x.levels; l++)
                px += texture_level_units(x.width >> l, x.height >> l, x.depth >> l,
                                          blocks ? (uint32_t)bw : 1u, blocks ? (uint32_t)bh : 1u);
            return px * x.layers * texel * std::max(1, d.samples);
        };
        // (decoded ASTC only by size: uncompressed maps may be read at exact texel coordinates, and the GPU may
        // render into them, where a level left out is wrong)
        // (a 2D array too big to keep whole, in a format with sparse residency: kept whole, as a sparse image)
        if (!astc && !depth && dev.sparse_images && x.type == VK_IMAGE_TYPE_2D && x.layers > 1 && d.samples <= 1 &&
            bytes_at(0) > max_bytes) {
            VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (!is_bc(x.format)) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            uint32_t np = 0;
            vkGetPhysicalDeviceSparseImageFormatProperties(dev.physical, x.format, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
                                                           u, VK_IMAGE_TILING_OPTIMAL, &np, nullptr);
            x.sparse = np > 0;
        }
        // (RX_VK_TEX_SKIP_MB: from how many MB as RGBA8 a decoded ASTC texture wider than max_dim loses its top levels;
        // 8 on Minimum with a small driver budget, otherwise 64. A 3072-square image is about 48 MB decoded:
        // below 64, and too wide for the BC3 scratch. Explicit overrides keep main's existing parsing.)
        static const char* skip_env = getenv("RX_VK_TEX_SKIP_MB");
        const uint64_t skip_from = (uint64_t)(skip_env ? atoi(skip_env) : dims.skip_from_mb) << 20;
        while (!x.sparse && astc && x.skip + 1 < x.levels && bytes_at(x.skip) > skip_from &&
               (std::max(x.width, x.height) >> x.skip) > max_dim)
            x.skip++;
        while (!x.sparse && x.skip + 1 < x.levels && bytes_at(x.skip) > max_bytes) x.skip++;
        if (x.skip) {
            static int logged;
            if (logged++ < 40)
                logf("vk: texture %#llx %ux%ux%u x%u (NVN %#x, %llu MB): its %u top level(s) left out (%llu MB)",
                     (unsigned long long)t->gpu, x.width, x.height, x.depth, x.layers, d.format,
                     (unsigned long long)(bytes_at(0) >> 20), x.skip, (unsigned long long)(bytes_at(x.skip) >> 20));
            x.width = std::max(1u, x.width >> x.skip), x.height = std::max(1u, x.height >> x.skip);
            x.depth = std::max(1u, x.depth >> x.skip);
            x.levels -= x.skip;
        }
    }
    if (!texture_size_ok(x.type, x.width, x.height, x.depth, x.layers, d.samples)) {
        static int logged;
        if (logged++ < 16)
            logf("vk: texture %#llx %ux%ux%u x%u samples %d (NVN %#x): larger than this GPU's images, not made",
                 (unsigned long long)t->gpu, x.width, x.height, x.depth, x.layers, d.samples, d.format);
        return nullptr;
    }
    x.samples = d.samples > 1 ? (VkSampleCountFlagBits)d.samples : VK_SAMPLE_COUNT_1_BIT;
    {   // (decoded ASTC as BC3: 2D, at most the scratch's size; from blocks of 6 x 6 texels up (2.25 bits a texel or
        // less: BC3's 8 keep what they have) from 2 MB as RGBA8 (RX_VK_ASTC_BC_MB), the finer ones (UI, characters)
        // only from 64 MB)
        static const uint64_t low = (uint64_t)(getenv("RX_VK_ASTC_BC_MB") ? atoi(getenv("RX_VK_ASTC_BC_MB")) : 2) << 20;
        // (issue 519: on a card the driver gives under 6 GB, the finer ones too. An RTX 3050 Laptop (4 GB, 3.3 allowed)
        // held 1.75 GB of decoded ASTC in Goron City, drawn every frame: 5.6 GB used of 3.3, paged, 4 fps. As BC3, a
        // quarter. Now 8 GB cards too: astc_bc_policy.h. RX_VK_ASTC_BC_ALL=1 or 0 says so on any card)
        static const bool all = [] {
            if (const char* e = getenv("RX_VK_ASTC_BC_ALL")) return e[0] == '1';
            uint64_t used = 0, allowed = 0;
            const bool small = vram_usage(&used, &allowed) && astc_bc_policy::all_by_vram(allowed);
            if (small) logf("vk: %llu MB of VRAM allowed: all large ASTC textures kept as BC3", (unsigned long long)(allowed >> 20));
            return small;
        }();
        const uint64_t min_bytes = (bw * bh >= 36 || all) ? low : 64ull << 20;
        uint64_t px = 0;
        for (uint32_t l = 0; l < x.levels; l++)
            px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l);
        // (not one with its top levels left out and a single layer: copies into those levels are blitted into its
        // first, which BC3 cannot take; trimmed, it is small)
        // (hookshot-amd) AMD's drivers sample BC levels wrong when a level's block grid is not level 0's halved: 1365 texels
        // are 342 blocks, but level 2 (341 texels) is 86 blocks, not 342 >> 2 = 85. Ocarina's terrain material arrays
        // (1365 x 1365 x 90) then draw a grid of dark lines along the terrain's seams, worse in the distance (Lichii #448 found
        // it on an RX 6600 XT; an RX 6700 XT here). On AMD such a texture is encoded at the nearest size whose blocks halve
        // evenly down to its 16-texel levels (1365 -> 1536, 683 -> 768; 768 and 1536 stay) and keeps only the levels that
        // halve evenly: the game samples with normalized coordinates, so a resized level covers the same area. Where that
        // would grow it by more than a quarter, or past the scratch, it stays RGBA8. RX_VK_UNEVEN_RGBA8=1: RGBA8 instead of resizing.
        // OFF by default since v2.0.6: in v2.0.5 an RX 6700 XT's GPU time went from 10-13 ms to 70-100 ms a frame in play
        // with it (issue 823, logs of both versions), and AMD players saw rainbow ground (Discord). RX_VK_UNEVEN_BC=0: on
        // (every card); unset or 1: BC3 as in v2.0.4.
        using bc_levels::first_uneven;
        using bc_levels::even_size;
        const bool uneven = first_uneven(x.width, x.height, x.levels) < x.levels;
        static const bool amd_fix = [] {
            if (const char* e = getenv("RX_VK_UNEVEN_BC")) return e[0] == '0';
            return false;
        }();
        static const bool rgba8_instead = env_on("RX_VK_UNEVEN_RGBA8");
        const bool bc_candidate = astc && !(x.skip && x.layers == 1) && x.type == VK_IMAGE_TYPE_2D &&
                                  x.samples == VK_SAMPLE_COUNT_1_BIT && x.width <= kBcDim && x.height <= kBcDim &&
                                  px * x.layers * 4 >= min_bytes;
        if (bc_candidate && uneven && amd_fix) {
            const uint32_t ew = even_size(x.width, x.levels), eh = even_size(x.height, x.levels);
            const uint32_t el = first_uneven(ew, eh, x.levels);
            const bool resize = ew != x.width || eh != x.height;
            static int n_resized, n_trimmed, n_rgba8;
            static uint64_t mb_saved;
            if (!rgba8_instead && ew <= kBcDim && eh <= kBcDim && ew * 4 <= x.width * 5 && eh * 4 <= x.height * 5 &&
                el >= 1 && bc_ready()) {
                uint64_t epx = 0;
                for (uint32_t l = 0; l < el; l++) epx += (uint64_t)std::max(1u, ew >> l) * std::max(1u, eh >> l);
                mb_saved += (px * x.layers * 4 - epx * x.layers) >> 20;
                int& n = resize ? n_resized : n_trimmed;
                n++;
                if (n <= 4 || (n & (n - 1)) == 0)
                    logf("vk: texture %#llx %ux%u x%u, %u levels: uneven BC block levels on AMD, as BC3 at %ux%u, %u levels "
                         "(%d resized, %d with fewer levels, %d as RGBA8; %llu MB less than RGBA8 so far)",
                         (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, ew, eh, el, n_resized, n_trimmed,
                         n_rgba8, (unsigned long long)mb_saved);
                x.width = ew, x.height = eh, x.levels = el;
                x.bc = true, x.bc_resized = resize;
                x.format = bc7_enabled() ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
            } else {
                n_rgba8++;
                if (n_rgba8 <= 4 || (n_rgba8 & (n_rgba8 - 1)) == 0)
                    logf("vk: texture %#llx %ux%u x%u, %u levels: uneven BC block levels on AMD, kept as RGBA8 (%d so far)",
                         (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, n_rgba8);
            }
        } else if (bc_candidate && bc_ready()) {
            x.bc = true;
            x.format = bc7_enabled() ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        }
    }
    const VkFormatProperties fp = format_props(x.format);
    x.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateFlags flags = 0;
    if (depth) {
        x.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        // (any format of the class may be a view: attachments and storage where one of them can)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (x.samples == VK_SAMPLE_COUNT_1_BIT) x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else {
        // Raw storage views address one compressed block per texel (8 or 16 bytes).
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT |
                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        const VkFormat raw = bpb == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;
        VkImageFormatProperties limits{};
        // (asked without STORAGE: AMD's driver checks that against the compressed format and says no, though it makes
        // the image and its raw views (RX 6600 XT, amdvlk); without them the game's BC encoders wrote nowhere and
        // Link's clothes showed stale blocks. If the image is then refused, it is made again without)
        // (RX_VK_BC_STORAGE=0: compressed images without it, as before v2.0.8 on AMD: issue 1278, an RX 6700 XT's GPU time 6-12 ms
        // a frame on v2.0.6 and 60-100 ms on v2.0.11, the game's BC encoders running there since 04675cf being the lead)
        static const bool bc_storage = !getenv("RX_VK_BC_STORAGE") || atoi(getenv("RX_VK_BC_STORAGE")) != 0;
        if (bc_storage && (format_props(raw).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            vkGetPhysicalDeviceImageFormatProperties(dev.physical, x.format, x.type, VK_IMAGE_TILING_OPTIMAL,
                x.usage, flags, &limits) == VK_SUCCESS)
            x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        // Render to raw block views of 2D images. A 3D image's attachments use a separate uncompressed mip.
        // On AMD drivers (amdvlk), queries against the compressed format return VK_ERROR_FORMAT_NOT_SUPPORTED (-11),
        // but raw block-texel views can be attached with EXTENDED_USAGE; a refused image is made again without the usage below (#1801).
        const bool is_amd = dev.props.vendorID == 0x1002;
        if (x.type != VK_IMAGE_TYPE_3D && (format_props(raw).optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            const VkResult r = vkGetPhysicalDeviceImageFormatProperties(dev.physical, x.format, x.type, VK_IMAGE_TILING_OPTIMAL,
                x.usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, flags, &limits);
            if (r == VK_SUCCESS || is_amd) x.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            static int told;
            if (r != VK_SUCCESS && told++ < 4)
                logf("vk: BC image %ux%u (vk %d) as a raw colour attachment: the driver says %d%s", x.width, x.height,
                     (int)x.format, (int)r, is_amd ? " (AMD: taken)" : "");
        }
    }
    if (x.type == VK_IMAGE_TYPE_2D && x.layers >= 6 && x.width == x.height && x.samples == VK_SAMPLE_COUNT_1_BIT)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, x.cube = true;
    if (x.type == VK_IMAGE_TYPE_3D) {
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        if (dev.view_2d_of_3d) flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = x.type;
    ci.format = x.format;
    ci.extent = {x.width, x.height, x.depth};
    ci.mipLevels = x.levels;
    ci.arrayLayers = x.layers;
    ci.samples = x.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = x.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // (a render target's image, not compressed: from the pool when one of this create info waits there, above)
    const uint64_t pool_key = as_target && !x.sparse && pool_cap() && !is_bc(x.format) && !is_astc(x.format) &&
                                      (x.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
                                  ? pool_key_of(ci)
                                  : 0;
    x.pool_key = pool_key;
    x.create_flags = ci.flags;
    // (hookshot, RX_VK_COMPACT) an image that is not a render target's: from the textures' own pool, the one compacted
    const bool in_textures = compact_on() && !x.sparse && !(as_target && !is_bc(x.format) && !is_astc(x.format));
    if (x.sparse) {
        ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
        bool ok = vkCreateImage(dev.device, &ci, nullptr, &x.image) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(dev.device, x.image, &mr);
            uint32_t n = 0;
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, nullptr);
            std::vector<VkSparseImageMemoryRequirements> reqs(n);
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, reqs.data());
            ok = false;
            for (const auto& r : reqs)
                if (r.formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) x.sparse_req = r, ok = true;
            x.sparse_align = mr.alignment;
            x.sparse_type = UINT32_MAX;
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && x.sparse_type == UINT32_MAX; i++)
                if ((mr.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    x.sparse_type = i;
            ok = ok && x.sparse_type != UINT32_MAX && x.sparse_align;
            if (!ok) vkDestroyImage(dev.device, x.image, nullptr), x.image = VK_NULL_HANDLE;
        }
        if (!ok) {
            logf("vk: sparse image %ux%u x%u levels %u format %d not made", x.width, x.height, x.layers, x.levels,
                 (int)x.format);
            return nullptr;
        }
        x.tiles.resize((size_t)x.layers * x.levels);
        x.tail_bound.assign(x.layers, 0);
        logf("vk: texture %#llx %ux%u x%u levels %u (NVN %#x): sparse, layers made resident as they are written "
             "(blocks %ux%u, mip tail from level %u, %llu KB%s)",
             (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, d.format,
             x.sparse_req.formatProperties.imageGranularity.width, x.sparse_req.formatProperties.imageGranularity.height,
             x.sparse_req.imageMipTailFirstLod, (unsigned long long)(x.sparse_req.imageMipTailSize >> 10),
             x.sparse_req.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? ", one for all layers" : " a layer");
    } else if (in_textures && create_image(ci, x.image, x.allocation, false, false, true)) {
        // (made in the textures' pool; one refused there is made as any other, below)
    } else if (!(pool_key && pool_take(pool_key, x.image, x.allocation, x.views)) && (st_pool_made += pool_key != 0, true) &&
               !create_image(ci, x.image, x.allocation, false, as_target && !is_bc(x.format) && !is_astc(x.format)) &&
               // (a compressed image refused: made again without its raw attachment usage, then without its raw storage
               // usage, as #1332 does; its raw views are then not made, as before)
               !((ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && is_bc(x.format) &&
                 (ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, x.usage = ci.usage, create_image(ci, x.image, x.allocation))) &&
               !((ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) && (is_bc(x.format) || is_astc(x.format)) &&
                 (ci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT, x.usage = ci.usage, create_image(ci, x.image, x.allocation)))) {
        static int logged;
        if (logged++ < 16)
            logf("vk: image %ux%ux%u x%u levels %u format %d (NVN %#x) not made", x.width, x.height, x.depth, x.layers,
                 x.levels, (int)x.format, d.format);
        return nullptr;
    }
    // what it costs: whole blocks for native BC/ASTC and re-encoded BC3, texels for other images.
    // Re-encoded BC3 has 4x4, 16-byte blocks, not the source ASTC's block dimensions.
    const bool blocks = is_bc(x.format) || is_astc(x.format);
    const uint32_t unit_w = x.bc ? 4u : blocks ? (uint32_t)bw : 1u;
    const uint32_t unit_h = x.bc ? 4u : blocks ? (uint32_t)bh : 1u;
    uint64_t px = 0;
    for (uint32_t l = 0; l < x.levels; l++)
        px += texture_level_units(x.width >> l, x.height >> l, x.depth >> l, unit_w, unit_h);
    const uint32_t texel = x.bc ? 16 : astc ? 4 : std::max(1, from ? (from == 3 ? 4 : from == 6 ? 8 : 16) : bpb);
    x.bytes = x.sparse ? 0 : px * x.layers * texel * x.samples;   // (a sparse image's: as its layers are bound)
    // (the budget counts what the allocator gave it: the texels' 8% short of that over a scene's 2000 textures, 3228 MB counted for 3504
    // MB held, and a budget of 4035 MB took 4300. RX_VK_COUNT_BY_ESTIMATE=1: the texels)
    static const bool by_estimate = env_on("RX_VK_COUNT_BY_ESTIMATE");
    if (!x.sparse && !by_estimate) x.pad = std::max(x.bytes, allocation_size(x.allocation)) - x.bytes;
    total_bytes += x.bytes + x.pad;
    InitRedirect redirect;   // (a parallel replay's worker: into the earliest open segment's init command buffer)
    // into GENERAL, and cleared (a target the game renders into before writing it all reads zeros, not garbage)
    VkCommandBuffer cmd = upload_cmd();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = x.image;
    b.subresourceRange = {x.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    if (depth) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        const VkClearColorValue z{};
        vkCmdClearColorImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    }
    if (!is_bc(x.format) && !is_astc(x.format)) {
        // (the clear finished before anything after it in this command buffer writes the image: the upload of its contents
        // comes right after, a copy into the same image. Two transfer writes with no barrier between them may run in either
        // order; NVIDIA's ran in order, AMD's (a fast clear) landed last, so a texture uploaded in the recording that made it
        // read zeros there. The menus' text is that case: the CPU writes each string's letters into a new R8 texture, which is
        // drawn once into the string's label, so every menu and title text was blank on AMD cards)
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    Texture* p = tex.get();
    redirect.done();
    static uint32_t ids;
    p->id = ++ids;
    p->first_seen = frame_no;
    st_made++;
    p->key_layout = key.layout, p->key_cls = key.cls;
    {   // (another image at this storage: what tells them apart; content one renders the other does not see)
        static int told;
        auto [a, e] = by_gpu.equal_range(p->gpu);
        for (auto it = a; it != e && told < 80; ++it) {
            const Texture* o = it->second;
            told++;
            logf("vk: texture #%u at %#llx beside #%u: %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d, "
                 "the other %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d%s",
                 p->id, (unsigned long long)p->gpu, o->id, p->width, p->height, p->depth, p->layers, p->levels,
                 key.layout, key.cls, d.format, d.target, o->width, o->height, o->depth, o->layers, o->levels,
                 o->key_layout, o->key_cls, o->nvn_format, o->d.target, o->gpu_written ? " (rendered)" : "");
        }
    }
    textures.emplace(key, std::move(tex));
    by_gpu.emplace(p->gpu, p);
    watch_tex(p, "made");
    if (remade_log_on()) {   // (one evicted not long ago, made again)
        std::lock_guard lk(evicted_mu);
        auto ev = evicted_at.find(evicted_key(p->gpu, p->width, p->height, p->nvn_format));
        if (ev != evicted_at.end()) {
            const uint64_t ago = frame_no - ev->second.frame;
            if (ago < 600) {
                remade[p] = ago;
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x made again %llu frames after it was evicted (%s then)", p->id,
                         (unsigned long long)p->gpu, p->width, p->height, p->nvn_format, (unsigned long long)ago,
                         ev->second.rendered ? "rendered" : "not rendered");
            }
            evicted_at.erase(ev);
        }
    }
    static int logged;
    if (logged++ < 40 || textures.size() % 500 == 0 || p->bytes > (64ull << 20))
        logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x -> vk %d%s (%zu textures, %llu MB)",
             (unsigned long long)p->gpu, p->width, p->height, p->depth, p->layers, p->levels, d.format, (int)p->format,
             astc ? " (ASTC decoded)" : "", textures.size(), (unsigned long long)(total_bytes >> 20));
    return p;
}

} // namespace

std::atomic<uint64_t> work_epoch{0};
namespace {
// (each thread's own: the display thread's present records barriers too)
thread_local VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;   // (the command buffer this thread's last full barrier went into, and the work count then)
thread_local uint64_t last_barrier_epoch = 0;
std::atomic<int64_t> st_barriers_made{0}, st_barriers_left_out{0};
} // namespace
void barrier_stats(int64_t* made, int64_t* left_out) {
    *made = st_barriers_made.load(std::memory_order_relaxed), *left_out = st_barriers_left_out.load(std::memory_order_relaxed);
}

// (hookshot) A full barrier right after another one on the same command buffer, no command that does or orders GPU work in between
// (vk_work.h counts them), orders nothing the first did not: left out. The game's own barrier (end of a pass), then the next pass's
// start barrier, then a dispatch's, were two or three in a row. RX_VK_BARRIER_DEDUPE_OFF=1: all recorded, as before.
void full_barrier(VkCommandBuffer cmd, const char* why) {
    static const bool off = env_on("RX_VK_BARRIER_DEDUPE_OFF");
    const bool dedupe = !off && !par_active.load(std::memory_order_relaxed);   // (a parallel replay's threads share command buffers and the count)
    if (dedupe && cmd == last_barrier_cmd && work_epoch.load(std::memory_order_relaxed) == last_barrier_epoch) {
        st_barriers_left_out.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    st_barriers_made.fetch_add(1, std::memory_order_relaxed);
    GprofSpan gp(cmd, why);   // (RX_VK_PASS_TIMING: what a barrier costs the GPU that has nothing left to wait for)
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    last_barrier_cmd = cmd, last_barrier_epoch = work_epoch.load(std::memory_order_relaxed);
}

namespace {
thread_local VkCommandBuffer last_pass_end_cmd = VK_NULL_HANDLE;   // (the command buffer this thread's last draw pass ended on, and the work count then)
thread_local uint64_t last_pass_end_epoch = 0;
std::atomic<int64_t> st_pass_relaxed{0};
} // namespace
void note_pass_end(VkCommandBuffer cmd) {
    last_pass_end_cmd = cmd, last_pass_end_epoch = work_epoch.load(std::memory_order_relaxed);
}
int64_t pass_barriers_relaxed() { return st_pass_relaxed.load(std::memory_order_relaxed); }
// (hookshot) RX_VK_GAME_ORDER: a draw pass right after another (nothing recorded between) waits in its fragment stages only
// (pass_start_barrier), and a compute dispatch right after another gets no barrier (vk_draw.cpp dispatch): their order is the
// game's own barriers', as on the console, where the game records nvnCommandBufferBarrier wherever a pass samples what an
// earlier one drew or a dispatch reads what another wrote, and the replay keeps each as a full barrier. A switch between draws
// and compute, the replay's own copies, clears, fills and uniform updates, an indirect dispatch, and the game's barriers keep
// the full barrier. Every pass begin and every dispatch was a full barrier: 2.3 ms of a 9.7 ms GPU frame in Kokiri Forest
// elsewhere, measured. On NVIDIA by default (the game's barriers were placed for an NVIDIA GPU), off on others, where it has
// not run: RX_VK_GAME_ORDER=1 there to try it, 0 off everywhere. RX_VK_PASS_BARRIER=game / full still set the pass part.
bool game_order() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_GAME_ORDER");
        const bool v = e ? !env_value_off(e) : dev.props.vendorID == 0x10de;
        logf("vk: passes and dispatches right after others ordered by the game's barriers: %s (RX_VK_GAME_ORDER%s)",
             v ? "on" : "off", e ? " set" : v ? "=0: off" : "=1: on");
        return v;
    }();
    return on;
}
void pass_start_barrier(VkCommandBuffer cmd) {
    static const bool game = [] {
        const char* e = getenv("RX_VK_PASS_BARRIER");
        const bool on = e ? !strcmp(e, "game") : game_order();
        if (on) logf("vk: RX_VK_PASS_BARRIER=game: a draw pass right after another, no barrier of the game's between, waits in its fragment stages only");
        return on;
    }();
    if (!game || par_active.load(std::memory_order_relaxed) || cmd != last_pass_end_cmd ||
        work_epoch.load(std::memory_order_relaxed) != last_pass_end_epoch) {
        full_barrier(cmd, "barrier (pass start)");
        return;
    }
    // (the previous pass's writes, all stages, made visible to this pass's fragment-side stages; its vertex, task and mesh stages run
    // as soon as they can, as on the console's GPU)
    st_pass_relaxed.fetch_add(1, std::memory_order_relaxed);
    GprofSpan gp(cmd, "barrier (pass start, fragments)");
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    last_barrier_cmd = VK_NULL_HANDLE;   // (not a full one: the next full barrier is not left out after it)
}

void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {
    last_barrier_cmd = VK_NULL_HANDLE;
    last_pass_end_cmd = VK_NULL_HANDLE;   // (a new recording: its command buffers start afresh, whatever their handles held)
    up_cmd = upload;
    if (value != rec_value) epoch++;   // (a submission's parts check a texture's memory once: nothing is collected between)
    rec_value = value;
    up_used = false;
    memory_table_begin();
    memory_rb_recording_begun();   // (which readback pages its uploads may refresh: memory_rb_refresh)
}
VkCommandBuffer upload_command_buffer() {
    return upload_cmd();
}
bool texture_uploads_recorded() {
    if (memory_rb_refresh(up_cmd)) up_used = true;   // (PR 430: readback pages from guest memory, last of the uploads)
    memory_table_flush();   // (vk_memory.cpp: the page table's changes, after this recording's mirror copies)
    if (up_used) full_barrier(up_cmd);   // (before the recording that reads them)
    return up_used;
}

int view_nvn_format(const nvn_tex* t, const nvn_view* v) {
    return v && v->set_format ? v->format : t->d.format;
}
unsigned view_level(const nvn_view* v) {
    return v && v->set_levels ? v->base_level : 0;
}
unsigned image_level(const Texture* tex, unsigned level) {
    return level > tex->skip ? std::min(level - tex->skip, tex->levels - 1) : 0;
}
unsigned view_layer(const nvn_view* v) {
    return v && v->set_layers ? v->min_layer : 0;
}

Texture* texture_get(const nvn_tex* t, bool for_sampling) {
    PROF_HOT(PROF_TEXGET);
    if (!t || !t->gpu || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return nullptr;
    const VkFormatInfo fi = vk_format(t->d.format);
    if (fi.format == VK_FORMAT_UNDEFINED) {
        static int seen[256];
        if (t->d.format >= 0 && t->d.format < 256 && seen[t->d.format]++ < 2)
            logf("vk: NVN format %#x has no Vulkan format: its textures are left out", t->d.format);
        return nullptr;
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return nullptr;
    Key k{};
    k.gpu = t->gpu;
    k.target = (uint32_t)t->d.target;
    // (a cube map and a 2D array of 6 layers over the same storage are one image; MS targets stay apart)
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_RECTANGLE) k.target = NVN_TEXTURE_TARGET_2D;
    k.width = t->d.width, k.height = t->d.height;
    k.depth = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : t->d.depth;
    if (k.target == NVN_TEXTURE_TARGET_2D) k.depth = 1;
    k.levels = std::max(1u, t->d.levels);
    k.samples = std::max(1, t->d.samples);
    k.layout = nvn_tex_is_linear(&t->d) ? 0x80000000u | (t->d.stride ? t->d.stride : host->tex_linear_stride(&t->d))
                                        : t->L.tileH | t->L.tileD << 4;
    k.cls = format_class(fi.format, t->d.format, bpb, fi.astc);
    Texture* tex;
    auto it = textures.find(k);
    if (it == textures.end()) {
        tex = create(k, t, fi.format, fi.astc, bw, bh, bpb, !for_sampling);
        if (!tex) return nullptr;
        static const int64_t log_from = getenv("RX_VK_LOG_MADE") ? atoll(getenv("RX_VK_LOG_MADE")) : -1;
        static int made_logged;
        if (log_from >= 0 && (int64_t)frame_no >= log_from && made_logged++ < 300) {
            auto [a, e] = by_gpu.equal_range(tex->gpu);
            int others = 0;
            for (auto it = a; it != e; ++it) others += it->second != tex;
            logf("vk: made #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d tileH %u, first %s; %d other images at it",
                 tex->id, (unsigned long long)tex->gpu, t->d.width, t->d.height, t->d.depth, tex->layers, t->d.levels,
                 t->d.format, t->d.target, t->L.tileH, for_sampling ? "sampled" : "rendered", others);
        }
    } else {
        tex = it->second.get();
    }
    return texture_touch(tex, for_sampling);
}

uint64_t texture_destroy_generation() {
    return destroy_gen;
}

uint32_t texture_epoch() {
    return epoch;
}

thread_local bool upload_defer_ok = false;
namespace {
/// (hookshot) A new ASTC texture a draw samples, met after this frame's uploads took their budget: not uploaded now, the draw
/// samples the placeholder, and the next frame's first look at it uploads it. An area's load made 300-800 ASTC uploads in one
/// frame, 200-600 ms of the replay thread (the "a frame's uploads took" lines); now that is spread over several frames, the
/// textures coming in a few frames late. ASTC only: those are the materials' colour, normal and mask textures; uncompressed
/// ones can be data (height maps, look-up tables) that a frame without would get wrong. RX_VK_UPLOAD_BUDGET_MS=<n>: the
/// budget (40: only an area load's burst; at 8 the ground's streamed textures waited a frame in ordinary play and the ground
/// flickered, Discord 2026-11-09), 0: no deferral, as before
bool upload_defer(const Texture& tex) {
    static const int64_t budget_ns = (int64_t)(getenv("RX_VK_UPLOAD_BUDGET_MS") ? atof(getenv("RX_VK_UPLOAD_BUDGET_MS")) : 40.0) * 1000000;
    if (!budget_ns || !upload_defer_ok || tex.uploaded || !tex.astc || tex.gpu_written) return false;
    if (frame_upload_ns.load(std::memory_order_relaxed) < budget_ns) return false;
    st_deferred.fetch_add(1, std::memory_order_relaxed), st_deferred_frame.fetch_add(1, std::memory_order_relaxed);
    return true;
}
}  // namespace
Texture* texture_touch(Texture* tex, bool for_sampling) {
    tex->last_frame = frame_no;
    tex->last_value = rec_value;
    if (!for_sampling) return tex;
    if (upload_defer(*tex)) return nullptr;
    // contents: from guest memory unless the GPU wrote the image (then it is the content, unless the texture was
    // finalized since and the CPU writes its memory again). What the CPU writes into it after the GPU's write is taken
    // all the same, those ranges alone, the rest staying the GPU's: Eden's texture cache takes a CPU write over the
    // GPU's contents, and MKW's font cache is cleared on the GPU with its glyphs written by the CPU as text appears
    // (the menus were missing every letter added later). Only whole pages of its storage: a write to another variable
    // sharing an edge page once re-uploaded garbage over a drawn LUT (the rainbow sky). Opt-in (RX_VK_CPU_OVER_GPU=1): transient render targets share memory the CPU writes for other things, and took that over their texels, and the menus' letters stayed missing with it.
    if (tex->gpu_written && !tex->released) {
        // (hookshot) and a texture the GPU only cleared: the game's font cache, whose letters the CPU writes as text appears. Its menu text
        // went missing at random (Hookshot issues 40 and 44), whenever the clear reached the image first
        static const bool take = env_on("RX_VK_CPU_OVER_GPU");
        if ((!take && !tex->only_cleared) || tex->checked_epoch == epoch) return tex;
        tex->checked_epoch = epoch;
        static thread_local Ranges cpu, inner;
        if (!texture_dirty(*tex, std::max(tex->upload_gen, tex->gpu_gen), cpu)) return tex;
        const uint64_t first = (0x1000 - (tex->gpu & 0xFFF)) & 0xFFF;
        const uint64_t last = ((tex->gpu + tex->guest_size) & ~0xFFFull) - tex->gpu;
        inner.clear();
        uint64_t bytes = 0;
        for (const auto& r : cpu) {
            const uint64_t a = std::max<uint64_t>(r.first, first), b = std::min<uint64_t>(r.second, last);
            if (a < b) inner.push_back({a, b}), bytes += b - a;
        }
        const uint32_t gen = memory_generation();
        if (!inner.empty() && upload(*tex, &inner)) {
            tex->upload_value = rec_value, tex->upload_seg = tl_seg;
            static int logged;
            if (logged++ < 20)
                logf("vk: texture %#llx %ux%u x%u NVN %#x: %llu KB the CPU wrote after the GPU taken", (unsigned long long)tex->gpu,
                     tex->width, tex->height, tex->layers, tex->nvn_format, (unsigned long long)(bytes >> 10));
        }
        tex->upload_gen = gen;
        return tex;
    }
    {   // (RX_VK_REUPLOAD_DIM=<w>x<h> (debugging): textures that size uploaded whole again every frame: is what a
        // sampled texture misses in guest memory, unseen by the write tracking, or not there at all?)
        static uint32_t rw = 0, rh = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_REUPLOAD_DIM")) sscanf(e, "%ux%u", &rw, &rh);
            return true;
        }();
        (void)parsed;
        static thread_local uint64_t last;
        if (rw && tex->width == rw && tex->height == rh && tex->uploaded && tex->checked_epoch != epoch &&
            last != frame_no) {
            last = frame_no;
            tex->checked_epoch = epoch;
            upload(*tex, nullptr);
            return tex;
        }
    }
    if (tex->uploaded && tex->upload_value == rec_value && tex->upload_seg > tl_seg) {
        // (a parallel replay: uploaded in this submission by a segment after this one, which reads it first in stream
        // order: uploaded again, the same data, in this segment's uploads)
        upload(*tex, nullptr);
        tex->upload_seg = tl_seg;
        return tex;
    }
    if (tex->uploaded && tex->checked_epoch == epoch) return tex;
    tex->checked_epoch = epoch;
    // (hookshot) an image whose last upload failed waits before the next try, before anything is checked or taken as seen:
    // a BC3 image's upload stages the whole ASTC source (some 200 MB for a terrain array), not to be repeated every
    // submission while the memory it needs is not there
    if (tex->retry_frame > frame_no) return tex;
    static thread_local Ranges dirty;
    static const bool no_partial = env_on("RX_VK_NO_PARTIAL");   // (A/B: whole re-uploads)
    // (a buffer the GPU wrote over its storage, done since: all of it again; the tiles of MKW's terrain are filled so
    // and stayed the flat placeholder they were made with)
    const uint64_t seen_before = tex->gpu_wrote_seen;   // (put back if the upload fails: the write is still to be taken)
    const bool gpu_dirty = tex->uploaded && gpu_wrote_into(*tex);
    const bool partial = !no_partial && !gpu_dirty && tex->uploaded && !tex->gpu_written && !tex->released;
    // (a texture the GPU drew and the game finalized: only what the CPU wrote since then. Its memory never got the
    // GPU's texels, and a write before (another variable in the same page) re-uploaded garbage over a drawn LUT:
    // the rainbow sky)
    const uint32_t since = tex->gpu_written && tex->released ? std::max(tex->upload_gen, tex->release_gen) : tex->upload_gen;
    if (tex->gpu_written && tex->released && frame_no > tex->last_written + 60) {
        // (hookshot: one the game finalized, sampled again well after it was drawn: what the eviction keeps them for)
        static std::unordered_set<uint32_t> told;
        if (told.size() < 200 && told.insert(tex->id).second)
            logf("vk: texture #%u %#llx %ux%u x%u NVN %#x the GPU drew, finalized by the game, sampled again at frame %llu (drawn at "
                 "%llu)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format,
                 (unsigned long long)frame_no, (unsigned long long)tex->last_written);
    }
    if (tex->uploaded && !gpu_dirty && !texture_dirty(*tex, since, dirty)) return tex;
    const uint32_t gen = memory_generation();
    // (storage render targets took last, which the CPU has not written since: nothing to upload, the memory holds
    // nothing the GPU's images did not replace. The game's transient heaps move with the dynamic resolution, and a
    // render target sampled before it is rendered at its new address (the culling reads last frame's shadow map)
    // was unswizzled from that memory: 55 MB for the 3072 x 3072 x 3 shadow map, frame after frame. Not "storage
    // the write watch never saw written": the game fills memory before it makes a pool of it.)
    static const bool upload_all = env_on("RX_VK_UPLOAD_ALL");   // (A/B: such storage uploaded too)
    // (from Shakoquo/Mako ff479e988d) never for a block-compressed texture (ASTC, BC, ETC): the GPU cannot render into one, so
    // its texels come from memory whatever render targets used that storage before. Skipped, a boss or a new enemy met for
    // the first time (its textures loaded into the transient heaps' memory) was drawn with an empty texture, all blue, for
    // the whole cutscene. RX_VK_SKIP_COMPRESSED_OVER_RT=1: as before
    static const bool skip_compressed = env_on("RX_VK_SKIP_COMPRESSED_OVER_RT");
    const bool compressed = tex->astc || tex->bc ||
                            (tex->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && tex->format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    uint32_t owned = !tex->uploaded && !upload_all && (skip_compressed || !compressed) ? gpu_owned_since(tex->gpu, tex->guest_size) : 0;
    if (!tex->uploaded && !upload_all && !skip_compressed && compressed) {
        static std::atomic<int> told{0};
        if (told.load(std::memory_order_relaxed) < 30 && gpu_owned_since(tex->gpu, tex->guest_size) && told++ < 30)
            logf("vk: texture #%u %#llx %ux%u x%u NVN %#x (compressed) over storage render targets took: uploaded from the game's "
                 "memory all the same", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers,
                 tex->nvn_format);
    }
    bool from_memory_taken = false;   // (put back if the upload fails)
    if (owned && !remade_skip_old()) {   // (one evicted that came from memory: uploaded from it again. Above)
        std::lock_guard lk(from_memory_mu);
        auto e = evicted_from_memory.find(evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format));
        if (e != evicted_from_memory.end() && e->second == tex->guest_size) {
            evicted_from_memory.erase(e);
            from_memory_taken = true;
            owned = 0;
            const uint64_t n = ++st_remade_uploaded;
            if (n <= 20 || n % 100 == 0)
                logf("vk: texture #%u %#llx %ux%u NVN %#x evicted, made again over storage render targets took: uploaded from "
                     "the game's memory (%llu so far)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height,
                     tex->nvn_format, (unsigned long long)n);
        }
    }
    watch_tex(tex, owned ? "sampled, over rendered storage" : "sampled, uploaded");
    if (owned && !texture_dirty(*tex, owned, dirty)) {
        tex->uploaded = true;
        tex->upload_gen = gen;
        watch_tex(tex, "NOT uploaded (over storage the GPU rendered)");
        if (remade_log_on()) {   // (made again after its eviction, and sampled empty)
            std::lock_guard lk(evicted_mu);
            auto r = remade.find(tex);
            if (r != remade.end()) {
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x, made again %llu frames after its eviction, sampled EMPTY "
                         "(storage the GPU rendered, not uploaded) at frame %llu", tex->id, (unsigned long long)tex->gpu,
                         tex->width, tex->height, tex->nvn_format, (unsigned long long)r->second,
                         (unsigned long long)frame_no);
                remade.erase(r);
            }
        }
        return tex;
    }
    if (partial && env_on("RX_VK_LOG_PARTIAL")) {
        static int logged;
        if (logged++ < 200) {
            char line[512];
            int n = snprintf(line, sizeof line, "vk: partial upload %#llx %ux%ux%u x%u levels %u NVN %#x size %llu flags %#x:",
                             (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                             tex->nvn_format, (unsigned long long)tex->guest_size, tex->d.flags);
            for (size_t i = 0; i < dirty.size() && i < 8 && n < 480; i++)
                n += snprintf(line + n, sizeof line - n, " [%#llx,%#llx)", (unsigned long long)dirty[i].first,
                              (unsigned long long)dirty[i].second);
            logf("%s", line);
        }
    }
    if (!tex->uploaded && tex->gpu >= (1ull << 40) && tex->page_map.empty()) {
        // (a sparse texture's mapping as it is uploaded first: its first check after does not take every page for
        // remapped, which uploaded the course maps twice)
        static thread_local Ranges first;
        texture_dirty(*tex, 0, first);
    }
    // (a sparse image, first or whole: only the storage ever written, not all of its gigabytes)
    const bool sparse_written = tex->sparse && !partial;
    if (sparse_written) texture_dirty(*tex, 0, dirty);
    if (tex->gpu_written) {   // (an image the GPU drew, finalized and sampled again: from guest memory, 400 lines)
        static int logged;
        uint64_t kb = 0;
        for (const auto& r : dirty) kb += (r.second - r.first) >> 10;
        if (logged++ < 400)
            logf("vk: upload over a drawn image: #%u %#llx %ux%ux%u x%u NVN %#x at present %llu (texture frame %llu), %s, "
                 "%llu KB dirty in %zu ranges%s", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth,
                 tex->layers, tex->nvn_format, (unsigned long long)(frame_no - 1), (unsigned long long)frame_no,
                 tex->released ? "finalized" : "live", (unsigned long long)kb, dirty.size(), gpu_dirty ? ", a GPU buffer wrote it" : "");
    }
    const auto up0 = std::chrono::steady_clock::now();
    const bool up_ok = upload(*tex, partial || sparse_written ? &dirty : nullptr);
    {   // (an upload over 15 ms: what it was, 60 of them)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - up0).count();
        static int slow_logged;
        if (ms > 15.0) programs_slow_upload(ms, tex->width, tex->height, tex->nvn_format);   // (RX_VK_LOG_PROGRAM_LIFE=1)
        if (ms > 15.0 && slow_logged++ < 60) {
            uint64_t part = 0;
            if (partial) for (const auto& r : dirty) part += r.second - r.first;
            logf("vk: slow upload %.1f ms: #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, %llu of %llu KB%s", ms, tex->id,
                 (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                 tex->nvn_format, tex->astc ? " ASTC" : "",
                 tex->bc ? (linear_twin(tex->format) == VK_FORMAT_BC7_UNORM_BLOCK ? " (BC7)" : " (BC3)") : "",
                 (unsigned long long)((partial ? part : tex->guest_size) >> 10), (unsigned long long)(tex->guest_size >> 10),
                 tex->uploaded ? " again" : "");
        }
    }
    if (up_ok) {
        tex->upload_value = rec_value, tex->upload_seg = tl_seg;
        // (its storage joins the submit-time capture: what the game writes into it after a submit is not taken for it)
        // (any size: the capture copies only the pages the CPU wrote, and the big CPU-streamed textures were read as the
        // game had rewritten them a frame or two later. RX_VK_TEX_CAPTURE_MAX_MB=<n>: only up to that, 8 before)
        static const uint64_t cap_max = getenv("RX_VK_TEX_CAPTURE_MAX_MB") ? (uint64_t)atoi(getenv("RX_VK_TEX_CAPTURE_MAX_MB")) << 20 : (2ull << 30);
        if (tex->gpu < (1ull << 40) && tex->guest_size < cap_max) memory_note_texture_upload(tex->gpu, tex->guest_size);
        if (verify_dim().first && tex->width == verify_dim().first && tex->height == verify_dim().second)
            tex->verify_hash = guest_hash(*tex, true);
        if (tex->uploaded) {
            static int logged;
            static std::unordered_map<uint64_t, int> big;
            if (logged++ < 20 || (tex->guest_size > (4u << 20) && big[tex->gpu]++ < 3)) {
                uint64_t part = 0, nz = 0, readable = 0;
                for (const auto& r : dirty) part += r.second - r.first;
                if (partial) {   // (what the CPU wrote: how much of it is not zero, and in which levels)
                    std::vector<uint8_t> b;
                    char lv[96] = "";
                    int lvn = 0;
                    for (const auto& r : dirty) {
                        b.resize((size_t)(r.second - r.first));
                        if (!host->gpu_read(tex->gpu + r.first, b.data(), b.size())) continue;
                        readable += b.size();
                        for (uint8_t c : b) nz += c != 0;
                        for (uint32_t l = 0; l < tex->levels && lvn < 90; l++) {
                            const uint64_t lo = host->tex_level_offset(&tex->d, l);
                            const uint64_t hi = l + 1 < tex->levels ? host->tex_level_offset(&tex->d, l + 1) : tex->guest_size;
                            if (r.first < hi && r.second > lo) lvn += snprintf(lv + lvn, sizeof lv - lvn, " L%u", l);
                        }
                    }
                    logf("vk:   (its written part: %llu of %llu KB readable, %llu bytes not zero; levels%s)",
                         (unsigned long long)(readable >> 10), (unsigned long long)(part >> 10), (unsigned long long)nz, lv);
                }
                logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x (%llu KB of storage) re-uploaded (its memory was "
                     "written; flags %#x): %s, %llu KB of it%s%s", (unsigned long long)tex->gpu, tex->width, tex->height,
                     tex->depth, tex->layers, tex->levels, tex->nvn_format, (unsigned long long)(tex->guest_size >> 10),
                     tex->d.flags, gpu_dirty ? "a GPU write into its storage" : partial ? "what the CPU wrote" : "whole",
                     (unsigned long long)((partial || sparse_written ? part : tex->guest_size) >> 10),
                     tex->gpu_written ? ", over what the GPU wrote" : "", tex->released ? " (finalized since)" : "");
            }
        }
        tex->uploaded = true;
        tex->upload_gen = gen;
        tex->gpu_written = false;
        tex->released = false;
        tex->retry_frame = 0;
    } else if (upload_never(*tex)) {
        tex->uploaded = true;   // (nothing to try again)
        tex->upload_gen = gen;
    } else {
        // (hookshot) a BC3 image holds nothing until its encode has run. Marked uploaded, its blocks as the memory had them
        // were drawn as coloured noise, and never made again unless the game wrote its memory: the path, the trees and Saria
        // after an alt-tab on a 4 GB RX 5500M, its VRAM full and the textures evicted and made again. The other kinds the same
        // way: a first upload that failed (no staging memory, the VRAM or RAM full) left an empty image, and one that failed
        // again dropped what the CPU or a GPU buffer had written since. Nothing is marked done: uploaded, what it was
        // uploaded after and the GPU write it found stay as they were, and it is tried again 30 frames on
        tex->retry_frame = frame_no + 30;
        tex->gpu_wrote_seen = seen_before;
        if (from_memory_taken) {   // (still one that came from memory: the next try has to find it so again)
            std::lock_guard lk(from_memory_mu);
            evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;
        }
        static std::atomic<int> told{0};
        if (told++ < 40) {
            if (tex->bc)
                logf("vk: BC3 texture #%u %#llx %ux%u x%u: its upload failed (%s): tried again in 30 frames", tex->id,
                     (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, astc_fail ? astc_fail : "unknown");
            else
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x: its upload failed: tried again in 30 frames", tex->id,
                     (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format);
        }
    }
    return tex;
}

VkImageView texture_view(Texture* tex, const nvn_tex* t, const nvn_view* v, VkImageViewType type, ViewUse use,
                         VkFormat* format_out, VkFormat force_format) {
    if (!tex) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    const VkFormat f = view_format_of(tex->format, tex->astc, tex->bc, nf, use, force_format);
    if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    const bool depth = is_depth_format(tex->format);

    const uint32_t vl = view_level(v);
    uint32_t level0 = image_level(tex, vl);
    uint32_t nlevels = v && v->set_levels && v->nlevels ? (vl + v->nlevels > tex->skip ? vl + v->nlevels - tex->skip - level0 : 1)
                                                       : tex->levels - level0;
    nlevels = std::clamp<uint32_t>(nlevels, 1, tex->levels - level0);
    const uint32_t total_layers = tex->type == VK_IMAGE_TYPE_3D ? 1 : tex->layers;
    uint32_t layer0 = std::min(view_layer(v), total_layers - 1);
    uint32_t nlayers = v && v->set_layers && v->nlayers ? v->nlayers : total_layers - layer0;
    nlayers = std::clamp<uint32_t>(nlayers, 1, total_layers - layer0);
    VkImageAspectFlags aspect = tex->aspect;
    if (use != ViewUse::Attachment && aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (use == ViewUse::Attachment) {
        nlevels = 1, nlayers = 1;
        type = tex->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
        // (a slice of a 3D target: a 2D view of it, 2D_ARRAY_COMPATIBLE, the slice as its layer)
        if (tex->type == VK_IMAGE_TYPE_3D) layer0 = std::min(view_layer(v), std::max(1u, tex->depth >> level0) - 1);
    } else if (use == ViewUse::Storage) {
        nlevels = 1;
        if (tex->type == VK_IMAGE_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_3D, layer0 = 0, nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_2D && nlayers > 1) nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_1D && nlayers > 1) nlayers = 1;
    }
    // the shader's type over the image's: what Vulkan allows of them
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        if (tex->type != VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_1D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if (tex->type == VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (tex->type == VK_IMAGE_TYPE_3D) {
            // (a 2D sample instruction over a 3D texture, as Eden's recompiler keeps the instruction's type: its first
            // slice, one level (VK_EXT_image_2d_view_of_3d). MKW reads a 64 x 64 x 64 texture so, which was sampled
            // as the dummy)
            if (use == ViewUse::Sample && dev.view_2d_of_3d) {
                type = VK_IMAGE_VIEW_TYPE_2D, nlevels = 1, layer0 = 0, nlayers = 1;
                break;
            }
            if (use != ViewUse::Attachment) return VK_NULL_HANDLE;
        }
        if (type == VK_IMAGE_VIEW_TYPE_2D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_3D:
        if (tex->type != VK_IMAGE_TYPE_3D) return VK_NULL_HANDLE;
        layer0 = 0, nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if (!tex->cube) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlayers = 6;
        else nlayers = std::max(6u, nlayers / 6 * 6);
        if (layer0 + nlayers > tex->layers) return VK_NULL_HANDLE;
        break;
    default: break;
    }
    if (tex->type == VK_IMAGE_TYPE_3D && use == ViewUse::Attachment) type = VK_IMAGE_VIEW_TYPE_2D;
    VkComponentMapping cm{};
    if (use == ViewUse::Sample) {
        const nvn_texdesc& d = t->d;
        unsigned sw[4] = {d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a};
        if (v && v->set_swizzle)
            for (int i = 0; i < 4; i++) sw[i] = v->swz[i];
        // (X formats read alpha as one)
        switch (nf) {
        case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
        case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN: case NVN_FORMAT_RGBX16UI:
        case NVN_FORMAT_RGBX16I: case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
        case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
            for (unsigned& s : sw)
                if (s == NVN_TEXTURE_SWIZZLE_A) s = NVN_TEXTURE_SWIZZLE_ONE;
            break;
        default: break;
        }
        static const bool swap_9e5 = env_on("RX_VK_9E5_SWAP");   // (hookshot, diagnosis: R and B swapped)
        if (swap_9e5 && nf == NVN_FORMAT_RGB9E5F) std::swap(sw[0], sw[2]);
        cm = {component(sw[0]), component(sw[1]), component(sw[2]), component(sw[3])};
        static const bool log_swizzle = env_on("RX_VK_LOG_SWIZZLE");   // (hookshot: RGB9E5 and R11G11B10 views)
        static int swizzle_logged;
        if (log_swizzle && nf == NVN_FORMAT_RGB9E5F && swizzle_logged++ < 12)
            logf("vk: sampled view of #%u NVN %#x as vk %d: swizzle %u %u %u %u (descriptor %u %u %u %u, view %d)", tex->id,
                 nf, (int)f, sw[0], sw[1], sw[2], sw[3], d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a,
                 v && v->set_swizzle ? 1 : 0);
    }
    uint64_t key = mix((uint64_t)use, (uint64_t)type);
    key = mix(key, (uint64_t)f << 32 | aspect);
    key = mix(key, (uint64_t)level0 << 48 | (uint64_t)nlevels << 32 | (uint64_t)layer0 << 16 | nlayers);
    key = mix(key, (uint64_t)cm.r << 24 | (uint64_t)cm.g << 16 | (uint64_t)cm.b << 8 | cm.a);
    if (format_out) *format_out = f;
    const VkImageUsageFlags usage = use == ViewUse::Sample ? VK_IMAGE_USAGE_SAMPLED_BIT
                                  : use == ViewUse::Storage ? VK_IMAGE_USAGE_STORAGE_BIT
                                  : depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return cached_image_view(tex, key, [&](VkResult* result) {
        const VkImageView view = make_view(tex, type, f, aspect, level0, nlevels, layer0, nlayers, cm, usage, result);
        if (!view) {
            static int logged;
            if (logged++ < 16)
                logf("vk: view of texture %#llx (%ux%u, vk %d, usage %#x) as vk %d type %d usage %#x "
                     "levels %u+%u layers %u+%u not made",
                     (unsigned long long)tex->gpu, tex->width, tex->height, (int)tex->format, tex->usage,
                     (int)f, (int)type, usage, level0, nlevels, layer0, nlayers);
        }
        return view;
    });
}

TextureAttachment texture_attachment(Texture* tex, const nvn_tex* t, const nvn_view* v) {
    TextureAttachment out;
    if (tex->type != VK_IMAGE_TYPE_3D || !is_bc(tex->format)) {
        out.view = texture_view(tex, t, v, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Attachment, &out.format);
        return out;
    }

    // NVIDIA's Pascal driver reports support for attaching a 2D raw view of a BC volume, then loses the device
    // even on a clear. Render the encoded blocks into an uncompressed mip and copy them to the sampled volume.
    const int nf = view_nvn_format(t, v);
    const VkFormat format = view_format_of(tex->format, tex->astc, tex->bc, nf, ViewUse::Attachment, VK_FORMAT_UNDEFINED);
    int bw, bh, widened_from;
    int bytes = nvn_block_size(nf, &bw, &bh);
    widened(vk_format(nf).format, &widened_from);
    if (widened_from) bytes = widened_from * 4 / 3;
    if (format == VK_FORMAT_UNDEFINED || bw != 1 || bh != 1 || bytes != tex->bpb ||
        !(format_props(format).optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        return out;

    const uint32_t level = image_level(tex, view_level(v));
    const uint32_t depth = std::max(1u, tex->depth >> level);
    const uint32_t slice = std::min(view_layer(v), depth - 1);
    const uint64_t key = mix(mix(0xbc3d, format), (uint64_t)level << 32 | slice);
    auto attachment = std::find_if(tex->block_attachments.begin(), tex->block_attachments.end(), [&](const auto& a) {
        return a.level == level && a.format == format;
    });
    if (attachment == tex->block_attachments.end()) {
        if (tex->view_failed_frame == frame_no) return out;
        // Each compressed mip can round up to a different block extent, so it has its own uncompressed image
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.format = format;
        ci.extent = {(std::max(1u, tex->width >> level) + 3) / 4, (std::max(1u, tex->height >> level) + 3) / 4, depth};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Texture::BlockAttachment made{VK_NULL_HANDLE, nullptr, format, level};
        if (!create_image(ci, made.image, made.allocation)) {
            tex->view_failed_frame = frame_no;
            return out;
        }
        try {
            tex->block_attachments.push_back(made);
        } catch (const std::exception&) {
            destroy_image(made.image, made.allocation);
            tex->view_failed_frame = frame_no;
            return out;
        }
        attachment = std::prev(tex->block_attachments.end());
        const uint64_t bytes = allocation_size(made.allocation);
        tex->pad += bytes;
        total_bytes += bytes;

        InitRedirect redirect;
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = made.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(upload_cmd(), &dependency);
    }

    out.view = cached_image_view(tex, key, [&](VkResult* result) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = attachment->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slice, 1};
        VkImageView view = VK_NULL_HANDLE;
        *result = vkCreateImageView(dev.device, &vi, nullptr, &view);
        return *result == VK_SUCCESS ? view : VK_NULL_HANDLE;
    });
    out.format = format;
    out.blocks = attachment->image;
    return out;
}

void copy_block_attachment(VkCommandBuffer cmd, const Texture& tex, VkImage blocks, uint32_t level, uint32_t slice,
                           bool to_attachment) {
    const uint32_t width = std::max(1u, tex.width >> level), height = std::max(1u, tex.height >> level);
    const VkImageSubresourceLayers compressed{VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    const VkImageSubresourceLayers raw{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkOffset3D offset{0, 0, (int32_t)std::min(slice, std::max(1u, tex.depth >> level) - 1)};
    // Vulkan takes the extent in source texels, which are blocks in the uncompressed attachment
    const VkImageCopy region{to_attachment ? compressed : raw, offset, to_attachment ? raw : compressed, offset,
                             to_attachment ? VkExtent3D{width, height, 1} : VkExtent3D{(width + 3) / 4, (height + 3) / 4, 1}};
    vkCmdCopyImage(cmd, to_attachment ? tex.image : blocks, VK_IMAGE_LAYOUT_GENERAL,
                   to_attachment ? blocks : tex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

bool texture_copy_into_bc(VkCommandBuffer cmd, Texture* src, uint32_t src_level, uint32_t src_layer, int sx, int sy,
                          Texture* dst, uint32_t dst_level, uint32_t dst_layer, int dx, int dy, uint32_t w, uint32_t h,
                          uint32_t layers) {
    if (!src || !dst || !dst->bc || !src->astc || src->bc || !bc_ready() || w > kBcDim || h > kBcDim || (dx & 3) || (dy & 3))
        return false;
    // (hookshot-amd) into a resized BC3 image only whole levels, resized as their upload is
    const uint32_t ow = std::max(1u, dst->width >> dst_level), oh = std::max(1u, dst->height >> dst_level);
    if (dst->bc_resized) {
        const uint32_t gl = dst_level + dst->skip;
        const uint32_t gw = std::max(1u, dst->d.width >> gl), gh = std::max(1u, dst->d.height >> gl);
        if (dx || dy || w != gw || h != gh) {
            static int logged;
            if (logged++ < 8)
                logf("vk: a copy into part of resized BC3 texture %#llx (level %u, %d,%d %ux%u of %ux%u) left out",
                     (unsigned long long)dst->gpu, dst_level, dx, dy, w, h, gw, gh);
            return false;
        }
    }
    if (!bc_scratch_layout(cmd, dst->bc_resized)) return false;
    for (uint32_t l0 = 0; l0 < layers; l0 += kBcLayers) {
        const uint32_t n = std::min(kBcLayers, layers - l0);
        full_barrier(cmd);
        const VkImageCopy cp{{VK_IMAGE_ASPECT_COLOR_BIT, src_level, src_layer + l0, n}, {sx, sy, 0},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, bc_scratch, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        if (dst->bc_resized)
            bc_encode_resized(cmd, w, h, ow, oh, n, *dst, dst_level, dst_layer + l0);
        else
            bc_encode_into(cmd, w, h, n, *dst, dst_level, dst_layer + l0, dx, dy);
    }
    return true;
}

// ---- what the GPU wrote into textures of virtual pools, back into their memory ----
// MK World draws its course maps' tiles (the terrain's height and material maps, 16384 x 16384 R8, 8192 x 8192 R16
// and RG16, sparse in virtual pools) into small 512 x 512 render targets in virtual pools of their own, finalizes
// those, unmaps their pools and maps the physical pages into the course map: on the console the texels are in that
// memory. Here they were only in the small targets' images, the course maps read zeros, and the terrain they shape
// was missing in places (ground drawn nowhere, the sky through it, snow left floating). Before virtual pages are
// unmapped or remapped (nvnMemoryPoolMapVirtual, nvnMemoryPoolFinalize: host/nvn_native.c), the textures the GPU
// wrote there are read back and written into their memory, block linear: the course maps then upload the tiles as
// they are mapped. The same when the CPU is about to read memory the GPU may have written (nvnMemoryPoolInvalidate-
// MappedRange, nvnBufferInvalidateMappedRange): MK World reads back a 1 x 1 RGBA32F texture its GPU computes (the
// scene's luminance), which was never in memory. RX_VK_NO_VMAP_FLUSH=1: off.
namespace {
constexpr uint64_t kFlushMax = 16ull << 20;   // (the small ones: render targets that size and more are not)
std::mutex vw_mu;
std::map<uint64_t, uint64_t> vw;   // (storage start -> end of the GPU-written textures of virtual pools that may be flushed)
bool flushable(const Texture& t) {
    if (!t.guest_size || t.guest_size > kFlushMax || t.astc || t.bc || t.sparse || t.skip ||
        t.samples != VK_SAMPLE_COUNT_1_BIT || t.aspect != VK_IMAGE_ASPECT_COLOR_BIT || t.bpb <= 0 || !t.image ||
        (nvn_tex_is_linear(&t.d) && (t.levels != 1 || t.type == VK_IMAGE_TYPE_3D)))   // (linear: one level, rows)
        return false;
    int from = 0;
    widened(t.d.format ? vk_format(t.nvn_format).format : t.format, &from);
    return !from;   // (the image's texels as the storage's: no 3-byte formats kept as four)
}
void vw_note(Texture& t) {
    static const bool off = env_on("RX_VK_NO_VMAP_FLUSH");
    if (off || t.vw_listed || !flushable(t)) return;
    std::lock_guard lk(vw_mu);
    uint64_t& end = vw[t.gpu];
    end = std::max(end, t.gpu + t.guest_size);
    t.vw_listed = true;
}
void vw_forget(const Texture& t) {   // (the entry goes; other listed textures at that start stay listed)
    if (!t.vw_listed) return;
    std::lock_guard lk(vw_mu);
    vw.erase(t.gpu);
    auto [a, b] = by_gpu.equal_range(t.gpu);
    for (auto it = a; it != b; ++it)
        if (it->second != &t && it->second->vw_listed) {
            uint64_t& end = vw[t.gpu];
            end = std::max(end, t.gpu + it->second->guest_size);
        }
}
// (a command buffer of its own for the read-backs, submitted and waited for at once)
VkCommandPool rb_pool = VK_NULL_HANDLE;
VkCommandBuffer rb_cmd = VK_NULL_HANDLE;
VkFence rb_fence = VK_NULL_HANDLE;
bool rb_pending = false;   // (a submission whose wait timed out: the GPU may still have its command buffer and fence)
bool readback_begin() {
    if (rb_pending) {   // (not reset, nor begun again, while pending: waited for first, or this read-back is not made)
        if (vkWaitForFences(dev.device, 1, &rb_fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) return false;
        rb_pending = false;
    }
    if (!rb_pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = dev.queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &rb_pool) != VK_SUCCESS) return rb_pool = VK_NULL_HANDLE, false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = rb_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(dev.device, &ai, &rb_cmd) != VK_SUCCESS ||
            vkCreateFence(dev.device, &fi, nullptr, &rb_fence) != VK_SUCCESS) {
            // (made again next time, not used half made: a null rb_cmd or rb_fence with rb_pool set)
            vkDestroyCommandPool(dev.device, rb_pool, nullptr);
            rb_pool = VK_NULL_HANDLE, rb_cmd = VK_NULL_HANDLE, rb_fence = VK_NULL_HANDLE;
            return false;
        }
    }
    vkResetCommandPool(dev.device, rb_pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(rb_cmd, &bi) == VK_SUCCESS;
}
/// (hookshot, RX_VK_FRAME_TRACE) The time spent in a read-back's submit and wait for the GPU (the write-backs before an
/// eviction, a flush to guest memory), and in texture_frame as a whole: neither was in any of the trace's columns. An area
/// change had frames of 344 and 528 ms with every column near zero, the replay inside the driver in this wait for 317 ms
std::atomic<int64_t> st_readback_wait_ns{0}, st_texture_frame_ns{0};
struct TraceTimer {
    std::atomic<int64_t>& into;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~TraceTimer() {
        into.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                       std::memory_order_relaxed);
    }
};
bool readback_submit_wait() {
    const TraceTimer timed{st_readback_wait_ns};
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (the copies' writes, for the host)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(rb_cmd, &d);
    if (vkEndCommandBuffer(rb_cmd) != VK_SUCCESS) return false;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = rb_cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    vkResetFences(dev.device, 1, &rb_fence);
    {
        std::lock_guard qk(dev.present_mu);   // (one queue: submits and presents take turns)
        queue_flush_locked();
        if (vkQueueSubmit2(dev.queue, 1, &si, rb_fence) != VK_SUCCESS) return false;
    }
    rb_pending = vkWaitForFences(dev.device, 1, &rb_fence, VK_TRUE, 5000000000ull) != VK_SUCCESS;
    return !rb_pending;
}
/// A texture's write-back: its image's levels and layers read back, swizzled as the storage lays them out, and written into
/// its memory within [clip_lo, clip_hi). The GPU half (flush_record) goes into rb_cmd, the CPU half (flush_finish) runs once
/// the read-back completed: flush_textures does a batch of them with one submission and one wait. Each wait drains the
/// queue, and the retirement sweep wrote back hundreds one by one at a scene load (seconds of stall)
struct Flush {
    Texture* t;
    uint64_t clip_lo, clip_hi;
    struct Part { uint32_t level, layer; uint64_t at, n; LevelGeo g; };
    std::vector<Part> parts;
    uint64_t total = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    bool ok = false;
    bool only_untouched = false;   // (drawn_writeback.h: only into memory still all zero, or holding its own last write-back)
    bool touched = false;          // (and it was neither, or a guest thread wrote it during the write: the CPU's, nothing written)
    uint64_t written_hash = 0;     // (and when written: the hash of its memory as the write left it)
};
// (cached host memory, which the CPU reads: the sequential-write kind is write-combined, or VRAM behind the BAR, and every read
// of it by the unswizzle goes over PCIe uncached. RX_VK_FLUSH_UNCACHED=1: as before)
bool flush_uncached() {
    static const bool uncached = env_on("RX_VK_FLUSH_UNCACHED");
    return uncached;
}
/// The copies of f's image into a staging buffer of its own, recorded into cmd (rb_cmd, begun by the caller, when none is
/// given); false: nothing recorded
bool flush_record(Flush& f, VkCommandBuffer cmd = VK_NULL_HANDLE) {
    if (!cmd) cmd = rb_cmd;
    Texture& t = *f.t;
    const uint32_t layers = t.type == VK_IMAGE_TYPE_3D ? 1 : t.layers;
    for (uint32_t level = 0; level < t.levels; level++) {
        const LevelGeo g = level_geo(t, level);
        for (uint32_t layer = 0; layer < layers; layer++) {
            const uint64_t n = (uint64_t)g.tw * g.th * g.td * (uint64_t)t.bpb;
            f.parts.push_back({level, layer, f.total, n, g});
            f.total += (n + 15) & ~15ull;
        }
    }
    if (!f.total) return f.ok = false;
    if (!(flush_uncached() ? create_buffer(f.total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, f.buf, f.alloc, &f.map)
                           : create_buffer_cached(f.total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, f.buf, f.alloc, &f.map)) || !f.map) {
        if (f.buf) destroy_buffer(f.buf, f.alloc);
        f.buf = VK_NULL_HANDLE, f.alloc = nullptr, f.map = nullptr;
        return f.ok = false;
    }
    std::vector<VkBufferImageCopy> c;
    for (const Flush::Part& p : f.parts) {
        VkBufferImageCopy r{};
        r.bufferOffset = p.at;
        r.bufferRowLength = p.g.tw * (uint32_t)t.block_w;
        r.bufferImageHeight = p.g.th * (uint32_t)t.block_h;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, p.level, t.type == VK_IMAGE_TYPE_3D ? 0 : p.layer, 1};
        r.imageExtent = {p.g.w, p.g.h, p.g.d};
        c.push_back(r);
    }
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, f.buf, (uint32_t)c.size(), c.data());
    return f.ok = true;
}
/// [gpu, gpu + n) of guest memory from `from` to `to`, only where it still holds `from` (drawn_writeback::swap_words; went
/// and only as there, n bytes each): false when a guest thread wrote some of it meanwhile, or it is not mapped
bool guest_swap(uint64_t gpu, const uint8_t* from, const uint8_t* to, uint64_t n, uint8_t* went = nullptr,
                const uint8_t* only = nullptr) {
    bool all = true;
    for (uint64_t at = 0; at < n;) {
        uint64_t avail = 0;
        uint8_t* p = host->gpu_ptr(gpu + at, &avail);
        if (!p || !avail) {
            all = false;
            break;
        }
        const uint64_t k = std::min(avail, n - at);
        all &= drawn_writeback::swap_words(p, from + at, to + at, (size_t)k, went ? went + at : nullptr, only ? only + at : nullptr) == 0;
        host->mark_written(p, k);   // (as host->gpu_write does: the soft write watch)
        at += k;
    }
    guest_cpu_wrote(gpu, n);
    return all;
}
/// The CPU half, once the batch's read-back completed (gpu_ok) or failed: the texels into the texture's memory, the buffer freed
bool flush_finish(Flush& f, bool gpu_ok) {
    using Part = Flush::Part;
    Texture& t = *f.t;
    const uint64_t clip_lo = f.clip_lo, clip_hi = f.clip_hi, total = f.total;
    const std::vector<Part>& parts = f.parts;
    void* map = f.map;
    bool ok = f.ok && gpu_ok;
    if (ok && !flush_uncached()) invalidate_mapped(f.alloc, total);
    // (only_untouched: the memory as it is now, whatever the page generations know of it yet. The texels are staged
    // against this snapshot and go in at the end, compared with it word by word: publish below)
    std::vector<uint8_t> was, staged;
    struct Piece { uint64_t gpu, at, n; };   // (staged[at, at + n) for guest memory at gpu)
    std::vector<Piece> pieces;
    if (ok && f.only_untouched) {
        was.resize((size_t)t.guest_size);
        if (!host->gpu_read(t.gpu, was.data(), was.size()) || !drawn_writeback::may_write(was.data(), was.size(), t.written_back_hash))
            ok = false, f.touched = true;
    }
    if (ok && total <= 64) {   // (RX_VK_LOG_TINY_FLUSH=1: the values of tiny ones, 400 lines: a 1 x 1 luminance)
        static const bool log_tiny = env_on("RX_VK_LOG_TINY_FLUSH");
        static int logged;
        if (log_tiny && logged++ < 400) {
            const float* f = (const float*)map;
            const uint32_t* u = (const uint32_t*)map;
            logf("vk: tiny write-back #%u %#llx NVN %#x: %g %g %g %g (%08x %08x %08x %08x)", t.id, (unsigned long long)t.gpu,
                 t.nvn_format, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
        }
    }
    // (hookshot) pages the CPU wrote after the GPU last wrote the texture keep the CPU's bytes: on the console the GPU's
    // writes are in memory as it renders, and a later CPU write is what is there. OoT reuses a render target's memory
    // for scene data and then invalidates a range of it: this wrote the whole texture back over that data, and its
    // SceneLowPriority thread read texels as pointers (an access violation at 0xffffffffffffffff)
    std::vector<std::pair<uint64_t, uint64_t>> cpu_runs;   // (offsets from t.gpu)
    if (ok && t.gpu < (1ull << 40)) memory_written_runs(t.gpu, t.guest_size, t.gpu_gen, cpu_runs);
    uint64_t kept_cpu = 0;
    // (and only into the pages being remapped: those are what the texture's content would be lost from. The rest of it stays where
    // the game may have put other things since. The watch above does not catch all of those: OoT's SceneLowPriority crash, walking
    // in Kokiri Forest, was texels landing right beside the remapped pages, on scene data)
    const auto write_back = [&](uint64_t whole_gpu, const uint8_t* whole_src, uint64_t whole_n) {
        const uint64_t from = std::max(whole_gpu, clip_lo), to = std::min(whole_gpu + whole_n, clip_hi);
        if (from >= to) return true;
        const uint64_t gpu = from, n = to - from;
        const uint8_t* src = whole_src + (from - whole_gpu);
        const uint64_t lo = gpu - t.gpu, hi = lo + n;
        bool w = true;
        const auto put = [&](uint64_t off, const uint8_t* bytes, uint64_t len) {
            if (!f.only_untouched) return guest_cpu_write(t.gpu + off, bytes, len) != 0;
            pieces.push_back({t.gpu + off, staged.size(), len});
            staged.insert(staged.end(), bytes, bytes + len);
            return true;
        };
        uint64_t at = lo;
        for (const auto& [a, b] : cpu_runs) {
            if (b <= at) continue;
            if (a >= hi) break;
            if (a > at) w &= put(at, src + (at - lo), a - at);
            kept_cpu += std::min(b, hi) - std::max(a, at);
            at = std::max(at, b);
            if (at >= hi) break;
        }
        if (at < hi) w &= put(at, src + (at - lo), hi - at);
        return w;
    };
    // (only_untouched) the staged texels into memory: the snapshot once more, since the unswizzle took its time, then
    // each word only where it still is as the snapshot has it. A word a guest thread wrote meanwhile stays, and then
    // the words that did go in (those only) come out again: either all of the texture's texels are in its memory, or
    // the memory is as the guest has it
    const auto publish = [&] {
        if (!ok || !f.only_untouched) return;
        std::vector<uint8_t> again(was.size());
        bool same = host->gpu_read(t.gpu, again.data(), again.size()) && again == was;
        if (same) {
            std::vector<uint8_t> went(staged.size());   // (the bytes that went in: the undo takes out those only)
            for (const Piece& p : pieces)
                same &= guest_swap(p.gpu, was.data() + (p.gpu - t.gpu), staged.data() + p.at, p.n, went.data() + p.at);
            if (!same)
                for (const Piece& p : pieces)
                    guest_swap(p.gpu, staged.data() + p.at, was.data() + (p.gpu - t.gpu), p.n, nullptr, went.data() + p.at);
        }
        if (!same) {
            ok = false, f.touched = true;
            return;
        }
        for (const Piece& p : pieces) std::memcpy(again.data() + (p.gpu - t.gpu), staged.data() + p.at, (size_t)p.n);
        f.written_hash = drawn_writeback::hash(again.data(), again.size());
    };
    const auto log_kept = [&] {
        static int logged;
        if (kept_cpu && logged++ < 20)
            logf("vk: write-back of #%u %#llx: %llu KB the CPU wrote since the GPU kept", t.id, (unsigned long long)t.gpu,
                 (unsigned long long)(kept_cpu >> 10));
    };
    if (ok && nvn_tex_is_linear(&t.d)) {   // (a linear texture: its rows at the stride, one level)
        const uint64_t stride = t.d.stride ? t.d.stride : host->tex_linear_stride(&t.d);
        for (const Part& p : parts) {
            const uint64_t row = (uint64_t)p.g.tw * (uint64_t)t.bpb;
            const uint64_t base = (uint64_t)p.layer * t.L.layer_size;
            for (uint32_t y = 0; y < p.g.th; y++) {
                const uint64_t off = base + (uint64_t)y * stride;
                if (off + row > t.guest_size) break;
                if (!write_back(t.gpu + off, (const uint8_t*)map + p.at + (uint64_t)y * row, row)) ok = false;
            }
        }
        publish();
        log_kept();
        if (f.buf) destroy_buffer(f.buf, f.alloc);
        return ok;
    }
    if (ok) {
        std::vector<uint8_t> sw;
        for (const Part& p : parts) {
            const uint64_t at = t.gpu + p.g.offset + (uint64_t)p.layer * t.L.layer_size;
            if (p.g.offset + (uint64_t)p.layer * t.L.layer_size >= t.guest_size) continue;
            const uint64_t span = std::min<uint64_t>(p.g.swizzled, t.guest_size - p.g.offset - (uint64_t)p.layer * t.L.layer_size);
            sw.assign(std::max<uint64_t>(p.g.swizzled, span), 0);
            // (what is there: the padding between texels kept)
            if (f.only_untouched) std::memcpy(sw.data(), was.data() + (at - t.gpu), (size_t)span);
            else host->gpu_read(at, sw.data(), span);
            Tegra::Texture::SwizzleTexture(std::span<uint8_t>(sw.data(), sw.size()),
                                           std::span<const uint8_t>((const uint8_t*)map + p.at, p.n), (u32)t.bpb, p.g.tw,
                                           p.g.th, p.g.td, p.g.tile_h, p.g.tile_d, 1);
            if (!write_back(at, sw.data(), span)) ok = false;   // (marks the pages written: textures over them upload)
        }
        publish();
        log_kept();
    }
    if (f.buf) destroy_buffer(f.buf, f.alloc);
    return ok;
}
/// A batch of write-backs: one submission and one wait for all of them
void flush_textures(std::vector<Flush>& fs) {
    bool any = false;
    if (readback_begin()) {
        full_barrier(rb_cmd);   // (after what the submissions before wrote)
        for (Flush& f : fs) any |= flush_record(f);
    } else {
        for (Flush& f : fs) f.ok = false;
    }
    const bool gpu_ok = any && readback_submit_wait();
    if (any && !gpu_ok) {
        // (the read-back did not complete: its 5 s wait, which also covers what queue_flush_locked just submitted, or a submit
        // that failed. The copies may still be writing into the staging buffers, so they are not freed here: they go the way a
        // destroyed image does, once a submission made after this one is done. The batch's write-backs are lost, as a single
        // one's was before)
        static int logged;
        if (logged++ < 8)
            logf("vk: a batch of %zu texture write-backs did not complete in time: not written back, its staging buffers kept "
                 "until the GPU is past them", fs.size());
        for (Flush& f : fs)
            if (f.buf) {
                Dead d{rec_value + 1, VK_NULL_HANDLE, nullptr, {}, {}};
                d.buffer = f.buf, d.buffer_alloc = f.alloc;
                dead.push_back(std::move(d));
                f.buf = VK_NULL_HANDLE, f.alloc = nullptr, f.map = nullptr;
            }
    }
    for (Flush& f : fs) f.ok = flush_finish(f, gpu_ok);
}
bool flush_texture(Texture& t, uint64_t clip_lo, uint64_t clip_hi) {
    std::vector<Flush> one;
    one.push_back(Flush{&t, clip_lo, clip_hi});
    flush_textures(one);
    return one[0].ok;
}
// (hookshot) Small mip-chained textures the GPU drew in one frame: their texels into their memory, from two frames later
// (drawn_writeback.h has the rule and why: OoT copies a drawn terrain tile's memory on the CPU, and the copy was all
// zero). Nothing waits for the GPU: a frame's batch has its copies recorded with the present's uploads, and a later
// frame writes the texels once the GPU is past that recording (a wait here took the frame 3 to 8 ms, and 232 ms behind
// an area load's uploads). RX_VK_NO_DRAWN_WRITEBACK=1: as before
std::mutex drawn_mu;
std::vector<std::pair<uint64_t, Texture*>> drawn_noted;   // (storage, texture: drawn_mu)
struct DrawnBatch {
    uint64_t value;   // (the recording its copies are in)
    uint64_t frame;
    std::vector<Flush> fs;
    struct Was { uint32_t id; uint64_t last_written; };   // (each texture as it was then: the same one, not drawn since)
    std::vector<Was> was;
};
std::deque<DrawnBatch> drawn_batches;
constexpr size_t kDrawnBatches = 4;   // (open at once: with the GPU that far behind, the textures due wait)
bool drawn_writeback_on() {
    static const bool on = !env_on("RX_VK_NO_DRAWN_WRITEBACK");
    return on;
}
void drawn_writeback_note(Texture* tex) {
    if (tex->writeback_noted || !drawn_writeback_on() || !drawn_writeback::noted(tex->gpu, tex->guest_size, tex->levels)) return;
    tex->writeback_noted = true;
    std::lock_guard lk(drawn_mu);
    drawn_noted.push_back({tex->gpu, tex});
}
bool drawn_alive(uint64_t gpu, const Texture* t) {   // (not destroyed since it was listed)
    auto [a, b] = by_gpu.equal_range(gpu);
    for (auto it = a; it != b; ++it)
        if (it->second == t) return true;
    return false;
}
/// For each texture (nullptr: skipped), the newest drawing into another image whose storage overlaps its own; 0 for none
std::vector<uint64_t> drawn_others(const std::vector<const Texture*>& ts) {
    std::vector<uint64_t> other(ts.size(), 0);
    for (const auto& kv : textures) {   // (from any start: a scratch heap's targets overlap at other offsets)
        const Texture* o = kv.second.get();
        if (!o->gpu_written || !o->guest_size) continue;
        for (size_t i = 0; i < ts.size(); i++) {
            const Texture* t = ts[i];
            if (t && o != t && o->gpu < t->gpu + t->guest_size && o->gpu + o->guest_size > t->gpu)
                other[i] = std::max(other[i], o->last_written);
        }
    }
    return other;
}
/// The batches the GPU is done copying: their texels into memory, where the texture is still as it was copied
void drawn_writeback_finish() {
    static int64_t batches, done, failed, left_cpu, dropped, bytes;
    static double all_ms;
    while (!drawn_batches.empty() && drawn_batches.front().value <= completed_value) {
        DrawnBatch batch = std::move(drawn_batches.front());
        drawn_batches.pop_front();
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<const Texture*> ts;
        for (size_t i = 0; i < batch.fs.size(); i++) {
            const Texture* t = batch.fs[i].t;   // (clip_lo is its storage: the pointer is looked at only once found there)
            ts.push_back(drawn_alive(batch.fs[i].clip_lo, t) && t->id == batch.was[i].id ? t : nullptr);
        }
        const std::vector<uint64_t> other = drawn_others(ts);
        int64_t wrote = 0;
        const Texture* first = nullptr;
        for (size_t i = 0; i < batch.fs.size(); i++) {
            Flush& f = batch.fs[i];
            // (drawn or uploaded over since the copy, or another image drawn over its storage: the copy is not its content)
            if (!ts[i] || !f.t->gpu_written || f.t->only_cleared || f.t->last_written != batch.was[i].last_written ||
                (other[i] && other[i] >= f.t->last_written)) {
                if (f.buf) destroy_buffer(f.buf, f.alloc);
                dropped++;
                continue;
            }
            f.ok = flush_finish(f, true);
            f.t->written_back_for = f.t->last_written;   // (written, or the CPU's memory now: not tried again for this drawing)
            if (!f.ok) {
                (f.touched ? left_cpu : failed)++;
                continue;
            }
            done++, wrote++, bytes += (int64_t)f.t->guest_size;
            f.t->written_back_hash = f.written_hash;   // (its own texels there, for the next drawing's write-back)
            if (!first) first = f.t;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        all_ms += ms;
        if (batches++ < 12 || batches % 200 == 0)
            logf("vk: %lld of %zu small textures the GPU drew written into their memory in %.1f ms, %llu frames after their copies "
                 "were recorded, the first #%u %#llx %ux%u levels %u NVN %#x (%lld so far, %lld KB, %.0f ms; %lld left, the CPU wrote "
                 "their memory since; %lld dropped, drawn over or gone; %lld failed)", (long long)wrote, batch.fs.size(), ms,
                 (unsigned long long)(frame_no - batch.frame), first ? first->id : 0, (unsigned long long)(first ? first->gpu : 0),
                 first ? first->width : 0, first ? first->height : 0, first ? first->levels : 0, first ? first->nvn_format : 0,
                 (long long)done, (long long)(bytes >> 10), all_ms, (long long)left_cpu, (long long)dropped, (long long)failed);
    }
}
void drawn_writeback_frame() {
    drawn_writeback_finish();
    if (drawn_batches.size() >= kDrawnBatches) return;
    std::vector<std::pair<uint64_t, Texture*>> noted;
    {
        std::lock_guard lk(drawn_mu);
        noted.swap(drawn_noted);
    }
    if (noted.empty()) return;
    std::vector<std::pair<uint64_t, Texture*>> wait, cands;
    std::vector<const Texture*> ts;
    for (const auto& e : noted)
        if (drawn_alive(e.first, e.second)) cands.push_back(e), ts.push_back(e.second);
    const std::vector<uint64_t> other = drawn_others(ts);
    DrawnBatch batch{rec_value, frame_no, {}, {}};
    for (size_t i = 0; i < cands.size(); i++) {
        Texture* t = cands[i].second;
        const drawn_writeback::When when = drawn_writeback::due(frame_no, t->first_written, t->last_written, t->written_back_for,
                                                                t->gpu_written, t->only_cleared, other[i]);
        if (when == drawn_writeback::When::Wait) {
            wait.push_back(cands[i]);
            continue;
        }
        t->writeback_noted = false;
        if (when == drawn_writeback::When::Now && flushable(*t)) {
            Flush f{t, t->gpu, t->gpu + t->guest_size};
            f.only_untouched = true;
            batch.fs.push_back(std::move(f));
        }
    }
    if (!wait.empty()) {
        std::lock_guard lk(drawn_mu);
        drawn_noted.insert(drawn_noted.end(), wait.begin(), wait.end());
    }
    if (batch.fs.empty()) return;
    // (the copies, in this present's recording with its uploads, as the compaction's are: the drawings are two frames
    // back, in recordings before it)
    VkCommandBuffer cmd = upload_cmd();
    full_barrier(cmd, "drawn write-back: before the copies");
    std::vector<Flush> recorded;
    for (Flush& f : batch.fs) {
        if (!flush_record(f, cmd)) continue;   // (no staging memory: its texels stay out of memory, as before this)
        f.t->last_value = std::max(f.t->last_value, rec_value);   // (its end, if it goes now, waits for the copy too)
        batch.was.push_back({f.t->id, f.t->last_written});
        recorded.push_back(std::move(f));
    }
    batch.fs = std::move(recorded);
    if (batch.fs.empty()) return;
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (the copies' writes, for the host)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    drawn_batches.push_back(std::move(batch));
}
} // namespace

void texture_describe_written(uint64_t lo, uint64_t size, std::string& out) {
    const uint64_t hi = lo + size;
    int n = 0;
    for (auto& kv : textures) {
        const Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo || !t->gpu_written) continue;
        if (n++ < 6) {
            char b[160];
            snprintf(b, sizeof b, " #%u %#llx %ux%ux%u x%u NVN %#x %llu KB%s%s;", t->id, (unsigned long long)t->gpu, t->width,
                     t->height, t->depth, t->layers, t->nvn_format, (unsigned long long)(t->guest_size >> 10),
                     t->released ? " finalized" : "", flushable(*t) ? "" : " (not flushable)");
            out += b;
        }
    }
    if (n > 6) out += " (" + std::to_string(n) + " in all)";
}

bool texture_gpu_written_in(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    std::lock_guard lk(vw_mu);
    for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi; ++it)
        if (it->second > lo) return true;
    return false;
}

int texture_flush_to_guest(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Texture*> todo;
    for (auto& kv : textures) {
        Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo) continue;
        t->vw_listed = false;
        if (t->gpu_written && flushable(*t)) todo.push_back(t);
    }
    {
        std::lock_guard lk(vw_mu);
        for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi;)
            it = it->second > lo ? vw.erase(it) : std::next(it);
    }
    int done = 0;
    uint64_t bytes = 0;
    for (Texture* t : todo) {
        if (!flush_texture(*t, lo, hi)) continue;
        done++, bytes += t->guest_size;
        if (t->gpu < lo || t->gpu + t->guest_size > hi) continue;   // (a part of it: the rest is still only in the image)
        // (its memory is its content now: CPU writes to it count again, and an image made over it uploads it)
        t->gpu_written = false;
        t->uploaded = true;
        t->upload_gen = memory_generation();
        watch_tex(t, "written back into its memory");
    }
    static int64_t flushes, all_bytes;
    static double all_ms;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    flushes += done, all_bytes += (int64_t)bytes, all_ms += ms;
    static int logged;
    if (done && (logged++ < 12 || logged % 200 == 0))
        logf("vk: virtual pages %#llx..%#llx remapped: %d texture(s) the GPU wrote there written back into their memory "
             "(%llu KB, %.1f ms; %lld in all, %lld MB, %.0f ms)", (unsigned long long)lo, (unsigned long long)hi, done,
             (unsigned long long)(bytes >> 10), ms, (long long)flushes, (long long)(all_bytes >> 20), all_ms);
    return done;
}

void texture_written(Texture* tex, const char* why) {
    if (!tex) return;
    if (tex->upload_value == rec_value && tex->upload_seg > tl_seg) {   // (a parallel replay's order broken: see dry_mark_written)
        static int64_t n;
        if (n++ < 40 || n % 500 == 0)
            logf("vk: parallel: #%u %#llx %ux%u x%u NVN %#x written in segment %llu, uploaded by the later segment %llu (%lld so); "
                 "written by: %s; before this the GPU had %s it%s%s, first seen at frame %llu, now %llu",
                 tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format,
                 (unsigned long long)tl_seg, (unsigned long long)tex->upload_seg, (long long)n, why,
                 tex->gpu_written ? "written" : "not written", tex->only_cleared ? " (only cleared)" : "",
                 tex->released ? " (released)" : "", (unsigned long long)tex->first_seen, (unsigned long long)frame_no);
    }
    vw_note(*tex);
    tex->only_cleared = false;
    if (tex->sparse && !tex->gpu_written) {
        static int logged;
        if (logged++ < 20) logf("vk: sparse texture %#llx written by the GPU (#%u)", (unsigned long long)tex->gpu, tex->id);
    }
    watch_tex(tex, "written");
    if (!tex->gpu_written || tex->released) tex->first_written = frame_no, drawn_writeback_note(tex);
    // (hookshot) A texture in a virtual pool that the GPU writes first is marked uploaded below without the first upload,
    // which notes its pages' mapping (texture_touch): its first check after the game finalized it took all of its storage
    // for remapped, and the whole image was uploaded from guest memory over the GPU's texels (OoT's HUD button labels blank
    // after a zone change). Noted here instead, by the writers that give a reason: each holds the ParLock a change to the
    // map needs, or is the dry pass (tests/test_drawn_page_map.py lists them). The upscaler's stand-ins give none and run
    // without that lock. A texture clear gives none: resolve() samples its texture first, which notes the map, unless
    // only writers that give none wrote it before (then as before). RX_VK_NO_DRAWN_PAGE_MAP=1: as before
    static const bool note_map = !env_on("RX_VK_NO_DRAWN_PAGE_MAP");
    if (why && strcmp(why, "?") != 0 && note_map && tex->gpu >= (1ull << 40) && tex->page_map.empty() && tex->guest_size) {
        static thread_local Ranges mapped;
        texture_dirty(*tex, 0, mapped);
    }
    tex->last_written = frame_no;
    tex->gpu_written = true;
    tex->gpu_gen = memory_generation();
    tex->released = false;
    tex->uploaded = true;
}


bool texture_touch_quick(Texture* tex, bool for_sampling) {
    tex->last_frame = frame_no;   // (the same values whichever thread stores them)
    tex->last_value = rec_value;
    if (!for_sampling) return true;
    if (tex->gpu_written && !tex->released && !tex->only_cleared) return true;
    if (tex->upload_value == rec_value && tex->upload_seg > tl_seg) return false;
    return tex->uploaded && tex->checked_epoch == epoch;
}
void texture_gpu_wrote(uint64_t gpu, uint64_t size) {
    if (!gpu || !size) return;
    const uint64_t lo = gpu, hi = gpu + size;
    {   // (RX_VK_WATCH_GPU_WRITE=<lo hex>[:<hi hex>]: the GPU buffer writes reaching that range, by program, 60 lines)
        static uint64_t wlo = 0, whi = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_WATCH_GPU_WRITE")) {
                unsigned long long a = 0, b = 0;
                if (sscanf(e, "%llx:%llx", &a, &b) < 2) b = a + 1;
                wlo = a, whi = b;
            }
            return true;
        }();
        (void)parsed;
        static int logged;
        if (wlo < hi && lo < whi && logged++ < 60)
            logf("vk: watch: GPU buffer write [%#llx, +%#llx) into [%#llx, %#llx), frame %llu, program %016llx",
                 (unsigned long long)lo, (unsigned long long)size, (unsigned long long)wlo, (unsigned long long)whi,
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog);
    }
    std::lock_guard lk(gpu_wrote_mu);
    auto it = gpu_wrote.find(lo);
    if (it != gpu_wrote.end() && it->second.first >= hi) {
        it->second.second = rec_value;
        return;
    }
    // (the records are kept apart, each part of the memory in one: gpu_wrote_into looks back at only the one record before a
    // texture, and an older write that a later, smaller one sat inside was missed. One this write lands in keeps what is left
    // of it before and after, with its own submission)
    auto e = gpu_wrote.lower_bound(lo);
    if (e != gpu_wrote.begin() && std::prev(e)->second.first > lo) {
        const auto [end, value] = std::prev(e)->second;
        std::prev(e)->second.first = lo;
        if (end > hi) gpu_wrote[hi] = {end, value};
    }
    while (e != gpu_wrote.end() && e->first < hi) {
        if (e->second.first > hi) gpu_wrote[hi] = e->second;
        e = gpu_wrote.erase(e);
    }
    gpu_wrote[lo] = {hi, rec_value};
    while (gpu_wrote.size() > 65536) gpu_wrote.erase(gpu_wrote.begin());   // (a write inside one adds two)
}
/// A finished GPU write into the texture's storage it was not uploaded after (and notes it as seen)
bool gpu_wrote_into(Texture& tex) {
    std::lock_guard lk(gpu_wrote_mu);
    if (gpu_wrote.empty()) return false;
    const uint64_t lo = tex.gpu, hi = tex.gpu + std::max<uint64_t>(tex.guest_size, 1);
    uint64_t newest = 0;
    auto it = gpu_wrote.upper_bound(lo);
    if (it != gpu_wrote.begin()) --it;
    for (; it != gpu_wrote.end() && it->first < hi; ++it)
        if (it->second.first > lo && it->second.second <= completed_value && it->second.second > tex.gpu_wrote_seen)
            newest = std::max(newest, it->second.second);
    if (!newest) return false;
    tex.gpu_wrote_seen = newest;
    static int logged;
    if (logged++ < 40)
        logf("vk: texture #%u %#llx %ux%u NVN %#x: the GPU wrote its storage (submission %llu): uploaded again", tex.id,
             (unsigned long long)tex.gpu, tex.width, tex.height, tex.nvn_format, (unsigned long long)newest);
    return true;
}
void texture_watch_prog(uint64_t prog) {
    tl_watch_prog = prog;
}
void texture_set_segment(uint64_t seg) {
    tl_seg = seg;
}
uint64_t texture_segment() {
    return tl_seg;
}
void texture_segments_begin(const VkCommandBuffer* init_cmds, size_t n) {
    std::lock_guard lk(seg_init_mu);
    seg_inits.assign(n, SegInit{});
    for (size_t i = 0; i < n; i++) seg_inits[i].cmd = init_cmds[i], seg_inits[i].open = true;
}
void texture_segment_worker(int index) {
    tl_seg_index = index;
}
int texture_segment_end(int index) {
    std::lock_guard lk(seg_init_mu);
    if (index < 0 || index >= (int)seg_inits.size()) return 0;
    SegInit& s = seg_inits[index];
    s.open = false;   // (no new image's change goes to it from here: it is ended)
    if (s.used) full_barrier(s.cmd);   // (before its uploads and its commands)
    if (vkEndCommandBuffer(s.cmd) != VK_SUCCESS) return s.used ? -1 : 0;
    return s.used ? 1 : 0;
}
bool texture_marked(const Texture* tex) {
    return !tex || (tex->gpu_written && !tex->released && tex->uploaded && tex->owned_frame == frame_no &&
                    tex->first_render_frame);
}

namespace {
std::mutex sparse_mu;
std::vector<VkSparseImageMemoryBind> sparse_binds;   // (what the next flush binds, under sparse_mu)
std::vector<VkSparseMemoryBind> sparse_opaque;
VkDeviceSize sparse_added = 0;

/// One sparse block's memory (or n bytes) from the texture's chunks; false when none can be had
bool sparse_take(Texture* tex, VkDeviceSize n, VkDeviceMemory& mem, VkDeviceSize& off) {
    constexpr VkDeviceSize kChunk = 64ull << 20;
    n = (n + tex->sparse_align - 1) / tex->sparse_align * tex->sparse_align;
    for (size_t i = 0; i < tex->sparse_free.size(); i++) {   // (a failed bind's memory first: a bind that keeps failing takes no more)
        if (tex->sparse_free[i].size < n) continue;
        mem = tex->sparse_free[i].memory, off = tex->sparse_free[i].offset;
        tex->sparse_free.erase(tex->sparse_free.begin() + (std::ptrdiff_t)i);
        return true;
    }
    if (tex->sparse_chunks.empty() || tex->chunk_used + n > kChunk) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = std::max(kChunk, n);
        ai.memoryTypeIndex = tex->sparse_type;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev.device, &ai, nullptr, &m) != VK_SUCCESS) {
            static int logged;
            if (logged++ < 8) logf("vk: sparse texture %#llx: no device memory for its blocks", (unsigned long long)tex->gpu);
            return false;
        }
        tex->sparse_chunks.push_back(m);
        tex->chunk_used = 0;
        tex->bytes += ai.allocationSize;
        sparse_added += ai.allocationSize;
    }
    mem = tex->sparse_chunks.back();
    off = tex->chunk_used;
    tex->chunk_used += n;
    return true;
}

/// The queued binds that were not made: their blocks and mip tails marked unbound again, so the next write to them
/// queues them anew rather than writing into unbacked memory (their memory goes to the texture's free list, for the retry)
void sparse_unqueue(Texture* tex) {
    const VkSparseImageMemoryRequirements& r = tex->sparse_req;
    const VkExtent3D g = r.formatProperties.imageGranularity;
    for (const VkSparseImageMemoryBind& b : sparse_binds) {
        const uint32_t level = b.subresource.mipLevel, layer = b.subresource.arrayLayer;
        if (level >= tex->levels || layer >= tex->layers || !g.width || !g.height) continue;
        std::vector<uint8_t>& map = tex->tiles[(size_t)layer * tex->levels + level];
        const uint32_t lw = std::max(1u, tex->width >> level);
        const size_t i = (size_t)(b.offset.y / g.height) * ((lw + g.width - 1) / g.width) + b.offset.x / g.width;
        if (i < map.size()) map[i] = 0;
        tex->sparse_free.push_back({b.memory, b.memoryOffset, tex->sparse_align});
    }
    const bool single = r.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
    for (const VkSparseMemoryBind& b : sparse_opaque) {
        const VkDeviceSize layer = single || !r.imageMipTailStride ? 0 : (b.resourceOffset - r.imageMipTailOffset) / r.imageMipTailStride;
        if (layer < tex->tail_bound.size()) tex->tail_bound[layer] = 0;
        const VkDeviceSize a = tex->sparse_align ? tex->sparse_align : 1;
        tex->sparse_free.push_back({b.memory, b.memoryOffset, (b.size + a - 1) / a * a});
    }
    total_bytes += sparse_added;
    sparse_added = 0;
    sparse_binds.clear();
    sparse_opaque.clear();
}

bool sparse_flush(Texture* tex) {
    if (sparse_binds.empty() && sparse_opaque.empty()) return true;
    VkSparseImageMemoryBindInfo ib{tex->image, (uint32_t)sparse_binds.size(), sparse_binds.data()};
    VkSparseImageOpaqueMemoryBindInfo ob{tex->image, (uint32_t)sparse_opaque.size(), sparse_opaque.data()};
    VkBindSparseInfo bi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bi.imageBindCount = sparse_binds.empty() ? 0 : 1;
    bi.pImageBinds = &ib;
    bi.imageOpaqueBindCount = sparse_opaque.empty() ? 0 : 1;
    bi.pImageOpaqueBinds = &ob;
    static VkFence fence = VK_NULL_HANDLE;
    if (!fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev.device, &fi, nullptr, &fence) != VK_SUCCESS) {   // (never a bind waited on through a null fence)
            fence = VK_NULL_HANDLE;
            static int logged;   // (retried on the next write: capped like the other sparse messages)
            if (logged++ < 8) logf("vk: sparse texture %#llx: no fence for its binds", (unsigned long long)tex->gpu);
            sparse_unqueue(tex);
            return false;
        }
    }
    VkResult res;
    {   // (bound before any work that writes the blocks is submitted: waited for here, on the recording thread. And
        // after the work submitted before is done: a sparse bind is not ordered with command buffers by their
        // barriers, and one that landed while a pass sampled the image changed what the pass read part-way through
        // its tiles. Opt-in, RX_VK_SPARSE_IDLE=1: MKW binds hundreds of blocks a race, and the replay's stalls at
        // each made its other races worse)
        static const bool no_idle = !env_on("RX_VK_SPARSE_IDLE");
        static int64_t binds;
        std::lock_guard qk(dev.present_mu);
        queue_flush_locked();
        if (!no_idle) vkQueueWaitIdle(dev.queue);
        res = vkQueueBindSparse(dev.queue, 1, &bi, fence);
        if (++binds % 200 == 0) logf("vk: %lld sparse binds", (long long)binds);
    }
    if (res == VK_SUCCESS) {
        vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev.device, 1, &fence);
    } else {
        static int failed;   // (retried on the next write: capped like the other sparse messages)
        if (failed++ < 8) logf("vk: sparse texture %#llx: binding failed (%d)", (unsigned long long)tex->gpu, (int)res);
        sparse_unqueue(tex);
        return false;
    }
    static int logged;
    static uint64_t next_mb = 0;
    if (logged < 20 || (tex->bytes >> 20) >= next_mb) {
        logged++;
        next_mb = (tex->bytes >> 20) + 256;
        logf("vk: sparse texture %#llx: %zu blocks bound (%llu MB of device memory)", (unsigned long long)tex->gpu,
             sparse_binds.size() + sparse_opaque.size(), (unsigned long long)(tex->bytes >> 20));
    }
    total_bytes += sparse_added;
    sparse_added = 0;
    sparse_binds.clear();
    sparse_opaque.clear();
    return res == VK_SUCCESS;
}

/// Queues the binds of what a rectangle reaches that is not bound yet (sparse_mu held)
bool sparse_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const VkSparseImageMemoryRequirements& r = tex->sparse_req;
    if (level >= tex->levels || layer >= tex->layers) return true;
    if (level >= r.imageMipTailFirstLod) {   // (the mip tail: bound whole)
        const bool single = r.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
        uint8_t& bound = tex->tail_bound[single ? 0 : layer];
        if (bound || !r.imageMipTailSize) return true;
        VkDeviceMemory m;
        VkDeviceSize off;
        if (!sparse_take(tex, r.imageMipTailSize, m, off)) return false;
        sparse_opaque.push_back({r.imageMipTailOffset + (single ? 0 : layer * r.imageMipTailStride), r.imageMipTailSize,
                                 m, off, 0});
        bound = 1;
        return true;
    }
    const VkExtent3D g = r.formatProperties.imageGranularity;
    const uint32_t lw = std::max(1u, tex->width >> level), lh = std::max(1u, tex->height >> level);
    if (x >= lw || y >= lh || !w || !h) return true;
    w = std::min(w, lw - x), h = std::min(h, lh - y);
    const uint32_t ntx = (lw + g.width - 1) / g.width, nty = (lh + g.height - 1) / g.height;
    std::vector<uint8_t>& map = tex->tiles[(size_t)layer * tex->levels + level];
    if (map.empty()) map.assign((size_t)ntx * nty, 0);
    bool ok = true;
    for (uint32_t ty = y / g.height; ty <= (y + h - 1) / g.height; ty++)
        for (uint32_t tx = x / g.width; tx <= (x + w - 1) / g.width; tx++) {
            uint8_t& bound = map[(size_t)ty * ntx + tx];
            if (bound) continue;
            VkDeviceMemory m;
            VkDeviceSize off;
            if (!sparse_take(tex, tex->sparse_align, m, off)) {
                ok = false;
                continue;
            }
            VkSparseImageMemoryBind b{};
            b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer};
            b.offset = {(int32_t)(tx * g.width), (int32_t)(ty * g.height), 0};
            b.extent = {std::min(g.width, lw - tx * g.width), std::min(g.height, lh - ty * g.height), 1};
            b.memory = m;
            b.memoryOffset = off;
            sparse_binds.push_back(b);
            bound = 1;
        }
    return ok;
}
} // namespace

/// (the upload's: queued, and bound together before its copies are recorded)
void texture_resident_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    std::lock_guard lk(sparse_mu);
    sparse_queue(tex, level, layer, x, y, w, h);
}
void texture_resident_flush(Texture* tex) {
    if (!tex || !tex->sparse) return;
    std::lock_guard lk(sparse_mu);
    sparse_flush(tex);
}

bool texture_resident(Texture* tex, uint32_t level, uint32_t first, uint32_t count, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        ok &= sparse_queue(tex, level, layer, x, y, w, h);
    return sparse_flush(tex) && ok;
}

bool texture_resident(Texture* tex, uint32_t first, uint32_t count) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        for (uint32_t level = 0; level < tex->levels; level++)
            ok &= sparse_queue(tex, level, layer, 0, 0, ~0u, ~0u);
    return sparse_flush(tex) && ok;
}

void texture_rendered(Texture* tex) {
    if (!tex) return;
    if (tex->sparse && !tex->first_render_frame) texture_resident(tex, 0, tex->layers);   // (rendered into: all of it)
    if (!tex->first_render_frame) tex->first_render_frame = frame_no;
    // (its storage's range as the GPU's, once a frame: render targets only. Copies and storage writes registered it
    // too, and a texture made later over such storage (MKW's terrain tiles reuse the slots of a pool) was not uploaded
    // and kept the placeholder it was made with)
    if (tex->owned_frame != frame_no) {
        tex->owned_frame = frame_no;
        const uint64_t lo = tex->gpu, hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
        for (auto it = gpu_owned.lower_bound(lo); it != gpu_owned.end() && it->first < hi;)
            it = it->second.first <= hi ? gpu_owned.erase(it) : std::next(it);
        gpu_owned[lo] = {hi, memory_generation()};
        if (gpu_owned.size() > 65536) gpu_owned.erase(gpu_owned.begin());   // (bounded; the oldest addresses go)
    }
}
uint64_t texture_frame_no() {
    return frame_no;
}

namespace {
// A guest GPU address may resolve to another imported buffer later. Keep the views with the actual backing,
// and release them only where memory.cpp already destroys that backing after its recorded users complete.
using TexelViewKey = std::tuple<VkFormat, uint64_t, uint64_t>;   // (format, offset, range)
struct TexelViews {
    std::map<TexelViewKey, VkBufferView> views;
    uint64_t failed_frame = UINT64_MAX;
};
std::mutex texel_views_mu;
std::map<VkBuffer, TexelViews> texel_views;
uint64_t texel_registry_failed_frame = UINT64_MAX;
} // namespace

void release_texel_buffer_views(VkBuffer buffer) {
    // Unpublished imports may also be dropped by the import worker. Never take pools_mu under this lock:
    // the callers that retire a published buffer already hold pools_mu, and gpu_buffer takes it first.
    std::lock_guard lk(texel_views_mu);
    auto it = texel_views.find(buffer);
    if (it == texel_views.end()) return;
    for (const auto& [key, view] : it->second.views)
        if (view) vkDestroyBufferView(dev.device, view, nullptr);
    texel_views.erase(it);   // (a later import may receive the same numerical VkBuffer handle)
}

VkBufferView texel_buffer_view(const nvn_tex* t, const nvn_view* v, bool storage) {
    if (!t || !t->gpu) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    VkFormat f = vk_format(nf).format;
    if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    {   // (a format the device has no texel buffers of, e.g. some 3-component ones: no view rather than an invalid one)
        if (!(format_props(f).bufferFeatures & (storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                logf("vk: texel buffer of format %d (%s) not supported by the device: unbound", (int)f, storage ? "storage" : "uniform");
            return VK_NULL_HANDLE;
        }
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(nf, &bw, &bh);
    if (bpb <= 0) return VK_NULL_HANDLE;
    const uint64_t size = (uint64_t)std::max(1u, t->d.width) * bpb;
    // (a formatted store into it: textures over that memory are uploaded again once the GPU is done)
    if (storage) texture_gpu_wrote(t->gpu, size);
    // Resolve and stamp even a cache hit: the address can be remapped, and cached-only bindings are not idle.
    BufferRef b = gpu_buffer(t->gpu, size);
    if (!b || b.offset % std::max<uint64_t>(dev.props.limits.minTexelBufferOffsetAlignment, 1)) return VK_NULL_HANDLE;
    const uint64_t range = std::min<uint64_t>(size, (uint64_t)dev.props.limits.maxTexelBufferElements * bpb);
    if (!range) return VK_NULL_HANDLE;
    const TexelViewKey key{f, b.offset, range};
    std::lock_guard lk(texel_views_mu);
    auto owner = texel_views.find(b.buffer);
    if (owner == texel_views.end()) {
        if (texel_registry_failed_frame == frame_no) return VK_NULL_HANDLE;
        try {
            owner = texel_views.try_emplace(b.buffer).first;
        } catch (const std::exception&) {
            texel_registry_failed_frame = frame_no;
            return VK_NULL_HANDLE;
        }
    }
    TexelViews& cache = owner->second;
    auto it = cache.views.find(key);
    if (it != cache.views.end()) return it->second;
    if (cache.failed_frame == frame_no) return VK_NULL_HANDLE;   // (one failed allocation per backing and present)
    VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    ci.buffer = b.buffer;
    ci.format = f;
    ci.offset = b.offset;
    ci.range = range;
    VkBufferView view = VK_NULL_HANDLE;
    if (vkCreateBufferView(dev.device, &ci, nullptr, &view) != VK_SUCCESS) {
        cache.failed_frame = frame_no;   // (an error's output handle is undefined; never cache or destroy it)
        return VK_NULL_HANDLE;
    }
    try {
        cache.views.emplace(key, view);
    } catch (const std::exception&) {
        vkDestroyBufferView(dev.device, view, nullptr);   // (only the new, uncommitted view)
        cache.failed_frame = frame_no;
        return VK_NULL_HANDLE;
    }
    return view;
}

// ---- (hookshot) VRAM compacted (on by default since the hold lets go of the dead render targets first and of long-unused
// textures only; RX_VK_COMPACT=0: off, as before) ----
// An evicted texture's memory stays inside its 128 MB block, and the driver counts the block whole: after some play 0.6 to
// 1.6 GB of what the driver counts is such free space (3 to 4 GB after the cache was emptied), and evicting more gives
// none of it back. Compacted, the textures that are not render targets have a pool of their own (vk_vma.cpp; the targets have theirs) and, when RX_VK_COMPACT_MB (512)
// or more of it is free space, VMA's defragmentation moves textures out of the sparsely used blocks, a few a pass: a new
// image at the new place, the texels copied on the GPU in the present's upload commands (every draw of the frame is in an
// earlier recording), the texture's views made again when next asked for, and the old image destroyed once the GPU is past
// the copy; then VMA frees the blocks that emptied. While a pass is open nothing of the pool is freed (the images the GPU
// is done with wait a frame or two): VMA holds the allocations it listed.
struct Compact {
    bool in_pass = false;
    uint64_t value = 0;                    // (the recording the open pass's copies are in)
    std::vector<VkImage> old_images;       // (of the open pass: destroyed when the GPU is past them)
    std::vector<VkImageView> old_views;
    uint64_t next_check = 0;               // (the present from which another compaction may start)
    uint64_t moved = 0, moved_bytes = 0, ignored = 0, passes = 0;   // (of the compaction under way)
    uint64_t runs = 0, all_moved = 0, all_freed = 0;
    // (the driver's count over 90% of what it allows, texture_frame's look every 10 presents: vram_compact.h)
    bool pressed = false;
} compact;
const Texture* present_source = nullptr;   // (what the present shows this frame: do_present holds its image and view)
void compact_step(uint64_t completed) {
    if (!compact_on()) return;
    if (compact.in_pass) {
        if (compact.value > completed) return;   // (the GPU has not run the copies yet)
        for (VkImageView v : compact.old_views) vkDestroyImageView(dev.device, v, nullptr);
        for (VkImage i : compact.old_images) vkDestroyImage(dev.device, i, nullptr);   // (their memory is VMA's: the pass's end frees it)
        compact.old_views.clear(), compact.old_images.clear();
        compact.in_pass = false;
        uint64_t freed = 0;
        uint32_t blocks = 0;
        if (!image_compact_pass_end(&freed, &blocks)) {
            compact.all_moved += compact.moved, compact.all_freed += freed;
            logf("vk: VRAM compacted: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks (%llu MB) "
                 "given back to the driver; %llu MB in all so far", (unsigned long long)compact.moved,
                 (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
                 (unsigned long long)compact.ignored, blocks, (unsigned long long)(freed >> 20),
                 (unsigned long long)(compact.all_freed >> 20));
            compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        }
        return;   // (the images the GPU is done with are freed now, before the next pass opens)
    }
    if (!image_compact_active()) {
        if (frame_no < compact.next_check) return;
        compact.next_check = frame_no + 120;
        static const uint64_t min_free = (uint64_t)(getenv("RX_VK_COMPACT_MB") ? std::max(1, atoi(getenv("RX_VK_COMPACT_MB"))) : 512) << 20;
        uint64_t block_bytes = 0, free_bytes = 0, kept = 0;   // (kept: the blocks made at the start, which no run gives back)
        if (!textures_pool_usage(&block_bytes, &free_bytes, &kept) ||
            vram_compact::returnable(block_bytes, free_bytes, kept) < vram_compact::run_from(min_free, compact.pressed))
            return;
        if (!image_compact_begin(64ull << 20, 12)) return;
        compact.runs++, compact.moved = compact.moved_bytes = compact.ignored = compact.passes = 0;
        logf("vk: VRAM compaction %llu begins: the textures' blocks hold %llu MB, %llu MB of it free space",
             (unsigned long long)compact.runs, (unsigned long long)(block_bytes >> 20), (unsigned long long)(free_bytes >> 20));
    }
    static thread_local std::vector<ImageMove> moves;
    uint64_t freed_now = 0;
    uint32_t blocks_now = 0;
    if (!image_compact_pass(moves, &freed_now, &blocks_now)) {   // (nothing left to move)
        compact.all_moved += compact.moved, compact.all_freed += freed_now;
        logf("vk: VRAM compaction %llu over: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks "
             "(%llu MB) given back to the driver, %llu MB in all so far", (unsigned long long)compact.runs,
             (unsigned long long)compact.moved, (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
             (unsigned long long)compact.ignored, blocks_now, (unsigned long long)(freed_now >> 20),
             (unsigned long long)(compact.all_freed >> 20));
        compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        return;
    }
    std::unordered_map<VmaAllocation, Texture*> by_allocation;
    by_allocation.reserve(textures.size());
    for (auto& kv : textures)
        if (kv.second->allocation) by_allocation[kv.second->allocation] = kv.second.get();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    for (size_t i = 0; i < moves.size(); i++) {
        const auto f = by_allocation.find(moves[i].src);
        Texture* t = f == by_allocation.end() ? nullptr : f->second;   // (none: an image the GPU is not done with)
        const VkImageUsageFlags both = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        bool ok = t && t->image && !t->sparse && t != present_source && t->samples == VK_SAMPLE_COUNT_1_BIT &&
                  (t->usage & both) == both;
        VkImage image = VK_NULL_HANDLE;
        if (ok) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (as create() made it)
            ci.flags = t->create_flags;
            ci.imageType = t->type;
            ci.format = t->format;
            ci.extent = {t->width, t->height, t->depth};
            ci.mipLevels = t->levels;
            ci.arrayLayers = t->layers;
            ci.samples = t->samples;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = t->usage;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ok = vkCreateImage(dev.device, &ci, nullptr, &image) == VK_SUCCESS;
            if (ok && !image_compact_bind(i, image)) {
                vkDestroyImage(dev.device, image, nullptr);
                ok = false;
            }
        }
        if (!ok) {
            image_compact_ignore(i);
            compact.ignored++;
            continue;
        }
        if (!cmd) {
            cmd = upload_cmd();
            full_barrier(cmd, "compaction: before the copies");
        }
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        static thread_local std::vector<VkImageCopy> regions;
        regions.clear();
        for (uint32_t l = 0; l < t->levels; l++) {
            VkImageCopy r{};
            r.srcSubresource = r.dstSubresource = {t->aspect, l, 0, t->layers};
            r.extent = {std::max(1u, t->width >> l), std::max(1u, t->height >> l), std::max(1u, t->depth >> l)};
            regions.push_back(r);
        }
        vkCmdCopyImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
        compact.old_images.push_back(t->image);
        for (auto& kv : t->views)
            if (kv.second) compact.old_views.push_back(kv.second);
        t->views.clear();   // (made again when next asked for)
        t->image = image;
        t->last_value = std::max(t->last_value, rec_value);   // (its end, if it goes now, waits for the copy too)
        compact.moved++, compact.moved_bytes += t->bytes + t->pad;
    }
    compact.passes++;
    if (!cmd) {   // (nothing of this pass could be moved: ended at once)
        uint64_t freed = 0;
        uint32_t blocks = 0;
        if (!image_compact_pass_end(&freed, &blocks)) {
            compact.all_moved += compact.moved, compact.all_freed += freed;
            logf("vk: VRAM compacted: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks (%llu MB) "
                 "given back to the driver; %llu MB in all so far", (unsigned long long)compact.moved,
                 (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
                 (unsigned long long)compact.ignored, blocks, (unsigned long long)(freed >> 20),
                 (unsigned long long)(compact.all_freed >> 20));
            compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        }
        return;
    }
    full_barrier(cmd, "compaction: after the copies");
    destroy_gen++;   // (what was resolved to the moved textures' views is stale)
    compact.in_pass = true, compact.value = rec_value;
}


Texture* texture_for_present(const nvn_tex* t) {
    Texture* tex = texture_get(t, false);
    if (tex && tex->gpu_written) return present_source = tex, tex;
    Texture* best = nullptr;   // (one rendered at its storage in another description)
    auto [a, b] = by_gpu.equal_range(t ? t->gpu : 0);
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && (it->second->aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
            (!best || it->second->last_frame > best->last_frame))
            best = it->second;
    present_source = best ? best : tex;
    return best ? best : tex;
}

/// (hookshot) The texture budget a quality preset sets, or the card's own (tests/test_preset_texture_budget.py). The preset's
/// replaced `own` outright, and `own` is where RX_VK_VRAM_MB and an integrated GPU's cap are applied: with a preset on, the
/// documented RX_VK_VRAM_MB did nothing (the mirrors' RX_VK_MIRROR_MB wins over the preset), and an integrated GPU on Low or
/// Balanced got 3.6 or 4.8 GB of textures past the 2.5 GB that keeps a 16 GB laptop from running out of RAM. Now
/// RX_VK_VRAM_MB wins over a preset, and on an integrated GPU the preset's budget is at most `own`
uint64_t preset_texture_budget(uint64_t preset_mb, bool preset_applies, bool vram_env, bool integrated, uint64_t own) {
    if (!preset_mb || !preset_applies || vram_env) return own;
    const uint64_t b = preset_mb << 20;
    return integrated ? std::min(b, own) : b;
}

int64_t texture_readback_wait_ns() { return st_readback_wait_ns.load(std::memory_order_relaxed); }
int64_t texture_frame_ns() { return st_texture_frame_ns.load(std::memory_order_relaxed); }

void texture_frame(uint64_t completed) {
    const TraceTimer timed{st_texture_frame_ns};   // (hookshot: the frame trace's tex_frame_ms)
    upload_frame_report();
    frame_upload_ns.store(0, std::memory_order_relaxed);
    if (const int64_t d = st_deferred_frame.exchange(0, std::memory_order_relaxed)) {
        // (the views and descriptor sets resolved to the placeholder this frame go stale, as when textures go: the next frame
        // looks those textures up again, and uploads them)
        destroy_gen++;
        static int told;
        if (told++ < 40 || st_deferred.load(std::memory_order_relaxed) % 2000 < d)
            logf("vk: %lld new ASTC textures left for a later frame (this frame's uploads took their budget; %lld so far)", (long long)d,
                 (long long)st_deferred.load(std::memory_order_relaxed));
    }
    completed_value = completed;
    frame_no++;
    drawn_writeback_frame();
    {   // (staging arenas the GPU is done with, beyond 256 MB of them, and any over 64 MB: freed. A scene change stages
        // hundreds of MB to over a GB (the course maps, the terrain's ASTC arrays), and that host memory was kept)
        // (hookshot: 64 on a card of 6 GB or less. On one with resizable BAR the arenas are VRAM: VMA puts a host-visible buffer the GPU
        // reads (STORAGE: the ASTC decoder) in DEVICE_LOCAL | HOST_VISIBLE. The RTX 4050 Laptop held 4 idle ones, 256 MB of 5.7 GB, through
        // all of Kokiri gameplay, which stages ~nothing. A scene change makes the ones it needs again. RX_VK_STAGING_KEEP_MB=256: as before)
        const uint64_t keep = staging_keep();
        uint64_t kept = 0;
        for (auto it = arenas.begin(); it != arenas.end();) {
            const bool idle = it->value <= completed && it->value != rec_value;
            // (and one nothing has staged into for 30 s: Kokiri Forest's gameplay stages next to nothing, and the 64 MB it kept are VRAM
            // behind the BAR. A load makes one again. RX_VK_STAGING_IDLE_S=<n>: that long, 0: kept)
            static const int64_t idle_s = getenv("RX_VK_STAGING_IDLE_S") ? atoll(getenv("RX_VK_STAGING_IDLE_S")) : 30;
            const bool stale = idle_s && std::chrono::steady_clock::now() - it->used_at > std::chrono::seconds(idle_s);
            if (idle && (it->size > kArena || kept + it->size > keep || stale)) {
                if (!spare_give_back(*it)) destroy_buffer(it->buffer, it->allocation);   // (SpareArenas)
                it = arenas.erase(it);
                continue;
            }
            kept += it->size;
            ++it;
        }
        if (frame_no % 600 == 0) {   // (the process's memory now and then: what the host side holds)
            PROCESS_MEMORY_COUNTERS_EX pm{};
            pm.cb = sizeof pm;
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
                logf("vk: memory: working set %llu MB (peak %llu), private %llu MB, staging %llu MB in %zu arenas",
                     (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PeakWorkingSetSize >> 20),
                     (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(kept >> 20), arenas.size());
#if defined(__linux__) && defined(__GLIBC__)
            // (glibc's heap: what its arenas hold free. The shader translation and the loads' decoding leave their garbage spread through
            // the threads' arenas (26 of them, 1.2 GB resident in Kokiri Forest, the heap's growth in a scene change never going back),
            // and a free chunk's pages are the process's till malloc_trim gives them to the system. Asked on a worker, when 192 MB or
            // more are free: RX_VK_MALLOC_TRIM_MB=<n>: that many, 0: never; the "vk: malloc:" line says what the arenas hold)
            // (every 1800 presents: mallinfo2 walks every free chunk of every arena with its lock held)
            static std::atomic<bool> busy{false};
            if (frame_no % 1800 == 0 && !busy.exchange(true))
                jobs_push([] {
                    static const uint64_t trim_mb = getenv("RX_VK_MALLOC_TRIM_MB") ? (uint64_t)atoll(getenv("RX_VK_MALLOC_TRIM_MB")) : 192;
                    const struct mallinfo2 a = mallinfo2();
                    PROCESS_MEMORY_COUNTERS_EX pm0{};
                    pm0.cb = sizeof pm0;
                    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm0, sizeof pm0);
                    logf("vk: malloc: %llu MB in arenas (%llu MB in use, %llu MB free, %llu MB of it at the tops), %llu MB mapped",
                         (unsigned long long)(a.arena >> 20), (unsigned long long)(a.uordblks >> 20), (unsigned long long)(a.fordblks >> 20),
                         (unsigned long long)(a.keepcost >> 20), (unsigned long long)(a.hblkhd >> 20));
                    if (trim_mb && (a.fordblks >> 20) >= trim_mb) {
                        const auto t0 = std::chrono::steady_clock::now();
                        malloc_trim(0);
                        PROCESS_MEMORY_COUNTERS_EX pm1{};
                        pm1.cb = sizeof pm1;
                        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm1, sizeof pm1);
                        logf("vk: malloc_trim: %llu MB free in the arenas now, the process's private memory %llu -> %llu MB (%.0f ms)",
                             (unsigned long long)(mallinfo2().fordblks >> 20), (unsigned long long)(pm0.PrivateUsage >> 20),
                             (unsigned long long)(pm1.PrivateUsage >> 20),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                    }
                    busy = false;
                });
#endif
        }
    }
    bc_scratch_idle();
    compact_step(completed);   // (hookshot, RX_VK_COMPACT: while its pass is open nothing of the textures' pool is freed)
    for (auto it = dead.begin(); it != dead.end();) {
        if (compact.in_pass) break;   // (hookshot, RX_VK_COMPACT: nothing is freed while a pass is open; a frame or two)
        if (it->value > completed) {
            ++it;
            continue;
        }
        if (it->pool_key) {   // (a render target's image: kept for the next of its shape, above)
            pool_put(*it);
            it = dead.erase(it);
            continue;
        }
        for (VkImageView v : it->views) vkDestroyImageView(dev.device, v, nullptr);
        destroy_image(it->image, it->allocation);
        if (it->buffer) destroy_buffer(it->buffer, it->buffer_alloc);
        for (VkDeviceMemory m : it->memory) vkFreeMemory(dev.device, m, nullptr);
        it = dead.erase(it);
    }
    pool_trim();
    // A budget: RX_VK_VRAM_MB of textures (default 4096), and less when the driver says the process is near what it
    // may have (other programs use the GPU too). The least recently used go first, down to 7/8 of it; nothing the
    // last 8 presents used, nothing recorded work may still read.
    static uint64_t own = 0;
    if (frame_no % 10) return;
    uint64_t used = 0, allowed = 0, block_free = 0;
    const bool known = vram_usage(&used, &allowed, &block_free);
    if (!own)   // (RX_VK_VRAM_MB, else 70% of what the driver allows the process at the first look)
        own = getenv("RX_VK_VRAM_MB") ? (uint64_t)atoi(getenv("RX_VK_VRAM_MB")) << 20
              : known                ? std::min<uint64_t>(allowed / 100 * (allowed <= (8704ull << 20) ? 45 : 70), 6144ull << 20)
                                     : 4096ull << 20;
    // (hookshot: at most 6 GB: a 16 GB card's 10.8 GB filled up, read as a leak: 105, 106. On a card of 8 GB or less, 45% of what the
    // driver allows, not 70%: the game's other VRAM (render targets, the pools' mirrors and imports, buffers, pipelines) is ~3.5-3.8 GB
    // in Hyrule Field, so 70% (5.1 GB of an 8 GB card's 7.3) put the whole past what the card holds. The driver paged VRAM to system
    // memory and the GPU time went from ~13 to 55 ms a frame for good: an RTX 3060 Ti at 14 fps ten seconds into play, #1400's
    // RTX 4050 at 5 fps. RX_VK_VRAM_MB=<MB>: the old size, or any)
    {   // (hookshot) an integrated GPU's "VRAM" is the system's RAM, dynamically calculated based on system RAM
        static bool told;
        if (!getenv("RX_VK_VRAM_MB") && dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            // Get system RAM
            uint64_t system_ram = 0;
#if defined(_WIN32)
            MEMORYSTATUSEX mem_info{sizeof(MEMORYSTATUSEX)};
            if (GlobalMemoryStatusEx(&mem_info)) {
                system_ram = mem_info.ullTotalPhys;
            }
#else
            // Linux: read from /proc/meminfo
            FILE* f = fopen("/proc/meminfo", "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    if (sscanf(line, "MemTotal: %llu kB", (unsigned long long*)&system_ram) == 1) {
                        system_ram *= 1024;
                        break;
                    }
                }
                fclose(f);
            }
#endif
            if (system_ram == 0) system_ram = 8ull << 30;  // 8 GB fallback

            // Reserve memory for non-texture usage:
            // - Game's own allocations (~10 GB for Switch 2 games)
            // - Render targets, buffers, pipelines (~3-4 GB)
            // - OS and other apps (~2-4 GB)
            // - Driver overhead
            uint64_t reserved = std::min<uint64_t>(system_ram / 3, 16ull << 30);
            if (reserved < 8ull << 30) reserved = 8ull << 30;

            uint64_t available = system_ram > reserved ? system_ram - reserved : 0;
            uint64_t driver_budget = own;  // 'own' already has the 70% calculation applied
            uint64_t budget = std::min(available, driver_budget);

            // Hard caps based on system RAM
            if (system_ram <= 8ull << 30) {
                budget = std::min<uint64_t>(budget, 1536ull << 20);   // 1.5 GB
            } else if (system_ram <= 12ull << 30) {
                budget = std::min<uint64_t>(budget, 2048ull << 20);   // 2 GB
            } else if (system_ram <= 16ull << 30) {
                budget = std::min<uint64_t>(budget, 2560ull << 20);   // 2.5 GB (previous default)
            } else if (system_ram <= 24ull << 30) {
                budget = std::min<uint64_t>(budget, 3584ull << 20);   // 3.5 GB
            } else if (system_ram <= 32ull << 30) {
                budget = std::min<uint64_t>(budget, 4096ull << 20);   // 4 GB
            } else {
                budget = std::min<uint64_t>(budget, 6144ull << 20);   // 6 GB (dGPU max)
            }

            budget = std::max<uint64_t>(budget, 1024ull << 20);  // Minimum 1 GB
            budget = std::min(budget, driver_budget);  // Don't exceed driver's limit
            own = budget;

            if (!told) {
                told = true;
                logf("vk: integrated GPU texture budget: %llu MB (system RAM %llu GB, driver allows %llu MB, reserved %llu GB)",
                     (unsigned long long)(own >> 20),
                     (unsigned long long)(system_ram >> 30),
                     (unsigned long long)(driver_budget >> 20),
                     (unsigned long long)(reserved >> 30));
            }
        }
    }
    // (a smaller preset on a bigger card: the card's own; RX_VK_VRAM_MB and an integrated GPU's cap: preset_texture_budget)
    const bool vram_env = getenv("RX_VK_VRAM_MB") != nullptr;
    const bool preset_budget = opts.vram_mb > 0 && !vram_env && preset_caps_memory();
    uint64_t budget = preset_texture_budget((uint64_t)std::max(opts.vram_mb, 0), preset_budget, vram_env,
                                            dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, own);
    if (preset_budget && known) {
        // Safety clamp: do not exceed 75% of driver-allowed memory on constrained VRAM (prevents VK_ERROR_DEVICE_LOST)
        budget = std::min(budget, (allowed / 4) * 3);
    }
    // (near what the driver allows, the budget shrinks by the excess. The excess of what the process holds less the
    // free space in VMA's blocks, which new images take before VMA makes a block: measured by the blocks alone, the
    // textures evicted freed space inside them that the driver still counted, so 10 frames later the budget shrank
    // again. A race kept ~3 GB free in the blocks and evicted and made again ~100 textures every 10 frames (1300 at
    // once), mostly the same ones: their uploads and decodes, and images made empty in the frame that drew them.
    // The driver's own count near its limit (97%) still squeezes, whatever the blocks hold free: then it pages VRAM
    // out. RX_VK_BUDGET_BY_BLOCKS=1: as before)
    static const bool by_blocks = env_on("RX_VK_BUDGET_BY_BLOCKS");
    uint64_t evict_to = 0;   // (hookshot, RX_VK_COMPACT: what an eviction goes down to when the compaction's hold set the budget)
    uint64_t hold_to = 0;    // (and what the hold's own line asks for, when the reserve's lower line set evict_to)
    // (hookshot, RX_VK_COMPACT) compacted, only the textures' pool's free space comes off the count: vram_compact.h
    uint64_t reclaim = block_free;
    bool kept_only = false;   // (the textures' pool is down to its blocks made at the start: the 97% rule, below)
    if (compact_on()) {
        uint64_t pool_blocks = 0, pool_free = 0, pool_kept = 0;
        if (textures_pool_usage(&pool_blocks, &pool_free, &pool_kept)) reclaim = vram_compact::reclaimable(block_free, pool_free);
        kept_only = vram_compact::kept_only(pool_blocks, pool_kept);
    }
    const uint64_t pressure = by_blocks ? used : used - std::min(used, reclaim);
    pool_pressed = known && pressure > allowed / 10 * 9;   // (the target pool given back meanwhile)
    compact.pressed = known && vram_compact::pressed(used, allowed);   // (hookshot, RX_VK_COMPACT: by the driver's own count)
    const uint64_t size_budget = budget;   // (the budget as a size, before the rules that take an excess off it: rule_target)
    if (known && pressure > allowed / 10 * 9) {
        const uint64_t over = pressure - allowed / 10 * 9;
        budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
    }
    // (hookshot, RX_VK_COMPACT: what the budget's size and the 90% rule ask on their own, before the hold's lines; the 97%
    // rule's joins it below. The hold's pass lets a texture unused for the eviction's own time go for that part, as a pass
    // does with the compaction off: vram_compact::hold_target)
    uint64_t rule_budget = budget;
    // (hookshot, RX_VK_COMPACT) and the count is held at RX_VK_COMPACT_PCT% of the allowance (85; 50 to 90), as it will be once a
    // run has given the textures' free space back: within ~0.8 GB of what the driver allows the GPU's frame went from ~10 ms
    // to 13-20 ms with the shadows in place, the driver itself short. At 85 a five-minute walk across Hyrule Field at DLAA
    // 2560 x 1440 had 11-12 frames over 33 ms against 34-49 uncompacted, and a 1% low of 37 fps against 26-29
    if (compact_on() && known && !by_blocks) {
        static const uint64_t pct = getenv("RX_VK_COMPACT_PCT") ? (uint64_t)std::clamp(atoi(getenv("RX_VK_COMPACT_PCT")), 50, 90) : 85;
        const uint64_t held = vram_compact::held_budget(budget, total_bytes, used, reclaim, allowed, pct);
        // (and what goes is the excess alone. Over its budget the cache is cut to 7/8 of it, below: right for a budget that
        // is a size, but this one is "what the textures hold less the excess", crossed again whenever anything else grows.
        // A recorder started beside the game took 0.8 GB of the allowance as play came back from 14 s in the menu: the line
        // was crossed on five passes in a row and the cache went from 4,589 to 2,312 MB (0.875^5) in two seconds, the
        // textures made again at 65-90 ms a frame)
        if (held < budget) budget = evict_to = held;
        hold_to = evict_to;
        // (and a reserve under that line, for the textures long unused: vram_compact.h. The pass below takes the others only
        // for the hold's own line)
        const uint64_t reserve = vram_compact::held_budget(budget, total_bytes, used, reclaim, allowed, vram_compact::reserve_pct(pct));
        if (reserve < budget) budget = evict_to = reserve;
    }
    // (hookshot) not while VMA's blocks hold an eighth of the allowance free: new images take that space, the driver's count does
    // not grow, and what this evicts frees space inside the blocks that the count keeps. Ten frames later it squeezed again: a
    // ratchet. On a 4 GB RX 5500M (3337 MB used of 3337, 1224 MB free in the blocks) a paused game, whose scene draws nothing,
    // lost 1700 MB of textures in seconds, made again on unpausing (the long wait), and the HUD's drawn textures went with them.
    // But only while the count is within the allowance: past it the driver pages VRAM out to system memory, whatever room the
    // blocks hold (#2156: an RTX 5050 8 GB at 7602 MB used of 6982 allowed, 1649 MB free in the blocks, paged 4.5 GB and ran at
    // 14-25 fps; v2.0.14, without this wait, squeezed there. Ported from Revens2's a4c7cb3). RX_VK_SQUEEZE_ALWAYS=1: as before
    static const bool squeeze_always = env_on("RX_VK_SQUEEZE_ALWAYS");
    const bool room_in_blocks = !squeeze_always && used <= allowed && block_free >= allowed / 8;
    // (nor while the textures' pool is down to its blocks made at the start, which VMA keeps: what this evicts would leave the
    // count as it is, the same ratchet, past the allowance too: there no eviction can bring the count down (#2711).
    // RX_VK_SQUEEZE_ALWAYS=1 squeezes there too)
    if (known && !by_blocks && !(kept_only && !squeeze_always) && used > allowed / 100 * 97 && !room_in_blocks) {
        const uint64_t over = used - allowed / 100 * 97;
        budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
        rule_budget = std::min(rule_budget, total_bytes > over ? total_bytes - over : 0);
    }
    // (hookshot) and Windows' commit limit (RAM plus the page file): every VRAM block is charged to the process's commit, as the
    // guest's memory is. A run on an RTX 4060 Ti held private commit at the VRAM blocks plus ~21 GB the whole way (19.3 GB at the
    // start, 35.4 GB in Hyrule Field with 13.3 GB of blocks); a 16 GB PC with a fixed 32 GB page file reached its 47.9 GB limit at
    // 33.9 GB of its own, a guest commit failed while an area loaded, and the game's next write there crashed it. Below the reserve
    // the shortfall comes off the textures' budget, as the driver's limit's excess does above; the eviction and the compaction
    // then give blocks back. RX_VK_COMMIT_RESERVE_MB=<MB> (default 2560; 0: off)
    {
        uint64_t commit_free = 0;
        const uint64_t reserve = commit_reserve();
        const bool headroom_known = reserve && commit_headroom(&commit_free);
        oom_reserve_tend(headroom_known, commit_free, reserve);
        if (headroom_known && commit_free < reserve) {
            const uint64_t over = reserve - commit_free;
            budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
            rule_budget = std::min(rule_budget, total_bytes > over ? total_bytes - over : 0);
            static uint64_t said_at;
            if (!said_at || frame_no - said_at >= 600) {
                said_at = frame_no;
                logf("vk: commit limit near: %llu MB of commit left (reserve %llu MB); the textures' budget down to %llu MB of "
                     "their %llu MB (RX_VK_COMMIT_RESERVE_MB)", (unsigned long long)(commit_free >> 20),
                     (unsigned long long)(reserve >> 20), (unsigned long long)(budget >> 20),
                     (unsigned long long)(total_bytes >> 20));
            }
        }
    }
    // (hookshot) the 90% rule and the driver's limit let go of their excess, not of an eighth of the cache, while that excess is
    // small (a large one is cut to 7/8 as before): vram_compact.h. RX_VK_EVICT_EXCESS=0: always to 7/8 of their budget, as before
    static const bool excess_only = !getenv("RX_VK_EVICT_EXCESS") || atoi(getenv("RX_VK_EVICT_EXCESS")) != 0;
    const uint64_t rule_to = excess_only ? vram_compact::rule_target(total_bytes, size_budget, rule_budget)
                                         : vram_compact::rule_target(total_bytes, rule_budget);
    // (hookshot, RX_VK_COMPACT: the driver's limit asks for more than the reserve does: a pass over the budget, to 7/8 of it, as
    // before. Rare once the reserve is set: its line is 17 points under the limit's and counts the textures' free space out, so
    // this takes more than that share of the allowance free in their pool. Otherwise the pass is the hold's, and what the
    // limit and the 90% rule ask for goes by rule_to)
    if (budget < evict_to) evict_to = hold_to = 0;
    if (frame_no % 600 == 0) {
        vma_log_heaps();
        vma_log_image_blocks();
        {
            std::lock_guard lk(pool_mu);
            logf("vk: render target images: %llu reused from the pool, %llu made new, %llu destroyed from it; it holds %llu "
                 "(%llu MB)%s", (unsigned long long)st_pool_reused, (unsigned long long)st_pool_made,
                 (unsigned long long)st_pool_destroyed, (unsigned long long)pool_n, (unsigned long long)(pool_bytes >> 20),
                 pool_cap() ? (pool_pressed ? ", VRAM short: given back" : "") : " (RX_VK_TARGET_POOL_MB=0)");
        }
        logf("vk: VRAM: %llu MB used of %llu MB allowed (%llu MB of it free in the blocks); textures %llu MB (budget %llu "
             "MB), %zu textures%s", (unsigned long long)(used >> 20), (unsigned long long)(allowed >> 20),
             (unsigned long long)(block_free >> 20), (unsigned long long)(total_bytes >> 20),
             (unsigned long long)(budget >> 20), textures.size(),
             known && used > allowed / 100 * 97 && room_in_blocks ? " (at the driver's limit, room in the blocks: not squeezed)" : "");
        // (what takes it: decoded ASTC, the GPU's images, the rest; the largest)
        uint64_t astc_b = 0, rt_b = 0, other_b = 0, recent_b = 0, baked_b = 0;
        size_t baked_n = 0;
        std::vector<const Texture*> big, baked;
        size_t bucket_n[4]{}, bucket_rel[4]{};
        uint64_t bucket_b[4]{};
        for (auto& kv : textures) {
            const Texture* t = kv.second.get();
            (t->astc ? astc_b : t->gpu_written ? rt_b : other_b) += t->bytes;
            if (frame_no - t->last_frame < 8) recent_b += t->bytes;
            if (t->gpu_written && t->first_written && t->first_written == t->last_written && t->bytes <= (4ull << 20)) {
                if (!t->released) baked_n++, baked_b += t->bytes;
                const int k = t->bytes <= (64u << 10) ? 0 : t->bytes <= (256u << 10) ? 1 : t->bytes <= (1u << 20) ? 2 : 3;
                bucket_n[k]++, bucket_b[k] += t->bytes, bucket_rel[k] += t->released;
                baked.push_back(t);
            }
            big.push_back(t);
        }
        if (env_on("RX_VK_LOG_BAKED")) {   // (what the textures drawn in one frame are: by size, finalized or not)
            logf("vk: drawn in one frame: <=64 KB %zu (%llu KB, %zu finalized), <=256 KB %zu (%llu KB, %zu), <=1 MB %zu "
                 "(%llu KB, %zu), <=4 MB %zu (%llu KB, %zu)", bucket_n[0], (unsigned long long)(bucket_b[0] >> 10),
                 bucket_rel[0], bucket_n[1], (unsigned long long)(bucket_b[1] >> 10), bucket_rel[1], bucket_n[2],
                 (unsigned long long)(bucket_b[2] >> 10), bucket_rel[2], bucket_n[3], (unsigned long long)(bucket_b[3] >> 10),
                 bucket_rel[3]);
            std::sort(baked.begin(), baked.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
            for (size_t i = 0; i < baked.size() && i < 10; i++) {
                const Texture* t = baked[i];
                logf("vk:   one frame: #%u %#llx %ux%ux%u x%u levels %u NVN %#x %llu KB, written at %llu, made at %llu, last "
                     "used %llu%s%s", t->id, (unsigned long long)t->gpu, t->width, t->height, t->depth, t->layers, t->levels,
                     t->nvn_format, (unsigned long long)(t->bytes >> 10), (unsigned long long)t->first_written,
                     (unsigned long long)t->first_seen, (unsigned long long)t->last_frame, t->released ? " finalized" : "",
                     t->first_render_frame ? " rendered" : " copied or stored");
            }
        }
        std::sort(big.begin(), big.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
        logf("vk: textures: decoded ASTC %llu MB, rendered %llu MB (%zu drawn in one frame, kept: %llu KB), other %llu MB; "
             "used in the last 8 presents %llu MB", (unsigned long long)(astc_b >> 20), (unsigned long long)(rt_b >> 20),
             baked_n, (unsigned long long)(baked_b >> 10), (unsigned long long)(other_b >> 20),
             (unsigned long long)(recent_b >> 20));
        for (size_t i = 0; i < big.size() && i < 8; i++)
            logf("vk:   %llu MB: %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, last used %llu presents ago",
                 (unsigned long long)(big[i]->bytes >> 20), (unsigned long long)big[i]->gpu, big[i]->width, big[i]->height,
                 big[i]->depth, big[i]->layers, big[i]->levels, big[i]->nvn_format, big[i]->astc ? " ASTC" : "",
                 big[i]->gpu_written ? " rendered" : "", (unsigned long long)(frame_no - big[i]->last_frame));
    }
    if (verify_dim().first && frame_no % 300 == 0) {
        int checked = 0, changed = 0, missed = 0;
        for (auto& kv : textures) {
            Texture* t = kv.second.get();
            if (t->width != verify_dim().first || t->height != verify_dim().second || !t->verify_hash || t->gpu_written) continue;
            checked++;
            const uint64_t h = guest_hash(*t);
            if (h == t->verify_hash) continue;
            changed++;
            static thread_local Ranges d;
            if (!texture_dirty(*t, t->upload_gen, d)) {
                missed++;
                static int logged;
                if (logged++ < 40) {
                    logf("vk: MISSED WRITE: texture #%u %#llx %ux%u NVN %#x: its memory changed since its upload, the write "
                         "tracking saw nothing", t->id, (unsigned long long)t->gpu, t->width, t->height, t->nvn_format);
                    verify_report(*t);
                }
            }
        }
        logf("vk: verify %ux%u: %d textures, %d changed since uploaded, %d of them unnoticed", verify_dim().first,
             verify_dim().second, checked, changed, missed);
    }
    static const bool no_evict = env_on("RX_VK_NO_EVICT");   // (an experiment: nothing evicted)
    // (hookshot) under the budget, every 600 presents: the render targets drawn over since (below), unused for 120
    const bool sweep = total_bytes <= budget;
    // (hookshot-amd) and every 60 presents for the render targets drawn over by one at another start (below): OoT makes a frame's
    // transient targets (768 x 432 RGBA16F, 1536 x 864) in a scratch heap whose offsets shift with what came before them in the
    // frame, so turning the camera made new ones each frame ~1.2 MB from the last, each a new image. Left for the 600-present
    // sweep or the budget, ~200 dead ones (1.2 GB) filled an 8 GB card's VRAM in Hyrule Field, the driver paged it out and the
    // GPU's frame went from 15 to 35 ms. RX_VK_DEAD_TARGETS=0: as before
    static const bool dead_targets = !getenv("RX_VK_DEAD_TARGETS") || atoi(getenv("RX_VK_DEAD_TARGETS")) != 0;
    // (hookshot) every 10 presents (RX_VK_DEAD_EVERY; 60 before): the target pool gets the dead ones as the game makes new ones,
    // not 300-700 at once every second, of which the new ones of the next second missed most. The choice is cheap: 1930 textures
    // ordered in 0.3 ms, 251 chosen in 0.2 (RTX 5070, Ryzen 7 9800X3D)
    static const uint64_t dead_every =
        getenv("RX_VK_DEAD_EVERY") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_DEAD_EVERY"))) : 10;
    if (no_evict || (sweep && frame_no % (dead_targets ? dead_every : 600))) return;
    std::vector<std::pair<uint64_t, Texture*>> order;
    order.reserve(textures.size());
    for (auto& kv : textures) order.push_back({kv.second->last_frame, kv.second.get()});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // (the rendered textures by where their storage starts, for drawn_over_shifted)
    std::vector<const Texture*> by_start;
    if (dead_targets) {
        for (auto& [last, tex] : order)
            if (tex->gpu_written && tex->first_render_frame && tex->guest_size) by_start.push_back(tex);
        std::sort(by_start.begin(), by_start.end(), [](const Texture* a, const Texture* b) { return a->gpu < b->gpu; });
    }
    uint64_t longest = 0;
    for (const Texture* t : by_start) longest = std::max(longest, t->guest_size);
    std::unordered_set<Texture*> gone;
    // (over the budget, what goes has gone unused for RX_VK_EVICT_MIN_UNUSED frames, 60 by default; it was 8. On an
    // 8 GB laptop card the VRAM sits at the driver's limit, the budget shrinks and this runs every 10 frames: with 8,
    // ~300 textures in a few minutes of Kokiri Forest were evicted and made again, all of them within 60 frames (200
    // within 10), uploaded and decoded again each time; and a 260 x 260 one the GPU renders and the game samples every
    // 40 frames came back empty each time (22 times; the lit ground black for a frame, a tiled pattern over the ground).
    // With 60: 2. The textures that are really done with still go, 52 frames later)
    static const uint64_t min_unused =
        getenv("RX_VK_EVICT_MIN_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_EVICT_MIN_UNUSED"))) : 60;
    // (hookshot) a render target another image has since been drawn over (same storage, another size or format:
    // OoT's dynamic resolution makes a new set at each size in the same pool memory) holds nothing any more: it
    // goes like any other. Kept, they were 3.6 GB of 4.5 GB rendered, VRAM near the driver's limit, and the
    // driver's paging made submissions of seconds (5-9 fps for a minute)
    const auto drawn_over = [&](const Texture* tex) {
        if (!tex->gpu_written) return false;
        auto [a, e] = by_gpu.equal_range(tex->gpu);
        for (auto o = a; o != e; ++o)
            if (o->second != tex && o->second->last_written > tex->last_frame) return true;
        return false;
    };
    // (hookshot-amd) a render target of one level and layer whose storage newer render targets drew over from another start
    // after its last use (target_overlap.h): the transient targets above. Its texels are gone from that memory on the console
    // too. Their shares are added up, several over parts of it: judged one by one (an eighth each) they left the big ones, and
    // what was missed grew to 3-4 GB of target images (issue 2180). A newer target inside it is a view of a part of it, not a
    // new target (5bfa6aa, PR #899): counted, a live scene target would go on its sub-rects being drawn as views
    const auto drawn_over_shifted = [&](const Texture* tex) {
        if (!dead_targets || !tex->gpu_written || !tex->first_render_frame || !tex->guest_size || tex->levels != 1 ||
            tex->layers != 1 || tex->depth > 1)
            return false;
        const uint64_t lo = tex->gpu, hi = tex->gpu + tex->guest_size;
        auto it = std::lower_bound(by_start.begin(), by_start.end(), lo > longest ? lo - longest : 0,
                                   [](const Texture* t, uint64_t g) { return t->gpu < g; });
        target_overlap::DrawnOver over;
        for (; it != by_start.end() && (*it)->gpu < hi; ++it)
            if (*it != tex)
                over.add(lo, hi, tex->last_frame, (*it)->gpu, (*it)->gpu + (*it)->guest_size, (*it)->last_written);
        return over.an_eighth(hi - lo);
    };
    // (the transient targets: a frame's own, not sampled again, their storage drawn over by another since. 16 frames left
    // ~500 MB of them in Hyrule Castle Town's market at 20 fps (59 768 x 432 RGBA16F, 135 384 x 384, 41 1536 x 864 at a
    // time), VRAM at the driver's limit; the GPU's own use is checked below (last_value). RX_VK_DEAD_MIN_UNUSED=<frames>, 3)
    static const uint64_t dead_min_unused =
        getenv("RX_VK_DEAD_MIN_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_DEAD_MIN_UNUSED"))) : 3;
    // (hookshot) a small texture the GPU drew in one frame that the game has finalized, with nothing drawn over its storage
    // since: the game may make a texture over that storage again and sample it without drawing it. OoT's terrain does that with its
    // tiles' 260 x 260 RG8 normal maps: drawn when a chunk comes in, finalized when it goes out of range, made again and sampled as
    // they are when it comes back. On the console the texels are still in that memory; here they were only in the image, and one
    // evicted came back empty (made again over storage the GPU rendered, not uploaded): streaks over the ground in square patches
    // after a while of play (issues 107 and 386). Kept, up to RX_VK_KEEP_FINALIZED_MB (512 by default; 0:
    // evicted as before), the least recently used beyond that going as before. (Not written back into the memory instead: the game
    // may have put other things there since, and a write-back over scene data once crashed it)
    static const uint64_t keep_finalized_max =
        (uint64_t)(getenv("RX_VK_KEEP_FINALIZED_MB") ? std::max(0, atoi(getenv("RX_VK_KEEP_FINALIZED_MB"))) : 512) << 20;
    const auto keep_finalized = [&](const Texture* tex) {
        if (!keep_finalized_max || !tex->gpu_written || !tex->released || !tex->first_written ||
            tex->first_written != tex->last_written || tex->bytes > (4ull << 20))
            return false;
        const auto g = gpu_owned.find(tex->gpu);   // (its storage's range still the one the GPU drew it in)
        if (g == gpu_owned.end() || g->second.first != tex->gpu + std::max<uint64_t>(tex->guest_size, 1)) return false;
        return !drawn_over(tex) && !drawn_over_shifted(tex);
    };
    uint64_t finalized_kept = 0;
    size_t finalized_kept_n = 0;
    if (keep_finalized_max)
        for (auto& [last, tex] : order)
            if (keep_finalized(tex)) finalized_kept += tex->bytes, finalized_kept_n++;
    if (finalized_kept && frame_no % 600 == 0) {
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, std::pair<size_t, uint64_t>> kinds;   // (w, h, layers, NVN format)
        for (auto& [last, tex] : order)
            if (keep_finalized(tex)) {
                auto& k = kinds[{tex->width, tex->height, tex->layers, tex->nvn_format}];
                k.first++, k.second += tex->bytes;
            }
        char line[512];
        int n = 0;
        for (const auto& [k, v] : kinds)
            if (n < 440)
                n += snprintf(line + n, sizeof line - n, " %ux%ux%u NVN %#x: %zu (%llu KB);", std::get<0>(k), std::get<1>(k),
                              std::get<2>(k), std::get<3>(k), v.first, (unsigned long long)(v.second >> 10));
        logf("vk: finalized textures the GPU drew once, kept for being made again: %zu (%llu KB):%s", finalized_kept_n,
             (unsigned long long)(finalized_kept >> 10), n ? line : "");
    }
    const uint64_t need_unused = sweep ? std::max<uint64_t>(120, min_unused) : min_unused;
    const uint64_t need_dead = dead_targets ? std::min(need_unused, dead_min_unused) : need_unused;
    static uint64_t n_dead, b_dead, dead_logged_at;
    // (hookshot) the textures to go are chosen first and destroyed after; the write-backs among them go to the GPU in a few
    // batches (flush_textures), not one submission and one wait each: a sweep of 500 at a scene load stalled for seconds
    struct Evict { Texture* tex; bool flush, shifted, overwritten; };
    std::vector<Evict> evict;
    uint64_t pending_bytes = 0;   // (chosen below, their bytes not yet off total_bytes)
    // (hookshot, RX_VK_COMPACT) a pass the compaction's hold asked for (evict_to) lets go of the dead render targets as a sweep
    // does, all of them, and of live textures only for the excess left after those, and only of ones long unused: vram_compact.h's
    // hold_met and hold_target. So it does not stop at the first texture past the excess: what the dead targets hold is counted first
    const bool by_hold = evict_to != 0 && !sweep;
    uint64_t hold_dead = 0, hold_live = 0;   // (what the dead targets hold; the live textures chosen so far)
    if (by_hold)
        for (auto& [last, tex] : order) {
            if (frame_no - last < need_dead) break;
            if (tex->last_value > completed) continue;
            const bool shifted = drawn_over_shifted(tex);
            if (frame_no - last < need_unused && !shifted) continue;
            if (shifted || drawn_over(tex)) hold_dead += tex->bytes + tex->pad;
        }
    for (auto& [last, tex] : order) {
        // (total_bytes can have drifted below the textures' sum (destroy_later clamps it): the chosen ones' bytes past it mean the
        // budget is met, not a wrapped difference that never is)
        if ((!sweep && !by_hold && (pending_bytes >= total_bytes || total_bytes - pending_bytes <= budget / 8 * 7)) ||
            frame_no - last < need_dead)
            break;
        const bool shifted = drawn_over_shifted(tex);
        if (frame_no - last < need_unused && !shifted) continue;   // (younger: only a dead transient target goes)
        if (tex->last_value > completed) continue;
        // (nor what the GPU wrote and the game still has: the image is its only copy, and one made again over that
        // storage started empty or from stale memory: a sky or course texture drawn once, garbage from then on.
        // RX_VK_EVICT_RENDERED=1: as before)
        static const bool evict_rendered = env_on("RX_VK_EVICT_RENDERED");
        // (one unused for 3600 presents (a minute) goes: a course left behind; the game draws it again when it comes
        // back to that course. Without it a course change kept 1.4 GB arrays and VRAM ran over)
        // (600 presents near what the driver allows, at the budget's 97%: past it the driver pages VRAM to system memory.
        // AMD's driver reports the use at most at what it allows, so "over it" never held there: keep_rendered.h.
        // RX_VK_KEEP_RENDERED_OVER=1: only past what it allows, as before)
        static const bool keep_over_only = env_on("RX_VK_KEEP_RENDERED_OVER");
        const uint64_t keep_rendered = keep_rendered::presents(known, used, allowed, keep_over_only);
        const bool overwritten = shifted || drawn_over(tex);   // (above)
        // (hookshot, RX_VK_COMPACT: the hold's pass, above. A live texture stays once the excess over its line is covered: the
        // reserve's line for one long unused, the hold's own for one unused ten seconds, and what the other rules ask (the 90%
        // rule, the budget's size, the driver's limit) for one unused as long as an eviction asks with the compaction off)
        if (by_hold && !overwritten) {
            const uint64_t to = vram_compact::hold_target(frame_no - last, need_unused, rule_to, hold_to, evict_to);
            if (to == UINT64_MAX || vram_compact::hold_met(total_bytes, hold_live, hold_dead, to)) continue;
        }
        if (finalized_kept && keep_finalized(tex)) {   // (above. Oldest first: beyond the cap the least recently used go)
            if (finalized_kept <= keep_finalized_max) continue;
            finalized_kept -= tex->bytes;
            static int logged;
            if (logged++ < 100)
                logf("vk: kept finalized textures over %llu MB: #%u %#llx %ux%u NVN %#x goes (unused for %llu frames)",
                     (unsigned long long)(keep_finalized_max >> 20), tex->id, (unsigned long long)tex->gpu, tex->width,
                     tex->height, tex->nvn_format, (unsigned long long)(frame_no - last));
        }
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && frame_no - last < keep_rendered) continue;
        if (sweep && !overwritten) continue;
        // (hookshot) nor a live texture the GPU drew whose texels cannot be written back into its memory (ASTC kept as BC3, sparse,
        // past the write-back's size, ...): its image is the only copy, and one made again over that storage is not uploaded (the
        // memory counts as the GPU's), so it showed whatever its new memory held: the rainbow/garbled textures that spread over play
        // time on 4-8 GB cards (issue 993)? Up to 256 MB of such textures each stay; bigger ones (a whole area left behind) go as
        // before. On by default since 2026-11-09 (8 GB RTX 3070s still showed rainbow textures once VRAM filled, Discord): the VRAM it
        // costs is bounded (256 MB a texture) and a wrong texture is worse than a fuller budget. RX_VK_EVICT_UNFLUSHABLE=1: evicted, as before
        static const bool evict_unflushable = getenv("RX_VK_EVICT_UNFLUSHABLE") && atoi(getenv("RX_VK_EVICT_UNFLUSHABLE")) != 0;
        if (!overwritten && tex->gpu_written && !tex->released && !evict_unflushable && tex->bytes < (256ull << 20) && !flushable(*tex)) {
            static std::atomic<int> kept_logged{0};
            if (kept_logged.fetch_add(1) < 40)
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x the GPU drew kept, not evicted: its texels cannot be written back",
                     tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format);
            continue;
        }
        // (and a small one never: its texels exist nowhere else, and it is made once and kept by the game. The sky's
        // 256 x 256 lookup table, unused while a menu was up, went and came back from memory as garbage: the rainbow
        // sky. The big ones that may go are a course left behind)
        // (a render target, drawn in the frames it was used in, goes: the dynamic resolution makes a new set of ~20 at
        // each size, and the old ones are not used again)
        const bool drawn_once = tex->last_written + 8 < tex->last_frame;
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && tex->bytes < (64ull << 20) && drawn_once) continue;
        // (nor a small one the GPU wrote in one frame alone: a table the game bakes once and samples now and then. The
        // pause screen's colour grading, 8 x 8 x 8, its slices drawn at boot and sampled next when the game is paused,
        // went after a minute unused, and the one made again over storage only the GPU had written stayed empty: the
        // pause blur black. Not a finalized one: the transient targets the game makes and finalizes every frame are
        // each "written in one frame", and kept so they added up, ~20 MB a minute)
        const bool baked = tex->first_written && tex->first_written == tex->last_written && tex->bytes <= (4ull << 20);
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && baked) continue;
        // (a texture the GPU drew that still holds the game's content goes into the game's memory first: made again later, it is
        // uploaded from there with what the GPU drew. Evicted without it, it came back empty or stale: a terrain chunk dark, a
        // tiled pattern over the Deku Tree's planks and Hyrule Field's path, with a 16 GB card's textures at the 6 GB budget
        // (issue 386). RX_VK_EVICT_NO_FLUSH=1: not written back, as before)
        static const bool no_flush = env_on("RX_VK_EVICT_NO_FLUSH");
        evict.push_back({tex, !no_flush && !overwritten && tex->gpu_written && !tex->released && flushable(*tex), shifted, overwritten});
        pending_bytes += tex->bytes + tex->pad;
        if (by_hold && !overwritten) hold_live += tex->bytes + tex->pad;
    }
    evict_reason::Tally tally;                     // (why each one goes: the retired line below)
    std::unordered_set<const Texture*> flushed_ok;   // (the write-backs that worked)
    {   // (the write-backs: batches of up to 64 textures or ~512 MB of staging memory, each one submission and one wait)
        std::vector<Flush> batch;
        uint64_t batch_bytes = 0;
        const auto run = [&] {
            if (batch.empty()) return;
            flush_textures(batch);
            for (const Flush& f : batch)
                if (f.ok) {
                    flushed_ok.insert(f.t);
                    static std::atomic<int64_t> flushed{0};
                    if (++flushed % 50 == 1) logf("vk: textures the GPU drew, written back into their memory before eviction: %lld so far", (long long)flushed.load());
                }
            batch.clear();
            batch_bytes = 0;
        };
        for (const Evict& e : evict) {
            if (!e.flush) continue;
            batch.push_back(Flush{e.tex, e.tex->gpu, e.tex->gpu + e.tex->guest_size});
            batch_bytes += e.tex->guest_size;   // (what its staging buffer holds: the texels as the storage lays them out)
            if (batch.size() >= 64 || batch_bytes >= (512ull << 20)) run();
        }
        run();
    }
    for (const Evict& e : evict) {   // (and each one's bookkeeping and its end, as before)
        Texture* tex = e.tex;
        if (e.shifted) n_dead++, b_dead += tex->bytes;
        {   // (hookshot) why it goes, and whether its texels go with it (evict_reason.h). The flush above can fail: then they did
            const bool lost = evict_reason::texels_lost(tex->gpu_written, e.overwritten, e.flush && flushed_ok.count(tex));
            tally.add(evict_reason::classify(e.shifted, e.overwritten), lost, tex->bytes, tex->released);
            static int lost_logged;
            if (lost && lost_logged++ < 100)
                // (always over the budget: a sweep lets only drawn-over textures go, and those lose nothing)
                logf("vk: texture #%u %#llx %ux%ux%u x%u NVN %#x the GPU drew goes with no copy of its texels (%s, unused for %llu "
                     "frames, over the budget): made again over this storage, it starts empty", tex->id, (unsigned long long)tex->gpu,
                     tex->width, tex->height, tex->depth, tex->layers, tex->nvn_format, tex->released ? "finalized" : "live",
                     (unsigned long long)(frame_no - tex->last_frame));
        }
        watch_tex(tex, "evicted");
        if (!tex->gpu_written && tex->uploaded && !remade_skip_old()) {   // (above: made again, it is uploaded from memory)
            std::lock_guard lk(from_memory_mu);
            if (evicted_from_memory.size() > 50000) evicted_from_memory.clear();   // (a bound)
            evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;
        }
        if (remade_log_on()) {
            std::lock_guard lk(evicted_mu);
            if (evicted_at.size() > 20000) evicted_at.clear();   // (a bound: only the recent ones matter)
            evicted_at[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = {frame_no, tex->gpu_written};
            remade.erase(tex);
        }
        destroy_later(*tex);
        forget_gpu(tex);
        gone.insert(tex);
    }
    for (auto it = textures.begin(); it != textures.end() && !gone.empty();) {
        if (gone.erase(it->second.get())) it = textures.erase(it);
        else ++it;
    }
    if (n_dead && frame_no >= dead_logged_at + 600) {   // (the dead transient targets let go so far, every 600 presents)
        dead_logged_at = frame_no;
        logf("vk: render targets drawn over from another start, let go: %llu so far (%llu MB)", (unsigned long long)n_dead,
             (unsigned long long)(b_dead >> 20));
    }
    const size_t n = order.size() - textures.size();
    if (n) {
        char why[160];
        evict_reason::describe(tally, why, sizeof why);
        logf("vk: %zu textures retired (%zu left, %llu MB): %s", n, textures.size(), (unsigned long long)(total_bytes >> 20), why);
    }
}

void texture_log_guest(const Texture* t) {
    if (!t || !t->guest_size) return;
    std::vector<uint8_t> b(65536);
    char lv[512] = "";
    int k = 0;
    uint64_t all_nz = 0, all_unmapped = 0;
    for (uint32_t l = 0; l < std::max(1u, t->d.levels) && l < 16; l++) {
        const uint64_t lo = host->tex_level_offset(&t->d, l);
        const uint64_t hi = l + 1 < std::max(1u, t->d.levels) ? host->tex_level_offset(&t->d, l + 1) : t->guest_size;
        uint64_t nz = 0, unmapped = 0, pages = 0, empty_pages = 0;
        for (uint64_t off = lo; off < hi; off += 65536) {
            const uint64_t n = std::min<uint64_t>(65536, hi - off);
            if (!host->gpu_read(t->gpu + off, b.data(), n)) { unmapped += n; continue; }
            uint64_t page_nz = 0;
            for (uint64_t i = 0; i < n; i++) page_nz += b[i] != 0;
            nz += page_nz, pages++, empty_pages += !page_nz;
        }
        all_nz += nz, all_unmapped += unmapped;
        if (k < 480)
            k += snprintf(lv + k, sizeof lv - k, " L%u: %llu KB, %llu not zero, %llu of %llu mapped pages empty%s;", l,
                          (unsigned long long)((hi - lo) >> 10), (unsigned long long)nz, (unsigned long long)empty_pages,
                          (unsigned long long)pages, unmapped ? " (part unmapped)" : "");
    }
    const uint64_t g0 = host->gpu_guest(t->gpu);
    logf("vk: dump guest memory of #%u %#llx %ux%u NVN %#x (guest %#llx%s): %llu bytes not zero, %llu KB unmapped;%s", t->id,
         (unsigned long long)t->gpu, t->width, t->height, t->nvn_format, (unsigned long long)g0,
         t->gpu >= (1ull << 40) ? ", a virtual pool's" : "", (unsigned long long)all_nz,
         (unsigned long long)(all_unmapped >> 10), lv);
}

void texture_released(const nvn_tex* t) {
    if (!t) return;
    auto [a, b] = by_gpu.equal_range(t->gpu);
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && !it->second->released) {
            it->second->released = true, it->second->release_gen = memory_generation();
            watch_tex(it->second, "finalized");
            static int logged;   // (a big one: the course maps' kind)
            if (it->second->bytes >= (32ull << 20) && logged++ < 40)
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x (%llu MB, rendered or copied into) finalized by the game",
                     it->second->id, (unsigned long long)it->second->gpu, it->second->width, it->second->height,
                     it->second->layers, it->second->nvn_format, (unsigned long long)(it->second->bytes >> 20));
        }
}

uint64_t texture_made() {
    return st_made;
}
void texture_unswizzle_stats(int64_t* bytes, int64_t* ns) {
    *bytes = st_unswizzle_bytes, *ns = st_unswizzle_ns;
}
size_t texture_count() {
    return textures.size();
}
uint64_t texture_bytes() {
    return total_bytes;
}
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded) {
    *uploads = st_uploads, *astc_gpu = st_astc, *bytes_uploaded = st_bytes;
}

bool texels_fit(const nvn_tex* t, const NVNcopyRegion* r, uint64_t row, uint64_t avail) {
    if (!t || !r) return false;
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return true;   // (texture_read_texels copies nothing)
    const uint64_t w = (uint64_t)((std::max(0, r->width) + bw - 1) / bw), h = (uint64_t)((std::max(0, r->height) + bh - 1) / bh);
    const uint64_t dd = (uint64_t)std::max(1, r->depth);
    if (!w || !h) return true;
    const uint64_t pitch = row ? row : w * bpb;
    uint64_t image = 0, a = 0, b = 0, n = 0;
    return !__builtin_mul_overflow(pitch, h, &image) && !__builtin_mul_overflow(dd - 1, image, &a) &&
           !__builtin_mul_overflow(h - 1, pitch, &b) && !__builtin_add_overflow(a, b, &n) &&
           !__builtin_add_overflow(n, w * bpb, &n) && n <= avail;
}

/// ReadTexels / WriteTexels: the CPU's copy of a region in guest memory (block linear, the Switch 2 GOB swizzle).
/// A write also stamps the pages, so the image is uploaded again; a texture the GPU wrote is read from guest memory
/// as it is (its newest texels are in the image: not yet read back).
void texture_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* data,
                         uint64_t row, uint64_t img, bool write) {
    if (!t || !t->gpu || !r) return;
    {   // (RX_VK_WATCH_RANGE: ReadTexels / WriteTexels of textures overlapping it, 400 lines)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && t->gpu < range.second && t->gpu + std::max<uint64_t>(t->L.size, 1) > range.first && logged++ < 400)
            logf("vk: watch range %s %#llx %ux%u NVN %#x level %u layer %u region (%d,%d %dx%d) mapped %s", write ? "WriteTexels" : "ReadTexels",
                 (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.format, level, layer0, r->xoffset, r->yoffset,
                 r->width, r->height, host->gpu_guest(t->gpu + host->tex_level_offset(&t->d, level)) ? "(level start yes)" : "(level start no)");
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return;
    const nvn_texdesc& d = t->d;
    const uint32_t lw = (std::max(1u, d.width >> level) + bw - 1) / bw;
    const uint32_t lh = d.target == NVN_TEXTURE_TARGET_1D || d.target == NVN_TEXTURE_TARGET_1D_ARRAY
                            ? 1 : (std::max(1u, d.height >> level) + bh - 1) / bh;
    const bool vol = d.target == NVN_TEXTURE_TARGET_3D;
    const uint64_t base = t->gpu + host->tex_level_offset(&d, level);
    unsigned th = 0, td = 0;
    host->tex_level_tiles(&d, level, &th, &td);
    const uint32_t x0 = (uint32_t)std::max(0, r->xoffset) / bw, y0 = (uint32_t)std::max(0, r->yoffset) / bh;
    const uint32_t w = (uint32_t)(std::max(0, r->width) + bw - 1) / bw, h = (uint32_t)(std::max(0, r->height) + bh - 1) / bh;
    const uint32_t z0 = (uint32_t)std::max(0, r->zoffset), dd = (uint32_t)std::max(1, r->depth);
    const uint64_t pitch = row ? row : (uint64_t)w * bpb, image = img ? img : pitch * h;
    const bool linear = nvn_tex_is_linear(&d);
    const uint32_t stride = linear ? (d.stride ? d.stride : host->tex_linear_stride(&d)) : 0;
    const uint64_t gobs_x = ((uint64_t)lw * bpb + 63) >> 6;
    const uint64_t rows_y = (lh + (8u << th) - 1) >> (3 + th);
    const uint64_t block = 512ull << (th + td);
    for (uint32_t k = 0; k < dd; k++) {
        const uint32_t z = vol ? z0 + k : 0;
        const uint64_t layer_base = base + (vol ? 0 : (uint64_t)(layer0 + z0 + k) * t->L.layer_size);
        for (uint32_t y = 0; y < h && y0 + y < lh; y++) {
            uint8_t* line = data + k * image + y * pitch;
            const uint32_t yy = y0 + y;
            for (uint32_t xb = 0; xb < (uint64_t)w * bpb;) {
                const uint32_t xbyte = x0 * bpb + xb;
                uint64_t off;
                uint32_t run;
                if (linear) {
                    off = (uint64_t)yy * stride + xbyte;
                    run = w * bpb - xb;
                } else {   // (16 bytes of a GOB row are contiguous)
                    const uint64_t gob = ((xbyte & 63) >> 5) << 8 | ((yy & 7) >> 2) << 7 | ((xbyte & 31) >> 4) << 6 |
                                         ((yy >> 1) & 1) << 5 | (yy & 1) << 4 | (xbyte & 15);
                    off = (uint64_t)(z >> td) * gobs_x * rows_y * block + (uint64_t)(yy >> (3 + th)) * gobs_x * block +
                          (uint64_t)(xbyte >> 6) * block + ((uint64_t)(z & ((1u << td) - 1)) << (9 + th)) +
                          ((uint64_t)((yy >> 3) & ((1u << th) - 1)) << 9) + gob;
                    run = std::min<uint32_t>(16 - (xbyte & 15), w * bpb - xb);
                }
                if (write) host->gpu_write(layer_base + off, line + xb, run);   // (the shadows hear of the whole: guest_cpu_wrote below)
                else host->gpu_read(layer_base + off, line + xb, run);
                xb += run;
            }
        }
    }
    if (write) {
        guest_cpu_wrote(t->gpu, t->L.size);   // (the write-back diff's shadows: one report for the 16-byte runs above)
        const uint64_t guest = host->gpu_guest(t->gpu);
        if (guest) memory_stamp(guest, t->L.size);
        auto [a, b] = by_gpu.equal_range(t->gpu);
        for (auto it = a; it != b; ++it) it->second->gpu_written = false, it->second->checked_epoch = 0;
    }
}

} // namespace mkwvk
