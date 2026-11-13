// NVN2 programs to SPIR-V with Eden's shader recompiler (gpu/vk/eden). A stage's package is its control section
// (guest memory, 0xd8 bytes: magic 0x98761234, code length +0x38, c1 constants +0x40..+0x44, stage +0x56, workgroup
// size +0x58) and its code area (GPU memory: a graphics stage's 128-byte header, then the Ampere instructions, then
// the c1 constants). The instructions are lowered to Maxwell (frontend/ampere) and translated like any Maxwell
// program, its questions answered from the NVN state: c0 / c2 from the driver banks, c1 from the code area, user
// uniform buffers (c3-c18: slots 0-15) from guest memory, texture types from the registered textures. As the WebGPU
// renderer's gpu/render/rx_shader.cpp does for suyu's recompiler, whose comments say more.
//
// A program's stages are emitted with one Backend::Bindings, so their descriptors share one set in Eden's order
// (per stage: uniform buffers, storage buffers, texture buffers, image buffers, textures, images), which
// vk_pipeline.cpp lays out from the stages' Info the same way.
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#endif
#include <io.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <cstdio>
#include <functional>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/ir_opt/fragment_output_guard.h"
#include "shader_recompiler/ir_opt/scene_tonemap_pass.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "vk_paths.h"
#include "vk_internal.h"
#include "vk_jobs.h"
#include "vk_draw.h"
#include "vk_progcache.h"
#include "vk_menu.h"

// (EvilOOT, RX_VK_PTR_DIAG) the pointer pass's counts for this thread's translations
// (global_memory_to_storage_buffer_pass.cpp, declared by hand so no Eden header changes)
namespace Shader::Optimization {
struct PointerDiag {
    uint32_t tracked, fallback, fallback_table, mixed, split, fallback_split;
};
extern thread_local PointerDiag g_pointer_diag;
// (collect_shader_info_pass.cpp: NVN2 compute's storage buffer table for the program translated on this thread, and
// where its c0 layout ends: a pointer table at 0x880 when it ends before)
extern thread_local uint32_t g_nvn2_compute_ssbo_base;
extern thread_local uint32_t g_nvn2_compute_ssbo_count;
extern thread_local uint32_t g_nvn2_compute_layout_end;
extern thread_local bool g_nvn2_miss_log_this;
extern thread_local bool g_all_global_this;
extern thread_local bool g_gm_port_this;
}
namespace mkwvk {
struct NvnState;
uint32_t compute_ssbo_table(const NvnState& s, uint32_t* count, uint32_t* layout_end);   // (vk_bank.cpp)
bool seed_cs_buffer_word(uint32_t offset);   // (vk_bank.cpp)
bool key_listed(const char* hex, const char* var);   // (below)
}
namespace {
bool ptr_diag_on() {
    static const bool on = getenv("RX_VK_PTR_DIAG") && atoi(getenv("RX_VK_PTR_DIAG")) != 0;
    return on;
}
void ptr_diag_reset() { Shader::Optimization::g_pointer_diag = {}; }
/// One line a program whose global memory accesses are not all plain constant buffer pointers
void ptr_diag_log(uint64_t key, const char* kind) {
    if (!ptr_diag_on()) return;
    const auto& d = Shader::Optimization::g_pointer_diag;
    if (!d.fallback && !d.mixed && !d.split) return;
    mkwvk::logf("vk: pointers %s %016llx: %u bound (%u with memory in the address, %u SPLIT), %u run-time search (%u through a table, "
         "%u split)",
         kind, (unsigned long long)key, d.tracked, d.mixed, d.split, d.fallback, d.fallback_table, d.fallback_split);
}
} // namespace
#include "vk_shader.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif

namespace mkwvk {
namespace {

constexpr uint32_t kControlMagic = 0x98761234;
constexpr uint32_t kControlSize = 0xd8;
constexpr uint32_t kAmpereHeader = 128;

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/// A stage's package, read from guest memory and lowered to Maxwell
struct Code {
    int stage = -1;   // NVN stage: 0 vertex, 1 fragment, 5 compute
    uint64_t key = 0;
    std::vector<uint64_t> code;   // header (0x50 bytes, zeros for compute), Maxwell instructions, native address map
    std::vector<uint8_t> c1;
    uint32_t local[3]{};
    uint32_t shared_memory = 0;   // compute: bytes from the NVN2 control section (+0x64; klimovich008's PR 91)
    // (hookshot) compute: local memory, bytes a thread, from the control section (+0x68; +0x50 is the same times 32, a warp's).
    // It was never read: a compute program that spills (LDL/STL) had no local array, and the SPIR-V back end aborted the game
    // at its first local access. OoT's two largest compute programs have 0xd80
    uint32_t local_memory = 0;
    uint32_t texture_bound = ~0u;   // the bank native TEX instructions name (~0: none; TEXS take the stage's own)
    bool isbe = false;              // an NVN2 task (stage 6) or mesh (stage 7) program: it writes the internal stage buffer
    // (task / mesh: control section +0x58 threads 15:0, payload bytes 31:16; +0x60 max primitives 15:0, max vertices
    // 31:16; +0x5f the output topology, 1 points, 3 triangles)
    uint32_t mesh_threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0;
    bool header_only = false;       // a graphics header and no code (NVN2's per-primitive output map of a mesh program)
    // (hookshot) a fragment stage with the game's tone map fit's five constants in its code: a scene mapping program, which
    // PsychoV35 may take (scene_tonemap_for)
    bool scene_fit = false;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    bool ok = false;
    std::string why;   // (hookshot) not ok: why, for the log ("1 failed" alone said nothing of which program, or why)
};

/// Whether this is a control section, and how much code it says there is
bool control_end(const uint8_t* ctl, uint32_t* end_out) {
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
uint64_t code_key(const uint8_t* ctl, const uint8_t* area, uint32_t end) {
    return hash_bytes(area, end, hash_bytes(ctl, kControlSize));
}
Code parse_code(const uint8_t* ctl, const std::vector<uint8_t>& area);

Code read_code(const nvn_stage_data& sd) {
    uint8_t ctl[kControlSize];
    uint32_t end = 0;
    if (!sd.control || !sd.data || !host->gpu_read(sd.control, ctl, sizeof ctl) || !control_end(ctl, &end)) return Code{};
    std::vector<uint8_t> area(end);
    if (!host->gpu_read(sd.data, area.data(), end)) return Code{};
    if (const char* dir = getenv("RX_VK_DUMP_STAGES")) {   // (hookshot, RX_VK_DUMP_STAGES=<folder>: control and code as the game has them)
        char path[512];
        snprintf(path, sizeof path, "%s/%016llx.bin", dir, (unsigned long long)code_key(ctl, area.data(), end));
        if (FILE* f = fopen(path, "wb")) fwrite(ctl, 1, sizeof ctl, f), fwrite(area.data(), 1, end, f), fclose(f);
    }
    return parse_code(ctl, area);
}

/// A control section and its code, from the game's memory or the archives (the seed)
Code parse_code(const uint8_t* ctl, const std::vector<uint8_t>& area) {
    Code c;
    uint32_t len, c1o;
    const uint32_t end = (uint32_t)area.size();
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    c.key = code_key(ctl, area.data(), end);
    c.stage = st;
    if (st == 1) {
        static constexpr float kFit[] = {2.51f, 0.03f, 2.43f, 0.59f, 0.14f};
        c.scene_fit = true;
        for (const float f : kFit) {
            uint8_t bits[4];
            std::memcpy(bits, &f, 4);
            c.scene_fit = c.scene_fit && std::search(area.begin(), area.end(), bits, bits + 4) != area.end();
        }
    }
    if (st == 6 || st == 7) {
        uint32_t w58, w60;
        std::memcpy(&w58, ctl + 0x58, 4), std::memcpy(&w60, ctl + 0x60, 4);
        c.isbe = true;
        c.mesh_threads = w58 & 0xFFFF, c.payload_bytes = w58 >> 16;
        c.max_primitives = w60 & 0xFFFF, c.max_vertices = w60 >> 16;
        c.topology = ctl[0x5f];
    }
    const bool graphics = st != 5;
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    if (graphics) std::memcpy(c.sph, area.data(), std::min<size_t>(sizeof c.sph, area.size()));
    if (graphics && len <= hdr + 16) {   // (a header alone)
        c.header_only = true;
        c.ok = true;
        return c;
    }
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; hdr + (i + 1) * sizeof(Instruction) <= len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area.data() + hdr + i * sizeof(Instruction), sizeof insn);
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            char why[96];
            snprintf(why, sizeof why, "the branch at instruction %u (+%#x) goes outside the program", i,
                     (unsigned)(i * sizeof(Instruction)));
            c.why = why;
            return c;
        }
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    {   // (hookshot, for bisecting a bad translation) RX_VK_REJECT_STAGES=<n,...> (NVN stages) and RX_VK_REJECT_KEYS=
        // <hex prefix,...>: those programs are taken as untranslatable, as a lowering failure leaves them
        static const std::string stages = getenv("RX_VK_REJECT_STAGES") ? getenv("RX_VK_REJECT_STAGES") : "";
        // (none by default: OoT's mesh program 32423793502c71e7 stayed out while it lost the device; it no longer does)
        static const std::string keys = getenv("RX_VK_REJECT_KEYS") && strcmp(getenv("RX_VK_REJECT_KEYS"), "-") ? getenv("RX_VK_REJECT_KEYS") : "";
        bool reject = false;
        for (size_t at = 0; !reject && at < stages.size();) {
            const size_t e = stages.find(',', at);
            const std::string t = stages.substr(at, e == std::string::npos ? std::string::npos : e - at);
            reject = !t.empty() && (unsigned)atoi(t.c_str()) == st;
            at = e == std::string::npos ? stages.size() : e + 1;
        }
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)c.key);
        for (size_t at = 0; !reject && at < keys.size();) {
            const size_t e = keys.find(',', at);
            const std::string t = keys.substr(at, e == std::string::npos ? std::string::npos : e - at);
            reject = !t.empty() && !strncmp(hex, t.c_str(), t.size());
            at = e == std::string::npos ? keys.size() : e + 1;
        }
        if (reject) return c.why = "left out by RX_VK_REJECT_STAGES or RX_VK_REJECT_KEYS", c;
    }
    // (compute only: the host's cooperative matrices are, vk_coopmat.cpp)
    // (hookshot) and only for the programs in RX_VK_NATIVE_MMA_KEYS (key prefixes, default the gameplay TAAU network,
    // whose output vk_draw.cpp's stand-in replaces; "all": every one). The others take the exact expansion: with the
    // cooperative matrices the movie upscaler lost its blue (the title's clip yellow) though the device's fragment
    // layout passes vk_coopmat.cpp's multiply; exact, its colours are right at the same frame rate
    // (default "all": a program left off it is expanded only with EDEN_NX2_TENSOR set, and is otherwise not lowered
    // at all; the per-key default dropped OoT's other gameplay networks, e409733c and others)
    static const std::string native_keys = getenv("RX_VK_NATIVE_MMA_KEYS") ? getenv("RX_VK_NATIVE_MMA_KEYS") : "all";
    bool native_key = native_keys == "all";
    {
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)c.key);
        for (size_t at = 0; !native_key && at < native_keys.size();) {
            const size_t e = native_keys.find(',', at);
            const std::string t = native_keys.substr(at, e == std::string::npos ? std::string::npos : e - at);
            native_key = !t.empty() && !strncmp(hex, t.c_str(), t.size());
            at = e == std::string::npos ? native_keys.size() : e + 1;
        }
    }
    Shader::Ampere::native_matrix_multiply = dev.native_mma && !graphics && native_key;
    {   // (super build) RX_VK_LOWER_SKIP=<key prefix,...>: those programs left unlowered, as if they did not lower (a bisection)
        static const std::string skip = getenv("RX_VK_LOWER_SKIP") ? getenv("RX_VK_LOWER_SKIP") : "";
        if (!skip.empty()) {
            char hex[24];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)c.key);
            for (size_t at = 0; at < skip.size();) {
                const size_t e = skip.find(',', at);
                const std::string t = skip.substr(at, e == std::string::npos ? std::string::npos : e - at);
                if (!t.empty() && !strncmp(hex, t.c_str(), t.size())) return c;
                at = e == std::string::npos ? skip.size() : e + 1;
            }
        }
    }
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    if (lowered && lowered.register_map_words) {   // (hookshot) which programs run with extended registers
        static std::atomic<int> logged{0};
        if (logged++ < 64)
            logf("vk: NVN2 program %016llx (stage %u) lowered with extended registers (%u REGMAP words)",
                 (unsigned long long)c.key, st, lowered.register_map_words);
    }
    if (const char* want = getenv("RX_VK_DUMP_NATIVE")) {   // (hookshot) <key prefix,...>: native code of those programs
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)c.key);
        for (const char* q = want; *q;) {
            const char* e = q;
            while (*e && *e != ',') e++;
            if (e > q && !strncmp(hex, q, (size_t)(e - q))) {
                char path[64];
                snprintf(path, sizeof path, "native_%u_%s.bin", st, hex);
                if (FILE* f = fopen(path, "wb")) {
                    fwrite(native.data(), sizeof(Instruction), native.size(), f);
                    fclose(f);
                }
            }
            q = *e ? e + 1 : e;
        }
    }
    if (!lowered) {   // (hookshot: each program once, up to 400, with the native opcode it stopped at)
        static std::mutex mu;
        static std::unordered_set<uint64_t> seen;
        std::lock_guard lk(mu);
        if (seen.size() < 400 && seen.insert(c.key).second) {
            const uint32_t at = lowered.error_offset / (uint32_t)sizeof(Instruction);
            const uint32_t op = at < native.size() ? (uint32_t)native[at].Bits(0, 12) : 0xFFFFFFFFu;
            logf("vk: NVN2 program %016llx (stage %u) does not lower at +%#x: %s (native op %#x, %08x %08x %08x %08x)", (unsigned long long)c.key,
                 st, lowered.error_offset, std::string(lowered.error).c_str(), op, at < native.size() ? native[at].words[0] : 0,
                 at < native.size() ? native[at].words[1] : 0, at < native.size() ? native[at].words[2] : 0, at < native.size() ? native[at].words[3] : 0);
            // (RX_VK_DUMP_FAILED=<folder>: the native code, for nx2/lowering tests: <stage>_<key>.bin, 16-byte words)
            if (const char* dir = getenv("RX_VK_DUMP_FAILED")) {
                char path[512];
                snprintf(path, sizeof path, "%s/%u_%016llx.bin", dir, st, (unsigned long long)c.key);
                if (FILE* f = fopen(path, "wb")) {
                    fwrite(native.data(), sizeof(Instruction), native.size(), f);
                    fclose(f);
                }
            }
        }
        char why[160];
        snprintf(why, sizeof why, "does not lower at +%#x: %s", lowered.error_offset, std::string(lowered.error).c_str());
        c.why = why;
        return c;
    }
    c.texture_bound = lowered.required_texture_buffer;
    c.code.assign(sizeof(Shader::ProgramHeader) / 8, 0);
    if (graphics) std::memcpy(c.code.data(), area.data(), sizeof(Shader::ProgramHeader));
    for (const uint64_t instruction : lowered.instructions) {
        if (c.code.size() * 8 % 32 == 0) c.code.push_back(0);   // (the Maxwell scheduling word)
        c.code.push_back(instruction);
    }
    c.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(c.code, lowered.source_instruction_offsets);
    c.c1.assign(area.begin() + c1o, area.end());
    if (!graphics) {
        std::memcpy(c.local, ctl + 0x58, 12);
        std::memcpy(&c.shared_memory, ctl + 0x64, 4);
        std::memcpy(&c.local_memory, ctl + 0x68, 4);
        uint32_t warp_local = 0;   // (+0x50: the same times 32, a warp's; a size that does not match it is not trusted)
        std::memcpy(&warp_local, ctl + 0x50, 4);
        if (c.local_memory && (uint64_t{c.local_memory} * 32 != warp_local || c.local_memory > (64u << 10))) {
            static std::atomic<int> told{0};
            if (told++ < 8)
                logf("vk: compute program %016llx: local memory %#x a thread, %#x a warp: not used", (unsigned long long)c.key,
                     c.local_memory, warp_local);
            c.local_memory = 0;
        }
    }
    c.ok = true;
    return c;
}

