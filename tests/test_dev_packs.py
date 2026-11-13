"""scripts/dev_packs.py: how a change is built, which modules and files a pack keeps, and a runtime from a base and packs.

python -I -B tests/test_dev_packs.py
"""
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


packs = load("dev_packs_under_test", ROOT / "scripts" / "dev_packs.py")
make_game = load("make_game_under_test", ROOT / "nx2" / "make_game.py")


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def write(p, data=b"x"):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def test_route():
    r = packs.route
    check("the replay alone builds the renderer alone", r(["gpu/vk/vk_replay.cpp", "tests/test_x.py"]) == "renderer")
    check("the recompiler builds the renderer alone", r(["gpu/vk/eden/shader_recompiler/ir_opt/x.cpp"]) == "renderer")
    check("imgui builds the renderer alone", r(["third_party/imgui/imgui.cpp"]) == "renderer")
    check("a host change builds the game", r(["host/hos_thread.c"]) == "game")
    check("a game patch builds the game", r(["nx2/oot_patches.py"]) == "game")
    check("a renderer file and a host file build the game (both modules)", r(["gpu/vk/vk_draw.cpp", "host/nvn_native.c"]) == "game")
    check("the WebGPU renderer is in the game", r(["gpu/render/x.c"]) == "game")
    check("a build script builds the game", r(["nx2/build.py"]) == "game")
    check("docs and tests build nothing", r(["docs/development.md", "tests/test_a.py", "README.md"]) is None)
    check("an unknown path builds the game", r(["somewhere/new.c"]) == "game")


def test_stamp_matches_build():
    """TRANSLATOR is nx2/build.py's shader stamp: a stamp file missing here would leave a runtime unprepared for a new translator"""
    src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    dirs = re.search(r'for top in \(([^)]*)\) for d, _, fs in os\.walk\(os\.path\.join\(vk, top\)\)', src)
    files = re.search(r'\+ \[os\.path\.join\(vk, n\) for n in \(([^)]*)\)\]', src)
    check("nx2/build.py's shader stamp found (anchor: 'for top in (...) for d, _, fs in os.walk(os.path.join(vk, top))')", dirs)
    check("nx2/build.py's shader stamp files found (anchor: '+ [os.path.join(vk, n) for n in (...)]')", files)
    names = lambda s: tuple(re.findall(r'"([^"]+)"', s))
    check("the stamp's folders are dev_packs.STAMP_DIRS", names(dirs.group(1)) == packs.STAMP_DIRS)
    check("the stamp's files are dev_packs.STAMP_FILES", names(files.group(1)) == packs.STAMP_FILES)


def test_renderer_files_the_game_reads():
    """The renderer files the game is built from take the full route, so the runtime gets the game built from them too"""
    found = packs.renderer_read_by_game(str(ROOT))
    for f in ("gpu/vk/vk_api.h", "gpu/vk/dlss_math.h", "gpu/vk/compat/hookshot_env.h"):
        check("%s found as read by the game (%s)" % (f, sorted(found)), f in found)
        check("%s takes the full route" % f, packs.route([f]) == "game")
    check("a renderer file the game does not read stays renderer-only", "gpu/vk/vk_replay.cpp" not in found)


def test_stamped_objects_match_build():
    """The objects named in MODULES as holding the version are the files nx2/build.py gives it to"""
    src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    stamped = re.findall(r'\[stamp_def\] if os\.path\.basename\((?:s|src)\) == "([^"]+)"', src)
    check("nx2/build.py's version files found (anchor: '[stamp_def] if os.path.basename(...) == ...')", stamped)
    check("they are dev_packs.STAMPED_SOURCES", sorted(stamped) == sorted(os.path.basename(s) for s in packs.STAMPED_SOURCES))


def test_caches_named_in_renderer():
    text = "".join(p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "gpu" / "vk").glob("*.cpp"))
    for c in packs.CACHES + (packs.SEED,):
        check("the renderer still names %s" % c.replace(".predicted", ""), c.replace(".predicted", "") in text)
    check("the predicted notes' suffix", '".predicted"' in text)


