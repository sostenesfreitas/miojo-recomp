/* SPDX-License-Identifier: GPL-3.0-or-later
   Authored process/guest stand-ins; the writer and adapter functions are real. */
#define _GNU_SOURCE
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "hos_report_prefixes.h"
#include <windows.h>
#ifndef _WIN32
#include <signal.h>
#include <pthread.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

static _Thread_local int poison_alloc;
static int fail_storage, io_mode, fail_prune;
#ifndef _WIN32
static int interruptions;
#endif
void *fixture_malloc(size_t n) { assert(!poison_alloc); return malloc(n); }
void *fixture_calloc(size_t n, size_t size) { assert(!poison_alloc); return calloc(n, size); }
void *fixture_realloc(void *p, size_t n) { assert(!poison_alloc); return realloc(p, n); }
void fixture_free(void *p) { assert(!poison_alloc); free(p); }
#ifdef _WIN32
static int force_reparse;
static DWORD WINAPI fixture_GetFileAttributesW(LPCWSTR path) {
    if (force_reparse && wcsstr(path, L"crash-20990101T000000Z.txt")) return FILE_ATTRIBUTE_REPARSE_POINT;
    return GetFileAttributesW(path);
}
static BOOL WINAPI fixture_DeleteFileW(LPCWSTR path) {
    if (fail_prune && wcsstr(path, L"/pending/")) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return DeleteFileW(path);
}
static HANDLE WINAPI fixture_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                         DWORD disposition, DWORD flags, HANDLE template_file) {
    if (fail_storage) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return CreateFileW(path, access, share, security, disposition, flags, template_file);
}
static BOOL WINAPI fixture_WriteFile(HANDLE f, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED where) {
    if (poison_alloc && io_mode == 2) { SetLastError(ERROR_DISK_FULL); return FALSE; }
    if (poison_alloc && io_mode == 1 && size > 7) size = 7;
    return WriteFile(f, data, size, written, where);
}
#define CreateFileW fixture_CreateFileW
#define WriteFile fixture_WriteFile
#define GetFileAttributesW fixture_GetFileAttributesW
#define DeleteFileW fixture_DeleteFileW
#else
static int fixture_open(const char *path, int flags, ...) {
    if (fail_storage) { errno = EACCES; return -1; }
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    return open(path, flags, mode);
}
static ssize_t fixture_write(int fd, const void *p, size_t n) {
    if (poison_alloc && io_mode == 2) { errno = ENOSPC; return -1; }
    if (poison_alloc && io_mode == 1) {
        if (!interruptions++) { errno = EINTR; return -1; }
        if (n > 7) n = 7;
    }
    return write(fd, p, n);
}
static int fixture_unlink(const char *path) {
    if (fail_prune && strstr(path, "/pending/")) { errno = EACCES; return -1; }
    return unlink(path);
}
#define open fixture_open
#define write fixture_write
#define unlink fixture_unlink
#endif
#define malloc fixture_malloc
#define calloc fixture_calloc
#define realloc fixture_realloc
#define free fixture_free
#include "hos_crash_record.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#ifdef _WIN32
#undef CreateFileW
#undef WriteFile
#undef GetFileAttributesW
#undef DeleteFileW
#else
#undef open
#undef write
#undef unlink
#endif
#ifndef _WIN32
static int simulate_signal, reraises;
typedef void (*SignalHandler)(int);
static SignalHandler fixture_signal(int sig, SignalHandler action) {
    return simulate_signal ? SIG_DFL : signal(sig, action);
}
static int fixture_raise(int sig) {
    if (simulate_signal) { ++reraises; return 0; }
    return raise(sig);
}
#define signal fixture_signal
#define raise fixture_raise
/* POSIX_ADAPTER */
#undef signal
#undef raise
#endif

