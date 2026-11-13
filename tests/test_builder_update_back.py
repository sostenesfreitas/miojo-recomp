"""Back from an update that stopped, the builder's first page offers the update again.

The button of the first page is left alone while an update runs (RefreshMode / refresh_mode return at once). An update that
stopped, or was cancelled, never let that go: back on the first page the button stayed what it was before the update. After a
start with --update (the game's update offer, or the setup once the new builder is in) the page had never been set up at
all, so it showed a grey "Build the game" with no game files, and the Update button only came back when the app was closed
and opened again (#2129).

The Python builder's own functions are run here, cut out of window.py, on a stand-in window; the C# builder's are read.

python -I -B tests/test_builder_update_back.py
"""
from pathlib import Path
import textwrap
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
WINDOW = (ROOT / "builder" / "py" / "hookshot_builder" / "window.py").read_text(encoding="utf-8")
MAIN = (ROOT / "builder" / "HookshotBuilder" / "MainWindow.xaml.cs").read_text(encoding="utf-8")


def method(name):
    """A method of window.py's window, as a function"""
    start = WINDOW.index("    def %s(self" % name)
    end = WINDOW.index("\n    def ", start + 1)
    scope = {"os": __import__("os"), "updater": Updater}
    exec(textwrap.dedent(WINDOW[start:end]), scope)
    return scope[name]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class Updater:
    @staticmethod
    def latest_tag():
        return "v9.9.9"


class Path_:
    def __init__(self, text):
        self.value = text

    def text(self):
        return self.value

    def setText(self, text):
        self.value = text


class Window:
    """What back and found_go touch, each call noted"""
    files_page = "files"

    def __init__(self, folder, updating, files_ready, saved="C:/Games/Last build"):
        self.out_path = Path_(folder)
        self.updating, self.files_ready, self.kept_folder, self.saved = updating, files_ready, "", saved
        self.calls = []
        self.latest_tag = None

    def note(name):
        return lambda self: self.calls.append(name)

    refresh, find_files, fill_dumps = note("refresh"), note("find_files"), note("fill_dumps")
    show_main, check_all, refresh_mode = note("show_main"), note("check_all"), note("refresh_mode")

    def set_step(self, on):
        self.calls.append("step %d" % on)

    def show_page(self, page):
        self.calls.append("page %s" % page)

    def apply_settings(self):   # (the saved folder, as the real one sets it)
        self.calls.append("apply_settings")
        self.out_path.setText(self.saved)

    def post(self, f, *a):
        f(*a)

    def got_latest(self, tag):
        self.latest_tag = tag
        self.refresh_mode()

    back = method("back")
    found_go = method("found_go")


class PythonBuilder(unittest.TestCase):
    def test_a_build_that_stopped_goes_back_as_before(self):
        w = Window("C:/Games/Hookshot", updating=False, files_ready=True)
        w.back()
        self.assertEqual(w.calls, ["step 0", "page files"])

    def test_an_update_that_stopped_lets_the_button_go(self):
        w = Window("C:/Games/Hookshot", updating=True, files_ready=True)
        w.back()
        self.assertFalse(w.updating)
        self.assertEqual(w.calls, ["step 0", "page files", "refresh"])

    def test_after_a_start_with_update_the_first_page_is_set_up_for_that_folder(self):
        w = Window("D:/Other/Hookshot", updating=True, files_ready=False)
        w.back()
        self.assertFalse(w.updating)
        self.assertEqual(w.calls, ["step 0", "page files", "find_files"])   # (start's own way to the first page)
        self.assertEqual(w.kept_folder, "D:/Other/Hookshot")
        w.back()   # (pressed again before the game files are found: not looked for twice)
        self.assertEqual(w.calls.count("find_files"), 1)
        w.calls.clear()
        w.found_go()
        for t in threading.enumerate():   # (found_go asks for the newest release on a thread of its own)
            if t is not threading.current_thread():
                t.join(5)
        self.assertEqual(w.out_path.text(), "D:/Other/Hookshot", "the saved folder replaced the update's")
        self.assertTrue(w.files_ready)
        self.assertLess(w.calls.index("apply_settings"), w.calls.index("fill_dumps"))
        self.assertEqual((w.latest_tag, w.calls[-1]), ("v9.9.9", "refresh_mode"))

    def test_the_first_start_keeps_the_saved_folder(self):
        w = Window("", updating=False, files_ready=False)
        w.found_go()
        self.assertEqual(w.out_path.text(), "C:/Games/Last build")
        self.assertTrue(w.files_ready)

    def test_a_cancelled_update_goes_the_same_way(self):
        start = WINDOW.index("    def ended(self")
        ended = WINDOW[start:WINDOW.index("\n    def ", start + 1)]
        cancelled = ended[ended.index('if failure == "cancelled":'):]
        self.assertIn("self.back()", cancelled[:cancelled.index("return")])
        # (what makes it needed: the button is not touched while an update runs)
        start = WINDOW.index("    def refresh_mode(self")
        self.assertIn("if self.updating or self.runner:\n            return", WINDOW[start:start + 200])


class WindowsBuilder(unittest.TestCase):
    def test_back_and_cancel_go_through_one_function(self):
        self.assertIn("void Back(object sender, RoutedEventArgs e) => ToFiles();", MAIN)
        ended = block(MAIN, "void Ended(string? failure)")
        cancelled = ended[ended.index('if (failure == "cancelled")'):]
        self.assertIn("ToFiles();", cancelled[:cancelled.index("return;")])
        self.assertEqual(MAIN.count("ShowPage(FilesPage);"), 1)   # (no other way back to it)

    def test_it_lets_the_button_go_and_sets_the_page_up(self):
        to_files = block(MAIN, "async void ToFiles()")
        self.assertIn("if (BuildButton == null || updating || runner != null) return;", block(MAIN, "void RefreshMode()"))
        order = ["ShowPage(FilesPage);", "if (!updating) return;", "updating = false;", "if (!filesReady)", "filesReady = true;",
                 "var folder = OutPath.Text;", "Finder.FindDumps", "ApplySettings();", "OutPath.Text = folder;", "FillDumps();",
                 "CheckAll();", "latestTag ??=", "Refresh();"]
        at = [to_files.index(part) for part in order]
        self.assertEqual(at, sorted(at))
        start = block(MAIN, "async void Start()")
        self.assertLess(start.index("FillDumps();"), start.index("filesReady = true;"))
        self.assertLess(start.index("filesReady = true;"), start.index("ShowMain();", start.index("FillDumps();")))


if __name__ == "__main__":
    unittest.main()
