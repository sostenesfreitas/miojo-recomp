"""Game-free prompt-cache proofs, using authored files and only replacing the expensive drawing.

python -I -B tests/test_prompt_skip.py
HOOKSHOT_PROMPT_TEST_SOURCE=/path/to/main/nx2 python -I -B tests/test_prompt_skip.py
The unchanged-build case also runs against old make_game.py: it fails because the drawing runs twice.
"""
import ast
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("HOOKSHOT_PROMPT_TEST_SOURCE", ROOT / "nx2"))
sys.path.insert(0, str(SOURCE))
spec = importlib.util.spec_from_file_location("prompt_assets", SOURCE / "prompt_assets.py")
P = importlib.util.module_from_spec(spec)
spec.loader.exec_module(P)
spec = importlib.util.spec_from_file_location("prompt_make_game", SOURCE / "make_game.py")
M = importlib.util.module_from_spec(spec)
spec.loader.exec_module(M)

# The actual finish-stage call site, including its no-fontTools fallback and packaged/source decision.
main = next(n for n in ast.parse((SOURCE / "make_game.py").read_text(encoding="utf-8")).body
            if isinstance(n, ast.FunctionDef) and n.name == "main")
start = next(i for i, n in enumerate(main.body) if isinstance(n, ast.Assign) and
             any(isinstance(t, ast.Name) and t.id == "prompts" for t in n.targets))
end = next(i for i in range(start + 1, len(main.body)) if isinstance(main.body[i], ast.Assign) and
           any(isinstance(t, ast.Name) and t.id == "icon" for t in main.body[i].targets))
FINISH = compile(ast.Module(body=main.body[start:end], type_ignores=[]), str(SOURCE / "make_game.py"), "exec")


class PromptSkipTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="prompt skip ")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.here = self.root / "nx2"
        self.romfs = self.root / "romfs"
        self.out = self.root / "game" / "prompts"
        self.out.parent.mkdir()
        self.source_files = [self.here / n for n in ("prompt_assets.py", "prompt_draw.py", "prompt_faces.py", "prompt_keys.py")]
        self.game_files = [self.romfs / n for n in (P.FONT, P.prompt_keys.LABEL_FONT, "Pack/ZsDic.pack.zs", P.TEXTURES, P.LAYOUTS,
                                                  "System/Resource/ResourceSizeTable.authored.rsizetable.zs")]
        self.kenney = self.root / "kenney_input"
        self.keyboard = self.root / "kenney_input_keyboard"
        self.font_files = [self.kenney / f["font"] for f in P.FACES.values()] + [self.keyboard / P.prompt_keys.FONT]
        for path in self.source_files + self.game_files + self.font_files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"authored input: " + path.name.encode())
        self.args = types.SimpleNamespace(romfs=str(self.romfs), out=str(self.out), faces=",".join(P.ALL_FACES),
                                          no_textures=False, dump=None)
        self.calls = 0
        self.during = None
        self.packages = {name: ["authored-version", True] for name in getattr(P, "PROMPT_PACKAGES", {"PIL": "pillow"})}
        self.log = io.StringIO()
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.dict(os.environ, {}, clear=True))
        self.stack.enter_context(contextlib.redirect_stdout(self.log))
        self.stack.enter_context(mock.patch.multiple(P, HERE=str(self.here), KENNEY=str(self.kenney), KENNEY_KEYBOARD=str(self.keyboard)))
        self.stack.enter_context(mock.patch.object(M.importlib.util, "find_spec", return_value=object()))
        if hasattr(P, "make_prompts"):
            self.stack.enter_context(mock.patch.object(P, "generate", side_effect=self.draw))
            self.stack.enter_context(mock.patch.object(P, "prompt_packages", side_effect=lambda: self.packages.copy()))
        self.stack.enter_context(mock.patch.object(M, "run", side_effect=self.child))

    def draw(self, a):
        self.calls += 1
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        (out / "External.bfotf").write_bytes(b"authored complete font")
        for face in a.faces.split(","):
            (out / face).mkdir()
            (out / face / "hint.bin").write_bytes(b"authored hint")
            if not a.no_textures and face != "keyboard":
                (out / face / "textures.bin").write_bytes(b"authored texture")
        for name in ("Common.blarc", "ring.bin", "rsizetable.zs") + (() if a.no_textures else ("Common.bntx",)):
            (out / name).write_bytes(b"authored " + name.encode())
        if self.during:
            self.during(out)

    def child(self, command, **kwargs):
        a = types.SimpleNamespace(**vars(self.args))
        a.out = command[command.index("--out") + 1]
        try:
            if hasattr(P, "make_prompts"):
                P.make_prompts(a, "--incremental" in command)
            else:
                self.draw(a)
        except (OSError, SystemExit):
            return 1   # a failed child, not an interruption of the builder itself
        return 0

    def build(self, packaged=False):
        env = dict(vars(M), romfs=str(self.romfs), out=str(self.out.parent), launcher="packaged launcher" if packaged else None)
        exec(FINISH, env)

    def stamp(self):
        return self.out / getattr(P, "PROMPT_KEY", ".prompt_key")

    def test_unchanged_skips_actual_finish_call(self):
        self.build()
        self.build()
        self.assertEqual(self.calls, 1, "unchanged finish must not draw the prompts twice")
        self.assertIn("prompts: unchanged, kept", self.log.getvalue())

    def test_missing_stamp_is_a_normal_first_build(self):
        self.assertFalse(self.out.exists())
        self.build()
        self.assertIn("prompts: regenerated (no stamp yet)", self.log.getvalue())
        self.stamp().unlink()
        self.log.seek(0)
        self.log.truncate()
        self.build()
        self.assertIn("prompts: regenerated (no stamp yet)", self.log.getvalue())

    def test_unreadable_stamp_keeps_its_reason(self):
        self.build()
        self.log.seek(0)
        self.log.truncate()
        open_file = open
        def denied(path, *args, **kwargs):
            if Path(path) == self.stamp():
                raise PermissionError("authored unreadable stamp")
            return open_file(path, *args, **kwargs)
        with mock.patch("builtins.open", side_effect=denied):
            self.build()
        self.assertIn("prompts: regenerated (stamp or outputs unreadable)", self.log.getvalue())
        self.assertNotIn("no stamp yet", self.log.getvalue())

    def staging(self, name, marked=False, old=False):
        folder = self.out.parent / name
        (folder / "new").mkdir(parents=True)
        (folder / "new" / "authored.bin").write_bytes(b"unfinished generation")
        if marked:
            (folder / ".owner").write_bytes(b"1")   # an initialized owner whose lock was released
        if old:
            age = time.time() - 2 * 24 * 60 * 60
            os.utime(folder, (age, age))
        return folder

    def test_stale_staging_is_removed_and_build_proceeds(self):
        stale = self.staging(".prompts-abandoned", marked=True, old=os.name == "nt")
        self.build()
        self.assertFalse(stale.exists())   # immediate on POSIX; Windows requires an old directory
        self.assertTrue(self.stamp().exists())
        self.assertEqual(self.calls, 1)

    def test_legacy_staging_waits_a_day(self):
        old = self.staging(".prompts-legacy-old", old=True)
        recent = self.staging(".prompts-legacy-recent")
        self.build()
        self.assertFalse(old.exists())
        self.assertTrue(recent.exists())
        self.assertTrue(self.stamp().exists())

    @unittest.skipUnless(os.name == "posix", "owner-based reclamation is POSIX-only")
    def test_live_owner_is_kept_even_when_old(self):
        live = self.staging(".prompts-live")
        with P.prompt_owner(str(live), create=True):
            age = time.time() - 2 * 24 * 60 * 60
            os.utime(live, (age, age))
            self.build()
            self.assertTrue((live / "new" / "authored.bin").exists())
        self.assertIn("kept staging directory", self.log.getvalue())
        self.build()
        self.assertFalse(live.exists())
        self.assertEqual(self.calls, 1)   # the sweep also runs on a cache hit

    def test_generation_holds_its_owner_lock(self):
        def sweep_during_generation(out):
            P.sweep_prompts(str(self.out))
            self.assertTrue((out / "External.bfotf").exists())
            self.assertTrue((out.parent / ".owner").exists())
        self.during = sweep_during_generation
        self.build()
        self.assertTrue(self.stamp().exists())

    @unittest.skipUnless(os.name == "posix", "owner-based reclamation is POSIX-only")
    def test_unready_or_unreadable_owner_is_kept(self):
        unready = self.staging(".prompts-unready", marked=True, old=True)
        (unready / ".owner").write_bytes(b"\0")
        unreadable = self.staging(".prompts-unreadable", marked=True, old=True)
        open_fd = os.open
        def denied(path, *args, **kwargs):
            relative_owner = (path == ".owner" and kwargs.get("dir_fd") is not None and
                              os.path.samestat(os.fstat(kwargs["dir_fd"]), unreadable.stat()))
            if Path(path) == unreadable / ".owner" or relative_owner:
                raise PermissionError("authored owner denial")
            return open_fd(path, *args, **kwargs)
        with mock.patch.object(P.os, "open", side_effect=denied):
            self.build()
        self.assertTrue(unready.exists())
        self.assertTrue(unreadable.exists())
        self.assertTrue(self.stamp().exists())
        self.assertEqual(self.log.getvalue().count("kept staging directory"), 2)

    def test_staging_removal_failure_is_nonfatal(self):
        stale = self.staging(".prompts-cannot-remove", marked=True, old=os.name == "nt")
        rmtree = P.shutil.rmtree
        def denied(path, *args, **kwargs):
            if Path(path) == stale or (path == stale.name and kwargs.get("dir_fd") is not None):
                raise PermissionError("authored cleanup denial")
            return rmtree(path, *args, **kwargs)
        with mock.patch.object(P.shutil, "rmtree", side_effect=denied):
            self.build()
        self.assertTrue(stale.exists())
        self.assertTrue(self.stamp().exists())
        self.assertEqual(self.log.getvalue().count("kept staging directory"), 1)

    def test_staging_sweep_leaves_other_entries_alone(self):
        neighbour = self.staging("prompts-old", marked=True, old=True)
        ordinary = self.out.parent / ".prompts-x"
        ordinary.write_bytes(b"ordinary file")
        nested = neighbour / ".prompts-nested"
        nested.mkdir()
        self.build()
        self.assertEqual(ordinary.read_bytes(), b"ordinary file")
        self.assertTrue((neighbour / "new" / "authored.bin").exists())
        self.assertTrue(nested.exists())

    def test_staging_sweep_never_follows_links(self):
        outside = self.root / "outside"
        outside.mkdir()
        sentinel = outside / "keep"
        sentinel.write_bytes(b"outside data")
        (outside / ".owner").write_bytes(b"1")
        link = self.out.parent / ".prompts-link"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError) as e:
            self.skipTest(f"cannot create a directory symlink: {e}")
        with mock.patch.object(P, "prompt_owner", wraps=P.prompt_owner) as owner:
            self.build()
        self.assertTrue(all(call.args[0] != str(link) for call in owner.call_args_list))
        self.assertTrue(link.is_symlink())
        self.assertEqual(sentinel.read_bytes(), b"outside data")

    def test_parent_swap_does_not_open_or_lock_external_owner(self):
        if os.name != "posix":
            self.skipTest("POSIX descriptor/lock interleaving")
        import fcntl
        victim = self.staging(".prompts-swapped", marked=True)
        moved = self.root / "moved-staging"
        outside = self.root / "outside"
        outside.mkdir()
        (outside / ".owner").write_bytes(b"1")
        (outside / "sentinel").write_bytes(b"outside data")
        observed = dict(swapped=False, outside_opens=0, outside_locked=False, rmtree_refused=False)
        scandir, open_fd, flock, rmtree = os.scandir, os.open, fcntl.flock, P.shutil.rmtree

        class SwapEntry:
            def __init__(entry, real):
                entry.real, entry.name, entry.path = real, real.name, real.path

            def is_dir(entry, **kwargs):
                return entry.real.is_dir(**kwargs)

            def stat(entry, **kwargs):
                result = entry.real.stat(**kwargs)
                if entry.name == victim.name and not observed["swapped"]:
                    victim.rename(moved)
                    victim.symlink_to(outside, target_is_directory=True)
                    observed["swapped"] = True
                return result

        @contextlib.contextmanager
        def swapping_scan(path):
            with scandir(path) as entries:
                yield (SwapEntry(entry) for entry in entries)

        with (outside / ".owner").open("r+b") as probe:
            outside_id = os.fstat(probe.fileno())
            def opening(path, *args, **kwargs):
                fd = open_fd(path, *args, **kwargs)
                if os.path.samestat(os.fstat(fd), outside_id):
                    observed["outside_opens"] += 1
                return fd

            def locking(fd, operation):
                flock(fd, operation)
                if os.path.samestat(os.fstat(fd if isinstance(fd, int) else fd.fileno()), outside_id):
                    try:
                        flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        observed["outside_locked"] = True
                    else:
                        flock(probe, fcntl.LOCK_UN)

            def removing(path, *args, **kwargs):
                try:
                    return rmtree(path, *args, **kwargs)
                except OSError:
                    if Path(path).name == victim.name:
                        observed["rmtree_refused"] = True
                    raise

            with mock.patch.object(P.os, "scandir", side_effect=swapping_scan), \
                    mock.patch.object(P.os, "open", side_effect=opening), \
                    mock.patch.object(fcntl, "flock", side_effect=locking), \
                    mock.patch.object(P.shutil, "rmtree", side_effect=removing):
                P.sweep_prompts(str(self.out))
            flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)   # the external lock is free after the sweep, even on the old code
            flock(probe, fcntl.LOCK_UN)
        self.assertTrue(observed["swapped"])
        self.assertTrue(victim.is_symlink())
        self.assertTrue((moved / "new" / "authored.bin").exists())
        self.assertEqual((outside / ".owner").read_bytes(), b"1")
        self.assertEqual((outside / "sentinel").read_bytes(), b"outside data")
        self.assertEqual(observed["outside_opens"], 0, observed)
        self.assertFalse(observed["outside_locked"], observed)

    def test_rmtree_refuses_a_top_level_directory_link(self):
        outside = self.root / "outside-rmtree"
        outside.mkdir()
        (outside / "sentinel").write_bytes(b"outside data")
        link = self.out.parent / ".prompts-rmtree-link"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError) as e:
            self.skipTest(f"cannot create a directory symlink: {e}")
        with self.assertRaises(OSError):
            P.shutil.rmtree(link)
        self.assertTrue(link.is_symlink())
        self.assertEqual((outside / "sentinel").read_bytes(), b"outside data")

    def test_windows_age_rule_keeps_fresh_owned_staging(self):
        folder = self.staging(".prompts-fresh-owned", marked=True)
        P.remove_old_prompt_staging(str(folder), folder.stat().st_mtime)
        if os.name == "nt":
            self.build()   # real Windows integration, without changing os.name on another platform
        self.assertEqual((folder / "new" / "authored.bin").read_bytes(), b"unfinished generation")
        self.assertEqual((folder / ".owner").read_bytes(), b"1")

    def test_windows_age_rule_removes_old_owned_and_legacy_staging(self):
        for marked in (False, True):
            with self.subTest(marked=marked):
                folder = self.staging(".prompts-old-" + str(marked), marked=marked, old=True)
                if os.name == "nt":
                    self.build()
                    self.assertTrue(self.stamp().exists())
                else:
                    # Exercise the portable age rule on Linux, not fake Windows filesystem semantics.
                    P.remove_old_prompt_staging(str(folder), folder.stat().st_mtime)
                self.assertFalse(folder.exists(), self.log.getvalue())

    def test_windows_age_rule_reports_removal_refusals(self):
        for error in (PermissionError("in use"), OSError("path too long")):
            with self.subTest(error=error):
                folder = self.staging(".prompts-refused-" + type(error).__name__, old=True)
                self.log.seek(0)
                self.log.truncate()
                with mock.patch.object(P.shutil, "rmtree", side_effect=error):
                    P.remove_old_prompt_staging(str(folder), folder.stat().st_mtime)
                self.assertTrue(folder.exists())
                self.assertEqual(self.log.getvalue().count("kept staging directory"), 1)
                self.assertIn(str(error), self.log.getvalue())

    @unittest.skipUnless(os.name == "nt", "Windows blocks deletion of an open file; Linux does not")
    def test_windows_open_file_keeps_staging_and_build_proceeds(self):
        folder = self.staging(".prompts-open-file", old=True)
        blocked = folder / "new" / "authored.bin"
        with blocked.open("rb"):
            self.build()
            self.assertTrue(folder.exists())
            self.assertTrue(blocked.exists())
            self.assertTrue(self.stamp().exists())
        self.assertIn("kept staging directory", self.log.getvalue())

    @unittest.skipUnless(os.name == "nt", "Windows read-only files refuse deletion")
    def test_windows_readonly_file_keeps_staging_and_build_proceeds(self):
        folder = self.staging(".prompts-readonly-file", old=True)
        blocked = folder / "new" / "authored.bin"
        blocked.chmod(0o444)
        try:
            self.build()
            self.assertTrue(folder.exists())
            self.assertTrue(blocked.exists())
            self.assertTrue(self.stamp().exists())
            self.assertIn("kept staging directory", self.log.getvalue())
        finally:
            if blocked.exists():
                blocked.chmod(0o600)

    def test_cleanup_without_descriptor_safe_rmtree_is_skipped(self):
        if os.name != "posix":
            self.skipTest("POSIX descriptor backend")
        leftover = self.staging(".prompts-no-safe-removal", marked=True)
        with mock.patch.object(P.shutil.rmtree, "avoids_symlink_attacks", False):
            self.build()
        self.assertTrue(leftover.exists())
        self.assertTrue(self.stamp().exists())
        self.assertIn("descriptor-safe removal unavailable", self.log.getvalue())

    def test_every_file_input_kind(self):
        paths = list(dict.fromkeys(self.source_files + self.game_files + self.font_files))
        for path in paths:
            with self.subTest(input=path.name):
                self.build()
                before = self.calls
                old_time = path.stat().st_mtime_ns
                path.write_bytes(b"changed input of the same or another size")
                os.utime(path, ns=(old_time, old_time))
                self.build()
                self.assertEqual(self.calls, before + 1, str(path))

    def test_override_added_changed_and_removed(self):
        path = self.here / "prompt_faces" / "xbox" / "Controller_Pro.png"
        self.build()
        for value in (b"first authored override", b"second authored override", None):
            before = self.calls
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value) if value is not None else path.unlink()
            self.build()
            self.assertEqual(self.calls, before + 1)

    def test_table_selection_and_optional_font_presence(self):
        self.build()
        table = self.game_files[-1].with_name("ResourceSizeTable.aaa.rsizetable.zs")
        table.write_bytes(b"new first table")
        self.build()
        self.assertEqual(self.calls, 2)
        self.game_files[1].unlink()
        self.build()
        self.assertEqual(self.calls, 3)
        self.game_files[1].write_bytes(b"bold font returns")
        self.build()
        self.assertEqual(self.calls, 4)

    def test_each_package_version_and_presence(self):
        self.build()
        for name in self.packages:
            for value in (["different-version", True], [None, False, name]):
                with self.subTest(package=name, value=value):
                    before = self.calls
                    self.packages[name] = value
                    self.build()
                    self.assertEqual(self.calls, before + 1)

    def test_switch_and_parameters(self):
        self.build()
        os.environ["SOURCE_DATE_EPOCH"] = "1234567890"
        self.build()
        self.assertEqual(self.calls, 2)
        self.args.no_textures = True
        self.build()
        self.assertEqual(self.calls, 3)
        self.args.faces = "switch"
        self.build()
        self.assertEqual(self.calls, 4)

    def test_missing_truncated_and_same_size_corrupt_output(self):
        self.build()
        for damage in (lambda p: p.unlink(), lambda p: p.write_bytes(b"cut"),
                       lambda p: p.write_bytes(b"X" * p.stat().st_size)):
            with self.subTest(damage=damage):
                before = self.calls
                damage(self.out / "xbox" / "textures.bin")
                self.build()
                self.assertEqual(self.calls, before + 1)

    def test_missing_empty_keyboard_directory(self):
        self.build()
        (self.out / "keyboard" / "hint.bin").unlink()
        self.build()
        (self.out / "keyboard" / "hint.bin").unlink()
        (self.out / "keyboard").rmdir()
        self.build()
        self.assertEqual(self.calls, 3)

    def test_kept_originals_unchanged_and_runtime_files_cleared(self):
        self.build()
        original = {p.relative_to(self.out): p.read_bytes() for p in self.out.rglob("*") if p.is_file()}
        for name in P.PROMPT_RUNTIME:
            (self.out / name).write_bytes(b"runtime output")
        self.build()
        self.assertEqual(self.calls, 1)
        self.assertEqual({p.relative_to(self.out): p.read_bytes() for p in self.out.rglob("*") if p.is_file()}, original)
        self.assertTrue(all(not (self.out / name).exists() for name in P.PROMPT_RUNTIME))

    def test_unclearable_runtime_file_regenerates_instead_of_keeping(self):
        self.build()
        runtime = self.out / "Common.key"
        runtime.write_bytes(b"runtime output")
        remove = os.remove
        def denied(path):
            if Path(path) == runtime:
                raise PermissionError("authored runtime file denial")
            remove(path)
        with mock.patch.object(P.os, "remove", side_effect=denied):
            self.build()
        self.assertEqual(self.calls, 2)
        self.assertFalse(runtime.exists())
        self.assertIn("runtime files could not be cleared", self.log.getvalue())

    def test_failed_generation_removes_prompts_for_source_and_packaged_builds(self):
        for packaged in (False, True):
            with self.subTest(packaged=packaged):
                self.during = None
                self.build(packaged=packaged)
                self.assertTrue(self.out.exists())
                os.environ["HOOKSHOT_FORCE_PROMPTS"] = "1"
                def failed(out):
                    (out / "External.bfotf").write_bytes(b"partial font")
                    raise OSError("authored drawing failure")
                self.during = failed
                self.build(packaged=packaged)
                self.assertFalse(self.out.exists())
                self.assertIn("the button prompts of other controllers were not made: the game shows its own",
                              self.log.getvalue())

    def test_interrupted_generation_keeps_old_output_without_stamp(self):
        self.build()
        old = (self.out / "External.bfotf").read_bytes()
        os.environ["HOOKSHOT_FORCE_PROMPTS"] = "1"
        def interrupt(out):
            (out / "External.bfotf").write_bytes(b"partial new font")
            raise KeyboardInterrupt()
        self.during = interrupt
        with self.assertRaises(KeyboardInterrupt):
            self.build()
        self.assertFalse(self.stamp().exists())
        self.assertEqual((self.out / "External.bfotf").read_bytes(), old)
        self.assertFalse(list(self.out.parent.glob(".prompts-*")))

    def test_mid_generation_edit_never_stamps_old_output(self):
        self.during = lambda out: self.source_files[0].write_bytes(b"changed while the old source was drawing")
        self.build()
        self.assertFalse(self.stamp().exists(), "post-work key must not certify old output")
        self.during = None
        self.build()
        self.assertEqual(self.calls, 2)
        self.assertTrue(self.stamp().exists())

    def test_unreadable_input_never_skips_or_stamps(self):
        self.build()
        real_file = P.prompt_file
        def denied(path):
            if Path(path) == self.source_files[0]:
                raise PermissionError("authored unreadable input")
            return real_file(path)
        with mock.patch.object(P, "prompt_file", side_effect=denied):
            self.build()
            self.build()
        self.assertEqual(self.calls, 3)
        self.assertFalse(self.stamp().exists())
        self.assertIn("input unreadable", self.log.getvalue())

    def test_forced_switch_and_env_on_false_values(self):
        self.build()
        for value in ("", "0", " false ", "OFF", "No"):
            os.environ["HOOKSHOT_FORCE_PROMPTS"] = value
            self.build()
            self.assertEqual(self.calls, 1)
        os.environ["HOOKSHOT_FORCE_PROMPTS"] = "yes"
        self.build()
        self.assertEqual(self.calls, 2)
        self.assertIn("regenerated (forced)", self.log.getvalue())

    def test_packaged_build_always_regenerates(self):
        self.build(packaged=True)
        self.build(packaged=True)
        self.assertEqual(self.calls, 2)
        self.assertFalse(self.stamp().exists())

    def test_malformed_and_empty_manifest(self):
        self.build()
        for text in ("{", "[]", '{"inputs": {}, "outputs": {}}'):
            self.stamp().write_text(text, encoding="utf-8")
            before = self.calls
            self.build()
            self.assertEqual(self.calls, before + 1)

    def test_incomplete_generation_leaves_no_prompts(self):
        self.build()
        os.environ["HOOKSHOT_FORCE_PROMPTS"] = "1"
        self.during = lambda out: (out / "External.bfotf").unlink()
        self.build()
        self.assertFalse(self.out.exists())
        self.assertFalse(self.stamp().exists())

    def test_failed_publication_leaves_no_prompts(self):
        self.build()
        os.environ["HOOKSHOT_FORCE_PROMPTS"] = "1"
        replace = os.replace
        def fail_new(src, dst):
            if Path(src).name == "new":
                raise PermissionError("authored publication failure")
            replace(src, dst)
        with mock.patch.object(P.os, "replace", side_effect=fail_new):
            self.build()
        self.assertFalse(self.out.exists())
        self.assertFalse(self.stamp().exists())

    def test_no_fonttools_keeps_original_fallback(self):
        self.build()
        with mock.patch.object(M.importlib.util, "find_spec", return_value=None):
            self.build()
        self.assertEqual(self.calls, 1)
        self.assertFalse(self.out.exists())


    def test_unreadable_reason_is_printed_even_if_forced_generation_fails(self):
        os.environ["HOOKSHOT_FORCE_PROMPTS"] = "1"
        def interrupted(out):
            raise OSError("authored generation failure")
        self.during = interrupted
        with mock.patch.object(P, "prompt_key", side_effect=PermissionError("authored unreadable input")):
            self.build()
        self.assertIn("input unreadable", self.log.getvalue())
        self.assertFalse(self.stamp().exists())
        self.assertFalse(self.out.exists())


