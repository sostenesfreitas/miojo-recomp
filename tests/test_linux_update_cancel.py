"""Cancel stops a Linux update while it is still downloading.

The Linux builder's Cancel only stopped make_game.py. An update first downloads the new version's parts on a thread of its
own, with no make_game.py yet, and through those minutes the button did nothing: the download went on, the new builder
started and the update ran. (The Windows builder cancels its download.)

Cancel now sets an event the download is given. linux_update reads a part a megabyte at a time and ends with Cancelled at
the next one, without the tries a failed download gets, and install removes what it had staged, so the version installed
before stays as it was. The builder's cancel and begin_update are run here on a stand-in window, with no Tk and no display.

python -I -B tests/test_linux_update_cancel.py
"""
import ast
import io
import os
import queue
from pathlib import Path
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

# (Linux only)
RUN_ALL_SKIP = {"windows": "the Linux builder's update, POSIX files"}

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder" / "linux"))
import linux_update  # noqa: E402

SOURCE = ROOT / "builder" / "linux" / "hookshot_builder.py"
BUILDER = next(node for node in ast.parse(SOURCE.read_text(encoding="utf-8")).body
               if isinstance(node, ast.ClassDef) and node.name == "Builder")


def method(name, scope):
    """The window's method of this name, compiled in scope"""
    node = next(node for node in BUILDER.body if isinstance(node, ast.FunctionDef) and node.name == name)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), "exec"), scope)
    return scope[name]


def part(dest, files):
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w") as zipped:
        for name, data in files.items():
            zipped.writestr(name, data)
    data = archive.getvalue()
    return linux_update.Part(linux_update._sha256(data), len(data), dest, "https://example.invalid/%s.zip" % (dest or "tools"),
                             None), data


