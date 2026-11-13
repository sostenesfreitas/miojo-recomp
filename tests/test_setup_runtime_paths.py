"""Runtime recovery executes setup's actual reuse, publication and fast-launch boundaries.

Only process start and publication writes are intercepted. File readiness uses real Win32 APIs.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"linux": "Windows setup entry paths"}


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def fixture(source):
    condition = re.search(r'if \((repair \|\| GetFileAttributesW\(marker\).*?)\) \{', source).group(1)
    start = source.index("        wchar_t staged_exe[MAX_PATH];")
    end = source.index("        write_parts(install, parts, nparts);", start) + len("        write_parts(install, parts, nparts);")
    stage = source[start:end]
    code = r'''
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
static struct { wchar_t root[MAX_PATH]; int portable; } g_home;
static int g_home_ok=1;
static wchar_t last_error[2048];
static int moves, markers, receipts, starts;
static jmp_buf failed;
struct Part { int unused; };
static BOOL fixture_start(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                         LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION pi) {
    starts++; pi->hThread=NULL; pi->hProcess=NULL; return TRUE;
}
static BOOL fixture_move(LPCWSTR, LPCWSTR) { moves++; return TRUE; }
static int publish_install(const wchar_t *from, const wchar_t *to, int) { return fixture_move(from,to); }
static void fail(const wchar_t *format, ...) {
    va_list args; va_start(args,format); vswprintf(last_error,2048,format,args); va_end(args); longjmp(failed,90);
}
static void write_small(const wchar_t *, const char *) { markers++; }
static void write_parts(const wchar_t *, const Part *, int) { receipts++; }
#define CreateProcessW fixture_start
#define MoveFileW fixture_move
'''
    code += function(source, "static int builder_ready(") + '\n'
    code += function(source, "static int home_path(") + '\n'
    code += function(source, "static int list_name_ok(") + '\n'
    code += function(source, "static void read_small(") + '\n'
    code += function(source, "static int launch_installed(") + '\n'
    code += '\nstatic int admission(const wchar_t *exe, const wchar_t *marker, int repair) { return (' + condition + '); }\n'
    code += r'''
static int stage(const wchar_t *folder) {
    wchar_t install[MAX_PATH], final_dir[MAX_PATH], marker[MAX_PATH];
    const wchar_t *wrun=L"HookshotBuilder\\HookshotBuilder.exe";
    const char *tag="fixture"; Part parts[1]; int nparts=0;
    wcscpy(install,folder); wcscpy(final_dir,L"authored-final"); wcscpy(marker,L"authored-marker");
    int error=setjmp(failed);
    if(error) return error;
''' + stage + r'''
    return 0;
}
int main(void) {
    int argc; wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv || argc!=5) return 2;
    swprintf(g_home.root,MAX_PATH,L"%ls\\HookshotBuilder",argv[1]);
    int reuse=admission(argv[2],argv[3],0), repair=admission(argv[2],argv[3],1);
    int published=stage(argv[4]);
    if(published && (!wcsstr(last_error,L"antivirus") || !wcsstr(last_error,argv[4]))) return 5;
    int launched=launch_installed(L"--launch authored-game");
    printf("%d %d %d %d %d %d %d %d\n",reuse,repair,published,moves,markers,receipts,launched,starts);
    LocalFree(argv); return 0;
}
'''
    return code


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="clang++")
    ap.add_argument("--source", type=Path, default=ROOT / "builder/setup/setup.c")
    ap.add_argument("--mutations", action="store_true")
    args = ap.parse_args()
    source = args.source.read_text(encoding="utf-8")
    variants = [("production", source)]
    if args.mutations:
        replacements = [
            ("reuse", "|| !builder_ready(exe))", "|| GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES)"),
            ("publication", "if (!builder_ready(staged_exe))", "if (GetFileAttributesW(staged_exe) == INVALID_FILE_ATTRIBUTES)"),
            ("fast-launch", "if (!builder_ready(exe)) return 0;", "if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return 0;")]
        for name, old, new in replacements:
            assert source.count(old) == 1, name
            variants.append((name, source.replace(old,new)))
    with tempfile.TemporaryDirectory(prefix="hookshot-runtime-paths-") as tmp:
        root = Path(tmp)
        folder = root / 'HookshotBuilder/versions/fixture/HookshotBuilder'
        folder.mkdir(parents=True)
        (folder.parent.parent / 'current.txt').write_text('fixture')
        marker = folder.parent / 'installed.txt'; marker.write_text('fixture')
        names = ['HookshotBuilder.exe'] + re.findall(r'L"([^"]+)"', function(source,'static int builder_ready('))
        for name in names:
            (folder / name).write_bytes(b'authored test file')
        for variant, text in variants:
            cpp = root / (variant+'.cpp'); exe = root / (variant+'.exe')
            cpp.write_text(fixture(text),encoding='utf-8')
            # Production is C: its Win32 {sizeof si} aggregate zero-initializes the remaining fields.
            subprocess.run([args.cxx,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',str(cpp),'-lshell32','-o',str(exe)],check=True)
            def run():
                p = subprocess.run([str(exe),str(root),str(folder/'HookshotBuilder.exe'),str(marker),str(folder.parent)],capture_output=True,text=True,check=True)
                return tuple(map(int,p.stdout.split()))
            assert run() == (0,1,0,1,1,1,1,1), (variant,'healthy',run())
            runtime = folder/'coreclr.dll'; runtime.unlink()
            got = run()
            runtime.write_bytes(b'restored authored runtime')
            expected = (1,1,90,0,0,0,0,0)
            if variant == 'production':
                assert got == expected, (variant,got)
            else:
                assert got != expected, (variant,'mutation escaped')
            print('PASS setup runtime paths: '+variant+' '+str(got))


if __name__ == '__main__':
    main()
