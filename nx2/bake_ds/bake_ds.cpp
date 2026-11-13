// (bake-deepseek) Offline HKSEED01 -> SPIR-V -> hookshot_programs.bin, with no GPU, no game and no window.
//
// This is the CPU half of the bake: every stage the census enumerated is parsed exactly as the back end
// parses it at run time (parse_code, gpu/vk/vk_shader.cpp:155), lowered with the same Ampere -> Maxwell
// lowering (lower_to_maxwell.h), translated by the same Eden recompiler (TranslateProgram), and emitted by
// the same SPIR-V back end (EmitSPIRV). The environment answers are the ones the seed path already proves
// offline (seed_program, vk_shader.cpp:1893: a blank NvnState, attributes only for the bone-generic variants,
// no colour output types), so for compute and graphics this mirrors that path, not a new one.
//
// Outputs, under <outdir>:
//   spv/<program-key>.<nvn-stage>.spv   per stage
//   hookshot_programs.bin               the back end's program cache format (kDiskMagic 0x31505348 + stamp +
//                                       records exactly as program_cache_save writes them)
//   bake_report.jsonl                   one line per program attempt: key, kind, stages, descriptors, status
//   bake_manifest.json                  stamp inputs, counts, what was not translated and why
//
//   bake_ds translate --seed <hookshot_shaders.bin> --out <dir> [--stamp <hex16>] [--depth 0|1]
//                     [--target plain|tensor|tensor-software|rdna2|gfx1201] [--feature <list>]
//                     [--variants 0|1|2|all] [--variant-cap N] [--limit N] [--no-bank] [--program-size N]
//                     [--ssbo-align N] [--spv-dir <dir>]
//   bake_ds verify --bank <hookshot_programs.bin> [--target <name>] [--stamp <hex16>] [--depth 0|1]
//
// The bank's standing: the header stamp is what a build of the SAME compiler/library computes for its
// Shader::Info/Program layout (disk_stamp, vk_shader.cpp:837). This tool computes it from its own layout, so a
// bank made by a libstdc++ build loads in a libstdc++ build and is refused (safely, whole-file) by a libc++ or
// MSVC one. --stamp overrides the translator half if a build pins another. verify re-reads a bank through a
// copy of program_cache_load's reader, so the format round-trips here; a load in the game itself is not tested.
//
// --target: the feature profile the bank is baked for. A target's features are the switches the game must run
// with (the same EDEN_*/RX_VK_* variables live's disk_stamp folds from the environment, vk_shader.cpp:837-864),
// and they enter the stamp the loader compares at vk_shader.cpp:902 (`kept == stamp`). A bank baked with the
// tensor feature therefore cannot load in a run that lacks it: the whole file is refused, as live refuses any
// other stamp. Profiles also expand compute answer variants (the integer/type answers the live seed_compute
// enumerates, vk_shader.cpp:1966-2082) for every target, plain included, capped at 64 and deduplicated by the
// answers hash as live's `variants.insert` does. `tensor-software` sets Shader::Backend::SPIRV::mma_software,
// so its modules call the software matrix function (emit_spirv_warp.cpp:496) rather than carrying
// OpCooperativeMatrixMulAddKHR; `tensor` is the shuffle expansion and `stub` the D=C stub.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <dlfcn.h>

#include <vulkan/vulkan_core.h>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"   // (mma_software, live: vk_coopmat.cpp)
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/fragment_output_guard.h"
#ifndef HOOKSHOT_ENV_H   // (env_on, as the live back end reads its switches)
#include "hookshot_env.h"
#endif
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

// (live: vk_shader.cpp:57-69; the definitions live in the recompiler's passes)
namespace Shader::Optimization {
struct PointerDiag {
    uint32_t tracked, fallback, fallback_table, mixed, split, fallback_split;
};
extern thread_local PointerDiag g_pointer_diag;
extern thread_local uint32_t g_nvn2_compute_ssbo_base;
extern thread_local uint32_t g_nvn2_compute_ssbo_count;
extern thread_local uint32_t g_nvn2_compute_layout_end;
extern thread_local bool g_nvn2_miss_log_this;
extern thread_local bool g_all_global_this;
} // namespace Shader::Optimization

namespace {

constexpr uint32_t kControlMagic = 0x98761234;   // (live: vk_shader.cpp:95)
constexpr uint32_t kControlSize = 0xd8;
constexpr uint32_t kAmpereHeader = 128;
constexpr uint32_t kProgramHeaderWords = sizeof(Shader::ProgramHeader) / 8;   // 0x50 bytes
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

// ---- target feature profiles (--target) ----
// (HMMA.16816.F16 handling: live's lower_to_maxwell.h:1621-1632 refuses it unless native_matrix_multiply is
// on (the device's cooperative matrices, vk_coopmat.cpp) or EDEN_NX2_TENSOR is set; RX_VK_MMA_SOFTWARE=1
// also sets native_matrix_multiply but emits the multiplies as a shader function on shuffles
// (emit_spirv_warp.cpp), so the SPIR-V needs no cooperative-matrix device feature)
enum class TensorMode { Off, Emulate, Software, Stub };
TensorMode g_tensor = TensorMode::Off;   // (set from --target before any stage is parsed)

struct Target {
    std::string name = "plain";
    TensorMode tensor = TensorMode::Off;
    bool integer_variants = true;     // (compute answer variants, as live's seed_compute enumerates them; live does this for every compute program, no tensor switch involved: vk_draw.cpp:5490-5494)
    bool cooperative_matrix = false;  // (device class only: recorded, not a baked mode -- see the comment on gfx1201)
    std::vector<std::string> env;     // (KEY=VALUE the game must run with; live folds these into disk_stamp)
};

std::string tensor_mode_name(TensorMode m) {
    switch (m) {
    case TensorMode::Emulate: return "emulate";
    case TensorMode::Software: return "software";
    case TensorMode::Stub: return "stub";
    default: return "off";
    }
}

std::optional<Target> target_preset(std::string_view name) {
    Target t;
    if (name == "plain") return t;
    if (name == "tensor" || name == "tensor-emulate" || name == "eden-tensor") {
        t.name = "tensor";
        t.tensor = TensorMode::Emulate;
        t.env = {"EDEN_NX2_TENSOR=1"};
        return t;
    }
    // The software-multiply form needs no cooperative-matrix device feature, and RX_VK_MMA_SOFTWARE=1 both
    // selects it and names it in the stamp (RX_VK_MMA prefix). A device class alone is not stampable: live
    // enables the same form on a GPU without cooperative matrices with no variable set, and disk_stamp cannot
    // see device capabilities, so the bank would load as plain. The presets therefore pin the variable;
    // run_translate also sets Shader::Backend::SPIRV::mma_software for TensorMode::Software, which is what
    // makes the emitter call the software function instead of emitting the cooperative-matrix op.
    if (name == "tensor-software" || name == "software") {
        t.name = "tensor-software";
        t.tensor = TensorMode::Software;
        t.env = {"RX_VK_MMA_SOFTWARE=1"};
        return t;
    }
    if (name == "rdna2" || name == "gfx1030") {
        t.name = "rdna2";
        t.tensor = TensorMode::Software;
        t.env = {"RX_VK_MMA_SOFTWARE=1"};
        return t;
    }
    // gfx1201 (RDNA4) has VK_KHR_cooperative_matrix, so live may take the native form -- after its on-device
    // layout test (vk_coopmat.cpp measure_order/test_multiply). That test is device work and cannot run here,
    // and the native form's SPIR-V carries an element order only the device can confirm, so this profile bakes
    // the software form and pins it. Same modules as tensor-software/rdna2; only the recorded class differs.
    if (name == "gfx1201" || name == "rdna4") {
        t.name = "gfx1201";
        t.tensor = TensorMode::Software;
        t.cooperative_matrix = true;
        t.env = {"RX_VK_MMA_SOFTWARE=1"};
        return t;
    }
    return std::nullopt;
}

/// --feature <comma list>: tensor | tensor=emulate|software|stub | integer | no-integer, on top of a preset
bool apply_features(Target& t, const std::string& list) {
    size_t at = 0;
    while (at <= list.size()) {
        const size_t e = list.find(',', at);
        const std::string f = list.substr(at, e == std::string::npos ? std::string::npos : e - at);
        at = e == std::string::npos ? list.size() + 1 : e + 1;
        if (f.empty()) continue;
        const auto set_tensor = [&](TensorMode m, const char* token) {
            t.tensor = m;
            t.env.clear();
            if (token) t.env.push_back(token);
        };
        if (f == "tensor" || f == "tensor=emulate") set_tensor(TensorMode::Emulate, "EDEN_NX2_TENSOR=1");
        else if (f == "tensor=software") set_tensor(TensorMode::Software, "RX_VK_MMA_SOFTWARE=1");
        else if (f == "tensor=stub") set_tensor(TensorMode::Stub, "EDEN_NX2_TENSOR=stub");
        else if (f == "no-tensor") set_tensor(TensorMode::Off, nullptr);
        else if (f == "integer") t.integer_variants = true;
        else if (f == "no-integer") t.integer_variants = false;
        else return false;
    }
    return true;
}

/// The target's switches go into this process (live reads them from its own environment: EDEN_NX2_TENSOR in
/// lower_to_maxwell.h, and the stamp), so the translation and the stamp agree on them by construction. The
/// two switches a preset owns are cleared when the target does not carry them, so a caller's environment
/// cannot silently turn a different tensor path on (the same command always bakes the same modules).
void export_target_env(const Target& t) {
    const auto has = [&t](const char* key) {
        for (const std::string& kv : t.env)
            if (kv.compare(0, std::strlen(key), key) == 0 && kv.size() > std::strlen(key) && kv[std::strlen(key)] == '=')
                return true;
        return false;
    };
    for (const std::string& kv : t.env) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
    }
    for (const char* key : {"EDEN_NX2_TENSOR", "RX_VK_MMA_SOFTWARE"})
        if (!has(key)) unsetenv(key);
}

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = kFnvOffset) {   // (live: vk_shader.cpp:99)
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * kFnvPrime;
    return h;
}

