// (hookshot) Quick pipelines (RX_VK_QUICK_MS) and the brief pipeline wait (RX_VK_PIPE_WAIT_MS), for a draw whose full
// pipeline build is not done (vk_draw.cpp, draw and quick_pipeline). Ported from Mako, by sharkiix (8292d363): AMD's
// optimizer took 5-13 s a pipeline for Volvagia's intro, whose colour draws were left out (a black silhouette) or, waited
// for, froze it. Only pipelines: a shader still translating is waited for as before
// (https://github.com/NotRexed/Hookshot/pull/1573, RX_VK_TRANSLATE_WAIT_MS).
#pragma once
#include <atomic>
#include <cstdint>

#include "translate_wait.h"   // (the same strict reading of a number of milliseconds)

namespace quick_pipe {

/// RX_VK_QUICK_MS's suggested value (off unless set, on every driver, until a run on AMD measures it): half a frame at 60 fps,
/// the same bound as RX_VK_TRANSLATE_WAIT_MS's tested 8. A skippable draw's quick build only stands in for leaving it out, so
/// its cost has to stay under a frame's: Mako's 1000 traded a missing draw for a hitch of up to a second. Where a quick build
/// takes longer than 8 ms (tens of ms on AMD's proprietary driver), it means one quick build a frame on each recording thread,
/// each a hitch of that build's length: the frame's first always runs (FrameBudget::fits) and nothing after it fits
constexpr double kQuickSuggestedMs = 8;
/// How long a draw that cannot be left out waits for a quick one another thread is making (a few ms as a rule), by the clock:
/// a poll sleeps 1 ms, which is about 15 ms on Windows unless the timer resolution was raised, so a count of polls is no time
constexpr double kOtherQuickWaitMs = 5000;

/// A switch's milliseconds: its value when it reads as a number (0 or less: off), def when unset; a value that is not a
/// number ("8ms", "off", "") is def too, with *bad set so that the caller says so in the log
inline double read_ms(const char* env, double def, bool* bad) {
    if (bad) *bad = false;
    if (!env) return def;
    double ms = 0;
    if (!translate_wait::parse(env, ms)) {
        if (bad) *bad = true;
        return def;
    }
    return ms > 0 ? ms : 0;
}
/// RX_VK_QUICK_MS: each recording thread's quick builds a frame, in ms (unset or 0: none, the draw left out or waited for as
/// before, on every driver)
inline double quick_ms(const char* env, bool* bad) { return read_ms(env, 0, bad); }
/// RX_VK_PIPE_WAIT_MS: each recording thread's waits a frame for skippable draws' pipelines, in ms (off unless set)
inline double wait_ms(const char* env, bool* bad) { return read_ms(env, 0, bad); }
/// RX_VK_TEST_SLOW_MS: a test that holds each full build that long, as AMD's optimizer on a cold cache (off unless set; at
/// most a minute)
inline int slow_ms(const char* env, bool* bad) {
    const double ms = read_ms(env, 0, bad);
    return ms > 60000 ? 60000 : (int)ms;
}

/// What is left of a budget in this frame, on one recording thread (a thread_local each)
struct FrameBudget {
    uint64_t frame = ~0ull;
    double used = 0;
    double build_ms = 0;   // (take's: the thread's last quick build, what the next one is expected to take; 0: none yet)
    int builds = 0;        // (quick builds started this frame on this thread: not a cache answer, nor a busy gate's retry)
    double left(uint64_t now, double total) {
        if (now != frame) frame = now, used = 0, builds = 0;
        return total - used;
    }
    /// take's: whether a quick build for a draw that may be left out is started now. One expected to fit in what is left, or
    /// the frame's first build whatever it is expected to take (so that a build slower than the whole budget is still made,
    /// one a frame): a frame runs past the budget by at most one build, not by every build started before it was spent. The
    /// first is counted by builds started, not by time used: a busy gate's retry or a cache answer early in the frame takes
    /// a little time but builds nothing, and must not keep the frame's one build from the draws after it
    bool fits(uint64_t now, double total) {
        const double l = left(now, total);
        return l > 0 && (builds == 0 || build_ms <= l);
    }
    void started(uint64_t now, double total) {
        left(now, total);
        builds++;
    }
    void spend(double ms) { used += ms; }
};

/// A thread_local flag set for a scope and cleared on leaving it, by a throw too (run_guarded keeps a worker running after its
/// job throws, and the flag would stay set for its next jobs)
struct Raise {
    bool& flag;
    explicit Raise(bool& f) : flag(f) { flag = true; }
    ~Raise() { flag = false; }
    Raise(const Raise&) = delete;
    Raise& operator=(const Raise&) = delete;
};

/// The quick state of an entry: none tried (or tried while the driver-fault gate was busy: tried again), one being made, one
/// tried (made, or one that will not be made: the full build unoptimized already, a program the driver faulted on)
enum : uint8_t { kNone = 0, kMaking = 1, kTried = 2 };
enum class Got { kNothing, kCached, kQuick, kOther };

/// The steps for a draw whose full pipeline is not ready and that has none to draw with (Entry: ready, pipeline and quick,
/// atomics). Once an entry: the full one asked of the caches without compiling (ask), else one built now without the
/// driver's optimizer and not into our pipeline cache (build); either set as the entry's pipeline, not ready, so the worker's
/// full build replaces it. A handle that loses to the full one, which came first, is destroyed (no draw has seen it).
/// must: a draw that cannot be left out: not held to the budget, and waits (poll) for a quick one another thread is making.
/// The time taken comes off the budget either way. kOther: another thread's (or the full build) is there to draw with.
/// ask and build set their bool when the driver-fault gate was busy (on AMD, one build of a program in the driver at a time:
/// often the entry's own full build, queued a moment before): then nothing is built, and the entry is left kNone, to be
/// tried again by a later draw. A retry is one non-blocking ask of the gate, and comes off the budget as any other
template <class Entry, class Ask, class Build, class Destroy, class Now, class Poll>
Got take(Entry& e, bool must, double budget_ms, FrameBudget& budget, uint64_t frame, Ask&& ask, Build&& build, Destroy&& destroy,
         Now&& now_ms, Poll&& poll) {
    using P = decltype(e.pipeline.load());
    const auto there = [&] { return e.pipeline.load(std::memory_order_acquire) != P{}; };
    if (budget_ms <= 0) return Got::kNothing;
    if (!must && !budget.fits(frame, budget_ms)) return there() ? Got::kOther : Got::kNothing;
    uint8_t none = kNone;
    if (!e.quick.compare_exchange_strong(none, kMaking, std::memory_order_acq_rel)) {
        const double from = must ? now_ms() : 0;
        while (must && e.quick.load(std::memory_order_acquire) == kMaking && !there() &&
               !e.ready.load(std::memory_order_acquire) && now_ms() - from < kOtherQuickWaitMs)
            poll();
        return there() ? Got::kOther : Got::kNothing;
    }
    // (the entry's state when this is over: kTried unless set below, so that a throw out of ask or build cannot leave it
    // kMaking, with every draw of it that must wait polling for the whole kOtherQuickWaitMs)
    struct Making {
        decltype(e.quick)& quick;
        uint8_t then = kTried;
        ~Making() { quick.store(then, std::memory_order_release); }
    } making{e.quick};
    const double t0 = now_ms();
    Got got = Got::kCached;
    bool busy = false;
    P pl = ask(busy);
    double t1 = 0;
    if (pl == P{} && !busy) {
        got = Got::kQuick, t1 = now_ms(), pl = build(busy);
        // (a build the gate let in, or one that will never be made: one that met the gate busy built nothing, and counted, it
        // would keep the frame's one build from the draws after it for as long as its program is in the driver)
        if (!busy) budget.started(frame, budget_ms);
    }
    const double t2 = now_ms();
    if (got == Got::kQuick && pl != P{}) budget.build_ms = t2 - t1;   // (one made: a busy gate or a NOOPT one took no time)
    budget.left(frame, budget_ms);
    budget.spend(t2 - t0);
    if (pl != P{}) {
        P empty{};
        if (!e.pipeline.compare_exchange_strong(empty, pl, std::memory_order_acq_rel)) destroy(pl), got = Got::kOther;
    } else {
        got = there() ? Got::kOther : Got::kNothing;
    }
    making.then = pl == P{} && busy ? kNone : kTried;
    return got;
}

/// The brief wait: a skippable draw's queued full build moved to the queue's front (hurry) and waited for (wait, at most
/// the ms it is given) while this frame's budget lasts. The build is not run on this thread: one started cannot be stopped,
/// and on AMD's optimizer it took seconds. true: ready, draw it
template <class Hurry, class Wait, class Now>
bool brief(const std::atomic<bool>& ready, double budget_ms, FrameBudget& budget, uint64_t frame, Hurry&& hurry, Wait&& wait,
           Now&& now_ms) {
    if (ready.load(std::memory_order_acquire)) return true;
    if (budget_ms <= 0) return false;
    const double left = budget.left(frame, budget_ms);
    if (left <= 0) return false;
    const double t0 = now_ms();
    hurry();
    const bool ok = wait(left);
    budget.spend(now_ms() - t0);
    return ok;
}

}  // namespace quick_pipe
