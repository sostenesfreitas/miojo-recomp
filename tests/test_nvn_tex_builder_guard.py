"""Compile the native NVN texture-builder setter and call it with no builder.

The crash this guards: the game passed a texture builder of no address (0) after a long freeze (the
ocarina note sheet's first build, combat in Hyrule Field), and nvnTextureBuilderSetSwizzle read
A64_MEM + 0x5c (guest 0x5c) and took an access violation (issue 500).  The fix turns a null builder
into a zeroed scratch builder, says so once in the log, and leaves a real builder alone.

The fixture is built from the actual function bodies in host/nvn_native.c, at -O2.  With the pre-fix
source the null call crashes the fixture; with the fix it logs once, and a real builder still keeps
its swizzle.

python tests/test_nvn_tex_builder_guard.py [--cc clang] [--source host/nvn_native.c]
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "clang"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    """The source text of the function or macro block starting at marker, braces balanced."""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="clang", help="C compiler (clang or gcc)")
    p.add_argument("--source", default=str(ROOT / "host" / "nvn_native.c"), type=Path,
                   help="nvn_native.c to test (the pre-fix file shows the crash)")
    a = p.parse_args()

    source = a.source.resolve().read_text(encoding="utf-8")
    try:
        guard = block(source, "static nvn_tex_builder *tb_or_scratch(void *p, uint64_t lr)")
    except ValueError:
        guard = ""   # the pre-fix source has no guard
    swizzle = block(source, "NV(nvnTextureBuilderSetSwizzle)")
    tb = ("#define TB(p) tb_or_scratch((p), c->x[30])" if guard
          else "#define TB(p) ((nvn_tex_builder *)(p))")

    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
typedef volatile long LONG;
static LONG InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
#endif

typedef struct { uint64_t x[32]; } Ctx;
typedef struct { uint32_t swizzle_r, swizzle_g, swizzle_b, swizzle_a; } nvn_texdesc;
typedef struct { nvn_texdesc d; } nvn_tex_builder;
#define GP(a) ((void *)(uintptr_t)(a))
#define P(i) ((void *)(c->x[i] ? GP(c->x[i]) : NULL))
#define RET(v) (c->x[0] = (uint64_t)(v))
#define U32(i) ((uint32_t)c->x[i])
#define NV(name) __attribute__((noinline)) static void n_##name(Ctx *c, void *u_)
static int logs;
static void hos_log(const char *fmt, ...) { (void)fmt; logs++; }
static void die(const char *fmt, ...) { (void)fmt; }
''' + guard + "\n" + tb + "\n" + swizzle + r'''
int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[1] = 1, c.x[2] = 2, c.x[3] = 3, c.x[4] = 4;

    /* Before the guard this is the crash: P(0) is NULL and the setter writes NULL + 0x5c. */
    n_nvnTextureBuilderSetSwizzle(&c, NULL);
    if (logs != 1) {
        fprintf(stderr, "FAIL: a null texture builder logged %d times\n", logs);
        return 1;
    }

    /* A real builder still answers as before. */
    nvn_tex_builder b;
    memset(&b, 0, sizeof b);
    c.x[0] = (uint64_t)(uintptr_t)&b;
    n_nvnTextureBuilderSetSwizzle(&c, NULL);
    if (b.d.swizzle_r != 1 || b.d.swizzle_g != 2 || b.d.swizzle_b != 3 || b.d.swizzle_a != 4) {
        fprintf(stderr, "FAIL: a real builder's swizzle = %u %u %u %u\n",
                b.d.swizzle_r, b.d.swizzle_g, b.d.swizzle_b, b.d.swizzle_a);
        return 1;
    }
    if (logs != 1) {
        fprintf(stderr, "FAIL: a real texture builder logged %d times\n", logs);
        return 1;
    }
    puts("PASS: a null texture builder is a scratch one; a real builder keeps its swizzle");
    return 0;
}
'''

    cc = shutil.which(a.cc)
    if not cc:
        raise SystemExit("test_nvn_tex_builder_guard: no C compiler %r (pass --cc)" % a.cc)
    with tempfile.TemporaryDirectory(prefix="nvn-tex-builder-guard-") as temp:
        d = Path(temp)
        cpp = d / "fixture.c"
        exe = d / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-O2", "-fno-strict-aliasing", str(cpp), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout, end="")
            print(result.stderr, end="")
            raise SystemExit("test_nvn_tex_builder_guard: the null texture builder crashed (exit %d); the source "
                             "dereferences the null builder" % result.returncode)
        print(result.stdout, end="")


if __name__ == "__main__":
    main()
