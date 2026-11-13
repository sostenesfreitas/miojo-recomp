// RX_VK_RECORDER's packets against a fake Vulkan (tests/test_recorder_sync.py builds it with the real vk_work.h and vk_recorder.h).
// Every wrapped vkCmd* goes to a fake that logs the call with its arguments fully dereferenced, the way a driver reads them (by the
// counts and descriptor types, the pointers themselves never), so two logs are equal exactly when the driver was handed the same
// values. A frame of representative calls is made the way the renderer makes them (thread-local descriptor writes reset at every draw,
// push constants and vertex arrays on the stack, a rendering info with attachments on the stack, barriers, a reused copy vector,
// dynamic state arrays), and with `mutate` the caller poisons (0xCD) all of that right after each call, as the next draw reuses it.
//
//   recorder_sync_test            the checks; exits 1 on the first difference
//   recorder_sync_test --bench N  the packet cost per draw on the fake (fakes that do nothing), ns, median of 5
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "vk_work.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int);   // (a fault exits, without a dialog)
extern "C" __declspec(dllimport) int __stdcall GetThreadTimes(void*, void*, void*, void*, void*);   // (--idle: the recorder's CPU)
#endif

namespace mkwvk {
std::atomic<uint64_t> work_epoch{0};
std::atomic<bool> par_active{false};
void logf(const char* fmt, ...) {   // (the renderer's log: printed, for test_recorder_sync.py to count)
    char b[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::printf("LOG: %s\n", b);
}
} // namespace mkwvk

// ---- the arena's aligned blocks: counted (a block lost is a leak), and the Nth one made to fail (g_fail_nth: an allocation failure;
// the throwing form throws std::bad_alloc as the system's does)
static std::atomic<long> g_aligned_live{0}, g_aligned_calls{0}, g_fail_nth{0};
static bool aligned_fails() { return g_fail_nth && ++g_aligned_calls == g_fail_nth; }
static void* aligned_get(size_t n, std::align_val_t al) {
#ifdef _WIN32
    void* p = _aligned_malloc(n, (size_t)al);
#else
    void* p = nullptr;
    if (posix_memalign(&p, (size_t)al, n)) p = nullptr;
#endif
    if (p) g_aligned_live++;
    return p;
}
static void aligned_put(void* p) {
    if (!p) return;
    g_aligned_live--;
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}
void* operator new(size_t n, std::align_val_t al) {
    void* p = aligned_fails() ? nullptr : aligned_get(n, al);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n, std::align_val_t al) { return operator new(n, al); }
void* operator new(size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return aligned_fails() ? nullptr : aligned_get(n, al); }
void* operator new[](size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return aligned_fails() ? nullptr : aligned_get(n, al); }
void operator delete(void* p, std::align_val_t) noexcept { aligned_put(p); }
void operator delete[](void* p, std::align_val_t) noexcept { aligned_put(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { aligned_put(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { aligned_put(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { aligned_put(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { aligned_put(p); }
using namespace mkwvk;

// ---- the fake driver's log (RX_VK_RECORDER=thread: the recorder thread and the producer both call the fake: one lock for the log,
// and a command buffer's in-use flag held across each call, so two calls on one command buffer at once are seen, not serialized)
struct Call {
    std::string op, bytes;
};
static std::vector<Call> g_log;
static std::mutex g_log_mu;
static bool g_quiet = false;   // (bench: the fakes only count)
static uint64_t g_calls = 0;
static int g_spin_ns = 0;      // (bench --overlap: the time a quiet fake vkCmd* takes, as the driver's encoding does)
static std::atomic<int> g_delay_ns{0};   // (thread runs: each logged vkCmd* takes this long, so the recorder lags the producer)
static std::atomic<bool> g_gate{false};   // (B_DIRECT control: vkCmd* on other threads wait until it is let go)
// (status reads: vkCmd* on other threads also wait until this steady-clock time, so the recorder cannot catch up before the read
// however the runner schedules the threads; a read that waits for the recorder is let through when it passes)
static std::atomic<int64_t> g_hold_until_ns{0};
static bool held_until() {
    const int64_t until = g_hold_until_ns.load();
    return until && std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() < until;
}
// (thread runs of the ordering scenarios: hold() before each point where the producer must wait for the recorder (a recording's
// end, a submit while another command buffer records, the replay lock let go, a pool reset) holds the recorder for this long, so its
// commands are still in flight there whatever the runner's load: a producer that does not wait is caught every time, not only when
// the 1 us a call happened to keep the recorder behind (the flakes of the broken variants on CI). 0: no hold (the reference runs)
static int g_hold_ms = 0;
static void hold() {
    if (!g_hold_ms) return;
    g_hold_until_ns = (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() +
                      (int64_t)g_hold_ms * 1000000;
}
static bool g_hash_only = false;         // (stress: each call folded into g_hash and dropped)
static uint64_t g_hash = 0xcbf29ce484222325ull;
static std::thread::id g_main_id;
static std::atomic<uint64_t> g_cmd_off_main{0}, g_cmd_calls{0};   // (vkCmd* the fake was given, and those on another thread)
static std::atomic<bool> g_throw{false};   // (--throw: the next vkCmdDraw throws, as a driver call could)
static void spin_for(int ns) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::nanoseconds(ns)) {}
}
static void fold(const Call& c) {
    for (const std::string* str : {&c.op, &c.bytes})
        for (unsigned char ch : *str) g_hash = (g_hash ^ ch) * 0x100000001b3ull;
    g_hash = (g_hash ^ 0xFF) * 0x100000001b3ull;
}

static void begin(const char* op) { g_log.push_back({op, {}}); }
template <class T>
static void put(const T& v) {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "fields one by one: no padding, no pointers");
    g_log.back().bytes.append(reinterpret_cast<const char*>(&v), sizeof v);
}
template <class T>
static void handle(T h) {
    put((uint64_t)(uintptr_t)h);
}
static void raw(const void* p, size_t n) { g_log.back().bytes.append(static_cast<const char*>(p), n); }
static void tag(const char* s) { g_log.back().bytes.append(s).push_back(0); }
// (a pointer or count the caller has since poisoned, which a missed copy hands over: logged as such, never followed)
static bool readable(const void* p, uint64_t n) {
    if ((uintptr_t)p == (uintptr_t)0xCDCDCDCDCDCDCDCDull || n > 4096) {
        tag("UNREADABLE");
        return false;
    }
    return true;
}
template <class T, class F>
static void each(const T* p, uint64_t n, F f) {
    put((uint32_t)n);
    if (!n || !readable(p, n)) return;
    for (uint64_t i = 0; i < n; i++) f(p[i]);
}
template <class T, class F>
static void opt(const T* p, F f) {   // (an optional struct: there or not, then what it holds)
    put((uint8_t)(p != nullptr));
    if (p && readable(p, 1)) f(*p);
}

static void L(const VkImageSubresourceRange& r) { put(r.aspectMask), put(r.baseMipLevel), put(r.levelCount), put(r.baseArrayLayer), put(r.layerCount); }
static void L(const VkImageSubresourceLayers& r) { put(r.aspectMask), put(r.mipLevel), put(r.baseArrayLayer), put(r.layerCount); }
static void L(const VkOffset3D& o) { put(o.x), put(o.y), put(o.z); }
static void L(const VkExtent3D& e) { put(e.width), put(e.height), put(e.depth); }
static void L(const VkRect2D& r) { put(r.offset.x), put(r.offset.y), put(r.extent.width), put(r.extent.height); }
static void L(const VkViewport& v) { put(v.x), put(v.y), put(v.width), put(v.height), put(v.minDepth), put(v.maxDepth); }
static void L(const VkClearValue& v) { raw(&v, sizeof v); }   // (a union of 16 bytes, no padding)
static void L(const VkClearColorValue& v) { raw(&v, sizeof v); }
static void L(const VkClearDepthStencilValue& v) { put(v.depth), put(v.stencil); }
static void L(const VkMemoryBarrier2& b) {
    put(b.sType), put((uint8_t)!!b.pNext), put(b.srcStageMask), put(b.srcAccessMask), put(b.dstStageMask), put(b.dstAccessMask);
}
static void L(const VkBufferMemoryBarrier2& b) {
    put(b.sType), put((uint8_t)!!b.pNext), put(b.srcStageMask), put(b.srcAccessMask), put(b.dstStageMask), put(b.dstAccessMask);
    put(b.srcQueueFamilyIndex), put(b.dstQueueFamilyIndex), handle(b.buffer), put(b.offset), put(b.size);
}
static void L(const VkImageMemoryBarrier2& b) {
    put(b.sType), put((uint8_t)!!b.pNext), put(b.srcStageMask), put(b.srcAccessMask), put(b.dstStageMask), put(b.dstAccessMask);
    put(b.oldLayout), put(b.newLayout), put(b.srcQueueFamilyIndex), put(b.dstQueueFamilyIndex), handle(b.image), L(b.subresourceRange);
}
static void L(const VkRenderingAttachmentInfo& a) {
    put(a.sType), put((uint8_t)!!a.pNext), handle(a.imageView), put(a.imageLayout), put(a.resolveMode), handle(a.resolveImageView);
    put(a.resolveImageLayout), put(a.loadOp), put(a.storeOp), L(a.clearValue);
}
static void L(const VkBufferCopy& c) { put(c.srcOffset), put(c.dstOffset), put(c.size); }
static void L(const VkBufferImageCopy& c) {
    put(c.bufferOffset), put(c.bufferRowLength), put(c.bufferImageHeight), L(c.imageSubresource), L(c.imageOffset), L(c.imageExtent);
}
static void L(const VkImageCopy& c) { L(c.srcSubresource), L(c.srcOffset), L(c.dstSubresource), L(c.dstOffset), L(c.extent); }
static void L(const VkImageResolve& c) { L(c.srcSubresource), L(c.srcOffset), L(c.dstSubresource), L(c.dstOffset), L(c.extent); }
static void L(const VkImageBlit& b) {
    L(b.srcSubresource), L(b.srcOffsets[0]), L(b.srcOffsets[1]), L(b.dstSubresource), L(b.dstOffsets[0]), L(b.dstOffsets[1]);
}
static void L(const VkClearAttachment& a) { put(a.aspectMask), put(a.colorAttachment), L(a.clearValue); }
static void L(const VkClearRect& r) { L(r.rect), put(r.baseArrayLayer), put(r.layerCount); }
static void L(const VkColorBlendEquationEXT& e) {
    put(e.srcColorBlendFactor), put(e.dstColorBlendFactor), put(e.colorBlendOp), put(e.srcAlphaBlendFactor), put(e.dstAlphaBlendFactor),
        put(e.alphaBlendOp);
}
static void L(const VkWriteDescriptorSet& w) {   // (as a driver reads a push: by type, one of the three arrays)
    put(w.sType), put((uint8_t)!!w.pNext), handle(w.dstSet), put(w.dstBinding), put(w.dstArrayElement), put(w.descriptorType);
    switch (rec::write_kind(w.descriptorType)) {
    case 1: each(w.pImageInfo, w.descriptorCount, [](const VkDescriptorImageInfo& i) { handle(i.sampler), handle(i.imageView), put(i.imageLayout); }); break;
    case 2: each(w.pBufferInfo, w.descriptorCount, [](const VkDescriptorBufferInfo& b) { handle(b.buffer), put(b.offset), put(b.range); }); break;
    case 3: each(w.pTexelBufferView, w.descriptorCount, [](VkBufferView v) { handle(v); }); break;
    default: put(w.descriptorCount); break;   // (not read here: the packer leaves these to the direct call)
    }
}
template <class T>
static void A(const T* p, uint64_t n) {
    each(p, n, [](const T& v) { L(v); });
}

#define QUIET \
    if (g_quiet) { g_calls++; if (g_spin_ns) spin_for(g_spin_ns); return; }
// ---- the fake lifecycle (g_track): each command buffer's state as a driver tracks it. A vkCmd* on one that is not recording, a begin
// of one recording, an end of one not recording or a submit of one not executable is a violation; the boundaries are logged as calls
// too, so two logs also compare where the begins, ends, resets, submits, waits, reads and destructions fall among the commands.
// Always: a call on a command buffer (or a reset of its pool, a submit of it) while another call on it is under way is a violation
enum CbState { INITIAL, RECORDING, EXECUTABLE };
static bool g_track = false;
static std::map<uint64_t, CbState> g_state;
static std::map<uint64_t, uint64_t> g_pool_of;   // (command buffer -> its pool)
static std::mutex g_viol_mu;
static int g_violations;
static std::string g_violation;
void violation(const char* op, const void* cb, const char* what) {
    std::lock_guard lk(g_viol_mu);
    if (g_violations++) return;
    char b[240];
    std::snprintf(b, sizeof b, "%s on command buffer %#llx %s (driver call %zu)", op, (unsigned long long)(uintptr_t)cb, what, g_log.size());
    g_violation = b;
}
static CbState& state(const void* cb) { return g_state[(uint64_t)(uintptr_t)cb]; }
static void track(const void* cb, const char* op) {
    if (g_track && state(cb) != RECORDING) violation(op, cb, "not recording");
}
// ---- the fake GPU (g_track): a command buffer's recorded effects (timestamps, checkpoints, fills) take effect when it is submitted,
// as on a device: then the timeline steps and the fence is signalled. The status reads below (fences, semaphores, checkpoints, fault
// data, memory invalidation) see only that, and are logged apart (g_reads), unordered against the vkCmd* stream; the ordered stream
// keeps every other call. Destroyed buffers are remembered: a recorded command naming one is a violation
struct Fx {
    int kind;   // 1 timestamp (a: pool << 16 | query), 2 checkpoint (a: marker), 3 fill (a: buffer, b: data)
    uint64_t a, b;
};
static std::map<uint64_t, std::vector<Fx>> g_cb_fx;
static std::map<uint64_t, uint64_t> g_query, g_mem;
static std::set<uint64_t> g_fences_done, g_destroyed;
static uint64_t g_timeline = 0, g_checkpoint = 0;
static std::vector<uint64_t> g_reads;          // (each status read: the call, what it saw)
static std::map<std::string, uint64_t> g_reads_overlapped;   // (status reads made while the producer had packets not yet run)
static void gpu_reset() {
    g_cb_fx.clear(), g_query.clear(), g_mem.clear(), g_fences_done.clear(), g_destroyed.clear();
    g_timeline = 0, g_checkpoint = 0, g_reads.clear(), g_reads_overlapped.clear();
}
static void fx(const void* cb, int kind, uint64_t a, uint64_t b = 0) {
    if (g_track) g_cb_fx[(uint64_t)(uintptr_t)cb].push_back({kind, a, b});
}
void violation(const char* op, const void* cb, const char* what);
static void alive(const void* cb, const char* op, VkBuffer b) {   // (a recorded command naming a destroyed buffer)
    if (g_track && g_destroyed.count((uint64_t)(uintptr_t)b)) violation(op, cb, "names a destroyed buffer");
}
static std::atomic<bool> g_inuse[256];
static std::atomic<bool>& inuse(uint64_t cb) { return g_inuse[(cb >> 8) & 255]; }   // (the tests' command buffers: 0x100, 0x200, 0x300)
/// One fake call: its command buffer marked in use for the call (a second call on it meanwhile is a violation), a vkCmd* slowed by
/// g_delay_ns, then the log's lock
struct Guard {
    const void* cb;
    bool held = false;
    std::unique_lock<std::mutex> lk;
    Guard(const void* c, const char* op, bool cmd) : cb(c) {
        while (cmd && std::this_thread::get_id() != g_main_id && (g_gate.load() || held_until())) std::this_thread::yield();   // (before the in-use mark)
        if (cb) {
            if (inuse((uint64_t)(uintptr_t)cb).exchange(true)) violation(op, cb, "in use by another thread (no external synchronization)");
            else held = true;
        }
        if (cmd) {
            g_cmd_calls++;
            if (std::this_thread::get_id() != g_main_id) g_cmd_off_main++;
            if (const int d = g_delay_ns.load(std::memory_order_relaxed)) spin_for(d);
        }
        lk = std::unique_lock(g_log_mu);
    }
    ~Guard() {
        if (g_hash_only && !g_log.empty()) fold(g_log.back()), g_log.pop_back();
        lk.unlock();
        if (held) inuse((uint64_t)(uintptr_t)cb).store(false);
    }
};
/// (a pool reset or a submit: its command buffers must not be in use meanwhile)
static void not_in_use(const void* cb, const char* op) {
    if (inuse((uint64_t)(uintptr_t)cb).load()) violation(op, cb, "in use by another thread (no external synchronization)");
}

#define F(name, params, ...) \
    static void VKAPI_CALL fake_##name params { QUIET Guard g_(cb, #name, true); begin(#name); track(cb, #name); __VA_ARGS__ }

F(vkCmdPipelineBarrier2, (VkCommandBuffer cb, const VkDependencyInfo* d), handle(cb); put(d->sType), put((uint8_t)!!d->pNext), put(d->dependencyFlags);
  A(d->pMemoryBarriers, d->memoryBarrierCount), A(d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount);
  A(d->pImageMemoryBarriers, d->imageMemoryBarrierCount);)
F(vkCmdBeginRendering, (VkCommandBuffer cb, const VkRenderingInfo* r), handle(cb); put(r->sType), put((uint8_t)!!r->pNext), put(r->flags);
  L(r->renderArea), put(r->layerCount), put(r->viewMask); A(r->pColorAttachments, r->colorAttachmentCount);
  opt(r->pDepthAttachment, [](const VkRenderingAttachmentInfo& a) { L(a); }); opt(r->pStencilAttachment, [](const VkRenderingAttachmentInfo& a) { L(a); });)
F(vkCmdEndRendering, (VkCommandBuffer cb), handle(cb);)
F(vkCmdDraw, (VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d), handle(cb), put(a), put(b), put(c), put(d);
  if (g_throw.exchange(false)) throw std::runtime_error("a driver call threw (test)");)
F(vkCmdDrawIndexed, (VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e), handle(cb), put(a), put(b), put(c), put(d), put(e);)
F(vkCmdDrawIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), handle(cb), handle(b), put(o), put(n), put(s);)
F(vkCmdDrawIndexedIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), handle(cb), handle(b), put(o), put(n), put(s);)
F(vkCmdDrawIndirectCount, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s),
  handle(cb), handle(b), put(o), handle(cbuf), put(co), put(m), put(s);)
F(vkCmdDrawIndexedIndirectCount, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s),
  handle(cb), handle(b), put(o), handle(cbuf), put(co), put(m), put(s);)
F(vkCmdDrawMeshTasksEXT, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), handle(cb), put(x), put(y), put(z);)
F(vkCmdDrawMeshTasksIndirectEXT, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), handle(cb), handle(b), put(o), put(n), put(s);)
F(vkCmdDrawMeshTasksIndirectCountEXT, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s),
  handle(cb), handle(b), put(o), handle(cbuf), put(co), put(m), put(s);)
