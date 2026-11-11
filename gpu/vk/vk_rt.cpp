// (miojo) The scene as ray-tracing acceleration structures: the first step of path tracing (vk_rt.h).
//
// The game's opaque scene draws are indexed triangle lists with the position at vertex attribute 0 and a 3 x 4 world matrix
// at the start of the object's buffer (the census: 3100 of a frame's draws, 1.1 million triangles, in Kakariko). Each mesh
// (a vertex buffer, an index range) is read from guest memory once, its positions made plain floats, and built into a
// bottom-level structure kept by its key; each frame's draws are the instances of a top-level structure built anew.
// The skinned draws (Link, the characters) are skinned on the CPU each frame with the bones the game drew them with, all
// of them into one more bottom-level structure in world space. Not in it yet: mesh-shader draws (grass) and draws whose
// position is not three floats or halves (the two-float one).
//
//   The menu's Ray tracing row (ray_tracing= in hookshot_options.ini, read as the device is made: a restart), RX_VK_RT=0 / 1
//   over it. With it on the Ambient occlusion row is traced (rt_debug.comp mode 3) in place of the screen-space one.
//   RX_VK_RT_RADIUS   how far the occlusion's rays look, in the world's units (1.5); RX_VK_RT_RAYS a pixel (4);
//   RX_VK_RT_REACH    how far they look for the Bounce light row's light (12)
//   RX_VK_RT_DEBUG=1  the rays' picture in place of the scene, mismatches with the game's depth coloured (rt_debug.comp);
//                     2: the game's picture, only the mismatches marked
//
// ponytail: no compaction of the bottom-level structures and a full rebuild of the top one a frame; compact and refit when
// the memory or the build time shows. A mesh whose buffer the game writes again under the same key stays as first read
// until it goes 18000 frames unused.
#include "vk_rt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dlss_actor.h"
#include "dlss_math.h"
#include "shaders/rt_debug_comp_spv.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_state.h"
#include "vk_texture.h"
#include "vk_work.h"   // (last: its macros count the commands for full_barrier)

