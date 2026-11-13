// The Insert menu's mouse: what a point over it is on. The menu records each control's rectangle as it draws it (Map::add, in
// the window image's pixels), and the next frame's mouse is tested against them (Map::at): the picture and the hit-test cannot
// drift apart, at any size. Plain arithmetic, no Vulkan or ImGui: tests/test_menu_hit.cpp checks it.
#pragma once

namespace mkwvk::menu_hit {

enum Part {
    kNone,      // off the menu's controls (still over the menu: a click does nothing)
    kOutside,   // off the menu
    kTabPrev,   // the tab strip's left end
    kTab,       // a tab: index
    kTabNext,   // its right end
    kRow,       // an option's row: index (the tab's own), side -1 the value's left half, 1 its right half, 0 the name
    kSlider,    // a slider row's track: index, permille 0 to 1000 along it
    kCtlOption, // the Controls page: one of its own rows above the grid (index), side -1 the value's left half, 1 its right
                // half, 0 the name
    kCtlRow,    // the Controls page: a row's name (index; index == the count: Reset to defaults)
    kCtlCell,   // a binding: index the row, side the CtlColumn
    kListLine,  // a list (the teleport list, the inventory): a line as shown (index, 0 the top one), side -1 the left half, 1 the right half
    kButton,    // the restart question's buttons: index 0 Later, 1 Restart now
};
enum CtlColumn { kCtlPrimary, kCtlAlternate, kCtlKeyboard, kCtlKeyboard2, kCtlColumns };
/// A key column's slot of the host's two keys per control (0 or 1); -1 for a gamepad column
constexpr int ctl_key_slot(int col) { return col >= kCtlKeyboard ? col - kCtlKeyboard : -1; }
/// The column a row's selection is in: a keys-only row (its gamepad cells off) keeps to the key columns
constexpr int ctl_column(int col, bool keys_only) { return keys_only && col < kCtlKeyboard ? kCtlKeyboard : col; }
/// The column after Left (dir -1) or Right (1): round all four, or round the two key columns on a keys-only row
constexpr int ctl_step(int col, int dir, bool keys_only) {
    return keys_only ? kCtlKeyboard + (ctl_key_slot(ctl_column(col, true)) + 1) % 2 : (col + kCtlColumns + dir) % kCtlColumns;
}
struct Hit {
    Part part = kNone;
    int index = -1;
    int side = 0;
    int permille = 0;
};

/// A point in the window's pixels (the cursor's, in a window win_w x win_h pixels) in the window's image (img_w x img_h:
/// the swapchain's; the same size as a rule, but not while a resize is on its way)
inline void to_image(int x, int y, int win_w, int win_h, int img_w, int img_h, int* ix, int* iy) {
    *ix = win_w > 0 ? (int)((long long)x * img_w / win_w) : x;
    *iy = win_h > 0 ? (int)((long long)y * img_h / win_h) : y;
}
/// A slider's value for a point along its track (lo to hi in steps of step), nearest step, held to its ends
inline int slider_value(int permille, int lo, int hi, int step) {
    if (step <= 0 || hi <= lo) return lo;
    const int steps = (hi - lo) / step;
    const int k = (permille * steps + 500) / 1000;
    return lo + (k < 0 ? 0 : k > steps ? steps : k) * step;
}
/// Where along the track a value is, in permille (the knob's place)
inline int slider_permille(int v, int lo, int hi) {
    if (hi <= lo) return 0;
    const int p = (int)((long long)(v - lo) * 1000 / (hi - lo));
    return p < 0 ? 0 : p > 1000 ? 1000 : p;
}

/// What a left click on an option's row does after it selects the row (side: the Hit's): kClickSelect nothing more, kClickRight
/// and kClickLeft as Right and Left, kClickConfirm as Enter. The name only selects the row and shows its help: a click there
/// used to change the value as Right, so clicking "Output resolution" to read about it moved it a step, and closing the menu
/// asked to restart. A choice's halves lower and raise it; a toggle's halves are its two values (left lo, right hi), so the half already
/// chosen leaves it as it is (on: the toggle is at hi) and the other flips it. A row without a value (a page, Restart, Quit)
/// opens
enum RowClick { kClickLeft = -1, kClickSelect = 0, kClickRight = 1, kClickConfirm = 2 };
inline RowClick row_click(bool has_value, bool toggle, int side, bool on = false) {
    if (!has_value) return kClickConfirm;
    if (side == 0) return kClickSelect;
    if (toggle) return (side > 0) == on ? kClickSelect : kClickRight;   // (a toggle's Right flips it either way)
    return side > 0 ? kClickRight : kClickLeft;
}

/// The controls as last drawn
struct Map {
    struct Box {
        float x0, y0, x1, y1;
        Part part;
        int index;
        float v0, v1;   // (a value's span, for its halves: side -1 left of the middle, 1 right; v0 >= v1: none)
        float t0, t1;   // (a slider's track: kSlider along it; t0 >= t1: none)
        int side;       // (a fixed side: the Controls page's cells)
    };
    static constexpr int kMax = 96;
    static constexpr float kGrab = 6;   // (a click this far past either end of a track still takes the end, in pixels)
    Box box[kMax];
    int n = 0;
    float ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;   // (the menu's area: off it, kOutside)

    void clear(float x0, float y0, float x1, float y1) { n = 0, ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1; }
    void add(float x0, float y0, float x1, float y1, Part part, int index, float v0 = 0, float v1 = 0, float t0 = 0, float t1 = 0,
             int side = 0) {
        if (n < kMax) box[n++] = {x0, y0, x1, y1, part, index, v0, v1, t0, t1, side};
    }
    Hit at(float x, float y) const {
        Hit hit;
        if (x < ax0 || y < ay0 || x >= ax1 || y >= ay1) return hit.part = kOutside, hit;
        for (int i = n - 1; i >= 0; i--) {   // (the last drawn on top)
            const Box& b = box[i];
            if (x < b.x0 || y < b.y0 || x >= b.x1 || y >= b.y1) continue;
            hit.part = b.part, hit.index = b.index, hit.side = b.side;
            if (b.t0 < b.t1 && x >= b.t0 - kGrab && x < b.t1 + kGrab) {
                hit.part = kSlider;
                hit.permille = permille(b, x);
                return hit;
            }
            if (b.v0 < b.v1 && x >= b.v0 && x < b.v1) hit.side = x < (b.v0 + b.v1) / 2 ? -1 : 1;
            return hit;
        }
        return hit;
    }
    /// A drag that started on row's slider: the point's place along its track, held to its ends, wherever the cursor went;
    /// -1 when the row has no track now
    int drag(float x, int row) const {
        for (int i = 0; i < n; i++)
            if (box[i].part == kRow && box[i].index == row && box[i].t0 < box[i].t1) return permille(box[i], x);
        return -1;
    }
    static int permille(const Box& b, float x) {
        const int p = (int)((x - b.t0) * 1000 / (b.t1 - b.t0));
        return p < 0 ? 0 : p > 1000 ? 1000 : p;
    }
};

} // namespace mkwvk::menu_hit
