"""Hookshot Builder for Linux: what builder/HookshotBuilder (WPF) is on Windows, in Tk. It runs nx2/make_game.py and
follows its @@ lines (make_game.py's docstring), then offers to start the game. The AppImage starts it (builder/linux/AppRun),
with the compiler, sysroot and libraries it brings in the environment.

    hookshot_builder.py                     the window. Its button is Build, Update, Play, or Update builder
    hookshot_builder.py --play              the game built last time, if there is one (else the window)
    hookshot_builder.py --launch <folder>   that folder's launcher: Play, and Update when a newer release is out
    hookshot_builder.py --update <folder>   download the new release (only parts that changed) and build the folder again
"""
import math
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("HOOKSHOT_TOOLS") or os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
import linux_update  # noqa: E402  (the release check and the part download)
import make_game  # noqa: E402  (LANGUAGES, BUILD_ID)
import pipeline_share  # noqa: E402  (Save my pipeline list: the zip for the "Submit your pipelines" issue)
import toolchain  # noqa: E402  (default_jobs: RX_JOBS)

CONFIG = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "hookshot", "builder.ini")
STAGES = {"read": "Reading your game files", "lift": "Translating the game to PC code", "compile": "Compiling",
          "finish": "Putting the game folder together", "shaders": "Preparing shaders"}
DLSS_LICENSE = (
    "NVIDIA DLSS is NVIDIA's, not Hookshot's. With it ticked, the build downloads NVIDIA's DLSS SDK (release v310.9.16) from "
    "github.com/NVIDIA/DLSS, under NVIDIA's own license, the NVIDIA RTX SDKs license:\n\n"
    "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt\n\n"
    "Its files stay on this PC (in the builder's ngx-sdk folder, and libnvidia-ngx-dlss.so and libnvidia-ngx-dlssg.so in "
    "the game folder's ngx folder). "
    "Hookshot never includes or shares them: never share a game folder built with them.\n\n"
    "Do you accept NVIDIA's license?"
)


def cache_dir():
    """The builder's own folder on Linux, as make_game.py's work folder: $XDG_CACHE_HOME/hookshot-builder (~/.cache/...)"""
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "hookshot-builder")


def ngx_sdk_dir():
    """Where NVIDIA's DLSS SDK goes when the player ticks NVIDIA DLSS: the builder's cache folder, as on Windows its own folder
    (%LOCALAPPDATA%\\HookshotBuilder\\ngx-sdk). A download, fetched again by the next build with DLSS when the cache was emptied;
    whether NVIDIA's license was accepted is kept in the builder's settings (license_accepted), not in it"""
    return os.path.join(cache_dir(), "ngx-sdk")


def old_ngx_sdk_dir():
    """Where the SDK went before: the game's data folder ($XDG_DATA_HOME/hookshot/builder/ngx-sdk), beside the builder's versions"""
    return os.path.join(linux_update.data_root(), "ngx-sdk")


def license_accepted(cfg=None):
    """Whether the player accepted NVIDIA's license here: the builder's settings say so (nvidia_license=1), or the SDK it was
    accepted for is still where builders before the record kept it"""
    cfg = load_config() if cfg is None else cfg
    return cfg.get("nvidia_license") == "1" or os.path.isfile(os.path.join(old_ngx_sdk_dir(), "LICENSE.txt"))


def move_old_ngx_sdk():
    """(once) The SDK a builder before kept in the game's data folder moved to the cache folder, and the license it was accepted
    under recorded in the settings. When it cannot move it is left: the next build with DLSS downloads it again"""
    old, new = old_ngx_sdk_dir(), ngx_sdk_dir()
    if not os.path.isfile(os.path.join(old, "LICENSE.txt")):
        return
    cfg = load_config()
    if cfg.get("nvidia_license") != "1":
        cfg["nvidia_license"] = "1"
        save_config(cfg)
    if os.path.exists(new):
        shutil.rmtree(old, ignore_errors=True)
        return
    try:
        os.makedirs(os.path.dirname(new), exist_ok=True)
        shutil.move(old, new)
    except OSError:
        pass


def folder_has_dlss(folder):
    """Whether a game folder was built with NVIDIA DLSS: its runtime in ngx/, and NVIDIA's license accepted here."""
    if not folder:
        return False
    ngx_dir = os.path.join(os.path.expanduser(folder), "ngx")
    try:
        has_runtime = any(name.startswith("libnvidia-ngx-dlss.so") for name in os.listdir(ngx_dir))
    except OSError:
        return False
    return has_runtime and license_accepted()


def dlss_args(dlss):
    return ["--fetch-ngx-sdk", "--accept-nvidia-license"] if dlss else []


def make_game_env(dlss, environ=None):
    """The environment make_game.py runs in. HOOKSHOT_NGX_SDK is set only when DLSS is chosen (one set in the shell is not
    passed on when unticked, as on Windows), and HOOKSHOT_VK_PREBUILT is dropped so build.py compiles hookshot_vk.so with NGX."""
    env = dict(os.environ if environ is None else environ)
    if dlss:
        env["HOOKSHOT_NGX_SDK"] = ngx_sdk_dir()
        env.pop("HOOKSHOT_VK_PREBUILT", None)
    else:
        env.pop("HOOKSHOT_NGX_SDK", None)
    return env


