"""Game-free: the extraction notes which romfs .zs files do not start as zstd as it writes them, and the build's damaged-
texture step takes that list instead of opening every file again (#1876: ~100,000 opens, minutes under a virus scanner,
and the Builder looked frozen):
python -I -B nx2/test_nup_zstd_heads.py"""

import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nup

SOURCE = Path(__file__).resolve().parent / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_heads_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

WHOLE = nup.ZSTD + b"authored whole member"
DAMAGED = b"\x00\x00\x00\x00authored damaged member"
MEMBERS = {
    "exefs": {"main": b"NSO0authored"},
    "romfs": {"Model/whole.bntx.zs": WHOLE, "Model/damaged.bntx.zs": DAMAGED, "Model/other.bntx.zs": WHOLE,
              "Data/plain.txt": b"\x00not a zs file"},
}


class Package:
    """Authored members written in two pieces, as a package reader may"""
    def walk(self, part):
        for rel, data in MEMBERS[part].items():
            yield rel, (part, rel, len(data), 0, len(data), 0)

    def copy_to(self, record, output, name):
        data = MEMBERS[record[0]][record[1]]
        output.write(data[:2])
        output.write(memoryview(data)[2:])


class Heads(unittest.TestCase):
    def extract(self, out):
        return nup.extract(Package(), {"exefs": "exefs", "romfs": "romfs"}, out, quiet=True)

    def test_the_extraction_lists_the_damaged_zs_files(self):
        with tempfile.TemporaryDirectory() as out:
            self.extract(out)
            self.assertEqual(nup.read_heads(os.path.join(out, nup.HEADS_NAME)), ["Model/damaged.bntx.zs"])
            self.assertEqual(Path(out, "romfs", "Model", "damaged.bntx.zs").read_bytes(), DAMAGED)

    def test_a_rerun_opens_only_the_files_it_kept(self):
        with tempfile.TemporaryDirectory() as out:
            self.extract(out)
            os.remove(os.path.join(out, nup.HEADS_NAME))
            self.extract(out)   # (every member is there already: each .zs is opened for its head)
            self.assertEqual(nup.read_heads(os.path.join(out, nup.HEADS_NAME)), ["Model/damaged.bntx.zs"])

    def test_no_list_without_the_whole_romfs(self):
        with tempfile.TemporaryDirectory() as out:
            nup.extract(Package(), {"exefs": "exefs", "romfs": "romfs"}, out, only="exefs", quiet=True)
            self.assertIsNone(nup.read_heads(os.path.join(out, nup.HEADS_NAME)))

    def test_the_build_takes_the_list_and_opens_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            out, game = os.path.join(d, "authored game"), os.path.join(d, "authored game", ".game")
            os.makedirs(game)
            self.extract(game)
            romfs = os.path.join(out, "romfs")
            os.replace(os.path.join(game, "romfs"), romfs)
            stamp = "unpacked by this builder: authored.nup 123"
            Path(out, ".hookshot_romfs").write_text(stamp + "\n")
            make_game.keep_heads(out, game, stamp)
            with mock.patch.object(nup, "damaged_game_files", side_effect=AssertionError("every file opened again")):
                make_game.stand_in_damaged_files(out, romfs)
            # (the damaged file now holds its stand-in: a whole one of the same kind)
            self.assertEqual(Path(romfs, "Model", "damaged.bntx.zs").read_bytes(), WHOLE)
            self.assertFalse(os.path.exists(os.path.join(out, make_game.HEADS)))
            self.assertEqual(Path(out, ".hookshot_romfs_checked").read_text(), "resolved-v2\n" + stamp + "\n")
            # (#2237: the list is kept in the game folder, where the build's output is not)
            note = Path(out, make_game.DAMAGED_NOTE)
            listed = note.read_text(encoding="utf-8")
            self.assertTrue(listed.startswith("Your game dump is incomplete: 1 game file in it is damaged"))
            self.assertIn(os.path.join("Model", "damaged.bntx.zs") + ": in its place ", listed)
            # (a whole dump unpacked next: the list goes)
            Path(out, ".hookshot_romfs").write_text("unpacked by this builder: whole.nup 456\n")
            make_game.stand_in_damaged_files(out, romfs)
            self.assertFalse(note.exists())

    def test_a_list_from_another_unpack_is_not_taken(self):
        with tempfile.TemporaryDirectory() as d:
            out, game = os.path.join(d, "authored game"), os.path.join(d, "authored game", ".game")
            os.makedirs(game)
            self.extract(game)
            romfs = os.path.join(out, "romfs")
            os.replace(os.path.join(game, "romfs"), romfs)
            make_game.keep_heads(out, game, "unpacked by this builder: an older.nup 1")
            Path(out, ".hookshot_romfs").write_text("unpacked by this builder: authored.nup 123\n")
            with mock.patch.object(nup, "damaged_game_files", wraps=nup.damaged_game_files) as opened:
                make_game.stand_in_damaged_files(out, romfs)
            opened.assert_called_once()
            self.assertEqual(Path(romfs, "Model", "damaged.bntx.zs").read_bytes(), WHOLE)

    def test_a_stamp_keeps_a_name_outside_the_code_page(self):
        """#2246: "Zelda™⁚ Ocarina.nup" stopped the build with a UnicodeEncodeError where Windows' code page is cp1252"""
        with tempfile.TemporaryDirectory() as d, mock.patch("locale.getencoding", return_value="cp1252"):
            stamp = "unpacked by this builder: Zelda™⁚ Ocarina.nup 123\n"
            path = os.path.join(d, ".hookshot_romfs")
            make_game.write_stamp(path, stamp)
            self.assertEqual(Path(path).read_bytes(), stamp.encode("utf-8"))
            self.assertEqual(make_game.read_stamp(path), stamp)
            # (a stamp an earlier builder wrote in the code page still reads as it was: no second unpack)
            old = "unpacked by this builder: Zelda™ Ocarina.nup 123\r\n"
            Path(path).write_bytes(old.encode("cp1252"))
            self.assertEqual(make_game.read_stamp(path), old)
            # (a byte the code page has no letter for: no stamp, not a name with a replaced letter)
            Path(path).write_bytes(b"unpacked by this builder: authored \x81.nup 22\n")
            self.assertEqual(make_game.read_stamp(path), "")

    def test_resolved_check_keeps_unicode_unpack_stamp_and_skips_only_after_success(self):
        with tempfile.TemporaryDirectory() as out, mock.patch("locale.getencoding", return_value="cp1252"):
            romfs = Path(out, "romfs")
            romfs.mkdir()
            stamp = "unpacked by this builder: authored\u2122\u205a.nup 123\n"
            make_game.write_stamp(Path(out, ".hookshot_romfs"), stamp)
            checked = Path(out, ".hookshot_romfs_checked")
            with mock.patch.object(nup, "stand_in_damaged", return_value=[]) as scan:
                make_game.stand_in_damaged_files(out, str(romfs))
                make_game.stand_in_damaged_files(out, str(romfs))
            self.assertEqual(scan.call_count, 1)
            self.assertEqual(checked.read_bytes(), ("resolved-v2\n" + stamp).encode("utf-8"))

    def test_a_stamp_that_cannot_be_read_matches_no_name(self):
        """A letter replaced while decoding made two names equal, and the second .nup's unpack was skipped (Python's UTF-8
        mode, where the preferred encoding is UTF-8 again)"""
        with tempfile.TemporaryDirectory() as d, mock.patch("locale.getencoding", return_value="utf-8"), \
                mock.patch("locale.getpreferredencoding", return_value="utf-8"):
            path = os.path.join(d, ".hookshot_romfs")
            Path(path).write_bytes(b"unpacked by this builder: authored \xff.nup 22\n")
            self.assertNotEqual(make_game.read_stamp(path), "unpacked by this builder: authored �.nup 22\n")
            self.assertEqual(make_game.read_stamp(path), "")


if __name__ == "__main__":
    unittest.main()
