#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
// (hookshot) NVIDIA DLSS's actor motion (vk_dlss.cpp): a skinned draw of the scene drawn again, its vertices skinned with this
// frame's bones and with the frame before's, so each pixel it covers gets its own motion and not that of what is behind it.
// The cameras are in a storage buffer (data): this frame's view-projection as the game drew with it, the jitter in it; the frame
// before's with its jitter taken off (both row major, clip = M * position); the jitter; the render size. The vertices and
// indices are read where the game keeps them, the poses (bone palettes of 3 x 4 row-major matrices, origins) where vk_dlss.cpp
// copied them, by their device addresses. The game skins in model space and adds the object's origin:
// world = sum(w * B[j] * p) + origin.
layout(std430, binding = 0) readonly buffer Data { uint w[]; } data;
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words { uint w[]; };
layout(push_constant) uniform P {
    uint64_t vertices;   // (the vertex stream's address)
    uint64_t indices;    // (the index buffer's address, 4-aligned down; first: the draw's first index in it)
    uint64_t origins[2]; // (the draw's origin, xyz: this frame's and the frame before's)
    uint64_t bones[2];   // (its palettes: this frame's, the frame before's)
    uint nbones;
    uint stride;    // (in words)
    uint count;     // (vertices in the stream: an index past them draws nothing)
    uint first;
    uint itype;     // (0: 8-bit, 1: 16-bit, 2: 32-bit indices)
    int base_vertex;
    uint pos, sets;          // (byte offsets in a vertex, the joint and weight sets: 1 or 2)
    uint joints[2], weights[2];
    uint formats;            // (1: the position RGBA16F, else RGB32F; 2 << set: that set's joints RGBA8UI, else RGBA16UI;
                             // 8 << set: a single influence, an R8 weight and an R8UI (2 << set) or R16UI joint;
                             // 32 << set: two, RG8 weights and RG8UI joints; 128 << set: no weight, the joint weighs 1)
    uint tolerance;          // (the fragment stage's)
} pc;
layout(location = 0) out vec4 before;

float f(uint i) { return uintBitsToFloat(data.w[i]); }
vec4 f4(uint i) { return vec4(f(i), f(i + 1), f(i + 2), f(i + 3)); }
mat4 camera(uint at) {   // (row major in the buffer)
    return transpose(mat4(f4(at), f4(at + 4), f4(at + 8), f4(at + 12)));
}
vec4 row(Words m, uint i) { return uintBitsToFloat(uvec4(m.w[i], m.w[i + 1u], m.w[i + 2u], m.w[i + 3u])); }
void skin(Words palette, uvec4 j, vec4 wt, vec4 p, inout vec3 acc) {
    for (int k = 0; k < 4; k++) {
        if (wt[k] <= 0.0 || j[k] >= pc.nbones) continue;
        const uint b = j[k] * 12u;
        acc += wt[k] * vec3(dot(row(palette, b), p), dot(row(palette, b + 4u), p), dot(row(palette, b + 8u), p));
    }
}
uint byte_at(Words vb, uint o) { return (vb.w[o / 4u] >> ((o & 3u) * 8u)) & 0xffu; }
uint short_at(Words vb, uint o) {   // (2-aligned in practice; one that straddles a word is put together)
    return (o & 3u) == 3u ? (byte_at(vb, o) | (byte_at(vb, o + 1u) << 8)) : ((vb.w[o / 4u] >> ((o & 3u) * 8u)) & 0xffffu);
}
void main() {
    const uint at = pc.first + uint(gl_VertexIndex);
    Words ib = Words(pc.indices);
    uint index;
    if (pc.itype == 2u) index = ib.w[at];
    else if (pc.itype == 1u) index = (ib.w[at >> 1] >> ((at & 1u) * 16u)) & 0xffffu;
    else index = (ib.w[at >> 2] >> ((at & 3u) * 8u)) & 0xffu;
    const int vi = pc.base_vertex + int(index);
    if (vi < 0 || uint(vi) >= pc.count) {   // (outside the stream: nothing, its triangle dropped)
        gl_Position = vec4(0.0, 0.0, 0.0, -1.0);
        before = vec4(0.0);
        return;
    }
    Words vb = Words(pc.vertices + uint64_t(uint(vi)) * uint64_t(pc.stride) * 4ul);
    const uint q = pc.pos / 4u;
    const vec4 p = (pc.formats & 1u) != 0u ? vec4(unpackHalf2x16(vb.w[q]), unpackHalf2x16(vb.w[q + 1u]).x, 1.0)
                                           : vec4(uintBitsToFloat(vb.w[q]), uintBitsToFloat(vb.w[q + 1u]),
                                                  uintBitsToFloat(vb.w[q + 2u]), 1.0);
    vec3 now = vec3(0.0), then = vec3(0.0);
    for (uint s = 0u; s < pc.sets; s++) {
        // A set's influences at any byte offset: four (RGBA8 weights, RGBA8UI or RGBA16UI joints), two (kPair:
        // RG8 / RG8UI), one (kSingle: R8 / R8UI or R16UI), or one weighing 1 (kSingle | kNoWeight: a rigid model on one bone)
        const uint n = (pc.formats & (8u << s)) != 0u ? 1u : (pc.formats & (32u << s)) != 0u ? 2u : 4u;
        const bool byte_joints = (pc.formats & (2u << s)) != 0u, no_weight = (pc.formats & (128u << s)) != 0u;
        uvec4 j = uvec4(0u);
        vec4 wt = vec4(0.0);
        for (uint k = 0u; k < n; k++) {
            j[k] = byte_joints ? byte_at(vb, pc.joints[s] + k) : short_at(vb, pc.joints[s] + 2u * k);
            wt[k] = no_weight ? 1.0 : float(byte_at(vb, pc.weights[s] + k)) / 255.0;
        }
        skin(Words(pc.bones[0]), j, wt, p, now);
        skin(Words(pc.bones[1]), j, wt, p, then);
    }
    const vec3 o0 = row(Words(pc.origins[0]), 0u).xyz, o1 = row(Words(pc.origins[1]), 0u).xyz;
    const vec4 clip = camera(0u) * vec4(now + o0, 1.0);
    before = camera(16u) * vec4(then + o1, 1.0);
    // (the game's clip space: y up, depth -1 to 1; Vulkan's: y down, depth 0 to 1)
    gl_Position = vec4(clip.x, -clip.y, (clip.z + clip.w) * 0.5, clip.w);
}