F(vkCmdDispatch, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), handle(cb), put(x), put(y), put(z);)
F(vkCmdDispatchIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), handle(cb), handle(b), put(o);)
F(vkCmdCopyBuffer, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r), handle(cb), handle(s), handle(d); A(r, n);
  alive(cb, "vkCmdCopyBuffer", s), alive(cb, "vkCmdCopyBuffer", d);)
F(vkCmdCopyBufferToImage, (VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r),
  handle(cb), handle(s), handle(d), put(l); A(r, n);)
F(vkCmdCopyImageToBuffer, (VkCommandBuffer cb, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy* r),
  handle(cb), handle(s), put(l), handle(d); A(r, n);)
F(vkCmdCopyImage, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r),
  handle(cb), handle(s), put(sl), handle(d), put(dl); A(r, n);)
F(vkCmdBlitImage, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f),
  handle(cb), handle(s), put(sl), handle(d), put(dl); A(r, n); put(f);)
F(vkCmdResolveImage, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve* r),
  handle(cb), handle(s), put(sl), handle(d), put(dl); A(r, n);)
F(vkCmdClearColorImage, (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c, uint32_t n, const VkImageSubresourceRange* r),
  handle(cb), handle(i), put(l); opt(c, [](const VkClearColorValue& v) { L(v); }); A(r, n);)
F(vkCmdClearDepthStencilImage, (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* c, uint32_t n,
  const VkImageSubresourceRange* r), handle(cb), handle(i), put(l); opt(c, [](const VkClearDepthStencilValue& v) { L(v); }); A(r, n);)
F(vkCmdClearAttachments, (VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r),
  handle(cb); A(a, na); A(r, nr);)
F(vkCmdFillBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d), handle(cb), handle(b), put(o), put(s), put(d);
  alive(cb, "vkCmdFillBuffer", b); fx(cb, 3, (uint64_t)(uintptr_t)b, d);)
F(vkCmdUpdateBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void* d), handle(cb), handle(b), put(o), put(s);
  if (readable(d, s / 16)) raw(d, (size_t)s);)
F(vkCmdBeginQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f), handle(cb), handle(p), put(q), put(f);)
F(vkCmdEndQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q), handle(cb), handle(p), put(q);)
F(vkCmdBindPipeline, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p), handle(cb), put(bp), handle(p);)
F(vkCmdBindDescriptorSets, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet* s,
  uint32_t nd, const uint32_t* d), handle(cb), put(bp), handle(l), put(first); each(s, n, [](VkDescriptorSet v) { handle(v); });
  each(d, nd, [](uint32_t v) { put(v); });)
F(vkCmdBindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), handle(cb), handle(b), put(o), put(t);)
F(vkCmdBindVertexBuffers2, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o, const VkDeviceSize* s,
  const VkDeviceSize* st), handle(cb), put(first); each(b, n, [](VkBuffer v) { handle(v); }); each(o, n, [](VkDeviceSize v) { put(v); });
  put((uint8_t)!!s); if (s) each(s, n, [](VkDeviceSize v) { put(v); }); put((uint8_t)!!st); if (st) each(st, n, [](VkDeviceSize v) { put(v); });)
F(vkCmdPushConstants, (VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags f, uint32_t o, uint32_t s, const void* v),
  handle(cb), handle(l), put(f), put(o), put(s); if (readable(v, s / 16)) raw(v, s);)
F(vkCmdPushDescriptorSetKHR, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n, const VkWriteDescriptorSet* w),
  handle(cb), put(bp), handle(l), put(set); A(w, n);)
F(vkCmdSetViewport, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v), handle(cb), put(first); A(v, n);)
F(vkCmdSetScissor, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r), handle(cb), put(first); A(r, n);)
F(vkCmdSetViewportWithCount, (VkCommandBuffer cb, uint32_t n, const VkViewport* v), handle(cb); A(v, n);)
F(vkCmdSetScissorWithCount, (VkCommandBuffer cb, uint32_t n, const VkRect2D* r), handle(cb); A(r, n);)
F(vkCmdSetBlendConstants, (VkCommandBuffer cb, const float b[4]), handle(cb); each(b, 4, [](float v) { put(v); });)
F(vkCmdSetStencilReference, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), handle(cb), put(f), put(v);)
F(vkCmdSetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), handle(cb), put(f), put(v);)
F(vkCmdSetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), handle(cb), put(f), put(v);)
F(vkCmdSetStencilOp, (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp c, VkCompareOp d),
  handle(cb), put(f), put(a), put(b), put(c), put(d);)
F(vkCmdSetDepthBias, (VkCommandBuffer cb, float a, float b, float c), handle(cb), put(a), put(b), put(c);)
F(vkCmdSetDepthBiasEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetLineWidth, (VkCommandBuffer cb, float v), handle(cb), put(v);)
F(vkCmdSetDepthBoundsTestEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetCullMode, (VkCommandBuffer cb, VkCullModeFlags v), handle(cb), put(v);)
F(vkCmdSetFrontFace, (VkCommandBuffer cb, VkFrontFace v), handle(cb), put(v);)
F(vkCmdSetPrimitiveTopology, (VkCommandBuffer cb, VkPrimitiveTopology v), handle(cb), put(v);)
F(vkCmdSetPrimitiveRestartEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetDepthTestEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetDepthWriteEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetDepthCompareOp, (VkCommandBuffer cb, VkCompareOp v), handle(cb), put(v);)
F(vkCmdSetStencilTestEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetRasterizerDiscardEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetPolygonModeEXT, (VkCommandBuffer cb, VkPolygonMode v), handle(cb), put(v);)
F(vkCmdSetDepthClampEnableEXT, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetAlphaToCoverageEnableEXT, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetColorBlendEnableEXT, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* v), handle(cb), put(first);
  each(v, n, [](VkBool32 x) { put(x); });)
F(vkCmdSetColorBlendEquationEXT, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* v), handle(cb), put(first); A(v, n);)
F(vkCmdSetColorWriteMaskEXT, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* v), handle(cb), put(first);
  each(v, n, [](VkColorComponentFlags x) { put(x); });)
F(vkCmdWriteBufferMarkerAMD, (VkCommandBuffer cb, VkPipelineStageFlagBits s, VkBuffer b, VkDeviceSize o, uint32_t m),
  handle(cb), put(s), handle(b), put(o), put(m);)
F(vkCmdSetCheckpointNV, (VkCommandBuffer cb, const void* m), handle(cb), handle(m); fx(cb, 2, (uint64_t)(uintptr_t)m);)   // (an opaque value: the pointer is the datum)
F(vkCmdWriteTimestamp2, (VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool p, uint32_t q), handle(cb), put(s), handle(p), put(q);
  fx(cb, 1, (uint64_t)(uintptr_t)p << 16 | q);)
