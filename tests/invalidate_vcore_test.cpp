// The production dispatch and away/back functions are included unchanged. Only the core grant and renderer are synthetic.
#define _Static_assert static_assert
#include "gpu/vk/vk_api.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace std::chrono_literals;

struct HosThread { int vc_held = 0, vc_escaped = 0, releases = 0, takes = 0; bool returning_caller = false; };
static thread_local HosThread* cur;
static thread_local bool callback_active;
static std::mutex lease_mu, renderer_mu;
static std::condition_variable lease_cv, progress_cv;
static HosThread* owner;
static bool enabled = true, producer_queued, producer_done;
static bool require_no_lease = true, need_producer;
static int vulkan_calls, fallback_calls;
static uint64_t want_gpu = 0x123456789abULL, want_size = 0x100002345ULL;
static const MkwVkRenderer* vk;

static int hos_vcores_on() { return enabled; }
static void vc_release(HosThread* t) {
    std::lock_guard<std::mutex> lock(lease_mu);
    CHECK(owner == t && t->vc_held == 1);
    owner = nullptr;
    t->vc_held = 0;
    t->releases++;
    lease_cv.notify_all();
}
static void vc_acquire(HosThread* t) {
    // Returning to guest execution must happen after the renderer's lock has unwound.
    if (t->returning_caller) {
        CHECK(!callback_active);
        CHECK(renderer_mu.try_lock());
        renderer_mu.unlock();
    }
    std::unique_lock<std::mutex> lock(lease_mu);
    CHECK(lease_cv.wait_for(lock, 5s, [] { return owner == nullptr; }));
    owner = t;
    t->vc_held = 1;
    t->takes++;
}
static void renderer(uint64_t gpu, uint64_t size) {
    std::lock_guard<std::mutex> render_lock(renderer_mu);
    callback_active = true;
    CHECK(gpu == want_gpu && size == want_size);
    if (require_no_lease) CHECK(!cur || !cur->vc_held);
    if (need_producer) {
        std::unique_lock<std::mutex> lock(lease_mu);
        // This is a liveness bound, not a performance assertion. The producer needs the caller's one allowed core.
        CHECK(progress_cv.wait_for(lock, 2s, [] { return producer_done; }));
    }
    callback_active = false;
}
static void vulkan_invalidate(uint64_t gpu, uint64_t size) { vulkan_calls++; renderer(gpu, size); }
static void wgr_nvn_invalidate(uint64_t gpu, uint64_t size) { fallback_calls++; renderer(gpu, size); }

#include "actual_invalidate.inc"

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const char* name = argv[1];
    const bool fallback = !std::strcmp(name, "fallback") || !std::strcmp(name, "fallback-immediate");
    const bool already_away = !std::strcmp(name, "already-away");
    const bool non_guest = !std::strcmp(name, "non-guest");
    enabled = std::strcmp(name, "disabled") != 0;
    need_producer = !std::strcmp(name, "vulkan") || !std::strcmp(name, "fallback") || !std::strcmp(name, "producer");
    // A separate case demonstrates producer starvation on the original wrapper, beyond the entry invariant.
    require_no_lease = std::strcmp(name, "producer") != 0;
    if (!std::strcmp(name, "zero")) want_gpu = want_size = 0;
    MkwVkRenderer backend{};
    backend.nvn_invalidate = vulkan_invalidate;
    vk = fallback ? nullptr : &backend;
    HosThread caller, producer;
    caller.returning_caller = true;
    cur = non_guest ? nullptr : &caller;
    if (cur && enabled) owner = &caller, caller.vc_held = 1;
    int outer_away = 0;
    if (already_away) outer_away = hos_vcore_away();
    const int before_release = caller.releases, before_take = caller.takes;

    std::thread worker;
    if (need_producer) {
        worker = std::thread([&] {
            cur = &producer;
            {
                std::lock_guard<std::mutex> lock(lease_mu);
                producer_queued = true;
                progress_cv.notify_all();
            }
            vc_acquire(&producer);
            {
                std::lock_guard<std::mutex> lock(lease_mu);
                producer_done = true;
                progress_cv.notify_all();
            }
            vc_release(&producer);
        });
        std::unique_lock<std::mutex> lock(lease_mu);
        CHECK(progress_cv.wait_for(lock, 2s, [] { return producer_queued; }));
    }
    rx_nvn_invalidate(want_gpu, want_size);
    if (worker.joinable()) worker.join();
    CHECK(vulkan_calls == (fallback ? 0 : 1));
    CHECK(fallback_calls == (fallback ? 1 : 0));
    const bool owned_on_entry = cur && enabled && !already_away;
    CHECK(caller.releases - before_release == (owned_on_entry ? 1 : 0));
    CHECK(caller.takes - before_take == (owned_on_entry ? 1 : 0));
    CHECK(caller.vc_held == (owned_on_entry ? 1 : 0));
    CHECK(owner == (owned_on_entry ? &caller : nullptr));
    if (need_producer) CHECK(producer_done && producer.takes == 1 && producer.releases == 1 && !producer.vc_held);
    if (outer_away) {
        hos_vcore_back();
        CHECK(caller.vc_held == 1 && caller.releases == 1 && caller.takes == 1);
    }
    std::printf("PASS: %s actual invalidate wrapper\n", name);
}
