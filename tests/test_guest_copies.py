"""Host copies the game asks for stay in guest memory (host/nn_libc.c, host/nn_fs.c, host/nn_video.c).

- The memcpy, memmove and memset hooks: a range past the guest window is left to the game's own (lifted) copy, never
  done by the host. Built from host/nn_libc.c as it is, over a small window with a guard page after it.
- nn::fs reads and writes take guest buffers only, and GetFileSize answers 0 for a handle with no size.
- ReadDecoderOutputImageNv12 reads nothing when the pool's NV12 allocation failed (its NULL plus an offset was any host
  address, copied to the game).

python -I -B tests/test_guest_copies.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


HOS_H = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t x[32]; } Ctx;
/* (a 64 KB guest window, then a page the test watches) */
extern uint8_t *window;
#define WINDOW 0x10000ull
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(window + (a)))
#define HOS_WRITTEN(p, n) ((void)0)
#define ENTRY_HOOK_SYM(sym) int entry_sym_##sym(Ctx *c)
static inline void *hos_guest_range(uint64_t a, uint64_t n) { return a < WINDOW && n <= WINDOW - a ? GP(a) : (void *)0; }
'''

FIXTURE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "hos.h"
uint8_t *window;
#include "nn_libc.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    window = mmap(NULL, WINDOW + 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(window != MAP_FAILED);
    uint8_t *after = window + WINDOW;
    memset(after, 0xAB, 0x1000);
    Ctx c = {{0}};
    /* inside the window: the host copies */
    memcpy(window + 0x100, "guest", 6);
    c.x[0] = 0x200, c.x[1] = 0x100, c.x[2] = 6;
    CHECK(entry_sym_memcpy(&c) == 1 && !memcmp(window + 0x200, "guest", 6));
    c.x[0] = 0x300, c.x[1] = 0x41, c.x[2] = 16;
    CHECK(entry_sym_memset(&c) == 1 && window[0x30f] == 0x41);
    /* running past the window's end: the game's own copy, the page after untouched */
    c.x[0] = WINDOW - 8, c.x[1] = 0, c.x[2] = 64;
    CHECK(entry_sym_memset(&c) == 0);
    c.x[0] = WINDOW - 8, c.x[1] = 0x100, c.x[2] = 64;
    CHECK(entry_sym_memcpy(&c) == 0);
    c.x[0] = WINDOW - 8, c.x[1] = 0x100, c.x[2] = 64;
    CHECK(entry_sym_memmove(&c) == 0);
    /* a source past the window, or an address that wraps */
    c.x[0] = 0x100, c.x[1] = WINDOW + 0x10, c.x[2] = 8;
    CHECK(entry_sym_memcpy(&c) == 0);
    c.x[0] = ~0ull - 4, c.x[1] = 0, c.x[2] = 16;
    CHECK(entry_sym_memset(&c) == 0);
    for (int i = 0; i < 0x1000; i++) CHECK(after[i] == 0xAB);
    printf("PASS: the libc hooks copy inside the guest window and leave the rest to the game's own code\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    fs = (ROOT / "host" / "nn_fs.c").read_text(encoding="utf-8")
    read_at = block(fs, "static uint64_t read_at(")
    check("if (!hos_guest_range(buf, size)) return 0;" in read_at, "read_at writes into a buffer it did not check")
    write = block(fs, "HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE)")
    check("hos_guest_range(ARG(2), ARG(3))" in write, "WriteFile reads a buffer it did not check")
    size = block(fs, "HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE)")
    check("LARGE_INTEGER sz = {0};" in size, "GetFileSize can answer with the host stack's bytes")
    video = (ROOT / "host" / "nn_video.c").read_text(encoding="utf-8")
    nv12 = block(video, "HLE(_ZN2nn5codec26ReadDecoderOutputImageNv12E")
    check("!o->nv12" in nv12, "ReadDecoderOutputImageNv12 reads a pool whose NV12 was not allocated")
    print("PASS: nn::fs and the NV12 reader check their buffers")

    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the hooks' fixture uses mmap (Linux)")
        return
    with tempfile.TemporaryDirectory(prefix="guest-copies-") as tmp:
        tmp = Path(tmp)
        (tmp / "hos.h").write_text(HOS_H, encoding="utf-8")
        (tmp / "fixture.c").write_text(FIXTURE, encoding="utf-8")
        # (beside the stand-in hos.h: an include in quotes looks in the including file's folder first)
        (tmp / "nn_libc.c").write_text((ROOT / "host" / "nn_libc.c").read_text(encoding="utf-8"), encoding="utf-8")
        exe = tmp / "fixture"
        subprocess.run([a.cc, "-std=gnu11", "-include", ENV_ON_H, "-Wall", "-Wextra", "-I", str(tmp), str(tmp / "fixture.c"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
