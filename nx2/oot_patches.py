"""Changes to the lifted C for Ocarina of Time, applied by lift.py after it writes the code (and by hand: python -I nx2/oot_patches.py).

Each is a runtime switch where it can be, so the game's own path stays reachable.

- Dynamic resolution (Lichii's built-in "Max Resolution" patch, main+0x805BD0): the engine's config flag at +0x94 picks the path that
  follows the GPU-time controller; with hos_fixed_resolution set it takes the other path, the 3D at full scale. The controller made
  a new set of render targets at each size, ~11 images a frame at times, and held the GPU near its budget.
- Text speed (the Insert menu's "Text speed"): f_80b1e140 (main+0xafe140) steps a text pane's letter animation, the
  LetterAnimControl of the panes with "LetterAnimOn" user data (dialogue, Navi, signs). Its time argument (s0) moves the
  letters' clock (+0x60 += rate +0x4c * time, the rate from the message's speed tag) and the pauses' timer (+0x58 -= time);
  hos_text_scale multiplies it, 1.0 as shipped. The auto-advance timer (+0x5c, before) keeps the game's time. A huge
  scale shows a page at once (one frame per pause), as the game's own skip on A (+0x72 = 2, f_80794b00) without its flags.
- The fixed path's scale (hos_fixed_scale_bits: the Insert menu's Resolution) reaching the 3D, in the caller f_831f3200
  (main+0x31d3200). It recomputes the 3D's scale (+0xe5c = scale * +0xe58 / +0xe50) only while +0xe2c bit 3 is set. The dynamic
  path sets it on a level change; the fixed path's flag 1 calls f_831f41d0, whose bit 3 the apply block clears in the same frame,
  before any recompute. So the fixed path kept the dynamic path's last scale (0.75 docked: 1536x864 at every setting). Flag 1 now
  also asks for the recompute, and on the fixed path the flags +0xe20 bits 3/4 (a hardcoded 0.75 / 0.5 in place of the scale)
  are not looked at.
- Camera speed (the Insert menu's "Camera speed", the supported build only): the player camera's update (f_80644c80) turns the
  camera each frame by the speeds the right stick's turn functions return in s0; hos_camera_scale multiplies them at each of
  their returns, 1.0 as shipped. Even the fastest of the game's own camera speeds turned slowly for players.
- The OptimizeChangeBank thread's allocations tried again when their heap is full (host/oot_heap_retry.c): f_804566e0's two
  calls of f_80522050, its GPU memory (GPU_ALLOC_SITES), and its two tryAllocs of CPU buffers (HEAP_ALLOC_SITES). They went on
  with a null pointer and crashed writing through it (issue 1697). The supported build only.
- Ocarina note controls: the note-wait action's start (f_828a9190) and tick (f_828a9280) report the wait to the host (hos_oc_wait, hos_oc_beat), which swaps the five notes for its own controls (host/nn_hid.c); a tick whose gates refuse reports no wait, so a dialogue keeps its buttons.
"""
import collections
import glob
import os
import re
import sys
import whole_write   # (rewrites written beside and moved over: issue 1197)

DECL = "extern volatile int hos_fixed_resolution;\nextern volatile int hos_dynres_runs;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
OLD = "if (!((c->x[8] >> 0) & 1)) { goto L_8082640c; }"
NEW = "if (hos_fixed_resolution || !((c->x[8] >> 0) & 1)) { goto L_8082640c; }\n    hos_dynres_runs++;"
LABEL = "L_8082640c:;"
SCALE_OLD = "c->x[9] = 0x3f800000ULL;"
SCALE_NEW = "c->x[9] = hos_fixed_scale_bits;"   # (the fixed path's scale, 1.0 as shipped)
DECL2 = "extern volatile int hos_fixed_resolution;\nextern volatile uint64_t hos_res_module;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
APPLY_OLD = "L_831f3efc:;\n    c->x[9]=recomp_load32(c,c->x[20] + 3628);"
APPLY_NEW = "L_831f3efc:;\n    if (hos_fixed_resolution) {   /* (only while +0xe5c is not the chosen scale: a recompute every frame remade the targets) */\n        union { uint32_t u; float f; } now = {(uint32_t)recomp_load32(c,c->x[20] + 3676)}, want = {hos_fixed_scale_bits},\n            e50 = {(uint32_t)recomp_load32(c,c->x[20] + 3664)}, e58 = {(uint32_t)recomp_load32(c,c->x[20] + 3672)};\n        const float d = now.f - (e50.f != 0 ? want.f * e58.f / e50.f : want.f);\n        if (d > 1e-4f || d < -1e-4f) c->x[8] |= 8;\n    }\n    c->x[9]=recomp_load32(c,c->x[20] + 3628);"
FLAGS_OLD = "L_831f3f08:;\n    c->x[8]=recomp_load64(c,c->x[20] + 3616);\n"
FLAGS_NEW = FLAGS_OLD + "    hos_res_module = c->x[20];\n    if (hos_fixed_resolution) c->x[8] &= ~0x18ULL;\n"


def fixed_scale(t):
    """The fixed path's scale reaching the 3D (the Resolution row). (t, 1) when applied here"""
    if APPLY_OLD not in t or FLAGS_OLD not in t:
        return t, 0
    t = t.replace(APPLY_OLD, APPLY_NEW, 1).replace(FLAGS_OLD, FLAGS_NEW, 1)
    return t.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL2, 1), 1

TEXT_DECL = "extern volatile float hos_text_scale;\nextern void hos_oot_text_pane(Ctx *c, uint64_t anim);\n"
LIFE_BUILD_ID = "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348"
LIFE_HEAD = "void f_804bd8c0(Ctx *__restrict A64_NOESC c) {\n"
MOVE_HEAD = "void f_82c415e0(Ctx *__restrict A64_NOESC c) {\n"
# The new-game opening (host/hos_main.c hos_oot_opening), this build's functions only. The request is still sent.
# The two movies are not started, and the opening's skip query is answered yes.
OPENING_REQUEST_HEAD = "void f_80d9a820(Ctx *__restrict A64_NOESC c) {\n"
OPENING_TIMELINE_HEAD = "void f_83b5bfb0(Ctx *__restrict A64_NOESC c) {\n"
OPENING_QUERY_HEAD = "void f_80de6cb0(Ctx *__restrict A64_NOESC c) {\n"
OPENING_REQUEST = "    hos_oot_opening(c, 0);   /* (nx2/oot_patches.py: skip the new-game opening) */\n"
OPENING_TIMELINE = "    if (hos_oot_opening(c, 1)) return;   /* (nx2/oot_patches.py: the opening's movies) */\n"
OPENING_QUERY = "    if (hos_oot_opening(c, 2)) return;   /* (nx2/oot_patches.py: the opening's skip edge) */\n"
OPENING_DECL = "extern int hos_oot_opening(Ctx *c, int mode);\n"
MOVE_ANCHOR = ("if (((c->x[8] >> 1) & 1)) { goto L_82c41748; }\n")
MOVE_MATRIX = "L_82c41778:;\n"
WARP_HEAD = "void f_806d2530(Ctx *__restrict A64_NOESC c) {\n"
WARP_ANCHOR = "{ uint64_t _t = c->x[8]; c->x[30] = 0x806d25c8ULL; A64_ICALL(c, _t); }\n"
VIEW_HEAD = "void f_804e48c0(Ctx *__restrict A64_NOESC c) {\n"
# (the head of the view's loop: a bare label, since nx2/poll.py puts the interrupt poll before each backward goto; with the
# poll after the label in C lifted before that)
VIEW_ANCHORS = ("L_804e5068:;\n", "L_804e5068:; A64_POLL(c);\n")
LIFE_CAS = ("{ uint64_t _a = c->x[13]; uint32_t _e = (uint32_t)c->x[11]; "
            "__atomic_compare_exchange_n((uint32_t *)A64_PTR(_a), &_e, (uint32_t)c->x[12], "
            "0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); A64_MARK(_a, 4); c->x[11] = (uint64_t)_e; }")


def patch_life(source):
    start = source.find(LIFE_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    if end < 0 or source[start:end].count(LIFE_CAS) != 2:
        return None
    # x19 is the actor. Confirm its type and life pointer before protecting this write.
    guard = ("    if ((uint32_t)c->x[12] < (uint32_t)c->x[11] && hos_oot_hearts_enabled() &&\n"
             "        hos_oot_is_player(c, c->x[19]) &&\n"
             "        c->x[13] == recomp_load64(c,c->x[19] + 7184) + 8)\n"
             "        c->x[12] = c->x[11];\n")
    body = source[start:end].replace(LIFE_CAS, guard + LIFE_CAS)
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern int hos_oot_hearts_enabled(void);\n'
        'extern int hos_oot_is_player(Ctx *c, uint64_t actor);\n', 1)


