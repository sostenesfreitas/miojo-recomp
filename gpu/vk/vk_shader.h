// NVN2 programs as SPIR-V modules (vk_shader.cpp) and what the pipelines need of them.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/shader_info.h"

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {

struct Stage {
    int nvn_stage = 0;   // 0 vertex, 1 fragment, 5 compute (what its banks are read for)
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;
    Shader::Info info;
    std::vector<uint8_t> c1;   // its c1 constants (the code area's tail)
    // (hookshot) its SPIR-V's OpPhi instructions (fallback_size_policy.h), noted where the SPIR-V is written to or read from the
    // program cache (vk_progcache.cpp), which every translated program passes. Not in vk_shader.cpp, whose bytes are part of
    // the translator's stamp: a change there translates every player's programs again
    mutable uint32_t phis = 0;
};

/// A texture handle the stage reads from a storage buffer: its handle program (Shader::Info::handle_programs, Eden's
/// recompiler) run over the stage's constant banks and storage buffers as the draw has them
uint32_t stage_handle(const NvnState& s, const Stage& st, uint32_t program);

/// A translated program variant: its stages, in descriptor order. The pipeline layout and pipelines made for it live
/// in vk_pipeline.cpp (keyed by key).
struct Program {
    uint64_t key = 0;
    bool compute = false, failed = false;
    std::atomic<bool> ready{false};   // translated (graphics programs are translated on a worker: vk_jobs.h)
    std::vector<Stage> stages;
    std::array<uint32_t, 3> workgroup{1, 1, 1};
    uint32_t shared_memory = 0;
    // made by vk_pipeline.cpp at first use
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorType> binding_types;   // by binding (what each descriptor write is)
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    // (the compute pipeline's build: 0 not started, 1 on a worker, 2 done (compute_pipeline made, or failed): vk_draw.cpp dispatch)
    std::atomic<int> compute_state{0};
    mutable std::atomic<bool> dispatched{false};   // (hookshot) noted in hookshot_computes.bin (vk_draw.cpp compute_note)
    bool push = false;             // its set is pushed (VK_KHR_push_descriptor)
    VkShaderStageFlags push_stages = 0;
    bool uses_render_area = false;
    bool uses_push = true;   // (a stage reads the push constants: render area or rescaling)
    bool mesh = false;              // task / mesh stages (VK_EXT_mesh_shader)
    bool scene_tonemap = false;     // (hookshot) its scene tone map is PsychoV35 (vk_shader.cpp scene_tonemap_for; in its key)
    // (hookshot) the GPU has no mesh shaders: the task and mesh stages are compute pipelines, and a generated vertex
    // stage reads the records they write. Its stages are (task,) mesh, vertex, fragment.
    bool fallback = false;
    bool fallback_task = false;   // (a task stage before the mesh stage: its payloads are per mesh workgroup)
    VkPrimitiveTopology fallback_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t fallback_vertices_per_workgroup = 1;   // (max primitives x corners: the records of one mesh workgroup)
    uint32_t fallback_record_words = 1;             // (validity, then the vertex and primitive lines)
    uint32_t fallback_payload_words = 0;
    uint32_t fallback_task_threads = 1;             // (mesh workgroups one task workgroup may start)
    std::vector<VkPipeline> fallback_compute;       // (per stage: the mesh program's dispatches)
    // What the translator was told about the bound textures (when it asked: most texture instructions carry their
    // type themselves), by stage and texture descriptor: the variant stands while the textures still answer the same
    struct Answer {
        uint16_t stage, desc;
        uint8_t type;   // Shader::TextureType | 0x80 when the format is integer
        bool image = false;   // (hookshot) a storage image descriptor's: only whether its format is integer
    };
    std::vector<Answer> answers;
    uint64_t variant = 0;   // (a hash of the answers: key alone names all the variants of these stages)
    mutable std::atomic<int64_t> wm_sig{-1};   // (hookshot: what its write masks are made from, vk_draw.cpp program_wm_sig; -1: not asked yet)
    // (hookshot) the translation's texture questions as answered, in the order asked: what makes this variant again with nothing
    // bound (the pipeline state list, vk_draw.cpp)
    std::vector<uint8_t> questions;
    uint64_t codes[4]{};   // (hookshot) its stages' code keys: vertex (mesh), fragment, task, the header-only stage (the state list)
    // (hookshot) a compute program read from the cache without its code (vk_progcache.cpp program_cache_read_light): where its
    // SPIR-V is in hookshot_programs.bin, and how many words; 0 words: it has its module (program_cache_complete makes it)
    int64_t code_at = 0;
    uint64_t code_words = 0;
};

enum class ProgramKind { Graphics, Compute, Mesh };

/// The program variant for the bound state (translated at first use; null when it does not translate).
/// color_types: the attachments' Shader::AttributeType (Float / SignedInt / UnsignedInt), for the fragment outputs.
/// Graphics programs may come back not ready yet (translation queued): the draw leaves itself out or program_wait()s.
const Program* program_for(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]);
uint64_t compute_code_key(const NvnState& s);   // (hookshot) the bound compute program's code key, 0: none
std::string program_textures(const NvnState& s, const Program& p);   // (hookshot, for logs)
std::string program_code_keys(const NvnState& s);   // (hookshot) " stage:key" for each of the bound program's codes
const Program* program_pick(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]);   // (program_for without its cache)
void program_wait(const Program* p);
uint64_t program_cache_stamp();   // (what the kept programs and pipelines are good for)
size_t program_cache_load_some(const std::unordered_set<uint64_t>& wanted, const std::function<void(Program&)>& fn,
                               bool computes = true, bool fallbacks = false);   // (computes: and every kept compute program)
