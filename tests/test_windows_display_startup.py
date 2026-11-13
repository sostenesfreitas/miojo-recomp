"""Exercise the production Windows display-startup entry point with API mocks."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cc", "gcc"]


ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "cl")
    parser.add_argument("--source", type=Path, default=ROOT / "host/display.c")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    template = (ROOT / "tests/windows_display_startup_test.c").read_text(encoding="utf-8")
    failure_helper = "static void display_start_failed("
    functions = [function(source, failure_helper)] if failure_helper in source else []
    functions.append(function(source, "void hos_display_start(void)"))
    create = source.index("hwnd = CreateWindowA(")
    dpi = source.index("if (window_dpi(hwnd)", create)
    functions.append("static void create_window(void) {\n" + source[create:dpi] + "}\n")
    fixture = template.replace("/* ACTUAL_DISPLAY_STARTUP */", "\n".join(functions))
    with tempfile.TemporaryDirectory(prefix="hookshot-display-startup-") as temp:
        c_file, executable = Path(temp) / "fixture.c", Path(temp) / "fixture.exe"
        c_file.write_text(fixture, encoding="utf-8")
        if Path(args.cc).name.lower() in ("cl", "cl.exe"):
            flags = ["/nologo", "/TC", "/W4", "/WX", f"/Fe:{executable}"]
        else:
            flags = ["-std=c11", "-include", ENV_ON_H, "-O0", "-Wall", "-Wextra", "-Werror", "-Wno-unknown-warning-option", "-Wno-unused-but-set-global",
                     "-o", str(executable)]   # (newer clang: a fixture global set and never read)
        subprocess.run([args.cc, *flags, str(c_file)], cwd=temp, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
