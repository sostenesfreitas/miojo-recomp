// (hookshot) The program cache: hookshot_programs.bin (and its .idx) beside the game, what translation made, kept between runs; its
// reads for play, the precompile, the prediction and the prepare; and the sizes of the prepare's worker pools.
//
// Not part of the translator stamp (nx2/build.py hashes vk_shader.cpp, vk_bank.cpp, vk_state.h, vk_api.h and the recompiler's
// folders): a change here keeps every player's translated programs. What the cache is good for is program_cache_stamp: the
// translator's stamp (translator_stamp, vk_shader.cpp) and kProgramCacheFormat below. A change to what a record holds or how it is
// laid out (put_head/get_head, put_info/get_info, program_cache_save/program_cache_read, the file and index heads) must raise
// kProgramCacheFormat: a cache of the old format is then not read, and the next start prepares again. That includes the types
// written as they lie in memory: Program::Answer, the questions, Stage::c1 and the head's words (vk_shader.h, which the translator
// stamp does not cover); the static_asserts beside kProgramCacheFormat stop a build whose layout of those changed, so that it is
// raised. (Shader::Info, also written as it lies, is the recompiler's: its folders are in the translator stamp.) The pipeline notes,
// the state lists' notes and hookshot_prepared.bin are stamped with program_cache_stamp too, so they go with it
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#endif
#include <io.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zstd.h"

#include "shader_recompiler/shader_info.h"
#include "hookshot_env.h"
#include "fallback_loaded.h"
#include "fallback_size_policy.h"
#include "vk_paths.h"
#include "vk_internal.h"
#include "vk_progcache.h"
#include "vk_programs.h"

