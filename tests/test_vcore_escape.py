"""A guest thread that escaped a long wait for a virtual core (host/hos_thread.c vc_acquire: it runs without one) takes a
core again after its next block, like every other thread (issue 1929). The actual HosThread, hos_thread_core and the whole
virtual-core block (from vc_lock to hos_cv_sleep) are compiled with a single-threaded harness: a clock the waits advance,
no-op locks, a log that counts escapes. Before, hos_vcore_away answered 0 for the escaped thread (it held no core to give
up), so its `if (away) hos_vcore_back();` never ran and the thread kept running with no core, and with the core number of
a core another thread held, for the rest of its life. A hos_vcore_back without its away (a thread's start) ends an escape
too: a flag left up while the thread holds a core would make an inner block's away answer 1 and take a core in the
middle of the outer block.

python -I -B tests/test_vcore_escape.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + (";" if source[end:end + 1] == ";" else "")


HARNESS = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *HANDLE;
typedef int BOOL;
typedef unsigned long DWORD;
typedef struct { void *p; } SRWLOCK, *PSRWLOCK;
typedef struct { void *p; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define SRWLOCK_INIT {0}
#define HOS_TLS_SLOTS 64
typedef struct { uint64_t x[32]; volatile int poll; } Ctx;
static int64_t now_ns = 1000000000;
static int64_t hos_now_ns(void) { return now_ns; }
enum { HOS_CHAIN_WAIT = 1, HOS_CHAIN_WOKE, HOS_CHAIN_SIGNAL, HOS_CHAIN_SLEEP, HOS_CHAIN_AWAKE, HOS_CHAIN_PRESENT };
#define HOS_CHAIN(kind, key) ((void)(kind), (void)(key))   /* (HOS_CHAIN off: host/hos.h) */
static int escapes_logged;
static void hos_log(const char *fmt, ...) { if (strstr(fmt, "ran without a core")) escapes_logged++; }
static int hos_cores(void) { return 2; }
static int sleeps;
/* (no other thread runs here: a wait for a core only lets the clock go on, 1 ms a look, as the real one times out) */
static BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lk, DWORD ms, unsigned long f) {
    (void)cv, (void)lk, (void)f;
    now_ns += (int64_t)(ms ? ms : 1) * 1000000;
    sleeps++;
    return 1;
}
static void WakeConditionVariable(PCONDITION_VARIABLE cv) { (void)cv; }
static void AcquireSRWLockExclusive(PSRWLOCK l) { (void)l; }
static void ReleaseSRWLockExclusive(PSRWLOCK l) { (void)l; }
@STRUCT@
static HosThread *cur;
@CORE@
@VCORES@
static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
static void thread(HosThread *t, const char *name, int prio, uint64_t mask) {
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->priority = prio, t->core_mask = mask, t->ideal_core = -2;
}
/* the holders of both cores never give them up while t waits: t escapes; then one holder blocks */
static void escape(HosThread *t, HosThread *h0, HosThread *h1) {
    cur = h0; hos_vcore_back();
    cur = h1; hos_vcore_back();
    check(h0->vc_held && h1->vc_held, "the two holders hold the two cores");
    const int before = escapes_logged;
    cur = t; hos_vcore_back();
    check(escapes_logged == before + 1 && !t->vc_held, "the waiter escaped (ran without a core)");
    cur = h1; check(hos_vcore_away() == 1, "a holder gives its core up for a block");
    cur = t;
}
int main(void) {
    HosThread h0, h1, t, u;
    thread(&h0, "holder0", 16, 1), thread(&h1, "holder1", 16, 2), thread(&t, "waiter", 16, 3);

    escape(&t, &h0, &h1);   /* a block of the escaped thread (hos_sleep_ns, hos_thread_wait, the NVN waits, ...) */
    const int away = hos_vcore_away();
    check(away == 1, "hos_vcore_away tells an escaped thread to take a core after its block");
    if (away) hos_vcore_back();
    check(t.vc_held == 2, "after its block the escaped thread holds the free core");
    check(hos_thread_core(&t) == 1 && hos_thread_core(&t) != hos_thread_core(&h0),
          "and reports that core's number, not the one the other holder runs on");
    check(hos_vcore_away() == 1 && !t.vc_held, "and gives it up at its next block as every holder does");

    thread(&t, "waiter", 16, 3), thread(&h0, "holder0", 16, 1), thread(&h1, "holder1", 16, 2);
    memset(vc_owner, 0, sizeof vc_owner);
    escape(&t, &h0, &h1);   /* the same through hos_cv_sleep (the nn::os waits) */
    SRWLOCK lk = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = {0};
    hos_cv_sleep(&cv, &lk, 1);
    check(t.vc_held == 2, "after a hos_cv_sleep the escaped thread holds the free core");

    thread(&t, "waiter", 16, 3), thread(&h0, "holder0", 16, 1), thread(&h1, "holder1", 16, 2);
    memset(vc_owner, 0, sizeof vc_owner);
    escape(&t, &h0, &h1);   /* a block inside a block: only the outer one takes the core again */
    const int outer = hos_vcore_away();
    const int inner = hos_vcore_away();
    check(outer == 1 && inner == 0, "only the outermost block of an escaped thread takes a core after it");
    if (inner) hos_vcore_back();
    check(!t.vc_held, "no core is taken inside the outer block");
    if (outer) hos_vcore_back();
    check(t.vc_held == 2, "the core is taken after the outer block");

    thread(&t, "waiter", 16, 3), thread(&h0, "holder0", 16, 1), thread(&h1, "holder1", 16, 2);
    memset(vc_owner, 0, sizeof vc_owner);
    escape(&t, &h0, &h1);   /* a back without its away (as at a thread's start) takes the core: the escape is over */
    hos_vcore_back();
    check(t.vc_held == 2, "a back without an away gives the escaped thread the free core");
    const int outer2 = hos_vcore_away();
    const int inner2 = hos_vcore_away();
    check(outer2 == 1 && inner2 == 0, "after a back without an away, a block inside a block takes no core after it");
    if (inner2) hos_vcore_back();
    check(!t.vc_held, "no core is taken inside the outer block after a back without an away");
    if (outer2) hos_vcore_back();
    check(t.vc_held == 2, "the core is taken after the outer block");

    thread(&t, "waiter", 16, 3), thread(&h0, "holder0", 16, 1), thread(&h1, "holder1", 16, 2);
    memset(vc_owner, 0, sizeof vc_owner);
    escape(&t, &h0, &h1);   /* a back without its away that escapes again: that escape ends at the next block */
    cur = &h1; hos_vcore_back();
    cur = &t;
    const int before = escapes_logged;
    hos_vcore_back();
    check(escapes_logged == before + 1 && !t.vc_held, "a back without an away escapes again while both cores are held");
    check(hos_vcore_away() == 1, "and the thread still takes a core after its next block");

    thread(&u, "never-held", 16, 3);   /* a thread that holds no core and never escaped: nothing to take after a block */
    cur = &u;
    check(hos_vcore_away() == 0, "a thread that never escaped and holds no core takes none after a block");
    if (!bad) puts("ok");
    return bad;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/hos_thread.c")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    start = src.index("static SRWLOCK vc_lock = SRWLOCK_INIT;")
    end = src.index("\n#else", src.index("int hos_cv_sleep(void *cv, void *lk, unsigned ms) {", start))
    fixture = (HARNESS.replace("@STRUCT@", block(src, "struct HosThread {") + "\ntypedef struct HosThread HosThread;")
               .replace("@CORE@", block(src, "int hos_thread_core(HosThread *t) {"))
               .replace("@VCORES@", src[start:end]))
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="vcore-escape-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([compiler, "-std=c++17", "-O1", "-w", str(folder / "fixture.cpp"), "-o", str(exe)], check=True,
                       timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        if result.returncode or "ok" not in result.stdout:
            raise SystemExit("an escaped thread does not take a core again after its block (issue 1929)")


if __name__ == "__main__":
    main()
