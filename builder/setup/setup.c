/* HookshotBuilder-Setup.exe: the one file players download. It reads the latest release's hookshot-setup.txt, downloads
   the parts it names (the builder and its tools from this repository's release, llvm-mingw from its own official
   release), checks each one's SHA-256, unpacks them with Windows' own tar.exe into %LOCALAPPDATA%\HookshotBuilder\<tag>
   and starts the builder there. Parts already downloaded and checked are used again; a release already installed just
   starts. Files fetched by a program carry no "downloaded from the internet" mark, and the download itself holds no
   scripts, which is what antivirus programs' machine-learning guesses (Trojan:Script/...!ml) fired on.

   hookshot-setup.txt, one item a line:
       tag <tag>
       part <sha256> <bytes> <folder under the install> <top folder in the zip to rename, or -> <new name, or -> <url>
       run <the builder, relative to the install>
   Build: clang -O2 -mwindows -municode setup.c setup_rc.o -lwinhttp -lbcrypt -lcomctl32 -lshell32 -lole32 -o HookshotBuilder-Setup.exe */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "portable_home.h"

#define WM_STATUS (WM_APP + 1)
#define WM_DONE (WM_APP + 2)
#define MAX_PARTS 16

static HWND g_win, g_text, g_bar;
static wchar_t g_status[512];
static volatile LONG g_percent = -1;
static DWORD g_http_status;   /* (the last answer: 0 when the server was not reached) */
static wchar_t g_cmdline[2048];
static HbHome g_home;
static int g_home_ok;
/* (RecompLauncher) --cli <make_game.py's options>: no window, the builder's lines on this process's stdout and stderr, its exit code
   as ours; --play <game folder>: the game itself, waited for. Neither is used by the builder's own start, which is unchanged. */
static int g_headless;
static DWORD g_exit;

static void say_error(const wchar_t *msg) {
    char u[2048];
    const int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, u, (int)sizeof u - 8, NULL, NULL);
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD wrote;
    if (n > 0 && h && h != INVALID_HANDLE_VALUE) WriteFile(h, "error ", 6, &wrote, NULL), WriteFile(h, u, n - 1, &wrote, NULL), WriteFile(h, "\n", 1, &wrote, NULL);
}

typedef struct {
    char sha[65];
    unsigned long long size;
    char dest[260], from[260], to[260], url[1024];
} Part;
/* the next non-empty line of a text, cut in place (its \r or \n made 0); *next moves past it. NULL at the end
   (strtok's work without its hidden state) */
static char *next_line(char **next) {
    char *p = *next;
    while (*p == '\r' || *p == '\n') p++;
    if (!*p) return *next = p, (char *)NULL;
    char *line = p;
    while (*p && *p != '\r' && *p != '\n') p++;
    if (*p) *p++ = 0;
    *next = p;
    return line;
}
/* a name from the setup list (a tag, or with path set a relative path with / or \ between its parts): letters, digits
   and . _ + - only, no ".." part, no drive, no root */
static int list_name_ok(const char *s, int path) {
    if (!*s || *s == '/' || *s == '\\') return 0;
    for (const char *q = s; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || strchr("._+-", *q) ||
              (path && (*q == '/' || *q == '\\'))))
            return 0;
    for (const char *q = s; *q;) {   /* (each part: not "." or "..") */
        const size_t n = path ? strcspn(q, "/\\") : strlen(q);
        if ((n == 1 && q[0] == '.') || (n == 2 && q[0] == '.' && q[1] == '.') || !n) return 0;
        q += n;
        if (*q) q++;
    }
    return 1;
}

static void status(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vswprintf(g_status, 512, fmt, ap);
    va_end(ap);
    PostMessageW(g_win, WM_STATUS, 0, 0);
}

static void fail(const wchar_t *fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);
    if (g_headless) say_error(msg), ExitProcess(1);
    MessageBoxW(g_win, msg, L"Hookshot Builder setup", MB_OK | MB_ICONERROR);
    PostMessageW(g_win, WM_DONE, 1, 0);
    ExitThread(1);
}

/* A portable root can be longer than AppData. Stop before a truncated derived path reaches a file operation. */
static int home_path(wchar_t *out, const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vswprintf(out, MAX_PATH, format, args);
    va_end(args);
    if (g_home.portable && (n < 0 || n >= MAX_PATH))
        fail(L"A path in this portable installation is too long for the Windows setup. Use a shorter Builder home.");
    return n;
}

/* ---- HTTPS GET into a sink, following redirects (GitHub's release links go to its download host) ---- */
typedef int (*Sink)(void *user, const void *data, DWORD n);