namespace mkwvk {

namespace {

struct Timed {   // (a scope's time added to a counter)
    std::atomic<int64_t>& to;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~Timed() { to.fetch_add((std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed); }
};

struct Buf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    void* map = nullptr;
    uint64_t address = 0, size = 0;
};
struct Mesh {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    Buf store;        // (a buffer of its own: the top level's, the characters')
    int block = -1;   // (or its block: a mesh's)
    uint64_t size = 0;
    uint32_t material = 0;   // (its slot in the materials' image; 0 not looked for yet, 1 none)
    uint64_t address = 0, used = 0;
    uint32_t triangles = 0;
};
struct Instance {
    uint64_t key;
    float m[12];
    uint64_t same;   // (the mesh and the matrix as one number: the same twice is a second pass over the scene)
    uint64_t place;  // (the matrix alone)
    int kind;        // (0 the scene's; 1 from a shadow map's pass: left out when the scene's own pass drew an object
                     // there, whose mesh may be another level of detail of the same thing; 2 no instance but a mark:
                     // the object at this place is a cutout, kept out of the rays)
};
struct Dead {
    uint64_t frame;
    Buf buf;
    VkAccelerationStructureKHR as;
};

/// The meshes' structures share large buffers: a buffer and a structure made for each mesh were most of a millisecond a
/// mesh, the stutter of walking into an area
struct Block {
    Buf buf;
    uint64_t at = 0;
    int live = 0;
};
constexpr uint64_t kBlockBytes = 32 << 20, kStageBytes = 8 << 20;

constexpr uint32_t kMaxInstances = 16384;
constexpr int kRing = 6;

struct Rt {
    bool asked = false, have_ext = false, ok = false;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    // (a hit's triangle, for its normal: which side's light the cache keeps)
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR pff{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    VkImage cache = VK_NULL_HANDLE;
    VmaAllocation cache_memory = nullptr;
    VkImageView cache_view = VK_NULL_HANDLE;
    PFN_vkCreateAccelerationStructureKHR create = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR address = nullptr;
    uint32_t scratch_align = 256;

    std::mutex mu;
    std::unordered_map<uint64_t, Mesh> meshes;
    // (meshes seen and not built yet: built once the same one was drawn in 4 frames, so what the game makes anew each
    // frame at another address (particles, cloth, batches) is never read and built: thousands a minute, the lag of the
    // first version)
    struct Pending { RtDraw d; uint64_t last = 0; int frames = 0; };
    std::unordered_map<uint64_t, Pending> pending;
    std::unordered_set<uint64_t> bad, seen;   // (meshes that could not be read; this frame's instances, by mesh and matrix)
    std::vector<Instance> instances;
    std::vector<Dead> dead;
    Buf scratch, ring[kRing];
    Buf stage[kRing];   // (a frame's new meshes' vertices and indices, for their builds)
    uint64_t stage_at = 0;
    std::vector<Block> blocks;
    int block_now = -1;
    // (the materials: a colour texture's slot in the image of mean albedos, and the ones still to be copied there)
    struct Blit { uint32_t slot; Texture* tex; uint64_t gen; };
    std::unordered_map<uint64_t, uint32_t> materials;
    std::vector<Blit> blits;
    uint32_t material_next = 2;
    VkImage atlas = VK_NULL_HANDLE;
    VmaAllocation atlas_memory = nullptr;
    VkImageView atlas_view = VK_NULL_HANDLE;
    uint64_t skin_indices = 0, skin_normals = 0;   // (the characters' structure's, this frame: device addresses)
    int ring_at = 0;
    Mesh top;
    // (the skinned draws: each model's vertices kept on the CPU, this frame's poses, and one structure of them all in world space)
    struct Model {
        std::vector<float> pos;        // (3 a vertex, model space)
        std::vector<float> normal;     // (3 a vertex: smooth, its triangles' summed, the vertices at one place as one)
        std::vector<uint16_t> joints;  // (4 a set a vertex)
        std::vector<uint8_t> weights;
        std::vector<uint32_t> idx;
        int sets = 0;
        uint64_t used = 0;
    };
    struct Pose {
        uint64_t key;
        SkinnedDraw k;
        std::vector<float> bones;   // (12 a bone)
        float origin[3];
    };
    std::unordered_map<uint64_t, Model> models;
    std::vector<Pose> poses;
    Buf skin_ring[kRing];
    Mesh skin;
    uint32_t skin_cap = 0;   // (triangles the structure was made for)
    uint64_t skin_triangles = 0, skin_draws = 0, skin_dropped = 0;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool pipe_tried = false;
    uint64_t built = 0, built_triangles = 0, skipped = 0, frames = 0;
    std::atomic<int64_t> phase_ns[4]{};   // (build_mesh's: the reads, the conversion, the structure made, the build recorded)
    std::atomic<int64_t> note_ns{0};   // (rt_note_draw's and rt_note_skinned's time, on the game's draws)
    // (the sun, from the game's shadow maps: rt_note_shadow_draw)
    uint64_t light_camera = 0, light_frame = 0, sun_frame = 0;
    bool light_is_sun = false;
    float to_sun[3]{};
    std::atomic<uint64_t> lights_frame{0};   // (the frame the lights' buffer was last read)
    float sun_colour[3]{};   // (the game's, before its exposure; 0: not known)
    std::atomic<uint64_t> traced_frame{0};   // (the frame rt_frame last drew its effects)
    double cpu_ms = 0;   // (rt_frame's time on the CPU since the last line: the builds, the skinning)
};
Rt& rt() {
    static Rt& r = *new Rt;   // (never destroyed: the process leaves with the device alive, as vk_temporal.cpp's state)
    return r;
}

bool make_buf(Buf& o, uint64_t size, VkBufferUsageFlags usage, bool host) {
    o = {};
    usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (!(host ? create_buffer(size, usage, true, o.buffer, o.memory, &o.map) : create_device_buffer(size, usage, o.buffer, o.memory)))
        return o = {}, false;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = o.buffer;
    o.address = vkGetBufferDeviceAddress(dev.device, &da);
    o.size = size;
    return true;
}
/// Freed a few frames on: work in flight may still use it
void later(Rt& r, Buf b, VkAccelerationStructureKHR as = VK_NULL_HANDLE) {
    if (b.buffer || as) r.dead.push_back({texture_frame_no(), b, as});
}
void free_dead(Rt& r) {
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < r.dead.size();) {
        if (now - r.dead[i].frame <= 8) { i++; continue; }
        if (r.dead[i].as) r.destroy(dev.device, r.dead[i].as, nullptr);
        if (r.dead[i].buf.buffer) destroy_buffer(r.dead[i].buf.buffer, r.dead[i].buf.memory);
        r.dead[i] = r.dead.back();
        r.dead.pop_back();
    }
}
/// The scratch buffer's address for a build of that size (one build at a time uses it, a barrier between them)
uint64_t scratch_for(Rt& r, uint64_t size) {
    size += r.scratch_align;
    if (r.scratch.size < size) {
        later(r, r.scratch);
        if (!make_buf(r.scratch, std::max<uint64_t>(size, 4 << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return 0;
    }
    return (r.scratch.address + r.scratch_align - 1) / r.scratch_align * r.scratch_align;
}

float half_float(uint16_t h) {
    const uint32_t e = h >> 10 & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((float)(m | 1024), (int)e - 25);
    return h & 0x8000 ? -v : v;
}

uint64_t key_of(const RtDraw& d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t v : {d.vbuf, d.ibuf, (uint64_t)d.count, (uint64_t)(uint32_t)d.base_vertex, (uint64_t)d.stride << 32 | d.pos,
                       (uint64_t)d.itype << 1 | (d.half ? 1 : 0)})
        h = (h ^ v) * 0x100000001b3ull, h ^= h >> 29;
    return h;
}

/// A structure of that type over one geometry of at most `count` primitives, in a buffer of its own
bool make_as(Rt& r, VkAccelerationStructureTypeKHR type, VkAccelerationStructureBuildGeometryInfoKHR& bi, uint32_t count, Mesh& out,
             uint64_t* scratch) {
    VkAccelerationStructureBuildSizesInfoKHR si{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    r.sizes(dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &count, &si);
    if (!si.accelerationStructureSize) return false;
    if (!make_buf(out.store, si.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false)) return false;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = out.store.buffer, ci.size = si.accelerationStructureSize, ci.type = type;
    if (r.create(dev.device, &ci, nullptr, &out.as) != VK_SUCCESS) {
        destroy_buffer(out.store.buffer, out.store.memory);
        return out = {}, false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = out.as;
    out.address = r.address(dev.device, &ai);
    *scratch = si.buildScratchSize;
    return true;
}

/// Room for a structure of `size` bytes in the blocks: its block and *offset in it, -1 for none
int block_room(Rt& r, uint64_t size, uint64_t* offset) {
    size = (size + 255) & ~255ull;
    const bool shared = size <= kBlockBytes / 2;
    if (shared && r.block_now >= 0 && r.blocks[r.block_now].at + size <= r.blocks[r.block_now].buf.size) {
        Block& b = r.blocks[r.block_now];
        *offset = b.at, b.at += size, b.live++;
        return r.block_now;
    }
    Block b;
    if (!make_buf(b.buf, shared ? kBlockBytes : size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false)) return -1;
    b.at = size, b.live = 1, *offset = 0;
    int at = -1;
    for (size_t i = 0; i < r.blocks.size() && at < 0; i++)
        if (!r.blocks[i].buf.buffer) at = (int)i;
    if (at < 0) r.blocks.push_back(b), at = (int)r.blocks.size() - 1;
    else r.blocks[at] = b;
    if (shared) r.block_now = at;
    return at;
}
/// A structure in a block is gone: the block too, once it holds none and is not the one being filled
void block_leave(Rt& r, int i) {
    Block& b = r.blocks[i];
    if (--b.live <= 0 && i != r.block_now) later(r, b.buf), b = {};
}

/// A mesh read from guest memory and built: 1; 0 when it cannot be (unreadable, out of its buffer, too large); 2 when
/// this frame's stage has no room for it (the next frame's)
int build_mesh(Rt& r, VkCommandBuffer cmd, const RtDraw& d, Mesh& out) {
    const uint32_t isz = d.itype == 0 ? 1 : d.itype == 1 ? 2 : 4, n = (uint32_t)d.count;
    if (d.count < 3 || d.count % 3 || d.count > 6000000 || !d.stride) return false;
    auto tp = std::chrono::steady_clock::now();
    const auto lap = [&](int k) {
        const auto t = std::chrono::steady_clock::now();
        r.phase_ns[k].fetch_add((t - tp).count(), std::memory_order_relaxed), tp = t;
    };
    std::vector<uint8_t> raw((size_t)n * isz);
    if (!memory_read_as_submitted(d.ibuf, raw.data(), raw.size())) return false;
    std::vector<uint32_t> idx(n);
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = 0;
        std::memcpy(&v, raw.data() + (size_t)i * isz, isz);
        const int64_t at = (int64_t)v + d.base_vertex;
        lo = std::min(lo, at), hi = std::max(hi, at);
        idx[i] = v;
    }
    const uint64_t nv = (uint64_t)(hi - lo + 1), psz = d.half ? 6 : 12;
    if (lo < 0 || nv > 4000000 || (uint64_t)hi * d.stride + d.pos + psz > d.vsize) return false;
    raw.resize((size_t)(nv * d.stride));
    if (!memory_read_as_submitted(d.vbuf + (uint64_t)lo * d.stride, raw.data(), (nv - 1) * d.stride + d.pos + psz)) return false;
    lap(0);
    Buf in;
    const uint64_t vbytes = nv * 12, bytes = vbytes + (uint64_t)n * 4;
    const bool own = bytes > kStageBytes;   // (a large one: a buffer of its own)
    const uint64_t staged = (r.stage_at + 15) & ~15ull;
    if (own) {
        if (!make_buf(in, bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true)) return 0;
    } else {
        Buf& stage = r.stage[r.ring_at];
        if (!stage.buffer && !make_buf(stage, kStageBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true))
            return 0;
        if (staged + bytes > kStageBytes) return 2;
        in = stage, in.map = (uint8_t*)stage.map + staged, in.address = stage.address + staged;
        r.stage_at = staged + bytes;
    }
    // (made here and copied over: the buffer is the GPU's memory, written through and never read back. Reading each
    // vertex back from it to check it was 2.4 ms a frame of builds, the stutter of walking)
    std::vector<float> made(nv * 3);
    float* p = made.data();
    for (uint64_t v = 0; v < nv; v++, p += 3) {
        const uint8_t* at = raw.data() + v * d.stride + d.pos;
        if (d.half) {
            uint16_t h[3];
            std::memcpy(h, at, 6);
            p[0] = half_float(h[0]), p[1] = half_float(h[1]), p[2] = half_float(h[2]);
        } else {
            std::memcpy(p, at, 12);
        }
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) p[0] = p[1] = p[2] = 0.0f;
    }
    std::memcpy(in.map, made.data(), vbytes);
    for (uint32_t i = 0; i < n; i++) idx[i] = (uint32_t)((int64_t)idx[i] + d.base_vertex - lo);
    std::memcpy((uint8_t*)in.map + vbytes, idx.data(), (size_t)n * 4);
    if (own) flush_mapped(in.memory, bytes);
    else flush_mapped_range(in.memory, staged, bytes);

    lap(1);
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& t = g.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, t.vertexData.deviceAddress = in.address, t.vertexStride = 12;
    t.maxVertex = (uint32_t)nv - 1, t.indexType = VK_INDEX_TYPE_UINT32, t.indexData.deviceAddress = in.address + vbytes;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, bi.geometryCount = 1, bi.pGeometries = &g;
    VkAccelerationStructureBuildSizesInfoKHR si{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    const uint32_t triangles = n / 3;
    r.sizes(dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &triangles, &si);
    uint64_t offset = 0;
    const int block = si.accelerationStructureSize ? block_room(r, si.accelerationStructureSize, &offset) : -1;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.offset = offset, ci.size = si.accelerationStructureSize, ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (block >= 0) ci.buffer = r.blocks[block].buf.buffer;
    if (block < 0 || r.create(dev.device, &ci, nullptr, &out.as) != VK_SUCCESS ||
        !(bi.scratchData.deviceAddress = scratch_for(r, si.buildScratchSize))) {
        if (block >= 0) block_leave(r, block);
        later(r, Buf{}, out.as);
        if (own) later(r, in);
        return out = {}, 0;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = out.as;
    out.address = r.address(dev.device, &ai);
    out.block = block, out.size = si.accelerationStructureSize;
    lap(2);
    bi.dstAccelerationStructure = out.as;
    const VkAccelerationStructureBuildRangeInfoKHR range{n / 3, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    r.build(cmd, 1, &bi, &ranges);
    note_work();
    full_barrier(cmd, "barrier (rt mesh)");
    if (own) later(r, in);
    lap(3);
    out.triangles = n / 3;
    return 1;
}

/// A skinned draw's model read from guest memory: false when it cannot be
bool read_model(const SkinnedDraw& k, Rt::Model& m) {
    const uint32_t isz = k.itype == 0 ? 1 : k.itype == 1 ? 2 : 4, n = (uint32_t)k.count;
    if (k.count < 3 || k.count % 3 || k.count > 3000000 || !k.stride || k.sets < 1 || k.sets > 2) return false;
    std::vector<uint8_t> raw((size_t)n * isz);
    if (!memory_read_as_submitted(k.ibuf, raw.data(), raw.size())) return false;
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    m.idx.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = 0;
        std::memcpy(&v, raw.data() + (size_t)i * isz, isz);
        const int64_t at = (int64_t)v + k.base_vertex;
        lo = std::min(lo, at), hi = std::max(hi, at);
        m.idx[i] = v;
    }
    const uint64_t nv = (uint64_t)(hi - lo + 1);
    if (lo < 0 || nv > 2000000 || (uint64_t)(hi + 1) * k.stride > k.vsize) return false;
    for (uint32_t i = 0; i < n; i++) m.idx[i] = (uint32_t)((int64_t)m.idx[i] + k.base_vertex - lo);
    raw.resize((size_t)(nv * k.stride));
    if (!memory_read_as_submitted(k.vbuf + (uint64_t)lo * k.stride, raw.data(), raw.size())) return false;
    const bool half = k.formats & dlss_actor::kHalfPosition;
    m.sets = k.sets;
    m.pos.resize(nv * 3), m.joints.assign(nv * 4 * k.sets, 0), m.weights.assign(nv * 4 * k.sets, 0);
    for (uint64_t v = 0; v < nv; v++) {
        const uint8_t* at = raw.data() + v * k.stride;
        float* p = &m.pos[v * 3];
        if (half) {
            uint16_t h[3];
            std::memcpy(h, at + k.pos, 6);
            p[0] = half_float(h[0]), p[1] = half_float(h[1]), p[2] = half_float(h[2]);
        } else {
            std::memcpy(p, at + k.pos, 12);
        }
        for (int s = 0; s < k.sets; s++) {
            uint16_t* j = &m.joints[(v * k.sets + s) * 4];
            if (k.formats & (dlss_actor::kByteJoints << s)) for (int c = 0; c < 4; c++) j[c] = at[k.joints[s] + c];
            else std::memcpy(j, at + k.joints[s], 8);
            std::memcpy(&m.weights[(v * k.sets + s) * 4], at + k.weights[s], 4);
        }
    }
    // (smooth normals: each triangle's, by its area, summed at its corners; corners at one place are one, so a seam in
    // the texture does not crease the skin)
    std::unordered_map<uint64_t, uint32_t> weld;
    std::vector<uint32_t> group(nv);
    for (uint64_t v = 0; v < nv; v++) {
        uint32_t bits[3];
        std::memcpy(bits, &m.pos[v * 3], 12);
        const uint64_t key = ((bits[0] * 0x9e3779b97f4a7c15ull) ^ bits[1]) * 0x100000001b3ull ^ bits[2];
        group[v] = weld.emplace(key, (uint32_t)weld.size()).first->second;
    }
    std::vector<float> sum(weld.size() * 3, 0.0f);
    for (uint32_t i = 0; i + 2 < n; i += 3) {
        const float *a = &m.pos[(size_t)m.idx[i] * 3], *b = &m.pos[(size_t)m.idx[i + 1] * 3], *c = &m.pos[(size_t)m.idx[i + 2] * 3];
        const float e[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, f[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float fn[3] = {e[1] * f[2] - e[2] * f[1], e[2] * f[0] - e[0] * f[2], e[0] * f[1] - e[1] * f[0]};
        for (int corner = 0; corner < 3; corner++)
            for (int x = 0; x < 3; x++) sum[(size_t)group[m.idx[i + corner]] * 3 + x] += fn[x];
    }
    m.normal.resize(nv * 3);
    for (uint64_t v = 0; v < nv; v++) {
        const float* g = &sum[(size_t)group[v] * 3];
        const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        for (int x = 0; x < 3; x++) m.normal[v * 3 + x] = len > 1e-20f ? g[x] / len : (x == 1 ? 1.0f : 0.0f);
    }
    return true;
}

/// This frame's skinned draws, skinned here and built as one structure in world space: its triangles, 0 for none
uint32_t build_skinned(Rt& r, VkCommandBuffer cmd, uint64_t now, int slot) {
    uint64_t nv = 0, ni = 0;
    for (auto it = r.poses.begin(); it != r.poses.end();) {
        auto m = r.models.find(it->key);
        if (m == r.models.end()) {
            Rt::Model made;
            if (r.bad.count(it->key) || !read_model(it->k, made)) {
                r.bad.insert(it->key);
                it = r.poses.erase(it);
                continue;
            }
            if (r.models.size() > 2048) r.models.clear();
            m = r.models.emplace(it->key, std::move(made)).first;
        }
        m->second.used = now;
        nv += m->second.pos.size() / 3, ni += m->second.idx.size();
        ++it;
    }
    if (!ni || nv > 4000000 || ni > 12000000) return 0;
    Buf& in = r.skin_ring[slot];
    const uint64_t vbytes = nv * 12, bytes = vbytes + ni * 4 + nv * 12;   // (positions, indices, normals)
    if (in.size < bytes) {
        later(r, in);
        if (!make_buf(in, bytes + bytes / 2, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true)) return 0;
    }
    float* out = (float*)in.map;
    uint32_t* q = (uint32_t*)((uint8_t*)in.map + vbytes);
    float* normals = (float*)((uint8_t*)in.map + vbytes + ni * 4);
    uint32_t base = 0;
    for (const Rt::Pose& p : r.poses) {
        const Rt::Model& m = r.models.find(p.key)->second;
        const uint32_t count = (uint32_t)(m.pos.size() / 3), nb = (uint32_t)(p.bones.size() / 12);
        // (a vertex no bone moves (no weight, or joints the palette does not have) stays at the object's origin, far from
        // the character: its triangles were spikes from the body to there, and their shadows stripes across the ground
        // beside the character's own. Such a vertex's triangles are left out, and any triangle longer than a character is)
        std::vector<float> at((size_t)count * 3);
        std::vector<uint8_t> placed(count);
        for (uint32_t v = 0; v < count; v++, out += 3, normals += 3) {
            const float* x = &m.pos[(size_t)v * 3];
            const float* y = &m.normal[(size_t)v * 3];
            float acc[3] = {p.origin[0], p.origin[1], p.origin[2]}, turned[3] = {0, 0, 0}, weight = 0;
            for (int k = 0; k < 4 * m.sets; k++) {
                const uint8_t w8 = m.weights[(size_t)v * 4 * m.sets + k];
                const uint32_t j = m.joints[(size_t)v * 4 * m.sets + k];
                if (!w8 || j >= nb) continue;
                const float w = w8 / 255.0f;
                weight += w;
                const float* b = &p.bones[(size_t)j * 12];
                for (int c = 0; c < 3; c++) {
                    acc[c] += w * (b[c * 4] * x[0] + b[c * 4 + 1] * x[1] + b[c * 4 + 2] * x[2] + b[c * 4 + 3]);
                    turned[c] += w * (b[c * 4] * y[0] + b[c * 4 + 1] * y[1] + b[c * 4 + 2] * y[2]);
                }
            }
            const bool fine = std::isfinite(acc[0]) && std::isfinite(acc[1]) && std::isfinite(acc[2]);
            out[0] = fine ? acc[0] : 0.0f, out[1] = fine ? acc[1] : 0.0f, out[2] = fine ? acc[2] : 0.0f;
            placed[v] = fine && weight > 0.5f;
            for (int c = 0; c < 3; c++) at[(size_t)v * 3 + c] = fine ? acc[c] : 0.0f;
            const float len = std::sqrt(turned[0] * turned[0] + turned[1] * turned[1] + turned[2] * turned[2]);
            const bool has = std::isfinite(len) && len > 1e-12f;
            for (int c = 0; c < 3; c++) normals[c] = has ? turned[c] / len : 0.0f;
        }
        for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
            const uint32_t a = m.idx[i], b = m.idx[i + 1], c = m.idx[i + 2];
            bool keep = placed[a] && placed[b] && placed[c];
            const uint32_t pair[3][2] = {{a, b}, {b, c}, {c, a}};
            for (int e = 0; e < 3 && keep; e++) {   // (an edge three times as long as the model has it: stretched)
                const float* u = &at[(size_t)pair[e][0] * 3];
                const float* w = &at[(size_t)pair[e][1] * 3];
                const float* mu = &m.pos[(size_t)pair[e][0] * 3];
                const float* mw = &m.pos[(size_t)pair[e][1] * 3];
                const float dx = u[0] - w[0], dy = u[1] - w[1], dz = u[2] - w[2];
                const float ex = mu[0] - mw[0], ey = mu[1] - mw[1], ez = mu[2] - mw[2];
                keep = dx * dx + dy * dy + dz * dz < 9.0f * (ex * ex + ey * ey + ez * ez) + 0.0004f;
            }
            // (left out: its three corners the same one, a triangle of no area)
            *q++ = base + a, *q++ = base + (keep ? b : a), *q++ = base + (keep ? c : a);
            if (!keep) r.skin_dropped++;
        }
        base += count;
    }
    flush_mapped(in.memory, bytes);
    const uint32_t triangles = (uint32_t)(ni / 3);
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;   // (not opaque: rt_debug.comp's rays pass a character's own layers)
    auto& t = g.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, t.vertexData.deviceAddress = in.address, t.vertexStride = 12;
    t.indexType = VK_INDEX_TYPE_UINT32, t.indexData.deviceAddress = in.address + vbytes;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, bi.geometryCount = 1, bi.pGeometries = &g;
    static uint64_t scratch = 0;
    if (!r.skin.as || triangles > r.skin_cap) {   // (made for half as many again, so a character walking in does not make it anew)
        later(r, r.skin.store, r.skin.as);
        r.skin = {};
        r.skin_cap = std::max(triangles + triangles / 2, 65536u);
        t.maxVertex = r.skin_cap * 3;
        if (!make_as(r, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, bi, r.skin_cap, r.skin, &scratch)) return r.skin_cap = 0, 0;
    }
    t.maxVertex = (uint32_t)nv - 1;
    if (!(bi.scratchData.deviceAddress = scratch_for(r, scratch))) return 0;
    bi.dstAccelerationStructure = r.skin.as;
    const VkAccelerationStructureBuildRangeInfoKHR range{triangles, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    r.build(cmd, 1, &bi, &ranges);
    note_work();
    full_barrier(cmd, "barrier (rt actors)");
    r.skin_triangles = triangles, r.skin_draws = r.poses.size();
    r.skin_indices = in.address + vbytes, r.skin_normals = in.address + vbytes + ni * 4;
    return triangles;
}

/// The world's light cache (rt_debug.comp): 1024 x 1024 slots of a hashed grid, zero (nothing known) when made
bool make_cache(Rt& r, VkCommandBuffer cmd) {
    if (r.cache_view) return true;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = VK_FORMAT_R16G16B16A16_SFLOAT, ic.extent = {1024, 1024, 1};
    ic.mipLevels = 1, ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(ic, r.cache, r.cache_memory)) return false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = r.cache, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = ic.format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &r.cache_view) != VK_SUCCESS) {
        destroy_image(r.cache, r.cache_memory);
        return r.cache = VK_NULL_HANDLE, r.cache_view = VK_NULL_HANDLE, false;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = r.cache, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    const VkClearColorValue zero{};
    vkCmdClearColorImage(cmd, r.cache, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &vc.subresourceRange);
    full_barrier(cmd, "barrier (rt cache)");
    return true;
}

/// The materials' image (rt_debug.comp): 256 x 256 mean albedos, a texel a colour texture, black (not known) when made
bool make_atlas(Rt& r, VkCommandBuffer cmd) {
    if (r.atlas_view) return true;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = VK_FORMAT_R8G8B8A8_UNORM, ic.extent = {256, 256, 1};
    ic.mipLevels = 1, ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(ic, r.atlas, r.atlas_memory)) return false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = r.atlas, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = ic.format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &r.atlas_view) != VK_SUCCESS) {
        destroy_image(r.atlas, r.atlas_memory);
        return r.atlas = VK_NULL_HANDLE, r.atlas_view = VK_NULL_HANDLE, false;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = r.atlas, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    const VkClearColorValue zero{};
    vkCmdClearColorImage(cmd, r.atlas, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &vc.subresourceRange);
    full_barrier(cmd, "barrier (rt materials)");
    return true;
}

/// The colour textures seen since the last frame: each one's smallest level blitted to its texel of the materials' image
void copy_materials(Rt& r, VkCommandBuffer cmd) {
    if (r.blits.empty() || !make_atlas(r, cmd)) return;
    const uint64_t gen = texture_destroy_generation();
    int done = 0;
    for (const Rt::Blit& b : r.blits) {
        // (a texture destroyed since it was noted: its pointer is not to be touched. Its slot stays black: not known)
        if (b.gen != gen) continue;
        const Texture* t = b.tex;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, t->format, &fp);
        if (!t->image || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) continue;
        const uint32_t level = t->levels ? t->levels - 1 : 0;
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.srcOffsets[1] = {(int32_t)std::max(1u, t->width >> level), (int32_t)std::max(1u, t->height >> level), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[0] = {(int32_t)(b.slot & 255), (int32_t)(b.slot >> 8), 0};
        region.dstOffsets[1] = {(int32_t)(b.slot & 255) + 1, (int32_t)(b.slot >> 8) + 1, 1};
        vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, r.atlas, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_LINEAR);
        done++;
    }
    r.blits.clear();
    if (done) full_barrier(cmd, "barrier (rt materials)");
}

bool make_pipeline(Rt& r) {
    if (r.pipe || r.pipe_tried) return r.pipe != VK_NULL_HANDLE;
    r.pipe_tried = true;
    const VkDescriptorType types[6] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding b[6]{};
    for (uint32_t i = 0; i < 6; i++) b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 6, li.pBindings = b;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 224};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &r.set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof RT_DEBUG_COMP_SPV, mi.pCode = RT_DEBUG_COMP_SPV;
    VkShaderModule m{};
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &r.set) != VK_SUCCESS ||
        vkCreatePipelineLayout(dev.device, &pi, nullptr, &r.layout) != VK_SUCCESS ||
        vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS)
        return logf("vk: rt: the check's pipeline layout was not made"), false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = r.layout;
    if (vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &r.pipe) != VK_SUCCESS) r.pipe = VK_NULL_HANDLE;
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (!r.pipe) logf("vk: rt: the check's pipeline was not made");
    return r.pipe != VK_NULL_HANDLE;
}

} // namespace

