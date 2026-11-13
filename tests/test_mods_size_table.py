"""Game-free: a mod's resource size table and the button prompts' (host/prompts.c, host/mods.c). The game reads one size
table, and the prompts' fonts are larger than the game's: before, a mod that shipped its own table won and the prompts
switched their font off (the Switch's icons in every text while a randomizer seed was loaded). Now the mod's table is served
with the prompts' entries merged into it. On made-up tables, no game data:

- merge_restbl, cut out of host/prompts.c: the mod's entries with the prompts' larger ones and those it lacks, hashed and
  named, sorted as the game's; tables it cannot merge refused (not a RESTBL, a wrong length, other name fields, out of order);
- (Linux: host/posix's file calls) host/mods.c and host/prompts.c as they are, over real folders, with zstd's decoder: what the
  game is served for the size table, the font and a mod's file with no mod, a mod with a table, a mod with two versions'
  tables (only the game's table name seeds the merge, whether the font or table is opened first), a mod without one that grows
  a file, two mods with tables (the one that sorts last is the base), a mod table that cannot be merged (served as it is, the
  game's own icons), no table of the prompts' (the mod's as it is, the icons kept), a mod with its own font (its font, the
  merged table), and, with the zstandard package, a table compressed as Clawshot writes it and one whose frame does not give
  its size.

python -I -B tests/test_mods_size_table.py [--cc gcc]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
TABLE = "System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs"
OTHER = "System/Resource/ResourceSizeTable.Product.110.Nin_Ounce_NVN2.rsizetable.zs"
FONT = "Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs"
GROWN = "Banc/Test/Test_Static.bcett.byml.zs"
ZSTD = ROOT / "third_party/zstd/lib"
ZSTD_SOURCES = ["common/debug.c", "common/entropy_common.c", "common/error_private.c", "common/fse_decompress.c",
                "common/xxhash.c", "common/zstd_common.c", "decompress/huf_decompress.c", "decompress/zstd_ddict.c",
                "decompress/zstd_decompress.c", "decompress/zstd_decompress_block.c"]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def restbl(hashed, named=(), name_size=160, magic=b"RESTBL"):
    """a size table as the game's: hashed (CRC, size) and named (path, size) entries, in the order given"""
    out = struct.pack("<6sIIII", magic, 1, name_size, len(hashed), len(named))
    out += b"".join(struct.pack("<II", c, s) for c, s in hashed)
    return out + b"".join(n.encode().ljust(name_size, b"\0") + struct.pack("<I", s) for n, s in named)


def merged(a, b):
    """the table host/prompts.c should make of a mod's (a) and ours (b), from their entries: each the larger, sorted"""
    def entries(t):
        ns, nh, nn = struct.unpack_from("<III", t, 10)
        hashed = dict(struct.unpack_from("<II", t, 22 + 8 * i) for i in range(nh))
        at = 22 + 8 * nh
        named = {t[at + (ns + 4) * i:at + (ns + 4) * i + ns].rstrip(b"\0").decode():
                 struct.unpack_from("<I", t, at + (ns + 4) * i + ns)[0] for i in range(nn)}
        return ns, hashed, named
    ns, ah, an = entries(a)
    _, bh, bn = entries(b)
    hashed = {k: max(ah.get(k, 0), bh.get(k, 0)) for k in set(ah) | set(bh)}
    named = {k: max(an.get(k, 0), bn.get(k, 0)) for k in set(an) | set(bn)}
    return restbl(sorted(hashed.items()), sorted(named.items(), key=lambda e: e[0].encode()), ns)


def frame(data):
    """data as a zstd frame of raw blocks of 128 KB, as host/prompts.c writes them (no zstandard needed)"""
    out = struct.pack("<IBBI", 0xFD2FB528, 0x80, 7 << 3, len(data))
    at = 0
    while True:
        k = min(len(data) - at, 131072)
        out += ((k << 3) | (at + k == len(data))).to_bytes(3, "little") + data[at:at + k]
        at += k
        if at == len(data):
            return out