def patch_movement(source):
    start = source.find(MOVE_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    if end < 0 or source[start:end].count(MOVE_ANCHOR) != 1 or source[start:end].count(MOVE_MATRIX) != 1:
        return None
    bypass = ("    hos_oot_lift_player(c, c->x[19]);\n"
              + MOVE_ANCHOR)
    body = source[start:end].replace(MOVE_ANCHOR, bypass, 1).replace(
        MOVE_MATRIX, MOVE_MATRIX + "    hos_oot_flight_pose(c, c->x[19], c->x[25]);\n", 1)
    source = source[:start] + body + source[end:]
    return source.replace('#include "a64.h"\n', '#include "a64.h"\n'
                          'extern void hos_oot_lift_player(Ctx *c, uint64_t actor);\n'
                          'extern void hos_oot_flight_pose(Ctx *c, uint64_t actor, uint64_t matrix);\n', 1)


def patch_flight_rotation(source):
    start = source.find(WARP_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    body = source[start:end]
    if end < 0 or body.count(WARP_ANCHOR) != 1:
        return None
    body = body.replace(WARP_ANCHOR, "    hos_oot_flight_rotation(c, c->x[0], c->x[1]);\n" + WARP_ANCHOR)
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\n'
        'extern void hos_oot_flight_rotation(Ctx *c, uint64_t controller, uint64_t matrix);\n', 1)


def patch_view(source):
    start = source.find(VIEW_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    body = source[start:end]
    if end < 0:
        return None
    for anchor in VIEW_ANCHORS:
        if body.count(anchor) == 1:
            break
    else:
        return None
    body = body.replace(anchor, anchor + "    hos_oot_free_view(c, c->x[20]);\n")
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_free_view(Ctx *c, uint64_t view);\n', 1)

# Movement speed (the Debug tab's row, host/hos_main.c hos_oot_cct_speed): a character controller's forward speed (s0), on its way
# to *(controller+40)+472, which the physics step moves the body by with its collisions. The host scales it for Link's controller
# while the row is above 1x. Three places set it: f_809552f0, the move Link's own walk, run and swim go through (a direction at
# x1, the speed in metres a frame), at its two calls of the setter, each after it took the controller into x0; and the setter
# itself, f_80559df0 (the controller's vtable +0x70), with f_80964bf0, which takes the CCT component and inlines that setter
# (OneShotPhysicsCctSetForwardSpeedMpf and others). The first inlines the setter too, so no speed is scaled twice. Only a walk's,
# run's or swim's speed is scaled: a climb up a ledge goes through f_809552f0 too, at some 55 m/s for a frame, and scaled it threw
# Link past the ledge.
SPEED_HOOK = ("    hos_oot_cct_speed(c, c->x[0], %d);   /* (nx2/oot_patches.py: movement speed; x0 the controller, s0 the speed, "
              "in metres a %s) */\n")
SPEED_PLACES = (   # (the function's head, the text in it the hook follows, and 1 when s0 is a second's speed, 0 a frame's)
    ("void f_809552f0(Ctx *__restrict A64_NOESC c) {\n", "L_80955424:;\n    c->x[0]=recomp_load64(c,c->x[8] + 8);\n", 0),
    ("void f_809552f0(Ctx *__restrict A64_NOESC c) {\n", "L_809554dc:;\n    c->x[0]=recomp_load64(c,c->x[8] + 8);\n", 0),
    ("void f_80559df0(Ctx *__restrict A64_NOESC c) {\n", "void f_80559df0(Ctx *__restrict A64_NOESC c) {\n", 1),
    ("void f_80964bf0(Ctx *__restrict A64_NOESC c) {\n", "void f_80964bf0(Ctx *__restrict A64_NOESC c) {\n    c->x[0]=recomp_load64(c,c->x[0] + 8);\n", 1),
)


def patch_cct_speed(source):
    """The forward speed's places in this file hooked: (source, how many are hooked now), None when none of them is here"""
    if not any(head in source for head, _, _ in SPEED_PLACES):
        return None
    n = 0
    for head, anchor, per_second in SPEED_PLACES:
        hook = SPEED_HOOK % (per_second, "second" if per_second else "frame")
        start = source.find(head)
        if start < 0:
            continue
        end = source.find("\n}", start)
        body = source[start:end]
        if body.count(anchor + hook) == 1:
            n += 1
        elif end >= 0 and body.count(anchor) == 1:
            source = source[:start] + body.replace(anchor, anchor + hook, 1) + source[end:]
            n += 1
    if n and "extern void hos_oot_cct_speed(" not in source:
        source = source.replace('#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_cct_speed(Ctx *c, uint64_t controller, int per_second);\n', 1)
    return source, n


# Native mouse look (host/mouse_native.c): the player camera's update, f_80644c80 (a virtual method, the camera in x0), hands
# its camera to the host before anything else, on the game's thread, once a frame; with the menu's Mouse camera on the mouse's
# motion turns the camera's yaw (+0x1a4) and pitch (+0x18c) there, before the camera reads its input.
CAM_HEAD = "void f_80644c80(Ctx *__restrict A64_NOESC c) {\n"


def patch_opening(source):
    """The supported build's three functions (a request, the timeline start, the skip query). None when this file has
    none of them. A file patched before is left as it is."""
    heads = ((OPENING_REQUEST_HEAD, OPENING_REQUEST), (OPENING_TIMELINE_HEAD, OPENING_TIMELINE),
             (OPENING_QUERY_HEAD, OPENING_QUERY))
    found = 0
    for head, line in heads:
        start = source.find(head)
        if start < 0:
            continue
        found += 1
        if line not in source:
            at = start + len(head)
            source = source[:at] + line + source[at:]
    if not found:
        return None
    if OPENING_DECL not in source:
        source = source.replace('#include "a64.h"\n', '#include "a64.h"\n' + OPENING_DECL, 1)
    return source


def patch_mouse_camera(source):
    start = source.find(CAM_HEAD)
    if start < 0:
        return None
    if "hos_mouse_camera(c->x[0]);" in source:   # (made before: a lift patched again)
        return source
    at = start + len(CAM_HEAD)
    source = source[:at] + "    hos_mouse_camera(c->x[0]);   /* (nx2/oot_patches.py: native mouse look) */\n" + source[at:]
    return source.replace('#include "a64.h"\n', '#include "a64.h"\nextern void hos_mouse_camera(uint64_t camera);\n', 1)


# The camera's state-3 yaw follow: after f_80642c80's authored heading override (the branch to 0x806492c4), the block at
# 0x8064720c derives a heading from Link's movement and eases yaw (+0x1a4) through f_80642410 at 0x806475e4. Skip only
# the movement path, restoring x25's parameter base as that path does, and join the other yaw paths at 0x80645ad8.
# State 4's stick, state 2's explicit recenter and authored heading overrides stay intact; pitch and collision follow.
CAM_FOLLOW_ANCHOR = "L_8064720c:;\nif (!c->z) { goto L_806492c4; }\n"
CAM_FOLLOW_JOIN = "L_80645ad8:;"
CAM_FOLLOW_HOOK = ("    if (hos_disable_camera_centering()) { c->x[25] = c->x[26]; goto L_80645ad8; }"
                   "   /* (nx2/oot_patches.py: disable camera centering) */\n")


def patch_camera_centering(source):
    start = source.find(CAM_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    if end < 0:
        return None
    body = source[start:end]
    if body.count(CAM_FOLLOW_ANCHOR) != 1 or body.count(CAM_FOLLOW_JOIN) != 1:
        return None
    if CAM_FOLLOW_HOOK in body:
        return source
    body = body.replace(CAM_FOLLOW_ANCHOR, CAM_FOLLOW_ANCHOR + CAM_FOLLOW_HOOK, 1)
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern int hos_disable_camera_centering(void);\n', 1)


MENU_FOCUS_HEAD = "void f_802ed660(Ctx *__restrict A64_NOESC c) {\n"


def patch_menu_focus(source):
    """f_802ed660, a menu screen's handler of a pane getting the focus (host/oot_menu.c), calls
    hos_oot_menu_focus first, with the screen, the pane and its caller's return address"""
    start = source.find(MENU_FOCUS_HEAD)
    if start < 0:
        return None
    if "hos_oot_menu_focus(c," in source:   # (made before: a lift patched again)
        return source
    at = start + len(MENU_FOCUS_HEAD)
    hook = "    hos_oot_menu_focus(c, c->x[0], c->x[1], c->x[30]);   /* (nx2/oot_patches.py: the menus' focus) */\n"
    return (source[:at] + hook + source[at:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr);\n', 1)


MENU_TICK_HEAD = "void f_802f1380(Ctx *__restrict A64_NOESC c) {\n"


def patch_menu_tick(source):
    """f_802f1380, a menu screen's update (each frame, before the screen's UI controller: +56, its vtable's
    +16), calls hos_oot_menu_tick first with the screen: the game's menus' mouse (host/oot_menu.c)"""
    start = source.find(MENU_TICK_HEAD)
    if start < 0:
        return None
    if "hos_oot_menu_tick(c," in source:   # (made before: a lift patched again)
        return source
    at = start + len(MENU_TICK_HEAD)
    hook = "    hos_oot_menu_tick(c, c->x[0]);   /* (nx2/oot_patches.py: the menus' mouse) */\n"
    return (source[:at] + hook + source[at:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_menu_tick(Ctx *c, uint64_t screen);\n', 1)


TEXT_DRAW_HEAD = "void f_8152b670(Ctx *__restrict A64_NOESC c) {\n"
TEXT_LAYOUT_HEAD = "void f_8152aec0(Ctx *__restrict A64_NOESC c) {\n"
GLYPH_HEAD = "void f_81680c60(Ctx *__restrict A64_NOESC c) {\n"


def _hook_head(source, head, call, decl):
    """call put first in the function that starts with head, once, and decl declared; None without the function"""
    start = source.find(head)
    if start < 0:
        return None
    if call.split("(")[0] + "(" in source:   # (made before: a lift patched again)
        return source
    at = start + len(head)
    return (source[:at] + call + source[at:]).replace('#include "a64.h"\n', '#include "a64.h"\n' + decl, 1)


def patch_text_draw(source):
    """f_8152b670, a text box's draw (x0 the box: each time the game shows it), calls hos_prompts_text_draw first with the box: the
    button prompts changed while the game runs, the box's codes for them rewritten and the box marked to be laid out again
    (host/prompts.c)"""
    return _hook_head(source, TEXT_DRAW_HEAD,
                      "    hos_prompts_text_draw(c->x[0]);   /* (nx2/oot_patches.py: the button prompts changed) */\n",
                      "extern void hos_prompts_text_draw(uint64_t box);\n")


def patch_text_layout(source):
    """f_8152aec0, a text box's layout (x0 the box: it lays its text out again when its status says so, at +8: bit 0x20000),
    calls hos_prompts_text_layout first with the box: the button prompts changed while the game runs, the box's codes for them
    rewritten and the box laid out again (host/prompts.c)"""
    return _hook_head(source, TEXT_LAYOUT_HEAD,
                      "    hos_prompts_text_layout(c->x[0]);   /* (nx2/oot_patches.py: the button prompts changed) */\n",
                      "extern void hos_prompts_text_layout(uint64_t box);\n")


def patch_glyph(source):
    """f_81680c60, FreeType's ft_smooth_render (x1 the glyph slot), calls hos_prompts_glyph first with the slot: where the game
    keeps the button prompts' font, for a change while it runs (host/prompts.c)"""
    return _hook_head(source, GLYPH_HEAD,
                      "    hos_prompts_glyph(c->x[1]);   /* (nx2/oot_patches.py: where the button prompts' font is) */\n",
                      "extern void hos_prompts_glyph(uint64_t slot);\n")


ZSTD_HEAD = "void f_80115c80(Ctx *__restrict A64_NOESC c) {\n"
ZSTD_CALL = "    if (hos_zstd_stream(c)) return;   /* (nx2/oot_patches.py: HOS_NATIVE_ZSTD, the host's libzstd) */\n"


def patch_zstd_stream(source):
    """f_80115c80, the game's zstd stream decode of one compressed chunk (x0 the stream object, x1 and x2 the chunk, x3 the
    DDict or 0; its job loop f_80115bf0 calls it for each chunk read), returns at once when hos_zstd_stream did the call:
    with HOS_NATIVE_ZSTD on, the host's libzstd decodes the stream (host/hos_zstd.c); off, the function runs as lifted"""
    start = source.find(ZSTD_HEAD)
    if start < 0:
        return None
    if ZSTD_CALL in source:   # (made before: a lift patched again)
        return source
    at = start + len(ZSTD_HEAD)
    return (source[:at] + ZSTD_CALL + source[at:]).replace('#include "a64.h"\n', '#include "a64.h"\nint hos_zstd_stream(Ctx *c);\n', 1)


TEXT_HEAD = "void f_80b1e140(Ctx *__restrict A64_NOESC c) {"
# (the letters' block: past the early returns, the auto-advance timer and the hidden / no-text checks; reached once a call)
TEXT_OLD = ("L_80b1e1b4:;\n"
            "    c->x[9]=recomp_load16(c,c->x[19] + 110);\n"
            "if ((uint32_t)c->x[9] == 0) { goto L_80b1e18c; }\n")
# (the same block in a build with interrupt polls: nx2/poll.py puts the poll before each backward goto, as a block)
TEXT_OLD_POLLED = TEXT_OLD.replace("{ goto L_80b1e18c; }", "{ { A64_POLL(c); goto L_80b1e18c; } }")
TEXT_ADD = ("    if (hos_text_scale != 1.0f) { float _t; uint32_t _u; memcpy(&_t,c->vreg[0],4); _t *= hos_text_scale; "
            "memcpy(&_u,&_t,4); a64_vset64(c->vreg[0],_u); }   /* (nx2/oot_patches.py: text speed) */\n"
            "    hos_oot_text_pane(c, c->x[19]);   /* (nx2/oot_patches.py: text size; x19 the LetterAnimControl) */\n")
TEXT_NEW = TEXT_OLD + TEXT_ADD


def patch_dynres(s):
    if OLD not in s:
        return None
    s = s.replace(OLD, NEW, 1)
    at = s.index(LABEL)
    scale = s.index(SCALE_OLD, at)
    s = s[:scale] + SCALE_NEW + s[scale + len(SCALE_OLD):]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL, 1)


# The frame's real time in place of the nominal rate, for a Frame rate cap above 60 (host/nn_vi.c hos_frame_seconds_bits; 0, as
# at a cap of 60: the game's own divide, nothing changes). The engine has two places where its integer nominal rate (+0x2c8 of
# the graphics module) becomes a time step, both a float divide "base / nominal":
# - f_80706be0: the frame's step, base 60.0 (30.0 in the half-rate mode), added to the game clock (the double at
#   [x19+0xd58], in 1/60 s) and handed on as the frame's seconds;
# - f_83b6ea90: the global step (60.0 / nominal, a float in .data) the rest of the engine reads.
# The engine's own rate is 60 / <whole display intervals>, never above 60, and its controller puts 60 back over a higher value
# written from outside before either divide runs: above 60 fps every frame still stepped 1/60 s and the game ran fps / 60 times
# too fast. With the host's measured frame time the step is base * seconds, whatever the rate. hos_step_probe: f_80706be0's
# x19, which tells the host the hook is in this build and gives it the game clock for its log (HOS_FPS_LOG=1).
STEP_SITES = (("void f_80706be0(Ctx *__restrict A64_NOESC c) {\n", 8, "\n    hos_step_probe = c->x[19];"),
              ("void f_83b6ea90(Ctx *__restrict A64_NOESC c) {\n", 0, ""))
STEP_OLD = "    _v=a64_fpx_div32_div32(c,_a,_b);\n    a64_vset64(c->vreg[%d],_v);}"
STEP_HOOK = ("    _v=a64_fpx_div32_div32(c,_a,_b);\n"
             "    { const uint32_t _hs = hos_frame_seconds_bits; if (_hs) { float _base, _secs; uint32_t _w; memcpy(&_base,&_a,4); "
             "memcpy(&_secs,&_hs,4); _base *= _secs; memcpy(&_w,&_base,4); _v = _w; } }   /* (base * the frame's seconds: nx2/oot_patches.py) */\n"
             "    a64_vset64(c->vreg[%d],_v);}")
STEP_MARK = "hos_frame_seconds_bits; if (_hs)"   # (once per divide with the hook)
STEP_DECL = "extern volatile uint32_t hos_frame_seconds_bits;\nextern volatile uint64_t hos_step_probe;\n"


def patch_frame_seconds(s):
    """The host's frame time at the step divides of f_80706be0 and f_83b6ea90: (s, the sites patched here). A site whose
    function is not in the file, is patched already, or whose code differs (another game version) is left as it is."""
    done = 0
    for head_text, reg, extra in STEP_SITES:
        head = s.find(head_text)
        if head < 0:
            continue
        end = s.find("\nvoid f_", head + len(head_text))
        body = s[head:end if end >= 0 else len(s)]
        old, new = STEP_OLD % reg, STEP_HOOK % reg + extra
        if new in body or body.count(old) != 1 or body.count("a64_fpx_div32_div32") != 1:
            continue
        at = head + body.index(old)
        s = s[:at] + new + s[at + len(old):]
        done += 1
    if done and STEP_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + STEP_DECL, 1)
    return s, done


def unpatch_frame_seconds(s):
    """s without the hook, as the lift wrote it: for a lift where only one of the two divides was found (apply)"""
    for _, reg, extra in STEP_SITES:
        s = s.replace(STEP_HOOK % reg + extra, STEP_OLD % reg)
    return s if STEP_MARK in s else s.replace(STEP_DECL, "", 1)   # (a hook of another shape stays, with its declarations)


# A movie's frame count above a 60 cap. The game's movie player counts its updates while a movie plays (f_83b7c710: one more
# at +760 of the player each frame), and a cutscene that shows a movie takes that count as its timeline's frame (f_83b5ec10,
# f_83b5e9a0): the narration, the rumble and the cutscene's end are placed by it. The movie itself follows the clock, so above
# 60 fps those cues ran ahead of the picture, fps / 60 times too fast (issue 1742). The count now grows by the frames' time, as
# the game's clock does (hos_movie_frames, host/nn_vi.c: 1 as before while the host gives no frame time).
MOVIE_HEAD = "void f_83b7c710(Ctx *__restrict A64_NOESC c) {\n"
# (the load and the add alone: the store after them is rewritten by the page marks' pass, which runs after these edits)
MOVIE_OLD = ("    c->x[8]=recomp_load32(c,c->x[20] + 760);\n"
             "    { uint64_t _a=c->x[8], _b=1ULL; uint64_t _r=_a+_b; _r&=0xFFFFFFFFULL; c->x[8]=_r; }\n")
MOVIE_NEW = MOVIE_OLD.replace("_b=1ULL", "_b=hos_movie_frames(c->x[20])")
MOVIE_DECL = "uint32_t hos_movie_frames(uint64_t player);\n"


def patch_movie_frames(s):
    """The movie player's count by the frames' time; None when the file has no such function, s unchanged when the function is
    there but its code differs (another game version) or has the patch already."""
    head = s.find(MOVIE_HEAD)
    if head < 0:
        return None
    end = s.find("\nvoid f_", head + len(MOVIE_HEAD))
    body = s[head:end if end >= 0 else len(s)]
    if body.count(MOVIE_OLD) != 1:
        return s
    at = head + body.index(MOVIE_OLD)
    s = s[:at] + MOVIE_NEW + s[at + len(MOVIE_OLD):]
    return s if MOVIE_DECL in s else s.replace('#include "a64.h"\n', '#include "a64.h"\n' + MOVIE_DECL, 1)


# Counters that add the frame step, or the step times a whole number, cut to whole frames (host/frame_whole.h). At 60 fps the step
# is 1; under a Frame rate cap above 60 it is the frame's own time, under 1 (0.42 at 144 fps), and the cut is 0 every frame:
# - f_80a87e50, the pick-up indicator above Link's head (a rupee, a Deku Stick): the count it shows (+864) moves toward the
#   wallet's (+860) by (int)(step * max(distance / 50, 1)) and the indicator fades when they meet. Cut to 0, it stayed at the old
#   count until a door or a talk replaced it (issue 1832).
# - f_800672e0, the water renderer: its ripple simulation runs (int)step times the water's substeps (+76 of the pass) each frame.
#   Cut to 0, the rings Link and the actors make in water stood still above 60 fps. Found by atk3001 for Lichii (zortos293/
#   Lichii#1152), as ported to Fritchii (Frizti/Fritchii d389685a4, 063dbb741): its remainder is kept per water object (x21).
# - f_803edf70, the time of day: each frame it adds (int)(2 * step) game seconds (+60) to the seconds of the day (+40, a day of
#   86400, then the day count +44). Cut, it added 1 a frame below 120 fps (the day slower: 83% at 100) and 0 above 120, where
#   the clock stood still and the sun swung back and forth over the frozen hour.
# hos_frame_whole carries the fractions per counter while the frames' own time is the step, so each moves at the game's rate;
# otherwise it cuts as the game does. A site is added here once it is seen to misbehave in game. (A site's function, its code to
# replace, and the counter's address for its remainder.)
WHOLE_SITES = (
    # (the cut alone, the function's one: the multiply before it is a64_fpx_hot_mul32_fma32 in a lift where the function is
    # among A64_HOT_FUNCS)
    ("void f_80a87e50(Ctx *__restrict A64_NOESC c) {@",
     "    c->x[12]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);@",
     "c->x[19] + 864"),
    # (the cut alone, the function's one)
    ("void f_800672e0(Ctx *__restrict A64_NOESC c) {@",
     "    c->x[9]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);@",
     "c->x[21]"),
    # (the cut with the store after it: the function cuts into x8 the same way once more, in the branch that sets
    #  the time from another source)
    ("void f_803edf70(Ctx *__restrict A64_NOESC c) {@",
     "    c->x[8]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);@"
     "    recomp_store32(c,c->x[19] + 60,c->x[8]);@",
     "c->x[19] + 60"),
)
WHOLE_SITES = tuple((h.replace("@", "\n"), o.replace("@", "\n"), k) for h, o, k in WHOLE_SITES)
WHOLE_MARK = "/* (the step's whole frames: nx2/oot_patches.py) */"
WHOLE_DECL = "extern uint32_t hos_frame_whole(uint64_t at, uint32_t amount_bits);\n"


def whole_new(old, key):
    """old with its cut to whole frames (the one recomp_fp_to_int) made hos_frame_whole's for the counter at key"""
    lines = old.split("\n")
    for i, line in enumerate(lines):
        if "recomp_fp_to_int(c->vreg[0][0]" in line:
            reg = line.split("=", 1)[0]
            lines[i] = "%s=hos_frame_whole(%s, (uint32_t)c->vreg[0][0]);   %s" % (reg, key, WHOLE_MARK)
    return "\n".join(lines)


def patch_whole_frames(s):
    """The counters of WHOLE_SITES take hos_frame_whole's whole frames: (s, the sites patched here). A file without the function,
    one patched before, or one whose code differs (another game version) is left as it is."""
    done = 0
    for head_text, old, key in WHOLE_SITES:
        head = s.find(head_text)
        if head < 0:
            continue
        end = s.find("\n}\n", head)
        body = s[head:end if end >= 0 else len(s)]
        new = whole_new(old, key)
        if new in body or body.count(old) != 1:
            continue
        at = head + body.index(old)
        s = s[:at] + new + s[at + len(old):]
        done += 1
    if done and WHOLE_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + WHOLE_DECL, 1)
    return s, done


# Above a Frame rate cap of 60, two places that need whole 60 fps frames (host/oot_high_fps.h has why and the logic; at a cap of
# 60 the host hands back what the game computed):
# - the sound manager's job, f_80435300 (phase in w2; phase 0 loads the game step, [[slot]]+0x10, and hands it to the sound
#   engine): its head asks hos_oot_sound_gate whether to run now (60 Hz above 60 fps, the other phases following phase 0), and
#   the step it loads goes through hos_oot_sound_step (a whole one while the gate holds the job). After zortos293/Lichii PR #1289
#   (auz466), which does the same as a code patch for the emulator.
# - the physics solver's step count, set each update from the frame step in f_838ceb80 and f_838cfc10 (the same code in both):
#   hos_oot_solver_steps rounds it up instead of down, at least 1, so Link stays on moving platforms (issue 1798).
SOUND_HEAD = "void f_80435300(Ctx *__restrict A64_NOESC c) {\n"
SOUND_SLOT = "g_module_base + 0x80435000ULL + (int64_t)75927552"   # (the page of the step's pointer, whose +2192 holds it)
SOUND_OLD = ("    c->x[8]=" + SOUND_SLOT + ";\n"
             "    { uint64_t _r = ((uint64_t)0 | ((c->x[0]) << 0)); c->x[19] = _r; }\n"
             "    c->x[8]=recomp_load64(c,c->x[8] + 2192);\n"
             "    c->x[8]=recomp_load64(c,c->x[8] + 0);\n"
             "    { { uint64_t _v=recomp_load32(c,(c->x[8]+16ULL)); a64_vset64(c->vreg[0],_v); } }\n")
SOUND_NEW = SOUND_OLD.replace("uint64_t _v=recomp_load32(c,(c->x[8]+16ULL));",
                              "uint64_t _v=hos_oot_sound_step((uint32_t)recomp_load32(c,(c->x[8]+16ULL)));")
SOUND_GATE = ("    if (!hos_oot_sound_gate(c, (uint32_t)c->x[2], " + SOUND_SLOT + " + 2192)) return;"
              "   /* (nx2/oot_patches.py: the sound job at 60 Hz above 60 fps) */\n")
SOUND_DECL = "extern int hos_oot_sound_gate(Ctx *c, uint32_t phase, uint64_t slot);\nextern uint32_t hos_oot_sound_step(uint32_t step_bits);\n"
SOLVER_HEADS = ("void f_838ceb80(Ctx *__restrict A64_NOESC c) {\n", "void f_838cfc10(Ctx *__restrict A64_NOESC c) {\n")
# (the cut of frame step * steps a frame + 0.001, then its store into the solver: vreg 0 the float cut, vreg 1 the steps a frame)
SOLVER_OLD = ("    c->x[1]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);\n"
              "    recomp_store32(c,c->x[0] + 712,c->x[1]);\n")
SOLVER_NEW = SOLVER_OLD.replace(
    "    recomp_store32(", "    c->x[1]=hos_oot_solver_steps((uint32_t)c->x[1], (uint32_t)c->vreg[0][0], (uint32_t)c->vreg[1][0]);"
    "   /* (nx2/oot_patches.py: solver steps rounded up above 60 fps) */\n    recomp_store32(", 1)
SOLVER_DECL = "extern uint32_t hos_oot_solver_steps(uint32_t game, uint32_t scaled_bits, uint32_t per_frame_bits);\n"


def _function(s, head_text):
    """(start, end) of the function whose head is head_text, or None"""
    head = s.find(head_text)
    if head < 0:
        return None
    end = s.find("\nvoid f_", head + len(head_text))
    return head, end if end >= 0 else len(s)


def patch_sound_job(s):
    """The sound job's gate and step: (s, 1) when the job is in s with them (made now or before), (s, 0) when it is not here or
    its code differs (another game version: left as it is)"""
    at = _function(s, SOUND_HEAD)
    if at is None:
        return s, 0
    head, end = at
    body = s[head:end]
    if body.count(SOUND_NEW) == 1 and body.count(SOUND_GATE) == 1:
        return s, 1
    if body.count(SOUND_OLD) != 1 or SOUND_GATE in body:
        return s, 0
    body = body.replace(SOUND_OLD, SOUND_NEW, 1)
    body = SOUND_HEAD + SOUND_GATE + body[len(SOUND_HEAD):]
    s = s[:head] + body + s[end:]
    if SOUND_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + SOUND_DECL, 1)
    return s, 1


def patch_solver_steps(s):
    """The solver step count's two places: (s, how many of them are in s with the hook, made now or before)"""
    done = 0
    for head_text in SOLVER_HEADS:
        at = _function(s, head_text)
        if at is None:
            continue
        head, end = at
        body = s[head:end]
        if body.count(SOLVER_NEW) == 1:
            done += 1
        elif body.count(SOLVER_OLD) == 1:
            s = s[:head] + body.replace(SOLVER_OLD, SOLVER_NEW, 1) + s[end:]
            done += 1
    if done and SOLVER_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + SOLVER_DECL, 1)
    return s, done


def patch_text_speed(s):
    """The scale at the head of the letters' block of f_80b1e140 (Text speed), and the call to hos_oot_text_pane after it
    (Text size: host/oot_text_size.c); None when the file has no such function, s unchanged when the
    function is there but its code differs (another game version)."""
    head = s.find(TEXT_HEAD)
    if head < 0:
        return None
    end = s.find("\nvoid f_", head + len(TEXT_HEAD))
    body = s[head:end if end >= 0 else len(s)]
    for old in (TEXT_OLD_POLLED, TEXT_OLD):   # (with the interrupt poll before the block's backward goto, or without: a lift run by hand with nx2/lift.py --no-polls)
        if old + TEXT_ADD in body:   # (applied already: a second run)
            return s
        if body.count(old) == 1:
            break
    else:
        return s
    if body.count("goto L_80b1e1b4;") != 1:
        return s
    at = head + body.index(old)
    s = s[:at] + old + TEXT_ADD + s[at + len(old):]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + TEXT_DECL, 1)