def left_text(seconds):
    """The countdown make_game.py and build.py work out from this PC's earlier builds."""
    if seconds < 45:
        return "less than a minute left"
    if seconds < 90:
        return "about a minute left"
    minutes = int(round(seconds / 60))
    return "about %d h %d min left" % (minutes // 60, minutes % 60) if minutes >= 60 else "about %d minutes left" % minutes


TRACK_NAMES = {"release": "Release", "rc": "Release candidate", "nightly": "Nightly"}


def load_config():
    cfg = {}
    try:
        for line in open(CONFIG, encoding="utf-8"):
            k, eq, v = line.rstrip("\n").partition("=")
            if eq:
                cfg[k] = v
    except OSError:
        pass
    return cfg


def save_config(cfg):
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    with open(CONFIG, "w", encoding="utf-8") as f:
        f.writelines("%s=%s\n" % kv for kv in cfg.items())


def game_ready(folder):
    return linux_update.game_exe(folder) is not None


def read_cgroup(path):
    """The unified control group, or "" when /proc is unavailable."""
    try:
        with open(path, encoding="utf-8") as f:
            return next((line.rstrip("\n")[3:] for line in f if line.startswith("0::")), "")
    except OSError:
        return ""


def own_scope(env, cgroup="/proc/self/cgroup", which=shutil.which, run=subprocess.run):
    """What goes before the game's command so that it outlives this window's systemd service, or [] when nothing has to.
    A desktop that starts each launcher as a service of its own (app-hookshot@<id>.service: COSMIC, GNOME, KDE) stops that
    service, and everything in its control group, when its main process ends; start_new_session leaves the session, not the
    control group, so Play then closing the window had systemd stop the game too, with SIGKILL 90 s later (#2529). In a scope
    of its own it stays. A terminal, a scope or no systemd: nothing to do, and the game starts as before"""
    unit = read_cgroup(cgroup)
    if not unit.rpartition("/")[2].endswith(".service"):
        return []
    tool = which("systemd-run", path=env.get("PATH"))
    if not tool:
        return []
    scope = [tool, "--user", "--scope", "--collect", "--quiet", "--"]
    # (once first with true: a user manager that cannot be reached would otherwise leave Play starting nothing)
    try:
        ok = run(scope + ["true"], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                 timeout=10).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return []
    return scope if ok else []


def wait_scope(process, proc_root="/proc", clock=time.monotonic, sleep=time.sleep, timeout=2):
    """Wait only until the real runner leaves this service, not until its game ends."""
    parent = read_cgroup(os.path.join(proc_root, "self", "cgroup"))
    if not parent:
        return False
    deadline = clock() + timeout
    while process.poll() is None:
        child = read_cgroup(os.path.join(proc_root, str(process.pid), "cgroup"))
        if child and child != parent and not child.startswith(parent + "/"):
            return True
        left = deadline - clock()
        if left <= 0:
            break
        sleep(min(0.01, left))
    return False


def start_scoped(scope, command, options, wait=wait_scope):
    """Start once: a runner that times out cannot later start a second game beside the plain fallback."""
    process = None
    try:
        gate, go = socket.socketpair()
        with gate, go:
            # stdout is normally discarded. Use it as a gate until /proc proves the runner has escaped;
            # the shell restores /dev/null before exec, leaving the diagnostics' stdin/stderr sockets alone.
            script = 'IFS= read -r ready <&1 || exit 1\n[ "$ready" = go ] || exit 1\nexec >/dev/null\nexec "$@"'
            process = subprocess.Popen(scope + ["/bin/sh", "-c", script, "hookshot-scope"] + command,
                                       **dict(options, stdout=gate))
            gate.close()
            if wait(process):
                go.sendall(b"go\n")
                return True
    except OSError:
        pass
    # No "go" was sent. Closing the gate above forbids a late exec, even if the move races this cancellation.
    if process is not None:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:   # (a stuck runner still cannot pass its closed gate)
            pass
    return False


def play(folder):
    """the game, on its own (it outlives this window), from its folder. The launcher already asked about updates."""
    exe = linux_update.game_exe(folder)
    if not exe:
        return
    env = make_game.game_env()   # (as make_game's shader preparation and hookshot-launch: the game's own SDL, the system's libraries)
    env["HOOKSHOT_NO_UPDATE_CHECK"] = "1"
    exe = os.path.abspath(exe)
    options = dict(cwd=folder, env=env, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    scope = own_scope(env)   # (out of the launcher's systemd service, when it runs in one: #2529)
    try:
        with open(os.path.join(HERE, "play-diagnostics.sh"), encoding="utf-8") as script:
            command = ["/bin/sh", "-c", script.read(), "hookshot-play", exe]
        # (stderr a socket, not a pipe: the game takes a socket, as the journal's where a desktop launcher sends it, for nowhere to
        # log, and writes hookshot_log.txt; a pipe it takes for a reader and writes no log file (host/posix/win32_posix.c GetFileType).
        # The script reads the other end: what the game prints before its log opens)
        reading, writing = socket.socketpair()
        with reading, writing:
            launch_options = dict(options, stdin=reading, stderr=writing)
            if not scope or not start_scoped(scope, command, launch_options):
                if scope:
                    command.append("Hookshot: systemd scope hand-off failed; started without a separate scope.")
                subprocess.Popen(command, **launch_options)
    except OSError:
        if not scope or not start_scoped(scope, [exe], options):
            subprocess.Popen([exe], **options)


def share_pipeline_list(folder, out_dir=None, copy=None, open_path=None):
    """Save my pipeline list (as the game's menu does it, nx2/pipeline_share.py): the game folder's pipeline list and the issue's
    template, filled in, zipped into the Downloads folder; then the template to copy, the folder and the issue to open. Nothing is
    uploaded. What happened, in the player's words"""
    opener = open_path or (lambda p: subprocess.Popen(["xdg-open", p], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    try:
        path, text, n = pipeline_share.make_zip(folder, out_dir)
    except pipeline_share.ShareError as e:
        return str(e)
    except OSError as e:
        return "The zip could not be written: %s" % e
    copied = False
    if copy:
        try:
            copy(text)
            copied = True
        except Exception:   # (no clipboard: the template is in the zip too)
            pass
    opened = True
    for target in (os.path.dirname(path), pipeline_share.ISSUE_URL):
        try:
            opener(target)
        except OSError:
            opened = False
    return ("Saved %s (%d pipelines) in %s. %s%s the zip to a comment, then paste the report and say where you played." %
            (os.path.basename(path), n, os.path.dirname(path), "The report is copied. " if copied else "The report is in the zip. ",
             "Issue #%d is open in your browser: attach" % pipeline_share.ISSUE if opened else
             "Open %s and attach" % pipeline_share.ISSUE_URL))


def desktop_exec(path):
    """One Exec argument. A % in the path is a field code, so "100% Games" is rejected and the menu entry does not run.
    Inside the quotes the Desktop Entry spec wants the characters double quote, backtick, dollar and backslash each
    preceded by a backslash, and every backslash written twice because the file's string escaping is applied first
    (so a literal backslash is four, and a dollar is two backslashes and the dollar)."""
    escaped = path.replace("\\", "\\\\\\\\")
    for char in ('"', "`", "$"):
        escaped = escaped.replace(char, "\\\\" + char)
    return '"%s"' % escaped.replace("%", "%%")


APP_ID = "io.github.NotRexed.Hookshot"   # (the game's window names itself so: host/posix/display_sdl.c's HOS_APP_ID)


def add_to_menu(folder):
    """an entry in the applications menu that starts the game"""
    if any(ch in folder for ch in "\r\n\0"):   # (a line break would start another key of the entry)
        raise ValueError("the game folder's name has a line break")
    icon = os.path.join(folder, "hookshot.png")
    src = os.path.join(ROOT, "assets", "hookshot.png")
    if os.path.exists(src):
        shutil.copyfile(src, icon)
    apps = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "applications")
    os.makedirs(apps, exist_ok=True)
    with open(os.path.join(apps, "hookshot.desktop"), "w", encoding="utf-8") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Hookshot\nComment=The Legend of Zelda: Ocarina of Time, built from your copy\n"
                "Exec=%s\nPath=%s\nIcon=%s\nCategories=Game;\nTerminal=false\nStartupWMClass=%s\n"
                % (desktop_exec(os.path.join(folder, "hookshot")), folder, icon, APP_ID))


def free_gb(path):
    path = os.path.expanduser(path or "")   # (the field takes ~/Games/Hookshot, as build() and apply_mode() read it)
    while path and not os.path.exists(path):
        path = os.path.dirname(path)
    try:
        return shutil.disk_usage(path or "/").free / 2 ** 30
    except OSError:
        return None


def slow_disk_warning(folder):
    """The game folder on a hard drive: the build works, but much more slowly than on an SSD. None for an SSD or a drive whose
    kind cannot be told (a network share)"""
    if make_game.spinning_disk(os.path.expanduser(folder)) is not True:
        return None
    return ("%s is on a hard drive. An SSD builds the game much faster: build there if you can.\n\n"
            "Build here anyway?" % os.path.expanduser(folder))


def xft_dpi():
    """the DPI KDE and GNOME hand X apps when the desktop is scaled. Tk on X11 and XWayland never reads it."""
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    try:
        out = subprocess.run(["xrdb", "-query"], env=env, capture_output=True, text=True, timeout=2).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    found = re.search(r"^Xft\.dpi:\s*([0-9.]+)", out, re.M)
    return float(found.group(1)) if found else None


def ui_scale(screen_height, dpi=None, environ=os.environ):
    """how much bigger than 96 DPI to draw the window. HOOKSHOT_UI_SCALE, then GDK_SCALE or QT_SCALE_FACTOR above 1
    (Wayland sessions export GDK_SCALE=1 whatever the screen), then Xft.dpi; with none of them (gamescope, a bare X
    session) a 1440p or 4K screen is still scaled by its height."""
    for name, least in (("HOOKSHOT_UI_SCALE", 0), ("GDK_SCALE", 1), ("QT_SCALE_FACTOR", 1)):
        try:
            value = float(environ.get(name, ""))
        except ValueError:
            continue
        if value > least:
            return min(max(value, 0.5), 4.0)
    if dpi:
        return min(max(dpi / 96, 0.5), 4.0)
    return min(screen_height / 1080, 4.0) if screen_height >= 1440 else 1.0


# ---- the look: the Windows builder's (builder/HookshotBuilder/App.xaml, "Slate"): its colours, Inter, its sizes in 96 DPI
# pixels, its rounded buttons, fields and cards. Tk draws no rounded corners or alpha, so they are painted into images here,
# antialiased and laid over the colour they sit on, and ttk's clam theme draws everything else ----
PAGE, RAISED, RAISED_HOVER, LINE_STRONG = "#14161a", "#1c1f24", "#23272d", "#3d434c"
INK, INK_SOFT, INK_FAINT = "#edeff2", "#a2a9b3", "#6e7681"
BRAND, BRAND_HOVER, BRAND_INK = "#3d63e6", "#4b70f0", "#dfe6ff"
GO, GO_HOVER, GO_INK, BAD = "#5fb08a", "#70c09a", "#06150e", "#e2776b"
UI_FONTS = ("Inter", "Inter Variable", "Segoe UI", "Cantarell", "Noto Sans", "DejaVu Sans", "Liberation Sans")
MONO_FONTS = ("Cascadia Mono", "JetBrains Mono", "DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono")


def mix(color, under, alpha):
    """color at alpha over under: the WPF window's #AARRGGBB glass brushes, flattened onto what is behind them"""
    if alpha >= 1:
        return color
    a, b = int(color[1:], 16), int(under[1:], 16)
    return "#%02x%02x%02x" % tuple(round(((a >> s) & 255) * alpha + ((b >> s) & 255) * (1 - alpha)) for s in (16, 8, 0))


WHITE = "#ffffff"
GLASS = mix(WHITE, PAGE, 0x0D / 255)          # cards, fields
GLASS_LINE = mix(WHITE, PAGE, 0x17 / 255)     # their edges, and a hovered quiet button
QUIET = mix(WHITE, PAGE, 0x08 / 255)          # Cancel, Change, Open folder
QUIET_LINE = mix(WHITE, PAGE, 0x1F / 255)
QUIET_HOVER_LINE = mix(WHITE, PAGE, 0x2A / 255)
TICK_LINE = mix(WHITE, PAGE, 0x40 / 255)
TRACK = mix(WHITE, PAGE, 0x12 / 255)          # a bar's empty part
LOG = mix("#000000", GLASS, 0x33 / 255)       # the build's lines


def box(x, y, w, h, r):
    """a distance function: from a point to the box at x, y of w x h with corners of radius r (negative inside)"""
    cx, cy, hx, hy = x + w / 2, y + h / 2, w / 2 - r, h / 2 - r

    def dist(px, py):
        qx, qy = abs(px - cx) - hx, abs(py - cy) - hy
        return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r
    return dist, (x, y, x + w, y + h)


def ring(cx, cy, r, width):
    def dist(px, py):
        return abs(math.hypot(px - cx, py - cy) - (r - width / 2)) - width / 2
    return dist, (cx - r, cy - r, cx + r, cy + r)


def stroke(points, width):
    """a line through points, width wide (a tick, a chevron)"""
    pairs = list(zip(points, points[1:]))

    def dist(px, py):
        best = 1e9
        for (ax, ay), (bx, by) in pairs:
            dx, dy = bx - ax, by - ay
            t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
            best = min(best, math.hypot(px - ax - t * dx, py - ay - t * dy))
        return best - width / 2
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    return dist, (min(xs) - width, min(ys) - width, max(xs) + width, max(ys) + width)


def triangle(points):
    """a filled convex triangle (Play's), its points clockwise"""
    edges = []
    for (ax, ay), (bx, by) in zip(points, points[1:] + points[:1]):
        length = math.hypot(bx - ax, by - ay)
        edges.append((ax, ay, (by - ay) / length, -(bx - ax) / length))

    def dist(px, py):
        return max((px - ax) * nx + (py - ay) * ny for ax, ay, nx, ny in edges)
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    return dist, (min(xs), min(ys), max(xs), max(ys))


def paint(w, h, under, layers):
    """Photo image data, w x h over the colour under: each layer (colour, (distance function, bounds)) laid on in turn,
    antialiased by its distance. A colour may be a function of y (the Windows step's gradient)."""
    rows = []
    for j in range(h):
        y = j + 0.5
        row = []
        for i in range(w):
            x = i + 0.5
            color = under
            for paint_color, (dist, (x0, y0, x1, y1)) in layers:
                if x < x0 - 1 or x > x1 + 1 or y < y0 - 1 or y > y1 + 1:
                    continue
                cover = 0.5 - dist(x, y)
                if cover > 0:
                    color = mix(paint_color(y) if callable(paint_color) else paint_color, color, min(cover, 1.0))
            row.append(color)
        rows.append("{%s}" % " ".join(row))
    return " ".join(rows)


def framed(w, h, r, fill, line, width):
    """the layers of a rounded box: its edge, and its fill inside it"""
    layers = [(line, box(0, 0, w, h, r))] if line and width else []
    return layers + [(fill, box(width, width, w - 2 * width, h - 2 * width, max(r - width, 0)) if line and width else
                      box(0, 0, w, h, r))]


def read_only(text):
    """text, a tk.Text, read-only as Windows' IsReadOnly box: it selects and copies (Ctrl+A, Ctrl+C) but typing, paste, cut
    and delete leave it as it is. Returns the function that adds to its end. Its insert and delete are refused in Tcl, so Tk's
    own bindings stay. (Not -state disabled: Tk 8.5 and early 8.6 give a disabled Text no focus from a click off Windows, so
    Ctrl+C would not reach it.)"""
    w = str(text)
    text.tk.call("rename", w, w + "_rw")
    text.tk.call("proc", w, "op args", "if {$op in {insert delete replace}} return\n"
                                       "uplevel 1 [list {%s_rw} $op {*}$args]" % w)
    # (Tk deletes the widget's own command when the window goes: the proc goes with it)
    text.tk.call("trace", "add", "command", w + "_rw", "delete", "apply {{w args} {rename $w {}}} {%s}" % w)
    # (Ctrl+A is line start in Tk's X11 bindings: here it selects all, as in the Windows box)
    text.bind("<Control-a>", lambda e: (text.tag_add("sel", "1.0", "end"), "break")[1])
    return lambda s: text.tk.call(w + "_rw", "insert", "end", s)


def _arg(name):
    args = sys.argv[1:]
    if name in args:
        index = args.index(name)
        if index + 1 < len(args):
            return args[index + 1]
    return None


class Builder(tk.Tk):
    def __init__(self, launch=None, update=None, fetched=False, tag=None, allow_downgrade_from=None):
        super().__init__()
        self.title("Hookshot Builder")
        self.apply_scale()
        self.apply_theme()
        self.minsize(self.px(760), self.px(520))
        move_old_ngx_sdk()   # (before the settings are read: it may record the license in them)
        self.cfg = load_config()
        self.proc = None
        self.stop_update = threading.Event()   # (Cancel while an update downloads: its thread ends the download)
        self.lines = queue.Queue()
        self.started = 0.0
        self.launch_folder = launch
        self.update_folder = update
        self.fetched = fetched
        self.update_tag = tag
        self.allow_downgrade_from = allow_downgrade_from
        self.mode = "build"
        self.latest = None
        self.source_picked = False
        self.dlss_toggled = False
        self.tools = linux_update.tools_tag(linux_update.running_root() or ROOT)
        # (the update track: the one chosen in the Track menu, saved; by default these tools' own)
        own = linux_update.track_of(self.tools)
        self.track = tk.StringVar(value=TRACK_NAMES[self.cfg.get("track") if self.cfg.get("track") in linux_update.TRACKS else own])
        self.status = tk.StringVar()
        self.kind = tk.StringVar(value=self.cfg.get("kind", "nup"))
        self.nup = tk.StringVar(value=self.cfg.get("nup", ""))
        self.romfs = tk.StringVar(value=self.cfg.get("romfs", ""))
        self.exefs = tk.StringVar(value=self.cfg.get("exefs", ""))
        self.out = tk.StringVar(value=self.cfg.get("out", os.path.expanduser("~/Games/Hookshot")))
        names = [n for _, n in make_game.LANGUAGES]
        code = self.cfg.get("language", "en-US")
        self.language = tk.StringVar(value=dict(make_game.LANGUAGES).get(code, names[0]))
        self.prepare = tk.BooleanVar(value=self.cfg.get("prepare", "1") != "0")   # (make_game.py's last step: off, at the first start)
        self.dlss = tk.BooleanVar(value=self.cfg.get("dlss", "0") == "1")
        self.check_text = tk.StringVar()
        self.space_text = tk.StringVar()
        self.stage_text = tk.StringVar()
        self.time_text = tk.StringVar()
        self.page = None
        self.build_button = None
        if update is not None:
            self.after(200, self.begin_update)
        elif launch:
            self.show_launch(launch)
        else:
            self.show_form()
            self.after(200, self.lookup_latest)
        self.after(100, self.pump)
        self.protocol("WM_DELETE_WINDOW", self.close)

    def apply_scale(self):
        """Fonts sized in points follow tk scaling; a named font sized in pixels (Tk 8.6 on X11: -12) does not, so it is
        multiplied."""
        self.scale = ui_scale(self.winfo_screenheight(), xft_dpi())
        if self.scale == 1.0:
            return
        self.tk.call("tk", "scaling", self.scale * 96 / 72)
        for name in self.tk.splitlist(self.tk.call("font", "names")):
            size = int(self.tk.call("font", "configure", name, "-size"))
            if size < 0:
                self.tk.call("font", "configure", name, "-size", -max(1, round(-size * self.scale)))

    def px(self, n):
        return int(round(n * self.scale))

    # ---- the look (the colours and images above, as ttk styles) ----
    def apply_theme(self):
        px = self.px
        self.images = {}
        self.configure(background=PAGE)
        families = {name.lower(): name for name in self.tk.splitlist(self.tk.call("font", "families"))}
        ui = next((families[n.lower()] for n in UI_FONTS if n.lower() in families), None)
        mono = next((families[n.lower()] for n in MONO_FONTS if n.lower() in families), None)
        named = set(self.tk.splitlist(self.tk.call("font", "names")))

        def font(name, size, weight="normal", family=ui):
            options = ["-size", -px(size), "-weight", weight] + (["-family", family] if family else [])
            self.tk.call("font", "configure" if name in named else "create", name, *options)
        for name, size, weight in (("HsUi", 14, "normal"), ("HsStrong", 14, "bold"), ("HsNote", 13, "normal"),
                                   ("HsNoteStrong", 13, "bold"), ("HsSmall", 12, "normal"), ("HsLabel", 12, "bold"),
                                   ("HsBrand", 15, "bold"), ("HsTitle", 24, "bold"), ("HsDisplay", 34, "bold"),
                                   ("HsPlay", 16, "bold"), ("TkDefaultFont", 13, "normal"), ("TkTextFont", 13, "normal"),
                                   ("TkMenuFont", 13, "normal"), ("TkHeadingFont", 13, "bold"), ("TkCaptionFont", 14, "bold")):
            font(name, size, weight)
        font("HsMono", 12, family=mono)
        font("HsLog", 11, family=mono)
        if mono:
            font("TkFixedFont", 12, family=mono)

        # (the classic widgets and dialogs: the file chooser, the DLSS license's question, the language list)
        for pattern, value in (("*Background", PAGE), ("*Foreground", INK), ("*selectBackground", BRAND),
                               ("*selectForeground", WHITE), ("*insertBackground", INK), ("*highlightBackground", PAGE),
                               ("*highlightColor", BRAND), ("*activeBackground", RAISED_HOVER), ("*activeForeground", INK),
                               ("*troughColor", TRACK), ("*Dialog.msg.font", "HsUi"),
                               ("*Dialog.msg.wrapLength", px(460)), ("*Dialog.dtl.font", "HsNote"),
                               ("*TCombobox*Listbox.background", RAISED), ("*TCombobox*Listbox.foreground", INK),
                               ("*TCombobox*Listbox.selectBackground", RAISED_HOVER),
                               ("*TCombobox*Listbox.selectForeground", BRAND_INK), ("*TCombobox*Listbox.font", "HsNote"),
                               ("*TCombobox*Listbox.borderWidth", 0), ("*TCombobox*Listbox.highlightThickness", 0),
                               ("*TCombobox*Listbox.relief", "flat")):
            self.option_add(pattern, value)

        s = self.style = ttk.Style(self)
        s.theme_use("clam")
        s.configure(".", background=PAGE, foreground=INK, font="HsUi", bordercolor=QUIET_LINE, lightcolor=PAGE,
                    darkcolor=PAGE, troughcolor=TRACK, focuscolor=BRAND, selectbackground=BRAND, selectforeground=WHITE,
                    fieldbackground=GLASS, insertcolor=INK, arrowcolor=INK_SOFT, borderwidth=0)
        s.map(".", foreground=[("disabled", INK_FAINT)], background=[("disabled", PAGE)])
        for name, options in (("TFrame", {}), ("Card.TFrame", {"background": GLASS}),
                              ("TLabel", {}), ("Title.TLabel", {"font": "HsTitle"}), ("Display.TLabel", {"font": "HsDisplay"}),
                              ("Sub.TLabel", {"foreground": INK_SOFT}), ("Note.TLabel", {"foreground": INK_SOFT, "font": "HsNote"}),
                              ("Small.TLabel", {"foreground": INK_SOFT, "font": "HsSmall"}),
                              ("Faint.TLabel", {"foreground": INK_FAINT, "font": "HsSmall"}),
                              ("Section.TLabel", {"foreground": INK_SOFT, "font": "HsLabel"}),
                              ("Status.TLabel", {"foreground": BRAND_INK, "font": "HsNote"}),
                              ("Stage.TLabel", {"background": GLASS, "font": "HsStrong"}),
                              ("Bad.Stage.TLabel", {"foreground": BAD}), ("Go.Stage.TLabel", {"foreground": GO}),
                              ("Card.Note.TLabel", {"background": GLASS})):
            s.configure(name, **options)

        # buttons: Quiet (also every plain ttk button, the dialogs' among them), Primary (blue: the step's action), Go (green: Play)
        def button(style, fill, hover, line, hover_line, focus_line, foreground, font, padding, radius=8, disabled=None):
            r, w = px(radius), px(1)
            n = 2 * r + 6
            image = {k: self.photo("%s-%s" % (style, k), n, n, PAGE, framed(n, n, r, f, l, w))
                     for k, f, l in (("normal", fill, line), ("hover", hover, hover_line), ("focus", fill, focus_line),
                                     ("disabled", disabled or fill, disabled or line))}
            s.element_create(style + ".box", "image", image["normal"], ("disabled", image["disabled"]),
                             ("pressed", image["hover"]), ("active", image["hover"]), ("focus", image["focus"]),
                             border=r + 2, padding=0, sticky="nsew")
            s.layout(style + ".TButton", [(style + ".box", {"sticky": "nsew", "children": [
                ("Button.padding", {"sticky": "nsew", "children": [("Button.label", {"sticky": "nsew"})]})]})])
            s.configure(style + ".TButton", foreground=foreground, font=font, padding=padding, anchor="center", background=fill)
            s.map(style + ".TButton", foreground=[("disabled", INK_FAINT)], background=[("disabled", disabled or fill)])
        button("Quiet", QUIET, GLASS_LINE, QUIET_LINE, QUIET_HOVER_LINE, BRAND_HOVER, INK, "HsStrong", (px(18), px(6)))
        button("Primary", BRAND, BRAND_HOVER, mix(WHITE, BRAND, 0x33 / 255), mix(WHITE, BRAND_HOVER, 0x33 / 255),
               mix(WHITE, BRAND, 0xAA / 255), WHITE, "HsStrong", (px(22), px(6)), disabled=mix(WHITE, PAGE, 0x14 / 255))
        button("Go", GO, GO_HOVER, mix(WHITE, GO, 0x40 / 255), mix(WHITE, GO_HOVER, 0x40 / 255), mix(WHITE, GO, 0xAA / 255),
               GO_INK, "HsPlay", (px(30), px(10)), radius=10)
        s.layout("TButton", s.layout("Quiet.TButton"))
        s.configure("TButton", **{k: s.lookup("Quiet.TButton", k) for k in ("foreground", "font", "padding", "anchor", "background")})
        s.map("TButton", foreground=[("disabled", INK_FAINT)])
        # (a text link: Open the game folder)
        s.layout("Link.TButton", [("Button.padding", {"sticky": "nsew", "children": [("Button.label", {"sticky": "nsew"})]})])
        s.configure("Link.TButton", foreground=INK_FAINT, font="HsNoteStrong", padding=0, background=PAGE)
        s.map("Link.TButton", foreground=[("active", INK)], background=[("active", PAGE)])

        # ticks and the game files' choice (the Windows Tick and DumpRow)
        b, t = px(18), px(8)
        for name, under, layers in (
                ("tick", PAGE, framed(b, b, px(5), PAGE, TICK_LINE, px(1.5))),
                ("tick-on", PAGE, framed(b, b, px(5), BRAND, BRAND, px(1.5)) +
                 [(WHITE, stroke([(px(2.5), px(8.5)), (px(6.5), px(12.5)), (px(14.5), px(3.5))], px(2)))]),
                ("radio", GLASS, [(INK_FAINT, ring(b / 2, b / 2, b / 2, px(2)))]),
                ("radio-on", GLASS, [(BRAND, ring(b / 2, b / 2, b / 2, px(2))), (BRAND, ring(b / 2, b / 2, px(4), px(4)))])):
            self.photo(name, b + px(14 if name.startswith("radio") else 8), b, under, layers)
        s.element_create("Hs.tick", "image", self.images["tick"], ("selected", self.images["tick-on"]), sticky="w")
        s.element_create("Hs.radio", "image", self.images["radio"], ("selected", self.images["radio-on"]), sticky="w")
        s.layout("TCheckbutton", [("Checkbutton.padding", {"sticky": "nsew", "children": [
            ("Hs.tick", {"side": "left", "sticky": ""}), ("Checkbutton.label", {"side": "left", "sticky": "nsew"})]})])
        s.configure("TCheckbutton", foreground=INK, font="HsNote", padding=(0, px(2)))
        s.map("TCheckbutton", background=[("active", PAGE)], foreground=[("active", WHITE)])
        s.layout("Row.TRadiobutton", [("Radiobutton.padding", {"sticky": "nsew", "children": [
            ("Hs.radio", {"side": "left", "sticky": ""}), ("Radiobutton.label", {"side": "left", "sticky": "nsew"})]})])
        s.configure("Row.TRadiobutton", background=GLASS, foreground=INK, font="HsUi", padding=(px(16), px(13)))
        s.map("Row.TRadiobutton", background=[("active", GLASS)], foreground=[("active", WHITE)])

        # fields: the paths (monospaced, as on Windows) and the language
        r, w, n = px(8), px(1), px(8) * 2 + 6
        field = self.photo("field", n, n, PAGE, framed(n, n, r, GLASS, GLASS_LINE, w))
        field_hover = self.photo("field-hover", n, n, PAGE, framed(n, n, r, GLASS, QUIET_HOVER_LINE, w))
        field_focus = self.photo("field-focus", n, n, PAGE, framed(n, n, r, GLASS, BRAND, w))
        s.element_create("Hs.field", "image", field, ("focus", field_focus), ("hover", field_hover), border=r + 2, padding=0,
                         sticky="nsew")
        s.layout("TEntry", [("Hs.field", {"sticky": "nsew", "children": [
            ("Entry.padding", {"sticky": "nsew", "children": [("Entry.textarea", {"sticky": "nsew"})]})]})])
        s.configure("TEntry", foreground=INK_SOFT, padding=(px(12), px(8)), fieldbackground=GLASS, insertwidth=max(1, px(1)))
        chevron = self.photo("chevron", px(8 + 14), px(8), GLASS,
                             [(INK_SOFT, stroke([(px(1), px(2)), (px(5), px(6)), (px(9), px(2))], px(1.4)))])
        s.element_create("Hs.chevron", "image", chevron, sticky="")
        s.layout("TCombobox", [("Hs.field", {"sticky": "nsew", "children": [
            ("Hs.chevron", {"side": "right", "sticky": ""}),
            ("Combobox.padding", {"sticky": "nsew", "children": [("Combobox.textarea", {"sticky": "nsew"})]})]})])
        s.configure("TCombobox", foreground=INK, padding=(px(12), px(7), px(4), px(7)), arrowsize=px(12))
        s.map("TCombobox", fieldbackground=[("readonly", GLASS)], foreground=[("readonly", INK)],
              selectbackground=[("readonly", GLASS)], selectforeground=[("readonly", INK)])
        s.configure("ComboboxPopdownFrame", background=RAISED, bordercolor=LINE_STRONG, relief="solid", borderwidth=1)

        # cards (the found list's, the build's steps on Windows)
        r, n = px(10), px(10) * 2 + 6
        self.photo("card", n, n, PAGE, framed(n, n, r, GLASS, GLASS_LINE, px(1)))
        s.element_create("Hs.card", "image", self.images["card"], border=r + 2, padding=0, sticky="nsew")
        s.layout("Card.TFrame", [("Hs.card", {"sticky": "nsew", "children": [("Frame.padding", {"sticky": "nsew"})]})])

        # bars: the free space (green, red when short) and a step's progress (blue), 6 px and round
        h = max(4, px(6))
        # (the parts' names end in trough and pbar: the progress bar finds the part it sizes by that name)
        for name, under, color in (("HsTrack.trough", PAGE, TRACK), ("HsGo.pbar", PAGE, GO), ("HsBad.pbar", PAGE, BAD),
                                   ("HsCardTrack.trough", GLASS, mix(WHITE, GLASS, 0x12 / 255)), ("HsCard.pbar", GLASS, BRAND)):
            self.photo(name, h + 4, h, under, [(color, box(0, 0, h + 4, h, h / 2))])
            s.element_create(name, "image", self.images[name], border=(h // 2 + 1, 0), padding=0, sticky="nsew")
        for style, track in (("Go", "HsTrack"), ("Bad", "HsTrack"), ("Card", "HsCardTrack")):
            s.layout(style + ".Horizontal.TProgressbar", [(track + ".trough", {"sticky": "nsew", "children": [
                ("Hs%s.pbar" % style, {"side": "left", "sticky": "ns"})]})])
            s.configure(style + ".Horizontal.TProgressbar", thickness=h, background=GLASS)

        # the log's scroll bar: a thumb on the log, no arrows
        s.layout("Log.Vertical.TScrollbar", [("Vertical.Scrollbar.trough", {"sticky": "ns", "children": [
            ("Vertical.Scrollbar.thumb", {"expand": "1", "sticky": "nsew"})]})])
        s.configure("Log.Vertical.TScrollbar", troughcolor=LOG, background=RAISED_HOVER, bordercolor=LOG, lightcolor=RAISED_HOVER,
                    darkcolor=RAISED_HOVER, gripcount=0, arrowsize=px(10))
        s.map("Log.Vertical.TScrollbar", background=[("active", LINE_STRONG)], lightcolor=[("active", LINE_STRONG)],
              darkcolor=[("active", LINE_STRONG)])

        # Play's triangle, and the hookshot of the window's head
        self.photo("play", px(11) + px(10), px(14), GO,
                   [(GO_INK, triangle([(px(0.5), px(0.5)), (px(11), px(7)), (px(0.5), px(13.5))]))])
        self.load_logo(px(28))

    def photo(self, name, w, h, under, layers):
        image = tk.PhotoImage(master=self, width=int(w), height=int(h))
        image.put(paint(int(w), int(h), under, layers))
        self.images[name] = image
        return image

    def load_logo(self, size):
        """assets/hookshot.png (the Windows head's and the game's icon), size pixels wide over the page: averaged down with its
        alpha on Tk 9, every nth pixel on an older Tk"""
        self.images["logo"] = None
        path = os.path.join(ROOT, "assets", "hookshot.png")
        try:
            full = tk.PhotoImage(master=self, file=path)
        except (tk.TclError, OSError):
            return
        w, h = full.width(), full.height()
        try:
            rows = [self.tk.splitlist(row) for row in
                    self.tk.splitlist(self.tk.call(full, "data", "-format", "default -colorformat rgba"))]
        except tk.TclError:
            self.images["logo"] = full.subsample(max(1, w // size))
            return
        tw, th = size, max(1, round(h * size / w))
        page = [int(PAGE[i:i + 2], 16) for i in (1, 3, 5)]
        out = []
        for j in range(th):
            y0, y1 = j * h // th, max(j * h // th + 1, (j + 1) * h // th)
            line = []
            for i in range(tw):
                x0, x1 = i * w // tw, max(i * w // tw + 1, (i + 1) * w // tw)
                acc, n = [0.0, 0.0, 0.0, 0.0], 0
                for y in range(y0, y1):
                    for x in range(x0, x1):
                        v = int(rows[y][x][1:], 16)
                        a = (v & 255) / 255
                        acc[0] += (v >> 24 & 255) * a
                        acc[1] += (v >> 16 & 255) * a
                        acc[2] += (v >> 8 & 255) * a
                        acc[3] += a
                        n += 1
                alpha = acc[3] / n
                line.append("#%02x%02x%02x" % tuple(min(255, round(acc[k] / n + page[k] * (1 - alpha))) for k in range(3)))
            out.append("{%s}" % " ".join(line))
        logo = tk.PhotoImage(master=self, width=tw, height=th)
        logo.put(" ".join(out))
        self.images["logo"] = logo

    def header(self, parent, step=None, chip=None):
        """the window's head, as Windows draws it: the hookshot and Hookshot at the left, and in the middle the steps (Game files >
        Build > Play) or, for the launcher, the game's version"""
        px = self.px
        canvas = tk.Canvas(parent, height=px(64), background=PAGE, highlightthickness=0, borderwidth=0)
        canvas.pack(fill="x")
        x = px(28)
        if self.images.get("logo") is not None:
            canvas.create_image(x, px(32), image=self.images["logo"], anchor="w")
            x += self.images["logo"].width() + px(10)
        canvas.create_text(x, px(32), text="Hookshot", font="HsBrand", fill=INK, anchor="w")
        x += int(self.tk.call("font", "measure", "HsBrand", "Hookshot"))
        if chip:
            canvas.create_text(x + px(12), px(32), text=chip, font="HsSmall", fill=INK_SOFT, anchor="w")
        self.header_canvas = canvas
        if step is not None:
            self.set_step(step)
            canvas.bind("<Configure>", lambda event: self.center_steps(event.width))
        return canvas

    def set_step(self, step):
        canvas = getattr(self, "header_canvas", None)
        if canvas is None or not canvas.winfo_exists():
            return
        canvas.delete("steps")
        pill, labels = self.step_pill(step)
        canvas.create_image(0, self.px(12), image=pill, anchor="nw", tags=("steps", "pill"))
        for lx, text, font, color in labels:
            canvas.create_text(lx, self.px(32), text=text, font=font, fill=color, anchor="w", tags="steps")
        self.center_steps(canvas.winfo_width())

    def center_steps(self, width):
        canvas = self.header_canvas
        pill = canvas.bbox("pill")
        if pill and width > 1:
            canvas.move("steps", (width - (pill[2] - pill[0])) // 2 - pill[0], 0)

    def step_pill(self, on):
        """Windows' SetStep: the steps in a pill, the one now in blue, the ones done with a green tick. One image (the shapes),
        and the labels with their x, for the canvas to write over it"""
        px = self.px
        names = ("Game files", "Build", "Play")
        container = mix(WHITE, PAGE, 0x0B / 255)
        layers, labels = [], []
        x = px(4)
        for i, name in enumerate(names):
            if i:
                layers.append((mix(INK_SOFT, container, 0.45), stroke([(x + px(5), px(16)), (x + px(9), px(20)), (x + px(5), px(24))], px(1.6))))
                x += px(14)
            font = "HsNoteStrong" if i == on else "HsNote"
            width = int(self.tk.call("font", "measure", font, name))
            done = i < on
            pill_w = (px(4) + px(22) + px(8) if done else px(14)) + width + px(14)
            if i == on:
                layers.append((lambda y: mix("#3557d6", "#4b70f0", min(1, max(0, (y - px(5)) / px(30)))),
                               box(x, px(5), pill_w, px(30), px(15))))
            if done:
                cx, cy = x + px(4) + px(11), px(20)
                layers.append((mix(GO, container, 0x26 / 255), box(cx - px(11), cy - px(11), px(22), px(22), px(11))))
                layers.append((GO, stroke([(cx - px(5), cy), (cx - px(1.5), cy + px(3.5)), (cx + px(5), cy - px(3.5))], px(1.8))))
            text_x = x + (px(4) + px(22) + px(8) if done else px(14))
            labels.append((text_x, name, font, WHITE if i == on else INK_SOFT if done else INK_FAINT))
            x += pill_w
        width = x + px(4)
        image = self.photo("steps-%d" % on, width, px(40), PAGE,
                           framed(width, px(40), px(20), container, GLASS_LINE, px(1)) + layers)
        return image, labels

    def section(self, parent, text, row, column=0, columnspan=1, top=24):
        """the Windows builder's section labels: FOUND ON THIS PC, INSTALL THE GAME TO"""
        label = ttk.Label(parent, text=text.upper(), style="Section.TLabel")
        label.grid(row=row, column=column, columnspan=columnspan, sticky="w", pady=(self.px(top), self.px(10)))
        return label

    # ---- the form ----
    def show_form(self):
        self.title("Hookshot Builder")
        if self.page:
            self.page.destroy()
        px = self.px
        page = self.page = ttk.Frame(self)
        page.pack(fill="both", expand=True)
        self.header(page, step=0)
        f = ttk.Frame(page, padding=(px(48), px(6), px(48), px(28)))
        f.pack(fill="both", expand=True)
        f.columnconfigure(0, weight=1, minsize=px(580))   # (every page px(800) wide: the window keeps its width, as on Windows)
        f.columnconfigure(1, minsize=px(20))
        f.columnconfigure(2, minsize=px(200))
        ttk.Label(f, text="Build your game", style="Title.TLabel").grid(row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(f, text="Pick your game files and where the game goes. Hookshot does the rest, once.", style="Sub.TLabel",
                  wraplength=px(800)).grid(row=1, column=0, columnspan=3, sticky="w", pady=(px(6), 0))
        if game_ready(self.cfg.get("out")):
            played = ttk.Frame(f)
            played.grid(row=2, column=0, columnspan=3, sticky="w", pady=(px(16), 0))
            ttk.Button(played, text="Play the game built in %s" % self.cfg["out"],
                       command=lambda: self.play_and_close(self.cfg["out"])).pack(side="left")
            self.share_row(played, self.cfg["out"], wrap=px(360)).pack(side="left", padx=(px(12), 0))
        self.section(f, "Game files", 3, columnspan=3)
        kinds = ttk.Frame(f, style="Card.TFrame", padding=px(5))
        kinds.grid(row=4, column=0, columnspan=3, sticky="we")
        kinds.columnconfigure(0, weight=1)
        ttk.Radiobutton(kinds, text=".nup file", value="nup", variable=self.kind, command=self.show_form,
                        style="Row.TRadiobutton", cursor="hand2").grid(row=0, column=0, sticky="we")
        tk.Frame(kinds, height=max(1, px(1)), background=GLASS_LINE).grid(row=1, column=0, sticky="we", padx=px(12))
        ttk.Radiobutton(kinds, text="romfs and exefs folders", value="folders", variable=self.kind, command=self.show_form,
                        style="Row.TRadiobutton", cursor="hand2").grid(row=2, column=0, sticky="we")
        paths = ttk.Frame(f)
        paths.grid(row=5, column=0, columnspan=3, sticky="we", pady=(px(10), 0))
        paths.columnconfigure(1, weight=1)
        if self.kind.get() == "nup":
            self.path_row(paths, 0, ".nup file", self.nup, lambda: self.pick_file(self.nup))
        else:
            self.path_row(paths, 0, "romfs", self.romfs, lambda: self.pick_dir(self.romfs))
            self.path_row(paths, 1, "exefs", self.exefs, lambda: self.pick_dir(self.exefs))
        ttk.Label(f, textvariable=self.check_text, style="Small.TLabel", wraplength=px(800)).grid(
            row=6, column=0, columnspan=3, sticky="w", pady=(px(6), 0))

        self.section(f, "Install the game to", 7, top=18)
        self.section(f, "Game language", 7, column=2, top=18)
        out = ttk.Frame(f)
        out.grid(row=8, column=0, sticky="we")
        out.columnconfigure(1, weight=1)
        self.path_row(out, 0, None, self.out, lambda: self.pick_dir(self.out), button="Change")
        ttk.Combobox(f, textvariable=self.language, values=[n for _, n in make_game.LANGUAGES], state="readonly",
                     font="HsNote", cursor="hand2").grid(row=8, column=2, sticky="we")
        # (the update track: Release, Release candidate or Nightly; the builder updates from it, saved once chosen)

        options = ttk.Frame(f)
        options.grid(row=9, column=0, sticky="we", pady=(px(22), 0))
        # (the update track: Release, Release candidate or Nightly. The builder updates from it; saved once chosen)
        tracks = ttk.Frame(f)
        tracks.grid(row=9, column=2, sticky="nwe", pady=(px(22), 0))
        tracks.columnconfigure(0, weight=1)
        self.section(tracks, "Update track", 0, top=0)
        track = ttk.Combobox(tracks, textvariable=self.track, values=list(TRACK_NAMES.values()), state="readonly", font="HsNote",
                             cursor="hand2")
        track.grid(row=1, column=0, sticky="we")
        track.bind("<<ComboboxSelected>>", self.on_track_changed)
        ttk.Checkbutton(options, text="Prepare shaders now", variable=self.prepare, cursor="hand2").pack(anchor="w")
        ttk.Label(options, text="A few minutes at the end of the build. Unticked, the game prepares them at its first start, "
                  "behind a black window.", style="Small.TLabel", wraplength=px(540)).pack(anchor="w", padx=(px(26), 0), pady=(px(2), 0))
        ttk.Checkbutton(options, text="NVIDIA DLSS (downloads NVIDIA's SDK under NVIDIA's license)", variable=self.dlss,
                        command=self.on_dlss_changed, cursor="hand2").pack(anchor="w", pady=(px(12), 0))
        ttk.Label(options, text="For NVIDIA RTX graphics cards. NVIDIA's files stay on this PC: never share a game folder built with them.",
                  style="Small.TLabel", wraplength=px(540)).pack(anchor="w", padx=(px(26), 0), pady=(px(2), 0))

        f.rowconfigure(10, weight=1, minsize=px(18))
        ttk.Label(f, textvariable=self.status, style="Status.TLabel", wraplength=px(800)).grid(
            row=11, column=0, columnspan=3, sticky="w", pady=(0, px(12)))
        foot = ttk.Frame(f)
        foot.grid(row=12, column=0, columnspan=3, sticky="we")
        foot.columnconfigure(1, weight=1)
        self.space_bar = ttk.Progressbar(foot, maximum=1.0, length=px(160), style="Go.Horizontal.TProgressbar")
        self.space_bar.grid(row=0, column=0)
        ttk.Label(foot, textvariable=self.space_text, style="Small.TLabel").grid(row=0, column=1, sticky="w", padx=(px(10), 0))
        self.about_label = ttk.Label(foot, text="About 45 min", style="Faint.TLabel")
        self.about_label.grid(row=0, column=2, padx=(px(14), px(14)))
        self.build_button = ttk.Button(foot, text="Build the game", command=self.build, style="Primary.TButton", cursor="hand2")
        self.build_button.grid(row=0, column=3)
        self.out.trace_add("write", lambda *a: self.update_space())
        self.update_space()
        self.apply_mode()

    def share_row(self, parent, folder, wrap=None):
        """Save my pipeline list (share_pipeline_list), and under it what it did"""
        box = ttk.Frame(parent)
        said = tk.StringVar(value="")
        note = ttk.Label(box, textvariable=said, style="Small.TLabel", wraplength=wrap or self.px(600), justify="center")

        def save():
            button.state(["disabled"])   # (one save at a time)
            said.set("Saving...")
            note.pack(pady=(self.px(2), 0))
            self.update_idletasks()
            try:
                said.set(share_pipeline_list(folder, copy=self.copy_text))
            finally:
                button.state(["!disabled"])

        button = ttk.Button(box, text="Save my pipeline list (helps remove stutter)", style="Link.TButton", cursor="hand2",
                            command=save)
        button.pack()
        return box

    def copy_text(self, text):
        self.clipboard_clear()
        self.clipboard_append(text)
        self.update()   # (the clipboard is the window's: kept while the builder is open)

    def path_row(self, f, row, label, var, browse, button="Browse…"):
        if label:
            ttk.Label(f, text=label, style="Small.TLabel").grid(row=row, column=0, sticky="w", padx=(0, self.px(12)))
        ttk.Entry(f, textvariable=var, font="HsMono").grid(row=row, column=1, sticky="we", pady=self.px(3))
        ttk.Button(f, text=button, command=browse, cursor="hand2").grid(row=row, column=2, padx=(self.px(10), 0), pady=self.px(3))

    def pick_file(self, var):
        p = filedialog.askopenfilename(title="Your game's .nup", filetypes=[("Switch 2 game package", "*.nup"), ("All files", "*")])
        if p:
            var.set(p)
            self.source_picked = True
            self.apply_mode()
            self.check_version()

    def pick_dir(self, var):
        p = filedialog.askdirectory(title="Choose a folder", initialdir=var.get() or os.path.expanduser("~"))
        if p:
            var.set(p)
            if var is self.exefs:
                self.source_picked = True
                self.apply_mode()
                self.check_version()

    def update_space(self):
        gb = free_gb(self.out.get())
        self.space_text.set("Needs 45 GB · %.0f GB free there" % gb if gb is not None else "Needs 45 GB")
        bar = getattr(self, "space_bar", None)   # (as on Windows: the 45 GB's share of what is free, green when it fits)
        try:
            if bar is not None and bar.winfo_exists():
                bar.configure(value=min(1.0, 45 / max(gb, 0.1)) if gb is not None else 0,
                              style="Go.Horizontal.TProgressbar" if gb is None or gb >= 45 else "Bad.Horizontal.TProgressbar")
        except tk.TclError:
            pass
        self.apply_mode()

    def on_dlss_changed(self):
        if self.dlss.get() and not messagebox.askyesno("NVIDIA DLSS", DLSS_LICENSE):
            self.dlss.set(False)
        else:
            self.dlss_toggled = True
            if self.dlss.get():
                self.cfg["nvidia_license"] = "1"   # (kept apart from the SDK, which is in the cache: folder_has_dlss)
        self.cfg["dlss"] = "1" if self.dlss.get() else "0"
        save_config(self.cfg)
        self.apply_mode()

    def check_version(self):
        args = ["--nup", self.nup.get()] if self.kind.get() == "nup" else ["--exefs", self.exefs.get()]
        self.check_text.set("Checking the game version...")

        def run():
            r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py"), "--check"] + args,
                               capture_output=True, text=True, cwd=ROOT)
            said = []
            for l in r.stdout.splitlines():
                t = make_game.reported(l)
                if t is not None:
                    said.append(t)
            tail = [l for l in (r.stderr or "").splitlines() if l.strip()]
            text = said[-1] if said else (tail[-1] if tail else "The version could not be read.")
            self.lines.put(("check", text))
        threading.Thread(target=run, daemon=True).start()

    def lookup_latest(self):
        def run():
            forced = os.environ.get("HOOKSHOT_LATEST_TAG")
            tag = None if forced == "none" else (forced or linux_update.latest_tag(track=track))
            self.lines.put(("latest", tag or ""))
        track = self.track_id()
        threading.Thread(target=run, daemon=True).start()

    def apply_mode(self):
        if self.launch_folder and self.build_button is None:
            self.apply_launch()
            return
        folder = self.out.get().strip()
        expanded = os.path.expanduser(folder) if folder else ""
        game, kept = linux_update.game_version(expanded) if expanded else (None, False)
        self.mode = linux_update.choose_mode(game, self.tools, self.latest, self.source_picked, kept, self.switching())
        dlss_changed = bool(kept and game and not self.source_picked and getattr(self, "dlss_toggled", False) and
                            hasattr(self, "dlss") and bool(self.dlss.get()) != folder_has_dlss(expanded))
        if self.mode == "up-to-date" and dlss_changed:
            self.mode = "update-game"
        button = self.build_button
        if button is None:
            return
        try:
            if not button.winfo_exists():
                return
        except tk.TclError:
            return
        button.configure(text={"update-game": "Update", "up-to-date": "Play", "update-builder": "Update builder",
                                "build": "Build the game"}[self.mode])
        about = getattr(self, "about_label", None)   # (Windows: the time only for a build)
        try:
            if about is not None and about.winfo_exists():
                about.configure(text="About 45 min" if self.mode == "build" else "")
        except tk.TclError:
            pass
        target = linux_update.update_target(self.latest, self.tools, self.switching())
        if self.mode == "update-game" and dlss_changed and game == target:
            self.status.set("Update builds your game again %s NVIDIA DLSS."
                            % ("with" if self.dlss.get() else "without"))
        elif self.mode == "update-game":
            self.status.set("Hookshot %s is available. Your game is %s. Update downloads only what changed, then compiles what changed."
                            % (target, game))
        elif self.mode == "up-to-date" and linux_update.older_version(target, game):
            self.status.set("Your game is %s, newer than this builder's update target %s. Play keeps this version; "
                            "choose another update track to change versions." % (game, target))
        elif self.mode == "up-to-date" and self.latest is None:
            self.status.set("Your game is %s, as this builder. GitHub did not answer, so newer versions could not be checked." % game)
        elif self.mode == "up-to-date":
            self.status.set("%s is the latest version." % game)
        elif self.mode == "update-builder":
            self.status.set("Hookshot Builder %s is available (this one is %s). Update it first, then build." % (target, self.tools or "this one"))
        else:
            self.status.set("")

    def apply_launch(self):
        if not self.launch_folder or not hasattr(self, "launch_status"):
            return
        game, kept = linux_update.game_version(self.launch_folder)
        target = linux_update.update_target(self.latest, self.tools, self.switching())
        show = False
        if not self.switching() and linux_update.older_version(target, game):
            self.launch_status.set("Your game is %s, newer than this builder's update target %s. Play keeps this version." % (game, target))
        elif target and game and game != target and kept:
            self.launch_status.set("Update available: %s" % target)
            show = True
        elif target and game and game != target:
            self.launch_status.set("%s is out. Build this folder again with Hookshot Builder to get it." % target)
        elif game and self.latest is None:
            self.launch_status.set("Up to date with this builder (GitHub did not answer)")
        elif game:
            self.launch_status.set("Up to date")
        else:
            self.launch_status.set("Could not check for updates (GitHub did not answer)" if self.latest is None else "Hookshot")
        if hasattr(self, "launch_update"):
            self.launch_update.pack(side="right", padx=self.px(4)) if show else self.launch_update.pack_forget()

    def show_launch(self, folder):
        if self.page:
            self.page.destroy()
        self.title("Hookshot")
        px = self.px
        page = self.page = ttk.Frame(self)
        page.pack(fill="both", expand=True)
        game, _kept = linux_update.game_version(folder)
        self.header(page, chip=game)
        # (the Windows launcher's page: the game's name in the middle, Update beside Play when there is one, the folder below)
        f = ttk.Frame(page, padding=(px(48), px(24), px(48), px(56)))
        f.pack(fill="both", expand=True)
        ttk.Frame(f, width=px(800), height=0).pack()
        ttk.Label(f, text="THE LEGEND OF ZELDA", style="Section.TLabel").pack()
        ttk.Label(f, text="Ocarina of Time", style="Display.TLabel").pack(pady=(px(4), 0))
        ttk.Label(f, text=folder, style="Sub.TLabel", wraplength=px(600), justify="center").pack(pady=(px(8), 0))
        self.launch_status = tk.StringVar(value="Checking for updates...")
        ttk.Label(f, textvariable=self.launch_status, style="Note.TLabel", wraplength=px(600), justify="center").pack(pady=(px(16), 0))
        buttons = ttk.Frame(f)
        buttons.pack(pady=(px(28), 0))
        self.launch_update = ttk.Button(buttons, text="Update", style="Primary.TButton", cursor="hand2",
                                        command=lambda: self.begin_update(folder))
        ttk.Button(buttons, text="Play", style="Go.TButton", image=self.images["play"], compound="left", cursor="hand2",
                   command=lambda: self.play_and_close(folder)).pack(side="right", padx=px(4))
        ttk.Button(f, text="Open the game folder", style="Link.TButton", cursor="hand2",
                   command=lambda: subprocess.Popen(["xdg-open", folder])).pack(pady=(px(22), 0))
        self.share_row(f, folder).pack(pady=(px(4), 0))
        self.lookup_latest()

    def track_id(self):
        return next((k for k, v in TRACK_NAMES.items() if v == self.track.get()), "release")

    def switching(self):
        """(a Track other than these tools' own: its newest is installed, even an older version)"""
        return self.track_id() != linux_update.track_of(self.tools)

    def on_track_changed(self, _event=None):
        self.cfg["track"] = self.track_id()
        save_config(self.cfg)
        self.latest = None
        self.apply_mode()
        self.lookup_latest()

    def older_ok(self, target, have):
        """Ask before any known downgrade, including a rebuild after changing DLSS on the same track."""
        if not linux_update.older_version(target, have):
            return True
        return messagebox.askyesno("Hookshot", "%s is older than %s, the version you have now. Install %s?" % (target, have, target))

    def begin_update(self, folder=None):
        folder = self.update_folder if folder is None else folder
        target = self.update_tag or linux_update.update_target(self.latest, self.tools, self.switching())
        have = linux_update.game_version(folder)[0] if folder else None
        approved = bool(have and self.fetched and self.tools == self.update_tag == target and
                        getattr(self, "allow_downgrade_from", None) == have)
        if folder and not approved and not self.older_ok(target, have):
            return   # (before changing pages or starting any download/build, including the launcher button)
        self.allow_downgrade_from = None   # (the handoff's consent is for this attempt and these two versions)
        downgrade_from = have if linux_update.older_version(target, have) else None
        if folder is not None:
            self.update_folder = folder
        self.show_progress("Updating your game" if self.update_folder else "Updating Hookshot Builder")
        folder = self.update_folder
        # (a game older than this builder is built again with it: nothing to download, and GitHub need not answer. So is one
        # from a builder that is not an older release, e.g. from the Actions tab: issue 902)
        fetched = self.fetched or bool(folder and self.tools and target == self.tools)
        stop = self.stop_update
        stop.clear()

        def run():
            try:
                if fetched and target and self.tools != target:
                    raise RuntimeError("This Builder is %s, not the requested %s. Start that version's Builder to update." %
                                       (self.tools or "unversioned", target))
                if not fetched:
                    root = linux_update.running_root()
                    if root is None:
                        raise RuntimeError("These tools are a source checkout, so they cannot download a release. Start the AppImage to update.")
                    # (another track's build, or a Track switch: that version's own list)
                    named = target if target and (self.switching() or linux_update.track_of(target) != "release") else None
                    manifest = linux_update.startup_manifest(root, get=lambda url: linux_update.fetch_bytes(url, 30, stop),
                                                             tag=named)
                    # The API and its download list can disagree right after a release. Check even when the list
                    # names the running tools: otherwise a newer target silently rebuilds with those older tools.
                    if target and manifest.tag != target:
                        raise RuntimeError("GitHub offered %s, not %s: its download links can lag right after a release. "
                                           "Try Update again in a few minutes." % (manifest.tag, target))
                    if manifest.tag != linux_update.tools_tag(root):
                        source =linux_update.read_manifest(root)
                        apprun = linux_update.install(manifest, root, source, note=lambda message: self.lines.put(("note", message)),
                                                      stop=stop)
                        for name in ("APPDIR", "APPIMAGE", "ARGV0", "OWD", "LD_LIBRARY_PATH"):
                            os.environ.pop(name, None)
                        args = [apprun]
                        if folder:
                            args += ["--update", folder, "--tag", manifest.tag, "--fetched"]
                            if downgrade_from:
                                args += ["--allow-downgrade-from", downgrade_from]
                        os.execv(apprun, args)
                if stop.is_set():
                    raise linux_update.Cancelled()
                self.lines.put(("do-build", folder or ""))
            except Exception as error:
                self.lines.put(("update-error", str(error)))

        threading.Thread(target=run, daemon=True).start()

    def start_make_update(self, folder):
        if not folder:
            self.show_form()
            self.status.set("This builder is the latest version.")
            self.lookup_latest()
            return
        dlss = bool(self.dlss.get()) if getattr(self, "dlss_toggled", False) else folder_has_dlss(folder)
        cmd = [sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py"), "--update", folder] + \
            ([] if self.prepare.get() else ["--no-prepare-shaders"]) + dlss_args(dlss)   # (as the last build chose)
        self.heading.configure(text="Updating your game")
        self.stage_text.set("Compiling what changed")
        self.started = time.time()
        self.proc = subprocess.Popen(cmd, cwd=ROOT, env=make_game_env(dlss), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1, encoding="utf-8", errors="replace", start_new_session=True)
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()
        self.tick()

    # ---- the build ----
    def build(self):
        mode = getattr(self, "mode", "build")
        if mode == "up-to-date":
            folder = os.path.expanduser(self.out.get().strip())
            if game_ready(folder):
                self.play_and_close(folder)
            return
        if mode == "update-builder":
            if not self.older_ok(linux_update.update_target(self.latest, self.tools, self.switching()), self.tools):
                return
        if mode == "update-game":
            self.fetched = False
            self.begin_update(os.path.expanduser(self.out.get().strip()))
            return
        if mode == "update-builder":
            self.fetched = False
            self.update_folder = ""
            self.begin_update("")
            return
        out = os.path.expanduser(self.out.get().strip())
        if self.kind.get() == "nup":
            if not os.path.isfile(self.nup.get()):
                return messagebox.showerror("Hookshot", "Choose your game's .nup file.")
            src = ["--nup", self.nup.get()]
        else:
            if not (os.path.isdir(self.romfs.get()) and os.path.isdir(self.exefs.get())):
                return messagebox.showerror("Hookshot", "Choose your game's romfs and exefs folders.")
            src = ["--romfs", self.romfs.get(), "--exefs", self.exefs.get()]
        if not out:
            return messagebox.showerror("Hookshot", "Choose where to save the game.")
        try:
            jobs = make_game.requested_jobs(os.environ.get("HOOKSHOT_JOBS"))
            if jobs is None and os.environ.get("RX_JOBS"):   # (the older setting, still honoured)
                jobs = toolchain.default_jobs()
        except ValueError as error:
            return messagebox.showerror("Hookshot", str(error))
        code = next((c for c, n in make_game.LANGUAGES if n == self.language.get()), "en-US")
        dlss = bool(self.dlss.get()) if hasattr(self, "dlss") else False
        self.cfg.update(kind=self.kind.get(), nup=self.nup.get(), romfs=self.romfs.get(), exefs=self.exefs.get(), out=out, language=code,
                        prepare="1" if self.prepare.get() else "0", **({"dlss": "1" if dlss else "0"} if hasattr(self, "dlss") else {}))
        save_config(self.cfg)
        # (issue 531) Without HOOKSHOT_JOBS or RX_JOBS, make_game chooses as on Windows, from the memory free now; this window chose
        # fewer, 80% of the total memory at 3.5 GB each, and the CPU sat at 29%
        cmd = [sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py")] + src + ["--out", out, "--language", code] + \
            ([] if jobs is None else ["--jobs", str(jobs)]) + ([] if self.prepare.get() else ["--no-prepare-shaders"]) + \
            (dlss_args(True) if dlss else [])
        slow = slow_disk_warning(out)   # (a warning, not a stop: a hard drive still builds, only much slower)
        if slow is not None and not messagebox.askokcancel("Hookshot", slow):
            return
        self.show_progress()
        self.started = time.time()
        env_kw = {"env": make_game_env(dlss)} if (dlss or "HOOKSHOT_NGX_SDK" in os.environ) else {}
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1, encoding="utf-8", errors="replace", start_new_session=True, **env_kw)
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()
        self.tick()

    def read(self, proc):
        for line in proc.stdout:
            self.lines.put(("line", line.rstrip("\n")))
        self.lines.put(("exit", proc.wait()))

    def show_progress(self, title="Building your game"):
        if self.page:
            self.page.destroy()
        self.changed, self.compiled_files = -1, 0
        self.eta = None
        self.compile_started = None
        self.times = None
        self.shaders_later = False
        self.built = self.error = None   # (this run's own: what make_game.py says, or Cancel)
        px = self.px
        page = self.page = ttk.Frame(self)
        page.pack(fill="both", expand=True)
        self.header(page, step=1)
        f = ttk.Frame(page, padding=(px(48), px(6), px(48), px(28)))
        f.pack(fill="both", expand=True)
        f.columnconfigure(0, weight=1, minsize=px(800))
        f.rowconfigure(3, weight=1)
        self.heading = ttk.Label(f, text=title, style="Title.TLabel")
        self.heading.grid(row=0, column=0, sticky="w")
        ttk.Label(f, text="This takes a while, and your PC stays usable.", style="Sub.TLabel").grid(
            row=1, column=0, sticky="w", pady=(px(6), 0))
        # (the step, its bar and the clock in a card, as Windows' steps; the lines make_game.py prints below it)
        card = ttk.Frame(f, style="Card.TFrame", padding=(px(20), px(16)))
        card.grid(row=2, column=0, sticky="we", pady=(px(22), px(14)))
        card.columnconfigure(0, weight=1)
        self.stage_label = ttk.Label(card, textvariable=self.stage_text, style="Stage.TLabel", wraplength=px(760))
        self.stage_label.grid(row=0, column=0, sticky="w")
        self.bar = ttk.Progressbar(card, maximum=1.0, mode="determinate", style="Card.Horizontal.TProgressbar")
        self.bar.grid(row=1, column=0, sticky="we", pady=(px(12), px(10)))
        ttk.Label(card, textvariable=self.time_text, style="Card.Note.TLabel").grid(row=2, column=0, sticky="w")
        lines = tk.Frame(f, background=LOG, highlightthickness=max(1, px(1)), highlightbackground=GLASS_LINE,
                         highlightcolor=GLASS_LINE)
        lines.grid(row=3, column=0, sticky="nsew")
        lines.columnconfigure(0, weight=1)
        lines.rowconfigure(0, weight=1)
        self.log = tk.Text(lines, height=12, wrap="none", font="HsLog", background=LOG, foreground=INK_SOFT,
                           borderwidth=0, highlightthickness=0, padx=px(10), pady=px(8), insertbackground=INK,
                           selectbackground=BRAND, selectforeground=WHITE)
        self.add_log = read_only(self.log)
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(lines, orient="vertical", command=self.log.yview, style="Log.Vertical.TScrollbar")
        scroll.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scroll.set)
        self.buttons = ttk.Frame(f)
        self.buttons.grid(row=4, column=0, sticky="e", pady=(px(14), 0))
        ttk.Button(self.buttons, text="Cancel", command=self.cancel, cursor="hand2").pack(side="right")
        self.stage_text.set("Starting")

    def tick(self):
        if self.proc and self.proc.poll() is None:
            s = int(time.time() - self.started)
            text = "Elapsed %d:%02d:%02d" % (s // 3600, s // 60 % 60, s % 60)
            if self.compile_started:
                c = int(time.time() - self.compile_started)
                text += " · compiling %d:%02d" % (c // 60, c % 60)
            if self.eta is not None:
                text += " · " + left_text(self.eta)
            self.time_text.set(text)
            self.after(1000, self.tick)

    def pump(self):
        try:
            while True:
                kind, v = self.lines.get_nowait()
                if kind == "check":
                    self.check_text.set(v)
                elif kind == "latest":
                    self.latest = v or None
                    self.apply_mode()
                elif kind == "note":
                    if hasattr(self, "stage_text") and not self.stop_update.is_set():
                        self.stage_text.set(v)
                elif kind == "do-build":
                    if self.stop_update.is_set():   # (Cancel after the thread's last look at it: no make_game.py)
                        self.lines.put(("update-error", str(linux_update.Cancelled())))
                    else:
                        self.start_make_update(v)
                elif kind == "update-error":
                    if hasattr(self, "stage_text"):
                        self.stage_text.set(v + " Your game folder is unchanged.")
                    if hasattr(self, "buttons"):
                        for widget in self.buttons.winfo_children():
                            widget.destroy()
                        ttk.Button(self.buttons, text="Back", command=self.show_form, cursor="hand2").pack(side="right", padx=self.px(4))
                    if hasattr(self, "stage_label"):
                        self.stage_label.configure(style="Bad.Stage.TLabel")
                elif kind == "line":
                    self.on_line(v)
                elif kind == "exit":
                    self.on_exit(v)
        except queue.Empty:
            pass
        self.after(100, self.pump)

    def on_line(self, line):
        if line.startswith("@@stage "):
            stage = line.split(" ", 2)[1]
            self.stage_text.set(STAGES.get(stage, line.split(" ", 2)[-1]))
            self.bar.configure(mode="determinate", value=0)
            # (the compile's clock and countdown are the compile's: a later step, the minutes-long shaders one, without them)
            self.compile_started = time.time() if stage == "compile" else None
            self.eta = None
        elif line.startswith("@@progress "):
            try:
                self.bar.configure(value=float(line.split()[1]))
            except ValueError:
                pass
        elif line.startswith("@@plan "):
            parts = line.split(" ")
            if len(parts) == 3:
                try:
                    self.changed, total = int(parts[1]), int(parts[2])
                except ValueError:
                    pass
                else:
                    self.stage_text.set("Compiling: everything is already built" if not self.changed else
                                        "Compiling %d changed files of %d" % (self.changed, total) if self.changed < total else
                                        "Compiling %d files" % total)
            return
        elif line.startswith("@@file "):
            parts = line.split(" ")
            if len(parts) == 4 and parts[3] in ("new", "again"):
                self.compiled_files += 1
                if self.changed > 0:
                    self.stage_text.set("Compiling %d of %d changed files" % (self.compiled_files, self.changed))
            return
        elif line.startswith("@@eta "):
            try:
                self.eta = float(line.split()[1])
            except (ValueError, IndexError):
                pass
            return
        elif line.startswith("@@times "):
            parts = line.split(" ")
            if len(parts) == 4:
                try:
                    self.times = (float(parts[1]), float(parts[2]), float(parts[3]))
                except ValueError:
                    pass
            return
        elif line.startswith("@@done "):
            self.built = line[7:]
        elif line.startswith("@@error "):
            self.error = line[8:]
        elif line.startswith(make_game.NOT_PREPARED):
            self.shaders_later = True
        if hasattr(self, "log"):
            self.add_log(line + "\n")
            self.log.see("end")

    def on_exit(self, code):
        self.proc = None
        self.time_text.set("")   # (the clock and its countdown are a running build's; the line above says how it ended)
        for w in self.buttons.winfo_children():
            w.destroy()
        folder = getattr(self, "built", None)
        if code == 0 and folder and game_ready(folder):
            s = int(time.time() - self.started)
            text = "Your game is ready, built in %d:%02d" % (s // 3600, s // 60 % 60)
            if self.times:
                total, compiled, previous = self.times
                text += ", compiled in %d:%02d" % (int(compiled) // 60, int(compiled) % 60)
                if previous > 0:
                    text += " · last time %d:%02d" % (int(previous) // 3600, int(previous) // 60 % 60)
            if self.shaders_later:
                text += " · the shaders are prepared at its first start"
            self.stage_text.set(text)
            self.bar.configure(value=1.0)
            try:
                add_to_menu(folder)
            except (OSError, ValueError):   # (no menu entry; the game is built)
                pass
            ttk.Button(self.buttons, text="Play", style="Go.TButton", image=self.images["play"], compound="left", cursor="hand2",
                       command=lambda: self.play_and_close(folder)).pack(side="right", padx=self.px(4))
            ttk.Button(self.buttons, text="Open folder", cursor="hand2",
                       command=lambda: subprocess.Popen(["xdg-open", folder])).pack(side="right", padx=self.px(4))
            self.heading.configure(text="Your game is ready")
            self.stage_label.configure(style="Go.Stage.TLabel")
            self.set_step(2)
        else:
            self.stage_text.set("The build stopped: %s" % (getattr(self, "error", None) or "see the lines above"))
            ttk.Button(self.buttons, text="Back", command=self.show_form, cursor="hand2").pack(side="right", padx=self.px(4))
            self.heading.configure(text="The build stopped")
            self.stage_label.configure(style="Bad.Stage.TLabel")

    def cancel(self):
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            self.error = "Build cancelled. You can start again from the same folder."
        elif self.proc is None:   # (an update still downloading: there is no make_game.py yet, its thread ends the download)
            self.stop_update.set()
            self.stage_text.set("Stopping the update")

    def play_and_close(self, folder):
        play(folder)
        self.destroy()

    def close(self):
        if self.proc and self.proc.poll() is None:
            if not messagebox.askyesno("Hookshot", "Stop the build and close?"):
                return
            os.killpg(self.proc.pid, signal.SIGTERM)
        self.destroy()


def main():
    if "--play" in sys.argv[1:]:
        folder = _arg("--play") or load_config().get("out")
        if game_ready(folder):
            play(folder)
            return
    launch = _arg("--launch")
    update = _arg("--update")
    if update is not None:
        Builder(update=update, fetched="--fetched" in sys.argv[1:], tag=_arg("--tag"),
                allow_downgrade_from=_arg("--allow-downgrade-from")).mainloop()
    elif launch:
        Builder(launch=launch).mainloop()
    else:
        Builder().mainloop()


if __name__ == "__main__":
    main()
