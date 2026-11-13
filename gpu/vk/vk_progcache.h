// (hookshot) The program cache's state and reads, shared by vk_shader.cpp (the translator, which saves what it makes and reads
// back what it has made) and vk_progcache.cpp (the cache itself, outside the translator stamp: see there)
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vk_shader.h"

namespace mkwvk {

struct DiskRecord { int64_t at; size_t size; uint32_t kind; };
extern std::mutex disk_mu;
extern FILE* disk_in;
extern std::string disk_path;
extern std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_records;
extern std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_taken;
extern bool disk_listing;
extern bool disk_on;
extern std::atomic<int64_t> st_cache_hits;

// (a record is read within its own bytes: one made wrong, a count that ran past them, read on into the heap until it met an
// unmapped page, a crash in program_cache_load. Past the end, bad is set, the gets give zeros, and the record is left out)
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;
    bool has(uint64_t n) {
        if (!bad && n > (uint64_t)(end - p)) bad = true;
        return !bad;
    }
    template <class T> T get() {
        alignas(T) uint8_t b[sizeof(T)]{};
        if (has(sizeof(T))) {
            std::memcpy(b, p, sizeof(T));
            p += sizeof(T);
        }
        return *(T*)b;
    }
    /// a count of elements of size elem, no more than the bytes left could hold (0 when bad)
    uint64_t count(size_t elem) {
        const uint64_t n = get<uint64_t>();
        if (!bad && n > (uint64_t)(end - p) / (elem ? elem : 1)) bad = true;
        return bad ? 0 : n;
    }
    template <class V> void list(V& v) {
        const uint64_t n = count(sizeof(typename V::value_type));
        v.clear();
        v.reserve((size_t)n);
        for (uint64_t i = 0; i < n && !bad; i++) v.push_back(get<typename V::value_type>());
    }
};

void disk_init();
void get_head(Reader& r, Program& p);
void program_cache_save(const Program& p, const std::vector<std::vector<uint32_t>>& spirv);
/// The payload of a record: the stored bytes the file has at d.at (d.size of them), unpacked when the kind says it is
/// compressed (RX_VK_PROGRAM_CACHE_ZSTD). On true, payload/payload_size are the program's bytes (plain, when it was
/// compressed); false when the record is not whole or the frame does not unpack.
bool program_cache_payload(const uint8_t* stored, size_t stored_size, uint32_t kind, std::vector<uint8_t>& plain,
                           const uint8_t** payload, size_t* payload_size);
/// (shader_mu held) the kept variants of key go to its list, ready, once (keep: borrowed, still on disk for play)
void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool keep = false);
/// The kept variants of key read from the file (in: a handle on it, used under mu when given) and made ready, to variants
void program_cache_read(FILE* in, std::mutex* mu, uint64_t key, const std::vector<DiskRecord>& recs,
                        std::vector<std::unique_ptr<Program>>& variants);
// ---- (hookshot) The load when the game makes a program (vk_programs.cpp): the records by position, one at a time ----
/// Whether a program of that key is kept and not read in yet
bool program_cache_has(uint64_t key);
/// Every such key now (the index's own copy: a create call on the game's thread then takes no lock a disk read holds)
std::unordered_set<uint64_t> program_cache_kept_keys();
/// The keys of the kept compute programs still in the file's list (the first start's warm: vk_draw.cpp precompile_warm)
std::vector<uint64_t> program_cache_compute_keys();
/// The records of key not read in yet, as the index has them (none: no such key, or all read in)
std::vector<DiskRecord> program_cache_records(uint64_t key);
/// A record's variant (the hash of its answers) from its head alone, read by position. False: the head does not read
bool program_cache_variant(FILE* in, uint64_t key, const DiskRecord& d, uint64_t* variant);
/// Those of want still not read in become read in (as program_cache_load takes them): the ones taken, for program_cache_read
std::vector<DiskRecord> program_cache_take(uint64_t key, const std::vector<DiskRecord>& want);
/// A kept compute record's program without its code (its head and descriptors, by position around the SPIR-V): what a pick
/// needs. program_cache_complete reads the code and makes the module. Null: not a record to read this way
std::unique_ptr<Program> program_cache_read_light(FILE* in, std::mutex* mu, uint64_t key, const DiskRecord& d);
bool program_cache_complete_unshared(Program& p, FILE* in);   // (program_cache_complete for a program no other thread has yet)
/// The cache's file, for a thread's own handle on it (empty: no cache)
std::string program_cache_path();

// (vk_shader.cpp's, for the cache)
uint64_t translator_stamp();   // (the translator and what it is told: HOOKSHOT_SHADER_STAMP, the GPU, the environment)
VkShaderModule make_module(const std::vector<uint32_t>& spirv);
uint64_t answers_hash(const Program& p);
bool answers_fit(const Program& p, const std::vector<Program::Answer>& answers);
extern std::mutex shader_mu;
extern std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> programs;   // by the stages' code keys

} // namespace mkwvk