// ---- NVN vertex attribute types (live: vk_shader.cpp:701 attribute_type_of; nvn_va_st fmt is the low byte) ----
Shader::AttributeType attribute_type_of(uint32_t vattr) {
    const uint32_t fmt = vattr & 0xFF;
    switch (fmt) {
    case 0x03: case 0x0F: case 0x1B: case 0x27: case 0x08: case 0x14: case 0x20: case 0x2C:
    case 0x0B: case 0x17: case 0x23: case 0x2F: case 0x3E:
        return Shader::AttributeType::UnsignedInt;
    case 0x04: case 0x10: case 0x1C: case 0x28: case 0x09: case 0x15: case 0x21: case 0x2D:
    case 0x0C: case 0x18: case 0x24: case 0x30: case 0x6A:
        return Shader::AttributeType::SignedInt;
    case 0x6B: return Shader::AttributeType::UnsignedScaled;
    case 0x6C: return Shader::AttributeType::SignedScaled;
    default:
        if (fmt >= 0x51 && fmt <= 0x68) return (fmt & 1) ? Shader::AttributeType::SignedScaled
                                                         : Shader::AttributeType::UnsignedScaled;
        return Shader::AttributeType::Float;
    }
}

struct StagePackage {
    int nvn_stage = -1;               // 0 vertex, 1 fragment, 5 compute, 6 task, 7 mesh
    uint64_t key = 0;
    std::vector<uint64_t> code;       // header (0x50 bytes, zeros for compute), Maxwell words, native address map
    std::vector<uint8_t> c1;
    uint32_t local[3]{};
    uint32_t shared_memory = 0;
    uint32_t texture_bound = ~0u;
    bool isbe = false;
    uint32_t mesh_threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0;
    bool header_only = false;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    bool ok = false;
    std::string error;                // empty when ok
};

bool control_end(const uint8_t* ctl, uint32_t* end_out) {   // (live: vk_shader.cpp:124)
    uint32_t magic, len, c1o, end;
    uint16_t st;
    std::memcpy(&magic, ctl, 4);
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&end, ctl + 0x44, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    if (magic != kControlMagic || !len || end > (16u << 20) || c1o > end || len > end || st > 7) return false;
    return *end_out = end, true;
}

/// The stage's control section and code area, parsed as parse_code's does (live: vk_shader.cpp:155).
/// Left out: the RX_VK_REJECT_* bisect lists, the RX_VK_NATIVE_MMA_KEYS choice (dev.native_mma is false here, so
/// HMMA lowers to the shuffle expansion), the native dumps and the logf lines.
StagePackage parse_stage(const uint8_t* ctl, const std::vector<uint8_t>& area) {
    StagePackage p;
    uint32_t len, c1o;
    const uint32_t end = static_cast<uint32_t>(area.size());
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    p.key = hash_bytes(area.data(), end, hash_bytes(ctl, kControlSize));
    p.nvn_stage = st;
    if (st == 6 || st == 7) {   // (the task/mesh control words, live: 160-168)
        uint32_t w58, w60;
        std::memcpy(&w58, ctl + 0x58, 4);
        std::memcpy(&w60, ctl + 0x60, 4);
        p.isbe = true;
        p.mesh_threads = w58 & 0xFFFF, p.payload_bytes = w58 >> 16;
        p.max_primitives = w60 & 0xFFFF, p.max_vertices = w60 >> 16;
        p.topology = ctl[0x5f];
    }
    const bool graphics = st != 5;
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    if (graphics) std::memcpy(p.sph, area.data(), std::min<size_t>(sizeof p.sph, area.size()));
    if (graphics && len <= hdr + 16) {   // (a header alone: the prim map stage of a mesh program)
        p.header_only = true;
        p.error = "header-only stage (mesh primitive map)";
        return p;
    }
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; hdr + (i + 1) * sizeof(Instruction) <= len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area.data() + hdr + i * sizeof(Instruction), sizeof insn);
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            p.error = "captured program's branch target is outside it";
            return p;
        }
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    Shader::Ampere::native_matrix_multiply = g_tensor == TensorMode::Software && !graphics;   // (live: dev.native_mma && !graphics && native_key)
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    if (!lowered) {
        char off[16];
        std::snprintf(off, sizeof off, "%#x", lowered.error_offset);
        p.error = std::string(lowered.error) + " (at +" + off + ")";
        return p;
    }
    p.texture_bound = lowered.required_texture_buffer;
    p.code.assign(kProgramHeaderWords, 0);
    if (graphics) std::memcpy(p.code.data(), area.data(), sizeof(Shader::ProgramHeader));
    for (const uint64_t instruction : lowered.instructions) {
        if (p.code.size() * 8 % 32 == 0) p.code.push_back(0);   // (the Maxwell scheduling word, live: 251)
        p.code.push_back(instruction);
    }
    p.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(p.code, lowered.source_instruction_offsets);
    p.c1.assign(area.begin() + c1o, area.end());
    if (!graphics) {
        std::memcpy(p.local, ctl + 0x58, 12);
        std::memcpy(&p.shared_memory, ctl + 0x64, 4);
    }
    p.ok = true;
    return p;
}

/// NVN2 compute's c0 layout words (live: vk_bank.cpp:49 CsLayout, read as seed_cs_control does, vk_bank.cpp:64)
struct CsLayout {
    uint32_t ubo = 0, ssbo = 0, tex = 0, img = 0, push = 0;
    uint32_t ubo_at() const { return 0x10 + push; }
    uint32_t ssbo_at() const { return ubo_at() + 16 * ubo; }
    uint32_t tex_at() const { return ssbo_at() + 16 * ssbo; }
    uint32_t img_at() const { return tex_at() + 8 * tex; }
    uint32_t end() const { return img_at() + 8 * img; }
};
CsLayout cs_layout_of(const uint8_t* c) {
    CsLayout l;
    l.ubo = c[0xb1 - 0x58], l.ssbo = c[0xb2 - 0x58], l.tex = c[0xb3 - 0x58], l.img = c[0xb6 - 0x58];
    l.push = (static_cast<uint32_t>(c[0xa9 - 0x58]) * 4 + 15) & ~15u;
    return l;
}

/// The translator's answers offline (live: vk_shader.cpp:357 Env). A live session answers from the draw's
/// state: driver banks c0/c2 (driver_bank_word), the uniform buffers' guest memory, registered textures. Here
/// nothing is bound, which is exactly what seed_program translates with (blank NvnState): c0/c2 read zero, c1
/// from the package, c3..c18 zero, texture slots answer Color2D and not-integer (an unbound handle's answer).
// (live: vk_shader.cpp:343 Forced) A compute program's texture variants: with nothing bound, every texture
// question of a compute program is asked of handle 0 and the slot that asks is unknown. Under `forced`, c0's
// texture and image handles read as tokens of their slots, each question is noted once by its handle, and the
// answer wanted for it is given in place of an unbound slot's. The bake uses it to enumerate the answers the
// live seed_compute tries (vk_shader.cpp:1966): one question at a time, then more, fewest first.
struct Forced {
    uint32_t lo = 0, hi = 0;   // c0's texture and image handles (the program's layout)
    std::vector<std::pair<uint32_t, uint8_t>> asked;   // (handle, 0: its type / 1: whether its format is integer)
    std::map<std::pair<uint32_t, uint8_t>, uint8_t> want;
    uint8_t answer(uint32_t handle, uint8_t kind, uint8_t plain) {
        const std::pair<uint32_t, uint8_t> q{handle, kind};
        if (std::find(asked.begin(), asked.end(), q) == asked.end()) asked.push_back(q);
        const auto it = want.find(q);
        return it != want.end() ? it->second : plain;
    }
};

