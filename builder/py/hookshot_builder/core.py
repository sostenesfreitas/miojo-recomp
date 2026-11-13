"""What the WPF builder's Runner.cs, Storage.cs, Finder.cs and BuildWorkers.cs do, the same rules: the build tools, nx2/make_game.py as a
child process, what the builder keeps on the PC, the game files found on it, and the game's shortcuts."""
import ctypes
import json
import os
import shutil
import string
import subprocess
import sys
import threading
import time
from ctypes import wintypes

from . import paths

CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL_PRIORITY_CLASS = 0x00004000


# ---------------- the build tools (Runner.cs: Tools) ----------------

class Tools:
    """Where nx2/make_game.py and its Python are: the installed version's tools, or the source tree this app sits in"""

    def __init__(self, root, python, python_args=None):
        self.root, self.python, self.python_args = root, python, python_args or []

    @property
    def version(self):
        """Which release these tools are (tools/version.txt, written when the release is packed); None from the source tree"""
        return paths.read_small(os.path.join(self.root, "version.txt")) or None

    @staticmethod
    def find():
        from . import setup_parts
        env = os.environ.get("HOOKSHOT_TOOLS")   # (development: these tools)
        cands = [env] if env else []
        cur = setup_parts.current()
        if cur:
            cands.append(os.path.join(cur, "HookshotBuilder", "tools"))
        for c in cands:
            if c and os.path.isfile(os.path.join(c, "nx2", "make_game.py")):
                # (tools with a Python of their own run no other. With its python.exe gone, the first "python" Windows found
                # ran them instead: another version, without the modules they need, or not one the player chose to trust
                # with the game files. Its start now fails and says how to get it back: own_python_missing)
                py = os.path.join(c, "python", "python.exe")
                return Tools(c, py if os.path.isfile(py) or os.path.isdir(os.path.join(c, "python")) else "python")
        d = paths.APP_DIR
        while True:   # (a source checkout: builder/py/hookshot_builder in it)
            if os.path.isfile(os.path.join(d, "nx2", "make_game.py")):
                return Tools(d, sys.executable)
            up = os.path.dirname(d)
            if up == d:
                return None
            d = up


OWN_PYTHON_MISSING = ("The builder's own Python (tools/python/python.exe) is missing. Your antivirus probably removed it: restore "
                      "it from quarantine, then try again.")


def own_python_missing(tools):
    """The tools' own Python is gone: an antivirus took it, most often, after a false alarm on the download"""
    return not os.path.isfile(tools.python) and os.path.isdir(os.path.join(tools.root, "python"))


def build_env(tools, workers=None, ngx_sdk=None):
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    if workers:
        env["HOOKSHOT_JOBS"] = str(int(workers))
    # (written into the game folder's hookshot_version.txt: the game starts this app again to update, host/hos_update.c)
    exe, args = paths.launcher_command()
    env["HOOKSHOT_BUILDER_EXE"] = exe
    env["HOOKSHOT_BUILDER_ARGS"] = subprocess.list2cmdline(args)
    # (NVIDIA DLSS: only when ticked. Without it, one set on the PC is not passed on either)
    if ngx_sdk:
        env["HOOKSHOT_NGX_SDK"] = ngx_sdk
    else:
        env.pop("HOOKSHOT_NGX_SDK", None)
    for name, folder in (("EMSDK", "emsdk"), ("RX_CPM", "cpm"), ("WGPU_NATIVE", "wgpu-native")):
        p = os.path.join(tools.root, folder)
        if os.path.isdir(p):
            env[name] = p
    return env


def build_command(tools, args):
    return [tools.python, *tools.python_args, "-I", os.path.join(tools.root, "nx2", "make_game.py"), *args]