void rt_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    Rt& r = rt();
    {   // (the menu's row, from the file beside the game as vk_main.cpp's mesh_shaders: the menu is not read yet)
        char exe[MAX_PATH];
        const DWORD len = game_module_path(exe, MAX_PATH);
        std::string ini(exe, len);
        ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "hookshot_options.ini";
        if (FILE* f = fopen(ini.c_str(), "r")) {
            char line[80];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "ray_tracing=", 12)) r.asked = atoi(line + 12) != 0;
            fclose(f);
        }
    }
    if (const char* e = getenv("RX_VK_RT")) r.asked = atoi(e) != 0;
    if (!r.asked) return;
    const char* const names[4] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                  VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                  VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME};
    for (const char* n : names)
        if (std::none_of(all.begin(), all.end(), [&](const VkExtensionProperties& p) { return !strcmp(p.extensionName, n); })) {
            logf("vk: rt: the device has no %s: off", n);
            return;
        }
    for (const char* n : names) want.push_back(n);
    r.have_ext = true;
}

void rt_device_features(void**& tail) {
    Rt& r = rt();
    if (!r.have_ext) return;
    *tail = &r.asf, tail = &r.asf.pNext;
    *tail = &r.rqf, tail = &r.rqf.pNext;
    *tail = &r.pff, tail = &r.pff.pNext;
}