class OfflineEnv final : public Shader::Environment {
public:
    OfflineEnv(const StagePackage& p, Shader::Stage st) : nvn{p} {
        stage = st;
        start_address = st == Shader::Stage::Compute ? static_cast<u32>(kProgramHeaderWords * 8) : 0;
        if (st != Shader::Stage::Compute) {
            std::memcpy(&sph, nvn.code.data(), sizeof sph);
            local_memory = static_cast<uint32_t>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        }
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override {
        return address / 8 < nvn.code.size() ? nvn.code[address / 8] : 0;
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(nvn.code, address)) return *t;
        throw Shader::RuntimeError("indirect branch to native address {:#x}, not in the program", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {
            uint32_t v = 0;
            if (static_cast<size_t>(offset) + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
            return v;
        }
        // (live: vk_shader.cpp:381: a slot's token; nothing is bound in the seed)
        if (forced && index == 0 && stage == Shader::Stage::Compute && offset >= forced->lo && offset < forced->hi && !(offset & 7))
            return 0x8000 | offset;
        return 0;   // c0/c2 not built (nothing bound), c3.. user uniform buffers unbound
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        texture_questions++;
        if (forced && handle) return static_cast<Shader::TextureType>(forced->answer(handle, 0, static_cast<uint8_t>(Shader::TextureType::Color2D)));
        return Shader::TextureType::Color2D;   // (an unbound slot's answer, live: vk_shader.cpp:394)
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat{}; }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        texture_questions++;
        if (forced && handle) return forced->answer(handle, 1, 0) != 0;
        return false;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override {
        if (nvn.texture_bound != ~0u) return nvn.texture_bound;
        return stage == Shader::Stage::Compute ? 0u : 2u;
    }
    u32 LocalMemorySize() const override { return local_memory; }
    u32 SharedMemorySize() const override {
        // (live: vk_shader.cpp:416; RX_VK_SHARED_48K=1 is the pre-PR-91 48 KB)
        static const bool old = env_on("RX_VK_SHARED_48K");
        return old ? 48u << 10 : nvn.shared_memory;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {std::max(1u, nvn.local[0]), std::max(1u, nvn.local[1]), std::max(1u, nvn.local[2])};
    }
    bool HasHLEMacroState() const override {
        // (live: vk_shader.cpp:430; RX_VK_NO_DRAW_PARAMS=1 is the pre-fix bank)
        static const bool off = env_on("RX_VK_NO_DRAW_PARAMS");
        return !off && stage == Shader::Stage::VertexB;
    }
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        // (live: 441: the word is a guest pointer or not. Every bank word but c1's reads zero; c1 holds the
        // program's own constants, and a mapping check is impossible offline, so false (as a blank state gives)
        const uint64_t key = static_cast<uint64_t>(index) << 32 | offset;
        if (const auto it = pointer_answers.find(key); it != pointer_answers.end()) return it->second;
        pointer_answers.emplace(key, false);
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        if (bank != 0 || !HasHLEMacroState()) return std::nullopt;
        switch (offset) {
        case 0x640: return Shader::ReplaceConstant::BaseVertex;
        case 0x644: return Shader::ReplaceConstant::BaseInstance;
        case 0x648: return Shader::ReplaceConstant::DrawID;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}
    /// (live: vk_shader.cpp:459 Env::set_mesh)
    void set_mesh(const std::array<u32, 3>& registers, const uint8_t* primitive_sph) {
        mesh_registers = registers;
        if (primitive_sph) std::memcpy(&mesh_primitive_header, primitive_sph, sizeof mesh_primitive_header);
    }

    int texture_questions = 0;
    Forced* forced = nullptr;   // (set only for a compute answer variant)

private:
    const StagePackage& nvn;
    uint32_t local_memory = 0;
    std::unordered_map<uint64_t, bool> pointer_answers;
};

uint64_t fnv_loop(const uint8_t* b, size_t n, uint64_t h = kFnvOffset) { return hash_bytes(b, n, h); }

/// The device limit the SPIR-V's storage-buffer address masking is baked with (live: dev.props.limits.
/// minStorageBufferOffsetAlignment). Offline it is a guess; --ssbo-align sets it. 16 (the Vulkan minimum) is
/// the safe lower bound: the mask only clears bits the descriptor address is known to have clear, so a
/// smaller value can never round a real base down (MiMo's offline translator made the same choice; GLM's
/// draft and my first cut used 64, which can move a 16/32-aligned descriptor base). A bank translated with
/// another device's limit is still the same program key, but its masked bases differ.
uint64_t g_ssbo_align = 16;

Shader::Profile make_profile() {   // (live: vk_shader.cpp:476; device properties faked)
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = 0x7FFFFFFFu;   // VK_SHADER_STAGE_ALL
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = false;
    p.support_scaled_attributes = false;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = false;
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = g_ssbo_align;   // (dev.props.limits.minStorageBufferOffsetAlignment)
    return p;
}

Shader::HostTranslateInfo host_info() {   // (live: vk_shader.cpp:508)
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    h.support_float16 = std::getenv("RX_VK_NO_F16") == nullptr;   // (live: vk_shader.cpp:510)
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = g_ssbo_align;
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;
    return h;
}


struct Translation {
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
    Shader::IR::Program program;
};

std::unique_ptr<Translation> translate(OfflineEnv& env, bool graphics, bool one_exit = false) {   // (live: vk_shader.cpp:532)
    auto t = std::make_unique<Translation>();
    Shader::Maxwell::Flow::CFG cfg{env, t->flow_blocks,
                                   static_cast<u32>(env.StartAddress() + (graphics ? sizeof(Shader::ProgramHeader) : 0u)),
                                   one_exit};
    t->program = Shader::Maxwell::TranslateProgram(t->insts, t->blocks, env, cfg, host_info());
    return t;
}

uint32_t read_word(const uint8_t* b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b + at, 4);
    return v;
}

/// HKSEED01 as seed_open reads it (live: vk_shader.cpp:1844)
struct SeedFile {
    std::vector<uint8_t> blob;
    struct Entry {
        uint32_t stage = 0;
        uint64_t key = 0;
        size_t control = 0, code = 0;
        uint32_t bytes = 0, end = 0;
        uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    };
    std::vector<Entry> stages;
    std::vector<std::vector<uint32_t>> programs;
    std::string error;
};

SeedFile read_seed(const char* path) {
    SeedFile seed;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        seed.error = "cannot open " + std::string(path);
        return seed;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    seed.blob.resize(static_cast<size_t>(size));
    if (size < 16 || std::fread(seed.blob.data(), 1, seed.blob.size(), f) != seed.blob.size() ||
        std::memcmp(seed.blob.data(), "HKSEED01", 8)) {
        seed.error = "not a HKSEED01 file";
        std::fclose(f);
        return seed;
    }
    std::fclose(f);
    const uint32_t nprograms = read_word(seed.blob.data(), 8), nstages = read_word(seed.blob.data(), 12);
    size_t at = 16;
    seed.stages.resize(nstages);
    for (auto& st : seed.stages) {
        if (at + 20 > seed.blob.size()) {
            seed.error = "stage table runs past the file";
            return seed;
        }
        const uint8_t* h = seed.blob.data() + at;
        const uint32_t csize = read_word(h, 12), bytes = read_word(h, 16);
        st.stage = read_word(h, 8);
        st.control = at + 20;
        st.code = st.control + csize;
        at = st.code + bytes;
        if (at > seed.blob.size()) {
            seed.error = "stage bytes run past the file";
            return seed;
        }
        if (csize >= kControlSize && control_end(seed.blob.data() + st.control, &st.end) && st.end <= bytes) {
            st.key = hash_bytes(seed.blob.data() + st.code, st.end,
                                hash_bytes(seed.blob.data() + st.control, kControlSize));
            std::memcpy(st.sph, seed.blob.data() + st.code, std::min<size_t>(sizeof st.sph, st.end));
        } else {
            st.end = 0;   // (not a control section this back end reads)
        }
    }
    seed.programs.resize(nprograms);
    for (auto& list : seed.programs) {
        if (at + 4 > seed.blob.size()) {
            seed.error = "program table runs past the file";
            return seed;
        }
        const uint32_t n = read_word(seed.blob.data(), at);
        at += 4;
        if (at + static_cast<size_t>(n) * 4 > seed.blob.size()) {
            seed.error = "program list runs past the file";
            return seed;
        }
        list.resize(n);
        for (uint32_t k = 0; k < n; k++) list[k] = read_word(seed.blob.data(), at + k * 4);
        at += static_cast<size_t>(n) * 4;
    }
    return seed;
}

// ---- keys (live: vk_shader.cpp:733 graphics_key, :744 mesh_key) ----
const Shader::ProgramHeader& header_of(const StagePackage& c) {
    return *reinterpret_cast<const Shader::ProgramHeader*>(c.sph);
}
uint64_t graphics_key(const StagePackage& vs, const StagePackage* fs, const uint32_t vattr[16],
                      const uint8_t color_types[8]) {
    uint64_t key = vs.key ^ (fs ? fs->key * 31 : 0);
    for (int i = 0; i < 16; i++) {
        const auto used = header_of(vs).vtg.InputGeneric(static_cast<size_t>(i));
        const bool read = used[0] || used[1] || used[2] || used[3];
        key = key * kFnvPrime ^ (static_cast<uint64_t>(read ? attribute_type_of(vattr[i])
                                                            : Shader::AttributeType::Float) << i);
    }
    uint8_t written[8]{};
    for (int i = 0; fs && i < 8; i++) written[i] = header_of(*fs).ps.HasOutputComponents(i) ? color_types[i] : 0;
    return hash_bytes(written, 8, key);
}
uint64_t mesh_key(const StagePackage* task, const StagePackage& mesh, const StagePackage* fs, const uint8_t color_types[8]) {
    uint64_t key = hash_bytes("mesh", 4, mesh.key);
    if (task) key = key * kFnvPrime ^ task->key;
    if (fs) key = key * kFnvPrime ^ fs->key;
    uint8_t written[8]{};
    for (int i = 0; fs && i < 8; i++) written[i] = header_of(*fs).ps.HasOutputComponents(i) ? color_types[i] : 0;
    return hash_bytes(written, 8, key);
}

// ---- the program cache's record format (live: vk_shader.cpp:1051-1113, program_cache_save/get_info) ----
struct Answer {
    uint16_t stage, desc;
    uint8_t type;
    bool image;
};
// (a layout mirror of mkwvk::Stage/Program for disk_stamp's sizeof(Program) word: the same members, types and
// order as vk_shader.h, with Vulkan's own handle types. A build whose struct differs is refused by the stamp
// (all-or-nothing), never misread)
struct StageLayout {
    int nvn_stage = 0;
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;
    Shader::Info info;
    std::vector<uint8_t> c1;
};
struct ProgramLayout {
    uint64_t key = 0;
    bool compute = false, failed = false;
    std::atomic<bool> ready{false};
    std::vector<StageLayout> stages;
    std::array<uint32_t, 3> workgroup{1, 1, 1};
    uint32_t shared_memory = 0;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorType> binding_types;
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    std::atomic<int> compute_state{0};
    bool push = false;
    VkShaderStageFlags push_stages = 0;
    bool uses_render_area = false;
    bool uses_push = true;
    bool mesh = false;
    std::vector<Answer> answers;
    uint64_t variant = 0;
    mutable std::atomic<int64_t> wm_sig{-1};
};

uint64_t disk_stamp(uint64_t translator_stamp, uint32_t depth) {   // (live: vk_shader.cpp:837)
    uint64_t h = kFnvOffset;
    const auto add = [&h](const void* d, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(d);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * kFnvPrime;
    };
    add(&translator_stamp, sizeof translator_stamp);
    add(&depth, sizeof depth);
    const uint32_t abi[] = {static_cast<uint32_t>(sizeof(Shader::Info)),
                            static_cast<uint32_t>(sizeof(ProgramLayout)),
                            static_cast<uint32_t>(sizeof(std::vector<uint32_t>)),
                            3u, 0u};   // libstdc++ (a libc++ or MSVC build writes 1/2 and its version)
    add(abi, sizeof abi);
    // (the allowlisted environment, in the process block's order, unsorted and not deduplicated: exactly
    // vk_shader.cpp:860-862. The target's switches were exported by export_target_env before this call, so they
    // sit where `environ` puts them -- the same position a run of this build sees. A multi-variable environment
    // whose allowlist entries are in another order is a different stamp, as it is live.)
    for (char** e = environ; e && *e; e++) {
        for (const char* pre : {"RX_VK_MMA", "RX_VK_NATIVE", "RX_VK_NAN", "RX_VK_TASK_CAP", "RX_VK_NETWORK",
                                "RX_VK_GP_", "RX_VK_FOLLOW", "RX_VK_NO_FOLLOW", "RX_VK_NO_F16", "RX_VK_SHARED", "RX_VK_MESH",
                                "RX_VK_NO_INVARIANT", "RX_VK_MARKERS", "RX_VK_NO_CBUF_LOAD_RULE",
                                "RX_VK_PTR_SPLIT_FALLBACK", "RX_VK_ALL_GLOBAL_PROGS", "RX_VK_NO_EXTREGS", "RX_VK_NO_DRAW_PARAMS",
                                "RX_VK_MISS_LOG", "RX_VK_BLOCK_PROBE", "RX_VK_LOWER_SKIP", "EDEN_"})
            if (!std::strncmp(*e, pre, std::strlen(pre))) add(*e, std::strlen(*e));
    }
    return h;
}

void put_bytes(std::vector<uint8_t>& o, const void* d, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(d);
    o.insert(o.end(), b, b + n);
}
template <class T> void put(std::vector<uint8_t>& o, const T& v) { put_bytes(o, &v, sizeof v); }
template <class V> void put_list(std::vector<uint8_t>& o, const V& v) {
    put(o, static_cast<uint64_t>(v.size()));
    for (const auto& e : v) put(o, e);
}
/// Shader::Info is plain data but for its heap members (boost vectors); they go after the raw bytes, as live does
void put_info(std::vector<uint8_t>& o, const Shader::Info& i) {
    put_bytes(o, &i, sizeof i);
    put_list(o, i.constant_buffer_descriptors);
    put_list(o, i.storage_buffers_descriptors);
    put_list(o, i.texture_buffer_descriptors);
    put_list(o, i.image_buffer_descriptors);
    put_list(o, i.texture_descriptors);
    put_list(o, i.image_descriptors);
    put(o, static_cast<uint64_t>(i.legacy_stores_mapping.size()));
    for (const auto& kv : i.legacy_stores_mapping) put(o, kv.first), put(o, kv.second);
    put(o, static_cast<uint64_t>(i.handle_programs.size()));
    for (const auto& h : i.handle_programs) put_list(o, h);
}

struct Reader {   // (live: vk_shader.cpp:1000; the same boundaries)
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;
    bool has(uint64_t n) {
        if (!bad && n > static_cast<uint64_t>(end - p)) bad = true;
        return !bad;
    }
    template <class T> T get() {
        alignas(T) uint8_t b[sizeof(T)]{};
        if (has(sizeof(T))) {
            std::memcpy(b, p, sizeof(T));
            p += sizeof(T);
        }
        return *reinterpret_cast<T*>(b);
    }
    uint64_t count(size_t elem) {
        const uint64_t n = get<uint64_t>();
        if (!bad && n > static_cast<uint64_t>(end - p) / (elem ? elem : 1)) bad = true;
        return bad ? 0 : n;
    }
    template <class V> void list(V& v) {
        const uint64_t n = count(sizeof(typename V::value_type));
        v.clear();
        v.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n && !bad; i++) v.push_back(get<typename V::value_type>());
    }
};
void get_info(Reader& r, Shader::Info& i) {
    if (!r.has(sizeof i)) return;
    alignas(Shader::Info) static thread_local uint8_t fresh[sizeof(Shader::Info)];
    std::memcpy(fresh, static_cast<const void*>(&i), sizeof i);
    std::memcpy(static_cast<void*>(&i), r.p, sizeof i);
    r.p += sizeof i;
#define KEEP(m) std::memcpy(static_cast<void*>(&i.m), fresh + (reinterpret_cast<const uint8_t*>(&i.m) - reinterpret_cast<const uint8_t*>(&i)), sizeof i.m)
    KEEP(constant_buffer_descriptors);
    KEEP(storage_buffers_descriptors);
    KEEP(texture_buffer_descriptors);
    KEEP(image_buffer_descriptors);
    KEEP(texture_descriptors);
    KEEP(image_descriptors);
    KEEP(legacy_stores_mapping);
    KEEP(handle_programs);
#undef KEEP
    r.list(i.constant_buffer_descriptors);
    r.list(i.storage_buffers_descriptors);
    r.list(i.texture_buffer_descriptors);
    r.list(i.image_buffer_descriptors);
    r.list(i.texture_descriptors);
    r.list(i.image_descriptors);
    for (uint64_t n = r.count(2 * sizeof(Shader::IR::Attribute)); n && !r.bad; n--) {
        const auto k = r.get<Shader::IR::Attribute>();
        i.legacy_stores_mapping[k] = r.get<Shader::IR::Attribute>();
    }
    i.handle_programs.resize(static_cast<size_t>(r.count(sizeof(uint64_t))));
    for (auto& h : i.handle_programs) r.list(h);
}

