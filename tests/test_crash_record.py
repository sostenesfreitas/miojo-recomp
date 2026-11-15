"""Game-free crash records, real fatal children, and locked unclean-end markers.

Uses the real writer, crash adapter, hos_fatal/logger, control quit and restart
functions. Linux uses the real POSIX exception adapter with core dumps disabled;
Windows uses the real exception filter and optionally DbgHelp on a tiny child.
"""
import argparse
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    assert text.count(marker) == 1, marker
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def source_contract():
    glue = (ROOT / "host/mkw_glue.c").read_text(encoding="utf-8")
    threads = (ROOT / "host/hos_thread.c").read_text(encoding="utf-8")
    report = block(glue, "static LONG WINAPI report_crash(")
    assert report.index("record_exception(info);") < report.index("GetModuleHandleExA(")
    assert 'if (code == 0xC0000374 || code == 0xC0000409) report_crash(info);' in glue
    assert 'SetUnhandledExceptionFilter(report_crash);' in glue
    assert 'AddVectoredExceptionHandler(1, commit_on_touch);' in glue
    for line in ('CRASH: exception %#lx (%s %#llx, guest %#llx) at %s+%#llx',
                 'CRASH: host backtrace:', 'CRASH: thread %s, sp %#llx',
                 'CRASH: guest backtrace: lr %#llx',
                 'CRASH: [x%d+%#05x] %016llx %016llx %016llx %016llx'):
        assert line in report, line
    fatal = block(threads, "void hos_fatal(")
    assert fatal.index("hos_crash_record_fatal(") < fatal.index("vsnprintf(")
    assert "hos_log_flush();" in report and "log_flush();" in fatal
    writer = block((ROOT / "host/hos_crash_record.c").read_text(encoding="utf-8"), "int hos_crash_record_write(")
    assert "hos_log(" not in writer and "hos_crash_record_log_notes(" not in writer
    renderer = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    replay = block(renderer, "[[noreturn]] void replay_fatal(")
    assert replay.index('logf("vk: FATAL:') < replay.index("std::_Exit(3)")
    assert 'strstr(buf, "VK_ERROR_DEVICE_LOST") ? "device lost"' in threads
    logger = block(threads, "static void hos_vlog(")
    assert logger.index("hos_crash_record_fatal(") < logger.index("snprintf(line,")
    for marker in ("void hos_log(", "void hos_log_at("):
        wrapper = block(threads, marker)
        assert "hos_vlog(" in wrapper and "hos_crash_record_fatal(" not in wrapper
    main = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    prepare = main.index("hos_crash_record_prepare(path);")
    rotate = main.index("MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);")
    assert main.index('memcpy(path + n, "hookshot_log.txt", 17);') < prepare < rotate < main.index('freopen(path, "w", stderr)')
    assert "else hos_crash_record_prepare(NULL);" in main
    assert "hos_crash_record_start(path, hos_version(), log_path);" in block(glue, "void hos_crash_record_prepare(")
    banner = main[main.index('    hos_log("hookshot %s", HOOKSHOT_VERSION);'):main.index("    removed_switch_notes();")]
    assert re.findall(r'hos_log\("([^"]+)"', banner)[:2] == ["hookshot %s", "run_id: %s"]
    assert banner.index('hos_log("run_id: %s",') < banner.index("hos_game_layout_log();")
    assert banner.index('hos_log("run_id: %s",') < banner.index("hos_crash_record_log_notes();")
    assert main.index("hos_game_layout();") < prepare
    linux_main = main[main.rindex("int main(int argc, char **argv) {"):]
    assert linux_main.index("hos_game_layout();") < linux_main.index("hos_display_init();")
    assert "hos_crash_clean_end();" in block(main, "static DWORD WINAPI game_thread(")

    # An exhaustive inventory: a new process exit must be classified, not silently
    # start generating unclean records. Declarations and comments are not calls.
    expected = {
        "control.c": ["ExitProcess(0);"],
        "display.c": ["ExitProcess(1);"],
        "hos_main.c": ["ExitProcess(result);"],
        "hos_relaunch.c": ["ExitProcess(0);"],
        "hos_svc.c": ["ExitProcess(0);"],
        "hos_thread.c": ["ExitProcess(3);"],  # fatal: record, not a clean end
        "posix/display_sdl.c": ["ExitProcess(1);"],
        "posix/win32_posix.c": ["_exit((int)code);", "ExitProcess(code);"],  # platform implementations
    }
    observed = {}
    for path in (ROOT / "host").rglob("*.c"):
        text = path.read_text(encoding="utf-8")
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
        calls = []
        for match in re.finditer(r"\b(?:ExitProcess|_exit|_Exit)\s*\([^;\n]*\);", text):
            prefix = text[text.rfind("\n", 0, match.start()) + 1:match.start()]
            if re.search(r"\bvoid\s*$", prefix):
                continue
            calls.append(match.group())
            rel = path.relative_to(ROOT / "host").as_posix()
            exempt = rel == "hos_thread.c" or rel == "posix/win32_posix.c"
            if not exempt:
                assert "hos_crash_clean_end();" in text[max(0, match.start() - 180):match.start()], rel
        if calls:
            observed[path.relative_to(ROOT / "host").as_posix()] = calls
    assert observed == expected, (observed, expected)
    assert not (ROOT / "host/hos_update.c").exists(), "the removed in-game updater must not return"
    print("PASS: every remaining host process exit classified; normal exits mark, fatal exits do not")
    return glue, threads, main