typedef struct Ctx { uint64_t x[32]; } Ctx;
typedef struct HosThread { char name[48]; } HosThread;
static HosThread guest_thread = {"fixture-thread"};
static HosThread *cur = &guest_thread;
static Ctx guest_context;
static unsigned char guest_memory[1024];
#define A64_MEM guest_memory
#define HOS_GUEST_SPACE sizeof guest_memory
static Ctx *hos_ctx(void) { return &guest_context; }
static HosThread *hos_self(void) { return cur; }
static const char *hos_thread_name(HosThread *t) { return t->name; }
/* RECORD_EXCEPTION */
/* FATAL_ADAPTER */
static LONG WINAPI report_crash(EXCEPTION_POINTERS *info) {
    poison_alloc = 1;
    record_exception(info);
    poison_alloc = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}
/* REPORT_FATAL */
static int log_queue(const char *line) { (void)line; return 0; }
volatile int hos_control_on;
void hos_control_log(const char *line) { (void)line; }
void hos_log_flush(void) {}
static void log_flush(void) {}
void hos_backtrace(void) {}
void hos_hostcall_stats(void) {}
/* HOS_LOG */
/* HOS_FATAL */
void rx_close_asked(void) {}
/* CONTROL_QUIT */
static int relaunch_asked = 1;
static void relaunch(void) { assert(marker == NO_FILE && !ready); }
/* WINDOW_EXIT */
static int game_argc;
static char **game_argv;
static int game_main(int argc, char **argv) { (void)argc; (void)argv; return 0; }
/* GAME_THREAD */

