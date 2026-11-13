#include "../gpu/vk/dlss_checks.h"

#include <clocale>
#include <cstdio>
#include <string>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace dc = mkwvk::dlss_checks;

/// The menu's size for a share of the output in twelfths (gpu/vk/vk_menu.cpp game_size): what the game draws the 3D at
static void game_size(unsigned ow, unsigned oh, int k, unsigned& w, unsigned& h) { w = (ow * k + 11) / 12, h = oh * k / 12; }

static std::wstring decode(const std::string& s, bool& bad) {
    bad = false;
    return dc::utf8_to_wide(s, &bad);
}

int main() {
    // ---- the scene sizes DLSS takes for an output ----
    // (the validation report's case: the handheld profile's 1536x864 at Ultra Performance draws its scene at 512x288,
    // which the old 640x360 floor turned away)
    CHECK("handheld Ultra Performance", dc::scene_size_fits(512, 288, 1536, 864));
    CHECK("docked Ultra Performance", dc::scene_size_fits(683, 384, 2048, 1152));
    CHECK("handheld at the game's 3/4", dc::scene_size_fits(1152, 648, 1536, 864));
    CHECK("DLAA, the scene at full size", dc::scene_size_fits(1536, 864, 1536, 864));
    CHECK("1080p Ultra Performance", dc::scene_size_fits(640, 360, 1920, 1080));
    // Rounding the render size may put either axis up to two input pixels below one third.
    CHECK("one pixel under a third wide", dc::scene_size_fits(511, 288, 1536, 864));
    CHECK("two pixels under a third wide", dc::scene_size_fits(510, 288, 1536, 864));
    CHECK("one pixel under a third high", dc::scene_size_fits(512, 287, 1536, 864));
    CHECK("two pixels under a third high", dc::scene_size_fits(512, 286, 1536, 864));
    CHECK("two pixels under a third on both axes", dc::scene_size_fits(510, 286, 1536, 864));
    CHECK("rounded 1440p Ultra Performance", dc::scene_size_fits(853, 480, 2560, 1440));
    // every output the menu has, with every DLSS share (8, 7, 6, 4 twelfths) and DLAA's (12)
    const unsigned outputs[][2] = {{1536, 864}, {1920, 1080}, {2048, 1152}, {2560, 1440}, {3840, 2160}};
    for (const auto& o : outputs)
        for (int k : {12, 8, 7, 6, 4}) {
            unsigned w, h;
            game_size(o[0], o[1], k, w, h);
            CHECK("each mode at each output", dc::scene_size_fits(w, h, o[0], o[1]));
        }
    // what is still turned away
    CHECK("output not known yet", !dc::scene_size_fits(512, 288, 0, 0));
    CHECK("output not known yet (1080p scene)", !dc::scene_size_fits(1536, 864, 0, 0));
    CHECK("no size", !dc::scene_size_fits(0, 0, 1536, 864));
    CHECK("beyond two pixels under a third wide", !dc::scene_size_fits(509, 288, 1536, 864));
    CHECK("beyond two pixels under a third high", !dc::scene_size_fits(512, 285, 1536, 864));
    CHECK("a square shadow-map-like target", !dc::scene_size_fits(384, 384, 1536, 864));
    CHECK("a half-size pass of the handheld scene", !dc::scene_size_fits(256, 144, 1536, 864));
    CHECK("the old floor is under a third of 2048x1152", !dc::scene_size_fits(640, 360, 2048, 1152));
    CHECK("larger than the output", !dc::scene_size_fits(2048, 1152, 1536, 864));
    CHECK("wider than the output", !dc::scene_size_fits(1537, 864, 1536, 864));
    CHECK("no 32-bit overflow in the third", dc::scene_size_fits(0x60000000u, 1, 0xFFFFFFFFu, 3));

    // ---- UTF-8 paths as NGX's wide strings, in the C locale (the decoding must not depend on it) ----
    std::setlocale(LC_ALL, "C");
    // (which paths NGX's Linux library takes as they are: ASCII only. The others go to it as /proc/self/fd/<n>)
    CHECK("ASCII path", dc::ascii("/home/u/Games/Hookshot/ngx") && dc::ascii(""));
    CHECK("accented path", !dc::ascii("/home/u/Games/Hookshot-Caf\xC3\xA9/ngx"));
    CHECK("CJK path", !dc::ascii("/g/\xE3\x82\xBC"));
    CHECK("a byte that is not UTF-8", !dc::ascii("/g/\xFF"));
    bool bad = false;
    CHECK("ASCII", decode("/home/u/Games/Hookshot/ngx", bad) == L"/home/u/Games/Hookshot/ngx" && !bad);
    CHECK("accented", decode("/home/u/Games/Hookshot-Caf\xC3\xA9/ngx", bad) == L"/home/u/Games/Hookshot-Café/ngx" && !bad);
    CHECK("CJK", decode("/g/\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80/ngx", bad) == L"/g/ゼルダ/ngx" && !bad);
    // (U+1F3AE: one wchar_t where it has 32 bits, a surrogate pair where it has 16, as on Windows)
    const std::wstring gamepad = sizeof(wchar_t) == 2 ? std::wstring{(wchar_t)0xD83C, (wchar_t)0xDFAE}
                                                      : std::wstring(1, (wchar_t)0x1F3AE);
    CHECK("four bytes", decode("/g/\xF0\x9F\x8E\xAE", bad) == L"/g/" + gamepad && !bad);
    CHECK("all together",
          decode("/home/u/Games/Hookshot-dlss-utf8-Caf\xC3\xA9-\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80/ngx", bad) ==
              L"/home/u/Games/Hookshot-dlss-utf8-Café-ゼルダ/ngx" && !bad);
    // (the bug this replaces: each byte widened alone)
    const std::string cafe = "Caf\xC3\xA9";
    CHECK("the old widening was wrong", std::wstring(cafe.begin(), cafe.end()) != decode(cafe, bad));
    // bytes that are not UTF-8: U+FFFD, one for each maximal ill-formed part, and *bad set
    CHECK("a Latin-1 byte", decode("a\xE9" "b", bad) == L"a�b" && bad);
    CHECK("a lone continuation", decode("\x80x", bad) == L"�x" && bad);
    CHECK("cut at the end", decode("x\xC3", bad) == L"x�" && bad);
    CHECK("cut in the middle", decode("\xE3\x82x", bad) == L"�x" && bad);
    CHECK("overlong", decode("\xC0\xAF", bad) == L"��" && bad);
    CHECK("overlong three", decode("\xE0\x80\xAF", bad) == L"���" && bad);
    CHECK("surrogate", decode("\xED\xA0\x80", bad) == L"���" && bad);
    CHECK("past U+10FFFF", decode("\xF4\x90\x80\x80", bad) == L"����" && bad);
    CHECK("F5 and up", decode("\xF5\xFF", bad) == L"��" && bad);
    CHECK("good after bad", decode("\xFF\xC3\xA9", bad) == L"�é" && bad);
    CHECK("no flag asked", dc::utf8_to_wide("\xFF") == L"�");
    CHECK("empty", decode("", bad).empty() && !bad);
    // ---- which DLSS mode takes a render size (vk_dlss.cpp prepare) ----
    {
        // (a 4416x1864 output, the 3840x2160-pixel widescreen picture on a 21:9 screen: NGX's ranges there. Quality,
        // Balanced and Performance share 50-100 %; Ultra Performance takes its third alone)
        const dc::ModeRange r[] = {{2208, 932, 4416, 1864}, {1472, 621, 1472, 621}};
        dc::ModeRange took;
        CHECK("Performance's half", dc::mode_for(2208, 932, 4416, 1864, r, 2, &took) == 0 && took.max_w == 4416);
        CHECK("Ultra Performance's exact third", dc::mode_for(1472, 621, 4416, 1864, r, 2, &took) == 1 && took.max_h == 621);
        // (the game's third of 1864 came out 622: no mode took it, and the game's own network upscaled in DLSS's place)
        CHECK("Ultra Performance's third rounded up",
              dc::mode_for(1472, 622, 4416, 1864, r, 2, &took) == 1 && took.min_h == 621 && took.max_h == 622 &&
              took.min_w == 1472 && took.max_w == 1472);
        // (a size between the two ranges is no mode's: 42 % between a third and a half)
        CHECK("between the modes", dc::mode_for(1856, 784, 4416, 1864, r, 2, &took) == -1);
        // (rounding below a third is also allowed within the scene-size tolerance, widening both axes)
        CHECK("Ultra Performance's third rounded down",
              dc::mode_for(1471, 620, 4416, 1864, r, 2, &took) == 1 && took.min_w == 1471 && took.max_w == 1472 &&
              took.min_h == 620 && took.max_h == 621);
        // (past the two-input-pixel allowance on either axis: never widened into Ultra Performance)
        CHECK("below a third beyond width tolerance", dc::mode_for(1469, 620, 4416, 1864, r, 2, &took) == -1);
        CHECK("below a third beyond height tolerance", dc::mode_for(1471, 619, 4416, 1864, r, 2, &took) == -1);
        // (a 2560x1440 output's Ultra Performance, 853x480: the game's twelfths make 854 (game_size above))
        const dc::ModeRange q[] = {{1280, 720, 2560, 1440}, {853, 480, 853, 480}};
        unsigned w = 0, h = 0;
        game_size(2560, 1440, 4, w, h);
        CHECK("1440p Ultra Performance, the menu's size", dc::mode_for(w, h, 2560, 1440, q, 2, &took) == 1);
        // (a range NGX did not give (zeros) is never widened into)
        const dc::ModeRange z[] = {{0, 0, 0, 0}};
        CHECK("no range", dc::mode_for(1472, 622, 4416, 1864, z, 1, &took) == -1);
    }
    std::puts("dlss checks: ok");
    return 0;
}