static int http_get(const char *url8, Sink sink, void *user, unsigned long long expect, const wchar_t *accept) {
    wchar_t url[1024];
    MultiByteToWideChar(CP_UTF8, 0, url8, -1, url, 1024);
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[1024];
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    HINTERNET s = WinHttpOpen(L"HookshotBuilder-Setup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) s = WinHttpOpen(L"HookshotBuilder-Setup/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) return 0;
    /* (a stalled download fails after a minute without data, with the message, instead of waiting forever: a GitHub 504 hour) */
    WinHttpSetTimeouts(s, 30000, 30000, 30000, 60000);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;   /* (a redirect never drops to clear text) */
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    LPCWSTR types[2] = {accept, NULL};
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, accept ? types : WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    int ok = 0;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &len, NULL);
        g_http_status = code;
        if (code == 200) {
            static char buf[1 << 16];
            unsigned long long got = 0;
            ok = 1;
            for (;;) {
                DWORD n = 0;
                if (!WinHttpReadData(r, buf, sizeof buf, &n)) { ok = 0; break; }
                if (!n) break;
                if (!sink(user, buf, n)) { ok = 0; break; }
                got += n;
                if (expect) InterlockedExchange(&g_percent, (LONG)(got * 100 / expect)), PostMessageW(g_win, WM_STATUS, 0, 0);
            }
            if (expect && got != expect) ok = 0;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

typedef struct { char *p; size_t n, cap; } Mem;
static int mem_sink(void *u, const void *d, DWORD n) {
    Mem *m = (Mem *)u;
    if (m->n + n + 1 > m->cap) {
        m->cap = (m->n + n + 1) * 2;
        m->p = (char *)realloc(m->p, m->cap);
        if (!m->p) return 0;
    }
    memcpy(m->p + m->n, d, n);
    m->n += n;
    m->p[m->n] = 0;
    return 1;
}

typedef struct { HANDLE f; BCRYPT_HASH_HANDLE h; } FileSink;
static int file_sink(void *u, const void *d, DWORD n) {
    FileSink *s = (FileSink *)u;
    DWORD w = 0;
    if (!WriteFile(s->f, d, n, &w, NULL) || w != n) return 0;
    return BCryptHashData(s->h, (PUCHAR)d, n, 0) == 0;
}

static void hex(const unsigned char *d, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* the SHA-256 of a file already on disk (a part downloaded before) */
static int file_sha256(const wchar_t *path, char out[65]) {
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    int ok = f != INVALID_HANDLE_VALUE;
    static unsigned char buf[1 << 20];
    DWORD n;
    while (ok && ReadFile(f, buf, sizeof buf, &n, NULL) && n) BCryptHashData(h, buf, n, 0);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    unsigned char d[32];
    BCryptFinishHash(h, d, 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    if (ok) hex(d, 32, out);
    return ok;
}

typedef struct {
    DWORD code, start_error, status_error, capture_error;
    char error[512];
} RunResult;

static RunResult run_wait(wchar_t *cmd, const wchar_t *dir) {
    RunResult result = {0};
    HANDLE rd = NULL, wr = NULL, null = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW, si.wShowWindow = SW_HIDE;
    if (CreatePipe(&rd, &wr, &sa, 0)) {
        if (SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0))
            null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        if (null != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = si.hStdOutput = null;   /* (setup has no console: supply valid inherited handles) */
            si.hStdError = wr;
        } else {
            result.capture_error = GetLastError();
            CloseHandle(rd), CloseHandle(wr);
            rd = wr = NULL;
        }
    } else result.capture_error = GetLastError();
    /* Capturing a diagnostic is optional: if pipe setup fails, keep the original child-process route. */
    if (!CreateProcessW(NULL, cmd, NULL, NULL, wr != NULL, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        result.start_error = GetLastError();
        if (rd) CloseHandle(rd);
        if (wr) CloseHandle(wr);
        if (null != INVALID_HANDLE_VALUE) CloseHandle(null);
        return result;
    }
    if (wr) CloseHandle(wr);   /* (otherwise EOF never arrives on the parent's reader) */
    if (null != INVALID_HANDLE_VALUE) CloseHandle(null);
    if (rd) {
        char buf[512];
        DWORD n;
        size_t kept = 0;
        while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
            size_t copy = n < sizeof result.error - 1 - kept ? n : sizeof result.error - 1 - kept;
            memcpy(result.error + kept, buf, copy);
            kept += copy;   /* (keep draining after this buffer fills, so a noisy child cannot block) */
        }
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &result.code)) result.status_error = GetLastError();
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return result;
}

static void mkdirs(const wchar_t *path) {
    wchar_t p[MAX_PATH];
    wcsncpy(p, path, MAX_PATH - 1);
    p[MAX_PATH - 1] = 0;
    for (wchar_t *q = p + 3; *q; q++)
        if (*q == L'\\') *q = 0, CreateDirectoryW(p, NULL), *q = L'\\';
    CreateDirectoryW(p, NULL);
}

/* --portable is an initial setup option, before the app/CLI arguments. Keep the remainder byte-for-byte. */
static int portable_prefix(wchar_t *command) {
    const wchar_t *p = command, *end = NULL;
    while (*p == L' ' || *p == L'\t') p++;
    if (!wcsncmp(p, L"--portable", 10) && (!p[10] || p[10] == L' ' || p[10] == L'\t')) end = p + 10;
    else if (!wcsncmp(p, L"\"--portable\"", 12) && (!p[12] || p[12] == L' ' || p[12] == L'\t')) end = p + 12;
    if (!end) return 0;
    while (*end == L' ' || *end == L'\t') end++;
    wmemmove(command, end, wcslen(end) + 1);
    return 1;
}

static int init_home(int portable) {
    wchar_t self[MAX_PATH], requested[MAX_PATH] = L"", standard[MAX_PATH] = L"", profile[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH || GetEnvironmentVariableW(L"HOOKSHOT_SETUP_BASE", requested, MAX_PATH) >= MAX_PATH) return 0;
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, profile) == S_OK)
        hb_home_join(standard, MAX_PATH, profile, L"HookshotBuilder");   /* an explicit/portable home needs no AppData */
    const wchar_t *name = wcsrchr(self, L'\\');
    name = name ? name + 1 : self;
    const int game = !_wcsicmp(name, L"hookshot.exe");
    if (portable) {
        if (*requested) {
            if (!hb_home_full(g_home.root, requested)) return 0;
        } else {
            wchar_t parent[MAX_PATH];
            if (!hb_home_full(parent, self) || !hb_home_parent(parent)) return 0;
            HbHome existing;
            if (!hb_home_discover(&existing, NULL, self, parent, game)) return 0;
            if (existing.portable) wcscpy(g_home.root, existing.root);
            else if (!hb_home_join(g_home.root, MAX_PATH, parent, L"HookshotBuilder")) return 0;
        }
        if (hb_home_volume(g_home.root)) return 0;
        mkdirs(g_home.root);
        if (!hb_home_mark(g_home.root)) return 0;
        g_home.portable = 1;
    } else if (!hb_home_discover(&g_home, requested, self, standard, game)) return 0;
    wchar_t inherited[3] = L"";
    if (GetEnvironmentVariableW(L"HOOKSHOT_BUILDER_PORTABLE", inherited, 3) == 1 &&
        inherited[0] == L'1' && !g_home.portable) return 0;
    /* Only this setup and the children it starts: no user/system environment or registry change. */
    return SetEnvironmentVariableW(L"HOOKSHOT_SETUP_BASE", g_home.root) &&
           SetEnvironmentVariableW(L"HOOKSHOT_BUILDER_PORTABLE", g_home.portable ? L"1" : NULL);
}

