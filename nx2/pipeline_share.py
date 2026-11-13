"""Save my pipeline list: the zip a player attaches to the "Submit your pipelines" issue (#2034), so that the pipelines their play
built go into the list the next release builds before the game (nx2/pipeline_states.py, assets/pipeline_states/README.md).

The zip holds two files and nothing else: the game folder's hookshot_pipeline_states.bin (checked first as
nx2/pipeline_states.py --check checks a list: whole, of a format this build reads, holding pipelines) and
hookshot_pipeline_report.txt, the issue's template filled in from the game folder: the version, the OS, the GPU and its driver
(from the game's last log), the output and render resolution, the upscaler, frame generation and HDR (from
hookshot_options.ini) and the mods found in its mods folder. Where the player played is left for them to fill in. Nothing is
uploaded: the player attaches the zip to the issue and pastes the template themselves.

The game's Insert menu does the same in C++ (Debug tab, Save my pipeline list: gpu/vk/pipeline_share.h, vk_menu.cpp), and
both builders through this file: the Linux one imports it (builder/linux/hookshot_builder.py), the Windows one runs it
(builder/HookshotBuilder/MainWindow.xaml.cs, --json).

    python -I nx2/pipeline_share.py <game folder> [--out-dir <folder>] [--json]
        the zip, hookshot-pipelines-<date>.zip, in --out-dir (default: the Downloads folder); prints its path and the template
        (--json: one line, {"ok": true, "zip": ..., "template": ..., "pipelines": n} or {"ok": false, "error": ...})
"""
import argparse
import datetime
import json
import lzma
import os
import platform
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pipeline_states  # noqa: E402  (the list's checks: parse, strict)

ISSUE = 2034
ISSUE_URL = "https://github.com/NotRexed/Hookshot/issues/%d" % ISSUE
LIST = "hookshot_pipeline_states.bin"
REPORT = "hookshot_pipeline_report.txt"
# (the issue's template, its lines in its order; gpu/vk/pipeline_share.h writes the same: tests/test_pipeline_share.py)
FIELDS = ("Hookshot version", "OS, GPU and driver", "Output resolution", "Upscaler + render resolution", "Frame generation / HDR",
          "Mods or texture packs", "Where you played (areas, dungeons, age, night/rain, ...)", "Pipelines")
WHERE = FIELDS[6]
NOTE = ("Attach this zip to a comment on %s and paste the lines below into it, with where you played filled in.\n"
        "The zip holds only %s (shader hashes, formats and draw state, and the hashes of the compute programs your play ran: no game "
        "code, game data or personal info) and this text.\n"
        % (ISSUE_URL, LIST))

# (hookshot_options.ini's values as the menu names them: gpu/vk/vk_menu.cpp tx_upscaler, tx_framegen, tx_hdr, and
# gpu/vk/upscaler_rows.h kPercent)
UPSCALERS = ("Game's own", "NVIDIA DLSS", "NVIDIA DLAA", "AMD FSR 3.1", "AMD FSR 4", "AMD FSR 4 Native AA", "Off (no upscaling)")
PERCENT = (0, 100, 75, 67, 50, 58, 33, 42, 83, 92)
MODES = {67: "Quality", 58: "Balanced", 50: "Performance", 33: "Ultra Performance"}
FRAMEGEN = ("Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x")
HDR = ("Off", "Expanded", "Native (the game's)")
OUTPUTS = ((2048, 1152), (2560, 1440), (3840, 2160), (1920, 1080), (5120, 1440), (5120, 2880))   # (vk_menu.cpp kOutputs)
HANDHELD = (1536, 864)


class ShareError(Exception):
    """Why there is nothing to share: said to the player as it is"""


