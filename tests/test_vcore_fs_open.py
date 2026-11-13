"""OpenFile gives its virtual core up before host_path, not only around CreateFileA (host/nn_fs.c, issue 1929).

The first open of a button-prompt file builds it inside host_path (host/prompts.c hos_prompts_romfs, through mods.c),
251-357 ms on Linux, and threads that open it meanwhile wait in there for the build. With the core held through that,
the threads waiting for the core ran without one (vc_acquire's escape). The actual OpenFile is compiled with stand-ins
that record whether the thread holds its core in host_path and in CreateFileA, then checks every way out of it:

- host_path and CreateFileA run without the core, and the core is held again when OpenFile returns: a file opened, an
  unknown mount, a write to a mount that is not writable, a movie left out (HOS_NO_MOVIES), a blanked icon, a file not
  found;
- a thread that holds no core when it calls OpenFile takes none.

python -I -B tests/test_vcore_fs_open.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
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
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif
typedef void *HANDLE;
typedef unsigned long DWORD;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
typedef struct { uint64_t x[32]; } Ctx;
#define HLE(sym) void imp_##sym(Ctx *c)
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(uintptr_t)(a))
#define G64(a) (*(uint64_t *)GP(a))
#define HOS_FS_PATH 1024
#define R_PATH_NOT_FOUND 0x202
static int hos_trace;
static int fs_trace(void) { return 0; }
static void hos_log(const char *f, ...) { (void)f; }
static const char *gstr(uint64_t a) { return (const char *)(uintptr_t)a; }

/* the thread's virtual core, as hos_vcore_away / hos_vcore_back keep it (host/hos_thread.c) */
static int held, takes;
int hos_vcore_away(void) { if (!held) return 0; held = 0; return 1; }
void hos_vcore_back(void) { if (!held) held = 1, takes++; }

/* host_path: the mount's path (here, the build of a prompt file would run in it); -1 = not called */
static int path_ok, path_writable, held_in_path, held_in_create, create_ok, blank;
static int host_path(const char *g, char *out, size_t cap, int *writable) {
    held_in_path = held;
    snprintf(out, cap, "%s", g);
    if (writable) *writable = path_writable;
    return path_ok;
}
int hos_ultrawide_redirect(const char *guest, char *out, size_t cap) { (void)guest, (void)out, (void)cap; return 0; }
static int hos_blank_cached_icon(const char *guest, const char *host) { (void)guest, (void)host; return blank; }
static DWORD GetFileAttributesA(const char *p) { (void)p; return INVALID_FILE_ATTRIBUTES; }
static HANDLE CreateFileA(const char *p, DWORD a, DWORD s, void *sa, DWORD d, DWORD f, HANDLE t) {
    (void)p, (void)a, (void)s, (void)sa, (void)d, (void)f, (void)t;
    held_in_create = held;
    return create_ok ? (HANDLE)(intptr_t)0x1234 : INVALID_HANDLE_VALUE;
}
typedef struct { HANDLE h; int writable; char path[HOS_FS_PATH]; } Handle;
static uint64_t new_handle(Handle *h) { free(h); return 7; }

@OPEN@

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
/* one OpenFile(handle, path, mode) by a thread that holds a core (or not) */
static uint64_t open_file(const char *path, int mode, int core) {
    static uint64_t handle;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)&handle, c.x[1] = (uint64_t)(uintptr_t)path, c.x[2] = (uint64_t)mode;
    held = core, takes = 0, held_in_path = held_in_create = -1;
    imp__ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci(&c);
    return c.x[0];
}
static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}
int main(void) {
    set_env("HOS_NO_MOVIES", NULL);
    set_env("HOS_FS_OVERLAY", NULL);
    path_ok = 1, path_writable = 0, create_ok = 1, blank = 0;
    uint64_t r = open_file("rom:/UI/LayoutArchive/Common.bntx.zs", 1, 1);
    check(r == 0, "a file of the game opens");
    check(held_in_path == 0, "host_path (where a prompt file is built at its first open) runs without the core");
    check(held_in_create == 0, "CreateFileA runs without the core");
    check(held == 1 && takes == 1, "the core is held again after OpenFile, taken once");

    path_ok = 0;
    r = open_file("nomount:/x", 1, 1);
    check(r == R_PATH_NOT_FOUND && held_in_path == 0 && held == 1, "an unknown mount: host_path without the core, the core back");
    path_ok = 1;

    r = open_file("rom:/x", 2, 1);
    check(r == R_PATH_NOT_FOUND && held == 1 && held_in_create == -1, "a write to a mount that is not writable: the core back");

    set_env("HOS_NO_MOVIES", "1");
    r = open_file("rom:/Movie/a.webm", 1, 1);
    check(r == R_PATH_NOT_FOUND && held == 1 && held_in_create == -1, "a movie left out (HOS_NO_MOVIES): the core back");
    set_env("HOS_NO_MOVIES", NULL);

    blank = 1;
    r = open_file("rom:/UI/icon.bntx", 1, 1);
    check(r == R_PATH_NOT_FOUND && held == 1 && held_in_create == -1, "a blanked icon: the core back");
    blank = 0;

    create_ok = 0;
    r = open_file("rom:/missing.bin", 1, 1);
    check(r == R_PATH_NOT_FOUND && held_in_create == 0 && held == 1, "a file not found: the core back");
    create_ok = 1;

    r = open_file("rom:/a.bin", 1, 0);
    check(r == 0 && held == 0 && takes == 0, "a thread that held no core takes none");
    if (!bad) puts("ok");
    return bad;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_fs.c")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    fixture = HARNESS.replace("@OPEN@", block(src, "HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {"))
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="vcore-fs-open-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / ("fixture.exe" if os.name == "nt" else "fixture")
        subprocess.run([compiler, "-std=c++17", "-include", ENV_ON_H, "-O1", "-w", str(folder / "fixture.cpp"), "-o", str(exe)], check=True,
                       timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        if result.returncode or "ok" not in result.stdout:
            raise SystemExit("OpenFile holds its virtual core through host_path (issue 1929)")


if __name__ == "__main__":
    main()
