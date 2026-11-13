// shaderbatch: the whole seed (HKSEED01 — nx2/shader_census.py writes it, nx2/bake_mimo.py reads it) through
// Eglantine's recompiler, offline and CPU-only: every stage it can lower goes to SPIR-V, and every program key is
// computed exactly as gpu/vk/vk_shader.cpp computes it. No Vulkan device: nothing here calls vkCreateShaderModule
// or vkCreate*Pipeline, so the pass runs where no GPU is (nx2/bake_mimo.py builds and runs this).
//   shaderbatch <hookshot_shaders.bin> <out_dir> [--limit N]
// writes:
//   <out_dir>/stages.tsv      one line per seed stage: its code_key, NVN stage, status, sizes, translation facts
//   <out_dir>/programs.tsv    one line per program x input variant: the runtime's program key and what became of it
//   <out_dir>/spv/<progkey>_<stage>.spv   the SPIR-V that came out
//
// (mirror) marks each place this copies vk_shader.cpp rather than inventing: code_key, graphics_key/mesh_key, the
// Shader::Stage an NVN stage number becomes, Profile/HostTranslateInfo, and the RuntimeInfo each stage's emit takes.
// What it cannot know with no game running: the bound textures' answers (Program::answers, the variants of
// seed_compute), the driver constant banks (ReadCbufValue is the program's own c1 only), and the device limits in
// min_ssbo_alignment (16 here). Those are what a Vulkan machine fills in — BAKE.md §3 (machine stop-lines).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <typeinfo>
#include <vector>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/fragment_output_guard.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

