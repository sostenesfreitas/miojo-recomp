"""One store of compiled objects for every work folder on this PC (HOOKSHOT_OBJ_STORE=<folder>; off when unset, empty or 0).

A work folder keeps its own object cache (<work>/objcache: nx2/build.py's cache_put and compile_one, make_game.py's
prune_object_cache) as before. With a store, build.py takes the objects this build needs and the folder lacks from the store
(fetch), and gives the store the folder's objects it lacks (share). Both are hard links, so an object is on the disk once
whatever number of folders use it; a copy where the store and the work folder are on two volumes (or a file system without
hard links), made under a temporary name and renamed in. The object keys hold no folder path (build.py flag_text, object_key),
so another work folder's object of the same key is the one this build would compile.

The keys do not name the compiler and the system headers: a work folder notes them in system_headers.txt (make_game.py
drop_stale_objects) and lets its objects go when they change. The store keeps a folder of its own per note (its first 16
characters), and a work folder without the note uses no store.

Nothing writes an object in place: the compiler writes a new file and renames it over the old one, and the folder's clean-up
only removes its own links, so a store object stays whole while any folder uses it. The store's own clean-up (prune, after each
make_game.py build): an object only the store holds (one link, or every object where there are none) goes once it has not been
used for HOOKSHOT_OBJ_STORE_DAYS (14), and the oldest go first while those come to more than HOOKSHOT_OBJ_STORE_GB (8). An
object used in the last hour is never taken, so a build that is fetching it keeps it.
"""
import os
import shutil
import time

STORE_ENV = "HOOKSHOT_OBJ_STORE"
NOTE = "system_headers.txt"
KEEP_DAYS, KEEP_GB = 14.0, 8.0
RECENT_S = 3600


def root():
    """The store's folder, or None when there is none"""
    r = os.environ.get(STORE_ENV, "").strip()
    return None if r in ("", "0") else os.path.abspath(os.path.expanduser(r))


def folder(work, store_root=None):
    """The store's folder for the compiler and headers work was last built with, or None (no store, no note)"""
    r = store_root or root()
    if not r:
        return None
    try:
        with open(os.path.join(work, NOTE), encoding="utf-8") as f:
            stamp = f.read().strip()
    except (OSError, UnicodeDecodeError):
        return None
    if len(stamp) < 16 or any(c not in "0123456789abcdef" for c in stamp[:16]):
        return None
    return os.path.join(r, stamp[:16])


def _place(src, dst):
    """src at dst: a hard link, else a copy renamed in. False when dst is there already or src is not"""
    if os.path.exists(dst):
        return False
    try:
        os.link(src, dst)
        return True
    except FileExistsError:
        return False
    except FileNotFoundError:
        return False
    except OSError:
        pass
    tmp = "%s.%d.tmp" % (dst, os.getpid())
    try:
        shutil.copyfile(src, tmp)
        os.rename(tmp, dst)   # (POSIX: over one that came in meanwhile, the same object; Windows: FileExistsError)
        return True
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False


def _touch(path):
    try:
        os.utime(path)
    except OSError:
        pass


def fetch(store, cache, keys):
    """The objects named by keys that cache lacks and store has, into cache. Returns how many"""
    if not store or not os.path.isdir(store):
        return 0
    os.makedirs(cache, exist_ok=True)
    n = 0
    for key in sorted(set(keys)):
        dst = os.path.join(cache, key + ".o")
        if os.path.exists(dst):
            continue
        src = os.path.join(store, key + ".o")
        if _place(src, dst):
            _touch(src)
            n += 1
    return n


def share(cache, store):
    """The objects of cache that store lacks, into store. Returns how many"""
    if not store or not os.path.isdir(cache):
        return 0
    try:
        os.makedirs(store, exist_ok=True)
        have = set(os.listdir(store))
    except OSError:
        return 0
    n = 0
    for f in os.listdir(cache):
        if f.endswith(".o") and f not in have and _place(os.path.join(cache, f), os.path.join(store, f)):
            _touch(os.path.join(store, f))
            n += 1
    return n


def forget(store, keys):
    """Those objects out of the store (a link that failed with them): the next build compiles them again. A work folder's own
    links stay as they are"""
    if not store:
        return
    for key in keys:
        try:
            os.remove(os.path.join(store, key + ".o"))
        except OSError:
            pass


def _number(name, default):
    try:
        return max(0.0, float(os.environ.get(name, "") or default))
    except ValueError:
        return default


def prune(store_root=None, now=None):
    """The store's own clean-up (the module's notes). Returns (objects removed, bytes freed)"""
    r = store_root or root()
    if not r or not os.path.isdir(r):
        return 0, 0
    now = time.time() if now is None else now
    days, limit = _number("HOOKSHOT_OBJ_STORE_DAYS", KEEP_DAYS), _number("HOOKSHOT_OBJ_STORE_GB", KEEP_GB) * (1 << 30)
    alone = []   # (last used, size, path) of the objects only the store holds
    for sub in os.listdir(r):
        d = os.path.join(r, sub)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            p = os.path.join(d, f)
            try:
                st = os.stat(p)
            except OSError:
                continue
            if f.endswith(".tmp") and now - st.st_mtime > RECENT_S:   # (a copy a stopped build left)
                alone.append((0.0, st.st_size, p))
            elif f.endswith(".o") and st.st_nlink <= 1:
                alone.append((st.st_mtime, st.st_size, p))
    alone.sort()
    total = sum(size for _, size, _ in alone)
    removed = freed = 0
    for used, size, p in alone:
        age = now - used
        if age < RECENT_S or (age < days * 86400 and total <= limit):
            continue
        try:
            os.remove(p)
        except OSError:
            continue
        total -= size
        removed += 1
        freed += size
    return removed, freed
