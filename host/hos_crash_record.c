/* SPDX-License-Identifier: GPL-3.0-or-later
   Independent of the renderer and of the logger. A crash first writes this record,
   then the existing best-effort CRASH log runs. No stdio, allocation or loader
   queries occur in the text writer. Startup/recovery may use ordinary libc. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "hos_crash_record.h"
#include "hos_report_privacy.h"
#include "hos_report_run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include "../gpu/vk/compat/hookshot_env.h"
typedef HANDLE File;
#define NO_FILE INVALID_HANDLE_VALUE
#else
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
typedef int File;
#define NO_FILE (-1)
#endif

enum { PATH_CAP = 4096, TEXT_CAP = 24576, FRAME_CAP = 32 };
typedef struct Session {
    char magic[24];
    uint64_t pid, epoch, monotonic_ms;
    uint32_t record_complete; /* committed before publication; survives a collector moving the report */
    char run_id[33];
    char version[129], log_path[PATH_CAP]; /* private marker, never copied to a report */
} Session;
typedef struct Text { char *bytes; size_t cap, n; int full; } Text;
typedef struct LogFacts {
    uint64_t bytes, epoch;
    size_t tail;
    int known, fatal, crash, lost;
} LogFacts;
static Session session;
static HosReportPrivacy privacy;
static File marker = NO_FILE, staging = NO_FILE;
static char pending_dir[PATH_CAP], running_dir[PATH_CAP], marker_path[PATH_CAP], staging_path[PATH_CAP];
static char published_stem[96];
static int ready, claimed, prune_failed;
static char record_bytes[TEXT_CAP];
static Text record = {record_bytes, sizeof record_bytes, 0, 0};
#ifdef _WIN32
static HANDLE self_process;
static uintptr_t executable_base;
static HANDLE dump_go, dump_done, dump_file = INVALID_HANDLE_VALUE, dump_status = INVALID_HANDLE_VALUE;
static int dump_requested, dump_status_written;
static DWORD dump_tid;
static EXCEPTION_RECORD dump_exception;
static CONTEXT dump_context;
static EXCEPTION_POINTERS dump_pointers = {&dump_exception, &dump_context};
static wchar_t dump_temp[PATH_CAP], dump_dir[PATH_CAP], dump_status_temp[PATH_CAP];
#else
static int maps_fd = -1;
#endif

