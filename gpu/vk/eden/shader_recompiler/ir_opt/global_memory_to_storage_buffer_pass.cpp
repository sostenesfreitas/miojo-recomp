// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/breadth_first_search.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif

namespace Shader::Optimization {

/// (EvilOOT) RX_VK_PTR_DIAG=1: how this thread's last translations' global memory accesses were bound, read and reset by
/// gpu/vk/vk_shader.cpp around each program (no header: declared there by hand, so Eden's ~500 files do not rebuild).
///   tracked   bound to a constant buffer pointer, the address made from constant buffer words only
///   fallback  not tracked: the run-time search (NVN slots, tables, the mesh pointer table), table = a table was found
///   mixed     bound to a constant buffer pointer although part of the address comes from memory (often only an index)
///   split     bound to a constant buffer pointer while a select or phi gives the address either from constant buffer
///             words or from memory (a pointer loaded through it): EvilOOT's near-leaf bug (zortos293/Lichii #410), the
///             loaded pointer's reads go to the wrong buffer
///   fallback_split  the same pattern among the fallbacks
struct PointerDiag {
    u32 tracked, fallback, fallback_table, mixed, split, fallback_split;
};
thread_local PointerDiag g_pointer_diag{};
// (collect_shader_info_pass.cpp: NVN2 compute's storage buffer slots for the program translated on this thread, 0: not compute)
extern thread_local u32 g_nvn2_compute_ssbo_base;
extern thread_local u32 g_nvn2_compute_ssbo_count;
extern thread_local bool g_all_global_this;   // (RX_VK_ALL_GLOBAL_PROGS, vk_shader.cpp: no access of this program bound)
extern thread_local bool g_gm_port_this;      // (EDEN_NX2_GM_PORT_PROGS, vk_shader.cpp: the port for this program)

namespace {
bool PointerDiagOn() {
    static const bool on = std::getenv("RX_VK_PTR_DIAG") != nullptr && std::atoi(std::getenv("RX_VK_PTR_DIAG")) != 0;
    return on;
}
/// (EvilOOT) a split address (see PointerDiag) is not bound to the constant buffer pointer on one side of its select or phi;
/// it is left to the run-time search, which finds whichever bound buffer (or table entry, or pointer table pool) the
/// address really falls in. #410's bug class (EvilOOT's near leaves): bound to one side, the other side's reads went to
/// the wrong buffer. On by default since the search covers whole pools (OoT's culling: characters, fallen leaves and
/// fireflies came back). RX_VK_PTR_SPLIT_FALLBACK=0: bound as before.
bool SplitFallbackOn() {
    static const bool on = !std::getenv("RX_VK_PTR_SPLIT_FALLBACK") || std::atoi(std::getenv("RX_VK_PTR_SPLIT_FALLBACK")) != 0;
    return on;
}
/// (hookshot) Switches for the analysis ported from Fritchii (Frizti/Fritchii), read once. EDEN_* variables are part of the
/// program cache's key (vk_shader.cpp disk_stamp), so a change translates the programs again.
///   EDEN_NX2_GM_LEGACY=0     the port (default 1 since v2.0.4: the tracking as before the port (NVN-biased, then unbiased search; no pairing, no native rules)
///   EDEN_NX2_GM_NATIVE=0     the pairing, but not the native-program rules (mapped pointers, candidates bound together)
///   EDEN_NX2_GM_COMPETING=0  a split access's tracked pointer is not bound (Lichii #410's adaptation off)
bool EnvSwitch(const char* name, bool fallback) {
    const char* const value{std::getenv(name)};
    return value && *value ? std::atoi(value) != 0 : fallback;
}
bool GmLegacy() {
    // (the tracking as before is the default again since v2.0.4: with the port on, v2.0.3 drew Link and the NPCs nearly black
    // and its lighting wrong (Discord, 11-08; an A/B in Link's house: his face lit with it off, a dark silhouette with it on),
    // and more draws cost fps. EDEN_NX2_GM_LEGACY=0: the port, for testing)
    static const bool on{EnvSwitch("EDEN_NX2_GM_LEGACY", true)};
    return on;
}
/// (hookshot) The tracking a stage gets. EDEN_NX2_GM_LEGACY=0/1: the port / the tracking as before, for every stage. Unset: the port
/// for the geometry and compute stages, the tracking as before for the fragment stage. The port placed the trees and terrain right
/// (v2.0.3: no floating trees, issue #729) and the dark characters were lighting, which the fragment stage reads (light probes).
/// EDEN_NX2_GM_SPLIT=1: that split; unset or 0: the tracking as before everywhere (v2.0.4/v2.0.5's default). Off by default again
/// (2026-11-08 test): with it the cold precompile of the 10340 compute programs took 650 s instead of ~130 s.
/// (floating trees, issue 994: the full split removed them but tore the cliffs) EDEN_NX2_GM_SPLIT=2: the port for the geometry
/// stages only; =3: for compute programs only. EDEN_NX2_GM_PORT_PROGS=<key prefix>[,...]: the port for those programs alone (the
/// bisection of which program places the trees)
bool GmLegacyFor(Stage stage) {
    if (g_gm_port_this) return false;
    static const char* const forced{std::getenv("EDEN_NX2_GM_LEGACY")};
    if (forced && *forced) return GmLegacy();
    static const int split{std::getenv("EDEN_NX2_GM_SPLIT") ? std::atoi(std::getenv("EDEN_NX2_GM_SPLIT")) : 0};
    switch (split) {
    case 1: return stage == Stage::Fragment;
    case 2: return stage == Stage::Fragment || stage == Stage::Compute;
    case 3: return stage != Stage::Compute;
    default: return true;
    }
}
bool GmNative() {
    static const bool on{EnvSwitch("EDEN_NX2_GM_NATIVE", true)};
    return on;
}
bool GmCompeting() {
    static const bool on{EnvSwitch("EDEN_NX2_GM_COMPETING", true)};
    return on;
}
/// Address in constant buffers to the storage buffer descriptor
struct StorageBufferAddr {
    auto operator<=>(const StorageBufferAddr&) const noexcept = default;

