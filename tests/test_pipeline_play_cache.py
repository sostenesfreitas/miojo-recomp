"""The play caches of gpu/vk/vk_draw.cpp, extracted unmodified, on a model of Vulkan pipeline caches and a driver's own disk cache.

In play every save wrote the whole pipeline cache again (0.5-1.2 GB) for what play had added: NVIDIA copies into the cache a
pipeline is made with each pipeline it takes from its own disk cache. Now what play makes goes into a cache of its own, one file a
session written again at each save (hookshot_vk_pipelines_play_<n>.bin), and the next start merges those files into one.

Checked: in play the start's cache is left as it was read, one it holds comes from it, one the driver has goes into the generation,
one nothing has is compiled into the generation; a save writes the generation alone and goes on into a new one; a write that did not
land is tried again; a save waits for a pipeline being made in the generation it takes away; the next start merges the play files,
drops another driver's, ignores other names and numbers on from the highest; at 16 they are written into the one file, and kept
when that write fails; RX_VK_PLAY_CACHE=0 and a device without pipelineCreationCacheControl keep the one cache; with the fetch lock on (Intel), a
save while a build sits between two of create_cached's builds finishes holding no lock, and a save with a bound (the window's
close) keeps a generation a stuck build is in rather than hang.

No Vulkan loader, GPU or game. python -I -B tests/test_pipeline_play_cache.py [--cxx clang++] [--source gpu/vk/vk_draw.cpp]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ["route", "save", "retry", "users", "merge", "merge-read-failed", "merge-create-failed", "compact", "compact-failed", "off", "no-control", "lock-users"]


def play_block(source):
    start_marker = "struct PlayCache {"
    end_marker = "// ---- (end of the play caches) ----"
    if source.count(start_marker) != 1 or source.count(end_marker) != 1:
        raise SystemExit("FAIL: the play caches of gpu/vk/vk_draw.cpp were not found")
    return source[source.index(start_marker):source.index(end_marker)]


def lock_block(source):
    start_marker = "std::shared_timed_mutex pcache_fetch_mu;"
    end_marker = "// ---- (hookshot) What play makes goes into a cache of its own"
    if source.count(start_marker) != 1 or source.count(end_marker) != 1:
        raise SystemExit("FAIL: the pipeline cache's fetch lock in gpu/vk/vk_draw.cpp was not found")
    return source[source.index(start_marker):source.index(end_marker)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++") or "g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    a = p.parse_args()
    failed = []
    with tempfile.TemporaryDirectory(prefix="hookshot-play-cache-") as temp:
        temp = Path(temp)
        source = a.source.read_text(encoding="utf-8")
        (temp / "pipeline_play_cache.inc").write_text(play_block(source), encoding="utf-8")
        (temp / "pcache_fetch_lock.inc").write_text(lock_block(source), encoding="utf-8")
        exe = temp / "check.exe"
        cmd = [a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(temp), "-I" + str(ROOT / "gpu/vk"),
               str(ROOT / "tests/pipeline_play_cache_test.cpp"), "-o", str(exe)]
        if os.name == "nt":
            cmd += ["-D_CRT_SECURE_NO_WARNINGS"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode:
            print(result.stdout + result.stderr)
            return 1
        for case in CASES:
            data = temp / ("data-" + case)
            data.mkdir()
            env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_PLAY_CACHE", "RX_VK_PCACHE_FETCH_LOCK")}
            if case == "off":
                env["RX_VK_PLAY_CACHE"] = "0"
            if case == "lock-users":   # (the fetch lock on, as on Intel's Windows driver)
                env["RX_VK_PCACHE_FETCH_LOCK"] = "1"
            # (each case a fresh process: the switch is read once, the generations are the process's)
            run = subprocess.run([str(exe), case, str(data)], capture_output=True, text=True, timeout=30, env=env)
            sys.stdout.write(run.stdout)
            if run.returncode:
                sys.stdout.write(run.stderr)
                failed.append(case)
    if failed:
        print("FAIL: " + ", ".join(failed))
        return 1
    print("all %d cases passed" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
