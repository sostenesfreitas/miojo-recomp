"""nx2/chunks.py's smaller chunks: besides chunks.txt's boundaries, a chunk starts at the first function at or after each SUB_STRIDE
of guest address (sub_starts). Those depend on the functions' addresses alone, so they stay put however much C each function's
translation is; a function found where none was found before moves at most one of them; they are not saved to chunks.txt.

Nothing here runs a compiler or needs the game.

python -I -B tests/test_sub_chunks.py
"""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party" / "a64-kit" / "lift"), str(ROOT / "third_party" / "a64-kit" / "tools")]
spec = importlib.util.spec_from_file_location("chunks_under_test", ROOT / "nx2" / "chunks.py")
try:
    chunks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(chunks)
except ImportError as e:   # (a64_lift needs the emitter's Python side; the pure functions are what is checked here)
    raise SystemExit("skip: %s" % e)

S = 0x1000


class SubStartsTests(unittest.TestCase):
    def test_the_first_function_past_each_stride(self):
        funcs = [0x80000000, 0x80000400, 0x80001010, 0x80001800, 0x80003004, 0x80003008]
        self.assertEqual(chunks.sub_starts(funcs, S), {0x80001010, 0x80003004})

    def test_a_long_function_spans_strides_without_a_boundary_inside_it(self):
        self.assertEqual(chunks.sub_starts([0x80000000, 0x80000ff0, 0x80005000], S), {0x80005000})

    def test_a_function_found_since_moves_one_boundary_at_most(self):
        before = [0x80000000 + 0x300 * i for i in range(40)]
        after = sorted(before + [0x80004001])
        a, b = chunks.sub_starts(before, S), chunks.sub_starts(after, S)
        self.assertLessEqual(len(a ^ b), 2)   # (one boundary moved: the old one gone, the new one there)

    def test_none_when_the_stride_is_zero_or_nothing_is_there(self):
        self.assertEqual(chunks.sub_starts([0x80000000, 0x80100000], 0), set())
        self.assertEqual(chunks.sub_starts([], S), set())

    def test_chunks_end_at_them_with_the_saved_boundaries(self):
        funcs = {0x80000000: 0, 0x80000800: 0, 0x80001010: 0, 0x80002020: 0, 0x80004000: 0}
        b = chunks.Boundaries(24 << 20, sorted({0x80002020} | chunks.sub_starts(funcs, S)), funcs)
        self.assertEqual(b.ends, {0x80000800, 0x80001010, 0x80002020})   # (the function before each start)

    def test_write_uses_them_and_saves_only_chunks_txts(self):
        src = (ROOT / "nx2" / "chunks.py").read_text(encoding="utf-8")
        body = src[src.index("\ndef write("):src.index("\ndef ", src.index("\ndef write(") + 1)]
        self.assertIn("Boundaries(a64_lift.CHUNK_BYTES, sorted(set(starts) | sub_starts(L.funcs))", body)
        self.assertIn("save(img_dir, starts or b.recorded)", body)
        self.assertIn('code_\\d{4,}\\.c', body)   # (the guard: three digits a chunk's name at most)

    def test_the_stride_makes_chunks_of_a_few_mb(self):
        # (~14.8 bytes of C a guest byte, measured on the game's lift: 2-6 MB a chunk, and under a thousand chunks for 88 MB)
        self.assertTrue(2 << 20 <= chunks.SUB_STRIDE * 14.8 <= 6 << 20)
        self.assertLess(88 * (1 << 20) // chunks.SUB_STRIDE, 900)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
