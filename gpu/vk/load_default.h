// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot) When kept programs are loaded, where RX_VK_LOAD does not say: at their create calls (create) or all before the
// title (start), and the safety net that takes an install back to start. Nothing of Vulkan here: tests/test_load_default.py.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>

namespace load_default {

struct Choice {
    bool create;
    const char* why;
};
/// env: RX_VK_LOAD's value or null. native_mesh: the GPU draws mesh programs itself (without, a start keeps every mesh fallback
/// program ready, and on the one such GPU measured first uses waited seconds for loads at create). marker: an earlier session
/// of this install met the safety net below. compressed: the compressed program cache is in use
inline Choice choose(const char* env, bool native_mesh, bool marker, bool compressed = false) {
    // (the compressed program cache, RX_VK_PROGRAM_CACHE_ZSTD: a record is only read whole, unpacked. The loads at a create call
    // read records by their parts' places and have not been run on it: start, whatever RX_VK_LOAD says)
    if (compressed) return {false, "the program cache is compressed (RX_VK_PROGRAM_CACHE_ZSTD), whose records are only read whole"};
    if (env && !std::strcmp(env, "create")) return {true, "RX_VK_LOAD=create"};
    if (env && !std::strcmp(env, "start")) return {false, "RX_VK_LOAD=start"};
    if (!native_mesh) return {false, "this GPU draws mesh programs through the compute fallback"};
    if (marker) return {false, "in an earlier session first uses waited too long for loads (hookshot_load_start.txt)"};
    return {true, "the default where the GPU has mesh shaders"};
}

/// The safety net: the replay thread's time running or waiting for loads at first uses, in one session. Measured where loads
/// at create work: 10-25 ms in all over a tour, the longest 2-7 ms (RTX 3090, RTX 4070 SUPER); where they do not: 2.5-4.7 s in
/// all, the longest 0.7-1.3 s (RX 580, 16 GB)
constexpr double kWorstMs = 100.0, kTotalMs = 500.0;
inline bool too_slow(double worst_ms, double total_ms) { return worst_ms > kWorstMs || total_ms > kTotalMs; }

/// The other thing loads at create calls must not do: find that the caches lack what a list names. A load asks the caches for
/// its program's noted pipelines and its compute pipelines without compiling; one that is in no cache is compiled when it is
/// first drawn, in play, with the draw left out: that is this mode's doing, and what start mode builds before the title.
/// Measured: 0 in every tour where the caches held the preparation's work (at most 39 on a GPU without mesh shaders); 4,189
/// and 5,384 in the two sessions of a folder whose caches did not answer.
/// Not counted: draws left out and waits for pipelines no list names (a state first seen in play, a new game's opening:
/// 1,906 left out and 353 ms of waits in eight minutes of a healthy session). Those happen the same in start mode, so they
/// say nothing about this one, and a longer session would have taken a healthy install back to start by them
constexpr long long kNotInCacheMost = 500;
inline bool caches_lacked(long long listed_not_in_a_cache) { return listed_not_in_a_cache > kNotInCacheMost; }

/// The marker a session that was too slow leaves for the next start (a preparation removes the file)
inline bool marker_there(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}
inline bool marker_write(const std::string& path, double worst_ms, double total_ms) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fprintf(f, "First uses of shader programs waited for their loads for %.0f ms in one session (the longest "
                                    "%.0f ms), so programs are loaded before the title again. Delete this file, or prepare the "
                                    "shaders again, to have them loaded when the game makes them.\n", total_ms, worst_ms) > 0;
    return std::fclose(f) == 0 && ok;
}
inline bool marker_write_lacked(const std::string& path, long long listed_not_in_a_cache) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fprintf(f, "In one session %lld listed pipelines were in no cache when their programs were loaded, and "
                                    "had to be compiled in play, so programs are loaded before the title again. Delete this "
                                    "file, or prepare the shaders again, to have them loaded when the game makes them.\n",
                                 listed_not_in_a_cache) > 0;
    return std::fclose(f) == 0 && ok;
}

}   // namespace load_default
