// tests/test_pcache_stamp.py compiles and runs this: gpu/vk/pcache_stamp.h, what the pipeline cache's files were made for.
#include "../gpu/vk/pcache_stamp.h"

#include <cstdio>
#include <filesystem>
#include <string>

static int failed;
#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            std::printf("FAILED line %d: %s\n", __LINE__, #x);  \
            failed++;                                           \
        }                                                       \
    } while (0)

static void put(const std::string& path, size_t bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    for (size_t i = 0; i < bytes; i++) fputc('x', f);
    fclose(f);
}
static bool there(const std::string& path) { return std::filesystem::exists(path); }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string dir = std::string(argv[1]) + "/";
    using namespace pcache_stamp;

    // the names it takes for the cache's: the main file and the play files, with their .tmp; nothing else in the folder
    CHECK(is_cache_file("mkw_vk_pipelines.bin") && is_cache_file("mkw_vk_pipelines.bin.tmp"));
    CHECK(is_cache_file("mkw_vk_pipelines_play_1.bin") && is_cache_file("mkw_vk_pipelines_play_123.bin.tmp"));
    CHECK(!is_cache_file("mkw_vk_pipelines.stamp") && !is_cache_file("hookshot_programs.bin") && !is_cache_file("hookshot_pipelines.bin"));
    CHECK(!is_cache_file("mkw_vk_pipelines_play_.bin") && !is_cache_file("mkw_vk_pipelines_play_x.bin") &&
          !is_cache_file("mkw_vk_pipelines_play_1.txt") && !is_cache_file("mkw_vk_pipelines_play_1"));
    CHECK(!is_cache_file("old_mkw_vk_pipelines.bin") && !is_cache_file("save.bin"));

    // a folder from before the stamp file: its cache files go, the others stay, and the stamp is written
    put(dir + "mkw_vk_pipelines.bin", 3000);
    put(dir + "mkw_vk_pipelines_play_1.bin", 200);
    put(dir + "mkw_vk_pipelines_play_7.bin", 100);
    put(dir + "hookshot_programs.bin", 50);
    put(dir + "save.bin", 10);
    Dropped d = check(dir, 0x1234);
    CHECK(d.files == 3 && d.bytes == 3300);
    CHECK(!there(dir + "mkw_vk_pipelines.bin") && !there(dir + "mkw_vk_pipelines_play_1.bin") && !there(dir + "mkw_vk_pipelines_play_7.bin"));
    CHECK(there(dir + "hookshot_programs.bin") && there(dir + "save.bin"));
    uint64_t s = 0;
    CHECK(read(dir, &s) && s == 0x1234);

    // the same stamp: nothing goes
    put(dir + "mkw_vk_pipelines.bin", 4000);
    put(dir + "mkw_vk_pipelines_play_2.bin", 300);
    d = check(dir, 0x1234);
    CHECK(d.files == 0 && there(dir + "mkw_vk_pipelines.bin") && there(dir + "mkw_vk_pipelines_play_2.bin"));

    // another translator: they go, the stamp is the new one
    d = check(dir, 0x5678);
    CHECK(d.files == 2 && d.bytes == 4300 && !there(dir + "mkw_vk_pipelines.bin"));
    CHECK(read(dir, &s) && s == 0x5678);

    // an empty folder: nothing to drop, the stamp written; a damaged stamp file counts as none
    d = check(dir, 0x5678);
    CHECK(d.files == 0);
    put(dir + "mkw_vk_pipelines.stamp", 3);
    CHECK(!read(dir, &s));
    put(dir + "mkw_vk_pipelines.bin", 10);
    d = check(dir, 0x5678);
    CHECK(d.files == 1 && read(dir, &s) && s == 0x5678);
    put(dir + "mkw_vk_pipelines.stamp", 9);   // (a byte too many)
    CHECK(!read(dir, &s));

    if (failed) return 1;
    std::printf("pcache_stamp: ok\n");
    return 0;
}
