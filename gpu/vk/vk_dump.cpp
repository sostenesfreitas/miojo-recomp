// Frame dumps (vk_dump.h): each step's image blitted (nearest) into a small RGBA32 image of its numeric kind and copied
// to host memory with the submission; written once the submission completed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <direct.h>

#include "vk_dump.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

enum Kind { KIND_FLOAT, KIND_UINT, KIND_SINT };

struct Item {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation image_alloc = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation buffer_alloc = nullptr;
    void* map = nullptr;
    uint32_t w = 0, h = 0, src_w = 0, src_h = 0;
    Kind kind = KIND_FLOAT;
    bool rgb9e5 = false;   // (hookshot, RX_VK_DUMP_9E5: an R11G11B10 image's words read as RGB9E5)
    int depth_bytes = 0;   // (a depth image: copied as it is, w x h = the level's; 2 = D16, 4 = D32F or D24 in 32 bits)
    bool depth_float = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t gpu = 0;
    int seq = 0;
    std::string what;
};

std::vector<int64_t> targets;   // the presents whose next frame is dumped
std::vector<int> counts;        // (and how many frames from there: RX_VK_DUMP=<present>+<count>)
bool armed = false;
int64_t armed_present = 0, armed_until = 0;
int seq = 0;
std::vector<Item> items;
std::string dir;

Kind kind_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A8B8G8R8_UINT_PACK32:
        return KIND_UINT;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        return KIND_SINT;
    default:
        return KIND_FLOAT;
    }
}

uint32_t dump_width() {
    static const uint32_t w = getenv("RX_VK_DUMP_W") ? std::clamp(atoi(getenv("RX_VK_DUMP_W")), 16, 4096) : 320;
    return w;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// Completion alone does not make the staging copy available to host reads.
void host_read_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

uint8_t unit(float v) {
    return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255);
}

/// A depth item's texels as RGBA32F, scaled to at most dump_width() wide: depth in red, green and blue
void depth_to_float(Item& it, std::vector<float>& out) {
    const uint32_t w = std::min(it.src_w, dump_width());
    const uint32_t h = std::max(1u, (uint32_t)((uint64_t)it.src_h * w / it.src_w));
    out.assign((size_t)w * h * 4, 1.0f);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const size_t at = (size_t)(y * it.src_h / h) * it.src_w + (size_t)x * it.src_w / w;
            float v;
            if (it.rgb9e5) {
                const uint32_t word = ((const uint32_t*)it.map)[at];
                const float scale = std::ldexp(1.0f, (int)(word >> 27) - 15 - 9);
                float* o = &out[((size_t)y * w + x) * 4];
                o[0] = (float)(word & 0x1ff) * scale, o[1] = (float)((word >> 9) & 0x1ff) * scale;
                o[2] = (float)((word >> 18) & 0x1ff) * scale;
                continue;
            }
            if (it.depth_bytes == 2) v = ((const uint16_t*)it.map)[at] / 65535.0f;
            else if (it.depth_float) std::memcpy(&v, (const uint8_t*)it.map + at * 4, 4);
            else v = (((const uint32_t*)it.map)[at] & 0xFFFFFF) / 16777215.0f;
            float* o = &out[((size_t)y * w + x) * 4];
            o[0] = o[1] = o[2] = v;
        }
    it.w = w, it.h = h;
}