Shader::TextureType texture_type(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return Shader::TextureType::Color1D;
    case NVN_TEXTURE_TARGET_3D: return Shader::TextureType::Color3D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return Shader::TextureType::ColorArray1D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return Shader::TextureType::ColorArray2D;
    case NVN_TEXTURE_TARGET_RECTANGLE: return Shader::TextureType::Color2DRect;
    case NVN_TEXTURE_TARGET_CUBEMAP: return Shader::TextureType::ColorCube;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return Shader::TextureType::ColorArrayCube;
    case NVN_TEXTURE_TARGET_BUFFER: return Shader::TextureType::Buffer;
    default: return Shader::TextureType::Color2D;
    }
}

} // namespace

Shader::TextureType handle_texture_type(uint32_t handle) {
    nvn_tex t;
    nvn_view v;
    bool has_view = false;
    if (!handle || !texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view)) return Shader::TextureType::Color2D;
    return texture_type(has_view && v.set_target ? v.target : t.d.target);
}

namespace {

bool handle_is_integer(uint32_t handle) {
    nvn_tex t;
    nvn_view v;
    bool has_view = false;
    if (!handle || !texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view)) return false;
    const VkFormat f = vk_format(has_view && v.set_format ? v.format : t.d.format).format;
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_S8_UINT: return true;
    default: return false;
    }
}

// (hookshot) A compute program's texture variants (seed_compute): translated with nothing bound, every texture question of a compute program
// is asked of handle 0, and which slot asks is not known. Under `forced`, c0's texture and image handles read as tokens of their
// slots, each question is noted once by its handle, and the answer wanted for it is given in place of an unbound slot's
struct Forced {
    uint32_t lo = 0, hi = 0;   // c0's texture and image handles (the program's layout)
    std::vector<std::pair<uint32_t, uint8_t>> asked;   // (handle, 0: its type / 1: whether its format is integer), in order
    std::map<std::pair<uint32_t, uint8_t>, uint8_t> want;
    uint8_t answer(uint32_t handle, uint8_t kind, uint8_t plain) {
        const std::pair<uint32_t, uint8_t> q{handle, kind};
        if (std::find(asked.begin(), asked.end(), q) == asked.end()) asked.push_back(q);
        const auto it = want.find(q);
        return it != want.end() ? it->second : plain;
    }
};
thread_local Forced* forced = nullptr;
thread_local std::vector<std::vector<uint32_t>>* seed_capture = nullptr;   // (the translation's SPIR-V, kept here and not on disk)

// (hookshot) The texture questions of a translation, in the order asked: noted in play (Program::questions), given back in that order
// when the pipeline state list makes the same variant before the game, with nothing bound to ask about
thread_local std::vector<uint8_t>* questions_out = nullptr;
thread_local const std::vector<uint8_t>* questions_in = nullptr;
thread_local size_t questions_at = 0;
uint8_t question_answer(uint8_t live) {
    if (questions_in) return questions_at < questions_in->size() ? (*questions_in)[questions_at++] : live;
    if (questions_out) questions_out->push_back(live);
    return live;
}
struct QuestionsNoted {
    explicit QuestionsNoted(Program& p) {
        p.questions.clear();
        if (questions_in) p.questions = *questions_in;
        else questions_out = &p.questions;
    }
    ~QuestionsNoted() { questions_out = nullptr; }
};
thread_local const std::vector<Program::Answer>* answers_in = nullptr;   // (and the variant's answers then)

class Env final : public Shader::Environment {
public:
    Env(const NvnState& state, const Code& c, Shader::Stage st) : s{state}, nvn{c} {
        stage = st;
        start_address = st == Shader::Stage::Compute ? static_cast<u32>(sizeof(Shader::ProgramHeader)) : 0;
        if (st != Shader::Stage::Compute) {
            std::memcpy(&sph, reinterpret_cast<const uint8_t*>(nvn.code.data()), sizeof sph);
            local_memory = static_cast<uint32_t>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        } else {
            local_memory = nvn.local_memory;   // (its control section's: a compute program has no header)
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
            if ((size_t)offset + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
            return v;
        }
        if (forced && index == 0 && nvn.stage == 5 && offset >= forced->lo && offset < forced->hi && !(offset & 7))
            return 0x8000 | offset;   // (a slot's token: nothing is bound in the seed)
        if (index == 0 || index == 2) return driver_bank_word(s, (int)index, nvn.stage, offset);
        if (index >= 3 && index < 3 + 16) {
            const nvn_p_bind_buf& u = s.ubo[nvn.stage][index - 3];
            uint32_t v = 0;
            if (u.addr && (uint64_t)offset + 4 <= (uint64_t)u.size) host->gpu_read(u.addr + offset, &v, 4);
            return v;
        }
        return 0;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        texture_questions++;
        if (forced && handle) return (Shader::TextureType)forced->answer(handle, 0, (uint8_t)Shader::TextureType::Color2D);
        return static_cast<Shader::TextureType>(question_answer(static_cast<uint8_t>(handle_texture_type(handle))));
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat{};   // (only for formatless image loads, which the device has)
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        texture_questions++;
        if (forced && handle) return forced->answer(handle, 1, 0) != 0;
        return question_answer(handle_is_integer(handle) ? 1 : 0) != 0;
    }
    int texture_questions = 0;   // (the variant then depends on the bound textures: Program::answers)
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        if (nvn.texture_bound != ~0u) return nvn.texture_bound;   // (as Eden's GraphicsEnvironment: TEX names it)
        return stage == Shader::Stage::Compute ? 0 : 2;   // (NVN2 compute: handles in c0)
    }
    u32 LocalMemorySize() const override {
        return local_memory;
    }
    u32 SharedMemorySize() const override {
        // (what the program declares, from the control section: 48 KB for every one was past an RX 9070 XT's 32 KB, and its
        // pipelines were invalid. klimovich008, Hookshot PR 91. RX_VK_SHARED_48K=1: as before)
        static const bool old = env_on("RX_VK_SHARED_48K");
        return old ? 48u << 10 : nvn.shared_memory;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {std::max(1u, nvn.local[0]), std::max(1u, nvn.local[1]), std::max(1u, nvn.local[2])};
    }
    // NVN's draw macros put the draw's base vertex, base instance and draw index in the vertex stage's driver bank, at
    // 0x640, 0x644 and 0x648 (Eden's macro.cpp HLE: DrawIndexedInstanced and the indirect forms); a program reads
    // them there for gl_BaseVertex / gl_BaseInstance / gl_DrawID. They are Vulkan's BaseVertex, BaseInstance and
    // DrawIndex, right for indirect draws too. The bank had zeros there: MKW's terrain patches, which pass log2 of
    // their template grid as the base instance (4: 16 x 16, 3: 8 x 8), took a 1 x 1 grid, every vertex on a corner:
    // each patch two triangles, one of them often degenerate (the triangular holes in the far terrain).
    // (RX_VK_NO_DRAW_PARAMS=1: as before)
    bool HasHLEMacroState() const override {
        static const bool off = env_on("RX_VK_NO_DRAW_PARAMS");
        return !off && (stage == Shader::Stage::VertexA || stage == Shader::Stage::VertexB);
    }
    bool HasNativeInstructions() const override { return true; }   // (lowered NVN2 programs: Lichii's passes ask)
    bool IsNativeProgram() const override { return true; }   // (Fritchii's storage buffer pass asks: EDEN_NX2_GM_NATIVE=0 off)
    /// (Fritchii's storage buffer pass) whether the 8 bytes at c<index>+offset are a buffer's address now: in a memory pool
    /// the game made. A texture handle and a float next to it (0x10001c00a, c5+0x60 of OoT's mesh programs) are not; nor is
    /// anything in c1, the program's own constants. Answered once per word (the pass asks per access).
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        if (index == 1) return false;
        // (#940: the seed's compute programs, before anything is bound: c0's buffer tables, as play has them)
        if (seed_cs_control && stage == Shader::Stage::Compute) return index == 0 && seed_cs_buffer_word(offset);
        const uint64_t key = (uint64_t)index << 32 | offset;
        if (const auto it = pointer_answers.find(key); it != pointer_answers.end()) return it->second;
        const uint64_t v = ReadCbufValue(index, offset) | (uint64_t)ReadCbufValue(index, offset + 4) << 32;
        // (4-byte aligned: a pointer the program reads words through. vk_bank.cpp's pointer table asks the same)
        const bool pointer = v && !(v & 3) && host->gpu_guest(v) && gpu_pool_end(v) > v;
        pointer_answers.emplace(key, pointer);
        return pointer;
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
    /// A task or mesh program's SET_MESH_SHADER_A / B and SET_MESH_INIT_SHADER, and (mesh) the header-only stage whose
    /// output map lists its per-primitive outputs (Eden's GraphicsEnvironment reads the pass-through geometry
    /// program's header for that)
    void set_mesh(const std::array<u32, 3>& registers, const uint8_t* primitive_sph) {
        mesh_registers = registers;
        if (primitive_sph) std::memcpy(&mesh_primitive_header, primitive_sph, sizeof mesh_primitive_header);
    }

private:
    const NvnState& s;
    const Code& nvn;
    uint32_t local_memory = 0;
    std::unordered_map<uint64_t, bool> pointer_answers;   // (IsCbufStorageBufferPointer's)
};

Shader::Profile make_profile() {
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
    p.supported_subgroup_stages = VK_SHADER_STAGE_ALL;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_float32_atomic_add = dev.float_atomic_add;   // (one OpAtomicFAddEXT for a float add, not a compare-exchange loop: vk_main.cpp)
    p.support_derivative_control = true;
    p.support_native_ndc = false;   // (the vertex stage converts NVN's -1..1 depth: RuntimeInfo::convert_depth_mode)
    p.support_scaled_attributes = false;   // (*_UI2F / *_I2F read as integers, converted in the shader: R32 has no scaled format)
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    // Unpinned SPIR-V 1.6 stages can use any supported subgroup size, even when the default is 32.
    // The stage-specific profiles below keep the short path only where pin_subgroup actually pins 32 lanes.
    p.warp_size_potentially_larger_than_guest = dev.subgroup_max > 32;
    // (RX_VK_WARP64=0 or 1, a test: the graphics stages' long warp path off or on, whatever the GPU; in the cache's stamp)
    if (const char* e = getenv("RX_VK_WARP64")) p.warp_size_potentially_larger_than_guest = e[0] == '1';
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    // (the lanes the broadcast fallbacks walk: all of the host's when a warp may be larger than the guest's)
    p.max_subgroup_size = p.warp_size_potentially_larger_than_guest ? dev.subgroup_max : 32;
    // (Lichii #273) AMD's Windows compiler gets fragment-stage subgroup shuffles wrong (a basket in Link's house drew as a white
    // blob): broadcasts and selects there instead
    p.has_broken_spirv_fragment_shuffle = dev.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY;
    // (RX_VK_FRAG_SHUFFLE_FALLBACK=0 or 1, a test: the fragment stages' shuffles native or through broadcasts, whatever the
    // driver; in the cache's stamp)
    if (const char* e = getenv("RX_VK_FRAG_SHUFFLE_FALLBACK")) p.has_broken_spirv_fragment_shuffle = e[0] == '1';
    // (hookshot) gathers nudged by 1/512 of a texel on AMD's and Intel's drivers, as Eden and Lichii have it (vk_vendor.h; in the
    // cache's stamp)
    p.need_gather_subpixel_offset = dev.fixes.gather_offset;
    p.min_ssbo_alignment = storage_binding_align();   // (at least 16: vk_internal.h)
    return p;
}

Shader::HostTranslateInfo host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    // (RX_VK_NO_F16, a test: the game's f16 math lowered to f32, as a card without f16 shaders would need (GTX 10xx, issue 98). Eden's
    // lowering made a picture of noise of Kokiri on an RTX card, so it is not used by itself)
    // (k10) and only where the GPU runs f16 shaders: RADV gives Polaris (RX 400/500) none (shaderFloat16 false), and f16 SPIR-V
    // there drew Kokiri Forest as coloured noise on an RX 580. Eden's lowering to f32 is what Pascal takes as well
    h.support_float16 = !env_on("RX_VK_NO_F16") && dev.vk12.shaderFloat16;
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = storage_binding_align();
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;   // (barriers in loops kept: see the WebGPU renderer's 4f5e3e6)
    return h;
}

/// The pools a translation's IR lives in, with small block chunks (the default chunk of 8192 IR::Blocks of 8 KB each
/// committed 68 MB a stage)
struct Pools {
    Shader::ObjectPool<Shader::IR::Inst> insts{8192};
    Shader::ObjectPool<Shader::IR::Block> blocks{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks{32};
};
/// (each thread's pools of its finished translations, emptied, for its next ones: fresh pages were 20-30% of a stage's
/// translation time. Room for three, the most alive at once: task, mesh and fragment)
std::vector<std::unique_ptr<Pools>>& spare_pools() {
    static thread_local std::vector<std::unique_ptr<Pools>> spare = [] {
        std::vector<std::unique_ptr<Pools>> v;
        v.reserve(3);
        return v;
    }();
    return spare;
}
/// (a released pool keeps the size of the largest stage it held for the thread's life: one bigger than this is freed)
constexpr size_t kSparePoolBytes = 8u << 20;

/// A translated stage: its IR program (kept for the next stage's inputs) and the pools its IR lives in
struct Translation {
    std::unique_ptr<Pools> pools;
    Shader::IR::Program program;
    Translation() {
        auto& spare = spare_pools();
        if (spare.empty()) {
            pools = std::make_unique<Pools>();
        } else {
            pools = std::move(spare.back());
            spare.pop_back();
        }
    }
    ~Translation() {
        size_t bytes = program.blocks.size() * (sizeof(Shader::IR::Block) + sizeof(Shader::Maxwell::Flow::Block));
        for (const auto* block : program.blocks) bytes += block->Instructions().size() * sizeof(Shader::IR::Inst);
        program = {};
        if (bytes > kSparePoolBytes) return;
        pools->flow_blocks.ReleaseContents();
        pools->blocks.ReleaseContents();
        pools->insts.ReleaseContents();
        spare_pools().push_back(std::move(pools));
    }
};

std::unique_ptr<Translation> translate(Env& env, bool graphics, bool one_exit = false) {
    auto t = std::make_unique<Translation>();
    // (NVN2 task / mesh programs get one exit: their counts are set there, as Eden's pipeline cache does)
    Shader::Maxwell::Flow::CFG cfg{env, t->pools->flow_blocks,
                                   env.StartAddress() + (graphics ? static_cast<u32>(sizeof(Shader::ProgramHeader)) : 0u),
                                   one_exit};
    t->program = Shader::Maxwell::TranslateProgram(t->pools->insts, t->pools->blocks, env, cfg, host_info());
    return t;
}

} // namespace

VkShaderModule make_module(const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev.device, &ci, nullptr, &m);
    return m;
}

/// A word of a stage's constant bank: c0 / c2 the driver's banks, c1 its own constants, c3.. its uniform buffers
uint32_t stage_cbuf_word(const NvnState& s, const Stage& st, uint32_t index, uint32_t offset) {
    if (index == 1) {
        uint32_t v = 0;
        if ((size_t)offset + 4 <= st.c1.size()) std::memcpy(&v, st.c1.data() + offset, 4);
        return v;
    }
    if (index == 0 || index == 2) return driver_bank_word(s, (int)index, st.nvn_stage, offset);
    if (index >= 3 && index < 3 + 16 && st.nvn_stage >= 0 && st.nvn_stage < kNvnStages) {
        const nvn_p_bind_buf& u = s.ubo[st.nvn_stage][index - 3];
        uint32_t v = 0;
        // (as the stream's place sees it, not the game's memory now: that may be its next frames' already, and a
        // handle read from it sampled their textures, for one frame: MKW's shadows in bands. RX_VK_HANDLES_LIVE=1:
        // as before)
        static const bool live = env_on("RX_VK_HANDLES_LIVE");
        // (EvilOOT: and a binding of no recorded size read as far as asked, as the storage buffer descriptors had it before)
        if (u.addr && (!u.size || (uint64_t)offset + 4 <= (uint64_t)u.size)) {
            if (live) host->gpu_read(u.addr + offset, &v, 4);
            else ub_read(u.addr + offset, &v, 4);
        }
        return v;
    }
    return 0;
}

