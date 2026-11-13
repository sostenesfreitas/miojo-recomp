// (hookshot) RX_VK_RECORDER: the command buffer calls as packets, step 1 of a recorder thread (the "sync" phase).
//
// Every vkCmd* call vk_work.h routes here is copied into a packet: its value arguments, and every array and struct it points to (the
// descriptor writes and the image/buffer/texel infos they point to, push constant bytes, vertex buffer arrays, the rendering info and
// its attachments, the barriers, the copy regions, the dynamic state arrays), so a packet owns all it needs and no longer reads the
// caller's memory. A packet is run by one decoder (execute: the packet's thunk calls the real ::vkx function with the copies).
//
//   RX_VK_RECORDER unset / anything else: off, the calls go straight to the driver as before (one relaxed load and a branch a call)
//   RX_VK_RECORDER=sync: each call packed, then run at once on the same thread, in the same order: the same calls with the same
//                        values, made from the packet's copies (the proof that the copies are complete and exact)
//   RX_VK_RECORDER=defer: (step 1.5, serial replay only) the packets held in the thread's arena and run at drain(), in order, on the
//                        same thread; the caller reuses its memory in between, as it will once a recorder thread runs them. Drained
//                        before every call of RX_REC_BOUNDARIES (vk_work.h: command buffer begin/end/reset, submits, presents, waits,
//                        query and fault reads, every destruction), when vk_replay.cpp's replay_mu is let go (DrainingMutex), before
//                        a fallback, at the soft byte budget, and when the arena has no memory (then the call is made directly).
//                        Refused (off, logged once) with RX_VK_PARALLEL>=2; while par_active, packets run at once.
//                        RX_VK_RECORDER_POISON=1: the arena filled with 0xCD after each drain. tests/test_recorder_sync.py
//   RX_VK_RECORDER=thread: (step 2, serial replay only) the thread that holds vk_replay.cpp's replay_mu is the producer: its packets
//                        go into one bounded ring (Recorder, below) and a recorder thread runs them, in order, while the replay goes
//                        on. Before every call of RX_REC_BOUNDARIES on the producer (but those of unordered()), and when it lets
//                        replay_mu go, it waits until
//                        the recorder has run everything it put in the ring: outside a replay_mu hold the recorder is idle, and no
//                        command buffer, pool or queue call of the replay's is made while its packets are in flight. Threads without
//                        replay_mu make their calls directly (after the recorder has run what was published, should it be busy:
//                        counted and logged once). The ring full: the producer waits (backpressure); a packet bigger than the room
//                        it is given, or no ring: the producer waits until the recorder is idle, then makes the call directly.
//                        Refused (off, logged once) with RX_VK_PARALLEL>=2. RX_VK_RECORDER_RING_MB (default 8),
//                        RX_VK_RECORDER_POISON=1 (each packet filled with 0xCD once run), RX_VK_RECORDER_STATS=0 (no 10 s stats
//                        line), RX_VK_RECORDER_SPIN_US (the recorder's spin with nothing to run, before it sleeps: default adaptive 20-200)
//
// A packet: [Packet header: thunk, next, op, size][the call's closure: its value arguments and the arena pointers of its copies],
// preceded in the arena by the copied arrays. The arena is the recording thread's own (thread_local: the parallel replay's workers each
// have theirs) and in sync mode is rewound after each packet runs, so it stays at one packet's size. A call whose structs carry a
// pNext chain (none does today) or a descriptor type the packer does not know is not packed: what is held is drained, then the call
// is made directly (counted in Arena::fallbacks).
//
// Include through vk_work.h, not by itself: the names below are the wrappers its macros call.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include "vk_loader.h"

namespace mkwvk {
void logf(const char* fmt, ...);        // (vk_main.cpp)
extern std::atomic<bool> par_active;   // (vk_replay.cpp: a parallel replay's segments are being recorded)
} // namespace mkwvk

// The calls the recorder carries. V: value arguments only (handles, numbers, enums), one generic packer; A: arrays or structs behind
// pointers, a packer of their own below. vk_work.h has a macro for each (the test checks the two lists agree).
#define RX_REC_CALLS(V, A)                                                                                                         \
    A(vkCmdPipelineBarrier2) A(vkCmdBeginRendering) V(vkCmdEndRendering)                                                           \
    V(vkCmdDraw) V(vkCmdDrawIndexed) V(vkCmdDrawIndirect) V(vkCmdDrawIndexedIndirect) V(vkCmdDrawIndirectCount)                     \
    V(vkCmdDrawIndexedIndirectCount) V(vkCmdDrawMeshTasksEXT) V(vkCmdDrawMeshTasksIndirectEXT) V(vkCmdDrawMeshTasksIndirectCountEXT) \
    V(vkCmdDispatch) V(vkCmdDispatchIndirect)                                                                                      \
    A(vkCmdCopyBuffer) A(vkCmdCopyBufferToImage) A(vkCmdCopyImageToBuffer) A(vkCmdCopyImage) A(vkCmdBlitImage) A(vkCmdResolveImage) \
    A(vkCmdClearColorImage) A(vkCmdClearDepthStencilImage) A(vkCmdClearAttachments) V(vkCmdFillBuffer) A(vkCmdUpdateBuffer)          \
    V(vkCmdBeginQuery) V(vkCmdEndQuery)                                                                                            \
    V(vkCmdBindPipeline) A(vkCmdBindDescriptorSets) V(vkCmdBindIndexBuffer) A(vkCmdBindVertexBuffers2) A(vkCmdPushConstants)        \
    A(vkCmdPushDescriptorSetKHR)                                                                                                   \
    A(vkCmdSetViewport) A(vkCmdSetScissor) A(vkCmdSetViewportWithCount) A(vkCmdSetScissorWithCount) A(vkCmdSetBlendConstants)       \
    V(vkCmdSetStencilReference) V(vkCmdSetStencilCompareMask) V(vkCmdSetStencilWriteMask) V(vkCmdSetStencilOp) V(vkCmdSetDepthBias) \
    V(vkCmdSetDepthBiasEnable) V(vkCmdSetLineWidth) V(vkCmdSetDepthBoundsTestEnable) V(vkCmdSetCullMode) V(vkCmdSetFrontFace)       \
    V(vkCmdSetPrimitiveTopology) V(vkCmdSetPrimitiveRestartEnable) V(vkCmdSetDepthTestEnable) V(vkCmdSetDepthWriteEnable)           \
    V(vkCmdSetDepthCompareOp) V(vkCmdSetStencilTestEnable) V(vkCmdSetRasterizerDiscardEnable) V(vkCmdSetPolygonModeEXT)             \
    V(vkCmdSetDepthClampEnableEXT) V(vkCmdSetAlphaToCoverageEnableEXT)                                                             \
    A(vkCmdSetColorBlendEnableEXT) A(vkCmdSetColorBlendEquationEXT) A(vkCmdSetColorWriteMaskEXT)                                   \
    V(vkCmdWriteBufferMarkerAMD) A(vkCmdSetCheckpointNV) V(vkCmdWriteTimestamp2) V(vkCmdResetQueryPool)

// (DEFER) The calls that drain the thread's held packets first (vk_work.h has a macro for each: the test checks the two lists agree,
// and that every file making one of these calls includes vk_work.h or is on its list of known exceptions). K: a ::vkx function,
// M: VMA's (global)
#define RX_REC_BOUNDARIES(K, M)                                                                                                    \
    K(vkBeginCommandBuffer) K(vkEndCommandBuffer) K(vkResetCommandPool) K(vkDestroyCommandPool)                                    \
    K(vkQueueSubmit2) K(vkQueueWaitIdle) K(vkQueueBindSparse) K(vkQueuePresentKHR) K(vkAcquireNextImageKHR) K(vkDeviceWaitIdle)    \
    K(vkWaitForFences) K(vkResetFences) K(vkWaitSemaphores) K(vkGetSemaphoreCounterValue)                                         \
    K(vkGetQueryPoolResults) K(vkResetQueryPool) K(vkGetQueueCheckpointDataNV) K(vkGetDeviceFaultInfoEXT) K(vkResetDescriptorPool)  \
    K(vkDestroyBuffer) K(vkDestroyImage) K(vkDestroyImageView) K(vkDestroyBufferView) K(vkDestroySampler) K(vkDestroyPipeline)      \
    K(vkDestroyPipelineLayout) K(vkDestroyDescriptorSetLayout) K(vkDestroyDescriptorPool) K(vkDestroyQueryPool)                   \
    K(vkDestroyShaderModule) K(vkDestroyPipelineCache) K(vkDestroyFence) K(vkDestroySemaphore) K(vkDestroySwapchainKHR)           \
    K(vkDestroySurfaceKHR) K(vkDestroyDevice) K(vkDestroyInstance) K(vkFreeMemory)                                                 \
    M(vmaDestroyBuffer) M(vmaDestroyImage) M(vmaInvalidateAllocation)

