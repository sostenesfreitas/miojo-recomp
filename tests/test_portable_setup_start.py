"""Execute real setup initialization (its Builder home, portable or standard) with module/profile boundaries intercepted."""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "Windows setup startup contract"}


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = (ROOT / "builder/setup/setup.c").read_text(encoding="utf-8")
    code = r'''
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdlib.h>
static int deny_writes;
static HANDLE fixture_open(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE template_file) {
    if (deny_writes && (access & GENERIC_WRITE)) { SetLastError(ERROR_WRITE_PROTECT); return INVALID_HANDLE_VALUE; }
    return CreateFileW(path,access,share,security,creation,flags,template_file);
}
#define CreateFileW fixture_open
#include "portable_home.h"
static HbHome g_home;
static int g_home_ok;
static wchar_t module[MAX_PATH], profile[MAX_PATH];
static DWORD fake_module(HMODULE m, wchar_t *out, DWORD cap) {
    (void)m; if (wcslen(module) >= cap) return cap;
    wcscpy(out,module); return (DWORD)wcslen(out);
}
static HRESULT fake_profile(HWND a,int b,HANDLE c,DWORD d,wchar_t *out) {
    (void)a;(void)b;(void)c;(void)d; wcscpy(out,profile); return S_OK;
}
#define GetModuleFileNameW fake_module
#define SHGetFolderPathW fake_profile
'''
    for marker in ("static void mkdirs(", "static int portable_prefix(", "static int init_home("):
        code += function(source, marker) + "\n"
    code += r'''
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"startup check failed at %d\n",__LINE__); return 1; } } while(0)
int wmain(int argc, wchar_t **argv) {
    if (argc != 5) return 10;
    wcscpy(module,argv[1]); wcscpy(profile,argv[2]);
    wchar_t command[512] = L" \t\"--portable\"  --cli --update \"Game ü, folder\"";
    CHECK(portable_prefix(command) && !wcscmp(command,L"--cli --update \"Game ü, folder\""));
    wcscpy(command,L"--portable-other --cli"); CHECK(!portable_prefix(command));
    wcscpy(command,L"--portable"); CHECK(portable_prefix(command) && !*command);
    int mode = _wtoi(argv[4]);
    deny_writes = mode == 4;
    g_home_ok = init_home(mode == 1 || mode == 4);
    if (mode == 2 || mode == 4) { CHECK(!g_home_ok); return 0; }
    CHECK(g_home_ok && !wcscmp(g_home.root,argv[3]));
    CHECK(g_home.portable == (mode != 3));
    wchar_t inherited[MAX_PATH];
    CHECK(GetEnvironmentVariableW(L"HOOKSHOT_SETUP_BASE",inherited,MAX_PATH) && !wcscmp(inherited,argv[3]));
    if (mode == 1) CHECK(hb_home_marked(g_home.root)==1);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="portable-setup-start-") as folder:
        root = Path(folder)
        c, exe = root / "start.c", root / "start.exe"
        c.write_text(code, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-municode", "-I" + str(ROOT / "builder/setup"), str(c), "-o", str(exe)], check=True)
        tree = root / "Portable ü, tree"
        tree.mkdir()
        home = tree / "HookshotBuilder-data"   # (not HookshotBuilder: the builder the zip carries, beside the setup)
        profile = root / "authored AppData"
        setup = tree / "Setup.exe"
        import os
        env = {k: v for k, v in os.environ.items() if k.upper() not in ("HOOKSHOT_SETUP_BASE", "HOOKSHOT_BUILDER_PORTABLE")}
        count = 0
        def run(module, expected, mode, overrides=None):
            nonlocal count
            subprocess.run([str(exe), str(module), str(profile), str(expected), str(mode)],
                           env=dict(env, **(overrides or {})), cwd=root, check=True)
            count += 1
        run(setup, home, 1)
        run(setup, home, 0)
        run(root / "downloaded-setup.exe", home, 0, {"HOOKSHOT_SETUP_BASE": str(home), "HOOKSHOT_BUILDER_PORTABLE": "1"})
        moved = root / "Relocated ü, tree"
        tree.rename(moved)
        home = moved / "HookshotBuilder-data"
        run(moved / "Setup.exe", home, 0)
        run(root / "standard-setup.exe", profile / "HookshotBuilder", 3)
        run(root / "downloaded-setup.exe", home, 2, {"HOOKSHOT_SETUP_BASE": "relative"})
        run(root / "downloaded-setup.exe", home, 2, {"HOOKSHOT_SETUP_BASE": str(profile), "HOOKSHOT_BUILDER_PORTABLE": "1"})
        readonly = root / "write-protected fixture"
        readonly.mkdir()
        run(root / "downloaded-setup.exe", readonly, 4, {"HOOKSHOT_SETUP_BASE": str(readonly)})
        assert not (readonly / ".hookshot-portable").exists()
        marker = home / ".hookshot-portable"
        marker.write_text("broken")
        run(moved / "Setup.exe", home, 2)
        assert not profile.exists(), "No persistent AppData writes expected during fixture startup"
        print(f"portable setup: {count} startup scenarios passed; write-protected file boundary, real roots/environment/markers")


if __name__ == "__main__":
    main()
