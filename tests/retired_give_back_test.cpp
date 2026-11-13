// retired_give_back (gpu/vk/retired_give_back.h): when a retired pool's import goes back to the driver. Off Windows and on
// Windows NVIDIA as soon as the GPU is past it, as before; on Windows AMD and Intel, where they were kept for the session
// and leaked, after a wait that the system heap's pressure shortens. No game, no GPU.
#include "../gpu/vk/retired_give_back.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

using retired_give_back::Mode;
using retired_give_back::due;
using retired_give_back::kLiveBudgetCap;
using retired_give_back::live_budget;
using retired_give_back::mode;
using retired_give_back::settled;
using retired_give_back::want;

constexpr uint64_t kMB = 1ull << 20, kGB = 1ull << 30;
constexpr uint64_t kWait = 30000, kFloor = 2000;   // (vk_memory.cpp's defaults: RX_VK_RETIRE_SECS, RX_VK_RETIRE_MIN_SECS)
constexpr int kHigh = 85, kLow = 75;               // (RX_VK_IMPORT_HIGH, RX_VK_IMPORT_LOW)

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

// (the defaults: main's everywhere but where it kept them)
static_assert(mode(false, false, false, false) == Mode::AT_ONCE, "Linux AMD and Intel: at once (issues 312, 413)");
static_assert(mode(false, false, false, true) == Mode::AT_ONCE, "Linux NVIDIA: at once");
static_assert(mode(false, false, true, true) == Mode::AT_ONCE, "Windows NVIDIA: at once (issue 1057)");
static_assert(mode(false, false, true, false) == Mode::GRACE, "Windows AMD and Intel: after the wait");
// (RX_VK_KEEP_RETIRED set: the same on every OS and vendor)
static_assert(mode(true, true, false, false) == Mode::KEEP && mode(true, true, true, false) == Mode::KEEP &&
              mode(true, true, true, true) == Mode::KEEP, "=1 keeps them for the session");
static_assert(mode(true, false, false, false) == Mode::AT_ONCE && mode(true, false, true, false) == Mode::AT_ONCE &&
              mode(true, false, true, true) == Mode::AT_ONCE, "=0, off, false: at once");

// (a pool the GPU is past)
static_assert(!due(Mode::KEEP, 1ull << 40, kWait, kFloor, true), "kept whatever its age and the pressure");
static_assert(due(Mode::AT_ONCE, 0, kWait, kFloor, false), "at once: no wait at all, the switches not looked at");
static_assert(!due(Mode::GRACE, 0, kWait, kFloor, false) && !due(Mode::GRACE, kWait - 1, kWait, kFloor, false),
              "the wait: not before it is over");
static_assert(due(Mode::GRACE, kWait, kWait, kFloor, false), "the wait: over");
static_assert(!due(Mode::GRACE, kFloor - 1, kWait, kFloor, true), "under pressure: still not before the short wait");
static_assert(due(Mode::GRACE, kFloor, kWait, kFloor, true), "under pressure: the short wait");
static_assert(due(Mode::GRACE, 0, kWait, 0, true), "RX_VK_RETIRE_MIN_SECS=0: under pressure, at once");

// (the watermarks)
static_assert(want(6 * kGB, 8 * kGB, kHigh, kLow) == 0, "75%: under the high watermark");
static_assert(want(8 * kGB / 100 * 85, 8 * kGB, kHigh, kLow) == 0, "at the high watermark, not past it");
static_assert(want(7 * kGB, 8 * kGB, kHigh, kLow) == 7 * kGB - 8 * kGB / 100 * 75, "past it: down to the low watermark");
static_assert(want(8 * kGB, 8 * kGB, 0, kLow) == 0, "RX_VK_IMPORT_HIGH=0: off");
static_assert(want(8 * kGB, 0, kHigh, kLow) == 0, "budget 0 (RX_VK_IMPORT_CAP_MB=0): off");
static_assert(settled(7 * kGB, kGB) == 6 * kGB && settled(kGB, 2 * kGB) == 0, "what is queued is as good as freed");
static_assert(live_budget(7503 * kMB, false) == 7503 * kMB, "under 12 GB (16 GB PC / Deck): the heap's own budget, unchanged");
static_assert(live_budget(15434 * kMB, false) == kLiveBudgetCap, "32 GB PC, default: capped at 4500 MB (high ~3825 MB, low ~3375 MB)");
static_assert(live_budget(15434 * kMB, true) == 15434 * kMB, "RX_VK_IMPORT_HIGH or RX_VK_IMPORT_LOW set: the heap's full budget");
static_assert(live_budget(15434 * kMB, false, 0) == 0, "RX_VK_IMPORT_CAP_MB=0 on >= 12 GB: off, as on main");
static_assert(live_budget(7503 * kMB, false, 0) == 7503 * kMB, "RX_VK_IMPORT_CAP_MB=0 under 12 GB: keeps the < 12 GB behaviour");
static_assert(live_budget(15434 * kMB, false, 6000) == 6000 * kMB, "RX_VK_IMPORT_CAP_MB=6000: tunable cap in MB");
static_assert(want(3878 * kMB, live_budget(15434 * kMB, false), kHigh, kLow) == 3878 * kMB - 3375 * kMB,
              "3878 MB used on a 15434 MB heap: past 85% of the 4500 MB cap, gives back down to 75% (3375 MB)");