# Camera speed (host/nn_vi.c hos_camera_scale: the Insert menu's Camera speed, 1.0 as shipped). The player camera's update
# (f_80644c80) multiplies the right stick into the turn of each frame by a speed each of these returns in s0:
# - f_80643f10: the yaw's, the entry of the game's own camera speed option (options +0x254) in its speed table, times a factor;
# - f_80643b60: the pitch's, the same option's entry in the other table;
# - f_82ea21f0, f_82ea2220: the same two at level 2, the path taken while the stick's actor has the flag at +0x218.
# None of them calls another, so each turn is scaled once. Only the supported build's functions have these names (apply).
CAMERA_SPEED_HEADS = tuple("void f_%x(Ctx *__restrict A64_NOESC c) {" % a for a in (0x80643f10, 0x80643b60, 0x82ea21f0, 0x82ea2220))
CAMERA_SPEED_DECL = "extern volatile float hos_camera_scale;\n"
CAMERA_SPEED_RETURN = ("{ if (hos_camera_scale != 1.0f) { float _t; uint32_t _u; memcpy(&_t,c->vreg[0],4); _t *= hos_camera_scale; "
                       "memcpy(&_u,&_t,4); a64_vset64(c->vreg[0],_u); } return; }   /* (nx2/oot_patches.py: camera speed) */")


