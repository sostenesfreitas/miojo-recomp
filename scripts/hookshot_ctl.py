"""A client for the game's control bridge (host/control.c, docs/control.md): start the game with HOS_CONTROL=<socket>, then
steer it and read its state from a shell, a test or a coding agent. Standard library only.

    python -I scripts/hookshot_ctl.py [--sock S] [--json] COMMAND ...
    --run DIR COMMAND ...                       re-attach using the run's manifest instead of a socket
    runs [ROOT] / --run DIR show / finish       list runs, show one, mark an orphaned dead run finished
    --run DIR stop [--timeout 10] [--terminate] quit, wait for its owner to reap it; force only when requested
    --run DIR log [--lines 100|--since CURSOR] [--expect TEXT [--regex]]  bounded run log reads

    launch GAME_DIR [--data DIR] [--headless]   start the game with the bridge on; prints the socket's path
           [--save FOLDER [--slot NN]]          a copy of a save folder (one slot of it) as the run's saves
    status                                      frame, fps, draws, state, window, version, OoT snapshot
    press A+B [--frames 8] [--exclusive]        buttons held for some frames (the reply once they are let go)
    stick L 0 1 [--frames 60]                   a stick at x, y (-1 to 1)
    sequence '0:A/8,60:A/8' [--hos]             steps, <at>:<buttons>[/<frames held>] (--hos: HOS_INPUT_SCRIPT's s and ms)
    release [--all]                             this client's queued inputs let go (and those of clients gone)
    shot [out.png] [--after FRAME]              a PNG of the next present; prints its path
    wait in_play [--timeout 120]                also: present --frames N, frame --frame N, state --state S,
                                                loading_start, loading_end, change, map [--map NAME]
    teleport [INDEX|MAP] [--arrive SECS]        (OoT) the debug teleport: sent or refused; none: the destinations
    age [child|adult] [--arrive SECS]           (OoT) the Debug tab's Switch age (none: the other age), reloading the map
    option get [NAME]  /  option set NAME VALUE the Insert menu's settings
    saves  /  slots [--thumbs DIR]              the save folder the game uses; its slots, newest first
    watch [--events present,state,log,map,oot] [--every 60] [--log TEXT] [--count N]
          [--flags NAME,0xHASH,...] [--changes]    oot: watched values and changed flags
    mem read ADDR SIZE [--format hex|u8|u16|u32|u64|f32]   read guest memory (HOS_CONTROL_MEM=ro or rw)
    mem write ADDR HEX                          write guest memory (HOS_CONTROL_MEM=rw); aliases: ro->r/read, rw->readwrite
    quit                                        the game exits
    raw '{"cmd": "status"}'                     any request

The socket defaults to $HOS_CONTROL. Plain lines by default, --json for the replies as they come. Exit status: 0 done,
1 an error reply, 2 no game or bad usage, 3 a timeout.
"""
import argparse
import codecs
import contextlib
import functools
import math
import json
import os
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

WINDOWS = os.name == "nt"
CLIENT_VERSION = "1"
MANIFEST_VERSION = 1
LOG_LIMIT = 1024 * 1024


class Failed(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code


def session_call(function):
    """The callable session API has the same expected-error contract as the command line"""
    @functools.wraps(function)
    def call(*args, **kwargs):
        try:
            return function(*args, **kwargs)
        except (OSError, ValueError, TypeError, KeyError) as e:
            raise Failed(str(e), 2) from e
    return call


class Bridge:
    """One connection: requests with ids, replies matched to them, events kept for watch"""

    def __init__(self, path, timeout=10.0, session=None):
        self.path, self.next_id, self.pending = path, 0, b""
        self.session = session
        try:
            if WINDOWS:
                name = path if path.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + path
                self.sock = None
                deadline = time.monotonic() + timeout
                while True:   # (the bridge keeps one instance waiting: a busy pipe is waited for)
                    try:
                        self.pipe = open(name, "r+b", buffering=0)
                        break
                    except OSError as e:
                        # (busy: winerror 231, ERROR_PIPE_BUSY; missing pipes are not waited for)
                        if getattr(e, "winerror", 0) != 231 or time.monotonic() > deadline:
                            raise
                        time.sleep(0.02)
            else:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(timeout)
                self.sock.connect(path)
        except OSError as e:
            raise Failed("no game listens on %s (%s)" % (path, e.strerror or e), 2)

    def close(self):
        (self.sock or self.pipe).close()

    def send(self, obj):
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        if self.sock:
            self.sock.sendall(data)
        else:
            self.pipe.write(data)

    def line(self, timeout):
        """The next line as an object; None when the game went away"""
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Failed("no answer from the game in %.0f s" % timeout, 3)
            try:
                if self.sock:
                    self.sock.settimeout(left)
                    chunk = self.sock.recv(65536)
                else:
                    chunk = self._pipe_read(left)
                    if chunk is None:
                        continue
            except socket.timeout:
                raise Failed("no answer from the game in %.0f s" % timeout, 3)
            except OSError:
                chunk = b""
            if not chunk:
                return None
            self.pending += chunk
        text, self.pending = self.pending.split(b"\n", 1)
        return json.loads(text.decode("utf-8", "replace"))

    def _pipe_read(self, left):
        """(Windows) what the pipe holds, after waiting up to `left` seconds for some: None for nothing yet, b"" when the
        bridge's end closed. PeekNamedPipe and then a read of only what is there: a read that blocks would hold the
        handle's writes too (one synchronous handle)"""
        import ctypes
        import msvcrt
        handle = msvcrt.get_osfhandle(self.pipe.fileno())
        avail = ctypes.c_ulong(0)
        end = time.monotonic() + min(left, 0.05)
        while True:
            if not ctypes.windll.kernel32.PeekNamedPipe(ctypes.c_void_p(handle), None, 0, None, ctypes.byref(avail), None):
                return b""
            if avail.value:
                return self.pipe.read(avail.value)
            if time.monotonic() >= end:
                return None
            time.sleep(0.002)

    def request(self, cmd, timeout=None, reply_timeout=None, **args):
        """The reply to one request (events that come meanwhile are dropped). Without a timeout the bridge's own default
        holds (for an input: 30 s plus its frames at 10 fps, so a long one can take hours): the client then waits as
        long as the bridge can (an hour at most), unless the connection ends"""
        session = getattr(self, "session", None)
        if session:
            args = session.prepare(cmd, args)
        if cmd == "sequence" and args.get("units") == "hos" and "script" in args:
            args["steps"] = hos_steps(args.pop("script"))
            args.pop("units")
        self.next_id += 1
        rid = self.next_id
        req = {"id": rid, "cmd": cmd}
        req.update({k: v for k, v in args.items() if v is not None})
        if timeout is not None:
            req["timeout"] = timeout
        self.send(req)
        wait = (timeout if timeout is not None else 3600 if cmd in ("press", "stick", "sequence") else 60) + 10
        deadline = time.monotonic() + (wait if reply_timeout is None else reply_timeout)
        while True:
            msg = self.line(wait if reply_timeout is None else max(0, deadline - time.monotonic()))
            if msg is None:
                if cmd == "quit":
                    return {"id": rid, "ok": True, "quitting": True}
                raise Failed("the game closed the connection", 2)
            if msg.get("id") == rid and "ok" in msg:
                if session and msg.get("ok"):
                    session.record(cmd, msg)
                return msg


def plain_text(text):
    """Text from the game for a terminal: its control characters (an escape sequence in a log line) shown as '?'"""
    return "".join("?" if ord(ch) < 0x20 or 0x7f <= ord(ch) < 0xa0 else ch for ch in str(text))


def when(t):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) if t else "-"


