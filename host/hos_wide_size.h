/* The widescreen picture's size, worked out the same way by host/hos_main.c (hos_wide_out, at the start) and by the Insert
   menu's Output and Render resolution rows (gpu/vk/vk_menu.cpp, the sizes they show): the Output resolution's height kept, the
   width at the screen's ratio (whole multiples of 8 pixels), then at most hos_wide_max_px() pixels with the shape kept.
   Header only, C and C++. */
#ifndef HOS_WIDE_SIZE_H
#define HOS_WIDE_SIZE_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* The host and the menu accept the same custom output shapes and pixel budget. */
static inline int hos_output_size_valid(uint32_t w, uint32_t h) {
    return w >= 640 && h >= 360 && (uint64_t)w * h <= 5120ULL * 2880 &&
           (uint64_t)w * 4 >= (uint64_t)h * 5 && (uint64_t)w * 9 <= (uint64_t)h * 48;
}

/* An output size of a shape of its own: more than 1 % off 16:9 (16:10, 4:3, 21:9, 32:9...), as the Insert menu's Aspect ratio
   row offers them. The picture takes that shape (its cameras and HUD as at the monitor's shape), over HOS_ASPECT and the
   Aspect ratio's ultrawide=, which then only shape a 16:9 output. The same 1 % as host/hos_main.c's screen ratio. */
static inline int hos_output_own_shape(uint32_t w, uint32_t h) {
    if (!w || !h) return 0;
    const double r = (double)w / (double)h, game = 16.0 / 9.0;
    return r > game * 1.01 || r < game * 0.99;
}

/* The screen's ratio as the picture takes it: 5:4 to 48:9. */
static inline double hos_wide_clamp(double want) {
    const double surround = 48.0 / 9.0;
    /* Accept the rounded value commonly used for HOS_ASPECT. */
    return want >= surround - 0.0001 ? surround : want < 5.0 / 4.0 ? 5.0 / 4.0 : want;
}

/* The most pixels a widescreen picture takes: 5120 x 2160's (a 21:9 screen at the 4K output, native; 7680 x 1440, 48:9 at
   1440, has as many). The game's render targets come from a pool scaled with the picture's pixels (host/nn_applet.c
   hos_rt_pool_size): 915 MB for that, from the game's 305 MB for 2560 x 1440. It was 3840 x 2160's: a 32:9 screen at the 4K
   output made a 7680 x 2160 picture whose pool at the output's size ran out as Kokiri Forest loaded (a null pointer, issue
   1031), and a pool for that whole picture, 1.4 GB, failed the game's own reservation at boot. HOS_WIDE_MAX_PX=<pixels>
   overrides it (3840 x 2160's, 8294400, as before). */
static inline double hos_wide_max_px(void) {
    const char *e = getenv("HOS_WIDE_MAX_PX");
    const double v = e ? atof(e) : 0.0;
    return v >= 640.0 * 360.0 ? v : 5120.0 * 2160.0;
}

/* (hookshot) The Aspect ratio row (ultrawide=) as the game starts with it: 1 the monitor's shape, 0 the game's 16:9. saved: the
   options file's value, -1 without the line (the default: the monitor's shape); version: its options_version, 0 without one.
   A file from before version 6 wrote ultrawide=0, the 16:9 that was the default then, whether chosen or not: on a screen
   narrower than 16:9 (16:10, the Steam Deck's 1280x800, 4:3), where 16:9 only adds bars above and below the picture, that
   moves to the monitor's shape. An ultrawide keeps its 16:9 (a choice there as often as not), and a file from version 6 on
   keeps whichever it says. "Narrower" as host/hos_main.c takes it: more than 1% under 16:9, and wider than tall.
   host/hos_main.c starts the game with it; the Insert menu's row follows what the host started with (gpu/vk/vk_menu.cpp
   menu_init) */
static inline int hos_aspect_on(int saved, int version, int screen_w, int screen_h) {
    if (saved < 0) return 1;
    if (saved == 0 && version < 6 && screen_w > screen_h && screen_h > 0 &&
        (double)screen_w / screen_h < 16.0 / 9.0 * 0.99)
        return 1;
    return saved != 0;
}

/* The picture for an output height at ratio r (hos_wide_clamp's): *w x *h */
static inline void hos_wide_size(unsigned out_h, double r, double max_px, unsigned *w, unsigned *h) {
    unsigned oh = out_h, ow = (unsigned)(oh * r / 8 + 0.5) * 8;
    if ((double)ow * oh > max_px) {   /* (the shape kept) */
        /* At 48:9, 24 rows keep both dimensions aligned to 8 without changing the ratio. */
        const unsigned aligned_height = r == 48.0 / 9.0 ? 24 : 8;
        oh = (unsigned)(sqrt(max_px / r) / aligned_height) * aligned_height;
        ow = (unsigned)(oh * r / 8) * 8;
    }
    *w = ow, *h = oh;
}

#endif
