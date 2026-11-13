"""Stable chunk boundaries for the lifted C (lift.py --write).

a64-kit's write() closes a chunk once it reaches CHUNK_BYTES of text, so a function that grows (a jump table found,
a fix in one form) moves every boundary after it, and build.py recompiles every chunk from there on: a fix to two
functions cost a full 45-minute rebuild. Here the boundaries are guest addresses, kept in work/image/chunks.txt:
a chunk ends after the last function below the next boundary, so only the chunks holding changed functions differ.
Without the file (or with --rechunk) the boundaries come from the sizes, as before, and are saved.

write()'s test is `size >= chunk_bytes`. chunk_bytes is a Boundaries instance: an int whose reflected comparison
(__le__, which Python tries first for a subclass on the right) decides from the function just emitted.

The functions' C is made by several processes: each emits a run of consecutive functions, and write() takes the texts in
its own order, as if it had made them. Where there is fork (Linux, macOS) the workers inherit the lifter as it stands; where
there is none (Windows) they are new processes, and each is sent the bodies of its run and the few tables the emit reads
(spawn_emit). The lifter's counters (stats, unhandled, ijump, and any passed in `counters`, such as poll.stats) get each
process's additions. The output is the one-process output, byte for byte (tests/test_parallel_emit.py for the forked
workers, tests/test_spawn_emit.py for the new processes, over a real lifter).
"""
import bisect
import collections
import glob
import multiprocessing
import os
import re
from concurrent.futures import ProcessPoolExecutor
from concurrent.futures.process import BrokenProcessPool
import whole_write   # (rewrites written beside and moved over: issue 1197)

import a64_lift
import lifted_c

FILE = "chunks.txt"


class Boundaries(int):
    def __new__(cls, size, starts, funcs):
        self = super().__new__(cls, size)
        self.starts = sorted(starts)
        self.recorded = []   # (without saved boundaries) where the size rule started a chunk
        self.current = None
        self.after = None    # the function emitted after `current`
        order = sorted(funcs)
        self.ends = set()
        for b in self.starts:
            i = bisect.bisect_left(order, b)
            if 0 < i < len(order):
                self.ends.add(order[i - 1])
        self.next_of = {a: b for a, b in zip(order, order[1:])}
        return self

    def __le__(self, size):   # `size >= self`
        e = self.current
        if self.starts:
            return e in self.ends
        if size >= int(self):
            nxt = self.next_of.get(e)
            if nxt is not None:
                self.recorded.append(nxt)
            return True
        return False


def load(img_dir):
    path = os.path.join(img_dir, FILE)
    if not os.path.exists(path):
        return []
    return [int(l, 16) for l in open(path) if l.strip() and not l.startswith("#")]


def save(img_dir, starts):
    # (lift.py --standalone writes this under <work>/image while its image.bin goes to <work>/sa/image, and nothing made
    # that folder: a fresh work folder stopped the lift at its last step with FileNotFoundError. make_game.py prepares it)
    os.makedirs(img_dir, exist_ok=True)
    with open(os.path.join(img_dir, FILE), "w") as f:
        f.write("# first guest address of each lifted C chunk after the first (nx2/chunks.py)\n")
        f.write("".join("%08x\n" % s for s in sorted(starts)))


def seed_from_gen(gen_dir):
    """The boundaries of an existing size-chunked lift: the first function defined in each code_NNN.c after the
    first"""
    starts = []
    pat = re.compile(r"^void f_([0-9a-f]+)\(Ctx \*__restrict A64_NOESC c\) \{")
    n = 1
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):
        for line in open(os.path.join(gen_dir, "code_%03d.c" % n)):
            m = pat.match(line)
            if m:
                starts.append(int(m.group(1), 16))
                break
        n += 1
    return starts


_job = None   # (the forked workers' view: the emitter, the instruction texts and the counters, inherited)


def _counts(counters):
    return [collections.Counter(c) for c in counters]


def _emit_run(es):
    emit, texts, counters = _job
    before = _counts(counters)
    out = [(e,) + tuple(emit(e, texts)) for e in es]
    return out, [collections.Counter({k: v - b.get(k, 0) for k, v in c.items() if v != b.get(k, 0)})
                 for b, c in zip(before, counters)]


