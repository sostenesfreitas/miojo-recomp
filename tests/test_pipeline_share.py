"""Save my pipeline list: the zip a player attaches to the "Submit your pipelines" issue (#2034), made by the game's menu
(gpu/vk/pipeline_share.h, vk_menu.cpp) and the builders (nx2/pipeline_share.py). Synthetic data only, no game:

  - the zip holds exactly hookshot_pipeline_states.bin (the bytes as the game wrote them) and hookshot_pipeline_report.txt, and
    nothing else from the game folder (saves, caches, the options file and the log stay out), from both the C++ and the Python;
  - the report is the issue's template filled in: every field in order, the version, the OS, the GPU and its driver from the
    game's log, the output, the upscaler and the 3D's size, frame generation and HDR from hookshot_options.ini, the mods the game
    loads (host/mods.c's rule), "where you played" left empty, and no path of the player's;
  - a list that is missing, not a list, cut short or empty is refused with the reason, and no zip is written;
  - the C++ and the Python agree: the same fields, the same checks; the Python's names for the options are the menu's;
  - the list holds only pipelines the game drew: state_record() is reached only from draw() (never the precompile, predicted
    pipelines or the shipped list);
  - the menu's row (Debug tab, under Developer) saves on Enter (not the Controls page's catch-all), opens nothing in a run without a window, and the builders
    (Linux: hookshot_builder.py; Windows: MainWindow.xaml.cs, through nx2/pipeline_share.py --json) offer the same.

python -I -B tests/test_pipeline_share.py [--cxx g++]
"""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
spec = importlib.util.spec_from_file_location("pipeline_share", ROOT / "nx2" / "pipeline_share.py")
share = importlib.util.module_from_spec(spec)
spec.loader.exec_module(share)

FIXTURE = r'''
#include "pipeline_share.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
using namespace mkwvk::pipeline_share;
// fixture check <list|->: the count (or -1) and the reason; fixture zip <list> <out>: the zip with a known template; fixture fields
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "fields") {
        for (const char* f : kFields) printf("%s\n", f);
        printf("%s\n%s\n%s\n", kIssueUrl, kListName, kReportName);
        return 0;
    }
    std::string bytes;
    bool present = false;
    if (argc > 2 && mode != "output" && std::string(argv[2]) != "-") {
        std::ifstream f(argv[2], std::ios::binary);
        present = (bool)f;
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::string why;
    const long long n = check_list(bytes, present, &why);
    if (mode == "check") return printf("%lld\n%s\n", n, why.c_str()), 0;
    if (mode == "output") return printf("%s\n", output_text(atoi(argv[2]), atoi(argv[3]), argv[4][0] == '1', argv[5][0] == '1', argv[6][0] == '1').c_str()), 0;
    if (mode == "xdg") return printf("[%s]\n", xdg_download_dir(bytes, "/home/someone").c_str()), 0;
    if (n < 0) return 1;
    std::vector<std::string> v(kFieldCount, "x");
    v[kWhere] = "";
    v[5] = mods_text({"a", "deep/b"});
    v[7] = pipelines_text(n, true, compute_count(bytes));
    const std::string z = share_zip(bytes, template_lines(v), (uint16_t)((2026 - 1980) << 9 | 11 << 5 | 11), 0);
    FILE* o = fopen(argv[3], "wb");
    fwrite(z.data(), 1, z.size(), o);
    fclose(o);
    printf("%s\n", zip_name(2026, 11, 11).c_str());
    return 0;
}
'''


def record(i, size=share.pipeline_states.SIZE2):
    r = struct.pack("<Q", i + 1) + bytes((i * 7 + k) & 0xFF for k in range(size - 8))
    # (a version 2 record's last two words: its kind, 0 for a pipeline, and padding)
    return r[:-8] + bytes(8) if size == share.pipeline_states.SIZE2 else r


def v2_list(n):
    return share.pipeline_states.MAGIC2 + struct.pack("<I", share.pipeline_states.SIZE2) + b"".join(record(i) for i in range(n))