static uint64_t epoch_now(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000000;
#else
    struct timespec t;
    return clock_gettime(CLOCK_REALTIME, &t) == 0 ? (uint64_t)t.tv_sec : 0;
#endif
}
static uint64_t monotonic_now(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ? (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000 : 0;
#endif
}
static size_t length(const char *s, size_t max) { size_t n = 0; if (s) while (n < max && s[n]) ++n; return n; }
static void put(Text *b, const char *s) {
    for (size_t i = 0; s[i]; ++i) {
        if (b->n + 1 >= b->cap) { b->full = 1; return; }
        b->bytes[b->n++] = s[i];
    }
    b->bytes[b->n] = 0;
}
static void number(Text *b, uint64_t n, unsigned base) {
    char digits[65]; size_t k = sizeof digits; digits[--k] = 0;
    do { digits[--k] = "0123456789abcdef"[n % base]; n /= base; } while (n);
    put(b, digits + k);
}
static void decimal_field(Text *b, const char *key, uint64_t n) { put(b, key); put(b, ": "); number(b, n, 10); put(b, "\n"); }
static void signed_field(Text *b, const char *key, int n) {
    put(b, key); put(b, ": "); if (n < 0) put(b, "-");
    number(b, n < 0 ? (uint64_t)(-(int64_t)n) : (uint64_t)n, 10); put(b, "\n");
}
static void field(Text *b, const char *key, const char *value) { put(b, key); put(b, ": "); put(b, value); put(b, "\n"); }
size_t hos_crash_redact(char *out, size_t cap, const char *text, size_t size) {
    return hos_report_privacy_redact(&privacy, out, cap, text, size);
}
static void label_field(Text *b, const char *key, const char *value, size_t max) {
    char clean[160];
    hos_crash_redact(clean, sizeof clean, value, max);
    field(b, key, clean[0] ? clean : "unknown");
}
static const char *basename_of(const char *path) {
    const char *base = path;
    for (size_t i = 0; path[i]; ++i) if (path[i] == '/' || path[i] == '\\') base = path + i + 1;
    return base;
}
/* UTC without gmtime, locale or the CRT's locks. */
static void timestamp(uint64_t epoch, char out[17]) {
    static const unsigned mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t days = epoch / 86400;
    unsigned year = 1970, month = 0, day, hour = (unsigned)(epoch / 3600 % 24);
    const unsigned minute = (unsigned)(epoch / 60 % 60), second = (unsigned)(epoch % 60);
    while (year < 9999) {
        unsigned leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        if (days < 365 + leap) break;
        days -= 365 + leap; ++year;
    }
    while (month < 11) {
        unsigned count = mdays[month] + (month == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (days < count) break;
        days -= count; ++month;
    }
    day = days < 31 ? (unsigned)days + 1 : 31; ++month;
    const unsigned values[] = {year / 100, year % 100, month, day, hour, minute, second};
    const unsigned positions[] = {0,2,4,6,9,11,13};
    for (unsigned i = 0; i < 7; ++i) {
        out[positions[i]] = (char)('0' + values[i] / 10);
        out[positions[i] + 1] = (char)('0' + values[i] % 10);
    }
    out[8] = 'T'; out[15] = 'Z'; out[16] = 0;
}
#ifdef _WIN32
static int wide(const char *path, wchar_t out[PATH_CAP]) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, PATH_CAP) > 0;
}
static File open_file(const char *path, int create, int exclusive) {
    wchar_t w[PATH_CAP]; if (!wide(path, w)) return NO_FILE;
    HANDLE f = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != NO_FILE && exclusive) {
        OVERLAPPED where = {0};
        if (!LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &where)) {
            CloseHandle(f); return NO_FILE;
        }
    }
    return f;
}
static void close_file(File f) { if (f != NO_FILE) CloseHandle(f); }
static int remove_file(const char *path) { wchar_t w[PATH_CAP]; return wide(path, w) && DeleteFileW(w); }
static int make_dir(const char *path) {
    wchar_t w[PATH_CAP];
    if (!wide(path, w) || (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) return 0;
    const DWORD attributes = GetFileAttributesW(w);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
static int read_file(File f, void *dst, size_t size, uint64_t at) {
    LARGE_INTEGER off; off.QuadPart = (LONGLONG)at; DWORD got = 0;
    return SetFilePointerEx(f, off, NULL, FILE_BEGIN) && ReadFile(f, dst, (DWORD)size, &got, NULL) && got == size;
}
static int reset_file(File f) {
    LARGE_INTEGER zero; zero.QuadPart = 0;
    return SetFilePointerEx(f, zero, NULL, FILE_BEGIN) && SetEndOfFile(f);
}
static int durable(File f) { return FlushFileBuffers(f) != 0; }
#else
static File open_file(const char *path, int create, int exclusive) {
    int f = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (f >= 0 && exclusive && flock(f, LOCK_EX | LOCK_NB) != 0) { close(f); return NO_FILE; }
    return f;
}
static void close_file(File f) { if (f != NO_FILE) close(f); }
static int remove_file(const char *path) { return unlink(path) == 0; }
static int make_dir(const char *path) {
    struct stat st;
    return (mkdir(path, 0700) == 0 || errno == EEXIST) && lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static int read_file(File f, void *dst, size_t size, uint64_t at) { return pread(f, dst, size, (off_t)at) == (ssize_t)size; }
static int reset_file(File f) { return ftruncate(f, 0) == 0 && lseek(f, 0, SEEK_SET) == 0; }
static int durable(File f) { return fsync(f) == 0; }
#endif
static int write_file(File f, const void *data, size_t size) {
    const char *p = (const char *)data;
    while (size) {
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile(f, p, (DWORD)size, &n, NULL) || !n) return 0;
#else
        ssize_t n = write(f, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
#endif
        p += n; size -= (size_t)n;
    }
    return 1;
}
static int join(char *out, size_t cap, const char *dir, const char *file) {
    const size_t a = length(dir, cap), b = length(file, cap);
    if (a + b + 2 > cap) return 0;
    memcpy(out, dir, a); out[a] = '/'; memcpy(out + a + 1, file, b + 1); return 1;
}
/* Publish only a complete, flushed record; never replace an older record, even
   when two games fail during the same second. Plain OS calls on the crash path. */
static int publish(const char *source, const char *kind, uint64_t epoch, uint64_t pid) {
    char when[17]; static char dest[PATH_CAP]; timestamp(epoch, when);
    for (unsigned collision = 0; collision < 100; ++collision) {
        char name_bytes[128]; Text name = {name_bytes, sizeof name_bytes, 0, 0};
        put(&name, kind); put(&name, "-"); put(&name, when);
        if (collision) { put(&name, "-"); number(&name, pid, 10); put(&name, "-"); number(&name, collision, 10); }
        if (name.n >= sizeof published_stem) return 0;
        for (size_t i = 0; i <= name.n; ++i) published_stem[i] = name.bytes[i];
        put(&name, ".txt");
        if (!join(dest, sizeof dest, pending_dir, name.bytes)) return 0;
#ifdef _WIN32
        /* Paths use UTF-8 only at normal startup; pending names are ASCII. The
           conversion is a bounded Windows conversion, never CRT/locale based. */
        static wchar_t from[PATH_CAP], to[PATH_CAP];
        if (!wide(source, from) || !wide(dest, to)) return 0;
        if (MoveFileExW(from, to, MOVEFILE_WRITE_THROUGH)) return 1;
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) return 0;
#else
        if (link(source, dest) == 0) { unlink(source); return 1; }
        if (errno != EEXIST) return 0;
#endif
    }
    return 0;
}
static int safe_read(uintptr_t address, void *out, size_t size) {
    if (!address || address > UINTPTR_MAX - size) return 0;
#ifdef _WIN32
    SIZE_T n = 0;
    return ReadProcessMemory(self_process, (const void *)address, out, size, &n) && n == size;
#else
    struct iovec local = {out, size}, remote = {(void *)address, size};
    return syscall(SYS_process_vm_readv, (long)session.pid, &local, 1UL, &remote, 1UL, 0UL) == (ssize_t)size;
#endif
}
#ifndef _WIN32
/* /proc/self/maps is opened at startup, read only through read/lseek here, and
   includes modules loaded since startup. No dladdr/loader lock in a signal. */
static int hex_digit(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static const char *hex_read(const char *p, uint64_t *n) {
    *n = 0; int d;
    while ((d = hex_digit((unsigned char)*p)) >= 0) { *n = (*n << 4) | (unsigned)d; ++p; }
    return p;
}
typedef struct CrashMap {
    uint64_t lo, hi, file_offset;
    const char *path;
    int executable;
} CrashMap;
static int map_line(const char *line, CrashMap *map) {
    const char *p = hex_read(line, &map->lo); if (*p != '-') return 0;
    p = hex_read(p + 1, &map->hi);
    if (map->hi <= map->lo || *p != ' ') return 0;
    const char *permissions = ++p;
    while (*p && *p != ' ') ++p;
    if (p - permissions < 3) return 0;
    map->executable = permissions[2] == 'x';
    while (*p == ' ') ++p;
    p = hex_read(p, &map->file_offset);
    for (int field_index = 0; field_index < 2; ++field_index) {
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
    }
    while (*p == ' ') ++p;
    map->path = p;
    return 1;
}
#endif
static int module_at(uintptr_t pc, char *name, size_t cap, uint64_t *offset) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void *)pc, &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE) return 0;
    const uintptr_t base = (uintptr_t)mbi.AllocationBase;
    *offset = pc - base;
    if (base == executable_base) { hos_crash_redact(name, cap, "hookshot.exe", 12); return 1; }
    /* Read the image's export name, not its loader entry/full path. */
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!safe_read(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 1048576 || !safe_read(base + (uintptr_t)dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;
    const DWORD rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    IMAGE_EXPORT_DIRECTORY exports;
    if (!rva || !safe_read(base + rva, &exports, sizeof exports) || !exports.Name) return 0;
    char raw[128]; size_t n = 0;
    for (; n + 1 < sizeof raw; ++n) if (!safe_read(base + exports.Name + n, raw + n, 1) || !raw[n]) break;
    raw[n] = 0;
    hos_crash_redact(name, cap, raw, n); return name[0] != 0;
#else
    if (maps_fd < 0) return 0;
    static char chunk[1024], line[PATH_CAP + 128], path[PATH_CAP + 128];
    uint64_t code_start = 0, base = 0;
    int selected = 0, have_base = 0;
    /* ELF's virtual segment offset need not equal its file offset (lld leaves
       gaps). Use the module's offset-zero mapping, as dladdr's load base does,
       not pc - executable_map_start + file_offset. No loader calls here. */
    for (int pass = 0; pass < 2; ++pass) {
        if (lseek(maps_fd, 0, SEEK_SET) < 0) return 0;
        size_t used = 0; int cut = 0, stop = 0;
        ssize_t got;
        while (!stop && (got = read(maps_fd, chunk, sizeof chunk)) > 0) {
            for (ssize_t i = 0; i < got; ++i) {
                if (chunk[i] == '\n') {
                    line[used] = 0;
                    CrashMap map;
                    if (!cut && map_line(line, &map)) {
                        if (!pass && pc >= map.lo && pc < map.hi && map.executable &&
                            (*map.path == '/' || *map.path == '[')) {
                            memcpy(path, map.path, strlen(map.path) + 1);
                            code_start = map.lo; selected = 1; stop = 1;
                            if (!map.file_offset) base = map.lo, have_base = 1;
                        } else if (pass) {
                            if (map.lo <= code_start && !map.file_offset && !strcmp(map.path, path))
                                base = map.lo, have_base = 1;
                            if (map.lo >= code_start) {
                                stop = 1;
                                if (map.lo != code_start || !map.executable || pc >= map.hi || strcmp(map.path, path))
                                    have_base = 0; /* module unmapped/changed during the read */
                            }
                        }
                    }
                    used = 0; cut = 0;
                    if (stop) break;
                } else if (used + 1 < sizeof line) line[used++] = chunk[i];
                else cut = 1;
            }
        }
        if (!selected || (pass && !stop)) return 0;
        if (have_base) break;
    }
    if (!have_base) return 0;
    const char *leaf = basename_of(path);
    hos_crash_redact(name, cap, leaf, length(leaf, 512));
    *offset = pc - base;
    return 1;
#endif
}
static void module_field(Text *b, const char *key, uintptr_t pc) {
    char name[160]; uint64_t offset;
    put(b, key); put(b, ": ");
    if (pc && module_at(pc, name, sizeof name, &offset)) { put(b, name); put(b, "+0x"); number(b, offset, 16); }
    else put(b, "unknown"); /* never substitute an ASLR/heap address */
    put(b, "\n");
}
static void host_stack(Text *b, const HosCrashContext *c) {
#ifdef _WIN32
    CONTEXT ctx;
    if (c->native_context && safe_read((uintptr_t)c->native_context, &ctx, sizeof ctx)) {
        field(b, "host_backtrace_method", "Windows unwind");
        for (unsigned i = 0; i < FRAME_CAP && ctx.Rip; ++i) {
            module_field(b, "host_backtrace", (uintptr_t)ctx.Rip);
            DWORD64 image = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
            if (f) {
                void *data; DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &data, &frame, NULL);
            } else {
                uint64_t next;
                if (!safe_read((uintptr_t)ctx.Rsp, &next, sizeof next)) break;
                ctx.Rsp += 8; ctx.Rip = next;
            }
        }
        return;
    }
#endif
    field(b, "host_backtrace_method", "frame pointer; may be partial");
    module_field(b, "host_backtrace", c->pc);
    uintptr_t fp = c->fp;
    for (unsigned i = 1; i < FRAME_CAP && fp >= c->sp && fp - c->sp <= (64u << 20) && !(fp & 7); ++i) {
        uintptr_t words[2];
        if (!safe_read(fp, words, sizeof words) || !words[1]) break;
        module_field(b, "host_backtrace", words[1]);
        if (words[0] <= fp) break;
        fp = words[0];
    }
}
static void guest_stack(Text *b, const HosCrashContext *c) {
    uint64_t regs[32];
    if (!c->guest_registers || !safe_read((uintptr_t)c->guest_registers, regs, sizeof regs)) return;
    for (unsigned i = 0; i < 32; ++i) {
        put(b, "guest_register: x"); number(b, i, 10); put(b, "=0x"); number(b, regs[i], 16); put(b, "\n");
    }
    put(b, "guest_backtrace: 0x"); number(b, regs[30], 16); put(b, "\n");
    uint64_t fp = regs[29];
    for (unsigned i = 0; i < 40 && fp && !(fp & 15); ++i) {
        uint64_t words[2];
        if (c->guest_size < sizeof words || fp > c->guest_size - sizeof words ||
            c->guest_base > UINTPTR_MAX - fp || !safe_read(c->guest_base + (uintptr_t)fp, words, sizeof words)) break;
        put(b, "guest_backtrace: 0x"); number(b, words[1], 16); put(b, "\n");
        if (words[0] <= fp) break;
        fp = words[0];
    }
}
static const char *fixed_reason(const char *reason) {
    static const char *const allowed[] = {"unhandled exception", "fatal signal", "hos_fatal", "device lost", "Vulkan fatal"};
    char text[32] = ""; size_t n = 0;
    for (; n + 1 < sizeof text; ++n)
        if (!reason || !safe_read((uintptr_t)reason + n, text + n, 1) || !text[n]) break;
    text[n] = 0;
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; ++i)
        if (!strcmp(text, allowed[i])) return allowed[i];
    return "unknown";
}
static int valid_run_id(const char id[33]) {
    if (!memcmp(id, "unknown", 8)) return 1;
    for (size_t i = 0; i < 32; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return id[32] == 0;
}

static void format_record(Text *b, const Session *s, const HosCrashContext *c, uint64_t now, const LogFacts *facts) {
    b->n = 0; b->full = 0;
    put(b, c ? "Hookshot crash record 1\n" : "Hookshot unclean record 1\n");
    label_field(b, "version", s->version, sizeof s->version);
    field(b, "run_id", s->run_id);
    char when[17]; timestamp(now, when); field(b, "when", when);
    if (c) {
        const uint64_t ms = monotonic_now();
        decimal_field(b, "uptime_s", ms >= s->monotonic_ms ? (ms - s->monotonic_ms) / 1000 : 0);
    } else field(b, "uptime_s", "unknown");
    /* Callers supply a fixed label. Never print a guest/driver's formatted error. */
    field(b, "reason", c ? fixed_reason(c->reason) : "ended without a clean exit");
    char thread[96] = "";
    if (c && c->thread) {
        size_t i = 0;
        for (; i + 1 < sizeof thread; ++i)
            if (!safe_read((uintptr_t)c->thread + i, thread + i, 1) || !thread[i]) break;
        thread[i] = 0;
    }
    label_field(b, "thread", thread[0] ? thread : "unknown", sizeof thread);
    module_field(b, "module_offset", c ? c->pc : 0);
    field(b, "last_present_age_ms", "unknown");
    label_field(b, "log", s->log_path[0] ? basename_of(s->log_path) : "unknown", 128);
    decimal_field(b, "pid", s->pid);
    timestamp(s->epoch, when); field(b, "started", when);
    if (c) {
        signed_field(b, "signal", c->signal_number); signed_field(b, "signal_code", c->signal_code);
        put(b, "exception: 0x"); number(b, c->code, 16); put(b, "\n");
        const char *address_class = !c->fault_address ? "null" :
            c->fault_address >= c->guest_base && c->fault_address - c->guest_base < c->guest_size ? "guest window" : "other";
        field(b, "fault_address_class", address_class);
        host_stack(b, c); guest_stack(b, c);
    } else {
        field(b, "crash_handler", "not observed");
        field(b, "termination_note", "ended from outside or lost power, not necessarily a crash");
        if (facts && facts->known) {
            decimal_field(b, "log_size_bytes", facts->bytes); decimal_field(b, "log_tail_bytes", facts->tail);
            timestamp(facts->epoch, when); field(b, "log_mtime_utc", when);
            field(b, "log_tail_fatal", facts->fatal ? "yes" : "no");
            field(b, "log_tail_crash", facts->crash ? "yes" : "no");
            field(b, "log_tail_device_lost", facts->lost ? "yes" : "no");
        }
    }
#ifdef _WIN32
    field(b, "dump", c && c->exception_filter && dump_go ? "requested locally, not for sharing" : "off");
#else
    field(b, "dump", "off");
#endif
    field(b, "record_complete", "yes");
}

/* Recovery uses ordinary calls. Only lengths/times and allow-listed booleans from
   the log tail leave the private marker; arbitrary log lines never enter records. */
static LogFacts log_facts(const Session *s) {
    LogFacts facts = {0};
#ifdef _WIN32
    wchar_t path[PATH_CAP];
    FILE *f = wide(s->log_path, path) ? _wfopen(path, L"rb") : NULL;
#else
    FILE *f = fopen(s->log_path, "rb");
#endif
    if (!f) return facts;
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(_fileno(f), &st) != 0 || st.st_size < 0) { fclose(f); return facts; }
    _fseeki64(f, st.st_size > 4096 ? st.st_size - 4096 : 0, SEEK_SET);
#else
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) { fclose(f); return facts; }
    fseeko(f, st.st_size > 4096 ? st.st_size - 4096 : 0, SEEK_SET);