class Answer:
    """A download's answer: its bytes as read() hands them out, each read noted; at set_at reads, the event is set"""

    def __init__(self, data, stop=None, set_at=None):
        self.data, self.stop, self.set_at, self.reads = io.BytesIO(data), stop, set_at, 0

    def read(self, n=-1):
        self.reads += 1
        if self.reads == self.set_at:
            self.stop.set()
        return self.data.read(n)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class DownloadTests(unittest.TestCase):
    def test_a_read_in_pieces_gives_the_same_bytes_and_keeps_the_limit(self):
        data = os.urandom((3 << 20) + 17)
        answer = Answer(data)
        self.assertEqual(linux_update._read_at_most(answer, len(data), threading.Event()), data)
        self.assertGreater(answer.reads, 3)   # (a megabyte at a time)
        self.assertEqual(linux_update._read_at_most(Answer(data), len(data)), data)
        with self.assertRaises(RuntimeError):
            linux_update._read_at_most(Answer(data), len(data) - 1, threading.Event())

    def test_a_cancelled_download_ends_at_the_next_megabyte_and_is_not_tried_again(self):
        stop = threading.Event()
        answers = []

        def opened(req, timeout=None):
            answers.append(Answer(b"x" * (8 << 20), stop, set_at=3))
            return answers[-1]

        with mock.patch.object(linux_update._OPENER, "open", opened), \
                mock.patch.object(linux_update.time, "sleep", lambda s: self.fail("waited to try again")):
            with self.assertRaises(linux_update.Cancelled) as raised:
                linux_update.fetch_bytes("https://github.com/NotRexed/Hookshot/releases/download/v9/part.zip", stop=stop)
        self.assertEqual(str(raised.exception), "The update was cancelled.")
        self.assertEqual([a.reads for a in answers], [3])   # (one download, three of its eight megabytes; not the API's either)

    def test_cancel_ends_the_wait_between_two_tries(self):
        stop = threading.Event()
        opens = []

        def opened(req, timeout=None):
            opens.append(req)
            stop.set()   # (Cancel, while GitHub does not answer)
            raise OSError("no answer")

        with mock.patch.object(linux_update._OPENER, "open", opened):
            with self.assertRaises(linux_update.Cancelled):
                linux_update.fetch_bytes("https://example.invalid/part.zip", stop=stop)
        self.assertEqual(len(opens), 1)

    def test_without_an_event_a_failed_download_is_tried_again_as_before(self):
        opens, waits = [], []

        def opened(req, timeout=None):
            opens.append(req)
            raise OSError("no answer")

        with mock.patch.object(linux_update._OPENER, "open", opened), mock.patch.object(linux_update.time, "sleep", waits.append):
            with self.assertRaises(RuntimeError):
                linux_update.fetch_bytes("https://example.invalid/part.zip")
        self.assertEqual((len(opens), waits), (4, [2, 4, 8]))


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="hookshot-update-cancel-")
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name) / "home"
        self.versions = self.home / "hookshot" / "builder" / "versions"
        (self.versions / "v1").mkdir(parents=True)
        (self.versions / "v1" / "kept").write_bytes(b"the version installed before")
        (self.versions.parent / "current").write_text("v1\n", encoding="utf-8")
        patch = mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(self.home)})
        patch.start()
        self.addCleanup(patch.stop)
        tools, self.tools = part(".", {"usr/share/hookshot/builder/linux/AppRun": b"#!/bin/sh\n", "inside": b"inside"})
        library, self.library = part("usr/lib", {"target": b"library"})
        self.manifest = linux_update.Manifest("v2", [tools, library])
        self.payloads = {tools.url: self.tools, library.url: self.library}

    def assert_unchanged(self):
        self.assertEqual(sorted(p.name for p in self.versions.iterdir()), ["v1"], "the staging folder was left, or v1 removed")
        self.assertEqual((self.versions / "v1" / "kept").read_bytes(), b"the version installed before")
        self.assertEqual((self.versions.parent / "current").read_text(encoding="utf-8"), "v1\n")

    def test_cancel_in_the_first_part_leaves_the_installed_version(self):
        stop, asked = threading.Event(), []

        def get(url):
            asked.append(url)
            stop.set()   # (Cancel while this part unpacks)
            return self.payloads[url]

        with self.assertRaises(linux_update.Cancelled):
            linux_update.install(self.manifest, None, None, get=get, stop=stop)
        self.assertEqual(len(asked), 1, "the next part was downloaded after Cancel")
        self.assert_unchanged()

    def test_cancel_in_the_last_part_still_leaves_it(self):
        stop, asked = threading.Event(), []

        def get(url):
            asked.append(url)
            if len(asked) == 2:
                stop.set()
            return self.payloads[url]

        with self.assertRaises(linux_update.Cancelled):
            linux_update.install(self.manifest, None, None, get=get, stop=stop)
        self.assert_unchanged()

    def test_cancel_reaches_the_download_install_starts_itself(self):
        stop = threading.Event()
        answers = []

        def opened(req, timeout=None):
            answers.append(Answer(self.payloads[req.full_url] + b"\0" * (4 << 20), stop, set_at=2))
            return answers[-1]

        with mock.patch.object(linux_update._OPENER, "open", opened):
            with self.assertRaises(linux_update.Cancelled):
                linux_update.install(self.manifest, None, None, stop=stop)
        self.assertEqual([a.reads for a in answers], [2])
        self.assert_unchanged()

    def test_an_update_nobody_cancels_installs_as_before(self):
        apprun = linux_update.install(self.manifest, None, None, get=lambda url: self.payloads[url], stop=threading.Event())
        self.assertEqual(apprun, str(self.versions / "v2" / "AppRun"))
        self.assertEqual((self.versions.parent / "current").read_text(encoding="utf-8"), "v2\n")
        self.assertEqual(sorted(p.name for p in self.versions.iterdir()), ["v2"])