    u32 index;
    u32 offset;
};

/// Block iterator to a global memory instruction and the storage buffer it uses
struct StorageInst {
    StorageBufferAddr storage_buffer;
    IR::Inst* inst;
    IR::Block* block;
};

/// Bias towards a certain range of constant buffers when looking for storage buffers
struct Bias {
    u32 index;
    u32 offset_begin;
    u32 offset_end;
    u32 alignment;
};

using boost::container::flat_set;
using boost::container::small_vector;
using StorageBufferSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;
using StorageInstVector = small_vector<StorageInst, 24>;
using StorageWritesSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;

struct StorageInfo {
    StorageBufferSet set;
    StorageInstVector to_replace;
    StorageWritesSet writes;
    /// Constant buffer words that step strided iterators, see CollectIteratorWords
    StorageBufferSet iterator_words;
    /// Constant buffer pointers a global atomic uses as its address, see CollectAtomicPointers
    StorageBufferSet atomic_pointers;
    /// Track results by the instruction the search starts from, with and without the NVN bias:
    /// accesses through the same address share them, and a search can walk thousands of
    /// instructions (the IR does not change while storage buffers are collected).
    std::unordered_map<const IR::Inst*, std::optional<StorageBufferAddr>> biased_tracks;
    std::unordered_map<const IR::Inst*, std::optional<StorageBufferAddr>> unbiased_tracks;
    StorageBufferSet tables;   // (hookshot: TrackTable's, for the fallbacks)
    std::unordered_map<const IR::Inst*, small_vector<StorageBufferAddr, 4>> native_candidates;
    /// A native access with several candidate pointers was left global.
    bool native_runtime_bounds{};
    /// (hookshot) Fritchii's analysis (pointer pairing, iterator words, atomics' pointers): off, the old tracks alone
    bool pairing{true};
    /// (hookshot) Fritchii's native-program rules (Environment::IsNativeProgram): pointers must be mapped, candidates
    bool native{};
    /// (hookshot) Several candidates of an access, and the tracked side of a split one, are bound; cleared when they
    /// would leave too few bindings (Lichii #421: the first result kept instead of overflowing MAX_SSBOS)
    bool bind_candidates{true};
    /// (hookshot, Lichii #410) the tracked side of a split access is bound (see CollectStorageBuffers)
    bool bind_competing{true};
    bool competing_bound{};
};

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemory(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicCompareExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemoryWrite(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicCompareExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Converts a global memory opcode to its storage buffer equivalent
IR::Opcode GlobalToStorage(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadGlobalS8:
        return IR::Opcode::LoadStorageS8;
    case IR::Opcode::LoadGlobalU8:
        return IR::Opcode::LoadStorageU8;
    case IR::Opcode::LoadGlobalS16:
        return IR::Opcode::LoadStorageS16;
    case IR::Opcode::LoadGlobalU16:
        return IR::Opcode::LoadStorageU16;
    case IR::Opcode::LoadGlobal32:
        return IR::Opcode::LoadStorage32;
    case IR::Opcode::LoadGlobal64:
        return IR::Opcode::LoadStorage64;
    case IR::Opcode::LoadGlobal128:
        return IR::Opcode::LoadStorage128;
    case IR::Opcode::WriteGlobalS8:
        return IR::Opcode::WriteStorageS8;
    case IR::Opcode::WriteGlobalU8:
        return IR::Opcode::WriteStorageU8;
    case IR::Opcode::WriteGlobalS16:
        return IR::Opcode::WriteStorageS16;
    case IR::Opcode::WriteGlobalU16:
        return IR::Opcode::WriteStorageU16;
    case IR::Opcode::WriteGlobal32:
        return IR::Opcode::WriteStorage32;
    case IR::Opcode::WriteGlobal64:
        return IR::Opcode::WriteStorage64;
    case IR::Opcode::WriteGlobal128:
        return IR::Opcode::WriteStorage128;
    case IR::Opcode::GlobalAtomicIAdd32:
        return IR::Opcode::StorageAtomicIAdd32;
    case IR::Opcode::GlobalAtomicSMin32:
        return IR::Opcode::StorageAtomicSMin32;
    case IR::Opcode::GlobalAtomicUMin32:
        return IR::Opcode::StorageAtomicUMin32;
    case IR::Opcode::GlobalAtomicSMax32:
        return IR::Opcode::StorageAtomicSMax32;
    case IR::Opcode::GlobalAtomicUMax32:
        return IR::Opcode::StorageAtomicUMax32;
    case IR::Opcode::GlobalAtomicInc32:
        return IR::Opcode::StorageAtomicInc32;
    case IR::Opcode::GlobalAtomicDec32:
        return IR::Opcode::StorageAtomicDec32;
    case IR::Opcode::GlobalAtomicAnd32:
        return IR::Opcode::StorageAtomicAnd32;
    case IR::Opcode::GlobalAtomicOr32:
        return IR::Opcode::StorageAtomicOr32;
    case IR::Opcode::GlobalAtomicXor32:
        return IR::Opcode::StorageAtomicXor32;
    case IR::Opcode::GlobalAtomicExchange32:
        return IR::Opcode::StorageAtomicExchange32;
    case IR::Opcode::GlobalAtomicCompareExchange32:
        return IR::Opcode::StorageAtomicCompareExchange32;
    case IR::Opcode::GlobalAtomicIAdd64:
        return IR::Opcode::StorageAtomicIAdd64;
    case IR::Opcode::GlobalAtomicSMin64:
        return IR::Opcode::StorageAtomicSMin64;
    case IR::Opcode::GlobalAtomicUMin64:
        return IR::Opcode::StorageAtomicUMin64;
    case IR::Opcode::GlobalAtomicSMax64:
        return IR::Opcode::StorageAtomicSMax64;
    case IR::Opcode::GlobalAtomicUMax64:
        return IR::Opcode::StorageAtomicUMax64;
    case IR::Opcode::GlobalAtomicAnd64:
        return IR::Opcode::StorageAtomicAnd64;
    case IR::Opcode::GlobalAtomicOr64:
        return IR::Opcode::StorageAtomicOr64;
    case IR::Opcode::GlobalAtomicXor64:
        return IR::Opcode::StorageAtomicXor64;
    case IR::Opcode::GlobalAtomicExchange64:
        return IR::Opcode::StorageAtomicExchange64;
    case IR::Opcode::GlobalAtomicIAdd32x2:
        return IR::Opcode::StorageAtomicIAdd32x2;
    case IR::Opcode::GlobalAtomicSMin32x2:
        return IR::Opcode::StorageAtomicSMin32x2;
    case IR::Opcode::GlobalAtomicUMin32x2:
        return IR::Opcode::StorageAtomicUMin32x2;
    case IR::Opcode::GlobalAtomicSMax32x2:
        return IR::Opcode::StorageAtomicSMax32x2;
    case IR::Opcode::GlobalAtomicUMax32x2:
        return IR::Opcode::StorageAtomicUMax32x2;
    case IR::Opcode::GlobalAtomicAnd32x2:
        return IR::Opcode::StorageAtomicAnd32x2;
    case IR::Opcode::GlobalAtomicOr32x2:
        return IR::Opcode::StorageAtomicOr32x2;
    case IR::Opcode::GlobalAtomicXor32x2:
        return IR::Opcode::StorageAtomicXor32x2;
    case IR::Opcode::GlobalAtomicExchange32x2:
        return IR::Opcode::StorageAtomicExchange32x2;
    case IR::Opcode::GlobalAtomicAddF32:
        return IR::Opcode::StorageAtomicAddF32;
    case IR::Opcode::GlobalAtomicAddF16x2:
        return IR::Opcode::StorageAtomicAddF16x2;
    case IR::Opcode::GlobalAtomicMinF16x2:
        return IR::Opcode::StorageAtomicMinF16x2;
    case IR::Opcode::GlobalAtomicMaxF16x2:
        return IR::Opcode::StorageAtomicMaxF16x2;
    case IR::Opcode::GlobalAtomicAddF32x2:
        return IR::Opcode::StorageAtomicAddF32x2;
    case IR::Opcode::GlobalAtomicMinF32x2:
        return IR::Opcode::StorageAtomicMinF32x2;
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return IR::Opcode::StorageAtomicMaxF32x2;
    default:
        throw InvalidArgument("Invalid global memory opcode {}", opcode);
    }
}

/// Returns true when a storage buffer address satisfies a bias
bool MeetsBias(const StorageBufferAddr& storage_buffer, const Bias& bias) noexcept {
    return storage_buffer.index == bias.index && storage_buffer.offset >= bias.offset_begin &&
           storage_buffer.offset < bias.offset_end;
}

struct LowAddrInfo {
    IR::U32 value;
    IR::U32 high;
    s32 imm_offset;
};

/// Tries to track the first 32-bits of a global memory instruction
std::optional<LowAddrInfo> TrackLowAddress(IR::Inst* inst) {
    // The first argument is the low level GPU pointer to the global memory instruction
    const IR::Value addr{inst->Arg(0)};
    if (addr.IsImmediate()) {
        // Not much we can do if it's an immediate
        return std::nullopt;
    }
    // This address is expected to either be a PackUint2x32, a IAdd64, or a CompositeConstructU32x2
    IR::Inst* addr_inst{addr.InstRecursive()};
    s32 imm_offset{0};
    if (addr_inst->GetOpcode() == IR::Opcode::IAdd64) {
        // If it's an IAdd64, get the immediate offset it is applying and grab the address
        // instruction. This expects for the instruction to be canonicalized having the address on
        // the first argument and the immediate offset on the second one.
        const IR::U64 imm_offset_value{addr_inst->Arg(1)};
        if (!imm_offset_value.IsImmediate()) {
            return std::nullopt;
        }
        imm_offset = static_cast<s32>(static_cast<s64>(imm_offset_value.U64()));
        const IR::U64 iadd_addr{addr_inst->Arg(0)};
        if (iadd_addr.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = iadd_addr.InstRecursive();
    }
    // With IAdd64 handled, now PackUint2x32 is expected
    if (addr_inst->GetOpcode() == IR::Opcode::PackUint2x32) {
        // PackUint2x32 is expected to be generated from a vector
        const IR::Value vector{addr_inst->Arg(0)};
        if (vector.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = vector.InstRecursive();
    }
    // The vector is expected to be a CompositeConstructU32x2
    if (addr_inst->GetOpcode() != IR::Opcode::CompositeConstructU32x2) {
        return std::nullopt;
    }
    // Grab the first argument from the CompositeConstructU32x2, this is the low address.
    return LowAddrInfo{
        .value{IR::U32{addr_inst->Arg(0)}},
        .high{IR::U32{addr_inst->Arg(1)}},
        .imm_offset = imm_offset,
    };
}

/// Tries to track the storage buffer address used by a global memory instruction
std::optional<StorageBufferAddr> Track(const IR::Value& value, const Bias* bias, bool skip_c1 = false) {
    const auto pred{[bias, skip_c1](const IR::Inst* inst) -> std::optional<StorageBufferAddr> {
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 &&
            inst->GetOpcode() != IR::Opcode::GetCbufU32x2) {
            return std::nullopt;
        }
        const IR::Value index{inst->Arg(0)};
        const IR::Value offset{inst->Arg(1)};
        if (!index.IsImmediate()) {
            // Definitely not a storage buffer if it's read from a
            // non-immediate index
            return std::nullopt;
        }
        if (!offset.IsImmediate()) {
            // TODO: Support SSBO arrays
            return std::nullopt;
        }
        const StorageBufferAddr storage_buffer{
            .index = index.U32(),
            .offset = offset.U32(),
        };
        if (skip_c1 && storage_buffer.index == 1) {
            return std::nullopt;   // (an offset, not a base: the search goes on to the other addend)
        }
        const u32 alignment{bias ? bias->alignment : 8U};
        if (!Common::IsAligned(storage_buffer.offset, alignment)) {
            // The SSBO pointer has to be aligned
            return std::nullopt;
        }
        if (bias && !MeetsBias(storage_buffer, *bias)) {
            // We have to blacklist some addresses in case we wrongly
            // point to them
            return std::nullopt;
        }
        return storage_buffer;
    }};
    // (hookshot, from Lichii) A pointer loaded from memory is a new address, not an offset into the buffer that supplied it: its
    // load is not followed back to that buffer's own constant-buffer descriptor.
    const auto stop_at_memory = [](IR::Value operand) {
        if (!operand.IsImmediate() && IsGlobalMemory(*operand.InstRecursive())) {
            return IR::Value{u32{0}};
        }
        return operand;
    };
    return BreadthFirstSearch(value, pred, stop_at_memory);
}

/// (hookshot) A low address read from a storage buffer table: GetCbuf(index, x * 16 + base), x a run-time value
std::optional<StorageBufferAddr> TrackTable(const IR::Value& value) {
    const auto pred{[](const IR::Inst* inst) -> std::optional<StorageBufferAddr> {
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 && inst->GetOpcode() != IR::Opcode::GetCbufU32x2) {
            return std::nullopt;
        }
        const IR::Value index{inst->Arg(0)};
        const IR::Value offset{inst->Arg(1)};
        if (!index.IsImmediate() || offset.IsImmediate()) {
            return std::nullopt;
        }
        const IR::Inst* const add{offset.InstRecursive()};
        if (add->GetOpcode() != IR::Opcode::IAdd32) {
            return std::nullopt;
        }
        for (size_t side = 0; side < 2; ++side) {
            const IR::Value base{add->Arg(side)};
            const IR::Value scaled{add->Arg(1 - side)};
            if (!base.IsImmediate() || scaled.IsImmediate() || base.U32() % 16 != 0) {
                continue;
            }
            const IR::Inst* const s{scaled.InstRecursive()};
            const bool by16{s->Arg(1).IsImmediate() &&
                            ((s->GetOpcode() == IR::Opcode::ShiftLeftLogical32 && s->Arg(1).U32() == 4) ||
                             (s->GetOpcode() == IR::Opcode::IMul32 && s->Arg(1).U32() == 16))};
            if (by16) {
                return StorageBufferAddr{.index = index.U32(), .offset = base.U32()};
            }
        }
        return std::nullopt;
    }};
    return BreadthFirstSearch(value, pred);
}

/// Finds the one constant buffer word added directly into the low address, which is the base
/// pointer even when other constant buffer reads feed the computed offset
std::optional<StorageBufferAddr> TrackAddend(const IR::Value& value) {
    std::optional<StorageBufferAddr> result;
    bool ambiguous{};
    const auto visit{[&](const auto& self, const IR::Value& term) -> void {
        if (term.IsImmediate() || ambiguous) {
            return;
        }
        const IR::Inst* const inst{term.InstRecursive()};
        if (inst->GetOpcode() == IR::Opcode::IAdd32) {
            self(self, inst->Arg(0));
            self(self, inst->Arg(1));
            return;
        }
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 || !inst->Arg(0).IsImmediate() ||
            !inst->Arg(1).IsImmediate() || !Common::IsAligned(inst->Arg(1).U32(), 8)) {
            return;
        }
        const StorageBufferAddr found{.index = inst->Arg(0).U32(), .offset = inst->Arg(1).U32()};
        ambiguous = result.has_value();
        result = found;
    }};
    visit(visit, value);
    return ambiguous ? std::nullopt : result;
}

/// Returns the constant buffer word an instruction reads, including one half of a 64-bit read
std::optional<StorageBufferAddr> CbufWord(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::CompositeExtractU32x2 && !inst->Arg(0).IsImmediate()) {
        const IR::Inst* const vector{inst->Arg(0).InstRecursive()};
        if (vector->GetOpcode() != IR::Opcode::GetCbufU32x2 || !vector->Arg(0).IsImmediate() ||
            !vector->Arg(1).IsImmediate()) {
            return std::nullopt;
        }
        return StorageBufferAddr{
            .index = vector->Arg(0).U32(),
            .offset = vector->Arg(1).U32() + inst->Arg(1).U32() * 4,
        };
    }
    if (inst->GetOpcode() != IR::Opcode::GetCbufU32 || !inst->Arg(0).IsImmediate() ||
        !inst->Arg(1).IsImmediate()) {
        return std::nullopt;
    }
    return StorageBufferAddr{.index = inst->Arg(0).U32(), .offset = inst->Arg(1).U32()};
}

struct TrackingConditions {
    using Predicate = std::pair<IR::Value, bool>;

