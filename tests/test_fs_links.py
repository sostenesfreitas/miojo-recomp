"""The game's file system stays inside its mounts and never works through links (host/nn_fs.c, host/mods.c).

Linux only (the posix layer's FindFirstFileA marks a symbolic link as a reparse point, as Windows does). Builds, from the
sources as they are, nn_fs.c's guest_path_ok and delete_tree and mods.c's RomFS walk over host/posix/win32_posix.c's file
functions, then checks:

- guest paths with ".." parts, a \\ or a : are refused, ordinary ones kept;
- DeleteDirectoryRecursively's walk removes a link inside the save, not the folder or file it points to;
- the mod index leaves out links to folders (two links back up made the walk endless) and still lists files;
- GetFileAttributesA marks a link as a reparse point (DeleteDirectoryRecursively on a link removes only the link);
- guest_path_ok as Windows builds it refuses device names: COM1, COM\u00b9 (UTF-8), "NUL .txt", CLOCK$.

python -I -B tests/test_fs_links.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the posix layer's checks run on Linux")
        return
    posix = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    fs = (ROOT / "host/nn_fs.c").read_text(encoding="utf-8")
    mods = (ROOT / "host/mods.c").read_text(encoding="utf-8")
    pieces = [
        block(posix, "static DWORD error_of(int e) {"),
        "static BOOL fail_errno(void) { last_error = error_of(errno); return FALSE; }",
        "enum { H_THREAD = 0x48540001, H_EVENT, H_SEMA, H_TIMER, H_FILE, H_FIND };",
        "typedef struct { int type; } HObj;",
        "static int is(HANDLE h, int type) { return h && h != INVALID_HANDLE_VALUE && (uintptr_t)h > 0x10000 && ((HObj *)h)->type == type; }",
        block(posix, "static void slashes(char *dst, size_t cap, const char *src) {"),
        block(posix, "DWORD GetFileAttributesA(LPCSTR name) {"),
        block(posix, "BOOL RemoveDirectoryA(LPCSTR name) {"),
        block(posix, "BOOL DeleteFileA(LPCSTR name) {"),
        "typedef struct { HObj h; DIR *d; char dir[PATH_MAX], pat[256]; } HFind;",
        block(posix, "static BOOL find_next(HFind *f, WIN32_FIND_DATAA *fd) {"),
        block(posix, "HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {"),
        block(posix, "BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {"),
        block(posix, "BOOL FindClose(HANDLE h) {"),
        block(fs, "static int guest_path_ok(const char *rest) {"),
        block(fs, "static void delete_tree(const char *dir) {"),
        block(mods, "static int index_romfs(const char *host_dir, const char *rel, int depth) {"),
    ]
    fixture = r'''#define _GNU_SOURCE
#include <windows.h>
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
#define HOS_FS_PATH 1024   /* (host/nn_fs.c's path buffers, #1497) */
static void hos_log(const char *f, ...) { (void)f; }
static void lower_slash(char *s) { (void)s; }
static int nput;
static void put(const char *key, const char *host, uint64_t size, int is_dir) { (void)key; (void)host; (void)size; (void)is_dir; nput++; }
''' + "\n".join(pieces) + r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    CHECK(guest_path_ok("") && guest_path_ok("slot_00/progress.sav") && guest_path_ok("a//b/c.d"));
    CHECK(!guest_path_ok("..") && !guest_path_ok("slot_00/../../x") && !guest_path_ok("a/./b"));
    CHECK(!guest_path_ok("a\\..\\b") && !guest_path_ok("C:/x") && !guest_path_ok("a:stream") && !guest_path_ok("a\nb"));
    char save[PATH_MAX], victim[PATH_MAX], keep[PATH_MAX], cmd[3 * PATH_MAX];
    snprintf(save, sizeof save, "%s/save", argv[1]);
    snprintf(victim, sizeof victim, "%s/victim", argv[1]);
    snprintf(keep, sizeof keep, "%s/victim/keep.txt", argv[1]);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/slot_00/inner' '%s' && echo x > '%s' && echo s > '%s/slot_00/progress.sav' && "
             "ln -s '%s' '%s/slot_00/link' && ln -s '%s' '%s/slot_00/inner/file'", save, victim, keep, save, victim, save, keep, save);
    CHECK(system(cmd) == 0);
    char slot[PATH_MAX];
    snprintf(slot, sizeof slot, "%s\\slot_00", save);
    delete_tree(slot);
    struct stat st;
    CHECK(stat(keep, &st) == 0);      /* what the links pointed to is still there */
    CHECK(lstat(slot, &st) != 0);     /* and the slot is gone, links and all */
    char romfs[PATH_MAX];
    snprintf(romfs, sizeof romfs, "%s/romfs", argv[1]);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/UI' && echo m > '%s/UI/a.bin' && ln -s . '%s/up1' && ln -s . '%s/up2' && "
             "ln -s '%s' '%s/UI/b.bin'", romfs, romfs, romfs, romfs, keep, romfs);
    CHECK(system(cmd) == 0);
    const int files = index_romfs(romfs, "", 0);
    CHECK(files == 2);                /* a.bin and the file link b.bin; not the folder links */
    char linked_root[PATH_MAX];       /* a mod whose romfs folder itself is a link */
    snprintf(linked_root, sizeof linked_root, "%s/linked-romfs", argv[1]);
    snprintf(cmd, sizeof cmd, "ln -s '%s' '%s'", romfs, linked_root);
    CHECK(system(cmd) == 0);
    CHECK(index_romfs(linked_root, "", 0) == 0);
    char dirlink[PATH_MAX];
    snprintf(dirlink, sizeof dirlink, "%s/up1", romfs);
    CHECK((GetFileAttributesA(dirlink) & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ==
          (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    CHECK(!(GetFileAttributesA(romfs) & FILE_ATTRIBUTE_REPARSE_POINT));
    printf("PASS: guest paths stay in their mounts; deleting a save folder removes links, not their targets; mods skip folder links\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="fs-links-") as tmp:
        tmp = Path(tmp)
        src = tmp / "fixture.c"
        src.write_text(fixture, encoding="utf-8")
        exe = tmp / "fixture"
        subprocess.run([a.cc, "-std=gnu11", "-I", str(ROOT / "host/posix"), "-Wno-unused-function", str(src), "-o", str(exe)],
                       check=True)
        work = tmp / "work"
        work.mkdir()
        r = subprocess.run([str(exe), str(work)], capture_output=True, text=True, timeout=60)
        print(r.stdout, end="")
        if r.returncode:
            print(r.stderr, end="")
            raise SystemExit("test_fs_links: failed (exit %d)" % r.returncode)
        # guest_path_ok as Windows builds it (its device-name part), on this compiler
        win = block(fs, "static int guest_path_ok(const char *rest) {").replace("#ifdef _WIN32", "#if 1")
        src = tmp / "win_names.c"
        src.write_text(r'''#include <stdio.h>
#include <string.h>
#include <strings.h>
#define _strnicmp strncasecmp
''' + win + r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    CHECK(guest_path_ok("slot_00/progress.sav") && guest_path_ok("console/a.bin") && guest_path_ok("COMX") && guest_path_ok("nullx"));
    CHECK(!guest_path_ok("CON") && !guest_path_ok("a/nul.txt") && !guest_path_ok("Com1") && !guest_path_ok("lpt9.log"));
    CHECK(!guest_path_ok("COM\xc2\xb9") && !guest_path_ok("lpt\xc2\xb3.x") && guest_path_ok("COM\xc2\xb4"));
    CHECK(!guest_path_ok("NUL .txt") && !guest_path_ok("a/aux  .b") && !guest_path_ok("CLOCK$") && !guest_path_ok("conin$"));
    CHECK(!guest_path_ok("a.") && !guest_path_ok("a ") && guest_path_ok("NUL x"));
    printf("PASS: Windows device names refused\n");
    return 0;
}
''', encoding="utf-8")
        exe = tmp / "win_names"
        subprocess.run([a.cc, "-std=gnu11", "-Wno-unused-function", str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        print(r.stdout, end="")
        if r.returncode:
            print(r.stderr, end="")
            raise SystemExit("test_fs_links: Windows names failed (exit %d)" % r.returncode)


if __name__ == "__main__":
    main()
