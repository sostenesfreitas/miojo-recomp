"""Task and mesh programs keep the driver's optimizer on Mesa's RADV.

On AMD the task and mesh programs are built without the optimizer (PR 430: the RX 6600 losing the device in issues 461 and 484),
but on RADV the unoptimized task programs run long enough to reset the GPU while the save loads at the Great Deku Tree
(issue 1000). This checks that the rule leaves RADV out and that RX_VK_MESH_OPT=1 still turns the optimizer back on.

python -I -B tests/test_radv_mesh_optimizer.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def main():
    draw = DRAW.read_text(encoding="utf-8")
    start = draw.index("static const bool amd =")
    for needle, why in (
        ("dev.props.vendorID == 0x1002", "the rule is no longer limited to AMD"),
        ("dev.driver_id != VK_DRIVER_ID_MESA_RADV", "RADV builds task and mesh programs without the optimizer again"),
        ('"RX_VK_MESH_OPT"', "RX_VK_MESH_OPT=1 no longer builds them with the optimizer"),
        ("if (amd && !mesh_opt)", "the optimizer is not turned off by the AMD rule and its override"),
        ("VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT", "the rule no longer turns the optimizer off"),
    ):
        if needle not in draw[start:draw.index("told++", start)]:
            raise SystemExit(f"vk_draw.cpp: {why}")
    print("RADV task and mesh programs with the optimizer ok")


if __name__ == "__main__":
    main()
