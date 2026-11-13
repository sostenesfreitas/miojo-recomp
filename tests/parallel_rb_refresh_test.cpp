// The readback refresh (PR 430) in a parallel replay's submission: the production memory_rb_refresh and its bookkeeping,
// texture_begin_recording / texture_uploads_recorded, the write-back note at the end of gpu_buffer_bind and vk_draw.cpp's
// storage-descriptor binding, included from rb_actual.inc (tests/test_parallel_rb_refresh.py cuts them from the source).
// Vulkan is a spy: copies are recorded and run later, reading their source when they run, as the GPU reads guest memory.
// The submission's begin/end (epoch, write-back) and replay_handle's order of recordings are modelled here.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

struct FakeBuf {
    const char* name;
    std::vector<uint8_t> mem;
};
using VkBuffer = FakeBuf*;
using VkDeviceMemory = void*;
using VmaAllocation = void*;
using VkDeviceAddress = uint64_t;
#define VK_NULL_HANDLE nullptr
struct VkBufferCopy {
    uint64_t srcOffset, dstOffset, size;
};
struct Cmd {
    char op;   // c: copy, a: add, s: store, r: read
    VkBuffer src, dst;
    std::vector<VkBufferCopy> r;
    uint64_t off;
    uint32_t val;
};
struct FakeCB {
    std::string name;
    std::vector<Cmd> cmds;
};
using VkCommandBuffer = FakeCB*;
void vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
    cb->cmds.push_back({'c', s, d, std::vector<VkBufferCopy>(r, r + n), 0, 0});
}

namespace mkwvk {
constexpr uint64_t kPageShift = 12;
void logf(const char*, ...) {}
struct GprofSpan {
    int items = 0;
    uint64_t bytes = 0;
    GprofSpan(VkCommandBuffer, const char*) {}
};
void full_barrier(VkCommandBuffer, const char* = "") {}
struct Host {   // (GPU addresses below 1 TB are guest addresses)
    bool gpu_ptr(uint64_t, uint64_t* avail) const { return *avail = ~0ull, true; }
    uint64_t gpu_guest(uint64_t g) const { return g; }
} const host_object;
const Host* host = &host_object;
std::atomic<bool> par_active{false};
void memory_table_begin() {}
bool memory_table_flush() { return false; }
void memory_protect_gpu(uint64_t, uint64_t) {}
bool slow_desc_timing() { return false; }
uint64_t texture_segment();
void memory_rb_recording_begun();
void memory_rb_segments_begin(uint64_t base, uint64_t n);
void memory_note_gpu_write(uint64_t gpu, uint64_t size);
bool memory_rb_refresh(VkCommandBuffer up);
#include "rb_bufferref.inc"
std::mutex pools_mu;
#include "rb_pool.inc"
std::map<uint64_t, Pool> imported;
uint64_t mirror_epoch = 0;
bool mirror_upload = false;
#include "rb_writeback.inc"
std::vector<WriteBack> writebacks;
void mirror_sync(Pool& p) {   // (model: no CPU page is dirty here)
    p.synced_epoch = mirror_epoch;
    p.sync_seg = texture_segment();
}
#include "rb_refresh.inc"
// (gpu/vk/wb_learn.h's table: every write-back queued in this model; tests/test_wb_learn.py runs its rules)
static bool wb_leave_out(uint64_t, uint64_t, std::vector<std::pair<uint64_t, uint64_t>>&) { return false; }
static bool wb_skips_log() { return false; }
static void wb_skip_note(uint64_t, uint64_t) {}

/// gpu_buffer_bind: the pool's mirror when it has one (model of its front), then its actual last lines
BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {
    std::lock_guard lk(pools_mu);
    auto it = imported.upper_bound(gpu);
    if (it == imported.begin()) return {};
    Pool* p = &(--it)->second;
    if (gpu + size > p->hi) return {};
    const uint64_t off = gpu - p->lo;
    if (!mirror_upload || p->mstate != Pool::MIRROR_ON) return {p->buffer, off, size, 0};
#include "rb_bindtail.inc"
    return {p->mirror, off, size, 0};
}
uint64_t gpu_pool_end(uint64_t gpu) {
    std::lock_guard lk(pools_mu);
    auto it = imported.upper_bound(gpu);
    return it == imported.begin() ? 0 : (--it)->second.hi;
}

thread_local VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;
thread_local VkCommandBuffer last_pass_end_cmd = VK_NULL_HANDLE;
thread_local VkCommandBuffer up_cmd = VK_NULL_HANDLE;
uint64_t rec_value = 0;
thread_local bool up_used = false;
thread_local uint64_t tl_seg = 0;
uint32_t epoch = 0;
#include "rb_texture.inc"

/// A storage descriptor bound as vk_draw.cpp binds it (its actual code: the bare entries' guess, then the binding)
struct Desc {
    uint32_t count;
    bool is_written;
    uint32_t cbuf_index, cbuf_offset;
};
BufferRef bind_storage(const Desc& d, uint64_t addr, uint64_t size, uint64_t* base_out) {
    static const uint64_t bare_read = 64ull << 20;   // (the device's reach)
    const uint64_t salign = 16;                      // (minStorageBufferOffsetAlignment)
    bool bare = false;
#include "rb_draw.inc"
    (void)t0;
    *base_out = base;
    return b;
}
}  // namespace mkwvk
using namespace mkwvk;