def read_list(path):
    """(the list's bytes as read, once, and its records): the bytes checked as nx2/pipeline_states.py --check checks a list
    (strict: whole records), so the zip holds exactly what was checked. ShareError with the reason when it is missing, not a list
    this build reads, cut short or empty"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        raise ShareError("No pipeline list yet: %s is not in the game folder. The game writes it as you play: play a while, then "
                         "save it again." % LIST)
    try:
        body = lzma.decompress(data) if data[:6] == pipeline_states.XZ_MAGIC else data
        recs = pipeline_states.parse(body, path, strict=True)
    except lzma.LZMAError as e:
        raise ShareError("%s cannot be shared: a damaged xz file (%s)." % (LIST, e))
    except SystemExit as e:   # (pipeline_states says why, with the path)
        why = str(e).replace(path + ": ", "")
        if why.startswith("not a pipeline state list"):   # (its advice is the developers': make it again with this build)
            why = "it is not a pipeline state list this build reads"
        if why.startswith("cut short"):   # (a game that ended while it wrote: the file cannot be trusted past there)
            why += ". Delete it, play again, then save it again"
        raise ShareError("%s cannot be shared: %s." % (LIST, why.rstrip(".")))
    if not recs:
        raise ShareError("%s holds no pipelines yet. Play a while, then save it again." % LIST)
    return data, recs


def check_list(path):
    """The list's records (read_list)"""
    return read_list(path)[1]


def read_ini(folder):
    values = {}
    try:
        with open(os.path.join(folder, "hookshot_options.ini"), encoding="utf-8", errors="replace") as f:
            for line in f:
                k, eq, v = line.strip().partition("=")
                if eq and k:
                    values[k.strip()] = v.strip()
    except OSError:
        pass
    return values


def _int(values, key, default=0):
    try:
        return int(values.get(key, default))
    except ValueError:
        return default


def output_size(values):
    """(width, height, words): the game's output as hookshot_options.ini sets it (vk_menu.cpp parse_output, shown_output)"""
    if _int(values, "handheld_profile"):
        return HANDHELD[0], HANDHELD[1], "Handheld profile"
    text = values.get("output_res", "")
    m = re.match(r"^\s*(\d+)\s*[xX]\s*(\d+)", text)
    if m:
        w, h = int(m.group(1)) & ~7, int(m.group(2)) & ~7
        if w >= 640 and h >= 360 and w * h <= 5120 * 2880 and 4 * w >= 5 * h and 9 * w <= 48 * h:   # (hos_output_size_valid)
            return w, h, "the game's own" if (w, h) == OUTPUTS[0] else ""
        return OUTPUTS[0][0], OUTPUTS[0][1], "the game's own"
    v = _int(values, "output_res")
    w, h = OUTPUTS[v] if 0 <= v < len(OUTPUTS) else OUTPUTS[0]
    return w, h, "the game's own" if (w, h) == OUTPUTS[0] else ""


def output_text(values):
    w, h, words = output_size(values)
    extra = [words] if words else []
    if _int(values, "ultrawide") and words != "Handheld profile":
        extra.append("Ultrawide on")
    return "%dx%d%s" % (w, h, " (%s)" % ", ".join(extra) if extra else "")


