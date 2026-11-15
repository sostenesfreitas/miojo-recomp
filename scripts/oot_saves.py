"""Copy Ocarina of Time saves between Hookshot and the emulators and ports that run the game: they all keep the same
files, so a copy is all it takes.

    python -I scripts/oot_saves.py list
    python -I scripts/oot_saves.py import <name>  [--profile ID] [--keep-settings] [--dry-run] [--force]
    python -I scripts/oot_saves.py export <name>  [--profile ID] [--keep-settings] [--dry-run] [--force]
    python -I scripts/oot_saves.py import --from DIR   (a save from any folder into Hookshot)
    python -I scripts/oot_saves.py export --to DIR     (Hookshot's save into any folder)
    python -I scripts/oot_saves.py scrub SRC --slot NN --out assets/saves/<name> [--name FARM]
    python -I scripts/oot_saves.py check-repo-saves [DIR]   (every save in assets/saves, CONTRIBUTING.md)
    --json (any command): one line of JSON on stdout instead of text, for the builders' Saves page

import copies into Hookshot, export copies out of it. <name> is one of EMULATORS below: lichii, frizti, orchid,
pokedinner, eden, eglantine, nx2navi or longshot (Hookshot with add-ons: the same save files) (to-lichii and to-hookshot still work: they are export lichii and import
lichii). --from/--to name the other side's folder instead of where it is looked for: the save folder itself (the one
with account.sav and slot_*), or a folder above it (an emulator's data folder, a portable user folder, NX2navi's game
folder), which is searched for the game's saves. --hookshot DIR does the same for Hookshot's side: its data folder
(Linux: $XDG_DATA_HOME/hookshot, default ~/.local/share/hookshot) or, on Windows, its game folder (the one with
image.bin and user\\), which has to be given there. The side being overwritten is copied first to
<backup>/<import or export>-<name>-<YYYYmmdd-HHMMSS> (Linux: $XDG_DATA_HOME/oot-save-backups, Windows:
%LOCALAPPDATA%\\Hookshot\\oot-save-backups; --backup-dir changes it). Close every game first: --force skips that check.
scrub makes a save in the repository (assets/saves) from one slot: its picture made mid-grey and its saved date set
to 2026-01-01 00:00:00 (there is no player's name in these saves); check-repo-saves checks them all.
assets/saves/README.md has the rules.
Standard library only; the guide is docs/saves.md.
"""
import argparse
import collections
import contextlib
import datetime
import json
import os
import re
import shutil
import stat
import tempfile
import subprocess
import sys

TITLE = "0400486024DEC000"
NO_ACCOUNT = "0000000000000000"
USER_ID = "5a3c9e1f0b7d24680000000000000001"   # (the one user Hookshot plays as: user_uid, host/nn_applet.c)
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
    # (Longshot: Hookshot with add-ons and the same save code, so its saves copy as they are. Windows: its builder's game folder)
    Program("longshot", "Longshot", {"linux": ["{data}/longshot"], "windows": ["{home}/Games/Longshot"]}, ["longshot"], []),
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
    files = [f for f in ("account.sav", "option.sav") if os.path.lexists(os.path.join(src, f))]
    for n in names + files:
        refuse_links(os.path.join(src, n))
    if keep_settings and "option.sav" in files:
        files.remove("option.sav")
    had = os.path.isdir(dst)
    if had:
        print("%s %s -> %s" % ("Would back up" if dry_run else "Backing up", dst, backup))
    else:
        print("%s %s (there is no save there yet)" % ("Would make" if dry_run else "Making", dst))
    print("%s %d slot(s) (%s) and %s from %s to %s" % ("Would copy" if dry_run else "Copying", len(names), ", ".join(names),
          ", ".join(files) or "no loose files", src, dst))
    if keep_settings:
        print("option.sav kept as it is on the target")
    if dry_run:
        print("Dry run: nothing was changed")
        return
    if had:
        os.makedirs(os.path.dirname(backup), exist_ok=True)
        shutil.copytree(dst, backup)
    # The new save is copied whole into a folder on the target's disk first, and the target's slots are swapped for it
    # by renames only after that worked: a copy that fails (a full disk) leaves the target as it was. (Inside the target,
    # so also when it links to another disk, is a disk of its own or sits in a folder that can't be written.)
    staging = dst if had else os.path.dirname(os.path.abspath(dst))
    os.makedirs(staging, exist_ok=True)
    incoming = tempfile.mkdtemp(prefix=".%s-incoming-" % os.path.basename(dst), dir=staging)
    try:
        for s in names:   # (links were refused above; copied as links, never followed, all the same)
            shutil.copytree(os.path.join(src, s), os.path.join(incoming, s), symlinks=True)
        for f in files:
            shutil.copy2(os.path.join(src, f), os.path.join(incoming, f), follow_symlinks=False)
        if not had:
            os.makedirs(dst)
        aside = swap_in(dst, incoming, names + files, slots(dst) + [f for f in files if os.path.lexists(os.path.join(dst, f))],
                        backup if had else None)
    except BaseException:
        if not remove_tree(incoming):
            print("Could not delete %s: delete it by hand" % incoming, file=sys.stderr)
        raise
    left = [p for p in (incoming, aside) if not remove_tree(p)]
    if left:
        # (a leftover folder in a save makes save_problems refuse it as a Hookshot save to import)
        print("Copied, but could not delete %s: delete %s by hand." % (" and ".join(left), "them" if len(left) > 1 else "it"))
    print("Done." + (" Backup of the old %s in %s" % (dst, backup) if had else ""))


