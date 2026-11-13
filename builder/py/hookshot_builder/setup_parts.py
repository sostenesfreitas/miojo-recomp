"""The setup's work (builder/setup/setup.c, the same rules): a release's hookshot-setup.txt names its tag and its parts (one zip each,
with its SHA-256 and size); each part is downloaded (or moved over from the installed version when its checksum did not change),
checked and unpacked into versions\\<tag>.part, which becomes versions\\<tag> once complete. The version installed stays, the others
and the downloads go.

hookshot-setup.txt, one item a line:
    tag <tag>
    part <sha256> <bytes> <folder under the install> <top folder in the zip to rename, or -> <new name, or -> <url>
    run <the WPF builder, relative to the install> (not needed here: the tools are what this app uses)"""
import hashlib
import json
import os
import re
import shutil
import time
import urllib.error
import urllib.request
import zipfile

from . import REPO, paths

UA = "HookshotBuilder-Setup/2.0"
WAITS = (3, 6, 12, 20, 30, 45, 60)   # (8 tries over ~3 minutes: GitHub's release downloads answered 504 for hours on 2026-11-08)
OWN = ("python", "emsdk", "cpm", "third_party", "wgpu-native")


class SetupError(Exception):
    pass


class Part:
    def __init__(self, sha, size, dest, frm, to, url):
        self.sha, self.size, self.dest, self.frm, self.to, self.url = sha, int(size), dest, frm, to, url


def retryable(status):
    """an answer worth trying again: none, a server error, a download cut off half way (200), a timeout or the API's rate limit"""
    return not status or status >= 500 or status in (200, 408, 429, 403)


class Http:
    def __init__(self):
        self.status = 0

    def get(self, url, sink=None, expect=0, accept=None, progress=None, cancel=None):
        """GET following redirects (never to http); the bytes to sink, or returned. Sets self.status; raises on failure"""
        if not url.startswith("https://"):
            raise SetupError("Only https downloads are allowed: " + url)
        req = urllib.request.Request(url, headers={"User-Agent": UA, **({"Accept": accept} if accept else {})})
        self.status = 0
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                if not r.geturl().startswith("https://"):
                    raise SetupError("A download was redirected away from https")
                self.status = r.status
                got, out = 0, []
                while True:
                    if cancel and cancel():
                        raise SetupError("cancelled")
                    b = r.read(1 << 16)
                    if not b:
                        break
                    got += len(b)
                    if sink:
                        sink(b)
                    else:
                        out.append(b)
                    if progress and expect:
                        progress(min(1.0, got / expect))
                if expect and got != expect:
                    self.status = 200   # (cut off half way: worth another try)
                    raise SetupError("The download ended early (%d of %d MB)" % (got >> 20, expect >> 20))
                return None if sink else b"".join(out)
        except urllib.error.HTTPError as e:
            self.status = e.code
            raise SetupError("GitHub returned HTTP %d" % e.code) from None
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            if not isinstance(e, SetupError):
                self.status = self.status if self.status and self.status != 200 else 0
                raise SetupError("Could not reach GitHub (%s)" % getattr(e, "reason", e)) from None
            raise


def api_link_for(http, url):
    """https://github.com/<owner>/<repo>/releases/download/<tag>/<name> (or .../latest/download/<name>) -> the API's own link to
    that file, which redirects to a fresh download (GitHub's cached release links answered 504 for hours while its API answered)"""
    m = re.match(r"https://github\.com/([^/]+/[^/]+)/releases/(?:download/([^/]+)|latest/download)/([^/?]+)$", url)
    if not m:
        return None
    repo, tag, name = m.groups()
    api = "https://api.github.com/repos/%s/releases/%s" % (repo, ("tags/" + tag) if tag else "latest")
    try:
        doc = json.loads(http.get(api, accept="application/vnd.github+json"))
        for a in doc.get("assets", []):
            if a.get("name") == name and str(a.get("url", "")).startswith("https://api.github.com/"):
                return a["url"]
    except (SetupError, ValueError):
        pass
    return None


def list_name_ok(s, path=False):
    """a name from the setup list (a tag, or with path a relative path with / or \\ between its parts): letters, digits and
    . _ + - only, no ".." part, no drive, no root"""
    if not re.fullmatch(r"[A-Za-z0-9._+\-/\\]+" if path else r"[A-Za-z0-9._+\-]+", s):
        return False
    names = re.split(r"[/\\]", s)
    if len(names) > 1 and not names[-1]:   # (as in setup.c: one separator at the end is no part)
        names.pop()
    return all(q not in ("", ".", "..") for q in names)


def list_ok(tag, parts):
    """(the list's names become paths under the install folder, a folder install() removes and remakes: they stay inside it)"""
    return list_name_ok(tag) and all(
        re.fullmatch(r"[0-9a-f]{64}", p.sha) and p.url[:8].lower() == "https://" and (p.dest == "." or list_name_ok(p.dest, True))
        and (p.frm == "-" or list_name_ok(p.frm)) and (p.to == "-" or list_name_ok(p.to)) for p in parts)


