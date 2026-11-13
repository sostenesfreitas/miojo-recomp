"""Development builds as a base and small packs over it: build the game once, then build only what a change touches and run it
from a copy of that base. For development and A/B tests only: it calls nx2/make_game.py and nx2/build.py as they are and
changes nothing in what players build (docs/development.md, *Testing a change against a base*).

    python -I scripts/dev_packs.py base <dir> --romfs <romfs> --exefs <exefs> [--seed <game folder>] [--predict-s N]
        Build this checkout (no uncommitted or untracked sources: a base is a commit) into <dir>/game, keeping its work folder
        <dir>/work. --seed copies the shader caches of an earlier game folder first, as a player's update keeps them: they are
        stamped by the shader translator, so the prepare redoes only what changed instead of starting cold. --predict-s N
        (development only; 0, the default, builds them all) stops building the whole game's predicted pipelines after N
        seconds (RX_VK_PREDICT_S): the rest are built in play when first drawn, so warm the base for the scenes you test.
    python -I scripts/dev_packs.py warm <dir> --run "<command with {runtime}>" [--run ...]
        Play a runtime with no pack once per --run command (split as a POSIX shell would; {runtime} is its folder; it uses
        <dir>/warm-runtime when rt made it there beforehand), copy the caches play made or grew back into the base's game
        folder, and delete the runtime: every later runtime starts warm for those scenes.
    python -I scripts/dev_packs.py adopt <dir> --work <work> --game <game> --romfs <romfs> --exefs <exefs>
        Use a full build of this checkout's commit that already exists (built with HOOKSHOT_KEEP_BUILD_FILES=1) as the base.
    python -I scripts/dev_packs.py pack <dir> <name> [--tree <checkout>] [--prepare | --no-prepare]
        Build a change (a checkout whose history contains the base's commit; committed, uncommitted and untracked files all
        count) into <dir>/packs/<name>:
          - only renderer files the game is not built from (gpu/vk/ and the renderer's third_party, but renderer_read_by_game):
            the renderer alone (RX_VK_ONLY: no lift, no game; seconds to minutes);
          - anything else: the game with make_game.py (HOOKSHOT_NO_PREPARE=1), which builds both modules, in one work folder
            shared by the base's packs (pack_folders), so after the first game pack the lift is skipped while the translator,
            the HLE list and the hot list stay the same.
        The pack keeps a module when an object it is linked from differs from the base's (module_changed), and any other file
        of the built game folder that differs from the base's (button prompts and the like). A runtime prepares again when the
        shader translator (what nx2/build.py stamps the shader caches with) or the shader seed changed.
    python -I scripts/dev_packs.py rt <dir> <runtime> [<pack>...]
        A game folder to run: the base's, its romfs hard-linked (read-only), the rest copied (the caches are written in play),
        the packs' files over it, prepared again if a pack needs it.
    python -I scripts/dev_packs.py ls <dir>

A pack's modules come from the same base as the rest of the runtime, so the game and the renderer always match
(MKWVK_API_VERSION). Two packs that bring the same file are refused: merge their branches and build one pack. base, pack, rt
and warm hold <dir>/dev_packs.busy while they run, so a second job on the same base is refused.
"""
import argparse
import ast
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import tempfile
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (the modules' names follow the target, not this PC: RX_TARGET=windows on Linux builds the Windows game)
WINDOWS = os.name == "nt" or os.environ.get("RX_TARGET", "").strip().lower() == "windows"
CROSS = WINDOWS and os.name != "nt"   # (the Windows game built on Linux: it cannot run here, so it prepares on the PC that plays it)
# (the game's own caches beside it, kept between runs and stamped by the translator (gpu/vk/vk_progcache.cpp): a stale one is
# not used, so a base can start from an older base's)
CACHES = ("hookshot_programs.bin", "hookshot_programs.idx", "hookshot_pipelines.bin", "hookshot_pipelines.bin.predicted",
          "hookshot_computes.bin", "hookshot_pipeline_states.bin", "hookshot_prepared.bin", "mkw_vk_pipelines.bin")
PLAY_FILES = re.compile(r"mkw_vk_pipelines_play_[0-9]+\.bin")   # (what play adds to the pipeline cache: gpu/vk/vk_draw.cpp play_path)
BUSY = "dev_packs.busy"   # (in a base's folder while a job builds, warms or copies from it)
SEED = "hookshot_shaders.bin"   # (the shader list the prepare goes through: a new one is prepared again)
# (module, its map, its link list in sa/build, the object that holds the version: nx2/build.py gives hos_main.c and vk_menu.cpp
# nx2/version.py's describe(), a fingerprint of every source, so those two objects differ in every build of another tree)
MODULES = ((("hookshot.exe", "hookshot.map", "objs.rsp", "host_hos_main.o"),
            ("hookshot_vk.dll", "hookshot_vk.map", "vk_objs.rsp", "vk_vk_menu.o")) if WINDOWS else
           (("hookshot", None, "objs.rsp", "host_hos_main.o"), ("hookshot_vk.so", None, "vk_objs.rsp", "vk_vk_menu.o")))
GAME, RENDERER = MODULES[0][0], MODULES[1][0]
STAMPED_SOURCES = ("host/hos_main.c", "gpu/vk/vk_menu.cpp")
# (nx2/build.py's shader stamp: every file in gpu/vk/{eden,sirit,compat}/ and these four; tests/test_dev_packs.py checks they agree)
STAMP_DIRS = ("eden", "sirit", "compat")
STAMP_FILES = ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h")
TRANSLATOR = tuple("gpu/vk/%s/" % d for d in STAMP_DIRS) + tuple("gpu/vk/" + f for f in STAMP_FILES)
RENDERER_ONLY = ("gpu/vk/", "third_party/imgui/", "third_party/fidelityfx")
INCLUDE = re.compile(r'#\s*include\s+"([^"]+)"')
NO_MODULE = ("docs/", "tests/", ".github/", ".claude/")
HEADERS = (".h", ".hpp", ".hh", ".inc", ".inl")
SOURCES = (".c", ".cc", ".cpp") + HEADERS
# (files the game writes beside itself while it runs, besides the caches: never a build's output, so never "deleted" by a pack)
WRITTEN_IN_PLAY = ("hookshot_options.ini", "hookshot_log.txt", "hookshot_driver_faults.txt", "hookshot_prepared.bin.warming")


def load_make_game(root=ROOT):
    spec = importlib.util.spec_from_file_location("make_game_for_packs", os.path.join(root, "nx2", "make_game.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def renderer_read_by_game(root):
    """The renderer's files the game is built from as well: what host/ includes from gpu/, what nx2/build.py adds to the game's
    dependencies from gpu/vk, and what those include in turn. A change there needs both modules"""
    todo = set()
    for top, _, fs in os.walk(os.path.join(root, "host")):
        for f in fs:
            if f.endswith((".c", ".h", ".cpp")):
                with open(os.path.join(top, f), encoding="utf-8", errors="replace") as src:
                    todo.update(os.path.normpath(os.path.join(top, inc)) for inc in INCLUDE.findall(src.read()) if "gpu/" in inc)
    with open(os.path.join(root, "nx2", "build.py"), encoding="utf-8") as src:
        todo.update(os.path.join(root, "gpu", "vk", f) for f in re.findall(r'host_deps \+= open\(os\.path\.join\(GPU, "vk", "([^"]+)"\)', src.read()))
    found = set()
    while todo:
        p = todo.pop()
        rel = os.path.relpath(p, root).replace(os.sep, "/")
        if rel in found or not os.path.isfile(p):
            continue
        found.add(rel)
        with open(p, encoding="utf-8", errors="replace") as src:
            todo.update(os.path.normpath(os.path.join(os.path.dirname(p), inc)) for inc in INCLUDE.findall(src.read()))
    return found


def route(paths, root=ROOT):
    """How a change is built: None (nothing a module or the game folder is made from), "renderer" (the renderer alone) or "game"
    (make_game.py, both modules and the game folder)"""
    paths = [p for p in paths if not (p.startswith(NO_MODULE) or ("/" not in p and p.endswith(".md")))]
    if not paths:
        return None
    if all(p.startswith(RENDERER_ONLY) for p in paths) and not set(paths) & renderer_read_by_game(root):
        return "renderer"
    return "game"


def git(tree, *args):
    return subprocess.run(["git", "-C", tree] + list(args), check=True, capture_output=True, text=True).stdout.strip()


def changed_files(tree, commit):
    """The files a checkout changed since a commit: in its commits, not committed, and new ones git does not ignore"""
    return set(git(tree, "diff", "--name-only", commit).split("\n") + git(tree, "ls-files", "--others", "--exclude-standard").split("\n")) - {""}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def same_file(a, b):
    try:
        return os.path.samefile(a, b) or (os.path.getsize(a) == os.path.getsize(b) and sha(a) == sha(b))
    except OSError:
        return False


def link_list(build, rsp):
    """The objects a module was linked from, by name (nx2/build.py's response file)"""
    with open(os.path.join(build, rsp), encoding="utf-8") as f:
        return {os.path.basename(line.strip().strip('"')): line.strip().strip('"') for line in f if line.strip()}


def module_changed(base_build, build, rsp, stamped, stamped_may_differ):
    """Whether a module differs from the base's beyond the version it names: another set of objects, an object that differs, or
    the object that holds the version when it may differ for more than that (stamped_may_differ: its source or a header
    changed). Its bytes cannot say it: the version (a fingerprint of all the sources) moves the rest of that object's code. True
    when the base kept no link list to compare with"""
    try:
        mine, theirs = link_list(build, rsp), link_list(base_build, rsp)
    except OSError:
        return True
    if set(mine) != set(theirs):
        return True
    return any(not same_file(mine[o], theirs[o]) for o in mine if o != stamped or stamped_may_differ)


def stamped_may_differ(paths, source):
    """Whether the object holding the version may differ for more than the version: its own source changed, or a header (any
    header: following every include path of every module is not worth the risk of missing one)"""
    return source in paths or any(p.endswith(HEADERS) for p in paths)


def build_scripts(root):
    """nx2/build.py and every Python module it imports (as make_game.lift_sources finds the lift's: a64-kit's folders, then nx2/),
    and nx2/make_game.py, which sets build.py's environment (the hot order, the profile)"""
    kit = os.path.join(root, "third_party", "a64-kit")
    path = [os.path.join(kit, d) for d in ("rt", "hos", "tools", "lift")] + [os.path.join(root, "nx2")]
    todo, seen = [os.path.join(root, "nx2", "build.py")], set()
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        with open(f, "rb") as src:
            tree = ast.parse(src.read(), f)
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                names = [n.name for n in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                names = [node.module]
            else:
                continue
            for name in names:
                for d in path:
                    m = os.path.join(d, name.split(".")[0] + ".py")
                    if os.path.isfile(m):
                        todo.append(m)
                        break
    return {os.path.relpath(f, root).replace(os.sep, "/") for f in seen} | {"nx2/make_game.py"}


def link_inputs_changed(paths, lift_files, build_files):
    """A change that reaches the modules besides their objects, which the link lists do not show: the build's own scripts
    (build_files: flags, link options, the Windows manifest, tools such as a64-kit's toolchain.py even when the lift reads them
    too), a data file under nx2/ that is not the translator's, or a file under host/, gpu/ or third_party/ that is not C or C++
    (libraries, resources, shader sources). The pack then keeps both modules. Other nx2/ Python (the lift's, make_game.py's)
    shows in the objects or in the game folder's files"""
    def reaches(p):
        if p in build_files:
            return True
        if p.startswith("nx2/"):
            return not p.endswith(".py") and p not in lift_files
        return p.startswith(("host/", "gpu/", "third_party/")) and not p.endswith(SOURCES) and p not in lift_files
    return any(reaches(p) for p in paths)


def build_outputs(game):
    """The files a build made in a new game folder, outside romfs: the base's, recorded when it is built, are the only files a
    pack may find gone (a played folder also holds saves, settings and other files no build makes)"""
    skip = {m for mod in MODULES for m in mod[:2] if m} | set(CACHES) | set(WRITTEN_IN_PLAY)
    out = []
    for top, dirs, fs in os.walk(game):
        dirs[:] = sorted(x for x in dirs if not (top == game and x == "romfs"))
        for f in sorted(fs):
            rel = os.path.relpath(os.path.join(top, f), game).replace(os.sep, "/")
            if rel not in skip and not rel.endswith(".log"):
                out.append(rel)
    return out


def generated_changes(base_game, game, base_outputs=None):
    """The files of a built game folder that differ from the base's, besides the modules, their maps and the caches: what the
    build generates (the shader seed, the shipped pipeline list, button prompts, files it would write into romfs; romfs files the
    same as the base's are its hard links, so only new ones are read), and the base's files outside romfs that this build did not
    make, of those its own build made (base_outputs; none when the base did not record them, as an adopted one)"""
    skip = {m for mod in MODULES for m in mod[:2] if m} | set(CACHES)
    changed = []
    for top, dirs, fs in os.walk(game):
        dirs.sort()
        for f in sorted(fs):
            p = os.path.join(top, f)
            rel = os.path.relpath(p, game).replace(os.sep, "/")
            if rel in skip:
                continue
            q = os.path.join(base_game, rel)
            if not (os.path.isfile(q) and same_file(p, q)):
                changed.append(rel)
    gone = [rel for rel in base_outputs or () if not os.path.isfile(os.path.join(game, rel))]
    return changed, gone


def clear_output(game):
    """A game folder that will hold only the next build's output: everything but romfs (hard links, rebuilt for nothing otherwise)
    goes, so a file an earlier pack's build made and this one does not is not taken for this change's"""
    for name in os.listdir(game) if os.path.isdir(game) else ():
        p = os.path.join(game, name)
        if name == "romfs":
            continue
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p)
        else:
            os.remove(p)


def run(cmd, cwd, env=None):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True, env=dict(os.environ, **(env or {})))


def base_env(predict_s):
    """The preparation's environment for a development base. predict_s > 0: it stops building the predicted pipelines (the whole
    game's, about 51,000) after that many seconds and leaves the rest to be built in play when first drawn (gpu/vk/vk_draw.cpp,
    RX_VK_PREDICT_S). For development only: a player's build prepares them all. 0 (the default) changes nothing. Given to the
    game's prepare only, never to make_game.py: its lift key hashes every RX_ variable"""
    return {"RX_VK_PREDICT_S": str(predict_s)} if predict_s > 0 else {}


def prepare(game, b):
    """The game's own preparation in a game folder, as make_game.py runs it (the game's environment, without the build's
    switches), with the base's budget; nothing on a Windows game built on Linux, which prepares at its first start"""
    if CROSS:
        print("the shaders were not prepared now (a Windows game built on Linux): the game prepares them at its first start", flush=True)
        return
    print("+ %s --prepare-shaders%s" % (GAME, " (RX_VK_PREDICT_S=%d)" % b["predict_s"] if b.get("predict_s") else ""), flush=True)
    subprocess.run([os.path.join(game, GAME), "--prepare-shaders"], cwd=game, check=True,
                   env=dict(load_make_game().game_env(), **base_env(b.get("predict_s", 0))))


def make_game(tree, b, work, game, env):
    run([sys.executable, "-I", "nx2/make_game.py", "--romfs", b["romfs"], "--exefs", b["exefs"], "--work", work, "--out", game,
         "--language", b.get("language", "en-US")], tree, dict(env, HOOKSHOT_KEEP_BUILD_FILES="1"))


def read_base(d):
    with open(os.path.join(d, "base.json"), encoding="utf-8") as f:
        return json.load(f)


def write_base(d, b):
    if not all(os.path.isfile(os.path.join(b["game"], m[0])) for m in MODULES):   # (make_game.py's launcher layout: game/bin/)
        sys.exit("%s has no %s and %s beside each other: a base is a plain game folder (not a launcher's, with bin/)" %
                 (b["game"], GAME, RENDERER))
    b["modules"] = {m[0]: sha(os.path.join(b["game"], m[0])) for m in MODULES}
    with open(os.path.join(d, "base.json"), "w", encoding="utf-8") as f:
        json.dump(b, f, indent=1)


def pack_folders(d):
    """Where a base's packs are built: one work folder for the game and one for the renderer, the same for every pack. The lift key
    (make_game.lift_key) hashes the work and game folders' paths with the lift's environment, so a translation is reused only in
    the folder, and for the game folder, it was made for: a game pack after the first on a base skips the lift while the translator,
    the HLE list and the hot list are the base's. One pack builds at a time"""
    return {"game_work": os.path.join(d, "game-work"), "game_out": os.path.join(d, "game-out"),
            "renderer_work": os.path.join(d, "renderer-work")}


def overlaps(a, b):
    """One folder is the other or holds it, through links and junctions too (folders on two drives never do)"""
    a, b = (os.path.normcase(os.path.realpath(x)) for x in (a, b))
    try:
        return os.path.commonpath([a, b]) in (a, b)
    except ValueError:
        return False


def check_folders(d, b):
    """A base whose work or game folder is, holds or lies in one of its packs' folders would be rebuilt or deleted by its own
    packs, and compared with itself"""
    for what in ("work", "game"):
        for f in list(pack_folders(d).values()) + [os.path.join(d, "packs")]:
            if overlaps(b[what], f):
                sys.exit("the base's %s folder %s overlaps %s, where its packs are built: give the base other folders" %
                         (what, b[what], f))


def game_language(game):
    """The language a game folder was built in (its language.txt), so a pack is built in the base's"""
    try:
        with open(os.path.join(game, "language.txt"), encoding="utf-8") as f:
            return next((line.strip() for line in f if line.strip() and not line.startswith("#")), None)
    except OSError:
        return None


def link_or_copy(src, dst):
    """A hard link, or a copy where the two folders are on different drives (as make_game.py falls back)"""
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def link_objects(base_work, work):
    """A new work folder with the base's compiled objects (hard links: the object cache's keys hold no folder path) and the note of
    the compiler's headers they were made with, so only what a change touches compiles"""
    if os.path.isdir(work):
        return
    src = os.path.join(base_work, "objcache")
    for top, _, fs in os.walk(src):
        dst = os.path.join(work, "objcache", os.path.relpath(top, src))
        os.makedirs(dst, exist_ok=True)
        for f in fs:
            link_or_copy(os.path.join(top, f), os.path.join(dst, f))
    os.makedirs(work, exist_ok=True)
    if os.path.isfile(os.path.join(base_work, "system_headers.txt")):
        shutil.copy2(os.path.join(base_work, "system_headers.txt"), os.path.join(work, "system_headers.txt"))


def cmd_base(a):
    tree, d = os.path.abspath(a.tree), os.path.abspath(a.dir)
    work, game = os.path.join(d, "work"), os.path.join(d, "game")
    if os.path.isdir(game) and os.listdir(game):   # (its files are recorded as what the build made: a played folder holds saves)
        sys.exit("%s is not empty: a base is built into a new game folder" % game)
    dirty = [p for p in changed_files(tree, "HEAD") if route([p], tree)]
    if dirty:
        sys.exit("%s has uncommitted or untracked sources (%s): a base is a commit" % (tree, ", ".join(sorted(dirty)[:5])))
    os.makedirs(game, exist_ok=True)
    if a.seed:
        for c in CACHES:
            if os.path.isfile(os.path.join(a.seed, c)) and not os.path.exists(os.path.join(game, c)):
                shutil.copy2(os.path.join(a.seed, c), os.path.join(game, c))
    b = {"commit": git(tree, "rev-parse", "HEAD"), "work": work, "game": game, "romfs": os.path.abspath(a.romfs),
         "exefs": os.path.abspath(a.exefs), "seeded_from": a.seed, "language": a.language, "predict_s": a.predict_s}
    t = time.time()
    # (a budget: make_game.py builds without preparing, then the preparation runs here with it, so it never reaches the lift key)
    make_game(tree, b, work, game, {"HOOKSHOT_NO_PREPARE": "1"} if a.predict_s > 0 else {})
    if a.predict_s > 0:
        prepare(game, b)
    b["build_s"] = round(time.time() - t)
    b["outputs"] = build_outputs(game)
    for m in MODULES:   # (the maps beside the modules, for profilers)
        if m[1]:
            shutil.copy2(os.path.join(work, "sa", "build", m[1]), os.path.join(game, m[1]))
    write_base(d, b)
    print("base %s in %s: %d min" % (b["commit"][:8], d, b["build_s"] // 60))


def cmd_adopt(a):
    d = os.path.abspath(a.dir)
    check_folders(d, {"work": a.work, "game": a.game})
    os.makedirs(d, exist_ok=True)
    write_base(d, {"commit": git(os.path.abspath(a.tree), "rev-parse", "HEAD"), "work": os.path.abspath(a.work),
                   "game": os.path.abspath(a.game), "romfs": os.path.abspath(a.romfs), "exefs": os.path.abspath(a.exefs),
                   "language": game_language(a.game) or "en-US"})


def cmd_pack(a):
    d, tree = os.path.abspath(a.dir), os.path.abspath(a.tree)
    b = read_base(d)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", a.name):   # (a plain name: packs/<name> is deleted and made again)
        sys.exit("a pack's name is letters, digits, '.', '_' and '-': %r" % a.name)
    check_folders(d, b)
    if subprocess.run(["git", "-C", tree, "merge-base", "--is-ancestor", b["commit"], "HEAD"]).returncode:
        sys.exit("%s is not built on the base %s: rebase it on the base, or make a new base" % (tree, b["commit"][:8]))
    changed = changed_files(tree, b["commit"])
    how = route(changed, tree)
    pack = os.path.join(d, "packs", a.name)
    if os.path.isdir(pack):   # (what an earlier build of this pack brought goes: this one says again what differs)
        shutil.rmtree(pack)
    os.makedirs(pack)
    print("%d files changed since the base: %s" % (len(changed), {None: "nothing to build", "renderer": "the renderer alone",
                                                                  "game": "the game (both modules)"}[how]), flush=True)
    t, w, files, extra, gone = time.time(), pack_folders(d), {}, [], []
    lift_files = {os.path.relpath(f, tree).replace(os.sep, "/") for f in load_make_game(tree).lift_sources()}
    whole = link_inputs_changed(changed, lift_files, build_scripts(tree))
    if how == "game":
        link_objects(b["work"], w["game_work"])
        clear_output(w["game_out"])
        make_game(tree, b, w["game_work"], w["game_out"], {"HOOKSHOT_NO_PREPARE": "1"})
        built, kinds = os.path.join(w["game_work"], "sa", "build"), MODULES
    elif how == "renderer":
        link_objects(b["work"], w["renderer_work"])
        run([sys.executable, "-I", "nx2/build.py", "--standalone"], tree,
            {"OOT_WORK": w["renderer_work"], "RX_RELEASE": "1", "RX_VK_ONLY": "1", "HOOKSHOT_KEEP_BUILD_FILES": "1"})
        built, kinds = os.path.join(w["renderer_work"], "sa", "build"), MODULES[1:]
    else:
        built, kinds = None, ()
    for (mod, mp, rsp, stamped), source in zip(kinds, STAMPED_SOURCES[-len(kinds):] if kinds else ()):
        if whole or module_changed(os.path.join(b["work"], "sa", "build"), built, rsp, stamped,
                                   stamped_may_differ(changed, source)):
            for f in (mod, mp):
                if f:
                    shutil.copy2(os.path.join(built, f), os.path.join(pack, f))
            files[mod] = sha(os.path.join(pack, mod))
    if how == "game":
        extra, gone = generated_changes(b["game"], w["game_out"], b.get("outputs"))
        for rel in extra:
            dst = os.path.join(pack, "files", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(w["game_out"], rel), dst)
    prepare = any(p.startswith(TRANSLATOR) for p in changed) or SEED in extra
    if a.prepare is not None:
        prepare = a.prepare
    with open(os.path.join(pack, "pack.json"), "w", encoding="utf-8") as f:
        json.dump({"base": b["commit"], "tree": tree, "head": git(tree, "rev-parse", "HEAD"), "changed": sorted(changed),
                   "route": how, "files": files, "generated": extra, "deleted": gone, "prepare": prepare,
                   "build_s": round(time.time() - t)}, f, indent=1)
    print("pack %s: %s%s%s%s, in %d s" % (a.name, ", ".join(files) or "no module differs from the base's",
                                          "; %d generated files" % len(extra) if extra else "",
                                          "; %d generated files gone" % len(gone) if gone else "", "; prepares" if prepare else "",
                                          time.time() - t))


def in_runtime(rt, rel):
    """A path a pack names (a module, a generated or deleted file) is relative and names a file below the runtime: pack.json is
    a plain file, and one that named an absolute path or climbed out with .. would have rt write or remove a file anywhere. Judged
    by its text alone, before the runtime exists: no .. anywhere (so no name comparison is needed, which a case-sensitive
    folder could fool); the links the runtime itself holds are checked where each file is written (resolves_inside)"""
    # (rooted counts too: "/x" is not isabs on Windows since Python 3.13, and join() would then put it at the drive's root)
    if not rel or os.path.isabs(rel) or os.path.splitdrive(rel)[0] or rel[0] in "/\\":
        return False
    parts = rel.replace("\\", "/").split("/")
    # (no ':' either: on NTFS romfs/foo:s names a stream of foo, and foo is a hard link shared with the base; the files a pack
    # names never hold one)
    return ".." not in parts and ":" not in rel and os.path.normpath(rel) not in (".", "")


def resolves_inside(rt, path):
    """path (which may not exist yet) lies inside the runtime once links are followed: its nearest existing folder, resolved,
    is the runtime or below it, compared by file identity rather than by name. A link in the base's romfs is cloned into the
    runtime, so a path that reads as inside can still lead out of it"""
    p = os.path.realpath(path)
    while not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            return False
        p = up
    root = os.path.realpath(rt)
    while True:
        if os.path.samefile(p, root):
            return True
        up = os.path.dirname(p)
        if up == p:
            return False
        p = up


def cmd_rt(a):
    d, rt = os.path.abspath(a.dir), os.path.abspath(a.runtime)
    b = read_base(d)
    if os.path.exists(rt):
        sys.exit("%s exists: delete it first" % rt)
    for f in [b["game"], b["work"], os.path.join(d, "packs")] + list(pack_folders(d).values()):
        if overlaps(rt, f):
            sys.exit("%s overlaps %s: a runtime goes in a folder of its own" % (rt, f))
    over, needs_prepare = {}, False   # (a file in the runtime -> the pack and the path it comes from, or None: the pack removes it)
    for p in a.packs:
        pdir = os.path.join(d, "packs", p)
        with open(os.path.join(pdir, "pack.json"), encoding="utf-8") as f:
            meta = json.load(f)
        if meta["base"] != b["commit"]:
            sys.exit("pack %s was built on %s, not the base %s: build it again" % (p, meta["base"][:8], b["commit"][:8]))
        mine = {m: os.path.join(pdir, m) for m in meta["files"]}
        mine.update({m[1]: os.path.join(pdir, m[1]) for m in MODULES
                     if m[0] in meta["files"] and m[1] and os.path.isfile(os.path.join(pdir, m[1]))})
        mine.update({rel: os.path.join(pdir, "files", rel) for rel in meta["generated"]})
        mine.update({rel: None for rel in meta.get("deleted", ())})
        if not set(meta["files"]) <= {m[0] for m in MODULES}:
            sys.exit("pack %s names modules that are not %s: %s" % (p, " or ".join(m[0] for m in MODULES), sorted(meta["files"])))
        bad = sorted(rel for rel in mine if not in_runtime(rt, rel))
        if bad:
            sys.exit("pack %s names paths outside the runtime: %s" % (p, ", ".join(map(repr, bad[:5]))))
        for rel, src in mine.items():
            if rel in over:
                sys.exit("packs %s and %s both bring %s: merge their branches and build one pack" % (over[rel][0], p, rel))
            over[rel] = (p, src)
        needs_prepare |= meta["prepare"]
    os.makedirs(rt)
    for top, _, fs in os.walk(os.path.join(b["game"], "romfs")):
        dst = os.path.join(rt, os.path.relpath(top, b["game"]))
        os.makedirs(dst, exist_ok=True)
        for f in fs:
            link_or_copy(os.path.join(top, f), os.path.join(dst, f))
    for name in os.listdir(b["game"]):
        s = os.path.join(b["game"], name)
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(rt, name))
        elif os.path.isdir(s) and name != "romfs":
            shutil.copytree(s, os.path.join(rt, name))
    for rel, (p, src) in over.items():
        dst = os.path.join(rt, rel)
        if not resolves_inside(rt, os.path.dirname(dst)):
            sys.exit("pack %s: %s leads outside the runtime through a link in it; nothing of the pack was written past this "
                     "file: delete %s" % (p, rel, rt))
        if os.path.lexists(dst):
            os.remove(dst)   # (a romfs hard link, or a link: replaced, never written through to the base or the link's target)
        if src:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
    if needs_prepare:
        prepare(rt, b)
    with open(os.path.join(rt, "dev_packs.json"), "w", encoding="utf-8") as f:
        json.dump({"base": b["commit"], "packs": a.packs, "modules": {m[0]: sha(os.path.join(rt, m[0])) for m in MODULES}}, f, indent=1)
    print("%s: the base %s%s" % (rt, b["commit"][:8], "".join(", %s from %s" % (m, over[m][0]) for m in (GAME, RENDERER) if m in over)))


def is_cache(name):
    return name in CACHES or bool(PLAY_FILES.fullmatch(name))


def cmd_warm(a):
    """A runtime with no pack, played once by each --run command ({runtime} in it is the runtime's folder; split as a POSIX
    shell would), then the caches play made or grew copied into the base's game folder, and the runtime deleted: every
    later runtime starts warm for those scenes. Only cache files are copied, through a temporary file moved into place"""
    d = os.path.abspath(a.dir)
    b = read_base(d)
    rt = os.path.join(d, "warm-runtime")
    if os.path.exists(rt):   # (made beforehand by `rt <dir> <dir>/warm-runtime`, so the copy can run under a lock of its own)
        try:
            with open(os.path.join(rt, "dev_packs.json"), encoding="utf-8") as f:
                made = json.load(f)
        except (OSError, ValueError):
            made = {}
        if made.get("base") != b["commit"] or made.get("packs"):
            sys.exit("%s exists and is not this base's runtime with no pack (an earlier warm-up stopped?): look at it, delete "
                     "it and run again" % rt)
        print("warming the runtime already made in %s" % rt, flush=True)
    else:
        cmd_rt(argparse.Namespace(dir=d, runtime=rt, packs=[]))
    for cmd in a.run:
        argv = [x.replace("{runtime}", rt) for x in shlex.split(cmd)]
        print("+ " + " ".join(argv), flush=True)
        subprocess.run(argv, check=True)
    copied = []
    for name in sorted(os.listdir(rt)):
        src, dst = os.path.join(rt, name), os.path.join(b["game"], name)
        # (a cache the game wrote: a plain file in the runtime, never a link, whose bytes go into a new temporary file of the
        # base's own and then over the old one, so neither a link in the runtime nor one left in the base is followed)
        if os.path.islink(src) or not os.path.isfile(src) or not is_cache(name) or os.path.islink(dst):
            continue
        if os.path.isfile(dst) and same_file(src, dst):
            continue
        fd, tmp = tempfile.mkstemp(prefix=name + ".", suffix=".tmp", dir=b["game"])
        with os.fdopen(fd, "wb") as out, open(src, "rb") as f:
            shutil.copyfileobj(f, out, 1 << 22)
        shutil.copystat(src, tmp)
        os.replace(tmp, dst)
        copied.append(name)
    b["warmed"] = {"at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "runs": a.run, "files": copied}
    write_base(d, b)
    shutil.rmtree(rt)
    print("the base's caches warmed by %d runs: %s" % (len(a.run), ", ".join(copied) or "nothing grew"))


class Busy:
    """dev_packs.busy in a base's folder while a job builds, warms or copies from it: a second job on the same base is refused
    (game packs share one work folder; a warm-up writes the base's caches). A marker left by a stopped job is deleted by hand"""
    def __init__(self, d, what):
        self.path, self.what = os.path.join(d, BUSY), what

    def __enter__(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        try:
            fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                with open(self.path, encoding="utf-8") as f:
                    held = f.read().strip()
            except OSError:
                held = "?"
            sys.exit("the base is busy (%s): wait for that job, or delete %s if it is no longer running" % (held, self.path))
        self.token = "%d %s %s" % (os.getpid(), self.what, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(self.token)
        return self

    def __exit__(self, *exc):
        try:
            with open(self.path, encoding="utf-8") as f:
                mine = f.read().strip() == self.token
            if mine:
                os.remove(self.path)
        except OSError:
            pass


def cmd_ls(a):
    d = os.path.abspath(a.dir)
    b = read_base(d)
    print("base %s: %s%s%s" % (b["commit"][:8], b["game"], "; predicted pipelines for %d s" % b["predict_s"] if b.get("predict_s") else "",
                             "; warmed %s" % b["warmed"]["at"] if b.get("warmed") else ""))
    packs = os.path.join(d, "packs")
    for p in sorted(os.listdir(packs)) if os.path.isdir(packs) else ():
        try:
            with open(os.path.join(packs, p, "pack.json"), encoding="utf-8") as f:
                meta = json.load(f)
        except OSError:
            continue
        print("  pack %s (%s, %s): %s" % (p, meta["head"][:8], "on the base" if meta["base"] == b["commit"] else "on another base",
                                          ", ".join(list(meta["files"]) + meta.get("generated", [])) or "-"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("base", "adopt"):
        s = sub.add_parser(name)
        s.add_argument("dir")
        s.add_argument("--romfs", required=True)
        s.add_argument("--exefs", required=True)
        s.add_argument("--tree", default=ROOT)
        if name == "base":
            s.add_argument("--seed")
            s.add_argument("--language", default="en-US")
            s.add_argument("--predict-s", type=int, default=0,
                           help="development only: stop building predicted pipelines after this many seconds (0: build all)")
        else:
            s.add_argument("--work", required=True)
            s.add_argument("--game", required=True)
    s = sub.add_parser("pack")
    s.add_argument("dir")
    s.add_argument("name")
    s.add_argument("--tree", default=ROOT)
    s.add_argument("--prepare", action="store_true", default=None)
    s.add_argument("--no-prepare", dest="prepare", action="store_false")
    s = sub.add_parser("rt")
    s.add_argument("dir")
    s.add_argument("runtime")
    s.add_argument("packs", nargs="*")
    s = sub.add_parser("warm")
    s.add_argument("dir")
    s.add_argument("--run", action="append", required=True, help="a command that plays the runtime once; {runtime} is its folder")
    s = sub.add_parser("ls")
    s.add_argument("dir")
    a = ap.parse_args(argv)
    job = {"base": cmd_base, "adopt": cmd_adopt, "pack": cmd_pack, "rt": cmd_rt, "warm": cmd_warm, "ls": cmd_ls}[a.cmd]
    if a.cmd in ("base", "pack", "rt", "warm"):
        with Busy(os.path.abspath(a.dir), a.cmd):
            job(a)
    else:
        job(a)


if __name__ == "__main__":
    main()