void rt_device_created() {
    Rt& r = rt();
    if (!r.have_ext) return;
    if (!r.asf.accelerationStructure || !r.rqf.rayQuery || !r.pff.rayTracingPositionFetch || !dev.push_descriptor) {
        logf("vk: rt: the device has no acceleration structures, ray queries, position fetch or push descriptors: off");
        return;
    }
#define RT_LOAD(member, name) r.member = (PFN_##name)vkGetDeviceProcAddr(dev.device, #name)
    RT_LOAD(create, vkCreateAccelerationStructureKHR), RT_LOAD(destroy, vkDestroyAccelerationStructureKHR);
    RT_LOAD(sizes, vkGetAccelerationStructureBuildSizesKHR), RT_LOAD(build, vkCmdBuildAccelerationStructuresKHR);
    RT_LOAD(address, vkGetAccelerationStructureDeviceAddressKHR);
#undef RT_LOAD
    VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &ap;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    r.scratch_align = std::max(1u, ap.minAccelerationStructureScratchOffsetAlignment);
    r.ok = r.create && r.destroy && r.sizes && r.build && r.address;
    logf("vk: rt: acceleration structures and ray queries %s (RX_VK_RT)", r.ok ? "on" : "not loaded: off");
}

bool rt_on() { return rt().ok; }