F(vkCmdResetQueryPool, (VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n), handle(cb), handle(p), put(f), put(n);)

static VkResult VKAPI_CALL fake_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    Guard g_(cb, "vkBeginCommandBuffer", false);
    begin("vkBeginCommandBuffer"), handle(cb), put(bi->flags);
    g_cb_fx.erase((uint64_t)(uintptr_t)cb);
    if (g_track && state(cb) == RECORDING) violation("vkBeginCommandBuffer", cb, "already recording");
    state(cb) = RECORDING;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_vkEndCommandBuffer(VkCommandBuffer cb) {
    Guard g_(cb, "vkEndCommandBuffer", false);
    begin("vkEndCommandBuffer"), handle(cb);
    track(cb, "vkEndCommandBuffer");
    state(cb) = EXECUTABLE;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_vkResetCommandPool(VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags f) {
    Guard g_(nullptr, "vkResetCommandPool", false);
    begin("vkResetCommandPool"), handle(d), handle(pool), put(f);
    for (const auto& [cb, of] : g_pool_of)
        if (of == (uint64_t)(uintptr_t)pool) not_in_use((const void*)(uintptr_t)cb, "vkResetCommandPool"), g_state[cb] = INITIAL, g_cb_fx.erase(cb);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence fence) {
    Guard g_(nullptr, "vkQueueSubmit2", false);
    begin("vkQueueSubmit2"), handle(q);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t c = 0; c < si[i].commandBufferInfoCount; c++) {
            const VkCommandBuffer cb = si[i].pCommandBufferInfos[c].commandBuffer;
            handle(cb);
            not_in_use(cb, "vkQueueSubmit2");
            if (g_track && state(cb) != EXECUTABLE) violation("vkQueueSubmit2", cb, "not executable");
            for (const Fx& e : g_cb_fx[(uint64_t)(uintptr_t)cb])   // (the fake GPU runs it: its effects, now)
                if (e.kind == 1) g_query[e.a] = g_timeline + 1;
                else if (e.kind == 2) g_checkpoint = e.a;
                else g_mem[e.a] = e.b;
        }
    handle(fence);
    g_timeline++;
    if (fence) g_fences_done.insert((uint64_t)(uintptr_t)fence);
    return VK_SUCCESS;
}
/// A status read: logged apart from the ordered stream, with what it saw; counted when it ran with packets not yet run
static void read_seen(const char* op, uint64_t v) {
    g_reads.push_back(std::hash<std::string>()(op)), g_reads.push_back(v);
    if (rec::mode() == rec::THREAD && rec::t_prod && rec::g_rec.w > rec::g_rec.R.load()) g_reads_overlapped[op]++;
}
static VkResult VKAPI_CALL fake_vkWaitForFences(VkDevice, uint32_t n, const VkFence* f, VkBool32, uint64_t) {
    Guard g_(nullptr, "vkWaitForFences", false);
    bool done = true;
    for (uint32_t i = 0; i < n; i++) done = done && g_fences_done.count((uint64_t)(uintptr_t)f[i]);
    read_seen("vkWaitForFences", done);
    return done ? VK_SUCCESS : VK_TIMEOUT;
}
static VkResult VKAPI_CALL fake_vkGetSemaphoreCounterValue(VkDevice, VkSemaphore, uint64_t* v) {
    Guard g_(nullptr, "vkGetSemaphoreCounterValue", false);
    *v = g_timeline;
    read_seen("vkGetSemaphoreCounterValue", *v);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo* wi, uint64_t) {
    Guard g_(nullptr, "vkWaitSemaphores", false);
    const bool done = g_timeline >= wi->pValues[0];
    read_seen("vkWaitSemaphores", done);
    return done ? VK_SUCCESS : VK_TIMEOUT;
}
static void VKAPI_CALL fake_vkGetQueueCheckpointDataNV(VkQueue, uint32_t* n, VkCheckpointDataNV* cd) {
    Guard g_(nullptr, "vkGetQueueCheckpointDataNV", false);
    if (cd && *n) cd[0].pCheckpointMarker = (void*)(uintptr_t)g_checkpoint, *n = 1;
    read_seen("vkGetQueueCheckpointDataNV", g_checkpoint);
}
static VkResult VKAPI_CALL fake_vkGetDeviceFaultInfoEXT(VkDevice, VkDeviceFaultCountsEXT* c, VkDeviceFaultInfoEXT*) {
    Guard g_(nullptr, "vkGetDeviceFaultInfoEXT", false);
    c->addressInfoCount = 0, c->vendorInfoCount = 0, c->vendorBinarySize = g_timeline;   // (what the device ran: the submits)
    read_seen("vkGetDeviceFaultInfoEXT", g_timeline);
    return VK_SUCCESS;
}
// (VMA's, faked: the allocation is named by the buffer the GPU wrote; what the host then sees)
struct VmaAllocator_T;
struct VmaAllocation_T;
typedef VmaAllocator_T* VmaAllocator;
typedef VmaAllocation_T* VmaAllocation;
VkResult (vmaInvalidateAllocation)(VmaAllocator, VmaAllocation a, VkDeviceSize, VkDeviceSize) {
    Guard g_(nullptr, "vmaInvalidateAllocation", false);
    const uint64_t v = g_mem.count((uint64_t)(uintptr_t)a) ? g_mem[(uint64_t)(uintptr_t)a] : 0;
    read_seen("vmaInvalidateAllocation", v);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_vkGetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t first, uint32_t n, size_t size, void* data,
                                                      VkDeviceSize stride, VkQueryResultFlags flags) {
    Guard g_(nullptr, "vkGetQueryPoolResults", false);
    begin("vkGetQueryPoolResults"), handle(d), handle(p), put(first), put(n), put(stride), put(flags);
    std::memset(data, 0, size);
    for (uint32_t i = 0; i < n && (i + 1) * stride <= size; i++) {   // (the timestamps the submitted command buffers wrote)
        const auto it = g_query.find((uint64_t)(uintptr_t)p << 16 | (first + i));
        const uint64_t v = it == g_query.end() ? 0 : it->second;
        std::memcpy(static_cast<uint8_t*>(data) + i * stride, &v, 8), put(v);
    }
    return VK_SUCCESS;
}
static void VKAPI_CALL fake_vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks*) {
    Guard g_(nullptr, "vkDestroyBuffer", false);
    begin("vkDestroyBuffer"), handle(d), handle(b);
    g_destroyed.insert((uint64_t)(uintptr_t)b);
}
static void VKAPI_CALL fake_vkDestroyDevice(VkDevice d, const VkAllocationCallbacks*) {
    Guard g_(nullptr, "vkDestroyDevice", false);
    begin("vkDestroyDevice"), handle(d);
}

namespace vkx {
#define RX_FAKE(f) PFN_##f f = fake_##f;
RX_REC_CALLS(RX_FAKE, RX_FAKE)
RX_FAKE(vkBeginCommandBuffer) RX_FAKE(vkEndCommandBuffer) RX_FAKE(vkResetCommandPool) RX_FAKE(vkQueueSubmit2) RX_FAKE(vkWaitForFences)
RX_FAKE(vkGetQueryPoolResults) RX_FAKE(vkDestroyBuffer) RX_FAKE(vkDestroyDevice) RX_FAKE(vkGetSemaphoreCounterValue)
RX_FAKE(vkWaitSemaphores) RX_FAKE(vkGetQueueCheckpointDataNV) RX_FAKE(vkGetDeviceFaultInfoEXT)
#undef RX_FAKE
} // namespace vkx

// ---- the renderer's shapes of argument storage
template <class T>
static T H(uint64_t v) {
    return (T)(uintptr_t)v;
}
template <class T>
static void poison(T* p, size_t n = 1) {
    std::memset((void*)p, 0xCD, sizeof(T) * n);
}
template <class T>
static void poison(std::vector<T>& v) {   // (the next draw's reset: the storage stays, its contents change)
    if (!v.empty()) poison(v.data(), v.size());
    v.clear();
}

static bool g_mutate = false;
constexpr uint32_t kPushSize = 96;   // (vk_draw.cpp's: MESH_FALLBACK_PUSH_END)

struct DescriptorWrites {   // (vk_draw.cpp's thread-local dw: reset at every build, the writes point into the info vectors)
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufs;
    std::vector<VkDescriptorImageInfo> imgs;
    std::vector<VkBufferView> views;
    void reset() {
        writes.clear(), bufs.clear(), imgs.clear(), views.clear();
        bufs.reserve(64), imgs.reserve(64), views.reserve(64);
    }
    void after_call() {
        if (g_mutate) poison(writes), poison(bufs), poison(imgs), poison(views);
    }
};
static thread_local DescriptorWrites dw;

static VkWriteDescriptorSet write(uint32_t binding, VkDescriptorType t, uint32_t count) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = binding, w.descriptorType = t, w.descriptorCount = count;
    return w;
}
static void build_graphics(int d) {
    dw.reset();
    for (uint32_t i = 0; i < 3; i++) {
        dw.bufs.push_back({H<VkBuffer>(0xB000 + d * 16 + i), 256ull * i + d, 64ull + i});
        VkWriteDescriptorSet w = write(i, i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
        w.pBufferInfo = &dw.bufs.back();
        if (i == 1) w.pImageInfo = (const VkDescriptorImageInfo*)(uintptr_t)0xCDCDCDCDCDCDCDCDull;   // (ignored for a buffer)
        dw.writes.push_back(w);
    }
    for (uint32_t i = 0; i < 4; i++) {
        dw.imgs.push_back({H<VkSampler>(0x5000 + i), H<VkImageView>(0x6000 + d * 16 + i), VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w = write(3 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);
        w.pImageInfo = &dw.imgs.back();
        dw.writes.push_back(w);
    }
    const size_t at = dw.imgs.size();   // (an array binding: two sampled images)
    dw.imgs.push_back({VK_NULL_HANDLE, H<VkImageView>(0x6100 + d), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    dw.imgs.push_back({VK_NULL_HANDLE, H<VkImageView>(0x6200 + d), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkWriteDescriptorSet arr = write(7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2);
    arr.dstArrayElement = 0, arr.pImageInfo = &dw.imgs[at];
    dw.writes.push_back(arr);
    dw.views.push_back(H<VkBufferView>(0x7000 + d));
    VkWriteDescriptorSet tb = write(8, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1);
    tb.pTexelBufferView = &dw.views.back();
    dw.writes.push_back(tb);
}

static void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {
    uint8_t data[kPushSize]{};
    for (uint32_t i = 0; i < kPushSize; i++) data[i] = (uint8_t)(i * 7 + d);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, kPushSize, data);
    if (g_mutate) poison(data, kPushSize);
    const uint64_t firsts = 0x1000ull + d;   // (a mesh draw's firsts: 8 bytes at their offset)
    uint64_t f = firsts;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 80, 8, &f);
    if (g_mutate) poison(&f);
}

struct DynState {   // (vk_draw.cpp's DynState: the arrays the dynamic state calls point into)
    VkViewport vp;
    VkRect2D sc;
    float blend[4];
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
};
static void dynamic_state(VkCommandBuffer cmd, int d) {
    DynState s{};
    s.vp = {0, (float)d, 1280, 720, 0, 1};
    s.sc = {{0, d}, {1280, 720}};
    for (int i = 0; i < 4; i++) s.blend[i] = 0.25f * i + d;
    for (uint32_t i = 0; i < 8; i++) {
        s.en[i] = (i + d) & 1;
        s.eq[i] = {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
                   (VkBlendOp)(i % 3)};
        s.wm[i] = 0xF >> (i & 3);
    }
    vkCmdSetViewportWithCount(cmd, 1, &s.vp);
    vkCmdSetScissorWithCount(cmd, 1, &s.sc);
    vkCmdSetBlendConstants(cmd, s.blend);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, 1 + d);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, 2 + d);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, 0xFF);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, 0x0F);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                      VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBias(cmd, 1.5f, 0.0f, -2.0f + d);
    vkCmdSetDepthBiasEnable(cmd, VK_TRUE);
    if (d == 0) vkCmdSetLineWidth(cmd, 1.0f), vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmd, d & 1);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER);
    vkCmdSetStencilTestEnable(cmd, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
    vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
    vkCmdSetDepthClampEnableEXT(cmd, VK_FALSE);
    vkCmdSetAlphaToCoverageEnableEXT(cmd, d == 2);
    vkCmdSetColorBlendEnableEXT(cmd, 0, 3, s.en);
    vkCmdSetColorBlendEquationEXT(cmd, 0, 3, s.eq);
    vkCmdSetColorWriteMaskEXT(cmd, 0, 3, s.wm);
    if (g_mutate) poison(&s);
}

