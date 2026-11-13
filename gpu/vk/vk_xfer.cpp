// Texture <-> buffer and texture <-> texture copies, and texture clears: totk-pc-recomp's runtime/vk_exec_xfer.c
// (MIT; sub_resolve, buffer_side, copy_buffer_image, copy_t2t, clear_tex), on the textures of vk_texture.cpp (an
// image per texture: a copy addresses a level, its layers or slices, and a region).
#include <atomic>
#include <algorithm>
#include <cstring>
#include <vector>

#include "vk_xfer.h"
#include "vk_draw.h"   // (end_pass: a pass cannot stay open across a transfer)
#include "vk_texture.h"
#include "vk_gprof.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

/// One side of a copy: the texture's level, layers (or 3D slices), and the region of it in texels
struct Sub {
    Texture* tex = nullptr;
    VkImageAspectFlags aspect = 0;
    uint32_t level = 0, layer = 0, layers = 1;
    int32_t x = 0, y = 0, z = 0;   // z: the first 3D slice (layers carry array layers)
    uint32_t w = 0, h = 0, d = 1;
    uint32_t bpb = 0;
    int bw = 1, bh = 1;
    int nvn_format = 0;
    uint32_t down = 0;   // (a level the image left out, copied into its first at 1 / 2^down the size)
};

int warn_budget[16];
void warn(int key, const char* what) {
    if (warn_budget[key]++ < 3) logf("vk: %s", what);
}

/// (RX_VK_LOG_BIG_COPY=1: copies into textures of 32 MB or more, the course maps' kind, 300 lines)
void log_big_copy(const char* kind, const Texture* t, uint32_t level, uint32_t layer, int x, int y, uint32_t w, uint32_t h,
                  uint64_t src) {
    static const bool on = env_on("RX_VK_LOG_BIG_COPY");
    static const std::pair<uint64_t, uint64_t> range = [] {   // (RX_VK_WATCH_RANGE=<lo>:<hi>: copies into textures there)
        unsigned long long a = 0, b = 0;
        if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
        return std::pair<uint64_t, uint64_t>{a, b};
    }();
    static int logged, rlogged;
    const bool in_range = t && range.second && t->gpu < range.second &&
                          t->gpu + std::max<uint64_t>(t->guest_size, 1) > range.first && rlogged < 400;
    if (in_range) rlogged++;
    else if (!on || !t || t->bytes < (32ull << 20) || logged >= 300) return;
    else logged++;
    logf("vk: big copy %s -> #%u %#llx %ux%u x%u NVN %#x level %u layer %u (%d,%d %ux%u) from %#llx%s%s", kind, t->id,
         (unsigned long long)t->gpu, t->width, t->height, t->layers, t->nvn_format, level, layer, x, y, w, h,
         (unsigned long long)src, t->gpu_written ? " (rendered)" : "", t->released ? " (finalized)" : "");
}

