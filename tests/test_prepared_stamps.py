"""The prepare keeps two stamps, and the driver's part is made from the program cache.

hookshot_prepared.bin said what the shaders were translated for (the translator and the seed), and the stage libraries were built
only as a program was translated. A program kept already was never translated again, so its libraries were never built again
either: a player who prepared without libraries and then switched them on had "0 new, 0 stage libraries", and after a driver
update, which empties the driver's cache, nothing made them again. The record now has a sixth word for the driver's part, and
that part (warm_libraries) walks the kept programs. This builds the record's reader, writer and check as they are in the source
and checks that a record of before the sixth word reads as warmed for nothing, that the word survives the record being written
again for a seed's new time, and that it does not survive another translator; and that the pieces are where they have to be: no
library built in the translation's loop, the walk on both of prepare_run's paths, the list of keys made without reading a
record, and RX_VK_GPL out of the translator's stamp.
python -I -B tests/test_prepared_stamps.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

def _with_cache(path):
    """vk_shader.cpp with the program cache it held before (vk_progcache.cpp and .h beside it, outside the translator stamp): definitions first"""
    parts = [path.read_text(encoding="utf-8")]
    for name in ("vk_progcache.cpp", "vk_progcache.h"):
        if (path.parent / name).exists():
            parts.append((path.parent / name).read_text(encoding="utf-8"))
    return "\n".join(parts)

DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"
SHADER = ROOT / "gpu" / "vk" / "vk_shader.cpp"


def block(text, marker):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default="g++")
    a = ap.parse_args()
    draw = DRAW.read_text(encoding="utf-8")
    shader = _with_cache(SHADER)
    start = draw.index("struct PreparedFor {")
    helpers = draw[start:draw.index("} // namespace", start)]
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
int main(int argc, char** argv) {
    const std::string dir = argv[1], path = dir + "/hookshot_prepared.bin", seed = dir + "/hookshot_shaders.bin";
    int fails = 0;
    const auto put = [&](const std::string& to, const void* d, size_t n) {
        FILE* f = fopen(to.c_str(), "wb");
        fwrite(d, 1, n, f);
        fclose(f);
    };
    const auto size_of = [&] { return (long)std::filesystem::file_size(path); };
    PreparedFor p;
    if (prepared_read(path, &p) != 0 || p.warmed) std::puts("no file: wanted no record"), fails++;
    const uint64_t five[5] = {0x1122334455667788ull, 2, 3, 4, 5};   // (a record of before the sixth word)
    put(path, five, 40);
    if (prepared_read(path, &p) != 2 || p.stamp != five[0] || !p.timed || p.warmed)
        std::puts("a record of five words: wanted it whole, and warmed for nothing"), fails++;
    put(path, five, 8);   // (and one of before the seed was in it)
    if (prepared_read(path, &p) != 1 || p.stamp != five[0] || p.warmed) std::puts("the stamp alone: wanted warmed for nothing"), fails++;
    uint8_t cut[44];
    std::memcpy(cut, five, 40);
    std::memset(cut + 40, 9, 4);
    put(path, cut, 44);
    if (prepared_read(path, &p) != 2 || p.warmed) std::puts("a sixth word cut short: wanted warmed for nothing"), fails++;
    PreparedFor w;
    w.stamp = 0xAAAA, w.seed_hash = 7, w.seed_size = 8, w.seed_time = 9, w.warmed = 0xBBBB;
    prepared_write(path, w);
    uint64_t first = 0;
    FILE* f = fopen(path.c_str(), "rb");
    if (fread(&first, 8, 1, f) != 1 || first != 0xAAAA || size_of() != 48)   // (prepare_is_update reads the first 8 bytes alone)
        std::puts("the file: wanted 48 bytes, the translator's stamp first"), fails++;
    fclose(f);
    if (prepared_read(path, &p) != 2 || p.stamp != 0xAAAA || p.seed_hash != 7 || p.seed_size != 8 || p.seed_time != 9 || p.warmed != 0xBBBB)
        std::puts("written and read: wanted every word back"), fails++;

    // the check hands the record's warmed stamp on, and keeps it where it writes the record again itself
    const std::string bytes(70000, 's');
    put(seed, bytes.data(), bytes.size());
    const auto hour_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(seed, hour_ago);
    PreparedFor now;
    double ms = 0;
    std::filesystem::remove(path);
    if (prepare_check(path, seed, 5, &now, &ms) != PrepareWhy::First || now.warmed) std::puts("first start: wanted nothing warmed"), fails++;
    now.warmed = 0xCCCC;
    prepared_write(path, now);
    PreparedFor next;
    if (prepare_check(path, seed, 5, &next, &ms) != PrepareWhy::None || next.warmed != 0xCCCC)
        std::puts("the next start: wanted the warmed stamp handed on"), fails++;
    // (the seed written again as it was, by a build: hashed, and the record written again by the check for its new time)
    put(seed, bytes.data(), bytes.size());
    std::filesystem::last_write_time(seed, hour_ago + std::chrono::seconds(30));
    if (prepare_check(path, seed, 5, &next, &ms) != PrepareWhy::None || ms == 0) std::puts("rewritten as it was: wanted it hashed, no prepare"), fails++;
    if (prepared_read(path, &p) != 2 || p.warmed != 0xCCCC || p.seed_time != next.seed_time)
        std::puts("rewritten as it was: the record written again lost the warmed stamp, or was not written"), fails++;
    // (another translator: its programs are not these, so nothing of them is warmed)
    if (prepare_check(path, seed, 6, &next, &ms) != PrepareWhy::Translator || next.warmed)
        std::puts("another translator: wanted nothing warmed"), fails++;
    // (a grown seed, the same translator: what was warmed is still warmed for what is kept; prepare_run decides the rest)
    const std::string grown = bytes + "more";
    put(seed, grown.data(), grown.size());
    std::filesystem::last_write_time(seed, hour_ago + std::chrono::seconds(60));
    if (prepare_check(path, seed, 5, &next, &ms) != PrepareWhy::Seed || next.warmed != 0xCCCC)
        std::puts("a grown seed: wanted a prepare, and the warmed stamp handed on"), fails++;
    if (fails) return 1;
    std::puts("ok");
}
"""
    with tempfile.TemporaryDirectory(prefix="prepared-stamps-") as tmp:
        c, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture"
        c.write_text(cpp, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", str(c), "-o", str(exe)], check=True)
        work = Path(tmp) / "game"
        work.mkdir()
        got = subprocess.run([str(exe), str(work)], capture_output=True, text=True)
        if got.returncode or got.stdout.strip() != "ok":
            raise SystemExit("the prepared file:\n" + got.stdout + got.stderr)
    prepare = block(draw, "bool prepare_run(")
    if "build_graphics(" in prepare:
        raise SystemExit("prepare_run builds stage libraries itself: only for the programs it translates, none for one kept already")
    if prepare.count("warm_libraries(progress)") != 2:
        raise SystemExit("prepare_run: wanted warm_libraries after the translation and on the path with no translation")
    alone = prepare[:prepare.index("seed_open();\n    if (!programs)")]
    if "warm_libraries(progress)" not in alone or "if (prepared_for.warmed == warmed || background) return false;" not in alone:
        raise SystemExit("prepare_run does not make the driver's part alone when only its stamp differs")
    at = 0
    for _ in range(2):   # (closed in the libraries: not written down as warmed)
        at = prepare.index("if (!warm_libraries(progress)) {", at) + 1
        if prepare.index("return false;", at) > prepare.index("prepared_for.warmed = warmed;\n", at):
            raise SystemExit("prepare_run writes the driver's part down as made when its pass was stopped")
    if prepare.count("prepared_for.warmed = warmed;\n") != 2 or prepare.count("prepared_for.warmed = 0;\n") != 1:
        raise SystemExit("prepare_run: wanted the warmed stamp set on its two ways out, and cleared before the libraries are made")
    cleared = prepare.index("prepared_for.warmed = 0;\n")
    if prepare.index("prepared_write(path, prepared_for);", cleared) > prepare.index("if (!warm_libraries(progress)) {", cleared):
        raise SystemExit("prepare_run: the translation is not written down before the libraries are made")
    stamp = block(draw, "uint64_t warmed_stamp(")
    for part in ("program_cache_stamp()", "pipelineCacheUUID", "driverVersion", "libraries", "kWarmRecipe"):
        if part not in stamp:
            raise SystemExit("warmed_stamp leaves out " + part)
    warm = block(draw, "bool warm_libraries(")
    for call in ("program_cache_keys(", "program_cache_borrow(", "pipe_notes_read()", "ub_thread_off();", "prepare_stopped()"):
        if call not in warm:
            raise SystemExit("warm_libraries: no " + call)
    if "program_cache_each(" in warm:
        raise SystemExit("warm_libraries lists the programs with program_cache_each, which reads every record")
    keys = block(shader, "std::vector<uint64_t> program_cache_keys(")
    if "fread(" in keys or "disk_records" not in keys or "shader_mu" not in keys:
        raise SystemExit("program_cache_keys: wanted the keys from the list of records and the variants in memory, no record read")
    env = block(shader, "uint64_t translator_stamp(")
    if '"RX_VK_GP"' in env or '"RX_VK_GP_"' not in env:
        raise SystemExit("the translator's stamp takes RX_VK_GPL: switching the libraries on throws the program cache away")
    print("PASS: two stamps in the prepared file; the driver's part is made from the program cache, on its own when it alone is stale")


if __name__ == "__main__":
    main()
