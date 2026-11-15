/* Damage taken at the player's life write (nx2/oot_patches.py patch_life).
   Life is in quarter hearts. Normal preserves the game's result; Half rounds
   damage up so even a one-quarter hit still hurts. Healing is never scaled.
   The damage site supplies the full hit before the game clamps next to zero. */
#include "../gpu/vk/vk_api.h"

int rx_option(int id, int otherwise);
int hos_oot_hearts_enabled(void);

uint32_t hos_oot_damage(uint32_t old, uint32_t next, uint32_t hit) {
    if (next >= old) return next;
    if (hos_oot_hearts_enabled()) return old;
    const int setting = rx_option(HOS_OPT_DAMAGE_TAKEN, 1);
    uint64_t damage = hit;
    switch (setting) {
    case 0: damage = (damage + 1) / 2; break;
    case 2: damage *= 2; break;
    case 3: damage *= 4; break;
    default: return next;
    }
    /* Widen before multiplying, then saturate: no underflow on lethal hits. */
    return damage >= old ? 0 : old - (uint32_t)damage;
}
