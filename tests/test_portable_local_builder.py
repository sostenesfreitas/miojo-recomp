"""Compile the production Windows local-launcher selector; authored files, no application launches."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "Windows local launcher selection"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = (ROOT / "host/hos_update.c").read_text(encoding="utf-8")
    reader = source[source.index("static int read_value(const wchar_t"):source.index("static char *get_body")]
    selector = source[source.index("static int local_builder("):source.index("void hos_update_check(void)")]
    with tempfile.TemporaryDirectory(prefix="portable-launcher-") as folder:
        root = Path(folder)
        code = '#include <windows.h>\n#include <stdio.h>\n#include <string.h>\n#include <wchar.h>\n' + reader + selector + r'''
int wmain(int argc, wchar_t **argv) {
    if (argc != 4) return 20;
    wchar_t out[MAX_PATH];
    const int found = local_builder(argv[1], argv[2], out, MAX_PATH);
    if (found != (argv[3][0] == L'1')) return 21;
    if (found) {
        wchar_t expected[MAX_PATH];
        _snwprintf(expected, MAX_PATH, L"%ls\\hookshot.exe", argv[2]);
        if (wcscmp(expected, out)) return 22;
        if (local_builder(argv[1], argv[2], out, 3)) return 23;
    }
    return 0;
}
'''
        c = root / "selector.c"
        exe = root / "selector.exe"
        c.write_text(code, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-municode", str(c), "-o", str(exe)], check=True)
        game = root / "Game ü, moved"
        game.mkdir()
        metadata = game / "hookshot_version.txt"
        launcher = game / "hookshot.exe"
        metadata.write_text("builder_local=1\nbuilder=C:\\unrelated.exe\nbuilder_args=ignored\n")
        def run(expected):
            subprocess.run([str(exe), str(metadata), str(game), str(expected)], cwd=root, check=True)
        run(0)
        launcher.write_text("authored; never launched")
        run(1)
        moved = root / "Moved ü, Game"
        game.rename(moved)
        game, metadata, launcher = moved, moved / metadata.name, moved / launcher.name
        run(1)
        launcher.unlink()
        launcher.mkdir()
        run(0)
        launcher.rmdir()
        launcher.write_text("authored")
        for mode in ("", "0", "../other.exe", "10"):
            metadata.write_text("builder_local=" + mode + "\n")
            run(0)
        print("portable local launcher: 8 native scenarios passed; bounded output and relocated owning path verified")


if __name__ == "__main__":
    main()