#endif
    char tail[4097]; facts.tail = fread(tail, 1, sizeof tail - 1, f); tail[facts.tail] = 0;
    facts.known = 1; facts.bytes = (uint64_t)st.st_size; facts.epoch = (uint64_t)st.st_mtime;
    facts.fatal = strstr(tail, "FATAL:") != NULL; facts.crash = strstr(tail, "CRASH:") != NULL;
    facts.lost = strstr(tail, "device lost") != NULL || strstr(tail, "VK_ERROR_DEVICE_LOST") != NULL;
    fclose(f); return facts;
}
static void recover_one(const char *name) {
    size_t digits = 0;
    while (name[digits] >= '0' && name[digits] <= '9') ++digits;
    if (!digits || digits > 20 || strcmp(name + digits, ".running")) return;
    char path[PATH_CAP], temp[PATH_CAP], leaf[40];
    if (!join(path, sizeof path, running_dir, name)) return;
    File old = open_file(path, 0, 1); if (old == NO_FILE) return; /* live owner holds the lock */
    Session before;
    if (!read_file(old, &before, sizeof before, 0) || memcmp(before.magic, "Hookshot running 1", 19) ||
        !valid_run_id(before.run_id) || !memchr(before.version, 0, sizeof before.version) ||
        !memchr(before.log_path, 0, sizeof before.log_path)) {
        close_file(old); return; /* not ours: do not remove or invent a report */
    }
    memcpy(leaf, name, digits); memcpy(leaf + digits, ".record", 8);
    if (!join(temp, sizeof temp, running_dir, leaf)) { close_file(old); return; }
    File file = open_file(temp, 0, 0);
    if (file == NO_FILE && before.record_complete) {
        /* Publication removed the staging name. The report may already have
           moved into a menu report: do not invent an unclean end for it. */
        remove_file(path); close_file(old); return;
    }
    if (file == NO_FILE) file = open_file(temp, 1, 0);
    if (file != NO_FILE) {
        /* A writer that finished but died before publishing needs its actual
           crash record retained, not an invented second unclean record. */
        char saved[TEXT_CAP]; size_t got = 0;
#ifdef _WIN32
        DWORD n = 0;
        if (ReadFile(file, saved, sizeof saved - 1, &n, NULL)) got = n;
#else
        ssize_t n = read(file, saved, sizeof saved - 1); if (n > 0) got = (size_t)n;
#endif
        saved[got] = 0;
        const int complete = got > 30 && !strncmp(saved, "Hookshot crash record 1\n", 24) &&
                             strstr(saved, "\nrecord_complete: yes\n") != NULL;
        int ok = complete;
        if (!complete) {
            LogFacts facts = log_facts(&before);
            format_record(&record, &before, NULL, epoch_now(), &facts);
            ok = !record.full && reset_file(file) && write_file(file, record.bytes, record.n) && durable(file);
        }
        close_file(file);
        if (ok && publish(temp, complete ? "crash" : "unclean", epoch_now(), before.pid)) {
            remove_file(path); close_file(old); return;
        }
    }
    close_file(old);
}
/* Startup retention only. A failing thread never enumerates or prunes files. */
enum { PENDING_KEEP = 15, PENDING_NAME = 96 };
static const char *pending_time(const char *name) {
    const char *p = !strncmp(name, "crash-", 6) ? name + 6 : !strncmp(name, "unclean-", 8) ? name + 8 : NULL;
    if (!p || length(name, PENDING_NAME) >= PENDING_NAME || length(p, 17) < 16) return NULL;
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 8) { if (p[i] != 'T') return NULL; }
        else if (i == 15) { if (p[i] != 'Z') return NULL; }
        else if (p[i] < '0' || p[i] > '9') return NULL;
    }
    const char *suffix = p + 16;
    if (*suffix == '-') {
        ++suffix;
        if (*suffix < '0' || *suffix > '9') return NULL;
        while (*suffix >= '0' && *suffix <= '9') ++suffix;
        if (*suffix++ != '-' || *suffix < '0' || *suffix > '9') return NULL;
        while (*suffix >= '0' && *suffix <= '9') ++suffix;
    }
    return !strcmp(suffix, ".txt") ? p : NULL;
}
static int pending_newer(const char *a, const char *b) {
    const int time_order = memcmp(pending_time(a), pending_time(b), 16);
    return time_order ? time_order : strcmp(a, b); /* deterministic ties within one UTC second */
}
static void prune_entry(const char *name, char keep[PENDING_KEEP][PENDING_NAME], int *count, int deleting) {
    char path[PATH_CAP];
    if (!pending_time(name) || !join(path, sizeof path, pending_dir, name)) return;
#ifdef _WIN32
    wchar_t w[PATH_CAP]; if (!wide(path, w)) return;
    const DWORD attributes = GetFileAttributesW(w);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
#else
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return; /* do not follow matching symlinks */
#endif
    if (deleting) {
        for (int i = 0; i < *count; ++i) if (!strcmp(keep[i], name)) return;
        if (!remove_file(path)) {
#ifdef _WIN32
            if (GetLastError() != ERROR_FILE_NOT_FOUND) prune_failed = 1;
#else
            if (errno != ENOENT) prune_failed = 1;
#endif
        }
        return;
    }
    if (*count == PENDING_KEEP && pending_newer(name, keep[*count - 1]) <= 0) return;
    int at = *count < PENDING_KEEP ? (*count)++ : PENDING_KEEP - 1;
    while (at && pending_newer(name, keep[at - 1]) > 0) {
        memcpy(keep[at], keep[at - 1], PENDING_NAME); --at;
    }
    memcpy(keep[at], name, strlen(name) + 1);
}
static void prune_pending(void) {
    char keep[PENDING_KEEP][PENDING_NAME] = {{0}};
    int count = 0;
    /* First select, then remove: directory order never decides which records stay. */
    for (int deleting = 0; deleting < 2; ++deleting) {
#ifdef _WIN32
        char pattern[PATH_CAP]; wchar_t w[PATH_CAP]; WIN32_FIND_DATAW data;
        if (!join(pattern, sizeof pattern, pending_dir, "*") || !wide(pattern, w)) return;
        HANDLE find = FindFirstFileW(w, &data); if (find == INVALID_HANDLE_VALUE) return;
        do {
            char name[PENDING_NAME];
            if (WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, sizeof name, NULL, NULL))
                prune_entry(name, keep, &count, deleting);
        } while (FindNextFileW(find, &data));
        FindClose(find);
#else
        DIR *dir = opendir(pending_dir); if (!dir) return;
        struct dirent *entry;
        while ((entry = readdir(dir))) prune_entry(entry->d_name, keep, &count, deleting);
        closedir(dir);
#endif
    }
}
void hos_crash_record_log_notes(void) {
    if (prune_failed) {
        prune_failed = 0;
        void hos_log(const char *fmt, ...);
        hos_log("crash records: some old pending records could not be removed");
    }
}

