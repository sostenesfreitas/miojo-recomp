"""The frame's critical path from a HOS_CHAIN recording (host/hos_chain.c): each frame walked back from its present.

    python -I scripts/chain.py <chain file> [<HOS_SAMPLE_ALL file of the same run>] [top]

From a frame's present, back to the present before it: the presenting thread ran until then; before that it was
blocked in a wait (or asleep); the signal that ended the wait came from another thread, which ran until it sent it;
and so on. Each step's time goes to one of: a thread running (its own work, the replay on the submitting thread, a
driver call: anything that is not a wait), a wake-up (from the signal to the waiter running again), a sleep, or a
wait whose ending signal was not recorded (a timeout, or an object this layer does not see). The sum is the frame.

Prints, a frame on average: the path's time by thread and kind; the hand-offs on it (waiter and where it waited, who
signaled and from where); with samples (HOS_SAMPLE_ALL, same run: its "# t0_ns" line puts them on this clock;
RX_MAPS=<game folder>/bin names the frames as for scripts/crit.py), the functions running on the path.
"""
import bisect, collections, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

WAIT, WOKE, SIGNAL, SLEEP, AWAKE, PRESENT = 1, 2, 3, 4, 5, 6
REC = struct.Struct("<qQQHB5x")


def load(path):
    names = {}
    with open(path, "rb") as f:
        if f.readline().strip() != b"HOSCHAIN1":
            sys.exit("%s: not a HOS_CHAIN recording" % path)
        while True:
            line = f.readline().decode("utf-8", "replace").split()
            if line[0] == "thread":
                names[int(line[1])] = line[2]
            elif line[0] == "records":
                n = int(line[1])
                break
        data = f.read(n * REC.size)
    recs = [REC.unpack_from(data, i * REC.size) for i in range(len(data) // REC.size)]
    recs.sort(key=lambda r: r[0])
    return names, recs


VCORE = 1 << 63   # (keys with this bit: a thread's wait for a virtual core, host/hos_thread.c vc_acquire)


def build(recs):
    """per thread: blocked intervals (sorted by end, none overlapping) as (end, begin, kind, key at the end, lr at the
    begin, whether it is a piece after a block inside it); per key: signals (time, thread, lr), sorted; presents by thread. A block inside another (a wait for a
    virtual core when a wait's sleep ends, or a sleep's) splits it: the outer wait up to the inner one's start, the
    inner one, and the outer again after it"""
    open_ = collections.defaultdict(list)
    blocks = collections.defaultdict(list)
    signals = collections.defaultdict(list)
    presents = collections.defaultdict(list)

    def kind_of(k, key):
        return "sleep" if k == SLEEP else "virtual core" if key & VCORE else "wait"

    for t, key, lr, th, kind in recs:
        if kind in (WAIT, SLEEP):
            open_[th].append([t, kind, key, lr, []])
        elif kind in (WOKE, AWAKE):
            if not open_[th]:
                continue
            b, k, _, blr, inner = open_[th].pop()
            kd = kind_of(k, key)
            at = b
            for ib, ie in inner:   # (the outer block's pieces around those inside it)
                if ib > at:
                    blocks[th].append((ib, at, kd, key, blr, at != b))
                at = ie
            if t > at:
                blocks[th].append((t, at, kd, key, blr, at != b))
            if open_[th]:
                open_[th][-1][4].append((b, t))
        elif kind == SIGNAL:
            signals[key].append((t, th, lr))
        elif kind == PRESENT:
            presents[th].append(t)
    for th in blocks:
        blocks[th].sort()
    blocks = Blocks(blocks)
    return blocks, signals, presents


class Blocks(dict):
    def __init__(self, d):
        super().__init__(d)
        self.ends = {k: [b[0] for b in v] for k, v in d.items()}


def walk(th, t, bound, blocks, signals, out):
    """the path back from (thread th, time t) to bound; out: callbacks"""
    ends = blocks.ends
    # (a stretch: one thread's part of the path, from the wake-up that started it to the signal that handed the path on)
    end_label, end_t = "the present", t
    for _ in range(100000):
        if t <= bound:
            out.stretch(th, "the frame before's present", end_label, end_t - bound)
            return
        e_list = ends.get(th, [])
        i = bisect.bisect_right(e_list, t) - 1
        while i >= 0 and blocks[th][i][1] >= t:   # (a block that begins after t: not before it)
            i -= 1
        if i < 0 or e_list[i] <= bound:
            out.run(th, bound, t)
            out.stretch(th, "the frame before's present", end_label, end_t - bound)
            return
        e, b, kind, key, blr, after_inner = blocks[th][i]
        out.run(th, e, t)
        t = e
        if kind == "sleep":
            out.other(th, "sleep", max(b, bound), e, blr)
            t = b
            continue
        if kind == "virtual core":
            # (contention, not a dependency: the holder's work before it gave the core up was not for this thread. The
            # time goes to this thread, waiting for a core, with the thread that gave it one; the path goes on here)
            sl = signals.get(key, [])
            j = bisect.bisect_right(sl, (e, 1 << 30, 0)) - 1
            out.core(th, blr, sl[j][1] if j >= 0 and sl[j][0] >= b else None, max(b, bound), e)
            t = b
            continue
        sl = signals.get(key, [])
        j = bisect.bisect_right(sl, (e, 1 << 30, 0)) - 1
        while j >= 0 and sl[j][1] == th:
            j -= 1
        if j >= 0 and sl[j][0] >= b:
            ts, sth, slr = sl[j]
            out.wake(th, blr, sth, slr, max(ts, bound), e, kind)
            out.stretch(th, "woken at lr %#x" % blr, end_label, end_t - e)
            end_label, end_t = "signal at lr %#x" % slr, ts
            th, t = sth, ts
        elif after_inner:   # (after a wait for a core inside it, no signal: the wait's own end, running)
            out.run(th, max(b, bound), e)
            t = b
        else:
            out.other(th, kind + ", no signal seen", max(b, bound), e, blr)
            t = b


class Acc:
    def __init__(self):
        self.by = collections.Counter()      # (thread, kind) -> ns
        self.hand = collections.Counter()    # (waiter, wait lr, signaler, signal lr) -> ns of wake-up
        self.handn = collections.Counter()
        self.waitat = collections.Counter()  # (thread, kind, lr) -> ns
        self.segs = []                       # running segments (thread, start, end)
        self.cores = collections.Counter()   # (waiter, the thread that gave it a core or None) -> ns
        self.stretches = collections.Counter()   # (thread, what started it, what ended it) -> ns
        self.nstretch = collections.Counter()

    def run(self, th, a, b):
        if b > a:
            self.by[(th, "running")] += b - a
            self.segs.append((th, a, b))

    def other(self, th, kind, a, b, lr):
        if b > a:
            self.by[(th, kind)] += b - a
            self.waitat[(th, kind, lr)] += b - a

    def wake(self, th, wlr, sth, slr, a, b, kind):
        self.by[(th, "signal to running")] += max(0, b - a)
        self.hand[(th, wlr, sth, slr)] += max(0, b - a)
        self.handn[(th, wlr, sth, slr)] += 1

    def stretch(self, th, start, end, ns):
        self.stretches[(th, start, end)] += max(0, ns)
        self.nstretch[(th, start, end)] += 1

    def core(self, th, lr, granter, a, b):
        if b > a:
            self.by[(th, "waiting for a virtual core")] += b - a
            self.cores[(th, granter)] += b - a


def samples_on_path(path, names, segs, top):
    import prof
    maps = prof.load_symbols()
    cache = {}

    def name(frame):
        if frame not in cache:
            mod, _, off = frame.rpartition("+")
            r = mod
            if mod in maps:
                addrs, nm = maps[mod]
                i = bisect.bisect_right(addrs, int(off, 16)) - 1
                if i >= 0:
                    r = prof.module_name(mod) + ":" + prof.demangle(nm[i])
            cache[frame] = r
        return cache[frame]

    t0 = at = None
    by_thread = collections.defaultdict(list)   # sample thread name -> [(ns, frames)]
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("# t0_ns "):
            if t0 is None:
                t0 = int(line.split()[2])
            continue
        if line.startswith("# at "):   # (the next sample's own time: a HOS_CHAIN run's samples have it)
            at = int(line.split()[2])
            continue
        head, _, stack = line.rstrip("\n").partition("|")
        ms, _, th = head.partition(" ")
        if stack and ms.isdigit() and t0 is not None:
            by_thread[th].append((at if at is not None else t0 + int(ms) * 1000000, stack.split(";")))
        at = None
    if t0 is None:
        print("(%s has no \"# t0_ns\" line: a sample file from before HOS_CHAIN)" % path)
        return

    shared = collections.Counter(names.values())

    def match(chain_name):   # (only a name one thread has in both: samples carry names, not thread ids)
        if shared[chain_name] > 1:
            return None
        return chain_name if chain_name in by_thread else None

    for v in by_thread.values():
        v.sort(key=lambda s: s[0])
    seg_by = collections.defaultdict(list)
    for th, a, b in segs:
        seg_by[th].append((a, b))
    inner = collections.Counter()
    leaf = collections.Counter()
    state = collections.Counter()   # (thread, working or waiting below the game)
    held = collections.Counter()    # (thread, the runtime or renderer function it waits in)
    n = 0
    for th, ss in seg_by.items():
        sn = match(names.get(th, "?"))
        if not sn:
            continue
        smp = by_thread[sn]
        times = [s[0] for s in smp]
        for a, b in ss:
            for k in range(bisect.bisect_left(times, a), bisect.bisect_right(times, b)):
                frames = [name(f) for f in smp[k][1]]
                fn = next((f for f in frames if ":f_" in f or f.startswith("hookshot_vk:")), frames[0])
                inner["%s  %s" % (sn, fn)] += 1
                leaf["%s  %s" % (sn, frames[0])] += 1
                cpu = not prof.waiting(smp[k][1][0].rpartition("+")[0], frames)
                state[(sn, cpu)] += 1
                if not cpu:
                    held["%s  %s" % (sn, next((f for f in frames if f.startswith(("hookshot:", "hookshot_vk:"))), frames[0]))] += 1
                n += 1
    print("-- samples on the path: %d" % n)
    if not n:
        return
    print("  by thread: working (the game, the renderer, libc's copies, the driver's own work) / waiting below the game (a")
    print("  host wait the path does not see: a fence, the present, a renderer lock; prof.WAIT_FRAMES)")
    for th in sorted({k[0] for k in state}, key=lambda t: -(state[(t, True)] + state[(t, False)]))[:top]:
        a, b = state[(th, True)], state[(th, False)]
        print("    %5.1f%%  %-28s %5.1f%% working, %5.1f%% waiting" % (100.0 * (a + b) / n, th, 100.0 * a / (a + b),
                                                                     100.0 * b / (a + b)))
    print("  waiting in (the innermost runtime or renderer frame):")
    for k, c in held.most_common(top):
        print("    %5.1f%%  %s" % (100.0 * c / n, k))
    print("  innermost game or renderer function:")
    for k, c in inner.most_common(top):
        print("    %5.1f%%  %s" % (100.0 * c / n, k))
    print("  leaf:")
    for k, c in leaf.most_common(top):
        print("    %5.1f%%  %s" % (100.0 * c / n, k))


def main():
    path = sys.argv[1]
    spath = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].isdigit() else None
    top = int(sys.argv[-1]) if sys.argv[-1].isdigit() else 20
    names, recs = load(path)
    blocks, signals, presents = build(recs)
    if not presents:
        sys.exit("no presents recorded")
    pth = max(presents, key=lambda k: len(presents[k]))
    ps = presents[pth]
    acc = Acc()
    frames = 0
    for k in range(1, len(ps)):
        walk(pth, ps[k], ps[k - 1], blocks, signals, acc)
        frames += 1
    span = ps[-1] - ps[0]
    counts = collections.Counter(names.values())
    nm = lambda th: names.get(th, str(th)) + ("#%d" % th if counts[names.get(th)] > 1 else "")
    print("%d records, %d threads; %d frames presented by %s, %.2f ms a frame (%.1f fps)" % (
        len(recs), len(names), frames, nm(pth), span / 1e6 / max(1, frames), 1e9 * frames / max(1, span)))
    tot = sum(acc.by.values())
    print("-- the path, a frame (ms): %.2f in all" % (tot / 1e6 / frames))
    kinds = collections.Counter()
    for (th, kind), ns in acc.by.items():
        kinds[kind] += ns
    for kind, ns in kinds.most_common():
        print("  %7.3f  %s" % (ns / 1e6 / frames, kind))
    print("-- the path's stretches: a thread, from the wake-up that put it on the path to the signal that handed it on (ms a")
    print("   frame; each also holds its waits for a core and its sleeps)")
    for (th, a, b), ns in acc.stretches.most_common(top):
        print("  %7.3f  %5.2f x  %-24s from %s to %s" % (ns / 1e6 / frames, acc.nstretch[(th, a, b)] / frames, nm(th), a, b))
    print("-- by thread and kind (ms a frame)")
    for (th, kind), ns in acc.by.most_common(top):
        print("  %7.3f  %-28s %s" % (ns / 1e6 / frames, nm(th), kind))
    print("-- hand-offs on the path: waiter (where it waited) <- signaler (where it signaled): a frame, wake-up ms")
    for key, ns in sorted(acc.handn.items(), key=lambda kv: -kv[1])[:top]:
        th, wlr, sth, slr = key
        print("  %5.2f x %6.3f ms  %s (lr %#x) <- %s (lr %#x)" % (acc.handn[key] / frames, acc.hand[key] / 1e6 / frames,
                                                              nm(th), wlr, nm(sth), slr))
    print("-- waits for a virtual core on the path (ms a frame): waiter <- the thread that gave the core up")
    for (th, g), ns in acc.cores.most_common(top):
        print("  %7.3f  %s <- %s" % (ns / 1e6 / frames, nm(th), nm(g) if g is not None else "(a core found free)"))
    print("-- sleeps and unexplained blocks on the path (ms a frame)")
    for (th, kind, lr), ns in acc.waitat.most_common(top):
        print("  %7.3f  %-28s %s (lr %#x)" % (ns / 1e6 / frames, nm(th), kind, lr))
    if spath:
        samples_on_path(spath, names, acc.segs, top)


if __name__ == "__main__":
    main()
