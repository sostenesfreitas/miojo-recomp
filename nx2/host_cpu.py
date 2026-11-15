"""The x86-64 level this PC's CPU runs (nx2/build.py's -march; issue 363). The game is built on the PC it runs on, so a
CPU without x86-64-v3's features (AVX2, FMA, BMI2, MOVBE) gets x86-64-v2, or the x86-64 baseline. RX_MARCH, read by
build.py, still chooses."""
import os


def host_march():
    want = ("avx2", "fma", "bmi2", "movbe")
    try:
        if os.name == "nt":
            import ctypes
            pf = ctypes.windll.kernel32.IsProcessorFeaturePresent
            # (PF_AVX2_INSTRUCTIONS_AVAILABLE 40; PF_SSE4_2_INSTRUCTIONS_AVAILABLE 38. Windows reports no FMA/BMI2 bit, but
            # every AVX2 CPU has them, as x86-64-v3 counts them together)
            if pf(40):
                return "x86-64-v3"
            return "x86-64-v2" if pf(38) else "x86-64"
        with open("/proc/cpuinfo") as f:
            flags = f.read().split("flags", 1)[1].split(chr(10), 1)[0].split()
        if all(f in flags for f in want):
            return "x86-64-v3"
        return "x86-64-v2" if all(f in flags for f in ("sse4_2", "popcnt", "ssse3")) else "x86-64"
    except Exception:
        return "x86-64-v3"


def native_cpu(cc, flags):
    """What clang makes of -march=native here, from `clang -###`: its -target-cpu, -tune-cpu and
    -target-feature list, as "<cpu>_<tune>_<sha1 of the features>" (e.g. "znver4_znver4_3f1c09a2b4"). The flag itself names
    no CPU, so an object key made from it would stay the same when the CPU changes (a new CPU, the disk in another PC);
    build.py puts this name in the flags instead. None if clang doesn't say."""
    import hashlib
    import subprocess
    try:
        quiet = {"creationflags": 0x08000000} if os.name == "nt" else {}   # (CREATE_NO_WINDOW: no console from the launcher)
        out = subprocess.run([cc] + flags + ["-###", "-x", "c", "-c", os.devnull, "-o", os.devnull],
                             capture_output=True, text=True, timeout=60, **quiet).stderr
    except Exception:
        return None
    words = [w.strip('"') for w in out.split()]
    cpu = tune = ""
    feats = []
    for i, w in enumerate(words[:-1]):
        if w == "-target-cpu":
            cpu = words[i + 1]
        elif w == "-tune-cpu":
            tune = words[i + 1]
        elif w == "-target-feature":
            feats.append(words[i + 1])
    if not cpu or cpu == "native":
        return None
    feats = hashlib.sha1(" ".join(sorted(feats)).encode(), usedforsecurity=False).hexdigest()[:10]
    return "%s_%s_%s" % (cpu, tune or cpu, feats)