bool resolve(const nvn_tex* t, const nvn_view* v, bool has_v, const NVNcopyRegion& r_in, Sub& s, bool scaled_ok = false) {
    NVNcopyRegion r = r_in;
    if (!t || !t->gpu) return false;
    const nvn_view* view = has_v ? v : nullptr;
    s.tex = texture_get(t, true);
    if (!s.tex) {
        warn(0, "copy of a texture with no image: skipped");
        return false;
    }
    s.nvn_format = view_nvn_format(t, view);
    s.aspect = s.tex->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                                          : s.tex->aspect;
    if (view_level(view) < s.tex->skip && scaled_ok && !s.tex->bc && s.tex->type == VK_IMAGE_TYPE_2D) {
        // (texture copies into a level the image left out, MKW's terrain textures streamed into level 0: into its
        // first level, scaled down)
        s.down = s.tex->skip - view_level(view);
        r.xoffset >>= s.down, r.yoffset >>= s.down;
        if (r.width > 0) r.width = std::max(1, r.width >> s.down);
        if (r.height > 0) r.height = std::max(1, r.height >> s.down);
    } else if (view_level(view) < s.tex->skip) {   // (a level the image left out)
        static int logged;
        if (logged++ < 40)
            logf("vk: copy into level %u layer %u of texture %#llx %ux%u x%u NVN %#x%s (its top %u level(s) left out): "
                 "skipped", view_level(view), view_layer(view) + (uint32_t)std::max(0, r.zoffset),
                 (unsigned long long)s.tex->gpu, s.tex->width, s.tex->height, s.tex->layers, s.tex->nvn_format,
                 s.tex->astc ? " ASTC" : "", s.tex->skip);
        return false;
    }
    if (!s.down && view_level(view) >= s.tex->skip + s.tex->levels) {   // (hookshot-amd: a level the image left out at its end)
        static int logged;
        if (logged++ < 20)
            logf("vk: copy with level %u of texture %#llx %ux%u, %u levels (its last levels left out): skipped",
                 view_level(view), (unsigned long long)s.tex->gpu, s.tex->width, s.tex->height, s.tex->levels);
        return false;
    }
    s.level = s.down ? 0 : image_level(s.tex, view_level(view));
    const uint32_t lw = std::max(1u, s.tex->width >> s.level), lh = std::max(1u, s.tex->height >> s.level);
    const uint32_t ld = s.tex->type == VK_IMAGE_TYPE_3D ? std::max(1u, s.tex->depth >> s.level) : 1;
    s.bpb = (uint32_t)nvn_block_size(s.tex->nvn_format, &s.bw, &s.bh);
    if (s.tex->astc) s.bpb = 4, s.bw = s.bh = 1;   // (the decoded texels)
    s.x = r.xoffset, s.y = r.yoffset;
    s.w = r.width > 0 ? (uint32_t)r.width : (s.x >= 0 && (uint32_t)s.x < lw ? lw - (uint32_t)s.x : 0);
    s.h = r.height > 0 ? (uint32_t)r.height : (s.y >= 0 && (uint32_t)s.y < lh ? lh - (uint32_t)s.y : 0);
    // (hookshot: 64-bit, so a layer range near 2^32 does not wrap around past the clamp below)
    const uint64_t first = (uint64_t)view_layer(view) + (uint64_t)std::max(0, r.zoffset);
    const uint32_t count = (uint32_t)std::max(1, r.depth);
    if (s.tex->type == VK_IMAGE_TYPE_3D) {
        s.z = (int32_t)std::max(0, r.zoffset), s.d = std::min(count, ld - std::min<uint32_t>(ld, (uint32_t)s.z));
        s.layer = 0, s.layers = 1;
    } else {
        s.layer = first < s.tex->layers ? (uint32_t)first : 0;
        s.layers = first < s.tex->layers ? (uint32_t)std::min<uint64_t>(count, s.tex->layers - first) : 0;
    }
    s.w = std::min<uint32_t>(s.w, lw > (uint32_t)std::max(0, s.x) ? lw - (uint32_t)std::max(0, s.x) : 0);
    s.h = std::min<uint32_t>(s.h, lh > (uint32_t)std::max(0, s.y) ? lh - (uint32_t)std::max(0, s.y) : 0);
    if (!s.bpb || s.x < 0 || s.y < 0 || !s.w || !s.h || !s.layers || !s.d) {
        warn(2, "copy region outside its texture: skipped");
        return false;
    }
    // (the sparse blocks of a sparse image it reaches; a source's too: reading them unbound would read zero)
    if (s.tex->sparse) texture_resident(s.tex, s.level, s.layer, s.layers, (uint32_t)s.x, (uint32_t)s.y, s.w, s.h);
    return true;
}

VkImageSubresourceLayers layers(const Sub& s) {
    return {s.aspect, s.level, s.layer, s.layers};
}
VkOffset3D offset(const Sub& s) {
    return {s.x, s.y, s.z};
}
VkExtent3D extent(const Sub& s) {
    return {s.w, s.h, s.d};
}

