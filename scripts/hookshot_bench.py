"""A repeatable benchmark route through the control bridge (docs/control.md): the same places, the same wait, the same
measurement window, so two builds, two settings or two PCs can be compared from numbers instead of impressions.

    python -I scripts/hookshot_bench.py GAME_DIR --name main [--env RX_VK_PARALLEL=3] [--places KokiriForest_Child,...]

One run:
1. starts the game folder with the bridge on (hookshot_ctl.py launch), with the switches given;
2. waits for its first frame, as long as a first start's preparation takes;
3. presses A until a save is loaded (the title, then Continue: the newest save);
4. for each place: teleports there, waits until no event or load is playing, lets it settle, then measures for --secs
   seconds without touching anything (Link standing, the camera where the teleport left it);
5. takes a screenshot of each place, quits the game and writes summary.json in the run folder.

What is measured comes from the game's own log lines over the window: "N presents, F fps; a frame: D draws", "GPU
time X ms a frame", "a frame's replay time (ms): ..." and "VRAM: U MB used of A MB allowed"; plus the bridge's fps and
the process's memory at the end of the window. Standard library only. Nothing leaves the PC: the run folder holds the
log, the screenshots and the summary.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hookshot_ctl as ctl  # noqa: E402

PLACES = ["KokiriForest_Child", "HyruleField_Child"]

RX_PRESENTS = re.compile(r"vk: (\d+) presents, ([\d.]+) fps; a frame: (\d+) draws")
RX_GPU = re.compile(r"vk: GPU time ([\d.]+) ms a frame")
RX_VRAM = re.compile(r"vk: VRAM: (\d+) MB used of (\d+) MB allowed")
RX_PAIR = re.compile(r"([a-z][a-z ]*?) ([\d.]+)")
REPLAY = "replay time (ms):"


def median(values, digits=2):
    return round(statistics.median(values), digits) if values else None


def parse_window(text):
    """The log lines of a measurement window, summed up: medians over the window's reports"""
    fps, draws, gpu, vram, replay = [], [], [], [], {}
    for line in text.splitlines():
        if m := RX_PRESENTS.search(line):
            fps.append(float(m[2]))
            draws.append(int(m[3]))
        elif m := RX_GPU.search(line):
            gpu.append(float(m[1]))
        elif REPLAY in line:
            for name, value in RX_PAIR.findall(line.split(REPLAY, 1)[1]):
                replay.setdefault(name.strip().replace(" ", "_"), []).append(float(value))
        elif m := RX_VRAM.search(line):
            vram.append((int(m[1]), int(m[2])))
    return {"reports": len(fps), "fps": median(fps, 1), "fps_min": min(fps) if fps else None, "draws": median(draws, 0),
            "gpu_ms": median(gpu), "replay_ms": {k: median(v) for k, v in replay.items()},
            "vram_used_mb": max(v[0] for v in vram) if vram else None, "vram_allowed_mb": vram[-1][1] if vram else None}


def process_memory_mb(pid):
    """The game's private memory (Windows: PrivateUsage) or resident anonymous memory (Linux: RssAnon), in MB; None if
    it cannot be read"""
    try:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            class Counters(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                    (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                                   "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                                   "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage",
                                                   "PrivateUsage")]
            k = ctypes.WinDLL("kernel32", use_last_error=True)
            psapi = ctypes.WinDLL("psapi", use_last_error=True)
            k.OpenProcess.restype = wintypes.HANDLE
            h = k.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                return None
            try:
                c = Counters()
                c.cb = ctypes.sizeof(c)
                if not psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb):
                    return None
                return round(c.PrivateUsage / 2**20)
            finally:
                k.CloseHandle(h)
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("RssAnon:"):
                    return round(int(line.split()[1]) / 1024)
    except (OSError, ValueError):
        return None
    return None


