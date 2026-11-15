/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (hookshot) The window right after a crash, at start or in play: "Hookshot closed unexpectedly", when, why where it is known,
   and Report, Open the log folder, Close. Report puts a redacted plain-text report on the clipboard, writes it next to the
   crash records (bug-reports/pending/report-<UTC>.txt) and opens the issue form for game problems in the browser; the game
   itself sends nothing. What decides is host/hos_crash_dialog.h.

   It is shown by a helper the game starts early (hos_crash_watch_start, after the crash record is set up and before the
   guest, the renderer, the sound and the input): the same executable, `hookshot --crash-watch <pid> <run id> <game folder>
   <log>`, which main() hands here first thing (hos_crash_helper_main), so the helper never starts any of the game. It waits
   for the game's process to end (Windows: its handle, with its exit code; Linux: a pidfd, else its parent changing), reads
   the records and the log, and shows the window at once, or ends quietly. A start that finds a crash record of an earlier run
   that no helper showed (bug-reports/shown.txt lists those shown) shows it first, through a helper it waits for
   (`--crash-show`). Not for --prepare-shaders, HOS_HEADLESS, HOS_PGO, HOS_CRASH_WATCH=0 or under gamescope
   (hos_crash_watch_wanted).

   The window is SDL's message box (SDL3 or SDL2, loaded here: the helper loads nothing else of the game's), Windows'
   MessageBoxW without SDL; the clipboard and the browser are Windows' own there; on Linux the clipboard is wl-copy, xclip or
   xsel's (SDL's X11 one the last resort) and the browser SDL's or xdg-open.
   HOS_CRASH_TEST=start (just after the crash handlers are set up) or =<seconds> (that long after) crashes the game on
   purpose, from the environment only (not hookshot_env.txt), to see the window. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "hos_crash_dialog.h"
#include "hos_crash_record.h"
#include "../gpu/vk/compat/hookshot_env.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <objbase.h>
#include <shellapi.h>
#else
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

enum { PATH_MAX_UTF8 = 4096 };
#ifdef HOS_CRASH_DIALOG_TEST   /* (tests/test_crash_dialog.py: the window, the clipboard and the browser are the test's) */
int hos_crash_test_box(const char *title, const char *text, const char *const *buttons, int n);
int hos_crash_test_clipboard(const char *text);
int hos_crash_test_open(const char *what);
#endif

/* ---- files, by UTF-8 paths ---- */
#ifdef _WIN32
static wchar_t *wide_of(const char *s) {   /* (malloc'd; NULL when not UTF-8) */
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) free(w), w = NULL;
    return w;
}
static char *utf8_of(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) != n) free(s), s = NULL;
    return s;
}
#endif
static FILE *open_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *w = wide_of(path), m[8];
    size_t i = 0;
    for (; mode[i] && i + 1 < 8; i++) m[i] = (wchar_t)mode[i];
    m[i] = 0;
    FILE *f = w ? _wfopen(w, m) : NULL;
    free(w);
    return f;
#else
    return fopen(path, mode);
#endif
}
static int join(char *out, size_t cap, const char *a, const char *b) {
    const int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}
