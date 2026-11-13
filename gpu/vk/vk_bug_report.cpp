// The Insert menu's Bug report: what is read and how it is written (vk_bug_report.h). No Vulkan and no menu here:
// tests/test_bug_report.py compiles this file alone.
#include "vk_bug_report.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <io.h>
#undef min
#undef max
#else
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#include <pwd.h>
#include <sys/utsname.h>
#include <unistd.h>
extern char** environ;
#endif

namespace mkwvk::bug_report {
namespace {

// (a log is copied whole up to 10 MB, GitHub taking files up to 25 MB; a longer one keeps its first MB, where the start, the
// device and the options are, and its last 9 MB, where the problem usually is: an hour of play logs about 10 MB)
constexpr uint64_t kLogCap = 10ull << 20, kLogHead = 1ull << 20;
constexpr const char* kIssues = "https://github.com/NotRexed/Hookshot/issues/new/choose";
constexpr const char* kForm = "https://github.com/NotRexed/Hookshot/issues/new?template=1-game-problem.yml";
// (the link's longest: a player who is not signed in is sent to GitHub's sign-in page with the whole address in it, encoded
// once more, and that page failed (HTTP 400 or 500, or the address dropped) from links of 3500 characters of escaped text on,
// signed in or not GitHub answers 414 from 8192; 2000 passes both, and every browser and chat program takes it)
constexpr size_t kLinkMax = 2000;

bool is_sep(char c) { return c == '/' || c == '\\'; }
/// (a character a file or folder name has: a name ends at any other)
bool name_char(char c) {
    const unsigned char u = (unsigned char)c;
    return std::isalnum(u) || c == '.' || c == '_' || c == '-' || u >= 0x80;
}
bool same_char(char a, char b, bool windows) {
    if (is_sep(a) && is_sep(b)) return windows || a == b;
    return windows ? std::tolower((unsigned char)a) == std::tolower((unsigned char)b) : a == b;
}
bool matches_at(const std::string& s, size_t at, const std::string& what, bool windows) {
    if (what.empty() || at + what.size() > s.size()) return false;
    for (size_t i = 0; i < what.size(); i++)
        if (!same_char(s[at + i], what[i], windows)) return false;
    return true;
}
std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}
std::string megabytes(uint64_t n) {
    char b[32];
    if (n < (1u << 20)) snprintf(b, sizeof b, "%llu KB", (unsigned long long)((n + 1023) >> 10));
    else snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    return b;
}
bool local_time(std::time_t t, std::tm* out) {
#ifdef _WIN32
    return localtime_s(out, &t) == 0;
#else
    return localtime_r(&t, out) != nullptr;
#endif
}
std::string when(std::time_t t, const char* format) {
    std::tm tm{};
    char b[64] = "";
    if (t && local_time(t, &tm)) strftime(b, sizeof b, format, &tm);
    return b;
}
bool exists(const std::string& path, std::time_t* written = nullptr) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path.c_str(), &st)) return false;
#else
    struct stat st;
    if (stat(path.c_str(), &st)) return false;
#endif
    if (written) *written = st.st_mtime;
    return true;
}
/// Makes a folder, or finds it made; why: the system's reason when it cannot be made
bool make_dir(const std::string& path, std::string* why) {
#ifdef _WIN32
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    const DWORD e = GetLastError();   // (CreateDirectoryA sets this, not errno)
    if (e == ERROR_ALREADY_EXISTS) return true;
    char text[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, text, sizeof text, nullptr);
    *why = "Windows error " + std::to_string(e) + (text[0] ? ": " + trimmed(text) : std::string());
    return false;
#else
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    *why = strerror(errno);
    return false;
#endif
}
uint64_t file_size(FILE* f) {
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    const int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    const int64_t n = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    return n > 0 ? (uint64_t)n : 0;
}
/// Whether two paths name one file (the log this run writes, and hookshot_log.txt in the game folder)
bool same_file(const std::string& a, const std::string& b, bool windows) {
    if (a.empty() || b.empty()) return false;
#ifndef _WIN32
    struct stat sa, sb;
    if (!windows && !stat(a.c_str(), &sa) && !stat(b.c_str(), &sb)) return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
    return a.size() == b.size() && matches_at(a, 0, b, windows);
}

