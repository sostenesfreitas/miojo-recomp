"""Copy Ocarina of Time saves between Hookshot and the emulators and ports that run the game: they all keep the same
files, so a copy is all it takes.

    python -I scripts/oot_saves.py list
    python -I scripts/oot_saves.py import <name>  [--profile ID] [--keep-settings] [--dry-run] [--force]
    python -I scripts/oot_saves.py export <name>  [--profile ID] [--keep-settings] [--dry-run] [--force]
    python -I scripts/oot_saves.py import --from DIR   (a save from any folder into Hookshot)
    python -I scripts/oot_saves.py export --to DIR     (Hookshot's save into any folder)

import copies into Hookshot, export copies out of it. <name> is one of EMULATORS below: lichii, frizti, orchid,
pokedinner, eden, eglantine or nx2navi (to-lichii and to-hookshot still work: they are export lichii and import
lichii). --from/--to name the other side's folder instead of where it is looked for: the save folder itself (the one
with account.sav and slot_*), or a folder above it (an emulator's data folder, a portable user folder, NX2navi's game
folder), which is searched for the game's saves. --hookshot DIR does the same for Hookshot's side: its data folder
(Linux: $XDG_DATA_HOME/hookshot, default ~/.local/share/hookshot) or, on Windows, its game folder (the one with
image.bin and user\\), which has to be given there. The side being overwritten is copied first to
<backup>/<import or export>-<name>-<YYYYmmdd-HHMMSS> (Linux: $XDG_DATA_HOME/oot-save-backups, Windows:
%LOCALAPPDATA%\\Hookshot\\oot-save-backups; --backup-dir changes it). Close every game first: --force skips that check.
Standard library only; the guide is docs/saves.md.
"""
import argparse
import collections
import datetime
import os
import re
import shutil
import stat
import subprocess
import sys

TITLE = "0400486024DEC000"
NO_ACCOUNT = "0000000000000000"
WINDOWS = os.name == "nt"

Program = collections.namedtuple("Program", "name display folders processes aliases")

# Everything that runs the game and where it keeps its data, one entry each. folders: per OS, where to look, in order
# ({data} = $XDG_DATA_HOME or ~/.local/share, {appdata} = %APPDATA%, {localappdata} = %LOCALAPPDATA%, {home} = the
# home folder); the saves are then found under it by LAYOUTS. processes: lower case, without .exe, for the
# running-game check. The sources are in docs/saves.md.
EDEN = {"linux": ["{data}/eden"], "windows": ["{appdata}/eden"]}
# (Lichii has a data folder of its own since its build 150, 2026-11-10; a build from before keeps its saves in Eden's)
LICHII = {"linux": ["{data}/lichii"] + EDEN["linux"], "windows": ["{appdata}/lichii"] + EDEN["windows"]}
EMULATORS = [
    Program("lichii", "Lichii", LICHII, ["lichii"], []),
    Program("frizti", "Frizti (Fritchii)", EDEN, ["lichii", "fritchii"], ["fritchii"]),
    Program("orchid", "Orchid", EDEN, ["orchid"], []),
    Program("pokedinner", "PokeDinner", EDEN, ["eglantine"], []),
    Program("eden", "Eden", EDEN, ["eden"], []),
    Program("eglantine", "Eglantine", {"linux": ["{data}/eglantine"], "windows": ["{appdata}/eglantine"]},
            ["eglantine"], []),
    Program("nx2navi", "NX2navi", {"linux": ["{home}/Games/Ocarina"],
                                   "windows": ["{localappdata}/Programs/NX2navi/game", "C:/Games/Ocarina"]},
            ["nx2navi", "ocarina"], []),
]
HOOKSHOT = Program("hookshot", "Hookshot", {"linux": ["{data}/hookshot"], "windows": []}, ["hookshot"], [])
FOLDER = Program("folder", "the folder", {"linux": [], "windows": []}, [], [])

# Where the save folders sit under a data folder. "<id>" is one folder per user profile (its name is the id shown).
# A folder deeper down the same path is accepted too. The first layout that finds a profile wins.
LAYOUTS = (
    ("nand", "user", "save", "account", "<id>", TITLE, "0"),   # Eden's newer layout: used first when it exists
    ("nand", "user", "save", NO_ACCOUNT, "<id>", TITLE),        # Eden and its forks (Lichii, Eglantine, ...)
    ("profile", "save", "<id>"),                                 # NX2navi's game folder
    ("user", "save", "<id>"),                                    # Hookshot's game folder on Windows
    ("save", "<id>"),                                            # Hookshot's data folder
)


class Fail(Exception):
    pass


def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")


