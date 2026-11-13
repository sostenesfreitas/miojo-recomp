"""The mesh fallback programs a start has in memory already get their compute pipelines before the title too.

Where mesh programs are drawn through the compute fallback (no mesh shaders, AMD's listed cards, or Mesh shaders = Compute),
the start makes every fallback program's compute pipelines before play: precompile_run and precompile_at_create
(gpu/vk/vk_draw.cpp) ask program_cache_load_some (gpu/vk/vk_progcache.cpp) for every program translated for the fallback.
That found them by their records in the program cache's list, and a program read in earlier in the same start has no
record left there: the state lists' programs, in the start that prepares the shaders. Their compute pipelines were made at
their first draw, which was left out until then (2,037 draws left out and 42 pipelines compiled in play in one session;
850 fallback programs made ready where a later start makes 2,096). Compile and run the rule (gpu/vk/fallback_loaded.h),
and check the source uses it for both starts. No game or GPU is needed.

python -I -B tests/test_fallback_loaded.py
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    start = cache.index("size_t program_cache_load_some(const std::unordered_set<uint64_t>& wanted")
    some = cache[start:cache.index("\n}\n", start)]
    for needle in ('fallback_loaded::on(getenv("RX_VK_FALLBACK_LOADED"))', "if (fallbacks && loaded_too) {",
                   "for (const auto& w : work) given.insert(w.first);", "fallback_loaded::each(programs, given, fn)"):
        if needle not in some:
            raise SystemExit("program_cache_load_some no longer has: " + needle)
    # (under the lock on the programs, after those read in now were given: a key read in now is given once)
    if not some.index("std::lock_guard lk(shader_mu);") < some.index("fn(*p);") < some.index("fallback_loaded::each("):
        raise SystemExit("program_cache_load_some gives the programs in memory before those it reads, or without the lock")
    # (both starts ask for the fallback programs, and make the compute pipelines of what they are given)
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    for head, call in (("void precompile_at_create() {", "}, false, true);"),
                       ("void precompile_run(void (*progress)(size_t done, size_t total)) {", "}, plan.all, true);")):
        at = draw.index(head)
        body = draw[at:draw.index("\n}\n", at)]
        if call not in body or "fallbacks.push_back(&p);" not in body or "fallback_compute_pipelines(" not in body:
            raise SystemExit(head.split("(")[0] + " no longer asks for every mesh fallback program and makes its compute pipelines")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="fallback-loaded-") as tmp:
        exe = Path(tmp) / "fallback-loaded"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/fallback_loaded_test.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == "__main__":
    main()
