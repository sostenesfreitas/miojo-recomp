"""The frame's critical path from an all-thread sample (host/mkw_glue.c HOS_SAMPLE_ALL): every 2 ms each guest thread
and renderer thread, "<ms> <name>|<frames>" (module+offset, leaf first).

    python -I scripts/crit.py work/runs/<run>_all.txt [top]

A thread is blocked at an instant when its leaf is outside hookshot.exe / hookshot_vk.dll (on Linux hookshot /
hookshot_vk.so) and the innermost frame of Hookshot's under it is a wait (prof.WAIT_FRAMES: a condition variable, a
sleep, a fence, the present); running otherwise, libc's copies and the driver's own work included. RX_MAPS=<folder>: another build's linker maps, or on Linux the game folder (its binaries' symbols). Prints each thread's running share;
then for the instants when no game thread runs (the frame waits on something outside the game's CPU work): what each
thread waits in and what the renderer's threads do; then for the instants when the main thread is blocked: how many
game threads run and the functions they are in.
"""
import collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prof  # noqa: E402

RUN_MODS = prof.RUN_MODS


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    maps = prof.load_symbols()
    cache = {}

    def name(frame):
        if frame in cache:
            return cache[frame]
        mod, _, off = frame.rpartition("+")
        r = mod
        if mod in maps:
            addrs, names = maps[mod]
            import bisect
            i = bisect.bisect_right(addrs, int(off, 16)) - 1
            if i >= 0:
                r = prof.module_name(mod) + ":" + prof.demangle(names[i])
        cache[frame] = r
        return r

    ticks = collections.defaultdict(dict)   # ms -> thread -> (running, frames named)
    for line in open(path, encoding="utf-8", errors="replace"):
        head, _, stack = line.rstrip("\n").partition("|")
        ms, _, thread = head.partition(" ")
        if not stack or not ms.isdigit():
            continue
        frames = stack.split(";")
        leafmod = frames[0].rpartition("+")[0]
        named = [name(f) for f in frames]
        ticks[int(ms)][thread] = (not prof.waiting(leafmod, named), named)
    n = len(ticks)
    print("%d instants" % n)
    running = collections.Counter()
    seen = collections.Counter()
    for t in ticks.values():
        for th, (r, _) in t.items():
            seen[th] += 1
            running[th] += r
    print("-- running share by thread")
    for th, c in sorted(running.items(), key=lambda kv: -kv[1])[:30]:
        print("  %5.1f%%  %s" % (100.0 * c / seen[th], th))

    def is_game(th):
        return not th.startswith("mkw_vk")

    def guest_frame(frames):
        """the innermost game function (f_...) and the import it called, if any"""
        imp = next((f for f in frames if ":imp_" in f), None)
        fn = next((f for f in frames if ":f_" in f), None)
        return (imp.split(":imp_")[1][:48] + " <- " if imp else "") + (fn or frames[-1])

    idle = [t for t in ticks.values() if not any(r for th, (r, _) in t.items() if is_game(th))]
    print("-- no game thread running: %.1f%% of instants" % (100.0 * len(idle) / max(1, n)))
    waits = collections.Counter()
    vk = collections.Counter()
    for t in idle:
        for th, (r, frames) in t.items():
            if th in ("sead::MainThread", "Presentation_Thread") or th.startswith("BaseProcExecutor0"):
                waits["%s: %s" % (th, guest_frame(frames))] += 1
            if not is_game(th):
                vk["%s %s: %s" % (th, "runs" if r else "waits", frames[0] if not r else
                                  next((f for f in frames if f.startswith("hookshot_vk:")), frames[0]))] += 1
    for k, c in waits.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / max(1, len(idle)), k))
    print("  (renderer threads then)")
    for k, c in vk.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / max(1, len(idle)), k))

    mw = [t for t in ticks.values() if "sead::MainThread" in t and not t["sead::MainThread"][0]]
    print("-- main thread blocked: %.1f%% of instants" % (100.0 * len(mw) / max(1, n)))
    hist = collections.Counter(sum(1 for th, (r, _) in t.items() if r and is_game(th)) for t in mw)
    print("  game threads running then: " + ", ".join("%d: %.0f%%" % (k, 100.0 * v / len(mw)) for k, v in sorted(hist.items())))
    mwhere = collections.Counter(guest_frame(t["sead::MainThread"][1]) for t in mw)
    print("  main waits in:")
    for k, c in mwhere.most_common(8):
        print("    %5.1f%%  %s" % (100.0 * c / len(mw), k))
    fns = collections.Counter()
    for t in mw:
        for th, (r, frames) in t.items():
            if r and is_game(th):
                fns[next((f for f in frames if ":f_" in f), frames[0])] += 1
    print("  game functions running then (innermost f_, share of those instants):")
    for k, c in fns.most_common(top):
        print("    %5.1f%%  %s" % (100.0 * c / len(mw), k))
    leaves = collections.Counter()
    for t in mw:
        for th, (r, frames) in t.items():
            if r and is_game(th):
                leaves[frames[0]] += 1
    print("  leaves running then:")
    for k, c in leaves.most_common(top):
        print("    %5.1f%%  %s" % (100.0 * c / len(mw), k))


if __name__ == "__main__":
    main()
