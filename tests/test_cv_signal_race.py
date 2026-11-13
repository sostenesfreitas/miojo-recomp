"""SignalConditionVariable's no-waiter early return skips only signals made before a waiter started to wait.

host/nn_os_sync.c's Signal returns at once when the condition variable's waiter count (cv_waiting) reads 0. A waiter
counts itself in with cv_count_in before it takes cv->lk. Counted in later (inside cv_enter, under cv->lk), a signal
made without the guest mutex could read 0 while the waiter held cv->lk between its generation read and cv_enter, and
return; the old Signal waited for cv->lk there and woke it.

The fixture runs the real code cut from the source (handoff_on, cv_signal_always, cv_count_in, cv_enter, cv_leave,
cv_reserve, wait_until, the Signal, Wait and TimedWait HLEs) on threads, the Win32 lock and condition variable calls
mapped onto pthreads:
- the window: a hook holds the waiter 30 ms right after its generation read (cv->lk held), and a signaller that does not
  own the guest mutex signals then. The waiter must wake (WaitConditionVariable), and TimedWaitConditionVariable must
  report a signal, not its 5 s timeout. 20 rounds each, with and without HOS_CV_SIGNAL_ALWAYS=1.
- the usual case: 20,000 hand-offs, each signal made under the guest mutex, must never stall.
A waiter counted in under cv->lk (the waits' count moved after AcquireSRWLockExclusive) must fail.

python -I -B tests/test_cv_signal_race.py [--cc gcc] [--source host/nn_os_sync.c]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
WAIT = "HLE(_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE) {"
TIMED = "HLE(_ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE) {"
PARTS = {"@HANDOFF@": "static int handoff_on(void) {", "@ALWAYS@": "static int cv_signal_always(void) {",
         "@COUNT@": "static void cv_count_in(Obj *cv) {", "@ENTER@": "static void cv_enter(Obj *cv, Obj *m) {",
         "@LEAVE@": "static int cv_leave(Obj *cv, Obj *m) {", "@RESERVE@": "static void cv_reserve(Obj *cv) {",
         "@UNTIL@": "static int wait_until(Obj *o, int64_t deadline) {",
         "@SIGNAL@": "HLE(_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE) {", "@WAIT@": WAIT, "@TIMED@": TIMED}
OPTIONAL = {"@COUNT@"}   # (absent before the waiters were counted in before cv->lk: the fixture must then fail)
HOOK_AT = "uint64_t gen = cv->gen;\n"


def block(source, marker):
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''#define _POSIX_C_SOURCE 200809L
#define _CRT_SECURE_NO_WARNINGS
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hookshot_env.h"   /* (env_on: the switches' one rule) */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* (the Win32 calls the HLE uses, on pthreads) */
typedef pthread_mutex_t SRWLOCK;
typedef pthread_cond_t CONDITION_VARIABLE;
typedef unsigned DWORD;
#define INFINITE 0xFFFFFFFFu
static void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(l); }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(l); }
static void WakeConditionVariable(CONDITION_VARIABLE *cv) { pthread_cond_signal(cv); }
static int64_t hos_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    if (ms == INFINITE) return pthread_cond_wait((pthread_cond_t *)cv, (pthread_mutex_t *)lk) == 0;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) t.tv_sec++, t.tv_nsec -= 1000000000;
    return pthread_cond_timedwait((pthread_cond_t *)cv, (pthread_mutex_t *)lk, &t) == 0;
}
typedef struct Obj {
    uint64_t key;
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    uint64_t gen;
    int reserved;
    int cv_waiting, cv_in, cv_owed, cv_mixed;
    struct Obj *cv_mutex, *cv_owed_mutex[8];
    int count;
    pthread_mutex_t guest;   /* (the guest mutex the waits give back and take again) */
} Obj;
static Obj cvo, mo;
enum { K_COND = 2, K_MUTEX = 1 };
static Obj *obj(uint64_t key, int kind) { (void)kind; return key == 0x1000 ? &cvo : &mo; }
static uint64_t watch_cvs[8];
static int watched(uint64_t m) { (void)m; return 0; }
static int watched_cv(uint64_t cv) { (void)cv; return 0; }
static uint64_t watch_mutex(void) { return 0; }
typedef struct { uint64_t x[8]; } Ctx;
static void watch_log(const char *w, uint64_t m, uint64_t cv, Ctx *c, Obj *o) { (void)w, (void)m, (void)cv, (void)c, (void)o; }
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL };
#define HOS_CHAIN(kind, key) ((void)(kind), (void)(key))   /* (HOS_CHAIN off: host/hos.h) */
#define ARG(i) (c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(name) static void name(Ctx *c)
static int mutex_release_all(Obj *m) { const int n = m->count; m->count = 0; pthread_mutex_unlock(&m->guest); return n; }
static void mutex_lock_owed(Obj *m, uint64_t guest, int owed) {
    (void)guest;
    pthread_mutex_lock(&m->guest);
    if (owed) __atomic_sub_fetch(&m->reserved, 1, __ATOMIC_SEQ_CST);   /* (as mutex_take_owed does) */
    m->count = 1;
}
/* the window: the waiter holds cv->lk, its generation read done, not yet in cv_enter */
static volatile int hook_on, in_window;
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL); }
static void window_hook(void) { if (hook_on) { __atomic_store_n(&in_window, 1, __ATOMIC_SEQ_CST); sleep_ms(30); } }

@HANDOFF@
@ALWAYS@
@COUNT@
@ENTER@
@LEAVE@
@RESERVE@
@UNTIL@
@SIGNAL@
@WAIT@
@TIMED@

static volatile int done, timed, signalled_result;
static void *waiter(void *p) {
    (void)p;
    pthread_mutex_lock(&mo.guest);
    mo.count = 1;
    Ctx c = {{0x1000, 0x2000, 5000000000ull}};
    if (timed) {
        _ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE(&c);
        signalled_result = (int)c.x[0];
    } else {
        _ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE(&c);
    }
    pthread_mutex_unlock(&mo.guest);
    __atomic_store_n(&done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void *signaller(void *p) {   /* (does not own the guest mutex) */
    (void)p;
    while (!__atomic_load_n(&in_window, __ATOMIC_SEQ_CST)) sleep_ms(0);
    Ctx c = {{0x1000}};
    _ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE(&c);
    return NULL;
}
static int inited;
static void reset(void) {   /* (between rounds both threads are joined: the objects are destroyed before they are made again) */
    if (inited) {
        pthread_mutex_destroy(&cvo.lk), pthread_cond_destroy(&cvo.cv), pthread_mutex_destroy(&mo.guest);
        pthread_mutex_destroy(&mo.lk), pthread_cond_destroy(&mo.cv);
    }
    inited = 1;
    memset(&cvo, 0, sizeof cvo), memset(&mo, 0, sizeof mo);
    cvo.key = 0x1000, mo.key = 0x2000;
    pthread_mutex_init(&cvo.lk, NULL), pthread_cond_init(&cvo.cv, NULL), pthread_mutex_init(&mo.guest, NULL);
    pthread_mutex_init(&mo.lk, NULL), pthread_cond_init(&mo.cv, NULL);
    __atomic_store_n(&done, 0, __ATOMIC_SEQ_CST), __atomic_store_n(&in_window, 0, __ATOMIC_SEQ_CST);
    signalled_result = -1;
}
/* one round of the window: 1 if the waiter woke by the signal within 2 s (timed: reported a signal, not its timeout) */
static int window_round(int t) {
    reset();
    timed = t, hook_on = 1;
    pthread_t a, b;
    pthread_create(&a, NULL, waiter, NULL), pthread_create(&b, NULL, signaller, NULL);
    pthread_join(b, NULL);
    const int64_t until = hos_now_ns() + 2000000000ll;
    while (!__atomic_load_n(&done, __ATOMIC_SEQ_CST) && hos_now_ns() < until) sleep_ms(1);
    if (!__atomic_load_n(&done, __ATOMIC_SEQ_CST)) return 0;   /* (lost: the waiter sleeps on; the process exits with it) */
    pthread_join(a, NULL);
    return !t || signalled_result == 1;
}
/* the usual case: the signal made under the guest mutex, 20,000 hand-offs */
static volatile int flag, rounds_done;
static void *owned_waiter(void *p) {
    (void)p;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&mo.guest);
        mo.count = 1;
        while (!flag) {
            Ctx c = {{0x1000, 0x2000}};
            _ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE(&c);
        }
        flag = 0;
        pthread_mutex_unlock(&mo.guest);
        __atomic_store_n(&rounds_done, i + 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}
static int owned_rounds(void) {
    reset();
    hook_on = 0, flag = 0;
    __atomic_store_n(&rounds_done, 0, __ATOMIC_SEQ_CST);
    pthread_t a;
    pthread_create(&a, NULL, owned_waiter, NULL);
    for (int i = 0; i < 20000; i++) {
        const int64_t until = hos_now_ns() + 5000000000ll;
        for (;;) {
            pthread_mutex_lock(&mo.guest);
            const int free_slot = !flag && __atomic_load_n(&rounds_done, __ATOMIC_SEQ_CST) == i;
            if (free_slot) {
                flag = 1;
                Ctx c = {{0x1000}};
                _ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE(&c);
            }
            pthread_mutex_unlock(&mo.guest);
            if (free_slot) break;
            if (hos_now_ns() > until) return i;   /* (stalled) */
        }
    }
    const int64_t until = hos_now_ns() + 5000000000ll;
    while (__atomic_load_n(&rounds_done, __ATOMIC_SEQ_CST) < 20000 && hos_now_ns() < until) sleep_ms(1);
    const int n = __atomic_load_n(&rounds_done, __ATOMIC_SEQ_CST);
    if (n < 20000) return n;
    pthread_join(a, NULL);
    return 20000;
}

int main(void) {
    for (int i = 0; i < 20; i++) CHECK(window_round(0));   /* (WaitConditionVariable) */
    for (int i = 0; i < 20; i++) CHECK(window_round(1));   /* (TimedWaitConditionVariable) */
    CHECK(owned_rounds() == 20000);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--source", default=str(ROOT / "host" / "nn_os_sync.c"), help="(to check another version of the file)")
    a = p.parse_args()

    src = Path(a.source).read_text(encoding="utf-8")
    parts = {}
    for key, marker in PARTS.items():
        if marker not in src:
            check(key in OPTIONAL, "host/nn_os_sync.c has no '%s'" % marker)
            parts[key] = ""
            continue
        parts[key] = block(src, marker)
    for key in ("@WAIT@", "@TIMED@"):
        check(parts[key].count(HOOK_AT) == 1, "%s no longer reads the generation as '%s'" % (key, HOOK_AT.strip()))
        parts[key] = parts[key].replace(HOOK_AT, HOOK_AT + "    window_hook();\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}

    def build(tmp, name, **changed):
        text = FIXTURE
        for key, part in parts.items():
            text = text.replace(key, changed.get(key, part))
        c, exe = Path(tmp) / (name + ".c"), Path(tmp) / (name + ".exe")
        c.write_text(text, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-pthread", "-I",
                        str(ROOT / "gpu" / "vk" / "compat"), str(c), "-o", str(exe)], check=True)
        return exe

    def run(exe, always):
        e = dict(env) if always is None else dict(env, HOS_CV_SIGNAL_ALWAYS=always)
        return subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=300)

    with tempfile.TemporaryDirectory(prefix="cv-race-") as tmp:
        exe = build(tmp, "fixture")
        for always in (None, "1"):
            r = run(exe, always)
            check(r.returncode == 0, "HOS_CV_SIGNAL_ALWAYS %r: a wake was lost or a hand-off stalled\n%s" % (always, r.stderr))
        if parts["@COUNT@"]:
            # (the count taken under cv->lk with cv_enter, after the generation read, as before: the window's wake must be lost)
            moved = {k: parts[k].replace("    cv_count_in(cv);\n    AcquireSRWLockExclusive(&cv->lk);\n",
                                         "    AcquireSRWLockExclusive(&cv->lk);\n")
                                 .replace("    cv_enter(cv, m);\n", "    cv_count_in(cv);\n    cv_enter(cv, m);\n") for k in ("@WAIT@", "@TIMED@")}
            check(all(moved[k] != parts[k] for k in moved), "the waits no longer count the waiter in right before cv->lk")
            r = run(build(tmp, "counted-late", **moved), None)
            check(r.returncode != 0 and "FAIL:" in r.stderr, "a waiter counted in under cv->lk passes: the fixture misses the window")


if __name__ == "__main__":
    main()
