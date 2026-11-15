/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (hookshot) The window after a crash (host/hos_crash_dialog.c): the parts that decide, as plain C with no system calls, for
   tests/test_crash_dialog.py.

   The game starts a small helper of itself early, `hookshot --crash-watch <pid> <run id> <game folder> <log>`, which waits for
   the game to end and then reads what it left: a crash record of its run in bug-reports/pending (hos_crash_record.c), an
   unclean-end record (the game ended without its crash handler: killed, out of memory), the quit the player asked for, and
   the run's log. From those it decides whether to say anything (hos_crash_verdict), when it happened (hos_crash_stage) and
   why, where that is known (hos_crash_cause). A start that finds a crash record nobody showed (no helper could run) asks a
   helper to show it: `hookshot --crash-show <record> <game folder> <log>`. Nothing is sent anywhere: Report copies a
   redacted text and opens the issue form, and only when the player clicks it. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOS_CRASH_WATCH_ARG "--crash-watch"
#define HOS_CRASH_SHOW_ARG "--crash-show"
/* (the game-problem form, as the Insert menu's Bug report links it: gpu/vk/vk_bug_report.cpp kForm. Forgejo fills a form's field
   from the query's field:<its id>, and the title from title=) */
#define HOS_CRASH_ISSUE_FORM "https://git.notrexed.dev/Hookshot/Hookshot/issues/new?template=.github/ISSUE_TEMPLATE/1-game-problem.yml"
#define HOS_CRASH_ISSUES "https://git.notrexed.dev/Hookshot/Hookshot/issues/new/choose"
enum { HOS_CRASH_LINK_MAX = 2000, HOS_CRASH_LOG_LINES = 200 };

/* ---- the helper's command line ---- */
typedef struct HosCrashArgs {
    int mode;              /* 1 watch, 2 show */
    uint64_t pid;          /* (watch) the game's process */
    char run_id[33];       /* (watch) its run's id, 32 hex digits (hos_report_run_id): what its records carry */
    const char *record;    /* (show) a record's file name in bug-reports/pending */
    const char *dir;       /* the game folder */
    const char *log;       /* the run's log, NULL when it logged to no file ("-") */
} HosCrashArgs;

static inline int hos_crash_hex_id(const char *s) {
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return 0;
    return n == 32;
}
/* A pending record's name, crash-<UTC>.txt or unclean-<UTC>.txt (hos_crash_record.c publish: maybe -<pid>-<n> before .txt):
   1 crash, 2 unclean, 0 any other name. when: its UTC, "YYYYMMDDTHHMMSSZ" */
