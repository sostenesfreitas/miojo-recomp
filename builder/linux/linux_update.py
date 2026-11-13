"""Linux builder updates, the same way Windows installs (builder/setup/setup.c, scripts/make_setup_parts.py).

A release publishes hookshot-linux.txt. Each part is a zip named by its SHA-256 and kept on the linux-parts release, so a
part whose files did not change is not downloaded again. The player's copy lives at
$XDG_DATA_HOME/hookshot/builder/versions/<tag> (AppRun at its root, the same layout as the AppImage). An unchanged part
is copied from the builder that is running now (the mounted AppImage, or the version installed last time).

    tag <tag>
    part <sha256> <bytes> <dir under the install> <url>
    tree                         the whole directory
    file <name>                  one file in that directory (repeat; instead of tree)
"""
import hashlib
import os
import re
import shutil
import stat
import time
import urllib.error
import urllib.request
import zipfile

from part_links import SYSROOT_PART, is_absolute_link, relative_sysroot_link

LIST_NAME = "hookshot-linux.txt"
LATEST_LIST = "https://github.com/NotRexed/Hookshot/releases/latest/download/" + LIST_NAME
LATEST_API = "https://api.github.com/repos/NotRexed/Hookshot/releases/latest"
RELEASES_API = "https://api.github.com/repos/NotRexed/Hookshot/releases?per_page=20"
# (the update tracks: the builder's Track choice, by default the track of the build installed. The game's own check,
# host/hos_update.c, and the Windows builder, Updater.cs, use the same rules)
TRACKS = ("release", "rc", "nightly")


class Part(object):
    def __init__(self, sha, size, dest, url, files):
        self.sha, self.size, self.dest, self.url, self.files = sha, size, dest, url, files  # files None: the whole directory

    def same(self, other):
        return other is not None and other.sha == self.sha and other.dest == self.dest and other.files == self.files


class Manifest(object):
    def __init__(self, tag, parts):
        self.tag, self.parts = tag, parts

    def by_sha(self, sha):
        for part in self.parts:
            if part.sha == sha:
                return part
        return None


def data_root():
    base = os.environ.get("HOOKSHOT_BUILDER_HOME") or os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "hookshot", "builder")


def parse_list(text, local=True):
    """The setup list. A part owns the following tree line, or the file lines, until the next part. local: a list from
    this PC (the tools' own, HOOKSHOT_LINUX_LIST), whose parts may be file:// links; a downloaded one's are https"""
    tag, parts, cur = "", [], None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("tag "):
            tag = line[4:].strip()
        elif line.startswith("part "):
            bits = line.split(" ")
            if len(bits) != 5:
                raise ValueError("a part line needs sha, size, directory and url")
            cur = Part(bits[1], int(bits[2]), bits[3], bits[4], [])
            parts.append(cur)
        elif line == "tree":
            if cur is None:
                raise ValueError("tree with no part")
            cur.files = None
        elif line.startswith("file "):
            if cur is None or cur.files is None:
                raise ValueError("file with no part")
            cur.files.append(line[5:].strip())
        else:
            raise ValueError("unknown list line")
    if not tag or not parts:
        raise ValueError("the setup list has no release")
    # (the tag names a folder under versions/ that an update removes and remakes, and a part's names are paths in it:
    # they stay inside it)
    if not re.fullmatch(r"[0-9A-Za-z._+-]{1,128}", tag) or tag in (".", ".."):
        raise ValueError("the setup list's tag is not a version name")
    for part in parts:
        if not re.fullmatch(r"[0-9a-f]{64}", part.sha) or not part.url.startswith(("https://", "file://") if local else "https://"):
            raise ValueError("a part line has a bad checksum or link")
        if part.dest != ".":
            _safe_name(part.dest)
        for name in part.files or ():
            _safe_name(name)
    return Manifest(tag, parts)


def release_key(tag):
    """A version to compare: a release tag (v2.0.5, v2.1.0-rc.1) or git describe's v2.0.5-3-g1a2b3c4[-dirty] (a build from
    the Actions tab or a checkout: after v2.0.5, before any later release). None for a build that names no release: a bare
    commit (digits alone too: no v and no dot), dev, or no version at all. Prereleases in semver order: v2.1.0-rc.1 <
    v2.1.0-rc.2 < v2.1.0."""
    m = re.match(r"^(?=v|\d+\.)v?(\d+(?:\.\d+)*)(?:-(?!\d+-g|dirty$)([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?(-\d+-g[0-9a-f]+)?(-dirty)?$",
                 (tag or "").strip())
    if not m:
        return None
    pre = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in (m.group(2) or "").split(".") if p)
    # (a prerelease before its release; commits after a tag, or changes to it, after that tag and before the next)
    return tuple(int(n) for n in m.group(1).split(".")), m.group(2) is None, pre, bool(m.group(3) or m.group(4))


