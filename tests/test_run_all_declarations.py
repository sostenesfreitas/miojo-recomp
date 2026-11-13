"""tests/run_all.py's reader of what a test declares in its own file (RUN_ALL_ARGS, RUN_ALL_MODULES, RUN_ALL_SKIP): the
valid shapes, the malformed ones that stop the run with the file and line, placeholders find_tools does not know, OSes
it does not know, and the retired central tables, which a branch from before the move must not fill again.

python -I -B tests/test_run_all_declarations.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("run_all", ROOT / "tests" / "run_all.py")
run_all = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_all)

TOOLS = {"cxx": None, "vulkan_headers": None, "spirv_headers": None, "spirv_val": None, "glslang": None, "dotnet": None,
         "dav1d": None}


class Reader(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def read(self, source):
        path = os.path.join(self.tmp.name, "test_x.py")
        with open(path, "w", encoding="utf-8") as f:
            f.write(textwrap.dedent(source))
        return run_all.read_declarations(path, "tests/test_x.py", TOOLS)

    def bad(self, source, *words):
        with self.assertRaises(run_all.DeclarationError) as e:
            self.read(source)
        self.assertIn("tests/test_x.py:", str(e.exception))
        for w in words:
            self.assertIn(w, str(e.exception))
        return str(e.exception)

    def test_nothing_declared(self):
        self.assertEqual(self.read('"""RUN_ALL_ARGS in the docstring is not a declaration"""\nimport os\n'),
                         {"args": [], "modules": [], "skip": {}, "unparsed": None})

    def test_all_three(self):
        d = self.read('''
            """doc"""
            import sys

            RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}", "--cc", "gcc"]
            RUN_ALL_MODULES = ["zstandard"]
            # (a GPU)
            RUN_ALL_SKIP = {
                "linux": "needs a Vulkan device",
                "windows": "needs a Vulkan device",
            }


            def main():
                RUN_ALL_ARGS = len   # (not top level: not a declaration)
            ''')
        self.assertEqual(d["args"], ["--cxx", "{cxx}", "--headers", "{vulkan_headers}", "--cc", "gcc"])
        self.assertEqual(d["modules"], ["zstandard"])
        self.assertEqual(d["skip"], {"linux": "needs a Vulkan device", "windows": "needs a Vulkan device"})

    def test_annotated_and_concatenated(self):
        d = self.read('RUN_ALL_SKIP: dict = {"windows": "a long reason"\n                               " in two parts"}\n')
        self.assertEqual(d["skip"], {"windows": "a long reason in two parts"})

    def test_syntax_error_declares_nothing(self):
        # (the runner does not stop for it: the check fails, with why, on every OS)
        d = self.read('RUN_ALL_ARGS = ["--cxx"\n')
        self.assertEqual((d["args"], d["modules"], d["skip"]), ([], [], {}))
        self.assertIn("tests/test_x.py: does not parse", d["unparsed"])

    def test_not_a_literal(self):
        self.bad('CXX = "{cxx}"\nRUN_ALL_ARGS = ["--cxx", CXX]\n', "RUN_ALL_ARGS", "plain literal", ":2:")
        self.bad('RUN_ALL_MODULES = list(("zstandard",))\n', "plain literal")
        self.bad('RUN_ALL_ARGS = ["--cxx"] + ["{cxx}"]\n', "plain literal")

    def test_wrong_shape(self):
        self.bad('RUN_ALL_ARGS = "--cxx {cxx}"\n', "list of strings")
        self.bad('RUN_ALL_ARGS = ("--cxx", "{cxx}")\n', "list of strings")
        self.bad('RUN_ALL_MODULES = ["zstandard", 1]\n', "list of strings")
        self.bad('RUN_ALL_SKIP = ["windows"]\n', "dict of OS -> reason")
        self.bad('RUN_ALL_SKIP = ({"windows"}, "why")\n', "dict of OS -> reason")
        self.bad('RUN_ALL_SKIP = {"windows": ""}\n', "dict of OS -> reason")

    def test_assigned_other_ways(self):
        self.bad('RUN_ALL_ARGS = ["--cc", "gcc"]\nRUN_ALL_ARGS = ["--cxx", "g++"]\n', "assigned twice", ":2:")
        self.bad('RUN_ALL_ARGS = []\nRUN_ALL_ARGS += ["--cc", "gcc"]\n', "NAME = literal")
        self.bad('RUN_ALL_ARGS, X = ["--cc"], 1\n', "NAME = literal")
        self.bad('RUN_ALL_ARGS = X = ["--cc", "gcc"]\n', "NAME = literal")
        self.bad('RUN_ALL_SKIP = {}\nRUN_ALL_SKIP["windows"] = "why"\n', "NAME = literal")

    def test_set_or_changed_elsewhere(self):
        # (the runner reads only a top-level NAME = literal: anything else would be ignored without a word)
        self.bad('import os\nif os.name == "nt":\n    RUN_ALL_SKIP = {"windows": "why"}\n', "top level", ":3:")
        self.bad('try:\n    import zstandard\nexcept ImportError:\n    RUN_ALL_MODULES = ["zstandard"]\n', "top level")
        self.bad('RUN_ALL_ARGS = ["--cxx"]\nRUN_ALL_ARGS.append("{cxx}")\n', ".append", ":2:")
        self.bad('RUN_ALL_SKIP = {}\nRUN_ALL_SKIP.update(windows="why")\n', ".update")
        self.bad('RUN_ALL_ARGS = ["--cc", "gcc"]\ndel RUN_ALL_ARGS\n', "top level")
        self.bad('import os\nif os.name == "nt":\n    RUN_ALL_SKIP["windows"] = "why"\nRUN_ALL_SKIP = {}\n', "NAME = literal")
        # (reading one is fine, and so is a function's own name)
        d = self.read('RUN_ALL_ARGS = ["--cc", "gcc"]\nprint(RUN_ALL_ARGS)\n\n\ndef f():\n    RUN_ALL_ARGS = []\n'
                      '    RUN_ALL_ARGS.append(1)\n')
        self.assertEqual(d["args"], ["--cc", "gcc"])

    def test_unknown_name(self):
        self.bad('RUN_ALL_ARG = ["--cxx", "{cxx}"]\n', "RUN_ALL_ARG is not", "RUN_ALL_ARGS")

    def test_unknown_placeholder(self):
        self.bad('RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_header}"]\n', "{vulkan_header}",
                 "unknown placeholder", "vulkan_headers")

    def test_placeholders_are_find_tools(self):
        # (the reader is checked against what find_tools looks for: TOOLS above must be that set)
        self.assertEqual(set(run_all.find_tools()), set(TOOLS))

    def test_unknown_os(self):
        self.bad('RUN_ALL_SKIP = {"win32": "why"}\n', "unknown OS 'win32'", "linux, windows")
        self.bad('RUN_ALL_SKIP = {"windows": "why", "macos": "why"}\n', "'macos'")


class Runner(unittest.TestCase):
    def run_main(self, root, *argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(run_all, "ROOT", root), mock.patch.object(sys, "argv", ["run_all.py", *argv]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = run_all.main()
        return rc, out.getvalue(), err.getvalue()

    def test_bad_declaration_stops_the_run(self):
        with tempfile.TemporaryDirectory() as root:
            for d in ("tests", "nx2"):
                os.mkdir(os.path.join(root, d))
            with open(os.path.join(root, "nx2", "test_bad.py"), "w", encoding="utf-8") as f:
                f.write('import os\n\nRUN_ALL_SKIP = {"window": "why"}\n')
            rc, out, err = self.run_main(root)
        self.assertEqual(rc, 2)
        self.assertIn("nx2/test_bad.py:3:", err)
        self.assertEqual(out, "")

    def test_bad_declaration_elsewhere_does_not_stop_a_named_run(self):
        with tempfile.TemporaryDirectory() as root:
            for d in ("tests", "nx2"):
                os.mkdir(os.path.join(root, d))
            with open(os.path.join(root, "nx2", "test_bad.py"), "w", encoding="utf-8") as f:
                f.write('RUN_ALL_SKIP = {"window": "why"}\n')
            with open(os.path.join(root, "tests", "test_good.py"), "w", encoding="utf-8") as f:
                f.write('import sys\n\nRUN_ALL_ARGS = ["--flag"]\nsys.exit(0 if sys.argv[1:] == ["--flag"] else 1)\n')
            with mock.patch.object(run_all, "compile_all", lambda: run_all.Result(0, b"")):
                rc, out, err = self.run_main(root, "test_good")
                self.assertEqual(rc, 0, out + err)
                self.assertIn("tests/test_good.py", out)
                rc, out, err = self.run_main(root, "test_bad")
                self.assertEqual(rc, 2)
                self.assertIn("nx2/test_bad.py:1:", err)

    def test_unparsed_test_fails_without_running(self):
        with tempfile.TemporaryDirectory() as root:
            for d in ("tests", "nx2"):
                os.mkdir(os.path.join(root, d))
            with open(os.path.join(root, "tests", "test_broken.py"), "w", encoding="utf-8") as f:
                f.write('RUN_ALL_SKIP = {"windows": "why"\n')
            for os_name in ("linux", "windows"):
                with mock.patch.object(run_all, "OS", os_name), \
                        mock.patch.object(run_all, "compile_all", lambda: run_all.Result(0, b"")):
                    rc, out, err = self.run_main(root, "test_broken")
                self.assertEqual(rc, 1, os_name)
                self.assertIn("does not parse", out)

    def test_stale_central_entry_stops_the_run(self):
        # (a branch from before the move that still adds its line to ARGS, MODULES or SKIP)
        with mock.patch.dict(run_all.ARGS, {"tests/test_new.py": ["--cxx", "{cxx}"]}), \
                mock.patch.dict(run_all.SKIP, {"tests/test_new.py": ({"windows"}, "why")}):
            rc, out, err = self.run_main(str(ROOT))
        self.assertEqual(rc, 2)
        self.assertIn("retired", err)
        self.assertIn("tests/test_new.py: declare it in the test: RUN_ALL_ARGS = ['--cxx', '{cxx}']", err)
        self.assertIn("tests/test_new.py: declare it in the test: RUN_ALL_SKIP = {'windows': 'why'}", err)
        self.assertEqual(out, "")

    def test_this_repository(self):
        self.assertEqual((run_all.ARGS, run_all.MODULES, run_all.SKIP), ({}, {}, {}),
                         "tests/run_all.py's ARGS, MODULES and SKIP are retired: declare RUN_ALL_ARGS, RUN_ALL_MODULES "
                         "or RUN_ALL_SKIP in the test file instead (see the docstring of tests/run_all.py)")
        for d in ("tests", "nx2"):
            for f in sorted(os.listdir(ROOT / d)):
                if f.startswith("test_") and f.endswith(".py"):
                    run_all.read_declarations(str(ROOT / d / f), d + "/" + f, TOOLS)


if __name__ == "__main__":
    unittest.main()