class Window:
    """What the builder's cancel and begin_update touch"""

    def __init__(self, proc=None, tools="v1", manifest_tag="v1"):
        # (latest: the version GitHub offers, the one the update builds)
        self.proc, self.tools, self.update_folder, self.update_tag, self.fetched, self.latest = proc, tools, "", None, False, manifest_tag
        self.stop_update = threading.Event()
        self.stage_text = SimpleNamespace(set=lambda text: setattr(self, "stage", text))
        self.stage, self.shown, self.killed, self.threads, self.installs = "", [], [], [], []
        self.lines = SimpleNamespace(put=lambda item: self.shown.append(item))

        def install(manifest, root, source, note=None, stop=None):
            self.installs.append(stop)
            raise RuntimeError("stop here")   # (the real one is in InstallTests)

        update = SimpleNamespace(
            Cancelled=linux_update.Cancelled, update_target=lambda latest, tools, switch: latest, track_of=lambda tag: "release",
            running_root=lambda: "/tools", tools_tag=lambda root: tools, read_manifest=lambda root: None,
            startup_manifest=lambda root, get=None, tag=None: SimpleNamespace(tag=manifest_tag), install=install,
            fetch_bytes=linux_update.fetch_bytes, game_version=lambda folder: (tools,), older_version=lambda new, old: False)
        scope = {"os": SimpleNamespace(killpg=lambda pid, sig: self.killed.append(pid)), "signal": SimpleNamespace(SIGTERM=15),
                 "linux_update": update, "queue": queue,
                 "threading": SimpleNamespace(Thread=lambda target, daemon: SimpleNamespace(start=lambda: self.threads.append(target)))}
        self.cancel_, self.begin_update_, self.pump_ = method("cancel", scope), method("begin_update", scope), method("pump", scope)

    def show_progress(self, title):
        self.shown.append(("page", title))

    def switching(self):
        return False

    def older_ok(self, target, have):
        return True

    def start_make_update(self, folder):
        self.shown.append(("make_game.py", folder))

    def after(self, ms, then):
        pass

    def pump(self, items):
        """The window's own loop, once, over these lines from the update's thread"""
        self.lines = queue.Queue()
        for item in items:
            self.lines.put(item)
        self.pump_(self)

    def cancel(self):
        self.cancel_(self)

    def begin_update(self, folder):
        self.begin_update_(self, folder)


class BuilderTests(unittest.TestCase):
    def test_cancel_with_make_game_running_stops_make_game(self):
        w = Window(proc=SimpleNamespace(pid=4242, poll=lambda: None))
        w.cancel()
        self.assertEqual((w.killed, w.stop_update.is_set()), ([4242], False))
        self.assertEqual(w.error, "Build cancelled. You can start again from the same folder.")

    def test_cancel_with_no_make_game_yet_stops_the_download(self):
        w = Window()
        w.cancel()
        self.assertEqual((w.killed, w.stop_update.is_set(), w.stage), ([], True, "Stopping the update"))

    def test_the_download_is_given_the_event_cancel_sets(self):
        w = Window(manifest_tag="v2")   # (a newer version: installed first)
        w.begin_update("/games/Hookshot")
        w.threads[0]()
        self.assertEqual(w.installs, [w.stop_update])

    def test_an_update_cancelled_before_it_builds_does_not_build(self):
        w = Window()   # (these tools are the version asked for: nothing to install, make_game.py is next)
        w.begin_update("/games/Hookshot")
        w.cancel()
        w.threads[0]()
        self.assertEqual(w.shown, [("page", "Updating your game"), ("update-error", "The update was cancelled.")])

    def test_a_cancel_after_the_threads_last_look_does_not_build(self):
        w = Window()
        w.begin_update("/games/Hookshot")
        w.threads[0]()   # (the thread looked, found no Cancel, and handed over the build)
        w.cancel()   # (before the window's loop took that line)
        w.pump([w.shown.pop()])
        self.assertEqual(w.shown, [("page", "Updating your game")])
        self.assertEqual(w.stage, "The update was cancelled. Your game folder is unchanged.")

    def test_the_next_update_starts_clean(self):
        w = Window()
        w.begin_update("/games/Hookshot")
        w.cancel()
        w.threads[0]()
        w.begin_update("/games/Hookshot")   # (Back, Update)
        self.assertFalse(w.stop_update.is_set())
        w.threads[1]()
        self.assertEqual(w.shown[-1], ("do-build", "/games/Hookshot"))


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
