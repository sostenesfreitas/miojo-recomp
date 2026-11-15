// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot, #1001, #1971) How the game's task and mesh programs run: 0 Automatic (the GPU's own mesh shaders where it has
// them, the compute fallback where not, and for the task + mesh draws of the GPUs task_mesh_fallback_default names on
// Windows), 1 Native (no fallback where there are mesh shaders), 2 Compute (the fallback on every GPU). A launch switch,
// RX_VK_MESH_SHADERS: the Graphics tab's Mesh shaders row is gone, as changing it translated every shader again and
// dropped the pipeline caches for no gain a player could see. Its old line in hookshot_options.ini, mesh_shaders=, no
// longer chooses. Nothing of Vulkan here: tests/test_mesh_mode_switch.py, tests/test_task_mesh_fallback_devices.py.
#pragma once
#include <cctype>
#include <cstdint>
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

/// (hookshot, https://github.com/NotRexed/Hookshot/issues/996) The GPUs whose task + mesh draws take the compute fallback by
/// default; vk_main.cpp asks only on Windows, where AMD's driver timed out on the native task + mesh path of RDNA 2. Navi 22
/// first (an RX 6700 XT), then Navi 21: on an RX 6800 XT (driver 0x800161) the native path lost the device within minutes at
/// Hyrule Castle's entrance, while the fallback played 25 minutes there with none. Navi 23: an RX 6600 (driver 0x800161)
/// lost the device on the session's first task + mesh draws (reported in Mako by sharkiix, no log published); another RX 6600
/// on the same driver played 15-20 minutes on the native path and lost the device only at the driver's VRAM limit
/// (https://git.notrexed.dev/Hookshot/Hookshot/issues/130); the fallback costs VRAM of its own (fallback_buffers_of,
/// vk_draw.cpp). Navi 24 (0x743f: RX 6400, 6500 XT; 0x7421 to 0x7424: RX 6300, Pro W6300 to W6500M) is not on by default: no
/// report, 4 GB cards. RX_VK_TASK_MESH_FALLBACK=1 to try.
/// Which IDs a chip brings: all its IDs that the PCI ID database (pci-ids.ucw.cz) or libdrm's amdgpu.ids name a card for,
/// consumer, mobile and Pro alike, as they share the chip and the driver's mesh path; not its USB or audio functions, nor
/// IDs neither list names a card for (Navi 22's 0x73c3, its SR-IOV function 0x73ce). Untested ones go in with their chip
inline bool task_mesh_fallback_default(uint32_t vendor, uint32_t device) {
    if (vendor != 0x1002) return false;
    switch (device) {
    case 0x73a1:   // Navi 21: Radeon Pro V620
    case 0x73a2:   // Navi 21: Radeon Pro W6900X
    case 0x73a3:   // Navi 21: Radeon Pro W6800
    case 0x73ab:   // Navi 21: Radeon Pro W6800X, W6800X Duo
    case 0x73ae:   // Navi 21: Radeon Pro V620 MxGPU
    case 0x73bf:   // Navi 21: RX 6800, 6800 XT, 6900 XT
    case 0x73af:   // Navi 21: RX 6900 XT (XTXH boards)
    case 0x73a5:   // Navi 21: RX 6950 XT
    case 0x73df:   // Navi 22: RX 6700, 6700 XT, 6750 XT, 6750 GRE 12GB, 6700M, 6800M, 6850M XT
    case 0x73e0:   // Navi 23: RX 6600M
    case 0x73e1:   // Navi 23: Radeon Pro W6600M
    case 0x73e3:   // Navi 23: Radeon Pro W6600
    case 0x73ef:   // Navi 23: RX 6650 XT, 6650M, 6650M XT, 6700S, 6800S
    case 0x73ff:   // Navi 23: RX 6600, 6600 XT, 6600 LE, 6600M, 6600S
        return true;
    default:
        return false;
    }
}

}  // namespace mesh_rule
