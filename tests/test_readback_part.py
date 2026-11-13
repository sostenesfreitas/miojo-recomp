"""A CPU read of GPU results waits for the part of the submission that writes its pages back, and looks at the stopped
queues first only when the CPU has signalled an event since they were last looked at (gpu/vk/vk_replay.cpp's invalidate).

A submission is recorded in parts (maybe_split, ~120 draws each); each part's end copies back, from the mirrors, what the
GPU wrote in it (memory_end_submission). Since a submission's value maps to its last part's recording (issue 997,
tests/test_value_rec_last_part.py) a readback waited for the GPU's whole submission, whichever part held its pages, and
before that it took sync_order_mu whenever any queue was stopped, behind the presenting thread's replay and present. In
Hyrule Field one job thread a frame was blocked 5.5 ms that way (0.1 ms before), the game's other job threads polling
meanwhile. The statements are cut from the source and run against stand-ins for the timeline wait and the queues:

  - the page table keeps each page's part with its value: a mirror write-back's part, 0 for a page written in place;
  - submit_recording maps a part to its recording, on the recordings' timeline;
  - the readback waits for the newest part of its pages; for the submission's last part (as before) when a page was
    written in place and that submission is not done, or when the map has no such part; a part noted and not yet
    submitted (the replay's thread is between its write-back and its submit) is waited for, 250 ms at most;
  - a readback takes the look at the stopped queues after a CPU signal nobody has looked at since, also one made while a
    look was under way; every time while a queue is stopped at an event nobody has set (its value is guest memory's,
    where a store meets the wait with no signal); and not otherwise;
  - the part wait gives way to the last part when a part after the range's own reads guest memory in place (a pool
    with no mirror, a mirror's pages no capture holds): vk_memory.cpp notes each such read.

python -I -B tests/test_readback_part.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    """the text from marker to the brace that closes its first '{'"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def between(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start) + len(last)]


FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
constexpr uint64_t kPageShift = 12, kChunkPages = 16, kPages = 64;
static bool env_on(const char*) { return false; }   // (no switch set)
namespace {
/* PAGES */
static int failures;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// ---- the recordings' timeline, as submit_recording keeps it ----
struct Recording { uint64_t value = 0, part = 0; };
struct Dev { int device = 0; } dev;
using VkSemaphore = int;
VkSemaphore rec_timeline = 1;
std::mutex rec_map_mu;
std::map<uint64_t, uint64_t> value_rec, part_rec;
std::condition_variable rec_map_cv;
std::vector<uint64_t> rec_open_values;
static void note(Recording& r, uint64_t seq) {
    /* BOOKKEEPING */
}
static bool per_queue_fences_off() { return false; }
static uint64_t waited_seq;   // (the recording the last wait was for; 0: none)
static void wait_rec_seq(uint64_t seq, uint64_t, int* ok) { waited_seq = seq, *ok = 1; }
/* WAITS */

// ---- invalidate's choice of a wait ----
static uint64_t done_value;   // (the GPU's progress, as completed())
static uint64_t completed() { return done_value; }
static int waited_whole_value;   // (readback_wait_value: the main timeline, every queue's work)
static bool replay_behind() { return false; }
static void wait_queue_empty() {}
static bool readback_wait_value(uint64_t) { waited_whole_value++; return true; }
static const bool by_part = true, unforced = true;
static uint64_t in_place_part;   // (the newest part that reads guest memory in place, as vk_memory.cpp keeps it)
static uint64_t memory_in_place_part() { return in_place_part; }
static int held_back;
/// what a readback of [guest, guest + size) waits for: the recording (0: none)
static uint64_t readback(uint64_t guest, uint64_t size) {
    bool w = false;
    waited_seq = 0;
    uint64_t part = 0, whole = 0;
    const uint64_t v = memory_writeback_value(guest, size, nullptr, &part, &whole);
    /* CHOICE */
    (void)w;
    return waited_seq;
}

// ---- the look at the stopped queues ----
struct QueueRun { bool waiting = false; const uint8_t* p = nullptr; };
std::map<uint64_t, QueueRun> qruns;
std::atomic<int> blocked_n{0};
/* COUNTERS */
#include <unordered_map>
std::unordered_map<uint64_t, uint32_t> ev_values;   // (the events the stream or nvnEventSignal has set)
struct WaitEvent { uint64_t gpu; };
static WaitEvent wait_at(const uint8_t* p) { return {*reinterpret_cast<const uint64_t*>(p)}; }
/* MEMORY_EVENT */
// (a burst: one recording for a submission's piece and what it lets on, finished at the burst's end)
std::mutex replay_mu;
bool burst_open = false;
uint64_t burst_value = 0;
int* rec_cur = nullptr;
static int unsettled_while_finishing;
static void finish_recording(uint64_t) {   // (ends the recording: its pages are noted and it is submitted in here)
    rec_cur = nullptr;
    unsettled_while_finishing += ev_cpu_settled.load() != ev_cpu_signals.load();
}
static int looks, signal_during_look;
static void cpu_signal() { ev_cpu_signals.fetch_add(1, std::memory_order_release); }
static bool wait_met_at(const uint8_t*) {
    if (signal_during_look) signal_during_look = 0, cpu_signal();   // (the game's thread signals while the queues are looked at)
    return false;
}
static void resume_queue(uint64_t, bool) {}
/* SCHEDULE */
/* BURST_END */
static void sync_settle() { looks++; schedule(); }
/* SETTLE */

int main() {
    const uint64_t P = 4096;
    // submission 10 in three parts (parts 1-3, recordings 1-3): page 1 written back in part 1, page 2 in part 3
    Recording a{10, 1}, b{10, 2}, c{10, 3};
    wb_pages_note(1 * P, 2 * P, 10, 1);
    note(a, 1);
    note(b, 2);
    wb_pages_note(2 * P, 3 * P, 10, 3);
    rec_open_values.push_back(10);
    note(c, 3);
    check(value_rec[10] == 3 && part_rec[1] == 1 && part_rec[3] == 3, "a value maps to its last part, a part to its own recording");
    done_value = 9;
    check(readback(1 * P, 64) == 1, "a range written back in the first part waits for that part, not the last");
    check(readback(2 * P, 64) == 3, "a range written back in the last part waits for the last");
    check(readback(1 * P + 4000, 200) == 3, "a range over both pages waits for the newer part");
    check(waited_whole_value == 0, "none of them waits for every queue's work");
    done_value = 10;
    check(readback(1 * P, 64) == 0, "nothing is waited for once the submission is done");
    // submission 11: page 3 written in place (no mirror): the whole submission, its last part
    Recording d{11, 4}, e{11, 5};
    wb_pages_note(3 * P, 4 * P, 11, 0);
    wb_pages_note(4 * P, 5 * P, 11, 4);
    note(d, 4);
    rec_open_values.push_back(11);
    note(e, 5);
    check(readback(3 * P, 64) == 5, "a range written in place waits for its submission's last part");
    check(readback(3 * P, 2 * P) == 5, "and so does a range with a page written in place and one written back from a mirror");
    check(readback(4 * P, 64) == 4, "the mirror's page alone waits for its part");
    // a page written back from a mirror, then written in place by a later submission: the part is gone with the value
    wb_pages_note(1 * P, 2 * P, 12, 0);
    uint64_t part = 9, whole = 0;
    check(memory_writeback_value(1 * P, 64, nullptr, &part, &whole) == 12 && part == 0 && whole == 12,
          "a write in place replaces the page's part with its value");
    // a page written in place by a submission that is done, then written back from a mirror: the part
    done_value = 11;
    wb_pages_note(3 * P, 4 * P, 13, 6);
    Recording f{13, 6};
    rec_open_values.push_back(13);
    {   // (its pages are noted at its end, on the replay's thread, just before its submit: a read in between waits for that)
        std::thread replay([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            note(f, 6);
        });
        check(readback(3 * P, 64) == 6 && waited_whole_value == 0, "a part noted and not submitted yet: the read waits for its submit, then for it");
        replay.join();
    }
    check(readback(3 * P, 64) == 6, "and for its recording once it is submitted");
    wb_pages_note(5 * P, 6 * P, 13, 5);   // (part 5 is in the map; were it not, with part 6 in, it would never come)
    part_rec.erase(5);
    check(readback(5 * P, 64) == 6 && waited_whole_value == 0, "a part the map no longer has: its submission's last part, as before, at once");
    wb_pages_note(6 * P, 7 * P, 14, 7);   // (a part that is never submitted: given up after 250 ms)
    const auto t0 = std::chrono::steady_clock::now();
    check(readback(6 * P, 64) == 0 && waited_whole_value == 1, "a part that never comes: every queue's work, as before");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(ms >= 200 && ms < 2000, "after 250 ms");
    check(readback(3 * P, 2 * P) == 6, "an older in-place write that is done does not bring the whole wait back");
    check(!wait_recorded_part(0, 0, nullptr) && !wait_recorded_part(99, 0, nullptr), "no part, or one not known: no wait made");
    // a part after the range's own reads guest memory in place (a pool with no mirror, a mirror's pages from no capture):
    // the game, let go after the range's part, would write what that part still reads. The last part, as before
    done_value = 10;
    held_back = 0;
    in_place_part = 4;
    check(readback(4 * P, 64) == 4 && held_back == 0, "guest memory read in place in the range's own part: that part is enough");
    in_place_part = 5;
    check(readback(4 * P, 64) == 5 && held_back == 1, "guest memory read in place in a later part: the submission's last part");
    in_place_part = 0;

    // the look at the stopped queues: only after a CPU signal nobody has looked at
    const uint64_t event = 0x1000;
    ev_values[event] = 1;   // (an event the stream has set: its value is the replay's, not memory's)
    qruns[1].waiting = true;
    qruns[1].p = reinterpret_cast<const uint8_t*>(&event);
    check(!waits_on_memory_event(), "a queue stopped at an event the stream has set does not wait on guest memory");
    check(!sync_settle_met() && looks == 0, "nothing stopped: no look");
    blocked_n = 1;
    check(!sync_settle_met() && looks == 0, "a queue stopped, no CPU signal since the last look: no look (its wait is not met)");
    cpu_signal();
    check(sync_settle_met() && looks == 1, "a CPU signal since: the readback looks");
    check(!sync_settle_met() && looks == 1, "and not again");
    cpu_signal();
    signal_during_look = 1;
    check(sync_settle_met() && looks == 2, "a signal, and another made while the queues are looked at");
    check(sync_settle_met() && looks == 3, "the one made during the look is still to be looked at");
    check(!sync_settle_met() && looks == 3, "then none");
    // a look taken in a burst counts only once the burst's recording is finished: until then its write-backs are not in
    // the page table, and a readback that found the counts equal would read the table without them (mooki000's finding)
    {
        int recording = 0;
        cpu_signal();
        burst_open = true, rec_cur = &recording;
        schedule();
        check(ev_cpu_settled.load() != ev_cpu_signals.load(), "a look in a burst is not counted while the burst's recording is open");
        burst_end();
        check(unsettled_while_finishing == 1, "nor while that recording is being finished");
        check(ev_cpu_settled.load() == ev_cpu_signals.load() && !burst_open, "it is counted at the burst's end");
        check(!sync_settle_met() && looks == 3, "and the readback then takes no look");
    }
    // an event neither the stream nor nvnEventSignal ever set: its value is guest memory's, a store there meets the wait
    // with no signal counted. While a queue is stopped at one the readback looks every time, as before
    ev_values.clear();
    check(waits_on_memory_event(), "a queue stopped at an event nobody has set waits on guest memory");
    ev_wait_in_memory = waits_on_memory_event();   // (as update_blocked stores it)
    check(sync_settle_met() && sync_settle_met() && looks == 5, "then the readback looks every time, with no signal");
    qruns[1].waiting = false;
    ev_wait_in_memory = waits_on_memory_event();
    check(!waits_on_memory_event() && !sync_settle_met() && looks == 5, "and no longer once that queue has gone on");
    if (failures) return 1;
    std::printf("PASS: a readback waits for the part that writes its pages back, and looks at the stopped queues only after a CPU signal\n");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    memory = (a.source / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    replay = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    pages = memory[memory.index("std::mutex wb_pages_mu;"):memory.index("uint64_t memory_writeback_value(")]
    pages += block(memory, "uint64_t memory_writeback_value(")
    submit = replay[replay.index("void submit_recording("):]
    bookkeeping = between(submit, "std::lock_guard mk(rec_map_mu);", "while (part_rec.size() > 8192) part_rec.erase(part_rec.begin());")
    assert len(bookkeeping) < 3000, "the bookkeeping must be in submit_recording's rec_timeline block"
    # Match the definition, not the forward declaration used by vk_timeline_wait.
    waits = block(replay, "bool wait_recorded(uint64_t f, uint64_t timeout_ns, int* ok) {") + "\n" + block(replay, "bool wait_recorded_part(")
    inv = replay[replay.index("void invalidate(uint64_t gpu, uint64_t size) {"):]
    lookup = "const uint64_t v = memory_writeback_value(host->gpu_guest(gpu), size, nullptr, &part, &whole);"
    after = inv[inv.index(lookup):]
    choice = block(after, "if (v > completed()) {\n                int ok = 0;")
    assert "wait_recorded_part(part" in choice, "the readback no longer waits for its pages' part: update this test"
    # (the settle before the lookup goes through sync_settle_met with per-queue fences)
    before = inv[:inv.index(lookup)]
    assert "else if (sync_settle_met())" in before, "the readback's look at the stopped queues is no longer sync_settle_met's"
    counters = (between(replay, "std::atomic<uint64_t> ev_cpu_signals{0}", ";") + "\n" + between(replay, "std::atomic<bool> ev_wait_in_memory{", ";") +
                "\n" + between(replay, "uint64_t ev_looked = 0;", "bool ev_looked_any = false;"))
    blocked = block(replay, "void update_blocked() {")
    assert "ev_wait_in_memory.store(n && waits_on_memory_event()" in blocked, \
        "update_blocked no longer says whether a stopped queue waits on an event in guest memory"
    assert "const bool in_place_after = memory_in_place_part() > part;" in choice and "!in_place_after &&" in choice, \
        "the readback's part wait no longer gives way to guest memory read in place after its part"
    memory_src = memory
    # every place the guest pool's own buffer is handed to the GPU to read: bound, copied from, or given out whole
    # (gpu_buffer: a texture unswizzled on the GPU, a texel buffer's view). The write-back's own copies into it, the
    # shadow's fill and the readback pages' refresh are the ones left out (the last with its reason, at note_in_place_read)
    noted = 0
    for site in ("return remember({p->buffer, off, size, 0});", "vkCmdCopyBuffer(upload_command_buffer(), p.buffer, p.mirror,",
                 "vkCmdCopyBuffer(upload_command_buffer(), p->buffer, p->mirror, 1, &all);",
                 "return {p->buffer, guest - p->lo, size, p->address + (guest - p->lo)};"):
        at = 0
        while (at := memory_src.find(site, at)) >= 0:
            assert "note_in_place_read(" in memory_src[max(0, at - 200):at], \
                "a read of guest memory in place is not noted for the readback's part wait: " + site
            at += 1
            noted += 1
    assert noted == 6, "the reads of guest memory in place have changed (%d found): look at each again" % noted
    import re
    sources = re.findall(r"vkCmdCopyBuffer\([^,]+, (?:p|pp)(?:->|\.)buffer, [^,]+", memory_src)
    assert len(sources) == 4, "a new copy out of a guest pool (%d found): is it noted for the readback's part wait?" % len(sources)
    assert memory_src.count("note_in_place_read(!mirror_upload);") == 2, \
        "a binding outside a recording's uploads must count for the next part (no part begun yet is the one that reads it)"
    refresh = block(memory_src, "bool memory_rb_refresh(VkCommandBuffer up) {")
    assert "not noted for the readback's part wait: see note_in_place_read" in refresh, \
        "memory_rb_refresh's copies out of guest memory: noted now, or their reason gone?"
    signal = block(replay, "void event_signal_queued(")
    assert signal.index("ev_cpu_pending.emplace_back") < signal.index("ev_cpu_signals.fetch_add"), \
        "a CPU signal must be counted after it is in the pending list (schedule's count comes before its tests)"
    fixture = (FIXTURE.replace("/* PAGES */", pages).replace("/* BOOKKEEPING */", bookkeeping).replace("/* WAITS */", waits)
               .replace("/* CHOICE */", choice).replace("/* COUNTERS */", counters)
               .replace("/* SCHEDULE */", block(replay, "void schedule() {"))
               .replace("/* SETTLE */", block(replay, "bool sync_settle_met() {"))
               .replace("/* MEMORY_EVENT */", block(replay, "bool waits_on_memory_event() {"))
               .replace("/* BURST_END */", block(replay, "void burst_end() {")))
    with tempfile.TemporaryDirectory(prefix="readback-part-") as temp:
        src = Path(temp) / "readback_part.cpp"
        exe = Path(temp) / "readback_part.exe"   # (the name llvm-mingw writes on Windows; any name runs on Linux)
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], text=True, capture_output=True, timeout=20)
        print(r.stdout, end="")
        raise SystemExit(r.returncode)


if __name__ == "__main__":
    main()