// ---- the picture (Inputs::ask_picture: the renderer answers with picture_written once the PNG is written)
std::mutex pic_mu;
std::condition_variable pic_cv;
uint64_t pic_next = 0, pic_done = 0, pic_late = 0;   // (pic_late: the ticket of a picture the report stopped waiting for)
bool pic_ok = false;
std::string pic_late_path;

uint64_t picture_ticket() {
    std::lock_guard lk(pic_mu);
    return ++pic_next;
}
/// Whether the picture was written; one that has not come when the time is up is given up (picture_written removes it then)
bool wait_picture(uint64_t ticket, const std::string& path, int ms) {
    std::unique_lock lk(pic_mu);
    if (pic_cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return pic_done == ticket; })) return pic_ok;
    pic_late = ticket, pic_late_path = path;
    return false;
}

// ---- the report's state, for the menu
std::atomic<int> report_state{kIdle};
std::mutex report_mu;
std::string report_shown, report_why;

/// A log as it was copied: what the report says of it
struct LogFacts {
    bool copied = false, cut = false;
    uint64_t size = 0;
    std::time_t written = 0;
    std::string version, map, trouble;   // (its first line's version, the last map it names, its first sign of trouble)
};
void take_facts(const std::string& line, LogFacts& f) {
    size_t at;
    if (f.version.empty() && (at = line.find("] hookshot ")) != std::string::npos) f.version = trimmed(line.substr(at + 11));
    if ((at = line.find("] map: ")) != std::string::npos) f.map = trimmed(line.substr(at + 7));   // (host/hos_main.c, on Link's update)
    if (f.trouble.empty() && (line.find("FATAL:") != std::string::npos || line.find("CRASH:") != std::string::npos ||
                              line.find("device lost") != std::string::npos))
        f.trouble = trimmed(line).substr(0, 300);
}
/// Copies a log, its paths redacted, as much of it as there was when the copy began (the game goes on writing this run's).
/// Read by length, not as C strings: a log a power cut or a kill ended can hold NUL bytes, and they are copied as they are
LogFacts copy_log(const std::string& from, const std::string& to, const Inputs& in) {
    LogFacts f;
    FILE* src = fopen(from.c_str(), "rb");
    if (!src) return f;
    exists(from, &f.written);
    f.size = file_size(src);
    FILE* dst = fopen(to.c_str(), "wb");
    if (!dst) {
        fclose(src);
        return f;
    }
    f.cut = f.size > kLogCap;
    const uint64_t gap_from = f.cut ? kLogHead : f.size, gap_to = f.cut ? f.size - (kLogCap - kLogHead) : f.size;
    std::string line;
    std::vector<char> buf(1 << 16);
    uint64_t read = 0, start = 0;   // (bytes read; where the line being gathered starts in the log)
    bool said_gap = false;
    const auto put = [&] {   // (a whole line, its '\n' kept, or the last part of the log)
        const std::string clean = redact(line, in.home, in.user, in.windows);
        take_facts(clean, f);
        if (start < gap_from || start >= gap_to) {
            fwrite(clean.data(), 1, clean.size(), dst);
        } else if (!said_gap) {
            said_gap = true;
            fprintf(dst, "[bug report: %s of this log's middle left out here: the copy keeps its first and its last part]\n",
                    megabytes(gap_to - gap_from).c_str());
        }
        start += line.size();
        line.clear();
    };
    while (read < f.size) {
        const size_t n = fread(buf.data(), 1, (size_t)std::min<uint64_t>(buf.size(), f.size - read), src);
        if (!n) break;
        read += n;
        for (size_t i = 0; i < n;) {
            const char* nl = (const char*)memchr(buf.data() + i, '\n', n - i);
            const size_t end = nl ? (size_t)(nl - buf.data()) + 1 : n;
            line.append(buf.data() + i, end - i);
            i = end;
            if (nl || line.size() >= (1u << 20)) put();   // (a line, or a MB of one that has no end)
        }
    }
    if (!line.empty()) put();
    fclose(src);
    f.copied = fclose(dst) == 0;
    return f;
}

