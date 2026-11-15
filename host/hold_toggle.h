/* Button edges, shared by keyboard and pad after remapping. No OS dependencies. */
#pragma once
#include <stdint.h>
typedef struct { uint64_t previous, latched, blocked; } HoldToggle;
static inline uint64_t hold_toggle_apply(HoldToggle *s, uint64_t down, uint64_t enabled, int clear) {
    if (clear) {
        s->latched = 0;
        s->blocked = enabled; /* require release after a menu/focus transition */
        s->previous = down;
        return down & ~enabled;
    }
    s->blocked &= down;
    s->latched &= enabled;
    s->latched ^= down & ~s->previous & enabled & ~s->blocked;
    s->previous = down;
    return (down & ~enabled) | s->latched;
}