namespace mkwvk::rec {

enum Op : uint16_t {
#define RX_REC_OP(f) OP_##f,
    RX_REC_CALLS(RX_REC_OP, RX_REC_OP)
#undef RX_REC_OP
    OP_COUNT
};
inline const char* op_name(uint32_t op) {
    static const char* const names[] = {
#define RX_REC_NAME(f) #f,
        RX_REC_CALLS(RX_REC_NAME, RX_REC_NAME)
#undef RX_REC_NAME
    };
    return op < OP_COUNT ? names[op] : "?";
}

/// (THREAD) where the producer waited for the recorder: each boundary call, then the recorder's own reasons
enum Bound : uint16_t {
#define RX_REC_BOUND(f) B_##f,
    RX_REC_BOUNDARIES(RX_REC_BOUND, RX_REC_BOUND)
#undef RX_REC_BOUND
    B_REPLAY_UNLOCK,   // replay_mu let go
    B_FALLBACK,        // a call not packed (pNext chain, unknown descriptor type)
    B_NOMEM,           // a packet bigger than the ring's room for it, or no ring
    B_PAR,             // a parallel replay's segments being recorded (par_active)
    B_RING_FULL,       // backpressure: the ring had no room
    B_FOREIGN,         // a thread without replay_mu made a call while the recorder was busy
    B_DIRECT,          // code about to record into a command buffer directly, outside vk_work.h (Dear ImGui: vk_menu.cpp)
    B_COUNT
};
inline const char* bound_name(uint32_t b) {
    static const char* const names[] = {
#define RX_REC_BNAME(f) #f,
        RX_REC_BOUNDARIES(RX_REC_BNAME, RX_REC_BNAME)
#undef RX_REC_BNAME
        "replay_mu unlock", "fallback", "no ring room", "par_active", "ring full", "off the replay lock", "direct recording"};
    static_assert(sizeof names / sizeof *names == B_COUNT);
    return b < B_COUNT ? names[b] : "?";
}

enum Mode : int { OFF = 0, SYNC = 1, DEFER = 2, THREAD = 3, PASS = 4 /* (an encoder's own: the call made directly) */ };
inline std::atomic<int> g_mode{-1};
inline std::atomic<bool> g_poison{false};   // (RX_VK_RECORDER_POISON=1: the arena filled with 0xCD after each drain)
/// (DEFER) the bytes held before the next packet drains them first (soft), and the arena's most (hard: past it a call is not
/// packed: what is held is drained, then the call made directly). Tests change them; the renderer does not
inline size_t g_soft_budget = size_t(8) << 20, g_hard_budget = size_t(64) << 20;
/// (THREAD) the ring's bytes (a power of two; RX_VK_RECORDER_RING_MB) and the room each packet is given (a vkCmdUpdateBuffer of
/// 64 KB fits; a bigger packet is made directly, after the recorder is idle). Read when the recorder starts: tests change them
inline size_t g_ring_bytes = size_t(8) << 20, g_ring_room = size_t(80) << 10;
/// (THREAD) the bytes of packets the producer writes before it publishes them (W): fewer stores to the line the recorder polls.
/// Everything is published before any wait, so a boundary still waits for every packet before it. Tests change it
inline size_t g_publish_bytes = 1024;
/// (THREAD) how long the recorder spins with nothing to run before it sleeps. Spinning keeps it awake through the replay's short gaps
/// (no wake-up call on the replay thread, no wake-up delay at its next wait); sleeping gives the core back. 0 (the default):
/// adaptive, between 20 and 200 us: back to 200 when a sleep ended within 200 us (a gap the spin would have covered: the replay paid
/// a wake-up for it), halved when a sleep lasted longer (a long gap: the spin was wasted). RX_VK_RECORDER_SPIN_US=<us>: fixed.
/// Read when the recorder starts: tests change it
inline uint64_t g_spin_ns = 0;
constexpr uint64_t kSpinLo = 20000, kSpinHi = 200000;
inline bool g_stats_log = true;   // (RX_VK_RECORDER_STATS=0: no stats line every 10 s)

/// RX_VK_PARALLEL as vk_replay.cpp's par_threads() reads it: 2 or more, segments of one command buffer recorded on several threads
inline bool parallel_configured() {
    const char* p = getenv("RX_VK_PARALLEL");
    return p && atoi(p) >= 2;
}
/// RX_VK_RECORDER: sync; defer, thread (serial only: refused, off, with RX_VK_PARALLEL); anything else, or unset, off
inline int mode_from_env(bool* refused) {
    const char* e = getenv("RX_VK_RECORDER");
    *refused = false;
    if (!e) return OFF;
    if (!strcmp(e, "sync")) return SYNC;
    if (!strcmp(e, "defer") || !strcmp(e, "thread")) {
        if (!parallel_configured()) return e[0] == 'd' ? DEFER : THREAD;
        *refused = true;
        return OFF;
    }
    return OFF;
}
inline int mode_init() {
    bool refused;
    const int m = mode_from_env(&refused);
    const char* p = getenv("RX_VK_RECORDER_POISON");
    g_poison.store((m == DEFER || m == THREAD) && p && atoi(p) != 0, std::memory_order_relaxed);
    if (m == THREAD) {
        if (const char* mb = getenv("RX_VK_RECORDER_RING_MB"))
            if (atoi(mb) > 0) g_ring_bytes = size_t(std::min(atoi(mb), 1024)) << 20;
        if (const char* s = getenv("RX_VK_RECORDER_STATS")) g_stats_log = atoi(s) != 0;
        if (const char* s = getenv("RX_VK_RECORDER_SPIN_US")) g_spin_ns = (uint64_t)std::max(1, atoi(s)) * 1000;
    }
    int was = -1;
    if (!g_mode.compare_exchange_strong(was, m, std::memory_order_relaxed)) return was;   // (another thread read it first)
    if (refused)   // (once: the thread that set the mode)
        ::mkwvk::logf("vk: RX_VK_RECORDER=%s refused: RX_VK_PARALLEL=%s records a command buffer on several threads (deferred "
                      "packets are the recording thread's own); the recorder is off",
                      getenv("RX_VK_RECORDER"), getenv("RX_VK_PARALLEL"));
    else if (m == DEFER)
        ::mkwvk::logf("vk: RX_VK_RECORDER=defer: vkCmd* packed and held, drained in order at each command buffer begin, end and "
                      "reset, submit, wait, readback, destruction and replay lock release (serial replay only)%s",
                      g_poison.load(std::memory_order_relaxed) ? "; the arena poisoned after each drain" : "");
    else if (m == THREAD)
        ::mkwvk::logf("vk: RX_VK_RECORDER=thread: vkCmd* packed on the replay thread and run in order by a recorder thread; the "
                      "replay waits for it at each command buffer begin, end and reset, submit, wait, readback, destruction and "
                      "replay lock release (serial replay only); ring %zu KB, spin %s%llu us%s",
                      g_ring_bytes >> 10, g_spin_ns ? "" : "adaptive 20-", (unsigned long long)(g_spin_ns ? g_spin_ns / 1000 : 200),
                      g_poison.load(std::memory_order_relaxed) ? "; packets poisoned once run" : "");
    return m;
}
inline int mode() {
    const int m = g_mode.load(std::memory_order_relaxed);
    return m >= 0 ? m : mode_init();
}
inline bool on() { return mode() != OFF; }

struct alignas(16) Packet {
    void (*run)(const Packet*);   // the decoder for this packet's call
    Packet* next;                 // (DEFER) the next held packet
    uint32_t op;                  // Op, for diagnostics
    uint32_t size;                // bytes of the closure after the header
};

/// The thread's packets. Chunks in a fixed table, each from the non-throwing aligned new: alloc never throws and never loses a
/// block. It returns null when the table is full, the hard budget would be passed, or the system has no memory; the encoder then
/// drains what is held (in order) and makes the call directly
struct Arena {
    static constexpr size_t kChunk = 1u << 20;
    static constexpr size_t kMaxChunks = 64;
    struct Chunk {
        uint8_t* p;
        size_t cap;
    };
    struct Mark {
        size_t ci, used, held;
    };
    Chunk chunks[kMaxChunks] = {};
    size_t nchunks = 0, capacity = 0;         // (the chunks made, and their bytes)
    size_t ci = 0, used = 0;                  // the chunk being filled and its bytes used
    size_t held = 0;                          // (DEFER) bytes encoded since the last drain
    Packet* head = nullptr;                   // (DEFER) the packets not run yet, in order
    Packet** tail = &head;
    uint64_t packets = 0, bytes = 0, fallbacks = 0, failures = 0, budget_drains = 0;