static void recover(void) {
#ifdef _WIN32
    char pattern[PATH_CAP]; wchar_t w[PATH_CAP]; WIN32_FIND_DATAW data;
    if (!join(pattern, sizeof pattern, running_dir, "*.running") || !wide(pattern, w)) return;
    HANDLE find = FindFirstFileW(w, &data); if (find == INVALID_HANDLE_VALUE) return;
    do {
        char name[64];
        if (WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, sizeof name, NULL, NULL)) recover_one(name);
    } while (FindNextFileW(find, &data));
    FindClose(find);
#else
    DIR *dir = opendir(running_dir); if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) recover_one(entry->d_name);
    closedir(dir);
#endif
}
#ifdef _WIN32
static BOOL CALLBACK dump_filter(void *unused, MINIDUMP_CALLBACK_INPUT *in, MINIDUMP_CALLBACK_OUTPUT *out) {
    (void)unused;
    if (in->CallbackType == ModuleCallback)
        out->ModuleWriteFlags &= ~(ModuleWriteDataSeg | ModuleWriteCodeSegs | ModuleWriteCvRecord | ModuleWriteMiscRecord);
    if (in->CallbackType == MemoryCallback) return FALSE; /* no additional memory */
    return TRUE;
}
static int dump_path(wchar_t out[PATH_CAP], const char *ending) {
    size_t n = 0;
    while (dump_dir[n] && n < PATH_CAP - 1) { out[n] = dump_dir[n]; ++n; }
    if (n + length(published_stem, sizeof published_stem) + length(ending, 64) + 2 > PATH_CAP) return 0;
    out[n++] = L'/';
    for (size_t i = 0; published_stem[i]; ++i) out[n++] = (wchar_t)published_stem[i];
    for (size_t i = 0; ending[i]; ++i) out[n++] = (wchar_t)ending[i];
    out[n] = 0; return 1;
}
/* Also usable for the filter's fixed timeout line: no formatting/allocation,
   one preopened handle and a single winner, never a change to the public text. */
