"""Work bounded in what is alive at once (gpu/vk/warm_bounded.h, used by gpu/vk/vk_draw.cpp precompile_warm): the first start
after a preparation with RX_VK_LOAD=create builds every noted pipeline into the pipeline cache and lets each go, instead of
keeping them all ready as the precompile does (which a 16 GB PC cannot hold). Compile and run the rule: every item once, never
more alive than threads, a stop honoured, progress reported. And the source: precompile_warm destroys each pipeline it builds
before it takes the next note, keeps no program, and writes the marker only when it got through. No game or GPU is needed.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    start = draw.index("void precompile_warm(void (*progress)(size_t done, size_t total)) {")
    warm = draw[start:draw.index("\n}\n", start)]
    for needle in ("warm_bounded::run(", "program_cache_borrow(", "vkDestroyPipeline(dev.device, made, nullptr);",
                   "warm_bounded::leaves_marker(prepare_only, all, prepare_stopped())) precompile_built_write(total);",
                   'static const bool prepare_only = env_on("HOOKSHOT_PREPARE_ONLY");',
                   "program_cache_compute_keys()", "cached_compute(&ci, &made)"):
        if needle not in warm:
            raise SystemExit("precompile_warm no longer has: " + needle)
    for kept in ("pipelines.emplace", "pipeline_keep_ready(", "programs[", "program_cache_load_some("):
        if kept in warm:
            raise SystemExit("precompile_warm keeps what it builds: " + kept)
    if warm.index("build_graphics(") > warm.index("vkDestroyPipeline(dev.device, made, nullptr);"):
        raise SystemExit("precompile_warm does not destroy the pipeline it has just built")
    run = draw[draw.index("void precompile_run(void (*progress)(size_t done, size_t total)) {"):]
    run = run[:run.index("const auto t0 = std::chrono::steady_clock::now();")]
    if "if (!precompile_covers_notes()) precompile_warm(progress);" not in run:
        raise SystemExit("precompile_run does not warm the notes before a load at create")
    # (the marker is the play process's alone: the preparation's own process warms, leaves none, and does not go on to load
    # at create calls or to the keep-everything precompile; and a start that dropped the pipeline cache's files drops the marker)
    if warm.count("precompile_built_write(") != 2 or warm.index("pipeline_cache_save(true);") > warm.index("warm_bounded::leaves_marker("):
        raise SystemExit("precompile_warm writes the marker by another way than warm_bounded::leaves_marker, or before the cache is saved")
    if 'if (prepare_stopped() || env_on("HOOKSHOT_PREPARE_ONLY")) return;' not in run:
        raise SystemExit("a --prepare-shaders run goes on after its warm")
    if "if (pcache_drop_stale()) remove(precompile_built_path().c_str());" not in run or \
            run.index("pcache_drop_stale()") > run.index("precompile_covers_notes()"):
        raise SystemExit("the marker outlives the pipeline cache files a start dropped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="warm-bounded-") as tmp:
        exe = Path(tmp) / "warm-bounded"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-pthread",
                        str(ROOT / "tests/warm_bounded_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == "__main__":
    main()