/* a folder and everything in it (a version folder, a half-unpacked one) */
static void rmtree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    home_path(pat, L"%ls\\*", dir);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            home_path(p, L"%ls\\%ls", dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) rmtree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p);   /* (a link: never what it points to) */
            else SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL), DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void read_small(const wchar_t *path, char *out, int n) {
    out[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    ReadFile(f, out, (DWORD)n - 1, &got, NULL);
    CloseHandle(f);
    out[got] = 0;
    for (char *q = out; *q; q++)
        if (*q == '\r' || *q == '\n') { *q = 0; break; }
}

static void write_small(const wchar_t *path, const char *text) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(f);
}

/* A part that unpacks into a folder of its own (the tools the builder ships: Python, emsdk, the CPM packages, third_party,
   wgpu-native, llvm-mingw): that folder, relative to a version's folder, in rel (0: the part has none, e.g. the builder's own code).
   Named from its download (HookshotBuilder-<name>-<sha>.zip) or, for llvm-mingw, from its dest and rename. */
static int part_folder(const Part *p, wchar_t *rel, int n) {
    if (strcmp(p->to, "-") && strcmp(p->dest, ".")) {   /* (llvm-mingw: dest HookshotBuilder/tools, renamed to llvm-mingw) */
        swprintf(rel, n, L"%hs\\%hs", p->dest, p->to);
    } else {
        const char *b = strrchr(p->url, '/');
        b = b ? b + 1 : p->url;
        static const char *const own[] = {"python", "emsdk", "cpm", "third_party", "wgpu-native"};
        int found = 0;
        for (int i = 0; i < 5 && !found; i++) {
            char want[64];
            snprintf(want, sizeof want, "HookshotBuilder-%s-", own[i]);
            if (!strncmp(b, want, strlen(want))) swprintf(rel, n, L"HookshotBuilder\\tools\\%hs", own[i]), found = 1;
        }
        if (!found) return 0;
    }
    for (wchar_t *q = rel; *q; q++) if (*q == L'/') *q = L'\\';
    return 1;
}

/* (players asked for less in %LOCALAPPDATA%: the downloaded zips are not kept beside the unpacked version any more. So that an update
   still downloads only what changed, a part whose checksum the installed version lists in its parts.txt (and whose folder it has)
   is moved over from it instead of downloaded: that version is deleted after the install anyway) */
/* Portable updates keep the old installation usable until publication; copied parts belong only to staging. */
static int copy_part_tree(const wchar_t *from, const wchar_t *to) {
    const DWORD attributes = GetFileAttributesW(from);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return CopyFileW(from, to, TRUE) != 0;
    if (!CreateDirectoryW(to, NULL)) return 0;
    wchar_t pattern[MAX_PATH];
    if (!hb_home_join(pattern, MAX_PATH, from, L"*")) return 0;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    int ok = 1;
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        wchar_t source[MAX_PATH], target[MAX_PATH];
        if (!hb_home_join(source, MAX_PATH, from, entry.cFileName) ||
            !hb_home_join(target, MAX_PATH, to, entry.cFileName) || !copy_part_tree(source, target)) { ok = 0; break; }
    } while (FindNextFileW(find, &entry));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
    FindClose(find);
    return ok;
}

static int publish_install(const wchar_t *staged, const wchar_t *final, int portable) {
    if (!portable || GetFileAttributesW(final) == INVALID_FILE_ATTRIBUTES) return MoveFileW(staged, final) != 0;
    wchar_t backup[MAX_PATH];
    const int n = home_path(backup, L"%ls.rollback-%lu-%llu", final, GetCurrentProcessId(), GetTickCount64());
    if (n < 0 || n >= MAX_PATH || !MoveFileW(final, backup)) return 0;
    if (!MoveFileW(staged, final)) {
        const DWORD error = GetLastError();
        /* If restoration is prevented externally, the complete old installation remains in .rollback-* for recovery. */
        MoveFileW(backup, final);
        SetLastError(error);
        return 0;
    }
    rmtree(backup);
    return 1;
}

static int reuse_part(const wchar_t *versions, const char *tag, const Part *p, const wchar_t *install) {
    wchar_t rel[MAX_PATH], cur[MAX_PATH], list[MAX_PATH], from[MAX_PATH], to[MAX_PATH], parent[MAX_PATH];
    char was[128];
    if (!part_folder(p, rel, MAX_PATH)) return 0;
    home_path(cur, L"%ls\\current.txt", versions);
    read_small(cur, was, sizeof was);
    if (!was[0] || !strcmp(was, tag)) return 0;
    home_path(list, L"%ls\\%hs\\parts.txt", versions, was);
    HANDLE f = CreateFileW(list, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char buf[8192];
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    if (!strstr(buf, p->sha)) return 0;
    home_path(from, L"%ls\\%hs\\%ls", versions, was, rel);
    home_path(to, L"%ls\\%ls", install, rel);
    if (GetFileAttributesW(from) == INVALID_FILE_ATTRIBUTES) return 0;
    wcscpy(parent, to);
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash) *slash = 0, mkdirs(parent);
    if (!g_home.portable) return MoveFileW(from, to) != 0;
    if (copy_part_tree(from, to)) return 1;
    rmtree(to);   /* only this attempt's derived part destination, never the source */
    return 0;
}

