"""(hookshot) The button-prompt faces: everything that differs between controllers, in one place (nx2/prompt_assets.py builds them).

To add a face: an entry in FACES below, its HOS_PROMPT_* in gpu/vk/vk_api.h (in this order, after the Switch), its name in
host/prompts.c's faces[], and the Insert menu's names for it (gpu/vk/vk_menu.cpp tx_prompts, pad_name;
tests/test_prompt_font_bind.py checks the order). To change one: its entry. To rework a texture by hand: nx2/prompt_assets.py --dump <dir> writes
every image the build makes, as PNG, under the names it looks for in nx2/prompt_faces/<face>/: a PNG put there takes that image's
place (nx2/prompt_faces/README.md).

Each face:
  font     Kenney's Input Prompts font its icons come from (third_party/kenney_input)
  prefix   the glyph names' prefix in that font
  icons    each pad input's glyph name after the prefix ("/name": the whole name); an input left out keeps the game's icon. No
           Guide: the makers' logos are their trademarks, every face keeps the game's Home icon.
  drawing  the controller in the pause menu's controls page (System), drawn at the build like the game's Pro Controller
           (nx2/prompt_draw.py): (Kenney font, its controller glyph: the body; None, None: the Steam Deck's own shape) and each
           control (kind, centre x, y in the body's box 0..1, size: a fraction of the body's width, or (w, h) for a "pad" or
           "screen", the pad input whose label it shows, None: none): "stick", "dpad" (the Pro's own, scaled), "face", "small" (a
           disc with the label; none: a blank button), "pad" (a touchpad), "screen". The build makes the page's tinted copy and
           the shadow from it. None: the game's Pro Controller with this face's labels on its face buttons.
  parts    where each pad input is in the picture, for the page's leader lines (picture pixels): (x, y, r), a stick's centre (its
           line ends on it), a button's centre and radius (its line ends just off the edge it comes from), a shoulder's top
           edge (r 0). The four face buttons are framed. Not a pad input's: boxes the lines keep off (x0, y0, x1, y1: a
           screen, a touchpad). SWITCH_PARTS: the game's Pro Controller's.
"""
import os

OVERRIDES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_faces")

# the d-pad's arms, the same names in each of Kenney's fonts (the outlined pad with the arm meant filled in)
_DPAD = {"up": "dpad_up_outline", "down": "dpad_down_outline", "left": "dpad_left_outline", "right": "dpad_right_outline"}


def _parts(axis, sticks, dpad, face, small, home, shoulders, **boxes):
    """a picture's parts: axis (x + its mirror's x), sticks ((x, y, r) left, right), dpad (x, y, an arm's distance, its r), face
    ((x, y) north, west, east, south, r), small ((x, y, r) back; start mirrored), home (x, y, r), shoulders ((x, y) trigger,
    bumper; the right ones mirrored), boxes (the lines keep off)"""
    (ls, rs), (dx, dy, arm, ar) = sticks, dpad
    (n, w, e, s, fr), (bx, by, br) = face, small
    (tx, ty), (sx, sy) = shoulders
    return dict(lstick=ls, rstick=rs, up=(dx, dy - arm, ar), down=(dx, dy + arm, ar), left=(dx - arm, dy, ar),
                right=(dx + arm, dy, ar), north=n + (fr,), west=w + (fr,), east=e + (fr,), south=s + (fr,), back=(bx, by, br),
                start=(axis - bx, by, br), guide=home, ltrigger=(tx, ty, 0), lshoulder=(sx, sy, 0), rtrigger=(axis - tx, ty, 0),
                rshoulder=(axis - sx, sy, 0), **boxes)


SWITCH_PARTS = _parts(1159, ((348, 227, 48), (683, 348, 48)), (455, 348, 45, 17),
                      ((800, 168), (731, 227), (869, 227), (800, 286), 32), (477, 161, 19), (631, 227, 21), ((375, 39), (290, 60)))

