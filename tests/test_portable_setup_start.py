"""Execute real setup initialization/fast-launch with module/profile/process boundaries intercepted."""
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
static void fail(const wchar_t *format, ...) { (void)format; exit(90); }
static wchar_t module[MAX_PATH], profile[MAX_PATH], started[4096], workdir[MAX_PATH];
static DWORD fake_module(HMODULE m, wchar_t *out, DWORD cap) {
    (void)m; if (wcslen(module) >= cap) return cap;
    wcscpy(out,module); return (DWORD)wcslen(out);
}
static HRESULT fake_profile(HWND a,int b,HANDLE c,DWORD d,wchar_t *out) {
    (void)a;(void)b;(void)c;(void)d; wcscpy(out,profile); return S_OK;
}
static BOOL fake_process(LPCWSTR a,LPWSTR b,LPSECURITY_ATTRIBUTES c,LPSECURITY_ATTRIBUTES d,BOOL e,DWORD f,LPVOID g,LPCWSTR h,LPSTARTUPINFOW i,LPPROCESS_INFORMATION j) {
    (void)a;(void)c;(void)d;(void)e;(void)f;(void)g;(void)i;
    wcscpy(started,b); wcscpy(workdir,h); j->hThread=j->hProcess=NULL;return TRUE;
}
static BOOL fake_close(HANDLE h) {(void)h;return TRUE;}
#define GetModuleFileNameW fake_module
#define SHGetFolderPathW fake_profile
#define CreateProcessW fake_process
'''
    for marker in ("static int builder_ready(", "static int home_path(", "static void mkdirs(", "static int portable_prefix(", "static int init_home(",
                   "static void read_small(", "static int list_name_ok("):
        code += function(source, marker) + "\n"
    # read_small must close real fixture handles; only the synthetic process handles use fake_close.
    code += "#define CloseHandle fake_close\n" + function(source, "static int launch_installed(")
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
    if (mode == 0) {
        CHECK(launch_installed(L"--update authored-game --fetched"));
        CHECK(wcsstr(started,L"--update authored-game --fetched") && wcsstr(workdir,argv[3])==workdir);
    }
    if (mode == 5) {
        CHECK(!launch_installed(L"--update authored-game --fetched"));
        CHECK(!*started);
    }
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
        home = tree / "HookshotBuilder"
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
        installed = home / "versions/v-authored/HookshotBuilder"
        installed.mkdir(parents=True)
        (installed / "HookshotBuilder.exe").write_text("authored; never executed")
        for name in re.findall(r'L"([^\"]+)"', function(source, "static int builder_ready(")):
            (installed / name).write_bytes(b"authored startup component; never executed")
        (home / "versions/current.txt").write_text("v-authored\n")
        run(setup, home, 0)
        runtime = installed / "coreclr.dll"
        runtime.unlink()
        run(setup, home, 5)
        runtime.write_bytes(b"authored startup component restored")
        run(setup, home, 0)
        run(installed / "HookshotBuilder-Setup.exe", home, 0)
        run(root / "downloaded-setup.exe", home, 0, {"HOOKSHOT_SETUP_BASE": str(home), "HOOKSHOT_BUILDER_PORTABLE": "1"})
        game = tree / "Game"
        game.mkdir()
        (game / ".hookshot_builder_home").write_text("../HookshotBuilder", encoding="utf-8")
        (game / "hookshot_version.txt").write_text("version=v-authored\nbuilder_local=1\n")
        run(game / "hookshot.exe", home, 0)
        moved = root / "Relocated ü, tree"
        tree.rename(moved)
        home, game = moved / "HookshotBuilder", moved / "Game"
        run(game / "hookshot.exe", home, 0)
        (game / ".hookshot_builder_home").unlink()
        run(game / "hookshot.exe", home, 2)
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
        print(f"portable setup: {count} startup scenarios passed; write-protected file boundary, real roots/environment/markers, intercepted process launch")


if __name__ == "__main__":
    main()