    void* alloc(size_t n, size_t align) noexcept {
        for (;;) {
            if (ci < nchunks) {
                const size_t at = (used + align - 1) & ~(align - 1);
                if (at + n <= chunks[ci].cap) {
                    used = at + n;
                    bytes += n, held += n;
                    return chunks[ci].p + at;
                }
                if (ci + 1 < nchunks || used) {   // (the next chunk, or a new one; a chunk too small even empty is passed)
                    ci++, used = 0;
                    continue;
                }
            }
            const size_t cap = n + align > kChunk ? n + align : kChunk;
            if (nchunks == kMaxChunks || cap > g_hard_budget || capacity > g_hard_budget - cap) return nullptr;
            void* p = ::operator new(cap, std::align_val_t(64), std::nothrow);
            if (!p) return nullptr;
            chunks[nchunks++] = {static_cast<uint8_t*>(p), cap};
            capacity += cap;
            ci = nchunks - 1, used = 0;
        }
    }
    Mark mark() const { return {ci, used, held}; }
    void rewind(Mark m) { ci = m.ci, used = m.used, held = m.held; }
    void poison() {   // (the bytes used since the last drain: the chunks before ci whole, ci up to used)
        for (size_t i = 0; i < nchunks && i <= ci; i++) std::memset(chunks[i].p, 0xCD, i < ci ? chunks[i].cap : used);
    }
    ~Arena();
};
inline thread_local Arena t_arena;

/// The decoder: runs one packet's call with its copies
inline void execute(const Packet* p) { p->run(p); }

/// (DEFER) runs every held packet, in order, and empties the arena
inline void drain() {
    Arena& a = t_arena;
    const Packet* p = a.head;
    a.head = nullptr, a.tail = &a.head;
    while (p) {
        const Packet* next = p->next;
        execute(p);
        p = next;
    }
    if (g_poison.load(std::memory_order_relaxed)) a.poison();
    a.ci = 0, a.used = 0, a.held = 0;
}
/// (a thread that ends with packets held, which the drains leave none of: they are run, never dropped)
inline Arena::~Arena() {
    if (head) drain();
    for (size_t i = 0; i < nchunks; i++) ::operator delete(chunks[i].p, std::align_val_t(64));
}

// ---- (THREAD) the ring and the recorder thread ----
// One ring, one recorder thread (started at the first packet, joined by stop()). The producer is whichever thread holds replay_mu
// (DrainingMutex sets t_prod): one at a time, handed on by the mutex, so the ring has one producer and one consumer. Byte counters:
// W, what the producer published; R, what the recorder has run (every packet before R has reached the driver). A packet is
// [Packet: run, next = its end, op, size = its closure's offset][copied arrays][closure], contiguous, 16-aligned; one never wraps:
// the producer gives each packet g_ring_room contiguous bytes (a wrap marker, run = null, ends the ring's tail when less is left,
// and a tail too small for a header is passed over by both sides). The memory never moves: the packets' pointers into it hold
inline void cpu_pause() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#endif
}
inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/// (set by vk_replay.cpp: the recorder thread's name and priority, which need windows.h)
inline void (*g_thread_start_hook)() = nullptr;
#ifdef RX_REC_TEST_HOOKS
inline void (*g_test_idle_locked)() = nullptr;   // (tests: called in idle() with sleep_mu held)
#endif
/// The recorder thread cannot go on (an exception out of a driver call): logged, written to stderr and the process ended, as
/// vk_replay.cpp's replay_fatal does (exit code 3). Never swallowed: a packet not run is a command lost
[[noreturn]] inline void recorder_fatal(const char* why) {
    ::mkwvk::logf("vk: FATAL: RX_VK_RECORDER=thread: the recorder thread: %s", why);
    std::fprintf(stderr, "vk: FATAL: RX_VK_RECORDER=thread: the recorder thread: %s\n", why);
    std::fflush(stderr);
    std::_Exit(3);
}

struct Recorder {
    alignas(64) std::atomic<uint64_t> W{0};   // bytes published (the producer)
    alignas(64) std::atomic<uint64_t> R{0};   // bytes run (the recorder)
    alignas(64) std::atomic<bool> sleeping{false};
    std::atomic<bool> stop_req{false};
    std::atomic<bool> ready{false};    // ring made, thread running
    std::atomic<bool> broken{false};   // no ring or no thread: the calls made directly
    std::atomic<bool> logged_direct{false}, logged_foreign{false};
    uint8_t* base = nullptr;
    size_t cap = 0, mask = 0, room = 0;
    // (the producer's: its cursor, what of it is published (W), R as it last read it, and how much it writes before publishing)
    alignas(64) uint64_t w = 0, w_pub = 0, r_seen = 0, batch = 0;
    uint64_t spin_limit = 0;   // (the recorder's: g_spin_ns when it started)
    // (stats: written by the producer or the recorder, read by the recorder's stats line; never on a packet's path)
    std::atomic<uint64_t> wait_n[B_COUNT]{}, wait_ns[B_COUNT]{}, wait_max[B_COUNT]{};
    std::atomic<uint64_t> spin_ns{0}, sleeps{0}, short_sleeps{0};   // (the recorder idle: time spun, sleeps, those under 200 us)
    std::atomic<uint64_t> packets{0}, busy_ns{0}, starts{0}, direct{0};   // (direct: packets that did not fit, made directly)
    std::mutex life_mu, sleep_mu;
    std::condition_variable sleep_cv;
    std::thread th;

    bool start();
    void stop();
    void main();
    void loop();
    bool idle(uint64_t r, uint64_t& seen, uint64_t& last_log);
    void log_stats(double secs);

