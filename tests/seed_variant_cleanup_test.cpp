// Execute the complete production function and actual owners. Only translation
// (which needs the recompiler) and Vulkan calls are replaced with synthetic data.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkShaderModule = uintptr_t;
using VkDescriptorSetLayout = uintptr_t;
using VkPipelineLayout = uintptr_t;
using VkPipeline = uintptr_t;
using VkDescriptorType = uint32_t;
using VkShaderStageFlagBits = uint32_t;
using VkShaderStageFlags = uint32_t;
using VkPrimitiveTopology = uint32_t;
constexpr uintptr_t VK_NULL_HANDLE = 0;
constexpr uint32_t VK_SHADER_STAGE_VERTEX_BIT = 1;
constexpr uint32_t VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3;
namespace Shader { struct Info {}; }
#include "seed_types.inc"

struct {
    uintptr_t device = 123;
    bool mesh_shader = true, mesh_fallback = false;
    bool uses_mesh_fallback(bool) const { return false; }
} dev;
std::map<VkShaderModule, unsigned> destroyed;
unsigned created = 0, releases = 0;
VkShaderModule make_test_module() {
    const VkShaderModule module = ++created;
    CHECK(destroyed.emplace(module, 0).second);
    return module;
}
void vkDestroyShaderModule(uintptr_t device, VkShaderModule module, const void* allocator) {
    CHECK(device == dev.device && allocator == nullptr && module != VK_NULL_HANDLE);
    auto it = destroyed.find(module);
    CHECK(it != destroyed.end() && it->second == 0);
    ++it->second;
    ++releases;
}
struct NvnState { uint32_t nvattr = 0, vattr[16]{}; };
struct Code { bool ok = true; };
struct SeedStage { uint32_t stage = 0; };
struct { std::unordered_map<uint64_t, uint32_t> index; std::vector<SeedStage> stages; } seed;
Code stage_of_seed(const SeedStage&) { return {}; }
uint64_t mesh_key(const Code*, const Code&, const Code*, const Code*, const uint8_t*) { return 71; }
uint64_t graphics_key(const Code&, const Code*, const uint32_t*, const uint8_t*, bool) { return 31; }
bool scene_tonemap_for(const Code*) { return false; }
thread_local const std::vector<uint8_t>* questions_in = nullptr;
thread_local size_t questions_at = 0;
thread_local const std::vector<Program::Answer>* answers_in = nullptr;
std::mutex shader_mu;
std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> programs;
bool translation_fails = false, null_only = false;
unsigned graphics_calls = 0, mesh_calls = 0;

void synthetic_translation(Program& p, const NvnState& s, unsigned modules) {
    CHECK(s.nvattr == 16 && s.vattr[0] == 12);
    CHECK(questions_in && questions_in->size() == 1 && (*questions_in)[0] == 4);
    CHECK(answers_in && answers_in->size() == 1 && (*answers_in)[0].type == 2);
    CHECK(questions_at == 0);
    for (unsigned i = 0; i < modules; ++i) {
        Stage stage;
        if (!null_only) stage.module = make_test_module();
        p.stages.push_back(std::move(stage));
    }
    // Failed translation can also leave a stage with no module.
    p.stages.emplace_back();
    p.variant = 99;
    p.failed = translation_fails;
    p.ready = true;
}
void translate_into(Program& p, const NvnState& s, const Code*, const Code*, const Code*, const uint8_t*) {
    ++graphics_calls;
    synthetic_translation(p, s, 1);
}
void translate_mesh_into(Program& p, const NvnState& s, const Code*, const Code&, const Code*, const Code*, const uint8_t*) {
    ++mesh_calls;
    synthetic_translation(p, s, 2);
}
#include "seed_function.inc"

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string name = argv[1];
    const bool mesh = name == "mesh_failure";
    translation_fails = name == "graphics_failure" || mesh || name == "null_failure";
    null_only = name == "null_failure";
    seed.index.emplace(42, 0);
    seed.stages.push_back({mesh ? 7u : 0u});
    seed.index.emplace(43, 1);
    seed.stages.push_back({1});
    seed.index.emplace(44, 2);
    seed.stages.push_back({6});
    const uint64_t code[3] = {42, 43, mesh ? 44u : 0u};
    uint32_t attributes[16] = {12};
    const uint8_t color_types[8]{};
    const std::vector<uint8_t> questions{4};
    const std::vector<Program::Answer> answers{{0, 0, 2, false}};
    const auto* result = seed_variant_of(code, mesh, attributes, color_types, questions, answers);
    CHECK(questions_in == nullptr && answers_in == nullptr);
    CHECK(graphics_calls == (mesh ? 0u : 1u) && mesh_calls == (mesh ? 1u : 0u));
    if (translation_fails) {
        CHECK(result == nullptr && programs.empty());
        CHECK(created == (null_only ? 0u : mesh ? 2u : 1u));
        std::printf("failed seed variant: created=%u destroyed=%u cached=%zu\n", created, releases, programs.size());
        CHECK(releases == created);
    } else {
        CHECK(result && programs.size() == 1 && programs.at(31).size() == 1);
        CHECK(result == programs.at(31).front().get());
        CHECK(result->stages[0].module == 1 && created == 1 && releases == 0);
        CHECK(result->codes[0] == 42 && result->codes[1] == 43);
        if (name == "duplicate") {
            const auto* duplicate = seed_variant_of(code, false, attributes, color_types, questions, answers);
            CHECK(duplicate == result && programs.at(31).size() == 1);
            CHECK(created == 2 && releases == 1);
            CHECK(destroyed.at(1) == 0 && destroyed.at(2) == 1);
            CHECK(questions_in == nullptr && answers_in == nullptr);
        } else CHECK(name == "success");
    }
    // The process owns the synthetic retained handles until exit; clearing the
    // cache must not supply the cleanup under test or hide premature destruction.
    programs.clear();
    std::puts("seed variant module ownership passed");
}
