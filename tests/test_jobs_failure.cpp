// Exercise the real job queues without a GPU or game, including a waiting draw stealing its job.
#include "vk_jobs.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

using namespace mkwvk;
using Fn = std::function<void()>;
using namespace std::chrono_literals;

// Keep the regression executable against the old API too: it must fail because recovery never ran.
template<class Push>
void push_shared(Push push, Fn fn, const std::atomic<bool>* flag, Fn failed) {
    if constexpr (std::is_invocable_v<Push, Fn, bool, const std::atomic<bool>*, Fn>)
        push(std::move(fn), false, flag, std::move(failed));
    else push(std::move(fn), false, flag);
}
template<class Push>
void push_serial(Push push, Fn fn, Fn failed) {
    if constexpr (std::is_invocable_v<Push, Fn, Fn, bool>) push(std::move(fn), std::move(failed), false);
    else if constexpr (std::is_invocable_v<Push, Fn, Fn>) push(std::move(fn), std::move(failed));
    else push(std::move(fn));
}
template<class Predicate>
bool until(Predicate pred) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!pred() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    return pred();
}
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::fflush(stderr); std::_Exit(1); }
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "worker";
    const bool serial = mode == "serial";
    const bool steal = mode == "steal";
    const bool nonstd = argc > 2;
    std::atomic<bool> release{false};
    std::atomic<int> held{0};
    if (steal) {
        for (int i = 0; i < 32; ++i) jobs_push([&] {
            ++held;
            while (!release.load()) std::this_thread::sleep_for(1ms);
        });
        check(until([&] { return held.load() >= 2; }), "workers started");
    }
    std::atomic<bool> started{false}, ready{false}, waiting_done{false};
    int failure_calls = 0;
    bool failed = false; // published by the release store to ready
    Fn body = [&] {
        started.store(true);
        if (!steal) while (!release.load()) std::this_thread::sleep_for(1ms);
        if (nonstd) throw 42;
        throw std::runtime_error("intentional job failure");
    };
    Fn recovery = [&]() noexcept {
        ++failure_calls;
        failed = true;
        ready.store(true, std::memory_order_release);
    };
    if (serial) push_serial(&jobs_push_serial, body, recovery);
    else push_shared(&jobs_push, body, &ready, recovery);
    if (!steal) check(until([&] { return started.load(); }), "job taken by worker");
    std::thread waiter([&] { jobs_wait(ready); waiting_done.store(true); });
    if (!steal) release.store(true);
    check(until([&] { return waiting_done.load(); }), "exception must complete the resource and release jobs_wait");
    waiter.join();
    check(ready.load(std::memory_order_acquire) && failed && failure_calls == 1,
          "failure is published exactly once before readiness");
    release.store(true);

    std::atomic<bool> next_done{false};
    Fn success = [&] { next_done.store(true, std::memory_order_release); };
    Fn unexpected = [&]() noexcept { std::_Exit(2); };
    if (serial) push_serial(&jobs_push_serial, success, unexpected);
    else push_shared(&jobs_push, success, &next_done, unexpected);
    check(until([&] { return next_done.load(std::memory_order_acquire); }), "the queue runs the next job");
    jobs_wait(next_done);
    check(until([] { return jobs_pending() == 0; }), "pending shared jobs drained");
    std::printf("PASS: %s, %s exception, waiter released and next job completed\n", mode.c_str(), nonstd ? "non-standard" : "standard");
    std::fflush(stdout);
    std::_Exit(0); // worker threads intentionally live for the process lifetime
}
