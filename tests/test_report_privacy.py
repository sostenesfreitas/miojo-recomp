"""Bounded, allocation-free pending-record labels; no game, OS, or renderer required."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
/* An allocation accidentally added to the inline redactor must not link. */
#define malloc forbidden_malloc_in_report_redactor
#define calloc forbidden_calloc_in_report_redactor
#define realloc forbidden_realloc_in_report_redactor
#define free forbidden_free_in_report_redactor
#include "hos_report_privacy.h"
int main(void) {
    HosReportPrivacy p;
    hos_report_privacy_init(&p, "/home/private-user", "private-user", "private-host");
    const char *bad[] = {"/home/private-user/game", "private-user.dll", "PRIVATE-HOST.dll",
        "10.20.30.40", "fe80::1", "dead::beef", "DEAD::BEEF", "sead:MainThread",
        "sead::MainThread/path", "sead::MainThread\\path",
        "user@example.test", "..\\secret", "line\ninjected", "key:\tvalue"};
    char b[80];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        hos_report_privacy_redact(&p, b, sizeof b, bad[i], strlen(bad[i]));
        assert(!strcmp(b, "redacted"));
    }
    const char *good[] = {"hookshot.exe", "libc.so.6", "nvoglv64.dll", "Presentation Thread", "v2.1.0-gabc123",
        "sead::MainThread", "sead::TaskMgr::Worker_2"};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; ++i) {
        hos_report_privacy_redact(&p, b, sizeof b, good[i], strlen(good[i]));
        assert(!strcmp(b, good[i]));
    }
    HosReportPrivacy identities;
    hos_report_privacy_init(&identities, "/home/privateuser", "privateuser", "privatehost");
    const char *named_user = "sead::privateuser::Thread";
    hos_report_privacy_redact(&identities, b, sizeof b, named_user, strlen(named_user));
    assert(!strcmp(b, "redacted"));
    const char *named_host = "sead::privatehost::Thread";
    hos_report_privacy_redact(&identities, b, sizeof b, named_host, strlen(named_host));
    assert(!strcmp(b, "redacted"));
    struct { char out[4]; char sentinel; } bounded = {{0}, 'X'};
    assert(hos_report_privacy_redact(&p, bounded.out, sizeof bounded.out, "abcdef", 6) == 3);
    assert(!strcmp(bounded.out, "abc") && bounded.sentinel == 'X');
    assert(hos_report_privacy_redact(&p, NULL, 0, "abc", 3) == 0);
    const char unterminated[3] = {'a', 'b', 'c'};
    hos_report_privacy_redact(&p, b, sizeof b, unterminated, sizeof unterminated);
    assert(!strcmp(b, "abc"));
    hos_report_privacy_redact(&p, b, sizeof b, NULL, 999);
    assert(!b[0]);
    hos_report_privacy_redact(&p, b, sizeof b, "\xc3\xa9", 2);
    assert(!strcmp(b, "??"));
    char long_id[300]; memset(long_id, 'z', sizeof long_id); long_id[299] = 0;
    hos_report_privacy_init(&p, "", long_id, "");
    hos_report_privacy_redact(&p, b, sizeof b, "ordinary", 8);
    assert(!strcmp(b, "redacted"));
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("gcc") or "clang")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-report-privacy-") as temp:
        src, exe = Path(temp) / "privacy.c", Path(temp) / "privacy.exe"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "host"), str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: report labels omit identities, paths and addresses; bounds and no-allocation guard")
    print("PASS: C++ namespace thread labels kept; IPv6, single colons, paths and names still redacted")


if __name__ == "__main__":
    main()