def patch_camera_speed(s):
    """Each return of the camera's turn-speed functions scales s0 by hos_camera_scale: (s, the functions of s that do, made now
    or by an earlier run). A function without a return is left as it is."""
    found = 0
    for head_text in CAMERA_SPEED_HEADS:
        head = s.find(head_text)
        if head < 0:
            continue
        end = s.find("\nvoid f_", head + len(head_text))
        end = end if end >= 0 else len(s)
        body = s[head:end]
        if CAMERA_SPEED_RETURN not in body:   # (not made before: a lift patched again keeps its scale once)
            body, n = re.subn(r"\breturn;", lambda _: CAMERA_SPEED_RETURN, body)
            if not n:
                continue
            s = s[:head] + body + s[end:]
        found += 1
    if found and CAMERA_SPEED_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + CAMERA_SPEED_DECL, 1)
    return s, found


# Scene format (host/nn_vi.c hos_scene_rgba16f: the Insert menu's HDR set to Native with the PsychoV35 tone mapper). f_80672260 picks the format of the scene
# buffer the game tone maps into (main+0x652340: RGBA16F, 44, under two of its flags, else R11G11B10F, 26) and makes the buffer
# again when the one it has differs (+440). R11G11B10F has no sign and 6 bits of mantissa, so the native HDR banded; with the
# switch on both choices are RGBA16F (RenoDX's zelda-oot-oz patch of main+0x65234C, github.com/clshortfuse/renodx, MIT).
SCENE_FORMAT_HEAD = "void f_80672260(Ctx *__restrict A64_NOESC c) {\n"
SCENE_FORMAT_OLD = "    c->x[10] = 0x2cULL;\n    c->x[10] &= 0xFFFFFFFFULL;\n"
SCENE_FORMAT_PICK = "    c->x[9] = 0x1aULL;\n    c->x[9] &= 0xFFFFFFFFULL;\n    { uint64_t _r = (!c->z) ? c->x[10] : c->x[9];"
SCENE_FORMAT_NEW = ("    c->x[9] = hos_scene_rgba16f ? 0x2cULL : 0x1aULL;\n    c->x[9] &= 0xFFFFFFFFULL;\n"
                    "    { uint64_t _r = (!c->z) ? c->x[10] : c->x[9];")
