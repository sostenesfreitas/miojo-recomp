"""nx2/build.py's links skipped when their inputs are the last successful link's (run_link; the dev-loop proposal's A4): the key
is the command, the compiler, each object's key (its .o.sha1 stamp), the other files the command names (inside an option too,
as the hot symbol order is) by content; a link that failed, or an output of another size, links again.

Nothing here runs a compiler or needs the game: the "linker" is this Python, writing its output and counting its runs.

python -I -B tests/test_link_skip.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party" / "a64-kit" / "tools")]
spec = importlib.util.spec_from_file_location("build_link_skip", ROOT / "nx2" / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

LINKER = ("import os, sys\n"
          "out, runs = sys.argv[1], os.environ['LINK_RUNS']\n"   # (the count by the environment: a named file would be an input)
          "open(runs, 'a').write('x')\n"
          "sys.exit(1) if os.path.exists(runs + '.fail') else open(out, 'wb').write(b'linked')\n")


class LinkSkipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot link skip ")
        self.addCleanup(self.temporary.cleanup)
        d = Path(self.temporary.name)
        self.d, self.out, self.runs = d, str(d / "hookshot.exe"), str(d / "runs.txt")
        self.obj = d / "a.o"
        self.obj.write_bytes(b"object")
        (d / "a.o.sha1").write_text("1" * 40)
        self.rsp = d / "objs.rsp"
        self.rsp.write_text('"%s"' % str(self.obj).replace("\\", "/"), encoding="utf-8")
        self.lib = d / "lib.a"
        self.lib.write_bytes(b"library")
        self.order = d / "hot_order.txt"
        self.order.write_text("f_80000000\n")
        self.script = d / "linker.py"
        self.script.write_text(LINKER)

    def link(self, fail=False):
        """(fail: the linker fails, told by a file beside its count: not by its command, which is part of the key)"""
        if fail:
            Path(self.runs + ".fail").write_text("")
        elif os.path.exists(self.runs + ".fail"):
            os.remove(self.runs + ".fail")
        cmd = [sys.executable, str(self.script), self.out, "@" + str(self.rsp), str(self.lib),
               "-Xlinker", "-Xlink=/order:@" + str(self.order), "-Xlinker", "/map:" + self.out[:-4] + ".map"]
        with contextlib.redirect_stdout(io.StringIO()), mock.patch.dict(os.environ, {"LINK_RUNS": self.runs}):
            return build.run_link(cmd, self.out).returncode

    def count(self):
        return len(Path(self.runs).read_text()) if os.path.exists(self.runs) else 0

    def test_the_same_inputs_are_not_linked_again(self):
        self.assertEqual((self.link(), self.count()), (0, 1))
        self.assertEqual((self.link(), self.count()), (0, 1))

    def test_a_changed_object_library_or_symbol_order_is_linked_again(self):
        self.link()
        for change in (lambda: (self.d / "a.o.sha1").write_text("2" * 40), lambda: self.lib.write_bytes(b"library 2"),
                       lambda: self.order.write_text("f_80000004\n")):
            before = self.count()
            change()
            self.link()
            self.assertEqual(self.count(), before + 1)

    def test_an_output_of_another_size_or_gone_is_linked_again(self):
        self.link()
        Path(self.out).write_bytes(b"something else")
        self.link()
        self.assertEqual(self.count(), 2)
        os.remove(self.out)
        self.link()
        self.assertEqual(self.count(), 3)

    def test_a_failed_link_forgets_the_key(self):
        self.link()
        self.assertEqual(self.link(fail=True), 0)   # (skipped: the inputs are the last good link's)
        (self.d / "a.o.sha1").write_text("3" * 40)
        self.assertEqual(self.link(fail=True), 1)
        self.assertFalse(os.path.exists(self.out + ".linkkey"))

    def test_the_files_inside_options_are_inputs(self):
        objs, others = build.link_inputs(["clang", "@" + str(self.rsp), "-Xlink=/order:@" + str(self.order),
                                          "--symbol-ordering-file=" + str(self.order), str(self.lib), "-o", self.out,
                                          "/map:" + self.out[:-4] + ".map"], self.out)
        self.assertEqual(objs, [str(self.obj).replace("\\", "/")])
        self.assertIn(str(self.order), others)
        self.assertIn(str(self.lib), others)
        self.assertNotIn(self.out, others)

    def test_the_compile_commands_are_the_compiles(self):
        import json
        jobs = [("clang", str(self.d / "a.c"), str(self.obj), ["-O1", "-Ifoo"], b"deps")]
        build.write_compdb(str(self.d), jobs)
        entries = json.loads((self.d / build.COMPDB).read_text(encoding="utf-8"))
        self.assertEqual(entries, [{"directory": os.getcwd(), "file": str(self.d / "a.c"), "output": str(self.obj),
                                    "arguments": ["clang", "-O1", "-Ifoo", "-c", str(self.d / "a.c"), "-o", str(self.obj)]}])
        src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        main = src[src.index("\ndef main("):]
        self.assertLess(main.index("write_compdb(out, jobs)"), main.index("plan_jobs(jobs)"))
        one = src[src.index("\ndef compile_one("):src.index("\ndef ", src.index("\ndef compile_one(") + 1)]
        self.assertIn('[cc] + fl + ["-c", src, "-o", obj]', one)   # (the command the database says, as compile_one runs it)

    def test_the_build_links_through_it(self):
        src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        tail = src[src.index("    vk_objs = {j[2] for j in vk_jobs}"):]
        self.assertEqual(tail.count("run_link("), 3)   # (the exe on Linux and on Windows, the back end's library)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