/* The checksums of the parts this version holds in folders of their own (reuse_part reads them at the next update) */
static void write_parts(const wchar_t *install, const Part *parts, int nparts) {
    wchar_t list[MAX_PATH], rel[MAX_PATH];
    home_path(list, L"%ls\\parts.txt", install);
    HANDLE f = CreateFileW(list, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    for (int i = 0; i < nparts; i++) {
        if (!part_folder(&parts[i], rel, MAX_PATH)) continue;
        char line[128];
        const int n = snprintf(line, sizeof line, "%s\n", parts[i].sha);
        DWORD put;
        WriteFile(f, line, (DWORD)n, &put, NULL);
    }
    CloseHandle(f);
}

/* (as NX2navi's launcher keeps them: the version just installed and the one before it stay, older versions go, and so do downloads
   the list no longer names; a part unchanged since the last release keeps its checksum and stays cached) */
static void prune(const wchar_t *versions, const wchar_t *downloads, const char *tag, const Part *parts, int nparts) {
    wchar_t cur[MAX_PATH], prev[MAX_PATH], pat[MAX_PATH], p[MAX_PATH];
    char was[128], before[128];
    home_path(cur, L"%ls\\current.txt", versions);
    home_path(prev, L"%ls\\previous.txt", versions);
    read_small(cur, was, sizeof was);
    if (strcmp(was, tag)) {
        if (was[0]) write_small(prev, was);
        write_small(cur, tag);
    }
    read_small(prev, before, sizeof before);
    WIN32_FIND_DATAW fd;
    home_path(pat, L"%ls\\*", versions);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            char name[260];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
            /* (issue 611: only the version installed stays; the one before was 1.3 GB nobody rolled back to. A folder still in use
               (the builder that started this, closing; a build it left) cannot be renamed: it stays, and goes at a later run) */
            (void)before;
            if (!strcmp(name, tag)) continue;
            home_path(p, L"%ls\\%ls", versions, fd.cFileName);
            wchar_t gone[MAX_PATH];
            home_path(gone, L"%ls.gone", p);
            if (wcslen(fd.cFileName) > 5 && !wcscmp(fd.cFileName + wcslen(fd.cFileName) - 5, L".gone")) rmtree(p);
            else if (MoveFileW(p, gone)) rmtree(gone);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    home_path(pat, L"%ls\\*.zip", downloads);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char name[260];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
            int keep = 0;
            for (int i = 0; i < nparts; i++)
                if (!strncmp(name, parts[i].sha, 64)) keep = 1;
            if (!keep) home_path(p, L"%ls\\%ls", downloads, fd.cFileName), DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void w(const char *s8, wchar_t *out, int n) { MultiByteToWideChar(CP_UTF8, 0, s8, -1, out, n); }

/* (HOOKSHOT_SETUP_LIST=<url>: another list, for testing a release before it is the latest) */
/* ---- The GitHub API's link for a release file (after PR 546 by nicholasgower, issue 544). GitHub answered the release
   page's cached link (releases/latest/download/...) with 504 for hours on 2026-11-08 while its API still answered: the API
   gives the file's own asset link, which redirects to a fresh download. Used when the release link fails. ---- */
static int json_space(char c) { return c == 32 || c == 9 || c == 10 || c == 13; }

static int json_string_after(const char *p, const char *end, char *out, int n) {
    while (p < end && json_space(*p)) p++;
    if (p >= end || *p != ':') return 0;
    for (p++; p < end && json_space(*p); p++) {}
    if (p >= end || *p != '"') return 0;
    int i = 0;
    for (p++; p < end && *p != '"'; p++) {
        if (*p == '\\' && p + 1 < end) p++;
        if (i + 1 < n) out[i++] = *p;
    }
    if (p >= end) return 0;
    out[i] = 0;
    return 1;
}

/* The API link of the asset called name in a release's JSON (in an asset object its own "url" comes before its "name") */
static int asset_from_json(const char *json, size_t len, const char *name, char *out, int n) {
    const char *end = json + len, *at = NULL;
    char v[600];
    for (const char *p = json; (p = strstr(p, "\"name\"")) && p < end; p += 6)
        if (json_string_after(p + 6, end, v, sizeof v) && !strcmp(v, name)) { at = p; break; }
    if (!at) return 0;
    const char *url = NULL;
    for (const char *p = json; p + 5 <= at; p++)
        if (!strncmp(p, "\"url\"", 5)) url = p;
    return url && json_string_after(url + 5, end, out, n) && !strncmp(out, "https://api.github.com/", 23);
}

/* repo "owner/name", tag NULL for the latest release */
static int api_asset_url(const char *repo, const char *tag, const char *name, char *out, int n) {
    char api[1024];
    if (tag) snprintf(api, sizeof api, "https://api.github.com/repos/%s/releases/tags/%s", repo, tag);
    else snprintf(api, sizeof api, "https://api.github.com/repos/%s/releases/latest", repo);
    Mem m = {0};
    const int found = http_get(api, mem_sink, &m, 0, L"application/vnd.github+json") && m.p && asset_from_json(m.p, m.n, name, out, n);
    free(m.p);
    return found;
}

/* https://github.com/<owner>/<repo>/releases/download/<tag>/<name> or .../releases/latest/download/<name> -> its API link */
static int api_link_for(const char *url, char *out, int n) {
    static const char pre[] = "https://github.com/";
    char repo[256], tag[256], name[260];
    if (strncmp(url, pre, sizeof pre - 1)) return 0;
    const char *p = url + sizeof pre - 1, *slash = strchr(p, '/');
    if (!slash || !(slash = strchr(slash + 1, '/'))) return 0;
    if ((size_t)(slash - p) >= sizeof repo) return 0;
    memcpy(repo, p, slash - p), repo[slash - p] = 0;
    const char *t;
    int latest = 0;
    if (!strncmp(slash, "/releases/latest/download/", 26)) latest = 1, t = slash + 26;
    else if (!strncmp(slash, "/releases/download/", 19)) {
        t = slash + 19;
        const char *e = strchr(t, '/');
        if (!e || (size_t)(e - t) >= sizeof tag) return 0;
        memcpy(tag, t, e - t), tag[e - t] = 0;
        t = e + 1;
    } else return 0;
    if (!*t || strlen(t) >= sizeof name || strpbrk(t, "/?#")) return 0;
    strcpy(name, t);
    return api_asset_url(repo, latest ? NULL : tag, name, out, n);
}

/* an answer worth trying again: none (no connection), a server error, a download cut off half way (200), a timeout, or the API's
   rate limit (403/429) */
static int retryable(DWORD status) {
    return !status || status >= 500 || status == 200 || status == 408 || status == 429 || status == 403;
}

/* (the release the builder asked for: --tag <t> in the arguments, "" when none) */
static void wanted_tag(char *out, size_t n) {
    out[0] = 0;
    const wchar_t *t = wcsstr(g_cmdline, L"--tag ");
    if (!t) return;
    t += 6;
    while (*t == L' ' || *t == L'"') t++;
    size_t k = 0;
    while (*t && *t != L' ' && *t != L'"' && k + 1 < n) {
        const wchar_t c = *t++;
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'.' || c == L'-' || c == L'_'))
            { out[0] = 0; return; }
        out[k++] = (char)c;
    }
    out[k] = 0;
}

