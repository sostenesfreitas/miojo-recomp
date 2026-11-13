"""(hookshot) Each face's controller for the pause menu's controls page, drawn like the game's Pro Controller (Controller_Pro: flat
greys, a black outline), at the build from the user's RomFS: nx2/prompt_assets.py calls picture(). Its look is the drawing in each
face's entry of nx2/prompt_faces.py; a hand-made picture in nx2/prompt_faces/<face>/Controller_Pro.png takes its place.

The body is the Kenney font's controller glyph (third_party/kenney_input; the Steam Deck: deck() below, Kenney's is a shorter one),
filled, outlined and with its bumpers raised (they stop short of a touchpad); the sticks and the d-pad are the Pro Controller's own,
scaled; the face and small buttons blank discs with the face's labels (the Home button's the game's own icon). All of it drawn SS
times as large and scaled down: smooth edges.

    python -I nx2/prompt_draw.py --romfs <dir> --out <dir>   (each face's picture as <face>.png, to look at)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prompt_assets as P  # noqa: E402
from prompt_faces import FACES  # noqa: E402
import prompt_keys  # noqa: E402

BODY, PART, INK, EDGE = (51, 51, 51, 255), (100, 100, 100, 255), (205, 205, 205, 255), (0, 0, 0, 255)
# the Pro Controller's stick and d-pad (centre, half size in its picture's pixels) and the box the body is fitted in
STICK, DPAD = (347, 228, 74), (453, 348, 70)
BOX, DECK_BOX = (151, 43, 1009, 651), (70, 105, 1088, 573)
SS = 4   # (the drawing's scale before the picture's size)


def sprite(pro, cx, cy, r):
    return pro.crop((cx - r, cy - r, cx + r + 1, cy + r + 1))


def blank_disc(r, k=1):
    """a button of the Pro, without its label (its outline k times as thick: drawn k times as large)"""
    from PIL import Image, ImageDraw
    m = 3 * k
    s = Image.new("RGBA", (2 * r + 2 * m + 1, 2 * r + 2 * m + 1), (0, 0, 0, 0))
    ImageDraw.Draw(s).ellipse((m, m, 2 * r + m, 2 * r + m), fill=PART, outline=EDGE, width=m)
    return s


def grown(mask, d):
    """an L mask (0/255) grown by d pixels all round (a blur's foot: far quicker than a max filter that size)"""
    from PIL import ImageFilter
    return mask.filter(ImageFilter.GaussianBlur(d / 2)).point(lambda v: 255 if v > 6 else 0)


def silhouette(fname, glyph, h):
    """a Kenney glyph, filled (its holes too), as an L mask"""
    from fontTools.ttLib import TTFont
    from PIL import Image, ImageChops, ImageDraw, ImageFont
    path = os.path.join(P.KENNEY, fname)
    rev = {n: c for c, n in TTFont(path).getBestCmap().items()}
    im = Image.new("L", (h * 5, h * 4), 0)
    ImageDraw.Draw(im).text((h, h), chr(rev[glyph]), font=ImageFont.truetype(path, h * 2), fill=255)
    im = im.crop(im.getbbox()).point(lambda v: 255 if v > 127 else 0)
    out = Image.new("L", (im.width + 4, im.height + 4), 255)
    out.paste(ImageChops.invert(im), (2, 2))
    ImageDraw.floodfill(out, (0, 0), 0)
    return ImageChops.lighter(im, out.crop((2, 2, im.width + 2, im.height + 2)))


def deck(w):
    """the Steam Deck's body: 2.55 times as wide as high, the grips rounder below"""
    from PIL import Image, ImageDraw
    h = round(w / 2.55)
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle((0, 0, w - 1, int(h * .92)), radius=int(h * .16), fill=255)
    d.ellipse((int(w * .01), int(h * .30), int(w * .27), h - 1), fill=255)
    d.ellipse((int(w * .73), int(h * .30), int(w * .99), h - 1), fill=255)
    d.rectangle((int(w * .14), int(h * .5), int(w * .86), int(h * .92)), fill=255)
    return m


def picture(face, pro, game_icons):
    """the face's controller (RGBA, the size of pro: the game's Controller_Pro), or None for a face with no drawing"""
    from PIL import Image, ImageChops, ImageDraw, ImageFilter
    drawing = FACES.get(face, {}).get("drawing")
    if not drawing:
        return None
    fname, glyph, controls = drawing
    W, H = pro.size[0] * SS, pro.size[1] * SS
    stick = sprite(pro, *STICK)
    dpad = sprite(pro, *DPAD)
    cross = Image.new("L", dpad.size, 0)   # (the cross alone: the square crop takes in the left stick's ring)
    ImageDraw.Draw(cross).rectangle((0, 42, 140, 98), fill=255)
    ImageDraw.Draw(cross).rectangle((44, 0, 100, 140), fill=255)
    dpad.putalpha(ImageChops.multiply(dpad.getchannel("A"), cross))
    sil = silhouette(fname, glyph, 600 * SS) if fname else deck(1600 * SS)
    box = [v * SS for v in (BOX if fname else DECK_BOX)]
    s = min((box[2] - box[0]) / sil.width, (box[3] - box[1]) / sil.height) * 0.97
    sil = sil.resize((round(sil.width * s), round(sil.height * s)), Image.LANCZOS).point(lambda v: 255 if v > 127 else 0)
    ox, oy = (W - sil.width) // 2, box[1] + (box[3] - box[1] - sil.height) // 2
    mask = Image.new("L", (W, H), 0)
    mask.paste(sil, (ox, oy))
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if fname:   # (the bumpers: the body raised, an ellipse over each top corner)
        up = Image.new("L", (W, H), 0)
        up.paste(mask, (0, -18 * SS))
        top = Image.new("L", (W, H), 0)
        for c in (.25, .75):
            ImageDraw.Draw(top).ellipse((ox + int(sil.width * (c - .17)), oy - int(sil.height * .12), ox + int(sil.width * (c + .17)),
                                         oy + int(sil.height * .16)), fill=255)
        for kind, cx, _, size, _ in controls:
            if kind == "pad":   # (a touchpad's top edge is no bumper)
                ImageDraw.Draw(top).rectangle((ox + int(sil.width * (cx - size[0] / 2 - .03)), 0,
                                               ox + int(sil.width * (cx + size[0] / 2 + .03)), H), fill=0)
        bump = ImageChops.multiply(up, top).filter(ImageFilter.GaussianBlur(SS)).point(lambda v: 255 if v > 127 else 0)
        im.paste(EDGE, (0, 0), grown(bump, 3 * SS))
        im.paste(PART, (0, 0), bump)
    im.paste(EDGE, (0, 0), grown(mask, 3 * SS))
    im.paste(BODY, (0, 0), mask)
    icons = P.Icons(face, None)
    d = ImageDraw.Draw(im)
    for kind, cx, cy, size, what in controls:
        x, y = ox + cx * sil.width, oy + cy * sil.height
        if kind in ("stick", "dpad"):
            r = round(size * sil.width / 2)
            im.alpha_composite((stick if kind == "stick" else dpad).resize((2 * r + 1, 2 * r + 1), Image.LANCZOS),
                               (round(x) - r, round(y) - r))
        elif kind == "screen":
            w2, h2 = size[0] * sil.width / 2, size[1] * sil.height / 2
            d.rounded_rectangle((x - w2, y - h2, x + w2, y + h2), radius=8 * SS, fill=(27, 27, 27, 255), outline=EDGE, width=3 * SS)
            d.rectangle((x - w2 + 22 * SS, y - h2 + 22 * SS, x + w2 - 22 * SS, y + h2 - 22 * SS), fill=(77, 77, 77, 255),
                        outline=EDGE, width=2 * SS)
        elif kind == "pad":
            w2, h2 = size[0] * sil.width / 2, size[1] * sil.height / 2
            d.rounded_rectangle((x - w2, y - h2, x + w2, y + h2), radius=round(min(w2, h2) * .35), fill=PART, outline=EDGE,
                                width=3 * SS)
        else:   # ("face", "small": a disc with the face's label, the Home button the game's icon)
            r = round(size * sil.width / 2)
            im.alpha_composite(blank_disc(r, SS), (round(x) - r - 3 * SS, round(y) - r - 3 * SS))
            ic = game_icons if what == "guide" else icons
            if what in ic.code:
                n = int(2 * r * (.92 if kind == "face" else .80))
                _, shape, holes = ic.masks(what, n)
                full = sum(i * k for i, k in enumerate(shape.histogram())) / 255 / n / n > 0.7   # (a disc with a letter in it)
                mark = holes if full else ic.masks(what, int(n * (.7 if kind == "face" else .85)))[0]
                im.paste(INK, (round(x) - mark.width // 2, round(y) - mark.height // 2), mark)
    return im.resize(pro.size, Image.BOX)



# ---- the keyboard face's keys (nx2/prompt_keys.py: which key shows which icon), into the game's font: Kenney's Keyboard & Mouse
# keys (third_party/kenney_input_keyboard), and the keys drawn here: the keys whose names are words as wider keys with the word
# cut out, in the game's own bold font; the mouse's three buttons as a key with a mouse cut out of it and that button left white.
# Every contour is apart from the others or inside one (none cross), so the icons fill the same by either winding rule.
MOST = 2.2   # (how much wider than the game's icon a word key may be)
K = 0.5523   # (a quarter circle's control points, as a fraction of its radius)


def _area(record):
    from fontTools.pens.areaPen import AreaPen
    p = AreaPen()
    record.replay(p)
    return p.value


def _contour(pen, sign, draw):
    """draw(pen) one closed contour, turned to go counter-clockwise (y up) for sign 1, clockwise for -1"""
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    rec = RecordingPen()
    draw(rec)
    rec.replay(pen if (_area(rec) > 0) == (sign > 0) else ReverseContourPen(pen))


def _rrect(x0, y0, x1, y1, r):
    r = min(r, (x1 - x0) / 2, (y1 - y0) / 2)
    k = K * r

    def draw(pen):
        pen.moveTo((x0 + r, y0))
        pen.lineTo((x1 - r, y0))
        pen.curveTo((x1 - r + k, y0), (x1, y0 + r - k), (x1, y0 + r))
        pen.lineTo((x1, y1 - r))
        pen.curveTo((x1, y1 - r + k), (x1 - r + k, y1), (x1 - r, y1))
        pen.lineTo((x0 + r, y1))
        pen.curveTo((x0 + r - k, y1), (x0, y1 - r + k), (x0, y1 - r))
        pen.lineTo((x0, y0 + r))
        pen.curveTo((x0, y0 + r - k), (x0 + r - k, y0), (x0 + r, y0))
        pen.closePath()
    return draw


def _button(a, b, y0, top, y1, ry, left):
    """a mouse button's area: x a..b, from y0 up to top, then a quarter ellipse up to (the inner side, y1) on the outer side (left:
    at a), ry high and b - a wide"""
    rx = b - a

    def draw(pen):
        pen.moveTo((a, y0))
        pen.lineTo((b, y0))
        if left:
            pen.lineTo((b, y1))
            pen.curveTo((b - K * rx, y1), (a, top + K * ry), (a, top))
        else:
            pen.lineTo((b, top))
            pen.curveTo((b, top + K * ry), (a + K * rx, y1), (a, y1))
        pen.closePath()
    return draw


def _box(box):
    gx0, gy0, gx1, gy1 = box
    return max(gx1 - gx0, gy1 - gy0), (gx0 + gx1) / 2, (gy0 + gy1) / 2


def word_key(pen, label, box, lset, lcmap):
    """a key as high as the game's icon (box: that glyph's bounds) and as wide as the word needs, the word cut out of it, in the
    label font (lset, lcmap: its glyph set and cmap): how much wider than the icon it is"""
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.transformPen import TransformPen
    side, cx, cy = _box(box)
    names = [lcmap[ord(c)] for c in label]
    adv = sum(lset[n].width for n in names)
    bp = BoundsPen(lset)
    lset[lcmap[ord("H")]].draw(bp)
    caph = bp.bounds[3]
    s = side * 0.48 / caph   # (the capitals about half the key's height)
    pad = side * 0.20
    w = max(side, adv * s + 2 * pad)
    if w > side * MOST:
        s *= (side * MOST - 2 * pad) / (adv * s)
        w = side * MOST
    x0, y0 = cx - side / 2, cy - side / 2
    _contour(pen, -1 if _letters_ccw(lset, lcmap) else 1, _rrect(x0, y0, x0 + w, y0 + side, side * 0.2))   # (the letters' outer contours go the other way)
    x, ty = x0 + (w - adv * s) / 2, cy - caph * s / 2
    for n in names:
        lset[n].draw(TransformPen(pen, (s, 0, 0, s, x, ty)))
        x += lset[n].width * s
    return w - side


def _letters_ccw(lset, lcmap):
    from fontTools.pens.recordingPen import RecordingPen
    rec = RecordingPen()
    lset[lcmap[ord("I")]].draw(rec)
    return _area(rec) > 0


def mouse(pen, which, box, sign=1):
    """a key (as the others) with a mouse cut out of it, the button `which` (left, right; middle: the wheel) left white in the
    mouse. sign: the white contours' direction"""
    side, cx, cy = _box(box)
    x0, y0 = cx - side / 2, cy - side / 2
    _contour(pen, sign, _rrect(x0, y0, x0 + side, y0 + side, side * 0.2))   # the key
    mw, mh = side * 0.52, side * 0.76
    mx0, my0, mx1, my1 = cx - mw / 2, cy - mh / 2, cx + mw / 2, cy + mh / 2
    _contour(pen, -sign, _rrect(mx0, my0, mx1, my1, mw / 2))   # the mouse, cut out
    g = side * 0.06   # (the dark line round the white button, and between the buttons)
    ix0, ix1, iy1, ri = mx0 + g, mx1 - g, my1 - g, mw / 2 - g
    by = cy + side * 0.02   # (the buttons' lower edge)
    top = iy1 - ri
    if which in ("left", "right"):
        left = which == "left"
        a, b = (ix0, cx - g / 2) if left else (cx + g / 2, ix1)
        _contour(pen, sign, _button(a, b, by, top, iy1, ri, left))
    else:   # the wheel
        ww = side * 0.13
        _contour(pen, sign, _rrect(cx - ww / 2, by, cx + ww / 2, iy1 - side * 0.02, ww / 2))


def keyboard_glyphs(add, gset, cmap, chars, hmtx, label_otf, kset, draw):
    """the keyboard face's glyphs into the game's font (add(name, charstring, width): faces_font's; kset: Kenney's font's glyphs;
    draw(target, Kenney name, scale): that glyph in the box of the game's glyph target, its charstring and width; label_otf: the
    game's bold font, for the keys whose names are words): {code point: glyph name} for every key at KEY_BASE + its code and
    the sticks at STICK_KEYS.. (nx2/prompt_keys.py), and each key's advance by its code (256 of them: 0 none)"""
    import io
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.ttLib import TTFont
    lfont = TTFont(io.BytesIO(label_otf)) if label_otf else None
    lset, lcmap = (lfont.getGlyphSet(), lfont.getBestCmap()) if lfont else (None, None)
    target = cmap[0xE040]   # (each key in the box of the game's A)
    bp = BoundsPen(gset)
    gset[target].draw(bp)
    box, cs, width0 = bp.bounds, chars[target], hmtx[target][0]

    def drawn(name, rec, over):
        pen = T2CharStringPen(width0 + round(over), gset)
        rec.replay(pen)
        return add(name, pen.getCharString(private=cs.private, globalSubrs=cs.globalSubrs), width0 + round(over))

    placed, widths = {}, [0] * 256
    for code in range(1, 256):
        name = f"key_{code:02x}"
        if code in prompt_keys.MICE:
            rec = RecordingPen()
            mouse(rec, prompt_keys.MICE[code], box)
            placed[prompt_keys.KEY_BASE + code] = drawn(name, rec, 0)
        elif lfont and code in prompt_keys.LABELS:
            rec = RecordingPen()
            placed[prompt_keys.KEY_BASE + code] = drawn(name, rec, word_key(rec, prompt_keys.LABELS[code], box, lset, lcmap))
        elif prompt_keys.kenney_name(code) in kset and code in prompt_keys.KEYS:
            placed[prompt_keys.KEY_BASE + code] = add(name, *draw(target, prompt_keys.kenney_name(code)))
        else:
            continue
        widths[code] = hmtx[placed[prompt_keys.KEY_BASE + code]][0]
    made = {}
    for i, (code, stick, ways) in enumerate(prompt_keys.STICKS):
        placed[prompt_keys.STICK_GAME + i] = cmap[code]
        if stick == "cam":   # (the mouse moving, those ways; the arrows round it take room: a little larger than a key)
            kn = "mouse_move" if len(ways) == 4 else "mouse_horizontal" if ways[0] in "lr" else "mouse_vertical"
            if kn not in made:
                c, w = draw(target, kn, 1.2)
                made[kn] = add(f"stick_{kn}", c, w)
            placed[prompt_keys.STICK_KEYS + i] = made[kn]
        elif len(ways) == 1:
            placed[prompt_keys.STICK_KEYS + i] = placed[prompt_keys.KEY_BASE + prompt_keys.MOVE[ways]]
        elif lfont:
            label = "".join(chr(prompt_keys.MOVE[w]) for w in ways)
            if label not in made:
                rec = RecordingPen()
                made[label] = drawn(f"stick_{label.lower()}", rec, word_key(rec, label, box, lset, lcmap))
            placed[prompt_keys.STICK_KEYS + i] = made[label]
        else:
            placed[prompt_keys.STICK_KEYS + i] = cmap[code]
    return placed, widths


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    dictionary = P.zs_dictionary(a.romfs)
    b = P.read_zs(a.romfs, P.TEXTURES, dictionary)
    pro = P.decode(b, P.bntx_textures(b)["Controller_Pro"]).convert("RGBA")
    game_icons = P.Icons("switch", P.unwrap(P.read_zs(a.romfs, P.FONT, dictionary))[1])
    os.makedirs(a.out, exist_ok=True)
    for face in FACES:
        im = picture(face, pro, game_icons)
        if im:
            im.save(os.path.join(a.out, f"{face}.png"))
            print(f"prompts: {face}.png", file=sys.stderr)


if __name__ == "__main__":
    main()