void write_item(Item& it) {
    std::vector<float> depth;
    if (it.depth_bytes) depth_to_float(it, depth), it.map = depth.data();
    const size_t n = (size_t)it.w * it.h;
    std::vector<uint8_t> rgba(n * 4, 255);
    char line[1024];
    if (it.kind == KIND_FLOAT) {
        const float* p = (const float*)it.map;
        int64_t nan = 0, inf = 0;
        double sum[4]{}, mx[4] = {-1e30, -1e30, -1e30, -1e30}, mn[4] = {1e30, 1e30, 1e30, 1e30};
        int64_t finite = 0;
        for (size_t i = 0; i < n; i++) {
            const float* q = p + i * 4;
            bool bad_nan = false, bad_inf = false;
            for (int c = 0; c < 4; c++) bad_nan |= std::isnan(q[c]), bad_inf |= std::isinf(q[c]);
            if (bad_nan || bad_inf) {
                nan += bad_nan, inf += bad_inf && !bad_nan;
                continue;
            }
            finite++;
            for (int c = 0; c < 4; c++) sum[c] += q[c], mx[c] = std::max(mx[c], (double)q[c]), mn[c] = std::min(mn[c], (double)q[c]);
        }
        const double top = std::max({mx[0], mx[1], mx[2]});
        const bool hdr = top > 1.0;
        for (size_t i = 0; i < n; i++) {
            const float* q = p + i * 4;
            uint8_t* o = &rgba[i * 4];
            bool bad_nan = false, bad_inf = false;
            for (int c = 0; c < 4; c++) bad_nan |= std::isnan(q[c]), bad_inf |= std::isinf(q[c]);
            if (bad_nan) { o[0] = 255, o[1] = 0, o[2] = 255; continue; }
            if (bad_inf) { o[0] = 0, o[1] = 255, o[2] = 0; continue; }
            for (int c = 0; c < 3; c++) {
                const float v = std::max(q[c], 0.0f);
                o[c] = unit(hdr ? v / (1.0f + v) : v);
            }
        }
        const double f = finite ? 1.0 / (double)finite : 0.0;
        int64_t ones = 0;   // (a depth image: texels still at the far plane, nothing drawn there)
        if (it.depth_bytes)
            for (size_t i = 0; i < n; i++) ones += p[i * 4] >= 1.0f;
        int ln = snprintf(line, sizeof line,
                 "vk: dump %03d %s: tex %#llx format %d %ux%u: NaN %lld, Inf %lld of %lld; mean %.4g %.4g %.4g %.4g, "
                 "min %.4g %.4g %.4g %.4g, max %.4g %.4g %.4g %.4g",
                 it.seq, it.what.c_str(), (unsigned long long)it.gpu, (int)it.format, it.src_w, it.src_h, (long long)nan,
                 (long long)inf, (long long)n, sum[0] * f, sum[1] * f, sum[2] * f, sum[3] * f, mn[0], mn[1], mn[2], mn[3],
                 mx[0], mx[1], mx[2], mx[3]);
        if (it.depth_bytes && ln > 0 && ln < (int)sizeof line)
            snprintf(line + ln, sizeof line - ln, "; far %lld of %lld", (long long)ones, (long long)n);
    } else {
        const int32_t* p = (const int32_t*)it.map;
        int64_t mx[4] = {INT64_MIN, INT64_MIN, INT64_MIN, INT64_MIN}, mn[4] = {INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX};
        int64_t nonzero = 0;
        for (size_t i = 0; i < n; i++) {
            bool nz = false;
            for (int c = 0; c < 4; c++) {
                const int64_t v = it.kind == KIND_UINT ? (int64_t)(uint32_t)p[i * 4 + c] : (int64_t)p[i * 4 + c];
                mx[c] = std::max(mx[c], v), mn[c] = std::min(mn[c], v);
                nz |= c < 3 && v != 0;
            }
            nonzero += nz;
        }
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 3; c++) {
                const int64_t v = it.kind == KIND_UINT ? (int64_t)(uint32_t)p[i * 4 + c] : (int64_t)p[i * 4 + c];
                const int64_t range = std::max<int64_t>(1, mx[c] - mn[c]);
                rgba[i * 4 + c] = (uint8_t)((v - mn[c]) * 255 / range);
            }
        snprintf(line, sizeof line,
                 "vk: dump %03d %s: tex %#llx format %d %ux%u: %s, %lld of %lld texels not zero; min %lld %lld %lld %lld, "
                 "max %lld %lld %lld %lld",
                 it.seq, it.what.c_str(), (unsigned long long)it.gpu, (int)it.format, it.src_w, it.src_h,
                 it.kind == KIND_UINT ? "uint" : "sint", (long long)nonzero, (long long)n, (long long)mn[0], (long long)mn[1],
                 (long long)mn[2], (long long)mn[3], (long long)mx[0], (long long)mx[1], (long long)mx[2], (long long)mx[3]);
    }
    // (RX_VK_DUMP_PIXELS=x,y;x,y... (or x,y/x,y): those texels too, in the dump's scaled picture)
    static const std::vector<std::pair<uint32_t, uint32_t>> pixels = [] {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        if (const char* e = getenv("RX_VK_DUMP_PIXELS"))
            for (const char* q = e; *q;) {
                unsigned x, y;
                if (sscanf(q, "%u,%u", &x, &y) != 2) break;
                v.push_back({x, y});
                const char* semi = strpbrk(q, ";/");
                if (!semi) break;
                q = semi + 1;
            }
        return v;
    }();
    for (const auto& [x, y] : pixels) {
        if (x >= it.w || y >= it.h) continue;
        const size_t i = (size_t)y * it.w + x;
        size_t len = strlen(line);
        if (it.kind == KIND_FLOAT) {
            const float* q = (const float*)it.map + i * 4;
            snprintf(line + len, sizeof line - len, " [%u,%u: %.5g %.5g %.5g %.5g]", x, y, q[0], q[1], q[2], q[3]);
        } else {
            const int32_t* q = (const int32_t*)it.map + i * 4;
            snprintf(line + len, sizeof line - len, " [%u,%u: %d %d %d %d]", x, y, q[0], q[1], q[2], q[3]);
        }
    }
    logf("%s", line);
    static const bool no_png = env_on("RX_VK_DUMP_NOPNG");   // (statistics only)
    if (no_png) return;
    // (RX_VK_DUMP_RAW: float pictures also as raw RGBA32F)
    char path[1024];
    snprintf(path, sizeof path, "%s/%03d_%s_%llx.png", dir.c_str(), it.seq, it.what.c_str(), (unsigned long long)it.gpu);
    for (char* c = path + dir.size() + 1; *c; c++)
        if (*c == ' ' || *c == ':' || *c == '/' || *c == '\\') *c = '_';
    write_png(path, rgba.data(), it.w, it.h);
    static const bool raw = env_on("RX_VK_DUMP_RAW");
    if (raw && it.kind == KIND_FLOAT) {
        std::string rp(path);
        rp.replace(rp.size() - 4, 4, ".f32");
        if (FILE* f = fopen(rp.c_str(), "wb")) {
            fwrite(&it.w, 4, 1, f), fwrite(&it.h, 4, 1, f);
            fwrite(it.map, 16, n, f);
            fclose(f);
        }
    }
}

} // namespace