uint32_t stage_handle(const NvnState& s, const Stage& st, uint32_t program) {
    if (program >= st.info.handle_programs.size()) return 0;
    return Shader::EvaluateHandleProgram(
               st.info.handle_programs[program],
               [&](u32 index, u32 offset) -> u32 { return stage_cbuf_word(s, st, index, offset); },
               [&](u32 binding, u32 offset) -> u32 {
                   if (binding >= st.info.storage_buffers_descriptors.size()) return 0U;
                   const auto& d = st.info.storage_buffers_descriptors[binding];
                   const uint64_t base = stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset) |
                                         (uint64_t)stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset + 4) << 32;
                   uint32_t v = 0;
                   static const bool live = env_on("RX_VK_HANDLES_LIVE");
                   if (base) {
                       if (live) host->gpu_read(base + offset, &v, 4);
                       else ub_read(base + offset, &v, 4);
                   }
                   return v;
               })
        .value_or(0U);
}

namespace {

/// Whether the textures a variant was translated for still have its types (each texture descriptor's handle word)
/// The handle a texture descriptor reads (its primary word, ORed with the secondary one as Eden's TexturePair does;
/// a handle the shader reads from a storage buffer, its handle program's)
uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::TextureDescriptor& d) {
    if (d.handle_program != ~0U) return stage_handle(s, st, d.handle_program);
    uint32_t h = stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset);
    if (d.has_secondary)
        h = h << d.shift_left | stage_cbuf_word(s, st, d.secondary_cbuf_index, d.secondary_cbuf_offset)
                                    << d.secondary_shift_left;
    return h;
}
/// (hookshot) a storage image descriptor's handle word (its handle program's, read from a storage buffer)
uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::ImageDescriptor& d) {
    if (d.handle_program != ~0U) return stage_handle(s, st, d.handle_program);
    return stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset);
}
uint8_t texture_answer(uint32_t handle) {
    if (!handle) return 0xFF;
    // (by the texture slot, until a slot is registered anew: variant checks ask on every draw)
    // (hookshot, fps: a flat table by slot, not a hash map: every draw's variant check asks this for each texture it samples,
    // and the map's lookups were ~16% of the back end's time on the replay thread. Slots are the texture pool's indices, ~33k in OoT)
    struct Known { uint32_t version; uint8_t answer, set; };
    static thread_local std::vector<Known> known;
    const uint32_t id = handle & 0xFFFFF;
    const uint32_t version = texture_slot_version((int)id);
    if (id >= known.size()) known.resize(std::min<size_t>(std::max<size_t>(id + 1, known.size() * 2), 1u << 20));
    Known& k = known[id];
    if (k.set && k.version == version) return k.answer;
    const uint8_t a = (uint8_t)((uint8_t)handle_texture_type(handle) | (handle_is_integer(handle) ? 0x80 : 0));
    k = {version, a, 1};
    return a;
}
/// What the bound textures answer now, for a program whose translation asked about them
// (hookshot) storage images too: whether the bound image's format is integer is baked into the translation (Eden's
// is_integer, the SPIR-V image's sampled type). OoT writes R11G11B10 and RGBA8 targets through R32UI views of them,
// packing the bits itself; a variant translated while that slot held a float texture wrote the packed words as
// floats, which the R32UI view converted: the scene went NaN and yellow before the TAAU
/// (hookshot) whether every answer names a stage of the program and a slot of that stage: answers come from the program
/// cache and the pipeline state list, files that are passed around, and index the stages' descriptor lists
} // namespace
bool answers_fit(const Program& p, const std::vector<Program::Answer>& answers) {
    for (const Program::Answer& a : answers) {
        if (a.stage >= p.stages.size()) return false;
        const auto& info = p.stages[a.stage].info;
        if (a.desc >= (a.image ? info.image_descriptors.size() : info.texture_descriptors.size())) return false;
    }
    return true;
}
namespace {
uint32_t answer_handle(const NvnState& s, const Program& p, const Program::Answer& a) {
    const Stage& st = p.stages[a.stage];
    return a.image ? descriptor_handle(s, st, st.info.image_descriptors[a.desc])
                   : descriptor_handle(s, st, st.info.texture_descriptors[a.desc]);
}
uint8_t answer_of(uint32_t handle, bool image) {
    return image ? (uint8_t)(handle ? texture_answer(handle) & 0x80 : 0xFF) : texture_answer(handle);
}
// Only ignore an answer bit when the translated module already has the corresponding type.
// Keep the entries themselves: handle/version tracking and binding-stage masks still use them.
uint8_t canonical_answer(const Program& p, const Program::Answer& a, uint8_t type) {
    static const bool strict = [] {
        const char* capture = getenv("RX_VK_CAPTURE");
        return env_on("RX_VK_HANDLES_LIVE") || env_on("RX_VK_REGISTER_AT_ONCE") ||
               (capture && atoi(capture) == 0);
    }();
    if (strict) return type;
    const auto& info = p.stages[a.stage].info;
    if (a.image) {
        const auto& d = info.image_descriptors[a.desc];
        if (d.is_integer) {
            switch (d.format) {
            case Shader::ImageFormat::R8_UINT:
            case Shader::ImageFormat::R16_UINT:
            case Shader::ImageFormat::R32_UINT:
            case Shader::ImageFormat::R32G32_UINT:
            case Shader::ImageFormat::R32G32B32A32_UINT:
                return 0x80;
            default:
                break;
            }
        }
    } else {
        const auto& d = info.texture_descriptors[a.desc];
        // (the type is the instruction's own, texture_pass.cpp: whatever is bound there runs the same module through the same
        // view (vk_draw.cpp's view_type(d.type)), so its dimension is no answer. A rectangle and a buffer still are: the pass
        // rewrites an implicit 2D sample and a 1D fetch for them)
        const uint8_t dim = type & 0x7f;
        if (d.type_fixed && dim != (uint8_t)Shader::TextureType::Color2DRect && dim != (uint8_t)Shader::TextureType::Buffer)
            type = (uint8_t)((type & 0x80) | (uint8_t)d.type);
        if (d.is_depth && !d.is_integer) return type & 0x7f;
    }
    return type;
}

}  // namespace
uint64_t answers_hash(const Program& p) {
    uint64_t h = 0;
    for (const Program::Answer& a : p.answers)
        h = (h ^ ((uint64_t)a.stage << 32 | (uint64_t)a.desc << 16 | (uint64_t)a.type << 1 | (uint64_t)a.image)) * 1099511628211ull;
    return h;
}
uint64_t answers_hash_of(const std::vector<Program::Answer>& answers) {   // (hookshot: answers no program holds)
    Program q;
    q.answers = answers;
    return answers_hash(q);
}
// (hookshot) An answer's stage as every GPU numbers it: the mesh fallback's program has a generated vertex stage after its mesh stage
// ((task,) mesh, vertex, fragment), which asks nothing; a native mesh program has none ((task,) mesh, fragment). Counted without it,
// the fragment stage's answers are the same on both, and so is the variant's hash (the pipeline state list, vk_draw.cpp)
namespace {
size_t generated_stage(const Program& p) {
    if (p.fallback)
        for (size_t k = 0; k < p.stages.size(); k++)
            if (p.stages[k].vk_stage == VK_SHADER_STAGE_VERTEX_BIT) return k;
    return SIZE_MAX;
}
}  // namespace
std::vector<Program::Answer> answers_portable(const Program& p) {
    const size_t g = generated_stage(p);
    std::vector<Program::Answer> out = p.answers;
    for (Program::Answer& a : out)
        if (a.stage > g) a.stage--;
    return out;
}
std::vector<Program::Answer> answers_here(const Program& p, std::vector<Program::Answer> portable) {
    const size_t g = generated_stage(p);
    for (Program::Answer& a : portable)
        if (a.stage >= g) a.stage++;
    return portable;
}
uint64_t portable_variant(const Program& p) {
    return generated_stage(p) == SIZE_MAX ? p.variant : answers_hash_of(answers_portable(p));
}
namespace {
/// What an unbound slot answers: the type its instruction declares (record_answers; program_find_plain looks for a variant whose
/// slots all answer it)
uint8_t declared_answer(const Program& p, size_t stage, size_t desc, bool image) {
    const auto& info = p.stages[stage].info;
    if (image) return (uint8_t)(info.image_descriptors[desc].is_integer ? 0x80 : 0);
    const auto& d = info.texture_descriptors[desc];
    return (uint8_t)((uint8_t)d.type | (d.is_integer ? 0x80 : 0));
}
// (an unbound slot answers what the instruction itself says: nothing to tell the variants apart by, and a program prepared before
// the game, with nothing bound at all, comes out as the plain variant of each of its slots)
void record_answers(Program& p, const NvnState& s) {
    p.answers.clear();
    // (the pipeline state list's variant: the answers it was made with, numbered as every GPU numbers its stages: answers_portable)
    std::vector<Program::Answer> here;
    if (answers_in) here = answers_here(p, *answers_in);
    if (answers_in && answers_fit(p, here)) {
        p.answers = std::move(here);
        p.variant = answers_hash(p);
        return;
    }
    for (size_t k = 0; k < p.stages.size(); k++) {
        const auto& list = p.stages[k].info.texture_descriptors;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].count == 1) {
                const uint32_t h = descriptor_handle(s, p.stages[k], list[i]);
                p.answers.push_back({(uint16_t)k, (uint16_t)i, h ? texture_answer(h) : declared_answer(p, k, i, false)});
            }
        const auto& images = p.stages[k].info.image_descriptors;
        for (size_t i = 0; i < images.size(); i++)
            if (images[i].count == 1) {
                const uint32_t h = descriptor_handle(s, p.stages[k], images[i]);
                p.answers.push_back({(uint16_t)k, (uint16_t)i, h ? answer_of(h, true) : declared_answer(p, k, i, true), true});
            }
    }
    for (Program::Answer& a : p.answers) a.type = canonical_answer(p, a, a.type);
    p.variant = answers_hash(p);
}
/// Whether the textures bound now answer what the variant's translation was told (a variant whose translation
/// asked nothing stands for any, and an unbound slot takes any)
bool still_valid(const NvnState& s, const Program& p) {
    for (const Program::Answer& a : p.answers) {
        const uint32_t h = answer_handle(s, p, a);
        if (h && canonical_answer(p, a, answer_of(h, a.image)) != canonical_answer(p, a, a.type)) return false;
    }
    return true;
}

} // namespace
std::mutex shader_mu;
std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> programs;   // by the stages' code keys
namespace {
std::unordered_map<uint64_t, std::vector<Code>> codes;                          // by program serial
const Shader::Profile& profile() {
    static const Shader::Profile p = make_profile();
    return p;
}

/// The profile of a stage pinned to 32 lanes (compute, and the mesh fallback's compute stages, pin_subgroup): the guest's warp
/// is the host's there, so the short path for warp ops, as before. The long one in every compute program made their SPIR-V new
/// and longer: the driver's cache had none of it, and AMD compiled the shaders for hours
const Shader::Profile& profile_pinned() {
    static const Shader::Profile p = [] {
        Shader::Profile q = profile();
        if (dev.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT) q.warp_size_potentially_larger_than_guest = false, q.max_subgroup_size = 32;
        return q;
    }();
    return p;
}

/// The profile of a graphics stage: one the GPU pins to 32 lanes as well (pin_subgroup: RDNA2's and RDNA3's fragment, task
/// and mesh stages, requiredSubgroupSizeStages 0xf0) keeps main's short path; only the ones it cannot pin (the vertex stage
/// there, every graphics stage on RDNA1's 0x20) take the long one. The fragment shuffle fallback stays in both: it is AMD's
/// compiler, not the lanes. RX_VK_WARP64 still sets every graphics stage
const Shader::Profile& profile_graphics(VkShaderStageFlags stage) {
    static const Shader::Profile pinned = [] {
        Shader::Profile q = profile();
        q.warp_size_potentially_larger_than_guest = false, q.max_subgroup_size = 32;
        return q;
    }();
    static const bool forced = getenv("RX_VK_WARP64") != nullptr;
    return !forced && (dev.sg32_stages & stage) ? pinned : profile();
}

const std::vector<Code>& codes_of(const NvnState& s) {
    auto& v = codes[s.prog.serial];
    if (v.empty())
        for (uint32_t i = 0; i < s.prog.nstage && i < 6; i++) {
            Code c = read_code(s.prog.sd[i]);
            if (c.ok) v.push_back(std::move(c));
        }
    return v;
}

std::atomic<int64_t> st_translated, st_failed, st_translate_ns;

} // namespace

Shader::AttributeType attribute_type_of(uint32_t vattr) {
    // (NVN vertex formats: the integer ones read as integers, everything else as float)
    const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&vattr);
    switch (a.fmt) {
    case NVN_FORMAT_R8UI: case NVN_FORMAT_RG8UI: case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGBA8UI:
    case NVN_FORMAT_R16UI: case NVN_FORMAT_RG16UI: case NVN_FORMAT_RGB16UI: case NVN_FORMAT_RGBA16UI:
    case NVN_FORMAT_R32UI: case NVN_FORMAT_RG32UI: case NVN_FORMAT_RGB32UI: case NVN_FORMAT_RGBA32UI:
    case NVN_FORMAT_RGB10A2UI:
        return Shader::AttributeType::UnsignedInt;
    case NVN_FORMAT_R8I: case NVN_FORMAT_RG8I: case NVN_FORMAT_RGB8I: case NVN_FORMAT_RGBA8I: case NVN_FORMAT_R16I:
    case NVN_FORMAT_RG16I: case NVN_FORMAT_RGB16I: case NVN_FORMAT_RGBA16I: case NVN_FORMAT_R32I: case NVN_FORMAT_RG32I:
    case NVN_FORMAT_RGB32I: case NVN_FORMAT_RGBA32I: case NVN_FORMAT_RGB10A2I:
        return Shader::AttributeType::SignedInt;
    case NVN_FORMAT_RGB10A2_UI2F: return Shader::AttributeType::UnsignedScaled;
    case NVN_FORMAT_RGB10A2_I2F: return Shader::AttributeType::SignedScaled;
    default:
        if (a.fmt >= NVN_FORMAT_R8_UI2F && a.fmt <= NVN_FORMAT_RGBA32_I2F)
            return (a.fmt - NVN_FORMAT_R8_UI2F) & 1 ? Shader::AttributeType::SignedScaled : Shader::AttributeType::UnsignedScaled;
        return Shader::AttributeType::Float;
    }
}

namespace {
const Shader::ProgramHeader& header_of(const Code& c) {
    return *reinterpret_cast<const Shader::ProgramHeader*>(c.sph);
}
/// The colour output types a fragment program can tell apart: those of the targets it writes. They were all in the program's key,
/// and the same program drawn with another integer target it never writes was another program
void written_types(const Code* fs, const uint8_t color_types[8], uint8_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = fs && header_of(*fs).ps.HasOutputComponents(i) ? color_types[i] : 0;
}
/// A vertex and fragment program's key: their code, and the vertex input and colour output types that change what they translate to
/// (hookshot) Whether this fragment stage's program is to have PsychoV35 for its scene tone map (scene_tonemap_pass.h): a scene
/// mapping program while the Tone mapper row asks for it. Its own key, so the row's change translates only these programs
bool scene_tonemap_for(const Code* fs) { return fs && fs->scene_fit && scene_tonemap_on(); }
uint64_t graphics_key(const Code& vs, const Code* fs, const uint32_t vattr[16], const uint8_t color_types[8],
                      bool tonemap = false) {
    uint64_t key = vs.key ^ (fs ? fs->key * 31 : 0);
    if (tonemap) key = hash_bytes("psychov35", 9, key);
    for (int i = 0; i < 16; i++) {
        const auto used = header_of(vs).vtg.InputGeneric((size_t)i);
        const bool read = used[0] || used[1] || used[2] || used[3];
        key = key * 1099511628211ull ^ (uint64_t)(read ? attribute_type_of(vattr[i]) : Shader::AttributeType::Float) << i;
    }
    uint8_t written[8];
    written_types(fs, color_types, written);
    return hash_bytes(written, 8, key);
}
uint64_t mesh_key(const Code* task, const Code& mesh, const Code* fs, const Code* prim, const uint8_t color_types[8]) {
    uint64_t key = hash_bytes("mesh", 4, mesh.key);
    if (task) key = key * 1099511628211ull ^ task->key;
    if (fs) key = key * 1099511628211ull ^ fs->key;
    if (prim) key = hash_bytes(prim->sph, sizeof prim->sph, key);
    uint8_t written[8];
    written_types(fs, color_types, written);
    return hash_bytes(written, 8, key);
}
} // namespace

