/* The game folder's two folders of what the game makes as it runs (game_layout.h):
   - cache/: the shader and pipeline caches (the program cache, the pipeline notes and lists, the Vulkan pipeline cache and its
     play files, the prepare's record, the driver's faults) and the button prompts made from the player's controls (cache/prompts,
     host/prompts.c). Made for this build and this GPU, several GB: its name says it can be deleted, and the next start makes it
     again;
   - logs/: hookshot_log.txt and the runs before it, and the other logs and traces a run writes.
   Both are made at the start, before anything writes there; the renderer finds them by gpu/vk/vk_paths.h's cache_path and
   log_path. What a build from before left loose beside the game is moved into them once: renames in the same folder, never over
   a file already there (one that cannot move stays, and the log says so). The Vulkan pipeline cache is renamed on the way, from
   mkw-recomp's mkw_vk_pipelines*.bin to hookshot_vk_pipelines*.bin, and what a write of it cut short left (a .tmp) is deleted;
   the button prompts an older build made in prompts/ are deleted, as they are made again in cache/prompts. The build's own files
   stay where the build put them (image.bin, romfs, hookshot_shaders.bin, the shipped pipeline list, prompts/<face>/, language.txt),
   and so do the settings (hookshot_options.ini, hookshot_env.txt) and the saves (user/). */
#include "hos_paths.h"   /* (first: windows.h, host/posix's on Linux) */
#include <stdio.h>
#include <string.h>
#include "game_layout.h"

#ifdef _WIN32
#define SEP_S "\\"
#else
#define SEP_S "/"
#endif

void hos_log(const char *fmt, ...);   /* (hos.h; declared here so tests/test_game_layout.py builds this file alone) */

/* The logs into logs/, the caches into cache/ */
static const char *const to_logs[] = {
    "hookshot_log.txt", "hookshot_log.1.txt", "hookshot_log.2.txt", "hookshot_log.3.txt", "hookshot_log.4.txt",
    "hookshot_startup.log", "hookshot_frames.csv",
};
static const char *const to_cache[] = {
    "hookshot_programs.bin", "hookshot_programs.idx", "hookshot_programs.zst.bin", "hookshot_programs.zst.idx",
    "hookshot_module_ids.bin", "hookshot_pipelines.bin", "hookshot_pipelines.bin.predicted",
    "hookshot_pipelines.bin.predicted_keys", "hookshot_pipelines.bin.built", "hookshot_pipelines.bin.running",
    "hookshot_pipelines.bin.predicting", "hookshot_pipelines.bin.built.warming", "hookshot_computes.bin",
    "hookshot_compute_variants.bin", "hookshot_pipeline_states.bin", "hookshot_pipeline_states_v1.bin", "hookshot_prepared.bin",
    "hookshot_prepared.bin.warming", "hookshot_prepare_skipped", "hookshot_driver_faults.txt", "hookshot_load_start.txt",
    "hookshot_load_start.txt.waited",
};
/* The Vulkan pipeline cache, mkw_vk_pipelines.bin, its stamp and its play files mkw_vk_pipelines_play_<n>.bin (mkw-recomp's
   names), renamed hookshot_vk_pipelines* as they move into cache/ (gpu/vk/vk_draw.cpp, pcache_stamp.h) */
static const char old_pcache[] = "mkw_vk_pipelines", new_pcache[] = "hookshot_vk_pipelines";
/* The program cache's lock files beside it (gpu/vk/vk_progcache.cpp): the game locks cache/'s now, and nothing holds these: deleted */
static const char *const old_locks[] = {"hookshot_programs.bin.lock", "hookshot_programs.zst.bin.lock"};
/* Made from the player's choices and made again when missing (host/prompts.c): deleted from the game folder */
static const char *const made_again[] = {
    "prompts" SEP_S "Common.bntx.zs", "prompts" SEP_S "Common.blarc.zs", "prompts" SEP_S "Common.key",
    "prompts" SEP_S "Common.blarc.key", "prompts" SEP_S "External.bfotf.zs", "prompts" SEP_S "rsizetable.mod.zs",
};

static int done, moved, kept;
static char kept_first[MAX_PATH];

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int is_dir(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* game's file name into dir (both with their separator) as as: never over one there, never a folder */
static void move_as(const char *game, const char *name, const char *dir, const char *as) {
    char from[MAX_PATH], to[MAX_PATH];
    if (snprintf(from, sizeof from, "%s%s", game, name) >= (int)sizeof from || !exists(from) || is_dir(from)) return;
    if (snprintf(to, sizeof to, "%s%s", dir, as) >= (int)sizeof to) return;
    if (!exists(to) && MoveFileExA(from, to, 0)) {
        moved++;
        return;
    }
    if (!kept++) snprintf(kept_first, sizeof kept_first, "%s", from);
}

/* the game folder's files whose names start with old_pcache and end with suffix: each renamed into cache (rename: 1) or deleted */
static void each_old_pcache(const char *game, const char *cache, const char *suffix, int rename) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s%s*%s", game, old_pcache, suffix);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strncmp(fd.cFileName, old_pcache, sizeof old_pcache - 1)) continue;
        if (rename) {
            char as[MAX_PATH];
            snprintf(as, sizeof as, "%s%s", new_pcache, fd.cFileName + sizeof old_pcache - 1);
            move_as(game, fd.cFileName, cache, as);
        } else {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s%s", game, fd.cFileName);
            DeleteFileA(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void hos_game_layout(void) {
    if (done) return;
    done = 1;
    char game[MAX_PATH], cache[MAX_PATH], logs[MAX_PATH], prompts[MAX_PATH];
    DWORD n = hos_game_path_a(game, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    while (n && game[n - 1] != '\\' && game[n - 1] != '/') n--;
    if (!n) return;
    game[n] = 0;   /* (the game folder, with its separator) */
    if (snprintf(cache, sizeof cache, "%scache" SEP_S, game) >= (int)sizeof cache ||
        snprintf(logs, sizeof logs, "%slogs" SEP_S, game) >= (int)sizeof logs ||
        snprintf(prompts, sizeof prompts, "%scache" SEP_S "prompts", game) >= (int)sizeof prompts)
        return;
    CreateDirectoryA(cache, NULL);
    CreateDirectoryA(logs, NULL);
    CreateDirectoryA(prompts, NULL);
    for (size_t i = 0; i < sizeof to_logs / sizeof *to_logs; i++) move_as(game, to_logs[i], logs, to_logs[i]);
    for (size_t i = 0; i < sizeof to_cache / sizeof *to_cache; i++) move_as(game, to_cache[i], cache, to_cache[i]);
    each_old_pcache(game, cache, ".bin", 1);
    move_as(game, "mkw_vk_pipelines.stamp", cache, "hookshot_vk_pipelines.stamp");   /* (which translator made them) */
    each_old_pcache(game, cache, ".tmp", 0);     /* (a write cut short: never read) */
    each_old_pcache(game, cache, ".tmp-*", 0);   /* (one with its writer's own name, gpu/vk/pipeline_cache_file.h) */
    for (size_t i = 0; i < sizeof old_locks / sizeof *old_locks; i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s%s", game, old_locks[i]);
        DeleteFileA(path);
    }
    for (size_t i = 0; i < sizeof made_again / sizeof *made_again; i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s%s", game, made_again[i]);
        DeleteFileA(path);
    }
}

void hos_game_layout_log(void) {
    if (moved) hos_log("game folder: %d logs and shader caches moved into its logs and cache folders", moved);
    if (kept) hos_log("game folder: %d files stayed where they were (one of that name was there already): %s, ...", kept, kept_first);
}
