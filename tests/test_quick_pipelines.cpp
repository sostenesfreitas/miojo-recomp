// Quick pipelines (gpu/vk/quick_pipe.h, vk_draw.cpp quick_pipeline): the decision without a GPU or the game. quick_pipe.h's
// rules with fake handles, then vk_draw.cpp's own quick_budget_ms, pipe_wait_budget_ms and quick_pipeline, cut out by
// tests/test_quick_pipelines.py into quick_cut.inc and built against the stand-ins below.
// argv[1]: "amd" or "nvidia" (the device's driver); argv[2]: what RX_VK_QUICK_MS is expected to come to.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "quick_pipe.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}
}  // namespace

// ---- stand-ins for what the cut code uses ----
using VkPipeline = uintptr_t;
constexpr VkPipeline VK_NULL_HANDLE = 0;
using VkPrimitiveTopology = int;
enum VkDriverId { VK_DRIVER_ID_AMD_PROPRIETARY = 1, VK_DRIVER_ID_NVIDIA_PROPRIETARY = 4 };
struct {
    VkDriverId driver_id = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    struct { bool pipelineCreationCacheControl = true; } vk13;
    void* device = nullptr;
} dev;
struct Program { uint64_t key = 0x1234; };
struct NvnState {};
struct Targets {};
std::vector<std::string> logs;
void logf(const char* f, ...) {
    char line[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(line, sizeof line, f, a);
    va_end(a);
    logs.push_back(line);
}
uint64_t frame_no = 1;
uint64_t texture_frame_no() { return frame_no; }
thread_local bool ask_start_first = false;
thread_local bool quick_build = false;
thread_local bool gate_busy = false;
// (build_graphics: what each call was, and what it answers: the caches' full one, or a quick one; gate_held: the driver-fault
// gate busy, another build of the program in the driver, as build_graphics answers it then)
struct Call { bool asked, quick, start_first; };
std::vector<Call> calls;
bool cache_has = false, gate_held = false, throws = false;
VkPipeline next_handle = 100;
VkPipeline build_graphics(const Program&, const NvnState&, const Targets&, VkPrimitiveTopology, int part = -1, bool* cold = nullptr) {
    calls.push_back({cold != nullptr, quick_build, ask_start_first});
    check(part == -1, "a whole pipeline is asked for");
    if (throws) throw 996;
    if (gate_held) return gate_busy = true, cold ? (*cold = true, VK_NULL_HANDLE) : VK_NULL_HANDLE;
    if (cold) return cache_has ? next_handle++ : (*cold = true, VK_NULL_HANDLE);
    return next_handle++;
}
std::vector<VkPipeline> destroyed;
void vkDestroyPipeline(void*, VkPipeline pl, void*) { destroyed.push_back(pl); }

#include "quick_cut.inc"   // (PipeEntry, quick_budget_ms, pipe_wait_budget_ms, quick_pipeline: vk_draw.cpp's own)

namespace {

struct FakeEntry {
    std::atomic<bool> ready{false};
    std::atomic<int> pipeline{0};
    std::atomic<uint8_t> quick{quick_pipe::kNone};
};

void header_rules() {
    bool bad = false;
    check(quick_pipe::quick_ms(nullptr, &bad) == 0 && !bad, "unset: off on every driver (until measured on AMD)");
    check(quick_pipe::quick_ms("250", &bad) == 250 && !bad, "a number sets it anywhere");
    check(quick_pipe::quick_ms("8", &bad) == quick_pipe::kQuickSuggestedMs && !bad, "the suggested value");
    check(quick_pipe::kQuickSuggestedMs > 0 && quick_pipe::kQuickSuggestedMs <= 16, "the suggested budget is under a 60 fps frame");
    check(quick_pipe::quick_ms(" 0 ", &bad) == 0 && !bad, "0 turns it off");
    check(quick_pipe::quick_ms("-5", &bad) == 0 && !bad, "a negative number is off");
    for (const char* v : {"", "8ms", "off", "nan", "inf", "1e999"}) {
        check(quick_pipe::quick_ms(v, &bad) == 0 && bad, "a non-number is off, said");
        check(quick_pipe::wait_ms(v, &bad) == 0 && bad, "a non-number wait is off, said");
    }
    check(quick_pipe::wait_ms(nullptr, &bad) == 0 && !bad, "the brief wait is off unless set");
    check(quick_pipe::wait_ms("100", &bad) == 100 && !bad, "the brief wait's budget");
    check(quick_pipe::slow_ms("8000", &bad) == 8000 && !bad, "the slow-build test");
    check(quick_pipe::slow_ms("999999", &bad) == 60000, "the slow-build test is at most a minute");
    check(quick_pipe::slow_ms("8s", &bad) == 0 && bad, "the slow-build test is strict");

    const auto none = [](bool&) { return 0; };
    int built = 0, asked = 0;
    std::vector<int> gone;
    double clock = 0;
    const auto ask_none = [&](bool&) { return asked++, 0; };
    const auto build_slow = [&](bool&) { return clock += 400, ++built, 50 + built; };   // (each quick build: 400 ms)
    const auto destroy = [&](int pl) { gone.push_back(pl); };
    const auto now = [&] { return clock; };
    const auto poll = [&] { clock += 1; };
    quick_pipe::FrameBudget budget;
    // (budget per frame: 1000 ms, 400 ms a build. Two skippable draws get one; the third would end at 1200 ms, past the budget,
    // so it is left out and not started: the frame ends at 800 ms, not 1200)
    FakeEntry e[6];
    int got = 0;
    for (int i = 0; i < 3; i++)
        got += quick_pipe::take(e[i], false, 1000, budget, 7, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kQuick;
    check(got == 2 && built == 2 && budget.used == 800, "a frame's quick builds stop before one that would overrun its budget");
    check(e[2].quick.load() == quick_pipe::kNone, "an entry left out for the budget can be given one in a later frame");
    check(quick_pipe::take(e[4], true, 1000, budget, 7, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kQuick,
          "a draw that cannot be left out is not held to the budget");
    check(quick_pipe::take(e[2], false, 1000, budget, 8, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kQuick,
          "the next frame has its budget again");
    check(quick_pipe::take(e[5], false, 0, budget, 9, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kNothing,
          "a budget of 0 is off");
    {   // (builds slower than the whole budget: the frame's first is still made, then none more in it: one build past it at most)
        quick_pipe::FrameBudget small;
        FakeEntry s[3];
        built = 0;
        check(quick_pipe::take(s[0], false, 8, small, 1, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kQuick,
              "a frame's first quick build is made whatever it is expected to take");
        check(quick_pipe::take(s[1], false, 8, small, 1, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kNothing &&
                  built == 1,
              "no second build in a frame its first overran");
        check(quick_pipe::take(s[1], false, 8, small, 2, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kQuick &&
                  built == 2,
              "the next frame makes one again");
        // (a busy gate's retry early in a frame takes a little time and builds nothing: it must not use up the frame's one
        // build, or an entry whose program is in the driver for seconds, retried at the same place each frame, keeps every
        // skippable draw after it from a quick pipeline for that long)
        FakeEntry r[2];
        const auto ask_busy = [&](bool& busy) { clock += 1; busy = true; return 0; };
        check(quick_pipe::take(r[0], false, 8, small, 3, ask_busy, build_slow, destroy, now, poll) == quick_pipe::Got::kNothing &&
                  r[0].quick.load() == quick_pipe::kNone,
              "a busy gate's retry builds nothing and leaves the entry to try again");
        check(quick_pipe::take(r[1], false, 8, small, 3, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kQuick &&
                  built == 3,
              "a frame's first build still comes after a busy retry took some of its time");
        // (the same where the gate is met at the quick build itself: no pipelineCreationCacheControl, so no ask, or the gate
        // taken between the ask and the build. small's last build took 400 ms, more than the whole budget of 8)
        FakeEntry q[2];
        const auto build_busy_slow = [&](bool& busy) { clock += 1; busy = true; return 0; };
        check(quick_pipe::take(q[0], false, 8, small, 4, ask_none, build_busy_slow, destroy, now, poll) ==
                      quick_pipe::Got::kNothing &&
                  q[0].quick.load() == quick_pipe::kNone && small.used > 0 && small.build_ms > 8,
              "a build that met the gate busy builds nothing and leaves the entry to try again");
        check(quick_pipe::take(q[1], false, 8, small, 4, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kQuick &&
                  built == 4,
              "a frame's first build still comes after a build that met the gate busy took some of its time");
    }
    {   // (a throw out of the ask or the build, which run_guarded survives on a worker: the entry is not left kMaking)
        quick_pipe::FrameBudget tb;
        FakeEntry x[2];
        const auto ask_throws = [&](bool&) -> int { throw 1; };
        const auto build_throws = [&](bool&) -> int { throw 2; };
        bool thrown = false;
        try { quick_pipe::take(x[0], false, 8, tb, 1, ask_throws, build_slow, destroy, now, poll); } catch (int) { thrown = true; }
        check(thrown && x[0].quick.load() == quick_pipe::kTried, "a throw at the cache ask leaves the entry tried, not kMaking");
        thrown = false;
        try { quick_pipe::take(x[1], true, 8, tb, 1, ask_none, build_throws, destroy, now, poll) ; } catch (int) { thrown = true; }
        check(thrown && x[1].quick.load() == quick_pipe::kTried, "a throw at the quick build leaves the entry tried, not kMaking");
        bool flag = false;
        try { const quick_pipe::Raise r(flag); check(flag, "Raise sets its flag"); throw 3; } catch (int) {}
        check(!flag, "Raise clears its flag on a throw");
    }
    // (cache first: a full one the caches have is taken, and nothing is built)
    FakeEntry c;
    const int before = built;
    check(quick_pipe::take(c, false, 1000, budget, 10, [](bool&) { return 9; }, build_slow, destroy, now, poll) ==
                  quick_pipe::Got::kCached &&
              c.pipeline.load() == 9 && built == before,
          "the caches' full pipeline first, without a quick build");
    // (once an entry: a second draw neither asks nor builds again)
    asked = 0;
    check(quick_pipe::take(c, false, 1000, budget, 10, ask_none, build_slow, destroy, now, poll) == quick_pipe::Got::kOther &&
              asked == 0 && built == before,
          "an entry is given one once");
    // (the full one came first: kept, the quick one destroyed, no draw has seen it)
    FakeEntry f;
    const auto build_racing = [&](bool&) { f.pipeline.store(1); f.ready.store(true); return 77; };
    check(quick_pipe::take(f, false, 1000, budget, 11, ask_none, build_racing, destroy, now, poll) == quick_pipe::Got::kOther &&
              f.pipeline.load() == 1 && gone == std::vector<int>{77},
          "a quick one that loses to the full build is destroyed, the full one kept");
    // (the full build replaces the quick one: the worker's store is unconditional; vk_draw.cpp is checked to do so)
    FakeEntry r;
    quick_pipe::take(r, false, 1000, budget, 12, ask_none, build_slow, destroy, now, poll);
    r.pipeline.store(1), r.ready.store(true);
    check(r.pipeline.load() == 1, "the full build's pipeline replaces the quick one");
    // (another thread making one: a draw that may be left out does not wait; one that may not waits for it, and not when
    // the other attempt is over without one)
    FakeEntry m;
    m.quick.store(quick_pipe::kMaking);
    double t0 = clock;
    check(quick_pipe::take(m, false, 1000, budget, 13, none, none, destroy, now, poll) == quick_pipe::Got::kNothing && clock == t0,
          "a skippable draw does not wait for another thread's quick one");
    int polls = 0;
    const auto poll_then_made = [&] { if (++polls == 3) m.pipeline.store(5); };
    check(quick_pipe::take(m, true, 1000, budget, 13, none, none, destroy, now, poll_then_made) == quick_pipe::Got::kOther &&
              polls == 3,
          "a draw that must wait takes another thread's quick one once made");
    FakeEntry t;
    t.quick.store(quick_pipe::kTried);
    polls = 0;
    check(quick_pipe::take(t, true, 1000, budget, 13, none, none, destroy, now, [&] { polls++; }) == quick_pipe::Got::kNothing &&
              polls == 0,
          "an attempt that is over without one is not waited for");
    {   // (another thread's attempt that never ends: a draw that must wait gives up after kOtherQuickWaitMs by the clock, however
        // long a poll sleeps: about 15 ms on Windows, where 5000 polls of "1 ms" would be some 78 s)
        FakeEntry h;
        h.quick.store(quick_pipe::kMaking);
        for (const double step : {1.0, 15.6}) {
            polls = 0;
            const double from = clock;
            check(quick_pipe::take(h, true, 1000, budget, 14, none, none, destroy, now, [&] { polls++, clock += step; }) ==
                          quick_pipe::Got::kNothing &&
                      clock - from >= quick_pipe::kOtherQuickWaitMs && clock - from < quick_pipe::kOtherQuickWaitMs + step + 1 &&
                      polls == (int)((quick_pipe::kOtherQuickWaitMs + step - 1) / step),
                  "a draw that must wait polls another thread's attempt for kOtherQuickWaitMs, by the clock");
        }
    }

    // (the driver-fault gate busy (AMD: the entry's own full build in the driver): nothing, and the entry left to be tried
    // again; neither a quick build after a busy cache ask, nor a wait. Once the gate is free a later draw gets one)
    {
        FakeEntry g;
        int asks = 0, builds = 0;
        const auto ask_busy = [&](bool& busy) { return asks++, busy = true, 0; };
        const auto build_busy = [&](bool& busy) { return builds++, busy = true, 0; };
        const auto build_ok = [&](bool&) { return builds++, 42; };
        quick_pipe::FrameBudget gb;
        check(quick_pipe::take(g, false, 8, gb, 1, ask_busy, build_ok, destroy, now, poll) == quick_pipe::Got::kNothing &&
                  asks == 1 && builds == 0 && g.quick.load() == quick_pipe::kNone,
              "a busy gate at the cache ask: no quick build, the entry tried again later");
        check(quick_pipe::take(g, true, 8, gb, 1, ask_none, build_busy, destroy, now, poll) == quick_pipe::Got::kNothing &&
                  builds == 1 && g.quick.load() == quick_pipe::kNone,
              "a busy gate at the quick build: the entry tried again later, a must draw goes on to wait for the full build");
        check(quick_pipe::take(g, false, 8, gb, 1, ask_none, build_ok, destroy, now, poll) == quick_pipe::Got::kQuick &&
                  g.pipeline.load() == 42 && g.quick.load() == quick_pipe::kTried,
              "once the gate is free, the entry gets its quick pipeline");
        FakeEntry n;   // (one that will not be made, the gate free: tried, not asked again)
        const auto build_never = [&](bool&) { return builds++, 0; };
        check(quick_pipe::take(n, false, 8, gb, 2, ask_none, build_never, destroy, now, poll) == quick_pipe::Got::kNothing &&
                  n.quick.load() == quick_pipe::kTried,
              "a quick pipeline that will not be made is tried once");
    }

    // (the brief wait: hurried, waited at most what is left of the frame's budget, the time taken off it)
    std::atomic<bool> ready{false};
    quick_pipe::FrameBudget wb;
    int hurried = 0;
    double asked_ms = -1;
    const auto wait_none = [&](double ms) { asked_ms = ms, clock += ms; return false; };
    check(!quick_pipe::brief(ready, 0, wb, 1, [&] { hurried++; }, wait_none, now) && hurried == 0, "the brief wait is off at 0");
    check(!quick_pipe::brief(ready, 100, wb, 1, [&] { hurried++; }, wait_none, now) && hurried == 1 && asked_ms == 100,
          "the brief wait hurries the build and waits for the frame's budget");
    asked_ms = -1;
    check(!quick_pipe::brief(ready, 100, wb, 1, [&] { hurried++; }, wait_none, now) && hurried == 1 && asked_ms == -1,
          "a frame's brief waits stop once its budget is spent");
    check(quick_pipe::brief(ready, 100, wb, 2, [&] { hurried++; }, [&](double) { clock += 3; return true; }, now) && hurried == 2,
          "the next frame waits again, and a build done in time is drawn");
    check(wb.used == 3, "only the time waited comes off the budget");
}

// vk_draw.cpp's own: the caches asked first without compiling and without the quick flag; a quick build only after, with it,
// and the flag cleared after; nothing asked where the driver cannot be asked without compiling
void cut_code(double want_ms) {
    PipeEntry e;
    Program p;
    NvnState s;
    Targets at;
    check(quick_budget_ms() == want_ms, "RX_VK_QUICK_MS comes to what it should on this driver");
    if (want_ms <= 0) {
        check(!quick_pipeline(e, p, s, at, 0, true) && calls.empty(), "off: nothing asked, nothing built");
        return;
    }
    cache_has = true;
    check(quick_pipeline(e, p, s, at, 0, false) && e.pipeline.load() == 100 && !e.ready.load(), "from the caches, not ready");
    check(calls.size() == 1 && calls[0].asked && !calls[0].quick && calls[0].start_first, "asked of the caches without compiling");
    check(!ask_start_first && !quick_build, "the thread's flags are left as they were");
    PipeEntry q;
    cache_has = false, calls.clear();
    check(quick_pipeline(q, p, s, at, 0, false) && q.pipeline.load() != 0 && !q.ready.load(), "a quick one, not ready");
    check(calls.size() == 2 && calls[0].asked && !calls[0].quick && !calls[1].asked && calls[1].quick && !calls[1].start_first,
          "the caches first, then a quick build (no optimizer, no cache: build_graphics's quick_build)");
    check(!quick_build, "quick_build is cleared after the quick build");
    bool logged = false;
    for (const std::string& l : logs) logged |= l.find("quick pipeline (no optimizer)") != std::string::npos;
    check(logged, "a quick build is logged");
    PipeEntry n;
    dev.vk13.pipelineCreationCacheControl = false, calls.clear();
    check(quick_pipeline(n, p, s, at, 0, false) && calls.size() == 1 && calls[0].quick,
          "without pipelineCreationCacheControl the caches are not asked (the flag needs it)");
    dev.vk13.pipelineCreationCacheControl = true;
    // (the driver-fault gate busy: build_graphics says so; the entry is not marked tried, and a later draw gets one)
    PipeEntry b;
    gate_held = true, calls.clear();
    check(!quick_pipeline(b, p, s, at, 0, true) && b.quick.load() == quick_pipe::kNone && calls.size() == 1 && calls[0].asked,
          "a busy gate at the cache ask: nothing, no quick build, the entry tried again later");
    dev.vk13.pipelineCreationCacheControl = false, calls.clear();
    check(!quick_pipeline(b, p, s, at, 0, true) && b.quick.load() == quick_pipe::kNone && calls.size() == 1 && calls[0].quick,
          "a busy gate at the quick build: nothing, the entry tried again later");
    dev.vk13.pipelineCreationCacheControl = true;
    gate_held = false, calls.clear();
    check(quick_pipeline(b, p, s, at, 0, true) && b.pipeline.load() != 0 && b.quick.load() == quick_pipe::kTried,
          "the gate free again: the entry gets one");
    // (a throw out of build_graphics: run_guarded keeps the thread, and its flags must not stay set for its next builds)
    PipeEntry t;
    throws = true, calls.clear();
    bool thrown = false;
    try { quick_pipeline(t, p, s, at, 0, true); } catch (int) { thrown = true; }
    check(thrown && !ask_start_first && !quick_build && t.quick.load() == quick_pipe::kTried,
          "a throw at the cache ask: ask_start_first cleared, the entry not left kMaking");
    PipeEntry u;
    dev.vk13.pipelineCreationCacheControl = false, thrown = false;
    try { quick_pipeline(u, p, s, at, 0, true); } catch (int) { thrown = true; }
    check(thrown && !quick_build && u.quick.load() == quick_pipe::kTried,
          "a throw at the quick build: quick_build cleared, the entry not left kMaking");
    dev.vk13.pipelineCreationCacheControl = true, throws = false;
    check(pipe_wait_budget_ms() == quick_pipe::wait_ms(std::getenv("RX_VK_PIPE_WAIT_MS"), nullptr), "RX_VK_PIPE_WAIT_MS as read");
}

}  // namespace

int main(int argc, char** argv) {
    dev.driver_id = argc > 1 && !std::strcmp(argv[1], "amd") ? VK_DRIVER_ID_AMD_PROPRIETARY : VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    const double want = argc > 2 ? std::atof(argv[2]) : 0;
    header_rules();
    cut_code(want);
    if (failures) return EXIT_FAILURE;
    std::printf("PASS (%s, RX_VK_QUICK_MS %s, budget %.0f ms): quick pipelines' decision\n", argc > 1 ? argv[1] : "nvidia",
                std::getenv("RX_VK_QUICK_MS") ? std::getenv("RX_VK_QUICK_MS") : "unset", want);
    return EXIT_SUCCESS;
}
