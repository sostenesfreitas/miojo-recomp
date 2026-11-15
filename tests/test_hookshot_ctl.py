"""scripts/hookshot_ctl.py, the control bridge's client, against a tiny fake game: a Python stand-in for the executable
that serves the bridge's protocol on $HOS_CONTROL (docs/control.md) and writes down each request. The commands' requests
and plain output, --json, exit codes (an error reply, a timeout, no game), watch, launch (which starts the stand-in as a
game folder's hookshot and waits for its socket), the mem commands, and complete OoT status/watch snapshots.
Linux uses a Unix socket; Windows uses the named pipe.

python -I -B tests/test_hookshot_ctl.py [--source-root OTHER_CHECKOUT]
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CTL = Path(os.environ.get("HOOKSHOT_CTL_SOURCE", ROOT / "scripts" / "hookshot_ctl.py"))


def client_module():
    spec = importlib.util.spec_from_file_location("hookshot_ctl", CTL)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

FAKE = r'''
import json, os, socket, struct, sys, threading, time, zlib
path = os.environ["HOS_CONTROL"]
log = open(os.environ["FAKE_LOG"], "a", buffering=1)
log.write(json.dumps({"cmd": "_start", "control": path, "xdg": os.environ.get("XDG_DATA_HOME"),
                      "argv": sys.argv[1:], "cwd": os.getcwd(), "headless": os.environ.get("HOS_HEADLESS"),
                      "audio": os.environ.get("HOS_AUDIO"), "env_file": os.environ.get("HOS_ENV_FILE")}) + "\n")
print("fake: started", flush=True)
started = time.monotonic()
options = {"text_speed": 3, "fps_counter": 0}


def png(path):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 1, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\0\xff\0")) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as out:
        out.write(data)

if os.environ.get("FAKE_EXIT"):
    sys.exit(int(os.environ["FAKE_EXIT"]))
if os.environ.get("FAKE_NO_BRIDGE"):
    time.sleep(120)
    sys.exit(0)
frame = [100]
game_state = [{"t": 6.567, "frame": 94, "map": "KokiriForest_Child", "pos": [-20.1, 0.0, 6.8],
               "facing": 108.6, "camera": 45.0, "event": False, "loading": False, "age": "child",
               "life": 12, "max_life": 16, "rupees": 7, "prompt": "Open", "music": "Test_\u001b[2J\"\\\u0080"}]
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
                     map="KokiriForest_Child", game=game_state[0])
            if os.environ.get("FAKE_SESSION"):
                age = time.monotonic() - started
                present = age >= float(os.environ.get("FAKE_PRESENT_AFTER", "0"))
                quiet = age >= float(os.environ.get("FAKE_PLAY_AFTER", "0"))
                r.update(frame=frame[0] if present else 0, stalled=False, state="play" if present else "boot",
                         game=dict(game_state[0], event=not quiet, loading=not quiet) if game_state[0] is not None else None)
        elif cmd == "fixture_game":   # test-only: change what the fake bridge reports, without restarting launch
            game_state[0] = req["game"]
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
            steps = req.get("steps") or [{"at": req.get("at", 0), "frames": req.get("frames", 8)}]
            r.update(start=frame[0] + min(s.get("at", 0) for s in steps),
                     end=frame[0] + max(s.get("at", 0) + s.get("frames", 8) for s in steps))
            print("fake: input %s completed" % cmd, flush=True)
        elif cmd == "shot":
            dest = req.get("path") or os.path.join(os.path.dirname(os.environ["FAKE_LOG"]), "shots", "shot.png")
            png(dest)
            r.update(present=frame[0] + 1, path=dest, width=2, height=1)
            # (gpu/vk/vk_replay.cpp, shot publication: exact success-line format)
            print("vk: present %d written to %s (the control bridge)" % (r["present"], dest), flush=True)
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
                before = options.get(req["name"], 0)
                options[req["name"]] = req["value"]
                r.update(was=before, option={"key": req["name"], "value": req["value"], "text": "On"})
        elif cmd == "option_get":
            rows = [{"key": k, "value": v, "text": "Instant" if k == "text_speed" else "On" if v else "Off"}
                    for k, v in options.items()]
            r.update(option=next(o for o in rows if o["key"] == req["name"])) if req.get("name") else r.update(options=rows)
        elif cmd == "mem_read":
            data = bytes(range(req["size"]))
            r.update(format=req.get("format", "hex"), addr=req["addr"], size=req["size"], data=data.hex())
        elif cmd == "mem_write":
            r.update(wrote=len(bytes.fromhex(req["hex"])), addr=req["addr"])
        elif cmd == "subscribe":
            if ("flags" in req or "changes" in req) and "oot" not in req.get("events", []):
                f.write((json.dumps(dict(r, ok=False, error="bad_request", message="flags and changes need events:[oot]")) + "\n").encode())
                continue
            f.write((json.dumps({"id": rid, "ok": True, "frame": frame[0], "events": req.get("events")}) + "\n").encode())
            for i in range(5):
                if "oot" in req.get("events", []):
                    g = dict(game_state[0])
                    g["frame"] = 94 + i * req.get("every", 60)
                    if "flags" in req:
                        g["flags"] = {name: {"Test.Flag": 1, "0x00000110": 1}.get(name) for name in req["flags"]}
                    if req.get("changes"):
                        g["changed"] = {} if not i else {"0x12345678": i}
                    event = {"event": "oot", "sub": rid, "frame": frame[0] + i * req.get("every", 60), "game": g}
                else:
                    event = {"event": "present", "sub": rid, "frame": frame[0] + i, "fps": 60.0, "state": "play"}
                f.write((json.dumps(event) + "\n").encode())
            continue
        elif cmd == "quit":
            print("control: quit asked by a client", flush=True)   # (host/control.c, cmd_quit)
            f.write((json.dumps(dict(r, quitting=True)) + "\n").encode())
            if not os.environ.get("FAKE_REFUSE_QUIT"):
                f.flush_and_exit()
            continue
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


class GamePaths(unittest.TestCase):
    def test_builder_and_development_layouts(self):
        m = client_module()
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            name = "hookshot.exe" if WINDOWS else "hookshot"
            (game / name).touch()
            self.assertEqual(m.game_exe(tmp), str(game / name))
            (game / "bin").mkdir()
            (game / "bin" / name).touch()
            self.assertEqual(m.game_exe(tmp), str(game / "bin" / name))
            (game / "bin" / name).unlink()
            self.assertEqual(m.game_exe(tmp), str(game / "bin" / name))
            self.assertEqual(m.game_exe(tmp, str(game / "custom")), str(game / "custom"))


class SourceSelection(unittest.TestCase):
    def test_environment_source_and_explicit_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            selected = root / "selected.py"
            selected.write_text("raise RuntimeError('selected environment client')\n", encoding="utf-8")
            (root / "scripts").mkdir()
            (root / "scripts" / "hookshot_ctl.py").write_text(
                "raise RuntimeError('selected checkout client')\n", encoding="utf-8")
            for args, marker in (([], "selected environment client"),
                                 (["--source-root", str(root)], "selected checkout client")):
                with self.subTest(args=args):
                    result = subprocess.run([sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                                             *args, "GamePaths"], capture_output=True, text=True, timeout=20,
                                            env=dict(os.environ, HOOKSHOT_CTL_SOURCE=str(selected)))
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("RuntimeError: " + marker, result.stderr)


class Sessions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ctl-session-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.fake = self.root / "fake.py"
        self.fake.write_text(FAKE, encoding="utf-8")
        self.m = client_module()
        self.sessions = []
        self.addCleanup(self.stop_sessions)

    def stop_sessions(self):
        for session in self.sessions:
            if session.info()["state"] not in ("finished", "failed"):
                session.stop(timeout=0.2, terminate=True)
            deadline = time.monotonic() + 5
            while self.m.same_process(**session.info()["owner"]) and time.monotonic() < deadline:
                time.sleep(0.02)

    def start(self, **kwargs):
        env = dict(FAKE_LOG=str(self.root / "requests.jsonl"), FAKE_SESSION="1")
        env.update(kwargs.pop("env", {}))
        session = self.m.start_session(str(self.root), root=str(self.root / "runs"), exe=sys.executable,
                                       image=str(self.fake), timeout=5, env=env, **kwargs)
        self.sessions.append(session)
        return session

    def requests(self, command):
        return [r for r in map(json.loads, (self.root / "requests.jsonl").read_text().splitlines()) if r["cmd"] == command]

    def test_profile_manifest_and_second_client(self):
        os.environ["CTL_INHERITED_SECRET"] = "not recorded"
        self.addCleanup(os.environ.pop, "CTL_INHERITED_SECRET", None)
        session = self.start(profile="test", ready="play",
                             env={"HOS_HEADLESS": "0", "HOS_AUDIO": "1", "HOS_ENV_FILE": "ambient.txt"})
        info = session.info()
        self.assertEqual((info["schema_version"], info["client_version"], info["state"]), (1, "1", "running"))
        self.assertEqual((info["game"], info["executable"]), (str(self.root), sys.executable))
        self.assertTrue(self.m.same_process(info["pid"], info["process_start"]))
        self.assertEqual((info["environment"]["HOS_HEADLESS"], info["environment"]["HOS_AUDIO"]), ("1", "0"))
        self.assertNotIn("CTL_INHERITED_SECRET", info["environment"])
        self.assertEqual(Path(info["environment"]["HOS_ENV_FILE"]).read_bytes(), b"")
        self.assertEqual(info["user"], str(session.folder / "user"))
        self.assertEqual(self.requests("_start")[-1]["argv"], ["--user", info["user"]])
        attached = self.m.Session.attach(session.folder)
        self.assertEqual(attached.request("status")["state"], "play")
        attached.request("press", buttons="A", frames=3)
        self.assertIs(self.requests("press")[-1]["exclusive"], True)
        ctl("--run", str(session.folder), "stick", "L", "0", "1", "--frames", "60", "--no-exclusive", check=0)
        self.assertIs(self.requests("stick")[-1]["exclusive"], False)
        listed = json.loads(ctl("--json", "runs", str(self.root / "runs"), check=0).stdout)
        self.assertEqual([r["run"] for r in listed], [str(session.folder)])
        shown = json.loads(ctl("--json", "--run", str(session.folder), "show", check=0).stdout)
        self.assertEqual(shown["pid"], info["pid"])
        with self.assertRaisesRegex(self.m.Failed, "still has a live"):
            attached.finish()
        stopped = attached.stop()
        self.assertEqual((stopped["state"], stopped["exit_code"], stopped["stop_method"]), ("finished", 0, "quit"))
        self.assertEqual(stopped["save_drain"], "unsupported")
        self.assertTrue(stopped["process_exited"])
        self.assertFalse(self.m.same_process(info["pid"], info["process_start"]))

    def test_sampled_state_after_reattach(self):
        session = self.start(profile="test", ready="play")
        attached = self.m.Session.attach(session.folder)
        status = attached.request("status")
        game = status["game"]
        self.assertEqual((game["frame"], game["pos"], game["life"], game["rupees"]),
                         (94, [-20.1, 0.0, 6.8], 12, 7))
        self.assertFalse(game["event"])
        self.assertFalse(game["loading"])
        self.assertGreater(status["frame"], game["frame"])
        args = ("--run", str(session.folder))
        self.assertEqual(json.loads(ctl("--json", *args, "status", check=0).stdout)["game"], game)
        plain = ctl(*args, "status", check=0).stdout
        self.assertEqual(json.loads(plain.splitlines()[1][7:]), game)
        self.assertNotIn("\x1b", plain)
        watched = ctl("--json", *args, "watch", "--events", "oot", "--every", "6",
                      "--flags", "Test.Flag,0x00000110", "--changes", "--count", "3", check=0)
        events = [json.loads(line) for line in watched.stdout.splitlines()]
        self.assertEqual([e["game"]["frame"] for e in events], [94, 100, 106])
        self.assertEqual([e["game"]["changed"] for e in events], [{}, {"0x12345678": 1}, {"0x12345678": 2}])
        for event in events:
            self.assertEqual(event["game"]["flags"], {"Test.Flag": 1, "0x00000110": 1})
            self.assertEqual(event["game"]["pos"], game["pos"])
        ctl(*args, "raw", '{"cmd":"fixture_game","game":null}', check=0)
        self.assertIsNone(attached.request("status")["game"])
        self.assertEqual(len(ctl(*args, "status", check=0).stdout.splitlines()), 1)

    def test_default_profile_and_unique_folders(self):
        first, second = self.start(), self.start()
        self.assertNotEqual(first.folder, second.folder)
        info = first.info()
        self.assertEqual(info["profile"], "default")
        for name in ("HOS_HEADLESS", "HOS_AUDIO", "HOS_ENV_FILE"):
            self.assertNotIn(name, info["environment"])
        first.request("press", buttons="B", frames=8, at=12)
        req = self.requests("press")[-1]
        self.assertEqual((req["frames"], req["at"]), (8, 12))
        self.assertNotIn("exclusive", req)

    def test_copied_save_and_explicit_paths(self):
        source = self.root / "source"
        (source / "slot_03").mkdir(parents=True)
        (source / "account.sav").write_bytes(b"authored account")
        (source / "slot_03" / "progress.sav").write_bytes(b"authored progress")
        before = [(p.relative_to(source), p.read_bytes(), p.stat().st_mtime_ns) for p in source.rglob("*") if p.is_file()]
        romfs = self.root / "romfs"
        romfs.mkdir()
        session = self.start(profile="test", save=str(source), slot="03", romfs=str(romfs), cwd=str(self.root))
        info = session.info()
        copied = Path(info["user"]) / "save" / self.m.SAVE_USER
        self.assertEqual((copied / "slot_03" / "progress.sav").read_bytes(), b"authored progress")
        self.assertEqual(info["save_source"], str(source))
        self.assertEqual(info["command"], [sys.executable, str(self.fake), "--romfs", str(romfs), "--user", info["user"]])
        after = [(p.relative_to(source), p.read_bytes(), p.stat().st_mtime_ns) for p in source.rglob("*") if p.is_file()]
        self.assertEqual(before, after)
        if not WINDOWS:
            (source / "bad").symlink_to(source / "account.sav")
            with self.assertRaisesRegex(self.m.Failed, "link or not a plain file"):
                self.start(profile="test", save=str(source))
        with self.assertRaisesRegex(self.m.Failed, "owns its user folder"):
            self.start(profile="test", user=str(source))
        explicit = self.start(user=str(self.root / "explicit-user"))
        self.assertEqual(explicit.info()["user"], str(self.root / "explicit-user"))

    def test_ready_levels_and_cleanup(self):
        for level, values in (("bridge", {"FAKE_PRESENT_AFTER": "99", "FAKE_PLAY_AFTER": "99"}),
                              ("present", {"FAKE_PRESENT_AFTER": "0.15", "FAKE_PLAY_AFTER": "99"}),
                              ("play", {"FAKE_PRESENT_AFTER": "0.1", "FAKE_PLAY_AFTER": "0.3"})):
            with self.subTest(level=level):
                session = self.start(ready=level, env=values)
                reply = session.info()["ready_reply"]
                self.assertTrue(self.m.ready_status(reply, level))
                if level != "play":
                    self.assertFalse(self.m.ready_status(reply, "play"))
                session.stop()
        for name, values, expected in (("timeout", {"FAKE_NO_BRIDGE": "1"}, "timed out"),
                                       ("play-timeout", {"FAKE_PLAY_AFTER": "99"}, "timed out")):
            with self.subTest(name=name):
                run = self.root / name
                env = dict(FAKE_LOG=str(self.root / "requests.jsonl"), FAKE_SESSION="1", **values)
                with self.assertRaisesRegex(self.m.Failed, expected):
                    self.m.start_session(str(self.root), run=str(run), exe=sys.executable, image=str(self.fake),
                                         ready="play", timeout=0.3, env=env)
                info = self.m.Session(run).info()
                self.assertEqual(info["state"], "failed")
                self.assertIsNotNone(info["exit_code"])
                self.assertFalse(self.m.same_process(info["pid"], info["process_start"]))

    def test_exit_before_first_ready_poll_is_recorded(self):
        from unittest import mock
        run = self.root / "exit"
        run.mkdir()
        (run / "manifest.lock").write_bytes(b"\0")
        env = {"FAKE_LOG": str(self.root / "exit-requests.jsonl"), "FAKE_EXIT": "7", "HOS_CONTROL": "never-listens"}
        session = self.m.Session(run)
        self.m.write_json(session.manifest, {"schema_version": 1, "command": [sys.executable, "-I", str(self.fake)],
                                           "cwd": str(self.root), "environment": env, "log": str(run / "log.txt"),
                                           "socket": "never-listens", "ready": "play", "ready_timeout": 0.3})
        popen, wait_ready = subprocess.Popen, self.m.wait_ready

        def exited_child(*args, **kwargs):
            child = popen(*args, **kwargs)
            self.assertEqual(child.wait(timeout=10), 7)   # (exit is established before the first readiness poll)
            return child

        def late_first_poll(*args):
            with mock.patch.object(self.m.time, "monotonic", side_effect=[0, 1]), \
                    mock.patch.object(self.m, "Bridge") as bridge:
                try:
                    return wait_ready(*args)
                finally:
                    bridge.assert_not_called()

        with mock.patch.object(self.m.subprocess, "Popen", side_effect=exited_child), \
                mock.patch.object(self.m, "wait_ready", side_effect=late_first_poll):
            self.assertEqual(self.m.supervise(run), 2)
        info = session.info()
        self.assertEqual((info["state"], info["exit_code"], info["error_code"]), ("failed", 7, 2))
        self.assertIn("game exited 7", info["error"])
        self.assertFalse(self.m.same_process(info["pid"], info["process_start"]))

    def test_ready_exit_at_deadline_wins_over_timeout(self):
        from unittest import mock
        session = mock.Mock(folder=self.root)
        session.info.return_value = {"socket": "unused", "log": "unused.log"}
        process = mock.Mock()
        process.poll.side_effect = [None, 7]   # (alive before checking time, exited at the deadline)
        with mock.patch.object(self.m.time, "monotonic", side_effect=[0, 1]), \
                mock.patch.object(self.m, "Bridge") as bridge:
            with self.assertRaisesRegex(self.m.Failed, "game exited 7") as caught:
                self.m.wait_ready(session, "play", 1, process)
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(process.poll.call_count, 2)
        bridge.assert_not_called()

    def test_ready_checks_exit_after_transport_poll(self):
        from unittest import mock
        session = mock.Mock(folder=self.root)
        session.info.return_value = {"socket": "unused", "log": "unused.log"}
        process = mock.Mock(returncode=7)
        process.poll.side_effect = [None, 7]
        with mock.patch.object(self.m.time, "monotonic", return_value=0), \
                mock.patch.object(self.m.time, "sleep") as sleep, \
                mock.patch.object(self.m, "Bridge", side_effect=self.m.Failed("not listening", 2)):
            with self.assertRaisesRegex(self.m.Failed, "game exited 7") as caught:
                self.m.wait_ready(session, "play", 1, process)
        self.assertEqual(caught.exception.code, 2)
        sleep.assert_not_called()

    def test_ready_live_child_at_deadline_still_times_out(self):
        from unittest import mock
        session = mock.Mock(folder=self.root)
        session.info.return_value = {"socket": "unused", "log": "unused.log"}
        process = mock.Mock()
        process.poll.return_value = None
        with mock.patch.object(self.m.time, "monotonic", side_effect=[0, 1]), \
                mock.patch.object(self.m, "Bridge") as bridge:
            with self.assertRaisesRegex(self.m.Failed, "ready=play timed out") as caught:
                self.m.wait_ready(session, "play", 1, process)
        self.assertEqual(caught.exception.code, 3)
        self.assertEqual(process.poll.call_count, 2)
        bridge.assert_not_called()

    def test_stop_rechecks_exit_at_wait_deadline(self):
        from unittest import mock
        for force in (False, True):
            with self.subTest(terminate=force):
                process = mock.Mock(returncode=7)
                process.wait.side_effect = subprocess.TimeoutExpired("fake", 1)
                process.poll.return_value = 7
                with mock.patch.object(self.m, "Bridge") as bridge:
                    bridge.return_value.request.return_value = {"ok": True}
                    result = self.m.stop_child(process, {"socket": "unused"}, 1, force)
                self.assertEqual((result["method"], result["process_exited"], result["exit_code"]), ("quit", True, 7))
                self.assertFalse(result["forced"])
                process.terminate.assert_not_called()
                process.kill.assert_not_called()
        # (an exit between that final poll and a requested fallback is not a forced stop either)
        process = mock.Mock(returncode=7)
        process.wait.side_effect = subprocess.TimeoutExpired("fake", 1)
        process.poll.side_effect = [None, 7]
        with mock.patch.object(self.m, "Bridge") as bridge:
            bridge.return_value.request.return_value = {"ok": True}
            result = self.m.stop_child(process, {"socket": "unused"}, 1, True)
        self.assertEqual(result["method"], "already-exited")
        self.assertFalse(result["forced"])
        process.terminate.assert_not_called()
        process.kill.assert_not_called()

    def test_terminate_rechecks_exit_before_escalating(self):
        from unittest import mock
        process = mock.Mock(returncode=-15)
        process.poll.side_effect = [None, -15]
        process.wait.side_effect = subprocess.TimeoutExpired("fake", 5)
        self.assertEqual(self.m.terminate_child(process), "terminate")
        process.terminate.assert_called_once()
        process.kill.assert_not_called()
        # (a genuinely live child still needs the explicit fallback)
        process = mock.Mock(returncode=-9)
        process.poll.side_effect = [None, None]
        process.wait.side_effect = [subprocess.TimeoutExpired("fake", 5), -9]
        self.assertEqual(self.m.terminate_child(process), "kill")
        process.terminate.assert_called_once()
        process.kill.assert_called_once()

    def test_stop_timeout_force_and_external_owner(self):
        session = self.start(env={"FAKE_REFUSE_QUIT": "1"})
        info = session.info()
        timed = session.stop(timeout=0.1)
        self.assertEqual(timed["state"], "running")
        self.assertEqual(timed["stop_result"]["method"], "quit-timeout")
        self.assertFalse(timed["stop_result"]["process_exited"])
        self.assertTrue(self.m.same_process(info["pid"], info["process_start"]))
        owner = timed["owner"]
        session.update(owner={"pid": os.getpid(), "birth": {"wrong": "identity"}})
        with self.assertRaisesRegex(self.m.Failed, "attached client will not kill"):
            self.m.Session.attach(session.folder).stop(timeout=0.1, terminate=True)
        self.assertTrue(self.m.same_process(info["pid"], info["process_start"]))
        session.update(owner=owner, process_start={"wrong": "identity"})
        with self.assertRaisesRegex(self.m.Failed, "pid was reused"):
            self.m.Session.attach(session.folder)
        session.update(process_start=info["process_start"])
        result = json.loads(ctl("--json", "--run", str(session.folder), "stop", "--timeout", "0.1", "--terminate", check=0).stdout)
        self.assertEqual(result["state"], "finished")
        self.assertIn(result["stop_method"], ("terminate", "kill"))
        self.assertNotEqual(result["exit_code"], 0)
        self.assertEqual(result["save_drain"], "unsupported")

    def test_log_cursor_expect_and_bounds(self):
        import threading
        session = self.start()
        cursor = session.log(lines=0)["cursor"]
        self.assertGreater(cursor, 0)
        session.request("press", buttons="A", frames=2)
        self.assertIn("input press", session.expect_log("input press", cursor, timeout=1)["text"])
        cursor = session.log(lines=0)["cursor"]
        with self.assertRaisesRegex(self.m.Failed, "timed out"):
            session.expect_log("input press", cursor, timeout=0.05)
        timer = threading.Timer(0.05, lambda: session.request("stick", stick="R", x=1, y=0, frames=4))
        timer.start()
        try:
            got = session.expect_log(r"input st.ck completed", cursor, timeout=2, regex=True)
            self.assertTrue(got["matched"])
        finally:
            timer.join()
        path = Path(session.info()["log"])
        with path.open("ab") as f:
            f.write(b"line\n" * 100)
        part = session.log(lines=2, limit=100)
        self.assertEqual(part["text"], "line\nline\n")
        self.assertTrue(part["truncated"])
        part = session.log(since=0, limit=20)
        self.assertEqual(part["cursor"], 20)
        self.assertTrue(part["more"])
        for kwargs in ({"limit": 0}, {"lines": -1}, {"since": -1}, {"since": path.stat().st_size + 1}):
            with self.assertRaises(self.m.Failed):
                session.log(**kwargs)
        text = ctl("--run", str(session.folder), "--json", "log", "--lines", "2", check=0)
        self.assertEqual(json.loads(text.stdout)["text"], "line\nline\n")
        ctl("--run", str(session.folder), "log", "--expect", "line", check=2)
        ctl("--run", str(session.folder), "log", "--since", str(cursor), "--expect", "line", check=0)

    def test_shot_is_decodable_and_recorded(self):
        import struct
        import zlib
        session = self.start()
        cursor = session.log(lines=0)["cursor"]
        reply = session.request("shot")
        line = "vk: present %d written to %s (the control bridge)" % (reply["present"], reply["path"])
        self.assertTrue(session.expect_log(line, cursor, timeout=1)["matched"])
        data = Path(reply["path"]).read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        offset, image = 8, b""
        while offset < len(data):
            size = struct.unpack_from(">I", data, offset)[0]
            kind, body = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + size]
            crc = struct.unpack_from(">I", data, offset + 8 + size)[0]
            self.assertEqual(crc, zlib.crc32(kind + body) & 0xffffffff)
            if kind == b"IHDR":
                self.assertEqual(struct.unpack(">IIBBBBB", body), (2, 1, 8, 2, 0, 0, 0))
            if kind == b"IDAT":
                image += body
            offset += size + 12
        self.assertEqual(zlib.decompress(image), b"\0\xff\0\0\0\xff\0")
        artifact = session.info()["artifacts"][0]
        self.assertEqual((artifact["path"], artifact["frame"], artifact["width"], artifact["height"]),
                         (reply["path"], reply["frame"], 2, 1))
        session.request("option_set", name="text_speed", value=1)
        self.assertEqual(session.request("option_get", name="text_speed")["option"]["value"], 1)
        self.assertEqual(session.stop()["artifacts"], [artifact])

    def test_frame_units_received_by_fake(self):
        session = self.start()
        session.request("press", buttons="A", at=7, frames=3)
        session.request("stick", stick="L", x=0, y=1, at=9, frames=45)
        ctl("--run", str(session.folder), "sequence", "0.025:A/25,1:B/1,2:LUP/2000", "--hos", check=0)
        self.assertEqual((self.requests("press")[-1]["at"], self.requests("press")[-1]["frames"]), (7, 3))
        self.assertEqual((self.requests("stick")[-1]["at"], self.requests("stick")[-1]["frames"]), (9, 45))
        self.assertEqual(self.requests("sequence")[-1]["steps"],
                         [{"at": 2, "frames": 2, "buttons": "A"}, {"at": 60, "frames": 1, "buttons": "B"},
                          {"at": 120, "frames": 120, "buttons": "LUP"}])

    def test_run_collision_finish_and_wrapper(self):
        import threading
        from unittest import mock
        wrapper = self.root / "wrapper.py"
        receipt, release = self.root / "wrapper-receipt", self.root / "wrapper-release"
        self.addCleanup(release.touch)
        wrapper.write_text("import pathlib, subprocess, sys, time\n"
                           "code = subprocess.call(sys.argv[3:])\n"
                           "pathlib.Path(sys.argv[1]).write_text(str(code))\n"
                           "while not pathlib.Path(sys.argv[2]).exists(): time.sleep(0.02)\n", encoding="utf-8")
        with mock.patch.object(self.m.subprocess, "Popen", wraps=subprocess.Popen) as spawn:
            session = self.start(wrapper=[sys.executable, "-I", str(wrapper), str(receipt), str(release)])
        self.assertTrue(spawn.call_args.kwargs["close_fds"])
        self.assertTrue(spawn.call_args.kwargs["stdout"].closed)
        self.assertFalse(receipt.exists())
        with self.assertRaisesRegex(self.m.Failed, "new or empty"):
            self.start(run=str(session.folder))
        results, errors = [], []

        def stop():
            try:
                results.append(session.stop())
            except BaseException as e:
                errors.append(e)

        thread = threading.Thread(target=stop)
        thread.start()
        try:
            deadline = time.monotonic() + 5
            while not receipt.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertEqual(receipt.read_text(), "0")
            self.assertTrue(thread.is_alive(), "stop returned before the admission wrapper closed its log handle")
            with self.assertRaisesRegex(self.m.Failed, "still has a live"):
                session.finish()
        finally:
            release.touch()
            thread.join(timeout=10)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(results[0]["exit_code"], 0)
        self.assertFalse(self.m.owners_alive(session.info()))
        # (Windows refuses either rename while a process still holds the file)
        for name in ("log.txt", "owner.log"):
            path = session.folder / name
            path.rename(path.with_suffix(".closed"))
        session.update(state="running", exit_code=None, stop_method=None)
        info = session.finish()
        self.assertEqual((info["state"], info["exit_code"], info["stop_method"]), ("finished", None, "observed-exit"))


    def test_api_errors_have_one_exception_type(self):
        def failed(call):
            with self.assertRaises(self.m.Failed) as caught:
                call()
            self.assertEqual(caught.exception.code, 2)

        failed(lambda: self.m.Session(self.root / "missing").info())
        folder = self.root / "bad-run"
        folder.mkdir()
        (folder / "manifest.lock").write_bytes(b"\0")
        manifest = folder / "manifest.json"
        manifest.write_text("{not JSON", encoding="utf-8")
        failed(lambda: self.m.Session(folder).info())
        failed(lambda: self.m.list_runs(self.root))
        manifest.write_text(json.dumps({"schema_version": 1, "log": str(folder / "missing.log")}), encoding="utf-8")
        failed(lambda: self.m.Session(folder).log())
        failed(lambda: self.m.start_session(str(self.root), exe=sys.executable, env={"BAD_VALUE": None}))


    def test_save_alias_is_not_a_link(self):
        from unittest import mock
        source = self.root / "source-alias"
        (source / "slot_00").mkdir(parents=True)
        (source / "account.sav").write_bytes(b"authored account")
        realpath = self.m.os.path.realpath
        # (model short-to-long Windows spelling on either OS: these /./ paths reach the same folders)
        with mock.patch.object(self.m.os.path, "realpath", side_effect=lambda p: realpath(p) + os.sep + "."):
            dest = self.m.copy_save(str(source), str(self.root / "alias-data"))
        self.assertEqual((Path(dest) / "account.sav").read_bytes(), b"authored account")

    def test_manifest_handles_and_binary_log_cursors(self):
        from unittest import mock
        session = self.start()
        session.stop()
        opened, original_open, original_replace = [], Path.open, os.replace

        def opening(path, *args, **kwargs):
            f = original_open(path, *args, **kwargs)
            if path == session.manifest:
                opened.append(f)
            return f

        def replacing(src, dest):
            self.assertTrue(all(f.closed for f in opened), "a manifest reader prevents Windows atomic replacement")
            return original_replace(src, dest)

        with mock.patch.object(Path, "open", opening), mock.patch.object(self.m.os, "replace", replacing):
            for i in range(10):
                session.info()
                session.update(test_counter=i)
            self.assertEqual(session.info()["test_counter"], 9)
        log = Path(session.info()["log"])
        payload = "first\r\ncaf\u00e9\r\nthird\n".encode("utf-8")
        log.write_bytes(payload)
        first = session.log(since=0, limit=7)
        self.assertEqual((first["text"], first["cursor"]), ("first\r\n", 7))
        second = session.log(since=first["cursor"], limit=7)
        self.assertEqual((second["text"], second["cursor"]), ("caf\u00e9\r\n", 14))
        self.assertEqual(session.log(since=14)["text"], "third\n")
        self.assertEqual(session.info()["log"], str(log))   # (native absolute path separators, not JSON-escaped text)


    @unittest.skipIf(WINDOWS, "symlinked system parents are a Linux path policy; Windows aliases are tested above")
    def test_save_destination_symlink_boundaries(self):
        source = self.root / "source-links"
        (source / "slot_00").mkdir(parents=True)
        (source / "account.sav").write_bytes(b"authored account")
        real = self.root / "real-parent"
        real.mkdir()
        parent = self.root / "linked-parent"
        parent.symlink_to(real, target_is_directory=True)
        dest = self.m.copy_save(str(source), str(parent / "data"))
        self.assertEqual((Path(dest) / "account.sav").read_bytes(), b"authored account")
        into_source = self.root / "into-source"
        into_source.symlink_to(source, target_is_directory=True)
        with self.assertRaisesRegex(self.m.Failed, "source and destination"):
            self.m.copy_save(str(source), user=str(into_source))
        root = self.root / "owned-root"
        root.mkdir()
        (root / "save").symlink_to(real, target_is_directory=True)
        with self.assertRaisesRegex(self.m.Failed, "link or not a plain folder"):
            self.m.copy_save(str(source), user=str(root))

    def test_utf8_cursor_keeps_incomplete_characters(self):
        session = self.start()
        log = Path(session.info()["log"])
        payload = "abcde\u20actail".encode("utf-8")
        log.write_bytes(payload)
        first = session.log(since=0, limit=7)
        self.assertEqual((first["text"], first["cursor"]), ("abcde", 5))
        second = session.log(since=first["cursor"], limit=7)
        self.assertEqual(first["text"] + second["text"], payload.decode("utf-8"))
        self.assertEqual(second["cursor"], len(payload))
        self.assertTrue(session.expect_log("\u20ac", 0, timeout=1, limit=7)["matched"])
        # (the writer has flushed only the first two bytes of the next character)
        log.write_bytes(b"abcde\xe2\x82")
        self.assertEqual(session.log(since=0)["cursor"], 5)
        self.assertEqual(session.log(lines=0)["cursor"], 5)
        with log.open("ab") as f:
            f.write(b"\xac")
        self.assertEqual(session.log(since=5)["text"], "\u20ac")
        log.write_bytes("\u20ac\nnext\n".encode("utf-8"))
        self.assertEqual(session.log(limit=8)["text"], "next\n")   # (the tail begins inside the three-byte character)
        session.stop()
        log.write_bytes(b"x\xe2")
        final = session.log(since=0)
        self.assertEqual((final["text"], final["cursor"]), ("x\ufffd", 2))   # (only a finished run's true EOF)

    def test_stop_accepts_older_result_fields(self):
        from unittest import mock
        session = self.start(env={"FAKE_REFUSE_QUIT": "1"})
        original = session.info

        def older():
            info = original()
            if info.get("stop_result"):
                info["stop_result"].pop("process_exited", None)
            return info

        with mock.patch.object(session, "info", side_effect=older):
            result = session.stop(timeout=0.1)
        self.assertEqual(result["state"], "running")
        self.assertEqual(result["stop_result"]["method"], "quit-timeout")


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
        self.assertLess(len(os.fsencode(self.sock)), 108)
        self.assertEqual(os.stat(os.path.dirname(self.sock)).st_mode & 0o777, 0o700)
        self.assertEqual(os.stat(self.run_dir).st_mode & 0o777, 0o700)
        self.assertTrue(os.path.isfile(self.info["log"]))

    def test_status(self):
        r = ctl("status", env=self.env, check=0)
        lines = r.stdout.splitlines()
        self.assertRegex(lines[0], r"^frame \d+  fps 59\.9  state play  draws 2400  window 1280x720  map KokiriForest_Child \(child\)  fake$")
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[1].startswith("game   "))
        game = json.loads(lines[1][7:])
        self.assertEqual(game, {"t": 6.567, "frame": 94, "map": "KokiriForest_Child", "pos": [-20.1, 0.0, 6.8],
                                "facing": 108.6, "camera": 45.0, "event": False, "loading": False, "age": "child",
                                "life": 12, "max_life": 16, "rupees": 7, "prompt": "Open", "music": "Test_\u001b[2J\"\\\u0080"})
        self.assertNotIn("\x1b", r.stdout)   # control text is escaped, never terminal input
        r = ctl("--sock", self.sock, "--json", "status", check=0)
        result = json.loads(r.stdout)
        self.assertEqual(result["state"], "play")
        self.assertEqual(result["protocol"], 1)
        self.assertEqual(result["game"], game)
        self.assertGreater(result["frame"], game["frame"])   # reply and sample presents are distinct

    def test_status_null_and_partial_game(self):
        original = json.loads(ctl("--json", "status", env=self.env, check=0).stdout)["game"]
        try:
            for game in (None, dict.fromkeys(original, None)):
                ctl("raw", json.dumps({"cmd": "fixture_game", "game": game}), env=self.env, check=0)
                text = ctl("status", env=self.env, check=0).stdout
                self.assertIn("map KokiriForest_Child", text)
                result = json.loads(ctl("--json", "status", env=self.env, check=0).stdout)
                self.assertEqual(result["game"], game)
                if game is None:
                    self.assertEqual(len(text.splitlines()), 1)
                else:
                    self.assertEqual(json.loads(text.splitlines()[1][7:]), game)
        finally:
            ctl("raw", json.dumps({"cmd": "fixture_game", "game": original}), env=self.env, check=0)

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
        self.assertEqual(self.last("sequence")["steps"], [{"at": 0, "frames": 120, "buttons": "LUP"},
                                                        {"at": 120, "frames": 12, "buttons": "A"}])
        self.assertEqual(ctl("release", env=self.env, check=0).stdout.strip(), "released 2")
        self.assertNotIn("all", self.last("release"))
        self.assertEqual(ctl("release", "--all", env=self.env, check=0).stdout.strip(), "released 2 (every client's)")
        self.assertIs(self.last("release")["all"], True)

    def test_input_waits_as_long_as_the_bridge(self):
        # (without --timeout the bridge's default holds: 30 s plus the input's frames at 10 fps. The client waited 70 s
        # for any reply and gave up on long inputs the game was still pressing)
        m = client_module()
        waits = []

        class Fake(m.Bridge):
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
        path = os.path.join(self.dir, "out.png")
        r = ctl("shot", path, env=self.env, check=0)
        self.assertEqual(r.stdout.strip(), os.path.abspath(path))   # (the client's folder, made absolute)
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

    def test_watch_oot(self):
        r = ctl("--json", "watch", "--events", "oot", "--every", "6", "--flags", "Test.Flag,0x00000110",
                "--changes", "--count", "3", env=self.env, check=0)
        request = self.last("subscribe")
        self.assertEqual((request["events"], request["every"], request["flags"], request["changes"]),
                         (["oot"], 6, ["Test.Flag", "0x00000110"], True))
        events = [json.loads(line) for line in r.stdout.splitlines()]
        self.assertEqual(len(events), 3)
        self.assertEqual([e["game"]["frame"] for e in events], [94, 100, 106])
        for event in events:
            self.assertEqual((event["event"], event["sub"]), ("oot", request["id"]))
            self.assertGreaterEqual(event["frame"], event["game"]["frame"])
            self.assertEqual(event["game"]["flags"], {"Test.Flag": 1, "0x00000110": 1})
        self.assertEqual([e["game"]["changed"] for e in events], [{}, {"0x12345678": 1}, {"0x12345678": 2}])
        r = ctl("watch", "--events", "oot", "--count", "2", env=self.env, check=0)
        request = self.last("subscribe")
        self.assertEqual(request["every"], 60)
        self.assertNotIn("flags", request)
        self.assertNotIn("changes", request)
        self.assertEqual(len(r.stdout.splitlines()), 2)
        self.assertNotIn("\x1b", r.stdout)
        for line in r.stdout.splitlines():
            self.assertTrue(line.startswith("game   "))
            game = json.loads(line[7:])
            self.assertNotIn("flags", game)
            self.assertNotIn("changed", game)
            self.assertEqual(game["pos"], [-20.1, 0.0, 6.8])
        ctl("watch", "--events", "oot", "--log", "x", "--count", "1", env=self.env, check=0)
        self.assertEqual(self.last("subscribe")["events"], ["oot", "log"])
        r = ctl("watch", "--events", "present", "--flags", "Test.Flag", "--count", "1", env=self.env, check=1)
        self.assertIn("bad_request", r.stderr)
        ctl("watch", "--every", "six", env=self.env, check=2)

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
        m = client_module()
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
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--source-root", type=Path)
    options, remaining = parser.parse_known_args()
    if options.source_root is not None:
        CTL = options.source_root.resolve() / "scripts" / "hookshot_ctl.py"
    unittest.main(argv=[sys.argv[0]] + remaining)
