"""Offline bake dependencies: real dictionary frames, native fallback and the constexpr requirement."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import bake_mimo
from bake_ds import fixture, romenum
import zstandard


class Portability(unittest.TestCase):
    def test_fixture_needs_no_external_compressor(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(
                fixture.subprocess, "run", side_effect=AssertionError("external compressor used")):
            self.assertEqual(fixture.build(folder, with_dict=True)["dict"], "written")
            dictionaries = romenum.dictionaries(folder)
            self.assertEqual(len(dictionaries), 1)
            frame = (Path(folder) / "Shader/Dict.bfsha.zs").read_bytes()
            self.assertTrue(romenum.zstd().decompress(frame, dictionaries).startswith(b"BNSH"))

    def test_dictionary_retry_and_rejected_frames(self):
        dictionary = bytes(range(256)) * 16
        payload = dictionary[100:1500] * 3
        frame = zstandard.ZstdCompressor(
            dict_data=zstandard.ZstdCompressionDict(dictionary)).compress(payload)
        reader = romenum._PythonZstd()
        with self.assertRaises(ValueError):
            reader.decompress(frame)
        self.assertEqual(reader.decompress(frame, [b"wrong dictionary", dictionary]), payload)
        with self.assertRaises(ValueError):
            reader.decompress(frame[:-1], [dictionary])
        with self.assertRaises(ValueError):
            reader.decompress(frame + b"trailing corrupt bytes", [dictionary])
        self.assertEqual(reader.decompress(b"plain container"), b"plain container")

    def test_native_fallback_when_python_package_absent(self):
        with mock.patch.object(romenum, "_ZSTD", None), mock.patch.object(
                romenum, "_PythonZstd", side_effect=ImportError), mock.patch.object(
                romenum, "_Zstd", return_value="native reader") as native:
            self.assertEqual(romenum.zstd(), "native reader")
            native.assert_called_once_with()

    def test_unknown_size_frame_under_bounded_memory(self):
        payload = b"unknown frame payload" * 100000
        frame = zstandard.ZstdCompressor(write_content_size=False).compress(payload)
        reader = romenum._PythonZstd()
        self.assertEqual(reader.decompress(frame), payload)
        with self.assertRaises(ValueError):
            reader.decompress(frame[:-1])
        with self.assertRaises(ValueError):
            reader.decompress(frame + b"trailing corrupt bytes")
        if sys.platform.startswith("linux"):
            code = """import sys, resource, zstandard
sys.path.insert(0, sys.argv[1])
from bake_ds import romenum
frame = zstandard.ZstdCompressor(write_content_size=False).compress(b'x' * 2000000)
resource.setrlimit(resource.RLIMIT_AS, (512 << 20, 512 << 20))
assert len(romenum._PythonZstd().decompress(frame)) == 2000000
"""
            p = subprocess.run([sys.executable, "-I", "-B", "-c", code, str(ROOT / "nx2")],
                               capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_real_compiler_accepts_assert_failure_path(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.dict(os.environ, {"BAKE_WORK": folder}):
            selected = bake_mimo.cxx_std(CXX)
            self.assertIn(selected, ("-std=c++23", "-std=c++2b"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args, rest = parser.parse_known_args()
    CXX = args.cxx
    unittest.main(argv=[sys.argv[0], *rest])
