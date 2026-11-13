"""A drive that cannot read its own files is said as the drive's fault with its repair, not as a builder error to report.

Issue 2063: listing the unpacked romfs\\Tex raised OSError [WinError 1392] (the file or directory is corrupted and
unreadable) and the Builder said "The builder stopped on an error it did not expect ... Please send the text under Show
details to the Hookshot issues page". The real make_game.cli() runs here with its build replaced by the error; no game
data, no tools, no process started.

python -I -B tests/test_builder_drive_error.py
"""
import contextlib
import errno
import importlib.util
import io
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party/a64-kit/tools"))
spec = importlib.util.spec_from_file_location("make_game_drive_error", ROOT / "nx2/make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

TEX = "D:\\Oot remake\\romfs\\Tex"


def windows_error(code, text, path=TEX, number=errno.EINVAL):
    """An OSError as Windows' Python raises it (winerror is only filled in there: set here on any system)"""
    error = OSError(number, text, path)
    error.winerror = code
    return error


class DriveErrorTests(unittest.TestCase):
    def stopped(self, error):
        output = io.StringIO()
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(make_game, "parse_args", return_value=mock.Mock(log=None)))
            stack.enter_context(mock.patch.object(make_game, "build_log", return_value=contextlib.nullcontext()))
            stack.enter_context(mock.patch.object(make_game, "main", side_effect=error))
            stack.enter_context(contextlib.redirect_stdout(output))
            stack.enter_context(contextlib.redirect_stderr(output))
            with self.assertRaises(SystemExit) as exit_:
                make_game.cli()
        self.assertEqual(exit_.exception.code, 1)
        text = output.getvalue()
        errors = [line for line in text.split("\n") if line.startswith("@@error ")]
        self.assertEqual(len(errors), 1, text)
        self.assertIn("Traceback", text)   # (Show details keeps where it happened)
        return errors[0]

    def test_the_reported_error_names_the_drive_and_its_repair(self):
        said = self.stopped(windows_error(1392, "The file or directory is corrupted and unreadable"))
        self.assertIn(TEX, said)
        self.assertIn("Windows error 1392", said)
        self.assertIn('chkdsk D: /f', said)
        self.assertIn("not of Hookshot or your game files", said)
        self.assertNotIn("did not expect", said)
        self.assertNotIn("issues page", said)

    def test_each_drive_fault_windows_reports(self):
        for code in (23, 1117, 1392, 1393):
            with self.subTest(code=code):
                said = make_game.drive_error(windows_error(code, "authored drive fault"))
                self.assertIn("Windows error %d" % code, said)
                self.assertIn('chkdsk D: /f', said)

    def test_a_share_or_a_missing_path_gets_no_chkdsk_command(self):
        for path in ("\\\\server\\share\\romfs\\Tex", None):
            with self.subTest(path=path):
                said = make_game.drive_error(windows_error(1392, "authored drive fault", path))
                self.assertNotIn("chkdsk", said)
                self.assertIn("check the drive for errors", said)
        self.assertIn("one of the build's files", make_game.drive_error(windows_error(1392, "authored drive fault", None)))

    def test_an_io_error_elsewhere_is_the_drive_too(self):
        said = make_game.drive_error(OSError(errno.EIO, "Input/output error", "/games/oot/romfs/Tex"))
        self.assertIn("/games/oot/romfs/Tex", said)
        self.assertIn("check the drive for errors", said)
        self.assertNotIn("chkdsk", said)

    def test_other_errors_are_still_the_builders_to_hear_of(self):
        others = (windows_error(5, "Access is denied", number=errno.EACCES),
                  windows_error(1224, "The requested operation cannot be performed on a file with a user-mapped section open"),
                  FileNotFoundError(errno.ENOENT, "No such file or directory", "/games/oot/romfs/Tex"),
                  OSError(errno.ENOSPC, "No space left on device"), ValueError("authored programming error"),
                  KeyError(1392))
        for error in others:
            with self.subTest(error=repr(error)):
                self.assertIsNone(make_game.drive_error(error))
        said = self.stopped(others[0])
        self.assertIn("The builder stopped on an error it did not expect", said)
        self.assertIn("issues page", said)


if __name__ == "__main__":
    unittest.main()
