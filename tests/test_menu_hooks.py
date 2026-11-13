"""The game's menus' mouse (host/oot_menu.c) hooks two functions of the lifted game at their heads: a menu screen's handler of
a pane getting the focus (f_802ed660: hos_oot_menu_focus) and a menu screen's update (f_802f1380: hos_oot_menu_tick). The
button prompts (host/prompts.c) hook a text box's draw (f_8152b670: hos_prompts_text_draw), its layout (f_8152aec0:
hos_prompts_text_layout) and FreeType's glyph render (f_81680c60: hos_prompts_glyph) the same way.

nx2/oot_patches.py's patch_menu_focus, patch_menu_tick, patch_text_draw, patch_text_layout and patch_glyph put each call first
in its function, once, and
declare it; a file without the function is left alone, and a file patched before is given back as it is (a lift patched
again). apply makes them only in the supported build's lift (protect_hearts): those addresses are its functions.

python -I -B tests/test_menu_hooks.py
"""
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

FOCUS = "    hos_oot_menu_focus(c, c->x[0], c->x[1], c->x[30]);"
TICK = "    hos_oot_menu_tick(c, c->x[0]);"
BOX = "    hos_prompts_text_draw(c->x[0]);"
GLYPH = "    hos_prompts_glyph(c->x[1]);"
LAYOUT = "    hos_prompts_text_layout(c->x[0]);"
BODY = "    c->x[19]=c->x[0];\n    c->x[8]=recomp_load64(c,c->x[19] + 56);\nreturn;\n}\n"


def lifted(*heads):
    return '#include "a64.h"\n' + "".join(head + BODY for head in heads)


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def hooked(patch, head, call, decl):
    out = patch(lifted(head))
    check(out is not None and out.count(call) == 1, "the hook is not in its function once: %s" % call.strip())
    check(head + call in out, "the hook is not first in its function: %s" % call.strip())
    check(decl in out, "the lifted file does not declare the hook: %s" % decl)
    check(BODY in out, "the function's body was changed")
    check(patch(out) == out, "a file patched before was patched again: %s" % call.strip())
    check(patch(lifted(head.replace("void f_8", "void f_9"))) is None, "a file without the function was patched")
    return out


def applied(protect_hearts):
    with tempfile.TemporaryDirectory() as gen:
        path = os.path.join(gen, "code_000.c")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(lifted(oot_patches.MENU_FOCUS_HEAD, oot_patches.MENU_TICK_HEAD, oot_patches.TEXT_DRAW_HEAD,
                           oot_patches.TEXT_LAYOUT_HEAD, oot_patches.GLYPH_HEAD))
        stdout = sys.stdout
        sys.stdout = open(os.devnull, "w")
        try:
            oot_patches.apply(gen, protect_hearts=protect_hearts)
        finally:
            sys.stdout.close()
            sys.stdout = stdout
        with open(path, encoding="utf-8", newline="") as f:
            return f.read()


def main():
    hooked(oot_patches.patch_menu_focus, oot_patches.MENU_FOCUS_HEAD, FOCUS,
           "extern void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr);")
    hooked(oot_patches.patch_menu_tick, oot_patches.MENU_TICK_HEAD, TICK,
           "extern void hos_oot_menu_tick(Ctx *c, uint64_t screen);")
    hooked(oot_patches.patch_text_draw, oot_patches.TEXT_DRAW_HEAD, BOX, "extern void hos_prompts_text_draw(uint64_t box);")
    hooked(oot_patches.patch_glyph, oot_patches.GLYPH_HEAD, GLYPH, "extern void hos_prompts_glyph(uint64_t slot);")
    hooked(oot_patches.patch_text_layout, oot_patches.TEXT_LAYOUT_HEAD, LAYOUT,
           "extern void hos_prompts_text_layout(uint64_t box);")
    every = applied(True)
    check(all(every.count(h) == 1 for h in (FOCUS, TICK, BOX, LAYOUT, GLYPH)),
          "the supported build's lift did not get each hook once")
    plain = applied(False)
    check(not any(h in plain for h in (FOCUS, TICK, BOX, LAYOUT, GLYPH)), "another build's lift got the hooks")
    print("PASS: the menus', the text boxes' (draw, layout) and the glyphs' hooks go first in their functions, once, only in the supported build's lift")


if __name__ == "__main__":
    main()
