"""Compile every guarded NVN memory-pool accessor and call it with no pool.

The crash this guards: while Kakariko changed from day to night, the OptimizeChangeBanc thread called
nvnMemoryPoolGetFlags(NULL); the HLE read flags at A64_MEM + 4 (guest 0x4) and took an access violation.
P(0) returns NULL for a 0 argument, but dereferencing it is undefined behaviour, so the compiler may fold the
check away (and did: the shipped code read 0x100000000004).  The fix guards before the dereference.

Every accessor that checks no_pool is covered here: flags, size, map, buffer address and invalidate mapped range.
The fixture is built from the actual function bodies in host/nvn_native.c, at -O2.  With the pre-fix source the
null calls crash the fixture; with the fix they answer 0 (or return without touching anything) and a real pool
still answers with its flags, size, CPU mapping and GPU address.

python tests/test_nvn_null_pool.py [--cc clang] [--source host/nvn_native.c]
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
        guard = block(source, "static int no_pool(Ctx *c, const char *what)")
    except ValueError:
        guard = ""   # the pre-fix source has no guard
    flags = block(source, "NV(nvnMemoryPoolGetFlags)")
    size = block(source, "NV(nvnMemoryPoolGetSize)")
    map_ = block(source, "NV(nvnMemoryPoolMap)")
    address = block(source, "NV(nvnMemoryPoolGetBufferAddress)")
    invalidate = block(source, "NV(nvnMemoryPoolInvalidateMappedRange)")

    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t x[32]; } Ctx;
typedef struct {
    uint32_t magic, flags;
    uint8_t *mem;
    uint64_t size, gpu;
    void *vmap;
} nvn_pool;
#define GP(a) ((void *)(uintptr_t)(a))
#define P(i) ((void *)(c->x[i] ? GP(c->x[i]) : NULL))
#define RET(v) (c->x[0] = (uint64_t)(v))
#define NV(name) __attribute__((noinline)) static void n_##name(Ctx *c, void *u_)
#define NVN_MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS 0x1
static int logs;
static void hos_log(const char *fmt, ...) { (void)fmt; logs++; }
static void die(const char *fmt, ...) { (void)fmt; }
static uint64_t h2g(void *p) { return (uint64_t)(uintptr_t)p; }
#define RETP(p) RET(h2g(p))
#define U64(i) (c->x[i])
static void rx_nvn_invalidate(uint64_t a, uint64_t b) { (void)a; (void)b; }
''' + guard + "\n" + flags + "\n" + size + "\n" + map_ + "\n" + address + "\n" + invalidate + r'''
int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c);

    /* Before the guard these crash: P(0) is NULL and the accessor reads through it. */
    n_nvnMemoryPoolGetFlags(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: flags of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolGetSize(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: size of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolMap(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: map of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolGetBufferAddress(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: buffer address of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);

    /* A real pool still answers as before. */
    nvn_pool pool;
    memset(&pool, 0, sizeof pool);
    pool.magic = NVN_MAGIC('P', 'O', 'O', 'L');
    pool.flags = 0x40;
    pool.mem = (uint8_t *)(uintptr_t)0x1234;
    pool.size = 0x2000;
    pool.gpu = 0x5678;
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolGetFlags(&c, NULL);
    if (c.x[0] != 0x40) {
        fprintf(stderr, "FAIL: flags of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolGetSize(&c, NULL);
    if (c.x[0] != 0x2000) {
        fprintf(stderr, "FAIL: size of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolMap(&c, NULL);
    if (c.x[0] != 0x1234) {
        fprintf(stderr, "FAIL: map of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolGetBufferAddress(&c, NULL);
    if (c.x[0] != 0x5678) {
        fprintf(stderr, "FAIL: buffer address of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);
    puts("PASS: a null memory pool answers 0; a real pool keeps its flags, size, mapping and address");
    return 0;
}
'''

    cc = shutil.which(a.cc)
    if not cc:
        raise SystemExit("test_nvn_null_pool: no C compiler %r (pass --cc)" % a.cc)
    with tempfile.TemporaryDirectory(prefix="nvn-null-pool-") as temp:
        d = Path(temp)
        cpp = d / "fixture.c"
        exe = d / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-O2", "-fno-strict-aliasing", str(cpp), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout, end="")
            print(result.stderr, end="")
            raise SystemExit("test_nvn_null_pool: a null-pool accessor crashed (exit %d); the source dereferences "
                             "the null pool" % result.returncode)
        print(result.stdout, end="")


if __name__ == "__main__":
    main()
