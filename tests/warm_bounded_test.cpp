// warm_bounded (gpu/vk/warm_bounded.h): every item worked on once, never more alive at once than there are threads, a stop
// honoured before the next item, progress reported. No game, no GPU.
#include <algorithm>
#include "../gpu/vk/warm_bounded.h"

#include <cstdio>
#include <mutex>

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

void bounded(size_t n, unsigned threads) {
    std::atomic<int> alive{0}, most{0};
    std::vector<std::atomic<int>> times(n);
    size_t last_tick = 0, ticks = 0;
    const size_t done = warm_bounded::run(n, threads, [&](size_t i) {
        const int now = ++alive;   // (made: a pipeline, in the game)
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        times[i]++;
        --alive;                   // (let go before the thread takes the next)
    }, [] { return false; }, [&](size_t d) { expect(d >= last_tick, "progress never goes back"), last_tick = d, ticks++; });
    expect(done == n, "every item is done");
    for (size_t i = 0; i < n; i++) expect(times[i] == 1, "each item is worked on exactly once");
    expect(most.load() <= (int)std::max(threads, 1u), "never more alive at once than there are threads");
    expect(n == 0 || most.load() >= 1, "something was made");
    expect(alive.load() == 0, "nothing is left alive at the end");
    expect(ticks >= 1 && last_tick == n, "the last progress report is the whole count");
}

void stopped() {
    std::atomic<size_t> worked{0};
    std::atomic<bool> stop{false};
    const size_t done = warm_bounded::run(100000, 4, [&](size_t) {
        if (++worked == 50) stop = true;
    }, [&] { return stop.load(); }, [](size_t) {});
    expect(done == worked.load() && done >= 50 && done < 100000, "a stop ends the work before the next item, and what was done is counted");
}
}   // namespace

int main() {
    bounded(0, 8);
    bounded(1, 8);
    bounded(500, 1);
    bounded(2000, 6);
    bounded(300, 0);   // (no thread count: one)
    stopped();
    // the marker: only a warm that got to its end in a process that plays; never the preparation's own process
    expect(warm_bounded::leaves_marker(false, true, false), "warm_bounded::leaves_marker(false, true, false)");
    expect(!warm_bounded::leaves_marker(true, true, false), "!warm_bounded::leaves_marker(true, true, false)");    // (--prepare-shaders: its cache does not answer the game's asks)
    expect(!warm_bounded::leaves_marker(false, false, false), "!warm_bounded::leaves_marker(false, false, false)");  // (not through)
    expect(!warm_bounded::leaves_marker(false, true, true), "!warm_bounded::leaves_marker(false, true, true)");    // (the window was closed)
    expect(!warm_bounded::leaves_marker(true, false, true), "!warm_bounded::leaves_marker(true, false, true)");
    if (failures) return std::printf("%d checks failed\n", failures), 1;
    std::printf("warm_bounded: ok\n");
    return 0;
}
