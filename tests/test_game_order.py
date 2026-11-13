"""RX_VK_GAME_ORDER (gpu/vk/vk_texture.cpp game_order, vk_draw.cpp dispatch, vk_texture.cpp pass_start_barrier).

The rules as written: on NVIDIA by default and off on other vendors, RX_VK_GAME_ORDER=0 / 1 over that; a direct dispatch right
after another on the same command buffer with no work recorded between (the work count unchanged since it) gets no barrier,
an indirect dispatch and a parallel replay's dispatches keep theirs; the dispatch notes the work count after its own command;
a pass right after a pass takes the fragments-only barrier with the switch, and RX_VK_PASS_BARRIER still decides that part.
python -I -B tests/test_game_order.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    checks = [
        ('const bool v = e ? !env_value_off(e) : dev.props.vendorID == 0x10de;' in tex, "NVIDIA by default, the switch over it"),
        ('const bool on = e ? !strcmp(e, "game") : game_order();' in tex, "RX_VK_PASS_BARRIER still sets the pass part"),
        ('const bool after_dispatch = !indirect && cmd == last_dispatch_cmd &&' in draw, "an indirect dispatch keeps its barrier"),
        ('work_epoch.load(std::memory_order_relaxed) == last_dispatch_epoch && game_order() &&' in draw,
         "only with nothing recorded since the last dispatch"),
        ('!par_active.load(std::memory_order_relaxed);' in draw.split("const bool after_dispatch")[1][:400],
         "not in a parallel replay"),
        ('if (work_since_barrier && !no_order && !after_dispatch) full_barrier(cmd, "barrier (dispatch)");' in draw,
         "every other dispatch keeps its barrier"),
        ('last_dispatch_cmd = cmd, last_dispatch_epoch = work_epoch.load(std::memory_order_relaxed);' in draw,
         "the count noted after the dispatch's own command"),
    ]
    bad = [what for ok, what in checks if not ok]
    if bad:
        raise SystemExit("FAIL: " + "; ".join(bad))
    # (the note comes after the dispatch commands, not before them)
    at = draw.index("last_dispatch_cmd = cmd, last_dispatch_epoch")
    if draw.rfind("vkCmdDispatch(cmd, x, y, z);", 0, at) < 0 or at - draw.rfind("vkCmdDispatch(cmd, x, y, z);", 0, at) > 200:
        raise SystemExit("FAIL: the work count is not noted right after vkCmdDispatch")
    print("PASS: back-to-back passes and dispatches ordered by the game's barriers, on NVIDIA by default")


if __name__ == "__main__":
    main()
