// Fixture of tests/test_registration_host_copies.py: gpu/vk/vk_bank.cpp's descriptor_ok, cut from the source, over a host that
// answers a GPU address as host/nvdrv.c's hos_gpu_ptr does with the native NVN layer (host/nvn_native.c's nvn_gpu_guest, cut
// from the source too) and a process whose memory VirtualQuery tells from a table. No pointer is read: the cases ask
// which memory a registration would be copied from.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/* CONSTANTS */

// ---- the host: the guest window, and what it takes for a GPU address
struct nvn_vmap { uint64_t gpu, size; const uint64_t* page; };
static nvn_vmap pool;   // (one virtual pool, in the case that maps it)
static const nvn_vmap* vmap_find(uint64_t gpu) {
    return pool.size && gpu >= pool.gpu && gpu < pool.gpu + pool.size ? &pool : nullptr;
}
/* NVN_GPU_GUEST */
static uint8_t* guest_base() { return (uint8_t*)(uintptr_t)A64_HOST_BASE; }
static uint64_t guest_size() { return HOS_GUEST_SPACE; }
static uint8_t* gpu_ptr(uint64_t va, uint64_t* avail) {   // (hos_gpu_ptr, the native NVN layer's branch)
    const uint64_t g = nvn_gpu_guest(va);
    if (!g || g >= guest_size()) return nullptr;
    *avail = va < guest_size() ? guest_size() - g : VPAGE - g % VPAGE;
    return guest_base() + g;
}
static const struct Host {
    uint8_t* (*guest_base)();
    uint64_t (*guest_size)();
    uint8_t* (*gpu_ptr)(uint64_t, uint64_t*);
} host_table{guest_base, guest_size, gpu_ptr}, *host = &host_table;

// ---- the process's own memory, as VirtualQuery tells it
enum : uint32_t {
    MEM_COMMIT = 0x1000, MEM_FREE = 0x10000,
    PAGE_NOACCESS = 0x1, PAGE_READONLY = 0x2, PAGE_READWRITE = 0x4, PAGE_WRITECOPY = 0x8, PAGE_GUARD = 0x100,
};
struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    size_t RegionSize;
    uint32_t State, Protect;
};
static struct Region { uintptr_t lo, hi; uint32_t protect; } region;   // (one committed region, in the cases that have it)
static size_t VirtualQuery(const void* p, MEMORY_BASIC_INFORMATION* mi, size_t size) {
    if (size < sizeof *mi) return 0;
    const uintptr_t page = (uintptr_t)p & ~(uintptr_t)4095;
    const bool in = (uintptr_t)p >= region.lo && (uintptr_t)p < region.hi;
    *mi = {(void*)page, in ? region.hi - page : 4096, in ? MEM_COMMIT : MEM_FREE, in ? region.protect : PAGE_NOACCESS};
    return sizeof *mi;
}
static unsigned refusals;
static void logf(const char* f, ...) {
    refusals += std::strstr(f, "refused") != nullptr;
}

/* DESCRIPTOR_OK */

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s: failed: %s\n", name.c_str(), #x); return 1; } } while (0)

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    const size_t n = 24;   // (a registration's size: no case depends on it but "short")
    const auto at = [](uint64_t a) { return (const void*)(uintptr_t)a; };
    const auto commit = [](uint64_t lo, uint64_t size, uint32_t protect) { region = {(uintptr_t)lo, (uintptr_t)(lo + size), protect}; };
    // (the address the handler committed while a Windows run handed over its kept calls: issue 1117's log)
    const uint64_t heap_low = 0x9873eec;
    if (name == "guest") {   // the NVN layer's pointers: a guest object's host pointer, up to the window's last bytes
        CHECK(descriptor_ok(guest_base() + 0x1040000000ull, n) == guest_base() + 0x1040000000ull);
        CHECK(descriptor_ok(guest_base() + guest_size() - n, n) == guest_base() + guest_size() - n);
        CHECK(descriptor_ok(guest_base() + guest_size() - n + 1, n) == nullptr);
    } else if (name == "copy-in-guest-address-range") {
        // a host copy whose address the host would take for a guest address: the copy is read, not guest memory there
        CHECK(nvn_gpu_guest(heap_low) == heap_low);
        commit(0x9870000, 0x20000, PAGE_READWRITE);
        CHECK(descriptor_ok(at(heap_low), n) == at(heap_low));
        CHECK(refusals == 0);
    } else if (name == "copy-at-virtual-pool-address") {
        // the same above 1 TB, where a heap address can equal a mapped virtual pool's GPU address
        static const uint64_t pages[1] = {0x1040000000ull};
        pool = {0x1e200000000ull, VPAGE, pages};
        CHECK(nvn_gpu_guest(0x1e200000040ull) == 0x1040000040ull);
        commit(0x1e200000000ull, 0x10000, PAGE_READWRITE);
        CHECK(descriptor_ok(at(0x1e200000040ull), n) == at(0x1e200000040ull));
    } else if (name == "copy-above-the-window") {   // glibc's and a high-entropy Windows heap: no GPU address
        CHECK(nvn_gpu_guest(0x7f1234567000ull) == 0);
        commit(0x7f1234560000ull, 0x10000, PAGE_READWRITE);
        CHECK(descriptor_ok(at(0x7f1234567000ull), n) == at(0x7f1234567000ull));
        commit(0x7f1234560000ull, 0x10000, PAGE_READONLY);
        CHECK(descriptor_ok(at(0x7f1234567000ull), n) == at(0x7f1234567000ull));
    } else if (name == "nothing-there") {   // an address no memory is at is refused, also where a guest address could be
        CHECK(descriptor_ok(at(heap_low), n) == nullptr);
        CHECK(descriptor_ok(at(0x7f1234567000ull), n) == nullptr);
        CHECK(descriptor_ok(nullptr, n) == nullptr);
        CHECK(refusals == 2);
    } else if (name == "not-readable") {
        commit(0x9870000, 0x20000, PAGE_READWRITE | PAGE_GUARD);
        CHECK(descriptor_ok(at(heap_low), n) == nullptr);
        commit(0x9870000, 0x20000, PAGE_NOACCESS);
        CHECK(descriptor_ok(at(heap_low), n) == nullptr);
    } else if (name == "short") {   // the region ends inside the registration
        commit(0x9870000, 0x4000, PAGE_READWRITE);
        CHECK(descriptor_ok(at(0x9874000 - n), n) == at(0x9874000 - n));
        CHECK(descriptor_ok(at(0x9874000 - n + 1), n) == nullptr);
    } else {
        std::fprintf(stderr, "unknown case %s\n", name.c_str());
        return 2;
    }
    return 0;
}