/// The buffer side: rows of the copy stride when the game set one, else of the region's blocks
bool buffer_side(const NvnState& st, const Sub& s, uint64_t addr, uint64_t& size, VkBufferImageCopy& c,
                 BufferRef& span, bool written) {
    const uint32_t wb = (s.w + (uint32_t)s.bw - 1) / (uint32_t)s.bw;
    const uint32_t hb = (s.h + (uint32_t)s.bh - 1) / (uint32_t)s.bh;
    const uint64_t row = st.copy_stride > 0 ? (uint64_t)st.copy_stride : (uint64_t)wb * s.bpb;
    if (row % s.bpb || row < (uint64_t)wb * s.bpb || row / s.bpb * (uint64_t)s.bw > UINT32_MAX) {
        warn(3, "copy row stride does not fit the level's blocks: skipped");
        return false;
    }
    // The last row ends at its last block: its trailing stride padding is not accessed by Vulkan.
    // (hookshot: overflow-checked; the stride is the game's 64-bit value, and a wrapped size checked one row of many)
    uint64_t rows = 0, body = 0;
    if (__builtin_mul_overflow((uint64_t)hb * s.layers, (uint64_t)s.d, &rows) || __builtin_mul_overflow(row, rows - 1, &body) ||
        __builtin_add_overflow(body, (uint64_t)wb * s.bpb, &size)) {
        warn(3, "copy buffer size overflows: skipped");
        return false;
    }
    span = gpu_buffer_bind(addr, size, written);
    if (!addr || !span) {
        warn(3, "copy buffer is not one mapped pool span: skipped");
        return false;
    }
    c = VkBufferImageCopy{};
    c.bufferOffset = span.offset;
    c.bufferRowLength = (uint32_t)(row / s.bpb) * (uint32_t)s.bw;
    c.bufferImageHeight = hb * (uint32_t)s.bh;
    c.imageSubresource = layers(s);
    c.imageOffset = offset(s);
    c.imageExtent = extent(s);
    return true;
}

bool size_compatible(const Sub& a, const Sub& b) {
    return a.bpb == b.bpb && a.bw == b.bw && a.bh == b.bh;
}

} // namespace