/// Whether a switch's value goes into the report. CONTRIBUTING.md lists hookshot_env.txt among the files never to share, so
/// only a value that cannot hold anything private is written: a number of at most 12 characters (60, -1, 1.5), a size
/// (1920x1080), or on/off, true/false, yes/no; and never for a name that may hold a secret, a place or a person (TOKEN, KEY,
/// ROOM, SECRET, PASS, SERVER or USER in it). Every other value is left out, paths and addresses with them
bool value_shown(const std::string& name, const std::string& value) {
    std::string upper = name, lower = value;
    for (char& ch : upper) ch = (char)std::toupper((unsigned char)ch);
    for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
    for (const char* word : {"TOKEN", "KEY", "ROOM", "SECRET", "PASS", "SERVER", "USER"})
        if (upper.find(word) != std::string::npos) return false;
    for (const char* word : {"", "on", "off", "true", "false", "yes", "no"})
        if (lower == word) return true;
    if (value.size() > 12) return false;
    const auto digits = [&](size_t& at) {
        const size_t from = at;
        while (at < value.size() && std::isdigit((unsigned char)value[at])) at++;
        return at > from;
    };
    const bool minus = value[0] == '-';
    size_t at = minus ? 1 : 0;
    if (!digits(at)) return false;
    if (at == value.size()) return true;   // (a whole number)
    const char mid = value[at++];
    return (mid == '.' || (mid == 'x' && !minus)) && digits(at) && at == value.size();   // (1.5, 1920x1080)
}
/// The switches set: the game's own (host/hos_main.c env_line_allowed's names) and what loads other code into it (the form
/// asks about software hooked in), a NAME=VALUE line each, the value as value_shown allows; names: the names alone
std::string switches(const std::vector<std::string>& env, std::string* names) {
    static const char* const prefixes[] = {"RX_", "HOS_", "HOOKSHOT_", "EDEN_", "NVN_", "SDL_"};
    static const char* const hooks[] = {"VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_DRIVER_FILES", "VK_ICD_FILENAMES",
                                        "ENABLE_VKBASALT", "MANGOHUD", "LD_PRELOAD"};
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& kv : env) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string name = kv.substr(0, eq), value = kv.substr(eq + 1);
        bool take = false;
        for (const char* p : prefixes) take |= name.compare(0, strlen(p), p) == 0;
        for (const char* h : hooks) take |= name == h;
        if (take) out.emplace_back(name, value_shown(name, value) ? value : "(value left out)");
    }
    std::sort(out.begin(), out.end());
    std::string text;
    names->clear();
    for (const auto& [name, value] : out) {
        text += name + "=" + value + "\n";
        *names += (names->empty() ? "" : ", ") + name;
    }
    return text;
}

std::string url_part(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else out += '%', out += hex[c >> 4], out += hex[c & 15];
    }
    return out;
}

}  // namespace