    void wake() {
        { std::lock_guard lk(sleep_mu); }
        sleep_cv.notify_one();
    }
    void kick() {   // (the recorder asleep with work published: a wake-up the packet's cheap check may have missed)
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_relaxed)) wake();
    }
    template <class C>
    void spin(C done) {
        kick();
        for (uint32_t i = 0; !done(); i++) {
            if (i < 4096) cpu_pause();
            else {
                std::this_thread::yield();
                if ((i & 255) == 0) kick();
            }
        }
    }
    bool idle_at(uint64_t target) const { return R.load(std::memory_order_acquire) >= target; }
    void note_wait(Bound k, uint64_t ns) {   // (a wait's count, time and longest, for the stats line)
        wait_n[k].fetch_add(1, std::memory_order_relaxed), wait_ns[k].fetch_add(ns, std::memory_order_relaxed);
        uint64_t m = wait_max[k].load(std::memory_order_relaxed);
        while (ns > m && !wait_max[k].compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
    }
    /// (the producer) what it has written, published
    void flush() {
        if (w == w_pub) return;
        W.store(w, std::memory_order_release);
        w_pub = w;
        if (sleeping.load(std::memory_order_relaxed)) [[unlikely]]
            wake();
    }
    /// (the producer) everything it wrote has reached the driver
    void wait_idle(Bound k) {
        flush();
        const uint64_t target = w;
        if (idle_at(target)) return;
        const uint64_t t0 = now_ns();
        spin([&] { return idle_at(target); });
        note_wait(k, now_ns() - t0);
    }
    /// (a thread without replay_mu) what was published so far has reached the driver: its direct call comes after it. Idle in
    /// the replay's design (replay_mu's unlock waits); otherwise counted, logged once
    void foreign_wait() {
        const uint64_t target = W.load(std::memory_order_acquire);
        if (idle_at(target)) return;
        if (!logged_foreign.exchange(true, std::memory_order_relaxed))
            ::mkwvk::logf("vk: RX_VK_RECORDER=thread: a command buffer or recorded call on a thread without the replay lock while the "
                          "recorder was busy: made after the recorder caught up (counted in the stats line; logged once)");
        const uint64_t t0 = now_ns();
        spin([&] { return idle_at(target); });
        note_wait(B_FOREIGN, now_ns() - t0);
    }
    /// (the producer) `need` contiguous-or-not free bytes: waits for the recorder (backpressure) when the ring has less
    void wait_room(uint64_t need) {
        if (cap - (w - r_seen) >= need) return;
        r_seen = R.load(std::memory_order_acquire);
        if (cap - (w - r_seen) >= need) return;
        flush();   // (what the recorder is to make room by running)
        const uint64_t t0 = now_ns();
        spin([&] { return cap - (w - (r_seen = R.load(std::memory_order_acquire))) >= need; });
        note_wait(B_RING_FULL, now_ns() - t0);
    }
    /// (the producer, an encoder starting) the next packet's place, with `room` contiguous bytes: its header, and the end of what it
    /// may use. Never more than `room`, even when the ring has more free: whether a packet is made directly depends on its size
    /// alone, not on how far the recorder had got (tests/test_recorder_sync.py: the oversize runs)
    Packet* reserve(uint8_t*& lim) {
        size_t off = (size_t)(w & mask), tail = cap - off;
        if (tail < room) {   // (the ring's tail: passed over, the packet at its start)
            wait_room(tail + room);
            if (tail >= sizeof(Packet)) {
                Packet* mk = reinterpret_cast<Packet*>(base + off);
                mk->run = nullptr, mk->next = nullptr, mk->op = 0, mk->size = 0;
            }
            w += tail;   // (published with the next packet)
            off = 0, tail = cap;
        } else {
            wait_room(room);
        }
        lim = base + off + room;   // (free and contiguous: tail >= room, and wait_room() made room bytes free past w)
        return reinterpret_cast<Packet*>(base + off);
    }
    void publish(const Packet* p, const uint8_t* end) {
        w += (uint64_t)(end - reinterpret_cast<const uint8_t*>(p));
        // (the recorder asleep: this packet now, and the wake-up with it, so that it runs while the replay goes on, not at its
        // next wait)
        if (w - w_pub >= batch || sleeping.load(std::memory_order_relaxed)) flush();
    }
    // (no destructor: the recorder lives as long as the process, below)
};
/// The recorder, for the process's lifetime: made once, never destroyed, as vk_jobs' detached workers. Windows ends every other
/// thread before a DLL's global destructors run (ExitProcess: the window's close, svcExitProcess, the updater, main's return,
/// hos_fatal), possibly while the recorder holds sleep_mu in idle(); a destructor that woke (sleep_mu) and joined it would then wait
/// for a mutex a dead thread owns. No global destructor touches its mutexes or thread: at exit nothing needs it (what is in the
/// ring is commands never to be submitted; it writes nothing else). stop() is the explicit teardown (vkDestroyDevice's boundary).
/// tests/test_recorder_sync.py: recorder_exit_test.cpp
inline Recorder& g_rec = *new Recorder;
/// (THREAD) the ring, while this thread holds replay_mu; null otherwise
inline constinit thread_local Recorder* t_prod = nullptr;

inline bool Recorder::start() {   // (the producer, its first packet)
    std::lock_guard lk(life_mu);
    if (ready.load(std::memory_order_relaxed)) return true;
    if (broken.load(std::memory_order_relaxed)) return false;
    if (!base) {
        size_t c = 4096;
        while (c < g_ring_bytes && c < (size_t(1) << 30)) c <<= 1;
        base = static_cast<uint8_t*>(::operator new(c, std::align_val_t(64), std::nothrow));
        if (!base) {
            broken.store(true);
            ::mkwvk::logf("vk: RX_VK_RECORDER=thread: no memory for the recorder's ring (%zu KB): every call made directly", c >> 10);
            return false;
        }
        cap = c, mask = c - 1;
    }
    room = std::max<size_t>(std::min(g_ring_room, cap / 4) & ~size_t(15), 2 * sizeof(Packet));
    // (the counters go on from where a stopped recorder left them, never back to 0: a foreign_wait() that read W just before
    // a restart still sees R reach it. Anything a stopped recorder left unrun is dropped, as a fresh ring did; offsets are & mask)
    const uint64_t at = W.load(std::memory_order_acquire);
    R.store(at, std::memory_order_release), w = w_pub = r_seen = at, stop_req.store(false);
    batch = std::min(g_publish_bytes, room);
    spin_limit = 0;   // (set by the recorder: g_spin_ns, or adaptive)
    try {
        th = std::thread([this] { main(); });
    } catch (...) {
        broken.store(true);
        ::mkwvk::logf("vk: RX_VK_RECORDER=thread: the recorder thread could not be started: every call made directly");
        return false;
    }
    starts.fetch_add(1);
    ready.store(true);
    return true;
}
/// Teardown: everything published run, the thread joined, the ring let go (made again at the next packet). Any thread, with no
/// producer encoding at the time (the renderer's teardown, or the producer itself: vkDestroyDevice's boundary)
inline void Recorder::stop() {
    std::lock_guard lk(life_mu);
    if (!th.joinable()) return;
    if (t_prod == this) flush();   // (the producer's own stop: what it wrote, published first)
    const uint64_t target = W.load(std::memory_order_acquire);
    spin([&] { return idle_at(target); });
    stop_req.store(true);
    wake();
    th.join();
    ready.store(false);
    if (g_stats_log) log_stats(0);
    ::operator delete(base, std::align_val_t(64));
    base = nullptr, cap = 0;
}
inline void Recorder::main() {
    if (g_thread_start_hook) g_thread_start_hook();
    try {
        loop();
    } catch (const std::exception& e) {
        recorder_fatal(e.what());
    } catch (...) {
        recorder_fatal("an exception out of a driver call");
    }
}
/// (the recorder, nothing published to run) spins a while, then sleeps until woken (or 2 ms); false: stop asked and nothing left
inline bool Recorder::idle(uint64_t r, uint64_t& seen, uint64_t& last_log) {
    const uint64_t t0 = now_ns();
    if (!spin_limit) spin_limit = g_spin_ns ? g_spin_ns : kSpinHi;   // (fixed, or adaptive from the top)
    for (uint32_t i = 0;; i++) {
        if ((seen = W.load(std::memory_order_acquire)) != r || stop_req.load(std::memory_order_acquire)) {
            spin_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
            return seen != r || W.load(std::memory_order_acquire) != r;
        }
        cpu_pause();
        if ((i & 15) == 15 && now_ns() - t0 > spin_limit) break;   // (spin_limit: a replay's gaps between calls, spun through)
    }
    spin_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
    sleeps.fetch_add(1, std::memory_order_relaxed);
    if (g_stats_log && t0 - last_log > 10000000000ull) {
        if (last_log) log_stats((double)(t0 - last_log) / 1e9);
        last_log = t0;
    }
    const uint64_t s0 = now_ns();
    {
        std::unique_lock lk(sleep_mu);
#ifdef RX_REC_TEST_HOOKS
        if (g_test_idle_locked) g_test_idle_locked();
#endif
        sleeping.store(true, std::memory_order_seq_cst);
        if (W.load(std::memory_order_seq_cst) == r && !stop_req.load()) sleep_cv.wait_for(lk, std::chrono::milliseconds(2));
        sleeping.store(false, std::memory_order_relaxed);
    }
    seen = W.load(std::memory_order_acquire);
    const uint64_t slept = now_ns() - s0;
    if (slept < kSpinHi) short_sleeps.fetch_add(1, std::memory_order_relaxed);
    if (!g_spin_ns) {   // (adaptive: a short sleep means a gap the longest spin covers; a long one, a spin to shorten)
        if (slept < kSpinHi && seen != r) spin_limit = kSpinHi;
        else if (slept >= kSpinHi) spin_limit = std::max(kSpinLo, spin_limit / 2);
    }
    return true;
}
inline void Recorder::loop() {
    uint64_t r = R.load(std::memory_order_relaxed), seen = r, last_log = now_ns(), n = 0, busy0 = 0;
    const bool poison = g_poison.load(std::memory_order_relaxed);
    for (;;) {
        if (r == seen && (seen = W.load(std::memory_order_acquire)) == r) {
            if (busy0) busy_ns.fetch_add(now_ns() - busy0, std::memory_order_relaxed), busy0 = 0;
            if (n) packets.fetch_add(n, std::memory_order_relaxed), n = 0;
            if (!idle(r, seen, last_log)) return;
            continue;
        }
        if (!busy0) busy0 = now_ns();
        const size_t off = (size_t)(r & mask), tail = cap - off;
        const Packet* p = reinterpret_cast<const Packet*>(base + off);
        if (tail < sizeof(Packet) || !p->run) {   // (the ring's tail: the next packet is at its start)
            r += tail;
            R.store(r, std::memory_order_release);
            continue;
        }
        p->run(p);
        const size_t len = (size_t)(reinterpret_cast<const uint8_t*>(p->next) - reinterpret_cast<const uint8_t*>(p));
        if (poison) std::memset(base + off, 0xCD, len);
        r += len, n++;
        R.store(r, std::memory_order_release);
    }
}
inline void Recorder::log_stats(double secs) {
    uint64_t n[B_COUNT], ns[B_COUNT], mx[B_COUNT], tot_n = 0, tot_ns = 0, max_ns = 0;
    for (int k = 0; k < B_COUNT; k++)
        n[k] = wait_n[k].exchange(0, std::memory_order_relaxed), ns[k] = wait_ns[k].exchange(0, std::memory_order_relaxed),
        mx[k] = wait_max[k].exchange(0, std::memory_order_relaxed), tot_n += n[k], tot_ns += ns[k], max_ns = std::max(max_ns, mx[k]);
    const uint64_t spun = spin_ns.exchange(0, std::memory_order_relaxed), slept = sleeps.exchange(0, std::memory_order_relaxed);
    const uint64_t short_slept = short_sleeps.exchange(0, std::memory_order_relaxed);
    const uint64_t pk = packets.exchange(0, std::memory_order_relaxed), busy = busy_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t dr = direct.exchange(0, std::memory_order_relaxed);
    if (!pk && !tot_n && !dr) return;
    char top[512];
    size_t at = 0;
    top[0] = 0;
    for (int shown = 0; shown < 6; shown++) {   // (the six it waited longest at)
        int best = -1;
        for (int k = 0; k < B_COUNT; k++)
            if (n[k] && (best < 0 || ns[k] > ns[best])) best = k;
        if (best < 0) break;
        const int c = std::snprintf(top + at, sizeof top - at, "%s %s %llu/%.2f ms (max %.2f)", shown ? "," : ";", bound_name(best),
                                    (unsigned long long)n[best], ns[best] / 1e6, mx[best] / 1e6);
        if (c < 0 || (size_t)c >= sizeof top - at) break;
        at += (size_t)c;
        n[best] = 0;
    }
    ::mkwvk::logf("vk: recorder: %llu packets run in %.1f s (busy %.2f s, spun idle %.2f s, slept %llu times, %llu of them under 200 "
                  "us), %llu calls made directly (no room); the replay waited for it %llu times, %.2f ms in all, the longest %.2f ms%s",
                  (unsigned long long)pk, secs, busy / 1e9, spun / 1e9, (unsigned long long)slept, (unsigned long long)short_slept,
                  (unsigned long long)dr, (unsigned long long)tot_n, tot_ns / 1e6, max_ns / 1e6, top);
}

