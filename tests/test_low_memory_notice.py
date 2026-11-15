"""Low physical RAM on Linux must warn even when swap makes the commit limit large.

Extract the production warning and boot call. Authored memory-query and UI spies
exercise the exact 7639 MiB RAM + 32767 MiB swap report, boundaries, query failure,
unit conversion and headless behavior. Windows retains its RAM + page-file rule.
On Linux, a second build selects the production Windows policy after the host
headers are included; its memory query and UI remain spies, not Windows runtime.

    python -I -B tests/test_low_memory_notice.py [--cc gcc] [--source host/hos_main.c]
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("marker not found once: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#define _CRT_SECURE_NO_WARNINGS
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hookshot_env.h"
/* Preserve the real host's environment API before selecting a policy. No
   production condition is changed: _WIN32 selects its existing Windows path. */
#ifdef _WIN32
#define TEST_NATIVE_WINDOWS
#endif
#if defined(TEST_WINDOWS_POLICY) && !defined(_WIN32)
#define _WIN32 1
#endif
static unsigned long long ram_mb = 7639, swap_mb = 32767;
static int query_failed, boxes;
#ifndef _WIN32
static unsigned mem_unit = 1;
static int sdl_loads;
#endif
static char last_log[1024], box_text[512];
#ifdef _WIN32
/* The notice only reads these fields. A union leaves its sizeof initializer
   warning-free without replacing the function under test. */
typedef union {
    unsigned long dwLength;
    struct { unsigned long long ullTotalPhys, ullTotalPageFile; } sizes;
} MEMORYSTATUSEX;
#define ullTotalPhys sizes.ullTotalPhys
#define ullTotalPageFile sizes.ullTotalPageFile
static int GlobalMemoryStatusEx(MEMORYSTATUSEX *m) {
    m->ullTotalPhys = ram_mb << 20;
    m->ullTotalPageFile = (ram_mb + swap_mb) << 20;
    return !query_failed;
}
#else
struct sysinfo { unsigned long totalram, totalswap; unsigned mem_unit; };
static int sysinfo(struct sysinfo *m) {
    m->totalram = (ram_mb << 20) / mem_unit;
    m->totalswap = (swap_mb << 20) / mem_unit;
    m->mem_unit = mem_unit;
    return query_failed;
}
#endif
#define MB_OK 0
#define MB_ICONWARNING 0x30
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, a);
    va_end(a);
}
static int MessageBoxA(void *w, const char *text, const char *caption, unsigned type) {
    (void)w, (void)caption, (void)type;
    snprintf(box_text, sizeof box_text, "%s", text);
    return ++boxes;
}
#ifndef _WIN32
int hos_sdl_load(void) { ++sdl_loads; return 0; }
#endif
static void headless(const char *value) {
#ifdef TEST_NATIVE_WINDOWS
    _putenv_s("HOS_HEADLESS", value);
#else
    setenv("HOS_HEADLESS", value, 1);
#endif
}
#define CHECK(c, why) do { if (!(c)) return printf("FAIL: %s\n", why), 1; } while (0)
@@CODE@@
int main(void) {
    static const unsigned long long cases[][2] = {
        {7639, 32767}, {8192, 0}, {8192, 2047}, {8192, 2048},
        {10239, 0}, {10239, 32768}, {10240, 0}, {16384, 0}, {98304, 32768}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        char text[512] = "";
        ram_mb = cases[i][0], swap_mb = cases[i][1];
#if defined(__linux__) && !defined(_WIN32)
        const int expected = ram_mb < 10240;
#else
        const int expected = ram_mb + swap_mb < 10240;
#endif
        int got = memory_notice_text(ram_mb, swap_mb, text, sizeof text);
        CHECK(got == expected, "wrong warning decision (swap must not hide low RAM on Linux)");
        if (got) {
#if defined(__linux__) && !defined(_WIN32)
            CHECK(strstr(text, "The game starts anyway."), "Linux warning does not explain that startup continues");
            CHECK(strstr(text, "unresponsive") && strstr(text, "not a replacement for RAM"), "missing paging caution");
            CHECK(strstr(text, "16 GB") && strstr(text, "13 GiB"), "missing RAM guidance");
#else
            CHECK(strstr(text, "It starts now anyway."), "legacy warning does not explain that startup continues");
            CHECK(strstr(text, "the page file on Windows") && strstr(text, "two together"), "missing commit-limit guidance");
            CHECK(!strstr(text, "unresponsive") && !strstr(text, "not a replacement for RAM") &&
                  !strstr(text, "13 GiB"), "Linux-only guidance in the legacy warning");
#endif
        } else {
            CHECK(!text[0], "a no-warning case wrote warning text");
        }
        for (int quiet = 0; quiet < 2; ++quiet) {
            boxes = 0; last_log[0] = box_text[0] = 0;
#ifndef _WIN32
            sdl_loads = 0;
#endif
            headless(quiet ? "1" : "0");
            memory_notice();
            CHECK(boxes == (expected && !quiet), "wrong message-box count");
            CHECK((strstr(last_log, "LOW MEMORY") != NULL) == expected, "wrong boot log decision");
            CHECK(strstr(last_log, "MB of memory") && strstr(last_log, "MB of swap"), "missing exact boot counts");
            if (boxes) CHECK(!strcmp(box_text, text), "message differs from the warning text");
#ifndef _WIN32
            CHECK(sdl_loads == boxes, "SDL loaded without showing a warning");
#endif
        }
        printf("PASS: %llu MiB RAM + %llu MiB swap, warning=%d, visible/headless\n", ram_mb, swap_mb, expected);
    }
    ram_mb = 7639, swap_mb = 32767;
#ifndef _WIN32
    mem_unit = 4096;
#endif
    headless("1"); memory_notice();
    CHECK(strstr(last_log, "7639 MB of memory, 32767 MB of swap"), "memory-query units changed");
    query_failed = 1; last_log[0] = 0; boxes = 0;
    memory_notice();
    CHECK(!last_log[0] && !boxes, "failed memory query invents a warning");
    puts("PASS: memory-query units and failure");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--source", type=Path, default=ROOT / "host" / "hos_main.c")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    boot = src.index('hos_log("boot: image %zu MB')
    call = src.find("memory_notice();", boot)
    if call < 0 or call > src.index("hos_mods_init(image);", boot):
        raise SystemExit("FAIL: the game does not check its memory at boot")
    if "#include <sys/sysinfo.h>" not in src:
        raise SystemExit("FAIL: host/hos_main.c reads Linux's memory without <sys/sysinfo.h>")
    define = next(line for line in src.splitlines() if line.startswith("#define HOS_MEMORY_NEEDED_MB"))
    code = "\n".join((define, block(src, "static int memory_notice_text("), block(src, "static void memory_notice(void) {")))
    variants = [("native", [])]
    if sys.platform.startswith("linux"):
        variants.append(("windows-policy-on-linux", ["-DTEST_WINDOWS_POLICY"]))
    with tempfile.TemporaryDirectory(prefix="low-memory-") as tmp:
        for label, flags in variants:
            c, exe = Path(tmp) / (label + ".c"), Path(tmp) / (label + ".exe")
            c.write_text(FIXTURE.replace("@@CODE@@", code), encoding="utf-8")
            command = [args.cc, "-x", "c", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", *flags,
                       "-I", str(ROOT / "gpu" / "vk" / "compat"), str(c), "-o", str(exe)]
            r = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if r.returncode:
                print(r.stdout + r.stderr)
                raise SystemExit("the fixture did not build: " + label)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            print(label + (" (not Windows execution)" if flags else "") + ":")
            print(r.stdout, end="")
            if r.returncode:
                raise SystemExit("FAIL: memory_notice " + label)
    print("PASS: production low-memory decisions, exact counts, message, headless and boot wiring")


if __name__ == "__main__":
    main()
