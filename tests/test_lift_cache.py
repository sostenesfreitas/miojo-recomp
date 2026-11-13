"""The lift is skipped only when the game, the tools and the settings are the ones it was made from.

python -I -B tests/test_lift_cache.py
"""
import importlib.util
import os
import tempfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_game_under_test", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def main():
    with tempfile.TemporaryDirectory(prefix="hookshot-lift-key-") as d:
        d = Path(d)
        exefs, work = d / "exefs", d / "work"
        (exefs).mkdir()
        (work / "image").mkdir(parents=True)
        (exefs / "main").write_bytes(b"NSO0 one")
        (work / "hot_funcs.txt").write_text("f_80000000\n")
        (work / "image" / "chunks.txt").write_text("80001000\n")
        env = {"A64_HOT_CHUNK": "1", "OOT_WORK": str(work)}
        with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}, clear=True):
            key = make_game.lift_key(str(exefs), str(work), env)
            check("the same inputs give the same key", key == make_game.lift_key(str(exefs), str(work), dict(env)))
            (exefs / "main").write_bytes(b"NSO0 two")
            check("another game module changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (exefs / "main").write_bytes(b"NSO0 one")
            (work / "hot_funcs.txt").write_text("f_80000004\n")
            check("another hot list changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (work / "hot_funcs.txt").write_text("f_80000000\n")
            (work / "image" / "chunks.txt").write_text("80002000\n")
            check("other chunk boundaries change the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (work / "image" / "chunks.txt").write_text("80001000\n")
            (work / "image" / "hot_pieces.txt").write_text("f_80000000 1\n")   # (split_hot's kept pieces: the pieces' C follows them)
            check("other kept hot pieces change the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (work / "image" / "hot_pieces.txt").unlink()
            check("another lift setting changes the key", make_game.lift_key(str(exefs), str(work), dict(env, A64_HOT_CHUNK="")) != key)
            with mock.patch.dict(os.environ, {"A64_KEEP_FLAGS": "1"}):
                check("a lifter switch in the environment changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            for name in make_game.COMPILE_ONLY_ENV:   # (the compile's switches: a build at another hot-code level keeps the lift)
                check("%s for the compile does not change the key" % name,
                      make_game.lift_key(str(exefs), str(work), dict(env, **{name: "O1"})) == key)
                with mock.patch.dict(os.environ, {name: "O1"}):
                    check("%s in the environment does not change the key" % name, make_game.lift_key(str(exefs), str(work), env) == key)
            real_open = open
            changed = str(ROOT / "nx2" / "lift.py")

            def edited(path, *a, **k):   # (the translator as if one of its files had changed)
                f = real_open(path, *a, **k)
                if os.path.abspath(path) == changed:
                    f.close()
                    import io
                    return io.BytesIO(b"# an edit\n")
                return f
            with mock.patch("builtins.open", edited):
                check("a change to the translator changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            for name in ("nx2/whole_write.py", "nx2/oot_patches.py",
                         "third_party/a64-kit/lift/a64_lift.py", "third_party/a64-kit/lift/a64emit.cpp"):
                changed = str(ROOT / name)   # (imported by an import, in a function, or compiled when the lift starts)
                with mock.patch("builtins.open", edited):
                    check("a change to %s changes the key" % name, make_game.lift_key(str(exefs), str(work), env) != key)
            for name in ("nx2/build.py", "nx2/make_game.py", "nx2/pipeline_states.py", "nx2/prompt_assets.py", "nx2/shader_census.py"):
                changed = str(ROOT / name)   # (the builder's other steps: not the lift's)
                with mock.patch("builtins.open", edited):
                    check("a change to %s leaves the key" % name, make_game.lift_key(str(exefs), str(work), env) == key)
            check("the inputs back as they were give the key again", make_game.lift_key(str(exefs), str(work), env) == key)
            import standalone
            names = standalone.implemented()
            with mock.patch.object(standalone, "implemented", lambda host=None: names | {"imp_new_hle"}):
                check("a new HLE function in host/ changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            host_c = str(ROOT / "host" / "hos_main.c")

            def host_edited(path, *a, **k):   # (host/ code changed, its HLE functions the same)
                if os.path.abspath(path) == host_c:
                    import io
                    return io.StringIO("/* an edit */\n")
                return real_open(path, *a, **k)
            with mock.patch("builtins.open", host_edited):
                check("other host/ code does not change the key", make_game.lift_key(str(exefs), str(work), env) == key)

        gen, image = work / "sa" / "gen", work / "sa" / "image"
        check("no lift yet: not lifted before", not make_game.lifted_before(str(work), key))
        gen.mkdir(parents=True)
        image.mkdir(parents=True)
        make_game.write_lift_key(str(work), key)
        check("a key without the lift's files is not trusted", not make_game.lifted_before(str(work), key))
        (gen / "a64_table.c").write_text("")
        (image / "image.bin").write_bytes(b"")
        check("a key written before the lift's files is not trusted", not make_game.lifted_before(str(work), key))
        make_game.write_lift_key(str(work), key)
        check("the finished lift with this key is reused", make_game.lifted_before(str(work), key))
        (gen / "code_000.c").write_text("/* another builder's lift */")
        check("C changed by another builder's lift is not", not make_game.lifted_before(str(work), key))
        (gen / "code_000.c").unlink()
        check("a key in the old one-line form is not", (gen / make_game.LIFT_KEY).write_text(key + "\n") and
              not make_game.lifted_before(str(work), key))
        make_game.write_lift_key(str(work), key)
        check("a finished lift with another key is not", not make_game.lifted_before(str(work), "0" * 40))
    check_lift_sources()
    check_compile_only()
    check_key_after_lift()
    print("ok: the lift key follows the game, the tools (the modules the lift imports), the hot list, the chunks and the settings")


def check_key_after_lift():
    """A fresh work folder has no image/hot_pieces.txt until its first lift's split_hot writes one, and the key hashes it: the key
    the lift is stored under is taken after the lift, from what the next build reads, or that build translates the game again
    (a no-op rebuild of a new work folder: 319 s on Windows, 290 s of it the lift). make_game.py's translate(), read as text"""
    src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
    body = src[src.index("    def translate():"):src.index("\n    translate()\n")]
    split = body.index('"nx2/split_hot.py"')
    again = body.find("after = lift_key(exefs, work, env)", split)
    write = body.find("write_lift_key(work, key)", split)
    check("the key is taken again after the lift and split_hot, then written", split < again < write)
    check("translate() sets the build's key, which keep_translation uses after the build", "nonlocal key" in body)
    start = body.find("inputs = lift_key(exefs, work, env, planning=False)")
    lift = body.index('"nx2/lift.py"')
    guard = body.find("if lift_key(exefs, work, env, planning=False) == inputs:", split)
    taken = body.find("key = after", guard)
    check("what the lift reads and does not write is taken as it starts", 0 <= start < lift)
    check("the key taken after the lift is kept only if that did not change while it ran", split < guard < taken < write)
    check("the key is taken before that comparison, so an edit between the two is caught by it", again < guard)
    with tempfile.TemporaryDirectory(prefix="hookshot-lift-key-after-") as d:
        d = Path(d)
        exefs, work = d / "exefs", d / "work"
        exefs.mkdir()
        (work / "image").mkdir(parents=True)
        (exefs / "main").write_bytes(b"NSO0 one")
        env = {"A64_HOT_CHUNK": "1"}
        with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}, clear=True):
            before = make_game.lift_key(str(exefs), str(work), env)
            (work / "image" / "hot_pieces.txt").write_text("f_80000000 1\n")   # (what the first lift's split_hot leaves)
            after = make_game.lift_key(str(exefs), str(work), env)
            check("a fresh folder's key before its first lift is not the next build's", before != after)
            check("the key after the lift is the next build's", after == make_game.lift_key(str(exefs), str(work), dict(env)))
            inputs = make_game.lift_key(str(exefs), str(work), env, planning=False)
            (work / "image" / "hot_pieces.txt").write_text("f_80000004 1\n")
            (work / "image" / "chunks.txt").write_text("80003000\n")
            check("the lift's own files are not among the other inputs", make_game.lift_key(str(exefs), str(work), env, planning=False)
                  == inputs)
            (exefs / "main").write_bytes(b"NSO0 two")
            check("the game's code is", make_game.lift_key(str(exefs), str(work), env, planning=False) != inputs)
            (exefs / "main").write_bytes(b"NSO0 one")
            (work / "hot_funcs.txt").write_text("f_80000004\n")
            check("the hot list is", make_game.lift_key(str(exefs), str(work), env, planning=False) != inputs)


def check_lift_sources():
    """Every module of nx2/ and a64-kit that loading the lift's two scripts really imports is one lift_sources() hashes (its
    import walk reads the source; this imports them, capstone and lz4 stood in where they are not installed)"""
    import subprocess
    import sys
    code = ("import importlib, os, sys, types\n"
            "class Stub(types.ModuleType):\n    def __getattr__(self, n):\n"
            "        if n.startswith('__'):\n            raise AttributeError(n)\n        return 0\n"
            "for name in ('capstone', 'lz4', 'lz4.block'):\n"
            "    try:\n        importlib.import_module(name)\n    except ImportError:\n"
            "        sys.modules[name] = Stub(name)\n"
            "sys.path.insert(0, sys.argv[1])\nsys.argv[1:] = []\n"
            "import lift, split_hot\n"
            "print('\\n'.join(os.path.abspath(m.__file__) for m in list(sys.modules.values()) if getattr(m, '__file__', None)))\n")
    run = subprocess.run([sys.executable, "-I", "-B", "-c", code, str(ROOT / "nx2")], capture_output=True, text=True)
    check("the lift's scripts import (%s)" % run.stderr.strip()[-2000:], run.returncode == 0)
    out = run.stdout
    tops = (str(ROOT / "nx2") + os.sep, str(ROOT / "third_party" / "a64-kit") + os.sep)
    loaded = {f for f in out.splitlines() if f.startswith(tops)}   # (lines: a path may hold a space)
    check("the lift loads its modules (%d)" % len(loaded), len(loaded) > 12 and str(ROOT / "nx2" / "chunks.py") in loaded)
    sources = {os.path.abspath(f) for f in make_game.lift_sources()}
    missing = sorted(loaded - sources)
    check("every module the lift loads is in its key (missing: %s)" % missing, not missing)
    check("the lift's later imports too", {str(ROOT / "nx2" / f) for f in ("oot_patches.py", "oot_wide.py")}
          <= sources)
    check("the builder's other modules are not", str(ROOT / "nx2" / "build.py") not in sources)
    import re
    dynamic = [f for f in sorted(sources) if f.endswith(".py") and re.search(r"\b__import__\(|\bimport_module\(|\bimportlib\b",
                                                                            open(f, encoding="utf-8", errors="replace").read())]
    check("no module of the lift imports by a name made at run time, which lift_sources' walk cannot see (%s)" % dynamic, not dynamic)



def check_compile_only():
    """The variables the key leaves out are the compile's alone: no file of the translator (lift_sources(), what the key hashes)
    names them, so a translator that starts reading one fails here instead of reusing a lift made without it"""
    for f in make_game.lift_sources():
        text = open(f, encoding="utf-8", errors="replace").read()
        for name in make_game.COMPILE_ONLY_ENV:
            check("%s reads %s, which the lift key leaves out as the compile's alone" % (os.path.relpath(f, ROOT), name), name not in text)


if __name__ == "__main__":
    main()