namespace mkwvk {

/// (hookshot) The cache's own format: raise it with any change to what a record holds or how the file or its index is laid out
constexpr uint32_t kProgramCacheFormat = 1;
// (what a record holds as it lies in memory, as format 1 has it: a build that changes one stops here. Raise kProgramCacheFormat, then
// change these to the new layout)
static_assert(sizeof(Program::Answer) == 6 && offsetof(Program::Answer, stage) == 0 && offsetof(Program::Answer, desc) == 2 &&
              offsetof(Program::Answer, type) == 4 && offsetof(Program::Answer, image) == 5 &&
              std::is_trivially_copyable_v<Program::Answer>, "Program::Answer's layout is in the program cache's records");
static_assert(std::is_same_v<decltype(Program::answers), std::vector<Program::Answer>>, "the answers are written whole");
static_assert(std::is_same_v<decltype(Program::questions), std::vector<uint8_t>>, "the questions are written as bytes");
static_assert(std::is_same_v<decltype(Stage::c1), std::vector<uint8_t>>, "Stage::c1 is written as bytes");
static_assert(std::is_same_v<decltype(Stage::nvn_stage), int> && sizeof(Stage::vk_stage) == 4, "a stage's two words");
static_assert(std::is_same_v<decltype(Program::workgroup), std::array<uint32_t, 3>> &&
              std::is_same_v<decltype(Program::shared_memory), uint32_t> &&
              std::is_same_v<decltype(Program::fallback_vertices_per_workgroup), uint32_t> &&
              std::is_same_v<decltype(Program::fallback_record_words), uint32_t> &&
              std::is_same_v<decltype(Program::fallback_payload_words), uint32_t> &&
              std::is_same_v<decltype(Program::fallback_task_threads), uint32_t>, "the record head's words (put_head)");

uint64_t program_cache_stamp() {
    static const uint64_t stamp = [] {
        uint64_t h = translator_stamp();
        const uint32_t format = kProgramCacheFormat;
        for (size_t i = 0; i < sizeof format; i++) h = (h ^ ((const uint8_t*)&format)[i]) * 1099511628211ull;
        return h;
    }();
    return stamp;
}

// ---- (hookshot) The program cache, hookshot_programs.bin beside the executable: what translation made, by the key of the
// stages' code and inputs. A program met for the first time stopped the frame for its translation, 9 s of it each run,
// the long stutters when walking somewhere (replay workers in jobs_wait for up to 150 ms). The pipelines themselves were
// already kept (mkw_vk_pipelines.bin). Written as made; good for this translator (HOOKSHOT_SHADER_STAMP) and these switches only
// (a file made by another starts again). RX_VK_NO_PROGRAM_CACHE=1: off ----
std::mutex disk_mu;
// (read as wanted: with every program of the game in it the file is gigabytes, and it was read into memory whole)
FILE* disk_in = nullptr;
std::string disk_path;   // (hookshot_programs.bin: the precompile opens it once a thread)
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_records;
// (hookshot) the records program_cache_load and program_cache_load_some took out of disk_records as they read them in: still in the
// file, and still kept programs to the prediction (program_cache_each). Without them a first start's prediction saw none of the
// programs the state list's notes name, which its own preparation had just read in: "predicted 0 pipelines" on every new build
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_taken;
FILE* disk_out = nullptr;
// (the records' heads, in the file's order, in hookshot_programs.idx beside it: see disk_init)
FILE* disk_idx = nullptr;
bool disk_listing = false;
bool disk_on = false;
bool disk_zstd = false;   // (hookshot) RX_VK_PROGRAM_CACHE_ZSTD=1: records compressed with zstd, hookshot_programs.zst.bin
std::atomic<int64_t> st_cache_hits{0};
constexpr uint32_t kDiskMagic = 0x31505348;
constexpr uint32_t kDiskMagicZstd = 0x32505348;   // ("HSP2": a 20-byte record head, key, stored size, kind)
constexpr uint32_t kIdxMagic = 0x58495048;   // ("HPIX", then a version and the cache's stamp; 20 bytes a record: key, size, kind)
constexpr uint32_t kIdxVersion = 1;
constexpr size_t kIdxEntry = 20;
// (the index's kind word is the record's first flags word: bits 0-3 are compute/mesh/fallback/fallback_task)
constexpr uint32_t kCompressedKind = 16;   // (a zstd record: 8-byte unpacked size, then the frame)
constexpr size_t kZstdHead = 20;           // (the zstd cache's record head: key, stored size, kind; the old cache's is 16)

void idx_put(std::vector<uint8_t>& o, uint64_t key, uint64_t size, uint32_t kind) {
    uint8_t e[kIdxEntry];
    std::memcpy(e, &key, 8), std::memcpy(e + 8, &size, 8), std::memcpy(e + 16, &kind, 4);
    o.insert(o.end(), e, e + kIdxEntry);
}
/// (disk_mu held, or at the start) the index made again whole, for these records in the file's order, and left open to add to
void idx_write(const std::string& path, uint64_t stamp, const std::vector<std::pair<uint64_t, DiskRecord>>& order) {
    if (disk_idx) fclose(disk_idx), disk_idx = nullptr;
    std::vector<uint8_t> o;
    o.reserve(16 + order.size() * kIdxEntry);
    const uint32_t h[2] = {kIdxMagic, kIdxVersion};
    o.insert(o.end(), (const uint8_t*)h, (const uint8_t*)h + 8);
    o.insert(o.end(), (const uint8_t*)&stamp, (const uint8_t*)&stamp + 8);
    for (const auto& [key, d] : order) idx_put(o, key, d.size, d.kind);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(o.data(), 1, o.size(), f) == o.size() && fflush(f) == 0;
    fclose(f);
    if (ok) disk_idx = fopen(path.c_str(), "ab");
    else remove(path.c_str());   // (a short one would only be read as far as it goes, but the next start makes it again anyway)
}


void disk_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (env_on("RX_VK_NO_PROGRAM_CACHE")) return;
        // (hookshot) RX_VK_PROGRAM_CACHE_ZSTD=1: each record compressed with zstd, in a file of its own so that a build
        // without this switch never reads it. SPIR-V compresses about 7.7x at zstd's level 3.
        disk_zstd = env_on("RX_VK_PROGRAM_CACHE_ZSTD");
        char exe[MAX_PATH];
        const DWORD n = game_module_path(exe, MAX_PATH);
        std::string path(exe, n);
        path = path.substr(0, path.find_last_of("\\/") + 1) + (disk_zstd ? "hookshot_programs.zst.bin" : "hookshot_programs.bin");
        // (one Hookshot at a time on a cache: a second one, started while the first wrote, took the record being written for one
        // cut short and cut the file there, under the first, whose list of records then named bytes written over since. Held
        // until the process ends)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const char ch : path) h = (h ^ (uint8_t)tolower((unsigned char)ch)) * 1099511628211ull;
            char name[64];
            snprintf(name, sizeof name, "Local\\HookshotProgramCache_%016llx", (unsigned long long)h);
#ifdef _WIN32
            const HANDLE held = CreateMutexA(nullptr, FALSE, name);
            const bool taken = !held || GetLastError() == ERROR_ALREADY_EXISTS;
#else   // (linux: a lock on a file beside the cache, let go when the process ends)
            const int fd = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
            const bool taken = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0;
#endif
            if (taken) {
                logf("vk: program cache %s: another Hookshot is using it: this one runs without it", path.c_str());
                return;
            }
        }
        const uint64_t stamp = program_cache_stamp();
        const size_t head_size = disk_zstd ? kZstdHead : 16;   // (the zstd cache's head has the record's kind too)
        const uint32_t magic = disk_zstd ? kDiskMagicZstd : kDiskMagic;
        // (hookshot_programs.idx: the records' heads, 20 bytes each, in the file's order. The records follow one another in the
        // file, so finding them was a read of each one's head, 22000 of them over the 4 GB: seconds at every start, each a read
        // somewhere else on the disk (and on a compressed file system a block of 128 KB unpacked for those 20 bytes). The index
        // is taken when its stamp is the cache's, its sizes fit in the file and the last record's head in the file is the one
        // it names; the records past it (written by a build without the index, or after a run that ended between the two
        // writes) are read as before. Anything else: all of them read as before, and the index written again)
        const std::string idx_path = path.substr(0, path.size() - 4) + ".idx";
        std::vector<std::pair<uint64_t, DiskRecord>> order;   // (every record, in the file's order: for the index)
        bool idx_whole = false;   // (the index named every record, and nothing more)
        int64_t good = 0, whole = 0;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            _fseeki64(f, 0, SEEK_END);
            whole = _ftelli64(f);
            _fseeki64(f, 0, SEEK_SET);
            uint32_t head[2]{};
            uint64_t kept = 0;
            if (whole >= 16 && fread(head, 4, 2, f) == 2 && fread(&kept, 8, 1, f) == 1 && head[0] == magic && kept == stamp) {
                good = 16;
                size_t idx_n = 0;
                bool idx_short = false;   // (it named records the file does not have whole)
                if (FILE* x = fopen(idx_path.c_str(), "rb")) {
                    std::vector<uint8_t> idx;
                    _fseeki64(x, 0, SEEK_END);
                    const int64_t n = _ftelli64(x);
                    _fseeki64(x, 0, SEEK_SET);
                    if (n >= 16) {
                        idx.resize((size_t)n);
                        if (fread(idx.data(), 1, idx.size(), x) != idx.size()) idx.clear();
                    }
                    fclose(x);
                    uint32_t ih[2]{};
                    uint64_t is = 0;
                    if (idx.size() >= 16) std::memcpy(ih, idx.data(), 8), std::memcpy(&is, idx.data() + 8, 8);
                    if (ih[0] == kIdxMagic && ih[1] == kIdxVersion && is == stamp) {
                        const size_t entries = (idx.size() - 16) / kIdxEntry;
                        int64_t at = 16;
                        for (size_t i = 0; i < entries; i++) {
                            const uint8_t* e = idx.data() + 16 + i * kIdxEntry;
                            uint64_t key, size;
                            uint32_t kind;
                            std::memcpy(&key, e, 8), std::memcpy(&size, e + 8, 8), std::memcpy(&kind, e + 16, 4);
                            if (size > (uint64_t)whole || at + (int64_t)head_size + (int64_t)size > whole) {
                                idx_short = true;
                                break;
                            }
                            order.push_back({key, {at + (int64_t)head_size, (size_t)size, kind}});
                            at += (int64_t)head_size + (int64_t)size;
                        }
                        // (the last one's head as the file has it: an index of another file, or of this one before it was cut
                        // or made again, is not taken)
                        bool same = !order.empty();
                        if (same) {
                            const auto& [key, d] = order.back();
                            uint64_t rec[2]{};
                            uint32_t kind = 0;
                            _fseeki64(f, d.at - (int64_t)head_size, SEEK_SET);
                            same = fread(rec, 8, 2, f) == 2 && fread(&kind, 4, 1, f) == 1 && rec[0] == key && rec[1] == d.size &&
                                   kind == d.kind;
                        }
                        if (same) {
                            idx_n = order.size();
                            good = at;
                            idx_whole = !idx_short && idx.size() == 16 + entries * kIdxEntry;
                        } else {
                            order.clear();
                        }
                    }
                }
                size_t scanned = 0;
                while (good + 20 <= whole) {
                    uint64_t rec[2];
                    uint32_t kind = 0;
                    _fseeki64(f, good, SEEK_SET);
                    if (fread(rec, 8, 2, f) != 2 || fread(&kind, 4, 1, f) != 1) break;
                    if (rec[1] > (uint64_t)(whole - (size_t)good - head_size)) break;   // (the run stopped while writing it; unsigned: no size wraps)
                    order.push_back({rec[0], {good + (int64_t)head_size, (size_t)rec[1], kind}});
                    good += (int64_t)head_size + (int64_t)rec[1];
                    scanned++;
                }
                if (scanned) idx_whole = false;
                for (const auto& [key, d] : order) disk_records[key].push_back(d);
                if (idx_n && !idx_whole)
                    logf("vk: program cache index: %zu records from it, %zu more read from the cache", idx_n, scanned);
            }
            fclose(f);
        }
        if (good) {
            if (good != whole) {   // (a record cut short: without it)
                if (FILE* f = fopen(path.c_str(), "r+b")) _chsize_s(_fileno(f), good), fclose(f);
            }
            disk_out = fopen(path.c_str(), "ab");
        } else {
            disk_out = fopen(path.c_str(), "wb");
            if (disk_out) {
                const uint32_t h[2] = {magic, 0};
                fwrite(h, 4, 2, disk_out);
                fwrite(&stamp, 8, 1, disk_out);
                fflush(disk_out);
            }
            order.clear();
        }
        if (disk_out) {
            if (idx_whole) disk_idx = fopen(idx_path.c_str(), "ab");
            else idx_write(idx_path, stamp, order);
        }
        disk_in = disk_out ? fopen(path.c_str(), "rb") : nullptr;
        disk_path = path;
        disk_on = disk_out != nullptr && disk_in != nullptr;
        logf("vk: program cache %s: %zu program keys kept, %lld KB", path.c_str(), disk_records.size(), (long long)(good >> 10));
    });
}

