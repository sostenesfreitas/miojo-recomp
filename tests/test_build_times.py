"""The build's timer and countdown: make_game.py's build_times.json, and the estimates build.py makes from it.

Nothing here runs a compiler or needs the game.

python -I -B tests/test_build_times.py
"""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party" / "a64-kit" / "tools")]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


make_game = load("make_game_times", ROOT / "nx2" / "make_game.py")
build = load("build_times", ROOT / "nx2" / "build.py")


class TimesRecordTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot-times-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)

    def record(self, files, compile_seconds=10.0, total_seconds=20.0, modules=3, kind="build"):
        return make_game.record_times(str(self.work), files, compile_seconds, total_seconds, modules=modules, kind=kind)

    def test_a_first_build_is_written_and_has_nothing_before_it(self):
        self.assertIsNone(self.record({"code_001.c": 120.4}))
        data = json.loads((self.work / "build_times.json").read_text(encoding="utf-8"))
        self.assertEqual(data["format"], 1)
        self.assertEqual(data["seconds"], {"code_001.c": 120.4})
        self.assertEqual(len(data["builds"]), 1)
        self.assertEqual(data["builds"][0]["compile_seconds"], 10.0)
        self.assertEqual(data["builds"][0]["total_seconds"], 20.0)
        self.assertEqual(data["builds"][0]["compiled"], 1)
        self.assertEqual(data["builds"][0]["modules"], 3)
        self.assertEqual(data["builds"][0]["kind"], "build")

    def test_the_next_build_sees_the_last_one_and_keeps_the_old_modules(self):
        self.record({"code_001.c": 120.4})
        self.assertEqual(self.record({"code_002.c": 6.0}, 30.0, 40.0), 20.0)
        data = make_game.load_times(str(self.work))
        self.assertEqual(data["seconds"], {"code_001.c": 120.4, "code_002.c": 6.0})
        self.assertEqual([b["total_seconds"] for b in data["builds"]], [20.0, 40.0])

    def test_a_module_a_cache_held_keeps_its_time(self):
        self.record({"code_001.c": 120.4})
        self.record({})   # (nothing compiled: an update that only had to link)
        self.assertEqual(make_game.load_times(str(self.work))["seconds"], {"code_001.c": 120.4})

    def test_a_zero_is_not_a_measurement(self):
        self.record({"code_001.c": 0.0, "code_002.c": 5.5})
        self.assertEqual(make_game.load_times(str(self.work))["seconds"], {"code_002.c": 5.5})

    def test_a_broken_or_foreign_file_is_started_again(self):
        (self.work / "build_times.json").write_text("{not json", encoding="utf-8")
        self.assertEqual(make_game.load_times(str(self.work))["builds"], [])
        (self.work / "build_times.json").write_text('{"format": 99, "seconds": {"a": 1}}', encoding="utf-8")
        self.assertEqual(make_game.load_times(str(self.work))["seconds"], {})
        self.assertIsNone(self.record({"code_001.c": 3.0}))

    def test_only_the_newest_builds_are_kept(self):
        for i in range(make_game.KEEP_BUILDS + 5):
            self.record({}, 1.0, float(i))
        builds = make_game.load_times(str(self.work))["builds"]
        self.assertEqual(len(builds), make_game.KEEP_BUILDS)
        self.assertEqual(builds[-1]["total_seconds"], float(make_game.KEEP_BUILDS + 4))


