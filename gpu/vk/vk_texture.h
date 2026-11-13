// Textures (vk_texture.cpp): one VkImage per texture storage, with every level and layer, the way the GPU sees the
// game's memory. Draws sample views of it, passes render into views of one level and layer, copies and clears
// address it by level and layer. Replaces the first version's image per level and layer (vk_image.cpp), which could
// not sample arrays, cube maps, 3D textures or mip chains.
//
// After totk-pc-recomp's runtime/vk_texture.c (MIT): images keyed by storage address plus descriptor, uploads on first
// use and again when the CPU wrote the storage (page write generations, vk_memory.cpp), no re-upload once the GPU
// wrote the image (it is the content then), every image in GENERAL layout. ASTC, which desktop NVIDIA GPUs do not
// sample, is decoded on the GPU from its block-linear storage by Eden's decoder (host_shaders/astc_decoder.comp,
// with the Switch 2 GOB swizzle); other formats are unswizzled on the CPU straight into the staging memory.
#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "vk_internal.h"

namespace mkwvk {

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;   // the image's own format (views may be others of its class)
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags create_flags = 0;   // (hookshot: as its image was made, for the compaction's copy of it)
    uint32_t width = 1, height = 1, depth = 1, levels = 1, layers = 1;   // the image's (after skip)
    uint32_t skip = 0;        // top levels of the texture left out (huge textures: RX_VK_TEX_MAX_DIM, _MAX_MB)
    // (a huge array kept whole as a sparse resident image instead: each layer gets device memory when it is first
    // written, so only what the game streams in costs VRAM. MKW's terrain arrays, 8192 x 8192 x 68 BC6H: 5.8 GB
    // each, written by the GPU's copies into level 0, which a texture with its top level left out lost)
    bool sparse = false;
    std::vector<std::vector<uint8_t>> tiles;    // (per layer and level: which of its sparse blocks have memory)
    std::vector<uint8_t> tail_bound;            // (per layer, or [0] with SINGLE_MIPTAIL: its mip tail has memory)
    std::vector<VkDeviceMemory> sparse_chunks;  // (the device memory blocks are taken from, 64 MB at a time)
    VkDeviceSize chunk_used = 0;
    struct SparseFree { VkDeviceMemory memory; VkDeviceSize offset, size; };
    std::vector<SparseFree> sparse_free;        // (taken for binds that failed: given out again before the chunks)
    VkSparseImageMemoryRequirements sparse_req{};
    uint32_t sparse_type = 0;
    VkDeviceSize sparse_align = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool cube = false;        // CUBE_COMPATIBLE
    bool astc = false;        // ASTC blocks decoded into RGBA8 (block_w x block_h)
    bool bc = false;          // (and kept as BC3 or BC7: big ones, re-encoded on the GPU after decoding; vk_texture.cpp)
    bool bc_resized = false;  // (hookshot-amd: its BC image is a resized copy, uneven block levels on AMD; vk_texture.cpp)
    uint8_t bc1 = 0;          // (hookshot: of a BC3 image, 0 not decided yet, 1 kept BC3 (alpha seen), 2 BC1: every block opaque)
    int block_w = 1, block_h = 1, bpb = 0;   // the storage's blocks
    // the game's texture
    uint64_t gpu = 0, guest = 0, guest_size = 0;   // storage (GPU address, guest address, bytes)
    nvn_texdesc d{};
    nvn_texlayout L{};
    int nvn_format = 0;       // the format it was made for
    // contents
    bool uploaded = false;    // from guest memory, as of upload_gen
    uint32_t upload_gen = 0;
    uint64_t retry_frame = 0;   // (hookshot) an image whose upload failed: tried again from this frame (vk_texture.cpp)
    uint64_t map_sig = 0;     // (a texture in a virtual pool: its pages' mapping when last uploaded)
    std::vector<uint64_t> page_map;   // (and each 64 KB page's guest address then: what a remap changed)
    struct Run { uint64_t off, guest, n; };   // (and its storage as runs of contiguous guest memory)
    std::vector<Run> runs;
    uint32_t runs_maps_gen = ~0u;             // (the GPU mappings' generation the runs were made at)
    bool gpu_written = false; // the GPU rendered, copied or stored into it: the image is the content
    bool only_cleared = false; // (hookshot) all the GPU did was clear it: what the CPU writes after still counts (a font cache's glyphs)
    uint32_t gpu_gen = 0;     // (the memory generation when the GPU last wrote it: CPU writes after it are taken)
    bool released = false;    // its texture was finalized after the GPU wrote it: CPU writes count again
    uint32_t release_gen = 0;
    uint64_t gpu_wrote_seen = 0;   // (the newest finished GPU write into its storage it was uploaded after)
    uint64_t last_written = 0;     // (the frame the GPU last wrote it: a render target, each frame it is used)
    uint64_t first_written = 0;    // (the frame its GPU contents began: written in that frame alone, a table the game baked)
    uint64_t written_back_for = 0; // (the last_written its texels went into guest memory for: drawn_writeback.h)
    bool writeback_noted = false;  // (in the list of textures waiting for that)
    uint64_t written_back_hash = 0; // (its memory's hash after that write-back: still its own texels there, or the CPU's since)
    uint64_t verify_hash = 0;      // (RX_VK_VERIFY_DIM: its guest memory's hash when last uploaded)
    uint32_t checked_epoch = 0;
    uint64_t upload_value = 0;   // (the submission its last upload was recorded in, and the parallel replay's segment
    uint64_t upload_seg = 0;     // that recorded it: one before it in the stream uploads it again)
    uint64_t last_frame = 0;  // the present it was last used in (the cache's age)
    uint64_t first_render_frame = 0;   // the present it was first rendered into in (0: never)
    uint64_t last_value = 0;  // the timeline value of the last submission that used it
    uint64_t bytes = 0;       // device memory it holds (the texels', or a sparse image's chunks)
    uint64_t pad = 0;         // (what the allocator gave it beyond that: padding and alignment; counted in the budget with bytes)
    uint32_t id = 0;          // (its creation number: diagnostics tell images at one address apart)
    uint64_t first_seen = 0;  // (the present it was made at)
    uint64_t owned_frame = 0; // (the present its storage was last noted as render targets' (gpu_owned))
    uint32_t key_layout = 0, key_cls = 0;   // (the key's storage layout and format class)
    bool vw_listed = false;   // (in the list of GPU-written textures of virtual pools: texture_flush_to_guest)
    uint64_t pool_key = 0;    // (a render target's image: its create info's key in the pool it goes to when the texture does)
    std::unordered_map<uint64_t, VkImageView> views;
    uint64_t view_failed_frame = 0;   // (transient allocation failure: retry uncached views next frame, keep cached ones usable)
    struct BlockAttachment {
        VkImage image;
        VmaAllocation allocation;
        VkFormat format;
        uint32_t level;
    };
    std::vector<BlockAttachment> block_attachments;
};