def saves_text(r):
    lines = ["%s  (account.sav %s, option.sav %s)" % (plain_text(r["path"]), "yes" if r["account"] else "no",
                                                        "yes" if r["option"] else "no")]
    for sl in r["slots"]:
        lines.append("slot_%s  %s%s" % (plain_text(sl["slot"]), when(sl["mtime"]), "  (newest)" if sl["newest"] else ""))
        for f in sorted(sl["files"], key=lambda f: f["name"]):
            lines.append("    %-14s %9d  %s" % (plain_text(f["name"]), f["size"], when(f["mtime"])))
    return "\n".join(lines)


def slots_text(r):
    rows = ["   slot  last written          progress   caption  scene"]
    for sl in r["slots"]:
        size = {f["name"]: f["size"] for f in sl["files"]}
        rows.append("%s  %-4s  %-19s %9s %9s  %s" % ("*" if sl["newest"] else " ", plain_text(sl["slot"]), when(sl["mtime"]),
                                                   size.get("progress.sav", "-"), size.get("caption.sav", "-"),
                                                   plain_text(sl.get("caption", {}).get("scene", ""))))
    rows.append("(* the newest: the one Continue loads)")
    return "\n".join(rows)


def write_thumbs(r, folder):
    """Each slot's file-select picture, as the JPEG caption.sav holds, read from the save folder (this PC's). What the game
    says is checked first: this client may run outside a sandbox the game is in, so a slot's name is only digits, the
    picture a bounded slice of that slot's caption.sav, and nothing is written outside folder"""
    os.makedirs(folder, exist_ok=True)
    for sl in r["slots"]:
        th = sl.get("caption", {}).get("thumbnail")
        if not th:
            continue
        slot, off, size = sl.get("slot"), th.get("offset"), th.get("size")
        if not (isinstance(slot, str) and re.fullmatch(r"[0-9]{1,3}", slot) and isinstance(off, int) and
                isinstance(size, int) and 0 <= off < (1 << 24) and 0 < size <= (4 << 20)):
            raise Failed("slots --thumbs: the game's reply does not look like a save listing: %r" % (sl,), 1)
        with open(os.path.join(r["path"], "slot_" + slot, "caption.sav"), "rb") as f:
            f.seek(off)
            data = f.read(size)
        if data[:2] == b"\xff\xd8":
            with open(os.path.join(folder, "slot_%s.jpg" % sl["slot"]), "wb") as out:
                out.write(data)


def addr_of(text):
    try:
        return int(text, 0)
    except ValueError:
        raise Failed("ADDR: a number, or 0x...", 2)


def format_bytes(data, fmt):
    import struct
    if fmt in (None, "hex"):
        return data.hex()
    if fmt == "base64":
        import base64
        return base64.b64encode(data).decode()
    sizes = {"u8": ("B", 1), "u16": ("<H", 2), "u32": ("<I", 4), "u64": ("<Q", 8), "f32": ("<f", 4)}
    code, n = sizes[fmt]
    out = []
    for i in range(0, len(data) - n + 1, n):
        out.append(struct.unpack(code, data[i:i + n])[0])
    return " ".join(("%g" if fmt == "f32" else "%d") % v for v in out)


def connect(a, timeout=10.0):
    if getattr(a, "run", None):
        if a.sock:
            raise Failed("--run and --sock are alternatives", 2)
        return Session.attach(a.run).connect(timeout)
    path = a.sock or os.environ.get("HOS_CONTROL")
    if not path:
        raise Failed("no socket: give --sock PATH or set HOS_CONTROL (hookshot_ctl.py launch GAME_DIR starts a game "
                     "with the bridge on and prints its socket)", 2)
    return Bridge(path, timeout)


def check(reply):
    if not reply.get("ok"):
        code = 3 if reply.get("error") == "timeout" else 1
        raise Failed("%s: %s" % (reply.get("error", "error"), reply.get("message", "")), code)
    return reply


def show(a, reply, plain):
    if a.json:
        print(json.dumps(reply))
    else:
        print(plain(reply))


