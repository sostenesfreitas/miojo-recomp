"""Exercise the production Linux build method without importing Tk or starting a build."""
import ast
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "builder" / "linux" / "hookshot_builder.py"
spec = importlib.util.spec_from_file_location("make_game_jobs", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

# Compile the actual method, with its existing validation, command and side effects.
# The class and module are not executed: no Tk import or window is needed.
builder = next(node for node in ast.parse(SOURCE.read_text(encoding="utf-8")).body
               if isinstance(node, ast.ClassDef) and node.name == "Builder")
build = next(node for node in builder.body if isinstance(node, ast.FunctionDef) and node.name == "build")
BUILD = compile(ast.Module(body=[build], type_ignores=[]), str(SOURCE), "exec")


def value(text):
    return SimpleNamespace(get=lambda: text)


class BuildHarness:
    def __init__(self, folder, environ, kind="nup"):
        self.environ = dict(environ)
        self.events, self.processes = [], []
        self.folder = folder
        nup = folder / "synthetic.nup"
        nup.touch()
        romfs, exefs = folder / "romfs", folder / "exefs"
        romfs.mkdir(exist_ok=True)
        exefs.mkdir(exist_ok=True)
        self.gui = SimpleNamespace(
            out=value(str(folder / "out")), kind=value(kind), nup=value(str(nup)),
            romfs=value(str(romfs)), exefs=value(str(exefs)), language=value("English (US)"), prepare=value(True),
            cfg={"previous": "kept"}, show_progress=lambda: self.events.append(("progress",)), read=lambda proc: None,
            tick=lambda: self.events.append(("tick",)))

        def start_process(command, **kwargs):
            self.events.append(("process",))
            self.processes.append((command, kwargs))
            return SimpleNamespace()

        def thread(**kwargs):
            self.events.append(("thread", kwargs))
            return SimpleNamespace(start=lambda: self.events.append(("start",)))

        namespace = {
            "os": SimpleNamespace(path=os.path, environ=self.environ), "ROOT": str(ROOT),
            "sys": sys, "make_game": make_game,
            # (toolchain.default_jobs is only asked for RX_JOBS now: it answers RX_JOBS as the real one does)
            "toolchain": SimpleNamespace(default_jobs=lambda: max(1, int(self.environ["RX_JOBS"]))),
            "messagebox": SimpleNamespace(showerror=lambda title, text: self.events.append(("error", title, text)),
                                          askokcancel=lambda title, text: True),
            # (the hard drive warning is asked first: these builds are on a folder that is not a hard drive)
            "slow_disk_warning": lambda folder: None,
            "save_config": lambda cfg: self.events.append(("save", dict(cfg))),
            "time": SimpleNamespace(time=lambda: 123.0),
            "subprocess": SimpleNamespace(Popen=start_process, PIPE=subprocess.PIPE, STDOUT=subprocess.STDOUT),
            "threading": SimpleNamespace(Thread=thread),
        }
        exec(BUILD, namespace)
        self.build = namespace["build"]

    def run(self):
        self.build(self.gui)


class BuilderBuildJobsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot-builder-jobs-")
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)

    def assert_started(self, harness, jobs=None):
        self.assertEqual([event[0] for event in harness.events], ["save", "progress", "process", "thread", "start", "tick"])
        gui = harness.gui
        source = ["--nup", gui.nup.get()] if gui.kind.get() == "nup" else ["--romfs", gui.romfs.get(), "--exefs", gui.exefs.get()]
        expected = [sys.executable, "-I", str(ROOT / "nx2" / "make_game.py")] + source + [
            "--out", gui.out.get(), "--language", "en-US"] + ([] if jobs is None else ["--jobs", str(jobs)])
        self.assertEqual(len(harness.processes), 1)
        command, options = harness.processes[0]
        self.assertEqual(command, expected)
        self.assertEqual(options, dict(cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                       encoding="utf-8", errors="replace", start_new_session=True))
        self.assertEqual(harness.events[0][1], dict(previous="kept", kind=gui.kind.get(), nup=gui.nup.get(),
                                                  romfs=gui.romfs.get(), exefs=gui.exefs.get(), out=gui.out.get(), language="en-US",
                                                  prepare="1"))
        self.assertEqual(harness.events[3][1], dict(target=gui.read, args=(gui.proc,), daemon=True))

    def test_positive_hookshot_jobs_is_passed_on(self):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": "4", "RX_JOBS": "2"})
        harness.run()
        self.assert_started(harness, 4)
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": "4", "RX_JOBS": "2"})

    def test_unticked_prepare_leaves_the_shaders_to_the_first_start(self):
        harness = BuildHarness(self.folder, {})
        harness.gui.prepare = value(False)
        harness.run()
        command, _ = harness.processes[0]
        self.assertEqual(command[-1], "--no-prepare-shaders")
        self.assertEqual(harness.events[0][1]["prepare"], "0")

    def test_zero_falls_back_to_rx_jobs_for_extracted_folders(self):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": "0", "RX_JOBS": "2"}, kind="folders")
        harness.run()
        self.assert_started(harness, 2)
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": "0", "RX_JOBS": "2"})

    def test_unset_falls_back_to_rx_jobs(self):
        harness = BuildHarness(self.folder, {"RX_JOBS": "2"})
        harness.run()
        self.assert_started(harness, 2)
        self.assertEqual(harness.environ, {"RX_JOBS": "2"})

    def test_empty_falls_back_to_rx_jobs_without_changing_inherited_environment(self):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": "", "RX_JOBS": "2"})
        harness.run()
        self.assert_started(harness, 2)
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": "", "RX_JOBS": "2"})

    # (issue 531) Neither set: no --jobs, so make_game chooses (one per CPU thread, the memory gate deciding), as on Windows
    def test_no_override_leaves_the_choice_to_make_game(self):
        for environ in ({}, {"HOOKSHOT_JOBS": "0"}, {"HOOKSHOT_JOBS": "", "RX_JOBS": ""}):
            with self.subTest(environ=environ):
                harness = BuildHarness(self.folder, environ)
                harness.run()
                self.assert_started(harness)

    def test_invalid_rx_jobs_stops_before_build_side_effects(self):
        harness = BuildHarness(self.folder, {"RX_JOBS": "n"})
        harness.run()
        self.assertEqual(harness.processes, [])
        self.assertEqual(harness.gui.cfg, {"previous": "kept"})
        self.assertEqual([event[0] for event in harness.events], ["error"])

    def assert_invalid(self, raw):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": raw, "RX_JOBS": "2"})
        harness.run()
        self.assertEqual(harness.gui.cfg, {"previous": "kept"})
        self.assertEqual(harness.processes, [])
        self.assertFalse(hasattr(harness.gui, "started"))
        self.assertFalse(hasattr(harness.gui, "proc"))
        self.assertEqual(len(harness.events), 1)
        kind, title, message = harness.events[0]
        self.assertEqual((kind, title), ("error", "Hookshot"))
        self.assertIn("HOOKSHOT_JOBS", message)
        self.assertIn("0 for automatic", message)
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": raw, "RX_JOBS": "2"})

    def test_negative_override_stops_before_build_side_effects(self):
        self.assert_invalid("-1")

    def test_nonnumeric_override_stops_before_build_side_effects(self):
        self.assert_invalid("n")


