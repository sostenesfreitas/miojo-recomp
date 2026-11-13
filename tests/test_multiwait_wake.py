"""A WaitAny is woken only by an object some holder links into a multi-wait (host/nn_os_sync.c poke_multiwait). The actual
functions (the watched-address table, poke_multiwait, holder_watch, Link/Unlink/UnlinkAll/MoveAll) are compiled with a
harness that counts WakeAllConditionVariable and every object lookup. Every change anywhere woke every WaitAny before.

python -I -B tests/test_multiwait_wake.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long LONG;
typedef struct { int x; } SRWLOCK;
typedef struct { int x; } CONDITION_VARIABLE;
#define SRWLOCK_INIT {0}
#define CONDITION_VARIABLE_INIT {0}
static int lock_depth, lookups_locked, target_lookups, wakes;
static int fail_next_realloc;
static void *test_realloc(void *p, size_t n) {
    if (fail_next_realloc) { fail_next_realloc = 0; return NULL; }
    return realloc(p, n);
}
#define realloc test_realloc
static LONG InterlockedCompareExchange(volatile LONG *p, LONG x, LONG c) { LONG v = *p; if (v == c) *p = x; return v; }
static void *mw_cv_at, *mw_lock_at;   /* (set in main) */
static int mw_held;                  /* (mw_lock itself held: any sleep then must let it go) */
static void AcquireSRWLockExclusive(SRWLOCK *l) { lock_depth++; if ((void *)l == mw_lock_at) mw_held++; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { lock_depth--; if ((void *)l == mw_lock_at) mw_held--; }
static void WakeAllConditionVariable(CONDITION_VARIABLE *cv) { (void)cv; wakes++; }
typedef struct Obj {
    uint64_t key, event; int sem_max, kind; struct Obj **links; int nlinks, caplinks; struct Obj *mw;
    int64_t due_ns, sig_ns; int sem, qlen, qcap, signaled, autoclear; SRWLOCK lk; CONDITION_VARIABLE cv;
} Obj;
typedef unsigned long DWORD;
#define INFINITE 0xFFFFFFFFu
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static LONG InterlockedDecrement(volatile LONG *p) { return --*p; }
static int64_t hos_now_ns(void) { static int64_t t = 1000; return t += 100000; }   /* (0.1 ms a call: timed waits end) */
/* (scheduling points and blocking waits: counted when a lock is held; a sleep there never ends in this fixture) */
static int sched_locked;
static void (*on_sleep)(void);
static int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    (void)cv; (void)lk; (void)ms;
    if (lock_depth > 1) { puts("FAIL: a sleep under mw_lock (WaitAny's scan)"); exit(1); }
    if (mw_held && lk != mw_lock_at) { puts("FAIL: a sleep with mw_lock held that does not let mw_lock go"); exit(1); }
    if (on_sleep) on_sleep();   /* (an untimed WaitAny: something gets signaled while it sleeps) */
    return 0;
}
static void hos_vcore_checkpoint(void) { if (lock_depth) sched_locked++; }
static void hos_vcore_yield(void) { if (lock_depth) sched_locked++; }
static int wake_lat_on(void) { return 1; }
static void wake_lat_note(int how, int64_t lat_ns) { (void)how; (void)lat_ns; }
/* (HOS_CHAIN off: host/hos.h) */
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL };
static volatile int hos_chain_on;
static void hos_chain_note(int kind, uint64_t key, uint64_t lr) { (void)kind; (void)key; (void)lr; }
#define HOS_CHAIN(kind, key) do { if (hos_chain_on) hos_chain_note((kind), (key), 0); } while (0)
enum { K_EVENT = 1, K_TIMER, K_SEMA, K_QUEUE, K_MULTIWAIT, K_HOLDER };
HOLDER_ENUM
static Obj objs[128];
static Obj *obj(uint64_t key, int kind) {   /* (the real one is a scheduling point; a target lookup can resurrect it) */
    if (lock_depth) lookups_locked++;
    if (key < 8) target_lookups++;
    objs[key].key = key; objs[key].kind = kind;
    return &objs[key];
}
static Obj *obj_nockpt(uint64_t key, int kind) {   /* (no checkpoint: not a scheduling point) */
    objs[key].key = key; objs[key].kind = kind;
    return &objs[key];
}
static void spin_until_set(Obj *o, volatile int *flag) { (void)o; (void)flag; if (lock_depth) sched_locked++; }
static int wait_until(Obj *o, int64_t deadline) { (void)o; (void)deadline; if (lock_depth) sched_locked++; return 0; }
typedef struct { uint64_t x[8]; } Ctx;
#define ARG(i) (c->x[i])
#define HLE(name) static void name(Ctx *c)
static SRWLOCK mw_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE mw_cv = CONDITION_VARIABLE_INIT;
static volatile LONG mw_waiting;
FUNCTIONS
static int bad;
static int count(uint64_t k) { int i = mw_watched_at_locked(k); return i < 0 ? 0 : mw_watched[i].n; }
static void expect(const char *step, int e1, int e2) {
    if (count(1) != e1 || count(2) != e2) {
        printf("FAIL after %s: event1 watched by %d (want %d), event2 by %d (want %d)\n", step, count(1), e1, count(2), e2);
        bad = 1;
    }
}
static int poked(uint64_t k, int waiting) { mw_waiting = waiting; wakes = 0; poke_multiwait(&objs[k]); return wakes; }
static void call(void (*f)(Ctx *), uint64_t a, uint64_t b) { Ctx c = {{a, b}}; f(&c); }
int main(int argc, char **argv) {
    mw_cv_at = &mw_cv, mw_lock_at = &mw_lock;
    const int all = argc > 1;   /* (HOS_MW_WAKE=all set by the caller) */
    const uint64_t A = 10, B = 11, h1 = 20, h2 = 21, h3 = 22, h4 = 23;
    objs[1].key = 1; objs[2].key = 2;
    objs[h1].event = 1; objs[h1].sem_max = H_EVENT;   /* (two holders over event 1, one over event 2, one over a thread) */
    objs[h2].event = 1; objs[h2].sem_max = H_EVENT;
    objs[h3].event = 2; objs[h3].sem_max = H_EVENT;
    objs[h4].event = 3; objs[h4].sem_max = H_THREAD;
    if (poked(1, 1) != all) { puts("FAIL: an unwatched change woke WaitAny"); bad = 1; }
    for (uint64_t h = h1; h <= h4; h++) call(LINK, A, h);
    expect("four links", 2, 1);
    if (poked(1, 1) != 1 || poked(2, 1) != 1) { puts("FAIL: a watched change did not wake WaitAny"); bad = 1; }
    if (poked(5, 1) != all) { puts("FAIL: an unwatched change woke WaitAny with holders linked"); bad = 1; }
    if (poked(1, 0)) { puts("FAIL: a wake with no WaitAny waiting"); bad = 1; }
    call(UNLINK, h2, 0);
    expect("unlink one", 1, 1);
    call(UNLINK, h2, 0);
    expect("unlink it again", 1, 1);
    memset(&objs[1], 0, sizeof objs[1]);   /* (event 1 initialized again, as fresh() does, its holder still linked) */
    objs[1].key = 1;
    if (poked(1, 1) != 1) { puts("FAIL: a re-initialized watched event no longer wakes WaitAny"); bad = 1; }
    call(MOVEALL, B, A);
    expect("move all", 1, 1);
    if (objs[h1].mw != &objs[B] || objs[A].nlinks) { puts("FAIL: move all did not move the holders"); bad = 1; }
    objs[2].kind = 0;   /* (event 2 finalized before its holder is unlinked) */
    call(UNLINKALL, B, 0);
    expect("unlink all", 0, 0);
    if (objs[2].kind) { puts("FAIL: unlink brought a finalized event back"); bad = 1; }
    call(LINK, A, h1);
    call(UNLINKALL, B, 0);
    expect("link again, unlink all of the other", 1, 0);
    call(UNLINK, h1, 0);
    expect("unlink the last", 0, 0);
    if (mw_nwatched) { printf("FAIL: %d addresses still watched\n", mw_nwatched); bad = 1; }
    /* Keep the holder array allocated so the injected failure hits the watched-address table. */
    objs[40].links = (Obj **)realloc(NULL, 32 * sizeof *objs[40].links); objs[40].caplinks = 32;
    for (uint64_t k = 1; k <= 17; k++) {
        objs[k].key = k; objs[64 + k].event = k; objs[64 + k].sem_max = H_EVENT;
        if (k <= 16) call(LINK, 40, 64 + k);
    }
    fail_next_realloc = 1;
    call(LINK, 40, 81);
    {   /* WaitAny's scan holds mw_lock: a lookup there that is a scheduling point (obj: a vcore checkpoint) can give the
           core up while the lock is held, and poke_multiwait's signallers, holding cores, then block on mw_lock until the
           250 ms vcore escape. TryWaitAny over a holder of every kind holder_ready looks up */
        const int before = lookups_locked;
        const int kinds[] = {H_EVENT, H_TIMER, H_SEMA, H_QUEUE_NOT_EMPTY, H_QUEUE_NOT_FULL};
        for (int k = 0; k < 5; k++) {
            objs[100 + k].event = 110 + k; objs[100 + k].sem_max = kinds[k];
            call(LINK, 42, 100 + k);
        }
        objs[111].due_ns = (int64_t)1 << 60;   /* (a timer not yet due: the scan reads its deadline too) */
        /* (every state of event_wait's try path the scan can meet: the event manual or auto-clear, unset (try, 5 ms timed,
           and untimed until it is set during a sleep) and set (try); mw_lock is held throughout each scan) */
        for (int ac = 0; ac < 2; ac++) {
            objs[110].signaled = 0, objs[110].autoclear = ac;
            if (wait_any(42, 0)) { puts("FAIL: TryWaitAny found a holder ready when none was"); bad = 1; }
            if (wait_any(42, 5000000)) { puts("FAIL: a timed WaitAny found a holder ready when none was"); bad = 1; }
            static int sleeps;
            sleeps = 0;
            on_sleep = [] { if (++sleeps == 3) objs[110].signaled = 1; };
            if (wait_any(42, -1) != 100 || sleeps < 3) { puts("FAIL: an untimed WaitAny did not return the event set while it slept"); bad = 1; }
            on_sleep = nullptr;
            objs[110].signaled = 1;
            if (wait_any(42, 0) != 100) { puts("FAIL: TryWaitAny missed the set event's holder"); bad = 1; }
            if (objs[110].signaled != !ac) { puts("FAIL: an auto-clear event not cleared by TryWaitAny"); bad = 1; }
        }
        objs[110].signaled = 0, objs[110].autoclear = 0;
        if (sched_locked) { printf("FAIL: WaitAny's scan reached %d spins, timed waits or vcore calls under mw_lock\n", sched_locked); bad = 1; }
        if (lookups_locked != before) {
            printf("FAIL: WaitAny's scan made %d lookups that are scheduling points under mw_lock\n", lookups_locked - before);
            bad = 1;
        }
        call(UNLINKALL, 42, 0);
    }
    if (fail_next_realloc || poked(17, 1) != 1) { puts("FAIL: failed watch registration lost a wake"); bad = 1; }
    call(UNLINK, 65, 0);   /* free a table slot, then link a second holder over address 17 */
    objs[82].event = 17; objs[82].sem_max = H_EVENT;
    call(LINK, 40, 82);
    call(UNLINK, 81, 0);
    if (poked(17, 1) != 1) { puts("FAIL: unlink of an uncounted holder hid another live holder"); bad = 1; }
    call(UNLINKALL, 40, 0);
    if (mw_nwatched) { puts("FAIL: watches remain after OOM-case cleanup"); bad = 1; }
    if (target_lookups) { printf("FAIL: %d lookups of a watched object (link, unlink, poke look up none)\n", target_lookups); bad = 1; }
    if (lookups_locked) { printf("FAIL: %d lookups (scheduling points) under mw_lock\n", lookups_locked); bad = 1; }
    puts(bad ? "FAILED" : "ok");
    return bad;
}
"""


