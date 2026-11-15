"""Game-free: a game folder's hookshot starts the program that made the game (its builder= line: the launcher, or a 2.x
builder AppImage, which plays it), and the game itself when that program is gone."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform.startswith("linux"), "actual POSIX folder launcher")
class FolderDispatch(unittest.TestCase):
    def test_recorded_builder_owns_its_game_with_a_game_fallback(self):
        with tempfile.TemporaryDirectory(prefix="builder-dispatch-") as folder:
            root = Path(folder)
            game = root / "Authored 'game % folder"
            (game / "bin").mkdir(parents=True)
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder/linux/hookshot-folder", script)
            script.chmod(0o700)
            recorded = root / "Recorded 'Builder.AppImage"
            direct = game / "bin/hookshot"
            for path, label in [(recorded, "recorded"), (direct, "game")]:
                path.write_text("#!/bin/sh\nprintf '%s\\n' " + label + " \"$@\"\n")
                path.chmod(0o700)
            stamp = game / "hookshot_version.txt"
            stamp.write_text("version=v2.1.0-rc.3\nbuilder=" + str(recorded) + "\n")
            def run(expected):
                ran = subprocess.run([str(script)], capture_output=True, text=True, timeout=10)
                self.assertEqual(ran.returncode, 0, ran.stderr)
                self.assertEqual(ran.stdout.splitlines(), [expected, "--launch", str(game)] if expected != "game" else ["game"])
            run("recorded")
            recorded.chmod(0o600)
            run("game")
            recorded.unlink()
            run("game")
            stamp.unlink()
            run("game")


if __name__ == "__main__":
    unittest.main()
