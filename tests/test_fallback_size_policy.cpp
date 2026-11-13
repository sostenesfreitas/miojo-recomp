// fallback_size (gpu/vk/fallback_size_policy.h): the mesh fallback compute stages not given to RADV on GCN 1-4.
// tests/test_fallback_size_policy.py
#include "../gpu/vk/fallback_size_policy.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

// A module: the header, then the instructions as (opcode, operand words)
static std::vector<uint32_t> module(const std::vector<std::vector<uint32_t>>& insts) {
    std::vector<uint32_t> w{0x07230203u, 0x00010300u, 0, 100, 0};
    for (const auto& i : insts) {
        w.push_back((uint32_t)(i.size() << 16) | i[0]);
        w.insert(w.end(), i.begin() + 1, i.end());
    }
    return w;
}

int main() {
    using namespace fallback_size;
    const std::vector<uint32_t> none = module({{248, 1}, {253}});
    expect(spirv_phis(none.data(), none.size()) == 0, "no phis");
    // OpLabel, two OpPhi (type, result, value, parent, value, parent), OpIAdd, OpReturn
    const std::vector<uint32_t> two = module({{248, 1}, {245, 7, 10, 11, 2, 12, 3}, {245, 7, 13, 14, 2, 15, 3}, {128, 7, 16, 10, 13}, {253}});
    expect(spirv_phis(two.data(), two.size()) == 2, "two phis counted, other instructions not");
    std::vector<uint32_t> cut = two;
    cut.resize(cut.size() - 2);   // (the OpReturn and the OpIAdd's last word gone: an instruction longer than what is left)
    expect(spirv_phis(cut.data(), cut.size()) == 0, "a module cut short: 0");
    std::vector<uint32_t> zero = two;
    zero[5] = 0;   // (a word count of 0)
    expect(spirv_phis(zero.data(), zero.size()) == 0, "a zero-length instruction: 0, not a loop");
    std::vector<uint32_t> bad = two;
    bad[0] = 0x12345678u;
    expect(spirv_phis(bad.data(), bad.size()) == 0, "not SPIR-V: 0");
    expect(spirv_phis(two.data(), 3) == 0, "shorter than a header: 0");

    uint32_t limit = 7;
    expect(parse_limit("50000", limit) && limit == 50000, "a number");
    expect(parse_limit("0", limit) && limit == 0, "0: no limit");
    expect(parse_limit("4294967295", limit) && limit == 4294967295u, "the largest");
    limit = 7;
    expect(!parse_limit("4294967296", limit) && limit == 7, "past 32 bits: refused, unchanged");
    expect(!parse_limit("-1", limit), "negative: refused (strtoul wraps it)");
    expect(!parse_limit("off", limit), "not a number: refused (atoi reads it as 0, no limit)");
    expect(!parse_limit("12k", limit) && !parse_limit(" 5", limit) && !parse_limit("", limit) && !parse_limit(nullptr, limit),
           "trailing text, a space, empty, unset: refused");

    expect(!too_big(true, 29372, kRadvGcnPhiLimit), "the biggest mesh programs RADV GCN builds (29,400 phis): built");
    expect(too_big(true, 79947, kRadvGcnPhiLimit), "the two it never finishes (80,000 phis): left out");
    expect(!too_big(false, 79947, kRadvGcnPhiLimit), "another driver or GFX9 on: built whatever the size");
    expect(!too_big(true, 79947, 0), "limit 0: no limit");
    expect(too_big(true, 101, 100) && !too_big(true, 100, 100), "over the limit, not at it");

    if (failures) return 1;
    std::printf("fallback_size_policy: ok\n");
    return 0;
}
