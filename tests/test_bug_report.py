"""The Insert menu's Bug report writes what an issue needs, and nothing private.

gpu/vk/vk_bug_report.cpp is compiled alone with a driver that writes reports from a made-up game folder in a made-up home
folder: this run's log and the run before's, with the home folder and the user's name in their paths, map lines and a crash.
The driver answers the picture request as vk_replay.cpp's shot_ask does (a PNG of a later present, written on a thread).
Checked:

- the report's folder holds report.txt, both logs and the picture, and nothing else; no file in it names the home folder,
  the user's name as a folder, or the game folder's path;
- report.txt's sections are the issue form's (.github/ISSUE_TEMPLATE/1-game-problem.yml), and scripts/hardware_report.py
  reads the GPU's vendor and class and the system from it as it reads an issue; its link fills in the form's own fields and
  stays within 2000 characters (GitHub's sign-in page fails on longer ones): more switches than fit are counted, not listed;
- where the player is comes from the last map line (host/hos_main.c logs them), the run before's crash line is named;
- the switches set are listed by name, and a value only when it is a number, a size or on/off and its name holds none of
  TOKEN, KEY, ROOM, SECRET, PASS, SERVER, USER: paths, addresses, scripts and secrets are left out; other variables are not
  listed; the link carries the switches' names and never a value;
- a log over 10 MB keeps its start and its end, and what is in its middle still counts; a log with NUL bytes (in a line,
  or a run of them at its end, as a power cut leaves) is copied byte for byte, and what follows a NUL still counts;
- started from a terminal (no log file), the run before's log is the game folder's hookshot_log.txt, and without a frame
  the report says it has no picture; a picture that comes after the report went without it is removed again; a second
  report in the same second gets a folder of its own; a folder that cannot be made is reported with the system's reason;
- the wiring: the Gameplay tab's row and its help, the press, the picture asked of shot_ask (its own image, copied from the
  game's frame without the menu), the host's map line.

python -I -B tests/test_bug_report.py [--cxx g++]
"""
import argparse
import importlib.util
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = "v2.1.0-rc.3-39-gbdfe09c (source 763271ad)"