// ---- the world ----
constexpr uint64_t LO = 0x10000000, SZ = 0x10000, A = LO + 0x2000;
FakeBuf G{"G", std::vector<uint8_t>(SZ)}, M{"M", std::vector<uint8_t>(SZ)};
std::vector<FakeCB*> queue;
std::string stream;   // (every copy the GPU ran, in order)
uint32_t& word(FakeBuf& b, uint64_t guest) { return *reinterpret_cast<uint32_t*>(b.mem.data() + (guest - LO)); }
void run_gpu() {
    for (FakeCB* cb : queue)
        for (Cmd& c : cb->cmds) {
            uint32_t* w = c.op == 'c' ? nullptr : reinterpret_cast<uint32_t*>(c.dst->mem.data() + c.off);
            if (c.op == 'c')
                for (const VkBufferCopy& r : c.r) {
                    std::memcpy(c.dst->mem.data() + r.dstOffset, c.src->mem.data() + r.srcOffset, r.size);
                    char line[128];
                    std::snprintf(line, sizeof line, "%s: %s->%s +%#llx %#llx\n", cb->name.c_str(), c.src->name, c.dst->name,
                                  (unsigned long long)r.srcOffset, (unsigned long long)r.size);
                    stream += line;
                }
            else if (c.op == 'a') *w += c.val;
            else if (c.op == 's') *w = c.val;
            else c.val = *w;   // (a read: what it saw)
        }
    queue.clear();
}
void setup() {
    Pool p;
    p.lo = LO, p.hi = LO + SZ, p.buffer = &G, p.mirror = &M, p.mstate = Pool::MIRROR_ON;
    imported.emplace(LO, p);
}
// (memory_begin_submission / memory_end_submission, modelled: the epoch, and the write-back of what was marked written)
void begin_submission() {
    std::lock_guard lk(pools_mu);
    mirror_epoch++, mirror_upload = true, writebacks.clear();
}
void end_submission(VkCommandBuffer cmd) {
    std::lock_guard lk(pools_mu);
    for (const WriteBack& w : writebacks) cmd->cmds.push_back({'c', &M, &G, {{w.lo, w.lo, w.hi - w.lo}}, 0, 0});
    writebacks.clear(), mirror_upload = false;
}
struct Rec {
    FakeCB *cmd, *up;
};
std::vector<FakeCB*> cbs;
Rec begin_recording(const char* name) {   // (vk_replay.cpp begin_recording: then texture_begin_recording)
    Rec r{new FakeCB{std::string(name) + ".cmd", {}}, new FakeCB{std::string(name) + ".up", {}}};
    last_pass_end_cmd = r.cmd;
    texture_begin_recording(r.up, 1);
    if (last_pass_end_cmd != VK_NULL_HANDLE) {
        std::fputs("a new recording kept the previous pass-end command buffer\n", stderr);
        std::exit(1);
    }
    return r;
}
void submit(const Rec& r) {   // (submit_recording: the uploads, the refresh last of them, ahead of the work)
    if (texture_uploads_recorded()) queue.push_back(r.up);
    queue.push_back(r.cmd);
}
void add(VkCommandBuffer cmd, uint64_t g, uint32_t v) {   // (a draw adding into a word bound written)
    const BufferRef b = gpu_buffer_bind(g, 16, true);
    cmd->cmds.push_back({'a', nullptr, b.buffer, {}, b.offset, v});
}
/// replay_handle's parallel block: the part before submitted, the segments recorded on their threads (one after
/// another, in order) and submitted in order, then the recording after them, in the same submission
Rec parallel(Rec& r, const std::vector<std::function<void(VkCommandBuffer)>>& segs) {
    submit(r);
    static uint64_t seq = 0;
    const uint64_t base = ++seq;
    seq += segs.size();
#if HAVE_SEGMENTS_BEGIN
    memory_rb_segments_begin(base, segs.size());
#endif
    par_active = true;
    for (size_t k = 0; k < segs.size(); k++) {
        Rec s{new FakeCB{"seg" + std::to_string(k + 1) + ".cmd", {}}, new FakeCB{"seg" + std::to_string(k + 1) + ".up", {}}};
        bool uploads = false;
        std::thread([&] {
            texture_set_segment(base + k);
            texture_begin_recording(s.up, 1);
            segs[k](s.cmd);
            uploads = texture_uploads_recorded();
        }).join();
        if (uploads) queue.push_back(s.up);
        queue.push_back(s.cmd);
    }
    par_active = false;
    texture_set_segment(++seq);
    return begin_recording("after");
}

