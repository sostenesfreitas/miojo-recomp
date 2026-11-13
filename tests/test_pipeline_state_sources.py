"""The sources of the shipped pipeline state list (assets/pipeline_states/: the tour's list, replaced whole, and the contributed ones,
one file each) and nx2/pipeline_states.py's add, tour, merge and check.

  - the committed sources pass check: each list whole, xz, of this build's format, holding pipelines once, with its note;
  - the merge is reproducible (the same bytes twice, from the command line too) and holds every source's pipelines;
  - the three lists the single assets/pipeline_states.bin.xz was split into still hold exactly its 11,527 pipelines;
  - add keeps only what no other contributed list holds (not the tour's), adds to its own list, and needs a note;
  - lists of both versions merge into version 2 (the version 1 ones converted), version 1 alone stays version 1;
  - tour replaces its own list and nothing else; check stops on a list without a note, a wrong count, a stray file, an
    uncompressed list or a pipeline twice.

Game-free: the lists hold shader hashes and draw state, and the rest is synthetic.
    python -I -B tests/test_pipeline_state_sources.py
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "nx2" / "pipeline_states.py"
# (assets/pipeline_states.bin.xz before the split: its pipelines, sorted and joined, hashed. Each of the three is the part a
# merge added: #1918's list, then #1935's and #2029's new pipelines)
MIGRATED = {"hckrman101-first-play": 1261, "joey-magnus-linux-dlss-hd-pack": 3015, "jarynclouatre-most-of-the-game": 7251}
MIGRATED_SHA256 = "15dcca8182bac64d1113c174154ced0561573f078d45cb284bdc1fa582742cac"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


tool = load("pipeline_states", TOOL)


def rec(i):
    return struct.pack("<Q", i) + bytes(tool.SIZE - 8)


NOTE = {"who": "someone", "gpu": "a GPU, driver 1, Linux", "build": "main", "settings": "1080p", "route": "the field"}


def quiet(f, *a):
    with contextlib.redirect_stdout(io.StringIO()):
        return f(*a)


def as_merged(recs, merged):
    """recs in the merged list's version: converted to version 2 when the merge is (a version 2 source, such as the
    tour's, makes the whole merge version 2)"""
    if any(len(r) == tool.SIZE2 for r in merged):
        (conv,), lost = tool.as_v2([list(recs)], force=True)
        assert lost == 0
        return conv
    return list(recs)


