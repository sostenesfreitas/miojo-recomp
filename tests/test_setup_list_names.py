"""The Python builder reads the setup list by builder/setup/setup.c's rules, refusing a bad list before any folder is removed

python -I -B tests/test_setup_list_names.py
"""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "builder" / "py"))
from hookshot_builder import setup_parts  # noqa: E402

SHA = "0123456789abcdef" * 4


def read(tag="v2", sha=SHA, dest="HookshotBuilder/tools", size="100"):
    text = "tag %s\npart %s %s %s - llvm-mingw https://x/y.zip\nrun a.exe\n" % (tag, sha, size, dest)
    with mock.patch.object(setup_parts.Http, "get", lambda self, url, **kw: text.encode()):
        return setup_parts.read_manifest(lambda s: None)


class SetupListNamesTests(unittest.TestCase):
    def test_a_list_with_a_name_outside_the_install_folder_or_a_bad_checksum_or_size_is_refused(self):
        self.assertEqual(read()[0], "v2")
        for bad in ({"tag": ".."}, {"tag": "C:\\Users"}, {"tag": "/tmp"}, {"dest": "a/../.."}, {"sha": SHA.upper()},
                    {"size": "1e3"}):
            with self.subTest(**bad), self.assertRaises(setup_parts.SetupError):
                read(**bad)

    def test_a_part_is_reused_only_for_its_exact_checksum(self):
        with tempfile.TemporaryDirectory() as base, mock.patch.dict(os.environ, {"HOOKSHOT_SETUP_BASE": base}):
            v = Path(base, "versions")
            (v / "v1" / "tools" / "llvm-mingw").mkdir(parents=True)
            (v / "current.txt").write_text("v1")
            p = setup_parts.Part(SHA, 100, "tools", "-", "llvm-mingw", "https://x/y.zip")
            for listed, want in (("f" + SHA, False), (SHA, True)):
                (v / "v1" / "parts.txt").write_text(listed + "\n")
                self.assertEqual(setup_parts.reuse_part("v2", p, str(v / "v2.part")), want)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))