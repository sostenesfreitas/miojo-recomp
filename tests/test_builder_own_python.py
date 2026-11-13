"""A builder package with a Python of its own never runs another one.

With tools/python/python.exe gone (an antivirus took it, most often) both Windows builders started the tools with the
first "python" Windows found (issue 2045, item 10): another version, without the modules the tools need, or not one the
player chose to trust with the game files. Now the start fails and says how to get the package's Python back, for the
check of the game files and for the build alike. Tools with no python folder at all still use the PC's.

The Python builder's real functions run here on authored folders (nothing is started: the interpreter named does not
exist). The WPF builder's Runner.cs is read for the same rule.

    python -I -B tests/test_builder_own_python.py
"""
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder" / "py"))
from hookshot_builder import core  # noqa: E402

RUNNER = (ROOT / "builder" / "HookshotBuilder" / "Runner.cs").read_text(encoding="utf-8")


class PythonBuilderTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hookshot-own-python-")
        self.addCleanup(temporary.cleanup)
        self.tools = Path(temporary.name) / "tools"
        (self.tools / "nx2").mkdir(parents=True)
        (self.tools / "nx2" / "make_game.py").write_text("raise SystemExit('authored: never run')\n")
        patch = mock.patch.dict(os.environ, {"HOOKSHOT_TOOLS": str(self.tools)})
        patch.start()
        self.addCleanup(patch.stop)
        self.own = str(self.tools / "python" / "python.exe")

    def test_its_own_python_is_used_when_it_is_there(self):
        (self.tools / "python").mkdir()
        (self.tools / "python" / "python.exe").write_bytes(b"authored")
        found = core.Tools.find()
        self.assertEqual(found.python, self.own)
        self.assertFalse(core.own_python_missing(found))

    def test_with_its_python_gone_no_other_is_named(self):
        (self.tools / "python" / "Lib").mkdir(parents=True)   # (what the antivirus left)
        found = core.Tools.find()
        self.assertEqual(found.python, self.own)   # (was "python": whatever Windows finds first)
        self.assertTrue(core.own_python_missing(found))
        self.assertEqual(core.build_command(found, ["--check"])[0], self.own)

    def test_the_check_and_the_build_both_say_how_to_get_it_back(self):
        (self.tools / "python").mkdir()
        found = core.Tools.find()
        with mock.patch.object(core, "CREATE_NO_WINDOW", 0), mock.patch.object(core, "BELOW_NORMAL_PRIORITY_CLASS", 0), \
                mock.patch.object(core.paths, "launcher_command", return_value=("builder", [])):
            self.assertEqual(core.check(found, ["--check"]), (False, core.OWN_PYTHON_MISSING))
            finished, done = [], threading.Event()
            runner = core.Runner(found, ["--out", "x"], on_finished=lambda why: (finished.append(why), done.set()))
            runner._run()
        self.assertEqual(finished, [core.OWN_PYTHON_MISSING])
        self.assertIn("restore it from quarantine", core.OWN_PYTHON_MISSING)

    def test_tools_with_no_python_folder_still_use_the_pcs(self):
        found = core.Tools.find()
        self.assertEqual(found.python, "python")
        self.assertFalse(core.own_python_missing(found))


class WpfBuilderTests(unittest.TestCase):
    def test_the_package_names_its_own_python_whenever_it_has_the_folder(self):
        find = RUNNER[RUNNER.index("public static Tools? Find()"):RUNNER.index("static (string Exe, string[]? Args) PythonOnPath()")]
        self.assertIn('return new Tools(bundled, File.Exists(py) || Directory.Exists(Path.Combine(bundled, "python")) ? py : "python");',
                      find)
        self.assertNotIn('File.Exists(py) ? py : "python"', RUNNER)

    def test_missing_is_the_file_gone_with_its_folder_there(self):
        self.assertIn('public bool OwnPythonMissing => !File.Exists(Python) && Directory.Exists(Path.Combine(Root, "python"));',
                      RUNNER)
        self.assertNotIn('tools.Python == "python"', RUNNER)   # (the old test: never true once the package names its own)

    def test_the_check_and_the_build_both_say_how_to_get_it_back(self):
        start = RUNNER[RUNNER.index("public void Start()"):RUNNER.index("public void Cancel()")]
        self.assertIn("Finished?.Invoke(tools.OwnPythonMissing ? Tools.OwnPythonMissingText :", start)
        check = RUNNER[RUNNER.index("public static async Task<(bool Ok, string Text)> Check("):]
        self.assertIn("if (tools.OwnPythonMissing) return (false, Tools.OwnPythonMissingText);", check)
        self.assertEqual(RUNNER.count("restore it"), 1)   # (one text for both)


if __name__ == "__main__":
    unittest.main()