class MakeGameJobsTests(unittest.TestCase):
    """make_game's own choice, which the Linux window now leaves to it (issue 531)"""

    def choose(self, cpus, os_name):
        with mock.patch.object(make_game.os, "cpu_count", return_value=cpus), \
                mock.patch.object(make_game.os, "name", os_name), \
                contextlib.redirect_stdout(io.StringIO()):
            return make_game.default_jobs()

    def test_one_a_cpu_thread_and_the_memory_gate_decides(self):
        # (nx2/build.py's MemoryGate starts each compiler when the memory free holds it, so the count is not fitted to the free
        # memory at the start; the window chose 7 from 32 GB total at 3.5 GB each on a 16-thread PC)
        self.assertEqual(self.choose(16, "posix"), 16)
        self.assertEqual(self.choose(16, "nt"), 16)
        self.assertEqual(self.choose(32, "posix"), 32)

    def test_the_gate_floor_is_windows_only(self):
        # (Windows pages rather than fails; Linux stops a compiler that does not fit)
        import build
        with mock.patch.object(build.os, "cpu_count", return_value=16), mock.patch.object(build.os, "name", "nt"):
            self.assertEqual(build.MemoryGate().floor, 4)
        with mock.patch.object(build.os, "cpu_count", return_value=16), mock.patch.object(build.os, "name", "posix"):
            self.assertEqual(build.MemoryGate().floor, 1)

    def chosen(self, given, environ):
        calls = []
        with mock.patch.dict(make_game.os.environ, environ, clear=True), \
                mock.patch.object(make_game, "default_jobs", side_effect=lambda: calls.append(1) or 9):
            return make_game.chosen_jobs(given), calls

    def test_jobs_given_wins_and_nothing_is_worked_out(self):
        # (the "compilers at once" line was printed even when --jobs was given, with a number the build did not use)
        self.assertEqual(self.chosen(5, {"HOOKSHOT_JOBS": "3"}), (5, []))

    def test_hookshot_jobs_then_automatic(self):
        self.assertEqual(self.chosen(0, {"HOOKSHOT_JOBS": "3"}), (3, []))
        self.assertEqual(self.chosen(0, {"HOOKSHOT_JOBS": "0"}), (9, [1]))
        self.assertEqual(self.chosen(0, {"HOOKSHOT_JOBS": ""}), (9, [1]))
        self.assertEqual(self.chosen(0, {}), (9, [1]))

    def test_invalid_hookshot_jobs_stops_with_its_message(self):
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            self.chosen(0, {"HOOKSHOT_JOBS": "n"})


if __name__ == "__main__":
    unittest.main()
