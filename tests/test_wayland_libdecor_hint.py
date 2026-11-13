"""SDL2 on Wayland takes libdecor from the start (host/posix/display_sdl.c hos_display_init): SDL2 2.32's switch to
libdecor while it waits for the window's first configure never ends (COSMIC, #992). Compiles the actual function
with authored SDL stubs and checks the hint for SDL2, SDL3 and a headless start; no display or game required."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "host/posix/display_sdl.c is the Linux display; the harness uses POSIX setenv"}

ROOT = Path(__file__).resolve().parents[1]
HINT = "SDL_VIDEO_WAYLAND_PREFER_LIBDECOR"

HARNESS = r"""
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define HOS_APP_ID "io.github.NotRexed.Hookshot"
enum { HOS_SDL_VIDEO = 0x20, HOS_SDL_EVENTS = 0x4000 };
static int version;
static std::vector<std::string> hints;
static int inits;
static void hos_log(const char*, ...) {}
static int hos_sdl_load() { return version; }
static void hos_sdl_identity(const char*, const char*) {}
static void hos_sdl_hint(const char* name, const char* value) {
    // (inits: a hint set after SDL_Init is one SDL2's Wayland back end has already read)
    hints.push_back(std::string(inits ? "late:" : "") + name + "=" + value);
}
static int hos_sdl_init(unsigned) { ++inits; return 1; }
static const char* hos_sdl_error() { return ""; }
static int hos_sdl_desktop_size(int* w, int* h) { *w = 2560; *h = 1440; return 1; }
static int hos_posix_window[4];
static int sdl_ready;
int hos_display_owns_sdl(void) { return sdl_ready; }

#include "display_init_actual.inc"

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    version = mode == "sdl3" ? 3 : 2;
    if (mode == "headless") setenv("HOS_HEADLESS", "1", 1);
    const int r = hos_display_init();
    printf("%s: init %d\n", mode.c_str(), r);
    for (const std::string& h : hints) printf("hint %s\n", h.c_str());
    return 0;
}
"""


def function(source, signature):
    start = source.index(signature)
    first = source.index("{", start)
    depth, at = 1, first + 1
    while depth:
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
        at += 1
    return source[start:at]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"), help="GCC/Clang C++ compiler")
    parser.add_argument("--source-root", type=Path, default=ROOT,
                        help="Public source root override for an original-source negative control")
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    display = (args.source_root / "host/posix/display_sdl.c").read_text(encoding="utf-8")
    # Initialization uses the title helper even though this fixture creates no window.
    actual = function(display, "static const char *window_title(") + "\n\n" + function(display, "int hos_display_init(void)")
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
    env.pop("HOS_HEADLESS", None)
    for name in ("SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER", "XDG_CURRENT_DESKTOP", HINT):
        env.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="hookshot-libdecor-hint-") as folder:
        tmp = Path(folder)
        (tmp / "display_init_actual.inc").write_text(actual + "\n", encoding="utf-8")
        (tmp / "harness.cpp").write_text(HARNESS, encoding="utf-8")
        exe = tmp / ("harness.exe" if os.name == "nt" else "harness")
        command = [cxx, "-std=c++20", "-O0", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(tmp), str(tmp / "harness.cpp"), "-o", str(exe),
                   # (env_on, the switches' rule: gpu/vk/compat/hookshot_env.h)
                   "-include", str(args.source_root / "gpu/vk/compat/hookshot_env.h")]
        result = subprocess.run(command, cwd=tmp, env=env, capture_output=True, text=True, timeout=60,
                                creationflags=flags)
        if result.returncode:
            print(result.stdout + result.stderr)
            return result.returncode
        # (mode: whether the hint is set before SDL_Init)
        cases = {"sdl2": True, "sdl3": False, "headless": False}
        failed = 0
        for mode, wanted in cases.items():
            result = subprocess.run([str(exe), mode], cwd=tmp, env=env, capture_output=True, text=True, timeout=10,
                                    creationflags=flags)
            out = result.stdout + result.stderr
            print(out.strip())
            set_early = ("hint %s=1" % HINT) in out.splitlines()
            set_late = ("hint late:%s=1" % HINT) in out.splitlines()
            if result.returncode or set_early != wanted or set_late:
                print("FAIL %s: %s %s before SDL_Init" % (mode, HINT, "not set" if wanted else "set"))
                failed += 1
        print("wayland libdecor hint: %d cases, %d failed" % (len(cases), failed))
        return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