class Runner:
    """nx2/make_game.py as a child process: its lines as they come (on_line, from a worker thread), and how it ended (on_finished:
    None, "cancelled", or why it stopped)"""

    def __init__(self, tools, args, workers=None, ngx_sdk=None, on_line=None, on_finished=None):
        if workers is not None and workers <= 0:
            raise ValueError("Build workers must be positive.")
        self.tools, self.args, self.workers, self.ngx_sdk = tools, args, workers, ngx_sdk
        self.on_line, self.on_finished = on_line or (lambda l: None), on_finished or (lambda f: None)
        self.process, self.error, self.cancelled = None, None, False

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        try:
            p = subprocess.Popen(build_command(self.tools, self.args), cwd=self.tools.root,
                                 env=build_env(self.tools, self.workers, self.ngx_sdk), stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                 creationflags=CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS)
        except OSError as e:
            self.on_finished(OWN_PYTHON_MISSING if own_python_missing(self.tools) else "The build tools could not start: %s" % e)
            return
        self.process = p

        def errs():
            for raw in p.stderr:
                self.on_line(raw.decode("utf-8", "replace").rstrip("\r\n"))
        threading.Thread(target=errs, daemon=True).start()
        for raw in p.stdout:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if line.startswith("@@error "):
                self.error = line[8:]
            self.on_line(line)
        p.wait()
        self.on_finished("cancelled" if self.cancelled else None if p.returncode == 0 else
                         self.error or "The build tools stopped without saying why. See the details.")

    def cancel(self):
        self.cancelled = True
        p = self.process
        if p and p.poll() is None:   # (the whole tree: the compilers it started too)
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True, creationflags=CREATE_NO_WINDOW)


def check(tools, args):
    """Is this the supported game and version? (ok, the answer in plain words)"""
    try:
        p = subprocess.run(build_command(tools, args), cwd=tools.root, env=build_env(tools), capture_output=True,
                           stdin=subprocess.DEVNULL, creationflags=CREATE_NO_WINDOW)
    except OSError:
        if own_python_missing(tools):
            return False, OWN_PYTHON_MISSING
        return False, "Python 3 was not found. Install it from python.org (not the Microsoft Store build)."
    result, answered = (False, "The game files could not be read."), False
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        if line.startswith("@@check ok "):
            result, answered = (True, line[11:]), True
        elif line.startswith("@@check bad "):
            result, answered = (False, line[12:]), True
        elif line.startswith("@@error "):
            result, answered = (False, line[8:]), True
    if not answered:
        errs = [l for l in p.stderr.decode("utf-8", "replace").splitlines() if l.strip()]
        last = errs[-1] if errs else None
        if last and "microsoft store" in last.lower():
            return False, ("Python 3 was not found: the 'python' command on this PC is the Microsoft Store shortcut, which only opens "
                           "the Store. Install Python 3 from python.org (not the Store version).")
        return False, last or "The game tools stopped without saying why (exit code %d)." % p.returncode
    return result


# ---------------- build workers (BuildWorkers.cs) ----------------

AUTOMATIC = "Automatic"


def worker_choices():
    return [AUTOMATIC] + list(range(1, max(1, os.cpu_count() or 1) + 1))


# ---------------- what the builder keeps on the PC (Storage.cs) ----------------

BUILD_FILES_BYTES = 4 << 30


def keep_build_files():
    v = os.environ.get("HOOKSHOT_KEEP_BUILD_FILES", "")
    return bool(v) and v != "0"


def _same(a, b):
    return os.path.normcase(os.path.abspath(a)).rstrip("\\/") == os.path.normcase(os.path.abspath(b)).rstrip("\\/")


def work():
    """Where builds keep their files: the folder the player chose (work.txt), else the builder's own"""
    chosen = paths.read_small(os.path.join(paths.base(), "work.txt"))
    return chosen if len(chosen) > 2 and os.path.isabs(chosen) else paths.base()


def work_chosen():
    return not _same(work(), paths.base())


def ngx_sdk():
    return os.path.join(paths.base(), "ngx-sdk")


WORK_PARTS = ("objcache", "sa", "exefs", "probe", "image", "system_headers.txt", "hot_funcs.txt", "hot_order.txt",
              "hookshot_pieces.prof", "hookshot.res")


def _delete(p):
    try:
        if os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.isfile(p):
            os.remove(p)
    except OSError:
        shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else None


def move_work(picked):
    """The build files go to a folder of their own in the one the player picked (None: back to the builder's own); what the last
    builds left at the old place goes"""
    old = work()
    to = paths.base() if picked is None else os.path.join(picked, "Hookshot build files")
    if _same(old, to):
        return
    os.makedirs(to, exist_ok=True)
    os.makedirs(paths.base(), exist_ok=True)
    if picked is None:
        try:
            os.remove(os.path.join(paths.base(), "work.txt"))
        except OSError:
            pass
    else:
        paths.write_small(os.path.join(paths.base(), "work.txt"), to)
    for n in WORK_PARTS:
        _delete(os.path.join(old, n))
    if not _same(old, paths.base()):
        try:
            os.rmdir(old)
        except OSError:
            pass