def run(a):
    if a.command == "launch":
        return launch(a)
    if a.command == "_supervise":
        return supervise(a.run)
    if a.command == "runs":
        show(a, list_runs(a.root), lambda rows: "\n".join("%s  %s" % (r["state"], r["run"]) for r in rows))
        return 0
    if a.command in ("show", "finish", "stop", "log"):
        if not a.run:
            raise Failed("%s needs --run DIR" % a.command, 2)
        session = Session(a.run)
        if a.command == "show":
            result = session.info()
        elif a.command == "finish":
            result = session.finish()
        elif a.command == "stop":
            result = session.stop(a.timeout, terminate=a.terminate)
        elif a.expect is not None:
            if a.since is None:
                raise Failed("log --expect needs --since CURSOR", 2)
            result = session.expect_log(a.expect, a.since, a.timeout, regex=a.regex, limit=a.bytes)
        else:
            result = session.log(lines=a.lines, since=a.since, limit=a.bytes)
        show(a, result, lambda r: "\n".join(plain_text(s) for s in r["text"].split("\n")) if "text" in r
             else json.dumps(r, indent=2))
        return 3 if a.command == "stop" and not result.get("process_exited") else 0
    if a.command == "mem":
        a.addr = addr_of(a.addr)
        if a.action == "read":
            if a.size is None:
                raise Failed("mem read ADDR SIZE", 2)
            a.size = addr_of(str(a.size))
        else:
            a.hex = a.hex if a.hex is not None else a.size   # write ADDR HEX
            if not a.hex:
                raise Failed("mem write ADDR HEX", 2)
    b = connect(a)
    try:
        if a.command == "status":
            r = check(b.request("status"))
            win = r.get("window") or {}
            g = r.get("game") or {}
            flags = " ".join(n for n, on in (("event", g.get("event")), ("loading", g.get("loading"))) if on)
            show(a, r, lambda r: "frame %d  fps %.1f  state %s  draws %s  window %s  map %s%s  %s" % (
                r["frame"], r["fps"], r["state"], "-" if r.get("draws") is None else "%.0f" % r["draws"],
                "%dx%d" % (win["width"], win["height"]) if win else "-", plain_text(r.get("map") or "-"),
                " (%s%s)" % (plain_text(g.get("age", "?")), ", " + flags if flags else "") if g else "",
                plain_text(r.get("version", ""))) + ("\ngame   " + json.dumps(g, ensure_ascii=True) if g else ""))
        elif a.command in ("press", "stick", "sequence"):
            args = {"exclusive": a.exclusive, "wait": False if a.no_wait else None}
            if a.command == "press":
                args.update(buttons=a.buttons, frames=a.frames, at=a.at)
            elif a.command == "stick":
                args.update(stick=a.which.upper(), x=a.x, y=a.y, frames=a.frames, at=a.at)
            else:
                args.update(script=a.script.strip(), units="hos" if a.hos else None)
            r = check(b.request(a.command, timeout=a.timeout, **args))
            show(a, r, lambda r: "queued %d input(s) at frame %d" % (r["queued"], r["frame"]) if "queued" in r
                 else "frames %d to %d (now %d)" % (r["start"], r["end"], r["frame"]))
        elif a.command == "release":
            show(a, check(b.request("release", all=True if a.all else None)),
                 lambda r: "released %d%s" % (r["released"], " (every client's)" if a.all else ""))
        elif a.command == "shot":
            path = os.path.abspath(a.path) if a.path else None   # (the bridge would take it as the socket folder's)
            r = check(b.request("shot", timeout=a.timeout, path=path, after=a.after))
            show(a, r, lambda r: r["path"])
        elif a.command == "wait":
            r = check(b.request("wait", timeout=a.timeout, event=a.event, frames=a.frames, frame=a.frame, state=a.state,
                                map=a.map))
            show(a, r, lambda r: "%s after %d frames (frame %d)%s" % (r["state"], r["waited"], r["frame"],
                                                                     "  map %s" % plain_text(r["map"]) if "map" in r else ""))
        elif a.command == "teleport":
            to = None if a.to is None else int(a.to) if a.to.isdigit() else a.to
            if to is None and (a.arrive or a.no_wait):
                raise Failed("teleport: --arrive and --no-wait go with a destination", 2)
            r = check(b.request("teleport", to=to, wait=False if a.no_wait else None,
                                timeout=None if to is None else a.timeout))
            if "destinations" in r:
                show(a, r, lambda r: "\n".join("%3d  %-28s %s" % (d["index"], plain_text(d["child"] or "-"),
                                                                  plain_text(d["adult"] or "-")) for d in r["destinations"]))
            elif not r.get("sent"):
                show(a, r, lambda r: "teleport %d asked for (child %s, adult %s): wait map to see it arrive" % (
                    r["teleport"], plain_text(r["child"] or "-"), plain_text(r["adult"] or "-")))
            elif not a.arrive:
                show(a, r, lambda r: "teleport %d sent: to %s%s" % (
                    r["teleport"], plain_text(r.get("map") or "-"),
                    "" if r.get("accepted", True) else " (but the game's event manager did not take it)"))
            else:   # (and until the game is in that map: the arrival)
                w = check(b.request("wait", timeout=a.arrive, event="map", map=r.get("map")))
                show(a, dict(r, arrived=w), lambda r: "teleport %d: in %s after %d frames (frame %d)" % (
                    r["teleport"], plain_text(w["map"]), w["waited"], w["frame"]))
        elif a.command == "age":
            r = check(b.request("age", to=a.to, wait=False if a.no_wait else None, timeout=a.timeout))
            if r.get("sent") is False:
                show(a, r, lambda r: "Link is the %s already" % plain_text(r["age"]))
            elif r.get("sent") is None:
                show(a, r, lambda r: "age switch to the %s asked for: wait loading_end to see it reload" % plain_text(r["age"]))
            elif not a.arrive:
                show(a, r, lambda r: "age switch sent: the %s, reloading %s%s" % (
                    plain_text(r["age"]), plain_text(r.get("map") or "-"),
                    "" if r.get("accepted", True) else " (but the game's event manager did not take it)"))
            else:   # (and until the reload's load has ended: in a dungeon the map has the same name for both ages, so
                    # a wait map would be met before the reload; loading_end counts from the ask, so a load already
                    # started when it is asked is still waited out)
                w = check(b.request("wait", timeout=a.arrive, event="loading_end"))
                show(a, dict(r, arrived=w), lambda r: "age switch: the %s, in %s after %d frames (frame %d)" % (
                    plain_text(r["age"]), plain_text(r.get("map") or "-"), w["waited"], w["frame"]))
        elif a.command == "option":
            if a.action == "get":
                r = check(b.request("option_get", name=a.name))
                rows = [r["option"]] if "option" in r else r["options"]
                show(a, r, lambda r: "\n".join("%-28s %4d  %s%s" % (o["key"], o["value"], plain_text(o["text"]),
                                                                    "  (restart)" if o.get("restart") else "") for o in rows))
            else:
                if a.value is None:
                    raise Failed("option set NAME VALUE", 2)
                v = {"on": 1, "true": 1, "off": 0, "false": 0}.get(a.value.lower())
                if v is None:
                    try:
                        v = int(a.value)
                    except ValueError:
                        raise Failed("VALUE: a number, on or off", 2)
                r = check(b.request("option_set", name=a.name, value=v))
                show(a, r, lambda r: "%s %d -> %d (%s)" % (r["option"]["key"], r["was"], r["option"]["value"], r["option"]["text"]))
        elif a.command == "mem":
            if a.action == "read":
                fmt = a.format if a.format in ("hex", "base64") else "hex"
                r = check(b.request("mem_read", addr=a.addr, size=a.size, format=fmt))
                data = bytes.fromhex(r["data"]) if r["format"] == "hex" else __import__("base64").b64decode(r["data"])
                show(a, r, lambda r: format_bytes(data, a.format))
            else:
                raw = a.hex.lower().replace("0x", "")
                try:
                    bytes.fromhex(raw)
                except ValueError:
                    raise Failed("HEX: an even number of hex digits", 2)
                r = check(b.request("mem_write", addr=a.addr, hex=raw))
                show(a, r, lambda r: "wrote %d bytes at %d" % (r["wrote"], r["addr"]))
        elif a.command == "saves":
            r = check(b.request("saves"))
            show(a, r, lambda r: saves_text(r))
        elif a.command == "slots":
            r = check(b.request("saves"))
            if a.thumbs:
                write_thumbs(r, a.thumbs)
            if a.json:
                print(json.dumps(r["slots"]))
            else:
                print(slots_text(r))
        elif a.command == "quit":
            show(a, check(b.request("quit")), lambda r: "quitting")
        elif a.command == "raw":
            req = json.loads(a.request)
            b.next_id += 1
            req.setdefault("id", b.next_id)
            b.send(req)
            while True:
                msg = b.line(float(req.get("timeout", 60)) + 10)
                if msg is None:
                    raise Failed("the game closed the connection", 2)
                if msg.get("id") == req["id"] and "ok" in msg:
                    print(json.dumps(msg))
                    return 0 if msg["ok"] else 1
        elif a.command == "watch":
            # (--log alone: the log lines only; with --events, those and the log lines)
            events = [e for e in (a.events or ("" if a.log else "present,state")).split(",") if e]
            if a.log and "log" not in events:
                events.append("log")
            check(b.request("subscribe", events=events, every=a.every, log=a.log,
                            flags=a.flags.split(",") if a.flags is not None else None,
                            changes=True if a.changes else None))
            n = 0
            while a.count is None or n < a.count:
                msg = b.line(3600 * 24)
                if msg is None:
                    return 0
                if "event" not in msg:
                    continue
                n += 1
                if a.json:
                    print(json.dumps(msg), flush=True)
                elif msg["event"] == "log":
                    print("log    %s" % plain_text(msg["line"]), flush=True)
                elif msg["event"] == "state":
                    print("state  frame %d: %s -> %s" % (msg["frame"], msg["from"], msg["state"]), flush=True)
                elif msg["event"] == "present":
                    print("frame  %d  fps %.1f  %s" % (msg["frame"], msg["fps"], msg["state"]), flush=True)
                elif msg["event"] == "map":
                    print("map    frame %d: %s -> %s" % (msg["frame"], plain_text(msg["from"] or "-"), plain_text(msg["map"])),
                          flush=True)
                elif msg["event"] == "oot":
                    print("game   " + json.dumps(msg["game"], ensure_ascii=True), flush=True)
                else:
                    print(json.dumps(msg), flush=True)
        return 0
    finally:
        b.close()


