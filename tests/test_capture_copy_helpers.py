"""The capture's copy helpers: copy_pages and copy_pairs share them, and each call waits for its own parts only.

take_capture's texture shadow copies its pages on the helpers (copy_pairs), and a capture copies guest pages on them
(copy_pages), possibly from another thread at the same time. Each call must return with every one of its own copies
made, whatever the other caller has queued, and none may wait forever. A caller runs its own parts no helper has taken:
with every helper busy on another caller's copies, copy_pairs (pools_mu and shadow_mu held) still returns. The shadow pass itself is checked by its shape:
the pages copied now keep their slots and are copied through copy_pairs, the others written are erased.

python -I -B tests/test_capture_copy_helpers.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, tail=""):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if tail:
        end = text.index(tail, end) + len(tail)
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    take = block(source, "bool take_capture(")
    for needle, why in (("copy_pairs(pairs.data()", "the shadow's copies do not go through the copy helpers"),
                        ("c->slots[j] == i", "the pages copied now are erased before they are copied again"),
                        ("shadow_erase(c->copied[i])", "a full shadow keeps the old copy of a page it could not replace")):
        if needle not in take:
            raise SystemExit("take_capture: " + why)
    if take.index("copy_pairs(pairs.data()") > take.index("const auto tk1"):
        raise SystemExit("take_capture: the shadow's copies are made after shadow_mu is let go")
    parts = [source[source.index("struct CopyPair {"):source.index("\n", source.index("struct CopyPair {"))],
             block(source, "struct CopyPart {", ";"),
             block(source, "void copy_part(const CopyPart& w)"),
             block(source, "struct CopyHelpers {", ";"),
             block(source, "void copy_helper_main()"),
             block(source, "int capture_copy_helpers()"),
             block(source, "void copy_split(CopyPart whole"),
             block(source, "void copy_pages(const uint32_t* pages"),
             block(source, "void copy_pairs(const CopyPair* pairs")]
    fixture = r'''#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::fflush(stderr); std::_Exit(1); } } while (0)
typedef size_t SIZE_T;
constexpr uint64_t kPageShift = 12;
static std::vector<uint8_t> guest(4096 * 4096);   // (16 MB of "guest memory")
struct Host { void* guest_base() { return guest.data(); } uint64_t guest_size() { return guest.size(); } } host_obj, *host = &host_obj;
static std::atomic<bool> hold_guest{false};   // (copy_pages' guest copies wait while it is set: the helpers kept busy)
static std::atomic<int> held{0};
static bool guarded_copy(void* dst, const void* src, size_t n) {
    if (hold_guest.load()) {
        held++;
        while (hold_guest.load()) std::this_thread::yield();
    }
    std::memcpy(dst, src, n);
    return true;
}
static void copy_fence() {}   // (#1619's fence after a page-by-page retry: the stub's copies are plain memcpy)
static int GetCurrentThread() { return 0; }
static void SetThreadDescription(int, const wchar_t*) {}
''' + "\n".join(parts) + r'''
static void fill(uint8_t* p, uint32_t seed) { for (int i = 0; i < 4096; i += 4) { uint32_t v = seed * 2654435761u + i; std::memcpy(p + i, &v, 4); } }
int main() {
    for (uint32_t pg = 0; pg < 4096; pg++) fill(guest.data() + (uint64_t)pg * 4096, pg + 1);
    std::atomic<bool> stop{false};
    // (another thread keeps the helpers busy with captures of guest pages, as the replay thread's would)
    std::thread other([&] {
        std::vector<uint32_t> pages(3000);
        std::vector<uint8_t> out(pages.size() * 4096), ok(pages.size());
        for (int round = 0; !stop.load(); round++) {
            for (size_t i = 0; i < pages.size(); i++) pages[i] = (uint32_t)((i * 7 + round) % 4096);
            copy_pages(pages.data(), (int64_t)pages.size(), out.data(), ok.data());
            for (size_t i = 0; i < pages.size(); i++) {
                CHECK(ok[i]);
                CHECK(std::memcmp(out.data() + i * 4096, guest.data() + (uint64_t)pages[i] * 4096, 4096) == 0);
            }
        }
    });
    // (this thread: the shadow's pairs, of every size around the helpers' threshold, each checked as soon as its call returns)
    std::vector<uint8_t> src(2500 * 4096), dst(2500 * 4096);
    for (int round = 0; round < 200; round++) {
        static const int sizes[] = {1, 127, 128, 129, 500, 2500};
        const int n = sizes[round % 6];
        for (int i = 0; i < n; i++) fill(src.data() + (uint64_t)i * 4096, (uint32_t)(round * 10007 + i));
        std::memset(dst.data(), 0, (size_t)n * 4096);
        std::vector<CopyPair> pairs;
        for (int i = 0; i < n; i++) pairs.push_back({dst.data() + (uint64_t)(n - 1 - i) * 4096, src.data() + (uint64_t)i * 4096});
        copy_pairs(pairs.data(), (int64_t)pairs.size());
        for (int i = 0; i < n; i++) CHECK(std::memcmp(dst.data() + (uint64_t)(n - 1 - i) * 4096, src.data() + (uint64_t)i * 4096, 4096) == 0);
    }
    stop = true;
    other.join();
    {   // (every helper (RX_VK_CAPTURE_THREADS=2) and the other caller held in copy_pages' guest copies: copy_pairs runs its
        // own parts and returns)
        hold_guest = true;
        std::vector<uint32_t> pages(3000);
        for (size_t i = 0; i < pages.size(); i++) pages[i] = (uint32_t)i;
        std::vector<uint8_t> out(pages.size() * 4096), ok(pages.size());
        std::thread held_caller([&] { copy_pages(pages.data(), (int64_t)pages.size(), out.data(), ok.data()); });
        while (held.load() < 3) std::this_thread::yield();
        const int n = 2500;
        for (int i = 0; i < n; i++) fill(src.data() + (uint64_t)i * 4096, (uint32_t)(77 + i));
        std::vector<CopyPair> pairs;
        for (int i = 0; i < n; i++) pairs.push_back({dst.data() + (uint64_t)i * 4096, src.data() + (uint64_t)i * 4096});
        copy_pairs(pairs.data(), n);
        CHECK(std::memcmp(dst.data(), src.data(), (size_t)n * 4096) == 0);
        hold_guest = false;
        held_caller.join();
        for (size_t i = 0; i < pages.size(); i++) CHECK(ok[i] && std::memcmp(out.data() + i * 4096, guest.data() + i * 4096, 4096) == 0);
    }
    std::printf("PASS: copy_pairs and copy_pages share the helpers; each call returns with its own copies made, "
                "its own parts run by itself when the helpers are busy\n");
    std::fflush(stdout);
    // (the helpers are detached and wait on copy_helpers' condition variable for good: running its destructor at exit would
    // wait for them, as the game never does)
    std::_Exit(0);
}
'''
    with tempfile.TemporaryDirectory(prefix="capture-copy-helpers-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / ("fixture.exe" if os.name == "nt" else "fixture")
        cpp.write_text(fixture, encoding="utf-8")
        # (_CRT_SECURE_NO_WARNINGS: the MSVC CRT's getenv deprecation, as the game's own Windows build passes)
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-pthread", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        "-include", str(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h"), str(cpp), "-o", str(exe)], check=True)   # (env_on: RX_VK_LOW_SPEC)
        subprocess.run([str(exe)], check=True, timeout=120, env={**os.environ, "RX_VK_CAPTURE_THREADS": "2"})


if __name__ == "__main__":
    main()
