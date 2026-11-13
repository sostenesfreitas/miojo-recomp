// RX_VK_RECORDER=thread at process exit, on Windows (tests/test_recorder_sync.py builds and runs it; skipped elsewhere).
//
// Built twice: with RX_EXIT_DLL, a DLL holding the real vk_work.h / vk_recorder.h (as hookshot_vk.dll does) and a fake vkCmdDraw;
// without, an exe that loads it, has the recorder parked in idle() holding sleep_mu (the test hook RX_REC_TEST_HOOKS), and calls
// ExitProcess, as the window's close (host/display.c) and svcExitProcess do. Windows ends the recorder thread first, then runs the
// DLL's global destructors. A destructor that takes sleep_mu, now owned by a dead thread, either hangs (MinGW-w64 GCC and
// clang with libstdc++; measured on Windows 11) or has the process ended on the spot, the DLL's other destructors skipped
// (llvm-mingw's libc++, clang for the MSVC target). So: the exe must end with code 0 within the watchdog's 20 s (it ends only
// this child), and the sentinel below, destroyed after the recorder's global, must print its line.
#ifdef RX_EXIT_DLL
#define RX_REC_TEST_HOOKS 1
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

extern "C" __declspec(dllimport) void* __stdcall GetStdHandle(unsigned long);
extern "C" __declspec(dllimport) int __stdcall WriteFile(void*, const void*, unsigned long, unsigned long*, void*);
/// (made before the recorder's global, so destroyed after it: its line shows the DLL's destructors all ran. The recorder's
/// global is an inline variable, whose dynamic initialization is only partially ordered against this file's statics: with
/// llvm-mingw it was made first, so this line printed before the old destructor ended the process and the broken variant
/// passed. init_priority makes this object first on every compiler that builds the test)
struct Sentinel {
    ~Sentinel() {
        static const char line[] = "DLL detach: the global destructors ran to the end\n";
        unsigned long n;
        WriteFile(GetStdHandle((unsigned long)-11), line, sizeof line - 1, &n, nullptr);
    }
};
static Sentinel g_sentinel __attribute__((init_priority(101)));

#include "vk_work.h"

namespace mkwvk {
std::atomic<uint64_t> work_epoch{0};
std::atomic<bool> par_active{false};
void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stdout, fmt, a);
    va_end(a);
    std::fputc('\n', stdout);
}
} // namespace mkwvk

static std::atomic<int> g_draws{0};
static std::atomic<bool> g_parked{false};
static void VKAPI_CALL fake_vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) { g_draws++; }
namespace vkx {
PFN_vkCmdDraw vkCmdDraw = fake_vkCmdDraw;
} // namespace vkx

/// The recorder started by a packet, run, then parked for good inside idle() with sleep_mu held; 1 once it is
extern "C" __declspec(dllexport) int park_recorder() {
    using namespace mkwvk;
    rec::g_stats_log = false;
    rec::g_test_idle_locked = [] {
        g_parked = true;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    };
    rec::set_mode(rec::THREAD);
    static rec::DrainingMutex mu;
    {
        std::lock_guard lk(mu);
        vkCmdDraw((VkCommandBuffer)(uintptr_t)0x100, 3, 1, 0, 0);
    }   // (the unlock: the draw has run)
    for (int i = 0; i < 5000 && !g_parked; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return g_parked && g_draws == 1 && rec::g_rec.th.joinable();
}
#else
#include <cstdio>
extern "C" {
__declspec(dllimport) void* __stdcall LoadLibraryA(const char*);
__declspec(dllimport) void* __stdcall GetProcAddress(void*, const char*);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    void* m = LoadLibraryA(argv[1]);
    auto park = m ? reinterpret_cast<int (*)()>(GetProcAddress(m, "park_recorder")) : nullptr;
    if (!park) {
        std::printf("no fixture DLL\n");
        return 2;
    }
    const int ok = park();
    std::printf("recorder parked in idle() holding sleep_mu: %d; ExitProcess\n", ok);
    std::fflush(stdout);
    ExitProcess(ok ? 0 : 2);
}
#endif