/* A file's first max bytes, ended by a NUL (malloc'd), NULL when it cannot be read */
static char *read_text(const char *path, size_t max, size_t *len) {
    FILE *f = open_utf8(path, "rb");
    if (!f) return NULL;
    char *b = (char *)malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    if (len) *len = n;
    return b;
}
static int write_text(const char *path, const char *text, size_t n) {
    FILE *f = open_utf8(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static int remove_utf8(const char *path) {
#ifdef _WIN32
    wchar_t *w = wide_of(path);
    const int ok = w && DeleteFileW(w);
    free(w);
    return ok;
#else
    return unlink(path) == 0;
#endif
}
/* A file replaced whole: written beside it under a name of this process's, then renamed over it, so a reader meanwhile has
   the old text or the new, never an empty or a half-written file */
static int replace_text(const char *path, const char *text, size_t n) {
    char temp[PATH_MAX_UTF8 + 32];
#ifdef _WIN32
    snprintf(temp, sizeof temp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(temp, sizeof temp, "%s.%ld.tmp", path, (long)getpid());
#endif
    if (!write_text(temp, text, n)) return remove_utf8(temp), 0;
#ifdef _WIN32
    wchar_t *from = wide_of(temp), *to = wide_of(path);
    const int ok = from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    free(from), free(to);
#else
    const int ok = rename(temp, path) == 0;
#endif
    if (!ok) remove_utf8(temp);
    return ok;
}
static void utc_now(char out[17]) {
    const time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(out, 17, "%Y%m%dT%H%M%SZ", &tm);
}
static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
#endif
}
/* Each name in a folder (UTF-8), in turn */
static void each_name(const char *dir, void (*take)(const char *name, void *user), void *user) {
#ifdef _WIN32
    char pattern[PATH_MAX_UTF8];
    if (!join(pattern, sizeof pattern, dir, "*")) return;
    wchar_t *w = wide_of(pattern);
    WIN32_FIND_DATAW d;
    HANDLE h = w ? FindFirstFileW(w, &d) : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name = utf8_of(d.cFileName);
        if (name && !(d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) take(name, user);
        free(name);
    } while (FindNextFileW(h, &d));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') take(e->d_name, user);
    closedir(d);
#endif
}

/* ---- a growing text ---- */
typedef struct Buf { char *s; size_t n, cap; } Buf;
static void put_n(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->n + n + 1 > cap) cap *= 2;
        char *more = (char *)realloc(b->s, cap);
        if (!more) return;
        b->s = more, b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void put(Buf *b, const char *s) { put_n(b, s, strlen(s)); }
static void putf(Buf *b, const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) put_n(b, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

/* ---- who is to be left out: the home folder and the user's name (the Insert menu's Bug report's fill_here) ---- */
typedef struct Who { char home[PATH_MAX_UTF8], user[256]; int windows; } Who;
static void who_here(Who *w) {
    memset(w, 0, sizeof *w);
#ifdef _WIN32
    w->windows = 1;
    const wchar_t *h = _wgetenv(L"USERPROFILE"), *u = _wgetenv(L"USERNAME");
    char *hs = h ? utf8_of(h) : NULL, *us = u ? utf8_of(u) : NULL;
    if (hs) snprintf(w->home, sizeof w->home, "%s", hs);
    if (us) snprintf(w->user, sizeof w->user, "%s", us);
    free(hs), free(us);
#else
    const struct passwd *pw = getpwuid(getuid());
    const char *h = getenv("HOME"), *u = getenv("USER");
    snprintf(w->home, sizeof w->home, "%s", h && *h ? h : pw && pw->pw_dir ? pw->pw_dir : "");
    snprintf(w->user, sizeof w->user, "%s", u && *u ? u : pw && pw->pw_name ? pw->pw_name : "");
#endif
}
static char *redacted(const Who *w, const char *s, size_t n) {   /* (malloc'd) */
    const size_t cap = n * 3 + 64;   /* (each byte at most "<user>" for a name of 2 or more: never longer than 3x and a home) */
    char *out = (char *)malloc(cap);
    if (out) hos_crash_redact_line(out, cap, s, n, w->home, w->user, w->windows);
    return out;
}

/* ---- the run's log ---- */
typedef struct RunLog {
    HosCrashLog facts;
    char *lines[HOS_CRASH_LOG_LINES];   /* (the last ones, redacted, a ring) */
    int count, next, found;
    char path[PATH_MAX_UTF8];
} RunLog;
static int log_is_run(const char *path, const char *run_id) {
    char *head = read_text(path, 8192, NULL);
    char want[48];
    snprintf(want, sizeof want, "run_id: %s", run_id);
    const int yes = head && strstr(head, want) != NULL;
    free(head);
    return yes;
}
/* This run's log: the path it named, else its first rotated name (hookshot_log.1.txt: a start since moved it), when its run
   id is the one wanted */
static int find_run_log(RunLog *r, const char *log, const char *run_id) {
    if (!log || !run_id || !run_id[0]) return 0;
    if (log_is_run(log, run_id)) return snprintf(r->path, sizeof r->path, "%s", log), 1;
    const char *dot = strrchr(log, '.'), *sep = strrchr(log, '/');
    const char *bsep = strrchr(log, '\\');
    if (bsep > sep) sep = bsep;
    if (dot && (!sep || dot > sep)) {
        snprintf(r->path, sizeof r->path, "%.*s.1%s", (int)(dot - log), log, dot);
        if (log_is_run(r->path, run_id)) return 1;
    }
    r->path[0] = 0;
    return 0;
}
static void read_log(RunLog *r, const Who *who) {
    FILE *f = r->path[0] ? open_utf8(r->path, "rb") : NULL;
    if (!f) return;
    r->found = 1;
    char line[4096];
    /* (the whole log for what it says, up to 256 MB; its last lines kept) */
    for (uint64_t taken = 0; taken < (256ull << 20) && fgets(line, sizeof line, f);) {
        const size_t n = strlen(line);
        taken += n;
        char *clean = redacted(who, line, n);
        if (!clean) continue;
        hos_crash_log_line(&r->facts, clean);
        free(r->lines[r->next]);
        r->lines[r->next] = clean;
        r->next = (r->next + 1) % HOS_CRASH_LOG_LINES;
        if (r->count < HOS_CRASH_LOG_LINES) r->count++;
    }
    fclose(f);
}

/* ---- the records ---- */
typedef struct Found { const char *pending, *run_id; char name[128]; char *text; int kind; } Found;
static void take_record(const char *name, void *user) {
    Found *f = (Found *)user;
    const int kind = hos_crash_record_name(name, NULL);
    if (!kind || (f->kind == 1 && kind == 2)) return;   /* (a crash record wins over an unclean end) */
    char path[PATH_MAX_UTF8], want[48];
    if (!join(path, sizeof path, f->pending, name)) return;
    char *text = read_text(path, 64 << 10, NULL);
    snprintf(want, sizeof want, "\nrun_id: %s\n", f->run_id);
    if (text && strstr(text, want)) {
        free(f->text);
        f->text = text, f->kind = kind;
        snprintf(f->name, sizeof f->name, "%s", name);
        return;
    }
    free(text);
}
static int pending_of(const char *dir, char *out, size_t cap) {
    char root[PATH_MAX_UTF8];
    return join(root, sizeof root, dir, "bug-reports") && join(out, cap, root, "pending");
}
/* This run's record, crash before unclean: its text (malloc'd), NULL when it has none */
static char *run_record(const char *dir, const char *run_id, char *name, size_t cap) {
    char pending[PATH_MAX_UTF8];
    Found f = {pending, run_id, "", NULL, 0};
    if (!pending_of(dir, pending, sizeof pending)) return NULL;
    each_name(pending, take_record, &f);
    snprintf(name, cap, "%s", f.name);
    return f.text;
}

/* ---- the ledger of records shown: bug-reports/shown.txt ---- */
static int ledger_path(const char *dir, char *out, size_t cap) {
    char root[PATH_MAX_UTF8];
    return join(root, sizeof root, dir, "bug-reports") && join(out, cap, root, "shown.txt");
}
typedef struct Keep { Buf *out; const char *old; } Keep;
static void keep_listed(const char *name, void *user) {
    Keep *k = (Keep *)user;
    if (hos_crash_record_name(name, NULL) && (!k->old || hos_crash_listed(k->old, name))) put(k->out, name), put(k->out, "\n");
}
/* Adds names to the ledger (a NULL name: every record now pending), keeping only the names still pending */
static int ledger_add(const char *dir, const char *name, int all) {
    char path[PATH_MAX_UTF8], pending[PATH_MAX_UTF8];
    if (!ledger_path(dir, path, sizeof path) || !pending_of(dir, pending, sizeof pending)) return 0;
    char *old = read_text(path, 64 << 10, NULL);
    Buf out = {0};
    put(&out, "");
    Keep k = {&out, all || !old ? NULL : old};   /* (no ledger yet: what was pending is not news) */
    each_name(pending, keep_listed, &k);
    if (name && !hos_crash_listed(out.s, name)) put(&out, name), put(&out, "\n");
    const int ok = replace_text(path, out.s ? out.s : "", out.n);
    free(old), free(out.s);
    return ok;
}
typedef struct Newest { const char *shown; int64_t now; char name[128]; } Newest;
static void newest_unshown(const char *name, void *user) {
    Newest *n = (Newest *)user;
    if (hos_crash_show_at_start(name, n->shown, n->now) && (!n->name[0] || strcmp(name, n->name) > 0))
        snprintf(n->name, sizeof n->name, "%s", name);
}

/* ---- this PC, as the issue form asks: "CPU (threads), RAM, system" (the menu's system_text) ---- */
static void system_text(char *out, size_t cap) {
    char cpu[64] = "";
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
    unsigned r[12] = {0};
#ifdef _WIN32
    int regs[4];
    __cpuid(regs, (int)0x80000000);
    if ((unsigned)regs[0] >= 0x80000004u)
        for (int k = 0; k < 3; k++) {
            __cpuid(regs, (int)(0x80000002u + (unsigned)k));
            for (int j = 0; j < 4; j++) r[k * 4 + j] = (unsigned)regs[j];
        }
#else
    unsigned top, ebx, ecx, edx;
    if (__get_cpuid(0x80000000u, &top, &ebx, &ecx, &edx) && top >= 0x80000004u)
        for (unsigned k = 0; k < 3; k++) __get_cpuid(0x80000002u + k, &r[k * 4], &r[k * 4 + 1], &r[k * 4 + 2], &r[k * 4 + 3]);
#endif
    memcpy(cpu, r, 48);
    cpu[48] = 0;
#endif
    char *cpu_name = cpu;   /* (the brand string, its spaces around it taken off) */
    while (*cpu_name == ' ') cpu_name++;
    for (size_t n = strlen(cpu_name); n && cpu_name[n - 1] == ' ';) cpu_name[--n] = 0;
    unsigned long long ram = 0;
    long threads = 0;
    char os[256] = "";
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) ram = m.ullTotalPhys;
    threads = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    typedef LONG(WINAPI * GetVersionFn)(OSVERSIONINFOW *);
    typedef const char *(__cdecl * WineFn)(void);
    const HMODULE nt = GetModuleHandleA("ntdll.dll");
    const GetVersionFn get = nt ? (GetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (get && get(&v) == 0)
        snprintf(os, sizeof os, "%s (build %lu)",
                 v.dwMajorVersion == 10 ? (v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") : "Windows", v.dwBuildNumber);
    else snprintf(os, sizeof os, "Windows");
    const WineFn wine = nt ? (WineFn)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
    if (wine) snprintf(os + strlen(os), sizeof os - strlen(os), ", Wine %s", wine());
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) ram = (unsigned long long)pages * (unsigned long long)page;
    threads = sysconf(_SC_NPROCESSORS_ONLN);
    for (int k = 0; k < 2 && !os[0]; k++) {
        FILE *f = fopen(k ? "/usr/lib/os-release" : "/etc/os-release", "r");
        char line[256];
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12;
                v[strcspn(v, "\r\n")] = 0;
                size_t n = strlen(v);
                if (n >= 2 && (v[0] == '"' || v[0] == '\'')) v[n - 1] = 0, v++;
                snprintf(os, sizeof os, "%s", v);
            }
        if (f) fclose(f);
    }
    if (!os[0]) snprintf(os, sizeof os, "Linux");
    struct utsname u;
    if (!uname(&u)) snprintf(os + strlen(os), sizeof os - strlen(os), " (%s %s)", u.sysname, u.release);
    const char *desktop = getenv("XDG_CURRENT_DESKTOP"), *session = getenv("XDG_SESSION_TYPE");
    if (desktop && *desktop)
        snprintf(os + strlen(os), sizeof os - strlen(os), ", %s%s%s%s", desktop, session && *session ? " (" : "",
                 session && *session ? session : "", session && *session ? ")" : "");
#endif
    char gb[32] = "";
    if (ram) snprintf(gb, sizeof gb, "%llu GB, ", (ram + (1ull << 29)) >> 30);
    snprintf(out, cap, "%s (%ld threads), %s%s", *cpu_name ? cpu_name : "an unknown CPU", threads, gb, os);
}
/* What the game was built for: this PC's own CPU (nx2/build.py, -march=native and HOOKSHOT_NATIVE_CPU), or a CPU level */
#define HOS_CRASH_TEXT_(...) #__VA_ARGS__
#define HOS_CRASH_TEXT(...) HOS_CRASH_TEXT_(__VA_ARGS__)
static const char *built_for(void) {
#ifdef HOOKSHOT_NATIVE_CPU
    return "this PC's CPU (" HOS_CRASH_TEXT(HOOKSHOT_NATIVE_CPU) ")";
#elif defined(__AVX2__)
    return "x86-64-v3";
#else
    return "an older CPU level (x86-64 or x86-64-v2: no AVX2)";
#endif
}
static const char *channel_of(const char *version) {
    if (strstr(version, "nightly")) return "Nightly";
    if (strstr(version, "-rc.")) return "Release candidate, or built from source";
    const char *p = version;
    if (*p++ != 'v') return "Built from source, or not known";
    for (int part = 0; part < 3; part++) {
        if (!(*p >= '0' && *p <= '9')) return "Built from source, or not known";
        while (*p >= '0' && *p <= '9') p++;
        if (part < 2 && *p++ != '.') return "Built from source, or not known";
    }
    return !*p || *p == ' ' ? "Release" : "Built from source, or not known";
}

/* ---- the report ---- */
typedef struct Report {
    int verdict, stage, cause;
    const char *record, *record_name, *dir;
    const RunLog *log;
    const Who *who;
    char system[400], gpu[400], version[160];
} Report;
static int cmp_text(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* The game's switches and what loads code into it, NAME=VALUE, a value only as hos_crash_value_shown allows (the menu's
   switches()) */
static void put_switches(Buf *b) {
    static const char *const prefixes[] = {"RX_", "HOS_", "HOOKSHOT_", "EDEN_", "NVN_", "SDL_"};
    static const char *const hooks[] = {"VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_DRIVER_FILES", "VK_ICD_FILENAMES",
                                        "ENABLE_VKBASALT", "MANGOHUD", "LD_PRELOAD"};
    char *lines[512];
    int n = 0;
#ifdef _WIN32
    wchar_t *block = GetEnvironmentStringsW();
    for (const wchar_t *e = block; e && *e && n < 512; e += wcslen(e) + 1) {
        char *kv = utf8_of(e);
#else
    for (char **e = environ; e && *e && n < 512; e++) {
        char *kv = strdup(*e);
#endif
        char *eq = kv ? strchr(kv, '=') : NULL;
        int take = 0;
        if (eq && eq != kv) {
            *eq = 0;
            for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) take |= !strncmp(kv, prefixes[i], strlen(prefixes[i]));
            for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) take |= !strcmp(kv, hooks[i]);
        }
        if (take) {
            const size_t cap = strlen(kv) + strlen(eq + 1) + 32;
            lines[n] = (char *)malloc(cap);
            if (lines[n]) snprintf(lines[n], cap, "%s=%s", kv, hos_crash_value_shown(kv, eq + 1) ? eq + 1 : "(value left out)"), n++;
        }
        free(kv);
    }
#ifdef _WIN32
    if (block) FreeEnvironmentStringsW(block);
#endif
    qsort(lines, (size_t)n, sizeof lines[0], cmp_text);
    if (!n) put(b, "Switches set: none.\n");
    else put(b, "Switches set (the environment and hookshot_env.txt; a value only when it is a number or on/off, and never for a "
                "name with TOKEN, KEY, ROOM, SECRET, PASS, SERVER or USER in it):\n");
    for (int i = 0; i < n; i++) put(b, lines[i]), put(b, "\n"), free(lines[i]);
}
/* hookshot_options.ini's settings, key=value, a value only when it is a number, a size or on/off (a device's name is left
   out); not the controls' bindings, nor the file itself */
static void put_options(Buf *b, const char *dir) {
    char path[PATH_MAX_UTF8];
    char *text = join(path, sizeof path, dir, "hookshot_options.ini") ? read_text(path, 256 << 10, NULL) : NULL;
    if (!text) {
        put(b, "hookshot_options.ini: none (every Insert menu option at its default).\n");
        return;
    }
    put(b, "hookshot_options.ini (a value only when it is a number or on/off; the controls' bindings left out):\n");
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        int name_ok = 1;
        for (const char *p = line; *p; p++) name_ok &= (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!name_ok || !strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4) || !strncmp(line, "toggle_", 7)) continue;
        putf(b, "%s=%s\n", line, hos_crash_value_shown(NULL, eq + 1) ? eq + 1 : "(value left out)");
    }
    free(text);
}
typedef struct Recent { int64_t now; int count; } Recent;
static void count_recent(const char *name, void *user) {
    Recent *r = (Recent *)user;
    char when[17];
    if (hos_crash_record_name(name, when) && r->now - hos_crash_epoch(when) <= 86400) r->count++;
}
static char *report_text(const Report *in, size_t *len) {
    Buf b = {0};
    char now[17];
    utc_now(now);
    putf(&b, "Hookshot crash report, made by the window Hookshot shows after a crash (%s UTC). Hookshot sent nothing: paste "
             "this into the issue form, and attach the game folder's logs/hookshot_log.txt if you can.\n\n", now);
    put(&b, "### What happened\n\n");
    char message[1024];
    hos_crash_message(message, sizeof message, in->verdict, in->stage, in->cause);
    message[strcspn(message, "\n")] = 0;
    putf(&b, "%s\nWhat went wrong: %s\n", message, hos_crash_cause_text(in->cause));
    HosCrashRecord r;
    hos_crash_record_parse(in->record ? in->record : "", &r);
    if (r.uptime[0] && strcmp(r.uptime, "unknown")) putf(&b, "It had run for %s s.\n", r.uptime);
    char pending[PATH_MAX_UTF8];
    Recent recent = {(int64_t)time(NULL), 0};
    if (pending_of(in->dir, pending, sizeof pending)) each_name(pending, count_recent, &recent);
    putf(&b, "Crash and unclean-end records in the last day: %d.\n\n", recent.count);
    putf(&b, "### Hookshot version\n\n%s\nChannel (from the version): %s\n\n", in->version, channel_of(in->version));
    putf(&b, "### Graphics card and driver version\n\n%s\n", in->gpu);
    if (in->log->facts.devices[0]) putf(&b, "Devices the log lists: %s\n", in->log->facts.devices);
    putf(&b, "\n### CPU, RAM and operating system\n\n%s\nBuilt for: %s\n\n", in->system, built_for());
    put(&b, "### Upscalers (the log's first line of each)\n\n");
    putf(&b, "%s\n%s\n\n", in->log->facts.dlss[0] ? in->log->facts.dlss : "NVIDIA DLSS: not named in the log",
         in->log->facts.fsr4[0] ? in->log->facts.fsr4 : "AMD FSR 4: not named in the log");
    put(&b, "### Changes from the defaults\n\n");
    put_options(&b, in->dir);
    put_switches(&b);
    putf(&b, "\n### Crash record%s%s\n\n", in->record_name && in->record_name[0] ? ": " : "",
         in->record_name ? in->record_name : "");
    if (in->record) {   /* (already private-free: hos_crash_record.c writes fixed labels, module names and registers only) */
        put(&b, "```\n");
        put(&b, in->record);
        if (b.n && b.s[b.n - 1] != '\n') put(&b, "\n");
        put(&b, "```\n\n");
    } else {
        put(&b, "None: the game's crash handler wrote no record (it stopped before the game could run, or ended normally).\n\n");
    }
    if (in->log->found) {
        putf(&b, "### The end of hookshot_log.txt\n\nIts last %d lines (the home folder written %s, the user's name <user>):\n\n```\n",
             in->log->count, in->who->windows ? "%USERPROFILE%" : "~");
        for (int i = 0; i < in->log->count; i++) {
            const char *line = in->log->lines[(in->log->next - in->log->count + i + HOS_CRASH_LOG_LINES) % HOS_CRASH_LOG_LINES];
            put(&b, line);
            if (!*line || line[strlen(line) - 1] != '\n') put(&b, "\n");
        }
        put(&b, "```\n\n");
    } else {
        put(&b, "### hookshot_log.txt\n\nNot found: the game logged to no file (started from a terminal or by a program that "
                "reads its output), or its log was moved since.\n\n");
    }
    put(&b, "### Not in this report\n\nSaves, game files, the game's executable and caches, memory dumps, keys, the options file "
            "itself and the path of your home folder.\n");
    if (len) *len = b.n;
    return b.s;
}