namespace {

constexpr u32 kControlSize = 0xd8;
constexpr u32 kAmpereHeader = 128;
constexpr u32 kControlMagic = 0x98761234;
constexpr u32 HeaderSize = sizeof(Shader::ProgramHeader);   // 0x50

// (mirror vk_shader.cpp:99-103 hash_bytes, :135-137 code_key) FNV-1a 64 over the control section, then the code area
u64 hash_bytes(const void* p, size_t n, u64 h = 0xcbf29ce484222325ull) {
    const u8* b = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
u64 code_key(const u8* ctl, const u8* area, u32 end) { return hash_bytes(area, end, hash_bytes(ctl, kControlSize)); }

// (mirror vk_shader.cpp:701-720 attribute_type_of) the integer NVN formats read as integers, everything else as
// floats; the bake only ever asks it of 0 (an unset generic) and 0x27 RGBA8UI (seed_program's bone variants)
u32 attribute_type_of(u32 vattr) {
    const u32 fmt = vattr & 0xff;
    switch (fmt) {
    case 0x03: case 0x04: case 0x05: case 0x06:   // R8UI, RG8UI, RGB8UI, RGBA8UI
    case 0x0b: case 0x0c: case 0x0d: case 0x0e:   // R16UI .. RGBA16UI
    case 0x13: case 0x14: case 0x15: case 0x16:   // R32UI .. RGBA32UI
    case 0x6b:                                    // RGB10A2_UI2F (UnsignedScaled in the runtime's switch)
        return 1;                                 // AttributeType::UnsignedInt
    default:
        if (fmt >= 0x51 && fmt <= 0x6a) return ((fmt - 0x51) & 1) ? 3 : 2;   // *_UI2F / *_I2F: scaled
        return 0;                                                             // AttributeType::Float
    }
}

std::string clean(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

std::string hex64(u64 v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// ---- the seed, read exactly as seed_open reads it (vk_shader.cpp:1844-1883) ----
struct SeedStage {
    u64 stored_key = 0;   // (the seed's first field: the census's own SHA-1 key; both the runtime and this tool recompute)
    u32 stage = 0, csize = 0, bytes = 0;
    size_t control = 0, code = 0;
    bool valid = false, header_only = false;
    u32 end = 0, len = 0, c1o = 0;
    u8 sph[HeaderSize]{};
    bool graphics = false;
    u32 local[3]{}, shared = 0;
    // (NVN2 task/mesh, from the control section as parse_code reads them, vk_shader.cpp:164-172)
    u32 mesh_threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0;
};

// (mirror vk_shader.cpp:124-134 control_end) whether this is a control section, and how much code it says there is
bool control_end(const u8* ctl, u32* end_out) {
    u32 magic, len, c1o, end;
    u16 st;
    std::memcpy(&magic, ctl, 4);
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&end, ctl + 0x44, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    if (magic != kControlMagic || !len || end > (16u << 20) || c1o > end || len > end || st > 7) return false;
    return *end_out = end, true;
}

bool parse_seed(const std::vector<u8>& blob, std::vector<SeedStage>& stages, std::vector<std::vector<u32>>& programs,
                std::string& why) {
    if (blob.size() < 16 || std::memcmp(blob.data(), "HKSEED01", 8)) return why = "not HKSEED01", false;
    const u32 nprograms = *reinterpret_cast<const u32*>(blob.data() + 8);
    const u32 nstages = *reinterpret_cast<const u32*>(blob.data() + 12);
    size_t at = 16;
    stages.resize(nstages);
    for (SeedStage& st : stages) {
        if (at + 20 > blob.size()) return why = "stage record past the end", false;
        const u8* h = blob.data() + at;
        std::memcpy(&st.stored_key, h, 8);
        std::memcpy(&st.stage, h + 8, 4);
        std::memcpy(&st.csize, h + 12, 4);
        std::memcpy(&st.bytes, h + 16, 4);
        st.control = at + 20;
        st.code = st.control + st.csize;
        at = st.code + st.bytes;
        if (at > blob.size()) return why = "stage data past the end", false;
        st.graphics = st.stage != 5;
        if (st.csize < kControlSize || !control_end(blob.data() + st.control, &st.end) || st.end > st.bytes)
            continue;   // (not one this game's programs can be: seed_open zeroes it the same way)
        const u8* ctl = blob.data() + st.control;
        std::memcpy(&st.len, ctl + 0x38, 4);
        std::memcpy(&st.c1o, ctl + 0x40, 4);
        st.valid = true;
        const u32 hdr = st.graphics ? kAmpereHeader : 0;
        if (st.graphics) std::memcpy(st.sph, blob.data() + st.code, std::min<size_t>(sizeof st.sph, st.end));
        st.header_only = st.graphics && st.len <= hdr + 16;
        if (!st.graphics) {
            std::memcpy(st.local, ctl + 0x58, 12);
            std::memcpy(&st.shared, ctl + 0x64, 4);
        }
        if (st.stage == 6 || st.stage == 7) {
            // (mirror parse_code vk_shader.cpp:164-170: threads 15:0 / payload 31:16 at +0x58, primitives 15:0 /
            //  vertices 31:16 at +0x60, the output topology at +0x5f)
            u32 w58, w60;
            std::memcpy(&w58, ctl + 0x58, 4), std::memcpy(&w60, ctl + 0x60, 4);
            st.mesh_threads = w58 & 0xFFFF;
            st.payload_bytes = w58 >> 16;
            st.max_primitives = w60 & 0xFFFF;
            st.max_vertices = w60 >> 16;
            st.topology = ctl[0x5f];
        }
    }
    programs.resize(nprograms);
    for (auto& list : programs) {
        if (at + 4 > blob.size()) return why = "program record past the end", false;
        const u32 n = *reinterpret_cast<const u32*>(blob.data() + at);
        if (at + 4 + u64{n} * 4 > blob.size()) return why = "program stages past the end", false;
        const u32* p = reinterpret_cast<const u32*>(blob.data() + at + 4);
        list.assign(p, p + n);
        at += 4 + u64{n} * 4;
    }
    return true;
}

// (mirror mesh_registers_of vk_shader.cpp:1311-1326: the control sections' numbers packed the way the driver
//  sets SET_MESH_SHADER_A / B and SET_MESH_INIT_SHADER, which Eden reads back in translate_program.cpp:478-499)
std::array<u32, 3> mesh_registers_of(const SeedStage* task, const SeedStage& mesh) {
    const u32 topology = mesh.topology == 3 ? 4 : mesh.topology == 1 ? 0 : mesh.topology;
    const u32 lines = task ? (task->payload_bytes + 127) / 128 : 0;
    const u32 most = std::max(mesh.max_vertices, mesh.max_primitives);
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13),
            std::min(mesh.mesh_threads, 0x3Fu) << 20,
            (task ? std::min(task->mesh_threads, 0x3Fu) : 0) | (std::min(lines, 0x1FFu) << 12) |
                (std::min(lines, 0x1FFu) << 22)};
}

// ---- one stage: the package lowered to Maxwell words, as parse_code builds it (vk_shader.cpp:155-295) ----
struct Pkg {
    std::vector<u64> words;   // header (0x50 bytes, zeros for compute), Maxwell instructions, native address map
    std::vector<u8> c1;
    u32 texture_bound = ~0u;
    size_t native = 0;
    bool ok = false;
    std::string fail;
};

// (mirror vk_shader.cpp:155-295 parse_code: capture, LowerToMaxwell, the word stream, the c1 tail)
void build_pkg(const SeedStage& st, const u8* ctl, const u8* area, Pkg& out) {
    (void)ctl;
    const bool graphics = st.graphics;
    const u32 hdr = graphics ? kAmpereHeader : 0;
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    for (u32 i = 0; hdr + (i + 1) * sizeof(Instruction) <= st.len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area + hdr + i * sizeof(Instruction), sizeof insn);
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            out.fail = "capture: branch outside the program at +" + hex64(i * 16);
            return;
        }
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    out.native = native.size();
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    if (!lowered) {
        out.fail = "lowering failed at +" + hex64(lowered.error_offset) + ": " + std::string(lowered.error);
        return;
    }
    out.texture_bound = lowered.required_texture_buffer;
    out.words.assign(HeaderSize / 8, 0);
    if (graphics) std::memcpy(out.words.data(), area, HeaderSize);
    for (const u64 instruction : lowered.instructions) {
        if (out.words.size() * 8 % 32 == 0) out.words.push_back(0);   // the Maxwell scheduling word
        out.words.push_back(instruction);
    }
    out.words.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(out.words, lowered.source_instruction_offsets);
    out.c1.assign(area + st.c1o, area + st.end);
    out.ok = true;
}

// ---- the environment the frontend reads the lowered program from (vk_shader.cpp's Env, with no game behind it) ----
class SeedEnv final : public Shader::Environment {
public:
    SeedEnv(const Pkg& p, const SeedStage& st, Shader::Stage s) : pkg{p}, stage_info{st} {
        stage = s;
        start_address = s == Shader::Stage::Compute ? HeaderSize : 0u;   // (mirror Env's ctor, vk_shader.cpp:361)
        // (mirror Env's ctor :362-367: the program header goes into the Environment itself — Eden reads env.SPH()
        //  for the mesh output lines and the geometry header fields — and the environment is a proprietary driver)
        if (s != Shader::Stage::Compute) std::memcpy(&sph, stage_info.sph, sizeof sph);
        is_proprietary_driver = true;
    }
    /// (mirror Env::set_mesh vk_shader.cpp:463-469): SET_MESH_SHADER_A / B, SET_MESH_INIT_SHADER registers, and
    /// the pass-through geometry header whose output map lists the per-primitive outputs (null for the seed path)
    void set_mesh(const std::array<u32, 3>& registers, const u8* primitive_sph) {
        mesh_registers = registers;
        if (primitive_sph) std::memcpy(&mesh_primitive_header, primitive_sph, sizeof mesh_primitive_header);
    }
    u64 ReadInstruction(u32 address) override {
        return address / 8 < pkg.words.size() ? pkg.words[address / 8] : 0;
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(pkg.words, address)) return *t;
        throw Shader::RuntimeError("invalid native indirect branch address {:#x}", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        // (mirror driver_bank_word vk_bank.cpp:405: one word of the vertex stage's bank is a constant, blank
        //  state or not — 2047.9375f at c0+0x610; everything else a blank NvnState gives is 0)
        if (index == 0 && stage_info.stage == 0 && offset == 0x610) {
            u32 v;
            const float far_plane = 2047.9375f;
            std::memcpy(&v, &far_plane, 4);
            return v;
        }
        // (mirror vk_shader.cpp:381-399: c1 is the program's own constants; the driver banks are game state this pass
        // has no game for, and they read as 0 — which is what makes IsCbufStorageBufferPointer false below)
        if (index == 1 && (size_t)offset + 4 <= pkg.c1.size()) {
            u32 v;
            std::memcpy(&v, pkg.c1.data() + offset, 4);
            return v;
        }
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat{}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override {
        // (mirror vk_shader.cpp:418-421: the buffer the native TEX names, else c0 for NVN2 compute, c2 otherwise)
        if (pkg.texture_bound != ~0u) return pkg.texture_bound;
        return stage == Shader::Stage::Compute ? 0 : 2;
    }
    u32 LocalMemorySize() const override {
        // (mirror vk_shader.cpp:363-364: the graphics header's own local memory; compute declares none there)
        if (stage == Shader::Stage::Compute) return 0;
        Shader::ProgramHeader sph;
        std::memcpy(&sph, stage_info.sph, sizeof sph);
        return static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
    }
    u32 SharedMemorySize() const override { return stage_info.shared; }   // (mirror :419-421, sized by the control section)
    std::array<u32, 3> WorkgroupSize() const override {
        // (mirror vk_shader.cpp:422-424: the control section's +0x58, each at least 1)
        return {std::max(1u, stage_info.local[0]), std::max(1u, stage_info.local[1]), std::max(1u, stage_info.local[2])};
    }
    bool HasHLEMacroState() const override {
        // (mirror vk_shader.cpp:434-437: the vertex stage reads the draw's parameters from the driver bank)
        return stage == Shader::Stage::VertexA || stage == Shader::Stage::VertexB;
    }
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32, u32) override { return false; }   // (mirror :443-451 with nothing bound)
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        // (mirror vk_shader.cpp:452-460: offset-based, so it works with no game running)
        if (bank != 0 || !HasHLEMacroState()) return std::nullopt;
        switch (offset) {
        case 0x640: return Shader::ReplaceConstant::BaseVertex;
        case 0x644: return Shader::ReplaceConstant::BaseInstance;
        case 0x648: return Shader::ReplaceConstant::DrawID;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}

