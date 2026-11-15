// (hookshot) How many workers the shader preparation's first part runs on (vk_draw.cpp prepare_run: every program translated,
// and the driver's compile of each compute program's variants, for its cache).
//
// Half the CPUs (half_cpus), as before, unless the process heap is the segment heap (nx2/win_manifest.py's heapType). The
// driver's compiler allocates and frees from every worker at once, through the process heap: on the NT heap the workers queue
// on its one lock (sampled on AMD's Windows driver 26.10.1: amdvlk64.dll -> RtlAllocateHeap / RtlFreeHeap ->
// RtlEnterCriticalSection, 76% of the workers' samples waiting), so 12 workers on a 6-core, 12-thread Ryzen 5 5600 kept 3.75
// CPUs busy and were slower than 6. On the segment heap 12 workers kept 11 busy, and the part took 904 s against 1,466 s
// with 6 on the NT heap (RX 6700 XT, a cold prepare). So with the segment heap every usable CPU gets a worker, as far as
// the free memory allows: each worker more held 0.4-0.5 GB more at the peak (6 more: 8.9 -> 11.3 GB private on one build,
// 9.0 -> 12.1 GB on another), and a worker is added only for each kPerWorkerMB free past kReserveMB. Without it (Linux, a
// Windows before 10 2004 that ignores the manifest, a build without it) the count is half_cpus, as before.
#pragma once

#include <algorithm>
#include <cstdint>

namespace prepare_workers {

constexpr uint64_t kReserveMB = 4096;    // (physical memory left to the rest of the PC)
constexpr uint64_t kPerWorkerMB = 768;   // (a worker past half the CPUs, each: measured 400-520 MB at the peak, with room)
constexpr unsigned kMaxWorkers = 32;

/// half: half_cpus(); usable: usable_cpus(); avail_mb: physical memory free now (MB); segment: the process heap is the segment
/// heap. At least 1, at most kMaxWorkers
inline unsigned count(unsigned half, unsigned usable, uint64_t avail_mb, bool segment) {
    const unsigned base = std::clamp(half, 1u, kMaxWorkers);
    if (!segment || usable <= base) return base;
    const uint64_t room = avail_mb > kReserveMB ? (avail_mb - kReserveMB) / kPerWorkerMB : 0;
    const uint64_t extra = std::min<uint64_t>(room, (uint64_t)(usable - base));
    return (unsigned)std::min<uint64_t>(base + extra, kMaxWorkers);
}

/// The process heap's header (GetProcessHeap()): the segment heap's has 0xDDEEDDEE at byte 16, the NT heap's 0xFFEEFFEE (Windows'
/// own layouts, not a documented interface: checked on Windows 11 26200 with and without the manifest's heapType; anything
/// else reads as not the segment heap, which is the old worker count)
inline bool segment_heap_signature(const void* heap) {
    if (!heap) return false;
    uint32_t sig = 0;
    const unsigned char* h = static_cast<const unsigned char*>(heap);
    for (int i = 0; i < 4; i++) sig |= (uint32_t)h[16 + i] << (8 * i);
    return sig == 0xDDEEDDEEu;
}

}  // namespace prepare_workers