void put_bytes(std::vector<uint8_t>& o, const void* d, size_t n) {
    o.insert(o.end(), (const uint8_t*)d, (const uint8_t*)d + n);
}
template <class T> void put(std::vector<uint8_t>& o, const T& v) { put_bytes(o, &v, sizeof v); }
template <class V> void put_list(std::vector<uint8_t>& o, const V& v) {
    put(o, (uint64_t)v.size());
    for (const auto& e : v) put(o, e);
}

/// (hookshot) A program record's head, what comes before its stages: written and read only here, so that program_cache_read and
/// program_cache_each read the same fields. program_cache_each read only the first ones and took the mesh fallback's five words
/// for the stage count: no kept program reached the prediction (vk_predict.cpp, "predicted 0 pipelines")
void put_head(std::vector<uint8_t>& o, const Program& p) {
    put(o, (uint32_t)((p.compute ? 1 : 0) | (p.mesh ? 2 : 0) | (p.fallback ? 4 : 0) | (p.fallback_task ? 8 : 0)));
    put_list(o, p.answers);
    put_list(o, p.questions);
    put(o, p.workgroup);
    put(o, p.shared_memory);
    put(o, (uint32_t)p.fallback_topology);
    put(o, p.fallback_vertices_per_workgroup);
    put(o, p.fallback_record_words);
    put(o, p.fallback_payload_words);
    put(o, p.fallback_task_threads);
}
void get_head(Reader& r, Program& p) {
    const uint32_t kind = r.get<uint32_t>();
    p.compute = (kind & 1) != 0, p.mesh = (kind & 2) != 0, p.fallback = (kind & 4) != 0, p.fallback_task = (kind & 8) != 0;
    r.list(p.answers);
    r.list(p.questions);
    p.workgroup = r.get<std::array<uint32_t, 3>>();
    p.shared_memory = r.get<uint32_t>();
    p.fallback_topology = (VkPrimitiveTopology)r.get<uint32_t>();
    p.fallback_vertices_per_workgroup = r.get<uint32_t>();
    p.fallback_record_words = r.get<uint32_t>();
    p.fallback_payload_words = r.get<uint32_t>();
    p.fallback_task_threads = r.get<uint32_t>();
}