std::string redact(const std::string& line, const std::string& home, const std::string& user, bool windows) {
    std::string h = home;
    while (h.size() > 1 && is_sep(h.back())) h.pop_back();
    const bool with_home = h.size() > 1;
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        const size_t after_home = i + h.size(), after_user = i + user.size();
        // (the home folder as a whole: not inside a longer name, not the end of another path)
        if (with_home && (i == 0 || (!name_char(line[i - 1]) && !is_sep(line[i - 1]))) && matches_at(line, i, h, windows) &&
            (after_home >= line.size() || !name_char(line[after_home]))) {
            out += windows ? "%USERPROFILE%" : "~";
            i = after_home;
            continue;
        }
        // (the user's name as a folder of any path: /run/media/<user>, D:\Users\<user>, /var/home/<user>)
        if (!user.empty() && i > 0 && is_sep(line[i - 1]) && matches_at(line, i, user, windows) &&
            (after_user >= line.size() || !name_char(line[after_user]))) {
            out += "<user>";
            i = after_user;
            continue;
        }
        out += line[i++];
    }
    return out;
}

std::string system_text() {
    std::string cpu;
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    {   // (the processor's own name, the same on every system)
        unsigned r[12] = {};
#ifdef _WIN32
        int v[4];
        __cpuid(v, (int)0x80000000);
        const unsigned top = (unsigned)v[0];
        for (unsigned k = 0; k < 3 && top >= 0x80000004u; k++) {
            __cpuid(v, (int)(0x80000002u + k));
            for (int j = 0; j < 4; j++) r[k * 4 + j] = (unsigned)v[j];
        }
#else
        unsigned a, b, c, d;
        if (__get_cpuid(0x80000000u, &a, &b, &c, &d) && a >= 0x80000004u)
            for (unsigned k = 0; k < 3; k++) __get_cpuid(0x80000002u + k, &r[k * 4], &r[k * 4 + 1], &r[k * 4 + 2], &r[k * 4 + 3]);
#endif
        char name[49] = {};
        memcpy(name, r, 48);
        cpu = trimmed(name);
    }
#endif
    if (cpu.empty()) cpu = "an unknown CPU";
    if (const unsigned n = std::thread::hardware_concurrency()) cpu += " (" + std::to_string(n) + " threads)";
    uint64_t ram = 0;
    std::string os;
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) ram = m.ullTotalPhys;
    {   // (RtlGetVersion: GetVersionEx answers as the program's manifest allows)
        typedef LONG(WINAPI * GetVersionFn)(OSVERSIONINFOW*);
        typedef const char*(__cdecl * WineFn)(void);
        const HMODULE nt = GetModuleHandleA("ntdll.dll");
        const GetVersionFn get = nt ? (GetVersionFn)(void*)GetProcAddress(nt, "RtlGetVersion") : nullptr;
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        if (get && get(&v) == 0) {
            const bool eleven = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000;
            os = v.dwMajorVersion == 10 ? (eleven ? "Windows 11" : "Windows 10")
                                        : "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion);
            os += " (build " + std::to_string(v.dwBuildNumber) + ")";
        } else {
            os = "Windows";
        }
        if (const WineFn wine = nt ? (WineFn)(void*)GetProcAddress(nt, "wine_get_version") : nullptr) os += ", Wine " + std::string(wine());
    }
#else
    {
        const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page > 0) ram = (uint64_t)pages * (uint64_t)page;
    }
    for (const char* file : {"/etc/os-release", "/usr/lib/os-release"}) {
        FILE* f = fopen(file, "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                std::string v = trimmed(line + 12);
                if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'')) v = v.substr(1, v.size() - 2);
                os = v;
            }
        fclose(f);
        if (!os.empty()) break;
    }
    if (os.empty()) os = "Linux";
    struct utsname u;
    if (!uname(&u)) os += std::string(" (") + u.sysname + " " + u.release + ")";
    {   // (the desktop and its display protocol: what a window problem depends on; gamescope is a Steam Deck's Game Mode)
        const char* desktop = getenv("XDG_CURRENT_DESKTOP");
        const char* session = getenv("XDG_SESSION_TYPE");
        if (desktop && *desktop) os += std::string(", ") + desktop + (session && *session ? std::string(" (") + session + ")" : "");
    }
    if (getenv("APPIMAGE")) os += ", the AppImage";
