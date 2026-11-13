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
    using load_default::too_slow;
    CHECK(!too_slow(6.7, 24.4) && !too_slow(2.0, 10.8) && !too_slow(100.0, 500.0));
    CHECK(too_slow(713, 3089) && too_slow(884, 2557) && too_slow(100.1, 101) && too_slow(40, 500.1));
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
