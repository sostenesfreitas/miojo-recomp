"""The programs the driver's compiler faulted on stay out of the driver (gpu/vk/driver_faults.h, issue 996).

On AMD's Windows driver a fault caught inside vkCreateGraphicsPipelines left that program's compile unfinished in the driver.
Every later build of the program then waited inside the driver forever, and the game froze when closing the pause menu. This
test compiles the real driver_faults.h and runs it:
- the file's lines are kept for this vendor and driver version only;
- a faulted program is refused from then on;
- a second build of a program waits outside the driver while the first one runs, and is refused when that one faulted;
- a build that must not wait (a cold probe) is told the program is busy instead.
It also checks that vk_draw.cpp's build_graphics puts the gate around guarded_graphics, and that both guards answer a fault
with their own result (kDriverFault), not with one the driver could return itself.

python -I -B tests/test_driver_faults.py [--cxx g++]
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include "driver_faults.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

using driver_faults::Quarantine;
using namespace std::chrono_literals;

static void file_lines() {
    const uint32_t amd = 0x1002, drv = 0x800161;
    std::string text = "# a comment\n";
    text += driver_faults::line(amd, drv, 0x6eaa646d07ea4e43ull);
    text += driver_faults::line(amd, drv, 0x5fa56485a6b07619ull);
    text += driver_faults::line(amd, 0x800170, 0x4e5ce183a33b0855ull);   // (another driver version: tried again)
    text += driver_faults::line(0x10de, drv, 0x1111111111111111ull);     // (another vendor)
    text += "1002 00800161\n";                                           // (malformed: no key)
    text += "1002 00800161 0000000000000000\n";                          // (no program has key 0)
    text += "1002 00800161 2222222222222222 trailing\n";                 // (malformed: more than three fields)
    text += "1002 00800161 3333333333333333";                            // (the last line without its newline)
    const auto keys = driver_faults::parse(text, amd, drv);
    assert(keys.size() == 3);
    assert(keys.count(0x6eaa646d07ea4e43ull) && keys.count(0x5fa56485a6b07619ull) && keys.count(0x3333333333333333ull));
    assert(driver_faults::line(amd, drv, 0x6eaa646d07ea4e43ull) == "1002 00800161 6eaa646d07ea4e43\n");
    assert(driver_faults::parse("", amd, drv).empty());
}

static void refused_after_fault() {
    Quarantine q;
    q.load({0xAAull});
    assert(q.contains(0xAA) && q.begin(0xAA) == Quarantine::kFaulted);   // (an earlier run's fault)
    assert(q.begin(0xBB) == Quarantine::kGo);
    assert(!q.end(0xBB, false));                                          // (built: not quarantined)
    assert(q.begin(0xBB) == Quarantine::kGo);
    assert(q.end(0xBB, true));                                            // (faulted: new)
    assert(q.contains(0xBB) && q.begin(0xBB) == Quarantine::kFaulted && q.begin(0xBB, false) == Quarantine::kFaulted);
}

/// A second build of a program waits while the first is in the driver, and learns how it ended
static void second_build_waits(bool faulted) {
    Quarantine q;
    assert(q.begin(0xCC) == Quarantine::kGo);
    assert(q.begin(0xCC, false) == Quarantine::kBusy);   // (a cold probe does not wait)
    assert(q.begin(0xDD, false) == Quarantine::kGo);     // (another program is not held up)
    q.end(0xDD, false);
    std::atomic<int> got{-1};
    std::thread second([&] { got = q.begin(0xCC); });
    std::this_thread::sleep_for(100ms);
    assert(got == -1);   // (still waiting: the first build is in the driver)
    q.end(0xCC, faulted);
    second.join();
    assert(got == (faulted ? Quarantine::kFaulted : Quarantine::kGo));
    if (!faulted) q.end(0xCC, false);
    assert(q.begin(0xCC, false) == (faulted ? Quarantine::kFaulted : Quarantine::kGo));
}

/// Many threads build the same program while one of them faults: none is left waiting, none goes to the driver afterwards
static void many_builders() {
    Quarantine q;
    std::atomic<int> in_driver{0}, most{0}, went{0}, refused{0};
    std::vector<std::thread> t;
    for (int i = 0; i < 8; i++)
        t.emplace_back([&, i] {
            if (q.begin(0xEE) != Quarantine::kGo) return void(refused++);
            const int now = ++in_driver;
            for (int m = most; now > m && !most.compare_exchange_weak(m, now);) {}
            std::this_thread::sleep_for(5ms);
            --in_driver;
            went++;
            q.end(0xEE, i == 3 || went == 4);   // (one of the first few faults)
        });
    for (auto& x : t) x.join();
    assert(most == 1);   // (one build of the program in the driver at a time)
    assert(went >= 1 && went + refused == 8 && refused >= 4 && q.contains(0xEE));
}

/// Not serial (a driver other than AMD's): builds of one program go to the driver side by side, never waiting nor busy, and a
/// fault still keeps the program out afterwards
static void not_serial() {
    Quarantine q;
    q.set_serial(false);
    assert(!q.serial());
    assert(q.begin(0xF1) == Quarantine::kGo && q.begin(0xF1) == Quarantine::kGo && q.begin(0xF1, false) == Quarantine::kGo);
    std::atomic<int> in_driver{0}, most{0};
    std::vector<std::thread> t;
    for (int i = 0; i < 4; i++)
        t.emplace_back([&] {
            assert(q.begin(0xF2) == Quarantine::kGo);
            const int now = ++in_driver;
            for (int m = most; now > m && !most.compare_exchange_weak(m, now);) {}
            std::this_thread::sleep_for(50ms);
            --in_driver;
            q.end(0xF2, false);
        });
    for (auto& x : t) x.join();
    assert(most > 1);   // (side by side, as before the quarantine)
    assert(q.end(0xF1, true) && !q.end(0xF1, true));   // (the fault is new once)
    assert(q.begin(0xF1) == Quarantine::kFaulted && q.begin(0xF1, false) == Quarantine::kFaulted);
}

int main() {
    file_lines();
    refused_after_fault();
    second_build_waits(true);
    second_build_waits(false);
    many_builders();
    not_serial();
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    assert '#include "driver_faults.h"' in draw
    # (the four guards, Windows and Linux, graphics and compute, answer a fault with -77, kDriverFault: a driver's own
    # VK_ERROR_UNKNOWN is not a fault)
    # (tests/test_precompile_compute_guard.py and tests/test_linux_driver_fault_guard.py compile the guards on their own: they
    # use nothing of driver_faults.h)
    guards = draw[draw.index("#ifdef _WIN32\n__declspec(noinline) VkResult guarded_graphics("):
                  draw.index("\n#endif\nVkPipeline build_graphics")]
    assert "VK_ERROR_UNKNOWN" not in guards
    assert guards.count("return (VkResult)-77;") == 4, "each of the four guards answers a fault with -77"
    assert "kDriverFault" not in guards and "driver_faults::" not in guards and "fault_quarantine" not in guards
    # (each fault branch empties the output first: a handle the driver wrote before it faulted is not a pipeline, and a caller
    # that destroys every non-null output on an error, as fallback_compute_pipelines does, would destroy it)
    for branch in re.split(r"\} __except \(1\) \{|if \(sigsetjmp\(fault_jmp, 1\) != 0\) \{", guards)[1:]:
        body = branch[:branch.index("return (VkResult)-77;")]
        assert "*pl = VK_NULL_HANDLE;" in body, "a fault branch leaves the driver's output as it was"
    assert "constexpr VkResult kDriverFault = (VkResult)-77;" in draw
    # (build_graphics: the gate before the driver, its end right after it, a fault noted in the file)
    build = draw[draw.index("VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, "
                            "VkPrimitiveTopology topo, int part, bool* cold) {"):]
    build = build[:build.index("\n}\n")]
    i_begin = build.find("fault_quarantine().begin(p.key, !cold)")
    i_end = build.find("fault_quarantine().end(p.key, r == kDriverFault)")
    # (the driver is reached through guarded_graphics, or graphics_with, which goes to it through the play caches, #1772)
    calls = [m.start() for m in re.finditer(r"\b(?:guarded_graphics|graphics_with)\(", build)]
    assert calls and 0 <= i_begin < calls[0] and calls[-1] < i_end, (i_begin, calls, i_end)   # (the AMD retry too, #1747)
    assert "driver_fault_note(p.key)" in build[i_end:]
    assert re.search(r"kBusy\) return \*cold = true, VK_NULL_HANDLE;", build), "a busy program answers a cold probe as cold"
    # (#1747's retry without the optimizer followed the guard's VK_ERROR_UNKNOWN, which a fault was then: it still does)
    if "recovered with optimization disabled" in build:
        assert "(r == VK_ERROR_UNKNOWN || r == kDriverFault)" in build
    # (the file's comment once: in append mode Windows' ftell says 0 until the first write, so the end is sought first)
    note = draw[draw.index("void driver_fault_note(uint64_t key) {"):]
    note = note[:note.index("\n}\n")]
    assert note.index("fseek(f, 0, SEEK_END);") < note.index("if (ftell(f) == 0)"), "the comment is written with every fault"
    # (a folder the game cannot write to is said once in the log, not passed over in silence)
    assert re.search(r"if \(!f\) \{[^}]*logf\(", note), "a file that cannot be written is not logged"
    # (one at a time on AMD, where the hang was seen, or with RX_VK_DRIVER_FAULTS=2; other drivers build side by side)
    assert "q.set_serial(driver_faults_mode() == 2 || dev.props.vendorID == 0x1002);" in draw
    assert 'std::clamp(atoi(getenv("RX_VK_DRIVER_FAULTS")), 0, 2)' in draw, "RX_VK_DRIVER_FAULTS is not read as 0, 1 or 2"

    with tempfile.TemporaryDirectory(prefix="driver-faults-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(source),
                        "-o", str(exe), "-pthread"], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: driver faults: a faulted program stays out of the driver, in this run and on this driver later; one build "
          "of a program in the driver at a time")


if __name__ == "__main__":
    main()