DRIVER = r'''#include "vk_bug_report.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
using namespace mkwvk::bug_report;
static int fails;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static void say(const char* line) { std::printf("LOG %s\n", line); }
static std::thread painter;
static bool ask(const char* path, uint64_t ticket) {   // (vk_replay.cpp's part: the PNG of a later present, on a thread)
    painter = std::thread([p = std::string(path), ticket] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (FILE* f = std::fopen(p.c_str(), "wb")) std::fputs("PNG stand-in", f), std::fclose(f);
        picture_written(ticket, true);
    });
    return true;
}
static std::string late_path;   // (the late picture: asked for, and written only once the report has gone without it)
static uint64_t late_ticket;
static bool ask_late(const char* path, uint64_t ticket) { late_path = path, late_ticket = ticket; return true; }
static std::atomic<bool> held{true};   // (the menu's way: the report is still saving until the driver lets its picture ask go)
static bool ask_held(const char*, uint64_t) {   // (no frame comes: the report goes without a picture)
    while (held.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return false;
}

int main(int argc, char** argv) {
    if (argc < 7) return 2;
    const std::string mode = argv[1];
    Inputs in;
    in.version = "''' + VERSION + r'''";
    in.gpu = "NVIDIA GeForce RTX 4090, NVIDIA 580.95.05";
    in.options = "Upscaler: NVIDIA DLSS (default: Game's)\n";
    in.system = "13th Gen Intel(R) Core(TM) i9-13900K (32 threads), 94 GB, Arch Linux (Linux 7.2.15)";
    in.env = {"HOS_HEADLESS=1", std::string("RX_VK_SHOTS=") + argv[4] + "/shots", "RX_VK_MENU_SCRIPT=1:INS,2:ENTER",
              "PATH=/usr/bin", "MANGOHUD=1", "HOS_ENV_FILE=C:\\x\\env.txt", "SECRET_TOKEN=abc", "=C:=C:\\x",
              "RX_VK_SHOT_EVERY=60", "RX_VK_X_MODE=off", "HOS_OUTPUT_RES=3840x2160", "RX_VK_NEG=-1", "RX_VK_SCALE=1.5",
              "HOS_COOP_ROOM=93127", "RX_API_KEY=58213", "HOS_COOP_SERVER=47731", "HOS_COOP_USER=1", "HOS_COOP_PASSWORD=hunter2",
              "HOS_RELAY=192.168.1.5", "HOS_RELAY_HOST=relay.example.org:47732", "RX_VK_LONG=1234567890123"};
    if (mode == "many")   // (a long hookshot_env.txt: more switches than a link takes)
        for (int i = 0; i < 80; i++) in.env.push_back("RX_TEST_SWITCH_" + std::to_string(i) + "=" + std::string(20, 'v'));
    in.game_dir = argv[2];
    in.current_log = argv[3];
    in.home = argv[4];
    in.user = argv[5];
    in.windows = std::string(argv[6]) == "1";
    in.now = 1789000000;
    in.log = say;
    in.picture_wait_ms = mode == "picture" ? 30000 : 300;   // (a picture ends the wait as soon as it is written)
    if (mode == "picture") in.ask_picture = ask;
    if (mode == "late") in.ask_picture = ask_late;
    if (mode == "start") {   // (the menu's way: on a thread of its own, its state read each frame)
        in.ask_picture = ask_held;
        CHECK(start(in));
        CHECK(state() == kSaving);
        CHECK(!start(in));   // (one at a time)
        held = false;
        for (int i = 0; i < 3000 && state() == kSaving; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(state() == kSaved);
        std::printf("FOLDER %s\n", shown().c_str());
    } else {
        const std::string folder = write(in);
        std::printf("FOLDER %s\n", folder.c_str());
        if (mode == "terminal") std::printf("FOLDER %s\n", write(in).c_str());   // (a second one, the same second)
        if (mode == "late") {   // (the picture comes after the report went without it: removed again)
            if (FILE* f = std::fopen(late_path.c_str(), "wb")) std::fputs("PNG stand-in", f), std::fclose(f);
            picture_written(late_ticket, true);
            FILE* png = std::fopen((in.game_dir + folder + (in.windows ? "\\" : "/") + "picture.png").c_str(), "rb");
            std::printf("LATE %s %s\n", png ? "kept" : "removed", late_path == in.game_dir + folder + (in.windows ? "\\" : "/") + "picture.png" ? "(asked for the report's own picture.png)" : "(asked for another path)");
            if (png) std::fclose(png);
        }
    }
    if (painter.joinable()) painter.join();

    // the redaction itself: whole folders only, either slash on Windows, whatever the case there
    CHECK(redact("-> /home/al/Games/x\n", "/home/al", "al", false) == "-> ~/Games/x\n");
    CHECK(redact("/home/al", "/home/al/", "al", false) == "~");
    CHECK(redact("/home/alice/x /home/al", "/home/al", "al", false) == "/home/alice/x ~");
    CHECK(redact("/run/media/al/SD, /var/home/al/x, user\\al\\save", "/home/al", "al", false) ==
          "/run/media/<user>/SD, /var/home/<user>/x, user\\<user>\\save");
    CHECK(redact("Malice and al, /x/alx/ and vk: al", "/home/al", "al", false) == "Malice and al, /x/alx/ and vk: al");
    CHECK(redact("/HOME/AL/x", "/home/al", "al", false) == "/HOME/AL/x");   // (Linux: case counts)
    CHECK(redact("C:\\Users\\John Smith\\Games\\x", "C:\\Users\\John Smith", "John Smith", true) == "%USERPROFILE%\\Games\\x");
    CHECK(redact("c:/users/john smith/Games", "C:\\Users\\John Smith", "John Smith", true) == "%USERPROFILE%/Games");
    CHECK(redact("D:\\Users\\JOHN SMITH\\x", "C:\\Users\\John Smith", "John Smith", true) == "D:\\Users\\<user>\\x");
    CHECK(redact("nothing here", "", "", false) == "nothing here");

    // this PC's part: the system as the form asks it, and what fill_here finds
    std::printf("SYSTEM %s\n", system_text().c_str());
    Inputs here;
    fill_here(here);
    CHECK(here.system == system_text() && here.now > 0 && !here.env.empty());
    return fails ? 1 : 0;
}
'''