SCENE_FORMAT_DECL = "extern volatile int hos_scene_rgba16f;\n"


def patch_scene_format(s):
    """The scene buffer's format by hos_scene_rgba16f in f_80672260; None when the file has no such function, s unchanged when the
    function is there but its code differs (another game version)."""
    head = s.find(SCENE_FORMAT_HEAD)
    if head < 0:
        return None
    end = s.find("\nvoid f_", head + len(SCENE_FORMAT_HEAD))
    end = end if end >= 0 else len(s)
    body = s[head:end]
    if SCENE_FORMAT_NEW in body:   # (applied already: a second run)
        return s
    if body.count(SCENE_FORMAT_OLD) != 1 or body.count(SCENE_FORMAT_PICK) != 1 or \
            body.index(SCENE_FORMAT_OLD) > body.index(SCENE_FORMAT_PICK):
        return s
    s = s[:head] + body.replace(SCENE_FORMAT_PICK, SCENE_FORMAT_NEW, 1) + s[end:]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + SCENE_FORMAT_DECL, 1)


# (from TomShafford/Hookshot 55d68e8cd6 and 65c1998222) f_804566e0's two calls of f_80522050, the GPU memory a model's material
# block is made with, made through host/oot_heap_retry.c hos_oot_gpu_alloc, which calls the game's own and tries a full heap's
# failure again in the heap's parents. f_80522050's other callers stay as they are.
GPU_ALLOC_SITES = (0x80456f1c, 0x80457098)   # (the calls' return addresses)
GPU_ALLOC_OLD = "c->x[30] = %#xULL; f_80522050(c);"
GPU_ALLOC_NEW = "c->x[30] = %#xULL; hos_oot_gpu_alloc(c, f_80522050);   /* (nx2/oot_patches.py: host/oot_heap_retry.c) */"
GPU_ALLOC_DECL = "void hos_oot_gpu_alloc(Ctx *A64_NOESC c, A64Fn game);\n"
# f_804566e0's tryAllocs of the CPU buffers at its MatBlock's +200 (its bump allocator's memory) and +168, each sized for the
# model (x0 the heap, x1 the size, x2 the alignment): a null result tried in the heap's parents (hos_oot_heap_retry)
HEAP_ALLOC_SITES = (0x80456910, 0x80456da4)
HEAP_ALLOC_OLD = "{ uint64_t _t = c->x[8]; c->x[30] = %#xULL; A64_ICALL(c, _t); }\n"
HEAP_ALLOC_NEW = ("{ const uint64_t _hh = c->x[0], _hn = c->x[1], _ha = c->x[2];   /* (nx2/oot_patches.py: host/oot_heap_retry.c) */\n"
                  "  if (hos_oot_heap_refuse()) c->x[0] = 0;\n"
                  "  else { uint64_t _t = c->x[8]; c->x[30] = %#xULL; A64_ICALL(c, _t); }\n"
                  "  if (!c->x[0]) hos_oot_heap_retry(c, _hh, _hn, _ha, %#xULL); }\n")