/// Shader::Info is plain data but for these members (the boost containers are heap vectors here, static_vector too);
/// they go on their own, the rest as it lies
void put_info(std::vector<uint8_t>& o, const Shader::Info& i) {
    put_bytes(o, &i, sizeof i);
    put_list(o, i.constant_buffer_descriptors);
    put_list(o, i.storage_buffers_descriptors);
    put_list(o, i.texture_buffer_descriptors);
    put_list(o, i.image_buffer_descriptors);
    put_list(o, i.texture_descriptors);
    put_list(o, i.image_descriptors);
    put(o, (uint64_t)i.legacy_stores_mapping.size());
    for (const auto& kv : i.legacy_stores_mapping) put(o, kv.first), put(o, kv.second);
    put(o, (uint64_t)i.handle_programs.size());
    for (const auto& h : i.handle_programs) put_list(o, h);
}
/// (hookshot) every descriptor array within kMaxDescriptorCount: the counts come from hookshot_programs.bin, and two of 2^31
/// wrapped the layout's sum to 0 (a push layout with billions of descriptors), and one made the binds loop that often
constexpr uint32_t kMaxDescriptorCount = 4096;
template <class Info> bool descriptor_counts_fit(const Info& i) {
    auto fit = [](const auto& list) {
        for (const auto& d : list)
            if (d.count > kMaxDescriptorCount) return false;
        return true;
    };
    return fit(i.constant_buffer_descriptors) && fit(i.storage_buffers_descriptors) && fit(i.texture_buffer_descriptors) &&
           fit(i.image_buffer_descriptors) && fit(i.texture_descriptors) && fit(i.image_descriptors);
}
void get_info(Reader& r, Shader::Info& i) {
    if (!r.has(sizeof i)) return;   // (left as constructed: the record is bad, and left out)
    alignas(Shader::Info) static thread_local uint8_t fresh[sizeof(Shader::Info)];
    std::memcpy(fresh, (const void*)&i, sizeof i);   // (those members as constructed, here)
    std::memcpy((void*)&i, r.p, sizeof i);
    r.p += sizeof i;
#define KEEP(m) std::memcpy((void*)&i.m, fresh + ((const uint8_t*)&i.m - (const uint8_t*)&i), sizeof i.m)
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
    i.handle_programs.resize((size_t)r.count(sizeof(uint64_t)));   // (each at least its own count)
    for (auto& h : i.handle_programs) r.list(h);
    if (!descriptor_counts_fit(i)) r.bad = true;   // (left out, and translated again)
}

void program_cache_save(const Program& p, const std::vector<std::vector<uint32_t>>& spirv) {
    // (each stage's phis, for fallback_compute_pipelines: the SPIR-V is not kept after this. Before the program is published)
    if (spirv.size() == p.stages.size())
        for (size_t k = 0; k < p.stages.size(); k++) p.stages[k].phis = fallback_size::spirv_phis(spirv[k].data(), spirv[k].size());
    disk_init();
    if (!disk_on || spirv.size() != p.stages.size()) return;
    std::vector<uint8_t> o;
    o.reserve(256 << 10);
    put_head(o, p);
    put(o, (uint64_t)p.stages.size());
    for (size_t k = 0; k < p.stages.size(); k++) {
        const Stage& st = p.stages[k];
        put(o, st.nvn_stage);
        put(o, st.vk_stage);
        put_list(o, spirv[k]);
        put_list(o, st.c1);
        put_info(o, st.info);
    }
    const uint32_t flags = *(const uint32_t*)o.data();
    std::vector<uint8_t> compressed;
    bool compressed_ok = false;
    if (disk_zstd && o.size() >= 256) {
        compressed.resize(ZSTD_compressBound(o.size()));
        const size_t n = ZSTD_compress(compressed.data(), compressed.size(), o.data(), o.size(), 3);
        if (!ZSTD_isError(n) && n + 8 < o.size()) {
            compressed.resize(n);
            compressed_ok = true;
        }
    }
    const uint64_t stored = compressed_ok ? 8 + (uint64_t)compressed.size() : (uint64_t)o.size();
    const uint32_t kind = flags | (compressed_ok ? kCompressedKind : 0);
    const size_t head_size = disk_zstd ? kZstdHead : 16;
    const uint64_t head[2] = {p.key, stored};
    std::lock_guard lk(disk_mu);
    _fseeki64(disk_out, 0, SEEK_END);
    const int64_t at = _ftelli64(disk_out);
    fwrite(head, 8, 2, disk_out);
    if (disk_zstd) fwrite(&kind, 4, 1, disk_out);
    if (compressed_ok) {
        const uint64_t usize = o.size();
        fwrite(&usize, 8, 1, disk_out);
        fwrite(compressed.data(), 1, compressed.size(), disk_out);
    } else {
        fwrite(o.data(), 1, o.size(), disk_out);
    }
    fflush(disk_out);
    if (disk_idx) {   // (after the record: an index ahead of the file would name a record it does not have)
        std::vector<uint8_t> e;
        idx_put(e, p.key, stored, kind);
        fwrite(e.data(), 1, e.size(), disk_idx);
        fflush(disk_idx);
    }
    // (the precompile's: nothing holds them but the file, and the game starts on them at once)
    if (disk_listing) disk_records[p.key].push_back({at + (int64_t)head_size, (size_t)stored, kind});
}

bool program_cache_payload(const uint8_t* stored, size_t stored_size, uint32_t kind, std::vector<uint8_t>& plain,
                           const uint8_t** payload, size_t* payload_size) {
    *payload = stored;
    *payload_size = stored_size;
    if (!(kind & kCompressedKind)) return true;
    if (stored_size < 8) return false;
    uint64_t usize;
    std::memcpy(&usize, stored, 8);
    if (usize < 4 || usize > (1ull << 31)) return false;
    plain.resize((size_t)usize);
    const size_t n = ZSTD_decompress(plain.data(), plain.size(), stored + 8, stored_size - 8);
    if (ZSTD_isError(n) || n != plain.size()) return false;
    *payload = plain.data();
    *payload_size = plain.size();
    return true;
}

