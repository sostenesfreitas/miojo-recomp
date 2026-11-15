"""Quick pipelines for draws whose full build is not done (gpu/vk/quick_pipe.h, vk_draw.cpp quick_pipeline; no game/GPU).

Ported from Mako, by sharkiix (8292d363). Checks, game-free:
- the budget a frame on each recording thread, the caches asked first, an entry given one once, the full build replacing the
  quick one and a quick one that lost to it destroyed, a draw that must wait not held to the budget (quick_pipe.h, with fake
  handles), and the brief wait's budget;
- a driver-fault gate that was only busy (AMD: the program's own full build in the driver) leaves the entry to be tried again,
  in quick_pipe.h and through vk_draw.cpp's quick_pipeline; a frame's budget is not overrun by more than one build;
- vk_draw.cpp's own quick_pipeline, cut out and built against stand-ins: the caches asked without compiling before a quick
  build, the quick build under quick_build, and RX_VK_QUICK_MS off by default on every driver, strictly read;
- build_graphics: a quick build goes into no pipeline cache, does not count towards a save nor in st_pipelines, is not made
  where the full build is made without the optimizer already, and does not wait at the driver-fault gate, which says when it
  was busy; RX_VK_TEST_SLOW_MS holds the full build inside the gate; the worker's full build replaces the quick one, and one
  that failed leaves it; draw(): only pipelines take the quick path, a shader still translating is waited for as before, and
  RX_VK_PIPE_SKIP=0 waits for every full pipeline.
python -I -B tests/test_quick_pipelines.py
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end, at=0):
    a = text.index(start, at)
    return text[a:text.index(end, a)]


def check_source(draw):
    build = between(draw, "VkPrimitiveTopology topo, int part, bool* cold) {", "\n}\n")
    quick = between(build, "    if (quick_build) {", "\n    }\n")
    if "if (part >= 0 || (ci.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT)) return VK_NULL_HANDLE;" not in quick or \
            "ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;" not in quick:
        raise SystemExit("a quick build is not made without the optimizer, or is made where the full one is unoptimized already")
    at = build.index("    if (quick_build) {")
    for rule in ('env_on("RX_VK_NOOPT")', "if (amd && !mesh_opt)", "VK_DRIVER_ID_AMD_PROPRIETARY"):
        if build.index(rule) > at:
            raise SystemExit(f"the quick build is decided before {rule}: it would not see that the full build is unoptimized")
    if at > build.index("fault_quarantine().begin(") or at > build.index("graphics_with("):
        raise SystemExit("the quick build is decided after the driver is asked")
    if "fault_quarantine().begin(p.key, !cold && !quick_build)" not in build:
        raise SystemExit("a quick build waits at the driver-fault gate behind the program's full build")
    if "if (gate == driver_faults::Quarantine::kBusy) return gate_busy = true," not in build:
        raise SystemExit("a busy driver-fault gate is not told apart: the entry would never be tried again")
    slow = build.index("if (slow_full_build && test_slow_ms() > 0) std::this_thread::sleep_for(")
    if not build.index("fault_quarantine().begin(") < slow < build.index("graphics_with("):
        raise SystemExit("RX_VK_TEST_SLOW_MS does not hold the full build inside the driver-fault gate")
    if "if (!quick_build) {   // (hookshot: quick builds are counted apart" not in build or \
            build.index("st_pipelines++;") < build.index("if (!quick_build) {   // (hookshot: quick builds are counted apart"):
        raise SystemExit("a quick build is counted in st_pipelines")
    if "const VkPipelineCache cache = quick_build ? VK_NULL_HANDLE" not in build:
        raise SystemExit("a quick build goes into a pipeline cache")
    if "pcache_built_since_save += !driver_cache_only && !quick_build;" not in build:
        raise SystemExit("a quick build counts towards saving the pipeline cache")
    job = between(draw, "    jobs_push([job] {\n        // (over a linked one", "}, false, &e->ready,")
    if "if (full) {\n            job->e->pipeline = full;" not in job:
        raise SystemExit("the worker's full build no longer replaces the quick one")
    raised = "const quick_pipe::Raise slow(slow_full_build);"
    if raised not in job or job.index(raised) > job.index("build_graphics(") or "slow_full_build = " in job:
        raise SystemExit("RX_VK_TEST_SLOW_MS does not hold the worker's full build, or a throw out of it would leave "
                         "slow_full_build set on that worker (quick_pipe::Raise)")
    slow = between(draw, "int test_slow_ms() {", "\n}\n")
    if 'getenv("RX_VK_TEST_SLOW_MS")' not in slow or "quick_pipe::slow_ms(" not in slow:
        raise SystemExit("RX_VK_TEST_SLOW_MS is not read strictly")
    body = draw[draw.index("void draw(VkCommandBuffer"):]
    body = body[:body.index("    cur_program = &p;")]
    translation = between(body, "    if (!cp->ready.load(std::memory_order_acquire)) {", "\n    }\n")
    for name in ("quick_pipeline(", "pipe_wait_budget_ms(", "jobs_hurry("):
        if name in translation:
            raise SystemExit(f"{name} applies to a shader still translating: that wait stays as it is "
                             "(https://github.com/NotRexed/Hookshot/pull/1573)")
    pipe = between(body, "    if (!pe->ready.load(std::memory_order_acquire)) {", "\n    if (use.on) use.pipeline_made(")
    for name in ("quick_pipeline(*pe, p, s, at, topo, !may_skip)", "pipe_wait_budget_ms()", "jobs_hurry(pe->ready)",
                 "wait_for(pe->ready)"):
        if name not in pipe:
            raise SystemExit(f"draw()'s pipeline wait lacks {name}")
    if "if (always_wait) {\n            wait_for(pe->ready);\n        } else if (" not in pipe or \
            pipe.index("if (always_wait) {") > pipe.index("quick_pipeline("):
        raise SystemExit("RX_VK_PIPE_SKIP=0 draws with a quick pipeline: it waits for every pipeline")
    for switch in ("RX_VK_QUICK_MS", "RX_VK_PIPE_WAIT_MS", "RX_VK_TEST_SLOW_MS"):
        if f'getenv("{switch}")' not in draw or "atoi(getenv(\"" + switch in draw or "atof(getenv(\"" + switch in draw:
            raise SystemExit(f"{switch} is not read strictly (quick_pipe.h)")
    docs = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for switch in ("RX_VK_QUICK_MS", "RX_VK_PIPE_WAIT_MS", "RX_VK_TEST_SLOW_MS"):
        if f"`{switch}" not in docs:
            raise SystemExit(f"{switch} is not in docs/development.md's switch table")


def cut(draw):
    entry = between(draw, "struct PipeEntry {", "\n};\n") + "\n};\n"
    code = between(draw, "/// (hookshot) RX_VK_QUICK_MS, read once and logged", "\n// ---- samplers")
    return entry + code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    check_source(draw)
    with tempfile.TemporaryDirectory(prefix="quick-pipelines-") as tmp:
        tmp = Path(tmp)
        (tmp / "quick_cut.inc").write_text(cut(draw), encoding="utf-8")
        for name in ("quick_pipe.h", "translate_wait.h"):
            shutil.copy(ROOT / "gpu/vk" / name, tmp / name)
        exe = tmp / "quick-pipelines"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(tmp),
                        str(ROOT / "tests/test_quick_pipelines.cpp"), "-o", str(exe)], check=True)
        for driver, value, want in (("amd", None, "0"), ("nvidia", None, "0"), ("amd", "0", "0"), ("nvidia", "250", "250"),
                                    ("amd", "8", "8"), ("amd", "8ms", "0"), ("nvidia", "fast", "0")):
            env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_QUICK_MS", "RX_VK_PIPE_WAIT_MS")}
            if value is not None:
                env["RX_VK_QUICK_MS"] = value
            subprocess.run([str(exe), driver, want], check=True, timeout=60, env=env)
    print("PASS: quick pipelines: budget a frame, caches first, never into the pipeline cache, replaced by the full build")


if __name__ == "__main__":
    main()