def remove_tree(path):
    """Deletes path, also a file in it that is read-only (Windows refuses to delete those). True when it is gone."""
    def writable_then_again(func, name, _):
        os.chmod(name, stat.S_IWRITE)
        func(name)
    try:
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=writable_then_again)
        else:
            shutil.rmtree(path, onerror=writable_then_again)
    except OSError:
        pass
    return not os.path.lexists(path)


def swap_in(dst, incoming, new, old, backup):
    """Moves the old entries of dst aside and the new ones from incoming into dst, all renames on one disk, and returns the
    folder the old ones are in. If one fails the moves are undone, so dst has either all of its old entries or all of the
    new ones; when a move back fails too (a file held open on Windows), the others are still put back and the old entries
    that could not be stay in that folder, which is named in the error."""
    aside = tempfile.mkdtemp(prefix=".%s-old-" % os.path.basename(dst), dir=os.path.dirname(incoming))
    moved, placed = [], []
    try:
        for n in old:
            os.replace(os.path.join(dst, n), os.path.join(aside, n))
            moved.append(n)
        for n in new:
            os.replace(os.path.join(incoming, n), os.path.join(dst, n))
            placed.append(n)
    except BaseException as error:
        new_left, old_left = [], []
        for n in reversed(placed):
            try:
                os.replace(os.path.join(dst, n), os.path.join(incoming, n))
            except OSError:
                new_left.append(n)
        for n in reversed(moved):
            try:
                if os.path.lexists(os.path.join(dst, n)):   # (its new one could not be moved out)
                    raise FileExistsError(n)
                os.replace(os.path.join(aside, n), os.path.join(dst, n))
            except OSError:
                old_left.append(n)
        if not new_left and not old_left:
            os.rmdir(aside)
            raise
        left = []
        if old_left:
            left.append("old ones left in %s: %s" % (aside, ", ".join(sorted(old_left))))
        if new_left:
            left.append("new ones left in the target: %s" % ", ".join(sorted(new_left)))
        raise Fail("copying into %s failed (%s), and so did putting everything back (%s). %s"
                   % (dst, error, "; ".join(left),
                      "Close whatever holds those files, then put the old save back from the backup, %s" % backup if backup
                      else "There was no save there before")) from error
    return aside



# ---- Saves in the repository (assets/saves, CONTRIBUTING.md): scrub and check-repo-saves ----
# A repo save is one slot of a save made fresh for the Test Farm, scrubbed: its file-select
# picture replaced by a mid-grey one of the same size and its saved date set to 2026-01-01 00:00:00.
# assets/saves/<name>/{README.md, slot_00/progress.sav, slot_00/caption.sav} and nothing else: no account.sav or
# option.sav, here or in a shared folder. The game makes both when it loads a slot found alone in an empty data folder
# (checked on 2026-11-14 with a real game), and a shipped one could carry a player's settings.

REPO_SAVES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets", "saves")
REPO_SAVE_NAME = re.compile(r"[a-z0-9][a-z0-9-]{0,47}")
MAX_SAVE_FILE = 4 << 20   # (a save file is a few kilobytes: anything larger is not one)
SLOT = re.compile(r"slot_\d{2,3}$")
PROFILE_DIR = "_profile"   # (an old layout's shared account.sav and option.sav: refused)
GAME_MADE = ("account.sav", "option.sav")
# (what each file may weigh in the repository: a few times what the game writes, so a save that is something else, or
# whose picture is a real one (about 38 KB), does not pass)
REPO_CAPS = {"progress.sav": 512 << 10, "caption.sav": 16 << 10, "README.md": 16 << 10}

# caption.sav, as host/control.c's caption_json reads it and as every caption.sav looked at so far (Hookshot's and
# Hookshot's, 12 files) has it: the magic 04 03 02 01; the scene's and the map's names at 684 and 748 (up to 64
# printable characters, NUL-ended); a 32-bit little-endian 1392 at 540 (the offset of the field that follows, it seems:
# the same in every file, so it is checked and kept); at 1392 the JPEG's length, 32-bit little-endian; the JPEG at 1396,
# up to the end of the file (nothing after it). No other header field holds the JPEG's length or the file's size. A file
# laid out any other way is refused, not guessed at.
CAPTION_MAGIC = b"\x04\x03\x02\x01"
CAPTION_SCENE, CAPTION_MAP, CAPTION_NAME_LEN = 684, 748, 64
CAPTION_CHECK = 540         # (holds 1392)
CAPTION_JPEG_LEN = 1392
CAPTION_JPEG = 1396

# The player's name: this game's saves show none. Its Load screen shows each slot's picture, the date and time it was
# saved, the place and the hearts, and no name (checked on 2026-11-14 with a real save, on the AMD lab PC). So there is
# no name field to set; --name-field stays for a field found later. Each entry: (file, offset, length, encoding).
KNOWN_NAME_FIELDS = ()
# The date and time the Load screen shows come from caption.sav's header, not from the files' modification times
# (checked on 2026-11-14 with a real game). Part of the header is a list of 8-byte entries, a 32-bit little-endian key
# (a hash) and a 32-bit little-endian value; these six are the date (none of them occurs in progress.sav). They are
# found by key, not by offset: each exactly once before CAPTION_JPEG_LEN, all on the same 8-byte grid. (0x1e55ecc5 looks
# like the current health in quarter hearts: kept.)
CAPTION_DATE_KEYS = (("year", 0x9811a3f7), ("month", 0xdfd840d3), ("day", 0xbd46f485), ("hour", 0x23f3d75e),
                     ("minute", 0x27853bf7), ("second", 0x5ee50bed))