    struct Scope {
        Predicate condition;
        IR::Block* header;
        IR::Block* merge;
    };
    std::unordered_map<const IR::Inst*, IR::Block*> owners;
    std::unordered_map<IR::Block*, std::vector<Scope>> scopes;
    std::unordered_map<IR::Block*, std::vector<Scope>> merges;
    std::unordered_map<IR::Block*, IR::Block*> loops;
    IR::Block* current{};
    using BitKey = std::tuple<const IR::Inst*, u32, const IR::Inst*, int>;
    mutable std::map<BitKey, std::optional<Predicate>> bit_cache;

    explicit TrackingConditions(IR::Program& program) {
        for (IR::Block* block : program.post_order_blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::Phi) {
                    owners.emplace(&inst, block);
                }
            }
        }
        std::vector<Scope> active;
        std::vector<IR::Block*> loop_stack;
        IR::Block* previous{};
        for (const auto& node : program.syntax_list) {
            switch (node.type) {
            case IR::AbstractSyntaxNode::Type::Block:
                previous = node.data.block;
                scopes[previous] = active;
                loops[previous] = loop_stack.empty() ? nullptr : loop_stack.back();
                break;
            case IR::AbstractSyntaxNode::Type::If: {
                bit_cache.clear();
                const Scope scope{Literal(node.data.if_node.cond), previous,
                                  node.data.if_node.merge};
                merges[scope.merge].push_back(scope);
                active.push_back(scope);
                break;
            }
            case IR::AbstractSyntaxNode::Type::EndIf:
                active.pop_back();
                break;
            case IR::AbstractSyntaxNode::Type::Loop:
                loop_stack.push_back(node.data.loop.body);
                break;
            case IR::AbstractSyntaxNode::Type::Repeat:
                loop_stack.pop_back();
                break;
            default:
                break;
            }
        }
    }

    static std::optional<bool> Constant(Predicate value) {
        if (value.first.IsImmediate() && value.first.Type() == IR::Type::U1) {
            return value.first.U1() == value.second;
        }
        return std::nullopt;
    }

    std::optional<Predicate> Bit(IR::Value value, u32 bit, u32 depth,
                                 std::optional<Predicate> assumed = {}) const {
        if (value.IsImmediate())
            return BitImpl(value, bit, depth, assumed);
        const BitKey key{
            value.InstRecursive(), bit, assumed ? assumed->first.TryInstRecursive() : nullptr,
            assumed
                ? (Constant(*assumed).has_value() ? 4 + *Constant(*assumed) : 1 + assumed->second)
                : 0};
        if (const auto found = bit_cache.find(key); found != bit_cache.end())
            return found->second;
        bit_cache[key] = std::nullopt;
        const auto result = BitImpl(value, bit, depth, assumed);
        bit_cache[key] = result;
        return result;
    }

    std::optional<Predicate> BitImpl(IR::Value value, u32 bit, u32 depth,
                                     std::optional<Predicate> assumed) const {
        if (bit >= 32 || depth == 0) {
            return std::nullopt;
        }
        if (value.IsImmediate()) {
            return Predicate{IR::Value{((value.U32() >> bit) & 1) != 0}, true};
        }
        const auto* inst = value.InstRecursive();
        if (inst->GetOpcode() == IR::Opcode::Phi && inst->NumArgs() == 2) {
            const auto owner = owners.find(inst);
            if (owner != owners.end()) {
                const auto merge = merges.find(owner->second);
                if (merge != merges.end() && merge->second.size() == 1) {
                    const auto& branch = merge->second.front();
                    size_t bypass = inst->PhiBlock(0) == branch.header ? 0 : 1;
                    if (inst->PhiBlock(bypass) == branch.header &&
                        inst->PhiBlock(1 - bypass)->GetOrder() < owner->second->GetOrder()) {
                        if (assumed && assumed->first == branch.condition.first) {
                            const bool take_body = assumed->second == branch.condition.second;
                            return Bit(inst->Arg(take_body ? 1 - bypass : bypass), bit, depth - 1,
                                       assumed);
                        }
                        auto false_guard = branch.condition;
                        false_guard.second = !false_guard.second;
                        const auto no = Bit(inst->Arg(bypass), bit, depth - 1, false_guard);
                        const auto yes =
                            Bit(inst->Arg(1 - bypass), bit, depth - 1, branch.condition);
                        if (no && yes) {
                            if (*no == *yes)
                                return no;
                            const auto nc = Constant(*no);
                            const auto yc = Constant(*yes);
                            if (nc && yc) {
                                if (*nc == *yc)
                                    return Predicate{IR::Value{*yc}, true};
                                auto result = branch.condition;
                                result.second = result.second == *yc;
                                return result;
                            }
                        }
                    }
                }
            }
            return std::nullopt;
        }
        if (inst->GetOpcode() == IR::Opcode::SelectU32) {
            const auto yes = Bit(inst->Arg(1), bit, depth - 1, assumed);
            const auto no = Bit(inst->Arg(2), bit, depth - 1, assumed);
            if (yes && no) {
                if (*yes == *no)
                    return yes;
                const auto yc = Constant(*yes);
                const auto nc = Constant(*no);
                if (yc && nc) {
                    if (*nc == *yc)
                        return Predicate{IR::Value{*yc}, true};
                    auto cond = Literal(inst->Arg(0), depth - 1);
                    cond.second = cond.second == *yc;
                    return cond;
                }
            }
        }
        if (inst->GetOpcode() == IR::Opcode::BitwiseOr32 ||
            inst->GetOpcode() == IR::Opcode::BitwiseAnd32) {
            const auto lhs = Bit(inst->Arg(0), bit, depth - 1, assumed);
            const auto rhs = Bit(inst->Arg(1), bit, depth - 1, assumed);
            const bool is_or = inst->GetOpcode() == IR::Opcode::BitwiseOr32;
            if (lhs) {
                if (const auto c = Constant(*lhs))
                    return *c == is_or ? lhs : rhs;
            }
            if (rhs) {
                if (const auto c = Constant(*rhs))
                    return *c == is_or ? rhs : lhs;
            }
        }
        if (inst->GetOpcode() == IR::Opcode::BitFieldUExtract && inst->Arg(1).IsImmediate() &&
            inst->Arg(2).IsImmediate()) {
            if (bit >= inst->Arg(2).U32())
                return Predicate{IR::Value{false}, true};
            return Bit(inst->Arg(0), bit + inst->Arg(1).U32(), depth - 1, assumed);
        }
        return std::nullopt;
    }

    Predicate Literal(IR::Value value, u32 depth = 256) const {
        value = value.Resolve();
        if (value.IsImmediate() || depth == 0) {
            return {value, true};
        }
        const auto* inst = value.InstRecursive();
        switch (inst->GetOpcode()) {
        case IR::Opcode::ConditionRef:
            return Literal(inst->Arg(0), depth - 1);
        case IR::Opcode::LogicalNot: {
            auto result = Literal(inst->Arg(0), depth - 1);
            result.second = !result.second;
            return result;
        }
        case IR::Opcode::SelectU1: {
            if (const auto c = Constant(Literal(inst->Arg(0), depth - 1))) {
                return Literal(inst->Arg(*c ? 1 : 2), depth - 1);
            }
            break;
        }
        case IR::Opcode::IEqual:
        case IR::Opcode::INotEqual:
            if (!inst->Arg(1).IsImmediate() || inst->Arg(1).U32() != 0) {
                break;
            }
            [[fallthrough]];
        case IR::Opcode::GetZeroFromOp: {
            const bool is_zero = inst->GetOpcode() != IR::Opcode::INotEqual;
            const auto input = inst->Arg(0).Resolve();
            if (input.IsImmediate() && input.Type() == IR::Type::U32) {
                return {IR::Value{input.U32() == 0}, is_zero};
            }
            if (!input.IsImmediate()) {
                const auto* extract = input.InstRecursive();
                if (extract->GetOpcode() == IR::Opcode::BitwiseAnd32) {
                    for (size_t arg = 0; arg < 2; ++arg) {
                        const auto mask = extract->Arg(arg).Resolve();
                        if (mask.IsImmediate() && std::has_single_bit(mask.U32())) {
                            if (auto result = Bit(extract->Arg(1 - arg),
                                                  std::countr_zero(mask.U32()), depth - 1)) {
                                result->second = result->second != is_zero;
                                return *result;
                            }
                        }
                    }
                }
                if (extract->GetOpcode() == IR::Opcode::BitFieldUExtract &&
                    extract->Arg(2).IsImmediate() && extract->Arg(2).U32() == 1) {
                    if (auto result = Bit(input, 0, depth - 1)) {
                        result->second = result->second != is_zero;
                        return *result;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        return {IR::Value{const_cast<IR::Inst*>(inst)}, true};
    }

    bool IsBackEdge(const IR::Inst* phi, size_t arg) const {
        const auto owner = owners.find(phi);
        return owner != owners.end() && phi->PhiBlock(arg)->GetOrder() >= owner->second->GetOrder();
    }

    bool AllowsPhi(const IR::Inst* phi, size_t arg) const {
        const auto owner = owners.find(phi);
        if (owner == owners.end() || !loops.contains(owner->second) || !loops.contains(current) ||
            loops.at(owner->second) != loops.at(current)) {
            return true;
        }
        const auto merge = merges.find(owner->second);
        const auto current_scopes = scopes.find(current);
        if (merge == merges.end() || current_scopes == scopes.end()) {
            return true;
        }
        for (const auto& branch : merge->second) {
            const auto [condition, positive] = branch.condition;
            for (const auto& known : current_scopes->second) {
                const auto [known_condition, known_positive] = known.condition;
                if (condition != known_condition) {
                    continue;
                }
                const bool taken = positive == known_positive;
                const bool bypass = phi->PhiBlock(arg) == branch.header;
                if (bypass == taken) {
                    return false;
                }
            }
        }
        return true;
    }
};

/// Visits the constant buffer words summed into a value, nearest first, looking through phis
template <typename Func>
void ForEachSummedWord(const IR::Value& value, Func&& func,
                       const TrackingConditions* conditions = nullptr) {
    std::deque<std::tuple<IR::Value, u32, bool>> queue{{value, 0, true}};
    std::array<std::unordered_set<const IR::Inst*>, 2> visited;
    while (!queue.empty()) {
        const auto [term, depth, same_iteration] = queue.front();
        queue.pop_front();
        if (term.IsImmediate()) {
            continue;
        }
        const IR::Inst* const inst{term.InstRecursive()};
        if (!visited[same_iteration].insert(inst).second) {
            continue;
        }
        if (const auto word{CbufWord(inst)}) {
            func(*word, depth);
            continue;
        }
        if (inst->GetOpcode() == IR::Opcode::IAdd32 || inst->GetOpcode() == IR::Opcode::Phi ||
            inst->GetOpcode() == IR::Opcode::SelectU32) {
            for (size_t arg = inst->GetOpcode() == IR::Opcode::SelectU32 ? 1 : 0;
                 arg < inst->NumArgs(); ++arg) {
                if (inst->GetOpcode() == IR::Opcode::Phi && conditions && same_iteration &&
                    !conditions->AllowsPhi(inst, arg)) {
                    continue;
                }
                const bool next_iteration =
                    same_iteration && !(conditions && inst->GetOpcode() == IR::Opcode::Phi &&
                                        conditions->IsBackEdge(inst, arg));
                queue.emplace_back(inst->Arg(arg), depth + 1, next_iteration);
            }
        }
    }
}

struct WordSum {
    StorageBufferSet words;
    u32 count{};
    u32 budget{64};
    bool reaches_loop{};
};

/// Collects the constant buffer words of an addition tree and returns false when another value
/// feeds it. Reaching the phi given as the loop variable is recorded in the sum.
bool SumWords(const IR::Value& value, const IR::Inst* loop, WordSum& sum) {
    if (value.IsImmediate()) {
        return true;
    }
    if (sum.budget == 0) {
        return false;
    }
    --sum.budget;
    const IR::Inst* const inst{value.InstRecursive()};
    if (inst == loop) {
        sum.reaches_loop = true;
        return false;
    }
    if (const auto word{CbufWord(inst)}) {
        sum.words.insert(*word);
        ++sum.count;
        return true;
    }
    if (inst->GetOpcode() != IR::Opcode::IAdd32) {
        return false;
    }
    const bool lhs{SumWords(inst->Arg(0), loop, sum)};
    if (!lhs && !loop) {
        return false;
    }
    const bool rhs{SumWords(inst->Arg(1), loop, sum)};
    return lhs && rhs;
}

/// Finds the constant buffer words that step strided iterators (CUTLASS-style kernel parameters):
/// words added to a loop-carried value on its back edge, and words summed with two or more other
/// words into an offset table. A base pointer is neither.
StorageBufferSet CollectIteratorWords(IR::Program& program) {
    StorageBufferSet result;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Phi) {
                for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
                    WordSum sum;
                    SumWords(inst.Arg(arg), &inst, sum);
                    if (sum.reaches_loop) {
                        for (const StorageBufferAddr& word : sum.words) result.insert(word);   // (the compat flat_set has no range insert)
                    }
                }
            } else if (inst.GetOpcode() == IR::Opcode::IAdd32) {
                WordSum sum;
                if (SumWords(IR::Value{&inst}, nullptr, sum) && sum.words.size() >= 3) {
                    for (const StorageBufferAddr& word : sum.words) result.insert(word);   // (the compat flat_set has no range insert)
                }
            }
        }
    }
    std::unordered_map<const IR::Inst*, u32> indices;
    std::unordered_map<const IR::Inst*, u32> lowlinks;
    std::unordered_set<const IR::Inst*> on_stack;
    std::vector<const IR::Inst*> stack;
    u32 next_index{};
    const auto visit = [&](auto&& self, const IR::Inst* inst) -> void {
        if (indices.contains(inst)) {
            return;
        }
        const u32 index = next_index++;
        indices[inst] = lowlinks[inst] = index;
        stack.push_back(inst);
        on_stack.insert(inst);
        const auto op = inst->GetOpcode();
        if (op == IR::Opcode::IAdd32 || op == IR::Opcode::Phi || op == IR::Opcode::SelectU32) {
            for (size_t arg = op == IR::Opcode::SelectU32 ? 1 : 0; arg < inst->NumArgs(); ++arg) {
                if (inst->Arg(arg).IsImmediate()) {
                    continue;
                }
                const IR::Inst* next = inst->Arg(arg).InstRecursive();
                if (!indices.contains(next)) {
                    self(self, next);
                    lowlinks[inst] = std::min(lowlinks[inst], lowlinks[next]);
                } else if (on_stack.contains(next)) {
                    lowlinks[inst] = std::min(lowlinks[inst], indices[next]);
                }
            }
        }
        if (lowlinks[inst] != index) {
            return;
        }
        std::vector<const IR::Inst*> component;
        const IR::Inst* member;
        do {
            member = stack.back();
            stack.pop_back();
            on_stack.erase(member);
            component.push_back(member);
        } while (member != inst);
        if (component.size() < 2) {
            return;
        }
        const std::unordered_set<const IR::Inst*> members(component.begin(), component.end());
        for (const IR::Inst* add : component) {
            if (add->GetOpcode() != IR::Opcode::IAdd32) {
                continue;
            }
            for (size_t arg = 0; arg < add->NumArgs(); ++arg) {
                const auto& term = add->Arg(arg);
                if (!term.IsImmediate() && members.contains(term.InstRecursive())) {
                    continue;
                }
                ForEachSummedWord(term, [&](StorageBufferAddr word, u32) { result.insert(word); });
            }
        }
    };
    for (IR::Block* block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Phi) {
                visit(visit, &inst);
            }
        }
    }
    return result;
}

