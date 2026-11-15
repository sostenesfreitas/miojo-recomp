"""CPU-only draw readiness decision check: inline fake worker completion, no Vulkan/shader execution.

With RX_VK_PIPE_SKIP and RX_VK_TRANSLATE_WAIT_MS unset it also checks the pipeline's quick path (quick_pipeline,
RX_VK_PIPE_WAIT_MS's brief wait): a quick pipeline is drawn with, a skippable draw is left out only after the brief wait, a
draw that must wait asks for one with must set, and a shader still translating is waited for as before. With
RX_VK_PIPE_SKIP=0, a quick pipeline is on offer and never asked for: every pipeline is waited for.

The real draw decision block is extracted intact. Publication and final resource writes are CPU witnesses,
not a claim about GPU output, timing, or a reproduced game failure.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
@TRANSLATE_WAIT_H@
@QUICK_PIPE_H@
#define PROF_EXACT() ((void)0)
#define PROF_HOT(x) ((void)0)
#define SKIP_HERE() return
using VkPipeline = int;
struct Desc { bool is_written = false; };
struct Info {
    bool stores_global_memory = false;
    std::vector<Desc> storage_buffers_descriptors, image_buffer_descriptors, image_descriptors;
};
struct Stage { Info info; };
int scans, early_reads, translation_waits, bounded_waits, pipeline_waits, draws, output, st_waits, st_pending_skips;
bool fast_translation;   // (whether the bounded wait sees the translation finish)
double st_wait_ms;
void logf(const char*, ...) {}
// Instrument only fixture iteration: reject inspecting ordinary stages before acquire-ready publication.
struct Stages {
    std::atomic<bool>* ready;
    std::vector<Stage> values;
    auto begin() const {
        if (!ready->load(std::memory_order_acquire)) ++early_reads;
        ++scans;
        return values.begin();
    }
    auto end() const { return values.end(); }
};
struct Program {
    std::atomic<bool> ready{false};
    bool failed = false, fallback = false, fallback_task = false;
    int layout = 1, fallback_topology = 0;
    Stages stages{&ready, {}};
};
struct PipeEntry {
    std::atomic<bool> ready{false};
    std::atomic<VkPipeline> pipeline{0};
    std::atomic<unsigned char> how{0}, quick{0};
};
// (the census's first-use report, gpu/vk/vk_programs.h: off here)
struct { bool on = false; void skipped() {} void pipeline(bool, bool) {} void pipeline_made(unsigned char) {} } use;
struct { long long pipes_new = 0; } use_counters;
struct Targets { bool persistent = true; };
struct NvnState {};
struct ParLock {};
struct DrawArgs { bool indirect = true; int groups[3]{1, 1, 1}; int first_task = 0; void* occ_pool = nullptr; };
#define VK_NULL_HANDLE nullptr
enum OccNotBegun { kOccTaskBatches };   // (vk_draw.cpp's occlusion query guard: the task batches leave it unbegun)
struct OccGuard { OccGuard(int, const DrawArgs&) {} void drop(OccNotBegun) {} };
int fallback_task_groups(const Program&) { std::abort(); }
void draw(int, NvnState&, const DrawArgs&) { std::abort(); }
Program* current;
PipeEntry pipe_entry;
int effect;
Stage shader_stage(int kind) {
    Stage s;
    if (kind == 1) s.info.stores_global_memory = true;
    if (kind == 2) s.info.storage_buffers_descriptors.push_back({true});
    if (kind == 3) s.info.image_buffer_descriptors.push_back({true});
    if (kind == 4) s.info.image_descriptors.push_back({true});
    // Nonempty read-only descriptors must still qualify for the color-only control.
    if (kind == 0) s.info.storage_buffers_descriptors.push_back({false});
    return s;
}
void publish() {
    // Put the write in a later stage, not stages[0].
    current->stages.values = {Stage{}, shader_stage(effect)};
    current->ready.store(true, std::memory_order_release);
}
bool jobs_wait(const std::atomic<bool>& ready) {
    if (&ready == &current->ready) { ++translation_waits; publish(); }
    else { ++pipeline_waits; pipe_entry.pipeline.store(1); pipe_entry.ready.store(true, std::memory_order_release); }
    return true;
}
int jobs_pending() { return 0; }
// (the quick path: what the fake quick_pipeline gives, the brief wait's budget, whether the build finishes within it)
bool quick_gives, fast_pipeline, last_must;
double brief_ms;
int quick_calls, hurries, brief_waits;
bool quick_pipeline(PipeEntry& e, const Program&, const NvnState&, const Targets&, int, bool must) {
    ++quick_calls, last_must = must;
    if (quick_gives) e.pipeline.store(7);
    return quick_gives;
}
double pipe_wait_budget_ms() { return brief_ms; }
bool jobs_hurry(const std::atomic<bool>&) { return ++hurries, true; }
bool jobs_wait_for(const std::atomic<bool>& ready, double) {
    if (&ready == &pipe_entry.ready) {
        ++brief_waits;
        if (fast_pipeline) pipe_entry.pipeline.store(1), pipe_entry.ready.store(true, std::memory_order_release);
        return ready.load(std::memory_order_acquire);
    }
    ++bounded_waits;
    if (&ready == &current->ready && fast_translation) publish();
    return ready.load(std::memory_order_acquire);
}
uint64_t texture_frame_no() { return 1; }
bool targets_persistent(const Targets& at) { return at.persistent; }
bool exposure_log_on() { return false; }
void exposure_note(NvnState&, const Program&) {}
void ensure_layout(Program&) {}
// These cases use non-fallback programs, whose topology stays unchanged.
int pipeline_topology(const Program&, int drawn) { return drawn; }
PipeEntry* graphics_pipeline(const Program&, NvnState&, const Targets&, int, const unsigned char*) { return &pipe_entry; }
@WRITES_MEMORY@
void decision(const Program* cp, const Targets& at) {
    NvnState s; DrawArgs d; int cmd = 0, topo = 0; unsigned char types[8]{};
@BLOCK@
    // Fake draw/consumer witness only: not actual GPU shader execution or resource binding.
    ++draws;
    if (effect) output = 42;
}
int failures;
void check(bool ok, const char* label) { if (!ok) { ++failures; std::printf("FAIL %s\n", label); } }
void scenario(int kind, bool translated, bool pipeline_hot, bool persistent, bool fast = true, bool checked = true) {
    scans = early_reads = translation_waits = bounded_waits = pipeline_waits = draws = output = st_pending_skips = st_waits = 0;
    Program p; current = &p; effect = kind; fast_translation = fast;
    if (translated) publish();
    pipe_entry.ready.store(pipeline_hot, std::memory_order_release);
    pipe_entry.pipeline.store(pipeline_hot ? 1 : 0), pipe_entry.quick.store(0);
    quick_calls = hurries = brief_waits = 0;
    const bool force = std::getenv("RX_VK_PIPE_SKIP") && std::atoi(std::getenv("RX_VK_PIPE_SKIP")) == 0;
    const bool legacy = std::getenv("RX_VK_PIPE_SKIP") && std::atoi(std::getenv("RX_VK_PIPE_SKIP")) == 2;
    // (RX_VK_TRANSLATE_WAIT_MS set to a number of 0 or more, default and mode 1: an unknown translation on persistent targets
    // is waited for within a frame's bound, then left out; unset, negative or not a number: waited out, as #1573)
    double wait_ms = -1;
    const char* w = std::getenv("RX_VK_TRANSLATE_WAIT_MS");
    if (!w || !translate_wait::parse(w, wait_ms)) wait_ms = -1;
    const bool bounded = wait_ms >= 0 && !force && !legacy && !translated && persistent;
    const bool spent = wait_ms <= 0;   // (a bound of 0: no wait at all, and none counted)
    const bool skip_unknown = (legacy && !translated && persistent) || (bounded && (!fast || spent));
    const bool should_draw = !skip_unknown && (pipeline_hot || force || !persistent || kind != 0);
    // (RX_VK_PIPE_SKIP=0 waits for every pipeline: a quick one on offer is not asked for, nor drawn with)
    const bool offered = quick_gives;
    if (force) quick_gives = true;
    decision(&p, {persistent});
    quick_gives = offered;
    if (force) check(quick_calls == 0, "RX_VK_PIPE_SKIP=0 asks for no quick pipeline");
    if (!checked) return;
    char label[120];
    std::snprintf(label, sizeof label, "kind%d translated%d hot%d persistent%d fast%d force%d", kind, translated, pipeline_hot,
                  persistent, fast, force);
    const int before = failures;
    check(draws == int(should_draw), label);
    if (kind && should_draw) check(output == 42, "producer write absent at fake consumer");
    check(!early_reads, "metadata inspected before publication");
    check(translation_waits == int(!translated && !skip_unknown && !bounded), "translation wait mode");
    check(bounded_waits == int(bounded && !spent), "bounded translation wait mode");
    check(st_waits == int(bounded && !spent) + (translation_waits + pipeline_waits), "a wait counted only when it could wait");
    if (pipeline_hot || force || !persistent || skip_unknown) check(scans == 0, "unnecessary classification");
    check(pipeline_waits == int(!pipeline_hot && should_draw), "pending pipeline must publish before draw");
    std::printf("%s %s draws%d skips%d scans%d twait%d pwait%d\n", failures == before ? "PASS" : "RED", label,
                draws, st_pending_skips, scans, translation_waits, pipeline_waits);
}
int main() {
    for (int kind = 0; kind <= 4; ++kind)
        for (bool translated : {false, true})
            for (bool hot : {false, true}) scenario(kind, translated, hot, true);
    scenario(1, true, false, false);
    scenario(1, false, false, false);
    scenario(0, false, false, false);
    // (a translation that does not finish within the frame's bound: left out on persistent targets by default)
    for (int kind = 0; kind <= 4; ++kind)
        for (bool hot : {false, true}) scenario(kind, false, hot, true, false);
    if (!std::getenv("RX_VK_PIPE_SKIP") && !std::getenv("RX_VK_TRANSLATE_WAIT_MS")) {
        const auto quick_case = [](const char* label, int kind, bool translated, bool gives, double brief, bool fast, int want_draws,
                                   int want_quick, bool want_must, int want_hurries, int want_pwaits, int want_twaits) {
            quick_gives = gives, brief_ms = brief, fast_pipeline = fast;
            scenario(kind, translated, false, true, true, false);
            const int before = failures;
            check(draws == want_draws, label);
            check(quick_calls == want_quick && (!want_quick || last_must == want_must), "quick_pipeline asked as expected");
            check(hurries == want_hurries && brief_waits == want_hurries, "the brief wait hurried and waited as expected");
            check(pipeline_waits == want_pwaits, "the full build waited for as expected");
            check(translation_waits == want_twaits, "a translation still waited out as before");
            check(st_waits == translation_waits + pipeline_waits + brief_waits, "every wait counted");
            check(pipe_entry.pipeline.load() != 0 || !want_draws, "drawn with a pipeline");
            std::printf("%s quick %s\n", failures == before ? "PASS" : "RED", label);
            quick_gives = false, brief_ms = 0, fast_pipeline = false;
        };
        // label, kind, translated, quick gives, brief ms, build done in the brief wait, draws, quick asks, must, hurries, waits
        quick_case("skippable draw drawn with a quick pipeline", 0, true, true, 0, false, 1, 1, false, 0, 0, 0);
        quick_case("memory writer gets one too, as must, and does not wait", 1, true, true, 0, false, 1, 1, true, 0, 0, 0);
        quick_case("skippable, none quick, no brief wait: left out", 0, true, false, 0, false, 0, 1, false, 0, 0, 0);
        quick_case("skippable, none quick, built within the brief wait: drawn", 0, true, false, 100, true, 1, 1, false, 1, 0, 0);
        quick_case("skippable, none quick, not built within it: left out", 0, true, false, 100, false, 0, 1, false, 1, 0, 0);
        quick_case("memory writer, none quick: waits for the full build", 1, true, false, 100, false, 1, 1, true, 0, 1, 0);
        quick_case("untranslated: the translation waited out, then quick", 0, false, true, 100, false, 1, 1, false, 0, 0, 1);
    }
    // (the switch's value: a typo is not read as 0)
    double v = 0;
    check(translate_wait::parse("8", v) && v == 8, "parse 8");
    check(translate_wait::parse(" 2.5 ", v) && v == 2.5, "parse 2.5 with spaces");
    check(translate_wait::parse("-1", v) && v == -1, "parse -1");
    for (const char* bad : {"", "8ms", "off", "nan", "inf", " "}) check(!translate_wait::parse(bad, v), "parse rejects a non-number");
    std::printf("%d failures; fake CPU decision check only\n", failures);
    return failures ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    src = (args.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    try:
        helper_start = src.index("static bool writes_memory(const Program& p)")
        helper_end = src.index("enum class FallbackRole", helper_start)
        start = src.index("    static const bool always_wait =", src.index("void draw(VkCommandBuffer"))
        end = src.index("    cur_program = &p;", start)
    except ValueError:
        raise SystemExit("draw readiness block changed; update this test's extraction anchors") from None
    header = (args.source / "gpu/vk/translate_wait.h").read_text(encoding="utf-8").replace("#pragma once", "")
    quick = ((args.source / "gpu/vk/quick_pipe.h").read_text(encoding="utf-8").replace("#pragma once", "")
             .replace('#include "translate_wait.h"', ""))
    fixture = (FIXTURE.replace("@WRITES_MEMORY@", src[helper_start:helper_end]).replace("@BLOCK@", src[start:end])
               .replace("@TRANSLATE_WAIT_H@", header).replace("@QUICK_PIPE_H@", quick))
    with tempfile.TemporaryDirectory(prefix="draw-side-effects-") as tmp:
        cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-variable",
                        str(cpp), "-o", str(exe)], check=True, timeout=60)
        failed = False
        for mode, wait in [(m, w) for m in (None, "0", "1", "2") for w in (None, "8", "0", "8ms")]:
            env = os.environ.copy()
            env.pop("RX_VK_PIPE_SKIP", None)
            env.pop("RX_VK_TRANSLATE_WAIT_MS", None)
            if mode is not None:
                env["RX_VK_PIPE_SKIP"] = mode
            if wait is not None:
                env["RX_VK_TRANSLATE_WAIT_MS"] = wait
            result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=10)
            lines = result.stdout.splitlines()
            complete = "0 failures; fake CPU decision check only" in lines and sum(l.startswith("PASS kind") for l in lines) == 33
            if mode is None and wait is None:   # (the quick path's cases run here)
                complete = complete and sum(l.startswith("PASS quick") for l in lines) == 7
            ok = result.returncode == 0 and complete
            print(f"RX_VK_PIPE_SKIP={mode if mode is not None else 'unset'} RX_VK_TRANSLATE_WAIT_MS={wait if wait is not None else 'unset'}: "
                  f"{'PASS' if ok else 'FAIL'}")
            if not ok:
                failed = True
                print("fixture failed or did not finish all 33 cases\n" + result.stdout + result.stderr)
        if failed:
            raise SystemExit(1)
    print("PASS: unknown translations are waited out by default; with RX_VK_TRANSLATE_WAIT_MS they wait within a frame's bound, "
          "then are left out on persistent targets (a bound of 0 counts no wait; a non-number is ignored); mode 0 waits them out; "
          "mode 2 restores their old skip; translated writers wait; hot draws do not scan stages")


if __name__ == "__main__":
    main()
