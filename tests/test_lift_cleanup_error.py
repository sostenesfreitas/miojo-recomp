"""Actual make_game fresh-lift ordering with tiny authored files and deletion-error doubles.

No game, translator, SDK, child process or permission change is used. Only the real
fresh-lift branch and its key/report helpers are compiled from the production AST.
"""
import ast
import contextlib
import errno
import hashlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "nx2/make_game.py"


def production():
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"), filename=str(SOURCE))
    helpers = {"say", "fail", "gen_print", "write_lift_key", "lifted_before", "kept_before", "kept_list_fits"}
    definitions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in helpers]
    assert {node.name for node in definitions} == helpers
    key = next(node for node in tree.body if isinstance(node, ast.Assign) and
               any(isinstance(target, ast.Name) and target.id == "LIFT_KEY" for target in node.targets))
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "main")
    branches = [node for node in ast.walk(main) if isinstance(node, ast.If) and isinstance(node.test, ast.Call) and
                isinstance(node.test.func, ast.Name) and node.test.func.id == "lifted_before"]
    assert len(branches) == 1
    # (PREPARE: the module global say() reads since --prepare for RecompLauncher; off, as make_game starts)
    namespace = dict(os=os, sys=sys, hashlib=hashlib, PREPARE=False)
    exec(compile(ast.Module(body=[key, *definitions], type_ignores=[]), str(SOURCE), "exec"), namespace)
    return namespace, compile(ast.Module(body=branches, type_ignores=[]), str(SOURCE), "exec")


class LiftCleanupErrorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot-lift-cleanup-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.work = self.root / "work"
        self.gen = self.work / "sa/gen"
        self.image = self.work / "sa/image"
        self.key = self.gen / ".lift_key"
        self.events = []
        self.namespace, self.branch = production()
        self.namespace.update(work=str(self.work), exefs=str(self.root / "exefs"), env={}, key="b" * 40, PY=["authored-python"],
                              gen_dir=str(self.gen), KEPT_LIST="lifted_objects.txt", KEPT_SCHEME=1,
                              kept_lift=[], contextlib=contextlib)
        self.actual_write = self.namespace["write_lift_key"]

        def write_key(work, key):
            self.events.append("key")
            self.actual_write(work, key)

        def run(command, environment):
            self.assertEqual(command, ["authored-python", "nx2/lift.py", "--standalone", "--write", "--exefs",
                                       str(self.root / "exefs"), "--work", str(self.work)])
            self.assertEqual(environment, {})
            self.events.append("lift")
            self.gen.mkdir(parents=True, exist_ok=True)
            self.image.mkdir(parents=True, exist_ok=True)
            (self.gen / "code_new.c").write_text("/* authored replacement */\n", encoding="utf-8")
            (self.gen / "a64_table.c").write_text("/* authored table */\n", encoding="utf-8")
            (self.image / "image.bin").write_bytes(b"authored image sentinel")

        self.inputs = ["i" * 40]   # (lift_key(planning=False) as the lift starts, then after it: unchanged unless a test says)

        def rekey(exefs, work, env, planning=True):   # (the key taken again after the lift, from what the next build reads)
            self.assertEqual((exefs, work, env), (str(self.root / "exefs"), str(self.work), {}))
            if not planning:
                return self.inputs[0] if len(self.inputs) == 1 else self.inputs.pop(0)
            self.events.append("rekey")
            return "c" * 40

        self.namespace.update(run=run, write_lift_key=write_key, lift_key=rekey)

    def seed(self, same_key=False):
        self.gen.mkdir(parents=True)
        self.image.mkdir(parents=True)
        (self.gen / "code_obsolete.c").write_text("/* authored obsolete shard */\n", encoding="utf-8")
        (self.gen / "code_obsolete.c.whole").write_text("/* authored old whole */\n", encoding="utf-8")
        (self.gen / "a64_table.c").write_text("/* authored old table */\n", encoding="utf-8")
        (self.image / "image.bin").write_bytes(b"authored old image")
        self.actual_write(str(self.work), "b" * 40 if same_key else "a" * 40)

    def exercise(self):
        self.assertTrue(self.gen.resolve().is_relative_to(self.root))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            try:
                exec(self.branch, self.namespace)
            finally:
                self.output = output.getvalue()

    def blocked(self, name, error_type, error_number):
        self.seed()
        target = self.gen / name
        error = error_type(error_number, "authored cleanup failure", str(target))
        remove = os.remove

        def delete(path):
            self.assertTrue(Path(path).resolve().is_relative_to(self.root))
            if Path(path) == target:
                raise error
            return remove(path)

        caught = None
        with mock.patch.object(os, "remove", side_effect=delete):
            try:
                self.exercise()
            except SystemExit as stopped:
                caught = stopped
        self.assertIsNotNone(caught, "cleanup error was suppressed: events=%r, key_exists=%r, stale_exists=%r" %
                             (self.events, self.key.exists(), target.exists()))
        self.assertEqual(caught.code, 1)
        self.assertEqual(self.events, [], "must stop before translation or a new key")
        self.assertFalse(self.key.exists())
        self.assertTrue(target.exists(), "a denied removal must not be bypassed")
        self.assertIn("@@error", self.output)
        self.assertIn(str(target), self.output)
        self.assertIn(str(error), self.output)

    def test_obsolete_c_permission_error_stops_before_lift(self):
        self.blocked("code_obsolete.c", PermissionError, errno.EACCES)

    def test_obsolete_whole_permission_error_stops_before_lift(self):
        self.blocked("code_obsolete.c.whole", PermissionError, errno.EACCES)

    def test_other_removal_oserror_stops_before_lift(self):
        self.blocked("code_obsolete.c", OSError, errno.EIO)

    def test_success_removes_only_recognized_generated_files_before_lift(self):
        self.seed()
        protected = {self.gen / "code_notes.txt": b"authored note", self.gen / "keep.c": b"authored unrelated C",
                     self.gen / "a64_support.h": b"authored header", self.work / "objcache/sentinel.o": b"authored cache"}
        for path, data in protected.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        child = self.namespace["run"]

        def run(command, environment):
            self.assertFalse((self.gen / "code_obsolete.c").exists())
            self.assertFalse((self.gen / "code_obsolete.c.whole").exists())
            self.assertFalse(self.key.exists())
            child(command, environment)

        self.namespace["run"] = run
        self.exercise()
        self.assertEqual(self.events, ["lift", "rekey", "key"])
        self.assertTrue(self.namespace["lifted_before"](str(self.work), "c" * 40))
        for path, data in protected.items():
            self.assertEqual(path.read_bytes(), data)

    def test_missing_file_race_is_benign(self):
        self.seed()
        target = self.gen / "code_obsolete.c"
        remove = os.remove

        def delete(path):
            if Path(path) == target:
                remove(path)
                raise FileNotFoundError(errno.ENOENT, "authored already removed", str(path))
            return remove(path)

        with mock.patch.object(os, "remove", side_effect=delete):
            self.exercise()
        self.assertEqual(self.events, ["lift", "rekey", "key"])
        self.assertTrue(self.namespace["lifted_before"](str(self.work), "c" * 40))

    def test_matching_cached_lift_is_not_cleared(self):
        self.seed(same_key=True)
        before = {path.name: path.read_bytes() for path in self.gen.iterdir()}
        with mock.patch.object(os, "remove", side_effect=AssertionError("cached lift must not be cleared")):
            self.exercise()
        self.assertEqual(self.events, [])
        self.assertEqual({path.name: path.read_bytes() for path in self.gen.iterdir()}, before)

    def test_first_lift_without_generated_directory(self):
        self.exercise()
        self.assertEqual(self.events, ["lift", "rekey", "key"])
        self.assertTrue(self.namespace["lifted_before"](str(self.work), "c" * 40))

    def test_an_input_changed_during_the_lift_keeps_the_key_from_before(self):
        """A translator file edited while the lift ran: its C is from the old file, so the new key must not vouch for it"""
        self.inputs = ["i" * 40, "j" * 40]
        self.exercise()
        self.assertEqual(self.events, ["lift", "rekey", "key"])   # (taken, then not kept: the comparison came after it)
        self.assertTrue(self.namespace["lifted_before"](str(self.work), "b" * 40))
        self.assertFalse(self.namespace["lifted_before"](str(self.work), "c" * 40))

    def test_failed_lift_leaves_no_fresh_key(self):
        self.seed()

        def stopped(command, environment):
            self.events.append("lift")
            raise SystemExit(37)

        self.namespace["run"] = stopped
        with self.assertRaises(SystemExit) as result:
            self.exercise()
        self.assertEqual(result.exception.code, 37)
        self.assertEqual(self.events, ["lift"])
        self.assertFalse(self.key.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
