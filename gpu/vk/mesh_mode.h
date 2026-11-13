// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot, #1001, #1971) How the game's task and mesh programs run: 0 Automatic (the GPU's own mesh shaders where it has
// them, the compute fallback where not, and for the task + mesh draws of the GPUs vk_main.cpp names), 1 Native (no fallback
// where there are mesh shaders), 2 Compute (the fallback on every GPU). A launch switch, RX_VK_MESH_SHADERS: the Graphics
// tab's Mesh shaders row is gone, as changing it translated every shader again and dropped the pipeline caches for no gain a
// player could see. Its old line in hookshot_options.ini, mesh_shaders=, no longer chooses. Nothing of Vulkan here:
// tests/test_mesh_mode_switch.py.
#pragma once
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace mesh_rule {

struct Choice {
    int mode;          // 0 Automatic, 1 Native, 2 Compute
    const char* why;   // where it came from, for the log
};

inline bool same_word(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (std::tolower((unsigned char)*a) != *b) return false;
    return !*a && !*b;
}

/// env: RX_VK_MESH_SHADERS's value or null (native or compute; anything else, Automatic). force_fallback:
/// RX_VK_FORCE_MESH_FALLBACK's value or null, a test that is Compute over it when set and not 0, as it was over the row
inline Choice choose(const char* env, const char* force_fallback) {
    if (force_fallback && std::atoi(force_fallback) != 0) return {2, "RX_VK_FORCE_MESH_FALLBACK"};
    if (env && same_word(env, "native")) return {1, "RX_VK_MESH_SHADERS"};
    if (env && same_word(env, "compute")) return {2, "RX_VK_MESH_SHADERS"};
    if (env && *env) return {0, "the default (RX_VK_MESH_SHADERS is neither native nor compute)"};
    return {0, "the default"};
}

/// A line of hookshot_options.ini: the old row's mesh_shaders= with Native (1) or Compute (2), which is no longer
/// read (the log says so once, and the menu's next save drops the line). *value: what it held
inline bool old_ini_choice(const char* line, int* value) {
    if (std::strncmp(line, "mesh_shaders=", 13)) return false;
    *value = std::atoi(line + 13);
    return *value == 1 || *value == 2;   // (what the row read: 0 to 2, anything else Automatic)
}

}  // namespace mesh_rule
