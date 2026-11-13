/* (hookshot) Whole frames out of a frame step that can be under one, for the game's counters that add the step, or the step
   times a whole number, cut to whole frames (nx2/oot_patches.py patch_whole_frames). At the console's 60 fps the step is 1 and
   they count frames. Under a Frame rate cap above 60 the step is the frame's own time (host/nn_vi.c, hos_frame_seconds_bits):
   0.42 at 144 fps, cut to 0 every frame, and the counter never moves, so what waits on it never comes: the pick-up indicator
   above Link's head counts the rupees shown up to the wallet's (f_80a87e50: the shown +864 moves by (int)(step * the
   distance / 50, at least 1)) and fades only when they meet; it stayed at the old count until a door or a talk replaced it.
   While the frames' own time is the step (carry), the amount is added to the counter's own remainder and the whole frames in
   it are handed out, so the counter moves at the game's rate whatever the frame rate. Otherwise it is cut as the game cuts it:
   60 fps as before. One remainder per counter address (or per object: each water surface keeps its own), reused oldest
   first. 64 slots: a scene's water surfaces each take one every frame, and more of them than slots would evict each other in
   turn, every one starting again from 0 and standing still. Not thread-safe by itself: the caller holds a lock. */
#pragma once
#include <stdint.h>
#include <string.h>

#define FRAME_WHOLE_SLOTS 64
typedef struct {
    uint64_t at[FRAME_WHOLE_SLOTS];
    float rem[FRAME_WHOLE_SLOTS];
    uint32_t used[FRAME_WHOLE_SLOTS];   /* (the call count at the slot's last use: the oldest is reused) */
    uint32_t calls;
} FrameWhole;

/* (as the lifted code cuts it: toward zero, NaN and below zero to 0, at most 0x7fffffff: the counts are small and positive) */
static inline uint32_t frame_whole_cut(float f) {
    if (!(f > 0.0f)) return 0;
    if (f >= 2147483648.0f) return 0x7fffffffu;
    return (uint32_t)f;
}

static inline uint32_t frame_whole(FrameWhole *w, uint64_t at, uint32_t amount_bits, int carry) {
    float amount;
    memcpy(&amount, &amount_bits, 4);
    if (!carry || !(amount > 0.0f) || amount >= 2147483648.0f) return frame_whole_cut(amount);
    w->calls++;
    int slot = -1, oldest = 0;
    for (int i = 0; i < FRAME_WHOLE_SLOTS; i++) {
        if (w->at[i] == at) { slot = i; break; }
        if (w->used[i] < w->used[oldest]) oldest = i;
    }
    if (slot < 0) slot = oldest, w->at[slot] = at, w->rem[slot] = 0.0f;
    w->used[slot] = w->calls;
    const float sum = w->rem[slot] + amount;
    const uint32_t whole = frame_whole_cut(sum);
    w->rem[slot] = sum - (float)whole;
    return whole;
}
