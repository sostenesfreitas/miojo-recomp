// When a retired pool's import goes back to the driver (vk_memory.cpp, retired_imports_give_back).
//
// A pool remade larger leaves the table and its import stays (retire_pool). Off Windows it is given back as soon as the
// GPU is past it: each import is a userptr buffer object to amdgpu, checked page by page at every submission (issues
// 312, 413). On Windows NVIDIA it is too: kept, 6.8 GB of retired imports on an RTX 3070 (8 GB) lost the device (issue
// 1057). On Windows AMD and Intel it was kept for the session, as the device was lost right after a give-back there (an
// Arc B390 entering Kokiri, issue 17; the Ally X, 309; an RX 6600, 571), so those two leaked every retired pool, multi-GB
// in a long session. There, and only there, it now goes back after a wait: retired for wait_ms, or for floor_ms while the
// system heap is past its high watermark. RX_VK_KEEP_RETIRED chooses for every OS and vendor, as before.
#pragma once

#include <cstdint>

namespace retired_give_back {

enum class Mode {
    KEEP,      // (for the session)
    AT_ONCE,   // (as soon as the GPU is past it)
    GRACE,     // (once the GPU is past it and it has been retired for a while)
};

/// set: RX_VK_KEEP_RETIRED has a value; on: one env_on reads as on (1: kept), not 0, off, false or no (given back at once).
/// Unset, the default of the OS and the vendor: the wait where retired pools were kept, at once everywhere else.
constexpr Mode mode(bool set, bool on, bool windows, bool nvidia) {
    if (set) return on ? Mode::KEEP : Mode::AT_ONCE;
    return windows && !nvidia ? Mode::GRACE : Mode::AT_ONCE;
}

/// Whether a retired pool the GPU is past goes back now. idle_ms: how long it has been retired; pressure: the system
/// heap is past its high watermark and the bytes wanted back are not queued yet.
constexpr bool due(Mode m, uint64_t idle_ms, uint64_t wait_ms, uint64_t floor_ms, bool pressure) {
    if (m == Mode::KEEP) return false;
    if (m == Mode::AT_ONCE) return true;
    return idle_ms >= wait_ms || (pressure && idle_ms >= floor_ms);
}

/// What the driver still counts of the heap's use less what is queued to be freed once the GPU is past it (pending):
/// a give-back does not lower the heap's use until then, and a second valve reading it would free for the same bytes.
constexpr uint64_t settled(uint64_t used, uint64_t pending) { return used > pending ? used - pending : 0; }

/// The bytes to give back: none up to high percent of the budget (0: off), past it down to low percent.
constexpr uint64_t want(uint64_t used, uint64_t budget, int high, int low) {
    if (high <= 0 || !budget || used <= budget / 100 * (uint64_t)high) return 0;
    const uint64_t target = budget / 100 * (uint64_t)low;
    return used > target ? used - target : 0;
}

/// The system-heap budget imports_give_back measures against:
/// - Under 12 GB (16 GB PCs and the Steam Deck), the heap's own budget as before (#479), unless RX_VK_IMPORT_CAP_MB > 0.
/// - At 12 GB and above (32 GB+ PCs, where imports_give_back was off), capped at kLiveBudgetCap (4500 MB) by default so
///   idle unmirrored pools do not pile up in amdgpu's per-submit BO list and capture ranges over a multi-hour session.
/// - RX_VK_IMPORT_CAP_MB=<MB> sets the cap in MB (> 0: capped on any budget; 0: off at >= 12 GB unless a watermark switch
///   is set). Setting RX_VK_IMPORT_HIGH or RX_VK_IMPORT_LOW without RX_VK_IMPORT_CAP_MB uses the full system-heap budget.
constexpr uint64_t kSmallBudget = 12ull << 30;
constexpr uint64_t kLiveBudgetCap = 4500ull << 20;
constexpr uint64_t live_budget(uint64_t budget, bool env_pct_set, int env_cap_mb = -1) {
    if (env_cap_mb > 0) {
        const uint64_t cap = (uint64_t)env_cap_mb << 20;
        return budget < cap ? budget : cap;
    }
    if (env_pct_set || budget < kSmallBudget) return budget;
    if (env_cap_mb == 0) return 0;
    return budget < kLiveBudgetCap ? budget : kLiveBudgetCap;
}

} // namespace retired_give_back