/* (an update to a named release reads THAT release's list: the latest/download link can answer with the release before it for a
   while after a publish, and the setup then installed the old version again and started it with --fetched, which builds without
   checking: players pressed Update and stayed on v2.0.3, 2026-11-08) */
/* (a nightly's version, git describe --long's v2.1.0-rc.1-3-g1a2b3c4: its files are on the rolling nightly release) */
static int nightly_tag(const char *s) {
    size_t n = strlen(s), h;
    if (n > 6 && !strcmp(s + n - 6, "-dirty")) n -= 6;
    for (h = n; h && strchr("0123456789abcdef", s[h - 1]); h--) {}
    if (h == n || h < 3 || s[h - 1] != 'g' || s[h - 2] != '-') return 0;
    size_t d = h - 2;
    while (d && s[d - 1] >= '0' && s[d - 1] <= '9') d--;
    return d < h - 2 && d > 0 && s[d - 1] == '-';
}

/* (HOOKSHOT_SETUP_RELEASE, set when a release candidate's or the nightly's setup is built: started with no --tag, it installs
   the release it came from (v2.1.0-rc.1, or nightly: the newest nightly), not the latest release) */
#ifndef HOOKSHOT_SETUP_RELEASE
#define HOOKSHOT_SETUP_RELEASE ""
#endif

/* The shipping Builder is a directory-based, self-contained WPF app: an EXE alone cannot start it.
   Keep this startup contract in step with the publish format; this is not an integrity check of every assembly. */
static int builder_ready(const wchar_t *exe) {
    static const wchar_t *const required[] = {
        L"HookshotBuilder.dll", L"HookshotBuilder.deps.json", L"HookshotBuilder.runtimeconfig.json",
        L"hostfxr.dll", L"hostpolicy.dll", L"coreclr.dll", L"System.Private.CoreLib.dll",
        L"WindowsBase.dll", L"PresentationCore.dll", L"PresentationFramework.dll",
        L"PresentationNative_cor3.dll", L"wpfgfx_cor3.dll"
    };
    wchar_t path[MAX_PATH];
    const size_t n = wcslen(exe);
    if (n >= MAX_PATH) return 0;
    wcscpy(path, exe);
    wchar_t *name = wcsrchr(path, L'\\');
    if (!name) return 0;
    const size_t prefix = (size_t)(++name - path);
    for (size_t i = 0; i <= sizeof required / sizeof *required; i++) {
        if (i) {
            if (prefix + wcslen(required[i - 1]) >= MAX_PATH) return 0;
            wcscpy(name, required[i - 1]);
        }
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (!info.nFileSizeHigh && !info.nFileSizeLow)) return 0;
    }
    return 1;
}

static const char *manifest_url(void) {
    static char u[1024];
    const char *e = getenv("HOOKSHOT_SETUP_LIST");
    char tag[128];
    wanted_tag(tag, sizeof tag);
    if (!tag[0] && HOOKSHOT_SETUP_RELEASE[0] && strlen(HOOKSHOT_SETUP_RELEASE) < sizeof tag) strcpy(tag, HOOKSHOT_SETUP_RELEASE);
    if (e && *e) snprintf(u, sizeof u, "%s", e);
    else if (tag[0])
        snprintf(u, sizeof u, "https://github.com/NotRexed/Hookshot/releases/download/%s/hookshot-setup.txt", nightly_tag(tag) ? "nightly" : tag);
    else snprintf(u, sizeof u, "%s", "https://github.com/NotRexed/Hookshot/releases/latest/download/hookshot-setup.txt");
    return u;
}

