"""scripts/linux/deps.sh pins what the tag archives unpack to: dav1d (linked into the game's executable) and the Vulkan,
SPIR-V and VMA headers (compiled into its renderer) were the only downloads taken with no check (issue 2045).

A tag archive is made on demand by its host, so its bytes may change with the host's compression and are not pinned;
what it unpacks to may not change. This runs the script's real tree_is on authored folders (no download, nothing
built), and reads the script for the order: unpacked, then checked, before anything is built from it.

    python -I -B tests/test_linux_deps_pins.py
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

RUN_ALL_SKIP = {"windows": "runs the script's bash function with find, sort and sha256sum: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / "scripts/linux/deps.sh").read_text(encoding="utf-8")


def function(name):
    start = SCRIPT.index("\n%s() {" % name) + 1
    return SCRIPT[start:SCRIPT.index("\n}\n", start) + 3]


def tree_sha(folder):
    """What tree_is computes, written again here: sha256sum's lines for the files in byte order of their ./names"""
    names = sorted((b"./" + os.path.relpath(os.path.join(d, f), folder).encode() for d, _, files in os.walk(folder) for f in files))
    lines = b"".join(hashlib.sha256((Path(folder) / n[2:].decode()).read_bytes()).hexdigest().encode() + b"  " + n + b"\n"
                     for n in names)
    return hashlib.sha256(lines).hexdigest()


class TreeIsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hookshot-deps-pins-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tree, self.downloads = self.root / "unpacked", self.root / "downloads"
        (self.tree / "include/vulkan").mkdir(parents=True)
        self.downloads.mkdir()
        (self.tree / "include/vulkan/vulkan.h").write_text("authored header\n")
        (self.tree / "include/a b.h").write_text("authored header with a space in its name\n")
        (self.tree / "LICENSE").write_bytes(b"")
        (self.downloads / "headers.tar.gz").write_bytes(b"authored download")
        self.pin = tree_sha(self.tree)

    def check(self, pin=None):
        script = 'set -euo pipefail\nDL="$1"\n%s\ntree_is "$2" "$3" headers.tar.gz\necho checked\n' % function("tree_is")
        return subprocess.run(["bash", "-c", script, "bash", str(self.downloads), str(self.tree), pin or self.pin],
                              capture_output=True, text=True)

    def stopped(self, why):
        run = self.check()
        self.assertEqual(run.returncode, 1, why)
        self.assertIn("headers.tar.gz: not the expected files", run.stdout)
        self.assertNotIn("checked", run.stdout)
        # (as fetch does for a download that differs: neither it nor what it unpacked to is left for the next run)
        self.assertFalse(self.tree.exists())
        self.assertFalse((self.downloads / "headers.tar.gz").exists())
        return run.stdout

    def test_the_pinned_files_pass_and_are_kept(self):
        run = self.check()
        self.assertEqual((run.returncode, run.stdout), (0, "checked\n"), run.stderr)
        self.assertTrue((self.tree / "include/vulkan/vulkan.h").is_file())
        self.assertTrue((self.downloads / "headers.tar.gz").is_file())

    def test_a_changed_byte_stops(self):
        (self.tree / "include/vulkan/vulkan.h").write_text("authored header.\n")
        self.assertIn("expected " + self.pin, self.stopped("a changed file"))

    def test_an_added_file_stops(self):
        (self.tree / "include/vulkan/extra.h").write_text("")
        self.stopped("an added file")

    def test_a_missing_file_stops(self):
        (self.tree / "LICENSE").unlink()
        self.stopped("a missing file")

    def test_a_moved_file_stops(self):
        (self.tree / "include/a b.h").rename(self.tree / "include/vulkan/a b.h")
        self.stopped("the same contents under another name")

    def test_a_link_stops_even_with_the_files_unchanged(self):
        os.symlink("/etc/hostname", self.tree / "include/link.h")
        self.assertIn("link.h is not a file", self.stopped("a link beside the pinned files"))

    def test_the_message_gives_the_new_pin_for_a_version_change(self):
        run = self.check("0" * 64)
        self.assertEqual(run.returncode, 1)
        self.assertIn("unpacks to is %s, expected %s" % (self.pin, "0" * 64), run.stdout)


class ScriptTests(unittest.TestCase):
    def test_each_tag_archive_is_checked_after_it_is_unpacked_and_before_it_is_used(self):
        dav1d = SCRIPT.index('tar -xzf "$DL/dav1d.tar.gz"')
        self.assertLess(dav1d, SCRIPT.index('tree_is "$DEPS/src-dav1d" "$DAV1D_TREE" dav1d.tar.gz'))
        self.assertLess(SCRIPT.index('tree_is "$DEPS/src-dav1d"'), SCRIPT.index("meson setup build"))
        headers = SCRIPT[SCRIPT.index("get_headers() {"):SCRIPT.index("mark cpm")]
        self.assertLess(headers.index("tar -xzf"), headers.index('tree_is "$DEPS/cpm/$3/$2" "$4" "$3-$2.tar.gz"'))
        calls = re.findall(r'^    get_headers \S+ "\$\w+" \S+(.*)$', SCRIPT, re.M)
        self.assertEqual(len(calls), 3)
        for rest in calls:
            self.assertRegex(rest, r'^ "\$\w+_TREE"$')

    def test_every_pin_is_a_sha256(self):
        pins = dict(re.findall(r"^(\w+_TREE)=(\S+)$", SCRIPT, re.M))
        self.assertEqual(sorted(pins), ["DAV1D_TREE", "SPIRV_HEADERS_TREE", "VK_HEADERS_TREE", "VMA_TREE"])
        for name, value in pins.items():
            self.assertRegex(value, r"^[0-9a-f]{64}$", name)
        self.assertEqual(len(set(pins.values())), 4)

    def test_no_download_is_left_without_a_check(self):
        """Every fetch names its SHA-256, or the lines after it check the file or what it unpacks to"""
        lines = SCRIPT.split("\n")
        for i, line in enumerate(lines):
            if not re.match(r"\s+fetch \"https://", line):
                continue
            following = "\n".join(lines[i + 1:i + 5])
            checked = re.search(r'"\$\w+_SHA256"$', line) or "sha256sum -c" in following or "tree_is " in following
            self.assertTrue(checked, "line %d: %s" % (i + 1, line.strip()))


if __name__ == "__main__":
    unittest.main()