def track_of(version):
    """The track a build is on, from its version: "nightly" for git describe's v2.1.0-rc.1-3-g1a2b3c4 (the nightly is
    named with --long, so even one built on a tagged commit says so), "rc" for a release candidate (v2.1.0-rc.1), else
    "release" (a release, and anything that names no release)."""
    v = (version or "").strip()
    if re.search(r"-\d+-g[0-9a-f]+(-dirty)?$", v):
        return "nightly"
    key = release_key(v)
    return "rc" if key and not key[1] else "release"


def release_of(tag):
    """The GitHub release that holds a version's files: the nightly's are on the rolling nightly release."""
    return "nightly" if track_of(tag) == "nightly" else tag


def list_url(tag=None):
    if not tag:
        return LATEST_LIST
    return "https://github.com/NotRexed/Hookshot/releases/download/%s/%s" % (release_of(tag), LIST_NAME)


def newest_release(tags):
    """The newest of a list of release tags (stable and release candidates), in semver order; None when none is one."""
    tags = [t for t in tags if t and re.fullmatch(r"v\d+\.\d+\.\d+(-rc\.\d+)?", t)]
    return max(tags, key=release_key) if tags else None


def update_target(latest, tools, switch=False):
    """The newest version this builder knows of: the latest release when it is newer than these tools, else the tools'
    own. (issue 902) Any version other than the latest release was made to update before it could build, so a test
    build from the Actions tab, newer than the release, was taken back to it. One made after an older release is still
    offered the newer one. latest is its track's newest (latest_tag): a nightly is taken whenever it is another build,
    and switch (a Track other than these tools' own was chosen) takes that track's newest even when it is older."""
    if not latest or not tools:
        return latest or tools
    if switch or track_of(latest) == "nightly":
        return latest
    have, out = release_key(tools), release_key(latest)
    return latest if have and out and out > have else tools


def older_version(target, have):
    """Known version order only: arbitrary hashes and differing nightlies cannot establish commit ancestry."""
    old, new = release_key(have), release_key(target)
    return bool(old and new and new < old)


def choose_mode(game, tools, latest, source_picked, kept, switch=False):
    """What the builder's button does: an older game that still has its files is updated, a game at or beyond the
    newest known release is played unless its track is explicitly switched, and an older builder updates
    itself before a new build. latest None: GitHub did not answer, so the builder's own version is the newest known.
    Newest is update_target's: a builder that is not an older release is not made to update."""
    target = update_target(latest, tools, switch)
    if kept and game and target and game != target and not source_picked and (switch or not older_version(target, game)):
        return "update-game"
    if game and not source_picked:
        return "up-to-date"
    if latest and tools and target != tools:
        return "update-builder"
    return "build"


def tools_tag(root):
    """The release these tools are, from the file a package writes (VERSION, or version.txt). A source checkout has neither."""
    if not root:
        return ""
    for rel in ("usr/share/hookshot/VERSION", "usr/share/hookshot/version.txt", "VERSION", "version.txt"):
        path = os.path.join(root, rel)
        try:
            with open(path, encoding="utf-8") as f:
                line = f.readline().strip()
        except OSError:
            continue
        if line:
            return line
    return ""


def running_root():
    """The AppDir that is running: the AppImage's mount, or an installed version (its AppRun beside usr/)."""
    appdir = os.environ.get("APPDIR")
    if appdir and os.path.isfile(os.path.join(appdir, "AppRun")):
        return appdir
    tools = os.environ.get("HOOKSHOT_TOOLS")
    if tools:
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(tools))))   # (<root>/usr/share/hookshot)
        if os.path.isfile(os.path.join(root, "AppRun")):
            return root
    return None


def read_manifest(root):
    path = os.path.join(root, LIST_NAME) if root else ""
    try:
        with open(path, encoding="utf-8") as f:
            return parse_list(f.read())
    except (OSError, ValueError):
        return None