def unframe(data):
    """the content of a frame of raw blocks (frame)"""
    check(data[:4] == b"\x28\xb5\x2f\xfd" and data[4] == 0x80, "the merged table is not a frame as write_zstd_raw writes")
    size, = struct.unpack_from("<I", data, 6)
    out, at = b"", 10
    while True:
        bh = int.from_bytes(data[at:at + 3], "little")
        check((bh >> 1) & 3 == 0, "a block that is not raw")
        out += data[at + 3:at + 3 + (bh >> 3)]
        at += 3 + (bh >> 3)
        if bh & 1:
            check(len(out) == size and at == len(data), "the frame's size or end")
            return out


# made-up tables: the game's; ours (the prompts': the font's entry raised, 0x30); two mods' (the first raises 0x10; the
# second a grown placement file's 0x20, a new file 0x50 and a named entry)
GAME = [(0x10, 100), (0x20, 200), (0x30, 300), (0x40, 400)], [("Collide/A", 50), ("Collide/B", 60)]
OURS = restbl([(0x10, 100), (0x20, 200), (0x30, 900), (0x40, 400)], GAME[1])
MOD_A = restbl([(0x10, 111), (0x20, 200), (0x30, 300), (0x40, 400)], GAME[1])
MOD_B = restbl([(0x10, 100), (0x20, 250), (0x30, 300), (0x40, 400), (0x50, 500)], [("Collide/A", 50), ("Collide/B", 70)])


def merge_checks(cc, src, tmp):
    harness = "\n".join([
        "#include <stdint.h>", "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
        re.search(r"static uint32_t rd32le\(.*", src).group(0),
        re.search(r"static void wr32le\(.*", src).group(0),
        block(src, "static int restbl_head("),
        block(src, "static int restbl_cmp("),
        block(src, "static long restbl_part("),
        block(src, "static uint8_t *merge_restbl("),
        "static uint8_t *load(const char *path, size_t *n) {",
        "    FILE *f = fopen(path, \"rb\");",
        "    static uint8_t buf[2][1 << 16];",
        "    static int k;",
        "    *n = fread(buf[k], 1, sizeof buf[k], f);",
        "    fclose(f);",
        "    return buf[k++];",
        "}",
        "int main(int argc, char **argv) {",
        "    if (argc != 4) return 2;",
        "    size_t an, bn, n;",
        "    int raised;",
        "    const uint8_t *a = load(argv[1], &an), *b = load(argv[2], &bn);",
        "    uint8_t *t = merge_restbl(a, an, b, bn, &n, &raised);",
        "    if (!t) { puts(\"refused\"); return 0; }",
        "    FILE *f = fopen(argv[3], \"wb\");",
        "    fwrite(t, 1, n, f);",
        "    fclose(f);",
        "    printf(\"%d\\n\", raised);",
        "    free(t);",
        "    return 0;",
        "}", ""])
    c, exe = tmp / "merge.c", tmp / ("merge.exe" if os.name == "nt" else "merge")
    c.write_text(harness, encoding="utf-8")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)

    def run(a, b):
        (tmp / "a").write_bytes(a)
        (tmp / "b").write_bytes(b)
        out = subprocess.run([str(exe), str(tmp / "a"), str(tmp / "b"), str(tmp / "out")], check=True, capture_output=True,
                             text=True).stdout.strip()
        return None if out == "refused" else (int(out), (tmp / "out").read_bytes())

    raised, got = run(MOD_B, OURS)
    check(got == merged(MOD_B, OURS), "the mod's table with ours merged in")
    check(raised == 1, "%d of our entries went in, not 1 (the font's)" % raised)
    check(struct.unpack_from("<II", got, 22 + 8 * 2) == (0x30, 900), "the font's entry is not ours")
    check(struct.unpack_from("<II", got, 22 + 8 * 1) == (0x20, 250), "the mod's grown entry was lowered")
    # entries only on one side, both ways, and named ones: each the larger, sorted
    a = restbl([(5, 1), (0x20, 999), (0xFFFFFFF0, 7)], [("B", 9), ("D", 1)])
    b = restbl([(1, 3), (0x20, 10), (0x30, 4)], [("A", 2), ("B", 10), ("C", 3)])
    raised, got = run(a, b)
    check(got == merged(a, b) and raised == 5, "entries on one side only, named ones (%d raised)" % raised)
    raised, got = run(restbl([]), b)
    check(got == b and raised == 6, "an empty mod table takes all of ours")
    # tables it cannot merge
    for what, mod, ours in (("not a RESTBL", restbl([(1, 1)], magic=b"RESTBX"), OURS),
                            ("a wrong length", MOD_B + b"\0", OURS),
                            ("a short table", b"RESTBL" + bytes(10), OURS),
                            ("other name fields", restbl([(1, 1)], name_size=128), OURS),
                            ("hashed entries out of order", restbl([(2, 1), (1, 1)]), OURS),
                            ("a hashed entry twice", restbl([(1, 1), (1, 2)]), OURS),
                            ("named entries out of order", MOD_B, restbl([], [("B", 1), ("A", 1)]))):
        check(run(mod, ours) is None, "%s was merged" % what)
    print("ok: merge_restbl: the larger entry of each file, entries of one side kept, sorted; 7 unmergeable tables refused")


