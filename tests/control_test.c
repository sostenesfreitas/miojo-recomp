/* The control bridge (host/control.c, built with HOS_CONTROL_NO_GAME) against a fake game, over the real transport: a
   Unix socket on Linux, a named pipe on Windows. A thread stands in for the game (a controller read, then a present,
   every 2 ms; draws a frame as a case sets them), and the test is the client. tests/test_control.py builds and runs it.

   control_test <folder> [quit | level | mem]: the cases, quit (a child serving while the parent asks it to quit), the
   HOS_CONTROL_MEM levels, or the memory cases at the level in the environment. (Windows: quitserve <pipe> and
   servefail <pipe> are the children those start.) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../host/control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)

/* ---- the platform: sleep, threads, a lock ---- */
#ifdef _WIN32
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
typedef struct { void *(*fn)(void *); void *arg; } Start;
static DWORD WINAPI start_tramp(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static void start_thread(void *(*fn)(void *), void *arg) {
    Start *s = (Start *)malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    HANDLE t = CreateThread(NULL, 0, start_tramp, s, 0, NULL);
    CHECK(t);
    CloseHandle(t);
}
static SRWLOCK log_mu = SRWLOCK_INIT;
static void log_lock(void) { AcquireSRWLockExclusive(&log_mu); }
static void log_unlock(void) { ReleaseSRWLockExclusive(&log_mu); }
#else
static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static void start_thread(void *(*fn)(void *), void *arg) {
    pthread_t t;
    CHECK(pthread_create(&t, NULL, fn, arg) == 0);
    pthread_detach(t);
}
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static void log_lock(void) { pthread_mutex_lock(&log_mu); }
static void log_unlock(void) { pthread_mutex_unlock(&log_mu); }
#endif

/* ---- the fake game ---- */
static volatile int64_t frame;
static volatile double draws_per_frame = 2;
static volatile int64_t draws_total;
static volatile int paused;
#define FRAMES 200000
static uint64_t seen_buttons[FRAMES];
static int32_t seen_stick[FRAMES][4];
static int seen_exclusive[FRAMES];
static char logged[64][300];
static int nlogged;

static int64_t fake_frame(void) { return __atomic_load_n(&frame, __ATOMIC_ACQUIRE); }
static int fake_frame_info(int64_t *replayed, int64_t *draws, int *w, int *h) {
    *replayed = fake_frame(), *draws = draws_total, *w = 1280, *h = 720;
    return 1;
}
typedef struct { char path[1100]; void (*done)(void *, int64_t, int); void *ctx; int64_t after; } Shot;
static void *shot_thread(void *p) {
    Shot *s = (Shot *)p;
    while (fake_frame() <= s->after) sleep_ms(1);
    const int64_t at = fake_frame();
    FILE *f = fopen(s->path, "wb");
    static const unsigned char head[24] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                           0, 0, 2, 0x80, 0, 0, 1, 0x68};   /* (a PNG's header: 640x360) */
    if (f) fwrite(head, 1, sizeof head, f), fclose(f);
    s->done(s->ctx, at, f != NULL);
    free(s);
    return NULL;
}
static int fake_shot(const char *path, int64_t after, void (*done)(void *, int64_t, int), void *ctx) {
    Shot *s = (Shot *)calloc(1, sizeof *s);
    snprintf(s->path, sizeof s->path, "%s", path);
    s->done = done, s->ctx = ctx, s->after = after;
    start_thread(shot_thread, s);
    return 1;
}
static HosControlSetting settings[3] = {
    {"text_speed", "Text speed", "Normal", "gameplay", 0, 0, 3, 1, 0, 0},
    {"fps_counter", "FPS counter", "Off", "gameplay", 0, 0, 1, 1, 0, 0},
    {"upscaling", "Upscaler", "Off", "graphics", 0, 0, 2, 1, 1, 1}};   /* (fixed: nothing to choose here) */
static int fake_setting(int i, HosControlSetting *out) {
    if (i < 0 || i >= 3) return 0;
    *out = settings[i];
    return 1;
}
static int fake_setting_set(const char *key, int v, HosControlSetting *out) {   /* (by key, as the menu's menu_setting_set) */
    for (int i = 0; i < 3; i++) {
        if (strcmp(settings[i].key, key)) continue;
        if (settings[i].fixed) return *out = settings[i], -1;
        settings[i].value = v < settings[i].lo ? settings[i].lo : v > settings[i].hi ? settings[i].hi : v;
        snprintf(settings[i].text, sizeof settings[i].text, "v%d", settings[i].value);
        *out = settings[i];
        return 1;
    }
    return 0;
}
static void fake_log(const char *line) {
    if (getenv("CONTROL_TEST_VERBOSE")) fprintf(stderr, "log: %s\n", line);
    log_lock();
    snprintf(logged[nlogged++ % 64], sizeof logged[0], "%s", line);
    log_unlock();
    hos_control_log(line);   /* (as hos_log does) */
}
/* a fake mapped region [MEM_BASE, MEM_BASE + MEM_N): the only memory the fake game has */
#define MEM_BASE 0x10000ull
#define MEM_N 4096u
static unsigned char guest[MEM_N];
static int fake_mem_region(uint64_t addr, uint64_t size) {
    uint64_t end;
    if (!size || __builtin_add_overflow(addr, size, &end)) return 0;
    return addr >= MEM_BASE && end <= MEM_BASE + MEM_N;
}
static void fake_mem_read(uint64_t addr, void *dst, uint64_t size) { memcpy(dst, guest + (addr - MEM_BASE), size); }
static void fake_mem_write(uint64_t addr, const void *src, uint64_t size) { memcpy(guest + (addr - MEM_BASE), src, size); }
int hos_control_mem_level(const char *v);   /* (host/control.c) */
static void fake_quit(void) {
    fflush(NULL);
#ifdef _WIN32
    ExitProcess(0);
#else
    _exit(0);
#endif
}
static const char *fake_renderer(void) { return "fake"; }
static char save_root[1100];   /* (a fake save folder, made by the saves case; empty: not opened yet) */
static int fake_save_root(char *out, size_t cap) {
    if (!save_root[0]) return 0;
    snprintf(out, cap, "%s", save_root);
    return 1;
}
/* (OoT) the map and the teleport: a teleport asked for is taken by the game's thread at its next frame, as the player's
   update takes it, unless teleport_held (not in play: nobody takes it). Link is the child: destination 2 has no child map,
   so it is refused, as hos_main.c's oot_teleport refuses it */
static const char *const dest_child[3] = {"KokiriForest_Child", "HyruleField_Child", NULL};
static const char *const dest_adult[3] = {"KokiriForest_Adult", "HyruleField_Adult", "GerudoFortress_Adult"};
static char fake_map_now[80];
static volatile int teleport_asked = -1, teleport_held;
static int tp_ticket, tp_done_ticket, tp_result, tp_taking;   /* (log_lock) */
static int fake_map(char *out, size_t cap) {
    log_lock();
    const int have = fake_map_now[0] != 0;
    if (have) snprintf(out, cap, "%s", fake_map_now);
    log_unlock();
    return have;
}
static int fake_destination(int i, const char **child, const char **adult) {
    if (i < 0 || i >= 3) return 0;
    *child = dest_child[i], *adult = dest_adult[i];
    return 1;
}
/* (the age switch: asked as a teleport is, with teleport_asked = AGE_ASKED + 1 for the adult, + 0 the child; taken, Link
   that age already: nothing done (result 2), else his age flips and the map reloads in it, as hos_main.c does.
   stale_age: the state's sample of the age is the other one, as an old sample can be) */
