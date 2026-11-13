"""The program cache's index (hookshot_programs.idx) and the precompile's parallel reads (gpu/vk/vk_shader.cpp).

The index names each record of hookshot_programs.bin (key, size, kind) in the file's order, so a start no longer reads every
record's head across the whole file. These checks read the source: the index is written after its record, never before; it is
taken only when its stamp, its sizes and the last record's head in the file agree with it, and what lies past it is read as
before; a record's head is checked against the index when it is loaded; the precompile reads its records on threads of their
own, each with its own handle on the file, and puts them in the program table under shader_mu.
python -I -B tests/test_program_cache_index.py
"""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def _with_cache(path):
    """vk_shader.cpp with the program cache it held before (vk_progcache.cpp and .h beside it, outside the translator stamp): definitions first"""
    parts = [path.read_text(encoding="utf-8")]
    for name in ("vk_progcache.cpp", "vk_progcache.h"):
        if (path.parent / name).exists():
            parts.append((path.parent / name).read_text(encoding="utf-8"))
    return "\n".join(parts)

SOURCE = ROOT / "gpu" / "vk" / "vk_shader.cpp"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class ProgramCacheIndexTests(unittest.TestCase):
    def setUp(self):
        self.source = _with_cache(SOURCE)

    def test_index_after_record(self):
        save = block(self.source, "void program_cache_save(")
        record = save.index("fwrite(o.data(), 1, o.size(), disk_out)")
        index = save.index("fwrite(e.data(), 1, e.size(), disk_idx)")
        self.assertLess(record, index, "the index entry must follow its record")
        self.assertIn("fflush(disk_idx)", save)

    def test_index_checked_at_start(self):
        init = block(self.source, "void disk_init(")
        self.assertIn("is == stamp", init)                       # (the cache's stamp)
        self.assertIn("at + (int64_t)head_size + (int64_t)size > whole", init)   # (sizes within the file)
        self.assertIn("rec[0] == key && rec[1] == d.size", init)  # (the last record's head as the file has it)
        self.assertIn("order.clear()", init)                     # (not taken: all read as before)
        # the records past the index are read the old way, from where it ends
        self.assertLess(init.index("good = at;"), init.index("while (good + 20 <= whole)"))
        self.assertIn("idx_write(idx_path, stamp, order)", init)   # (written again unless it was whole)

    def test_scan_size_unsigned(self):
        # (a record size past 2^63 was negative as int64_t and passed the old check: the scan went backwards)
        init = block(self.source, "void disk_init(")
        self.assertIn("if (rec[1] > (uint64_t)(whole - (size_t)good - head_size)) break;", init)
        self.assertNotIn("good + 16 + (int64_t)rec[1] > whole", init)

    def test_head_checked_on_load(self):
        read = block(self.source, "void program_cache_read(")
        self.assertIn("_fseeki64(in, d.at - (int64_t)head_size, SEEK_SET)", read)
        self.assertIn("head[0] != key || head[1] != d.size", read)

    def test_parallel_reads(self):
        some = block(self.source, "size_t program_cache_load_some(")
        self.assertIn('fopen(disk_path.c_str(), "rb")', some)   # (a handle per thread)
        self.assertIn("program_cache_read(in, nullptr", some)
        # the program table is touched only after the threads are joined, under shader_mu
        self.assertLess(some.index("t.join()"), some.index("std::lock_guard lk(shader_mu)"))
        self.assertLess(some.index("std::lock_guard lk(shader_mu)"), some.index("programs[work[i].first]"))


if __name__ == "__main__":
    unittest.main()