/// Returns true when a condition reads a constant buffer word, as the carry out of an addition to
/// that word does
bool ReadsCbufWord(const IR::Value& condition, StorageBufferAddr word) {
    std::deque<IR::Value> queue{condition};
    std::unordered_set<const IR::Inst*> visited;
    while (!queue.empty() && visited.size() < 64) {
        const IR::Value value{queue.front()};
        queue.pop_front();
        if (value.IsImmediate()) {
            continue;
        }
        const IR::Inst* const inst{value.InstRecursive()};
        if (!visited.insert(inst).second) {
            continue;
        }
        if (CbufWord(inst) == word) {
            return true;
        }
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            queue.push_back(inst->Arg(arg));
        }
    }
    return false;
}

/// Finds the 64-bit constant buffer pointers a global atomic's address is based on: the whole
/// address apart from an immediate offset, or a 64-bit sum whose high half adds the pointer's high
/// word and the carry out of its low word. An atomic has no global fallback, so such a pointer is a
/// buffer even while it is unmapped. A 32-bit scalar added to a pointer carries nothing.
StorageBufferSet CollectAtomicPointers(IR::Program& program) {
    const auto word = [](const IR::U32& value) {
        return value.IsImmediate() ? std::nullopt : CbufWord(value.InstRecursive());
    };
    StorageBufferSet result;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsGlobalMemoryWrite(inst) || inst.Type() == IR::Type::Void) {
                continue;
            }
            const std::optional<LowAddrInfo> addr{TrackLowAddress(&inst)};
            if (!addr) {
                continue;
            }
            const std::optional<StorageBufferAddr> low{word(addr->value)};
            if (low && Common::IsAligned(low->offset, 8) &&
                word(addr->high) ==
                    StorageBufferAddr{.index = low->index, .offset = low->offset + 4}) {
                result.insert(*low);
                continue;
            }
            StorageBufferSet high_words;
            small_vector<IR::Value, 2> carries;
            std::vector<IR::Value> pending{addr->high};
            std::unordered_set<const IR::Inst*> visited;
            while (!pending.empty()) {
                const IR::Value term{pending.back()};
                pending.pop_back();
                if (term.IsImmediate() || !visited.insert(term.InstRecursive()).second) {
                    continue;
                }
                const IR::Inst* const term_inst{term.InstRecursive()};
                if (const auto high_word{CbufWord(term_inst)}) {
                    high_words.insert(*high_word);
                    continue;
                }
                switch (term_inst->GetOpcode()) {
                case IR::Opcode::IAdd32:
                case IR::Opcode::Phi:
                    for (size_t arg = 0; arg < term_inst->NumArgs(); ++arg) {
                        pending.push_back(term_inst->Arg(arg));
                    }
                    break;
                case IR::Opcode::SelectU32:
                    if (term_inst->Arg(1).IsImmediate() && term_inst->Arg(2).IsImmediate()) {
                        carries.push_back(term_inst->Arg(0));
                    }
                    break;
                default:
                    break;
                }
            }
            if (carries.empty()) {
                continue;
            }
            ForEachSummedWord(addr->value, [&](StorageBufferAddr pointer, u32) {
                if (Common::IsAligned(pointer.offset, 8) &&
                    high_words.contains({.index = pointer.index, .offset = pointer.offset + 4}) &&
                    std::ranges::any_of(carries, [&](const IR::Value& carry) {
                        return ReadsCbufWord(carry, pointer);
                    })) {
                    result.insert(pointer);
                }
            });
        }
    }
    return result;
}

