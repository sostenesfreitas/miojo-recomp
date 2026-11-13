// The production cache loader (program_cache_save, _read, _load, program_cache_read_light, program_cache_complete), cut out by the
// runner, with a made-up Vulkan and a counted fread: a compute key's variants are read without their code, the pick that
// follows takes the one it took when each was read whole, and only that one's code is read and made a module of.
// Info is a minimal stand-in with the cache's separately serialized containers, as program_cache_cleanup_test.cpp's.
#include <array>
#include <atomic>
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

#include "../gpu/vk/fallback_size_policy.h"
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
constexpr uint32_t VK_SHADER_STAGE_VERTEX_BIT = 1, VK_SHADER_STAGE_FRAGMENT_BIT = 16, VK_SHADER_STAGE_COMPUTE_BIT = 32;
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
struct Descriptor {
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
// the made-up driver: a module's handle says which code it was made of (the SPIR-V's second word) and how long it was
unsigned created = 0, destroyed = 0;
std::map<VkShaderModule, std::pair<uint32_t, size_t>> made_of;
int vkCreateShaderModule(uintptr_t device, const VkShaderModuleCreateInfo* info, const void* allocator, VkShaderModule* module) {
    CHECK(device == dev.device && allocator == nullptr);
    CHECK(info->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && info->codeSize >= 8 && info->pCode);
    *module = 1000 + ++created;
    made_of[*module] = {info->pCode[1], info->codeSize / 4};
    return 0;
}
void vkDestroyShaderModule(uintptr_t, VkShaderModule module, const void*) { CHECK(made_of.count(module)); ++destroyed; }
void disk_init() {}
void logf(const char*, ...) {}
bool whole_variants = false;   // (RX_VK_LOAD_ALL_VARIANTS=1: the way before)
bool env_on(const char* name) { return !std::strcmp(name, "RX_VK_LOAD_ALL_VARIANTS") && whole_variants; }
bool disk_on = true, disk_listing = true;
FILE* disk_in = nullptr;
FILE* disk_out = nullptr;
FILE* disk_idx = nullptr;
std::mutex disk_mu;
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_records;
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_taken;
std::atomic<int64_t> st_cache_hits{0};
#define PROF_EXACT()
// (vk_progcache.cpp's compressed cache, RX_VK_PROGRAM_CACHE_ZSTD: off here but for the last case below, which only asks that a
// compressed record is not read by its parts' places. The zstd calls the cut functions name are never made with it off)
bool disk_zstd = false;
constexpr size_t kZstdHead = 20;
constexpr uint32_t kCompressedKind = 16;
extern "C" {
size_t ZSTD_compressBound(size_t n) { return n + 64; }
size_t ZSTD_compress(void*, size_t, const void*, size_t, int) { return (size_t)-1; }
size_t ZSTD_decompress(void*, size_t, const void*, size_t) { return (size_t)-1; }
unsigned ZSTD_isError(size_t n) { return n == (size_t)-1; }
}
// (the census's counters and the load at a program's create call, gpu/vk/vk_programs.h: counted, none here)
struct { int64_t loads = 0, load_ns = 0; bool background = false; } use_counters;
struct { int64_t play_loads = 0, play_load_ns = 0; } census;
std::atomic<bool> programs_in_play{false};
void programs_load_wait(uint64_t) {}
void module_id_learned(uint64_t, uint64_t, VkShaderModule) {}   // (shader module identifiers, vk_programs.cpp: none here)
// (every byte the loader reads from the cache file)
size_t bytes_read = 0;
static size_t counted_fread(void* to, size_t size, size_t n, FILE* f) {
    const size_t got = std::fread(to, size, n, f);
    bytes_read += got * size;
    return got;
}
#define fread counted_fread
#include "cache_functions.inc"
#undef fread

constexpr size_t kWords = 30000;   // (120 KB of SPIR-V a variant, as the game's compute programs have)
// a compute variant: one stage, one texture whose answer is `type`, its code marked with `mark`
static void save(uint64_t key, uint8_t type, uint32_t mark, size_t answers = 1, bool two_stages = false) {
    Program p;
    p.key = key;
    p.compute = true;
    p.workgroup = {8, 8, mark};
    p.stages.emplace_back();
    p.stages[0].nvn_stage = 5;
    p.stages[0].vk_stage = VK_SHADER_STAGE_COMPUTE_BIT;
    p.stages[0].info.marker = mark;
    p.stages[0].info.texture_descriptors = {1, 1};
    p.stages[0].info.image_descriptors = {1};
    p.stages[0].info.handle_programs = {{mark, 2, 3}};
    p.stages[0].c1 = {(uint8_t)mark, (uint8_t)(mark + 1)};
    for (size_t i = 0; i < answers; i++) p.answers.push_back({0, (uint16_t)(i & 1), type, false});
    std::vector<std::vector<uint32_t>> code{std::vector<uint32_t>(kWords, 0x11111111u)};
    code[0][0] = 0x07230203, code[0][1] = mark;
    if (two_stages) {
        p.stages.emplace_back();
        p.stages[1].nvn_stage = 1, p.stages[1].vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        code.push_back({0x07230203, mark + 100});
    }
    program_cache_save(p, code);
}
// (what vk_shader.cpp's still_valid reads of a variant: its answers and its stage's descriptors; here the texture bound has
// the type `bound`)
static bool fits(const Program& p, uint8_t bound) {
    for (const Program::Answer& a : p.answers) {
        CHECK(a.stage < p.stages.size() && a.desc < p.stages[a.stage].info.texture_descriptors.size());
        if (a.type != bound) return false;
    }
    return true;
}
// (vk_shader.cpp program_pick's loop)
static Program* pick(std::vector<std::unique_ptr<Program>>& variants, uint8_t bound) {
    for (auto& p : variants) {
        if (!p->ready.load()) return p.get();
        if (p->failed) return nullptr;
        if (fits(*p, bound)) return p.get();
    }
    return nullptr;
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    whole_variants = !std::strcmp(argv[1], "whole");
    disk_in = disk_out = std::tmpfile();
    CHECK(disk_in);
    // key 7: twelve variants. Two fit a texture of type 4 (the first of them in the file is the one taken); one has a head past
    // what is read of it at first (700 answers) and one is cut short in the file
    const uint8_t types[12] = {1, 2, 3, 4, 5, 4, 6, 7, 8, 9, 10, 11};
    for (uint32_t i = 0; i < 12; i++) save(7, types[i], 100 + i, i == 8 ? 700 : 1);
    --disk_records.at(7)[10].size;   // (the variant of type 10: not whole)
    const size_t file_bytes = (size_t)_ftelli64(disk_out);
    std::vector<std::unique_ptr<Program>> variants;
    program_cache_load(7, variants);
    CHECK(variants.size() == 11 && st_cache_hits == 11 && !disk_records.count(7) && disk_taken.at(7).size() == 12);
    const size_t load_bytes = bytes_read;
    const unsigned load_modules = created;
    // the same programs either way, but for their modules: the order, the head, the stage's descriptors
    for (size_t i = 0; i < variants.size(); i++) {
        const Program& p = *variants[i];
        const uint32_t mark = p.workgroup[2];
        CHECK(p.key == 7 && p.compute && p.ready.load() && !p.failed && p.stages.size() == 1);
        CHECK(mark == 100 + i + (i >= 10 ? 1 : 0) && p.answers.size() == (mark == 108 ? 700u : 1u) && p.answers[0].type == types[mark - 100]);
        CHECK(p.variant == answers_hash(p));
        const Stage& st = p.stages[0];
        CHECK(st.nvn_stage == 5 && st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT && st.info.marker == mark);
        CHECK(st.info.texture_descriptors.size() == 2 && st.info.image_descriptors.size() == 1);
        CHECK(st.info.handle_programs.size() == 1 && st.info.handle_programs[0] == (std::vector<uint32_t>{mark, 2, 3}));
        CHECK(st.c1 == (std::vector<uint8_t>{(uint8_t)mark, (uint8_t)(mark + 1)}));
    }
    if (whole_variants) {
        CHECK(load_modules == 11 && load_bytes > 11 * kWords * 4);
    } else {
        // no code read: the long-headed variant alone is read whole (and made), the others by their head and descriptors
        // (and the record cut short is read once more, whole, to be refused as before)
        CHECK(load_modules == 1 && variants[8]->stages[0].module && load_bytes < 2 * kWords * 4 + 12 * 8192);
        for (size_t i = 0; i < variants.size(); i++) CHECK(i == 8 || !variants[i]->stages[0].module);
    }
    // the picks: for each texture type the variant the loop takes, and that one made whole. Printed for the runner, which
    // compares the two ways
    for (const int bound : {4, 1, 11, 9, 4, 10, 12, 3, 1}) {
        Program* p = pick(variants, (uint8_t)bound);
        if (!p) {
            std::printf("bound %d: none\n", bound);
            continue;
        }
        const unsigned before = created;
        const size_t read_before = bytes_read;
        CHECK(program_cache_complete(*p) && !p->failed);
        const VkShaderModule m = p->stages[0].module;
        CHECK(m && made_of.at(m).first == p->workgroup[2] && made_of.at(m).second == kWords);   // (its own code, all of it)
        CHECK(created - before <= 1 && bytes_read - read_before <= kWords * 4);
        CHECK(program_cache_complete(*p) && p->stages[0].module == m);   // (once)
        std::printf("bound %d: variant %zu mark %u hash %016llx\n", bound, (size_t)p->workgroup[2] - 100,
                    p->workgroup[2], (unsigned long long)p->variant);
    }
    if (!whole_variants) {
        // six different variants were taken (types 4, 1, 11, 9, 3, and 4 and 1 again): five modules made after the load, one each
        CHECK(created == load_modules + 5);
        CHECK(bytes_read - load_bytes == 5 * kWords * 4);
    }
    // a key that is not a compute program's, and a compute key borrowed whole (the prepare's): as before, either way
    {
        Program g;
        g.key = 9;
        g.stages.emplace_back();
        g.stages[0].nvn_stage = 0, g.stages[0].vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
        program_cache_save(g, {{0x07230203, 7}});
        save(11, 1, 300), save(11, 2, 301);
        std::vector<std::unique_ptr<Program>> graphics, borrowed;
        const unsigned before = created;
        program_cache_load(9, graphics);
        CHECK(graphics.size() == 1 && graphics[0]->stages[0].module && created == before + 1);
        program_cache_load(11, borrowed, true);
        CHECK(borrowed.size() == 2 && borrowed[0]->stages[0].module && borrowed[1]->stages[0].module && created == before + 3);
        CHECK(disk_records.at(11).size() == 2);
        CHECK(program_cache_complete(*graphics[0]) && program_cache_complete(*borrowed[1]) && created == before + 3);
    }
    // a compute key with a record of two stages (not one this build writes): that record whole, the other without its code
    {
        save(13, 1, 400, 1, true), save(13, 2, 401);
        std::vector<std::unique_ptr<Program>> v;
        const unsigned before = created;
        program_cache_load(13, v);
        CHECK(v.size() == 2 && v[0]->stages.size() == 2 && v[0]->stages[0].module && v[0]->stages[1].module);
        CHECK(created == before + (whole_variants ? 3u : 2u) && (whole_variants || !v[1]->stages[0].module));
        CHECK(program_cache_complete(*v[1]) && v[1]->stages[0].module && made_of.at(v[1]->stages[0].module).first == 401);
    }
    // code that no longer reads when the variant is taken (the file cut after the load): failed, no module, said once
    if (!whole_variants) {
        save(15, 1, 500);
        std::vector<std::unique_ptr<Program>> v;
        program_cache_load(15, v);
        CHECK(v.size() == 1 && !v[0]->stages[0].module);
        std::FILE* cut = std::tmpfile();
        CHECK(cut);
        std::FILE* was = disk_in;
        disk_in = cut;   // (an empty file in its place)
        CHECK(!program_cache_complete(*v[0]) && v[0]->failed && !v[0]->stages[0].module);
        CHECK(!program_cache_complete(*v[0]));
        disk_in = was;
        std::fclose(cut);
    }
    {   // the compressed cache: a record is one zstd frame, so it is not read by its parts' places at all (nothing is read
        // of it here), and its caller reads it whole; the same record with the cache as it is by default reads this way
        save(21, 1, 600);
        const DiskRecord rec = disk_records.at(21)[0];
        const size_t before = bytes_read;
        disk_zstd = true;
        CHECK(!program_cache_read_light(disk_in, &disk_mu, 21, rec) && bytes_read == before);
        disk_zstd = false;
        const std::unique_ptr<Program> light = program_cache_read_light(disk_in, &disk_mu, 21, rec);
        CHECK(light && light->workgroup[2] == 600 && light->code_words && !light->stages[0].module && bytes_read > before);
    }
    std::printf("modules %u, bytes read at the load %zu of %zu\n", load_modules, load_bytes, file_bytes);
    return 0;
}
