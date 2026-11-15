"""Missing selected packages use the builder's expected-error protocol, without game data or tools."""
import contextlib
import errno
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("HOOKSHOT_MISSING_NUP_TEST_SOURCE", ROOT / "nx2/make_game.py"))
# The source override changes only make_game.py; its ordinary, stdlib-only toolchain import stays current.
sys.path.insert(0, str(ROOT / "third_party/a64-kit/tools"))
spec = importlib.util.spec_from_file_location("make_game_missing_nup", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class MissingNupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hookshot-missing-nup-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.package = self.root / "selected game \u6f22 [1].nup"
        self.out, self.work = self.root / "output", self.root / "work"
        (self.out / "romfs").mkdir(parents=True)
        (self.out / "romfs/asset").write_bytes(b"authored asset")
        (self.out / ".hookshot_exefs").mkdir()
        (self.out / ".hookshot_exefs/main").write_bytes(b"authored code sentinel")
        (self.out / "user/save").mkdir(parents=True)
        (self.out / "user/save/sentinel").write_bytes(b"authored save sentinel")
        (self.out / ".hookshot_romfs").write_text("previous authored package\n", encoding="utf-8")
        self.before = self.files()
        self.events = []

    def files(self):
        return {p.relative_to(self.out).as_posix(): p.read_bytes() for p in self.out.rglob("*") if p.is_file()}

    def invoke(self, stat_error=None):
        actual_stat = os.stat

        def stat(path, *args, **kwargs):
            if os.fspath(path) == str(self.package) and stat_error is not None:
                raise stat_error
            return actual_stat(path, *args, **kwargs)

        def run(command, *args, **kwargs):
            self.assertEqual(command, make_game.PY + ["nx2/nup.py", str(self.package), "--extract", str(self.out / ".game")])
            self.events.append("extract")
            raise SystemExit(0)   # Stop at the real extraction request; never run a tool or read a package.

        def modules(path):
            self.assertEqual(path, str(self.work / "exefs"))
            self.events.append("cached modules")
            raise SystemExit(0)

        argv = [str(SOURCE), "--jobs", "1", "--nup", str(self.package), "--out", str(self.out), "--work", str(self.work)]
        output = io.StringIO()
        with contextlib.ExitStack() as stack:
            for name in ("check_windows_sdk", "stop_stray_compilers", "drop_stale_objects", "no_throttle"):
                stack.enter_context(mock.patch.object(make_game, name))
            stack.enter_context(mock.patch("ctypes.WinDLL", create=True))   # No Windows power-state change.
            blocked = [stack.enter_context(mock.patch.object(make_game.subprocess, "Popen", side_effect=AssertionError("process launched"))),
                       stack.enter_context(mock.patch.object(os, "system", side_effect=AssertionError("shell launched"))),
                       stack.enter_context(mock.patch.object(os, "startfile", create=True, side_effect=AssertionError("file launched")))]
            stack.enter_context(mock.patch.object(make_game, "run", side_effect=run))
            stack.enter_context(mock.patch.object(make_game, "check_modules", side_effect=modules))
            stack.enter_context(mock.patch.object(os, "stat", side_effect=stat))
            stack.enter_context(mock.patch.object(sys, "argv", argv))
            stack.enter_context(contextlib.redirect_stdout(output))
            stack.enter_context(contextlib.redirect_stderr(output))
            with self.assertRaises(SystemExit) as stopped:
                make_game.cli()   # Actual argument parsing, main's package branch, fail(), and CLI exception handling.
            for launch in blocked:
                launch.assert_not_called()
        return stopped.exception.code, output.getvalue()

    def expected_error(self, stat_error=None):
        code, text = self.invoke(stat_error)
        self.assertEqual(code, 1, text)
        self.assertIn("@@error Could not access your .nup file", text)
        self.assertIn(str(self.package), text)
        self.assertIn("choose the file again", text)
        self.assertIn("reconnect", text)
        self.assertNotIn("Traceback", text)
        self.assertNotIn("error it did not expect", text)
        if stat_error is not None:
            self.assertIn(str(stat_error), text)
        self.assertEqual(self.events, [])
        self.assertEqual(self.files(), self.before)

    def test_missing_file_with_unicode_spaces_and_brackets(self):
        self.expected_error()

    def test_missing_parent_directory(self):
        self.package = self.root / "disconnected drive" / "selected.nup"
        self.expected_error()

    def test_saved_selection_removed_before_build(self):
        self.package.write_bytes(b"authored temporary selection")
        self.package.unlink()
        self.expected_error()

    def test_denied_stat_preserves_os_reason(self):
        self.expected_error(PermissionError(errno.EACCES, "authored stat denial", str(self.package)))

    def test_other_stat_error_preserves_os_reason(self):
        self.expected_error(OSError(errno.EIO, "authored device unavailable", str(self.package)))

    def test_existing_package_still_requests_extraction(self):
        self.package.write_bytes(b"authored package sentinel")
        code, text = self.invoke()
        self.assertEqual(code, 0, text)
        self.assertEqual(self.events, ["extract"])
        self.assertNotIn("@@error", text)
        self.assertEqual(self.files(), self.before)

    def test_existing_package_still_reuses_matching_cache(self):
        self.package = self.root / "existing cached game [1].nup"
        self.package.write_bytes(b"authored package sentinel")
        stamp = "unpacked by this builder: %s %d\n" % (self.package.name, self.package.stat().st_size)
        (self.out / ".hookshot_romfs").write_text(stamp, encoding="utf-8")
        self.before = self.files()
        code, text = self.invoke()
        self.assertEqual(code, 0, text)
        self.assertEqual(self.events, ["cached modules"])
        self.assertIn("reading it again is skipped", text)
        self.assertNotIn("@@error", text)
        self.assertEqual(self.files(), self.before)
        self.assertEqual((self.work / "exefs/main").read_bytes(), b"authored code sentinel")

    def test_programming_error_still_uses_unexpected_error_handler(self):
        code, text = self.invoke(RuntimeError("authored unexpected failure"))
        self.assertEqual(code, 1, text)
        self.assertIn("Traceback", text)
        self.assertIn("error it did not expect: RuntimeError: authored unexpected failure", text)
        self.assertEqual(self.events, [])


if __name__ == "__main__":
    unittest.main()