static void vertex_streams(VkCommandBuffer cmd, int d) {
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16], vsizes[16], vstrides[16];
    for (uint32_t i = 0; i < 16; i++) vbufs[i] = H<VkBuffer>(0x9000 + i), voffs[i] = 64ull * i + d, vsizes[i] = 4096 - i, vstrides[i] = 12 + 4 * (i % 3);
    vkCmdBindVertexBuffers2(cmd, 0, 16, vbufs, voffs, vsizes, vstrides);
    if (g_mutate) poison(vbufs, 16), poison(voffs, 16), poison(vsizes, 16), poison(vstrides, 16);
    VkBuffer one = H<VkBuffer>(0x9100 + d);
    VkDeviceSize off = 32;
    vkCmdBindVertexBuffers2(cmd, 3, 1, &one, &off, nullptr, nullptr);   // (sizes and strides left out: null stays null)
    if (g_mutate) poison(&one), poison(&off);
}

static void begin_pass(VkCommandBuffer cmd, int pass) {   // (vk_draw.cpp's begin_pass: ca[8] and da on the stack)
    VkRenderingAttachmentInfo ca[8]{}, da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    for (uint32_t i = 0; i < 8; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = H<VkImageView>(0xA000 + pass * 16 + i);
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = i ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[i].clearValue.color.float32[0] = 0.5f + i, ca[i].clearValue.color.float32[3] = 1.0f;
    }
    da.imageView = H<VkImageView>(0xA0FF + pass), da.imageLayout = VK_IMAGE_LAYOUT_GENERAL, da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE, da.clearValue.depthStencil = {0.0f, 7};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {1280u + pass, 720}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = pass ? &da : nullptr;   // (the same struct for both, or no stencil)
    vkCmdBeginRendering(cmd, &ri);
    if (g_mutate) poison(ca, 8), poison(&da), poison(&ri);
}

static void barrier(VkCommandBuffer cmd, int k) {   // (full_barrier / texture transitions: barriers on the stack)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | k;
    VkBufferMemoryBarrier2 bb[2]{{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}, {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}};
    for (int i = 0; i < 2; i++) bb[i].buffer = H<VkBuffer>(0xC000 + i), bb[i].offset = 16ull * k, bb[i].size = VK_WHOLE_SIZE, bb[i].srcQueueFamilyIndex = bb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkImageMemoryBarrier2 ib[3]{};
    for (int i = 0; i < 3; i++) {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].image = H<VkImage>(0xD000 + i + k);
        ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1 + (uint32_t)i, 0, VK_REMAINING_ARRAY_LAYERS};
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    di.bufferMemoryBarrierCount = 2, di.pBufferMemoryBarriers = bb;
    di.imageMemoryBarrierCount = k ? 3 : 0, di.pImageMemoryBarriers = ib;   // (a count of 0 with a pointer: not read)
    vkCmdPipelineBarrier2(cmd, &di);
    if (g_mutate) poison(&mb), poison(bb, 2), poison(ib, 3), poison(&di);
}

static thread_local std::vector<VkBufferCopy> copies;   // (vk_memory.cpp's mirror copies: a reused thread-local vector)
static void copy_runs(VkCommandBuffer up, int k) {
    copies.clear();
    for (int i = 0; i < 5 - k; i++) copies.push_back({4096ull * i, 8192ull * i + k, 256ull + i});
    vkCmdCopyBuffer(up, H<VkBuffer>(0xE000), H<VkBuffer>(0xE001 + k), (uint32_t)copies.size(), copies.data());
    if (g_mutate) poison(copies);
}

static void frame() {
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100), up = H<VkCommandBuffer>(0x200);   // (the recording's and its upload one)
    const VkPipelineLayout layout = H<VkPipelineLayout>(0x300);
    vkCmdResetQueryPool(up, H<VkQueryPool>(0x400), 0, 64);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, H<VkQueryPool>(0x400), 0);
    copy_runs(up, 0);
    copy_runs(up, 2);
    {
        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(255 - i);
        vkCmdUpdateBuffer(up, H<VkBuffer>(0xE100), 128, sizeof data, data);
        if (g_mutate) poison(data, 64);
    }
    vkCmdFillBuffer(cmd, H<VkBuffer>(0xE200), 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, 0);
    for (int pass = 0; pass < 2; pass++) {
        begin_pass(cmd, pass);
        for (int d = 0; d < 3; d++) {   // (draw(): pipeline, descriptors, push constants, dynamic state, streams, the draw)
            const int at = pass * 3 + d;
            vkCmdWriteBufferMarkerAMD(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, H<VkBuffer>(0xF000), 8ull * at, 100 + at);
            vkCmdSetCheckpointNV(cmd, (void*)(uintptr_t)(at + 1));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipeline>(0x800 + at));
            build_graphics(at);
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)dw.writes.size(), dw.writes.data());
            {   // (bind_descriptors' delta: the changed writes copied into another thread-local vector)
                static thread_local std::vector<VkWriteDescriptorSet> changed;
                changed.clear();
                changed.push_back(dw.writes[0]), changed.push_back(dw.writes[4]), changed.push_back(dw.writes[8]);
                vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)changed.size(), changed.data());
                if (g_mutate) poison(changed);
            }
            dw.after_call();
            push_constants(cmd, layout, at);
            dynamic_state(cmd, at);
            vertex_streams(cmd, at);
            vkCmdBindIndexBuffer(cmd, H<VkBuffer>(0x9F00), 4ull * at, d ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdBeginQuery(cmd, H<VkQueryPool>(0x401), at, 0);
            switch ((pass * 3 + d) % 6) {
            case 0: vkCmdDrawIndexed(cmd, 36, 1, 0, -4, 0); vkCmdDraw(cmd, 3, 1, 0, 0); break;
            case 1: vkCmdDrawIndirect(cmd, H<VkBuffer>(0x9E00), 16, 4, 16); vkCmdDrawIndexedIndirect(cmd, H<VkBuffer>(0x9E00), 64, 2, 20); break;
            case 2: vkCmdDrawIndirectCount(cmd, H<VkBuffer>(0x9E00), 0, H<VkBuffer>(0x9E01), 4, 8, 16);
                    vkCmdDrawIndexedIndirectCount(cmd, H<VkBuffer>(0x9E00), 0, H<VkBuffer>(0x9E01), 8, 8, 20); break;
            case 3: vkCmdDrawMeshTasksEXT(cmd, 4, 1, 1); break;
            case 4: vkCmdDrawMeshTasksIndirectEXT(cmd, H<VkBuffer>(0x9E02), 12, 3, 12); break;
            default: vkCmdDrawMeshTasksIndirectCountEXT(cmd, H<VkBuffer>(0x9E02), 0, H<VkBuffer>(0x9E01), 12, 5, 12); break;
            }
            vkCmdEndQuery(cmd, H<VkQueryPool>(0x401), at);
        }
        {   // (a clear inside the pass: a rect and an attachment on the stack; the shader clear's own viewport and scissor)
            VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 1, {}};
            ca.clearValue.color.uint32[2] = 0xABCD + pass;
            VkClearRect cr{{{0, 0}, {64, 32}}, 0, 1};
            vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
            if (g_mutate) poison(&ca), poison(&cr);
            VkViewport v{0, 0, 64, 32, 0, 1};
            VkRect2D rect{{0, 0}, {64, 32}};
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            if (g_mutate) poison(&v), poison(&rect);
        }
        vkCmdEndRendering(cmd);
        barrier(cmd, 1 + pass);
    }
    {   // (a compute dispatch: storage image, storage buffer, texel buffer; push constants; descriptor sets with dynamic offsets)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, H<VkPipeline>(0x900));
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, H<VkImageView>(0x6500), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo bi{H<VkBuffer>(0xB500), 0, VK_WHOLE_SIZE};
        VkBufferView tv = H<VkBufferView>(0x7500);
        VkWriteDescriptorSet w[3] = {write(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1), write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                                     write(2, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1)};
        w[0].pImageInfo = &ii, w[1].pBufferInfo = &bi, w[2].pTexelBufferView = &tv;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, w);
        if (g_mutate) poison(&ii), poison(&bi), poison(&tv), poison(w, 3);
        uint32_t size[3] = {640, 480, 1};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, size);
        if (g_mutate) poison(size, 3);
        vkCmdDispatch(cmd, 80, 60, 1);
        vkCmdDispatchIndirect(cmd, H<VkBuffer>(0x9D00), 48);
        VkDescriptorSet sets[2] = {H<VkDescriptorSet>(0x1100), H<VkDescriptorSet>(0x1101)};
        uint32_t dyn[3] = {256, 512, 768};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 3, dyn);
        if (g_mutate) poison(dyn, 3);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 2, 1, &sets[1], 0, dyn);   // (no dynamic offsets: not read)
        if (g_mutate) poison(sets, 2);
    }
    {   // (texture work: uploads, copies, blits, resolves, clears; regions and values on the stack)
        VkBufferImageCopy bic[2]{};
        for (int i = 0; i < 2; i++) bic[i] = {1024ull * i, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, 0, 1}, {i, 0, 0}, {64u >> i, 64u >> i, 1}};
        vkCmdCopyBufferToImage(up, H<VkBuffer>(0xE300), H<VkImage>(0xD100), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, bic);
        vkCmdCopyImageToBuffer(cmd, H<VkImage>(0xD101), VK_IMAGE_LAYOUT_GENERAL, H<VkBuffer>(0xE301), 1, &bic[1]);
        if (g_mutate) poison(bic, 2);
        VkImageCopy ic{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {1, 2, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {3, 4, 0}, {16, 16, 1}};
        vkCmdCopyImage(cmd, H<VkImage>(0xD102), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(0xD103), VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        if (g_mutate) poison(&ic);
        VkImageBlit bl{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {128, 64, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, {{0, 0, 0}, {64, 32, 1}}};
        vkCmdBlitImage(cmd, H<VkImage>(0xD104), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(0xD105), VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        if (g_mutate) poison(&bl);
        VkImageResolve rs{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {1280, 720, 1}};
        vkCmdResolveImage(cmd, H<VkImage>(0xD106), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(0xD107), VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
        if (g_mutate) poison(&rs);
        VkClearColorValue col{};
        col.float32[0] = 0.1f, col.float32[1] = 0.2f, col.float32[2] = 0.3f, col.float32[3] = 1.0f;
        VkImageSubresourceRange ranges[2] = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 1, 2, 0, 6}};
        vkCmdClearColorImage(cmd, H<VkImage>(0xD108), VK_IMAGE_LAYOUT_GENERAL, &col, 2, ranges);
        if (g_mutate) poison(&col);
        VkClearDepthStencilValue ds{1.0f, 0x55};
        VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, H<VkImage>(0xD109), VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &dr);
        if (g_mutate) poison(&ds), poison(&dr), poison(ranges, 2);
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, H<VkQueryPool>(0x400), 1);
}

// (a call the packer does not take: a pNext chain, a descriptor type it does not know. Held packets go first, then the direct call)
static void fallbacks() {
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100);
    vkCmdDraw(cmd, 1, 2, 3, 4);
    VkMemoryBarrier2 next{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.pNext = &next;
    vkCmdPipelineBarrier2(cmd, &di);
    vkCmdDraw(cmd, 5, 6, 7, 8);
    VkWriteDescriptorSet w = write(0, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 16);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipelineLayout>(0x300), 0, 1, &w);
    vkCmdDispatch(cmd, 1, 1, 1);
}

