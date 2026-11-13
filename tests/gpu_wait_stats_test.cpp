// SPDX-License-Identifier: GPL-3.0-or-later
// Real boundary macros and accounting helpers, with no Vulkan library or GPU.
#include "gpu_wait_stats.h"
#include <cstdio>
#include <thread>

namespace gs = mkwvk::gpu_wait_stats;
static int failures, checks, boundaries, calls;
static bool excluded;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures, std::printf("FAIL: %s\n", what);
}
static int64_t total(const gs::Snapshot& s) { int64_t n = 0; for (int64_t v : s) n += v; return n; }
struct FakeClock {
    static inline thread_local int64_t ticks = 0;
    static inline thread_local int reads = 0;
    static int64_t now() { ++reads; return ticks; }
};
static void reset() {
    gs::counters.take();
    check(gs::depth == 0, "a completed scope leaves no nesting state");
    FakeClock::ticks = 0;
    FakeClock::reads = 0;
}
static void driver_work(bool timed) {
    ++calls;
    check(gs::depth == (timed || excluded ? 1u : 0u), "driver call is timed, except a poll or pacing exclusion");
    volatile unsigned work = 0;
    for (unsigned i = 0; i < 50000; i++) work = work + i;
    (void)work;
}
using Device = uintptr_t;
using Fence = uint64_t;
struct WaitInfo { uint64_t value; };
constexpr unsigned VK_QUERY_RESULT_WAIT_BIT = 2;
namespace mkwvk::rec {
enum Bound { B_vkWaitForFences, B_vkWaitSemaphores, B_vkQueueWaitIdle, B_vkDeviceWaitIdle, B_vkGetQueryPoolResults };
inline void boundary(Bound) {
    ++boundaries;
    check(gs::depth == (excluded ? 1u : 0u), "recorder boundary precedes the GPU-wait timer");
}
}
namespace vkx {
static int vkWaitForFences(Device d, unsigned n, const Fence* f, unsigned all, uint64_t timeout) {
    check(d == 1 && n == 1 && *f == 77 && all == 1, "fence arguments forwarded unchanged");
    driver_work(timeout != 0);
    return -7; // Error results still return unchanged and their call time is recorded.
}
static int vkWaitSemaphores(Device d, const WaitInfo* w, uint64_t timeout) {
    check(d == 1 && w->value == 23, "timeline arguments forwarded unchanged");
    driver_work(timeout != 0);
    return 2; // A timeout is not a successful wait, but its elapsed time is real.
}
static int vkQueueWaitIdle(Device d) { check(d == 4, "queue argument forwarded unchanged"); driver_work(true); return 4; }
static int vkDeviceWaitIdle(Device d) { check(d == 5, "device argument forwarded unchanged"); driver_work(true); return 5; }
static int vkGetQueryPoolResults(Device d, unsigned pool, unsigned first, unsigned count, size_t size, uint64_t* data,
                                 uint64_t stride, unsigned flags) {
    check(d == 1 && pool == 8 && first == 2 && count == 1 && size == 8 && stride == 8, "query arguments forwarded unchanged");
    driver_work((flags & VK_QUERY_RESULT_WAIT_BIT) != 0);
    *data = 123;
    return -9;
}
}
#include "wait_macros.h"

int main() {
    reset();
    const int result = gs::timed<FakeClock>(gs::kTimeline, true, [] { FakeClock::ticks += 100; return 42; });
    auto s = gs::counters.take();
    check(result == 42 && s[gs::kTimeline] == 100 && total(s) == 100, "one timeline interval counted exactly");
    check(FakeClock::reads == 2 && total(gs::counters.take()) == 0, "two clock reads; snapshots consume no time twice");

    reset();
    gs::timed<FakeClock>(gs::kFence, true, [] {
        FakeClock::ticks += 5;
        gs::timed<FakeClock>(gs::kTimeline, true, [] { FakeClock::ticks += 11; });
        FakeClock::ticks += 7;
    });
    s = gs::counters.take();
    check(s[gs::kFence] == 23 && s[gs::kTimeline] == 0 && FakeClock::reads == 2, "nested waits are one outer interval");

    reset();
    int invoked = 0;
    gs::timed<FakeClock>(gs::kFence, false, [&] { ++invoked; FakeClock::ticks += 13; });
    check(invoked == 1 && FakeClock::reads == 0 && total(gs::counters.take()) == 0, "poll executes once without a clock or count");
    {
        const gs::Exclude pacing;
        gs::timed<FakeClock>(gs::kTimeline, true, [] { FakeClock::ticks += 200; });
    }
    check(gs::depth == 0 && FakeClock::reads == 0 && total(gs::counters.take()) == 0, "pacing is excluded and scope restores state");

    reset();
    std::array<std::thread, 4> workers;
    for (auto& t : workers) t = std::thread([] {
        for (int i = 0; i < 1000; i++)
            gs::timed<FakeClock>(gs::kTimeline, true, [] { FakeClock::ticks += 10; });
    });
    for (auto& t : workers) t.join();
    s = gs::counters.take();
    check(s[gs::kTimeline] == 40000 && total(s) == 40000, "simultaneous threads accumulate without lost updates");

    reset();
    const Fence fence = 77;
    const WaitInfo wait{23};
    int evaluated = 0;
    check(vkWaitForFences(Device(1), unsigned(++evaluated), &fence, 1u, uint64_t(123)) == -7,
          "fence error result preserved");
    s = gs::counters.take();
    check(evaluated == 1 && s[gs::kFence] > 0 && s[gs::kTimeline] == 0 && s[gs::kIdle] == 0,
          "real fence macro feeds only the fence bucket, arguments evaluated once");
    check(vkWaitSemaphores(Device(1), &wait, uint64_t(456)) == 2, "timeline timeout result preserved");
    s = gs::counters.take();
    check(s[gs::kTimeline] > 0 && s[gs::kFence] == 0 && s[gs::kIdle] == 0,
          "real timeline macro feeds the capture/readback wait bucket");
    check(vkQueueWaitIdle(Device(4)) == 4 && vkDeviceWaitIdle(Device(5)) == 5, "idle results preserved");
    s = gs::counters.take();
    check(s[gs::kIdle] > 0 && s[gs::kFence] == 0 && s[gs::kTimeline] == 0, "both idle macros feed the idle bucket");
    vkWaitForFences(Device(1), 1u, &fence, 1u, uint64_t(0));
    vkWaitSemaphores(Device(1), &wait, uint64_t(0));
    check(total(gs::counters.take()) == 0, "real zero-timeout macros are polls, not waits");
    uint64_t query = 0;
    check(vkGetQueryPoolResults(Device(1), 8u, 2u, 1u, size_t(8), &query, uint64_t(8), VK_QUERY_RESULT_WAIT_BIT) == -9 &&
          query == 123, "query result and output preserved");
    s = gs::counters.take();
    check(s[gs::kQuery] > 0 && total(s) == s[gs::kQuery], "waiting query result feeds only the query bucket");
    vkGetQueryPoolResults(Device(1), 8u, 2u, 1u, size_t(8), &query, uint64_t(8), 0u);
    check(total(gs::counters.take()) == 0, "nonwaiting timestamp queries are not GPU waits");
    excluded = true;
    {
        const gs::Exclude pacing;
        vkWaitSemaphores(Device(1), &wait, uint64_t(100000000));
    }
    excluded = false;
    check(total(gs::counters.take()) == 0 && gs::depth == 0, "real Reflex-shaped timeline call is excluded");
    check(calls == 9 && boundaries == 9, "every native call and recorder boundary occurs exactly once");
    std::printf("GPU wait accounting: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
