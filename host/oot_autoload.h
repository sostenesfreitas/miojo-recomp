/* (hookshot) The Insert menu's Skip title screen row: the most recent save, and the presses that load it (oot_autoload.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* The newest progress.sav under user_dir/save/<profile>/slot_*, as its mtime in seconds since the epoch (0: none). path,
   when given, is set to the file found. */
uint64_t hos_oot_newest_save(const char *user_dir, char *path, size_t size);

/* Called at each controller read while the game starts (host/nn_hid.c, read_input). Reads the option once, and while it is
   on and a save exists, taps A through the title screen and its file list until the player is running. */
void hos_oot_autoload_input(void);

/* (tests) forget the run's state, as after a restart. */
void hos_oot_autoload_reset(void);