// ---- RX_VK_RECORDER=defer's lifecycle: the recording's two command buffers (its own and the upload one, in pools of their own, as
// vk_replay.cpp's begin_recording / submit_recording), reset, begun, recorded into interleaved, ended, submitted, waited for, twice;
// a query read and a destruction after. Every held command must reach the fake while its command buffer records, none after its end
static const VkDevice kDev = H<VkDevice>(0x50);
static const VkCommandPool kPool = H<VkCommandPool>(0x10), kUpPool = H<VkCommandPool>(0x20);
static const VkCommandBuffer kCmd = H<VkCommandBuffer>(0x100), kUp = H<VkCommandBuffer>(0x200);
static void lifecycle_reset() {
    g_state.clear(), g_violations = 0, g_violation.clear();
    gpu_reset();
    g_pool_of = {{0x100, 0x10}, {0x200, 0x20}, {0x300, 0x60}};
}
static void recording_begin() {
    vkResetCommandPool(kDev, kPool, 0);
    vkResetCommandPool(kDev, kUpPool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(kCmd, &bi);
    vkBeginCommandBuffer(kUp, &bi);
}
static void recording_end(int k) {
    hold();   // (the recording's commands in flight at its end)
    vkEndCommandBuffer(kUp);
    vkEndCommandBuffer(kCmd);
    VkCommandBufferSubmitInfo cbi[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    cbi[0].commandBuffer = kUp, cbi[1].commandBuffer = kCmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 2, si.pCommandBufferInfos = cbi;
    const VkFence fence = H<VkFence>(0x30 + k);
    vkQueueSubmit2(H<VkQueue>(0x40), 1, &si, fence);
    if (g_mutate) poison(cbi, 2), poison(&si);
    vkWaitForFences(kDev, 1, &fence, VK_TRUE, UINT64_MAX);
}
static void lifecycle() {
    for (int k = 0; k < 2; k++) {
        recording_begin();
        frame();
        fallbacks();   // (FIFO around the calls not packed)
        frame();
        recording_end(k);
    }
    uint64_t q[2];
    vkGetQueryPoolResults(kDev, H<VkQueryPool>(0x400), 0, 2, sizeof q, q, 8, VK_QUERY_RESULT_64_BIT);
    vkDestroyBuffer(kDev, H<VkBuffer>(0xE000), nullptr);
}
/// (one recording of 40 frames: past 1 MB of packets held, for the budgets)
static void big() {
    recording_begin();
    for (int i = 0; i < 40; i++) frame();
    recording_end(7);
}
/// A recording handed between two threads under the replay's lock (vk_replay.cpp's replay_mu, a DrainingMutex): one begins it and
/// records, lets the lock go and stays alive; the other records more, ends and submits it
static rec::DrainingMutex g_replay_mu;
static void handoff() {
    std::atomic<int> step{0};
    std::thread other([&] {
        {
            std::lock_guard lk(g_replay_mu);
            recording_begin();
            hold();   // (its commands in flight when it lets the lock go)
            frame();
        }
        step = 1;
        while (step.load() != 2) std::this_thread::yield();
    });
    while (step.load() != 1) std::this_thread::yield();
    {
        std::lock_guard lk(g_replay_mu);
        frame();
        recording_end(9);
    }
    step = 2;
    other.join();
}

// ---- RX_VK_RECORDER=thread's own (each also run direct, for the reference log)
static const VkCommandPool kPoolB = H<VkCommandPool>(0x60);
static const VkCommandBuffer kCmdB = H<VkCommandBuffer>(0x300);
static void draws_into(VkCommandBuffer cb, int n, int k) {   // (draws with push constants on the stack)
    for (int i = 0; i < n; i++) {
        uint32_t pc[4] = {(uint32_t)i, (uint32_t)k, 0xABCD, 7};
        vkCmdPushConstants(cb, H<VkPipelineLayout>(0x300), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
        if (g_mutate) poison(pc, 4);
        vkCmdDraw(cb, 3, 1, (uint32_t)i, (uint32_t)k);
    }
}
static void submit(const VkCommandBuffer* cbs, uint32_t n, VkFence fence) {
    VkCommandBufferSubmitInfo cbi[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    for (uint32_t i = 0; i < n; i++) cbi[i].commandBuffer = cbs[i];
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = n, si.pCommandBufferInfos = cbi;
    vkQueueSubmit2(H<VkQueue>(0x40), 1, &si, fence);
    if (g_mutate) poison(cbi, 2), poison(&si);
    vkWaitForFences(kDev, 1, &fence, VK_TRUE, UINT64_MAX);
}
/// Two recordings at once: A ended and submitted while B, still recording, has commands in flight (A's submit must not pass them)
static void two_recordings() {
    recording_begin();
    vkResetCommandPool(kDev, kPoolB, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(kCmdB, &bi);
    frame();
    draws_into(kCmdB, 40, 0);
    vkEndCommandBuffer(kUp);
    vkEndCommandBuffer(kCmd);
    hold();   // (B's commands in flight at A's submit)
    draws_into(kCmdB, 40, 1);
    const VkCommandBuffer a[2] = {kUp, kCmd};
    submit(a, 2, H<VkFence>(0x31));
    hold();
    draws_into(kCmdB, 40, 2);
    vkEndCommandBuffer(kCmdB);
    submit(&kCmdB, 1, H<VkFence>(0x32));
}
/// A recording given up: its upload pool reset while recording (valid: nothing of it is pending), begun again, recorded, ended
static void reset_while_recording() {
    recording_begin();
    copy_runs(kUp, 0);
    frame();
    hold();   // (its commands in flight at the pool's reset)
    copy_runs(kUp, 2);
    vkResetCommandPool(kDev, kUpPool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(kUp, &bi);
    copy_runs(kUp, 1);
    recording_end(5);
}
/// The replay lock let go by a thread that recorded, then another thread, without the lock, ends and submits the recording: the
/// recorder must be idle once the lock is let go
static void unlock_then_end() {
    std::atomic<int> step{0};
    std::thread other([&] {
        {
            std::lock_guard lk(g_replay_mu);
            recording_begin();
            hold();   // (its commands in flight when it lets the lock go)
            frame();
        }
        step = 1;
        while (step.load() != 2) std::this_thread::yield();
    });
    while (step.load() != 1) std::this_thread::yield();
    recording_end(11);
    step = 2;
    other.join();
}
/// A packet bigger than the room it is given (with the 2 KB room of the run): made directly, in its place among the others
static void oversize() {
    recording_begin();
    draws_into(kCmd, 20, 0);
    std::vector<uint8_t> big(4096);
    for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 13);
    vkCmdUpdateBuffer(kUp, H<VkBuffer>(0xE100), 0, big.size(), big.data());
    if (g_mutate) poison(big.data(), big.size());
    draws_into(kCmd, 20, 1);
    recording_end(6);
}
/// The same packet first, the ring empty (the recorder just started, at the ring's start): the ring has room for it, the packet's
/// room has not, so it is still made directly (the room alone decides, not how far the recorder has got)
static void oversize_first() {
    recording_begin();
    std::vector<uint8_t> big(4096);
    for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 7);
    vkCmdUpdateBuffer(kUp, H<VkBuffer>(0xE200), 0, big.size(), big.data());
    if (g_mutate) poison(big.data(), big.size());
    draws_into(kCmd, 20, 2);
    recording_end(7);
}
/// Teardown with work queued: g_at_stop runs with frames in flight (the recorder stopped and joined), then more is recorded (the
/// recorder started again), then vkDestroyDevice (stopped again: its boundary)
static void (*g_at_stop)() = nullptr;
static void teardown() {
    recording_begin();
    for (int i = 0; i < 4; i++) frame();
    g_at_stop();
    frame();
    recording_end(12);
    vkDestroyDevice(kDev, nullptr);
}
/// The status reads (the calls the producer makes without waiting: unordered()) while a recording's commands are in flight, and after
/// its submit: each must see what the direct calls see, the submitted work and nothing recorded since
static void status_reads_at(VkFence prev, bool recording) {
    // (recording: a few commands put in flight before each read, so that one read's wait cannot empty the ring for the next. The
    // recorder is held for 50 ms from there: a read made without waiting finds them still in flight on a loaded runner too, and
    // one that waits is let through once the hold ends)
    const auto busy = [&] {
        if (!recording) return;
        g_hold_until_ns = (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count() + 50000000;
        draws_into(kCmd, 3, 7);
    };
    uint64_t v = 0;
    busy();
    vkGetSemaphoreCounterValue(kDev, H<VkSemaphore>(0x70), &v);
    const VkSemaphore sem = H<VkSemaphore>(0x70);
    const uint64_t want = v + 1;   // (not reached until the next submit)
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1, wi.pSemaphores = &sem, wi.pValues = &want;
    busy();
    vkWaitSemaphores(kDev, &wi, 0);
    wi.pValues = &v;
    busy();
    vkWaitSemaphores(kDev, &wi, 0);
    busy();
    vkWaitForFences(kDev, 1, &prev, VK_TRUE, 0);
    busy();
    vmaInvalidateAllocation(nullptr, (VmaAllocation)(uintptr_t)0xE400, 0, 16);
    uint32_t n = 1;
    VkCheckpointDataNV cd{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    busy();
    vkGetQueueCheckpointDataNV(H<VkQueue>(0x40), &n, &cd);
    VkDeviceFaultCountsEXT fc{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    busy();
    vkGetDeviceFaultInfoEXT(kDev, &fc, nullptr);
    g_hold_until_ns = 0;
}
static void status_reads() {
    for (int k = 0; k < 3; k++) {
        recording_begin();
        vkCmdFillBuffer(kCmd, H<VkBuffer>(0xE400), 0, 16, 0x100 + k);
        vkCmdSetCheckpointNV(kCmd, (void*)(uintptr_t)(0x200 + k));
        frame();
        {   // (a query read mid-recording, the frame's commands in flight: kept ordered, so in the stream after them)
            uint64_t q[2];
            vkGetQueryPoolResults(kDev, H<VkQueryPool>(0x400), 0, 2, sizeof q, q, 8, VK_QUERY_RESULT_64_BIT);
        }
        frame();
        status_reads_at(H<VkFence>(0x30 + 20 + k - 1), true);
        vkCmdFillBuffer(kCmd, H<VkBuffer>(0xE400), 0, 16, 0x180 + k);
        vkCmdSetCheckpointNV(kCmd, (void*)(uintptr_t)(0x280 + k));
        status_reads_at(H<VkFence>(0x30 + 20 + k), true);
        recording_end(20 + k);
        status_reads_at(H<VkFence>(0x30 + 20 + k), false);
        uint64_t q[2];
        vkGetQueryPoolResults(kDev, H<VkQueryPool>(0x400), 0, 2, sizeof q, q, 8, VK_QUERY_RESULT_64_BIT);
    }
}
/// A buffer destroyed while a recording that names it is open (valid: that recording is reset, never submitted); every command
/// naming it must have reached the driver before the destruction
static void destroy_recorded() {
    recording_begin();
    frame();
    copy_runs(kUp, 0);   // (names 0xE001: in flight behind the frame when it is destroyed)
    vkDestroyBuffer(kDev, H<VkBuffer>(0xE001), nullptr);
    vkResetCommandPool(kDev, kUpPool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(kUp, &bi);
    copy_runs(kUp, 2);
    recording_end(14);
}
static int g_stress_frames = 0;
static void stress() {
    recording_begin();
    for (int i = 0; i < g_stress_frames; i++) frame();
    recording_end(13);
}
/// --throw: a driver call throws on the recorder thread: the process must end loudly (exit code 3, "FATAL"), never go on
static int throw_test() {
    rec::set_mode(rec::THREAD);
    std::lock_guard lk(g_replay_mu);
    vkCmdDraw(kCmd, 1, 1, 0, 0);
    g_throw = true;
    vkCmdDraw(kCmd, 2, 1, 0, 0);
    vkCmdDraw(kCmd, 3, 1, 0, 0);
    rec::boundary(rec::B_vkEndCommandBuffer);
    std::printf("the recorder's exception was not reported\n");
    return 0;
}

// ---- checks
static int failures = 0;
static std::vector<Call> run(void (*scenario)(), int mode, bool mutate) {
    rec::set_mode(mode);
    g_mutate = mutate;
    g_log.clear();
    if (mode == rec::THREAD) {   // (the producer: the replay lock's holder; its unlock waits for the recorder)
        std::lock_guard lk(g_replay_mu);
        scenario();
    } else {
        scenario();
    }
    if (mode == rec::DEFER && !g_log.empty() && scenario == frame) {
        std::printf("FAIL deferred: %zu calls reached the driver before the drain\n", g_log.size());
        failures++;
    }
    rec::drain();
    rec::set_mode(rec::OFF);
    return g_log;
}
static void same(const char* what, const std::vector<Call>& ref, const std::vector<Call>& got) {
    for (size_t i = 0; i < std::max(ref.size(), got.size()); i++) {
        if (i < ref.size() && i < got.size() && ref[i].op == got[i].op && ref[i].bytes == got[i].bytes) continue;
        size_t at = 0;
        if (i < ref.size() && i < got.size())
            while (at < ref[i].bytes.size() && at < got[i].bytes.size() && ref[i].bytes[at] == got[i].bytes[at]) at++;
        std::printf("MISMATCH %s: call %zu of %zu/%zu: expected %s, got %s (bytes differ from %zu)\n", what, i, ref.size(), got.size(),
                    i < ref.size() ? ref[i].op.c_str() : "(end)", i < got.size() ? got[i].op.c_str() : "(end)", at);
        failures++;
        return;
    }
    size_t bytes = 0;
    for (const Call& c : got) bytes += c.bytes.size();
    std::printf("PASS %s: %zu calls, %zu argument bytes identical\n", what, got.size(), bytes);
}

/// A scenario with the lifecycle tracked: its log, the violations, and the arena's counters for the run (own_thread: on a thread of
/// its own, an arena that starts empty: its aligned blocks are counted against its chunks)
struct Out {
    std::vector<Call> log;
    std::vector<uint64_t> reads;
    std::map<std::string, uint64_t> overlapped;
    int violations = 0;
    std::string violation;
    bool escaped = false;
    uint64_t failures = 0, budget_drains = 0, fallbacks = 0;
    size_t capacity = 0;
    long leaked = 0;
};
static Out run_tracked(void (*scenario)(), int mode, bool mutate, bool own_thread = false, bool lock = true) {
    Out o;
    const auto body = [&] {
        rec::Arena& a = rec::t_arena;
        const uint64_t f0 = a.failures, b0 = a.budget_drains, fb0 = a.fallbacks;
        const long live0 = g_aligned_live.load() - (long)a.nchunks;
        rec::set_mode(mode);
        g_mutate = mutate, g_track = true;
        lifecycle_reset();
        g_log.clear();
        try {
            if (mode == rec::THREAD && lock) {
                std::lock_guard lk(g_replay_mu);
                scenario();
            } else {
                scenario();
            }
        } catch (const std::bad_alloc&) {
            o.escaped = true;
        }
        rec::drain();
        g_track = false;
        o.log = g_log, o.violations = g_violations, o.violation = g_violation, o.reads = g_reads, o.overlapped = g_reads_overlapped;
        o.failures = a.failures - f0, o.budget_drains = a.budget_drains - b0, o.fallbacks = a.fallbacks - fb0, o.capacity = a.capacity;
        if (own_thread) o.leaked = g_aligned_live.load() - live0 - (long)a.nchunks;
    };
    if (own_thread) {
        std::thread t(body);
        t.join();
    } else {
        body();
    }
    rec::set_mode(rec::OFF);
    return o;
}
static void check_run(const char* what, const std::vector<Call>& ref, const Out& o, const std::vector<uint64_t>* ref_reads = nullptr) {
    if (ref_reads && *ref_reads != o.reads)
        std::printf("MISMATCH %s: the status reads (fences, semaphores, checkpoints, fault data, invalidated memory) saw other "
                    "values: %zu/%zu\n", what, ref_reads->size(), o.reads.size()), failures++;
    if (o.escaped) std::printf("FAIL %s: an allocation failure escaped a vkCmd* (the call lost)\n", what), failures++;
    if (o.violations) std::printf("FAIL %s: %d lifecycle violation(s), the first: %s\n", what, o.violations, o.violation.c_str()), failures++;
    same(what, ref, o.log);
}

static double now_ns() { return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- bench: a typical draw (~4 calls: the descriptor delta, push constants, streams, the draw) and a cold one (the whole state)
static void draw_typical(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {
    static thread_local std::vector<VkWriteDescriptorSet> changed;
    changed.clear();
    changed.push_back(dw.writes[0]), changed.push_back(dw.writes[3]), changed.push_back(dw.writes[4]);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, changed.data());
    uint8_t data[kPushSize];
    std::memset(data, d, sizeof data);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, kPushSize, data);
    VkBuffer vb[16];
    VkDeviceSize vo[16], vs[16], vst[16];
    for (int i = 0; i < 16; i++) vb[i] = H<VkBuffer>(0x9000 + i), vo[i] = i + d, vs[i] = 4096, vst[i] = 16;
    vkCmdBindVertexBuffers2(cmd, 0, 16, vb, vo, vs, vst);
    vkCmdDrawIndexed(cmd, 36, 1, 0, d & 7, 0);
}
static void draw_cold(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipeline>(0x800 + (d & 3)));
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)dw.writes.size(), dw.writes.data());
    DynState s{};
    s.vp = {0, 0, 1280, 720, 0, 1}, s.sc = {{0, 0}, {1280, 720}}, s.en[0] = d & 1, s.wm[0] = 0xF;
    vkCmdSetViewportWithCount(cmd, 1, &s.vp);
    vkCmdSetScissorWithCount(cmd, 1, &s.sc);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmd, d & 1);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER);
    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, s.en);
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, s.eq);
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, s.wm);
    vkCmdBindIndexBuffer(cmd, H<VkBuffer>(0x9F00), 0, VK_INDEX_TYPE_UINT16);
    draw_typical(cmd, layout, d);
}
static void draw_typical_direct(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {   // (no wrapper: the driver's functions, as before)
    static thread_local std::vector<VkWriteDescriptorSet> changed;
    changed.clear();
    changed.push_back(dw.writes[0]), changed.push_back(dw.writes[3]), changed.push_back(dw.writes[4]);
    (::vkx::vkCmdPushDescriptorSetKHR)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, changed.data());
    uint8_t data[kPushSize];
    std::memset(data, d, sizeof data);
    (::vkx::vkCmdPushConstants)(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, kPushSize, data);
    VkBuffer vb[16];
    VkDeviceSize vo[16], vs[16], vst[16];
    for (int i = 0; i < 16; i++) vb[i] = H<VkBuffer>(0x9000 + i), vo[i] = i + d, vs[i] = 4096, vst[i] = 16;
    (::vkx::vkCmdBindVertexBuffers2)(cmd, 0, 16, vb, vo, vs, vst);
    (::vkx::vkCmdDrawIndexed)(cmd, 36, 1, 0, d & 7, 0);
}
static int bench(int n) {
    if (const char* e = getenv("BENCH_PUBLISH_BYTES")) rec::g_publish_bytes = (size_t)atoll(e);   // (the publication batch to time)
    g_quiet = true;
    build_graphics(0);
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100);
    const VkPipelineLayout layout = H<VkPipelineLayout>(0x300);
    struct Shape {
        const char* name;
        void (*draw)(VkCommandBuffer, VkPipelineLayout, int);
        void (*direct)(VkCommandBuffer, VkPipelineLayout, int);
        int calls;
    } shapes[] = {{"typical draw (push descriptors x3 writes, push constants 96 B, vertex buffers x16, draw)", draw_typical,
                   draw_typical_direct, 4},
                  {"cold draw (+ pipeline, 9 writes, 9 dynamic states, index buffer)", draw_cold, nullptr, 16}};
    rec::Arena& a = rec::t_arena;
    for (const Shape& sh : shapes) {
        // (modes: 0 direct (no wrapper), 1 off (the wrapper's branch), 2 sync, 3 deferred: encode and drain timed apart, 4 thread:
        // the producer's encode and publish, and its wait at the boundary, timed apart; the recorder's busy time a packet)
        double med[7] = {};
        double bytes_per_packet = 0, rec_ns = 0;
        for (int m = 0; m < 5; m++) {
            if (m == 0 && !sh.direct) continue;
            std::vector<double> reps[3];
            for (int rep = 0; rep < 5; rep++) {
                rec::set_mode(m <= 1 ? rec::OFF : m == 2 ? rec::SYNC : m == 3 ? rec::DEFER : rec::THREAD);
                const uint64_t p0 = a.packets, b0 = a.bytes;
                const uint64_t rp0 = rec::g_rec.packets.load(), rb0 = rec::g_rec.busy_ns.load();
                double enc = 0, drn = 0;
                {
                    std::unique_lock<rec::DrainingMutex> lk(g_replay_mu, std::defer_lock);
                    if (m == 4) lk.lock();
                    for (int i = 0; i < n; i += 256) {   // (deferred: drained every 256 draws, about a recording's draws; thread: waited)
                        const double t0 = now_ns();
                        for (int j = 0; j < 256; j++) (m == 0 ? sh.direct : sh.draw)(cmd, layout, i + j);
                        const double t1 = now_ns();
                        if (m == 4) rec::boundary(rec::B_vkEndCommandBuffer);
                        else rec::drain();
                        enc += t1 - t0, drn += now_ns() - t1;
                    }
                }
                reps[0].push_back(enc / n), reps[1].push_back(drn / n);
                if (m == 3) bytes_per_packet = (double)(a.bytes - b0) / (double)(a.packets - p0);
                if (m == 4) {
                    rec::set_mode(rec::OFF);   // (the recorder idle: its counters are in)
                    reps[2].push_back((double)(rec::g_rec.busy_ns.load() - rb0) / (double)(rec::g_rec.packets.load() - rp0));
                }
            }
            for (auto& r : reps) std::sort(r.begin(), r.end());
            med[m] = reps[0][2];
            if (m == 3) med[5] = reps[1][2];
            if (m == 4) med[6] = reps[1][2], rec_ns = reps[2][2];
        }
        std::printf("BENCH %s, %d calls: ", sh.name, sh.calls);
        if (sh.direct) std::printf("direct %.1f ns, ", med[0]);
        std::printf("off %.1f ns, sync %.1f ns (+%.1f over off, +%.1f a call), deferred encode %.1f ns + drain %.1f ns; "
                    "%.0f bytes a packet (median of 5, %d draws)\n",
                    med[1], med[2], med[2] - med[1], (med[2] - med[1]) / sh.calls, med[3], med[5], bytes_per_packet, n);
        std::printf("BENCH   thread: the replay thread %.1f ns a draw (+%.1f over off, %.1f a call), its wait at the boundary every 256 "
                    "draws %.1f ns a draw; the recorder %.1f ns a packet busy (%.1f M packets/s)\n",
                    med[4], med[4] - med[1], med[4] / sh.calls, med[6], rec_ns, rec_ns > 0 ? 1e3 / rec_ns : 0.0);
    }
    rec::set_mode(rec::OFF);
    std::printf("BENCH %llu fake calls, arena %zu chunk(s)\n", (unsigned long long)g_calls, a.nchunks);
    g_quiet = false;
    return 0;
}

