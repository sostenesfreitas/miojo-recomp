"""scripts/check_third_party.py, the third_party rule, over fake parts in a temporary folder (no game, no network): a clean
tree passes; a changed file fails and is named; a part with no PROVENANCE.md, or a note with no tree-sha256 line, fails; a
pull request that changes a part without its note fails against the base; --update makes a tree pass again; --upstream
compares a part with its archive (a local file:// one here) and skips a part that names none. Also: the repository's own
third_party passes."""
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("check_third_party", ROOT / "scripts" / "check_third_party.py")
ctp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ctp)


def run(*args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = ctp.main(list(map(str, args)))
    return code, out.getvalue() + err.getvalue()


class ThirdParty(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="third-party-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        lib = self.dir / "third_party" / "libfoo"
        (lib / "src").mkdir(parents=True)
        (lib / "src" / "foo.c").write_text("int foo(void) { return 1; }\n")
        (lib / "LICENSE").write_text("MIT\n")
        (lib / "PROVENANCE.md").write_text("# libfoo\n\nlibfoo 1.0, unmodified.\n")
        (self.dir / ".gitignore").write_text("*.dll\n")
        run("--root", self.dir, "--update")

    def git(self, *args):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args], cwd=self.dir, check=True,
                       capture_output=True)

    def test_a_clean_tree_passes(self):
        code, out = run("--root", self.dir)
        self.assertEqual(code, 0, out)
        self.assertRegex((self.dir / "third_party/libfoo/PROVENANCE.md").read_text(), r"\ntree-sha256: [0-9a-f]{64}\n$")
        # (build output the repository ignores is not part of it)
        (self.dir / "third_party/libfoo/src/foo.dll").write_bytes(b"MZ")
        # (and one with a non-ASCII name: without -z git quotes it in its answer, and it was hashed with the part)
        (self.dir / "third_party/libfoo/src/\N{LATIN SMALL LETTER E WITH ACUTE}.dll").write_bytes(b"MZ")
        self.git("init", "-q")
        code, out = run("--root", self.dir)
        self.assertEqual(code, 0, out)

    def test_a_modified_file_fails(self):
        (self.dir / "third_party/libfoo/src/foo.c").write_text("int foo(void) { return 2; }\n")
        code, out = run("--root", self.dir)
        self.assertEqual(code, 1)
        self.assertIn("third_party/libfoo differs", out)
        self.assertIn("Don't modify third_party/", out)

    def test_an_executable_bit_counts(self):
        if os.name == "nt":
            self.skipTest("no executable bit")
        os.chmod(self.dir / "third_party/libfoo/LICENSE", 0o755)
        self.assertEqual(run("--root", self.dir)[0], 1)

    def test_the_changed_files_are_named(self):
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "libfoo")
        (self.dir / "third_party/libfoo/src/foo.c").write_text("changed\n")
        (self.dir / "third_party/libfoo/src/new.c").write_text("new\n")
        code, out = run("--root", self.dir)
        self.assertEqual(code, 1)
        self.assertIn("third_party/libfoo/src/foo.c", out)
        self.assertIn("third_party/libfoo/src/new.c", out)

    def test_a_folder_without_a_note_fails(self):
        (self.dir / "third_party/libbar").mkdir()
        (self.dir / "third_party/libbar/bar.c").write_text("x\n")
        code, out = run("--root", self.dir)
        self.assertEqual(code, 1)
        self.assertIn("third_party/libbar has no PROVENANCE.md", out)
        (self.dir / "third_party/libbar/PROVENANCE.md").write_text("libbar, from somewhere.\n")
        code, out = run("--root", self.dir)
        self.assertEqual(code, 1)
        self.assertIn("no tree-sha256 line", out)

    def test_a_change_without_its_note_fails_against_the_base(self):
        self.git("init", "-q", "-b", "main")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "libfoo")
        self.git("switch", "-q", "-c", "change")
        (self.dir / "third_party/libfoo/src/foo.c").write_text("int foo(void) { return 3; }\n")
        self.git("commit", "-q", "-am", "edit")
        code, out = run("--root", self.dir, "--base", "main")
        self.assertEqual(code, 1)
        self.assertIn("third_party/libfoo changed (src/foo.c) and its PROVENANCE.md did not", out)
        # (a deliberate refresh: the note says so and records the new tree)
        note = self.dir / "third_party/libfoo/PROVENANCE.md"
        note.write_text(note.read_text().replace("libfoo 1.0, unmodified.", "libfoo 1.1, unmodified."))
        run("--root", self.dir, "--update", "libfoo")
        self.git("commit", "-q", "-am", "refresh")
        code, out = run("--root", self.dir, "--base", "main")
        self.assertEqual(code, 0, out)

    def test_update_makes_it_pass(self):
        (self.dir / "third_party/libfoo/src/foo.c").write_text("refreshed from upstream\n")
        self.assertEqual(run("--root", self.dir)[0], 1)
        code, out = run("--root", self.dir, "--update", "libfoo")
        self.assertEqual(code, 0)
        self.assertEqual(run("--root", self.dir)[0], 0)
        self.assertEqual((self.dir / "third_party/libfoo/PROVENANCE.md").read_text().count("tree-sha256:"), 1)

    def test_upstream(self):
        archive = self.dir / "libfoo-1.0.tar.gz"
        with tarfile.open(archive, "w:gz") as t:
            for rel in ("src/foo.c", "LICENSE"):
                t.add(self.dir / "third_party/libfoo" / rel, "libfoo-1.0/" + rel)
        sha = hashlib.sha256(archive.read_bytes()).hexdigest()
        note = self.dir / "third_party/libfoo/PROVENANCE.md"
        note.write_text(note.read_text() + "\nupstream: %s\nupstream-sha256: %s\nupstream-root: libfoo-1.0\n"
                        % (archive.as_uri(), sha))
        self.assertEqual(run("--root", self.dir, "--upstream")[0], 0)
        (self.dir / "third_party/libfoo/src/foo.c").write_text("changed\n")
        code, out = run("--root", self.dir, "--upstream")
        self.assertEqual(code, 1)
        self.assertIn("differs from upstream", out)
        self.assertIn("src/foo.c", out)
        note.write_text(note.read_text().replace(sha, "0" * 64))
        code, out = run("--root", self.dir, "--upstream")
        self.assertIn("is not the archive its note names", out)
        (self.dir / "third_party/libbar").mkdir()
        (self.dir / "third_party/libbar/PROVENANCE.md").write_text("no archive\n")
        code, out = run("--root", self.dir, "--upstream", "libbar")
        self.assertEqual(code, 0)
        self.assertIn("no upstream archive in its note, skipped", out)

    def test_the_repository_passes(self):
        code, out = run("--root", ROOT)
        self.assertEqual(code, 0, out)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    unittest.main()