def test_module_changed(d):
    base, mine = d / "base-build", d / "pack-build"
    for build in (base, mine):
        write(build / "obj" / "a.o", b"same a")
        write(build / "obj" / "host_hos_main.o", b"version " + build.name.encode())
        (build / "objs.rsp").write_text("".join('"%s"\n' % (build / "obj" / n) for n in ("a.o", "host_hos_main.o")))
    mc = lambda changed_source=False: packs.module_changed(str(base), str(mine), "objs.rsp", "host_hos_main.o", changed_source)
    check("only the version differs: the base's module", not mc())
    check("the version's own source changed: the pack's", mc(True))
    write(mine / "obj" / "a.o", b"other a")
    check("another object differs: the pack's", mc())
    write(mine / "obj" / "a.o", b"same a")
    write(mine / "obj" / "b.o", b"new")
    (mine / "objs.rsp").write_text("".join('"%s"\n' % (mine / "obj" / n) for n in ("a.o", "b.o", "host_hos_main.o")))
    check("another set of objects: the pack's", mc())
    check("no link list in the base: the pack's", packs.module_changed(str(d / "none"), str(mine), "objs.rsp", "host_hos_main.o", False))


def test_inputs_beside_the_objects():
    lift = {os.path.relpath(f, ROOT).replace(os.sep, "/") for f in make_game.lift_sources()}
    build = packs.build_scripts(str(ROOT))
    for f in ("nx2/build.py", "nx2/win_manifest.py", "nx2/version.py", "third_party/a64-kit/tools/toolchain.py"):
        check("%s found among the build's scripts (%s)" % (f, sorted(build)), f in build)
    check("the lift's own scripts are not the build's", "nx2/lift.py" not in build)
    lic = lambda *p: packs.link_inputs_changed(list(p), lift, build)
    check("a game patch shows in the objects", not lic("nx2/oot_patches.py", "host/hos_thread.c"))
    check("toolchain.py's link options, though the lift reads it too: both modules", lic("third_party/a64-kit/tools/toolchain.py"))
    check("make_game.py's own helpers show in the game folder", not lic("nx2/pipeline_states.py"))
    check("make_game.py sets build.py's environment (the hot order): both modules", lic("nx2/make_game.py"))
    check("the Windows manifest does not: both modules", lic("nx2/win_manifest.py"))
    check("nor build.py's flags", lic("nx2/build.py"))
    check("nor a library", lic("third_party/sdl2/SDL2.lib"))
    check("nor a shader source", lic("gpu/vk/shaders/blit.comp"))
    data = sorted(f for f in lift if f.endswith((".txt", ".json")))
    check("a translator data file shows in the objects (%s)" % data[:1], data and not lic(data[0]))
    md = packs.stamped_may_differ
    check("the version object's own source", md(["host/hos_main.c"], "host/hos_main.c"))
    check("a header it may include (oot_cheat_items.h reaches hos_main.c and vk_menu.cpp)", md(["gpu/vk/oot_cheat_items.h"], "host/hos_main.c"))
    check("another source alone: only the version differs there", not md(["host/nvn_native.c"], "host/hos_main.c"))


def refused(what, argv, reason):
    """Refused for that reason, not another check's"""
    try:
        packs.main(argv)
    except SystemExit as e:
        check(what + " refused because %s (%r)" % (reason, e.code), isinstance(e.code, str) and reason in e.code)
    else:
        check(what + " refused", False)