/// --overlap F: the critical thread's time a frame, off vs thread, with a driver that takes D ns a vkCmd* (4 calls a draw), translation
/// of W ns a draw, 2000 draws a frame, and every K draws a recording's end (the producer waits for the recorder) plus 30 us that stays
/// on the replay thread in both (vkQueueSubmit2, the next recording's begin)
static void spin_ns_busy(int ns) { spin_for(ns); }
static int overlap(int frames) {
    g_quiet = true;
    build_graphics(0);
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100);
    const VkPipelineLayout layout = H<VkPipelineLayout>(0x300);
    const int draws = 2000, D = 300, W = 4000;
    g_spin_ns = D;
    std::printf("OVERLAP %d draws a frame, driver %d ns a call (%.2f ms a frame), translation %d ns a draw, 30 us a recording end "
                "(median of 5, %d frames)\n", draws, D, draws * 4 * D / 1e6, W, frames);
    for (int K : {120, 30, 8, 1}) {
        double med[2];
        uint64_t waits = 0;
        double waited = 0;
        for (int m = 0; m < 2; m++) {
            std::vector<double> reps;
            for (int rep = 0; rep < 5; rep++) {
                rec::set_mode(m ? rec::THREAD : rec::OFF);
                const uint64_t n0 = rec::g_rec.wait_n[rec::B_vkEndCommandBuffer].load(), ns0 = rec::g_rec.wait_ns[rec::B_vkEndCommandBuffer].load();
                const double t0 = now_ns();
                for (int f = 0; f < frames; f++) {
                    std::lock_guard lk(g_replay_mu);
                    for (int d = 0; d < draws; d++) {
                        spin_ns_busy(W);
                        draw_typical(cmd, layout, d);
                        if ((d + 1) % K == 0 || d + 1 == draws) {
                            rec::boundary(rec::B_vkEndCommandBuffer);
                            spin_ns_busy(30000);
                        }
                    }
                }
                reps.push_back((now_ns() - t0) / frames / 1e6);
                if (m) waits = rec::g_rec.wait_n[rec::B_vkEndCommandBuffer].load() - n0, waited = (double)(rec::g_rec.wait_ns[rec::B_vkEndCommandBuffer].load() - ns0) / frames / 1e6;
            }
            std::sort(reps.begin(), reps.end());
            med[m] = reps[2];
        }
        std::printf("OVERLAP every %4d draws (%4d waits a frame): off %.2f ms, thread %.2f ms a frame: %.2f ms saved of %.2f ms of "
                    "driver time (%.0f%%); waited %.3f ms a frame, %llu of the last run's waits found the recorder busy\n",
                    K, (draws + K - 1) / K, med[0], med[1], med[0] - med[1], draws * 4 * D / 1e6, 100 * (med[0] - med[1]) / (draws * 4 * D / 1e6),
                    waited, (unsigned long long)waits);
    }
    rec::set_mode(rec::OFF);
    g_spin_ns = 0;
    g_quiet = false;
    return 0;
}

