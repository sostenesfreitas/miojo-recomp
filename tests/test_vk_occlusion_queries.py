"""Real occlusion counts on Vulkan (RX_VK_OCC_QUERIES, gpu/vk/vk_replay.cpp): Ocarina of Time reads its samples-passed
reports back on the CPU and leaves out of its draw lists the objects whose count was 0 (docs/development.md). The rule
under test: a count that is not known for sure stays "visible" (4096); a wrong 0 hides an object that may be in view.

The real code is cut out of the sources and compiled against a fake query pool, command buffer and guest memory:

- vk_replay.cpp's ring (from kOccPool to occ_take): the numbering (one counter for every queue's state, a run split by
  another queue's queries left visible, a queue's state copied mid-run), the reset and report cases' helpers, the ring's
  room, the harvest (occ_collect), the report's submission value, the slot's report sequence (two reports into one slot
  within one timestamp tick), and a slot made visible ahead of a stopped queue;
- vk_draw.cpp's OccGuard and the mesh fallback's task-batch block, inside a fake draw(): a draw that returns after its
  pass began, or whose vertex input was not all bound, or that is split into task batches, begins no query, and its
  report stays visible; every query is begun at most once;
- occ_queries_on() in its own process per environment: RX_VK_OCC_FIXED wins over RX_VK_OCC_QUERIES=1, and the queries
  are off with RX_VK_ASYNC=1.

The rest is checked in the source: where the helpers are called, every draw command in draw() preceded by its query's
issue with no way out between, the guard made after StatsQuery, the stopped queues' reports made visible.

    python -I -B tests/test_vk_occlusion_queries.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
REPLAY = ROOT / "gpu" / "vk" / "vk_replay.cpp"
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def ring_code(text):
    """vk_replay.cpp's ring: from its constants to occ_take, without occ_queries_on (the pool is the fixture's)"""
    start = text.index("constexpr uint64_t kOccPool")
    end = text.index("/// The next recording slot, begun")
    code = text[start:end]
    return code.replace(block(code, "bool occ_queries_on()"), "")


def guard_code(text):
    """vk_draw.cpp's OccGuard, with its counters and occ_draw_counts"""
    start = text.index("enum OccNotBegun")
    guard = text[start:text.index("};", text.index("struct OccGuard {", start)) + 2]
    return guard + "\n" + block(text, "void occ_draw_counts(uint64_t out[3]) {") + "\n"


