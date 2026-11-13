"""The shader list (hookshot_shaders.bin, nx2/shader_census.py) is made again only when what it is made from changed.

Every build ran the census (4 s, 514 MB written) though an update seldom changes the game's shader files or the census. make_game.py
notes what the list was made from (shader_list_note: the census, the Python, the names, sizes and times of the files it reads, and
the list file itself) and skips the census when all are the same. Synthetic files only.

python -I -B tests/test_shader_list_skip.py
"""
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_game_under_test", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class ShaderListSkipTests(unittest.TestCase):
    def folder(self, d):
        romfs, work, out = Path(d) / "romfs", Path(d) / "work", Path(d) / "game" / "hookshot_shaders.bin"
        for p in (romfs / "Shader", romfs / "Pack", romfs / "Model", work, out.parent):
            p.mkdir(parents=True)
        (romfs / "Shader" / "a.bfsha.zs").write_bytes(b"shader")
        (romfs / "Pack" / "ZsDic.pack.zs").write_bytes(b"dict")
        (romfs / "Model" / "m.bfres.zs").write_bytes(b"model")
        out.write_bytes(b"list")
        return str(romfs), str(work), str(out)

    def test_the_same_files_skip_the_census(self):
        with tempfile.TemporaryDirectory() as d:
            romfs, work, out = self.folder(d)
            self.assertFalse(make_game.shader_list_current(work, romfs, out))   # (no note: a first build)
            make_game.note_shader_list(work, romfs, out)
            self.assertTrue(make_game.shader_list_current(work, romfs, out))
            Path(romfs, "Model", "m.bfres.zs").write_bytes(b"other model")   # (a file the census does not read)
            self.assertTrue(make_game.shader_list_current(work, romfs, out))

    def test_a_changed_shader_file_or_list_makes_it_again(self):
        with tempfile.TemporaryDirectory() as d:
            romfs, work, out = self.folder(d)
            make_game.note_shader_list(work, romfs, out)
            Path(romfs, "Shader", "a.bfsha.zs").write_bytes(b"other shader")
            self.assertFalse(make_game.shader_list_current(work, romfs, out))
        with tempfile.TemporaryDirectory() as d:
            romfs, work, out = self.folder(d)
            make_game.note_shader_list(work, romfs, out)
            Path(out).write_bytes(b"a list of another build")
            self.assertFalse(make_game.shader_list_current(work, romfs, out))
        with tempfile.TemporaryDirectory() as d:
            romfs, work, out = self.folder(d)
            make_game.note_shader_list(work, romfs, out)
            os.remove(out)
            self.assertFalse(make_game.shader_list_current(work, romfs, out))
            self.assertFalse(make_game.shader_list_current(work, romfs, out + ".elsewhere"))

    def test_the_census_reads_what_the_key_names(self):
        """CENSUS_READS follows shader_census.py: its archives() folders and the dictionary file"""
        census = (ROOT / "nx2" / "shader_census.py").read_text(encoding="utf-8")
        self.assertIn('for folder in ("Shader", "Layout2Shader", "Lib", "Movie", "Effect"):', census)
        self.assertIn('os.path.join(romfs, "Pack", "ZsDic.pack.zs")', census)
        self.assertEqual(make_game.CENSUS_READS[:5], ("Shader", "Layout2Shader", "Lib", "Movie", "Effect"))


if __name__ == "__main__":
    unittest.main()