def game_version(folder):
    """The release a folder was built with, when it still has the game files an update builds from."""
    if not folder:
        return None, False
    kept = os.path.isdir(os.path.join(folder, ".hookshot_exefs")) and os.path.isdir(os.path.join(folder, "romfs"))
    version = None
    try:
        with open(os.path.join(folder, "hookshot_version.txt"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("version="):
                    version = line[8:].strip() or None
                    break
    except OSError:
        pass
    return version, kept


def game_exe(folder):
    """bin/hookshot under the launcher, or hookshot in a folder built before the launcher."""
    if not folder:
        return None
    nested = os.path.join(folder, "bin", "hookshot")
    if os.path.isfile(nested) and os.path.isfile(os.path.join(folder, "image.bin")):
        return nested
    flat = os.path.join(folder, "hookshot")
    if os.path.isfile(flat) and os.path.isfile(os.path.join(folder, "image.bin")):
        return flat
    return None


MAX_DOWNLOAD = 4 << 30   # (no part or list is this big: a download that is stops there)


class Cancelled(Exception):
    """The player stopped the update (the builder's Cancel sets the stop event a download is given). It is not tried again,
    and install removes what it had staged, as for any other error."""

    def __init__(self):
        super().__init__("The update was cancelled.")


def _stopped(stop):
    if stop is not None and stop.is_set():
        raise Cancelled()


def _read_at_most(response, limit, stop=None):
    if stop is None:
        data = response.read(limit + 1)
    else:   # (a megabyte at a time, stop looked at before each: one read of a part is hundreds of them)
        pieces, size = [], 0
        while size <= limit:
            _stopped(stop)
            piece = response.read(min(1 << 20, limit + 1 - size))
            if not piece:
                break
            pieces.append(piece)
            size += len(piece)
        data = b"".join(pieces)
    if len(data) > limit:
        raise RuntimeError("the download is larger than expected")
    return data


def _https(url):
    """Only https links are fetched (never file:, ftp: or a plain http: link from a release's answer)."""
    if not isinstance(url, str) or not url.startswith("https://"):
        raise ValueError("not an https link: %r" % (url,))
    return url


class _HttpsRedirects(urllib.request.HTTPRedirectHandler):
    """A redirect is followed only to another https link. urllib's own follows one to http: and ftp: too, so _https saw the
    first link and not where the download came from; the Python builder for Windows refuses the same (setup_parts.py)."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return super().redirect_request(req, fp, code, msg, headers, _https(newurl))


_OPENER = urllib.request.build_opener(_HttpsRedirects)


def _open(req, timeout):
    return _OPENER.open(req, timeout=timeout)


def _request(url, timeout, limit=MAX_DOWNLOAD, stop=None):
    # (no second argument: Request's second is the POST body, and a str body failed every download before connecting)
    req = urllib.request.Request(_https(url), headers={"User-Agent": "Hookshot", "Accept": "application/vnd.github+json"})
    with _open(req, timeout) as response:
        return _read_at_most(response, limit, stop)


def _api_download(url, stop=None):
    """A GitHub release link that failed: the same file through the API (scripts/linux/deps.sh does this for 504s)."""
    prefix = "https://github.com/"
    mark = "/releases/download/"
    if not url.startswith(prefix) or mark not in url:
        return None
    repo, _, rest = url[len(prefix):].partition(mark)
    tag, _, name = rest.partition("/")
    if not repo or not tag or not name or "/" in name:
        return None
    info = _request("https://api.github.com/repos/%s/releases/tags/%s" % (repo, tag), 30)
    import json
    for asset in json.loads(info.decode("utf-8")).get("assets", []):
        if asset.get("name") == name:
            req = urllib.request.Request(_https(asset.get("url")), headers={
                "User-Agent": "Hookshot", "Accept": "application/octet-stream"})
            with _open(req, 120) as response:
                return _read_at_most(response, MAX_DOWNLOAD, stop)
    return None


def fetch_bytes(url, timeout=60, stop=None):
    """The url's bytes. A GitHub download that the server fails (5xx) is tried again, then through the API. stop: an event
    (threading.Event) that ends it with Cancelled once set, during a read or a wait between tries."""
    delay, last = 2, "the download failed"
    for attempt in range(4):
        try:
            return _request(url, timeout, stop=stop)
        except Cancelled:
            raise
        except urllib.error.HTTPError as error:
            last = "GitHub answered %s" % error.code
            if error.code < 500 and error.code != 429:
                break
        except Exception as error:
            last = str(error) or error.__class__.__name__
        if attempt < 3:
            if stop is None:
                time.sleep(delay)
            else:
                stop.wait(delay)
                _stopped(stop)
            delay = min(delay * 2, 20)
    try:
        got = _api_download(url, stop)
    except Cancelled:
        raise
    except Exception as error:
        got = None
        last = str(error) or last
    if got is not None:
        return got
    raise RuntimeError(last)


def latest_tag(get=None, track="release"):
    """The newest build of a track, or None when GitHub did not answer. release: the latest release's tag, the API first
    (60 an hour), the setup list's tag otherwise, which is a file download and not that limit (the same fallback as the
    Windows builder). rc: the newest of the release candidates and the releases (an RC is offered its version's release
    once that ships), the latest release when the API does not answer. nightly: the nightly's list's tag."""
    get = get or (lambda url: fetch_bytes(url, 8))
    if track == "nightly":
        try:
            return parse_list(get(list_url("nightly")).decode("utf-8"), local=False).tag
        except Exception:
            return None
    if track == "rc":
        try:
            import json
            newest = newest_release([r.get("tag_name") for r in json.loads(get(RELEASES_API).decode("utf-8"))
                                     if not r.get("draft")])
            if newest:
                return newest
        except Exception:
            pass
    try:
        import json
        body = json.loads(get(LATEST_API).decode("utf-8"))
        tag = body.get("tag_name")
        if tag:
            return tag
    except Exception:
        pass
    try:
        text = get(LATEST_LIST).decode("utf-8")
        return parse_list(text, local=False).tag
    except Exception:
        return None


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _clone(src, dst, root, rooted_links=None):
    """A copy of src at dst. A hard link when the two are on one disk (the installed copy); a real copy from the
    AppImage's mount, which does not link."""
    if os.path.islink(src):
        link_to = os.readlink(src)
        if rooted_links is not None and is_absolute_link(link_to):
            rooted_links.append((dst, link_to))
        else:
            _symlink(root, link_to, dst)
        return
    try:
        os.link(src, dst)
        return
    except OSError:
        shutil.copy2(src, dst)


def _clone_tree(src, dst, rebase_sysroot=False, root=None):
    root = dst if root is None else root
    rooted_links = [] if rebase_sysroot else None
    for dirpath, dirnames, filenames in os.walk(src, followlinks=False):
        rel = os.path.relpath(dirpath, src)
        out = dst if rel == os.curdir else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        kept = []
        for name in dirnames:
            link = os.path.join(dirpath, name)
            if os.path.islink(link):
                _clone(link, os.path.join(out, name), root, rooted_links)
            else:
                kept.append(name)
        dirnames[:] = kept
        for name in filenames:
            _clone(os.path.join(dirpath, name), os.path.join(out, name), root, rooted_links)
    if rooted_links is not None:
        _finish_sysroot_links(dst, rooted_links)


def _safe_name(name):
    if not name or name.startswith("/") or ".." in name.split("/"):
        raise ValueError("the zip has a path outside its folder")
    return name


def _check_part_directory(path):
    if os.path.realpath(path) != os.path.abspath(path):
        raise ValueError("the update would write through a symbolic link: %s" % path)


def _symlink(root, link_to, target):
    # The whole staging tree is the boundary: a valid toolchain link may name a different part or a later entry.
    root = os.path.abspath(root)
    # (".." only at the start: "x/../.." is checked as written, but the system resolves x first, and x may be a link the
    # same zip or a later part makes, so the check and the system would disagree)
    parts = link_to.split("/")
    ups = 0
    while ups < len(parts) and parts[ups] == "..":
        ups += 1
    if ".." in parts[ups:]:
        raise ValueError("an update symlink climbs after a name: %s -> %s" % (target, link_to))
    destination = os.path.abspath(os.path.join(os.path.dirname(target), link_to))
    real_root = os.path.realpath(root)
    if (is_absolute_link(link_to) or os.path.commonpath((root, destination)) != root or
            os.path.commonpath((real_root, os.path.realpath(destination))) != real_root):
        raise ValueError("an update symlink leaves its staging folder: %s -> %s" % (target, link_to))
    os.symlink(link_to, target)


def _finish_sysroot_links(dest, rooted_links):
    while rooted_links:
        pending = []
        for target, link_to in rooted_links:
            try:
                relative = relative_sysroot_link(dest, target, link_to)
            except FileNotFoundError:
                pending.append((target, link_to))
                continue
            _check_part_directory(os.path.dirname(target))
            os.symlink(relative, target)
        if len(pending) == len(rooted_links):
            raise ValueError("sysroot links have missing or circular targets: %s" %
                             ", ".join("%s -> %s" % (target, link_to) for target, link_to in pending))
        rooted_links = pending


def _unpack(data, dest, files, rebase_sysroot=False, root=None):
    root = dest if root is None else root
    os.makedirs(dest, exist_ok=True)
    rooted_links = []
    with zipfile.ZipFile(__import__("io").BytesIO(data)) as zipped:
        for info in zipped.infolist():
            name = _safe_name(info.filename)
            if not name or name.endswith("/"):
                continue
            if files is not None and name not in files:
                raise ValueError("the zip has %s, which this part does not list" % name)
            target = os.path.join(dest, name)
            parent = os.path.dirname(target)
            _check_part_directory(parent)
            os.makedirs(parent, exist_ok=True)
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode):
                link_to = zipped.read(info).decode("utf-8")
                # Released sysroots contain /lib links rooted inside the sysroot; other parts must reject absolute links.
                if is_absolute_link(link_to):
                    if not rebase_sysroot:
                        raise ValueError("a zip symlink has an absolute target: %s -> %s" % (name, link_to))
                    rooted_links.append((target, link_to))
                else:
                    _symlink(root, link_to, target)
            else:
                # A part can ship symlinks, but cannot overwrite one (or any earlier entry).
                with open(target, "xb") as out:
                    out.write(zipped.read(info))
                if mode & 0o777:
                    # (the modes the parts are made with: no group- or world-writable files from a zip)
                    os.chmod(target, 0o755 if mode & 0o111 else 0o644)
    _finish_sysroot_links(dest, rooted_links)


