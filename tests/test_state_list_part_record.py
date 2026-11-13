"""The pipeline lists the game adds to as it plays (gpu/vk/vk_draw.cpp): the state list, hookshot_pipeline_states.bin
(state_open_locked), and the pipeline notes (notes_open_locked). A crash or a full disk while one is written can leave it
ending in part of a record. The next run appended its new records after that part, so none of them was read back: their
pipelines were built again in play at each start, and "Save my pipeline list" sent a list whose end was garbage.

The open is cut from the source and run on lists of its own: a whole record followed by 1 byte, and by a record's size less
one, of the next. Then a new record is added as play adds one, and the list is read back: both records there, the file a
whole number of records. The notes the same, opened with their records already read (before play) and without. A list
that ends on a record's edge is left as it is. No game or GPU required.

python -I -B tests/test_state_list_part_record.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#define _CRT_SECURE_NO_WARNINGS 1
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#define _fseeki64 fseeko
#endif
static int fails, logged;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static void logf(const char*, ...) { logged++; }
static std::string dir;
std::string state_list_path(const char* name) { return dir + name; }
std::vector<uint64_t> computes_read() { return {}; }
struct PipeNote { uint64_t key; uint8_t rest[200]; };   // (a stand-in of the same kind: a key first, a fixed size)
static uint64_t program_cache_stamp() { return 0x1234; }
static std::string pipe_notes_path() { return dir + "notes.bin"; }
std::unordered_set<uint64_t> noted;
FILE* note_file = nullptr;
/* RECORDS */
static uint64_t state_hash(const StateRec2& r) { return r.code[0]; }
std::mutex* unused_mu;
FILE* state_file = nullptr;
std::unordered_set<uint64_t> state_seen;
/* CUT */
/* NOTES_OPEN */
/* STATE_OPEN */

static uint64_t size_of(const std::string& p) { return (uint64_t)std::filesystem::file_size(p); }

static void state_case(size_t tail) {
    const std::string path = dir + "hookshot_pipeline_states.bin";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        const uint32_t size = sizeof(StateRec2);
        std::fwrite(kStateMagic2, 8, 1, f), std::fwrite(&size, 4, 1, f);
        StateRec2 r{};
        r.code[0] = 1;
        std::fwrite(&r, sizeof r, 1, f);
        if (tail) {
            r.code[0] = 99;
            std::fwrite(&r, tail, 1, f);   // (part of the next one)
        }
        std::fclose(f);
    }
    state_seen.clear();
    const int before = logged;
    state_open_locked();
    CHECK(logged - before == (tail ? 1 : 0));   // (a cut is logged)
    CHECK(state_file != nullptr);
    StateRec2 r{};
    r.code[0] = 2;   // (a new pipeline met in play, as state_record writes it)
    std::fwrite(&r, sizeof r, 1, state_file);
    std::fclose(state_file), state_file = nullptr;
    CHECK(size_of(path) == 12 + 2 * sizeof(StateRec2));
    FILE* f = std::fopen(path.c_str(), "rb");
    std::fseek(f, 12, SEEK_SET);
    std::vector<uint64_t> keys;
    while (std::fread(&r, sizeof r, 1, f) == 1) keys.push_back(r.code[0]);
    std::fclose(f);
    if (keys != std::vector<uint64_t>{1, 2}) std::printf("FAIL: the state list after a part record of %zu bytes\n", tail), fails++;
}

static void notes_case(size_t tail, bool known) {
    const std::string path = pipe_notes_path();
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        const uint64_t head[2] = {program_cache_stamp(), sizeof(PipeNote)};
        std::fwrite(head, 8, 2, f);
        PipeNote n{};
        n.key = 1;
        std::fwrite(&n, sizeof n, 1, f);
        if (tail) {
            n.key = 99;
            std::fwrite(&n, tail, 1, f);
        }
        std::fclose(f);
    }
    noted.clear();
    std::vector<std::unique_ptr<PipeNote>> read;
    read.push_back(std::make_unique<PipeNote>());
    read.back()->key = 1;
    const int before = logged;
    notes_open_locked(known ? &read : nullptr);
    CHECK(logged - before == (tail ? 1 : 0));
    CHECK(note_file != nullptr && noted.count(1));
    PipeNote n{};
    n.key = 2;
    std::fwrite(&n, sizeof n, 1, note_file);
    std::fclose(note_file), note_file = nullptr;
    CHECK(size_of(path) == 16 + 2 * sizeof(PipeNote));
    FILE* f = std::fopen(path.c_str(), "rb");
    std::fseek(f, 16, SEEK_SET);
    std::vector<uint64_t> keys;
    while (std::fread(&n, sizeof n, 1, f) == 1) keys.push_back(n.key);
    std::fclose(f);
    if (keys != std::vector<uint64_t>{1, 2})
        std::printf("FAIL: the notes after a part record of %zu bytes (%s)\n", tail, known ? "read before" : "read here"), fails++;
}

int main(int, char** argv) {
    dir = std::string(argv[1]) + "/";
    for (size_t tail : {(size_t)0, (size_t)1, sizeof(StateRec2) - 1}) state_case(tail);
    for (bool known : {false, true})
        for (size_t tail : {(size_t)0, (size_t)1, sizeof(PipeNote) - 1}) notes_case(tail, known);
    return fails ? 1 : 0;
}
'''


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    src = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    records = "\n".join(src[src.index(m):src.index("\n", src.index(m))] for m in ("constexpr char kStateMagic[8]",
                                                                                  "constexpr char kStateMagic2[8]"))
    records += "\n" + block(src, "struct StateRec {") + ";\n" + block(src, "struct StateRec2 {") + ";\n"
    cut = block(src, "static void cut_part_record(") if "static void cut_part_record(" in src else ""
    code = (DRIVER.replace("/* RECORDS */", records).replace("/* CUT */", cut)
            .replace("/* NOTES_OPEN */", block(src, "void notes_open_locked("))
            .replace("/* STATE_OPEN */", block(src, "void state_open_locked() {"))
            .replace("std::mutex* unused_mu;\n", ""))
    with tempfile.TemporaryDirectory(prefix="state-list-part-") as tmp:
        cpp, exe = Path(tmp) / "lists.cpp", Path(tmp) / "lists.exe"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        str(cpp), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe), tmp], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: a list ending in part of a record:\n" + r.stdout + r.stderr)
    print("PASS: the state list and the notes are cut back to their last whole record before records are added")


if __name__ == "__main__":
    main()
