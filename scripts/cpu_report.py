"""The CPU report: where the game's CPU time goes, by category, thread and function, from all-thread sampled runs.

    python -I scripts/cpu_report.py <run folder>... --maps <folder with hookshot.map, hookshot_vk.map> --out report.md
        [--csv <folder>] [--functions scripts/cpu_functions.json] [--readobj <llvm-readobj>] [--top 40] [--cover 0.9]

A run folder holds the sampler's file (HOS_SAMPLE_ALL: `all.txt`, or `*_all.txt`), and, when the run was measured by
claude-perf's run_profile.py or an equivalent, `load.json` (the measured window: fps and cores) and `presents.csv` (frame times).
Without `load.json`, shares are still given as % of a core; "ms a frame" needs the run's fps (`--fps`).

Each line of the sampler's file is one thread at one instant: "<ms> <thread>|<frame>;<frame>;...", leaf first, each frame
module+offset. A sample is on the CPU unless its leaf is a waiting system call (cpu_categories.BLOCKING_SYSCALLS); naming a
system DLL's frames needs its exports, which --readobj (llvm-readobj) reads on Windows (without it, system leaves are counted
as on the CPU and the report says so).

Two views of each on-CPU sample:
  self   the category of the leaf frame: who executes the instructions;
  owner  the leaf, or for a leaf in the system, the C runtime or the driver, the first frame of ours above it: who asked for the
         work. A sleep's CPU inside ntdll under the host's WaitLightEvent is os/sync in this view.
Inclusive time per function counts each function once a sample when it is anywhere in the stack.

Categories and function descriptions are data (scripts/cpu_categories.py, scripts/cpu_functions.json), keyed on module,
object file, symbol and guest address, so the report regenerates from a new capture of a new build with no edits; symbols no
rule matches are listed as unclassified, hot functions with no description as undocumented.
"""
import argparse, bisect, collections, csv, glob, json, os, re, statistics, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpu_categories as cats  # noqa: E402

BASES = {".exe": 0x140000000, ".dll": 0x180000000}
MAP_LINE = re.compile(r"^\s*[0-9a-f]{4}:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S*)", re.I)


def load_map(path):
    """(addresses relative to the image, [(symbol, object)]) from an lld/MSVC linker map"""
    base, rows = None, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if base is None and "Preferred load address is" in line:
                base = int(line.split()[-1], 16)
            m = MAP_LINE.match(line)
            if m:
                obj = re.sub(r"^.*[\\/]", "", m.group(3))
                rows.append((int(m.group(2), 16), m.group(1), obj))
    if base is None:
        base = BASES.get(os.path.splitext(path)[1].replace(".map", ""), 0x140000000)
    rows.sort()
    return [r[0] - base for r in rows], [(r[1], r[2]) for r in rows]


def base_name(sym):
    """The key a function keeps across builds: lifted f_<guest address> and C names as they are; MSVC-mangled C++ as
    namespace::name, without the anonymous namespace's hash and the signature, which change between builds"""
    if "<lambda" in sym:
        m = re.search(r"\?\?([A-Za-z_]\w*)@((?:\w+@)*)@", sym)
        if m:
            scopes = [p for p in m.group(2).split("@") if p and not p.startswith("?A0x")]
            return "::".join(reversed(scopes)) + "::" + m.group(1) + "::lambda"
    if sym.startswith(("??0", "??1")):
        m = re.match(r"^\?\?[01](\w+)@((?:[\w?]+@)*)@", sym)
        if m:
            scopes = [p for p in m.group(2).split("@") if p and not p.startswith("?A0x")]
            return "::".join(list(reversed(scopes)) + [m.group(1), ("~" if sym[2] == "1" else "") + m.group(1)])
    if sym.startswith("?"):
        m = re.match(r"^\?\??\$?([A-Za-z_]\w*)@((?:[\w?]+@)*)@", sym)
        if m:
            scopes = [p for p in m.group(2).split("@") if p and not p.startswith("?A0x") and not p.startswith("?")]
            return "::".join(list(reversed(scopes)) + [m.group(1)])
    return sym


def load_exports(readobj, dll):
    """A system DLL's exports by RVA (llvm-readobj --coff-exports); ([], []) when they cannot be read"""
    path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", dll)
    if not readobj or not os.path.exists(path):
        return [], []
    try:
        out = subprocess.run([readobj, "--coff-exports", path], capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.TimeoutExpired):
        return [], []
    rows = sorted((int(rva, 16), name) for name, rva in re.findall(r"Name: (\S+)\s+RVA: 0x([0-9A-Fa-f]+)", out))
    return [r[0] for r in rows], [(r[1], dll) for r in rows]


LIFTED = "lifted game code"   # (all lifted functions as one row: per-function game rows are game-derived and stay local)


class Names:
    """frame "module+offset" -> (symbol, object, module, category)"""

    def __init__(self, maps_dir, readobj=None, game_detail=False):
        self.game_detail = game_detail   # (False: every lifted function is one row, LIFTED; no guest address is printed)
        self.syms = {}
        for mod, mp in (("hookshot.exe", "hookshot.map"), ("hookshot_vk.dll", "hookshot_vk.map")):
            p = os.path.join(maps_dir, mp)
            if os.path.exists(p):
                self.syms[mod] = load_map(p)
        self.readobj = readobj
        self.maps_dir = maps_dir
        self.cache = {}
        self.rules = [tuple(re.compile(x) if x else None for x in r[:3]) + (r[3],) for r in cats.RULES]
        self.block = re.compile(cats.BLOCKING_SYSCALLS)
        self.system = re.compile(cats.SYSTEM_MODULES)
        self.system_named = False

    def category(self, mod, obj, sym):
        for rm, ro, rs, cat in self.rules:
            if (rm is None or rm.search(mod)) and (ro is None or ro.search(obj)) and (rs is None or rs.search(sym)):
                return cat
        return "unclassified" if mod in OUR_MODULES else "external"

    def of(self, frame):
        r = self.cache.get(frame)
        if r is None:
            mod, _, off = frame.rpartition("+")
            if mod in ("hookshot", "hookshot_vk.so"):   # (a Linux capture: its ELF maps are not read, so do not mislabel it)
                raise SystemExit("a Linux capture (%s): cpu_report.py reads Windows captures with lld-link maps only" % mod)
            if mod not in self.syms and self.system.search(mod):
                local = os.path.join(self.maps_dir, mod + ".map")
                self.syms[mod] = load_map(local) if os.path.exists(local) else load_exports(self.readobj, mod)
                self.system_named |= bool(self.syms[mod][0])
            sym, obj = mod or frame, mod
            if mod in self.syms and self.syms[mod][0]:
                addrs, names = self.syms[mod]
                try:
                    i = bisect.bisect_right(addrs, int(off, 16)) - 1
                except ValueError:
                    i = -1
                if i >= 0:
                    sym, obj = names[i]
            cat = self.category(mod, obj, sym)
            if mod in OUR_MODULES and not self.game_detail and cat.startswith("game"):
                key = LIFTED
            elif mod in OUR_MODULES:
                key = base_name(sym)
            elif sym != mod:   # (a system DLL's nearest export: may be the wrong name for an internal function)
                key = "%s (%s)" % (sym, mod)
            else:   # (a module without names: the driver)
                key = mod
            r = self.cache[frame] = (key, obj, mod, cat, sym)
        return r

    def blocked(self, leaf):
        return bool(self.block.search(leaf[4])) and bool(self.system.search(leaf[2]))


