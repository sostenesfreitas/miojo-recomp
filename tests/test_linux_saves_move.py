"""The Linux game's move of its saves to $XDG_DATA_HOME/hookshot leaves a linked folder alone (host/hos_main.c, saves_dir).

A user/ folder beside the game is moved to the new place at the first start. A symbolic link in it, to saves the player
keeps elsewhere (a synced folder, another drive), was moved as a file: by rename, and one written from where it stood
("../../sync") then named nothing; or, from another drive, copied as the folder it names, and that folder emptied
afterwards. That start still found the saves; the next found the link in user/, kept the old place for good and showed
none. An entry the new place had already kept the old place too, but only after the others had gone.

Linux only. Builds saves_dir and what it calls, from the source as it is, over host/posix/win32_posix.c, and starts it
twice (the move, then the start after) on folders of its own:

- a link, written in full or from where it stands, is made again in the new place to the same folder, whose files stay;
- so is one deeper in (user/save/<user id>); one to an entry that moves too names it in the new place, written in full
  or not; absolute links made before and after save/ cover both creation-order and newest-first listings (the file
  system's order used to decide whether they named the old place after its entry had gone); one that names nothing
  goes as it was written;
- from another drive too, when this PC has one to try (/dev/shm);
- a user/ that is a link itself is used as it is;
- with an entry the new place has already, nothing moves;
- a user/ without links moves as before, and saves-moved.txt says where to.

python -I -B tests/test_linux_saves_move.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USER_ID = "5a3c9e1f0b7d24680000000000000001"


def build(cc, tmp):
    source = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    start = source.index("static int is_dir(const char *p) {")
    end = source.index("#endif", source.index("static const char *saves_dir(const char *beside) {"))
    program = r'''#include <windows.h>
#include <stdarg.h>
#define SEP '/'
#define SEP_S "/"
static void hos_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar(10); }
''' + source[start:end] + r'''
int main(int argc, char **argv) { if (argc != 2) return 2; printf("uses %s\n", saves_dir(argv[1])); return 0; }
'''
    src, exe = tmp / "mover.c", tmp / "mover"
    src.write_text(program, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-D_GNU_SOURCE", "-w", "-I", str(ROOT / "host" / "posix"), str(src),
                    str(ROOT / "host" / "posix" / "win32_posix.c"), "-o", str(exe), "-lpthread", "-ldl"], check=True)
    return exe


class Case:
    """A game folder with user/ in it, the player's own folder beside it, and an empty data folder: each start's answer"""

    def __init__(self, exe, name, game_parent, data_parent):
        self.exe, self.name = exe, name
        self.game = Path(tempfile.mkdtemp(prefix="game-", dir=game_parent))
        # (deeper than the game's user/, as ~/.local/share is: a link written from where it stands names another place from it)
        self.data = Path(tempfile.mkdtemp(prefix="home-", dir=data_parent)) / ".local" / "share"
        self.data.mkdir(parents=True)
        self.user, self.new = self.game / "user", self.data / "hookshot"
        self.sync = self.game.parent / (self.game.name + "-sync")   # (beside the game: "../../<name>-sync" from user/save)
        (self.user / "temp").mkdir(parents=True)
        (self.sync / USER_ID).mkdir(parents=True)
        (self.sync / USER_ID / "account.sav").write_text("my save", encoding="ascii")

    def start(self):
        env = dict(os.environ, XDG_DATA_HOME=str(self.data))
        r = subprocess.run([str(self.exe), str(self.user)], env=env, capture_output=True, text=True, timeout=60)
        self.check(r.returncode == 0, "the start failed: %s" % r.stderr)
        return Path(r.stdout.strip().splitlines()[-1][len("uses "):])

    def check(self, ok, what):
        if not ok:
            raise SystemExit("test_linux_saves_move: %s: %s" % (self.name, what))

    def saves_kept(self):
        save = self.sync / USER_ID / "account.sav"
        self.check(save.is_file() and save.read_text(encoding="ascii") == "my save", "the linked folder lost its save")

    def moved(self, save):
        """Both starts use the new place, where save reads the player's file; user/ is gone"""
        for n in (1, 2):
            self.check(self.start() == self.new, "start %d did not use the new place" % n)
        self.check((self.new / save / "account.sav").is_file(), "the save is not read from the new place")
        self.check(not self.user.exists(), "user/ is still there")
        self.check("%s\n" % self.new in (self.game / "saves-moved.txt").read_text(encoding="utf-8"), "saves-moved.txt")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the posix layer's checks run on Linux")
        return
    with tempfile.TemporaryDirectory(prefix="saves-move-") as tmp:
        tmp = Path(tmp)
        exe = build(a.cc, tmp)

        def case(name, game_parent=tmp):
            return Case(exe, name, game_parent, tmp)

        c = case("a link written in full")
        (c.user / "save").symlink_to(c.sync)
        c.moved("save/" + USER_ID)
        c.check((c.new / "save").is_symlink() and (c.new / "save").resolve() == c.sync.resolve(), "the new place has no link to it")
        c.saves_kept()

        c = case("a link written from where it stands")
        (c.user / "save").symlink_to("../../%s" % c.sync.name)
        c.moved("save/" + USER_ID)
        c.check((c.new / "save").resolve() == c.sync.resolve(), "the link names another folder now")
        c.saves_kept()

        c = case("a link deeper in")
        (c.user / "save").mkdir()
        (c.user / "save" / USER_ID).symlink_to("../../../%s/%s" % (c.sync.name, USER_ID))
        (c.user / "save" / "notes.txt").write_text("mine", encoding="ascii")
        c.moved("save/" + USER_ID)
        c.check((c.new / "save" / USER_ID).is_symlink() and (c.new / "save" / "notes.txt").read_text(encoding="ascii") == "mine",
                "the folder around the link did not move as it was")
        c.saves_kept()

        c = case("a link to an entry that moves too")
        # (both creation-order and newest-first listings visit some links after save/, which has gone by then)
        firsts = ["last-%s" % ch for ch in "abcdefghijklmnopqrstuvwxyz0123456789"]
        for name in firsts[::2]:
            (c.user / name).symlink_to(c.user / "save" / USER_ID)
        (c.user / "save" / USER_ID).mkdir(parents=True)
        (c.user / "save" / USER_ID / "account.sav").write_text("my save", encoding="ascii")
        (c.user / "save" / "latest").symlink_to(USER_ID)
        (c.user / "last").symlink_to(c.user / "save" / USER_ID)
        for name in firsts[1::2]:
            (c.user / name).symlink_to(c.user / "save" / USER_ID)
        c.moved("save/latest")
        for link in [c.new / "save" / "latest", c.new / "last"] + [c.new / name for name in firsts]:
            c.check(link.is_symlink() and link.resolve() == (c.new / "save" / USER_ID).resolve(), "%s names another folder" % link.name)

        c = case("a link that names nothing")
        (c.user / "old").symlink_to("../gone")
        (c.user / "save" / USER_ID).mkdir(parents=True)
        (c.user / "save" / USER_ID / "account.sav").write_text("my save", encoding="ascii")
        c.moved("save/" + USER_ID)
        c.check(os.readlink(c.new / "old") == "../gone", "it did not go as it was written")

        other = Path("/dev/shm")
        drives = other.is_dir() and os.access(other, os.W_OK) and other.stat().st_dev != tmp.stat().st_dev
        if drives:
            with tempfile.TemporaryDirectory(prefix="saves-move-", dir=other) as far:
                c = case("a link, the game on another drive", Path(far))
                (c.user / "save").symlink_to(c.sync)
                c.moved("save/" + USER_ID)
                c.check((c.new / "save").resolve() == c.sync.resolve(), "the new place has no link to it")
                c.saves_kept()

                c = case("no link, the game on another drive", Path(far))
                (c.user / "save" / USER_ID).mkdir(parents=True)
                (c.user / "save" / USER_ID / "account.sav").write_text("my save", encoding="ascii")
                c.moved("save/" + USER_ID)
                c.check(not (c.new / "save").is_symlink(), "a folder became a link")

        c = case("user/ is a link itself")
        kept = c.game / "kept"
        c.user.rename(kept)
        (kept / "save").symlink_to(c.sync)
        c.user.symlink_to(kept)
        for n in (1, 2):
            c.check(c.start() == c.user, "start %d left the player's place" % n)
        c.check((kept / "temp").is_dir() and (kept / "save").is_symlink() and not c.new.joinpath("temp").exists(), "something moved")
        c.saves_kept()

        c = case("the new place has an entry already")
        (c.user / "save" / USER_ID).mkdir(parents=True)
        (c.user / "cache").mkdir()
        (c.new / "temp").mkdir(parents=True)
        for n in (1, 2):
            c.check(c.start() == c.user, "start %d left the old place" % n)
        c.check((c.user / "cache").is_dir() and (c.user / "save" / USER_ID).is_dir() and not (c.new / "cache").exists()
                and not (c.new / "save").exists(), "an entry went to the new place all the same")

        c = case("no link")
        (c.user / "save" / USER_ID).mkdir(parents=True)
        (c.user / "save" / USER_ID / "account.sav").write_text("my save", encoding="ascii")
        c.moved("save/" + USER_ID)
        c.check((c.new / "temp").is_dir() and not (c.new / "save").is_symlink(), "the folders did not move as they were")

    print("PASS: the saves' move makes a link again in the new place and leaves its folder alone%s; a linked user/ and a "
          "new place with entries keep the old place whole" % (", from another drive too" if drives else " (no other drive to try here)"))


if __name__ == "__main__":
    main()
