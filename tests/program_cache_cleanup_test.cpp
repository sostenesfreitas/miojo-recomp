// Instrument Vulkan, not the loader: the runner compiles the actual Stage/Program
// declarations, serializer, Reader, make_module and program_cache_load functions.
// Info is a minimal stand-in with the cache's separately serialized containers;
// this test covers module ownership, not Shader::Info layout or SPIR-V validity.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
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

#include "../gpu/vk/fallback_size_policy.h"   // (program_cache_save and _read note each stage's phis)
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>   // (posix_fadvise: program_cache_load)
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

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
constexpr uint32_t VK_SHADER_STAGE_VERTEX_BIT = 1, VK_SHADER_STAGE_FRAGMENT_BIT = 16;
constexpr uint32_t VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3;
constexpr size_t kIdxEntry = 20;
constexpr int VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 15;
struct VkShaderModuleCreateInfo {
    int sType;
    size_t codeSize = 0;
    const uint32_t* pCode = nullptr;
};
namespace Shader {
namespace IR { enum class Attribute : uint64_t {}; }
struct Descriptor {   // (a descriptor's array size, which get_info checks)
    uint32_t count;
    Descriptor(uint32_t c = 1) : count(c) {}
};
struct Info {
    uint32_t marker = 0;
    std::vector<Descriptor> constant_buffer_descriptors, storage_buffers_descriptors,
        texture_buffer_descriptors, image_buffer_descriptors, texture_descriptors, image_descriptors;
    std::map<IR::Attribute, IR::Attribute> legacy_stores_mapping;
    std::vector<std::vector<uint32_t>> handle_programs;
};
}
#include "cache_types.inc"

struct { uintptr_t device = 123; } dev;
std::map<VkShaderModule, unsigned> destroyed;
unsigned attempts = 0, created = 0, releases = 0, fail_at = 0;
int vkCreateShaderModule(uintptr_t device, const VkShaderModuleCreateInfo* info, const void* allocator,
                         VkShaderModule* module) {
    CHECK(device == dev.device && allocator == nullptr);
    CHECK(info->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && info->codeSize && info->pCode);
    ++attempts;
    *module = VK_NULL_HANDLE;
    if (attempts == fail_at) return -1;
    *module = ++created;
    CHECK(destroyed.emplace(*module, 0).second);
    return 0;
}
void vkDestroyShaderModule(uintptr_t device, VkShaderModule module, const void* allocator) {
    CHECK(device == dev.device && allocator == nullptr && module != VK_NULL_HANDLE);
    auto it = destroyed.find(module);
    CHECK(it != destroyed.end() && it->second == 0);
    ++it->second;
    ++releases;
}
void disk_init() {}
void logf(const char*, ...) {}
bool env_on(const char*) { return false; }   // (RX_VK_LOAD_ALL_VARIANTS: program_cache_load)
bool disk_on = true, disk_listing = true;
bool disk_zstd = false;   // (vk_progcache.cpp's RX_VK_PROGRAM_CACHE_ZSTD; the tests below use the uncompressed records)
constexpr size_t kZstdHead = 20;
constexpr uint32_t kCompressedKind = 16;
// (the zstd calls the cache functions may make: a stub that copies, so the compressed record path and its 8-byte
// unpacked size are exercised without linking zstd. It keeps the last plain payload for the matching decompress.)
static std::vector<uint8_t> zstd_plain;
extern "C" {
size_t ZSTD_compressBound(size_t n) { return n + (n >> 8) + 64; }
size_t ZSTD_compress(void* dst, size_t cap, const void* src, size_t n, int) {
    zstd_plain.assign((const uint8_t*)src, (const uint8_t*)src + n);
    if (!cap) return (size_t)-1;
    ((uint8_t*)dst)[0] = 0;   // (a one-byte "frame"; ZSTD_decompress hands the payload back)
    return 1;
}
size_t ZSTD_decompress(void* dst, size_t cap, const void*, size_t) {
    if (zstd_plain.size() > cap) return (size_t)-1;
    if (!zstd_plain.empty()) std::memcpy(dst, zstd_plain.data(), zstd_plain.size());
    return zstd_plain.size();
}
unsigned ZSTD_isError(size_t n) { return n == (size_t)-1; }
}
FILE* disk_in = nullptr;
FILE* disk_out = nullptr;
FILE* disk_idx = nullptr;
std::mutex disk_mu;
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_records;
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_taken;   // (vk_shader.cpp: the records read in, for the prediction)
std::atomic<int64_t> st_cache_hits{0};
#define PROF_EXACT()   // (vk_prof.h's exact-timing scope, since #989: nothing to time here)
// (the census's counters, gpu/vk/vk_programs.h: counted, not checked here)
struct { int64_t loads = 0, load_ns = 0; bool background = false; } use_counters;
struct { int64_t play_loads = 0, play_load_ns = 0; } census;
std::atomic<bool> programs_in_play{false};
void programs_load_wait(uint64_t) {}   // (the load at a program's create call, gpu/vk/vk_programs.cpp: none here)
void module_id_learned(uint64_t, uint64_t, VkShaderModule) {}   // (shader module identifiers, vk_programs.cpp: none here)
#include "cache_functions.inc"