bool dump_on() {
    return armed;
}

int dump_seq() {
    return seq;
}

void dump_present(int64_t presents) {
    static bool parsed;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("RX_VK_DUMP"))
            for (const char* q = e; *q;) {
                targets.push_back(atoll(q));
                while (*q && *q != ',' && *q != '+') q++;
                counts.push_back(*q == '+' ? std::max(1, atoi(q + 1)) : 1);
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
    }
    if (armed) logf("vk: dump of the frame after present %lld: %d steps so far", (long long)armed_present, seq);
    if (armed && presents < armed_until) return;   // (a run of frames: one folder, steps numbered on)
    armed = std::find(targets.begin(), targets.end(), presents) != targets.end();
    if (!armed) return;
    armed_present = presents;
    armed_until = presents + 1;
    for (size_t i = 0; i < targets.size(); i++)
        if (targets[i] == presents) armed_until = presents + counts[i];
    seq = 0;
    const char* base = getenv("RX_VK_DUMP_DIR") ? getenv("RX_VK_DUMP_DIR") : getenv("RX_VK_SHOTS");
    dir = std::string(base ? base : ".") + "/dump_" + std::to_string(presents);
    _mkdir(dir.c_str());
    logf("vk: dumping the frame after present %lld to %s", (long long)presents, dir.c_str());
}

void dump_request(int64_t present, int count) {
    dump_present(-1);   // (RX_VK_DUMP read first)
    targets.push_back(present), counts.push_back(std::max(1, count));
}