enum { AGE_ASKED = 100 };
static volatile int fake_child = 1, stale_age;
static int fake_state(int *event, int *loading, int *child) {
    if (!fake_map(NULL, 0)) return 0;
    *event = 0, *loading = 1, *child = __atomic_load_n(&fake_child, __ATOMIC_RELAXED) ^ (stale_age != 0);
    return 1;
}
static int fake_teleport(int i) {
    if (i < 0 || i >= 3) return 0;
    log_lock();
    const int ticket = ++tp_ticket;
    __atomic_store_n(&teleport_asked, i, __ATOMIC_RELAXED);
    log_unlock();
    return ticket;
}
static int fake_teleport_result(int ticket, char *why, size_t why_cap, char *map, size_t map_cap, int *accepted) {
    log_lock();
    int r = -2;
    if (tp_done_ticket == ticket) {
        r = tp_result, *accepted = r == 1;
        snprintf(why, why_cap, "%s", r < 0 ? "this destination has no map for the child" : "");
        snprintf(map, map_cap, "%s", r == 1 ? fake_map_now : "");
    } else if (tp_taking == ticket || (tp_ticket == ticket && __atomic_load_n(&teleport_asked, __ATOMIC_RELAXED) >= 0)) r = 0;
    log_unlock();
    return r;
}
static int fake_age(int adult) {
    log_lock();
    const int ticket = ++tp_ticket;
    __atomic_store_n(&teleport_asked, AGE_ASKED + (adult ? 1 : 0), __ATOMIC_RELAXED);
    log_unlock();
    return ticket;
}
static void fake_teleport_cancel(int ticket) {
    log_lock();
    if (tp_ticket == ticket) __atomic_store_n(&teleport_asked, -1, __ATOMIC_RELAXED);
    log_unlock();
}
static HosControlGame game = {
    .frame = fake_frame, .frame_info = fake_frame_info, .shot = fake_shot, .setting = fake_setting,
    .setting_set = fake_setting_set, .mem_region = fake_mem_region, .mem_read = fake_mem_read,
    .mem_write = fake_mem_write, .save_root = fake_save_root, .log = fake_log, .quit = fake_quit,
    .renderer = fake_renderer, .version = "test", .map = fake_map, .destination = fake_destination,
    .teleport = fake_teleport, .teleport_result = fake_teleport_result, .teleport_cancel = fake_teleport_cancel,
    .age = fake_age, .oot_state = fake_state};

static void *game_thread(void *unused) {
    (void)unused;
    for (;;) {
        sleep_ms(2);
        if (paused) continue;
        uint64_t b = 0;
        int32_t st[4] = {0, 0, 0, 0};
        const int ex = hos_control_input(&b, st);
        if (!teleport_held) {   /* (taken, then done a while later, as the game's update takes it and then asks for it) */
            log_lock();
            const int t = __atomic_exchange_n(&teleport_asked, -1, __ATOMIC_RELAXED);
            const int ticket = tp_ticket;
            if (t >= 0) tp_taking = ticket;
            log_unlock();
            if (t >= AGE_ASKED) {   /* (the age switch: the current map, reloaded in the age asked for; that age already: none) */
                sleep_ms(20);
                log_lock();
                const int child = t == AGE_ASKED;
                if (__atomic_load_n(&fake_child, __ATOMIC_RELAXED) == child) tp_result = 2;
                else {
                    __atomic_store_n(&fake_child, child, __ATOMIC_RELAXED);
                    snprintf(fake_map_now, sizeof fake_map_now, "%s", child ? dest_child[0] : dest_adult[0]);
                    tp_result = 1;
                }
                tp_done_ticket = ticket, tp_taking = 0;
                log_unlock();
            } else if (t >= 0) {
                sleep_ms(20);
                log_lock();
                tp_done_ticket = ticket, tp_result = dest_child[t] ? 1 : -1, tp_taking = 0;
                if (dest_child[t]) snprintf(fake_map_now, sizeof fake_map_now, "%s", dest_child[t]);
                log_unlock();
            }
        }
        const int64_t f = fake_frame();
        if (f < FRAMES) seen_buttons[f] = b, memcpy(seen_stick[f], st, sizeof st), seen_exclusive[f] = ex;
        draws_total += (int64_t)draws_per_frame;
        __atomic_store_n(&frame, f + 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* ---- the client: a connection to the bridge, and the bytes after its last whole line ---- */
static char sock_path[300];
typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
    char pend[1 << 16];
    size_t n;
} Client;

#ifdef _WIN32
static Client *dial(void) {
    Client *c = (Client *)calloc(1, sizeof *c);
    for (int i = 0; i < 500; i++) {   /* (the bridge has one instance waiting at a time: busy ones are waited for) */
        c->h = CreateFileA(sock_path, GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (c->h != INVALID_HANDLE_VALUE) return c;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeA(sock_path, 20);
        else sleep_ms(10);
    }
    CHECK(!"the pipe could not be opened");
    return NULL;
}
static void say_raw(Client *c, const void *p, size_t n) {
    DWORD wrote = 0;
    CHECK(WriteFile(c->h, p, (DWORD)n, &wrote, NULL) && wrote == n);
}
/* bytes waiting, ms at most: 1 some, 0 none yet, -1 the bridge's end closed */
static int wait_readable(Client *c, int ms) {
    for (int waited = 0;; waited++) {
        DWORD avail = 0;
        if (!PeekNamedPipe(c->h, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail) return 1;
        if (waited >= ms) return 0;
        sleep_ms(1);
    }
}
static long read_some(Client *c, void *p, size_t n) {
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(c->h, NULL, 0, NULL, &avail, NULL) || !avail) return -1;
    if (!ReadFile(c->h, p, (DWORD)(avail < n ? avail : n), &got, NULL)) return -1;
    return (long)got;
}
static void hangup(Client *c) { CloseHandle(c->h), free(c); }
#else
static Client *dial(void) {
    Client *c = (Client *)calloc(1, sizeof *c);
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
    CHECK(connect(c->fd, (struct sockaddr *)&a, sizeof a) == 0);
    return c;
}
static void say_raw(Client *c, const void *p, size_t n) { CHECK(write(c->fd, p, n) == (ssize_t)n); }
static int wait_readable(Client *c, int ms) {
    struct pollfd p = {c->fd, POLLIN, 0};
    const int r = poll(&p, 1, ms);
    return r < 0 ? -1 : r > 0;
}
static long read_some(Client *c, void *p, size_t n) { return (long)read(c->fd, p, n); }
static void hangup(Client *c) { close(c->fd), free(c); }
#endif
static void say(Client *c, const char *s) { say_raw(c, s, strlen(s)); }
/* one line (without the newline), waiting ms at most: 0 when none came */
static int hear(Client *c, char *out, size_t cap, int ms) {
    for (;;) {
        char *nl = (char *)memchr(c->pend, '\n', c->n);
        if (nl) {
            const size_t n = (size_t)(nl - c->pend);
            CHECK(n < cap);
            memcpy(out, c->pend, n);
            out[n] = 0;
            memmove(c->pend, nl + 1, c->n - n - 1);
            c->n -= n + 1;
            return 1;
        }
        if (wait_readable(c, ms) <= 0) return 0;
        const long r = read_some(c, c->pend + c->n, sizeof c->pend - c->n);
        if (r <= 0) return 0;
        c->n += (size_t)r;
    }
}
/* the reply with this id (events and other replies skipped) */
static void ask(Client *c, const char *req, const char *id_text, char *out, size_t cap, int ms) {
    say(c, req);
    char want[64];
    snprintf(want, sizeof want, "{\"id\":%s,", id_text);
    for (;;) {
        CHECK(hear(c, out, cap, ms));
        if (!strncmp(out, want, strlen(want))) return;
    }
}
/* status until its line has want (and want2, if given), asked again for at most ms: its game state is the one the
   bridge's thread samples every 100 ms, so a change shows within that, not at the next frame */
static void status_until(Client *c, const char *id_text, const char *want, const char *want2, char *out, size_t cap,
                         int ms) {
    char req[64];
    snprintf(req, sizeof req, "{\"id\":%s,\"cmd\":\"status\"}\n", id_text);
    for (int waited = 0;; waited += 20) {
        ask(c, req, id_text, out, cap, 2000);
        if ((strstr(out, want) && (!want2 || strstr(out, want2))) || waited >= ms) return;
        sleep_ms(20);
    }
}
/* the replies with these two ids, whichever comes first (events and other replies skipped) */
static void hear_two(Client *c, const char *id_a, char *a, const char *id_b, char *b, size_t cap, int ms) {
    char want_a[64], want_b[64], line[8192];
    snprintf(want_a, sizeof want_a, "{\"id\":%s,", id_a);
    snprintf(want_b, sizeof want_b, "{\"id\":%s,", id_b);
    a[0] = b[0] = 0;
    while (!a[0] || !b[0]) {
        CHECK(hear(c, line, sizeof line, ms));
        if (!strncmp(line, want_a, strlen(want_a))) snprintf(a, cap, "%s", line);
        if (!strncmp(line, want_b, strlen(want_b))) snprintf(b, cap, "%s", line);
    }
}
static long long num_after(const char *s, const char *key) {
    const char *p = strstr(s, key);
    CHECK(p);
    return atoll(p + strlen(key));
}
/* a reply's "path", its JSON escapes (a Windows path's backslashes) undone */
static void path_of(const char *line, char *out, size_t cap) {
    const char *p = strstr(line, "\"path\":\"");
    CHECK(p);
    p += 8;
    size_t n = 0;
    for (; *p && *p != '"' && n + 1 < cap; p++) out[n++] = *p == '\\' && p[1] ? *++p : *p;
    out[n] = 0;
}
static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}
static void make_dir(const char *p) {
#ifdef _WIN32
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0700);
#endif
}
static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(data, 1, n, f) == n);
    fclose(f);
}
static void wait_frames(int64_t n) { const int64_t to = fake_frame() + n; while (fake_frame() < to) sleep_ms(1); }