/* ---- SDL's message box, clipboard and links (loaded here, not through host/hos_sdl.c: the helper starts nothing else) ---- */
static struct {
    int version;   /* 3, 2 or 0 */
    void *ShowMessageBox, *SetClipboardText, *OpenURL, *InitSubSystem, *PumpEvents;
} sdl;
static void *sdl_lib(const char *name) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4], wname[64];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 4);
    while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/') n--;
    size_t k = 0;
    for (; name[k] && k + 1 < 64; k++) wname[k] = (wchar_t)name[k];
    wname[k] = 0;
    HMODULE m = NULL;
    if (n && n + k + 1 < MAX_PATH * 4) {
        memcpy(exe + n, wname, (k + 1) * sizeof *exe);
        m = LoadLibraryW(exe);
    }
    return m ? (void *)m : (void *)LoadLibraryExW(wname, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    char exe[PATH_MAX_UTF8];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 64);
    void *m = NULL;
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) strcpy(slash + 1, name), m = dlopen(exe, RTLD_NOW | RTLD_LOCAL);
    }
    return m ? m : dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void *sdl_fn(void *lib, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}
static int sdl_load(void) {
    static int tried;
    if (tried) return sdl.version;
    tried = 1;
    const char *want = getenv("HOS_SDL_VERSION");
#ifdef _WIN32
    const char *names[2] = {"SDL3.dll", "SDL2.dll"};
#else
    const char *names[2] = {"libSDL3.so.0", "libSDL2-2.0.so.0"};
#endif
    for (int k = want && atoi(want) == 2 ? 1 : 0; k < 2 && !sdl.version; k++) {
        void *lib = sdl_lib(names[k]);
        if (!lib) continue;
        sdl.ShowMessageBox = sdl_fn(lib, "SDL_ShowMessageBox");
        sdl.SetClipboardText = sdl_fn(lib, "SDL_SetClipboardText");
        sdl.OpenURL = sdl_fn(lib, "SDL_OpenURL");   /* (SDL 2.0.14 on) */
        sdl.InitSubSystem = sdl_fn(lib, "SDL_InitSubSystem");
        sdl.PumpEvents = sdl_fn(lib, "SDL_PumpEvents");
        if (sdl.ShowMessageBox && sdl.InitSubSystem) sdl.version = k ? 2 : 3;
    }
    return sdl.version;
}
typedef struct { uint32_t flags; int buttonid; const char *text; } SdlButton;   /* (SDL3's and SDL2's alike) */
typedef struct {
    uint32_t flags;
    void *window;
    const char *title, *message;
    int numbuttons;
    const SdlButton *buttons;
    const void *colors;
} SdlBox;
/* The button clicked, by its index (the first is the Enter key's, the last Escape's), -1 when the box was closed otherwise, -2
   when it could not be shown */