void log_program_stages(const NvnState& s, const char* why) {
    char line[1024];
    int n = snprintf(line, sizeof line, "vk: %s: program %llx, %u stages:", why, (unsigned long long)s.prog.serial,
                     s.prog.nstage);
    for (uint32_t i = 0; i < s.prog.nstage && i < 6 && n < (int)sizeof line - 32; i++) {
        uint8_t ctl[0x60]{};
        host->gpu_read(s.prog.sd[i].control, ctl, sizeof ctl);
        uint16_t st = 0;
        uint32_t magic = 0;
        std::memcpy(&magic, ctl, 4);
        std::memcpy(&st, ctl + 0x56, 2);
        const Code c = read_code(s.prog.sd[i]);
        uint32_t sph0 = 0;
        if (c.ok && !c.code.empty()) std::memcpy(&sph0, c.code.data(), 4);
        n += snprintf(line + n, sizeof line - n, " [%u: stage %u%s, code %#llx, sph type %u bits24-25 %u, isbe %d payload %u]",
                      i, st, magic == kControlMagic ? "" : " ?", (unsigned long long)s.prog.sd[i].data,
                      (sph0 >> 10) & 0xf, (sph0 >> 24) & 3, (int)c.isbe, c.payload_bytes);
    }
    logf("%s", line);
}

namespace {

/// RX_VK_IR=<program key>[,...] (hex, a prefix is enough): the stages' Eden IR written to ir_<key>_<stage>.txt in the
/// working folder when the program is translated (diagnostics)
void dump_ir(const Program& p, const char* stage, const Shader::IR::Program& prog, uint64_t code_key = 0) {
    static const char* want = getenv("RX_VK_IR");
    if (!want) return;
    char key[24], alt[24];
    snprintf(key, sizeof key, "%016llx", (unsigned long long)p.key);
    snprintf(alt, sizeof alt, "%016llx", (unsigned long long)code_key);   // (hookshot: or the stage code's own key)
    bool match = false;
    for (const char* q = want; *q;) {
        const char* e = q;
        while (*e && *e != ',') e++;
        if (e > q && (!strncmp(key, q, (size_t)(e - q)) || (code_key && !strncmp(alt, q, (size_t)(e - q))))) match = true;
        q = *e ? e + 1 : e;
    }
    if (!match) return;
    char path[96];
    snprintf(path, sizeof path, "ir_%s_%s.txt", key, stage);
    if (FILE* f = fopen(path, "w")) {
        const std::string text = Shader::IR::DumpProgram(prog);
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        logf("vk: IR of program %s, %s stage, written to %s", key, stage, path);
    }
}

/// Translates a program variant's stages into p (on a worker for graphics programs: s and the codes are copies)

// (hookshot) RX_VK_MESH_NOCULL=<task shader code key prefixes,...>, off unless set. OoT's meshlet
// culling task shader walks every cell of a coarse depth grid its meshlet covers, reading four words a cell from c3 by a
// dynamic index: on RDNA 2 one draw runs past the kernel's 10 s and the GPU is reset (Hookshot issue 817). Its answer is
// visible = (result & 1) | (c3[2320] & 4), bit 4 being the game's own "draw everything". That comparison becomes true,
// and dead code elimination takes the grid walk with it: every meshlet is drawn (the rasterizer clips what is off
// screen), none is left out; RX_VK_SKIP_CODE left these programs out instead, and their models with them. The Steam
// Deck plays with RX_VK_MESH_NOCULL=2c56dc1006942f74 (issue 817). A name under RX_VK_MESH, so the program cache starts
// again when it changes.
bool mesh_nocull_wanted(uint64_t code_key) {
    static const std::string list = getenv("RX_VK_MESH_NOCULL") ? getenv("RX_VK_MESH_NOCULL") : "";
    if (list.empty() || list == "0") return false;
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)code_key);
    for (size_t at = 0; at < list.size();) {
        const size_t e = list.find(',', at);
        const std::string k = list.substr(at, e == std::string::npos ? std::string::npos : e - at);
        if (!k.empty() && !strncmp(hex, k.c_str(), k.size())) return true;
        at = e == std::string::npos ? list.size() : e + 1;
    }
    return false;
}

bool reads_c3_2320_bit4(const Shader::IR::Value& v) {   // (BitwiseAnd32 of #4 and GetCbufU32 #3, #2320, through Identity)
    using namespace Shader::IR;
    const Value r = v.Resolve();
    if (r.IsImmediate()) return false;
    const Inst* and_ = r.InstRecursive();
    if (and_->GetOpcode() != Opcode::BitwiseAnd32) return false;
    bool four = false, cbuf = false;
    for (size_t i = 0; i < 2; i++) {
        const Value a = and_->Arg(i).Resolve();
        if (a.IsImmediate()) {
            four = four || a.U32() == 4;
            continue;
        }
        const Inst* c = a.InstRecursive();
        if (c->GetOpcode() != Opcode::GetCbufU32) continue;
        const Value bank = c->Arg(0).Resolve(), off = c->Arg(1).Resolve();
        cbuf = cbuf || (bank.IsImmediate() && off.IsImmediate() && bank.U32() == 3 && off.U32() == 2320);
    }
    return four && cbuf;
}

void mesh_nocull(Shader::IR::Program& program, uint64_t code_key) {
    using namespace Shader::IR;
    if (!mesh_nocull_wanted(code_key)) return;
    int made = 0;
    for (Block* block : program.blocks)
        for (Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != Opcode::INotEqual) continue;
            const Value zero = inst.Arg(1).Resolve();
            if (!zero.IsImmediate() || zero.U32() != 0) continue;
            const Value orv = inst.Arg(0).Resolve();
            if (orv.IsImmediate()) continue;
            const Inst* o = orv.InstRecursive();
            if (o->GetOpcode() != Opcode::BitwiseOr32) continue;
            if (!reads_c3_2320_bit4(o->Arg(0)) && !reads_c3_2320_bit4(o->Arg(1))) continue;
            inst.ReplaceUsesWith(Value{true});
            made++;
        }
    if (made) Shader::Optimization::DeadCodeEliminationPass(program);
    static std::atomic<int> told{0};
    if (told++ < 8)
        logf("vk: task code %016llx: culling %s (RX_VK_MESH_NOCULL)", (unsigned long long)code_key,
             made ? "off, every meshlet drawn" : "pattern not found, left as it is");
}

// (hookshot) What the program cache's records are good for, as far as the translation goes: this translator (HOOKSHOT_SHADER_STAMP,
// nx2/build.py) and what it is told of the GPU and the environment. The cache's own format and I/O are vk_progcache.cpp's, outside
// the stamp, with a version of their own (kProgramCacheFormat): a change to how the cache is read or scheduled no longer throws
// every player's translated programs away
} // namespace
uint64_t translator_stamp() {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto add = [&h](const void* d, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)d)[i]) * 1099511628211ull;
    };
    // (what translates shaders, nx2/build.py: a rebuild of anything else keeps the cache. It was the DLL's bytes, and every build
    // lost it)
    const uint64_t translator = HOOKSHOT_SHADER_STAMP;
    add(&translator, sizeof translator);
    const uint32_t depth = host->depth_minus_w() != 0;   // (d3zessete's PR 54: programs made in either depth mode are kept apart)
    add(&depth, sizeof depth);
    // (and a GPU's mesh shaders: a file of native task and mesh programs is no use to the compute fallback, nor its to one)
    const uint32_t mesh = (dev.mesh_shader ? 1u : 0u) | (dev.mesh_fallback ? 2u : 0u) | (dev.task_mesh_fallback ? 4u : 0u);
    add(&mesh, sizeof mesh);
    // (and what of the GPU the translator is told (make_profile): its warps' lanes and the driver's shuffle workaround, and
    // which stages it pins to 32 lanes, which keep the short path: profile_graphics. An explicit warp override bypasses
    // the pinned graphics profile even if the base profile already has the requested width)
    const uint32_t lanes[] = {profile().warp_size_potentially_larger_than_guest ? 1u : 0u, profile().max_subgroup_size,
                              profile().has_broken_spirv_fragment_shuffle ? 1u : 0u, (uint32_t)dev.sg32_stages,
                              getenv("RX_VK_WARP64") ? 1u : 0u,
                              // (and where the translation starts a storage binding: offsets are counted from there, so a cache
                              // made on a 16-aligned GPU is no use on a 64-aligned one in the same folder, a laptop's two GPUs)
                              (uint32_t)storage_binding_align()};
    add(lanes, sizeof lanes);
    // (k10) f16 lowered to f32 on a GPU without f16 shaders (host_info): only then, so other GPUs keep their caches
    if (!dev.vk12.shaderFloat16) {
        const uint32_t no_f16 = 0x6e663136u;   // ("nf16")
        add(&no_f16, sizeof no_f16);
    }
    // (the records keep Shader::Info as it lies in memory: a cache written by another C++ library (the MSVC build's, then the
    // llvm-mingw one's libc++) has other layouts, and reading it crashed at start. The library and the sizes, so it starts again)
    const uint32_t abi[] = {(uint32_t)sizeof(Shader::Info), (uint32_t)sizeof(Program), (uint32_t)sizeof(std::vector<uint32_t>),
#if defined(_LIBCPP_VERSION)
                            1u, (uint32_t)_LIBCPP_VERSION
#elif defined(_MSVC_STL_VERSION)
                            2u, (uint32_t)_MSVC_STL_VERSION
#else
                            3u, 0u
#endif
    };
    add(abi, sizeof abi);
    {   // (the translation's per-GPU settings, set by coopmat_init at device creation: AMD/Intel's invariant positions, the colour
        // NaN guard, the matrix layout measured on this device. Without them a cache made on one GPU was used on another (a laptop's
        // integrated and dedicated GPUs), and RX_VK_NO_INVARIANT / RX_VK_NAN_GUARD did nothing for programs already cached)
        using namespace Shader::Backend::SPIRV;
        // (and the vendor fixes of the translation, vk_vendor.h: out-of-bounds fetches, NaN tests, the gather offset)
        const uint32_t flags = (position_invariant ? 1u : 0u) | (frag_nan_guard ? 2u : 0u) | (mma_software ? 4u : 0u) |
                               (global_table_loop ? 8u : 0u) | (fetch_bounds ? 16u : 0u) | (nan_bits ? 32u : 0u) |
                               (profile().need_gather_subpixel_offset ? 64u : 0u) | (fetch_bounds_buffers ? 128u : 0u);
        add(&flags, sizeof flags);
        add(mma_order_a.data(), sizeof(uint32_t) * mma_order_a.size());
        add(mma_order_b.data(), sizeof(uint32_t) * mma_order_b.size());
        add(mma_order_c.data(), sizeof(uint32_t) * mma_order_c.size());
    }
    // (and whether a float add is the GPU's own atomic (make_profile): a program made with OpAtomicFAddEXT is no use to a GPU
    // without it, nor one made with RX_VK_NATIVE_FADD=0's loop wanted back once it is native again)
    const uint32_t float_add = profile().support_float32_atomic_add ? 1u : 0u;
    add(&float_add, sizeof float_add);
    // (RX_VK_GP_, not RX_VK_GP: that took RX_VK_GPL with it, which changes no translation, and switching the stage libraries on
    // threw the whole program cache away. What the libraries are good for is the prepare's second stamp, vk_draw.cpp warmed_stamp)
    // (and every other switch the translation reads: the storage buffer pass's (RX_VK_NO_CBUF_LOAD_RULE, RX_VK_PTR_SPLIT_FALLBACK,
    // RX_VK_ALL_GLOBAL_PROGS), the lowering's extended registers (RX_VK_NO_EXTREGS), the draw parameters (RX_VK_NO_DRAW_PARAMS)
    // and the miss log's buffer and block counters (RX_VK_MISS_LOG, RX_VK_BLOCK_PROBE): without them a switch did nothing for
    // the programs already cached. tests/test_program_stamp_knobs.py checks the translator's switches against this list)
    for (char** e = _environ; e && *e; e++)
        for (const char* pre : {"RX_VK_MMA", "RX_VK_NATIVE", "RX_VK_NAN", "RX_VK_TASK_CAP", "RX_VK_NETWORK", "RX_VK_GP_", "RX_VK_FOLLOW", "RX_VK_NO_FOLLOW", "RX_VK_NO_F16", "RX_VK_SHARED", "RX_VK_MESH", "RX_VK_NO_INVARIANT", "RX_VK_MARKERS",
                                "RX_VK_NO_CBUF_LOAD_RULE", "RX_VK_PTR_SPLIT_FALLBACK", "RX_VK_ALL_GLOBAL_PROGS", "RX_VK_NO_EXTREGS", "RX_VK_NO_DRAW_PARAMS", "RX_VK_MISS_LOG", "RX_VK_BLOCK_PROBE", "EDEN_"})
            if (!strncmp(*e, pre, strlen(pre))) add(*e, strlen(*e));
    return h;
}
namespace {

void translate_into(Program& p, const NvnState& s, const Code* vsc, const Code* fsc, const Code* csc,
                    const uint8_t color_types[8]) {
    const auto t0 = std::chrono::steady_clock::now();
    QuestionsNoted noted{p};
    ptr_diag_reset();
    try {
        Shader::Backend::Bindings bindings{};
        int questions = 0;
        std::vector<std::vector<uint32_t>> kept;   // (each stage's SPIR-V, for the program cache)
        if (csc) {
            Env env{s, *csc, Shader::Stage::Compute};
            {   // (EvilOOT) where this program's storage buffer table is in c0 (NVN2's layout, not Maxwell's 0x310)
                uint32_t count = 0, end = 0;
                const uint32_t base = compute_ssbo_table(s, &count, &end);
                Shader::Optimization::g_nvn2_compute_ssbo_base = count ? base : 0;   // (no table found: Maxwell's, as before)
                Shader::Optimization::g_nvn2_compute_ssbo_count = count ? count : 16;
                Shader::Optimization::g_nvn2_compute_layout_end = count ? end : 0;
                // (RX_VK_MISS_LOG_PROG=<key prefix>: the miss log for that program only)
                static const char* only = getenv("RX_VK_MISS_LOG_PROG");
                char hex[24];
                snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
                Shader::Optimization::g_nvn2_miss_log_this = !only || !strncmp(hex, only, strlen(only));
                // (RX_VK_ALL_GLOBAL_PROGS=<key prefix>[,...], a test: no access of those programs bound to a descriptor)
                bool all_global = false;
                if (const char* ag = getenv("RX_VK_ALL_GLOBAL_PROGS"))
                    for (const char* q = ag; *q;) {
                        const char* e = strchr(q, ',');
                        const size_t len = e ? (size_t)(e - q) : strlen(q);
                        if (len && !strncmp(hex, q, len)) all_global = true;
                        q = e ? e + 1 : q + len;
                    }
                Shader::Optimization::g_all_global_this = all_global;
                if (all_global) logf("vk: program %s: every memory access left global (RX_VK_ALL_GLOBAL_PROGS)", hex);
                Shader::Optimization::g_gm_port_this = key_listed(hex, "EDEN_NX2_GM_PORT_PROGS");
                if (Shader::Optimization::g_gm_port_this) logf("vk: program %s: the Fritchii tracking (EDEN_NX2_GM_PORT_PROGS)", hex);
            }
            struct ResetTable {
                ~ResetTable() {
                    Shader::Optimization::g_nvn2_compute_ssbo_base = 0, Shader::Optimization::g_nvn2_compute_ssbo_count = 16;
                    Shader::Optimization::g_nvn2_compute_layout_end = 0;
                    Shader::Optimization::g_all_global_this = false;
                    Shader::Optimization::g_gm_port_this = false;
                }
            } reset_table;
            auto t = translate(env, false);
            questions += env.texture_questions;
            Shader::RuntimeInfo rt{};
            dump_ir(p, "compute", t->program);   // (before the SPIR-V: the programs it fails on are the ones to read)
            if (getenv("RX_VK_IR"))
                logf("vk: compute %016llx: workgroup %ux%ux%u, shared memory %u bytes, local memory %u", (unsigned long long)p.key,
                     t->program.workgroup_size[0], t->program.workgroup_size[1], t->program.workgroup_size[2],
                     t->program.shared_memory_size, t->program.local_memory_size);
            // (hookshot) The movie upscaler's dozen networks, on a card that cannot run them as the console does: their software matrix multiplies
            // took minutes each for AMD's compiler (the black screen with sound, Hookshot issue 26: JaredEzz's analysis) and the movies ran at 10 fps
            // (issues 38, 43). Left out, the last pass becomes a linear upscale of the movie (upscale_stub in vk_draw.cpp): the same picture at 60
            // fps, a little softer. The gameplay TAAU network stays. RX_VK_NETWORK_KEEP=<key prefixes>, "all": every one runs
            if (t->program.info.uses_cooperative_matrix) {
                static const std::string keep = getenv("RX_VK_NETWORK_KEEP") ? getenv("RX_VK_NETWORK_KEEP") : "fd029a317a9a4b94,9dc98871eb7fc101";   // (amd-fix: 9dc98871 resolves the dynamic-resolution scene to 2048x1152; left out, the 3D picture is black under the HUD)
                char hex[24];
                snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
                bool kept_one = keep == "all";
                for (size_t at = 0; !kept_one && at < keep.size();) {
                    const size_t e = keep.find(',', at);
                    const std::string k = keep.substr(at, e == std::string::npos ? std::string::npos : e - at);
                    kept_one = !k.empty() && !strncmp(hex, k.c_str(), k.size());
                    at = e == std::string::npos ? keep.size() : e + 1;
                }
                if ((!kept_one && (Shader::Backend::SPIRV::mma_software || dev.props.vendorID != 0x10DE)) || env_on("RX_VK_NO_F16") || !dev.vk12.shaderFloat16)
                    throw std::runtime_error("a network left out on this card");
            }
            const std::vector<uint32_t> spirv = Shader::Backend::SPIRV::EmitSPIRV(profile_pinned(), rt, t->program, bindings);
            // (EvilOOT) RX_VK_DUMP_SPV=<folder>: the SPIR-V of the programs RX_VK_MISS_LOG_PROG names (all: unset), for spirv-val
            if (const char* dir = getenv("RX_VK_DUMP_SPV"); dir && Shader::Optimization::g_nvn2_miss_log_this) {
                char path[512];
                snprintf(path, sizeof path, "%s/%016llx.spv", dir, (unsigned long long)p.key);
                if (FILE* f = fopen(path, "wb")) fwrite(spirv.data(), 4, spirv.size(), f), fclose(f);
            }
            if (const char* d = getenv("RX_VK_IR")) {   // (the SPIR-V too, for spirv-val)
                char name[64];
                snprintf(name, sizeof name, "spv_%016llx.spv", (unsigned long long)p.key);
                if (strstr(d, name + 4 > name ? name + 4 : name) || true)
                    if (std::string(d).find(std::string(name + 4, 16)) != std::string::npos)
                        if (FILE* f = fopen(name, "wb")) fwrite(spirv.data(), 4, spirv.size(), f), fclose(f);
            }
            Stage st;
            st.nvn_stage = 5;
            st.info = t->program.info;
            st.module = make_module(spirv);
            kept.push_back(spirv);
            st.vk_stage = VK_SHADER_STAGE_COMPUTE_BIT;
            st.c1 = csc->c1;
            p.workgroup = {t->program.workgroup_size[0], t->program.workgroup_size[1], t->program.workgroup_size[2]};
            p.shared_memory = t->program.shared_memory_size;
            p.stages.push_back(std::move(st));
        } else {
            Env vs_env{s, *vsc, Shader::Stage::VertexB};
            auto vs = translate(vs_env, true);
            questions += vs_env.texture_questions;
            std::unique_ptr<Translation> fs;
            if (fsc) {
                Env fs_env{s, *fsc, Shader::Stage::Fragment};
                fs = translate(fs_env, true);
                questions += fs_env.texture_questions;
            }
            Shader::RuntimeInfo vrt{};
            Shader::RuntimeInfo frt{};
            if (!vs->program.info.stores[Shader::IR::Attribute::PointSize]) vrt.fixed_state_point_size = 1.0f;
            // (an attribute the draw leaves unset: build_graphics gives it a stream of (0, 0, 0, 1). It took Disabled here, under the
            // same key)
            for (int i = 0; i < 16; i++) vrt.generic_input_types[i] = attribute_type_of(s.vattr[i]);
            vrt.previous_stage_stores.mask.set();
            vrt.convert_depth_mode = host->depth_minus_w() != 0;
            // Deku Tree web (#455): with DontInline, NVIDIA's compiled vertex shader substitutes the grid-origin
            // load result for the grid dimensions in the coordinate clamps. The SPIR-V names the right result.
            constexpr uint64_t kDekuWebVertexCode = 0xb449bcdb69b36a8aull;
            vrt.inline_global_128_loads = dev.props.vendorID == 0x10DE && vsc->key == kDekuWebVertexCode;
            // (OoT's Royal Family Tomb water writes clip.xy/clip.w as a noperspective varying. Clipping a
            // vertex with w close to zero then makes AMD interpolate the wrong depth, whose exp2 overflows
            // into the red dotted line. Write it times Position.w and interpolate perspective-correct in
            // the fragment stage, where multiplying by FragCoord.w restores the screen-linear value.)
            // Indexed fragment reads cannot undo that multiply, transform feedback would capture the
            // scaled value, and mesh pipelines keep their existing per-primitive interpolation.
            if (fs && !fs->program.info.loads_indexed_attributes && vrt.xfb_count == 0) {
                for (size_t i = 0; i < fs->program.info.interpolation.size(); ++i) {
                    if (fs->program.info.interpolation[i] == Shader::Interpolation::NoPerspective) {
                        vrt.noperspective_outputs.set(i);
                    }
                }
                frt.scaled_noperspective_inputs = vrt.noperspective_outputs.any();
            }
            Stage v;
            v.nvn_stage = 0;
            const std::vector<uint32_t> vspirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_VERTEX_BIT), vrt, vs->program, bindings);
            dump_ir(p, "vertex", vs->program);
            {   // (the programs that read the draw's parameters from the driver bank, each once)
                const auto& ld = vs->program.info.loads;
                const bool bi = ld[Shader::IR::Attribute::BaseInstance], bv = ld[Shader::IR::Attribute::BaseVertex],
                           di = ld[Shader::IR::Attribute::DrawID];
                static std::atomic<int> logged{0};
                if ((bi || bv || di) && logged++ < 64)
                    logf("vk: program %016llx reads the draw's%s%s%s", (unsigned long long)p.key, bv ? " base vertex" : "",
                         bi ? " base instance" : "", di ? " draw index" : "");
            }
            v.info = vs->program.info;
            v.module = make_module(vspirv);
            kept.push_back(vspirv);
            v.vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
            v.c1 = vsc->c1;
            p.stages.push_back(std::move(v));
            if (fs) {
                frt.previous_stage_stores = vs->program.info.stores;
                for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
                if (p.scene_tonemap) {   // (the HDR tone mapper row: PsychoV35; before the NaN guard, which then guards it)
                    const char* why = Shader::Optimization::SceneToneMapPass(fs->program);
                    static std::atomic<int> told{0};
                    if (told++ < 64)
                        logf("vk: program %016llx: scene tone map %s%s", (unsigned long long)p.key, why ? "left as the game's: " : "PsychoV35",
                             why ? why : "");
                }
                if (Shader::Backend::SPIRV::frag_nan_guard) {   // (RX_VK_NAN_GUARD=0: off, vk_coopmat.cpp coopmat_init)
                    uint32_t float_targets = 0;
                    for (int i = 0; i < 8; i++)
                        if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
                    Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
                }
                const std::vector<uint32_t> fspirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_FRAGMENT_BIT), frt, fs->program, bindings);
                dump_ir(p, "fragment", fs->program);
                Stage f;
                f.nvn_stage = 1;
                f.info = fs->program.info;
                f.module = make_module(fspirv);
                kept.push_back(fspirv);
                f.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                f.c1 = fsc->c1;
                p.stages.push_back(std::move(f));
            }
        }
        for (const Stage& st : p.stages)
            if (!st.module) throw Shader::LogicError("vkCreateShaderModule failed");
        if (questions || answers_in) record_answers(p, s);
        st_translated++;
        ptr_diag_log(p.key, csc ? "compute" : fsc ? "graphics" : "vertex");
        if (seed_capture) *seed_capture = std::move(kept);
        else program_cache_save(p, kept);
    } catch (const std::exception& e) {
        p.failed = true;
        const bool trial = forced && !forced->want.empty();   // (an answer seed_compute tries: it may well not translate)
        if (!trial) st_failed++;
        static std::atomic<int> logged;
        if (!trial && logged++ < 30) logf("vk: program %016llx not translated: %s", (unsigned long long)p.key, e.what());
    }
    st_translate_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    p.ready.store(true, std::memory_order_release);
}

} // namespace