/// (THREAD) The boundary calls the producer makes without waiting for the recorder: only a call that touches no command buffer or
/// pool and can read or change nothing a command still in the ring could do. The ring holds vkCmd* recordings only, and every
/// submission (vkQueueSubmit2, vkQueueBindSparse) waits for it: what has been submitted when such a call runs is what it would be
/// with the recorder off. Each case's reason is beside it;
/// tests/test_recorder_sync.py checks each listed call runs with packets in flight and every other one waits, and that adding
/// vkQueueSubmit2, vkDestroyBuffer or vkEndCommandBuffer here fails it
constexpr bool unordered(Bound k) {
    switch (k) {
    case B_vkGetSemaphoreCounterValue: return true;   // (a timeline's value: signalled only by submissions, each made after the ring is empty)
    case B_vkWaitSemaphores: return true;   // (a timeline's value: signalled only by submissions, each made after the ring is empty, or by
                                            // the driver itself: vk_reflex.cpp's latency-sleep semaphore, which no recorded command signals)
    case B_vkGetQueueCheckpointDataNV: return true;   // (what the device ran of submitted work, after a loss)
    case B_vkGetDeviceFaultInfoEXT: return true;   // (what the device ran of submitted work, after a loss)
    default: return false;   // (unordered: the end of the list)
    }
}

/// Before a call that begins, ends or resets command buffers, submits, presents or waits, reads back what the GPU did, or lets go of
/// what recorded commands use (vk_work.h's second list), and when the replay's lock is let go: the thread's held packets reach the
/// driver first. DEFER: drained; THREAD: on the producer, waits until the recorder has run all it published (vkDestroyDevice and
/// vkDestroyInstance: the recorder stopped and joined first); otherwise a relaxed load and a branch
inline void boundary(Bound k) {
    const int m = g_mode.load(std::memory_order_relaxed);
    if (m == DEFER) {
        drain();
    } else if (m == THREAD) {
        if (k == B_vkDestroyDevice || k == B_vkDestroyInstance) g_rec.stop();
        else if (unordered(k)) return;
        else if (Recorder* g = t_prod) g->wait_idle(k);
    }
}

/// A std::mutex whose unlock drains the thread's held packets first (vk_replay.cpp's replay_mu): what a thread recorded into the
/// replay's command buffers reaches the driver before another thread may record into them. THREAD: the holder is the ring's
/// producer; the unlock waits for the recorder to run all it published, so the recorder is idle whenever no thread holds it
struct DrainingMutex {
    std::mutex m;
    void lock() {
        m.lock();
        enter();
    }
    bool try_lock() {
        if (!m.try_lock()) return false;
        enter();
        return true;
    }
    void unlock() {
        boundary(B_REPLAY_UNLOCK);
        t_prod = nullptr;
        m.unlock();
    }
    static void enter() {
        if (mode() == THREAD) t_prod = &g_rec;
    }
};

/// (for tests) a mode, or back; what is held is run first (THREAD: what was published)
inline void set_mode(int m) {
    drain();
    if (Recorder* g = t_prod) g->flush();
    if (g_rec.ready.load()) {
        const uint64_t target = g_rec.W.load(std::memory_order_acquire);
        g_rec.spin([&] { return g_rec.idle_at(target); });
    }
    g_mode.store(m, std::memory_order_relaxed);
}

/// THREAD: the recorder has run what this thread may depend on (the producer: all it published; another thread: what was published)
inline void settle(Bound k) {
    if (Recorder* g = t_prod) g->wait_idle(k);
    else g_rec.foreign_wait();
}

/// A call that is not packed (a pNext chain, an unknown descriptor type): what is held goes first, then the call itself
template <class F>
inline void fallback(const F& direct) {
    if (g_mode.load(std::memory_order_relaxed) == THREAD) settle(B_FALLBACK);
    else drain();
    t_arena.fallbacks++;
    direct();
}

template <class F>
void thunk(const Packet* p) {
    (*reinterpret_cast<const F*>(p + 1))();
}
template <class F>
void thunk_at(const Packet* p) {   // (THREAD: the closure at its offset in the packet, after the copied arrays)
    (*reinterpret_cast<const F*>(reinterpret_cast<const uint8_t*>(p) + p->size))();
}

