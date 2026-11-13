"""BroadcastConditionVariable and the timed/try semaphore acquires run on the HLE's objects, not the guest SDK's words.

host/nn_os_sync.c replaces the nn::os condition variable and semaphore functions with host objects. Siblings left to the
lifted SDK act on guest words the HLE never writes, and signal kernel keys no HLE waiter sleeps on:
- BroadcastConditionVariable woke nobody (the movie framework and NVIDIA's OS layer call it);
- TimedAcquireSemaphore (the C library's timed semaphore wait, NVIDIA's OS layer) never saw an HLE release.

The fixture runs the real code cut from the source (handoff_on, cv_enter, cv_leave, cv_reserve, the Broadcast HLE,
sema_acquire_for) over stubs: counted lock pairs and wake calls, and a fake clock whose wait can release the semaphore.
- Broadcast: it moves the generation and wakes all, and every waiter not yet owed the mutex is owed it, up to
  cv_reserve's 8; none with mixed mutexes or HOS_NO_HANDOFF=1.
- Semaphore: a count is taken at once, a zero timeout never waits, a release inside the timeout is taken, and the
  deadline ends the wait.
Broken variants must fail: one wake instead of all, no generation move, one reservation, a single wait, a count of 0
taken, no decrement.

python -I -B tests/test_os_lifted_siblings.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
PARTS = {"@HANDOFF@": "static int handoff_on(void) {", "@COUNT@": "static void cv_count_in(Obj *cv) {",
         "@ENTER@": "static void cv_enter(Obj *cv, Obj *m) {",
         "@LEAVE@": "static int cv_leave(Obj *cv, Obj *m) {", "@RESERVE@": "static void cv_reserve(Obj *cv) {",
         "@BROADCAST@": "HLE(_ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE) {",
         "@SEMA@": "static int sema_acquire_for(Obj *s, int64_t ns) {"}
# (each acquire the guest SDK would otherwise run, as one line calling sema_acquire_for)
SEMA_HLE = {"_ZN2nn2os21TimedAcquireSemaphoreEPNS0_13SemaphoreTypeENS_8TimeSpanE": "(int64_t)ARG(1)",
            "_ZN2nn2os26TimedAcquireLightSemaphoreEPNS0_18LightSemaphoreTypeENS_8TimeSpanE": "(int64_t)ARG(1)",
            "_ZN2nn2os24TryAcquireLightSemaphoreEPNS0_18LightSemaphoreTypeE": "0"}


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
static int wakes, wakealls;
static void AcquireSRWLockExclusive(SRWLOCK *l) { CHECK(!l->held); l->held = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { CHECK(l->held); l->held = 0; }
static void WakeConditionVariable(CONDITION_VARIABLE *cv) { (void)cv; wakes++; }
static void WakeAllConditionVariable(CONDITION_VARIABLE *cv) { (void)cv; wakealls++; }
typedef struct Obj {
    uint64_t key;
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    uint64_t gen;
    int sem;
    int reserved;
    int cv_waiting, cv_in, cv_owed, cv_mixed;   // (cv_in: #2708, the waiters inside under cv->lk)
    struct Obj *cv_mutex, *cv_owed_mutex[8];
} Obj;
static Obj cvo, so, mo, mo2;
enum { K_COND = 2, K_SEMA = 4 };
static Obj *obj(uint64_t key, int kind) { Obj *o = kind == K_COND ? &cvo : &so; o->key = key; return o; }
static int watched_cv(uint64_t cv) { (void)cv; return 0; }
static uint64_t watch_mutex(void) { return 0; }
typedef struct { uint64_t x[8]; } Ctx;
static void watch_log(const char *w, uint64_t m, uint64_t cv, Ctx *c, Obj *o) { (void)w, (void)m, (void)cv, (void)c, (void)o; }
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL };
#define HOS_CHAIN(kind, key) ((void)(kind), (void)(key))   /* (HOS_CHAIN off: host/hos.h) */
static int hos_chain_on, chain_notes;
static void hos_chain_note(int kind, uint64_t key, uint64_t lr) { (void)kind, (void)key, (void)lr; chain_notes++; }
#define ARG(i) (c->x[i])
#define HLE(name) static void name(Ctx *c)
/* a fake clock: each wait sleeps 1 ms (or less, to its deadline); a release can be due at a time */
static int64_t now_ns = 1000000000, release_at;
static int waits;
static int64_t hos_now_ns(void) { return now_ns; }
static int wait_until(Obj *o, int64_t deadline) {   /* (host/nn_os_sync.c's: 0 on timeout) */
    CHECK(o->lk.held && deadline);
    CHECK(++waits < 100000);
    const int64_t left = deadline - now_ns;
    if (left <= 0) return 0;
    now_ns += left < 1000000 ? left : 1000000;
    if (release_at && now_ns >= release_at) { o->sem++; release_at = 0; }
    return 1;
}

@HANDOFF@
@COUNT@
@ENTER@
/* a waiter as the wait HLEs bring one in: counted (cv_count_in, before cv->lk), then in (cv_enter) */
static void waiter_in(Obj *cv, Obj *m) { cv_count_in(cv); cv_enter(cv, m); }
@LEAVE@
@RESERVE@
@BROADCAST@
@SEMA@

static void broadcast(void) {
    const int a = wakealls, w = wakes;
    const uint64_t g = cvo.gen;
    Ctx c = {{0x1000}};
    _ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE(&c);
    CHECK(cvo.gen == g + 1 && wakealls == a + 1 && wakes == w && !cvo.lk.held);
}
/* all out: how many were owed their mutex (each owed one takes it, as mutex_take_owed does) */
static int leave_all(Obj *m) {
    int owed = 0;
    while (cvo.cv_waiting) {
        if (cv_leave(&cvo, m)) { owed++; m->reserved--; }
    }
    CHECK(cvo.cv_owed == 0 && m->reserved == 0);
    return owed;
}
static int acquire(int sem, int64_t ns, int64_t release_after) {
    so.sem = sem;
    waits = 0;
    release_at = release_after ? now_ns + release_after : 0;
    const int ok = sema_acquire_for(&so, ns);
    CHECK(!so.lk.held);
    return ok;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const int handoff = atoi(argv[1]);   /* (HOS_NO_HANDOFF set by the caller) */
    /* Broadcast with nobody waiting: the generation moves, nothing is owed */
    broadcast();
    CHECK(mo.reserved == 0);
    /* three waiters on one mutex: all wake and all are owed it */
    for (int i = 0; i < 3; i++) waiter_in(&cvo, &mo);
    broadcast();
    CHECK(mo.reserved == (handoff ? 3 : 0));
    CHECK(leave_all(&mo) == (handoff ? 3 : 0));
    /* one owed by an earlier signal, two more in: the broadcast owes only the two */
    waiter_in(&cvo, &mo);
    if (handoff) cv_reserve(&cvo);
    waiter_in(&cvo, &mo);
    waiter_in(&cvo, &mo);
    broadcast();
    CHECK(mo.reserved == (handoff ? 3 : 0));
    CHECK(leave_all(&mo) == (handoff ? 3 : 0));
    /* ten waiters: at most 8 owed (cv_reserve's limit), the others lock as anyone does */
    for (int i = 0; i < 10; i++) waiter_in(&cvo, &mo);
    broadcast();
    CHECK(mo.reserved == (handoff ? 8 : 0));
    CHECK(leave_all(&mo) == (handoff ? 8 : 0));
    /* waiters with different mutexes: no hand-off */
    waiter_in(&cvo, &mo);
    waiter_in(&cvo, &mo2);
    broadcast();
    CHECK(mo.reserved == 0 && mo2.reserved == 0);
    cv_leave(&cvo, &mo), cv_leave(&cvo, &mo2);
    CHECK(cvo.cv_waiting == 0 && cvo.cv_owed == 0);

    /* semaphore: a count there is taken at once */
    CHECK(acquire(2, 0, 0) == 1 && so.sem == 1 && waits == 0);
    CHECK(acquire(1, 5000000, 0) == 1 && so.sem == 0 && waits == 0);
    /* none there, zero or negative timeout: fails without waiting */
    CHECK(acquire(0, 0, 0) == 0 && so.sem == 0 && waits <= 1);
    CHECK(acquire(0, -5, 0) == 0 && so.sem == 0 && waits <= 1);
    /* a release 3 ms into a 10 ms wait: taken, before the deadline */
    int64_t t0 = now_ns;
    CHECK(acquire(0, 10000000, 3000000) == 1 && so.sem == 0 && now_ns - t0 < 10000000);
    /* no release: the deadline ends it */
    t0 = now_ns;
    CHECK(acquire(0, 10000000, 0) == 0 && so.sem == 0 && now_ns - t0 >= 10000000);
    /* a release after the deadline is not taken */
    CHECK(acquire(0, 2000000, 5000000) == 0 && so.sem == 0);
    /* a huge timeout (the deadline must not wrap): a release 3 ms in is taken */
    CHECK(acquire(0, INT64_MAX, 3000000) == 1 && so.sem == 0);
    /* HOS_CHAIN on: a wait is noted (in and out), an acquire that does not wait is not */
    hos_chain_on = 1;
    CHECK(acquire(1, 5000000, 0) == 1 && chain_notes == 0);
    CHECK(acquire(0, 5000000, 1000000) == 1 && chain_notes == 2);
    hos_chain_on = 0;
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    src = (ROOT / "host" / "nn_os_sync.c").read_text(encoding="utf-8")
    parts = {key: block(src, marker) for key, marker in PARTS.items()}
    for name, arg in SEMA_HLE.items():
        check(re.search(r"^HLE\(%s\) \{ RET\(sema_acquire_for\(obj\(ARG\(0\), K_SEMA\), %s\)\); \}$" % (name, re.escape(arg)), src, re.M),
              "%s is not the HLE's sema_acquire_for (left to the guest SDK, it never sees an HLE release)" % name)
    # (the fixture stubs wait_until: its clamp, below INFINITE's 0xFFFFFFFF ms, is pinned here)
    wait = block(src, "static int wait_until(Obj *o, int64_t deadline) {")
    check("(DWORD)(ms < 0x7FFFFFFF ? ms : 0x7FFFFFFF)" in wait,
          "wait_until no longer caps one sleep below 0xFFFFFFFF ms (INFINITE): a long finite wait could sleep forever")
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

    def run(exe, no_handoff, handoff):
        e = dict(env) if no_handoff is None else dict(env, HOS_NO_HANDOFF=no_handoff)
        return subprocess.run([str(exe), str(handoff)], env=e, capture_output=True, text=True)

    with tempfile.TemporaryDirectory(prefix="os-siblings-") as tmp:
        exe = build(tmp, "fixture")
        for no_handoff, handoff in ((None, 1), ("1", 0), ("0", 1)):
            r = run(exe, no_handoff, handoff)
            check(r.returncode == 0, "HOS_NO_HANDOFF %r: not as expected\n%s" % (no_handoff, r.stderr))
            r = run(exe, no_handoff, 1 - handoff)
            check(r.returncode != 0 and "FAIL:" in r.stderr, "HOS_NO_HANDOFF %r passes both ways" % no_handoff)

        def broken(name, part, old, new, what):
            check(old in parts[part], "%s no longer has the line the broken variant '%s' changes" % (part, name))
            r = run(build(tmp, name, **{part: parts[part].replace(old, new)}), None, 1)
            check(r.returncode != 0 and "FAIL:" in r.stderr, what)

        broken("wake-one", "@BROADCAST@", "WakeAllConditionVariable(&cv->cv);", "WakeConditionVariable(&cv->cv);",
               "a broadcast waking one waiter passes")
        broken("no-gen", "@BROADCAST@", "cv->gen++;", "", "a broadcast not moving the generation passes")
        broken("owe-one", "@BROADCAST@", "for (int n = cv->cv_waiting - cv->cv_owed; n > 0; n--) cv_reserve(cv);",
               "cv_reserve(cv);", "a broadcast owing the mutex to one waiter passes")
        broken("wait-once", "@SEMA@", "while (s->sem <= 0 && wait_until(s, deadline) && hos_now_ns() < deadline) {}",
               "if (s->sem <= 0) wait_until(s, deadline);", "a timed acquire that waits once passes")
        broken("take-zero", "@SEMA@", "const int ok = s->sem > 0;", "const int ok = s->sem >= 0;",
               "a timed acquire taking a count of 0 passes")
        broken("no-take", "@SEMA@", "if (ok) s->sem--;", "", "a timed acquire not taking the count passes")


if __name__ == "__main__":
    main()
