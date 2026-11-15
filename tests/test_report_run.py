"""One cached system-random report id per process; independent of crash/stall code."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include "hos_report_run.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define THREAD_RESULT DWORD WINAPI
#define YIELD() Sleep(0)
#else
#include <pthread.h>
#include <sched.h>
#define THREAD_RESULT void *
#define YIELD() sched_yield()
#endif
static int waiting, go, attempts, winners;
static THREAD_RESULT compete(void *unused) {
    (void)unused;
    __atomic_add_fetch(&waiting, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) YIELD();
    const int acquired = hos_report_dbghelp_try();
    if (acquired) __atomic_add_fetch(&winners, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&attempts, 1, __ATOMIC_RELEASE);
    if (acquired) {
        while (__atomic_load_n(&attempts, __ATOMIC_ACQUIRE) != 2) YIELD();
        hos_report_dbghelp_done();
    }
    return 0;
}
int main(void) {
    const char *id = hos_report_run_id();
    assert(strlen(id) == 32);
    assert(id == hos_report_run_id());
#ifdef _WIN32
    HANDLE a = CreateThread(NULL, 0, compete, NULL, 0, NULL), b = CreateThread(NULL, 0, compete, NULL, 0, NULL);
    assert(a && b);
#else
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, compete, NULL) && !pthread_create(&b, NULL, compete, NULL));
#endif
    while (__atomic_load_n(&waiting, __ATOMIC_ACQUIRE) != 2) YIELD();
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
#ifdef _WIN32
    assert(WaitForSingleObject(a, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(b, 5000) == WAIT_OBJECT_0);
    CloseHandle(a); CloseHandle(b);
#else
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
#endif
    assert(winners == 1 && attempts == 2);
    assert(hos_report_dbghelp_try() && !hos_report_dbghelp_try());
    hos_report_dbghelp_done();
    puts(id);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("gcc") or "clang")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-report-run-") as tmp:
        src, exe = Path(tmp) / "run.c", Path(tmp) / "run.exe"
        src.write_text(FIXTURE, encoding="utf-8")
        cmd = [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"),
               str(src), str(ROOT / "host/hos_report_run.c"), "-o", str(exe)]
        if os.name == "nt":
            cmd += ["-ladvapi32"]
        else:
            cmd += ["-pthread"]
        subprocess.run(cmd, check=True, timeout=60)
        ids = [subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=10).stdout.strip()
               for _ in range(2)]
        assert all(re.fullmatch(r"[0-9a-f]{32}", value) for value in ids)
        assert ids[0] != ids[1], ids
    print("PASS: two starts have different 128-bit ids; each process keeps its original id")
    print("PASS: racing DbgHelp workers have exactly one nonblocking winner; release allows the next collection")


if __name__ == "__main__":
    main()
