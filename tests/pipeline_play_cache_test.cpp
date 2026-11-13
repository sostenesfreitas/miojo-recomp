// The play caches of gpu/vk/vk_draw.cpp (struct PlayCache to the end of create_cached) extracted unmodified, on a model of
// Vulkan pipeline caches: a cache is a set of pipeline keys, its data "FAKE" and the keys. The driver keeps a disk cache of its own,
// and, as NVIDIA's does, copies a pipeline it takes from there into the cache the pipeline is asked of. No driver is loaded.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX   // (the extracted code calls std::max: no min/max macros from windows.h)
#include <windows.h>
#else
constexpr unsigned MOVEFILE_REPLACE_EXISTING = 1;
void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif

using VkResult = int;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_UNKNOWN = -13, VK_PIPELINE_COMPILE_REQUIRED = 1000297000;
constexpr unsigned VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT = 0x100;
constexpr unsigned VK_PIPELINE_CREATE_LIBRARY_BIT_KHR = 0x800;
enum { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO = 17 };
struct FakeCache {
    std::mutex mu;
    std::set<uint64_t> keys;
};
struct FakePipeline {};
using VkPipelineCache = FakeCache*;
using VkPipeline = FakePipeline*;
#define VK_NULL_HANDLE nullptr
struct VkPipelineCacheCreateInfo {
    int sType;
    const void* pNext = nullptr;
    unsigned flags = 0;
    size_t initialDataSize = 0;
    const void* pInitialData = nullptr;
};
struct {
    int device = 1;
    struct { bool pipelineCreationCacheControl = true; } vk13;
} dev;
VkPipelineCache pcache = nullptr;
std::string pcache_dir, pcache_path;
std::atomic<int> creates{0}, destroys{0}, merges{0}, compiles{0}, failed_moves{0};
std::atomic<int> fail_moves{0};   // (the next moves into place that fail)
FakePipeline the_pipeline;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

void logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}
std::set<uint64_t> parse(const uint8_t* p, size_t n) {
    std::set<uint64_t> keys;
    for (size_t at = 4; at + 8 <= n; at += 8) {
        uint64_t k;
        std::memcpy(&k, p + at, 8);
        keys.insert(k);
    }
    return keys;
}
/// (vk_draw.cpp's own checks the header: here a file not starting "FAKE" is another driver's)
static bool fail_reads = false, fail_cache_create = false;
struct ReadPart : std::vector<uint8_t> {
    bool incompatible = false;
    bool rejected() const { return incompatible; }
    bool reset() { clear(); incompatible=false; return true; }
};
ReadPart pcache_file_read(const std::string& path) {
    ReadPart data;
    if (fail_reads) return data;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
    }
    if (data.size() < 4 || std::memcmp(data.data(), "FAKE", 4)) { data.clear(); data.incompatible = true; }
    return data;
}
VkResult vkCreatePipelineCache(int, const VkPipelineCacheCreateInfo* ci, const void*, VkPipelineCache* out) {
    if (fail_cache_create) return VK_ERROR_UNKNOWN;
    auto* c = new FakeCache;
    if (ci->initialDataSize) c->keys = parse((const uint8_t*)ci->pInitialData, ci->initialDataSize);
    ++creates;
    *out = c;
    return VK_SUCCESS;
}
void vkDestroyPipelineCache(int, VkPipelineCache c, const void*) {
    ++destroys;
    delete c;
}
VkResult vkGetPipelineCacheData(int, VkPipelineCache c, size_t* n, void* data) {
    std::lock_guard lk(c->mu);
    const size_t size = 4 + 8 * c->keys.size();
    if (!data) return *n = size, VK_SUCCESS;
    if (*n < size) return VK_ERROR_UNKNOWN;
    auto* p = (uint8_t*)data;
    std::memcpy(p, "FAKE", 4);
    size_t at = 4;
    for (uint64_t k : c->keys) std::memcpy(p + at, &k, 8), at += 8;
    *n = size;
    return VK_SUCCESS;
}
VkResult vkMergePipelineCaches(int, VkPipelineCache dst, uint32_t count, const VkPipelineCache* src) {
    for (uint32_t i = 0; i < count; i++) {
        if (src[i] == dst) std::abort();
        std::scoped_lock lk(dst->mu, src[i]->mu);
        dst->keys.insert(src[i]->keys.begin(), src[i]->keys.end());
    }
    ++merges;
    return VK_SUCCESS;
}
int move_into_place(const char* from, const char* to, unsigned flags) {
    if (flags != MOVEFILE_REPLACE_EXISTING) std::abort();
    if (fail_moves.load() > 0) {
        --fail_moves, ++failed_moves;
        return 0;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
}

// ---- the driver: its own disk cache, and what a pipeline's making does to the cache it is asked of ----
std::mutex driver_mu;
std::set<uint64_t> driver;
std::atomic<uint64_t> block_key{0};   // (a compile of this key waits for release: one under way while a save comes)
std::atomic<bool> blocked{false}, release{false};
struct FakeCI {
    unsigned flags = 0;
    uint64_t key = 0;
};
bool has(VkPipelineCache c, uint64_t key) {
    std::lock_guard lk(c->mu);
    return c->keys.count(key) != 0;
}
VkResult fake_create(VkPipelineCache c, const FakeCI* ci, VkPipeline* pl) {
    *pl = (VkPipeline)&driver;   // (not a pipeline: the caller must not take it on failure)
    if (!c) std::abort();
    if (ci->flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) {
        if (has(c, ci->key)) return *pl = &the_pipeline, VK_SUCCESS;
        bool in_driver;
        {
            std::lock_guard lk(driver_mu);
            in_driver = driver.count(ci->key) != 0;
        }
        if (!in_driver) return VK_PIPELINE_COMPILE_REQUIRED;
        std::lock_guard lk(c->mu);
        c->keys.insert(ci->key);   // (NVIDIA: what it takes from its own cache goes into the one asked)
        return *pl = &the_pipeline, VK_SUCCESS;
    }
    ++compiles;
    if (ci->key == block_key.load()) {
        blocked = true;
        while (!release.load()) Sleep(1);
    }
    {
        std::lock_guard lk(driver_mu);
        driver.insert(ci->key);
    }
    std::lock_guard lk(c->mu);
    c->keys.insert(ci->key);
    return *pl = &the_pipeline, VK_SUCCESS;
}

#ifdef _WIN32
static BOOL mapped_move(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (fail_moves.load() > 0) { --fail_moves; ++failed_moves; return FALSE; }
    return MoveFileExW(from, to, flags);
}
#define MoveFileExW mapped_move
#else
static int mapped_move(const char* from, const char* to) {
    if (fail_moves.load() > 0) { --fail_moves; ++failed_moves; return -1; }
    return rename(from, to);
}
#define rename mapped_move
#endif
#include "pipeline_cache_file.h"
using mkwvk::PipelineCacheFile;
#ifdef _WIN32
#undef MoveFileExW
#else
#undef rename
#endif
// (the census's counters, gpu/vk/vk_programs.h: counted, not checked here)
struct { std::atomic<int64_t> play_from_driver{0}, play_from_start{0}, play_compiled{0}; } census;
thread_local struct { uint8_t made_how = 0; } use_counters;
#include "pipeline_play_cache.inc"

VkResult make(uint64_t key, unsigned flags = 0) {
    FakeCI ci;
    ci.key = key, ci.flags = flags;
    VkPipeline pl = VK_NULL_HANDLE;
    const VkResult r = create_cached(fake_create, &ci, &pl);
    if ((r == VK_SUCCESS) != (pl == &the_pipeline)) std::abort();   // (a failure leaves no pipeline behind)
    return r;
}
std::set<uint64_t> keys_of(VkPipelineCache c) {
    std::lock_guard lk(c->mu);
    return c->keys;
}
size_t size_of(VkPipelineCache c) {
    size_t n = 0;
    vkGetPipelineCacheData(dev.device, c, &n, nullptr);
    return n;
}
template <class F>
bool wait_for(F done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < end) Sleep(1);
    return done();
}
bool exists(const std::string& path) {
    return std::filesystem::exists(path);
}
std::set<uint64_t> file_keys(const std::string& path) {
    const std::vector<uint8_t> data = pcache_file_read(path);
    return data.empty() ? std::set<uint64_t>{} : parse(data.data(), data.size());
}
void write_file(const std::string& path, const std::string& head, std::set<uint64_t> keys) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(head.data(), 1, head.size(), f);
    for (uint64_t k : keys) std::fwrite(&k, 8, 1, f);
    std::fclose(f);
}
std::string at(const char* name) {
    return pcache_dir + name;
}
VkPipelineCache cache_of(std::set<uint64_t> keys) {
    VkPipelineCache c;
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(dev.device, &ci, nullptr, &c);
    c->keys = std::move(keys);
    return c;
}