def _available_gb():
    """MemAvailable from /proc/meminfo in GB (Windows: the available physical memory; None where neither is known)"""
    if os.name == "nt":
        import ctypes

        class Status(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong)] + [(n, ctypes.c_ulonglong) for n in ("tpf", "apf", "tv", "av", "ae")]
        st = Status()
        st.length = ctypes.sizeof(st)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            return st.avail / (1 << 30)
        return None
    try:
        with open("/proc/meminfo", encoding="ascii") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / (1024 * 1024)
    except (OSError, ValueError, IndexError):
        pass
    return None


def _no_fork():
    """No fork() here (Windows), or HOOKSHOT_LIFT_NO_FORK=1 (to try the Windows path elsewhere)"""
    return os.environ.get("HOOKSHOT_LIFT_NO_FORK") == "1" or "fork" not in multiprocessing.get_all_start_methods()


def file_jobs():
    """Processes for the per-file passes (split_big, edit_c) and for the emit: HOOKSHOT_LIFT_JOBS, else the cores this process
    may use (at most 16; each holds its run's texts and the pages of the lift it touches), and at most one per 2 GB of available
    memory (on Windows too: its available physical memory): a Steam Deck (16 GB) ran out with 8, and the system's out-of-memory
    handling stopped the lift ("lift.py stopped (signal 15): BrokenPipeError") or one worker (the update then sat at "jump tables"
    for good). The per-file workers take a file's path and read the file themselves, so they start as new processes (spawn) where
    they cannot be forked: the three passes over the 1.5 GB of C took ~30 s in one process on Windows"""
    if os.environ.get("HOOKSHOT_LIFT_JOBS"):
        return max(1, int(os.environ["HOOKSHOT_LIFT_JOBS"]))
    try:
        n = len(os.sched_getaffinity(0))
    except AttributeError:
        n = os.cpu_count() or 1
    n = max(1, min(16, n))
    gb = _available_gb()
    if gb is not None:
        n = max(1, min(n, int(gb // 2)))
    return n


def emit_jobs():
    """Processes for the emit: the count of the per-file passes too (file_jobs has the rule)"""
    return file_jobs()


def parallel_emit(emit, order, texts, counters, jobs, run=2000):
    """(e, text, refs) for every function of order, in order, emitted by `jobs` forked processes. A worker that dies
    (stopped for memory) ends the pool with an error instead of a wait forever (multiprocessing.Pool's imap never
    returns then); the runs not done yet are emitted here, one process."""
    global _job
    if jobs <= 1 or len(order) < 2 * run:
        for e in order:
            yield (e,) + tuple(emit(e, texts))
        return
    import gc
    from concurrent.futures import ProcessPoolExecutor
    from concurrent.futures.process import BrokenProcessPool
    runs = [order[i:i + run] for i in range(0, len(order), run)]
    done = 0
    _job = (emit, texts, counters)
    gc.collect()
    gc.freeze()   # (the lift's objects out of the collector's sight: its passes would write to every page the children share)
    try:
        ctx = multiprocessing.get_context("fork")
        with ProcessPoolExecutor(max_workers=jobs, mp_context=ctx) as pool:
            for out, deltas in pool.map(_emit_run, runs):
                for c, d in zip(counters, deltas):
                    for k, v in d.items():
                        c[k] = c.get(k, 0) + v
                done += 1
                yield from out
    except BrokenProcessPool:
        print("emit: a worker process stopped (out of memory?): the last %d of %d runs are emitted in this process"
              % (len(runs) - done, len(runs)), flush=True)
    finally:
        _job = None
        gc.unfreeze()
    for es in runs[done:]:
        for e in es:
            yield (e,) + tuple(emit(e, texts))


# What the emit reads of the lifter besides the bodies of the functions it emits (a64_lift.Lifter.emit_function and the
# methods it calls): sent once to each new process
EMIT_TABLES = ("entries", "plt", "jt", "devirt", "after", "no_local", "setjmp_owner", "comments", "names")
_view = None   # (a new process's view: a lifter with the tables only, and its counters)


def poll_only(counters):
    """The counters passed to write() are nx2/poll.py's alone: the only one a new process can count into"""
    import poll
    return len(counters) == 1 and counters[0] is poll.stats


def _identities():
    """Where the callables the emit goes through come from, as (module, qualified name): a64emit.translate (nx2/insn.py's), the
    lifter's emit and the passes it calls (nx2/poll.py wraps emit_function). The new processes get these patches only by running
    nx2/lift.py's module level again, so each compares its own with the lift's before it emits"""
    import a64emit
    L = a64_lift.Lifter
    return [(f.__module__, f.__qualname__) for f in (a64emit.translate, a64_lift.op_text, L.emit_function, L.dead_flags,
                                                     L.dirty_regs, L.transfer, L.callee)]


def _plain(L):
    """L is a64_lift.Lifter itself, with no emit_function of its own: what a new process rebuilds from the tables"""
    return type(L) is a64_lift.Lifter and "emit_function" not in vars(L)


def _spawn_init(tables, polls, identities):
    global _view
    import poll
    lifter = object.__new__(a64_lift.Lifter)
    lifter.__dict__.update(tables)
    lifter.stats, lifter.unhandled, lifter.ijump = collections.Counter(), collections.Counter(), collections.Counter()
    counters = [lifter.stats, lifter.unhandled, lifter.ijump]
    if polls:
        poll.install()   # (as lift.py did in the lift's own process)
        counters.append(poll.stats)
    if _identities() != identities:
        raise RuntimeError("this process emits with %s, the lift's with %s" % (_identities(), identities))
    _view = (lifter, counters)


def _emit_bodies(funcs):
    """_emit_run for a new process: funcs is {entry: (body, labels)}, the run's functions, in order. It translates the
    instructions of the run itself, as a64_lift.Lifter.write does for all of them (one result per distinct word, per pc
    where it reads the pc)"""
    import a64emit
    lifter, counters = _view
    keys = {}
    for body, _ in funcs.values():
        for pc, (k, w) in body.items():
            if k[0] == "op":
                keys.setdefault((w, pc) if a64_lift.pc_relative(w) else w, (pc, w))
    order = list(keys)
    tx, fl = a64emit.translate([keys[x] for x in order])
    fpx = set()   # (the shapes go to a64_fpx.h from the lift's own process, which translates all)
    texts = dict(zip(order, zip((a64_lift.op_text(t, fpx) for t in tx), fl)))
    lifter.funcs = funcs
    before = _counts(counters)
    out = [(e,) + tuple(a64_lift.Lifter.emit_function(lifter, e, texts)) for e in funcs]
    return out, [collections.Counter({k: v - b.get(k, 0) for k, v in c.items() if v != b.get(k, 0)})
                 for b, c in zip(before, counters)]


def spawn_emit(L, base_emit, order, texts, counters, jobs, polls, run=2000):
    """parallel_emit where there is no fork: the workers are new processes, each sent the tables of EMIT_TABLES once and, with
    each run, the bodies of its functions. polls: nx2/poll.py is installed (its counter is the last of `counters`). A worker
    whose emit is not this process's (_identities) stops, and the runs are emitted here"""
    runs = [order[i:i + run] for i in range(0, len(order), run)]
    tables = {k: getattr(L, k) for k in EMIT_TABLES if hasattr(L, k)}
    done = 0
    try:
        with ProcessPoolExecutor(max_workers=jobs, mp_context=multiprocessing.get_context("spawn"),
                                 initializer=_spawn_init, initargs=(tables, polls, _identities())) as pool:
            for out, deltas in pool.map(_emit_bodies, ({e: L.funcs[e] for e in es} for es in runs)):
                for c, d in zip(counters, deltas):
                    for k, v in d.items():
                        c[k] = c.get(k, 0) + v
                done += 1
                yield from out
    except BrokenProcessPool:
        print("emit: a worker process stopped or could not start (out of memory, or its emit is not this process's): "
              "the last %d of %d runs are emitted in this process" % (len(runs) - done, len(runs)), flush=True)
    for es in runs[done:]:
        for e in es:
            yield (e,) + tuple(base_emit(e, texts))


def write(L, gen_dir, img_dir, rechunk=False, counters=(), jobs=None, took=None):
    """L.write(gen_dir) with stable boundaries; returns its files. counters: dicts the emit adds to besides the
    lifter's own (poll.stats); jobs: emit processes (emit_jobs()); took(step): told as each step ends (nx2/lift.py's times)"""
    took = took or (lambda what: None)
    starts = [] if rechunk else load(img_dir)
    if not starts and not rechunk:
        starts = seed_from_gen(gen_dir)
    b = Boundaries(a64_lift.CHUNK_BYTES, starts, L.funcs)
    plain = _plain(L)   # (before L.emit_function is set below)
    base_emit = L.emit_function
    jobs = emit_jobs() if jobs is None else jobs
    made = None
    procs = 1   # (what ran: the gate below may decline the processes)

    def emit_function(e, texts):
        nonlocal made, procs
        b.current = e
        if made is None:   # (the first call: write() has its texts now, and asks for the functions in sorted order)
            import poll
            every = [L.stats, L.unhandled, L.ijump] + list(counters)
            if _no_fork():
                # (new processes rebuild the lifter from the tables: only for a plain Lifter, with the polls in the workers
                # exactly where they are here, and their counter the one passed in)
                if jobs > 1 and len(L.funcs) >= 4000 and plain and poll._installed == bool(counters) \
                        and (not counters or poll_only(counters)):
                    made = spawn_emit(L, base_emit, sorted(L.funcs), texts, every, jobs, bool(counters))
                    procs = jobs
                else:
                    made = parallel_emit(base_emit, sorted(L.funcs), texts, every, 1)
            else:
                made = parallel_emit(base_emit, sorted(L.funcs), texts, every, jobs)
                procs = jobs if jobs > 1 and len(L.funcs) >= 4000 else 1
        got = next(made, None)
        if got is None or got[0] != e:
            raise SystemExit("chunks: write() asked for f_%08x, the emit made %s" % (e, "f_%08x" % got[0] if got else "nothing"))
        return got[1], got[2]
    L.emit_function = emit_function
    try:
        files = L.write(gen_dir, chunk_bytes=b)
    finally:
        del L.emit_function
        if made is not None:
            made.close()   # (the processes go now, not when the generator is collected)
    took("emit and write (%d process%s)" % (procs, "" if procs == 1 else "es"))
    # (the numbered chunks only: files has code_hot.c too, and counting it skipped the first stale chunk, linked twice)
    n = sum(1 for f in files if re.fullmatch(r"code_\d{3}\.c", os.path.basename(f)))
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):   # (fewer chunks than the last lift)
        os.remove(os.path.join(gen_dir, "code_%03d.c" % n))
        n += 1
    # (an earlier lift's hot pieces go now, with or without a new code_hot.c, before the patches and the page-mark groups read
    # code_*.c: split_hot.py makes this lift's after it. A lift with no code_hot.c (no hot list, A64_HOT_CHUNK unset, or none of the
    # listed functions in it): split_hot.py had renamed the old one code_hot.c.whole and left code_hot_*.c, which build.py compiled
    # again, the earlier lift's hot functions, each defined in a chunk too, linked twice. A lift with one: the old pieces, patched
    # already, and the new whole file each held the output-size places, and nx2/oot_patches.py's count of them failed the lift)
    for f in os.listdir(gen_dir):
        if f == "code_hot.c.whole" or HOT_PIECE.fullmatch(f):
            os.remove(os.path.join(gen_dir, f))
    files += split_big(gen_dir, files)
    took("big chunks split")
    save(img_dir, starts or b.recorded)
    print("chunks: %d, boundaries %s" % (len(files), "kept" if starts else "from sizes (saved)"))
    return files


HEAD = lifted_c.HEAD_NOTED
PART = re.compile(r"code_(\d{3})_(\d+)\.c")
HOT_PIECE = re.compile(r"code_hot_\d+\.c")   # (nx2/split_hot.py's pieces of code_hot.c)
# (a function's compile time grows with the square of its size: LLVM's passes over one function of tens of thousands of blocks. Fitted to
# a cold build of the 48 chunks at -O1 on a 24-thread desktop: seconds ~ 1.5 x MB + 4.4 x MB^2 per function, so a function of QUAD bytes
# costs twice its size. A 24 MB chunk of small functions took ~45 s; one of 20 MB holding a few 2 MB functions, 350 s)
QUAD = 350000
COSTS = "chunk_costs.txt"   # (in the gen folder: each file's estimated compile cost, for build.py's order)


def fn_cost(size):
    """A function's estimated compile cost, in bytes of plain code"""
    return size + size * size // QUAD


def text_cost(text):
    """The estimated compile cost of a C file of lifted functions"""
    marks = [m.start() for m in HEAD.finditer(text)] + [len(text)]
    return marks[0] + sum(fn_cost(b - a) for a, b in zip(marks, marks[1:]))


def write_costs(gen_dir, costs):
    """Adds or replaces files' costs in chunk_costs.txt; costs: {file name: cost}"""
    path = os.path.join(gen_dir, COSTS)
    known = {}
    if os.path.exists(path):
        for line in open(path):
            name, _, n = line.partition(" ")
            if n.strip().isdigit() and os.path.exists(os.path.join(gen_dir, name)):
                known[name] = int(n)
    known.update(costs)
    with open(path, "w", newline="\n") as f:
        f.write("".join("%s %d\n" % kv for kv in sorted(known.items())))


def _cut(text, budget):
    """A chunk's text cut at function starts into parts of about equal cost when its cost is past the budget:
    ([(part's text, its cost)], the total cost) with the chunk's own (first) part first, or (None, its cost) when it stays whole"""
    marks = [h.start() for h in HEAD.finditer(text)]
    ends = marks[1:] + [len(text)]
    cost = [fn_cost(b - a) for a, b in zip(marks, ends)]
    total = sum(cost)
    if total <= budget * 3 // 2 or len(marks) < 2:
        return None, marks[0] + total if marks else len(text)
    parts = -(-total // budget)
    cuts, run, k = [0], 0, 1   # (indexes into marks where a part starts)
    for i, c in enumerate(cost):
        if i and run >= total * k // parts:
            cuts.append(i)
            k = run * parts // total + 1   # (past a giant function: the next share, not a sliver after it)
        run += c
    if len(cuts) < 2:   # (no cut: a giant function last, past every share)
        return None, marks[0] + total
    bounds = [marks[i] for i in cuts] + [len(text)]
    preamble = text[:marks[0]]
    pieces = []
    for j in range(len(cuts)):
        piece = text[:bounds[1]] if j == 0 else preamble + text[bounds[j]:bounds[j + 1]]
        pieces.append((piece, len(preamble) + sum(cost[cuts[j]:cuts[j + 1] if j + 1 < len(cuts) else len(cost)])))
    return pieces, total


def _line(path, text, total, n):
    return "chunks: %s (%d MB, cost %d MB) in %d parts" % (os.path.basename(path), len(text) >> 20, total >> 20, n)


def _cut_already(gen_dir, path, nnn, text, budget):
    """(_split_one, a chunk under the budget) the chunk when it is the first part of a cut this lift made already: a worker that
    finished the cut and whose result was lost when another worker stopped (the pool breaks, _each runs the chunk again here).
    The chunk and its parts on disk joined and cut again must give those same files, so an earlier lift's parts beside a
    chunk that is small now are not taken: (the parts, {file: cost}, the line to print), or None"""
    marks = [h.start() for h in HEAD.finditer(text)]
    if not marks:
        return None
    preamble = text[:marks[0]]
    files, texts = [], []
    while os.path.exists(os.path.join(gen_dir, "code_%s_%d.c" % (nnn, len(files) + 1))):
        files.append(os.path.join(gen_dir, "code_%s_%d.c" % (nnn, len(files) + 1)))
        with open(files[-1], encoding="utf-8", newline="") as f:
            texts.append(f.read())
        if not texts[-1].startswith(preamble):
            return None
    if not files:
        return None
    whole = text + "".join(t[len(preamble):] for t in texts)
    pieces, total = _cut(whole, budget)
    if pieces is None or [p for p, _ in pieces] != [text] + texts:
        return None
    return files, {os.path.basename(f): c for f, (_, c) in zip([path] + files, pieces)}, _line(path, whole, total, len(pieces))


def _split_one(job):
    """(split_big, one chunk, in a process of its own where there are several) the chunk cut into parts when its cost is past the
    budget: (the parts made besides the chunk itself, {file: cost}, the line to print or None)"""
    gen_dir, path, budget = job
    m = re.fullmatch(r"code_(\d{3})\.c", os.path.basename(path))
    if not m:
        return [], {}, None
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    pieces, total = _cut(text, budget)
    if pieces is None:
        return _cut_already(gen_dir, path, m.group(1), text, budget) or ([], {os.path.basename(path): total}, None)
    made, costs = [], {}
    # (the chunk itself last: a worker stopped part way leaves it whole, so the run again in the lift's process cuts it again,
    # where a chunk already cut to its first part made no parts and split_big then removed the ones written)
    for j in reversed(range(len(pieces))):
        out = path if j == 0 else os.path.join(gen_dir, "code_%s_%d.c" % (m.group(1), j))
        whole_write.write_whole(out, pieces[j][0])
        costs[os.path.basename(out)] = pieces[j][1]
        if j:
            made.insert(0, out)
    return made, costs, _line(path, text, total, len(pieces))


def split_big(gen_dir, files, budget=None, jobs=None):
    """Chunks whose estimated compile time (fn_cost) is well past a plain chunk's, cut at function starts into parts of about equal
    cost: code_NNN.c, code_NNN_1.c ... A chunk of the giant functions took 350 s, alone at the end of the build, where a chunk of
    ordinary functions takes 45; the boundaries kept in chunks.txt also let chunks grow (44 MB beside 25). The first part keeps the
    name, so seed_from_gen and the boundaries are as before. Writes chunk_costs.txt for every chunk; returns the new parts' paths.
    Each chunk is read (1.5 GB in all) and cut in a process of its own (jobs: file_jobs()): 5 s on one core before"""
    budget = budget or a64_lift.CHUNK_BYTES
    jobs = file_jobs() if jobs is None else jobs
    made, costs = [], {}
    for parts, cost, line in _each(_split_one, [(gen_dir, path, budget) for path in files], jobs):
        made += parts
        costs.update(cost)
        if line:
            print(line)
    for f in os.listdir(gen_dir):   # (parts of an earlier lift that this one did not make)
        if PART.fullmatch(f) and os.path.join(gen_dir, f) not in made:
            os.remove(os.path.join(gen_dir, f))
    write_costs(gen_dir, costs)
    return made


def _edit_patches(job):
    """(edit_c, one file, in a process of its own) the game's patches (oot_patches)"""
    import oot_patches
    path, supported = job
    with open(path, encoding="utf-8", newline="") as f:
        s = f.read()
    out, counts, lines = oot_patches.patch_file(s, os.path.basename(path), supported)
    if out != s:
        whole_write.write_whole(path, out, newline="")
    return counts, lines


def _edit_marks(path):
    """(edit_c, one file, in a process of its own) the output-size edits, then the page marks of each group of stores
    (a64_lift.mark_groups, which changes the text they match)"""
    import oot_patches
    with open(path, encoding="utf-8", newline="") as f:
        s = f.read()
    out, found, lines = oot_patches.output_size_file(s, os.path.basename(path))
    st = collections.Counter()
    if not os.environ.get("A64_MARK_EACH"):   # (a64_lift.group_marks' switch: no groups)
        out = a64_lift.mark_groups(out, st)
    if out != s:
        whole_write.write_whole(path, out, newline="")
    return found, lines, st


def _each(fn, items, jobs):
    """fn over items, in order: forked processes (one file each at a time) where there are several, else here. A worker that
    dies (stopped for memory) breaks the pool: the items without a result are done here (a file is rewritten whole or not at
    all, whole_write, and each edit finds the places it made already, as on a second lift over the same files)"""
    if jobs <= 1 or len(items) < 2:
        return [fn(x) for x in items]
    import gc
    from concurrent.futures import ProcessPoolExecutor
    out = [None] * len(items)
    got = [False] * len(items)
    spawn = _no_fork()   # (new processes, which import this module again: Windows)
    gc.collect()
    gc.freeze()   # (as parallel_emit: the lift's objects out of the collector's sight, the pages shared with the children)
    try:
        with ProcessPoolExecutor(max_workers=min(jobs, len(items)),
                                 mp_context=multiprocessing.get_context("spawn" if spawn else "fork")) as pool:
            futures = [pool.submit(fn, x) for x in items]
            for i, f in enumerate(futures):
                try:
                    out[i], got[i] = f.result(), True
                except BrokenProcessPool:
                    pass
    except BrokenProcessPool:
        pass
    finally:
        gc.unfreeze()
    if not all(got):
        print("edits: a worker process stopped (out of memory?): %d of %d files are edited in this process" % (
            got.count(False), len(items)), flush=True)
        for i, x in enumerate(items):
            if not got[i]:
                out[i] = fn(x)
    return out


def _side_run(fn, conn):
    try:
        conn.send((True, fn()))
    except BaseException as e:   # (said in the lift's own process: end_side does it again there)
        conn.send((False, repr(e)))
    finally:
        conn.close()


def start_side(fn):
    """fn() started in a forked process, beside what the lift does next; None where there is no fork or one process is asked for
    (HOOKSHOT_LIFT_JOBS=1). end_side(side, fn) waits for it and gives its result"""
    if _no_fork() or emit_jobs() <= 1:
        return None
    ctx = multiprocessing.get_context("fork")
    mine, theirs = ctx.Pipe(duplex=False)
    p = ctx.Process(target=_side_run, args=(fn, theirs))
    p.start()
    theirs.close()
    return p, mine


def end_side(side, fn):
    """start_side's result; fn() here when it was not started, or its process failed or died (the files are written whole again)"""
    if side is None:
        return fn()
    p, conn = side
    try:
        ok, out = conn.recv()
    except EOFError:
        ok, out = False, "the process stopped"
    p.join()
    conn.close()
    if ok and p.exitcode == 0:
        return out
    print("side process failed (%s): done here" % out, flush=True)
    return fn()


def edit_c(gen, stats, supported, whole=True, took=None, jobs=None):
    """(nx2/lift.py, after the lift writes its C; here, beside the writing, so it imports without the lift's tools) The edits of the
    written C, in this order: the game's patches (nx2/oot_patches.py, which find the lift's exact text; the supported build's whole
    standalone lift fails when an output-size edit finds no place), then, last, the page marks of each group of stores shared
    (a64_lift.mark_groups), which changes that text. stats: the lift's, for its report.
    Each file's edits are its own; only the counts are over all of them. So the files go through two passes, each a process per
    file (jobs: file_jobs(); new processes where there is no fork): the patches; then, after the counts' rules (oot_patches.report: the step
    divides both or neither), the output size and the groups. Five passes over the 1.5 GB of C on one core took a minute. The
    files and the lines printed are the one-process ones, byte for byte (tests/test_mark_groups.py)"""
    import oot_patches
    took = took or (lambda what: None)
    jobs = file_jobs() if jobs is None else jobs
    paths = sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c")))
    done = _each(_edit_patches, [(p, supported) for p in paths], jobs)
    for _, lines in done:
        if lines:
            print("\n".join(lines))
    oot_patches.report(paths, [counts for counts, _ in done], supported)
    took("patches (%d processes)" % jobs)
    found = [0] * len(oot_patches.OUT_EDITS)
    for here, lines, st in _each(_edit_marks, paths, jobs):
        found = [a + b for a, b in zip(found, here)]
        if lines:
            print("\n".join(lines))
        for k, v in st.items():
            stats[k] = stats.get(k, 0) + v
    oot_patches.check_output_size(found, required=supported and whole)
    took("output size and page-mark groups")