HEAP_ALLOC_DECL = "int hos_oot_heap_refuse(void);\nvoid hos_oot_heap_retry(Ctx *c, uint64_t heap, uint64_t size, uint64_t align, uint64_t lr);\n"


def patch_heap_retry(s):
    """The four places above in one file's text: (text, [GPU sites, CPU sites] made now or found made). apply() wants
    [2, 2] over the whole lift and warns otherwise"""
    found = [0, 0]
    decls = []
    for kind, sites, old, new, decl in ((0, GPU_ALLOC_SITES, GPU_ALLOC_OLD, GPU_ALLOC_NEW, GPU_ALLOC_DECL),
                                        (1, HEAP_ALLOC_SITES, HEAP_ALLOC_OLD, HEAP_ALLOC_NEW, HEAP_ALLOC_DECL)):
        for lr in sites:
            o, n = old % lr, (new % lr if kind == 0 else new % (lr, lr))
            if n in s:
                found[kind] += 1
            elif s.count(o) == 1 and s.count('#include "a64.h"\n') == 1:
                s = s.replace(o, n, 1)
                found[kind] += 1
                if decl not in decls:
                    decls.append(decl)
    for decl in decls:
        if decl not in s:
            s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + decl, 1)
    return s, found


# (the wait action's start and tick, code_033.c; the anchors carry call-return PC literals, so each is once in its function)
OC_DECL = ("extern volatile int hos_oc_wait;\nextern volatile int hos_oc_last_kind;\n"
           "extern volatile unsigned hos_oc_beat;\n"
           "#define HOS_OC_BUTTONS(k) ((k) == 1 || (k) == 2 || (k) == 3 || (k) == 10)\n")
OC_ENTRY_HEAD = "void f_828a9190(Ctx *__restrict A64_NOESC c) {"
OC_ENTRY_OLD = ("    c->x[1]=recomp_load32(c,c->x[19] + 32);\n"
                "    { uint64_t _r = ((uint64_t)0 | ((c->x[0]) << 0)); c->x[20] = _r; }\n"
                "c->x[30] = 0x828a91e4ULL; f_832b35d0(c);")
OC_ENTRY_NEW = ("    c->x[1]=recomp_load32(c,c->x[19] + 32);\n"
                "    hos_oc_last_kind = (int)c->x[1], hos_oc_wait = HOS_OC_BUTTONS(c->x[1]) ? 1 : 2, hos_oc_beat++;"
                "   /* (nx2/oot_patches.py: the note wait began; 1 and 3 are the field ocarina, 2 a song lesson, "
                "10 the scarecrow's recording, 13 a dialogue's, which stays the game's) */\n"
                "    { uint64_t _r = ((uint64_t)0 | ((c->x[0]) << 0)); c->x[20] = _r; }\n"
                "c->x[30] = 0x828a91e4ULL; f_832b35d0(c);")
OC_TICK_HEAD = "void f_828a9280(Ctx *__restrict A64_NOESC c) {"
OC_TICK_OLD = ("    c->x[8]=recomp_load8(c,c->x[0] + 24);\n"
               "if (!((c->x[8] >> 1) & 1)) { goto L_828a92e4; }")
OC_TICK_NEW = ("    c->x[8]=recomp_load8(c,c->x[0] + 24);\n"
               "if (!((c->x[8] >> 1) & 1)) { hos_oc_wait = 0; goto L_828a92e4; }"
               "   /* (nx2/oot_patches.py: this action stopped running: no wait) */\n"
               "    hos_oc_wait = HOS_OC_BUTTONS(hos_oc_last_kind) ? 1 : 2, hos_oc_beat++;"
               "   /* (nx2/oot_patches.py: re-armed every running tick: the input resets between note windows end "
               "one wait without ending the action; the beat tells the host the wait is alive) */")
# (the tick's own gates sit after the re-arm: while any of them refuses, the game is not asking for notes, say a
# dialogue took the buttons; the host must hear that, or its pad swap eats the buttons the dialogue wants)
OC_TICK_GATES = (
    ("if (!c->z) { goto L_828a92e4; }",
     "if (!c->z) { hos_oc_wait = 0; goto L_828a92e4; }"
     "   /* (nx2/oot_patches.py: the action's mode check refused: no wait) */"),
    ("c->x[30] = 0x828a92e0ULL; f_806aac40(c);\n"
     "if (c->x[0] != 0) { goto L_828a92f8; }",
     "c->x[30] = 0x828a92e0ULL; f_806aac40(c);\n"
     "if (c->x[0] != 0) { goto L_828a92f8; }\n"
     "hos_oc_wait = 0;   /* (nx2/oot_patches.py: the input context is gone: no wait) */"),
    ("c->x[30] = 0x828a9318ULL; f_82856760(c);\n"
     "if (c->x[0] == 0) { { A64_POLL(c); goto L_828a92e4; } }",
     "c->x[30] = 0x828a9318ULL; f_82856760(c);\n"
     "if (c->x[0] == 0) { { A64_POLL(c); hos_oc_wait = 0; goto L_828a92e4; } }"
     "   /* (nx2/oot_patches.py: the wait's owner refused: no wait, and the pads are the game's again) */"),
)
OC_DONE_OLD = "recomp_store32(c,c->x[19] + 24,c->x[9]);"
OC_DONE_NEW = (OC_DONE_OLD + "\n    hos_oc_wait = 0;"
               "   /* (nx2/oot_patches.py: the wait action finished) */")
OC_RESET_OLD = "c->x[30] = 0x828a93e4ULL; f_832b35d0(c);"
OC_RESET_NEW = (OC_RESET_OLD + "\n    hos_oc_wait = 0;"
                "   /* (nx2/oot_patches.py: the input reset: this window's wait is over) */")


def patch_ocarina_wait(s):
    """The wait action reports itself to the host; None when the lift has no such functions, s unchanged when an anchor drifted or it is applied already (never a half-patch)."""
    splices, hits = [], 0
    for head, pairs in ((OC_ENTRY_HEAD, [(OC_ENTRY_OLD, OC_ENTRY_NEW)]),
                        (OC_TICK_HEAD, [(OC_TICK_OLD, OC_TICK_NEW)] + [p for p in OC_TICK_GATES] +
                                       [(OC_DONE_OLD, OC_DONE_NEW), (OC_RESET_OLD, OC_RESET_NEW)])):
        at = s.find(head)
        if at < 0:
            return None
        end = s.find("\nvoid f_", at + len(head))
        body = s[at:end if end >= 0 else len(s)]
        out = body
        for old, new in pairs:
            if new in out:
                continue
            if out.count(old) != 1:
                return s
            out = out.replace(old, new, 1)
        if out != body:
            splices.append((at, end, out))
            hits = 1
    if not hits:
        return s
    for at, end, out in reversed(splices):   # (later functions first: earlier splices keep the earlier offsets valid)
        s = s[:at] + out + (s[end:] if end >= 0 else "")
    if OC_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + OC_DECL, 1)
    return s