def _place(part, source_root, source, dest_root, get, note):
    # Resolve installation parent links without following the staging directory itself.
    parent, staging = os.path.split(os.path.abspath(dest_root))
    staging_root = os.path.join(os.path.realpath(parent), staging)
    dest = os.path.join(staging_root, _safe_name(part.dest))
    _check_part_directory(dest)
    old = source.by_sha(part.sha) if source else None
    src = os.path.join(source_root, part.dest) if old and old.same(part) and source_root else ""
    ready = src and os.path.isdir(src) and (part.files is None or all(
        os.path.lexists(os.path.join(src, name)) for name in part.files))
    rebase_sysroot = part.dest == SYSROOT_PART
    if ready:
        note("Keeping %s (unchanged)" % part.dest)
        if part.files is None:
            _clone_tree(src, dest, rebase_sysroot, staging_root)
        else:
            os.makedirs(dest, exist_ok=True)
            rooted_links = [] if rebase_sysroot else None
            for name in part.files:
                _clone(os.path.join(src, name), os.path.join(dest, name), staging_root, rooted_links)
            if rooted_links is not None:
                _finish_sysroot_links(dest, rooted_links)
        return
    note("Downloading %s (%d MB)" % (part.dest, part.size >> 20))
    data = get(part.url)
    if len(data) != part.size or _sha256(data) != part.sha:
        raise RuntimeError("%s did not download correctly (its checksum differs)" % part.dest)
    _unpack(data, dest, part.files, rebase_sysroot, staging_root)