/// The kept variants of key read from the file (in: a handle on it, used under mu when given) and made ready, to variants
void program_cache_read(FILE* in, std::mutex* mu, uint64_t key, const std::vector<DiskRecord>& recs,
                        std::vector<std::unique_ptr<Program>>& variants) {
    for (const DiskRecord& d : recs) {
        // (the census: the records each thread reads, and those read in play, vk_programs.h)
        struct Counted {
            const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~Counted() {
                const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
                use_counters.loads++, use_counters.load_ns += ns;
                if (programs_in_play.load(std::memory_order_relaxed) && !use_counters.background) census.play_loads++, census.play_load_ns += ns;
            }
        } counted;
        const size_t head_size = disk_zstd ? kZstdHead : 16;
        std::vector<uint8_t> data(head_size + d.size);   // (with its head: the key, size and kind are checked against the file's)
        {
            std::unique_lock<std::mutex> lk;
            if (mu) lk = std::unique_lock<std::mutex>(*mu);
            _fseeki64(in, d.at - (int64_t)head_size, SEEK_SET);
            if (fread(data.data(), 1, data.size(), in) != data.size()) continue;
        }
        uint64_t head[2];
        std::memcpy(head, data.data(), 16);
        uint32_t kind = 0;
        if (disk_zstd) std::memcpy(&kind, data.data() + 16, 4);
        if (head[0] != key || head[1] != d.size || (disk_zstd && kind != d.kind)) {
            static std::atomic<int> logged{0};
            if (logged++ < 20)
                logf("vk: program cache: no record of program %016llx at %lld as the index said: translated again", (unsigned long long)key,
                     (long long)d.at);
            continue;
        }
        std::vector<uint8_t> plain;
        const uint8_t* payload = nullptr;
        size_t payload_size = 0;
        if (!program_cache_payload(data.data() + head_size, d.size, d.kind, plain, &payload, &payload_size)) {
            static std::atomic<int> logged{0};
            if (logged++ < 20)
                logf("vk: program cache: the compressed record of program %016llx at %lld did not unpack: translated again",
                     (unsigned long long)key, (long long)d.at);
            continue;
        }
        Reader r{payload, payload + payload_size};
        auto p = std::make_unique<Program>();
        p->key = key;
        get_head(r, *p);
        p->variant = answers_hash(*p);
        const uint64_t n = r.count(1);
        bool ok = !r.bad && n <= 8;   // (a program has at most a few stages)
        for (uint64_t k = 0; ok && k < n; k++) {
            p->stages.emplace_back();
            Stage& st = p->stages.back();
            st.nvn_stage = r.get<int>();
            st.vk_stage = r.get<VkShaderStageFlagBits>();
            std::vector<uint32_t> spirv;
            r.list(spirv);
            r.list(st.c1);
            get_info(r, st.info);
            if (r.bad || spirv.empty()) {
                ok = false;
                break;
            }
            st.module = make_module(spirv);
            st.phis = fallback_size::spirv_phis(spirv.data(), spirv.size());
            ok = ok && st.module;
        }
        ok = ok && answers_fit(*p, p->answers);   // (hookshot: an answer past the stages or their slots: not this record)
        if (!ok) {
            static std::atomic<int> logged{0};
            // Program owns the stage containers, but their Vulkan handles are not RAII-managed.
            for (Stage& st : p->stages) {
                if (st.module) {
                    vkDestroyShaderModule(dev.device, st.module, nullptr);
                    st.module = VK_NULL_HANDLE;
                }
            }
            if (r.bad && logged++ < 20)
                logf("vk: program cache: the record of program %016llx at %lld (%zu bytes) is not whole: left out, translated again",
                     (unsigned long long)key, (long long)d.at, d.size);
            continue;
        }
        p->ready.store(true, std::memory_order_release);
        variants.push_back(std::move(p));
        st_cache_hits++;
    }
}

// ---- (hookshot) A compute program's kept variants, read at its first dispatch without their code. A key has up to 64 kept
// variants (the texture answers the prepare tried), and the dispatch runs one: which one is decided from each variant's answers
// and its stage's descriptors (vk_shader.cpp still_valid), not from its SPIR-V. Reading every variant whole and making a module
// of each was half of what a first dispatch cost on the thread replaying (RTX 3090, a tour with no compute program kept ready:
// 148 first dispatches read 865 variants, 111 MB, 541 ms of their 1,048 ms; a 64-variant program 48 to 72 ms). Now each
// variant's head and descriptors are read by position around its SPIR-V, the program is given to the pick as it is, and the
// one the dispatch takes gets its code read and its module made (program_cache_complete). RX_VK_LOAD_ALL_VARIANTS=1: every
// variant whole, as before ----
// (where the code of a program read without it is: kept in the program itself, Program::code_at and code_words, not in a table
// by the program's address, which a program let go and another made at the same address would find a stale entry in)
namespace {
std::mutex light_mu;
}   // namespace
/// A kept record's program without its code. nullptr: not a record to read this way (more than one stage, a head longer than
/// kFront or a tail longer than kTailMost, one that does not read): it is read whole, as before
std::unique_ptr<Program> program_cache_read_light(FILE* in, std::mutex* mu, uint64_t key, const DiskRecord& d) {
    // (the compressed cache, RX_VK_PROGRAM_CACHE_ZSTD: a record is one zstd frame behind a 20-byte head, and nothing in it has
    // a place of its own in the file. Not a record to read this way: the caller reads it whole, program_cache_read unpacks it)
    if (disk_zstd) return nullptr;
    constexpr size_t kFront = 4096, kTailMost = 8u << 20;
    struct Counted {   // (the census, as program_cache_read counts)
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Counted() {
            const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            use_counters.loads++, use_counters.load_ns += ns;
            if (programs_in_play.load(std::memory_order_relaxed) && !use_counters.background) census.play_loads++, census.play_load_ns += ns;
        }
    } counted;
    std::vector<uint8_t> front(16 + std::min(d.size, kFront));
    {
        std::unique_lock<std::mutex> lk;
        if (mu) lk = std::unique_lock<std::mutex>(*mu);
        _fseeki64(in, d.at - 16, SEEK_SET);
        if (fread(front.data(), 1, front.size(), in) != front.size()) return nullptr;
    }
    uint64_t head[2];
    std::memcpy(head, front.data(), 16);
    if (head[0] != key || head[1] != d.size) return nullptr;
    Reader r{front.data() + 16, front.data() + front.size()};
    auto p = std::make_unique<Program>();
    p->key = key;
    get_head(r, *p);
    p->variant = answers_hash(*p);
    if (r.get<uint64_t>() != 1 || r.bad || !p->compute) return nullptr;   // (one stage: a compute program's)
    p->stages.emplace_back();
    Stage& st = p->stages.back();
    st.nvn_stage = r.get<int>();
    st.vk_stage = r.get<VkShaderStageFlagBits>();
    const uint64_t words = r.get<uint64_t>();
    if (r.bad) return nullptr;
    const size_t code_at = (size_t)(r.p - (front.data() + 16));
    if (!words || words > (d.size - code_at) / 4) return nullptr;
    const size_t tail_at = code_at + (size_t)words * 4;
    if (d.size - tail_at > kTailMost) return nullptr;
    std::vector<uint8_t> tail(d.size - tail_at);
    if (!tail.empty()) {
        std::unique_lock<std::mutex> lk;
        if (mu) lk = std::unique_lock<std::mutex>(*mu);
        _fseeki64(in, d.at + (int64_t)tail_at, SEEK_SET);
        if (fread(tail.data(), 1, tail.size(), in) != tail.size()) return nullptr;
    }
    Reader t{tail.data(), tail.data() + tail.size()};
    t.list(st.c1);
    get_info(t, st.info);
    if (t.bad || !answers_fit(*p, p->answers)) return nullptr;
    p->code_at = d.at + (int64_t)code_at, p->code_words = words;
    p->ready.store(true, std::memory_order_release);
    return p;
}
namespace {
/// (in: a handle on the cache's file, used under mu when given) the code read and the module made
bool complete_from(Program& p, FILE* in, std::mutex* mu) {
    if (!p.code_words) return !p.failed;
    const int64_t code_at = p.code_at;
    std::vector<uint32_t> spirv((size_t)p.code_words);
    p.code_words = 0;
    bool ok = in != nullptr;
    if (ok) {
        std::unique_lock<std::mutex> dk;
        if (mu) dk = std::unique_lock<std::mutex>(*mu);
        _fseeki64(in, code_at, SEEK_SET);
        ok = fread(spirv.data(), 4, spirv.size(), in) == spirv.size();
    }
    Stage& st = p.stages[0];
    if (ok) st.module = make_module(spirv);
    if (!ok || !st.module) {
        static std::atomic<int> logged{0};
        if (logged++ < 20)
            logf("vk: program cache: the code of compute program %016llx at %lld did not read or made no module: left out",
                 (unsigned long long)p.key, (long long)code_at);
        p.failed = true;
        return false;
    }
    st.phis = fallback_size::spirv_phis(spirv.data(), spirv.size());
    module_id_learned(p.key, p.variant, st.module);   // (the next run's load makes its pipeline without this read: vk_programs.h)
    return true;
}
}   // namespace
bool program_cache_complete(Program& p) {
    std::lock_guard lk(light_mu);
    return complete_from(p, disk_in, &disk_mu);
}
/// (the load at a program's create call, vk_programs.cpp) the same for a program no other thread has yet, with the worker's
/// own handle on the file: neither light_mu nor disk_mu is held through its read, which a dispatch on the replay's thread
/// would wait behind
bool program_cache_complete_unshared(Program& p, FILE* in) {
    return complete_from(p, in, nullptr);
}

