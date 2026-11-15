"""Compile and execute the actual Windows dump worker against authored OS/DbgHelp spies."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


HEAD = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define WINAPI
#define CALLBACK
#define FALSE 0
#define TRUE 1
#define INFINITE (~0u)
#define WAIT_OBJECT_0 0
#define MOVEFILE_WRITE_THROUGH 8
#define INVALID_HANDLE_VALUE ((uintptr_t)-1)
typedef int BOOL;
typedef unsigned long DWORD;
typedef uintptr_t HANDLE;
typedef unsigned MINIDUMP_TYPE;
typedef struct { long long QuadPart; } LARGE_INTEGER;
enum { MiniDumpNormal = 0, MiniDumpFilterMemory = 8, MiniDumpFilterModulePaths = 128,
       MiniDumpWithoutOptionalData = 1024, MiniDumpWithoutAuxiliaryState = 16384 };
enum { ModuleCallback = 0, MemoryCallback = 5, ModuleWriteModule = 1,
       ModuleWriteDataSeg = 2, ModuleWriteMiscRecord = 4, ModuleWriteCvRecord = 8, ModuleWriteCodeSegs = 64 };
typedef struct { unsigned CallbackType; } MINIDUMP_CALLBACK_INPUT;
typedef struct { unsigned ModuleWriteFlags; } MINIDUMP_CALLBACK_OUTPUT;
typedef struct { DWORD ThreadId; void *ExceptionPointers; BOOL ClientPointers; } MINIDUMP_EXCEPTION_INFORMATION;
typedef struct { BOOL (*CallbackRoutine)(void *, MINIDUMP_CALLBACK_INPUT *, MINIDUMP_CALLBACK_OUTPUT *);
                 void *CallbackParam; } MINIDUMP_CALLBACK_INFORMATION;
enum { PATH_CAP = 4096 };
static HANDLE self_process = 1, dump_file = 2, dump_go = 3, dump_done = 4;
static DWORD dump_tid = 5;
static int dump_pointers;
static struct { uint64_t pid; } session = {6};
static wchar_t dump_temp[PATH_CAP] = L"local-only/6.dmp.tmp", dump_dir[PATH_CAP] = L"local-only";
static char published_stem[] = "crash-19700101T000000Z";
static int calls, flushed, closed, moved, deleted, signalled, fail, guard, blocked;
static char status[192];
static int hos_report_dbghelp_try(void) { if (blocked) return 0; assert(!guard); guard = 1; return 1; }
static void hos_report_dbghelp_done(void) { assert(guard); guard = 0; }
static size_t length(const char *s, size_t max) { size_t n = strlen(s); return n < max ? n : max; }
static DWORD GetLastError(void) { return 123; }
static BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) { assert(h == dump_file); size->QuadPart = 64; return TRUE; }
static void dump_outcome(const char *line) { assert(strlen(line) < sizeof status); strcpy(status, line); }
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) { assert(h == dump_go && ms == INFINITE); return WAIT_OBJECT_0; }
static BOOL MiniDumpWriteDump(HANDLE process, DWORD pid, HANDLE file, MINIDUMP_TYPE type,
                              MINIDUMP_EXCEPTION_INFORMATION *e, void *user, MINIDUMP_CALLBACK_INFORMATION *cb) {
    assert(process == self_process && pid == session.pid && file == dump_file);
    assert(guard);
    assert(type == (MiniDumpNormal | MiniDumpFilterMemory | MiniDumpFilterModulePaths |
                    MiniDumpWithoutOptionalData | MiniDumpWithoutAuxiliaryState));
    assert(e->ThreadId == dump_tid && e->ExceptionPointers == &dump_pointers && !e->ClientPointers && !user);
    MINIDUMP_CALLBACK_INPUT in = {ModuleCallback};
    MINIDUMP_CALLBACK_OUTPUT out = {ModuleWriteModule | ModuleWriteDataSeg | ModuleWriteCodeSegs | ModuleWriteCvRecord | ModuleWriteMiscRecord};
    assert(cb->CallbackRoutine(cb->CallbackParam, &in, &out));
    assert(out.ModuleWriteFlags == ModuleWriteModule);
    in.CallbackType = MemoryCallback;
    assert(!cb->CallbackRoutine(cb->CallbackParam, &in, &out));
    ++calls; return !fail;
}
static BOOL FlushFileBuffers(HANDLE file) { assert(file == dump_file && calls == 1); ++flushed; return TRUE; }
static BOOL CloseHandle(HANDLE file) { assert(file == dump_file && (flushed == 1 || blocked)); ++closed; return TRUE; }
static BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags) {
    assert(!wcscmp(from, dump_temp) && !wcscmp(to, L"local-only/crash-19700101T000000Z.dmp"));
    assert(flags == MOVEFILE_WRITE_THROUGH && closed == 1); ++moved; return TRUE;
}
static BOOL DeleteFileW(const wchar_t *file) { assert(!wcscmp(file, dump_temp)); ++deleted; return TRUE; }
static BOOL SetEvent(HANDLE event) { assert(event == dump_done && closed == 1); ++signalled; return TRUE; }
'''
MAIN = r'''
int main(void) {
    assert(dump_main(NULL) == 0);
    assert(calls == 1 && moved == 1 && !deleted && signalled == 1);
    assert(!guard && strstr(status, "written locally; bytes=64; error=0; not for sharing"));
    calls = flushed = closed = moved = deleted = signalled = 0;
    dump_file = 2; fail = 1;
    assert(dump_main(NULL) == 0);
    assert(calls == 1 && !moved && deleted == 1 && signalled == 1);
    assert(!guard && strstr(status, "failed; bytes=64; error=123; not for sharing"));
    calls = flushed = closed = moved = deleted = signalled = 0;
    dump_file = 2; blocked = 1;
    assert(dump_main(NULL) == 0);
    assert(!calls && !flushed && !moved && deleted == 1 && signalled == 1 && !guard);
    assert(!strcmp(status, "skipped, another collection was running\n"));
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("gcc") or "clang")
    args = parser.parse_args()
    source = (ROOT / "host/hos_crash_record.c").read_text(encoding="utf-8")
    worker = block(source, "static DWORD WINAPI dump_main(")
    fixture = HEAD + block(source, "static BOOL CALLBACK dump_filter(") + block(source, "static int dump_path(") + worker + MAIN
    with tempfile.TemporaryDirectory(prefix="hookshot-dump-policy-") as tmp:
        src, exe = Path(tmp) / "dump.c", Path(tmp) / "dump.exe"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)],
                       check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: actual dump worker requests only filtered stacks/modules, removes code/data/path-bearing records, keeps dumps local")
    print("PASS: failed dump removed; busy DbgHelp skips without waiting; outcomes stay local (not native Windows execution)")


if __name__ == "__main__":
    main()
