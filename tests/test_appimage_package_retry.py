"""Execute the real AppImage package bootstrap with failing apt/PPA commands.

No network or package installation. The command stand-ins record ordering and
inject transient or persistent failures at each bootstrap stage under sh -e.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

# (Linux only)
RUN_ALL_SKIP = {"windows": "the AppImage package bootstrap is POSIX shell; exercised on Linux"}

ROOT = Path(__file__).resolve().parents[1]
BASE = ("base_update", "prerequisites")
PPA = ("ppa", "ppa_update", "toolchain", "dependencies")
STAGES = BASE + PPA
STUB = r'''#!/usr/bin/env python3
import json,os,signal,subprocess,sys,time
from pathlib import Path
name=Path(sys.argv[0]).name
state=Path(os.environ['BOOTSTRAP_STATE'])
data=json.loads(state.read_text())
args=sys.argv[1:]
if any(word in ' '.join(args).lower() for word in ('allow-unauthenticated','allow-insecure','trusted=yes')):
    raise SystemExit('authentication must not be bypassed')
def step(stage):
    data['calls'].append({'stage':stage,'argv':args})
    fail=stage==os.environ.get('BOOTSTRAP_FAIL_STAGE') and data['failures']<int(os.environ.get('BOOTSTRAP_FAIL_COUNT','0'))
    if fail:data['failures']+=1
    if stage=='ppa':
        data['configured']=True  # add-apt-repository writes the source before its key request can fail.
        data['key_valid']=not fail
        data['signed_index']=False
    if stage=='ppa_update':
        if not data['key_valid']:fail=True  # apt refuses the incomplete source, even after Launchpad recovers.
        data['signed_index']=not fail
    if stage in ('toolchain','dependencies') and not data['signed_index']:
        raise SystemExit('installation requires valid signed PPA indexes')
    state.write_text(json.dumps(data))
    if fail and stage=='ppa' and os.environ.get('BOOTSTRAP_STALL')!='0':
        if os.environ.get('BOOTSTRAP_STALL')=='2':signal.signal(signal.SIGTERM,signal.SIG_IGN)
        time.sleep(3)
    if fail:raise SystemExit(int(os.environ.get('BOOTSTRAP_FAIL_CODE','100')))
if name=='timeout':
    data['timeouts'].append(args[:2])
    state.write_text(json.dumps(data))
    if args[:2]!=['--kill-after=5s','180s']:raise SystemExit('enrollment must have a bounded termination/kill deadline')
    # The real GNU timeout supervises the authored child. Shorten only this test's clock, not the workflow budget.
    command=([os.environ['BOOTSTRAP_REAL_TIMEOUT'],'--kill-after=1s','1s'] if os.environ.get('BOOTSTRAP_STALL')!='0' else [])+args[2:]
    status=subprocess.call(command)
    raise SystemExit(status if status>=0 else 128-status)  # the shell's convention for a signaled timeout process
elif name=='sleep':step('sleep')
elif name=='add-apt-repository':
    step('ppa')
    if '--no-update' not in args:step('ppa_update')  # the old command refreshed implicitly.
elif 'update' in args:step('ppa_update' if data['configured'] else 'base_update')
elif 'software-properties-common' in args:step('prerequisites')
elif 'g++-13' in args:step('toolchain')
elif 'libx11-dev' in args:step('dependencies')
else:raise SystemExit('unexpected package command')
'''


def bootstrap(root):
    source = (root / ".github/workflows/linux-appimage.yml").read_text(encoding="utf-8")
    start = source.index("      - name: System packages")
    start = source.index("        run: |\n", start) + len("        run: |\n")
    lines = []
    for line in source[start:].splitlines():
        if line and not line.startswith("          "):
            break
        lines.append(line[10:] if line else "")
    return "\n".join(lines) + "\n"


@unittest.skipIf(os.name == "nt", "POSIX AppImage package bootstrap")
class PackageRetry(unittest.TestCase):
    def run_bootstrap(self, stage="", failures=0, failure_code=100, stall=False):
        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        with tempfile.TemporaryDirectory(prefix="appimage-packages-") as temp:
            folder = Path(temp)
            tools = folder / "bin"
            tools.mkdir()
            for command in ("apt-get", "add-apt-repository", "sleep", "timeout"):
                path = tools / command
                path.write_text(STUB, encoding="utf-8")
                path.chmod(0o755)
            state = folder / "state.json"
            state.write_text(json.dumps({"calls": [], "failures": 0, "configured": False,
                                         "key_valid": False, "signed_index": False, "timeouts": []}), encoding="utf-8")
            script = folder / "bootstrap.sh"
            script.write_text(bootstrap(ROOT), encoding="utf-8")
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       BOOTSTRAP_STATE=str(state), BOOTSTRAP_FAIL_STAGE=stage,
                       BOOTSTRAP_FAIL_COUNT=str(failures), BOOTSTRAP_FAIL_CODE=str(failure_code),
                       BOOTSTRAP_STALL="2" if stall == "ignore-term" else "1" if stall else "0",
                       BOOTSTRAP_REAL_TIMEOUT=shutil.which("timeout") or "",
                       DEBIAN_FRONTEND="noninteractive")
            if stall:self.assertTrue(env["BOOTSTRAP_REAL_TIMEOUT"], "GNU timeout is needed for the enrolled-child timeout cases")
            result = subprocess.run([shell, "-e", str(script)], capture_output=True, text=True, env=env, timeout=30)
            self.assertFalse(result.stderr, result.stderr)
            self.last_state = json.loads(state.read_text(encoding="utf-8"))
            return result.returncode, self.last_state["calls"]

    def test_success_has_signed_refresh_before_each_install_group(self):
        code, calls = self.run_bootstrap()
        self.assertEqual(code, 0)
        self.assertEqual([c["stage"] for c in calls], list(STAGES))

    def test_each_transient_failure_retries_its_own_recovery_boundary(self):
        for stage in STAGES:
            with self.subTest(stage=stage):
                code, calls = self.run_bootstrap(stage, 1)
                self.assertEqual(code, 0)
                phase = BASE if stage in BASE else PPA
                before = [] if stage in BASE else list(BASE)
                after = list(PPA) if stage in BASE else []
                prefix = list(phase[:phase.index(stage) + 1])
                self.assertEqual([c["stage"] for c in calls], before + prefix + ["sleep"] + list(phase) + after)
                self.assertEqual([c["argv"] for c in calls if c["stage"] == "sleep"], [["20"]])

    def test_two_index_failures_can_recover_on_the_last_attempt(self):
        code, calls = self.run_bootstrap("base_update", 2)
        self.assertEqual(code, 0)
        self.assertEqual([c["stage"] for c in calls], ["base_update", "sleep", "base_update", "sleep"] + list(STAGES))

    def test_persistent_failures_stop_after_three_attempts_with_the_original_status(self):
        for stage in STAGES:
            with self.subTest(stage=stage):
                code, calls = self.run_bootstrap(stage, 10)
                self.assertEqual(code, 100)
                phase = BASE if stage in BASE else PPA
                before = [] if stage in BASE else list(BASE)
                prefix = list(phase[:phase.index(stage) + 1])
                self.assertEqual([c["stage"] for c in calls], before + prefix + ["sleep"] + prefix + ["sleep"] + prefix)

    def test_partial_source_key_failure_recovers_on_last_enrollment_attempt(self):
        code, calls = self.run_bootstrap("ppa", 2)
        self.assertEqual(code, 0)
        self.assertEqual([c["stage"] for c in calls], list(BASE) + ["ppa", "sleep", "ppa", "sleep"] + list(PPA))
        self.assertEqual(sum(c["stage"] == "ppa" for c in calls), 3)

    def test_nonstandard_failure_status_is_preserved(self):
        code, calls = self.run_bootstrap("ppa", 10, failure_code=73)
        self.assertEqual(code, 73)
        self.assertEqual(sum(c["stage"] == "ppa" for c in calls), 3)
        self.assertFalse(any(c["stage"] in ("ppa_update", "toolchain", "dependencies") for c in calls))

    def test_timed_out_enrollment_is_repaired_on_the_next_attempt(self):
        code, calls = self.run_bootstrap("ppa", 1, stall=True)
        self.assertEqual(code, 0)
        self.assertEqual([c["stage"] for c in calls], list(BASE) + ["ppa", "sleep"] + list(PPA))
        self.assertEqual(self.last_state["timeouts"], [["--kill-after=5s", "180s"]] * 2)

    def test_persistent_enrollment_stall_stops_with_timeout_status(self):
        code, calls = self.run_bootstrap("ppa", 10, stall=True)
        self.assertEqual(code, 124)
        self.assertEqual([c["stage"] for c in calls], list(BASE) + ["ppa", "sleep", "ppa", "sleep", "ppa"])
        self.assertEqual(self.last_state["timeouts"], [["--kill-after=5s", "180s"]] * 3)

    def test_enrollment_ignoring_termination_is_killed_with_its_status_preserved(self):
        code, calls = self.run_bootstrap("ppa", 10, stall="ignore-term")
        self.assertEqual(code, 137)
        self.assertEqual([c["stage"] for c in calls], list(BASE) + ["ppa", "sleep", "ppa", "sleep", "ppa"])
        self.assertEqual(self.last_state["timeouts"], [["--kill-after=5s", "180s"]] * 3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    args, remaining = parser.parse_known_args()
    ROOT = args.source
    unittest.main(argv=[sys.argv[0]] + remaining)