class Route:
    """The route's steps against a bridge (hookshot_ctl.Bridge, or a stand-in with the same request())"""

    MESSAGE_S = 8   # (an event this long is a message waiting for a button: the wake-up cucco a time of day hatches)

    def __init__(self, bridge, clock=time.monotonic, sleep=time.sleep):
        self.b, self.clock, self.sleep = bridge, clock, sleep
        self.presses = 0   # (A presses that closed the game's messages, in the summary)

    def close_message(self):
        self.ask("press", buttons="A", frames=8, timeout=30)
        self.presses += 1

    def ask(self, cmd, **args):
        return self.b.request(cmd, **args)

    def first_frame(self, total_s):
        """Waits for frame 1 in waits of 50 minutes: the bridge answers a wait within the hour, and a first start's
        preparation can take longer on a slow driver"""
        end = self.clock() + total_s
        while True:
            if self.ask("wait", event="frame", frame=1, timeout=3000).get("ok"):
                return
            if self.clock() > end:
                raise ctl.Failed("no first frame in %d s" % total_s, 3)

    def to_play(self, timeout=600):
        """The title, then the file select: A until a map is loaded and the state is play (Continue: the newest save)"""
        end = self.clock() + timeout
        while self.clock() < end:
            s = self.ask("status")
            if s.get("state") == "play" and s.get("map"):
                return s
            self.ask("press", buttons="A", frames=8, timeout=30)
            self.ask("wait", event="present", frames=90, timeout=60)
        raise ctl.Failed("no save loaded in %d s" % timeout, 3)

    def settled(self, timeout=120):
        """Until no event (a cutscene, a map change's) or load is playing, in play. An event that lasts MESSAGE_S is a
        message waiting for a button: A, every 2 s"""
        end = self.clock() + timeout
        s, since = {}, None
        while self.clock() < end:
            s = self.ask("status")
            g = s.get("game") or {}
            if s.get("state") == "play" and not g.get("event") and not g.get("loading"):
                return s
            if g.get("event") and not g.get("loading"):
                since = self.clock() if since is None else since
                if self.clock() - since >= self.MESSAGE_S:
                    self.close_message()
                    self.sleep(2)
                    continue
            else:
                since = None
            self.ask("wait", event="present", frames=30, timeout=30)
        raise ctl.Failed("still in an event or a load after %d s: %s" % (timeout, s.get("map")), 3)

    def go(self, place, current_map):
        """Teleports to place and waits until it is there and settled; teleports are 8 s apart at least (docs/control.md)"""
        self.settled()
        r = self.ask("teleport", to=place, timeout=30)
        if not r.get("ok") and r.get("error") == "refused":   # (the last teleport's 8 s, or an event that just began)
            self.sleep(9)
            self.settled()
            r = self.ask("teleport", to=place, timeout=30)
        if not r.get("ok"):
            raise ctl.Failed("teleport to %s: %s" % (place, r.get("message") or r.get("error")), 1)
        if (r.get("map") or "").lower() != (current_map or "").lower():
            self.ask("wait", event="map", map=place, timeout=60)
        return self.settled()

    def window(self, seconds, every=2.0):
        """The bridge's fps every `every` seconds over the window, and how many of those samples had an event playing (the
        game's own: a dialogue or a cutscene that time of day brings, which changes what is drawn)"""
        fps, events, since = [], 0, None
        end = self.clock() + seconds
        while self.clock() < end:
            s = self.ask("status")
            fps.append(s.get("fps") or 0.0)
            if (s.get("game") or {}).get("event"):
                events += 1
                since = self.clock() if since is None else since
                if self.clock() - since >= self.MESSAGE_S:   # (a message left open would be measured as a still screen)
                    self.close_message()
                    since = None
            else:
                since = None
            self.sleep(every)
        return fps, events


def saves_in(game_dir, src, slot=None):
    """(Windows, where the game keeps its saves in its own folder, user/save) The folder's saves set aside and src's copied
    in, so every run starts from the same save: the game saves by itself as it plays, and Continue loads the newest. src
    is a save folder (account.sav, option.sav, slot_NN/), only read; with slot, that slot alone. Returns the function
    that puts the folder's own saves back"""
    import shutil
    src = os.path.abspath(src)
    if os.path.islink(src) or not os.path.isdir(src):
        raise ctl.Failed("--save: %s is not a folder" % src, 2)
    ctl.plain_tree(src)
    names = os.listdir(src)
    slots = sorted(n for n in names if n.startswith("slot_") and os.path.isdir(os.path.join(src, n)))
    if not slots:
        raise ctl.Failed("--save: %s has no slot_NN folder (it is <game>/user/save/<user id>)" % src, 2)
    if slot is not None and ctl.slot_name(slot) not in slots:
        raise ctl.Failed("--save: %s has no %s" % (src, ctl.slot_name(slot)), 2)
    saves = os.path.join(os.path.abspath(game_dir), "user", "save")
    aside = saves + ".hookshot_bench"
    if os.path.exists(aside):
        raise ctl.Failed("--save: %s is left from a run that did not end: put it back as %s first" % (aside, saves), 2)
    had = os.path.isdir(saves)
    if had:
        os.rename(saves, aside)
    try:
        dest = os.path.join(saves, ctl.SAVE_USER)
        os.makedirs(dest)
        for n in names:
            p = os.path.join(src, n)
            if n.startswith("slot_") and os.path.isdir(p):
                if slot is None or n == ctl.slot_name(slot):
                    shutil.copytree(p, os.path.join(dest, n))
            elif os.path.isfile(p):
                shutil.copy2(p, os.path.join(dest, n))
    except Exception:
        shutil.rmtree(saves, ignore_errors=True)
        if had:
            os.rename(aside, saves)
        raise

    def put_back():
        shutil.rmtree(saves, ignore_errors=True)
        if had:
            os.rename(aside, saves)
    return put_back


def log_since(path, start):
    with open(path, "rb") as f:
        f.seek(start)
        return f.read().decode("utf-8", "replace")


def grep_log(path, needles, keep=3):
    """The last `keep` lines of the whole log holding each text"""
    found = {n: [] for n in needles}
    if needles:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                for n in needles:
                    if n in line:
                        found[n] = (found[n] + [line.strip()[:400]])[-keep:]
    return found


