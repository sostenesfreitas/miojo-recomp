"""Two switches left over from past measurements are gone, their default kept for everyone: RX_SOFT_WRITE_WATCH (=0 built
with Windows' write watch instead of the soft one, rt/a64.h A64_MARK) and HOOKSHOT_OCCLUSION_OFF (=1 made the game skip
its occlusion queries, nx2/oot_occlusion.py). Nothing reads either any more: the soft write watch is always built in, the
guest window is never write-watched, and the game makes its occlusion queries."""
import os
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
ROOTS = ("nx2", "host", "gpu", "scripts", "builder", "docs", ".forgejo", "README.md", "CONTRIBUTING.md", "AGENTS.md")
SKIP = {"third_party", "build", "gen", "work", "node_modules", "__pycache__"}   # (third_party/a64-kit's comments: its tree hash)


def sources():
    for top in ROOTS:
        top = ROOT / top
        if top.is_file():
            yield top
            continue
        for base, dirs, files in os.walk(top):
            dirs[:] = [d for d in dirs if d not in SKIP]
            for name in files:
                if name.endswith((".py", ".c", ".h", ".cpp", ".md", ".yml", ".sh", ".ps1", ".cs")):
                    yield Path(base) / name


class SwitchesRemoved(unittest.TestCase):
    def test_neither_switch_is_read(self):
        for path in sources():
            text = path.read_text(encoding="utf-8", errors="replace")
            for switch in ("RX_SOFT_WRITE_WATCH", "HOOKSHOT_OCCLUSION_OFF", "hos_occlusion_off"):
                self.assertNotIn(switch, text, "%s: %s" % (path.relative_to(ROOT), switch))

    def test_the_defaults_stay(self):
        build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        self.assertIn('WRITE_TRACK = ["-DA64_WRITE_TRACK"]', build)
        self.assertFalse((ROOT / "nx2" / "oot_occlusion.py").exists(), "the occlusion-off patch is back")
        self.assertNotIn("oot_occlusion", (ROOT / "nx2" / "lift.py").read_text(encoding="utf-8"))
        self.assertNotIn("MEM_WRITE_WATCH", (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8"))


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    unittest.main()
