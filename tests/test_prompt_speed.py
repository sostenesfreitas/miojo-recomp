"""Game-free: the button prompts made faster give the same results (nx2/prompt_assets.py read_zs, guide_route, face_jobs).

  - read_zs reads a frame that states its size in one call, and one that does not as a stream: the same bytes as the
    decompressobj it replaced, with the romfs's kind of dictionary;
  - guide_route widens the lines that stay once for all the lines routed round the same image: the way it finds is the
    same as with the widening made afresh, and an image the cache does not hold is widened anew;
  - face_jobs: HOOKSHOT_PROMPT_JOBS first, else one process a face, at most the CPU threads and one per GB available
    (each holds the 244 MB Common.bntx), and never none.

python -I -B tests/test_prompt_speed.py
"""
import os
from pathlib import Path
import random
import sys
import tempfile
import unittest
from unittest import mock

RUN_ALL_MODULES = ["PIL", "numpy", "zstandard"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets as P  # noqa: E402

SIZE = (1555, 1511)   # (ControllerGuide_Pro_00 and _01)


class ReadZs(unittest.TestCase):
    def test_sized_and_unsized_frames_read_the_same(self):
        import zstandard
        rnd = random.Random(7)
        words = [bytes(rnd.randrange(256) for _ in range(rnd.randrange(3, 12))) for _ in range(300)]
        data = b"".join(rnd.choice(words) for _ in range(200000))   # (~1.5 MB, compressible as textures are)
        dictionary = b"".join(words) * 4
        zd = zstandard.ZstdCompressionDict(dictionary)
        with tempfile.TemporaryDirectory(prefix="hookshot-prompt-zs-") as romfs:
            for name, sized in (("sized.zs", True), ("unsized.zs", False)):
                with self.subTest(name):
                    frame = zstandard.ZstdCompressor(dict_data=zd, write_content_size=sized).compress(data)
                    self.assertEqual(zstandard.frame_content_size(frame) > 0, sized)
                    Path(romfs, name).write_bytes(frame)
                    self.assertEqual(P.read_zs(romfs, name, dictionary), data)
                    before = zstandard.ZstdDecompressor(dict_data=zstandard.ZstdCompressionDict(dictionary))
                    self.assertEqual(before.decompressobj().decompress(frame), data)   # (main's way, the same bytes)


class WidenedOnce(unittest.TestCase):
    def test_the_same_way_with_and_without_the_cache(self):
        from PIL import Image, ImageDraw
        stays = Image.new("L", SIZE, 0)
        d = ImageDraw.Draw(stays)
        d.line((P.BARS[0], 333, 450, 333, 450, 500), fill=255, width=6)   # (a line that stays, for the others to keep off)
        d.line((P.BARS[1], 558, 1100, 558), fill=255, width=6)
        where = P.parts("switch")
        cases = [(0, 667, "lstick"), (1, 713, "south"), (0, 333, "lshoulder")]
        P._WIDENED.clear()
        cached = [P.guide_route(SIZE, bar, row, where, pad, stays) for bar, row, pad in cases]
        self.assertIs(P._WIDENED[id(stays)][0], stays, "the widened lines were not kept for the next line")
        fresh = []
        for bar, row, pad in cases:
            P._WIDENED.clear()
            fresh.append(P.guide_route(SIZE, bar, row, where, pad, stays))
        self.assertEqual(cached, fresh)

    def test_another_image_is_widened_anew(self):
        from PIL import Image, ImageDraw
        where = P.parts("switch")
        empty = Image.new("L", SIZE, 0)
        P._WIDENED.clear()
        P.guide_route(SIZE, 0, 667, where, "lstick", empty)
        blocked = Image.new("L", SIZE, 0)
        ImageDraw.Draw(blocked).rectangle((P.BARS[0], 600, 900, 740), fill=255)   # (over where the empty one's line went)
        way = P.guide_route(SIZE, 0, 667, where, "lstick", blocked)
        self.assertIs(P._WIDENED[id(blocked)][0], blocked)
        P._WIDENED.clear()
        self.assertEqual(way, P.guide_route(SIZE, 0, 667, where, "lstick", blocked))


class FaceJobs(unittest.TestCase):
    def setUp(self):
        env = mock.patch.dict(os.environ)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("HOOKSHOT_PROMPT_JOBS", None)

    def jobs(self, faces, cpus, gb):
        with mock.patch.object(P.os, "cpu_count", return_value=cpus):
            if os.name == "nt":
                import ctypes
                real = ctypes.windll.kernel32.GlobalMemoryStatusEx

                def status(p):
                    real(p)
                    p._obj.avail = int(gb * (1 << 30))
                    return 1
                with mock.patch.object(ctypes.windll.kernel32, "GlobalMemoryStatusEx", status):
                    return P.face_jobs(faces)
            text = "MemTotal: 16000000 kB\nMemAvailable: %d kB\n" % int(gb * (1 << 20))
            with mock.patch("builtins.open", mock.mock_open(read_data=text)):
                return P.face_jobs(faces)

    def test_one_a_face_within_threads_and_memory(self):
        self.assertEqual(self.jobs(5, 12, 10.0), 5)
        self.assertEqual(self.jobs(5, 4, 10.0), 4)
        self.assertEqual(self.jobs(5, 12, 2.5), 2)
        self.assertEqual(self.jobs(5, 12, 0.4), 1)

    def test_the_variable_first(self):
        os.environ["HOOKSHOT_PROMPT_JOBS"] = "3"
        self.assertEqual(self.jobs(5, 12, 0.4), 3)


if __name__ == "__main__":
    unittest.main()