/// The precompile of every shader of the game (hookshot_shaders.bin, from nx2/shader_census.py): how many programs it names, 0: none
size_t seed_open();
std::string seed_path();   // (hookshot_shaders.bin, beside the game)
void seed_close();
/// Program i of the seed, translated and kept for the default state (null: kept already, or it does not translate); its modules are yours
std::unique_ptr<Program> seed_program(size_t i, int inputs = 0);
bool seed_is_compute(size_t i);
/// (hookshot) Seed program i, a compute program, kept in hookshot_programs.bin with every texture variant its questions can make (up
/// to cap variants); made is given each variant not kept before
struct SeedVariants {
    size_t variants = 0, questions = 0;
    bool kept = false, failed = false, capped = false;
};
SeedVariants seed_compute(size_t i, size_t cap, const std::function<void(Program&)>& made);
/// (hookshot, the prediction: vk_predict.cpp) a kept graphics program variant as hookshot_programs.bin has it: each stage's SPIR-V
struct KeptProgram {
    uint64_t key = 0, variant = 0;
    int64_t at = 0;   // (where its record is in the file: of two records of one variant, the later is the one kept)
    bool mesh = false;
    std::vector<std::pair<VkShaderStageFlagBits, std::vector<uint32_t>>> stages;
};
void program_cache_each(const std::function<void(const KeptProgram&)>& fn);   // (fn: on several threads at once, in no order)
void program_cache_borrow(uint64_t key, const std::function<void(Program&)>& fn);   // (made for fn, then let go; still kept)
/// (hookshot) A compute program read from the cache at its key's first pick has no code yet (vk_progcache.cpp
/// program_cache_load): its SPIR-V is read and its module made here, before the first use of the module (vk_draw.cpp dispatch).
/// True: it is whole (or was). False: its code did not read or made no module; it is marked failed
bool program_cache_complete(Program& p);
/// (hookshot) Every kept graphics and mesh program's key, in the file's order, no record read; *loaded: the variants in memory already
std::vector<uint64_t> program_cache_keys(std::vector<Program*>* loaded);
const Program* program_find(uint64_t key, uint64_t variant);
/// (hookshot) The pipeline state list's variants, the same on every GPU: the answers with their stages numbered without the mesh
/// fallback's generated vertex stage, and their hash (a graphics or native mesh program's: its own); a loaded variant by that hash
std::vector<Program::Answer> answers_portable(const Program& p);
std::vector<Program::Answer> answers_here(const Program& p, std::vector<Program::Answer> portable);   // (the inverse)
uint64_t answers_hash_of(const std::vector<Program::Answer>& answers);   // (a variant's: Program::variant)
uint64_t portable_variant(const Program& p);
const Program* program_find_portable(uint64_t key, uint64_t variant);
/// (hookshot) The pipeline state list (vk_draw.cpp): a program by its stages' code keys (graphics: vertex, fragment; mesh: mesh,
/// fragment, task) and the vertex inputs and colour output types it was drawn with, from the seed (seed_open first): translated
/// and kept unless kept already (null then, or when it does not translate). *key_out: its key, 0 when a stage is not in the seed
std::unique_ptr<Program> seed_program_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8],
                                         uint64_t* key_out);
/// (hookshot) The same for a program variant made by other textures' answers: translated with the answers given in the order asked
/// (Program::questions) and taking those answers (Program::answers, numbered as answers_portable has them); kept in memory for the precompile, and in the program cache.
/// Null when a stage is not in the seed or it does not translate
const Program* seed_variant_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8],
                               const std::vector<uint8_t>& questions, const std::vector<Program::Answer>& answers);
/// (hookshot) The variant nothing bound makes (each texture as its instruction declares it), kept the same way: what a state list
/// record with variant 0 and no answers or questions stands for (one converted from records that hold no texture answers)
const Program* seed_plain_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8]);
/// (hookshot) That variant if it is loaded and ready: every slot answering the type its instruction declares. Its own variant is
/// the hash of those answers, not 0, so program_find_portable(key, 0) does not find it (state_list_prepare looks here first)
const Program* program_find_plain(uint64_t key);
/// The NVN stages of the bound program, for logs (stage field of each control section)
void log_program_stages(const NvnState& s, const char* why);
Shader::AttributeType attribute_type_of(uint32_t vattr);
Shader::TextureType handle_texture_type(uint32_t handle);
void shader_stats(int64_t* translated, int64_t* failed);
/// The networks this card left out on purpose (not among the failed)
int64_t shader_left_out();
double shader_translate_ms();
void program_released(const nvn_prog* p);
/// (hookshot) A guest program as the game makes it (nvnProgramSetShaders), before any draw (vk_programs.cpp): its stages' code keys
/// as Program::codes has them (vertex or mesh, fragment, task, the header-only stage; a compute program's stage in codes[0]),
/// read from the game's memory now and hashed, nothing lowered. ok: it has a stage a draw or dispatch can use
struct CreatedProgram {
    uint64_t codes[4]{};
    bool ok = false, compute = false, mesh = false;
    int stages = 0;
};
CreatedProgram program_created_read(const nvn_prog& prog);   // (the game's thread; kept by the program's serial)
/// The key program_pick will make for that program drawn with these vertex input formats and colour output types (a compute
/// program's: its stage's key); 0: no such serial was read
uint64_t program_created_key(uint64_t serial, const uint32_t vattr[16], const uint8_t color_types[8]);
void program_created_forget(uint64_t serial);
int64_t shader_translated();   // (programs translated so far, by any thread)

} // namespace mkwvk