def with_computes(n, keys):
    """A list of n pipelines and the compute programs keys, as the game writes them (vk_draw.cpp state_record_compute)"""
    return v2_list(n) + b"".join(share.pipeline_states.compute_record(k) for k in keys)


def game_folder(root, list_bytes=None):
    g = root / "game"
    g.mkdir()
    if list_bytes is not None:
        (g / share.LIST).write_bytes(list_bytes)
    # what must never go into the zip
    for name in ("hookshot_options.ini", "hookshot_log.txt", "hookshot_shaders.bin", "hookshot_pipelines.bin", "language.txt",
                 "hookshot_pipeline_states_shipped.bin", "hookshot_pipeline_states_v1.bin"):
        (g / name).write_bytes(b"")
    (g / "hookshot_options.ini").write_text("output_res=2560x1440\ndlss=1\nresolution=3\nframe_generation=2\nhdr=0\nultrawide=0\n"
                                            "options_version=4\n", encoding="utf-8")
    (g / "hookshot_log.txt").write_text(
        "[hos host] hookshot v2.2.0-rc.1-5-gabcdef1 (source 0123abcd)\n"
        "[hos host] vk: device 0: Some GPU, Vulkan 1.4, driver 0x9b118180, type 2\n"
        "[hos host] vk: Some GPU, Vulkan 1.4, driver 0x9b118180; mesh shaders yes, host memory import yes, pipeline libraries yes\n"
        "[hos host] vk: driver: SomeDriver 1.2.3\n", encoding="utf-8")
    (g / "hookshot_version.txt").write_text("version=v2.1.0\n", encoding="utf-8")
    for d in ("mods/a/romfs", "mods/_off/romfs", "mods/-off2/romfs", "mods/contents/deep/b/romfs", "mods/notamod/x",
              "user/save/1/slot_00"):
        (g / d).mkdir(parents=True)
    (g / "user/save/1/slot_00/save.bin").write_bytes(b"save")
    return g