class EstimateTests(unittest.TestCase):
    def test_no_files_left_is_no_time(self):
        self.assertEqual(build.eta_left([], 8), 0.0)

    def test_the_work_spreads_over_the_compilers(self):
        self.assertEqual(build.eta_left([100.0, 100.0, 100.0, 100.0], 4), 100.0)
        self.assertEqual(build.eta_left([100.0, 100.0], 1), 200.0)

    def test_the_longest_file_sets_the_floor(self):
        self.assertEqual(build.eta_left([600.0, 60.0], 8), 600.0)

    def test_measured_seconds_are_used_as_they_are(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "code_003.c"
            src.write_bytes(b"x" * 1024)
            jobs = [("cc", str(src), str(Path(d) / "code_003.o"), [], b"")]
            self.assertEqual(build.per_file_seconds(jobs, {"code_003.c": 42.0}), [42.0])

    def test_an_unmeasured_module_follows_the_measured_rate(self):
        with tempfile.TemporaryDirectory() as d:
            known = Path(d) / "code_001.c"
            known.write_bytes(b"x" * 1024)
            new = Path(d) / "code_002.c"
            new.write_bytes(b"x" * 2048)
            jobs = [("cc", str(known), "", [], b""), ("cc", str(new), "", [], b"")]
            self.assertEqual(build.per_file_seconds(jobs, {"code_001.c": 10.0}), [10.0, 20.0])

    def test_a_first_build_still_has_an_estimate_for_every_module(self):
        with tempfile.TemporaryDirectory() as d:
            jobs = []
            for name in ("code_hot.c", "code_001.c", "host_main.c"):
                src = Path(d) / name
                src.write_bytes(b"x" * (1 << 20))
                jobs.append(("cc", str(src), "", [], b""))
            seconds = build.per_file_seconds(jobs, {})
            self.assertEqual(len(seconds), 3)
            self.assertTrue(all(seconds))
            self.assertGreater(seconds[0], seconds[1])   # (the hot file's rate is the highest)

    def test_a_broken_record_is_no_estimate(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "build_times.json"
            self.assertEqual(build.times_from(str(path)), {})
            path.write_text("{not json", encoding="utf-8")
            self.assertEqual(build.times_from(str(path)), {})
            path.write_text('{"seconds": {"code_001.c": 12.5, "odd": "x"}}', encoding="utf-8")
            self.assertEqual(build.times_from(str(path)), {"code_001.c": 12.5})

    def test_the_plan_leaves_out_what_a_cache_already_holds(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "code_001.c"
            src.write_text("int f(void) { return 0; }\n", encoding="utf-8")
            obj = Path(d) / "code_001.o"
            job = ("cc", str(src), str(obj), ["-O1"], b"headers")
            work, keys = build.plan_jobs([job])
            self.assertEqual(work, [job])
            key = keys[(str(src), str(obj))]
            obj.write_bytes(b"")
            Path(str(obj) + ".sha1").write_text(key, encoding="utf-8")
            self.assertEqual(build.plan_jobs([job])[0], [])


class PhaseTests(unittest.TestCase):
    """Where a build's time went (PhaseClock): from the stages and the lines the steps print, in build_times.json"""

    def clock(self):
        self.t = [100.0]
        return make_game.PhaseClock(100.0, now=lambda: self.t[0])

    def test_a_build_is_cut_into_its_stages_and_their_parts(self):
        c = self.clock()
        self.t[0] = 102.0
        c.enter("read")
        self.t[0] = 112.0
        c.enter("lift")
        # (lift.py's and build.py's own lines, as they print them)
        for line in ("image: rtld@0x80000000+0x20000, main@0x80020000+0x4ea0000; 295178 entries (7s)",
                     "discovered 294522 functions (70s)",
                     "lift: coverage in 39.5s (109s)",
                     "lift: emit and write (1 processes) in 170.5s (280s)",
                     "lift: patches (16 processes) in 7.9s (295s)"):
            c.line(line)
        self.t[0] = 443.0
        c.enter("compile")
        self.t[0] = 452.0
        c.line("491 files, 24 jobs, -D_CRT_SECURE_NO_WARNINGS -O1 -DA64_POLLING -DA64_WRITE_TRACK")
        c.line("compiled in 1282s, 0 failed")
        c.line(r"linked E:\work\sa\build\hookshot.exe: 440 MB (3s)")
        c.line("linked /home/me/work/sa/build/hookshot_vk.so: 11 MB (0s)")
        self.t[0] = 1743.0
        c.enter("finish")
        self.t[0] = 1800.0
        c.enter(None)
        s = c.seconds
        self.assertEqual((s["checks"], s["read"], s["lift"], s["compile"], s["finish"]), (2.0, 10.0, 331.0, 1300.0, 57.0))
        self.assertEqual((s["lift: image and discovery"], s["lift: emit and write"], s["lift: patches"]), (70.0, 170.5, 7.9))
        self.assertEqual((s["compile: planning"], s["compile: compiling"]), (9.0, 1282.0))
        self.assertEqual((s["compile: linking hookshot.exe"], s["compile: linking hookshot_vk.so"]), (3.0, 0.0))
        table = c.table()
        self.assertEqual(table[0], "where the time went:")
        self.assertTrue(any("emit and write" in row and "170 s" in row for row in table))
        self.assertLess(next(i for i, r in enumerate(table) if r.split()[0] == "lift"),
                        next(i for i, r in enumerate(table) if "emit and write" in r))

    def test_a_stage_twice_adds_up(self):
        c = self.clock()
        c.enter("lift")
        self.t[0] = 110.0
        c.enter("compile")
        self.t[0] = 115.0
        c.enter("lift")
        self.t[0] = 130.0
        c.enter(None)
        self.assertEqual((c.seconds["lift"], c.seconds["compile"]), (25.0, 5.0))

    def test_the_planning_is_the_compile_stage_only(self):
        c = self.clock()
        c.enter("finish")
        self.t[0] = 140.0
        c.line("12 files, 4 jobs, -O1")
        self.assertNotIn("compile: planning", c.seconds)

    def test_the_phases_are_kept_with_the_build(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-phases-") as d:
            make_game.record_times(d, {}, 10.0, 20.0, phases={"lift": 331.04, "compile: linking hookshot.exe": 3.0})
            make_game.record_times(d, {}, 10.0, 20.0)
            builds = json.loads((Path(d) / "build_times.json").read_text(encoding="utf-8"))["builds"]
        self.assertEqual(builds[0]["phases"], {"lift": 331.0, "compile: linking hookshot.exe": 3.0})
        self.assertNotIn("phases", builds[1])

    def test_the_build_feeds_the_clock(self):
        """run() gives it every line a step prints; say("stage") moves it on; the build stops it before recording"""
        src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
        run = src[src.index("\ndef run("):src.index("\ndef ", src.index("\ndef run(") + 1)]
        self.assertIn("PHASES.line(line)", run)
        say = src[src.index("\ndef say("):src.index("\ndef ", src.index("\ndef say(") + 1)]
        self.assertIn("PHASES.enter(str(words[1]))", say)
        self.assertLess(src.index("PHASES.enter(None)"), src.index("phases=PHASES.seconds"))
        self.assertNotIn("@@phase", src)   # (no new line for the builders to read: they parse @@stage, @@file, @@times ...)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