def _kept_version(d):
    from . import setup_parts
    cur = setup_parts.current()
    return bool(cur) and _same(cur, d)


def kept_translation(sa):
    """make_game.py keep_translation left the lift's small files in sa (its .lift_key marked "kept"): the next update with the same
    tools skips the lift with them. Not let go after a build (Free up space still lets them go)"""
    try:
        with open(os.path.join(sa, "gen", ".lift_key"), encoding="utf-8") as f:
            return f.read().split()[2:3] == ["kept"]
    except (OSError, UnicodeDecodeError):
        return False


def freeable(after_build=False):
    for d in (("sa", "exefs", "probe") if after_build else ("objcache", "sa", "exefs", "probe")):
        if not (after_build and d == "sa" and kept_translation(os.path.join(work(), d))):
            yield os.path.join(work(), d)
    for d in ("downloads", "update"):
        yield os.path.join(paths.base(), d)
    v = paths.versions()
    if os.path.isdir(v):
        for name in os.listdir(v):
            p = os.path.join(v, name)
            if os.path.isdir(p) and not _kept_version(p):
                yield p


def _size(d, seen):
    n = 0
    for root, _dirs, files in os.walk(d):
        for f in files:
            try:
                st = os.stat(os.path.join(root, f))
            except OSError:
                continue
            if not st.st_size:
                continue
            if st.st_nlink > 1:   # (an object hard-linked in both sa\build and objcache counted once)
                key = (st.st_dev, st.st_ino)
                if key in seen:
                    continue
                seen.add(key)
            n += st.st_size
    return n


def measure():
    """(bytes in the builder's folder and the build files folder, bytes Free lets go)"""
    seen = set()
    free = sum(_size(d, seen) for d in freeable())
    seen = set()
    total = _size(paths.base(), seen) + (_size(work(), seen) if work_chosen() else 0)
    return max(total, free), free


def free(after_build=False):
    """Lets go of what measure counts as freeable; returns the bytes it could not delete (a file in use)"""
    for d in list(freeable(after_build)):
        _delete(d)
    if not after_build:
        _delete(os.path.join(work(), "system_headers.txt"))
        return measure()[1]
    seen = set()
    return sum(_size(d, seen) for d in freeable(True))


def gb(n):
    return "%.1f GB" % (n / (1 << 30)) if n >= 1 << 30 else "%d MB" % (n >> 20)


def free_space(path):
    try:
        root = os.path.splitdrive(os.path.abspath(path))[0] + "\\"
        return root, shutil.disk_usage(root).free
    except OSError:
        return None, None


def spinning(path):
    """True on a hard drive, False on an SSD, None when the drive cannot be told (Disk.cs's IOCTL: the volume's seek penalty).
    A build reads and writes tens of gigabytes, and an SSD makes that much faster: a warning only, never a stop."""
    try:
        from ctypes import wintypes
        root = os.path.splitdrive(os.path.abspath(os.path.expanduser(path)))[0]
        if not root:
            return None

        class Query(ctypes.Structure):
            _fields_ = [("PropertyId", wintypes.DWORD), ("QueryType", wintypes.DWORD), ("Additional", wintypes.DWORD)]

        class SeekPenalty(ctypes.Structure):
            _fields_ = [("Version", wintypes.DWORD), ("Size", wintypes.DWORD), ("IncursSeekPenalty", ctypes.c_ubyte)]

        k = ctypes.windll.kernel32
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        k.CreateFileW.restype = wintypes.HANDLE
        k.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        k.DeviceIoControl.restype = wintypes.BOOL
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = k.CreateFileW("\\\\.\\" + root, 0, 0x1 | 0x2, None, 3, 0, None)   # (read + write share, OPEN_EXISTING)
        if not handle or handle == wintypes.HANDLE(-1).value:
            return None
        try:
            query, answer, returned = Query(7, 0, 0), SeekPenalty(), wintypes.DWORD()
            ok = k.DeviceIoControl(handle, 0x002D1400, ctypes.byref(query), ctypes.sizeof(query), ctypes.byref(answer),
                                   ctypes.sizeof(answer), ctypes.byref(returned), None)
            if not ok or returned.value < ctypes.sizeof(answer):
                return None
            return bool(answer.IncursSeekPenalty)
        finally:
            k.CloseHandle(handle)
    except (OSError, ValueError, AttributeError):
        return None


# ---------------- the player's game files (Finder.cs) ----------------