/// Makes texels [x, x + w) x [y, y + h) of level `level`, layers [first, first + count), of a sparse texture resident
/// (device memory bound to the sparse blocks they fall in) before anything writes them; true when they are (always,
/// for a texture that is not sparse). Without a rectangle: all of those layers, every level.
bool texture_resident(Texture* tex, uint32_t level, uint32_t first, uint32_t count, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h);
bool texture_resident(Texture* tex, uint32_t first, uint32_t count);
void texture_resident_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void texture_resident_flush(Texture* tex);
/// What the replay's recording is: the main command buffer, and the one uploads and new images' layout changes go to
/// (submitted right before it, so nothing has to end a render pass for them)
void texture_begin_recording(VkCommandBuffer upload_cmd, uint64_t value);
/// true when anything was recorded into the upload command buffer since texture_begin_recording
bool texture_uploads_recorded();
/// The recording's upload command buffer, to record into (its first use orders it after the work before)
VkCommandBuffer upload_command_buffer();
/// A copy of an RGBA8 region (decoded ASTC kept as RGBA8) into a texture kept as BC3: through the encoder
bool texture_copy_into_bc(VkCommandBuffer cmd, Texture* src, uint32_t src_level, uint32_t src_layer, int sx, int sy,
                          Texture* dst, uint32_t dst_level, uint32_t dst_layer, int dx, int dy, uint32_t w, uint32_t h,
                          uint32_t layers);