FACES = {
    "xbox": dict(
        font="kenney_input_xbox_series.otf", prefix="xbox_",
        drawing=("kenney_input_xbox_series.otf", "controller_xboxseries", [
            ("stick", .215, .262, .158, "lstick"), ("dpad", .345, .530, .160, None), ("stick", .640, .495, .158, "rstick"),
            ("face", .700, .275, .080, "west"), ("face", .775, .375, .080, "south"), ("face", .775, .175, .080, "north"),
            ("face", .850, .275, .080, "east"), ("small", .498, .120, .090, "guide"), ("small", .420, .275, .052, "back"),
            ("small", .576, .275, .052, "start"), ("small", .498, .355, .044, None)]),
        parts=_parts(1156, ((343, 210, 43), (696, 344, 43)), (451, 364, 44, 16), ((809, 160), (746, 218), (871, 218), (809, 275), 31),
                     (513, 218, 19), (578, 129, 34), ((330, 59), (470, 57)), share=(558, 243, 598, 283)),
        icons=dict(_DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
                   start="button_menu", lshoulder="lb", rshoulder="rb", ltrigger="lt", rtrigger="rt", lstick="ls", rstick="rs")),
    "ps4": dict(
        font="kenney_input_playstation_series.otf", prefix="playstation_",
        drawing=("kenney_input_playstation_series.otf", "controller_playstation4", [
            ("dpad", .195, .290, .180, None), ("stick", .340, .520, .150, "lstick"), ("stick", .660, .520, .150, "rstick"),
            ("pad", .499, .200, (.320, .280), None), ("face", .742, .285, .072, "west"), ("face", .814, .175, .072, "north"),
            ("face", .814, .395, .072, "south"), ("face", .886, .285, .072, "east"), ("small", .298, .130, .036, "back"),
            ("small", .702, .130, .036, "start"), ("small", .500, .510, .046, "guide")]),
        parts=_parts(1160, ((447, 357, 40), (713, 357, 40)), (326, 237, 49, 18), ((841, 177), (781, 234), (901, 234), (841, 292), 28),
                     (412, 153, 12), (580, 352, 16), ((320, 67), (408, 97)), touchpad=(446, 117, 712, 263)),
        icons=dict(_DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
                   back="/playstation4_button_share", start="/playstation4_button_options", lshoulder="trigger_l1",
                   rshoulder="trigger_r1", ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "ps5": dict(
        font="kenney_input_playstation_series.otf", prefix="playstation_",
        drawing=("kenney_input_playstation_series.otf", "controller_playstation5", [
            ("dpad", .170, .265, .190, None), ("stick", .320, .490, .145, "lstick"), ("stick", .680, .490, .145, "rstick"),
            ("pad", .498, .175, (.400, .300), None), ("face", .765, .265, .078, "west"), ("face", .840, .150, .078, "north"),
            ("face", .840, .380, .078, "south"), ("face", .915, .265, .078, "east"), ("small", .248, .080, .036, "back"),
            ("small", .750, .080, .036, "start"), ("small", .496, .480, .046, "guide"), ("small", .496, .575, .030, None)]),
        parts=_parts(1158, ((430, 341, 39), (730, 341, 39)), (305, 217, 51, 19), ((863, 153), (800, 217), (925, 217), (863, 281), 30),
                     (370, 114, 12), (577, 336, 16), ((321, 52), (383, 70)), touchpad=(412, 84, 744, 250), mute=(563, 375, 591, 403)),
        icons=dict(_DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
                   back="/playstation5_button_create", start="/playstation5_button_options", lshoulder="trigger_l1",
                   rshoulder="trigger_r1", ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "steamdeck": dict(
        font="kenney_input_steam_deck.otf", prefix="steamdeck_",
        drawing=(None, None, [   # (the real Deck's proportions: Kenney's icon is a shorter one)
            ("screen", .500, .465, (.580, .860), None), ("dpad", .072, .200, .078, None), ("stick", .158, .220, .085, "lstick"),
            ("stick", .842, .220, .085, "rstick"), ("pad", .140, .510, (.112, .270), None), ("pad", .860, .510, (.112, .270), None),
            ("face", .935, .090, .036, "north"), ("face", .935, .270, .036, "south"), ("face", .900, .180, .036, "west"),
            ("face", .970, .180, .036, "east"), ("small", .125, .075, .024, "back"), ("small", .875, .075, .024, "start"),
            ("small", .150, .755, .030, "guide"), ("small", .850, .755, .030, None)]),
        parts=_parts(1159, ((242, 230, 27), (917, 230, 27)), (157, 222, 25, 9), ((1009, 180), (974, 215), (1043, 215), (1009, 249), 16),
                     (209, 174, 9), (234, 437, 12), ((115, 153), (165, 144)), screen=(294, 159, 866, 491),
                     ltrackpad=(169, 290, 279, 394), rtrackpad=(880, 290, 990, 394), more=(908, 420, 942, 454)),
        icons=dict(_DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
                   start="button_options", lshoulder="button_l1", rshoulder="button_r1", ltrigger="button_l2",
                   rtrigger="button_r2")),
}


def override(face, name):
    """the path of a hand-made image taking the place of the build's (name: as prompt_assets.py --dump writes it), or None"""
    path = os.path.join(OVERRIDES, face, name)
    return path if os.path.exists(path) else None


def parts(face):
    """the face's picture's parts (the Switch face: the game's Pro Controller), or None"""
    return SWITCH_PARTS if face == "switch" else FACES[face].get("parts")
