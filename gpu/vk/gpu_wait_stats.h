// Host wall time inside explicit GPU resource waits, not GPU utilization or a frame's critical path.
// The recorder's drain happens before these helpers. Concurrent threads add their own elapsed time;
// nested waits count once, zero-timeout polls and explicitly excluded pacing waits count nothing.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace mkwvk::gpu_wait_stats {

enum Kind { kFence, kTimeline, kIdle, kQuery, kCount };
using Snapshot = std::array<int64_t, kCount>;

struct Counters {
    std::atomic<int64_t> ns[kCount]{};
    Snapshot take() {
        Snapshot out{};
        for (int i = 0; i < kCount; i++) out[i] = ns[i].exchange(0, std::memory_order_relaxed);
        return out;
    }
};
inline Counters counters;
inline thread_local unsigned depth = 0;

struct Clock {
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

// Reflex's latency semaphore is pacing, not completion of submitted GPU work.
struct Exclude {
    Exclude() { ++depth; }
    ~Exclude() { --depth; }
    Exclude(const Exclude&) = delete;
    Exclude& operator=(const Exclude&) = delete;
};

template<class Time = Clock> struct Scope {
    Kind kind;
    bool outer;
    int64_t start;
    explicit Scope(Kind k) : kind(k), outer(depth++ == 0), start(outer ? Time::now() : 0) {}
    ~Scope() {
        if (outer) {
            const int64_t elapsed = Time::now() - start;
            if (elapsed > 0) counters.ns[kind].fetch_add(elapsed, std::memory_order_relaxed);
        }
        --depth;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

template<class Time = Clock, class F, class... Args>
inline auto timed(Kind kind, bool wait, F call, Args... args) -> decltype(call(args...)) {
    if (!wait || depth) return call(args...);
    const Scope<Time> scope(kind);
    return call(args...);
}

template<class F, class Device, class Count, class Fence, class All, class Timeout>
inline auto fences(F call, Device device, Count count, const Fence* handles, All all, Timeout timeout)
    -> decltype(call(device, count, handles, all, timeout)) {
    return timed(kFence, timeout != 0, call, device, count, handles, all, timeout);
}

template<class F, class Device, class Info, class Timeout>
inline auto timeline(F call, Device device, const Info* info, Timeout timeout) -> decltype(call(device, info, timeout)) {
    return timed(kTimeline, timeout != 0, call, device, info, timeout);
}

template<class F, class Handle> inline auto idle(F call, Handle handle) -> decltype(call(handle)) {
    return timed(kIdle, true, call, handle);
}

template<class F, class Mask, class Device, class Pool, class First, class Count, class Size, class Data, class Stride, class Flags>
inline auto queries(F call, Mask wait_bit, Device device, Pool pool, First first, Count count, Size size, Data* data,
                    Stride stride, Flags flags) -> decltype(call(device, pool, first, count, size, data, stride, flags)) {
    return timed(kQuery, (flags & wait_bit) != 0, call, device, pool, first, count, size, data, stride, flags);
}

} // namespace mkwvk::gpu_wait_stats
