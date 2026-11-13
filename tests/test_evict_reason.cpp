// evict_reason (gpu/vk/evict_reason.h): why the eviction pass let a texture go, and whether its texels went with it.
// tests/test_evict_reason.py
#include "../gpu/vk/evict_reason.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    using namespace evict_reason;
    expect(classify(false, false) == BUDGET, "neither drawn over nor shifted: the budget");
    expect(classify(false, true) == DRAWN_OVER, "drawn over at the same start");
    expect(classify(true, true) == DEAD, "shifted over (also overwritten): a dead target");
    expect(classify(true, false) == DEAD, "shifted alone: a dead target");

    expect(!texels_lost(false, false, false), "uploaded from the game's memory: made again from there");
    expect(texels_lost(true, false, false), "drawn by the GPU, no write-back, nothing drawn over it: lost");
    expect(!texels_lost(true, false, true), "drawn by the GPU and written back: kept in memory");
    expect(!texels_lost(true, true, false), "drawn over since: nothing left to keep");

    Tally t;
    expect(t.total() == 0, "an empty tally");
    t.add(BUDGET, false, 1 << 20);
    t.add(BUDGET, true, 3 << 20);
    t.add(DEAD, false, 8 << 20);
    t.add(DRAWN_OVER, true, 1 << 20, true);
    expect(t.total() == 4 && t.n[BUDGET] == 2 && t.n[DEAD] == 1 && t.n[DRAWN_OVER] == 1, "counts by reason");
    expect(t.lost == 2 && t.lost_bytes == (4ull << 20), "the lost ones and their bytes");
    expect(t.lost_finalized == 1, "of them, the finalized one");

    char line[160];
    describe(t, line, sizeof line);
    expect(!std::strcmp(line, "budget 2, drawn over 1, dead targets 1; GPU-drawn with no copy kept: 1 live, 1 finalized, 4 MB"), line);
    Tally clean;
    clean.add(DEAD, false, 1);
    describe(clean, line, sizeof line);
    expect(!std::strcmp(line, "budget 0, drawn over 0, dead targets 1"), "nothing lost: no second part");
    char tiny[8];
    describe(t, tiny, sizeof tiny);
    expect(std::strlen(tiny) == sizeof tiny - 1, "a short buffer: cut, still terminated");

    if (failures) return EXIT_FAILURE;
    std::printf("evict reason: all checks passed\n");
    return EXIT_SUCCESS;
}
