"""Windows cleanup selects only proven work/output owners; all process/kill calls are authored."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import random
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("HOOKSHOT_CLEANUP_TEST_SOURCE", ROOT / "nx2/make_game.py"))
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party/a64-kit/tools")]
spec = importlib.util.spec_from_file_location("cleanup_make_game", SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
WORK = r"C:\authored cleanup\work 漢"
SCRIPT = r"C:\authored tools\nx2\make_game.py"


def row(pid, name, argv, parent=9999):
    return dict(ProcessId=pid, ParentProcessId=parent, Name=name,
                CommandLine=subprocess.list2cmdline(argv) if isinstance(argv, list) else argv)


def python(pid, script="lift.py", work=WORK, parent=9999):
    return row(pid, "python.exe", ["python", "-I", "-u", "-X", "utf8",
                                  "C:\\authored tools\\nx2\\" + script,
                                  "--standalone", "--work", work], parent)


class CleanupTests(unittest.TestCase):
    def invoke(self, rows, work=WORK, default=WORK, query_code=0, kill_code=0, linux=False, error=None):
        calls = []

        def run(command, **kwargs):
            if command[0] == "powershell":
                self.assertIn("Get-CimInstance Win32_Process", command[-1])
                if error:
                    raise error
                return subprocess.CompletedProcess(command, query_code, json.dumps(rows), "")
            self.assertEqual(command[:3], ["taskkill", "/F", "/PID"])
            self.assertEqual(len(command), 4)  # Never kill an unexamined /T tree.
            calls.append(int(command[-1]))
            return subprocess.CompletedProcess(command, kill_code, "", "")

        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(module, "LINUX", linux))
            stack.enter_context(mock.patch.object(module, "__file__", SCRIPT))
            stack.enter_context(mock.patch.object(module.os, "getpid", return_value=100))
            stack.enter_context(mock.patch.object(module.subprocess, "run", side_effect=run))
            stack.enter_context(mock.patch.object(module.time, "sleep"))
            log = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            module.stop_stray_compilers(work, default)
        return calls, log.getvalue()

    def test_unrelated_orphans_and_sibling_outputs(self):
        rows = [python(1, work=r"D:\another\work"),
                row(2, "clang.exe", ["clang", "-c", "x.c", "-o", WORK + r"-other\x.o"]),
                python(3), row(4, "clang.exe", ["clang", "-o", WORK + r"\x.o"])]
        self.assertEqual(self.invoke(rows)[0], [3, 4])

    def test_exact_script_and_explicit_work(self):
        rows = [python(1, "not_lift.py"), python(2, "build.py"),
                python(3, work=r"C:relative"), python(4, work=r"\root-relative"),
                python(5, work=WORK + r"\..\other"), python(6),
                row(7, "python.exe", ["python", "-c", "print('lift.py')", "--work", WORK]),
                row(8, "python.exe", ["python", "unrelated.py", "--script", "lift.py", "--work", WORK])]
        self.assertEqual(self.invoke(rows)[0], [6])

    def test_equals_unicode_case_and_trailing_separator(self):
        rows = [row(1, "python.exe", ["python", "lift.py", "--work=" + WORK + "\\"]),
                python(2, work=WORK.upper()), python(3, work=WORK.replace("\\", "/"))]
        self.assertEqual(self.invoke(rows)[0], [1, 2, 3])

    def test_default_requires_same_absolute_entrypoint(self):
        rows = [row(1, "python.exe", ["python", SCRIPT]),
                row(2, "python.exe", ["python", r"D:\other\nx2\make_game.py"]),
                row(3, "python.exe", ["python", "make_game.py"]),
                row(4, "python.exe", ["python", "lift.py", "--standalone"]),
                row(5, "python.exe", ["python", "build.py", "--standalone"])]
        self.assertEqual(self.invoke(rows)[0], [1])
        self.assertEqual(self.invoke(rows, default=r"D:\other")[0], [])

    def test_compiler_outputs_not_inputs_includes_or_response_files(self):
        rows = [row(1, "clang.exe", ["clang", "-I" + WORK, "-o", r"D:\other\x.o"]),
                row(2, "clang.exe", ["clang", "-c", WORK + r"\x.c"]),
                row(3, "clang.exe", ["clang", "@" + WORK + r"\args.rsp"]),
                row(4, "lld-link.exe", ["lld-link", "/OUT:" + WORK + r"\game.exe"]),
                row(5, "clang-cl.exe", ["clang-cl", "/Fo" + WORK + r"\x.obj"]),
                row(6, "ld.lld.exe", ["ld.lld", "-o", WORK + r"\game.exe"]),
                row(7, "unrelated.exe", ["unrelated", "-o", WORK + r"\x.o"]),
                row(8, "clang++.exe", ["clang++", "-o", "relative.o"])]
        self.assertEqual(self.invoke(rows)[0], [4, 5, 6])

    def test_default_work_across_packaged_builder_versions(self):
        home = r"C:\Users\authored user\AppData\Local\HookshotBuilder"
        old = home + r"\versions\v-old\HookshotBuilder\tools\nx2\make_game.py"
        rows = [row(10, "python.exe", ["python", "-I", old]),
                row(11, "python.exe", ["python", "nx2/build.py", "--standalone"], parent=10),
                row(12, "clang.exe", ["clang", "-o", home + r"\sa\build\one.o"]),
                row(20, "python.exe", ["python", r"D:\other\versions\v-old\HookshotBuilder\tools\nx2\make_game.py"]),
                row(21, "python.exe", ["python", home + r"-other\versions\v-old\HookshotBuilder\tools\nx2\make_game.py"]),
                row(22, "python.exe", ["python", home + r"\versions\v-old\extra\HookshotBuilder\tools\nx2\make_game.py"]),
                row(23, "python.exe", ["python", home + r"\versions\v-old\other\tools\nx2\make_game.py"]),
                row(24, "python.exe", ["python", old, "--work", r"D:\other"])]
        self.assertEqual(self.invoke(rows, work=home, default=home)[0], [10, 12, 11])
        # A packaged script's implicit folder never establishes a selected custom work.
        self.assertEqual(self.invoke(rows, work=WORK, default=home)[0], [])

    def test_ancestors_across_non_python_parents_and_unknown_children(self):
        rows = [python(100, parent=101), row(101, "launcher.exe", "launcher", parent=102),
                python(102, parent=103), row(103, "cmd.exe", "cmd", parent=0),
                python(110), row(111, "unrelated.exe", "other", parent=110)]
        self.assertEqual(self.invoke(rows)[0], [110])

    def test_build_child_requires_proven_live_make_game_parent(self):
        parent = python(10, "make_game.py")
        rows = [parent,
                row(11, "python.exe", ["python", "-I", "nx2/build.py", "--standalone"], parent=10),
                row(12, "python.exe", ["python", "other.py", "--standalone"], parent=10),
                row(13, "python.exe", ["python", "nx2/build.py", "--standalone"], parent=9999),
                row(14, "python.exe", ["python", r"D:\other\nx2\build.py", "--standalone"], parent=10),
                python(20, "make_game.py", work=r"D:\other"),
                row(21, "python.exe", ["python", "nx2/build.py", "--standalone"], parent=20)]
        self.assertEqual(self.invoke(rows)[0], [10, 11])

    def test_ambiguous_or_missing_metadata(self):
        rows = [row(1, "python.exe", 'python lift.py --work "unterminated'),
                row(2, "python.exe", None), row(3, "python.exe", ["python", "lift.py", "--work"]),
                row(4, "python.exe", ["python", "lift.py", "--work", WORK, "--work", r"D:\other"]),
                row(5, "clang.exe", ["clang", "-o", WORK + r"\x.o", "-o", r"D:\other\x.o"])]
        self.assertEqual(self.invoke(rows)[0], [])

    def test_empty_single_row_query_and_nonfatal_failures(self):
        self.assertEqual(self.invoke([])[0], [])
        self.assertEqual(self.invoke(python(1))[0], [1])
        calls, log = self.invoke([python(1)], query_code=1)
        self.assertEqual(calls, [])
        self.assertIn("could not look for an earlier build's processes", log)
        self.assertEqual(self.invoke([python(1)], error=TimeoutError("authored timeout"))[0], [])
        calls, log = self.invoke([python(1)], kill_code=1)
        self.assertEqual(calls, [1])
        self.assertNotIn("stopped 1", log)
        self.assertEqual(self.invoke([python(1)], linux=True)[0], [])

    def test_quoted_argument_roundtrip(self):
        if not hasattr(module, "_cleanup_argv"):
            self.fail("Windows argument ownership parser is missing")
        cases = [["python", SCRIPT, "--work", WORK + "\\"],
                 ["clang", "", "one two", 'quote"inside', "C:\\one\\", "tab\there"],
                 ["python", "-I", "-u", "-X", "utf8", SCRIPT, "--work=" + WORK]]
        for argv in cases:
            with self.subTest(argv=argv):
                self.assertEqual(module._cleanup_argv(subprocess.list2cmdline(argv)), argv)
        generator = random.Random(2142)
        for _ in range(500):
            argv = ["python"] + ["".join(generator.choice('ab \\t\\\\"漢')
                                       for _ in range(generator.randrange(20)))
                                  for _ in range(4)]
            self.assertEqual(module._cleanup_argv(subprocess.list2cmdline(argv)), argv)


if __name__ == "__main__":
    unittest.main()
