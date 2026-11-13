"""(hookshot) The button prompts of other controllers: the game's button-icon font redrawn per controller face, from the user's RomFS
and Kenney's Input Prompts (CC0, third_party/kenney_input).

    python -I nx2/prompt_assets.py --romfs <dir> --out <game folder>/prompts

Writes <out>/External.bfotf, one font with every face's icons (switch, xbox, ps4, ps5, steamdeck). Every text in the game draws
its button icons from Font2/Scalable/External.*.bfotf (dialogue, the control hints, menus): A B X Y at U+E040..E043, L R ZL ZR at
E044..E047, Plus and Minus at E04B and E04C, the stick presses at E056 and E057, the d-pad at E060..E063. The written font has, for
each face, one icon per pad input at U+F700 + 32 x HOS_PROMPT_* + HOS_PAD_* (gpu/vk/vk_api.h) and the face's d-pad icons (STATIC) at
U+F700 + 32 x HOS_PROMPT_* + 24.., and each of those Switch buttons' and d-pad icons' code points in a cmap segment of its own: the
game (host/prompts.c) points each one at the face's icon of the pad input bound to that button when it serves the font, and again
when the face or the controls change while it runs, so the icons follow the controls, changed ones too. As built, the font's code
points are the game's: its own images.

The textures with button icons (the ocarina's notes, the letters on black, the controller in the pause menu) the same way, with
Pillow, astc_encoder and texture2ddecoder: <out>/Common.bntx is the game's UI/Layout2Archive/Common bntx decompressed, and
<face>/textures.bin the encoded blocks to write over it for each Switch button on each pad input, which the game picks by the
controls when it serves the file. The Talk ring's buttons and the ocarina hint's icons (the layouts of UI/Layout2Archive/Common,
<out>/Common.blarc) the same way: <out>/ring.bin and <face>/hint.bin say what to write over them for the controls.

Each face's images (the notes, the letters, the controller picture drawn by nx2/prompt_draw.py, the leader lines) are made as
layers first: --dump <dir> writes them as PNG, and a PNG of the same name in nx2/prompt_faces/<face>/ takes a layer's place, to
rework one by hand (nx2/prompt_faces/README.md).

The font is larger than the game's, and the game drops a file larger than its entry in the resource size table: <out>/
rsizetable.zs is the game's table with the font's entry raised to fit it, served in its place (host/prompts.c merges it into a
mod's own table when a mod ships one).

A .bfotf is an OpenType font behind an 8-byte header and XORed per little-endian word: the header is the magic 0x18029a7f and the
font's size (big-endian), each XORed with the same key as the font. The .zs files are zstd frames made with the dictionary zs.zsdic
of Pack/ZsDic.pack.zs.
"""
import argparse
import contextlib
import hashlib
import importlib
import importlib.metadata
import io
import json
import os
import platform
import shutil
import stat
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)   # (python -I leaves the script folder off)
from prompt_faces import FACES, override, parts   # noqa: E402 (the faces: their icons, drawings and their parts)
import prompt_keys   # noqa: E402 (the keyboard face)
KENNEY = os.path.join(os.path.dirname(HERE), "third_party", "kenney_input")
KENNEY_KEYBOARD = os.path.join(os.path.dirname(HERE), "third_party", "kenney_input_keyboard")   # (the keyboard face's)
FONT = "Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs"
MAGIC = 0x18029A7F
PAD_BASE = 0xF700
FACE_CODES = 32   # (the code points of each face: its pad icons, then at STATIC_AT its d-pad icons)
STATIC_AT = 24
# a Switch button's icon in a text once the face or the controls changed while the game runs (host/prompts.c rewrites the text's
# code): BIND_BASE + (face x 16 + Switch button) x 17 + pad input, the face's icon of that pad input. A code of its own for each,
# where the game keeps a code's glyph drawn: a new icon is drawn anew, never an old one shown
BIND_BASE = 0xF000
ALL_FACES = ["switch"] + list(FACES) + ["keyboard"]   # (HOS_PROMPT_*: the keyboard last, nx2/prompt_keys.py)
WIDE = 1.6   # (how much wider than the game's icon a wide Kenney icon may be)
# the HOS_PAD_* inputs (gpu/vk/vk_api.h), in order
PADS = ["south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder", "up", "down", "left",
        "right", "ltrigger", "rtrigger"]
# the Switch buttons' code points, in HOS_SW_* order (A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down), and hookshot's
# default controls for them (host/nn_hid.c pad_default: by position, A the right face button)
SWITCH = [0xE040, 0xE041, 0xE042, 0xE043, 0xE056, 0xE057, 0xE044, 0xE045, 0xE046, 0xE047, 0xE04B, 0xE04C, 0xE062, 0xE060, 0xE063,
          0xE061]
DEFAULT = ["east", "south", "north", "west", "lstick", "rstick", "lshoulder", "rshoulder", "ltrigger", "rtrigger", "start", "back",
           "left", "up", "right", "down"]
# the game's own icon for each pad input: what the Switch face shows
SWITCH_ICON = {"south": 0xE041, "east": 0xE040, "west": 0xE043, "north": 0xE042, "back": 0xE04C, "guide": 0xE04A, "start": 0xE04B,
               "lstick": 0xE056, "rstick": 0xE057, "lshoulder": 0xE044, "rshoulder": 0xE045, "up": 0xE060, "down": 0xE061,
               "left": 0xE062, "right": 0xE063, "ltrigger": 0xE046, "rtrigger": 0xE047}
# the icons that do not follow the controls, by the Kenney glyph's name after the face's prefix: the whole d-pad (the sticks keep the
# game's icons, an L or R cap with its arrows: right for any controller, where Kenney's are drawn in perspective)
STATIC = {0xE049: "dpad", 0xE064: "dpad_vertical_outline", 0xE065: "dpad_horizontal_outline"}


def zs_dictionary(romfs):
    """zs.zsdic, out of Pack/ZsDic.pack.zs (a SARC, itself zstd without a dictionary)"""
    import zstandard
    with open(os.path.join(romfs, "Pack", "ZsDic.pack.zs"), "rb") as f:
        b = zstandard.ZstdDecompressor().decompressobj().decompress(f.read())
    data, = struct.unpack_from("<I", b, 12)
    hl, = struct.unpack_from("<H", b, 4)
    n, = struct.unpack_from("<H", b, hl + 6)
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        attr, start, end = struct.unpack_from("<III", b, hl + 12 + 16 * i + 4)
        o = names + (attr & 0xFFFFFF) * 4
        if b[o:b.index(b"\0", o)] == b"zs.zsdic":
            return b[data + start:data + end]
    raise SystemExit("prompts: no zs.zsdic in Pack/ZsDic.pack.zs")


def read_zs(romfs, rel, dictionary):
    import zstandard
    d = zstandard.ZstdDecompressor(dict_data=zstandard.ZstdCompressionDict(dictionary))
    with open(os.path.join(romfs, rel), "rb") as f:
        return d.decompressobj().decompress(f.read())


def size_table(romfs, dictionary, game_size, font_size):
    """the game's resource size table (System/Resource/ResourceSizeTable.*.rsizetable.zs: "RESTBL", u32 version, u32 name length,
    u32 hashed entries, u32 named entries; the hashed ones u32 CRC-32 of the path, u32 size, by hash) with the External font's
    entry raised from the game's font (game_size bytes) to fit one of font_size: the game drops a file larger than its entry. None
    when it fits already"""
    import zlib
    folder = os.path.join(romfs, "System", "Resource")
    name = next((n for n in sorted(os.listdir(folder)) if n.startswith("ResourceSizeTable.") and n.endswith(".rsizetable.zs")),
                None)
    if not name:
        raise SystemExit("prompts: no System/Resource/ResourceSizeTable.*.rsizetable.zs")
    b = bytearray(read_zs(romfs, f"System/Resource/{name}", dictionary))
    if b[:6] != b"RESTBL":
        raise SystemExit(f"prompts: {name} is not a RESTBL table")
    hashed, = struct.unpack_from("<I", b, 14)
    want = zlib.crc32(FONT[:-3].encode())
    for i in range(hashed):
        at = 22 + 8 * i
        h, size = struct.unpack_from("<II", b, at)
        if h == want:
            fit = font_size + size - game_size   # (the entry's allowance over the file kept: 288 bytes)
            if fit <= size:
                return None
            struct.pack_into("<I", b, at + 4, fit)
            return bytes(b)
    raise SystemExit(f"prompts: {name} has no entry for {FONT[:-3]}")


def unwrap(blob):
    key = struct.unpack_from("<I", blob, 0)[0] ^ MAGIC
    size = struct.unpack(">I", struct.pack("<I", struct.unpack_from("<I", blob, 4)[0] ^ key))[0]
    body = b"".join(struct.pack("<I", w ^ key) for w in struct.unpack_from(f"<{(len(blob) - 8) // 4}I", blob, 8))
    return key, body[:size]


def wrap(key, otf):
    otf += b"\0" * (-len(otf) % 4)
    head = struct.pack("<II", MAGIC ^ key, struct.unpack("<I", struct.pack(">I", len(otf)))[0] ^ key)
    return head + b"".join(struct.pack("<I", w ^ key) for w in struct.unpack(f"<{len(otf) // 4}I", otf))


