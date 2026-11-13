"""Linux updates download a part only when its checksum changed, and the launcher starts the installed builder.

python -I -B tests/test_linux_update.py
"""
import io
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
import zipfile
from pathlib import Path

# (Linux only)
RUN_ALL_SKIP = {"windows": "the Linux builder's update and folder launcher, POSIX files and shell scripts"}

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder" / "linux"))
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "scripts" / "linux"))
import linux_update  # noqa: E402
import make_parts  # noqa: E402


def sysroot_fixture(root):
    source = root / "usr" / "lib" / "hookshot" / "sysroot"
    lib = source / "usr" / "lib" / "x86_64-linux-gnu"
    lib.mkdir(parents=True)
    (source / "lib").symlink_to("usr/lib")
    (lib / "libmvec.so").symlink_to("/lib/x86_64-linux-gnu/libmvec.so.1")
    (lib / "libmvec.so.1").write_bytes(b"contents")
    return source


def zip_link(zipped, name, target):
    info = zipfile.ZipInfo(name)
    info.create_system = 3
    info.external_attr = (stat.S_IFLNK | 0o777) << 16
    zipped.writestr(info, target)


def appdir(path):
    """A tiny AppDir: one file in each part, plus the launcher script an install copies to AppRun."""
    tools = path / "usr" / "share" / "hookshot"
    (tools / "builder" / "linux").mkdir(parents=True)
    shutil.copyfile(ROOT / "builder" / "linux" / "AppRun", tools / "builder" / "linux" / "AppRun")
    (tools / "VERSION").write_text("v1\n", encoding="utf-8")
    (tools / "nx2").mkdir()
    (tools / "nx2" / "marker.py").write_text("print('one')\n", encoding="utf-8")
    for dest in ("usr/python", "usr/lib/hookshot/emsdk", "usr/lib/hookshot/ffmpeg", "usr/lib/hookshot/sysroot"):
        folder = path / dest
        folder.mkdir(parents=True)
        (folder / "kept").write_text(dest + "\n", encoding="utf-8")
    lib = path / "usr" / "lib" / "hookshot"
    (lib / "hookshot_vk.so").write_bytes(b"vk")
    (lib / "libSDL2-2.0.so.0").write_bytes(b"sdl")
    (path / "AppRun").write_text("#!/bin/sh\n", encoding="utf-8")
    (path / "AppRun").chmod(0o755)


