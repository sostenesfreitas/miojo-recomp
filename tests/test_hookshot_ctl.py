"""scripts/hookshot_ctl.py, the control bridge's client, against a tiny fake game: a Python stand-in for the executable
that serves the bridge's protocol on $HOS_CONTROL (docs/control.md) and writes down each request. The commands' requests
and plain output, --json, exit codes (an error reply, a timeout, no game), watch, launch (which starts the stand-in as a
game folder's hookshot and waits for its socket), and the mem commands. Linux only: a Unix socket.

python -I -B tests/test_hookshot_ctl.py
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CTL = ROOT / "scripts" / "hookshot_ctl.py"

FAKE = r'''
import json, os, socket, sys, threading, time
path = os.environ["HOS_CONTROL"]
log = open(os.environ["FAKE_LOG"], "a", buffering=1)
log.write(json.dumps({"cmd": "_start", "control": path, "xdg": os.environ.get("XDG_DATA_HOME")}) + "\n")
frame = [100]
if os.name == "nt":   # a named pipe, through ctypes: one synchronous handle a connection, read then write, never both at once
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateNamedPipeW.restype = wintypes.HANDLE
    k.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
    for f in (k.ConnectNamedPipe, k.ReadFile, k.WriteFile, k.FlushFileBuffers, k.CloseHandle):
        f.restype = wintypes.BOOL
    name = path if path.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + path
    class Conn:
        def __init__(self, h):
            self.h, self.buf = h, b""
        def __iter__(self):
            while True:
                while b"\n" not in self.buf:
                    chunk, n = ctypes.create_string_buffer(65536), wintypes.DWORD(0)
                    if not k.ReadFile(self.h, chunk, 65536, ctypes.byref(n), None) or not n.value:
                        return
                    self.buf += chunk.raw[:n.value]
                line, self.buf = self.buf.split(b"\n", 1)
                yield line
        def write(self, data):
            n = wintypes.DWORD(0)
            k.WriteFile(self.h, data, len(data), ctypes.byref(n), None)
        def flush_and_exit(self):
            k.FlushFileBuffers(self.h)   # (the reply read before the process and its pipe go)
            os._exit(0)
    def accept():
        h = k.CreateNamedPipeW(name, 3, 0, 255, 65536, 65536, 0, None)   # (duplex, byte mode, waiting)
        if not k.ConnectNamedPipe(h, None) and ctypes.get_last_error() != 535:   # ERROR_PIPE_CONNECTED
            k.CloseHandle(h)
            return None
        return Conn(h)
else:
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)
    class Conn:
        def __init__(self, c):
            self.f = c.makefile("rwb", buffering=0)
        def __iter__(self):
            return iter(self.f)
        def write(self, data):
            self.f.write(data)
        def flush_and_exit(self):
            os.unlink(path)
            os._exit(0)
    def accept():
        return Conn(srv.accept()[0])
def ticker():
    while True:
        time.sleep(0.01)
        frame[0] += 1
threading.Thread(target=ticker, daemon=True).start()
def serve(f):
    for raw in f:
        req = json.loads(raw)
        log.write(json.dumps(req) + "\n")
        rid, cmd = req.get("id"), req.get("cmd")
        r = {"id": rid, "ok": True, "frame": frame[0]}
        if cmd == "status":
            r.update(protocol=1, fps=59.9, state="play", draws=2400.0, window={"width": 1280, "height": 720}, version="fake",
                     map="KokiriForest_Child")
        elif cmd == "teleport":
            if req.get("to") is None:
                r.update(destinations=[{"index": 0, "child": "KokiriForest_Child", "adult": "KokiriForest_Adult"},
                                       {"index": 1, "child": None, "adult": "GerudoFortress_Adult"}])
            elif req.get("to") == "refuse":
                r = {"id": rid, "ok": False, "frame": frame[0], "error": "refused",
                     "message": "the game refused the teleport: a map is loading"}
            else:
                r.update(teleport=1, child=None, adult="GerudoFortress_Adult")
                r.update(sent=None if req.get("wait") is False else True)
                if r["sent"]:
                    r.update(map="GerudoFortress_Adult", accepted=req.get("to") != "gerudofortress_adult")
        elif cmd == "age":   # (Link is the child here: to the child is nothing to do)
            if req.get("to") == "child":
                r.update(age="child", sent=False)
            else:
                r.update(age="adult", sent=None if req.get("wait") is False else True)
                if r["sent"]:
                    r.update(map="KokiriForest_Adult", accepted=True)
        elif cmd in ("press", "stick", "sequence"):
            r.update(start=frame[0], end=frame[0] + req.get("frames", 8))
        elif cmd == "shot":
            r.update(present=frame[0] + 1, path=req.get("path") or "/run/shots/shot_0001.png")
        elif cmd == "wait":
            if req.get("event") == "loading_end" and req.get("timeout", 60) <= 1:
                r = {"id": rid, "ok": False, "frame": frame[0], "error": "timeout", "message": "not met in time"}
            else:
                r.update(state="play", waited=3)
                if req.get("event") == "map":
                    r.update(map=req.get("map") or "HyruleField_Child")
        elif cmd == "option_set":
            if req["name"] == "nope":
                r = {"id": rid, "ok": False, "frame": frame[0], "error": "not_found", "message": "no setting nope"}
            else:
                r.update(was=0, option={"key": req["name"], "value": req["value"], "text": "On"})
        elif cmd == "option_get":
            r.update(options=[{"key": "text_speed", "value": 3, "text": "Instant"}, {"key": "fps_counter", "value": 0, "text": "Off"}])
        elif cmd == "mem_read":
            data = bytes(range(req["size"]))
            r.update(format=req.get("format", "hex"), addr=req["addr"], size=req["size"], data=data.hex())
        elif cmd == "mem_write":
            r.update(wrote=len(bytes.fromhex(req["hex"])), addr=req["addr"])
        elif cmd == "subscribe":
            f.write((json.dumps({"id": rid, "ok": True, "frame": frame[0], "events": req.get("events")}) + "\n").encode())
            for i in range(5):
                f.write((json.dumps({"event": "present", "sub": rid, "frame": frame[0] + i, "fps": 60.0, "state": "play"}) + "\n").encode())
            continue
        elif cmd == "quit":
            f.write((json.dumps(dict(r, quitting=True)) + "\n").encode())
            f.flush_and_exit()
        elif cmd == "release":
            r.update(released=2)
        elif cmd == "saves":
            root = os.environ.get("FAKE_SAVES", "")
            r.update(path=root, account=True, option=True, slots=[
                {"slot": "03", "mtime": 1700000100, "newest": True,
                 "files": [{"name": "progress.sav", "size": 120724, "mtime": 1700000100},
                           {"name": "caption.sav", "size": 1500, "mtime": 1700000100}],
                 "caption": {"scene": "KokiriForest", "map": "KokiriForest/KokiriForest_Child",
                             "thumbnail": {"format": "jpeg", "offset": 1396, "size": 4}}},
                {"slot": "00", "mtime": 1700000000, "newest": False,
                 "files": [{"name": "progress.sav", "size": 120572, "mtime": 1700000000}]}])
        else:
            r = {"id": rid, "ok": False, "frame": frame[0], "error": "unknown_cmd", "message": "no command"}
        # (an event first: the client has to skip it)
        f.write(b'{"event":"present","sub":0,"frame":1,"fps":60,"state":"play"}\n')
        f.write((json.dumps(r) + "\n").encode())
while True:
    c = accept()
    if c:
        threading.Thread(target=serve, args=(c,), daemon=True).start()
'''


def ctl(*args, env=None, check=None):
    r = subprocess.run([sys.executable, "-I", str(CTL)] + list(args), capture_output=True, text=True, env=env, timeout=60)
    if check is not None and r.returncode != check:
        raise AssertionError("exit %d (wanted %d): %s %s" % (r.returncode, check, r.stdout, r.stderr))
    return r


WINDOWS = os.name == "nt"


class HookshotCtl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="ctl-", dir=None if WINDOWS or not os.path.isdir("/tmp") else "/tmp")
        cls.dir = cls.tmp.name
        cls.log = os.path.join(cls.dir, "requests.jsonl")
        os.environ["FAKE_LOG"] = cls.log   # (the stand-in's environment, through launch or directly)
        cls.saves = os.path.join(cls.dir, "fake-saves")   # (what the stand-in's saves reply names: a caption.sav in slot_03)
        os.makedirs(os.path.join(cls.saves, "slot_03"))
        with open(os.path.join(cls.saves, "slot_03", "caption.sav"), "wb") as f:
            f.write(b"\x04\x03\x02\x01" + bytes(1392) + b"\xff\xd8\xff\xd9")
        os.environ["FAKE_SAVES"] = cls.saves
        if WINDOWS:
            # (launch starts a game folder's hookshot.exe, which a script cannot stand in for: the stand-in is started
            # here, on a pipe of its own; launch is tested on Linux)
            fake = os.path.join(cls.dir, "fake.py")
            with open(fake, "w") as f:
                f.write(FAKE)
            cls.sock = "hookshot-control-test-%d" % os.getpid()
            cls.proc = subprocess.Popen([sys.executable, "-I", fake], env=dict(os.environ, HOS_CONTROL=cls.sock))
            cls.info = {"pid": cls.proc.pid}
            for _ in range(200):   # (until it answers)
                if ctl("--sock", cls.sock, "status").returncode == 0:
                    break
                time.sleep(0.05)
        else:
            cls.game = os.path.join(cls.dir, "game")
            os.mkdir(cls.game)
            exe = os.path.join(cls.game, "hookshot")
            with open(exe, "w") as f:
                f.write("#!%s -I\n%s" % (sys.executable, FAKE))
            os.chmod(exe, 0o755)
            # (launch: the stand-in started as the game folder's executable, its socket in the run folder)
            cls.run_dir = os.path.join(cls.dir, "run")
            r = ctl("--json", "launch", cls.game, "--run", cls.run_dir, "--headless", "--timeout", "20", check=0)
            cls.info = json.loads(r.stdout)
            cls.sock = cls.info["socket"]
        cls.env = dict(os.environ, HOS_CONTROL=cls.sock)

    @classmethod
    def tearDownClass(cls):
        try:
            if WINDOWS:
                cls.proc.kill()
                cls.proc.wait()
            else:
                os.kill(cls.info["pid"], 9)
        except OSError:
            pass
        cls.tmp.cleanup()

    def last(self, cmd):
        with open(self.log) as f:
            reqs = [json.loads(line) for line in f]
        return [r for r in reqs if r["cmd"] == cmd][-1]

    @unittest.skipIf(WINDOWS, "launch starts a game folder's hookshot.exe")
    def test_launch(self):
        self.assertEqual(self.sock, os.path.join(self.run_dir, "control.sock"))
        self.assertEqual(os.stat(self.run_dir).st_mode & 0o777, 0o700)
        self.assertTrue(os.path.isfile(self.info["log"]))

    def test_status(self):
        r = ctl("status", env=self.env, check=0)
        self.assertRegex(r.stdout, r"^frame \d+  fps 59\.9  state play  draws 2400  window 1280x720  map KokiriForest_Child  fake")
        r = ctl("--sock", self.sock, "--json", "status", check=0)
        self.assertEqual(json.loads(r.stdout)["state"], "play")

    def test_inputs(self):
        r = ctl("press", "A+B", "--frames", "8", "--exclusive", env=self.env, check=0)
        self.assertRegex(r.stdout, r"^frames \d+ to \d+")
        req = self.last("press")
        self.assertEqual((req["buttons"], req["frames"], req["exclusive"]), ("A+B", 8, True))
        ctl("stick", "l", "0", "1", "--frames", "60", env=self.env, check=0)
        req = self.last("stick")
        self.assertEqual((req["stick"], req["x"], req["y"], req["frames"]), ("L", 0.0, 1.0, 60))
        ctl("sequence", "0:A/8,60:A/8", env=self.env, check=0)
        self.assertEqual(self.last("sequence")["script"], "0:A/8,60:A/8")
        self.assertNotIn("units", self.last("sequence"))
        ctl("sequence", "0:LUP/2000,2:A", "--hos", env=self.env, check=0)
        self.assertEqual(self.last("sequence")["units"], "hos")
        self.assertEqual(ctl("release", env=self.env, check=0).stdout.strip(), "released 2")
        self.assertNotIn("all", self.last("release"))
        self.assertEqual(ctl("release", "--all", env=self.env, check=0).stdout.strip(), "released 2 (every client's)")
        self.assertIs(self.last("release")["all"], True)

    def test_input_waits_as_long_as_the_bridge(self):
        # (without --timeout the bridge's default holds: 30 s plus the input's frames at 10 fps. The client waited 70 s
        # for any reply and gave up on long inputs the game was still pressing)
        sys.path.insert(0, str(CTL.parent))
        try:
            import hookshot_ctl
        finally:
            sys.path.pop(0)
        waits = []

        class Fake(hookshot_ctl.Bridge):
            def __init__(self):
                self.next_id, self.pending, self.sock = 0, b"", None

            def send(self, obj):
                self.sent = obj

            def line(self, timeout):
                waits.append(timeout)
                return {"id": self.sent["id"], "ok": True}

        b = Fake()
        b.request("stick", stick="L", x=0, y=1, frames=36000)
        b.request("status")
        b.request("press", timeout=5, buttons="A")
        self.assertEqual(waits, [3610, 70, 15])

    def test_shot(self):
        r = ctl("shot", "out.png", env=self.env, check=0)
        self.assertEqual(r.stdout.strip(), os.path.abspath("out.png"))   # (the client's folder, made absolute)
        ctl("shot", "--after", "500", env=self.env, check=0)
        self.assertNotIn("path", self.last("shot"))
        self.assertEqual(self.last("shot")["after"], 500)

    def test_wait_and_errors(self):
        r = ctl("wait", "in_play", "--timeout", "120", env=self.env, check=0)
        self.assertIn("play after 3 frames", r.stdout)
        self.assertEqual(self.last("wait")["timeout"], 120)
        r = ctl("wait", "loading_end", "--timeout", "1", env=self.env, check=3)
        self.assertIn("timeout", r.stderr)
        r = ctl("wait", "map", "--map", "GerudoFortress_Adult", env=self.env, check=0)
        self.assertIn("map GerudoFortress_Adult", r.stdout)
        self.assertEqual(self.last("wait")["map"], "GerudoFortress_Adult")
        r = ctl("teleport", env=self.env, check=0)
        self.assertRegex(r.stdout, r"(?m)^  1  -\s+GerudoFortress_Adult$")
        self.assertNotIn("to", self.last("teleport"))
        r = ctl("teleport", "1", env=self.env, check=0)
        self.assertEqual(r.stdout.strip(), "teleport 1 sent: to GerudoFortress_Adult")
        self.assertEqual((self.last("teleport")["to"], self.last("teleport")["timeout"]), (1, 10))
        self.assertNotIn("wait", self.last("teleport"))
        r = ctl("teleport", "gerudofortress_adult", env=self.env, check=0)
        self.assertEqual(self.last("teleport")["to"], "gerudofortress_adult")
        self.assertIn("did not take it", r.stdout)   # (accepted false: said)
        r = ctl("teleport", "1", "--no-wait", env=self.env, check=0)
        self.assertIn("teleport 1 asked for", r.stdout)
        self.assertIs(self.last("teleport")["wait"], False)
        r = ctl("teleport", "1", "--arrive", "30", env=self.env, check=0)   # (sent, then wait map for its map)
        self.assertRegex(r.stdout, r"^teleport 1: in GerudoFortress_Adult after 3 frames")
        self.assertEqual((self.last("wait")["event"], self.last("wait")["map"], self.last("wait")["timeout"]),
                         ("map", "GerudoFortress_Adult", 30))
        r = ctl("teleport", "refuse", env=self.env, check=1)
        self.assertIn("refused: the game refused the teleport: a map is loading", r.stderr)
        ctl("teleport", "--arrive", "5", env=self.env, check=2)
        r = ctl("age", env=self.env, check=0)   # (none: the other age, waited for)
        self.assertEqual(r.stdout.strip(), "age switch sent: the adult, reloading KokiriForest_Adult")
        self.assertEqual(self.last("age")["timeout"], 10)
        self.assertNotIn("to", self.last("age"))
        self.assertNotIn("wait", self.last("age"))
        r = ctl("age", "child", env=self.env, check=0)
        self.assertEqual(r.stdout.strip(), "Link is the child already")
        self.assertEqual(self.last("age")["to"], "child")
        r = ctl("age", "adult", "--no-wait", env=self.env, check=0)
        self.assertIn("age switch to the adult asked for", r.stdout)
        self.assertIs(self.last("age")["wait"], False)
        # (sent, then the end of the reload's load: in a dungeon the map is the same name for both ages, so not wait map)
        r = ctl("age", "adult", "--arrive", "30", env=self.env, check=0)
        self.assertRegex(r.stdout, r"^age switch: the adult, in KokiriForest_Adult after 3 frames")
        self.assertEqual((self.last("wait")["event"], self.last("wait")["timeout"]), ("loading_end", 30))
        self.assertNotIn("map", self.last("wait"))
        ctl("age", "teen", env=self.env, check=2)   # (argparse: child or adult only)
        r = ctl("option", "set", "nope", "1", env=self.env, check=1)
        self.assertIn("not_found", r.stderr)
        r = ctl("raw", '{"cmd":"fly"}', env=self.env, check=1)
        self.assertEqual(json.loads(r.stdout)["error"], "unknown_cmd")
        r = ctl("--sock", os.path.join(self.dir, "none.sock"), "status", check=2)
        self.assertIn("no game listens", r.stderr)
        env = {k: v for k, v in os.environ.items() if k != "HOS_CONTROL"}
        self.assertIn("HOS_CONTROL", ctl("status", env=env, check=2).stderr)

    def test_options(self):
        r = ctl("option", "get", env=self.env, check=0)
        self.assertRegex(r.stdout, r"text_speed\s+3  Instant")
        r = ctl("option", "set", "fps_counter", "on", env=self.env, check=0)
        self.assertEqual(self.last("option_set")["value"], 1)
        self.assertIn("fps_counter 0 -> 1 (On)", r.stdout)

    def test_mem(self):
        r = ctl("mem", "read", "0x80000000", "4", env=self.env, check=0)
        self.assertEqual(r.stdout.strip(), "00010203")
        self.assertEqual(self.last("mem_read")["addr"], 0x80000000)
        r = ctl("mem", "read", "16", "4", "--format", "u32", env=self.env, check=0)
        self.assertEqual(r.stdout.strip(), "50462976")   # 0x03020100, little-endian
        r = ctl("mem", "write", "0x1000000000", "deadbeef", env=self.env, check=0)
        self.assertEqual(self.last("mem_write")["hex"], "deadbeef")
        self.assertIn("wrote 4 bytes", r.stdout)
        r = ctl("mem", "write", "0x10", "zz", env=self.env, check=2)   # bad hex, caught client-side
        self.assertIn("hex digits", r.stderr)

    def test_watch(self):
        r = ctl("watch", "--count", "3", "--every", "30", "--log", "vk:", env=self.env, check=0)
        self.assertEqual(len(r.stdout.splitlines()), 3)
        req = self.last("subscribe")
        self.assertEqual((req["events"], req["every"], req["log"]), (["log"], 30, "vk:"))
        ctl("watch", "--count", "1", "--events", "present", "--log", "x", env=self.env, check=0)
        self.assertEqual(self.last("subscribe")["events"], ["present", "log"])

    def test_saves_and_slots(self):
        r = ctl("saves", env=self.env, check=0)
        self.assertIn("(account.sav yes, option.sav yes)", r.stdout)
        self.assertRegex(r.stdout, r"slot_03  \S+ \S+  \(newest\)")
        self.assertRegex(r.stdout, r"progress.sav\s+120724")
        r = ctl("slots", env=self.env, check=0)
        lines = r.stdout.splitlines()
        self.assertRegex(lines[1], r"^\*  03 .* 120724\s+1500  KokiriForest$")
        self.assertRegex(lines[2], r"^   00 .* 120572\s+-  $")
        r = ctl("--json", "slots", env=self.env, check=0)
        self.assertEqual([s["slot"] for s in json.loads(r.stdout)], ["03", "00"])
        thumbs = os.path.join(self.dir, "thumbs")
        ctl("slots", "--thumbs", thumbs, env=self.env, check=0)
        with open(os.path.join(thumbs, "slot_03.jpg"), "rb") as f:
            self.assertEqual(f.read(), b"\xff\xd8\xff\xd9")
        self.assertFalse(os.path.exists(os.path.join(thumbs, "slot_00.jpg")))

    def test_copy_save(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("hookshot_ctl", CTL)
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        src = os.path.join(self.dir, "src-save")
        for d in ("slot_00", "slot_03"):
            os.makedirs(os.path.join(src, d))
            for n in ("progress.sav", "caption.sav"):
                with open(os.path.join(src, d, n), "w") as f:
                    f.write(d + n)
        for n in ("account.sav", "option.sav"):
            with open(os.path.join(src, n), "w") as f:
                f.write(n)
        before = sorted((dp, fn, os.stat(os.path.join(dp, fn)).st_mtime_ns) for dp, _, fns in os.walk(src) for fn in fns)
        dest = m.copy_save(src, os.path.join(self.dir, "d-all"))
        self.assertTrue(dest.endswith(os.path.join("hookshot", "save", m.SAVE_USER)))
        self.assertEqual(sorted(os.listdir(dest)), ["account.sav", "option.sav", "slot_00", "slot_03"])
        for slot in ("03", "slot_03", "3"):
            dest = m.copy_save(src, os.path.join(self.dir, "d-" + slot), slot)
            self.assertEqual(sorted(os.listdir(dest)), ["account.sav", "option.sav", "slot_03"])
            with open(os.path.join(dest, "slot_03", "progress.sav")) as f:
                self.assertEqual(f.read(), "slot_03progress.sav")
        with self.assertRaisesRegex(m.Failed, "has no slot_05"):
            m.copy_save(src, os.path.join(self.dir, "d-x"), "05")
        with self.assertRaisesRegex(m.Failed, "a slot number"):
            m.copy_save(src, os.path.join(self.dir, "d-y"), "slot_x")
        with self.assertRaisesRegex(m.Failed, "does not look like a save folder"):
            m.copy_save(os.path.join(src, "slot_00"), os.path.join(self.dir, "d-z"))
        with self.assertRaisesRegex(m.Failed, "not a folder"):
            m.copy_save(os.path.join(self.dir, "nothing-here"), os.path.join(self.dir, "d-w"))
        with self.assertRaisesRegex(m.Failed, "already holds saves"):
            m.copy_save(src, os.path.join(self.dir, "d-all"))
        after = sorted((dp, fn, os.stat(os.path.join(dp, fn)).st_mtime_ns) for dp, _, fns in os.walk(src) for fn in fns)
        self.assertEqual(before, after)   # (the source only read)
        if not WINDOWS:   # (a link in a save from elsewhere: refused, never followed into this user's files)
            linked = os.path.join(self.dir, "src-linked")
            import shutil
            shutil.copytree(src, linked)
            secret = os.path.join(self.dir, "secret.txt")
            with open(secret, "w") as f:
                f.write("not a save")
            os.remove(os.path.join(linked, "slot_03", "progress.sav"))
            os.symlink(secret, os.path.join(linked, "slot_03", "progress.sav"))
            with self.assertRaisesRegex(m.Failed, "is a link or not a plain file"):
                m.copy_save(linked, os.path.join(self.dir, "d-link"), "03")
            self.assertFalse(os.path.exists(os.path.join(self.dir, "d-link")))
        # (the thumbnails: what the game says is checked; a slot name that is a path is refused)
        bad = {"path": self.dir, "slots": [{"slot": "../../x", "caption": {"thumbnail": {"offset": 0, "size": 4}}}]}
        with self.assertRaisesRegex(m.Failed, "does not look like a save listing"):
            m.write_thumbs(bad, os.path.join(self.dir, "thumbs-bad"))
        self.assertEqual(m.plain_text("a\x1b[2Jb\tc"), "a?[2Jb?c")
        if not WINDOWS:   # (launch --save: the copy in the run folder's own data folder, which the game is given)
            run = os.path.join(self.dir, "run-save")
            r = ctl("--json", "launch", self.game, "--run", run, "--headless", "--save", src, "--slot", "03",
                    "--timeout", "20", check=0)
            info = json.loads(r.stdout)
            got = os.path.join(run, "data", "hookshot", "save", m.SAVE_USER)
            self.assertEqual(sorted(os.listdir(got)), ["account.sav", "option.sav", "slot_03"])
            with open(self.log) as f:
                starts = [json.loads(line) for line in f if '"_start"' in line]
            self.assertEqual([s["xdg"] for s in starts if s["control"] == info["socket"]], [os.path.join(run, "data")])
            ctl("--sock", info["socket"], "quit", check=0)
            r = ctl("launch", self.game, "--slot", "03", check=2)
            self.assertIn("--slot goes with --save", r.stderr)
        else:
            r = ctl("launch", self.dir, "--save", src, check=2)

    def test_zz_quit(self):   # (last: the stand-in ends)
        r = ctl("quit", env=self.env, check=0)
        self.assertEqual(r.stdout.strip(), "quitting")
        if WINDOWS:
            self.assertEqual(self.proc.wait(timeout=10), 0)
            self.assertIn("no game listens", ctl("status", env=self.env, check=2).stderr)   # (the pipe gone)
            return
        for _ in range(100):
            if not os.path.exists(self.sock):
                break
            time.sleep(0.05)
        self.assertFalse(os.path.exists(self.sock))


if __name__ == "__main__":
    unittest.main()
