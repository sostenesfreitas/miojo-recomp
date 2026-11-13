// auto_settings (gpu/vk/auto_settings.h): the Quality preset "Auto (this PC)"'s rules. tests/test_auto_settings.py
#include "../gpu/vk/auto_settings.h"
#include "../gpu/vk/display_modes.h"
#include "../gpu/vk/upscaler_rows.h"
namespace ur = mkwvk::upscaler_rows;

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    using namespace auto_settings;
    // (the copies of the menu's indices agree with upscaler_rows.h)
    expect((int)GAME == (int)ur::GAME && (int)DLSS == (int)ur::DLSS && (int)FSR4 == (int)ur::FSR4, "upscaler indices as upscaler_rows.h's");
    expect(ur::percent(RES_QUALITY) == 67 && ur::percent(RES_BALANCED) == 58 && ur::percent(RES_PERFORMANCE) == 50,
           "render resolution indices: Quality 67, Balanced 58, Performance 50");

    Hardware h;
    h.vendor = NVIDIA, h.rtx = true, h.dlss_built = true, h.vram_mb = 12282, h.ram_mb = 32000, h.screen_w = 2560, h.screen_h = 1440;
    Choice c = choose(h);
    expect(c.tier == 4 && c.upscaler == DLSS && c.resolution == RES_QUALITY && c.output_w == 2560 && c.output_h == 1440,
           "RTX, 12 GB, 1440p: High, DLSS Quality, output 2560x1440");
    expect(std::strstr(c.why, "DLSS Quality") && std::strstr(c.why, "2560x1440"), c.why);

    h.dlss_built = false;
    c = choose(h);
    expect(c.upscaler == GAME && c.resolution == 0 && std::strstr(c.why, "no DLSS"), "a build without DLSS: the game's own");

    Hardware amd;
    amd.vendor = AMD, amd.rdna4 = true, amd.fsr4_built = true, amd.vram_mb = 16368, amd.screen_w = 3840, amd.screen_h = 2160;
    c = choose(amd);
    expect(c.tier == 5 && c.upscaler == FSR4 && c.resolution == RES_QUALITY && c.output_w == 3840 && c.output_h == 2160, "RX 9070 XT, 4K: Ultra, FSR 4, 4K");

    Hardware r6600;
    r6600.vendor = AMD, r6600.vram_mb = 8176, r6600.screen_w = 3840, r6600.screen_h = 2160;
    c = choose(r6600);
    expect(c.tier == 3 && c.upscaler == GAME && c.output_w == 0, "8 GB on a 4K screen: Balanced, the game's output");
    expect(std::strstr(c.why, "needs tier 4") != nullptr, c.why);

    Hardware six;
    six.vendor = NVIDIA, six.rtx = true, six.dlss_built = true, six.vram_mb = 6144, six.screen_w = 1920, six.screen_h = 1080;
    c = choose(six);
    expect(c.tier == 2 && c.upscaler == DLSS && c.resolution == RES_PERFORMANCE && c.output_w == 1920 && c.output_h == 1080,
           "RTX 6 GB, 1080p: Low, DLSS Performance, the screen's 1080p (fewer pixels than the game's own)");

    Hardware igpu;
    igpu.vendor = INTEL, igpu.integrated = true, igpu.vram_mb = 16000;
    c = choose(igpu);
    expect(c.tier == 1 && c.upscaler == GAME, "an integrated GPU: Minimum whatever memory it reports");

    Hardware deck;
    deck.vendor = AMD, deck.handheld = true, deck.vram_mb = 1024, deck.screen_w = 1280, deck.screen_h = 800;
    c = choose(deck);
    expect(c.tier == 1 && c.output_w == 1280 && c.output_h == 800 && std::strstr(c.why, "handheld"), "a Steam Deck: Minimum, its own 1280x800");

    Hardware gtx;
    gtx.vendor = NVIDIA, gtx.mesh_native = false, gtx.vram_mb = 8192;
    c = choose(gtx);
    expect(c.tier == 1 && std::strstr(c.why, "fallback"), "no mesh shaders: Minimum");

    // (a card that has mesh shaders, run as compute: Linux AMD's Automatic (#2245) or the Mesh shaders row's Compute. Its
    // tier is its memory's, and the run's mode is not in the fingerprint: changing the row does not make Auto choose again)
    Hardware radv;
    radv.vendor = AMD, radv.vram_mb = 16368, radv.screen_w = 2560, radv.screen_h = 1440, radv.mesh_as_compute = true;
    c = choose(radv);
    expect(c.tier == 5 && c.output_w == 2560, "RX 6800 on RADV, mesh shaders run as compute: Ultra, not Minimum");
    expect(std::strstr(c.why, "run as compute") && !std::strstr(c.why, "fallback"), c.why);
    Hardware radv_native = radv;
    radv_native.mesh_as_compute = false;
    expect(choose(radv_native).tier == c.tier, "the same card with mesh shaders native: the same tier");
    expect(fingerprint(radv, 0x1002, 0x73bf) == fingerprint(radv_native, 0x1002, 0x73bf),
           "the run's mesh mode: the same fingerprint");
    Hardware rx5700 = radv;
    rx5700.mesh_native = false, rx5700.mesh_as_compute = false;
    expect(choose(rx5700).tier == 1, "an RX 5700 (no mesh shaders, the same memory): Minimum");
    expect(fingerprint(rx5700, 0x1002, 0x73bf) != fingerprint(radv, 0x1002, 0x73bf), "mesh shaders or not: another fingerprint");

    // (High from 11.5 GB: 10 and 11 GB cards on Balanced, 12 GB cards (12,000-12,288 MB) on High)
    Hardware ten;
    ten.vendor = NVIDIA, ten.vram_mb = 10240;
    expect(choose(ten).tier == 3, "RTX 3080 10 GB: Balanced");
    ten.vram_mb = 11264;
    expect(choose(ten).tier == 3, "RTX 2080 Ti 11 GB: Balanced");
    ten.vram_mb = 11776;
    expect(choose(ten).tier == 3, "11.5 GB: Balanced");
    ten.vram_mb = 12000;
    expect(choose(ten).tier == 4, "a 12 GB card reporting 12,000 MB: High");
    ten.vram_mb = 9216;
    expect(choose(ten).tier == 3, "9 GB: Balanced");

    Hardware lowram;
    lowram.vendor = NVIDIA, lowram.vram_mb = 12282, lowram.ram_mb = 8192;
    c = choose(lowram);
    expect(c.tier == 2 && std::strstr(c.why, "12 GB of RAM"), "8 GB of RAM: held at Low");

    Hardware unknown;
    unknown.vram_mb = 24564;
    c = choose(unknown);
    expect(c.tier == 5 && c.output_w == 0, "no screen known: the game's output");

    expect(fingerprint(h, 0x10de, 0x2782) != fingerprint(h, 0x10de, 0x2783), "another GPU: another fingerprint");
    Hardware moved = h;
    moved.screen_w = 3840, moved.screen_h = 2160;
    expect(fingerprint(h, 0x10de, 0x2782) != fingerprint(moved, 0x10de, 0x2782), "another screen: another fingerprint");
    moved = h;
    moved.vram_mb += 3;   // (the heap's size wobbles by a few MB between drivers)
    expect(fingerprint(h, 0x10de, 0x2782) == fingerprint(moved, 0x10de, 0x2782), "a few MB more memory: the same fingerprint");

    // (the upscaler that runs: DLSS or FSR 4 chosen from the build, but not running on this PC)
    expect(kTierResolution[1] == 4 && kTierResolution[2] == 2 && kTierResolution[3] == 1 && ur::percent(kTierResolution[2]) == 75,
           "the tiers' own Render resolution: 50%, 75%, then 100%");
    {
        Hardware rtx = h;
        rtx.dlss_built = true;
        const Choice q = choose(rtx);   // (High, DLSS Quality 67%)
        const int chosen = ur::snap_render(q.upscaler, q.resolution);
        expect(resolution_for(q, GAME, chosen, chosen) == kTierResolution[q.tier],
               "DLSS chosen, the game's own runs: back to the tier's own (100%)");
        expect(resolution_for(q, DLSS, chosen, chosen) == 0, "DLSS chosen and running: as it is");
        expect(resolution_for(q, DLSS, kTierResolution[q.tier], chosen) == q.resolution,
               "DLSS running again at the tier's own: back to the choice");
        const int tuned = ur::step(GAME, kTierResolution[q.tier], 1);
        expect(resolution_for(q, GAME, tuned, chosen) == 0, "a step the tuning made: left alone");
        Hardware six_fsr;
        six_fsr.vendor = AMD, six_fsr.rdna4 = true, six_fsr.fsr4_built = true, six_fsr.vram_mb = 6144;
        const Choice l = choose(six_fsr);   // (Low, FSR 4 Performance 50%)
        expect(l.tier == 2 && resolution_for(l, GAME, ur::snap_render(l.upscaler, l.resolution),
                                             ur::snap_render(l.upscaler, l.resolution)) == kTierResolution[2],
               "FSR 4 chosen at Low, the game's own runs: Low's own 75%");
        expect(resolution_for(choose(r6600), GAME, 1, 1) == 0, "the game's own chosen: as it is");
    }

    // (the tuning in play)
    expect(best_step(DLSS) == RES_QUALITY && best_step(FSR4) == RES_QUALITY && best_step(3) == RES_QUALITY &&
           best_step(GAME) == 1 && best_step(6) == 1, "the ceiling: Quality with an upscaler, 100% with the game's own or Off");
    {
        Tuning t;
        for (int i = 0; i < kWindows - 1; i++) expect(tune(t, 17.5f, 900, 52, 60, true, true) == WAIT, "gathering");
        expect(tune(t, 17.5f, 900, 52, 60, true, true) == LIGHTER, "frames missed, the GPU over 90% of 16.7 ms: one step lighter");
        expect(t.gpu_mid == 17.5f && t.fps_mid == 52, "the middle values the verdict was made on, for the log");
        for (int i = 0; i < kWindows - 1; i++) tune(t, 6.0f, 900, 60, 60, true, true);
        expect(tune(t, 6.0f, 900, 60, 60, true, true) == KEEP && t.done, "after a step down, never back up: done");
    }
    {
        Tuning t;
        for (int i = 0; i < kWindows - 1; i++) tune(t, 15.8f, 900, 60, 60, true, true);
        expect(tune(t, 15.8f, 900, 60, 60, true, true) == KEEP && t.done,
               "the cap held with the GPU at 95%: the picture kept (no step down without missed frames)");
    }
    {
        Tuning t;
        for (int i = 0; i < kWindows; i++) tune(t, 20, 100, 30, 60, true, true);   // (a menu: under 150 draws)
        expect(t.n == 0 && !t.done, "menus and loading screens do not count");
        for (int i = 0; i < kWindows - 1; i++) tune(t, 7.0f, 900, 45, 60, true, true);
        expect(tune(t, 7.0f, 900, 45, 60, true, true) == CPU_BOUND && t.done, "45 fps with the GPU at 7 ms: the CPU holds it");
    }
    {
        Tuning t;
        for (int i = 0; i < kWindows - 1; i++) tune(t, 5.0f, 900, 60, 60, true, true);
        expect(tune(t, 5.0f, 900, 60, 60, true, true) == HEAVIER, "the cap held, the GPU at 30%: one step heavier");
        for (int i = 0; i < kWindows - 1; i++) tune(t, 5.0f, 900, 60, 60, true, false);
        expect(tune(t, 5.0f, 900, 60, 60, true, false) == KEEP, "at the best step: no further");
    }
    {
        Tuning t;
        for (int i = 0; i < kWindows - 1; i++) tune(t, 5.0f, 900, 50, 60, true, true);
        expect(tune(t, 5.0f, 900, 50, 60, true, true) == CPU_BOUND, "frames missed with the GPU idle: never a step up");
    }
    {
        Tuning t;
        int lighter = 0;
        for (int r = 0; r < 5; r++)
            for (int i = 0; i < kWindows; i++) lighter += tune(t, 25.0f, 900, 40, 60, true, true) == LIGHTER;
        expect(lighter == kMaxSteps && t.done, "at most two steps");
    }
    {
        Tuning t;
        for (int i = 0; i < kWindows - 1; i++) tune(t, 18.0f, 900, 55, 60, false, true);
        expect(tune(t, 18.0f, 900, 55, 60, false, true) == KEEP, "no lighter step left: keep");
    }
    {
        // (40 fps under a 60 cap with the GPU at 17 ms, over the 16.7 ms budget: the GPU's limit, not the CPU's, though it is
        // under 75% of the 25 ms frame shown)
        Tuning t;
        for (int i = 0; i < kWindows - 1; i++) tune(t, 17.0f, 900, 40, 60, true, true);
        expect(tune(t, 17.0f, 900, 40, 60, true, true) == LIGHTER, "40 fps, the GPU at 17 ms of 16.7: one step lighter");
    }
    {
        Tuning t;
        for (int i = 0; i < kWindows - 1; i++) tune(t, 14.0f, 900, 50, 60, true, true);
        expect(tune(t, 14.0f, 900, 50, 60, true, true) == CPU_BOUND && t.steps == 0,
               "50 fps, the GPU at 14 ms (under 0.9 of 16.7 and 0.75 of 20): the CPU holds it, nothing changed");
    }
    {
        // (the steps kept across restarts, as menu_init restores them from auto_steps=: after a step down, a new run with the cap
        // held and the GPU idle does not step back up)
        Tuning t;
        t.steps = 1, t.went_down = true;
        for (int i = 0; i < kWindows - 1; i++) tune(t, 5.0f, 900, 60, 60, true, true);
        expect(tune(t, 5.0f, 900, 60, 60, true, true) == KEEP, "restored after a step down: never back up");
        Tuning u;
        u.steps = kMaxSteps;
        for (int i = 0; i < kWindows - 1; i++) tune(u, 25.0f, 900, 40, 60, true, true);
        expect(tune(u, 25.0f, 900, 40, 60, true, true) == KEEP, "restored at two steps: no third");
    }

    // (the output by size, #2566's list: the screen's own where the tier carries its pixels)
    expect(output_tier(1366, 768) == 1 && output_tier(1920, 1080) == 1 && output_tier(1920, 1200) == 1 && output_tier(2560, 1440) == 3 && output_tier(2560, 1600) == 3 &&
           output_tier(3440, 1440) == 4 && output_tier(5120, 1440) == 4 && output_tier(5120, 2880) == 5, "the tier each size needs");
    {
        Hardware uw;
        uw.vendor = NVIDIA, uw.rtx = true, uw.dlss_built = true, uw.vram_mb = 12282, uw.screen_w = 3440, uw.screen_h = 1440;
        Choice u = choose(uw);
        expect(u.output_w == 3440 && u.output_h == 1440, "a 3440x1440 ultrawide at High: its own size");
        uw.vram_mb = 8192;
        u = choose(uw);
        expect(u.output_w == 0 && std::strstr(u.why, "needs tier 4"), "the same screen at Balanced: the game's");
    }
    {   // (a screen whose sides are not multiples of 8: its size as the menu offers it, each side rounded down to 8)
        namespace dm = mkwvk::display_modes;
        const int screens[][4] = {{1366, 768, 1360, 768}, {1600, 900, 1600, 896}, {1680, 1050, 1680, 1048}};
        for (const auto& s : screens) {
            Hardware lap;
            lap.vendor = NVIDIA, lap.rtx = true, lap.dlss_built = true, lap.vram_mb = 6144, lap.screen_w = s[0], lap.screen_h = s[1];
            const Choice l = choose(lap);
            const std::vector<dm::Size> menu = dm::offered({{(uint32_t)s[0], (uint32_t)s[1]}});
            const bool listed = std::find(menu.begin(), menu.end(), dm::Size{(uint32_t)l.output_w, (uint32_t)l.output_h}) != menu.end();
            expect(l.output_w == s[2] && l.output_h == s[3] && listed, "a screen not a multiple of 8: its size as the menu offers it");
        }
    }

    if (failures) return EXIT_FAILURE;
    std::printf("auto settings: all checks passed\n");
    return EXIT_SUCCESS;
}