class UpdateTests(unittest.TestCase):
    def test_relative_links_and_final_list_stay_inside_the_staging_tree(self):
        from unittest import mock
        with tempfile.TemporaryDirectory(prefix="hookshot-link-bounds-") as tmp:
            root = Path(tmp).resolve()
            cases = (
                ("download-manifest-escape", False, linux_update.LIST_NAME, "../canary", False),
                ("reuse-manifest-escape", True, linux_update.LIST_NAME, "../canary", False),
                ("download-unused-escape", False, "alias", "../canary", False),
                ("reuse-unused-escape", True, "alias", "../canary", False),
                ("reuse-listed-escape", True, "alias", "../canary", False),
                ("reuse-absolute-escape", True, "alias", None, False),
                ("part-supplied-list", False, linux_update.LIST_NAME, None, False),
                ("internal-list-alias", False, linux_update.LIST_NAME, "inside", False),
                ("download-cross-part", False, "alias", "usr/lib/target", True),
                ("reuse-cross-part", True, "alias", "usr/lib/target", True),
                ("download-forward-link", False, "alias", "inside", True),
                ("reuse-listed-internal", True, "alias", "inside", True),
            )
            for label, reuse, name, link_to, succeeds in cases:
                with self.subTest(case=label):
                    case = root / label
                    home = case / "home"
                    versions = home / "hookshot" / "builder" / "versions"
                    previous = versions / "v1"
                    previous.mkdir(parents=True)
                    (previous / "kept").write_bytes(b"previous version")
                    current = versions.parent / "current"
                    current.write_text("v1\n", encoding="utf-8")
                    canary = versions / "canary"
                    canary.write_bytes(b"authored sibling canary")
                    partial = versions / "v2.part"
                    self.assertEqual(partial.parent, canary.parent)
                    self.assertIn(root, canary.resolve().parents)
                    before = canary.read_bytes()
                    files = {"usr/share/hookshot/builder/linux/AppRun": b"authored placeholder, never executed\n",
                             "inside": b"inside"}
                    if "listed" in label:
                        del files["usr/share/hookshot/builder/linux/AppRun"]
                    if link_to is None and reuse:
                        link_to = str(canary)
                    if link_to is None:
                        files[name] = b"part-supplied list"
                    archive = io.BytesIO()
                    with zipfile.ZipFile(archive, "w") as zipped:
                        if link_to is not None:
                            zip_link(zipped, name, link_to)  # before its target, when it is a valid forward link
                        for filename, data in files.items():
                            zipped.writestr(filename, data)
                    data = archive.getvalue()
                    part = linux_update.Part(linux_update._sha256(data), len(data), ".", "authored-part", None)
                    if "listed" in label:
                        part.files = list(files) + [name]
                    parts, payloads = [part], {part.url: data}
                    if "listed" in label:
                        archive = io.BytesIO()
                        with zipfile.ZipFile(archive, "w") as zipped:
                            zipped.writestr("usr/share/hookshot/builder/linux/AppRun", b"authored placeholder, never executed\n")
                        data = archive.getvalue()
                        tools = linux_update.Part(linux_update._sha256(data), len(data), ".", "authored-tools", None)
                        parts.append(tools)
                        payloads[tools.url] = data
                    if "cross-part" in label:
                        archive = io.BytesIO()
                        with zipfile.ZipFile(archive, "w") as zipped:
                            zipped.writestr("target", b"cross-part")
                        data = archive.getvalue()
                        other = linux_update.Part(linux_update._sha256(data), len(data), "usr/lib", "authored-library", None)
                        parts.append(other)
                        payloads[other.url] = data
                    source_root, source = None, None
                    if reuse:
                        source_root = case / "source"
                        source_root.mkdir()
                        for filename, content in files.items():
                            path = source_root / filename
                            path.parent.mkdir(parents=True, exist_ok=True)
                            path.write_bytes(content)
                        (source_root / name).symlink_to(link_to)
                        source = linux_update.Manifest("v1", [part])
                    error = None
                    with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(home)}):
                        try:
                            apprun = linux_update.install(linux_update.Manifest("v2", parts),
                                                         str(source_root) if reuse else None, source,
                                                         get=lambda url: payloads[url])
                        except Exception as caught:
                            error = caught
                    # Check even when install raised: an error after truncation is not safe rejection.
                    self.assertEqual(canary.read_bytes(), before, "the sibling canary was overwritten")
                    if succeeds:
                        self.assertIsNone(error)
                        installed = versions / "v2"
                        self.assertEqual(apprun, str(installed / "AppRun"))
                        self.assertEqual(current.read_text(encoding="utf-8"), "v2\n")
                        self.assertEqual((installed / "alias").read_bytes(),
                                         b"cross-part" if "cross-part" in label else b"inside")
                        self.assertTrue((installed / "alias").is_symlink())
                    else:
                        self.assertIsInstance(error, (ValueError, FileExistsError))
                        self.assertEqual(current.read_text(encoding="utf-8"), "v1\n")
                        self.assertEqual((previous / "kept").read_bytes(), b"previous version")
                        self.assertFalse(partial.exists())
                    print("PASS link bounds:", label)
            with self.subTest(case="relative-target-through-an-existing-alias"):
                case = root / "resolved-escape"
                dest = case / "stage"
                dest.mkdir(parents=True)
                canary = case / "canary"
                canary.write_bytes(b"resolved sibling canary")
                before = canary.read_bytes()
                (dest / "redirect").symlink_to("..", target_is_directory=True)
                archive = io.BytesIO()
                with zipfile.ZipFile(archive, "w") as zipped:
                    zip_link(zipped, "alias", "redirect/canary")
                error = None
                try:
                    linux_update._unpack(archive.getvalue(), str(dest), None)
                except Exception as caught:
                    error = caught
                self.assertEqual(canary.read_bytes(), before)
                self.assertIsInstance(error, ValueError)
                self.assertFalse((dest / "alias").is_symlink())
                print("PASS link bounds: relative-target-through-an-existing-alias")
            with self.subTest(case="published-parts-do-not-own-the-list"):
                source, out = root / "publisher", root / "parts"
                appdir(source)
                with mock.patch.object(sys, "argv", ["make_parts.py", str(source), "v2", str(out)]):
                    make_parts.main()
                manifest = linux_update.parse_list((out / linux_update.LIST_NAME).read_text(encoding="utf-8"))
                payloads = {}
                for part in manifest.parts:
                    archive = out / part.url.rsplit("/", 1)[-1]
                    with zipfile.ZipFile(archive) as zipped:
                        self.assertNotIn(linux_update.LIST_NAME, zipped.namelist())
                    payloads[part.url] = archive.read_bytes()
                home = root / "publisher-home"
                versions = home / "hookshot" / "builder" / "versions"
                versions.mkdir(parents=True)
                canary = versions / "canary"
                canary.write_bytes(b"publisher sibling canary")
                before = canary.read_bytes()
                with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(home)}):
                    apprun = linux_update.install(manifest, None, None, get=lambda url: payloads[url])
                self.assertEqual(canary.read_bytes(), before)
                self.assertTrue(Path(apprun).is_file())
                installed = linux_update.read_manifest(str(versions / "v2"))
                self.assertEqual(installed.tag, "v2")
                self.assertEqual(len(installed.parts), len(make_parts.PARTS))
                print("PASS link bounds: published-parts-do-not-own-the-list")

    def test_install_rejects_a_staging_link_before_cleanup_or_placing_parts(self):
        from unittest import mock
        for exists in (False, True):
            with self.subTest(target_exists=exists), tempfile.TemporaryDirectory(prefix="hookshot-stage-") as tmp:
                root = Path(tmp).resolve()
                home = root / "home"
                versions = home / "hookshot" / "builder" / "versions"
                versions.mkdir(parents=True)
                target = root / "outside-stage"
                sentinel = root / "sentinel"
                sentinel.write_text("unchanged", encoding="utf-8")
                if exists:
                    target.mkdir()
                    (target / "kept").write_text("unchanged", encoding="utf-8")
                self.assertIn(root, target.resolve().parents)
                partial = versions / "v2.part"
                partial.symlink_to(target, target_is_directory=True)
                current = versions.parent / "current"
                current.write_text("v1\n", encoding="utf-8")
                manifest = linux_update.Manifest("v2", [linux_update.Part("a" * 64, 1, "usr/python", "unused", None)])
                get = mock.Mock(side_effect=AssertionError("download must not start"))
                with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(home)}), \
                        mock.patch.object(linux_update, "_place", side_effect=AssertionError("placement must not start")) as place, \
                        mock.patch.object(linux_update.shutil, "rmtree", side_effect=AssertionError("cleanup must not start")) as cleanup:
                    error = None
                    try:
                        linux_update.install(manifest, None, None, get=get)
                    except Exception as caught:
                        error = caught
                    self.assertIsInstance(error, ValueError)
                    self.assertIn(str(partial), str(error))
                    self.assertIn("symbolic link", str(error))
                    cleanup.assert_not_called()
                    place.assert_not_called()
                    get.assert_not_called()
                self.assertTrue(partial.is_symlink())
                self.assertEqual(os.readlink(partial), str(target))
                self.assertEqual(target.exists(), exists)
                if exists:
                    self.assertEqual(sorted(p.name for p in target.iterdir()), ["kept"])
                    self.assertEqual((target / "kept").read_text(encoding="utf-8"), "unchanged")
                self.assertEqual(sentinel.read_text(encoding="utf-8"), "unchanged")
                self.assertEqual(current.read_text(encoding="utf-8"), "v1\n")

    def test_install_keeps_real_staging_and_symlinked_parent_roots_working(self):
        from unittest import mock
        for linked_home, stale in ((False, False), (False, True), (True, False)):
            with self.subTest(linked_home=linked_home, stale=stale), tempfile.TemporaryDirectory(prefix="hookshot-stage-") as tmp:
                root = Path(tmp).resolve()
                real_home = root / "real-home"
                real_home.mkdir()
                home = root / "linked-home" if linked_home else real_home
                if linked_home:
                    home.symlink_to(real_home, target_is_directory=True)
                self.assertIn(root, home.resolve().parents)
                versions = home / "hookshot" / "builder" / "versions"
                old = versions / "v1"
                old.mkdir(parents=True)
                (old / "kept").write_text("old", encoding="utf-8")
                partial = versions / "v2.part"
                if stale:
                    partial.mkdir()
                    (partial / "stale").write_text("old", encoding="utf-8")
                part = linux_update.Part("a" * 64, 1, "usr/share/hookshot", "unused", None)
                manifest = linux_update.Manifest("v2", [part])

                def place(part, source_root, source, dest_root, get, note):
                    self.assertEqual(dest_root, str(partial))
                    tools = Path(dest_root) / part.dest / "builder" / "linux"
                    tools.mkdir(parents=True)
                    (tools / "AppRun").write_text("authored placeholder, never executed\n", encoding="utf-8")

                get = mock.Mock(side_effect=AssertionError("no download in this fixture"))
                with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(home)}), \
                        mock.patch.object(linux_update, "_place", side_effect=place) as placed:
                    apprun = linux_update.install(manifest, None, None, get=get)
                installed = versions / "v2"
                self.assertEqual(apprun, str(installed / "AppRun"))
                self.assertEqual((installed / "AppRun").read_text(encoding="utf-8"), "authored placeholder, never executed\n")
                self.assertEqual((versions.parent / "current").read_text(encoding="utf-8"), "v2\n")
                self.assertEqual(sorted(p.name for p in versions.iterdir()), ["v2"])
                self.assertFalse((installed / "stale").exists())
                self.assertTrue((installed / linux_update.LIST_NAME).is_file())
                placed.assert_called_once()
                get.assert_not_called()
                if linked_home:
                    self.assertTrue(home.is_symlink())

    def test_a_list_whose_names_leave_the_versions_folder_is_refused(self):
        sha = "a" * 64
        good = "tag v2.1.0\npart %s 10 usr/share/hookshot https://example.invalid/p.zip\nfile nx2/marker.py\n" % sha
        self.assertEqual(linux_update.parse_list(good).tag, "v2.1.0")
        bad = [
            good.replace("tag v2.1.0", "tag .."),
            good.replace("tag v2.1.0", "tag /home/someone"),
            good.replace("tag v2.1.0", "tag ../../x"),
            good.replace("usr/share/hookshot", "../../outside"),
            good.replace("usr/share/hookshot", "/abs"),
            good.replace("file nx2/marker.py", "file ../../../x"),
            good.replace(sha, "abc"),
            good.replace("https://", "http://"),
        ]
        for text in bad:
            with self.assertRaises(ValueError, msg=text):
                linux_update.parse_list(text)
        # a file link is for a list from this PC only, not one downloaded (it read /dev/zero, or tested a file's hash)
        local = good.replace("https://example.invalid/p.zip", "file:///dev/zero")
        self.assertEqual(linux_update.parse_list(local).tag, "v2.1.0")
        with self.assertRaises(ValueError):
            linux_update.parse_list(local, local=False)
        with self.assertRaises(ValueError):
            linux_update.fetch_manifest(get=lambda url: local.encode())

    def test_a_download_reads_at_most_its_limit(self):
        import io
        self.assertEqual(linux_update._read_at_most(io.BytesIO(b"x" * 10), 10), b"x" * 10)
        with self.assertRaises(RuntimeError):
            linux_update._read_at_most(io.BytesIO(b"x" * 11), 10)

    def test_a_download_is_a_get(self):
        # (Request's second argument is the POST body: v2.0.12's str there failed every download before it connected)
        from unittest import mock
        seen = []

        class Answer(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

        def opened(req, timeout=None):
            seen.append(req)
            return Answer(b"list")
        with mock.patch.object(linux_update._OPENER, "open", opened):
            self.assertEqual(linux_update._request("https://example.invalid/list.txt", 5), b"list")
        self.assertEqual(seen[0].get_method(), "GET")
        self.assertIsNone(seen[0].data)

    def test_a_redirect_is_followed_only_to_https(self):
        # (urllib's own handler follows a redirect to http: and ftp: too: _https checked the first link alone)
        import urllib.request
        handlers = [h for h in linux_update._OPENER.handlers if isinstance(h, urllib.request.HTTPRedirectHandler)]
        self.assertEqual([type(h) for h in handlers], [linux_update._HttpsRedirects])   # (in place of urllib's, not beside it)
        first = urllib.request.Request("https://github.com/NotRexed/Hookshot/releases/download/v9/part.zip",
                                       headers={"User-Agent": "Hookshot"})
        onward = handlers[0].redirect_request(first, None, 302, "Found", {}, "https://objects.example.invalid/part.zip")
        self.assertEqual(onward.full_url, "https://objects.example.invalid/part.zip")
        self.assertEqual(onward.get_method(), "GET")
        for away in ("http://objects.example.invalid/part.zip", "ftp://objects.example.invalid/part.zip",
                     "file:///etc/hostname"):
            with self.subTest(away=away), self.assertRaisesRegex(ValueError, "not an https link"):
                handlers[0].redirect_request(first, None, 302, "Found", {}, away)

    def test_a_redirect_to_http_never_connects(self):
        # (the real opener and urllib's own redirect handling, with the https connection alone replaced: it answers 302)
        import email
        import urllib.request
        import urllib.response
        from unittest import mock
        asked = []

        def https_open(req):
            asked.append(req.full_url)
            headers = email.message_from_string("Location: http://objects.example.invalid/part.zip\n\n")
            answer = urllib.response.addinfourl(io.BytesIO(b""), headers, req.full_url, 302)
            answer.msg = "Found"
            return answer

        def http_open(req):
            asked.append(req.full_url)
            raise AssertionError("connected to a plain http link")

        https = next(h for h in linux_update._OPENER.handlers if isinstance(h, urllib.request.HTTPSHandler))
        http = next(h for h in linux_update._OPENER.handlers if type(h) is urllib.request.HTTPHandler)
        with mock.patch.object(https, "https_open", https_open), mock.patch.object(http, "http_open", http_open), \
                self.assertRaisesRegex(ValueError, "not an https link: 'http://objects.example.invalid/part.zip'"):
            linux_update._request("https://github.com/NotRexed/Hookshot/releases/download/v9/part.zip", 5)
        self.assertEqual(asked, ["https://github.com/NotRexed/Hookshot/releases/download/v9/part.zip"])

    def test_a_link_chain_cannot_leave_the_staging_tree(self):
        # x/../.. is inside the folder as written, but the system resolves x first: a link made after it (in the same zip
        # or a later part) put the chain anywhere, and AppRun copied into a link wrote a file outside the install
        from unittest import mock
        deep = "a/b/c/d/e/f"
        chains = (
            ("climb-after-a-name", [("L", "t/../canary"), ("t", ".")]),
            ("apprun-through-a-chain", [("AppRun", deep + "/L"), (deep + "/L", "t/../canary"), (deep + "/t", "../" * 6)]),
        )
        for label, links in chains:
            with self.subTest(case=label), tempfile.TemporaryDirectory(prefix="hookshot-chain-") as tmp:
                home = Path(tmp).resolve() / "home"
                versions = home / "hookshot" / "builder" / "versions"
                versions.mkdir(parents=True)
                canary = versions / "canary"
                canary.write_bytes(b"canary")
                archive = io.BytesIO()
                with zipfile.ZipFile(archive, "w") as zipped:
                    for name, target in links:
                        zip_link(zipped, name, target)
                    zipped.writestr("usr/share/hookshot/builder/linux/AppRun", b"#!/bin/sh\necho changed\n")
                data = archive.getvalue()
                part = linux_update.Part(linux_update._sha256(data), len(data), ".", "chain-part", None)
                with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(home)}):
                    with self.assertRaises(ValueError):
                        linux_update.install(linux_update.Manifest("v2", [part]), None, None, get=lambda url: data)
                self.assertEqual(canary.read_bytes(), b"canary")
                self.assertFalse((versions / "v2").exists())
                self.assertFalse((versions / "v2.part").exists())
        # (leading .. stays allowed: the toolchain's links are written that way)
        with tempfile.TemporaryDirectory(prefix="hookshot-chain-ok-") as tmp:
            dest = Path(tmp) / "stage"
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as zipped:
                zipped.writestr("lib/real", b"real")
                zip_link(zipped, "bin/alias", "../lib/real")
            linux_update._unpack(archive.getvalue(), str(dest), None)
            self.assertEqual((dest / "bin" / "alias").read_bytes(), b"real")

    def test_unpacked_files_are_not_group_or_world_writable(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-mode-") as tmp:
            dest = Path(tmp) / "stage"
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as zipped:
                for name, mode in (("tool", 0o777), ("data", 0o666), ("setuid", 0o4755)):
                    info = zipfile.ZipInfo(name)
                    info.create_system = 3
                    info.external_attr = (stat.S_IFREG | mode) << 16
                    zipped.writestr(info, b"x")
            linux_update._unpack(archive.getvalue(), str(dest), None)
            self.assertEqual((dest / "tool").stat().st_mode & 0o7777, 0o755)
            self.assertEqual((dest / "data").stat().st_mode & 0o7777, 0o644)
            self.assertEqual((dest / "setuid").stat().st_mode & 0o7777, 0o755)

    def test_the_button_matches_the_windows_builder(self):
        self.assertEqual(linux_update.choose_mode("v1", "v2", "v2", False, True), "update-game")
        self.assertEqual(linux_update.choose_mode("v2", "v2", "v2", False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v1", "v1", None, False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v1", "v1", "v2", False, True), "update-game")
        self.assertEqual(linux_update.choose_mode(None, "v1", "v2", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2", "v2", False, False), "build")
        self.assertEqual(linux_update.choose_mode("v2", "v2", "v2", True, True), "build")

    def test_a_build_that_is_not_an_older_release_is_not_made_to_update(self):
        # (issue 902) an AppImage from the Actions tab is git describe's version, after the latest release: it builds
        ci = "v2.0.5-3-g1a2b3c4"
        self.assertEqual(linux_update.choose_mode(None, ci, "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "1a2b3c4", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "dev", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "v2.0.6", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "v2.1.0-rc.1", "v2.0.5", False, False), "build")
        # (its game is played, not taken back to the release; an older game is built again with this builder)
        self.assertEqual(linux_update.choose_mode(ci, ci, "v2.0.5", False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v2.0.4", ci, "v2.0.5", False, True), "update-game")
        self.assertEqual(linux_update.update_target("v2.0.5", ci), ci)
        # (a game the release built is older than this Actions builder: its Update builds it again with the builder running,
        # nothing downloaded, as for any older game)
        self.assertEqual(linux_update.choose_mode("v2.0.5", ci, "v2.0.5", False, True), "update-game")
        # an older release still gets the newer one
        self.assertEqual(linux_update.choose_mode(None, "v2.0.4", "v2.0.5", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2.1.0-rc.1", "v2.1.0", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2.0.10", "v2.0.9", False, False), "build")
        self.assertEqual(linux_update.choose_mode("v2.0.4", "v2.0.4", "v2.0.5", False, True), "update-game")
        self.assertEqual(linux_update.update_target("v2.0.5", "v2.0.4"), "v2.0.5")
        # a build from the Actions tab made after an older release is offered the newer one
        self.assertEqual(linux_update.choose_mode(None, "v2.0.4-3-g1a2b3c4", "v2.0.5", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2.0.5-3-g1a2b3c4-dirty", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "v2.1.0-rc.1-2-gabc1234", "v2.1.0", False, False), "update-builder")

    def test_an_rc_build_with_its_own_list_is_not_downgraded_to_the_release(self):
        # (issue 990: a -rc AppImage ships correct tools and a list naming its own parts, while releases/latest is an
        # older release whose zip lacks a file these tools have: reinstalling it every launch lost that file)
        from unittest import mock
        with tempfile.TemporaryDirectory(prefix="hookshot-updates-") as tmp:
            root = Path(tmp) / "rc"
            appdir(root)
            (root / "usr" / "share" / "hookshot" / "VERSION").write_text("v1.1-rc.1\n", encoding="utf-8")
            (root / "hookshot-linux.txt").write_text(
                "tag v1.1-rc.1\npart %s 10 usr/share/hookshot file://%s/rc.zip\nfile nx2/marker.py\n" % ("a" * 64, tmp),
                encoding="utf-8")
            stable = "tag v1.0\npart %s 10 usr/share/hookshot https://example.invalid/old.zip\nfile nx2/marker.py\n" % ("b" * 64)
            got = linux_update.startup_manifest(str(root), get=lambda url: stable.encode())
            self.assertEqual((got.tag, got.parts[0].sha), ("v1.1-rc.1", "a" * 64))     # (its own: nothing reinstalled)
            newer = "tag v1.2\npart %s 10 usr/share/hookshot https://example.invalid/new.zip\nfile nx2/marker.py\n" % ("c" * 64)
            got = linux_update.startup_manifest(str(root), get=lambda url: newer.encode())
            self.assertEqual((got.tag, got.parts[0].sha), ("v1.2", "c" * 64))          # (a newer release still wins)
            # a HOOKSHOT_LINUX_LIST is taken even when the tags match (a patched part for a release, issue 990)
            (root / "patched.txt").write_text(
                "tag v1.1-rc.1\npart %s 10 usr/share/hookshot file://%s/patched.zip\nfile nx2/marker.py\n" % ("d" * 64, tmp),
                encoding="utf-8")
            with mock.patch.dict(os.environ, {"HOOKSHOT_LINUX_LIST": str(root / "patched.txt")}):
                got = linux_update.startup_manifest(str(root), get=lambda url: stable.encode())
                self.assertEqual(got.parts[0].sha, "d" * 64)
            # an AppImage at the latest release keeps using its bundled list (nothing was ever reinstalled there)
            release = Path(tmp) / "release"
            appdir(release)     # (VERSION v1)
            (release / "hookshot-linux.txt").write_text(
                "tag v1\npart %s 10 usr/share/hookshot file://%s/release.zip\nfile nx2/marker.py\n" % ("e" * 64, tmp),
                encoding="utf-8")
            same = "tag v1\npart %s 10 usr/share/hookshot https://example.invalid/old.zip\nfile nx2/marker.py\n" % ("b" * 64)
            got = linux_update.startup_manifest(str(release), get=lambda url: same.encode())
            self.assertEqual(got.parts[0].sha, "e" * 64)
            # and one older than the release still downloads it
            (release / "usr" / "share" / "hookshot" / "VERSION").write_text("v0.9\n", encoding="utf-8")
            got = linux_update.startup_manifest(str(release), get=lambda url: stable.encode())
            self.assertEqual(got.parts[0].sha, "b" * 64)

    def test_a_changed_part_is_downloaded_and_the_rest_are_kept(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-parts-") as tmp:
            root = Path(tmp)
            old = root / "old"
            appdir(old)
            first = root / "parts1"
            argv = sys.argv
            try:
                sys.argv = ["make_parts.py", str(old), "v1", str(first)]
                make_parts.main()
            finally:
                sys.argv = argv
            manifest = linux_update.parse_list((first / "hookshot-linux.txt").read_text(encoding="utf-8"))
            home = root / "home"
            os.environ["HOOKSHOT_BUILDER_HOME"] = str(home)
            self.addCleanup(lambda: os.environ.pop("HOOKSHOT_BUILDER_HOME", None))
            downloaded = []

            def get(url):
                downloaded.append(url)
                name = url.rsplit("/", 1)[-1]
                return (first / name).read_bytes()

            linux_update.install(manifest, str(old), manifest, get=get)
            self.assertEqual(downloaded, [])
            installed = home / "hookshot" / "builder" / "versions" / "v1"
            self.assertTrue((installed / "AppRun").stat().st_mode & stat.S_IXUSR)
            self.assertEqual((installed / "usr" / "share" / "hookshot" / "nx2" / "marker.py").read_text(encoding="utf-8"), "print('one')\n")

            new = root / "new"
            shutil.copytree(old, new)
            (new / "usr" / "share" / "hookshot" / "nx2" / "marker.py").write_text("print('two')\n", encoding="utf-8")
            second = root / "parts2"
            try:
                sys.argv = ["make_parts.py", str(new), "v2", str(second)]
                make_parts.main()
            finally:
                sys.argv = argv
            nxt = linux_update.parse_list((second / "hookshot-linux.txt").read_text(encoding="utf-8"))
            downloaded.clear()

            def get2(url):
                downloaded.append(url)
                name = url.rsplit("/", 1)[-1]
                return (second / name).read_bytes()

            linux_update.install(nxt, str(installed), manifest, get=get2)
            self.assertEqual(len(downloaded), 1)
            self.assertIn("usr-share-hookshot", downloaded[0])
            built = home / "hookshot" / "builder" / "versions" / "v2"
            self.assertEqual((built / "usr" / "share" / "hookshot" / "nx2" / "marker.py").read_text(encoding="utf-8"), "print('two')\n")
            self.assertEqual((built / "usr" / "lib" / "hookshot" / "hookshot_vk.so").read_bytes(), b"vk")
            self.assertFalse((home / "hookshot" / "builder" / "versions" / "v1").exists())

    def test_an_installed_builder_finds_its_own_root(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-root-") as tmp:
            root = Path(tmp) / "versions" / "v1"
            appdir(root)
            saved = {k: os.environ.get(k) for k in ("APPDIR", "HOOKSHOT_TOOLS")}
            os.environ.pop("APPDIR", None)
            os.environ["HOOKSHOT_TOOLS"] = str(root / "usr" / "share" / "hookshot")
            try:
                found = linux_update.running_root()
            finally:
                for k, v in saved.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v
            self.assertEqual(found, str(root))
            self.assertEqual(linux_update.tools_tag(found), "v1")

    @unittest.skipUnless(sys.platform.startswith("linux"), "the Linux folder layout")
    def test_a_packaged_build_puts_the_launcher_in_the_folder_and_the_game_in_bin(self):
        import ast
        from unittest import mock
        import make_game
        main = next(n for n in ast.parse((ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")).body
                    if isinstance(n, ast.FunctionDef) and n.name == "main")
        start = next(i for i, n in enumerate(main.body) if isinstance(n, ast.Assign)
                     and any(isinstance(t, ast.Name) and t.id == "staged" for t in n.targets))
        end = next(i for i, n in enumerate(main.body[start + 1:], start + 1) if isinstance(n, ast.If)
                   and any(isinstance(c, ast.Call) and isinstance(c.func, ast.Name) and c.func.id == "shader_census_cmd"
                           for c in ast.walk(n)))
        finish = compile(ast.Module(body=main.body[start:end], type_ignores=[]), "make_game.py", "exec")
        with tempfile.TemporaryDirectory(prefix="hookshot-finish-") as tmp:
            work, out = Path(tmp) / "work", Path(tmp) / "game"
            built = work / "sa" / "build"
            built.mkdir(parents=True)
            (work / "sa" / "image").mkdir()
            (work / "sa" / "image" / "image.bin").write_bytes(b"image")
            for name in make_game.BESIDE:
                (built / name).write_bytes(b"new " + name.encode())
            out.mkdir()
            for name in make_game.BESIDE:   # (a folder from a builder before the launcher: the game and its libraries in it)
                (out / name).write_bytes(b"old " + name.encode())
            with mock.patch.dict(os.environ, {"HOOKSHOT_TOOLS": str(ROOT)}):
                exec(finish, make_game.__dict__, {"built": str(built), "out": str(out), "work": str(work),
                                                   "a": SimpleNamespace(fsr=False, no_fsr=False, update=None, fetch_fsr_runtime=False)})
            self.assertEqual((out / "hookshot").read_bytes(), (ROOT / "builder" / "linux" / "hookshot-folder").read_bytes())
            self.assertTrue((out / "hookshot").stat().st_mode & stat.S_IXUSR)
            self.assertEqual(sorted(p.name for p in out.iterdir()), ["bin", "hookshot", "image.bin"])
            self.assertEqual(sorted(p.name for p in (out / "bin").iterdir()), sorted(make_game.BESIDE))
            self.assertEqual((out / "bin" / "hookshot").read_bytes(), b"new hookshot")

    def test_the_sysroot_path_is_not_part_of_an_object_key(self):
        import build
        handle, name = tempfile.mkstemp()
        os.write(handle, b"int x;\n")
        os.close(handle)
        try:
            one = build.object_key(name, ["--sysroot=/tmp/.mount_aaa/usr/lib/hookshot/sysroot", "-O2"], b"deps")
            two = build.object_key(name, ["--sysroot=/tmp/.mount_bbb/usr/lib/hookshot/sysroot", "-O2"], b"deps")
        finally:
            os.remove(name)
        self.assertEqual(one, two)

    def test_the_folder_launcher_starts_the_builder(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            (game / "image.bin").write_bytes(b"image")
            started = root / "started"
            builder = root / "builder"
            builder.write_text("#!/bin/sh\nprintf '%%s\\n' \"$0\" \"$@\" > '%s'\n" % started, encoding="utf-8")
            builder.chmod(0o755)
            (game / "hookshot_version.txt").write_text("version=v1\nbuilder=%s\n" % builder, encoding="utf-8")
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(root / "none")
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            lines = started.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[1], "--launch")
            self.assertEqual(lines[2], str(game))

    def test_the_folder_launcher_finds_the_builder_under_the_builder_home(self):
        # HOOKSHOT_BUILDER_HOME is the data directory, as XDG_DATA_HOME is: linux_update installs to
        # <home>/hookshot/builder and host/hos_update.c looks there, so the launcher must too
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            home = root / "home"
            started = root / "started"
            installed = home / "hookshot" / "builder"
            (installed / "versions" / "v9").mkdir(parents=True)
            (installed / "current").write_text("v9\n", encoding="utf-8")
            apprun = installed / "versions" / "v9" / "AppRun"
            apprun.write_text("#!/bin/sh\nprintf '%%s\\n' \"$0\" \"$@\" > '%s'\n" % started, encoding="utf-8")
            apprun.chmod(0o755)
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(home)
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            lines = started.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], str(apprun))
            self.assertEqual(lines[1:], ["--launch", str(game)])

    def test_the_folder_launcher_starts_the_game_when_no_builder_is_installed(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            (game / "bin" / "hookshot").write_text("#!/bin/sh\nprintf 'game\\n'\n", encoding="utf-8")
            (game / "bin" / "hookshot").chmod(0o755)
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(root / "none")
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertEqual(ran.stdout.strip(), "game")

    def test_a_part_cannot_write_through_a_zip_symlink(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-zip-") as tmp:
            root = Path(tmp)
            outside = root / "outside"
            outside.mkdir()
            marker = outside / "marker"
            marker.write_bytes(b"original")
            # The third shape: an absolute target needs no write through the link to escape (#1295)
            for link_target, member in ((outside, "link/marker"), (marker, "./link"), ("C:/escape" if os.name == "nt" else "/tmp/escape", None)):
                with self.subTest(member=member):
                    archive = io.BytesIO()
                    with zipfile.ZipFile(archive, "w") as zipped:
                        link = zipfile.ZipInfo("link")
                        link.create_system = 3
                        link.external_attr = (stat.S_IFLNK | 0o777) << 16
                        zipped.writestr(link, str(link_target))
                        if member:
                            zipped.writestr(member, b"changed")
                    dest = root / ("directory" if member == "link/marker" else "file-abs" if member is None else "file")
                    with self.assertRaises((ValueError, FileExistsError)):
                        linux_update._unpack(archive.getvalue(), str(dest), None)
                    self.assertEqual(marker.read_bytes(), b"original")

    def test_a_part_cannot_extract_through_a_symlinked_destination(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-zip-") as tmp:
            root = Path(tmp)
            outside = root / "outside"
            outside.mkdir()
            dest = root / "stage"
            dest.mkdir()
            (dest / "link").symlink_to(outside, target_is_directory=True)
            staging_link = root / "staging-link"
            staging_link.symlink_to(outside / "missing", target_is_directory=True)
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as zipped:
                zipped.writestr("marker", b"changed")
            data = archive.getvalue()
            for stage, folder in ((dest, "link/sub"), (staging_link, "usr")):
                with self.subTest(folder=folder):
                    part = linux_update.Part(linux_update._sha256(data), len(data), folder, "test", None)
                    with self.assertRaises(ValueError):
                        linux_update._place(part, None, None, str(stage), lambda _: data, lambda _: None)
                    self.assertEqual(list(outside.iterdir()), [])

    def test_a_part_keeps_toolchain_links(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-zip-") as tmp:
            root = Path(tmp)
            source = root / "sysroot"
            source.mkdir()
            (source / "lib").symlink_to("usr/lib")
            (source / "libmvec.so").symlink_to("usr/lib/libmvec.so.1")
            (source / "usr" / "lib").mkdir(parents=True)
            (source / "usr" / "lib" / "marker").write_bytes(b"contents")
            (source / "usr" / "lib" / "libmvec.so.1").write_bytes(b"library")
            archive = root / "part.zip"
            make_parts.write_zip(str(archive), str(root), "sysroot", None)
            dest = root / "unpacked"
            linux_update._unpack(archive.read_bytes(), str(dest), None)
            self.assertEqual(os.readlink(dest / "libmvec.so"), "usr/lib/libmvec.so.1")
            self.assertEqual((dest / "libmvec.so").read_bytes(), b"library")
            self.assertEqual(os.readlink(dest / "lib"), "usr/lib")
            self.assertEqual((dest / "lib" / "marker").read_bytes(), b"contents")

    def test_a_new_sysroot_part_rebases_rooted_links(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-zip-") as tmp:
            root = Path(tmp)
            sysroot_fixture(root)
            archive = root / "part.zip"
            make_parts.write_zip(str(archive), str(root), linux_update.SYSROOT_PART, None)
            dest = root / "unpacked"
            linux_update._unpack(archive.read_bytes(), str(dest), None)
            link = dest / "usr" / "lib" / "x86_64-linux-gnu" / "libmvec.so"
            self.assertFalse(os.path.isabs(os.readlink(link)))
            self.assertEqual(link.read_bytes(), b"contents")
            self.assertEqual(os.readlink(dest / "lib"), "usr/lib")

    def test_a_downloaded_legacy_sysroot_rebases_rooted_links(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-zip-") as tmp:
            root = Path(tmp)
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as zipped:
                zip_link(zipped, "lib", "usr/lib")
                zip_link(zipped, "usr/lib/x86_64-linux-gnu/libmvec.so", "/lib/x86_64-linux-gnu/libmvec.so.1")
                zipped.writestr("usr/lib/x86_64-linux-gnu/libmvec.so.1", b"contents")
            data = archive.getvalue()
            with self.assertRaises(ValueError):
                linux_update._unpack(data, str(root / "ordinary"), None)
            part = linux_update.Part(linux_update._sha256(data), len(data), linux_update.SYSROOT_PART, "legacy", None)
            dest = root / "installed"
            linux_update._place(part, None, None, str(dest), lambda _: data, lambda _: None)
            link = dest / linux_update.SYSROOT_PART / "usr" / "lib" / "x86_64-linux-gnu" / "libmvec.so"
            self.assertFalse(os.path.isabs(os.readlink(link)))
            self.assertEqual(link.read_bytes(), b"contents")

    def test_a_kept_legacy_sysroot_rebases_rooted_links(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-sysroot-") as tmp:
            root = Path(tmp)
            source = root / "source"
            sysroot_fixture(source)
            part = linux_update.Part("a" * 64, 1, linux_update.SYSROOT_PART, "unused", None)
            previous = linux_update.Manifest("old", [part])
            dest = root / "installed"
            def no_download(_):
                self.fail("an unchanged sysroot must not be downloaded")
            linux_update._place(part, str(source), previous, str(dest), no_download, lambda _: None)
            link = dest / linux_update.SYSROOT_PART / "usr" / "lib" / "x86_64-linux-gnu" / "libmvec.so"
            self.assertFalse(os.path.isabs(os.readlink(link)))
            self.assertEqual(link.read_bytes(), b"contents")

    def test_a_rooted_sysroot_link_cannot_escape_or_be_dangling(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-sysroot-") as tmp:
            root = Path(tmp)
            outside = root / "outside"
            outside.mkdir()
            marker = outside / "marker"
            marker.write_bytes(b"original")
            for name, link_to, indirect in (("lexical", "/../outside/marker", False),
                                            ("indirect", "/lib/marker", True),
                                            ("missing", "/missing", False)):
                with self.subTest(name=name):
                    archive = io.BytesIO()
                    with zipfile.ZipFile(archive, "w") as zipped:
                        if indirect:
                            zip_link(zipped, "lib", "../outside")
                        zip_link(zipped, "link", link_to)
                    with self.assertRaises(ValueError):
                        linux_update._unpack(archive.getvalue(), str(root / name), None, rebase_sysroot=True)
                    self.assertEqual(marker.read_bytes(), b"original")
                    self.assertFalse((root / name / "link").is_symlink())

    def test_rooted_sysroot_links_resolve_chains_but_not_cycles(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-sysroot-") as tmp:
            root = Path(tmp)
            for name, third in (("chain", True), ("cycle", False)):
                with self.subTest(name=name):
                    archive = io.BytesIO()
                    with zipfile.ZipFile(archive, "w") as zipped:
                        zip_link(zipped, "first", "/second")
                        zip_link(zipped, "second", "/third" if third else "/first")
                        if third:
                            zipped.writestr("third", b"contents")
                    dest = root / name
                    if third:
                        linux_update._unpack(archive.getvalue(), str(dest), None, rebase_sysroot=True)
                        self.assertEqual((dest / "first").read_bytes(), b"contents")
                        self.assertFalse(os.path.isabs(os.readlink(dest / "first")))
                    else:
                        with self.assertRaises(ValueError):
                            linux_update._unpack(archive.getvalue(), str(dest), None, rebase_sysroot=True)

    def test_packaging_refuses_a_sysroot_link_outside_its_root(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-sysroot-") as tmp:
            root = Path(tmp)
            source = root / linux_update.SYSROOT_PART
            source.mkdir(parents=True)
            (source / "bad").symlink_to("/../outside")
            with self.assertRaises(ValueError):
                make_parts.write_zip(str(root / "part.zip"), str(root), linux_update.SYSROOT_PART, None)

    @unittest.skipUnless(sys.platform.startswith("linux"), "the Linux sysroot copy loop")
    def test_sysroot_system_aliases_round_trip_as_library_bytes(self):
        script = (ROOT / "scripts" / "linux" / "appimage.sh").read_text(encoding="utf-8")
        start = script.index("for f in crt1.o ")
        loop = script[start:script.index("\ndone", start) + len("\ndone")]
        with tempfile.TemporaryDirectory(prefix="hookshot-sysroot-") as tmp:
            root = Path(tmp)
            system = root / "system"
            system.mkdir()
            real = system / "libmvec.so.1"
            real.write_bytes(b"private library bytes")
            alias = system / "libmvec.so"
            alias.symlink_to(real)
            source = root / "sysroot"
            (source / "usr" / "lib" / "x86_64-linux-gnu").mkdir(parents=True)
            (source / "lib").symlink_to("usr/lib")
            env = dict(os.environ, M=str(system), S=str(source))
            subprocess.run(["bash", "-euo", "pipefail", "-c", loop], env=env, check=True)
            copied = source / "usr" / "lib" / "x86_64-linux-gnu" / alias.name
            self.assertFalse(copied.is_symlink(), "the sysroot must carry the alias's bytes, not an absolute target")
            archive = root / "part.zip"
            make_parts.write_zip(str(archive), str(root), "sysroot", None)
            dest = root / "unpacked"
            linux_update._unpack(archive.read_bytes(), str(dest), None)
            self.assertEqual((dest / "lib" / "x86_64-linux-gnu" / alias.name).read_bytes(), real.read_bytes())
            self.assertEqual(os.readlink(alias), str(real))
            self.assertEqual(real.read_bytes(), b"private library bytes")


if __name__ == "__main__":
    unittest.main()
