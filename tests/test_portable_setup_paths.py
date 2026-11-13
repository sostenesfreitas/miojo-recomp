"""Execute production native path formatting at its derived-path boundary; no file operations."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "Windows native setup MAX_PATH contract"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = (ROOT / "builder/setup/setup.c").read_text(encoding="utf-8")
    start = source.index("static int home_path(")
    end = source.index("\n}", start) + 2
    production = source[start:end]
    code = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
static struct { int portable; } g_home;
static void fail(const wchar_t *format, ...) { (void)format; exit(90); }
''' + production + r'''
int main(int argc, char **argv) {
    if (argc != 3) return 20;
    g_home.portable = atoi(argv[1]);
    int length = atoi(argv[2]);
    wchar_t downloads[MAX_PATH], hash[65], zip[MAX_PATH];
    if (length < 3 || length >= MAX_PATH) return 21;
    wmemset(downloads,L'd',(size_t)length); downloads[length]=0;
    downloads[0]=L'C'; downloads[1]=L':'; downloads[2]=L'\\';
    wmemset(hash,L'a',64); hash[64]=0;
    int n = home_path(zip,L"%ls\\%ls.zip",downloads,hash);
    if (length == 190) return n == 259 && wcslen(zip) == 259 ? 0 : 22;
    if (g_home.portable) { fprintf(stderr,"portable path guard did not stop an oversized derived path\n"); return 23; }
    return n < 0 || n >= MAX_PATH ? 0 : 24;
}
'''
    with tempfile.TemporaryDirectory(prefix="portable-native-path-") as folder:
        root = Path(folder)
        c, exe = root / "path.c", root / "path.exe"
        c.write_text(code, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        for portable, length, expected in ((1,190,0), (1,191,90), (1,240,90), (0,190,0), (0,191,0)):
            result = subprocess.run([str(exe), str(portable), str(length)], capture_output=True, text=True)
            assert result.returncode == expected, (portable, length, expected, result.returncode, result.stderr)
        print("portable derived paths: five boundary scenarios passed; oversized paths stop before file operations")


if __name__ == "__main__":
    main()