namespace {

/// The mesh registers of an NVN2 task / mesh pair as the Switch 2 driver sets them up (clc597.h SET_MESH_SHADER_A / B,
/// SET_MESH_INIT_SHADER), from their control sections: what Eden's recompiler sizes the stage buffer and outputs by
/// A program translated in play: what it was (the shader precompile leaves none of these to be met)
std::unordered_set<uint64_t> seed_keys;   // (RX_VK_SEED_CHECK=1: the seed's stages, for miss_note)
void miss_note(uint64_t key, const char* what, uint64_t a, uint64_t b, const NvnState& s, const uint8_t color_types[8], size_t others) {
    static std::atomic<int> told{0};
    if (told++ >= 1500) return;
    char attrs[20]{};
    for (int i = 0; i < 16; i++) attrs[i] = (char)('0' + (int)attribute_type_of(s.vattr[i]));
    logf("vk: in play: %s %016llx (%016llx%s %016llx%s), inputs %s, outputs %02x %02x %02x %02x %02x %02x %02x %02x, %zu other variants", what,
         (unsigned long long)key, (unsigned long long)a, seed_keys.count(a) ? "+" : "", (unsigned long long)b, seed_keys.count(b) ? "+" : "", attrs,
         color_types[0], color_types[1], color_types[2],
         color_types[3], color_types[4], color_types[5], color_types[6], color_types[7], others);
}

std::array<u32, 3> mesh_registers_of(const Code* task, const Code& mesh) {
    // (the control section's topology in GL's numbers: 3 triangles; 1 points, not lines. OoT's four mesh programs with a 1 (the grass
    // drawn into the 2400 x 2400 shadow map around Link) write one index byte per primitive, the primitive count = their vertex
    // count, and a point size. Read as lines, each joined vertex 2i to 2i + 1 of its meshlet: a mesh of long thin dark stripes on
    // the ground, the water and Link around him, on AMD; Hookshot issue 302)
    const uint32_t topology = mesh.topology == 3 ? 4 : mesh.topology == 1 ? 0 : mesh.topology;
    const uint32_t lines = task ? (task->payload_bytes + 127) / 128 : 0;
    // (the larger of the two for both: the control section's pair is not always vertices and primitives in that order. The door's
    // says 126 and 64, where its code writes 64 vertices and up to 128 primitives; held to 64, a meshlet's other primitives were
    // never output: holes in models and through the walls of the scene)
    const uint32_t most = std::max(mesh.max_vertices, mesh.max_primitives);
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13),
            std::min(mesh.mesh_threads, 0x3Fu) << 20,
            (task ? std::min(task->mesh_threads, 0x3Fu) : 0) | (std::min(lines, 0x1FFu) << 12) |
                (std::min(lines, 0x1FFu) << 22)};
}

/// The stage whose banks a task / mesh program reads (its control section's stage when it is one of NVN's six)
int bank_stage(const Code& c) {
    return c.stage >= 0 && c.stage < kNvnStages ? c.stage : 0;   // (NVN2: task 6, mesh 7, with their own bindings)
}

