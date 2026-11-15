"""A throwing shader/pipeline job must publish failure and wake its waiter on every execution path."""
import argparse
from pathlib import Path
import runpy
import re
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def recoveries(path):
    text = (ROOT / path).read_text(encoding="utf-8")
    bodies = []
    for match in re.finditer(r"\[job\]\(\) noexcept \{", text):
        start = match.start()
        end = match.end()
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        bodies.append(text[start:end])
    return bodies


def resource_fixture():
    """Execute the production cleanup lambdas against small resource stand-ins."""
    shaders = recoveries("gpu/vk/vk_shader.cpp")
    draw = recoveries("gpu/vk/vk_draw.cpp")
    if len(shaders) != 2 or len(draw) != 3:
        raise SystemExit("a shader or pipeline resource recovery callback is missing")
    cases = []
    for callback in shaders:
        cases.append("{ Program p; auto job = std::make_shared<Job>(); job->p = &p; "
                     "auto recover = " + callback + "; recover(); "
                     "check(p.ready.load(std::memory_order_acquire) && p.failed); }")
    for pipeline in (0, 123):
        cases.append("{ PipeEntry e; e.pipeline = " + str(pipeline) + "; auto job = std::make_shared<Job>(); job->e = &e; "
                     "auto recover = " + draw[0] + "; recover(); "
                     "check(e.ready.load(std::memory_order_acquire) && e.pipeline == " + str(pipeline) + "); }")
    # These two callbacks capture Program* directly rather than the translation/graphics Job.
    cases.append("{ Program p; p.compute_state = 1; auto* job = &p; auto recover = " + draw[1] + "; recover(); "
                 "check(p.compute_state.load(std::memory_order_acquire) == 2 && p.fallback_compute.empty()); }")
    cases.append("{ Program p; p.compute_state = 1; p.compute_pipeline = 123; const auto* job = &p; "
                 "auto recover = " + draw[2] + "; recover(); "
                 "check(p.compute_state.load(std::memory_order_acquire) == 2 && p.failed && !p.compute_pipeline); }")
    return """#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>
constexpr uintptr_t VK_NULL_HANDLE = 0;
struct Program {
    bool failed = false;
    std::atomic<bool> ready{false};
    std::atomic<int> compute_state{0};
    uintptr_t compute_pipeline = 0;
    std::vector<uintptr_t> fallback_compute;
};
struct PipeEntry { uintptr_t pipeline = 0; std::atomic<bool> ready{false}; };
struct Job { Program* p = nullptr; PipeEntry* e = nullptr; };
void check(bool ok) { if (!ok) std::abort(); }
int main() {
""" + "\n".join(cases) + "\n}\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    stubs = runpy.run_path(str(ROOT / "tests/test_jobs_wait_runs_queued.py"))
    with tempfile.TemporaryDirectory(prefix="jobs-failure-") as folder:
        folder = Path(folder)
        for name in ("vk_jobs.cpp", "vk_jobs.h"):
            shutil.copy(ROOT / "gpu/vk" / name, folder / name)
        (folder / "vk_internal.h").write_text(stubs["STUB_INTERNAL"], encoding="utf-8")
        (folder / "vk_draw.h").write_text(stubs["STUB_DRAW"], encoding="utf-8")
        exe = folder / "jobs-failure.exe"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-pthread", "-I", str(folder), "-I", str(ROOT / "gpu/vk/compat"),
                        str(folder / "vk_jobs.cpp"), str(ROOT / "tests/test_jobs_failure.cpp"), "-o", str(exe)], check=True)
        for mode in ("worker", "serial", "steal"):
            for kind in ([], ["nonstd"]):
                subprocess.run([str(exe), mode, *kind], check=True, timeout=10)
        fixture = folder / "resource-recovery.cpp"
        fixture.write_text(resource_fixture(), encoding="utf-8")
        resource_exe = folder / "resource-recovery.exe"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", str(fixture), "-o", str(resource_exe)], check=True)
        subprocess.run([str(resource_exe)], check=True, timeout=10)
        print("PASS: production shader, graphics (null/linked), fallback and compute recovery states")


if __name__ == "__main__":
    main()