def check_python(tmp):
    t = Path(tmp)
    data = v2_list(3)
    g = game_folder(t, data)
    out = t / "out"
    path, text, n = share.make_zip(str(g), str(out), day=share.datetime.date(2026, 11, 11))
    assert n == 3 and Path(path).name == "hookshot-pipelines-2026-11-11.zip", (path, n)
    with zipfile.ZipFile(path) as z:
        assert z.namelist() == [share.LIST, share.REPORT], "the zip holds %s, not exactly the list and the report" % z.namelist()
        assert z.testzip() is None
        assert z.read(share.LIST) == data, "the list is not as the game wrote it"
        report = z.read(share.REPORT).decode("utf-8")
    assert report.endswith(text) and share.ISSUE_URL in report
    lines = text.splitlines()
    assert [l.split(":")[0] for l in lines] == ["- " + f for f in share.FIELDS], lines
    want = {
        "Hookshot version": "v2.2.0-rc.1-5-gabcdef1 (source 0123abcd)",
        "Output resolution": "2560x1440",
        "Upscaler + render resolution": "NVIDIA DLSS, Quality 67% (1707x960)",
        "Frame generation / HDR": "2x / Off",
        "Mods or texture packs": "a, contents/deep/b",
        "Pipelines": "3 (version 2)",
    }
    got = dict(l[2:].split(": ", 1) for l in lines if ": " in l)
    for k, v in want.items():
        assert got.get(k) == v, "%s: %r, not %r" % (k, got.get(k), v)
    assert got["OS, GPU and driver"].endswith("; Some GPU, driver SomeDriver 1.2.3"), got["OS, GPU and driver"]
    assert "- %s:\n" % share.WHERE in text, "where the player played is not left for them"
    assert str(t) not in report and os.path.expanduser("~") not in report, "a path of the player's is in the report"
    assert sorted(os.listdir(out)) == [Path(path).name], "something besides the zip was left in the folder"

    # a list that names the compute programs the play ran: sent with the pipelines, the bytes as they are, and counted in the report
    both = with_computes(2, (0x1111, 0x2222, 0x3333))
    (g / share.LIST).write_bytes(both)
    path2, text2, n2 = share.make_zip(str(g), str(t / "out-computes"), day=share.datetime.date(2026, 11, 11))
    with zipfile.ZipFile(path2) as z:
        assert z.namelist() == [share.LIST, share.REPORT] and z.read(share.LIST) == both, "the compute records are not sent as written"
        assert "compute programs your play ran" in z.read(share.REPORT).decode("utf-8")
    assert n2 == 5 and "- Pipelines: 5 (version 2, 3 of them compute programs)" in text2.splitlines(), text2
    assert share.pipeline_states.compute_keys(share.pipeline_states.read_sent(str(g / share.LIST))) == [0x1111, 0x2222, 0x3333]
    (g / share.LIST).write_bytes(data)
    # the list is read once, and the bytes zipped are the bytes checked (a game appending between two reads changed them)
    src = (ROOT / "nx2" / "pipeline_share.py").read_text(encoding="utf-8")
    mz = src[src.find("def make_zip("):src.find("def main(")]
    assert mz.count("read_list(") == 1 and "open(path" not in mz, "make_zip reads the list more than once"
    # the part file is made fresh: a link left at <zip>.new is replaced, not followed
    if hasattr(os, "symlink") and os.name != "nt":
        target = t / "elsewhere.txt"
        target.write_text("keep", encoding="utf-8")
        os.symlink(target, Path(path + ".new"))
        share.make_zip(str(g), str(out), day=share.datetime.date(2026, 11, 11))
        assert target.read_text(encoding="utf-8") == "keep" and not os.path.lexists(path + ".new"), "the zip was written through a link"
    # older logs: the driver from its number; no log: the version file; settings: handheld, DLAA, the game's own output
    (g / "hookshot_log.txt").write_text("[hos host] vk: NVIDIA GeForce RTX 3090, Vulkan 1.4, driver 0x9b118180; mesh shaders yes\n",
                                        encoding="utf-8")
    assert share.gpu_text(str(g)) == "NVIDIA GeForce RTX 3090, driver 620.70.6", share.gpu_text(str(g))
    assert share.version_text(str(g)) == "v2.1.0"
    vals = {"handheld_profile": "1", "dlss": "2", "hdr": "2", "frame_generation": "0"}
    assert share.output_text(vals) == "1536x864 (Handheld profile)"
    assert share.upscaler_text(vals) == "NVIDIA DLAA, Native"
    assert share.framegen_hdr_text(vals) == "Off / Native (the game's)"
    assert share.output_text({}) == "2048x1152 (the game's own)"
    assert share.upscaler_text({"dlss": "0", "resolution": "0"}) == "Game's own, Auto (dynamic)"
    assert share.upscaler_text({"dlss": "3", "resolution": "4", "output_res": "3840x2160"}) == "AMD FSR 3.1, Performance 50% (1920x1080)"

    # refused: missing, not a list, cut short, empty; no zip each time
    bad = {"missing": None, "not a list": b"PK\x03\x04" + bytes(600), "cut short": v2_list(2)[:-5],
           "empty": share.pipeline_states.MAGIC2 + struct.pack("<I", share.pipeline_states.SIZE2)}
    words = {"missing": "No pipeline list yet", "not a list": "not a pipeline state list", "cut short": "Delete it, play again",
             "empty": "holds no pipelines"}
    for name, content in bad.items():
        if content is None:
            (g / share.LIST).unlink(missing_ok=True)
        else:
            (g / share.LIST).write_bytes(content)
        out2 = t / ("out-" + name.replace(" ", "-"))
        try:
            share.make_zip(str(g), str(out2))
        except share.ShareError as e:
            assert words[name] in str(e), (name, str(e))
            assert str(g) not in str(e), "the reason names the player's path"
        else:
            raise AssertionError("a list that is %s was shared" % name)
        assert not out2.exists() or not os.listdir(out2), "a zip was written for a list that is %s" % name

    # the command the Windows builder runs
    (g / share.LIST).write_bytes(data)
    r = subprocess.run([sys.executable, "-I", str(ROOT / "nx2" / "pipeline_share.py"), str(g), "--out-dir", str(t / "cli"), "--json"],
                       capture_output=True, text=True)
    j = json.loads(r.stdout)
    assert r.returncode == 0 and j["ok"] and j["pipelines"] == 3 and j["url"] == share.ISSUE_URL and j["template"].startswith("- "), r
    (g / share.LIST).unlink()
    r = subprocess.run([sys.executable, "-I", str(ROOT / "nx2" / "pipeline_share.py"), str(g), "--out-dir", str(t / "cli2"), "--json"],
                       capture_output=True, text=True)
    j = json.loads(r.stdout)
    assert r.returncode == 1 and not j["ok"] and "No pipeline list yet" in j["error"], r

    # the Downloads folder: user-dirs.dirs' (not the home folder itself), else ~/Downloads
    home = t / "home"
    (home / "Dl").mkdir(parents=True)
    (home / ".config").mkdir()
    (home / ".config" / "user-dirs.dirs").write_text('XDG_DOWNLOAD_DIR="$HOME/Dl"\n', encoding="utf-8")
    env = dict(os.environ, HOME=str(home))
    env.pop("XDG_CONFIG_HOME", None)
    r = subprocess.run([sys.executable, "-I", "-c", "import sys; sys.path.insert(0, %r); import pipeline_share; "
                        "print(pipeline_share.downloads_dir())" % str(ROOT / "nx2")], capture_output=True, text=True, env=env)
    if os.name != "nt":
        assert r.stdout.strip() == str(home / "Dl"), r


