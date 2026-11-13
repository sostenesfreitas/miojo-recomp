"""The Linux builder's game files page must keep Build in view on a short screen (the Steam Deck's 800 px, #3006): its body
scrolls, the row with the button stays, and the window opens no taller than the screen. Needs Tk and a display; skips without.

python -I -B tests/test_builder_form_scroll.py
"""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

try:
    import tkinter as tk
except ImportError:
    tk = None


def load_builder():
    spec = importlib.util.spec_from_file_location("hookshot_builder_form_scroll", ROOT / "builder" / "linux" / "hookshot_builder.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FormScrollTests(unittest.TestCase):
    def form(self, screen_height):
        if tk is None:
            self.skipTest("no tkinter")
        builder = load_builder()
        win = builder.Builder.__new__(builder.Builder)
        try:
            tk.Tk.__init__(win)
        except tk.TclError as e:
            self.skipTest("no display: %s" % e)
        self.addCleanup(win.destroy)
        out = tempfile.TemporaryDirectory()
        self.addCleanup(out.cleanup)
        win.geometry("+0+0")
        win.winfo_screenheight = lambda: screen_height
        win.scale = 1.0
        win.apply_theme()
        win.cfg = {"out": out.name}
        for name in ("kind", "nup", "romfs", "exefs", "out", "language", "track", "status", "check_text", "space_text"):
            setattr(win, name, tk.StringVar(win, value="nup" if name == "kind" else out.name if name == "out" else ""))
        win.prepare, win.dlss = tk.BooleanVar(win), tk.BooleanVar(win)
        win.page = win.build_button = win.launch_folder = None
        win.apply_mode = lambda: None
        win.show_form()
        win.update()
        return win

    def bottom_of(self, widget, win):
        return widget.winfo_rooty() + widget.winfo_height() - win.winfo_rooty()

    def test_the_window_opens_no_taller_than_the_screen(self):
        win = self.form(560)
        self.assertLessEqual(win.winfo_reqheight(), 560)
        self.assertLessEqual(self.bottom_of(win.build_button, win), win.winfo_height())

    def test_build_stays_in_a_short_window_and_the_page_scrolls(self):
        win = self.form(2000)
        win.geometry("1000x420")
        win.update()
        self.assertGreater(win.winfo_reqheight(), 420)   # (the page is taller than the window)
        self.assertLessEqual(self.bottom_of(win.build_button, win), win.winfo_height())
        canvas = win.build_button.master.master.master.pack_slaves()[-1]
        self.assertIsInstance(canvas, tk.Canvas)
        self.assertEqual(canvas.yview()[0], 0.0)
        self.assertLess(canvas.yview()[1], 1.0)
        self.assertTrue(any(w.place_info() for w in win.page.winfo_children()))   # (the scroll bar shows)
        canvas.winfo_children()[0].event_generate("<MouseWheel>", delta=-120)
        win.update()
        self.assertGreater(canvas.yview()[0], 0.0)


if __name__ == "__main__":
    unittest.main()
