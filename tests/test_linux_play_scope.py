"""Linux Builder's Play starts the game in a systemd scope of its own when the launcher runs in a systemd service (#2529):
a desktop that starts each launcher as app-<name>@<id>.service stops the service, and the game with it, when the window
closes. The scope is only asked for from a .service, only when systemd-run is there, and only when a first systemd-run
works. The real launch is then acknowledged before closing; a failed or timed-out runner cannot start a second game."""
import argparse
import ast
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / "builder/linux"
SOURCE = BUILDER / "hookshot_builder.py"

SERVICE = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-hookshot@0123abcd.service\n"
SCOPE = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-hookshot-4242.scope\n"
TOOL = "/usr/bin/systemd-run"
PREFIX = [TOOL, "--user", "--scope", "--collect", "--quiet", "--"]


def functions(*names):
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"))
    helpers = {"read_cgroup", "wait_scope", "start_scoped"}
    found = {node.name: node for node in tree.body
             if isinstance(node, ast.FunctionDef) and (node.name in names or node.name in helpers)}
    missing = set(names) - set(found)
    assert not missing, "hookshot_builder.py no longer has %s: update this test" % ", ".join(sorted(missing))
    namespace = dict(os=os, shutil=shutil, signal=signal, socket=socket, subprocess=subprocess, time=time)
    exec(compile(ast.Module(body=list(found.values()), type_ignores=[]), str(SOURCE), "exec"), namespace)
    return namespace


def check_own_scope(directory):
    own_scope = functions("own_scope")["own_scope"]
    env = {"PATH": "/usr/local/bin:/usr/bin"}

    def cgroup(text):
        path = Path(directory) / "cgroup"
        path.write_text(text, encoding="utf-8")
        return str(path)

    calls = []

    def run_ok(command, **options):
        calls.append((command, options))
        return SimpleNamespace(returncode=0)

    def which(name, path=None):
        assert name == "systemd-run" and path == env["PATH"], (name, path)
        return TOOL

    # In a launcher's service, with a user manager: the scope, after one trial run of true in it, in the game's environment.
    assert own_scope(env, cgroup(SERVICE), which, run_ok) == PREFIX
    assert len(calls) == 1 and calls[0][0] == PREFIX + ["true"], calls
    assert calls[0][1]["env"] is env and calls[0][1]["timeout"] > 0
    assert all(calls[0][1][k] is subprocess.DEVNULL for k in ("stdin", "stdout", "stderr"))

    # A hybrid hierarchy has cgroup v1 lines before the unified one.
    assert own_scope(env, cgroup("12:pids:/user.slice\n1:name=systemd:/user.slice\n" + SERVICE), which, run_ok) == PREFIX

    def never(*args, **kwargs):
        raise AssertionError("own_scope ran systemd-run where nothing has to change")

    # Nothing to do: a scope (a terminal, or a desktop that starts launchers in scopes) ends only when the game does,
    # cgroup v1 alone, no /proc, the root.
    for text in (SCOPE, "1:name=systemd:/user.slice/app-hookshot@1.service\n", "0::/\n", ""):
        assert own_scope(env, cgroup(text), never, never) == [], text
    assert own_scope(env, str(Path(directory) / "missing"), never, never) == []

    # No systemd-run: as before.
    assert own_scope(env, cgroup(SERVICE), lambda name, path=None: None, never) == []

    # A user manager that cannot be reached: as before, so that Play still starts the game.
    def failing(code=None, error=None):
        def run(command, **options):
            if error:
                raise error
            return SimpleNamespace(returncode=code)
        return run

    for run in (failing(code=1), failing(error=OSError("exec")),
                failing(error=subprocess.TimeoutExpired(PREFIX, 10))):
        assert own_scope(env, cgroup(SERVICE), which, run) == []


def check_play(directory):
    folder = Path(directory) / "game"
    folder.mkdir()
    launched = []

    def popen(command, **options):
        assert options["start_new_session"] and options["cwd"] == str(folder)
        launched.append(command)

    def scoped(scope, command, options):
        popen(scope + command, **options)
        return True

    namespace = functions("play")
    namespace.update(
        HERE=str(BUILDER), own_scope=lambda env: list(PREFIX), start_scoped=scoped,
        linux_update=SimpleNamespace(game_exe=lambda f: str(Path(f) / "hookshot")),
        make_game=SimpleNamespace(game_env=lambda: {"PATH": "/usr/bin"}),
        subprocess=SimpleNamespace(Popen=popen, DEVNULL=subprocess.DEVNULL))
    exe = os.path.abspath(str(folder / "hookshot"))

    # The diagnostics script, in the scope.
    namespace["play"](str(folder))
    command = launched[-1]
    assert command[:len(PREFIX)] == PREFIX and command[len(PREFIX):len(PREFIX) + 2] == ["/bin/sh", "-c"], command
    assert command[-2:] == ["hookshot-play", exe], command

    # The game alone (the script could not be read), in the scope too.
    namespace["HERE"] = str(Path(directory) / "no-builder")
    namespace["play"](str(folder))
    assert launched[-1] == PREFIX + [exe], launched[-1]

    # Nothing to do: the command is the one Play always ran.
    namespace.update(HERE=str(BUILDER), own_scope=lambda env: [])
    namespace["play"](str(folder))
    assert launched[-1][:2] == ["/bin/sh", "-c"], launched[-1]