def batch_code(text):
    """draw()'s mesh fallback task batches: one draw of the game's as several"""
    return block(block(text, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {"),
                 "if (p.fallback_task && !d.indirect) {")


COMMON = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
typedef struct VkQueryPool_T* VkQueryPool;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef int VkResult;
typedef struct VkDevice_T* VkDevice;
#define VK_NULL_HANDLE nullptr
#define VK_SUCCESS 0
#define VK_NOT_READY 1
#define VK_QUERY_RESULT_64_BIT 1
#define VK_QUERY_RESULT_WITH_AVAILABILITY_BIT 4
#define VK_QUERY_CONTROL_PRECISE_BIT 1
void logf(const char*, ...) {}
'''

FIXTURE = COMMON + r'''
struct { VkDevice device = nullptr; bool occlusion_precise = true; } dev;
struct NvnState {
    bool render_enabled = true, raster_discard = false;
    uint64_t occ_next = 0, occ_first = 0, occ_limit = 0;
    bool occ_on = false, occ_overflow = false, occ_color = false;
    struct { uint32_t mask = 0; } chmask;   // (no colour written: the game's depth-only boxes)
};
struct DrawArgs {
    bool indirect = false;
    int groups[3] = {0, 1, 1};
    uint32_t first_task = 0;
    VkQueryPool occ_pool = VK_NULL_HANDLE;
    uint32_t occ_query = 0;
};
// the GPU: each query's count and whether it is done; the guest's memory: 8-byte words
struct Q { uint64_t count = 0; bool avail = false; };
std::vector<Q> gpu(1 << 16);
std::map<uint64_t, uint64_t> guest;
uint64_t done_value = 0, rec_part_next = 0;
uint64_t completed() { return done_value; }
struct {
    void gpu_read(uint64_t a, void* d, size_t n) { uint64_t v = guest[a]; std::memcpy(d, &v, n); }
    uint64_t gpu_guest(uint64_t a) { return a; }
} host_obj, *host = &host_obj;
int guest_cpu_write(uint64_t a, const void* s, uint64_t n) { uint64_t v = 0; std::memcpy(&v, s, n); guest[a] = v; return 0; }
void memory_cpu_owned(uint64_t, uint64_t) {}
bool env_on(const char*) { return false; }
bool occ_queries_on() { return true; }
void occ_draw_counts(uint64_t out[3]);
int resets = 0, reads = 0;
// (the rule: a query is reset from the host only once the submission it went out in has completed on the GPU, its
// timeline value reached. Its availability bit is not enough: the command buffer may still be executing (the validation
// layer's VUID-vkResetQueryPool-firstQuery-02741, then -vkCmdBeginQuery-None-00807 at its next use))
std::vector<uint64_t> sent_in(1 << 16);   // (each query's submission value, 0: not taken since its reset)
void vkResetQueryPool(VkDevice, VkQueryPool, uint32_t first, uint32_t n) {
    resets++;
    for (uint32_t i = 0; i < n; i++) {
        if (sent_in[first + i] > done_value) {
            std::fprintf(stderr, "query %u reset while its submission %llu is not done (done %llu)\n", first + i,
                         (unsigned long long)sent_in[first + i], (unsigned long long)done_value);
            std::exit(1);
        }
        gpu[first + i] = {}, sent_in[first + i] = 0;
    }
}
VkResult vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t first, uint32_t n, size_t, void* data, uint64_t stride, int) {
    reads++;
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t* w = (uint64_t*)((char*)data + i * stride);
        w[1] = gpu[first + i].avail;
        if (gpu[first + i].avail) w[0] = gpu[first + i].count;
        else r = VK_NOT_READY;
    }
    return r;
}
// the command buffer: which queries are begun (each at most once until it is reset), and the samples drawn inside them
std::set<uint32_t> open_q, begun_q;
int begins = 0, ends = 0;
bool begun_twice = false;
std::map<uint32_t, uint64_t> drawn;
void vkCmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t q, int) {
    begins++;
    if (begun_q.count(q)) begun_twice = true;
    begun_q.insert(q), open_q.insert(q), drawn[q] = 0;
}
void vkCmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t q) { ends++; open_q.erase(q); }
''' + "%RING%" + "%GUARD%" + r'''
// a draw() with the real task-batch block and guard: path picks how it ends
struct Program { bool fallback_task = false; } prog;
uint32_t task_batch = 0;
uint32_t fallback_task_groups(const Program&) { return task_batch; }
#define SKIP_HERE() return
enum Path { kDraws, kReturnsEarly, kVertexMissing } path = kDraws;
uint64_t samples_each = 0;
void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {
    Program& p = prog;
    (void)s;
''' + "%BATCH%" + r'''
    OccGuard occ(cmd, d);
    if (path == kVertexMissing) occ.drop(kOccVertexInput);
    if (path == kReturnsEarly) return;   // (a way out after the pass began: a missing sampler, a buffer not bound...)
    occ.issue();
    for (uint32_t q : open_q) drawn[q] += samples_each;   // (the draw command)
}
VkCommandBuffer cmd = (VkCommandBuffer)1;
// the GPU runs what was recorded: a begun query is done with what was drawn inside it; one never begun has no result
void gpu_runs() {
    for (uint32_t q : begun_q) gpu[q] = {drawn[q], true};
    begun_q.clear();
}
// the replay's side, through vk_replay.cpp's helpers as its cases call them
NvnState S;
uint64_t value = 10;   // (rec_cur_value)
void reset(NvnState& s = S) { occ_counter_reset(s, false, value); }
std::vector<uint32_t> draws(int n, bool dry = false, NvnState& s = S) {
    std::vector<uint32_t> q;
    for (int i = 0; i < n; i++) {
        DrawArgs da;
        occ_take(s, da, dry);
        if (da.occ_pool) q.push_back(da.occ_query), sent_in[da.occ_query] = value;
    }
    return q;
}
void report(uint64_t addr, uint64_t ts, NvnState& s = S) {
    const OccRun run = occ_close(s, true);
    guest[addr] = 4096, guest[addr + 8] = ts;   // (the replay's "visible", and its timestamp)
    occ_reported(run, addr, ts, false, value);
}
void ran(const std::vector<uint32_t>& q, std::vector<uint64_t> counts) {
    for (size_t i = 0; i < q.size(); i++) gpu[q[i]] = {counts[i], true};
}
uint64_t oldest_other(const NvnState& a, const NvnState& b) { return b.occ_on ? b.occ_first : UINT64_MAX; (void)a; }
void begin(NvnState& s, const NvnState& other) { occ_begin_buffer(s, value, oldest_other(s, other)); }
int main() {
    occ_pool = (VkQueryPool)1;
    NvnState none;
    none.occ_on = false;
    begin(S, none);
    CHECK(S.occ_limit == kOccBuffer);
    value = 10;
    // a run of three draws: its count is their sum, once they have all ended on the GPU
    reset();
    auto a = draws(3);
    CHECK(a.size() == 3 && a[0] == 0 && a[2] == 2);
    report(0x100, 7);
    occ_collect();
    CHECK(guest[0x100] == 4096 && occ_pending.size() == 1 && resets == 0);   // (not run: waits, nothing reset)
    ran({a[0], a[1]}, {0, 5});
    occ_collect();
    CHECK(guest[0x100] == 4096 && occ_pending.size() == 1);   // (two of three ended: waits)
    ran({a[2]}, {0});
    occ_collect();
    CHECK(guest[0x100] == 5 && occ_pending.empty());   // (written as soon as its queries are done...)
    CHECK(gpu[0].avail && resets == 0 && occ_freeing.size() == 1);   // (... reset only once the submission is: not yet)
    occ_next_all = kOccPool - kOccBuffer + 1;   // (the ring's room counts them: a command buffer's worth past them is too much)
    CHECK(!occ_room(UINT64_MAX));
    occ_next_all = S.occ_next;
    done_value = 10;
    occ_collect();
    CHECK(!gpu[0].avail && !gpu[1].avail && resets == 1 && occ_freeing.empty());   // (the ring's queries reset once read and done)
    // hidden: every draw counted 0
    value = 11;
    reset();
    auto b = draws(2);
    report(0x200, 8);
    ran(b, {0, 0});
    done_value = 11;
    occ_collect();
    CHECK(guest[0x200] == 0);
    // a draw that never ran (its query never begun): once the GPU is past the submission, "visible" stands
    value = 12;
    reset();
    auto c = draws(2);
    report(0x300, 9);
    gpu[c[0]] = {0, true};
    occ_collect();
    CHECK(occ_pending.size() == 1);   // (the GPU not past it yet: waits)
    done_value = 12;
    occ_collect();
    CHECK(guest[0x300] == 4096 && occ_pending.empty());
    // the slot reported into again before the count came: the newer report stands
    value = 13;
    reset();
    auto d = draws(1);
    report(0x400, 20);
    guest[0x408] = 21;
    ran(d, {0});
    done_value = 13;
    occ_collect();
    CHECK(guest[0x400] == 4096);
    // ... also within one timestamp tick: the newer report's run is not counted (a colour draw), and its timestamp is the
    // older one's. The older 0 must not land over the newer "visible" (the timestamp word alone let it)
    value = 14;
    reset();
    auto e1 = draws(1);
    report(0x480, 30);
    reset();
    S.chmask.mask = 0xf;
    CHECK(draws(1).empty() && S.occ_overflow);
    S.chmask.mask = 0;
    report(0x480, 30);
    ran(e1, {0});
    done_value = 14;
    occ_collect();
    CHECK(guest[0x480] == 4096 && occ_pending.empty());
    // ... and a slot made "visible" ahead of a stopped queue's report: the older count waiting for it is not written
    reset();
    auto e2 = draws(1);
    report(0x4c0, 31);
    occ_slot_visible(0x4c0);
    CHECK(guest[0x4c0] == 4096);
    guest[0x4c0] = 0;   // (even if something wrote the word since)
    occ_slot_visible(0x4c0);
    ran(e2, {0});
    occ_collect();
    CHECK(guest[0x4c0] == 4096 && occ_pending.empty());
    // two slots sharing an entry of the slot table: the evicted one's count is not written (it stays "visible")
    {
        uint64_t other = 0x5010;
        while (&occ_slot(other) != &occ_slot(0x5000)) other += 0x10;
        reset();
        auto x1 = draws(1);
        report(0x5000, 33);
        reset();
        auto x2 = draws(1);
        report(other, 34);
        ran(x1, {0}), ran(x2, {0});
        occ_collect();
        CHECK(guest[0x5000] == 4096 && guest[other] == 0 && occ_pending.empty());
    }
    // the report's submission value is the one passed (rec_cur_value), not the last recording begun: a present or frame
    // generation recording begins with its own. A query never begun: unknown only once that submission is done
    value = 40;
    reset();
    auto v = draws(1);
    report(0x4e0, 32);
    CHECK(occ_pending.back().value == 40);
    done_value = 39;   // (an earlier value done, as a present's)
    occ_collect();
    CHECK(occ_pending.size() == 1 && guest[0x4e0] == 4096);
    done_value = 40;
    occ_collect();
    CHECK(occ_pending.empty() && guest[0x4e0] == 4096);
    (void)v;
    // in order, and in one batch: every waiting report is read with one call and the finished prefix reset with one,
    // whatever its size; a report still on the GPU waits, and so do those after it
    value = 41;
    std::vector<std::vector<uint32_t>> runs;
    for (int i = 0; i < 40; i++) {
        reset();
        runs.push_back(draws(1 + i % 3));
        report(0x1000 + 16 * i, 100 + i);
        std::vector<uint64_t> k(runs.back().size(), 0);
        k[0] = i % 2;
        ran(runs.back(), k);
    }
    value = 42;
    reset();
    auto later = draws(1);
    report(0x800, 50);
    reset();
    auto after = draws(1);
    report(0x810, 51);
    ran(after, {0});
    reads = resets = 0;
    occ_collect();
    CHECK(reads == 1 && resets == 0 && occ_pending.size() == 2 && occ_freeing.size() == 1);
    for (int i = 0; i < 40; i++) CHECK(guest[0x1000 + 16 * i] == (uint64_t)(i % 2));
    CHECK(guest[0x800] == 4096 && guest[0x810] == 4096);
    ran(later, {9});
    occ_collect();
    CHECK(guest[0x800] == 9 && guest[0x810] == 0 && occ_pending.empty() && occ_freeing.size() == 2 && resets == 0);
    done_value = 41;   // (the first batch's submission done, the second's not: one reset, in order)
    occ_collect();
    CHECK(resets == 1 && occ_freeing.size() == 1 && !gpu[runs[0][0]].avail && gpu[after[0]].avail);
    done_value = 42;
    occ_collect();
    CHECK(resets == 2 && occ_freeing.empty() && !gpu[after[0]].avail);
    // more draws than one report counts: it stays "visible", and its queries are still freed
    value = 43;
    reset();
    auto f = draws((int)kOccRun + 5);
    CHECK(f.size() == kOccRun && S.occ_overflow);
    report(0x700, 40);
    std::vector<uint64_t> zeros(f.size(), 0);
    ran(f, zeros);
    done_value = 43;
    occ_collect();
    CHECK(guest[0x700] == 4096 && occ_pending.empty() && !gpu[f[0]].avail);
    // a run with a draw that writes colour is not counted: it stays "visible", and its queries are still freed
    reset();
    auto box = draws(1);
    S.chmask.mask = 0xf;
    auto real = draws(1);
    S.chmask.mask = 0;
    CHECK(box.size() == 1 && real.empty() && S.occ_overflow);
    CHECK(draws(1).empty());   // (no more queries for a run already unknown)
    report(0x780, 41);
    ran(box, {0});
    occ_collect();
    CHECK(guest[0x780] == 4096 && occ_pending.empty() && !gpu[box[0]].avail);
    // a run with no report is freed at the next reset
    value = 44;
    reset();
    auto g = draws(2);
    reset();
    CHECK(occ_pending.size() == 1 && occ_pending.front().addr == 0);
    ran(g, {1, 1});
    done_value = 44;
    occ_collect();
    CHECK(occ_pending.empty() && !gpu[g[0]].avail);
    occ_end_buffer(S);

    // ---- several queues: one numbering ----
    // each of the game's queues has its own state (vk_replay.cpp's qstates); their command buffers take turns. Their
    // queries come from one counter: never the same query twice while it is in use
    {
        NvnState A = S, B = S;
        occ_state_copied(B);
        std::set<uint32_t> in_use;
        auto take = [&](NvnState& s, int n) {
            auto q = draws(n, false, s);
            for (uint32_t x : q) { CHECK(!in_use.count(x)); in_use.insert(x); }
            return q;
        };
        value = 50;
        begin(A, B);
        reset(A);
        auto qa = take(A, 3);
        report(0x2000, 60, A);
        occ_end_buffer(A);
        begin(B, A);
        reset(B);
        auto qb = take(B, 2);
        report(0x2010, 61, B);
        occ_end_buffer(B);
        CHECK(qb[0] == qa[2] + 1);   // (B goes on from A's numbers: each queue counting for itself handed out A's again)
        // A's run left open across B's command buffer: split. A's report stays visible; its queries and B's are freed
        begin(A, B);
        reset(A);
        auto qa2 = take(A, 2);
        occ_end_buffer(A);
        begin(B, A);
        CHECK(B.occ_limit && B.occ_limit - B.occ_next == kOccBuffer);
        reset(B);
        auto qb2 = take(B, 2);
        report(0x2020, 62, B);
        occ_end_buffer(B);
        begin(A, B);
        CHECK(A.occ_overflow && A.occ_first == A.occ_next);   // (split: its count would not be the game's)
        auto qa3 = take(A, 1);
        CHECK(qa3.empty());
        report(0x2030, 63, A);
        occ_end_buffer(A);
        // the GPU: every draw drew nothing; A's split run must not read 0
        ran(qa, {0, 0, 0}), ran(qb, {0, 0}), ran(qa2, {0, 0}), ran(qb2, {4, 0});
        done_value = 50;
        for (int i = 0; i < 4; i++) occ_collect();   // (out of order: a gap ends a batch)
        CHECK(occ_pending.empty());
        CHECK(guest[0x2000] == 0 && guest[0x2010] == 0 && guest[0x2020] == 4 && guest[0x2030] == 4096);
        for (uint32_t x : in_use) CHECK(!gpu[x].avail);   // (all freed)
        // a queue's state copied from another's mid-run: the run is not the new queue's
        begin(A, B);
        reset(A);
        take(A, 1);
        NvnState C = A;
        occ_state_copied(C);
        CHECK(!C.occ_on && C.occ_first == C.occ_next);
        // the ring's room counts a run another queue has open (its queries are not in occ_pending yet)
        occ_end_buffer(A);
        occ_pending.clear();
        occ_next_all = A.occ_first + kOccPool - kOccBuffer + 1;
        B.occ_on = false;
        begin(B, A);
        CHECK(B.occ_limit == 0);   // (A's open run is the ring's tail: no room)
        A.occ_on = false;
        begin(B, A);
        CHECK(B.occ_limit != 0);
        S = B;
        occ_end_buffer(S);
    }

    // ---- the draw side: a query only counts a draw that drew ----
    {
        occ_pending.clear();
        for (auto& q : gpu) q = {};
        S.occ_on = false, S.occ_overflow = false;
        begin(S, none);
        value = 60;
        auto one = [&](uint64_t addr, Path how, int groups = 0) {
            reset();
            DrawArgs da;
            da.groups[0] = groups;
            occ_take(S, da, false);
            CHECK(da.occ_pool);
            sent_in[da.occ_query] = value;
            path = how;
            draw(cmd, S, da);
            report(addr, 70);
        };
        samples_each = 0;   // (every draw hidden: an empty query would read 0, as a hidden box does)
        one(0x3000, kDraws);
        one(0x3010, kReturnsEarly);
        one(0x3020, kVertexMissing);
        prog.fallback_task = true, task_batch = 2;
        one(0x3030, kDraws, 5);   // (5 task groups in batches of 2: three draws of the batch path)
        prog.fallback_task = false;
        CHECK(!begun_twice && open_q.empty() && begins == ends && begins == 1);
        uint64_t nd[3];
        occ_draw_counts(nd);
        CHECK(nd[0] == 1 && nd[1] == 1 && nd[2] == 1);
        gpu_runs();
        done_value = 60;
        occ_collect();
        CHECK(occ_pending.empty());
        CHECK(guest[0x3000] == 0);      // (drew, hidden)
        CHECK(guest[0x3010] == 4096);   // (returned after the pass began: no query, not a 0)
        CHECK(guest[0x3020] == 4096);   // (zero_buffer for a stream: its count means nothing)
        CHECK(guest[0x3030] == 4096);   // (task batches: one query cannot be begun in each)
        samples_each = 3;
        one(0x3040, kDraws);
        gpu_runs();
        occ_collect();
        CHECK(guest[0x3040] == 3);
        occ_end_buffer(S);
    }

    // ---- the ring wraps: a batch across its end is read and reset in two calls ----
    occ_pending.clear();
    S.occ_on = false;
    occ_next_all = kOccPool - 2;
    S.occ_next = 0;
    begin(S, none);
    CHECK(S.occ_next == kOccPool - 2 && S.occ_limit);
    value = 61;
    reset();
    auto h = draws(4);
    CHECK(h.size() == 4 && h[0] == kOccPool - 2 && h[3] == 1);
    report(0x900, 60);
    ran(h, {0, 0, 0, 2});
    done_value = 61;
    reads = resets = 0;
    occ_collect();
    CHECK(guest[0x900] == 2 && reads == 2 && resets == 2 && !gpu[kOccPool - 1].avail && !gpu[1].avail);
    occ_end_buffer(S);
    begin(S, none);
    reset();
    // a dry pass numbers its draws as a worker does, but attaches no query
    const NvnState before = S;
    CHECK(draws(4, true).empty() && S.occ_next == before.occ_next + 4);
    S = before;
    auto w = draws(4);
    CHECK(w.size() == 4 && w[0] == before.occ_next % kOccPool);
    S = before;
    // a worker numbers from its copy and frees nothing at a reset
    {
        NvnState wk = before;
        draws(2, false, wk);
        const size_t n0 = occ_pending.size();
        occ_counter_reset(wk, true, value);
        CHECK(occ_pending.size() == n0 && wk.occ_first == before.occ_next + 2);
    }
    // no draws counted while off: discarded rasterization, a false condition, or no limit
    S.raster_discard = true;
    CHECK(draws(2).empty() && S.occ_next == before.occ_next);
    S.raster_discard = false;
    S.occ_limit = 0;
    reset();
    CHECK(!S.occ_on && draws(2).empty());
    // the ring: no room once the queries since the oldest unread one come within a command buffer's worth of its size
    occ_pending.clear();
    occ_pending.push_back({0, 0, 1000, 1, value, rec_part_next});
    occ_next_all = 1000 + kOccPool - kOccBuffer + 1;
    CHECK(!occ_room(UINT64_MAX));
    occ_next_all = 1000 + kOccPool - kOccBuffer;
    CHECK(occ_room(UINT64_MAX));
    occ_pending.clear();
    std::puts("PASS: occlusion counts: summed once their draws are done; visible when unknown, overflowed, with a colour draw, "
              "reported again (also within a tick), ahead of a stopped queue, split by another queue's queries, or from a draw "
              "that did not draw (an early return, vertex input not bound, task batches); one numbering for all queues; the "
              "report's own submission value; in order, read and reset in one batch (two across the ring's end)");
}
'''

# occ_queries_on() alone: what each environment gives
SWITCH = COMMON + r'''
struct { VkDevice device = nullptr; bool host_query_reset = true, occlusion_precise = true; } dev;
#define VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO 11
#define VK_QUERY_TYPE_OCCLUSION 0
struct VkQueryPoolCreateInfo { int sType; const void* pNext = nullptr; int flags = 0; int queryType = 0; uint32_t queryCount = 0; int stats = 0; };
VkResult vkCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo*, const void*, VkQueryPool* p) { *p = (VkQueryPool)1; return VK_SUCCESS; }
void vkResetQueryPool(VkDevice, VkQueryPool, uint32_t, uint32_t) {}
bool async_on() { return getenv("RX_VK_ASYNC") && atoi(getenv("RX_VK_ASYNC")) != 0; }
constexpr uint64_t kOccPool = 1 << 16;
VkQueryPool occ_pool = VK_NULL_HANDLE;
''' + "%SWITCH%" + r'''
int main() { std::printf("%d\n", occ_queries_on() ? 1 : 0); }
'''

SWITCH_CASES = [
    ({}, 1),
    ({"RX_VK_OCC_QUERIES": "0"}, 0),
    ({"RX_VK_OCC_QUERIES": "1"}, 1),
    ({"RX_VK_OCC_FIXED": "4096"}, 0),
    ({"RX_VK_OCC_QUERIES": "1", "RX_VK_OCC_FIXED": "4096"}, 0),   # (the fixed value wins, as documented)
    ({"RX_VK_OCC_QUERIES": "2", "RX_VK_OCC_FIXED": "-1"}, 0),
    ({"RX_VK_ASYNC": "1"}, 0),
]


def check_wiring(replay, draw):
    """Where the helpers are called, checked in the source"""
    # every draw takes its query before it is recorded (the dry pass too)
    for case in ("case NOP_DRAW: {", "case NOP_DRAW_IDX: {", "case NOP_DRAW_INDIRECT: {", "case NOP_DRAW_MESH: {"):
        body = block(replay, case)
        take = body.index("occ_take(s, da, dry);")
        assert take < body.index("if (s.render_enabled && !s.raster_discard && !dry) {"), case
        assert "DrawArgs da;" not in body[take:], "%s: a second DrawArgs after the query was taken" % case
    # a report closes its run in every mode, before the workers leave the case; the reset and report pass the submission
    # being recorded, not the last begin_recording's value
    report = block(replay, "case NOP_REPORT_COUNTER: {")
    assert report.index("occ_close(s, ") < report.index("if (worker) break;")
    assert "occ_reported(occ_run, c.addr, rec[1], flag_written, rec_cur_value);" in report
    reset = block(replay, "case NOP_RESET_COUNTER: {")
    assert "occ_counter_reset(s, worker, rec_cur_value);" in reset
    assert "occ_value" not in replay, "a global value every begin_recording overwrites"
    # one numbering: set before a parallel replay copies its start state, written back however the replay returns
    handle = block(replay, "const uint8_t* replay_handle(")
    assert handle.index("occ_begin_buffer(state, rec_cur_value, occ_open_oldest(&state));") < handle.index("const NvnState start = state;")
    assert handle.index("~OccEnd() { occ_end_buffer(s); }") < handle.index("replay(cmd, state, h);")
    assert "occ_begin_buffer(*cur_state" in block(replay, "void bench_replay(")
    assert "occ_state_copied(it->second);" in block(replay, "void use_queue_state(uint64_t queue) {")
    # work held back at an event wait: its reports visible until replayed
    run = block(replay, "bool run_submit(")
    assert run.index("if (value > q.ahead_upto) {") < run.index("occ_visible_ahead(handles, n, cb, stop);")
    job = block(replay, "void submit_job(")
    assert job.index("occ_visible_ahead(") < job.index("later.push_back(")
    # the harvest runs as each recording begins
    assert "occ_collect();" in block(replay, "Recording& begin_recording(uint64_t value)")
    # draw(): the guard made after StatsQuery (it ends first, inside the pass: StatsQuery's end copies outside it); every
    # draw command after it preceded by its query's issue, with no way out between; nothing else begins it
    fn = block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    guard = fn.index("OccGuard occ(cmd, d);")
    assert fn.index("StatsQuery stats_query(cmd, p, d);") < guard
    assert fn.index("begin_pass(cmd, at);") < guard
    assert "vkCmdBeginQuery" not in fn and "d.occ_pool" not in fn[:guard].replace("batch.occ_pool", "")
    body = fn[guard:]
    calls = [m.start() for m in re.finditer(r"\bvkCmdDraw\w*\(", body)]
    issues = [m.start() for m in re.finditer(r"occ\.issue\(\);", body)]
    assert len(calls) >= 12, "draw commands after the guard: %d" % len(calls)
    for at in calls:
        issue = body.rfind("occ.issue();", 0, at)
        assert issue >= 0, "a draw command with no occ.issue() before it"
        between = body[issue:at]
        assert "SKIP_HERE" not in between and "return" not in between, "a way out between occ.issue() and a draw command"
    for at in issues:   # (and each issue reaches a draw command with no way out first)
        nxt = min([c for c in calls if c > at] or [len(body)])
        between = body[at:nxt]
        assert nxt < len(body) and "SKIP_HERE" not in between and "return" not in between, \
            "occ.issue() with a way out before its draw command"
    assert "if (!vertex_complete) occ.drop(kOccVertexInput);" in fn
    batches = batch_code(draw)
    assert batches.index("batch.occ_pool = VK_NULL_HANDLE") < batches.index("draw(cmd, s, batch);")


def compile_and_run(cxx, tmp, name, code, env=None):
    cpp = Path(tmp) / (name + ".cpp")
    exe = Path(tmp) / (name + ".exe")
    cpp.write_text(code, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function", "-Werror",
                        str(cpp), "-o", str(exe)], capture_output=True, text=True)
    if r.returncode:
        sys.exit("the %s fixture did not compile:\n%s%s" % (name, r.stdout, r.stderr))
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    replay = REPLAY.read_text(encoding="utf-8")
    draw = DRAW.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="occ-queries-") as tmp:
        code = FIXTURE.replace("%RING%", ring_code(replay)).replace("%GUARD%", guard_code(draw))
        code = code.replace("%BATCH%", batch_code(draw))
        subprocess.run([str(compile_and_run(args.cxx, tmp, "ring", code))], check=True)
        exe = compile_and_run(args.cxx, tmp, "switch", SWITCH.replace("%SWITCH%", block(replay, "bool occ_queries_on()")))
        for extra, want in SWITCH_CASES:
            env = {k: v for k, v in os.environ.items() if not k.startswith(("RX_VK_OCC", "RX_VK_ASYNC"))}
            env.update(extra)
            got = int(subprocess.run([str(exe)], check=True, capture_output=True, text=True, env=env).stdout.strip())
            if got != want:
                sys.exit("occ_queries_on() with %s: %d, want %d" % (extra, got, want))
        print("PASS: occ_queries_on(): RX_VK_OCC_FIXED wins over RX_VK_OCC_QUERIES; off with RX_VK_ASYNC=1")
    check_wiring(replay, draw)
    print("PASS: the wiring in vk_replay.cpp and draw()")


if __name__ == "__main__":
    main()