GUEST = {"starts": [], "rows": []}
# (how a guest range of the local guest map was labelled -> how far the report trusts it)
TRUST = {"module": "reliable", "paths": "reliable", "paths+words": "reliable", "stacks": "reliable", "test": "reliable",
         "names": "reliable", "read": "reliable", "behaviour": "guess", "words": "guess", "calls": "guess", "layout": "unlabelled"}


def load_guest(rows):
    """rows of (start, end, module, subsystem, how[, functions]), sorted by start for bisect"""
    rows = sorted(rows, key=lambda r: r[0])
    GUEST["rows"] = rows
    GUEST["starts"] = [r[0] for r in rows]


def nn_name(sym):
    """imp__ZN2nn2os13WaitLightEventEP... -> nn::os::WaitLightEvent (the HLE entry the game called); other names unchanged"""
    m = re.match(r"^(?:imp_)?_ZN", sym)
    if not m:
        return sym.replace("n_nvn", "nvn", 1) if sym.startswith("n_nvn") else sym
    rest, parts = sym[m.end():], []
    while rest and rest[0].isdigit():   # (length-prefixed names until the nested name's E)
        n = re.match(r"^(\d+)", rest)
        k, d = int(n.group(1)), len(n.group(1))
        parts.append(rest[d:d + k])
        rest = rest[d + k:]
    return "::".join(parts) if parts else sym


def disp(sym):
    """a function as the tables print it: the HLE entries Hookshot implements by their SDK name (nn::os::LockMutex)"""
    return nn_name(sym) if sym.startswith(("imp__ZN", "_ZN")) else sym


def wait_key(named):
    """a blocked sample: the HLE entry the game called (nn::..., nvn...) and the game function that called it"""
    entry = next((n[0] for n in named if n[0].startswith(("imp__ZN", "_ZN", "n_nvn"))), None)
    lifted = next((n[0] for n in named if n[3].startswith("game")), None)
    if entry:
        return nn_name(entry) + (" from " + lifted if lifted else "")
    # (an HLE entry that ends in a tail call leaves no frame of its own: the host function that waits, and the game's caller)
    owner = next((n for n in named if is_ours(n[3])), named[0])
    return owner[0] + (" from " + lifted if lifted and owner[0] != lifted else "")


def guest_row(sym):
    m = re.match(r"^f_([0-9a-fA-F]{8,16})$", sym)
    if not m or not GUEST["rows"]:
        return None
    a = int(m.group(1), 16)
    i = bisect.bisect_right(GUEST["starts"], a) - 1
    if i >= 0 and a < GUEST["rows"][i][1]:
        return GUEST["rows"][i]
    return ()


def guest_subsystem(sym):
    """game/lifted -> game/<module>/<subsystem> from the guest map (f_<guest address>); None when not lifted or no map"""
    r = guest_row(sym)
    if r is None:
        return None
    if not r:
        return "game/unmapped"
    return "game/%s/%s" % (r[2], "unlabelled" if TRUST.get(r[4]) == "unlabelled" else r[3])


def guest_trust(sym):
    r = guest_row(sym)
    return None if r is None else (TRUST.get(r[4], "guess") if r else "unlabelled")


OURS = ("game/", "runtime/", "os/", "render/", "tooling/", "unclassified")
OUR_MODULES = ("hookshot.exe", "hookshot_vk.dll", "hookshot", "hookshot_vk.so")


def is_ours(cat):
    return cat.startswith(OURS)


def run_files(folder):
    for name in ("all.txt",):
        if os.path.exists(os.path.join(folder, name)):
            return os.path.join(folder, name)
    found = sorted(glob.glob(os.path.join(folder, "*_all.txt")))
    return found[0] if found else None


def analyse(path, names, fps=None, window=None, cands=()):
    """One run: counters of on-CPU samples by (view, key) and the run's instants"""
    instants = set()
    c = collections.defaultdict(collections.Counter)
    fcache = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            head, _, frames = line.rstrip("\n").partition("|")
            parts = head.split(" ", 1)
            if len(parts) != 2 or not frames:
                continue
            try:
                ms = float(parts[0])
            except ValueError:
                continue
            if window and not (window[0] <= ms / 1000.0 <= window[1]):
                continue
            thread = re.sub(r"\d+$", "N", parts[1]) if parts[1].startswith(("ModuleSystemWorker", "BaseProcExecutor", "NavMeshCAStepDepWorker")) else parts[1]
            if not names.game_detail:   # (an unnamed thread is logged by its guest entry address: not published)
                thread = re.sub(r"^Thread_[0-9a-fA-F]+$", "unnamed thread", thread)
            instants.add(ms)
            named = [names.of(fr) for fr in frames.split(";") if fr]
            if not named:
                continue
            leaf = named[0]
            c["thread_all"][thread] += 1
            if names.blocked(leaf):
                c["blocked_in"][(thread, wait_key(named))] += 1
                continue
            c["thread_cpu"][thread] += 1
            selfcat = guest_subsystem(leaf[0]) or leaf[3]
            owner = next((n for n in named if is_ours(n[3])), leaf)
            ownercat = guest_subsystem(owner[0]) or owner[3]
            c["self_cat"][selfcat] += 1
            c["owner_cat"][ownercat] += 1
            c["self_fn"][(leaf[0], selfcat)] += 1
            c["owner_fn"][(owner[0], ownercat)] += 1
            c["thread_owner"][(thread, ownercat.split("/")[0])] += 1
            trust = guest_trust(owner[0])
            if trust:
                c["game_trust"][trust] += 1
            seen = set()
            for n in named:
                if n[0] not in seen:
                    seen.add(n[0])
                    c["incl_fn"][(n[0], guest_subsystem(n[0]) or n[3])] += 1
            if cands:
                hit = set()
                for n in named:
                    ids = fcache.get(n[0])
                    if ids is None:
                        ids = fcache[n[0]] = frozenset(cid for cid, rx in cands if rx.search(n[0]))
                    hit |= ids
                for cid in hit:
                    c["cand"][cid] += 1
                    if thread in ("sead::MainThread", "Presentation_Thread"):
                        c["cand_path"][cid] += 1
            if ownercat == "unclassified":
                c["unclassified"][(owner[2], owner[1], owner[0])] += 1
    return c, len(instants)