int fails = 0;
void expect(const char* what, uint32_t got, uint32_t want) {
    std::printf("%s %s: %u (expected %u)\n", got == want ? "ok  " : "FAIL", what, got, want);
    fails += got != want;
}

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    setup();
    memory_note_readback(A, 16);   // (the CPU reads A back: OoT's exposure meter)
    if (which == "parallel") {
        // P1: the part before, 3 segments and the part after each add 1 into the meter, in one submission; the CPU's 0
        // lands after the recording, before the GPU runs it
        Rec r = begin_recording("before");
        begin_submission();
        add(r.cmd, A, 1);
        auto one = [](VkCommandBuffer c) { add(c, A, 1); };
        Rec after = parallel(r, {one, one, one});
        add(after.cmd, A, 1);
        end_submission(after.cmd);
        submit(after);
        word(G, A) = 0;
        run_gpu();
        expect("parallel: the meter in guest memory after the write-back (the GPU's sum)", word(G, A), 5);
    } else if (which == "untracked") {
        // P12: a bare table entry (no size: bound written=false, 1 MB guessed, only its first 64 bytes noted written) stores
        // into A's page past that head in segment 1; segment 3 reads it back
        const Desc table{2, true, 3, 0x40};
        const uint64_t at = LO + 0x1000, w = A + 0x80;
        Cmd* read = nullptr;
        Rec r = begin_recording("before");
        begin_submission();
        Rec after = parallel(r, {[&](VkCommandBuffer c) {
                                     uint64_t base = 0;
                                     const BufferRef b = bind_storage(table, at, 0, &base);
                                     c->cmds.push_back({'s', nullptr, b.buffer, {}, b.offset + (w - base), 7});
                                 },
                                 [](VkCommandBuffer) {},
                                 [&](VkCommandBuffer c) {
                                     uint64_t base = 0;
                                     const BufferRef b = bind_storage(table, at, 0, &base);
                                     c->cmds.push_back({'r', nullptr, b.buffer, {}, b.offset + (w - base), 0});
                                     read = &c->cmds.back();
                                 }});
        end_submission(after.cmd);
        submit(after);
        run_gpu();
        expect("untracked writer: segment 3 reads what segment 1 stored past the bare entry's head", read->val, 7);
    } else if (which == "serial") {
        // serial replay: one recording a submission, then one after its end (frame generation's); the CPU writes A
        // between them. The copies the GPU runs must be today's, exactly
        for (int f = 1; f <= 3; f++) {
            Rec r = begin_recording(("serial" + std::to_string(f)).c_str());
            begin_submission();
            add(r.cmd, A, 2);
            end_submission(r.cmd);
            submit(r);
            word(G, A) = 0;
            run_gpu();
            word(G, A) += 10;
            Rec g = begin_recording(("framegen" + std::to_string(f)).c_str());
            submit(g);
            run_gpu();
        }
        static const char* const want =
            "serial1.up: G->M +0x2000 0x1000\nserial1.cmd: M->G +0x2000 0x10\nframegen1.up: G->M +0x2000 0x1000\n"
            "serial2.up: G->M +0x2000 0x1000\nserial2.cmd: M->G +0x2000 0x10\nframegen2.up: G->M +0x2000 0x1000\n"
            "serial3.up: G->M +0x2000 0x1000\nserial3.cmd: M->G +0x2000 0x10\nframegen3.up: G->M +0x2000 0x1000\n";
        if (stream != want) std::printf("FAIL serial: the copies differ from today's:\n%s", stream.c_str());
        fails += stream != want;
        expect("serial: the meter in guest memory", word(G, A), 12);
    } else {
        std::fprintf(stderr, "usage: parallel | untracked | serial\n");
        return 2;
    }
    return fails ? 1 : 0;
}
