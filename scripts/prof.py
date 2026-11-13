"""Offline profile of a thread sampled with HOS_SAMPLE + HOS_SAMPLE_FILE (host/mkw_glue.c, mkw-recomp work/prof.py): each line is a stack of
module+offset frames, leaf first. Frames in hookshot_vk.dll / hookshot.exe are named from the linker maps beside them;
on Linux, frames in hookshot_vk.so / hookshot from the binaries' own symbols (nm). RX_MAPS=<folder>: another build's
maps or binaries (on Linux, the game folder).

    python -I scripts/prof.py work/runs/<run>/stacks.txt [top] [skip] [only]
    (skip: the first fraction of the samples, e.g. 0.5; only: samples with that function in their stack, e.g.
    bench_replay)

Prints the functions by inclusive share (in the stack at all) and by self share (the leaf), and the leaf modules.
"""
import bisect, collections, itertools, os, re, stat, subprocess, sys
from contextlib import ExitStack
from tempfile import TemporaryFile

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work", "sa", "build")


def load_map(path, base):
    syms = []
    pat = re.compile(r"^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s", re.I)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if m:
                syms.append((int(m.group(2), 16) - base, m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


# The modules whose frames are Hookshot's own code (the game and the runtime, the renderer): Windows, then Linux
GAME_MODS = ("hookshot.exe", "hookshot")
VK_MODS = ("hookshot_vk.dll", "hookshot_vk.so")
RUN_MODS = GAME_MODS + VK_MODS
# A sample whose leaf is outside Hookshot is a wait only when a wait is on its stack: libc's memcpy and the driver's own
# vkCmd* work are CPU time of the thread. These names (as the samples are named: Hookshot's frames, leaf first) wait.
WAIT_FRAMES = ("SleepConditionVariableSRW", "WaitForSingleObject", "hos_cv_sleep", "condition_variable::wait",
               "hos_sleep_ns", "sleep_ns", "hos_thread_wait", "vc_acquire", "present_out", "vkWaitForFences",
               "vkWaitSemaphores", "wait_value", "vk_timeline_wait", "recording_wait", ":Sleep", ":SleepEx",
               "WaitForMultipleObjects", "WaitOnAddress")


def waiting(leaf_module, names):
    """whether a sample (its leaf's module, its frames named) waits: outside Hookshot, under a wait"""
    if leaf_module in RUN_MODS:
        return False
    for n in names:
        if n.startswith(("hookshot:", "hookshot_vk:", "hookshot.exe:", "hookshot_vk.dll:")):
            return n.endswith(WAIT_FRAMES)
    return True   # (no frame of Hookshot's at all: a thread blocked outside it)


def load_elf(path):
    """An ELF's function symbols by their offset in the file's address space, which is the offset from the load address
    that a sample of a position-independent executable or a shared object gives (binutils' nm, demangled)"""
    out = subprocess.run(["nm", "--defined-only", "-C", path], capture_output=True, text=True, encoding="utf-8",
                         errors="replace").stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in ("t", "T", "w", "W") and len(parts[0]) >= 8:
            # Parentheses also occur in anonymous namespaces and operator names.
            # Keep the complete demangled symbol so distinct hot functions and
            # overloads do not collapse into one profile entry.
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def load_symbols(folder=None):
    """{module: (addresses, names)} for the modules found in folder (RX_MAPS, else the build): the linker maps of the
    Windows build, the binaries of the Linux one"""
    folder = folder or os.environ.get("RX_MAPS", BUILD)
    maps = {}
    for mod in ("hookshot_vk.dll", "hookshot.exe"):
        mp = os.path.join(folder, mod[:-4] + ".map")
        if os.path.exists(mp):
            maps[mod] = load_map(mp, 0x180000000 if mod.endswith(".dll") else 0x140000000)
    for mod in ("hookshot_vk.so", "hookshot"):
        b = os.path.join(folder, mod)
        if os.path.isfile(b):
            try:
                maps[mod] = load_elf(b)
            except OSError:   # (no nm: frames stay module+offset)
                pass
    return maps


def module_name(mod):
    """hookshot_vk / hookshot, whichever system the sample came from"""
    return mod.split(".")[0]


def demangle(n):
    # ?name@ns@@... -> ns::name (enough to read)
    m = re.match(r"\?([^@]+)@([^@]*)@", n)
    if m and not n.startswith("??"):
        return "%s::%s" % (m.group(2), m.group(1)) if m.group(2) else m.group(1)
    m = re.match(r"\?\?\$?([^@]+)@", n)
    return m.group(1) if m else n


def profile_lines(path, skip):
    """Keep the old line-based slice without holding the capture in memory."""
    with open(path, encoding="utf-8", errors="replace") as f, ExitStack() as files:
        start = 0
        if skip:
            # Windows can report a pipe as seekable, even though it cannot be replayed.
            if stat.S_ISREG(os.fstat(f.fileno()).st_mode) and f.seekable():
                count = sum(1 for _ in f)
            else:
                # A pipe can only be read once. Save its lines on disk to keep memory bounded.
                spool = files.enter_context(TemporaryFile(mode="w+t", encoding="utf-8", newline="\n"))
                count = 0
                for count, line in enumerate(f, 1):
                    spool.write(line)
                f = spool
            start = int(count * skip)
            if start < 0:
                start = max(0, count + start)
            else:
                start = min(start, count)
            f.seek(0)
        yield from itertools.islice(f, start, None)


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    only = sys.argv[4] if len(sys.argv) > 4 else None
    maps = load_symbols()

    def name(frame):
        mod, _, off = frame.rpartition("+")
        if mod in maps:
            addrs, names = maps[mod]
            i = bisect.bisect_right(addrs, int(off, 16)) - 1
            if i >= 0:
                return module_name(mod) + ":" + demangle(names[i])
        return mod

    incl, self_, leafmod = collections.Counter(), collections.Counter(), collections.Counter()
    n = 0
    for line in profile_lines(path, skip):
        frames = line.strip().split(";")
        if not frames or not frames[0]:
            continue
        named = [name(f) for f in frames]
        if only and not any(only in f for f in named):
            continue
        n += 1
        self_[named[0]] += 1
        leafmod[frames[0].rpartition("+")[0]] += 1
        for f in set(named):
            incl[f] += 1
    print("%d samples" % n)
    print("-- leaf modules")
    for k, v in leafmod.most_common(10):
        print("%6.1f%%  %s" % (100.0 * v / n, k))
    print("-- inclusive")
    for k, v in incl.most_common(top):
        print("%6.1f%%  %s" % (100.0 * v / n, k))
    print("-- self")
    for k, v in self_.most_common(top):
        print("%6.1f%%  %s" % (100.0 * v / n, k))


if __name__ == "__main__":
    main()