def default_backup_dir():
    if WINDOWS:
        base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
        return os.path.join(base, "Hookshot", "oot-save-backups")
    return os.path.join(data_home(), "oot-save-backups")


def folders(program):
    """program's data folders on this OS, in the order they are tried."""
    home = os.path.expanduser("~")
    values = {"data": data_home(), "home": home,
              "appdata": os.environ.get("APPDATA") or os.path.join(home, "AppData", "Roaming"),
              "localappdata": os.environ.get("LOCALAPPDATA") or os.path.join(home, "AppData", "Local")}
    return [os.path.normpath(t.format(**values)) for t in program.folders["windows" if WINDOWS else "linux"]]


def subdirs(path):
    try:
        return sorted(n for n in os.listdir(path) if os.path.isdir(os.path.join(path, n)))
    except OSError:
        return []


def slots(path):
    return [n for n in subdirs(path) if n.startswith("slot_")]


def is_save_folder(path):
    return os.path.isfile(os.path.join(path, "account.sav")) or bool(slots(path))


def walk(root, parts, key):
    if not parts:
        yield key, root
        return
    for n in subdirs(root):
        if parts[0] == "<id>":
            yield from walk(os.path.join(root, n), parts[1:], n)
        elif n.lower() == parts[0].lower():
            yield from walk(os.path.join(root, n), parts[1:], key)


def find_saves(root):
    """The game's save folders under root, as {profile id: path}; root may be the save folder itself."""
    norm = os.path.normpath(os.path.abspath(root))
    title_folder = os.path.basename(norm).upper() == TITLE
    if is_save_folder(norm) or title_folder and "0" not in subdirs(norm):
        key = norm
        while os.path.basename(key).upper() in (TITLE, "0") and os.path.dirname(key) != key:
            key = os.path.dirname(key)
        return {os.path.basename(key) or norm: norm}
    found = {}
    for layout in LAYOUTS:
        idpos = layout.index("<id>")
        for start in range(len(layout)):
            if TITLE in layout and TITLE not in layout[start:] and not title_folder:
                continue   # (below the title id folder, only the title id folder itself is a place to start)
            key = None
            if start > idpos:   # (root is at or below the <id> folder: the id is a folder above it)
                up = norm
                for _ in range(start - idpos - 1):
                    up = os.path.dirname(up)
                key = os.path.basename(up)
            for k, path in walk(norm, layout[start:], key):
                # A folder named by the layout (the title id folder) counts as soon as it is there: the game may not
                # have saved yet. A profile folder counts only when it holds a save (Eden's profile folders do not).
                if (layout[-1] != "<id>" or is_save_folder(path)) and k not in found and path not in found.values():
                    found[k] = path
    return found


def locate(program, override, flag):
    """(folder, {profile id: save folder}) for program: override when given, else the first of its folders with saves."""
    if override is not None:
        if not os.path.isdir(override):
            raise Fail("no folder at %s" % override)
        found = find_saves(override)
        if not found:
            raise Fail("no Ocarina of Time save under %s (a folder with account.sav and slot_*, or %s under an "
                       "emulator's nand\\user\\save): start the game there once and save" % (override, TITLE))
        return override, found
    tried = folders(program)
    for d in tried:
        found = find_saves(d) if os.path.isdir(d) else {}
        if found:
            return d, found
    if program is HOOKSHOT and not tried:
        raise Fail("Hookshot's saves are in the user folder of your game folder on Windows: pass the game folder with "
                   "--hookshot (for example --hookshot C:\\Games\\Hookshot)")
    raise Fail("no %s save found (looked in %s): start the game in %s once and save, or pass %s <folder>"
               % (program.display, ", ".join(tried) or "no default folder", program.display, flag))


def pick(found, name, flag, wanted=None):
    if wanted is not None:
        for k, v in found.items():
            if k.lower() == wanted.lower():
                return k, v
        raise Fail("%s has no profile %s; it has: %s" % (name, wanted, ", ".join(sorted(found))))
    if len(found) > 1:
        lines = "".join("\n  %s  %s" % (k, found[k]) for k in sorted(found))
        raise Fail("%s has Ocarina of Time saves in %d profiles, so pick one with %s:%s" % (name, len(found), flag, lines))
    return next(iter(found.items()))


def pending_commit(save):
    """NX2navi writes a save to profile/save-tx first and moves it into profile/save/<account> after: while something is
    waiting there, that is the save the game loads next, not the account folder."""
    parent = os.path.dirname(os.path.normpath(save))
    tx = os.path.join(os.path.dirname(parent), "save-tx")
    if os.path.basename(parent).lower() == "save" and os.path.isdir(tx):
        for _, _, files in os.walk(tx):
            if files:
                return tx
    return None