def test_guards(d):
    base = d / "base"
    write(base / "game" / "user" / "save0.bin", b"played")
    refused("a base built into a played game folder", ["base", str(base), "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)],
            "new game folder")
    f = packs.pack_folders(str(base))
    refused("adopting the packs' own work folder", ["adopt", str(base), "--work", f["game_work"], "--game", str(base / "game"),
                                                    "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)], "packs are built")
    write(base / "game" / packs.GAME, b"g")
    write(base / "game" / packs.RENDERER, b"r")
    packs.write_base(str(base), {"commit": "c" * 40, "work": str(d / "w"), "game": str(base / "game"), "romfs": "r", "exefs": "e"})
    refused("adopting a game folder inside the packs' output folder",
            ["adopt", str(d / "b2"), "--work", str(d / "w"), "--game", os.path.join(packs.pack_folders(str(d / "b2"))["game_out"], "g"),
             "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)], "where its packs are built")
    check("folders on two drives do not overlap (no error)", not packs.overlaps("C:\\a", "D:\\a") if os.name == "nt" else True)
    link = d / "alias"
    target = packs.pack_folders(str(d / "b3"))["game_out"]
    os.makedirs(target)
    try:
        os.symlink(target, link, target_is_directory=True)
    except OSError:   # (Windows without the right to make links: a junction, which needs none)
        import _winapi
        _winapi.CreateJunction(target, str(link))
    refused("adopting the packs' output folder through a link", ["adopt", str(d / "b3"), "--work", str(d / "w"), "--game", str(link),
                                                                 "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)],
            "where its packs are built")
    write(d / "launcher" / "bin" / packs.GAME, b"g")
    refused("a launcher's game folder (modules in bin/)", ["adopt", str(d / "b4"), "--work", str(d / "w"), "--game", str(d / "launcher"),
                                                         "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)], "plain game folder")
    refused("a runtime inside the base's game folder", ["rt", str(base), str(base / "game" / "runtime")], "folder of its own")
    check("...and nothing written there", not (base / "game" / "runtime").exists())
    write(base / "game" / "language.txt", b"# the game's language\nde-DE\n")
    check("the base's language is read for its packs", packs.game_language(str(base / "game")) == "de-DE")
    for name in ("../game", "..", "a/b", str(d)):
        refused("the pack name %r" % name, ["pack", str(base), name, "--tree", str(ROOT)], "pack's name")
    check("the base is still there", (base / "game" / "user" / "save0.bin").exists())


def test_pack_folders(d):
    """The lift key hashes the work and game folders' paths (make_game passes them to the lift as OOT_WORK and OOT_DUMP): a game
    pack reuses a translation only when every pack of a base builds in the same two folders"""
    f = packs.pack_folders(str(d))
    check("the same folders for every pack", f == packs.pack_folders(str(d)) and not f["game_work"].startswith(os.path.join(str(d), "packs")))
    exefs, work, out = d / "exefs", Path(f["game_work"]), Path(f["game_out"])
    write(exefs / "main", b"NSO0 one")
    env = lambda w, o: {"OOT_WORK": str(w), "OOT_DUMP": str(o), "RX_RELEASE": "1", "A64_HOT_CHUNK": "1"}
    key = make_game.lift_key(str(exefs), str(work), env(work, out))
    check("the key is the same in the same folders", key == make_game.lift_key(str(exefs), str(work), env(work, out)))
    check("...and differs in another (why packs share one folder)",
          key != make_game.lift_key(str(exefs), str(d / "other"), env(d / "other", out)))
    base = d / "base-work"
    write(base / "objcache" / "ab" / "abcdef.o", b"object")
    write(base / "system_headers.txt", b"headers")
    write(base / "sa" / "gen" / "code_0001.c", b"lifted")
    packs.link_objects(str(base), str(work))
    check("the base's objects, hard-linked", os.path.samefile(work / "objcache" / "ab" / "abcdef.o", base / "objcache" / "ab" / "abcdef.o"))
    check("the compiler's header note copied", (work / "system_headers.txt").read_bytes() == b"headers")
    check("no lifted C of the base's (another folder's key)", not (work / "sa").exists())
    (work / "objcache" / "ab" / "mine.o").write_bytes(b"pack")
    packs.link_objects(str(base), str(work))
    check("a work folder already there is left as it is", (work / "objcache" / "ab" / "mine.o").exists())
    real_link = os.link

    def cross_drive(src, dst):
        raise OSError(18, "Invalid cross-device link")
    os.link = cross_drive
    try:
        packs.link_objects(str(base), str(d / "other-drive"))
    finally:
        os.link = real_link
    check("on another drive the objects are copied", (d / "other-drive" / "objcache" / "ab" / "abcdef.o").read_bytes() == b"object")


def test_generated_and_runtime(d):
    game_mod, vk_mod = packs.GAME, packs.RENDERER
    base_dir, game, out = d / "base", d / "base" / "game", d / "out"
    write(game / "romfs" / "a" / "data.bin", b"romfs")
    write(game / "romfs" / "prompts" / "icon.bntx", b"base icon")
    write(game / game_mod, b"base game")
    write(game / vk_mod, b"base renderer")
    write(game / "hookshot_programs.bin", b"cache")
    write(game / "language.txt", b"en-US")
    write(game / "prompts" / "Common.bntx", b"base prompts")
    write(game / "hookshot_pipeline_states_shipped.bin", b"shipped")
    write(game / "hookshot_options.ini", b"[menu]")
    if packs.MODULES[0][1]:   # (the game's map: Windows only)
        write(game / packs.MODULES[0][1], b"map")
    outputs = packs.build_outputs(str(game))
    write(game / "user" / "save0.bin", b"a save, made in play")
    write(game / "hookshot_env.txt", b"HOS_X=1")
    os.makedirs(out / "romfs" / "a")
    os.link(game / "romfs" / "a" / "data.bin", out / "romfs" / "a" / "data.bin")
    write(out / "romfs" / "prompts" / "icon.bntx", b"new icon")
    write(out / game_mod, b"built game")
    write(out / "hookshot_programs.bin", b"other cache")
    write(out / "language.txt", b"en-US")
    gen, gone = packs.generated_changes(str(game), str(out), outputs)
    check("a regenerated file is kept, the same ones, the modules and the caches are not: %s" % gen, gen == ["romfs/prompts/icon.bntx"])
    check("generated files the build no longer makes are gone; saves, settings and the game's own files are not: %s" % gone,
          gone == ["hookshot_pipeline_states_shipped.bin", "prompts/Common.bntx"])
    check("a base that recorded no outputs (adopted) loses nothing", packs.generated_changes(str(game), str(out))[1] == [])
    write(out / "stale.bin", b"an earlier pack's")
    packs.clear_output(str(out))
    check("the output folder keeps only romfs for the next build", sorted(os.listdir(out)) == ["romfs"])
    check("...its links intact", os.path.samefile(out / "romfs" / "a" / "data.bin", game / "romfs" / "a" / "data.bin"))
    b = {"commit": "c" * 40, "work": str(base_dir / "work"), "game": str(game), "romfs": "r", "exefs": "e"}
    packs.write_base(str(base_dir), b)
    for name, files, generated, on in (("cpu", {game_mod: "h"}, ["romfs/prompts/icon.bntx"], b["commit"]),
                                       ("cpu2", {game_mod: "h"}, [], b["commit"]), ("old", {vk_mod: "h"}, [], "o" * 40)):
        p = base_dir / "packs" / name
        for m in files:
            write(p / m, b"pack " + name.encode())
        for rel in generated:
            write(p / "files" / rel, b"new icon")
        (p / "pack.json").write_text(json.dumps({"base": on, "files": files, "generated": generated, "prepare": False,
                                                 "deleted": ["prompts/Common.bntx"] if name == "cpu" else []}))
    rt = d / "rt"
    packs.main(["rt", str(base_dir), str(rt), "cpu"])
    check("the pack's game", (rt / game_mod).read_bytes() == b"pack cpu")
    check("the base's renderer", (rt / vk_mod).read_bytes() == b"base renderer")
    check("romfs hard-linked", os.path.samefile(rt / "romfs" / "a" / "data.bin", game / "romfs" / "a" / "data.bin"))
    check("the caches copied, not linked (the game writes them)", not os.path.samefile(rt / "hookshot_programs.bin", game / "hookshot_programs.bin"))
    check("the pack's generated file", (rt / "romfs" / "prompts" / "icon.bntx").read_bytes() == b"new icon")
    check("...not written through the base's hard link", (game / "romfs" / "prompts" / "icon.bntx").read_bytes() == b"base icon")
    check("the pack's deleted file is not in the runtime", not (rt / "prompts" / "Common.bntx").exists())
    check("...and still in the base", (game / "prompts" / "Common.bntx").exists())
    for asked, what, why in ((["cpu", "cpu2"], "two packs with the same module", "both bring"),
                             (["old"], "a pack on another base", "not the base")):
        refused(what, ["rt", str(base_dir), str(d / ("rt-" + asked[-1])), *asked], why)
    victim = d / "outside.txt"
    write(victim, b"not the runtime's")
    for name, meta, why in (("abs", {"generated": [str(victim)]}, "outside the runtime"),
                            ("up", {"deleted": ["../outside.txt"]}, "outside the runtime"),
                            ("deep", {"generated": ["romfs/../../outside.txt"]}, "outside the runtime"),
                            ("mod", {"files": {"../outside.txt": "h"}}, "names modules")):
        p = base_dir / "packs" / name
        (p / "files").mkdir(parents=True, exist_ok=True)
        (p / "pack.json").write_text(json.dumps(dict({"base": b["commit"], "files": {}, "generated": [], "deleted": [],
                                                      "prepare": False}, **meta)))
        refused("a pack.json naming %s" % meta, ["rt", str(base_dir), str(d / ("rt-" + name)), name], why)
        check("...and nothing made", not (d / ("rt-" + name)).exists())
    check("the file outside is untouched", victim.read_bytes() == b"not the runtime's")
    refused("a pack.json naming ..\\outside.txt (a backslash path)",
            ["rt", str(base_dir), str(d / "rt-bs"), *_pack(base_dir, b, "bs", {"deleted": ["..\\outside.txt"]})], "outside the runtime")
    # A link in the base's romfs is cloned into the runtime. A relative one that names a file from the base names a folder
    # beside the runtime once cloned (../../outside from base/game/romfs is base/outside, from rt-link/romfs it is outside/),
    # so a deleted path through it reads as inside and is not. The runtime is made, then refused at that file.
    write(base_dir / "outside", b"a file beside the base's game folder")
    write(d / "outside" / "victim.txt", b"beside the runtime")
    try:
        os.symlink(os.path.join("..", "..", "outside"), game / "romfs" / "escape")
    except OSError:   # (no right to make a file link here, as on Windows without developer mode: nothing to check)
        print("  skipped: a file link in the base's romfs (this account cannot make one)")
    else:
        try:
            packs.main(["rt", str(base_dir), str(d / "rt-link"), _pack(base_dir, b, "link", {"deleted": ["romfs/escape/victim.txt"]})[0]])
        except SystemExit as e:
            if os.path.islink(d / "rt-link" / "romfs" / "escape"):
                check("a deleted path through a cloned link refused (%r)" % e.code, isinstance(e.code, str) and "through a link" in e.code)
        check("the file beside the runtime is untouched", (d / "outside" / "victim.txt").read_bytes() == b"beside the runtime")
        os.remove(game / "romfs" / "escape")


def test_resolves_inside(d):
    """The check made where each file is written: a folder link inside the runtime that leads out is outside"""
    rt, out = d / "rt", d / "out"
    (rt / "romfs" / "real").mkdir(parents=True)
    out.mkdir()
    try:
        os.symlink(out, rt / "romfs" / "door", target_is_directory=True)
    except OSError:   # (Windows without the right to make links: a junction, which needs none)
        import _winapi
        _winapi.CreateJunction(str(out), str(rt / "romfs" / "door"))
    check("a real folder in the runtime is inside", packs.resolves_inside(str(rt), str(rt / "romfs" / "real")))
    check("a folder not made yet below the runtime is inside", packs.resolves_inside(str(rt), str(rt / "romfs" / "new" / "deeper")))
    check("a folder link in the runtime that leads out is outside", not packs.resolves_inside(str(rt), str(rt / "romfs" / "door")))
    check("...and a path through it", not packs.resolves_inside(str(rt), str(rt / "romfs" / "door" / "x")))
    check("the folder beside the runtime is outside", not packs.resolves_inside(str(rt), str(out)))
    upper = str(rt).upper() + os.sep + "ROMFS"
    if os.path.exists(upper) and os.path.samefile(upper, rt / "romfs"):   # (a case-insensitive folder: the usual one on Windows)
        check("a path written in another case is the same folder (compared by identity, not by name)",
              packs.resolves_inside(str(rt), upper))
    for rel, ok in (("romfs/a.bin", True), ("hookshot.exe", True), ("..", False), ("a/../b", False), ("a\\..\\b", False),
                    (".", False), ("", False), ("/x", False), ("romfs/foo:evil", False), ("romfs/foo::$DATA", False)):
        check("in_runtime(%r) is %s" % (rel, ok), packs.in_runtime(str(rt), rel) == ok)


def _pack(base_dir, b, name, meta):
    """A pack folder with this pack.json; returns [name] for an rt command line"""
    p = base_dir / "packs" / name
    (p / "files").mkdir(parents=True, exist_ok=True)
    (p / "pack.json").write_text(json.dumps(dict({"base": b["commit"], "files": {}, "generated": [], "deleted": [], "prepare": False},
                                                 **meta)))
    return [name]


def fake_base(d, predict_s=0):
    """A base with a game folder: romfs, both modules, two caches"""
    base_dir, game = d / "base", d / "base" / "game"
    write(game / "romfs" / "a" / "data.bin", b"romfs")
    write(game / packs.GAME, b"base game")
    write(game / packs.RENDERER, b"base renderer")
    write(game / "hookshot_programs.bin", b"programs")
    write(game / "mkw_vk_pipelines.bin", b"pipelines")
    packs.write_base(str(base_dir), {"commit": "c" * 40, "work": str(base_dir / "work"), "game": str(game), "romfs": "r",
                                     "exefs": "e", "predict_s": predict_s})
    return base_dir, game


def test_predict_budget(d):
    """--predict-s reaches make_game.py's environment (and the runtime's own prepare), and 0 leaves it out"""
    check("no budget by default", packs.base_env(0) == {})
    check("a budget is RX_VK_PREDICT_S", packs.base_env(60) == {"RX_VK_PREDICT_S": "60"})
    seen = []
    real_run = packs.run
    packs.run = lambda cmd, cwd, env=None: seen.append(env)
    try:
        packs.make_game(str(ROOT), {"romfs": "r", "exefs": "e"}, str(d / "w"), str(d / "g"), packs.base_env(60))
    finally:
        packs.run = real_run
    check("make_game.py gets the budget (%s)" % seen, seen and seen[0].get("RX_VK_PREDICT_S") == "60"
          and seen[0].get("HOOKSHOT_KEEP_BUILD_FILES") == "1")
    # (base itself, its git, its build and the game's prepare stood in for: make_game.py builds without preparing and never sees
    # the budget, which would change its lift key; the prepare after it gets it; base.json keeps it)
    got, prepared = {}, []

    def fake_make_game(tree, b, work, game, env):
        got.update(env)
        for m in packs.MODULES:
            write(Path(game) / m[0], b"built")
            if m[1]:
                write(Path(work) / "sa" / "build" / m[1], b"map")
    saved = packs.make_game, packs.changed_files, packs.git, packs.subprocess.run
    packs.make_game, packs.changed_files, packs.git = fake_make_game, (lambda tree, commit: set()), (lambda tree, *a: "c" * 40)
    packs.subprocess.run = lambda argv, **kw: prepared.append(kw.get("env")) or packs.subprocess.CompletedProcess(argv, 0)
    try:
        packs.main(["base", str(d / "b60"), "--romfs", "r", "--exefs", "e", "--tree", str(ROOT), "--predict-s", "60"])
    finally:
        packs.make_game, packs.changed_files, packs.git, packs.subprocess.run = saved
    check("make_game.py builds without preparing and never sees the budget (%s)" % got,
          got.get("HOOKSHOT_NO_PREPARE") == "1" and "RX_VK_PREDICT_S" not in got)
    if not packs.CROSS:
        check("the prepare after it gets the budget (%s)" % [e and e.get("RX_VK_PREDICT_S") for e in prepared],
              len(prepared) == 1 and prepared[0].get("RX_VK_PREDICT_S") == "60")
    check("base.json keeps it", json.loads((d / "b60" / "base.json").read_text()).get("predict_s") == 60)
    base_dir, game = fake_base(d, predict_s=45)
    p = base_dir / "packs" / "tr"
    write(p / packs.RENDERER, b"new renderer")
    (p / "pack.json").write_text(json.dumps({"base": "c" * 40, "files": {packs.RENDERER: "h"}, "generated": [], "deleted": [],
                                             "prepare": True}))
    envs = []
    real = packs.subprocess.run
    packs.subprocess.run = lambda argv, **kw: envs.append(kw.get("env")) or packs.subprocess.CompletedProcess(argv, 0)
    try:
        packs.main(["rt", str(base_dir), str(d / "rt-tr"), "tr"])
    finally:
        packs.subprocess.run = real
    if packs.CROSS:
        check("a cross-built runtime prepares nothing", envs == [])
    else:
        check("the runtime's prepare gets the base's budget (%s)" % [e and e.get("RX_VK_PREDICT_S") for e in envs],
              len(envs) == 1 and envs[0].get("RX_VK_PREDICT_S") == "45")


def test_warm(d):
    """warm plays a runtime with no pack, copies back the caches play made or grew and nothing else, and deletes the runtime"""
    base_dir, game = fake_base(d)
    play = d / "play.py"
    play.write_text("import sys, os\n"
                    "rt = sys.argv[1]\n"
                    "open(os.path.join(rt, 'hookshot_programs.bin'), 'ab').write(b' and more')\n"
                    "open(os.path.join(rt, 'mkw_vk_pipelines_play_1.bin'), 'wb').write(b'played')\n"
                    "open(os.path.join(rt, 'hookshot_log.txt'), 'w').write('a log')\n"
                    "open(os.path.join(rt, %r), 'wb').write(b'not a cache')\n" % packs.GAME)
    cmd = '"%s" -I "%s" "{runtime}"' % (sys.executable.replace("\\", "/"), str(play).replace("\\", "/"))
    packs.main(["warm", str(base_dir), "--run", cmd])
    check("a grown cache comes back", (game / "hookshot_programs.bin").read_bytes() == b"programs and more")
    check("a new play file comes back", (game / "mkw_vk_pipelines_play_1.bin").read_bytes() == b"played")
    check("an unchanged cache stays", (game / "mkw_vk_pipelines.bin").read_bytes() == b"pipelines")
    check("a log is not a cache", not (game / "hookshot_log.txt").exists())
    check("a module is never copied back", (game / packs.GAME).read_bytes() == b"base game")
    check("the warm-up's runtime is gone", not (base_dir / "warm-runtime").exists())
    b = json.loads((base_dir / "base.json").read_text())
    check("base.json records the warm-up (%s)" % b.get("warmed"),
          b.get("warmed", {}).get("files") == ["hookshot_programs.bin", "mkw_vk_pipelines_play_1.bin"])
    check("the busy marker is gone after the job", not (base_dir / packs.BUSY).exists())
    packs.main(["rt", str(base_dir), str(base_dir / "warm-runtime")])
    packs.main(["warm", str(base_dir), "--run", cmd])
    check("a runtime made beforehand by rt is warmed and deleted", not (base_dir / "warm-runtime").exists())
    packs.main(["rt", str(base_dir), str(base_dir / "warm-runtime")])
    p = base_dir / "packs" / "x"
    write(p / packs.GAME, b"pack game")
    (p / "pack.json").write_text(json.dumps({"base": "c" * 40, "files": {packs.GAME: "h"}, "generated": [], "deleted": [],
                                            "prepare": False}))
    (base_dir / "warm-runtime" / "dev_packs.json").write_text(json.dumps({"base": "c" * 40, "packs": ["x"]}))
    refused("warming a runtime with a pack in it", ["warm", str(base_dir), "--run", cmd], "not this base's runtime with no pack")
    shutil.rmtree(base_dir / "warm-runtime")
    # (a cache in the runtime that is a link to a file outside it is never followed: its bytes stay out of the base)
    outside = d / "outside.bin"
    outside.write_bytes(b"not a cache of this runtime")
    packs.main(["rt", str(base_dir), str(base_dir / "warm-runtime")])
    lnk = base_dir / "warm-runtime" / "mkw_vk_pipelines_play_7.bin"
    try:
        os.symlink(outside, lnk)
    except OSError:
        lnk = None
    if lnk is None:
        print("  skipped: a file link in the warm-up's runtime (this account cannot make one)")
        shutil.rmtree(base_dir / "warm-runtime")
    else:
        packs.main(["warm", str(base_dir), "--run", '"%s" -c "pass"' % sys.executable.replace("\\", "/")])
        check("a linked cache file is not copied back", not (game / "mkw_vk_pipelines_play_7.bin").exists())


def test_busy(d):
    """A second job on a busy base is refused, and the other job's marker is left alone"""
    base_dir, game = fake_base(d)
    (base_dir / packs.BUSY).write_text("1234 pack 2026-11-12T12:00:00Z")
    refused("a runtime while the base is busy", ["rt", str(base_dir), str(d / "rt-x")], "busy")
    refused("a warm-up while the base is busy", ["warm", str(base_dir), "--run", "x"], "busy")
    check("nothing made", not (d / "rt-x").exists() and not (base_dir / "warm-runtime").exists())
    check("the other job's marker is untouched", (base_dir / packs.BUSY).read_text() == "1234 pack 2026-11-12T12:00:00Z")


def main():
    test_route()
    test_stamp_matches_build()
    test_renderer_files_the_game_reads()
    test_stamped_objects_match_build()
    test_caches_named_in_renderer()
    test_inputs_beside_the_objects()
    with tempfile.TemporaryDirectory(prefix="hookshot-dev-packs-") as d:
        d = Path(d)
        for name, t in (("mc", test_module_changed), ("pf", test_pack_folders), ("rt", test_generated_and_runtime),
                        ("guard", test_guards), ("inside", test_resolves_inside), ("predict", test_predict_budget), ("warm", test_warm), ("busy", test_busy)):
            (d / name).mkdir()
            t(d / name)
    print("test_dev_packs: ok")


if __name__ == "__main__":
    main()