/// (shader_mu held) the kept variants of key go to its list, ready, once
void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool keep) {
    disk_init();
    if (!disk_on) return;
    // (a load the game's create call queued for this key (vk_programs.cpp) is run or waited for here, and what it read is in the
    // list before the rest is looked for: its records are not read twice, and its program is not translated a second time)
    if (!keep) programs_load_wait(key);
    std::vector<DiskRecord> recs;
    {
        std::lock_guard lk(disk_mu);
        auto it = disk_records.find(key);
        if (keep) {   // (borrowed: still on disk for play)
            // (and the records read in already: a program the state list loaded for its notes still has variants no note names,
            // which the prediction lists (program_cache_each walks disk_taken) and borrows. Their pipelines were never built: of
            // 67,106 predicted, 65,449 made, and nothing said why)
            if (it != disk_records.end()) recs = it->second;
            if (const auto t = disk_taken.find(key); t != disk_taken.end()) recs.insert(recs.end(), t->second.begin(), t->second.end());
            if (recs.empty()) return;
        } else {
            if (it == disk_records.end()) return;
            recs = std::move(it->second);
            disk_records.erase(it);
            disk_taken[key].insert(disk_taken[key].end(), recs.begin(), recs.end());
        }
    }
    PROF_EXACT();   // (from disk: a mesh program's too)
    static const bool whole = env_on("RX_VK_LOAD_ALL_VARIANTS");
    if (keep || whole || recs.empty() || !(recs[0].kind & 1)) {
        program_cache_read(disk_in, &disk_mu, key, recs, variants);
        return;
    }
    // (a compute program at its first pick: each variant without its code, in the file's order as before; one that does not
    // read that way, whole)
#ifndef _WIN32
    // (two small reads a variant, anywhere in a file of gigabytes that is seldom in memory: all asked of the disk first, so
    // that it serves them together and not one after the other. A record's head is its first bytes, its descriptors its last)
    if (disk_in)
        for (const DiskRecord& d : recs) {
            posix_fadvise(fileno(disk_in), d.at - 16, 4096, POSIX_FADV_WILLNEED);
            if (d.size > 8192) posix_fadvise(fileno(disk_in), d.at + (int64_t)d.size - 8192, 8192, POSIX_FADV_WILLNEED);
        }
#endif
    for (const DiskRecord& d : recs) {
        if (std::unique_ptr<Program> p = program_cache_read_light(disk_in, &disk_mu, key, d)) {
            variants.push_back(std::move(p));
            st_cache_hits++;
        } else {
            program_cache_read(disk_in, &disk_mu, key, {d}, variants);
        }
    }
}

