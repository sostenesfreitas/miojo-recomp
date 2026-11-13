// tests/test_fallback_loaded.py: the mesh fallback programs already in memory are given to the step that makes their compute
// pipelines (gpu/vk/fallback_loaded.h), as program_cache_load_some uses it.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../gpu/vk/fallback_loaded.h"

static int failures = 0;
#define CHECK(x) do { if (!(x)) std::printf("failed: %s (line %d)\n", #x, __LINE__), failures++; } while (0)

struct Program {
    uint64_t key = 0, variant = 0;
    bool fallback = false, failed = false, compute = false;
    std::atomic<bool> ready{false};
};
using Programs = std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>>;

static void add(Programs& programs, uint64_t key, uint64_t variant, bool fallback, bool ready = true, bool failed = false) {
    auto p = std::make_unique<Program>();
    p->key = key, p->variant = variant, p->fallback = fallback, p->failed = failed, p->ready = ready;
    programs[key].push_back(std::move(p));
}

int main() {
    // the switch: on unless it says 0
    CHECK(fallback_loaded::on(nullptr) && fallback_loaded::on("1") && fallback_loaded::on(""));
    CHECK(!fallback_loaded::on("0"));

    Programs programs;
    add(programs, 1, 0, true);            // a state list's fallback program, read in before: no record left to find it by
    add(programs, 1, 7, true);            // (its other variant)
    add(programs, 2, 0, false);           // an ordinary graphics program read in before
    add(programs, 3, 0, true);            // read in now: its key is among those given, the caller had every variant of it
    add(programs, 4, 0, true, false);     // still translating
    add(programs, 5, 0, true, true, true);   // its translation failed
    add(programs, 6, 9, true);
    programs[7].push_back(nullptr);
    const std::unordered_set<uint64_t> given{3};
    std::set<std::pair<uint64_t, uint64_t>> got;
    const size_t n = fallback_loaded::each(programs, given, [&](Program& p) { got.insert({p.key, p.variant}); });
    CHECK(n == 3 && got.size() == 3);
    CHECK(got.count({1, 0}) && got.count({1, 7}) && got.count({6, 9}));
    CHECK(!got.count({2, 0}) && !got.count({3, 0}) && !got.count({4, 0}) && !got.count({5, 0}));
    // nothing in memory, or every key given: none
    CHECK(fallback_loaded::each(Programs{}, given, [](Program&) { failures++; }) == 0);
    const std::unordered_set<uint64_t> all{1, 2, 3, 4, 5, 6, 7};
    CHECK(fallback_loaded::each(programs, all, [](Program&) { failures++; }) == 0);
    if (failures) return std::printf("%d checks failed\n", failures), 1;
    std::printf("fallback_loaded: ok\n");
    return 0;
}