# (the one account the runtime has, host/nn_applet.c user_uid: its saves are <data>/hookshot/save/<this>)
SAVE_USER = "5a3c9e1f0b7d24680000000000000001"


def slot_name(text):
    """03, 3 or slot_03 -> slot_03"""
    t = text[5:] if text.lower().startswith("slot_") else text
    if not t.isdigit() or len(t) > 2:
        raise Failed("--slot: a slot number, 03 or slot_03", 2)
    return "slot_%02d" % int(t)


def plain_tree(path):
    """Refused (Failed) when path or anything under it is a link or not a plain file or folder: a save from elsewhere could
    hold a link to one of this user's files, which the copy would read and the game then write through"""
    for top, dirs, files in os.walk(path):
        for n in [top] + [os.path.join(top, d) for d in dirs] + [os.path.join(top, f) for f in files]:
            st = os.lstat(n)
            if not (stat.S_ISDIR(st.st_mode) or stat.S_ISREG(st.st_mode)) or \
                    getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
                raise Failed("--save: %s is a link or not a plain file: refused" % n, 2)


def plain_save_dirs(root):
    """The two entries the copier creates must be directories, not links; /home or a data root may itself be a link"""
    for path in (Path(root) / "save", Path(root) / "save" / SAVE_USER):
        try:
            st = path.lstat()
        except FileNotFoundError:
            continue
        if not stat.S_ISDIR(st.st_mode) or getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
            raise Failed("--save: %s is a link or not a plain folder: refused" % path, 2)


@session_call
def copy_save(src, data=None, slot=None, user=None):
    """A save folder (account.sav, option.sav, slot_NN/) copied to where the game reads it in the data folder; with slot,
    that slot alone (so Continue loads it). The source is only read, and holds plain files and folders only (no links).
    Returns the destination"""
    import shutil
    src = os.path.abspath(src)
    if os.path.islink(src) or not os.path.isdir(src):
        raise Failed("--save: %s is not a folder" % src, 2)
    plain_tree(src)
    names = os.listdir(src)
    slots = sorted(n for n in names if n.startswith("slot_") and os.path.isdir(os.path.join(src, n)))
    if not slots or not ({"account.sav", "option.sav"} & set(names)):
        raise Failed("--save: %s does not look like a save folder (account.sav or option.sav, and slot_NN folders): "
                     "it is <data>/hookshot/save/<user id>" % src, 2)
    if slot is not None:
        slot = slot_name(slot)
        if slot not in slots:
            raise Failed("--save: %s has no %s (it has %s)" % (src, slot, ", ".join(slots)), 2)
    root = os.path.abspath(user or os.path.join(data, "hookshot"))
    dest = os.path.join(root, "save", SAVE_USER)
    plain_save_dirs(root)
    source_real, dest_real = (os.path.normcase(os.path.realpath(p)) for p in (src, dest))
    try:
        overlap = os.path.commonpath([source_real, dest_real]) in (source_real, dest_real)
    except ValueError:   # (different Windows drives cannot overlap)
        overlap = False
    if overlap:
        raise Failed("--save: source and destination must be separate plain folders", 2)
    if os.path.exists(dest) and os.listdir(dest):
        raise Failed("--save: %s already holds saves: give an empty --data (or none: the run folder's own)" % dest, 2)
    os.makedirs(dest, exist_ok=True)
    for n in names:
        p = os.path.join(src, n)
        if n.startswith("slot_") and os.path.isdir(p):
            if slot is None or n == slot:
                shutil.copytree(p, os.path.join(dest, n), symlinks=True)   # (none: plain_tree; never followed)
        elif os.path.isfile(p):
            shutil.copy2(p, os.path.join(dest, n), follow_symlinks=False)
    return dest


def game_exe(game, exe=None):
    """A builder folder's root executable is the launcher, not the game; a source build has no bin folder"""
    if exe:
        return os.path.abspath(exe)
    name = "hookshot.exe" if WINDOWS else "hookshot"
    nested = os.path.join(game, "bin", name)
    if os.path.isfile(nested) or os.path.isdir(os.path.join(game, "bin")):
        return nested   # (an incomplete builder folder must not fall back to its launcher)
    return os.path.join(game, name)