static void dump_outcome(const char *line) {
    if (__atomic_exchange_n(&dump_status_written, 1, __ATOMIC_ACQ_REL) ||
        dump_status == INVALID_HANDLE_VALUE) return;
    write_file(dump_status, line, length(line, 256)); FlushFileBuffers(dump_status);
    CloseHandle(dump_status); dump_status = INVALID_HANDLE_VALUE;
    static wchar_t final[PATH_CAP];
    if (dump_path(final, ".outcome.txt")) MoveFileExW(dump_status_temp, final, MOVEFILE_WRITE_THROUGH);
}

static DWORD WINAPI dump_main(void *unused) {
    (void)unused;
    if (WaitForSingleObject(dump_go, INFINITE) != WAIT_OBJECT_0) return 0;
    if (!hos_report_dbghelp_try()) {
        CloseHandle(dump_file); dump_file = INVALID_HANDLE_VALUE; DeleteFileW(dump_temp);
        dump_outcome("skipped, another collection was running\n");
        SetEvent(dump_done); return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception = {dump_tid, &dump_pointers, FALSE};
    MINIDUMP_CALLBACK_INFORMATION callback = {dump_filter, NULL};
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpFilterMemory | MiniDumpFilterModulePaths |
                                             MiniDumpWithoutOptionalData | MiniDumpWithoutAuxiliaryState);
    int ok = MiniDumpWriteDump(self_process, (DWORD)session.pid, dump_file, type, &exception, NULL, &callback);
    DWORD error = ok ? 0 : GetLastError();
    hos_report_dbghelp_done();
    LARGE_INTEGER bytes; bytes.QuadPart = 0; GetFileSizeEx(dump_file, &bytes);
    FlushFileBuffers(dump_file); CloseHandle(dump_file); dump_file = INVALID_HANDLE_VALUE;
    if (ok) {
        wchar_t final[PATH_CAP];
        if (!dump_path(final, ".dmp") || !MoveFileExW(dump_temp, final, MOVEFILE_WRITE_THROUGH))
            ok = 0, error = GetLastError();
    } else DeleteFileW(dump_temp);
    char outcome[192];
    snprintf(outcome, sizeof outcome, "%s; bytes=%llu; error=%lu; not for sharing\n",
             ok ? "written locally" : "failed", (unsigned long long)bytes.QuadPart, (unsigned long)error);
    dump_outcome(outcome);
    SetEvent(dump_done); return 0;
}
void hos_crash_record_dump(void *info) {
    if (!dump_go || !published_stem[0] || __atomic_exchange_n(&dump_requested, 1, __ATOMIC_ACQ_REL)) return;
    EXCEPTION_POINTERS *p = (EXCEPTION_POINTERS *)info;
    dump_exception = *p->ExceptionRecord; dump_exception.ExceptionRecord = NULL;
    dump_context = *p->ContextRecord; dump_tid = GetCurrentThreadId();
    SetEvent(dump_go);
    if (WaitForSingleObject(dump_done, 2000) != WAIT_OBJECT_0)
        dump_outcome("timed out waiting; worker may finish later; not for sharing\n");
}
static void prepare_dump(const char *root) {
    if (!env_on("HOS_CRASH_MINIDUMP")) return;
    char dir[PATH_CAP], path[PATH_CAP], leaf[64];
    snprintf(leaf, sizeof leaf, "%llu.dmp.tmp", (unsigned long long)session.pid);
    if (!join(dir, sizeof dir, root, "local-only") || !make_dir(dir) || !wide(dir, dump_dir) ||
        !join(path, sizeof path, dir, leaf) || !wide(path, dump_temp)) return;
    dump_file = CreateFileW(dump_temp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dump_file == INVALID_HANDLE_VALUE) return;
    snprintf(leaf, sizeof leaf, "%llu.outcome.tmp", (unsigned long long)session.pid);
    if (join(path, sizeof path, dir, leaf) && wide(path, dump_status_temp))
        dump_status = CreateFileW(dump_status_temp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    dump_go = CreateEventW(NULL, FALSE, FALSE, NULL); dump_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE worker = dump_go && dump_done && dump_status != INVALID_HANDLE_VALUE
                        ? CreateThread(NULL, 0, dump_main, NULL, 0, NULL) : NULL;
    if (worker) CloseHandle(worker);
    else {
        close_file(dump_file); dump_file = INVALID_HANDLE_VALUE; DeleteFileW(dump_temp);
        close_file(dump_status); dump_status = INVALID_HANDLE_VALUE; DeleteFileW(dump_status_temp);
        if (dump_go) CloseHandle(dump_go);
        if (dump_done) CloseHandle(dump_done);
        dump_go = dump_done = NULL;
    }
}
#endif
void hos_crash_clean_end(void) {
    int expected = 0;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) ||
        !__atomic_compare_exchange_n(&claimed, &expected, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&ready, 0, __ATOMIC_RELEASE);
    close_file(staging); staging = NO_FILE; remove_file(staging_path);
    remove_file(marker_path); close_file(marker); marker = NO_FILE;
#ifdef _WIN32
    if (dump_file != INVALID_HANDLE_VALUE) { CloseHandle(dump_file); dump_file = INVALID_HANDLE_VALUE; DeleteFileW(dump_temp); }
    if (dump_status != INVALID_HANDLE_VALUE) { CloseHandle(dump_status); dump_status = INVALID_HANDLE_VALUE; DeleteFileW(dump_status_temp); }
#endif
}
void hos_crash_record_start(const char *dir, const char *version, const char *log_path) {
    if (ready) return;
    char root[PATH_CAP], host[256] = "", leaf[64];
#ifdef _WIN32
    const char *home = getenv("USERPROFILE"), *user = getenv("USERNAME");
    DWORD host_size = sizeof host; GetComputerNameA(host, &host_size);
    self_process = GetCurrentProcess(); executable_base = (uintptr_t)GetModuleHandleW(NULL);
    session.pid = GetCurrentProcessId();
#else
    struct passwd *pw = getpwuid(getuid());
    const char *home = getenv("HOME"), *user = getenv("USER");
    if (!home || !*home) home = pw ? pw->pw_dir : NULL;
    if (!user || !*user) user = pw ? pw->pw_name : NULL;
    gethostname(host, sizeof host - 1);
    session.pid = (uint64_t)getpid();
#endif
    hos_report_privacy_init(&privacy, home, user, host);
    if (!dir || !join(root, sizeof root, dir, "bug-reports") || !make_dir(root) ||
        !join(pending_dir, sizeof pending_dir, root, "pending") || !make_dir(pending_dir) ||
        !join(running_dir, sizeof running_dir, root, "running") || !make_dir(running_dir)) return;
    recover();
    prune_pending();
    memcpy(session.magic, "Hookshot running 1", 19);
    session.epoch = epoch_now(); session.monotonic_ms = monotonic_now();
    hos_crash_redact(session.version, sizeof session.version, version, length(version, 128));
    const char *run_id = hos_report_run_id();
    memcpy(session.run_id, run_id, length(run_id, sizeof session.run_id - 1) + 1);
#ifdef _WIN32
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE); wchar_t log[PATH_CAP];
    if (GetFileType(err) == FILE_TYPE_DISK) {
        DWORD n = GetFinalPathNameByHandleW(err, log, PATH_CAP, FILE_NAME_NORMALIZED);
        if (n && n < PATH_CAP) WideCharToMultiByte(CP_UTF8, 0, log, -1, session.log_path, PATH_CAP, NULL, NULL);
    }
