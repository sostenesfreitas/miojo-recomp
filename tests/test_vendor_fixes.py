"""Which GPUs get which vendor workaround (gpu/vk/vk_vendor.h), on made-up devices, and where the renderer uses them.

vendor_fixes() is compiled with the Vulkan headers and run on the properties of GPUs players have: NVIDIA's driver on
Windows and Linux (and NVK on the same cards), AMD's Windows driver in its two branches (RDNA 1/2: "AMD proprietary
shader compiler"; RDNA 3/4: LLPC), RADV, AMDVLK, Intel's Windows driver on an integrated GPU and on an Arc card, ANV,
llvmpipe. Each fix must be on for the GPUs it is for and off for every other, and NVIDIA's driver must get none: that is
what keeps the renderer and the translated shaders on NVIDIA as they were. The RX_VK_ switches force each one either way.
Then the source: the device's fixes are made from its properties at device creation, and the translator is told
fetch_bounds, nan_bits and the gather offset (and the program cache's stamp tells the three apart).

    python -I -B tests/test_vendor_fixes.py --headers <Vulkan-Headers/include> [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

# name, vendor, driver id, device type, driverInfo -> fetch_bounds, nan_bits, gather_offset
DEVICES = [
    ("RTX 3090, NVIDIA's Linux driver", 0x10DE, "NVIDIA_PROPRIETARY", "DISCRETE_GPU", "580.105.08", "000"),
    ("RTX 4070 laptop, NVIDIA's Windows driver", 0x10DE, "NVIDIA_PROPRIETARY", "DISCRETE_GPU", "581.57", "000"),
    ("GTX 1650, NVK", 0x10DE, "MESA_NVK", "DISCRETE_GPU", "Mesa 26.0.1", "000"),
    ("RX 6700 XT, AMD's Windows driver (RDNA 2 branch)", 0x1002, "AMD_PROPRIETARY", "DISCRETE_GPU",
     "26.10.1 (AMD proprietary shader compiler)", "111"),
    ("RX 5700, AMD's Windows driver (RDNA 1 branch)", 0x1002, "AMD_PROPRIETARY", "DISCRETE_GPU",
     "25.9.1 (AMD proprietary shader compiler)", "111"),
    ("RX 9070 XT, AMD's Windows driver (LLPC)", 0x1002, "AMD_PROPRIETARY", "DISCRETE_GPU", "26.10.1 (LLPC)", "101"),
    ("RX 7600, AMD's Windows driver, no driverInfo", 0x1002, "AMD_PROPRIETARY", "DISCRETE_GPU", "", "101"),
    ("AMD's driver naming another shader compiler", 0x1002, "AMD_PROPRIETARY", "DISCRETE_GPU", "26.10.1 (LLPC shader compiler)",
     "101"),
    ("RX 580, RADV", 0x1002, "MESA_RADV", "DISCRETE_GPU", "Mesa 26.2.5", "101"),
    ("Steam Deck, RADV", 0x1002, "MESA_RADV", "INTEGRATED_GPU", "Mesa 25.1.0", "101"),
    ("RX 6800, AMDVLK", 0x1002, "AMD_OPEN_SOURCE", "DISCRETE_GPU", "2025.Q2.1 (LLPC)", "101"),
    ("UHD Graphics, Intel's Windows driver", 0x8086, "INTEL_PROPRIETARY_WINDOWS", "INTEGRATED_GPU", "101.7088", "101"),
    ("Iris Xe, Intel's Windows driver", 0x8086, "INTEL_PROPRIETARY_WINDOWS", "INTEGRATED_GPU", "101.7092", "101"),
    ("Arc B580, Intel's Windows driver", 0x8086, "INTEL_PROPRIETARY_WINDOWS", "DISCRETE_GPU", "101.8132", "101"),
    ("Iris Xe, ANV", 0x8086, "INTEL_OPEN_SOURCE_MESA", "INTEGRATED_GPU", "Mesa 26.0.1", "101"),
    ("llvmpipe", 0x10005, "MESA_LLVMPIPE", "CPU", "Mesa 26.0.1 (LLVM 20.1.8)", "100"),
]
FIELDS = ['fetch_bounds', 'nan_bits', 'gather_offset']

FIXTURE = r"""
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "vk_vendor.h"
using namespace mkwvk;
static std::map<std::string, std::string> env;
static const char* get(const char* name) {
    auto it = env.find(name);
    return it == env.end() ? nullptr : it->second.c_str();
}
static void print(const char* what, const VendorFixes& x) {
    std::printf("%s %d%d%d\n", what, x.fetch_bounds, x.nan_bits, x.gather_offset);
}
int main() {
    struct D { const char* name; uint32_t vendor; VkDriverId driver; VkPhysicalDeviceType type; const char* info; };
    const D devices[] = {
@@DEVICES@@
    };
    for (const D& d : devices) {
        VendorFacts f;
        f.vendor_id = d.vendor, f.driver_id = d.driver, f.device_type = d.type, f.driver_info = d.info;
        VendorFixes x = vendor_fixes(f);
        print(d.name, x);
        vendor_fixes_env(x, get);   // (no switch set: as it was)
        print(d.name, x);
        const char* names[] = {"RX_VK_FETCH_BOUNDS", "RX_VK_NAN_BITS", "RX_VK_GATHER_OFFSET"};
        for (const char* n : names)
            for (const char* v : {"0", "1", ""}) {
                env.clear();
                env[n] = v;
                VendorFixes y = vendor_fixes(f);
                vendor_fixes_env(y, get);
                std::printf("%s=%s ", n, v);
                print(d.name, y);
            }
        env.clear();
    }
    return 0;
}
"""


def cut(text, marker):
    at = text.index(marker)
    end = text.index("{", at) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[at:end]


def check_source():
    main = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    at = main.index("dev.fixes = vendor_fixes(facts);")
    for text in ("vendor_fixes_env(dev.fixes,",):
        if main.find(text, at) < 0:
            raise SystemExit("FAIL: vk_main.cpp: %s not after the fixes are made" % text)
    if main.index("dev.fixes = vendor_fixes(facts);") > main.index("vkCreateDevice(dev.physical, &ci"):
        raise SystemExit("FAIL: the fixes are made after the device")
    for field in ("vendor_id = p2.properties.vendorID", "driver_id = drv.driverID", "device_type = p2.properties.deviceType",
                  "driver_info = drv.driverInfo"):
        if "facts." + field not in main:
            raise SystemExit("FAIL: vk_main.cpp does not fill facts.%s" % field)
    coop = (VK / "vk_coopmat.cpp").read_text(encoding="utf-8")
    for text in ("Shader::Backend::SPIRV::fetch_bounds = dev.fixes.fetch_bounds;", "Shader::Backend::SPIRV::nan_bits = dev.fixes.nan_bits;",
                 "Shader::Backend::SPIRV::fetch_bounds_buffers = dev.fixes.fetch_bounds && !dev.features.robustBufferAccess;"):
        if text not in cut(coop, "void coopmat_init() {"):
            raise SystemExit("FAIL: coopmat_init does not set %s" % text)
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    if "p.need_gather_subpixel_offset = dev.fixes.gather_offset;" not in cut(shader, "Shader::Profile make_profile() {"):
        raise SystemExit("FAIL: make_profile does not set the gather offset")
    stamp = cut(shader, "uint64_t translator_stamp() {")
    for text in ("fetch_bounds ? 16u : 0u", "nan_bits ? 32u : 0u", "profile().need_gather_subpixel_offset ? 64u : 0u",
                 "fetch_bounds_buffers ? 128u : 0u"):
        if text not in stamp:
            raise SystemExit("FAIL: the program cache's stamp lacks %s" % text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    rows = ",\n".join('        {"%s", 0x%X, VK_DRIVER_ID_%s, VK_PHYSICAL_DEVICE_TYPE_%s, "%s"}' % (n, v, d, t, i)
                      for n, v, d, t, i, _ in DEVICES)
    with tempfile.TemporaryDirectory(prefix="vendor-fixes-") as tmp:
        tmp = Path(tmp)
        cpp, exe = tmp / "fixture.cpp", tmp / "fixture"
        cpp.write_text(FIXTURE.replace("@@DEVICES@@", rows), encoding="utf-8")
        r = subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Werror", "-I" + str(VK), "-I" + str(args.headers),
                            str(cpp), "-o", str(exe)], capture_output=True, text=True, timeout=300)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, check=True).stdout.splitlines()
    per_device = 2 + len(FIELDS) * 3
    if len(out) != per_device * len(DEVICES):
        raise SystemExit("FAIL: %d lines" % len(out))
    failures = []
    for k, (name, _, _, _, _, want) in enumerate(DEVICES):
        lines = out[k * per_device:(k + 1) * per_device]
        for line in lines[:2]:
            if line.split()[-1] != want:
                failures.append("%s: %s, expected %s" % (name, line.split()[-1], want))
        for i, line in enumerate(lines[2:]):
            switch, value = ["RX_VK_FETCH_BOUNDS", "RX_VK_NAN_BITS", "RX_VK_GATHER_OFFSET"][i // 3], ["0", "1", ""][i % 3]
            got = list(line.split()[-1])
            expect = list(want)
            if value:
                field = i // 3
                expect[field] = "1" if value == "1" else "0"
            if got != expect:
                failures.append("%s with %s=%s: %s, expected %s" % (name, switch, value, "".join(got), "".join(expect)))
    if failures:
        raise SystemExit("FAIL:\n  " + "\n  ".join(failures))
    print("ok: %d devices, each fix on only where it is for, none on NVIDIA's driver; every switch forces its fix both ways"
          % len(DEVICES))


if __name__ == "__main__":
    main()