def fixture(source):
    table = source[source.index("typedef struct { uint64_t key; int n; } MwWatched;"):source.index("/* o: the object that changed")]
    parts = [table, block(source, "static void poke_multiwait("), block(source, "static uint64_t holder_watch(")]
    defines = []
    for alias, name in (("LINK", "LinkMultiWaitHolder"), ("UNLINK", "UnlinkMultiWaitHolder"),
                        ("UNLINKALL", "UnlinkAllMultiWaitHolder"), ("MOVEALL", "MoveAllMultiWaitHolder")):
        marker = re.search(r"HLE\((_ZN2nn2os\d+%sE\w*)\)" % name, source)
        parts.append(block(source, marker.group(0)))
        defines.append("#define %s %s" % (alias, marker.group(1)))
    parts.append(block(source, "static int event_wait(Obj *e, int64_t timeout_ns, int try_only) {"))   # (the real one)
    if "static int event_peek(Obj *e) {" in source:
        parts.append(block(source, "static int event_peek(Obj *e) {"))
    parts += [block(source, "static int holder_ready("), block(source, "static uint64_t wait_any(")]
    enum = re.search(r"enum \{ H_EVENT[^}]*\};", source).group(0)
    return HARNESS.replace("HOLDER_ENUM", enum).replace("FUNCTIONS", "\n".join(parts + defines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_os_sync.c")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")

    # (scope: a "scheduling point" is a call that can give the virtual core up or wait for one, or put the thread to sleep
    # (condition or timed wait, Sleep): what turns mw_lock into the 250 ms vcore-escape stall. Out of scope: a short CPU-pause
    # spin, and blocking on a host lock (an SRW lock keeps the core) whose holders never give the core up while holding it,
    # as table_lock, which obj_lookup already takes on a cache miss.
    # the fixture stubs obj_nockpt: everything it can reach in this file runs under mw_lock during WaitAny's scan, so no
    # function in that call graph may hold a virtual-core call, yield, sleep or wait. event_wait is run for real in the
    # fixture, whose stubs count spins, timed waits and vcore calls under the lock and fail on a sleep. Controls below)
    sched = re.compile(r"\b(hos_vcore_\w+|vc_\w+|hos_cv_sleep|hos_sleep\w*|hos_thread_wait|hos_yield\w*|Sleep\w*|"
                       r"SwitchToThread|WaitFor\w+|WaitOnAddress|sched_yield|usleep|nanosleep|wait_until|spin_\w+)\s*\(")

    # (an allowlist, not a list of bad names: every call in wait_any's mw_lock region and in what it reaches must be to a
    # function of this file (followed) or to one of these, which keep the core and never sleep; anything else, a wrapper in
    # another file included, fails)
    safe_external = {"AcquireSRWLockExclusive", "AcquireSRWLockShared", "ReleaseSRWLockExclusive", "ReleaseSRWLockShared",
                     "InitializeSRWLock", "InitializeConditionVariable", "InterlockedIncrement", "InterlockedDecrement",
                     "InterlockedCompareExchange", "InterlockedExchange", "__atomic_load_n", "__atomic_store_n",
                     "malloc", "calloc", "realloc", "free", "memset", "memcpy", "memmove", "getenv", "env_on", "hos_now_ns", "offsetof"}
    not_calls = {"if", "for", "while", "switch", "return", "sizeof", "case", "SLEEP_LETS_MW_LOCK_GO"}

    def code_of(s):   # (comments blanked: words in them are not calls)
        return re.sub(r"/\*.*?\*/|//[^\n]*", " ", s, flags=re.S)

    def lookup_graph_unsafe(text):
        defs = {}   # (every definition of a name, e.g. one per platform behind #if, is followed)
        # (a definition is any 'name(params) {', whatever is before it: static or not, attributes, indentation; params may
        # hold one level of parentheses; keywords are not functions)
        code = code_of(text)
        for m in re.finditer(r"\b(\w+)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*\{", code):
            if m.group(1) not in ("if", "for", "while", "switch", "return", "sizeof"):
                defs.setdefault(m.group(1), []).append(block(code[m.start():], m.group(0)))   # (this one, not the first alike)
        # (wait_any's own mw_lock region: no call but its one sleep, which lets mw_lock go (the fixture checks that))
        wa = "\n".join(defs.get("wait_any", []))
        region = wa[wa.find("AcquireSRWLockExclusive(&mw_lock)"):wa.rfind("ReleaseSRWLockExclusive(&mw_lock)")]
        if not region:
            return ["wait_any (its mw_lock region was not found)"]
        rest = re.sub(r"\bhos_cv_sleep\s*\(\s*&mw_cv\s*,\s*&mw_lock\s*,", "SLEEP_LETS_MW_LOCK_GO(", region, count=1)
        bad, seen, todo = [], set(), [("wait_any's mw_lock region", rest), ("holder_ready", None)]
        while todo:
            name, body = todo.pop()
            if body is None:
                if name in seen or name not in defs:
                    continue
                seen.add(name)
                body = "\n".join(defs[name])
            # (a call through a pointer, a member or an array element: (*f)(), p->f(), s.f(), f[i]())
            if re.search(r"\(\s*\*[^()]*\)\s*\(|(->|\.)\s*\w+\s*\(|\]\s*\(", body):
                bad.append("%s (an indirect call)" % name)
            for c in re.findall(r"\b(\w+)\s*\(", body):
                if c in defs:
                    todo.append((c, None))
                elif c not in safe_external and c not in not_calls:
                    bad.append("%s (calls %s)" % (name, c))
        if "holder_ready" not in seen:
            bad.append("holder_ready (not found)")
        return sorted(set(bad))

    # (file-wide: the scan also takes table_lock and the objects' lk. A thread anywhere in this file that waits for a
    # virtual core while holding one of those (or mw_lock) can hold the scan up the same way. So no region holding them,
    # nor anything of this file it reaches, may call something that waits for a core. hos_cv_sleep is fine: it lets the
    # lock go before taking a core back)
    core_wait = re.compile(r"\b(hos_vcore_\w+|vc_\w+|a64_poll\w*|hos_yield\w*|hos_thread_wait|hos_sleep\w*|Sleep\w*|"
                           r"SwitchToThread|sched_yield)\s*\(")
    scan_lock = re.compile(r"AcquireSRWLock(?:Exclusive|Shared)\(\s*&\s*(mw_lock|table_lock|\w+\s*->\s*lk)\s*\)")

    def locked_regions_unsafe(text):
        code = code_of(text)
        defs = {}
        for m in re.finditer(r"\b(\w+)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*\{", code):
            if m.group(1) not in ("if", "for", "while", "switch", "return", "sizeof"):
                defs.setdefault(m.group(1), []).append(block(code[m.start():], m.group(0)))
        waits = {}   # (does a function of this file wait for a core, itself or through what it calls)

        def waits_for_core(name, stack=()):
            if name not in waits:
                waits[name] = False if name in stack else any(
                    core_wait.search(b) or any(c in defs and waits_for_core(c, stack + (name,))
                                               for c in re.findall(r"\b(\w+)\s*\(", b))
                    for b in defs[name])
            return waits[name]
        bad = []
        for name, bodies in defs.items():
            for b in bodies:
                for m in scan_lock.finditer(b):
                    lk = re.sub(r"\s", "", m.group(1))
                    name_pat = r"\s*->\s*".join(re.escape(part) for part in lk.split("->"))   # (t->lk: any spacing)
                    rel = re.search(r"ReleaseSRWLock(?:Exclusive|Shared)\(\s*&\s*" + name_pat + r"\s*\)", b[m.end():])
                    region = b[m.end():m.end() + rel.start()] if rel else b[m.end():]
                    if core_wait.search(region) or any(c in defs and waits_for_core(c) for c in re.findall(r"\b(\w+)\s*\(", region)):
                        bad.append("%s (holding %s)" % (name, lk))
        return sorted(set(bad))

    unsafe = locked_regions_unsafe(source)
    if unsafe:
        raise SystemExit("FAIL: waits for a virtual core while holding a lock WaitAny's scan takes: " + ", ".join(unsafe))
    for what, old, new in (("a checkpoint in event_signal holding e->lk", "    AcquireSRWLockExclusive(&e->lk);\n    e->signaled = 1;",
                            "    AcquireSRWLockExclusive(&e->lk);\n    hos_vcore_checkpoint();\n    e->signaled = 1;"),
                           ("an obj() lookup under table_lock", "static Obj *obj_lookup(uint64_t key, int kind) {",
                            "static Obj *obj_lookup(uint64_t key, int kind) { AcquireSRWLockShared(&table_lock); obj(1, 2); ReleaseSRWLockShared(&table_lock);")):
        if old not in source:
            raise SystemExit("FAIL: the locked-region check's control '%s' lost its anchor: update the test" % what)
        if not locked_regions_unsafe(source.replace(old, new, 1)):
            raise SystemExit("FAIL: the locked-region check missed " + what)

    unsafe = lookup_graph_unsafe(source)
    if unsafe:
        raise SystemExit("FAIL: reached from WaitAny's scan under mw_lock, a scheduling point in " + ", ".join(unsafe))
    # (controls: each must be caught; a control whose anchor is gone fails too, so a reshaped function updates the test)
    callee = re.search(r"^static [^;{}()]*?\bfind_unlocked\s*\([^;{]*\)\s*\{", source, re.M)
    controls = (("a checkpoint in obj_nockpt", "static Obj *obj_nockpt(uint64_t key, int kind) {",
                 "static Obj *obj_nockpt(uint64_t key, int kind) { hos_vcore_checkpoint();"),
                ("a vcore yield in obj_nockpt", "static Obj *obj_nockpt(uint64_t key, int kind) {",
                 "static Obj *obj_nockpt(uint64_t key, int kind) { hos_vcore_yield();"),
                ("a sleep in obj_lookup", "static Obj *obj_lookup(uint64_t key, int kind) {",
                 "static Obj *obj_lookup(uint64_t key, int kind) { Sleep(1);"),
                ("a call through a pointer in obj_nockpt", "static Obj *obj_nockpt(uint64_t key, int kind) {",
                 "static Obj *obj_nockpt(uint64_t key, int kind) { (*lookup_hook)();"),
                ("a call through an array element in obj_nockpt", "static Obj *obj_nockpt(uint64_t key, int kind) {",
                 "static Obj *obj_nockpt(uint64_t key, int kind) { lookup_observers[0]();"),
                ("event_wait back in the scan", "return event_peek(obj_nockpt(h->event, K_EVENT));",
                 "return event_wait(obj_nockpt(h->event, K_EVENT), 0, 1);"),
                ("a checkpoint in event_peek", "static int event_peek(Obj *e) {",
                 "static int event_peek(Obj *e) { hos_vcore_checkpoint();"),
                ("a periodic checkpoint in WaitAny's holder loop", "for (int i = 0; i < mw->nlinks && !found; i++) {",
                 "for (int i = 0; i < mw->nlinks && !found; i++) { if (i && (i & 63) == 0) hos_vcore_checkpoint();"),
                ("a second sleep in WaitAny's mw_lock region", "        if (found) break;",
                 "        if (found) break;\n        hos_cv_sleep(&mw_cv, &mw_lock, 0);"),
                ("a checkpoint in find_unlocked", callee.group(0) if callee else "static ... find_unlocked(...) {",
                 (callee.group(0) + " hos_vcore_checkpoint();") if callee else ""))
    for what, old, new in controls:
        if old not in source:
            raise SystemExit("FAIL: the check's control '%s' lost its anchor %r: update the test" % (what, old[:60]))
        if not lookup_graph_unsafe(source.replace(old, new, 1)):
            raise SystemExit("FAIL: the check missed " + what)
    second = source + "\n#if 0\nstatic Obj *find_unlocked(uint64_t key, int kind) { hos_vcore_checkpoint(); return 0; }\n#endif\n"
    if callee and not lookup_graph_unsafe(second):
        raise SystemExit("FAIL: the check missed a checkpoint in a second (#else) find_unlocked")

    calls = re.findall(r"poke_multiwait\(([^)]*)\);", source)   # (every change passes the object that changed)
    assert calls and all(re.fullmatch(r"\w+", c) for c in calls), "a poke_multiwait call without the object: %r" % calls

    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="multiwait-wake-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture(source), encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([compiler, "-std=c++17", "-include", ENV_ON_H, "-O1", "-w", str(folder / "fixture.cpp"), "-o", str(exe)], check=True,
                       timeout=120)
        env = os.environ.copy()
        for mode in ("targeted", "all"):
            env.pop("HOS_MW_WAKE", None)
            argv = [str(exe)]
            if mode == "all":
                env["HOS_MW_WAKE"] = "all"
                argv.append("all")
            result = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=30)
            print(mode + ":", result.stdout.strip())
            if result.returncode:
                raise SystemExit("FAIL: " + mode)
    print("PASS: WaitAny woken only for watched addresses (HOS_MW_WAKE=all: every change); link, unlink, unlink all and move all"
          " keep the counts by address; a re-initialized watched event still wakes; finalize-before-unlink resurrects nothing;"
          " no watched-target lookups or lookups under the extracted paths' mw_lock; allocation failure preserves wakes")


if __name__ == "__main__":
    main()
