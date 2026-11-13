"""On Linux, "Skip this version" in the game's update question is kept, and the game keeps its saves (host/hos_update.c,
host/hos_main.c).

The check wrote hookshot_update_skip.txt to user/ beside the game, as on Windows. Linux keeps its saves in
$XDG_DATA_HOME/hookshot, and the same start then moved that user/ folder there as saves from an earlier build
(saves_dir): the file went along, saves-moved.txt was written, and the next start asked again. Skipped a second time,
the file was in both places, which the move reads as saves it cannot move: the game ran on the empty user/ beside it
from then on, and showed no saves.

Linux only. Builds the game's start as hos_main.c has it (the part of main that finds the game folder, asks about an
update and picks the saves' place, with saves_dir) with hos_update.c and host/posix/win32_posix.c, puts it in a game
folder of its own as bin/hookshot, and starts it with a curl that names a release and a zenity that answers "Skip this
version" and notes that it was asked:

- with saves in the data folder: asked once, never again for that release, and every start uses the data folder; no
  user/ and no saves-moved.txt appear beside the game; a newer release is asked about again;
- with a user/ from an earlier build that holds saves and a skip of this release: moved once, and not asked.

python -I -B tests/test_linux_update_skip.py [--cc gcc]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USER_ID = "5a3c9e1f0b7d24680000000000000001"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def build(cc, tmp):
    main = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    start = main.index("static int is_dir(const char *p) {")
    saves = main[start:main.index("#endif", main.index("static const char *saves_dir(const char *beside) {"))]
    program = r'''#include <windows.h>
#include <stdarg.h>
#include "hos_paths.h"
#define SEP '/'
#define SEP_S "/"
static void hos_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar(10); }
const char *hos_romfs_dir, *hos_user_dir;
void hos_update_check(void);
''' + saves + r'''
int main(void) {
    const char *image = NULL;
    static char beside_image[MAX_PATH], beside_romfs[MAX_PATH], beside_user[MAX_PATH];
''' + block(main, "    if (!image) {   /* (hookshot) started with nothing") + r'''
    printf("uses %s\n", hos_user_dir ? hos_user_dir : "");
    return image ? 0 : 2;
}
'''
    src, exe = tmp / "start.c", tmp / "start"
    src.write_text(program, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-D_GNU_SOURCE", "-w", "-I", str(ROOT / "host" / "posix"), "-I", str(ROOT / "host"), str(src),
                    str(ROOT / "host" / "hos_update.c"), str(ROOT / "host" / "posix" / "win32_posix.c"), "-o", str(exe),
                    "-lpthread", "-ldl"], check=True)
    return exe


class Game:
    """A built game folder (bin/hookshot is the program above), a data folder, and the tools the check starts"""

    def __init__(self, exe, tmp, name):
        self.name = name
        home = Path(tempfile.mkdtemp(prefix="home-", dir=tmp))
        self.game, self.data, tools = home / "Games" / "Hookshot", home / ".local" / "share", home / "tools"
        self.new, self.asked = self.data / "hookshot", home / "asked.txt"
        for folder in (self.game / "bin", self.data, tools):
            folder.mkdir(parents=True)
        shutil.copy2(exe, self.game / "bin" / "hookshot")
        (self.game / "image.bin").write_bytes(b"")
        (self.game / "hookshot_version.txt").write_text("version=v2.1.0\n", encoding="ascii")
        for tool, script in (("curl", 'printf \'{"tag_name":"%s"}\' "$HOOKSHOT_TEST_TAG"'),
                             ("zenity", 'echo asked >> "$HOOKSHOT_TEST_ASKED"; echo "Skip this version"')):
            (tools / tool).write_text("#!/bin/sh\n%s\n" % script, encoding="ascii")
            (tools / tool).chmod(0o755)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("HOOKSHOT_")}
        self.env.update(PATH="%s:%s" % (tools, os.environ.get("PATH", "")), HOME=str(home), XDG_DATA_HOME=str(self.data),
                        HOOKSHOT_TEST_ASKED=str(self.asked))

    def save_in(self, user):
        (user / "save" / USER_ID).mkdir(parents=True)
        (user / "save" / USER_ID / "account.sav").write_text("my save", encoding="ascii")

    def check(self, ok, what):
        if not ok:
            raise SystemExit("test_linux_update_skip: %s: %s" % (self.name, what))

    def start(self, latest, asked):
        """A start with `latest` out: the saves' place is the data folder's, the question was put `asked` times so far"""
        r = subprocess.run([str(self.game / "bin" / "hookshot")], env=dict(self.env, HOOKSHOT_TEST_TAG=latest), capture_output=True,
                           text=True, timeout=60)
        self.check(r.returncode == 0, "the start failed: %s" % r.stderr)
        uses = r.stdout.strip().splitlines()[-1][len("uses "):]
        self.check(uses == str(self.new), "the game uses %s for its saves, not %s" % (uses, self.new))
        self.check((self.new / "save" / USER_ID / "account.sav").is_file(), "the save is not in the saves' place")
        times = len(self.asked.read_text(encoding="ascii").split()) if self.asked.exists() else 0
        self.check(times == asked, "asked %d times, not %d" % (times, asked))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the posix layer's checks run on Linux")
        return
    with tempfile.TemporaryDirectory(prefix="update-skip-") as tmp:
        tmp = Path(tmp)
        exe = build(a.cc, tmp)

        g = Game(exe, tmp, "saves in the data folder")
        g.save_in(g.new)
        g.start("v9.9.9", asked=1)
        g.start("v9.9.9", asked=1)
        g.start("v9.9.9", asked=1)
        g.check(not (g.game / "user").exists() and not (g.game / "saves-moved.txt").exists(), "the skip was taken for saves to move")
        g.check((g.new / "hookshot_update_skip.txt").read_text(encoding="ascii") == "skip=v9.9.9\n", "the skip is not with the saves")
        g.start("v9.9.10", asked=2)   # (a newer release: asked about again, and that one skipped)
        g.start("v9.9.10", asked=2)

        g = Game(exe, tmp, "a user folder from an earlier build")
        g.save_in(g.game / "user")
        (g.game / "user" / "hookshot_update_skip.txt").write_text("skip=v9.9.9\n", encoding="ascii")
        g.start("v9.9.9", asked=0)
        g.start("v9.9.9", asked=0)
        g.check(not (g.game / "user").exists() and (g.game / "saves-moved.txt").is_file(), "the old user folder did not move")

    print("PASS: a skipped release is asked about once, its note kept with the saves; the game keeps its saves' place")


if __name__ == "__main__":
    main()