def build(cc, temp, glue, threads, main):
    template = (ROOT / "tests/crash_record_test.c").read_text(encoding="utf-8")
    header = (ROOT / "host/hos.h").read_text(encoding="utf-8")
    start = header.index("enum { HOS_LOG_ERROR")
    levels = header[start:header.index(";", start) + 1]
    replacements = {
        "RECORD_EXCEPTION": block(glue, "static void record_exception("),
        "FATAL_ADAPTER": block(glue, "void hos_crash_record_fatal("),
        "REPORT_FATAL": block(glue, "static LONG CALLBACK report_fatal("),
        "HOS_LOG": "\n".join((levels, block(threads, "static const char *log_level_tag("),
                               block(threads, "static void hos_vlog("), block(threads, "void hos_log("),
                               block(threads, "void hos_log_at("))),
        "HOS_FATAL": block(threads, "void hos_fatal("),
        "CONTROL_QUIT": block((ROOT / "host/control.c").read_text(encoding="utf-8"), "static void game_quit("),
        "WINDOW_EXIT": block((ROOT / "host/hos_relaunch.c").read_text(encoding="utf-8"), "void hos_window_closed_exit("),
        "GAME_THREAD": block(main, "static DWORD WINAPI game_thread("),
    }
    shim = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    replacements["POSIX_ADAPTER"] = (shim[shim.index("/* ---- exceptions ---- */"):
                                             shim.index("/* ---- the window's state,")] +
                                     block(shim, "void ExitProcess(UINT code) {")) if os.name != "nt" else ""
    for name, text in replacements.items():
        template = template.replace("/* " + name + " */", text)
    src, exe = temp / "fixture.c", temp / "fixture.exe"
    src.write_text(template, encoding="utf-8")
    cmd = [cc, "-std=gnu11", "-O1", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
           "-I", str(ROOT / "host"), str(src), str(ROOT / "host/hos_report_run.c"), "-o", str(exe)]
    if os.name == "nt":
        cmd += ["-ldbghelp", "-ladvapi32"]
    else:
        cmd += ["-I", str(ROOT / "host/posix"), "-pthread", "-ldl"]
    subprocess.run(cmd, check=True, timeout=120)
    return exe


def records(folder, kind="crash"):
    return sorted((folder / "bug-reports/pending").glob(kind + "-*.txt"))