/* the transport's own name for this run: a socket in the folder, or a pipe named after the process */
static void transport_name(const char *folder, const char *tag) {
#ifdef _WIN32
    (void)folder;
    snprintf(sock_path, sizeof sock_path, "\\\\.\\pipe\\hookshot-control-test-%lu-%s", (unsigned long)GetCurrentProcessId(), tag);
#else
    snprintf(sock_path, sizeof sock_path, "%s/%s.sock", folder, tag);
#endif
}

#ifdef _WIN32
/* this program again, as a child (quitserve, servefail): its exit code, or the running process */
static HANDLE spawn_self(const char *mode, const char *pipe) {
    char exe[MAX_PATH], cmd[MAX_PATH * 2 + 400];
    CHECK(GetModuleFileNameA(NULL, exe, MAX_PATH));
    snprintf(cmd, sizeof cmd, "\"%s\" . %s %s", exe, mode, pipe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    CHECK(CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
static DWORD wait_exit(HANDLE p, DWORD ms) {
    DWORD code = 999;
    CHECK(WaitForSingleObject(p, ms) == WAIT_OBJECT_0);
    GetExitCodeProcess(p, &code);
    CloseHandle(p);
    return code;
}
#endif

static void quit_case(const char *folder) {
    transport_name(folder, "quit");
#ifdef _WIN32
    HANDLE child = spawn_self("quitserve", sock_path);
#else
    const pid_t child = fork();
    if (child == 0) {
        if (!hos_control_serve(sock_path, &game)) _exit(3);
        start_thread(game_thread, NULL);
        for (;;) pause();
    }
    struct stat st;
    for (int i = 0; i < 500 && stat(sock_path, &st); i++) sleep_ms(2);
#endif
    Client *c = dial();
    char line[4096];
    ask(c, "{\"id\":9,\"cmd\":\"quit\"}\n", "9", line, sizeof line, 2000);
    CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"quitting\":true"));
#ifdef _WIN32
    CHECK(wait_exit(child, 5000) == 0);
    hangup(c);
    CHECK(CreateFileA(sock_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE);
    puts("PASS: quit: the reply, then a clean exit, the pipe gone");
#else
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(stat(sock_path, &st) != 0);   /* (the socket removed) */
    puts("PASS: quit: the reply, then a clean exit, the socket removed");
#endif
}

static void level_unit(void) {
    struct { const char *v; int lvl; } cases[] = {
        {NULL, 0}, {"", 0}, {"ro", 1}, {"RO", 1}, {"r", 1}, {"read", 1}, {"Read", 1},
        {"rw", 2}, {"RW", 2}, {"readwrite", 2}, {"ReadWrite", 2},
        {"w", -1}, {"rwx", -1}, {"yes", -1}, {"1", -1}, {"readwritex", -1}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++)
        CHECK(hos_control_mem_level(cases[i].v) == cases[i].lvl);
    puts("PASS: HOS_CONTROL_MEM levels (ro/r/read, rw/readwrite, bad values)");
}

static void mem_case(const char *folder) {
    const int level = hos_control_mem_level(getenv("HOS_CONTROL_MEM"));   /* what this run was started with */
    for (unsigned i = 0; i < MEM_N; i++) guest[i] = (unsigned char)(i * 7 + 3);
    transport_name(folder, "mem");
    CHECK(hos_control_serve(sock_path, &game));
    start_thread(game_thread, NULL);
    Client *c = dial();
    static char line[70000];
    if (level <= 0) {   /* unset, empty, or a bad value: every mem command is forbidden, with the hint */
        ask(c, "{\"id\":1,\"cmd\":\"mem_read\",\"addr\":65536,\"size\":4}\n", "1", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"forbidden\"") && strstr(line, "HOS_CONTROL_MEM"));
        puts("PASS: memory off: mem_read is forbidden with the hint");
        paused = 1;
        return;
    }
    /* a read in range (ro and rw) */
    ask(c, "{\"id\":2,\"cmd\":\"mem_read\",\"addr\":65536,\"size\":8}\n", "2", line, sizeof line, 2000);
    CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"format\":\"hex\"") && strstr(line, "\"data\":\"030a11181f262d34\""));
    ask(c, "{\"id\":3,\"cmd\":\"mem_read\",\"addr\":65536,\"size\":4,\"format\":\"base64\"}\n", "3", line, sizeof line, 2000);
    CHECK(strstr(line, "\"data\":\"AwoRGA==\""));
    /* out of range: below, above, and crossing the end */
    ask(c, "{\"id\":4,\"cmd\":\"mem_read\",\"addr\":0,\"size\":4}\n", "4", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"out_of_range\""));
    ask(c, "{\"id\":5,\"cmd\":\"mem_read\",\"addr\":69632,\"size\":1}\n", "5", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"out_of_range\""));
    ask(c, "{\"id\":6,\"cmd\":\"mem_read\",\"addr\":69630,\"size\":4}\n", "6", line, sizeof line, 2000);   /* crosses MEM_BASE+MEM_N */
    CHECK(strstr(line, "\"error\":\"out_of_range\""));
    ask(c, "{\"id\":7,\"cmd\":\"mem_read\",\"addr\":65536,\"size\":2000000}\n", "7", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));   /* over 1 MB */
    /* addresses as strings: 0x hex or decimal; no sign, no octal, no fraction, nothing past 64 bits */
    ask(c, "{\"id\":71,\"cmd\":\"mem_read\",\"addr\":\"0x10000\",\"size\":\"2\"}\n", "71", line, sizeof line, 2000);
    CHECK(strstr(line, "\"data\":\"030a\""));
    static const char *const bad_addrs[] = {"\"-1\"", "\"+65536\"", "\" 65536\"", "\"0x\"", "\"0x10000000000000000\"",
                                            "\"99999999999999999999999\"", "65536.5", "\"65536x\""};
    for (unsigned i = 0; i < sizeof bad_addrs / sizeof *bad_addrs; i++) {
        char req[200];
        snprintf(req, sizeof req, "{\"id\":72,\"cmd\":\"mem_read\",\"addr\":%s,\"size\":2}\n", bad_addrs[i]);
        ask(c, req, "72", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"bad_request\""));
    }
    ask(c, "{\"id\":73,\"cmd\":\"mem_read\",\"addr\":\"065536\",\"size\":2}\n", "73", line, sizeof line, 2000);
    CHECK(strstr(line, "\"data\":\"030a\""));   /* (decimal, not octal) */
    /* many ranges in one snapshot */
    ask(c, "{\"id\":8,\"cmd\":\"mem_read_many\",\"ranges\":[{\"addr\":65536,\"size\":2},{\"addr\":65792,\"size\":2}]}\n",
        "8", line, sizeof line, 2000);
    CHECK(strstr(line, "\"ranges\":[") && strstr(line, "\"addr\":65536") && strstr(line, "\"addr\":65792"));
    if (level < 2) {   /* ro: writes refused */
        ask(c, "{\"id\":9,\"cmd\":\"mem_write\",\"addr\":65536,\"hex\":\"ff\"}\n", "9", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"forbidden\"") && strstr(line, "rw"));
        CHECK(guest[0] == 3);   /* unchanged */
        puts("PASS: HOS_CONTROL_MEM=ro: reads work, bounds enforced, writes forbidden");
        paused = 1;
        return;
    }
    /* rw: a write, then read it back */
    ask(c, "{\"id\":10,\"cmd\":\"mem_write\",\"addr\":65536,\"hex\":\"deadbeef\"}\n", "10", line, sizeof line, 2000);
    CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"wrote\":4"));
    ask(c, "{\"id\":11,\"cmd\":\"mem_read\",\"addr\":65536,\"size\":4}\n", "11", line, sizeof line, 2000);
    CHECK(strstr(line, "\"data\":\"deadbeef\""));
    CHECK(guest[0] == 0xde && guest[3] == 0xef);
    ask(c, "{\"id\":12,\"cmd\":\"mem_write\",\"addr\":0,\"hex\":\"00\"}\n", "12", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"out_of_range\""));
    ask(c, "{\"id\":13,\"cmd\":\"mem_write\",\"addr\":65536,\"hex\":\"zz\"}\n", "13", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    puts("PASS: HOS_CONTROL_MEM=rw: a write lands and reads back; bounds and bad bytes refused");
    paused = 1;
}