static int sdl_box(const char *title, const char *text, const char *const *buttons, int n) {
    SdlButton b[4];
    for (int i = 0; i < n && i < 4; i++) b[i].flags = (i == 0 ? 1u : 0u) | (i == n - 1 ? 2u : 0u), b[i].buttonid = i, b[i].text = buttons[i];
    const SdlBox box = {0x10 | 0x80 /* ERROR, buttons left to right */, NULL, title, text, n, b, NULL};
    int hit = -1;
    if (sdl.version == 3) return ((_Bool (*)(const SdlBox *, int *))sdl.ShowMessageBox)(&box, &hit) ? hit : -2;
    return ((int (*)(const SdlBox *, int *))sdl.ShowMessageBox)(&box, &hit) == 0 ? hit : -2;
}
static int ui_box(const char *title, const char *text, const char *const *buttons, int n) {
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_box(title, text, buttons, n);
#endif
    if (sdl_load()) {
        const int hit = sdl_box(title, text, buttons, n);
        if (hit != -2) return hit;
    }
#ifdef _WIN32
    /* (no SDL: Windows' own box, its Yes/No/Cancel standing for the buttons, said in its text) */
    Buf t = {0};
    put(&t, text);
    put(&t, "\n\n");
    static const char *const keys[] = {"Yes", "No", "Cancel"};
    for (int i = 0; i < n && i < 3; i++) putf(&t, "%s%s: %s", i ? ", " : "", n == 1 ? "OK" : keys[i], buttons[i]);
    wchar_t *wt = t.s ? wide_of(t.s) : NULL, *wc = wide_of(title);
    const UINT kind = n >= 3 ? MB_YESNOCANCEL : n == 2 ? MB_YESNO : MB_OK;
    const int r = wt && wc ? MessageBoxW(NULL, wt, wc, kind | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST) : 0;
    free(t.s), free(wt), free(wc);
    return r == IDYES || r == IDOK ? 0 : r == IDNO ? 1 : r == IDCANCEL ? 2 : r ? -1 : -2;
#else
    return -2;   /* (no SDL, no window: the game itself would have had none) */
#endif
}

