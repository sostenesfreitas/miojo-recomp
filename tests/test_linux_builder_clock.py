"""The Linux builder's clock runs during a build.

The line under the bar ("Elapsed 0:12:40 · compiling 9:05 · about 20 minutes left") is written by tick, which goes on, once a
second, only while make_game.py runs. A build called it from show_progress, before make_game.py was started, so it stopped at
once: a build showed no clock and no countdown from its first second to its last. Only an update, which calls it again after
starting make_game.py, showed them.

The builder's own tick and on_exit are run here on a stand-in window, with no Tk and no display; that build starts the clock
once make_game.py runs is in test_linux_builder_jobs.py.

python -I -B tests/test_linux_builder_clock.py
"""
import ast
from pathlib import Path
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "builder" / "linux" / "hookshot_builder.py"
TREE = ast.parse(SOURCE.read_text(encoding="utf-8"))
BUILDER = next(node for node in TREE.body if isinstance(node, ast.ClassDef) and node.name == "Builder")


def functions(names, scope):
    """The builder's functions of these names (the window's methods, or the file's own), compiled in scope"""
    found = [node for node in TREE.body + BUILDER.body if isinstance(node, ast.FunctionDef) and node.name in names]
    assert sorted(node.name for node in found) == sorted(names)
    exec(compile(ast.Module(body=found, type_ignores=[]), str(SOURCE), "exec"), scope)
    return scope


class Text:
    def __init__(self):
        self.value, self.style = "", None

    def set(self, value):
        self.value = value

    def configure(self, **options):
        self.__dict__.update(options)


class Window:
    """What tick and on_exit touch"""

    def __init__(self, now):
        self.now = now
        self.proc, self.started, self.compile_started, self.eta = None, 0.0, None, None
        self.time_text, self.stage_text, self.heading, self.stage_label = Text(), Text(), Text(), Text()
        self.buttons = SimpleNamespace(winfo_children=lambda: [])
        self.later = []
        scope = functions(["tick", "on_exit", "left_text"], {
            "time": SimpleNamespace(time=lambda: self.now[0]), "game_ready": lambda folder: False,
            "ttk": SimpleNamespace(Button=lambda *a, **k: SimpleNamespace(pack=lambda **k: None))})
        self.tick_, self.on_exit_ = scope["tick"], scope["on_exit"]

    def tick(self):
        self.tick_(self)

    def after(self, ms, call):
        self.later.append((ms, call))

    def px(self, n):
        return n

    def show_form(self):
        pass

    def start(self):   # (as build and start_make_update: make_game.py started, then the clock)
        self.started = self.now[0]
        self.proc = SimpleNamespace(poll=lambda: None)
        self.tick()

    def second(self):
        self.now[0] += 1
        ms, call = self.later.pop(0)
        assert ms == 1000 and not self.later
        call()


class ClockTests(unittest.TestCase):
    def test_before_make_game_runs_there_is_nothing_to_count(self):
        w = Window([1000.0])
        w.tick()   # (where show_progress called it)
        self.assertEqual((w.time_text.value, w.later), ("", []))

    def test_it_counts_each_second_of_the_build(self):
        w = Window([1000.0])
        w.start()
        self.assertEqual(w.time_text.value, "Elapsed 0:00:00")
        for _ in range(65):
            w.second()
        self.assertEqual(w.time_text.value, "Elapsed 0:01:05")
        w.compile_started, w.eta = w.now[0] - 5, 1200.0   # (on_line's, at "@@stage compile" and "@@eta 1200")
        w.second()
        self.assertEqual(w.time_text.value, "Elapsed 0:01:06 · compiling 0:06 · about 20 minutes left")

    def test_it_ends_with_the_build(self):
        w = Window([1000.0])
        w.start()
        w.compile_started, w.eta = w.now[0], 1200.0
        w.second()
        self.assertIn("about 20 minutes left", w.time_text.value)
        w.on_exit_(w, 1)   # (cancelled, or stopped)
        self.assertIsNone(w.proc)
        self.assertEqual(w.time_text.value, "", "a build that stopped still says how long is left")
        self.assertEqual(w.heading.text, "The build stopped")
        w.second()   # (the second that was still waited for)
        self.assertEqual((w.time_text.value, w.later), ("", []))


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