void translate_mesh_into(Program& p, const NvnState& s, const Code* task, const Code& mesh, const Code* prim,
                         const Code* fsc, const uint8_t color_types[8]) {
    const auto t0 = std::chrono::steady_clock::now();
    QuestionsNoted noted{p};
    ptr_diag_reset();
    {   // (EvilOOT, RX_VK_MISS_LOG_PROG=<key prefix>: the miss log for that program only)
        static const char* only = getenv("RX_VK_MISS_LOG_PROG");
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
        Shader::Optimization::g_nvn2_miss_log_this = !only || !strncmp(hex, only, strlen(only));
    }
    try {
        Shader::Backend::Bindings bindings{};
        std::vector<std::vector<uint32_t>> kept;   // (each stage's SPIR-V, for the program cache: mesh programs were never kept,
                                                   // so each was translated and its pipeline built in the middle of play, every run)
        const bool fallback = p.fallback;
        std::unique_ptr<Translation> task_t;
        const std::array<u32, 3> mesh_regs = mesh_registers_of(task, mesh);
        if (task) {
            Env env{s, *task, Shader::Stage::Task};
            env.set_mesh(mesh_regs, nullptr);
            task_t = translate(env, true, true);
            mesh_nocull(task_t->program, task->key);
            dump_ir(p, "task", task_t->program, task->key);
            Shader::RuntimeInfo rt{};
            rt.previous_stage_stores.mask.set();
            rt.mesh_fallback = fallback;
            rt.mesh_fallback_capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
            const std::vector<uint32_t> spirv = Shader::Backend::SPIRV::EmitSPIRV(fallback ? profile_pinned() : profile_graphics(VK_SHADER_STAGE_TASK_BIT_EXT), rt, task_t->program, bindings);
            kept.push_back(spirv);
            Stage st;
            st.nvn_stage = bank_stage(*task);
            st.info = task_t->program.info;
            st.module = make_module(spirv);
            st.vk_stage = fallback ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_TASK_BIT_EXT;
            st.c1 = task->c1;
            p.stages.push_back(std::move(st));
        }
        Env menv{s, mesh, Shader::Stage::Mesh};
        menv.set_mesh(mesh_regs, prim ? prim->sph : nullptr);
        auto mesh_t = translate(menv, true, true);
        dump_ir(p, "mesh", mesh_t->program, mesh.key);
        {
            static std::atomic<int> told{0};
            if (told++ < 400) {
                const auto& mi = mesh_t->program.mesh;
                char l[300];
                int k = snprintf(l, sizeof l, "vk: mesh %016llx: threads %u max v %u p %u payload %u, %zu vertex lines:", (unsigned long long)p.key,
                                 mesh_t->program.workgroup_size[0], mi.max_vertices, mi.max_primitives, mi.payload_words, mi.vertex_lines.size());
                for (const u32 line : mi.vertex_lines)
                    if (k < 270) k += snprintf(l + k, sizeof l - k, " %x", line);
                k += snprintf(l + k, sizeof l - k, "; %zu primitive", mi.primitive_lines.size());
                logf("%s", l);
            }
        }
        int questions = menv.texture_questions;
        Shader::RuntimeInfo mrt{};
        if (task_t) mrt.previous_stage_stores = task_t->program.info.stores;
        else mrt.previous_stage_stores.mask.set();
        mrt.convert_depth_mode = host->depth_minus_w() != 0;
        mrt.mesh_fallback = fallback;
        mrt.mesh_fallback_capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
        mrt.mesh_fallback_task = task_t != nullptr;
        mrt.mesh_fallback_task_threads = task_t ? task_t->program.workgroup_size[0] : 0;
        const std::vector<uint32_t> mspirv = Shader::Backend::SPIRV::EmitSPIRV(fallback ? profile_pinned() : profile_graphics(VK_SHADER_STAGE_MESH_BIT_EXT), mrt, mesh_t->program, bindings);
        kept.push_back(mspirv);
        Stage ms;
        ms.nvn_stage = bank_stage(mesh);
        ms.info = mesh_t->program.info;
        ms.module = make_module(mspirv);
        ms.vk_stage = fallback ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_MESH_BIT_EXT;
        ms.c1 = mesh.c1;
        p.stages.push_back(std::move(ms));
        if (fallback) {   // (the ordinary vertex stage that reads the mesh epilogue's records)
            const Shader::MeshInfo& mi = mesh_t->program.mesh;
            Shader::RuntimeInfo vrt{};
            vrt.mesh_fallback = true;
            vrt.mesh_fallback_vertex = true;
            vrt.mesh_fallback_capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
            vrt.convert_depth_mode = host->depth_minus_w() != 0;
            const Shader::Stage saved_stage{mesh_t->program.stage};
            // (the generated vertex stage has no descriptors of its own: its inputs are the records. Only the stores
            // (the outputs) are kept from the mesh program's info.)
            Shader::Info saved_info{std::move(mesh_t->program.info)};
            mesh_t->program.info = Shader::Info{};
            mesh_t->program.info.stores = saved_info.stores;
            mesh_t->program.stage = Shader::Stage::VertexB;
            for (const u32 line : mi.primitive_lines) {
                const auto attribute = static_cast<Shader::IR::Attribute>(line);
                if (Shader::IR::IsGeneric(attribute))
                    vrt.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
            const std::vector<uint32_t> vspirv =
                Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_VERTEX_BIT), vrt, mesh_t->program, bindings);
            mesh_t->program.info = std::move(saved_info);
            mesh_t->program.stage = saved_stage;
            kept.push_back(vspirv);
            Stage v;
            v.nvn_stage = 0;
            v.vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
            v.module = make_module(vspirv);
            v.c1 = mesh.c1;
            p.stages.push_back(std::move(v));
            const u32 topo{mesh.topology == 3 ? 4u : mesh.topology == 1 ? 0u : mesh.topology};
            p.fallback = true;
            p.fallback_task = task_t != nullptr;
            p.fallback_task_threads = task_t ? task_t->program.workgroup_size[0] : 1;
            p.fallback_topology = topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                 : topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                             : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            p.fallback_vertices_per_workgroup = mi.max_primitives * mi.Corners();
            p.fallback_record_words = 1 + static_cast<u32>(mi.vertex_lines.size()) +
                                      static_cast<u32>(mi.primitive_lines.size());
            p.fallback_payload_words = mi.payload_words;
        }
        if (fsc) {
            Env fenv{s, *fsc, Shader::Stage::Fragment};
            auto fs = translate(fenv, true);
            questions += fenv.texture_questions;
            Shader::RuntimeInfo frt{};
            // (what the mesh program stores, and its per-primitive generics: Eden's MakeRuntimeInfo)
            frt.previous_stage_stores = mesh_t->program.info.stores;
            for (const u32 line : mesh_t->program.mesh.primitive_lines) {
                const auto attribute = static_cast<Shader::IR::Attribute>(line);
                if (!Shader::IR::IsGeneric(attribute)) continue;
                const u32 index = Shader::IR::GenericAttributeIndex(attribute);
                if (fallback)   // (a vertex pipeline has no PerPrimitiveEXT: Flat the same values, as the generated stage writes them)
                    fs->program.info.interpolation[index] = Shader::Interpolation::Flat;
                else
                    frt.per_primitive_generics.set(index);
            }
            // (points: a generic the fragment program reads and the mesh program never writes is the point sprite's coordinate,
            // as the hardware replaces it. OoT's grass shadow reads its alpha mask at generic 5's (s, t): left at (0, 0, 0, 1),
            // each point was a filled square in the shadow map instead of a tuft)
            if (mesh_t->program.mesh.topology == 0)
                for (size_t i = 0; i < Shader::IR::NUM_GENERICS; i++)
                    if (fs->program.info.loads.Generic(i) && !mesh_t->program.info.stores.Generic(i))
                        frt.point_coord_generics.set(i);
            for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
            const std::vector<uint32_t> fspirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_FRAGMENT_BIT), frt, fs->program, bindings);
            kept.push_back(fspirv);
            Stage f;
            f.nvn_stage = 1;
            f.info = fs->program.info;
            f.module = make_module(fspirv);
            f.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            f.c1 = fsc->c1;
            p.stages.push_back(std::move(f));
        }
        // (RX_VK_DUMP_MESH_SPV=<folder>: each stage's SPIR-V of the mesh programs translated, <key>_<n>_<stage>.spv, for
        // spirv-val; the fallback's compute and generated vertex stages too. A program the cache holds is not translated)
        if (const char* dir = getenv("RX_VK_DUMP_MESH_SPV"); dir && kept.size() == p.stages.size()) {
            static std::atomic<uint32_t> dumped{0};
            const uint32_t n = dumped++;
            for (size_t k = 0; k < kept.size(); k++) {
                const VkShaderStageFlags vs = p.stages[k].vk_stage;
                const char* name = vs == VK_SHADER_STAGE_COMPUTE_BIT     ? (p.fallback_task && k == 0 ? "task_cs" : "mesh_cs")
                                   : vs == VK_SHADER_STAGE_TASK_BIT_EXT  ? "task"
                                   : vs == VK_SHADER_STAGE_MESH_BIT_EXT  ? "mesh"
                                   : vs == VK_SHADER_STAGE_VERTEX_BIT    ? "vert"
                                   : vs == VK_SHADER_STAGE_FRAGMENT_BIT ? "frag"
                                                                         : "stage";
                char path[512];
                snprintf(path, sizeof path, "%s/%016llx_%u_%s.spv", dir, (unsigned long long)p.key, n, name);
                if (FILE* f = fopen(path, "wb")) fwrite(kept[k].data(), 4, kept[k].size(), f), fclose(f);
            }
        }
        for (const Stage& st : p.stages)
            if (!st.module) throw Shader::LogicError("vkCreateShaderModule failed");
        record_answers(p, s);   // (task questions not counted: kept conservative)
        (void)questions;
        ptr_diag_log(p.key, task ? "task+mesh" : "mesh");
        program_cache_save(p, kept);
        // (where its stages read their storage buffers: the driver bank layout of NVN2's stages 6 and 7)
        static std::atomic<int> logged_ssbo;
        for (const Stage& st : p.stages)
            if (!st.info.storage_buffers_descriptors.empty() && logged_ssbo++ < 12)
                logf("vk: mesh program %016llx, NVN stage %d: %zu storage buffers, first in c%u at %#x, base %#x",
                     (unsigned long long)p.key, st.nvn_stage, st.info.storage_buffers_descriptors.size(),
                     st.info.storage_buffers_descriptors[0].cbuf_index, st.info.storage_buffers_descriptors[0].cbuf_offset,
                     st.info.nvn_buffer_base);
        st_translated++;
    } catch (const std::exception& e) {
        p.failed = true;
        st_failed++;
        static std::atomic<int> logged;
        if (logged++ < 30) logf("vk: mesh program %016llx not translated: %s", (unsigned long long)p.key, e.what());
    }
    st_translate_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    p.ready.store(true, std::memory_order_release);
}

/// An NVN2 mesh program: the stages that write the internal stage buffer are its task (attribute writes only, when
/// there are two) and mesh programs; a header-only stage, its per-primitive outputs (shader_lock held)
const Program* mesh_program_locked(const NvnState& s, const uint8_t color_types[8]) {
    if (!dev.mesh_shader && !dev.mesh_fallback) return nullptr;
    const std::vector<Code>& cs = codes_of(s);
    const Code *fsc = nullptr, *prim = nullptr, *task = nullptr, *mesh = nullptr;
    for (const Code& c : cs) {
        if (c.header_only) prim = &c;
        else if (c.stage == 6) task = &c;
        else if (c.stage == 7) mesh = &c;
        else if (c.stage == 1) fsc = &c;
    }
    static int logged;
    if (logged++ < 6) log_program_stages(s, "mesh draw");
    if (!mesh) return nullptr;
    const bool fallback = dev.uses_mesh_fallback(task != nullptr);
    const uint64_t key = mesh_key(task, *mesh, fsc, prim, color_types);
    auto& variants = programs[key];
    program_cache_load(key, variants);
    const uint64_t codes[4] = {mesh->key, fsc ? fsc->key : 0, task ? task->key : 0, prim ? prim->key : 0};
    for (auto& p : variants)
        if (!p->codes[0]) std::memcpy(p->codes, codes, sizeof codes);
    for (auto& p : variants) {
        if (p->fallback != fallback || p->mesh == fallback) continue;
        if (!p->ready.load(std::memory_order_acquire)) return p.get();
        if (p->failed) return nullptr;
        if (still_valid(s, *p)) return p.get();
    }
    miss_note(key, "mesh", mesh->key, fsc ? fsc->key : 0, s, color_types, variants.size());
    auto p = std::make_unique<Program>();
    p->key = key;
    p->mesh = !fallback;
    p->fallback = fallback;
    std::memcpy(p->codes, codes, sizeof codes);
    Program* raw = p.get();
    variants.push_back(std::move(p));
    struct Job {
        Program* p;
        NvnState s;
        Code task, mesh, prim, fs;
        bool has_task, has_prim, has_fs;
        uint8_t ct[8];
    };
    auto job = std::make_shared<Job>();
    job->p = raw, job->s = s;
    job->has_task = task != nullptr, job->has_prim = prim != nullptr, job->has_fs = fsc != nullptr;
    if (task) job->task = *task;
    job->mesh = *mesh;
    if (prim) job->prim = *prim;
    if (fsc) job->fs = *fsc;
    std::memcpy(job->ct, color_types, 8);
    jobs_push([job] {
        translate_mesh_into(*job->p, job->s, job->has_task ? &job->task : nullptr, job->mesh,
                            job->has_prim ? &job->prim : nullptr, job->has_fs ? &job->fs : nullptr, job->ct);
    }, false, &raw->ready, [job]() noexcept {
        job->p->failed = true;
        job->p->ready.store(true, std::memory_order_release);
    });   // (a waiting draw can run the job; failure completes it too)
    return raw;
}

const Program* mesh_program_for(const NvnState& s, const uint8_t color_types[8]) {
    std::lock_guard lk(shader_mu);
    return mesh_program_locked(s, color_types);
}

} // namespace

namespace {
/// Picks by program and inputs (the replay thread's): draws mostly use programs seen a moment ago with the same
/// inputs; the variant search runs again only when those changed, and a variant chosen for its textures' types is
/// kept while the handles it was checked against are the same
struct Pick {
    const Program* p = nullptr;
    std::vector<uint32_t> handles;   // (at its answers' descriptors: their slots and the slots' versions)
};
thread_local std::unordered_map<uint64_t, Pick> picks;   // (each recording thread's: program_pick has its own lock)
uint64_t pick_inputs(const NvnState& s, const uint8_t color_types[8]) {
    uint64_t h = 0x9E37;
    for (int i = 0; i < 16; i++) h = h * 1099511628211ull ^ (uint64_t)s.vattr[i];
    h = h * 1099511628211ull ^ (uint64_t)s.nvattr;
    uint64_t ct = 0;
    std::memcpy(&ct, color_types, 8);
    return h * 1099511628211ull ^ ct;
}
void answer_handles(const NvnState& s, const Program& p, std::vector<uint32_t>& out) {
    out.clear();
    for (const Program::Answer& a : p.answers) {
        const uint32_t id = answer_handle(s, p, a) & 0xFFFFF;
        out.push_back(id);
        out.push_back(texture_slot_version((int)id));
    }
}
} // namespace

/// (hookshot) the bound program's stages and code keys, as "stage:key" words (RX_VK_SKIP_PROG's log)
/// (for logs) what the program's textures resolve to now
std::string program_textures(const NvnState& s, const Program& p) {
    std::string out;
    for (const Stage& st : p.stages)
        for (const auto& d : st.info.texture_descriptors) {
            const uint32_t h = descriptor_handle(s, st, d);
            nvn_tex t;
            nvn_view v;
            bool has_view = false;
            char b[160];
            const bool known = h && texture_of((int)(h & 0xFFFFF), &t, &v, &has_view);
            if (known)
                snprintf(b, sizeof b, " tex %#x %ux%ux%u fmt %#x target %d view %d", h, (unsigned)t.d.width, (unsigned)t.d.height, (unsigned)t.d.depth,
                         (unsigned)t.d.format, (int)t.d.target, (int)has_view);
            else
                snprintf(b, sizeof b, " tex %#x (not registered)", h);
            out += b;
        }
    return out;
}

std::string program_code_keys(const NvnState& s) {
    std::string out;
    if (!s.has_prog) return out;
    for (const Code& c : codes_of(s)) {
        char b[40];
        snprintf(b, sizeof b, " %u:%016llx", c.stage, (unsigned long long)c.key);
        out += b;
    }
    return out;
}

/// (hookshot) the bound compute program's code key (0: none): what "does not lower" names, for a rejected dispatch
uint64_t compute_code_key(const NvnState& s) {
    if (!s.has_prog) return 0;
    for (const Code& c : codes_of(s))
        if (c.stage == 5 && !c.header_only) return c.key;
    return 0;
}

/// The last answer for this serial, these stages, these bindings and these vertex inputs
bool prog_same(int kind, uint64_t serial, uint64_t stages, uint64_t bind, uint64_t reg, uint64_t ub, int nvattr,
               const uint32_t* vattr, const uint8_t* ct, int ckind, uint64_t cserial, uint64_t cstages, uint64_t cbind,
               uint64_t creg, uint64_t cub, int cnvattr, const uint32_t* cvattr, const uint8_t* cct) {
    if (kind != ckind || serial != cserial || stages != cstages || bind != cbind || reg != creg || ub != cub ||
        nvattr != cnvattr)
        return false;
    for (int i = 0; i < 16; i++)
        if (vattr[i] != cvattr[i]) return false;
    for (int i = 0; i < 8; i++)
        if (ct[i] != cct[i]) return false;
    return true;
}
uint64_t prog_stage_sig(const nvn_prog& p) {
    uint64_t h = (uint64_t)p.nstage << 32 | p.magic;
    for (int i = 0; i < 6; i++) h = h * 1099511628211ull ^ p.sd[i].data ^ (p.sd[i].control << 1);
    return h ^ p.serial;
}
// (hookshot) The stages whose bindings a graphics program's answers are read from, one bit a stage, when each answer is a handle in c2 of the
// stage itself (a handle program reads uniform memory, another bank other state): then the pick stands while those stages' textures,
// samplers and images do (NvnState::tex_gen), whatever else was bound. ~0: not so, the pick stands while nothing at all was bound anew
uint32_t answer_stage_mask(const Program& p) {
    uint32_t mask = 0;
    for (const Program::Answer& a : p.answers) {
        const Stage& st = p.stages[a.stage];
        if (st.nvn_stage < 0 || st.nvn_stage > 4) return ~0u;
        bool c2;
        if (a.image) {
            const auto& d = st.info.image_descriptors[a.desc];
            c2 = d.handle_program == ~0U && d.cbuf_index == 2;
        } else {
            const auto& d = st.info.texture_descriptors[a.desc];
            c2 = d.handle_program == ~0U && d.cbuf_index == 2 && (!d.has_secondary || d.secondary_cbuf_index == 2);
        }
        if (!c2) return ~0u;
        mask |= 1u << st.nvn_stage;
    }
    return mask;
}
struct ProgFast {
    bool on = false;
    int kind = 0;
    uint64_t serial = 0, stages = 0, bind = 0, reg = 0, ub = 0;
    int nvattr = 0;
    uint32_t vattr[16]{};
    uint8_t ct[8]{};
    const Program* p = nullptr;
    uint32_t amask = ~0u;           // (answer_stage_mask of p, ~0 for a compute program)
    uint64_t tex_gen[kNvnStages]{};   // (the stages' tex_gen it was kept at)
};
thread_local ProgFast prog_fast[2];

