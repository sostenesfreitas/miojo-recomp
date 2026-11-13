"""Execute submit_recording's value -> recording bookkeeping: a submission recorded in parts maps to its last part.

A submission longer than RX_VK_CHUNK draws (default 120) is recorded in parts (maybe_split), each part's recording
carrying the submission's value. wait_recorded waits for the recording a value maps to: the map kept the first part's
(emplace), so a wait for the fence, or the exposure meter's readback wait, came back after the first ~120 draws, and
the game wrote over memory the later parts still read (one-frame HUD and lighting garbage on AMD iGPUs, issue 997).
The statements are cut from gpu/vk/vk_replay.cpp and compiled with the containers they use. No GPU or game.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>
struct Recording { uint64_t value = 0; };
std::mutex rec_map_mu;
std::map<uint64_t, uint64_t> value_rec;
std::vector<uint64_t> rec_open_values;
static void note(Recording& r, uint64_t seq) {
    /* BOOKKEEPING */
}
static int failures;
static void expect(uint64_t value, uint64_t seq, const char* what) {
    auto it = value_rec.find(value);
    const uint64_t got = it == value_rec.end() ? 0 : it->second;
    std::printf("%s  %s: value %llu -> recording %llu (expected %llu)\n", got == seq ? "ok  " : "FAIL", what,
                (unsigned long long)value, (unsigned long long)got, (unsigned long long)seq);
    failures += got != seq;
}
int main() {
    // submission 10 in three parts: two split off (maybe_split), the last finishes it (finish_recording)
    Recording a{10}, b{10}, c{10};
    note(a, 1);
    expect(10, 1, "first part submitted: the part the GPU has");
    note(b, 2);
    rec_open_values.push_back(10);
    note(c, 3);
    expect(10, 3, "three parts: the last part's recording");
    if (!rec_open_values.empty()) std::printf("FAIL  the open values were not cleared\n"), failures++;
    // submission 11 stopped at an event wait, finished in a later recording made for submission 12
    Recording d{11}, e{12};
    note(d, 4);
    rec_open_values.push_back(11);
    rec_open_values.push_back(12);
    note(e, 5);
    expect(11, 5, "stopped submission finished in a later recording");
    expect(12, 5, "the later recording's own submission");
    // an older recording never lowers a value's recording
    Recording f{12};
    note(f, 2);
    expect(12, 5, "an older seq leaves the later recording");
    if (failures) return 1;
    std::printf("PASS: a submission recorded in parts maps to its last part's recording\n");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    submit = source[source.index("void submit_recording("):]
    start = submit.index("std::lock_guard mk(rec_map_mu);")
    end_marker = "rec_open_values.clear();"
    end = submit.index(end_marker, start) + len(end_marker)
    assert end < submit.index("si.signalSemaphoreInfoCount"), "the bookkeeping must be in submit_recording's rec_timeline block"
    fixture = FIXTURE.replace("/* BOOKKEEPING */", submit[start:end])
    with tempfile.TemporaryDirectory(prefix="value-rec-") as temp:
        src = Path(temp) / "value_rec.cpp"
        exe = Path(temp) / "value_rec.exe"   # (the name llvm-mingw writes on Windows; any name runs on Linux)
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], text=True, capture_output=True, timeout=20)
        print(r.stdout, end="")
        raise SystemExit(r.returncode)


if __name__ == "__main__":
    main()