def _check_links_inside(dest):
    """Every link in the finished staging tree resolves inside it, whatever order the parts made them in."""
    real = os.path.realpath(dest)
    for dirpath, dirnames, filenames in os.walk(dest, followlinks=False):
        for name in dirnames + filenames:
            path = os.path.join(dirpath, name)
            if os.path.islink(path) and os.path.commonpath((real, os.path.realpath(path))) != real:
                raise ValueError("an update symlink leaves its staging folder: %s" % path)


def _finish_tree(dest):
    """AppRun at the root, from the tools just installed, so this folder starts the way the AppImage does."""
    _check_links_inside(dest)
    for name in ("AppRun", "hookshot.png", "hookshot-builder.desktop"):
        if os.path.islink(os.path.join(dest, name)):   # (written below: never through a link a part made)
            raise ValueError("the update has a link where %s goes" % name)
    tools = os.path.join(dest, "usr", "share", "hookshot")
    apprun = os.path.join(dest, "AppRun")
    shutil.copyfile(os.path.join(tools, "builder", "linux", "AppRun"), apprun)
    os.chmod(apprun, 0o755)   # (rwxr-xr-x: the launcher must run; only its owner may change it)
    icon = os.path.join(tools, "assets", "hookshot.png")
    if os.path.isfile(icon):
        shutil.copyfile(icon, os.path.join(dest, "hookshot.png"))
    desktop = os.path.join(tools, "builder", "linux", "hookshot-builder.desktop")
    if os.path.isfile(desktop):
        shutil.copyfile(desktop, os.path.join(dest, "hookshot-builder.desktop"))
    return apprun