def manifest_url(tag=None):
    """(an update to a named release reads THAT release's list: the latest/download link can answer with the release before it for
    a while after a publish. HOOKSHOT_SETUP_LIST=<url>: another list, for testing a release before it is the latest)"""
    e = os.environ.get("HOOKSHOT_SETUP_LIST")
    if e:
        return e
    if tag:
        return "https://github.com/%s/releases/download/%s/hookshot-setup.txt" % (REPO, tag)
    return "https://github.com/%s/releases/latest/download/hookshot-setup.txt" % REPO


def read_manifest(status, tag=None, cancel=None):
    """(tag, parts) of the release's list, tried again on a server error or no answer"""
    http, url = Http(), manifest_url(tag)
    text, last = None, ""
    for attempt in range(len(WAITS) + 1):
        if attempt:
            status("GitHub didn't answer (%s). Trying again in %d s..." % (http.status or "no answer", WAITS[attempt - 1]))
            time.sleep(WAITS[attempt - 1])
            status("Looking for the latest Hookshot Builder...")
        try:
            text = http.get(url, cancel=cancel).decode("utf-8", "replace")
            break
        except SetupError as e:
            last = str(e)
            first = http.status
            if not first or first >= 500:   # (the API's link instead)
                api = api_link_for(http, url)
                if api:
                    try:
                        text = http.get(api, accept="application/octet-stream", cancel=cancel).decode("utf-8", "replace")
                        break
                    except SetupError:
                        http.status = first
            if not retryable(http.status):
                break
    if text is None:
        if http.status == 404:
            raise SetupError("The latest Hookshot release has no setup files yet. Download it from the releases page for now: "
                             "https://github.com/%s/releases/latest" % REPO)
        if http.status:
            raise SetupError("GitHub's download servers are failing right now (an error 500 or above, tried 8 times over 3 minutes). "
                             "This is on GitHub's side: try again in a few minutes, or later today.")
        raise SetupError("Could not reach GitHub to get the Hookshot Builder (%s). Check your internet connection and try again." % last)
    got_tag, parts = "", []
    for line in text.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] == "tag":
            got_tag = p[1]
        elif len(p) == 7 and p[0] == "part":
            if not re.fullmatch(r"[0-9]+", p[2]):
                raise SetupError("The release's setup list could not be read. Try again later.")
            parts.append(Part(*p[1:]))
    if not got_tag or not parts:
        raise SetupError("The release's setup list could not be read. Try again later.")
    if not list_ok(got_tag, parts):
        raise SetupError("The release's setup list has a name this setup does not accept. Try again later.")
    if tag and tag != got_tag:
        raise SetupError("GitHub answered with the setup list of %s, not %s (its download links can lag right after a release). "
                         "Try the update again in a few minutes." % (got_tag, tag))
    return got_tag, parts


def part_folder(p):
    """The folder a part unpacks into, relative to a version's folder (None: the part has none of its own)"""
    if p.to != "-" and p.dest != ".":
        return os.path.join(p.dest, p.to).replace("/", os.sep)
    b = p.url.rsplit("/", 1)[-1]
    for own in OWN:
        if b.startswith("HookshotBuilder-%s-" % own):
            return os.path.join("HookshotBuilder", "tools", own)
    return None


def reuse_part(tag, p, install):
    """A part whose checksum the installed version lists in its parts.txt (and whose folder it has) is moved over from it instead of
    downloaded: that version is deleted after the install anyway"""
    rel = part_folder(p)
    was = paths.read_small(os.path.join(paths.versions(), "current.txt"))
    if not rel or not was or was == tag or not list_name_ok(was):
        return False
    if p.sha not in paths.read_small(os.path.join(paths.versions(), was, "parts.txt")).split():
        return False
    src, dst = os.path.join(paths.versions(), was, rel), os.path.join(install, rel)
    if not os.path.exists(src):
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        os.replace(src, dst)
        return True
    except OSError:
        return False


def rmtree(p):
    shutil.rmtree(p, ignore_errors=True)


def installed(tag):
    d = os.path.join(paths.versions(), tag)
    return d if os.path.isfile(os.path.join(d, "installed.txt")) and os.path.isfile(
        os.path.join(d, "HookshotBuilder", "tools", "nx2", "make_game.py")) else None


def current():
    """The version installed last, its folder (None: none yet)"""
    tag = paths.read_small(os.path.join(paths.versions(), "current.txt"))
    return installed(tag) if tag else None