def process_names():
    """{process name: what it belongs to} for every program in the tables."""
    names = collections.OrderedDict()
    for program in [HOOKSHOT] + EMULATORS:
        for p in program.processes:
            names[p] = names[p] + " or " + program.display if p in names else program.display
    return names


def running_games():
    """What is running among Hookshot and the emulators in EMULATORS."""
    known = process_names()
    names = set()
    if WINDOWS:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            image = line.split('","')[0].strip('"').lower()
            if image.endswith(".exe") and image[:-4] in known:
                names.add(known[image[:-4]])
        return sorted(names)
    appimage = re.compile(r"^(%s)[-_.].*\.appimage$" % "|".join(map(re.escape, known)))
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                argv0 = f.read().split(b"\0")[0].decode("utf-8", "replace")
            with open("/proc/%s/comm" % pid, encoding="utf-8", errors="replace") as f:
                comm = f.read().strip()
        except OSError:
            continue
        for candidate in (comm, os.path.basename(argv0)):
            low = candidate.lower()
            m = appimage.match(low)
            if low in known:
                names.add(known[low])
            elif m:
                names.add(known[m.group(1)])
    return sorted(names)


def describe(path):
    out = []
    for s in slots(path):
        p = os.path.join(path, s, "progress.sav")
        if os.path.isfile(p):
            when = datetime.datetime.fromtimestamp(os.path.getmtime(p)).strftime("%Y-%m-%d %H:%M")
            out.append("%s (saved %s)" % (s, when))
        else:
            out.append(s)
    return out


def show(title, folder, found):
    print("%s: %s" % (title, folder))
    for k in sorted(found):
        print("  %s  %s" % (k, found[k]))
        for line in describe(found[k]) or ["(no slots)"]:
            print("      " + line)
        if pending_commit(found[k]):
            print("      (a save is still waiting in %s: start and quit NX2navi before copying)" % pending_commit(found[k]))


def list_all(args):
    try:
        show("Hookshot", *locate(HOOKSHOT, args.hookshot, "--hookshot"))
    except Fail as e:
        print("Hookshot: %s" % e)
    # (by the folder import and export use, the first of a program's with saves, as locate: the programs whose saves are there)
    groups = collections.OrderedDict()
    for program in EMULATORS:
        tried = folders(program)
        used = next((d for d in tried if os.path.isdir(d) and find_saves(d)), None)
        groups.setdefault(used or tuple(tried), []).append(program.display)
    for where, displays in groups.items():
        title = " / ".join(displays) + (" (one shared folder)" if len(displays) > 1 else "")
        if isinstance(where, str):
            show(title, where, find_saves(where))
        else:
            print("%s: nothing in %s" % (title, ", ".join(where) or "(no default folder)"))
    if args.folder:
        show("Folder", *locate(FOLDER, args.folder, "--from"))


def refuse_links(path):
    """A save from someone else is only files and folders: a link in it would copy what it points to (any file or folder
    of this PC) into the save"""
    def link(p):
        # (any reparse point on Windows: a junction is not a symbolic link, and os.path.isjunction is only in 3.12 and
        # newer; copytree walks into a junction as into a folder)
        try:
            st = os.lstat(p)
        except OSError:
            return False
        return stat.S_ISLNK(st.st_mode) or bool(getattr(st, "st_file_attributes", 0) & 0x400)
    if link(path):
        raise Fail("%s is a link: a save holds only files and folders, so nothing was copied" % path)
    for root, dirs, files in os.walk(path):
        for n in dirs + files:
            if link(os.path.join(root, n)):
                raise Fail("%s is a link: a save holds only files and folders, so nothing was copied" % os.path.join(root, n))


def copy_saves(src, dst, backup, keep_settings, dry_run):
    target_path = os.path.normcase(os.path.realpath(dst))
    backup_path = os.path.normcase(os.path.realpath(backup))
    try:
        inside_target = os.path.commonpath((target_path, backup_path)) == target_path
    except ValueError:
        inside_target = False  # Windows paths can be on different drives.
    if inside_target:
        raise Fail("choose a --backup-dir outside the target save folder (%s)" % dst)
    names = slots(src)
    if not names:
        raise Fail("%s has no slot_* folders: nothing to copy" % src)
    for s in names:
        refuse_links(os.path.join(src, s))
    for f in ("account.sav", "option.sav"):
        if os.path.lexists(os.path.join(src, f)):
            refuse_links(os.path.join(src, f))
    files = [f for f in ("account.sav", "option.sav") if os.path.isfile(os.path.join(src, f))]
    if keep_settings and "option.sav" in files:
        files.remove("option.sav")
    print("%s %s -> %s" % ("Would back up" if dry_run else "Backing up", dst, backup))
    print("%s %d slot(s) (%s) and %s from %s to %s" % ("Would copy" if dry_run else "Copying", len(names), ", ".join(names),
          ", ".join(files) or "no loose files", src, dst))
    if keep_settings:
        print("option.sav kept as it is on the target")
    if dry_run:
        print("Dry run: nothing was changed")
        return
    os.makedirs(os.path.dirname(backup), exist_ok=True)
    shutil.copytree(dst, backup)
    for s in slots(dst):
        shutil.rmtree(os.path.join(dst, s))
    for s in names:   # (links were refused above; copied as links, never followed, all the same)
        shutil.copytree(os.path.join(src, s), os.path.join(dst, s), symlinks=True)
    for f in files:
        shutil.copy2(os.path.join(src, f), os.path.join(dst, f), follow_symlinks=False)
    print("Done. Backup of the old %s in %s" % (dst, backup))


