/* The Insert menu's rows that take a restart (vk_menu.cpp: GPU device, Output resolution, Profile, Aspect ratio,
   Robust buffers), as plain code: which of them differ from what the game started with, whether closing the menu asks to
   restart, and the rows' names as the menu lists them. No Vulkan, no menu: tests/test_menu_restart.cpp checks it. */
#pragma once

#include <string>

namespace mkwvk::menu_restart {

constexpr int kMax = 16;   // (rows that take a restart: the menu has ten; a row past kMax would have no slot)

struct State {
    int n = 0;
    int start[kMax] = {};      // each row's value at the start: what runs
    int declined[kMax] = {};   // the values when the player last chose Later
    bool later = false;        // Later was chosen, and nothing has changed since

    /// A row that takes a restart, with the value the game started with: its slot
    int add(int start_value) {
        if (n >= kMax) return -1;
        start[n] = start_value;
        return n++;
    }
    /// The slot's start value again (the GPU row's, once the cards are counted)
    void restart_value(int slot, int start_value) {
        if (slot >= 0 && slot < n) start[slot] = start_value;
    }
    /// The rows (a bit a slot) whose value now differs from the start's: a restart is pending while it is not 0. A row
    /// changed back to its start value is no longer pending
    unsigned pending(const int* now) const {
        unsigned m = 0;
        for (int i = 0; i < n; i++)
            if (now[i] != start[i]) m |= 1u << i;
        return m;
    }
    /// After any change of a row: a value other than the ones Later was chosen for asks again at the next close (even if
    /// it is changed back to them later)
    void note(const int* now) {
        if (!later) return;
        for (int i = 0; i < n; i++)
            if (now[i] != declined[i]) later = false;
    }
    /// Closing the menu: ask to restart when something is pending, unless Later was chosen for the same values
    bool ask(const int* now) {
        note(now);
        return pending(now) && !later;
    }
    /// Later: the settings stay saved for the next start, and the same choice is not asked about again
    void choose_later(const int* now) {
        later = true;
        for (int i = 0; i < n; i++) declined[i] = now[i];
    }
};

/// The names of the rows in mask (a bit a slot, names[slot]), in slot order: "A, B" with sep ", ", the last one after
/// last ("A, B and C" with last " and ")
inline std::string names(const char* const* names, int n, unsigned mask, const char* sep = ", ", const char* last = ", ") {
    std::string s;
    int left = 0;
    for (int i = 0; i < n; i++) left += (mask >> i) & 1;
    for (int i = 0; i < n; i++) {
        if (!((mask >> i) & 1)) continue;
        if (!s.empty()) s += left == 1 ? last : sep;
        s += names[i];
        left--;
    }
    return s;
}

} // namespace mkwvk::menu_restart
