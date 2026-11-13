"""Windows paths as UTF-8: the game exe's manifest makes its code page UTF-8 (nx2/win_manifest.py), so a game folder named
outside the system's ANSI code page (Japanese on a Western Windows, an emoji anywhere) opens; before, the game closed at start.

Everywhere: the manifest resource's bytes, that nx2/build.py links it into hookshot.exe, that host/hos_main.c checks at start,
and that host/ and gpu/vk convert no text with the ANSI code page (CP_ACP and the like: UTF-8 is the one code page of the game's
char strings). With a compiler for Windows: tests/utf8_paths_test.c (host/hos_main.c's narrow_paths_ok pasted in) built with the
manifest, and once without it. On Windows both run from a folder named "ゲーム Zélda 🎮": with the manifest the A functions,
fopen, argv, getenv and the log (stderr freopen'd, as hookshot_log.txt) are UTF-8 and the paths round-trip; without it the
start-up check refuses the folder (as the game used to fail).
Elsewhere the build is checked to carry the manifest resource (HOOKSHOT_MINGW=<llvm-mingw> on Linux).

    python -I tests/test_utf8_paths.py [--cc <clang for Windows>]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import win_manifest

# (the game's own sources: not host/posix, the Linux stand-ins, nor the vendored emulator and compiler code)
SOURCES = [ROOT / "host", ROOT / "gpu" / "vk"]
VENDORED = ("host/posix/", "gpu/vk/eden/", "gpu/vk/sirit/", "gpu/vk/compat/")
ANSI = re.compile(r"\b(CP_ACP|CP_OEMCP|CP_THREAD_ACP|CP_MACCP|SetFileApisToOEM|SetFileApisToANSI)\b")


def resources(data):
    """(type, name, language, data) of each entry of a .res file"""
    out, at = [], 0
    while at < len(data):
        size, header = struct.unpack_from("<II", data, at)
        ff1, kind, ff2, name = struct.unpack_from("<HHHH", data, at + 8)
        assert ff1 == 0xFFFF and ff2 == 0xFFFF and header == 32, "numeric type and name only"
        language = struct.unpack_from("<H", data, at + 22)[0]
        out.append((kind, name, language, data[at + header:at + header + size]))
        at += header + size + (-size & 3)
    return out


def check_manifest():
    entries = resources(win_manifest.res_bytes())
    assert entries[0][:2] == (0, 0) and not entries[0][3], "a .res starts with an empty entry"
    assert [(k, n) for k, n, _, _ in entries[1:]] == [(24, 1)], "one RT_MANIFEST, resource 1 (what the loader reads for an exe)"
    root = ET.fromstring(entries[1][3])
    page = root.findall(".//{http://schemas.microsoft.com/SMI/2019/WindowsSettings}activeCodePage")
    assert len(page) == 1 and page[0].text == "UTF-8", "activeCodePage UTF-8"
    level = root.find(".//{urn:schemas-microsoft-com:asm.v3}requestedExecutionLevel")
    assert level is not None and level.get("level") == "asInvoker", "no elevation asked for"


def check_build_links_it():
    src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    start = src.index('exe = os.path.join(out, "hookshot.exe")')
    link = src[start:src.index("capture_output=True", start)]
    assert "win_manifest.write(" in link, "nx2/build.py links hookshot.exe without nx2/win_manifest.py's resource"


def check_start_up():
    src = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    main = src.index("int main(int argc, char **argv) {\n#else")
    first = src[main:src.index("load_hookshot_env();", main)]
    assert "narrow_paths_ok(argc, argv)" in first, "host/hos_main.c's main checks its folder's path before anything opens a file"


def check_no_ansi_code_page():
    bad = []
    for top in SOURCES:
        for p in sorted(top.rglob("*")):
            rel = p.relative_to(ROOT).as_posix()
            if p.suffix not in (".c", ".h", ".cpp", ".hpp") or rel.startswith(VENDORED):
                continue
            for i, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if ANSI.search(line):
                    bad.append("%s:%d: %s" % (rel, i, line.strip()))
    assert not bad, "text converted with the ANSI code page (the game's char strings are UTF-8: CP_UTF8):\n" + "\n".join(bad)


def narrow_paths_ok_source():
    src = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    start = src.index("static int narrow_paths_ok(")
    end = src.index("{", start) + 1
    depth = 1
    while depth:
        depth += (src[end] == "{") - (src[end] == "}")
        end += 1
    return src[start:end]


def windows_cc(given):
    """a C compiler that makes Windows programs: --cc, else clang on Windows, else llvm-mingw's (HOOKSHOT_MINGW)"""
    if given:
        return given
    if os.name == "nt":
        return shutil.which("clang")
    mingw = os.environ.get("HOOKSHOT_MINGW", "")
    cc = os.path.join(mingw, "bin", "x86_64-w64-mingw32-clang")
    return cc if mingw and os.path.exists(cc) else None


