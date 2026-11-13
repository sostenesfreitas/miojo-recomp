"""The profile readers (scripts/prof.py, crit.py, hotlist.py) name frames from both builds.

Windows samples name hookshot.exe / hookshot_vk.dll, read through the linker maps; Linux samples name hookshot /
hookshot_vk.so, read through the binaries' own symbols (nm). Before, the Linux names were unknown: crit.py counted every
Linux thread as never running (0.0%) and named nothing. A small position-independent executable called hookshot prints
its functions' offsets from its load address, as a sample has them; crit.py and hotlist.py must name and count them. A
synthetic linker map checks the Windows path. The ELF part needs a C compiler and nm (skipped without them).
python -I -B tests/test_prof_symbols.py [--cc gcc]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"

PROBE = r'''
#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <stdint.h>
__attribute__((noinline)) int f_80001000(int x) { return x * 3 + 1; }
__attribute__((noinline)) int prof_symbol_probe(int x) { return f_80001000(x) + 2; }
static int first(struct dl_phdr_info *i, size_t n, void *out) { (void)n; *(uintptr_t *)out = i->dlpi_addr; return 1; }
int main(void) {
    uintptr_t base = 0;
    dl_iterate_phdr(first, &base);
    printf("%lx %lx\n", (unsigned long)((uintptr_t)&f_80001000 - base), (unsigned long)((uintptr_t)&prof_symbol_probe - base));
    return prof_symbol_probe(1) == 6 ? 0 : 1;
}
'''


def run(script, args, maps):
    env = dict(os.environ, RX_MAPS=str(maps))
    r = subprocess.run([sys.executable, "-I", str(SCRIPTS / script)] + args, capture_output=True, text=True, env=env)
    if r.returncode:
        raise SystemExit("FAIL: %s exited %d:\n%s%s" % (script, r.returncode, r.stdout, r.stderr))
    return r.stdout


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def windows_map(tmp):
    """the linker-map path, unchanged: a frame in hookshot.exe named from hookshot.map"""
    maps = tmp / "win"
    maps.mkdir()
    (maps / "hookshot.map").write_text(
        " Address         Publics by Value              Rva+Base               Lib:Object\n\n"
        " 0001:00000000       f_80002000                 0000000140001000 f   code.obj\n"
        " 0001:00000100       f_80002400                 0000000140001100 f   code.obj\n")
    samples = tmp / "win_all.txt"
    samples.write_text("".join("%d sead::MainThread|hookshot.exe+1010;hookshot.exe+1110\n" % (2 * i) for i in range(1, 6)))
    out = run("crit.py", [str(samples)], maps)
    check("100.0%  sead::MainThread" in out, "crit.py: the Windows main thread runs in every sample\n" + out)
    hot = tmp / "win_hot.txt"
    run("hotlist.py", [str(samples), str(hot)], maps)
    check(hot.read_text().split()[0] == "f_80002000", "hotlist.py: the innermost lifted function from the map, first")


def linux_elf(tmp, cc):
    if os.name == "nt" or not shutil.which(cc) or not shutil.which("nm"):
        print("skip: the ELF part needs %s and nm" % cc)
        return
    maps = tmp / "linux"
    maps.mkdir()
    src = tmp / "probe.c"
    src.write_text(PROBE)
    exe = maps / "hookshot"
    subprocess.run([cc, "-O1", "-fPIE", "-pie", "-o", str(exe), str(src)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0, "the probe runs")
    lifted, probe = (int(x, 16) for x in r.stdout.split())
    # (the main thread inside f_80001000 called from prof_symbol_probe; the replay thread blocked in libc)
    lines = []
    for i in range(1, 9):
        lines.append("%d sead::MainThread|hookshot+%x;hookshot+%x;libc.so.6+2a1ca\n" % (2 * i, lifted + 2, probe + 4))
        lines.append("%d Presentation_Thread|libc.so.6+98d61;hookshot+%x\n" % (2 * i, probe + 4))
    samples = tmp / "linux_all.txt"
    samples.write_text("".join(lines))
    out = run("crit.py", [str(samples)], maps)
    check("100.0%  sead::MainThread" in out, "crit.py: a Linux thread in hookshot is running\n" + out)
    check("0.0%  Presentation_Thread" in out, "crit.py: a Linux thread in libc is blocked\n" + out)
    hot = tmp / "linux_hot.txt"
    run("hotlist.py", [str(samples), str(hot)], maps)
    check(hot.read_text().split()[0] == "f_80001000", "hotlist.py: the lifted function named from the ELF symbols, first")
    sys.path.insert(0, str(SCRIPTS))
    import prof
    addrs, names = prof.load_symbols(str(maps))["hookshot"]
    check("prof_symbol_probe" in names and addrs[names.index("prof_symbol_probe")] == probe,
          "prof.load_symbols: the symbol's offset is the one a sample gives")


def main():
    from unittest.mock import patch
    from types import SimpleNamespace
    sys.path.insert(0, str(SCRIPTS))
    import prof
    symbols = [
        "mkwvk::(anonymous namespace)::build_descriptors(int)",
        "mkwvk::(anonymous namespace)::texture_view_of(unsigned int)",
        "mkwvk::Recorder::operator()(int)",
        "mkwvk::Recorder::operator()(float)",
    ]
    output = "\n".join("%016x t %s" % (0x1000 + i * 16, name)
                       for i, name in enumerate(symbols))
    with patch.object(prof.subprocess, "run", return_value=SimpleNamespace(stdout=output)):
        addresses, names = prof.load_elf("synthetic.so")
    check(names == symbols and addresses == [0x1000, 0x1010, 0x1020, 0x1030],
          "anonymous namespace functions and overloaded operators keep distinct names")
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        windows_map(tmp)
        linux_elf(tmp, a.cc)
    print("ok")


if __name__ == "__main__":
    main()