#ifdef _WIN32
/* The pipe's DACL, read back through a client's handle: protected, one ACE, allowing this user alone */
static void dacl_case(Client *c) {
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    CHECK(GetSecurityInfo(c->h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd) == ERROR_SUCCESS);
    CHECK(dacl && dacl->AceCount == 1);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    CHECK(GetSecurityDescriptorControl(sd, &control, &revision) && (control & SE_DACL_PROTECTED));
    ACCESS_ALLOWED_ACE *ace = NULL;
    CHECK(GetAce(dacl, 0, (void **)&ace) && ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
    HANDLE token;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    char info[256];
    DWORD len = 0;
    CHECK(GetTokenInformation(token, TokenUser, info, sizeof info, &len));
    CloseHandle(token);
    CHECK(EqualSid((PSID)&ace->SidStart, ((TOKEN_USER *)info)->User.Sid));
    LocalFree(sd);
    puts("PASS: the pipe's DACL: protected, one ACE, this user alone");
}
#endif

int main(int argc, char **argv) {
    CHECK(argc >= 2);
#ifdef _WIN32
    if (argc > 3 && !strcmp(argv[2], "quitserve")) {   /* (quit_case's child) */
        snprintf(sock_path, sizeof sock_path, "%s", argv[3]);
        if (!hos_control_serve(sock_path, &game)) return 3;
        start_thread(game_thread, NULL);
        for (;;) sleep_ms(1000);
    }
    if (argc > 3 && !strcmp(argv[2], "servefail")) {   /* (a second bridge on a name in use: refused) */
        snprintf(sock_path, sizeof sock_path, "%s", argv[3]);
        return hos_control_serve(sock_path, &game) ? 0 : 3;
    }
#endif
    if (argc > 2 && !strcmp(argv[2], "quit")) return quit_case(argv[1]), 0;
    if (argc > 2 && !strcmp(argv[2], "level")) return level_unit(), 0;
    if (argc > 2 && !strcmp(argv[2], "mem")) return mem_case(argv[1]), 0;
    static char line[70000];

#ifndef _WIN32
    /* a file that is not a socket is never replaced */
    snprintf(sock_path, sizeof sock_path, "%s/plain", argv[1]);
    FILE *f = fopen(sock_path, "w");
    fputs("keep", f), fclose(f);
    CHECK(!hos_control_serve(sock_path, &game));
    f = fopen(sock_path, "r");
    CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, "keep"));
    fclose(f);
    puts("PASS: an existing file that is not a socket is left alone");
    {   /* a folder others can write to (a shared /tmp) is refused: another user could put a link or a socket of theirs there */
        char shared[1100];
        snprintf(shared, sizeof shared, "%s/shared", argv[1]);
        CHECK(mkdir(shared, 0700) == 0 && chmod(shared, 0777) == 0);
        snprintf(sock_path, sizeof sock_path, "%s/control.sock", shared);
        CHECK(!hos_control_serve(sock_path, &game));
        struct stat st;
        CHECK(lstat(sock_path, &st) != 0);   /* (nothing made there) */
        CHECK(chmod(shared, 0730) == 0);   /* (group-writable: refused too) */
        CHECK(!hos_control_serve(sock_path, &game));
        puts("PASS: a socket folder others can write to is refused");
    }
#endif

    transport_name(argv[1], "game");
    CHECK(hos_control_serve(sock_path, &game));
#ifdef _WIN32
    {   /* the name is this bridge's: a first instance by anyone else is refused, and so is a second bridge */
        HANDLE squat = CreateNamedPipeA(sock_path, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE, 1, 4096,
                                        4096, 0, NULL);
        CHECK(squat == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED);
        CHECK(wait_exit(spawn_self("servefail", sock_path), 10000) == 3);
        puts("PASS: the pipe's name: another first instance, and a second bridge, refused");
    }
#else
    struct stat st;
    CHECK(!lstat(sock_path, &st) && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);
    puts("PASS: the socket is 0600");
#endif
    start_thread(game_thread, NULL);
    Client *c = dial();
#ifdef _WIN32
    dacl_case(c);
