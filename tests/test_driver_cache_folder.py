"""The folder __GL_SHADER_DISK_CACHE_PATH names is made before Vulkan loads (host/hos_main.c driver_cache_folder).

NVIDIA's Linux driver keeps no shader cache at all when that folder does not exist: it neither makes it nor falls back to
~/.cache/nvidia, and every start compiles every pipeline again. This cuts driver_cache_folder out of hos_main.c, compiles
it and runs it on missing, nested, existing and impossible paths, and checks that the Linux main calls it before the
window. It also keeps the game's Linux file named hookshot: NVIDIA files its cache by the program's file name alone, so a
new name would start every player's cache from nothing.
python -I -B tests/test_driver_cache_folder.py [--cxx c++]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "the Linux main's code (POSIX mkdir)"}

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
%s
int main() { driver_cache_folder(); return 0; }
"""


def function(source, signature):
    start = source.index(signature)
    first = source.index("{", start)
    depth, at = 1, first + 1
    while depth:
        depth += {"{": 1, "}": -1}.get(source[at], 0)
        at += 1
    return source[start:at]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    src = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    main_fn = src[src.rindex("int main(int argc, char **argv) {"):]
    if "driver_cache_folder();" not in main_fn:
        raise SystemExit("FAIL: the Linux main does not call driver_cache_folder")
    if main_fn.index("driver_cache_folder();") > main_fn.index("hos_display_init()"):
        raise SystemExit("FAIL: driver_cache_folder runs after the window (SDL) may have loaded Vulkan")
    exe_name = re.search(r'^EXE = "([^"]+)" if LINUX', (ROOT / "nx2" / "build.py").read_text(encoding="utf-8"), re.M)
    if not exe_name or exe_name.group(1) != "hookshot":
        raise SystemExit("FAIL: the Linux game is no longer named hookshot (nx2/build.py EXE): NVIDIA's cache is filed by that name")

    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="hookshot-cache-folder-") as folder:
        tmp = Path(folder)
        (tmp / "t.cpp").write_text(HARNESS % function(src, "static void driver_cache_folder(void) {"), encoding="utf-8")
        exe = tmp / "t"
        build = subprocess.run([cxx, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Werror", str(tmp / "t.cpp"), "-o", str(exe)],
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise SystemExit("FAIL: does not compile\n" + build.stdout + build.stderr)
        (tmp / "file").write_text("x", encoding="utf-8")
        cases = [  # (the value, the folder that must exist after, whether it says it could not)
            (str(tmp / "a" / "b" / "c"), tmp / "a" / "b" / "c", False),
            (str(tmp / "a" / "b" / "c"), tmp / "a" / "b" / "c", False),   # (already there)
            (str(tmp / "d" / "e") + "/", tmp / "d" / "e", False),
            (str(tmp / "f") + "//g", tmp / "f" / "g", False),
            (str(tmp / "file" / "h"), None, True),
            (str(tmp / "file"), None, True),   # (a file at the name itself)
            ("", None, False),
        ]
        failed = 0
        for value, made, complains in cases:
            env = dict(os.environ, __GL_SHADER_DISK_CACHE_PATH=value)
            r = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=10)
            ok = r.returncode == 0 and (made is None or made.is_dir()) and (("cannot make" in r.stderr) == complains)
            print("%s %r%s" % ("ok  " if ok else "FAIL", value, (": " + r.stderr.strip()) if r.stderr else ""))
            failed += not ok
        r = subprocess.run([str(exe)], env={k: v for k, v in os.environ.items() if k != "__GL_SHADER_DISK_CACHE_PATH"},
                           capture_output=True, text=True, timeout=10)
        failed += r.returncode != 0
    if failed:
        raise SystemExit("FAIL: %d case(s)" % failed)
    print("PASS: the folder NVIDIA's shader cache is pointed at is made before Vulkan loads")


if __name__ == "__main__":
    main()