/// The draw's colour texture: the first of its fragment stage's textures in an sRGB format (the census: slot 0 of 83% of a
/// frame's draws, BC7 or ASTC), as a slot of the materials' image; 1 for none
void rt_note_lights(uint64_t address, uint64_t size) {
    Rt& r = rt();
    const uint64_t now = texture_frame_no();
    if (!r.ok || size < 0x50 || r.lights_frame.exchange(now, std::memory_order_relaxed) == now) return;
    float f[20];
    if (!memory_read_as_submitted(address, f, sizeof f)) return;
    std::lock_guard lk(r.mu);
    // (the light travels along f[0..2]: the buffer is the lights' when that is the way from the sun the shadow maps gave)
    const float along = -(f[0] * r.to_sun[0] + f[1] * r.to_sun[1] + f[2] * r.to_sun[2]);
    const bool fine = now - r.sun_frame < 4 && along > 0.99f && std::isfinite(f[16] + f[17] + f[18]) && f[16] >= 0.0f && f[17] >= 0.0f &&
                      f[18] >= 0.0f && f[16] + f[17] + f[18] < 1000.0f;
    for (int i = 0; i < 3; i++) r.sun_colour[i] = fine ? f[16 + i] : 0.0f;
    static int told = 0;
    if (fine && told < 3 && now % 900 == 0) told++, logf("vk: rt: the game's sun: colour %.2f %.2f %.2f", f[16], f[17], f[18]);
}

