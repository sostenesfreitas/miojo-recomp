"""Exercise Linux Builder's detached Play path with an authored executable, and with one that decides where to log by the
game's own rule (host/posix/win32_posix.c GetFileType, as host/hos_main.c uses it): Play must leave it its hookshot_log.txt."""
import argparse
import ast
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
from types import SimpleNamespace

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "uses the Linux Builder's /bin/sh launch path"}

ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / "builder/linux"


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


def line(source, start):
    at = source.index("\n" + start) + 1
    return source[at:source.index("\n", at)]


def logging_game(cxx, exe):
    """A game that prints a line before its log opens, then opens hookshot_log.txt only when stderr is nowhere to log, as
    host/hos_main.c does, by the shim's own GetFileType, and logs 16384 lines (more than Play's 64 KiB)."""
    game_main = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    assert "GetFileType(err) == FILE_TYPE_UNKNOWN" in game_main and '"hookshot_log.txt"' in game_main, \
        "host/hos_main.c no longer opens hookshot_log.txt when GetFileType(stderr) is FILE_TYPE_UNKNOWN: update this test"
    shim = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    source = "\n".join([
        '#include <stdint.h>\n#include <stdio.h>\n#include <sys/stat.h>\n#include <unistd.h>\n#include "windows.h"',
        line(shim, "enum { H_THREAD"), line(shim, "typedef struct { int type; } HObj;"),
        line(shim, "static int is(HANDLE h, int type)"), line(shim, "typedef struct { HObj h; int fd; } HFile;"),
        function(shim, "DWORD GetFileType(HANDLE h) {"),
        "int main() {",
        '    fputs("before the log\\n", stderr);',
        "    HFile err = {{H_FILE}, 2};",
        '    if (GetFileType(&err) == FILE_TYPE_UNKNOWN && freopen("hookshot_log.txt", "w", stderr))',
        '        for (int i = 0; i < 16384; i++) fprintf(stderr, "log line %d\\n", i);',
        "    return 73;",
        "}",
    ])
    cpp = exe.with_name("logging_game.cpp")
    cpp.write_text(source + "\n", encoding="utf-8")
    subprocess.run([cxx, "-std=c++20", "-O0", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "host/posix"), str(cpp),
                    "-o", str(exe)], check=True, timeout=120)
    cpp.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"), help="GCC/Clang C++ compiler")
    args = parser.parse_args()
    if os.name != "posix":
        return
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    path = BUILDER / "hookshot_builder.py"
    play = next(node for node in ast.parse(path.read_text(encoding="utf-8")).body
                if isinstance(node, ast.FunctionDef) and node.name == "play")
    processes = []

    def launch(command, **options):
        assert options["start_new_session"]
        processes.append(subprocess.Popen(command, **options))

    namespace = dict(HERE=str(BUILDER), os=os, socket=socket, own_scope=lambda env: [],   # (test_linux_play_scope.py has it)
                     linux_update=SimpleNamespace(game_exe=lambda folder: str(Path(folder) / "hookshot")),
                     make_game=SimpleNamespace(game_env=lambda: dict(os.environ)),
                     subprocess=SimpleNamespace(Popen=launch, DEVNULL=subprocess.DEVNULL))
    exec(compile(ast.Module(body=[play], type_ignores=[]), str(path), "exec"), namespace)

    with tempfile.TemporaryDirectory(prefix="hookshot-play-") as directory:
        folder = Path(directory)
        game = folder / "hookshot"
        game.write_text(
            '#!/bin/sh\n'
            'printf "stdout sentinel\\n"\n'
            'printf "stderr sentinel\\n" >&2\n'
            'head -c 1048576 /dev/zero >&2\n'
            'touch completed\n'
            'exit 73\n', encoding="utf-8")
        game.chmod(0o700)
        for _ in range(2):
            namespace["play"](str(folder))
        for process in processes:
            assert process.wait(timeout=10) == 0
        log = folder / "hookshot_startup.log"
        data = log.read_bytes()
        assert data == b"stderr sentinel\n" + bytes(65536 - len(b"stderr sentinel\n")) + b"\nHookshot exited with status 73\n"
        assert (folder / "completed").is_file() and log.stat().st_mode & 0o777 == 0o600
        assert not list(folder.glob(".hookshot-startup.*"))

        # A non-writable destination must not prevent the game from running.
        log.unlink()
        log.mkdir()
        (folder / "completed").unlink()
        namespace["play"](str(folder))
        processes[-1].wait(timeout=10)
        assert (folder / "completed").is_file() and not list(log.iterdir())
        assert not list(folder.glob(".hookshot-startup.*"))

        # A failed real scope start uses the same bounded diagnostics, including why Play fell back.
        log.rmdir()
        namespace.update(own_scope=lambda env: ["scope"], start_scoped=lambda *args: False)
        namespace["play"](str(folder))
        assert processes[-1].wait(timeout=10) == 0
        assert log.read_bytes() == data + b"Hookshot: systemd scope hand-off failed; started without a separate scope.\n"
        assert not list(folder.glob(".hookshot-startup.*"))
        namespace["own_scope"] = lambda env: []

    # The game's own log: a pipe on stderr is a reader to the game (FILE_TYPE_PIPE), and it then writes no hookshot_log.txt.
    with tempfile.TemporaryDirectory(prefix="hookshot-play-log-") as directory:
        folder = Path(directory)
        logging_game(cxx, folder / "hookshot")
        namespace["play"](str(folder))
        assert processes[-1].wait(timeout=10) == 0
        assert (folder / "hookshot_log.txt").is_file(), \
            "Play gave the game a stderr it takes for a reader (GetFileType not FILE_TYPE_UNKNOWN): no hookshot_log.txt"
        lines = (folder / "hookshot_log.txt").read_text(encoding="utf-8").splitlines()
        assert lines == ["log line %d" % i for i in range(16384)], "hookshot_log.txt is not the game's whole log"
        assert (folder / "hookshot_startup.log").read_bytes() == b"before the log\n\nHookshot exited with status 73\n"
        assert not list(folder.glob(".hookshot-startup.*"))
    print("PASS: detached Play, bounded stderr, exit status, concurrent launches, publication failure, and the game's own "
          "log beside the startup lines, including scope fallback")


if __name__ == "__main__":
    main()