/// A texture already found used again (its age, and its contents from guest memory when written since: texture_get's
/// second half)
Texture* texture_touch(Texture* tex, bool for_sampling);
/// texture_touch without the lock where it has nothing more to do (its contents up to date for this submission):
/// false when the full texture_touch is needed
bool texture_touch_quick(Texture* tex, bool for_sampling);
/// Whether texture_written and texture_rendered would change nothing this frame (a parallel replay: no lock then)
bool texture_marked(const Texture* tex);
/// The parallel replay's segment this thread records (its order in the submission; uploads and mirror syncs a later
/// segment made are made again by an earlier one that needs them, as each segment's uploads precede it)
void texture_set_segment(uint64_t seg);
uint64_t texture_segment();
/// A parallel replay's segments, in queue order: each one's init command buffer (begun; submitted right before its
/// uploads). A new image's first layout change (UNDEFINED -> GENERAL, and its clear) that a segment's worker makes goes
/// to the earliest segment's still being recorded, not to the maker's own uploads: a segment before the maker's that
/// uses it is submitted first, and found it UNDEFINED (VUID-vkCmdDraw-None-09600 with RX_VK_PARALLEL only)
void texture_segments_begin(const VkCommandBuffer* init_cmds, size_t n);
/// This thread records segment `index` of them (-1: none, after it)
void texture_segment_worker(int index);
/// Segment `index` recorded: its init command buffer ended (no new image's change goes to it after). 1: something went
/// to it (submit it before the segment's uploads), 0: nothing, -1: something did and it could not be ended (lost: the
/// replay stops)
int texture_segment_end(int index);
/// Changes with every submission (a texture touched in it is not checked again in it)
uint32_t texture_epoch();
/// Changes whenever a texture is destroyed (pointers to textures kept elsewhere go stale)
uint64_t texture_destroy_generation();
// (hookshot, RX_VK_FRAME_TRACE: time no other column has; totals in ns, the trace takes their differences)
int64_t texture_readback_wait_ns();   // (a read-back's submit and wait for the GPU: the write-backs before an eviction)
int64_t texture_frame_ns();           // (texture_frame as a whole: each present's eviction, compaction and sweeps)
/// The texture an NVN texture's storage is (made and uploaded at first use, uploaded again when the CPU wrote its
/// storage since). null for formats and targets that have no image (buffers, unknown formats).
Texture* texture_get(const nvn_tex* t, bool for_sampling);
/// A view of it: the view's levels and layers (all when v is null), as view_type, in the view's format
/// (attachment: one level, one layer; storage: one level, every layer, in a storage-capable format)
enum class ViewUse { Sample, Attachment, Storage };
struct TextureAttachment {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImage blocks = VK_NULL_HANDLE;   // A separate attachment holding the encoded blocks of a 3D BC mip
};
TextureAttachment texture_attachment(Texture* tex, const nvn_tex* t, const nvn_view* v);
void copy_block_attachment(VkCommandBuffer cmd, const Texture& tex, VkImage blocks, uint32_t level, uint32_t slice,
                           bool to_attachment);
VkImageView texture_view(Texture* tex, const nvn_tex* t, const nvn_view* v, VkImageViewType view_type, ViewUse use,
                         VkFormat* format_out = nullptr, VkFormat force_format = VK_FORMAT_UNDEFINED);