def cmap4(mapping, single):
    """a cmap table of one format 4 subtable (for Unicode BMP, platforms 0 and 3) from {code: glyph id}: runs of codes with
    consecutive glyphs share a segment, and each code of `single` has one of its own; every segment by idDelta alone"""
    segs = []
    for c in sorted(mapping):
        g = mapping[c]
        if segs and c not in single and segs[-1][1] == c - 1 and segs[-1][1] not in single and \
                (segs[-1][2] + c) & 0xFFFF == g:
            segs[-1][1] = c
        else:
            segs.append([c, c, (g - c) & 0xFFFF])
    segs.append([0xFFFF, 0xFFFF, 1])
    n = len(segs)
    search = 2 * (1 << (n.bit_length() - 1))
    sub = struct.pack(">HHHHHHH", 4, 16 + 8 * n, 0, 2 * n, search, (search // 2).bit_length() - 1, 2 * n - search)
    sub += b"".join(struct.pack(">H", s[1]) for s in segs) + b"\0\0"
    sub += b"".join(struct.pack(">H", s[0]) for s in segs)
    sub += b"".join(struct.pack(">H", s[2]) for s in segs)
    sub += b"\0\0" * n
    return struct.pack(">HHHHIHHI", 0, 2, 0, 3, 20, 3, 1, 20) + sub


def faces_font(otf, faces, label_otf=None):
    """the game's font (decrypted) with every face's icons (faces: their names, in HOS_PROMPT_* order), and each face's icons'
    advances ({face: [advance per pad input]}; the keyboard's: per key code): a pad icon per input at PAD_BASE + FACE_CODES x face +
    its number and the face's d-pad icons at PAD_BASE + FACE_CODES x face + STATIC_AT.. (the Switch's, and an input a face has no
    glyph for: the game's own glyph), each Switch button's on each pad input at BIND_BASE.., the game's code points kept. The
    keyboard face (nx2/prompt_keys.py; label_otf: the game's bold font, for its word keys): the Switch's pad icons, and its keys'
    and sticks' icons"""
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables.DefaultTable import DefaultTable

    game = TTFont(io.BytesIO(otf))
    gset, cmap = game.getGlyphSet(), dict(game.getBestCmap())
    top = game["CFF "].cff.topDictIndex[0]
    chars = top.CharStrings
    order = list(game.getGlyphOrder())
    hmtx = game["hmtx"]

    def bounds(glyphset, name):
        p = BoundsPen(glyphset)
        glyphset[name].draw(p)
        return p.bounds

    def add(name, charstring, width):
        chars.charStringsIndex.append(charstring)
        chars.charStrings[name] = len(chars.charStringsIndex) - 1
        top.charset.append(name)
        order.append(name)
        hmtx.metrics[name] = (width, 0)
        return name

    def drawer(kset):
        def draw(target, kn, scale=1):
            """the Kenney glyph scaled into the box of the game's glyph `target` (as a square around its centre); a wide one
            (the Menu and View pills on the square Plus and Minus) as high as the box, up to WIDE times as wide, the glyph
            widened"""
            gb, kb = bounds(gset, target), bounds(kset, kn)
            side = max(gb[2] - gb[0], gb[3] - gb[1])
            s = scale * min(side * WIDE / (kb[2] - kb[0]), side / (kb[3] - kb[1]))
            over = max(0, s * (kb[2] - kb[0]) - side)
            dx = (gb[0] + gb[2] + over) / 2 - s * (kb[0] + kb[2]) / 2
            dy = (gb[1] + gb[3]) / 2 - s * (kb[1] + kb[3]) / 2
            width = hmtx[target][0] + round(over)
            pen = T2CharStringPen(width, gset)
            kset[kn].draw(TransformPen(pen, (s, 0, 0, s, dx, dy)))
            cs = chars[target]
            return pen.getCharString(private=cs.private, globalSubrs=cs.globalSubrs), width
        return draw

    assert BIND_BASE + len(faces) * len(SWITCH) * len(PADS) <= PAD_BASE
    assert PAD_BASE + FACE_CODES * len(faces) <= prompt_keys.KEY_BASE
    placed, widths = {}, {}   # ({code: glyph name}, the advances)
    for n, face in enumerate(faces):
        base = PAD_BASE + FACE_CODES * n
        if face in ("switch", "keyboard"):
            for i, p in enumerate(PADS):
                placed[base + i] = cmap[SWITCH_ICON[p]]
            for k, c in enumerate(STATIC):
                if c in cmap:
                    placed[base + STATIC_AT + k] = cmap[c]
        else:
            f = FACES[face]
            prefix, names = f["prefix"], f["icons"]
            kset = TTFont(os.path.join(KENNEY, f["font"])).getGlyphSet()
            draw = drawer(kset)

            def kname(s):
                return s[1:] if s.startswith("/") else prefix + s

            for i, p in enumerate(PADS):
                kn = kname(names[p]) if p in names else None
                target = cmap[SWITCH_ICON[p]]
                # (no such button on this face, the PlayStation's PS button: the game's own icon)
                placed[base + i] = add(f"pad_{face}_{p}", *draw(target, kn)) if kn in kset else target
            for k, (c, sn) in enumerate(STATIC.items()):
                kn = prefix + sn
                if c in cmap:
                    placed[base + STATIC_AT + k] = add(f"static_{face}_{c:04x}", *draw(cmap[c], kn)) if kn in kset else cmap[c]
        widths[face] = [hmtx[placed[base + i]][0] for i in range(len(PADS))]
        if face == "keyboard":
            import prompt_draw
            kset = TTFont(os.path.join(KENNEY_KEYBOARD, prompt_keys.FONT)).getGlyphSet()
            keys, widths[face] = prompt_draw.keyboard_glyphs(add, gset, cmap, chars, hmtx, label_otf, kset, drawer(kset))
            placed.update(keys)
            for i in range(prompt_keys.SLOTS):   # (host/prompts.c points each at its key when it gives it out)
                placed[prompt_keys.SLOT_BASE + i] = cmap[SWITCH[0]]
        for b in range(len(SWITCH)):
            for i in range(len(PADS)):
                placed[BIND_BASE + (n * len(SWITCH) + b) * len(PADS) + i] = placed[base + i]

    game.setGlyphOrder(order)
    gid = {n: i for i, n in enumerate(order)}
    mapping = {c: gid[n] for c, n in cmap.items() if c <= 0xFFFF}
    mapping.update({c: gid[n] for c, n in placed.items()})
    t = DefaultTable("cmap")
    singles = set(SWITCH) | set(STATIC)   # (each a segment of its own: host/prompts.c points them at the face's icons)
    if "keyboard" in faces:
        singles |= {c for c, _, _ in prompt_keys.STICKS} | {prompt_keys.SLOT_BASE + i for i in range(prompt_keys.SLOTS)}
    t.data = cmap4(mapping, singles)
    game["cmap"] = t
    out = io.BytesIO()
    game.save(out)
    return out.getvalue(), widths


# ---- the textures with button icons (UI/Layout2Archive/Common.*.bntx: 878 textures, ASTC 4x4 and BC4, the Switch 2's block-linear
# layout). prompts/Common.bntx is the game's file, decompressed; prompts/<face>/textures.bin the blocks to write over it, for each
# Switch button and each pad input it can be bound to (host/prompts.c writes those of the controls in use):
#     "HPTX", u32 count, then count times: u8 Switch button (HOS_SW_*), u8 pad input (HOS_PAD_*), u16 0, u32 runs,
#     runs times: u32 offset in the file, u32 length, the bytes
# Each face's images are made first, as layers (face_layers), then encoded: --dump writes the layers as PNG, and a PNG of the same
# name in nx2/prompt_faces/<face>/ takes a layer's place (prompt_faces.override). A layer's name: the texture's, then for a Switch
# button's image on a pad input ".<pad input>" (the notes, the letters; the leader lines ".<Switch button>.<pad input>": only the
# line, drawn over the texture's lines that stay), ".png".
TEXTURES = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs"
SW = {n: i for i, n in enumerate(["a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down"])}
# the ocarina's notes (white icons, dark letters) and the white letters on black (KeyTexFont, BC4)
NOTES = {"OcarinaButtonA_00": "a", "OcarinaButtonB_00": "b", "OcarinaButtonX_00": "x", "OcarinaButtonY_00": "y",
         "OcarinaButtonL_00": "l", "OcarinaButtonR_00": "r"}
LETTERS = {"KeyTexFontA_00": "a", "KeyTexFontB_00": "b", "KeyTexFontX_00": "x", "KeyTexFontY_00": "y"}
# the Pro Controller in the pause menu's controls page (System): the picture, the page's copy of it (tinted: PICTURE_TINT) and its
# shadow (PICTURE_SHADOW); a face's own picture (nx2/prompt_faces.py) takes their place, else the face buttons' discs (centre,
# radius in pixels) by the pad input there get the face's labels (a picture of the controller: its own labels, whatever the controls)
PICTURES = ("Controller_Pro", "Controller_Pro1")
PICTURE_TINT, PICTURE_SHADOW = "Controller_Pro1", "ControllerSh_Pro1"
PICTURE_BUTTONS = {"east": (869, 227, 31), "south": (800, 286, 31), "north": (800, 168, 31), "west": (732, 227, 31)}
# the page's leader lines (BC4, white on black, drawn over the picture: GUIDE_SCALE and GUIDE_AT take its pixels to theirs), from
# the labels' bars (BARS: the left ones' right edge, the right ones' left edge, kept from the game's) to the picture's parts
# (nx2/prompt_faces.py): each line's texture, bar (0 left, 1 right), row (its top; 6 pixels thick) and end: a Switch button (the
# part of the pad input bound to it: one line per pad input, chosen by the controls; a face button or the d-pad: a frame round the
# group, as the game's round its face buttons, the label's icon telling which) or a pad input (fixed: the sticks)
GUIDES = ("ControllerGuide_Pro_00", "ControllerGuide_Pro_01")
GUIDE_SCALE, GUIDE_AT, BARS = 1.1136, (131.3, 462.6), (147, 1407)
GUIDE_LINES = [(0, 0, 333, "zl"), (1, 0, 561, "minus"), (0, 0, 667, "lstick"), (0, 1, 558, "plus"), (0, 1, 713, "a"),
               (0, 1, 1225, "rstick")]


def bntx_textures(b):
    """{name: (BRTI offset, format, width, height, GOBs per block, data offset)} of a Switch 2 BNTX (platform "Ounc")"""
    count, = struct.unpack_from("<I", b, 0x24)
    info, = struct.unpack_from("<Q", b, 0x28)
    out = {}
    for i in range(count):
        p, = struct.unpack_from("<Q", b, info + 8 * i)
        no, = struct.unpack_from("<Q", b, p + 0x60)
        nl, = struct.unpack_from("<H", b, no)
        fmt, = struct.unpack_from("<I", b, p + 0x1C)
        w, h = struct.unpack_from("<II", b, p + 0x24)
        bh, = struct.unpack_from("<I", b, p + 0x34)
        mips, = struct.unpack_from("<Q", b, p + 0x70)
        data, = struct.unpack_from("<Q", b, mips)
        out[b[no + 2:no + 2 + nl].decode()] = (p, fmt, w, h, 1 << bh, data)
    return out


def gob_offset(x, y, gobs, pitch):
    """a byte's place in a block-linear texture (x in bytes, y in rows of blocks): the Switch 2's GOB, the Tegra X1's with
    offset bits 5 and 6 swapped (gpu/render/rx_nvn.cpp bl_offset)"""
    a = (y // (8 * gobs)) * 512 * gobs * pitch + (x // 64) * 512 * gobs + (y % (8 * gobs) // 8) * 512
    return a + (((x & 63) >> 5) << 8 | ((y & 7) >> 2) << 7 | ((x & 31) >> 4) << 6 | ((y >> 1) & 1) << 5 | (y & 1) << 4 | (x & 15))


def decode(b, t):
    """a texture as a PIL image (RGBA, or L for BC4)"""
    import texture2ddecoder
    from PIL import Image
    _, fmt, w, h, gobs, data = t
    bpe = 16 if fmt >> 8 == 0x2D else 8
    bw, bh = (w + 3) // 4, (h + 3) // 4
    pitch = (bw * bpe + 63) // 64
    lin = b"".join(b[data + o:data + o + bpe] for o in (gob_offset(x * bpe, y, gobs, pitch) for y in range(bh) for x in range(bw)))
    if bpe == 16:
        return Image.frombytes("RGBA", (bw * 4, bh * 4), texture2ddecoder.decode_astc(lin, bw * 4, bh * 4, 4, 4), "raw",
                               "BGRA").crop((0, 0, w, h))
    return Image.frombytes("L", (bw * 4, bh * 4), lin, "bcn", 4).crop((0, 0, w, h))


def encode_bc4(im):
    """BC4 by each block's darkest and lightest, the 6 levels between"""
    import numpy as np
    v = np.asarray(im, dtype=np.int32)
    v = v.reshape(im.height // 4, 4, im.width // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
    hi, lo = v.max(1), v.min(1)
    span = np.maximum(hi - lo, 1)[:, None]
    t = np.where((hi > lo)[:, None], np.rint((v - lo[:, None]) * 7 / span), 7).astype(np.uint64)
    code = np.where(t == 7, 0, np.where(t == 0, 1, 8 - t)).astype(np.uint64)   # (0 the lightest, 1 the darkest, 2..7 between)
    idx = (code << (np.arange(16, dtype=np.uint64) * 3)).sum(1, dtype=np.uint64)
    out = np.zeros((len(v), 8), np.uint8)
    out[:, 0], out[:, 1] = hi, lo
    out[:, 2:] = idx.astype("<u8").view(np.uint8).reshape(-1, 8)[:, :6]
    return out.tobytes()


def encode_astc(im, srgb):
    import astc_encoder as A
    ctx = A.ASTCContext(A.ASTCConfig(A.ASTCProfile.LDR_SRGB if srgb else A.ASTCProfile.LDR, 4, 4, 1, A.ASTCQualityPreset.THOROUGH))
    return ctx.compress(A.ASTCImage(A.ASTCType.U8, im.width, im.height, 1, im.tobytes()), A.ASTCSwizzle.from_str("RGBA"))


def runs(t, im, box=None, mask=None):
    """the file's bytes for the blocks of im (the whole texture's size) inside box (pixels; all of it by default), or those with
    any of mask (L, im's size) in them, as (offset, bytes) runs"""
    from PIL import Image
    _, fmt, w, h, gobs, data = t
    bpe = 16 if fmt >> 8 == 0x2D else 8
    bw, bh = (w + 3) // 4, (h + 3) // 4
    x0, y0, x1, y1 = box or (mask and mask.getbbox()) or (0, 0, w, h)
    bx0, by0, bx1, by1 = max(0, x0 // 4), max(0, y0 // 4), min(bw, (x1 + 3) // 4), min(bh, (y1 + 3) // 4)
    part = Image.new(im.mode, (bw * 4, bh * 4))   # (only the box's blocks are encoded)
    part.paste(im, (0, 0))
    part = part.crop((bx0 * 4, by0 * 4, bx1 * 4, by1 * 4))
    blocks = encode_astc(part.convert("RGBA"), (fmt & 0xFF) == 6) if bpe == 16 else encode_bc4(part.convert("L"))
    pw = bx1 - bx0
    pitch = (bw * bpe + 63) // 64
    if mask:   # (the blocks with any of the mask)
        import numpy as np
        m = np.zeros((bh * 4, bw * 4), bool)
        m[:h, :w] = np.asarray(mask) > 0
        cover = m.reshape(bh, 4, bw, 4).any((1, 3))
    pieces = sorted((data + gob_offset(x * bpe, y, gobs, pitch),
                     blocks[((y - by0) * pw + x - bx0) * bpe:((y - by0) * pw + x - bx0 + 1) * bpe])
                    for y in range(by0, by1) for x in range(bx0, bx1) if not mask or cover[y, x])
    out = []
    for off, piece in pieces:   # (blocks next to each other in the file: one run)
        if out and out[-1][0] + len(out[-1][1]) == off:
            out[-1][1] += piece
        else:
            out.append([off, bytearray(piece)])
    return out


class Icons:
    """a face's icon for each pad input, drawn from a font (the game's own for the Switch face, else Kenney's)"""

    def __init__(self, face, otf):
        from fontTools.ttLib import TTFont
        if face == "switch":
            self.path, self.code = io.BytesIO(otf), {p: SWITCH_ICON[p] for p in PADS}
        else:
            f = FACES[face]
            prefix, names = f["prefix"], f["icons"]
            self.path = os.path.join(KENNEY, f["font"])
            rev = {n: c for c, n in TTFont(self.path).getBestCmap().items()}
            self.code = {}
            for p in PADS:
                n = names.get(p)
                n = n and (n[1:] if n.startswith("/") else prefix + n)
                if n in rev:
                    self.code[p] = rev[n]

    def masks(self, pad, size):
        """(the icon, its silhouette with the holes filled, the holes) as L masks of size x size, the icon centred in it"""
        from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont
        if isinstance(self.path, io.BytesIO):
            self.path.seek(0)
        f = ImageFont.truetype(self.path, size * 4)
        g = Image.new("L", (size * 8, size * 8), 0)
        ImageDraw.Draw(g).text((size * 2, size * 2), chr(self.code[pad]), font=f, fill=255)
        g = g.crop(g.getbbox())
        s = size / max(g.size)
        g = g.resize((max(1, round(g.width * s)), max(1, round(g.height * s))), Image.LANCZOS)
        icon = Image.new("L", (size, size), 0)
        icon.paste(g, ((size - g.width) // 2, (size - g.height) // 2))
        outside = Image.new("L", (size + 4, size + 4), 255)   # (a margin: every side's outside joins the corner's)
        outside.paste(ImageChops.invert(icon.point(lambda v: 255 if v > 127 else 0)), (2, 2))
        ImageDraw.floodfill(outside, (0, 0), 128)   # (what the corner reaches: the outside)
        shape = outside.crop((2, 2, size + 2, size + 2)).point(lambda v: 0 if v == 128 else 255)
        inner = shape.filter(ImageFilter.MinFilter(5))   # (the soft rim of the icon is not a hole)
        holes = ImageChops.multiply(inner, ImageChops.invert(icon))
        return icon, shape, holes


class Layer:
    """an image of a texture's: written whole, or its box (pixels) of it, or (over: a layer's name) drawn over that layer where
    it has any of itself; for a Switch button (0xff: always) on a pad input"""

    def __init__(self, name, texture, sw, pad, im, box=None, over=None):
        self.name, self.texture, self.sw, self.pad, self.im, self.box, self.over = name, texture, sw, pad, im, box, over


def face_layers(b, tex, face, otf):
    """this face's images, as layers (in textures.bin's order), each a hand-made one if nx2/prompt_faces/<face>/ has it"""
    from PIL import Image, ImageDraw
    import prompt_draw
    icons = Icons(face, otf)
    layers = []

    def keep(sw, pad):   # (the Switch face's default controls: the game's own textures)
        return pad in icons.code and not (face == "switch" and DEFAULT[SW[sw]] == pad)

    for name, sw in NOTES.items():
        t = tex[name]
        w, h = t[2], t[3]
        for pad in PADS:
            if not keep(sw, pad):
                continue
            icon, shape, holes = icons.masks(pad, int(min(w, h) * 0.9))
            im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            o = ((w - icon.width) // 2, (h - icon.height) // 2)
            im.paste(Image.new("RGBA", icon.size, (255, 255, 255, 255)), o, shape)
            im.paste(Image.new("RGBA", icon.size, (0, 0, 0, 255)), o, holes)
            layers.append(Layer(f"{name}.{pad}.png", name, SW[sw], PADS.index(pad), im))
    for name, sw in LETTERS.items():
        t = tex[name]
        w, h = t[2], t[3]
        for pad in PADS:
            if not keep(sw, pad):
                continue
            icon, shape, holes = icons.masks(pad, int(min(w, h) * 0.9))
            letter = holes if holes.getbbox() else icon   # (an icon with no letter in it: its shape)
            im = Image.new("L", (w, h), 0)
            im.paste(letter, ((w - letter.width) // 2, (h - letter.height) // 2))
            layers.append(Layer(f"{name}.{pad}.png", name, SW[sw], PADS.index(pad), im))
    own = None
    if face != "switch":
        hand = override(face, PICTURES[0] + ".png")
        own = Image.open(hand).convert("RGBA") if hand else prompt_draw.picture(
            face, decode(b, tex[PICTURES[0]]).convert("RGBA"), Icons("switch", otf))
    if own:
        layers += picture_layers(b, tex, own)
    for name in PICTURES if face != "switch" and not own else ():
        t = tex[name]
        im = decode(b, t)
        px = im.load()
        for pad, (cx, cy, r) in PICTURE_BUTTONS.items():
            fill, ink = px[cx + r - 6, cy], (205, 205, 205, 255)
            ImageDraw.Draw(im).ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill)
            size = int(2 * r * 0.92)
            icon, shape, holes = icons.masks(pad, size)
            disc = sum(i * n for i, n in enumerate(shape.histogram())) / 255 / size / size > 0.7   # (a disc with a letter in it: the letter)
            mark = holes if disc else icons.masks(pad, int(size * 0.7))[0]
            im.paste(Image.new("RGBA", mark.size, ink), (cx - mark.width // 2, cy - mark.height // 2), mark)
        xs, ys = [c[0] for c in PICTURE_BUTTONS.values()], [c[1] for c in PICTURE_BUTTONS.values()]
        r = max(c[2] for c in PICTURE_BUTTONS.values())
        layers.append(Layer(name + ".png", name, 0xFF, 0, im, (min(xs) - r, min(ys) - r, max(xs) + r + 1, max(ys) + r + 1)))
    layers += guide_layers_of(b, tex, face)
    for layer in layers:
        hand = override(face, layer.name)
        if hand:
            im = Image.open(hand).convert(layer.im.mode)
            if im.size != layer.im.size:
                raise SystemExit(f"prompts: {hand} is {im.size[0]}x{im.size[1]}, the texture {layer.im.size[0]}x{layer.im.size[1]}")
            layer.im = im
    return layers


def face_textures(b, tex, layers):
    """textures.bin's entries for these layers: [(Switch button, pad input, runs)], Switch button 0xff for those always written"""
    from PIL import ImageChops
    entries, made = [], {}
    for layer in layers:
        t, im = tex[layer.texture], layer.im
        made[layer.name] = im
        if layer.over:
            entries.append((layer.sw, layer.pad, runs(t, ImageChops.lighter(made[layer.over], im), mask=im)))
        elif layer.box:
            x0, y0, x1, y1 = layer.box
            if im.getbbox():   # (a hand-made image may reach further)
                bx0, by0, bx1, by1 = im.getbbox()
                x0, y0, x1, y1 = min(x0, bx0), min(y0, by0), max(x1, bx1), max(y1, by1)
            entries.append((layer.sw, layer.pad, runs(t, im, (x0, y0, x1, y1))))
        else:
            entries.append((layer.sw, layer.pad, runs(t, im)))
    return entries


# the groups a line frames, ending on the frame, when its pad input is one of them; the face buttons' labels (Switch A's line, under
# one bar with B, X and Y's) always frame one: the d-pad when A is on it, else the face buttons
GUIDE_GROUPS = (("north", "west", "east", "south"), ("up", "left", "right", "down"))
GUIDE_FACE = "a"
# the lines' router: a grid of GUIDE_GRID pixels; a bend costs GUIDE_BEND pixels of length, crossing a line that stays GUIDE_CROSS,
# coming to a part other than from above GUIDE_SIDE; a line keeps GUIDE_CLEAR pixels off the other parts (the sticks GUIDE_CLEAR / 2)
GUIDE_GRID, GUIDE_BEND, GUIDE_CROSS, GUIDE_SIDE, GUIDE_CLEAR = 8, 64, 40, 48, 16


def guide_at(part):
    """a part (picture pixels: x, y, r) in the lines' pixels"""
    return part[0] * GUIDE_SCALE + GUIDE_AT[0], part[1] * GUIDE_SCALE + GUIDE_AT[1], part[2] * GUIDE_SCALE


def guide_frame_at(where, group):
    """a group's frame: its centre and its corners' distance from it (the lines' pixels)"""
    pts = [guide_at(where[p]) for p in group]
    cx, cy = sum(p[0] for p in pts) / 4, sum(p[1] for p in pts) / 4
    return cx, cy, max(max(abs(x - cx), abs(y - cy)) + r for x, y, r in pts) + 8   # (from the centre to a corner, as the game's)


def guide_route(size, bar, row, where, pad, stays, frame=None):
    """the way (points, the lines' pixels) of a line from a bar's row to the pad input's part, round the picture's other parts and
    with the fewest bends, crossing the lines that stay (L) as little as it can: it comes to a stick's centre, to a part's edge
    (from above best), or to a frame's corner (frame: centre, corners' distance)"""
    import heapq
    import math
    import numpy as np
    from PIL import ImageFilter
    g = GUIDE_GRID
    x0, y0 = BARS[0] if bar == 0 else BARS[1], row + 3
    ilo, ihi = (0, (BARS[1] - x0) // g) if bar == 0 else (-((x0 - BARS[0]) // g), 0)
    jlo, jhi = -((y0 - 250) // g), (min(size[1] - 20, 1420) - y0) // g
    xs, ys = x0 + g * np.arange(ilo, ihi + 1), y0 + g * np.arange(jlo, jhi + 1)
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    blocked = np.zeros(X.shape, bool)
    own = set(next((grp for grp in GUIDE_GROUPS if pad in grp), ())) if frame else {pad}
    for p, part in where.items():
        if len(part) == 4:   # (a box to keep off)
            (bx0, by0, _), (bx1, by1, _) = guide_at(part[:2] + (0,)), guide_at(part[2:] + (0,))
            m = GUIDE_CLEAR / 2
            blocked |= (X > bx0 - m) & (X < bx1 + m) & (Y > by0 - m) & (Y < by1 + m)
            continue
        x, y, r = guide_at(part)
        if p in own:
            continue
        clear = GUIDE_CLEAR / 2 if p in ("lstick", "rstick") else GUIDE_CLEAR
        blocked |= (X - x) ** 2 + (Y - y) ** 2 < (r + clear) ** 2
    for grp in GUIDE_GROUPS:   # (a group: inside its frame, which another line may draw, its own inside it)
        if all(p in where for p in grp):
            cx, cy, e = guide_frame_at(where, grp)
            if frame and pad in grp:
                blocked |= (X - cx) ** 2 + (Y - cy) ** 2 < (e - 8) ** 2
            else:   # (the frame: a square on its corner, its sides 1.15 corners' distance off in x + y)
                blocked |= np.abs(X - cx) + np.abs(Y - cy) < 1.15 * e + GUIDE_CLEAR
    blocked |= ((np.abs(X - BARS[0]) < 14) | (np.abs(X - BARS[1]) < 14)) & (Y != y0)   # (off the bars but on its own row)
    st = np.asarray(stays.filter(ImageFilter.MaxFilter(9)))
    pen = np.where(st[np.clip(Y, 0, size[1] - 1), np.clip(X, 0, size[0] - 1)] > 0, GUIDE_CROSS, 0)
    dirs = ((1, 0), (-1, 0), (0, 1), (0, -1))
    goals = {}   # ((i, j, dir): (the end, extra cost)): the cell two before the end, coming the way it ends

    def goal(ex, ey, d, extra):
        i, j = round((ex - x0) / g) - 2 * dirs[d][0], round((ey - y0) / g) - 2 * dirs[d][1]
        cells = [(i + k * dirs[d][0], j + k * dirs[d][1]) for k in range(3)]
        if all(ilo <= ci <= ihi and jlo <= cj <= jhi and not blocked[ci - ilo, cj - jlo] for ci, cj in cells[:2]):
            goals[(i, j, d)] = ((ex, ey), extra)

    x, y, r = guide_at(where[pad])
    if frame:   # (a corner, or beside it on the grid's nearest line, along the frame's side)
        cx, cy, e = frame
        for k in (0, 1):
            sy, sx = y0 + g * (math.floor((cy - y0) / g) + k) - cy, x0 + g * (math.floor((cx - x0) / g) + k) - cx
            for (ex, ey), d, extra in (((cx + e - abs(sy), cy + sy), 1, 0), ((cx + sx, cy - e + abs(sx)), 2, GUIDE_SIDE),
                                       ((cx - e + abs(sy), cy + sy), 0, GUIDE_SIDE),
                                       ((cx + sx, cy + e - abs(sx)), 3, GUIDE_SIDE)):
                goal(ex, ey, d, extra)
    elif pad in ("lstick", "rstick"):
        for d in range(4):
            goal(x, y, d, 0)
    else:   # (on its edge, as the game's own lines end: 2 pixels into it)
        e = max(r - 2, 0)
        for (ex, ey), d, extra in (((x, y - e), 2, 0), ((x, y + e), 3, GUIDE_SIDE), ((x - e, y), 0, GUIDE_SIDE),
                                   ((x + e, y), 1, GUIDE_SIDE)):
            if r or d >= 2:
                goal(ex, ey, d, extra)
    start = (0, 0, 0 if bar == 0 else 1)
    best, came, heap = {start: 0}, {}, [(0, start)]
    while heap:
        c, (i, j, d) = heapq.heappop(heap)
        if c > best.get((i, j, d), 1e18):
            continue
        if (i, j, d) in goals:
            (ex, ey), _ = goals[(i, j, d)]
            way, k = [], (i, j, d)
            while k:
                way.append(k)
                k = came.get(k)
            way.reverse()
            pts = [(x0 + g * way[0][0], y0 + g * way[0][1])]
            for p0, p1 in zip(way, way[1:]):
                if p0[2] != p1[2]:
                    pts.append((x0 + g * p0[0], y0 + g * p0[1]))
            pts.append((ex, ey) if len(pts) > 1 or abs(ey - y0) > g else (ex, y0))   # (one leg: on its row)
            if len(pts) > 2:   # (the last leg's corner moved onto the end's line: its leg before only longer or shorter)
                cx_, cy_ = pts[-2]
                pts[-2] = (cx_, ey) if dirs[d][0] else (ex, cy_)
            return _square(pts)
        for nd in range(4):
            if nd == d ^ 1 or (nd != d and j == 0 and abs(i) < 5):   # (no turning back; the label's stub first)
                continue
            ni, nj = i + dirs[nd][0], j + dirs[nd][1]
            if not (ilo <= ni <= ihi and jlo <= nj <= jhi) or blocked[ni - ilo, nj - jlo]:
                continue
            nc = c + g + pen[ni - ilo, nj - jlo] + (GUIDE_BEND if nd != d else 0) + goals.get((ni, nj, nd), (0, 0))[1]
            if nc < best.get((ni, nj, nd), 1e18):
                best[(ni, nj, nd)], came[(ni, nj, nd)] = nc, (i, j, d)
                heapq.heappush(heap, (nc, (ni, nj, nd)))
    return None


def _square(pts):
    """a way's points with every leg level or upright (each point shares x or y with the one before)"""
    out = [pts[0]]
    for x, y in pts[1:]:
        px, py = out[-1]
        if (x, y) == (px, py):
            continue
        if x != px and y != py:
            out.append((px, y) if len(out) > 1 and out[-2][0] == px else (x, py))
        out.append((x, y))
    return out


def guide_draw(size, way, dot):
    """a line along its way (6 pixels thick, square corners), ending on a dot"""
    from PIL import Image, ImageDraw
    im = Image.new("L", size, 0)
    d = ImageDraw.Draw(im)
    for (x0, y0), (x1, y1) in zip(way, way[1:]):
        x0, x1, y0, y1 = round(min(x0, x1)), round(max(x0, x1)), round(min(y0, y1)), round(max(y0, y1))
        d.rectangle((x0 - 3, y0 - 3, x1 + 2, y1 + 2), fill=255)
    if dot:
        d.regular_polygon((round(way[-1][0]), round(way[-1][1]), 11), 5, rotation=18, fill=255)
    return im


def guide_line(size, bar, row, where, pad, stays):
    """a leader line (L, size) from a bar's row to the pad input's part (where: the picture's parts), ending on a dot"""
    way = guide_route(size, bar, row, where, pad, stays)
    if not way:   # (no way round: straight along the row, then up or down to the part's edge, as guide_route ends)
        x, y, r = guide_at(where[pad])
        e = 0 if pad in ("lstick", "rstick") else max(r - 2, 0)
        y += -e if row < y else e
        way = [(BARS[0] if bar == 0 else BARS[1], row + 3), (x, row + 3), (x, y)]
    return guide_draw(size, way, True)


def guide_frame(size, bar, row, where, group, stays):
    """a group's frame (L, size; a square on its corner, rounded, round its parts) and its line from a bar's row"""
    import math
    from PIL import Image, ImageChops, ImageDraw
    cx, cy, e = guide_frame_at(where, group)
    ss, round_ = 4, 0.36 * e
    a = (e + (math.sqrt(2) - 1) * round_) / math.sqrt(2)
    n = int(2 * a * ss) + 8
    sq = Image.new("L", (n, n), 0)
    ImageDraw.Draw(sq).rounded_rectangle((4, 4, n - 4, n - 4), radius=round_ * ss, outline=255, width=6 * ss)
    sq = sq.rotate(45, Image.BICUBIC, expand=True)
    sq = sq.resize((sq.width // ss, sq.height // ss), Image.LANCZOS)
    im = Image.new("L", size, 0)
    im.paste(sq, (round(cx - sq.width / 2), round(cy - sq.height / 2)))
    way = guide_route(size, bar, row, where, group[0], stays, (cx, cy, e - 3)) or \
        [(BARS[0] if bar == 0 else BARS[1], row + 3), (cx + e - 3 if bar else cx - e + 3, row + 3)]
    return ImageChops.lighter(im, guide_draw(size, way, False))


def game_line(im, bar, row):
    """where the game's line from bar (0 left, 1 right) at row is drawn in im (L, a guide texture): the shape it makes between
    the bars, as it is (the router's route to the same part runs elsewhere: taking that out left pieces of the game's line), a
    little wider for its soft edge. The whole shape the line's pixels touch: a game line touching another between the bars
    would be taken with it (the game's do not touch). None found near the label: an empty mask, said on stderr"""
    from PIL import Image, ImageDraw, ImageFilter
    x0, x1 = BARS[0] + 3, BARS[1] - 3   # (off the bars: they join the lines of one side)
    lit = im.point(lambda v: 255 if v > 24 else 0)
    inner = Image.new("L", im.size, 0)
    inner.paste(lit.crop((x0, 0, x1, im.height)), (x0, 0))
    x = x0 + 2 if bar == 0 else x1 - 3
    y = min((y for y in range(row - 12, row + 13) if 0 <= y < im.height and inner.getpixel((x, y))), key=lambda y: abs(y - row),
            default=None)
    out = Image.new("L", im.size, 0)
    if y is None:
        print(f"prompts: no game line from bar {bar} at row {row}: nothing taken out there", file=sys.stderr)
        return out
    ImageDraw.floodfill(inner, (x, y), 128)
    out.paste(inner.point(lambda v: 255 if v == 128 else 0).filter(ImageFilter.MaxFilter(7)).crop((BARS[0] + 1, 0, BARS[1], im.height)),
              (BARS[0] + 1, 0))
    return out


def guide_layers(game, face):
    """the page's leader lines for this face's picture, from the game's (L images, GUIDES): each texture's lines that stay (the
    Switch face: the game's), and {texture: [(Switch button, {pad input: its line})]}"""
    from PIL import Image, ImageChops
    where, size = parts(face), game[0].size
    keep = []   # (the game's bars, or on the Switch face all the game's but the lines that move)
    for im in game:
        k = Image.new("L", size, 0)
        if face == "switch":
            k.paste(im)
        else:
            k.paste(im.crop((0, 0, BARS[0], size[1])), (0, 0))
            k.paste(im.crop((BARS[1], 0, size[0], size[1])), (BARS[1], 0))
        keep.append(k)
    for tx, bar, row, end in GUIDE_LINES:   # (the lines that stay first: those that move keep off them)
        if end not in SW and face != "switch":
            keep[tx] = ImageChops.lighter(keep[tx], guide_line(size, bar, row, where, end, keep[tx]))
    moving = {}
    for tx, bar, row, end in GUIDE_LINES:
        if end not in SW:
            continue
        stays, frames = keep[tx], {}
        for group in GUIDE_GROUPS:
            if all(p in where for p in group):
                im = guide_frame(size, bar, row, where, group, stays)
                frames.update((p, im) for p in group)
        lines = {pad: frames.get(pad) or (frames["south"] if end == GUIDE_FACE and "south" in frames else
                                          guide_line(size, bar, row, where, pad, stays)) for pad in PADS if pad in where}
        if face == "switch":   # (the game's own line where the pad input's is hookshot's default's: taken out of what stays)
            own = lines[DEFAULT[SW[end]]]
            around = game_line(game[tx], bar, row)
            game_own = ImageChops.multiply(game[tx], around)
            for pad, im in lines.items():
                if im is own:
                    lines[pad] = game_own
            keep[tx] = ImageChops.subtract(keep[tx], around)
        moving.setdefault(tx, []).append((end, lines))
    return keep, moving


def guide_layers_of(b, tex, face):
    """the layers of the page's leader lines: each texture's lines that stay (0xff), then each Switch button's line for each pad
    input, drawn over them (host/prompts.c writes the entries in order)"""
    from PIL import Image, ImageChops
    if not parts(face):
        return []
    game = [decode(b, tex[n]) for n in GUIDES]
    keep, moving = guide_layers(game, face)
    layers = []
    for tx, name in enumerate(GUIDES):
        box = Image.new("L", game[tx].size, 0)   # (every place a line is or may be)
        for im in [game[tx], keep[tx]] + [im for _, lines in moving.get(tx, []) for im in lines.values()]:
            box = ImageChops.lighter(box, im)
        layers.append(Layer(name + ".png", name, 0xFF, 0, keep[tx], box.getbbox()))
        for end, lines in moving.get(tx, []):
            for pad, im in lines.items():
                layers.append(Layer(f"{name}.{end}.{pad}.png", name, SW[end], PADS.index(pad), im, over=name + ".png"))
    return layers


def picture_layers(b, tex, im):
    """the layers putting a face's controller picture (1160x680 RGBA, drawn like Controller_Pro) in place of the Pro Controller:
    the picture, the page's tinted copy (each channel a line of the grey level, fitted on the game's pair) and the shadow (the
    outline's left part squeezed into ControllerSh_Pro1 and blurred, fitted on the game's)"""
    import numpy as np
    from PIL import Image, ImageFilter
    plain, tinted = (np.asarray(decode(b, tex[n]).convert("RGBA")).astype(float) for n in (PICTURES[0], PICTURE_TINT))
    solid = plain[..., 3] > 250
    grey = plain[solid][:, :3].sum(1)
    lines = [np.polyfit(grey, tinted[solid][:, c], 1) for c in range(3)]
    px = np.asarray(im).astype(float)
    level = px[..., :3].sum(2)
    tint = np.dstack([np.clip(np.polyval(lines[c], level), 0, 255) for c in range(3)] + [px[..., 3]])
    sh = tex[PICTURE_SHADOW]
    shadow = im.getchannel("A").crop((40, 0, 480, 680)).resize((sh[2], sh[3])).filter(ImageFilter.GaussianBlur(24))
    shadow = shadow.point(lambda v: round(v * 0.7))
    return [Layer(PICTURES[0] + ".png", PICTURES[0], 0xFF, 0, im),
            Layer(PICTURE_TINT + ".png", PICTURE_TINT, 0xFF, 0, Image.fromarray(tint.astype("uint8"))),
            Layer(PICTURE_SHADOW + ".png", PICTURE_SHADOW, 0xFF, 0, shadow)]


def write_textures(path, entries):
    with open(path, "wb") as f:
        f.write(b"HPTX" + struct.pack("<I", len(entries)))
        for sw, pad, rs in entries:
            f.write(struct.pack("<BBHI", sw, pad, 0, len(rs)))
            for off, data in rs:
                f.write(struct.pack("<II", off, len(data)) + bytes(data))


# ---- the Talk ring (DoCommand_00 in UI/Layout2Archive/Common.*.blarc, a BFFH layout, four copies): one group per Switch button
# an action can be on, N_A/B/X/Y_00, each its base, its big button and the three others' letters (KeyTexFont) at the Switch's
# places. prompts/Common.blarc is the game's file decompressed, prompts/ring.bin what to write over it to put each one where its
# pad input is (host/prompts.c picks the places by the controls): "HPRG", u32 count, then count times: u8 group (HOS_SW_A..Y),
# u8 the Switch button placed (the group's own: its big button and base), u8 place (RING_PLACES), u8 0, u32 runs, the runs
# (u32 offset, u32 length, the bytes)
LAYOUTS = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
RING_PLACES = {"east": (80.0, 0.0), "south": (0.0, -80.0), "north": (0.0, 80.0), "west": (-80.0, 0.0)}
# each group's panes (in the layout's order): its base, the others' letters, its big button (then its five children); the group
# whose big button is at each place gives the base's look (CrossBase_A or _B, mirrored or not) and the button's banner
RING_GROUPS = {"a": (5, {"b": 6, "x": 7, "y": 8}, 9), "b": (16, {"a": 17, "x": 18, "y": 19}, 20),
               "x": (27, {"a": 28, "b": 29, "y": 30}, 31), "y": (38, {"a": 39, "b": 40, "x": 41}, 42)}
RING_AT = {"east": "a", "south": "b", "north": "x", "west": "y"}


def layout_copies(blarc, name):
    """the (start, end) of each BFFH layout in the archive that is the layout `name`"""
    starts, at = [], 0
    while (at := blarc.find(b"BFFH", at)) >= 0:
        starts.append(at)
        at += 4
    return [(a, z) for a, z in zip(starts, starts[1:] + [len(blarc)]) if blarc.find(b"\0" + name.encode() + b"\0RootPane\0", a, z) > 0]


def ring_entries(blarc):
    """ring.bin's entries: [(group, Switch button placed, place, runs)], or None when the layout is not the one known"""
    def pane(k):   # (a pane's record: its transform (rotation, scale, translation) first, then parent, size, material)
        return 0x968 + k * 0x44 - 0x28

    copies = [a for a, _ in layout_copies(blarc, "DoCommand_00")]
    if not copies:
        return None
    lay = blarc[copies[0]:copies[0] + 0x2000]
    if any(struct.unpack_from("<2f", lay, pane(base) + 0x2c) != (320.0, 320.0) for base, _, _ in RING_GROUPS.values()):
        return None
    out = []
    for group, (base, small, btn) in RING_GROUPS.items():
        for place, xy in RING_PLACES.items():
            tb, _, tbtn = RING_GROUPS[RING_AT[place]]
            parts = [(pane(base) + 0x10, lay[pane(tb) + 0x10:pane(tb) + 0x18]),   # (the base's scale, its material)
                     (pane(base) + 0x3c, lay[pane(tb) + 0x3c:pane(tb) + 0x40]),
                     (pane(btn) + 0x1c, lay[pane(tbtn) + 0x1c:pane(tbtn) + 0x24])]   # (the button, its banner and text)
            parts += [(pane(btn + c), lay[pane(tbtn + c):pane(tbtn + c) + 0x24]) for c in range(1, 6)]
            out.append((SW[group], SW[group], list(RING_PLACES).index(place),
                        [[c + o, bytearray(v)] for c in copies for o, v in parts]))
            for other, k in small.items():
                out.append((SW[group], SW[other], list(RING_PLACES).index(place),
                            [[c + pane(k) + 0x1c, bytearray(struct.pack("<2f", *xy))] for c in copies]))
    return out


def write_ring(path, entries):
    with open(path, "wb") as f:
        f.write(b"HPRG" + struct.pack("<I", len(entries)))
        for group, placed, place, rs in entries:
            f.write(struct.pack("<BBBBI", group, placed, place, 0, len(rs)))
            for off, data in rs:
                f.write(struct.pack("<II", off, len(data)) + bytes(data))


# ---- the ocarina's hints ("Play a note" and the notes' icons: PaGuideKeyOcarinaInput_00 and _01, and PaGuideKeyCapture_00 while a
# song is taught to the scarecrow, in the same archive, four copies each): each icon a text pane of its own, drawn from its left,
# 90 apart, room for the game's icons. A wider one (the Xbox's LB and RB pills) runs into the next, so host/prompts.c moves each
# icon on by the extra width of the icons bound before it: the last one stays, the others and the text move left. prompts/<face>/hint.bin: "HPHN", u32 count of PADS, each pad input's icon's extra width (f32, layout units), u32
# panes, then each: u32 offset (its x in prompts/Common.blarc), u8 slot (0-4 the notes' icons in the hint's order, 5 the text),
# u8 0 x3, f32 its x
HINTS = ("PaGuideKeyOcarinaInput_00", "PaGuideKeyOcarinaInput_01", "PaGuideKeyCapture_00")
HINT_PANES = ["T_Icon_00", "T_Icon_01", "T_Icon_02", "T_Icon_03", "T_Icon_04", "T_Guide_00"]
HINT_FITS = 720   # (the widest advance the game's spacing holds: its own shoulder icons; font units)
HINT_SCALE = 0.1   # (layout units per font unit: the icons' panes draw the font at 100 per 1000 em)


def layout_panes(lay):
    """a BFFH layout's panes: {name: its record's offset} (the record: rotation, scale at +0x10, translation at +0x1c, size at
    +0x2c); the header gives the offsets of the names' table (+0x118), the records' table (+0x120) and the count (+0x130)"""
    names, recs = struct.unpack_from("<QQ", lay, 0x118)
    n = struct.unpack_from("<I", lay, 0x130)[0]
    out = {}
    for k in range(n):
        name = struct.unpack_from("<Q", lay, names + 8 * k)[0]
        out[lay[name:lay.index(b"\0", name)].decode()] = struct.unpack_from("<Q", lay, recs + 8 * k)[0]
    return out


def hint_panes(blarc):
    """the hint's panes in every copy: [(offset of an x, slot, x)], or None when the layouts are not the ones known. Each pane's
    x, and the keys of its x in the layout's animation (its "Type" sets the text's and the first two icons' x, the same in every
    state): a curve's values are an array (u64 offset of the floats, u32 count, u32 4, the floats) of (value, slope) pairs, taken
    when every value is a pane's x and every slope 0"""
    out = []
    for name in HINTS:
        for a, z in layout_copies(blarc, name):
            lay = blarc[a:z]
            panes = layout_panes(lay)
            if any(p not in panes for p in HINT_PANES):
                return None
            xs = {}
            for slot, p in enumerate(HINT_PANES):
                x = struct.unpack_from("<f", lay, panes[p] + 0x1c)[0]
                xs.setdefault(x, slot)
                out.append((a + panes[p] + 0x1c, slot, x))
            for o in range(0, len(lay) - 16, 8):
                at, n, size = struct.unpack_from("<QII", lay, o)
                if at != o + 16 or size != 4 or not 2 <= n <= 64 or n % 2 or o + 16 + 4 * n > len(lay):
                    continue
                v = struct.unpack_from(f"<{n}f", lay, at)
                if v[0] in xs and all(k == v[0] for k in v[::2]) and not any(v[1::2]):
                    out += [(a + at + 8 * k, xs[v[0]], v[0]) for k in range(n // 2)]
    return out or None


def write_hint(path, widths, panes):
    """widths: the face's icons' advances, per pad input (faces_font; the keyboard's: per key code)"""
    with open(path, "wb") as f:
        f.write(b"HPHN" + struct.pack("<I", len(widths)))
        f.write(b"".join(struct.pack("<f", round(max(0, w - HINT_FITS) * HINT_SCALE)) for w in widths))
        f.write(struct.pack("<I", len(panes)) + b"".join(struct.pack("<IB3xf", at, slot, x) for at, slot, x in panes))


def dump(out, face, b, tex, layers):
    """the face's layers as PNG, and the game's textures they go into (once)"""
    os.makedirs(os.path.join(out, face), exist_ok=True)
    os.makedirs(os.path.join(out, "game"), exist_ok=True)
    for layer in layers:
        layer.im.save(os.path.join(out, face, layer.name))
        game = os.path.join(out, "game", layer.texture + ".png")
        if not os.path.exists(game):
            decode(b, tex[layer.texture]).save(game)


PROMPT_KEY = ".prompt_key"
# host/prompts.c writes these at run time, without changing the build's originals.
PROMPT_RUNTIME = {"External.bfotf.zs", "Common.bntx.zs", "Common.key", "Common.blarc.zs", "Common.blarc.key", "rsizetable.mod.zs"}
PROMPT_PACKAGES = {"fontTools": "fonttools", "PIL": "pillow", "astc_encoder": "astc-encoder-py",
                   "texture2ddecoder": "texture2ddecoder", "numpy": "numpy", "zstandard": "zstandard", "archspec": "archspec"}
PROMPT_ENV = ("SOURCE_DATE_EPOCH", "FREETYPE_PROPERTIES", "ARCHSPEC_CPU_DIR", "ARCHSPEC_EXTENSION_CPU_DIR",
              "NPY_DISABLE_CPU_FEATURES")


def prompt_file(path):
    """Size and content, not just mtime: an input restored with its old time can still have changed."""
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            size += len(block)
            h.update(block)
    return [size, h.hexdigest()]


def prompt_tree(folder, suffix=None, ignore=()):
    """Files and directories, including empty face directories (the keyboard's is a runtime marker)."""
    result = {}
    try:
        os.stat(folder)
    except FileNotFoundError:   # an optional override directory may not exist
        return result
    def unreadable(error):
        raise error
    for d, dirs, files in os.walk(folder, onerror=unreadable):
        for name in sorted(dirs + files):
            p = os.path.join(d, name)
            rel = os.path.relpath(p, folder).replace(os.sep, "/")
            if rel in ignore:
                continue
            if os.path.islink(p) and os.path.isdir(p):
                raise OSError(f"cannot fingerprint linked directory: {p}")
            if os.path.isdir(p):
                if suffix is None:
                    result[rel] = None
            elif suffix is None or name.endswith(suffix):
                result[rel] = prompt_file(p)
    return result


def prompt_packages():
    """The optional packages' usable presence and versions, including their native drawing/encoding libraries."""
    result = {}
    for module, package in PROMPT_PACKAGES.items():
        try:
            version = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            version = None
        try:
            importlib.import_module(module)
            if version is None:
                raise OSError(f"{module} has no package version")
            result[module] = [version, True]
        except ImportError as e:
            result[module] = [version, False, e.name]
    if result["PIL"][1]:
        from PIL import features
        result["Pillow libraries"] = {n: features.version(n) for n in features.get_supported()}
    if result["astc_encoder"][1]:
        result["ASTC backend"] = sorted(n for n in sys.modules if n.startswith("astc_encoder._encoder_"))
    if result["zstandard"][1]:
        import zstandard
        result["zstd"] = list(zstandard.ZSTD_VERSION)
    return result


def prompt_key(a):
    """Everything generate() reads; keep this list with its file opens/imports and tests/test_prompt_skip.py."""
    source = {n: prompt_file(os.path.join(HERE, n)) for n in ("prompt_assets.py", "prompt_draw.py")}
    tables = {n: prompt_file(os.path.join(HERE, n)) for n in ("prompt_faces.py", "prompt_keys.py")}
    fonts = {FONT: prompt_file(os.path.join(a.romfs, FONT))}
    try:
        fonts[prompt_keys.LABEL_FONT] = prompt_file(os.path.join(a.romfs, prompt_keys.LABEL_FONT))
    except FileNotFoundError:   # generate() uses Kenney's labels without the game's bold font
        fonts[prompt_keys.LABEL_FONT] = None
    for folder in (KENNEY, KENNEY_KEYBOARD):
        fonts[os.path.basename(folder)] = prompt_tree(folder, ".otf")
    game = {n: prompt_file(os.path.join(a.romfs, n)) for n in ("Pack/ZsDic.pack.zs", TEXTURES, LAYOUTS)}
    folder = os.path.join(a.romfs, "System", "Resource")
    tables.update({n: prompt_file(os.path.join(folder, n)) for n in sorted(os.listdir(folder))
                   if n.startswith("ResourceSizeTable.") and n.endswith(".rsizetable.zs")})
    switches = {n: os.environ.get(n) for n in PROMPT_ENV}
    # A custom archspec database can select another ASTC binary without changing the package's version.
    for n in ("ARCHSPEC_CPU_DIR", "ARCHSPEC_EXTENSION_CPU_DIR"):
        if n in os.environ:
            switches[n] = [os.environ[n], prompt_tree(os.environ[n], ".json")]
    return {"source": source, "font": fonts, "table": tables, "game assets": game,
            "overrides": prompt_tree(os.path.join(HERE, "prompt_faces"), ".png"), "packages": prompt_packages(),
            "switch": switches, "parameters": [a.faces, a.no_textures, a.dump],
            "Python/platform": [sys.version, sys.platform, platform.machine(), sys.byteorder]}


def prompt_outputs(out):
    return prompt_tree(out, ignore=PROMPT_RUNTIME | {PROMPT_KEY})


def prompts_before(out, key):
    """None means the complete outputs still match; otherwise the reason they need making again."""
    try:
        try:
            f = open(os.path.join(out, PROMPT_KEY), encoding="utf-8")
        except FileNotFoundError:
            return "no stamp yet"
        with f:
            note = json.load(f)
        if not isinstance(note, dict) or not isinstance(note.get("inputs"), dict) or not isinstance(note.get("outputs"), dict):
            return "invalid stamp"
        for kind, value in key.items():
            if note["inputs"].get(kind) != value:
                return kind + " changed"
        if note["inputs"] != key:
            return "inputs changed"
        if "External.bfotf" not in note["outputs"] or note["outputs"] != prompt_outputs(out):
            return "output missing or changed"
    except (OSError, ValueError):
        return "stamp or outputs unreadable"
    return None


def prompt_directory_matches(expected, actual):
    return (stat.S_ISDIR(actual.st_mode) and not getattr(actual, "st_file_attributes", 0) & 0x400
            and expected.st_ino != 0 and os.path.samestat(expected, actual))


@contextlib.contextmanager
def prompt_owner(folder, create=False):
    """Hold the owner's lock; POSIX sweepers pass the already checked directory descriptor."""
    with contextlib.ExitStack() as handles:
        relative = {}
        if os.name != "nt":
            directory = folder
            if not isinstance(directory, int):
                directory = os.open(folder, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                handles.callback(os.close, directory)
            path, relative = ".owner", {"dir_fd": directory}
        else:
            path = os.path.join(folder, ".owner")
        if not create:
            st = os.stat(path, follow_symlinks=False, **relative)
            if not stat.S_ISREG(st.st_mode) or getattr(st, "st_file_attributes", 0) & 0x400:
                raise OSError("not a regular owner file")
        flags = os.O_RDWR | (os.O_CREAT | os.O_EXCL if create else 0) | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(path, flags, 0o600, **relative), "r+b") as owner:
            if not create and (not st.st_ino or not os.path.samestat(st, os.fstat(owner.fileno()))):
                raise OSError("owner file changed")
            if create:
                owner.write(b"\0")
                owner.flush()
            owner.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(owner.fileno(), msvcrt.LK_LOCK if create else msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(owner, fcntl.LOCK_EX | (0 if create else fcntl.LOCK_NB))
            if create:
                owner.write(b"1")   # readiness is published only while the lock is held
                owner.flush()
            elif owner.read(1) != b"1":
                raise OSError("ownership not established")
            yield   # closing releases the lock, including on an exception; process death releases it too


def remove_old_prompt_staging(path, modified):
    """Windows: age alone authorizes a cleanup attempt; an OS refusal is nonfatal."""
    if time.time() - modified >= 24 * 60 * 60:
        try:
            shutil.rmtree(path)
        except OSError as e:
            print(f"prompts: kept staging directory {path} ({e})", flush=True)


def sweep_prompts(out):
    """Reclaim staging: checked descriptors/owner locks on POSIX, a 24-hour age rule on Windows."""
    out = os.path.abspath(out)
    parent = os.path.dirname(out)
    anchored = os.name != "nt"
    if anchored and (not getattr(shutil.rmtree, "avoids_symlink_attacks", False) or
                     not hasattr(os, "O_DIRECTORY") or not hasattr(os, "O_NOFOLLOW")):
        print("prompts: staging cleanup skipped (descriptor-safe removal unavailable)", flush=True)
        return
    try:
        with contextlib.ExitStack() as handles:
            parent_fd = None
            if anchored:
                parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                handles.callback(os.close, parent_fd)
            with os.scandir(parent_fd if anchored else parent) as entries:
                for entry in entries:
                    path = os.path.join(parent, entry.name)
                    if not entry.name.startswith(".prompts-") or path == out:
                        continue
                    try:
                        if not entry.is_dir(follow_symlinks=False):
                            continue
                        st = entry.stat(follow_symlinks=False)
                        if not stat.S_ISDIR(st.st_mode) or getattr(st, "st_file_attributes", 0) & 0x400:
                            continue
                        if not anchored:
                            remove_old_prompt_staging(path, st.st_mtime)
                            continue
                        if not st.st_ino:
                            raise OSError("staging directory identity unavailable")
                        with contextlib.ExitStack() as candidate:
                            directory = os.open(entry.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent_fd)
                            candidate.callback(os.close, directory)
                            if not prompt_directory_matches(st, os.fstat(directory)):
                                raise OSError("staging directory changed")
                            try:
                                os.stat(".owner", dir_fd=directory, follow_symlinks=False)
                            except FileNotFoundError:
                                if time.time() - st.st_mtime < 24 * 60 * 60:
                                    continue   # legacy, unmarked directories get a full day's grace
                            else:
                                with prompt_owner(directory):
                                    pass
                            # Recheck the listed name; rmtree also performs descriptor-relative lstat/open/fstat checks.
                            if not prompt_directory_matches(st, os.stat(entry.name, dir_fd=parent_fd, follow_symlinks=False)):
                                raise OSError("staging directory changed before removal")
                            shutil.rmtree(entry.name, dir_fd=parent_fd)
                    except Exception as e:
                        print(f"prompts: kept staging directory {path} ({e})", flush=True)
    except FileNotFoundError:
        pass   # the first build's output parent may not exist yet
    except OSError as e:
        print(f"prompts: staging cleanup skipped ({e})", flush=True)


def make_prompts(a, incremental=False):
    """Generate in a fresh sibling directory; only complete outputs can replace the previous set."""
    from kit import env_on
    sweep_prompts(a.out)
    key = None
    reason = "packaged or direct build"
    if incremental:
        try:
            key = prompt_key(a)   # before the drawing, never certify old output with a key taken only afterwards
            reason = prompts_before(a.out, key)
        except (OSError, ValueError) as e:
            reason = f"input unreadable: {e}"
            print(f"prompts: {reason}; regenerating without a stamp", flush=True)
    if env_on("HOOKSHOT_FORCE_PROMPTS"):
        reason = "forced"
    if reason is None:
        # Every build used to clear these along with the originals. Their runtime keys do not cover host/prompts.c.
        try:
            for name in sorted(PROMPT_RUNTIME):
                try:
                    os.remove(os.path.join(a.out, name))
                except FileNotFoundError:
                    pass
        except OSError as e:
            reason = f"runtime files could not be cleared: {e}"
            print(f"prompts: {reason}; regenerating", flush=True)
        else:
            print("prompts: unchanged, kept", flush=True)
            return
    stamp = os.path.join(a.out, PROMPT_KEY)
    try:
        os.remove(stamp)   # an interrupted retry must not leave even the previous stamp behind
    except FileNotFoundError:
        pass
    out = a.out
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".prompts-", dir=os.path.dirname(os.path.abspath(out))) as temp, prompt_owner(temp, create=True):
        a.out = os.path.join(temp, "new")
        try:
            generate(a)
            outputs = prompt_outputs(a.out)
            if "External.bfotf" not in outputs or any(face not in outputs for face in a.faces.split(",")):
                raise OSError("prompts: incomplete generated output")
            if key is not None:
                try:
                    stable = key == prompt_key(a)
                except (OSError, ValueError):
                    stable = False
                if stable:
                    with open(os.path.join(a.out, PROMPT_KEY), "w", encoding="utf-8") as f:
                        json.dump({"inputs": key, "outputs": outputs}, f, sort_keys=True)
                else:
                    reason += "; inputs changed or unreadable during generation, not stamped"
            previous = os.path.join(temp, "previous")
            if os.path.exists(out):
                os.replace(out, previous)
            try:
                os.replace(a.out, out)
            except BaseException:
                if os.path.exists(previous):
                    os.replace(previous, out)
                raise
        finally:
            a.out = out
    print(f"prompts: regenerated ({reason})", flush=True)


def generate(a):
    dictionary = zs_dictionary(a.romfs)
    game_font = read_zs(a.romfs, FONT, dictionary)
    key, otf = unwrap(game_font)
    textures = None
    if not a.no_textures:
        try:
            import astc_encoder  # noqa: F401
            import PIL  # noqa: F401
            import texture2ddecoder  # noqa: F401
            textures = read_zs(a.romfs, TEXTURES, dictionary)
        except ImportError as e:   # (the module that is missing: a package's own dependency too, astc_encoder's archspec)
            print(f"prompts: the textures keep the Switch's icons (no {e.name or e})", file=sys.stderr)
    if textures:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "Common.bntx"), "wb") as f:
            f.write(textures)
        tex = bntx_textures(textures)
    blarc = read_zs(a.romfs, LAYOUTS, dictionary)
    hint = hint_panes(blarc)
    if not hint:
        print("prompts: the ocarina's hint keeps the game's spacing (its layout is not the one known)", file=sys.stderr)
    faces = a.faces.split(",")
    if faces != ALL_FACES[:len(faces)]:
        raise SystemExit("prompts: --faces must be the Switch, then prompt_faces.FACES in their order, then the keyboard (HOS_PROMPT_*)")
    try:   # (the keyboard face's word keys are set in the game's own bold font; without it, Kenney's keys)
        label = unwrap(read_zs(a.romfs, prompt_keys.LABEL_FONT, dictionary))[1]
    except OSError:
        label = None
    plain, widths = faces_font(otf, faces, label)
    font = wrap(key, plain)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "External.bfotf"), "wb") as f:
        f.write(font)
    for face in faces:
        d = os.path.join(a.out, face)
        os.makedirs(d, exist_ok=True)
        if hint:
            write_hint(os.path.join(d, "hint.bin"), widths[face], hint)
        if textures and face != "keyboard":   # (the keyboard's: the Switch's textures, host/prompts.c texture_face)
            layers = face_layers(textures, tex, face, otf)
            write_textures(os.path.join(d, "textures.bin"), face_textures(textures, tex, layers))
            if a.dump:
                dump(a.dump, face, textures, tex, layers)
        print(f"prompts: {face}", file=sys.stderr)
    ring = ring_entries(blarc)
    if ring or hint:
        with open(os.path.join(a.out, "Common.blarc"), "wb") as f:
            f.write(blarc)
    if ring:
        write_ring(os.path.join(a.out, "ring.bin"), ring)
        print("prompts: the Talk ring", file=sys.stderr)
    else:
        print("prompts: the Talk ring keeps the Switch's places (its layout is not the one known)", file=sys.stderr)
    table, path = size_table(a.romfs, dictionary, len(game_font), len(font)), os.path.join(a.out, "rsizetable.zs")
    if table:
        import zstandard
        with open(path, "wb") as f:
            f.write(zstandard.ZstdCompressor(level=3).compress(table))
        print(f"prompts: the size table lets the font have {len(font)} bytes", file=sys.stderr)
    elif os.path.exists(path):
        os.remove(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--faces", default=",".join(ALL_FACES))
    ap.add_argument("--no-textures", action="store_true")
    ap.add_argument("--dump", help="also write every face's images as PNG into <dir>/<face>/ (the names nx2/prompt_faces/<face>/ "
                    "takes in their place), and the game's textures they go into into <dir>/game/")
    ap.add_argument("--incremental", action="store_true", help="keep complete, unchanged output (source-checkout builds)")
    a = ap.parse_args()
    make_prompts(a, a.incremental and not a.dump)   # a layer dump is requested output too: always draw it


if __name__ == "__main__":
    main()