static DWORD WINAPI work(LPVOID unused) {
    (void)unused;
    /* (--repair, from the builder's Repair button: every part checked again and the version unpacked anew, after the builder that
       started this has closed. It is not passed on) */
    int repair = 0;
    wchar_t *flag = wcsstr(g_cmdline, L"--repair");
    if (flag) {
        repair = 1;
        wmemmove(flag, flag + 8, wcslen(flag + 8) + 1);
        Sleep(2000);
    }
    wchar_t base[MAX_PATH], downloads[MAX_PATH];
    if (!g_home_ok || (g_home.portable && hb_home_marked(g_home.root) != 1)) fail(L"The Builder home is unavailable or its portable marker is invalid.");
    wcscpy(base, g_home.root);
    home_path(downloads, L"%ls\\downloads", base);
    mkdirs(downloads);

    status(L"Looking for the latest Hookshot Builder...");
    Mem man = {0};
    /* (tried again on a server error or no answer: GitHub's release downloads answered 504 for an hour, 2026-11-08, and every
       start failed at once) */
    /* (low-vram, issue 558: 5 tries in 41 s were not enough while GitHub's site and API both failed on and off for hours; 8 tries
       over ~3 minutes now, alternating the release link and the API's) */
    static const int waits[] = {3, 6, 12, 20, 30, 45, 60};
    enum { TRIES = 8 };
    int ok = 0;
    for (int attempt = 0; attempt < TRIES && !ok; attempt++) {
        if (attempt) {
            status(L"GitHub didn't answer (%lu). Trying again in %d s...", g_http_status, waits[attempt - 1]);
            Sleep(waits[attempt - 1] * 1000);
            status(L"Looking for the latest Hookshot Builder...");
        }
        free(man.p);
        man = (Mem){0};
        g_http_status = 0;
        ok = http_get(manifest_url(), mem_sink, &man, 0, NULL) && man.p;
        const DWORD first = g_http_status;
        char api[1024];
        if (!ok && (!first || first >= 500) && api_link_for(manifest_url(), api, sizeof api)) {   /* (the API's link instead) */
            free(man.p);
            man = (Mem){0};
            ok = http_get(api, mem_sink, &man, 0, L"application/octet-stream") && man.p;
            if (!ok) g_http_status = first;
        }
        if (!ok && !retryable(g_http_status)) break;   /* (404 and the like: trying again changes nothing) */
    }
    if (!ok)
        fail(g_http_status == 404
                 ? L"The latest Hookshot release has no setup files yet. Download HookshotBuilder.zip from the releases page for now: "
                   L"https://github.com/NotRexed/Hookshot/releases/latest"
             : g_http_status ? L"GitHub's download servers are failing right now (an error 500 or above, tried 8 times over 3 minutes). "
                               L"This is on GitHub's side: try again in a few minutes, or later today."
                             : L"Could not reach GitHub to get the Hookshot Builder. Check your internet connection and try again.");

    char tag[128] = "", run[260] = "";
    Part parts[MAX_PARTS];
    int nparts = 0;
    for (char *next = man.p, *line; (line = next_line(&next)) != NULL;) {
        if (!strncmp(line, "tag ", 4)) sscanf(line + 4, "%127s", tag);
        else if (!strncmp(line, "run ", 4)) sscanf(line + 4, "%259s", run);
        else if (!strncmp(line, "part ", 5) && nparts < MAX_PARTS) {
            Part *p = &parts[nparts];
            if (sscanf(line + 5, "%64s %llu %259s %259s %259s %1023s", p->sha, &p->size, p->dest, p->from, p->to, p->url) == 6) nparts++;
        }
    }
    if (!tag[0] || !run[0] || !nparts) fail(L"The release's setup list could not be read. Try again later.");
    {   /* (the list's names become paths under the install folder, a folder this removes and remakes: they stay inside it) */
        int bad = !list_name_ok(tag, 0) || !list_name_ok(run, 1);
        for (int i = 0; i < nparts && !bad; i++) {
            const Part *p = &parts[i];
            bad = strlen(p->sha) != 64 || strspn(p->sha, "0123456789abcdef") != 64 || _strnicmp(p->url, "https://", 8) ||
                  (strcmp(p->dest, ".") && !list_name_ok(p->dest, 1)) || (strcmp(p->from, "-") && !list_name_ok(p->from, 0)) ||
                  (strcmp(p->to, "-") && !list_name_ok(p->to, 0));
        }
        if (bad) fail(L"The release's setup list has a name this setup does not accept. Try again later.");
    }
    {
        char want[128];
        wanted_tag(want, sizeof want);
        if (want[0] && strcmp(want, tag)) {
            wchar_t ww[128], wt[128];
            MultiByteToWideChar(CP_UTF8, 0, want, -1, ww, 128), MultiByteToWideChar(CP_UTF8, 0, tag, -1, wt, 128);
            fail(L"GitHub answered with the setup list of %ls, not %ls (its download links can lag right after a release). Try the "
                 L"update again in a few minutes.", wt, ww);
        }
    }

    wchar_t install[MAX_PATH], marker[MAX_PATH], wtag[128], wrun[260], exe[MAX_PATH], versions[MAX_PATH], final_dir[MAX_PATH];
    w(tag, wtag, 128), w(run, wrun, 260);
    home_path(versions, L"%ls\\versions", base);
    home_path(final_dir, L"%ls\\%ls", versions, wtag);
    home_path(install, L"%ls\\%ls", versions, wtag);
    home_path(marker, L"%ls\\installed.txt", install);
    home_path(exe, L"%ls\\%ls", install, wrun);
    for (wchar_t *q = exe; *q; q++) if (*q == L'/') *q = L'\\';

    if (repair || GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES || !builder_ready(exe)) {
        /* (unpacked into <tag>.part and renamed once complete: a setup stopped half way never leaves a version that looks installed) */
        home_path(install, L"%ls\\%ls.part", versions, wtag);
        rmtree(install);
        if (!g_home.portable) rmtree(final_dir);
        mkdirs(install);
        for (int i = 0; i < nparts; i++) {
            Part *p = &parts[i];
            wchar_t zip[MAX_PATH], wsha[65];
            w(p->sha, wsha, 65);
            home_path(zip, L"%ls\\%ls.zip", downloads, wsha);
            if (reuse_part(versions, tag, p, install)) {   /* (unchanged since the installed version: its folder moved over) */
                status(L"Part %d of %d unchanged: kept from the installed version", i + 1, nparts);
                continue;
            }
            char have[65] = "";
            if (!(GetFileAttributesW(zip) != INVALID_FILE_ATTRIBUTES && file_sha256(zip, have) && !strcmp(have, p->sha))) {
                wchar_t part[MAX_PATH];
                home_path(part, L"%ls.part", zip);
                int got = 0;
                for (int attempt = 0; attempt < TRIES && !got; attempt++) {   /* (a server error or a dropped connection: again) */
                    if (attempt) {
                        status(L"Part %d of %d: GitHub didn't answer (%lu). Trying again in %d s...", i + 1, nparts, g_http_status,
                               waits[attempt - 1]);
                        Sleep(waits[attempt - 1] * 1000);
                    }
                    status(L"Downloading part %d of %d (%llu MB)...", i + 1, nparts, p->size >> 20);
                    FileSink fs;
                    BCRYPT_ALG_HANDLE a;
                    fs.f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
                    if (fs.f == INVALID_HANDLE_VALUE) fail(L"Could not write to %ls.", downloads);
                    BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0);
                    BCryptCreateHash(a, &fs.h, NULL, 0, NULL, 0, 0);
                    g_http_status = 0;
                    /* (after a failed try the API's link to the same file, every other try) */
                    char api[1024];
                    if (attempt % 2 == 1 && api_link_for(p->url, api, sizeof api))
                        got = http_get(api, file_sink, &fs, p->size, L"application/octet-stream");
                    else
                        got = http_get(p->url, file_sink, &fs, p->size, NULL);
                    CloseHandle(fs.f);
                    unsigned char d[32];
                    BCryptFinishHash(fs.h, d, 32, 0);
                    BCryptDestroyHash(fs.h);
                    BCryptCloseAlgorithmProvider(a, 0);
                    InterlockedExchange(&g_percent, -1);
                    hex(d, 32, have);
                    /* (a download cut off half way answered 200 and stopped the tries at once: only a definite no stops them) */
                    if (!got && !retryable(g_http_status)) break;
                }
                if (!got) DeleteFileW(part), fail(retryable(g_http_status)
                    ? L"Downloading part %d of %d failed: GitHub's download servers are failing right now (tried 8 times over 3 "
                      L"minutes). This is on GitHub's side. The parts already downloaded are kept: run HookshotBuilder.exe again in a "
                      L"few minutes and it carries on from here."
                    : L"Downloading part %d of %d failed. Check your connection and free space, then try again.", i + 1, nparts);
                if (strcmp(have, p->sha)) DeleteFileW(part), fail(L"Part %d of %d did not download correctly (its checksum differs). Try again.", i + 1, nparts);
                /* (the checked file or nothing: an old <sha>.zip that could not be removed is never unpacked instead) */
                if (!MoveFileExW(part, zip, MOVEFILE_REPLACE_EXISTING)) {
                    const DWORD e = GetLastError();
                    DeleteFileW(part);
                    fail(L"Could not place part %d of %d in %ls (Windows error %lu).", i + 1, nparts, downloads, e);
                }
            }
            status(L"Unpacking part %d of %d...", i + 1, nparts);
            wchar_t dest[MAX_PATH], wdest[260], tarcmd[2 * MAX_PATH + 64], sys[MAX_PATH];
            w(p->dest, wdest, 260);
            if (!strcmp(p->dest, ".")) wcscpy(dest, install);
            else home_path(dest, L"%ls\\%ls", install, wdest);
            for (wchar_t *q = dest; *q; q++) if (*q == L'/') *q = L'\\';
            mkdirs(dest);
            GetSystemDirectoryW(sys, MAX_PATH);
            swprintf(tarcmd, 2 * MAX_PATH + 64, L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, zip, dest);
            RunResult tar = run_wait(tarcmd, dest);
            if (tar.start_error == ERROR_FILE_NOT_FOUND)
                fail(L"Windows' tar.exe was not found (Windows error 2).\n"
                     L"Open Command Prompt as administrator and run:\n"
                     L"DISM /Online /Cleanup-Image /RestoreHealth\n"
                     L"After DISM succeeds, run:\nsfc /scannow\n"
                     L"Then restart Windows and run setup again.\n"
                     L"Archive: %ls\nDestination: %ls", zip, dest);
            if (tar.start_error) fail(L"Could not start Windows' tar.exe (Windows error %lu).\nArchive: %ls\nDestination: %ls", tar.start_error, zip, dest);
            if (tar.status_error) fail(L"Could not read Windows' tar.exe exit status (Windows error %lu).\nArchive: %ls\nDestination: %ls", tar.status_error, zip, dest);
            if (tar.code) {
                wchar_t detail[512] = L"";
                if (tar.capture_error) swprintf(detail, 512, L"Could not capture stderr (Windows error %lu).", tar.capture_error);
                /* Best-effort UTF-8: tar may use an ANSI/OEM code page, or the byte cap may split a sequence. */
                else MultiByteToWideChar(CP_UTF8, 0, tar.error, -1, detail, 512);
                wchar_t heading[1024] = L"";
                swprintf(heading, 1024, L"Unpacking part %d of %d failed: Windows' tar.exe exited with code %lu.\nArchive: %ls\nDestination: %ls\n%ls:\n",
                         i + 1, nparts, tar.code, zip, dest,
                         tar.capture_error ? L"Stderr unavailable" : L"Tar stderr excerpt (up to 511 bytes captured)");
                heading[1023] = 0;
                /* Keep both paths; the displayed excerpt uses only the space left in fail()'s 1024-wide message. */
                fail(L"%ls%.*ls", heading, (int)(1023 - wcslen(heading)), detail);
            }
            if (strcmp(p->from, "-") && strcmp(p->to, "-")) {   /* (llvm-mingw-<version>-ucrt-x86_64 -> llvm-mingw) */
                wchar_t from[MAX_PATH], to[MAX_PATH], wf[260], wt[260];
                w(p->from, wf, 260), w(p->to, wt, 260);
                home_path(from, L"%ls\\%ls", dest, wf);
                home_path(to, L"%ls\\%ls", dest, wt);
                if (GetFileAttributesW(to) == INVALID_FILE_ATTRIBUTES && !MoveFileW(from, to))
                    fail(L"Could not set up part %d of %d (renaming %ls).", i + 1, nparts, wf);
            }
        }
        wchar_t staged_exe[MAX_PATH];
        home_path(staged_exe, L"%ls\\%ls", install, wrun);
        for (wchar_t *q = staged_exe; *q; q++) if (*q == L'/') *q = L'\\';
        if (!builder_ready(staged_exe))
            fail(L"The Builder or its self-contained .NET runtime is incomplete after unpacking (%ls). Your antivirus may have quarantined files from %ls. Check its quarantine/history, then run the setup again. No complete installation was published.", staged_exe, install);
        if (!publish_install(install, final_dir, g_home.portable))
            fail(L"Could not finish installing into %ls. A preserved repair backup, if restoration was blocked, is beside it as .rollback-*.", final_dir);
        wcscpy(install, final_dir);
        write_small(marker, tag);
        write_parts(install, parts, nparts);
    }
    {   /* (a version an older setup installed has no parts.txt: written now, so the next update can move its folders over) */
        wchar_t have_list[MAX_PATH];
        home_path(have_list, L"%ls\\parts.txt", final_dir);
        if (GetFileAttributesW(have_list) == INVALID_FILE_ATTRIBUTES) write_parts(final_dir, parts, nparts);
    }
    prune(versions, downloads, tag, parts, nparts);
    rmtree(downloads);   /* (installed: its zips are not kept; an update moves unchanged folders over instead, reuse_part) */

    status(L"Starting the Hookshot Builder...");
    wchar_t cmd[4096], dir[MAX_PATH];
    wcscpy(dir, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    swprintf(cmd, 4096, L"\"%ls\" %ls", exe, g_cmdline);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    BOOL inherit = FALSE;
    if (g_headless) {   /* (the builder writes to the same pipes we were given) */
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE), si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE), si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        if (si.hStdOutput && si.hStdOutput != INVALID_HANDLE_VALUE && si.hStdError && si.hStdError != INVALID_HANDLE_VALUE) {
            if (!si.hStdInput || si.hStdInput == INVALID_HANDLE_VALUE) si.hStdInput = NULL;
            SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            si.dwFlags = STARTF_USESTDHANDLES, inherit = TRUE;
        }
    }
    if (!CreateProcessW(NULL, cmd, NULL, NULL, inherit, 0, NULL, dir, &si, &pi)) fail(L"Could not start %ls.", exe);
    if (g_headless) WaitForSingleObject(pi.hProcess, INFINITE), GetExitCodeProcess(pi.hProcess, &g_exit);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    PostMessageW(g_win, WM_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_STATUS: {
        SetWindowTextW(g_text, g_status);
        const LONG p = g_percent;
        if (p < 0) SendMessageW(g_bar, PBM_SETMARQUEE, TRUE, 30);
        else SendMessageW(g_bar, PBM_SETMARQUEE, FALSE, 0), SendMessageW(g_bar, PBM_SETPOS, (WPARAM)p, 0);
        return 0;
    }
    case WM_DONE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* (launch mode) "Hookshot Launcher.exe" in a game folder (nx2/make_game.py puts this file there under that name): the installed
   builder opened as the game's launcher (--launch <that folder>: Play, and Update when a newer release is out), at once, with no
   window, download or network of its own. With no builder installed yet, the setup runs as usual and then opens the launcher. */
static int launch_installed(const wchar_t *args) {
    wchar_t base[MAX_PATH], cur[MAX_PATH], exe[MAX_PATH], dir[MAX_PATH], cmd[4096];
    if (!g_home_ok) return 0;
    wcscpy(base, g_home.root);
    char tag[128] = "";
    home_path(cur, L"%ls\\versions\\current.txt", base);
    read_small(cur, tag, sizeof tag);
    if (!tag[0] || tag[0] == '.' || !list_name_ok(tag, 0)) return 0;
    wchar_t wtag[128];
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, wtag, 128);
    home_path(dir, L"%ls\\versions\\%ls\\HookshotBuilder", base, wtag);
    home_path(exe, L"%ls\\HookshotBuilder.exe", dir);
    if (!builder_ready(exe)) return 0;
    swprintf(cmd, 4096, L"\"%ls\" %ls", exe, args);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) return 0;
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return 1;
}

