"""Damage taken: real arithmetic and life-write guards without a game dump."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches as patches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    options = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    assert "HOS_OPT_DAMAGE_TAKEN = 56" in api
    assert "int damage_taken = 1;" in options
    assert '{TAB_DEBUG, "Damage taken", &opts.damage_taken, 0, 3, 1, tx_damage_taken' in menu
    assert '{TAB_GAMEPLAY, "Damage taken"' not in menu
    assert '"Half", "Normal", "Double", "Quadruple"' in menu
    # Hidden means Normal: with the Debug tools off the host is told Normal, whatever the saved choice.
    assert "if (id == HOS_OPT_DAMAGE_TAKEN) return menu_damage_taken();" in replay
    assert "int menu_damage_taken() { return cheats_on() ? opts.damage_taken : 1; }" in menu
    source = '#include "a64.h"\n' + patches.LIFE_HEAD + patches.LIFE_CAS + '\n' + patches.LIFE_CAS + '\n}\n'
    made = patches.patch_life(source)
    assert made and made != source
    assert patches.patch_life(made) == made, "Applying the patch twice must not scale twice"
    assert patches.patch_life(source.replace(patches.LIFE_CAS, '', 1)) is None
    assert patches.patch_life(source.replace(patches.LIFE_HEAD, 'void unrelated(void) {\n')) is None
    assert patches.patch_file(source, 'authored.c', protect_hearts=False)[0] == source
    assert patches.patch_file(source, 'authored.c', protect_hearts=True)[0] == made
    # Compile the guard from each injected site, not a reimplementation of it.
    body = made.split(patches.LIFE_HEAD)[1].rsplit('\n}', 1)[0]
    guards = body.split(patches.LIFE_CAS)[:2]
    host = (ROOT / 'host/oot_damage.c').read_text(encoding='utf-8')
    host = '\n'.join(line for line in host.splitlines() if not line.startswith('#include'))
    head = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
enum { HOS_OPT_DAMAGE_TAKEN = 55 };
static int setting = 1, protection, player = 1;
int rx_option(int id, int otherwise) { assert(id == 55 && otherwise == 1); return setting; }
int hos_oot_hearts_enabled(void) { return protection; }
typedef struct { uint64_t x[32]; } Ctx;
static int hos_oot_is_player(Ctx *c, uint64_t actor) { (void)c; return player && actor == 100; }
static uint64_t recomp_load64(Ctx *c, uint64_t address) { (void)c; assert(address == 100 + 7184); return 200; }
'''
    main_c = r'''
int main(void) {
    for (setting = 0; setting < 4; setting++) {
        for (uint32_t old = 0; old <= 80; old++) {
            for (uint32_t next = 0; next <= 80; next++) {
                uint32_t expected = next;
                if (next < old) {
                    uint32_t damage = old - next;
                    damage = setting == 0 ? (damage + 1) / 2 : damage * (1u << (setting - 1));
                    expected = damage >= old ? 0 : old - damage;
                }
                protection = 0;
                assert(hos_oot_damage(old, next, old - next) == expected);
                protection = 1;
                assert(hos_oot_damage(old, next, old - next) == (next < old ? old : next));
            }
        }
        protection = 0;
        assert(hos_oot_damage(1, 0, 1) == 0); /* Even Half keeps a one-quarter hit. */
        assert(hos_oot_damage(UINT32_MAX, 0, UINT32_MAX) == (setting == 0 ? UINT32_MAX / 2 : 0));
        for (int which = 0; which < 2; which++) {
            Ctx c = {{0}}; c.x[9] = 4; c.x[11] = 12; c.x[12] = 8; c.x[19] = 100; c.x[13] = 208;
            site(&c, which); assert(c.x[12] == hos_oot_damage(12, 8, 4));
            c.x[12] = 8; player = 0; site(&c, which); assert(c.x[12] == 8); player = 1;
            c.x[13] = 209; site(&c, which); assert(c.x[12] == 8); c.x[13] = 208;
            protection = 1; site(&c, which); assert(c.x[12] == 12); protection = 0;
            c.x[12] = 16; site(&c, which); assert(c.x[12] == 16);
        }
    }
    protection = 0;
    setting = 0;
    Ctx lethal = {{0}};
    lethal.x[9] = 4; lethal.x[11] = 2; lethal.x[12] = 0;
    lethal.x[19] = 100; lethal.x[13] = 208;
    site(&lethal, 1); /* Damage site's raw four-unit hit, with only two units left. */
    assert(lethal.x[12] == 0); /* Half of four is two: still lethal. */
    /* Test every lethal and nonlethal raw hit, including zero and odd quarters. */
    for (setting = 0; setting < 4; setting++) {
        for (uint32_t old = 0; old <= 80; old++) {
            for (uint32_t hit = 0; hit <= 160; hit++) {
                uint32_t next = hit >= old ? 0 : old - hit;
                uint32_t scaled = setting == 0 ? (hit + 1) / 2 : hit * (1u << (setting - 1));
                uint32_t expected = scaled >= old ? 0 : old - scaled;
                assert(hos_oot_damage(old, next, hit) == expected);
                lethal.x[9] = hit; lethal.x[11] = old; lethal.x[12] = next;
                site(&lethal, 1); assert(lethal.x[12] == expected);
                assert(lethal.x[9] == hit); /* Hook must not modify the retry's raw hit. */
                lethal.x[12] = next; /* A failed CAS recomputes next from old and hit. */
                site(&lethal, 1); assert(lethal.x[12] == expected);
            }
        }
    }
    setting = 0;
    lethal.x[9] = UINT32_MAX; lethal.x[11] = 2; lethal.x[12] = 0;
    site(&lethal, 1); assert(lethal.x[12] == 0);
    lethal.x[12] = 4; site(&lethal, 0); assert(lethal.x[12] == 4); /* Heal ignores x9. */
    protection = 1; lethal.x[12] = 0; site(&lethal, 1); assert(lethal.x[12] == 2);
    protection = 0;
    /* A failed CAS can supply a different old life, but must not shrink the hit. */
    lethal.x[9] = 4; lethal.x[11] = 6; lethal.x[12] = 2;
    site(&lethal, 1); assert(lethal.x[12] == 4 && lethal.x[9] == 4);
    lethal.x[11] = 2; lethal.x[12] = 0;
    site(&lethal, 1); assert(lethal.x[12] == 0 && lethal.x[9] == 4);
    setting = -1; assert(hos_oot_damage(12, 8, 4) == 8);
    setting = 4; assert(hos_oot_damage(12, 8, 4) == 8);
    puts("PASS: damage arithmetic, lethal/quarter hits, healing, protection, both player guards and option wiring");
}
'''
    site = 'static void site(Ctx *c, int which) { if (which) {\n' + guards[0] + '\n} else {\n' + guards[1] + '\n} }\n'
    with tempfile.TemporaryDirectory(prefix='oot-damage-') as temporary:
        c = Path(temporary) / 'damage.c'
        exe = Path(temporary) / 'damage.exe'
        c.write_text(head + host + site + main_c, encoding='utf-8')
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