#endif

    ask(c, "{\"id\":1,\"cmd\":\"status\"}\n", "1", line, sizeof line, 2000);
    CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"frame\":") && strstr(line, "\"version\":\"test\"") &&
          strstr(line, "\"renderer\":\"fake\"") && strstr(line, "\"window\":{\"width\":1280,\"height\":720}"));
    puts("PASS: status");

    /* malformed requests: an error, and the connection goes on */
    say(c, "{not json\n");
    CHECK(hear(c, line, sizeof line, 2000) && strstr(line, "\"id\":null") && strstr(line, "\"error\":\"bad_json\""));
    ask(c, "{\"id\":\"x\",\"cmd\":\"fly\"}\n", "\"x\"", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"unknown_cmd\""));
    say(c, "[1,2]\n");
    CHECK(hear(c, line, sizeof line, 2000) && strstr(line, "\"error\":\"bad_request\""));
    ask(c, "{\"id\":{\"a\":[1,true]}}\n", "{\"a\":[1,true]}", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    ask(c, "{\"id\":2,\"cmd\":\"press\",\"buttons\":\"A+NOPE\"}\n", "2", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\"") && strstr(line, "NOPE"));
    ask(c, "{\"id\":3,\"cmd\":\"press\",\"buttons\":\"A\",\"frames\":0}\n", "3", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    say(c, "\n   \r\n");   /* (empty lines: nothing) */
    ask(c, "{\"id\":\"tab\\t\\u00e9\",\"cmd\":\"status\"}\n", "\"tab\\t\xc3\xa9\"", line, sizeof line, 2000);
    /* numbers past int64_t's range: refused, never converted (undefined behaviour) */
    ask(c, "{\"id\":5,\"cmd\":\"press\",\"buttons\":\"A\",\"frames\":1e300}\n", "5", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    ask(c, "{\"id\":6,\"cmd\":\"press\",\"buttons\":\"A\",\"at\":-1e300}\n", "6", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    ask(c, "{\"id\":7,\"cmd\":\"wait\",\"event\":\"present\",\"frames\":1e300}\n", "7", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    ask(c, "{\"id\":8,\"cmd\":\"wait\",\"event\":\"frame\",\"frame\":1e300,\"timeout\":0.05}\n", "8", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"timeout\""));
    ask(c, "{\"id\":9,\"cmd\":\"sequence\",\"script\":\"1e300:A\"}\n", "9", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    ask(c, "{\"id\":10,\"cmd\":\"sequence\",\"script\":\"0:A/1e300\",\"units\":\"hos\"}\n", "10", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    {   /* an id longer than 200 characters as JSON: refused (it was taken as null) */
        char big[400];
        memset(big, 'x', 260);
        big[260] = 0;
        char req[600];
        snprintf(req, sizeof req, "{\"id\":\"%s\",\"cmd\":\"status\"}\n", big);
        ask(c, req, "null", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"bad_request\"") && strstr(line, "id: too long"));
    }
    puts("PASS: malformed JSON, unknown commands, bad arguments: errors, the connection kept");

    {   /* a line past 64 KB: refused, and the next request is served */
        static char big[100000];
        memset(big, 'x', sizeof big);
        say_raw(c, big, sizeof big);
        say(c, "\n");
        CHECK(hear(c, line, sizeof line, 2000) && strstr(line, "\"error\":\"too_long\""));
        ask(c, "{\"id\":4,\"cmd\":\"status\"}\n", "4", line, sizeof line, 2000);
        CHECK(strstr(line, "\"ok\":true"));
        puts("PASS: an oversized line is refused, the next one served");
    }

    /* press: held exactly its frames, from the first controller read after the request */
    ask(c, "{\"id\":5,\"cmd\":\"press\",\"buttons\":[\"A\"],\"frames\":5}\n", "5", line, sizeof line, 3000);
    CHECK(strstr(line, "\"ok\":true"));
    {
        const long long s = num_after(line, "\"start\":"), e = num_after(line, "\"end\":");
        CHECK(e == s + 5 && s >= 0);
        CHECK((s == 0 || !(seen_buttons[s - 1] & 1)) && !(seen_buttons[e] & 1));
        for (long long i = s; i < e; i++) CHECK(seen_buttons[i] == 1 && !seen_exclusive[i]);
        CHECK(num_after(line, "\"frame\":") >= e);
    }
    puts("PASS: press A for 5 frames: exactly frames start..end-1");

    ask(c, "{\"id\":6,\"cmd\":\"stick\",\"stick\":\"R\",\"x\":-1,\"y\":0.5,\"frames\":3,\"exclusive\":true}\n", "6", line,
        sizeof line, 3000);
    {
        const long long s = num_after(line, "\"start\":"), e = num_after(line, "\"end\":");
        CHECK(e == s + 3);
        for (long long i = s; i < e; i++)
            CHECK(seen_stick[i][2] == -32767 && seen_stick[i][3] == 16384 && seen_stick[i][0] == 0 && seen_exclusive[i]);
        CHECK(!seen_exclusive[e]);
    }
    puts("PASS: stick R (-1, 0.5) for 3 frames, exclusive while it lasts");

    ask(c, "{\"id\":7,\"cmd\":\"sequence\",\"script\":\"0:A/3,10:B+LX=-16000/2\"}\n", "7", line, sizeof line, 3000);
    {
        const long long s = num_after(line, "\"start\":"), e = num_after(line, "\"end\":");
        CHECK(e == s + 12);
        CHECK(seen_buttons[s] == 1 && seen_buttons[s + 2] == 1 && seen_buttons[s + 3] == 0);
        CHECK(seen_buttons[s + 10] == 2 && seen_stick[s + 10][0] == -16000 && seen_buttons[s + 12] == 0);
    }
    ask(c, "{\"id\":8,\"cmd\":\"sequence\",\"steps\":[{\"at\":0,\"buttons\":\"ZL\",\"frames\":4},{\"at\":2,\"lx\":1,\"frames\":1}]}\n",
        "8", line, sizeof line, 3000);
    {
        const long long s = num_after(line, "\"start\":");
        CHECK(seen_buttons[s] == 1 << 8 && seen_stick[s + 2][0] == 32767 && seen_stick[s + 3][0] == 0);
    }
    ask(c, "{\"id\":81,\"cmd\":\"sequence\",\"script\":\"1:A/100\",\"units\":\"hos\"}\n", "81", line, sizeof line, 3000);
    CHECK(num_after(line, "\"end\":") - num_after(line, "\"start\":") == 6);   /* (100 ms at 60 frames a second, from 1 s on) */
    puts("PASS: sequence: a script in frames, steps, HOS_INPUT units");

    ask(c, "{\"id\":9,\"cmd\":\"press\",\"buttons\":\"X\",\"frames\":50,\"wait\":false}\n", "9", line, sizeof line, 2000);
    CHECK(strstr(line, "\"queued\":1"));
    wait_frames(5);
    ask(c, "{\"id\":10,\"cmd\":\"release\"}\n", "10", line, sizeof line, 2000);
    CHECK(strstr(line, "\"released\":1"));
    wait_frames(3);
    CHECK(seen_buttons[fake_frame() - 1] == 0);
    puts("PASS: press without waiting, and release");

    {   /* inputs nobody waits for are freed once over: more of them than the queue holds, one after another */
        for (int i = 0; i < 300; i++) {
            char req[200], idt[16];
            snprintf(idt, sizeof idt, "%d", 1000 + i);
            snprintf(req, sizeof req, "{\"id\":%d,\"cmd\":\"press\",\"buttons\":\"B\",\"frames\":1,\"wait\":false}\n", 1000 + i);
            ask(c, req, idt, line, sizeof line, 2000);
            CHECK(strstr(line, "\"queued\":1"));
            if (i % 50 == 0) wait_frames(2);
        }
        wait_frames(10);
        sleep_ms(20);
        ask(c, "{\"id\":1400,\"cmd\":\"status\"}\n", "1400", line, sizeof line, 2000);
        CHECK(strstr(line, "\"queued\":0"));
        puts("PASS: presses without waiting are freed once over (300 of them; the queue holds 256)");
    }
    {   /* release: the caller's own inputs and those of clients gone; everyone's with all */
        Client *other = dial(), *left = dial();
        ask(other, "{\"id\":1,\"cmd\":\"press\",\"buttons\":\"X\",\"frames\":100000,\"wait\":false}\n", "1", line, sizeof line, 2000);
        ask(left, "{\"id\":1,\"cmd\":\"press\",\"buttons\":\"Y\",\"frames\":100000,\"wait\":false}\n", "1", line, sizeof line, 2000);
        hangup(left);   /* (its input stays: it asked for no wait) */
        say(other, "{\"id\":2,\"cmd\":\"press\",\"buttons\":\"ZR\",\"frames\":100000}\n");   /* (waited on) */
        wait_frames(5);
        ask(c, "{\"id\":1401,\"cmd\":\"release\"}\n", "1401", line, sizeof line, 2000);
        CHECK(strstr(line, "\"released\":1,\"all\":false"));   /* (the one left behind; not the other client's) */
        wait_frames(3);
        CHECK(seen_buttons[fake_frame() - 2] == ((1u << 2) | (1u << 9)));   /* (X and ZR still held, Y let go) */
        ask(c, "{\"id\":1402,\"cmd\":\"release\",\"all\":true}\n", "1402", line, sizeof line, 2000);
        CHECK(strstr(line, "\"released\":2,\"all\":true"));
        CHECK(hear(other, line, sizeof line, 2000) && strstr(line, "\"id\":2") && strstr(line, "\"error\":\"cancelled\""));
        wait_frames(3);
        CHECK(seen_buttons[fake_frame() - 2] == 0);
        hangup(other);
        puts("PASS: release: the caller's and abandoned inputs only; all: everyone's, a waited press told cancelled");
    }
#ifndef _WIN32
    {   /* a client that shuts its sending side after a request still gets the reply (printf ... | socat) */
        Client *half = dial();
        say(half, "{\"id\":1,\"cmd\":\"wait\",\"event\":\"present\",\"frames\":20}\n");
        CHECK(shutdown(half->fd, SHUT_WR) == 0);
        CHECK(hear(half, line, sizeof line, 3000) && strstr(line, "\"id\":1,\"ok\":true"));
        char rest[16];
        CHECK(wait_readable(half, 2000) > 0 && read_some(half, rest, sizeof rest) == 0);   /* (then closed) */
        hangup(half);
        puts("PASS: a half-closed client gets its reply, then the bridge closes it");
    }
#endif
    ask(c, "{\"id\":1403,\"cmd\":\"subscribe\",\"every\":1e18}\n", "1403", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));

    /* waits */
    ask(c, "{\"id\":11,\"cmd\":\"wait\",\"event\":\"present\",\"frames\":20}\n", "11", line, sizeof line, 3000);
    CHECK(strstr(line, "\"ok\":true") && num_after(line, "\"waited\":") >= 20);
    ask(c, "{\"id\":12,\"cmd\":\"wait\",\"event\":\"in_play\",\"timeout\":0.3}\n", "12", line, sizeof line, 3000);
    CHECK(strstr(line, "\"error\":\"timeout\"") && strstr(line, "\"state\":\"intro\""));
    say(c, "{\"id\":13,\"cmd\":\"wait\",\"event\":\"in_play\",\"timeout\":10}\n");
    draws_per_frame = 40;   /* a menu */
    ask(c, "{\"id\":14,\"cmd\":\"wait\",\"event\":\"state\",\"state\":\"menu\",\"timeout\":5}\n", "14", line, sizeof line, 6000);
    CHECK(strstr(line, "\"ok\":true"));
    draws_per_frame = 900;   /* play */
    CHECK(hear(c, line, sizeof line, 6000) && strstr(line, "\"id\":13") && strstr(line, "\"state\":\"play\""));
    say(c, "{\"id\":15,\"cmd\":\"wait\",\"event\":\"loading_start\",\"timeout\":5}\n");
    say(c, "{\"id\":16,\"cmd\":\"wait\",\"event\":\"loading_end\",\"timeout\":5}\n");
    sleep_ms(100);
    draws_per_frame = 5;   /* loading */
    CHECK(hear(c, line, sizeof line, 6000) && strstr(line, "\"id\":15") && strstr(line, "\"state\":\"loading\""));
    draws_per_frame = 1000;
    CHECK(hear(c, line, sizeof line, 6000) && strstr(line, "\"id\":16") && strstr(line, "\"state\":\"play\""));
    ask(c, "{\"id\":17,\"cmd\":\"wait\",\"event\":\"sunrise\"}\n", "17", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    puts("PASS: wait for frames, states, loading's start and end, with timeouts");

    /* shots */
    {
        char path[1100];
        ask(c, "{\"id\":18,\"cmd\":\"shot\"}\n", "18", line, sizeof line, 3000);
        CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"width\":640,\"height\":360"));   /* (the PNG's own size) */
        path_of(line, path, sizeof path);
#ifdef _WIN32
        CHECK(strstr(path, "\\shots\\shot_0001_f"));
#else
        CHECK(strstr(path, "/shots/shot_0001_f"));
#endif
        CHECK(file_exists(path));
        const long long target = fake_frame() + 30;
        char req[300];
        snprintf(req, sizeof req, "{\"id\":19,\"cmd\":\"shot\",\"path\":\"named.png\",\"after\":%lld}\n", target);
        ask(c, req, "19", line, sizeof line, 3000);
        CHECK(num_after(line, "\"present\":") > target);
        path_of(line, path, sizeof path);   /* (relative: the run folder's, the socket's own on Linux) */
        CHECK(file_exists(path) && strlen(path) > 9 && !strcmp(path + strlen(path) - 9, "named.png"));
#ifndef _WIN32
        snprintf(req, sizeof req, "%s/named.png", argv[1]);
        CHECK(!strcmp(path, req));
#endif
        ask(c, "{\"id\":20,\"cmd\":\"shot\",\"path\":\"/nonexistent-dir/x.png\"}\n", "20", line, sizeof line, 3000);
        CHECK(strstr(line, "\"error\":\"failed\""));
        puts("PASS: shots: the default folder, a relative path (the run folder), after a frame, an unwritable path");
    }

    /* the Insert menu's settings */
    ask(c, "{\"id\":21,\"cmd\":\"option_get\"}\n", "21", line, sizeof line, 2000);
    CHECK(strstr(line, "\"options\":[") && strstr(line, "\"key\":\"text_speed\"") && strstr(line, "\"key\":\"fps_counter\""));
    ask(c, "{\"id\":22,\"cmd\":\"option_set\",\"name\":\"Text speed\",\"value\":9}\n", "22", line, sizeof line, 2000);
    CHECK(strstr(line, "\"was\":0") && strstr(line, "\"value\":3") && strstr(line, "\"text\":\"v3\""));
    ask(c, "{\"id\":23,\"cmd\":\"option_set\",\"name\":\"fps-counter\",\"value\":true}\n", "23", line, sizeof line, 2000);
    CHECK(strstr(line, "\"value\":1"));
    ask(c, "{\"id\":24,\"cmd\":\"option_get\",\"name\":\"text_speed\"}\n", "24", line, sizeof line, 2000);
    CHECK(strstr(line, "\"option\":{\"key\":\"text_speed\"") && strstr(line, "\"value\":3"));
    ask(c, "{\"id\":25,\"cmd\":\"option_set\",\"name\":\"nope\",\"value\":1}\n", "25", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"not_found\""));
    ask(c, "{\"id\":26,\"cmd\":\"option_set\",\"name\":\"Upscaler\",\"value\":1}\n", "26", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"failed\"") && strstr(line, "nothing to choose"));
    ask(c, "{\"id\":27,\"cmd\":\"option_get\",\"name\":\"upscaling\"}\n", "27", line, sizeof line, 2000);
    CHECK(strstr(line, "\"restart\":true") && strstr(line, "\"fixed\":true"));
    ask(c, "{\"id\":28,\"cmd\":\"option_set\",\"name\":\"text_speed\",\"value\":1e300}\n", "28", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"bad_request\""));
    puts("PASS: option_get and option_set by key or name; a fixed row refused; a value out of range");

    {   /* saves: none before the game opens them; then the folder, newest slot first, caption.sav's fields when its
           layout is the game's */
        ask(c, "{\"id\":40,\"cmd\":\"saves\"}\n", "40", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"unavailable\""));
