"""One rule for on/off switches: env_on (gpu/vk/compat/hookshot_env.h) in C and C++, kit.env_on in Python.

Unset, empty, 0, false, off and no (any case, spaces around them not counted) are off; 1 and anything else are on, so
RX_VK_NO_PROGRAM_CACHE=0 no longer turns the feature on. env_int reads a number with a default. Compile the header as C
and as C++ and check both, and the Python twin, against the same table. Also check that no file in host/ or gpu/vk reads
an on/off switch the old way, getenv(name) != NULL, for a name it reads nowhere else for a value. No game or GPU is needed.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402

CASES = [(None, 0), ("", 0), ("0", 0), (" 0 ", 0), ("false", 0), ("FALSE", 0), ("Off", 0), ("no", 0), ("0\r\n", 0),
         ("1", 1), ("2", 1), ("yes", 1), ("on", 1), ("true", 1), ("00", 1), ("2c56dc1006942f74", 1), ("dir/x", 1)]

FIXTURE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hookshot_env.h"
#ifdef _WIN32
static void put(const char* name, const char* value) {
    char s[256];
    snprintf(s, sizeof s, "%s=%s", name, value ? value : "");
    _putenv(s);
}
#else
static void put(const char* name, const char* value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}
#endif
int main(int argc, char** argv) {
    /* argv: [value|-unset] want ... */
    int i;
    for (i = 1; i + 1 < argc; i += 2) {
        const char* v = strcmp(argv[i], "-unset") ? argv[i] : NULL;
        put("HOOKSHOT_T_SWITCH", v);
        if (env_on("HOOKSHOT_T_SWITCH") != atoi(argv[i + 1])) {
            fprintf(stderr, "FAIL: env_on with [%s] is %d\n", v ? v : "(unset)", env_on("HOOKSHOT_T_SWITCH"));
            return 1;
        }
    }
    put("HOOKSHOT_T_NUMBER", "12");
    if (env_int("HOOKSHOT_T_NUMBER", 5) != 12) return 2;
    put("HOOKSHOT_T_NUMBER", "x");
    if (env_int("HOOKSHOT_T_NUMBER", 5) != 5) return 3;
    put("HOOKSHOT_T_NUMBER", NULL);
    if (env_int("HOOKSHOT_T_NUMBER", 5) != 5) return 4;
    puts("PASS");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    for value, want in CASES:   # (Python's twin)
        env = os.environ.copy()
        os.environ.pop("HOOKSHOT_T_SWITCH", None)
        if value is not None:
            os.environ["HOOKSHOT_T_SWITCH"] = value
        got = kit.env_on("HOOKSHOT_T_SWITCH")
        os.environ.clear(), os.environ.update(env)
        if got != bool(want):
            raise SystemExit("FAIL: kit.env_on with [%r] is %s" % (value, got))

    argv = []
    for value, want in CASES:
        if value is not None and ("\r" in value or "\n" in value):
            continue   # (not passed through a command line)
        argv += ["-unset" if value is None else value, str(want)]
    include = ROOT / "gpu" / "vk" / "compat"
    with tempfile.TemporaryDirectory(prefix="env-on-") as tmp:
        for lang, suffix, std in (("C", ".c", "-std=c11"), ("C++", ".cpp", "-std=c++20")):
            src = Path(tmp) / ("fixture" + suffix)
            exe = Path(tmp) / ("fixture_%s%s" % (suffix[1:], ".exe" if os.name == "nt" else ""))
            src.write_text(FIXTURE, encoding="utf-8")
            flags = ["-x", "c", "-D_POSIX_C_SOURCE=200809L"] if lang == "C" else []   # (C++'s compiler, told it is C)
            # (_CRT_SECURE_NO_WARNINGS: the fixture's stdio.h comes before hookshot_env.h, and Windows' CRT then calls getenv
            # deprecated for the rest of the file)
            subprocess.run([args.cxx, std, "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", *flags, "-I", str(include),
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe), *argv], check=True)

    # (no on/off switch read the old way, set-means-on: a game switch (RX_, HOS_, HOOKSHOT_, EDEN_, NVN_) whose every read in
    # host/ or gpu/ only asks whether it is set: getenv(name) != NULL / == NULL, !getenv(name), getenv(name) followed by ?,
    # && or ||, or alone between ( or && / || and ) or && / || or the line's end. A line that reads the name twice reads its
    # value too (getenv(x) ? atoi(getenv(x)) : ...), and a getenv inside a call (atoi(getenv(x)), parse(getenv(x))) is a value)
    name = r'(?:std::)?getenv\("((?:RX|HOS|HOOKSHOT|EDEN|NVN)_[A-Z0-9_]+)"\)'
    forms = [re.compile(name + r"\s*[!=]=\s*(?:nullptr|NULL)"), re.compile(r"!\s*" + name),
             re.compile(name + r"\s*(?:\?|&&|\|\|)"),
             re.compile(r"(?:(?<![A-Za-z0-9_])\(|(?<=\bif)\(|(?<=while)\(|&&|\|\|)\s*" + name + r"\s*(?:\)|&&|\|\||$)")]
    reads, presence = {}, {}
    for path in sorted(p for d in ("host", "gpu") for p in (ROOT / d).rglob("*") if p.suffix in (".c", ".cpp", ".h")):
        rel = path.relative_to(ROOT).as_posix()
        if "gpu/render" in rel or "third_party" in rel or "/tests/" in rel or "sirit" in rel:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").split("\n"), 1):
            if line.strip().startswith(("//", "/*", "*")):
                continue
            for m in re.finditer(name, line):
                n = m.group(1)
                reads[n] = reads.get(n, 0) + 1
                if line.count('getenv("%s")' % n) == 1 and any(f.group(1) == n for form in forms for f in form.finditer(line)):
                    presence.setdefault(n, []).append("%s:%d" % (rel, number))
    old = sorted(n for n, where in presence.items() if len(where) == reads.get(n, 0))
    if old:
        raise SystemExit("FAIL: on/off switches still read as set-means-on (set to 0 they are on): use env_on:\n  " +
                         "\n  ".join("%s (%s)" % (n, ", ".join(presence[n])) for n in old))
    print("PASS: env_on's rule in C, C++ and Python (%d values); no on/off switch read the old way" % len(CASES))


if __name__ == "__main__":
    main()