/// Finds the 64-bit constant buffer pointer summed into an address: its low word is summed into
/// the low half and its high word into the high half. Iterator steps and 32-bit scalars such as
/// strides and sizes are skipped. Competing bases require the legacy nearest-word fallback.
std::optional<StorageBufferAddr> TrackPointerAddend(const LowAddrInfo& addr,
                                                    const StorageBufferSet& iterator_words,
                                                    const TrackingConditions& conditions,
                                                    bool legacy_nearest = false) {
    StorageBufferSet high_words;
    ForEachSummedWord(
        addr.high, [&](StorageBufferAddr word, u32) { high_words.insert(word); }, &conditions);
    std::optional<StorageBufferAddr> result;
    u32 result_depth{};
    bool tied{};
    StorageBufferSet candidates;
    ForEachSummedWord(
        addr.value,
        [&](StorageBufferAddr word, u32 depth) {
            if (!Common::IsAligned(word.offset, 8) || iterator_words.contains(word) ||
                !high_words.contains({.index = word.index, .offset = word.offset + 4})) {
                return;
            }
            candidates.insert(word);
            if (!result || depth < result_depth) {
                result = word;
                result_depth = depth;
                tied = false;
            } else if (depth == result_depth && *result != word) {
                tied = true;
            }
        },
        &conditions);
    return tied || (!legacy_nearest && candidates.size() != 1) ? std::nullopt : result;
}

