"""The pipeline tour: plays the game unattended, from a new save through every debug Teleport destination as child and as
adult, with the items, their effects and the menus, so that its graphics pipelines are met and written to the pipeline state list
(gpu/vk/vk_draw.cpp: hookshot_pipeline_states.bin). The parts' lists become the tour's source of the list a release ships,
assets/pipeline_states/tour.bin.xz and tour.md (nx2/pipeline_states.py tour): the first start prepares those pipelines before the
title.

Ported from Hookshot's draft pull request https://github.com/NotRexed/Hookshot/pull/2048 by HumanBean (hckrman101 on GitHub), and
changed to drive the game through the control bridge (host/control.c, docs/control.md) instead of a live input file and the log.

    python -I scripts/pipeline_tour.py tour --game <game folder> --out <dir> [--name play] [--parts ...] [--publish] [--env ...] [--ini ...]
                                            [--vk-profile <dir>] [--settle-quiet FRAMES] [--settle-max FRAMES] [--compare-to <list>]
    python -I scripts/pipeline_tour.py run  --game <game folder> --out <dir> [--name play] [--part <part>] [--first <stop>] [--stops N]
                                            [--skip-opening] [--env NAME=VALUE ...] [--ini key=value ...] [--window WxH] [--dry-run]
                                            [--vk-profile <dir>] [--settle-quiet FRAMES] [--settle-max FRAMES]
    python -I scripts/pipeline_tour.py merge-profile <vk-profile dir> [--out <file>] [--publish] [--llvm-profdata <path>]
    python -I scripts/pipeline_tour.py merge --out <file> <list> ...
    python -I scripts/pipeline_tour.py compare <new list> <old list>
    python -I scripts/pipeline_tour.py report <out>/<name>
    python -I scripts/pipeline_tour.py value <run dir> ... [--sub] [--floor 10]

run: one game. The game folder is used as it is, so give it a copy (cp -a --reflink=always): its cache/hookshot_pipelines.bin (the
pipeline notes), cache/hookshot_pipeline_states.bin (and _v1) and hookshot_pipeline_states_shipped.bin are moved aside for the run
(a precompiled pipeline is not noted again, and only what this run meets counts) and put back after it. The run starts with no save
at all (an empty data folder of its own, <out>/<name>/data): the title, New Game, the opening, then control of Link. Then every item, and each destination: the
teleport, its loading, the camera all around, then on once no new pipeline came for --settle-quiet frames (60: a second of game
time; --settle-max, 600, at most). The sword, the shield and an item from the pause menu are used once an age (ITEMS_ONCE) and at
the few stops with passes of their own an item's draws could meet (ITEMS_PASSES: water, the Lens of Truth, heat haze), not at
every stop: a pipeline is a program and its fixed draw state, and a scene's light and fog are inputs to the same pipelines.

The game is driven through the control bridge (HOS_CONTROL, a socket in the run's folder): presses timed in the game's frames
(sequence), the debug teleport by destination and the age switch with the game's own answer (refused, cancelled, sent; then the map
it arrives in), and the game's state (the map, an event playing, a map loading, Link's age: status). The one thing the bridge does
not do, the Debug tab's inventory, goes through the live input file (HOS_INPUT_LIVE, "!cheat <action>"). No controller is opened
(HOS_NO_SDL) and nothing reaches the desktop. As fast as the game goes: no window (HOS_HEADLESS), the smallest output (640x360, the
game's own upscaler at 50%: a pipeline's key has formats and state, not sizes), frame pacing off, no frame generation, the cap
Uncapped with every frame a 60th of a second of game time (HOS_FPS_MAX=1000, HOS_FPS_STEP=0), text at once, and the opening skipped
by the game itself (HOS_SKIP_OPENING) in every part but the one that watches it. Every wait on the game counts its frames (the
bridge's wait present), so a faster game waits less; only the inventory's live lines are spaced in wall time as well (the game
reads that file every 30 ms). Each step has a limit: a step that does not happen fails the run, it does not wander. --env and
--ini run it under other settings (RX_VK_FORCE_MESH_FALLBACK=1, dlss=0, hdr=1, HOS_HANDHELD=1); the options file is restored.
--window shows it in a window instead.

tour: the parts one after another, one game at a time, in a reflinked copy of the game folder, in games of --chunk stops (a
fresh game each), at nice 19 on --cpus, each under the memory guard (MEM: it waits for memory before a game, and ends a game that
holds too much or leaves the machine too little, the next game going on from there; on Linux the game runs in a systemd scope
with a MemoryMax); merged (--compare-to <list>: what the merged list lacks of an older one); --publish writes the repository's
tour list from them (nx2/pipeline_states.py tour, with the GPU, build, settings and route the runs logged).

--vk-profile <dir> (run and tour): the renderer's play profile recorded on the tour's route (docs/development.md, "The
renderer's play profile"). The game folder's renderer must be an RX_VK_PGO=gen one (a counting pack: RX_VK_PGO=gen python -I
scripts/dev_packs.py pack <base> vk-count): it is refused otherwise. Each game runs with RX_VK_PGO_FILE set; once it has control
of Link the counters are cleared (<file>.reset: the loading, the prepare and the title left out), and at its end, failed or not,
they are written (<file>.dump) and moved to <dir>/<run name>.profraw, with <run name>.json saying where they were recorded. A
renderer that takes neither request fails the run rather than leaving nothing. The pipeline lists are made as without it (the
counting renderer is slower, its pipelines the same). merge-profile then merges <dir>/*.profraw with the toolchain's
llvm-profdata into work/hookshot_vk.profdata (what nx2/make_game.py builds with); --publish also writes
assets/profiles/hookshot_vk.profdata.xz (xz -9e) and the note for assets/profiles/README.md (<dir>/README-note.md).

compare: two lists' pipelines against each other (either may be a tour's merged <out>/<name>.bin, a run's states.bin or a
tour.bin.xz): how many each has that the other lacks. A faster tour that loses nothing has none in the old list that the new lacks.

value: each step's pipelines over runs that the players' lists (the shipped list's contrib/ sources) and the other steps do not
have, its seconds, and which steps add too little to keep.

Each run writes <out>/<name>/: states.bin (its list), report.txt and report.json (the pipelines after each step, the new ones each
added, its frames and wall time), log.txt (the game's log) and steps.txt (what was sent, at which frame).

Linux (the memory guard reads /proc; the bridge is a Unix socket). Standard library only. The actions' numbers come from
host/hos_main.c, gpu/vk/oot_cheat_items.h and gpu/vk/vk_menu.cpp; the bridge's client is scripts/hookshot_ctl.py.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (in the game folder: the pipeline notes and the lists play adds to are in cache/, the shipped list beside the game
# (gpu/vk/vk_draw.cpp state_list_path, pipe_notes_path; host/game_layout.c), and a build from before left its loose beside the
# game, which the game's start moves into cache/. hookshot_computes.bin: the compute programs earlier play dispatched, which the
# game writes into the list when it opens it (vk_draw.cpp state_open_locked): aside too, or every run's list starts with them)
STATE_FILES = ("cache/hookshot_pipelines.bin", "cache/hookshot_pipeline_states.bin", "cache/hookshot_pipeline_states_v1.bin",
               "cache/hookshot_computes.bin", "hookshot_pipeline_states_shipped.bin", "hookshot_pipelines.bin",
               "hookshot_pipeline_states.bin", "hookshot_pipeline_states_v1.bin", "hookshot_computes.bin")
LIST = "cache/hookshot_pipeline_states.bin"
OPTIONS = "hookshot_options.ini"

# no window, no sound, no controller, no pacing, the cap Uncapped with a 60th of a second of game time a frame, the smallest
# output, the bridge's input alone, text at once, no frame generation
BASE_ENV = {
    "HOS_HEADLESS": "1",
    "HOS_NO_SDL": "1",
    "HOS_AUDIO": "0",
    "HOS_INPUT_EXCLUSIVE": "1",
    "HOS_INPUT_SCRIPT_BACKGROUND": "1",
    "HOS_OOT_CHEATS": "1",            # (the Debug tab's tools: the inventory, through the live input)
    "HOS_OOT_PROTECT_HEARTS": "1",
    "HOS_NO_PACING": "1",
    "HOS_REALTIME_STEP": "0",         # (Game speed: Per frame)
    "HOS_FPS_MAX": "1000",            # (Uncapped's)
    "HOS_FPS_STEP": "0",              # (every frame a 60th of a second of game time: host/nn_vi.c)
    "HOS_FPS_LOG": "1",
    "HOS_OUTPUT_RES": "640x360",
    "HOS_TEXT_SPEED": "3",
    "HOS_DISCORD": "0",
    "RX_VK_FRAMEGEN": "0",
    "HOS_RT_POOL_MB": "320",     # (render target pool: 640x360 needs far less than the default 512 MB)
    "RX_VK_SHOT_EVERY": "600",   # (with RX_VK_SHOTS, set per run: a small picture every 600 frames, to see where a run got stuck)
    "RX_VK_SHOT_W": "320",
    "RX_VK_SHOT_H": "180",
}
FPS = 60   # (the game's frames a second of game time: the presses' clock)
# the options for the run (its own copy of hookshot_options.ini, restored after it): the cheapest of each that does not take passes
# or formats away. The output 640x360 (as HOS_OUTPUT_RES), the upscaler the game's own (dlss=0), render scale 50% (resolution=4),
# no frame generation, sharpening or HDR, the cap Uncapped (frame_rate_cap=6), Game speed Per frame, no V-Sync, text at once. Set
# in the file before the game starts: several rows take effect only at a start
BASE_INI = {"output_res": "640x360", "dlss": "0", "upscaling": "0", "resolution": "4", "frame_generation": "0", "sharpness": "0",
            "hdr": "0", "frame_rate_cap": "6", "game_speed": "0", "vsync": "0", "text_speed": "3", "fps_counter": "0",
            "frame_pacing": "0", "discord_status": "0"}


# ---- the Debug tab's actions, as the source has them ----

def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def define(text, name):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, text)
    if not m:
        raise SystemExit("pipeline_tour: %s not found" % name)
    return int(m.group(1))


def bancs(main):
    """Each destination's maps, child and adult (None: none), as host/hos_main.c's oot_destinations has them"""
    table = main.split("oot_destinations[] = {", 1)[1].split("\n};", 1)[0]
    q = r'(NULL|"[^"]*")'
    out = []
    for m in re.finditer(r"\{%s,\s*%s,\s*%s,\s*%s," % (q, q, q, q), table):
        out.append(tuple(None if v == "NULL" else v.strip('"') for v in (m.group(1), m.group(4))))
    return out


def actions(root_read=read):
    """The action numbers and the lists they index: {age, item, bancs, teleports: [(name, adult_only)], dungeons,
    items: {pouch name: (index, kind)}}"""
    main, items_h, menu = root_read("host/hos_main.c"), root_read("gpu/vk/oot_cheat_items.h"), root_read("gpu/vk/vk_menu.cpp")
    names = re.search(r"teleport_names\[\]\s*=\s*\{(.*?)\};", menu, re.S)
    notes = re.search(r"teleport_notes\[\]\s*=\s*\{(.*?)\};", menu, re.S)
    if not names or not notes:
        raise SystemExit("pipeline_tour: the teleport list not found in gpu/vk/vk_menu.cpp")
    labels = re.findall(r'"((?:[^"\\]|\\.)*)"', names.group(1))
    note_list = [n.strip() for n in notes.group(1).split(",")]
    destinations = len(re.findall(r"\},\s*/\*", main.split("oot_destinations[]", 1)[1].split("};", 1)[0]))
    if destinations != len(labels) or len(note_list) != len(labels):
        raise SystemExit("pipeline_tour: %d teleport names, %d notes, %d destinations" % (len(labels), len(note_list), destinations))
    items = re.findall(r'OOT_ITEM\("[^"]*",\s*"([^"]+)",\s*(\d)\)', items_h)
    dungeons = re.search(r'\{"Dungeons", (\d+)\}', menu)
    return {
        "teleport": define(main, "OOT_TELEPORT_ACTION"),
        "age": define(main, "OOT_AGE_ACTION"),
        "bancs": bancs(main),
        "item": define(items_h, "OOT_CHEAT_ITEM_ACTION"),
        "teleports": [(label, "adult only" in note) for label, note in zip(labels, note_list)],
        "dungeons": int(dungeons.group(1)) if dungeons else len(labels),
        "items": {name: (i, int(kind)) for i, (name, kind) in enumerate(items)},
    }


QUEST_ITEMS = ("Item_Medal_", "Item_Stone_", "Item_Melody_")
# Stops left out: a scene starts there that waits for something the tour does not do, and the map can then not be left
LEFT_OUT = {("adult", "Sacred Forest Meadow"): "Sheik's lesson starts on arrival and waits for the ocarina to be played",
            # (Hookshot #2048's runs 2-4: what only these met, against everything else)
            ("child", "Lost Woods"): "20 pipelines no other stop met", ("adult", "Lost Woods"): "7",
            ("child", "Sacred Forest Meadow"): "4",
            # (pipeline_tour.py value over runs 2-4 and the short runs: what each stop met that neither the players' lists in
            # assets/pipeline_states/contrib nor any other kept stop has; under 10 is not worth the stop's 15-30 s)
            ("adult", "Kokiri Forest"): "4 pipelines no other stop or player's list has", ("child", "Graveyard"): "4",
            ("child", "Death Mountain Crater"): "4 (the gameover part still goes there)", ("adult", "Shadow Temple"): "2",
            ("adult", "Dodongo's Cavern"): "6", ("adult", "Gerudo Training Ground"): "7", ("adult", "Haunted Wasteland"): "7",
            ("child", "Ice Cavern"): "6", ("child", "Ganon's Castle"): "9", ("child", "Spirit Temple"): "4"}
# Game Over: the "gameover" part (its own run: hearts are not protected there) takes Link, a child, to the crater, whose heat
# empties his hearts; A now and then chooses Continue on the Game Over screen
GAME_OVER_AT = "Death Mountain Crater"
GAME_OVER_FRAMES = 60 * 75     # (the heat takes a child's three hearts in ~45 s; Try Again, back in the crater by ~60 s)
GAME_OVER_PRESS_FROM = 60 * 40
# Stops the game does not teleport out of (the request is sent, the map does not change), visited last in their part
LAST = {"Haunted Wasteland"}
# Overworld stops seen at night too (the Debug tab's clock: !cheat 100 + hour), then back to noon. Measured on a short run: Hyrule
# Field +4 pipelines at night, Kakariko +10, for ~10 s each: none by default (add names here for a run that wants them)
NIGHT_AT = set()
TIME_ACTION = 100
# Where Link uses the sword, the shield and an item picked from the pause menu (a different one at each such stop of the whole
# route: item_stops). A pipeline is a program and its fixed draw state (formats, blending, depth); a scene's light and fog are
# inputs to the same programs, so the sword, the shield and an item draw with the same pipelines wherever they are used. Once an
# age (the child's and the adult's models and items differ), at the first stop of the age's overworld:
ITEMS_ONCE = {"child": "Kokiri Forest", "adult": "Hyrule Field"}
# and at the stops with passes of their own, which an item's draws (its effect, a projectile, Link holding it) could combine
# with into pipelines met nowhere else. The rest only look around
ITEMS_PASSES = {
    "Zora's Domain": "water: its surface and what is drawn through it, at both ages (the adult's Lake Hylia dives instead)",
    "Water Temple": "water around and above Link, at both ages",
    "Shadow Temple": "the Lens of Truth's pass (the child's visit: the adult's is left out)",
    "Death Mountain Crater": "heat haze (the adult's visit: the child's is left out)",
    "Fire Temple": "heat haze, at both ages",
}


def uses_items(age, name):
    return ITEMS_ONCE.get(age) == name or name in ITEMS_PASSES


def item_stops(a):
    """The stops where Link uses the sword, the shield and an item, in the whole route's order: [(age, index)]. A stop's place
    here is its pause menu pick (pause_pick), the same whichever game of the tour visits it"""
    return [(age, i) for age, i in plan_stops(a, "all") if uses_items(age, a["teleports"][i][0])]


def items_unlock(a):
    """Every item Link can have (kind 0: unlocked once), the quantities filled (kind 1: +60): the Debug tab's actions"""
    out = []
    for name, (i, kind) in a["items"].items():
        if name.startswith("Armor_Child_Mask_") and name != "Armor_Child_Mask_Truth":
            continue   # (one mask slot: the last one added is the one kept)
        if name.startswith(QUEST_ITEMS):
            continue   # (the story's flags: medallions, stones and songs start scenes that wait for the player)
        out.append(a["item"] + i * 256 + (200 if kind == 0 else 64 + 60))
    return out