def run(a):
    out = os.path.abspath(a.out or os.path.join("bench-runs", time.strftime("%Y%m%d-%H%M%S") + "-" + a.name))
    os.makedirs(out, exist_ok=True)
    cmd = [sys.executable, "-I", os.path.join(HERE, "hookshot_ctl.py"), "--json", "launch", a.game_dir, "--run", out,
           "--timeout", "1800"] + (["--headless"] if a.headless else []) + sum((["--env", e] for e in a.env), [])
    put_back = None
    if a.save and ctl.WINDOWS:
        put_back = saves_in(a.game_dir, a.save, a.slot)
    elif a.save:   # (Linux: the run folder's own data folder, which launch copies the save into)
        cmd += ["--save", a.save] + (["--slot", a.slot] if a.slot else [])
    try:
        return measure(a, cmd, out)
    finally:
        if put_back:
            put_back()


def measure(a, cmd, out):
    started = subprocess.run(cmd, capture_output=True, text=True)
    if started.returncode:
        raise ctl.Failed("launch: " + (started.stderr or started.stdout).strip(), started.returncode)
    info = json.loads(started.stdout)
    bridge = ctl.Bridge(info["socket"], 30)
    route = Route(bridge)
    summary = {"name": a.name, "env": a.env, "places": {}, "game_dir": os.path.abspath(a.game_dir)}
    t0 = time.monotonic()
    try:
        route.first_frame(a.first_frame_s)
        summary["first_frame_s"] = round(time.monotonic() - t0)
        s = route.to_play()
        summary["version"], summary["renderer"] = s.get("version"), s.get("renderer")
        current = s.get("map")
        for place in a.places.split(","):
            s = route.go(place, current)
            current = s.get("map")
            route.sleep(a.settle)
            start = os.path.getsize(info["log"])
            fps, events = route.window(a.secs)
            r = parse_window(log_since(info["log"], start))
            r["bridge_fps"] = median(fps, 1)
            r["bridge_fps_min"] = min(fps) if fps else None
            r["event_share"] = round(events / len(fps), 2) if fps else None
            r["memory_mb"] = process_memory_mb(info["pid"])
            bridge.request("shot", path=os.path.join(out, place + ".png"), timeout=30)
            summary["places"][place] = r
            print("%-20s %s fps (min %s), %s draws, GPU %s ms, replay submit %s ms, VRAM %s of %s MB, memory %s MB%s" % (
                place, r["fps"], r["fps_min"], r["draws"], r["gpu_ms"], r["replay_ms"].get("submit"), r["vram_used_mb"],
                r["vram_allowed_mb"], r["memory_mb"],
                ", an event played in %d%% of it" % (r["event_share"] * 100) if r["event_share"] else ""), flush=True)
    finally:
        summary["wall_s"] = round(time.monotonic() - t0)
        summary["messages_closed"] = route.presses
        summary["lines"] = grep_log(info["log"], a.grep)
        with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        try:
            bridge.request("quit", timeout=10)
        except ctl.Failed:
            pass
        wait_exit(info["pid"], 60)   # (before the saves are put back: the game writes as it closes)
    print(out)
    return 0


def process_alive(pid):
    if os.name == "nt":
        import ctypes
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.OpenProcess.restype = ctypes.c_void_p
        h = k.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        try:
            code = ctypes.c_ulong(0)
            return bool(k.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code))) and code.value == 259   # STILL_ACTIVE
        finally:
            k.CloseHandle(ctypes.c_void_p(h))
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def wait_exit(pid, seconds):
    end = time.monotonic() + seconds
    while process_alive(pid) and time.monotonic() < end:
        time.sleep(0.5)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("game_dir", help="the game folder (hookshot.exe or hookshot beside image.bin)")
    p.add_argument("--name", required=True, help="a short name for the run, in its folder's name")
    p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="a switch for this run (repeatable)")
    p.add_argument("--places", default=",".join(PLACES), help="maps to teleport to, in order (comma-separated)")
    p.add_argument("--secs", type=int, default=60, help="seconds measured at each place (default 60)")
    p.add_argument("--settle", type=int, default=15, help="seconds left to settle at each place first (default 15)")
    p.add_argument("--grep", action="append", default=[], metavar="TEXT", help="log lines to keep in the summary")
    p.add_argument("--out", help="the run folder (default bench-runs/<time>-<name>)")
    p.add_argument("--save", help="a save folder (<user id> under the saves) every run starts from: the game saves as it "
                                  "plays, and Continue loads the newest. Windows: the game folder's saves are set aside "
                                  "and put back after the run; Linux: copied into the run's own data folder")
    p.add_argument("--slot", help="with --save: that slot alone (03 or slot_03)")
    p.add_argument("--headless", action="store_true", help="no window (HOS_HEADLESS)")
    p.add_argument("--first-frame-s", type=int, default=8 * 3600, help="how long a first start may prepare (default 8 h)")
    a = p.parse_args(argv)
    try:
        return run(a)
    except ctl.Failed as e:
        print("hookshot_bench: %s" % e, file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