class PromptDependencyTests(unittest.TestCase):
    def test_package_versions_and_unavailable_imports_are_recorded(self):
        versions = {package: "authored-" + package for package in P.PROMPT_PACKAGES.values()}
        def unavailable(name):
            raise ModuleNotFoundError(name=name)
        with mock.patch.object(P.importlib.metadata, "version", side_effect=lambda n: versions[n]), \
                mock.patch.object(P.importlib, "import_module", side_effect=unavailable):
            got = P.prompt_packages()
        for module, package in P.PROMPT_PACKAGES.items():
            self.assertEqual(got[module], [versions[package], False, module])
        # An importable package without version metadata is not a basis for reusing output.
        with mock.patch.object(P.importlib.metadata, "version", side_effect=P.importlib.metadata.PackageNotFoundError), \
                mock.patch.object(P.importlib, "import_module", return_value=object()):
            with self.assertRaises(OSError):
                P.prompt_packages()

    def test_local_imports_and_environment_are_accounted_for(self):
        sources = {"prompt_assets", "prompt_draw", "prompt_faces", "prompt_keys"}
        for name in sources:
            tree = ast.parse((SOURCE / (name + ".py")).read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                modules = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                    [node.module] if isinstance(node, ast.ImportFrom) and node.module else []
                for module in modules:
                    if (SOURCE / (module.split(".")[0] + ".py")).is_file():
                        self.assertIn(module, sources | {"kit"}, "new prompt dependency needs fingerprint coverage")
        self.assertIn("SOURCE_DATE_EPOCH", P.PROMPT_ENV)
        self.assertIn("FREETYPE_PROPERTIES", P.PROMPT_ENV)
        self.assertIn("archspec", P.PROMPT_PACKAGES)
        self.assertIn("numpy", P.PROMPT_PACKAGES)
        self.assertIn("zstandard", P.PROMPT_PACKAGES)


if __name__ == "__main__":
    unittest.main()
