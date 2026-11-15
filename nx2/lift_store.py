"""Translations kept for every work folder on this PC (HOOKSHOT_LIFT_STORE=<folder>; off when unset, empty or 0).

The lift (nx2/lift.py and split_hot.py, make_game.py translate) writes the same C for the same game, translator, hot list,
chunk boundaries, kept hot pieces, settings and Python, whatever the work folder: make_game.py lift_key covers all of that,
but also the work and game folders' paths (the lift's environment). make_game.py lift_share_key is that key with the two
folders' paths taken out. After a lift, its output is saved here under the share key taken as the lift started (save): the
lifted C and the image (<work>/sa/gen, <work>/sa/image) and the two files the lift writes for the next one (image/chunks.txt,
image/hot_pieces.txt), so a folder that takes it (restore) has the boundaries that folder had after its lift, as if it had
lifted the game itself. A work folder whose share key finds a snapshot takes it instead of lifting.

Every file is copied, never hard-linked: the later steps write in the lifted C's folder. On a file system with copy-on-write
(btrfs, XFS: Linux's FICLONE) the copy is a clone and shares the blocks. A snapshot is complete or absent: it is written under
a temporary name with its list of files and sizes last, then renamed in, and a restore checks every file against that list
first. The newest HOOKSHOT_LIFT_STORE_KEEP (3) are kept, by last use; one used in the last hour is never taken.
"""
import os
import shutil
import time

STORE_ENV = "HOOKSHOT_LIFT_STORE"
DIRS = (("sa", "gen"), ("sa", "image"))
FILES = (("image", "chunks.txt"), ("image", "hot_pieces.txt"))
SKIP = (".lift_key", "lifted_objects.txt", "lifted_objects.txt.new")   # (make_game.py LIFT_KEY and build.py's KEPT_LIST: not the lift's)
MANIFEST = "snapshot.txt"
KEEP = 3
RECENT_S = 3600


def root():
    r = os.environ.get(STORE_ENV, "").strip()
    return None if r in ("", "0") else os.path.abspath(os.path.expanduser(r))


def _clone(src, dst):
    """A copy of src at dst, a copy-on-write clone where the file system has them"""
    if hasattr(os, "O_BINARY") or not hasattr(os, "fork"):   # (Windows: a plain copy)
        shutil.copyfile(src, dst)
        return
    import fcntl
    with open(src, "rb") as s, open(dst, "wb") as d:
        try:
            fcntl.ioctl(d.fileno(), 0x40049409, s.fileno())   # (FICLONE)
            return
        except OSError:
            pass
        shutil.copyfileobj(s, d, 1 << 20)


def _entries(base):
    """(relative path, absolute path) of every file a snapshot holds, read from base (a work folder or a snapshot)"""
    out = []
    for parts in DIRS:
        top = os.path.join(base, *parts)
        for d, dirs, fs in os.walk(top):
            dirs.sort()
            for f in sorted(fs):
                if d == top and f in SKIP:
                    continue
                p = os.path.join(d, f)
                out.append((os.path.relpath(p, base).replace(os.sep, "/"), p))
    for parts in FILES:
        p = os.path.join(base, *parts)
        if os.path.isfile(p):
            out.append(("/".join(parts), p))
    return out


def save(store, name, work, keep=None):
    """work's lift as the snapshot name. False when there was one already or it could not be written"""
    snap = os.path.join(store, name)
    if os.path.isdir(snap):
        return False
    tmp = os.path.join(store, ".%s.%d.tmp" % (name, os.getpid()))
    try:
        os.makedirs(store, exist_ok=True)
        shutil.rmtree(tmp, ignore_errors=True)
        listed = []
        for rel, src in _entries(work):
            dst = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            _clone(src, dst)
            listed.append("%d %s" % (os.path.getsize(dst), rel))
        with open(os.path.join(tmp, MANIFEST), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(listed) + "\n")
        os.rename(tmp, snap)
    except OSError as e:
        shutil.rmtree(tmp, ignore_errors=True)
        if not os.path.isdir(snap):
            print("the translation was not kept for other work folders (%s)" % e, flush=True)
        return False
    prune(store, keep)
    return True


