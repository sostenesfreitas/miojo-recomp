"""Why the Linux renderer did not load is in the log (host/posix/win32_posix.c hos_load_error, host/rx_backend.c load_vulkan).

hookshot_vk.so is opened with RTLD_NOW and takes symbols from the executable, so one from another build fails at dlopen,
before mkwvk_init's version check, and the game falls back to WebGPU. The POSIX LoadLibraryA kept only
ERROR_FILE_NOT_FOUND, so the log said "(2)" for that and for a missing file alike. It now keeps dlerror()'s text, which
names the file or the symbol, and load_vulkan prints it (the dev-loop proposal's B1, #2636).

Everywhere: the source does so. On Linux it also builds host/posix/win32_posix.c as it is and loads, through
LoadLibraryA, a .dll name with no library behind it, a library that needs a symbol nothing has, then a good one.

python -I -B tests/test_renderer_load_error.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r"""#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        HMODULE m = LoadLibraryA(argv[i]);
        printf("%s|%lu|%s\n", m ? "loaded" : "failed", m ? 0ul : (unsigned long)GetLastError(), hos_load_error());
    }
    return 0;
}
"""


def check_source():
    shim = (ROOT / "host" / "posix" / "win32_posix.c").read_text(encoding="utf-8")
    fn = shim[shim.index("HMODULE LoadLibraryA(LPCSTR name) {"):]
    fn = fn[:fn.index("\n}\n")]
    if fn.index("load_error[0] = 0;") > fn.index("if (!name) return NULL;"):
        raise SystemExit("LoadLibraryA: an early failure leaves the last one's reason behind")
    if "dlerror()" not in fn or "snprintf(load_error, sizeof load_error" not in fn:
        raise SystemExit("LoadLibraryA: dlerror()'s text is not kept")
    if "const char *hos_load_error(void) { return load_error; }" not in shim:
        raise SystemExit("win32_posix.c: hos_load_error is gone")
    if "const char *hos_load_error(void);" not in (ROOT / "host" / "posix" / "windows.h").read_text(encoding="utf-8"):
        raise SystemExit("host/posix/windows.h: hos_load_error is not declared")
    rx = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
    load = rx[rx.index("static const MkwVkRenderer *load_vulkan(const char *dll) {"):]
    load = load[:load.index("\n}\n")]
    failed = load[load.index("if (!m) {"):load.index("return NULL;", load.index("if (!m) {"))]
    if "#ifdef _WIN32" not in failed or "hos_load_error()" not in failed.split("#else")[1]:
        raise SystemExit("load_vulkan: the Linux renderer's failure to load is logged without its reason")
    print("ok   the source keeps dlerror()'s text and load_vulkan logs it")


def check_posix(cc):
    with tempfile.TemporaryDirectory(prefix="renderer-load-error-") as tmp:
        tmp = Path(tmp)
        (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
        exe = tmp / "driver"
        subprocess.run([cc, "-std=gnu11", "-D_GNU_SOURCE", "-w", "-I", str(ROOT / "host" / "posix"), str(tmp / "driver.c"),
                        str(ROOT / "host" / "posix" / "win32_posix.c"), "-o", str(exe), "-lpthread", "-ldl"], check=True)
        # a library that needs a symbol nothing loaded has, as hookshot_vk.so from another build would
        (tmp / "needs.c").write_text("extern int mkwvk_from_another_build(void);\nint use(void) { return mkwvk_from_another_build(); }\n",
                                     encoding="utf-8")
        subprocess.run([cc, "-shared", "-fPIC", str(tmp / "needs.c"), "-o", str(tmp / "needs.so")], check=True)
        (tmp / "good.c").write_text("int fine(void) { return 1; }\n", encoding="utf-8")
        subprocess.run([cc, "-shared", "-fPIC", str(tmp / "good.c"), "-o", str(tmp / "good.so")], check=True)
        r = subprocess.run([str(exe), str(tmp / "absent_vk.dll"), str(tmp / "needs.dll"), str(tmp / "good.dll")],
                           capture_output=True, text=True, timeout=60)
        if r.returncode:
            raise SystemExit("the driver stopped (%d): %s" % (r.returncode, r.stderr[-2000:]))
        rows = [line.split("|", 2) for line in r.stdout.splitlines()]
        if len(rows) != 3:
            raise SystemExit("the driver printed %r" % r.stdout)
        (absent, needs, good) = rows
        if absent[0] != "failed" or absent[1] != "2" or "absent_vk.so" not in absent[2]:
            raise SystemExit("a missing library: %r (wanted the failure, 2, and the file named)" % absent)
        if needs[0] != "failed" or "mkwvk_from_another_build" not in needs[2]:
            raise SystemExit("a library needing a missing symbol: %r (wanted the symbol named)" % needs)
        if good[0] != "loaded" or good[2] != "":
            raise SystemExit("a good library: %r (wanted it loaded, and no reason left from the failure before)" % good)
        print("ok   a missing file: %s" % absent[2])
        print("ok   a missing symbol: %s" % needs[2])
        print("ok   a good library loads, with no reason left over")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_source()
    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: loading through the posix layer runs on Linux")
        return
    check_posix(a.cc)
    print("renderer load error: ok")


if __name__ == "__main__":
    main()
