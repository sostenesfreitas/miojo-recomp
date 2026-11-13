// The Insert menu's hit-test (gpu/vk/vk_menu_hit.h): the controls as recorded while drawing (Map::add), and where a mouse
// position falls on them, in the window image's pixels: the tabs, a row's name and its value's halves, a slider's track and a
// drag along it, the Controls page's cells, a list line, the restart question's buttons; off the controls and off the menu;
// through a window whose pixels are not the swapchain's.
#include "../gpu/vk/vk_menu_hit.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

using namespace mkwvk::menu_hit;

int main() {
    Map m;
    Hit h;
    // ---- the options page of a 1920 x 1080 window: the tab keys, three tabs, rows 40 high every 48 from y 100 ----
    m.clear(0, 0, 1920, 1080);
    m.add(0, 0, 640, 87, kTabPrev, -1);
    m.add(1280, 0, 1920, 87, kTabNext, -1);
    for (int i = 0; i < 3; i++) m.add(660 + i * 200, 0, 860 + i * 200, 87, kTab, i);
    for (int i = 0; i < 10; i++) {
        const float y = 100 + i * 48;
        if (i == 7) m.add(100, y, 1000, y + 40, kRow, i, 600, 1000, 640, 960);   // (Sharpness: a slider's track 640 to 960)
        else m.add(100, y, 1000, y + 40, kRow, i, 600, 1000);
    }
    h = m.at(100, 40);
    CHECK("tab key, left", h.part == kTabPrev);
    h = m.at(1500, 40);
    CHECK("tab key, right", h.part == kTabNext);
    h = m.at(700, 40);
    CHECK("tab 0", h.part == kTab && h.index == 0);
    h = m.at(1000, 40);
    CHECK("tab 1", h.part == kTab && h.index == 1);
    h = m.at(1200, 40);
    CHECK("tab 2", h.part == kTab && h.index == 2);
    h = m.at(200, 110);
    CHECK("row 0's name", h.part == kRow && h.index == 0 && h.side == 0);
    h = m.at(650, 110 + 48 * 3);
    CHECK("row 3's value, left half", h.part == kRow && h.index == 3 && h.side == -1);
    h = m.at(950, 110 + 48 * 3);
    CHECK("row 3's value, right half", h.part == kRow && h.index == 3 && h.side == 1);
    h = m.at(500, 100 + 48 * 2 + 44);
    CHECK("between two rows: the menu, no control", h.part == kNone && h.index == -1);
    h = m.at(800, 110 + 48 * 7);
    CHECK("the slider's track, its middle", h.part == kSlider && h.index == 7 && h.permille == 500);
    h = m.at(636, 110 + 48 * 7);
    CHECK("just before the track: its start", h.part == kSlider && h.permille == 0);
    h = m.at(965, 110 + 48 * 7);
    CHECK("just past the track: its end", h.part == kSlider && h.permille == 1000);
    h = m.at(620, 110 + 48 * 7);
    CHECK("the slider row's value, off the track", h.part == kRow && h.index == 7 && h.side == -1);
    CHECK("a drag along the track", m.drag(720, 7) == 250);
    CHECK("a drag past either end", m.drag(100, 7) == 0 && m.drag(1900, 7) == 1000);
    CHECK("a drag on a row without a track", m.drag(720, 3) == -1);
    CHECK("the slider's value", slider_value(m.drag(720, 7), 0, 100, 10) == 30 && slider_value(1000, 0, 100, 10) == 100);
    CHECK("the knob's place", slider_permille(60, 0, 100) == 600 && slider_permille(500, 10, 300) == 1000);

    // ---- the Controls page: a name, two gamepad cells and a keyboard cell per row, then Reset to defaults ----
    m.clear(0, 0, 1920, 1080);
    for (int i = 0; i < 3; i++) {
        const float y = 120 + i * 42;
        m.add(100, y, 400, y + 36, kCtlRow, i);
        m.add(400, y, 547, y + 36, kCtlCell, i, 0, 0, 0, 0, kCtlPrimary);
        m.add(553, y, 697, y + 36, kCtlCell, i, 0, 0, 0, 0, kCtlAlternate);
        m.add(703, y, 1000, y + 36, kCtlCell, i, 0, 0, 0, 0, kCtlKeyboard);
    }
    m.add(100, 120 + 3 * 42, 390, 120 + 3 * 42 + 36, kCtlRow, 3);
    h = m.at(200, 130 + 42);
    CHECK("a control's name", h.part == kCtlRow && h.index == 1);
    h = m.at(500, 130 + 84);
    CHECK("its primary gamepad cell", h.part == kCtlCell && h.index == 2 && h.side == kCtlPrimary);
    h = m.at(620, 130 + 84);
    CHECK("its alternate gamepad cell", h.part == kCtlCell && h.index == 2 && h.side == kCtlAlternate);
    CHECK("the gap between gamepad cells", m.at(550, 130 + 84).part == kNone);
    h = m.at(800, 130);
    CHECK("its keyboard cell", h.part == kCtlCell && h.index == 0 && h.side == kCtlKeyboard);
    h = m.at(200, 130 + 126);
    CHECK("Reset to defaults", h.part == kCtlRow && h.index == 3);
    m.add(100, 60, 1000, 96, kCtlOption, 1, 600, 1000);   // (a row of the page's own, above the grid: Button prompts)
    h = m.at(650, 70);
    CHECK("the page's own row, its value's left half", h.part == kCtlOption && h.index == 1 && h.side == -1);
    h = m.at(200, 70);
    CHECK("its name", h.part == kCtlOption && h.side == 0);

    // ---- a list line with an amount (its halves), and one without ----
    m.clear(0, 0, 1920, 1080);
    m.add(100, 100, 1000, 130, kListLine, 0, 900, 1000);
    m.add(100, 134, 1000, 164, kListLine, 1);
    h = m.at(920, 110);
    CHECK("an amount, its left half", h.part == kListLine && h.index == 0 && h.side == -1);
    h = m.at(990, 110);
    CHECK("an amount, its right half", h.side == 1);
    h = m.at(990, 140);
    CHECK("a line without an amount", h.part == kListLine && h.index == 1 && h.side == 0);

    // ---- the restart question: the menu is the dialog; two buttons ----
    m.clear(680, 424, 1240, 656);
    m.add(706, 584, 916, 630, kButton, 0);
    m.add(940, 584, 1150, 630, kButton, 1);
    h = m.at(800, 600);
    CHECK("Later", h.part == kButton && h.index == 0);
    h = m.at(1000, 600);
    CHECK("Restart now", h.part == kButton && h.index == 1);
    h = m.at(900, 450);
    CHECK("the dialog, off its buttons", h.part == kNone);
    h = m.at(100, 100);
    CHECK("off the dialog", h.part == kOutside);

    // ---- what was drawn last is on top; the map holds kMax boxes and drops the rest ----
    m.clear(0, 0, 100, 100);
    m.add(0, 0, 100, 100, kNone, -1);
    m.add(10, 10, 20, 20, kButton, 1);
    h = m.at(15, 15);
    CHECK("the last drawn, on top", h.part == kButton);
    for (int i = 0; i < Map::kMax + 10; i++) m.add(0, 0, 1, 1, kRow, i);
    CHECK("no more than kMax", m.n == Map::kMax);

    // ---- a window of 1280 x 720 pixels over a 2560 x 1440 image (the swapchain not yet resized, or a scaled window) ----
    int ix = 0, iy = 0;
    to_image(640, 360, 1280, 720, 2560, 1440, &ix, &iy);
    CHECK("to the image's pixels", ix == 1280 && iy == 720);
    to_image(5, 7, 0, 0, 2560, 1440, &ix, &iy);
    CHECK("no window size: as it is", ix == 5 && iy == 7);

    // ---- a click on an option's row: the name selects it and leaves the value (a click on "Output resolution" to read its
    // help stepped it, and closing the menu then asked to restart for a resolution never chosen) ----
    CHECK("the name of a choice: select only", row_click(true, false, 0) == kClickSelect);
    CHECK("the name of a toggle: select only", row_click(true, true, 0) == kClickSelect);
    CHECK("a choice's halves", row_click(true, false, -1) == kClickLeft && row_click(true, false, 1) == kClickRight);
    CHECK("a toggle's other half flips it", row_click(true, true, -1, true) == kClickRight && row_click(true, true, 1, false) == kClickRight);
    CHECK("a toggle's half already chosen leaves it (Docked clicked while Docked)",
          row_click(true, true, -1, false) == kClickSelect && row_click(true, true, 1, true) == kClickSelect);
    CHECK("a page anywhere opens", row_click(false, false, 0) == kClickConfirm && row_click(false, false, 1) == kClickConfirm);
    {   // (as drawn: the name from 0 to 100, the value from 100 to 300)
        Map rows;
        rows.clear(0, 0, 400, 100);
        rows.add(0, 0, 300, 40, kRow, 3, 100, 300);
        const Hit name = rows.at(50, 20), left = rows.at(150, 20), right = rows.at(250, 20);
        CHECK("the name is side 0", name.part == kRow && name.index == 3 && name.side == 0);
        CHECK("a click on the name changes nothing", row_click(true, false, name.side) == kClickSelect);
        CHECK("the value's halves", row_click(true, false, left.side) == kClickLeft && row_click(true, false, right.side) == kClickRight);
    }

    std::printf("PASS: the hit-test over the controls as drawn\n");
    return 0;
}