#ifdef _WIN32
        const char sep = '\\';
#else
        const char sep = '/';
#endif
        char p[1300];
        snprintf(save_root, sizeof save_root, "%s%csave_user", argv[1], sep);
        make_dir(save_root);
        snprintf(p, sizeof p, "%s%caccount.sav", save_root, sep), write_file(p, "acct", 4);
        snprintf(p, sizeof p, "%s%cslot_00", save_root, sep), make_dir(p);
        snprintf(p, sizeof p, "%s%cslot_00%cprogress.sav", save_root, sep, sep), write_file(p, "old progress", 12);
        sleep_ms(1100);   /* (modification times in whole seconds) */
        snprintf(p, sizeof p, "%s%cslot_03", save_root, sep), make_dir(p);
        static unsigned char cap[1500];
        memset(cap, 0, sizeof cap);
        memcpy(cap, "\x04\x03\x02\x01", 4);
        memcpy(cap + 684, "KokiriForest", 12);
        memcpy(cap + 748, "KokiriForest/KokiriForest_Child", 31);
        cap[1392] = 100;   /* (the JPEG's length, little-endian) */
        cap[1396] = 0xff, cap[1397] = 0xd8;
        snprintf(p, sizeof p, "%s%cslot_03%ccaption.sav", save_root, sep, sep), write_file(p, cap, sizeof cap);
        snprintf(p, sizeof p, "%s%cslot_03%cprogress.sav", save_root, sep, sep), write_file(p, "new", 3);
        ask(c, "{\"id\":41,\"cmd\":\"saves\"}\n", "41", line, sizeof line, 2000);
        CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"account\":true") && strstr(line, "\"option\":false"));
        const char *s03 = strstr(line, "\"slot\":\"03\""), *s00 = strstr(line, "\"slot\":\"00\"");
        CHECK(s03 && s00 && s03 < s00);   /* (newest first) */
        CHECK(strstr(s03, "\"newest\":true") && strstr(s00, "\"newest\":false"));
        CHECK(strstr(s03, "\"name\":\"caption.sav\",\"size\":1500"));
        CHECK(strstr(s03, "\"caption\":{\"scene\":\"KokiriForest\",\"map\":\"KokiriForest/KokiriForest_Child\","
                          "\"thumbnail\":{\"format\":\"jpeg\",\"offset\":1396,\"size\":100}}"));
        CHECK(!strstr(s00, "\"caption\""));   /* (no caption.sav: no fields) */
        CHECK(strstr(s00, "\"name\":\"progress.sav\",\"size\":12"));
        puts("PASS: saves: unavailable before the game opens them, then the folder, newest slot first, caption fields");
    }

    ask(c, "{\"id\":26,\"cmd\":\"mem_read\",\"addr\":0,\"size\":4}\n", "26", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"forbidden\""));   /* (this run has no HOS_CONTROL_MEM) */
    ask(c, "{\"id\":261,\"cmd\":\"symbols\"}\n", "261", line, sizeof line, 2000);
    CHECK(strstr(line, "\"error\":\"unavailable\""));
    puts("PASS: memory off without HOS_CONTROL_MEM; symbols not served");

    /* subscribe: presents, state changes, log lines with a filter */
    {
        Client *sub = dial();
        ask(sub, "{\"id\":\"s\",\"cmd\":\"subscribe\",\"events\":[\"present\",\"state\",\"log\"],\"every\":10,\"log\":\"needle\"}\n",
            "\"s\"", line, sizeof line, 2000);
        CHECK(strstr(line, "\"events\":[\"present\",\"state\",\"log\"]"));
        int presents = 0, logs = 0, states = 0;
        game.log("a haystack line");
        game.log("a needle line");
        draws_per_frame = 30;
        for (int i = 0; i < 400 && (presents < 3 || !logs || !states); i++) {
            if (!hear(sub, line, sizeof line, 50)) continue;
            CHECK(strstr(line, "\"sub\":\"s\""));
            presents += strstr(line, "\"event\":\"present\"") != NULL;
            if (strstr(line, "\"event\":\"log\"")) {
                CHECK(strstr(line, "needle") && !strstr(line, "haystack"));
                logs++;
            }
            states += strstr(line, "\"event\":\"state\"") && strstr(line, "\"state\":\"menu\"");
        }
        CHECK(presents >= 3 && logs == 1 && states >= 1);
        ask(sub, "{\"id\":\"u\",\"cmd\":\"unsubscribe\"}\n", "\"u\"", line, sizeof line, 2000);
        hangup(sub);
        puts("PASS: subscribe: presents, state changes, filtered log lines; unsubscribe");
    }

    /* (OoT) the map and the teleport */
    {
        ask(c, "{\"id\":70,\"cmd\":\"status\"}\n", "70", line, sizeof line, 2000);
        CHECK(strstr(line, "\"map\":null") && strstr(line, "\"game\":null"));
        ask(c, "{\"id\":71,\"cmd\":\"teleport\"}\n", "71", line, sizeof line, 2000);
        CHECK(strstr(line, "\"destinations\":[{\"index\":0,\"child\":\"KokiriForest_Child\"") &&
              strstr(line, "{\"index\":2,\"child\":null,\"adult\":\"GerudoFortress_Adult\"}]"));
        ask(c, "{\"id\":72,\"cmd\":\"teleport\",\"to\":9}\n", "72", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"not_found\""));
        ask(c, "{\"id\":73,\"cmd\":\"teleport\",\"to\":\"nowhere\"}\n", "73", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"not_found\""));
        Client *sub = dial();
        ask(sub, "{\"id\":\"m\",\"cmd\":\"subscribe\",\"events\":[\"map\"]}\n", "\"m\"", line, sizeof line, 2000);
        CHECK(strstr(line, "\"events\":[\"map\"]"));
        /* (wait map's name in any case, as teleport takes it) */
        say(c, "{\"id\":74,\"cmd\":\"wait\",\"event\":\"map\",\"map\":\"hyrulefield_child\",\"timeout\":5}\n");
        say(c, "{\"id\":75,\"cmd\":\"teleport\",\"to\":\"hyrulefield_adult\"}\n");
        static char r74[8192], r75[8192];
        hear_two(c, "74", r74, "75", r75, sizeof r74, 5000);
        CHECK(strstr(r75, "\"ok\":true") && strstr(r75, "\"teleport\":1") && strstr(r75, "\"child\":\"HyruleField_Child\"") &&
              strstr(r75, "\"sent\":true,\"map\":\"HyruleField_Child\",\"accepted\":true"));
        CHECK(strstr(r74, "\"ok\":true") && strstr(r74, "\"map\":\"HyruleField_Child\""));
        CHECK(hear(sub, line, sizeof line, 2000) && strstr(line, "\"event\":\"map\"") &&
              strstr(line, "\"map\":\"HyruleField_Child\",\"from\":null"));
        say(c, "{\"id\":76,\"cmd\":\"wait\",\"event\":\"map\",\"timeout\":5}\n");   /* (any other map) */
        say(c, "{\"id\":77,\"cmd\":\"teleport\",\"to\":0}\n");
        hear_two(c, "76", r74, "77", r75, sizeof r74, 5000);
        CHECK(strstr(r74, "\"ok\":true") && strstr(r74, "\"map\":\"KokiriForest_Child\""));
        CHECK(strstr(r75, "\"sent\":true"));
        CHECK(hear(sub, line, sizeof line, 2000) && strstr(line, "\"from\":\"HyruleField_Child\""));
        /* refused by the game (no child map there): said, with the game's reason, not left to a wait map's timeout */
        ask(c, "{\"id\":771,\"cmd\":\"teleport\",\"to\":2}\n", "771", line, sizeof line, 3000);
        CHECK(strstr(line, "\"error\":\"refused\"") && strstr(line, "no map for the child"));
        /* not taken (not in play): a timeout that says so, and the ask is withdrawn */
        teleport_held = 1;
        ask(c, "{\"id\":772,\"cmd\":\"teleport\",\"to\":1,\"timeout\":0.2}\n", "772", line, sizeof line, 3000);
        CHECK(strstr(line, "\"error\":\"timeout\"") && strstr(line, "in play"));
        CHECK(__atomic_load_n(&teleport_asked, __ATOMIC_RELAXED) == -1);
        /* a later teleport replaces one not taken yet; without waiting, the reply is at once */
        say(c, "{\"id\":773,\"cmd\":\"teleport\",\"to\":1}\n");
        sleep_ms(30);
        ask(c, "{\"id\":774,\"cmd\":\"teleport\",\"to\":0,\"wait\":false}\n", "774", line, sizeof line, 3000);
        CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"sent\":null"));
        CHECK(hear(c, line, sizeof line, 3000) && strstr(line, "\"id\":773") && strstr(line, "\"error\":\"cancelled\""));
        teleport_held = 0;
        wait_frames(5);
        ask(c, "{\"id\":78,\"cmd\":\"status\"}\n", "78", line, sizeof line, 2000);
        CHECK(strstr(line, "\"map\":\"KokiriForest_Child\"") &&
              strstr(line, "\"game\":{\"event\":false,\"loading\":true,\"age\":\"child\"}"));
        /* the age switch: none is the other age, and the reply has it and the map it reloads; to the age Link has already:
           said, nothing done; anything but child or adult: a bad request */
        ask(c, "{\"id\":79,\"cmd\":\"age\",\"to\":\"teen\"}\n", "79", line, sizeof line, 2000);
        CHECK(strstr(line, "\"error\":\"bad_request\""));
        ask(c, "{\"id\":791,\"cmd\":\"age\",\"to\":\"child\"}\n", "791", line, sizeof line, 2000);
        CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"age\":\"child\",\"sent\":false"));
        ask(c, "{\"id\":792,\"cmd\":\"age\"}\n", "792", line, sizeof line, 3000);
        CHECK(strstr(line, "\"ok\":true") &&
              strstr(line, "\"age\":\"adult\",\"sent\":true,\"map\":\"KokiriForest_Adult\",\"accepted\":true"));
        status_until(c, "793", "\"age\":\"adult\"", NULL, line, sizeof line, 2000);
        CHECK(strstr(line, "\"age\":\"adult\""));
        /* the bridge's sample of the age old (it says the child, Link is the adult): the game checks the age asked for when
           it takes it, so "adult" does not flip the adult into the child, and none (the other age by the sample: the
           adult) neither; both say the adult already */
        stale_age = 1;
        ask(c, "{\"id\":7931,\"cmd\":\"age\",\"to\":\"adult\"}\n", "7931", line, sizeof line, 3000);
        CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"age\":\"adult\",\"sent\":false"));
        CHECK(__atomic_load_n(&fake_child, __ATOMIC_RELAXED) == 0);
        ask(c, "{\"id\":7932,\"cmd\":\"age\"}\n", "7932", line, sizeof line, 3000);
        CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"age\":\"adult\",\"sent\":false"));
        CHECK(__atomic_load_n(&fake_child, __ATOMIC_RELAXED) == 0);
        stale_age = 0;
        /* not taken (not in play): a timeout that names the age switch, and the ask is withdrawn; one not taken yet is
           replaced by a later teleport, as a teleport is */
        teleport_held = 1;
        ask(c, "{\"id\":794,\"cmd\":\"age\",\"to\":\"child\",\"timeout\":0.2}\n", "794", line, sizeof line, 3000);
        CHECK(strstr(line, "\"error\":\"timeout\"") && strstr(line, "age switch"));
        CHECK(__atomic_load_n(&teleport_asked, __ATOMIC_RELAXED) == -1);
        say(c, "{\"id\":795,\"cmd\":\"age\"}\n");
        sleep_ms(30);
        ask(c, "{\"id\":796,\"cmd\":\"teleport\",\"to\":0,\"wait\":false}\n", "796", line, sizeof line, 3000);
        CHECK(hear(c, line, sizeof line, 3000) && strstr(line, "\"id\":795") && strstr(line, "\"error\":\"cancelled\""));
        teleport_held = 0;
        wait_frames(5);
        /* without waiting, the reply is at once; the switch back to the child then happens at the next update */
        ask(c, "{\"id\":797,\"cmd\":\"age\",\"to\":\"child\",\"wait\":false}\n", "797", line, sizeof line, 2000);
        CHECK(strstr(line, "\"ok\":true") && strstr(line, "\"age\":\"child\",\"sent\":null"));
        status_until(c, "798", "\"age\":\"child\"", "\"map\":\"KokiriForest_Child\"", line, sizeof line, 2000);
        CHECK(strstr(line, "\"map\":\"KokiriForest_Child\"") && strstr(line, "\"age\":\"child\""));
        hangup(sub);
        puts("PASS: (OoT) the map in status, events and waits; the teleport by index or map, and its list; the age switch");
    }

    /* a client gone in the middle of a press: its input is let go */
    {
        Client *gone = dial();
        say(gone, "{\"id\":1,\"cmd\":\"press\",\"buttons\":\"Y\",\"frames\":100000}\n");
        wait_frames(10);
        CHECK(seen_buttons[fake_frame() - 2] == 1 << 3);
        hangup(gone);
        wait_frames(20);
        CHECK(seen_buttons[fake_frame() - 2] == 0);
        ask(c, "{\"id\":27,\"cmd\":\"status\"}\n", "27", line, sizeof line, 2000);
        CHECK(strstr(line, "\"queued\":0"));
        puts("PASS: a client that disconnects mid-press: the press is let go, the bridge goes on");
    }

    /* a client that sends half a request and goes; too many clients */
    {
        Client *half = dial();
        say(half, "{\"id\":1,\"cmd\":\"sta");
        hangup(half);
        sleep_ms(50);
        Client *many[12];
        int busy = 0;
        for (int i = 0; i < 12; i++) many[i] = dial();
        for (int i = 0; i < 12; i++) {
            char got[512] = "";
            if (wait_readable(many[i], 300) > 0) {
                const long r = read_some(many[i], got, sizeof got - 1);
                if (r > 0) got[r] = 0;
            }
            busy += strstr(got, "too many clients") != NULL;
        }
        CHECK(busy >= 4);
        for (int i = 0; i < 12; i++) hangup(many[i]);
        sleep_ms(50);
        ask(c, "{\"id\":28,\"cmd\":\"status\"}\n", "28", line, sizeof line, 2000);
        CHECK(strstr(line, "\"ok\":true"));
        puts("PASS: a half request then a disconnect, and too many clients: the bridge goes on");
    }
    /* the game never waits for a silent client: one that reads nothing while presents stream to it */
    {
        Client *deaf = dial();
        say(deaf, "{\"id\":1,\"cmd\":\"subscribe\",\"every\":1,\"events\":[\"present\"]}\n");
        const int64_t f0 = fake_frame();
        wait_frames(300);
        CHECK(fake_frame() - f0 >= 300);
        hangup(deaf);
        puts("PASS: a client that never reads does not hold the game");
    }
    paused = 1;
    return 0;
}
