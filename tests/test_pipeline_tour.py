"""The pipeline tour (scripts/pipeline_tour.py), without the game: the Debug tab's action numbers and the teleport list it reads
from the source, its route (the items once an age and at the passes' stops), the presses it sends, the switches and options it
runs with, its command line (the settle's frames), its counts, merge and compare of the pipeline state lists, the memory guard,
and the bridge driving: a fake control bridge (a Unix socket served from a thread, as
tests/test_hookshot_ctl.py's stand-in serves one) that answers the way host/control.c does, refusing, cancelling and timing out
where the game would, with the real client (scripts/hookshot_ctl.py Bridge) in between. And --vk-profile, the renderer's play
profile on the tour's route: a counting renderer told apart, RX_VK_PGO_FILE's reset after control and dump at each run's end
against a stand-in for host/pgo_rt.c's writer, the refusals when nothing counts, and merge-profile (a stand-in llvm-profdata).

The tour teleports through the bridge by the destination's index and switches the age through it (host/control.c, host/hos_main.c),
and gives the items through the live input file's "!cheat <action>" lines (host/nn_hid.c), whose numbers are the Insert menu's
(gpu/vk/oot_cheat_items.h): a renumbered action or a reordered teleport list must not send the tour somewhere else.
Synthetic data only. Linux only (the bridge's Unix socket).

python -I -B tests/test_pipeline_tour.py
"""
import importlib.util
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