def window_of(folder):
    """(fps, cores, (start_s, end_s)) from load.json when the run was measured, else (None, None, None)"""
    p = os.path.join(folder, "load.json")
    if not os.path.exists(p):
        return None, None, None
    j = json.load(open(p, encoding="utf-8"))
    w = j.get("window_present_s")
    fps = None
    pres = os.path.join(folder, "presents.csv")
    if w and os.path.exists(pres):
        rows = list(csv.DictReader(open(pres, encoding="utf-8")))
        if rows:
            t0 = int(rows[0]["return_ns"]) - int(rows[0]["interval_ns"])
            ts = [(int(r["return_ns"]) - t0) / 1e9 for r in rows]
            n = sum(1 for t in ts if w[0] <= t <= w[1])
            fps = n / (w[1] - w[0]) if w[1] > w[0] else None
    return fps, j.get("window_cores"), None


def log_stats(folder):
    """The renderer's own per-frame lines in game.log, medians: GPU time, the replay's submit time, draws"""
    p = os.path.join(folder, "game.log")
    if not os.path.exists(p):
        return {}
    text = open(p, encoding="utf-8", errors="replace").read()
    def grab(pat):
        xs = [float(x) for x in re.findall(pat, text)]
        # (the last lines: a run stops at the end of its measured window, so they are play; the title screen before the load
        # prints far more lines at an unlocked frame rate, so "the second half" would be the menu)
        return statistics.median(xs[-4:]) if xs else None
    return dict(gpu_ms=grab(r"vk: GPU time ([0-9.]+) ms a frame"),
                submit_ms=grab(r"a frame's replay time \(ms\): submit ([0-9.]+)"),
                draws=grab(r"presents, [0-9.]+ fps; a frame: ([0-9.]+) draws"))


def hle_calls(folder, blocks=4):
    """HOS_HLE_STATS=1's "hle:" summaries (every 5 s) in game.log: calls a second of each HLE entry, from the last `blocks`
    summaries (a run stops at the end of its measured window, so those are play). Names as the counter prints them: nn
    functions mangled without the "imp_" prefix (cut at 64 characters), NVN ones without the "n_" prefix."""
    log = os.path.join(folder, "game.log")
    if not os.path.exists(log):
        return {}, None
    lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
    heads = [i for i, l in enumerate(lines) if re.search(r"hle: \d+ calls of \d+ functions in ([0-9.]+) s", l)]
    if not heads:
        return {}, None
    use = heads[-blocks:]
    calls = collections.Counter()
    secs = 0.0
    for h in use:
        secs += float(re.search(r"in ([0-9.]+) s", lines[h]).group(1))
        for l in lines[h + 1:h + 40]:
            if "hle: " not in l or re.search(r"hle: \d+ calls of", l):
                break
            for n, name in re.findall(r"(\d+) ([^;]+?)(?:;|$)", l.split("hle: ", 1)[1]):
                calls[name.strip()] += int(n)
    return {k: v / secs for k, v in calls.items()}, secs


def calls_key(sym):
    """the counter's name for a frame key: imp__ZN... -> _ZN... (64 characters), n_nvnX -> nvnX"""
    if sym.startswith("imp_"):
        return sym[4:][:64]
    if sym.startswith("n_nvn"):
        return sym[2:]
    return sym


MEASURE = re.compile(r"\d\s*%|%\s*of|\d[\d,.]*\s+(million\s+|thousand\s+)?(\w+\s+)?(a|per)\s+(frame|second)\b|"
                     r"\bon-CPU\b|\bof (the |all )?(on-CPU )?samples\b|\bsamples (of|are|under|in)\b|"
                     r"\b(a third|half|a quarter|two thirds|three quarters|most) of (all|its|the)\b|\bthe (largest|hottest|most sampled)\b|"
                     r"\b(one|two|three|four|five|\d+) (of|in) (every )?(two|three|four|five|six|ten|\d+) "
                     r"(waits|calls|lookups|samples|frames|times|runs|misses|hits)\b|\b(dozens|hundreds|thousands|millions) of (times|calls)\b|"
                     r"\b(a|one|two|several|a few) (dozen|hundred|thousand|million)\b[\w ]{0,40}?\b(a|per) (frame|second)\b|"
                     r"\b(one|two|three|four|\d+) or (two|three|four|five|\d+) [\w-]+( [\w-]+)? (a|per) \w+|\bwas measured\b", re.I)
SPEED = re.compile(r"vi: game clock [0-9.]+: [0-9.]+ a second of wall time \(speed ([0-9.]+)\)")


def measured(folder):
    """An unsampled run's numbers: fps and cores from load.json/presents.csv, the game clock from game.log, frame times"""
    fps, cores, _ = window_of(folder)
    out = dict(name=os.path.basename(os.path.normpath(folder)), fps=fps, cores=cores)
    out["cpu_ms"] = cores * 1000.0 / fps if fps and cores else None
    log = os.path.join(folder, "game.log")
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    sp = [float(x) for x in SPEED.findall(text)]
    out["clock"] = statistics.median(sp[-4:]) if sp else None
    out["valid"] = out["clock"] is not None and abs(out["clock"] - 1.0) <= 0.01 + 1e-9 and fps is not None
    p, ld = os.path.join(folder, "presents.csv"), os.path.join(folder, "load.json")
    if os.path.exists(p) and os.path.exists(ld):
        w = json.load(open(ld, encoding="utf-8")).get("window_present_s")
        rows = list(csv.DictReader(open(p, encoding="utf-8")))
        if w and rows:
            t0 = int(rows[0]["return_ns"]) - int(rows[0]["interval_ns"])
            iv = sorted(int(r["interval_ns"]) / 1e6 for r in rows if w[0] <= (int(r["return_ns"]) - t0) / 1e9 <= w[1])
            if iv:
                out.update(p99=iv[min(len(iv) - 1, int(0.99 * len(iv)))], worst=iv[-1], over50=sum(x > 50 for x in iv),
                           over100=sum(x > 100 for x in iv), over250=sum(x > 250 for x in iv))
    return out