/// One packet being encoded: its copies, then emit() with the closure that makes the call and the direct call. Sync: run at emit, the
/// arena rewound when the encoder goes out of scope. Defer: held; drained first when the held bytes reached the soft budget, and
/// made at once (after a drain) while a parallel replay records (par_active: never held then). An allocation that fails (the hard
/// budget, the table, the system) makes emit drain what is held, in order, then make the direct call: no call lost or reordered
///
/// THREAD, on the producer (the replay lock's holder): the packet is written straight into the ring (header first, then the copies,
/// then the closure) and published at emit; no room for it (bigger than the room a packet is given) or no ring: the producer waits
/// until the recorder has run everything published, then makes the call directly. On another thread, or while par_active: the
/// call made directly (PASS), after what was published has run
inline uint8_t* align_up(uint8_t* p, size_t a) { return reinterpret_cast<uint8_t*>(((uintptr_t)p + a - 1) & ~(uintptr_t)(a - 1)); }
struct Enc {
    int md = mode();
    Arena* a = nullptr;   // (SYNC, DEFER)
    Arena::Mark m{};
    bool failed = false;
    Recorder* g = nullptr;   // (THREAD) the ring, its packet's header, where the copies go and how far they may
    Packet* hdr = nullptr;
    uint8_t *cur = nullptr, *lim = nullptr;

    Enc() {
        if (md == THREAD) {
            begin_thread();
            return;
        }
        a = &t_arena;
        if (md == DEFER) {
            if (par_active.load(std::memory_order_relaxed)) {
                drain();
                md = SYNC;
            } else if (a->held >= g_soft_budget) {
                drain();
                a->budget_drains++;
            }
        }
        m = a->mark();
    }
    void begin_thread() {
        g = t_prod;
        if (!g || par_active.load(std::memory_order_relaxed)) [[unlikely]] {
            settle(B_PAR);
            md = PASS, failed = true;
            return;
        }
        if (!g->ready.load(std::memory_order_relaxed) && !g->start()) [[unlikely]] {
            md = PASS, failed = true;
            return;
        }
        hdr = g->reserve(lim);
        cur = reinterpret_cast<uint8_t*>(hdr + 1);
    }
    template <class T>
    T* alloc(size_t n) {
        if (failed) return nullptr;
        if (md == THREAD) {
            uint8_t* at = align_up(cur, alignof(T) < 8 ? 8 : alignof(T));
            if (at > lim || (size_t)(lim - at) < sizeof(T) * n) {
                failed = true;
                return nullptr;
            }
            cur = at + sizeof(T) * n;
            return reinterpret_cast<T*>(at);
        }
        T* p = static_cast<T*>(a->alloc(sizeof(T) * n, alignof(T) < 8 ? 8 : alignof(T)));
        if (!p) failed = true;
        return p;
    }
    template <class T>
    const T* dup(const T* p, size_t n) {
        if (!p || !n) return nullptr;
        T* d = alloc<T>(n);
        if (!d) return nullptr;
        std::memcpy(static_cast<void*>(d), p, sizeof(T) * n);
        return d;
    }
    const void* dup_bytes(const void* p, size_t n) { return dup(static_cast<const uint8_t*>(p), n); }

    template <class F, class D>
    void emit(Op op, const F& f, const D& direct) {
        static_assert(std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F>, "a packet holds values only");
        static_assert(alignof(F) <= alignof(Packet), "a closure aligned beyond the header");
        if (md == THREAD || md == PASS) {
            emit_thread(op, f, direct);
            return;
        }
        Packet* p = failed ? nullptr : static_cast<Packet*>(a->alloc(sizeof(Packet) + sizeof(F), alignof(Packet)));
        if (!p) {
            const size_t held = a->held;
            if (md == DEFER) drain();   // (what is held first, in order; this call's partial copies let go with it)
            else a->rewind(m);
            if (a->failures++ == 0)
                ::mkwvk::logf("vk: RX_VK_RECORDER: no arena memory for %s (%zu bytes held, %zu in chunks, hard budget %zu): the "
                              "held packets drained, the call made directly (logged once a thread)",
                              op_name(op), held, a->capacity, g_hard_budget);
            direct();
            return;
        }
        p->run = &thunk<F>, p->next = nullptr, p->op = op, p->size = (uint32_t)sizeof(F);
        new (static_cast<void*>(p + 1)) F(f);
        a->packets++;
        if (md == SYNC) {
            execute(p);
        } else {
            *a->tail = p;
            a->tail = &p->next;
        }
    }
    template <class F, class D>
    void emit_thread(Op op, const F& f, const D& direct) {
        uint8_t* at = failed ? nullptr : align_up(cur, alignof(Packet));
        if (md == PASS) {
            direct();
            return;
        }
        if (!at || at > lim || (size_t)(lim - at) < sizeof(F)) [[unlikely]] {   // (nothing published: the recorder idle, then direct)
            g->wait_idle(B_NOMEM);
            g->direct.fetch_add(1, std::memory_order_relaxed);
            if (!g->logged_direct.exchange(true, std::memory_order_relaxed))
                ::mkwvk::logf("vk: RX_VK_RECORDER=thread: %s did not fit in the room a packet is given (%zu bytes): made directly once "
                              "the recorder had run what was before it (logged once)",
                              op_name(op), g->room);
            direct();
            return;
        }
        new (static_cast<void*>(at)) F(f);
        hdr->run = &thunk_at<F>, hdr->op = op, hdr->size = (uint32_t)(at - reinterpret_cast<uint8_t*>(hdr));
        uint8_t* end = align_up(at + sizeof(F), alignof(Packet));
        hdr->next = reinterpret_cast<Packet*>(end);
        g->publish(hdr, end);
    }
    ~Enc() {
        if (md == SYNC) a->rewind(m);
    }
};

// ---- The wrapper each vk_work.h macro calls: the driver's function when the recorder is off (the branch is all the off path costs: the
// packer is a function of its own, out of line, so the caller's code stays as it was), else the call's packer
#if defined(__GNUC__) || defined(__clang__)
#define RX_REC_PACKER __attribute__((noinline)) inline
#else
#define RX_REC_PACKER __declspec(noinline) inline
#endif
template <auto& F, auto& PK, class Sig = std::remove_cv_t<std::remove_reference_t<decltype(F)>>>
struct Route;
template <auto& F, auto& PK, class... P>
struct Route<F, PK, void(VKAPI_PTR*)(P...)> {
    static void call(P... p) {
        if (mode() == OFF) [[likely]]
            return F(p...);
        PK(p...);
    }
};

// ---- V: value arguments only. The closure holds them as the call's own parameter types (converted at the wrapper, as the direct call
// converts them). A pointer to const data here would be a pointer kept past the call: refused at compile time (handles are pointers to
// non-const opaque types and pass)
template <auto& F, Op OP, class Sig = std::remove_cv_t<std::remove_reference_t<decltype(F)>>>
struct Value;
template <auto& F, Op OP, class... P>
struct Value<F, OP, void(VKAPI_PTR*)(P...)> {
    static_assert(((!std::is_pointer_v<P> || !std::is_const_v<std::remove_pointer_t<P>>) && ...),
                  "a call with a pointer to data needs a packer of its own");
    RX_REC_PACKER static void pack(P... p) {
        Enc e;
        const auto call = [=] { F(p...); };
        e.emit(OP, call, call);
    }
};
#define RX_REC_VALUE(f) inline constexpr auto& rx_##f = Route<::vkx::f, Value<::vkx::f, OP_##f>::pack>::call;
#define RX_REC_SKIP(f)
RX_REC_CALLS(RX_REC_VALUE, RX_REC_SKIP)
#undef RX_REC_VALUE
#undef RX_REC_SKIP

// ---- A: the calls with arrays and structs behind pointers. Each copies what the driver reads, by the counts the driver reads by
template <class T>
inline bool chained(const T* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (p[i].pNext) return true;
    return false;
}

RX_REC_PACKER void pk_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    if (di->pNext || chained(di->pMemoryBarriers, di->memoryBarrierCount) ||
        chained(di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount) ||
        chained(di->pImageMemoryBarriers, di->imageMemoryBarrierCount))
        return fallback([&] { ::vkx::vkCmdPipelineBarrier2(cb, di); });
    Enc e;
    VkDependencyInfo c = *di;
    c.pMemoryBarriers = e.dup(di->pMemoryBarriers, di->memoryBarrierCount);
    c.pBufferMemoryBarriers = e.dup(di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount);
    c.pImageMemoryBarriers = e.dup(di->pImageMemoryBarriers, di->imageMemoryBarrierCount);
    e.emit(OP_vkCmdPipelineBarrier2, [=] { ::vkx::vkCmdPipelineBarrier2(cb, &c); }, [&] { ::vkx::vkCmdPipelineBarrier2(cb, di); });
}
inline constexpr auto& rx_vkCmdPipelineBarrier2 = Route<::vkx::vkCmdPipelineBarrier2, pk_vkCmdPipelineBarrier2>::call;