/// Native programs: every constant buffer pointer the low address can come from, following only
/// additions, selects and phis. A predicated address assignment becomes a phi whose other input
/// can point into a different buffer, so the search does not stop at the first pointer.
small_vector<StorageBufferAddr, 4> NativePointerCandidates(const IR::U32& low_addr,
                                                           Environment& env) {
    small_vector<StorageBufferAddr, 4> candidates;
    small_vector<const IR::Inst*, 16> pending{low_addr.InstRecursive()};
    std::unordered_set<const IR::Inst*> visited{pending.front()};
    const auto enqueue = [&](const IR::Value& value) {
        if (!value.IsImmediate() && visited.insert(value.InstRecursive()).second) {
            pending.push_back(value.InstRecursive());
        }
    };
    for (size_t next = 0; next < pending.size(); ++next) {
        const IR::Inst* const inst{pending[next]};
        switch (inst->GetOpcode()) {
        case IR::Opcode::GetCbufU32:
        case IR::Opcode::GetCbufU32x2: {
            const IR::Value index{inst->Arg(0)};
            const IR::Value offset{inst->Arg(1)};
            if (!index.IsImmediate() || !offset.IsImmediate()) {
                break;
            }
            const StorageBufferAddr pointer{.index = index.U32(), .offset = offset.U32()};
            if (Common::IsAligned(pointer.offset, 8) &&
                std::ranges::find(candidates, pointer) == candidates.end() &&
                env.IsCbufStorageBufferPointer(pointer.index, pointer.offset)) {
                candidates.push_back(pointer);
            }
            break;
        }
        case IR::Opcode::IAdd32:
        case IR::Opcode::Phi:
            for (size_t arg = inst->NumArgs(); arg--;) {
                enqueue(inst->Arg(arg));
            }
            break;
        case IR::Opcode::SelectU32:
            enqueue(inst->Arg(1));
            enqueue(inst->Arg(2));
            break;
        case IR::Opcode::CompositeExtractU32x2:
            if (inst->Arg(1).IsImmediate() && inst->Arg(1).U32() == 0) {
                enqueue(inst->Arg(0));
            }
            break;
        default:
            break;
        }
    }
    return candidates;
}

/// (EvilOOT, RX_VK_PTR_DIAG) Where an address comes from: constant buffer words, memory (a global load: a pointer loaded
/// from memory), and whether a select or phi joins values on the way. The walk stops at both kinds of source.
/// split: a select or phi with a side from constant buffer words only and a side from memory (#410's pattern: the
/// pointer is c0[x] or *c0[x], and only c0[x] gets bound). An index loaded from memory into a constant buffer
/// pointer's offset is not split.
struct AddrOrigins {
    bool cbuf{}, load{}, branch{}, split{};
};
/// (11-07) One walk for the whole program, each instruction's origins kept: the first version walked up to 1024
/// instructions per address and walked each side of every select or phi again, nested three deep, which on a compute
/// shader with many loop phis ran for minutes on the submit's thread (the freeze after Load, vk_hangdump.cpp found it).
/// Origins of an instruction = the union of its arguments' (a select's condition left out); split where a select or
/// phi joins a side from constant buffer words only with a side from memory, or below. A phi's argument still being
/// walked (a loop) adds nothing.
thread_local std::unordered_map<const IR::Inst*, AddrOrigins> g_origins;
AddrOrigins Origins(const IR::Value& root) {
    if (root.IsImmediate()) {
        return {};
    }
    const IR::Inst* const top{root.InstRecursive()};
    if (const auto it{g_origins.find(top)}; it != g_origins.end()) {
        return it->second;
    }
    const auto is_cbuf{[](IR::Opcode op) { return op == IR::Opcode::GetCbufU32 || op == IR::Opcode::GetCbufU32x2; }};
    const auto first_arg{[](IR::Opcode op) -> size_t {
        return op == IR::Opcode::SelectU32 || op == IR::Opcode::SelectU64 ? 1 : 0;   // (not the condition)
    }};
    struct Frame {
        const IR::Inst* inst;
        size_t next;
    };
    std::vector<Frame> stack{{top, first_arg(top->GetOpcode())}};
    std::unordered_set<const IR::Inst*> walking{top};
    while (!stack.empty()) {
        const IR::Inst* const inst{stack.back().inst};
        const IR::Opcode op{inst->GetOpcode()};
        const bool source{is_cbuf(op) || IsGlobalMemory(*inst)};
        const IR::Inst* child{};
        while (!source && !child && stack.back().next < inst->NumArgs()) {
            const IR::Value arg{inst->Arg(stack.back().next++)};
            if (arg.IsImmediate()) {
                continue;
            }
            const IR::Inst* const a{arg.InstRecursive()};
            if (!g_origins.contains(a) && !walking.contains(a)) {
                child = a;
            }
        }
        if (child) {
            stack.push_back({child, first_arg(child->GetOpcode())});
            walking.insert(child);
            continue;
        }
        AddrOrigins o;
        if (is_cbuf(op)) {
            o.cbuf = true;
        } else if (IsGlobalMemory(*inst)) {
            o.load = true;
        } else {
            const size_t first{first_arg(op)};
            const bool joins{op == IR::Opcode::Phi || first == 1};
            bool cbuf_only{}, from_memory{};
            o.branch = joins;
            for (size_t i = first; i < inst->NumArgs(); ++i) {
                const IR::Value arg{inst->Arg(i)};
                if (arg.IsImmediate()) {
                    continue;
                }
                const auto it{g_origins.find(arg.InstRecursive())};
                if (it == g_origins.end()) {
                    continue;   // (a loop's phi, still being walked)
                }
                const AddrOrigins& side{it->second};
                o.cbuf |= side.cbuf, o.load |= side.load, o.branch |= side.branch, o.split |= side.split;
                cbuf_only |= side.cbuf && !side.load;
                from_memory |= side.load;
            }
            o.split |= joins && cbuf_only && from_memory;
        }
        g_origins.emplace(inst, o);
        walking.erase(inst);
        stack.pop_back();
    }
    return g_origins[top];
}
/// (hookshot, from Lichii b8d81e20) Whether an address is a select or phi of which one input is a pointer loaded from memory: Ocarina's foliage
/// placement records are read from c0[12] or from *c0[12]. Tracked to the constant buffer pointer alone, the loaded path read out of bounds
/// as zeros, the near-leaf pass wrote NaN positions, and trees had no leaves. Such a load stays global: the backend's page table reads it
bool MixesLoadedPointer(const IR::Value& value) {
    if (value.IsImmediate()) {
        return false;
    }
    std::vector<const IR::Inst*> todo{value.InstRecursive()};
    std::unordered_set<const IR::Inst*> seen;
    bool merge{}, loaded{};
    while (!todo.empty() && seen.size() < 96) {
        const IR::Inst* const inst{todo.back()};
        todo.pop_back();
        if (!seen.insert(inst).second) {
            continue;
        }
        switch (inst->GetOpcode()) {
        case IR::Opcode::Phi:
        case IR::Opcode::SelectU32:
        case IR::Opcode::SelectU64:
            merge = true;
            break;
        case IR::Opcode::LoadStorage32:
        case IR::Opcode::LoadStorage64:
        case IR::Opcode::LoadStorage128:
        case IR::Opcode::LoadGlobal32:
        case IR::Opcode::LoadGlobal64:
        case IR::Opcode::LoadGlobal128:
            loaded = true;
            continue;   // (what it loads from is another matter)
        default:
            break;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            const IR::Value arg{inst->Arg(i)};
            if (!arg.IsImmediate()) {
                todo.push_back(arg.InstRecursive());
            }
        }
    }
    return merge && loaded;
}

thread_local bool g_follow_stage = false;   // (hookshot: the program being translated is compute, where the page table is)
thread_local bool g_graphics_stage = false;   // (sharkiix: the program being translated is a geometry stage: vertex, tessellation, geometry, task, mesh)