static uint32_t material_of(Rt& r, const RtDraw& d) {
    // (the world's main program keeps its colours in an array of 90 textures, the layer chosen by its material: with an
    // array among the first textures the sRGB one beside it is a shared tint, the same tan for every wall and rock.
    // ponytail: no material for those until the layer is read from the material's buffer (fragment uniform buffer 2))
    for (int i = 0; i < 4; i++) {
        nvn_tex x;
        nvn_view v;
        bool has_view = false;
        if (!d.tex[i] || !texture_of((int)(d.tex[i] & 0xFFFFF), &x, &v, &has_view) || x.d.depth <= 1) continue;
        const int f = x.d.format;   // (an array of colour textures: the atlas. Arrays of other things, noise and masks, are no sign of it)
        if ((f >= 0x45 && f <= 0x48) || f == 0x4e || (f >= 0x87 && f <= 0x94)) return 1;
    }
    for (int i = 0; i < 4; i++) {
        nvn_tex x;
        nvn_view v;
        bool has_view = false;
        if (!d.tex[i] || !texture_of((int)(d.tex[i] & 0xFFFFF), &x, &v, &has_view)) continue;
        const int f = x.d.format;
        if (!((f >= 0x45 && f <= 0x48) || f == 0x4e || (f >= 0x87 && f <= 0x94))) continue;
        Texture* t = texture_get(&x, true);
        if (!t || !t->image || t->type != VK_IMAGE_TYPE_2D || t->samples != VK_SAMPLE_COUNT_1_BIT || t->sparse) return 1;
        std::lock_guard lk(r.mu);
        const uint64_t id = (x.gpu * 0x9e3779b97f4a7c15ull) ^ x.serial;
        const auto it = r.materials.find(id);
        if (it != r.materials.end()) return it->second;
        if (r.material_next > 0xffff) return 1;   // (the image is full)
        const uint32_t slot = r.material_next++;
        r.materials.emplace(id, slot);
        r.blits.push_back({slot, t, texture_destroy_generation()});
        if (slot < 60)
            logf("vk: rt: material %u: fragment texture %d, %ux%u, depth %u, levels %u, format %#x, target %d; the image %ux%u, %u layers, %u levels%s",
                 slot, i, x.d.width, x.d.height, x.d.depth, x.d.levels, (unsigned)x.d.format, x.d.target, t->width, t->height, t->layers,
                 t->levels, t->astc ? ", ASTC decoded" : "");
        return slot;
    }
    return 1;
}

void rt_note_draw(const RtDraw& d, bool shadow_pass) {
    Rt& r = rt();
    if (!r.ok) return;
    Timed timed{r.note_ns};
    float o[19];   // (the world matrix, 3 x 4 row major, and the origin added to it at 0x40)
    if (!memory_read_as_submitted(d.object, o, sizeof o)) return;
    Instance in;
    in.key = key_of(d);
    std::memcpy(in.m, o, sizeof in.m);
    in.m[3] += o[16], in.m[7] += o[17], in.m[11] += o[18];
    uint64_t place = 0xcbf29ce484222325ull;
    for (float v : in.m) {
        if (!std::isfinite(v)) return;
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        place = (place ^ bits) * 0x100000001b3ull;
    }
    in.place = place, in.same = (place ^ in.key) * 0x100000001b3ull, in.kind = shadow_pass ? 1 : 0;
    bool look = false;   // (a built mesh drawn into the scene with no material yet: its colour texture looked for, once)
    {
        std::lock_guard lk(r.mu);
        if (r.instances.size() >= 4 * kMaxInstances) r.instances.clear();   // (no upscale for a while: stale)
        r.instances.push_back(in);
        const auto it = r.meshes.find(in.key);
        if (it != r.meshes.end()) {
            look = !shadow_pass && it->second.material == 0;
        } else if (!r.bad.count(in.key) && r.pending.size() < 16384) {
            Rt::Pending& w = r.pending[in.key];
            const uint64_t now = texture_frame_no();
            if (w.last != now) w.d = d, w.last = now, w.frames++;
        }
    }
    if (!look) return;
    const uint32_t slot = material_of(r, d);
    std::lock_guard lk(r.mu);
    const auto it = r.meshes.find(in.key);
    if (it != r.meshes.end()) it->second.material = slot;
}

bool rt_note_shadow_draw(const RtDraw* k, bool skinned, uint64_t camera, uint64_t camera_size, bool cutout, uint64_t object) {
    Rt& r = rt();
    if (!r.ok || camera_size < 0xb0) return false;
    const uint64_t now = texture_frame_no();
    bool built = false;   // (the rays have this mesh now: one still to be built keeps its place in the game's map)
    {   // (the pass's camera, read once for all its draws: an orthographic projection's w row is 0, 0, 0, 1)
        Timed timed{r.note_ns};
        std::lock_guard lk(r.mu);
        if (r.light_camera != camera || r.light_frame != now) {
            float c[0xb0 / 4];
            r.light_camera = camera, r.light_frame = now, r.light_is_sun = false;
            if (memory_read_as_submitted(camera, c, sizeof c) && c[0xa0 / 4] == 0.0f && c[0xa4 / 4] == 0.0f && c[0xa8 / 4] == 0.0f &&
                c[0xac / 4] == 1.0f) {
                // (the view-projection's depth row runs along the light: the way to the sun is against it, and up)
                float d[3] = {-c[0x50 / 4], -c[0x54 / 4], -c[0x58 / 4]};
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (std::isfinite(len) && len > 1e-6f) {
                    const float sign = d[1] < 0.0f ? -1.0f : 1.0f;
                    for (int i = 0; i < 3; i++) r.to_sun[i] = sign * d[i] / len;
                    r.sun_frame = now, r.light_is_sun = true;
                    static int told = 0;
                    if (told < 6 && (now % 600 == 0 || !told)) {
                        told++;
                        char t[900];
                        int m = snprintf(t, sizeof t, "vk: rt: a shadow map's camera (size %llu), to the sun %.3f %.3f %.3f; from 0x30:", (unsigned long long)camera_size, r.to_sun[0], r.to_sun[1], r.to_sun[2]);
                        for (int i = 0x30 / 4; i < 0xb0 / 4; i++) m += snprintf(t + m, sizeof t - m, " %.4g", c[i]);
                        logf("%s", t);
                    }
                }
            }
        }
        if (!r.light_is_sun) return false;
        built = k && r.meshes.count(key_of(*k));
    }
    if (cutout) {   // (its place marked: nothing drawn there is in the rays this frame; the game's map keeps its shadow)
        float o[19];
        if (!object || !memory_read_as_submitted(object, o, sizeof o)) return false;
        o[3] += o[16], o[7] += o[17], o[11] += o[18];
        Instance in{};
        uint64_t place = 0xcbf29ce484222325ull;
        for (int i = 0; i < 12; i++) {
            uint32_t bits;
            std::memcpy(&bits, &o[i], 4);
            place = (place ^ bits) * 0x100000001b3ull;
        }
        in.place = place, in.same = place ^ 0x5bd1e995, in.kind = 2;
        std::lock_guard lk(r.mu);
        if (r.instances.size() < 4 * kMaxInstances) r.instances.push_back(in);
        return false;
    }
    if (k) rt_note_draw(*k, true);
    // (left out of the game's map only while the rays draw their shadows: the row on, and rt_frame traced a moment ago)
    return (built || skinned) && opts.sun > 0 && now - r.traced_frame.load(std::memory_order_relaxed) < 4;
}

