// (hookshot) Which gamepad buttons confirm and go back in the Insert menu: the ones the Switch's A and B are bound to (the
// Controls page, the Layout preset), so the menu answers to A and B as the game does. With the Nintendo layout, the default,
// A is the right face button and B the bottom one, as on a Switch; with Xbox / PC, A is the bottom one. A binding the menu
// cannot use (none, or not a face button: the menu keeps the D-pad, Home and the shoulders) falls back to the bottom button
// confirming and the right one going back. Plain arithmetic: tests/test_menu_keys.py checks it with the real menu_tick's use.
#pragma once

namespace mkwvk::menu_keys {

// (the gamepad's face buttons by position, as vk_api.h's HOS_PAD_SOUTH ... HOS_PAD_NORTH)
enum { kSouth = 0, kEast = 1, kWest = 2, kNorth = 3 };

/// a and b: the pad positions the Switch's A and B are bound to (-1 none); *confirm, *back: the menu's
inline void confirm_back(int a, int b, int* confirm, int* back) {
    const bool face_a = a >= kSouth && a <= kNorth, face_b = b >= kSouth && b <= kNorth;
    *confirm = face_a ? a : kSouth;
    *back = face_b && b != *confirm ? b : *confirm == kEast ? kSouth : kEast;
}

/// The button prompts' dot for a position (vk_ui.h glyph: 0 the top, 1 the right, 2 the left, 3 the bottom)
inline int dot(int position) {
    switch (position) {
    case kNorth: return 0;
    case kEast: return 1;
    case kWest: return 2;
    default: return 3;
    }
}

/// (hookshot) The button that pastes into a text field of the menu while it has the keyboard: the top face button, else the
/// first of the left, the right and the bottom that neither confirms nor goes back
inline int paste(int confirm, int back) {
    const int order[4] = {kNorth, kWest, kEast, kSouth};
    for (int i = 0; i < 4; i++)
        if (order[i] != confirm && order[i] != back) return order[i];
    return kNorth;
}

} // namespace mkwvk::menu_keys
