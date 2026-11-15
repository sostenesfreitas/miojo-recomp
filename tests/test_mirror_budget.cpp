// The VRAM mirrors' budget by the card (gpu/vk/mirror_budget.h): tests/test_mirror_budget.py compiles and runs this.
#include "../gpu/vk/mirror_budget.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    using namespace mkwvk::mirror_rule;
    const uint64_t MB = 1ull << 20, GB = 1ull << 30;
    // Auto: an eighth up to 4.5 GB, a quarter above it, at most 3 GB; 3 GB when the card is not known
    CHECK(auto_bytes(2 * GB) == 256 * MB);
    CHECK(auto_bytes(4 * GB) == 512 * MB);                      // (a GTX 1650, an RX 5500M: as before)
    CHECK(auto_bytes(4 * GB + 512 * MB) == 576 * MB);           // (the line itself: an eighth)
    CHECK(auto_bytes(6 * GB) == 1536 * MB);                     // (a 2060, a 3060 Laptop: a quarter, not the eighth's 768)
    CHECK(auto_bytes(8 * GB) == 2048 * MB);                     // (as before)
    CHECK(auto_bytes(12 * GB) == 3072 * MB && auto_bytes(24 * GB) == 3072 * MB);   // (at most 3 GB, as before)
    CHECK(auto_bytes(0) == 3072 * MB);
    // a preset's mirrors never past Auto's on the card; the VRAM budget row's own choice as chosen, but on a small card capped too
    CHECK(budget_bytes(1536 * MB, true, 6 * GB) == 1536 * MB);  // (Low on a 6 GB card)
    CHECK(budget_bytes(1536 * MB, true, 4 * GB) == 512 * MB);   // (Low on a 4 GB card: where it was)
    CHECK(budget_bytes(256 * MB, true, 4 * GB) == 256 * MB);    // (Minimum: its own, smaller)
    CHECK(budget_bytes(1024 * MB, true, 8 * GB) == 1024 * MB);  // (Balanced on an 8 GB card: its own)
    CHECK(budget_bytes(3072 * MB, true, 16 * GB) == 3072 * MB); // (Ultra on a 16 GB card)
    CHECK(budget_bytes(1536 * MB, false, 4 * GB) == 512 * MB);  // (3.6 GB chosen on the row on a 4 GB card: 512, as before)
    CHECK(budget_bytes(3072 * MB, false, 4 * GB) == 512 * MB);  // (11 GB chosen on the row on a 4 GB card: Auto's too)
    CHECK(budget_bytes(256 * MB, false, 4 * GB) == 256 * MB);   // (a smaller choice on a small card: its own)
    CHECK(budget_bytes(3072 * MB, false, 6 * GB) == 3072 * MB); // (11 GB chosen on the row on a 6 GB card: the player's)
    CHECK(budget_bytes(1536 * MB, false, 8 * GB) == 1536 * MB); // (3.6 GB chosen on the row on an 8 GB card: the player's)
    CHECK(small_card(4 * GB + 512 * MB) && !small_card(4 * GB + 513 * MB) && !small_card(0));
    CHECK(budget_bytes(1536 * MB, true, 0) == 1536 * MB);       // (the card not known: as asked)
    std::printf("PASS: the mirrors' budget by the card\n");
    return 0;
}
