// jobs_wait (gpu/vk/vk_jobs.cpp): a waited-for job still in the queue runs on the waiting thread at once, instead of after
// every job queued before it, and only once. Built against stand-ins for the renderer's headers
// (tests/test_jobs_wait_runs_queued.py). argv[1]: "steal" (the default), "race", or "off" (run with RX_VK_JOB_STEAL off).
#include "vk_jobs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace mkwvk;

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

// (every worker held in a job until released, so that a job pushed after them stays queued)
std::mutex gate_mu;
std::condition_variable gate_cv;
bool gate_open = false;
std::atomic<int> held{0};
void hold() {
    held++;
    std::unique_lock lk(gate_mu);
    gate_cv.wait(lk, [] { return gate_open; });
}
void release() {
    {
        std::lock_guard lk(gate_mu);
        gate_open = true;
    }
    gate_cv.notify_all();
}

constexpr unsigned kHolds = 32;   // (more than start() makes: every worker is held, and some holds stay queued)
std::vector<std::atomic<bool>> holds_done(kHolds);
void hold_every_worker(int workers_at_least = 2) {
    for (unsigned i = 0; i < kHolds; i++)
        jobs_push([i] { hold(); holds_done[i].store(true); jobs_notify(); }, false, &holds_done[i]);
    const auto t0 = std::chrono::steady_clock::now();
    while (held.load() < workers_at_least && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::yield();
    expect(held.load() >= workers_at_least, "the workers took the holding jobs");
}
bool drained() {
    const auto t0 = std::chrono::steady_clock::now();
    while (jobs_pending() != 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return jobs_pending() == 0;
}

// The draw's pipeline build, queued behind every busy worker, runs on the waiting thread, once
void steal() {
    hold_every_worker();
    std::atomic<bool> ready{false};
    std::atomic<int> runs{0};
    std::thread::id ran_on{};
    jobs_push([&] { runs++; ran_on = std::this_thread::get_id(); ready.store(true, std::memory_order_release); }, false, &ready);
    const bool here = jobs_wait(ready);   // (every worker is held: without the steal, this waited until release())
    expect(ready.load(), "the waited-for job ran");
    expect(ran_on == std::this_thread::get_id(), "it ran on the waiting thread");
    expect(here, "jobs_wait says it ran the job here");

    std::atomic<bool> unflagged{false};
    jobs_push([&] { unflagged.store(true, std::memory_order_release); jobs_notify(); });   // (no flag: left in the queue)
    release();
    expect(!jobs_wait(unflagged), "a job pushed without its flag is not run by the waiter");
    expect(unflagged.load(), "a job pushed without its flag still runs on a worker");
    for (unsigned i = 0; i < kHolds; i++) jobs_wait(holds_done[i]);
    expect(drained(), "nothing is left in the queue");
    expect(runs.load() == 1, "the stolen job ran once: the queue kept no copy of it for a worker");
}

// Free workers and waiters racing for the same job: whoever takes it, it runs once and no copy stays queued
void race() {
    constexpr int kRounds = 2000;
    int twice = 0, never = 0, here = 0;
    for (int r = 0; r < kRounds; r++) {
        std::atomic<bool> ready{false};
        std::atomic<int> runs{0};
        jobs_push([&] { runs++; ready.store(true, std::memory_order_release); jobs_notify(); }, false, &ready);
        std::atomic<int> ran_here{0};
        std::thread other([&] { ran_here += jobs_wait(ready); });   // (a second recording thread waiting for the same one)
        ran_here += jobs_wait(ready);
        other.join();
        twice += runs.load() > 1, never += runs.load() == 0, here += ran_here.load();
        if (ran_here.load() > 1) twice++;
    }
    expect(twice == 0, "a waited-for job never runs twice");
    expect(never == 0, "a waited-for job always runs before jobs_wait returns");
    expect(drained(), "no taken job is left in the queue");
    std::printf("(race: %d of %d jobs ran on a waiting thread, the rest on a worker)\n", here, kRounds);
}

// RX_VK_JOB_STEAL off: the job waits for a worker, as before the steal
void off() {
    hold_every_worker();
    std::atomic<bool> ready{false};
    std::thread::id ran_on{};
    jobs_push([&] { ran_on = std::this_thread::get_id(); ready.store(true, std::memory_order_release); jobs_notify(); }, false,
              &ready);
    std::thread releaser([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release();
    });
    const bool here = jobs_wait(ready);
    releaser.join();
    expect(ready.load(), "the job ran once the workers were released");
    expect(!here && ran_on != std::this_thread::get_id(), "with the steal off, it ran on a worker");
    for (unsigned i = 0; i < kHolds; i++) jobs_wait(holds_done[i]);
}

// (run with RX_VK_WORKERS=1) jobs_hurry: a queued job moved to the front runs next, on the worker, before what was queued
// ahead of it; one not queued is not found
void hurry() {
    hold_every_worker(1);
    std::atomic<int> order{0};
    std::atomic<bool> a_done{false}, b_done{false};
    std::atomic<int> a_at{-1}, b_at{-1};
    std::atomic<int> b_runs{0};
    std::thread::id b_on{};
    jobs_push([&] { a_at = order++; a_done.store(true); jobs_notify(); }, false, &a_done);
    jobs_push([&] { b_runs++; b_on = std::this_thread::get_id(); b_at = order++; b_done.store(true); jobs_notify(); }, false, &b_done);
    expect(jobs_hurry(b_done), "the queued job is found");
    std::atomic<bool> never{false};
    expect(!jobs_hurry(never), "a flag with no queued job is not found");
    release();
    expect(jobs_wait_for(b_done, 5000), "the hurried job ran");
    jobs_wait(a_done);
    expect(b_at.load() < a_at.load(), "the hurried job ran before the one queued ahead of it");
    expect(b_on != std::this_thread::get_id() && b_runs.load() == 1, "it ran once, on the worker");
    expect(!jobs_hurry(b_done), "a done job is not hurried");
    for (unsigned i = 0; i < kHolds; i++) jobs_wait(holds_done[i]);
    expect(drained(), "nothing is left in the queue");
}

} // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "steal";
    if (!std::strcmp(mode, "race")) race();
    else if (!std::strcmp(mode, "off")) off();
    else if (!std::strcmp(mode, "hurry")) hurry();
    else steal();

    if (failures) return EXIT_FAILURE;
    std::printf("PASS (%s): a waited-for job still queued runs once, on the waiting thread unless the steal is off\n", mode);
    std::fflush(stdout);
    std::_Exit(EXIT_SUCCESS);   // (the workers are detached and wait forever)
}
