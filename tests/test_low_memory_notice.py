"""The game says at boot when the PC has too little memory and swap for it (host/hos_main.c memory_notice, issue 1241).

On 8 GB of RAM and no swap the game closed before the title with no word of why; adding swap let it play. Memory and
swap (the page file on Windows) together below about 10 GB now give a LOW MEMORY line in the log and a message once at
boot, and the game starts anyway. memory_notice_text and memory_notice are cut into a fixture with hos_log and
MessageBoxA as spies: the threshold at a few sizes, the boot line with this PC's numbers, and no message with
HOS_HEADLESS. The source is checked for the call at boot.

    python -I -B tests/test_low_memory_notice.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cc", "gcc"]

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
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#define MB_OK 0
#define MB_ICONWARNING 0x30
static int boxes;
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("log: "), vprintf(fmt, a), printf("\n");
    va_end(a);
}
static int MessageBoxA(void *w, const char *text, const char *caption, unsigned type) {
    (void)w, (void)text, (void)caption, (void)type;
    return ++boxes;
}
int hos_sdl_load(void) { return 0; }
@@CODE@@
int main(void) {
    static const unsigned long long cases[][3] = {
        {8192, 0, 1}, {8192, 2047, 1}, {8192, 2048, 0}, {16384, 0, 0}, {10239, 0, 1}, {10240, 0, 0}};
    char text[512];
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const int got = memory_notice_text(cases[i][0], cases[i][1], text, sizeof text);
        printf("case %llu MB + %llu MB swap: %s\n", cases[i][0], cases[i][1], got ? "notice" : "none");
        if (got != (int)cases[i][2]) return printf("FAIL: wrong answer\n"), 1;
        if (got && (!strstr(text, "8.0 GB") && cases[i][0] == 8192)) return printf("FAIL: the text has no size: %s\n", text), 1;
    }
    setenv("HOS_HEADLESS", "1", 1);
    memory_notice();
    printf("boxes %d\n", boxes);
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    src = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    boot = src.index('hos_log("boot: image %zu MB')
    call = src.find("memory_notice();", boot)
    if call < 0 or call > src.index("hos_mods_init(image);", boot):
        raise SystemExit("FAIL: the game does not check its memory at boot")
    if "#include <sys/sysinfo.h>" not in src:
        raise SystemExit("FAIL: host/hos_main.c reads Linux's memory without <sys/sysinfo.h>")
    if os.name == "nt":
        print("PASS (source only): the fixture reads Linux's memory")
        return
    define = next(line for line in src.splitlines() if line.startswith("#define HOS_MEMORY_NEEDED_MB"))
    code = "\n".join((define, block(src, "static int memory_notice_text("), block(src, "static void memory_notice(void) {")))
    with tempfile.TemporaryDirectory(prefix="low-memory-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(FIXTURE.replace("@@CODE@@", code), encoding="utf-8")
        r = subprocess.run([args.cc, "-std=gnu11", "-include", ENV_ON_H, "-O1", str(c), "-o", str(exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(r.stdout, end="")
        if r.returncode or "boxes 0" not in r.stdout or "MB of swap" not in r.stdout:
            raise SystemExit("FAIL: memory_notice")
    print("PASS: below ~10 GB of memory and swap the log and a message say so; with HOS_HEADLESS no message")


if __name__ == "__main__":
    main()