def patch_file(s, name, protect_hearts=False):
    """One lifted file's patches (apply(), and nx2/chunks.py edit_c, a process per file): (its text, the places counted in it, the
    lines to print). The counts add up over the files; report() decides from the sums"""
    counts = collections.Counter()
    lines = []
    out = s
    d = patch_dynres(out)
    if d is not None:
        out = d
        counts["dynamic resolution branch"] += 1
        lines.append("oot_patches: dynamic resolution switch in " + name)
    t = patch_text_speed(out)
    if t is not None and t != out:
        out = t
        counts["text speed block"] += 1
        lines.append("oot_patches: text speed scale and text size in " + name)
    focus = patch_menu_focus(out) if protect_hearts else None
    if focus is not None:
        out = focus
        counts["menu focus hook"] += 1
        lines.append("oot_patches: the menus' focus hook in " + name)
    tick = patch_menu_tick(out) if protect_hearts else None
    if tick is not None:
        out = tick
        counts["menu tick hook"] += 1
        lines.append("oot_patches: the menus' update hook in " + name)
    draw = patch_text_draw(out) if protect_hearts else None
    if draw is not None:
        out = draw
        counts["text draw hook"] += 1
        lines.append("oot_patches: the text boxes' draw hook in " + name)
    glyph = patch_glyph(out) if protect_hearts else None
    if glyph is not None:
        out = glyph
        counts["glyph hook"] += 1
        lines.append("oot_patches: the glyphs' hook in " + name)
    zs = patch_zstd_stream(out) if protect_hearts else None
    if zs is not None:
        if zs != out:
            lines.append("oot_patches: the zstd stream decode's host hook in " + name)
        out = zs
        counts["zstd stream hook"] += 1
    layout = patch_text_layout(out) if protect_hearts else None
    if layout is not None:
        out = layout
        counts["text layout hook"] += 1
        lines.append("oot_patches: the text boxes' layout hook in " + name)
    movie = patch_movie_frames(out)
    if movie is not None:
        if MOVIE_NEW in movie:   # (made now, or by an earlier run over this file)
            counts["movie's frame count"] += 1
        if movie != out:
            lines.append("oot_patches: the movie's frame count by the frames' time in " + name)
        out = movie
    ocw = patch_ocarina_wait(out)
    if ocw is not None:
        if OC_ENTRY_NEW in ocw:   # (made now, or by an earlier run over this file)
            counts["ocarina note wait"] += 1
        if ocw != out:
            lines.append("oot_patches: the ocarina note wait in " + name)
        out = ocw
    out, n = fixed_scale(out)
    if n:
        counts["fixed path's scale"] += 1
        lines.append("oot_patches: the fixed path's scale in " + name)
    life = patch_life(out) if protect_hearts else None
    if life is not None:
        out = life
        counts["player heart protection"] += 1
        lines.append("oot_patches: player heart protection in " + name)
    move = patch_movement(out) if protect_hearts else None
    if move is not None:
        out = move
        counts["player flight"] += 1
        lines.append("oot_patches: player flight in " + name)
    rotation = patch_flight_rotation(out) if protect_hearts else None
    if rotation is not None:
        out = rotation
        counts["player flight rotation"] += 1
        lines.append("oot_patches: player flight rotation in " + name)
    mouse = patch_mouse_camera(out) if protect_hearts else None
    if mouse is not None:
        out = mouse
        counts["mouse camera"] += 1
        lines.append("oot_patches: native mouse camera in " + name)
    center = patch_camera_centering(out) if protect_hearts else None
    if center is not None:
        out = center
        counts["disable camera centering"] += 1
        lines.append("oot_patches: disable camera centering switch in " + name)
    opening = patch_opening(out) if protect_hearts else None
    if opening is not None:
        n = opening.count(OPENING_REQUEST) + opening.count(OPENING_TIMELINE) + opening.count(OPENING_QUERY)
        counts["opening skip"] += n
        if opening != out:
            lines.append("oot_patches: the opening skip at %d function(s) in %s" % (n, name))
        out = opening
    speed = patch_cct_speed(out) if protect_hearts else None
    if speed is not None:
        if speed[0] != out:
            lines.append("oot_patches: movement speed in " + name)
        out = speed[0]
        counts["speed hooks"] += speed[1]
    view = patch_view(out) if protect_hearts else None
    if view is not None:
        out = view
        counts["free camera"] += 1
        lines.append("oot_patches: free camera in " + name)
    scene = patch_scene_format(out) if protect_hearts else None
    if scene is not None and SCENE_FORMAT_NEW in scene:
        counts["scene format"] += 1
        if scene != out:
            lines.append("oot_patches: the scene buffer's format for native HDR in " + name)
        out = scene
    if protect_hearts:   # (found by name alone: in another build these addresses are other functions)
        made, n = patch_camera_speed(out)
        counts["camera speed"] += n
        if made != out:
            out = made
            lines.append("oot_patches: camera speed at %d function(s) in %s" % (n, name))
    out, sites = patch_frame_seconds(out)
    counts["step sites"] += out.count(STEP_MARK)   # (made now, or by an earlier run over this file)
    if sites:
        lines.append("oot_patches: the frame's time at %d step divide(s) in %s" % (sites, name))
    heap, found = patch_heap_retry(out) if protect_hearts else (out, [0, 0])
    counts["heap GPU sites"] += found[0]
    counts["heap CPU sites"] += found[1]
    if heap != out:
        lines.append("oot_patches: heap-full retries of the OptimizeChangeBank allocations in " + name)
    out = heap
    out, whole = patch_whole_frames(out)
    counts["whole sites"] += out.count(WHOLE_MARK)   # (made now, or by an earlier run over this file)
    if whole:
        lines.append("oot_patches: %d frame counter(s) with the step's whole frames in %s" % (whole, name))
    sound, found = patch_sound_job(out)
    counts["sound jobs"] += found
    if sound != out:
        lines.append("oot_patches: the sound job at 60 Hz above 60 fps in " + name)
    out = sound
    solver, found = patch_solver_steps(out)
    counts["solver sites"] += found
    if solver != out:
        lines.append("oot_patches: the solver steps rounded up above 60 fps in " + name)
    out = solver
    return out, counts, lines


def report(paths, counts, protect_hearts=False):
    """The warnings from the files' counts (each file's patch_file counts, in the order of paths), and the step divides' rule:
    both or neither, so a file of paths that has the hook is put back"""
    done = {"dynamic resolution branch": 0, "text speed block": 0, "fixed path's scale": 0, "movie's frame count": 0,
            "ocarina note wait": 0}
    if protect_hearts:   # (the supported build's: these places are its functions)
        done["menu focus hook"] = 0
        done["menu tick hook"] = 0
        done["text draw hook"] = 0
        done["glyph hook"] = 0
        done["text layout hook"] = 0
        done["player heart protection"] = 0
        done["player flight"] = 0
        done["player flight rotation"] = 0
        done["free camera"] = 0
        done["mouse camera"] = 0
        done["disable camera centering"] = 0
        done["scene format"] = 0
        done["zstd stream hook"] = 0
    total = collections.Counter()
    for c in counts:
        total.update(c)
    for what in done:
        done[what] = total[what]
    speed_hooks = total["speed hooks"]   # (SPEED_PLACES', in one file or several)
    camera_speed = total["camera speed"]   # (the turn-speed functions that scale by hos_camera_scale: patch_camera_speed)
    heap_sites = [total["heap GPU sites"], total["heap CPU sites"]]   # (patch_heap_retry, the supported build only)
    if protect_hearts and heap_sites != [len(GPU_ALLOC_SITES), len(HEAP_ALLOC_SITES)]:   # (the supported build's functions)
        print("oot_patches: WARNING: the OptimizeChangeBank allocations were found %s times, not once each: a full heap is "
              "not tried again" % heap_sites)
    step_sites = total["step sites"]   # (the divides with the hook)
    step_files = [p for p, c in zip(paths, counts) if c["step sites"]]   # (the files that have it)
    whole_sites = total["whole sites"]   # (the counters of WHOLE_SITES with the whole frames: patch_whole_frames)
    if total["sound jobs"] != 1:   # (without it, above 60 fps sounds and then the music can stop after a few loading zones)
        print("oot_patches: WARNING: the sound job was found %d times, not once" % total["sound jobs"])
    if total["solver sites"] != len(SOLVER_HEADS):   # (without them, above 60 fps moving platforms can drop Link)
        print("oot_patches: WARNING: the solver step count was found at %d of %d places" % (total["solver sites"], len(SOLVER_HEADS)))
    for what, n in done.items():
        if n != 1:
            print("oot_patches: WARNING: the %s was found %d times" % (what, n))
    if protect_hearts and speed_hooks != len(SPEED_PLACES):
        print("oot_patches: WARNING: the movement speed's places were found %d times, not %d" % (speed_hooks, len(SPEED_PLACES)))
    if protect_hearts and total["opening skip"] != 3:
        print("oot_patches: WARNING: the opening skip was found at %d of 3 functions" % total["opening skip"])
    if protect_hearts and camera_speed != len(CAMERA_SPEED_HEADS):
        print("oot_patches: WARNING: the camera's turn speed was found in %d of %d functions (the Camera speed row turns only "
              "those)" % (camera_speed, len(CAMERA_SPEED_HEADS)))
    if step_sites != len(STEP_SITES):
        # (both or neither: with one divide timed and the other stepping 1/60 s the two clocks of the game would run apart. Without
        # the hook the host keeps the rate up to the cap, as before: host/nn_vi.c, hos_step_probe)
        for path in step_files:
            with open(path, encoding="utf-8", newline="") as f:
                s = f.read()
            whole_write.write_whole(path, unpatch_frame_seconds(s), newline="")
        print("oot_patches: WARNING: the frame's time was found at %d of %d step divides, none patched (a Frame rate cap above 60 "
              "keeps the game's own step)" % (step_sites, len(STEP_SITES)))
    elif whole_sites != len(WHOLE_SITES):   # (the frames' own time without them: the pick-up indicator stays above 60 fps)
        print("oot_patches: WARNING: %d of %d frame counters found for the step's whole frames (above 60 fps a counter can stop: "
              "the pick-up indicator, the water ripples, the time of day)" % (whole_sites, len(WHOLE_SITES)))


def apply(gen, *, protect_hearts=False, required=False):
    """required: the lift of the supported build (LIFE_BUILD_ID), whole: a missing output-size edit stops it (apply_output_size).
    The other patches only warn when their place is not found once. (nx2/chunks.py edit_c makes the same edits, a process per
    file)"""
    paths = sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c")))
    counts = []
    for path in paths:
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        out, c, lines = patch_file(s, os.path.basename(path), protect_hearts)
        counts.append(c)
        if lines:
            print("\n".join(lines))
        if out != s:
            whole_write.write_whole(path, out, newline="")
    report(paths, counts, protect_hearts)
    apply_output_size(gen, required=required)


