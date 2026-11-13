"""SignalConditionVariable with no waiter returns at once; with one it signals as before.

host/nn_os_sync.c's SignalConditionVariable reads the condition variable's waiter count (cv_waiting, atomic) and returns
without its lock pair and wake call when it is 0. A waiter counts itself in (cv_count_in) before it takes cv->lk, and
so before it gives the
guest mutex back (mutex_release_all), and out (cv_leave) once it has left the wait, so a waiter woken and not yet out
still counts. HOS_CV_SIGNAL_ALWAYS=1: every signal takes the lock, as before.

The fixture runs the real code cut from the source (handoff_on, cv_count_in, cv_enter, cv_leave, cv_reserve, cv_signal_always and the
SignalConditionVariable HLE) over stubs that count lock pairs and wake calls, through the steps of a wait: in, signalled,
out; a signal with nobody in, after the last one left, and between two waiters. A structural check pins the order the
argument rests on: in both waits, cv_count_in comes before cv->lk is taken, and cv_enter before mutex_release_all.
(tests/test_cv_signal_race.py runs the waits on threads.)

python -I -B tests/test_cv_signal_early.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
SIGNAL = "HLE(_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE) {"
WAIT = "HLE(_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE) {"
TIMED = "HLE(_ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE) {"
PARTS = {"@HANDOFF@": "static int handoff_on(void) {", "@COUNT@": "static void cv_count_in(Obj *cv) {",
         "@ENTER@": "static void cv_enter(Obj *cv, Obj *m) {",
         "@LEAVE@": "static int cv_leave(Obj *cv, Obj *m) {", "@RESERVE@": "static void cv_reserve(Obj *cv) {",
         "@ALWAYS@": "static int cv_signal_always(void) {", "@SIGNAL@": SIGNAL}


def block(source, marker):
    if marker not in source:
        raise SystemExit("FAIL: host/nn_os_sync.c has no '%s'" % marker.strip())
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hookshot_env.h"   /* (env_on: the switches' one rule) */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { int held; } SRWLOCK;
typedef struct { int x; } CONDITION_VARIABLE;
static int locks, wakes;
static void AcquireSRWLockExclusive(SRWLOCK *l) { CHECK(!l->held); l->held = 1; locks++; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { CHECK(l->held); l->held = 0; }
static void WakeConditionVariable(CONDITION_VARIABLE *cv) { (void)cv; wakes++; }
typedef struct Obj {
    uint64_t key;
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    uint64_t gen;
    int reserved;
    int cv_waiting, cv_in, cv_owed, cv_mixed;
    struct Obj *cv_mutex, *cv_owed_mutex[8];
} Obj;
static Obj cvo, mo, mo2;
enum { K_COND = 2 };
static Obj *obj(uint64_t key, int kind) { (void)kind; cvo.key = key; return &cvo; }
static int watched_cv(uint64_t cv) { (void)cv; return 0; }
static uint64_t watch_mutex(void) { return 0; }
typedef struct { uint64_t x[8]; } Ctx;
static void watch_log(const char *w, uint64_t m, uint64_t cv, Ctx *c, Obj *o) { (void)w, (void)m, (void)cv, (void)c, (void)o; }
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL };
#define HOS_CHAIN(kind, key) ((void)(kind), (void)(key))   /* (HOS_CHAIN off: host/hos.h) */
#define ARG(i) (c->x[i])
#define HLE(name) static void name(Ctx *c)

@HANDOFF@
@COUNT@
@ENTER@
@LEAVE@
@RESERVE@
@ALWAYS@
@SIGNAL@

/* one signal: whether it took the lock and made a wake call (both or neither) */
static int signal_once(void) {
    const int l = locks, w = wakes;
    const uint64_t g = cvo.gen;
    Ctx c = {{0x1000}};
    _ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE(&c);
    const int took = locks != l;
    CHECK(took == (wakes != w) && took == (cvo.gen != g));
    return took;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const int always = atoi(argv[1]);   /* (HOS_CV_SIGNAL_ALWAYS set by the caller) */
    /* nobody waits: a signal returns at once */
    CHECK(signal_once() == always);
    /* a waiter in (as WaitConditionVariable does under cv->lk, before it gives the mutex back): the signal is made, and
       reserves the mutex for it */
    cv_count_in(&cvo), cv_enter(&cvo, &mo);
    CHECK(signal_once() == 1 && mo.reserved == 1);
    /* woken and not yet out: still counted, so another signal is made (and reserves nothing more: one waiter) */
    CHECK(signal_once() == 1 && mo.reserved == 1);
    CHECK(cv_leave(&cvo, &mo) == 1);   /* (out, owed the mutex) */
    mo.reserved--;   /* (as mutex_take_owed does once it has the mutex) */
    /* the last one out: back to returning at once */
    CHECK(signal_once() == always);
    /* two waiters with different mutexes, one leaves: the other is still signalled */
    cv_count_in(&cvo), cv_enter(&cvo, &mo);
    cv_count_in(&cvo), cv_enter(&cvo, &mo2);
    CHECK(cvo.cv_mixed);
    CHECK(cv_leave(&cvo, &mo) == 0);
    CHECK(signal_once() == 1);
    CHECK(cv_leave(&cvo, &mo2) == 0);
    CHECK(signal_once() == always);
    CHECK(cvo.cv_waiting == 0 && cvo.cv_in == 0 && cvo.cv_owed == 0 && !cvo.lk.held);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    src = (ROOT / "host" / "nn_os_sync.c").read_text(encoding="utf-8")
    parts = {key: block(src, marker) for key, marker in PARTS.items()}
    # (the order the argument rests on: a waiter is counted in before it gives the guest mutex back, in both waits)
    for name, marker in (("WaitConditionVariable", WAIT), ("TimedWaitConditionVariable", TIMED)):
        body = block(src, marker)
        check("cv_enter(cv, m);" in body and "mutex_release_all(m);" in body
              and body.index("cv_enter(cv, m);") < body.index("mutex_release_all(m);"),
              "%s no longer counts its waiter in (cv_enter) before it gives the mutex back" % name)
        check("cv_count_in(cv);" in body and body.index("cv_count_in(cv);") < body.index("AcquireSRWLockExclusive(&cv->lk);"),
              "%s no longer counts its waiter in (cv_count_in) before it takes cv->lk" % name)
        check(body.index("AcquireSRWLockExclusive(&cv->lk);") < body.index("cv_enter(cv, m);"),
              "%s counts its waiter in outside cv->lk" % name)
    check("cv_waiting++" not in src and "cv_waiting--" not in src, "cv_waiting written without an atomic again")
    env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}

    def build(tmp, name, **changed):
        text = FIXTURE
        for key, part in parts.items():
            text = text.replace(key, changed.get(key, part))
        c, exe = Path(tmp) / (name + ".c"), Path(tmp) / (name + ".exe")
        c.write_text(text, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I",
                        str(ROOT / "gpu" / "vk" / "compat"), str(c), "-o", str(exe)], check=True)
        return exe

    def run(exe, asked, always):
        e = dict(env) if asked is None else dict(env, HOS_CV_SIGNAL_ALWAYS=asked)
        return subprocess.run([str(exe), str(always)], env=e, capture_output=True, text=True)

    with tempfile.TemporaryDirectory(prefix="cv-signal-") as tmp:
        exe = build(tmp, "fixture")
        for asked, always in ((None, 0), ("1", 1), ("0", 0), ("off", 0), ("on", 1)):
            r = run(exe, asked, always)
            check(r.returncode == 0, "HOS_CV_SIGNAL_ALWAYS %r: signals not as expected\n%s" % (asked, r.stderr))
            r = run(exe, asked, 1 - always)
            check(r.returncode != 0 and "FAIL:" in r.stderr, "HOS_CV_SIGNAL_ALWAYS %r passes both ways" % asked)

        def broken(name, part, old, new, what):
            check(old in parts[part], "%s no longer has the line the broken variant '%s' changes" % (part, name))
            r = run(build(tmp, name, **{part: parts[part].replace(old, new)}), None, 0)
            check(r.returncode != 0 and "FAIL:" in r.stderr, what)

        early = "if (!__atomic_load_n(&cv->cv_waiting, __ATOMIC_SEQ_CST) && !cv_signal_always()) return;"
        broken("always-early", "@SIGNAL@", early, "if (!cv_signal_always()) return;",
               "a signal returning with a waiter in passes")
        broken("owed-count", "@SIGNAL@", early, early.replace("cv->cv_waiting", "cv->cv_owed"),
               "a signal looking at the owed count, not the waiters, passes")
        broken("leave-early", "@COUNT@", "__atomic_add_fetch(&cv->cv_waiting, 1, __ATOMIC_SEQ_CST);", "(void)cv;",
               "a waiter never counted in passes")


if __name__ == "__main__":
    main()