def install(manifest, source_root, source, note=None, get=None, stop=None):
    """versions/<tag> from the list. Unchanged parts come from source_root. The new folder replaces nothing until it
    is complete, and the version installed before stays until this one is. stop: an event that ends it with Cancelled once
    set, in a download or between two parts, up to the moment the new folder is complete."""
    note = note or (lambda message: None)
    get = get or (lambda url: fetch_bytes(url, stop=stop))
    root = data_root()
    versions = os.path.join(root, "versions")
    dest = os.path.join(versions, manifest.tag)
    partial = dest + ".part"
    if os.path.islink(partial):
        raise ValueError("the update staging folder is a symbolic link: %s" % partial)
    if os.path.isdir(partial):
        shutil.rmtree(partial)
    os.makedirs(versions, exist_ok=True)
    try:
        for part in manifest.parts:
            _stopped(stop)
            _place(part, source_root, source, partial, get, note)
        # The publisher keeps the list outside all hashed parts. Never follow or overwrite a part-supplied leaf.
        with open(os.path.join(partial, LIST_NAME), "x", encoding="utf-8", newline="\n") as handle:
            handle.write("tag %s\n" % manifest.tag)
            for part in manifest.parts:
                handle.write("part %s %d %s %s\n" % (part.sha, part.size, part.dest, part.url))
                handle.write("tree\n" if part.files is None else "".join("file %s\n" % name for name in part.files))
        apprun = _finish_tree(partial)
        _stopped(stop)   # (the last moment: from here the new version takes the old one's place)
    except Exception:
        shutil.rmtree(partial, ignore_errors=True)
        raise
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.rename(partial, dest)
    current = os.path.join(root, "current")
    tmp = current + ".new"
    with open(tmp, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(manifest.tag + "\n")
    os.replace(tmp, current)
    for name in os.listdir(versions):
        if name != manifest.tag:
            shutil.rmtree(os.path.join(versions, name), ignore_errors=True)
    return os.path.join(dest, "AppRun") if os.path.isfile(os.path.join(dest, "AppRun")) else apprun


def installed_apprun():
    root = data_root()
    try:
        with open(os.path.join(root, "current"), encoding="utf-8") as handle:
            tag = handle.read().strip()
    except OSError:
        return ""
    path = os.path.join(root, "versions", tag, "AppRun")
    return path if tag and os.path.isfile(path) else ""


def fetch_manifest(get=None, tag=None):
    get = get or (lambda url: fetch_bytes(url, 30))
    override = os.environ.get("HOOKSHOT_LINUX_LIST")
    if override:
        with open(override, encoding="utf-8") as handle:
            return parse_list(handle.read())
    return parse_list(get(list_url(tag)).decode("utf-8"), local=False)


def startup_manifest(root, get=None, tag=None):
    """The list begin_update installs on launch: the latest release, except when these tools ship a list of their own
    that is that same release and no older than it (a self-contained -rc or Actions tab AppImage). Reinstalling an
    older release's parts over those tools replaces files the working tools had (issue 990: a -rc shipped
    gpu/vk/oot_cheat_items.h, the release's zip did not, and every launch lost it). The comparison is update_target's:
    a builder that is not an older release still updates. A HOOKSHOT_LINUX_LIST is a deliberate override: it wins. tag:
    that version's list instead (another track's, or the chosen track's newest), as asked."""
    latest = fetch_manifest(get, tag)
    if not tag and not os.environ.get("HOOKSHOT_LINUX_LIST"):
        own = read_manifest(root)
        if own and own.tag == tools_tag(root) and update_target(latest.tag, own.tag) == own.tag:
            return own
    return latest
