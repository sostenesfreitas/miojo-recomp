"""A submission cut into parts (gpu/vk/vk_replay.cpp maybe_split, RX_VK_CHUNK) writes the stream's uniform updates into
guest memory once, at its last part, as an unsplit submission does (finish_recording), on Intel without VRAM mirrors
(pools_mirrored, the iGPUs); elsewhere, or with RX_VK_SPLIT_UBFLUSH=1, each part's end writes those recorded so far, as
before. Source checks plus the decision compiled
alone against a table; no game or GPU required.

Written at each part's end, the draws of the later parts that read those ranges straight from guest memory saw a state an
unsplit submission never shows: on Intel, white blocks and flashes in Kokiri Forest and white save thumbnails, and a
whole-screen white flash every 8 frames on an Arc 140V (https://github.com/NotRexed/Hookshot/issues/999). flush_ub_updates writes the entries from
ub_first_of_recording on, so a part's end must neither write them nor move that index when it does not flush.

python -I -B tests/test_split_ub_flush.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    """From marker to the brace closing its first one"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


# RX_VK_SPLIT_UBFLUSH's value (None: unset), the GPU's vendor, whether its buffer pools have VRAM mirrors, and whether a
# part's end writes the updates
VENDORS = {"intel": 0x8086, "amd": 0x1002, "nvidia": 0x10DE}
CASES = ([(None, "intel", 0, 0), (None, "intel", 1, 1)] +
         [(None, g, m, 1) for g in ("amd", "nvidia") for m in (0, 1)] +
         [(v, g, m, 1) for v in ("1", "on", "yes") for g in VENDORS for m in (0, 1)] +
         [(v, g, m, 0) for v in ("0", "off", "false", "no", "") for g in VENDORS for m in (0, 1)])
FIXTURE = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct { struct { unsigned vendorID; } props; } dev;
static bool the_mirrored;       // (vk_memory.cpp mirror_on: the buffer pools have VRAM mirrors)
static bool pools_mirrored() { return the_mirrored; }
static const char* the_value;   // (the switch's value, nullptr: unset; read in place of the environment)
static const char* fake_getenv(const char* name) { return strcmp(name, "RX_VK_SPLIT_UBFLUSH") ? nullptr : the_value; }
static bool fake_env_on(const char* name) { const char* v = fake_getenv(name); return v && !env_value_off(v); }
static bool part_flush_for(const char* value, unsigned vendor, bool mirrored) {
    the_value = value;
    dev.props.vendorID = vendor;
    the_mirrored = mirrored;
    return @@DECISION@@();
}
int main(int argc, char** argv) {
    // (argv: value or -unset, the vendor, 1 with mirrors or 0, 1 written at each part's end or 0)
    for (int i = 1; i + 3 < argc; i += 4) {
        const char* v = strcmp(argv[i], "-unset") ? argv[i] : nullptr;
        const unsigned vendor = (unsigned)strtoul(argv[i + 1], nullptr, 0);
        const bool mirrored = atoi(argv[i + 2]) != 0;
        const bool got = part_flush_for(v, vendor, mirrored);
        if (got != (atoi(argv[i + 3]) != 0)) {
            printf("FAIL: RX_VK_SPLIT_UBFLUSH=[%s], vendor 0x%04x, %s: %s\n", v ? v : "(unset)", vendor,
                   mirrored ? "mirrors" : "no mirrors", got ? "written at each part's end" : "written at the last part");
            return 1;
        }
    }
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    args = p.parse_args()
    src = (VK / "vk_replay.cpp").read_text(encoding="utf-8")

    split = block(src, "void maybe_split(VkCommandBuffer& cmd) {")
    if "static const bool part_flush = [] {" not in split:
        raise SystemExit("FAIL: maybe_split writes the uniform updates at every part's end (no part_flush decision)")
    code = "\n".join(line.split("//")[0] for line in split.splitlines())
    if code.count("flush_ub_updates(") != 1 or "if (part_flush) flush_ub_updates(cmd);" not in code:
        raise SystemExit("FAIL: maybe_split writes the uniform updates at a part's end without asking part_flush")
    if code.count("ub_first_of_recording =") != 1 or \
            "if (part_flush) ub_first_of_recording = ub_updates.size();" not in code:
        raise SystemExit("FAIL: maybe_split moves ub_first_of_recording when the part's end did not write the updates: "
                         "the last part would leave the earlier parts' updates unwritten")
    finish = block(src, "void finish_recording(uint64_t value) {")
    if "flush_ub_updates(r.cmd);" not in finish:
        raise SystemExit("FAIL: finish_recording no longer writes the submission's uniform updates")
    flush = block(src, "void flush_ub_updates(VkCommandBuffer cmd) {")
    if "for (size_t i = ub_first_of_recording; i < ub_updates.size(); i++)" not in flush:
        raise SystemExit("FAIL: flush_ub_updates no longer writes from ub_first_of_recording on: check what a part's "
                         "end has to keep")
    print("source: a part's end writes the updates and moves the window only under part_flush; the last part writes "
          "them all")

    lam = split[split.index("static const bool part_flush = [] {") + len("static const bool part_flush = "):]
    lam = lam[:lam.index("}();") + 1]
    with tempfile.TemporaryDirectory(prefix="split-ubflush-") as tmp:
        tmp = Path(tmp)
        cpp, exe = tmp / "t.cpp", tmp / "t.exe"
        lam = lam.replace("getenv(", "fake_getenv(").replace("env_on(", "fake_env_on(")
        cpp.write_text(FIXTURE.replace("@@DECISION@@", "(" + lam + ")"), encoding="utf-8")
        r = subprocess.run([args.cxx, "-std=c++20", "-include", str(VK / "compat" / "hookshot_env.h"), "-Wall",
                            "-Wextra", "-Werror", str(cpp), "-o", str(exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("FAIL: the part_flush decision did not build alone")
        argv = []
        for value, gpu, mirrored, want in CASES:
            argv += ["-unset" if value is None else value, hex(VENDORS[gpu]), str(mirrored), str(want)]
        r = subprocess.run([str(exe), *argv], capture_output=True, text=True, timeout=30)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("FAIL: a split submission writes its uniform updates where it should not (Intel without "
                             "mirrors: at the last part; elsewhere at each part's end; RX_VK_SPLIT_UBFLUSH by the "
                             "switches' rule)")
    print("decision: %d cases: unset, Intel without mirrors at the last part, Intel with mirrors and others at each part's "
          "end; RX_VK_SPLIT_UBFLUSH by the switches' rule" % len(CASES))
    print("PASS")


if __name__ == "__main__":
    main()
