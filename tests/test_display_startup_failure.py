"""Actual POSIX display startup failure paths with authored SDL/thread stubs; no display/game required."""
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
CASES = ("headless", "headless_zero", "load_failure", "init_failure", "window_failure",
         "visible_success", "thread_failure", "prepare_success", "prepare_no_vulkan",
         "flag_as_romfs", "flag_as_user")


def function(source, signature):
    start = source.rindex(signature)
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
    parser.add_argument("--case", action="append", choices=CASES)
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    display = (args.source_root / "host/posix/display_sdl.c").read_text(encoding="utf-8")
    host = (args.source_root / "host/hos_main.c").read_text(encoding="utf-8")
    # Keep the complete init/start/main functions and the loop's actual startup
    # through window-creation failure. Successful event/render handling is outside
    # this fixture: stop at its first normal window assignment, with a marker.
    loop = function(display, "void hos_display_loop(void)")
    loop = loop[:loop.index("    vk_window.window = window;")] + "    ++visible_window_started;\n}\n"
    app_main = function(host, "int main(int argc, char **argv)").replace("int main(", "int app_main(", 1)
    # Include the real title helper used by both initialization and window creation.
    actual = "\n\n".join([function(display, "static const char *window_title("),
                             function(display, "int hos_display_init(void)"),
                             function(display, "void hos_display_start(void)"), loop, app_main])
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="hookshot-display-start-") as folder:
        tmp = Path(folder)
        (tmp / "display_startup_actual.inc").write_text(actual + "\n", encoding="utf-8")
        exe = tmp / ("display_start.exe" if os.name == "nt" else "display_start")
        command = [cxx, "-std=c++20", "-include", ENV_ON_H, "-O0", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                   "-I" + str(tmp), str(ROOT / "tests/display_startup_failure_test.cpp"), "-o", str(exe)]
        result = subprocess.run(command, cwd=tmp, env=env, capture_output=True, text=True, timeout=60,
                                creationflags=flags)
        if result.returncode:
            print(result.stdout + result.stderr)
            return result.returncode
        failed = 0
        for case in args.case or CASES:
            result = subprocess.run([str(exe), case], cwd=tmp, env=env, capture_output=True, text=True,
                                    timeout=10, creationflags=flags)
            print((result.stdout + result.stderr).strip())
            expected = ("display: no SDL: no window" if case == "load_failure" else
                        "display: SDL_Init: authored missing Vulkan loader: no window" if case == "init_failure" else
                        "display: SDL_CreateWindow: authored missing Vulkan loader: no window" if case == "window_failure" else None)
            diagnostic_missing = expected is not None and expected not in result.stderr
            if diagnostic_missing:
                print("FAIL %s: missing stderr startup diagnostic" % case)
            failed += result.returncode != 0 or diagnostic_missing
        print("display startup: %d cases, %d failed" % (len(args.case or CASES), failed))
        return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