#ifdef _WIN32
/* The report onto the clipboard: 0 not, 1 there for good (the system's or a tool's), 2 only while this helper runs */
static int clipboard(const char *text) {   /* (Windows' own: the text stays on the clipboard after the helper ends) */
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_clipboard(text);
#endif
    Buf crlf = {0};
    put(&crlf, "");
    for (const char *p = text; *p; p++) {   /* (Notepad and the like want \r\n) */
        if (*p == '\n' && (p == text || p[-1] != '\r')) put_n(&crlf, "\r", 1);
        put_n(&crlf, p, 1);
    }
    wchar_t *w = crlf.s ? wide_of(crlf.s) : NULL;
    free(crlf.s);
    if (!w) return 0;
    const size_t bytes = (wcslen(w) + 1) * sizeof *w;
    HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    int ok = 0;
    for (int tries = 0; tries < 20 && !ok; tries++) {   /* (another program can hold the clipboard for a moment) */
        if (!OpenClipboard(owner)) {
            sleep_ms(50);
            continue;
        }
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
        void *p = g ? GlobalLock(g) : NULL;
        if (p) {
            memcpy(p, w, bytes);
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
        }
        if (g && !ok) GlobalFree(g);
        CloseClipboard();
        break;
    }
    if (owner) DestroyWindow(owner);
    free(w);
    return ok;
}
static int open_thing(const char *what) {   /* (a link, a folder or a file: what the system opens it with) */
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_open(what);
#endif
    wchar_t *w = wide_of(what);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const int ok = w && (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
    free(w);
    return ok;
}
#else
static int video_on;
/* A clipboard tool (hos_crash_clip_tool) given the text on its input: 1 when it took it (it exited with 0, as wl-copy, xclip and
   xsel do once a background copy of theirs serves the selection; or is still running a moment later, serving it itself) */
static int clip_tool(const char *const *argv, const char *text) {
    int in[2];
    if (pipe2(in, O_CLOEXEC)) return 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    const int started = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    if (!started) return close(in[1]), 0;
    signal(SIGPIPE, SIG_IGN);   /* (a tool that ends without reading: a failed write, not this helper's end) */
    int wrote = 1;
    for (size_t n = strlen(text), at = 0; at < n;) {
        const ssize_t k = write(in[1], text + at, n - at);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { wrote = 0; break; }
        at += (size_t)k;
    }
    close(in[1]);
    for (int waited = 0; waited < 3000; waited += 20) {   /* (reaped here: no zombie; its background copy is init's) */
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return wrote && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (r < 0 && errno != EINTR) return 0;
        sleep_ms(20);
    }
    return wrote;   /* (still running after 3 s with all of the text: it serves it in the foreground) */
}
/* Wayland's or X11's clipboard, through a tool that keeps it after the helper ends; else X11's through SDL, given while the next
   box is open (hence its pumping then). Never SDL on Wayland: it takes the text only from a focused window, which the helper
   has none of once its box is gone, and says it did anyway */
static int clipboard(const char *text) {
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_clipboard(text);
#endif
    const char *wl = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    const int wayland = wl && *wl, x11 = x && *x;
    for (int i = 0; hos_crash_clip_tool(wayland, x11, i); i++)
        if (clip_tool(hos_crash_clip_tool(wayland, x11, i), text)) return 1;
    if (!x11 || !sdl_load() || !sdl.SetClipboardText || !sdl.PumpEvents) return 0;
    if (!video_on) {
        setenv("SDL_VIDEODRIVER", "x11", 1), setenv("SDL_VIDEO_DRIVER", "x11", 1);
        video_on = sdl.version == 3 ? ((_Bool (*)(uint32_t))sdl.InitSubSystem)(0x20) : ((int (*)(uint32_t))sdl.InitSubSystem)(0x20) == 0;
        if (!video_on) return 0;
    }
    const int ok = sdl.version == 3 ? ((_Bool (*)(const char *))sdl.SetClipboardText)(text)
                                    : ((int (*)(const char *))sdl.SetClipboardText)(text) == 0;
    return ok ? 2 : 0;
}
/* A path as a file:// link (each byte that is not plain escaped, its slashes kept) */
static void file_url(char *out, size_t cap, const char *path) {
    size_t n = (size_t)snprintf(out, cap, "file://");
    for (const char *p = path; *p && n + 4 < cap; p++) {
        if (*p == '/') out[n++] = '/';
        else {
            const char one[2] = {*p, 0};
            n += hos_crash_url_part(out + n, cap - n, one);
        }
    }
    out[n < cap ? n : cap - 1] = 0;
}
static int open_thing(const char *what) {
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_open(what);
#endif
    char url[PATH_MAX_UTF8 * 3 + 16];
    if (what[0] == '/') file_url(url, sizeof url, what);
    else snprintf(url, sizeof url, "%s", what);
    if (sdl_load() && sdl.OpenURL)
        return sdl.version == 3 ? ((_Bool (*)(const char *))sdl.OpenURL)(url) : ((int (*)(const char *))sdl.OpenURL)(url) == 0;
    /* (an SDL before 2.0.14: xdg-open itself) */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {(char *)"xdg-open", url, NULL};
    pid_t pid;
    const int ok = posix_spawnp(&pid, "xdg-open", &fa, NULL, argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    int status = 0;
    if (ok) waitpid(pid, &status, 0);
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
typedef struct BoxJob { const char *title, *text; const char *const *buttons; int n, hit; volatile int done; } BoxJob;
static void *box_thread(void *p) {
    BoxJob *j = (BoxJob *)p;
    j->hit = ui_box(j->title, j->text, j->buttons, j->n);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}
#endif
/* A box while the clipboard is given (Linux: the box on a thread of its own, SDL's events pumped here meanwhile, on the thread
   that started SDL's video, so a paste in the browser gets the text) */
static int box_serving(const char *title, const char *text, const char *const *buttons, int n) {
#ifndef _WIN32
    if (video_on && sdl.PumpEvents) {
        BoxJob j = {title, text, buttons, n, -1, 0};
        pthread_t t;
        if (!pthread_create(&t, NULL, box_thread, &j)) {
            while (!__atomic_load_n(&j.done, __ATOMIC_ACQUIRE)) ((void (*)(void))sdl.PumpEvents)(), sleep_ms(20);
            pthread_join(t, NULL);
            return j.hit;
        }
    }
#endif
    return ui_box(title, text, buttons, n);
}

/* ---- the window, and Report ---- */
typedef struct Shown {
    int verdict, stage, cause;
    const char *dir, *record, *record_name;
    RunLog *log;
    Who who;
} Shown;
static void log_folder(const Shown *s, char *out, size_t cap) {
    snprintf(out, cap, "%s", s->log->path[0] ? s->log->path : "");
    char *a = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > a) a = b;
    if (a) *a = 0;
    else if (!join(out, cap, s->dir, "logs")) snprintf(out, cap, "%s", s->dir);
#ifdef _WIN32
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '\\';
    if (!strncmp(out, "\\\\?\\", 4) && strncmp(out, "\\\\?\\UNC\\", 8)) memmove(out, out + 4, strlen(out + 4) + 1);
#endif
}
/* The reports Report saved (bug-reports/pending/report-<UTC>.txt): the newest REPORTS_KEPT kept, the older removed (the
   records' own pruning, hos_crash_record.c prune_pending, takes only crash- and unclean- names) */
enum { REPORTS_KEPT = 10 };
typedef struct Reports { char names[64][32]; int n; } Reports;
static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void take_report(const char *name, void *user) {
    Reports *r = (Reports *)user;
    const size_t n = strlen(name);
    if (r->n < 64 && n == 27 && !strncmp(name, "report-", 7) && !strcmp(name + 23, ".txt"))
        memcpy(r->names[r->n++], name, n + 1);
}
static void prune_reports(const char *pending) {
    Reports r;
    r.n = 0;
    each_name(pending, take_report, &r);
    qsort(r.names, (size_t)r.n, sizeof r.names[0], cmp_name);   /* (oldest first: their UTC) */
    for (int i = 0; i + REPORTS_KEPT < r.n; i++) {
        char path[PATH_MAX_UTF8];
        if (join(path, sizeof path, pending, r.names[i])) remove_utf8(path);
    }
}
static void report(const Shown *s) {
    Report in;
    memset(&in, 0, sizeof in);
    in.verdict = s->verdict, in.stage = s->stage, in.cause = s->cause;
    in.record = s->record, in.record_name = s->record_name, in.dir = s->dir, in.log = s->log, in.who = &s->who;
    system_text(in.system, sizeof in.system);
    const HosCrashLog *f = &s->log->facts;
    if (f->gpu[0]) snprintf(in.gpu, sizeof in.gpu, "%s, %s", f->gpu, f->driver[0] ? f->driver : "driver not named in the log");
    else snprintf(in.gpu, sizeof in.gpu, "Not known: the log names no Vulkan device in use");
    char record_version[160] = "";
    if (s->record) hos_crash_record_value(s->record, "version", record_version, sizeof record_version);
    snprintf(in.version, sizeof in.version, "%s", f->version[0] ? f->version : record_version[0] ? record_version : "not known");
    size_t n = 0;
    char *text = report_text(&in, &n);
    if (!text) return;
    /* (beside the crash records: bug-reports/pending/report-<UTC>.txt) */
    char pending[PATH_MAX_UTF8], path[PATH_MAX_UTF8], leaf[64], stamp[17];
    utc_now(stamp);
    snprintf(leaf, sizeof leaf, "report-%s.txt", stamp);
    const int saved = pending_of(s->dir, pending, sizeof pending) && join(path, sizeof path, pending, leaf) && write_text(path, text, n);
    if (saved) prune_reports(pending);
    const int copied = clipboard(text);
    char headline[160], title[200], what[400], url[HOS_CRASH_LINK_MAX + 1];
    hos_crash_headline(headline, sizeof headline, hos_crash_title(s->verdict), s->stage);
    snprintf(title, sizeof title, "Game: %s", headline);
    snprintf(what, sizeof what, "%s. %s (The report from the crash window is pasted below.)", headline,
             hos_crash_cause_text(s->cause));
    hos_crash_issue_url(url, sizeof url, title, in.version, f->gpu[0] ? in.gpu : "", in.system, what);
    const int opened = open_thing(url);
    char local[PATH_MAX_UTF8];   /* (the saved report, as the system writes paths: what the box names and Open the report opens) */
    {   /* (one separator between names: the game folder ends with one; a share's leading two kept) */
        const char *from = saved ? path : "";
        size_t o = 0;
        for (size_t i = 0; from[i] && o + 1 < sizeof local; i++) {
            const int sep = from[i] == '/' || from[i] == '\\';
            if (sep && i > 1 && o && (local[o - 1] == '/' || local[o - 1] == '\\')) continue;
#ifdef _WIN32
            local[o++] = sep ? '\\' : from[i];
#else
            local[o++] = from[i];
#endif
        }
        local[o] = 0;
    }
    char *m = (char *)malloc(PATH_MAX_UTF8 + 1024);
    if (m) hos_crash_ready_text(m, PATH_MAX_UTF8 + 1024, copied, opened, saved ? local : NULL, HOS_CRASH_ISSUES);
    const char *const buttons[] = {"Open the report", "Done"};
    for (int shown = 0;; shown++) {
        const int hit = box_serving("Hookshot: report ready", m ? m : "", saved ? buttons : buttons + 1, saved ? 2 : 1);
        /* (no box could be shown at all: the saved report opened instead, so the player has it in front of them) */
        if (hit == -2 && !shown && saved) open_thing(local);
        if (!saved || hit != 0) break;
        open_thing(local);
    }
    free(m), free(text);
}
static void show(Shown *s) {
    char message[1400];
    hos_crash_message(message, sizeof message, s->verdict, s->stage, s->cause);
    const char *const buttons[] = {"Report", "Open the log folder", "Close"};
    for (;;) {
        const int hit = ui_box(hos_crash_title(s->verdict), message, buttons, 3);
        if (hit == 1) {
            char folder[PATH_MAX_UTF8];
            log_folder(s, folder, sizeof folder);
            open_thing(folder);
            continue;   /* (the window again: Report is still to be had) */
        }
        if (hit == 0) report(s);
        return;
    }
}

/* ---- the helper ---- */
#ifdef _WIN32
/* (the arguments as Windows has them, as UTF-8: the CRT's argv is the code page's, UTF-8 only with the game's manifest) */
static char **utf8_argv(int *argc) {
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), argc);
    char **a = w ? (char **)calloc((size_t)*argc + 1, sizeof *a) : NULL;
    for (int i = 0; a && i < *argc; i++) a[i] = utf8_of(w[i]);
    if (w) LocalFree(w);
    return a;
}
#endif
/* Waits for the game to end: 1, with its exit code where the system gives one; 0 when another run took its place (Linux's
   restart: the same process, a new run) */
