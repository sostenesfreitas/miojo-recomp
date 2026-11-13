/* (hookshot) Valve's own controllers under Steam, and the motion sensors behind Steam Input's stand-in pads
   (host/pad_sdl.c).

   Started from Steam, the game gets SDL_GAMECONTROLLER_IGNORE_DEVICES, which lists Valve's controllers (the Steam
   Controller, the Steam Deck's) and others Steam reads itself, and SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1.
   SDL then opens only Steam Input's stand-in pad, which has no gyroscope or accelerometer, so Motion controls found no
   pad (#1897: a Steam Controller on CachyOS, and on Windows). Valve's entries come out of that list, so SDL opens the
   real pad as well, all but the Steam Deck's built-in controls (STEAM_PADS_DECK): SDL's Steam Deck driver resets their
   trackpad and button settings as it opens them, which is not yet tested beside Steam Input in Game Mode, so they stay
   as Steam set them. While a Steam stand-in pad is open, a real Valve pad gives its motion only: its buttons and sticks
   come through the stand-in pad, as the player's Steam layout maps them, and are not counted twice.

   Pure (no state, no SDL): tests/test_steam_pads.py compiles it. */
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define STEAM_PADS_VALVE 0x28de
#define STEAM_PADS_DECK 0x1205   /* (the Steam Deck's built-in controls: kept in the list) */

/* SDL's ignore list (0xVVVV/0xPPPP entries split by commas) without Valve's entries but the Steam Deck's, into out (cap
   bytes, always ended). Returns how many entries came out, or -1 when out is left empty and the list should stay as it
   is: a list SDL reads from a file (@path), or one too long for out. */
static inline int steam_pads_without_valve(const char *in, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = 0;
    if (!in) return 0;
    while (*in == ' ' || *in == '\t') in++;
    if (*in == '@') return -1;
    size_t n = 0;
    int dropped = 0;
    for (const char *p = in; *p;) {
        const char *end = strchr(p, ',');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        char *after;
        const unsigned long vendor = strtoul(p, &after, 16);   /* (16 takes the 0x too) */
        const unsigned long product = after < p + len && *after == '/' ? strtoul(after + 1, NULL, 16) : 0;
        if (after != p && after <= p + len && vendor == STEAM_PADS_VALVE && product != STEAM_PADS_DECK) {
            dropped++;
        } else if (len) {
            if (n + (n ? 1 : 0) + len + 1 > cap) {
                out[0] = 0;
                return -1;
            }
            if (n) out[n++] = ',';
            memcpy(out + n, p, len);
            n += len;
            out[n] = 0;
        }
        p += len;
        if (*p == ',') p++;
    }
    return dropped;
}

/* Whether a pad gives its motion only: a real Valve pad (vendor 0x28de and no Steam Input handle) while a Steam
   stand-in pad (one with a handle) is open */
static inline int steam_pads_motion_only(unsigned vendor, unsigned long long steam_handle, int stand_in_open) {
    return vendor == STEAM_PADS_VALVE && !steam_handle && stand_in_open;
}