// ---- answers (live: vk_shader.cpp:604-668). Offline the banks are blank, so handles read 0 and the plain
//      answer is the descriptor's own type, exactly as an unbound slot is recorded live.
uint32_t stage_handle(const Shader::Info& info, uint32_t program, OfflineEnv& env) {
    if (program >= info.handle_programs.size()) return 0;
    return Shader::EvaluateHandleProgram(
               info.handle_programs[program],
               [&](u32 index, u32 offset) -> u32 { return env.ReadCbufValue(index, offset); },
               [&](u32 binding, u32) -> u32 {
                   if (binding >= info.storage_buffers_descriptors.size()) return 0;
                   const auto& d = info.storage_buffers_descriptors[binding];
                   const uint64_t base = env.ReadCbufValue(d.cbuf_index, d.cbuf_offset) |
                                         static_cast<uint64_t>(env.ReadCbufValue(d.cbuf_index, d.cbuf_offset + 4)) << 32;
                   return base ? 0u : 0u;   // (a storage buffer's guest words are unbound offline: zero)
               })
        .value_or(0u);
}
uint32_t descriptor_handle(const Shader::Info& info, const Shader::TextureDescriptor& d, OfflineEnv& env) {
    if (d.handle_program != ~0U) return stage_handle(info, d.handle_program, env);
    uint32_t h = env.ReadCbufValue(d.cbuf_index, d.cbuf_offset);
    if (d.has_secondary)
        h = h << d.shift_left | env.ReadCbufValue(d.secondary_cbuf_index, d.secondary_cbuf_offset) << d.secondary_shift_left;
    return h;
}
uint32_t descriptor_handle(const Shader::Info& info, const Shader::ImageDescriptor& d, OfflineEnv& env) {
    if (d.handle_program != ~0U) return stage_handle(info, d.handle_program, env);
    return env.ReadCbufValue(d.cbuf_index, d.cbuf_offset);
}
uint8_t texture_answer(uint32_t handle) {   // (live: 618; an unregistered or zero handle is Color2D, not integer)
    return handle ? static_cast<uint8_t>(Shader::TextureType::Color2D) : 0xFF;
}
void record_answers(std::vector<Answer>& answers, const std::vector<Shader::Info>& infos, OfflineEnv* envs[]) {
    answers.clear();
    for (size_t k = 0; k < infos.size(); k++) {
        const auto& list = infos[k].texture_descriptors;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].count == 1) {
                const uint32_t h = descriptor_handle(infos[k], list[i], *envs[k]);
                answers.push_back({static_cast<uint16_t>(k), static_cast<uint16_t>(i),
                                   h ? texture_answer(h)
                                     : static_cast<uint8_t>(static_cast<uint8_t>(list[i].type) |
                                                            (list[i].is_integer ? 0x80 : 0))});
            }
        const auto& images = infos[k].image_descriptors;
        for (size_t i = 0; i < images.size(); i++)
            if (images[i].count == 1) {
                const uint32_t h = descriptor_handle(infos[k], images[i], *envs[k]);
                answers.push_back({static_cast<uint16_t>(k), static_cast<uint16_t>(i),
                                   h ? static_cast<uint8_t>(texture_answer(h) & 0x80)
                                     : static_cast<uint8_t>(images[i].is_integer ? 0x80 : 0),
                                   true});
            }
    }
}
uint64_t answers_hash(const std::vector<Answer>& answers) {   // (live: vk_shader.cpp:642)
    uint64_t h = 0;
    for (const Answer& a : answers)
        h = (h ^ (static_cast<uint64_t>(a.stage) << 32 | static_cast<uint64_t>(a.desc) << 16 |
                  static_cast<uint64_t>(a.type) << 1 | static_cast<uint64_t>(a.image))) * kFnvPrime;
    return h;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (const char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += c;
            }
        }
    }
    return o;
}
std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

struct StageResult {
    int nvn_stage = -1;
    uint32_t vk_stage = 0;   // 0x10 vertex, 0x20 fragment, 0x20? compute 0x20? (set below)
    std::vector<uint32_t> spirv;
    std::vector<uint8_t> c1;
    Shader::Info info;
};
/// (live: vk_shader.cpp:1298 mesh_registers_of)
std::array<uint32_t, 3> mesh_registers_of(const StagePackage* task, const StagePackage& mesh) {
    const uint32_t topology = mesh.topology == 3 ? 4 : mesh.topology == 1 ? 0 : mesh.topology;
    const uint32_t lines = task ? (task->payload_bytes + 127) / 128 : 0;
    const uint32_t most = std::max(mesh.max_vertices, mesh.max_primitives);
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13),
            std::min(mesh.mesh_threads, 0x3Fu) << 20,
            (task ? std::min(task->mesh_threads, 0x3Fu) : 0) | (std::min(lines, 0x1FFu) << 12) |
                (std::min(lines, 0x1FFu) << 22)};
}

struct ProgramResult {
    uint64_t key = 0;
    bool compute = false, mesh = false;
    std::vector<StageResult> stages;
    std::array<uint32_t, 3> workgroup{1, 1, 1};
    uint32_t shared_memory = 0;
    std::vector<Answer> answers;
    uint64_t variant = 0;
};

/// A seed stage, parsed once per stage index (the seed lists each once)
struct ParsedStage {
    StagePackage pkg;
    bool parsed = false;
};