static int wait_game(const HosCrashArgs *a, int *have_exit, unsigned long *code) {
    *have_exit = 0;
#ifdef _WIN32
    HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)a->pid);
    if (!p) return 1;   /* (gone already) */
    WaitForSingleObject(p, INFINITE);
    DWORD c = 0;
    if (GetExitCodeProcess(p, &c) && c != STILL_ACTIVE) *have_exit = 1, *code = c;
    CloseHandle(p);
    return 1;
#else
    (void)code;
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
    const int fd = (int)syscall(SYS_pidfd_open, (pid_t)a->pid, 0);
    for (;;) {
        if (fd >= 0) {
            struct pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, 500) > 0) break;
        } else {
            /* (an older kernel: the game is this helper's parent, so its end makes another process the parent) */
            if (getppid() != (pid_t)a->pid || (kill((pid_t)a->pid, 0) && errno == ESRCH)) break;
            sleep_ms(250);
        }
        if (hos_crash_record_marker(a->dir, a->pid, a->run_id) == 3) {   /* (restarted: execve, a new run's marker) */
            if (fd >= 0) close(fd);
            return 0;
        }
    }
    if (fd >= 0) close(fd);
    return 1;
#endif
}
static int helper(const HosCrashArgs *a) {
    Shown s;
    memset(&s, 0, sizeof s);
    s.dir = a->dir;
    who_here(&s.who);
    RunLog *log = (RunLog *)calloc(1, sizeof *log);
    if (!log) return 0;
    s.log = log;
    char name[128] = "", run_id[33] = "";
    char *record = NULL;
    int have_exit = 0;
    unsigned long code = 0;
    if (a->mode == 1) {
        if (!wait_game(a, &have_exit, &code)) return 0;
        /* (its marker becomes an unclean record once the process is gone; Windows lets go of the marker's lock a moment
           after the process ends, so a while) */
        for (int i = 0; i < 30; i++) {
            const int m = hos_crash_record_marker(a->dir, a->pid, a->run_id);
            if (m == 0 || m == 3) break;
            if (m == 1) hos_crash_record_recover(a->dir);
            if (hos_crash_record_marker(a->dir, a->pid, a->run_id) == 0) break;
            sleep_ms(100);
        }
        snprintf(run_id, sizeof run_id, "%s", a->run_id);
        record = run_record(a->dir, run_id, name, sizeof name);
    } else {
        char pending[PATH_MAX_UTF8], path[PATH_MAX_UTF8];
        if (pending_of(a->dir, pending, sizeof pending) && join(path, sizeof path, pending, a->record))
            record = read_text(path, 64 << 10, NULL);
        if (!record) return 0;
        snprintf(name, sizeof name, "%s", a->record);
        hos_crash_record_value(record, "run_id", run_id, sizeof run_id);
    }
    HosCrashRecord r;
    hos_crash_record_parse(record ? record : "", &r);
    if (find_run_log(log, a->log, run_id)) read_log(log, &s.who);
    s.verdict = hos_crash_verdict(&r, &log->facts, have_exit, code, 0);
    if (a->mode == 2 && r.kind != 1) s.verdict = HOS_CRASH_QUIET;
#ifdef _WIN32
    /* (a logoff or a shutdown ended the game: no window, which would hold the shutdown up; nor listed as shown, so a crash
       record of the run is still shown at the next start) */
#ifndef SM_SHUTTINGDOWN
#define SM_SHUTTINGDOWN 0x2000
#endif
    if (s.verdict != HOS_CRASH_QUIET && GetSystemMetrics(SM_SHUTTINGDOWN)) {
        free(record);
        for (int i = 0; i < HOS_CRASH_LOG_LINES; i++) free(log->lines[i]);
        free(log);
        return 0;
    }
#endif
    if (s.verdict != HOS_CRASH_QUIET) {
        s.stage = log->found ? hos_crash_stage(&log->facts) : HOS_STAGE_UNKNOWN;
        s.cause = hos_crash_cause(s.verdict, &r, &log->facts, have_exit, code);
        s.record = record, s.record_name = name[0] ? name : NULL;
        if (name[0]) ledger_add(a->dir, name, 0);   /* (before the window: a start meanwhile does not show it again) */
        show(&s);
    } else if (name[0]) {
        ledger_add(a->dir, name, 0);   /* (an end that says nothing: never shown at a start either) */
    }
    free(record);
    for (int i = 0; i < HOS_CRASH_LOG_LINES; i++) free(log->lines[i]);
    free(log);
    return 0;
}
#ifndef _WIN32
/* (Linux: what the game had open without close-on-exec (what a launcher or a shell left it) is let go first, so the helper,
   which can outlive the game while its window is open, holds no pipe or file of the game's) */