/// --idle N: the recorder's idle spin (RX_VK_RECORDER_SPIN_US) against its CPU and the replay thread's waits. (a) bursts of one
/// typical draw after a gap of G us of translation, each followed by a boundary: the replay thread's wait there (it includes the
/// recorder's wake-up when it slept in the gap) and its encode time (it includes the wake-up call); (b) replay-like frames: 2000
/// draws of 4 us of translation, a recording end every 120, a 5 ms gap between frames, the fake driver 300 ns a call: the
/// recorder's CPU (its thread's times, and its own spun/busy count) against the wall time, and the replay thread's frame time
// (native_handle() is a Windows HANDLE with MSVC's and libc++'s std::thread, an integer pthread id with MinGW's
// libstdc++ (winpthreads): the thread's CPU only where it is a HANDLE. A template, so the other branch is not compiled)
template <class H> static bool thread_cpu_ticks(H h, uint64_t* kernel, uint64_t* user) {
#ifdef _WIN32
    if constexpr (std::is_pointer_v<H>) {
        uint64_t c, e;
        return GetThreadTimes(h, &c, &e, kernel, user) != 0;
    }
#endif
    (void)h, (void)kernel, (void)user;
    return false;
}
static double recorder_cpu_s() {
    uint64_t k, u;
    if (rec::g_rec.th.joinable() && thread_cpu_ticks(rec::g_rec.th.native_handle(), &k, &u)) return (double)(k + u) / 1e7;
    return -1;   // (not measured)
}
static int idle_bench(int iters) {
    g_quiet = true;
    build_graphics(0);
    rec::g_stats_log = false;
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100);
    const VkPipelineLayout layout = H<VkPipelineLayout>(0x300);
    for (const uint64_t spin_us : {200ull, 20ull, 0ull}) {   // (0: adaptive, the default)
        rec::g_rec.stop();
        rec::g_spin_ns = spin_us * 1000;
        rec::set_mode(rec::THREAD);
        {   // (start it, idle)
            std::lock_guard lk(g_replay_mu);
            draw_typical(cmd, layout, 0);
        }
        for (const int gap : {2, 10, 30, 100, 1000}) {
            std::vector<double> waits, encs;
            const uint64_t sl0 = rec::g_rec.sleeps.load(), sp0 = rec::g_rec.spin_ns.load(), bu0 = rec::g_rec.busy_ns.load();
            const int n = std::max(iters, 300000 / gap);   // (at least ~0.3 s)
            const double c0 = recorder_cpu_s(), t0 = now_ns();
            {
                std::lock_guard lk(g_replay_mu);
                for (int i = 0; i < n; i++) {
                    spin_for(gap * 1000);
                    const double a = now_ns();
                    draw_typical(cmd, layout, i);
                    const double b = now_ns();
                    rec::boundary(rec::B_vkEndCommandBuffer);
                    encs.push_back(b - a), waits.push_back(now_ns() - b);
                }
            }
            const double wall = (now_ns() - t0) / 1e9, cpu = recorder_cpu_s() - c0;
            std::sort(waits.begin(), waits.end()), std::sort(encs.begin(), encs.end());
            double wsum = 0, esum = 0;
            for (double w : waits) wsum += w;
            for (double e : encs) esum += e;
            std::printf("IDLE spin %s us, gap %4d us: boundary wait mean %.2f us, p99 %.2f, max %.1f; encode (4 calls) mean %.0f ns, "
                        "p99 %.0f; recorder slept %.2f times a burst, CPU %.2f of a core (its own count; thread times %.2f)\n",
                        spin_us ? std::to_string(spin_us).c_str() : "adaptive", gap, wsum / n / 1e3, waits[n * 99 / 100] / 1e3,
                        waits.back() / 1e3, esum / n, encs[n * 99 / 100], (double)(rec::g_rec.sleeps.load() - sl0) / n,
                        (double)(rec::g_rec.spin_ns.load() - sp0 + rec::g_rec.busy_ns.load() - bu0) / 1e9 / wall, cpu / wall);
        }
        {   // (b) frames
            const int frames = 30, draws = 2000;
            g_spin_ns = 300;
            const uint64_t sp0 = rec::g_rec.spin_ns.load(), bu0 = rec::g_rec.busy_ns.load(), sl0 = rec::g_rec.sleeps.load();
            const uint64_t wn0 = rec::g_rec.wait_n[rec::B_vkEndCommandBuffer].load(), wt0 = rec::g_rec.wait_ns[rec::B_vkEndCommandBuffer].load();
            rec::g_rec.wait_max[rec::B_vkEndCommandBuffer] = 0;
            const double c0 = recorder_cpu_s(), t0 = now_ns();
            double replay = 0;
            for (int f = 0; f < frames; f++) {
                const double a = now_ns();
                {
                    std::lock_guard lk(g_replay_mu);
                    for (int d = 0; d < draws; d++) {
                        spin_for(d % 50 == 49 ? 64000 : 4000);   // (and a 60 us stretch of translation every 50 draws)
                        draw_typical(cmd, layout, d);
                        if ((d + 1) % 120 == 0 || d + 1 == draws) rec::boundary(rec::B_vkEndCommandBuffer);
                    }
                }
                replay += now_ns() - a;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            g_spin_ns = 0;
            const double wall = (now_ns() - t0) / 1e9, cpu = recorder_cpu_s() - c0;
            std::printf("IDLE spin %s us, frames (2000 draws x 4 us, 60 us more every 50, an end every 120, 5 ms between): replay %.2f ms a frame, waited "
                        "%.3f ms a frame (max %.1f us); recorder CPU %.2f of a core by thread times (busy %.2f, spun %.2f s of %.2f s), "
                        "slept %llu times a frame\n",
                        spin_us ? std::to_string(spin_us).c_str() : "adaptive", replay / frames / 1e6,
                        (double)(rec::g_rec.wait_ns[rec::B_vkEndCommandBuffer].load() - wt0) / frames / 1e6,
                        rec::g_rec.wait_max[rec::B_vkEndCommandBuffer].load() / 1e3, cpu / wall,
                        (double)(rec::g_rec.busy_ns.load() - bu0) / 1e9, (double)(rec::g_rec.spin_ns.load() - sp0) / 1e9, wall,
                        (unsigned long long)(rec::g_rec.sleeps.load() - sl0) / frames);
            (void)wn0;
        }
        rec::set_mode(rec::OFF);
    }
    rec::g_rec.stop();
    g_quiet = false;
    return 0;
}