/// Collects the storage buffer used by a global memory instruction and the instruction itself
void CollectStorageBuffers(IR::Block& block, IR::Inst& inst, StorageInfo& info, Environment& env,
                           const TrackingConditions* conditions_or_null) {
    if (g_all_global_this) {
        return;   // (EvilOOT, a test: every access left global, found by its address at run time: search, then the page table)
    }
    // NVN puts storage buffers in a specific range, we have to bias towards these addresses to
    // avoid getting false positives
    static constexpr Bias nvn_bias{
        .index = 0,
        .offset_begin = 0x100,
        .offset_end = 0x700,
        .alignment = 16,
    };
    // (EvilOOT, RX_VK_PTR_DIAG) counted on every way out of here
    const bool diag{PointerDiagOn()};
    const bool split_fallback{SplitFallbackOn()};
    const AddrOrigins origins{diag || split_fallback ? Origins(inst.Arg(0)) : AddrOrigins{}};
    const auto count_fallback{[&](bool table) {
        if (diag) {
            ++g_pointer_diag.fallback;
            g_pointer_diag.fallback_table += table ? 1 : 0;
            g_pointer_diag.fallback_split += origins.split ? 1 : 0;
        }
    }};
    // Track the low address of the instruction
    const std::optional<LowAddrInfo> low_addr_info{TrackLowAddress(&inst)};
    if (!low_addr_info) {
        // Failed to track the low address, use NVN fallbacks
        count_fallback(false);
        return;
    }
    // (EvilOOT) NVN2 compute: its own storage buffer slots are the bias (collect_shader_info_pass.cpp
    // g_nvn2_compute_ssbo_base), not Maxwell's 0x100..0x700, which never matched: every compute access went to the
    // unbiased track, which took the first constant buffer word it met
    const Bias compute_bias{
        .index = 0,
        .offset_begin = g_nvn2_compute_ssbo_base,
        .offset_end = g_nvn2_compute_ssbo_base + 16 * g_nvn2_compute_ssbo_count,
        .alignment = 16,
    };
    const Bias* const bias{g_nvn2_compute_ssbo_base ? &compute_bias : &nvn_bias};
    {
        // (only where the page table is: compute. A graphics stage reads such a load as 0, and that blanked every menu text)
        static const bool follow{!env_on("RX_VK_NO_FOLLOW")};
        const IR::Opcode op{inst.GetOpcode()};
        // (and the writes and 32-bit atomics through such an address: the culling's outputs and counters, which the backend's page
        // table now reaches too. Tracked to the constant buffer alone they went to a buffer at address 0, and every count stayed 0)
        const bool load{op == IR::Opcode::LoadGlobal32 || op == IR::Opcode::LoadGlobal64 || op == IR::Opcode::LoadGlobal128};
        const bool store{op == IR::Opcode::WriteGlobal32 || op == IR::Opcode::WriteGlobal64 || op == IR::Opcode::WriteGlobal128 ||
                         op == IR::Opcode::GlobalAtomicIAdd32 || op == IR::Opcode::GlobalAtomicUMax32 || op == IR::Opcode::GlobalAtomicUMin32 ||
                         op == IR::Opcode::GlobalAtomicSMax32 || op == IR::Opcode::GlobalAtomicSMin32 || op == IR::Opcode::GlobalAtomicAnd32 ||
                         op == IR::Opcode::GlobalAtomicOr32 || op == IR::Opcode::GlobalAtomicXor32 || op == IR::Opcode::GlobalAtomicExchange32 ||
                         op == IR::Opcode::GlobalAtomicCompareExchange32};
        // (RX_VK_FOLLOW_STORES=1: the writes too. Off: they made the culling count but drew no leaves, and a GPU faulted in it, #132)
        static const bool no_store{!env_on("RX_VK_FOLLOW_STORES")};
        if (follow && g_follow_stage && (load || (store && !no_store)) && MixesLoadedPointer(low_addr_info->value)) {
            return;   // (left global: MixesLoadedPointer)
        }
    }
    const IR::U32 low_addr{low_addr_info->value};
    // (hookshot) Left global: found by its address at run time (the bound buffers, the program's tables, the page table).
    // The table the address is read from, if it is, is kept for that search, as on every way to the run-time search.
    const auto leave_global{[&] {
        bool table_found{};
        if (!low_addr.IsImmediate()) {
            if (const std::optional<StorageBufferAddr> table{TrackTable(low_addr)}) {
                info.tables.insert(*table);
                table_found = true;
            }
        }
        count_fallback(table_found);
    }};
    // (EvilOOT) c1 is the program's own constants: no buffer's address is known when a program is built, so a word of
    // c1 in the address is an offset (OoT's culling passes: a pointer loaded from memory + c1[0] = 12, taken for the
    // base: their results went to a buffer at address 12). Left to the run-time search, which finds the real one.
    const auto not_c1{[](std::optional<StorageBufferAddr> found) {
        return found && found->index == 1 ? std::nullopt : found;
    }};
    const auto cached_track = [&](const Bias* bias, auto& cache) {
        if (low_addr.IsImmediate()) {
            return Track(low_addr, bias);
        }
        const IR::Inst* const start{low_addr.InstRecursive()};
        if (const auto it = cache.find(start); it != cache.end()) {
            return it->second;
        }
        const std::optional<StorageBufferAddr> result{Track(low_addr, bias)};
        cache.emplace(start, result);
        return result;
    };
    std::optional<StorageBufferAddr> storage_buffer;
    if (info.pairing && conditions_or_null) {
        const TrackingConditions& conditions{*conditions_or_null};
        // (Fritchii) The 64-bit constant buffer pointer whose low word is summed into the address's low half and whose
        // high word into its high half. Hookshot's mesh programs had a texture handle and a float (c5+0x60) taken for
        // their base by the searches below, which read only the low half: the access read zeros (Death Mountain's
        // terrain under its trees missing: the floating trees).
        const std::optional<StorageBufferAddr> paired_pointer{
            not_c1(TrackPointerAddend(*low_addr_info, info.iterator_words, conditions))};
        storage_buffer = paired_pointer;
        if (info.native && storage_buffer &&
            !env.IsCbufStorageBufferPointer(storage_buffer->index, storage_buffer->offset)) {
            storage_buffer.reset();
        }
        if (info.native && !storage_buffer && !low_addr.IsImmediate()) {
            const IR::Inst* const start{low_addr.InstRecursive()};
            auto it{info.native_candidates.find(start)};
            if (it == info.native_candidates.end()) {
                it = info.native_candidates.emplace(start, NativePointerCandidates(low_addr, env)).first;
            }
            const auto& candidates{it->second};
            if (candidates.size() > 1) {
                // Bind every candidate and keep the access global; the backend picks the buffer at
                // run time. Rebasing on one of them drops the other path's accesses.
                // (hookshot: unless they would take too many bindings, StorageInfo::bind_candidates: then the run-time
                // search alone)
                if (info.bind_candidates) {
                    for (const StorageBufferAddr& candidate : candidates) {
                        info.set.insert(candidate);
                        if (IsGlobalMemoryWrite(inst)) {
                            info.writes.insert(candidate);
                        }
                    }
                    info.native_runtime_bounds = true;
                }
                leave_global();
                return;
            }
            if (candidates.size() == 1) {
                // The one pointer the address is built from. The NVN search below only takes 16-byte
                // aligned slots, so for a pointer at an 8-byte slot it can return a dimension or stride
                // the address arithmetic reads instead, and the access then lands in the wrong buffer.
                storage_buffer = candidates.front();
            }
        }
        if (info.native && !storage_buffer) {
            // A pointer that is an atomic's address keeps its buffer while unmapped, for the atomic
            // and every access based on it: Bananza's append counters and the records stored after
            // them were unmapped when their pipelines were first built. The binding reads the pointer
            // again for every dispatch.
            if (!paired_pointer || !info.atomic_pointers.contains(*paired_pointer)) {
                leave_global();
                return;
            }
            storage_buffer = paired_pointer;
        }
        if (!storage_buffer) {
            storage_buffer = not_c1(TrackPointerAddend(*low_addr_info, info.iterator_words, conditions, true));
        }
        if (!storage_buffer) {
            storage_buffer = not_c1(TrackAddend(low_addr));
        }
    }
    // First try to find storage buffers in the NVN address
    if (!storage_buffer) {
        storage_buffer = cached_track(bias, info.biased_tracks);
    }
    if (!storage_buffer) {
        // If it fails, track without a bias
        storage_buffer = not_c1(cached_track(nullptr, info.unbiased_tracks));
        // (hookshot) A float add has no run-time search: its fallback stores nothing, so where the search above found only a c1
        // word the sum was lost. OoT's 499dbfd0721d2858 adds the sky's spherical harmonics (the outdoor ambient light) at
        // c0+0x20's buffer plus a c1 offset: the c0 pointer beside that offset is the base. Without it the scene lost its
        // ambient light and the exposure lifted the dark night until it looked like day. EDEN_F32_ADD_TRACK=0: as before
        static const bool f32_track{EnvSwitch("EDEN_F32_ADD_TRACK", true)};
        if (!storage_buffer && f32_track && inst.GetOpcode() == IR::Opcode::GlobalAtomicAddF32) {
            storage_buffer = Track(low_addr, nullptr, true);
        }
        if (!storage_buffer) {
            // If that also fails, use NVN fallbacks (and the table it reads the address from, if it does)
            leave_global();
            LOG_WARNING(Shader, "Storage buffer failed to track, using global memory fallbacks");
            return;
        }
        LOG_DEBUG(Shader, "Storage buffer tracked without bias, index {} offset {}",
                    storage_buffer->index, storage_buffer->offset);
    }
    {   // (sharkiix, from BernabeCR93 in upstream issue 667) A graphics stage's address tracked to another constant buffer than
        // c0 while part of it comes from memory: the far-foliage and mesh programs (95db514a35353e28, d905f52b7517c681,
        // 51183fe750d5d0c8...) took float constants for a buffer address ("storage buffer NOT bound (reads zeros)", c5+0x60),
        // read zeros and drew the plants without colour (white/sky-blue "mirror" ferns, glowing flowers). Left to the run-time
        // search. RX_VK_NO_CBUF_LOAD_RULE=1: off
        static const bool cbuf_load_rule{!env_on("RX_VK_NO_CBUF_LOAD_RULE")};
        if (cbuf_load_rule && storage_buffer->index != 0 && g_graphics_stage && Origins(inst.Arg(0)).load) {
            leave_global();
            return;
        }
    }
    if (split_fallback && origins.split) {
        // (Lichii #410, Duck912, adapted) The tracked pointer is one side of a select or phi whose other side is a pointer
        // loaded from memory: bound too, so that the run-time search finds the accesses through it in every stage (the
        // loaded side's are the page table's, where the stage has one). OoT's near-leaf pass reads its placement records
        // from c0[12] or from *c0[12]. EDEN_NX2_GM_COMPETING=0: left to the search unbound, as before
        if (info.bind_candidates && info.bind_competing) {
            info.set.insert(*storage_buffer);
            if (IsGlobalMemoryWrite(inst)) {
                info.writes.insert(*storage_buffer);
            }
            info.native_runtime_bounds |= info.native;
            info.competing_bound = true;
        }
        count_fallback(false);   // (counted as a run-time search, split)
        return;
    }
    if (diag) {
        ++g_pointer_diag.tracked;
        g_pointer_diag.mixed += origins.load ? 1 : 0;
        g_pointer_diag.split += origins.split ? 1 : 0;
    }
    // Collect storage buffer and the instruction
    if (IsGlobalMemoryWrite(inst)) {
        info.writes.insert(*storage_buffer);
    }
    info.set.insert(*storage_buffer);
    info.to_replace.push_back(StorageInst{
        .storage_buffer{*storage_buffer},
        .inst = &inst,
        .block = &block,
    });
}