static void close_inherited(void) {
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
    int fds[256], n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return;
    const int own = dirfd(d);
    for (struct dirent *e; (e = readdir(d)) && n < 256;) {
        const int fd = atoi(e->d_name);
        if (fd > 2 && fd != own) fds[n++] = fd;
    }
    closedir(d);
    for (int i = 0; i < n; i++) close(fds[i]);
}
#endif
/* main()'s first call: the helper's work and its exit code, or -1 for the game itself */
int hos_crash_helper_main(int argc, char **argv) {
    HosCrashArgs a;
#ifdef _WIN32
    int wargc = 0;
    char **wargv = argc >= 2 && argv[1] && (!strcmp(argv[1], HOS_CRASH_WATCH_ARG) || !strcmp(argv[1], HOS_CRASH_SHOW_ARG))
                       ? utf8_argv(&wargc) : NULL;
    if (wargv) argc = wargc, argv = wargv;
    if (wargv) {   /* (sharp text in the window on a scaled screen, as the game's own: per-monitor aware) */
        typedef BOOL(WINAPI * SetAware)(void *);
        SetAware set = (SetAware)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (set) set((void *)(intptr_t)-4);
    }
#endif
    const int mode = hos_crash_args(argc, argv, &a);
    if (!mode) return -1;
#ifndef _WIN32
    close_inherited();
#endif
    return mode < 0 ? 0 : helper(&a);
}