// ---- (hookshot) The records by position, for the load when the game makes a program (vk_programs.cpp) ----
bool program_cache_has(uint64_t key) {
    disk_init();
    if (!disk_on) return false;
    std::lock_guard lk(disk_mu);
    const auto it = disk_records.find(key);
    return it != disk_records.end() && !it->second.empty();
}
std::vector<DiskRecord> program_cache_records(uint64_t key) {
    disk_init();
    if (!disk_on) return {};
    std::lock_guard lk(disk_mu);
    const auto it = disk_records.find(key);
    return it == disk_records.end() ? std::vector<DiskRecord>{} : it->second;
}
bool program_cache_variant(FILE* in, uint64_t key, const DiskRecord& d, uint64_t* variant) {
    if (disk_zstd) return false;   // (a compressed record's head is not read by its place: the caller takes the record whole)
    // (a record's head: its key and size, the kind word, then the answers, which are what its variant is the hash of)
    constexpr size_t kFront = 1024;
    std::vector<uint8_t> front(16 + std::min(d.size, kFront));
    if (!in || _fseeki64(in, d.at - 16, SEEK_SET) || fread(front.data(), 1, front.size(), in) != front.size()) return false;
    uint64_t head[2];
    std::memcpy(head, front.data(), 16);
    if (head[0] != key || head[1] != d.size) return false;
    Reader r{front.data() + 16, front.data() + front.size()};
    Program p;
    r.get<uint32_t>();
    r.list(p.answers);
    if (r.bad) return false;   // (more answers than the front holds: the caller reads the record whole)
    *variant = answers_hash(p);
    return true;
}
std::vector<DiskRecord> program_cache_take(uint64_t key, const std::vector<DiskRecord>& want) {
    std::vector<DiskRecord> taken;
    std::lock_guard lk(disk_mu);
    const auto it = disk_records.find(key);
    if (it == disk_records.end()) return taken;
    auto& have = it->second;
    for (const DiskRecord& w : want) {
        const auto at = std::find_if(have.begin(), have.end(), [&](const DiskRecord& d) { return d.at == w.at; });
        if (at == have.end()) continue;
        taken.push_back(*at);
        have.erase(at);
    }
    auto& gone = disk_taken[key];
    gone.insert(gone.end(), taken.begin(), taken.end());
    if (have.empty()) disk_records.erase(it);
    return taken;
}
std::unordered_set<uint64_t> program_cache_kept_keys() {
    disk_init();
    std::unordered_set<uint64_t> keys;
    if (!disk_on) return keys;
    std::lock_guard lk(disk_mu);
    for (const auto& kv : disk_records)
        if (!kv.second.empty()) keys.insert(kv.first);
    return keys;
}
std::vector<uint64_t> program_cache_compute_keys() {
    disk_init();
    std::vector<uint64_t> keys;
    if (!disk_on) return keys;
    std::lock_guard lk(disk_mu);
    for (const auto& kv : disk_records)
        if (!kv.second.empty() && (kv.second[0].kind & 1)) keys.push_back(kv.first);
    return keys;
}
std::string program_cache_path() {
    disk_init();
    return disk_on ? disk_path : std::string();
}