int run(const std::string& name) {
    if (name == "route") {
        pcache = cache_of({1});   // (1: the start's cache alone has it)
        driver = {2};             // (2: the driver's own cache alone)
        CHECK(make(3) == VK_SUCCESS && has(pcache, 3) && compiles == 1);   // (before play: into the start's cache)
        play_cache_begin();
        PlayCache* g = play_cache.load();
        CHECK(g && g->cache);
        const size_t start = size_of(pcache);
        CHECK(make(1) == VK_SUCCESS && g->from_start == 1 && keys_of(g->cache).empty());
        CHECK(make(2) == VK_SUCCESS && g->from_driver == 1 && keys_of(g->cache) == std::set<uint64_t>{2});
        CHECK(make(4) == VK_SUCCESS && g->compiled == 1 && compiles == 2 && keys_of(g->cache) == (std::set<uint64_t>{2, 4}));
        CHECK(g->built == 2);
        // (one the caller already asks without compiling, as a dispatch does: the same order, no compile)
        constexpr unsigned ask = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        CHECK(make(5, ask) == VK_PIPELINE_COMPILE_REQUIRED);   // (nowhere: not compiled)
        CHECK(compiles == 2 && !has(g->cache, 5) && !has(pcache, 5));
        driver.insert(6);
        CHECK(make(6, ask) == VK_SUCCESS && has(g->cache, 6) && !has(pcache, 6));   // (the driver's: into the generation)
        CHECK(make(1, ask) == VK_SUCCESS && g->from_start == 2);   // (the start's cache's: from it)
        CHECK(compiles == 2 && size_of(pcache) == start && keys_of(pcache) == (std::set<uint64_t>{1, 3}));   // (left as it was read)
        // (a stage library asked without compiling: the start's cache first, where the warm pass put the libraries, so the
        // driver's own cache is not gone to for it; then the generation; no compile)
        constexpr unsigned lib = ask | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        const int64_t driver0 = g->from_driver.load(), built0 = g->built.load();
        CHECK(make(7, lib) == VK_PIPELINE_COMPILE_REQUIRED && compiles == 2 && !has(g->cache, 7) && !has(pcache, 7));
        CHECK(make(1, lib) == VK_SUCCESS && g->from_start == 3 && size_of(pcache) == start);   // (the start's cache's: from it)
        CHECK(g->from_driver == driver0 && g->built == built0);                             // (and nothing asked of the generation)
        driver.insert(8);
        // (one the warm pass did not leave in the start's cache: the driver answers the ask of it and copies it in, in memory)
        CHECK(make(8, lib) == VK_SUCCESS && g->from_start == 4 && has(pcache, 8) && !has(g->cache, 8));
        {   // (one the generation alone holds, on a driver with no cache of its own: from the generation)
            std::lock_guard lk(g->cache->mu);
            g->cache->keys.insert(9);
        }
        CHECK(make(9, lib) == VK_SUCCESS && g->from_driver == driver0 + 1 && !has(pcache, 9));
        CHECK(compiles == 2 && keys_of(pcache) == (std::set<uint64_t>{1, 3, 8}));
        play_cache_begin();   // (once)
        CHECK(play_cache.load() == g);
    } else if (name == "save") {
        pcache = cache_of({1});
        play_cache_save();   // (before play: nothing)
        CHECK(!exists(play_path(1)));
        play_cache_begin();
        PlayCache* g = play_cache.load();
        CHECK(make(10) == VK_SUCCESS && make(11) == VK_SUCCESS);
        play_cache_save();
        CHECK(file_keys(play_path(1)) == (std::set<uint64_t>{10, 11}) && play_seq == 1);
        CHECK(play_cache.load() != g && keys_of(play_cache.load()->cache).empty() && !g->cache && destroys == 1);
        CHECK(make(1) == VK_SUCCESS);   // (from the start's cache: nothing for a play file)
        play_cache_save();
        CHECK(!exists(play_path(2)));
        CHECK(make(12) == VK_SUCCESS);
        play_cache_save();
        // (issue 2579: one file a session, written again with each save's generation)
        CHECK(file_keys(play_path(1)) == (std::set<uint64_t>{10, 11, 12}) && !exists(play_path(2)));
        CHECK(keys_of(pcache) == std::set<uint64_t>{1});
    } else if (name == "retry") {
        pcache = cache_of({});
        play_cache_begin();
        CHECK(make(20) == VK_SUCCESS);
        fail_moves = 1;
        play_cache_save();   // (not in place: kept)
        CHECK(failed_moves == 1 && !exists(play_path(1)) && play_seq == 0 && destroys == 1);   // (kept in the session's cache)
        CHECK(make(21) == VK_SUCCESS);
        play_cache_save();   // (the one kept and this one's, in the session's one file)
        CHECK(file_keys(play_path(1)) == (std::set<uint64_t>{20, 21}) && !exists(play_path(2)));
        CHECK(destroys == 2 && play_seq == 1);
        fail_moves = 1;
        play_cache_save();   // (nothing new: nothing written)
        CHECK(failed_moves == 1);
        CHECK(make(22) == VK_SUCCESS);
        play_cache_save();   // (a failed write of the session's file is tried again at the next save, with nothing new)
        CHECK(failed_moves == 2 && file_keys(play_path(1)) == (std::set<uint64_t>{20, 21}));
        play_cache_save();
        CHECK(file_keys(play_path(1)) == (std::set<uint64_t>{20, 21, 22}));
    } else if (name == "users") {
        pcache = cache_of({});
        play_cache_begin();
        PlayCache* g = play_cache.load();
        block_key = 30;
        std::thread maker([] { make(30); });
        CHECK(wait_for([] { return blocked.load(); }));
        play_cache_save();   // (nothing made in it yet, only one under way: not taken away)
        CHECK(play_cache.load() == g && !exists(play_path(1)));
        CHECK(make(29) == VK_SUCCESS && g->built == 1);
        std::atomic<bool> saved{false};
        std::thread saver([&] { play_cache_save(); saved = true; });
        CHECK(wait_for([&] { return play_cache.load() != g; }));   // (taken away; the save waits for the compile under way)
        CHECK(make(31) == VK_SUCCESS && has(play_cache.load()->cache, 31) && !has(g->cache, 31));
        Sleep(50);
        CHECK(!saved.load() && !exists(play_path(1)));
        release = true;
        maker.join();
        saver.join();
        CHECK(file_keys(play_path(1)) == (std::set<uint64_t>{29, 30}));   // (the compile under way in its generation's file)
        play_cache_save();
        CHECK(file_keys(play_path(1)) == (std::set<uint64_t>{29, 30, 31}) && !exists(play_path(2)));
    } else if (name == "merge-read-failed" || name == "merge-create-failed") {
        pcache = cache_of({1});
        write_file(play_path(1), "FAKE", {40});
        fail_reads = name == "merge-read-failed";
        fail_cache_create = name == "merge-create-failed";
        CHECK(play_files_merge() == 0);
        CHECK(exists(play_path(1)) == (name == "merge-read-failed"));
        fail_reads = false; fail_cache_create = false;
        if(name == "merge-create-failed")write_file(play_path(1), "FAKE", {40});
        CHECK(file_keys(play_path(1)) == std::set<uint64_t>{40});
        CHECK(play_files_merge() == 1 && has(pcache, 40));
    } else if (name == "merge") {
        pcache = cache_of({1});
        write_file(at("mkw_vk_pipelines_play_3.bin"), "FAKE", {40});
        write_file(at("mkw_vk_pipelines_play_7.bin"), "FAKE", {41, 1});
        write_file(at("mkw_vk_pipelines_play_5.bin"), "OTHR", {42});      // (another driver's: removed)
        write_file(at("mkw_vk_pipelines_play_.bin"), "FAKE", {43});       // (not a play file's name: left alone)
        write_file(at("mkw_vk_pipelines_play_x1.bin"), "FAKE", {44});
        write_file(at("mkw_vk_pipelines_play_9.bin.tmp"), "FAKE", {45});
        write_file(at("mkw_vk_pipelines_play_1234567890.bin"), "FAKE", {46});
        write_file(pcache_path, "FAKE", {1});
        CHECK(play_files_merge() == 2);
        CHECK(keys_of(pcache) == (std::set<uint64_t>{1, 40, 41}) && play_seq == 8);
        CHECK(!exists(at("mkw_vk_pipelines_play_5.bin")) && exists(at("mkw_vk_pipelines_play_.bin")));
        CHECK(exists(at("mkw_vk_pipelines_play_x1.bin")) && exists(at("mkw_vk_pipelines_play_9.bin.tmp")));
        CHECK(exists(at("mkw_vk_pipelines_play_1234567890.bin")));
        // (under 16: put together into one play file, numbered after the highest, in their place: issue 2579)
        CHECK(!exists(at("mkw_vk_pipelines_play_3.bin")) && !exists(at("mkw_vk_pipelines_play_7.bin")));
        CHECK(file_keys(play_path(8)) == (std::set<uint64_t>{1, 40, 41}));
        CHECK(file_keys(pcache_path) == std::set<uint64_t>{1});   // (the start's file as it was)
        play_cache_begin();
        CHECK(make(40) == VK_SUCCESS && play_cache.load()->from_start == 1);   // (merged: from the start's cache)
        CHECK(make(47) == VK_SUCCESS);
        play_cache_save();
        CHECK(file_keys(play_path(9)) == std::set<uint64_t>{47});   // (numbered after the highest read)
    } else if (name == "compact") {
        pcache = cache_of({1});
        write_file(pcache_path, "FAKE", {1});
        std::set<uint64_t> all{1};
        for (uint64_t i = 1; i <= kPlayFilesKept; i++) write_file(play_path((uint32_t)i), "FAKE", {100 + i}), all.insert(100 + i);
        CHECK(play_files_merge() == kPlayFilesKept);
        CHECK(keys_of(pcache) == all && file_keys(pcache_path) == all);   // (written into the one file)
        for (uint32_t i = 1; i <= kPlayFilesKept; i++) CHECK(!exists(play_path(i)));
        CHECK(play_seq == kPlayFilesKept);
    } else if (name == "compact-failed") {
        pcache = cache_of({1});
        write_file(pcache_path, "FAKE", {1});
        for (uint64_t i = 1; i <= kPlayFilesKept; i++) write_file(play_path((uint32_t)i), "FAKE", {100 + i});
        fail_moves = 1;
        CHECK(play_files_merge() == kPlayFilesKept);
        CHECK(file_keys(pcache_path) == std::set<uint64_t>{1});   // (not in place: the play files stay for the next start)
        for (uint32_t i = 1; i <= kPlayFilesKept; i++) CHECK(exists(play_path(i)));
    } else if (name == "off" || name == "no-control") {
        if (name == "no-control") dev.vk13.pipelineCreationCacheControl = false;
        pcache = cache_of({});
        play_cache_begin();
        CHECK(!play_cache.load());
        CHECK(make(50) == VK_SUCCESS && has(pcache, 50));   // (into the one cache, as before)
        play_cache_save();
        CHECK(!exists(play_path(1)));
    } else {
        CHECK(false);
    }
    std::printf("PASS: %s\n", name.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    pcache_dir = std::string(argv[2]) + "/";
    pcache_path = pcache_dir + "mkw_vk_pipelines.bin";
    return run(argv[1]);
}