const Program* program_for(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]) {
    if (!s.has_prog) return nullptr;
    if (kind == ProgramKind::Mesh) return mesh_program_for(s, color_types);
    {   // (the HDR tone mapper changed: the picks before it name the other variant of the scene mapping programs)
        static thread_local int tonemap_seen = -1;
        const int tonemap_now = scene_tonemap_on() ? 1 : 0;
        if (tonemap_now != tonemap_seen) {
            tonemap_seen = tonemap_now;
            picks.clear();
            prog_fast[0].on = prog_fast[1].on = false;
        }
    }
    const bool compute = kind == ProgramKind::Compute;
    static int64_t st_hit, st_none, st_differ, st_calls, st_by_tex;   // (logged now and then: how the cache does)
    if (++st_calls % 200000 == 0)
        logf("vk: program picks: %lld kept (%lld by the last picks' textures, a uniform buffer bound since), %lld new, %lld with other textures (of %lld)",
             (long long)st_hit, (long long)st_by_tex, (long long)st_none, (long long)st_differ, (long long)st_calls);
    const uint64_t stages = prog_stage_sig(s.prog);
    const uint64_t bind = s.bind_gen, reg = registrations_generation(), ub = ub_generation();
    // (hookshot) the pair of the last picks stood only while nothing at all was bound anew, and OoT binds a uniform buffer for nearly every
    // draw: a draw of the same program with the same textures found its program through the full pick (the key, the answers' handles, a
    // compare). It stands now while the textures its answers are read from do (answer_stage_mask). RX_VK_PICK_BIND_GEN=1: as before
    static const bool by_bind = env_on("RX_VK_PICK_BIND_GEN");
    for (const ProgFast& f : prog_fast) {   // (the draw before this one, and the one that took turns with it)
        if (!(f.on && f.p && f.p->ready.load(std::memory_order_acquire) && !f.p->failed)) continue;
        uint64_t fbind = f.bind;
        bool by_tex = false;
        if (fbind != bind && !by_bind && !compute && f.amask != ~0u) {
            by_tex = true;
            for (int k = 0; k < kNvnStages; k++)
                if ((f.amask >> k & 1) && f.tex_gen[k] != s.tex_gen[k]) by_tex = false;
            if (by_tex) fbind = bind;
        }
        if (prog_same((int)kind, s.prog.serial, stages, bind, reg, ub, s.nvattr, s.vattr, color_types, f.kind, f.serial,
                      f.stages, fbind, f.reg, f.ub, f.nvattr, f.vattr, f.ct)) {
            st_by_tex += by_tex && f.bind != bind;
            return st_hit++, f.p;
        }
    }
    const uint64_t key = (compute ? 0x5EED : pick_inputs(s, color_types)) * 1099511628211ull ^ s.prog.serial;
    Pick& pk = picks[key];
    auto keep = [&](const Program* p) {
        if (!p || !p->ready.load(std::memory_order_acquire) || p->failed) return p;
        if (!(prog_fast[0].on && prog_fast[0].p == p)) prog_fast[1] = prog_fast[0];
        ProgFast& f = prog_fast[0];
        f.on = true, f.kind = (int)kind, f.serial = s.prog.serial, f.stages = stages, f.bind = bind, f.reg = reg, f.ub = ub;
        f.nvattr = s.nvattr, f.p = p;
        f.amask = compute ? ~0u : answer_stage_mask(*p);
        std::memcpy(f.tex_gen, s.tex_gen, sizeof f.tex_gen);
        std::memcpy(f.vattr, s.vattr, sizeof f.vattr);
        std::memcpy(f.ct, color_types, sizeof f.ct);
        return p;
    };
    if (pk.p && pk.p->ready.load(std::memory_order_acquire) && !pk.p->failed) {
        if (pk.p->answers.empty()) return st_hit++, keep(pk.p);
        static thread_local std::vector<uint32_t> now;
        answer_handles(s, *pk.p, now);
        if (now == pk.handles) return st_hit++, keep(pk.p);
        st_differ++;
        if (still_valid(s, *pk.p)) {   // (other textures of the same types: the same variant)
            pk.handles.swap(now);
            return keep(pk.p);
        }
    } else {
        st_none++;
    }
    const Program* picked = program_pick(s, kind, color_types);
    if (picked && picked->ready.load(std::memory_order_acquire) && !picked->failed) {
        pk.p = picked;
        answer_handles(s, *picked, pk.handles);
    } else {
        pk.p = nullptr;
    }
    if (picks.size() > 65536) picks.clear();
    return keep(picked);
}

const Program* program_pick(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]) {
    PROF_EXACT();   // (the picks' slow path: a translation, a cache read; PROF_PROGRAM samples the rest)
    const bool compute = kind == ProgramKind::Compute;
    std::lock_guard lk(shader_mu);
    const std::vector<Code>& cs = codes_of(s);
    const Code* stage_code[8]{};
    for (const Code& c : cs)
        if (!c.header_only) stage_code[c.stage] = &c;
    if (compute ? !stage_code[5] : !stage_code[0]) return nullptr;
    if (!compute && (stage_code[2] || stage_code[3] || stage_code[4])) {   // (geometry/tessellation: not translated)
        static int logged;
        if (logged++ < 40) log_program_stages(s, "dropped stages");
    }
    // the variants of these stages, for these vertex input types and colour output types
    const bool tonemap = !compute && scene_tonemap_for(stage_code[1]);
    const uint64_t key = compute ? stage_code[5]->key : graphics_key(*stage_code[0], stage_code[1], s.vattr, color_types, tonemap);
    auto& variants = programs[key];
    program_cache_load(key, variants);
    const uint64_t codes[4] = {stage_code[compute ? 5 : 0]->key, !compute && stage_code[1] ? stage_code[1]->key : 0, 0, 0};
    for (auto& p : variants)
        if (!p->codes[0]) std::memcpy(p->codes, codes, sizeof codes);
    for (auto& p : variants) {
        if (!p->ready.load(std::memory_order_acquire)) return p.get();   // (being translated)
        if (p->failed) return nullptr;
        if (still_valid(s, *p)) return p.get();
    }
    miss_note(key, compute ? "compute" : "graphics", stage_code[compute ? 5 : 0]->key, stage_code[1] ? stage_code[1]->key : 0, s, color_types,
              variants.size());
    auto p = std::make_unique<Program>();
    p->key = key;
    p->compute = compute;
    p->scene_tonemap = tonemap;
    std::memcpy(p->codes, codes, sizeof codes);
    Program* raw = p.get();
    variants.push_back(std::move(p));
    static const bool sync = env_on("RX_VK_SYNC_SHADERS");
    if (compute || sync) {   // (compute: dispatches are never left out, so they are translated where they are)
        translate_into(*raw, s, stage_code[0], stage_code[1], compute ? stage_code[5] : nullptr, color_types);
        return raw->failed ? nullptr : raw;
    }
    struct Job {
        Program* p;
        NvnState s;
        Code vs, fs;
        bool has_fs;
        uint8_t ct[8];
    };
    auto job = std::make_shared<Job>();
    job->p = raw, job->s = s, job->vs = *stage_code[0], job->has_fs = stage_code[1] != nullptr;
    if (job->has_fs) job->fs = *stage_code[1];
    std::memcpy(job->ct, color_types, 8);
    jobs_push([job] { translate_into(*job->p, job->s, &job->vs, job->has_fs ? &job->fs : nullptr, nullptr, job->ct); }, false,
              &raw->ready, [job]() noexcept {
                  job->p->failed = true;
                  job->p->ready.store(true, std::memory_order_release);
              });   // (as the mesh one: a waiting draw runs it, not every job queued before it)
    return raw;
}

void program_wait(const Program* p) {
    if (p && !p->ready.load(std::memory_order_acquire)) jobs_wait(p->ready);
}

/// (hookshot) Whether the environment variable var lists hex (key prefixes, comma-separated)
bool key_listed(const char* hex, const char* var) {
    const char* list = getenv(var);
    for (const char* q = list ? list : ""; *q;) {
        const char* e = strchr(q, ',');
        const size_t len = e ? (size_t)(e - q) : strlen(q);
        if (len && !strncmp(hex, q, len)) return true;
        q = e ? e + 1 : q + len;
    }
    return false;
}
// ---- (hookshot) Every shader of the game before it starts, as TotK Recomp's tools/prepare_shaders.py and totk.exe --prepare-shaders: the
// builder writes hookshot_shaders.bin (nx2/shader_census.py) from the archives, each program's stages, and here each is translated
// for the default state (nothing bound, float inputs and outputs) and kept in hookshot_programs.bin. What a draw then asks for is on
// disk, whatever part of the game it is first met in ----
namespace {
struct SeedStage {
    uint64_t key = 0;
    uint32_t stage = 0, end = 0;
    size_t control = 0, code = 0;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
};
struct Seed {
    std::vector<uint8_t> blob;
    std::vector<SeedStage> stages;
    std::vector<std::vector<uint32_t>> programs;
    std::unordered_map<uint64_t, uint32_t> index;   // (a stage by its code key: the pipeline state list's programs)
} seed;
std::string beside_exe(const char* name) {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + name;
}
Code stage_of_seed(const SeedStage& st) {
    return parse_code(seed.blob.data() + st.control, std::vector<uint8_t>(seed.blob.begin() + (ptrdiff_t)st.code,
                                                                          seed.blob.begin() + (ptrdiff_t)st.code + st.end));
}
/// (hookshot) A seed stage that does not parse fails its program: which one, and why (the prepare's "1 failed" named nothing)
void seed_stage_failed(const Code& c, uint64_t program) {
    static std::atomic<int> logged{0};
    if (logged++ < 30)
        logf("vk: program %016llx not translated: its NVN stage %d (code %016llx) %s", (unsigned long long)program, c.stage,
             (unsigned long long)c.key, c.why.empty() ? "does not parse" : c.why.c_str());
}
} // namespace

std::string seed_path() { return beside_exe("hookshot_shaders.bin"); }

size_t seed_open() {
    if (FILE* f = fopen(seed_path().c_str(), "rb")) {
        _fseeki64(f, 0, SEEK_END);
        seed.blob.resize((size_t)_ftelli64(f));
        _fseeki64(f, 0, SEEK_SET);
        if (seed.blob.size() < 16 || fread(seed.blob.data(), 1, seed.blob.size(), f) != seed.blob.size() ||
            memcmp(seed.blob.data(), "HKSEED01", 8))
            seed.blob.clear();
        fclose(f);
    }
    if (seed.blob.empty()) return 0;
    disk_init();
    disk_listing = true;
    size_t at = 16;
    // (hookshot) every count, size and index is checked against the file before it is used: a file cut short or made
    // wrong is no seed, never a read past it
    const size_t size = seed.blob.size();
    const auto u32 = [](size_t pos) { uint32_t v; std::memcpy(&v, seed.blob.data() + pos, 4); return v; };
    const auto bad = [] {
        seed.blob.clear(), seed.stages.clear(), seed.programs.clear(), seed.index.clear();
        disk_listing = false;
        return (size_t)0;
    };
    const uint32_t nprograms = u32(8), nstages = u32(12);
    if (nstages > (size - 16) / 20 || nprograms > (size - 16) / 4) return bad();
    seed.stages.resize(nstages);
    for (SeedStage& st : seed.stages) {
        if (size - at < 20) return bad();
        const uint32_t csize = u32(at + 12), bytes = u32(at + 16);
        if (csize > size - at - 20 || bytes > size - at - 20 - csize) return bad();
        st.stage = u32(at + 8);
        st.control = at + 20;
        st.code = st.control + csize;
        at = st.code + bytes;
        if (csize < kControlSize || !control_end(seed.blob.data() + st.control, &st.end) || st.end > bytes) {
            st.end = 0;   // (not one this game's programs can be)
            continue;
        }
        st.key = code_key(seed.blob.data() + st.control, seed.blob.data() + st.code, st.end);
        seed_keys.insert(st.key);
        seed.index.emplace(st.key, (uint32_t)(&st - seed.stages.data()));
        std::memcpy(st.sph, seed.blob.data() + st.code, std::min<size_t>(sizeof st.sph, st.end));
    }
    seed.programs.resize(nprograms);
    for (auto& list : seed.programs) {
        if (size - at < 4) return bad();
        const uint32_t n = u32(at);
        if (n > (size - at - 4) / 4) return bad();
        list.resize(n);
        if (n) std::memcpy(list.data(), seed.blob.data() + at + 4, (size_t)n * 4);
        for (const uint32_t k : list)
            if (k >= nstages) return bad();
        at += 4 + (size_t)n * 4;
    }
    return seed.programs.size();
}

void seed_close() {
    seed = Seed{};
    disk_listing = false;
}


std::unique_ptr<Program> seed_translate(const SeedStage* const by_stage[8], const uint32_t attributes[16], const uint8_t color_types[8],
                                        uint64_t* key_out);

/// (hookshot) How many variants of key are in memory, made or read in already, their variant hashes to *variants. They are kept
/// too, though disk_records no longer lists them: program_cache_load and program_cache_load_some take a key's records out as they
/// read them. By disk_records alone the prepare in play (prepare_background) translated again every program the precompile had
/// read, every kept compute program among them, and kept each a second time
size_t loaded_variants(uint64_t key, std::unordered_set<uint64_t>* variants) {
    std::lock_guard lk(shader_mu);
    const auto it = programs.find(key);
    if (it == programs.end()) return 0;
    size_t n = 0;
    for (const auto& p : it->second)
        if (p->ready.load(std::memory_order_acquire) && !p->failed) {
            n++;
            if (variants) variants->insert(p->variant);
        }
    return n;
}

// (the game's skinned models read their bone indices as unsigned integers from generic attributes 6 and 7: each vertex program that
// reads those is also made that way)
std::unique_ptr<Program> seed_program(size_t i, int inputs) {
    static const uint8_t no_types[8]{};
    uint32_t attributes[16]{};
    if (inputs) {
        nvn_va_st a{};
        a.fmt = NVN_FORMAT_RGBA8UI;
        std::memcpy(&attributes[6], &a, 4);
        if (inputs == 2) attributes[7] = attributes[6];
    }
    const SeedStage* by_stage[8]{};
    for (const uint32_t k : seed.programs[i]) {
        if (!seed.stages[k].end) return nullptr;
        by_stage[seed.stages[k].stage & 7] = &seed.stages[k];
    }
    const SeedStage* one = by_stage[5] ? by_stage[5] : by_stage[7] ? by_stage[7] : by_stage[0];
    if (!one) return nullptr;
    if (inputs) {   // (a variant only where it changes what the program reads)
        if (by_stage[5] || by_stage[7]) return nullptr;
        const auto read = [&](size_t g) {
            const auto used = reinterpret_cast<const Shader::ProgramHeader*>(by_stage[0]->sph)->vtg.InputGeneric(g);
            return used[0] || used[1] || used[2] || used[3];
        };
        if (!read(6) || (inputs == 2 && !read(7))) return nullptr;
    }
    return seed_translate(by_stage, attributes, no_types, nullptr);
}

/// The program of these seed stages for these vertex inputs and colour outputs, translated and kept (null: kept already, or it does
/// not translate); its key to *key_out
std::unique_ptr<Program> seed_translate(const SeedStage* const by_stage[8], const uint32_t attributes[16], const uint8_t color_types[8],
                                        uint64_t* key_out) {
    // the key first, from what the stages say of themselves: a program kept already is only read back
    Code head[8];   // (the back end's Code is a large object only once parsed)
    for (int k = 0; k < 8; k++)
        if (by_stage[k]) head[k].key = by_stage[k]->key, std::memcpy(head[k].sph, by_stage[k]->sph, sizeof head[k].sph);
    const Code* fs_head = by_stage[1] ? &head[1] : nullptr;
    const uint64_t key = by_stage[5] ? by_stage[5]->key
                         : by_stage[7] ? mesh_key(by_stage[6] ? &head[6] : nullptr, head[7], fs_head, nullptr, color_types)
                                       : graphics_key(head[0], fs_head, attributes, color_types);
    if (key_out) *key_out = key;
    bool have;
    {
        std::lock_guard lk(disk_mu);
        have = disk_records.count(key) != 0;
    }
    if (have || loaded_variants(key, nullptr)) return nullptr;
    Code code[8];
    for (int k = 0; k < 8; k++)
        if (by_stage[k]) {
            code[k] = stage_of_seed(*by_stage[k]);
            if (!code[k].ok) return seed_stage_failed(code[k], key), st_failed++, nullptr;
        }
    auto blank = std::make_unique<NvnState>();
    blank->nvattr = 16;
    std::memcpy(blank->vattr, attributes, sizeof blank->vattr);
    auto p = std::make_unique<Program>();
    p->key = key;
    p->compute = by_stage[5] != nullptr;
    const bool mesh = by_stage[7] != nullptr;
    if (mesh && !dev.mesh_shader && !dev.mesh_fallback) return nullptr;
    p->fallback = mesh && dev.uses_mesh_fallback(by_stage[6] != nullptr);
    p->mesh = mesh && !p->fallback;
    if (mesh)
        translate_mesh_into(*p, *blank, by_stage[6] ? &code[6] : nullptr, code[7], nullptr, by_stage[1] ? &code[1] : nullptr, color_types);
    else if (p->compute) {
        seed_cs_control = seed.blob.data() + by_stage[5]->control;
        translate_into(*p, *blank, nullptr, nullptr, &code[5], color_types);
        seed_cs_control = nullptr;
    } else
        translate_into(*p, *blank, &code[0], by_stage[1] ? &code[1] : nullptr, nullptr, color_types);
    return p;
}

bool seed_is_compute(size_t i) {
    for (const uint32_t k : seed.programs[i])
        if ((seed.stages[k].stage & 7) == 5) return true;
    return false;
}

