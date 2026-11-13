"""A self-contained Builder must have nonempty startup components before setup can launch it.

Uses the production helper and real Windows file APIs with authored files, never the installed Builder.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"linux": "Windows setup uses Win32 file APIs"}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="clang++")
    ap.add_argument("--source", type=Path, default=ROOT / "builder/setup/setup.c")
    args = ap.parse_args()
    text = args.source.read_text(encoding="utf-8")
    helper = text[text.index("static int builder_ready("):text.index("static const char *manifest_url(")]
    required = re.findall(r'L"([^"]+)"', helper)
    assert required and len(set(required)) == len(required)
    prelude = '#define WIN32_LEAN_AND_MEAN\n#define _CRT_SECURE_NO_WARNINGS\n#include <windows.h>\n#include <shellapi.h>\n#include <wchar.h>\n#include <stdio.h>\n'
    driver = '\nint main(void) { int argc; wchar_t **argv=CommandLineToArgvW(GetCommandLineW(), &argc); if(!argv || argc != 2) return 2; printf("%d\\n",builder_ready(argv[1])); LocalFree(argv); return 0; }\n'
    with tempfile.TemporaryDirectory(prefix="hookshot-setup-runtime-") as temp:
        root = Path(temp)
        source = root / "fixture.cpp"
        source.write_text(prelude + helper + driver, encoding="utf-8")
        exe = root / "fixture.exe"
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-lshell32", "-o", str(exe)], check=True)
        folder = root / "Builder space é"
        folder.mkdir()
        names = ["HookshotBuilder.exe"] + required
        for name in names:
            (folder / name).write_bytes(b"authored fixture, not a program")
        app = folder / names[0]
        count = 0

        def check(path, expected, case):
            nonlocal count
            p = subprocess.run([str(exe), str(path)], capture_output=True, text=True, check=True)
            assert p.stdout.strip() == str(expected), (case, p.stdout, p.stderr)
            count += 1

        check(app, 1, "complete")
        (folder / "unrelated.txt").write_text("extra files do not invalidate startup")
        check(app, 1, "extra file")
        for name in names:
            path = folder / name
            path.unlink()
            check(app, 0, "missing " + name)
            path.touch()
            check(app, 0, "empty " + name)
            path.unlink()
            path.mkdir()
            check(app, 0, "directory " + name)
            path.rmdir()
            path.write_bytes(b"restored authored fixture")
            check(app, 1, "restored " + name)
        check("no-directory.exe", 0, "no parent")
        check("C:\\" + "x" * 300, 0, "overlong path")
        print("setup runtime: %d native file-readiness cases pass" % count)


if __name__ == "__main__":
    main()