void rt_note_skinned(const SkinnedDraw& k) {
    Rt& r = rt();
    if (!r.ok || !k.bones || k.bones_size < 48 || !k.object || k.object_size < 0x4c) return;
    Timed timed{r.note_ns};
    Rt::Pose p;
    p.k = k;
    p.bones.resize(std::min<uint64_t>(k.bones_size / 48, 280) * 12);
    if (!memory_read_as_submitted(k.bones, p.bones.data(), p.bones.size() * 4) ||
        !memory_read_as_submitted(k.object + 0x40, p.origin, sizeof p.origin))
        return;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t v : {k.vbuf, k.ibuf, (uint64_t)k.count, (uint64_t)(uint32_t)k.base_vertex, (uint64_t)k.stride})
        h = (h ^ v) * 0x100000001b3ull, h ^= h >> 29;
    p.key = h;
    uint64_t same = h;   // (the same model in the same pose again: a second pass over the scene)
    const auto mix = [&](const float* f, size_t n) {
        for (size_t i = 0; i < n; i++) {
            uint32_t bits;
            std::memcpy(&bits, f + i, 4);
            same = (same ^ bits) * 0x100000001b3ull;
        }
    };
    mix(p.origin, 3), mix(p.bones.data(), std::min<size_t>(p.bones.size(), 48));
    std::lock_guard lk(r.mu);
    if (r.poses.size() >= 512) r.poses.clear();   // (no upscale for a while: stale)
    if (!r.seen.insert(same).second) return;
    r.poses.push_back(std::move(p));
}