// (hookshot) The seed's compute programs, each with its texture variants. A program's texture questions are answered in play by the
// textures bound (Program::answers), which nothing before the game knows; the questions themselves are few, so each is tried with every
// answer it can get, and an answer that changes the SPIR-V makes a variant. Variants of two answers and more are taken fewest first, up
// to cap: one of them is the variant play makes, which still_valid then finds kept. All go to hookshot_programs.bin, as play keeps them,
// and each new one is given to made (its modules live until it returns)
SeedVariants seed_compute(size_t i, size_t cap, const std::function<void(Program&)>& made) {
    static const uint8_t no_types[8]{};
    SeedVariants r;
    const SeedStage* cs = nullptr;
    for (const uint32_t k : seed.programs[i])
        if ((seed.stages[k].stage & 7) == 5) cs = &seed.stages[k];
    if (!cs || !cs->end) return r;
    // (the variants kept already, by play or an earlier prepare: the others are
    // still asked for, as a kept base says nothing of them)
    std::unordered_set<uint64_t> variants;
    {
        std::lock_guard lk(disk_mu);
        if (auto it = disk_records.find(cs->key); it != disk_records.end())
            for (const DiskRecord& d : it->second) {
                std::vector<uint8_t> data(d.size), plain;
                if (_fseeki64(disk_in, d.at, SEEK_SET) || fread(data.data(), 1, d.size, disk_in) != d.size) continue;
                const uint8_t* payload = nullptr;
                size_t payload_size = 0;
                if (!program_cache_payload(data.data(), data.size(), d.kind, plain, &payload, &payload_size)) continue;
                Reader rd{payload, payload + payload_size};
                Program q;
                rd.get<uint32_t>();
                rd.list(q.answers);
                if (!rd.bad) variants.insert(answers_hash(q));
            }
    }
    loaded_variants(cs->key, &variants);
    const size_t had = variants.size();
    const Code code = stage_of_seed(*cs);
    if (!code.ok) return seed_stage_failed(code, cs->key), st_failed++, r.failed = true, r;
    auto blank = std::make_unique<NvnState>();
    blank->nvattr = 16;
    seed_cs_control = seed.blob.data() + cs->control;
    struct Reset { ~Reset() { seed_cs_control = nullptr, forced = nullptr, seed_capture = nullptr; } } reset;
    Forced f;
    uint32_t count = 0, end = 0;
    f.lo = compute_ssbo_table(*blank, &count, &end) + 16 * count;   // (the texture handles follow the storage buffers)
    f.hi = end;
    struct Try {
        std::unique_ptr<Program> p;
        std::vector<std::vector<uint32_t>> spirv;
        std::vector<std::pair<uint32_t, uint8_t>> asked;
    };
    const auto run = [&](std::map<std::pair<uint32_t, uint8_t>, uint8_t> want) {
        Try t;
        f.asked.clear();
        f.want = std::move(want);
        t.p = std::make_unique<Program>();
        t.p->key = cs->key;
        t.p->compute = true;
        forced = &f, seed_capture = &t.spirv;
        translate_into(*t.p, *blank, nullptr, nullptr, &code, no_types);
        forced = nullptr, seed_capture = nullptr;
        t.asked = f.asked;
        return t;
    };
    const auto drop = [](Try& t) {
        for (Stage& st : t.p->stages)
            if (st.module) vkDestroyShaderModule(dev.device, st.module, nullptr), st.module = VK_NULL_HANDLE;
    };
    std::vector<Try> kept;
    kept.push_back(run({}));
    if (kept[0].p->failed) return drop(kept[0]), r.failed = true, r;
    const auto base_asked = kept[0].asked;
    const auto base_spirv = kept[0].spirv;
    r.questions = base_asked.size();
    // each question alone: the answers that change the SPIR-V
    std::vector<std::vector<uint8_t>> options(base_asked.size());
    for (size_t q = 0; q < base_asked.size(); q++) {
        const uint8_t kind = base_asked[q].second, plain = kind ? 0 : (uint8_t)Shader::TextureType::Color2D;
        for (uint8_t a = 0; a < (kind ? 2 : 9); a++) {
            if (a == plain) continue;
            Try t = run({{base_asked[q], a}});
            if (!t.p->failed && t.asked == base_asked && t.spirv != base_spirv && kept.size() < cap) {
                options[q].push_back(a);
                kept.push_back(std::move(t));
            } else {
                drop(t);
            }
        }
    }
    // then two answers and more, fewest first
    std::vector<size_t> open;
    for (size_t q = 0; q < options.size(); q++)
        if (!options[q].empty()) open.push_back(q);
    for (size_t n = 2; n <= open.size() && kept.size() < cap; n++) {
        std::vector<size_t> pick(n), choice(n);
        for (size_t k = 0; k < n; k++) pick[k] = k;
        for (;;) {   // (each n of the open questions, each answer of each)
            std::fill(choice.begin(), choice.end(), 0);
            for (;;) {
                if (kept.size() >= cap) break;
                std::map<std::pair<uint32_t, uint8_t>, uint8_t> want;
                for (size_t k = 0; k < n; k++) want[base_asked[open[pick[k]]]] = options[open[pick[k]]][choice[k]];
                Try t = run(std::move(want));
                if (!t.p->failed && t.asked == base_asked) kept.push_back(std::move(t));
                else drop(t);
                size_t k = 0;
                while (k < n && ++choice[k] == options[open[pick[k]]].size()) choice[k++] = 0;
                if (k == n) break;
            }
            if (kept.size() >= cap) break;
            size_t k = n;
            while (k > 0 && pick[k - 1] == open.size() - n + k - 1) k--;
            if (k == 0) break;
            pick[k - 1]++;
            for (size_t j = k; j < n; j++) pick[j] = pick[j - 1] + 1;
        }
    }
    r.capped = kept.size() >= cap;
    for (Try& t : kept) {
        if (variants.insert(t.p->variant).second) {
            program_cache_save(*t.p, t.spirv), r.variants++;
            if (made) made(*t.p);
        }
        drop(t);
    }
    r.kept = had && !r.variants;
    return r;
}


/// (hookshot) a program of the pipeline state list (vk_draw.cpp): by its stages' code keys (graphics: vertex, fragment; mesh: mesh,
/// fragment, task), as the seed holds them, and the vertex input and colour output types it was drawn with
std::unique_ptr<Program> seed_program_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8],
                                         uint64_t* key_out) {
    *key_out = 0;
    const SeedStage* by_stage[8]{};
    const int want[3] = {mesh ? 7 : 0, 1, mesh ? 6 : -1};
    for (int k = 0; k < 3; k++) {
        if (!code[k]) continue;
        const auto it = seed.index.find(code[k]);
        if (want[k] < 0 || it == seed.index.end()) return nullptr;
        const SeedStage& st = seed.stages[it->second];
        if ((int)(st.stage & 7) != want[k]) return nullptr;
        by_stage[want[k]] = &st;
    }
    if (!by_stage[want[0]]) return nullptr;
    return seed_translate(by_stage, attributes, color_types, key_out);
}
/// (seed_variant_of, seed_plain_of) the variant translated with the questions and answers given, or with none: nothing bound, each
/// slot answers what its instruction declares (as seed_translate's program), and the questions asked are noted
static const Program* seed_variant_made(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8],
                                        const std::vector<uint8_t>* questions, const std::vector<Program::Answer>* answers) {
    const SeedStage* by_stage[8]{};
    const int want[3] = {mesh ? 7 : 0, 1, mesh ? 6 : -1};
    for (int k = 0; k < 3; k++) {
        if (!code[k]) continue;
        const auto it = seed.index.find(code[k]);
        if (want[k] < 0 || it == seed.index.end()) return nullptr;
        const SeedStage& st = seed.stages[it->second];
        if ((int)(st.stage & 7) != want[k]) return nullptr;
        by_stage[want[k]] = &st;
    }
    if (!by_stage[want[0]]) return nullptr;
    Code code_of[8];
    for (int k = 0; k < 8; k++)
        if (by_stage[k]) {
            code_of[k] = stage_of_seed(*by_stage[k]);
            if (!code_of[k].ok) return nullptr;
        }
    const Code* fs = by_stage[1] ? &code_of[1] : nullptr;
    auto blank = std::make_unique<NvnState>();
    blank->nvattr = 16;
    std::memcpy(blank->vattr, attributes, sizeof blank->vattr);
    auto p = std::make_unique<Program>();
    p->scene_tonemap = !mesh && scene_tonemap_for(fs);
    p->key = mesh ? mesh_key(by_stage[6] ? &code_of[6] : nullptr, code_of[7], fs, nullptr, color_types)
                  : graphics_key(code_of[0], fs, attributes, color_types, p->scene_tonemap);
    if (mesh && !dev.mesh_shader && !dev.mesh_fallback) return nullptr;
    p->fallback = mesh && dev.uses_mesh_fallback(by_stage[6] != nullptr);
    p->mesh = mesh && !p->fallback;
    p->codes[0] = code[0], p->codes[1] = code[1], p->codes[2] = code[2];
    if (questions) questions_in = questions, questions_at = 0, answers_in = answers;
    struct Reset { ~Reset() { questions_in = nullptr, answers_in = nullptr; } } reset;
    if (mesh) translate_mesh_into(*p, *blank, by_stage[6] ? &code_of[6] : nullptr, code_of[7], nullptr, fs, color_types);
    else translate_into(*p, *blank, &code_of[0], fs, nullptr, color_types);
    if (p->failed) {
        for (Stage& st : p->stages)
            if (st.module) vkDestroyShaderModule(dev.device, st.module, nullptr);
        return nullptr;
    }
    std::lock_guard lk(shader_mu);
    auto& variants = programs[p->key];
    for (auto& v : variants)
        if (v->variant == p->variant && v->mesh == p->mesh && v->fallback == p->fallback && v->ready.load(std::memory_order_acquire) &&
            !v->failed) {
            for (Stage& st : p->stages)
                if (st.module) vkDestroyShaderModule(dev.device, st.module, nullptr);
            return v.get();
        }
    variants.push_back(std::move(p));
    return variants.back().get();
}
const Program* seed_variant_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8],
                               const std::vector<uint8_t>& questions, const std::vector<Program::Answer>& answers) {
    return seed_variant_made(code, mesh, attributes, color_types, &questions, &answers);
}
const Program* seed_plain_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8]) {
    return seed_variant_made(code, mesh, attributes, color_types, nullptr, nullptr);
}

const Program* program_find_portable(uint64_t key, uint64_t variant) {
    std::lock_guard lk(shader_mu);
    auto it = programs.find(key);
    if (it == programs.end()) return nullptr;
    for (auto& p : it->second)
        if (p->ready.load(std::memory_order_acquire) && !p->failed && portable_variant(*p) == variant) return p.get();
    return nullptr;
}
const Program* program_find_plain(uint64_t key) {
    std::lock_guard lk(shader_mu);
    auto it = programs.find(key);
    if (it == programs.end()) return nullptr;
    for (auto& p : it->second) {
        if (!p->ready.load(std::memory_order_acquire) || p->failed) continue;
        // (an answer for each slot record_answers counts, each the type its instruction declares, as record_answers makes it with
        // nothing bound: not a variant that asked nothing, which stands for any textures)
        size_t slots = 0;
        for (const Stage& st : p->stages) {
            for (const auto& d : st.info.texture_descriptors) slots += d.count == 1;
            for (const auto& d : st.info.image_descriptors) slots += d.count == 1;
        }
        bool plain = p->answers.size() == slots;
        for (size_t i = 0; plain && i < p->answers.size(); i++) {
            const Program::Answer& a = p->answers[i];
            plain = answers_fit(*p, {a}) && a.type == canonical_answer(*p, a, declared_answer(*p, a.stage, a.desc, a.image));
        }
        if (plain) return p.get();
    }
    return nullptr;
}
const Program* program_find(uint64_t key, uint64_t variant) {
    std::lock_guard lk(shader_mu);
    auto it = programs.find(key);
    if (it == programs.end()) return nullptr;
    for (auto& p : it->second)
        if (p->variant == variant && p->ready.load(std::memory_order_acquire) && !p->failed) return p.get();
    return nullptr;
}

void shader_stats(int64_t* translated, int64_t* failed) {
    static int64_t hits_logged;
    if (st_cache_hits.load() >= hits_logged + 500) logf("vk: %lld programs from the program cache", (long long)(hits_logged = st_cache_hits.load()));
    *translated = st_translated, *failed = st_failed;
}
double shader_translate_ms() {
    return st_translate_ns.load() / 1e6;
}

void program_released(const nvn_prog* p) {
    if (!p) return;
    std::lock_guard lk(shader_mu);
    codes.erase(p->serial);
}

// ---- (hookshot) A guest program as the game makes it (nvnProgramSetShaders), before any draw: vk_programs.cpp. Its stages are
// read from the game's memory and hashed as a draw's are (code_key), but not lowered: only what names the program (the stages'
// code keys) and what its key is made of (graphics_key, mesh_key: the headers). Kept by the program's serial, under a lock of
// their own: the game's thread calls this, and shader_mu is held through whole translations ----
namespace {
std::mutex created_mu;
std::unordered_map<uint64_t, std::vector<Code>> created_codes;
Code read_code_head(const nvn_stage_data& sd) {
    Code c;
    uint8_t ctl[kControlSize];
    uint32_t end = 0;
    if (!sd.control || !sd.data || !host->gpu_read(sd.control, ctl, sizeof ctl) || !control_end(ctl, &end)) return c;
    std::vector<uint8_t> area(end);
    if (!host->gpu_read(sd.data, area.data(), end)) return c;
    uint32_t len;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    c.key = code_key(ctl, area.data(), end);
    c.stage = st;
    if (st == 1) {   // (as parse_code: a scene mapping program's fragment stage)
        static constexpr float kFit[] = {2.51f, 0.03f, 2.43f, 0.59f, 0.14f};
        c.scene_fit = true;
        for (const float f : kFit) {
            uint8_t bits[4];
            std::memcpy(bits, &f, 4);
            c.scene_fit = c.scene_fit && std::search(area.begin(), area.end(), bits, bits + 4) != area.end();
        }
    }
    const bool graphics = st != 5;
    if (graphics) std::memcpy(c.sph, area.data(), std::min<size_t>(sizeof c.sph, area.size()));
    c.header_only = graphics && len <= kAmpereHeader + 16;
    c.ok = true;
    return c;
}
struct CreatedStages {
    const Code *vs = nullptr, *fs = nullptr, *task = nullptr, *mesh = nullptr, *prim = nullptr, *compute = nullptr;
};
CreatedStages created_stages(const std::vector<Code>& cs) {
    CreatedStages o;
    for (const Code& c : cs) {
        if (c.header_only) o.prim = &c;
        else if (c.stage == 0) o.vs = &c;
        else if (c.stage == 1) o.fs = &c;
        else if (c.stage == 5) o.compute = &c;
        else if (c.stage == 6) o.task = &c;
        else if (c.stage == 7) o.mesh = &c;
    }
    return o;
}
} // namespace

CreatedProgram program_created_read(const nvn_prog& prog) {
    CreatedProgram out;
    std::vector<Code> cs;
    for (uint32_t i = 0; i < prog.nstage && i < 6; i++) {
        Code c = read_code_head(prog.sd[i]);
        if (c.ok) cs.push_back(std::move(c));
    }
    const CreatedStages st = created_stages(cs);
    out.stages = (int)cs.size();
    if (st.compute) {
        out.compute = true, out.codes[0] = st.compute->key;
    } else if (st.mesh) {
        out.mesh = true, out.codes[0] = st.mesh->key, out.codes[1] = st.fs ? st.fs->key : 0;
        out.codes[2] = st.task ? st.task->key : 0, out.codes[3] = st.prim ? st.prim->key : 0;
    } else if (st.vs) {
        out.codes[0] = st.vs->key, out.codes[1] = st.fs ? st.fs->key : 0;
    } else {
        return out;
    }
    out.ok = true;
    std::lock_guard lk(created_mu);
    created_codes[prog.serial] = std::move(cs);
    return out;
}
uint64_t program_created_key(uint64_t serial, const uint32_t vattr[16], const uint8_t color_types[8]) {
    std::lock_guard lk(created_mu);
    const auto it = created_codes.find(serial);
    if (it == created_codes.end()) return 0;
    const CreatedStages st = created_stages(it->second);
    if (st.compute) return st.compute->key;
    if (st.mesh) return mesh_key(st.task, *st.mesh, st.fs, st.prim, color_types);
    if (!st.vs) return 0;
    const bool tonemap = scene_tonemap_for(st.fs);
    return graphics_key(*st.vs, st.fs, vattr, color_types, tonemap);
}
void program_created_forget(uint64_t serial) {
    std::lock_guard lk(created_mu);
    created_codes.erase(serial);
}
int64_t shader_translated() {
    return st_translated.load();
}

} // namespace mkwvk