class Dump:
    def __init__(self, path, where, size, modified):
        self.path, self.where, self.size, self.modified = path, where, size, modified


def _known_folder(fid):
    buf = ctypes.c_wchar_p()
    guid = (ctypes.c_byte * 16).from_buffer_copy(fid)
    if ctypes.windll.shell32.SHGetKnownFolderPath(guid, 0, None, ctypes.byref(buf)) == 0:
        p = buf.value
        ctypes.windll.ole32.CoTaskMemFree(buf)
        return p
    return None


def _guid(s):
    import uuid
    return uuid.UUID(s).bytes_le


FOLDER_DESKTOP = _guid("B4BFCC3A-DB2C-424C-B029-7FE99A87C641")
FOLDER_DOCUMENTS = _guid("FDD39AD0-238F-46AF-ADB4-6C85480369C7")
FOLDER_DOWNLOADS = _guid("374DE290-123F-4565-9164-39C4925E467B")
FOLDER_PROGRAMS = _guid("A77F5D77-2E2B-44C3-A6A2-ABA601054A51")


def find_dumps():
    """.nup files in Downloads, Desktop, Documents, the home folder and the top of each drive, one folder level down too, newest first"""
    home = os.path.expanduser("~")
    places = [(_known_folder(FOLDER_DOWNLOADS) or os.path.join(home, "Downloads"), "Downloads"),
              (_known_folder(FOLDER_DESKTOP) or os.path.join(home, "Desktop"), "Desktop"),
              (_known_folder(FOLDER_DOCUMENTS) or os.path.join(home, "Documents"), "Documents"), (home, "Home")]
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    for i, letter in enumerate(string.ascii_uppercase):
        if mask >> i & 1:
            root = letter + ":\\"
            if ctypes.windll.kernel32.GetDriveTypeW(root) in (2, 3):   # (removable, fixed)
                places.append((root, letter + ":"))
    seen, found = set(), []
    for d, name in places:
        if not d or not os.path.isdir(d) or os.path.normcase(os.path.abspath(d)) in seen:
            continue
        seen.add(os.path.normcase(os.path.abspath(d)))
        dirs = [d]
        try:
            subs = [e.path for e in os.scandir(d) if e.is_dir() and not e.name.startswith((".", "$"))
                    and e.name.lower() != "windows" and not e.name.lower().startswith("program")]
            dirs += subs[:200]
        except OSError:
            pass
        for sub in dirs:
            try:
                for e in os.scandir(sub):
                    if e.is_file() and e.name.lower().endswith(".nup"):
                        full = os.path.abspath(e.path)
                        if os.path.normcase(full) not in seen:
                            seen.add(os.path.normcase(full))
                            st = e.stat()
                            found.append(Dump(full, name, st.st_size, st.st_mtime))
            except OSError:
                pass
    return sorted(found, key=lambda x: -x.modified)


def ago(t):
    import datetime
    now, then = datetime.datetime.now(), datetime.datetime.fromtimestamp(t)
    days = (now - then).total_seconds() / 86400
    if days < 1 and then.date() == now.date():
        return "modified today"
    if days < 2:
        return "modified yesterday"
    if days < 14:
        return "%d days ago" % int(days)
    if days < 60:
        return "%d weeks ago" % int(days / 7)
    return "%d %s" % (then.day, then.strftime("%b %Y"))


def size_text(n):
    return "%.1f GB" % (n / (1 << 30)) if n >= 1 << 30 else "%d MB" % (n >> 20)


# ---------------- the game folder ----------------

def _version_line(folder):
    for line in paths.read_small(os.path.join(folder, "hookshot_version.txt")).splitlines():
        if line.startswith("version="):
            return line[8:].strip() or None
    return None


def game_version(folder):
    """The release a game folder was built with, when it keeps its game files for updates"""
    if not os.path.isdir(os.path.join(folder, ".hookshot_exefs")) or not os.path.isdir(os.path.join(folder, "romfs")):
        return None
    return _version_line(folder)


def read_version(folder):
    """(a folder built before it kept its game files has a version too: only shown)"""
    return _version_line(folder)


def game_exe(folder):
    g = os.path.join(folder, "bin", "hookshot.exe")
    return g if os.path.isfile(g) else os.path.join(folder, "hookshot.exe")


def start_game(folder):
    """(the launcher already checked for updates: the game does not ask again)"""
    env = dict(os.environ, HOOKSHOT_NO_UPDATE_CHECK="1")
    subprocess.Popen([game_exe(folder)], cwd=folder, env=env)