POSIX_PIECES = ["static DWORD error_of(int e) {", "static void slashes(char *dst, size_t cap, const char *src) {",
                "DWORD GetFileAttributesA(LPCSTR name) {", "static BOOL find_next(HFind *f, WIN32_FIND_DATAA *fd) {",
                "HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {",
                "BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {", "BOOL FindClose(HANDLE h) {"]


def routing_checks(cc, tmp, zstandard, host):
    """mods.c and prompts.c as they are, over folders: what the game is served"""
    posix = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    src = tmp / "src"
    src.mkdir(parents=True)
    for f in ("mods.c", "prompts.c"):   # (beside a hos.h of the test's: hos_log, and what the live prompts read: none here)
        (src / f).write_bytes((host / f).read_bytes())
    (src / "hos.h").write_text("#pragma once\n#include <stddef.h>\n#include <stdint.h>\nvoid hos_log(const char *f, ...);\n"
                               "#define HOS_WRITTEN(p, n) ((void)0)\n"
                               "static inline void *hos_guest_range(uint64_t a, uint64_t n) { (void)a; (void)n; return NULL; }\n"
                               "int hos_regions(uint64_t (*out)[2], int cap);\nint64_t hos_now_ns(void);\n")
    pieces = [block(posix, m) for m in POSIX_PIECES]
    pieces.insert(1, "static BOOL fail_errno(void) { last_error = error_of(errno); return FALSE; }\n"
                     "enum { H_THREAD = 0x48540001, H_EVENT, H_SEMA, H_TIMER, H_FILE, H_FIND };\n"
                     "typedef struct { int type; } HObj;\n"
                     "static int is(HANDLE h, int type) { return h && h != INVALID_HANDLE_VALUE && (uintptr_t)h > 0x10000 && "
                     "((HObj *)h)->type == type; }")
    pieces.insert(4, "typedef struct { HObj h; DIR *d; char dir[PATH_MAX], pat[256]; } HFind;")
    (tmp / "posix.c").write_text(r'''#include <windows.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static DWORD last_error;
''' + "\n".join(pieces) + r'''
void Sleep(DWORD ms) { usleep(ms * 1000); }
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap) { (void)m; return (DWORD)snprintf(out, cap, "%s", getenv("TEST_GAME_EXE")); }
''', encoding="utf-8")
    (tmp / "main.c").write_text(r'''#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
void hos_log(const char *f, ...) { va_list ap; va_start(ap, f); printf("log: "); vprintf(f, ap); printf("\n"); va_end(ap); }
int hos_binding(int set, int button, int slot) { (void)set; (void)button; (void)slot; return -1; }
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) { (void)buttons; (void)stick; return 0; }
int hos_xinput_pads(void) { return 0; } /* no controllers in this routing fixture */
void hos_sdl_pad_first_name(char *out, size_t cap) { if (cap) *out = 0; }
int hos_regions(uint64_t (*out)[2], int cap) { (void)out; (void)cap; return 0; }
int64_t hos_now_ns(void) { return 0; }
int rx_option(int id, int otherwise) { (void)id; return otherwise; }
void hos_mods_init(const char *image_path);
int hos_mods_romfs(const char *rel, char *out, size_t cap);
const char *hos_romfs_dir;
int main(int argc, char **argv) {
    hos_romfs_dir = getenv("TEST_ROMFS");
    hos_mods_init(NULL);
    for (int i = 1; i < argc; i++) {
        char out[4096] = "";
        const int r = hos_mods_romfs(argv[i], out, sizeof out);
        printf("open %s: %s\n", argv[i], r ? out : "(the game's)");
    }
    return 0;
}
''', encoding="utf-8")
    exe = tmp / "fixture"
    subprocess.run([cc, "-std=gnu11", "-D_GNU_SOURCE", "-DZSTD_DISABLE_ASM", "-DZSTD_LEGACY_SUPPORT=0", "-DZSTD_MULTITHREAD=0",
                    "-I", str(ROOT / "host"), "-I", str(ROOT / "host/posix"), "-I", str(ZSTD), str(src / "mods.c"),
                    str(src / "prompts.c"), str(tmp / "posix.c"), str(tmp / "main.c"),
                    *(str(ZSTD / f) for f in ZSTD_SOURCES), "-o", str(exe)], check=True)

    def case(name, mods, ours=OURS, opens=(TABLE, FONT)):
        """a game folder with the prompts (the faces' font, ours as their table unless None) and mods {folder: {path: bytes}};
        the fixture opens each of opens: {path: what was served}, the merged table's content if served, the log"""
        top = tmp / name
        game = top / "game"
        (game / "prompts/xbox").mkdir(parents=True)
        (game / "hookshot_options.ini").write_text("button_prompts=2\n")   # (Xbox: no controller asked for)
        (game / "romfs" / TABLE).parent.mkdir(parents=True)
        (game / "romfs" / TABLE).write_bytes(frame(restbl(*GAME)))
        (game / "prompts/External.bfotf").write_bytes(struct.pack("<I", 0x18029A7F) + bytes(28))   # (every face's: one font)
        if ours is not None:
            (game / "prompts/rsizetable.zs").write_bytes(frame(ours))
        (top / "mods").mkdir()
        for folder, files in mods.items():
            for rel, data in files.items():
                (top / "mods" / folder / "romfs" / rel).parent.mkdir(parents=True, exist_ok=True)
                (top / "mods" / folder / "romfs" / rel).write_bytes(data)
        env = dict(os.environ, TEST_GAME_EXE=str(game / "hookshot"), HOS_MODS=str(top / "mods"), TEST_ROMFS=str(game / "romfs"))
        out = subprocess.run([str(exe), *opens], env=env, check=True, capture_output=True, text=True, timeout=60).stdout
        served = {}
        for line in out.splitlines():
            if line.startswith("open "):
                rel, _, what = line[5:].partition(": ")
                what = what.replace("\\", "/")
                served[rel] = what if what == "(the game's)" else os.path.relpath(what, top).replace("\\", "/")
        table = served.get(TABLE, "")
        content = unframe((top / table).read_bytes()) if table == "game/prompts/rsizetable.mod.zs" else None
        return served, content, out

    mod_file = "mods/%s/romfs/" + TABLE
    # no mod: our table and the font
    served, _, log = case("none", {})
    check(served == {TABLE: "game/prompts/rsizetable.zs", FONT: "game/prompts/External.bfotf.zs"}, "no mod: %s" % served)
    # a mod with a table: the mod's with ours merged in, and the font; in either order of opening
    for i, opens in enumerate(((TABLE, FONT), (FONT, TABLE))):
        served, content, log = case("one-%d" % i, {"seed": {TABLE: frame(MOD_B)}}, opens=opens)
        check(served == {TABLE: "game/prompts/rsizetable.mod.zs", FONT: "game/prompts/External.bfotf.zs"},
              "a mod with a table: %s\n%s" % (served, log))
        check(content == merged(MOD_B, OURS), "a mod with a table: the merged table's entries")
        check("with the button prompts' sizes merged in" in log and "1 entries raised or added" in log, "its log:\n" + log)
    # Both tables are made up. The extra version sorts first in the old hash lookup.
    version_errors = []
    for i, opens in enumerate(((TABLE, FONT), (FONT, TABLE))):
        served, content, log = case("versions-%d" % i, {"seed": {TABLE: frame(MOD_B), OTHER: frame(MOD_A)}}, opens=opens)
        if served != {TABLE: "game/prompts/rsizetable.mod.zs", FONT: "game/prompts/External.bfotf.zs"}:
            version_errors.append("two table versions, %s first: %s\n%s" % ("table" if i == 0 else "font", served, log))
        if content != merged(MOD_B, OURS):
            version_errors.append("two table versions, %s first: the merge is not the opened table's" % ("table" if i == 0 else "font"))
        if "Product.110" in log:
            version_errors.append("two table versions: the other one was read:\n" + log)
    check(not version_errors, "\n".join(version_errors))
    # a mod without a table that grows a file: our table (its entry is still the game's), its file, the font
    served, _, log = case("grows", {"grown": {GROWN: frame(b"x" * 4096)}}, opens=(TABLE, GROWN, FONT))
    check(served == {TABLE: "game/prompts/rsizetable.zs", GROWN: mod_file.replace(TABLE, GROWN) % "grown",
                     FONT: "game/prompts/External.bfotf.zs"}, "a mod without a table: %s" % served)
    # two mods with tables: the one that sorts last is the base (its entries; the other's raise is not in it)
    served, content, log = case("two", {"01-a": {TABLE: frame(MOD_A)}, "02-b": {TABLE: frame(MOD_B)}})
    check(served[TABLE] == "game/prompts/rsizetable.mod.zs" and content == merged(MOD_B, OURS), "two mods: %s" % served)
    check(("02-b" in log.split("with the button prompts' sizes")[0].splitlines()[-1]), "two mods: 02-b's table:\n" + log)
    check(served[FONT] == "game/prompts/External.bfotf.zs", "two mods: the font")
    # a mod table that cannot take ours: as it is, and the game's own icons
    served, _, log = case("bad", {"bad": {TABLE: frame(b"NOT A TABLE" * 4)}})
    check(served == {TABLE: mod_file % "bad", FONT: "(the game's)"}, "a table that cannot be merged: %s" % served)
    check("not a resource size table as the game's" in log and "the game's own icons in texts" in log, "its log:\n" + log)
    # no table of ours (the faces' fonts fit the game's entry): the mod's as it is, and the font
    served, _, log = case("no-ours", {"seed": {TABLE: frame(MOD_B)}}, ours=None)
    check(served == {TABLE: mod_file % "seed", FONT: "game/prompts/External.bfotf.zs"}, "no table of ours: %s" % served)
    # a mod with a table and the font of its own: its font wins, its table still gets ours
    served, content, log = case("own-font", {"seed": {TABLE: frame(MOD_B), FONT: b"a mod's own font"}})
    check(served == {TABLE: "game/prompts/rsizetable.mod.zs", FONT: mod_file.replace(TABLE, FONT) % "seed"}
          and content == merged(MOD_B, OURS), "a mod with its own font: %s" % served)
    n = 10
    if zstandard:   # (Clawshot writes its table at level 19, with the frame's size: zstandard's default)
        served, content, log = case("level19", {"seed": {TABLE: zstandard.ZstdCompressor(level=19).compress(MOD_B)}})
        check(content == merged(MOD_B, OURS), "a compressed mod table: %s" % served)
        sizeless = zstandard.ZstdCompressor(level=3, write_content_size=False).compress(MOD_B)
        served, _, log = case("sizeless", {"seed": {TABLE: sizeless}})
        check(served == {TABLE: mod_file % "seed", FONT: "(the game's)"} and "not a zstd frame that gives its size" in log,
              "a frame without its size: %s" % served)
        n += 2
    print("ok: %d cases served as they should: no mod, a mod with a table, two table versions in both open orders, "
          "one that grows a file without, two mods, a table that cannot be merged, no table of ours, a mod's own font%s"
          % (n, ", compressed and size-less frames" if zstandard else " (no zstandard: compressed frames not tried)"))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--source", type=Path, default=ROOT / "host", help="host source directory to exercise")
    a = p.parse_args()
    src = (a.source / "prompts.c").read_text(encoding="utf-8")
    try:
        import zstandard
    except ImportError:
        zstandard = None
    with tempfile.TemporaryDirectory(prefix="mods-size-table-") as tmp:
        merge_checks(a.cc, src, Path(tmp))
        if os.name == "nt" or sys.platform == "darwin":
            print("SKIP: the mods' and prompts' files over folders run on Linux (host/posix's file calls)")
            return
        routing_checks(a.cc, Path(tmp) / "routing", zstandard, a.source)


if __name__ == "__main__":
    main()