/// (the precompile, before the game: vk_draw.cpp) the kept compute programs and those named, ready, each given to fn. The rest of the
/// game's programs stay on disk until a draw asks for one
size_t program_cache_load_some(const std::unordered_set<uint64_t>& wanted, const std::function<void(Program&)>& fn, bool computes,
                               bool fallbacks) {
    disk_init();
    // (read and made on several threads, each with a handle of its own on the file: one after the other, the few hundred records
    // (about 200 MB) were a second or more of the start before the first pipeline was built, a CPU or the disk at a time)
    std::vector<std::pair<uint64_t, std::vector<DiskRecord>>> work;
    if (disk_on) {
        std::lock_guard lk(disk_mu);
        for (auto it = disk_records.begin(); it != disk_records.end();) {
            // (fallbacks: and every program translated for the mesh fallback (kind 4), not only those play has drawn, whatever the
            // compute list says: their compute pipelines, made in play, took AMD up to 12 s each on first sight, the foliage and the
            // door white until then. Only a GPU without mesh shaders has such programs)
            if (wanted.count(it->first) ||
                (!it->second.empty() && ((computes && (it->second[0].kind & 1)) || (fallbacks && (it->second[0].kind & 4))))) {
                work.push_back({it->first, std::move(it->second)});
                auto& taken = disk_taken[it->first];
                taken.insert(taken.end(), work.back().second.begin(), work.back().second.end());
                it = disk_records.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::vector<std::vector<std::unique_ptr<Program>>> made(work.size());
    std::atomic<size_t> next{0};
    const auto worker = [&] {
        FILE* in = fopen(disk_path.c_str(), "rb");
        for (;;) {
            const size_t i = next++;
            if (i >= work.size()) break;
            if (in) program_cache_read(in, nullptr, work[i].first, work[i].second, made[i]);
            else program_cache_read(disk_in, &disk_mu, work[i].first, work[i].second, made[i]);
        }
        if (in) fclose(in);
    };
    {
        const unsigned cpus = usable_cpus();
        std::vector<std::thread> threads;
        for (unsigned i = 1; i < std::min({cpus, 16u, (unsigned)work.size()}); i++) threads.emplace_back(worker);
        worker();
        for (std::thread& t : threads) t.join();
    }
    std::lock_guard lk(shader_mu);
    size_t n = 0;
    for (size_t i = 0; i < work.size(); i++) {
        auto& variants = programs[work[i].first];
        for (auto& p : made[i]) variants.push_back(std::move(p));
        for (auto& p : variants) n++, fn(*p);
    }
    // (and the mesh fallback programs in memory already, whose records an earlier read took: the state lists' programs in the
    // start that prepared the shaders. They had no compute pipelines until their first draw, which was left out: fallback_loaded.h)
    static const bool loaded_too = fallback_loaded::on(getenv("RX_VK_FALLBACK_LOADED"));
    if (fallbacks && loaded_too) {
        std::unordered_set<uint64_t> given;
        for (const auto& w : work) given.insert(w.first);
        const size_t loaded = fallback_loaded::each(programs, given, fn);
        if (loaded) logf("vk: %zu mesh fallback programs read in earlier in this start are made ready with the rest", loaded);
        n += loaded;
    }
    return n;
}

/// (hookshot) Program key's kept variants made for fn, then let go (modules and layouts): the records stay for play
void program_cache_borrow(uint64_t key, const std::function<void(Program&)>& fn) {
    std::vector<std::unique_ptr<Program>> variants;
    // Own the handles before loading: both the loader and the callback may throw.
    // Program's vectors are RAII-managed, but its Vulkan handles are not.
    struct Cleanup {
        std::vector<std::unique_ptr<Program>>& variants;
        static void release(Program& p) {
            for (Stage& st : p.stages)
                if (st.module) vkDestroyShaderModule(dev.device, st.module, nullptr), st.module = VK_NULL_HANDLE;
            if (p.layout) vkDestroyPipelineLayout(dev.device, p.layout, nullptr), p.layout = VK_NULL_HANDLE;
            if (p.set_layout) vkDestroyDescriptorSetLayout(dev.device, p.set_layout, nullptr), p.set_layout = VK_NULL_HANDLE;
        }
        ~Cleanup() {
            for (auto& p : variants) release(*p);
        }
    } cleanup{variants};
    program_cache_load(key, variants, true);
    for (auto& p : variants) {
        fn(*p);
        Cleanup::release(*p);   // (release each variant before starting the next one)
    }
}

/// (hookshot, the prepare's driver part: vk_draw.cpp warm_libraries) the key of every kept graphics and mesh program, whoever made
/// it (the seed, a state list, play), in the file's order, from the list of records alone: program_cache_each reads every record
/// for its list, the whole file (6.7 GB with every program of the game). *loaded: the variants read in already, whose records
/// that list no longer names (see loaded_variants)
std::vector<uint64_t> program_cache_keys(std::vector<Program*>* loaded) {
    disk_init();
    std::vector<std::pair<int64_t, uint64_t>> at;
    if (disk_on) {
        std::lock_guard lk(disk_mu);
        for (const auto& kv : disk_records)
            if (!kv.second.empty() && !(kv.second[0].kind & 1)) at.push_back({kv.second[0].at, kv.first});
    }
    std::sort(at.begin(), at.end());   // (file order: borrowed one after the other, the disk is read forwards)
    std::vector<uint64_t> keys;
    keys.reserve(at.size());
    for (const auto& a : at) keys.push_back(a.second);
    std::lock_guard lk(shader_mu);
    for (const auto& kv : programs)
        for (const auto& p : kv.second)
            if (p->ready.load(std::memory_order_acquire) && !p->failed && !p->compute) loaded->push_back(p.get());
    return keys;
}

/// (hookshot, the prediction: vk_predict.cpp) every kept graphics program variant, read without making its modules. fn is called on
/// several threads at once, in no order
void program_cache_each(const std::function<void(const KeptProgram&)>& fn) {
    disk_init();
    if (!disk_on) return;
    std::vector<std::pair<uint64_t, DiskRecord>> all;
    {
        std::lock_guard lk(disk_mu);
        for (const auto* list : {&disk_records, &disk_taken})
            for (const auto& kv : *list)
                for (const DiskRecord& d : kv.second)
                    if (!(d.kind & 1)) all.push_back({kv.first, d});
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.at < b.second.at; });   // (file order)
    // (read on several threads, each with a handle of its own and a run of records at a time, as program_cache_load_some: on one,
    // under disk_mu, every record of the game (5.6 GB) was 7-16 s of the prediction's "deciding", the file in memory already)
    constexpr size_t kRun = 64;
    std::atomic<size_t> next{0};
    const auto worker = [&] {
        FILE* in = fopen(disk_path.c_str(), "rb");
        KeptProgram k;
        std::vector<uint8_t> data;
        for (;;) {
            const size_t first = next.fetch_add(kRun);
            if (first >= all.size()) break;
            for (size_t i = first; i < std::min(first + kRun, all.size()); i++) {
                const auto& [key, d] = all[i];
                data.resize(d.size);
                if (in) {
                    if (_fseeki64(in, d.at, SEEK_SET) || fread(data.data(), 1, d.size, in) != d.size) continue;
                } else {
                    std::lock_guard lk(disk_mu);
                    _fseeki64(disk_in, d.at, SEEK_SET);
                    if (fread(data.data(), 1, d.size, disk_in) != d.size) continue;
                }
                std::vector<uint8_t> plain;
                const uint8_t* payload = nullptr;
                size_t payload_size = 0;
                if (!program_cache_payload(data.data(), data.size(), d.kind, plain, &payload, &payload_size)) continue;
                Reader r{payload, payload + payload_size};
                Program p;
                p.key = key;
                get_head(r, p);   // (the whole head: reading past only part of it took the fallback's fields for the stage count)
                k.key = key, k.variant = answers_hash(p), k.mesh = p.mesh, k.at = d.at;
                const uint64_t n = r.count(1);
                k.stages.resize(n <= 8 ? n : 0);
                for (auto& [stage, spirv] : k.stages) {
                    r.get<int>();
                    stage = r.get<VkShaderStageFlagBits>();
                    r.list(spirv);
                    std::vector<uint8_t> c1;
                    r.list(c1);
                    Shader::Info info;
                    get_info(r, info);
                }
                if (!r.bad && !k.stages.empty()) fn(k);
            }
        }
        if (in) fclose(in);
    };
    const unsigned cpus = usable_cpus();
    std::vector<std::thread> threads;
    for (unsigned i = 1; i < std::min<size_t>({cpus, 16u, (all.size() + kRun - 1) / kRun}); i++) threads.emplace_back(worker);
    worker();
    for (std::thread& t : threads) t.join();
}

unsigned usable_cpus() {
    const unsigned all = std::max(1u, std::thread::hardware_concurrency());
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc) return all;
    unsigned n = 0;
    for (uint64_t m = (uint64_t)proc; m; m &= m - 1) n++;
    // (a mask covers one processor group, 64 CPUs at most: past that, Windows may run the process on every group)
    return n < 64 ? std::clamp(n, 1u, all) : all;
}
unsigned half_cpus() {
    return std::max(1u, std::min(std::max(1u, std::thread::hardware_concurrency()) / 2, usable_cpus()));
}

} // namespace mkwvk
