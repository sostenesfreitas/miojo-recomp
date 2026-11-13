// The presenter thread's queue choice (gpu/vk/presenter_queue.h) on real devices' family layouts: never a queue another part of the
// renderer takes. Built and run by tests/test_present_thread.py
#include <cstdio>

#include "../gpu/vk/presenter_queue.h"

using namespace mkwvk;
static int failures;
static void check(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}
static void expect(const char* what, const QueueFamilyInfo* f, uint32_t n, uint32_t graphics, uint32_t compute_taken, bool found,
                   uint32_t family = 0, uint32_t index = 0) {
    uint32_t fa = 99, ix = 99;
    const bool got = presenter_queue_pick(f, n, graphics, compute_taken, fa, ix);
    char line[256];
    std::snprintf(line, sizeof line, "%s: got %s family %u queue %u", what, got ? "a queue," : "none,", fa, ix);
    check(got == found && (!found || (fa == family && ix == index)), line);
    // (and never queue 0 of the replay's family, nor queue 0 of the family the presents' own queue takes)
    if (got) check(!(fa == graphics && ix == 0) && !(fa == compute_taken && ix == 0), "the presenter shares a queue");
}
int main() {
    const uint32_t G = kQueueGraphics | kQueueCompute, C = kQueueCompute, T = 0x4;
    // NVIDIA (RTX 3090, 620.70): graphics family with 16 queues
    const QueueFamilyInfo nv[] = {{G, 16}, {T, 2}, {C | T, 8}, {T, 1}, {T, 1}, {T, 1}, {G, 1}};
    expect("NVIDIA", nv, 7, 0, UINT32_MAX, true, 0, 1);
    expect("NVIDIA, presents' own queue on compute family 2", nv, 7, 0, 2, true, 0, 1);
    // RADV (RX 580): one graphics queue, a compute family of 4 (or 8)
    const QueueFamilyInfo radv[] = {{G | T, 1}, {C | T, 4}, {T, 1}};
    expect("RADV", radv, 3, 0, UINT32_MAX, true, 1, 0);
    expect("RADV, presents' own queue on compute family 1 (queue 0)", radv, 3, 0, 1, true, 1, 1);
    // a compute family of one queue that the presents' own queue already takes: none free, the game's thread presents
    const QueueFamilyInfo one[] = {{G | T, 1}, {C | T, 1}};
    expect("one compute queue, taken", one, 2, 0, 1, false);
    expect("one compute queue, free", one, 2, 0, UINT32_MAX, true, 1, 0);
    // one family for everything, one queue: none
    const QueueFamilyInfo solo[] = {{G | T, 1}};
    expect("a single queue", solo, 1, 0, UINT32_MAX, false);
    // the graphics family not first (an Intel-like layout with the second graphics family elsewhere): never another graphics family
    const QueueFamilyInfo gl[] = {{T, 1}, {G | T, 1}, {G | T, 1}};
    expect("a second graphics family is not taken", gl, 3, 1, UINT32_MAX, false);
    if (!failures) std::printf("PASS: the presenter's queue is a second graphics queue, else a free compute queue, never a taken one\n");
    return failures ? 1 : 0;
}
