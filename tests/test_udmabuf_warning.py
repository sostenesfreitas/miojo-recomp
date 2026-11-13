"""udmabuf permission warnings reach stderr once, including when startup redirects the log."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "Linux startup stderr duplication (POSIX file descriptors)"}
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    src = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    start = src.index('const int u = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);')
    code = src[src.index("\n", start):src.index("if (u >= 0)", start)]
    redirect = src[src.index("startup_stderr = fcntl("):src.index('if (freopen(path, "w", stderr))')]
    redirect = redirect.replace("#endif", "")
    fixture = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define hos_log(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
static int startup_stderr = -1;
static void probe(int u) {
    (void)u;
@@CODE@@
}
int main(int argc, char **argv) {
    if (argc == 3) {
        @@REDIRECT@@
        if (!freopen(argv[2], "w", stderr)) return 2;
    }
    const int errors[] = {EACCES, EPERM, ENOENT, EIO, EACCES};
    const int test = atoi(argv[1]);
    for (int i = 0; i < 2; ++i) {
        errno = errors[test];
        probe(test == 4 ? 3 : -1);
    }
    return startup_stderr != -1;
}
'''
    with tempfile.TemporaryDirectory(prefix="udmabuf-warning-") as tmp:
        c, exe, log = (Path(tmp) / name for name in ("fixture.c", "fixture", "log.txt"))
        c.write_text(fixture.replace("@@CODE@@", code).replace("@@REDIRECT@@", redirect), encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        for redirected in (False, True):
            for case in range(5):
                result = subprocess.run([str(exe), str(case)] + ([str(log)] if redirected else []),
                                        capture_output=True, text=True, check=True)
                assert not result.stdout
                if redirected:
                    assert log.read_text() == result.stderr
                if case < 2:
                    assert result.stderr.count("WARNING:") == 1, result.stderr
                    for word in ("/dev/udmabuf", "permission", "group", "udev", "README", "AMD"):
                        assert word in result.stderr, (word, result.stderr)
                else:
                    assert not result.stderr, result
    print("PASS: EACCES/EPERM warn once in both sinks; ENOENT/EIO/success stay quiet")


if __name__ == "__main__":
    main()