def has_manifest(exe, mingw):
    """the exe's resources (llvm-readobj) list an RT_MANIFEST"""
    readobj = shutil.which("llvm-readobj") or (os.path.join(mingw, "bin", "llvm-readobj") if mingw else None)
    if not readobj or not os.path.exists(readobj):
        return None
    out = subprocess.run([readobj, "--coff-resources", str(exe)], capture_output=True, text=True, check=True).stdout
    return "MANIFEST (ID 24)" in out


def check_probe(cc_arg):
    cc = windows_cc(cc_arg)
    if not cc:
        print("SKIP probe: no compiler for Windows here (clang on Windows, or HOOKSHOT_MINGW=<llvm-mingw>)")
        return
    template = (ROOT / "tests" / "utf8_paths_test.c").read_text(encoding="utf-8")
    assert template.count("/* ACTUAL_NARROW_PATHS_OK */") == 1
    with tempfile.TemporaryDirectory(prefix="hookshot-utf8-") as temp:
        build = Path(temp) / "build"
        run_dir = Path(temp) / "ゲーム Zélda 🎮"
        build.mkdir()
        run_dir.mkdir()
        source = build / "probe.c"
        source.write_text(template.replace("/* ACTUAL_NARROW_PATHS_OK */", narrow_paths_ok_source()), encoding="utf-8")
        res = win_manifest.write(str(build / "hookshot_manifest.res"))
        target = [] if os.name == "nt" or "mingw32" in os.path.basename(cc) else ["--target=x86_64-w64-mingw32"]
        flags = target + ["-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-fuse-ld=lld",
                          "-include", str(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h")]   # (env_on: narrow_paths_ok's)
        libs = ["-luser32"]
        with_manifest, without = build / "probe.exe", build / "probe_ansi.exe"
        subprocess.run([cc] + flags + [str(source), res, "-o", str(with_manifest)] + libs, check=True)
        subprocess.run([cc] + flags + [str(source), "-o", str(without)] + libs, check=True)
        mingw = os.environ.get("HOOKSHOT_MINGW", "")
        found = has_manifest(with_manifest, mingw)
        if found is not None:
            assert found, "the linked exe carries no RT_MANIFEST"
            assert not has_manifest(without, mingw), "the control build has a manifest of its own"
        if os.name != "nt":
            print("probe built for Windows%s; it runs on Windows only" % ("" if found is None else ", manifest linked in"))
            return
        for exe, mode in ((with_manifest, "utf8"), (without, "ansi")):
            copy = run_dir / exe.name
            shutil.copyfile(exe, copy)
            env = dict(os.environ, HOS_HEADLESS="1")
            r = subprocess.run([str(copy), mode, "Hookshot-DLSS ゼルダ Zélda"], capture_output=True, text=True, encoding="utf-8", errors="replace", env=env,
                               cwd=str(run_dir))
            print(r.stdout.strip())
            assert r.returncode == 0, "%s %s: %s%s" % (exe.name, mode, r.stdout, r.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="")
    args = parser.parse_args()
    check_manifest()
    check_build_links_it()
    check_start_up()
    check_no_ansi_code_page()
    check_probe(args.cc)
    print("PASS: Windows paths as UTF-8")


if __name__ == "__main__":
    main()