RX_REC_PACKER void pk_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    if (ri->pNext || chained(ri->pColorAttachments, ri->colorAttachmentCount) || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) ||
        (ri->pStencilAttachment && ri->pStencilAttachment->pNext))
        return fallback([&] { ::vkx::vkCmdBeginRendering(cb, ri); });
    Enc e;
    VkRenderingInfo c = *ri;
    c.pColorAttachments = e.dup(ri->pColorAttachments, ri->colorAttachmentCount);
    c.pDepthAttachment = e.dup(ri->pDepthAttachment, 1);
    c.pStencilAttachment = e.dup(ri->pStencilAttachment, 1);
    e.emit(OP_vkCmdBeginRendering, [=] { ::vkx::vkCmdBeginRendering(cb, &c); }, [&] { ::vkx::vkCmdBeginRendering(cb, ri); });
}
inline constexpr auto& rx_vkCmdBeginRendering = Route<::vkx::vkCmdBeginRendering, pk_vkCmdBeginRendering>::call;

RX_REC_PACKER void pk_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    Enc e;
    const VkBufferCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyBuffer, [=] { ::vkx::vkCmdCopyBuffer(cb, src, dst, n, c); }, [&] { ::vkx::vkCmdCopyBuffer(cb, src, dst, n, r); });
}
inline constexpr auto& rx_vkCmdCopyBuffer = Route<::vkx::vkCmdCopyBuffer, pk_vkCmdCopyBuffer>::call;

RX_REC_PACKER void pk_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t n,
                                      const VkBufferImageCopy* r) {
    Enc e;
    const VkBufferImageCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyBufferToImage, [=] { ::vkx::vkCmdCopyBufferToImage(cb, src, dst, layout, n, c); },
           [&] { ::vkx::vkCmdCopyBufferToImage(cb, src, dst, layout, n, r); });
}
inline constexpr auto& rx_vkCmdCopyBufferToImage = Route<::vkx::vkCmdCopyBufferToImage, pk_vkCmdCopyBufferToImage>::call;

RX_REC_PACKER void pk_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst, uint32_t n,
                                      const VkBufferImageCopy* r) {
    Enc e;
    const VkBufferImageCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyImageToBuffer, [=] { ::vkx::vkCmdCopyImageToBuffer(cb, src, layout, dst, n, c); },
           [&] { ::vkx::vkCmdCopyImageToBuffer(cb, src, layout, dst, n, r); });
}
inline constexpr auto& rx_vkCmdCopyImageToBuffer = Route<::vkx::vkCmdCopyImageToBuffer, pk_vkCmdCopyImageToBuffer>::call;

RX_REC_PACKER void pk_vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                              const VkImageCopy* r) {
    Enc e;
    const VkImageCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyImage, [=] { ::vkx::vkCmdCopyImage(cb, src, sl, dst, dl, n, c); },
           [&] { ::vkx::vkCmdCopyImage(cb, src, sl, dst, dl, n, r); });
}
inline constexpr auto& rx_vkCmdCopyImage = Route<::vkx::vkCmdCopyImage, pk_vkCmdCopyImage>::call;

RX_REC_PACKER void pk_vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                              const VkImageBlit* r, VkFilter filter) {
    Enc e;
    const VkImageBlit* c = e.dup(r, n);
    e.emit(OP_vkCmdBlitImage, [=] { ::vkx::vkCmdBlitImage(cb, src, sl, dst, dl, n, c, filter); },
           [&] { ::vkx::vkCmdBlitImage(cb, src, sl, dst, dl, n, r, filter); });
}
inline constexpr auto& rx_vkCmdBlitImage = Route<::vkx::vkCmdBlitImage, pk_vkCmdBlitImage>::call;

RX_REC_PACKER void pk_vkCmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                 const VkImageResolve* r) {
    Enc e;
    const VkImageResolve* c = e.dup(r, n);
    e.emit(OP_vkCmdResolveImage, [=] { ::vkx::vkCmdResolveImage(cb, src, sl, dst, dl, n, c); },
           [&] { ::vkx::vkCmdResolveImage(cb, src, sl, dst, dl, n, r); });
}
inline constexpr auto& rx_vkCmdResolveImage = Route<::vkx::vkCmdResolveImage, pk_vkCmdResolveImage>::call;

RX_REC_PACKER void pk_vkCmdClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearColorValue* color, uint32_t n,
                                    const VkImageSubresourceRange* ranges) {
    Enc e;
    const VkClearColorValue* col = e.dup(color, 1);
    const VkImageSubresourceRange* c = e.dup(ranges, n);
    e.emit(OP_vkCmdClearColorImage, [=] { ::vkx::vkCmdClearColorImage(cb, image, layout, col, n, c); },
           [&] { ::vkx::vkCmdClearColorImage(cb, image, layout, color, n, ranges); });
}
inline constexpr auto& rx_vkCmdClearColorImage = Route<::vkx::vkCmdClearColorImage, pk_vkCmdClearColorImage>::call;

RX_REC_PACKER void pk_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue* ds,
                                           uint32_t n, const VkImageSubresourceRange* ranges) {
    Enc e;
    const VkClearDepthStencilValue* v = e.dup(ds, 1);
    const VkImageSubresourceRange* c = e.dup(ranges, n);
    e.emit(OP_vkCmdClearDepthStencilImage, [=] { ::vkx::vkCmdClearDepthStencilImage(cb, image, layout, v, n, c); },
           [&] { ::vkx::vkCmdClearDepthStencilImage(cb, image, layout, ds, n, ranges); });
}
inline constexpr auto& rx_vkCmdClearDepthStencilImage = Route<::vkx::vkCmdClearDepthStencilImage, pk_vkCmdClearDepthStencilImage>::call;

RX_REC_PACKER void pk_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* att, uint32_t nr, const VkClearRect* rects) {
    Enc e;
    const VkClearAttachment* a = e.dup(att, na);
    const VkClearRect* r = e.dup(rects, nr);
    e.emit(OP_vkCmdClearAttachments, [=] { ::vkx::vkCmdClearAttachments(cb, na, a, nr, r); },
           [&] { ::vkx::vkCmdClearAttachments(cb, na, att, nr, rects); });
}
inline constexpr auto& rx_vkCmdClearAttachments = Route<::vkx::vkCmdClearAttachments, pk_vkCmdClearAttachments>::call;

RX_REC_PACKER void pk_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void* data) {
    Enc e;
    const void* d = e.dup_bytes(data, (size_t)size);
    e.emit(OP_vkCmdUpdateBuffer, [=] { ::vkx::vkCmdUpdateBuffer(cb, dst, offset, size, d); },
           [&] { ::vkx::vkCmdUpdateBuffer(cb, dst, offset, size, data); });
}
inline constexpr auto& rx_vkCmdUpdateBuffer = Route<::vkx::vkCmdUpdateBuffer, pk_vkCmdUpdateBuffer>::call;

RX_REC_PACKER void pk_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                                       const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    Enc e;
    const VkDescriptorSet* s = e.dup(sets, n);
    const uint32_t* d = e.dup(dyn, nd);
    e.emit(OP_vkCmdBindDescriptorSets, [=] { ::vkx::vkCmdBindDescriptorSets(cb, bp, layout, first, n, s, nd, d); },
           [&] { ::vkx::vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, nd, dyn); });
}
inline constexpr auto& rx_vkCmdBindDescriptorSets = Route<::vkx::vkCmdBindDescriptorSets, pk_vkCmdBindDescriptorSets>::call;

