"""The game's sleeps under a millisecond wait on the timer where the process has few logical processors.

The game's idle job workers sleep 20 us and look again, thousands of times a second. host/hos_thread.c's sleep_ns answers
a sleep under a millisecond by yielding until its time (the Windows timer wakes about 0.5 ms late), which keeps a
processor busy for each sleeping thread. With SHORT_SLEEP_TIMER_THREADS logical processors or fewer those loops take the
time the frame's threads need, so there the game's own sleeps (SleepThread) wait on the timer; the host's own short
sleeps (the frame limiter, the audio renderer) still yield and come back at their time. HOS_SHORT_SLEEP_TIMER, when it is
set, is read by env_on's rule (gpu/vk/compat/hookshot_env.h): on is the timer for every short sleep, off (0, false, off,
no, empty) is yielding for every one; not set, the processors decide.

Where the game's short sleeps yield (more processors), at most HOS_SHORT_SLEEP_POLLERS of the game's threads (not set: a
quarter of the processors; 0: no limit; none when HOS_SHORT_SLEEP_TIMER is set) yield through one at the same time; the next waits on the timer until one is back.
The frame's serial threads (hos_thread_serial) and the host's own sleeps always yield and are not counted.

The fixture runs the real code cut from the source (the choice, hos_sleep_ns, sleep_ns and the game's entry
hos_sleep_logged) over stubs that count the yields and the timer waits, one process a case (the choice is made once).

python -I -B tests/test_short_sleep_timer.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
PROCS = "enum { SHORT_SLEEP_TIMER_THREADS = 6 };"   # (with process_threads: the processors, shared with hos_spin_ns)
CHOICE = "static int hos_short_sleep_timer(const char *asked, int threads) {"
AWAY = "static void sleep_away(int64_t ns, int guest) {"
HOST = "void hos_sleep_ns(int64_t ns) { sleep_away(ns, 0); }\n"
SLEEP = "static void sleep_ns(int64_t ns, int guest) {"
GUEST = "void hos_sleep_logged(int64_t ns, uint64_t lr) {"
POLLERS = "static volatile LONG short_pollers;"


def block(source, marker):
    if marker not in source:
        raise SystemExit("FAIL: host/hos_thread.c has no '%s'" % marker.strip())
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hookshot_env.h"   /* (env_value_off: the switches' one rule) */
#ifndef _WIN32
#define _WIN32 1   /* (the choice by processors is the Windows build's: the cut code is that build on every runner) */
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef uintptr_t DWORD_PTR;
typedef long LONG;
typedef int BOOL;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
#define INFINITE 0xFFFFFFFFul
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2
#define TIMER_ALL_ACCESS 0x1F0003
static int64_t now_ns = 1000;
static int yields, timer_waits, sleeps, logged, threads, groups = 1, affinity_calls, core_yields, aways, backs;
static int64_t timer_due;
static int64_t hos_now_ns(void) { return now_ns; }
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL, HOS_CHAIN_SLEEP, HOS_CHAIN_AWAKE, HOS_CHAIN_PRESENT };
#define HOS_CHAIN(kind, key) ((void)(kind), (void)(key))   /* (HOS_CHAIN off: host/hos.h) */
static BOOL SwitchToThread(void) { yields++; now_ns += 3000; return 1; }
static void YieldProcessor(void) { now_ns += 100; }
static void Sleep(DWORD ms) { sleeps++; now_ns += (int64_t)ms * 1000000; }
static HANDLE GetCurrentProcess(void) { return (HANDLE)(uintptr_t)1; }
static unsigned short GetActiveProcessorGroupCount(void) { return (unsigned short)groups; }
static BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
    affinity_calls++;
    if (threads < 0) return 0;   /* (the call failed: the processors are not known) */
    *proc = threads >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << threads) - 1);
    *sys = ~(DWORD_PTR)0;
    return 1;
}
static LONG InterlockedExchange(volatile LONG *p, LONG v) { const LONG old = *p; *p = v; return old; }
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static LONG InterlockedDecrement(volatile LONG *p) { return --*p; }
typedef struct HosThread HosThread;
static int serial;   /* (whether the sleeping thread is one of the frame's serial threads) */
static HosThread *hos_self(void) { return (HosThread *)(uintptr_t)8; }
static int hos_thread_serial(const HosThread *t) { return t && serial; }
static void AcquireSRWLockExclusive(SRWLOCK *l) { CHECK(!*l); *l = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { CHECK(*l); *l = 0; }
static HANDLE CreateWaitableTimerExW(void *a, void *n, DWORD flags, DWORD access) {
    (void)a, (void)n, (void)flags, (void)access;
    return (HANDLE)(uintptr_t)2;
}
static BOOL SetWaitableTimerEx(HANDLE t, const LARGE_INTEGER *due, LONG period, void *f, void *arg, void *ctx, DWORD tol) {
    (void)t, (void)period, (void)f, (void)arg, (void)ctx, (void)tol;
    timer_due = -due->QuadPart * 100;
    return 1;
}
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    (void)h, (void)ms;
    timer_waits++;
    now_ns += timer_due + 500000;   /* (the timer wakes about half a millisecond late) */
    return 0;
}
static void hos_log(const char *fmt, ...) { (void)fmt; logged++; }
static void hos_vcore_yield(void) { core_yields++; }
static int hos_vcore_away(void) { aways++; return 1; }
static void hos_vcore_back(void) { backs++; }
static void sleep_ns(int64_t ns, int guest);

@PROCS@
@CHOICE@
@POLLERS@
@AWAY@
@HOST@
@SLEEP@
@GUEST@

/* one sleep under a millisecond: whether it waited on the timer; never back early, and back at its time when it yields */
static int short_sleep(int guest, int64_t ns) {
    const int64_t t0 = now_ns;
    const int waits = timer_waits, y = yields;
    if (guest) hos_sleep_logged(ns, 0x8009a478); else hos_sleep_ns(ns);
    CHECK(now_ns - t0 >= ns);
    if (timer_waits == waits) {
        CHECK(yields > y && now_ns - t0 < ns + 100000);
        return 0;
    }
    CHECK(timer_waits == waits + 1 && yields == y);
    return 1;
}

/* the limit on the game's threads yielding at once: cap of them may (0: no limit) */
static void pollers(int cap) {
    CHECK(short_pollers == 0);
    if (!cap) {   /* (no limit: however many poll, the game's next short sleep yields) */
        short_pollers = 1000;
        CHECK(short_sleep(1, 20000) == 0 && short_pollers == 1000);
        short_pollers = 0;
        return;
    }
    short_pollers = cap - 1;   /* (room for one more: it yields, and is not counted once back) */
    CHECK(short_sleep(1, 20000) == 0 && short_pollers == cap - 1);
    short_pollers = cap;   /* (full: the game's next short sleep waits on the timer, and the count stays) */
    CHECK(short_sleep(1, 20000) == 1 && short_pollers == cap);
    CHECK(short_sleep(1, 999999) == 1 && short_pollers == cap);
    serial = 1;   /* (a serial thread yields all the same, not counted) */
    CHECK(short_sleep(1, 20000) == 0 && short_pollers == cap);
    serial = 0;
    CHECK(short_sleep(0, 20000) == 0 && short_pollers == cap);   /* (the host's own sleeps: as before) */
    short_pollers = 0;
    /* (the choice by itself) */
    CHECK(hos_short_sleep_pollers(NULL, 16) == 4 && hos_short_sleep_pollers(NULL, 8) == 2 && hos_short_sleep_pollers(NULL, 7) == 1);
    CHECK(hos_short_sleep_pollers(NULL, 6) == 0 && hos_short_sleep_pollers(NULL, 0) == 0 && hos_short_sleep_pollers(NULL, -1) == 0);
    CHECK(hos_short_sleep_pollers("0", 16) == 0 && hos_short_sleep_pollers("2", 16) == 2 && hos_short_sleep_pollers("3", 4) == 3);
    CHECK(hos_short_sleep_pollers("-1", 16) == 0 && hos_short_sleep_pollers("x", 16) == 4 && hos_short_sleep_pollers("", 16) == 4);
}

int main(int argc, char **argv) {
    CHECK(argc >= 3 && argc <= 5);
    threads = atoi(argv[1]);
    if (argc >= 4) groups = atoi(argv[3]);   /* (processor groups: the mask is then one group's) */
    const int cap = argc == 5 ? atoi(argv[4]) : 0;   /* (the game's threads that may yield at once; 0: no limit) */
    const int how = atoi(argv[2]);   /* 0: all yield, 1: the game's sleeps on the timer, 2: all on the timer */
    CHECK(how >= 0 && how <= 2);
    /* (the choice by itself, at its edge) */
    CHECK(hos_short_sleep_timer(NULL, SHORT_SLEEP_TIMER_THREADS) == 1);
    CHECK(hos_short_sleep_timer(NULL, SHORT_SLEEP_TIMER_THREADS + 1) == 0);
    CHECK(hos_short_sleep_timer(NULL, 1) == 1 && hos_short_sleep_timer(NULL, 0) == 0 && hos_short_sleep_timer(NULL, -1) == 0);
    CHECK(hos_short_sleep_timer("", 1) == 0 && hos_short_sleep_timer("", 64) == 0);   /* (set but empty: off, as env_on has it) */
    CHECK(hos_short_sleep_timer("1", 64) == 2 && hos_short_sleep_timer("1", 1) == 2 && hos_short_sleep_timer("0", 1) == 0);
    CHECK(hos_short_sleep_timer("on", 64) == 2 && hos_short_sleep_timer("yes", 64) == 2 && hos_short_sleep_timer("true", 1) == 2);
    CHECK(hos_short_sleep_timer("off", 1) == 0 && hos_short_sleep_timer("No", 1) == 0 && hos_short_sleep_timer(" false ", 1) == 0);
    /* the host's own short sleeps (the frame limiter's last part), then the game's 20 us sleeps, then one more of each */
    for (int i = 0; i < 2; i++) CHECK(short_sleep(0, 20000) == (how == 2));
    for (int i = 0; i < 3; i++) CHECK(short_sleep(1, 20000) == (how >= 1));
    CHECK(short_sleep(0, 999999) == (how == 2));
    CHECK(short_sleep(1, 999999) == (how >= 1));
    /* a millisecond or more is the timer's whoever asks */
    int waits = timer_waits, y = yields;
    hos_sleep_ns(2000000);
    hos_sleep_logged(1000000, 0x8009a478);
    CHECK(timer_waits == waits + 2 && yields == y && sleeps == 0);
    /* a sleep of 0 gives the core to a waiter and the processor to another thread: no wait */
    waits = timer_waits, y = yields;
    const int a = aways;
    hos_sleep_logged(0, 0x8009a478);
    CHECK(core_yields == 1 && yields == y + 1 && timer_waits == waits && aways == a);
    CHECK(aways == 9 && backs == aways);   /* (every sleep gave its virtual core up and took one back) */
    CHECK(affinity_calls == (groups == 1));   /* (chosen once; with several groups the mask is not asked for) */
    CHECK(logged == (how || cap ? 1 : 0));   /* (the log says so once where the timer or a limit is chosen) */
    if (how == 0) pollers(cap);
    return 0;
}
'''

# logical processors the process may run on (-1: not known), HOS_SHORT_SLEEP_TIMER (None: not set), how short sleeps wait
CASES = [(1, None, 1), (2, None, 1), (4, None, 1), (6, None, 1),
         (7, None, 0), (8, None, 0), (12, None, 0), (16, None, 0), (64, None, 0), (-1, None, 0), (0, None, 0),
         (16, "1", 2), (4, "1", 2), (-1, "1", 2), (4, "0", 0), (16, "0", 0), (4, "", 0), (16, "", 0),
         (16, "on", 2), (16, "yes", 2), (16, "true", 2), (4, "off", 0), (4, "no", 0), (4, "false", 0), (4, " 0 ", 0), (16, "2", 2)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    src = (ROOT / "host" / "hos_thread.c").read_text(encoding="utf-8")
    parts = {"@PROCS@": block(src, PROCS), "@CHOICE@": block(src, CHOICE), "@POLLERS@": block(src, POLLERS), "@AWAY@": block(src, AWAY), "@SLEEP@": block(src, SLEEP),
             "@GUEST@": block(src, GUEST)}
    check(HOST in src, "host/hos_thread.c has no '%s'" % HOST.strip())
    parts["@HOST@"] = HOST
    # (the game's sleeps reach hos_sleep_logged: nn::os::SleepThread and the kernel's)
    check("hos_sleep_logged((int64_t)ARG(0), c->x[30]);" in src, "nn::os::SleepThread no longer sleeps through hos_sleep_logged")
    check("hos_sleep_logged((int64_t)c->x[0]" in (ROOT / "host" / "hos_svc.c").read_text(encoding="utf-8"),
          "the kernel's SleepThread no longer sleeps through hos_sleep_logged")
    env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}

    def build(tmp, name, **changed):
        text = FIXTURE
        for key, part in parts.items():
            text = text.replace(key, changed.get(key.strip("@").lower(), part))
        c, exe = Path(tmp) / (name + ".c"), Path(tmp) / (name + ".exe")
        c.write_text(text, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(ROOT / "gpu" / "vk" / "compat"),
                        str(c), "-o", str(exe)], check=True)
        return exe

    def run(exe, threads, asked, how, groups=1, cap=None, polled=None):
        e = dict(env) if asked is None else dict(env, HOS_SHORT_SLEEP_TIMER=asked)
        if polled is not None:
            e["HOS_SHORT_SLEEP_POLLERS"] = polled
        if cap is None:   # (the default limit where the game's short sleeps yield: a quarter of the processors)
            cap = threads // 4 if how == 0 and groups == 1 and threads > 6 and asked is None else 0
        return subprocess.run([str(exe), str(threads), str(how), str(groups), str(cap)], env=e, capture_output=True, text=True)

    def broken(tmp, name, part, old, new, case, what):
        check(old in parts[part], "%s no longer has the line the broken variant '%s' changes" % (part, name))
        exe = build(tmp, name, **{part.strip("@").lower(): parts[part].replace(old, new)})
        r = run(exe, *case)
        check(r.returncode != 0 and "FAIL:" in r.stderr, what)

    with tempfile.TemporaryDirectory(prefix="short-sleep-") as tmp:
        exe = build(tmp, "fixture")
        for threads, asked, how in CASES:
            r = run(exe, threads, asked, how)
            check(r.returncode == 0, "%d threads, HOS_SHORT_SLEEP_TIMER %r: short sleeps do not wait as in %d\n%s"
                  % (threads, asked, how, r.stderr))
            for other in {0, 1, 2} - {how}:   # (and the fixture tells the three apart)
                r = run(exe, threads, asked, other)
                check(r.returncode != 0 and "FAIL:" in r.stderr, "%d threads, %r passes as %d too: the checks prove nothing"
                      % (threads, asked, other))
        # more than one processor group: the mask is one group's (4 of 128 processors, say), so all yield as shipped
        for asked, how in ((None, 0), ("1", 2), ("0", 0)):
            r = run(exe, 4, asked, how, groups=2)
            check(r.returncode == 0, "two processor groups, a mask of 4, HOS_SHORT_SLEEP_TIMER %r: not as in %d\n%s"
                  % (asked, how, r.stderr))
        r = run(exe, 4, None, 1, groups=2)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "one group's mask of 4 counts as the process's processors")
        # the limit on the game's threads yielding at once (HOS_SHORT_SLEEP_POLLERS), where they yield
        for threads, polled, cap in ((16, None, 4), (8, None, 2), (12, None, 3), (16, "0", 0), (16, "2", 2), (16, "off", 4),
                                     (64, None, 16), (-1, None, 0), (-1, "2", 2)):
            r = run(exe, threads, None, 0, cap=cap, polled=polled)
            check(r.returncode == 0, "%d threads, HOS_SHORT_SLEEP_POLLERS %r: not a limit of %d\n%s" % (threads, polled, cap, r.stderr))
            r = run(exe, threads, None, 0, cap=cap + 1, polled=polled)
            check(r.returncode != 0 and "FAIL:" in r.stderr, "%d threads, %r passes as a limit of %d too" % (threads, polled, cap + 1))
        r = run(exe, 4, None, 1, polled="2")   # (where the game's sleeps wait on the timer, no limit applies)
        check(r.returncode == 0, "4 threads, HOS_SHORT_SLEEP_POLLERS=2: not the timer as without it\n" + r.stderr)
        # broken variants: each must fail the case it breaks
        broken(tmp, "never", "@CHOICE@", "threads > 0 && threads <= SHORT_SLEEP_TIMER_THREADS ? 1 : 0", "threads < 0 ? 1 : 0",
               (4, None, 1), "all yielding on 4 threads passes: the processors are not looked at")
        broken(tmp, "presence", "@CHOICE@", "env_value_off(asked) ? 0 : 2", "2",
               (4, "0", 0), "HOS_SHORT_SLEEP_TIMER=0 read by its presence passes")
        broken(tmp, "number", "@CHOICE@", "env_value_off(asked) ? 0 : 2", "atoi(asked) != 0 ? 2 : 0",
               (16, "on", 2), "HOS_SHORT_SLEEP_TIMER=on read as a number passes: not env_on's rule")
        broken(tmp, "unwired", "@GUEST@", "sleep_away(ns, 1)", "sleep_away(ns, 0)",
               (4, None, 1), "the game's sleeps passed on as the host's pass on 4 threads")
        broken(tmp, "host-too", "@SLEEP@", "short_timer < (guest ? 1 : 2)", "!short_timer && (guest || 1)",
               (4, None, 1), "the host's own short sleeps on the timer pass on 4 threads")
        broken(tmp, "one-group", "@PROCS@", "GetActiveProcessorGroupCount() == 1 && ", "",
               (4, None, 0, 2), "one group's mask taken for the process's processors passes")
        broken(tmp, "no-limit", "@SLEEP@", "InterlockedIncrement(&short_pollers) <= poll_cap",
               "(InterlockedIncrement(&short_pollers), 1)",
               (16, None, 0), "the game's short sleeps all yielding past the limit passes")
        broken(tmp, "serial-counted", "@SLEEP@", "guest && poll_cap && !hos_thread_serial(hos_self())", "guest && poll_cap",
               (16, None, 0), "a serial thread sent to the timer at the limit passes")
        broken(tmp, "leak", "@SLEEP@", "if (counted) InterlockedDecrement(&short_pollers);", "",
               (16, None, 0), "a poller never counted out passes")
        edge = "enum { SHORT_SLEEP_TIMER_THREADS = 6 };"   # (the number the measurements gave: a change of it is a change here too)
        check(edge in parts["@PROCS@"], "SHORT_SLEEP_TIMER_THREADS is no longer 6: the cases above name 6 and 7 threads")


if __name__ == "__main__":
    main()
