// SPDX-License-Identifier: GPL-3.0-or-later
// gpu/vk/load_default.h alone (tests/test_load_default.py)
#include "../gpu/vk/load_default.h"
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

int main(int argc, char** argv) {
    CHECK(argc == 2);
    using load_default::choose;
    // the switch always wins, whatever the GPU and the marker
    for (int mesh = 0; mesh < 2; mesh++)
        for (int marker = 0; marker < 2; marker++) {
            CHECK(choose("create", mesh, marker).create);
            CHECK(!choose("start", mesh, marker).create);
        }
    // the compressed program cache: start, whatever the switch, the GPU and the marker say
    for (int mesh = 0; mesh < 2; mesh++)
        for (int marker = 0; marker < 2; marker++) {
            CHECK(!choose("create", mesh, marker, true).create && !choose(nullptr, mesh, marker, true).create);
            CHECK(std::strstr(choose("create", mesh, marker, true).why, "RX_VK_PROGRAM_CACHE_ZSTD"));
            CHECK(choose("create", mesh, marker, false).create);
        }
    // the default: create only with mesh shaders and no marker; anything else the switch says is not a choice
    CHECK(choose(nullptr, true, false).create);
    CHECK(!choose(nullptr, false, false).create);
    CHECK(!choose(nullptr, true, true).create);
    CHECK(!choose(nullptr, false, true).create);
    CHECK(choose("", true, false).create && choose("1", true, false).create && !choose("yes", false, false).create);
    CHECK(std::strstr(choose(nullptr, true, true).why, "hookshot_load_start.txt"));
    // the safety net: the measured good sessions pass, the measured bad ones do not; at the limits it has not fired
    using load_default::Waits;
    using load_default::waits_verdict;
    // only long waits count: a healthy session's (2-7 ms each) never add up, however many there are
    CHECK(!load_default::counts(2.0) && !load_default::counts(6.7) && !load_default::counts(20.0) && load_default::counts(20.1));
    CHECK(waits_verdict(0, false) == Waits::kFine && waits_verdict(0, true) == Waits::kFine);
    // one bad moment on a healthy PC (one wait of 261 ms, with other work taking the memory): nothing, with or without a strike
    CHECK(waits_verdict(261, false) == Waits::kFine && waits_verdict(261, true) == Waits::kFine);
    CHECK(waits_verdict(500, false) == Waits::kFine);
    // over 500 ms: a strike the first time, the marker when the session before was over it too
    CHECK(waits_verdict(500.1, false) == Waits::kStrike && waits_verdict(1999, false) == Waits::kStrike);
    CHECK(waits_verdict(500.1, true) == Waits::kStart && waits_verdict(2000, true) == Waits::kStart);
    // the PC where loads at create do not work (RX 580: tours of 2.5-4.7 s, and one session of 62.6 ms followed by one of
    // 2,559 ms): the marker in its first session over 2 s, nothing for the 62.6 ms one
    CHECK(waits_verdict(62.6, false) == Waits::kFine);
    CHECK(waits_verdict(2557, false) == Waits::kStart && waits_verdict(3089, false) == Waits::kStart &&
          waits_verdict(4700, false) == Waits::kStart && waits_verdict(2000.1, false) == Waits::kStart);
    CHECK(waits_verdict(2000, false) == Waits::kStrike);
    // the strike: not there, written, taken once
    {
        const std::string m = std::string(argv[1]) + "/hookshot_load_start.txt";
        CHECK(!load_default::strike_take(m));
        CHECK(load_default::strike_write(m, 640.2) && load_default::marker_there(load_default::strike_path(m)));
        CHECK(!load_default::marker_there(m));   // (a strike is not the marker)
        CHECK(load_default::strike_take(m) && !load_default::strike_take(m));
        CHECK(!load_default::strike_write(std::string(argv[1]) + "/no-such-folder/x.txt", 1));
    }
    // the second part: listed pipelines the loads found in no cache. The clean tours had none (39 at most without mesh
    // shaders); the two sessions of the folder whose caches did not answer had 4,189 and 5,384
    using load_default::caches_lacked;
    CHECK(!caches_lacked(0) && !caches_lacked(39) && !caches_lacked(500));
    CHECK(caches_lacked(501) && caches_lacked(3720 + 469) && caches_lacked(4718 + 666));
    // a healthy session's draws left out for pipelines no list names (1,906, and 353 ms of waits, in eight minutes of a new
    // game's opening; the same in start mode) are no input of the rule at all: with 0 listed pipelines missing it does not trip,
    // however long the session
    CHECK(!caches_lacked(0));
    // the marker: not there, written, there; its text says how to take it away
    const std::string path = std::string(argv[1]) + "/hookshot_load_start.txt";
    CHECK(!load_default::marker_there(path));
    CHECK(load_default::marker_write(path, 884.4, 2556.6) && load_default::marker_there(path));
    char text[400]{};
    FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f && std::fread(text, 1, sizeof text - 1, f) > 0);
    std::fclose(f);
    CHECK(std::strstr(text, "2557 ms") && std::strstr(text, "884 ms") && std::strstr(text, "Delete this file"));
    CHECK(!load_default::marker_write(std::string(argv[1]) + "/no-such-folder/x.txt", 1, 1));
    CHECK(std::remove(path.c_str()) == 0 && !load_default::marker_there(path));
    CHECK(load_default::marker_write_lacked(path, 4189) && load_default::marker_there(path));
    CHECK(std::remove(path.c_str()) == 0);
    std::printf("load_default: ok\n");
    return 0;
}
