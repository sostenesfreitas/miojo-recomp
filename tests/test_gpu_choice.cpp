#include "../gpu/vk/gpu_choice.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::gpu_choice::menu_device;
    CHECK("Auto stays Auto", menu_device(0, 2) == -1);
    CHECK("the n-th usable GPU", menu_device(1, 2) == 0 && menu_device(2, 2) == 1);
    CHECK("a GPU taken out since is Auto", menu_device(2, 1) == -1);
    CHECK("a negative choice (a hand-edited ini) is Auto", menu_device(-1, 2) == -1);
    CHECK("no usable GPU: Auto", menu_device(1, 0) == -1);

    // (the card itself: vendor and device ids, and the how-manieth of that model)
    using mkwvk::gpu_choice::Card;
    using mkwvk::gpu_choice::card_id;
    using mkwvk::gpu_choice::menu_of_card;
    const std::vector<Card> laptop = {{0x8086, 0xa7a0}, {0x10de, 0x2820}};   // (an Intel iGPU, then an RTX card)
    const std::vector<Card> swapped = {{0x10de, 0x2820}, {0x8086, 0xa7a0}};   // (the driver lists them the other way)
    CHECK("a card's id", card_id(laptop, 1) == "10de:2820:0" && card_id(laptop, 0) == "8086:a7a0:0");
    CHECK("no card, no id", card_id(laptop, -1).empty() && card_id(laptop, 2).empty());
    CHECK("the saved card where it is now", menu_of_card(laptop, "10de:2820:0") == 2 && menu_of_card(swapped, "10de:2820:0") == 1);
    CHECK("a card taken out: none", menu_of_card({{0x8086, 0xa7a0}}, "10de:2820:0") == 0 && menu_of_card({}, "10de:2820:0") == 0);
    const std::vector<Card> twins = {{0x1002, 0x744c}, {0x8086, 0xa7a0}, {0x1002, 0x744c}};   // (two of the same card)
    CHECK("two of the same card stay apart", card_id(twins, 0) == "1002:744c:0" && card_id(twins, 2) == "1002:744c:1"
          && menu_of_card(twins, "1002:744c:0") == 1 && menu_of_card(twins, "1002:744c:1") == 3);
    CHECK("the second of a model with one left: that one", menu_of_card(laptop, "10de:2820:1") == 2);
    CHECK("each id round-trips", menu_of_card(twins, card_id(twins, 1)) == 2 && menu_of_card(swapped, card_id(swapped, 0)) == 1);
    CHECK("a hand-edited id is none", menu_of_card(laptop, "") == 0 && menu_of_card(laptop, "10de") == 0
          && menu_of_card(laptop, "10de:2820") == 0 && menu_of_card(laptop, "10de:2820:0x") == 0
          && menu_of_card(laptop, "10de:2820:-1") == 0 && menu_of_card(laptop, "nvidia") == 0);
    std::puts("gpu choice: all checks passed");
}
