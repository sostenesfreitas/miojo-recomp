"""An nn::os call about the calling thread's own ThreadType finds its record without the scan of every thread.

host/hos_thread.c's thread_arg (GetThreadPriority, GetThreadId, ChangeThreadPriority...) answered every call by
hos_thread_of: a linear scan of all threads under threads_lock. The game asks about its own thread most of the time
(GetThreadPriority(GetCurrentThread()), ~2,400 times a frame in Hyrule): the calling thread's record answers that,
compared with its current ThreadType (the main thread's changes when it adopts the SDK's, hos_thread_adopt_guest).

The fixture runs the real thread_arg over a stub scan that counts its calls.

python -I -B tests/test_thread_arg_self.py [--cc gcc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
MARKER = "static HosThread *thread_arg(Ctx *c, uint64_t tt) {"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t x[8]; } Ctx;
typedef struct HosThread { uint64_t guest_tt; } HosThread;
static HosThread a = {0x100}, b = {0x200};
static HosThread *cur;
static int scans, fatals;
static HosThread *hos_thread_of(uint64_t tt) {
    scans++;
    return tt == a.guest_tt ? &a : tt == b.guest_tt ? &b : NULL;
}
static void hos_fatal(Ctx *c, const char *fmt, ...) { (void)c, (void)fmt; fatals++; }

@THREAD_ARG@

int main(void) {
    cur = &a;
    CHECK(thread_arg(NULL, 0x100) == &a && scans == 0);   /* (its own: no scan) */
    CHECK(thread_arg(NULL, 0x200) == &b && scans == 1);   /* (another thread's: the scan) */
    a.guest_tt = 0x300;                                   /* (adopted the SDK's ThreadType: the current one counts) */
    CHECK(thread_arg(NULL, 0x300) == &a && scans == 1);
    CHECK(thread_arg(NULL, 0x100) == NULL && scans == 2 && fatals == 1);   /* (the old one is unknown now) */
    cur = NULL;                                           /* (a host thread: the scan) */
    CHECK(thread_arg(NULL, 0x200) == &b && scans == 3);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "hos_thread.c").read_text(encoding="utf-8")
    check(MARKER in src, "host/hos_thread.c has no '%s'" % MARKER)
    start = src.index(MARKER)
    part = src[start:start + re.search(r"^}", src[start:], re.M).end()]
    check("HLE(_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE) { RET((uint32_t)hos_thread_priority(thread_arg(c, ARG(0))" in src,
          "GetThreadPriority no longer finds its thread through thread_arg")

    def run(tmp, name, body):
        c, exe = Path(tmp) / (name + ".c"), Path(tmp) / (name + ".exe")
        c.write_text(FIXTURE.replace("@THREAD_ARG@", body), encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-but-set-variable", "-Wno-unknown-warning-option", "-Wno-unused-but-set-global", str(c), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], capture_output=True, text=True)

    fast = "if (cur && cur->guest_tt == tt) return cur;"
    with tempfile.TemporaryDirectory(prefix="thread-arg-") as tmp:
        r = run(tmp, "fixture", part)
        check(r.returncode == 0, "thread_arg: not as expected\n" + r.stderr)
        check(fast in part, "thread_arg no longer answers the calling thread's own ThreadType first")
        for name, new, what in (("scan", "", "every call scanning passes"),
                                ("any", "if (cur) return cur;", "the calling thread answering for any ThreadType passes")):
            r = run(tmp, name, part.replace(fast, new))
            check(r.returncode != 0 and "FAIL:" in r.stderr, what)


if __name__ == "__main__":
    main()