# Output size (host/nn_applet.c hos_output_size: the Insert menu's Output resolution, 2560 x 1440 as shipped):
# - nnMain (f_81222ce0) puts the display size in the framework's create arg (+20/+24, the framework's +156/+160); the window
#   textures are made at it.
# - f_810a7b70 sets up the display-size object (B = [[0x84ca29c0]+216]) from a config (x1): its docked size list (+8, first
#   2560 x 1440) is the frame's base (B+244; the frame is the base times the graphics scale's factor, f_806702e0, capped by the
#   scale's display class: docked 3840 x 2160 at scale 0). Its first entry asks the host when it differs.
OUT_DECL = "uint32_t hos_output_size(int height);\nuint64_t hos_rt_pool_size(uint64_t game);\n"
OUT_EDITS = [
    # (the picture's 2560 x 1440 at 0x81202ee4, not f_831f3200's copy at +0xb68; nx2/oot_wide.py's widescreen size, after it,
    # stays over it: host/hos_main.c makes that one at this height)
    ("    c->x[8] = 0xa00ULL;\n    c->x[8] = (c->x[8] & ~(0xFFFFULL<<32)) | (0x5a0ULL<<32);\n"
     "{ extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }   /* (nx2/oot_wide.py: hos_wide_out) */\n"
     "    { uint64_t _b=c->x[21]; int64_t _o=20; recomp_store64(c,(_b+_o),c->x[8]); }",
     "    c->x[8] = hos_output_size(0) | ((uint64_t)hos_output_size(1) << 32);\n"
     "{ extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }   /* (nx2/oot_wide.py: hos_wide_out) */\n"
     "    { uint64_t _b=c->x[21]; int64_t _o=20; recomp_store64(c,(_b+_o),c->x[8]); }"),
    ("    { uint64_t _a=c->x[0], _b=724ULL; uint64_t _r=_a+_b; c->x[21]=_r; }\n"
     "    { uint64_t _r = ((uint64_t)0 | ((c->x[1]) << 0)); c->x[20] = _r; }\n",
     "    { uint64_t _a=c->x[0], _b=724ULL; uint64_t _r=_a+_b; c->x[21]=_r; }\n"
     "    { uint64_t _r = ((uint64_t)0 | ((c->x[1]) << 0)); c->x[20] = _r; }\n"
     "    if ((hos_output_size(0) != 2560 || hos_output_size(1) != 1440) && recomp_load32(c, c->x[20] + 8) == 2560) { recomp_store32(c, c->x[20] + 8, hos_output_size(0)); recomp_store32(c, c->x[20] + 12, hos_output_size(1)); }\n"),
    # - f_80ea2f30: the 3D's largest size, ResolutionParam's BaseResolution (or 1920 x 1080 as constants), 3/4 of 2560 x 1440;
    #   above it the game takes a smaller upscale network (1/3: 1280 x 720 at 4K). Scaled with the output.
    ("c->x[30] = 0x80ea2ffcULL; f_831cca50(c);\n"
     "    { uint64_t _b=c->x[0]; int64_t _o=0; { uint64_t _p0,_p1; recomp_ldp32(c,(_b+_o),&_p0,&_p1); c->x[24]=_p0; c->x[25]=_p1; } }\n",
     "c->x[30] = 0x80ea2ffcULL; f_831cca50(c);\n"
     "    { uint64_t _b=c->x[0]; int64_t _o=0; { uint64_t _p0,_p1; recomp_ldp32(c,(_b+_o),&_p0,&_p1); c->x[24]=_p0; c->x[25]=_p1; } }\n"
     "    if (hos_output_size(0) != 2560 || hos_output_size(1) != 1440) { c->x[24] = (uint32_t)c->x[24] * hos_output_size(0) / 2560; c->x[25] = (uint32_t)c->x[25] * hos_output_size(1) / 1440; }\n"),
    ("    c->x[8] = 0x44f00000ULL;\n    c->x[8] &= 0xFFFFFFFFULL;\n    recomp_store32(c,c->x[19] + 3668,c->x[8]);\n"
     "    c->x[8] = 0x44870000ULL;\n    c->x[8] &= 0xFFFFFFFFULL;\n",
     "    c->x[8] = hos_output_size(0) * 3 / 4;\n    { float _f = (float)c->x[8]; uint32_t _u; memcpy(&_u, &_f, 4); c->x[8] = _u; }\n"
     "    recomp_store32(c,c->x[19] + 3668,c->x[8]);\n"
     "    c->x[8] = hos_output_size(1) * 3 / 4;\n    { float _f = (float)c->x[8]; uint32_t _u; memcpy(&_u, &_f, 4); c->x[8] = _u; }\n"),
    # - The render targets' GPU pool: 0x13100000 bytes (the graphics config's +1464, read in f_836ce9d0, which takes its storage from
    #   the "Graphics" heap and makes a physical pool and a virtual one over it: f_80c4a1c0 -> f_80c49070 -> f_80c49380 -> f_811c9ba0).
    #   Sized for 2560 x 1440: a 3840x2160 3D at the 4K output ran out of it and the next target came back null (crash in f_8005ed00,
    #   the pool's allocator, its +0x28 0x13100000). Scaled with the output's pixels (host/nn_applet.c
    #   hos_rt_pool_size: 687 MB at 3840 x 2160), at the config, so the storage and the pool agree.
    ("L_836cea54:;\n",
     "L_836cea54:;\n"
     "    if (recomp_load64(c, c->x[19] + 1464) == 0x13100000ULL) recomp_store64(c, c->x[19] + 1464, hos_rt_pool_size(0x13100000ULL));\n"),
    # - The "Graphics" heap holds it, made by f_83b7b340 with the other module heaps (twice: 0x34900000 bytes in f_83b7a2c0's first
    #   pass, then the heap kept, 0x348ffe20; it had under 16 MB to spare): grown by as much as the pool.
    ("c->x[30] = 0x83b7b440ULL; f_809df710(c);\n",
     "if (c->x[0] == 0x34900000ULL || c->x[0] == 0x348ffe20ULL) c->x[0] += hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL;\n"
     "c->x[30] = 0x83b7b440ULL; f_809df710(c);\n"),
    # - f_83b7b340 then takes each module heap's use (its vtable +120) off the module's budget (the configured size, a table of slots)
    #   and aborts below zero (f_8351c280): the Graphics heap's use counts as the shipped one.
    ("{ uint64_t _t = c->x[8]; c->x[30] = 0x83b7b4e0ULL; A64_ICALL(c, _t); }\n",
     "{ uint64_t _t = c->x[8]; c->x[30] = 0x83b7b4e0ULL; A64_ICALL(c, _t); }\n"
     "{ const uint64_t _b = recomp_load64(c, c->x[21] + (c->x[22] << 3)), _g = hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL; "
     "if ((_b == 0x34900000ULL || _b == 0x348ffe20ULL) && c->x[0] > _g) c->x[0] -= _g; }\n"),
    # - f_80b72340 (from the graphics init f_836cf4b0) makes a heap of what the Graphics heap can still give, which took the growth as
    #   well (0x1b6cb48 bytes as shipped, 0x1996cb48 grown) and left too little for the pool: it keeps its shipped size.
    ("c->x[30] = 0x80b723acULL; f_809df700(c);\n",
     "if (c->x[0] > hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL) c->x[0] -= hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL;\n"
     "c->x[30] = 0x80b723acULL; f_809df700(c);\n"),
]


def output_size_file(s, name):
    """One lifted file's output-size edits: (its text, each place's count in it, the lines to print). apply_output_size(), and
    nx2/chunks.py edit_c, a process per file"""
    found = [0] * len(OUT_EDITS)
    hit = False
    for i, (old, new) in enumerate(OUT_EDITS):
        # (every place: each made one, and each old text outside a made one; a new text can hold its old one. A file with
        # both is counted for both, and its old ones are left: a second made place or a stray raw one is not once)
        made = s.count(new)
        raw = (s.replace(new, "\0") if made else s).count(old)
        found[i] += made + raw
        if raw and not made:
            s = s.replace(old, new)
            hit = True
    if hit:
        if OUT_DECL not in s:
            s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + OUT_DECL, 1)
        return s, found, ["oot_patches: output size in " + name]
    return s, found, []


def check_output_size(found, required=False):
    """found: each place's count over all the files"""
    if found != [1] * len(OUT_EDITS):
        if required:
            raise SystemExit("oot_patches: the output size places were found %s times, not once each (OUT_EDITS %s): the "
                             "lifted C no longer has the text they match" % (found, [i for i, n in enumerate(found) if n != 1]))
        print("oot_patches: WARNING: the output size places were found %s times" % found)


def apply_output_size(gen, required=False):
    """required (the supported build's whole lift): each edit must be made, or found made, exactly once, else SystemExit: without
    one the Insert menu's Output resolution is ignored, or a 4K output runs out of the render targets' pool. Elsewhere (another
    build, part of a lift, by hand) a warning"""
    found = [0] * len(OUT_EDITS)
    for path in sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        out, here, lines = output_size_file(s, os.path.basename(path))
        found = [a + b for a, b in zip(found, here)]
        if out != s:
            whole_write.write_whole(path, out, newline="")
            print("\n".join(lines))
    check_output_size(found, required)


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