void save(uint64_t key, bool empty_second = false, int bad_answer = 0) {
    Program program;
    program.key = key;
    program.stages.emplace_back();
    program.stages.emplace_back();
    program.stages[0].nvn_stage = 0;
    program.stages[0].vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    program.stages[0].c1.assign(300, 0x5a);   // (past 256 bytes: the zstd path compresses the record)
    program.stages[1].nvn_stage = 1;
    program.stages[1].vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    program.stages[1].info.handle_programs = {{1, 2}};
    program.stages[1].info.texture_descriptors = {0};
    program.answers.push_back({1, 0, 1, false});   // (a fitting answer: stage 1's one texture)
    // (the file is passed around: an answer naming a stage or slot the program does not have is refused)
    if (bad_answer == 1) program.answers.push_back({7, 0, 1, false});
    if (bad_answer == 2) program.answers.push_back({1, 3, 1, false});
    std::vector<std::vector<uint32_t>> code{{0x07230203}, {0x07230203}};
    if (empty_second) code[1].clear();
    program_cache_save(program, code);
    CHECK(disk_records.at(key).size() > 0);
}
void release_accepted(std::vector<std::unique_ptr<Program>>& programs) {
    for (auto& p : programs) for (auto& stage : p->stages) {
        CHECK(stage.module && destroyed.at(stage.module) == 0);
        vkDestroyShaderModule(dev.device, stage.module, nullptr);
        stage.module = VK_NULL_HANDLE;
    }
    programs.clear();
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    disk_in = disk_out = std::tmpfile();
    CHECK(disk_in);
    const std::string scenario = argv[1];
    disk_zstd = scenario == "compressed";
    save(7, scenario == "empty", scenario == "answer_stage" ? 1 : scenario == "answer_desc" ? 2 : 0);
    if (scenario == "first") fail_at = 1;
    if (scenario == "second" || scenario == "mixed") fail_at = 2;
    if (scenario == "truncated") --disk_records.at(7).back().size;
    if (scenario == "mixed") {
        save(7); // A valid variant after the failing variant, under the same key.
        save(9); // An independent key must also remain usable.
    }
    std::vector<std::unique_ptr<Program>> programs;
    program_cache_load(7, programs);
    unsigned accepted = scenario == "valid" || scenario == "mixed" || scenario == "compressed" ? 1 : 0;
    CHECK(programs.size() == accepted && st_cache_hits == accepted);
    std::printf("%s: created=%u destroyed=%u accepted_modules=%u\n", scenario.c_str(), created, releases, accepted * 2);
    CHECK(created - releases == accepted * 2);
    if (scenario == "first") CHECK(created == 0 && releases == 0);
    if (scenario == "second" || scenario == "empty") CHECK(created == 1 && releases == 1);
    if (scenario == "truncated") CHECK(created == 0 && releases == 0);
    if (scenario == "answer_stage" || scenario == "answer_desc") CHECK(created == 2 && releases == 2);
    if (scenario == "valid") CHECK(created == 2 && releases == 0);
    if (scenario == "compressed") CHECK(created == 2 && releases == 0);
    if (scenario == "mixed") {
        CHECK(created == 3 && releases == 1);
        program_cache_load(9, programs);
        CHECK(programs.size() == 2 && st_cache_hits == 2 && created == 5 && releases == 1);
        program_cache_load(7, programs); // Consumed records cannot be cleaned up twice.
        CHECK(programs.size() == 2 && created == 5 && releases == 1);
    }
    for (const auto& p : programs) CHECK(p->ready.load() && p->stages.size() == 2);
    release_accepted(programs);
    CHECK(created == releases);
    for (const auto& [module, count] : destroyed) { (void)module; CHECK(count == 1); }
    CHECK(std::fclose(disk_in) == 0);
    std::puts("PASS: no live, invalid or double-destroyed module");
}
