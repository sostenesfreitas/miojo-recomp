/* (hookshot) The Insert menu's "Auto run" (Controls): the sprint goes on after the run button (A) is let go.

   In this game Link sprints while A is held (the Xbox / PC layout puts A on the pad's bottom button). With the row on,
   A held for AUTO_RUN_HOLD_NS while the left stick is pushed and then let go with the stick still pushed stays held for
   the game, so the sprint goes on. A short press (a roll on its own) is never kept. It ends when the stick comes back
   under AUTO_RUN_STICK (Link stops), when A is pressed again (the player's A takes over: letting go of it stops, and a
   long hold keeps the sprint again), when Plus or Minus is pressed (the pause menu or the map open with A let go, so A
   never picks an entry there), or when Link is no longer the player's to move (`player_free`: an event or a talk, the
   pause, the Insert menu; host/hos_main.c hos_oot_player_free). The stick passes as it is. A header of its own so
   tests/test_auto_run.py runs it without the game. */
#ifndef HOS_AUTO_RUN_H
#define HOS_AUTO_RUN_H

#include <stdint.h>

#define AUTO_RUN_HOLD_NS 400000000LL   /* (A held this long while moving, then let go: the sprint goes on) */
#define AUTO_RUN_STICK 9830            /* (30% of the stick's travel: under it the stick counts as let go) */

typedef struct {
    int locked;          /* A is held for the player */
    int a_was;           /* the player's own A at the last read */
    int64_t held_since;  /* when the player's A went down while moving; 0 when not */
} AutoRun;

/* buttons in nn::hid's bit order (A 0, Plus 10, Minus 11); lx, ly the left stick; on the menu row (nonzero on);
   player_free whether Link is the player's to move now; now in ns. The buttons the game is to see */
static inline uint64_t hos_auto_run(AutoRun *s, uint64_t buttons, int32_t lx, int32_t ly, int on, int player_free,
                                    int64_t now) {
    const uint64_t a_bit = 1ULL << 0, menus = 1ULL << 10 | 1ULL << 11;
    const int a = (buttons & a_bit) != 0;
    const int moving = (int64_t)lx * lx + (int64_t)ly * ly > (int64_t)AUTO_RUN_STICK * AUTO_RUN_STICK;
    if (!on || !player_free || !moving || (buttons & menus)) {
        s->locked = 0;
        s->held_since = 0;
    } else if (s->locked) {
        if (a && !s->a_was) s->locked = 0, s->held_since = now;   /* (pressed again: the player's A from here) */
        else buttons |= a_bit;
    } else if (a) {
        if (!s->held_since) s->held_since = now;
    } else {
        if (s->a_was && s->held_since && now - s->held_since >= AUTO_RUN_HOLD_NS) s->locked = 1, buttons |= a_bit;
        s->held_since = 0;
    }
    s->a_was = a;
    return buttons;
}

#endif
