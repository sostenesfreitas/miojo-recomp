"""A colour pass that tests depth EQUAL gets a margin towards the camera on Intel.

The game draws a depth prepass, then colour passes that test EQUAL against it with another program. Intel's compiler
does not give the two programs the same position to the last bit, so most ground pixels failed the test and the sky
showed through (issue 17, Arc B390). The test becomes LESS_OR_EQUAL with a depth bias towards the camera, only where depth
is tested, not written, and the game asked for EQUAL. RX_VK_EQ_BIAS=<units> chooses it on any GPU, 0 turns it off.
python -I -B tests/test_equal_depth_margin.py
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def main():
    source = SOURCE.read_text(encoding="utf-8")
    state = block(source, "void dynamic_state(VkCommandBuffer cmd")
    if 'getenv("RX_VK_EQ_BIAS")' not in state:
        raise SystemExit("no RX_VK_EQ_BIAS switch")
    if "dev.props.vendorID == 0x8086 ? 64.0f : 0.0f" not in state:
        raise SystemExit("the margin is not on by default on Intel only")
    cond = "if (eq_bias > 0 && d.dt && !d.dw && d.dop == VK_COMPARE_OP_EQUAL)"
    if cond not in state:
        raise SystemExit("the margin is not limited to EQUAL tests that do not write depth")
    margin = block(state, cond)
    for line in ("d.dop = VK_COMPARE_OP_LESS_OR_EQUAL;", "d.bias_en = VK_TRUE;", "d.bias[0] = -eq_bias"):
        if line not in margin:
            raise SystemExit("the margin lacks %r" % line)
    if not state.index("d.dop = compare_op(s.ds.df);") < state.index(cond):
        raise SystemExit("the margin must come after the game's compare op is read")
    print("EQUAL depth margin ok")


if __name__ == "__main__":
    main()