def hos_steps(script):
    """HOS seconds/ms to game presents at 60 Hz: nearest frame, halves up; a positive hold is at least one frame"""
    steps = []
    for text in script.split(","):
        if not text.strip():
            continue
        match = re.fullmatch(r"\s*([0-9.+eE-]+):([^/,]+)(?:/([0-9.+eE-]+))?\s*", text)
        if not match:
            raise Failed("sequence: expected seconds:buttons[/milliseconds]", 2)
        try:
            at, held = float(match[1]), float(match[3] or 200)
        except ValueError:
            raise Failed("sequence: invalid seconds or milliseconds", 2)
        if not math.isfinite(at) or not math.isfinite(held) or at < 0 or held <= 0 or at > 3600 or held > 3600000:
            raise Failed("sequence: time outside 0..3600 seconds or hold outside 0..3600000 ms", 2)
        steps.append({"at": math.floor(at * 60 + 0.5), "frames": max(1, math.floor(held * 60 / 1000 + 0.5)),
                      "buttons": match[2].strip()})
    if not 1 <= len(steps) <= 256:
        raise Failed("sequence: 1..256 steps", 2)
    return steps


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def process_start(pid):
    """A live process's birth identity, not just its reusable pid; None for a dead process (including a Linux zombie)"""
    if not pid:
        return None
    if WINDOWS:
        import ctypes
        from ctypes import wintypes
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.OpenProcess.restype = wintypes.HANDLE
        k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        k.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        h = k.OpenProcess(0x100000 | 0x1000, False, pid)   # (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION)
        if not h:
            return None
        try:
            times = [wintypes.FILETIME() for _ in range(4)]
            if k.WaitForSingleObject(h, 0) != 258 or not k.GetProcessTimes(h, *[ctypes.byref(t) for t in times]):
                return None
            return {"clock": "windows-filetime", "ticks": (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime}
        finally:
            k.CloseHandle(h)
    try:
        fields = Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()
        if fields[0] in ("Z", "X"):
            return None
        return {"clock": "linux-start-ticks", "ticks": int(fields[19]),
                "boot": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
    except (OSError, ValueError, IndexError):
        return None


def same_process(pid, birth):
    return birth is not None and process_start(pid) == birth


def owners_alive(info):
    return any(same_process(**info[name]) for name in ("owner", "launcher") if info.get(name))


def wait_owners(session, timeout=15):
    deadline = time.monotonic() + timeout
    while owners_alive(session.info()):
        if time.monotonic() >= deadline:
            raise Failed("game exited, but its owner or wrapper has not exited: %s" % session.folder, 3)
        time.sleep(0.02)


def write_json(path, value):
    tmp = Path(str(path) + "." + uuid.uuid4().hex + ".tmp")
    try:
        with tmp.open("x", encoding="utf-8") as f:
            json.dump(value, f, indent=2)
            f.write("\n")
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


@contextlib.contextmanager
def run_lock(folder):
    """Only a manifest update holds this lock; never a bridge request or a process wait"""
    with (Path(folder) / "manifest.lock").open("r+b") as f:
        if WINDOWS:
            import msvcrt
            msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX)
        try:
            yield
        finally:
            f.seek(0)
            if WINDOWS:
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(f, fcntl.LOCK_UN)


def runs_root():
    return Path.cwd() / "work" / "control-runs"


@session_call
def list_runs(root=None):
    root = Path(root) if root else runs_root()
    return [Session(p.parent).info() for p in sorted(root.glob("*/manifest.json"))]


class Session:
    """A durable run created by this client; a surviving supervisor, not an attaching caller, owns its process"""

    @session_call
    def __init__(self, folder):
        self.folder = Path(folder).resolve()
        self.manifest = self.folder / "manifest.json"

    @session_call
    def info(self):
        with run_lock(self.folder):
            return self._read()

    def _read(self):
        with self.manifest.open(encoding="utf-8") as f:
            value = json.load(f)
        if value.get("schema_version") != MANIFEST_VERSION:
            raise Failed("unsupported run manifest version", 2)
        return value

    @session_call
    def update(self, **fields):
        with run_lock(self.folder):
            value = self._read()
            value.update(fields)
            write_json(self.manifest, value)
        return value

    @classmethod
    @session_call
    def attach(cls, folder):
        session = cls(folder)
        session.require_live()
        return session

    @session_call
    def require_live(self):
        info = self.info()
        if not same_process(info["pid"], info["process_start"]):
            raise Failed("run's game is gone or its pid was reused: %s" % self.folder, 2)
        return info

    @session_call
    def connect(self, timeout=10):
        return Bridge(self.require_live()["socket"], timeout, session=self)

    def prepare(self, cmd, args):
        args = dict(args)
        if cmd in ("press", "stick", "sequence") and args.get("exclusive") is None:
            if self.info()["profile"] == "test":
                args["exclusive"] = True
        if cmd == "shot" and not args.get("path"):
            args["path"] = str(self.folder / "shots" / ("shot-" + uuid.uuid4().hex + ".png"))
        return args

    def record(self, cmd, reply):
        if cmd != "shot":
            return
        # (Only metadata from the reply: an untrusted game must not make the client open an arbitrary file)
        artifact = {k: reply.get(k) for k in ("path", "frame", "present", "width", "height")}
        artifact["kind"] = "shot"
        with run_lock(self.folder):
            value = self._read()
            value["artifacts"].append(artifact)
            write_json(self.manifest, value)

    @session_call
    def request(self, cmd, timeout=None, **args):
        b = self.connect()
        try:
            return check(b.request(cmd, timeout=timeout, **args))
        finally:
            b.close()

    @session_call
    def finish(self):
        """Mark an orphaned, dead run; only its supervisor can know an exit code"""
        with run_lock(self.folder):
            info = self._read()
            if same_process(info["pid"], info["process_start"]) or owners_alive(info):
                raise Failed("run still has a live game or owner; use stop", 2)
            if info["state"] not in ("finished", "failed"):
                info.update(state="finished", ended_at=utc_now(), stop_method="observed-exit", save_drain="unknown",
                            process_exited=True)
                write_json(self.manifest, info)
        return info

    @session_call
    def stop(self, timeout=10, terminate=False):
        """Ask the original supervisor to quit and reap its child; never signal a pid read from a manifest"""
        if not math.isfinite(timeout) or timeout <= 0:
            raise Failed("stop timeout must be positive seconds", 2)
        request = None
        with run_lock(self.folder):
            info = self._read()
            if info["state"] not in ("finished", "failed"):
                if not same_process(**info["owner"]):
                    raise Failed("the run's owner is gone; an attached client will not kill its game", 2)
                if not same_process(info["pid"], info["process_start"]):
                    raise Failed("run's game is gone or its pid was reused", 2)
                if info.get("stop_request"):
                    raise Failed("a stop is already being waited for", 2)
                request = {"id": uuid.uuid4().hex, "timeout": timeout, "terminate": terminate,
                           "pid": info["pid"], "process_start": info["process_start"]}
                info["stop_request"] = request
                write_json(self.manifest, info)
        deadline = time.monotonic() + timeout + 15
        while True:
            info = self.info()
            if info["state"] in ("finished", "failed"):
                wait_owners(self, max(0, deadline - time.monotonic()))
                return info
            result = info.get("stop_result", {})
            if request and result.get("id") == request["id"] and not result.get("process_exited"):
                return info
            if not same_process(**info["owner"]):
                raise Failed("run owner ended without an exit receipt; inspect the run and use finish", 2)
            if time.monotonic() >= deadline:
                raise Failed("no stop receipt yet: %s (the run remains owned)" % self.folder, 3)
            time.sleep(0.05)

    @session_call
    def log(self, lines=100, since=None, limit=65536):
        """A byte cursor into this run's append-only UTF-8 log; every read and returned text is bounded"""
        if not 1 <= limit <= LOG_LIMIT or not 0 <= lines <= 10000 or (since is not None and since < 0):
            raise Failed("log: bytes 1..1048576, lines 0..10000, cursor >= 0", 2)
        info = self.info()
        with open(info["log"], "rb") as f:
            size = os.fstat(f.fileno()).st_size
            if since is not None and since > size:
                raise Failed("log cursor is past the end (the file was truncated?)", 2)
            start = max(0, size - limit) if since is None else since
            f.seek(start)
            data = f.read(min(limit, size - start))
        cursor = start + len(data)
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        text = decoder.decode(data, final=cursor == size and info["state"] in ("finished", "failed"))
        cursor -= len(decoder.getstate()[0])   # (at most three trailing bytes: the next read starts before them)
        if since is None:
            if start:
                text = text.partition("\n")[2]   # (a clipped first line, possibly starting inside a character)
            text = "".join(text.splitlines(keepends=True)[-lines:]) if lines else ""
        return {"text": text, "cursor": cursor, "start": start,
                "more": cursor < size, "truncated": start > 0 if since is None else False}

    @session_call
    def expect_log(self, text, since, timeout=10, regex=False, limit=65536):
        if not text or len(text) > 4096 or not math.isfinite(timeout) or timeout <= 0:
            raise Failed("log expect: a pattern of 1..4096 characters and a positive timeout", 2)
        try:
            pattern = re.compile(text) if regex else None
        except re.error as e:
            raise Failed("log regex: %s" % e, 2)
        deadline, cursor, pending = time.monotonic() + timeout, since, ""
        while True:
            part = self.log(since=cursor, limit=limit)
            pending += part["text"]
            if (pattern.search(pending) if pattern else text in pending):
                return dict(part, text=pending[-limit:], matched=True)
            pending = pending[-limit:]
            advanced = part["cursor"] != cursor
            cursor = part["cursor"]
            if time.monotonic() >= deadline:
                raise Failed("log expect timed out after cursor %d: %s" % (since, text), 3)
            if not part["more"] or not advanced:
                time.sleep(0.05)


def ready_status(reply, level):
    if not reply.get("ok"):
        return False
    if level == "bridge":
        return True
    if reply.get("frame", 0) < 1:
        return False
    if level == "present":
        return True
    game = reply.get("game") or {}
    return reply.get("state") == "play" and not reply.get("stalled", True) and game.get("event") is False \
        and game.get("loading") is False


def wait_ready(session, level, timeout, process):
    def check_exit():
        code = process.poll()
        if code is not None:
            raise Failed("game exited %d before ready=%s" % (code, level), 2)

    deadline, last = time.monotonic() + timeout, "no status reply"
    while True:
        check_exit()   # (also before testing the deadline: an already-exited child is not a readiness timeout)
        if (session.folder / "cancel").exists():
            raise Failed("launch cancelled while waiting for readiness", 3)
        left = deadline - time.monotonic()
        if left <= 0:
            check_exit()   # (the child may have exited between the poll and the deadline check)
            raise Failed("ready=%s timed out: %s; see %s" % (level, last, session.info()["log"]), 3)
        b, reply = None, None
        try:
            b = Bridge(session.info()["socket"], min(1, left))
            reply = b.request("status", reply_timeout=max(0, min(1, deadline - time.monotonic())))
            last = "state=%s frame=%s game=%s" % (reply.get("state"), reply.get("frame"), reply.get("game"))
        except Failed as e:
            last = str(e)
        finally:
            if b:
                b.close()
        check_exit()   # (a connection failure or reply can arrive after the child exited)
        if reply is not None and ready_status(reply, level):
            return reply
        time.sleep(min(0.05, max(0, deadline - time.monotonic())))


def stop_child(process, info, timeout, terminate):
    deadline, method, error = time.monotonic() + timeout, "quit", None
    b = None
    try:
        b = Bridge(info["socket"], min(timeout, 1))
        check(b.request("quit", reply_timeout=max(0.001, deadline - time.monotonic())))
    except (Failed, OSError) as e:
        error = str(e)
    finally:
        if b:
            b.close()
    try:
        process.wait(timeout=max(0, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        if process.poll() is None:   # (wait can lose the race to an exit at its deadline)
            if not terminate:
                return {"method": "quit-timeout", "process_exited": False, "forced": False,
                        "save_drain": "unsupported", "error": error}
            method = terminate_child(process)
    return {"method": method, "process_exited": True, "forced": method in ("terminate", "kill"), "exit_code": process.returncode,
            "save_drain": "unsupported", "error": error}


def terminate_child(process):
    # (Only a Popen child held by its original owner, never os.kill(manifest["pid"]))
    if process.poll() is not None:
        return "already-exited"
    process.terminate()
    try:
        process.wait(timeout=5)
        return "terminate"
    except subprocess.TimeoutExpired:
        if process.poll() is None:
            process.kill()
            process.wait()
            return "kill"
        return "terminate"


def supervise(folder):
    session, process = Session(folder), None
    info = session.update(owner={"pid": os.getpid(), "birth": process_start(os.getpid())}, state="starting")
    method, error, code = "exit", None, 0
    try:
        if (session.folder / "cancel").exists():
            raise Failed("launch cancelled before admission", 3)
        with open(info["log"], "ab", buffering=0) as out:
            process = subprocess.Popen(info["command"], cwd=info["cwd"], env=dict(os.environ, **info["environment"]),
                                       stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, close_fds=True)
        birth = process_start(process.pid)
        info = session.update(pid=process.pid, process_start=birth, started_at=utc_now())
        reply = wait_ready(session, info["ready"], info["ready_timeout"], process)
        session.update(state="running", ready_reply=reply)
        while process.poll() is None:
            info = session.info()
            request = info.get("stop_request")
            if request:
                if request["pid"] != process.pid or request["process_start"] != birth:
                    result = {"method": "refused", "process_exited": False, "error": "process identity changed"}
                else:
                    session.update(state="stopping")
                    result = stop_child(process, info, request["timeout"], request["terminate"])
                method = result["method"]
                session.update(stop_result=dict(result, id=request["id"]), stop_request=None,
                               state="stopping" if result["process_exited"] else "running")
            if (session.folder / "cancel").exists():
                method = "cancel-" + terminate_child(process)
            time.sleep(0.05)
    except BaseException as e:
        error, code = str(e), e.code if isinstance(e, Failed) else 2
        if process is not None:
            method = "startup-" + terminate_child(process)
    finally:
        session.update(state="failed" if error else "finished", error=error, error_code=code, ended_at=utc_now(),
                       exit_code=process.wait() if process is not None else None, stop_method=method,
                       process_exited=True, save_drain="unsupported")
    return code


@session_call
def start_session(game_dir, *, run=None, root=None, exe=None, image=None, romfs=None, user=None, cwd=None, data=None,
                  save=None, slot=None, profile="default", headless=False, sock=None, env=None, ready="bridge",
                  timeout=600, wrapper=None):
    """Start a durable supervisor, optionally through an admission wrapper (argv, never a shell command)"""
    game = os.path.abspath(game_dir)
    executable = game_exe(game, exe)
    if not os.path.isfile(executable):
        raise Failed("no game at %s" % executable, 2)
    if slot and not save:
        raise Failed("--slot goes with --save", 2)
    if profile not in ("default", "test") or ready not in ("bridge", "present", "play"):
        raise Failed("unknown profile or ready level", 2)
    if not math.isfinite(timeout) or timeout <= 0:
        raise Failed("launch timeout must be positive seconds", 2)
    if profile == "test" and (user or data):
        raise Failed("test profile owns its user folder; use --save for a copy, not --user or --data", 2)
    if cwd and not image:
        raise Failed("--cwd needs --image (otherwise the game changes back to its image folder)", 2)
    if WINDOWS and save and not user and profile != "test":
        raise Failed("--save on Windows needs --user or --profile test", 2)
    changes = dict(env or {})
    if any(not k or "=" in k or "\0" in k + v for k, v in changes.items()):
        raise Failed("--env NAME=VALUE", 2)
    if wrapper is not None and (not isinstance(wrapper, list) or not all(isinstance(s, str) and s for s in wrapper)):
        raise Failed("wrapper must be a JSON array of command arguments", 2)
    if run:
        folder = Path(run).absolute()
        if folder.is_symlink() or (folder.exists() and (not folder.is_dir() or any(folder.iterdir()))):
            raise Failed("run folder must be new or empty: %s" % folder, 2)
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
        folder = folder.resolve()
    else:
        parent = Path(root) if root else runs_root()
        parent.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="run-", dir=parent)).resolve()
    if not WINDOWS:
        folder.chmod(0o700)
    with (folder / "manifest.lock").open("xb") as lock:
        lock.write(b"\0")
    (folder / "shots").mkdir()
    log = folder / "log.txt"
    log.touch()
    user = os.path.abspath(user) if user else str(folder / "user") if profile == "test" else None
    data = os.path.abspath(data) if data else str(folder / "data") if save and not user else None
    if save:
        copy_save(save, data, slot, user=user)
    if user:
        Path(user).mkdir(parents=True, exist_ok=True)
    if data:
        changes.setdefault("XDG_DATA_HOME", data)
    if headless:
        changes.setdefault("HOS_HEADLESS", "1")
    socket_dir = None
    if not sock:
        if WINDOWS:
            sock = "hookshot-control-" + uuid.uuid4().hex
        else:
            runtime = os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir()
            if len(os.fsencode(runtime)) > 48:
                runtime = "/tmp"
            socket_dir = tempfile.mkdtemp(prefix="hctl-", dir=runtime)
            sock = os.path.join(socket_dir, "control.sock")
    if not WINDOWS:
        sock = os.path.abspath(sock)
    # (Keep the default profile's explicit --env precedence; record the effective endpoint too)
    changes.setdefault("HOS_CONTROL", sock)
    sock = changes["HOS_CONTROL"]
    if profile == "test":
        empty = folder / "empty-env.txt"
        empty.touch()
        changes.update(HOS_HEADLESS="1", HOS_AUDIO="0", HOS_ENV_FILE=str(empty))
    command = [executable]
    if image:
        command.append(os.path.abspath(image))
    for flag, value in (("romfs", romfs), ("user", user)):
        if value:
            command.extend(["--" + flag, os.path.abspath(value)])
    working = os.path.abspath(cwd) if cwd else game
    user_path = user or (os.path.join(working, "user") if WINDOWS or image else
                        os.path.join(changes.get("XDG_DATA_HOME", os.environ.get("XDG_DATA_HOME") or
                                                os.path.expanduser("~/.local/share")), "hookshot"))
    session = Session(folder)
    info = {"schema_version": MANIFEST_VERSION, "client_version": CLIENT_VERSION, "run": str(folder),
            "game": game, "executable": executable, "command": command, "cwd": working, "environment": changes,
            "profile": profile, "user": user_path, "save_source": os.path.abspath(save) if save else None, "slot": slot,
            "socket": sock, "socket_dir": socket_dir, "log": str(log), "pid": None, "process_start": None,
            "owner": {"pid": None, "birth": None}, "created_at": utc_now(), "started_at": None, "ended_at": None,
            "state": "queued", "exit_code": None, "stop_method": None, "save_drain": "unsupported",
            "process_exited": False, "artifacts": [], "ready": ready, "ready_timeout": timeout, "wrapper": wrapper or []}
    write_json(session.manifest, info)
    command = (wrapper or []) + [sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                                 "--run", str(folder), "_supervise"]
    flags = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS} if WINDOWS \
        else {"start_new_session": True}
    try:
        with (folder / "owner.log").open("ab") as out:
            owner = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                     close_fds=True, **flags)
    except OSError as e:
        session.update(state="failed", error=str(e), error_code=2, ended_at=utc_now(), process_exited=True)
        raise
    session.update(launcher={"pid": owner.pid, "birth": process_start(owner.pid)})
    import threading
    threading.Thread(target=owner.wait, daemon=True).start()   # (reap it if this caller outlives the supervisor)
    try:
        while True:
            info = session.info()
            if info["state"] == "running":
                return session
            if info["state"] == "failed":
                wait_owners(session)
                raise Failed(info["error"], info["error_code"])
            if owner.poll() is not None:
                error = "launch owner exited %s; see %s" % (owner.returncode, folder / "owner.log")
                session.update(state="failed", error=error, error_code=2, process_exited=info["pid"] is None,
                               ended_at=utc_now())
                raise Failed(error, 2)
            time.sleep(0.05)
    except BaseException:
        (folder / "cancel").touch()   # (also checked after a queued admission: no game may start later)
        raise


