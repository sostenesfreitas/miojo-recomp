"""Game-free: the System page's leader lines (nx2/prompt_assets.py). A line moved off its Switch button takes the game's own
line out whole: game_line finds the shape the game drew from that label, not the router's idea of it (that left pieces of
the game's ZL, Minus and Plus lines beside the moved ones). And a line ends on its part's edge, as the game's own do (2
pixels into it; on the point of a part with no size, the shoulders), on every face: 13 pixels short of it, the lines
stopped beside the small buttons (the PlayStation 5's Create and Options).

python -I -B tests/test_prompt_guide_lines.py
"""
import math
import sys
from pathlib import Path

RUN_ALL_MODULES = ["PIL", "numpy"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets as P  # noqa: E402
import prompt_faces  # noqa: E402

SIZE = (1555, 1511)   # (ControllerGuide_Pro_00 and _01)


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def game_lines():
    """two lines from the left bar (one with a bend and a dot), the bar joining them, as the game draws them"""
    from PIL import Image, ImageDraw
    im = Image.new("L", SIZE, 0)
    d = ImageDraw.Draw(im)
    x0 = P.BARS[0]
    d.rectangle((x0 - 7, 300, x0 + 1, 700), fill=255)   # (the bar: the game's, 140 to 148, ends at BARS[0] + 1)
    d.line((x0, 333, 450, 333, 450, 500), fill=255, width=6)
    d.ellipse((440, 490, 460, 510), fill=255)
    d.line((x0, 667, 600, 667), fill=255, width=6)
    return im


def main():
    import numpy as np
    from PIL import Image, ImageDraw
    im = game_lines()
    first = np.asarray(P.game_line(im, 0, 333)) > 0
    check(first[333, 300] and first[450, 450] and first[500, 450] and first[505, 450], "game_line: the line from the label "
          "at row 333 is not found whole (its leg, its bend, its dot)")
    check(not first[667, 400], "game_line: the line from row 333 takes in the other line from the same bar")
    check(not first[500, P.BARS[0]], "game_line: the label's bar is taken out with its line")
    second = np.asarray(P.game_line(im, 0, 667)) > 0
    check(second[667, 500] and not second[333, 300], "game_line: the line from row 667 is not the one found")
    check(not np.asarray(P.game_line(Image.new("L", SIZE, 0), 0, 333)).any(), "game_line: a line found where none is drawn")

    stays = Image.new("L", SIZE, 0)
    ends = 0
    for face in ["switch"] + list(prompt_faces.FACES):
        where = P.parts(face)
        for bar, row, pads in ((0, 333, ("ltrigger", "lshoulder", "back")), (1, 558, ("rtrigger", "rshoulder", "start"))):
            for pad in pads:
                way = P.guide_route(SIZE, bar, row, where, pad, stays)
                check(way, f"{face}: no line from bar {bar} to {pad}")
                x, y, r = P.guide_at(where[pad])
                off = math.dist(way[-1], (x, y))
                check(abs(off - max(r - 2, 0)) < 1, f"{face}: the line to {pad} ends {off:.1f} pixels from its centre, its "
                      f"edge is {r:.1f} (want {max(r - 2, 0):.1f})")
                ends += 1
    print(f"ok: game_line finds one line whole; {ends} lines on {len(prompt_faces.FACES) + 1} faces end on their part's edge")


if __name__ == "__main__":
    main()