static inline int hos_crash_record_name(const char *name, char when[17]) {
    const int kind = !strncmp(name, "crash-", 6) ? 1 : !strncmp(name, "unclean-", 8) ? 2 : 0;
    if (!kind) return 0;
    const char *p = name + (kind == 1 ? 6 : 8);
    for (int i = 0; i < 16; i++) {
        const char c = p[i];
        if (i == 8 ? c != 'T' : i == 15 ? c != 'Z' : !(c >= '0' && c <= '9')) return 0;
    }
    const char *s = p + 16;
    if (*s == '-') {   /* (-<pid>-<n>) */
        s++;
        if (!(*s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
        if (*s++ != '-' || !(*s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
    }
    if (strcmp(s, ".txt")) return 0;
    if (when) memcpy(when, p, 16), when[16] = 0;
    return kind;
}
/* 1 watch, 2 show, 0 not a helper's command line (the game itself), -1 a helper's that is not whole: it then does nothing */
static inline int hos_crash_args(int argc, char **argv, HosCrashArgs *a) {
    memset(a, 0, sizeof *a);
    if (argc < 2 || !argv[1]) return 0;
    const int watch = !strcmp(argv[1], HOS_CRASH_WATCH_ARG), show = !strcmp(argv[1], HOS_CRASH_SHOW_ARG);
    if (!watch && !show) return 0;
    if (argc != (watch ? 6 : 5)) return -1;
    if (watch) {
        const char *p = argv[2];
        uint64_t pid = 0;
        size_t n = 0;
        for (; p[n] >= '0' && p[n] <= '9' && n < 20; n++) pid = pid * 10 + (uint64_t)(p[n] - '0');
        if (!n || p[n] || !pid || !hos_crash_hex_id(argv[3])) return -1;
        a->pid = pid;
        memcpy(a->run_id, argv[3], 33);
    } else {
        if (!hos_crash_record_name(argv[2], NULL)) return -1;   /* (a bare name: never a path) */
        a->record = argv[2];
    }
    const char *dir = argv[watch ? 4 : 3], *log = argv[watch ? 5 : 4];
    if (!dir[0] || !log[0]) return -1;
    a->dir = dir;
    a->log = strcmp(log, "-") ? log : NULL;
    a->mode = watch ? 1 : 2;
    return a->mode;
}
/* Whether the game runs under gamescope (the Steam Deck's Game Mode, or a gamescope session): GAMESCOPE_WAYLAND_DISPLAY is
   what gamescope sets for what it starts (gamescope-0), and the Deck's Game Mode session also sets XDG_CURRENT_DESKTOP=gamescope.
   (Not SteamDeck=1 alone: Steam sets that in the Deck's Desktop Mode too.) Whether gamescope shows a box of a child process
   is not tested yet, and a box nobody sees would keep the helper, and so the game in Steam's eyes, running */
static inline int hos_crash_under_gamescope(const char *gamescope_display, const char *desktop) {
    if (gamescope_display && gamescope_display[0]) return 1;
    if (!desktop) return 0;
    for (const char *p = desktop; *p;) {   /* (a list: KDE:gamescope; any case) */
        size_t n = 0;
        while (p[n] && p[n] != ':') n++;
        if (n == 9) {
            size_t k = 0;
            for (; k < 9 && (p[k] | 0x20) == "gamescope"[k]; k++) {}
            if (k == 9) return 1;
        }
        p += n + (p[n] == ':');
    }
    return 0;
}
/* Whether this start gets a helper: a game run with a window, not the build's --prepare-shaders run, not a helper itself, not
   with HOS_HEADLESS (no window: tests, frame dumps), HOS_CRASH_WATCH=0 or in a profile-guided training run (HOS_PGO: the helper
   is the same program and would run the profile's writer too), not under gamescope (hos_crash_under_gamescope: neither the
   helper nor the next start's box of an earlier crash), and only with a run id its records can be found by */
static inline int hos_crash_watch_wanted(int argc, char **argv, int headless, int switched_off, int pgo, int gamescope,
                                         const char *run_id) {
    if (headless || switched_off || pgo || gamescope || !run_id || !hos_crash_hex_id(run_id)) return 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--prepare-shaders") || !strcmp(argv[i], HOS_CRASH_WATCH_ARG) || !strcmp(argv[i], HOS_CRASH_SHOW_ARG))
            return 0;
    return 1;
}
/* One argument of a Windows command line as CommandLineToArgvW reads it back: quoted, a quote inside escaped, the backslashes
   before a quote (and before the closing one) doubled. The length written, 0 when it does not fit */
static inline size_t hos_crash_quote_arg(char *out, size_t cap, const char *arg) {
    size_t n = 0;
#define HOS_CRASH_PUT(c) do { if (n + 1 >= cap) return 0; out[n++] = (c); } while (0)
    HOS_CRASH_PUT('"');
    for (const char *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == '\\') slashes++, p++;
        if (!*p) {
            for (size_t i = 0; i < slashes * 2; i++) HOS_CRASH_PUT('\\');
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) HOS_CRASH_PUT('\\');
        } else {
            for (size_t i = 0; i < slashes; i++) HOS_CRASH_PUT('\\');
        }
        HOS_CRASH_PUT(*p);
    }
    HOS_CRASH_PUT('"');
#undef HOS_CRASH_PUT
    out[n] = 0;
    return n;
}

/* ---- what the run's log says (each line in turn) ---- */
typedef struct HosCrashLog {
    int map, preparing, prepared;   /* the stage: a map line (host/hos_main.c, a save loaded), the shaders' preparation */
    int files, vulkan, window, reserve, memory, low_memory, device_lost;
    char version[128], gpu[160], driver[160], devices[240], dlss[240], fsr4[240];
} HosCrashLog;
static inline void hos_crash_take(char *dst, size_t cap, const char *from) {
    size_t n = 0;
    while (from[n] && from[n] != '\n' && from[n] != '\r' && n + 1 < cap) n++;
    memcpy(dst, from, n);
    dst[n] = 0;
}
/* A line of the log (without its home folder: hos_crash_redact_line first, for what is kept of it) */
static inline void hos_crash_log_line(HosCrashLog *f, const char *line) {
    const char *at;
    /* (the stage) */
    if (strstr(line, "] map: ")) f->map = 1;
    if (strstr(line, "] vk: start-up: the shaders took")) f->prepared = 1;
    else if (strstr(line, "] vk: start-up: ") || strstr(line, "] vk: precompile") || strstr(line, "] vk: preparation: ") ||
             strstr(line, "] vk: prepared "))
        f->preparing = 1;
    /* (a start that cannot go on: host/hos_main.c's own lines, written straight to the log) */
    if (!strncmp(line, "cannot open ", 12) || !strncmp(line, "usage: ", 7) || strstr(line, ": lift and image do not match") ||
        strstr(line, "] rx: mkwvk_init version mismatch") || strstr(line, " has no mkwvk_init"))
        f->files = 1;
    if (!strncmp(line, "cannot reserve the guest window", 31)) f->reserve = 1;
    /* (the Vulkan back end did not start: host/rx_backend.c, gpu/vk/vk_main.cpp, host/posix/display_sdl.c) */
    if (strstr(line, "] rx: RX_BACKEND=vulkan, but ") || strstr(line, "] vk: load_global failed") ||
        strstr(line, "] vk: vkCreateInstance failed") || strstr(line, "] vk: no Vulkan 1.3 GPU") ||
        strstr(line, "] vk: create_device failed") || strstr(line, "] rx: the Vulkan back end could not start") ||
        strstr(line, "] display: the renderer did not start"))
        f->vulkan = 1;
    if (strstr(line, "] display: no SDL") || strstr(line, "] display: SDL_Init: ") || strstr(line, "] display: SDL_CreateWindow: ") ||
        ((at = strstr(line, "] display: ")) && strstr(at, " failed (error ")))
        f->window = 1;
    if (strstr(line, "] mem: Windows is out of memory") || ((at = strstr(line, "] mem: commit of ")) && strstr(at, " failed (error ")))
        f->memory = 1;
    if (strstr(line, "] boot: LOW MEMORY")) f->low_memory = 1;
    if (strstr(line, "device lost") || strstr(line, "VK_ERROR_DEVICE_LOST")) f->device_lost = 1;
    /* (what the report names) */
    if (!f->version[0] && (at = strstr(line, "] hookshot "))) hos_crash_take(f->version, sizeof f->version, at + 11);
    if ((at = strstr(line, "] vk: driver: "))) hos_crash_take(f->driver, sizeof f->driver, at + 14);
    if ((at = strstr(line, "] vk: ")) && strstr(at, ", Vulkan ") && strstr(at, "; mesh shaders ")) {   /* (the device chosen) */
        const char *name = at + 6, *end = strstr(name, ", Vulkan ");
        const size_t n = (size_t)(end - name) < sizeof f->gpu ? (size_t)(end - name) : sizeof f->gpu - 1;
        memcpy(f->gpu, name, n), f->gpu[n] = 0;
    }
    if ((at = strstr(line, "] vk: device ")) && strstr(at, ", Vulkan ")) {   /* (every device listed) */
        const char *name = strchr(at + 13, ':');
        const char *end = name ? strstr(name, ", Vulkan ") : NULL;
        const size_t used = strlen(f->devices);
        if (name && end && end > name + 2 && used + (size_t)(end - name) + 2 < sizeof f->devices) {
            if (used) memcpy(f->devices + used, "; ", 3);
            strncat(f->devices, name + 2, (size_t)(end - name - 2));
        }
    }
    if (!f->dlss[0] && (at = strstr(line, "] vk: NVIDIA DLSS: "))) hos_crash_take(f->dlss, sizeof f->dlss, at + 6);
    if (!f->fsr4[0] && (at = strstr(line, "] vk: AMD FSR 4: "))) hos_crash_take(f->fsr4, sizeof f->fsr4, at + 6);
}
/* (HOS_STAGE_UNKNOWN: the run's log was not found (it logged to a terminal or a program's pipe, or has been rotated away since):
   the window then says nothing about when, rather than "during the start" for a crash in play) */
enum { HOS_STAGE_START, HOS_STAGE_PREPARING, HOS_STAGE_PLAY, HOS_STAGE_UNKNOWN };
static inline int hos_crash_stage(const HosCrashLog *f) {
    return f->map ? HOS_STAGE_PLAY : f->preparing && !f->prepared ? HOS_STAGE_PREPARING : HOS_STAGE_START;
}
/* (a start that stopped before play for a reason the log names) */
static inline int hos_crash_start_failed(const HosCrashLog *f) {
    return !f->map && (f->files || f->vulkan || f->window || f->reserve);
}

/* ---- a pending record (hos_crash_record.c format_record): its "key: value" lines ---- */
typedef struct HosCrashRecord {
    int kind;                 /* 0 none, 1 crash, 2 unclean */
    int quit;                 /* (unclean) the player had asked to quit: "quit_asked: yes" */
    uint32_t code;            /* exception: 0x... */
    int signal_number;
    char reason[40], module[160], run_id[33], when[17], uptime[24];
} HosCrashRecord;
static inline int hos_crash_record_value(const char *text, const char *key, char *out, size_t cap) {
    const size_t k = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (!strncmp(p, key, k) && p[k] == ':' && p[k + 1] == ' ') {
            hos_crash_take(out, cap, p + k + 2);
            return 1;
        }
    }
    if (cap) out[0] = 0;
    return 0;
}
static inline void hos_crash_record_parse(const char *text, HosCrashRecord *r) {
    memset(r, 0, sizeof *r);
    if (!strncmp(text, "Hookshot crash record 1\n", 24)) r->kind = 1;
    else if (!strncmp(text, "Hookshot unclean record 1\n", 26)) r->kind = 2;
    else return;
    char v[40];
    hos_crash_record_value(text, "reason", r->reason, sizeof r->reason);
    hos_crash_record_value(text, "module_offset", r->module, sizeof r->module);
    hos_crash_record_value(text, "run_id", r->run_id, sizeof r->run_id);
    hos_crash_record_value(text, "when", r->when, sizeof r->when);
    hos_crash_record_value(text, "uptime_s", r->uptime, sizeof r->uptime);
    if (hos_crash_record_value(text, "exception", v, sizeof v)) r->code = (uint32_t)strtoul(v, NULL, 16);
    if (hos_crash_record_value(text, "signal", v, sizeof v)) r->signal_number = atoi(v);
    r->quit = hos_crash_record_value(text, "quit_asked", v, sizeof v) && !strcmp(v, "yes");
}
/* A crash in the graphics driver's own code: the record's module (a file name, never a path) */
static inline int hos_crash_driver_module(const char *module) {
    static const char *const drivers[] = {"nvoglv64", "nvgpucomp", "nvvk", "amdvlk", "amdxc", "igvk", "igc", "igd", "libnvidia-",
                                          "libvulkan_", "libGLX_nvidia", "libamdvlk"};
    for (size_t i = 0; i < sizeof drivers / sizeof drivers[0]; i++) {
        const size_t n = strlen(drivers[i]);
        size_t k = 0;
        for (; k < n && module[k]; k++) {
            char c = module[k];
            if (c >= 'A' && c <= 'Z' && drivers[i][k] >= 'a') c = (char)(c + 32);
            if (c != drivers[i][k]) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

/* ---- was it a crash ---- */
enum { HOS_CRASH_QUIET, HOS_CRASH_SHOW_CRASH, HOS_CRASH_SHOW_UNCLEAN, HOS_CRASH_SHOW_START };
/* What the helper says once the game has ended. A crash record of the run: it crashed, whatever came before it, a quit
   included (as the renderer's crash markers count a crash after a quit: PR 498). A start the log says could not go on (its
   files, Vulkan, the window, memory) before a save was loaded: that, a quit included (the black window closed). An unclean
   end, the run's marker left with no crash record (killed, out of memory, a handler that could not run): only when the
   player had not asked to quit (a quit never counts, however the process ends after it: a second close of a window that
   stopped answering while it closes, PR 498) and, on Windows, the exit code is not 0. Anything else (the marker removed:
   a quit, a return, a restart) says nothing; nor does a run another took the place of (Linux's restart: the same process). */
static inline int hos_crash_verdict(const HosCrashRecord *r, const HosCrashLog *f, int have_exit, unsigned long exit_code,
                                    int replaced) {
    if (replaced) return HOS_CRASH_QUIET;
    if (r->kind == 1) return HOS_CRASH_SHOW_CRASH;
    if (hos_crash_start_failed(f)) return HOS_CRASH_SHOW_START;
    if (r->kind == 2 && !r->quit && !(have_exit && exit_code == 0)) return HOS_CRASH_SHOW_UNCLEAN;
    return HOS_CRASH_QUIET;
}
enum { HOS_CAUSE_UNKNOWN, HOS_CAUSE_FILES, HOS_CAUSE_VULKAN, HOS_CAUSE_WINDOW, HOS_CAUSE_MEMORY, HOS_CAUSE_GPU_DRIVER,
       HOS_CAUSE_ENDED };
/* Why, where the record or the log says: the first that holds of the game's files, Vulkan, the window (before play), memory,
   the graphics driver; an unclean end with nothing else known was ended from outside (or out of memory, when the start
   warned of low memory); else not known */
static inline int hos_crash_cause(int verdict, const HosCrashRecord *r, const HosCrashLog *f, int have_exit, unsigned long exit_code) {
    if (f->files && !f->map) return HOS_CAUSE_FILES;
    if (f->vulkan && !f->map) return HOS_CAUSE_VULKAN;
    if (f->window && !f->map) return HOS_CAUSE_WINDOW;
    if (f->reserve || f->memory || r->code == 0xC0000017u || (have_exit && exit_code == 0xC0000017ul)) return HOS_CAUSE_MEMORY;
    if (!strcmp(r->reason, "device lost") || !strcmp(r->reason, "Vulkan fatal") || f->device_lost ||
        (r->kind == 1 && hos_crash_driver_module(r->module)))
        return HOS_CAUSE_GPU_DRIVER;
    if (verdict == HOS_CRASH_SHOW_UNCLEAN) return f->low_memory ? HOS_CAUSE_MEMORY : HOS_CAUSE_ENDED;
    return HOS_CAUSE_UNKNOWN;
}
static inline const char *hos_crash_title(int verdict) {
    return verdict == HOS_CRASH_SHOW_START ? "Hookshot couldn't start" : "Hookshot closed unexpectedly";
}
static inline const char *hos_crash_stage_text(int stage) {
    return stage == HOS_STAGE_PLAY ? "while you were playing" : stage == HOS_STAGE_PREPARING ? "while it prepared the shaders"
         : stage == HOS_STAGE_UNKNOWN ? "" : "during the start";
}
/* What happened and when, as a sentence without its full stop: "Hookshot closed unexpectedly while you were playing" */
static inline void hos_crash_headline(char *out, size_t cap, const char *what, int stage) {
    const char *when = hos_crash_stage_text(stage);
    snprintf(out, cap, "%s%s%s", what, when[0] ? " " : "", when);
}
static inline const char *hos_crash_cause_text(int cause) {
    switch (cause) {
    case HOS_CAUSE_FILES:
        return "Hookshot's game files are missing or don't match each other. Build the game again with Hookshot Builder.";
    case HOS_CAUSE_VULKAN:
        return "Vulkan didn't start on your graphics card. Update your graphics driver: Hookshot needs a graphics card and "
               "driver with Vulkan 1.3.";
    case HOS_CAUSE_WINDOW: return "Hookshot couldn't open its window. Restart your PC, and update your graphics driver.";
    case HOS_CAUSE_MEMORY:
        return "Your PC ran out of memory. Close other programs and try again; Hookshot needs 16 GB of RAM, and on Windows a "
               "page file Windows manages.";
    case HOS_CAUSE_GPU_DRIVER:
        return "The graphics driver failed. Update your graphics driver, and turn off overlays and tools hooked into the game.";
    case HOS_CAUSE_ENDED:
        return "Hookshot ended without a crash report: it was closed from outside (Task Manager, a kill), or the system "
               "stopped it, often for running out of memory.";
    default: return "An unexpected error.";
    }
}
/* The window's text: what happened and when, why, and what Report does */
static inline size_t hos_crash_message(char *out, size_t cap, int verdict, int stage, int cause) {
    char headline[160];
    hos_crash_headline(headline, sizeof headline, verdict == HOS_CRASH_SHOW_START ? "Hookshot stopped" : "Hookshot closed unexpectedly",
                       stage);
    const int n = snprintf(out, cap,
                           "%s.\n\nWhat went wrong: %s\n\n"
                           "Report copies a report to your clipboard (the error, your PC's hardware and the end of the log, "
                           "with your home folder and user name taken out) and opens Hookshot's issue form in your browser, "
                           "for you to paste it into. Hookshot itself sends nothing.",
                           headline, hos_crash_cause_text(cause));
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}

/* ---- privacy: the same rules as the Insert menu's Bug report (gpu/vk/vk_bug_report.cpp redact, value_shown) ---- */
static inline int hos_crash_sep(char c) { return c == '/' || c == '\\'; }
static inline int hos_crash_name_char(char c) {
    const unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '.' || c == '_' || c == '-' || u >= 0x80;
}
static inline int hos_crash_matches_at(const char *s, size_t n, size_t at, const char *what, size_t wn, int windows) {
    if (!wn || at + wn > n) return 0;
    for (size_t i = 0; i < wn; i++) {
        char a = s[at + i], b = what[i];
        if (hos_crash_sep(a) && hos_crash_sep(b)) {
            if (!windows && a != b) return 0;
            continue;
        }
        if (windows) {
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        }
        if (a != b) return 0;
    }
    return 1;
}
/* A log line with the home folder written ~ (%USERPROFILE% on Windows) and the user's name as a folder <user>: the menu's
   redact(), the same output for the same line. The length written (cut to cap) */
static inline size_t hos_crash_redact_line(char *out, size_t cap, const char *line, size_t n, const char *home, const char *user,
                                           int windows) {
    size_t hn = home ? strlen(home) : 0, un = user ? strlen(user) : 0, o = 0;
    while (hn > 1 && hos_crash_sep(home[hn - 1])) hn--;
    const int with_home = hn > 1;
    if (!cap) return 0;
#define HOS_CRASH_OUT(s, k) do { for (size_t j_ = 0; j_ < (k) && o + 1 < cap; j_++) out[o++] = (s)[j_]; } while (0)
    for (size_t i = 0; i < n;) {
        if (with_home && (i == 0 || (!hos_crash_name_char(line[i - 1]) && !hos_crash_sep(line[i - 1]))) &&
            hos_crash_matches_at(line, n, i, home, hn, windows) && (i + hn >= n || !hos_crash_name_char(line[i + hn]))) {
            const char *w = windows ? "%USERPROFILE%" : "~";
            HOS_CRASH_OUT(w, strlen(w));
            i += hn;
            continue;
        }
        if (un && i > 0 && hos_crash_sep(line[i - 1]) && hos_crash_matches_at(line, n, i, user, un, windows) &&
            (i + un >= n || !hos_crash_name_char(line[i + un]))) {
            HOS_CRASH_OUT("<user>", 6);
            i += un;
            continue;
        }
        HOS_CRASH_OUT(line + i, 1);
        i++;
    }
#undef HOS_CRASH_OUT
    out[o] = 0;
    return o;
}
/* Whether a setting's value goes into the report: a number of at most 12 characters (60, -1, 1.5), a size (1920x1080), or
   on/off, true/false, yes/no, or empty; and, for a switch (name not NULL), never for a name that may hold a secret, a place or
   a person (TOKEN, KEY, ROOM, SECRET, PASS, SERVER or USER in it). The menu's value_shown; every other value is left out */
static inline int hos_crash_value_shown(const char *name, const char *value) {
    if (name) {
        static const char *const words[] = {"TOKEN", "KEY", "ROOM", "SECRET", "PASS", "SERVER", "USER"};
        char upper[128];
        size_t n = 0;
        for (; name[n] && n + 1 < sizeof upper; n++) upper[n] = (name[n] >= 'a' && name[n] <= 'z') ? (char)(name[n] - 32) : name[n];
        upper[n] = 0;
        for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
            if (strstr(upper, words[i])) return 0;
    }
    static const char *const plain[] = {"", "on", "off", "true", "false", "yes", "no"};
    const size_t vn = strlen(value);
    if (vn <= 5) {
        char lower[8];
        for (size_t i = 0; i <= vn; i++) lower[i] = (value[i] >= 'A' && value[i] <= 'Z') ? (char)(value[i] + 32) : value[i];
        for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++)
            if (!strcmp(lower, plain[i])) return 1;
    }
    if (vn > 12) return 0;
    size_t at = value[0] == '-' ? 1 : 0, from = at;
    while (value[at] >= '0' && value[at] <= '9') at++;
    if (at == from) return 0;
    if (!value[at]) return 1;
    const char mid = value[at++];
    if (mid != '.' && !(mid == 'x' && value[0] != '-')) return 0;
    from = at;
    while (value[at] >= '0' && value[at] <= '9') at++;
    return at > from && !value[at];
}

/* ---- the issue form's link ---- */
static inline size_t hos_crash_url_part(char *out, size_t cap, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    if (!cap) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const int plain = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' ||
                          *p == '_' || *p == '.' || *p == '~';
        if (n + (plain ? 1 : 3) >= cap) break;
        if (plain) out[n++] = (char)*p;
        else out[n++] = '%', out[n++] = hex[*p >> 4], out[n++] = hex[*p & 15];
    }
    out[n] = 0;
    return n;
}
/* The game-problem form with the title and the one-line answers filled in, each only while the whole link stays within
   HOS_CRASH_LINK_MAX (the menu's kLinkMax: what the sign-in page's redirect keeps): the version first, then the GPU, the
   system, the title and what happened. Never the report itself: the player pastes that */
static inline size_t hos_crash_issue_url(char *out, size_t cap, const char *title, const char *version, const char *gpu,
                                         const char *system, const char *what) {
    const char *keys[] = {"&field:version=", "&field:gpu=", "&field:system=", "&title=", "&field:what="};
    const char *values[] = {version, gpu, system, title, what};
    const size_t max = cap < HOS_CRASH_LINK_MAX + 1 ? cap : HOS_CRASH_LINK_MAX + 1;
    size_t n = strlen(HOS_CRASH_ISSUE_FORM);
    if (n + 1 > max) return 0;
    memcpy(out, HOS_CRASH_ISSUE_FORM, n + 1);
    char part[HOS_CRASH_LINK_MAX + 1];
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (!values[i] || !values[i][0]) continue;
        const size_t k = strlen(keys[i]);
        if (k >= sizeof part) continue;
        memcpy(part, keys[i], k);
        const size_t v = hos_crash_url_part(part + k, sizeof part - k, values[i]);
        if (!v || n + k + v + 1 > max) continue;   /* (left out: the form asks for it) */
        memcpy(out + n, part, k + v + 1);
        n += k + v;
    }
    return n;
}

/* ---- Linux's clipboard: a tool that keeps serving the text once the helper has ended (a Wayland selection is only taken from
   a client with the keyboard's focus and dies with its process; X11's, with its owner), in turn: wl-copy on Wayland, then
   xclip and xsel on X11 (XWayland too). The text goes on the tool's input. NULL past the last */
static inline const char *const *hos_crash_clip_tool(int wayland, int x11, int i) {
    static const char *const wl[] = {"wl-copy", NULL};
    static const char *const xclip[] = {"xclip", "-selection", "clipboard", "-in", NULL};
    static const char *const xsel[] = {"xsel", "--clipboard", "--input", NULL};
    const char *const *all[3];
    int n = 0;
    if (wayland) all[n++] = wl;
    if (x11) all[n++] = xclip, all[n++] = xsel;
    return i >= 0 && i < n ? all[i] : NULL;
}
/* The box after Report: whether the report is on the clipboard (copied: 0 not, 1 kept there by the system or a tool, 2 by this
   helper only while it runs: X11 through SDL), whether the issue form opened, and the saved report's path (NULL when it could
   not be saved). Never says it was copied when it was not; with nothing copied, the saved file is the way to it */
static inline size_t hos_crash_ready_text(char *out, size_t cap, int copied, int opened, const char *saved, const char *issues) {
    const int n = snprintf(out, cap, "%s%s%s%s%s%s",
        copied ? "The report is on your clipboard. "
        : saved ? "The report could not be put on the clipboard. Click Open the report, select all of it and copy it. "
                : "The report could not be put on the clipboard, nor saved. ",
        opened ? "In the issue form that opened in your browser, paste it into the hookshot_log.txt box, say what you were doing, "
                 "and fill in the rest."
               : "Open ",
        opened ? "" : issues,
        opened ? "" : ", choose Game problem, and paste it into the hookshot_log.txt box.",
        saved ? "\n\nIt is saved as " : "", saved ? saved : "");
    size_t used = n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
    const char *tail = saved ? "." : "";
    const char *keep = copied == 2 ? "\n\nKeep this window open until you have pasted it." : "";
    if (used < cap) {
        const int m = snprintf(out + used, cap - used, "%s%s", tail, keep);
        if (m > 0) used += (size_t)m < cap - used ? (size_t)m : cap - used - 1;
    }
    return used;
}

/* ---- which records were shown (bug-reports/shown.txt: a name a line) ---- */
static inline int hos_crash_listed(const char *list, const char *name) {
    const size_t k = strlen(name);
    for (const char *p = list; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, name, k) && (p[k] == '\n' || p[k] == '\r' || !p[k])) return 1;
    return 0;
}
/* Seconds since 1970 of a record's UTC ("YYYYMMDDTHHMMSSZ"): days from the civil date (H. Hinnant's algorithm) */
static inline int64_t hos_crash_epoch(const char when[17]) {
    int v[6], at[6] = {0, 4, 6, 9, 11, 13}, len[6] = {4, 2, 2, 2, 2, 2};
    for (int i = 0; i < 6; i++) {
        v[i] = 0;
        for (int k = 0; k < len[i]; k++) v[i] = v[i] * 10 + (when[at[i] + k] - '0');
    }
    int y = v[0] - (v[1] <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (v[1] + (v[1] > 2 ? -3 : 9)) + 2) / 5 + v[2] - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + v[3] * 3600 + v[4] * 60 + v[5];
}
/* Whether a start shows a record no helper showed (its helper could not run, or was ended with the game): a crash record,
   not an unclean end (a game a launcher or a logout stopped leaves one of those, and is no crash), a day old at most, and
   not listed as shown */
static inline int hos_crash_show_at_start(const char *name, const char *shown, int64_t now) {
    char when[17];
    if (hos_crash_record_name(name, when) != 1 || hos_crash_listed(shown, name)) return 0;
    const int64_t t = hos_crash_epoch(when);
    return t <= now + 300 && now - t <= 86400;
}
