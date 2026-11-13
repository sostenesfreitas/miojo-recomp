"""A program cache record is read within its own bytes (hookshot_programs.bin).

A record whose counts ran past its end was read on into the heap until an unmapped page: a crash at start in
program_cache_load. The Reader of gpu/vk/vk_shader.cpp is compiled here on its own (with AddressSanitizer when the
compiler has it) and given records cut short, counts too large for the bytes left and a count near 2^64.
python -I -B tests/test_program_cache_reader.py
"""
import shutil
import subprocess
import tempfile
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
    return text[start:end] + ";"


TEST = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
READER
static std::vector<uint8_t> bytes(std::initializer_list<uint64_t> words, size_t cut) {
    std::vector<uint8_t> v;
    for (uint64_t w : words) { uint8_t b[8]; std::memcpy(b, &w, 8); v.insert(v.end(), b, b + 8); }
    v.resize(cut);
    // (exactly the record's bytes on the heap: AddressSanitizer stops a read one byte past them)
    return v;
}
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    {   // a whole record: a list of three, then a word
        auto v = bytes({3, 10, 20, 30, 99}, 40);
        Reader r{v.data(), v.data() + v.size()};
        std::vector<uint64_t> l;
        r.list(l);
        CHECK(!r.bad && l.size() == 3 && l[2] == 30 && r.get<uint64_t>() == 99 && !r.bad);
    }
    {   // the count says three, the bytes hold two
        auto v = bytes({3, 10, 20}, 24);
        Reader r{v.data(), v.data() + v.size()};
        std::vector<uint64_t> l;
        r.list(l);
        CHECK(r.bad && l.empty());
    }
    {   // a count near 2^64: no allocation, nothing read
        auto v = bytes({~0ull, 1}, 16);
        Reader r{v.data(), v.data() + v.size()};
        std::vector<uint32_t> l;
        r.list(l);
        CHECK(r.bad && l.empty() && r.count(1) == 0);
    }
    {   // cut in the middle of a word: zeros, and bad
        auto v = bytes({7}, 5);
        Reader r{v.data(), v.data() + v.size()};
        CHECK(r.get<uint64_t>() == 0 && r.bad && !r.has(1));
    }
    {   // once bad, it stays bad
        auto v = bytes({1, 2}, 16);
        Reader r{v.data(), v.data() + v.size()};
        r.get<uint64_t>(); r.get<uint64_t>(); r.get<uint32_t>();
        CHECK(r.bad && r.get<uint64_t>() == 0);
    }
    if (!fails) std::printf("PASS\n");
    return fails != 0;
}
"""


class ProgramCacheReaderTests(unittest.TestCase):
    def test_source_uses_bounds(self):
        source = _with_cache(SOURCE)
        load = block(source, "void program_cache_read(")
        self.assertIn("Reader r{payload, payload + payload_size}", load)   # (the record's own bytes, compressed or not)
        self.assertIn("program_cache_payload(", load)
        self.assertIn("r.bad", load)
        self.assertIn("r.count(", block(source, "void get_info("))

    def test_every_reader_uses_the_payload_helper(self):
        source = _with_cache(SOURCE)
        self.assertIn("bool program_cache_payload(", source)
        for marker in ("void program_cache_read(", "void program_cache_each(", "SeedVariants seed_compute("):
            self.assertIn("program_cache_payload(", block(source, marker), marker)

    def test_reader_stays_in_record(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no C++ compiler")
        reader = block(_with_cache(SOURCE), "struct Reader {")
        with tempfile.TemporaryDirectory(prefix="hookshot-program-cache-") as tmp:
            src, binary = Path(tmp) / "t.cpp", Path(tmp) / "t"
            src.write_text(TEST.replace("READER", reader), encoding="utf-8")
            base = [compiler, "-std=c++20", "-Wall", "-Wextra", "-O1", "-g", str(src), "-o", str(binary)]
            build = subprocess.run(base[:1] + ["-fsanitize=address"] + base[1:], text=True, capture_output=True)
            if build.returncode:   # (no AddressSanitizer here: the checks of the reader's answers still run)
                build = subprocess.run(base, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_descriptor_counts_are_bounded(self):
        # (two cached counts of 2^31 wrapped the layout's sum to 0; one made the binds loop billions of times)
        source = _with_cache(SOURCE)
        self.assertIn("if (!descriptor_counts_fit(i)) r.bad = true;", block(source, "void get_info("))
        draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
        self.assertIn("uint64_t total = 0;", block(draw, "void ensure_layout("))
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no C++ compiler")
        start = source.index("constexpr uint32_t kMaxDescriptorCount")
        fit = source[start:source.index("void get_info(", start)]
        test = "#include <cstdint>\n#include <vector>\n" + fit + r"""
struct D { uint32_t count; };
struct Info { std::vector<D> constant_buffer_descriptors, storage_buffers_descriptors, texture_buffer_descriptors,
                             image_buffer_descriptors, texture_descriptors, image_descriptors; };
int main() {
    Info i;
    i.texture_descriptors = {{1}, {kMaxDescriptorCount}};
    if (!descriptor_counts_fit(i)) return 1;
    i.image_descriptors = {{0x80000000u}};
    if (descriptor_counts_fit(i)) return 2;
    i.image_descriptors.clear();
    i.storage_buffers_descriptors = {{kMaxDescriptorCount + 1}};
    return descriptor_counts_fit(i) ? 3 : 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="hookshot-descriptor-counts-") as tmp:
            src, binary = Path(tmp) / "t.cpp", Path(tmp) / "t"
            src.write_text(test, encoding="utf-8")
            build = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", str(src), "-o", str(binary)],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            self.assertEqual(subprocess.run([str(binary)]).returncode, 0)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