def check_cxx(cxx, tmp):
    t = Path(tmp)
    src, exe = t / "fixture.cpp", t / ("fixture.exe" if os.name == "nt" else "fixture")
    src.write_text(FIXTURE, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(ROOT / "gpu" / "vk"), str(src), "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("pipeline_share.h does not compile:\n" + r.stderr[-3000:])
    run = lambda *a: subprocess.run([str(exe)] + [str(x) for x in a], capture_output=True, text=True)
    fields = run("fields").stdout.splitlines()
    assert fields[:-3] == list(share.FIELDS), "the C++ template's fields are not the Python's: %s" % fields
    assert fields[-3:] == [share.ISSUE_URL, share.LIST, share.REPORT], fields[-3:]
    # the same checks as the Python's (nx2/pipeline_states.py --check)
    cases = {"good": v2_list(4), "v1": share.pipeline_states.MAGIC + struct.pack("<I", share.pipeline_states.SIZE) +
             record(0, share.pipeline_states.SIZE), "not a list": b"HKSTATE2" + struct.pack("<I", 528) + bytes(528),
             "cut short": v2_list(2)[:-1], "empty": v2_list(0), "tiny": b"HKS"}
    for name, data in cases.items():
        p = t / ("list-" + name.replace(" ", "-"))
        p.write_bytes(data)
        n, why = (run("check", p).stdout.splitlines() + [""])[:2]
        try:
            py = len(share.check_list(str(p)))
        except share.ShareError:
            py = -1
        assert int(n) == py, "%s: the C++ counts %s, the Python %d" % (name, n, py)
        assert (int(n) < 0) == bool(why), (name, n, why)
        if name == "cut short":
            assert "Delete it, play again" in why, why
    n, why = run("check", "-").stdout.splitlines()[:2]
    assert n == "-1" and "No pipeline list yet" in why, (n, why)
    # its zip: exactly the two files, readable, the list's bytes as they were
    data = v2_list(5)
    (t / "good.bin").write_bytes(data)
    r = run("zip", t / "good.bin", t / "c.zip")
    assert r.returncode == 0 and r.stdout.strip() == "hookshot-pipelines-2026-11-11.zip", r
    with zipfile.ZipFile(t / "c.zip") as z:
        assert z.namelist() == [share.LIST, share.REPORT], z.namelist()
        assert z.testzip() is None, "a CRC does not match"
        assert z.read(share.LIST) == data
        report = z.read(share.REPORT).decode("utf-8")
        assert z.getinfo(share.LIST).date_time[:3] == (2026, 11, 11)
    lines = report.split("\n\n", 1)[1].splitlines()
    assert [l.split(":")[0] for l in lines] == ["- " + f for f in share.FIELDS], lines
    assert "- %s:" % share.WHERE in lines and "- Mods or texture packs: a, deep/b" in lines and "- Pipelines: 5 (version 2)" in lines
    assert share.ISSUE_URL in report
    # a list with compute records: the same bytes in the zip, the same Pipelines line as the Python's
    both = with_computes(2, (0x1111, 0x2222, 0x3333))
    (t / "both.bin").write_bytes(both)
    assert run("zip", t / "both.bin", t / "both.zip").returncode == 0
    with zipfile.ZipFile(t / "both.zip") as z:
        assert z.read(share.LIST) == both, "the game's zip does not hold the compute records as written"
        report = z.read(share.REPORT).decode("utf-8")
    assert "- Pipelines: 5 (version 2, 3 of them compute programs)" in report.splitlines(), report
    assert "- Pipelines: %s" % share.pipelines_text(share.pipeline_states.parse(both, "both", strict=True)) in report.splitlines()
    assert "compute programs your play ran" in report
    # a list that is not one: no zip
    assert run("zip", t / "list-cut-short", t / "bad.zip").returncode != 0 and not (t / "bad.zip").exists()
    # the Output resolution line combined the same way (the Handheld profile, the game's own size, Ultrawide)
    for ini in ({"output_res": "2560x1440"}, {"output_res": "2560x1440", "ultrawide": "1"}, {"ultrawide": "1"}, {},
                {"handheld_profile": "1", "ultrawide": "1"}, {"output_res": "3840x2160", "handheld_profile": "1"}):
        w, h, _ = share.output_size(ini)
        flags = ["1" if share._int(ini, "handheld_profile") else "0", "1" if (w, h) == share.OUTPUTS[0] else "0",
                 "1" if share._int(ini, "ultrawide") else "0"]
        got = run("output", w, h, *flags).stdout.strip()
        assert got == share.output_text(ini), "Output resolution for %s: the C++ says %r, the Python %r" % (ini, got, share.output_text(ini))
    # user-dirs.dirs
    for text, want in (('XDG_DOWNLOAD_DIR="$HOME/Downloads"\n', "/home/someone/Downloads"), ('# x\nXDG_DOWNLOAD_DIR="/data/dl/"\n',
                       "/data/dl"), ('XDG_DOWNLOAD_DIR="$HOME/"\n', ""), ('XDG_MUSIC_DIR="$HOME/Music"\n', "")):
        (t / "dirs").write_text(text, encoding="utf-8")
        assert run("xdg", t / "dirs").stdout.strip() == "[%s]" % want, (text, run("xdg", t / "dirs").stdout)


def check_sources():
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    rows = (ROOT / "gpu" / "vk" / "upscaler_rows.h").read_text(encoding="utf-8")
    # the Python names the options as the menu does
    m = re.search(r"const char\* tx_upscaler\(int\) \{\s*static const char\* t\[ur::kUpscalers\] = \{(.*?)\};", menu, re.S)
    assert m and tuple(re.findall(r'"([^"]*)"', m.group(1))) == share.UPSCALERS, "the Upscaler's names differ from the menu's"
    m = re.search(r"const char\* tx_framegen\(int v\) \{\s*static const char\* t\[\] = \{(.*?)\};", menu, re.S)
    assert m and tuple(re.findall(r'"([^"]*)"', m.group(1))) == share.FRAMEGEN, "Frame generation's names differ from the menu's"
    m = re.search(r'const char\* tx_hdr\(int v\) \{ return v == 1 \? "([^"]*)" : v == 2 \? "([^"]*)" : "([^"]*)"; \}', menu)
    assert m and (m.group(3), m.group(1), m.group(2)) == share.HDR, "HDR's names differ from the menu's"
    m = re.search(r"constexpr int kPercent\[\] = \{([^}]*)\}", rows)
    assert m and tuple(int(x) for x in m.group(1).split(",")) == share.PERCENT, "the Render resolution's shares differ"
    m = re.search(r"constexpr Out kOutputs\[\] = \{(.*?)\};", menu)
    assert m and tuple((int(a), int(b)) for a, b in re.findall(r"\{(\d+), (\d+)\}", m.group(1))) == share.OUTPUTS
    # the row, on the Debug tab under the Developer heading (the tab lists only the rows after it), saving on Enter before the
    # catch-all that opens the Controls page, and drawn by the tab's own layout
    assert re.search(r'\{TAB_DEBUG, "Save my pipeline list", nullptr,', menu), "the menu has no Save my pipeline list row on the Debug tab"
    at_row, at_head = menu.find('"Save my pipeline list", nullptr'), menu.find('{TAB_DEBUG, "Developer", nullptr')
    assert 0 < at_head < at_row, "the row is not under the Debug tab's Developer heading"
    debug = menu[menu.find("void draw_debug_rows("):]
    debug = debug[:debug.index("\n}\n")]
    assert debug.count("is_share_row(r)") >= 2, "the Debug tab's layout neither sizes nor draws the row (cheat_value(-1))"
    assert "is_share_row(const Row& r) { return !r.value && r.tab == TAB_DEBUG" in menu
    # the compact tuning bar (row_value_cell, its prompts) shows it as an action too, not as a page to open
    cell = body_of(menu, "void row_value_cell(")
    assert 'is_share_row(r)) choice_cell(c, va, vb, share_value.empty() ? "Save" : share_value' in cell, "the tuning bar shows the row as a page"
    assert menu.count('is_share_row(r) ? "Save" : "Open"') + menu.count('is_share_row(selr) ? "Save" : "Open"') >= 2, \
        "a prompt says Open for Save my pipeline list"
    cs = (ROOT / "builder" / "HookshotBuilder" / "MainWindow.xaml.cs").read_text(encoding="utf-8")
    tick = menu[menu.find("void menu_tick() {"):]
    save, catch_all = tick.find("is_share_row(r)) share_pipeline_list()"), tick.find("!r.value && can_bind()) page = 1")
    assert 0 < save < catch_all, "Enter on the row would open the Controls page"
    body = menu[menu.find("void share_pipeline_list() {"):]
    body = body[:body.index("\n}\n")]
    assert "ps::share_zip(" in body and "state_list_snapshot(" in body and "ps::check_list(" in body
    assert 'env_on("HOS_HEADLESS")' in body, "a run without a window would open the browser"
    assert body.find("ps::check_list(") < body.find("ps::share_zip("), "the list is zipped before it is checked"
    # the clipboard and the opens on a worker (the present thread must not wait on ShellExecuteW or xdg-open), COM on it for
    # ShellExecuteW, and a press while it works is ignored
    assert body.split("\n", 1)[1].lstrip().startswith("if (share_busy.load()) return;"), "a second press runs again"
    worker = body[body.find("std::thread(["):]
    assert body.find("std::thread([") > body.find('env_on("HOS_HEADLESS")'), "the opens are not on the worker"
    for call in ("share_clipboard(", "share_open(ps::kIssueUrl)", '"CoInitializeEx"', "co_uninit()", "share_result.store("):
        assert call in worker, "the worker lacks " + call
    assert "catch (const std::system_error&)" in body and "share_finish();" in body.split("catch (const std::system_error&)")[1], \
        "a thread that cannot start leaves the row busy for good"
    dl = body_of(menu, "std::string share_downloads(")
    assert dl.count('"your home folder"') == 2, "the home folder is named on screen (the user's name)"
    assert 'L"WINDIR"' in menu and 'GetFolderPath(Environment.SpecialFolder.Windows)' in cs, "explorer.exe is taken from the search path"
    assert "kShareSaidLongest" in menu[menu.find("float measure_help("):][:900], "the help box has no room kept for the line after a save"
    assert "share_finish();" in menu[menu.find("void menu_tick() {"):][:400], "menu_tick never collects the worker's answer"
    spawn = menu[menu.find("bool share_spawn("):]
    spawn = spawn[:spawn.index("\n}\n")]
    assert "pipe2(fds, O_CLOEXEC)" in spawn and "addclosefrom_np(&fa, 3)" in spawn, \
        "a desktop program can inherit the clipboard pipe (wl-copy then waits for the browser to exit)"
    assert "pthread_sigmask(SIG_BLOCK, &pipe_set" in spawn and "sigtimedwait(&pipe_set" in spawn, \
        "a clipboard program that exits before reading would end the game (SIGPIPE)"
    assert "WEXITSTATUS(status) == 0" in spawn, "a clipboard program that failed counts as copied (no fallback to xclip)"
    for word in ("WinHttp", "curl", "POST"):
        assert word not in body, "Save my pipeline list uploads something (%s)" % word
    # the builders
    linux = (ROOT / "builder" / "linux" / "hookshot_builder.py").read_text(encoding="utf-8")
    assert "import pipeline_share" in linux and "Save my pipeline list" in linux
    cs = (ROOT / "builder" / "HookshotBuilder" / "MainWindow.xaml.cs").read_text(encoding="utf-8")
    xaml = (ROOT / "builder" / "HookshotBuilder" / "MainWindow.xaml").read_text(encoding="utf-8")
    assert '"pipeline_share.py"' in cs and '"--json"' in cs and "Save my pipeline list" in xaml
    assert share.ISSUE_URL in cs


def body_of(source, signature):
    at = source.find(signature)
    assert at >= 0, signature + " is missing"
    body = source[at:]
    return body[:body.index("\n}\n")]


def check_drawn_only():
    """The list a player shares holds only pipelines the game drew on that PC: hookshot_pipeline_states.bin gets a record only from
    state_record(), which only pipeline_note() calls, and only with the draw's colour types, which only graphics_pipeline() passes,
    which only draw() calls. The precompile's notes, predicted pipelines (predict_run, vk_predict.cpp) and the shipped list never
    reach it"""
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    others = {p.name: p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "gpu" / "vk").glob("*.cpp") if p.name != "vk_draw.cpp"}
    for name, text in others.items():
        assert "state_record(" not in text and "state_file" not in text, "%s writes the pipeline state list" % name
    calls = [m.start() for m in re.finditer(r"(?<![\w:])state_record\(", draw)]
    defs = [m.start() for m in re.finditer(r"\nvoid state_record\(", draw)]
    callers = [c for c in calls if not any(d < c < d + 20 for d in defs)]
    note = body_of(draw, "void pipeline_note(")
    assert len(callers) == 1 and note.count("if (color_types) state_record(") == 1, \
        "state_record() is called from somewhere other than pipeline_note(), with the draw's colour types"
    # pipeline_note with colour types (a 6th argument): only graphics_pipeline()
    def top_args(text):   # (the arguments at the call's own level)
        n, depth = 1, 0
        for ch in text:
            depth += ch in "([{" and 1 or ch in ")]}" and -1 or 0
            n += ch == "," and depth == 0
        return n
    six = [m for m in re.finditer(r"pipeline_note\(([^;]*)\);", draw) if top_args(m.group(1)) >= 6 and "uint64_t key" not in m.group(1)]
    gp = body_of(draw, "PipeEntry* graphics_pipeline(")
    assert len(six) == 1 and six[0].group(0) in gp, "pipeline_note() gets colour types from outside graphics_pipeline()"
    users = [m.start() for m in re.finditer(r"(?<![\w:*])graphics_pipeline\(", draw)]
    users = [u for u in users if draw[max(0, u - 12):u] != "PipeEntry* " and not draw[max(0, u - 11):u].endswith("PipeEntry* ")]
    d = body_of(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    assert len(users) == 1 and d.count("graphics_pipeline(") == 1, "graphics_pipeline() is reached from outside draw()"
    for fn in ("void precompile_run(", "void state_list_prepare("):
        if fn in draw:
            b = body_of(draw, fn)
            assert "state_record(" not in b and "graphics_pipeline(" not in b, fn + " can write the list"
    # records are written in state_record() alone (state_open_locked writes a new list's header), and a compute program's in
    # state_record_compute(), which only dispatch() calls, at the program's first dispatch (tests/test_pipeline_state_computes.py)
    writes = [m.start() for m in re.finditer(r"fwrite\([^;]*state_file", draw)]
    spans = []
    for fn in (r"\nvoid state_record\([^;{]*\) \{", r"\nvoid state_open_locked\(\) \{", r"\nvoid state_record_compute\(uint64_t key\) \{"):
        m = re.search(fn, draw)
        assert m, fn
        spans.append((m.start(), draw.index("\n}\n", m.start())))
    assert writes and all(any(a <= w < b for a, b in spans) for w in writes), "the list is written outside state_record()"
    assert "fwrite(&r, sizeof r, 1, state_file)" in draw[spans[0][0]:spans[0][1]]
    computes = [m.start() for m in re.finditer(r"(?<![\w:])state_record_compute\(", draw)]
    d = body_of(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    assert len(computes) == 2 and d.count("state_record_compute(p.key)") == 1, "a compute record is written outside dispatch()"
    assert all("state_record_compute(" not in text for text in others.values()), "another file writes compute records"


def check_linux_builder(tmp):
    """The Linux builder's action, without a display: the zip, then the clipboard, the folder and the issue through what it is
    given; a refusal opens nothing"""
    spec = importlib.util.spec_from_file_location("hookshot_builder_share", ROOT / "builder" / "linux" / "hookshot_builder.py")
    try:
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
    except ImportError as e:   # (no tkinter: the module cannot load; the source checks above still ran)
        print("the Linux builder's action not run: %s" % e)
        return
    t = Path(tmp)
    g = game_folder(t, v2_list(2))
    calls = []
    said = builder.share_pipeline_list(str(g), out_dir=str(t / "dl"), copy=lambda s: calls.append(("copy", s)),
                                       open_path=lambda p: calls.append(("open", p)))
    zips = list((t / "dl").glob("*.zip"))
    assert len(zips) == 1 and zipfile.ZipFile(zips[0]).namelist() == [share.LIST, share.REPORT]
    assert [c[0] for c in calls] == ["copy", "open", "open"] and calls[0][1].startswith("- Hookshot version:"), calls
    assert calls[1][1] == str(t / "dl") and calls[2][1] == share.ISSUE_URL, calls
    assert zips[0].name in said and "attach" in said, said
    (g / share.LIST).unlink()
    calls.clear()
    said = builder.share_pipeline_list(str(g), out_dir=str(t / "dl2"), copy=lambda s: calls.append(s), open_path=calls.append)
    assert not calls and "No pipeline list yet" in said and not (t / "dl2").exists(), (calls, said)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    check_sources()
    check_drawn_only()
    with tempfile.TemporaryDirectory() as tmp:
        check_python(tmp)
    with tempfile.TemporaryDirectory() as tmp:
        check_cxx(a.cxx, tmp)
    with tempfile.TemporaryDirectory() as tmp:
        check_linux_builder(tmp)
    print("pipeline share: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
