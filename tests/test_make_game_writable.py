"""Game-free: a game folder the build cannot write to stops at once with a plain reason, not minutes later inside nup.py with a
bare "PermissionError: [WinError 5]" (a game folder under Program Files).

Run with: python -I tests/test_make_game_writable.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_writable_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class Writable(unittest.TestCase):
    def stopped(self, out, env=None, error=None):
        printed = io.StringIO()
        real_open = open

        def refusing_open(path, *args, **kwargs):
            if os.path.basename(path) == ".hookshot_write_check":
                raise error or PermissionError(13, "Access is denied")
            return real_open(path, *args, **kwargs)

        with contextlib.redirect_stdout(printed), mock.patch("builtins.open", refusing_open), \
                mock.patch.dict(os.environ, env or {}), self.assertRaises(SystemExit) as stop:
            make_game.check_writable(out)
        self.assertEqual(stop.exception.code, 1)
        return printed.getvalue()

    def test_a_writable_folder_passes_and_leaves_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "authored game folder")
            make_game.check_writable(out)
            self.assertTrue(os.path.isdir(out))
            self.assertEqual(os.listdir(out), [])

    def test_a_refused_folder_stops_with_the_folder_named(self):
        with tempfile.TemporaryDirectory() as d:
            text = self.stopped(d)
        self.assertTrue(text.startswith("@@error The game folder "), text)
        self.assertIn(d, text)
        self.assertIn("Access is denied", text)
        self.assertIn("choose a game folder of your own", text)
        self.assertNotIn("Program Files", text)

    def test_program_files_is_named_as_the_reason(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "authored game folder")
            text = self.stopped(out, {"ProgramFiles(x86)": d})
        self.assertIn("under Program Files", text)

    def test_program_files_is_not_blamed_for_another_error(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "authored game folder")
            text = self.stopped(out, {"ProgramFiles(x86)": d}, error=OSError(28, "No space left on device"))
        self.assertIn("No space left on device", text)
        self.assertNotIn("Program Files", text)

    def test_a_blocked_write_with_another_error_stops_the_same_way(self):
        # (#2792: the probe refused in a folder just made with "[Errno 2] No such file or directory")
        with tempfile.TemporaryDirectory() as d:
            text = self.stopped(d, error=FileNotFoundError(2, "No such file or directory"))
        self.assertTrue(text.startswith("@@error The game folder "), text)
        self.assertIn(d, text)
        self.assertIn("No such file or directory", text)
        self.assertIn("choose a game folder of your own", text)


if __name__ == "__main__":
    unittest.main()