# ---- the route ----
# Presses: <seconds>:<button>[+<button>][/<ms>] (docs/development.md, "Input for scripted runs"), in game time: the bridge's
# sequence with "units": "hos" (docs/control.md).
# The buttons as the remake has them: B the sword (held: the spin attack), R the shield, ZL a target, A a roll, ZR the item on the
# item button; PLUS the pause menu, whose pages ZL and ZR turn (Collection, Equipment, Items, System) and B closes; on its Items
# page the stick moves the cursor and A puts the item on the item button.

def presses(items):
    return ",".join("%g:%s" % (t, b) for t, b in items)


LOOK = presses([(0, "RLEFT/1500"), (1.5, "RUP/400"), (1.9, "RRIGHT/700"), (2.6, "RDOWN/700"), (3.3, "RUP/300"),
                (0, "LUP/600"), (0.6, "LDOWN/600")])
LOOK_SPAN = 3.7
ACT = presses([(0, "B"), (0.35, "B"), (0.7, "B/900"), (1.7, "R/500"), (2.3, "ZL/300"), (3.2, "ZR/600"),
               (4, "ZR"), (4.6, "ZR/600")])   # (no A: next to someone it starts a conversation)
ACT_SPAN = 5.4
# Presses that end what holds a map change back (a conversation, a question, an item's prompt), one at a time in turn
UNSTICK = ["0:A/100", "0:B/100", "0:LDOWN/150,0.3:A/100", "0:B/100", "0:LUP/150,0.3:A/100", "0:B/100,0.4:B/100"]