SCRUB_DATE = {"year": 2026, "month": 1, "day": 1, "hour": 0, "minute": 0, "second": 0}
# The files' modification times are set to the same fixed moment, so the files say nothing about when they were made.
SCRUB_TIME = 1767225600   # 2026-01-01 00:00:00 UTC
NAME_ENCODINGS = {"ascii": "ascii", "utf16le": "utf-16-le"}


def u32le(data, offset):
    return int.from_bytes(data[offset:offset + 4], "little")


def caption_text(data, offset):
    raw = data[offset:offset + CAPTION_NAME_LEN].split(b"\0", 1)[0]
    if any(c < 0x20 or c > 0x7e for c in raw):
        raise Fail("not the caption.sav layout understood: no name at %d" % offset)
    return raw.decode("ascii")


def caption_layout(data):
    """caption.sav's parts, as {scene, map, jpeg, width, height}; Fail when the file is not laid out exactly as above."""
    if len(data) < CAPTION_JPEG + 4 or data[:4] != CAPTION_MAGIC:
        raise Fail("not the caption.sav layout understood: no 04 03 02 01 magic")
    if u32le(data, CAPTION_CHECK) != CAPTION_JPEG_LEN:
        raise Fail("not the caption.sav layout understood: %d at %d, not %d" % (u32le(data, CAPTION_CHECK), CAPTION_CHECK,
                                                                               CAPTION_JPEG_LEN))
    size = u32le(data, CAPTION_JPEG_LEN)
    if CAPTION_JPEG + size != len(data):
        raise Fail("not the caption.sav layout understood: the JPEG's length at %d (%d) does not reach the end of the file "
                   "(%d bytes after %d)" % (CAPTION_JPEG_LEN, size, len(data) - CAPTION_JPEG, CAPTION_JPEG))
    jpeg = data[CAPTION_JPEG:]
    if jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
        raise Fail("not the caption.sav layout understood: no JPEG at %d" % CAPTION_JPEG)
    width, height = jpeg_size(jpeg)
    return {"scene": caption_text(data, CAPTION_SCENE), "map": caption_text(data, CAPTION_MAP), "jpeg": jpeg,
            "width": width, "height": height}


def caption_dates(data):
    """{field: (entry offset, value)} of the six date entries of caption.sav's header; Fail when one is missing, is
    there more than once, or is not on the same 8-byte grid as the others."""
    header = bytes(data[:CAPTION_JPEG_LEN])
    found = {}
    for field, key in CAPTION_DATE_KEYS:
        raw, at, offsets = key.to_bytes(4, "little"), 0, []
        while True:
            at = header.find(raw, at)
            if at < 0:
                break
            offsets.append(at)
            at += 1
        if len(offsets) != 1 or offsets[0] + 8 > len(header):
            raise Fail("not the caption.sav layout understood: the date's %s key 0x%08x is %s in the header (before %d)"
                       % (field, key, "%d times" % len(offsets) if len(offsets) > 1 else "not whole" if offsets else "not",
                      CAPTION_JPEG_LEN))
        found[field] = (offsets[0], u32le(header, offsets[0] + 4))
    grid = {o % 8 for o, _ in found.values()}
    if len(grid) != 1:
        raise Fail("not the caption.sav layout understood: the date's entries are not 8-byte aligned with each other (%s)"
                   % ", ".join("%s at %d" % (f, o) for f, (o, _) in found.items()))
    return found


def set_caption_date(data):
    """caption.sav (a bytearray) with its six date fields set to SCRUB_DATE. Returns what it changed, as lines."""
    changes = []
    for field, (offset, old) in caption_dates(data).items():
        data[offset + 4:offset + 8] = SCRUB_DATE[field].to_bytes(4, "little")
        changes.append("caption.sav: the save date (%s at %d): %d -> %d" % (field, offset, old, SCRUB_DATE[field]))
    return changes


def caption_date_problem(data):
    """None when caption.sav's date reads 2026-01-01 00:00:00, else why not"""
    try:
        dates = caption_dates(data)
    except Fail as e:
        return str(e)
    wrong = ["%s %d" % (f, v) for f, (_, v) in dates.items() if v != SCRUB_DATE[f]]
    return "the save date is not 2026-01-01 00:00:00 (%s): not scrubbed" % ", ".join(wrong) if wrong else None


# -- JPEG, standard library only: the size of any baseline or progressive JPEG, a flat grey baseline one written, and
# a baseline one's coefficients read back to tell that every block is flat (no picture in it) --

def jpeg_segments(jpeg):
    """(marker, payload) up to and including SOS (whose payload is followed by the scan, not returned)."""
    if jpeg[:2] != b"\xff\xd8":
        raise Fail("not a JPEG")
    i = 2
    while True:
        if i + 4 > len(jpeg) or jpeg[i] != 0xFF:
            raise Fail("not a JPEG: no marker at %d" % i)
        marker = jpeg[i + 1]
        if marker == 0xFF:   # (fill bytes)
            i += 1
            continue
        length = int.from_bytes(jpeg[i + 2:i + 4], "big")
        if length < 2 or i + 2 + length > len(jpeg):
            raise Fail("not a JPEG: a segment runs past the end")
        yield marker, jpeg[i + 4:i + 2 + length], i + 2 + length
        if marker == 0xDA:
            return
        i += 2 + length


