"""Exercise thread destruction from hos_thread.c with counted host handles, without a game.

Removing the wake CloseHandle must fail the repeated-lifecycle and free-order checks.
The registry removal is production code; only host locks, handles and free are instrumented.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

RUN_ALL_ARGS = ["--cc", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
COMPILER = None


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test, message) do { if (!(test)) { fprintf(stderr, "%s\n", message); exit(1); } } while (0)
typedef struct { int closed, kind; } HostHandle;
typedef HostHandle *HANDLE;
typedef struct { HANDLE handle, wake; } HosThread;
static HosThread *threads[1024], *destroying;
static int nthreads, threads_lock, lock_held, frees, thread_closes, wake_closes, check_free_order;

static void AcquireSRWLockExclusive(int *lock) {
    CHECK(lock == &threads_lock && !lock_held, "invalid registry lock acquisition");
    lock_held = 1;
}
static void ReleaseSRWLockExclusive(int *lock) {
    CHECK(lock == &threads_lock && lock_held, "invalid registry lock release");
    lock_held = 0;
}
static void check_unregistered(void) {
    CHECK(!lock_held, "resource released while registry lock held");
    for (int i = 0; i < nthreads; i++)
        CHECK(threads[i] != destroying, "destroyed thread remains registered");
}
static int CloseHandle(HANDLE handle) {
    CHECK(handle != NULL, "null handle closed");
    CHECK(!handle->closed, "handle closed more than once");
    CHECK(handle == destroying->handle || handle == destroying->wake, "wrong handle closed");
    check_unregistered();
    if (handle->kind == 1) thread_closes++;
    else {
        CHECK(!destroying->handle || destroying->handle->closed, "wake closed before thread handle");
        wake_closes++;
    }
    handle->closed = 1;
    return 1;
}
static void checked_free(void *memory) {
    CHECK(memory == destroying, "wrong thread freed");
    check_unregistered();
    if (check_free_order) {
        CHECK(!destroying->handle || destroying->handle->closed, "thread handle live at free");
        CHECK(!destroying->wake || destroying->wake->closed, "wake event live at free");
    }
    frees++;
    free(memory);
}
#define free checked_free
@DESTROY@
#undef free

static void cycle(int handles, int registered) {
    HostHandle thread = {0, 1}, wake = {0, 2};
    HosThread before = {NULL, NULL}, after = {NULL, NULL};
    destroying = (HosThread *)calloc(1, sizeof *destroying);
    CHECK(destroying != NULL, "allocation failed");
    if (handles & 1) destroying->handle = &thread;
    if (handles & 2) destroying->wake = &wake;
    threads[0] = &before;
    if (registered) { threads[1] = destroying; threads[2] = &after; nthreads = 3; }
    else { threads[1] = &after; nthreads = 2; }
    hos_thread_destroy(destroying);
    CHECK(nthreads == 2, "wrong registry size after destroy");
    CHECK(threads[0] == &before && threads[1] == &after, "surviving registry entries changed");
    destroying = NULL;
}
int main(int argc, char **argv) {
    CHECK(argc == 2, "expected test case");
    if (!strcmp(argv[1], "repeated")) {
        for (int i = 0; i < 10000; i++) cycle(3, 1);
        CHECK(frees == 10000 && thread_closes == 10000, "thread lifecycle count mismatch");
        CHECK(wake_closes == 10000, "wake event leak: expected 10000 closes");
    } else if (!strcmp(argv[1], "order")) {
        check_free_order = 1;
        cycle(3, 1);
        CHECK(frees == 1 && thread_closes == 1 && wake_closes == 1, "close count mismatch");
    } else if (!strcmp(argv[1], "nulls")) {
        check_free_order = 1;
        for (int registered = 0; registered < 2; registered++)
            for (int handles = 0; handles < 4; handles++) cycle(handles, registered);
        CHECK(frees == 8 && thread_closes == 4 && wake_closes == 4, "null-handle lifecycle count mismatch");
    } else CHECK(0, "unknown test case");
    puts("PASS: thread resources closed once and registry preserved");
    return 0;
}
'''


class ThreadWakeCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = COMPILER or shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            raise unittest.SkipTest("no C compiler")
        cls.temporary = tempfile.TemporaryDirectory(prefix="hookshot-thread-cleanup-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        production = (ROOT / "host/hos_thread.c").read_text(encoding="utf-8")
        source = directory / "thread_cleanup.c"
        source.write_text(FIXTURE.replace("@DESTROY@", block(production, "void hos_thread_destroy(")),
                          encoding="utf-8")
        cls.executable = directory / "check.exe"
        result = subprocess.run([compiler, "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                 str(source), "-o", str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_repeated_lifecycles_release_all_wake_events(self):
        self.run_case("repeated")

    def test_handles_close_before_owner_is_freed(self):
        self.run_case("order")

    def test_null_handles_and_registered_or_unregistered_threads(self):
        self.run_case("nulls")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    args, rest = parser.parse_known_args()
    COMPILER = args.cc
    unittest.main(argv=[__file__] + rest)
