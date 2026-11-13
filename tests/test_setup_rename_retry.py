"""builder/setup/setup.c's move_retry, cut out of the source and run with a MoveFileW and a Sleep of the test's own: a move Windows
refuses with access denied or a sharing violation is tried again for up to 20 s, any other error returns at once and is kept

python -I -B tests/test_setup_rename_retry.py
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cc", "gcc"]

FIXTURE = r'''
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int BOOL;
typedef unsigned long DWORD;
#define TRUE 1
#define FALSE 0
#define ERROR_FILE_NOT_FOUND 2UL
#define ERROR_PATH_NOT_FOUND 3UL
#define ERROR_ACCESS_DENIED 5UL
#define ERROR_SHARING_VIOLATION 32UL
static DWORD last_error, held_error;
static int calls, waits, held;
static DWORD GetLastError(void) { return last_error; }
static BOOL MoveFileW(const wchar_t *from, const wchar_t *to) {
    calls++;
    if (wcscmp(from, L"from") || wcscmp(to, L"to")) { last_error = ERROR_FILE_NOT_FOUND; return FALSE; }
    if (held < 0 || calls <= held) { last_error = held_error; return FALSE; }
    return TRUE;
}
static void Sleep(DWORD ms) { if (ms == 250) waits++; last_error = 0; }
/* MOVE_RETRY */
static int check(const char *name, DWORD error, int times, BOOL want, int want_calls, int want_waits, DWORD want_error) {
    held_error = error, held = times, calls = waits = 0, last_error = 0;
    const BOOL got = move_retry(L"from", L"to");
    const int ok = got == want && calls == want_calls && waits == want_waits && (want || GetLastError() == want_error);
    printf("%s %s: %s, %d calls, %d waits, error %lu\n", ok ? "ok" : "FAIL", name, got ? "moved" : "not moved", calls, waits, GetLastError());
    return ok;
}
int main(void) {
    int ok = 1;
    ok &= check("free", 0, 0, TRUE, 1, 0, 0);
    ok &= check("denied 3 times, then free", ERROR_ACCESS_DENIED, 3, TRUE, 4, 3, 0);
    ok &= check("shared 2 times, then free", ERROR_SHARING_VIOLATION, 2, TRUE, 3, 2, 0);
    ok &= check("path not found", ERROR_PATH_NOT_FOUND, -1, FALSE, 1, 0, ERROR_PATH_NOT_FOUND);
    ok &= check("denied for ever", ERROR_ACCESS_DENIED, -1, FALSE, 80, 79, ERROR_ACCESS_DENIED);
    return !ok;
}
'''


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = (ROOT / "builder/setup/setup.c").read_text(encoding="utf-8")
    for call in ("move_retry(from, to)", "move_retry(staged, final)"):
        assert call in source, "setup.c does not use move_retry for " + call
    assert "renaming %ls, Windows error %lu" in source, "the rename message does not carry Windows' error"
    with tempfile.TemporaryDirectory(prefix="setup-rename-retry-") as folder:
        c, exe = Path(folder) / "retry.c", Path(folder) / "retry.exe"
        c.write_text(FIXTURE.replace("/* MOVE_RETRY */", function(source, "static BOOL move_retry(")), encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        assert result.returncode == 0, "move_retry failed a case"
    print("setup rename retry: ok")


if __name__ == "__main__":
    main()