constexpr uint32_t kVkVertex = 0x00000001;     // VK_SHADER_STAGE_VERTEX_BIT
constexpr uint32_t kVkFragment = 0x00000010;   // VK_SHADER_STAGE_FRAGMENT_BIT
constexpr uint32_t kVkCompute = 0x00000020;    // VK_SHADER_STAGE_COMPUTE_BIT
constexpr uint32_t kVkTask = 0x00000040;       // VK_SHADER_STAGE_TASK_BIT_EXT
constexpr uint32_t kVkMesh = 0x00000080;       // VK_SHADER_STAGE_MESH_BIT_EXT

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  bake_ds translate --seed <hookshot_shaders.bin> --out <dir> [--stamp <hex16>] [--depth 0|1]\n"
                 "                    [--target plain|tensor|tensor-software|rdna2|gfx1201] [--feature <list>]\n"
                 "                    [--variants 0|1|2|all] [--variant-cap N] [--limit N] [--no-bank] [--program-size N]\n"
                 "                    [--ssbo-align N] [--spv-dir <dir>]\n"
                 "  bake_ds verify --bank <hookshot_programs.bin> [--target <name>] [--feature <list>] [--stamp <hex16>] [--depth 0|1]\n"
                 "  bake_ds query                       (physical devices' properties/features: no logical device)\n"
                 "\n"
                 "  --target      the feature profile the bank is baked for; its switches enter the cache stamp, so a bank\n"
                 "                made for one profile is refused by a run with another (as live refuses any stamp)\n"
                 "  --feature     comma list on top of the preset: tensor | tensor=emulate|software|stub | integer | no-integer\n");
    return 2;
}

int verify_bank(const char* path, const Target* target, uint64_t translator_stamp, uint32_t depth) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "verify: cannot open %s\n", path);
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    const long long whole = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint32_t head[2]{};
    uint64_t stamp = 0;
    if (whole < 16 || std::fread(head, 4, 2, f) != 2 || std::fread(&stamp, 8, 1, f) != 1 || head[0] != 0x31505348) {
        std::fprintf(stderr, "verify: not a program cache (bad header)\n");
        std::fclose(f);
        return 1;
    }
    // (live: vk_shader.cpp:902: disk_init reads magic + stamp and refuses the whole file when `kept != stamp`.
    // A target check computes the stamp the game would for that profile, with the same feature tokens and the
    // same other allowlisted environment, and refuses a bank baked for another profile the same way.)
    if (target) {
        const uint64_t want = disk_stamp(translator_stamp, depth);
        if (want != stamp) {
            std::fprintf(stderr, "verify: stamp %s is not target '%s's %s: refused (the loader compares the same word, "
                                 "vk_shader.cpp:902)\n", hex64(stamp).c_str(), target->name.c_str(), hex64(want).c_str());
            std::fclose(f);
            return 3;
        }
        std::printf("verify: target '%s' stamp %s matches\n", target->name.c_str(), hex64(want).c_str());
    }
    std::vector<uint8_t> data(static_cast<size_t>(whole) - 16);
    std::fseek(f, 16, SEEK_SET);
    const bool whole_read = std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!whole_read) {
        std::fprintf(stderr, "verify: short read\n");
        return 1;
    }
    size_t at = 0;
    size_t records = 0, stages = 0, words = 0, bad = 0;
    while (at + 16 <= data.size()) {
        uint64_t key, size;
        std::memcpy(&key, data.data() + at, 8);
        std::memcpy(&size, data.data() + at + 8, 8);
        at += 16;
        if (at + size > data.size()) {
            std::fprintf(stderr, "verify: record %s runs past the file (%llu > %zu)\n", hex64(key).c_str(),
                         static_cast<unsigned long long>(size), data.size() - at);
            bad++;
            break;
        }
        Reader r{data.data() + at, data.data() + at + size};
        const uint32_t kind = r.get<uint32_t>();
        std::vector<Answer> answers;
        r.list(answers);
        const auto wg = r.get<std::array<uint32_t, 3>>();
        const uint32_t shared = r.get<uint32_t>();
        const uint64_t n = r.count(1);
        bool ok = !r.bad && n >= 1 && n <= 8;
        Shader::Info info{};
        for (uint64_t k = 0; ok && k < n; k++) {
            const int nvn_stage = r.get<int>();
            const uint32_t vk_stage = r.get<uint32_t>();
            std::vector<uint32_t> spirv;
            std::vector<uint8_t> c1;
            r.list(spirv);
            r.list(c1);
            get_info(r, info);
            words += spirv.size();
            stages++;
            ok = !r.bad && !spirv.empty();
            (void)nvn_stage;
            (void)vk_stage;
        }
        if (!ok || r.bad) {
            bad++;
            std::fprintf(stderr, "verify: record %s unreadable\n", hex64(key).c_str());
        } else {
            records++;
            (void)wg;
            (void)shared;
        }
        at += size;
    }
    std::printf("verify: magic 0x31505348, stamp %s, %zu records, %zu stages, %zu SPIR-V words, %zu bad, %zu trailing bytes\n",
                hex64(stamp).c_str(), records, stages, words, bad, data.size() - at);
    return bad ? 1 : 0;
}

// ---- the translation pass ----

struct Options {
    std::string seed, out;
    uint64_t stamp = ~0ull;
    uint32_t depth = 0;
    int variants = -1;   // (0: input variant 0 only; 1: 0 and 1; 2: 0,1,2; -1: all, prepare_run's programs*3 default)
    int limit = 0;
    bool bank = true;
    long program_size = 0;
    uint64_t ssbo_align = 16;
    Target target;
    int variant_cap = 64;   // (compute answer variants per program, baseline included; live's prepare_run uses 64: vk_draw.cpp:5492)
    std::string spv_dir;   // (default <out>/spv)
};

struct AttrVariant {
    uint32_t attributes[16]{};
    int index = 0;
};

std::vector<AttrVariant> attr_variants(const Options& o) {
    std::vector<AttrVariant> v;
    v.push_back({});
    const int most = o.variants < 0 ? 2 : o.variants;
    for (int i = 1; i <= most; i++) {
        AttrVariant a;
        a.index = i;
        a.attributes[6] = 0x27;   // RGBA8UI in nvn_va_st.fmt (NVN_FORMAT_RGBA8UI = 0x27)
        if (i >= 2) a.attributes[7] = 0x27;
        v.push_back(a);
    }
    return v;
}

bool reads_generic(const StagePackage& vs, int g) {
    const auto used = header_of(vs).vtg.InputGeneric(static_cast<size_t>(g));
    return used[0] || used[1] || used[2] || used[3];
}

void write_u64(FILE* f, uint64_t v) { std::fwrite(&v, 8, 1, f); }

