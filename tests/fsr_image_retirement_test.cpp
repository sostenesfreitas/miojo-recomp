#define _CRT_SECURE_NO_WARNINGS
// No game or GPU: completion is advanced independently of CPU presents and issued signals.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <algorithm>
#include <chrono>
#include <deque>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>
using VmaAllocation = void*;
struct { VkDevice device{}; VkQueue queue{}; std::mutex present_mu; } dev;
VkSemaphore timeline = reinterpret_cast<VkSemaphore>(uintptr_t(1));
uint64_t signaled = 20, gpu_done = 0, cpu_frames = 1;
struct Bundle { uint64_t required; unsigned views = 0; bool image = false; };
std::map<uintptr_t, Bundle> bundles;
uint64_t completed() { return gpu_done; }
[[maybe_unused]] uint64_t texture_frame_no() { return cpu_frames; }
void need(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView v, const VkAllocationCallbacks*) {
    auto& b = bundles[reinterpret_cast<uintptr_t>(v) / 10];
    need(gpu_done >= b.required, "view destroyed while GPU use is pending");
    need(!b.image, "view must precede its image");
    b.views++;
}
namespace mkwvk {
void destroy_image(VkImage image, VmaAllocation) {
    auto& b = bundles[reinterpret_cast<uintptr_t>(image)];
    need(gpu_done >= b.required, "image destroyed while GPU use is pending");
    need(b.views == 3 && !b.image, "all three views destroyed exactly once before image");
    b.image = true;
}
}
// Keep submission code real; the queue executes captured command buffers only on advance().
struct Recording { int slot = 0; VkCommandBuffer cmd{}, upload{}; VkFence fence{}; uint64_t value = 0, batch = 0, part = 0; bool submitted = false; };
Recording* rec_cur = nullptr;
uint64_t done_max = 20, oldest = UINT64_MAX, rec_seq = 0, batch_next = 0, batch_flushed = 0;
VkSemaphore rec_timeline = reinterpret_cast<VkSemaphore>(uintptr_t(2));
VkSemaphore batch_tl = reinterpret_cast<VkSemaphore>(uintptr_t(3));
std::mutex rec_map_mu;
// (the readback's part bookkeeping and the held-submit flag: gpu/vk/vk_replay.cpp)
uint64_t rec_part_next = 0;
std::map<uint64_t,uint64_t> part_rec;
std::condition_variable rec_map_cv;
std::atomic<bool> pend_held{false};
std::map<uint64_t,uint64_t> value_rec;
std::vector<uint64_t> rec_open_values;
struct { std::vector<VkCommandBufferSubmitInfo> cbs; uint64_t signal = 0, rec = 0; bool any_signal = false; uint64_t reflex = 0; } pend;
uint64_t reflex_frame() { return 0; }   // (vk_reflex.cpp: no Reflex here)
const void* reflex_submit_id(const void* next, uint64_t = ~0ull) { return next; }   // (vk_reflex.cpp: no tag without Reflex)
std::chrono::steady_clock::time_point pend_since;
int st_flushes = 0, st_reason[2]{};
int64_t st_batched_cbs = 0;
enum { FR_OTHER, FR_CAP }; int flush_reason = FR_OTHER;
bool batched = false;
bool batch_on() { return batched; }
bool batch_gpu_idle() { return false; }   // (#1993's flush at an idle GPU: none here)
bool texture_uploads_recorded() { return false; }
// Stream flushing is a modeled boundary; keep the real submission's slot/order contract.
int flushed_slot = -1;
void draw_flush_recording(int slot) { flushed_slot = slot; }
void gprof_submitted(int slot, bool) {
    need(flushed_slot == slot, "recording streams flushed before submission profiling");
    flushed_slot = -1;
}
void gpu_time_end(Recording&) {}
void end_pass(VkCommandBuffer) {}
void flush_ub_updates(VkCommandBuffer) {}
void memory_end_submission(VkCommandBuffer, uint64_t) {}
void full_barrier(VkCommandBuffer, const char*) {}
void logf(const char*, ...) {}
void device_lost() { need(false, "unexpected device loss"); }
void note_timeline_sent(uint64_t) {}
uint64_t oldest_unfinished() { return oldest; }
#define PROF(x) ((void)0)
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
struct Queued { std::vector<uintptr_t> uses; uint64_t signal = 0; };
std::deque<Queued> queued;
std::map<VkCommandBuffer,std::vector<uintptr_t>> command_uses;
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue, uint32_t count, const VkSubmitInfo2* info, VkFence) {
    need(count == 1, "one submit"); Queued q;
    for (uint32_t i = 0; i < info->commandBufferInfoCount; i++) {
        auto& uses = command_uses[info->pCommandBufferInfos[i].commandBuffer];
        q.uses.insert(q.uses.end(), uses.begin(), uses.end());
    }
    for (uint32_t i = 0; i < info->signalSemaphoreInfoCount; i++) {
        auto& sig = info->pSignalSemaphoreInfos[i];
        if (sig.semaphore == timeline) {
            need(sig.stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, "completion covers all commands");
            q.signal = sig.value;
        }
    }
    queued.push_back(q); return VK_SUCCESS;
}
void advance() {
    need(!queued.empty(), "queued GPU work");
    auto q = queued.front(); queued.pop_front();
    for (auto id : q.uses) need(!bundles[id].image && !bundles[id].views, "GPU accessed a destroyed image bundle");
    if (q.signal) gpu_done = std::max(gpu_done, q.signal);
}
// PRODUCTION
void retire(uintptr_t id, uint64_t required) {
    bundles[id].required = required;
    Image im;
    im.image = reinterpret_cast<VkImage>(id);
    im.memory = reinterpret_cast<void*>(id);
    im.view = reinterpret_cast<VkImageView>(id * 10);
    im.levels = {reinterpret_cast<VkImageView>(id * 10 + 1), reinterpret_cast<VkImageView>(id * 10 + 2)};
    drop_image(im);
    need(!im.image && !im.view && im.levels.empty(), "retirement transfers the whole bundle");
}
void retained(uintptr_t id) { need(!bundles[id].views && !bundles[id].image, "pending bundle retained intact"); }
void freed(uintptr_t id) { need(bundles[id].views == 3 && bundles[id].image, "completed bundle reclaimed"); }
int main(int argc, char** argv) {
    need(argc == 2, "case required");
    const char* mode = argv[1];
    if (!std::strncmp(mode, "queue-", 6)) {
        gpu_done = 20;
        batched = !std::strcmp(mode, "queue-batch");
        Recording old; old.value = !std::strcmp(mode, "queue-post-present") ? 21 : 9; old.cmd = reinterpret_cast<VkCommandBuffer>(uintptr_t(11));
        command_uses[old.cmd] = {1};
        retire(1, 21);
        // Old logical9 resumes after signal20, or an unsignalled FG recording follows present20.
        submit_recording(old, false);
        if (!std::strcmp(mode, "queue-parallel")) {
            Recording worker = old; worker.cmd = reinterpret_cast<VkCommandBuffer>(uintptr_t(12));
            command_uses[worker.cmd] = {1}; submit_recording(worker, false);
        }
        for (unsigned i = 0; i < 100; i++) { cpu_frames++; free_dead(); retained(1); }
        Recording next; next.value = 21; next.cmd = reinterpret_cast<VkCommandBuffer>(uintptr_t(13));
        rec_cur = &next; done_max = 21; finish_recording(21);
        need(signaled == 21, "actual finish issues covering signal");
        free_dead(); retained(1); // Issued or batched does not mean completed.
        if (batched) { need(queued.empty(), "batch stays held"); queue_flush_locked(); }
        while (!queued.empty()) { advance(); free_dead(); if (gpu_done < 21) retained(1); }
        freed(1); free_dead(); return 0;
    }
    if (!std::strcmp(mode, "empty")) { free_dead(); need(bundles.empty(), "empty retirement is harmless"); return 0; }
    if (!std::strcmp(mode, "no-timeline") || !std::strcmp(mode, "overflow")) {
        if (!std::strcmp(mode, "no-timeline")) timeline = VK_NULL_HANDLE;
        else signaled = UINT64_MAX;
        retire(1, 21); gpu_done = UINT64_MAX; cpu_frames = 100; free_dead(); retained(1); return 0;
    }
    if (!std::strcmp(mode, "out-of-order")) {
        signaled = 30; retire(1, 31); signaled = 20; retire(2, 21);
        cpu_frames = 100; gpu_done = 21; free_dead(); retained(1); freed(2);
        gpu_done = 31; free_dead(); freed(1); freed(2); free_dead(); return 0;
    }
    if (!std::strcmp(mode, "completed")) {
        retire(1, 21); gpu_done = 21; free_dead(); freed(1); free_dead(); return 0;
    }
    // A resumed logical submission may be value9 after value20 is already complete.
    // An unsignalled FG recording likewise follows the present's signal20.
    if (!std::strcmp(mode, "resumed") || !std::strcmp(mode, "post-present")) gpu_done = 20;
    retire(1, 21);
    if (!std::strcmp(mode, "several")) { signaled = 21; retire(2, 22); }
    // More CPU frames, including already issued/batched signals, are not GPU progress.
    for (unsigned i = 0; i < 100; i++) { cpu_frames++; free_dead(); retained(1); }
    if (!std::strcmp(mode, "batch")) { signaled = 40; free_dead(); retained(1); }
    gpu_done = 21; free_dead(); freed(1);
    if (!std::strcmp(mode, "several")) { retained(2); gpu_done = 22; free_dead(); freed(2); }
    free_dead(); freed(1);
}