def check_handoff(directory, case):
    """Inject the clock, cgroups and runner: no systemd, real sleeping or game required, on either OS."""
    namespace = functions("own_scope", "play")
    folder = Path(directory) / case
    folder.mkdir()
    proc_root = folder / "proc"
    (proc_root / "self").mkdir(parents=True)
    (proc_root / "1234").mkdir()
    parent = proc_root / "self/cgroup"
    child = proc_root / "1234/cgroup"
    parent.write_text(SERVICE, encoding="utf-8")
    child.write_text(SCOPE if case == "ready" else SERVICE, encoding="utf-8")
    events, sleeps, launched, gates = [], [], [], []
    now = 0.0

    def clock():
        return now

    def sleep(seconds):
        nonlocal now
        sleeps.append(seconds)
        now += seconds
        if case == "delayed" and now >= 0.25:
            child.write_text(SCOPE, encoding="utf-8")

    class Runner:
        pid = 1234
        returncode = None

        def poll(self):
            return self.returncode

        def wait(self, timeout=None):
            events.append("reaped")
            assert self.returncode is not None, "an abandoned runner was not stopped"
            return self.returncode

    runner = Runner()

    def killpg(pid, sig):
        assert pid == runner.pid and sig == 9
        events.append("cancelled")
        runner.returncode = -9

    def popen(command, **options):
        launched.append(command)
        assert options["start_new_session"] and options["cwd"] == str(folder)
        if command[:len(PREFIX)] == PREFIX:
            events.append("scope")
            if case == "exec-error":
                raise OSError("scope exec failed")
            if case == "failed":
                runner.returncode = 72
            if isinstance(options["stdout"], socket.socket):
                gates.append(options["stdout"].dup())   # what the child inherits, until it is reaped
            return runner
        events.append("plain")
        return SimpleNamespace()

    # Use the real choice function with a genuinely successful (injected) first trial in every case.
    own_scope = namespace["own_scope"]

    def probe(command, **options):
        assert command == PREFIX + ["true"]
        events.append("probe")
        return SimpleNamespace(returncode=0)

    namespace.update(
        HERE=str(BUILDER),
        own_scope=lambda env: own_scope(env, str(parent), lambda *a, **kw: TOOL, probe),
        linux_update=SimpleNamespace(game_exe=lambda f: str(Path(f) / "hookshot")),
        make_game=SimpleNamespace(game_env=lambda: {"PATH": "/usr/bin"}),
        os=SimpleNamespace(path=os.path, killpg=killpg), signal=SimpleNamespace(SIGKILL=9),   # (Windows has no SIGKILL)
        subprocess=SimpleNamespace(Popen=popen, DEVNULL=subprocess.DEVNULL, TimeoutExpired=subprocess.TimeoutExpired))
    if "start_scoped" in namespace:
        start_scoped, wait_scope = namespace["start_scoped"], namespace["wait_scope"]
        namespace["start_scoped"] = lambda scope, command, options: start_scoped(
            scope, command, options, wait=lambda p: wait_scope(
                p, proc_root=str(proc_root), clock=clock, sleep=sleep, timeout=2))

    try:
        namespace["play"](str(folder))
        permission = None
        if gates:
            gates[0].setblocking(False)
            permission = gates[0].recv(16)
        assert events.count("probe") == 1 and events.count("scope") == 1, events
        if case in ("failed", "exec-error", "timeout"):
            assert events.count("plain") == 1, "scope failure did not fall back exactly once: %s" % events
            assert permission in (None, b""), "an abandoned scoped game was also authorised"
            assert "systemd scope hand-off failed" in launched[-1][-1], launched[-1]
            if case == "timeout":
                assert now == 2 and events.index("cancelled") < events.index("reaped") < events.index("plain"), events
                # A late move cannot start a second game: the gate closed without a newline/permission.
                child.write_text(SCOPE, encoding="utf-8")
                assert permission == b""
            elif case == "failed":
                assert not sleeps and events.index("reaped") < events.index("plain"), events
        else:
            assert permission == b"go\n" and "plain" not in events, \
                "Play returned without acknowledging the real hand-off: %s" % events
            assert "cancelled" not in events and "reaped" not in events
            if case == "ready":
                assert not sleeps and now == 0, "a ready scope paid a fixed delay"
            else:
                assert 0.25 <= now < 0.26, "did not stop at the first proof: %s" % sleeps
    finally:
        for gate in gates:
            gate.close()


def main():
    global SOURCE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--case", choices=("failed", "delayed", "timeout", "ready", "exec-error"))
    args = parser.parse_args()
    SOURCE = args.source
    with tempfile.TemporaryDirectory(prefix="hookshot-play-scope-") as directory:
        if args.case:
            check_handoff(directory, args.case)
        else:
            check_own_scope(directory)
            check_play(directory)
            for case in ("failed", "delayed", "timeout", "ready", "exec-error"):
                check_handoff(directory, case)
    print("PASS: scope choice, acknowledged Play hand-off, delayed/failing runner, bounded single fallback and first-proof return"
          + (" (" + args.case + ")" if args.case else ""))


if __name__ == "__main__":
    main()
