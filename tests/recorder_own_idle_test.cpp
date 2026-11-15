// RX_VK_RECORDER=thread's two opt-in switches against a fake Vulkan (tests/test_recorder_own_idle.py builds it with the real
// vk_work.h and vk_recorder.h): RX_VK_RECORDER_OWN (a recording slot's own resets at its reuse without the wait for the recorder)
// and RX_VK_RECORDER_IDLE=wait (the recorder's idle waits on its ring counter). Deterministic: the recorder thread is held in a
// gated packet instead of timed, and a watchdog (3 s) ends a wait that should not happen, as a failure.
//
//   recorder_own_idle_test <case>    exits 0 when the case holds, 1 (with a FAIL line) when it does not
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

#include "vk_work.h"

namespace mkwvk {
std::atomic<uint64_t> work_epoch{0};
std::atomic<bool> par_active{false};
std::atomic<int> logs_own{0}, logs_idle{0};
void logf(const char* fmt, ...) {   // (the renderer's log: printed, and the lines this test looks for counted)
    char b[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::printf("LOG: %s\n", b);
    if (std::strstr(b, "reused before the recorder ran its last end")) logs_own++;
    if (std::strstr(b, "no WaitOnAddress here")) logs_idle++;
}
} // namespace mkwvk
namespace rec = mkwvk::rec;

// ---- the fake driver: every call a no-op, but for those below
template <class F>
struct Fake;
template <class R, class... A>
struct Fake<R(VKAPI_PTR*)(A...)> {
    static R VKAPI_CALL call(A...) {
        if constexpr (!std::is_void_v<R>) return R{};
    }
};
namespace vkx {
#define RX_FAKE(f) PFN_##f f = Fake<PFN_##f>::call;
RX_REC_CALLS(RX_FAKE, RX_FAKE)
RX_FAKE(vkBeginCommandBuffer) RX_FAKE(vkEndCommandBuffer) RX_FAKE(vkResetCommandPool) RX_FAKE(vkQueueSubmit2) RX_FAKE(vkWaitForFences)
RX_FAKE(vkGetQueryPoolResults) RX_FAKE(vkDestroyBuffer) RX_FAKE(vkDestroyDevice) RX_FAKE(vkGetSemaphoreCounterValue)
RX_FAKE(vkWaitSemaphores) RX_FAKE(vkGetQueueCheckpointDataNV) RX_FAKE(vkGetDeviceFaultInfoEXT) RX_FAKE(vkResetDescriptorPool)
RX_FAKE(vkDestroyDescriptorPool)
#undef RX_FAKE
} // namespace vkx

static std::mutex g_mu;
static std::condition_variable g_cv;
static bool g_gate_open = true;      // (the timestamp packet runs only once the gate is open)
static std::atomic<int> g_ran{0};    // (timestamp packets run by the recorder)
static std::atomic<bool> g_watchdog{false};
static int g_ran_at = -1;            // (g_ran when the call under test reached the driver)

static void VKAPI_CALL gated_timestamp(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t) {
    std::unique_lock lk(g_mu);
    g_cv.wait(lk, [] { return g_gate_open; });
    g_ran++;
}
static VkResult VKAPI_CALL seen_reset_descriptor_pool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) {
    g_ran_at = g_ran.load();
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL seen_end(VkCommandBuffer) {
    g_ran_at = g_ran.load();
    return VK_SUCCESS;
}
static void open_gate() {
    {
        std::lock_guard lk(g_mu);
        g_gate_open = true;
    }
    g_cv.notify_all();
}
/// a failure after 3 s: the gate opened, so the case ends and reports it. Each armed watchdog has its number; done() disarms it
static std::atomic<int> g_armed{0}, g_done{0};
static void watchdog() {
    const int id = ++g_armed;
    std::thread([id] {
        for (int i = 0; i < 300 && g_done.load() < id; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (g_done.load() >= id) return;
        g_watchdog = true;
        open_gate();
    }).detach();
}
/// (_Exit, not a return: the recorder thread may still be waiting on this file's mutexes, which exit() would destroy)
[[noreturn]] static void pass(const std::string& c) {
    std::printf("PASS %s\n", c.c_str());
    std::fflush(stdout);
    std::_Exit(0);
}
[[noreturn]] static void fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    std::fflush(stdout);
    std::_Exit(1);
}

static const VkCommandBuffer kCmd = (VkCommandBuffer)(uintptr_t)0x10;
static const VkDevice kDev = (VkDevice)(uintptr_t)0x20;
static const VkDescriptorPool kPool = (VkDescriptorPool)(uintptr_t)0x30;
static rec::DrainingMutex replay_mu;

/// one timestamp packet published and the recorder held in it (the gate closed): a packet in flight
static void packet_in_flight() {
    {
        std::lock_guard lk(g_mu);
        g_gate_open = false;
    }
    vkCmdWriteTimestamp2(kCmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, (VkQueryPool)(uintptr_t)0x40, 0);
    rec::t_prod->flush();
}
/// (the producer) the recorder's first packet run and the recorder started
static void warm_up() {
    replay_mu.lock();
    vkCmdWriteTimestamp2(kCmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, (VkQueryPool)(uintptr_t)0x40, 0);
    replay_mu.unlock();
    g_ran = 0;
}

// ---- RX_VK_RECORDER_OWN: with a packet in flight (the recorder held), which call reached the driver before the packet ran
enum class Where { OWN_SLOT, OUTSIDE };
static int reset_with_packet_in_flight(uint64_t end_pos, Where where, bool end_instead = false) {
    g_ran = 0, g_ran_at = -1;
    replay_mu.lock();
    packet_in_flight();
    watchdog();
    rec::g_test_producer_waits = open_gate;   // (the call waits for the recorder: the packet let run then, never before)
    rec::slot_begin(end_pos);
    if (where == Where::OWN_SLOT) {
        rec::OwnSlot own;
        if (end_instead) vkEndCommandBuffer(kCmd);
        else vkResetDescriptorPool(kDev, kPool, 0);
    } else {
        if (end_instead) vkEndCommandBuffer(kCmd);
        else vkResetDescriptorPool(kDev, kPool, 0);
    }
    rec::slot_done();
    rec::g_test_producer_waits = nullptr;
    const int at = g_ran_at;
    open_gate();
    replay_mu.unlock();
    g_done = g_armed.load();
    if (g_watchdog) fail("the call waited for the recorder held in its packet (3 s watchdog)");
    return at;
}

// ---- RX_VK_RECORDER_IDLE=wait: fake WaitOnAddress and WakeByAddressSingle
static std::mutex w_mu;
static std::condition_variable w_cv;
static bool w_woken = false, w_parked = false;
static bool w_wake_works = true;
static std::atomic<int> w_waits{0}, w_returned_at_once{0};
static bool fake_wait(volatile void* addr, void* compare, size_t size, uint32_t) {   // (the 2 ms timeout not kept: no retry)
    w_waits++;
    if (std::memcmp((const void*)addr, compare, size)) {
        w_returned_at_once++;
        return true;
    }
    std::unique_lock lk(w_mu);
    w_parked = true;
    w_cv.notify_all();
    w_cv.wait(lk, [] { return w_woken || g_watchdog.load(); });
    w_woken = false, w_parked = false;
    return true;
}
static void fake_wake(void*) {
    if (!w_wake_works) return;
    std::lock_guard lk(w_mu);
    w_woken = true;
    w_cv.notify_all();
}
static void release_parked() {
    std::lock_guard lk(w_mu);
    g_watchdog = true;
    w_cv.notify_all();
}
static bool wait_until(const std::function<bool()>& ok) {   // (up to 3 s)
    for (int i = 0; i < 300; i++) {
        if (ok()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ok();
}

// the transition: a publish between sleeping set and the wait. The wake does nothing here, so only WaitOnAddress's compare
// (W against the r it was given) keeps the recorder from waiting for a wake that never comes
static std::atomic<bool> race_armed{false};
static std::atomic<int> race_step{0};
static void race_hook() {
    if (!race_armed.load()) return;
    int expect = 0;
    if (!race_step.compare_exchange_strong(expect, 1)) return;   // (once)
    while (race_step.load() != 2 && !g_watchdog.load()) std::this_thread::yield();   // (the producer publishes)
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string c = argv[1];
    vkx::vkCmdWriteTimestamp2 = gated_timestamp;
    vkx::vkResetDescriptorPool = seen_reset_descriptor_pool;
    vkx::vkEndCommandBuffer = seen_end;
    const int mode = rec::mode();   // (first: it reads the switches)
    std::printf("mode %d own %d idle_wait %d\n", mode, (int)rec::g_own, (int)rec::g_idle_wait);
    if (c == "env") pass(c);   // (the switches as the renderer reads them: the Python side checks the line)
    if (rec::mode() != rec::THREAD) fail("RX_VK_RECORDER=thread expected");

    if (c.rfind("own", 0) == 0) {
        warm_up();
        if (c == "own_skip") {   // (RX_VK_RECORDER_OWN=1: the slot's descriptor pool reset made while the packet is held)
            if (!rec::g_own) fail("RX_VK_RECORDER_OWN=1 expected");
            if (reset_with_packet_in_flight(0, Where::OWN_SLOT) != 0) fail("the slot's own reset waited for the recorder");
        } else if (c == "own_off") {   // (the switch unset or 0: the reset waits, as before)
            if (rec::g_own) fail("RX_VK_RECORDER_OWN off expected");
            if (reset_with_packet_in_flight(0, Where::OWN_SLOT) != 1) fail("with the switch off the reset did not wait");
        } else if (c == "own_other") {   // (within OwnSlot, a call that is not the slot's own reset or begin still waits)
            if (reset_with_packet_in_flight(0, Where::OWN_SLOT, true) != 1) fail("vkEndCommandBuffer within OwnSlot did not wait");
        } else if (c == "own_outside") {   // (the reset outside an OwnSlot waits)
            if (reset_with_packet_in_flight(0, Where::OUTSIDE) != 1) fail("the reset outside OwnSlot did not wait");
        } else if (c == "own_guard") {   // (the slot's last end not yet run by the recorder: the reset waits, logged once)
            replay_mu.lock();
            const uint64_t far = rec::produced() + (uint64_t(1) << 40);
            replay_mu.unlock();
            if (reset_with_packet_in_flight(far, Where::OWN_SLOT) != 1) fail("the guard let the reset skip the wait");
            if (reset_with_packet_in_flight(far, Where::OWN_SLOT) != 1) fail("the guard let the reset skip the wait (again)");
            if (mkwvk::logs_own != 1) fail("the guard's line not logged exactly once");
        } else {
            return 2;
        }
        pass(c);
    }

    if (c == "idle_default") {   // (RX_VK_RECORDER_IDLE unset: the condition variable as before, WaitOnAddress never called)
        rec::g_wait_on_address = fake_wait, rec::g_wake_by_address = fake_wake;
        warm_up();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));   // (the recorder idles: spin, then sleep)
        warm_up();
        if (w_waits) fail("WaitOnAddress called with RX_VK_RECORDER_IDLE unset");
    } else if (c == "idle_no_hooks") {   // (=wait without the functions: off at the start, logged, and the packets still run)
        if (!rec::g_idle_wait) fail("RX_VK_RECORDER_IDLE=wait expected");
        warm_up();
        if (rec::g_idle_wait || mkwvk::logs_idle != 1) fail("the idle wait not turned off, or not logged once, without WaitOnAddress");
    } else if (c == "idle_parked") {   // (the recorder waiting on W: a publish wakes it, and its packet runs)
        rec::g_wait_on_address = fake_wait, rec::g_wake_by_address = fake_wake;
        warm_up();
        if (!wait_until([] { std::lock_guard lk(w_mu); return w_parked; })) fail("the recorder never waited on W");
        replay_mu.lock();
        vkCmdWriteTimestamp2(kCmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, (VkQueryPool)(uintptr_t)0x40, 0);
        rec::t_prod->flush();   // (published, the recorder woken; the producer does not wait for it, so no kick())
        const bool ran = wait_until([] { return g_ran.load() == 1; });
        if (!ran) release_parked();
        replay_mu.unlock();
        if (!ran) fail("a publish did not wake the recorder waiting on W (3 s)");
    } else if (c == "idle_race") {   // (a publish after sleeping is set, before the wait: the wait returns at once)
        rec::g_wait_on_address = fake_wait, rec::g_wake_by_address = fake_wake;
        rec::g_test_idle_waiting = race_hook;
        warm_up();               // (the recorder started; it idles, waiting on W: the hook not armed yet)
        race_armed = true;
        fake_wake(nullptr);      // (the parked recorder let go with nothing new: it idles again, into the armed hook)
        if (!wait_until([] { return race_step.load() == 1; })) fail("the recorder never reached its wait");
        {
            std::lock_guard lk(w_mu);
            w_woken = false;     // (a wake left from above, had the recorder not been parked yet)
        }
        w_wake_works = false;    // (from here: only the compare)
        replay_mu.lock();
        vkCmdWriteTimestamp2(kCmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, (VkQueryPool)(uintptr_t)0x40, 0);
        rec::t_prod->flush();
        race_step = 2;
        const bool ran = wait_until([] { return g_ran.load() == 1; });
        if (!ran) release_parked();
        replay_mu.unlock();
        if (!ran) fail("the recorder waited on W after a publish it should have compared against (3 s)");
        if (!w_returned_at_once) fail("WaitOnAddress did not return at once on the changed W");
    } else {
        return 2;
    }
    pass(c);
}