int run_translate(const Options& o) {
    SeedFile seed = read_seed(o.seed.c_str());
    if (!seed.error.empty()) {
        std::fprintf(stderr, "bake_ds: %s\n", seed.error.c_str());
        return 2;
    }
    g_ssbo_align = o.ssbo_align ? o.ssbo_align : 16;
    g_tensor = o.target.tensor;
    // (live: vk_coopmat.cpp:339-353. Under RX_VK_MMA_SOFTWARE=1 the switch opens the native gate AND makes
    // emit_spirv_warp.cpp:496 call the software matrix function; without this assignment the gate opens but the
    // module still carries OpCooperativeMatrixMulAddKHR, i.e. a bank named "software" that needs the very
    // device feature it was meant to avoid. Set once, before any stage is translated.)
    Shader::Backend::SPIRV::mma_software = g_tensor == TensorMode::Software;
    std::error_code ec;
    const std::string spv_dir = o.spv_dir.empty() ? o.out + "/spv" : o.spv_dir;
    std::filesystem::create_directories(o.out, ec);
    std::filesystem::create_directories(spv_dir, ec);
    // (HOOKSHOT_SHADER_STAMP is the translator half of the stamp; the tool's default 0 is for format tests.
    // --stamp takes the value nx2/build.py computed and the game logged.)
    const uint64_t disk = disk_stamp(o.stamp == ~0ull ? 0 : o.stamp, o.depth);
    std::fprintf(stderr, "bake_ds: %zu programs, %zu stages; target %s (%s, integer variants %s); translator stamp %s -> "
                         "cache stamp %s; sizeof(Info) %zu, sizeof(ProgramLayout) %zu\n",
                 seed.programs.size(), seed.stages.size(), o.target.name.c_str(),
                 tensor_mode_name(o.target.tensor).c_str(), o.target.integer_variants ? "on" : "off",
                 hex64(o.stamp == ~0ull ? 0 : o.stamp).c_str(), hex64(disk).c_str(), sizeof(Shader::Info),
                 sizeof(ProgramLayout));
    for (const std::string& kv : o.target.env) std::fprintf(stderr, "bake_ds: target requires %s in the game's environment\n", kv.c_str());
    FILE* bank = nullptr;
    std::string bank_path = o.out + "/hookshot_programs.bin";
    std::vector<uint8_t> bank_head;
    if (o.bank) {
        bank = std::fopen(bank_path.c_str(), "wb");
        if (!bank) {
            std::fprintf(stderr, "bake_ds: cannot write %s\n", bank_path.c_str());
            return 2;
        }
        const uint32_t h[2] = {0x31505348, 0};
        std::fwrite(h, 4, 2, bank);
        write_u64(bank, disk);
        std::fflush(bank);
    }
    FILE* report = std::fopen((o.out + "/bake_report.jsonl").c_str(), "w");
    if (!report) {
        std::fprintf(stderr, "bake_ds: cannot write the report\n");
        if (bank) std::fclose(bank);
        return 2;
    }
    std::vector<ParsedStage> parsed(seed.stages.size());
    const auto pkg_of = [&](uint32_t idx) -> const StagePackage& {
        ParsedStage& ps = parsed[idx];
        if (!ps.parsed) {
            const auto& e = seed.stages[idx];
            if (!e.end) {
                ps.pkg.error = "not a control section";
            } else {
                std::vector<uint8_t> area(seed.blob.begin() + e.code, seed.blob.begin() + e.code + e.end);
                ps.pkg = parse_stage(seed.blob.data() + e.control, area);
            }
            ps.parsed = true;
        }
        return ps.pkg;
    };

    size_t translated = 0, failed = 0, skipped = 0, program_slots = 0, duplicate_variants = 0;
    // (live: seed_compute's `variants` set, per key and across prepare calls. A trial whose recorded answers hash
    // equals one already kept is not a variant: a type-only answer changes the SPIR-V but not the answers (the
    // descriptor keeps its own type), so live drops it at vk_shader.cpp:2073-2079. Ours must drop it too, or the
    // bank gains two records of one key with one hash, which no loader/ still_valid pair can tell apart.)
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> kept_variants;
    std::vector<AttrVariant> variants = attr_variants(o);
    const uint8_t no_types[8]{};
    for (size_t pi = 0; pi < seed.programs.size(); pi++) {
        if (o.limit > 0 && static_cast<int>(pi) >= o.limit) break;
        // (the program's stages by NVN stage, as seed_program does; a stage that is not a control section drops the program)
        const StagePackage* by_stage[8]{};
        const char* bad_status = nullptr;
        std::string bad_what;
        uint32_t bad_stage = 0;
        for (const uint32_t k : seed.programs[pi]) {
            if (k >= seed.stages.size() || !seed.stages[k].end) {
                bad_status = "INVALID_STAGE";
                bad_what = "not a control section this back end reads";
                bad_stage = k < seed.stages.size() ? seed.stages[k].stage : 0;
                break;
            }
            const StagePackage& pkg = pkg_of(k);
            if (!pkg.ok) {
                bad_status = pkg.header_only ? "HEADER_ONLY" : "LOWER_ERROR";
                bad_what = pkg.error;
                bad_stage = seed.stages[k].stage;
                break;
            }
            by_stage[seed.stages[k].stage & 7] = &pkg;
        }
        if (bad_status) {   // (status names follow bake-mimo's stop-line taxonomy)
            skipped++;
            std::fprintf(report, "{\"program\":%zu,\"status\":\"%s\",\"stage\":%u,\"what\":\"%s\"}\n",
                         pi, bad_status, bad_stage, json_escape(bad_what).c_str());
            continue;
        }
        const StagePackage* one = by_stage[5] ? by_stage[5] : by_stage[7] ? by_stage[7] : by_stage[0];
        if (!one) {
            skipped++;
            std::fprintf(report, "{\"program\":%zu,\"status\":\"no-vertex-or-compute\"}\n", pi);
            continue;
        }
        const bool compute = by_stage[5] != nullptr;
        const bool mesh = !compute && by_stage[7] != nullptr;
        // (a compute translation with the c0 texture/image handles as slot tokens when `f` is set: the live
        // seed_compute's run(), one Try per answer combination)
        struct ComputeTry {
            ProgramResult pr;
            CsLayout cs{};
            int questions = 0;
            std::vector<std::pair<uint32_t, uint8_t>> asked;
            std::string err;
        };
        const auto compute_try = [&](Forced* f, ComputeTry& t) {
            t = ComputeTry{};
            // (the compute stage's own control section; it is stage 5 of the program)
            const uint8_t* ctl5 = nullptr;
            for (const uint32_t k : seed.programs[pi])
                if ((seed.stages[k].stage & 7) == 5) ctl5 = seed.blob.data() + seed.stages[k].control;
            t.cs = cs_layout_of(ctl5 + 0x58);
            OfflineEnv env{*by_stage[5], Shader::Stage::Compute};
            env.forced = f;
            Shader::Optimization::g_nvn2_compute_ssbo_base = t.cs.ssbo ? t.cs.ssbo_at() : 0;
            Shader::Optimization::g_nvn2_compute_ssbo_count = t.cs.ssbo ? t.cs.ssbo : 16;
            Shader::Optimization::g_nvn2_compute_layout_end = t.cs.ssbo ? t.cs.end() : 0;
            Shader::Optimization::g_nvn2_miss_log_this = true;
            Shader::Optimization::g_all_global_this = false;
            struct Reset {
                ~Reset() {
                    Shader::Optimization::g_nvn2_compute_ssbo_base = 0;
                    Shader::Optimization::g_nvn2_compute_ssbo_count = 16;
                    Shader::Optimization::g_nvn2_compute_layout_end = 0;
                    Shader::Optimization::g_all_global_this = false;
                }
            } reset;
            t.pr.compute = true;
            try {
                auto tr = translate(env, false);
                t.questions = env.texture_questions;
                Shader::RuntimeInfo rt{};
                StageResult sr;
                sr.info = tr->program.info;
                sr.nvn_stage = 5;
                sr.c1 = by_stage[5]->c1;
                Shader::Backend::Bindings bindings{};
                sr.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), rt, tr->program, bindings);
                if (sr.spirv.empty()) t.err = "compute EmitSPIRV returned no words";
                t.pr.workgroup = {tr->program.workgroup_size[0], tr->program.workgroup_size[1], tr->program.workgroup_size[2]};
                t.pr.shared_memory = tr->program.shared_memory_size;
                sr.vk_stage = kVkCompute;
                t.pr.stages.push_back(std::move(sr));
            } catch (const std::exception& e) {
                t.err = e.what();
            }
            if (f) t.asked = f->asked;
        };
        // (live: vk_shader.cpp:1966-2082 seed_compute: each question's other answers alone, then two and more,
        // fewest first, up to the cap; an answer that changes the SPIR-V makes a variant. The baseline is
        // translate() with nothing forced, so a plain run's records are the ones it always was.)
        const auto answer_variants = [&](const ComputeTry& base) {
            std::vector<ComputeTry> kept;
            if (base.asked.empty() || !base.err.empty() || base.pr.stages.empty()) return kept;
            const auto base_asked = base.asked;
            const auto& base_spirv = base.pr.stages[0].spirv;
            const auto room = [&] { return kept.size() + 1 < static_cast<size_t>(o.variant_cap); };
            std::vector<std::vector<uint8_t>> options(base_asked.size());
            for (size_t q = 0; q < base_asked.size(); q++) {
                const uint8_t kind = base_asked[q].second;
                const uint8_t plain = kind ? 0 : static_cast<uint8_t>(Shader::TextureType::Color2D);
                for (uint8_t a = 0; a < (kind ? 2 : static_cast<uint8_t>(Shader::NUM_TEXTURE_TYPES)); a++) {
                    if (a == plain || !room()) continue;
                    Forced f;
                    f.lo = base.cs.tex_at();
                    f.hi = base.cs.end();
                    f.want[base_asked[q]] = a;
                    ComputeTry t;
                    compute_try(&f, t);
                    if (t.err.empty() && t.asked == base_asked && !t.pr.stages.empty() &&
                        t.pr.stages[0].spirv != base_spirv) {
                        options[q].push_back(a);
                        kept.push_back(std::move(t));
                    }
                }
            }
            std::vector<size_t> open;
            for (size_t q = 0; q < options.size(); q++)
                if (!options[q].empty()) open.push_back(q);
            for (size_t n = 2; n <= open.size() && room(); n++) {
                std::vector<size_t> pick(n), choice(n);
                for (size_t k = 0; k < n; k++) pick[k] = k;
                for (;;) {
                    std::fill(choice.begin(), choice.end(), 0);
                    for (;;) {
                        if (!room()) break;
                        Forced f;
                        f.lo = base.cs.tex_at();
                        f.hi = base.cs.end();
                        for (size_t k = 0; k < n; k++)
                            f.want[base_asked[open[pick[k]]]] = options[open[pick[k]]][choice[k]];
                        ComputeTry t;
                        compute_try(&f, t);
                        if (t.err.empty() && t.asked == base_asked) kept.push_back(std::move(t));
                        size_t k = 0;
                        while (k < n && ++choice[k] == options[open[pick[k]]].size()) choice[k++] = 0;
                        if (k == n) break;
                    }
                    if (!room()) break;
                    size_t k = n;
                    while (k > 0 && pick[k - 1] == open.size() - n + k - 1) k--;
                    if (k == 0) break;
                    pick[k - 1]++;
                    for (size_t j = k; j < n; j++) pick[j] = pick[j - 1] + 1;
                }
            }
            return kept;
        };
        // (one bank record, its answers hash and its SPIR-V files: live: vk_shader.cpp:1021 program_cache_save)
        const auto emit_slot = [&](ProgramResult& pr, uint64_t key, int attr_variant, int answer_variant, const CsLayout& cs,
                                   int questions, std::unordered_set<uint64_t>* seen) -> bool {
            // answers, as record_answers would leave them for this translation
            std::vector<Shader::Info> infos;
            std::vector<std::unique_ptr<OfflineEnv>> envs_held;
            std::vector<OfflineEnv*> envs;
            for (size_t k = 0; k < pr.stages.size(); k++) {
                const StagePackage& pkg = pr.compute ? *by_stage[5]
                                          : mesh ? (k == 0 && by_stage[6] ? *by_stage[6]
                                                    : pr.stages[k].nvn_stage == 7 ? *by_stage[7] : *by_stage[1])
                                                 : (k == 0 ? *by_stage[0] : *by_stage[1]);
                envs_held.push_back(std::make_unique<OfflineEnv>(pkg, Shader::Stage::Compute));
                envs.push_back(envs_held.back().get());
                infos.push_back(pr.stages[k].info);
            }
            record_answers(pr.answers, infos, envs.data());
            pr.variant = answers_hash(pr.answers);
            // (live: vk_shader.cpp:2073-2079 `if (variants.insert(t.p->variant).second)`; the baseline is inserted
            // first, then each trial, so a duplicate answer set is dropped instead of written twice)
            if (seen && !seen->insert(pr.variant).second) {
                duplicate_variants++;
                return false;
            }
            // write SPIR-V (the baseline keeps <key>.<stage>.spv; an answer variant adds .av<N>)
            for (auto& s : pr.stages) {
                std::string name = hex64(key) + "." + std::to_string(s.nvn_stage);
                if (answer_variant) name += ".av" + std::to_string(answer_variant);
                const std::string path = spv_dir + "/" + name + ".spv";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(s.spirv.data(), 4, s.spirv.size(), f);
                    std::fclose(f);
                }
            }
            // bank record (live: vk_shader.cpp:1047 program_cache_save)
            if (bank) {
                std::vector<uint8_t> o2;
                put(o2, static_cast<uint32_t>((pr.compute ? 1u : 0u) | (pr.mesh ? 2u : 0u)));
                put_list(o2, pr.answers);
                put(o2, pr.workgroup);
                put(o2, pr.shared_memory);
                put(o2, static_cast<uint64_t>(pr.stages.size()));
                for (auto& s : pr.stages) {
                    put(o2, static_cast<int32_t>(s.nvn_stage));
                    put(o2, s.vk_stage);
                    put_list(o2, s.spirv);
                    put_list(o2, s.c1);
                    put_info(o2, s.info);
                }
                const uint64_t head[2] = {key, o2.size()};
                std::fwrite(head, 8, 2, bank);
                std::fwrite(o2.data(), 1, o2.size(), bank);
                std::fflush(bank);
            }
            translated++;
            // report line with the descriptor plan (what a pipeline layout/bank must bind)
            std::fprintf(report,
                         "{\"program\":%zu,\"key\":\"%s\",\"kind\":\"%s\",\"variant\":%d,\"answer_variant\":%d,"
                         "\"answers_hash\":\"%s\",\"status\":\"translated\","
                         "\"workgroup\":[%u,%u,%u],\"shared_memory\":%u,\"answers\":%zu,\"questions\":%d,\"stages\":[",
                         pi, hex64(key).c_str(), compute ? "compute" : mesh ? "mesh" : "graphics", attr_variant, answer_variant,
                         hex64(pr.variant).c_str(), pr.workgroup[0], pr.workgroup[1], pr.workgroup[2], pr.shared_memory,
                         pr.answers.size(), questions);
            for (size_t k = 0; k < pr.stages.size(); k++) {
                const auto& s = pr.stages[k];
                std::fprintf(report,
                             "%s{\"nvn_stage\":%d,\"spirv_words\":%zu,\"cbufs\":[", k ? "," : "", s.nvn_stage, s.spirv.size());
                for (size_t d = 0; d < s.info.constant_buffer_descriptors.size(); d++) {
                    const auto& c = s.info.constant_buffer_descriptors[d];
                    std::fprintf(report, "%s[%u,%u]", d ? "," : "", c.index, c.count);
                }
                std::fprintf(report, "],\"ssbos\":[");
                for (size_t d = 0; d < s.info.storage_buffers_descriptors.size(); d++) {
                    const auto& b = s.info.storage_buffers_descriptors[d];
                    std::fprintf(report, "%s[%u,%u,%u,%d]", d ? "," : "", b.cbuf_index, b.cbuf_offset, b.count,
                                 b.is_written ? 1 : 0);
                }
                std::fprintf(report, "],\"textures\":[");
                for (size_t d = 0; d < s.info.texture_descriptors.size(); d++) {
                    const auto& t = s.info.texture_descriptors[d];
                    std::fprintf(report, "%s[%u,%u,%u,%u,%u]", d ? "," : "", t.cbuf_index, t.cbuf_offset,
                                 static_cast<uint32_t>(t.type), t.is_integer ? 1 : 0, t.count);
                }
                std::fprintf(report, "],\"images\":[");
                for (size_t d = 0; d < s.info.image_descriptors.size(); d++) {
                    const auto& im = s.info.image_descriptors[d];
                    std::fprintf(report, "%s[%u,%u,%u,%u]", d ? "," : "", im.cbuf_index, im.cbuf_offset,
                                 im.is_integer ? 1 : 0, im.count);
                }
                std::fprintf(report, "],\"c1_bytes\":%zu,\"uses_fp16\":%d,\"uses_fp64\":%d,\"uses_cm\":%d,\"local_memory\":%u}",
                             s.c1.size(), s.info.uses_fp16 ? 1 : 0, s.info.uses_fp64 ? 1 : 0,
                             s.info.uses_cooperative_matrix ? 1 : 0, s.info.uses_local_memory ? 1 : 0);
            }
            if (compute)
                std::fprintf(report,
                             "],\"cs\":{\"push_words\":%u,\"push_bytes\":%u,\"ubo\":%u,\"ssbo\":%u,\"tex\":%u,\"img\":%u,"
                             "\"ubo_at\":%u,\"ssbo_at\":%u,\"tex_at\":%u,\"img_at\":%u,\"end\":%u}}\n",
                             static_cast<uint32_t>(cs.push / 4), cs.push, cs.ubo, cs.ssbo, cs.tex, cs.img, cs.ubo_at(),
                             cs.ssbo_at(), cs.tex_at(), cs.img_at(), cs.end());
            else
                std::fprintf(report, "]}\n");
            return true;
        };
        for (const AttrVariant& av : variants) {
            if (av.index && (compute || mesh)) continue;
            if (av.index && (!reads_generic(*by_stage[0], 6) || (av.index == 2 && !reads_generic(*by_stage[0], 7)))) continue;
            program_slots++;
            ProgramResult pr;
            pr.compute = compute;
            const uint64_t key = compute ? by_stage[5]->key
                                 : mesh ? mesh_key(by_stage[6], *by_stage[7], by_stage[1], no_types)
                                        : graphics_key(*by_stage[0], by_stage[1], av.attributes, no_types);
            pr.key = key;
            std::string err;
            int questions = 0;
            CsLayout cs{};   // (compute c0 layout, for the bank plan)
            if (mesh) {
                // (live: vk_shader.cpp:1320 translate_mesh_into with prim = null, as seed_program calls it)
                const std::array<uint32_t, 3> regs = mesh_registers_of(by_stage[6], *by_stage[7]);
                pr.mesh = true;
                std::unique_ptr<Translation> task_t;
                if (by_stage[6]) {
                    StageResult s6;
                    OfflineEnv env6{*by_stage[6], Shader::Stage::Task};
                    env6.set_mesh(regs, nullptr);
                    task_t = translate(env6, true, true);
                    questions += env6.texture_questions;
                    Shader::RuntimeInfo rt6{};
                    rt6.previous_stage_stores.mask.set();
                    s6.info = task_t->program.info;
                    s6.nvn_stage = 6;
                    s6.c1 = by_stage[6]->c1;
                    Shader::Backend::Bindings b6{};
                    s6.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), rt6, task_t->program, b6);
                    if (s6.spirv.empty()) err = "task EmitSPIRV returned no words";
                    s6.vk_stage = kVkTask;
                    pr.stages.push_back(std::move(s6));
                }
                StageResult sm;
                OfflineEnv menv{*by_stage[7], Shader::Stage::Mesh};
                menv.set_mesh(regs, nullptr);
                auto mesh_t = translate(menv, true, true);
                questions += menv.texture_questions;
                Shader::RuntimeInfo mrt{};
                if (task_t) mrt.previous_stage_stores = task_t->program.info.stores;
                else mrt.previous_stage_stores.mask.set();
                mrt.convert_depth_mode = o.depth != 0;
                sm.info = mesh_t->program.info;
                sm.nvn_stage = 7;
                sm.c1 = by_stage[7]->c1;
                Shader::Backend::Bindings bm{};
                sm.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), mrt, mesh_t->program, bm);
                if (sm.spirv.empty()) err = "mesh EmitSPIRV returned no words";
                sm.vk_stage = kVkMesh;
                pr.stages.push_back(std::move(sm));
                if (err.empty() && by_stage[1]) {
                    OfflineEnv fenv{*by_stage[1], Shader::Stage::Fragment};
                    auto fs = translate(fenv, true);
                    questions += fenv.texture_questions;
                    Shader::RuntimeInfo frt{};
                    frt.previous_stage_stores = mesh_t->program.info.stores;
                    for (const uint32_t line : mesh_t->program.mesh.primitive_lines) {
                        const auto attribute = static_cast<Shader::IR::Attribute>(line);
                        if (Shader::IR::IsGeneric(attribute)) frt.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
                    }
                    if (mesh_t->program.mesh.topology == 0)
                        for (size_t i = 0; i < Shader::IR::NUM_GENERICS; i++)
                            if (fs->program.info.loads.Generic(i) && !mesh_t->program.info.stores.Generic(i))
                                frt.point_coord_generics.set(i);
                    for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(no_types[i]);
                    uint32_t float_targets = 0;
                    for (int i = 0; i < 8; i++)
                        if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
                    static const bool nan_guard2 = !std::getenv("RX_VK_NAN_GUARD") || std::atoi(std::getenv("RX_VK_NAN_GUARD")) != 0;
                    if (nan_guard2) Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
                    StageResult f;
                    f.info = fs->program.info;
                    f.nvn_stage = 1;
                    f.c1 = by_stage[1]->c1;
                    Shader::Backend::Bindings fb{};
                    f.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), frt, fs->program, fb);
                    if (f.spirv.empty()) err = "fragment EmitSPIRV returned no words";
                    f.vk_stage = kVkFragment;
                    pr.stages.push_back(std::move(f));
                }
            }
            if (!compute && !mesh) {
                Shader::RuntimeInfo vrt{};
                OfflineEnv env{*by_stage[0], Shader::Stage::VertexB};
                auto t = translate(env, true);
                questions += env.texture_questions;
                if (!t->program.info.stores[Shader::IR::Attribute::PointSize]) vrt.fixed_state_point_size = 1.0f;
                for (int i = 0; i < 16; i++) vrt.generic_input_types[i] = attribute_type_of(av.attributes[i]);
                vrt.previous_stage_stores.mask.set();
                vrt.convert_depth_mode = o.depth != 0;
                // (the fragment stage translated first, as vk_shader.cpp translate_into: its noperspective inputs are written
                // times Position.w by the vertex stage and interpolated perspective-correct, the AMD clipped-vertex workaround;
                // a bank baked without it would keep the old interpolation)
                std::unique_ptr<Translation> ft;
                Shader::RuntimeInfo frt{};
                if (by_stage[1]) {
                    OfflineEnv fenv{*by_stage[1], Shader::Stage::Fragment};
                    ft = translate(fenv, true);
                    questions += fenv.texture_questions;
                    if (!ft->program.info.loads_indexed_attributes && vrt.xfb_count == 0) {
                        for (size_t i = 0; i < ft->program.info.interpolation.size(); ++i)
                            if (ft->program.info.interpolation[i] == Shader::Interpolation::NoPerspective)
                                vrt.noperspective_outputs.set(i);
                        frt.scaled_noperspective_inputs = vrt.noperspective_outputs.any();
                    }
                }
                StageResult v;
                v.info = t->program.info;
                v.nvn_stage = 0;
                v.c1 = by_stage[0]->c1;
                Shader::Backend::Bindings bindings{};
                v.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), vrt, t->program, bindings);
                if (v.spirv.empty()) err = "vertex EmitSPIRV returned no words";
                v.vk_stage = kVkVertex;
                pr.stages.push_back(std::move(v));
                if (err.empty() && ft) {
                    frt.previous_stage_stores = t->program.info.stores;
                    for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(no_types[i]);
                    uint32_t float_targets = 0;
                    for (int i = 0; i < 8; i++)
                        if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
                    static const bool nan_guard = !std::getenv("RX_VK_NAN_GUARD") || std::atoi(std::getenv("RX_VK_NAN_GUARD")) != 0;
                    if (nan_guard) Shader::Optimization::GuardFragmentOutputs(ft->program, float_targets);
                    StageResult f;
                    f.info = ft->program.info;
                    f.nvn_stage = 1;
                    f.c1 = by_stage[1]->c1;
                    Shader::Backend::Bindings fbindings{};
                    f.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), frt, ft->program, fbindings);
                    if (f.spirv.empty()) err = "fragment EmitSPIRV returned no words";
                    f.vk_stage = kVkFragment;
                    pr.stages.push_back(std::move(f));
                }
            }
            ComputeTry base;
            if (compute) {
                // (with answer variants on, even the baseline runs with the slot tokens, as live's
                // seed_compute does with an empty want map: the questions it asks are the variant space)
                const uint8_t* ctl5 = nullptr;
                for (const uint32_t k : seed.programs[pi])
                    if ((seed.stages[k].stage & 7) == 5) ctl5 = seed.blob.data() + seed.stages[k].control;
                const CsLayout base_cs = cs_layout_of(ctl5 + 0x58);
                Forced base_forced;
                base_forced.lo = base_cs.tex_at();
                base_forced.hi = base_cs.end();
                compute_try(o.target.integer_variants ? &base_forced : nullptr, base);
                err = base.err;
                questions = base.questions;
                cs = base.cs;
            }
            if (!err.empty()) {
                failed++;
                std::fprintf(report,
                             "{\"program\":%zu,\"key\":\"%s\",\"kind\":\"%s\",\"variant\":%d,\"status\":\"translate-failed\","
                             "\"what\":\"%s\"}\n",
                             pi, hex64(key).c_str(), compute ? "compute" : mesh ? "mesh" : "graphics", av.index, json_escape(err).c_str());
                continue;
            }
            if (compute) {
                // (the per-key seen set: live's seed_compute re-reads the key's records from the bank at entry,
                // so it is shared by every program with that key; this map is the same set across the bake)
                std::unordered_set<uint64_t>& seen = kept_variants[key];
                emit_slot(base.pr, key, av.index, 0, base.cs, base.questions, &seen);
                // (each answer variant is one more record of the same key, told apart by its answers hash, as
                // live keeps them: program_cache_load loads every record of a key and still_valid picks by the
                // bound answers, vk_shader.cpp:669-677)
                if (o.target.integer_variants && o.variant_cap > 1) {
                    int ordinal = 0;
                    for (ComputeTry& v : answer_variants(base))
                        emit_slot(v.pr, key, av.index, ++ordinal, v.cs, v.questions, &seen);
                }
            } else {
                emit_slot(pr, key, av.index, 0, cs, questions, nullptr);
            }
        }
    }
    if (bank) std::fclose(bank);
    std::fclose(report);
    FILE* mf = std::fopen((o.out + "/bake_manifest.json").c_str(), "w");
    if (mf) {
        std::string env_json;
        for (size_t k = 0; k < o.target.env.size(); k++)
            env_json += (k ? "," : "") + std::string("\"") + json_escape(o.target.env[k]) + "\"";
        // (the plan this bank needs: the switches the game must run with, and the stamp they produce)
        std::fprintf(mf,
                     "{\n  \"tool\": \"bake_ds\",\n  \"seed\": \"%s\",\n  \"seed_stages\": %zu,\n  \"seed_programs\": %zu,\n"
                     "  \"program_slots\": %zu,\n  \"translated\": %zu,\n  \"failed\": %zu,\n  \"skipped\": %zu,\n"
                     "  \"target\": \"%s\",\n  \"tensor\": \"%s\",\n  \"cooperative_matrix_class\": %s,\n"
                     "  \"integer_variants\": %s,\n  \"variant_cap\": %d,\n  \"duplicate_variants\": %zu,\n"
                     "  \"mma_software\": %s,\n  \"feature_env\": [%s],\n"
                     "  \"cache_stamp\": \"%s\",\n  \"translator_stamp\": \"%s\",\n  \"depth_mode\": %u,\n"
                     "  \"sizeof_info\": %zu,\n  \"sizeof_program_layout\": %zu,\n  \"ssbo_alignment\": %llu,\n  \"bank\": %s\n}\n",
                     json_escape(o.seed).c_str(), seed.stages.size(), seed.programs.size(), program_slots, translated, failed,
                     skipped, json_escape(o.target.name).c_str(), tensor_mode_name(o.target.tensor).c_str(),
                     o.target.cooperative_matrix ? "true" : "false", o.target.integer_variants ? "true" : "false",
                     o.variant_cap, duplicate_variants, Shader::Backend::SPIRV::mma_software ? "true" : "false",
                     env_json.c_str(), hex64(disk).c_str(), hex64(o.stamp == ~0ull ? 0 : o.stamp).c_str(),
                     o.depth, sizeof(Shader::Info), sizeof(ProgramLayout), static_cast<unsigned long long>(g_ssbo_align),
                     o.bank ? "\"hookshot_programs.bin\"" : "null");
        std::fclose(mf);
    }
    std::fprintf(stderr, "bake_ds: %zu slots: %zu translated, %zu duplicate answer variants dropped, %zu failed, %zu programs skipped (unreadable/mesh)\n",
                 program_slots, translated, duplicate_variants, failed, skipped);
    return failed ? 1 : 0;
}

} // namespace

