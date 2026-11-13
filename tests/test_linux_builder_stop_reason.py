"""The Linux builder says why this build stopped, not why an earlier one did.

The reason under "The build stopped" is the last "@@error" line of make_game.py, or Cancel's own sentence. The window kept it
from one build to the next: a build that ended without a reason of its own (make_game.py killed from outside, by the system
when memory runs out most likely) then gave the reason of the build before it, e.g. "Build cancelled. You can start again
from the same folder." for a build nobody cancelled. The folder a finished build names ("@@done") was kept the same way.

The builder's own functions are run here on a stand-in window, with no Tk and no display: on_line, cancel and on_exit whole,
and of show_progress what it sets before it draws the page.

python -I -B tests/test_linux_builder_stop_reason.py
"""
import ast
import copy
from pathlib import Path
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "builder" / "linux" / "hookshot_builder.py"
BUILDER = next(node for node in ast.parse(SOURCE.read_text(encoding="utf-8")).body
               if isinstance(node, ast.ClassDef) and node.name == "Builder")


def method(name, scope, until=None):
    """The window's method of this name, compiled in scope; with until, only the statements before the one that source starts"""
    node = copy.deepcopy(next(node for node in BUILDER.body if isinstance(node, ast.FunctionDef) and node.name == name))
    if until:
        cut = next(i for i, statement in enumerate(node.body) if ast.unparse(statement).startswith(until))
        node.body = node.body[:cut]
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), "exec"), scope)
    return scope[name]


class Text:
    def __init__(self):
        self.value = ""

    def set(self, value):
        self.value = value

    def configure(self, **options):
        self.__dict__.update(options)

    def see(self, where):
        pass


class Window:
    """What the four functions touch"""

    def __init__(self):
        self.proc, self.page, self.started = None, None, 0.0
        self.killed = []
        self.stage_text, self.time_text, self.heading, self.stage_label = Text(), Text(), Text(), Text()
        self.log, self.bar = Text(), Text()
        self.buttons = SimpleNamespace(winfo_children=lambda: [])
        scope = {"time": SimpleNamespace(time=lambda: 0.0), "game_ready": lambda folder: False, "STAGES": {},
                 "make_game": SimpleNamespace(NOT_PREPARED="(not prepared)"), "signal": SimpleNamespace(SIGTERM=15),
                 "os": SimpleNamespace(killpg=lambda pid, sig: self.killed.append((pid, sig))),
                 "ttk": SimpleNamespace(Button=lambda *a, **k: SimpleNamespace(pack=lambda **k: None))}
        self.reset = method("show_progress", scope, until="px = ")
        self.on_line_, self.on_exit_, self.cancel_ = method("on_line", scope), method("on_exit", scope), method("cancel", scope)

    def px(self, n):
        return n

    def show_form(self):
        pass

    def add_log(self, text):
        pass

    def start(self):   # (build and begin_update: the progress page, then make_game.py)
        self.reset(self)
        self.proc = SimpleNamespace(pid=4242, poll=lambda: None)

    def line(self, text):
        self.on_line_(self, text)

    def ended(self, code):
        self.on_exit_(self, code)
        return self.stage_text.value


class StopReasonTests(unittest.TestCase):
    CANCELLED = "The build stopped: Build cancelled. You can start again from the same folder."
    NO_REASON = "The build stopped: see the lines above"

    def test_a_cancelled_build_says_so(self):
        w = Window()
        w.start()
        w.cancel_(w)
        self.assertEqual(w.killed, [(4242, 15)])
        self.assertEqual(w.ended(-15), self.CANCELLED)

    def test_the_next_build_does_not_repeat_it(self):
        w = Window()
        w.start()
        w.cancel_(w)
        w.ended(-15)
        w.start()   # (Back, Build the game)
        w.line("@@stage lift Translating the game to PC code")
        self.assertEqual(w.ended(-9), self.NO_REASON)   # (killed from outside: no "@@error")

    def test_nor_the_reason_make_game_gave_last_time(self):
        w = Window()
        w.start()
        w.line("@@error The drive is full.")
        self.assertEqual(w.ended(1), "The build stopped: The drive is full.")
        w.start()
        self.assertEqual(w.ended(-9), self.NO_REASON)
        w.start()
        w.line("@@error Another reason.")
        self.assertEqual(w.ended(1), "The build stopped: Another reason.")

    def test_the_folder_of_a_finished_build_is_that_build_s(self):
        w = Window()
        w.start()
        w.line("@@done /games/first")
        self.assertEqual(w.built, "/games/first")
        w.start()
        self.assertIsNone(w.built)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