def checked_record(path, kind="crash"):
    text = path.read_text(encoding="utf-8")
    assert text.startswith("Hookshot " + kind + " record 1\n"), text
    lines = text.splitlines()[1:]
    assert all(re.fullmatch(r"[a-z_]+: [^\r\n]*", line) for line in lines), text
    values = dict(line.split(": ", 1) for line in lines)
    for key in ("version", "when", "uptime_s", "reason", "thread", "module_offset", "last_present_age_ms", "log", "pid"):
        assert key in values, key
    assert values["version"] == "fixture-v1-g1234"
    assert values["record_complete"] == "yes"
    assert re.fullmatch(r"\d{8}T\d{6}Z", values["when"])
    assert re.fullmatch(r"[0-9a-f]{32}", values["run_id"])
    assert re.fullmatch(r"(?:crash|unclean)-\d{8}T\d{6}Z(?:-[\d-]+)?\.txt", path.name)
    for secret in ("private-user", "private-host", "/home/", "C:\\Users\\", os.environ.get("HOME", "")):
        if secret:
            assert secret not in text, (secret, text)
    return text, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("gcc") or "clang")
    args = parser.parse_args()
    glue, threads, host_main = source_contract()
    with tempfile.TemporaryDirectory(prefix="hookshot-crash-record-") as tmp:
        temp = Path(tmp)
        exe = build(args.cc, temp, glue, threads, host_main)
        env = os.environ.copy()
        env.update(USER="private-user", USERNAME="private-user", HOS_CRASH_MINIDUMP="0")
        count = 0

        def run(folder, mode, expected=0, extra=None):
            nonlocal count
            result = subprocess.run([str(exe), str(folder), mode], env=env | (extra or {}),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=20)
            if expected is None:
                assert result.returncode != 0, (mode, result)
            else:
                assert result.returncode == expected, (mode, result.returncode, result.stderr)
            count += 1
            return result

        for mode in ("clean", "return", "control", "restart", "game-return", "denied", "device-mention"):
            folder = temp / mode; folder.mkdir()
            run(folder, mode)
            assert not records(folder) and not records(folder, "unclean")
            assert not list((folder / "bug-reports/running").glob("*.running"))
        print("PASS: clean return, explicit clean, control quit, restart, game return, denied storage")
        if os.name != "nt":
            folder = temp / "signal-metadata"; folder.mkdir()
            run(folder, "signal-metadata")
            assert not records(folder) and not records(folder, "unclean")
            print("PASS: sent signals re-raised; real faults return to preserve original core siginfo")
            folder = temp / "module-offsets"; folder.mkdir()
            run(folder, "module-offsets")
            print("PASS: module offsets equal dladdr load-base offsets; gapped segments, same basenames and repeated mappings covered")

        device_modes = ("device-lost", "device-early", "device-lost-tagged", "device-early-tagged")
        for mode in ("fake", "partial", "privacy", "namespace", "bounds", "fatal", *device_modes, "crash", "signal", "abort", "heap"):
            folder = temp / mode; folder.mkdir()
            result = run(folder, mode, 3 if mode == "fatal" or mode in device_modes else None if mode in ("crash", "signal", "abort", "heap") else 0)
            paths = records(folder); assert len(paths) == 1, (mode, paths)
            text, values = checked_record(paths[0])
            assert "[hos host] run_id: " + values["run_id"] + "\n" in result.stderr
            assert values["dump"] == "off"
            if mode == "bounds":
                assert values["thread"] == "unknown" and values["module_offset"] == "unknown"
                assert "guest_register:" not in text
            else:
                assert "+0x" in text and "host_backtrace:" in text
                if mode not in device_modes:
                    assert "guest_register: x0=0x1000" in text and "guest_backtrace: 0x80005678" in text, (mode, text)
            if mode == "privacy":
                assert values["thread"] == "redacted"
            if mode == "namespace":
                assert values["thread"] == "sead::MainThread"
            if mode in device_modes:
                assert values["reason"] == "device lost"
                assert result.stderr.count("vk: FATAL:") == 2  # no writer recursion or duplicate record
                if mode.endswith("-tagged"):
                    assert result.stderr.count("[error] vk: FATAL:") == 2
                if mode == "device-early-tagged":
                    assert "[warn] vk: device lost: reading the checkpoints" in result.stderr
            if mode == "signal" and os.name != "nt":
                assert values["signal"] == str(signal.SIGSEGV)
                assert int(values["signal_code"]) <= 0
            if mode == "crash" and os.name != "nt":
                assert values["signal_code"] == "1"  # SEGV_MAPERR: the authored 0x40 fault, not SI_TKILL
            run(folder, "clean")
            assert not records(folder, "unclean") and records(folder) == paths
        print("PASS: allocation-poisoned writer, bounds, short writes/EINTR, privacy, fatal/device-lost and real fatal children")

        folder = temp / "disk-full"; folder.mkdir()
        run(folder, "disk-full")
        assert not records(folder)
        run(folder, "clean")
        assert len(records(folder, "unclean")) == 1
        for mode in ("staged", "published-marker"):
            folder = temp / mode; folder.mkdir()
            run(folder, mode, 3)
            run(folder, "clean")
            assert len(records(folder)) == 1 and not records(folder, "unclean")
        print("PASS: failed writes retain a marker; durable staged/published crashes never become unclean ends")

        folder = temp / "collision"; folder.mkdir()
        pending = folder / "bug-reports/pending"; pending.mkdir(parents=True)
        reserved = [pending / "crash-19700101T000000Z.txt"]
        reserved[0].write_text("preserved\n", encoding="utf-8")
        run(folder, "collision")
        added = set(records(folder)) - set(reserved)
        assert len(added) == 1 and re.search(r"Z-\d+-\d+\.txt$", next(iter(added)).name)
        assert all(p.read_text(encoding="utf-8") == "preserved\n" for p in reserved)
        print("PASS: same-second publication cannot overwrite another record")

        folder = temp / "retention"; folder.mkdir()
        pending = folder / "bug-reports/pending"; pending.mkdir(parents=True)
        owned = []
        for index in range(40):
            kind = "crash" if index % 2 else "unclean"
            stamp = (datetime(2000, 1, 1, tzinfo=timezone.utc) + timedelta(seconds=index)).strftime("%Y%m%dT%H%M%SZ")
            path = pending / (kind + "-" + stamp + ".txt")
            owned.append(path)
        for path in reversed(owned):  # mtime/creation order is the opposite of the required UTC order
            kind, stamp = path.stem.split("-", 1)
            path.write_text("Hookshot " + kind + " record 1\nwhen: " + stamp + "\n", encoding="utf-8")
        target = temp / "untouched-target.txt"; target.write_text("outside pending\n", encoding="utf-8")
        link = pending / "crash-20990101T000000Z.txt"
        prune_mode = "clean"
        try:
            link.symlink_to(target)
        except OSError:
            if os.name != "nt":
                raise
            link.write_text("authored reparse-point stand-in\n", encoding="utf-8")
            prune_mode = "prune-reparse"
            print("Windows symlink creation unavailable: actual reparse-attribute branch exercised with an authored stand-in")
        directory = pending / "unclean-20990101T000001Z.txt"; directory.mkdir()
        lookalikes = [pending / "stall-20000101T000000Z.txt", pending / "crash-20000101T000000Z.txt.bak",
                      pending / "unclean-20000101T000000Z-12--3.txt"]
        local_dump = folder / "bug-reports/local-only/authored.dmp"; local_dump.parent.mkdir()
        completed = folder / "bug-reports/completed/report.txt"; completed.parent.mkdir()
        for path in [*lookalikes, local_dump, completed]:
            path.write_text("untouched\n", encoding="utf-8")
        run(folder, prune_mode)
        assert [p for p in owned if p.exists()] == owned[-15:]
        assert link.exists() and directory.is_dir() and target.read_text(encoding="utf-8") == "outside pending\n"
        assert all(p.read_text(encoding="utf-8") == "untouched\n" for p in [*lookalikes, local_dump, completed])
        folder = temp / "prune-denied"; folder.mkdir()
        pending = folder / "bug-reports/pending"; pending.mkdir(parents=True)
        for index in range(16):
            (pending / ("crash-20000101T0000%02dZ.txt" % index)).write_text("retained\n", encoding="utf-8")
        denied = run(folder, "prune-denied")
        assert len(records(folder)) == 16
        assert denied.stderr.count("crash records: some old pending records could not be removed") == 1
        print("PASS: newest15 of 40 retained; symlink/directory/stall/near-miss/local-only/completed files untouched; one deletion warning")

        folder = temp / "two-live"; folder.mkdir()
        children = []
        try:
            for _ in range(2):
                child = subprocess.Popen([str(exe), str(folder), "hold"], env=env, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                children.append(child)
                assert child.stdout.readline().strip() == "ready"
            run(folder, "clean")
            assert not records(folder, "unclean")
            children[0].kill(); children[0].wait(timeout=10)
            run(folder, "clean")
            paths = records(folder, "unclean"); assert len(paths) == 1
            _, values = checked_record(paths[0], "unclean")
            assert values["reason"] == "ended without a clean exit" and values["crash_handler"] == "not observed"
            assert values["pid"] == str(children[0].pid)
            children[1].communicate("\n", timeout=10)
            run(folder, "clean")
            assert records(folder, "unclean") == paths
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill(); child.wait(timeout=10)
        folder = temp / "stale-live"; folder.mkdir()
        child = subprocess.Popen([str(exe), str(folder), "stale-live"], env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            assert child.stdout.readline().strip() == "ready"
            run(folder, "clean")
            assert len(records(folder, "unclean")) == 1  # PID alive is not proof this old marker is owned
            child.communicate("\n", timeout=10)
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=10)
        print("PASS: two live owners, real forced kill recovery, and stale marker with a live reused PID")

        if os.name != "nt":
            for signo in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
                folder = temp / ("outside-" + str(signo)); folder.mkdir()
                child = subprocess.Popen([str(exe), str(folder), "hold"], env=env, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    assert child.stdout.readline().strip() == "ready"
                    child.send_signal(signo); assert child.wait(timeout=10) == -signo
                finally:
                    if child.poll() is None:
                        child.kill(); child.wait(timeout=10)
                run(folder, "clean")
                _, values = checked_record(records(folder, "unclean")[0], "unclean")
                assert values["termination_note"] == "ended from outside or lost power, not necessarily a crash"
                assert not records(folder)
            print("PASS: SIGTERM/SIGINT/SIGHUP remain explicitly unclean observations, not crashes")

        folder = temp / "old-log"; folder.mkdir()
        old_log = folder / "previous.log"
        with old_log.open("w+", encoding="utf-8") as log:
            child = subprocess.Popen([str(exe), str(folder), "hold"], env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=log, text=True)
            try:
                assert child.stdout.readline().strip() == "ready"
                log.seek(0)
                old_id = re.search(r"run_id: ([0-9a-f]{32})", log.read()).group(1)
                log.seek(0, 2)
                log.write("[hos host] FATAL: private-user /home/private-user token\n")
                log.flush()
                child.kill(); child.wait(timeout=10)
            finally:
                if child.poll() is None:
                    child.kill(); child.wait(timeout=10)
        fresh = run(folder, "clean")
        text, values = checked_record(records(folder, "unclean")[0], "unclean")
        assert values["run_id"] == old_id and old_id not in fresh.stderr
        assert values["log_tail_fatal"] == "yes" and values["log_tail_crash"] == "no"
        assert int(values["log_size_bytes"]) == old_log.stat().st_size
        assert values["log"] == "previous.log" and "token" not in text
        print("PASS: unclean record keeps previous run id and bounded log facts, never private log text")

        for relative in ("logs/hookshot_log.txt", "hookshot_log.txt"):
            folder = temp / ("selected-" + relative.split("/")[0]); folder.mkdir()
            selected = folder / relative
            selected.parent.mkdir(exist_ok=True)
            child = subprocess.Popen([str(exe), str(folder), "hold", str(selected)], env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                assert child.stdout.readline().strip() == "ready"
                child.kill(); _, previous = child.communicate(timeout=10)
            finally:
                if child.poll() is None:
                    child.kill(); child.wait(timeout=10)
            old_id = re.search(r"run_id: ([0-9a-f]{32})", previous).group(1)
            selected.write_text("[hos host] run_id: " + old_id + "\n[hos host] FATAL: private-user token\n", encoding="utf-8")
            fresh = run(folder, "clean")
            text, values = checked_record(records(folder, "unclean")[0], "unclean")
            assert values["run_id"] == old_id and old_id not in fresh.stderr
            assert values["log"] == "hookshot_log.txt" and values["log_tail_fatal"] == "yes"
            assert int(values["log_size_bytes"]) == selected.stat().st_size
            assert "token" not in text
        print("PASS: selected logs/ destination and beside-game fallback retain previous run id and log facts")

        if os.name == "nt":
            folder = temp / "fatal-no-exception-dump"; folder.mkdir()
            run(folder, "fatal", 3, {"HOS_CRASH_MINIDUMP": "1"})
            _, values = checked_record(records(folder)[0])
            assert values["dump"] == "off" and not list((folder / "bug-reports/local-only").glob("*.dmp"))
            folder = temp / "native-dump"; folder.mkdir()
            run(folder, "crash", None, {"HOS_CRASH_MINIDUMP": "1"})
            text, values = checked_record(records(folder)[0])
            assert values["dump"] == "requested locally, not for sharing"
            dumps = list((folder / "bug-reports/local-only").glob("*.dmp"))
            assert len(dumps) == 1 and dumps[0].read_bytes()[:4] == b"MDMP"
            assert not list((folder / "bug-reports/pending").glob("*.dmp"))
            outcomes = list((folder / "bug-reports/local-only").glob("*.outcome.txt"))
            assert len(outcomes) == 1 and "written locally;" in outcomes[0].read_text(encoding="utf-8")
            print("PASS: native Windows tiny-child minidump, %d bytes, local-only" % dumps[0].stat().st_size)
        else:
            folder = temp / "read-only"; folder.mkdir(); folder.chmod(0o500)
            try:
                if os.geteuid() != 0:
                    result = subprocess.run([str(exe), str(folder), "read-only"], env=env, capture_output=True, text=True, timeout=20)
                    assert result.returncode == 0
                    assert not records(folder, "unclean")
            finally:
                folder.chmod(0o700)
            print("Windows DbgHelp execution: not run on Linux (native Windows case is part of this driver)")
        print("PASS: %d completed child runs plus locked-owner/kill cases" % count)


if __name__ == "__main__":
    main()
