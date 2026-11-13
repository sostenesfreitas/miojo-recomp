// SPDX-License-Identifier: GPL-3.0-or-later
// gpu/vk/module_ids.h alone: the side file of shader module identifiers (tests/test_module_ids.py)
#include "../gpu/vk/module_ids.h"
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string path = std::string(argv[1]) + "/ids.bin";
    const uint8_t driver[16] = {1, 2, 3}, other[16] = {9};
    uint8_t a[32], b[20];
    for (int i = 0; i < 32; i++) a[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 20; i++) b[i] = (uint8_t)(200 - i);
    module_ids::Id got;
    {
        module_ids::Store s;
        s.open(path, 77, driver);   // (no file yet)
        CHECK(s.size() == 0 && !s.pending() && s.flush());
        CHECK(s.add(10, 1, a, 32) && s.add(10, 2, b, 20) && s.pending());
        CHECK(!s.add(10, 1, b, 20));            // (known: the first stays)
        CHECK(!s.add(11, 1, a, 0) && !s.add(11, 1, a, 33));   // (not an identifier)
        CHECK(s.find(10, 1, &got) && got.n == 32 && !std::memcmp(got.bytes, a, 32));
        CHECK(!s.find(10, 3, &got) && s.has(10, 2) && !s.has(11, 1));
        CHECK(s.flush() && !s.pending());
        CHECK(s.add(12, 0, b, 20) && s.flush());   // (appended to the file it began)
    }
    {
        module_ids::Store s;
        s.open(path, 77, driver);
        CHECK(s.size() == 3 && !s.pending());
        CHECK(s.find(10, 2, &got) && got.n == 20 && !std::memcmp(got.bytes, b, 20));
        CHECK(s.find(12, 0, &got) && got.n == 20);
    }
    {   // another program cache, another driver: none of it is used, and the next flush begins the file again
        module_ids::Store s;
        s.open(path, 78, driver);
        CHECK(s.size() == 0);
        s.open(path, 77, other);
        CHECK(s.size() == 0);
        CHECK(s.add(20, 5, a, 32) && s.flush());
        module_ids::Store t;
        t.open(path, 77, other);
        CHECK(t.size() == 1 && t.has(20, 5));
        t.open(path, 77, driver);
        CHECK(t.size() == 0);
    }
    {   // cut short in an entry, and an entry that is none: what read before it is kept
        module_ids::Store s;
        s.open(path, 77, other);
        CHECK(s.add(21, 1, b, 20) && s.add(22, 1, b, 20) && s.flush());
        FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f);
        std::vector<uint8_t> all(32 + 3 * 52);
        CHECK(std::fread(all.data(), 1, all.size(), f) == all.size());
        std::fclose(f);
        f = std::fopen(path.c_str(), "wb");
        CHECK(f && std::fwrite(all.data(), 1, all.size() - 10, f) == all.size() - 10);
        std::fclose(f);
        module_ids::Store t;
        t.open(path, 77, other);
        CHECK(t.size() == 2 && t.has(20, 5) && t.has(21, 1) && !t.has(22, 1));
        const uint32_t big = 99;
        std::memcpy(all.data() + 32 + 52 + 16, &big, 4);   // (the second entry's size)
        f = std::fopen(path.c_str(), "wb");
        CHECK(f && std::fwrite(all.data(), 1, all.size(), f) == all.size());
        std::fclose(f);
        t.open(path, 77, other);
        CHECK(t.size() == 1 && t.has(20, 5));
    }
    std::printf("module_ids: ok\n");
    return 0;
}