    const Pkg& pkg;
    const SeedStage& stage_info;
};

// (mirror vk_shader.cpp:528-533 Translation: the IR program with the pools its instructions live in)
struct Translation {
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::IR::Program program;
};

// (mirror vk_shader.cpp:456-474 make_profile; min_ssbo_alignment is a device limit, 16 offline)
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
    p.supported_subgroup_stages = 0x7FFFFFFF;   // VK_SHADER_STAGE_ALL, without this tool's Vulkan headers
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
    p.min_ssbo_alignment = 16;
    return p;
}

// (mirror vk_shader.cpp:499-515 host_info; min_ssbo_alignment is the device's again, 16 offline)
Shader::HostTranslateInfo make_host() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    h.support_float16 = true;
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = 16;
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;
    return h;
}

// (mirror the by_stage[] choices of seed_program: NVN 0 vertex, 1 fragment, 5 compute; 6/7 are NVN2 task/mesh,
// which translate through translate_mesh_into's own path)
Shader::Stage nvn_stage_of(int s) {
    switch (s) {
    case 0: return Shader::Stage::VertexB;
    case 1: return Shader::Stage::Fragment;
    case 2: return Shader::Stage::Geometry;
    case 3: return Shader::Stage::TessellationControl;
    case 4: return Shader::Stage::TessellationEval;
    case 6: return Shader::Stage::Task;
    case 7: return Shader::Stage::Mesh;
    default: return Shader::Stage::Compute;
    }
}

