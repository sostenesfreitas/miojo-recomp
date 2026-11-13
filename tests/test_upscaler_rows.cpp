#include "../gpu/vk/upscaler_rows.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace ur = mkwvk::upscaler_rows;
    using ur::DLAA;
    using ur::DLSS;
    using ur::GAME;
    // (the values saved before keep their shares; the new ones are DLSS's ratios, each a twelfth of the output)
    CHECK("saved values keep their shares", ur::percent(0) == 0 && ur::percent(1) == 100 && ur::percent(2) == 75 &&
                                                ur::percent(3) == 67 && ur::percent(4) == 50);
    CHECK("Balanced and Ultra Performance", ur::percent(5) == 58 && ur::percent(6) == 33);
    CHECK("a value past the row is Auto", ur::percent(10) == 0 && ur::percent(-1) == 0);   // (7 to 9: the slider)
    for (int v = 1; v < ur::kValues; v++) {
        const int k = (int)(ur::percent(v) * 12 / 100.0 + 0.5);
        CHECK("each share is a twelfth", k * 100 / 12 == ur::percent(v) || (k * 100 + 11) / 12 == ur::percent(v));
    }
    CHECK("DLSS's mode names", !std::strcmp(ur::dlss_mode_name(3), "Quality") && !std::strcmp(ur::dlss_mode_name(5), "Balanced") &&
                                   !std::strcmp(ur::dlss_mode_name(4), "Performance") &&
                                   !std::strcmp(ur::dlss_mode_name(6), "Ultra Performance"));
    CHECK("no mode name for Auto, 100% or 75%", !ur::dlss_mode_name(0) && !ur::dlss_mode_name(1) && !ur::dlss_mode_name(2));

    // the game's own: Auto, 100, 75, 67, 50, as before
    CHECK("game: Right", ur::step(GAME, 0, 1) == 1 && ur::step(GAME, 1, 1) == 2 && ur::step(GAME, 3, 1) == 4 && ur::step(GAME, 4, 1) == 0);
    CHECK("game: Left", ur::step(GAME, 0, -1) == 4 && ur::step(GAME, 1, -1) == 0 && ur::step(GAME, 4, -1) == 3);
    // DLSS: Auto, Quality 67, Balanced 58, Performance 50, Ultra Performance 33
    CHECK("dlss: Right", ur::step(DLSS, 0, 1) == 3 && ur::step(DLSS, 3, 1) == 5 && ur::step(DLSS, 5, 1) == 4 &&
                             ur::step(DLSS, 4, 1) == 6 && ur::step(DLSS, 6, 1) == 0);
    CHECK("dlss: Left", ur::step(DLSS, 0, -1) == 6 && ur::step(DLSS, 3, -1) == 0 && ur::step(DLSS, 6, -1) == 4);
    // a value that is not a step (saved with the other upscaler): its neighbours by size
    CHECK("dlss from 75%", ur::step(DLSS, 2, 1) == 3 && ur::step(DLSS, 2, -1) == 0);
    CHECK("dlss from 100%", ur::step(DLSS, 1, 1) == 3 && ur::step(DLSS, 1, -1) == 0);
    CHECK("game from 58%", ur::step(GAME, 5, 1) == 4 && ur::step(GAME, 5, -1) == 3);
    CHECK("game from 33%", ur::step(GAME, 6, 1) == 0 && ur::step(GAME, 6, -1) == 4);
    CHECK("DLAA does not step", ur::step(DLAA, 3, 1) == 3 && ur::step(DLAA, 0, -1) == 0);

    // another upscaler: the nearest step (the larger on a tie), Auto stays Auto
    CHECK("snap to DLSS", ur::snap(DLSS, 0) == 0 && ur::snap(DLSS, 1) == 3 && ur::snap(DLSS, 2) == 3 && ur::snap(DLSS, 3) == 3 &&
                              ur::snap(DLSS, 4) == 4);
    CHECK("snap to the game's", ur::snap(GAME, 5) == 4 && ur::snap(GAME, 6) == 4 && ur::snap(GAME, 3) == 3 && ur::snap(GAME, 1) == 1);
    CHECK("DLAA keeps the value", ur::snap(DLAA, 5) == 5 && ur::snap(DLAA, 1) == 1);
    CHECK("a value past the row snaps to Auto", ur::snap(GAME, 10) == 0 && ur::snap(DLSS, -3) == 0);

    // the Upscaler row: all three where DLSS runs, the game's own alone elsewhere
    CHECK("upscaler with DLSS", ur::step_upscaler(0, 1, true) == 1 && ur::step_upscaler(1, 1, true) == 2 &&
                                    ur::step_upscaler(2, 1, true) == 0 && ur::step_upscaler(0, -1, true) == 2);
    CHECK("upscaler without DLSS", ur::step_upscaler(0, 1, false) == 0 && ur::step_upscaler(2, -1, false) == 0);
    CHECK("upscaler from a bad value", ur::step_upscaler(7, 1, true) == 1);

    // the Upscaler row only where more than the game's own runs; the rows with nothing to choose
    CHECK("upscaler row hidden with the game's own alone", !ur::upscaler_shown(ur::bit(GAME)) && !ur::upscaler_shown(0u) &&
                                                             ur::upscaler_shown(ur::bit(GAME) | ur::dlss_bits(true)) &&
                                                             !ur::upscaler_shown(ur::bit(GAME) | ur::dlss_bits(false)));
    // AMD FSR 3.1 on the same row, after DLAA: stepping visits only what runs here, and wraps
    {
        const unsigned all = ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(ur::FSR3);
        const unsigned fsr3 = ur::bit(GAME) | ur::bit(ur::FSR3);
        CHECK("FSR 3.1 after DLAA", ur::FSR3 == 3);
        CHECK("upscaler steps through all four", ur::step_upscaler(DLAA, 1, all) == ur::FSR3 &&
                                                   ur::step_upscaler(ur::FSR3, 1, all) == GAME &&
                                                   ur::step_upscaler(GAME, -1, all) == ur::FSR3 &&
                                                   ur::step_upscaler(ur::FSR3, -1, all) == DLAA);
        CHECK("upscaler without DLSS skips it", ur::step_upscaler(GAME, 1, fsr3) == ur::FSR3 &&
                                                  ur::step_upscaler(ur::FSR3, 1, fsr3) == GAME &&
                                                  ur::step_upscaler(GAME, -1, fsr3) == ur::FSR3);
        CHECK("an upscaler that cannot run here steps as the game's own", ur::step_upscaler(DLSS, 1, fsr3) == ur::FSR3 &&
                                                                            ur::step_upscaler(ur::FSR3, 1, ur::bit(GAME)) == GAME);
        CHECK("the DLSS-only form is the mask's", ur::step_upscaler(DLAA, 1, true) == GAME && ur::step_upscaler(GAME, -1, true) == DLAA);
        CHECK("upscaler fixed only with nothing but the game's own",
              ur::upscaler_fixed(ur::bit(GAME)) && !ur::upscaler_fixed(fsr3) && !ur::upscaler_fixed(all));
        CHECK("upscaler row shown with FSR 3.1 alone", ur::upscaler_shown(fsr3) && ur::upscaler_shown(all) &&
                                                          ur::upscaler_shown(ur::bit(GAME) | ur::fsr4_bits(true)));
        CHECK("FSR 3.1's modes are DLSS's shares", ur::scales_by_mode(ur::FSR3) && ur::scales_by_mode(DLSS) &&
                                                     !ur::scales_by_mode(DLAA) && !ur::scales_by_mode(GAME));
        // (FSR 3.1: Auto, 100 % (its native AA), 92 % to 75 %, then AMD's modes)
        CHECK("FSR 3.1 steps: above Quality too", ur::step(ur::FSR3, 0, 1) == 1 && ur::step(ur::FSR3, 1, 1) == 9 &&
                                                    ur::step(ur::FSR3, 9, 1) == 8 && ur::step(ur::FSR3, 8, 1) == 2 &&
                                                    ur::step(ur::FSR3, 2, 1) == 3 && ur::step(ur::FSR3, 3, 1) == 5 &&
                                                    ur::step(ur::FSR3, 6, 1) == 0 && ur::step(ur::FSR3, 0, -1) == 6 &&
                                                    ur::snap(ur::FSR3, 2) == 2 && ur::snap(ur::FSR3, 7) == 4);
        CHECK("the model row waits with FSR 3.1", ur::model_fixed(ur::FSR3, false) && !ur::model_fixed(DLSS, false) &&
                                                    !ur::model_fixed(DLAA, false));
        CHECK("FSR 3.1 has a Render resolution", !ur::resolution_fixed(ur::FSR3));
    }
    // AMD FSR 4 and its native AA after FSR 3.1 (saved values keep their meaning), on the same row and mask
    {
        using ur::FSR3;
        using ur::FSR4;
        using ur::FSR4_AA;
        const unsigned every = ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(FSR3) | ur::fsr4_bits(true);
        const unsigned fsr4 = ur::bit(GAME) | ur::fsr4_bits(true);
        CHECK("FSR 4 after FSR 3.1", FSR4 == 4 && FSR4_AA == 5);
        CHECK("upscaler steps through all six", ur::step_upscaler(FSR3, 1, every) == FSR4 &&
                                                  ur::step_upscaler(FSR4, 1, every) == FSR4_AA &&
                                                  ur::step_upscaler(FSR4_AA, 1, every) == GAME &&
                                                  ur::step_upscaler(GAME, -1, every) == FSR4_AA &&
                                                  ur::step_upscaler(FSR4, -1, every) == FSR3);
        CHECK("upscaler with FSR 4 alone", ur::step_upscaler(GAME, 1, fsr4) == FSR4 && ur::step_upscaler(FSR4_AA, 1, fsr4) == GAME &&
                                               ur::step_upscaler(GAME, -1, fsr4) == FSR4_AA && !ur::upscaler_fixed(fsr4));
        CHECK("a saved FSR 4 where it cannot run steps as the game's own",
              ur::step_upscaler(FSR4, 1, ur::bit(GAME) | ur::dlss_bits(true)) == DLSS &&
              ur::step_upscaler(FSR4_AA, -1, ur::bit(GAME) | ur::bit(FSR3)) == FSR3);
        CHECK("engines", ur::is_dlss(DLSS) && ur::is_dlss(DLAA) && !ur::is_dlss(FSR3) && !ur::is_dlss(FSR4) &&
                             ur::is_fsr4(FSR4) && ur::is_fsr4(FSR4_AA) && !ur::is_fsr4(FSR3) && !ur::is_fsr4(DLAA) &&
                             !ur::is_fsr4(GAME));
        CHECK("full scale", ur::full_scale(DLAA) && ur::full_scale(FSR4_AA) && !ur::full_scale(FSR3) && !ur::full_scale(FSR4) &&
                                !ur::full_scale(DLSS) && !ur::full_scale(GAME));
        CHECK("FSR 4's modes are DLSS's shares", ur::scales_by_mode(FSR4) && !ur::scales_by_mode(FSR4_AA));
        for (int v = 0; v < ur::kValues; v++)
            for (int dir : {1, -1})
                CHECK("FSR 4 steps as DLSS", ur::step(FSR4, v, dir) == ur::step(DLSS, v, dir) && ur::snap(FSR4, v) == ur::snap(DLSS, v));
        CHECK("FSR 4's native AA does not step", ur::step(FSR4_AA, 3, 1) == 3 && ur::snap(FSR4_AA, 5) == 5);
        CHECK("the model row waits with FSR 4", ur::model_fixed(FSR4, false) && ur::model_fixed(FSR4_AA, false));
    }
    // Off (no upscaler, the game's network left out too): after FSR 4's native AA (saved values keep their meaning), on every
    // PC (the menu's mask always has it), with its own Render resolution steps
    {
        using ur::OFF;
        const unsigned menu_min = ur::bit(GAME) | ur::bit(OFF);   // (the menu's mask on a PC with nothing else)
        const unsigned every = menu_min | ur::dlss_bits(true) | ur::bit(ur::FSR3) | ur::fsr4_bits(true);
        CHECK("Off is the last value", OFF == 6 && ur::kUpscalers == 7);
        CHECK("Off steps after FSR 4's native AA and wraps to the game's own",
              ur::step_upscaler(ur::FSR4_AA, 1, every) == OFF && ur::step_upscaler(OFF, 1, every) == GAME &&
              ur::step_upscaler(GAME, -1, every) == OFF && ur::step_upscaler(OFF, -1, every) == ur::FSR4_AA);
        CHECK("Off with the game's own alone", ur::step_upscaler(GAME, 1, menu_min) == OFF &&
                                                  ur::step_upscaler(OFF, 1, menu_min) == GAME &&
                                                  ur::step_upscaler(GAME, -1, menu_min) == OFF);
        CHECK("a saved DLSS where it cannot run steps as the game's own, to Off", ur::step_upscaler(DLSS, 1, menu_min) == OFF);
        CHECK("the row is shown everywhere Off is", ur::upscaler_shown(menu_min) && !ur::upscaler_fixed(menu_min));
        CHECK("Off is not an engine", !ur::is_dlss(OFF) && !ur::is_fsr4(OFF) && !ur::full_scale(OFF) && !ur::scales_by_mode(OFF));
        CHECK("Off has a Render resolution", !ur::resolution_fixed(OFF));
        CHECK("the model row waits with Off", ur::model_fixed(OFF, false));
        // its steps: Auto, 100, 75, 67, 50, 33
        CHECK("off: Right", ur::step(OFF, 0, 1) == 1 && ur::step(OFF, 1, 1) == 2 && ur::step(OFF, 2, 1) == 3 &&
                                ur::step(OFF, 3, 1) == 4 && ur::step(OFF, 4, 1) == 6 && ur::step(OFF, 6, 1) == 0);
        CHECK("off: Left", ur::step(OFF, 0, -1) == 6 && ur::step(OFF, 6, -1) == 4 && ur::step(OFF, 1, -1) == 0);
        CHECK("off from 58%", ur::step(OFF, 5, 1) == 4 && ur::step(OFF, 5, -1) == 3);
        CHECK("snap to Off", ur::snap(OFF, 0) == 0 && ur::snap(OFF, 1) == 1 && ur::snap(OFF, 2) == 2 && ur::snap(OFF, 6) == 6 &&
                                 ur::snap(OFF, 5) == 4 && ur::snap(OFF, 9) == 1 && ur::snap(OFF, 12) == 0);
        CHECK("DLSS's Ultra Performance kept by Off, the game's own takes 50%", ur::snap(OFF, 6) == 6 && ur::snap(GAME, 6) == 4);
        CHECK("Off's 33% to DLSS stays Ultra Performance", ur::snap(DLSS, ur::step(OFF, 4, 1)) == 6);
        // the row by engine (step_engine): Off after FSR 4, and back to the game's own; from DLAA (DLSS's Native) too
        CHECK("Off by engine", ur::step_engine(ur::FSR4, 1, every) == OFF && ur::step_engine(ur::FSR4_AA, 1, every) == OFF &&
                                   ur::step_engine(OFF, 1, every) == GAME && ur::step_engine(GAME, -1, every) == OFF &&
                                   ur::step_engine(OFF, -1, every) == ur::FSR4 && ur::step_engine(GAME, 1, menu_min) == OFF &&
                                   ur::step_engine(OFF, 1, menu_min) == GAME);
        CHECK("Off is its own engine, without a Native", ur::engine(OFF) == OFF && ur::native_of(OFF) < 0 && !ur::render_fixed(OFF));
        int up = 0, res = 0;
        ur::step_render(OFF, 2, 1, &up, &res);
        CHECK("Off's Render resolution steps its own way", up == OFF && res == 3 && ur::snap_render(OFF, 6) == 6 &&
                                                              ur::snap_render(OFF, 8) == 2);
    }
    CHECK("upscaler fixed where DLSS cannot run", ur::upscaler_fixed(false) && !ur::upscaler_fixed(true));
    CHECK("render resolution fixed at full scale only", ur::resolution_fixed(DLAA) && ur::resolution_fixed(ur::FSR4_AA) &&
                                                             !ur::resolution_fixed(DLSS) && !ur::resolution_fixed(ur::FSR4) &&
                                                             !ur::resolution_fixed(GAME));
    // the Window scaling filter: needed only when the window is not the picture's size (unknown: needed)
    CHECK("filter not needed at the output's size", !ur::filter_needed(2560, 1440, 2560, 1440));
    CHECK("filter needed otherwise", ur::filter_needed(2048, 1152, 2560, 1440) && ur::filter_needed(2560, 1440, 2560, 1080) &&
                                         ur::filter_needed(0, 0, 0, 0) && ur::filter_needed(2560, 1440, 0, 0));
    // the DLSS model row: its presets (NVIDIA's numbers, K = 11), RX_VK_DLSS_PRESET over it, and when it applies
    CHECK("model presets", ur::model_preset(ur::MODEL_K) == 11 && ur::model_preset(ur::MODEL_M) == 13 &&
                               ur::model_preset(ur::MODEL_L) == 12 && ur::model_preset(ur::MODEL_DEFAULT) == 0);
    CHECK("model: a value past the row is K", ur::model_preset(-1) == 11 && ur::model_preset(ur::kModels) == 11);
    CHECK("model without the switch", ur::dlss_preset(ur::MODEL_M, nullptr) == 13 && ur::dlss_preset(ur::MODEL_DEFAULT, nullptr) == 0);
    CHECK("RX_VK_DLSS_PRESET wins", ur::dlss_preset(ur::MODEL_M, "K") == 11 && ur::dlss_preset(ur::MODEL_K, "m") == 13 &&
                                        ur::dlss_preset(ur::MODEL_K, "J") == 10 && ur::dlss_preset(ur::MODEL_M, "0") == 0);
    CHECK("RX_VK_DLSS_PRESET not a preset", !ur::preset_env(nullptr) && !ur::preset_env("") && !ur::preset_env("1") &&
                                                !ur::preset_env("05") && ur::dlss_preset(ur::MODEL_M, "7") == 13 &&
                                                ur::dlss_preset(ur::MODEL_L, "") == 12);
    CHECK("preset letters", ur::preset_letter(11) == 'K' && ur::preset_letter(13) == 'M' && ur::preset_letter(0) == '0');
    CHECK("model row shown only where DLSS runs", ur::model_shown(true) && !ur::model_shown(false));
    CHECK("model row fixed with the game's own or the switch", ur::model_fixed(GAME, false) && !ur::model_fixed(DLSS, false) &&
                                                               !ur::model_fixed(DLAA, false) && ur::model_fixed(DLSS, true));

    // Native under its engine: the Upscaler row by engine, DLAA / FSR 4's native AA as the Render
    // resolution row's Native (Auto, Native, then the modes)
    {
        using ur::FSR3;
        using ur::FSR4;
        using ur::FSR4_AA;
        CHECK("engines", ur::engine(DLAA) == DLSS && ur::engine(FSR4_AA) == FSR4 && ur::engine(DLSS) == DLSS &&
                             ur::engine(FSR3) == FSR3 && ur::engine(GAME) == GAME);
        CHECK("natives", ur::native_of(DLSS) == DLAA && ur::native_of(FSR4) == FSR4_AA && ur::native_of(GAME) < 0 &&
                             ur::native_of(FSR3) < 0);
        const unsigned all = ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(FSR3) | ur::fsr4_bits(true);
        CHECK("engine row: Right", ur::step_engine(GAME, 1, all) == DLSS && ur::step_engine(DLSS, 1, all) == FSR3 &&
                                       ur::step_engine(FSR3, 1, all) == FSR4 && ur::step_engine(FSR4, 1, all) == GAME);
        CHECK("engine row: Left", ur::step_engine(GAME, -1, all) == FSR4 && ur::step_engine(FSR3, -1, all) == DLSS);
        CHECK("engine row: Native goes with the engine", ur::step_engine(DLAA, 1, all) == FSR3 &&
                                                            ur::step_engine(DLAA, -1, all) == GAME &&
                                                            ur::step_engine(FSR4_AA, 1, all) == GAME &&
                                                            ur::step_engine(FSR4_AA, -1, all) == FSR3);
        const unsigned dlss_fsr4 = ur::bit(GAME) | ur::dlss_bits(true) | ur::fsr4_bits(true);
        CHECK("engine row: Native kept between engines that have one", ur::step_engine(DLAA, 1, dlss_fsr4) == FSR4_AA &&
                                                                         ur::step_engine(FSR4_AA, -1, dlss_fsr4) == DLAA);
        CHECK("engine row: DLAA is never a stop of its own", ur::step_engine(DLSS, 1, ur::bit(GAME) | ur::dlss_bits(true)) == GAME);
        CHECK("engine row: only the game's own", ur::step_engine(GAME, 1, ur::bit(GAME)) == GAME &&
                                                     ur::step_engine(DLSS, 1, ur::bit(GAME)) == GAME);
        // (the slider: Auto, Native, then the twelfths DLSS takes: 92 83 75 67 58 50 33 % (not 42: no DLSS mode takes it))
        CHECK("slider values", ur::percent(7) == 42 && ur::percent(8) == 83 && ur::percent(9) == 92);
        int up = 0, res = 0;
        ur::step_render(DLSS, 0, 1, &up, &res);
        CHECK("render: Auto, then Native", up == DLAA && res == 0);
        ur::step_render(DLAA, 0, 1, &up, &res);
        CHECK("render: Native, then 92%", up == DLSS && res == 9);
        ur::step_render(DLAA, 4, -1, &up, &res);
        CHECK("render: Native, back to Auto", up == DLSS && res == 0);
        ur::step_render(DLSS, 9, -1, &up, &res);
        CHECK("render: 92%, back to Native (the value kept)", up == DLAA && res == 9);
        ur::step_render(DLSS, 3, -1, &up, &res);
        CHECK("render: Quality, Left to 75%", up == DLSS && res == 2);
        ur::step_render(DLSS, 3, 1, &up, &res);
        CHECK("render: Quality, Right to Balanced", up == DLSS && res == 5);
        ur::step_render(DLSS, 4, 1, &up, &res);
        CHECK("render: Performance, Right to Ultra Performance (42% is not a stop)", up == DLSS && res == 6);
        ur::step_render(DLSS, 6, 1, &up, &res);
        CHECK("render: Ultra Performance wraps to Auto", up == DLSS && res == 0);
        ur::step_render(DLSS, 0, -1, &up, &res);
        CHECK("render: Auto, Left to Ultra Performance", up == DLSS && res == 6);
        ur::step_render(DLSS, 1, -1, &up, &res);
        CHECK("render: 100% (a saved value) counts as 92%", up == DLAA && res == 1);
        ur::step_render(FSR4_AA, 5, 1, &up, &res);
        CHECK("render: FSR 4's Native the same", up == FSR4 && res == 9);
        CHECK("snap_render keeps a stop", ur::snap_render(DLSS, 8) == 8 && ur::snap_render(DLAA, 5) == 5 &&
                                              ur::snap_render(FSR4, 2) == 2 && ur::snap_render(DLSS, 0) == 0);
        CHECK("snap_render: 100% to 92%, a saved 42% to 50%", ur::snap_render(DLSS, 1) == 9 && ur::snap_render(DLSS, 7) == 4);
        CHECK("snap_render: engines without a Native as snap", ur::snap_render(GAME, 5) == ur::snap(GAME, 5) &&
                                                                   ur::snap_render(ur::FSR3, 2) == ur::snap(ur::FSR3, 2));
        ur::step_render(FSR3, 0, 1, &up, &res);
        CHECK("render: FSR 3.1 has no Native: its 100 % is its native AA", up == FSR3 && res == 1);
        ur::step_render(GAME, 0, 1, &up, &res);
        CHECK("render: the game's own as before", up == GAME && res == 1);
        CHECK("render row never fixed", !ur::render_fixed(DLAA) && !ur::render_fixed(FSR4_AA) && !ur::render_fixed(DLSS) &&
                                            !ur::render_fixed(GAME) && !ur::render_fixed(FSR3));
    }
    {   // the texture LOD bias added to the game's -0.5 (its materials' bias for its own 3/4): NVIDIA's guide for DLSS
        // (native bias + log2(render / output) - 1), the output's mips for FSR, nothing for the rest
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        const auto total = [&](int up, int share) { return -0.5f + ur::upscale_lod_bias_for(up, share, -1.0f); };
        const auto nvidia = [](float r) { return -0.5f - std::log2(0.75f) + std::log2(r) - 1.0f; };   // (native bias ~ -0.085)
        for (int share : {3333, 5000, 5833, 6667, 7500, 9167})
            CHECK("DLSS: NVIDIA's mip bias at each fixed share", near(total(DLSS, share), nvidia(share / 10000.0f)));
        CHECK("DLSS Quality: 1.17 more", near(ur::upscale_lod_bias_for(DLSS, 6667, -1.0f), -1.1699f));
        CHECK("DLSS Performance: 1.58 more", near(ur::upscale_lod_bias_for(DLSS, 5000, -1.0f), -1.585f));
        CHECK("DLAA: the output's own size, -1 in all", near(total(DLAA, 0), nvidia(1.0f)) && near(total(DLAA, 6667), nvidia(1.0f)));
        CHECK("DLSS at Auto: the game's 3/4", near(total(DLSS, 0), nvidia(0.75f)));
        CHECK("every DLSS bias is negative and lower at smaller shares",
              ur::upscale_lod_bias_for(DLSS, 3333, -1.0f) < ur::upscale_lod_bias_for(DLSS, 5000, -1.0f) &&
                  ur::upscale_lod_bias_for(DLSS, 5000, -1.0f) < ur::upscale_lod_bias_for(DLSS, 6667, -1.0f) &&
                  ur::upscale_lod_bias_for(DLAA, 0, -1.0f) < 0.0f);
        CHECK("offset 0: the render size's own detail (as before)", near(ur::upscale_lod_bias_for(DLSS, 6667, 0.0f), std::log2(6667 / 7500.0f)) &&
                                                                      ur::upscale_lod_bias_for(DLSS, 0, 0.0f) == 0.0f &&
                                                                      ur::upscale_lod_bias_for(DLAA, 0, 0.0f) == 0.0f);
        CHECK("FSR: the output's mips below 3/4 only", near(ur::upscale_lod_bias_for(ur::FSR3, 5000, -1.0f), std::log2(5000 / 7500.0f)) &&
                                                         ur::upscale_lod_bias_for(ur::FSR4, 0, -1.0f) == 0.0f &&
                                                         ur::upscale_lod_bias_for(ur::FSR3, 10000, -1.0f) == 0.0f);
        CHECK("no bias for the game's own, Off and FSR 4's native AA",
              ur::upscale_lod_bias_for(GAME, 6667, -1.0f) == 0.0f && ur::upscale_lod_bias_for(ur::OFF, 5000, -1.0f) == 0.0f &&
                  ur::upscale_lod_bias_for(ur::FSR4_AA, 0, -1.0f) == 0.0f);
    }
    std::puts("upscaler rows: all checks passed");
}