def _listed(snap):
    """The snapshot's files and sizes, or None when it is not complete"""
    try:
        with open(os.path.join(snap, MANIFEST), encoding="utf-8") as f:
            rows = [line.rstrip("\n").split(" ", 1) for line in f if line.strip()]
        listed = {rel: int(size) for size, rel in rows}
    except (OSError, ValueError, UnicodeDecodeError):
        return None
    if not any(rel.startswith("sa/gen/") for rel in listed) or not any(rel.startswith("sa/image/") for rel in listed):
        return None
    for rel, size in listed.items():
        try:
            if os.path.getsize(os.path.join(snap, *rel.split("/"))) != size:
                return None
        except OSError:
            return None
    return listed


def forget(store, name):
    """A snapshot that turned out bad, gone: the next folder lifts the game itself"""
    shutil.rmtree(os.path.join(store, name), ignore_errors=True)


def restore(store, name, work, bad=None):
    """The snapshot name into work, in place of its lifted C, image and the lift's two files. False (and work's lifted C and image
    gone, the two files as they were) when there is none or it could not be copied. bad(): a check of the copied C (make_game.py
    lifted_cut_short); one that finds anything forgets the snapshot"""
    snap = os.path.join(store, name)
    listed = _listed(snap) if os.path.isdir(snap) else None
    if listed is None:
        return False
    try:
        os.utime(snap)
    except OSError:
        pass
    try:
        for parts in DIRS:
            shutil.rmtree(os.path.join(work, *parts), ignore_errors=True)
            if os.path.exists(os.path.join(work, *parts)):
                raise OSError("could not clear %s" % os.path.join(work, *parts))
        later = []
        for rel in sorted(listed):
            src, dst = os.path.join(snap, *rel.split("/")), os.path.join(work, *rel.split("/"))
            if tuple(rel.split("/")) in FILES:
                later.append((src, dst))
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            _clone(src, dst)
        if any(os.path.getsize(os.path.join(work, *rel.split("/"))) != size for rel, size in listed.items()
               if tuple(rel.split("/")) not in FILES):
            raise OSError("a file came out another size")
        found = bad() if bad else None
        if found:
            forget(store, name)
            raise OSError("its lifted C is cut short (%s), so it is let go" % ", ".join(found[:6]))
        for parts in FILES:   # (last: until here the folder keeps its own, and lifts from them if this fails)
            dst = os.path.join(work, *parts)
            src = next((s for s, d in later if d == dst), None)
            if src is None:
                if os.path.exists(dst):
                    os.remove(dst)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            tmp = dst + ".%d.tmp" % os.getpid()
            _clone(src, tmp)
            os.replace(tmp, dst)
    except OSError as e:
        for parts in DIRS:
            shutil.rmtree(os.path.join(work, *parts), ignore_errors=True)
        print("the translation of another work folder could not be copied (%s): the game is translated here" % e, flush=True)
        return False
    return True


def prune(store, keep=None, now=None):
    """The newest snapshots kept (by last use), the rest gone; a snapshot used in the last hour stays, and a temporary folder a
    stopped save left goes after a day. Returns how many went"""
    if not store or not os.path.isdir(store):
        return 0
    if keep is None:
        try:
            keep = max(1, int(os.environ.get("HOOKSHOT_LIFT_STORE_KEEP", "") or KEEP))
        except ValueError:
            keep = KEEP
    now = time.time() if now is None else now
    snaps, gone = [], 0
    for f in os.listdir(store):
        p = os.path.join(store, f)
        try:
            used = os.stat(p).st_mtime
        except OSError:
            continue
        if not os.path.isdir(p):
            continue
        if f.startswith("."):
            if now - used > 86400:
                shutil.rmtree(p, ignore_errors=True)
                gone += 1
            continue
        snaps.append((used, p))
    snaps.sort(reverse=True)
    for used, p in snaps[keep:]:
        if now - used >= RECENT_S:
            shutil.rmtree(p, ignore_errors=True)
            gone += 1
    return gone
