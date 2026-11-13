"""Authored CPM-part fixtures: scripts/release_parts.py package --cpm-add puts dav1d (the AV1 movies' decoder on Windows,
scripts/windows/dav1d.sh) into a new CPM part when the latest release's lacks it, with every file of that part kept, and
leaves the part to be reused as it is when it has it."""
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release_parts", ROOT / "scripts/release_parts.py")
release_parts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_parts)

PREFIX = "HookshotBuilder/tools/cpm/"
OLD = {"ffmpeg/9.0.2-windows-amd64/lib/avcodec.lib": b"authored ffmpeg",
       "vulkanheaders/v1.4.345/include/vulkan/vulkan.h": b"authored headers"}
DAV1D = {"dav1d/1.5.4/include/dav1d/dav1d.h": b"authored dav1d header",
         "dav1d/1.5.4/lib/libdav1d.a": b"!<arch>\nauthored",
         "dav1d/1.5.4/COPYING": b"BSD 2-clause"}


class CpmAdd(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hookshot-cpm-add-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.part = self.root / "authored-cpm.zip"
        self.tools = self.root / "tools"
        self.add = self.root / "dav1d-cpm"
        for rel, data in DAV1D.items():
            (self.add / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.add / rel).write_bytes(data)

    def cpm_part(self, files):
        with zipfile.ZipFile(self.part, "w") as archive:
            for rel, data in files.items():
                archive.writestr(PREFIX + rel, data)

    def run_add(self):
        with patch.object(release_parts, "named_part", return_value=str(self.part)), \
                patch.object(release_parts, "get", side_effect=AssertionError("no network in this fixture")), \
                redirect_stdout(io.StringIO()):
            self.assertEqual(release_parts.cpm_add("authored-setup.txt", str(self.add), str(self.tools)), 0)

    def test_part_without_dav1d_is_unpacked_with_it_added(self):
        self.cpm_part(OLD)
        self.run_add()
        for rel, data in {**OLD, **DAV1D}.items():
            self.assertEqual((self.tools / "cpm" / rel).read_bytes(), data, rel)

    def test_part_with_dav1d_is_reused_as_it_is(self):
        self.cpm_part({**OLD, **DAV1D})
        self.run_add()
        self.assertFalse(self.tools.exists(), "nothing unpacked: make_setup_parts.py --reuse keeps the release's part")

    def test_another_dav1d_version_is_added(self):
        self.cpm_part({**OLD, **{rel.replace("1.5.4", "1.5.3"): data for rel, data in DAV1D.items()}})
        self.run_add()
        self.assertTrue((self.tools / "cpm/dav1d/1.5.4/lib/libdav1d.a").exists())
        self.assertTrue((self.tools / "cpm/dav1d/1.5.3/lib/libdav1d.a").exists())


if __name__ == "__main__":
    unittest.main()
