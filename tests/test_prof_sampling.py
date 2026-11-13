"""The per-draw phase timers (PROF_HOT) time one draw in eight and still add up.

The real scopes from gpu/vk/vk_internal.h run a synthetic replay on a fake clock: a phase nested in a draw never reads
more than the one around it, a slow path (PROF / PROF_EXACT) inside a draw not timed counts once as it is, the long-run
totals match the true ones, and RX_VK_PROF_ALL=1 (or prof_full, RX_VK_BENCH) times every scope exactly.
python -I -B tests/test_prof_sampling.py [--cxx clang++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
static int64_t now_ns, clock_reads;
namespace mkwvk {
enum ProfPhase { PROF_SUBMIT, PROF_PRESENT, PROF_DRAW, PROF_DESC, PROF_TEXGET, PROF_UPLOAD, PROF_PIPE, PROF_PROGRAM,
                 PROF_CMDS, PROF_DISPATCH, PROF_MEMSYNC, PROF_QSUBMIT, PROF_SLOTWAIT, PROF_COUNT };
int64_t prof_ns[PROF_COUNT];
inline int64_t prof_now() { return clock_reads++, now_ns; }
@SCOPES@
}
@MACROS@
using namespace mkwvk;

static int64_t truth[PROF_COUNT];   // (each phase's true time, from the clock itself)
struct Truth {
    int phase;
    int64_t t0 = now_ns;
    explicit Truth(int p) : phase(p) {}
    ~Truth() { truth[phase] += now_ns - t0; }
};
static std::mt19937_64 work(12345);
static int64_t cheap() { return 300 + (int64_t)(work() % 4000); }
static bool rare(int in) { return work() % in == 0; }

struct Draw { int64_t d[PROF_COUNT]; int k; bool slow; int64_t exact; };   // (exact: its slow paths' time)
/// One draw as vk_draw.cpp's draw() nests its phases: a program, a pipeline, descriptors with texture lookups, commands
static Draw draw() {
    int64_t before[PROF_COUNT];
    std::memcpy(before, prof_ns, sizeof before);
    Draw r{};
    {
        PROF_HOT(PROF_DRAW);
        Truth t(PROF_DRAW);
        now_ns += cheap();
        if (rare(200)) { PROF_EXACT(); now_ns += 5000000, r.exact += 5000000, r.slow = true; }   // (mesh_indirect_convert, a wait)
        {
            PROF_HOT(PROF_PROGRAM);
            Truth tp(PROF_PROGRAM);
            now_ns += cheap();
            if (rare(500)) { PROF_EXACT(); now_ns += 15000000, r.exact += 15000000, r.slow = true; }   // (program_pick: a translation)
        }
        {
            PROF_HOT(PROF_PIPE);
            Truth tp(PROF_PIPE);
            now_ns += cheap();
            if (rare(300)) { PROF_EXACT(); now_ns += 20000000, r.exact += 20000000, r.slow = true; }   // (graphics_pipeline: one made)
        }
        if (!rare(3)) {
            PROF_HOT(PROF_DESC);
            Truth td(PROF_DESC);
            now_ns += cheap();
            for (int i = 0, n = (int)(work() % 4); i < n; i++) {
                PROF_HOT(PROF_TEXGET);
                Truth tt(PROF_TEXGET);
                now_ns += cheap() / 4;
                if (rare(400)) {   // (create, then upload)
                    PROF_EXACT();
                    now_ns += 3000000;
                    PROF(PROF_UPLOAD);
                    Truth tu(PROF_UPLOAD);
                    now_ns += 7000000;
                    r.exact += 10000000, r.slow = true;
                }
            }
        }
        PROF_HOT(PROF_CMDS);
        Truth tc(PROF_CMDS);
        now_ns += cheap();
        r.k = prof_t.k;
    }
    CHECK(prof_t.open == 0);
    for (int i = 0; i < PROF_COUNT; i++) r.d[i] = prof_ns[i] - before[i];
    return r;
}

static void reset() { std::memset(prof_ns, 0, sizeof prof_ns), std::memset(truth, 0, sizeof truth), clock_reads = 0; }

int main(int argc, char** argv) {
    const bool all = argc > 1 && !std::strcmp(argv[1], "all");
    const int n = 400000;
    // (a, b) nesting, and the slow paths of a draw not timed counted once
    int64_t slow_unsampled = 0, sampled = 0;
    reset();
    for (int i = 0; i < n; i++) {
        int64_t t[PROF_COUNT];
        std::memcpy(t, truth, sizeof t);
        const Draw r = draw();
        CHECK(all ? r.k == 1 : (r.k == 0 || r.k == 8));
        sampled += r.k != 0;
        CHECK(r.d[PROF_PROGRAM] + r.d[PROF_PIPE] + r.d[PROF_DESC] + r.d[PROF_CMDS] <= r.d[PROF_DRAW]);
        CHECK(r.d[PROF_TEXGET] <= r.d[PROF_DESC] && r.d[PROF_UPLOAD] <= r.d[PROF_TEXGET]);
        CHECK(r.d[PROF_UPLOAD] == truth[PROF_UPLOAD] - t[PROF_UPLOAD]);   // (PROF: exact either way)
        const int64_t draw_ns = truth[PROF_DRAW] - t[PROF_DRAW];
        if (r.k == 0) {   // (only the slow paths, once and as they are: 5, 15, 20, 3 + 7 ms)
            slow_unsampled += r.slow;
            CHECK(r.d[PROF_DRAW] == r.exact);
            for (int p : {PROF_PROGRAM, PROF_PIPE, PROF_DESC, PROF_TEXGET, PROF_CMDS}) CHECK(r.d[p] % 1000000 == 0);
        }
        if (r.k == 8) CHECK(r.d[PROF_DRAW] == (draw_ns - r.exact) * 8 + r.exact);   // (the rest weighed, the slow part not)
        if (r.k == 1)
            for (int p = 0; p < PROF_COUNT; p++) CHECK(r.d[p] == truth[p] - t[p]);
    }
    if (all) {   // (d) every scope timed: exactly the true times
        CHECK(sampled == n);
        for (int p = 0; p < PROF_COUNT; p++) CHECK(prof_ns[p] == truth[p]);
        std::printf("PASS: RX_VK_PROF_ALL=1 times every scope exactly (%d draws)\n", n);
        return 0;
    }
    CHECK(slow_unsampled > 1000);
    CHECK(sampled > n / 8 * 0.95 && sampled < n / 8 * 1.05);
    // (c) unbiased: the long-run totals within 2% of the true ones
    for (int p : {PROF_DRAW, PROF_PROGRAM, PROF_PIPE, PROF_DESC, PROF_TEXGET, PROF_CMDS}) {
        const double e = (double)prof_ns[p] / (double)truth[p] - 1.0;
        std::printf("phase %d: %.3f%% off the true total\n", p, e * 100.0);
        CHECK(e > -0.02 && e < 0.02);
    }
    const int64_t reads = clock_reads;
    // (prof_full, as RX_VK_BENCH sets it while it replays: every scope exact, with about eight times the clock reads)
    prof_full++;
    reset();
    for (int i = 0; i < n; i++) CHECK(draw().k == 1);
    for (int p = 0; p < PROF_COUNT; p++) CHECK(prof_ns[p] == truth[p]);
    prof_full--;
    std::printf("clock reads: %lld sampled, %lld all\n", (long long)reads, (long long)clock_reads);
    CHECK(reads * 4 < clock_reads);
    std::printf("PASS: nested phases bounded, slow paths exact once, totals unbiased, prof_full exact (%d draws, %lld timed)\n",
                n, (long long)sampled);
}
'''


def scopes(source):
    """The sampled timers, from their comment to the namespace's end, and the PROF macros"""
    start = source.index("// (hookshot) The per-draw phases (PROF_HOT)")
    end = source.index("} // namespace mkwvk", start)
    macros = [line for line in source[end:].splitlines() if line.startswith("#define PROF")]
    if len(macros) != 3:
        raise SystemExit("PROF, PROF_HOT and PROF_EXACT are not all defined after the scopes")
    return source[start:end], "\n".join(macros)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    body, macros = scopes((ROOT / "gpu" / "vk" / "vk_internal.h").read_text(encoding="utf-8"))
    fixture = FIXTURE.replace("@SCOPES@", body).replace("@MACROS@", macros)
    with tempfile.TemporaryDirectory(prefix="prof-sampling-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", str(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h"), "-O1", "-Wall",
                        "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)   # (env_on: RX_VK_PROF_ALL)
        env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_PROF_ALL", "RX_VK_SLOW_LOG")}
        subprocess.run([str(exe)], check=True, env=env)
        subprocess.run([str(exe), "all"], check=True, env=dict(env, RX_VK_PROF_ALL="1"))


if __name__ == "__main__":
    main()