def pause_pick(k):
    """The pause menu's pages, then on the Items page the k-th item put on the item button (a cursor walk that differs from stop to
    stop), and closed. ZL three times first: from whatever page it opened on to the first, then ZR twice to Items"""
    seq, t = [(0, "PLUS")], 0.8
    for b in ("ZL", "ZL", "ZL", "ZR", "ZR"):
        seq.append((t, b))
        t += 0.45
    walk = ["LDOWN"] * (k % 5) + ["LRIGHT"] * ((k // 5) % 4)
    for b in walk:
        seq.append((t, b + "/120"))
        t += 0.25
    seq += [(t, "A"), (t + 0.4, "B"), (t + 0.9, "B")]
    return presses(seq), t + 1.3


PAUSE_TOUR = presses([(0, "PLUS"), (0.8, "ZR"), (1.5, "ZR"), (2.2, "ZR"), (2.9, "ZR"), (3.6, "ZL"), (4.3, "ZL"), (5, "ZL"),
                      (5.7, "ZL"), (6.4, "B"), (6.9, "B")])
PAUSE_SPAN = 7.4
PAUSE_PAGES = False   # (measured in #2048: 0 pipelines the item stops' own pause menu visits and the players' lists lack, for 8 s)

# Water: at these stops Link arrives swimming; A held dives (the scales are given). As adult the pause menu's Equipment page then
# puts on the iron boots and the Zora tunic (its cursor starts on the Echo Gem in a new game: up is the iron boots, right then down
# the tunics' row), the stop is reached again, and Link walks the lake's bottom; the boots come off after
DIVE_AT = {"Lake Hylia"}
DIVE = presses([(0, "A/4000")])
DIVE_SPAN = 4.5
UNDERWATER_GEAR = presses([(0, "PLUS"), (0.8, "ZL"), (1.2, "ZL"), (1.6, "ZL"), (2.0, "ZR"), (2.6, "UP"), (3.0, "A"), (3.5, "RIGHT"),
                           (3.9, "DOWN"), (4.3, "RIGHT"), (4.7, "RIGHT"), (5.1, "A"), (5.6, "B"), (6.1, "B")])
GEAR_SPAN = 6.6
BOOTS_OFF = presses([(0, "PLUS"), (0.8, "ZL"), (1.2, "ZL"), (1.6, "ZL"), (2.0, "ZR"), (2.6, "LEFT"), (3.0, "LEFT"), (3.4, "LEFT"),
                     (3.8, "A"), (4.3, "B"), (4.8, "B")])   # (from the Zora tunic: left three times is the iron boots again)
BOOTS_SPAN = 5.3


# A stop is left once no new pipeline came for SETTLE_QUIET frames (a frame a 60th of a second of game time, HOS_FPS_STEP=0:
# the faster the game, the less wall time), SETTLE_MAX frames at most. Before, 150 and 600 (2.5 s and 10 s of game time): the
# look around and the items come before it, so its quiet frames are what a stop waits for nothing (run --settle-quiet 150 for
# the old wait)
SETTLE_QUIET = 60
SETTLE_MAX = 600

PARTS = ("all", "child", "adult", "opening", "child-overworld", "child-dungeons", "adult-overworld", "adult-dungeons", "dungeons",
         "gameover")


def dungeons_from(a):
    """The first dungeon's index in the teleport list (vk_menu.cpp teleport_sections: "Dungeons")"""
    return a.get("dungeons", len(a["teleports"]))


def plan_stops(a, part="all", stops=None, first=None):
    """The destinations a part visits, in order: [(age, index)]. opening: none (the new game's opening alone, watched whole);
    child-overworld, dungeons (both ages) and adult-overworld split the route in three"""
    adult_only = {i for i, (_, only) in enumerate(a["teleports"]) if only}
    first_dungeon = dungeons_from(a)
    child = [("child", i) for i in range(len(a["teleports"])) if i not in adult_only]
    adult = [("adult", i) for i in range(len(a["teleports"]))]
    over = lambda plan: [(age, i) for age, i in plan if i < first_dungeon]
    under = lambda plan: [(age, i) for age, i in plan if i >= first_dungeon]
    plan = {"all": child + adult, "child": child, "adult": adult, "opening": [], "child-overworld": over(child),
            "dungeons": under(child) + under(adult), "adult-overworld": over(adult), "child-dungeons": under(child),
            "adult-dungeons": under(adult), "gameover": []}[part]
    plan = [(age, i) for age, i in plan if (age, a["teleports"][i][0]) not in LEFT_OUT]
    # (a stop the game will not teleport out of goes last: the next map change after it is the next run's)
    ages = [age for age, _ in plan]
    plan = sorted(plan, key=lambda x: (ages.index(x[0]), a["teleports"][x[1]][0] in LAST))   # (last within its age)
    if first and part not in ("gameover", "opening"):   # (a run that starts again at a stop: "<name>" or "<name> (<age>)")
        labels = ["%s (%s)" % (a["teleports"][i][0], age) for age, i in plan]
        at = [k for k, (lab, (age, i)) in enumerate(zip(labels, plan)) if first in (lab, a["teleports"][i][0])]
        if not at:
            raise SystemExit("pipeline_tour: no stop %r in this part" % first)
        plan = plan[at[0]:]
    return plan[:stops] if stops is not None else plan


def route(a, part="all", stops=None, first=None):
    """The steps of a run: ("newgame",), ("items",), ("press", text, span, label), ("age",), ("visit", age, index, k),
    ("gameover", index). The pause menu's pages once, in the opening part or a whole run"""
    steps = [("newgame",), ("items",)]
    if PAUSE_PAGES and part in ("all", "child", "opening"):
        steps.append(("press", PAUSE_TOUR, PAUSE_SPAN, "pause menu pages"))
    age = "child"
    for k, (want, i) in enumerate(plan_stops(a, part, stops, first)):
        if want != age:
            steps += [("age",), ("items",)]   # (the adult's equipment is only given to the adult: a child's add is lost)
            age = want
        steps.append(("visit", want, i, k))
    if part == "gameover":   # (hearts not protected: the crater's heat takes them, the Game Over screen, Continue)
        steps.append(("gameover", [n for n, _ in a["teleports"]].index(GAME_OVER_AT)))
    return steps


# ---- the list ----

def state_tool():
    spec = importlib.util.spec_from_file_location("pipeline_states", os.path.join(ROOT, "nx2", "pipeline_states.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_CTL = []


def ctl_module():
    """scripts/hookshot_ctl.py, the bridge's client: its Bridge (one connection, replies matched to requests) and Failed. Loaded
    once: Game.ask catches its Failed, which is another class in each load"""
    if not _CTL:
        spec = importlib.util.spec_from_file_location("hookshot_ctl", os.path.join(ROOT, "scripts", "hookshot_ctl.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _CTL.append(mod)
    return _CTL[0]


def list_counts(tool, path):
    """(pipelines, programs, records) of a list, as nx2/pipeline_states.py reads and counts them (whatever its format)"""
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return 0, 0, set()
    try:
        recs = tool.read(path)
    except SystemExit:
        return 0, 0, set()
    m = re.search(r"of (\d+) programs", tool.describe(recs))
    return len(recs), int(m.group(1)) if m else 0, set(recs)


# ---- memory ----
# One game holds 4 to 14 GB (the guest memory is shared memory, touched as the game goes, and not given back; the program cache's
# file pages come on top and can be dropped). This PC-wide guard keeps a run from taking the machine down: before a game starts
# there must be MIN_FREE_GB available and at most MAX_SWAP_GB of swap in use (else it waits, WAIT_S at most, then gives up); while
# it runs, the game's own memory (anonymous + shared: what cannot be dropped) and the machine's are read once a second, and
# - past SOFT_MB after a stop: the run ends there and the tour goes on in a fresh game (one game's memory only grows),
# - past HARD_MB, under FLOOR_GB available or past MAX_SWAP_GB + 4 of swap: the game is stopped at once, its list kept, and the
#   tour waits for memory and goes on from the stop it was at.
# On Linux with systemd the game also runs in a scope of its own with MemoryMax (SCOPE_GB) and little swap: should all of that
# fail, the kernel ends the game, not the desktop or the terminal it was started from.
MEM = {"min_free_gb": 8.0, "max_swap_gb": 16.0, "floor_gb": 3.0, "soft_mb": 9000, "hard_mb": 12000, "wait_s": 1800, "scope_gb": 16}


def meminfo(text=None):
    """/proc/meminfo: (GB available, GB of swap in use); (None, None) where there is none"""
    if text is None:
        try:
            with open("/proc/meminfo") as f:
                text = f.read()
        except OSError:
            return None, None
    kb = {}
    for line in text.splitlines():
        k, _, v = line.partition(":")
        if v.strip():
            kb[k] = int(v.split()[0])
    if "MemAvailable" not in kb:
        return None, None
    return kb["MemAvailable"] / 1048576, (kb.get("SwapTotal", 0) - kb.get("SwapFree", 0)) / 1048576


def held_mb(status_text):
    """A process's memory that cannot be dropped, MB (/proc/<pid>/status: RssAnon + RssShmem)"""
    kb = {}
    for line in status_text.splitlines():
        k, _, v = line.partition(":")
        if k in ("RssAnon", "RssShmem", "VmRSS"):
            kb[k] = int(v.split()[0])
    return (kb.get("RssAnon", 0) + kb.get("RssShmem", 0)) // 1024


def start_problem(mem, info):
    """Why a game may not start now (None: it may)"""
    free, swap = info
    if free is None:
        return None
    if free < mem["min_free_gb"]:
        return "%.1f GB available, a game wants %.0f" % (free, mem["min_free_gb"])
    if swap > mem["max_swap_gb"]:
        return "%.1f GB of swap in use, at most %.0f" % (swap, mem["max_swap_gb"])
    return None


def run_problem(mem, info, held):
    """Why a running game must stop at once (None: it may go on)"""
    free, swap = info
    if held is not None and held > mem["hard_mb"]:
        return "the game holds %d MB, at most %d" % (held, mem["hard_mb"])
    if free is not None and free < mem["floor_gb"]:
        return "%.1f GB available, under %.0f" % (free, mem["floor_gb"])
    if swap is not None and swap > mem["max_swap_gb"] + 4:
        return "%.1f GB of swap in use, over %.0f" % (swap, mem["max_swap_gb"] + 4)
    return None


def wait_memory(mem, read=meminfo, sleep=time.sleep, clock=time.monotonic, say=print):
    """Until a game may start: None, or why it still may not after mem["wait_s"]"""
    end, said = clock() + mem["wait_s"], None
    while True:
        why = start_problem(mem, read())
        if why is None:
            return None
        if clock() >= end:
            return why
        if why != said:
            say("pipeline tour: waits for memory: %s" % why, flush=True)
            said = why
        sleep(10)


def scope_prefix(mem, run=subprocess.run):
    """systemd-run's scope for the game (MemoryMax, a little swap), or [] where there is none"""
    if not mem.get("scope_gb") or not shutil.which("systemd-run") or os.environ.get("OOT_NO_SCOPE"):
        return []
    pre = ["systemd-run", "--user", "--scope", "--quiet", "--collect", "-p", "MemoryMax=%dG" % mem["scope_gb"],
           "-p", "MemorySwapMax=2G", "--"]
    try:
        if run(pre + ["true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20).returncode != 0:
            return []
    except (OSError, subprocess.TimeoutExpired):
        return []
    return pre


class MemoryStop(Exception):
    """The game stopped for memory (not a failure of the route: the tour goes on from `at` in a fresh game)"""
    def __init__(self, why, soft=False):
        Exception.__init__(self, why)
        self.soft = soft


class Failed(Exception):
    pass


def aside_name(rel):
    """A STATE_FILES path's name in the folder it is put aside in, and in the run's folder: cache/'s by their own names, a loose
    one beside the game that cache/ has too as loose_<name>"""
    if rel.startswith("cache/"):
        return rel[len("cache/"):]
    return "loose_" + rel if "cache/" + rel in STATE_FILES else rel


# ---- the renderer's play profile (--vk-profile) ----
# An RX_VK_PGO=gen renderer (nx2/build.py) counts its branches, and its copy of host/pgo_rt.c, reading RX_VK_PGO_FILE=<file>,
# looks every 500 ms for <file>.reset (the counters cleared) and <file>.dump (the counters written to <file>), deleting each once
# done. A run with --vk-profile clears them once the new game has given control of Link (the loading, the prepare and the
# title left out) and writes them at its end, failed or not, as <dir>/<run name>.profraw with <run name>.json (where it was
# recorded); merge-profile merges them with llvm-profdata (docs/development.md, "The renderer's play profile")
PGO_ENV = "RX_VK_PGO_FILE"
PGO_MARK = b"RX_VK_PGO_FILE\x00"   # (the counting renderer's getenv: only its copy of pgo_rt.c names it, -DPGO_ENV)
PGO_RAW_MAGIC = struct.pack("<Q", 0xff6c70726f667281)   # (LLVM's raw profile, host/pgo_rt.c pgo_write: \xfflprofr\x81)
PGO_WAIT_S = 10           # (the writer looks every 500 ms; a written file comes within a second)
RENDERERS = ("hookshot_vk.so", "hookshot_vk.dll")   # (beside the game's executable: host/rx_backend.c load_vulkan)
VK_PROFILE = os.path.join("assets", "profiles", "hookshot_vk.profdata.xz")
VK_PROFILE_WORK = os.path.join("work", "hookshot_vk.profdata")   # (what nx2/make_game.py builds with, before the committed one)


def drop(path):
    """path deleted if it is there (the renderer's writer may delete it at the same time)"""
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def counting_renderer(exe_dir):
    """The renderer beside the game's executable is an RX_VK_PGO=gen one (it reads RX_VK_PGO_FILE): its path, else None"""
    for name in RENDERERS:
        path = os.path.join(exe_dir, name)
        if not os.path.isfile(path):
            continue
        tail = b""
        with open(path, "rb") as f:
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                if PGO_MARK in tail + chunk:
                    return path
                tail = chunk[-len(PGO_MARK):]
    return None


class Profile:
    """The renderer's counters in one game: file is RX_VK_PGO_FILE's, out the --vk-profile folder, name the run's"""
    def __init__(self, file, out, name, wait_s=PGO_WAIT_S, sleep=time.sleep, clock=time.monotonic):
        self.file, self.out, self.name, self.wait_s, self.sleep, self.clock = file, out, name, wait_s, sleep, clock
        self.was_reset = False
        for stale in (file, file + ".reset", file + ".dump"):   # (an earlier run's: not this one's counters)
            drop(stale)

    def _until(self, done, wait_s):
        end = self.clock() + wait_s
        while not done():
            if self.clock() > end:
                return False
            self.sleep(0.1)
        return True

    def reset(self):
        """The counters cleared (from here on only play counts): Failed when the renderer does not take the request, as one
        that does not count never does"""
        open(self.file + ".reset", "w").close()
        if not self._until(lambda: not os.path.exists(self.file + ".reset"), self.wait_s):
            drop(self.file + ".reset")
            raise Failed("--vk-profile: the renderer did not take %s.reset in %g s: not an RX_VK_PGO=gen renderer" % (
                self.file, self.wait_s))
        self.was_reset = True

    def dump(self, wait_s=None):
        """The counters written and moved to <out>/<name>.profraw: its path, None when the renderer wrote nothing"""
        drop(self.file)
        open(self.file + ".dump", "w").close()
        written = lambda: not os.path.exists(self.file + ".dump") and os.path.isfile(self.file) and os.path.getsize(self.file) > 0
        if not self._until(written, self.wait_s if wait_s is None else wait_s):
            drop(self.file + ".dump")
            return None
        os.makedirs(self.out, exist_ok=True)
        to = os.path.join(self.out, self.name + ".profraw")
        shutil.move(self.file, to)
        return to


def profile_end(profile, alive, hard=False):
    """A run's end with --vk-profile: the counters written if the game is there and they were cleared after control.
    ({file, reset, why}, the run's error or None): a renderer that writes nothing fails the run, never an empty profile"""
    if profile is None:
        return None, None
    if not profile.was_reset:   # (no control of Link yet: the counters hold the loading and the title, not play)
        return {"file": None, "reset": False, "why": "the counters were never cleared (no control of Link): not kept"}, None
    if not alive:
        return {"file": None, "reset": True, "why": "the game ended before its counters were written"}, None
    to = profile.dump(3 if hard else None)   # (a hard memory stop: a few seconds at most)
    if to is None:
        why = "--vk-profile: the renderer wrote no counters to %s in %g s: not an RX_VK_PGO=gen renderer" % (
            profile.file, 3 if hard else profile.wait_s)
        return {"file": None, "reset": True, "why": why}, why
    return {"file": to, "reset": True, "why": None}, None


# ---- the game, through the bridge ----

class Game:
    """The game behind its control bridge: the bridge's replies (each carries the frame), what was sent, at which frame.
    bridge: an object with request(cmd, timeout=None, **args) -> reply dict (scripts/hookshot_ctl.py Bridge, or a test's).
    live: the live input file, for the Debug tab's inventory (the one thing the bridge does not do). list_path: the game's
    pipeline state list. proc: the game's process (None: not ours, as in a test)"""
    def __init__(self, bridge, run_dir, list_path, live=None, proc=None, mem=None, dry=False, cheat_gap=0.032):
        self.b, self.run_dir, self.list_path, self.live, self.proc = bridge, run_dir, list_path, live, proc
        self.mem, self.dry, self.cheat_gap = mem, dry, cheat_gap
        self.peak = 0           # (the most the game held, MB: anonymous + shared memory)
        self.checked = 0.0
        self.steps = open(os.path.join(run_dir, "steps.txt"), "w", encoding="utf-8")
        self.t0 = time.monotonic()
        self.frame = 0          # (the game's presents so far, as the last reply said)
        self.marks = []         # (the current step's parts: [(what, records so far)])
        self.last = None        # (the last status)
        self.profile = None     # (--vk-profile: the renderer's counters, Profile)

    def now(self):
        return time.monotonic() - self.t0

    def note(self, what):
        self.steps.write("%8.1f s %7d  %s\n" % (self.now(), self.frame, what))
        self.steps.flush()

    def alive(self):
        return self.dry or self.proc is None or self.proc.poll() is None

    def check(self):
        """The game still there, and within the memory guard's limits"""
        if not self.alive():
            raise Failed("the game ended")
        if self.mem and self.proc is not None and time.monotonic() - self.checked >= 1:
            self.checked = time.monotonic()
            why = run_problem(self.mem, meminfo(), self.held())
            if why:
                raise MemoryStop(why)

    def ask(self, cmd, timeout=None, **args):
        """One request; its reply (ok or not), the frame taken from it"""
        self.check()
        try:
            r = self.b.request(cmd, timeout=timeout, **args)
        except ctl_module().Failed as e:   # (the game closed the bridge, or did not answer: a crash, mostly in a wait)
            raise Failed("%s: the bridge: %s%s" % (cmd, e, "" if self.alive() else " (the game ended)"))
        if isinstance(r.get("frame"), int):
            self.frame = max(self.frame, r["frame"])
        return r

    def must(self, cmd, timeout=None, **args):
        r = self.ask(cmd, timeout=timeout, **args)
        if not r.get("ok"):
            raise Failed("%s: %s: %s" % (cmd, r.get("error"), r.get("message", "")))
        return r

    def status(self):
        """{map, event, loading, age, state, draws} from the bridge's status (event, loading and age None before Link was
        first seen)"""
        r = self.must("status")
        game = r.get("game") or {}
        self.last = {"map": r.get("map"), "event": game.get("event"), "loading": game.get("loading"), "age": game.get("age"),
                     "state": r.get("state"), "draws": r.get("draws") or 0, "version": r.get("version"),
                     "renderer": r.get("renderer"), "fresh": fresh(r)}
        return self.last

    def frames(self, n, limit_s=300):
        """n frames of the game, in pieces of two seconds at most (the memory guard looks between them)"""
        if self.dry:
            return
        end, until = time.monotonic() + limit_s, self.frame + n
        while self.frame < until:
            left = min(until - self.frame, 120)
            r = self.ask("wait", timeout=max(5, min(60, limit_s)), event="present", frames=left)
            if not r.get("ok") and r.get("error") != "timeout":
                raise Failed("wait: %s: %s" % (r.get("error"), r.get("message", "")))
            if time.monotonic() > end:
                raise Failed("%d frames did not come in %d s" % (n, limit_s))

    def press(self, script, label=None):
        """A press script in HOS_INPUT_SCRIPT's terms (seconds and milliseconds of game time), the reply once it is over"""
        self.note(label or script)
        if self.dry:
            return
        r = self.ask("sequence", script=script, units="hos", exclusive=True)
        if not r.get("ok"):
            raise Failed("sequence %s: %s: %s" % (script, r.get("error"), r.get("message", "")))

    def tap(self, buttons, frames=6):
        """A press not waited for (the title's A)"""
        if not self.dry:
            self.ask("press", buttons=buttons, frames=frames, wait=False)

    def cheat(self, action):
        """A Debug tab action through the live input file: one a live poll, the game keeping the last one a poll read. The game
        reads the file at a frame's input once 30 ms have passed since it last did (host/nn_hid.c live_input): so the next line
        waits those 30 ms (cheat_gap), then three presents, the first two of which can be of frames that read their input before
        the line was there (with the render thread, Threaded rendering, the game is a frame ahead of its presents). A frame
        slowed by a pipeline build is waited for, as a wall-clock pause alone did not"""
        self.note("!cheat %d" % action)
        if self.dry:
            return
        with open(self.live, "a", encoding="utf-8") as f:
            f.write("!cheat %d\n" % action)
        time.sleep(self.cheat_gap)
        self.frames(3)

    def held(self):
        """The game's memory that cannot be dropped, MB (its most so far kept in peak); None where it cannot be read"""
        try:
            with open("/proc/%d/status" % self.proc.pid) as f:
                mb = held_mb(f.read())
        except (OSError, AttributeError, ValueError):
            return None
        self.peak = max(self.peak, mb)
        return mb

    def memory(self):
        """The game's resident memory in MB (Linux): {rss, anon, shmem} (guest memory is shared: shmem), {} elsewhere"""
        out = {}
        try:
            with open("/proc/%d/status" % self.proc.pid) as f:
                for line in f:
                    k, _, v = line.partition(":")
                    if k in ("VmRSS", "RssAnon", "RssShmem"):
                        out[{"VmRSS": "rss", "RssAnon": "anon", "RssShmem": "shmem"}[k]] = int(v.split()[0]) // 1024
        except (OSError, AttributeError, ValueError):
            pass
        return out

    def list_len(self):
        """The records in the list so far (its size: a 12-byte head, then records of the size it gives)"""
        try:
            with open(self.list_path, "rb") as f:
                head = f.read(12)
            return (os.path.getsize(self.list_path) - 12) // max(1, struct.unpack_from("<I", head, 8)[0])
        except (OSError, struct.error):
            return 0

    def mark(self, what):
        """A step's part (arrived, looked, items used...): the list's length now, for `value --sub`"""
        self.marks.append((what, self.list_len()))

    def list_size(self):
        try:
            return os.path.getsize(self.list_path)
        except OSError:
            return 0

    def settle(self, quiet, most, step=15):
        """Frames until the list has had no new pipeline for `quiet` frames, `most` at most"""
        if self.dry:
            return
        start, size, since = self.frame, self.list_size(), self.frame
        while self.frame - start < most:
            self.frames(step)
            now = self.list_size()
            if now != size:
                size, since = now, self.frame
            elif self.frame - since >= quiet:
                return

    def calm(self, want, limit_frames, every=15):
        """Until the game's state is what is wanted (want(state)), no map loading and no event playing: that state"""
        start = self.frame
        while True:
            st = self.status()
            if st["map"] and want(st) and st["loading"] is False and st["event"] is False and st["fresh"]:
                return st
            if self.frame - start > limit_frames:
                raise Failed("not settled after %d frames (last state %s)" % (self.frame - start, st))
            self.frames(every)


STALE_FRAMES = 10   # (a status's game snapshot taken more presents than this before its reply: kept from before a load or a pause)


def fresh(r):
    """A status reply's game snapshot is of now (docs/control.md: compare game.frame with the reply's frame; a snapshot kept
    through a load says nothing of control)"""
    taken, now = (r.get("game") or {}).get("frame"), r.get("frame")
    return isinstance(taken, int) and isinstance(now, int) and 0 <= now - taken <= STALE_FRAMES


def calm_after(g, want, start, limit_frames):
    """g.calm with what is left of a step's frames"""
    return g.calm(want, max(60, limit_frames - (g.frame - start)))


# what in a refusal's message says that asking again will never do (host/hos_main.c oot_teleport's teleport.why: no map for
# Link's age there, no such destination, a request the game could not build, Link's age not in its data); the rest (an event
# playing, a map loading, another teleport in the last 8 s, the event manager not there yet) pass
PERMANENT = ("no such destination", "has no map", "could not be", "not in the game's data", "out of memory")
# (the refusal of a teleport within 8 s of the last, counted in wall time (hos_now_ns): waited out, not pressed at)
COOLDOWN = "another teleport is still in progress"


def map_change(g, cmd, want, limit_frames=60 * 60 * 3, arrive_s=60, reload=False, **args):
    """A teleport or the age switch through the bridge, asked again while the game is busy (refused: an event playing, a map
    loading, another teleport in the last 8 s; timeout: Link not being updated; cancelled), with presses that end what holds
    it back (a conversation, a prompt) once it has been busy for a while; done once the game is in the map wanted (want(state)),
    nothing loading and no event playing (the arrival's own scene over). Returns that state. A refusal that names something
    asking again cannot change ends it (Failed), as does a sent request that does not move Link five times"""
    if g.dry:
        g.note("%s %s" % (cmd, args))
        return {"map": "?"}
    start, busy_since, unstick, unmoved = g.frame, None, 0, 0
    while True:
        if g.frame - start > limit_frames:
            raise Failed("%s %s: not done after %d frames (last state %s)" % (cmd, args, g.frame - start, g.last))
        g.note("%s %s" % (cmd, args))
        r = g.ask(cmd, timeout=10, **args)
        if r.get("ok"):
            if cmd == "age" and r.get("sent") is False:   # (Link is that age already)
                return calm_after(g, want, start, limit_frames)
            if r.get("sent"):
                # (reload: a teleport to the map Link is in, whose wait for that map is met at once: the load's end instead)
                wait = {"event": "map", "map": r.get("map")} if cmd == "teleport" and not reload else {"event": "loading_end"}
                w = g.ask("wait", timeout=arrive_s, **wait)
                if w.get("ok"):
                    return calm_after(g, want, start, limit_frames)
                st = g.status()   # (the wait ran out: arrived anyway, or the game let the request go)
                if want(st):
                    return calm_after(g, want, start, limit_frames)
                unmoved += 1
                g.note("sent, not moved (accepted %s, %s)" % (r.get("accepted"), st))
                if unmoved >= 5:
                    raise Failed("%s %s: sent five times, the map did not change (as in the Haunted Wasteland)" % (cmd, args))
                g.frames(120)
                continue
            raise Failed("%s %s: an answer the tour does not know: %s" % (cmd, args, r))
        error, message = r.get("error"), r.get("message", "")
        if error == "refused" and any(p in message for p in PERMANENT):
            raise Failed("the game refused %s %s: %s" % (cmd, args, message))
        if error not in ("refused", "timeout", "cancelled", "busy"):
            raise Failed("%s %s: %s: %s" % (cmd, args, error, message))
        g.note("%s: %s" % (error, message))
        if error == "refused" and COOLDOWN in message:   # (the 8 s are wall time: nothing to press away, only to wait out)
            g.frames(60)
            continue
        busy_since = g.frame if busy_since is None else busy_since
        if g.frame - busy_since > 240:   # (an event that waits for a button: a conversation the tour started, a question)
            g.press(UNSTICK[unstick % len(UNSTICK)], "unstick")
            unstick += 1
        g.frames(30)


def teleport(g, index, banc, age, limit_frames=60 * 60 * 2):
    """The debug teleport to destination `index`, until Link stands in `banc` at `age`"""
    return map_change(g, "teleport", lambda st: st["map"] == banc and st["age"] == age, limit_frames, to=index)


def switch_age(g, age, limit_frames=60 * 60 * 3):
    return map_change(g, "age", lambda st: st["age"] == age, limit_frames, to=age)


def new_game(g, limit_s=1200, title_draws=15, first_frame_s=1800):
    """From no save: A at the title until New Game starts, then through the opening (A now and then: its text), until the game's
    state says Link is in a map with no event playing and nothing loading, three times in a row a second apart: control.
    Returns the phases' (frame, s)"""
    phases = {}
    if g.dry:
        g.note("new game")
        return phases
    end = time.monotonic() + limit_s
    # (a first start prepares the shaders before its first frame, and the game reads no controller until then)
    first = g.ask("wait", timeout=first_frame_s, event="frame", frame=1)
    if not first.get("ok"):
        raise Failed("new game: no first frame in %d s (%s)" % (first_frame_s, first.get("message", "")))
    next_a, calm = 0, 0
    while calm < 3:
        st = g.status()
        if "title" not in phases and (st["draws"] >= title_draws or st["state"] in ("menu", "play")):
            phases["title"] = (g.frame, round(g.now(), 1))
            g.note("title")
        if "title" in phases:
            if st["map"] and "player" not in phases:
                phases["player"] = (g.frame, round(g.now(), 1))
                g.note("Link is there: %s" % st)
            calm = calm + 1 if st["map"] and st["event"] is False and st["loading"] is False and st["fresh"] else 0
            if calm == 0 and g.frame >= next_a:
                g.tap("A")
                next_a = g.frame + 45
        if time.monotonic() > end:
            raise Failed("new game: %s after %d s (last state %s)" % ("no control" if "title" in phases else "no title", limit_s, st))
        g.frames(60 if calm else 20)
    phases["control"] = (g.frame, round(g.now(), 1))
    g.note("control: %s" % g.last)
    return phases


def game_exe(game):
    """The game's executable in its folder: bin/'s whenever there is a bin/ (a builder's folder, whose top-level hookshot starts
    the launcher), else the folder's own (a source build); as scripts/hookshot_ctl.py game_exe"""
    return ctl_module().game_exe(game)


def start_game(game, run_dir, data, env, prefix=(), timeout=900, ctl=None):
    """The game in its folder with the bridge on (a socket in a 0700 folder of the run's own, docs/control.md) and the live input
    file: (process, bridge, live file path). Waits until the bridge answers (a first start prepares the shaders first)"""
    ctl = ctl or ctl_module()
    sock_dir = os.path.join(run_dir, "ctl")
    os.makedirs(sock_dir, mode=0o700, exist_ok=True)
    os.chmod(sock_dir, 0o700)
    sock = os.path.join(sock_dir, "control.sock")
    if len(sock.encode()) > 107:   # (a Unix socket's path is at most 107 bytes)
        raise SystemExit("pipeline_tour: %s is too long for a socket: give a shorter --out" % sock)
    live = os.path.join(run_dir, "live.txt")
    open(live, "w").close()
    user = os.path.join(data, "user")   # (the saves: the run's own, whatever the folder's user/ is or links to)
    os.makedirs(user, exist_ok=True)
    no_env = os.path.join(run_dir, "empty-env.txt")   # (HOS_ENV_FILE: not the folder's hookshot_env.txt, as hookshot_ctl's test profile)
    open(no_env, "w").close()
    full = run_environ(os.environ, env)
    full.update({"XDG_DATA_HOME": data, "HOS_INPUT_LIVE": live, "HOS_CONTROL": sock, "HOS_ENV_FILE": no_env})
    exe = game_exe(game)
    with open(os.path.join(run_dir, "log.txt"), "wb") as logf:   # (the game has its own copy of the handle)
        proc = subprocess.Popen(list(prefix) + [exe, "--user", user], cwd=game, env=full, stdout=logf, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True)
    end = time.monotonic() + timeout
    try:
        while True:
            if proc.poll() is not None:
                raise Failed("the game ended (%d) before its bridge answered: %s" % (proc.returncode, os.path.join(run_dir, "log.txt")))
            try:
                b = ctl.Bridge(sock, 5)
                if b.request("status", timeout=5).get("ok"):
                    return proc, b, live
                b.close()
            except ctl.Failed:
                pass
            if time.monotonic() > end:
                raise Failed("the game's bridge did not answer in %d s" % timeout)
            time.sleep(0.1)   # (no frames to count yet: the bridge is not there)
    except BaseException:   # (no bridge, or Ctrl+C while waiting: the game is not left running in the folder put back under it)
        stop_game(proc, None, 3)
        raise


def run_environ(current, env):
    """The game's environment: this one's without its own Hookshot switches (a player's RX_VK_NO_STATE_LIST=1 in the shell would
    change the list unsaid), then the run's (env, which report.json records)"""
    full = {k: v for k, v in current.items() if not k.startswith(("HOS_", "RX_", "A64_", "OOT_"))}
    full.update(env)
    return full


def stop_game(proc, bridge=None, grace=15):
    """The game asked to quit through the bridge (it flushes its log and lists as when its window closes), then ended"""
    if proc is None or proc.poll() is not None:
        return
    if bridge is not None:
        try:
            bridge.request("quit", timeout=5)
        except Exception:   # (the game may be gone, or not answer: it is ended below)
            pass
    try:
        proc.wait(grace)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(grace)
    except (subprocess.TimeoutExpired, ProcessLookupError):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()


# ---- a run ----

def set_ini(path, pairs):
    """The options file with these keys set: the original's text, to put back (None: there was none, restore_ini removes it)"""
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    lines = (old or "").splitlines()
    for key, value in pairs:
        for k, line in enumerate(lines):
            if line.split("=", 1)[0] == key:
                lines[k] = "%s=%s" % (key, value)
                break
        else:
            lines.append("%s=%s" % (key, value))
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return old


def restore_ini(path, old):
    """The options file as set_ini found it: its text, or no file (a folder never played keeps none of the run's options)"""
    if old is None:
        if os.path.exists(path):
            os.remove(path)
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write(old)


def set_aside(game, aside):
    """The folder's pipeline notes and lists (STATE_FILES) moved into aside (in the game folder: a rename), so only what the run
    meets counts"""
    os.makedirs(aside)
    for name in STATE_FILES:
        if os.path.exists(os.path.join(game, name)):
            os.replace(os.path.join(game, name), os.path.join(aside, aside_name(name)))


def put_back(game, aside, run_dir):
    """The run's notes and lists moved into its folder (shutil.move: --out may be on another drive), then the folder's own put
    back from aside, each file on its own: one that cannot move is named and the rest still go. aside is removed once empty.
    Returns the problems ([] when all went back)"""
    problems = []
    for name in STATE_FILES:
        mine, kept = os.path.join(game, name), os.path.join(aside, aside_name(name))
        try:
            if os.path.exists(mine):
                shutil.move(mine, os.path.join(run_dir, aside_name(name)))
            if os.path.exists(kept):
                os.replace(kept, mine)
        except OSError as e:
            problems.append("%s: %s" % (name, e))
    try:
        os.rmdir(aside)
    except OSError as e:
        problems.append("%s left: %s (its files go back to %s by their names)" % (aside, e, game))
    return problems


def write_report(run_dir, rows, meta):
    total = rows[-1]["pipelines"] if rows else 0
    out = ["pipeline tour %s%s: %d pipelines, %d programs, %.0f s, %s frames%s" % (
        meta["name"], "" if meta.get("completed", True) else " (NOT COMPLETED: %s)" % (meta.get("error") or
                                                                                    "memory, %d stops left" % len(meta.get("left") or [])), total,
        rows[-1]["programs"] if rows else 0, meta["seconds"], meta.get("frames", "?"),
        ", %.0f frames a second" % meta["fps"] if meta.get("fps") else "")]
    if meta.get("peak_mb") or meta.get("memory_stop"):
        out.append("memory: the game held %s MB at most%s" % (meta.get("peak_mb") or "?", "; stopped: %s; left: %s" % (
            meta["memory_stop"], ", ".join(meta.get("left") or []) or "none") if meta.get("memory_stop") else ""))
    if "vk_profile" in meta:
        vk = meta["vk_profile"] or {}
        out.append("renderer profile: %s" % (vk.get("file") or "none (%s)" % (vk.get("why") or "the game did not start")))
    if meta.get("settle"):
        out.append("settle: on after %d frames with no new pipeline, %d at most" % tuple(meta["settle"]))
    out.append("settings: %s" % " ".join("%s=%s" % kv for kv in sorted(meta["env"].items())))
    if meta.get("ini"):
        out.append("options: %s" % " ".join(meta["ini"]))
    out.append("%7s %7s %7s %7s %6s %6s %6s %6s  %s" % ("at s", "took s", "frames", "total", "new", "progs", "rss MB", "shm MB", "step"))
    for r in rows:
        mem = r.get("memory") or {}
        out.append("%7.0f %7.1f %7d %7d %6d %6d %6s %6s  %s" % (r["seconds"], r.get("took", 0), r.get("frames", 0), r["pipelines"],
                                                             r["new"], r["programs"], mem.get("rss", "-"), mem.get("shmem", "-"),
                                                             r["stop"]))
    text = "\n".join(out) + "\n"
    with open(os.path.join(run_dir, "report.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    with open(os.path.join(run_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "stops": rows}, f, indent=1)
    return text


def run_env(args):
    """The run's switches: BASE_ENV, a window instead of none, hearts unprotected for the Game Over, the opening skipped by the
    game unless this run watches it, --env over them all (NAME=- drops one)"""
    env = dict(BASE_ENV)
    if args.window:   # (a window: no headless run, the window's size; the output stays 640x360, scaled into the window)
        env.pop("HOS_HEADLESS", None)
        env["HOS_WINDOW_SIZE"] = args.window
        env["HOS_WINDOW_NOACTIVATE"] = "1"
    if args.part == "gameover":
        env["HOS_OOT_PROTECT_HEARTS"] = "0"
    env["HOS_SKIP_OPENING"] = "1" if args.skip_opening else "0"
    for kv in args.env:
        k, _, v = kv.partition("=")
        if v == "-":
            env.pop(k, None)
        else:
            env[k] = v
    return env


def play(g, a, steps, rows_out, count, args):
    """The route's steps on game g. count(label) adds a report row. args: settle_quiet and settle_max, a stop's settle (frames)"""
    misses = 0
    dived = False
    quiet, most = getattr(args, "settle_quiet", SETTLE_QUIET), getattr(args, "settle_max", SETTLE_MAX)
    picks = item_stops(a)
    for at, step in enumerate(steps):
        g.at = at
        kind = step[0]
        if g.mem and not g.dry and kind == "visit" and at and stop_labels(a, steps[:at]):
            held = g.held()   # (between stops: a game past the soft limit ends here, a fresh one does the rest)
            if held is not None and held > g.mem["soft_mb"]:
                raise MemoryStop("the game holds %d MB after a stop, a fresh game from here on (over %d)" % (held, g.mem["soft_mb"]),
                                 soft=True)
        if kind == "newgame":
            phases = new_game(g)
            if g.profile is not None and not g.dry:   # (control: the counters from here on are play's)
                g.profile.reset()
                g.note("vk profile: counters cleared")
            count("new game: title, opening, Link's house")
            rows_out[-1]["phases"] = phases
        elif kind == "items":
            for n in items_unlock(a):   # (one a live poll: the game keeps the last action a poll read, host/nn_hid.c)
                g.cheat(n)
            g.frames(30)
            count("every item")
        elif kind == "press":
            g.press(step[1], step[3])
            g.settle(30, 120)
            count(step[3])
        elif kind == "age":
            switch_age(g, "adult")
            count("age switched: adult")
        elif kind == "gameover":
            banc = a["bancs"][step[1]][0]
            teleport(g, step[1], banc, "child")
            count("%s (child), hearts not protected" % GAME_OVER_AT)
            end = g.frame + GAME_OVER_FRAMES
            while g.frame < end and not g.dry:   # (the heat; then A: Continue)
                g.frames(180)
                if g.frame > end - GAME_OVER_FRAMES + GAME_OVER_PRESS_FROM:
                    g.tap("A")
            count("Game Over")
        elif kind == "visit":
            _, age, i, k = step
            name = a["teleports"][i][0]
            banc = a["bancs"][i][0 if age == "child" else 1]
            try:
                where = teleport(g, i, banc, age)
            except Failed as e:   # (one stop missed, said in the report; three in a row end the run)
                misses += 1
                g.note("missed: %s" % e)
                count("%s (%s): MISSED" % (name, age))
                if misses >= 3:
                    raise Failed("three stops in a row missed: %s" % e)
                continue
            misses = 0
            g.note("arrived: %s" % where)
            g.mark("arrive")
            if name in DIVE_AT:
                g.press(DIVE, "dive")
            g.press(LOOK, "look")
            g.mark("look")
            if name in DIVE_AT and age == "adult" and not dived:
                dived = True
                count("%s (%s)" % (name, age))
                g.press(UNDERWATER_GEAR, "iron boots, Zora tunic")
                try:   # (arrived again, in the water: with the boots Link sinks to the bottom)
                    map_change(g, "teleport", lambda st: True, 60 * 60, reload=True, to=i)
                    g.frames(300)
                    g.press(LOOK, "look")
                    g.settle(quiet, most)
                    count("%s (%s) underwater" % (name, age))
                except Failed as e:
                    g.note("underwater missed: %s" % e)
                g.press(BOOTS_OFF, "boots off")
                continue
            if (age, i) in picks:   # (once an age, and where a pass of the stop's own could meet them: ITEMS_ONCE, ITEMS_PASSES)
                k = picks.index((age, i))
                pick, _ = pause_pick(k)
                g.press(pick, "item %d from the pause menu" % (k + 1))
                g.press(ACT, "sword, shield, item")
                g.mark("items")
            g.settle(quiet, most)   # (on once no new pipeline came for `quiet` frames, `most` at most)
            count("%s (%s)" % (name, age))
            if name in NIGHT_AT and i < dungeons_from(a):
                g.cheat(TIME_ACTION + 22)
                g.frames(60)
                g.press(LOOK, "look at night")
                g.settle(quiet, most)
                count("%s (%s) at night" % (name, age))
                g.cheat(TIME_ACTION + 12)
                g.frames(60)


def run(args):
    """One game of the route (main sends --dry-run to dry_route). The game folder is changed only inside the try whose finally
    puts it back: the notes and lists aside, the options file; what can fail before that (--cpus, the memory wait) fails first"""
    a = actions()
    steps = route(a, args.part, args.stops, args.first)
    run_dir = os.path.join(os.path.abspath(args.out), args.name)
    os.makedirs(run_dir, exist_ok=True)
    for old in ("states.bin", "report.json", "report.txt"):   # (an earlier tour's, with the same --out and --name: not this run's)
        if os.path.exists(os.path.join(run_dir, old)):
            os.remove(os.path.join(run_dir, old))
    env = run_env(args)
    game = os.path.abspath(args.game)
    vk_out = os.path.abspath(args.vk_profile) if getattr(args, "vk_profile", None) else None
    if vk_out:
        refuse_uncounted(game)
        env[PGO_ENV] = os.path.join(run_dir, "vk.profraw")   # (the renderer's counters: written here, then moved to <vk_out>/<name>.profraw)
    if not any(kv.startswith("RX_VK_SHOTS=") for kv in args.env):   # (--env RX_VK_SHOTS=-: none)
        env["RX_VK_SHOTS"] = os.path.join(run_dir, "shots")
    if env.get("RX_VK_SHOTS"):
        os.makedirs(env["RX_VK_SHOTS"], exist_ok=True)
    cpus = cpu_list(args.cpus) if args.cpus else None
    if hasattr(os, "sched_setaffinity"):   # (the game inherits them)
        if cpus:
            try:
                os.sched_setaffinity(0, cpus)
            except OSError as e:
                raise SystemExit("pipeline_tour: --cpus %s: %s" % (args.cpus, e))
        os.nice(max(0, args.nice - os.nice(0)))
    tool = state_tool()
    mem = mem_settings(args)
    prefix = []
    if mem:
        why = wait_memory(mem)
        if why:   # (nothing touched yet: the tour may try again later)
            print("pipeline tour %s: not started, memory: %s" % (args.name, why), flush=True)
            with open(os.path.join(run_dir, "report.json"), "w", encoding="utf-8") as f:
                json.dump({"meta": {"name": args.name, "part": args.part, "completed": False, "error": "memory: " + why,
                                    "memory_wait": why, "left": stop_labels(a, steps)}, "stops": []}, f, indent=1)
            return 3
        prefix = scope_prefix(mem)
    aside = os.path.join(game, ".pipeline_tour_aside")
    if os.path.exists(aside):
        raise SystemExit("pipeline_tour: %s exists (an earlier run did not end): put its files back first" % aside)
    data = os.path.join(run_dir, "data")
    ini = dict(BASE_INI)
    ini.update(kv.split("=", 1) for kv in args.ini)
    rows, seen = [], set()
    proc = bridge = g = profile_meta = None
    touched, old_ini = False, None
    error, memory_stop, left, hard, interrupted = None, None, [], False, False
    try:
        touched = True
        set_aside(game, aside)
        old_ini = set_ini(os.path.join(game, OPTIONS), sorted(ini.items()))
        shutil.rmtree(data, ignore_errors=True)
        os.makedirs(data)   # (no save: the run makes its own)
        proc, bridge, live = start_game(game, run_dir, data, env, prefix)
        g = Game(bridge, run_dir, os.path.join(game, LIST), live, proc, mem)
        if vk_out:
            g.profile = Profile(env[PGO_ENV], vk_out, args.name)
        g.at = 0
        mark = [0.0, 0]

        def count(label):
            nonlocal seen
            n, progs, recs = list_counts(tool, g.list_path)
            rows.append({"stop": label, "seconds": round(g.now(), 1), "took": round(g.now() - mark[0], 1), "frames": g.frame - mark[1],
                         "pipelines": n, "programs": progs, "new": len(recs - seen), "memory": g.memory(), "marks": g.marks})
            g.marks = []
            seen |= recs
            g.note("%s: %d pipelines (+%d), %d programs" % (label, n, rows[-1]["new"], progs))
            mark[0], mark[1] = g.now(), g.frame

        play(g, a, steps, rows, count, args)
    except MemoryStop as e:
        memory_stop, hard = str(e), not e.soft
        left = stop_labels(a, steps[g.at:])   # (the stop it was at, again: a hard stop cut it short)
        g.note("MEMORY: %s; left: %s" % (memory_stop, left))
        print("pipeline tour %s: memory: %s" % (args.name, memory_stop), flush=True)
        if not left:
            error = "memory: " + memory_stop
    except Failed as e:
        error = str(e)
        if g:
            g.note("FAILED: " + error)
    except KeyboardInterrupt:   # (Ctrl+C, or SIGTERM: stopped. The game is ended and the folder put back, and the report written)
        error, interrupted = "stopped (Ctrl+C or SIGTERM)", True
        if g:
            g.note("STOPPED")
    finally:
        hold_stops()
        if g is not None and g.profile is not None:   # (failed or not: what the game counted since control)
            try:
                profile_meta, why = profile_end(g.profile, proc is not None and proc.poll() is None, hard)
            except OSError as e:
                profile_meta, why = {"file": None, "reset": g.profile.was_reset, "why": str(e)}, "--vk-profile: %s" % e
            if why and error is None:
                error = why
            g.note("vk profile: %s" % profile_meta)
        stop_game(proc, bridge if not hard else None, 3 if hard else 15)
        if bridge is not None:
            bridge.close()
        if touched:
            if os.path.exists(os.path.join(game, LIST)):
                shutil.copyfile(os.path.join(game, LIST), os.path.join(run_dir, "states.bin"))
            problems = put_back(game, aside, run_dir)
            try:
                restore_ini(os.path.join(game, OPTIONS), old_ini)
            except OSError as e:
                problems.append("%s: %s" % (OPTIONS, e))
            for p in problems:
                print("pipeline tour %s: NOT PUT BACK: %s" % (args.name, p), flush=True)
            if problems and error is None:
                error = "the game folder was not all put back: " + "; ".join(problems)
    seconds = round(g.now(), 1) if g else 0
    frames = g.frame if g else 0
    last = (g.last or {}) if g else {}
    meta = {"name": args.name, "part": args.part, "seconds": seconds, "frames": frames, "env": env, "ini": args.ini,
            "completed": error is None and not left, "error": error, "fps": frames / seconds if seconds else None,
            "memory_stop": memory_stop, "memory_soft": memory_stop is not None and not hard, "left": left,
            "peak_mb": (g.peak or None) if g else None, "version": last.get("version"), "renderer": last.get("renderer"),
            "settle": [getattr(args, "settle_quiet", SETTLE_QUIET), getattr(args, "settle_max", SETTLE_MAX)]}
    if vk_out:
        meta["vk_profile"] = profile_meta
        if profile_meta and profile_meta.get("file"):   # (where the counters were recorded, for merge-profile's note)
            with open(os.path.join(vk_out, args.name + ".json"), "w", encoding="utf-8") as f:
                json.dump({k: meta[k] for k in ("name", "part", "seconds", "frames", "completed", "error", "version", "renderer")},
                          f, indent=1)
    print(write_report(run_dir, rows, meta), end="")
    if interrupted:
        return 130
    return 1 if error is not None else 4 if left else 0


def stop_labels(a, steps):
    """The stops of steps, as --first takes them: "<name> (<age>)" """
    return ["%s (%s)" % (a["teleports"][step[2]][0], step[1]) for step in steps if step[0] == "visit"]


def mem_settings(args):
    """The memory guard's limits (MEM, --mem key=value over them), None with --no-mem-guard"""
    if getattr(args, "no_mem_guard", False):
        return None
    mem = dict(MEM)
    for kv in getattr(args, "mem", []) or []:
        k, _, v = kv.partition("=")
        if k not in mem:
            raise SystemExit("pipeline_tour: --mem %s: one of %s" % (kv, ", ".join(sorted(MEM))))
        mem[k] = type(MEM[k])(float(v))
    return mem


def cpu_list(text):
    """"0-3,12-15" -> {0, 1, 2, 3, 12, 13, 14, 15}; anything else refused (before the run touches the game folder)"""
    out = set()
    for piece in text.split(","):
        lo, _, hi = piece.strip().partition("-")
        try:
            first, last = int(lo), int(hi or lo)
        except ValueError:
            raise SystemExit("pipeline_tour: --cpus %s: CPU numbers and ranges, as 0-3,12-15" % text)
        if first < 0 or last < first:
            raise SystemExit("pipeline_tour: --cpus %s: %s is not a range" % (text, piece))
        out.update(range(first, last + 1))
    return out


def refuse_uncounted(game):
    """--vk-profile with a renderer that does not count: refused before anything is touched"""
    exe = game_exe(game)
    if not counting_renderer(os.path.dirname(exe)):
        raise SystemExit("pipeline_tour: --vk-profile: the renderer beside %s does not count (no %s in %s): make a counting "
                         "runtime (RX_VK_PGO=gen python -I scripts/dev_packs.py pack <base> vk-count, then python -I "
                         "scripts/dev_packs.py rt <base> <runtime> vk-count) and tour that" % (exe, PGO_ENV, " or ".join(RENDERERS)))


def chunks(a, part, size, limit=None):
    """A part's stops in runs of `size` (a fresh game each: one game's memory grows by about 1 GB an area and is not given back):
    [(first stop's label or None, stops)]. The opening part, or size 0: one run"""
    plan = plan_stops(a, part, limit)
    if not plan:
        return [(None, None)]
    if not size:
        size = len(plan)
    return [("%s (%s)" % (a["teleports"][plan[k][1]][0], plan[k][0]), min(size, len(plan) - k)) for k in range(0, len(plan), size)]


def part_runs(a, part, size, start, say=print, limit=None):
    """A part's games, one after another: its chunks of `size` stops, each a fresh game; a game the memory guard ended (start's
    code 4, or 3: not started) goes on from the stops it left in another fresh game, and a stop the guard ended twice in a row is
    left out. start(first, stops, k) runs game k and returns (code, its report's meta). Returns [(k, code)]; it ends early when
    a game could not start for memory or failed"""
    out, k = [], 0
    for first, stops in chunks(a, part, size, limit):
        stuck = None
        while True:
            code, meta = start(first, stops, k)
            out.append((k, code))
            k += 1
            left = meta.get("left") or []
            if code == 3:
                say("tour: part %s ends: no memory to start a game (%s)" % (part, meta.get("memory_wait")), flush=True)
                return out
            if code != 4 or not left:
                break
            if left[0] == stuck:   # (the same stop ended by the guard twice: left out)
                say("tour: %s left out: the game ran out of memory there twice" % left[0], flush=True)
                left = left[1:]
                if not left:
                    break
            stuck = left[0] if meta.get("memory_stop") and not meta.get("memory_soft") else None
            first, stops = left[0], len(left)
        if code not in (0, 4):
            return out
    return out


def run_command(args, part, name, first, stops, skip_opening):
    """The `run` a tour starts for one game of a part"""
    cmd = [sys.executable, "-I", os.path.abspath(__file__), "run", "--game", args.copy, "--out", args.out_dir, "--name", name,
           "--part", part, "--nice", str(args.nice), "--settle-quiet", str(args.settle_quiet), "--settle-max",
           str(args.settle_max)] + sum((["--env", e] for e in args.env), []) + \
        sum((["--ini", i] for i in args.ini), []) + sum((["--mem", m] for m in args.mem), [])
    if args.no_mem_guard:
        cmd.append("--no-mem-guard")
    if first:
        cmd += ["--first", first, "--stops", str(stops)]
    if skip_opening:
        cmd.append("--skip-opening")
    if args.cpus:
        cmd += ["--cpus", args.cpus]
    if args.window:
        cmd += ["--window", args.window]
    if getattr(args, "vk_profile", None):
        cmd += ["--vk-profile", os.path.abspath(args.vk_profile)]
    return cmd


def run_child(cmd, grace=120, popen=subprocess.Popen):
    """A `run`, waited for; its code. Stopped (Ctrl+C, SIGTERM) while it runs: it is sent SIGTERM (Ctrl+C reached it already when
    it came from the terminal) and given `grace` seconds to end its game, dump the profile and put the folder back, before it is
    killed; then the stop goes on. (subprocess.run would kill it a quarter of a second in, its game left running)"""
    child = popen(cmd)
    try:
        return child.wait()
    except KeyboardInterrupt:
        end = time.monotonic() + grace
        if TERMINATED:   # (Ctrl+C reached the run itself: a second signal would only be held)
            try:
                child.send_signal(signal.SIGTERM)
            except OSError:
                pass
        while True:
            try:
                child.wait(max(0.1, end - time.monotonic()))
                break
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                break
            except KeyboardInterrupt:   # (pressed again: still waited for, the folder is being put back)
                print("tour: waiting for the run to put the game folder back (%.0f s at most)" % max(0, end - time.monotonic()),
                      flush=True)
        raise


def tour(args):
    """The parts one after another (one game at a time: one game can hold 14 GB), each in a reflinked copy of the game folder
    and in games of --chunk stops, under the memory guard; then merged"""
    a = actions()
    parts = args.parts.split(",")
    for part in parts:
        if part not in PARTS:
            raise SystemExit("pipeline_tour: no part %r (%s)" % (part, ", ".join(PARTS)))
    if getattr(args, "vk_profile", None):   # (refused before the copy, as each run would refuse it)
        args.vk_profile = os.path.abspath(args.vk_profile)
        refuse_uncounted(os.path.abspath(args.game))
        os.makedirs(args.vk_profile, exist_ok=True)
        had = [n for n in os.listdir(args.vk_profile) if n.endswith(".profraw")]
        if had:
            print("tour: %s holds %d .profraw files already (merge-profile takes every one): %s" % (
                args.vk_profile, len(had), ", ".join(sorted(had))), flush=True)
    args.out_dir = out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    t0 = time.monotonic()
    args.copy = copy = os.path.abspath(args.game).rstrip("/") + "-tour"
    if os.path.exists(copy):   # (an earlier tour's, kept or cut short: maybe of another build, maybe with its files still aside)
        raise SystemExit("pipeline_tour: %s exists (an earlier tour's copy): remove it, and the tour copies the game folder again"
                         % copy)
    if subprocess.run(["cp", "-a", "--reflink=always", os.path.abspath(args.game), copy]).returncode:
        shutil.rmtree(copy, ignore_errors=True)
        raise SystemExit("pipeline_tour: the game folder could not be copied with --reflink=always (a copy-on-write copy, on btrfs "
                         "or XFS): put it on such a drive, as a full copy of the game per tour is not made")
    results = []

    def start_for(part):
        def start(first, stops, k):
            name = "%s-%s" % (args.name, part) + ("-%d" % k if first or k else "")
            # (the opening is watched whole once: by its own part, else by the first part's first game)
            skip = part != "opening" and ("opening" in parts or part != parts[0] or k > 0)
            print("tour: %s" % name, flush=True)
            code = run_child(run_command(args, part, name, first, stops, skip))
            results.append((part, os.path.join(out, name), code))
            try:
                with open(os.path.join(out, name, "report.json"), encoding="utf-8") as f:
                    meta = json.load(f)["meta"]
            except (OSError, ValueError, KeyError):
                meta = {}
            return code, meta
        return start

    try:
        for part in parts:
            runs = part_runs(a, part, args.chunk, start_for(part), limit=args.stops)
            if runs and runs[-1][1] == 3:
                break   # (no memory: the tour ends here, with what it has merged)
    finally:
        if not args.keep:
            shutil.rmtree(copy, ignore_errors=True)
    rc = 0 if all(code in (0, 4) for _, _, code in results) else 1
    order = sorted(results, key=lambda r: (parts.index(r[0]), r[1]))
    have = [os.path.join(d, "states.bin") for _, d, _ in order if os.path.exists(os.path.join(d, "states.bin"))]
    merged = os.path.join(out, "%s.bin" % args.name)
    if have:
        merge(argparse.Namespace(out=merged, lists=have))
        if getattr(args, "compare_to", None):   # (a faster or changed tour against an older one's list: what it lost)
            compare(argparse.Namespace(new=merged, old=args.compare_to))
    if args.publish and have and (not rc or args.publish_partial):
        rc = publish(have, [d for _, d, _ in order], parts)
    elif args.publish:
        print("tour %s: not published (a run failed or made no list: --publish-partial publishes anyway)" % args.name)
    print("tour %s: %.0f s in all, parts %s, %d runs%s" % (args.name, time.monotonic() - t0, ",".join(parts), len(results),
                                                         "" if not rc else " (a run failed)"))
    if getattr(args, "vk_profile", None):
        raws = sorted(n for n in os.listdir(args.vk_profile) if n.endswith(".profraw")) if os.path.isdir(args.vk_profile) else []
        print("tour %s: the renderer's counters of %d games in %s; merged by: python -I %s merge-profile %s [--publish]" % (
            args.name, len(raws), args.vk_profile, os.path.relpath(os.path.abspath(__file__), ROOT), args.vk_profile))
    return rc


NOT_SETTINGS = (PGO_ENV, "RX_VK_SHOTS")   # (paths of the run's, in the committed tour.md otherwise)


def published_settings(env):
    """The run's switches tour.md names: the output, the frame rate and the renderer's, without the ones that are paths of the run"""
    return {k: v for k, v in env.items() if k.startswith(("HOS_OUTPUT", "HOS_FPS", "RX_VK")) and k not in NOT_SETTINGS
            and not os.path.isabs(v)}


def publish(lists, run_dirs, parts, sources=None):
    """nx2/pipeline_states.py tour: the repository's tour list replaced by these, its note from what the runs' bridges said
    (the build, the renderer: GPU and driver) and their settings"""
    gpu = build = ""
    settings, ini, minutes = {}, None, 0.0
    for d in run_dirs:
        try:
            with open(os.path.join(d, "report.json"), encoding="utf-8") as f:
                meta = json.load(f)["meta"]
        except (OSError, ValueError, KeyError):
            continue
        gpu = gpu or meta.get("renderer") or ""
        build = build or meta.get("version") or ""
        if not gpu:   # (a run from before its renderer answered: the log's device line)
            try:
                log = open(os.path.join(d, "log.txt"), encoding="utf-8", errors="replace").read()
            except OSError:
                log = ""
            m = re.search(r"vk: (.+?), Vulkan [\d.]+, driver (\S+); mesh shaders", log)
            gpu = (m and "%s (driver %s)" % (m.group(1), m.group(2))) or ""
        settings = settings or published_settings(meta.get("env", {}))
        if ini is None:   # (the options file as the runs had it: BASE_INI with their --ini over it)
            ini = dict(BASE_INI)
            ini.update(kv.split("=", 1) for kv in meta.get("ini") or [])
        minutes += meta.get("seconds", 0) / 60
    ini = " ".join("%s=%s" % kv for kv in sorted((ini or BASE_INI).items()))
    cmd = [sys.executable, "-I", os.path.join(ROOT, "nx2", "pipeline_states.py"), "tour"] + lists + [
        "--gpu", gpu or "unknown", "--build", build or "unknown",
        "--settings", ini + " " + " ".join("%s=%s" % kv for kv in sorted(settings.items())),
        "--route", "scripts/pipeline_tour.py, parts %s, from a new save (%.0f minutes of runs)" % (", ".join(parts), minutes)]
    if sources:
        cmd += ["--dir", sources]
    return subprocess.run(cmd).returncode


def merge(args):
    """The lists merged by nx2/pipeline_states.py (its format, whatever it is), and how much each added"""
    tool = state_tool()
    seen = set()
    for path in args.lists:
        n, progs, recs = list_counts(tool, path)
        print("%s: %d pipelines, %d programs, %d not in the lists before it" % (path, n, progs, len(recs - seen)))
        seen |= recs
    ran = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "nx2", "pipeline_states.py"), "--out", args.out] + args.lists)
    return ran.returncode


def list_diff(tool, new, old):
    """Two lists' records against each other (version 1 ones as version 2 where the other has those): {new, old: how many each
    has, lost: those in old not in new, gained: those in new not in old}"""
    (n, o), _ = tool.as_v2([tool.read(new), tool.read(old)])
    n, o = set(n), set(o)
    return {"new": len(n), "old": len(o), "lost": o - n, "gained": n - o}


def compare(args):
    """How many pipelines each of two lists has that the other lacks: a new tour's against an old one's"""
    tool = state_tool()
    d = list_diff(tool, args.new, args.old)
    print("new %s: %d pipelines; old %s: %d pipelines" % (args.new, d["new"], args.old, d["old"]))
    for what, recs in (("in the old, not in the new (lost)", d["lost"]), ("in the new, not in the old (gained)", d["gained"])):
        print("%s: %s" % (what, tool.describe(sorted(recs)) if recs else "none"))
    return 0


def raw_problem(path):
    """Why a --vk-profile file is not one llvm-profdata should merge (None: it is): LLVM's raw format, with counted functions"""
    with open(path, "rb") as f:
        head = f.read(32)
    if head[:8] != PGO_RAW_MAGIC:
        return "not an LLVM raw profile (host/pgo_rt.c's)"
    if len(head) < 32 or struct.unpack_from("<Q", head, 24)[0] == 0:
        return "no counted functions in it (a renderer built without RX_VK_PGO=gen counting)"
    return None


def profdata_tool():
    """The toolchain's llvm-profdata (the compiler that builds the renderer: third_party/a64-kit/tools/toolchain.py)"""
    sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
    import toolchain
    return toolchain.need(toolchain.find("llvm-profdata"))


def profile_note(d, raws, tool, run=subprocess.run, today=None):
    """The block for assets/profiles/README.md: when, where (GPU and driver, the build), the route's parts, the compiler"""
    games = []
    for raw in raws:
        try:
            with open(raw[:-len(".profraw")] + ".json", encoding="utf-8") as f:
                games.append(json.load(f))
        except (OSError, ValueError):
            games.append({})
    parts = []
    for m in games:
        if m.get("part") and m["part"] not in parts:
            parts.append(m["part"])
    gpus = sorted({m["renderer"] for m in games if m.get("renderer")})
    builds = sorted({m["version"] for m in games if m.get("version")})
    minutes = sum(m.get("seconds") or 0 for m in games) / 60
    failed = [m["name"] for m in games if m.get("name") and not m.get("completed")]
    try:
        version = run([tool, "--version"], capture_output=True, text=True).stdout
    except OSError:
        version = ""
    llvm = next((line.strip() for line in version.splitlines() if "LLVM version" in line), "llvm-profdata (version unknown)")
    cpu = ""
    try:
        with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
            cpu = next((line.split(":", 1)[1].strip() for line in f if line.startswith("model name")), "")
    except OSError:
        pass
    import platform
    return "\n".join([
        "- Recorded: %s, the pipeline tour (`scripts/pipeline_tour.py tour --vk-profile`), parts %s from a new save: %d games, "
        "%.0f minutes, an `RX_VK_PGO=gen` renderer on build %s; each game's counters cleared once it had control of Link and "
        "written at its end%s" % (today or time.strftime("%Y-%m-%d", time.gmtime()), ", ".join(parts) or "unknown", len(raws),
                                   minutes, ", ".join(builds) or "unknown",
                                   "" if not failed else " (not completed, their counters kept: %s)" % ", ".join(failed)),
        "- GPU and driver: %s, %s%s" % ("; ".join(gpus) or "unknown", platform.system() or "unknown", "; " + cpu if cpu else ""),
        "- Compiler: %s (the llvm-profdata that merged it)" % llvm]) + "\n"


def merge_profile(args, root=ROOT, run=subprocess.run):
    """The --vk-profile folder's counters merged by llvm-profdata into the renderer's play profile (work/hookshot_vk.profdata,
    which nx2/make_game.py builds with); --publish also writes the committed one, assets/profiles/hookshot_vk.profdata.xz (xz
    -9e), and the note for assets/profiles/README.md. Refused when there is nothing counted to merge"""
    d = os.path.abspath(args.dir)
    raws = sorted(os.path.join(d, n) for n in os.listdir(d) if n.endswith(".profraw")) if os.path.isdir(d) else []
    if not raws:
        raise SystemExit("pipeline_tour: merge-profile: no .profraw in %s (a run or tour with --vk-profile %s writes them)" % (d, d))
    for raw in raws:
        why = raw_problem(raw)
        if why:
            raise SystemExit("pipeline_tour: merge-profile: %s: %s" % (raw, why))
    tool = args.llvm_profdata or profdata_tool()
    out = os.path.abspath(args.out) if args.out else os.path.join(root, VK_PROFILE_WORK)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    cmd = [tool, "merge", "-o", out] + raws
    print(" ".join(cmd), flush=True)
    if run(cmd).returncode or not os.path.isfile(out) or not os.path.getsize(out):
        raise SystemExit("pipeline_tour: merge-profile: llvm-profdata wrote no profile (%s)" % out)
    print("merge-profile: %d games' counters in %s (%d bytes)" % (len(raws), out, os.path.getsize(out)))
    if args.publish:
        import lzma
        with open(out, "rb") as f:
            data = f.read()
        shipped = os.path.join(root, VK_PROFILE)
        with open(shipped, "wb") as f:   # (as xz -9e: the preset 9, extreme)
            f.write(lzma.compress(data, format=lzma.FORMAT_XZ, preset=9 | lzma.PRESET_EXTREME))
        note = profile_note(d, raws, tool, run)
        with open(os.path.join(d, "README-note.md"), "w", encoding="utf-8") as f:
            f.write(note)
        print("merge-profile: %s written (%d bytes). Its note, for assets/profiles/README.md in place of the Recorded, GPU and "
              "driver, and Compiler lines (also in %s):\n%s" % (shipped, os.path.getsize(shipped),
                                                                 os.path.join(d, "README-note.md"), note), end="")
    return 0


# ---- what each step is worth ----
# Steps the route needs whatever they add: without them no later step happens
NEEDED = ("new game:", "every item", "age switched")
NEEDED_END = (" / arrive", ", hearts not protected")   # (a stop's arrival with --sub; the Game Over's way there)


def run_steps(tool, run_dir, sub=False):
    """A run's steps: [(label, seconds, {records})], the list sliced at each step's count (the game only appends to it). sub:
    a step with marks (arrive, look, items) split at them, "<step> / <part>", the rest "<step> / settle" (seconds: the step's)"""
    with open(os.path.join(run_dir, "report.json"), encoding="utf-8") as f:
        rows = json.load(f)["stops"]
    path = os.path.join(run_dir, "states.bin")
    recs = tool.read(path) if os.path.exists(path) and os.path.getsize(path) else []
    (recs,), _ = tool.as_v2([recs], force=True)   # (version 1 runs compared with version 2 lists; those that do not convert left out)
    out, prev = [], 0
    for r in rows:
        n = min(r["pipelines"], len(recs))
        if sub and r.get("marks"):
            at = prev
            for what, m in r["marks"] + [("settle", n)]:
                m = max(at, min(m, n))
                out.append(("%s / %s" % (r["stop"], what), r.get("took", 0), set(recs[at:m])))
                at = m
        else:
            out.append((r["stop"], r.get("took", 0), set(recs[prev:n])))
        prev = max(prev, n)
    return out


def worth(runs, against=(), floor=10):
    """Each step's worth over runs: [{step, runs, seconds, met, beyond, only, kept}] in route order. met: the pipelines it met
    (over all its runs), beyond: those not in the `against` records (the players' lists), only: of those, the ones no other kept step
    met. Steps whose `only` is under `floor` are cut one at a time, the least worth a second first, the others then counted again
    (two steps that meet the same pipelines: one is kept); the steps the route needs (NEEDED) are kept whatever they add"""
    against = set(against)
    met, secs, order = {}, {}, []
    for steps in runs:
        for label, took, recs in steps:
            if label not in met:
                met[label], secs[label] = set(), []
                order.append(label)
            met[label] |= recs
            secs[label].append(took)
    beyond = {k: v - against for k, v in met.items()}
    kept = set(order)
    holders = {}   # (a pipeline: how many kept steps met it)
    for k in order:
        for r in beyond[k]:
            holders[r] = holders.get(r, 0) + 1
    only = lambda k: sum(1 for r in beyond[k] if holders[r] == (1 if k in kept else 0))
    median = lambda k: sorted(secs[k])[len(secs[k]) // 2]
    while True:
        weak = [(only(k) / max(1.0, median(k)), -median(k), k) for k in kept if not k.startswith(NEEDED) and not k.endswith(NEEDED_END)
                and only(k) < floor]
        if not weak:
            break
        k = min(weak)[2]   # (the least a second; of equals, the slowest)
        kept.discard(k)
        for r in beyond[k]:
            holders[r] -= 1
    return [{"step": k, "runs": len(secs[k]), "seconds": median(k), "met": len(met[k]), "beyond": len(beyond[k]), "only": only(k),
             "kept": k in kept} for k in order]


def value(args):
    """The steps of runs, each with what it added that the players' lists (the shipped list's contrib/ sources) and the other
    steps do not have, and whether it is worth keeping"""
    tool = state_tool()
    against = []
    if not args.no_contrib:
        for src in tool.sources(tool.sources_dir()):
            if os.path.basename(src) != tool.TOUR:
                against += tool.read(src)
    for path in args.against:
        against += tool.read(path)
    (against,), _ = tool.as_v2([against], force=True)
    runs = [run_steps(tool, d, args.sub) for d in args.runs]
    rows = worth(runs, against, args.floor)
    sources = (["players' lists"] if not args.no_contrib else []) + list(args.against)
    print("%d runs; against %d pipelines (%s)" % (len(runs), len(set(against)), ", ".join(sources) or "none"))
    print("%6s %6s %6s %6s %4s  %-5s %s" % ("met", "beyond", "only", "secs", "runs", "", "step"))
    for r in rows:
        print("%6d %6d %6d %6.0f %4d  %-5s %s" % (r["met"], r["beyond"], r["only"], r["seconds"], r["runs"],
                                                 "keep" if r["kept"] else "CUT", r["step"]))
    cut = [r for r in rows if not r["kept"]]
    print("cut %d of %d steps: %.0f s of %.0f (median seconds a run)" % (len(cut), len(rows), sum(r["seconds"] for r in cut),
                                                                        sum(r["seconds"] for r in rows)))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)
    return 0


def report(args):
    with open(os.path.join(args.run, "report.json"), encoding="utf-8") as f:
        r = json.load(f)
    print(write_report(args.run, r["stops"], r["meta"]), end="")
    return 0


def dry_route(args):
    """--dry-run: the route's steps, without the game"""
    a = actions()
    picks = item_stops(a)
    for step in route(a, args.part, args.stops, args.first):
        if step[0] == "visit":
            item = ", sword, shield, item %d" % (picks.index(step[1:3]) + 1) if step[1:3] in picks else ""
            print("visit %s (%s): teleport %d%s" % (a["teleports"][step[2]][0], step[1], step[2], item))
        elif step[0] == "items":
            print("items: %d Debug tab actions" % len(items_unlock(a)))
        else:
            print(" ".join(str(s) for s in step[:2]))
    return 0


TERMINATED = []   # (SIGTERM came: a tour passes it on to its run, which Ctrl+C at the terminal reaches by itself)


def stopped(signum, frame):
    TERMINATED.append(signum)
    raise KeyboardInterrupt   # (SIGTERM as SIGINT: the run's finally ends the game and puts the game folder's files back)


def hold_stops():
    """No more Ctrl+C or SIGTERM from here: a run putting the game folder back is not cut short by a second one (a tour that
    waited too long kills it instead)"""
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, signal.SIG_IGN)
        except ValueError:   # (not the main thread: the tests')
            pass


def settle_options(p):
    p.add_argument("--settle-quiet", type=int, default=SETTLE_QUIET, metavar="FRAMES",
                   help="a stop is left after this many frames with no new pipeline (default %d; before, 150)" % SETTLE_QUIET)
    p.add_argument("--settle-max", type=int, default=SETTLE_MAX, metavar="FRAMES",
                   help="and after this many at most (default %d)" % SETTLE_MAX)


def parser():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="one game")
    r.add_argument("--game", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--name", default="play")
    r.add_argument("--part", choices=PARTS, default="all")
    r.add_argument("--cpus", help="the CPUs the game may use (Linux), e.g. 0-3,12-15")
    r.add_argument("--nice", type=int, default=5, help="the game's niceness (default 5: the desktop stays responsive)")
    r.add_argument("--stops", type=int, help="only the first N stops")
    r.add_argument("--first", metavar="STOP", help="start the route at this stop (its name, or \"<name> (<age>)\"): after a failure")
    r.add_argument("--env", action="append", default=[], help="NAME=VALUE over the run's switches (NAME=- drops one)")
    r.add_argument("--ini", action="append", default=[], help="key=value in hookshot_options.ini for the run")
    r.add_argument("--window", metavar="WxH", help="a window instead of the headless run")
    r.add_argument("--skip-opening", action="store_true", help="the game skips the new-game opening (HOS_SKIP_OPENING)")
    r.add_argument("--dry-run", action="store_true", help="the route's steps, without the game")
    r.add_argument("--mem", action="append", default=[], metavar="KEY=VALUE",
                   help="the memory guard's limits over its defaults (%s)" % ", ".join("%s=%s" % kv for kv in sorted(MEM.items())))
    r.add_argument("--no-mem-guard", action="store_true", help="no memory guard (no waiting, no limits, no scope)")
    r.add_argument("--vk-profile", metavar="DIR", help="an RX_VK_PGO=gen renderer's counters, from control of Link to the "
                   "run's end, to DIR/<name>.profraw (merge-profile merges them)")
    settle_options(r)
    t = sub.add_parser("tour", help="the parts, one game at a time, merged")
    t.add_argument("--game", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--name", default="play")
    t.add_argument("--parts", default="opening,child-overworld,dungeons,adult-overworld")
    t.add_argument("--cpus", help="the CPUs the games may use (Linux), e.g. 6-11,18-23: off the desktop's")
    t.add_argument("--nice", type=int, default=19, help="the games' niceness (default 19)")
    t.add_argument("--mem", action="append", default=[], metavar="KEY=VALUE", help="the memory guard's limits (run --help)")
    t.add_argument("--no-mem-guard", action="store_true")
    t.add_argument("--env", action="append", default=[])
    t.add_argument("--ini", action="append", default=[])
    t.add_argument("--keep", action="store_true", help="keep the parts' game folder copy")
    t.add_argument("--stops", type=int, help="only each part's first N stops (a short test)")
    t.add_argument("--chunk", type=int, default=8, help="stops a game, then a fresh one (one game grows ~1 GB an area); 0: no limit")
    t.add_argument("--publish-partial", action="store_true", help="--publish even when a run failed")
    t.add_argument("--window", metavar="WxH", help="the games in a window")
    t.add_argument("--publish", action="store_true",
                   help="write the repository's tour list (assets/pipeline_states/tour.bin.xz and tour.md) from the parts' lists")
    t.add_argument("--vk-profile", metavar="DIR", help="each game's RX_VK_PGO=gen renderer counters to DIR/<game>.profraw "
                   "(run --vk-profile; merge-profile merges them)")
    settle_options(t)
    t.add_argument("--compare-to", metavar="LIST", help="after the merge: how many pipelines LIST (an older tour's merged list, "
                   "or tour.bin.xz) has that this tour's lacks, and the other way round (compare)")
    mp = sub.add_parser("merge-profile", help="the --vk-profile counters merged into the renderer's play profile")
    mp.add_argument("dir", help="the --vk-profile folder (its *.profraw)")
    mp.add_argument("--out", help="the merged profile (default work/hookshot_vk.profdata, which nx2/make_game.py builds with)")
    mp.add_argument("--publish", action="store_true", help="also write assets/profiles/hookshot_vk.profdata.xz (xz -9e) and "
                    "the note for assets/profiles/README.md")
    mp.add_argument("--llvm-profdata", metavar="PATH", help="this llvm-profdata, not the toolchain's (the compiler's that "
                    "built the counting renderer)")
    m = sub.add_parser("merge", help="lists merged, and what each added")
    m.add_argument("--out", required=True)
    m.add_argument("lists", nargs="+")
    c = sub.add_parser("compare", help="two lists: how many pipelines each has that the other lacks")
    c.add_argument("new", help="the new list (a tour's <out>/<name>.bin, a run's states.bin, a .bin.xz)")
    c.add_argument("old", help="the list it should lose nothing of")
    s = sub.add_parser("report", help="a run's report again")
    s.add_argument("run")
    v = sub.add_parser("value", help="each step's new pipelines and seconds over runs, and the steps worth cutting")
    v.add_argument("runs", nargs="+", help="run folders (<out>/<name>: report.json and states.bin)")
    v.add_argument("--against", action="append", default=[], help="a list whose pipelines count as had already")
    v.add_argument("--no-contrib", action="store_true", help="not against the players' lists in assets/pipeline_states/contrib")
    v.add_argument("--floor", type=int, default=10, help="a step that alone adds fewer pipelines than this is cut (default 10)")
    v.add_argument("--sub", action="store_true", help="the stops' parts apart (arrive, look, items, settle), where runs marked them")
    v.add_argument("--json")
    return p


def main(argv=None):
    signal.signal(signal.SIGTERM, stopped)
    a = parser().parse_args(argv)
    if a.cmd == "run" and a.dry_run:
        return dry_route(a)
    return {"run": run, "tour": tour, "merge": merge, "compare": compare, "report": report, "value": value,
            "merge-profile": merge_profile}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