#endif
    char gb[32] = "";
    if (ram) snprintf(gb, sizeof gb, "%llu GB", (unsigned long long)((ram + (1ull << 29)) >> 30));
    return cpu + ", " + (ram ? std::string(gb) + ", " : std::string()) + os;
}

void fill_here(Inputs& in) {
    in.system = system_text();
    in.now = std::time(nullptr);
    in.env.clear();
#ifdef _WIN32
    in.windows = true;
    if (char* block = GetEnvironmentStringsA()) {   // (NAME=VALUE\0 ... \0\0; the host's _putenv of hookshot_env.txt reaches it)
        for (const char* e = block; *e; e += strlen(e) + 1) in.env.push_back(e);
        FreeEnvironmentStringsA(block);
    }
    if (const char* h = getenv("USERPROFILE")) in.home = h;
    if (const char* u = getenv("USERNAME")) in.user = u;
    const HANDLE err = (HANDLE)_get_osfhandle(_fileno(stderr));   // (the game freopens stderr as hookshot_log.txt)
    char path[MAX_PATH * 3 + 16];
    if (err != INVALID_HANDLE_VALUE && GetFileType(err) == FILE_TYPE_DISK) {
        const DWORD n = GetFinalPathNameByHandleA(err, path, (DWORD)sizeof path, FILE_NAME_NORMALIZED);
        if (n && n < sizeof path) {
            in.current_log = path;
            if (!in.current_log.compare(0, 8, "\\\\?\\UNC\\")) in.current_log = "\\\\" + in.current_log.substr(8);
            else if (!in.current_log.compare(0, 4, "\\\\?\\")) in.current_log = in.current_log.substr(4);
        }
    }
#else
    in.windows = false;
    for (char** e = environ; e && *e; e++) in.env.push_back(*e);
    const passwd* pw = getpwuid(getuid());
    const char* h = getenv("HOME");
    const char* u = getenv("USER");
    in.home = h && *h ? h : pw && pw->pw_dir ? pw->pw_dir : "";
    in.user = u && *u ? u : pw && pw->pw_name ? pw->pw_name : "";
    struct stat st;
    char path[4096];
    if (!fstat(2, &st) && S_ISREG(st.st_mode)) {   // (a terminal or a pipe: the log is not in a file)
        const ssize_t n = readlink("/proc/self/fd/2", path, sizeof path - 1);
        if (n > 0) in.current_log.assign(path, (size_t)n);
    }
#endif
}