RX_REC_PACKER void pk_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* buffers, const VkDeviceSize* offsets,
                                       const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    Enc e;
    const VkBuffer* b = e.dup(buffers, n);
    const VkDeviceSize* o = e.dup(offsets, n);
    const VkDeviceSize* s = e.dup(sizes, n);     // (null stays null: the whole buffer)
    const VkDeviceSize* st = e.dup(strides, n);  // (null stays null: the pipeline's strides)
    e.emit(OP_vkCmdBindVertexBuffers2, [=] { ::vkx::vkCmdBindVertexBuffers2(cb, first, n, b, o, s, st); },
           [&] { ::vkx::vkCmdBindVertexBuffers2(cb, first, n, buffers, offsets, sizes, strides); });
}
inline constexpr auto& rx_vkCmdBindVertexBuffers2 = Route<::vkx::vkCmdBindVertexBuffers2, pk_vkCmdBindVertexBuffers2>::call;

RX_REC_PACKER void pk_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                  const void* values) {
    Enc e;
    const void* v = e.dup_bytes(values, size);
    e.emit(OP_vkCmdPushConstants, [=] { ::vkx::vkCmdPushConstants(cb, layout, stages, offset, size, v); },
           [&] { ::vkx::vkCmdPushConstants(cb, layout, stages, offset, size, values); });
}
inline constexpr auto& rx_vkCmdPushConstants = Route<::vkx::vkCmdPushConstants, pk_vkCmdPushConstants>::call;

/// Which of a write's three arrays the driver reads, by its type (the other two are ignored, and left null in the copy); 0: a type
/// the packer does not know (inline uniform blocks, acceleration structures: their data is in pNext)
inline int write_kind(VkDescriptorType t) {
    switch (t) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return 1;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return 2;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 3;
    default: return 0;
    }
}

RX_REC_PACKER void pk_vkCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set, uint32_t n,
                                         const VkWriteDescriptorSet* w) {
    for (uint32_t i = 0; i < n; i++)
        if (w[i].pNext || !write_kind(w[i].descriptorType))
            return fallback([&] { ::vkx::vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, w); });
    Enc e;
    VkWriteDescriptorSet* c = n ? e.alloc<VkWriteDescriptorSet>(n) : nullptr;
    for (uint32_t i = 0; c && i < n; i++) {   // (no memory: emit makes the direct call)
        c[i] = w[i];
        c[i].pImageInfo = nullptr, c[i].pBufferInfo = nullptr, c[i].pTexelBufferView = nullptr;
        switch (write_kind(w[i].descriptorType)) {
        case 1: c[i].pImageInfo = e.dup(w[i].pImageInfo, w[i].descriptorCount); break;
        case 2: c[i].pBufferInfo = e.dup(w[i].pBufferInfo, w[i].descriptorCount); break;
        default: c[i].pTexelBufferView = e.dup(w[i].pTexelBufferView, w[i].descriptorCount); break;
        }
    }
    e.emit(OP_vkCmdPushDescriptorSetKHR, [=] { ::vkx::vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, c); },
           [&] { ::vkx::vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, w); });
}
inline constexpr auto& rx_vkCmdPushDescriptorSetKHR = Route<::vkx::vkCmdPushDescriptorSetKHR, pk_vkCmdPushDescriptorSetKHR>::call;

RX_REC_PACKER void pk_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    Enc e;
    const VkViewport* c = e.dup(v, n);
    e.emit(OP_vkCmdSetViewport, [=] { ::vkx::vkCmdSetViewport(cb, first, n, c); }, [&] { ::vkx::vkCmdSetViewport(cb, first, n, v); });
}
inline constexpr auto& rx_vkCmdSetViewport = Route<::vkx::vkCmdSetViewport, pk_vkCmdSetViewport>::call;

RX_REC_PACKER void pk_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    Enc e;
    const VkRect2D* c = e.dup(r, n);
    e.emit(OP_vkCmdSetScissor, [=] { ::vkx::vkCmdSetScissor(cb, first, n, c); }, [&] { ::vkx::vkCmdSetScissor(cb, first, n, r); });
}
inline constexpr auto& rx_vkCmdSetScissor = Route<::vkx::vkCmdSetScissor, pk_vkCmdSetScissor>::call;

RX_REC_PACKER void pk_vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport* v) {
    Enc e;
    const VkViewport* c = e.dup(v, n);
    e.emit(OP_vkCmdSetViewportWithCount, [=] { ::vkx::vkCmdSetViewportWithCount(cb, n, c); },
           [&] { ::vkx::vkCmdSetViewportWithCount(cb, n, v); });
}
inline constexpr auto& rx_vkCmdSetViewportWithCount = Route<::vkx::vkCmdSetViewportWithCount, pk_vkCmdSetViewportWithCount>::call;

RX_REC_PACKER void pk_vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D* r) {
    Enc e;
    const VkRect2D* c = e.dup(r, n);
    e.emit(OP_vkCmdSetScissorWithCount, [=] { ::vkx::vkCmdSetScissorWithCount(cb, n, c); },
           [&] { ::vkx::vkCmdSetScissorWithCount(cb, n, r); });
}
inline constexpr auto& rx_vkCmdSetScissorWithCount = Route<::vkx::vkCmdSetScissorWithCount, pk_vkCmdSetScissorWithCount>::call;

RX_REC_PACKER void pk_vkCmdSetBlendConstants(VkCommandBuffer cb, const float blend[4]) {
    Enc e;
    const float* c = e.dup(blend, 4);
    e.emit(OP_vkCmdSetBlendConstants, [=] { ::vkx::vkCmdSetBlendConstants(cb, c); }, [&] { ::vkx::vkCmdSetBlendConstants(cb, blend); });
}
inline constexpr auto& rx_vkCmdSetBlendConstants = Route<::vkx::vkCmdSetBlendConstants, pk_vkCmdSetBlendConstants>::call;

RX_REC_PACKER void pk_vkCmdSetColorBlendEnableEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* en) {
    Enc e;
    const VkBool32* c = e.dup(en, n);
    e.emit(OP_vkCmdSetColorBlendEnableEXT, [=] { ::vkx::vkCmdSetColorBlendEnableEXT(cb, first, n, c); },
           [&] { ::vkx::vkCmdSetColorBlendEnableEXT(cb, first, n, en); });
}
inline constexpr auto& rx_vkCmdSetColorBlendEnableEXT = Route<::vkx::vkCmdSetColorBlendEnableEXT, pk_vkCmdSetColorBlendEnableEXT>::call;

RX_REC_PACKER void pk_vkCmdSetColorBlendEquationEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* eq) {
    Enc e;
    const VkColorBlendEquationEXT* c = e.dup(eq, n);
    e.emit(OP_vkCmdSetColorBlendEquationEXT, [=] { ::vkx::vkCmdSetColorBlendEquationEXT(cb, first, n, c); },
           [&] { ::vkx::vkCmdSetColorBlendEquationEXT(cb, first, n, eq); });
}
inline constexpr auto& rx_vkCmdSetColorBlendEquationEXT = Route<::vkx::vkCmdSetColorBlendEquationEXT, pk_vkCmdSetColorBlendEquationEXT>::call;

RX_REC_PACKER void pk_vkCmdSetColorWriteMaskEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* wm) {
    Enc e;
    const VkColorComponentFlags* c = e.dup(wm, n);
    e.emit(OP_vkCmdSetColorWriteMaskEXT, [=] { ::vkx::vkCmdSetColorWriteMaskEXT(cb, first, n, c); },
           [&] { ::vkx::vkCmdSetColorWriteMaskEXT(cb, first, n, wm); });
}
inline constexpr auto& rx_vkCmdSetColorWriteMaskEXT = Route<::vkx::vkCmdSetColorWriteMaskEXT, pk_vkCmdSetColorWriteMaskEXT>::call;

/// (the marker is an opaque value the driver hands back, never read through: kept as the pointer value)
RX_REC_PACKER void pk_vkCmdSetCheckpointNV(VkCommandBuffer cb, const void* marker) {
    Enc e;
    e.emit(OP_vkCmdSetCheckpointNV, [=] { ::vkx::vkCmdSetCheckpointNV(cb, marker); }, [&] { ::vkx::vkCmdSetCheckpointNV(cb, marker); });
}
inline constexpr auto& rx_vkCmdSetCheckpointNV = Route<::vkx::vkCmdSetCheckpointNV, pk_vkCmdSetCheckpointNV>::call;

} // namespace mkwvk::rec
