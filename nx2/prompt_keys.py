"""(hookshot) The keyboard face of the button prompts: which icon each key and the sticks show, and the code points they take
(nx2/prompt_draw.py draws them, from Kenney's Input Prompts Keyboard & Mouse font, CC0, third_party/kenney_input_keyboard: the
keys whose names are words and the mouse's buttons drawn there).

The font (nx2/prompt_assets.py faces_font) has, beside the faces' icons, the icon of each key at KEY_BASE + its key code (past
every face's code points: PAD_BASE + FACE_CODES x HOS_PROMPT_N). On the keyboard face host/prompts.c points each Switch button's
code point at the icon of the first key bound to it (hookshot_options.ini key_<button>, the Controls page), as the pad faces point
them at their pad input's. A text rewritten after a change while the game runs (host/prompts.c text_code) gets a code of its own
for each Switch button and key it shows, given out as they come from SLOT_BASE: SLOTS codes no font of the game has (the CJK fonts
use most of the private use area), each pointed at its key's icon when given out.

Data alone: the standard library imports it (the prompt packages are for nx2/prompt_assets.py and nx2/prompt_draw.py, CONTRIBUTING).
"""

KEY_BASE = 0xF7C0   # (host/prompts.c: PAD_BASE + FACE_CODES x HOS_PROMPT_N)
SLOT_BASE, SLOTS = 0xEEC0, 320   # (host/prompts.c; U+EEC0..U+EFFF: in none of the game's fonts, the External font's BIND_BASE after)
FONT = "kenney_input_keyboard_&_mouse.otf"   # (in third_party/kenney_input_keyboard: nx2/prompt_assets.py KENNEY_KEYBOARD)

# Windows virtual-key codes (learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) -> Kenney's glyph name after "keyboard_"
# ("/name": the whole name). The keys whose names are long words have Kenney's symbol for them, which reads at the icons' size.
KEYS = {
    0x01: "/mouse_left", 0x02: "/mouse_right", 0x04: "/mouse_scroll", 0x05: "/mouse_side_back", 0x06: "/mouse_side_forward",
    0x08: "backspace_icon", 0x09: "tab_icon", 0x0D: "return", 0x10: "shift_icon", 0x11: "ctrl", 0x12: "alt", 0x13: "pause",
    0x14: "capslock_icon", 0x1B: "escape", 0x20: "space_icon", 0x21: "page_up", 0x22: "page_down", 0x23: "end", 0x24: "home",
    0x25: "arrow_left", 0x26: "arrow_up", 0x27: "arrow_right", 0x28: "arrow_down", 0x2C: "printscreen", 0x2D: "insert",
    0x2E: "delete", 0x5B: "win", 0x5C: "win", 0x6A: "asterisk", 0x6B: "numpad_plus", 0x6D: "minus", 0x6E: "period",
    0x6F: "slash_forward", 0x90: "numlock", 0x91: "scroll_lock", 0xA0: "shift_icon", 0xA1: "shift_icon", 0xA2: "ctrl",
    0xA3: "ctrl", 0xA4: "alt", 0xA5: "alt", 0xBA: "semicolon", 0xBB: "equals", 0xBC: "comma", 0xBD: "minus", 0xBE: "period",
    0xBF: "slash_forward", 0xC0: "tilde", 0xDB: "bracket_open", 0xDC: "slash_back", 0xDD: "bracket_close", 0xDE: "apostrophe",
}
KEYS.update({0x30 + i: str(i) for i in range(10)})   # the digits
KEYS.update({0x60 + i: str(i) for i in range(10)})   # (the numpad's digits: Kenney has none of their own)
KEYS.update({0x41 + i: chr(0x61 + i) for i in range(26)})   # the letters
KEYS.update({0x70 + i: f"f{i + 1}" for i in range(12)})
UNKNOWN = "any"   # (a key code Kenney has no key for: its "ANY" key)


def kenney_name(code):
    n = KEYS.get(code, UNKNOWN)
    return n[1:] if n.startswith("/") else "keyboard_" + n


# hookshot's default keys for the Switch's buttons, in HOS_SW_* order (A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down): the
# first of each button's keys (host/nn_hid.c key_default), the icons the font has before host/prompts.c points them at the controls
DEFAULT = [0x45, 0x10, 0x20, 0x01, 0x11, 0x43, 0x51, 0x52, 0x5A, 0x58, 0x09, 0x4D, 0x31, 0x32, 0x33, 0x34]


# ---- the keys nx2/prompt_draw.py draws, not Kenney's: the keys whose names are words (Kenney's words are too small to read at the
# icons' size) as wider keys with the word cut out, in the game's own bold font; the mouse's three buttons as a key with a mouse
# cut out of it and that button left white
LABELS = {0x05: "M4", 0x06: "M5", 0x11: "Ctrl", 0x12: "Alt", 0x13: "Pause", 0x1B: "Esc", 0x21: "PgUp", 0x22: "PgDn",
          0x23: "End", 0x24: "Home", 0x2C: "PrtSc", 0x2D: "Ins", 0x2E: "Del", 0x90: "NumLk", 0x91: "ScrLk", 0xA2: "Ctrl",
          0xA3: "Ctrl", 0xA4: "Alt", 0xA5: "Alt"}
LABELS.update({0x60 + i: f"Num{i}" for i in range(10)})
MICE = {0x01: "left", 0x02: "right", 0x04: "middle"}
LABEL_FONT = "Font2/Scalable/ClarimoUDPE-DemiBold.Nin_Ounce_NVN2.bfotf.zs"   # (the game's own: read from the romfs at the build)


# ---- the sticks: the game's stick icons (L or R, with the ways they mean) as the keys that move (WASD: hookshot's defaults,
# host/nn_hid.c key_default) and the mouse that turns the camera. In a text after a change while the game runs, each one's code
# is rewritten to the face's: STICK_KEYS + its place on the keyboard face, STICK_GAME + its place (the game's own icon) on the
# rest (host/prompts.c stick_code[]; U+F8C0..U+F8DF, in none of the game's fonts)
STICKS = [   # (code point, stick, the ways shown: up down left right), in host/prompts.c stick_code order
    (0xE050, "move", "d"), (0xE052, "move", "u"), (0xE054, "move", "lr"), (0xE058, "move", "r"), (0xE05A, "move", "l"),
    (0xE05C, "move", "ud"), (0xE05E, "move", "uldr"),
    (0xE051, "cam", "d"), (0xE053, "cam", "u"), (0xE055, "cam", "lr"), (0xE059, "cam", "r"), (0xE05B, "cam", "l"),
    (0xE05D, "cam", "ud"), (0xE05F, "cam", "uldr"), (0xE068, "cam", "uldr")]
STICK_KEYS, STICK_GAME = 0xF8C0, 0xF8D0
MOVE = {"u": 0x57, "d": 0x53, "l": 0x41, "r": 0x44}