def upscaler_text(values):
    up = _int(values, "dlss")
    up = up if 0 <= up < len(UPSCALERS) else 0
    name = UPSCALERS[up]
    if up in (2, 5):   # (DLAA, FSR 4's native AA: the 3D at full size)
        return "%s, Native" % name
    v = _int(values, "resolution")
    p = PERCENT[v] if 0 < v < len(PERCENT) else 0
    if not p:
        return "%s, Auto (dynamic)" % name
    w, h, _ = output_size(values)
    k = min(max(int(p / 100.0 * 12 + 0.5), 1), 12)   # (a twelfth of the output: vk_menu.cpp game_size)
    size = "%dx%d" % ((w * k + 11) // 12, h * k // 12)
    mode = MODES.get(p) if up in (1, 3, 4) else None
    return "%s, %s%d%% (%s)" % (name, mode + " " if mode else "", p, size)


def framegen_hdr_text(values):
    fg = _int(values, "frame_generation")
    hdr = _int(values, "hdr")
    return "%s / %s" % (FRAMEGEN[fg] if 0 <= fg < len(FRAMEGEN) else "Off", HDR[hdr] if 0 <= hdr < len(HDR) else "Off")


def find_mods(folder):
    """The mods the game loads from the game folder's mods folder, by the rule host/mods.c follows: a folder holding romfs or
    exefs, up to four folders deep, those whose names start with '-' or '_' left out (off)"""
    root = os.path.join(folder, "mods")
    found = []

    def walk(d, name, depth):
        if depth > 0 and (os.path.isdir(os.path.join(d, "romfs")) or os.path.isdir(os.path.join(d, "exefs"))):
            found.append(name)
            return
        if depth >= 4:
            return
        try:
            entries = sorted(os.listdir(d), key=str.lower)
        except OSError:
            return
        for e in entries:
            if e.startswith((".", "-", "_")) or not os.path.isdir(os.path.join(d, e)):
                continue
            walk(os.path.join(d, e), "%s/%s" % (name, e) if name else e, depth + 1)

    walk(root, "", 0)
    return sorted(found, key=str.lower)


def mods_text(names):
    return ", ".join(names) if names else "none"


def _log_lines(folder):
    for d in (folder, os.path.join(folder, "bin")):
        try:
            with open(os.path.join(d, "hookshot_log.txt"), encoding="utf-8", errors="replace") as f:
                return f.read(4 << 20).splitlines()
        except OSError:
            continue
    return []


def version_text(folder, log=None):
    """The version the game's last run said (its log's first line: the version and its source), else hookshot_version.txt's"""
    log = _log_lines(folder) if log is None else log
    for line in log[:5]:
        m = re.search(r"\bhookshot (\S+(?: \(source [0-9a-f]+\))?)", line)
        if m:
            return m.group(1)
    try:
        with open(os.path.join(folder, "hookshot_version.txt"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("version="):
                    return line[8:].strip()
    except OSError:
        pass
    return ""


def driver_number(name, v):
    """A Vulkan driver version as its vendor numbers it (an older log has only the number)"""
    if "nvidia" in name.lower():
        return "%d.%d.%d" % (v >> 22, v >> 14 & 0xFF, v >> 6 & 0xFF)
    if "intel" in name.lower() and os.name == "nt":
        return "%d.%d" % (v >> 14, v & 0x3FFF)
    return "%d.%d.%d" % (v >> 22, v >> 12 & 0x3FF, v & 0xFFF)


def gpu_text(folder, log=None):
    """The GPU the game's last run used and its driver, from its log (gpu/vk/vk_main.cpp: "vk: <GPU>, Vulkan ..., driver ...;"
    and, since this file, "vk: driver: <name> <info>")"""
    log = _log_lines(folder) if log is None else log
    gpu, number, driver = "", None, ""
    for line in log:
        m = re.search(r"\bvk: (.+?), Vulkan \d+\.\d+, driver (0x[0-9a-fA-F]+|\d+); mesh shaders", line)
        if m:
            gpu, number = m.group(1), int(m.group(2), 0)
        m = re.search(r"\bvk: driver: (.+)$", line)
        if m:
            driver = m.group(1).strip()
    if not gpu:
        return ""
    if not driver and number is not None:
        driver = driver_number(gpu, number)
    return "%s, driver %s" % (gpu, driver) if driver else gpu


def os_text():
    if os.name == "nt":
        release, version, _, _ = platform.win32_ver()
        try:
            build = int(version.split(".")[2])
        except (IndexError, ValueError):
            build = 0
        edition = getattr(platform, "win32_edition", lambda: "")() or ""
        name = "Windows Server" if "server" in edition.lower() else "Windows 11" if build >= 22000 else \
            ("Windows %s" % release if release else "Windows")
        return "%s (%s)" % (name, version) if version else name
    pretty = ""
    for path in ("/etc/os-release", "/usr/lib/os-release"):
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    if line.startswith("PRETTY_NAME="):
                        pretty = line.split("=", 1)[1].strip().strip('"')
            break
        except OSError:
            continue
    system = platform.system() or "Linux"
    return "%s (%s, kernel %s)" % (system, pretty, platform.release()) if pretty else "%s (kernel %s)" % (system, platform.release())


def details(folder, recs, log=None):
    """The template's values, in FIELDS' order ("" for one to fill in)"""
    log = _log_lines(folder) if log is None else log
    values = read_ini(folder)
    gpu = gpu_text(folder, log)
    return [version_text(folder, log), "%s; %s" % (os_text(), gpu or "(GPU and driver: fill in)"), output_text(values),
            upscaler_text(values), framegen_hdr_text(values), mods_text(find_mods(folder)), "",
            pipelines_text(recs)]


def pipelines_text(recs):
    """The Pipelines line: the count and the list's version, and how many of them are compute programs when it names any
    (gpu/vk/pipeline_share.h pipelines_text). The list is zipped as it is: they go with the pipelines"""
    computes = sum(pipeline_states.kind(r) == pipeline_states.COMPUTE for r in recs)
    return "%d (%s%s)" % (len(recs), "version 2" if recs and len(recs[0]) == pipeline_states.SIZE2 else "version 1",
                          ", %d of them compute programs" % computes if computes else "")


def template(values):
    """The issue's template, filled in: "- <field>: <value>" a line"""
    return "".join("- %s: %s\n" % (k, v) if v else "- %s:\n" % k for k, v in zip(FIELDS, values))


def zip_name(day=None):
    return "hookshot-pipelines-%s.zip" % (day or datetime.date.today()).isoformat()


def downloads_dir():
    """Where a browser's file picker opens: the Downloads folder (on Linux the one user-dirs.dirs names), else the home folder"""
    home = os.path.expanduser("~")
    if os.name != "nt":
        conf = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config"), "user-dirs.dirs")
        try:
            with open(conf, encoding="utf-8") as f:
                for line in f:
                    m = re.match(r'^\s*XDG_DOWNLOAD_DIR\s*=\s*"(.*)"\s*$', line)
                    if m:
                        d = m.group(1).replace("$HOME", home)
                        if os.path.isabs(d) and d.rstrip("/") != home and os.path.isdir(d):
                            return d
        except OSError:
            pass
    d = os.path.join(home, "Downloads")
    return d if os.path.isdir(d) else home


def make_zip(folder, out_dir=None, day=None):
    """The zip for the issue, from the game folder: (its path, the filled template, the number of pipelines). ShareError when the
    list is missing or not one to share (nothing written then)"""
    path = os.path.join(folder, LIST)
    data, recs = read_list(path)   # (read once: the bytes checked are the bytes zipped)
    text = template(details(folder, recs))
    out_dir = out_dir or downloads_dir()
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, zip_name(day))
    part = out + ".new"
    try:
        os.unlink(part)   # (a run that stopped halfway; a link is removed, not followed)
    except FileNotFoundError:
        pass
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    try:
        with os.fdopen(os.open(part, flags, 0o644), "wb") as f, zipfile.ZipFile(f, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr(LIST, data)
            z.writestr(REPORT, NOTE + "\n" + text)
        os.replace(part, out)
    except BaseException:
        try:
            os.unlink(part)
        except OSError:
            pass
        raise
    return out, text, len(recs)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("folder", help="the game folder")
    p.add_argument("--out-dir", help="where the zip goes (default: the Downloads folder)")
    p.add_argument("--json", action="store_true", help="one line of JSON (the Windows builder reads it)")
    a = p.parse_args(argv)
    try:
        out, text, n = make_zip(a.folder, a.out_dir)
    except (ShareError, OSError) as e:
        message = str(e) if isinstance(e, ShareError) else "The zip could not be written: %s" % e
        print(json.dumps({"ok": False, "error": message}) if a.json else message)
        return 1
    if a.json:
        print(json.dumps({"ok": True, "zip": out, "template": text, "pipelines": n, "url": ISSUE_URL}))
    else:
        print("%s: %d pipelines\n\nAttach it to %s and paste:\n\n%s" % (out, n, ISSUE_URL, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