static void authored_context(HosCrashContext *c) {
    memset(c, 0, sizeof *c);
    c->reason = "unhandled exception"; c->thread = guest_thread.name;
    c->code = 0xc0000005u;
    c->pc = (uintptr_t)authored_context;
    c->sp = (uintptr_t)c; c->fp = 0;
    c->guest_base = (uintptr_t)guest_memory; c->guest_size = sizeof guest_memory;
    c->fault_address = c->guest_base + 64;
    c->guest_registers = guest_context.x;
}
int main(int argc, char **argv) {
    assert(argc >= 3);
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    /* Same no-core rule as test_linux_driver_fault_guard.py. */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
    const char *mode = argv[2];
    if (!strcmp(mode, "denied")) fail_storage = 1;
    if (!strcmp(mode, "prune-denied")) fail_prune = 1;
#ifdef _WIN32
    if (!strcmp(mode, "prune-reparse")) force_reparse = 1;
#endif
    hos_crash_record_start(argv[1], "fixture-v1-g1234", argc > 3 ? argv[3] : NULL);
    fprintf(stderr, "[hos host] hookshot fixture-v1-g1234\n[hos host] run_id: %s\n", hos_report_run_id());
    hos_crash_record_log_notes();
    hos_crash_record_log_notes(); /* any deletion warning is emitted once only */
    if (fail_storage || !strcmp(mode, "read-only")) { assert(!ready); hos_crash_clean_end(); return 0; }
    assert(ready);
    for (unsigned i = 0; i < 32; ++i) guest_context.x[i] = 0x1000 + i;
    guest_context.x[29] = 64; guest_context.x[30] = 0x80001234;
    const uint64_t frames[] = {80, 0x80005678, 0, 0x80009abc};
    memcpy(guest_memory + 64, frames, sizeof frames);
    char stamp[17]; timestamp(0, stamp); assert(!strcmp(stamp, "19700101T000000Z"));
    if (!strcmp(mode, "prune-denied") || !strcmp(mode, "prune-reparse")) { hos_crash_clean_end(); return 0; }
#ifndef _WIN32
    if (!strcmp(mode, "module-offsets")) {
        const uintptr_t pcs[] = {(uintptr_t)authored_context, (uintptr_t)malloc};
        for (unsigned i = 0; i < sizeof pcs / sizeof pcs[0]; ++i) {
            Dl_info info; char name[160]; uint64_t offset = 0;
            assert(dladdr((const void *)pcs[i], &info));
            assert(module_at(pcs[i], name, sizeof name, &offset));
            assert(offset == pcs[i] - (uintptr_t)info.dli_fbase);
        }
        /* Executable virtual offsets differ from file offsets by one page.
           Same basename/different path and a second instance must not alias. */
        static const char maps[] =
            "00100000-00101000 r--p 00000000 00:01 1 /authored/private-user/libgap.so\n"
            "00102000-00104000 r-xp 00001000 00:01 1 /authored/private-user/libgap.so\n"
            "00200000-00201000 r--p 00000000 00:01 2 /other/libgap.so\n"
            "00202000-00204000 r-xp 00001000 00:01 2 /other/libgap.so\n"
            "00302000-00304000 r-xp 00001000 00:01 3 /missing/libgap.so\n"
            "00400000-00401000 r--p 00000000 00:01 1 /authored/private-user/libgap.so\n"
            "00403000-00405000 r-xp 00002000 00:01 1 /authored/private-user/libgap.so\n";
        char path[PATH_CAP]; assert(join(path, sizeof path, argv[1], "authored.maps"));
        File file = open_file(path, 1, 0); assert(file != NO_FILE);
        assert(write_file(file, maps, sizeof maps - 1));
        const int original = maps_fd; maps_fd = file;
        char name[160]; uint64_t offset = 0;
        poison_alloc = 1;
        assert(module_at(0x102020, name, sizeof name, &offset) && offset == 0x2020 && !strcmp(name, "libgap.so"));
        assert(module_at(0x202020, name, sizeof name, &offset) && offset == 0x2020);
        assert(module_at(0x403020, name, sizeof name, &offset) && offset == 0x3020);
        assert(!module_at(0x302020, name, sizeof name, &offset)); /* no offset-zero mapping: unknown, not a guessed base */
        assert(!module_at(0x100020, name, sizeof name, &offset)); /* not executable */
        poison_alloc = 0;
        maps_fd = original; close_file(file);
        hos_crash_clean_end(); return 0;
    }
    if (!strcmp(mode, "signal-metadata")) {
        ucontext_t uc; assert(getcontext(&uc) == 0);
        siginfo_t si; memset(&si, 0, sizeof si); si.si_addr = (void *)0x40;
        simulate_signal = 1;
        si.si_code = SEGV_MAPERR; on_signal(SIGSEGV, &si, &uc); assert(reraises == 0);
        si.si_code = SI_USER; on_signal(SIGSEGV, &si, &uc); assert(reraises == 1);
        si.si_code = SI_TKILL; on_signal(SIGSEGV, &si, &uc); assert(reraises == 2);
        hos_crash_clean_end(); return 0;
    }
#endif
    if (!strcmp(mode, "collision")) {
        HosCrashContext c; authored_context(&c);
        format_record(&record, &session, &c, 0, NULL);
        assert(write_file(staging, record.bytes, record.n) && durable(staging));
        close_file(staging); staging = NO_FILE;
        assert(publish(staging_path, "crash", 0, session.pid));
        hos_crash_clean_end(); return 0;
    }
    if (!strcmp(mode, "clean")) { hos_crash_clean_end(); hos_crash_clean_end(); return 0; }
    if (!strcmp(mode, "device-mention")) {
        hos_log("%s", "a quoted vk: device lost: line is not a diagnostic entry");
        hos_log("%s", "a quoted vk: FATAL: line is not a diagnostic entry");
        hos_crash_clean_end(); return 0;
    }
    if (!strcmp(mode, "return")) return 0;
    if (!strcmp(mode, "control")) { game_quit(); return 90; }
    if (!strcmp(mode, "restart")) { hos_window_closed_exit(); return 90; }
    if (!strcmp(mode, "game-return")) { game_thread(NULL); return 90; }
    if (!strcmp(mode, "fatal")) { hos_fatal(NULL, "private-user /home/private-user secret"); return 90; }
    if (!strcmp(mode, "device-lost-tagged") || !strcmp(mode, "device-early-tagged")) {
        if (!strcmp(mode, "device-early-tagged"))
            hos_log_at(HOS_LOG_WARN, "%s", "vk: device lost: reading the checkpoints");
        hos_log_at(HOS_LOG_ERROR, "%s", "vk: FATAL: the GPU device was lost (VK_ERROR_DEVICE_LOST)");
        hos_log_at(HOS_LOG_ERROR, "%s", "vk: FATAL: a second fatal line must not replace the first record");
        ExitProcess(3);
    }
    if (!strcmp(mode, "device-lost") || !strcmp(mode, "device-early")) {
        if (!strcmp(mode, "device-early")) hos_log("%s", "vk: device lost: reading the checkpoints");
        hos_log("%s", "vk: FATAL: the GPU device was lost (VK_ERROR_DEVICE_LOST)");
        hos_log("%s", "vk: FATAL: a second fatal line must not replace the first record");
        ExitProcess(3);
    }
    if (!strcmp(mode, "hold") || !strcmp(mode, "stale-live")) {
        if (!strcmp(mode, "stale-live")) { close_file(marker); marker = NO_FILE; ready = 0; }
        puts("ready"); fflush(stdout); (void)getchar();
        if (ready) hos_crash_clean_end();
        return 0;
    }
    if (!strcmp(mode, "staged") || !strcmp(mode, "published-marker")) {
        HosCrashContext c; authored_context(&c);
        format_record(&record, &session, &c, epoch_now(), NULL);
        assert(write_file(staging, record.bytes, record.n) && durable(staging));
        session.record_complete = 1;
        assert(reset_file(marker) && write_file(marker, &session, sizeof session) && durable(marker));
        close_file(staging); staging = NO_FILE;
        if (!strcmp(mode, "published-marker")) assert(publish(staging_path, "crash", epoch_now(), session.pid));
        /* Deliberately imitate termination between the durable record and cleanup. */
        ExitProcess(3);
    }
    if (!strcmp(mode, "crash") || !strcmp(mode, "signal") || !strcmp(mode, "abort") || !strcmp(mode, "heap")) {
        SetUnhandledExceptionFilter(report_crash);
        AddVectoredExceptionHandler(0, report_fatal);
#ifdef _WIN32
        if (!strcmp(mode, "heap")) RaiseException(0xc0000374u, EXCEPTION_NONCONTINUABLE, 0, NULL);
        else if (!strcmp(mode, "abort")) RaiseException(0xc0000409u, EXCEPTION_NONCONTINUABLE, 0, NULL);
        else { static volatile uintptr_t address = 0x40; *(volatile int *)address = 1; }
#else
        if (!strcmp(mode, "signal")) raise(SIGSEGV);
        else if (!strcmp(mode, "abort")) raise(SIGABRT);
        else { static volatile uintptr_t address = 0x40; *(volatile int *)address = 1; }
#endif
        return 91;
    }
    HosCrashContext c; authored_context(&c);
    if (!strcmp(mode, "bounds")) {
        c.thread = (const char *)1; c.guest_registers = (const uint64_t *)1;
        c.pc = 1; c.fp = UINTPTR_MAX; c.fault_address = UINTPTR_MAX;
        struct { char bytes[8]; char guard; } small = {{0}, 'X'};
        Text out = {small.bytes, sizeof small.bytes, 0, 0};
        format_record(&out, &session, &c, epoch_now(), NULL);
        assert(out.full && small.guard == 'X');
    }
    if (!strcmp(mode, "partial")) io_mode = 1;
    if (!strcmp(mode, "disk-full")) io_mode = 2;
    if (!strcmp(mode, "privacy")) strcpy(guest_thread.name, "private-user@private-host");
    if (!strcmp(mode, "namespace")) strcpy(guest_thread.name, "sead::MainThread");
    poison_alloc = 1;
    hos_crash_record_write(&c);
    hos_crash_record_write(&c); /* reentry/second fault must leave the first record alone */
    poison_alloc = 0;
    return 0;
}