/// RX_VK_RECORDER=thread: the recorder thread's checks (also run alone, --only thread, for the broken variants)
static void thread_checks(const std::vector<Call>& ref) {
    rec::g_stats_log = false;
    const bool was_poison = rec::g_poison.load();
    rec::g_poison = true;   // (each packet filled with 0xCD once run: a pointer kept into a run packet would show)
    // the frame, every vkCmd* run by the recorder thread
    {
        const uint64_t c0 = g_cmd_calls, off0 = g_cmd_off_main;
        same("thread (packed, run by the recorder thread in order), caller memory poisoned", ref, run(frame, rec::THREAD, true));
        const uint64_t c = g_cmd_calls - c0, off = g_cmd_off_main - off0;
        if (c != ref.size() || off != c) std::printf("FAIL thread: %llu of %llu vkCmd* on the recorder thread\n", (unsigned long long)off, (unsigned long long)c), failures++;
        else std::printf("PASS thread: all %llu vkCmd* made on the recorder thread\n", (unsigned long long)off);
    }
    g_delay_ns = 1000;   // (the recorder behind the producer: a missed wait shows as a call out of place or on a busy command buffer)
    const Out lref = run_tracked(lifecycle, rec::OFF, false);
    const uint64_t fb0 = rec::t_arena.fallbacks;
    g_hold_ms = 50;   // (and held at each point the producer must wait: hold())
    const Out lt = run_tracked(lifecycle, rec::THREAD, true);
    g_hold_ms = 0, g_hold_until_ns = 0;
    check_run("thread lifecycle (2 command buffers interleaved; reset, begin, end, submit, wait x2; query read; destruction; caller "
              "memory poisoned; driver 1 us a call)", lref.log, lt);
    if (rec::t_arena.fallbacks - fb0 != 4) std::printf("FAIL thread lifecycle: %llu fallbacks, 4 expected\n", (unsigned long long)(rec::t_arena.fallbacks - fb0)), failures++;
    // (each thread run with the recorder held at the points the producer must wait: hold(); the direct references without)
    const auto held = [](void (*scenario)(), bool lock) {
        g_hold_ms = 50;
        Out o = run_tracked(scenario, rec::THREAD, true, false, lock);
        g_hold_ms = 0, g_hold_until_ns = 0;
        return o;
    };
    check_run("thread handoff (a recording across two threads under the replay lock)", run_tracked(handoff, rec::OFF, false).log,
              held(handoff, false));
    check_run("thread: a submit while another command buffer records (its commands in flight)", run_tracked(two_recordings, rec::OFF, false).log,
              held(two_recordings, true));
    check_run("thread: a pool reset while its command buffer records (re-begun)", run_tracked(reset_while_recording, rec::OFF, false).log,
              held(reset_while_recording, true));
    check_run("thread: the replay lock let go, then the recording ended on another thread without it",
              run_tracked(unlock_then_end, rec::OFF, false).log, held(unlock_then_end, false));
    {   // (the status reads made without waiting: the same values as direct, each made while packets were in flight)
        const Out sref = run_tracked(status_reads, rec::OFF, false);
        const Out st = run_tracked(status_reads, rec::THREAD, true);
        check_run("thread: status reads mid-recording (semaphore value and wait, fence wait, invalidate, checkpoints, fault info) "
                  "without waiting for the recorder", sref.log, st, &sref.reads);
        // (each made without waiting exactly when rec::unordered() lists it: then with packets in flight; else always after them)
        const std::pair<const char*, rec::Bound> calls[] = {
            {"vkGetSemaphoreCounterValue", rec::B_vkGetSemaphoreCounterValue}, {"vkWaitSemaphores", rec::B_vkWaitSemaphores},
            {"vkWaitForFences", rec::B_vkWaitForFences}, {"vmaInvalidateAllocation", rec::B_vmaInvalidateAllocation},
            {"vkGetQueueCheckpointDataNV", rec::B_vkGetQueueCheckpointDataNV}, {"vkGetDeviceFaultInfoEXT", rec::B_vkGetDeviceFaultInfoEXT}};
        std::string ran, waited;
        for (const auto& [c, b] : calls) {
            const auto it = st.overlapped.find(c);
            const uint64_t n = it == st.overlapped.end() ? 0 : it->second;
            if (rec::unordered(b) && !n)
                std::printf("FAIL thread status reads: %s (in unordered()) always waited for the recorder\n", c), failures++;
            if (!rec::unordered(b) && n)
                std::printf("FAIL thread status reads: %s made %llu times with packets in flight: it must wait\n", c, (unsigned long long)n), failures++;
            std::string& to = rec::unordered(b) ? ran : waited;
            to += std::string(to.empty() ? "" : ", ") + c + (rec::unordered(b) ? " " + std::to_string(n) : std::string());
        }
        if (sref.reads.size() < 60) std::printf("FAIL thread status reads: only %zu values read\n", sref.reads.size() / 2), failures++;
        std::printf("PASS thread status reads: %zu values identical to direct; made with packets in flight: %s; waited: %s\n",
                    st.reads.size() / 2, ran.empty() ? "(none)" : ran.c_str(), waited.empty() ? "(none)" : waited.c_str());
        check_run("thread: a buffer destroyed while a recording names it (the commands naming it run first)",
                  run_tracked(destroy_recorded, rec::OFF, false).log, run_tracked(destroy_recorded, rec::THREAD, true));
    }
    {   // (teardown with work queued: stopped, joined, started again; vkDestroyDevice stops it too)
        static size_t at_stop;
        static bool joined_at_stop;
        g_at_stop = [] { at_stop = g_log.size(); };
        const Out tref = run_tracked(teardown, rec::OFF, false);
        const size_t ref_at = at_stop;
        g_at_stop = [] {
            rec::g_rec.stop();
            std::lock_guard lk(g_log_mu);
            at_stop = g_log.size(), joined_at_stop = !rec::g_rec.th.joinable();
            if (!joined_at_stop) std::printf("FAIL thread teardown: stop() returned with the recorder thread not joined\n"), failures++;
        };
        const uint64_t want_starts = rec::g_rec.ready.load() ? 1 : 2;   // (running: started again after the stop; else at the first packet too)
        const uint64_t starts0 = rec::g_rec.starts.load();
        const Out t = run_tracked(teardown, rec::THREAD, true);
        check_run("thread teardown (stopped with 4 frames queued, started again, stopped at vkDestroyDevice)", tref.log, t);
        const uint64_t starts = rec::g_rec.starts.load() - starts0;
        if (at_stop != ref_at || !joined_at_stop || rec::g_rec.th.joinable() || starts != want_starts)
            std::printf("FAIL thread teardown: %zu of %zu calls in when stop() returned, joined %d, joinable after vkDestroyDevice %d, "
                        "started %llu times (%llu expected)\n", at_stop, ref_at, (int)joined_at_stop, (int)rec::g_rec.th.joinable(),
                        (unsigned long long)starts, (unsigned long long)want_starts), failures++;
        else std::printf("PASS thread teardown: all %zu queued calls run when stop() returned, the thread joined; started again lazily; "
                         "joined at vkDestroyDevice\n", at_stop);
    }
    {   // (backpressure: an 8 KB ring, 2 KB of room a packet)
        rec::g_rec.stop();
        const size_t bytes = rec::g_ring_bytes, room = rec::g_ring_room;
        rec::g_ring_bytes = 8192, rec::g_ring_room = 2048;
        const uint64_t full0 = rec::g_rec.wait_n[rec::B_RING_FULL].load(), d0 = rec::g_rec.direct.load();
        check_run("thread, an 8 KB ring (the producer waits for room)", lref.log, run_tracked(lifecycle, rec::THREAD, true));
        const uint64_t full = rec::g_rec.wait_n[rec::B_RING_FULL].load() - full0;
        if (!full) std::printf("FAIL thread 8 KB ring: the producer never waited for room\n"), failures++;
        else std::printf("PASS thread 8 KB ring: the producer waited for room %llu times\n", (unsigned long long)full);
        const Out oref = run_tracked(oversize, rec::OFF, false);
        check_run("thread, a packet bigger than its room (4 KB vkCmdUpdateBuffer, 2 KB room): made directly in order", oref.log,
                  run_tracked(oversize, rec::THREAD, true));
        if (rec::g_rec.direct.load() - d0 != 1) std::printf("FAIL thread oversize: %llu calls made directly, 1 expected\n", (unsigned long long)(rec::g_rec.direct.load() - d0)), failures++;
        // (again with the ring empty: stopped, its counters moved on to the ring's start, so the 8 KB free ahead of the first
        // packet would hold it; the 2 KB room must still decide)
        rec::g_rec.stop();
        const uint64_t at = (rec::g_rec.W.load() + 8191) & ~uint64_t(8191);
        rec::g_rec.W.store(at), rec::g_rec.R.store(at);
        const uint64_t d1 = rec::g_rec.direct.load();
        check_run("thread, the same packet first with the ring empty: made directly in order",
                  run_tracked(oversize_first, rec::OFF, false).log, run_tracked(oversize_first, rec::THREAD, true));
        if (rec::g_rec.direct.load() - d1 != 1)
            std::printf("FAIL thread oversize, ring empty: %llu calls made directly, 1 expected\n",
                        (unsigned long long)(rec::g_rec.direct.load() - d1)), failures++;
        rec::g_rec.stop();
        rec::g_ring_bytes = bytes, rec::g_ring_room = room;
    }
    {   // (no memory for the ring: every call made directly, logged once)
        rec::g_rec.stop();
        g_aligned_calls = 0, g_fail_nth = 1;
        const Out o = run_tracked(lifecycle, rec::THREAD, true);
        g_fail_nth = 0;
        check_run("thread, no memory for the ring (every call made directly)", lref.log, o);
        if (!rec::g_rec.broken.load()) std::printf("FAIL thread no ring: the recorder ran\n"), failures++;
        rec::g_rec.broken = false;
    }
    {   // (a parallel replay records: each packet made at once, after what was published)
        par_active = true;
        const std::vector<Call> got = run(frame, rec::THREAD, true);
        par_active = false;
        same("thread while par_active (each call made directly)", ref, got);
    }
    g_delay_ns = 0;
    {   // (stress: >= 1e6 packets through a 16 KB ring, twice; each run's stream hashed, against the direct one's)
        rec::g_rec.stop();
        const size_t bytes = rec::g_ring_bytes, room = rec::g_ring_room;
        rec::g_ring_bytes = 16384, rec::g_ring_room = 4096;
        g_stress_frames = (int)(1000000 / ref.size()) + 1;
        uint64_t h[3];
        uint64_t calls = 0, full = 0;
        for (int i = 0; i < 3; i++) {
            g_hash_only = true, g_hash = 0xcbf29ce484222325ull;
            const uint64_t c0 = g_cmd_off_main, f0 = rec::g_rec.wait_n[rec::B_RING_FULL].load();
            const Out o = run_tracked(stress, i ? rec::THREAD : rec::OFF, i != 0);
            g_hash_only = false;
            h[i] = g_hash;
            if (o.violations) std::printf("FAIL thread stress: %s\n", o.violation.c_str()), failures++;
            if (i) calls += g_cmd_off_main - c0, full += rec::g_rec.wait_n[rec::B_RING_FULL].load() - f0;
        }
        if (h[1] != h[0] || h[2] != h[0] || calls < 2000000)
            std::printf("FAIL thread stress: hashes %016llx (direct) %016llx %016llx, %llu calls on the recorder\n", (unsigned long long)h[0],
                        (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)calls), failures++;
        else std::printf("PASS thread stress: 2 runs of %llu packets through a 16 KB ring (%llu waits for room), each stream identical "
                         "to the direct one (hash %016llx)\n", (unsigned long long)(calls / 2), (unsigned long long)full, (unsigned long long)h[0]);
        rec::g_rec.stop();
        rec::g_ring_bytes = bytes, rec::g_ring_room = room;
    }
    rec::g_poison = was_poison;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(0x0002 | 0x0001);   // (SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS)
#endif
#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);   // (a fault or abort ends it without a core dump: no crash report, no notification)
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // (a broken variant may end the process: what it printed before is kept)
    // (the switch as the renderer reads it: first use, from the environment)
    std::printf("env mode %d\n", rec::mode());
    const int env_mode = rec::mode();
    g_main_id = std::this_thread::get_id();
    if (argc > 2 && !std::strcmp(argv[1], "--bench")) return bench(std::atoi(argv[2]));
    if (argc > 2 && !std::strcmp(argv[1], "--overlap")) return overlap(std::atoi(argv[2]));
    if (argc > 2 && !std::strcmp(argv[1], "--idle")) return idle_bench(std::atoi(argv[2]));
    if (argc > 1 && !std::strcmp(argv[1], "--throw")) return throw_test();
    const bool only_thread = argc > 2 && !std::strcmp(argv[1], "--only") && !std::strcmp(argv[2], "thread");

    const std::vector<Call> ref = run(frame, rec::OFF, false);
    if (only_thread) {
        thread_checks(ref);
        std::printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
        return failures ? 1 : 0;
    }
    same("direct, caller memory reused after each call", ref, run(frame, rec::OFF, true));
    if (env_mode != rec::OFF) same("RX_VK_RECORDER from the environment, caller memory reused", ref, run(frame, env_mode, true));
    rec::t_arena.packets = 0;
    same("sync (packed, run at once), caller memory reused", ref, run(frame, rec::SYNC, true));
    if (rec::t_arena.packets != ref.size()) std::printf("FAIL sync: %llu packets for %zu calls\n", (unsigned long long)rec::t_arena.packets, ref.size()), failures++;
    if (rec::t_arena.ci || rec::t_arena.used) std::printf("FAIL sync: the arena not rewound\n"), failures++;
    same("deferred (packed, run at the drain), caller memory poisoned in between", ref, run(frame, rec::DEFER, true));
    if (rec::t_arena.fallbacks) std::printf("FAIL: %llu calls not packed\n", (unsigned long long)rec::t_arena.fallbacks), failures++;

    const std::vector<Call> fref = run(fallbacks, rec::OFF, false);
    same("fallback order (pNext chain, inline uniform block), deferred", fref, run(fallbacks, rec::DEFER, false));
    if (rec::t_arena.fallbacks != 2) std::printf("FAIL: %llu fallbacks, 2 expected\n", (unsigned long long)rec::t_arena.fallbacks), failures++;

    // ---- defer (step 1.5): the lifecycle, the replay lock, the budgets, an allocation failure, a parallel replay
    const Out lref = run_tracked(lifecycle, rec::OFF, false);
    if (lref.violations) std::printf("FAIL the lifecycle scenario itself: %s\n", lref.violation.c_str()), failures++;
    const Out ld = run_tracked(lifecycle, rec::DEFER, true);
    check_run("defer lifecycle (2 command buffers interleaved; reset, begin, end, submit, wait x2; query read; destruction; caller memory "
              "poisoned)", lref.log, ld);
    if (ld.fallbacks != 4) std::printf("FAIL defer lifecycle: %llu fallbacks, 4 expected\n", (unsigned long long)ld.fallbacks), failures++;
    const Out href = run_tracked(handoff, rec::OFF, false);
    check_run("defer handoff (a recording across two threads under the replay lock)", href.log, run_tracked(handoff, rec::DEFER, true));
    {
        const size_t soft = rec::g_soft_budget;
        rec::g_soft_budget = 2048;
        const Out o = run_tracked(lifecycle, rec::DEFER, true);
        rec::g_soft_budget = soft;
        check_run("defer soft budget 2 KB (held packets drained first past it)", lref.log, o);
        if (!o.budget_drains) std::printf("FAIL defer soft budget: no drain at the budget\n"), failures++;
        else std::printf("PASS defer soft budget: %llu drains at the budget\n", (unsigned long long)o.budget_drains);
    }
    const Out bref = run_tracked(big, rec::OFF, false);
    {
        const size_t hard = rec::g_hard_budget;
        rec::g_hard_budget = size_t(1) << 20;
        const Out o = run_tracked(big, rec::DEFER, true, true);
        rec::g_hard_budget = hard;
        check_run("defer hard budget 1 MB (past it: what is held drained, the call made directly)", bref.log, o);
        if (!o.failures || o.capacity > (size_t(1) << 20))
            std::printf("FAIL defer hard budget: %llu calls past it, %zu bytes of chunks\n", (unsigned long long)o.failures, o.capacity), failures++;
        else std::printf("PASS defer hard budget: %llu calls made directly, %zu bytes of chunks\n", (unsigned long long)o.failures, o.capacity);
    }
    {
        const long live = g_aligned_live.load();
        g_aligned_calls = 0, g_fail_nth = 2;
        const Out o = run_tracked(big, rec::DEFER, true, true);
        g_fail_nth = 0;
        check_run("defer allocation failure (the second chunk's allocation fails)", bref.log, o);
        if (o.failures != 1 || o.leaked || g_aligned_live.load() != live)
            std::printf("FAIL defer allocation failure: %llu failures (1 expected), %ld blocks lost, %ld left after the thread\n",
                        (unsigned long long)o.failures, o.leaked, g_aligned_live.load() - live), failures++;
        else std::printf("PASS defer allocation failure: held packets drained, the call made directly, no block lost\n");
    }
    {   // (a parallel replay records: never held, each packet run at once)
        par_active = true;
        rec::set_mode(rec::DEFER);
        g_mutate = true, g_log.clear();
        frame();
        const bool at_once = g_log.size() == ref.size() && !rec::t_arena.head;
        rec::drain();
        const std::vector<Call> got = g_log;
        rec::set_mode(rec::OFF);
        par_active = false;
        if (!at_once) std::printf("FAIL defer while par_active: packets held\n"), failures++;
        same("defer while par_active (each packet run at once)", ref, got);
    }

    thread_checks(ref);

    {   // (B_DIRECT: code that records into the command buffer directly, as Dear ImGui's backend does through its own pointers,
        // after packed calls: with the boundary the driver sees packed, direct, packed in order; without it (the control) not)
        const auto order = [](int mode, bool bounded) {
            const VkCommandBuffer cmd = H<VkCommandBuffer>(0x300);
            rec::set_mode(mode);
            g_log.clear();
            // (thread: with the boundary, each packed call takes 0.3 ms so a missing wait would show; without it (the
            // control), the recorder is held at the fake until the direct call is made, so the wrong order is certain)
            if (mode == rec::THREAD) bounded ? (void)(g_delay_ns = 300000) : (void)(g_gate = true);
            {
                std::lock_guard lk(g_replay_mu);
                vkCmdSetLineWidth(cmd, 1.0f);
                vkCmdSetLineWidth(cmd, 2.0f);
                if (bounded) rec::boundary(rec::B_DIRECT);
                fake_vkCmdSetLineWidth(cmd, 9.0f);   // (opaque: not through vk_work.h)
                g_gate = false;
                vkCmdSetLineWidth(cmd, 3.0f);
            }
            rec::drain();
            g_delay_ns = 0;
            std::string seq;
            for (const Call& c : g_log) {
                float v;
                std::memcpy(&v, c.bytes.data() + c.bytes.size() - sizeof v, sizeof v);
                seq += (char)('0' + (int)v);
            }
            rec::set_mode(rec::OFF);
            return seq;
        };
        for (const int mode : {rec::DEFER, rec::THREAD}) {
            const char* name = mode == rec::DEFER ? "defer" : "thread";
            const std::string with = order(mode, true), without = order(mode, false);
            if (with != "1293") std::printf("FAIL %s B_DIRECT: the driver saw %s, 1293 expected\n", name, with.c_str()), failures++;
            else if (without == "1293") std::printf("FAIL %s B_DIRECT control: without the boundary the order was still 1293\n", name), failures++;
            else std::printf("PASS %s B_DIRECT: packed, direct, packed reach the driver in order (%s; without the boundary %s)\n", name,
                             with.c_str(), without.c_str());
        }
    }

    std::set<std::string> seen;
    for (const Call& c : ref) seen.insert(c.op);
    int missing = 0;
    for (uint32_t op = 0; op < rec::OP_COUNT; op++)
        if (!seen.count(rec::op_name(op))) std::printf("NOT COVERED %s\n", rec::op_name(op)), missing++;
    if (missing) failures++;
    else std::printf("PASS coverage: all %d packed calls made by the frame\n", (int)rec::OP_COUNT);
    std::printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
