"""The pipeline cache's fetches and builds take turns on Intel (gpu/vk/vk_draw.cpp PcacheFetch, PcacheUse;
https://github.com/NotRexed/Hookshot/issues/999).

Intel's Windows driver faults in vkGetPipelineCacheData while other threads build pipelines with the cache. The check cuts the
lock out of vk_draw.cpp and runs it with no Vulkan: a fetch that may not wait is refused while a build holds the cache and had
once it is let go, a waiting fetch waits for the build, a queued fetch with a bound gives up on a build that does not end,
a fetch inside a fetch on the same thread and a build inside one do not lock themselves out, and with the lock off nothing
is held. The source is checked for the lock held around vkGetPipelineCacheData alone: never across play_cache_save, whose
wait for a generation's users would wait for a build that waits for the lock (tests/test_pipeline_play_cache.py lock-users
runs that save against such a build).
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

HEAD = r'''
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
typedef struct VkPipelineCache_T* VkPipelineCache;
struct Props { unsigned vendorID; };
struct Dev { Props props; } dev{{0x8086}};
bool env_on(const char* name) { const char* v = std::getenv(name); return v && std::strcmp(v, "0"); }
'''

MAIN = r'''
int main(int argc, char** argv) {
    const bool lock_on = argc > 1 && !std::strcmp(argv[1], "on");
    CHECK(pcache_fetch_lock_on() == lock_on);
    VkPipelineCache cache = (VkPipelineCache)0x10;
    if (!lock_on) {   // (off: a fetch is had at once, a build holds nothing)
        PcacheUse use(cache);
        PcacheFetch fetch(kFetchPollMs, false);
        CHECK(fetch.held());
        return 0;
    }
    std::atomic<int> stage{0};
    std::thread build([&] {
        PcacheUse use(cache);
        stage = 1;
        while (stage.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    while (stage.load() < 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {   // (a build under way: the shared side cannot be had alone)
        CHECK(!pcache_fetch_mu.try_lock());
    }
    stage = 2;
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<bool> other_built{false};
    std::thread other;
    {
        PcacheFetch fetch(-1);   // (waits for the build)
        CHECK(fetch.held());
        CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(40));
        {
            PcacheFetch inner(-1);     // (a fetch inside a fetch on the same thread)
            CHECK(inner.held());
            PcacheUse use(cache);      // (a build on the fetching thread does not wait for itself)
        }
        CHECK(fetch.held());
        other = std::thread([&] { PcacheUse u(cache); other_built = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(!other_built.load());   // (another thread's build waits for the fetch)
    }
    other.join();   // (the fetch let go: the build goes on)
    CHECK(other_built.load());
    build.join();
    {   // (no build now: a fetch that may not wait is had)
        PcacheFetch fetch(kFetchPollMs, false);
        CHECK(fetch.held());
    }
    {   // (a build held for longer than the background save's patience: the save is put off, not stalled)
        std::atomic<bool> go{false};
        std::thread slow([&] { PcacheUse u(cache); go = true; std::this_thread::sleep_for(std::chrono::milliseconds(2500)); });
        while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        PcacheFetch fetch(kFetchPollMs, false);
        CHECK(!fetch.held());
        {   // (queued with a bound, the window's close: gives up too, it does not hang)
            PcacheFetch queued(100, true);
            CHECK(!queued.held());
        }
        slow.join();
    }
    std::printf("ok\n");
    return 0;
}
'''


def cut(source, start, end):
    i = source.index(start)
    return source[i:source.index(end, i)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    lock = cut(draw, "std::shared_timed_mutex pcache_fetch_mu;", "// ---- (hookshot) What play makes goes into a cache of its own")
    # (every fetch of a cache's data is a PcacheFetch, every build with a cache a PcacheUse; a fetch is held around
    # vkGetPipelineCacheData alone, in a scope of its own, never across play_cache_save and its wait for a generation's users)
    save = cut(draw, "void pipeline_cache_save(bool force) {", "\n}\n")
    assert "PcacheFetch lock(wait_ms, queue);" in save and "if (!had)" in save and "kFetchPutOffs" in save
    assert save.count("vkGetPipelineCacheData") == 1, "a fetch in the save outside its locked fetch"
    write = cut(draw, "size_t play_file_write(", "\n}\n")
    assert write.count("PcacheFetch fetch(wait_ms, queue);") == 2 and write.count("vkGetPipelineCacheData") == 2
    assert "PcacheFetch fetch(-1);" in cut(draw, "bool pcache_saved_now() {", "\n}\n")
    for name in ("void pipeline_cache_close() {", "void pipeline_cache_save_play() {", "void play_cache_save("):
        assert "PcacheFetch" not in cut(draw, name, "\n}\n"), name + " holds the fetch lock across play_cache_save"
    assert "play_cache_save(kFetchQueueMs)" in cut(draw, "void pipeline_cache_close() {", "\n}\n")
    # (the users wait bounded only with the lock on, polled or queued: off, the close waits for the builds as before)
    assert "const bool bounded = pcache_fetch_lock_on() && wait_ms >= 0;" in cut(draw, "void play_cache_save(", "\n}\n")
    assert save.count("return put_off_now();") == 2, "a save whose data fetch was put off is lost without a log line"
    assert "create_cached(guarded_graphics_held, ci, pl)" in draw and "create_cached(guarded_compute_held, ci, pl)" in draw
    assert "guarded_compute(pcache," not in draw, "a build with pcache that does not hold it from a fetch"
    with tempfile.TemporaryDirectory(prefix="pcache-fetch-") as tmp:
        source, exe = Path(tmp) / "check.cpp", Path(tmp) / "check.exe"
        source.write_text(HEAD + lock + MAIN, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-O1", "-pthread", str(source), "-o", str(exe)], check=True)
        for mode, env in (("on", {"RX_VK_PCACHE_FETCH_LOCK": "1"}), ("off", {"RX_VK_PCACHE_FETCH_LOCK": "0"})):
            subprocess.run([str(exe), mode], check=True, timeout=60, env={**os.environ, **env})
    print("PASS: on Intel a pipeline cache's data is fetched with no build under way, and the background save does not stall builds")


if __name__ == "__main__":
    main()