/// Returns the offset in indices (not bytes) for an equivalent storage instruction
IR::U32 StorageOffset(IR::Block& block, IR::Inst& inst, StorageBufferAddr buffer, u32 alignment) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    IR::U32 offset;
    if (const std::optional<LowAddrInfo> low_addr{TrackLowAddress(&inst)}) {
        offset = low_addr->value;
        if (low_addr->imm_offset != 0) {
            offset = ir.IAdd(offset, ir.Imm32(low_addr->imm_offset));
        }
    } else {
        offset = ir.UConvert(32, IR::U64{inst.Arg(0)});
    }
    // Subtract the least significant 32 bits from the guest offset. The result is the storage
    // buffer offset in bytes.
    IR::U32 low_cbuf{ir.GetCbuf(ir.Imm32(buffer.index), ir.Imm32(buffer.offset))};

    // Align the offset base to match the host alignment requirements
    low_cbuf = ir.BitwiseAnd(low_cbuf, ir.Imm32(~(alignment - 1U)));
    return ir.ISub(offset, low_cbuf);
}

/// Replace a global memory load instruction with its storage buffer equivalent
void ReplaceLoad(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                 const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{
        &*block.PrependNewInst(it, new_opcode, {storage_index, offset})};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory write instruction with its storage buffer equivalent
void ReplaceWrite(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                  const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)});
    inst.Invalidate();
}

/// Replace an atomic operation on global memory instruction with its storage buffer equivalent
void ReplaceAtomic(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                   const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{inst.NumArgs() > 2
        ? &*block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1), inst.Arg(2)})   // (compare-exchange)
        : &*block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)})};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory instruction with its storage buffer equivalent
void Replace(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
             const IR::U32& offset) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
        return ReplaceLoad(block, inst, storage_index, offset);
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
        return ReplaceWrite(block, inst, storage_index, offset);
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicCompareExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return ReplaceAtomic(block, inst, storage_index, offset);
    default:
        throw InvalidArgument("Invalid global memory opcode {}", inst.GetOpcode());
    }
}
} // Anonymous namespace

void GlobalMemoryToStorageBufferPass(Environment& env, IR::Program& program,
                                     const HostTranslateInfo& host_info) {
    const bool has_global_memory =
        std::ranges::any_of(program.post_order_blocks, [](const auto* block) {
            return std::ranges::any_of(block->Instructions(), IsGlobalMemory);
        });
    if (!has_global_memory) {
        return;
    }
    // (hookshot: compute only; the task and mesh stages were tried for the foliage, which it did not bring back)
    g_follow_stage = program.stage == Stage::Compute;
    // (geometry stages only: with the fragment stage too, the sky drew black rectangles and the lighting went too bright at the castle)
    g_graphics_stage = program.stage != Stage::Compute && program.stage != Stage::Fragment;
    g_origins.clear();   // (EvilOOT: Origins' walk, kept per program)
    const bool pairing{!GmLegacyFor(program.stage)};
    std::optional<TrackingConditions> conditions;
    StorageInfo info;
    info.pairing = pairing;
    // (hookshot: every NVN2 program Hookshot translates is native, Env::IsNativeProgram)
    info.native = pairing && GmNative() && env.IsNativeProgram();
    // (Lichii #410: the global memory functions that pick a buffer at run time need 64-bit integers)
    info.bind_competing = GmCompeting() && host_info.support_int64;
    info.bind_candidates = host_info.support_int64;
    if (pairing) {
        conditions.emplace(program);
        info.iterator_words = CollectIteratorWords(program);
        info.atomic_pointers = CollectAtomicPointers(program);
    }
    const PointerDiag diag_before{g_pointer_diag};
    const auto collect{[&] {
        for (IR::Block* const block : program.post_order_blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (!IsGlobalMemory(inst)) {
                    continue;
                }
                if (conditions) {
                    conditions->current = block;
                }
                CollectStorageBuffers(*block, inst, info, env, conditions ? &*conditions : nullptr);
            }
        }
    }};
    collect();
    // (hookshot, as Lichii #421) The candidates bound together, and the tracked sides of split accesses, must leave room for
    // what translate_program.cpp adds after this pass (up to 16 NVN slots, the tables, the pointer table, the miss log):
    // past it the descriptors' static_vector threw bad_alloc. Collected again without them: those accesses are left to
    // the run-time search unbound.
    static constexpr size_t kLaterDescriptors{16 + 4 + 2};
    if ((info.native_runtime_bounds || info.competing_bound) &&
        info.set.size() + kLaterDescriptors > Info::MAX_SSBOS) {
        StorageInfo again;
        again.pairing = info.pairing;
        again.native = info.native;
        again.bind_competing = info.bind_competing;
        again.bind_candidates = false;
        again.iterator_words = std::move(info.iterator_words);
        again.atomic_pointers = std::move(info.atomic_pointers);
        info = std::move(again);
        g_pointer_diag = diag_before;
        collect();
    }
    program.info.native_global_bounds |= info.native_runtime_bounds;
    for (const StorageBufferAddr& storage_buffer : info.set) {
        program.info.storage_buffers_descriptors.push_back({
            .cbuf_index = storage_buffer.index,
            .cbuf_offset = storage_buffer.offset,
            .count = 1,
            .is_written = info.writes.contains(storage_buffer),
        });
    }
    for (const StorageBufferAddr& table : info.tables) {
        if (program.info.num_global_tables < program.info.global_tables.size()) {
            program.info.global_tables[program.info.num_global_tables++] = {table.index, table.offset};
        }
    }
    for (const StorageInst& storage_inst : info.to_replace) {
        const StorageBufferAddr storage_buffer{storage_inst.storage_buffer};
        const auto it{info.set.find(storage_inst.storage_buffer)};
        const IR::U32 index{IR::Value{static_cast<u32>(info.set.index_of(it))}};
        IR::Block* const block{storage_inst.block};
        IR::Inst* const inst{storage_inst.inst};
        const IR::U32 offset{
            StorageOffset(*block, *inst, storage_buffer, u32(host_info.min_ssbo_alignment))};
        Replace(*block, *inst, index, offset);
    }
}

void JoinStorageInfo(Info& base, Info& source) {
    base.native_global_bounds |= source.native_global_bounds;
    auto& descriptors = base.storage_buffers_descriptors;
    for (auto& desc : source.storage_buffers_descriptors) {
        auto it{std::ranges::find_if(descriptors, [&desc](const auto& existing) {
            return desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count;
        })};
        if (it != descriptors.end()) {
            it->is_written |= desc.is_written;
            continue;
        }
        descriptors.push_back(desc);
    }
}

} // namespace Shader::Optimization
