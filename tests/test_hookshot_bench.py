"""scripts/hookshot_bench.py, the benchmark route, with no game: the log window's sums from lines in the game's own
format, and the route's steps against a stand-in bridge (the replies docs/control.md gives), on a fake clock.

python -I -B tests/test_hookshot_bench.py
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import hookshot_bench as hb  # noqa: E402
import hookshot_ctl as ctl  # noqa: E402

# (lines as a v2.1.0 log prints them, an RX 5600 XT's)
WINDOW = """\
[hos Presentation Thread] vk: 28560 presents, 35.4 fps; a frame: 1424 draws (0 skipped), 157 dispatches, 196 passes, 148 barriers, 6 submits, 0.1 uploads (0.1 MB), 80 ub updates (56 overlays), GPU-bound wait 0.0 ms; 11126 pipelines (252695 ms), 0 programs (11 failed), 1267 textures (2109 MB)
[hos Presentation Thread] vk: GPU time 21.8 ms a frame (the longest submission 8.1 ms); 2.9 textures made a frame
[hos Presentation Thread] vk: a frame's replay time (ms): submit 15.61, present 0.13, draw 5.14, descriptors 1.52, texture lookups 0.15, uploads 0.00, pipelines 0.28, programs 0.98, commands 2.02, dispatch 0.71, memory sync 7.74, queue submit 0.43, slot wait 0.05
[hos sead::MainThread] vk: import 2.2 ms: commit+properties 0.1, buffer 0.0, allocate 2.1, bind+address 0.0
[hos Presentation Thread] vk: 28680 presents, 36.7 fps; a frame: 1004 draws (0 skipped), 114 dispatches, 164 passes, 145 barriers, 6 submits, 0.0 uploads (0.0 MB), 80 ub updates (56 overlays), GPU-bound wait 0.0 ms; 11126 pipelines (252695 ms), 0 programs (11 failed), 1203 textures (2093 MB)
[hos Presentation Thread] vk: GPU time 21.3 ms a frame (the longest submission 9.9 ms); 2.2 textures made a frame
[hos Presentation Thread] vk: VRAM: 4259 MB used of 4259 MB allowed (1389 MB of it free in the blocks); textures 2056 MB (budget 2400 MB), 1187 textures (at the driver's limit, room in the blocks: not squeezed)
[hos Presentation Thread] vk: a frame's replay time (ms): submit 14.37, present 0.12, draw 5.00, descriptors 1.54, texture lookups 0.31, uploads 0.18, pipelines 0.27, programs 0.91, commands 1.95, dispatch 0.70, memory sync 6.81, queue submit 0.41, slot wait 0.05
[hos Presentation Thread] vk: 28800 presents, 37.3 fps; a frame: 956 draws (0 skipped), 106 dispatches, 163 passes, 145 barriers, 6 submits, 0.4 uploads (0.0 MB), 82 ub updates (57 overlays), GPU-bound wait 0.0 ms; 11126 pipelines (252695 ms), 0 programs (11 failed), 1185 textures (2050 MB)
"""


class Clock:
    """A fake clock: sleep moves it, and so does each request (a reply takes a moment)"""

    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class FakeBridge:
    """Replies scripted per command: a list is consumed in order (its last entry repeats), a callable gets the request"""

    def __init__(self, clock, replies):
        self.clock, self.replies, self.asked = clock, replies, []

    def request(self, cmd, **args):
        self.asked.append((cmd, args))
        self.clock.t += 0.5
        r = self.replies.get(cmd, {"ok": True})
        if callable(r):
            return r(args)
        if isinstance(r, list):
            return r.pop(0) if len(r) > 1 else r[0]
        return r


def play(map_name="KokiriForest_Child", event=False, loading=False):
    return {"ok": True, "state": "play", "map": map_name, "fps": 40.0,
            "game": {"event": event, "loading": loading, "age": "child"}}


class ParseWindow(unittest.TestCase):
    def test_medians_over_the_window(self):
        r = hb.parse_window(WINDOW)
        self.assertEqual(r["reports"], 3)
        self.assertEqual(r["fps"], 36.7)
        self.assertEqual(r["fps_min"], 35.4)
        self.assertEqual(r["draws"], 1004)
        self.assertEqual(r["gpu_ms"], 21.55)
        self.assertEqual(r["replay_ms"]["submit"], 14.99)
        self.assertEqual(r["replay_ms"]["memory_sync"], 7.28)
        self.assertEqual(r["replay_ms"]["texture_lookups"], 0.23)
        self.assertEqual((r["vram_used_mb"], r["vram_allowed_mb"]), (4259, 4259))

    def test_an_empty_window(self):
        r = hb.parse_window("[hos host] nothing measured here\n")
        self.assertEqual(r["reports"], 0)
        self.assertIsNone(r["fps"])
        self.assertIsNone(r["vram_used_mb"])
        self.assertEqual(r["replay_ms"], {})


class RouteSteps(unittest.TestCase):
    def test_first_frame_waits_again_after_a_timeout(self):
        clock = Clock()
        timeout = {"ok": False, "error": "timeout", "message": "not met in time"}
        b = FakeBridge(clock, {"wait": [timeout, timeout, {"ok": True, "state": "intro", "waited": 1}]})
        hb.Route(b, clock, clock.sleep).first_frame(8 * 3600)
        self.assertEqual([a["timeout"] for c, a in b.asked], [3000, 3000, 3000])

    def test_first_frame_gives_up(self):
        clock = Clock()

        def never(args):
            clock.t += args["timeout"]
            return {"ok": False, "error": "timeout"}
        b = FakeBridge(clock, {"wait": never})
        with self.assertRaises(ctl.Failed):
            hb.Route(b, clock, clock.sleep).first_frame(7200)

    def test_to_play_presses_a_until_a_save_is_loaded(self):
        clock = Clock()
        menu = {"ok": True, "state": "menu", "map": None}
        b = FakeBridge(clock, {"status": [menu, menu, play()]})
        s = hb.Route(b, clock, clock.sleep).to_play()
        self.assertEqual(s["map"], "KokiriForest_Child")
        self.assertEqual([a.get("buttons") for c, a in b.asked if c == "press"], ["A", "A"])

    def test_go_waits_out_a_refused_teleport_and_the_map(self):
        clock = Clock()
        refused = {"ok": False, "error": "refused", "message": "another teleport in the last 8 s"}
        sent = {"ok": True, "sent": True, "map": "HyruleField_Child"}
        b = FakeBridge(clock, {"teleport": [refused, sent],
                               "status": [play(event=True), play(), play(), play("HyruleField_Child", loading=True),
                                          play("HyruleField_Child")]})
        s = hb.Route(b, clock, clock.sleep).go("HyruleField_Child", "KokiriForest_Child")
        self.assertEqual(s["map"], "HyruleField_Child")
        cmds = [c for c, a in b.asked]
        self.assertEqual(cmds.count("teleport"), 2)
        self.assertIn(("wait", {"event": "map", "map": "HyruleField_Child", "timeout": 60}), b.asked)

    def test_go_to_the_map_it_is_on_does_not_wait_for_a_map_change(self):
        clock = Clock()
        b = FakeBridge(clock, {"teleport": {"ok": True, "sent": True, "map": "KokiriForest_Child"}, "status": play()})
        hb.Route(b, clock, clock.sleep).go("KokiriForest_Child", "KokiriForest_Child")
        self.assertNotIn("map", [a.get("event") for c, a in b.asked if c == "wait"])

    def test_a_teleport_that_fails_stops_the_run(self):
        clock = Clock()
        b = FakeBridge(clock, {"teleport": {"ok": False, "error": "timeout"}, "status": play()})
        with self.assertRaises(ctl.Failed):
            hb.Route(b, clock, clock.sleep).go("HyruleField_Child", "KokiriForest_Child")

    def test_window_samples_the_bridge_fps_and_events(self):
        clock = Clock()
        samples = [dict(play(event=e), fps=f) for f, e in ((41.0, False), (39.5, True), (40.2, True), (38.0, False),
                                                            (40.0, False))]
        b = FakeBridge(clock, {"status": samples})
        route = hb.Route(b, clock, clock.sleep)
        fps, events = route.window(10, every=2.0)
        self.assertEqual(fps, [41.0, 39.5, 40.2, 38.0])   # (2.5 s a sample on this clock: 0, 2.5, 5, 7.5)
        self.assertEqual(events, 2)
        self.assertEqual(route.presses, 0)   # (5 s of an event: a cutscene's, left alone)

    def test_a_message_left_open_is_closed_with_a(self):
        clock = Clock()
        b = FakeBridge(clock, {"status": [play(event=True)] * 12 + [play()]})   # (~1 s a round on this clock)
        route = hb.Route(b, clock, clock.sleep)
        route.settled()
        self.assertGreaterEqual(route.presses, 1)
        self.assertIn(("press", {"buttons": "A", "frames": 8, "timeout": 30}), b.asked)

    def test_a_message_in_the_window_is_closed_too(self):
        clock = Clock()
        b = FakeBridge(clock, {"status": [play(event=True)] * 20})
        route = hb.Route(b, clock, clock.sleep)
        fps, events = route.window(30, every=2.0)
        self.assertEqual(events, len(fps))
        self.assertGreaterEqual(route.presses, 2)


class Helpers(unittest.TestCase):
    def test_log_since_and_grep(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "log.txt")
            Path(p).write_text("old line\n", encoding="utf-8")
            start = os.path.getsize(p)
            with open(p, "a", encoding="utf-8") as f:
                f.write(WINDOW)
            self.assertEqual(hb.parse_window(hb.log_since(p, start))["reports"], 3)
            g = hb.grep_log(p, ["VRAM:", "not in the log"], keep=1)
            self.assertEqual(len(g["VRAM:"]), 1)
            self.assertEqual(g["not in the log"], [])

    def test_saves_in_and_back(self):
        with tempfile.TemporaryDirectory() as d:
            game, src = Path(d, "game"), Path(d, "mine")
            own = game / "user" / "save" / ctl.SAVE_USER
            (own / "slot_00").mkdir(parents=True)
            (own / "slot_00" / "progress.sav").write_bytes(b"theirs")
            (src / "slot_03").mkdir(parents=True)
            (src / "slot_03" / "progress.sav").write_bytes(b"bench")
            (src / "slot_04").mkdir()
            (src / "slot_04" / "progress.sav").write_bytes(b"other")
            (src / "account.sav").write_bytes(b"a")
            put_back = hb.saves_in(str(game), str(src), "3")
            self.assertEqual((own / "slot_03" / "progress.sav").read_bytes(), b"bench")
            self.assertFalse((own / "slot_04").exists())   # (the slot asked for alone: Continue loads it)
            self.assertFalse((own / "slot_00").exists())
            self.assertEqual((own / "account.sav").read_bytes(), b"a")
            with self.assertRaises(ctl.Failed):   # (a run that did not end left the folder's saves aside)
                hb.saves_in(str(game), str(src))
            put_back()
            self.assertEqual((own / "slot_00" / "progress.sav").read_bytes(), b"theirs")
            self.assertFalse((own / "slot_03").exists())
            self.assertFalse(Path(str(game / "user" / "save") + ".hookshot_bench").exists())

    def test_saves_in_refuses_a_folder_with_no_slots(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "mine").mkdir()
            Path(d, "mine", "account.sav").write_bytes(b"a")
            with self.assertRaises(ctl.Failed):
                hb.saves_in(os.path.join(d, "game"), os.path.join(d, "mine"))
            self.assertFalse(Path(d, "game", "user", "save").exists())

    def test_process_alive(self):
        self.assertTrue(hb.process_alive(os.getpid()))

    def test_process_memory_of_this_process(self):
        mb = hb.process_memory_mb(os.getpid())
        if sys.platform.startswith(("win", "linux")):
            self.assertIsInstance(mb, int)
            self.assertGreater(mb, 0)


class RunFolder(unittest.TestCase):
    def test_the_default_run_folder_is_ignored_by_git(self):
        # It holds game screenshots and the full log: CONTRIBUTING.md keeps both out of the repository.
        src = (ROOT / "scripts" / "hookshot_bench.py").read_text(encoding="utf-8")
        self.assertIn('os.path.join("bench-runs", ', src)
        ignored = (ROOT / ".gitignore").read_text(encoding="utf-8").splitlines()
        self.assertIn("bench-runs/", [line.strip() for line in ignored])


if __name__ == "__main__":
    unittest.main()
