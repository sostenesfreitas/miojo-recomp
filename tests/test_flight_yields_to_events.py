"""Game-free: the debug tools' flight lets Link go while an event plays.

An event that takes Link (a map change, the age toggle's reload, a cutscene) waits for him to stand. The flight moved him
to its own position every tick, so with it on he never stood: flying into an area's exit or toggling his age left the
screen black until the flight was switched off. host/hos_main.c's oot_update_player_cheats now reads the event manager
before it flies him, and starts the flight again from where he stands once the event has ended. And a Link the game
moved while he flew (a respawn after a fall keeps his actor) was flown straight back to where the flight had him, over
the same drop, again and again: the flight starts from where the game put him instead.

python -I -B tests/test_flight_yields_to_events.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def main():
    source = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    playing = block(source, "static int oot_event_playing(Ctx *c) {")
    need("oot_singleton(c, OOT_EVENTMGR_SLOT)" in playing and "events + 104" in playing,
         "oot_event_playing does not read the event manager's playing event")
    cheats = block(source, "static void oot_update_player_cheats(Ctx *c, uint64_t actor, uint64_t components) {")
    read = cheats.index("int flying = rx_option(HOS_OPT_CHEAT_FLIGHT, 0) > 0;")
    yields = cheats.index("if (flying && oot_event_playing(c)) flying = 0;")
    forgets = cheats.index("if (!flying) flying_actor = 0;")
    moves = cheats.index("change_position(c);")
    need(read < yields < forgets < moves, "the flight moves Link before it has asked whether an event plays")
    # (flying_actor cleared: the flight's own position is taken from Link again when it resumes, not the one from before)
    need("flying_actor != actor" in cheats[forgets:moves], "a flight that resumes does not start from where Link stands")
    # and a Link the game moved (a respawn, a warp: the same actor) is not taken back to where the flight had him
    moved = block(source, "static int oot_flight_moved_away(Ctx *c, uint64_t actor, const float held[3]) {")
    need("oot_load_float(c, actor + 796 + i * 4) - held[i]" in moved and "return apart > 2.0f * 2.0f;" in moved,
         "oot_flight_moved_away does not compare Link's position with the flight's")
    seed = cheats.index("oot_flight_moved_away(c, actor, target)) {")
    again = cheats.index("target[i] = oot_load_float(c, actor + 796 + i * 4);", seed)
    need(seed < again < cheats.index("return;", again) < moves, "a Link the game moved is flown back before the flight starts again from him")
    # the teleport still refuses during an event, with the line a player's log shows
    need("cheats: teleport waits until the current event ends" in source, "the teleport's refusal during an event is gone")
    print("PASS: the flight yields to a playing event and resumes from Link's position")


if __name__ == "__main__":
    main()
