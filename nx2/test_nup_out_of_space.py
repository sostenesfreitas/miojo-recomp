"""Game-free: a NUP extraction that runs out of space names the member and its drive (issue 1388):
python -I -B nx2/test_nup_out_of_space.py"""

import errno
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nup


class Package:
    """One authored member whose write fails."""
    def __init__(self, failure):
        self.failure = failure

    def walk(self, directory):
        yield "Model/authored.zs", (0, 0, 12345, 0, 12345, 0)

    def copy_to(self, record, output, name):
        output.write(b"some bytes")
        raise self.failure


class OutOfSpace(unittest.TestCase):
    def extract(self, failure, out):
        return nup.extract(Package(failure), {"romfs": 0}, out, only="romfs", quiet=True)

    def test_full_drive_names_the_member_and_the_drive(self):
        with tempfile.TemporaryDirectory() as out:
            member = Path(os.path.realpath(out)) / "romfs" / "Model" / "authored.zs"
            seen = []

            def usage(path):
                seen.append((path, member.exists()))
                return SimpleNamespace(free=196 * 10 ** 9)

            original = OSError(errno.ENOSPC, "authored disk-full failure")
            with patch.object(nup.shutil, "disk_usage", side_effect=usage), \
                    patch.object(nup, "file_system", return_value="NTFS"), \
                    self.assertRaises(OSError) as error:
                self.extract(original, out)
            self.assertEqual(error.exception.errno, errno.ENOSPC)
            self.assertEqual(error.exception.strerror,
                             "No space left on device writing romfs/Model/authored.zs (12345 bytes): 196.0 GB free there, NTFS")
            self.assertIs(error.exception.__cause__, original)
            # (measured while the failed member still holds its space)
            # (the folder as given or as its Windows long path, \\?\...: nup.py extracts through long paths since #1497)
            plain = [(path[4:] if path.startswith("\\\\?\\") else path, held) for path, held in seen]
            self.assertEqual(plain, [(str(member.parent), True)])
            self.assertFalse(member.exists())

    def test_other_errors_pass_through(self):
        with tempfile.TemporaryDirectory() as out:
            original = PermissionError(errno.EACCES, "authored access failure")
            with self.assertRaises(PermissionError) as error:
                self.extract(original, out)
            self.assertIs(error.exception, original)

    def test_file_system_of_a_real_folder(self):
        with tempfile.TemporaryDirectory() as out:
            if os.name == "nt":
                self.assertTrue(nup.file_system(out))
            else:
                self.assertIsNone(nup.file_system(out))


if __name__ == "__main__":
    unittest.main()
