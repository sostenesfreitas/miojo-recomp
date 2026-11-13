"""A stage library the driver has not compiled is not compiled in the draw, and is built once at most (gpu/vk/vk_draw.cpp
gpl_part, gpl_build_cold; no game/GPU).

The two functions are compiled as they are in vk_draw.cpp, beside a stand-in driver that has some libraries compiled, has
to compile others and cannot make one: the draw's thread must never compile, a cold library must be handed to a worker
once, and one that failed must not be asked for again. What the workers then build of the cold ones is counted, with its
time.
python -I -B tests/test_gpl_cold_library.py
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <unordered_map>
using VkPipeline = uint64_t;
using VkPrimitiveTopology = int;
#define VK_NULL_HANDLE 0
struct Program { uint64_t key; };
struct NvnState {};
struct Targets {};
struct { int device; struct { bool pipelineCreationCacheControl; } vk13; } dev;
std::atomic<int64_t> st_lib_hits, st_lib_cold, st_lib_built, st_lib_build_us;
inline uint64_t mix(uint64_t a, uint64_t b) { return (a * 1000003 + b + 1) * 0x9E3779B97F4A7C15ull; }
inline void logf(const char*, ...) {}
int destroyed, compiles_in_draw, compiles;
bool in_draw, driver_keeps;   // (driver_keeps: what it compiles it has from then on)
std::set<uint64_t> driver_has, driver_cannot;   // (by program key: compiled already; never made)
inline void vkDestroyPipeline(int, VkPipeline, void*) { destroyed++; }
VkPipeline build_graphics(const Program& p, const NvnState&, const Targets&, VkPrimitiveTopology, int part, bool* cold = nullptr) {
    if (driver_cannot.count(p.key)) return VK_NULL_HANDLE;
    if (part == 0 || part == 3 || driver_has.count(p.key)) return 1000 + p.key * 4 + part;
    if (cold) return *cold = true, VK_NULL_HANDLE;
    compiles++, compiles_in_draw += in_draw;
    if (driver_keeps) driver_has.insert(p.key);
    return 1000 + p.key * 4 + part;
}
@CODE@
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int run(bool feature) {
    dev.vk13.pipelineCreationCacheControl = feature;
    NvnState s;
    Targets at;
    const Program warm{1}, cold{2}, broken{3};
    driver_has = {1}, driver_cannot = {3};
    in_draw = true;
    uint64_t k = 0;
    // (the driver has it: made in the draw, and the same one after)
    const VkPipeline w = gpl_part(warm, s, at, 0, 1, 11, &k);
    CHECK(w && !k && gpl_part(warm, s, at, 0, 1, 11, &k) == w && !k);
    // (the parts without shaders are made in the draw whatever the driver has)
    CHECK(gpl_part(cold, s, at, 0, 0, 20, nullptr) && gpl_part(cold, s, at, 0, 3, 23, nullptr));
    if (!feature) {   // (no pipelineCreationCacheControl: compiled in the draw, as before)
        CHECK(gpl_part(cold, s, at, 0, 1, 21, &k) && !k && compiles_in_draw == 1);
        CHECK(!gpl_part(broken, s, at, 0, 2, 32, &k) && !k);
        return 0;
    }
    // (cold: nothing in the draw, its key given once for a worker, and not asked of the driver again while it is not there)
    CHECK(!gpl_part(cold, s, at, 0, 1, 21, &k) && k && st_lib_cold == 1 && st_lib_hits == 1);
    const uint64_t queued = k;
    k = 0;
    CHECK(!gpl_part(cold, s, at, 0, 1, 21, &k) && !k && st_lib_cold == 1);
    in_draw = false;
    gpl_build_cold(cold, s, at, 0, 1, queued);
    in_draw = true;
    CHECK(gpl_part(cold, s, at, 0, 1, 21, &k) && !k && compiles == 1 && compiles_in_draw == 0 && st_lib_built == 1);
    // (one the driver cannot make: not cold, not queued, and not tried again)
    CHECK(!gpl_part(broken, s, at, 0, 2, 32, &k) && !k);
    driver_cannot.clear();
    CHECK(!gpl_part(broken, s, at, 0, 2, 32, &k) && !k && st_lib_cold == 1);
    // (a cold one the worker cannot make: stays missing, and is not given to a worker a second time)
    const Program late{4};
    CHECK(!gpl_part(late, s, at, 0, 2, 42, &k) && k);
    const uint64_t queued2 = k;
    k = 0;
    driver_cannot = {4};
    in_draw = false;
    gpl_build_cold(late, s, at, 0, 2, queued2);
    in_draw = true;
    driver_cannot.clear();
    CHECK(!gpl_part(late, s, at, 0, 2, 42, &k) && !k && st_lib_cold == 2 && compiles_in_draw == 0 && !destroyed);
    CHECK(st_lib_built == 1);   // (the one a worker made, not the one it could not)
    // (cold, and no one to tell: nothing in the draw, and no crash)
    const Program quiet{8};
    CHECK(!gpl_part(quiet, s, at, 0, 1, 81, nullptr) && st_lib_cold == 3 && compiles_in_draw == 0);
    return 0;
}
int main(int argc, char**) {
    return run(argc == 1);
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source(draw):
    """The flag is only asked of a device that has the feature, and the cold library's job is one a draw does not wait for"""
    if "dev.vk13.pipelineCreationCacheControl" not in between(draw, "bool gpl_ask_only() {", "\n}\n"):
        raise SystemExit("the compile-required flag is used without pipelineCreationCacheControl")
    build = draw[draw.index("VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, "
                            "VkPrimitiveTopology topo, int part, bool* cold) {"):]
    if "if (cold) ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;" not in build or \
            "if (cold && r == VK_PIPELINE_COMPILE_REQUIRED) return *cold = true, VK_NULL_HANDLE;" not in build:
        raise SystemExit("build_graphics does not tell a library the driver has to compile from one it cannot make")
    if "jobs_push([job, i] { gpl_build_cold(*job->p, job->s, job->at, job->topo, i + 1, job->cold[i]); });" not in draw:
        raise SystemExit("a cold library is not handed to a worker, or its job carries a flag a waiting draw would run")
    # (after the pipeline is ready, not before: the draw that waits for it runs this job itself, jobs_wait, and would be held
    # for as long as anything put before that takes)
    job = between(draw, "    jobs_push([job] {\n        // (over a linked one", "}, false, &e->ready,")
    if job.index("job->e->ready.store(true") > job.index("gpl_build_cold("):
        raise SystemExit("the cold libraries are handed to the workers before the pipeline the draw waits for is ready")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "if (draw_stats4(&links, &link_ms, &lib_hits, &lib_cold, &lib_built, &lib_build_ms))" not in replay:
        raise SystemExit("the links, the libraries had and cold and the workers' builds of them are not in the frame statistics, "
                         "or are there without stage libraries too")
    cold = between(draw, "void gpl_build_cold(", "\n}\n")
    if "st_lib_built++" not in cold or "st_lib_build_us +=" not in cold or cold.index("st_lib_build_us +=") > cold.index("if (!made)"):
        raise SystemExit("what the workers spend on cold libraries is not counted (a library that fails took its time too)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    check_source(draw)
    code = between(draw, "bool gpl_ask_only() {", "/// cold: the keys of the pre-rasterization")
    with tempfile.TemporaryDirectory(prefix="gpl-cold-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE.replace("@CODE@", code), encoding="utf-8")
        exe = tmp / "gpl-cold"
        subprocess.run([args.cxx, "-std=c++20", "-include", ENV_ON_H, "-Wall", "-Wextra", str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        for extra, what in (([], "with the feature"), (["off"], "without the feature")):
            r = subprocess.run([str(exe)] + extra, capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(f"{what}: {r.stdout.strip()}")
    print("PASS: a cold stage library is left to a worker, and one that fails is built once at most")


if __name__ == "__main__":
    main()