/* ---- the game's side ---- */
/* Starts this executable with args; waits for it to end when asked. 0 when it could not be started */
static int spawn(const char *const *args, int n, int wait) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH * 4);
    if (!len || len >= MAX_PATH * 4) return 0;
    char *exe8 = utf8_of(exe);
    Buf line = {0};
    char quoted[PATH_MAX_UTF8 * 2 + 8];
    if (!exe8 || !hos_crash_quote_arg(quoted, sizeof quoted, exe8)) return free(exe8), 0;
    put(&line, quoted);
    for (int i = 0; i < n; i++) {
        if (!hos_crash_quote_arg(quoted, sizeof quoted, args[i])) return free(exe8), free(line.s), 0;
        put(&line, " "), put(&line, quoted);
    }
    wchar_t *cmd = line.s ? wide_of(line.s) : NULL;
    free(exe8), free(line.s);
    if (!cmd) return 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    /* (no console (a console build's), no handle of the game's (its log stays the game's to move), and out of the launcher's
       job where it allows that, so the window outlives a job closed with the game) */
    const DWORD flags = CREATE_NO_WINDOW;
    int ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) ||
             CreateProcessW(exe, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok) return 0;
    AllowSetForegroundWindow(pi.dwProcessId);   /* (its window may come to the front once the game is gone) */
    if (wait) WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return 1;
#else
    char *argv[8] = {(char *)"hookshot"};
    for (int i = 0; i < n && i < 6; i++) argv[i + 1] = (char *)args[i];
    argv[n + 1 < 8 ? n + 1 : 7] = NULL;
    /* (its output to nowhere: not the game's log, and not a launcher's pipe, which it would keep open after the game) */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    const int ok = posix_spawn(&pid, "/proc/self/exe", &fa, NULL, argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    if (ok && wait) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    return ok;
#endif
}
/* At the start, once the crash record is set up, before anything heavy: a crash record of an earlier run nobody showed is
   shown first (the game waits for that window), then the helper for this run starts */
void hos_crash_watch_start(int argc, char **argv) {
    void hos_log(const char *fmt, ...);
    const char *run = hos_crash_record_run(), *dir = hos_crash_record_dir(), *log = hos_crash_record_log();
    const char *off = getenv("HOS_CRASH_WATCH"), *pgo = getenv("HOS_PGO");
    const int gamescope = hos_crash_under_gamescope(getenv("GAMESCOPE_WAYLAND_DISPLAY"), getenv("XDG_CURRENT_DESKTOP"));
    if (!hos_crash_watch_wanted(argc, argv, env_on("HOS_HEADLESS"), off && env_value_off(off), pgo && *pgo, gamescope, run) || !dir) {
        if (gamescope && run) hos_log("crash window: off under gamescope (the Steam Deck's Game Mode): a crash is in bug-reports");
        return;
    }
    const char *log_arg = log && log[0] ? log : "-";
    char ledger[PATH_MAX_UTF8], pending[PATH_MAX_UTF8];
    if (ledger_path(dir, ledger, sizeof ledger) && pending_of(dir, pending, sizeof pending)) {
        char *shown = read_text(ledger, 64 << 10, NULL);
        if (!shown) {
            ledger_add(dir, NULL, 1);   /* (the first start with this window: what was pending before is not news) */
        } else {
            Newest pick = {shown, (int64_t)time(NULL), ""};
            each_name(pending, newest_unshown, &pick);
            if (pick.name[0] && ledger_add(dir, pick.name, 0)) {
                hos_log("crash window: %s was not shown after its run: shown now", pick.name);
                const char *args[] = {HOS_CRASH_SHOW_ARG, pick.name, dir, log_arg};
                spawn(args, 4, 1);
            }
            free(shown);
        }
    }
    char pid[24];
#ifdef _WIN32
    snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
#else
    snprintf(pid, sizeof pid, "%ld", (long)getpid());
#endif
    const char *args[] = {HOS_CRASH_WATCH_ARG, pid, run, dir, log_arg};
    hos_log(spawn(args, 5, 0) ? "crash window: watching this run (HOS_CRASH_WATCH=0: off)"
                              : "crash window: its helper could not be started: a crash is shown at the next start");
}

/* ---- HOS_CRASH_TEST: a crash on purpose, to see the window ---- */
static void crash_now(void) {
    void hos_log(const char *fmt, ...);
    void hos_log_flush(void);
    hos_log("crash test: crashing now (HOS_CRASH_TEST)");
    hos_log_flush();
    volatile uintptr_t where = 16;
    *(volatile int *)where = 1;
}
#ifdef _WIN32
static DWORD WINAPI crash_later(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    crash_now();
    return 0;
}
#else
static void *crash_later(void *p) {
    sleep_ms((int)(uintptr_t)p * 1000);
    crash_now();
    return NULL;
}
#endif
/* After the crash handlers are set up (hos_main.c reserve_guest): start, at once; a number, that many seconds later */
void hos_crash_test_arm(void) {
    const char *e = getenv("HOS_CRASH_TEST");
    if (!e || !*e) return;
    if (!strcmp(e, "start")) crash_now();
    const int seconds = atoi(e);
    if (seconds <= 0) return;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, crash_later, (void *)(uintptr_t)seconds, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (!pthread_create(&t, NULL, crash_later, (void *)(uintptr_t)seconds)) pthread_detach(t);
#endif
}