#else
    struct stat st;
    if (fstat(STDERR_FILENO, &st) == 0 && S_ISREG(st.st_mode)) {
        ssize_t n = readlink("/proc/self/fd/2", session.log_path, sizeof session.log_path - 1);
        if (n > 0) session.log_path[n] = 0;
    }
    maps_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
#endif
    /* Icon launches supply the exact destination selected before log rotation,
       including the logs/ folder or the host's fallback beside the game.
       External regular-file stderr keeps its actual path privately. */
    if (!session.log_path[0] && log_path) {
        const size_t n = length(log_path, sizeof session.log_path);
        if (n < sizeof session.log_path) memcpy(session.log_path, log_path, n + 1);
    }
    snprintf(leaf, sizeof leaf, "%llu.running", (unsigned long long)session.pid);
    if (!join(marker_path, sizeof marker_path, running_dir, leaf)) return;
    marker = open_file(marker_path, 1, 1); if (marker == NO_FILE) return;
    snprintf(leaf, sizeof leaf, "%llu.record", (unsigned long long)session.pid);
    if (!join(staging_path, sizeof staging_path, running_dir, leaf)) { close_file(marker); marker = NO_FILE; remove_file(marker_path); return; }
    staging = open_file(staging_path, 1, 0);
    if (staging == NO_FILE || !write_file(marker, &session, sizeof session) || !durable(marker)) {
        close_file(staging); staging = NO_FILE; close_file(marker); marker = NO_FILE;
        remove_file(staging_path); remove_file(marker_path); return;
    }
    atexit(hos_crash_clean_end);
#ifdef _WIN32
    prepare_dump(root);
#endif
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
}
int hos_crash_record_write(const HosCrashContext *context) {
    int expected = 0;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || !context ||
        !__atomic_compare_exchange_n(&claimed, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    const uint64_t now = epoch_now();
    format_record(&record, &session, context, now, NULL);
    if (record.full || !write_file(staging, record.bytes, record.n) || !durable(staging)) return 0;
    const uint32_t complete = 1;
#ifdef _WIN32
    LARGE_INTEGER at; at.QuadPart = offsetof(Session, record_complete);
    if (!SetFilePointerEx(marker, at, NULL, FILE_BEGIN) || !write_file(marker, &complete, sizeof complete)) return 0;
#else
    if (pwrite(marker, &complete, sizeof complete, offsetof(Session, record_complete)) != (ssize_t)sizeof complete) return 0;
#endif
    if (!durable(marker)) return 0;
    close_file(staging); staging = NO_FILE;
    if (!publish(staging_path, "crash", now, session.pid)) return 0;
    remove_file(marker_path); close_file(marker); marker = NO_FILE;
    return 1;
}
