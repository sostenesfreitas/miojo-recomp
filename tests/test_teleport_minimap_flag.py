"""The debug teleport's minimap repair survives a save and reload (issues 2217, 2588).

An area's saved MapIconDeactivateByAreaActor flag can outlive a debug teleport. Clearing only its live byte restores
the HUD but leaves the save's copy hidden. The teleport must use the game's queued Bool setter, which owns both.

Linux only (the stand-in game is a mapping of the guest's addresses). Builds oot_teleport and what it calls from the
source over a stand-in GameDataMgr, event manager and guest functions. The fixture keeps the live flags and serialized
flags separate: only processing an engine-setter request updates both. It checks:

- an accepted teleport queues the repair, preserves the caller's registers, and survives a simulated save/reload;
- an already-visible minimap does not queue a write or log a repair;
- a refused/throttled teleport, or an event-manager answer of 0, changes neither live nor serialized state;
- unavailable setter bindings do not fall back to a misleading live-only repair.

python -I -B tests/test_teleport_minimap_flag.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

STAND_IN = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>

typedef struct { uint64_t x[32]; } Ctx;
typedef void (*A64Fn)(Ctx *);
static uint8_t *guest;   /* (the guest's first 4 GB, untouched pages cost nothing) */
#define A64_PTR(a) (guest + (uint64_t)(a))
static uint8_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return *A64_PTR(a); }
static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c; uint32_t v; memcpy(&v, A64_PTR(a), 4); return v; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; uint64_t v; memcpy(&v, A64_PTR(a), 8); return v; }
static void recomp_store8(Ctx *c, uint64_t a, uint8_t v) { (void)c; *A64_PTR(a) = v; }
static void recomp_store32(Ctx *c, uint64_t a, uint32_t v) { (void)c; memcpy(A64_PTR(a), &v, 4); }
static void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c; memcpy(A64_PTR(a), &v, 8); }
static void recomp_store64_sp(Ctx *c, uint64_t a, uint64_t v) { recomp_store64(c, a, v); }
static int64_t clock_ns = 1000000000000LL;
static int64_t hos_now_ns(void) { return clock_ns; }
static uint64_t hos_sys_alloc(uint64_t size, uint64_t align) { (void)size, (void)align; return 0x30000000ULL; }
static char said[4096];
static void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const size_t n = strlen(said);
    vsnprintf(said + n, sizeof said - n, fmt, ap);
    va_end(ap);
    strncat(said, "\n", sizeof said - strlen(said) - 1);
}

/* the stand-in game: GameDataMgr at MGR (its flags' table at +32, 8 slots; the bools at +24, 24 bytes each), the event
   manager at EVENTS (+104: the event playing) */
enum { MGR = 0x20000000, TABLE = 0x20001000, BOOLS = 0x20002000, EVENTS = 0x21000000, SLOTS = 8, ENTRIES = 0x22000000,
       NAMES = 0x23000000 };
static const uint32_t flags[3] = {0xcea220d3u, 0xef2b60c2u, 0x9a4dd779u};   /* IsChildhood, IsLoadingBanc, the minimap's */
static int requests;
static void game_get_bool(Ctx *c) {   /* f_8022d9c0(mgr, out, hash) */
    c->x[0] = 0;
    for (uint32_t i = 0; i < 3; i++)
        if (flags[i] == (uint32_t)c->x[2]) *A64_PTR(c->x[1]) = *A64_PTR(BOOLS + i * 24 + 22), c->x[0] = 1;
}
static int set_available = 1, set_calls, set_args_bad, pending, pending_value, saved_hidden;
static void game_set_bool(Ctx *c) {
    set_args_bad |= c->x[0] != MGR || c->x[1] > 1 || c->x[2] != flags[2];
    set_calls++, pending = 1, pending_value = (int)c->x[1];
    c->x[0] = 0xdead, c->x[2] = 0xbeef;   /* guest calls may clobber registers */
}
static void game_tick(void) {
    if (pending) {
        *A64_PTR(BOOLS + 2 * 24 + 22) = (uint8_t)pending_value;
        saved_hidden = pending_value;
        pending = 0;
    }
}
static void game_make_arg(Ctx *c) {   /* an empty EventRequestArg: 8 entries with room for their names, and the event's name */
    const uint64_t arg = c->x[0];
    recomp_store32(c, arg, 8), recomp_store64(c, arg + 8, ENTRIES), recomp_store32(c, arg + 16, 0);
    for (uint32_t i = 0; i < 8; i++)
        recomp_store64(c, ENTRIES + i * 200 + 8, NAMES + i * 64), recomp_store32(c, ENTRIES + i * 200 + 16, 64);
    recomp_store64(c, arg + 6800, NAMES + 0x1000), recomp_store32(c, arg + 6808, 64);
}
static uint64_t answer = 1;   /* the event manager's answer: 0 when it does not take the request */
static void game_request(Ctx *c) { requests++, c->x[0] = answer; }
static void game_nothing(Ctx *c) { (void)c; }
static A64Fn a64_lookup(uint64_t a) {
    return a == 0x8022d9c0ULL ? game_get_bool : a == 0x806a2b20ULL ? (set_available ? game_set_bool : NULL) :
           a == 0x80d99f10ULL ? game_make_arg : a == 0x80d9a820ULL ? game_request :
           a == 0x802dc440ULL || a == 0x8022b6f0ULL || a == 0x80cd0ea0ULL ? game_nothing : NULL;
}
static int oot_gamedata_bool(Ctx *c, uint32_t hash, int *value);
static int oot_gamedata_set_bool(Ctx *c, uint32_t hash, int value);
'''

RUN = r'''
static void game_start(void) {
    Ctx c = {0};
    guest = mmap(NULL, 1ULL << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (guest == MAP_FAILED) exit(3);
    recomp_store64(&c, OOT_GAMEDATA_SLOT, 0x1f000000), recomp_store64(&c, 0x1f000000, MGR);
    recomp_store64(&c, OOT_EVENTMGR_SLOT, 0x1f000100), recomp_store64(&c, 0x1f000100, EVENTS);
    recomp_store64(&c, MGR + 32, TABLE), recomp_store32(&c, MGR + 44, SLOTS);
    recomp_store64(&c, MGR + 24, BOOLS), recomp_store32(&c, MGR + 16, 3);
    for (uint32_t i = 0; i < 3; i++) {   /* (open addressing from hash % size, as oot_gamedata_set_bool walks it) */
        uint32_t slot = flags[i] % SLOTS;
        while (recomp_load32(&c, TABLE + slot * 8)) slot = (slot + 1) % SLOTS;
        recomp_store32(&c, TABLE + slot * 8, flags[i]), recomp_store32(&c, TABLE + slot * 8 + 4, i);
    }
    *A64_PTR(BOOLS + 0 * 24 + 22) = 1;   /* a child */
}
static int hidden(void) { return *A64_PTR(BOOLS + 2 * 24 + 22); }
static void hide(int on) { *A64_PTR(BOOLS + 2 * 24 + 22) = (uint8_t)on; saved_hidden = on; }
static int failed;
static void check(int ok, const char *what) {
    if (!ok) printf("FAIL: %s\n", what), failed = 1;
}

int main(void) {
    Ctx c = {0};
    for (unsigned i = 0; i < 32; i++) c.x[i] = 0x12340000ULL + i;
    c.x[31] = 0x40000000ULL;
    const Ctx original = c;
    game_start();
    const char *line = "the area left had hidden the minimap";

    hide(1);
    check(oot_teleport(&c, 0x50000000ULL, 1) == 1 && requests == 1, "the teleport was not sent");
    check(set_calls == 1 && pending && !set_args_bad, "the repair did not use the engine's Bool setter (mgr, value, hash)");
    check(hidden() && saved_hidden, "the repair wrote live state outside the engine setter");
    game_tick();   /* the game's normal update processes its queued changes before a save */
    check(!hidden(), "a teleport out of an area that hid the minimap left it hidden (MapIconDeactivateByAreaActor still set)");
    check(strstr(said, line) != NULL, "the log does not say the minimap was shown again");
    check(!saved_hidden, "a native save after teleport kept the minimap hidden");
    *A64_PTR(BOOLS + 2 * 24 + 22) = (uint8_t)saved_hidden;   /* a reload of the serialized value */
    check(!hidden(), "the minimap disappeared again after a save and reload");
    check(!memcmp(&c, &original, sizeof c), "the game's registers were not put back");

    clock_ns += 9000000000LL, said[0] = 0;
    check(oot_teleport(&c, 0x50000000ULL, 3) == 1 && requests == 2 && !hidden(), "a teleport with the minimap shown");
    check(set_calls == 1 && !pending && !saved_hidden, "a visible minimap queued a redundant repair");
    check(strstr(said, line) == NULL, "the log speaks of the minimap when nothing hid it");

    clock_ns += 9000000000LL, hide(1);
    recomp_store64(&c, EVENTS + 104, 0x51000000ULL);   /* an event is playing: the teleport waits */
    check(oot_teleport(&c, 0x50000000ULL, 1) == 0 && requests == 2, "a teleport was sent while an event played");
    check(hidden(), "a teleport that was refused showed the minimap of the area Link is still in");
    check(set_calls == 1 && !pending && saved_hidden, "a refused teleport changed the saved state");
    recomp_store64(&c, EVENTS + 104, 0);

    check(oot_teleport(&c, 0x50000000ULL, 1) == 1 && requests == 3, "the teleport after the event");
    game_tick();
    check(set_calls == 2 && !hidden() && !saved_hidden, "the teleport after the event did not repair both values");
    hide(1);   /* (the area he arrived in hides its own) */
    clock_ns += 1000000000LL;
    check(oot_teleport(&c, 0x50000000ULL, 3) == 0 && requests == 3, "a second teleport was sent a second after the first");
    check(hidden(), "a teleport that was ignored showed the minimap");
    check(set_calls == 2 && !pending && saved_hidden, "a throttled teleport changed the saved state");

    clock_ns += 9000000000LL, answer = 0;   /* sent, but the event manager does not take it: the map does not change */
    check(oot_teleport(&c, 0x50000000ULL, 1) == 1 && requests == 4, "the teleport the game turned down was not sent");
    check(hidden(), "a teleport the game turned down showed the minimap of the area Link is still in");
    check(set_calls == 2 && !pending && saved_hidden, "a declined request changed the saved state");

    clock_ns += 9000000000LL, answer = 1, set_available = 0, said[0] = 0;
    check(oot_teleport(&c, 0x50000000ULL, 1) == 1 && requests == 5, "the teleport with no setter binding was not sent");
    check(hidden() && saved_hidden && set_calls == 2 && !pending, "a missing setter fell back to a live-only write");
    check(strstr(said, "restore queued") == NULL, "a missing setter logged a successful repair");
    check(strstr(said, "the game's Bool setter not found, left hidden") != NULL, "a missing setter was not named in the log");
    return failed;
}
'''


def block_end(text, start):
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return end


def build(cc, tmp, source_path=None):
    source = (source_path or ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    places = source.index("typedef struct {\n    const char *child_banc")   # (OotDestination and the table of them)
    places = source[places:source.index("};", source.index("static const OotDestination oot_destinations[] = {")) + 2]
    start = source.index("#define OOT_GAMEDATA_SLOT")
    end = block_end(source, source.index("static int oot_gamedata_set_bool(Ctx *c, uint32_t hash, int value) {"))
    src, exe = tmp / "teleport.c", tmp / "teleport"
    src.write_text(STAND_IN + places + "\n" + source[start:end] + RUN, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-O1", "-w", str(src), "-o", str(exe)], check=True)
    return exe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--source", type=Path, help="host/hos_main.c from another revision (the regression negative)")
    a = p.parse_args()
    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the stand-in game maps the guest's addresses as Linux does")
        return
    with tempfile.TemporaryDirectory(prefix="teleport-minimap-") as tmp:
        r = subprocess.run([str(build(a.cc, Path(tmp), a.source))], capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise SystemExit("test_teleport_minimap_flag: %s" % (r.stdout.strip() or "the stand-in stopped with %d" % r.returncode))
    print("PASS: accepted teleport repairs live and saved minimap state; refusals and missing bindings leave it alone")


if __name__ == "__main__":
    main()
