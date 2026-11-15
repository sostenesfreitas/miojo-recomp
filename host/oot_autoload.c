/* (hookshot) The Insert menu's Skip title screen row (gpu/vk/vk_menu.cpp): start with the most recent save instead of the
   title screen.

   The remake has one save the player chooses (its slot_NN folders are the game's own autosave slots). The title screen and
   its file list need a press of A to carry on with that save, so the host does not choose a slot: the game loads its save.
   The newest progress.sav is looked up to know that there is one (a first launch is left on the title screen, where a new
   game can start) and is logged.

   The presses are the host's, in host/nn_hid.c's input script syntax (hos_hid_tap), added to whatever the player and a
   test's script do. They begin 20 s after the game's first controller read (the intro and title), every 2 s, and stop as
   soon as the player is running (host/hos_main.c: the lifted player's movement) or after 120 s. */
#include "oot_autoload.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_OPT_SKIP_TITLE) */
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#define SEP_S "\\"
#else
#include <dirent.h>
#define SEP_S "/"
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#endif

extern const char *hos_user_dir;   /* (host/nn_fs.c) */
extern void hos_log(const char *fmt, ...);
extern int64_t hos_now_ns(void);   /* (host/hos_main.c) */
extern int rx_option(int id, int otherwise);   /* (host/rx_backend.c) */
extern int hos_oot_player_seen(void);   /* (host/hos_main.c) */
extern void hos_hid_tap(const char *presses);   /* (host/nn_hid.c) */

/* (the wait before the first press, the gap between presses, and when to give up: the intro and title take about 23 s on
   the supported build, and preparing the shaders comes before it) */
enum { kFirstMs = 20000, kEveryMs = 2000, kGiveUpMs = 120000 };

static uint64_t file_time(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (uint64_t)st.st_mtime : 0;
}

/* A path built a part at a time, cut to fit (the game's folder can be long; nothing here may leave the buffer). */
static void path_part(char *path, size_t size, const char *part) {
    if (!path || !size) return;
    size_t used = strlen(path);
    if (used + 1 >= size) return;
    size_t copy = strlen(part);
    if (copy > size - used - 1) copy = size - used - 1;
    memcpy(path + used, part, copy);
    path[used + copy] = 0;
}
static void path_copy(char *path, size_t size, const char *text) {
    if (!path || !size) return;
    path[0] = 0, path_part(path, size, text);
}

#ifdef _WIN32
/* The newest progress.sav in one profile's slots: the game writes a new save to a slot of its own choosing, so the file's
   mtime is the save's time. */
static uint64_t profile_save(const char *user_dir, const char *profile, char *path, size_t size) {
    char pattern[MAX_PATH];
    path_copy(pattern, sizeof pattern, user_dir);
    path_part(pattern, sizeof pattern, SEP_S "save" SEP_S);
    path_part(pattern, sizeof pattern, profile);
    path_part(pattern, sizeof pattern, SEP_S "slot_*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t best = 0;
    char best_path[MAX_PATH] = "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char progress[MAX_PATH];
        path_copy(progress, sizeof progress, user_dir);
        path_part(progress, sizeof progress, SEP_S "save" SEP_S);
        path_part(progress, sizeof progress, profile);
        path_part(progress, sizeof progress, SEP_S);
        path_part(progress, sizeof progress, fd.cFileName);
        path_part(progress, sizeof progress, SEP_S "progress.sav");
        const uint64_t time = file_time(progress);
        if (time > best) {
            best = time;
            path_copy(best_path, sizeof best_path, progress);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    path_copy(path, size, best_path);
    return best;
}

uint64_t hos_oot_newest_save(const char *user_dir, char *path, size_t size) {
    if (!user_dir || !*user_dir) return 0;
    char pattern[MAX_PATH];
    path_copy(pattern, sizeof pattern, user_dir);
    path_part(pattern, sizeof pattern, SEP_S "save" SEP_S "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t best = 0;
    char best_path[MAX_PATH] = "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        char found[MAX_PATH];
        const uint64_t time = profile_save(user_dir, fd.cFileName, found, sizeof found);
        if (time > best) {
            best = time;
            path_copy(best_path, sizeof best_path, found);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    path_copy(path, size, best_path);
    return best;
}
#else
/* (Linux: the same, with the POSIX calls the rest of the host uses) */
static uint64_t profile_save(const char *user_dir, const char *profile, char *path, size_t size) {
    char dir[MAX_PATH];
    path_copy(dir, sizeof dir, user_dir);
    path_part(dir, sizeof dir, SEP_S "save" SEP_S);
    path_part(dir, sizeof dir, profile);
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t best = 0;
    char best_path[MAX_PATH] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "slot_", 5)) continue;
        char progress[MAX_PATH];
        path_copy(progress, sizeof progress, dir);
        path_part(progress, sizeof progress, SEP_S);
        path_part(progress, sizeof progress, e->d_name);
        path_part(progress, sizeof progress, SEP_S "progress.sav");
        const uint64_t time = file_time(progress);
        if (time > best) {
            best = time;
            path_copy(best_path, sizeof best_path, progress);
        }
    }
    closedir(d);
    path_copy(path, size, best_path);
    return best;
}

uint64_t hos_oot_newest_save(const char *user_dir, char *path, size_t size) {
    if (!user_dir || !*user_dir) return 0;
    char dir[MAX_PATH];
    path_copy(dir, sizeof dir, user_dir);
    path_part(dir, sizeof dir, SEP_S "save");
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t best = 0;
    char best_path[MAX_PATH] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char found[MAX_PATH];
        const uint64_t time = profile_save(user_dir, e->d_name, found, sizeof found);
        if (time > best) {
            best = time;
            path_copy(best_path, sizeof best_path, found);
        }
    }
    closedir(d);
    path_copy(path, size, best_path);
    return best;
}
#endif

static int started, active;
static int64_t start_ms, next_ms;
static char save_path[MAX_PATH];

void hos_oot_autoload_reset(void) {
    started = active = 0;
    start_ms = next_ms = 0;
    save_path[0] = 0;
}

void hos_oot_autoload_input(void) {
    const int64_t now = hos_now_ns() / 1000000;
    if (!started) {
        started = 1;
        start_ms = now, next_ms = now + kFirstMs;
        if (!rx_option(HOS_OPT_SKIP_TITLE, 0)) return;
        if (!hos_oot_newest_save(hos_user_dir, save_path, sizeof save_path)) {
            hos_log("skip title: no saved game yet: the title screen stays");
            return;
        }
        active = 1;
        hos_log("skip title: %s is the most recent save: pressing A from %.0f s", save_path, kFirstMs / 1000.0);
    }
    if (!active) return;
    if (!rx_option(HOS_OPT_SKIP_TITLE, 0)) {
        hos_log("skip title: the row was turned off while waiting");
        active = 0;
        return;
    }
    if (rx_option(1, 0)) return;   /* (the Insert menu is open: its presses are its own) */
    if (hos_oot_player_seen()) {
        hos_log("skip title: loaded, the player is running");
        active = 0;
        return;
    }
    if (now - start_ms >= kGiveUpMs) {
        hos_log("skip title: gave up after %.0f s", kGiveUpMs / 1000.0);
        active = 0;
        return;
    }
    if (now < next_ms) return;
    hos_hid_tap("0:A/200");
    next_ms = now + kEveryMs;
}