void rt_frame(VkCommandBuffer cmd, const float view_proj[16], VkImageView color, VkImageView depth, VkSampler point, uint32_t w,
              uint32_t h, float ao, float seed, VkImageView scene, float gi) {
    Rt& r = rt();
    if (!r.ok) return;
    std::lock_guard lk(r.mu);
    const uint64_t now = texture_frame_no();
    free_dead(r);
    r.ring_at = (r.ring_at + 1) % kRing, r.stage_at = 0;
    // (the new meshes: 1 ms of them a frame (RX_VK_RT_BUILD_MS), so walking into an area does not slow the game; what
    // is not built yet is not in the rays for a moment. 128 meshes a frame, as it was first, were 30 to 60 ms frames)
    static const double build_ms = getenv("RX_VK_RT_BUILD_MS") ? atof(getenv("RX_VK_RT_BUILD_MS")) : 1.0;
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t budget = 600000;
    for (auto it = r.pending.begin(); it != r.pending.end();) {
        if (now - it->second.last > 2) { it = r.pending.erase(it); continue; }   // (not drawn any more)
        if (it->second.frames < 4 || budget == 0 ||
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= build_ms) {
            ++it;
            continue;
        }
        Mesh m;
        const int made = build_mesh(r, cmd, it->second.d, m);
        if (made == 2) break;   // (the stage is full: the rest next frame)
        if (made) {
            m.used = now;
            budget -= std::min(budget, m.triangles);
            r.built++, r.built_triangles += m.triangles;
            r.meshes.emplace(it->first, m);
        } else {
            if (r.bad.size() > 65536) r.bad.clear();
            r.bad.insert(it->first), r.skipped++;
        }
        it = r.pending.erase(it);
    }
    // (the top level: this frame's instances whose mesh is built)
    Buf& ring = r.ring[r.ring_at];
    if (!ring.buffer && !make_buf(ring, (uint64_t)kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR),
                                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true)) {
        r.instances.clear(), r.seen.clear();
        return;
    }
    auto* out = (VkAccelerationStructureInstanceKHR*)ring.map;
    uint32_t n = 0;
    // (by place, the scene's own before the shadow maps': a shadow map's draw where the scene drew is left out)
    std::sort(r.instances.begin(), r.instances.end(), [](const Instance& a, const Instance& b) {
        const int order[3] = {1, 2, 0};   // (a place's mark first, then the scene's, then the shadow maps')
        return a.place != b.place ? a.place < b.place : a.kind != b.kind ? order[a.kind] < order[b.kind] : a.same < b.same;
    });
    r.instances.erase(std::unique(r.instances.begin(), r.instances.end(), [](const Instance& a, const Instance& b) { return a.same == b.same; }),
                      r.instances.end());
    uint64_t scene_place = 0, cut_place = 0;
    bool scene_there = false, cut_there = false;
    for (const Instance& in : r.instances) {
        if (n >= kMaxInstances - 1) break;
        if (in.kind == 2) { cut_place = in.place, cut_there = true; continue; }
        if (cut_there && cut_place == in.place) continue;
        if (in.kind == 0) scene_place = in.place, scene_there = true;
        else if (scene_there && scene_place == in.place) continue;
        const auto it = r.meshes.find(in.key);
        if (it == r.meshes.end()) continue;
        it->second.used = now;
        VkAccelerationStructureInstanceKHR& a = out[n];
        std::memcpy(a.transform.matrix, in.m, sizeof in.m);
        a.instanceCustomIndex = it->second.material, a.mask = 0xff, a.instanceShaderBindingTableRecordOffset = 0;
        a.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        a.accelerationStructureReference = it->second.address;
        n++;
    }
    // (the characters: one more instance, where the world is)
    r.skin_indices = r.skin_normals = 0;
    if (n < kMaxInstances && build_skinned(r, cmd, now, r.ring_at)) {
        VkAccelerationStructureInstanceKHR& a = out[n++];
        a = {};
        a.transform.matrix[0][0] = a.transform.matrix[1][1] = a.transform.matrix[2][2] = 1.0f;
        a.instanceCustomIndex = 0xfffffe, a.mask = 0xff;
        a.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        a.accelerationStructureReference = r.skin.address;
    }
    const size_t noted = r.instances.size(), posed = r.poses.size();
    r.instances.clear(), r.seen.clear(), r.poses.clear();
    flush_mapped(ring.memory, std::max<uint64_t>(1, n) * sizeof *out);
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress = ring.address;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, bi.geometryCount = 1, bi.pGeometries = &g;
    static uint64_t top_scratch = 0;
    if (!r.top.as && !make_as(r, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, bi, kMaxInstances, r.top, &top_scratch)) return;
    if (!(bi.scratchData.deviceAddress = scratch_for(r, top_scratch))) return;
    bi.dstAccelerationStructure = r.top.as;
    const VkAccelerationStructureBuildRangeInfoKHR range{n, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    r.build(cmd, 1, &bi, &ranges);
    note_work();
    full_barrier(cmd, "barrier (rt scene)");

    r.cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (++r.frames % 256 == 0) {   // (meshes not drawn for a while go; and a line of what there is)
        uint64_t triangles = 0, bytes = 0;
        for (auto it = r.meshes.begin(); it != r.meshes.end();) {
            if (now - it->second.used > 18000) {
                later(r, it->second.store, it->second.as);
                if (it->second.block >= 0) block_leave(r, it->second.block);
                it = r.meshes.erase(it);
            } else {
                triangles += it->second.triangles, bytes += it->second.size, ++it;
            }
        }
        logf("vk: rt: %u of %zu draws in the scene's structure; %zu meshes kept (%llu triangles, %llu MB), %llu built and %llu "
             "unreadable since the start, %zu waiting; %zu skinned draws (%llu triangles, %llu left out a frame); %.2f ms of CPU a frame and %.2f on its draws; builds: reads %.2f, conversion %.2f, structure %.2f, record %.2f ms a frame",
             n, noted, r.meshes.size(), (unsigned long long)triangles, (unsigned long long)(bytes >> 20),
             (unsigned long long)r.built, (unsigned long long)r.skipped, r.pending.size(), posed, (unsigned long long)r.skin_triangles, (unsigned long long)(r.skin_dropped / 256), r.cpu_ms / 256.0,
             (double)r.note_ns.exchange(0) / 256e6, (double)r.phase_ns[0].exchange(0) / 256e6, (double)r.phase_ns[1].exchange(0) / 256e6,
             (double)r.phase_ns[2].exchange(0) / 256e6, (double)r.phase_ns[3].exchange(0) / 256e6);
        r.cpu_ms = 0, r.skin_dropped = 0;
    }

    static const int debug = getenv("RX_VK_RT_DEBUG") ? atoi(getenv("RX_VK_RT_DEBUG")) : 0;
    if (dev.props.limits.maxPushConstantsSize < 224) return;
    if ((!debug && !(ao > 0.0f) && !(gi > 0.0f) && opts.sun <= 0) || !n || !make_pipeline(r) || !make_cache(r, cmd) ||
        !make_atlas(r, cmd))
        return;
    copy_materials(r, cmd);
    struct {
        float inv[16], vp[16];
        int32_t size[2];
        int32_t mode;
        float ao, radius, seed;
        int32_t rays;
        float gi, reach, skin;
        float sun, pad;   // (to_sun is a vec3 in the block: at a multiple of 16)
        float to_sun[3];
        float pad2;
        uint64_t skin_indices, skin_normals;
        float sun_share[3];
        float pad3;
    } pc{};
    static_assert(sizeof pc == 224, "rt_debug.comp's push constants");
    {   // (what a shadow takes: the sun over the sky's light, 3.5 whatever the hour (the sky dims with the sun: scaled by
        // the sun's brightness, as it was first, a sunset's shadows were faint beside the game's own), in the sun's own
        // colour: a warm low sun leaves bluer shadows. Not known: a little warm)
        const float* c = r.sun_colour;
        const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        for (int i = 0; i < 3; i++) pc.sun_share[i] = luma > 1e-4f ? std::min(3.5f * c[i] / luma, 8.0f) : (i == 0 ? 3.9f : i == 1 ? 3.5f : 2.9f);
    }
    pc.skin_indices = r.skin_indices, pc.skin_normals = r.skin_normals;
    if (now - r.sun_frame < 4) pc.sun = (float)opts.sun / 100.0f, std::memcpy(pc.to_sun, r.to_sun, sizeof pc.to_sun);
    r.traced_frame.store(now, std::memory_order_relaxed);
    static const float skin = getenv("RX_VK_RT_SKIN") ? (float)atof(getenv("RX_VK_RT_SKIN")) : 0.12f;
    pc.skin = skin;
    static const float reach = getenv("RX_VK_RT_REACH") ? (float)atof(getenv("RX_VK_RT_REACH")) : 12.0f;
    std::memcpy(pc.vp, view_proj, sizeof pc.vp);
    pc.gi = gi, pc.reach = reach;
    static const float radius = getenv("RX_VK_RT_RADIUS") ? (float)atof(getenv("RX_VK_RT_RADIUS")) : 1.5f;
    static const int rays = getenv("RX_VK_RT_RAYS") ? std::clamp(atoi(getenv("RX_VK_RT_RAYS")), 1, 16) : 4;
    static const int show_ao = getenv("RX_VK_AO_DEBUG") ? std::max(1, atoi(getenv("RX_VK_AO_DEBUG"))) : 0;
    pc.ao = ao, pc.radius = radius, pc.seed = seed, pc.rays = rays;
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!dlss_math::reprojection(view_proj, identity, pc.inv)) return;   // (identity * inverse(view_proj))
    pc.size[0] = (int32_t)w, pc.size[1] = (int32_t)h, pc.mode = debug ? debug : show_ao == 5 ? 8 : show_ao == 4 ? 7 : show_ao == 3 ? 6 : show_ao == 2 ? 5 : show_ao ? 4 : 3;
    VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    wa.accelerationStructureCount = 1, wa.pAccelerationStructures = &r.top.as;
    const VkDescriptorImageInfo info[3] = {{point, depth, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, color, VK_IMAGE_LAYOUT_GENERAL},
                                           {point, scene, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorImageInfo kept{VK_NULL_HANDLE, r.cache_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo colours{point, r.atlas_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[6]{};
    for (uint32_t i = 0; i < 6; i++) wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, wr[i].dstBinding = i, wr[i].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, wr[0].pNext = &wa;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[1].pImageInfo = &info[0];
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[2].pImageInfo = &info[1];
    wr[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[3].pImageInfo = &info[2];
    wr[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[4].pImageInfo = &kept;
    wr[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[5].pImageInfo = &colours;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.layout, 0, 6, wr);
    vkCmdPushConstants(cmd, r.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cmd, "barrier (rt check)");
}

} // namespace mkwvk
