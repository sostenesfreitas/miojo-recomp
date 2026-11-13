// The worker threads (vk_jobs.h). As many as a third of the hardware threads (2 to 4): the game itself runs six
// virtual cores, and the replay thread is one of the busiest.
#include <algorithm>
#include <atomic>
#include <exception>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "vk_jobs.h"
#include "vk_internal.h"
#include "vk_draw.h"   // (ub_thread_off)

namespace mkwvk {
namespace {

std::mutex mu;
std::condition_variable work_cv, done_cv;
struct Entry {
    std::function<void()> fn;
    const std::atomic<bool>* flag;   // (what the job sets when done, for jobs_wait to find it while queued)
};
std::deque<Entry> queue;
std::atomic<int> pending{0};
bool started = false;

// (a job that throws: the game ended at once ("approach Malon, crash": an exception escaped a worker, KERNELBASE's RaiseException
// with no handler, 2026-11-08). Resource jobs publish failure before the exception is logged, so their waiters finish.)
void run_guarded(std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        static std::atomic<int> logged;
        if (logged++ < 20) logf("vk: a background job failed: %s", e.what());
    } catch (...) {
        static std::atomic<int> logged;
        if (logged++ < 20) logf("vk: a background job failed (not a std::exception)");
    }
}

// Wrap before queueing: shared workers, the serial worker and jobs_wait's stolen jobs all run the same recovery.
// The resource owns its failure representation (failed program, null pipeline, completed compute state).
std::function<void()> with_failure(std::function<void()> fn, std::function<void()> on_failure) {
    if (!on_failure) return fn;
    return [fn = std::move(fn), on_failure = std::move(on_failure)] {
        try {
            fn();
        } catch (...) {
            on_failure();
            throw;   // run_guarded still logs the original exception
        }
    };
}

void worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"mkw_vk worker");
    ub_thread_off();
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lk(mu);
            work_cv.wait(lk, [] { return !queue.empty(); });
            fn = std::move(queue.front().fn);
            queue.pop_front();
        }
        run_guarded(fn);
        pending--;
        jobs_notify();
    }
}

void start() {
    // (hookshot: with 16 hardware threads or more, 6 workers; RX_VK_WORKERS=<n>: n, 1 to 16)
    const unsigned hw = std::thread::hardware_concurrency();
    unsigned n = hw >= 16 ? 6u : std::clamp(hw / 3, 2u, 4u);
    if (const char* e = getenv("RX_VK_WORKERS")) n = (unsigned)std::clamp(atoi(e), 1, 16);
    for (unsigned i = 0; i < n; i++) std::thread(worker).detach();
    started = true;
    logf("vk: %u background workers (shader translation, pipelines)", n);
}

} // namespace

void jobs_push(std::function<void()> fn, bool urgent, const std::atomic<bool>* flag, std::function<void()> on_failure) {
    fn = with_failure(std::move(fn), std::move(on_failure));
    std::lock_guard lk(mu);
    if (!started) start();
    pending++;
    if (urgent) queue.push_front({std::move(fn), flag});
    else queue.push_back({std::move(fn), flag});
    work_cv.notify_one();
}
// (issue 496) Intel A750: four of the game's big compute pipelines built at once by Intel's compiler (up to 58 s each) froze
// the whole PC. One thread builds them one at a time there, below normal priority, beside the shared workers
void jobs_push_serial(std::function<void()> fn, std::function<void()> on_failure) {
    fn = with_failure(std::move(fn), std::move(on_failure));
    static std::mutex smu;
    static std::condition_variable scv;
    static std::deque<std::function<void()>> sq;
    static bool sstarted = false;
    std::lock_guard lk(smu);
    if (!sstarted) {
        sstarted = true;
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SetThreadDescription(GetCurrentThread(), L"mkw_vk serial builds");
            ub_thread_off();
            for (;;) {
                std::function<void()> fn;
                {
                    std::unique_lock lk(smu);
                    scv.wait(lk, [] { return !sq.empty(); });
                    fn = std::move(sq.front());
                    sq.pop_front();
                }
                run_guarded(fn);
                jobs_notify();
            }
        }).detach();
        logf("vk: compute pipelines are built one at a time");
    }
    sq.push_back(std::move(fn));
    scv.notify_one();
}
void jobs_notify() {
    std::lock_guard lk(mu);
    done_cv.notify_all();
}
void jobs_wait(const std::atomic<bool>& flag) {
    std::unique_lock lk(mu);
    // (still queued: run here, now. A draw that must wait for its pipeline waited for every job queued before it, and a new
    // area queues a hundred pipelines: with ten workers at 0.3-2 s a build, frames stopped for 6-12 s, the draw's own build
    // still in the queue. RX_VK_JOB_STEAL=0: waited for in turn, as before)
    static const bool steal = !getenv("RX_VK_JOB_STEAL") || atoi(getenv("RX_VK_JOB_STEAL")) != 0;
    if (steal && !flag.load(std::memory_order_acquire))
        for (auto it = queue.begin(); it != queue.end(); ++it)
            if (it->flag == &flag) {
                std::function<void()> fn = std::move(it->fn);
                queue.erase(it);
                lk.unlock();
                run_guarded(fn);
                pending--;
                jobs_notify();
                lk.lock();
                break;
            }
    done_cv.wait(lk, [&] { return flag.load(std::memory_order_acquire); });
}
bool jobs_wait_for(const std::atomic<bool>& flag, double ms) {
    if (flag.load(std::memory_order_acquire)) return true;
    if (ms <= 0) return false;
    std::unique_lock lk(mu);
    return done_cv.wait_for(lk, std::chrono::duration<double, std::milli>(ms),
                            [&] { return flag.load(std::memory_order_acquire); });
}
int jobs_pending() {
    return pending.load();
}

} // namespace mkwvk