def install(status, progress=lambda f: None, tag=None, repair=False, cancel=None):
    """Installs the release (the latest, or tag) unless it is there already; returns (tag, its folder)"""
    tag, parts = read_manifest(status, tag, cancel)
    versions, downloads = paths.versions(), os.path.join(paths.base(), "downloads")
    final = os.path.join(versions, tag)
    if repair or not installed(tag):
        work = final + ".part"
        rmtree(work)
        rmtree(final)
        os.makedirs(work, exist_ok=True)
        os.makedirs(downloads, exist_ok=True)
        n = len(parts)
        for i, p in enumerate(parts, 1):
            if cancel and cancel():
                raise SetupError("cancelled")
            if reuse_part(tag, p, work):
                status("Part %d of %d unchanged: kept from the installed version" % (i, n))
                continue
            zpath = os.path.join(downloads, p.sha + ".zip")
            if not (os.path.isfile(zpath) and sha256_file(zpath) == p.sha):
                download_part(status, progress, p, i, n, zpath, cancel)
            status("Unpacking part %d of %d..." % (i, n))
            progress(-1)
            dest = work if p.dest == "." else os.path.join(work, p.dest.replace("/", os.sep))
            os.makedirs(dest, exist_ok=True)
            try:
                with zipfile.ZipFile(zpath) as z:
                    z.extractall(dest)
            except (OSError, zipfile.BadZipFile) as e:
                raise SetupError("Unpacking part %d of %d failed: %s" % (i, n, e)) from None
            if p.frm != "-" and p.to != "-":   # (llvm-mingw-<version>-ucrt-x86_64 -> llvm-mingw)
                src, dst = os.path.join(dest, p.frm), os.path.join(dest, p.to)
                if not os.path.exists(dst):
                    try:
                        os.replace(src, dst)
                    except OSError:
                        raise SetupError("Could not set up part %d of %d (renaming %s)." % (i, n, p.frm)) from None
        if not os.path.isfile(os.path.join(work, "HookshotBuilder", "tools", "nx2", "make_game.py")):
            raise SetupError("The build tools are missing after unpacking. Your antivirus may have removed them: allow the folder %s "
                             "and try again." % paths.base())
        try:
            os.replace(work, final)
        except OSError:
            raise SetupError("Could not finish installing into %s." % final) from None
        paths.write_small(os.path.join(final, "installed.txt"), tag)
        write_parts(final, parts)
    elif not os.path.isfile(os.path.join(final, "parts.txt")):
        write_parts(final, parts)
    prune(tag, parts)
    rmtree(downloads)   # (installed: its zips are not kept; an update moves unchanged folders over instead)
    return tag, final


def download_part(status, progress, p, i, n, zpath, cancel):
    http, part = Http(), zpath + ".part"
    for attempt in range(len(WAITS) + 1):
        if attempt:
            status("Part %d of %d: GitHub didn't answer (%s). Trying again in %d s..." % (i, n, http.status or "no answer",
                                                                                          WAITS[attempt - 1]))
            time.sleep(WAITS[attempt - 1])
        status("Downloading part %d of %d (%d MB)..." % (i, n, p.size >> 20))
        h = hashlib.sha256()
        try:
            with open(part, "wb") as f:
                def sink(b):
                    f.write(b)
                    h.update(b)
                url = api_link_for(http, p.url) if attempt % 2 == 1 else None   # (the API's link, every other try)
                if url:
                    http.get(url, sink, p.size, "application/octet-stream", progress, cancel)
                else:
                    http.get(p.url, sink, p.size, None, progress, cancel)
        except SetupError as e:
            if str(e) == "cancelled":
                raise
            if not retryable(http.status):
                break
            continue
        except OSError:
            raise SetupError("Could not write to %s." % os.path.dirname(zpath)) from None
        progress(-1)
        if h.hexdigest() != p.sha:
            os.remove(part)
            raise SetupError("Part %d of %d did not download correctly (its checksum differs). Try again." % (i, n))
        os.replace(part, zpath)
        return
    try:
        os.remove(part)
    except OSError:
        pass
    raise SetupError(("Downloading part %d of %d failed: GitHub's download servers are failing right now (tried 8 times over 3 "
                      "minutes). This is on GitHub's side. The parts already downloaded are kept: try again in a few minutes."
                      if retryable(http.status) else
                      "Downloading part %d of %d failed. Check your connection and free space, then try again.") % (i, n))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def write_parts(install_dir, parts):
    """The checksums of the parts this version holds in folders of their own (reuse_part reads them at the next update)"""
    paths.write_small(os.path.join(install_dir, "parts.txt"), "".join(p.sha + "\n" for p in parts if part_folder(p)))


def prune(tag, parts):
    """Only the version installed stays (a folder still in use cannot be renamed: it goes at a later run), and downloads the list
    no longer names go"""
    versions = paths.versions()
    was = paths.read_small(os.path.join(versions, "current.txt"))
    if was != tag:
        if was:
            paths.write_small(os.path.join(versions, "previous.txt"), was)
        paths.write_small(os.path.join(versions, "current.txt"), tag)
    for name in os.listdir(versions):
        p = os.path.join(versions, name)
        if not os.path.isdir(p) or name == tag:
            continue
        if name.endswith(".gone"):
            rmtree(p)
            continue
        try:
            os.replace(p, p + ".gone")
            rmtree(p + ".gone")
        except OSError:
            pass
