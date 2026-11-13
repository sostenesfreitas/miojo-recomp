"""The GPUs whose task + mesh draws take the compute fallback by default (gpu/vk/vk_main.cpp, issue 996).

On AMD's Windows driver the native task + mesh path timed out the driver on Navi 22 (#1747) and on Navi 21: an RX 6800 XT lost
the device within minutes at Hyrule Castle's entrance, and played 25 minutes there with none through the fallback. This checks
that both chips are in the default, only for AMD and only on Windows, and that RX_VK_TASK_MESH_FALLBACK still decides over it.

python -I -B tests/test_task_mesh_fallback_devices.py
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
DEVICES = {
    0x73BF: "Navi 21 (RX 6800, 6800 XT, 6900 XT)",
    0x73AF: "Navi 21 (RX 6900 XT, XTXH)",
    0x73A5: "Navi 21 (RX 6950 XT)",
    0x73DF: "Navi 22 (RX 6700 XT)",
}


def main():
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    at = main_cpp.index("bool task_mesh_fallback = false;")
    block = main_cpp[at:main_cpp.index("dev.task_mesh_fallback =", at)]
    default = re.search(r"#ifdef _WIN32\n(.*?)#endif\n", block, re.S)
    assert default, "the default is no longer Windows-only"
    line = re.search(r"task_mesh_fallback = (.*?);", default.group(1), re.S)
    assert line, "no default for task_mesh_fallback"
    cond = line.group(1)
    assert "dev.props.vendorID == 0x1002" in cond, "the default is not limited to AMD"
    ids = {int(x, 16) for x in re.findall(r"dev\.props\.deviceID == (0x[0-9a-fA-F]+)", cond)}
    for device, name in DEVICES.items():
        assert device in ids, "%s (0x%04x) is not in the default" % (name, device)
    assert ids == set(DEVICES), "unexpected devices in the default: %s" % sorted(hex(i) for i in ids - set(DEVICES))
    # (the environment variable is read after the default, so it decides for any GPU)
    override = block.index('getenv("RX_VK_TASK_MESH_FALLBACK")')
    assert override > default.end(), "RX_VK_TASK_MESH_FALLBACK is read before the default and is overwritten by it"
    print("PASS: task + mesh fallback by default on Windows for AMD's %s" % ", ".join(DEVICES.values()))


if __name__ == "__main__":
    main()