def launch(a):
    changes = {}
    for kv in a.env:
        if "=" not in kv:
            raise Failed("--env NAME=VALUE", 2)
        k, v = kv.split("=", 1)
        changes[k] = v
    session = start_session(a.game_dir, run=a.run, root=a.root, exe=a.exe, image=a.image, romfs=a.romfs, user=a.user,
                            cwd=a.cwd, data=a.data, save=a.save, slot=a.slot, profile=a.profile, headless=a.headless,
                            sock=a.sock, env=changes, ready=a.ready, timeout=a.timeout,
                            wrapper=json.loads(a.wrapper) if a.wrapper else None)
    info = session.info()
    print(json.dumps(info) if a.json else info["socket"])
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--sock", help="the bridge's socket (default $HOS_CONTROL)")
    p.add_argument("--run", help="an existing run folder to re-attach to")
    p.add_argument("--json", action="store_true", help="the replies as JSON lines")
    sub = p.add_subparsers(dest="command", required=True)

    def inputs(q):
        q.add_argument("--frames", type=int, default=8)
        q.add_argument("--at", type=int, default=0, help="frames from the next controller read")
        q.add_argument("--exclusive", action=argparse.BooleanOptionalAction, default=None,
                       help="the player's own devices left out meanwhile (default for test runs)")
        q.add_argument("--no-wait", action="store_true", help="reply as soon as it is queued")
        q.add_argument("--timeout", type=float)

    q = sub.add_parser("launch", help="start a game folder with the bridge on")
    q.add_argument("game_dir")
    q.add_argument("--exe", help="the game executable, instead of finding it in GAME_DIR")
    q.add_argument("--image", help="an explicit image.bin (the game's positional image argument)")
    q.add_argument("--romfs", help="the game's --romfs folder")
    q.add_argument("--user", help="the game's --user folder, on Linux and Windows")
    q.add_argument("--data", help="XDG_DATA_HOME for the run (its saves)")
    q.add_argument("--save", help="a save folder (<data>/hookshot/save/<user id>) copied in first (Linux); the source is "
                                  "only read")
    q.add_argument("--slot", help="with --save: that slot alone (03 or slot_03), so Continue loads it")
    q.add_argument("--run", default=argparse.SUPPRESS, help="a new or empty run folder")
    q.add_argument("--root", help="parent for unique run folders (default work/control-runs)")
    q.add_argument("--profile", choices=["default", "test"], default="default")
    q.add_argument("--ready", choices=["bridge", "present", "play"], default="bridge")
    q.add_argument("--cwd", help="working/cache folder; needs --image")
    q.add_argument("--wrapper", help="JSON argv prefix around the lifetime supervisor, for admission")
    q.add_argument("--headless", action="store_true")
    q.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    q.add_argument("--timeout", type=float, default=600, help="seconds to wait for the bridge (shaders may be prepared first)")
    sub.add_parser("status")
    q = sub.add_parser("press")
    q.add_argument("buttons", help="A, A+B, ZL+LUP, LX=-16000 ...")
    inputs(q)
    q = sub.add_parser("stick")
    q.add_argument("which", choices=["L", "R", "l", "r"])
    q.add_argument("x", type=float)
    q.add_argument("y", type=float)
    inputs(q)
    q = sub.add_parser("sequence")
    q.add_argument("script", help="<at>:<buttons>[/<frames>],...")
    q.add_argument("--hos", action="store_true", help="times in seconds and holds in ms, as HOS_INPUT_SCRIPT")
    q.add_argument("--exclusive", action=argparse.BooleanOptionalAction, default=None)
    q.add_argument("--no-wait", action="store_true")
    q.add_argument("--timeout", type=float)
    q = sub.add_parser("release", help="let go of this client's queued inputs and those of clients gone")
    q.add_argument("--all", action="store_true", help="every client's, also those still connected")
    q = sub.add_parser("shot")
    q.add_argument("path", nargs="?")
    q.add_argument("--after", type=int, help="the first present after this frame")
    q.add_argument("--timeout", type=float)
    q = sub.add_parser("wait")
    q.add_argument("event", choices=["present", "frame", "in_play", "state", "loading_start", "loading_end", "change", "map"])
    q.add_argument("--frames", type=int)
    q.add_argument("--frame", type=int)
    q.add_argument("--state")
    q.add_argument("--map", help="map: until the game is in this map (default: any other map than now)")
    q.add_argument("--timeout", type=float, default=60)
    q = sub.add_parser("option")
    q.add_argument("action", choices=["get", "set"])
    q.add_argument("name", nargs="?")
    q.add_argument("value", nargs="?")
    q = sub.add_parser("watch")
    q.add_argument("--events", help="present,state,log,map,oot (default present,state; with --log alone, log)")
    q.add_argument("--every", type=int, default=60)
    q.add_argument("--log", help="log lines that contain this text (adds log to the events)")
    q.add_argument("--count", type=int, help="stop after this many events")
    q.add_argument("--flags", help="with --events oot: comma-separated flag names or 0x and eight hex digits")
    q.add_argument("--changes", action="store_true", help="with --events oot: changed Bool/Int/Enum/UInt flags")
    q = sub.add_parser("mem")
    q.add_argument("action", choices=["read", "write"])
    q.add_argument("addr", help="a guest address, decimal or 0x...")
    q.add_argument("size", nargs="?", help="read: how many bytes")
    q.add_argument("hex", nargs="?", help="write: the bytes as hex")
    q.add_argument("--format", default="hex", choices=["hex", "base64", "u8", "u16", "u32", "u64", "f32"],
                   help="read: how to print the bytes")
    q = sub.add_parser("teleport", help="(OoT) the debug teleport: a destination's index or map; none lists them")
    q.add_argument("to", nargs="?")
    q.add_argument("--timeout", type=float, default=10, help="seconds for the game to take it (it does in play)")
    q.add_argument("--arrive", type=float, metavar="SECS", help="then wait up to SECS until the game is in the map")
    q.add_argument("--no-wait", action="store_true", help="reply once asked, without whether the game took it")
    q = sub.add_parser("age", help="(OoT) the Debug tab's Switch age: child or adult (none: the other age)")
    q.add_argument("to", nargs="?", choices=["child", "adult"])
    q.add_argument("--timeout", type=float, default=10, help="seconds for the game to take it (it does in play)")
    q.add_argument("--arrive", type=float, metavar="SECS", help="then wait up to SECS until the map has reloaded (its "
                   "load ended)")
    q.add_argument("--no-wait", action="store_true", help="reply once asked, without whether the game took it")
    sub.add_parser("saves", help="the save folder the game uses: its files, newest slot first")
    q = sub.add_parser("slots", help="the save slots: last written, sizes, scene; * the newest (Continue's)")
    q.add_argument("--thumbs", metavar="DIR", help="each slot's file-select picture written there as slot_NN.jpg")
    sub.add_parser("quit")
    q = sub.add_parser("raw")
    q.add_argument("request", help="a JSON request object")
    q = sub.add_parser("runs", help="list run manifests under a root")
    q.add_argument("root", nargs="?")
    sub.add_parser("show", help="show --run's manifest")
    sub.add_parser("finish", help="mark an orphaned dead run finished (exit code unknown)")
    q = sub.add_parser("stop", help="ask --run's owner to quit and wait; never kill an externally attached process")
    q.add_argument("--timeout", type=float, default=10)
    q.add_argument("--terminate", action="store_true", help="allow the original owner to terminate, then kill, its child")
    q = sub.add_parser("log", help="read --run's log without a bridge command")
    q.add_argument("--lines", type=int, default=100)
    q.add_argument("--since", type=int, help="byte cursor returned by the previous read")
    q.add_argument("--bytes", type=int, default=65536, help="read/output bound, at most 1048576")
    q.add_argument("--expect", help="wait for text after --since")
    q.add_argument("--regex", action="store_true")
    q.add_argument("--timeout", type=float, default=10)
    sub.add_parser("_supervise", help=argparse.SUPPRESS)
    a = p.parse_args(argv)
    try:
        return run(a)
    except Failed as e:
        print("hookshot_ctl: %s" % e, file=sys.stderr)
        return e.code
    except (BrokenPipeError, ConnectionResetError):
        print("hookshot_ctl: the game closed the connection", file=sys.stderr)
        return 2
    except json.JSONDecodeError as e:
        print("hookshot_ctl: not JSON: %s" % e, file=sys.stderr)
        return 2
    except OSError as e:
        print("hookshot_ctl: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
