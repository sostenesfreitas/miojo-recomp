/* (hookshot) The control bridge (host/control.c, docs/control.md): HOS_CONTROL=<socket path> lets a tool on the same PC
   drive the running game over a local socket, one JSON request a line. Off unless that is set. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hos_main.c, at the start: listens when HOS_CONTROL is set (version: the build's, for status) */
void hos_control_start(const char *version);
/* nn_hid.c, each read of the controller: the bridge's presses added to buttons and stick. 1 while one of them asked for
   the player's own devices to be left out (exclusive) */
int hos_control_input(uint64_t *buttons, int32_t stick[4]);
/* hos_thread.c, each log line: for the clients that subscribed to log lines */
void hos_control_log(const char *line);
extern volatile int hos_control_on;

/* One of the Insert menu's settings (gpu/vk/vk_api.h MkwVkSetting's fields) */
typedef struct HosControlSetting {
    char key[32], name[48], text[64], tab[16];
    int value, lo, hi, step;
    int restart, fixed;   /* applies at the next start; nothing to choose on this PC */
} HosControlSetting;

/* OoT samples are bounded JSON objects, made on Link's update, never from guest memory on the socket thread.
   Watches have separate bounded pending changes; the table baselines are shared. The slot is a connection slot. */
#define HOS_CONTROL_OOT_JSON 32768
#define HOS_CONTROL_OOT_CLIENTS 8
#define HOS_CONTROL_OOT_FLAGS 64
typedef struct HosControlOotWatch {
    int64_t every;
    int nflags, changes;
    char flags[HOS_CONTROL_OOT_FLAGS][48];
} HosControlOotWatch;

/* The game as the bridge sees it: control.c's own for the game, a fake one in tests/control_test.c */
typedef struct HosControlGame {
    int64_t (*frame)(void);   /* the game's presents so far */
    /* the presents replayed, the draws recorded so far, the picture's size: 0 when there is no such renderer */
    int (*frame_info)(int64_t *replayed, int64_t *draws, int *width, int *height);
    /* a PNG of the first present after `after`; done(ctx, present, ok) from any thread. 0: not taken */
    int (*shot)(const char *path, int64_t after, void (*done)(void *ctx, int64_t present, int ok), void *ctx);
    int (*setting)(int index, HosControlSetting *out);   /* the index-th settings row, to list them: 0 past the last */
    /* the row whose hookshot_options.ini key is key set to value as the menu sets it, out the row after it: 1 set, 0 no such
       row, -1 nothing to choose on this PC, -2 no step of a list row reaches value here, left as it was (out the row as it
       is) */
    int (*setting_set)(const char *key, int value, HosControlSetting *out);
    /* guest memory, when a level is on (host/control.c reads HOS_CONTROL_MEM). region: 0 when [addr, addr + size) is not
       wholly in mapped guest memory (hos_mem_region's class otherwise). read/write copy between guest memory and a host
       buffer; write also marks the pages so the GPU mirror picks them up. Both are called on the game thread, at a
       controller read */
    int (*mem_region)(uint64_t addr, uint64_t size);
    void (*mem_read)(uint64_t addr, void *dst, uint64_t size);
    void (*mem_write)(uint64_t addr, const void *src, uint64_t size);
    /* the folder the game's saves are mounted from (<user>/save/<user id>): 0 before the game opened them */
    int (*save_root)(char *out, size_t cap);
    void (*log)(const char *line);
    void (*quit)(void);   /* does not return */
    const char *(*renderer)(void);   /* the renderer's description, or NULL */
    const char *version;
    /* (OoT) the map the game is in, the last one known (Sequence_CurrentBanc): 0 before any */
    int (*map)(char *out, size_t cap);
    /* (OoT) the debug teleport's destinations: the index-th's maps for the child and the adult (either NULL when it has
       none), 0 past the last */
    int (*destination)(int index, const char **child, const char **adult);
    /* (OoT) a teleport there asked for, taken at the player's next update (in play): a ticket (> 0), 0 no such
       destination. A later ask replaces one not taken yet. teleport_result: 1 sent (map: the map it goes to; accepted: the
       game's event manager took the request), -1 refused (why: the game's reason), 2 (the age switch) Link was that age
       already and nothing was done, 0 not taken yet, -2 replaced or cancelled. teleport_cancel: not to be taken any more */
    int (*teleport)(int index);
    int (*teleport_result)(int ticket, char *why, size_t why_cap, char *map, size_t map_cap, int *accepted);
    void (*teleport_cancel)(int ticket);
    /* (OoT) the Debug tab's Switch age to the adult (1) or the child (0) asked for: taken at the player's next update as a
       teleport is, and done as the menu does it (Link's age flipped, the map reloaded in the other age through the
       teleport) unless Link is that age when it is taken (teleport_result 2, nothing done). A ticket in the teleport's
       numbering (> 0), read with teleport_result (map: the map it reloads) and dropped with teleport_cancel; a later
       teleport or age ask replaces one not taken yet. 0: not asked */
    int (*age)(int adult);
    /* (OoT) with the map: an event playing (a cutscene, a map change's), a map loading, Link the child. 0 before any */
    int (*oot_state)(int *event, int *loading, int *child);
    /* (OoT) coalesced request for the next safe Link-update snapshot; cancel drops an unanswered request. */
    uint64_t (*oot_sample_ask)(void);
    void (*oot_sample_cancel)(uint64_t ticket);
    /* Copy a snapshot that satisfies ticket, or 0 while pending. Ticket 0 reads the last retained snapshot.
       No guest reads here; frame/t still date retained samples. status falls back to oot_state for null
       event/loading/age, so those fields keep their protocol-1 types inside a non-null game. */
    int (*oot_sample)(uint64_t ticket, char *out, size_t cap);
    /* Replace a connection's watch (NULL cancels and forgets pending changes); no guest reads here. */
    void (*oot_watch)(int slot, const HosControlOotWatch *watch);
    /* Consume the latest pending sample for that connection, 0 when none. No guest reads or socket I/O. */
    int (*oot_poll)(int slot, char *out, size_t cap);
} HosControlGame;

/* Listens on path and serves on a thread of its own. 0 when it could not (logged) */
int hos_control_serve(const char *path, const HosControlGame *game);

#ifdef __cplusplus
}
#endif
