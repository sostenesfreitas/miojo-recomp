"""nx2/build.py's record of what each source's object key was (HASHES, hash_cache.json; the dev-loop proposal's A5): a source
whose size, modification time and file id are the ones recorded is not read again to name its object, and its key is the one
object_key gives; anything else about it, or a file written too near the record, is read and hashed as before.

Nothing here runs a compiler or needs the game.

python -I -B tests/test_build_hash_cache.py
"""
import builtins
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party" / "a64-kit" / "tools")]
spec = importlib.util.spec_from_file_location("build_hash_cache", ROOT / "nx2" / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

FLAGS, DEPS = ["-O1", "-DA64_POLLING"], b"/* the headers */"
MINUTE_AGO = time.time() - 60


class HashCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot-hash-cache-")
        self.addCleanup(self.temporary.cleanup)
        self.dir = Path(self.temporary.name)
        self.src = self.dir / "code_001.c"
        self.write("void f_80000000(void) {}\n")
        build.load_hashes(str(self.dir))

    def write(self, text, when=MINUTE_AGO):
        self.src.write_bytes(text.encode("utf-8"))   # (bytes: Windows' text mode would make the newlines CRLF)
        os.utime(self.src, (when, when))

    def next_build(self):
        """the record saved, then read as the next build starts"""
        build.save_hashes(str(self.dir))
        build.load_hashes(str(self.dir))

    def unread(self):
        """open() refusing the source: a key that comes back was not read from it"""
        real = builtins.open
        src = str(self.src)

        def guarded(path, *a, **k):
            if str(path) == src:
                raise AssertionError("the source was read")
            return real(path, *a, **k)
        return mock.patch("builtins.open", guarded)

    def test_the_key_is_object_keys_and_the_next_build_does_not_read_the_source(self):
        key = build.key_of(str(self.src), FLAGS, DEPS)
        self.assertEqual(key, build.object_key(str(self.src), FLAGS, DEPS))
        self.next_build()
        with self.unread():
            self.assertEqual(build.key_of(str(self.src), FLAGS, DEPS), key)
            self.assertEqual(build.content_sha1(str(self.src)),
                             hashlib.sha1(b"void f_80000000(void) {}\n", usedforsecurity=False).hexdigest())

    def test_a_changed_file_is_read_again(self):
        build.key_of(str(self.src), FLAGS, DEPS)
        self.next_build()
        for text, when in (("void f_80000000(void) { }\n", MINUTE_AGO),   # (another size)
                           ("void f_80000004(void) {}\n", MINUTE_AGO - 30)):   # (the same size, another time)
            with self.subTest(text=text):
                self.write(text, when)
                self.assertEqual(build.key_of(str(self.src), FLAGS, DEPS), build.object_key(str(self.src), FLAGS, DEPS))
                self.next_build()

    def test_other_flags_or_headers_are_another_key_and_both_are_kept(self):
        first = build.key_of(str(self.src), FLAGS, DEPS)
        hot = build.key_of(str(self.src), FLAGS + ["-O2"], DEPS)
        other = build.key_of(str(self.src), FLAGS, DEPS + b"/* a new header */")
        self.assertEqual(len({first, hot, other}), 3)
        self.next_build()
        with self.unread():
            self.assertEqual([build.key_of(str(self.src), FLAGS, DEPS), build.key_of(str(self.src), FLAGS + ["-O2"], DEPS)],
                             [first, hot])

    def test_a_file_written_near_the_record_is_not_trusted(self):
        self.write("void f_80000000(void) {}\n", when=time.time())   # (within RACY_NS of the record written next)
        build.key_of(str(self.src), FLAGS, DEPS)
        self.next_build()
        with self.unread(), self.assertRaises(AssertionError):
            build.key_of(str(self.src), FLAGS, DEPS)

    def test_an_untrusted_record_of_other_content_loses_its_keys(self):
        """(a file changed within RACY_NS of the record, its size and time the same: the record's keys are of the old content)"""
        st = build.file_stat(str(self.src))
        part = build.kept_part(FLAGS, DEPS)
        build.HASHES[str(self.src)] = {"stat": st, "sha1": "0" * 40, "keys": {part: "stale", "other": "stale too"}}
        build.HASHES_AT[0] = st[1]   # (the record written as the file was: not trusted)
        self.assertEqual(build.key_of(str(self.src), FLAGS, DEPS), build.object_key(str(self.src), FLAGS, DEPS))
        self.assertNotIn("stale", build.HASHES[str(self.src)]["keys"].values())
        self.assertNotIn("stale too", build.HASHES[str(self.src)]["keys"].values())

    def test_a_record_that_cannot_be_read_is_no_record(self):
        build.key_of(str(self.src), FLAGS, DEPS)
        self.next_build()
        record = self.dir / build.HASH_CACHE
        for text in ("{not json", json.dumps({"scheme": build.HASH_SCHEME + 1, "written_ns": 0, "files": {}}),
                     json.dumps({"scheme": build.HASH_SCHEME, "written_ns": time.time_ns(),
                                 "files": {str(self.src): {"stat": "no", "sha1": 1, "keys": []}}})):
            with self.subTest(text=text[:20]):
                record.write_text(text, encoding="utf-8")
                build.load_hashes(str(self.dir))
                self.assertEqual(build.HASHES, {})
                self.assertEqual(build.key_of(str(self.src), FLAGS, DEPS), build.object_key(str(self.src), FLAGS, DEPS))

    def test_a_file_gone_is_not_kept_in_the_record(self):
        build.key_of(str(self.src), FLAGS, DEPS)
        self.src.unlink()
        build.save_hashes(str(self.dir))
        data = json.loads((self.dir / build.HASH_CACHE).read_text(encoding="utf-8"))
        self.assertEqual(data["files"], {})

    def test_migrate_stamps_reads_nothing_stamped_with_todays_kind_of_key(self):
        gen, obj_dir, cache = self.dir / "gen", self.dir / "obj", self.dir / "objcache"
        for d in (gen, obj_dir, cache):
            d.mkdir()
        self.src = gen / "code_002.c"
        self.write("void f_80000008(void) {}\n")
        key = build.object_key(str(self.src), FLAGS, DEPS)
        (obj_dir / "code_002.o").write_bytes(b"object")
        (obj_dir / "code_002.o.sha1").write_text(key)
        (cache / (key + ".o")).write_bytes(b"object")
        with mock.patch.object(build, "OBJ_CACHE", str(cache)), self.unread():
            build.migrate_stamps(str(gen), str(obj_dir), FLAGS, DEPS)
        self.assertEqual((obj_dir / "code_002.o.sha1").read_text(), key)

    def test_the_build_uses_the_record(self):
        """main loads it before the stamps and saves it after the plan; the plan, the kept list and the fingerprint use it.
        compile_one keeps object_key: with make_game.py's HOOKSHOT_TIMES every key it needs is planned already"""
        src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        main = src[src.index("\ndef main("):]
        self.assertLess(main.index("load_hashes(out)"), main.index("migrate_stamps("))
        self.assertLess(main.index("plan_jobs(jobs)"), main.index("save_hashes(out)"))
        plan = src[src.index("\ndef plan_jobs("):src.index("\ndef migrate_stamps(")]
        self.assertIn("key_of(src, fl, deps)", plan)
        kept = src[src.index("\ndef write_kept("):src.index("\ndef times_from(")]
        self.assertIn("key_of(src, fl, deps)", kept)
        self.assertIn("content_sha1(code_src)", main)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
