"""The shaders are prepared again when the seed changes, not only when the translator does.

prepare_run (gpu/vk/vk_draw.cpp) prepared once per translator: hookshot_prepared.bin held program_cache_stamp() and nothing
else. The seed (hookshot_shaders.bin, nx2/shader_census.py) grows apart from the translator (4221050d added 19 compute
programs), and a folder prepared before such an update never prepared the new programs: each was translated in play when
first met. The record now also holds the seed's hash, size and write time; prepare_check hashes the seed only when its size
or time is not the recorded one. This builds the record's helpers as they are in the source and checks, on files in a
temporary folder: the same seed is not prepared again (and not hashed again); a changed seed is, and so is a changed
translator; a seed rewritten as it was (a new time) is hashed once and not prepared; a record from before (the stamp alone)
prepares once; a seed whose time is as new as the record's is hashed (its time cannot show a second write in the same tick:
whole seconds from libstdc++'s std::filesystem on Windows); and that prepare_run uses all of this. Every write time is set
by the test, so that it does not depend on how fast it runs or the file system's resolution.
python -I -B tests/test_prepare_seed_stamp.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default="g++")
    a = ap.parse_args()
    src = DRAW.read_text(encoding="utf-8")
    start = src.index("struct PreparedFor {")
    helpers = src[start:src.index("} // namespace", start)]
    cpp = r"""
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
""" + helpers + r"""
int fails = 0;
std::string dir, rec, seed;
void put(const std::string& path, const std::string& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}
// Every write time is set, not left to the clock: a write's own time can be the last one's (whole seconds from libstdc++ on
// Windows, a timer tick at best), which would make the test depend on how fast it runs. An hour back, ten seconds apart: well
// before each record is written, where the shortcut is taken
const auto base = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
int tick = 0;
void stamp_time(std::filesystem::file_time_type t) {
    std::error_code ec;
    std::filesystem::last_write_time(seed, t, ec);
    if (ec) std::printf("could not set the write time: %s\n", ec.message().c_str()), fails++;
}
void later() { stamp_time(base + std::chrono::seconds(10 * ++tick)); }
/// One start: the check, and the record written when it prepares (as prepare_run does once done)
PrepareWhy start(uint64_t stamp, double* hash_ms = nullptr) {
    PreparedFor now;
    double ms = 0;
    const PrepareWhy why = prepare_check(rec, seed, stamp, &now, &ms);
    if (why != PrepareWhy::None) prepared_write(rec, now);
    if (hash_ms) *hash_ms = ms;
    return why;
}
void want(const char* what, PrepareWhy got, PrepareWhy wanted) {
    if (got != wanted) std::printf("%s: got %d, wanted %d\n", what, (int)got, (int)wanted), fails++;
}
void old_record(uint64_t stamp) {   // (from before the seed was in it: the translator's stamp alone, 8 bytes)
    FILE* f = fopen(rec.c_str(), "wb");
    fwrite(&stamp, 8, 1, f);
    fclose(f);
}
int main(int argc, char** argv) {
    dir = argv[1], rec = dir + "/hookshot_prepared.bin", seed = dir + "/hookshot_shaders.bin";
    std::string bytes(3 * (1 << 20) + 77, '\0');   // (more than one read, and bytes over the last 32)
    for (size_t i = 0; i < bytes.size(); i++) bytes[i] = (char)(i * 2654435761u >> 13);
    put(seed, bytes);
    later();
#ifdef _WIN32
    // FILETIME's native ticks fit; nanoseconds since 1601 do not. Check the
    // production helpers against the Win32 clocks without signed multiplication.
    FILETIME before, after;
    GetSystemTimeAsFileTime(&before);
    const int64_t clock = file_clock_now();
    GetSystemTimeAsFileTime(&after);
    const auto ticks = [](FILETIME t) {
        ULARGE_INTEGER n; n.LowPart = t.dwLowDateTime; n.HighPart = t.dwHighDateTime;
        return n.QuadPart;
    };
    if (clock < 0 || (uint64_t)clock < ticks(before) || (uint64_t)clock > ticks(after))
        std::puts("current clock is not in FILETIME ticks"), fails++;
    HANDLE file = CreateFileA(seed.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME written;
    uint64_t size;
    int64_t time;
    if (file == INVALID_HANDLE_VALUE || !GetFileTime(file, nullptr, nullptr, &written) ||
        !seed_file_id(seed, &size, &time) || time < 0 || (uint64_t)time != ticks(written))
        std::puts("seed time is not in FILETIME ticks"), fails++;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#endif
    double ms = 0;
    want("first start", start(1), PrepareWhy::First);
    want("same seed, same translator", start(1, &ms), PrepareWhy::None);
    if (ms != 0) std::puts("same seed: hashed again"), fails++;
    want("changed translator", start(2), PrepareWhy::Translator);
    want("then the same again", start(2), PrepareWhy::None);
    // a seed rewritten with the same bytes (a build): hashed once, not prepared, and its new time kept
    later();
    want("rewritten as it was", start(2, &ms), PrepareWhy::None);
    if (ms == 0) std::puts("rewritten as it was: not hashed"), fails++;
    want("rewritten, the next start", start(2, &ms), PrepareWhy::None);
    if (ms != 0) std::puts("rewritten, the next start: hashed again"), fails++;
    // a grown seed (an update's new programs)
    put(seed, bytes + std::string(4096, 'x'));
    later();
    want("grown seed", start(2), PrepareWhy::Seed);
    want("grown seed, the next start", start(2), PrepareWhy::None);
    // one byte changed in the middle, the size the same
    std::string changed = bytes + std::string(4096, 'x');
    changed[changed.size() / 2] ^= 1;
    put(seed, changed);
    later();
    want("one byte changed", start(2), PrepareWhy::Seed);
    // one byte in the last, partial block
    changed[changed.size() - 3] ^= 1;
    put(seed, changed);
    later();
    want("a tail byte changed", start(2), PrepareWhy::Seed);
    // a record of four words (the seed's hash, size and time, not when it was written): hashed once, then the shortcut
    {
        PreparedFor four;
        double unused;
        prepare_check(rec, seed, 2, &four, &unused);
        const uint64_t r[4] = {four.stamp, four.seed_hash, four.seed_size, (uint64_t)four.seed_time};
        FILE* f = fopen(rec.c_str(), "wb");
        fwrite(r, 8, 4, f);
        fclose(f);
    }
    want("four-word record", start(2, &ms), PrepareWhy::None);
    if (ms == 0) std::puts("four-word record: the shortcut was taken"), fails++;
    want("four-word record, the next start", start(2, &ms), PrepareWhy::None);
    if (ms != 0) std::puts("four-word record, the next start: hashed again"), fails++;
    // a record from before the seed was in it
    old_record(2);
    want("old record, same translator", start(2), PrepareWhy::Seed);
    want("old record, then", start(2), PrepareWhy::None);
    old_record(2);
    want("old record, changed translator", start(3), PrepareWhy::Translator);
    // a seed written as the record was (the same second, tick): its time cannot tell a write that follows, of the same size,
    // in that same second. Both writes get the very same time here: the second is still seen, as the first was hashed
    const auto now = std::filesystem::file_time_type::clock::now();
    put(seed, bytes);
    stamp_time(now);
    want("written just now", start(3), PrepareWhy::Seed);
    want("written just now, the next start", start(3, &ms), PrepareWhy::None);
    if (ms == 0) std::puts("written just now: the shortcut was taken"), fails++;
    std::string close = bytes;
    close[12345] ^= 1;
    put(seed, close);
    stamp_time(now);
    want("rewritten in the same tick", start(3), PrepareWhy::Seed);
    // once its time is well before a record, the shortcut again
    later();
    want("written well before", start(3, &ms), PrepareWhy::None);
    if (ms == 0) std::puts("written well before: not hashed"), fails++;
    want("written well before, the next start", start(3, &ms), PrepareWhy::None);
    if (ms != 0) std::puts("written well before, the next start: hashed again"), fails++;
    // The guard is still two seconds, including when Windows changes time units.
    stamp_time(std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5));
    want("five seconds old", start(3, &ms), PrepareWhy::None);
    want("five seconds old, the next start", start(3, &ms), PrepareWhy::None);
    if (ms != 0) std::puts("five seconds old: hashed again"), fails++;
#ifdef _WIN32
    // Old five-word records stored wrapped nanoseconds. They must hash once and
    // rewrite the times, without preparing unchanged content again. Unsigned
    // multiplication constructs those legacy bits without repeating their UB.
    {
        PreparedFor had;
        prepare_check(rec, seed, 3, &had, &ms);
        const uint64_t old[5] = {had.stamp, had.seed_hash, had.seed_size,
                                (uint64_t)had.seed_time * 100, (uint64_t)file_clock_now() * 100};
        FILE* f = fopen(rec.c_str(), "wb");
        fwrite(old, 8, 5, f);
        fclose(f);
    }
    want("legacy Windows record", start(3, &ms), PrepareWhy::None);
    if (ms == 0) std::puts("legacy Windows record: not rehashed"), fails++;
    want("legacy Windows record, the next start", start(3, &ms), PrepareWhy::None);
    if (ms != 0) std::puts("legacy Windows record: hashed twice"), fails++;
#endif
    // no seed: the translator alone, as before
    std::filesystem::remove(seed);
    want("no seed, same translator", start(3), PrepareWhy::None);
    want("no seed, changed translator", start(4), PrepareWhy::Translator);
    if (fails) return 1;
    std::puts("ok");
}
"""
    with tempfile.TemporaryDirectory(prefix="seed-stamp-") as tmp:
        c, exe = Path(tmp) / "fixture.cpp", Path(tmp) / ("fixture.exe" if os.name == "nt" else "fixture")
        c.write_text(cpp, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-O1", str(c), "-o", str(exe)], check=True)
        work = Path(tmp) / "game"
        work.mkdir()
        got = subprocess.run([str(exe), str(work)], capture_output=True, text=True)
        if got.returncode or got.stdout.strip() != "ok":
            raise SystemExit("prepare_check:\n" + got.stdout + got.stderr)
    run = block(src, "bool prepare_run(")
    for need, why in (("prepare_check(path, seed_path(), program_cache_stamp()", "asks prepare_check"),
                      ("prepared_write(path, prepared_for)", "records what it prepared for"),
                      ("why == PrepareWhy::Seed", "says when the seed made it prepare again")):
        if need not in run:
            raise SystemExit("prepare_run no longer " + why)
    if "fwrite(&stamp, 8, 1" in run:
        raise SystemExit("prepare_run writes the translator's stamp alone")
    print("PASS: the shaders are prepared again for a new seed or translator, and not for the same ones")


if __name__ == "__main__":
    main()