def machine(folder):
    """version, GPU and the run's switches, from game.log and process.json"""
    info = {}
    log = os.path.join(folder, "game.log")
    if os.path.exists(log):
        for line in open(log, encoding="utf-8", errors="replace"):
            if "hookshot v" in line and "version" not in info:
                info["version"] = line.split("hookshot ", 1)[1].strip()
            # (the device the renderer chose, logged with its features; "vk: device N:" lines list every device)
            m = re.search(r"vk: ([^;]+?, Vulkan \d+\.\d+, driver \w+); mesh shaders", line)
            if m and "gpu" not in info:
                info["gpu"] = m.group(1)
            if len(info) == 2:
                break
    pj = os.path.join(folder, "process.json")
    if os.path.exists(pj):
        env = json.load(open(pj, encoding="utf-8")).get("env", {})
        keep = ("HOS_FPS_MAX", "HOS_FPS_LOG", "HOS_WINDOW_SIZE", "HOS_ASPECT", "RX_VK_ASYNC", "RX_VK_PIPE_SKIP", "HOS_INPUT_FRAME_CLOCK")
        info["switches"] = " ".join("%s=%s" % (k, env[k]) for k in keep if k in env)
        rendering = {k: env[k] for k in env if k.startswith(("RX_VK_UPSCALE", "RX_VK_FRAMEGEN", "RX_VK_DLSS", "RX_VK_FSR",
                                                              "RX_VK_PACE", "HOS_DYNRES", "HOS_OUTPUT_RES", "HOS_HANDHELD"))}
        if rendering:
            info["switches"] += " " + " ".join("%s=%s" % kv for kv in sorted(rendering.items()))
    ini = os.path.join(folder, "hookshot_options.ini")   # (a runner may copy the game folder's options into the run)
    if os.path.exists(ini):
        opts = dict(l.strip().split("=", 1) for l in open(ini, encoding="utf-8", errors="replace") if "=" in l)
        info["menu"] = " ".join("%s=%s" % (k, opts[k]) for k in ("quality_preset", "frame_generation", "vsync", "dlss",
                                                                     "resolution", "upscaling", "output_res") if k in opts)
    if os.path.exists(log):   # (the effective settings, as the run itself logged them)
        found = []
        for line in open(log, encoding="utf-8", errors="replace"):
            for pat in (r"oe: output resolution .*", r"vk: swapchain .*", r"(vk|menu): .*(upscaler|frame generation|DLSS|FSR [34]).*(on|off|chosen|used|runs).*",
                        r"vk: dynamic resolution .*"):
                m = re.search(pat, line)
                if m and not any(f_.split(":")[0] == m.group(0).split(":")[0] and f_[:20] == m.group(0)[:20] for f_ in found):
                    found.append(m.group(0).strip())
            if len(found) >= 5:
                break
        if found:
            info["options"] = "; ".join(found)
    return info