std::string write(const Inputs& in) {
    const char sep = in.windows ? '\\' : '/';
    const std::string stamp = when(in.now, "%Y-%m-%d_%H-%M-%S");
    std::string rel, dir, why;
    if (make_dir(in.game_dir + "bug-reports", &why)) {
        for (int k = 1; k < 100 && dir.empty() && why.empty(); k++) {
            const std::string r = std::string("bug-reports") + sep + stamp + (k > 1 ? "-" + std::to_string(k) : "");
            if (!exists(in.game_dir + r) && make_dir(in.game_dir + r, &why)) rel = r, dir = in.game_dir + r + sep;
        }
    }
    if (dir.empty()) {
        std::lock_guard lk(report_mu);
        report_why = "the folder bug-reports could not be made in the game folder (" +
                     (why.empty() ? std::string("99 reports made in this second already") : why) + ")";
        if (in.log) in.log(("menu: bug report not written: " + report_why).c_str());
        return "";
    }
    // (first: the frame on screen when the report was asked for)
    const uint64_t ticket = picture_ticket();
    const std::string picture_path = dir + "picture.png";
    const bool asked = in.ask_picture && in.ask_picture(picture_path.c_str(), ticket);

    // the logs: this run's, and the run before's (its own after a start from the icon, which moves the last one to .1)
    const std::string g0 = in.game_dir + "hookshot_log.txt", g1 = in.game_dir + "hookshot_log.1.txt";
    const std::string before = same_file(in.current_log, g0, in.windows) ? g1 : exists(g0) ? g0 : g1;
    LogFacts now, last;
    if (!in.current_log.empty()) now = copy_log(in.current_log, dir + "hookshot_log.txt", in);
    if (!same_file(before, in.current_log, in.windows)) last = copy_log(before, dir + "hookshot_log.1.txt", in);
    const bool picture = asked && wait_picture(ticket, picture_path, in.picture_wait_ms);

    // (the first line answers the form's one-line field, as the link fills it in; the second says where it comes from)
    std::string where;
    if (!now.map.empty()) where = now.map + "\n(The map this run's log names last: add what you were doing there.";
    else if (in.current_log.empty()) where = "Not known\n(This run's log is not in a file: the game was started from a terminal, or by a program that reads its output.";
    else where = "Not known\n(This run's log names no map yet: no save loaded since the start.";
    where += last.copied && !last.map.empty() ? " The run before was last in " + last.map + ".)" : std::string(")");
    std::string settings = in.options.empty() ? "Every Insert menu option is at its default.\n" : in.options;
    std::string names;
    const std::string sw = switches(in.env, &names);
    settings += sw.empty() ? "Switches set: none.\n"
                           : "Switches set (the environment, hookshot_env.txt; a value only when it is a number or on/off, and never for "
                             "a name with TOKEN, KEY, ROOM, SECRET, PASS, SERVER or USER in it):\n" + sw;
    std::string logs;
    if (now.copied) {
        logs += "hookshot_log.txt: this run's log as it was when the report was made, " + megabytes(now.size) + ".\n";
        if (now.cut) logs += "  Only its first and its last part are in the copy: it is longer than " + megabytes(kLogCap) + ".\n";
    } else {
        logs += in.current_log.empty() ? "This run's log is not here: it is not in a file (the game was started from a terminal, or by a "
                                         "program that reads its output).\n"
                                       : "This run's log could not be read.\n";
    }
    if (last.copied) {
        logs += "hookshot_log.1.txt: the run before's log";
        if (!last.version.empty() || last.written)
            logs += " (" + (last.version.empty() ? std::string() : "hookshot " + last.version) +
                    (!last.version.empty() && last.written ? ", " : "") +
                    (last.written ? "last written " + when(last.written, "%Y-%m-%d %H:%M") : std::string()) + ")";
        logs += ", " + megabytes(last.size) + ".\n";
        if (last.cut) logs += "  Only its first and its last part are in the copy: it is longer than " + megabytes(kLogCap) + ".\n";
    } else {
        logs += "No log of an earlier run was found in the game folder.\n";
    }
    if (!now.trouble.empty()) logs += "This run's first sign of trouble: " + now.trouble + "\n";
    if (!last.trouble.empty()) logs += "The run before's first sign of trouble: " + last.trouble + "\n";
    logs += in.windows ? "In both, the home folder is written %USERPROFILE% and the user's name <user>.\n"
                       : "In both, the home folder is written ~ and the user's name <user>.\n";
    std::string files = "report.txt (this file)";
    for (const auto& [here, name] : {std::pair{now.copied, "hookshot_log.txt"}, std::pair{last.copied, "hookshot_log.1.txt"},
                                     std::pair{picture, "picture.png"}})
        if (here) files += std::string(", ") + name;
    const std::string gpu = in.gpu.empty() ? "Not known: the game has no Vulkan device." : in.gpu;
    // (the link fills in the form's one-line answers and, while the whole link stays within kLinkMax, the options changed and
    // the switches' names: never a switch's value, which only report.txt has. When they do not fit, the options and how many
    // switches there are, else only the two counts)
    std::string link = std::string(kForm) + "&version=" + url_part(in.version) + (now.map.empty() ? "" : "&where=" + url_part(now.map)) +
                       "&gpu=" + url_part(gpu) + "&system=" + url_part(in.system);
    const auto count = [](const std::string& s) { return std::to_string(std::count(s.begin(), s.end(), '\n')); };
    const std::string options = in.options.empty() ? "Every Insert menu option is at its default.\n" : in.options;
    const std::string named = sw.empty() ? "Switches set: none.\n" : "Switches set (their values are in the attached report.txt): " + names + ".\n";
    const std::string listed = sw.empty() ? "" : "Switches set: " + count(sw) + ", listed in the attached report.txt.\n";
    bool link_short = false;
    for (const std::string& s : {options + named, options + listed,
                                 count(in.options) + " Insert menu options changed and " + count(sw) +
                                     " switches set, listed in the attached report.txt.\n"}) {
        const std::string part = "&settings=" + url_part(s);
        if (link.size() + part.size() <= kLinkMax) {
            link += part;
            break;
        }
        link_short = true;
    }

    std::string text;
    text += "Hookshot bug report, made by the Insert menu's Bug report on " + when(in.now, "%Y-%m-%d at %H:%M") + ".\n\n";
    text += std::string("To send it: open ") + kIssues + " and choose Game problem (or the link at the end of this file, which "
            "fills in the answers below), copy the answers below into the fields of the same names, say what happened, and drag "
            "the files of this folder, this one too, into the issue.\n\n";
    text += "### Hookshot version\n\n" + in.version + "\n\n";
    text += "### Where in the game\n\n" + where + "\n\n";
    text += "### Graphics card and driver version\n\n" + gpu + "\n\n";
    text += "### CPU, RAM and operating system\n\n" + in.system + "\n\n";
    text += "### Changes from the defaults\n\n" + settings + "\n";
    text += "### hookshot_log.txt\n\n" + logs + "\n";
    text += std::string("### Screenshots or video\n\n") +
            (picture ? "picture.png: the game's picture when the report was made, without the menu.\n\n"
                     : "No picture: the game showed no new frame while the report was made, or its picture could not be written.\n\n");
    text += "### What this folder holds\n\n" + files + ". Nothing else from this PC: no saves, no game files, not the options file "
            "itself.\n\nThe link with the answers filled in" +
            (link_short ? std::string(" (the changes from the defaults in short: the switches' names did not fit in a link)") : "") +
            ":\n" + link + "\n";
    FILE* f = fopen((dir + "report.txt").c_str(), "wb");
    const bool written = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f) fclose(f);
    if (!written) {
        std::lock_guard lk(report_mu);
        report_why = "report.txt could not be written in " + rel;
        if (in.log) in.log(("menu: bug report not written: " + report_why).c_str());
        return "";
    }
    if (in.log) {
        std::string line = "menu: bug report written to " + rel + ": report.txt";
        if (now.copied) line += ", hookshot_log.txt (" + megabytes(now.size) + ")";
        if (last.copied) line += ", hookshot_log.1.txt (" + megabytes(last.size) + ")";
        line += picture ? ", picture.png" : ", no picture";
        in.log(line.c_str());
    }
    return rel;
}

bool start(Inputs in) {
    int was = report_state.load();
    if (was == kSaving || !report_state.compare_exchange_strong(was, kSaving)) return false;
    std::thread([in = std::move(in)] {
        const std::string rel = write(in);
        {
            std::lock_guard lk(report_mu);
            report_shown = rel.empty() ? report_why : rel;
        }
        report_state.store(rel.empty() ? kFailed : kSaved);
    }).detach();
    return true;
}

State state() { return (State)report_state.load(); }

std::string shown() {
    std::lock_guard lk(report_mu);
    return report_shown;
}

void picture_written(uint64_t ticket, bool ok) {
    std::string late;
    {
        std::lock_guard lk(pic_mu);
        if (ticket == pic_late) late = std::move(pic_late_path), pic_late = 0;   // (the report went without it, and says so)
        else pic_done = ticket, pic_ok = ok;
    }
    if (!late.empty()) std::remove(late.c_str());
    pic_cv.notify_all();
}

}  // namespace mkwvk::bug_report