void copy_b2t(VkCommandBuffer cmd, NvnState& st, const nvn_p_copy_bt& p, bool to_image) {
    // (ASTC blocks into a decoded image: into the texture's storage on the CPU, which uploads it again. Other formats are
    // copied on the GPU: the buffer may hold what the GPU wrote this submission (not yet in guest memory), and the image
    // it fills is kept (texture_frame: a texture the GPU filled once is not evicted). RX_VK_B2T_CPU=1: on the CPU too)
    static const bool b2t_gpu = !env_on("RX_VK_B2T_CPU");
    const bool astc = to_image && p.tex && vk_format(view_nvn_format(p.tex, p.has_view ? &p.view : nullptr)).astc;
    if (to_image && p.tex) {   // (RX_VK_LOG_BIG_COPY)
        if (Texture* t = texture_get(p.tex, false))
            log_big_copy(b2t_gpu && !astc ? "buffer (GPU)" : "buffer (CPU)", t, view_level(p.has_view ? &p.view : nullptr),
                         view_layer(p.has_view ? &p.view : nullptr) + (uint32_t)std::max(0, p.r.zoffset), p.r.xoffset,
                         p.r.yoffset, (uint32_t)std::max(0, p.r.width), (uint32_t)std::max(0, p.r.height), p.buf);
    }
    {   // (hookshot, RX_VK_LOG_B2T=1) the first 60 copies into textures 960 wide or more: where they read, and what
        static const bool on = env_on("RX_VK_LOG_B2T");
        static std::atomic<int> n{0};
        if (on && to_image && p.tex && p.r.width >= 960 && n.fetch_add(1) < 60) {
            uint64_t avail = 0, sum = 0, cnt = 0;
            const uint8_t* src = host->gpu_ptr(p.buf, &avail);
            const uint64_t span = (uint64_t)p.r.width * p.r.height * 2;   // (the region copied, at most 2 bytes a texel)
            for (uint64_t k = 0; src && k < avail && k < span; k += 997, cnt++) sum += src[k];
            logf("vk: b2t: buffer %#llx (guest %#llx, %s, mean %.1f) -> tex %#llx %dx%d at %d,%d stride %d",
                 (unsigned long long)p.buf, (unsigned long long)host->gpu_guest(p.buf), src ? "mapped" : "unmapped",
                 cnt ? (double)sum / cnt : -1.0, (unsigned long long)p.tex->gpu, p.r.width, p.r.height, p.r.xoffset,
                 p.r.yoffset, st.copy_stride);
        }
    }
    if (to_image && p.tex && (!b2t_gpu || astc)) {
        uint64_t avail = 0;
        const uint8_t* src = host->gpu_ptr(p.buf, &avail);
        if (src && !texels_fit(p.tex, &p.r, st.copy_stride > 0 ? (uint64_t)st.copy_stride : 0, avail)) {
            warn(3, "copy buffer runs past its mapping: skipped");
            return;
        }
        if (src) {
            const nvn_view* v = p.has_view ? &p.view : nullptr;
            texture_read_texels(p.tex, view_level(v), view_layer(v), &p.r, const_cast<uint8_t*>(src),
                                st.copy_stride > 0 ? (uint64_t)st.copy_stride : 0, 0, true);
            return;
        }
        if (astc) return;   // (others without CPU access to the buffer: on the GPU, below)
    }
    Sub s;
    if (!resolve(p.tex, &p.view, p.has_view != 0, p.r, s)) return;
    uint64_t size = 0;
    VkBufferImageCopy c{};
    BufferRef span;
    if (!buffer_side(st, s, p.buf, size, c, span, !to_image)) return;
    if (s.tex->astc && !to_image) {
        warn(4, "copy of a decoded ASTC texture to a buffer: skipped");
        return;
    }
    if (s.tex->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) && to_image) {
        warn(5, "copy of a buffer into a depth-stencil texture: skipped");
        return;
    }
    end_pass(cmd);
    GprofSpan gp(cmd, to_image ? "xfer copy buffer->texture (with its barriers)" : "xfer copy texture->buffer (with its barriers)");
    gp.bytes = size;
    full_barrier(cmd);
    if (to_image) {
        vkCmdCopyBufferToImage(cmd, span.buffer, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        texture_written(s.tex, "a buffer-to-texture copy");
    } else {
        vkCmdCopyImageToBuffer(cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, span.buffer, 1, &c);
        const uint64_t guest = host->gpu_guest(p.buf);
        if (guest) memory_stamp_gpu(guest, size);   // (textures over that memory are uploaded again)
        texture_gpu_wrote(p.buf, size);           // (and once the copy is done: the stamp came before it ran)
    }
    full_barrier(cmd);
}