def bottleneck_text(fps, gpu_ms, busy, serial):
    """the limit, in words, from the numbers: the GPU, a saturated thread, or the chain of waits"""
    frame = 1000.0 / fps if fps else None
    if frame and gpu_ms and gpu_ms > 0.8 * frame:
        return ("the GPU takes %s of the %s ms a frame, close to all of it: the GPU is the likely limit here, and CPU "
                "savings would mostly free CPU rather than raise the frame rate." % (fmt(gpu_ms, 1), fmt(frame, 1)))
    serial_names = {t for t, _b, _w in serial}
    hot = sorted(((t, b) for t, b in busy.items() if t in serial_names and b is not None and b >= 90.0), key=lambda kv: (-kv[1], kv[0]))
    if hot:
        return ("`%s`, one of the two serial threads, is on the CPU %s %% of the time: it is saturated, so its own work is "
                "the likely limit." % (hot[0][0], fmt(hot[0][1])))
    top_t = min(busy, key=lambda k: (-(busy[k] or 0), k))   # (the busiest thread, ties by name)
    parts = ["the GPU takes %s of the %s ms a frame and neither serial thread is saturated (the busiest thread overall: "
             "`%s`, %s %%), so the frame is most likely paced by the chain of hand-offs between threads" % (
                 fmt(gpu_ms, 1), fmt(frame, 1), top_t, fmt(busy[top_t]))]
    for t, run_pct, waits in serial:
        if waits:
            parts.append("`%s` runs %s %% of the time and waits most in `%s` (%s %%)" % (t, fmt(run_pct), waits[0][0], fmt(waits[0][1])))
    return ("; ".join(parts) + ". So the frame's path runs through the serial threads' own work and the work they wait "
            "for; a saving there can raise the frame rate, while a saving elsewhere frees CPU. Sampling shows where the "
            "threads wait, not which awaited job finishes last in each frame, so it cannot say which saving raises the "
            "frame rate most; a per-frame trace of the job system would settle that.")


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def fmt(x, nd=1):
    return "-" if x is None else ("%.*f" % (nd, x))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("runs", nargs="+")
    p.add_argument("--maps", required=True)
    p.add_argument("--out")
    p.add_argument("--csv")
    p.add_argument("--functions", action="append", default=[], help="more description files (JSON, same keys), over "
                   "scripts/cpu_functions.json: e.g. a local file of generated lines for lifted functions")
    p.add_argument("--readobj")
    p.add_argument("--guest-map", help="LOCAL ONLY: a Python file with GUEST_MAP rows (start, end, module, subsystem, how, "
                   "functions) made from the game binary. With it lifted code is listed per function and per subsystem; "
                   "without it (what may be published) all lifted code is one row")
    p.add_argument("--candidates", action="append", default=[], help="LOCAL ONLY: more candidate files (same format), "
                   "e.g. ones that match lifted functions or game subsystems")
    p.add_argument("--fps", type=float, help="the runs' fps when they have no load.json")
    p.add_argument("--title", default="CPU profile")
    p.add_argument("--measured", nargs="*", default=[], help="unsampled runs of the same scene: fps, CPU and frame times "
                   "come from these (sampling costs some fps); a run whose game clock is off 1.000 by more than 0.01 is "
                   "listed but left out of the medians")
    p.add_argument("--menu", default="", help="the Insert-menu options the runs used, when the run folders hold no "
                   "hookshot_options.ini")
    p.add_argument("--machine", default="", help="a line naming the CPU, RAM, OS and driver version")
    p.add_argument("--into", help="write the report into this markdown file between <!-- MEASURED:<section> --> and "
                   "<!-- END MEASURED:<section> --> instead of --out")
    p.add_argument("--section")
    p.add_argument("--details", help="write the full function table (to --cover) to this markdown file and link it from the "
                   "report, which then shows the first --top rows only")
    p.add_argument("--hashes", help="a sha256sum file of the build's binaries")
    p.add_argument("--calls", help="a run made with HOS_HLE_STATS=1: calls a frame and the cost of a call for the HLE entries")
    p.add_argument("--top", type=int, default=40)
    p.add_argument("--cover", type=float, default=0.9, help="list functions until this share of on-CPU time is covered")
    a = p.parse_args()

    names = Names(a.maps, a.readobj, game_detail=bool(a.guest_map))
    rows = []
    if a.guest_map:
        ns = {}
        exec(compile(open(a.guest_map, encoding="utf-8").read(), a.guest_map, "exec"), ns)   # (a local data file of literals)
        rows = list(ns["GUEST_MAP"])
    load_guest(rows)
    docs = {}
    for fp in [os.path.join(os.path.dirname(os.path.abspath(__file__)), "cpu_functions.json")] + a.functions:
        if os.path.exists(fp):
            for k, v in json.load(open(fp, encoding="utf-8")).items():
                docs.setdefault(k, v) if v.get("source") == "generated" else docs.__setitem__(k, v)
    cdata = {"candidates": [], "game_own": []}
    for cfile in [os.path.join(os.path.dirname(os.path.abspath(__file__)), "cpu_candidates.json")] + a.candidates:
        if os.path.exists(cfile):
            for k, v in json.load(open(cfile, encoding="utf-8")).items():
                cdata.setdefault(k, []).extend(v)
    cands = [(x["id"], re.compile(x["match"])) for x in cdata["candidates"]]
    runs = []
    for folder in a.runs:
        path = run_files(folder)
        if not path:
            print("no sampler file in %s" % folder, file=sys.stderr)
            continue
        fps, cores, _ = window_of(folder)
        c, inst = analyse(path, names, cands=cands)
        runs.append(dict(name=os.path.basename(os.path.normpath(folder)), fps=fps or a.fps, cores=cores, c=c, inst=inst,
                         log=log_stats(folder)))
    if not runs:
        raise SystemExit("no runs")
    if not any(m in names.syms and names.syms[m][0] for m in OUR_MODULES):
        # (a Linux capture names hookshot / hookshot_vk.so, whose ELF maps this script does not read yet)
        raise SystemExit("no linker map of hookshot.exe or hookshot_vk.dll in --maps: cpu_report.py reads Windows captures "
                         "with lld-link maps only")

    def share(run, view, key):   # (% of one core)
        return 100.0 * run["c"][view][key] / run["inst"] if run["inst"] else 0.0

    def ms(run, pct):
        return pct * 10.0 / run["fps"] if run["fps"] else None

    def table(view, keys):
        rows = []
        for k in keys:
            pcts = [share(r, view, k) for r in runs]
            rows.append((k, med(pcts), min(pcts), max(pcts), med([ms(r, x) for r, x in zip(runs, pcts)])))
        return rows

    out = []
    w = out.append
    w("# %s\n" % a.title)
    w("Generated by `scripts/cpu_report.py` from %d run(s): %s. Shares are %% of one CPU core over the sampled instants, the "
      "median of the runs (min-max in brackets); ms a frame = share x 10 / fps.\n" % (len(runs), ", ".join(r["name"] for r in runs)))
    glance_at = len(out)
    glance = {}
    if not names.system_named:
        w("System DLL frames were not named (no `--readobj`): blocked waits inside them are counted as on-CPU `system/other`.\n")
    if a.measured:
        ms_runs = [measured(f) for f in a.measured]
        info = machine(a.measured[0])
        w("## Measured without the sampler\n")
        w("The frame rate and CPU numbers of the scene. Sampling costs some frame rate, so these come from runs without it; "
          "the shares below come from the sampled runs.\n")
        if info.get("version") or info.get("gpu"):
            w("- Hookshot %s; GPU %s%s" % (info.get("version", "?"), info.get("gpu", "?"), ("; " + a.machine) if a.machine else ""))
        if a.hashes and os.path.exists(a.hashes):
            w("- Binaries (sha256): " + ", ".join("`%s` %s" % (l.split()[1].lstrip("*"), l.split()[0][:16])
                                                 for l in open(a.hashes, encoding="utf-8") if l.strip()))
        if info.get("switches"):
            w("- Switches: `%s`" % info["switches"])
        if info.get("options"):
            w("- Graphics settings, as the run logged them: %s" % info["options"])
        if info.get("menu") or a.menu:
            w("- Insert-menu options: %s" % (info.get("menu") or a.menu))
        w("")
        w("| run | fps | CPU ms a frame | cores | game clock | p99 ms | worst ms | frames >50 / >100 / >250 ms | counted |")
        w("|---|---|---|---|---|---|---|---|---|")
        for m in ms_runs:
            w("| %s | %s | %s | %s | %s | %s | %s | %s / %s / %s | %s |" % (
                m["name"], fmt(m["fps"]), fmt(m["cpu_ms"]), fmt(m["cores"], 2), fmt(m["clock"], 3), fmt(m.get("p99")),
                fmt(m.get("worst")), m.get("over50", "-"), m.get("over100", "-"), m.get("over250", "-"),
                "yes" if m["valid"] else "no (clock)"))
        ok = [m for m in ms_runs if m["valid"]]
        glance.update(fps=med([m["fps"] for m in ok]), cpu_ms=med([m["cpu_ms"] for m in ok]), cores=med([m["cores"] for m in ok]),
                      n=len(ok), over100=sum(m.get("over100", 0) for m in ok))
        w("| **median (%d counted)** | **%s** | **%s** | **%s** | **%s** | **%s** | max **%s** | %d / %d / %d in all | |" % (
            len(ok), fmt(med([m["fps"] for m in ok])), fmt(med([m["cpu_ms"] for m in ok])), fmt(med([m["cores"] for m in ok]), 2),
            fmt(med([m["clock"] for m in ok]), 3), fmt(med([m.get("p99") for m in ok])),
            fmt(max([m.get("worst") or 0 for m in ok]) if ok else None), sum(m.get("over50", 0) for m in ok),
            sum(m.get("over100", 0) for m in ok), sum(m.get("over250", 0) for m in ok)))
        w("")
    w("## Runs\n")
    w("Sampled shares are occupancy: a thread counts as on the CPU when its user-mode leaf is not a waiting system call, "
      "which includes a thread that is ready but preempted, and leaves out time in the kernel. The measured process cores "
      "(from the operating system's CPU time) are the reference; the two columns below show how far the sampled total is "
      "from it.\n")
    w("| run | fps | cores (measured) | cores (sampled, on-CPU) | instants | on-CPU samples | blocked samples |")
    w("|---|---|---|---|---|---|---|")
    for r in runs:
        oncpu = sum(r["c"]["thread_cpu"].values())
        blocked = sum(r["c"]["thread_all"].values()) - oncpu
        w("| %s | %s | %s | %s | %d | %d | %d |" % (r["name"], fmt(r["fps"]), fmt(r["cores"], 2),
                                                fmt(oncpu / r["inst"] if r["inst"] else None, 2), r["inst"], oncpu, blocked))
    w("")

    for view, title in (("owner_cat", "By category: who asked for the work (owner view)"), ("self_cat", "By category: who executes it (self view)")):
        keys = sorted({k for r in runs for k in r["c"][view]}, key=lambda k: (-med([share(r, view, k) for r in runs]), str(k)))
        tops_run = []
        for r in runs:
            t_ = collections.Counter()
            for k, n in r["c"][view].items():
                t_[k.split("/")[0]] += n
            tops_run.append({k: 100.0 * v / r["inst"] for k, v in t_.items()} if r["inst"] else {})
        tops = {k: med([t.get(k, 0.0) for t in tops_run]) for k in {k for t in tops_run for k in t}}
        w("## %s\n" % title)
        w("| category | % of a core | ms a frame |")
        w("|---|---|---|")
        if view == "owner_cat":
            glance["tops"] = sorted(tops.items(), key=lambda kv: (-kv[1], kv[0]))
            subs = {k: med([share(r, view, k) for r in runs]) for k in keys}
            glance["subs"] = sorted(((k, v) for k, v in subs.items() if "/" in k), key=lambda kv: (-kv[1], kv[0]))
        for top, pct in sorted(tops.items(), key=lambda kv: (-kv[1], kv[0])):
            w("| **%s** | **%s** | **%s** |" % (top, fmt(pct), fmt(med([ms(r, t.get(top, 0.0)) for r, t in zip(runs, tops_run)]), 2)))
            for k, m_, lo, hi, msf in table(view, [k for k in keys if k.split("/")[0] == top]):
                if k != top:
                    w("| &nbsp;&nbsp;%s | %s (%s-%s) | %s |" % (k, fmt(m_), fmt(lo), fmt(hi), fmt(msf, 2)))
        w("")

    w("## What limits the frame\n")
    w("Three things can set the frame time: the GPU, when its time a frame is close to the frame time; one of the two serial "
      "threads below, when it is on the CPU nearly all the time; or the chain of waits between threads. Occupancy shows "
      "which threads are busy and where the serial threads wait; it does not by itself prove which dependency is the "
      "limit, so the reading below says how far the numbers go.\n")
    fps_m = med([r["fps"] for r in runs])
    gpu = med([r["log"].get("gpu_ms") for r in runs])
    sub = med([r["log"].get("submit_ms") for r in runs])
    w("| | value |")
    w("|---|---|")
    w("| frame time of the sampled runs (1000 / their fps) | %s ms |" % fmt(1000.0 / fps_m if fps_m else None, 2))
    w("| GPU time a frame (renderer's log) | %s ms |" % fmt(gpu, 2))
    w("| replay's submit time a frame (renderer's log) | %s ms |" % fmt(sub, 2))
    w("| draws a frame | %s |" % fmt(med([r["log"].get("draws") for r in runs]), 0))
    w("")
    w("| thread | busy (on CPU / sampled) | on CPU, % of a core |")
    w("|---|---|---|")
    def busy_shares():
        return {t_: med([100.0 * r["c"]["thread_cpu"][t_] / max(1, r["c"]["thread_all"][t_]) for r in runs])
                for t_ in {k for r in runs for k in r["c"]["thread_all"]}}

    busy = sorted(th_all := {k for r in runs for k in r["c"]["thread_all"]},
                  key=lambda k: (-med([100.0 * r["c"]["thread_cpu"][k] / max(1, r["c"]["thread_all"][k]) for r in runs]), k))
    for t in busy[:12]:
        b = med([100.0 * r["c"]["thread_cpu"][t] / max(1, r["c"]["thread_all"][t]) for r in runs])
        w("| %s | %s %% | %s |" % (t, fmt(b), fmt(med([share(r, "thread_cpu", t) for r in runs]))))
    w("")

    w("### The frame's two serial threads: running, and what they wait in\n")
    w("The game's main thread (`sead::MainThread`) builds each frame's commands; the game's `Presentation_Thread` submits "
      "them, and with the default `RX_VK_ASYNC=0` the renderer replays each submission on Vulkan on that same thread. When "
      "neither is saturated, the frame is paced by what each waits for: the other thread, the game's job workers (when the "
      "main thread waits for their jobs, their work is on the frame's path too), or the GPU and the display.\n")
    w("| thread | running | waits in (top 6, % of the thread's samples) |")
    w("|---|---|---|")
    for t in ("sead::MainThread", "Presentation_Thread"):
        if not any(r["c"]["thread_all"][t] for r in runs):
            continue
        run_pct = med([100.0 * r["c"]["thread_cpu"][t] / max(1, r["c"]["thread_all"][t]) for r in runs])
        waits = collections.Counter()
        for r in runs:
            allt = r["c"]["thread_all"][t] or 1
            for (tt, f), n in r["c"]["blocked_in"].items():
                if tt == t:
                    waits[f] += 100.0 * n / allt / len(runs)
        glance.setdefault("serial", []).append((t, run_pct, waits.most_common(2)))
        w("| %s | %s %% | %s |" % (t, fmt(run_pct), "; ".join("`%s` %s %%" % (f, fmt(p)) for f, p in waits.most_common(6))))
    w("")
    w("**Reading:** " + bottleneck_text(fps_m, gpu, busy_shares(), glance.get("serial", [])) + "\n")

    if any(r["c"]["game_trust"] for r in runs):
        w("## How far the game's subsystem labels can be trusted\n")
        w("Share of lifted game code's on-CPU samples by how its guest range was labelled: reliable (the module table, source "
          "paths in the code, its place in every stack), guess (what it calls, words it logs, behaviour) or unlabelled.\n")
        w("| label | % of game samples |")
        w("|---|---|")
        for k in ("reliable", "guess", "unlabelled"):
            pcts = [100.0 * r["c"]["game_trust"][k] / max(1, sum(r["c"]["game_trust"].values())) for r in runs]
            w("| %s | %s |" % (k, fmt(med(pcts))))
        w("")

    w("## Threads\n")
    w("| thread | on CPU, % of a core | ms a frame | of which game | runtime | os | render | system/driver/crt |")
    w("|---|---|---|---|---|---|---|---|")
    th = sorted({k for r in runs for k in r["c"]["thread_cpu"]}, key=lambda k: (-med([share(r, "thread_cpu", k) for r in runs]), str(k)))
    for t in th[:30]:
        parts = [med([share(r, "thread_owner", (t, top)) for r in runs]) for top in ("game", "runtime", "os", "render")]
        other = med([share(r, "thread_cpu", t) - sum(share(r, "thread_owner", (t, top)) for top in ("game", "runtime", "os", "render"))
                     for r in runs])
        w("| %s | %s | %s | %s |" % (t, fmt(med([share(r, "thread_cpu", t) for r in runs])),
                                     fmt(med([ms(r, share(r, "thread_cpu", t)) for r in runs]), 2),
                                     " | ".join(fmt(x) for x in parts + [other])))
    w("")

    total = med([sum(r["c"]["owner_fn"].values()) for r in runs]) or 1
    cps, csecs = hle_calls(a.calls) if a.calls else ({}, None)
    if a.calls and not cps:
        raise SystemExit("--calls %s: no 'hle:' summaries in its game.log (HOS_HLE_STATS=1 needs a build with the HLE call "
                         "counters, see docs/cpu-profile.md)" % a.calls)
    cfps = window_of(a.calls)[0] if a.calls else None
    if cps:
        w("Call counts come from `%s`, a run of the same scene with the HLE call counters on (`HOS_HLE_STATS=1`), %d s of "
          "play at %s fps: **calls a frame** = calls a second / that run's fps; **us a call** = the function's ms a frame "
          "(owner view) x 1000 / calls a frame.\n" % (os.path.basename(os.path.normpath(a.calls)), int(csecs), fmt(cfps)))
    def by_sym(view):   # (% of a core of each function, whatever its category, a run)
        out = []
        for r in runs:
            d = collections.Counter()
            for (sym_, _cat), n in r["c"][view].items():
                d[sym_] += n
            out.append({k: 100.0 * v / r["inst"] for k, v in d.items()} if r["inst"] else {})
        return out

    self_s, incl_s = by_sym("self_fn"), by_sym("incl_fn")
    oncpu = med([100.0 * sum(r["c"]["thread_cpu"].values()) / r["inst"] for r in runs if r["inst"]]) or 1.0

    def doc_text(sym):
        d = docs.get(sym, {})
        # (a description states what a function does; a sentence that states a measurement would go stale with the next
        # build or scene, so it is left out, and every number on the page comes from the capture. The first sentence of
        # 'does' is the behaviour: measured parentheses are taken out of it rather than dropping it)
        def keep(part, first):
            out_ = []
            for i_, x in enumerate(re.split(r"(?<=[.;])\s+(?=[A-Z(])", part or "")):
                if MEASURE.search(x) and first and i_ == 0:
                    x = re.sub(r"\s*\([^()]*\)", lambda m_: "" if MEASURE.search(m_.group(0)) else m_.group(0), x)
                if x and not MEASURE.search(x):
                    out_.append(x)
            return " ".join(out_)
        text = (keep(d.get("does", ""), True) + " " + keep(d.get("note", ""), False)).strip()
        if text and d.get("source") == "generated":
            text = "(generated) " + text
        return d, text.replace("|", "/")

    # (one table, the owner view, listed until --cover of the on-CPU time is covered, with each function's self and
    # inclusive share beside its owner share; the first --top rows shown, the rest folded)
    keys = sorted({k for r in runs for k in r["c"]["owner_fn"]}, key=lambda k: (-med([share(r, "owner_fn", k) for r in runs]), str(k)))
    w("## Functions: what each costs\n")
    w("Every function in the order of its **owner** share (the work it asked for: its own instructions, and the system, "
      "C runtime and driver time below it); **self** is the time in its own instructions, **inclusive** the time with it "
      "anywhere in the stack (callees included). Listed until %d %% of the on-CPU time is covered; the first %d rows "
      "are shown, the rest are folded below them.\n" % (int(a.cover * 100), a.top))
    withcalls = bool(cps)
    head = ("| function | category | owner: %% of a core | ms a frame | self | inclusive |%s what it does |"
            % (" calls a frame | us a call |" if withcalls else ""))
    rule = "|---|---|---|---|---|---|%s---|" % ("---|---|" if withcalls else "")
    rows, undocumented = [], []
    run_oncpu = [100.0 * sum(r["c"]["thread_cpu"].values()) / r["inst"] if r["inst"] else 0.0 for r in runs]
    run_cov = [0.0] * len(runs)
    covered = 0.0
    for k, m_, lo, hi, msf in table("owner_fn", keys):
        if med([c_ / o_ if o_ else 1.0 for c_, o_ in zip(run_cov, run_oncpu)]) >= a.cover:
            break
        sym, cat = k
        d, text = doc_text(sym)
        run_cov = [c_ + share(r, "owner_fn", k) for c_, r in zip(run_cov, runs)]
        covered = med(run_cov)
        if not d.get("does"):
            undocumented.append(sym)
        extra = ""
        if withcalls:
            cpf = cps.get(calls_key(sym), 0) / cfps if cfps else None
            extra = " %s | %s |" % (fmt(cpf, 0) if cpf else "-", fmt(msf * 1000.0 / cpf, 2) if cpf and msf else "-")
        rows.append("| `%s` | %s | %s (%s-%s) | %s | %s | %s |%s %s |" % (
            disp(sym), cat, fmt(m_), fmt(lo), fmt(hi), fmt(msf, 2), fmt(med([x.get(sym, 0.0) for x in self_s])),
            fmt(med([x.get(sym, 0.0) for x in incl_s])), extra, text))
    w(head)
    w(rule)
    for line in rows[:a.top]:
        w(line)
    w("")
    if len(rows) > a.top and a.details:
        rel = os.path.relpath(os.path.abspath(a.details), os.path.dirname(os.path.abspath(a.into or a.out or "."))).replace(os.sep, "/")
        w("The full table, all %d functions up to %d %% of the on-CPU time, is in [%s](%s).\n" % (
            len(rows), int(a.cover * 100), os.path.basename(a.details), rel))
        os.makedirs(os.path.dirname(os.path.abspath(a.details)) or ".", exist_ok=True)
        with open(a.details, "w", encoding="utf-8", newline="\n") as f:
            f.write("# %s: every function to %d %% of the on-CPU time\n\n" % (a.title, int(a.cover * 100)))
            f.write("Generated by `scripts/cpu_report.py` with the scene's section of [the CPU profile](%s); the columns are "
                    "explained there.\n\n" % os.path.relpath(os.path.abspath(a.into or a.out or "."),
                                                            os.path.dirname(os.path.abspath(a.details))).replace(os.sep, "/"))
            f.write(head + "\n" + rule + "\n" + "\n".join(rows) + "\n")
    elif len(rows) > a.top:
        w("<details><summary>The other %d functions up to %d %% of the on-CPU time</summary>\n" % (len(rows) - a.top, int(a.cover * 100)))
        w(head)
        w(rule)
        for line in rows[a.top:]:
            w(line)
        w("\n</details>\n")
    w("These %d functions cover %s %% of the on-CPU time (the median of the runs; %s of %s %% of a core).\n" % (
        len(rows), fmt(100.0 * med([c_ / o_ if o_ else 1.0 for c_, o_ in zip(run_cov, run_oncpu)])), fmt(covered), fmt(oncpu)))
    if a.csv and undocumented:
        os.makedirs(a.csv, exist_ok=True)
        open(os.path.join(a.csv, "undocumented.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(undocumented) + "\n")
    if undocumented:
        w("Not yet described (add them to `scripts/cpu_functions.json`): %d, the hottest: %s\n" % (
            len(undocumented), ", ".join("`%s`" % x for x in undocumented[:20])))

    # (the table above has the self and inclusive columns; the two sorted lists are for a --details report only)
    for view, title in (("self_fn", "Functions by self time (the leaf: instructions executed there)"),
                        ("incl_fn", "Functions by inclusive time (in the stack at all)")) if a.details else ():
        keys = sorted({k for r in runs for k in r["c"][view]}, key=lambda k: (-med([share(r, view, k) for r in runs]), str(k)))
        w("## %s, top %d\n" % (title, a.top))
        w("| function | category | %% of a core | ms a frame | what it does |")
        w("|---|---|---|---|---|")
        for k, m_, lo, hi, msf in table(view, keys)[:a.top]:
            sym, cat = k
            d, text = doc_text(sym)
            w("| `%s` | %s | %s (%s-%s) | %s | %s |" % (disp(sym), cat, fmt(m_), fmt(lo), fmt(hi), fmt(msf, 2), text))
        w("")

    w("## Blocked: where threads wait (not CPU time)\n")
    w("| thread | waiting in | % of the thread's samples |")
    w("|---|---|---|")
    bl = collections.Counter()
    for r in runs:
        for (t, f), n in r["c"]["blocked_in"].items():
            allt = r["c"]["thread_all"][t] or 1
            bl[(t, f)] += 100.0 * n / allt / len(runs)
    for (t, f), pct in bl.most_common(a.top):
        w("| %s | `%s` | %s |" % (t, f, fmt(pct)))
    w("")

    if cdata["candidates"]:
        w("## What could be removed, merged or done more cheaply\n")
        w("Each row is priced from these runs: **cost** is the on-CPU time of every sample with the named work anywhere in "
          "its stack; **on the serial threads** is the part of it on the main thread and the Presentation Thread, where a "
          "saving is most likely to shorten the frame (work on the job workers can shorten it too when the main thread is "
          "waiting for that work, as the waits table shows; otherwise a saving frees CPU, which helps smaller CPUs); **gain** is the cost times "
          "the share the fix is expected to remove, always an estimate; **supporting measurement** names an earlier "
          "measurement behind the estimate, where there is one. Ranked by the gain's middle.\n")
        rows = []
        for x in cdata["candidates"]:
            pct = med([share(r, "cand", x["id"]) for r in runs]) or 0.0
            path = med([share(r, "cand_path", x["id"]) for r in runs]) or 0.0
            msf = med([ms(r, share(r, "cand", x["id"])) for r in runs]) or 0.0
            lo, hi = x["gain"]
            rows.append((msf * (lo + hi) / 2, x, pct, path, msf, lo, hi))
        rows.sort(key=lambda t: -t[0])
        glance["cands"] = rows
        w("| # | what | cost: % of a core (ms a frame) | on the serial threads, % of a core | fix | gain (estimate), ms a frame | risk | supporting measurement |")
        w("|---|---|---|---|---|---|---|---|")
        for i, (_, x, pct, path, msf, lo, hi) in enumerate(rows, 1):
            what_ = " ".join(y for y in re.split(r"(?<=[.;])\s+(?=[A-Z(])", x["what"]) if not MEASURE.search(y))
            what_ = re.sub(r"\s*\([^()]*\)", lambda m_: "" if MEASURE.search(m_.group(0)) else m_.group(0), what_)
            w("| %d | **%s**%s | %s (%s) | %s | %s | %s to %s | %s | %s |" % (
                i, x["title"], (": " + what_) if what_ else "", fmt(pct), fmt(msf, 2), fmt(path), x["fix"], fmt(msf * lo, 2), fmt(msf * hi, 2),
                x["risk"], x["evidence"]))
        w("")
        if cdata.get("game_own"):
            w("Large costs that are the game's own work (not Hookshot's to remove; for scale):\n")
            w("| what | % of a core | ms a frame | note |")
            w("|---|---|---|---|")
            for x in cdata["game_own"]:
                pcts = [sum(v for k, v in r["c"]["owner_cat"].items() if k.startswith(x["match"])) * 100.0 / r["inst"]
                        for r in runs]
                w("| %s | %s | %s | %s |" % (x["title"], fmt(med(pcts)), fmt(med([ms(r, p_) for r, p_ in zip(runs, pcts)]), 2),
                                            x["note"]))
            w("")

    unc = collections.Counter()
    for r in runs:
        unc.update(r["c"]["unclassified"])
    if unc:
        w("## Unclassified (add a rule to `scripts/cpu_categories.py`)\n")
        w("| module | object | symbol | samples |")
        w("|---|---|---|---|")
        for (mod, obj, sym), n in unc.most_common(30):
            w("| %s | %s | `%s` | %d |" % (mod, obj, sym, n))
        w("")

    g = []
    if glance.get("fps"):
        g.append("- **Speed:** %s fps and %s ms of CPU a frame (%s cores), median of %d runs without the sampler; %d frames over "
                 "100 ms in all of them." % (fmt(glance["fps"]), fmt(glance["cpu_ms"]), fmt(glance["cores"], 2), glance["n"],
                                             glance["over100"]))
    if glance.get("tops"):
        g.append("- **Where the CPU goes:** " + ", ".join("%s %s %%" % (k, fmt(v)) for k, v in glance["tops"][:5]) +
                 " of a core; the largest parts: " + ", ".join("%s %s %%" % (k, fmt(v)) for k, v in glance["subs"][:5]) + ".")
    for t, busy, waits in glance.get("serial", []):
        g.append("- **%s:** running %s %% of the time; it waits most in %s." % (
            t, fmt(busy), " and ".join("`%s` (%s %%)" % (f, fmt(p_)) for f, p_ in waits) or "-"))
    if glance.get("cands"):
        top3 = glance["cands"][:3]
        path = sorted(glance["cands"], key=lambda r_: -(r_[3] * (r_[5] + r_[6]) / 2))[:3]
        g.append("- **Largest estimated savings:** " + "; ".join("%s (%s to %s ms a frame)" % (r_[1]["title"], fmt(r_[4] * r_[5], 2),
                                                                                           fmt(r_[4] * r_[6], 2)) for r_ in top3) + ".")
        g.append("- **On the serial threads** (the main and Presentation threads, where a saving is most likely to shorten the frame): " +
                 "; ".join("%s (%s %% of a core there)" % (r_[1]["title"], fmt(r_[3])) for r_ in path) + ".")
    if g:
        out[glance_at:glance_at] = ["## At a glance\n"] + g + [""]
    text = "\n".join(out) + "\n"
    if a.into:
        if not a.section:
            raise SystemExit("--into needs --section")
        body = "\n".join(("#" + l) if l.startswith("#") else l for l in out) + "\n"   # (one level down inside the page)
        page = open(a.into, encoding="utf-8").read()
        start, end = "<!-- MEASURED:%s -->" % a.section, "<!-- END MEASURED:%s -->" % a.section
        if start not in page or end not in page:
            raise SystemExit("%s has no %s ... %s markers" % (a.into, start, end))
        i, j = page.index(start) + len(start), page.index(end)
        page = page[:i] + "\n" + body + page[j:]
        open(a.into, "w", encoding="utf-8", newline="\n").write(page)
        target = a.into
    else:
        if not a.out:
            raise SystemExit("--out or --into is needed")
        os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
        open(a.out, "w", encoding="utf-8", newline="\n").write(text)
        target = a.out
    if a.csv:
        os.makedirs(a.csv, exist_ok=True)
        for view in ("owner_cat", "self_cat", "owner_fn", "self_fn", "incl_fn", "thread_cpu"):
            with open(os.path.join(a.csv, view + ".csv"), "w", encoding="utf-8", newline="") as f:
                wr = csv.writer(f)
                wr.writerow(["key"] + ["%s_pct_core" % r["name"] for r in runs])
                keys = sorted({k for r in runs for k in r["c"][view]}, key=lambda k: (-med([share(r, view, k) for r in runs]), str(k)))
                for k in keys:
                    wr.writerow([k if isinstance(k, str) else "|".join(map(str, k))] + ["%.3f" % share(r, view, k) for r in runs])
    print("wrote %s (%d runs)" % (target, len(runs)))


if __name__ == "__main__":
    main()
