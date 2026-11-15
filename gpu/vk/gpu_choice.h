/* Which GPU the Insert menu's GPU device row asks for (vk_main.cpp's pick_device). RX_VK_DEVICE, when set, wins over it
   there. The row is saved as the card itself (gpu_device_id=, card_id), not only its place in the list (gpu_device=): the
   place changes when a GPU is added or taken out, a driver lists the cards in another order, or Windows' own GPU choice for
   the program is lost with an update into a new folder, and the same number then picked another card. */
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mkwvk::gpu_choice {

/// The row's choice as a position in the Vulkan 1.3 devices (0-based), or -1 for Auto. menu: opts.gpu_device (0 Auto,
/// n the n-th usable device); usable: how many there are now. A choice past them (a GPU taken out since) is Auto.
inline int menu_device(int menu, int usable) { return menu > 0 && menu <= usable ? menu - 1 : -1; }

/// A usable GPU as Vulkan names its model: VkPhysicalDeviceProperties' vendorID and deviceID (the PCI ids)
struct Card {
    uint32_t vendor = 0, device = 0;
};

/// The card at index (0-based) of cards as the options file keeps it: "<vendor>:<device>:<n>" in lower-case hex, n the
/// how-manieth card of that model in the list (0 the first), so two of the same card stay apart. "" for none
inline std::string card_id(const std::vector<Card>& cards, int index) {
    if (index < 0 || index >= (int)cards.size()) return "";
    int n = 0;
    for (int i = 0; i < index; i++) n += cards[i].vendor == cards[index].vendor && cards[i].device == cards[index].device;
    char id[40];
    std::snprintf(id, sizeof id, "%x:%x:%d", (unsigned)cards[index].vendor, (unsigned)cards[index].device, n);
    return id;
}

/// The row's value (1-based, opts.gpu_device) of the card a saved id names in this run's cards: 0 when it is not there (taken
/// out, or an id that is not one). The same model's n-th; with fewer of it now, its last
inline int menu_of_card(const std::vector<Card>& cards, const std::string& id) {
    unsigned vendor = 0, device = 0;
    int n = 0, used = 0;
    if (std::sscanf(id.c_str(), "%x:%x:%d%n", &vendor, &device, &n, &used) != 3 || used != (int)id.size() || n < 0) return 0;
    int found = 0, seen = 0;
    for (int i = 0; i < (int)cards.size(); i++)
        if (cards[i].vendor == vendor && cards[i].device == device) {
            found = i + 1;
            if (seen++ == n) return found;
        }
    return found;
}

} // namespace mkwvk::gpu_choice