RUN_ALL_SKIP = {"windows": "the tour runs on Linux (/proc, the bridge's Unix socket): checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
TOUR = ROOT / "scripts" / "pipeline_tour.py"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fail(why):
    raise SystemExit("pipeline tour: " + why)


def text(path):
    return (ROOT / path).read_text(encoding="utf-8")


def source(tour, a):
    """What the tour reads from the source, and what it relies on there"""
    main_c, hid, menu, ctl_c = text("host/hos_main.c"), text("host/nn_hid.c"), text("gpu/vk/vk_menu.cpp"), text("host/control.c")
    # the bridge: a teleport by index is the Debug tab's OOT_TELEPORT_ACTION + index, the age switch OOT_AGE_ACTION
    if "action = OOT_TELEPORT_ACTION + asked - 1" not in main_c or "action = OOT_AGE_ACTION" not in main_c:
        fail("host/hos_main.c no longer takes the bridge's teleport and age switch as the Debug tab's actions")
    for cmd in ("teleport", "age", "status", "wait", "sequence", "press", "quit"):
        if '!strcmp(cmd, "%s")' % cmd not in ctl_c:
            fail("host/control.c has no %s command" % cmd)
    # (status's game: the fields as oot_state reads them, or the sampled state of host/oot_state_tap.c, whose event, loading and
    # age are filled from oot_state where the sample could not read them)
    tap = text("host/oot_state_tap.c")
    if 'buf_puts(&b, ",\\"game\\":");' not in ctl_c or '"{\\"event\\":%s,\\"loading\\":%s,\\"age\\":\\"%s\\"}"' not in ctl_c or \
            'const char *keys[] = {"event", "loading", "age"};' not in ctl_c:
        fail("host/control.c's status no longer says the game's event, loading and age, which the tour waits on")
    for field in ('",\\"event\\":%s"', '",\\"loading\\":%s"', '",\\"age\\":%s"'):
        if field not in tap:
            fail("host/oot_state_tap.c's sample no longer has %s, which status's game passes on" % field)
    # (the presses: HOS_INPUT_SCRIPT's units, which scripts/hookshot_ctl.py turns into the bridge's frame steps, as
    # host/control.c's own script reading would)
    ctl_py = text("scripts/hookshot_ctl.py")
    if 'strcmp(units, "hos")' not in ctl_c or 'args["steps"] = hos_steps(args.pop("script"))' not in ctl_py or \
            'const JNode *list = j_get(d, req, "steps");' not in ctl_c:
        fail("host/control.c's sequence or scripts/hookshot_ctl.py no longer takes HOS_INPUT_SCRIPT's units (the tour's presses)")
    if '"refused"' not in ctl_c or '"cancelled"' not in ctl_c:
        fail("host/control.c no longer replies refused or cancelled")
    # the refusals asking again cannot change are named in the game's words
    whys = re.findall(r'teleport\.why = (?:child \? )?"([^"]+)"', main_c)
    if not whys:
        fail("host/hos_main.c's teleport refusals not found")
    for why in whys:
        permanent = any(p in why for p in tour.PERMANENT)
        transient = any(w in why for w in ("event is playing", "map is loading", "in progress", "not there yet"))
        if permanent == transient:
            fail("a teleport refusal neither asked again nor given up on, or both: %r" % why)
    # the live input: "!cheat <action>", the Debug tab's actions (the inventory)
    if not re.search(r'if \(!strncmp\(line, "!cheat ", 7\)\) __atomic_store_n\(&hos_oot_live_action, atoi\(line \+ 7\)', hid):
        fail("host/nn_hid.c's live input no longer reads \"!cheat <action>\"")
    if "cheat_action.store(1000 + teleport_choice)" not in menu or a["teleport"] != 1000:
        fail("the menu's teleport action and OOT_TELEPORT_ACTION (%d) differ" % a["teleport"])
    if "cheat_action.store(999)" not in menu or a["age"] != 999:
        fail("the menu's age action and OOT_AGE_ACTION (%d) differ" % a["age"])
    # the teleports: one for each destination, in its order; the adult-only ones those without a child map
    table = main_c.split("oot_destinations[] = {", 1)[1].split("\n};", 1)[0]
    entries = re.findall(r"\{(NULL|\"[^\"]*\"),.*?\},\s*/\* ([^*]+) \*/", table, re.S)
    if [label for _, label in entries] != [name for name, _ in a["teleports"]]:
        fail("the teleport names and host/hos_main.c's destinations differ:\n%s\n%s" % (
            [label for _, label in entries], [name for name, _ in a["teleports"]]))
    for (child, label), (_, adult_only) in zip(entries, a["teleports"]):
        if (child == "NULL") != adult_only:
            fail("%s: adult only in the menu %s, no child map in the table %s" % (label, adult_only, child == "NULL"))
    if len(a["bancs"]) != len(a["teleports"]) or any((child is None) != only for (child, _), (_, only) in zip(a["bancs"], a["teleports"])):
        fail("the destinations' maps and the teleport list differ")
    if not 0 < a["dungeons"] < len(a["teleports"]):
        fail("the teleport list's Dungeons section not found (vk_menu.cpp teleport_sections)")
    main_time = re.search(r"action >= (\d+) && action < (\d+)\) \{\s*oot_set_time\(c, action - (\d+)\)", main_c)
    if not main_time or int(main_time.group(1)) != tour.TIME_ACTION or int(main_time.group(2)) < tour.TIME_ACTION + 23:
        fail("host/hos_main.c's clock action is not TIME_ACTION + hour")
    # the inventory: the item list's order is the action's index; kinds 0 (unlock) and 1 (a quantity)
    names = list(a["items"])
    for must in ("Item_Bomb", "Item_Hookshot", "Item_Longshot", "Item_Arrow_Fire", "Item_DinFire", "Weapon_Range_Bow",
                 "Item_Ocarina_Time"):
        if must not in a["items"]:
            fail("%s is not in the inventory list" % must)
    if a["items"][names[0]][0] != 0 or a["items"]["Item_Bomb"][1] != 1 or a["items"]["Item_Longshot"][1] != 0 or \
            a["items"]["Item_Hookshot"][1] != 0 or a["items"]["Item_Hookshot"][0] + 1 != a["items"]["Item_Longshot"][0]:
        fail("the inventory's indices or kinds are read wrong: %s" % list(a["items"].items())[:16])
    unlock = tour.items_unlock(a)
    count = len(re.findall(r'OOT_ITEM\("', text("gpu/vk/oot_cheat_items.h")))
    by_index = {i: name for name, (i, _) in a["items"].items()}
    for n in unlock:
        item, op = divmod(n - a["item"], 256)
        if not 0 <= item < count or item not in by_index or not (op == 200 or 0 < op - 64 <= 63):
            fail("an inventory action out of range: %d" % n)
    if any(n.startswith(tour.QUEST_ITEMS) for n in [by_index[(x - a["item"]) // 256] for x in unlock]):
        fail("the quest items (the story's flags) are given")
    # the switches the tour sets are ones the game reads, and the options its menu writes
    env_src = "".join(text(p) for p in ("host/nn_vi.c", "host/nn_hid.c", "host/pad_sdl.c", "host/nn_audio.c", "host/hos_main.c",
                                        "host/nn_applet.c", "host/discord_rpc.c", "host/display.c")) + menu + \
        "".join(p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "gpu" / "vk").glob("*.cpp"))
    for name in list(tour.BASE_ENV) + ["HOS_SKIP_OPENING", "HOS_CONTROL", "HOS_INPUT_LIVE", "HOS_WINDOW_SIZE", "HOS_WINDOW_NOACTIVATE",
                                       "RX_VK_SHOTS"]:
        if '"%s"' % name not in env_src and name not in ("HOS_CONTROL",):
            fail("%s: no switch of that name in the game's source" % name)
    keys = {"output_res", "vsync", "resolution", "frame_rate_cap", "upscaling", "dlss", "handheld_profile", "ultrawide"}
    for row in re.findall(r'\{TAB_[A-Z]+, "([^"]+)", &opts\.', menu):
        keys.add(row.lower().replace(" ", "_"))
    for key in tour.BASE_INI:
        if key not in keys:
            fail("%s: no Insert menu row writes it to hookshot_options.ini" % key)
    if tour.BASE_INI["frame_rate_cap"] != "6" or "Uncapped" not in re.search(r"tx_fps_cap\(int v\) \{(.*?)\}", menu, re.S).group(1).split(",")[6]:
        fail("frame_rate_cap=6 is not Uncapped")
    # the lists the tour moves aside and reads: the notes and play's lists in the game folder's cache/, the shipped one beside the
    # game (and a build from before's loose ones, which the game's start moves into cache/)
    draw, paths, layout = text("gpu/vk/vk_draw.cpp"), text("gpu/vk/vk_paths.h"), text("host/game_layout.c")
    if 'return strstr(name, "_shipped") ? game_folder_path() + name : cache_path(name);' not in draw or \
            'std::string pipe_notes_path() { return cache_path("hookshot_pipelines.bin"); }' not in draw or \
            'cache_path(const char* name) { return game_sub_path("cache", name); }' not in paths:
        fail("the pipeline notes and lists are no longer in the game folder's cache/ (vk_draw.cpp, vk_paths.h)")
    if tour.LIST != "cache/hookshot_pipeline_states.bin" or tour.LIST not in tour.STATE_FILES:
        fail("the tour's list is not cache/hookshot_pipeline_states.bin: %s" % tour.LIST)
    if "for (const uint64_t key : computes_read())" not in draw:
        fail("the state list no longer takes the earlier play's compute programs in (vk_draw.cpp): hookshot_computes.bin need not go aside")
    for name in ("hookshot_pipelines.bin", "hookshot_pipeline_states.bin", "hookshot_pipeline_states_v1.bin", "hookshot_computes.bin"):
        if '"%s"' % name not in layout.split("to_cache[] = {", 1)[1].split("};", 1)[0] or "cache/" + name not in tour.STATE_FILES \
                or name not in tour.STATE_FILES:
            fail("%s: not put aside in cache/ and beside the game, or the game no longer moves it into cache/" % name)
    if "hookshot_pipeline_states_shipped.bin" not in tour.STATE_FILES or len({tour.aside_name(n) for n in tour.STATE_FILES}) != \
            len(tour.STATE_FILES):
        fail("the shipped list is not put aside, or two files are put aside under one name")
    # the renderer's counters (--vk-profile): RX_VK_PGO_FILE, <file>.reset and <file>.dump as host/pgo_rt.c reads them in the
    # renderer's copy, and its raw format's magic
    pgo, build = text("host/pgo_rt.c"), text("nx2/build.py")
    if '-DPGO_ENV="RX_VK_PGO_FILE"' not in build or tour.PGO_ENV != "RX_VK_PGO_FILE" or tour.PGO_MARK != b"RX_VK_PGO_FILE\x00":
        fail("the renderer's copy of host/pgo_rt.c no longer reads RX_VK_PGO_FILE (nx2/build.py -DPGO_ENV)")
    for want in ('snprintf(dump, sizeof dump, "%s.dump", path);', 'snprintf(reset, sizeof reset, "%s.reset", path);',
                 "memset(PGO_CNT_BEGIN, 0,", "pgo_write(path);\n            DeleteFileA(dump);", "DeleteFileA(reset);",
                 "const char *e = getenv(PGO_ENV);"):
        if want not in pgo:
            fail("host/pgo_rt.c no longer has %r (the tour's reset and dump)" % want)
    magic = re.search(r"(0x[0-9a-f]+)ULL,\s*/\* magic", pgo)
    if not magic or struct.pack("<Q", int(magic.group(1), 16)) != tour.PGO_RAW_MAGIC or "ndata, 0, ncnt" not in pgo:
        fail("host/pgo_rt.c's raw profile header is not the one merge-profile checks")
    if '"hookshot_vk.dll"' not in text("host/rx_backend.c") or set(tour.RENDERERS) != {"hookshot_vk.so", "hookshot_vk.dll"}:
        fail("the renderer module the tour looks at is not the one the game loads (host/rx_backend.c)")


def routes(tour, a):
    """The route: a new game first, every destination as child (but the adult-only ones) and as adult, the age switched once"""
    steps = tour.route(a)
    if steps[0] != ("newgame",) or ("items",) not in steps:
        fail("the route does not start with a new game and the items")
    visits = [(s[1], s[2]) for s in steps if s[0] == "visit"]
    for i, (name, adult_only) in enumerate(a["teleports"]):
        for age in ("child", "adult"):
            want = not (age == "child" and adult_only) and (age, name) not in tour.LEFT_OUT
            if ((age, i) in visits) != want:
                fail("%s (%s) is %s the route" % (name, age, "not in" if want else "in"))
    ages = [k for k, s in enumerate(steps) if s == ("age",)]
    if len(ages) != 1 or any(s[0] == "visit" and s[1] == "adult" for s in steps[:ages[0]]):
        fail("the age is not switched once, before the adult stops")
    for part in ("child", "adult"):
        plan = tour.plan_stops(a, part)
        if not plan or any(age != part for age, _ in plan):
            fail("the %s part visits other ages" % part)
    for age, name in tour.LEFT_OUT:
        if name not in [n for n, _ in a["teleports"]]:
            fail("a left-out stop names no destination: %s" % name)
    if len(tour.plan_stops(a, "all")) != len(tour.plan_stops(a, "child")) + len(tour.plan_stops(a, "adult")):
        fail("the child and adult parts are not the whole route")
    for part in ("child-dungeons", "adult-dungeons"):
        if not tour.plan_stops(a, part) or any(i < a["dungeons"] for _, i in tour.plan_stops(a, part)):
            fail("%s visits the overworld" % part)
    for age in ("child", "adult"):   # (a stop the game does not teleport out of goes last in its age)
        names = [a["teleports"][i][0] for g, i in tour.plan_stops(a, "all") if g == age]
        for last in tour.LAST:
            if last in names and names.index(last) != len(names) - 1 - sum(1 for n in names[names.index(last) + 1:] if n in tour.LAST):
                fail("%s is not last in the %s's stops" % (last, age))
    resumed = tour.plan_stops(a, "child", first="Market")
    if a["teleports"][resumed[0][1]][0] != "Market" or len(resumed) >= len(tour.plan_stops(a, "child")):
        fail("--first Market does not start the route there")
    for part in tour.PARTS:   # (the runs of 8 stops cover the part's route, each starting where the one before ended)
        runs = tour.chunks(a, part, 8)
        covered = sum((tour.plan_stops(a, part, stops, first) for first, stops in runs), [])
        if covered != tour.plan_stops(a, part):
            fail("the %s part's runs of 8 stops are not its route" % part)
    if tour.chunks(a, "opening", 8) != [(None, None)]:
        fail("the opening part is not one run")
    if len([s for s in tour.route(a, stops=3) if s[0] == "visit"]) != 3:
        fail("--stops 3 is not three stops")
    over = tour.route(a, "gameover")
    if over[-1][0] != "gameover" or a["teleports"][over[-1][1]][0] != tour.GAME_OVER_AT:
        fail("the gameover part does not go to %s" % tour.GAME_OVER_AT)
    items_where(tour, a)


def items_where(tour, a):
    """The sword, the shield and an item: once an age, at the first stop of its overworld (ITEMS_ONCE), and at the stops with
    passes of their own (ITEMS_PASSES), each a stop the route visits and with its reason; a different pause menu pick at each,
    the same whichever game visits it; and play uses them there and nowhere else, settling each stop by the run's frames"""
    import types
    names = [n for n, _ in a["teleports"]]
    plan = tour.plan_stops(a, "all")
    if set(tour.ITEMS_ONCE) != {"child", "adult"}:
        fail("the items are not used once for each age: %s" % tour.ITEMS_ONCE)
    for age, name in tour.ITEMS_ONCE.items():
        first = tour.plan_stops(a, age + "-overworld")[0]
        if names[first[1]] != name:
            fail("the %s's items are not at the first stop of its overworld (%s, not %s)" % (age, name, names[first[1]]))
    for name, why in tour.ITEMS_PASSES.items():
        if name not in names or not any(names[i] == name for _, i in plan) or not why.strip():
            fail("an item stop the route does not visit, or with no reason: %s (%r)" % (name, why))
    stops = tour.item_stops(a)
    for age in ("child", "adult"):
        at = [names[i] for g, i in stops if g == age]
        want = [names[i] for g, i in plan if g == age and (names[i] == tour.ITEMS_ONCE[age] or names[i] in tour.ITEMS_PASSES)]
        if at != want or at.count(tour.ITEMS_ONCE[age]) != 1:
            fail("the %s's item stops: %s, wanted %s" % (age, at, want))
    if len(stops) > 20 or len({tour.pause_pick(k)[0] for k in range(len(stops))}) != len(stops):
        fail("the item stops' picks are not all different (%d stops)" % len(stops))
    if len(stops) >= len(plan) // 3:
        fail("the items are used at %d of %d stops: not once an age and at the passes' stops" % (len(stops), len(plan)))
    # (play, without the game: which stops press the items, and the settle's frames as the run's options give them)
    with tempfile.TemporaryDirectory() as d:
        g = tour.Game(None, d, os.path.join(d, "none.bin"), dry=True)
        seen, rows = [], []
        g.press = lambda script, label=None: seen.append(("press", label))
        g.settle = lambda quiet, most, step=15: seen.append(("settle", quiet, most))
        tour.play(g, a, tour.route(a), rows, lambda label: (seen.append(("count", label)), rows.append({"stop": label})),
                  types.SimpleNamespace(settle_quiet=150, settle_max=600))
        g.steps.close()
    pressed, used, picks, settles = [], set(), [], set()
    for ev in seen:
        if ev[0] == "press":
            pressed.append(ev[1])
            if ev[1].startswith("item "):
                picks.append(ev[1])
        elif ev[0] == "settle":
            settles.add(ev[1:])
        else:
            if "sword, shield, item" in pressed:
                used.add(ev[1])
            pressed = []
    want = {"%s (%s)" % (names[i], age) for age, i in stops}
    if used != want:
        fail("play uses the items at %s, not %s" % (sorted(used ^ want), sorted(want)))
    if len(picks) != len(set(picks)) or len(picks) != len(stops):
        fail("play's item picks: %s" % picks)
    if settles != {(150, 600)}:
        fail("play's settle is not the run's --settle-quiet and --settle-max: %s" % settles)


def presses(tour):
    """The presses: buttons host/nn_hid.c knows, in its <seconds>:<button>[+<button>][/<ms>] form, each within its step's span"""
    hid = text("host/nn_hid.c")
    known = set(re.findall(r'\{"([A-Z]+)", (?:B_|-1)', hid))
    if not {"A", "B", "PLUS", "ZL", "ZR", "LUP", "RLEFT"} <= known:
        fail("host/nn_hid.c's button names not read: %s" % sorted(known))
    texts = [(tour.LOOK, tour.LOOK_SPAN), (tour.ACT, tour.ACT_SPAN), (tour.PAUSE_TOUR, tour.PAUSE_SPAN), (tour.DIVE, tour.DIVE_SPAN),
             (tour.UNDERWATER_GEAR, tour.GEAR_SPAN), (tour.BOOTS_OFF, tour.BOOTS_SPAN)]
    texts += [tour.pause_pick(k) for k in range(25)] + [(u, 1.0) for u in tour.UNSTICK]
    for script, span in texts:
        for press in script.split(","):
            m = re.fullmatch(r"([\d.]+):([A-Z+]+)(?:/(\d+))?", press)
            if not m or not set(m.group(2).split("+")) <= known:
                fail("a press nn_hid.c does not read: %s" % press)
            if float(m.group(1)) + int(m.group(3) or 200) / 1000 > span + 1e-6:
                fail("a press past its step's span (%g s): %s" % (span, press))
    if len({tour.pause_pick(k)[0] for k in range(20)}) != 20:
        fail("the item picks do not differ from stop to stop")
    if any("A" in p.split(":", 1)[1].split("/")[0].split("+") for p in tour.ACT.split(",")):
        fail("the item use presses A (next to someone it starts a conversation)")


def cli(tour, a):
    """The command line: every command parses, a dry run prints the route, the switches and the tour's runs"""
    p = tour.parser()
    r = p.parse_args(["run", "--game", "g", "--out", "o"])
    if (r.part, r.name, r.nice, r.env, r.ini, r.skip_opening) != ("all", "play", 5, [], [], False):
        fail("run's defaults: %s" % r)
    t = p.parse_args(["tour", "--game", "g", "--out", "o", "--publish"])
    if t.parts != "opening,child-overworld,dungeons,adult-overworld" or t.chunk != 8 or t.nice != 19 or not t.publish:
        fail("tour's defaults: %s" % t)
    for parts in t.parts.split(","):
        if parts not in tour.PARTS:
            fail("a default part that is not one: %s" % parts)
    for argv in (["run", "--game", "g", "--out", "o", "--part", "nope"], ["frobnicate"]):
        try:
            with open(os.devnull, "w") as null:
                old, sys.stderr = sys.stderr, null
                try:
                    p.parse_args(argv)
                finally:
                    sys.stderr = old
            fail("%s taken" % argv)
        except SystemExit as e:
            if e.code != 2:
                raise
    try:
        tour.tour(p.parse_args(["tour", "--game", "g", "--out", "o", "--parts", "child,nope"]))
        fail("a tour with an unknown part ran")
    except SystemExit as e:
        if "nope" not in str(e):
            raise
    ran = subprocess.run([sys.executable, "-I", str(TOUR), "run", "--game", "x", "--out", "x", "--dry-run", "--part", "child",
                          "--stops", "3"], capture_output=True, text=True)
    lines = ran.stdout.splitlines()
    first = "visit %s (child): teleport 0" % a["teleports"][0][0] + (", sword, shield, item 1" if ("child", 0) in tour.item_stops(a)
                                                                     else "")
    if ran.returncode or lines[0] != "newgame" or not lines[1].startswith("items: ") or len(lines) != 5 or lines[2] != first:
        fail("run --dry-run: %s%s" % (ran.stdout, ran.stderr))
    # the switches: fast and headless by default; a window, the Game Over's hearts, the opening, --env over them
    env = tour.run_env(p.parse_args(["run", "--game", "g", "--out", "o"]))
    for k, v in (("HOS_HEADLESS", "1"), ("HOS_FPS_MAX", "1000"), ("HOS_FPS_STEP", "0"), ("HOS_NO_PACING", "1"),
                 ("HOS_OUTPUT_RES", "640x360"), ("RX_VK_FRAMEGEN", "0"), ("HOS_SKIP_OPENING", "0"), ("HOS_OOT_PROTECT_HEARTS", "1")):
        if env.get(k) != v:
            fail("the run's %s is %s, not %s" % (k, env.get(k), v))
    env = tour.run_env(p.parse_args(["run", "--game", "g", "--out", "o", "--window", "960x540", "--part", "gameover",
                                     "--skip-opening", "--env", "HOS_AUDIO=-", "--env", "RX_VK_FORCE_MESH_FALLBACK=1"]))
    if "HOS_HEADLESS" in env or env.get("HOS_WINDOW_SIZE") != "960x540" or env["HOS_OOT_PROTECT_HEARTS"] != "0" or \
            env["HOS_SKIP_OPENING"] != "1" or "HOS_AUDIO" in env or env.get("RX_VK_FORCE_MESH_FALLBACK") != "1":
        fail("the run's switches: %s" % env)
    for part in tour.PARTS:   # (the speed settings in every part's game, the opening's too)
        env = tour.run_env(p.parse_args(["run", "--game", "g", "--out", "o", "--part", part] + (["--skip-opening"] if part != "opening"
                                                                                             else [])))
        for k, v in (("HOS_HEADLESS", "1"), ("HOS_OUTPUT_RES", "640x360"), ("HOS_NO_PACING", "1"), ("HOS_FPS_MAX", "1000"),
                     ("HOS_FPS_STEP", "0")):
            if env.get(k) != v:
                fail("the %s part's %s is %s, not %s" % (part, k, env.get(k), v))
    for k, v in (("output_res", "640x360"), ("frame_pacing", "0"), ("frame_rate_cap", "6"), ("vsync", "0")):
        if tour.BASE_INI.get(k) != v:
            fail("the runs' options: %s=%s, not %s" % (k, tour.BASE_INI.get(k), v))
    # the settle: in frames, shorter than the 150 it was, an option of run and tour that the tour passes to its runs
    if (r.settle_quiet, r.settle_max) != (tour.SETTLE_QUIET, tour.SETTLE_MAX) or not 30 <= tour.SETTLE_QUIET < 150 or \
            tour.SETTLE_MAX < 4 * tour.SETTLE_QUIET:
        fail("the settle's defaults: %d quiet frames, %d at most" % (tour.SETTLE_QUIET, tour.SETTLE_MAX))
    old = p.parse_args(["run", "--game", "g", "--out", "o", "--settle-quiet", "150", "--settle-max", "600"])
    if (old.settle_quiet, old.settle_max) != (150, 600):
        fail("run --settle-quiet 150 --settle-max 600: %s %s" % (old.settle_quiet, old.settle_max))
    ot = p.parse_args(["tour", "--game", "g", "--out", "o", "--settle-quiet", "150", "--compare-to", "old.bin"])
    ot.copy, ot.out_dir = "/g-tour", "/o"
    cmd = tour.run_command(ot, "child", "play-child", None, None, True)
    if cmd[cmd.index("--settle-quiet") + 1] != "150" or ot.compare_to != "old.bin" or \
            p.parse_args(["tour", "--game", "g", "--out", "o"]).compare_to is not None:
        fail("tour --settle-quiet is not passed to its runs, or --compare-to not taken: %s" % cmd)
    # the tour's runs: the opening watched once, by its own part; the next game of a part where the last one ended
    t = p.parse_args(["tour", "--game", "g", "--out", "o", "--cpus", "6-11", "--env", "X=1", "--mem", "soft_mb=5000"])
    t.copy, t.out_dir = "/g-tour", "/o"
    cmd = tour.run_command(t, "dungeons", "play-dungeons-1", "Fire Temple (child)", 3, True)
    want = ["run", "--game", "/g-tour", "--out", "/o", "--name", "play-dungeons-1", "--part", "dungeons", "--nice", "19",
            "--settle-quiet", str(tour.SETTLE_QUIET), "--settle-max", str(tour.SETTLE_MAX), "--env", "X=1",
            "--mem", "soft_mb=5000", "--first", "Fire Temple (child)", "--stops", "3", "--skip-opening", "--cpus", "6-11"]
    if cmd[3:] != want:
        fail("the tour's run: %s" % cmd[3:])
    # --vk-profile: off by default (the runs' commands and switches as above); on, each run is given the folder
    if r.vk_profile is not None or t.vk_profile is not None or tour.PGO_ENV in tour.run_env(r):
        fail("--vk-profile is on by default")
    t.vk_profile = "/prof"
    if tour.run_command(t, "dungeons", "play-dungeons-1", None, None, True)[-2:] != ["--vk-profile", "/prof"]:
        fail("the tour's run is not given --vk-profile")
    if p.parse_args(["run", "--game", "g", "--out", "o", "--vk-profile", "d"]).vk_profile != "d":
        fail("run --vk-profile")
    if tour.cpu_list("0-3,12-13,20") != {0, 1, 2, 3, 12, 13, 20}:
        fail("cpu_list")


def lists(tour, states, rec):
    """The counts and the merge, through nx2/pipeline_states.py (whatever its format); the options file; the report; --publish"""
    head = states.MAGIC + struct.pack("<I", states.SIZE)
    with tempfile.TemporaryDirectory() as d:
        one, two, out = (os.path.join(d, n) for n in ("one.bin", "two.bin", "out.bin"))
        open(one, "wb").write(head + rec(1, 2, 3) + rec(1, 2, 4))
        open(two, "wb").write(head + rec(1, 2, 4) + rec(5, 0, 3))
        n, programs, recs = tour.list_counts(states, one)
        if (n, programs, len(recs)) != (2, 1, 2):
            fail("list_counts: %d pipelines, %d programs, expected 2 and 1" % (n, programs))
        if tour.list_counts(states, os.path.join(d, "none.bin"))[:2] != (0, 0):
            fail("a missing list is not empty")
        ran = subprocess.run([sys.executable, "-I", str(TOUR), "merge", "--out", out, one, two], capture_output=True, text=True)
        if ran.returncode or "1 not in the lists before it" not in ran.stdout:
            fail("merge: %s%s" % (ran.stdout, ran.stderr))
        if states.read(out) != [rec(1, 2, 3), rec(1, 2, 4), rec(5, 0, 3)]:
            fail("merge did not write the three different pipelines in order")
        # compare: what the old list has that the new lacks (lost), and the other way round
        diff = tour.list_diff(states, one, two)
        if (diff["new"], diff["old"], diff["lost"], diff["gained"]) != (2, 2, {rec(5, 0, 3)}, {rec(1, 2, 3)}):
            fail("list_diff: %s" % diff)
        if tour.list_diff(states, out, one)["lost"]:
            fail("list_diff: the merge lost a pipeline of one of its lists")
        ran = subprocess.run([sys.executable, "-I", str(TOUR), "compare", one, two], capture_output=True, text=True)
        if ran.returncode or "lost): 1 pipelines" not in ran.stdout or "gained): 1 pipelines" not in ran.stdout:
            fail("compare: %s%s" % (ran.stdout, ran.stderr))
        ran = subprocess.run([sys.executable, "-I", str(TOUR), "compare", out, one], capture_output=True, text=True)
        if ran.returncode or "lost): none" not in ran.stdout:
            fail("compare against a list it holds whole: %s%s" % (ran.stdout, ran.stderr))
        ini = os.path.join(d, "hookshot_options.ini")
        open(ini, "w").write("dlss=1\nhdr=0\n")
        old = tour.set_ini(ini, [("hdr", "1"), ("handheld_profile", "1")])
        if old != "dlss=1\nhdr=0\n" or open(ini).read() != "dlss=1\nhdr=1\nhandheld_profile=1\n":
            fail("set_ini: %r" % open(ini).read())
        report = tour.write_report(d, [{"stop": "a", "seconds": 1, "pipelines": 2, "programs": 1, "new": 2},
                                       {"stop": "b", "seconds": 2, "pipelines": 3, "programs": 2, "new": 1}],
                                   {"name": "t", "seconds": 2, "env": {"X": "1"}, "ini": [], "completed": False, "error": "x"})
        if "3 pipelines" not in report or "NOT COMPLETED: x" not in report or not os.path.exists(os.path.join(d, "report.json")):
            fail("the report: %s" % report)
        if "renderer profile" in report:
            fail("a report without --vk-profile speaks of the renderer's profile")
        report = tour.write_report(d, [], {"name": "t", "seconds": 2, "env": {}, "ini": [], "settle": [60, 600],
                                           "vk_profile": {"file": "/p/t.profraw", "reset": True, "why": None}})
        if "renderer profile: /p/t.profraw" not in report:
            fail("the report does not say where the renderer's counters went: %s" % report)
        if "settle: on after 60 frames with no new pipeline, 600 at most" not in report:
            fail("the report does not say the settle's frames: %s" % report)
    # --publish: the tour's source written by nx2/pipeline_states.py tour, its note from what the runs' bridges said
    with tempfile.TemporaryDirectory() as d:
        part = os.path.join(d, "run-child")
        os.makedirs(part)
        open(os.path.join(part, "states.bin"), "wb").write(head + rec(1, 2, 3) + rec(5, 0, 3))
        json.dump({"meta": {"seconds": 120, "env": {"HOS_OUTPUT_RES": "640x360", "RX_VK_PGO_FILE": "/run/vk.profraw"},
                            "version": "v9.9.9-1-gabc",
                            "renderer": "Vulkan: Some GPU 9000, driver 1.2.3"}, "stops": []},
                  open(os.path.join(part, "report.json"), "w"))
        sources = os.path.join(d, "sources")
        os.makedirs(sources)
        if tour.publish([os.path.join(part, "states.bin")], [part], ["child"], sources):
            fail("publish failed")
        note = open(os.path.join(sources, "tour.md"), encoding="utf-8").read()
        if states.read(os.path.join(sources, "tour.bin.xz")) != [rec(1, 2, 3), rec(5, 0, 3)]:
            fail("publish did not write the parts' pipelines")
        for want in ("Some GPU 9000", "v9.9.9-1-gabc", "640x360", "parts child", "frame_rate_cap=6"):
            if want not in note:
                fail("the tour's note lacks %r:\n%s" % (want, note))
        if "RX_VK_PGO_FILE" in note or "vk.profraw" in note:
            fail("the tour's note names the renderer counters' file as a setting:\n%s" % note)


# ---- a fake bridge ----

class FakeGame:
    """The game as its bridge shows it (docs/control.md): frames advance with the waits, the title, then Link after some A
    presses, events that end, teleports refused while an event plays, a map that is not left, and the age switch"""
    def __init__(self, bancs):
        self.bancs, self.frame, self.map, self.age, self.event_until, self.loading = bancs, 100, None, "child", 0, False
        self.title_at, self.a_presses, self.pending, self.requests, self.lock = 300, 0, None, [], threading.Lock()
        self.refuse = {}     # (index: refusals still to give)
        self.stuck = set()   # (indices sent but never arriving)
        self.inputs = []
        self.stale_from, self.stale_until = 0, 0   # (status.game kept from stale_from until stale_until: a load's)
        self.refuse_text = None   # (the refusals' message: an event playing unless set)

    def advance(self, n):
        self.frame += n
        if self.pending and self.frame >= self.pending[1]:
            self.map, self.loading, self.event_until, self.pending = self.pending[0], False, self.frame + 40, None
        if self.map is None and self.a_presses >= 3:
            self.map, self.event_until = "LinkHouse", self.frame + 200

    def answer(self, req):
        with self.lock:
            self.requests.append(req)
            cmd, r = req.get("cmd"), {"id": req.get("id"), "ok": True}
            event = self.frame < self.event_until or self.loading
            if cmd == "status":
                self.advance(1)
                r.update(state="play" if self.map else "menu" if self.frame >= self.title_at else "intro",
                         draws=400.0 if self.frame >= self.title_at else 5.0, map=self.map, version="v0.0.0-test",
                         renderer="Vulkan: Fake GPU, driver 1")
                if self.map:   # (the snapshot is of this frame, or of the last before a stale spell: docs/control.md)
                    taken = self.frame if self.frame >= self.stale_until else self.stale_from
                    r["game"] = {"event": event, "loading": self.loading, "age": self.age, "frame": taken}
            elif cmd == "press":
                self.advance(1)
                if req.get("buttons") == "A" and self.frame >= self.title_at:
                    self.a_presses += 1
                r.update(queued=1, frames=req.get("frames", 8))
            elif cmd == "sequence":   # (steps in frames, as scripts/hookshot_ctl.py sends HOS units; or a script in them)
                if isinstance(req.get("steps"), list) and req["steps"]:
                    self.inputs.append(req["steps"])
                    span = max(s["at"] + s["frames"] for s in req["steps"])
                elif req.get("units") == "hos" and req.get("script"):
                    self.inputs.append(req["script"])
                    span = int(60 * max(float(p.split(":")[0]) + int((p.split("/") + ["200"])[1]) / 1000
                                        for p in req["script"].split(",")))
                else:
                    return {"id": req.get("id"), "ok": False, "error": "bad_request", "message": "steps or a script in hos units"}
                start = self.frame
                self.advance(span + 1)
                r.update(start=start, end=self.frame)
            elif cmd == "wait":
                if req["event"] == "present":
                    self.advance(req.get("frames", 1))
                elif req["event"] == "frame":
                    self.advance(max(0, req["frame"] - self.frame))
                elif req["event"] in ("map", "loading_end"):
                    if not self.pending or self.pending[1] == float("inf"):
                        self.advance(int(req.get("timeout", 60) * 60))
                        return {"id": req["id"], "ok": False, "frame": self.frame, "error": "timeout", "message": "not met"}
                    self.advance(self.pending[1] - self.frame)
                    if req["event"] == "map":
                        r["map"] = self.map
                r.update(state="play", waited=1)
            elif cmd in ("teleport", "age"):
                self.advance(2)
                to = req.get("to")
                key = to if cmd == "teleport" else "age"
                if self.refuse.get(key):
                    self.refuse[key] -= 1
                    r = {"id": req["id"], "ok": False, "error": "refused", "message": self.refuse_text or
                         "the game refused the teleport: an event is playing (a cutscene, a map change's)"}
                elif event:
                    r = {"id": req["id"], "ok": False, "error": "refused",
                         "message": "the game refused the teleport: an event is playing (a cutscene, a map change's)"}
                elif cmd == "age":
                    if self.age == to:
                        r.update(age=to, sent=False)
                    else:
                        self.age, self.loading = to, True
                        self.pending = (self.map, self.frame + 90)
                        r.update(age=to, sent=True, map=self.map, accepted=True)
                else:
                    child, adult = self.bancs[to]
                    target = child if self.age == "child" else adult
                    if target is None:
                        r = {"id": req["id"], "ok": False, "error": "refused",
                             "message": "the game refused the teleport: this destination has no map for the child"}
                    else:
                        self.pending = (target, float("inf") if to in self.stuck else self.frame + 120)
                        self.loading = to not in self.stuck
                        r.update(teleport=to, sent=True, map=target, accepted=to not in self.stuck)
            elif cmd == "quit":
                r["quitting"] = True
            else:
                r = {"id": req.get("id"), "ok": False, "error": "unknown_cmd", "message": cmd}
            r["frame"] = self.frame
            return r


def serve(fake, path):
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(2)

    def one(c):
        f = c.makefile("rwb", buffering=0)
        for raw in f:
            r = fake.answer(json.loads(raw))
            f.write(b'{"event":"present","sub":0,"frame":1,"fps":60,"state":"play"}\n')   # (an event first: skipped)
            f.write((json.dumps(r) + "\n").encode())

    def loop():
        while True:
            c, _ = srv.accept()
            threading.Thread(target=one, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return srv


def bridge_driving(tour, a, states, rec):
    """The run's steps against the fake bridge: a new game to control, the items through the live file, a teleport refused and
    asked again, a stop with no map for the age missed and said so, a map that is not left given up on, the age switched"""
    short = len(tempfile.gettempdir()) <= 48 or not os.path.isdir("/tmp")
    with tempfile.TemporaryDirectory(prefix="tour-", dir=None if short else "/tmp") as d:
        fake = FakeGame(a["bancs"])
        path = os.path.join(d, "control.sock")
        srv = serve(fake, path)
        ctl = tour.ctl_module()
        b = ctl.Bridge(path, 5)
        live = os.path.join(d, "live.txt")
        open(live, "w").close()
        lst = os.path.join(d, tour.LIST)
        os.makedirs(os.path.dirname(lst), exist_ok=True)
        open(lst, "wb").write(states.MAGIC + struct.pack("<I", states.SIZE) + rec(1, 2, 3))
        g = tour.Game(b, d, lst, live, cheat_gap=0)
        phases = tour.new_game(g)
        if set(phases) != {"title", "player", "control"} or fake.map != "LinkHouse" or fake.frame < fake.event_until:
            fail("new game: %s, in %s at frame %d (its event until %d)" % (phases, fake.map, fake.frame, fake.event_until))
        if not any(r.get("cmd") == "press" and r.get("wait") is False for r in fake.requests):
            fail("the title's A is not pressed through the bridge")
        # a teleport refused twice (an event playing) and asked again; then in the map, its arrival's event over
        child_stop = next(i for i, (c, _) in enumerate(a["bancs"]) if c)
        fake.refuse[child_stop] = 2
        st = tour.teleport(g, child_stop, a["bancs"][child_stop][0], "child")
        if st["map"] != a["bancs"][child_stop][0] or fake.map != st["map"] or st["event"] or st["loading"]:
            fail("the teleport did not arrive: %s" % st)
        if sum(1 for r in fake.requests if r.get("cmd") == "teleport") != 3:
            fail("a refused teleport is not asked again")
        # every request the tour sent is one the bridge has, with its arguments
        for r in fake.requests:
            if r["cmd"] == "teleport" and not isinstance(r.get("to"), int):
                fail("a teleport not by the destination's index: %s" % r)
        # the presses go as HOS_INPUT_SCRIPT's units, the reply once they are over
        before = fake.frame
        g.press(tour.LOOK)
        if fake.inputs[-1] not in (tour.LOOK, ctl.hos_steps(tour.LOOK)) or fake.frame - before < int((tour.LOOK_SPAN - 0.2) * 60):
            fail("the look around is not one sequence over its span")
        # the items: the Debug tab's actions in the live file, one a line
        before = fake.frame
        g.cheat(a["item"] + 200)
        g.cheat(a["item"] + 256 + 124)
        if open(live).read() != "!cheat %d\n!cheat %d\n" % (a["item"] + 200, a["item"] + 256 + 124):
            fail("the live file: %r" % open(live).read())
        if fake.frame - before < 6:   # (three presents after each line: one of a frame that read its input after it was there)
            fail("the live lines are not three frames apart (%d frames for two)" % (fake.frame - before))
        # an adult-only stop as the child: refused for good, a Failed at once (one try)
        adult_only = next(i for i, (c, _) in enumerate(a["bancs"]) if c is None)
        n = len(fake.requests)
        try:
            tour.teleport(g, adult_only, a["bancs"][adult_only][1], "child")
            fail("a teleport with no map for the age arrived")
        except tour.Failed as e:
            if "no map" not in str(e):
                raise
        if sum(1 for r in fake.requests[n:] if r.get("cmd") == "teleport") != 1:
            fail("a refusal for good is asked again")
        # a map that is not left (the Haunted Wasteland: sent, never moved): given up on after five sends
        other = next(i for i, (c, _) in enumerate(a["bancs"]) if c and i != child_stop)
        fake.stuck.add(other)
        n = len(fake.requests)
        try:
            tour.map_change(g, "teleport", lambda st: st["map"] == a["bancs"][other][0], 10 ** 9, arrive_s=1, to=other)
            fail("a teleport that never moved arrived")
        except tour.Failed as e:
            if "five times" not in str(e):
                raise
        if sum(1 for r in fake.requests[n:] if r.get("cmd") == "teleport") != 5:
            fail("an unmoved teleport is not sent five times")
        fake.stuck.clear()
        fake.pending = None
        # the age: switched, reloaded, then the adult's map; asked again for the adult: nothing to do
        st = tour.switch_age(g, "adult")
        if st["age"] != "adult" or fake.age != "adult":
            fail("the age switch: %s" % st)
        st = tour.switch_age(g, "adult")
        if st["age"] != "adult":
            fail("the age switch to the age Link is: %s" % st)
        # an adult stop now arrives at its adult map
        st = tour.teleport(g, adult_only, a["bancs"][adult_only][1], "adult")
        if st["map"] != a["bancs"][adult_only][1]:
            fail("the adult's teleport: %s" % st)
        # the settle: no new pipeline for the quiet frames, then on
        before = fake.frame
        g.settle(150, 600)
        if not 150 <= fake.frame - before <= 200:
            fail("settle took %d frames, wanted about 150" % (fake.frame - before))
        before = fake.frame
        g.settle(tour.SETTLE_QUIET, tour.SETTLE_MAX)
        if not tour.SETTLE_QUIET <= fake.frame - before <= tour.SETTLE_QUIET + 50:
            fail("the default settle took %d frames, wanted about %d" % (fake.frame - before, tour.SETTLE_QUIET))
        # a snapshot kept from before a load (game.frame behind the reply's) is not taken for control
        fake.stale_from, fake.stale_until = fake.frame - 1, fake.frame + 200
        g.calm(lambda st: True, 2000)
        if fake.frame < fake.stale_until:
            fail("calm took a stale snapshot (game.frame %d, reply %d) for control" % (fake.stale_from, fake.frame))
        # the teleport's 8 s (wall time) refusal: waited out, never pressed at
        presses = sum(1 for r in fake.requests if r.get("cmd") == "sequence")
        adult_stop = next(i for i, (_, adult) in enumerate(a["bancs"]) if adult and i != adult_only)
        fake.refuse_text = "the game refused the teleport: another teleport is still in progress (one every 8 s)"
        fake.refuse[adult_stop] = 12   # (720 frames of refusals: past the 240 after which a busy game is pressed at)
        st = tour.teleport(g, adult_stop, a["bancs"][adult_stop][1], "adult")
        fake.refuse_text = None
        if st["map"] != a["bancs"][adult_stop][1] or sum(1 for r in fake.requests if r.get("cmd") == "sequence") != presses:
            fail("the teleport's 8 s refusal is pressed at (unstick), where it is only waited out: %s" % st)
        g.steps.close()
        tour.stop_game(None, b)
        b.close()
        srv.close()


def folder_kept(tour):
    """What a run does to the game folder is undone: the notes and lists back from aside whatever the run left (and the run's moved
    out, across drives too), the options file as it was or none, the executable in bin/ in a builder's folder, a bad --cpus
    refused before anything; tour.md names no path of the run's and the run's own options; a stopped tour waits for its run"""
    with tempfile.TemporaryDirectory() as d:
        game, run_dir = os.path.join(d, "game"), os.path.join(d, "run")
        os.makedirs(os.path.join(game, "cache"))
        os.makedirs(run_dir)
        for name, body in (("cache/hookshot_pipeline_states.bin", "player's list"), ("cache/hookshot_computes.bin", "player's"),
                           ("hookshot_pipeline_states_shipped.bin", "shipped")):
            open(os.path.join(game, name), "w").write(body)
        aside = os.path.join(game, ".pipeline_tour_aside")
        tour.set_aside(game, aside)
        if any(os.path.exists(os.path.join(game, n)) for n in tour.STATE_FILES):
            fail("set_aside left a list or note in the game folder")
        open(os.path.join(game, "cache", "hookshot_pipeline_states.bin"), "w").write("the run's list")
        open(os.path.join(game, "cache", "hookshot_pipelines.bin"), "w").write("the run's notes")
        moved = []
        saved = tour.shutil.move
        tour.shutil.move = lambda src, dst: (moved.append(src), saved(src, dst))[1]   # (shutil.move: a copy across drives)
        try:
            problems = tour.put_back(game, aside, run_dir)
        finally:
            tour.shutil.move = saved
        if problems or os.path.exists(aside) or len(moved) != 2:
            fail("put_back: %s, aside %s, moved %s" % (problems, os.path.exists(aside), moved))
        if open(os.path.join(game, "cache", "hookshot_pipeline_states.bin")).read() != "player's list" or \
                open(os.path.join(game, "cache", "hookshot_computes.bin")).read() != "player's" or \
                os.path.exists(os.path.join(game, "cache", "hookshot_pipelines.bin")) or \
                open(os.path.join(run_dir, "hookshot_pipeline_states.bin")).read() != "the run's list":
            fail("put_back did not put the player's files back and the run's in its folder")
        # the options file: as it was, or none when there was none
        ini = os.path.join(game, tour.OPTIONS)
        old = tour.set_ini(ini, [("dlss", "0")])
        tour.restore_ini(ini, old)
        if old is not None or os.path.exists(ini):
            fail("a folder with no options file keeps the run's")
        open(ini, "w").write("dlss=1\n")
        tour.restore_ini(ini, tour.set_ini(ini, [("dlss", "0")]))
        if open(ini).read() != "dlss=1\n":
            fail("the options file is not put back")
        # the executable: bin/'s in a builder's folder, whose top-level hookshot starts the launcher
        open(os.path.join(game, "hookshot"), "w").close()
        os.makedirs(os.path.join(game, "bin"))
        if tour.game_exe(game) != os.path.join(game, "bin", "hookshot.exe" if os.name == "nt" else "hookshot"):
            fail("game_exe starts the builder's launcher, not bin/'s game: %s" % tour.game_exe(game))
    for bad in ("0-3,", "x", "5-2"):
        try:
            tour.cpu_list(bad)
            fail("--cpus %s taken" % bad)
        except SystemExit as e:
            if "--cpus" not in str(e):
                raise
    if tour.cpu_list("0-3, 12") != {0, 1, 2, 3, 12}:
        fail("cpu_list")
    src = TOUR.read_text(encoding="utf-8")
    body = src.split("\ndef run(args):", 1)[1].split("\ndef ", 1)[0]
    if "try:\n        touched = True\n        set_aside(game, aside)" not in body or \
            body.index("cpu_list(args.cpus)") > body.index("set_aside(game, aside)"):
        fail("the run changes the game folder before what can fail, or outside the try that puts it back")
    if "except KeyboardInterrupt:" not in body or "hold_stops()" not in body.split("finally:", 1)[1][:40]:
        fail("a stopped run does not write its report, or a second stop can cut its putting back short")
    # the environment: the shell's Hookshot switches are not the run's
    env = tour.run_environ({"PATH": "/bin", "RX_VK_NO_STATE_LIST": "1", "HOS_FPS_MAX": "30", "HOME": "/h"}, {"HOS_FPS_MAX": "1000"})
    if env != {"PATH": "/bin", "HOME": "/h", "HOS_FPS_MAX": "1000"}:
        fail("run_environ: %s" % env)
    if '"HOS_ENV_FILE": no_env' not in src or '[exe, "--user", user]' not in src:
        fail("the game reads the folder's hookshot_env.txt, or its saves are not the run's own")
    # tour.md: no path of the run's, the options as the runs had them
    if tour.published_settings({"RX_VK_SHOTS": "/home/u/run/shots", "RX_VK_PGO_FILE": "/r/vk.profraw", "RX_VK_SHOT_W": "320",
                                "HOS_OUTPUT_RES": "640x360", "RX_VK_X": "/abs"}) != {"RX_VK_SHOT_W": "320", "HOS_OUTPUT_RES": "640x360"}:
        fail("tour.md would name a path of the run's")
    # a stopped tour: its run given time to put the folder back, not killed a quarter of a second in
    class Child:
        def __init__(self):
            self.calls, self.signals = 0, []

        def wait(self, timeout=None):
            self.calls += 1
            if self.calls == 1:
                raise KeyboardInterrupt
            return 130

        def send_signal(self, sig):
            self.signals.append(sig)

        def kill(self):
            fail("a stopped tour killed its run at once")
    for term in (False, True):
        child = Child()
        del tour.TERMINATED[:]
        if term:
            tour.TERMINATED.append(15)
        try:
            tour.run_child(["run"], popen=lambda cmd: child)
            fail("a stopped tour went on")
        except KeyboardInterrupt:
            pass
        if child.calls != 2 or child.signals != ([tour.signal.SIGTERM] if term else []):
            fail("run_child (%s): waited %d times, signals %s" % ("SIGTERM" if term else "Ctrl+C", child.calls, child.signals))
    del tour.TERMINATED[:]
    if "run_child(run_command(" not in src:
        fail("the tour does not start its runs through run_child")


def memory_guard(tour, a):
    """The memory guard: /proc's numbers read, a game not started without memory (waiting, then giving up), a running one
    stopped past its limits, and the tour going on from where a stopped game was"""
    info = "MemTotal: 31000000 kB\nMemAvailable: %d kB\nSwapTotal: 60000000 kB\nSwapFree: %d kB\n"
    free, swap = tour.meminfo(info % (10 * 1048576, 60000000 - 2 * 1048576))
    if abs(free - 10) > 1e-9 or abs(swap - 2) > 1e-9:
        fail("meminfo: %s %s" % (free, swap))
    if tour.meminfo("MemTotal: 1 kB\n") != (None, None):
        fail("meminfo without MemAvailable")
    if tour.held_mb("VmRSS:\t9000000 kB\nRssAnon:\t1048576 kB\nRssFile:\t5000000 kB\nRssShmem:\t2097152 kB\n") != 3072:
        fail("held_mb is not anonymous + shared memory (the file pages can be dropped)")
    mem = dict(tour.MEM)
    if tour.start_problem(mem, (mem["min_free_gb"] + 1, 0)) is not None or tour.start_problem(mem, (None, None)) is not None:
        fail("a game may not start with memory free")
    if not tour.start_problem(mem, (mem["min_free_gb"] - 1, 0)) or not tour.start_problem(mem, (20, mem["max_swap_gb"] + 1)):
        fail("a game starts without memory or with swap full")
    if tour.run_problem(mem, (10, 1), mem["hard_mb"] - 1) is not None:
        fail("a game within its limits stopped")
    for info, held in (((10, 1), mem["hard_mb"] + 1), ((mem["floor_gb"] - 0.5, 1), 1000), ((10, mem["max_swap_gb"] + 5), 1000)):
        if not tour.run_problem(mem, info, held):
            fail("a game past a limit not stopped: %s, %s MB" % (info, held))
    if not mem["soft_mb"] < mem["hard_mb"] < mem["scope_gb"] * 1024 or mem["min_free_gb"] <= mem["floor_gb"]:
        fail("the limits are out of order: %s" % mem)
    now, looks = [0.0], iter([(2, 0), (2, 0), (12, 0)])
    sleep = lambda s: now.__setitem__(0, now[0] + s)
    if tour.wait_memory(dict(mem, wait_s=100), read=lambda: next(looks), sleep=sleep, clock=lambda: now[0], say=lambda *x, **k: 0):
        fail("wait_memory gave up though the memory came")
    now[0] = 0.0
    why = tour.wait_memory(dict(mem, wait_s=100), read=lambda: (1, 0), sleep=sleep, clock=lambda: now[0], say=lambda *x, **k: 0)
    if not why or now[0] < 100 or now[0] > 120:
        fail("wait_memory did not give up after its wait (%s, %s s)" % (why, now[0]))
    if tour.scope_prefix(dict(mem, scope_gb=0)) != []:
        fail("a scope though scope_gb is 0")
    ok = type("R", (), {"returncode": 0})
    bad = type("R", (), {"returncode": 1})
    if tour.shutil.which("systemd-run") and not os.environ.get("OOT_NO_SCOPE"):
        pre = tour.scope_prefix(mem, run=lambda *x, **k: ok)
        if pre[:3] != ["systemd-run", "--user", "--scope"] or "MemoryMax=%dG" % mem["scope_gb"] not in pre or pre[-1] != "--":
            fail("the scope: %s" % pre)
        if tour.scope_prefix(mem, run=lambda *x, **k: bad) != []:
            fail("a scope that does not start is used")
    plan = ["%s (%s)" % (a["teleports"][i][0], age) for age, i in tour.plan_stops(a, "child-overworld")]
    calls = []

    def start(script):
        def run(first, stops, k):
            calls.append((first, stops))
            return script[k]
        return run
    first_chunk = tour.chunks(a, "child-overworld", 4)
    tour.part_runs(a, "child-overworld", 4, start([(4, {"memory_stop": "x", "memory_soft": True, "left": plan[2:4]})] +
                                                  [(0, {})] * 20), say=lambda *x, **k: 0)
    if calls[:3] != [first_chunk[0], (plan[2], 2), first_chunk[1]] or len(calls) != len(first_chunk) + 1:
        fail("the tour does not go on from where the guard ended a game: %s" % calls[:4])
    calls.clear()
    hard = (4, {"memory_stop": "x", "memory_soft": False, "left": plan[1:4]})
    tour.part_runs(a, "child-overworld", 4, start([hard, hard] + [(0, {})] * 20), say=lambda *x, **k: 0)
    if calls[:3] != [first_chunk[0], (plan[1], 3), (plan[2], 2)]:
        fail("a stop the guard ended twice is not left out: %s" % calls[:3])
    calls.clear()
    out = tour.part_runs(a, "child-overworld", 4, start([(0, {}), (3, {"memory_wait": "x", "left": plan[4:8]})] + [(0, {})] * 20),
                         say=lambda *x, **k: 0)
    if out[-1] != (1, 3) or len(calls) != 2:
        fail("a part goes on after a game could not start for memory: %s" % out)
    calls.clear()
    tour.part_runs(a, "child-overworld", 4, start([(1, {})] + [(0, {})] * 20), say=lambda *x, **k: 0)
    if len(calls) != 1:
        fail("a part goes on after a failed game")
    args = type("A", (), {"mem": ["soft_mb=5000", "floor_gb=2.5"], "no_mem_guard": False})()
    m = tour.mem_settings(args)
    if m["soft_mb"] != 5000 or m["floor_gb"] != 2.5 or m["hard_mb"] != tour.MEM["hard_mb"]:
        fail("--mem: %s" % m)
    if tour.mem_settings(type("A", (), {"mem": [], "no_mem_guard": True})()) is not None:
        fail("--no-mem-guard still guards")
    try:
        tour.mem_settings(type("A", (), {"mem": ["nope=1"], "no_mem_guard": False})())
        fail("--mem with an unknown key taken")
    except SystemExit as e:
        if "nope" not in str(e):
            raise
    src = TOUR.read_text(encoding="utf-8")
    if "threading" in src:
        fail("the tour runs games at once (one game can hold 14 GB)")
    sleeps = re.findall(r"^.*\btime\.sleep\(.*$", src, re.M)
    if [s.strip() for s in sleeps] != ["time.sleep(self.cheat_gap)", "time.sleep(0.1)   # (no frames to count yet: the bridge is not there)"] \
            or "self.frames(3)" not in src.split("def cheat(", 1)[1].split("\n    def ", 1)[0]:
        fail("the tour waits on the game in wall time where it could count frames: %s" % sleeps)
    if "raise MemoryStop(why)" not in src or "wait_memory(mem)" not in src:
        fail("the run does not check memory")


def worth(tour, states, rec):
    """value: the steps a run's list was sliced into, what each alone adds, and the cut of those adding too little"""
    with tempfile.TemporaryDirectory() as d:
        r = [rec(i, 0, 3) for i in range(1, 40)]
        open(os.path.join(d, "states.bin"), "wb").write(states.MAGIC + struct.pack("<I", states.SIZE) + b"".join(r[:30]))
        rows = [{"stop": "new game: x", "took": 50, "pipelines": 10}, {"stop": "A", "took": 20, "pipelines": 22,
                 "marks": [["arrive", 15], ["look", 20]]}, {"stop": "B", "took": 30, "pipelines": 30}]
        json.dump({"meta": {}, "stops": rows}, open(os.path.join(d, "report.json"), "w"))
        steps = tour.run_steps(states, d)
        (r2,), _ = states.as_v2([r], force=True)
        if [(k, len(v)) for k, _, v in steps] != [("new game: x", 10), ("A", 12), ("B", 8)] or steps[1][2] != set(r2[10:22]):
            fail("run_steps: %s" % [(k, len(v)) for k, _, v in steps])
        sub = tour.run_steps(states, d, sub=True)
        if [(k, len(v)) for k, _, v in sub] != [("new game: x", 10), ("A / arrive", 5), ("A / look", 5), ("A / settle", 2), ("B", 8)]:
            fail("run_steps --sub: %s" % [(k, len(v)) for k, _, v in sub])
    s = lambda *ids: {i for i in ids}
    runs = [[("new game: x", 50, s(1)), ("B", 20, s(2, 3, 4)), ("D", 10, s(1, 2))],
            [("new game: x", 50, s(1)), ("C", 40, s(2, 3, 4)), ("E", 10, s(*range(10, 30)))]]
    rows = {r["step"]: r for r in tour.worth(runs, against=s(4), floor=2)}
    if not rows["new game: x"]["kept"] or rows["D"]["kept"] or not rows["E"]["kept"] or rows["E"]["only"] != 20:
        fail("worth: %s" % rows)
    if rows["B"]["kept"] == rows["C"]["kept"] or not rows["B"]["kept"]:   # (the slower of two equal steps goes)
        fail("worth: of two steps meeting the same pipelines not the faster kept: %s" % rows)
    if rows["B"]["beyond"] != 2 or rows["B"]["only"] != 2:
        fail("worth: the players' lists not taken away: %s" % rows["B"])


class CountingWriter:
    """host/pgo_rt.c's thread as an RX_VK_PGO=gen renderer runs it: every `period` s <file>.reset clears the counters and
    <file>.dump writes them to <file> (a raw profile with one counted function), each deleted once done"""
    def __init__(self, file, period=0.05):
        self.file, self.period, self.cleared, self.dumps, self.done = file, period, 0, 0, threading.Event()
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while not self.done.wait(self.period):
            if os.path.exists(self.file + ".reset"):
                self.cleared += 1
                os.remove(self.file + ".reset")
            if os.path.exists(self.file + ".dump"):
                self.dumps += 1
                with open(self.file, "wb") as f:
                    f.write(RAW_MAGIC + struct.pack("<QQQ", 10, 0, 1) + bytes(64) + struct.pack("<Q", self.cleared))
                os.remove(self.file + ".dump")


RAW_MAGIC = struct.pack("<Q", 0xff6c70726f667281)


def vk_profile(tour, a):
    """--vk-profile: a counting renderer told apart from another; the counters cleared once the new game gives control and
    written at a run's end, failed or not, then moved to <dir>/<name>.profraw; a renderer that takes neither request refused
    rather than leaving nothing; merge-profile's llvm-profdata merge, its refusals and --publish (xz -9e and the note)"""
    import lzma
    import types
    with tempfile.TemporaryDirectory() as d:
        # the renderer beside the game: the counting one names RX_VK_PGO_FILE (here across a read's 1 MB boundary)
        counting, plain, none = (os.path.join(d, n) for n in ("counting", "plain", "none"))
        for folder in (counting, plain, none):
            os.makedirs(folder)
        open(os.path.join(counting, "hookshot_vk.so"), "wb").write(b"x" * ((1 << 20) - 5) + tour.PGO_MARK + b"y" * 100)
        open(os.path.join(plain, "hookshot_vk.so"), "wb").write(b"x" * 5000 + b"RX_VK_PGO\x00HOS_PGO\x00")
        if tour.counting_renderer(counting) != os.path.join(counting, "hookshot_vk.so") or tour.counting_renderer(plain) or \
                tour.counting_renderer(none):
            fail("a counting renderer is not told apart from another")
        # run and tour refuse a game whose renderer does not count, before anything in its folder is touched
        open(os.path.join(plain, "hookshot"), "w").close()
        p = tour.parser()
        for argv in (["run", "--game", plain, "--out", os.path.join(d, "out"), "--vk-profile", os.path.join(d, "prof"),
                      "--no-mem-guard"],
                     ["tour", "--game", plain, "--out", os.path.join(d, "out"), "--vk-profile", os.path.join(d, "prof")]):
            try:
                (tour.run if argv[0] == "run" else tour.tour)(p.parse_args(argv))
                fail("%s --vk-profile ran with a renderer that does not count" % argv[0])
            except SystemExit as e:
                if "RX_VK_PGO=gen" not in str(e):
                    raise
            if os.path.exists(os.path.join(plain, ".pipeline_tour_aside")) or os.path.exists(os.path.join(plain, tour.OPTIONS)) \
                    or os.path.exists(plain + "-tour"):
                fail("%s --vk-profile touched the game folder before refusing it" % argv[0])
        # a run's counters: an earlier run's files gone, cleared on request, written at the end and moved
        out = os.path.join(d, "prof")
        file = os.path.join(d, "vk.profraw")
        for stale in (file, file + ".dump", file + ".reset"):
            open(stale, "w").close()
        prof = tour.Profile(file, out, "play-child-0", wait_s=5)
        if any(os.path.exists(f) for f in (file, file + ".dump", file + ".reset")):
            fail("an earlier run's counter files are kept")
        meta, why = tour.profile_end(prof, True)
        if why or meta["file"] or meta["reset"] or os.path.exists(file + ".dump"):
            fail("counters never cleared after control are dumped, or the run failed for it: %s %s" % (meta, why))
        writer = CountingWriter(file)
        prof.reset()
        if writer.cleared != 1 or not prof.was_reset or os.path.exists(file + ".reset"):
            fail("the counters are not cleared on request (%d)" % writer.cleared)
        meta, why = tour.profile_end(prof, True)
        to = os.path.join(out, "play-child-0.profraw")
        if why or meta["file"] != to or not os.path.isfile(to) or os.path.exists(file) or writer.dumps != 1 or tour.raw_problem(to):
            fail("the counters are not written and moved to <dir>/<name>.profraw: %s %s" % (meta, why))
        meta, why = tour.profile_end(prof, False)
        if why or meta["file"] or writer.dumps != 1:
            fail("a game that ended is asked for its counters: %s" % meta)
        writer.done.set()
        # a renderer that takes neither request: the reset fails the run, a dump that never comes fails it too
        lone = tour.Profile(os.path.join(d, "lone.profraw"), out, "play-adult-0", wait_s=0.3)
        try:
            lone.reset()
            fail("a reset nothing took passed")
        except tour.Failed as e:
            if "RX_VK_PGO=gen" not in str(e) or os.path.exists(lone.file + ".reset"):
                raise
        lone.was_reset = True
        meta, why = tour.profile_end(lone, True)
        if not why or "RX_VK_PGO=gen" not in why or meta["file"] or os.path.exists(os.path.join(out, "play-adult-0.profraw")) or \
                os.path.exists(lone.file + ".dump"):
            fail("a dump nothing wrote does not fail the run: %s %s" % (meta, why))
        if tour.profile_end(None, True) != (None, None):
            fail("a run without --vk-profile has a profile")
    # the reset comes once the new game has given control, before the items; none without --vk-profile
    for with_profile in (True, False):
        order, saved = [], tour.new_game
        tour.new_game = lambda g: order.append("control") or {}
        try:
            prof = types.SimpleNamespace(reset=lambda: order.append("reset")) if with_profile else None
            g = types.SimpleNamespace(mem=None, dry=False, profile=prof, note=lambda w: None,
                                      cheat=lambda n: order.append("cheat") if "cheat" not in order else None,
                                      frames=lambda n: None)
            rows = []
            tour.play(g, a, [("newgame",), ("items",)], rows, lambda label: (order.append("count"), rows.append({"stop": label})),
                      None)
        finally:
            tour.new_game = saved
        if order != (["control", "reset", "count", "cheat", "count"] if with_profile else ["control", "count", "cheat", "count"]):
            fail("the counters are not cleared right after control (%s): %s" % ("on" if with_profile else "off", order))
    src = TOUR.read_text(encoding="utf-8")
    if "env[PGO_ENV] = os.path.join(run_dir, \"vk.profraw\")" not in src or "g.profile = Profile(env[PGO_ENV], vk_out, args.name)" \
            not in src or "profile_end(g.profile, proc is not None and proc.poll() is None, hard)" not in src:
        fail("the run does not set RX_VK_PGO_FILE for the game, or does not write its counters at its end")
    if src.index("profile_end(g.profile") > src.index("stop_game(proc, bridge if not hard"):
        fail("the counters are asked for after the game was stopped")
    # merge-profile: llvm-profdata merge -o <out> <dir>/*.profraw; --publish: xz -9e and the note; refused with nothing counted
    with tempfile.TemporaryDirectory() as d:
        root, prof = os.path.join(d, "root"), os.path.join(d, "prof")
        os.makedirs(os.path.join(root, "assets", "profiles"))
        os.makedirs(prof)
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd)
            if cmd[1:] == ["--version"]:
                return types.SimpleNamespace(returncode=0, stdout="LLVM (http://llvm.org/):\n  LLVM version 23.1.5\n")
            with open(cmd[3], "wb") as f:
                for raw in cmd[4:]:
                    f.write(open(raw, "rb").read())
            return types.SimpleNamespace(returncode=0, stdout="")
        args = types.SimpleNamespace(dir=prof, out=None, publish=True, llvm_profdata="/kit/llvm-profdata")
        try:
            tour.merge_profile(args, root, fake_run)
            fail("merge-profile merged an empty folder")
        except SystemExit as e:
            if "no .profraw" not in str(e):
                raise
        raws = []
        for k, part in enumerate(("child-overworld", "dungeons")):
            raw = os.path.join(prof, "play-%s-%d.profraw" % (part, k))
            open(raw, "wb").write(RAW_MAGIC + struct.pack("<QQQ", 10, 0, 2) + bytes(64))
            json.dump({"name": "play-%s-%d" % (part, k), "part": part, "seconds": 600, "completed": k == 0,
                       "version": "v0.0.0-test", "renderer": "Vulkan: Fake GPU, driver 1"}, open(raw[:-8] + ".json", "w"))
            raws.append(raw)
        tour.merge_profile(args, root, fake_run)
        merged = os.path.join(root, "work", "hookshot_vk.profdata")
        if calls[0] != ["/kit/llvm-profdata", "merge", "-o", merged] + sorted(raws):
            fail("merge-profile's command: %s" % calls[0])
        data = open(merged, "rb").read()
        if lzma.decompress(open(os.path.join(root, tour.VK_PROFILE), "rb").read()) != data or \
                "preset=9 | lzma.PRESET_EXTREME" not in src:
            fail("--publish does not write the merged profile as xz -9e")
        note = open(os.path.join(prof, "README-note.md"), encoding="utf-8").read()
        for want in ("Fake GPU, driver 1", "child-overworld, dungeons", "2 games", "v0.0.0-test", "LLVM version 23.1.5",
                     "play-dungeons-1", time.strftime("%Y-%m-%d", time.gmtime())[:4]):
            if want not in note:
                fail("the profile's note lacks %r:\n%s" % (want, note))
        os.remove(os.path.join(root, tour.VK_PROFILE))
        tour.merge_profile(types.SimpleNamespace(dir=prof, out=os.path.join(d, "own.profdata"), publish=False,
                                                 llvm_profdata="/kit/llvm-profdata"), root, fake_run)
        if not os.path.isfile(os.path.join(d, "own.profdata")) or os.path.exists(os.path.join(root, tour.VK_PROFILE)):
            fail("merge-profile --out without --publish")
        for bad, why in ((b"not a profile at all, no" + bytes(16), "not an LLVM raw profile"),
                         (RAW_MAGIC + struct.pack("<QQQ", 10, 0, 0) + bytes(64), "no counted functions")):
            open(os.path.join(prof, "zz.profraw"), "wb").write(bad)
            try:
                tour.merge_profile(args, root, fake_run)
                fail("merge-profile merged a file that is %s" % why)
            except SystemExit as e:
                if why not in str(e):
                    raise
        os.remove(os.path.join(prof, "zz.profraw"))
        try:
            tour.merge_profile(args, root, lambda cmd, **kw: types.SimpleNamespace(returncode=1, stdout=""))
            fail("merge-profile passed though llvm-profdata failed")
        except SystemExit as e:
            if "wrote no profile" not in str(e):
                raise


def main():
    tour = load("pipeline_tour", TOUR)
    a = tour.actions()
    states = tour.state_tool()

    def rec(vs, fs, topo):
        r = bytearray(states.SIZE)
        struct.pack_into("<QQ", r, 0, vs, fs)
        struct.pack_into("<I", r, 44, topo)
        return bytes(r)

    source(tour, a)
    routes(tour, a)
    presses(tour)
    cli(tour, a)
    lists(tour, states, rec)
    bridge_driving(tour, a, states, rec)
    folder_kept(tour)
    memory_guard(tour, a)
    worth(tour, states, rec)
    vk_profile(tour, a)
    print("pipeline tour: source, route, presses, command line, lists, bridge, memory guard, worth and renderer profile ok")


if __name__ == "__main__":
    main()
