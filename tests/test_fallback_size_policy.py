"""Compile and run which mesh fallback compute stages are not given to RADV on GCN 1-4 (gpu/vk/fallback_size_policy.h; no game/GPU)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The renderer still notes each stage's phis, tells RADV's GCN cards apart, and asks the policy before a fallback build"""
    needles = {
        "gpu/vk/vk_progcache.cpp": ['#include "fallback_size_policy.h"',
                                    "p.stages[k].phis = fallback_size::spirv_phis(spirv[k].data(), spirv[k].size());",
                                    "st.phis = fallback_size::spirv_phis(spirv.data(), spirv.size());"],
        "gpu/vk/vk_main.cpp": ["dev.radv_gcn = drv.driverID == VK_DRIVER_ID_MESA_RADV && !f11.storageInputOutput16;"],
        "gpu/vk/vk_draw.cpp": ['#include "fallback_size_policy.h"',
                               "fallback_size::too_big(dev.radv_gcn, st.phis, phi_limit)", "RX_VK_FALLBACK_PHI_LIMIT",
                               "fallback_size::parse_limit(e, limit)"],
        "gpu/vk/vk_main.cpp#log": ['RADV on GCN 1-4: %s', 'dev.radv_gcn ? "yes" : "no"'],
    }
    for path, wanted in needles.items():
        src = (ROOT / path.split("#")[0]).read_text(encoding="utf-8")
        for needle in wanted:
            if needle not in src:
                raise SystemExit(path + " no longer has: " + needle)
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    fn = draw[draw.index("bool fallback_compute_pipelines(Program& p) {"):]
    if fn.index("fallback_size::too_big") > fn.index("cached_compute(&ci, &made[i])"):
        raise SystemExit("vk_draw.cpp: the size check must come before any fallback compute pipeline is built")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="fallback-size-") as tmp:
        exe = Path(tmp) / "fallback-size"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_fallback_size_policy.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
