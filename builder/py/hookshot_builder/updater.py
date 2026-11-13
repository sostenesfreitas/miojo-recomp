"""The launcher's update and the builder's own (Updater.cs, the same rules). A game built when it asked about updates itself starts
this app with --update <game folder> --tag <release>. When these tools are not that release, the release is installed (setup_parts: only the
parts that changed are downloaded) and this app starts again on it with --fetched to build the folder; when they are, the folder is
built again here (nx2/make_game.py --update: from the game files the folder keeps)."""
import json
import os
import subprocess
import threading
import time
import urllib.request

from . import REPO, paths, setup_parts
from .core import Tools

LATEST = "https://git.notrexed.dev/api/v1/repos/%s/releases/latest" % REPO


def latest_tag():
    """The latest release's tag, quietly (None: GitHub did not answer); a few tries, 20 s at most"""
    if os.environ.get("HOOKSHOT_LATEST_TAG"):   # (development: a made-up latest release, "none" for no answer)
        t = os.environ["HOOKSHOT_LATEST_TAG"]
        return None if t == "none" else t
    for attempt in range(3):
        try:
            req = urllib.request.Request(LATEST, headers={"User-Agent": "HookshotBuilder", "Accept": "application/vnd.github+json"})
            with urllib.request.urlopen(req, timeout=8) as r:
                return json.loads(r.read()).get("tag_name")
        except urllib.error.HTTPError as e:
            if e.code < 500:   # (the API's limit, 60 an hour a PC: the list below instead)
                break
        except (OSError, ValueError):
            pass
        time.sleep(2 << attempt)
    # (v2.0.0: when the API did not answer, the builder said a game was up to date. The release's setup list names its tag in its
    # first line, from GitHub's download service, not the API)
    for _ in range(2):
        try:
            req = urllib.request.Request(setup_parts.manifest_url(), headers={"User-Agent": "HookshotBuilder"})
            with urllib.request.urlopen(req, timeout=8) as r:
                for line in r.read().decode("utf-8", "replace").splitlines():
                    if line.startswith("tag "):
                        return line[4:].strip()
        except OSError:
            pass
        time.sleep(2)
    return None


def restart(args):
    """This app started again (on the version just installed: hookshot_builder_start.py picks it) with args, and this one closes"""
    exe, base_args = paths.launcher_command()
    subprocess.Popen([exe, *base_args, *args], cwd=os.path.dirname(exe))


class Install:
    """The release installed on a worker thread; ui is the window (note, download_progress, download_failed, and done(tag) called
    on the window's thread through ui.post)"""

    def __init__(self, ui, tag=None, then=None):
        self.ui, self.tag, self.then, self.cancelled = ui, tag, then, False

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()
        return self

    def cancel(self):
        self.cancelled = True

    def _run(self):
        post = self.ui.post
        try:
            tag, _ = setup_parts.install(lambda s: post(self.ui.note, s), lambda f: post(self.ui.download_progress, f) if f >= 0 else None,
                                         tag=self.tag, cancel=lambda: self.cancelled)
            post(self.then, tag)
        except setup_parts.SetupError as e:
            msg = str(e)
            post(self.ui.download_failed, "The download was cancelled. Your game folder is unchanged." if msg == "cancelled" else
                 msg + " Your game folder is unchanged.")
        except Exception as e:   # (anything else: said, not a silent window)
            post(self.ui.download_failed, "%s. Your game folder is unchanged." % e)


def update_folder(ui, folder, tag, fetched):
    """The game in folder built again with the latest release: here when these tools are it (or fetched), else after installing it
    (this app then starts again on it with --update folder --tag t --fetched)"""
    ui.begin_update(folder, tag)
    tools = Tools.find()
    # (just installed, but another release than asked for: GitHub's latest link lags right after a publish)
    if fetched and tag and tools and tools.version and tools.version != tag:
        ui.download_failed("The update installed %s, not %s: GitHub's download links can lag right after a release. Try Update again "
                           "in a few minutes." % (tools.version, tag))
        return
    if tools and (fetched or (tag and tools.version == tag)):
        ui.update_build(tools, folder)
        return
    ui.note("Asking GitHub for the latest release")

    def installed(new_tag):
        restart(["--update", folder, "--tag", new_tag, "--fetched"])
        ui.quit_app()
    ui.download = Install(ui, tag, installed).start()


def run_latest_setup(ui):
    """The latest release installed, then this app started again on it (the builder's own update)"""
    ui.note("Asking GitHub for the latest release")

    def installed(_tag):
        restart([])
        ui.quit_app()
    ui.download = Install(ui, None, installed).start()
