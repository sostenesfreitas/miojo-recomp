// (hookshot) What the pipeline cache's files were made for (vk_draw.cpp's pcache_init), apart from Vulkan so that
// tests/test_pcache_stamp.py can run it.
//
// mkw_vk_pipelines.bin is checked against the GPU, the driver and the cache UUID only. A new translator (or a switch that changes
// what it makes) gives every program new SPIR-V: the program cache and the prepare record start over (program_cache_stamp), but
// the pipeline cache kept every older translator's pipelines, none of which can be hit again. It only grew: 2.4 GB after a
// few dozen test builds in one folder, read whole at every start and held by the driver, a working set of 18.7 GB in a field
// walk against 14.2 GB with a fresh file (the same build, the same walk). Now the stamp the files were made with is kept beside
// them in mkw_vk_pipelines.stamp, and files of another stamp are dropped before they are read.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace pcache_stamp {

inline const char* kStampFile = "mkw_vk_pipelines.stamp";
inline const char* kMainFile = "mkw_vk_pipelines.bin";
inline const char* kPlayPrefix = "mkw_vk_pipelines_play_";

/// The stamp kept in dir's stamp file; false: none (no file, or not 8 bytes)
inline bool read(const std::string& dir, uint64_t* stamp) {
    FILE* f = fopen((dir + kStampFile).c_str(), "rb");
    if (!f) return false;
    uint64_t v = 0;
    const bool ok = fread(&v, sizeof v, 1, f) == 1 && fgetc(f) == EOF;
    fclose(f);
    if (ok) *stamp = v;
    return ok;
}

/// stamp into dir's stamp file, through a .tmp renamed over it; whether it is in place
inline bool write(const std::string& dir, uint64_t stamp) {
    const std::string path = dir + kStampFile, tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(&stamp, sizeof stamp, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    return ok && !ec;
}

/// Whether name is one of the pipeline cache's files: the main one, or a play file (mkw_vk_pipelines_play_<n>.bin, also .tmp)
inline bool is_cache_file(const std::string& name) {
    if (name == kMainFile || name == std::string(kMainFile) + ".tmp") return true;
    const std::string pre = kPlayPrefix;
    if (name.size() <= pre.size() || name.compare(0, pre.size(), pre)) return false;
    const std::string rest = name.substr(pre.size());
    const size_t dot = rest.find('.');
    if (dot == 0 || dot == std::string::npos || rest.substr(0, dot).find_first_not_of("0123456789") != std::string::npos) return false;
    const std::string ext = rest.substr(dot);
    return ext == ".bin" || ext == ".bin.tmp";
}

struct Dropped { size_t files = 0; uint64_t bytes = 0; };

/// The cache's files in dir go when they were made for another stamp than `stamp` (or for none: a folder from before the stamp
/// file, whose files may hold any older translator's pipelines); the stamp file then says `stamp`. What went
inline Dropped check(const std::string& dir, uint64_t stamp) {
    Dropped d;
    uint64_t kept = 0;
    if (read(dir, &kept) && kept == stamp) return d;
    // (nothing here may throw: it runs at every start. The range-for's ++ throws on an error, and path::string() throws on
    // Windows for a name the code page cannot hold, so the iterator is stepped by increment(ec) and such a name, never one of
    // ours, is passed over)
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name;
        try {
            name = it->path().filename().string();
        } catch (...) {
            continue;
        }
        if (!is_cache_file(name)) continue;
        std::error_code ec2;
        const uint64_t size = it->file_size(ec2);
        const bool sized = !ec2;
        if (std::filesystem::remove(it->path(), ec2)) d.files++, d.bytes += sized ? size : 0;
    }
    write(dir, stamp);
    return d;
}

}   // namespace pcache_stamp