def open_folder(folder):
    subprocess.Popen(["explorer.exe", folder])


# ---------------- shortcuts (Finder.cs: IShellLinkW + IPersistFile, Windows' own shell object writes the .lnk) ----------------

def desktop_link():
    return os.path.join(_known_folder(FOLDER_DESKTOP) or os.path.join(os.path.expanduser("~"), "Desktop"), "Hookshot.lnk")


def start_link():
    return os.path.join(_known_folder(FOLDER_PROGRAMS) or "", "Hookshot.lnk")


def folder_link(game_folder):
    """The game folder's own launcher: Play, and Update left of it when a newer release is out (the setup exe's launch mode)"""
    return os.path.join(game_folder, "Hookshot.lnk")


shortcut_error = None


def _com_call(obj, index, restype, *args):
    vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    fn = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *[type(a) for a in args])(vtbl[index])
    return fn(obj, *args)


def make_link(link, target, args, workdir, icon, description):
    ole32 = ctypes.windll.ole32
    ole32.CoInitialize(None)
    clsid = (ctypes.c_byte * 16).from_buffer_copy(_guid("00021401-0000-0000-C000-000000000046"))
    iid_link = (ctypes.c_byte * 16).from_buffer_copy(_guid("000214F9-0000-0000-C000-000000000046"))
    iid_file = (ctypes.c_byte * 16).from_buffer_copy(_guid("0000010B-0000-0000-C000-000000000046"))
    sl = ctypes.c_void_p()
    hr = ole32.CoCreateInstance(clsid, None, 1, iid_link, ctypes.byref(sl))
    if hr != 0:
        raise OSError("CoCreateInstance(ShellLink) failed: 0x%08x" % (hr & 0xFFFFFFFF))
    try:
        W = ctypes.c_wchar_p
        _com_call(sl, 20, ctypes.HRESULT, W(target))            # SetPath
        _com_call(sl, 11, ctypes.HRESULT, W(args))              # SetArguments
        _com_call(sl, 9, ctypes.HRESULT, W(workdir))            # SetWorkingDirectory
        _com_call(sl, 17, ctypes.HRESULT, W(icon), ctypes.c_int(0))   # SetIconLocation
        _com_call(sl, 7, ctypes.HRESULT, W(description))        # SetDescription
        pf = ctypes.c_void_p()
        _com_call(sl, 0, ctypes.HRESULT, ctypes.cast(iid_file, ctypes.c_void_p), ctypes.byref(pf))   # QueryInterface
        try:
            os.makedirs(os.path.dirname(link), exist_ok=True)
            _com_call(pf, 6, ctypes.HRESULT, W(link), wintypes.BOOL(True))   # IPersistFile::Save
        finally:
            _com_call(pf, 2, ctypes.c_ulong)
    finally:
        _com_call(sl, 2, ctypes.c_ulong)


def shortcuts(game_folder, desktop, start, folder=True):
    """Made or removed as the ticks say: each starts this app's launcher for the folder (Play, and Update when a newer release is
    out), with the game's own icon"""
    global shortcut_error
    exe, args = paths.launcher_command()
    args = subprocess.list2cmdline(args + ["--launch", game_folder])
    icon = os.path.join(game_folder, "bin", "hookshot.exe")
    if not os.path.isfile(icon):
        icon = os.path.join(game_folder, "hookshot.exe")
    for link, want in ((desktop_link(), desktop), (start_link(), start), (folder_link(game_folder), folder)):
        try:
            if not want:
                if os.path.isfile(link):
                    os.remove(link)
                continue
            if not os.path.isfile(icon):
                continue
            make_link(link, exe, args, game_folder, icon, "The Legend of Zelda: Ocarina of Time, built by Hookshot")
        except OSError as e:
            shortcut_error = "%s: %s" % (os.path.basename(link), e)


def refresh_shortcuts(game_folder):
    shortcuts(game_folder, os.path.isfile(desktop_link()), os.path.isfile(start_link()))


# ---------------- what the player chose last time (builder-settings.json, the WPF builder's) ----------------

def settings_path():
    return os.path.join(paths.base(), "builder-settings.json")


def load_settings():
    try:
        with open(settings_path(), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_settings(s):
    try:
        os.makedirs(paths.base(), exist_ok=True)
        with open(settings_path(), "w", encoding="utf-8") as f:
            json.dump(s, f)
    except OSError:
        pass