class Committed(unittest.TestCase):
    def test_check_passes(self):
        r = subprocess.run([sys.executable, "-I", str(TOOL), "check"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("merged:", r.stdout)

    def test_merge_is_reproducible(self):
        with tempfile.TemporaryDirectory() as d:
            outs = []
            for n in ("a.bin", "b.bin", "a.bin.xz", "b.bin.xz"):
                r = subprocess.run([sys.executable, "-I", str(TOOL), "merge", "--out", os.path.join(d, n)],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                outs.append(Path(d, n).read_bytes())
            self.assertEqual(outs[0], outs[1])
            self.assertEqual(outs[2], outs[3])
            recs = tool.parse(outs[0], "merged", strict=True)
            want = set()
            for p in tool.sources(tool.sources_dir()):
                want.update(tool.read(p, strict=True))
            want = set(as_merged(want, recs))
            self.assertEqual(set(recs), want)
            self.assertEqual(len(recs), len(want))

    def test_split_list_lost_nothing(self):
        c = Path(tool.sources_dir(), tool.CONTRIB)
        old = []
        for name, n in MIGRATED.items():
            # (a list may grow: add keeps the pipelines it holds first, so the migrated ones are its first n)
            recs = tool.read(str(c / (name + tool.EXT)), strict=True)
            self.assertGreaterEqual(len(recs), n, name)
            old += recs[:n]
        self.assertEqual(len(set(old)), sum(MIGRATED.values()), "the split lists overlap")
        self.assertEqual(hashlib.sha256(b"".join(sorted(old))).hexdigest(), MIGRATED_SHA256,
                         "the split lists no longer hold assets/pipeline_states.bin.xz's pipelines")
        merged = tool.merged(tool.sources_dir())
        self.assertTrue(set(as_merged(old, merged)) <= set(merged))


class Tool(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp(prefix="hookshot-state-sources-"))
        self.src = self.d / "src"

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def raw(self, name, recs):
        path = str(self.d / name)
        tool.write(path, recs)
        return path

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL)] + list(args) + ["--dir", str(self.src)], capture_output=True,
                              text=True)

    def add(self, name, recs, note=NOTE):
        args = ["add", name, self.raw(name + ".bin", recs)]
        for k, v in note.items():
            args += ["--" + k, v]
        return self.run_tool(*args)

    def test_add_tour_merge_check(self):
        tour = self.raw("tour.bin", [rec(1), rec(2)])
        r = self.run_tool("tour", tour, "--gpu", "g", "--build", "b", "--settings", "s", "--route", "r")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.add("b-second", [rec(2), rec(3), rec(3), rec(4)])
        self.assertEqual(r.returncode, 0, r.stderr)
        # (the tour's rec(2) is kept: the tour's list is replaced whole)
        self.assertEqual(tool.read(str(self.src / "contrib" / "b-second.bin.xz")), [rec(2), rec(3), rec(4)])
        r = self.add("a-first", [rec(4), rec(5)])
        self.assertEqual(tool.read(str(self.src / "contrib" / "a-first.bin.xz")), [rec(5)], "add kept another list's pipeline")
        # (adding to one's own list: its pipelines kept, the new ones after them; the note's fields kept)
        r = self.run_tool("add", "a-first", self.raw("more.bin", [rec(6), rec(5), rec(3)]))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(tool.read(str(self.src / "contrib" / "a-first.bin.xz")), [rec(5), rec(6)])
        note = tool.read_note(str(self.src / "contrib" / "a-first.md"))
        self.assertEqual((note["Who"], note["Pipelines"]), ("someone", "2"))
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        # the tour's first, then the contributed lists by name
        self.assertEqual(tool.merged(str(self.src)), [rec(1), rec(2), rec(5), rec(6), rec(3), rec(4)])
        # a new tour replaces its own list only
        r = self.run_tool("tour", self.raw("tour2.bin", [rec(7)]), "--gpu", "g", "--build", "b2", "--settings", "s", "--route", "r")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(tool.merged(str(self.src)), [rec(7), rec(5), rec(6), rec(2), rec(3), rec(4)])
        self.assertEqual(tool.read_note(str(self.src / "tour.md"))["Build"], "b2")

    def test_written_lists_are_reproducible(self):
        a = Path(self.raw("a.bin.xz", [rec(1), rec(2)])).read_bytes()
        b = Path(self.raw("b.bin.xz", [rec(1), rec(2)])).read_bytes()
        self.assertEqual(a, b)

    def test_add_refuses(self):
        self.assertNotEqual(self.add("Bad Name", [rec(1)]).returncode, 0)
        r = self.add("someone", [rec(1)], note=dict(NOTE, route=""))
        self.assertNotEqual(r.returncode, 0, "a list was added without saying its route")
        self.assertIn("--route", r.stderr)

    def test_check_stops(self):
        self.assertEqual(self.add("someone", [rec(1), rec(2)]).returncode, 0)
        c = self.src / "contrib"
        good = {p: p.read_bytes() for p in c.iterdir()}
        note = c / "someone.md"
        cases = {
            "no note": lambda: note.unlink(),
            "a field missing": lambda: note.write_text(note.read_text().replace("- Route:", "- Path:")),
            "a wrong count": lambda: note.write_text(note.read_text().replace("- Pipelines: 2", "- Pipelines: 3")),
            "a stray file": lambda: (c / "notes.txt").write_text("x"),
            "an uncompressed list": lambda: tool.write(str(c / "someone.bin.xz.raw"), [rec(1), rec(2)]) or
            os.replace(c / "someone.bin.xz.raw", c / "someone.bin.xz"),
            "a pipeline twice": lambda: tool.write(str(c / "someone.bin.xz"), [rec(1), rec(1)]),
            "an empty list": lambda: tool.write(str(c / "someone.bin.xz"), []),
        }
        for why, damage in cases.items():
            with self.subTest(why):
                for p in list(c.iterdir()):
                    p.unlink()
                for p, data in good.items():
                    p.write_bytes(data)
                damage()
                with self.assertRaises(SystemExit):
                    quiet(tool.check_sources, str(self.src))
        for p in list(c.iterdir()):
            p.unlink()
        with self.assertRaises(SystemExit):
            quiet(tool.check_sources, str(self.src))

    def test_versions_merge(self):
        # (a version 2 source: the merge, and check, have the version 1 ones converted; version 1 alone stays version 1)
        def rec2(i):
            return struct.pack("<Q", i) + bytes(tool.SIZE2 - 8)
        self.assertEqual(self.add("a-old", [rec(1), rec(2)]).returncode, 0)
        self.assertEqual(tool.load(str(self.src / "contrib" / "a-old.bin.xz"))[:8], tool.MAGIC)
        self.assertEqual(tool.merged(str(self.src)), [rec(1), rec(2)])
        r = self.run_tool("tour", self.raw("tour.bin", [rec2(3), rec2(1)]), "--gpu", "g", "--build", "b", "--settings", "s",
                          "--route", "r")
        self.assertEqual(r.returncode, 0, r.stderr)
        merged = tool.merged(str(self.src))
        self.assertEqual(merged, [rec2(3), rec2(1), tool.v1_to_v2(rec(2))], "the converted rec(1) is the tour's, kept once")
        self.assertEqual(tool.pack(merged)[:8], tool.MAGIC2)
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        # (adding to a list of version 1 from a version 2 one: the list becomes version 2, and holds what the others lack)
        self.assertEqual(self.add("a-old", [rec2(4), rec2(2)]).returncode, 0)
        self.assertEqual(tool.read(str(self.src / "contrib" / "a-old.bin.xz")),
                         [tool.v1_to_v2(rec(1)), tool.v1_to_v2(rec(2)), rec2(4)])

    def test_legacy_merge(self):
        # (the old form, which scripts and older docs use: lists merged, each pipeline once, whatever they are)
        out = str(self.d / "out.bin")
        r = subprocess.run([sys.executable, "-I", str(TOOL), "--out", out, self.raw("a.bin", [rec(1), rec(2)]),
                            self.raw("b.bin", [rec(2), rec(3)])], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(tool.read(out), [rec(1), rec(2), rec(3)])


if __name__ == "__main__":
    unittest.main()
