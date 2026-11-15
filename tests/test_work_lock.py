"""One build at a time in a work folder (make_game.py lock_work): a second build into a work folder another build is using stops at
once, saying so, instead of compiling again what the first had not cached yet; the lock goes with the process that held it, so a
build that ended (or was killed) keeps no one out; and main takes it before it reads or clears anything in the work folder.

    python -I -B tests/test_work_lock.py
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import make_game  # noqa: E402

HOLD = ("import sys; sys.path.insert(0, %r); import make_game; make_game.lock_work(sys.argv[1]); print('held', flush=True); "
        "sys.stdin.read()" % str(ROOT / "nx2"))

with tempfile.TemporaryDirectory() as top:
    work = os.path.join(top, "work")
    os.makedirs(work)
    holder = subprocess.Popen([sys.executable, "-I", "-c", HOLD, work], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert holder.stdout.readline().strip() == "held"
        try:
            make_game.lock_work(work)
        except SystemExit as e:
            assert e.code == 1, e.code
        else:
            raise AssertionError("a second build took the work folder another build holds")
        assert not make_game._WORK_LOCK
    finally:
        holder.stdin.close()
        holder.wait(timeout=30)
    killed = subprocess.Popen([sys.executable, "-I", "-c", HOLD, work], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert killed.stdout.readline().strip() == "held"
    killed.kill()
    killed.wait(timeout=30)
    make_game.lock_work(work)   # (the build before ended, or was killed: its lock went with it)
    held = make_game._WORK_LOCK[1]
    make_game.lock_work(work)   # (a second main() in the same process: the lock it holds, kept)
    assert make_game._WORK_LOCK[1] is held and not held.closed
    other = os.path.join(top, "other")
    os.makedirs(other)
    make_game.lock_work(other)   # (another work folder: the lock on the first one let go)
    assert held.closed and make_game._WORK_LOCK[0] == os.path.join(other, make_game.WORK_LOCK)
    holder = subprocess.Popen([sys.executable, "-I", "-c", HOLD, work], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert holder.stdout.readline().strip() == "held"   # (another build takes the first folder while this one holds "other")
        make_game._WORK_LOCK[1].close()
    finally:
        holder.stdin.close()
        holder.wait(timeout=30)

src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
body = src[src.index("\ndef main(a=None):"):]
lock = body.index("lock_work(work)")
for later in ("check_windows_sdk(work)", "stop_stray_compilers(work", "drop_stale_objects(work)", "translate()"):
    assert lock < body.index(later), later
assert lock > body.index("return check(a, a.work)")   # (make_game --check reads the game into work/probe beside a build: no lock)
print("work lock: ok")