void copy_t2t(VkCommandBuffer cmd, NvnState& st, const nvn_p_copy_tt& p) {
    Sub s, d;
    if (!resolve(p.src, &p.sv, p.has_sv != 0, p.sr, s, true)) return;
    if (!resolve(p.dst, &p.dv, p.has_dv != 0, p.dr, d, true)) return;
    {   // (RX_VK_LOG_COPY: the texture copies, 400 of them)
        static const bool log_copy = env_on("RX_VK_LOG_COPY");
        static int logged;
        if (log_copy && logged++ < 400)
            logf("vk: copy #%u %#llx %ux%u NVN %#x%s level %u (%d,%d %ux%u) -> #%u %#llx %ux%u NVN %#x level %u (%d,%d "
                 "%ux%u) flags %#x", s.tex->id, (unsigned long long)s.tex->gpu, s.tex->width, s.tex->height, s.nvn_format,
                 s.tex->gpu_written ? " rendered" : s.tex->uploaded ? " uploaded" : " empty", s.level, s.x, s.y, s.w, s.h,
                 d.tex->id, (unsigned long long)d.tex->gpu, d.tex->width, d.tex->height, d.nvn_format, d.level, d.x, d.y,
                 d.w, d.h, p.flags);
    }
    log_big_copy("texture", d.tex, d.level, d.layer, d.x, d.y, d.w, d.h, s.tex->gpu);
    if (s.tex == d.tex && s.level == d.level && s.layer == d.layer) {
        warn(6, "texture copy within one subresource: skipped");
        return;
    }
    const bool color = s.aspect == VK_IMAGE_ASPECT_COLOR_BIT && d.aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    if (!color && s.tex->format != d.tex->format) {
        warn(7, "texture copy between unlike depth formats: skipped");
        return;
    }
    s.layers = d.layers = std::min(s.layers, d.layers);
    end_pass(cmd);
    GprofSpan gp(cmd, "xfer copy texture->texture (with its barriers)");
    full_barrier(cmd);
    const bool same_size = s.w == d.w && s.h == d.h && s.d == d.d;
    if (s.tex->bc != d.tex->bc || (s.tex->bc && !same_size)) {   // (big ASTC kept as BC3 on one side: vk_texture.cpp)
        bool done = false;
        if (d.tex->bc && same_size) {
            done = texture_copy_into_bc(cmd, s.tex, s.level, s.layer, s.x, s.y, d.tex, d.level, d.layer, d.x, d.y, s.w, s.h,
                                        s.layers);
        } else if (s.tex->bc && !d.tex->bc && s.tex->bc_resized) {   // (hookshot-amd: its texels are not the game's)
            static int logged;
            if (logged++ < 8)
                logf("vk: a copy out of resized BC3 texture %#llx left out", (unsigned long long)s.tex->gpu);
        } else if (s.tex->bc && !d.tex->bc) {   // (decompressed by a blit)
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(dev.physical, s.tex->format, &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) {
                VkImageBlit bl{};
                bl.srcSubresource = layers(s);
                bl.dstSubresource = layers(d);
                bl.srcOffsets[0] = offset(s);
                bl.srcOffsets[1] = {s.x + (int32_t)s.w, s.y + (int32_t)s.h, s.z + (int32_t)s.d};
                bl.dstOffsets[0] = offset(d);
                bl.dstOffsets[1] = {d.x + (int32_t)d.w, d.y + (int32_t)d.h, d.z + (int32_t)d.d};
                vkCmdBlitImage(cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, d.tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                               VK_FILTER_NEAREST);
                done = true;
            }
        }
        if (!done) warn(10, "texture copy into or out of a BC3-kept ASTC texture it cannot make: skipped");
        full_barrier(cmd);
        if (done) texture_written(d.tex, "a copy into a BC texture");
        return;
    }
    if (s.tex->samples != VK_SAMPLE_COUNT_1_BIT && d.tex->samples == VK_SAMPLE_COUNT_1_BIT && same_size) {
        VkImageResolve rs{layers(s), offset(s), layers(d), offset(d), extent(s)};
        vkCmdResolveImage(cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, d.tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
    } else if (same_size && size_compatible(s, d) && s.tex->samples == d.tex->samples) {
        VkImageCopy cp{layers(s), offset(s), layers(d), offset(d), extent(s)};
        vkCmdCopyImage(cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, d.tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    } else {   // scaled or converting: nearest, or linear when the game asked for it (colour only)
        VkImageBlit bl{};
        bl.srcSubresource = layers(s);
        bl.dstSubresource = layers(d);
        bl.srcOffsets[0] = offset(s);
        bl.srcOffsets[1] = {s.x + (int32_t)s.w, s.y + (int32_t)s.h, s.z + (int32_t)s.d};
        bl.dstOffsets[0] = offset(d);
        bl.dstOffsets[1] = {d.x + (int32_t)d.w, d.y + (int32_t)d.h, d.z + (int32_t)d.d};
        const bool linear = ((p.flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) || d.down > s.down) && color;
        vkCmdBlitImage(cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, d.tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    full_barrier(cmd);
    texture_written(d.tex, "a texture-to-texture copy");
}

void clear_tex(VkCommandBuffer cmd, NvnState& st, const nvn_p_clear_tex& p) {
    Sub s;
    if (!resolve(p.tex, &p.view, p.has_view != 0, p.r, s)) return;
    const uint32_t lw = std::max(1u, s.tex->width >> s.level), lh = std::max(1u, s.tex->height >> s.level);
    const bool whole = !s.x && !s.y && s.w == lw && s.h == lh;
    if ((p.mask & 0xf) != 0xf && (s.tex->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) warn(8, "texture clear with a channel mask: cleared whole");
    end_pass(cmd);
    GprofSpan gp(cmd, "xfer clear texture (with its barriers)");
    full_barrier(cmd);
    const bool volume = s.tex->type == VK_IMAGE_TYPE_3D;
    const bool raw = s.tex->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && s.tex->format <= VK_FORMAT_BC7_SRGB_BLOCK;
    if (whole && !volume && !raw) {
        VkImageSubresourceRange rng{s.tex->aspect, s.level, 1, s.layer, s.layers};
        if (s.tex->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue cv{};
            std::memcpy(&cv, &p.c, sizeof cv);
            vkCmdClearColorImage(cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rng);
        } else {
            VkClearDepthStencilValue dv{};
            if (s.tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) dv.depth = p.c.f[0];
            if (s.tex->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) dv.stencil = (uint32_t)p.c.u[0];
            vkCmdClearDepthStencilImage(cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rng);
        }
    } else {   // (a region: a pass over that level and layer, and vkCmdClearAttachments)
        const nvn_view* v = p.has_view ? &p.view : nullptr;
        const uint32_t slices = volume ? s.d : s.layers;
        for (uint32_t k = 0; k < slices; k++) {
            const uint32_t slice = (volume ? (uint32_t)s.z : s.layer) + k;
            nvn_view lv = v ? *v : nvn_view{};
            // texture_view takes a guest mip level and subtracts skip itself.
            lv.set_levels = 1, lv.base_level = (uint8_t)(s.level + s.tex->skip), lv.nlevels = 1;
            lv.set_layers = 1, lv.min_layer = (uint16_t)slice, lv.nlayers = 1;
            const TextureAttachment attachment = texture_attachment(s.tex, p.tex, &lv);
            if (!attachment.view) continue;
            // (an attachment view of a compressed image is a raw one, one texel a block: its area and the rect in blocks, as
            // targets() sizes a pass over it)
            const uint32_t aw = raw ? (lw + 3) / 4 : lw, ah = raw ? (lh + 3) / 4 : lh;
            const int32_t rx = raw ? s.x / 4 : s.x, ry = raw ? s.y / 4 : s.y;
            const uint32_t rw = std::min<uint32_t>(raw ? (s.x + s.w + 3) / 4 - rx : s.w, aw - std::min<uint32_t>(aw, (uint32_t)rx));
            const uint32_t rh = std::min<uint32_t>(raw ? (s.y + s.h + 3) / 4 - ry : s.h, ah - std::min<uint32_t>(ah, (uint32_t)ry));
            if (!rw || !rh) continue;
            if (attachment.blocks) {
                full_barrier(cmd);
                copy_block_attachment(cmd, *s.tex, attachment.blocks, s.level, slice, true);
                full_barrier(cmd);
            }
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            a.imageView = attachment.view;
            a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {aw, ah}};
            ri.layerCount = 1;
            const bool color = s.tex->aspect & VK_IMAGE_ASPECT_COLOR_BIT;
            if (color) ri.colorAttachmentCount = 1, ri.pColorAttachments = &a;
            if (s.tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &a;
            if (s.tex->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &a;
            vkCmdBeginRendering(cmd, &ri);
            VkClearAttachment ca{};
            ca.aspectMask = s.tex->aspect;
            if (color) std::memcpy(&ca.clearValue.color, &p.c, sizeof ca.clearValue.color);
            else ca.clearValue.depthStencil = {p.c.f[0], p.c.u[0]};
            const VkClearRect rect{{{rx, ry}, {rw, rh}}, 0, 1};
            vkCmdClearAttachments(cmd, 1, &ca, 1, &rect);
            vkCmdEndRendering(cmd);
            if (attachment.blocks) {
                full_barrier(cmd);
                copy_block_attachment(cmd, *s.tex, attachment.blocks, s.level, slice, false);
            }
        }
    }
    full_barrier(cmd);
    const bool fresh = !s.tex->gpu_written || s.tex->released || s.tex->only_cleared;
    texture_written(s.tex);
    s.tex->only_cleared = fresh && s.tex->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
}

} // namespace mkwvk
