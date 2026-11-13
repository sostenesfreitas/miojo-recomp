// program_life (gpu/vk/program_life.h): the states of what the game's programs need loaded. A unit is queued once when the game
// first makes a program of its stages, a first use waits only for a load that is under way, a unit is let go only by count, only
// when the game has freed every program of it and it was drawn, the longest freed first, and is loaded again by the next create.
// No game, no GPU.
#include "../gpu/vk/program_life.h"

#include <cstdio>

namespace {

using program_life::State;
using program_life::Table;
using program_life::Use;

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

void create_and_use() {
    Table t;
    expect(t.use(7) == Use::Miss, "a unit no create call named is a miss: the first-use path as before");
    expect(t.create(1, 7, true), "the first program of prepared stages queues a load");
    expect(t.find(7)->state == State::Created, "created until the job is queued");
    expect(t.use(7) == Use::Miss, "not queued yet: a draw does not wait for a job that does not exist");
    t.loading(7);
    expect(!t.create(2, 7, true), "a second program of the same stages does not queue a second load");
    expect(t.use(7) == Use::Wait, "a draw while the job runs waits for it");
    t.loaded(7, true);
    expect(t.find(7)->state == State::Ready && !t.find(7)->drawn, "loaded: ready, not drawn yet");
    expect(!t.create(3, 7, true), "a program of stages already loaded queues nothing");
    expect(t.use(7) == Use::Ready && t.find(7)->drawn, "a draw of a ready unit goes ahead, and it counts as drawn");
    expect(t.find(7)->live == 3 && t.programs() == 3 && t.units() == 1, "three programs, one unit");
    expect(t.unit_of(2) == 7 && t.unit_of(9) == 0, "a serial's unit; none for a serial never created");
}

void nothing_prepared() {
    Table t;
    expect(!t.create(1, 5, false), "stages the prepare kept nothing for queue no load: never a translation at create");
    t.loading(5);
    expect(t.find(5)->state == State::Loading, "(loading is the caller's to call only after a true create)");
    Table u;
    expect(!u.create(1, 5, false) && u.use(5) == Use::Miss, "and their first draw is a miss: translated there, as before");
    expect(u.create(2, 5, true), "once something is prepared for them (learned), the next create queues it");
}

void failed_job() {
    Table t;
    expect(t.create(1, 5, true), "queued");
    t.loading(5);
    t.loaded(5, false);
    expect(t.find(5)->state == State::Created && t.use(5) == Use::Miss, "a job that loaded nothing leaves the first-use path");
    expect(!t.create(2, 5, false), "and is not queued again for the same stages");
    expect(t.find(5)->loads == 0, "nothing was loaded");
}

void set_shaders_again() {
    Table t;
    t.create(1, 5, true);
    t.loading(5), t.loaded(5, true);
    expect(t.create(1, 6, true), "the same program given other shaders, with no free call: the new stages queue");
    expect(t.find(5)->live == 0 && t.find(6)->live == 1 && t.unit_of(1) == 6, "the old stages' unit lost that program");
    t.free(1);
    t.free(1);
    expect(t.find(6)->live == 0 && t.programs() == 0, "a free call counts once");
}

void release_by_count() {
    Table t;
    for (uint64_t u = 1; u <= 6; u++) {
        t.create(u, 100 + u, true);
        t.loading(100 + u), t.loaded(100 + u, true);
    }
    for (uint64_t u = 1; u <= 5; u++) t.use(100 + u);   // (106 is loaded and never drawn)
    expect(t.release_candidates(0).empty(), "nothing is let go while the game still has the programs");
    t.free(3), t.free(1), t.free(6), t.free(2);   // (freed in this order; 104 and 105 are still alive)
    expect(t.release_candidates(3).empty(), "three freed and drawn units, three kept: none let go");
    const auto one = t.release_candidates(2);
    expect(one.size() == 1 && one[0] == 103, "above the count only the excess goes, the longest freed first");
    const auto all = t.release_candidates(0);
    expect(all.size() == 3 && all[0] == 103 && all[1] == 101 && all[2] == 102, "oldest first, in the order they were freed");
    for (const uint64_t u : all) expect(u != 106, "a unit loaded and never drawn is not let go early");
    for (const uint64_t u : all) expect(u != 104 && u != 105, "a unit the game still has a program of is never let go");
    t.released(103);
    expect(t.find(103)->state == State::Released && t.use(103) == Use::Miss, "let go: a use finds nothing loaded");
    t.released(104);
    expect(t.find(104)->state == State::Ready, "released() refuses a unit that is still alive");
    expect(t.release_candidates(0).size() == 2, "a released unit is not a candidate again");
    // the game makes a program of the released stages again (back and forth between two areas)
    expect(t.create(7, 103, true), "a create of released stages queues the load again, from prepared data");
    t.loading(103), t.loaded(103, true);
    expect(t.find(103)->loads == 2 && t.find(103)->state == State::Ready && !t.find(103)->drawn, "loaded a second time: a reload");
    expect(t.release_candidates(0).size() == 2, "alive again: not a candidate");
    // a unit freed and made again before any release keeps what it has
    t.free(4);
    expect(!t.create(8, 104, true) && t.find(104)->state == State::Ready, "freed and made again without a release: nothing is reloaded");
    expect(t.count(State::Ready) == 6 && t.count(State::Released) == 0, "the counts by state");
}

}   // namespace

int main() {
    create_and_use();
    nothing_prepared();
    failed_job();
    set_shaders_again();
    release_by_count();
    if (failures) return std::printf("%d checks failed\n", failures), 1;
    std::printf("program_life: ok\n");
    return 0;
}