def load_hardware_report():
    spec = importlib.util.spec_from_file_location("hardware_report", ROOT / "scripts" / "hardware_report.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check(ok, why):
    if not ok:
        raise SystemExit("bug report: " + why)


def run(exe, mode, game, current, home, user, windows):
    out = subprocess.run([str(exe), mode, game, current, home, user, "1" if windows else "0"], capture_output=True,
                         text=True, encoding="utf-8", timeout=60)
    if out.returncode:
        raise SystemExit("bug report driver (%s) failed:\n%s%s" % (mode, out.stdout, out.stderr))
    lines = out.stdout.splitlines()
    return [ln[7:] for ln in lines if ln.startswith("FOLDER ")], lines


def files_of(folder):
    return {p.name: p.read_text(encoding="utf-8", errors="replace") for p in folder.iterdir()}


def no_private(files, home, user, game):
    for name, text in files.items():
        for needle in (home, home.replace("\\", "/"), game.rstrip("\\/")):
            check(needle.lower() not in text.lower(), "%s names %s" % (name, needle))
        check(not re.search(r"[/\\]" + re.escape(user) + r"(?![A-Za-z0-9._-])", text, re.I), "%s names the user as a folder" % name)


def wiring():
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    at = menu.index('{TAB_GAMEPLAY, "Bug report", nullptr')
    row = menu[at:menu.index("},", at)]
    for word in ("report.txt", "logs", "picture", "user name", "No saves", "nothing is sent"):
        check(word in row, "the row's help does not say " + word)
    check(menu.index('"Bug report"') < menu.index('{TAB_GAMEPLAY, "Restart"') and
          "constexpr int kBugReportRow = kRestartRow - 1;" in menu, "the row is not the one before Restart")
    check("&r == &rows[kBugReportRow]) bug_report_save()" in menu[menu.index("void menu_tick() {"):],
          "A or Enter on the row does not save the report")
    check(menu.count('&selr == &rows[kBugReportRow] || is_share_row(selr) ? "Save" : "Open"') +
          menu.count('&r == &rows[kBugReportRow] || is_share_row(r) ? "Save" : "Open"') >= 2,
          "a prompt (the menu's, or the compact bar's) does not say Save for the row")
    save = menu[menu.index("void bug_report_save() {"):]
    save = save[:save.index("\n}\n")]
    for call in ("fill_here(in)", "device_report_text()", "changed_rows_text()", "ini_path()", "bug_report::start("):
        check(call in save, "bug_report_save does not call " + call)
    picture = menu[menu.index("bool bug_report_picture(const char* path, uint64_t ticket) {"):]
    picture = picture[:picture.index("\n}\n")]
    check("in.ask_picture = bug_report_picture;" in save and "shot_ask_next(path," in picture and
          "bug_report::picture_written(" in picture, "the report's picture is not asked of shot_ask_next")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    nxt = replay[replay.index("int shot_ask_next("):]
    check("return shot_ask(path, -1, done, ctx);" in nxt[:nxt.index("\n")], "shot_ask_next does not ask for the next present")
    record = replay[replay.index("void shot_ask_record("):]
    record = record[:record.index("\n}\n")]
    check("blit_crop(cmd, src, sw, sh, crop, ask_image," in record and "menu" not in record and "ReportShot" not in replay,
          "the picture is not copied from the game's frame into shot_ask's own image, or the menu is drawn into it")
    host = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    refresh = host[host.index("static void oot_inventory_refresh("):]
    refresh = refresh[:refresh.index("\n}\n")]
    check('hos_log("map: %s", line)' in refresh and "0x1d6189dau" in refresh,
          "the host does not log the map (Sequence_CurrentBanc) the report's place comes from")
    # (the switches the report lists are the names hookshot_env.txt takes: env_line_allowed's prefixes)
    allowed = re.search(r"prefixes\[\] = \{([^}]*)\}", host[host.index("static int env_line_allowed("):]).group(1)
    report = (ROOT / "gpu" / "vk" / "vk_bug_report.cpp").read_text(encoding="utf-8")
    listed = re.search(r"prefixes\[\] = \{([^}]*)\}", report[report.index("std::string switches("):]).group(1)
    check(listed == allowed, "the report lists switches by %s, hookshot_env.txt takes %s" % (listed, allowed))
    form = (ROOT / ".github" / "ISSUE_TEMPLATE" / "1-game-problem.yml").read_text(encoding="utf-8")
    for label in ("Hookshot version", "Where in the game", "Graphics card and driver version", "CPU, RAM and operating system",
                  "Changes from the defaults", "hookshot_log.txt", "Screenshots or video"):
        check("label: " + label + "\n" in form, "the issue form has no field " + label)
    for field in ("version", "where", "gpu", "system", "settings"):
        check("id: " + field + "\n" in form, "the issue form has no field id " + field)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    wiring()
    hw = load_hardware_report()
    windows = os.name == "nt"
    with tempfile.TemporaryDirectory(prefix="bug-report-") as tmp:
        tmp = Path(tmp)
        cpp = tmp / "driver.cpp"
        exe = tmp / ("driver.exe" if windows else "driver")
        cpp.write_text(DRIVER, encoding="utf-8")
        # (_CRT_SECURE_NO_WARNINGS: the MSVC CRT's getenv deprecation, as the game's own Windows build passes)
        subprocess.run([a.cxx, "-std=c++20", "-O1", *([] if windows else ["-pthread"]), "-Wall", "-Wextra", "-Werror",
                        "-D_CRT_SECURE_NO_WARNINGS", "-I", str(ROOT / "gpu" / "vk"), str(cpp),
                        str(ROOT / "gpu" / "vk" / "vk_bug_report.cpp"), "-o", str(exe)], check=True)
        user = "alice"
        home = str(tmp / "home" / user)
        game_path = Path(home) / "Games" / "Hookshot"
        game_path.mkdir(parents=True)
        game = str(game_path) + os.sep
        sep = "\\" if windows else "/"
        home_as = "%USERPROFILE%" if windows else "~"
        now_log = game_path / "hookshot_log.txt"
        now_log.write_text(
            "[hos host] hookshot " + VERSION + "\n"
            "[hos sead::MainThread] fs: mounted content: -> " + home + sep + "Games" + sep + "Hookshot" + sep + "romfs\n"
            "[hos sead::MainThread] fs: mounted save: -> " + home + "/Games/Hookshot/user\\save\\5a3c\n"
            "[hos MainThread] vk: pipeline cache /run/media/" + user + "/SD/Hookshot/mkw_vk_pipelines.bin (1 KB)\n"
            "[hos Main] map: KokiriForest_Child (child)\n"
            "[hos host] Malice and alice123 stay, as /x/alice123/ does\n"
            "[hos Main] map: HyruleField_Child (child)\n", encoding="utf-8")
        (game_path / "hookshot_log.1.txt").write_text(
            "[hos host] hookshot v2.0.14\n"
            "[hos sead::MainThread] fs: mounted content: -> " + home + sep + "Games" + sep + "Hookshot" + sep + "romfs\n"
            "[hos Main] map: Market_Child (child)\n"
            "[hos host] CRASH: exception 0xc0000005 (read 0x0, guest 0x0) at hookshot+0x1234\n"
            "[hos host] CRASH: thread Main, sp 0x1000\n", encoding="utf-8")

        # 1. started from its icon: this run logs to hookshot_log.txt, the one before is hookshot_log.1.txt; a picture comes
        folders, out = run(exe, "picture", game, str(now_log), home, user, windows)
        check(len(folders) == 1 and folders[0].startswith("bug-reports" + sep), "no report folder: %s" % out)
        folder = game_path / folders[0]
        files = files_of(folder)
        check(set(files) == {"report.txt", "hookshot_log.txt", "hookshot_log.1.txt", "picture.png"},
              "the folder holds %s" % sorted(files))
        check(set(p.name for p in (game_path / "bug-reports").iterdir()) == {folder.name}, "more than the report's folder")
        no_private(files, home, user, game)
        copy = files["hookshot_log.txt"]
        for kept in (home_as + sep + "Games" + sep + "Hookshot" + sep + "romfs", home_as + "/Games/Hookshot/user\\save",
                     "/run/media/<user>/SD/Hookshot", "Malice and alice123 stay, as /x/alice123/ does"):
            check(kept in copy, "the copy of this run's log does not read " + kept)
        check(len(copy.splitlines()) == len(now_log.read_text(encoding="utf-8").splitlines()), "lines lost in the copy")
        report = files["report.txt"]
        section = lambda name: hw.section(report, name)
        check(section("Hookshot version") == VERSION, "the version: " + section("Hookshot version"))
        gpu = section("Graphics card")
        check(gpu == "NVIDIA GeForce RTX 4090, NVIDIA 580.95.05" and hw.classify(gpu) == ("NVIDIA", "RTX 40"),
              "hardware_report.py reads the GPU as %s from %r" % (hw.classify(gpu), gpu))
        check(hw.os_of(section("CPU, RAM and operating system")) == "Linux / SteamOS", "hardware_report.py's OS")
        where = section("Where in the game")
        check(where.startswith("HyruleField_Child (child)") and "Market_Child (child)" in where, "where: " + where)
        settings = section("Changes from the defaults")
        lines = settings.splitlines()
        for want in ("Upscaler: NVIDIA DLSS (default: Game's)", "HOS_HEADLESS=1", "MANGOHUD=1", "RX_VK_SHOT_EVERY=60",
                     "RX_VK_X_MODE=off", "HOS_OUTPUT_RES=3840x2160", "RX_VK_NEG=-1", "RX_VK_SCALE=1.5"):
            check(want in lines, "the settings do not read " + want)
        for left_out in ("RX_VK_SHOTS", "HOS_ENV_FILE", "RX_VK_MENU_SCRIPT", "HOS_COOP_ROOM", "RX_API_KEY", "HOS_COOP_SERVER",
                         "HOS_COOP_USER", "HOS_COOP_PASSWORD", "HOS_RELAY", "HOS_RELAY_HOST", "RX_VK_LONG"):
            check(left_out + "=(value left out)" in lines, "the settings do not leave out the value of " + left_out)
        for unwanted in ("PATH=", "SECRET_TOKEN", "=C:", "93127", "58213", "47731", "hunter2", "192.168", "relay.example", "47732",
                         "1234567890123", "1:INS"):
            check(unwanted not in report, "report.txt holds " + unwanted)
        logs = section("hookshot_log.txt")
        check("hookshot v2.0.14" in logs and "CRASH: exception 0xc0000005" in logs, "the logs' section: " + logs)
        check("picture.png" in section("Screenshots or video"), "the picture is not named")
        link = report[report.index("https://github.com/NotRexed/Hookshot/issues/new?template=1-game-problem.yml"):].split()[0]
        for part in ("&version=v2.1.0-rc.3-39-gbdfe09c%20%28source%20763271ad%29", "&gpu=NVIDIA%20GeForce%20RTX%204090",
                     "&where=HyruleField_Child%20%28child%29", "&system=", "&settings=Upscaler", "HOS_HEADLESS", "RX_VK_MENU_SCRIPT"):
            check(part in link, "the link does not fill in " + part)
        check("%3D" not in link and not any(v in link for v in ("93127", "58213", "hunter2", "192.168", "3840x2160", "1%3AINS")),
              "the link carries a switch's value: " + link)
        check(len(link) <= 2000 and "in short" not in report, "the link: %d characters" % len(link))
        check(any("menu: bug report written to " + folders[0] in ln for ln in out), "nothing logged: %s" % out)

        # 2. the menu's way: on a thread, its state read each frame; no frame comes, so no picture
        folders, out = run(exe, "start", game, str(now_log), home, user, windows)
        files = files_of(game_path / folders[0])
        check("picture.png" not in files and "No picture" in hw.section(files["report.txt"], "Screenshots or video"),
              "a report without a frame: %s" % sorted(files))

        # 2b. the picture comes after the report went without it (no present in time): the report says it has none, and the
        # file is removed when it comes
        folders, out = run(exe, "late", game, str(now_log), home, user, windows)
        files = files_of(game_path / folders[0])
        check("LATE removed (asked for the report's own picture.png)" in out and "picture.png" not in files and
              "No picture" in hw.section(files["report.txt"], "Screenshots or video"), "a late picture: %s %s" % (out, sorted(files)))

        # 3. started from a terminal: no log file this run; the run before's is the game folder's hookshot_log.txt
        folders, out = run(exe, "terminal", game, "", home, user, windows)
        check(len(folders) == 2 and folders[0] != folders[1] and all(folders), "two reports in a second: %s" % folders)
        files = files_of(game_path / folders[0])
        check(set(files) == {"report.txt", "hookshot_log.1.txt"}, "from a terminal the folder holds %s" % sorted(files))
        check("HyruleField_Child" in files["hookshot_log.1.txt"], "the run before's log is not the game folder's last one")
        report = files["report.txt"]
        check("terminal" in hw.section(report, "Where in the game") and "terminal" in hw.section(report, "hookshot_log.txt"),
              "the report does not say where this run's log is")
        no_private(files, home, user, game)

        # 4. a log over 10 MB: its first MB and its last 9 MB, and the map in its middle still counts
        big = tmp / "big.log"
        with open(big, "w", encoding="utf-8", newline="\n") as f:
            f.write("[hos host] hookshot " + VERSION + "\n")
            filler = "[hos host] vk: status: " + "x" * 100 + " " + home + "/Games\n"
            for _ in range(60000):
                f.write(filler)
            f.write("[hos Main] map: DeathMountainTrail_Child (child)\n")
            for _ in range(60000):
                f.write(filler)
            f.write("[hos host] the last line\n")
        folders, out = run(exe, "big", game, str(big), home, user, windows)
        copy_path = game_path / folders[0] / "hookshot_log.txt"
        copy = copy_path.read_text(encoding="utf-8")
        check(copy_path.stat().st_size <= (10 << 20) + 4096, "the long log's copy is %d bytes" % copy_path.stat().st_size)
        check(copy.startswith("[hos host] hookshot " + VERSION) and copy.endswith("[hos host] the last line\n") and
              "of this log's middle left out here" in copy, "the long log's copy lost its start, its end or the note")
        report = (game_path / folders[0] / "report.txt").read_text(encoding="utf-8")
        check(hw.section(report, "Where in the game").startswith("DeathMountainTrail_Child (child)"), "the middle's map")
        check(home not in copy, "the long log's copy names the home folder")

        # 4b. more switches than a link takes: the link stays within 2000 characters and carries the options and how many
        # switches there are; report.txt lists them all
        folders, out = run(exe, "many", game, str(now_log), home, user, windows)
        report = (game_path / folders[0] / "report.txt").read_text(encoding="utf-8")
        link = report[report.index("https://github.com/NotRexed/Hookshot/issues/new?template=1-game-problem.yml"):].split()[0]
        check(len(link) <= 2000, "the link with 98 switches: %d characters" % len(link))
        settings = hw.section(report, "Changes from the defaults")
        listed = [ln for ln in settings.splitlines() if re.match(r"[A-Z_][A-Z0-9_]*=", ln)]
        check("&settings=Upscaler%3A%20NVIDIA%20DLSS" in link and
              "Switches%%20set%%3A%%20%d%%2C%%20listed%%20in%%20the%%20attached%%20report.txt" % len(listed) in link and
              "RX_TEST_SWITCH" not in link and "in short" in report, "the shortened link: " + link)
        check(all("RX_TEST_SWITCH_%d=(value left out)" % i in listed for i in range(80)) and "HOS_HEADLESS=1" in listed and
              "v" * 20 not in report, "report.txt does not list every switch, or writes a value it should leave out")

        # 5. this PC: the system as the form asks it, read by hardware_report.py as this OS
        system = next(ln[7:] for ln in out if ln.startswith("SYSTEM "))
        check(hw.os_of(system) == ("Windows" if windows else "Linux / SteamOS") and " GB, " in system,
              "this PC's system reads " + system)

        # 6. NUL bytes, one in a line and a run of them at the end (what a power cut or a hard kill leaves): the copy is the
        # log byte for byte, its paths redacted, and the map after the NUL is the place. (The file is not named nul.log: on
        # Windows NUL with any extension is the null device, so nothing was written and an empty log was read back; the
        # read-back below says so if a name ever does that again)
        nul = tmp / "power-cut.log"
        nul_text = ("[hos host] hookshot " + VERSION + "\n[hos Main] fs: mounted content: -> " + home + "/Games\n"
                    "[hos Main] map: KokiriForest_Child (child)\n[hos Main] half a line\0 [hos Main] map: LostWoods_Child (child)\n" +
                    "\0" * 4096)
        nul.write_bytes(nul_text.encode("utf-8"))
        check(nul.read_bytes() == nul_text.encode("utf-8"), "the test's own log with NUL bytes was not written as given")
        folders, out = run(exe, "plain", game, str(nul), home, user, windows)
        copy = (game_path / folders[0] / "hookshot_log.txt").read_bytes()
        check(copy == nul_text.replace(home, home_as).encode("utf-8"), "the copy of a log with NUL bytes differs: %r" % copy[-200:])
        report = (game_path / folders[0] / "report.txt").read_text(encoding="utf-8")
        check(hw.section(report, "Where in the game").startswith("LostWoods_Child (child)"), "the map after a NUL is lost")

        # 7. a report's folder that cannot be made (bug-reports is a file here): the system's reason, GetLastError's on Windows
        blocked = tmp / "blocked" / "Hookshot"
        blocked.mkdir(parents=True)
        (blocked / "bug-reports").write_text("a file where the folder would go", encoding="utf-8")
        folders, out = run(exe, "plain", str(blocked) + os.sep, str(now_log), home, user, windows)
        said = next((ln for ln in out if "menu: bug report not written:" in ln), "")
        check(folders == [""] and ("(Windows error " if windows else "(Not a directory)") in said, "no folder: %s" % out)
    print("bug report: ok (report.txt as the issue form, both logs redacted, the picture, the long log, the terminal start, "
          "switch values, NUL bytes, the folder's reason)")


if __name__ == "__main__":
    main()
