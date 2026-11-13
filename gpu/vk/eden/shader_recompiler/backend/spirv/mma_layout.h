// (hookshot) The device's element order of the 16x8x16 f16 cooperative matrices HMMA_nx2 runs on.
//
// A lane's HMMA fragment registers hold its matrix elements in the console's order (PTX mma.m16n8k16's: two f16
// a register, low half first). The host's cooperative matrices hold each lane's share in an order of the
// implementation's own; vk_coopmat.cpp measures it at start, and only when every lane holds the same positions in
// both does the back end turn HMMA_nx2 on. Element e of the console's fragment is then the device's element
// mma_order_x[e].
#pragma once

#include <array>
#include <cstdint>

namespace Shader::Backend::SPIRV {

inline std::array<uint32_t, 8> mma_order_a{0, 1, 2, 3, 4, 5, 6, 7};
inline std::array<uint32_t, 4> mma_order_b{0, 1, 2, 3};
inline std::array<uint32_t, 4> mma_order_c{0, 1, 2, 3};
inline bool mma_software = false;   // (HMMA_nx2 in software: emit_spirv_warp.cpp, RX_VK_MMA_SOFTWARE=1)
// (amd-fix) float colour outputs carry no NaN or infinity to their target (EmitSetFragColor): one NaN texel spreads
// through bloom/blur/downsample averages into glowing blocks (OoT's flowers on AMD, Hookshot issue 606). On for every
// card since v2.0.6 (vk_coopmat.cpp coopmat_init); RX_VK_NAN_GUARD=0 turns it off. Idea from NX2navi patch 0027.
inline bool frag_nan_guard = false;
// (amd-fix) gl_Position decorated Invariant (vertex and mesh outputs): the game draws a depth pre-pass, then the colour
// pass again with depth EQUAL; Maxwell computes the same position in both, AMD's compiler may not without Invariant, and
// the colour pass is then rejected in spots: holes showing the bright sky, blown up by bloom (the glowing flowers).
// On for every card but NVIDIA (vk_coopmat.cpp); RX_VK_NO_INVARIANT=1 off. From NX2navi patch 0007.
inline bool position_invariant = false;
// (hookshot) An out-of-bounds texel fetch answers zero, as NVIDIA's GPUs answer it (measured on an RTX 3090), rather than
// whatever the host's does (EmitImageFetch). Vulkan leaves it undefined without robustImageAccess; on AMD the ground fog's
// off-screen depth reads drew a pale box at the screen's edges. Every card but NVIDIA (gpu/vk/vk_vendor.h); RX_VK_FETCH_BOUNDS
inline bool fetch_bounds = false;
// (hookshot) and texel buffers' fetches too: only when robustBufferAccess2 is off (the Debug tab's Robust buffers Off, RX_VK_ROBUST=0,
// or a driver without it), which otherwise answers their out-of-bounds reads with zero already (vk_coopmat.cpp)
inline bool fetch_bounds_buffers = false;
// (hookshot) isnan tested on the value's bits (IsNan in emit_spirv_floating_point.cpp), not OpIsNan, for AMD's older Windows
// shader compiler (RX 5000 and RX 6000: gpu/vk/vk_vendor.h); RX_VK_NAN_BITS=0 or 1
inline bool nan_bits = false;
// (hookshot) a storage buffer table's entries searched in a loop the driver is asked not to unroll (DefineGlobalMemoryFunctions),
// not one test an entry written out sixteen times. Mesa inlines every function, DontInline or not: OoT's foliage culling
// (53caac5f) has 46 global accesses, each with its ~70 table entries written out, over 100k instructions that Intel's
// compiler did not finish in minutes, and every compute pipeline queued behind it on Intel's one build thread waited:
// mesh-drawn rocks and doors white, the forest grey. In a loop: 12 s. On for Intel's Mesa driver (vk_coopmat.cpp); RX_VK_GM_LOOP=0 or 1
inline bool global_table_loop = false;

} // namespace Shader::Backend::SPIRV
