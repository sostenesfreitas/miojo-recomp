"""The GPUs whose task + mesh draws take the compute fallback by default (gpu/vk/mesh_mode.h, gpu/vk/vk_main.cpp,
https://github.com/NotRexed/Hookshot/issues/996).

On AMD's Windows driver the native task + mesh path timed out the driver on Navi 22
(https://github.com/NotRexed/Hookshot/pull/1747) and on Navi 21: an RX 6800 XT lost the device within minutes at Hyrule
Castle's entrance, and played 25 minutes there with none through the fallback. An RX 6600 (Navi 23) lost the device on the
session's first task + mesh draws (reported in Mako by sharkiix). Navi 24 is held out: no report, and 4 GB cards where the
fallback's buffers cost the most. This compiles mesh_rule::task_mesh_fallback_default and checks that it names those chips'
device IDs, only for AMD, and no other; and reads vk_main.cpp to check that it is the default only on Windows and that
RX_VK_TASK_MESH_FALLBACK still decides over it.

python -I -B tests/test_task_mesh_fallback_devices.py
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
# (device IDs and models: the PCI ID database, pci-ids.ucw.cz, and libdrm's amdgpu.ids)
DEVICES = {
    0x73A1: "Navi 21 (Radeon Pro V620)",
    0x73A2: "Navi 21 (Radeon Pro W6900X)",
    0x73A3: "Navi 21 (Radeon Pro W6800)",
    0x73AB: "Navi 21 (Radeon Pro W6800X, W6800X Duo)",
    0x73AE: "Navi 21 (Radeon Pro V620 MxGPU)",
    0x73BF: "Navi 21 (RX 6800, 6800 XT, 6900 XT)",
    0x73AF: "Navi 21 (RX 6900 XT, XTXH)",
    0x73A5: "Navi 21 (RX 6950 XT)",
    0x73DF: "Navi 22 (RX 6700, 6700 XT, 6750 XT)",
    0x73E0: "Navi 23 (RX 6600M)",
    0x73E1: "Navi 23 (Radeon Pro W6600M)",
    0x73E3: "Navi 23 (Radeon Pro W6600)",
    0x73EF: "Navi 23 (RX 6650 XT)",
    0x73FF: "Navi 23 (RX 6600, 6600 XT, 6600 LE)",
}
# (the rule, mesh_mode.h: every ID of an included chip that pci-ids or amdgpu.ids name a card for, consumer, mobile and Pro.
# Not in the default: RDNA 1; the included chips' IDs that are no named card (USB 0x73A4, 0x73C4, 0x73E4; unnamed 0x73C3,
# Navi 22's SR-IOV 0x73CE; 0x73DA in neither list); all of Navi 24 (0x743F RX 6400 and 6500 XT, 0x7421 to 0x7424 RX 6300
# and Pro W6300 to W6500M), held until a Windows run; RDNA 3)
OTHERS = [0x731F, 0x7340, 0x73A4, 0x73C3, 0x73C4, 0x73CE, 0x73DA, 0x73E4, 0x743F, 0x7421, 0x7422, 0x7423, 0x7424, 0x73F0,
          0x744C, 0x7480]

RULE = r'''#include "mesh_mode.h"
#include <cstdio>
int main(int argc, char** argv) {
    // argv: vendor device pairs, in hex; prints 1 or 0 for each
    for (int i = 1; i + 1 < argc; i += 2) {
        unsigned vendor = 0, device = 0;
        std::sscanf(argv[i], "%x", &vendor);
        std::sscanf(argv[i + 1], "%x", &device);
        std::printf("%d\n", mesh_rule::task_mesh_fallback_default(vendor, device) ? 1 : 0);
    }
    return 0;
}
'''


def check_source():
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    at = main_cpp.index("bool task_mesh_fallback = false;")
    block = main_cpp[at:main_cpp.index("dev.task_mesh_fallback =", at)]
    default = re.search(r"#ifdef _WIN32\n(.*?)#endif\n", block, re.S)
    assert default, "the default is no longer Windows-only"
    line = re.search(r"task_mesh_fallback = (.*?);", default.group(1), re.S)
    assert line, "no default for task_mesh_fallback"
    assert line.group(1).strip() == "mesh_rule::task_mesh_fallback_default(dev.props.vendorID, dev.props.deviceID)", \
        "the Windows default does not come from mesh_mode.h's list: %s" % line.group(1)
    # (the environment variable is read after the default, so it decides for any GPU)
    override = block.index('getenv("RX_VK_TASK_MESH_FALLBACK")')
    assert override > default.end(), "RX_VK_TASK_MESH_FALLBACK is read before the default and is overwritten by it"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    cases = [(0x1002, d) for d in DEVICES] + [(0x1002, d) for d in OTHERS] + [(0x10DE, d) for d in DEVICES] + \
        [(0x8086, d) for d in DEVICES]
    with tempfile.TemporaryDirectory(prefix="task-mesh-devices-") as tmp:
        src = Path(tmp) / "task_mesh_devices.cpp"
        src.write_text(RULE, encoding="utf-8")
        exe = Path(tmp) / "task-mesh-devices"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(src),
                        "-o", str(exe)], check=True)
        argv = [str(exe)]
        for vendor, device in cases:
            argv += ["%x" % vendor, "%x" % device]
        out = subprocess.run(argv, check=True, timeout=60, capture_output=True, text=True).stdout.split()
    got = dict(zip(cases, (x == "1" for x in out)))
    for device, name in DEVICES.items():
        assert got[(0x1002, device)], "%s (0x%04x) is not in the default" % (name, device)
        assert not got[(0x10DE, device)] and not got[(0x8086, device)], "0x%04x is in the default for a maker other than AMD" % device
    extra = [hex(d) for d in OTHERS if got[(0x1002, d)]]
    assert not extra, "unexpected devices in the default: %s" % extra
    print("PASS: task + mesh fallback by default on Windows for AMD's %s" % ", ".join(DEVICES.values()))


if __name__ == "__main__":
    main()