/* (RecompLauncher's Play) --play <game folder>: bin\hookshot.exe there, started like the game folder's launcher starts it but without
   its update question, and waited for, so the launcher knows when the session ends. */
static int play_folder(const wchar_t *args) {
    wchar_t folder[MAX_PATH], cmd[MAX_PATH + 32];
    while (*args == L' ') args++;
    const int quoted = *args == L'"';
    size_t k = 0;
    for (args += quoted; *args && !(quoted ? *args == L'"' : *args == L' ') && k + 1 < MAX_PATH; ) folder[k++] = *args++;
    folder[k] = 0;
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\\bin\\hookshot.exe\"", folder);
    _wputenv_s(L"HOOKSHOT_NO_UPDATE_CHECK", L"1");
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!k || !CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, folder, &si, &pi)) {
        g_headless = 1;
        say_error(L"No built game to start. Build it first (the game folder needs bin\\hookshot.exe).");
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE), GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return (int)code;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev;
    /* (libraries loaded from here on come from System32 only, never from the folder the setup was downloaded to) */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    wcsncpy(g_cmdline, cmdline ? cmdline : L"", 2047);
    g_cmdline[2047] = 0;
    const int portable = portable_prefix(g_cmdline);
    {
        const wchar_t *c = g_cmdline;
        while (*c == L' ') c++;
        if (!wcsncmp(c, L"--play", 6) && (c[6] == L' ' || !c[6])) return play_folder(c + 6);
        if (!wcsncmp(c, L"--cli", 5) && (c[5] == L' ' || !c[5])) g_headless = 1, show = SW_HIDE;
    }
    g_home_ok = init_home(portable);
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        const wchar_t *name = wcsrchr(self, L'\\');
        name = name ? name + 1 : self;
        if (!_wcsicmp(name, L"hookshot.exe") && !wcsstr(g_cmdline, L"--")) {   /* (the game folder's launcher) */
            wchar_t *slash = wcsrchr(self, L'\\');
            if (slash) *slash = 0;
            swprintf(g_cmdline, 2047, L"--launch \"%ls\"", self);
            if (launch_installed(g_cmdline)) return 0;
            /* (no builder installed on this PC: the game itself, at once; nothing is downloaded to show a Play button) */
            wchar_t game[MAX_PATH], cmd[MAX_PATH + 4];
            home_path(game, L"%ls\\bin\\hookshot.exe", self);
            swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", game);
            STARTUPINFOW si = {sizeof si};
            PROCESS_INFORMATION pi;
            if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, self, &si, &pi)) {
                CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
                return 0;
            }
            MessageBoxW(NULL, L"The game (bin\\hookshot.exe) is missing from this folder. Build it again with the Hookshot Builder.", L"Hookshot",
                        MB_ICONERROR);
            return 1;
        }
        if (wcsstr(g_cmdline, L"--launch") && launch_installed(g_cmdline)) return 0;
    }
    if (!g_home_ok) {
        const wchar_t *why = L"The Builder home is unavailable or its portable marker is invalid. For a new portable install, put this setup in a writable folder and start it with --portable.";
        if (g_headless) say_error(why);
        else MessageBoxW(NULL, why, L"Hookshot Builder setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc, wc.hInstance = inst, wc.lpszClassName = L"HookshotSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    const int ww = 460, wh = 140;
    g_win = CreateWindowW(L"HookshotSetup", L"Hookshot Builder setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    g_text = CreateWindowW(L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE, 16, 16, ww - 48, 22, g_win, NULL, inst, NULL);
    g_bar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 16, 48, ww - 48, 20, g_win, NULL, inst, NULL);
    HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(g_text, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_bar, PBM_SETMARQUEE, TRUE, 30);
    ShowWindow(g_win, show);
    CloseHandle(CreateThread(NULL, 0, work, NULL, 0, NULL));
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) TranslateMessage(&m), DispatchMessageW(&m);
    return (int)g_exit;
}