/// (hookshot) The format rules texture_get and texture_view follow (vk_texture.cpp), shared with the pipeline state list
VkFormat image_format_of(VkFormat vf, bool astc_decoded);
VkFormat view_format_of(VkFormat image, bool astc_decoded, bool bc, int view_nvn, ViewUse use, VkFormat force_format);
/// The format a draw renders in on this device, into a texture of NVN format tex_nvn through a view of NVN format view_nvn
VkFormat attachment_format(int tex_nvn, int view_nvn);
/// The view format an NVN texture and view sample / render in
int view_nvn_format(const nvn_tex* t, const nvn_view* v);
/// The level and layer a view starts at (the texture's levels)
unsigned view_level(const nvn_view* v);
/// The image level of a texture level (the levels a texture left out read as its first)
unsigned image_level(const Texture* tex, unsigned level);
unsigned view_layer(const nvn_view* v);
/// The GPU wrote it (in the submission being recorded)
void texture_written(Texture* tex, const char* why = "?");   // (why: who wrote it, for the parallel replay's order warning)
/// The GPU writes [gpu, gpu + size) in the submission being recorded (a storage buffer, a buffer copy): textures over
/// it are uploaded again from guest memory once that submission is done
void texture_gpu_wrote(uint64_t gpu, uint64_t size);
/// The program the replay draws or dispatches with next (RX_VK_WATCH_TEX / RX_VK_WATCH_DIM logs name it)
void texture_watch_prog(uint64_t prog);
/// A pass renders into it (first_render_frame)
void texture_rendered(Texture* tex);
/// The presents so far (the frame number textures are aged by)
uint64_t texture_frame_no();
/// A texel buffer view over guest memory (texture targets of NVN_TEXTURE_TARGET_BUFFER, and image buffers)
VkBufferView texel_buffer_view(const nvn_tex* t, const nvn_view* v, bool storage);
/// Release an imported buffer's views immediately before its completed destruction (memory.cpp).
void release_texel_buffer_views(VkBuffer buffer);
/// The texture whose image was rendered for the window (present): t's own, or one rendered at its storage
Texture* texture_for_present(const nvn_tex* t);
/// Ages the cache by one present; images nothing used for a while go, after the work that last read them
void texture_frame(uint64_t completed_value);
void texture_released(const nvn_tex* t);
/// (diagnostics: what guest memory holds of a texture's storage now, level by level, logged)
void texture_log_guest(const Texture* t);
/// (diagnostics, the replay's lock held) the textures the GPU wrote in [gpu, gpu + size), appended to out
void texture_describe_written(uint64_t gpu, uint64_t size, std::string& out);
/// (a host hook's fast check, any thread) whether a texture the GPU wrote may lie in virtual pages [gpu, gpu + size)
bool texture_gpu_written_in(uint64_t gpu, uint64_t size);
/// Textures the GPU wrote in [gpu, gpu + size) read back and written into their memory, block linear, as the GPU
/// would have left them there: before those virtual pages are unmapped or remapped (the work recorded before is
/// submitted; the replay's lock held). How many were.
int texture_flush_to_guest(uint64_t gpu, uint64_t size);
size_t texture_count();
uint64_t texture_made();
void texture_unswizzle_stats(int64_t* bytes, int64_t* ns);
uint64_t texture_bytes();
/// A full memory barrier (all commands, all accesses): what separates dependent work for now
/// (why: its label in RX_VK_PASS_TIMING's barrier lines)
void full_barrier(VkCommandBuffer cmd, const char* why = "barrier (other)");
/// (hookshot) RX_VK_GAME_ORDER: the game's own barriers order back-to-back passes and dispatches (on NVIDIA by default; vk_texture.cpp)
bool game_order();
/// Full barriers recorded and left out (RX_VK_BARRIER_DEDUPE_OFF=1: none), since the start
void barrier_stats(int64_t* made, int64_t* left_out);
/// (hookshot) Set around a draw's sampled-texture lookups (vk_draw.cpp): a new ASTC texture there may wait for a later frame
/// once this frame's uploads passed their budget (texture_touch). Never for compute: a dispatch may bake what it reads once
extern thread_local bool upload_defer_ok;
/// A draw pass just ended on cmd (end_pass): pass_start_barrier knows nothing else ran after it
void note_pass_end(VkCommandBuffer cmd);
/// The barrier a draw pass starts with. Full, unless RX_VK_PASS_BARRIER=game and the last thing on cmd was a draw pass with no
/// barrier of the game's after it: then the new pass waits for the earlier work in its fragment stages only, so its vertex work
/// may overlap the previous pass's fragments (NVN asks for a barrier before rendering results are read; only vertex-stage
/// reads of the previous pass's output with no barrier from the game can change order)
void pass_start_barrier(VkCommandBuffer cmd);
/// Pass starts with the fragment-stage barrier, since the start
int64_t pass_barriers_relaxed();
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded);
/// (hookshot) whether texture_read_texels' walk of r at that row stride (0: the region's own) and its own image stride
/// stays within avail bytes of the data it is given (overflow-checked: the stride is the game's)
bool texels_fit(const nvn_tex* t, const NVNcopyRegion* r, uint64_t row, uint64_t avail);
/// ReadTexels / WriteTexels: the CPU's copy of a region of the texture in guest memory (vk_texture.cpp)
void texture_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst,
                         uint64_t row, uint64_t img, bool write);

} // namespace mkwvk