struct StageOut {   // (one stages.tsv line; the first attempt at a stage is the one recorded)
    std::string status = "NOT_ATTEMPTED", detail;
    size_t native = 0, spirv_words = 0;
    u64 cbuf_mask = 0;
    size_t textures = 0, storage = 0, images = 0;
};

struct EmitOut {
    bool ok = false;
    std::string status = "NOT_ATTEMPTED", detail;
    std::vector<u32> spirv;
    u64 cbuf_mask = 0;
    size_t textures = 0, storage = 0, images = 0;
    size_t native = 0;
};

}   // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: shaderbatch <hookshot_shaders.bin> <out_dir> [--limit N]\n");
        return 2;
    }
    size_t limit = ~size_t{0};
    if (argc > 4 && !std::strcmp(argv[3], "--limit")) limit = std::strtoul(argv[4], nullptr, 10);

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) return std::fprintf(stderr, "%s: cannot read\n", argv[1]), 2;
    std::vector<u8> blob((std::istreambuf_iterator<char>(in)), {});
    std::vector<SeedStage> stages;
    std::vector<std::vector<u32>> programs;
    std::string why;
    if (!parse_seed(blob, stages, programs, why)) return std::fprintf(stderr, "%s: %s\n", argv[1], why.c_str()), 2;

    std::filesystem::path out = argv[2];
    std::error_code ec;
    std::filesystem::create_directories(out / "spv", ec);
    if (ec) return std::fprintf(stderr, "%s: %s\n", out.c_str(), ec.message().c_str()), 2;

    // ---- the stage keys, exactly as the runtime recomputes them from the same bytes (seed_open :1872) ----
    std::vector<u64> keys(stages.size(), 0);
    for (size_t i = 0; i < stages.size(); i++)
        if (stages[i].valid) keys[i] = code_key(blob.data() + stages[i].control, blob.data() + stages[i].code, stages[i].end);

    std::map<size_t, StageOut> stage_out;   // (first attempt at a stage wins its line)
    const auto record = [&](int idx, const EmitOut& e, const Pkg* pkg) {
        if (stage_out.count(idx)) return;
        StageOut s;
        s.status = e.status;
        s.detail = e.detail;
        s.spirv_words = e.spirv.size();
        s.cbuf_mask = e.cbuf_mask;
        s.textures = e.textures;
        s.storage = e.storage;
        s.images = e.images;
        s.native = pkg ? pkg->native : 0;
        stage_out.emplace(idx, std::move(s));
    };

    const Shader::Profile profile = make_profile();
    const Shader::HostTranslateInfo host = make_host();

    // ---- one stage's translation, cached (the IR does not depend on the program around it) ----
    struct TrRes {
        std::unique_ptr<Translation> tr;
        std::string err;
        Pkg pkg;
    };
    std::map<int, TrRes> tr_cache;
    // (the task/mesh pair's registers, set before those stages translate: Eden reads them back for the payload
    //  size, topology and output counts, translate_program.cpp:478-503; the mesh branch fills this in first)
    std::map<int, std::array<u32, 3>> mesh_regs;
    auto translate_stage = [&](int idx) -> const TrRes& {
        auto it = tr_cache.find(idx);
        if (it != tr_cache.end()) return it->second;
        TrRes r;
        const SeedStage& st = stages[idx];
        build_pkg(st, blob.data() + st.control, blob.data() + st.code, r.pkg);
        if (!r.pkg.ok) {
            r.err = r.pkg.fail;
        } else {
            r.tr = std::make_unique<Translation>();
            try {
                SeedEnv env(r.pkg, st, nvn_stage_of(st.stage & 7));
                // (mirror vk_shader.cpp:534-539: graphics starts after the program header, compute at 0x50;
                //  task/mesh get one_exit, as translate(...) is called with it for them, :1338 and :1351)
                const bool one_exit = (st.stage & 7) >= 6;
                if (one_exit) {
                    const auto regs = mesh_regs.find(idx);
                    if (regs != mesh_regs.end()) env.set_mesh(regs->second, nullptr);   // (prim=null: seed_program :1945)
                }
                Shader::Maxwell::Flow::CFG cfg{env, r.tr->flow_blocks,
                                               env.StartAddress() + (st.graphics ? HeaderSize : 0u), one_exit};
                r.tr->program = Shader::Maxwell::TranslateProgram(r.tr->insts, r.tr->blocks, env, cfg, host);
            } catch (const std::exception& e) {
                r.err = std::string(e.what()) + " [" + typeid(e).name() + "]";
                std::fprintf(stderr, "shaderbatch: stage %d translate threw: %s\n", idx, r.err.c_str());
                r.tr.reset();
            } catch (...) {
                r.err = "unknown non-std exception";
                std::fprintf(stderr, "shaderbatch: stage %d translate threw a non-std exception\n", idx);
                r.tr.reset();
            }
        }
        return tr_cache.emplace(idx, std::move(r)).first->second;
    };

    // ---- one stage's emit in one role, cached by what the emit depends on ----
    struct EmitCtx {
        int idx;
        int role;      // 0 compute, 1 vertex, 2 fragment
        u64 vs_key;    // fragment: the program key it is emitted for (previous_stage_stores come from that vertex stage)
        u32 inputs;    // vertex: seed_program's 0 / 1 / 2 (generics 6 and 7 as RGBA8UI)
        bool operator<(const EmitCtx& o) const { return std::tie(idx, role, vs_key, inputs) < std::tie(o.idx, o.role, o.vs_key, o.inputs); }
    };
    std::map<EmitCtx, EmitOut> emit_cache;

    auto emit_stage = [&](const EmitCtx& ctx) -> const EmitOut& {
        auto found = emit_cache.find(ctx);
        if (found != emit_cache.end()) return found->second;
        const TrRes& tr = translate_stage(ctx.idx);
        EmitOut e;
        if (!tr.tr) {
            e.status = tr.pkg.fail.empty() ? "TRANSLATE_ERROR" : "LOWER_ERROR";
            e.detail = tr.err;
            e.native = tr.pkg.native;
            return emit_cache.emplace(ctx, std::move(e)).first->second;
        }
        e.native = tr.pkg.native;
        const SeedStage& st = stages[ctx.idx];
        if (st.header_only) {   // (a graphics header and no code: parse_code returns early with one, :174-179)
            e.status = "HEADER_ONLY";
            e.detail = "the control section sizes a header and no instructions";
            return emit_cache.emplace(ctx, std::move(e)).first->second;
        }
        try {
            Shader::Backend::Bindings bindings{};
            Shader::RuntimeInfo rt{};
            std::vector<u32> spirv;
            if (ctx.role == 1) {
                // (mirror seed_program's blank state :1915-1926 and translate_into's vrt :1211-1216)
                u32 attributes[16]{};
                if (ctx.inputs) attributes[6] = 0x27;   // NVN_FORMAT_RGBA8UI
                if (ctx.inputs == 2) attributes[7] = 0x27;
                for (int i = 0; i < 16; i++)
                    rt.generic_input_types[i] = static_cast<Shader::AttributeType>(attribute_type_of(attributes[i]));
                rt.previous_stage_stores.mask.set();
                rt.convert_depth_mode = false;   // (host->depth_minus_w() of a device: 0 here)
                if (!tr.tr->program.info.stores[Shader::IR::Attribute::PointSize]) rt.fixed_state_point_size = 1.0f;
                spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, tr.tr->program, bindings);
            } else {
                // (compute :1174-1180 RuntimeInfo{}; task takes all previous-stage stores, mirror :1339-1340;
                //  the fragment path fills its stores in emit_fs)
                if (ctx.role == 3) rt.previous_stage_stores.mask.set();
                spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, tr.tr->program, bindings);
            }
            if (spirv.empty() || spirv[0] != 0x07230203u) {
                e.status = "EMIT_ERROR";
                e.detail = "the back end returned no SPIR-V module";
            } else {
                e.ok = true;
                e.status = "OK";
                e.spirv = std::move(spirv);
            }
        } catch (const std::exception& ex) {
            e.status = "EMIT_ERROR";
            e.detail = ex.what();
        }
        const Shader::Info& info = tr.tr->program.info;
        e.cbuf_mask = info.constant_buffer_mask;
        e.textures = info.texture_descriptors.size();
        e.storage = info.storage_buffers_descriptors.size();
        e.images = info.image_descriptors.size();
        return emit_cache.emplace(ctx, std::move(e)).first->second;
    };

    // (the fragment emit: frt.previous_stage_stores = the stage before it stores, translate_into :1235-1246; after
    //  a mesh program its per-primitive generics and point-sprite coordinates come too, :1385-1398)
    auto emit_fs = [&](int idx, u64 for_program_key, const Translation* prev, bool after_mesh) -> const EmitOut& {
        const EmitCtx ctx{idx, after_mesh ? 5 : 2, for_program_key, 0};
        auto found = emit_cache.find(ctx);
        if (found != emit_cache.end()) return found->second;
        const TrRes& tr = translate_stage(idx);
        EmitOut e;
        if (!tr.tr) {
            e.status = tr.pkg.fail.empty() ? "TRANSLATE_ERROR" : "LOWER_ERROR";
            e.detail = tr.err;
            e.native = tr.pkg.native;
            return emit_cache.emplace(ctx, std::move(e)).first->second;
        }
        e.native = tr.pkg.native;
        if (stages[idx].header_only) {
            e.status = "HEADER_ONLY";
            e.detail = "the control section sizes a header and no instructions";
            return emit_cache.emplace(ctx, std::move(e)).first->second;
        }
        try {
            Shader::Backend::Bindings bindings{};
            Shader::RuntimeInfo rt{};
            rt.previous_stage_stores = prev->program.info.stores;
            if (after_mesh) {
                for (const u32 line : prev->program.mesh.primitive_lines) {
                    const auto attribute = static_cast<Shader::IR::Attribute>(line);
                    if (Shader::IR::IsGeneric(attribute))
                        rt.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
                }
                if (prev->program.mesh.topology == 0)
                    for (size_t i = 0; i < Shader::IR::NUM_GENERICS; i++)
                        if (tr.tr->program.info.loads.Generic(i) && !prev->program.info.stores.Generic(i))
                            rt.point_coord_generics.set(i);
            }
            for (int i = 0; i < 8; i++) rt.color_output_types[i] = Shader::AttributeType::Float;
            Shader::Optimization::GuardFragmentOutputs(tr.tr->program, 0xFFu);
            const std::vector<u32> spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, tr.tr->program, bindings);
            if (spirv.empty() || spirv[0] != 0x07230203u) {
                e.status = "EMIT_ERROR";
                e.detail = "the back end returned no SPIR-V module";
            } else {
                e.ok = true;
                e.status = "OK";
                e.spirv = spirv;
            }
        } catch (const std::exception& ex) {
            e.status = "EMIT_ERROR";
            e.detail = ex.what();
        }
        const Shader::Info& info = tr.tr->program.info;
        e.cbuf_mask = info.constant_buffer_mask;
        e.textures = info.texture_descriptors.size();
        e.storage = info.storage_buffers_descriptors.size();
        e.images = info.image_descriptors.size();
        return emit_cache.emplace(ctx, std::move(e)).first->second;
    };

    // (the mesh emit, mirror translate_mesh_into :1354-1360: the task stage's stores, or every bit when the
    //  program has no task stage — seed_program passes prim=null, so neither does this pass)
    auto emit_mesh = [&](int idx, u64 for_program_key, const Translation* task) -> const EmitOut& {
        const EmitCtx ctx{idx, 4, for_program_key, 0};
        auto found = emit_cache.find(ctx);
        if (found != emit_cache.end()) return found->second;
        const TrRes& tr = translate_stage(idx);
        EmitOut e;
        if (!tr.tr) {
            e.status = tr.pkg.fail.empty() ? "TRANSLATE_ERROR" : "LOWER_ERROR";
            e.detail = tr.err;
            e.native = tr.pkg.native;
            return emit_cache.emplace(ctx, std::move(e)).first->second;
        }
        e.native = tr.pkg.native;
        if (stages[idx].header_only) {
            e.status = "HEADER_ONLY";
            e.detail = "the control section sizes a header and no instructions";
            return emit_cache.emplace(ctx, std::move(e)).first->second;
        }
        try {
            Shader::Backend::Bindings bindings{};
            Shader::RuntimeInfo rt{};
            if (task) rt.previous_stage_stores = task->program.info.stores;
            else rt.previous_stage_stores.mask.set();
            rt.convert_depth_mode = false;   // (host->depth_minus_w() of a device: 0 here)
            const std::vector<u32> spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, tr.tr->program, bindings);
            if (spirv.empty() || spirv[0] != 0x07230203u) {
                e.status = "EMIT_ERROR";
                e.detail = "the back end returned no SPIR-V module";
            } else {
                e.ok = true;
                e.status = "OK";
                e.spirv = spirv;
            }
        } catch (const std::exception& ex) {
            e.status = "EMIT_ERROR";
            e.detail = ex.what();
        }
        const Shader::Info& info = tr.tr->program.info;
        e.cbuf_mask = info.constant_buffer_mask;
        e.textures = info.texture_descriptors.size();
        e.storage = info.storage_buffers_descriptors.size();
        e.images = info.image_descriptors.size();
        return emit_cache.emplace(ctx, std::move(e)).first->second;
    };

    // ---- the program keys (mirror graphics_key :733-743 and mesh_key :744-752 over the stages' code keys) ----
    auto graphics_key = [&](int vs_idx, int fs_idx, u32 inputs) {
        u32 attributes[16]{};
        if (inputs) attributes[6] = 0x27;
        if (inputs == 2) attributes[7] = 0x27;
        u64 key = keys[vs_idx] ^ (fs_idx >= 0 ? keys[fs_idx] * 31 : 0);
        const Shader::ProgramHeader& sph = *reinterpret_cast<const Shader::ProgramHeader*>(stages[vs_idx].sph);
        for (int i = 0; i < 16; i++) {
            const auto used = sph.vtg.InputGeneric(static_cast<size_t>(i));
            const bool read = used[0] || used[1] || used[2] || used[3];
            key = key * 1099511628211ull ^ (u64)(read ? attribute_type_of(attributes[i]) : 0u) << i;
        }
        const u8 written[8]{};   // (seed_program's no_types: written_types keeps zeros whatever the fragment writes)
        return hash_bytes(written, sizeof written, key);
    };
    auto mesh_key = [&](int task_idx, int mesh_idx, int fs_idx) {
        u64 key = hash_bytes("mesh", 4, keys[mesh_idx]);
        if (task_idx >= 0) key = key * 1099511628211ull ^ keys[task_idx];
        if (fs_idx >= 0) key = key * 1099511628211ull ^ keys[fs_idx];
        const u8 written[8]{};
        return hash_bytes(written, sizeof written, key);
    };

    // ---- every program, in seed order ----
    std::ofstream stages_tsv(out / "stages.tsv"), programs_tsv(out / "programs.tsv");
    programs_tsv << "#index\tinputs\tkey\tkind\tstages\tstage_keys\tstatus\tspirv\tdetail\n";
    size_t done = 0, translated = 0, failed = 0, skipped = 0;
    for (size_t pi = 0; pi < programs.size() && pi < limit; pi++) {
        int by_stage[8];
        for (int& s : by_stage) s = -1;
        bool invalid = false;
        for (const u32 k : programs[pi]) {
            if (k >= stages.size()) { invalid = true; continue; }
            if (!stages[k].valid) invalid = true;
            by_stage[stages[k].stage & 7] = int(k);   // (last wins, as seed_program's loop does)
        }
        const auto stage_list = [&] {
            std::string s, ks;
            for (int s_i = 0; s_i < 8; s_i++)
                if (by_stage[s_i] >= 0) {
                    s += s.empty() ? "" : ",";
                    s += std::to_string(s_i);
                    ks += ks.empty() ? "" : ",";
                    ks += hex64(keys[by_stage[s_i]]);
                }
            return std::make_pair(s, ks);
        };
        const auto row = [&](int inputs, u64 key, const char* kind, const std::string& status, const std::string& spirv,
                             const std::string& detail) {
            const auto [s, ks] = stage_list();
            programs_tsv << pi << '\t' << inputs << '\t' << hex64(key) << '\t' << kind << '\t' << s << '\t' << ks << '\t'
                         << clean(status) << '\t' << clean(spirv) << '\t' << clean(detail) << '\n';
        };
        if (invalid) {
            row(0, 0, "invalid", "INVALID_STAGE", "", "a stage of this program is not one the back end can read");
            failed++;
            continue;
        }
        // kind, as seed_program chooses it: compute, then mesh, then a vertex stage (:1907-1911)
        if (by_stage[5] >= 0) {   // ---- compute ----
            const int idx = by_stage[5];
            const EmitOut& e = emit_stage({idx, 0, 0, 0});
            const std::string file = e.ok ? "spv/" + hex64(keys[idx]) + "_5.spv" : "";
            if (e.ok) {
                std::ofstream f(out / file, std::ios::binary);
                f.write(reinterpret_cast<const char*>(e.spirv.data()), e.spirv.size() * 4);
            }
            record(idx, e, &tr_cache[idx].pkg);
            row(0, keys[idx], "compute", e.status, file, e.detail);
            e.ok ? translated++ : failed++;
        } else if (by_stage[7] >= 0) {   // ---- NVN2 mesh: task, mesh, and the fragment after them ----
            // (mirror translate_mesh_into vk_shader.cpp:1328-1419 with prim=null — which is what the seed path
            //  passes at seed_program :1945 — and mesh_registers_of :1311-1326 from the control sections)
            const int mesh = by_stage[7], task = by_stage[6], fs = by_stage[1];
            const u64 key = mesh_key(task, mesh, fs);
            std::string status = "OK", detail, spirv;
            const auto write = [&](const EmitOut& e, const char* stage_no) {
                const std::string file = "spv/" + hex64(key) + "_" + stage_no + ".spv";
                std::ofstream f(out / file, std::ios::binary);
                f.write(reinterpret_cast<const char*>(e.spirv.data()), e.spirv.size() * 4);
                spirv += spirv.empty() ? "" : ",";
                spirv += file;
            };
            const auto fail_with = [&](const EmitOut& e, const char* what) {
                if (status == "OK" && !e.ok) status = e.status, detail = std::string(what) + ": " + e.detail;
            };
            const std::array<u32, 3> regs = mesh_registers_of(task >= 0 ? &stages[task] : nullptr, stages[mesh]);
            if (task >= 0) mesh_regs[task] = regs;
            mesh_regs[mesh] = regs;
            const Translation* task_tr = nullptr;
            if (task >= 0) {
                const EmitOut& et = emit_stage({task, 3, 0, 0});
                record(task, et, &tr_cache[task].pkg);
                if (et.ok) {
                    write(et, "6");
                    task_tr = tr_cache[task].tr.get();
                } else
                    fail_with(et, "task");
            }
            const EmitOut& em = emit_mesh(mesh, key, task_tr);
            record(mesh, em, &tr_cache[mesh].pkg);
            const Translation* mesh_tr = em.ok ? tr_cache[mesh].tr.get() : nullptr;
            if (em.ok)
                write(em, "7");
            else
                fail_with(em, "mesh");
            if (fs >= 0 && mesh_tr) {
                const EmitOut& ef = emit_fs(fs, key, mesh_tr, true);
                record(fs, ef, &tr_cache[fs].pkg);
                if (ef.ok)
                    write(ef, "1");
                else
                    fail_with(ef, "fragment");
            }
            row(0, key, "mesh", status, spirv, detail);
            status == "OK" ? translated++ : failed++;
        } else if (by_stage[0] >= 0) {   // ---- graphics: vertex, and fragment after it ----
            const int vs = by_stage[0], fs = by_stage[1];
            for (u32 inputs = 0;; inputs++) {
                const u64 key = graphics_key(vs, fs, inputs);
                const EmitOut& ev = emit_stage({vs, 1, 0, inputs});
                std::string status = ev.status, detail = ev.detail, spirv;
                const Translation* vst = tr_cache[vs].tr.get();
                if (ev.ok) {
                    const std::string file = "spv/" + hex64(key) + "_0.spv";
                    std::ofstream f(out / file, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(ev.spirv.data()), ev.spirv.size() * 4);
                    spirv += file;
                    if (fs >= 0 && vst) {
                        const EmitOut& ef = emit_fs(fs, key, vst, false);
                        record(fs, ef, &tr_cache[fs].pkg);
                        if (ef.ok) {
                            const std::string f2 = "spv/" + hex64(key) + "_1.spv";
                            std::ofstream g(out / f2, std::ios::binary);
                            g.write(reinterpret_cast<const char*>(ef.spirv.data()), ef.spirv.size() * 4);
                            spirv += "," + f2;
                        } else if (status == "OK") {
                            status = ef.status;
                            detail = "fragment: " + ef.detail;
                        }
                    }
                }
                record(vs, ev, &tr_cache[vs].pkg);
                row(inputs, key, "graphics", status, spirv, detail);
                status == "OK" ? translated++ : failed++;
                // (seed_program's input variants: only where the vertex stage reads generic 6, then 7 :1913-1926)
                if (inputs == 0) {
                    const Shader::ProgramHeader& sph = *reinterpret_cast<const Shader::ProgramHeader*>(stages[vs].sph);
                    const auto used6 = sph.vtg.InputGeneric(6);
                    if (!(used6[0] || used6[1] || used6[2] || used6[3])) break;
                } else if (inputs == 1) {
                    const Shader::ProgramHeader& sph = *reinterpret_cast<const Shader::ProgramHeader*>(stages[vs].sph);
                    const auto used7 = sph.vtg.InputGeneric(7);
                    if (!(used7[0] || used7[1] || used7[2] || used7[3])) break;
                }
                if (inputs >= 2) break;
            }
            done++;
            continue;
        } else {
            row(0, 0, "none", "NO_VS", "", "a program with no vertex, compute or mesh stage");
            failed++;
            done++;
            continue;
        }
        done++;
    }

    stages_tsv << "#key\tnvn_stage\tstatus\tdetail\tcontrol_bytes\tcode_bytes\tnative\tspirv_words\tcbuf_mask\ttextures\tstorage\timages\n";
    for (size_t i = 0; i < stages.size(); i++) {
        const SeedStage& st = stages[i];
        const auto it = stage_out.find(int(i));
        const StageOut s = it != stage_out.end() ? it->second : StageOut{};
        stages_tsv << (st.valid ? hex64(keys[i]) : "-") << '\t' << st.stage << '\t' << clean(s.status) << '\t'
                   << clean(s.detail) << '\t' << st.csize << '\t' << st.bytes << '\t' << s.native << '\t'
                   << s.spirv_words << '\t' << s.cbuf_mask << '\t' << s.textures << '\t' << s.storage << '\t'
                   << s.images << '\n';
    }
    std::printf("shaderbatch: %zu programs (%zu translated, %zu failed, %zu skipped), %zu stages, %zu translations, %zu emits\n",
                done, translated, failed, skipped, stages.size(), tr_cache.size(), emit_cache.size());
    return 0;
}