struct Retired { uint64_t bytes; int64_t at_ms; bool imported = true; };

/// One pass of retired_imports_give_back's loop over pools the GPU is past: the bytes queued
uint64_t pass(std::vector<Retired>& pools, Mode m, int64_t now_ms, uint64_t wanted) {
    uint64_t queued = 0;
    for (Retired& p : pools) {   // (in the order they were retired)
        if (!p.imported) continue;
        const uint64_t idle = (uint64_t)std::max<int64_t>(0, now_ms - p.at_ms);
        if (!due(m, idle, kWait, kFloor, wanted > 0)) continue;
        p.imported = false, queued += p.bytes;
        wanted -= std::min(wanted, p.bytes);
    }
    return queued;
}
uint64_t imported_bytes(const std::vector<Retired>& pools) {
    uint64_t n = 0;
    for (const Retired& p : pools) n += p.imported ? p.bytes : 0;
    return n;
}

/// The leak: 20 areas, one 340 MB pool retired in each, 10 s apart, a pass at each (issue 1057's tour: 6.8 GB kept)
uint64_t tour(Mode m, uint64_t* peak) {
    std::vector<Retired> pools;
    *peak = 0;
    for (int area = 0; area < 20; area++) {
        const int64_t now = area * 10000;
        pools.push_back({340 * kMB, now});
        pass(pools, m, now, 0);
        *peak = std::max(*peak, imported_bytes(pools));
    }
    return imported_bytes(pools);
}

} // namespace

int main() {
    uint64_t peak = 0;
    expect(tour(Mode::KEEP, &peak) == 20 * 340 * kMB, "kept: every retired pool of the tour still imported (6.8 GB)");
    expect(tour(Mode::AT_ONCE, &peak) == 0 && peak == 0, "at once: none left at any pass");
    // (30 s at 10 s an area: the pool of this area and the two before it are still waiting)
    expect(tour(Mode::GRACE, &peak) == 3 * 340 * kMB && peak == 3 * 340 * kMB, "the wait: only the last 30 s of pools imported");

    {   // (pressure: an 8 GB budget, 7 GB used, so about 1 GB over the low watermark: two pools and part of a third)
        std::vector<Retired> pools{{400 * kMB, 0}, {400 * kMB, 1000}, {400 * kMB, 2000}, {400 * kMB, 9000}, {400 * kMB, 9500}};
        const int64_t now = 10000;
        const uint64_t used = 7 * kGB, budget = 8 * kGB;
        const uint64_t wanted = want(used, budget, kHigh, kLow);
        expect(wanted > 800 * kMB && wanted <= 1200 * kMB, "about 1 GB wanted back: more than two pools, no more than three");
        expect(pass(pools, Mode::GRACE, now, 0) == 0, "no pressure: none of them is 30 s old");
        const uint64_t queued = pass(pools, Mode::GRACE, now, wanted);
        expect(queued == 1200 * kMB, "pressure: the three oldest, and no more once the low watermark's worth is queued");
        expect(pools[3].imported && pools[4].imported, "the two retired under 2 s ago stay, pressure or not");
        // (the driver's figure is the same until the GPU is past them: neither valve asks for those bytes again)
        expect(want(settled(used, queued), budget, kHigh, kLow) == 0, "the next probe counts what is queued as given back");
        expect(want(used, budget, kHigh, kLow) == wanted, "(and without that it would ask for all of it a second time)");
        expect(pass(pools, Mode::AT_ONCE, now, 0) == 800 * kMB, "at once: the rest, whatever their age");
    }
    {   // (kept: pressure frees nothing)
        std::vector<Retired> pools{{400 * kMB, 0}};
        expect(pass(pools, Mode::KEEP, 1 << 30, kGB) == 0, "kept under pressure too");
    }
    if (failures) return std::printf("%d failed\n", failures), 1;
    std::printf("retired_give_back: ok\n");
    return 0;
}
