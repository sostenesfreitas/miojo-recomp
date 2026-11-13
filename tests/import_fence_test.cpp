// gpu/vk/import_fence.h on real pages: the region that starts at an import's range ends with the range while the fence
// stands, the page it changed has its protection back afterwards, and a page that is not plain read-write is left alone.
#include "import_fence.h"
#include <cstdio>
#include <cstring>

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

#ifdef __linux__
int main() {
    uint8_t page[1] = {0x5a};
    const mkwvk::ImportFence fence(page, 1, true);
    CHECK(!fence.set() && page[0] == 0x5a);
    printf("no fence on Linux, failures=%d\n", bad);
    return bad ? 1 : 0;
}
#else
static SIZE_T page;
static SIZE_T region(const uint8_t* p) {
    MEMORY_BASIC_INFORMATION mi{};
    return VirtualQuery(p, &mi, sizeof mi) == sizeof mi ? mi.RegionSize : 0;
}
static DWORD protect(const uint8_t* p) {
    MEMORY_BASIC_INFORMATION mi{};
    return VirtualQuery(p, &mi, sizeof mi) == sizeof mi ? mi.Protect : 0;
}
int main() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    page = si.dwPageSize;
    const SIZE_T pages = 64, range = 16;
    // (one more page reserved past the committed ones: the committed run ends there by itself)
    uint8_t* base = (uint8_t*)VirtualAlloc(nullptr, page * (pages + 1), MEM_RESERVE, PAGE_NOACCESS);
    if (!base || VirtualAlloc(base, page * pages, MEM_COMMIT, PAGE_READWRITE) != base) return 2;
    memset(base, 0x5a, page * pages);
    uint8_t* after = base + page * range;
    DWORD old = 0;

    CHECK(region(base) == page * pages);   // (the walk an import pays without the fence: the whole run)
    {
        const mkwvk::ImportFence fence(base, page * range, true);
        CHECK(fence.set());
        CHECK(region(base) == page * range);
        CHECK(protect(after) == PAGE_EXECUTE_READWRITE && protect(after + page) == PAGE_READWRITE);
        CHECK(after[0] == 0x5a);   // (read and written as before)
        after[1] = 0x6b;
    }
    CHECK(region(base) == page * pages && protect(after) == PAGE_READWRITE && after[1] == 0x6b);

    {   // (switched off: nothing changes)
        const mkwvk::ImportFence fence(base, page * range, false);
        CHECK(!fence.set() && region(base) == page * pages);
    }
    {   // (the range runs to the end of the committed pages: the page after is reserved)
        const mkwvk::ImportFence fence(base, page * pages, true);
        CHECK(!fence.set() && region(base) == page * pages);
    }
    // (a write watch's read-only page after the range stays read-only, during and after)
    CHECK(VirtualProtect(after, page, PAGE_READONLY, &old));
    {
        const mkwvk::ImportFence fence(base, page * range, true);
        CHECK(!fence.set() && protect(after) == PAGE_READONLY);
    }
    CHECK(protect(after) == PAGE_READONLY);
    CHECK(VirtualProtect(after, page, PAGE_READWRITE, &old));
    // (a watch armed on the page while the fence stands is the one that stays)
    {
        const mkwvk::ImportFence fence(base, page * range, true);
        CHECK(fence.set());
        CHECK(VirtualProtect(after, page, PAGE_READONLY, &old) && old == PAGE_EXECUTE_READWRITE);
    }
    CHECK(protect(after) == PAGE_READONLY);
    CHECK(VirtualProtect(after, page, PAGE_READWRITE, &old));

    for (SIZE_T i = 0; i < page * pages; i++)
        if (base[i] != (i == page * range + 1 ? 0x6b : 0x5a)) {
            CHECK(false);
            break;
        }
    printf("region %llu pages, %llu with the fence, failures=%d\n", (unsigned long long)pages, (unsigned long long)range, bad);
    VirtualFree(base, 0, MEM_RELEASE);
    return bad ? 1 : 0;
}
#endif
