"""The spin before a wait sleeps is long on the frame's serial threads and short on the others.

host/hos_thread.c's hos_spin_ns gives the calling thread's spin (nn_os_sync.c's events, queues and mutexes, hos_svc.c's
mutexes spin that long before they sleep): HOS_SPIN_US (20 us) on the frame's serial threads (the game thread, the
Presentation Thread: hos_thread_serial, from the name the game gives them), HOS_SPIN_OTHER_US (5 us, never more than
HOS_SPIN_US) on the others where the process has more than 6 processors. With 6 or fewer, or not known, every thread
keeps HOS_SPIN_US unless HOS_SPIN_OTHER_US is set. HOS_SPIN_US=0: no spin; HOS_SPIN_OTHER_US=20: one spin for all, as
before.

The fixture runs the real code cut from the source (the serial names, hos_spin_ns) over a stub thread and a stub
processor count (process_threads), one process a case (the values are read once).

python -I -B tests/test_spin_by_role.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
NAMES = "static int serial_name(const char *name) {"
SERIAL = "int hos_thread_serial(const HosThread *t) {"
SPIN = "int64_t hos_spin_ns(void) {"
LIMIT = "enum { SHORT_SLEEP_TIMER_THREADS = 6 };"


def block(source, marker):
    if marker not in source:
        raise SystemExit("FAIL: host/hos_thread.c has no '%s'" % marker.strip())
    start = source.index(marker)
    end = source.index("\n", start) + 1 if source[start:source.index("\n", start)].rstrip().endswith("}") else \
        start + re.search(r"^}", source[start:], re.M).end()
    return source[start:end]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct HosThread { int serial; } HosThread;
static HosThread me;
static HosThread *hos_self(void) { return &me; }
static int threads;   /* (the processors: argv[3]) */
static int process_threads(void) { return threads; }
@LIMIT@

@NAMES@
@SERIAL@
@SPIN@

int main(int argc, char **argv) {
    CHECK(argc == 4);
    const int64_t serial_ns = atoll(argv[1]), other_ns = atoll(argv[2]);
    threads = atoi(argv[3]);
    /* (the names the game gives its serial threads, and some it gives others) */
    CHECK(serial_name("sead::MainThread") && serial_name("MainThread") && serial_name("Presentation Thread"));
    CHECK(!serial_name("ModuleSystemWorker1") && !serial_name("BaseProcExecutor0") && !serial_name("Presentation"));
    CHECK(!hos_thread_serial(NULL));
    for (int i = 0; i < 2; i++) {   /* (read once: the same answer again) */
        me.serial = 1;
        CHECK(hos_spin_ns() == serial_ns);
        me.serial = 0;
        CHECK(hos_spin_ns() == other_ns);
    }
    return 0;
}
'''

# HOS_SPIN_US, HOS_SPIN_OTHER_US (None: not set), the serial threads' and the others' spin in ns (16 processors)
CASES = [(None, None, 20000, 5000), ("0", None, 0, 0), (None, "20", 20000, 20000), ("3", None, 3000, 3000),
         (None, "50", 20000, 20000), ("40", None, 40000, 5000), ("40", "10", 40000, 10000), (None, "0", 20000, 0),
         (None, "-5", 20000, 0), ("0", "5", 0, 0), (None, "9223372036854776", 20000, 20000), ("9223372036854776", None, 1000000000, 5000)]
# the processors: the short spin only above 6; 6 or fewer, or not known (0), one spin for all unless HOS_SPIN_OTHER_US is set
THREAD_CASES = [(7, None, None, 20000, 5000), (6, None, None, 20000, 20000), (4, None, None, 20000, 20000),
                (0, None, None, 20000, 20000), (6, "40", None, 40000, 40000), (6, "0", None, 0, 0),
                (6, None, "5", 20000, 5000), (6, "-3", None, 0, 0), (4, "9223372036854776", None, 1000000000, 1000000000)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    src = (ROOT / "host" / "hos_thread.c").read_text(encoding="utf-8")
    check(LIMIT in src, "host/hos_thread.c no longer has '%s'" % LIMIT)
    parts = {"@NAMES@": block(src, NAMES), "@SERIAL@": block(src, SERIAL), "@SPIN@": block(src, SPIN), "@LIMIT@": LIMIT}
    check("const int threads = process_threads();" in src, "sleep_ns no longer counts the processors with process_threads")
    # (wired: a thread's flag follows its name, and every spin asks hos_spin_ns)
    check("__atomic_store_n(&t->serial, serial_name(t->name), __ATOMIC_RELAXED);" in src, "hos_thread_set_name no longer sets the thread's serial flag")
    sync = (ROOT / "host" / "nn_os_sync.c").read_text(encoding="utf-8")
    svc = (ROOT / "host" / "hos_svc.c").read_text(encoding="utf-8")
    check(sync.count("hos_now_ns() + hos_spin_ns()") == 2 and "if (hos_spin_ns() <= 0) return 0;" in sync,
          "nn_os_sync.c's spins no longer take their length from hos_spin_ns")
    check("const int64_t limit = hos_spin_ns();" in svc, "hos_svc.c's mutex spin no longer takes its length from hos_spin_ns")
    check("HOS_SPIN_US" not in sync and "HOS_SPIN_US" not in svc, "a spin reads HOS_SPIN_US by itself again")
    env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}

    def build(tmp, name, **changed):
        text = FIXTURE
        for key, part in parts.items():
            text = text.replace(key, changed.get(key.strip("@").lower(), part))
        c, exe = Path(tmp) / (name + ".c"), Path(tmp) / (name + ".exe")
        c.write_text(text, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(c), "-o", str(exe)],
                       check=True)
        return exe

    def run(exe, spin, other, serial_ns, other_ns, threads=16):
        e = dict(env)
        if spin is not None:
            e["HOS_SPIN_US"] = spin
        if other is not None:
            e["HOS_SPIN_OTHER_US"] = other
        return subprocess.run([str(exe), str(serial_ns), str(other_ns), str(threads)], env=e, capture_output=True, text=True)

    with tempfile.TemporaryDirectory(prefix="spin-role-") as tmp:
        exe = build(tmp, "fixture")
        for spin, other, serial_ns, other_ns in CASES:
            r = run(exe, spin, other, serial_ns, other_ns)
            check(r.returncode == 0, "HOS_SPIN_US %r, HOS_SPIN_OTHER_US %r: not %d / %d ns\n%s"
                  % (spin, other, serial_ns, other_ns, r.stderr))
        for threads, spin, other, serial_ns, other_ns in THREAD_CASES:
            r = run(exe, spin, other, serial_ns, other_ns, threads)
            check(r.returncode == 0, "%d processors, HOS_SPIN_US %r, HOS_SPIN_OTHER_US %r: not %d / %d ns\n%s"
                  % (threads, spin, other, serial_ns, other_ns, r.stderr))
        r = run(exe, None, None, 20000, 20000)   # (the fixture tells the roles apart)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "one spin for all passes as the default: the checks prove nothing")

        def broken(name, part, old, new, case, what):
            check(old in parts[part], "%s no longer has the line the broken variant '%s' changes" % (part, name))
            r = run(build(tmp, name, **{part.strip("@").lower(): parts[part].replace(old, new)}), *case)
            check(r.returncode != 0 and "FAIL:" in r.stderr, what)

        broken("no-role", "@SPIN@", "hos_thread_serial(hos_self()) ? b >> 32 : b & 0xFFFFFFFF", "b >> 32",
               (None, None, 20000, 5000), "the others spinning as long as the serial threads passes")
        broken("no-clamp", "@SPIN@", "other = other > s ? s : other;", "", (None, "50", 20000, 20000),
               "the others spinning longer than HOS_SPIN_US passes")
        broken("no-off", "@SPIN@", "other < 0 ? 0 : ", "", (None, "-5", 20000, 0), "a negative spin passes")
        broken("presentation", "@NAMES@", '!strcmp(name, "Presentation Thread")', "0", (None, None, 20000, 5000),
               "the Presentation Thread not counted as a serial thread passes")
        gate = "process_threads() > SHORT_SLEEP_TIMER_THREADS ? 5 : s"
        broken("no-gate", "@SPIN@", gate, "5", (None, None, 20000, 20000, 6),
               "the short spin on 6 processors passes")
        broken("gate-at-6", "@SPIN@", gate, gate.replace(" > ", " >= "), (None, None, 20000, 20000, 6),
               "the short spin from 6 processors on passes")


if __name__ == "__main__":
    main()
