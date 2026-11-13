// (hookshot) Work done one item at a time on a few threads, each item made, used and let go before the thread takes the
// next: no more than `threads` items are alive at once, however many there are (vk_draw.cpp precompile_warm: every noted
// pipeline built into the pipeline cache and destroyed, where keeping them all ready needs more memory than some PCs have).
// tests/test_warm_bounded.py.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace warm_bounded {

/// work(i) for every i < n, on up to `threads` threads (at least one), each call finished before its thread takes another
/// item. stop(): asked before each item; once true no new item is taken. tick(done): called on the calling thread about
/// every 50 ms while the others work, and once at the end. Returns how many items were done
template <class Work, class Stop, class Tick>
size_t run(size_t n, unsigned threads, Work&& work, Stop&& stop, Tick&& tick) {
    std::atomic<size_t> next{0}, done{0};
    std::atomic<unsigned> busy{0};
    const auto worker = [&] {
        for (;;) {
            if (stop()) break;
            const size_t i = next.fetch_add(1);
            if (i >= n) break;
            work(i);
            done.fetch_add(1);
        }
        busy.fetch_sub(1);
    };
    const unsigned count = n ? (unsigned)std::min<size_t>(threads ? threads : 1, n) : 0;
    busy = count;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < count; t++) pool.emplace_back(worker);
    while (busy.load()) {
        tick(done.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (std::thread& t : pool) t.join();
    tick(done.load());
    return done.load();
}


/// Whether a warm may leave the marker that says every noted pipeline is in the pipeline cache (hookshot_pipelines.bin.built),
/// after which a start loads at create calls and builds nothing itself. Only a warm that got to its end, and only in a process
/// that plays: on NVIDIA the cache a --prepare-shaders process writes does not answer the asks of the play process that
/// follows (0 of 32 of a sample; 3,720 of 3,880 pipelines "in no cache" in the first session, compiled in play with draws left
/// out), while what a play process builds is answered from then on. So the preparation's process never leaves the marker,
/// and the first start of the game after it warms again, itself
inline bool leaves_marker(bool prepare_only, bool got_to_the_end, bool stopped) { return !prepare_only && got_to_the_end && !stopped; }
} // namespace warm_bounded