def program_named(name):
    for program in EMULATORS:
        if name.lower() in [program.name] + program.aliases:
            return program
    raise Fail("unknown emulator %s: use one of %s, or --from/--to <folder>"
               % (name, ", ".join(p.name for p in EMULATORS)))


def run(args):
    if args.command == "list":
        list_all(args)
        return
    other = program_named(args.name) if args.name else FOLDER
    if other is FOLDER and not args.folder:
        raise Fail("%s what? Name the emulator (%s) or the folder (%s <folder>)"
                   % (args.command, ", ".join(p.name for p in EMULATORS), "--from" if args.command == "import" else "--to"))
    flag = "--from" if args.command == "import" else "--to"
    _, hs = locate(HOOKSHOT, args.hookshot, "--hookshot")
    _, em = locate(other, args.folder, flag)
    hs_id, hs_path = pick(hs, "Hookshot", "--hookshot <the user id folder>")
    em_id, em_path = pick(em, other.display, "--profile <id>", args.profile)
    src, dst = (em_path, hs_path) if args.command == "import" else (hs_path, em_path)
    if os.path.realpath(src) == os.path.realpath(dst):
        raise Fail("%s is both the source and the target" % src)
    if not args.force:
        tx = pending_commit(em_path)
        if tx:
            raise Fail("NX2navi still has a save waiting in %s, which it loads instead of %s: start NX2navi and quit "
                       "it from the game, then copy (or pass --force)" % (tx, em_path))
    if not args.dry_run and not args.force:
        busy = running_games()
        if busy:
            raise Fail("close %s first (or pass --force): a running game can write its save again and undo the copy"
                       % " and ".join(busy))
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = base = os.path.join(args.backup_dir or default_backup_dir(), "%s-%s-%s" % (args.command, other.name, stamp))
    n = 1
    while os.path.exists(backup):   # (two copies in the same second)
        n += 1
        backup = "%s-%d" % (base, n)
    copy_saves(src, dst, backup, args.keep_settings, args.dry_run)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], prog="oot_saves.py")
    p.add_argument("command", choices=["list", "import", "export", "to-lichii", "to-hookshot"],
                   help="import: into Hookshot; export: out of Hookshot; to-lichii, to-hookshot: export/import lichii")
    p.add_argument("name", nargs="?", help="the emulator: %s" % ", ".join(e.name for e in EMULATORS))
    p.add_argument("--from", dest="from_dir", metavar="DIR", help="import from this folder (a save, or one to search)")
    p.add_argument("--to", dest="to_dir", metavar="DIR", help="export to this folder (a save, or one to search)")
    p.add_argument("--lichii", metavar="DIR", help=argparse.SUPPRESS)   # (the old name of --from/--to)
    p.add_argument("--hookshot", metavar="DIR", help="Hookshot's data folder (Windows: the game folder)")
    p.add_argument("--profile", metavar="ID", help="the emulator's profile, when several have saves")
    p.add_argument("--backup-dir", metavar="DIR", help="where the backup of the overwritten side goes")
    p.add_argument("--keep-settings", action="store_true", help="leave the target's option.sav (the game's settings) alone")
    p.add_argument("--dry-run", action="store_true", help="say what would happen and change nothing")
    p.add_argument("--force", action="store_true",
                   help="copy even if a game seems to be running, or NX2navi has a save waiting")
    args = p.parse_args(argv)
    if args.command in ("to-lichii", "to-hookshot"):
        if args.name:
            p.error("%s takes no name" % args.command)
        args.command, args.name = ("export" if args.command == "to-lichii" else "import"), "lichii"
    if args.command == "import" and args.to_dir or args.command == "export" and args.from_dir:
        p.error("import reads --from, export writes --to")
    args.folder = args.from_dir or args.to_dir or args.lichii
    try:
        run(args)
    except Fail as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