// ---- device query (--target query): vulkaninfo-style enumeration, no logical device ----
// Creates an instance and reads physical-device properties/features/extensions only: no vkCreateDevice, no
// queue, no dispatch. That is enough to pick a target (the device's minimum storage-buffer offset alignment
// is the profile-wide number the SPIR-V masks are baked with) and to record the class of device a bank is for.
int run_query() {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "query: no Vulkan loader (libvulkan.so.1)\n");
        return 4;
    }
    const auto sym = [lib](const char* name) { return dlsym(lib, name); };
    auto create = reinterpret_cast<PFN_vkCreateInstance>(sym("vkCreateInstance"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(sym("vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(sym("vkEnumeratePhysicalDevices"));
    auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(sym("vkGetPhysicalDeviceProperties2"));
    auto feats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(sym("vkGetPhysicalDeviceFeatures2"));
    auto exts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(sym("vkEnumerateDeviceExtensionProperties"));
    if (!create || !destroy || !enumerate || !props2 || !feats2 || !exts) {
        std::fprintf(stderr, "query: the Vulkan loader lacks the 1.1 entry points\n");
        return 4;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bake_ds";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&ci, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "query: vkCreateInstance failed\n");
        return 4;
    }
    uint32_t count = 0;
    if (enumerate(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        std::fprintf(stderr, "query: no Vulkan physical devices\n");
        destroy(instance, nullptr);
        return 4;
    }
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(instance, &count, devices.data());
    for (const VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2(dev, &p);
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        f11.pNext = &f12;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f11;
        feats2(dev, &f2);
        uint32_t next = 0;
        exts(dev, nullptr, &next, nullptr);
        std::vector<VkExtensionProperties> list(next);
        if (next) exts(dev, nullptr, &next, list.data());
        bool cooperative_matrix = false;
        for (const auto& e : list) cooperative_matrix |= !std::strcmp(e.extensionName, "VK_KHR_cooperative_matrix");
        std::printf("{\"device\": \"%s\", \"api\": \"%u.%u.%u\", \"driver\": %u, \"vendor\": %u, \"device_id\": %u, "
                    "\"type\": %u, \"ssbo_align\": %u, \"max_compute_shared\": %u, \"float16\": %s, "
                    "\"storage16\": %s, \"cooperative_matrix\": %s}\n",
                    json_escape(p.properties.deviceName).c_str(), VK_API_VERSION_MAJOR(p.properties.apiVersion),
                    VK_API_VERSION_MINOR(p.properties.apiVersion), VK_API_VERSION_PATCH(p.properties.apiVersion),
                    p.properties.driverVersion, p.properties.vendorID, p.properties.deviceID,
                    static_cast<unsigned>(p.properties.deviceType),
                    p.properties.limits.minStorageBufferOffsetAlignment, p.properties.limits.maxComputeSharedMemorySize,
                    f12.shaderFloat16 ? "true" : "false", f11.storageBuffer16BitAccess ? "true" : "false",
                    cooperative_matrix ? "true" : "false");
    }
    destroy(instance, nullptr);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string_view cmd = argv[1];
    if (cmd == "query") return run_query();
    if (cmd != "verify" && cmd != "translate") return usage();
    Options o;
    const char* bank_arg = nullptr;
    std::string target_name = "plain";
    std::string feature_list;
    bool target_given = false;
    for (int i = 2; i < argc; i++) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seed") o.seed = next();
        else if (a == "--out") o.out = next();
        else if (a == "--bank") bank_arg = next();
        else if (a == "--stamp") o.stamp = std::strtoull(next(), nullptr, 16);
        else if (a == "--depth") o.depth = static_cast<uint32_t>(std::atoi(next()));
        else if (a == "--target") target_name = next(), target_given = true;
        else if (a == "--feature") feature_list = next(), target_given = true;
        else if (a == "--variants") {
            const std::string_view v = next();
            o.variants = v == "all" ? -1 : std::atoi(std::string(v).c_str());
        } else if (a == "--limit") o.limit = std::atoi(next());
        else if (a == "--no-bank") o.bank = false;
        else if (a == "--program-size") o.program_size = std::atol(next());
        else if (a == "--ssbo-align") o.ssbo_align = std::strtoull(next(), nullptr, 0);
        else if (a == "--variant-cap") o.variant_cap = std::max(1, std::atoi(next()));
        else if (a == "--spv-dir") o.spv_dir = next();
        else return usage();
    }
    const auto preset = target_preset(target_name);
    if (!preset) {
        std::fprintf(stderr, "bake_ds: unknown target '%s' (plain, tensor, tensor-software, rdna2, gfx1201)\n",
                     target_name.c_str());
        return 2;
    }
    o.target = *preset;
    if (!feature_list.empty() && !apply_features(o.target, feature_list)) {
        std::fprintf(stderr, "bake_ds: unknown --feature in '%s'\n", feature_list.c_str());
        return 2;
    }
    export_target_env(o.target);
    if (cmd == "verify") {
        if (!bank_arg) return usage();
        // (no --target: read the bank as any profile, as before; with one, also check the stamp)
        return verify_bank(bank_arg, target_given ? &o.target : nullptr, o.stamp == ~0ull ? 0 : o.stamp, o.depth);
    }
    if (o.seed.empty() || o.out.empty()) return usage();
    return run_translate(o);
}