def jpeg_size(jpeg):
    """(width, height) from the frame header (SOF0, SOF1 or SOF2)."""
    for marker, payload, _ in jpeg_segments(jpeg):
        if marker in (0xC0, 0xC1, 0xC2):
            if len(payload) < 6:
                break
            height, width = int.from_bytes(payload[1:3], "big"), int.from_bytes(payload[3:5], "big")
            if not width or not height:
                break
            return width, height
    raise Fail("not a JPEG this understands: no frame size")


# ITU T.81 Annex K: the example quantization tables (in zigzag order here: a flat picture uses only the first) and the
# standard Huffman tables (code counts by length 1..16, then the values)
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
          28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
          54, 47, 55, 62, 63]
QUANT_LUMA = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22,
              29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103,
              121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
QUANT_CHROMA = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99,
                99, 99, 99, 99, 99] + [99] * 32
DC_LUMA = ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12)))
DC_CHROMA = ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12)))
AC_LUMA = ([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d], bytes.fromhex(
    "01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a"
    "434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9"
    "aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa"))
AC_CHROMA = ([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77], bytes.fromhex(
    "000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a3536373839"
    "3a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7"
    "a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa"))


def huffman_codes(counts, values):
    """{value: (code, length)} from a table's counts and values (T.81 Annex C)."""
    codes, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            codes[values[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


def segment(marker, payload):
    return bytes((0xFF, marker)) + (len(payload) + 2).to_bytes(2, "big") + payload


def grey_jpeg(width, height, level=128):
    """A baseline JPEG of width x height, every pixel grey `level` (0..255): YCbCr 4:2:0 as the game writes its pictures,
    the Annex K tables, each block one DC value and no AC (the first block of each component carries the value, the rest
    a difference of 0)."""
    if not (0 < width < 65536 and 0 < height < 65536 and 0 <= level <= 255):
        raise Fail("grey_jpeg: a size of 1..65535 and a level of 0..255")
    out = bytearray(b"\xff\xd8")
    out += segment(0xE0, b"JFIF\0\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    out += segment(0xDB, bytes([0]) + bytes(QUANT_LUMA[z] for z in ZIGZAG))
    out += segment(0xDB, bytes([1]) + bytes(QUANT_CHROMA[z] for z in ZIGZAG))
    out += segment(0xC0, bytes([8]) + height.to_bytes(2, "big") + width.to_bytes(2, "big") +
                   bytes([3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1]))
    for cls_id, (counts, values) in ((0x00, DC_LUMA), (0x10, AC_LUMA), (0x01, DC_CHROMA), (0x11, AC_CHROMA)):
        out += segment(0xC4, bytes([cls_id] + counts) + bytes(values))
    out += segment(0xDA, bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0]))
    dc = [huffman_codes(*DC_LUMA), huffman_codes(*DC_CHROMA)]
    eob = [huffman_codes(*AC_LUMA)[0], huffman_codes(*AC_CHROMA)[0]]
    luma_dc = round((level - 128) * 8 / QUANT_LUMA[0])   # (the DC of a flat block: 8 times its level-shifted value)
    bits, nbits = [0], [0]

    def put(code, length):
        bits[0] = bits[0] << length | code
        nbits[0] += length
        while nbits[0] >= 8:
            nbits[0] -= 8
            byte = bits[0] >> nbits[0] & 0xFF
            out.append(byte)
            if byte == 0xFF:
                out.append(0)   # (byte stuffing)
        bits[0] &= (1 << nbits[0]) - 1

    def block(table, diff):
        size = abs(diff).bit_length()
        put(*dc[table][size])
        if size:
            put(diff if diff > 0 else diff + (1 << size) - 1, size)
        put(*eob[table])

    first = True
    for _ in range(-(-height // 16) * -(-width // 16)):
        for _ in range(4):
            block(0, luma_dc if first else 0)
            first = False
        block(1, 0)
        block(1, 0)
    if nbits[0]:
        put((1 << (8 - nbits[0])) - 1, 8 - nbits[0])   # (padded with ones)
    out += b"\xff\xd9"
    return bytes(out)


def jpeg_blocks(jpeg):
    """A baseline JPEG's scan read to its coefficients (no picture is made): {component id: (DC values, whether any
    block has an AC coefficient)}. Fail on anything else: progressive, restart intervals, several scans, a scan that
    is short or has data after it."""
    tables, frame, scan, end = {}, None, None, None
    for marker, payload, after in jpeg_segments(jpeg):
        if marker == 0xC4:
            p = 0
            while p < len(payload):
                counts = list(payload[p + 1:p + 17])
                n = sum(counts)
                if len(counts) < 16 or p + 17 + n > len(payload):
                    raise Fail("JPEG: a Huffman table runs past its segment")
                codes = huffman_codes(counts, payload[p + 17:p + 17 + n])
                tables[payload[p]] = {(length, code): v for v, (code, length) in codes.items()}
                p += 17 + n
        elif marker == 0xC0:
            if len(payload) < 6 or payload[0] != 8 or len(payload) != 6 + 3 * payload[5]:
                raise Fail("JPEG: a frame header this does not read")
            height, width = int.from_bytes(payload[1:3], "big"), int.from_bytes(payload[3:5], "big")
            frame = (width, height, [(payload[6 + 3 * k], payload[7 + 3 * k] >> 4, payload[7 + 3 * k] & 15)
                                     for k in range(payload[5])])
        elif marker in (0xC1, 0xC2, 0xC3) or 0xC5 <= marker <= 0xCF and marker != 0xC8:
            raise Fail("JPEG: not baseline")
        elif marker == 0xDD and int.from_bytes(payload[:2], "big"):
            raise Fail("JPEG: restart intervals are not read here")
        elif marker == 0xDA:
            n = payload[0]
            scan = [(payload[1 + 2 * k], payload[2 + 2 * k] >> 4, payload[2 + 2 * k] & 15) for k in range(n)]
            end = after
    if frame is None or scan is None:
        raise Fail("JPEG: no baseline frame or scan")
    width, height, comps = frame
    if [c[0] for c in comps] != [s[0] for s in scan]:
        raise Fail("JPEG: the scan does not hold every component (several scans)")
    hmax, vmax = max(c[1] for c in comps), max(c[2] for c in comps)
    if any(c[1] not in (1, 2) or c[2] not in (1, 2) for c in comps):
        raise Fail("JPEG: a sampling factor this does not read")
    if len(comps) == 1:
        mcus, layout = -(-width // 8) * -(-height // 8), [(comps[0][0], 1)]
    else:
        mcus, layout = -(-width // (8 * hmax)) * -(-height // (8 * vmax)), [(c[0], c[1] * c[2]) for c in comps]
    dc_tab = {s[0]: tables.get(s[1]) for s in scan}
    ac_tab = {s[0]: tables.get(0x10 | s[2]) for s in scan}
    if None in dc_tab.values() or None in ac_tab.values():
        raise Fail("JPEG: a Huffman table the scan uses is missing")
    pos, acc, have = [end], [0], [0]

    def bit():
        if not have[0]:
            if pos[0] >= len(jpeg):
                raise Fail("JPEG: the scan ends early")
            byte = jpeg[pos[0]]
            if byte == 0xFF:
                if pos[0] + 1 >= len(jpeg) or jpeg[pos[0] + 1] != 0:
                    raise Fail("JPEG: the scan ends early")
                pos[0] += 1
            pos[0] += 1
            acc[0], have[0] = byte, 8
        have[0] -= 1
        return acc[0] >> have[0] & 1

    def decode(table):
        code = 0
        for length in range(1, 17):
            code = code << 1 | bit()
            if (length, code) in table:
                return table[(length, code)]
        raise Fail("JPEG: not a Huffman code")

    def receive(size):
        v = 0
        for _ in range(size):
            v = v << 1 | bit()
        return v if v >= 1 << size - 1 else v - (1 << size) + 1

    out = {c: ([], False) for c, _ in layout}
    pred = {c: 0 for c, _ in layout}
    for _ in range(mcus):
        for c, count in layout:
            for _ in range(count):
                size = decode(dc_tab[c])
                if size > 11:
                    raise Fail("JPEG: a DC difference out of range")
                pred[c] += receive(size) if size else 0
                out[c][0].append(pred[c])
                k, ac = 1, False
                while k < 64:
                    rs = decode(ac_tab[c])
                    run, size = rs >> 4, rs & 15
                    if not size:
                        if run != 15:
                            break
                        k += 16
                        continue
                    k += run
                    if k > 63:
                        raise Fail("JPEG: an AC coefficient past the block")
                    if receive(size):
                        ac = True
                    k += 1
                if ac:
                    out[c] = (out[c][0], True)
    rest = jpeg[pos[0]:]
    if rest != b"\xff\xd9":
        raise Fail("JPEG: %d byte(s) after the scan, not just its end" % (len(rest) - 2 if rest.endswith(b"\xff\xd9")
                                                                          else len(rest)))
    return out


def jpeg_is_mid_grey(jpeg):
    """None when jpeg is a flat mid-grey picture (every coefficient of every block 0), else why not."""
    try:
        blocks = jpeg_blocks(jpeg)
    except Fail as e:
        return str(e)
    for c, (dcs, ac) in sorted(blocks.items()):
        if ac or any(dcs):
            return "the picture is not flat mid-grey (component %d has %s)" % (c, "detail" if ac else "another shade")
    return None


# -- the player's name --

def parse_name_field(text):
    """progress.sav:OFFSET:LENGTH[:ENCODING] (OFFSET decimal or 0x...) -> (file, offset, length, encoding)"""
    parts = text.split(":")
    try:
        if len(parts) not in (3, 4) or parts[0] not in ("progress.sav", "caption.sav"):
            raise ValueError
        offset, length = int(parts[1], 0), int(parts[2], 0)
        encoding = parts[3] if len(parts) == 4 else "ascii"
        if offset < 0 or not 0 < length <= 64 or encoding not in NAME_ENCODINGS:
            raise ValueError
    except ValueError:
        raise Fail("--name-field FILE:OFFSET:LENGTH[:ENCODING]: FILE progress.sav or caption.sav, LENGTH 1..64, ENCODING "
                   "%s" % " or ".join(NAME_ENCODINGS))
    return parts[0], offset, length, encoding


def set_player_name(files, name, fields):
    """The name step: each field of `fields` in files ({file name: bytearray}) set to name, NUL-padded. Returns what it
    changed, as lines. Refuses when no field is known: the name is not where anyone has looked."""
    if not fields:
        return ["no player name to set: this game's saves show none (the Load screen: picture, date, place, hearts)"]
    changes = []
    for file, offset, length, encoding in fields:
        new = name.encode(NAME_ENCODINGS[encoding])
        if len(new) > length:
            raise Fail("the name %s does not fit the %d-byte field at %s:%d" % (name, length, file, offset))
        data = files[file]
        if offset + length > len(data):
            raise Fail("the name field %s:%d+%d is past the end of %s (%d bytes)" % (file, offset, length, file, len(data)))
        new = new.ljust(length, b"\0")
        old = bytes(data[offset:offset + length])
        data[offset:offset + length] = new
        changes.append("%s: player name at %d (%d bytes, %s): %s -> %s (%s)" % (
            file, offset, length, encoding, old.hex(), new.hex(), name) if old != new else
            "%s: player name at %d already %s" % (file, offset, name))
    return changes


# -- scrub and check-repo-saves --

def lstat_plain(path):
    """'file', 'dir', or None for anything else (a link, a junction, a device)"""
    st = os.lstat(path)
    if is_link(path):
        return None
    return "dir" if stat.S_ISDIR(st.st_mode) else "file" if stat.S_ISREG(st.st_mode) else None


def is_link(p):
    # (any reparse point on Windows: a junction is not a symbolic link, and os.path.isjunction is only in 3.12 and newer;
    # copytree walks into a junction as into a folder)
    try:
        st = os.lstat(p)
    except OSError:
        return False
    return stat.S_ISLNK(st.st_mode) or bool(getattr(st, "st_file_attributes", 0) & 0x400)


def read_capped(path, cap, name):
    if lstat_plain(path) != "file":
        raise Fail("%s is not a plain file" % path)
    size = os.path.getsize(path)
    if not 0 < size <= cap:
        raise Fail("%s is %s" % (name, "empty" if not size else "%d bytes, over the %d a repo save allows" % (size, cap)))
    with open(path, "rb") as f:
        return f.read()


def scrub(src, slot, out, name="FARM", name_fields=None):
    """One slot of the save folder src, scrubbed, as the repo save `out`: slot_00/progress.sav (as it is: there is no
    player's name to set, see set_player_name) and slot_00/caption.sav with its date set to 2026-01-01 00:00:00 and a
    mid-grey picture of the same size. Nothing is written when any step is refused. Prints every change."""
    if not SLOT.match(slot):
        slot = "slot_%02d" % int(slot) if slot.isdigit() and len(slot) <= 3 else slot
    if not SLOT.match(slot):
        raise Fail("--slot: a slot number, 03 or slot_03")
    if not os.path.isdir(src):
        raise Fail("no save folder at %s" % src)
    folder = os.path.join(src, slot)
    if not os.path.isdir(folder):
        raise Fail("%s has no %s (it has %s)" % (src, slot, ", ".join(slots(src)) or "no slots"))
    refuse_links(folder)
    base = os.path.basename(os.path.normpath(out))
    if base == PROFILE_DIR or not REPO_SAVE_NAME.fullmatch(base):
        raise Fail("--out: a repo save's folder is named with a-z, 0-9 and -, up to 48 (not %s)" % base)
    if os.path.lexists(out):
        if lstat_plain(out) != "dir" or set(os.listdir(out)) - {"README.md"}:
            raise Fail("--out %s already holds a save: give a new folder (or one with only its README.md)" % out)
    extra = sorted(set(os.listdir(folder)) - {"progress.sav", "caption.sav"})
    if extra:
        raise Fail("%s holds %s besides progress.sav and caption.sav" % (folder, ", ".join(extra)))
    files = {}
    for f in ("progress.sav", "caption.sav"):
        p = os.path.join(folder, f)
        if not os.path.isfile(p):
            raise Fail("%s has no %s" % (folder, f))
        files[f] = bytearray(read_capped(p, MAX_SAVE_FILE, f))
    sizes = {f: len(d) for f, d in files.items()}
    changes = []
    cap = caption_layout(bytes(files["caption.sav"]))
    fields = list(name_fields) if name_fields else list(KNOWN_NAME_FIELDS)
    changes += set_player_name(files, name, fields)
    changes += set_caption_date(files["caption.sav"])
    cap = caption_layout(bytes(files["caption.sav"]))   # (again: a name field in caption.sav must not have broken it)
    if cap["width"] > 2048 or cap["height"] > 2048:   # (the game's are 543x306)
        raise Fail("caption.sav's picture is %dx%d: larger than a file-select picture, refused" % (cap["width"], cap["height"]))
    grey = grey_jpeg(cap["width"], cap["height"])
    old = bytes(files["caption.sav"])
    files["caption.sav"] = bytearray(old[:CAPTION_JPEG_LEN] + len(grey).to_bytes(4, "little") + grey)
    if (caption_layout(bytes(files["caption.sav"]))["jpeg"] != grey or jpeg_is_mid_grey(grey) or
            caption_date_problem(bytes(files["caption.sav"]))):
        raise Fail("the scrubbed caption.sav did not read back: nothing was written")
    changes.append("caption.sav: the picture at %d (JPEG %dx%d, %d bytes) replaced by a mid-grey JPEG %dx%d of %d bytes"
                   % (CAPTION_JPEG, cap["width"], cap["height"], len(cap["jpeg"]), cap["width"], cap["height"], len(grey)))
    changes.append("caption.sav: the JPEG's length at %d: %d -> %d" % (CAPTION_JPEG_LEN, len(cap["jpeg"]), len(grey)))
    changes.append("caption.sav: %d -> %d bytes; the rest of its header kept as it was (scene %s, map %s)"
                   % (sizes["caption.sav"], len(files["caption.sav"]), cap["scene"] or "-", cap["map"] or "-"))
    for f in ("progress.sav", "caption.sav"):
        if len(files[f]) > REPO_CAPS[f]:
            raise Fail("the scrubbed %s is %d bytes, over the %d a repo save allows: nothing was written"
                       % (f, len(files[f]), REPO_CAPS[f]))
    os.makedirs(os.path.join(out, "slot_00"))
    for f, data in files.items():
        path = os.path.join(out, "slot_00", f)
        with open(path, "xb") as fh:
            fh.write(data)
        os.utime(path, (SCRUB_TIME, SCRUB_TIME))
    changes.append("progress.sav, caption.sav: modification times set to 2026-01-01 00:00 UTC (the files' own; the "
                   "Load screen's date is caption.sav's, above)")
    readme = os.path.join(out, "README.md")
    if not os.path.exists(readme):
        with open(readme, "x", encoding="utf-8", newline="\n") as fh:
            fh.write("%s: <where the save stands, in one line>\n\nMade fresh for the Test Farm from %s of a new profile; "
                     "scrubbed with oot_saves.py scrub.\n" % (base, slot))
        changes.append("README.md: written from the template: fill in its first line")
    print("Scrubbed %s/%s into %s/slot_00:" % (src, slot, out))
    for line in changes:
        print("  " + line)
    if sizes["progress.sav"] == len(files["progress.sav"]) and not any(c.startswith("progress.sav:") for c in changes):
        print("  progress.sav: copied unchanged")
    print("Check it with: python -I scripts/oot_saves.py check-repo-saves")


def repo_save_problems(root):
    """What keeps the folder root (assets/saves) from passing: the layout, no links, no other file (no account.sav or
    option.sav anywhere, no _profile), the sizes, the pictures grey, the dates reset and every save's README. An empty
    list when it passes."""
    problems = []
    if not os.path.isdir(root) or is_link(root):
        return ["%s is not a folder" % root]
    for folder, dirs, files in os.walk(root):   # (links are not followed)
        for n in sorted(dirs + files):
            if n in GAME_MADE:
                problems.append("%s: %s is made by the game and never shipped (it could carry a player's settings)"
                                % (os.path.relpath(os.path.join(folder, n), root), n))
    names = sorted(os.listdir(root))
    saves = []
    for n in names:
        p = os.path.join(root, n)
        kind = lstat_plain(p)
        if kind is None:
            problems.append("%s is a link or not a plain file or folder" % n)
        elif n == "README.md" and kind == "file" or n == ".gitkeep" and kind == "file" and not os.path.getsize(p):
            continue
        elif n == PROFILE_DIR:
            problems.append("%s/ is not shipped any more: account.sav and option.sav are made by the game" % n)
        elif kind == "dir" and REPO_SAVE_NAME.fullmatch(n):
            saves.append(n)
        else:
            problems.append("%s does not belong in %s" % (n, root))
    if "README.md" not in names:
        problems.append("README.md is missing")

    def expect(folder, label, wanted):
        """folder holds exactly `wanted` ({name: 'file' or 'dir'}); the files there within their caps"""
        have = sorted(os.listdir(folder))
        for n in have:
            p = os.path.join(folder, n)
            kind = lstat_plain(p)
            if kind is None:
                problems.append("%s/%s is a link or not a plain file or folder" % (label, n))
            elif wanted.get(n) != kind:
                problems.append("%s/%s does not belong in a repo save" % (label, n))
        for n, kind in wanted.items():
            if n not in have:
                problems.append("%s/%s is missing" % (label, n))
        return {n: os.path.join(folder, n) for n in have if wanted.get(n) == lstat_plain(os.path.join(folder, n))}

    def content(path, label, name):
        try:
            return read_capped(path, REPO_CAPS[name], label)
        except Fail as e:
            problems.append(str(e))
            return None

    for s in saves:
        found = expect(os.path.join(root, s), s, {"README.md": "file", "slot_00": "dir"})
        if "README.md" in found:
            text = content(found["README.md"], "%s/README.md" % s, "README.md")
            try:
                first = text.decode("utf-8").splitlines()[0].strip() if text else ""
            except (UnicodeDecodeError, IndexError):
                first = ""
            if text is not None and not first:
                problems.append("%s/README.md has no first line (what the save is)" % s)
            elif "<" in first and ">" in first:
                problems.append("%s/README.md's first line is still the template's" % s)
        if "slot_00" in found:
            files = expect(found["slot_00"], s + "/slot_00", {"progress.sav": "file", "caption.sav": "file"})
            if "progress.sav" in files:
                content(files["progress.sav"], "%s/slot_00/progress.sav" % s, "progress.sav")
            if "caption.sav" in files:
                data = content(files["caption.sav"], "%s/slot_00/caption.sav" % s, "caption.sav")
                if data is not None:
                    try:
                        why = jpeg_is_mid_grey(caption_layout(data)["jpeg"])
                    except Fail as e:
                        why = str(e)
                    for w in (why, caption_date_problem(data)):
                        if w:
                            problems.append("%s/slot_00/caption.sav: %s" % (s, w))
    return problems


def check_repo_saves(root):
    problems = repo_save_problems(root)
    if problems:
        print("%s does not pass:" % root)
        for p in problems:
            print("  " + p)
        return 1
    n = sum(1 for e in os.listdir(root) if os.path.isdir(os.path.join(root, e)))
    print("%s passes: %d repo save(s)" % (root, n))
    return 0


def repo_main(argv):
    p = argparse.ArgumentParser(prog="oot_saves.py", description="Saves in the repository (assets/saves): make one with "
                                "scrub, check them all with check-repo-saves")
    sub = p.add_subparsers(dest="command", required=True)
    q = sub.add_parser("scrub", help="one slot of a save folder, scrubbed, as a repo save")
    q.add_argument("src", help="the save folder (the one with account.sav and slot_*)")
    q.add_argument("--slot", required=True, help="the slot to take (03 or slot_03); it becomes slot_00")
    q.add_argument("--out", required=True, help="the new repo save's folder, assets/saves/<name>")
    q.add_argument("--name", default="FARM", help="the player's name the save must have (default FARM)")
    q.add_argument("--name-field", action="append", default=[], metavar="FILE:OFFSET:LENGTH[:ENCODING]",
                   help="where the player's name is, until it is known (ENCODING ascii or utf16le)")
    q = sub.add_parser("check-repo-saves", help="check every repo save: layout, links, extra files (no account.sav, "
                       "option.sav or _profile), sizes, grey pictures, dates reset")
    q.add_argument("root", nargs="?", default=REPO_SAVES, help="default: this checkout's assets/saves")
    args = p.parse_args(argv)
    try:
        if args.command == "scrub":
            if not re.fullmatch(r"[A-Za-z0-9]{1,8}", args.name):
                raise Fail("--name: up to 8 letters and digits")
            scrub(args.src, args.slot, args.out, args.name, [parse_name_field(f) for f in args.name_field])
            return 0
        return check_repo_saves(args.root)
    except Fail as e:
        print("error: %s" % e, file=sys.stderr)
        return 1

def program_named(name):
    for program in EMULATORS:
        if name.lower() in [program.name] + program.aliases:
            return program
    raise Fail("unknown emulator %s: use one of %s, or --from/--to <folder>"
               % (name, ", ".join(p.name for p in EMULATORS)))


def save_info(path):
    out = []
    for s in slots(path):
        p = os.path.join(path, s, "progress.sav")
        when = datetime.datetime.fromtimestamp(os.path.getmtime(p)).isoformat(timespec="seconds") if os.path.isfile(p) else None
        out.append({"slot": s, "saved": when})
    return out


def list_json(args):
    """list as one JSON object: Hookshot's save, and every other program's folder with saves (programs that share a
    folder are one entry, named by the first of them, as import takes it)"""
    out = {"sources": [], "running": running_games()}
    try:
        folder, found = locate(HOOKSHOT, args.hookshot, "--hookshot")
        out["hookshot"] = {"folder": folder, "saves": [{"profile": k, "path": v, "slots": save_info(v)}
                                                       for k, v in sorted(found.items())]}
    except Fail as e:
        out["hookshot"] = {"error": str(e)}
    groups = collections.OrderedDict()
    for program in EMULATORS:
        used = next((d for d in folders(program) if os.path.isdir(d) and find_saves(d)), None)
        if used:
            groups.setdefault(used, []).append(program)
    for where, programs in groups.items():
        saves = [{"profile": k, "path": v, "slots": save_info(v), "pending": pending_commit(v)}
                 for k, v in sorted(find_saves(where).items()) if slots(v)]
        if saves:
            out["sources"].append({"name": programs[0].name, "display": " / ".join(p.display for p in programs),
                                   "folder": where, "saves": saves})
    print(json.dumps(out))


def hookshot_target(args):
    """Hookshot's save folders for an import; when it has none yet, its save folder made (where the game makes it)"""
    try:
        return locate(HOOKSHOT, args.hookshot, "--hookshot")[1]
    except Fail:
        root = args.hookshot or (None if WINDOWS else folders(HOOKSHOT)[0])
        if not root:
            raise
        if WINDOWS and not os.path.isdir(root):
            raise Fail("no game folder at %s: build the game there first" % root)
        user = WINDOWS or os.path.isdir(os.path.join(root, "user"))
        save = os.path.join(root, *(["user"] if user else []), "save", USER_ID)
        if not args.dry_run:
            os.makedirs(save, exist_ok=True)
        print("Hookshot has no save yet: made %s" % save, file=sys.stderr if args.json else sys.stdout)
        return {USER_ID: save}


def run(args):
    if args.command == "list":
        list_json(args) if args.json else list_all(args)
        return
    other = program_named(args.name) if args.name else FOLDER
    if other is FOLDER and not args.folder:
        raise Fail("%s what? Name the emulator (%s) or the folder (%s <folder>)"
                   % (args.command, ", ".join(p.name for p in EMULATORS), "--from" if args.command == "import" else "--to"))
    flag = "--from" if args.command == "import" else "--to"
    hs = hookshot_target(args) if args.command == "import" else locate(HOOKSHOT, args.hookshot, "--hookshot")[1]
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
    if args.json:
        with contextlib.redirect_stdout(sys.stderr):   # (the text goes to stderr; stdout holds only the answer)
            copy_saves(src, dst, backup, args.keep_settings, args.dry_run)
        print(json.dumps({"ok": True, "from": src, "to": dst, "backup": None if args.dry_run else backup,
                          "dry_run": args.dry_run, "slots": slots(src)}))
    else:
        copy_saves(src, dst, backup, args.keep_settings, args.dry_run)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv and argv[0] in ("scrub", "check-repo-saves"):
        return repo_main(argv)
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
    p.add_argument("--json", action="store_true", help="answer with one line of JSON (the builders' Saves page)")
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
        if args.json:
            print(json.dumps({"error": str(e)}))
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
