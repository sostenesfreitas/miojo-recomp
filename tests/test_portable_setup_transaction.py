"""Execute production portable part copying and repair publication over authored directories."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "Windows setup directory transactions"}


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = (ROOT / "builder/setup/setup.c").read_text(encoding="utf-8")
    code = r'''
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdlib.h>
#include "portable_home.h"
static HbHome g_home;
static void fail(const wchar_t *format, ...) { (void)format; exit(90); }
static const wchar_t *failed_stage;
static BOOL move_fixture(LPCWSTR from, LPCWSTR to) {
    if (failed_stage && !wcscmp(from,failed_stage)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return MoveFileW(from,to);
}
#define MoveFileW move_fixture
#define Sleep(ms) ((void)(ms))
'''
    part_end = source.index("} Part;") + len("} Part;")
    code += source[source.rfind("typedef struct", 0, part_end):part_end] + "\n"
    for marker in ("static int home_path(", "static void rmtree(", "static void mkdirs(", "static void read_small(", "static int part_folder(",
                   "static int copy_part_tree(", "static BOOL move_retry(", "static int publish_install(", "static int reuse_part("):
        code += function(source, marker) + "\n"
    code += r'''
int wmain(int argc, wchar_t **argv) {
    if (argc != 5) return 20;
    int mode = _wtoi(argv[1]);
    if (mode == 4 || mode == 5) {
        Part part = {0};
        strcpy(part.sha,"authored-checksum"); strcpy(part.dest,"."); strcpy(part.from,"-"); strcpy(part.to,"-");
        strcpy(part.url,"https://invalid.example/HookshotBuilder-python-authored.zip");
        g_home.portable = mode == 4;
        return reuse_part(argv[2],"v-new",&part,argv[3]) == _wtoi(argv[4]) ? 0 : 22;
    }
    if (mode == 2) failed_stage = argv[2];
    int ok = mode == 3 ? copy_part_tree(argv[2],argv[3]) : publish_install(argv[2],argv[3],mode != 0);
    return ok == _wtoi(argv[4]) ? 0 : 21;
}
'''
    with tempfile.TemporaryDirectory(prefix="portable-transaction-") as folder:
        root = Path(folder)
        c, exe = root / "transaction.c", root / "transaction.exe"
        c.write_text(code, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-municode", "-I" + str(ROOT / "builder/setup"), str(c), "-o", str(exe)], check=True)
        def run(mode, source, target, result):
            subprocess.run([str(exe), str(mode), str(source), str(target), str(result)], check=True)
        source, copied = root / "old parts ü, tree", root / "staged parts"
        (source / "nested").mkdir(parents=True)
        (source / "nested/keep").write_text("old part content")
        (source / "empty").mkdir()
        run(3, source, copied, 1)
        assert (source / "nested/keep").read_text() == (copied / "nested/keep").read_text()
        (copied / "nested/keep").write_text("changed copy")
        assert (source / "nested/keep").read_text() == "old part content"
        run(3, source, copied, 0)
        assert (copied / "nested/keep").read_text() == "changed copy"
        run(3, root / "missing", root / "missing copy", 0)
        final, staged = root / "installed", root / "candidate.part"
        final.mkdir()
        (final / "keep").write_text("last known good")
        staged.mkdir()
        (staged / "keep").write_text("new version")
        run(2, staged, final, 0)
        assert (final / "keep").read_text() == "last known good"
        assert (staged / "keep").read_text() == "new version"
        assert not list(root.glob("installed.rollback-*"))
        run(1, staged, final, 1)
        assert (final / "keep").read_text() == "new version" and not staged.exists()
        assert not list(root.glob("installed.rollback-*"))
        next_stage = root / "next.part"
        next_stage.mkdir()
        (next_stage / "keep").write_text("standard candidate")
        run(0, next_stage, final, 0)
        assert (final / "keep").read_text() == "new version"
        fresh = root / "fresh version"
        run(1, next_stage, fresh, 1)
        assert (fresh / "keep").read_text() == "standard candidate"
        versions = root / "versions"
        old = versions / "v-old/HookshotBuilder/tools/python"
        old.mkdir(parents=True)
        (old / "keep").write_text("original reusable part")
        (versions / "current.txt").write_text("v-old\n")
        (versions / "v-old/parts.txt").write_text("authored-checksum\n")
        candidate = versions / "v-new.part"
        run(4, versions, candidate, 1)
        copied_part = candidate / "HookshotBuilder/tools/python/keep"
        assert old.is_dir() and copied_part.read_text() == "original reusable part"
        copied_part.write_text("isolated portable copy")
        assert (old / "keep").read_text() == "original reusable part"
        standard_candidate = versions / "standard.part"
        run(5, versions, standard_candidate, 1)
        assert not old.exists() and (standard_candidate / "HookshotBuilder/tools/python/keep").read_text() == "original reusable part"
        print("portable setup transaction: 9 native scenarios passed; actual part reuse, source preservation, repair rollback")


if __name__ == "__main__":
    main()
