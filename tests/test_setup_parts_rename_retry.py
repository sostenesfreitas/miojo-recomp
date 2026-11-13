"""The Python setup tries a rename that Windows refuses with access denied again for up to 20 s, and raises any other error at once

python -I -B tests/test_setup_parts_rename_retry.py
"""
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "builder" / "py"))
from hookshot_builder import setup_parts  # noqa: E402


class ReplaceRetry(unittest.TestCase):
    def run_with(self, errors, system="nt"):
        calls, waits = [], []

        def replace(src, dst):
            calls.append((src, dst))
            if errors:
                raise errors.pop(0) if isinstance(errors, list) else errors()
        with mock.patch.object(setup_parts.os, "replace", replace), mock.patch.object(setup_parts.os, "name", system), \
                mock.patch.object(setup_parts.time, "sleep", waits.append):
            try:
                setup_parts.replace_retry("from", "to")
                return calls, waits, None
            except OSError as e:
                return calls, waits, e

    def test_held_twice_then_free(self):
        calls, waits, error = self.run_with([PermissionError(13, "Access is denied"), PermissionError(13, "Access is denied")])
        self.assertIsNone(error)
        self.assertEqual(len(calls), 3)
        self.assertEqual(waits, [0.25, 0.25])

    def test_other_error_at_once(self):
        calls, waits, error = self.run_with([FileNotFoundError(2, "The system cannot find the file specified")])
        self.assertIsInstance(error, FileNotFoundError)
        self.assertEqual((len(calls), waits), (1, []))

    def test_held_for_ever(self):
        calls, waits, error = self.run_with(lambda: PermissionError(13, "Access is denied"))
        self.assertIsInstance(error, PermissionError)
        self.assertEqual((len(calls), len(waits)), (80, 79))

    def test_not_windows_at_once(self):
        calls, waits, error = self.run_with(lambda: PermissionError(13, "Permission denied"), system="posix")
        self.assertIsInstance(error, PermissionError)
        self.assertEqual((len(calls), waits), (1, []))

    def test_install_uses_it(self):
        source = Path(setup_parts.__file__).read_text(encoding="utf-8")
        body = source[source.index("def install("):source.index("\ndef ", source.index("def install("))]
        self.assertNotIn("os.replace(", body)
        self.assertEqual(body.count("replace_retry("), 2)


if __name__ == "__main__":
    unittest.main()