/// RX_VK_DUMP_STEPS=<lo>-<hi>: only those steps read back and written (full-size dumps of a few passes)
bool step_wanted(int step, const char* what) {
    // (RX_VK_DUMP_PROGS=<key>,...: only steps of those programs, wherever they fall in the frame)
    if (const char* want = getenv("RX_VK_DUMP_PROGS")) {
        for (const char* q = want; *q;) {
            const char* e = q;
            while (*e && *e != ',') e++;
            if (e > q && std::string(what).find(std::string(q, e)) != std::string::npos) return true;
            q = *e ? e + 1 : e;
        }
        return false;
    }
    static int lo = -1, hi = 1 << 30;
    if (lo == -1) {
        lo = 0;
        if (const char* e = getenv("RX_VK_DUMP_STEPS")) {
            lo = atoi(e);
            const char* d = strchr(e, '-');
            hi = d ? atoi(d + 1) : lo;
        }
    }
    return step >= lo && step <= hi;
}

void dump_image(VkCommandBuffer cmd, Texture* tex, uint32_t level, uint32_t layer, const char* what) {
    if (!armed || !tex || !tex->image || tex->samples != VK_SAMPLE_COUNT_1_BIT) return;
    if (!step_wanted(seq, what)) return void(seq++);
    // (RX_VK_DUMP_DEPTH_ONLY=1: the passes' colour targets left out, depth only (per-draw dumps of big targets fit))
    static const bool depth_only = env_on("RX_VK_DUMP_DEPTH_ONLY");
    if (depth_only && (tex->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && std::strncmp(what, "pass", 4) == 0) return void(seq++);
    // (RX_VK_DUMP_ONLY_W=<width>[,<width>...]: only targets that wide (the minimap's 468: its per-draw dumps alone,
    // small; the scene's and the output's: 2048,3840)
    static const std::vector<uint32_t> only_w = [] {
        std::vector<uint32_t> v;
        if (const char* e = getenv("RX_VK_DUMP_ONLY_W"))
            for (const char* q = e; *q;) {
                v.push_back((uint32_t)atoi(q));
                while (*q && *q != ',') q++;
                if (*q) q++;
            }
        return v;
    }();
    if (!only_w.empty() && std::find(only_w.begin(), only_w.end(), std::max(1u, tex->width >> std::min(level, tex->levels - 1))) ==
                               only_w.end())
        return void(seq++);
    static const bool as_9e5 = env_on("RX_VK_DUMP_9E5");
    const bool raw_9e5 = as_9e5 && tex->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    if (!(tex->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || raw_9e5) {
        if (!(tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !raw_9e5) return;
        Item it;
        it.rgb9e5 = raw_9e5;
        it.format = tex->format;
        it.gpu = tex->gpu;
        it.seq = seq++;
        it.what = std::string(what) + " #" + std::to_string(tex->id);
        level = std::min(level, tex->levels - 1);
        it.src_w = it.w = std::max(1u, tex->width >> level), it.src_h = it.h = std::max(1u, tex->height >> level);
        it.depth_bytes = tex->format == VK_FORMAT_D16_UNORM || tex->format == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4;
        it.depth_float = tex->format == VK_FORMAT_D32_SFLOAT || tex->format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        if (!create_buffer((uint64_t)it.w * it.h * it.depth_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, it.buffer,
                           it.buffer_alloc, &it.map))
            return;
        full_barrier(cmd);
        const VkBufferImageCopy c{0, 0, 0, {(VkImageAspectFlags)(raw_9e5 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT), level,
                                            std::min(layer, tex->layers - 1), 1}, {0, 0, 0},
                                  {it.w, it.h, 1}};
        vkCmdCopyImageToBuffer(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, it.buffer, 1, &c);
        host_read_barrier(cmd);
        full_barrier(cmd);
        items.push_back(std::move(it));
        return;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, tex->format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        logf("vk: dump %03d %s: tex %#llx format %d not blittable", seq, what, (unsigned long long)tex->gpu, (int)tex->format);
        seq++;
        return;
    }
    level = std::min(level, tex->levels - 1);
    Item it;
    it.kind = kind_of(tex->format);
    it.format = tex->format;
    it.gpu = tex->gpu;
    it.seq = seq++;
    it.what = std::string(what) + " #" + std::to_string(tex->id);
    it.src_w = std::max(1u, tex->width >> level), it.src_h = std::max(1u, tex->height >> level);
    it.w = std::min(it.src_w, dump_width());
    it.h = std::max(1u, (uint32_t)((uint64_t)it.src_h * it.w / it.src_w));
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = it.kind == KIND_UINT ? VK_FORMAT_R32G32B32A32_UINT
                : it.kind == KIND_SINT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    ci.extent = {it.w, it.h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(ci, it.image, it.image_alloc)) return;
    if (!create_buffer((uint64_t)it.w * it.h * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, it.buffer, it.buffer_alloc, &it.map)) {
        destroy_image(it.image, it.image_alloc);
        return;
    }
    full_barrier(cmd);
    barrier(cmd, it.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, tex->type == VK_IMAGE_TYPE_3D ? 0 : std::min(layer, tex->layers - 1), 1};
    const int32_t z = tex->type == VK_IMAGE_TYPE_3D ? (int32_t)std::min(layer, std::max(1u, tex->depth >> level) - 1) : 0;
    b.srcOffsets[0] = {0, 0, z};
    b.srcOffsets[1] = {(int32_t)it.src_w, (int32_t)it.src_h, z + 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {(int32_t)it.w, (int32_t)it.h, 1};
    vkCmdBlitImage(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, it.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_NEAREST);
    barrier(cmd, it.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {it.w, it.h, 1}};
    vkCmdCopyImageToBuffer(cmd, it.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, it.buffer, 1, &c);
    host_read_barrier(cmd);
    full_barrier(cmd);
    items.push_back(std::move(it));
}

namespace {
struct BufItem {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t n = 0;
    std::string what;
};
std::vector<BufItem> buf_items;
} // namespace

void dump_buffer(VkCommandBuffer cmd, VkBuffer src, VkDeviceSize off, VkDeviceSize n, const char* what) {
    if (!armed || !src || !n) return;
    n = std::min<VkDeviceSize>(n, 65536) & ~3ull;
    if (!n) return;
    BufItem it;
    it.n = n;
    it.what = what;
    if (!create_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, it.buffer, it.alloc, &it.map) || !it.map) return;
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy c{off, 0, n};
    vkCmdCopyBuffer(cmd, src, it.buffer, 1, &c);
    host_read_barrier(cmd);
    vkCmdPipelineBarrier2(cmd, &d);
    buf_items.push_back(std::move(it));
}

bool dump_pending() {
    return !items.empty() || !buf_items.empty();
}

void dump_write() {
    for (Item& it : items) {
        // Before write_item converts depth and changes its dimensions/map.
        invalidate_mapped(it.buffer_alloc, (uint64_t)it.w * it.h * (it.depth_bytes ? it.depth_bytes : 16));
        write_item(it);
        destroy_buffer(it.buffer, it.buffer_alloc);
        if (it.image) destroy_image(it.image, it.image_alloc);
    }
    items.clear();
    for (BufItem& it : buf_items) {
        invalidate_mapped(it.alloc, it.n);
        const uint32_t* w = (const uint32_t*)it.map;
        const size_t nw = it.n / 4;
        size_t nz = 0;
        char line[1200];
        int k = 0;
        for (size_t i = 0; i < nw; i++) nz += w[i] != 0;
        k += snprintf(line + k, sizeof line - k, "vk: dump buffer %s: %llu bytes, %zu of %zu words not zero:", it.what.c_str(),
                      (unsigned long long)it.n, nz, nw);
        int shown = 0;
        for (size_t i = 0; i < nw && shown < 24 && k < 1100; i++)
            if (w[i]) k += snprintf(line + k, sizeof line - k, " [%zu]=%#x", i, w[i]), shown++;
        logf("%s", line);
        destroy_buffer(it.buffer, it.alloc);
    }
    buf_items.clear();
}

} // namespace mkwvk
